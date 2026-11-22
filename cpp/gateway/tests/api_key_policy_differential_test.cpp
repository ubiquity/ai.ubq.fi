// Differential scenario for the src/api-key-policy.ts paths its ported tests do not reach (the authentication
// cache and its invalidation, deferred admission and its 429 dispatch error, usage reads, live-reservation and
// reclaim helpers, key builders, used percent): one call sequence ran under Bun 1.4.0 over the TS CountingKv
// with Date.now fixed (tests/api_key_policy_differential_vectors.inc), replayed here over the C++ CountingKv. Each
// step compares the call's result (responses as status, header entries and JSON body) and every stored row.
// Links: tests/api_key_policy_differential_test.cpp tests/helpers/counting_kv.cpp src/api_key_policy.cpp src/api_keys.cpp
//        src/authority/native_api_key_policy.cpp src/authority/native_api_key.cpp src/authority/native_contracts.cpp
//        src/authority/native_paid_policy.cpp src/http.cpp src/kv.cpp src/types.cpp src/utils.cpp src/config.cpp src/release.cpp
//        src/auth/relay.cpp src/provider/cerebras_rate_limits.cpp src/storage/kv.cpp src/storage/key_compatibility.cpp
// src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp
#include "uos/api_key_policy.hpp"

#include "tests/helpers/counting_kv.hpp"
#include "uos/api_keys.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/memory.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::api_key_policy;
using uos::tests::helpers::counting_kv::CountingKv;

namespace {
  constexpr std::string_view VECTORS =
#include "tests/api_key_policy_differential_vectors.inc"
    ;
  constexpr double NOW = 1'800'000'000'000;

  json::value undefinedMarker() { return rt::jsonParse(R"({"$undefined":true})"); }

  bool isUndefinedMarker(const json::value& value) {
    const json::object* object = value.if_object();
    return object != nullptr && object->size() == 1 && rt::get(*object, "$undefined") != nullptr;
  }

  json::value stripUndefined(const json::value& value) {
    if (const json::object* object = value.if_object()) {
      json::object out;
      for (const auto& member : *object) {
        if (!isUndefinedMarker(member.value())) rt::objectDefine(out, member.key(), stripUndefined(member.value()));
      }
      return out;
    }
    if (const json::array* array = value.if_array()) {
      json::array out;
      for (const json::value& item : *array) out.push_back(stripUndefined(item));
      return out;
    }
    return value;
  }

  json::value rowsOf(const CountingKv& kv) {
    std::vector<std::pair<std::string, json::value>> rows;
    for (const auto& [encoded, entry] : kv.entries) rows.emplace_back(encoded, entry.value.toJson());
    std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
    json::array out;
    for (auto& [encoded, value] : rows) {
      json::array row;
      row.push_back(json::string(encoded));
      row.push_back(std::move(value));
      out.push_back(std::move(row));
    }
    return out;
  }

  json::value keyJson(const rt::KvKey& key) {
    json::array out;
    for (const rt::KvKeyPart& part : key) out.push_back(part.isString() ? json::value(json::string(part.string())) : rt::makeNumber(part.number()));
    return out;
  }

  json::value headerMap(const rt::OrderedMap<std::string, std::string>& headers) {
    json::object out;
    for (const auto& [name, value] : headers) out[name] = json::string(value);
    return out;
  }

  // `{ status, headers: [...response.headers], body: await response.json() }`
  rt::Task<json::value> responseOf(rt::Response response) {
    json::array headers;
    for (const auto& [name, value] : response.headers().entries()) {
      json::array entry;
      entry.push_back(json::string(name));
      entry.push_back(json::string(value));
      headers.push_back(std::move(entry));
    }
    rt::Task<json::value> reading = response.json();
    json::value body = co_await std::move(reading);
    json::object out;
    out["status"] = response.status();
    out["headers"] = std::move(headers);
    out["body"] = std::move(body);
    co_return out;
  }

  rt::Task<json::value> decisionOf(ApiKeyPolicyDecision decision) {
    if (const auto* refused = std::get_if<ApiKeyPolicyDecisionNotOk>(&decision)) {
      rt::Task<json::value> reading = responseOf(refused->response);
      json::value response = co_await std::move(reading);
      json::object out;
      out["ok"] = false;
      out["response"] = std::move(response);
      co_return out;
    }
    json::object out;
    out["ok"] = true;
    out["policy"] = json::value_from(std::get<ApiKeyPolicyDecisionOk>(decision).policy);
    co_return out;
  }

  rt::Task<json::value> reservationDecisionOf(ApiKeyUsageReservationDecision decision) {
    if (const auto* refused = std::get_if<ApiKeyUsageReservationDecisionNotOk>(&decision)) {
      rt::Task<json::value> reading = responseOf(refused->response);
      json::value response = co_await std::move(reading);
      json::object out;
      out["ok"] = false;
      out["response"] = std::move(response);
      co_return out;
    }
    const ApiKeyUsageReservation& reservation = std::get<ApiKeyUsageReservationDecisionOk>(decision).reservation;
    json::object summary;
    summary["policy"] = json::value_from(reservation.policy);
    summary["request_id"] = json::string(reservation.request_id);
    summary["route"] = json::string(reservation.route);
    json::object out;
    out["ok"] = true;
    out["reservation"] = std::move(summary);
    co_return out;
  }

  json::value errorOf(const rt::Reason& reason) {
    json::object error;
    try {
      std::rethrow_exception(reason);
    } catch (const ApiKeyQuotaDispatchError& dispatch) {
      error["name"] = json::string(dispatch.name);
      error["message"] = json::string(dispatch.message);
      error["status"] = rt::makeNumber(dispatch.status);
      error["code"] = json::string(dispatch.code);
      error["errorType"] = json::string(dispatch.errorType);
      error["retryAfter"] = dispatch.retryAfter ? json::value(json::string(*dispatch.retryAfter)) : json::value(nullptr);
      json::array headers;
      for (const auto& [name, value] : dispatch.headers) {
        json::array entry;
        entry.push_back(json::string(name));
        entry.push_back(json::string(value));
        headers.push_back(std::move(entry));
      }
      error["headers"] = std::move(headers);
    } catch (const rt::JsError& failure) {
      error["name"] = json::string(failure.name);
      error["message"] = json::string(failure.message);
      error["status"] = nullptr;
      error["code"] = failure.code ? json::value(json::string(*failure.code)) : json::value(nullptr);
      error["errorType"] = nullptr;
      error["retryAfter"] = nullptr;
      error["headers"] = nullptr;
    }
    json::object out;
    out["$error"] = std::move(error);
    return out;
  }

  rt::Task<json::value> settle(std::function<rt::Task<json::value>()> run) {
    rt::Reason failure;
    json::value out;
    try {
      rt::Task<json::value> running = run();
      out = co_await std::move(running);
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) co_return errorOf(failure);
    co_return out;
  }

  rt::KvValue hashRecord(std::string_view overrides) {
    json::value record = rt::jsonParse(R"({"id":"key-p","expires_at_ms":-1,"revoked_at_ms":null,"usage_limit_requests":2,"usage_requests":0,
      "usage_reset_at_ms":1800003600000,"window_ms":3600000,"usage_quota_version":3,"paid_fallback_enabled":true,"paid_fallback_limit_microcredits":1000,
      "paid_fallback_spent_microcredits":250,"paid_fallback_reserved_microcredits":125,"paid_fallback_reservation_request_id":null})");
    const json::value patch = rt::jsonParse(overrides);
    for (const auto& member : patch.as_object()) rt::objectSet(record.as_object(), member.key(), member.value());
    return rt::KvValue(record);
  }

  json::value numberOrNull(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }
}

ASYNC_TEST("api-key-policy authentication cache, deferred admission and usage helpers match the TS step by step") {
  rt::globalDateNow() = [] { return NOW; };
  const json::value vectors = rt::jsonParse(VECTORS);
  const auto kv = std::make_shared<CountingKv>();
  struct Step {
    std::string name;
    json::value out;
    json::value rows;
  };
  std::vector<Step> steps;
  const auto record = [&steps, &kv](std::string name, json::value out) { steps.push_back(Step{.name = std::move(name), .out = std::move(out), .rows = rowsOf(*kv)}); };

  const std::string token = "u_" + rt::repeat("ab", 32);
  rt::Task<std::string> hashing = uos::utils::sha256Base64Url(token);
  const std::string hash = co_await std::move(hashing);
  EXPECT_EQ(hash, std::string(*rt::getString(vectors, "hash")));
  kv->seed(uos::api_keys::apiKeyHashKey(hash), hashRecord("{}"));
  resetApiKeyPolicyCacheForTest();

  const auto authenticate = [](std::optional<std::shared_ptr<rt::Kv>> store, std::optional<double> nowMs, std::string candidate) {
    return rt::async([store, nowMs, candidate]() -> rt::Task<json::value> {
      AuthenticateApiKeyTokenOptions options;
      options.kv = store;
      options.nowMs = nowMs;
      rt::Task<ApiKeyPolicyDecision> authenticating = authenticateApiKeyToken(candidate, std::move(options));
      ApiKeyPolicyDecision decision = co_await std::move(authenticating);
      rt::Task<json::value> converting = decisionOf(std::move(decision));
      json::value out = co_await std::move(converting);
      co_return out;
    });
  };
  ApiKeyPolicy policy{};
  {
    rt::Task<json::value> step = settle([&] { return authenticate(kv, NOW, token); });
    json::value out = co_await std::move(step);
    if (const json::value* found = rt::get(out, "policy")) policy = json::value_to<ApiKeyPolicy>(*found);
    record("authenticate", std::move(out));
  }
  {
    rt::Task<json::value> step = settle([&] { return authenticate(kv, std::nullopt, "u_123"); });
    json::value out = co_await std::move(step);
    record("authenticate malformed token", std::move(out));
  }
  {
    rt::Task<json::value> step = settle([&] { return authenticate(std::shared_ptr<rt::Kv>(), std::nullopt, token); });
    json::value out = co_await std::move(step);
    record("authenticate without kv", std::move(out));
  }
  {
    ApiKeyPolicy disabled = policy;
    disabled.paid_fallback_enabled = false;
    ApiKeyPolicy unlimited = policy;
    unlimited.paid_fallback_limit_microcredits = -1;
    ApiKeyPolicy overspent = policy;
    overspent.paid_fallback_spent_microcredits = 5'000;
    json::array out;
    out.push_back(numberOrNull(apiKeyQuotaUsedPercent(&policy)));
    out.push_back(numberOrNull(apiKeyQuotaUsedPercent(nullptr)));
    out.push_back(numberOrNull(apiKeyQuotaUsedPercent(&disabled)));
    out.push_back(numberOrNull(apiKeyQuotaUsedPercent(&unlimited)));
    out.push_back(numberOrNull(apiKeyQuotaUsedPercent(&overspent)));
    record("used percent", std::move(out));
  }
  {
    ApiKeyPolicy legacy = policy;
    legacy.policy_version = "legacy";
    ApiKeyPolicy unlimited = policy;
    unlimited.usage_limit_requests = -1;
    ApiKeyPolicy fractional = policy;
    fractional.window_ms = 1'500;
    json::object out;
    out["v2"] = keyJson(apiKeyUsageV2Key(policy));
    out["v2Plain"] = keyJson(apiKeyUsageV2Key(legacy));
    out["window"] = keyJson(apiKeyUsageV3WindowKey(policy));
    out["request"] = keyJson(apiKeyUsageV3RequestKey(policy, "rq"));
    out["headers"] = headerMap(apiKeyRateLimitPolicyHeaders(&policy));
    out["unlimitedHeaders"] = headerMap(apiKeyRateLimitPolicyHeaders(&unlimited));
    out["nullHeaders"] = headerMap(apiKeyRateLimitPolicyHeaders(nullptr));
    out["fractionalWindow"] = headerMap(apiKeyRateLimitPolicyHeaders(&fractional));
    out["retention"] = rt::makeNumber(apiKeyUsageV3RetentionMs(NOW + 10));
    out["window3"] = json::value_from(makeApiKeyUsageWindowV3(policy));
    record("keys and headers", std::move(out));
  }

  std::map<std::string, ApiKeyUsageReservation> reservations;
  const auto reserve = [&reservations, &kv, &policy](std::string requestId, std::optional<double> nowMs, std::optional<bool> deferWhenFull) {
    return rt::async([&reservations, store = kv, current = policy, requestId, nowMs, deferWhenFull]() -> rt::Task<json::value> {
      ReserveApiKeyUsageV3Options options;
      options.kv = store;
      options.nowMs = nowMs;
      options.deferWhenFull = deferWhenFull;
      rt::Task<ApiKeyUsageReservationDecision> reserving = reserveApiKeyUsageV3(current, requestId, std::string("responses"), std::move(options));
      ApiKeyUsageReservationDecision decision = co_await std::move(reserving);
      if (const auto* ok = std::get_if<ApiKeyUsageReservationDecisionOk>(&decision)) reservations.insert_or_assign(requestId, ok->reservation);
      rt::Task<json::value> converting = reservationDecisionOf(std::move(decision));
      json::value out = co_await std::move(converting);
      co_return out;
    });
  };
  const auto replay = [&](std::string name, std::function<rt::Task<json::value>()> work) -> rt::Task<void> {
    return rt::async([&record, name, work]() -> rt::Task<void> {
      rt::Task<json::value> step = settle(work);
      json::value out = co_await std::move(step);
      record(name, std::move(out));
    });
  };

  {
    rt::Task<void> step = replay("reserve r1", [&] { return reserve("r1", std::nullopt, std::nullopt); });
    co_await std::move(step);
  }
  {
    rt::Task<void> step = replay("reserve r2", [&] { return reserve("r2", NOW + 1, std::nullopt); });
    co_await std::move(step);
  }
  {
    rt::Task<void> step = replay("reserve r3 deferred", [&] { return reserve("r3", std::nullopt, true); });
    co_await std::move(step);
  }
  {
    const ApiKeyUsageReservation deferred = reservations.at("r3");
    rt::Task<void> step = replay("deferred dispatch is refused", [deferred]() -> rt::Task<json::value> {
      return rt::async([deferred]() -> rt::Task<json::value> {
        rt::Task<std::optional<ApiKeyProviderDispatch>> dispatching = deferred.beforeProviderDispatch("cerebras");
        const std::optional<ApiKeyProviderDispatch> dispatch = co_await std::move(dispatching);
        co_return dispatch ? json::value(json::object()) : undefinedMarker();
      });
    });
    co_await std::move(step);
  }
  {
    const ApiKeyUsageReservation deferred = reservations.at("r3");
    rt::Task<void> step = replay("deferred release is a no-op", [deferred]() -> rt::Task<json::value> {
      return rt::async([deferred]() -> rt::Task<json::value> {
        rt::Task<void> releasing = deferred.release(std::string("done"));
        co_await std::move(releasing);
        co_return undefinedMarker();
      });
    });
    co_await std::move(step);
  }
  {
    rt::Task<void> step = replay("reserve r4 full", [&] { return reserve("r4", std::nullopt, std::nullopt); });
    co_await std::move(step);
  }
  {
    rt::Task<void> step = replay("reserve without request id", [&] { return reserve("", std::nullopt, std::nullopt); });
    co_await std::move(step);
  }
  const auto usage = [&policy](std::optional<std::shared_ptr<rt::Kv>> store) {
    return rt::async([current = policy, store]() -> rt::Task<json::value> {
      rt::Task<double> reading = getApiKeyUsageV3(current, store);
      const double value = co_await std::move(reading);
      co_return rt::makeNumber(value);
    });
  };
  {
    rt::Task<void> step = replay("usage before dispatch", [&] { return usage(kv); });
    co_await std::move(step);
  }
  {
    const ApiKeyUsageReservation r1 = reservations.at("r1");
    rt::Task<void> step = replay("dispatch r1", [r1]() -> rt::Task<json::value> {
      return rt::async([r1]() -> rt::Task<json::value> {
        rt::Task<std::optional<ApiKeyProviderDispatch>> dispatching = r1.beforeProviderDispatch("deepseek");
        const std::optional<ApiKeyProviderDispatch> dispatch = co_await std::move(dispatching);
        if (dispatch) dispatch->markTransportStarted();
        co_return json::value(dispatch.has_value());
      });
    });
    co_await std::move(step);
  }
  {
    rt::Task<void> step = replay("usage after dispatch", [&] { return usage(kv); });
    co_await std::move(step);
  }
  {
    rt::Task<void> step = replay("usage without kv", [&] { return usage(std::shared_ptr<rt::Kv>()); });
    co_await std::move(step);
  }
  const auto live = [&kv](double nowMs) {
    return rt::async([store = kv, nowMs]() -> rt::Task<json::value> {
      rt::Task<bool> checking = hasLiveApiKeyUsageReservationsV3(store, std::string("key-p"), nowMs);
      const bool found = co_await std::move(checking);
      co_return json::value(found);
    });
  };
  {
    rt::Task<void> step = replay("live reservations now", [&] { return live(NOW); });
    co_await std::move(step);
  }
  {
    rt::Task<void> step = replay("live reservations later", [&] { return live(NOW + 600'000); });
    co_await std::move(step);
  }
  {
    rt::Task<void> step = replay("reclaim for key", [&]() -> rt::Task<json::value> {
      return rt::async([store = kv]() -> rt::Task<json::value> {
        rt::Task<void> reclaiming = reclaimApiKeyUsageReservationsForKeyV3(store, std::string("key-p"), NOW + 600'000);
        co_await std::move(reclaiming);
        co_return undefinedMarker();
      });
    });
    co_await std::move(step);
  }
  {
    rt::Task<void> step = replay("live reservations after reclaim", [&] { return live(NOW); });
    co_await std::move(step);
  }
  {
    rt::Task<void> step = replay("reclaim window", [&]() -> rt::Task<json::value> {
      return rt::async([store = kv, current = policy]() -> rt::Task<json::value> {
        rt::Task<void> reclaiming = reclaimExpiredApiKeyUsageReservationsV3(store, current, NOW + 600'000);
        co_await std::move(reclaiming);
        co_return undefinedMarker();
      });
    });
    co_await std::move(step);
  }
  kv->seed(uos::api_keys::apiKeyHashKey(hash), hashRecord(R"({"revoked_at_ms":1799999999999})"));
  {
    rt::Task<void> step = replay("cached authentication survives revocation", [&] { return authenticate(kv, NOW + 1, token); });
    co_await std::move(step);
  }
  {
    rt::Task<void> step = replay("reserve against a revoked row", [&] { return reserve("r5", std::nullopt, std::nullopt); });
    co_await std::move(step);
  }
  invalidateApiKeyPolicy("other-key");
  {
    rt::Task<void> step = replay("other key invalidation keeps the cache", [&] { return authenticate(kv, NOW + 2, token); });
    co_await std::move(step);
  }
  invalidateApiKeyPolicy("key-p");
  {
    rt::Task<void> step = replay("invalidated authentication rereads", [&] { return authenticate(kv, NOW + 3, token); });
    co_await std::move(step);
  }
  kv->seed(uos::api_keys::apiKeyHashKey(hash), hashRecord(R"({"usage_quota_version":2})"));
  {
    rt::Task<void> step = replay("unmigrated row", [&] { return authenticate(kv, NOW + 4, token); });
    co_await std::move(step);
  }
  {
    rt::Task<void> step = replay("reserve against an unmigrated row", [&] { return reserve("r6", std::nullopt, std::nullopt); });
    co_await std::move(step);
  }
  kv->seed(uos::api_keys::apiKeyHashKey(hash), hashRecord(R"({"expires_at_ms":1800000000010})"));
  resetApiKeyPolicyCacheForTest();
  {
    rt::Task<void> step = replay("authenticate before expiry", [&] { return authenticate(kv, NOW + 5, token); });
    co_await std::move(step);
  }
  {
    rt::Task<void> step = replay("cached policy expires", [&] { return authenticate(kv, NOW + 11, token); });
    co_await std::move(step);
  }
  kv->seed(uos::api_keys::apiKeyHashKey(hash), hashRecord("{}"));
  invalidateApiKeyPolicy();
  {
    rt::Task<void> step = replay("cached window rolls forward", [&]() -> rt::Task<json::value> {
      return rt::async([&authenticate, &kv, token]() -> rt::Task<json::value> {
        rt::Task<json::value> warming = authenticate(kv, NOW, token);
        co_await std::move(warming);
        rt::Task<json::value> rolling = authenticate(kv, NOW + 3'600'000 + 5, token);
        json::value out = co_await std::move(rolling);
        co_return out;
      });
    });
    co_await std::move(step);
  }
  {
    rt::Task<void> step = replay("delete usage", [&]() -> rt::Task<json::value> {
      return rt::async([store = kv]() -> rt::Task<json::value> {
        rt::Task<void> deleting = deleteApiKeyUsageV3(store, std::string("key-p"));
        co_await std::move(deleting);
        co_return undefinedMarker();
      });
    });
    co_await std::move(step);
  }
  resetApiKeyPolicyCacheForTest();

  const json::array& expected = rt::get(vectors, "steps")->as_array();
  EXPECT_EQ(steps.size(), expected.size());
  for (std::size_t index = 0; index < steps.size() && index < expected.size(); ++index) {
    const json::value& item = expected[index];
    EXPECT_EQ(steps[index].name, std::string(*rt::getString(item, "name")));
    const json::value& expectedOut = *rt::get(item, "out");
    const json::value wantOut = isUndefinedMarker(expectedOut) ? expectedOut : stripUndefined(expectedOut);
    const json::value gotOut = rt::canonicalNumbers(steps[index].out);
    if (!rt::deepEqual(gotOut, wantOut)) EXPECT_EQ(rt::jsonStringify(gotOut), rt::jsonStringify(wantOut) + "  <- out of " + steps[index].name);
    const json::value wantRows = stripUndefined(*rt::get(item, "rows"));
    const json::value gotRows = rt::canonicalNumbers(steps[index].rows);
    if (!rt::deepEqual(gotRows, wantRows)) EXPECT_EQ(rt::jsonStringify(gotRows), rt::jsonStringify(wantRows) + "  <- rows of " + steps[index].name);
  }
}

ASYNC_TEST("API key policy memo evicts old entries and reauthenticates them from KV") {
  resetApiKeyPolicyCacheForTest();
  const auto cleanup = rt::scopeExit([] { resetApiKeyPolicyCacheForTest(); });
  const auto kv = std::make_shared<CountingKv>();
  const AuthenticateApiKeyTokenOptions options{.kv = kv, .nowMs = NOW};
  const auto tokenFor = [](std::size_t index) {
    const std::string suffix = std::to_string(index);
    return "u_" + std::string(64 - suffix.size(), '0') + suffix;
  };
  const auto limit = rt::limits::kApiKeyPolicyCacheEntries;
  for (std::size_t index = 0; index <= limit; ++index) {
    const auto token = tokenFor(index);
    auto hashing = uos::utils::sha256Base64Url(token);
    const auto hash = co_await std::move(hashing);
    const uos::types::ApiKeyHashRecord record{
      .id = "cache-fixture-" + std::to_string(index), .expires_at_ms = -1, .revoked_at_ms = {},
      .usage_limit_requests = 100, .usage_requests = 0, .usage_reset_at_ms = NOW + 60'000,
      .window_ms = 60'000, .usage_quota_version = 3, .paid_fallback_enabled = false,
      .paid_fallback_limit_microcredits = 0, .paid_fallback_spent_microcredits = 0,
      .paid_fallback_reserved_microcredits = 0, .paid_fallback_reservation_request_id = {},
    };
    kv->seed(uos::api_keys::apiKeyHashKey(hash), rt::KvValue(json::value_from(record)));
    auto authenticating = authenticateApiKeyToken(token, options);
    const auto decision = co_await std::move(authenticating);
    ASSERT(std::holds_alternative<ApiKeyPolicyDecisionOk>(decision));
  }
  const auto memory = rt::memory::snapshot(false);
  const auto& cache = memory.at("caches").as_object().at("api_key_policy").as_object();
  EXPECT_EQ(cache.at("entries").as_uint64(), limit);
  EXPECT(cache.at("bytes").as_uint64() <= rt::limits::kApiKeyPolicyCacheBytes);

  const auto before = kv->commands.size();
  auto authenticating = authenticateApiKeyToken(tokenFor(0), options);
  const auto decision = co_await std::move(authenticating);
  ASSERT(std::holds_alternative<ApiKeyPolicyDecisionOk>(decision));
  EXPECT_EQ(std::get<ApiKeyPolicyDecisionOk>(decision).policy.key_id, std::string("cache-fixture-0"));
  EXPECT_EQ(kv->commands.size(), before + 1);
}
