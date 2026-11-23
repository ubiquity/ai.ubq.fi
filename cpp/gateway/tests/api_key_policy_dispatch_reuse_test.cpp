// Port of tests/api-key-policy-dispatch-reuse.test.ts (all seven cases).
// Links: tests/api_key_policy_dispatch_reuse_test.cpp tests/helpers/counting_kv.cpp src/api_key_policy.cpp src/api_keys.cpp
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
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/types.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::api_key_policy;
using uos::tests::helpers::counting_kv::CountingKv;
using uos::tests::helpers::counting_kv::encodeKey;
using uos::tests::helpers::counting_kv::KvMeasurementContext;
using uos::tests::helpers::counting_kv::KvOperationBudget;

namespace {
  // `kv.entries.get(JSON.stringify(key))?.value ?? null`
  json::value storedValue(const CountingKv& kv, const rt::KvKey& key) {
    const auto* entry = kv.entries.get(encodeKey(key));
    return entry ? entry->value.toJson() : json::value(nullptr);
  }

  struct Setup {
    std::shared_ptr<CountingKv> kv;
    ApiKeyPolicy policy;
  };

  Setup setupPolicy(const std::string& id, double usageLimitRequests, double nowMs) {
    const std::string tokenHash = "dispatch-reuse-" + id;
    const uos::types::ApiKeyHashRecord record{
      .id = id,
      .expires_at_ms = -1,
      .revoked_at_ms = std::nullopt,
      .usage_limit_requests = usageLimitRequests,
      .usage_requests = 0,
      .usage_reset_at_ms = nowMs + 60 * 60'000,
      .window_ms = 60 * 60'000,
      .usage_quota_version = 3,
      .paid_fallback_enabled = false,
      .paid_fallback_limit_microcredits = 0,
      .paid_fallback_spent_microcredits = 0,
      .paid_fallback_reserved_microcredits = 0,
      .paid_fallback_reservation_request_id = std::nullopt,
    };
    const std::optional<ApiKeyPolicy> policy = apiKeyPolicyFromHashRecord(tokenHash, record, nowMs);
    if (!policy) throw std::runtime_error("test API key policy must be valid");
    auto kv = std::make_shared<CountingKv>();
    kv->seed(uos::api_keys::apiKeyHashKey(tokenHash), rt::KvValue(json::value_from(record)));
    return Setup{.kv = kv, .policy = *policy};
  }
  Setup setupPolicy(const std::string& id, double usageLimitRequests) { return setupPolicy(id, usageLimitRequests, rt::dateNow()); }

  rt::Task<ApiKeyUsageReservation> reserve(std::shared_ptr<CountingKv> kv, ApiKeyPolicy policy, std::string requestId, double nowMs) {
    ReserveApiKeyUsageV3Options options;
    options.kv = kv;
    options.nowMs = nowMs;
    rt::Task<ApiKeyUsageReservationDecision> reserving = reserveApiKeyUsageV3(std::move(policy), std::move(requestId), "responses", std::move(options));
    const ApiKeyUsageReservationDecision decision = co_await std::move(reserving);
    if (const auto* refused = std::get_if<ApiKeyUsageReservationDecisionNotOk>(&decision)) {
      throw std::runtime_error("unexpected admission failure: " + std::to_string(refused->response.status()));
    }
    co_return std::get<ApiKeyUsageReservationDecisionOk>(decision).reservation;
  }
  rt::Task<ApiKeyUsageReservation> reserve(std::shared_ptr<CountingKv> kv, ApiKeyPolicy policy, std::string requestId) {
    rt::Task<ApiKeyUsageReservation> reserving = reserve(std::move(kv), std::move(policy), std::move(requestId), rt::dateNow());
    ApiKeyUsageReservation reservation = co_await std::move(reserving);
    co_return reservation;
  }

  json::value windowFor(const CountingKv& kv, const ApiKeyPolicy& policy) {
    json::value window = storedValue(kv, apiKeyUsageV3WindowKey(policy));
    if (window.is_null()) throw std::runtime_error("expected V3 aggregate window");
    return window;
  }

  json::value requestFor(const CountingKv& kv, const ApiKeyPolicy& policy, const std::string& requestId) {
    json::value request = storedValue(kv, apiKeyUsageV3RequestKey(policy, requestId));
    if (request.is_null()) throw std::runtime_error("expected V3 request row");
    return request;
  }

  // `{ name: source.member, ... }` as deepEqual sees it.
  json::object pick(const json::value& source, const std::vector<std::pair<std::string, std::string>>& fields) {
    json::object out;
    for (const auto& [name, member] : fields) {
      if (const json::value* value = rt::get(source, member)) out[name] = *value;
    }
    return out;
  }

  bool sameJson(const json::value& actual, std::string_view expected) { return rt::deepEqual(actual, rt::jsonParse(expected)); }

  rt::Task<std::optional<ApiKeyProviderDispatch>> dispatchFor(const ApiKeyUsageReservation& reservation, std::string provider) {
    rt::Task<std::optional<ApiKeyProviderDispatch>> dispatching = reservation.beforeProviderDispatch(std::move(provider));
    std::optional<ApiKeyProviderDispatch> dispatch = co_await std::move(dispatching);
    co_return dispatch;
  }

  rt::Task<void> releaseWith(const ApiKeyUsageReservation& reservation, std::optional<std::string> reason) {
    rt::Task<void> releasing = reservation.release(std::move(reason));
    co_await std::move(releasing);
  }
}

ASYNC_TEST("V3 local dispatch state skips only the redundant post-dispatch release reads") {
  const Setup setup = setupPolicy("normal-dispatch", 2);
  const auto& kv = setup.kv;
  const ApiKeyPolicy& policy = setup.policy;
  const std::function<void()> finish = kv->beginMeasurement(KvMeasurementContext{.authKind = "bounded_api_key", .outcome = "post_dispatch_release"});
  {
    auto cleanup = rt::scopeExit([&finish] { finish(); });
    rt::Task<ApiKeyUsageReservation> reserving = reserve(kv, policy, "normal-dispatch-request");
    const ApiKeyUsageReservation reservation = co_await std::move(reserving);
    rt::Task<std::optional<ApiKeyProviderDispatch>> dispatching = dispatchFor(reservation, "chatgpt_codex");
    const std::optional<ApiKeyProviderDispatch> dispatch = co_await std::move(dispatching);
    if (!dispatch) throw std::runtime_error("first dispatch must create a transport context");
    dispatch->markTransportStarted();

    const std::size_t beforeRelease = kv->commands.size();
    rt::Task<void> releasing = releaseWith(reservation, std::string("provider_http_failure"));
    co_await std::move(releasing);
    // a locally dispatched reservation must not reread for completion
    EXPECT_EQ(kv->commands.size(), beforeRelease);
  }

  const std::vector<KvOperationBudget> budgets = kv->budgets();
  EXPECT(!budgets.empty());
  const KvOperationBudget& budget = budgets.at(0);
  EXPECT_EQ(budget.commands, 7.0);
  EXPECT_EQ(budget.read_commands, 5.0);
  EXPECT_EQ(budget.write_mutations, 4.0);
  EXPECT_EQ(budget.atomic_commits, 2.0);
  EXPECT_EQ(budget.atomic_checks, 5.0);
  EXPECT_EQ(budget.atomic_mutations, 4.0);
  json::object observed;
  observed["committed"] = *rt::get(windowFor(*kv, policy), "committed_requests");
  observed["reserved"] = *rt::get(windowFor(*kv, policy), "reserved_requests");
  observed["state"] = *rt::get(requestFor(*kv, policy, "normal-dispatch-request"), "state");
  EXPECT(sameJson(observed, R"({"committed":1,"reserved":0,"state":"dispatched"})"));
}

ASYNC_TEST("V3 pre-transport cancellation still compensates before the local completion no-op") {
  const Setup setup = setupPolicy("pre-transport-cancellation", 1);
  const auto& kv = setup.kv;
  const ApiKeyPolicy& policy = setup.policy;
  rt::Task<ApiKeyUsageReservation> reserving = reserve(kv, policy, "pre-transport-cancellation-request");
  const ApiKeyUsageReservation reservation = co_await std::move(reserving);
  rt::Task<std::optional<ApiKeyProviderDispatch>> dispatching = dispatchFor(reservation, "chatgpt_codex");
  const std::optional<ApiKeyProviderDispatch> dispatch = co_await std::move(dispatching);
  if (!dispatch) throw std::runtime_error("first dispatch must create a transport context");

  const std::size_t beforeCancellation = kv->commands.size();
  rt::Task<void> cancelling = dispatch->cancelBeforeTransport();
  co_await std::move(cancelling);
  const std::size_t afterCancellation = kv->commands.size();
  // cancellation must retain two reads and its CAS compensation
  EXPECT_EQ(afterCancellation - beforeCancellation, std::size_t{3});

  rt::Task<void> releasing = releaseWith(reservation, std::nullopt);
  co_await std::move(releasing);
  // completion must not issue a second settled-state reread
  EXPECT_EQ(kv->commands.size(), afterCancellation);
  const json::value observed = pick(windowFor(*kv, policy), {{"committed", "committed_requests"}, {"reserved", "reserved_requests"}});
  json::object expected = observed.as_object();
  expected["state"] = *rt::get(requestFor(*kv, policy, "pre-transport-cancellation-request"), "state");
  EXPECT(sameJson(expected, R"({"committed":0,"reserved":0,"state":"released"})"));
}

ASYNC_TEST("V3 retry and upstream failure retain one dispatch while completion stays read-free") {
  const Setup setup = setupPolicy("retry-and-upstream-failure", 2);
  const auto& kv = setup.kv;
  const ApiKeyPolicy& policy = setup.policy;
  rt::Task<ApiKeyUsageReservation> reserving = reserve(kv, policy, "retry-and-upstream-failure-request");
  const ApiKeyUsageReservation reservation = co_await std::move(reserving);
  rt::Task<std::optional<ApiKeyProviderDispatch>> dispatching = dispatchFor(reservation, "chatgpt_codex");
  const std::optional<ApiKeyProviderDispatch> firstDispatch = co_await std::move(dispatching);
  if (!firstDispatch) throw std::runtime_error("first dispatch must create a transport context");
  firstDispatch->markTransportStarted();

  const std::size_t beforeRetry = kv->commands.size();
  rt::Task<std::optional<ApiKeyProviderDispatch>> retrying = dispatchFor(reservation, "metered");
  const std::optional<ApiKeyProviderDispatch> retryDispatch = co_await std::move(retrying);
  // a retry must not commit another dispatch
  EXPECT(!retryDispatch.has_value());
  // the existing retry ledger verification remains strongly read-backed
  EXPECT_EQ(kv->commands.size() - beforeRetry, std::size_t{2});

  const std::size_t beforeCompletion = kv->commands.size();
  rt::Task<void> releasing = releaseWith(reservation, std::string("provider_http_failure"));
  co_await std::move(releasing);
  // a dispatched upstream failure must not add completion reads
  EXPECT_EQ(kv->commands.size(), beforeCompletion);
  json::object observed = pick(windowFor(*kv, policy), {{"committed", "committed_requests"}, {"reserved", "reserved_requests"}});
  observed["provider"] = *rt::get(requestFor(*kv, policy, "retry-and-upstream-failure-request"), "provider");
  EXPECT(sameJson(observed, R"({"committed":1,"reserved":0,"provider":"chatgpt_codex"})"));
}

ASYNC_TEST("V3 non-dispatch paths retain the durable release CAS") {
  const Setup setup = setupPolicy("non-dispatch", 1);
  const auto& kv = setup.kv;
  const ApiKeyPolicy& policy = setup.policy;
  rt::Task<ApiKeyUsageReservation> reserving = reserve(kv, policy, "non-dispatch-request");
  const ApiKeyUsageReservation reservation = co_await std::move(reserving);
  const std::size_t beforeRelease = kv->commands.size();

  rt::Task<void> releasing = releaseWith(reservation, std::string("validation_failed"));
  co_await std::move(releasing);

  // an undispatched reservation must still read and CAS-release
  EXPECT_EQ(kv->commands.size() - beforeRelease, std::size_t{3});
  json::object observed = pick(windowFor(*kv, policy), {{"committed", "committed_requests"}, {"reserved", "reserved_requests"}});
  const json::value request = requestFor(*kv, policy, "non-dispatch-request");
  observed["state"] = *rt::get(request, "state");
  observed["reason"] = *rt::get(request, "release_reason");
  EXPECT(sameJson(observed, R"({"committed":0,"reserved":0,"state":"released","reason":"validation_failed"})"));
}

ASYNC_TEST("V3 duplicate admission and concurrent bounded admission preserve one reservation and dispatch") {
  const Setup setup = setupPolicy("idempotency-and-concurrency", 1);
  const auto& kv = setup.kv;
  const ApiKeyPolicy& policy = setup.policy;
  rt::Task<ApiKeyUsageReservation> reservingFirst = reserve(kv, policy, "same-request");
  const ApiKeyUsageReservation first = co_await std::move(reservingFirst);
  rt::Task<ApiKeyUsageReservation> reservingDuplicate = reserve(kv, policy, "same-request");
  const ApiKeyUsageReservation duplicate = co_await std::move(reservingDuplicate);
  // the same request id must not reserve quota twice
  EXPECT(sameJson(*rt::get(windowFor(*kv, policy), "reserved_requests"), "1"));

  rt::Task<std::optional<ApiKeyProviderDispatch>> dispatching = dispatchFor(duplicate, "cerebras");
  const std::optional<ApiKeyProviderDispatch> dispatch = co_await std::move(dispatching);
  if (!dispatch) throw std::runtime_error("duplicate admission must still dispatch the one durable row");
  dispatch->markTransportStarted();
  rt::Task<void> releasingFirst = releaseWith(first, std::nullopt);
  co_await std::move(releasingFirst);
  EXPECT(sameJson(*rt::get(windowFor(*kv, policy), "committed_requests"), "1"));

  const Setup concurrent = setupPolicy("concurrent-admission", 1);
  std::vector<rt::Promise<ApiKeyUsageReservationDecision>> pending;
  for (int index = 0; index < 8; ++index) {
    ReserveApiKeyUsageV3Options options;
    options.kv = concurrent.kv;
    pending.push_back(rt::Promise<ApiKeyUsageReservationDecision>(reserveApiKeyUsageV3(concurrent.policy, "concurrent-" + std::to_string(index), "responses", options)));
  }
  auto all = rt::all(std::move(pending));
  const std::vector<ApiKeyUsageReservationDecision> decisions = co_await all.wait();
  std::vector<ApiKeyUsageReservation> admitted;
  for (const ApiKeyUsageReservationDecision& decision : decisions) {
    if (const auto* ok = std::get_if<ApiKeyUsageReservationDecisionOk>(&decision)) admitted.push_back(ok->reservation);
  }
  // only one bounded request may reserve the last slot
  EXPECT_EQ(admitted.size(), std::size_t{1});
  if (admitted.empty()) throw std::runtime_error("expected a concurrent admission winner");
  const ApiKeyUsageReservation winner = admitted.at(0);
  rt::Task<std::optional<ApiKeyProviderDispatch>> winnerDispatching = dispatchFor(winner, "voyage");
  const std::optional<ApiKeyProviderDispatch> winnerDispatch = co_await std::move(winnerDispatching);
  if (!winnerDispatch) throw std::runtime_error("the winning reservation must dispatch");
  winnerDispatch->markTransportStarted();
  rt::Task<void> releasingWinner = releaseWith(winner, std::nullopt);
  co_await std::move(releasingWinner);
  const json::object observed = pick(windowFor(*concurrent.kv, concurrent.policy), {{"committed", "committed_requests"}, {"reserved", "reserved_requests"}});
  EXPECT(sameJson(observed, R"({"committed":1,"reserved":0})"));
}

ASYNC_TEST("V3 abandoned reservations remain reclaimable after a simulated process crash") {
  const double nowMs = rt::dateNow();
  const Setup setup = setupPolicy("crash-reclaim", 1, nowMs);
  const auto& kv = setup.kv;
  const ApiKeyPolicy& policy = setup.policy;
  rt::Task<ApiKeyUsageReservation> abandoning = reserve(kv, policy, "abandoned-request", nowMs);
  co_await std::move(abandoning);

  rt::Task<ApiKeyUsageReservation> replacing = reserve(kv, policy, "replacement-request", nowMs + API_KEY_USAGE_V3_RESERVATION_LEASE_MS + 1);
  const ApiKeyUsageReservation replacement = co_await std::move(replacing);

  const json::value abandoned = requestFor(*kv, policy, "abandoned-request");
  json::object observed;
  observed["abandonedState"] = *rt::get(abandoned, "state");
  observed["abandonedReason"] = *rt::get(abandoned, "release_reason");
  observed["committed"] = *rt::get(windowFor(*kv, policy), "committed_requests");
  observed["reserved"] = *rt::get(windowFor(*kv, policy), "reserved_requests");
  EXPECT(sameJson(observed, R"({"abandonedState":"released","abandonedReason":"lease_expired","committed":0,"reserved":1})"));
  rt::Task<void> releasing = releaseWith(replacement, std::nullopt);
  co_await std::move(releasing);
  const json::object after = pick(windowFor(*kv, policy), {{"committed", "committed_requests"}, {"reserved", "reserved_requests"}});
  EXPECT(sameJson(after, R"({"committed":0,"reserved":0})"));
}

ASYNC_TEST("bounded API-key quota responses publish standard rate-limit headers") {
  const double nowMs = rt::dateNow();
  const Setup setup = setupPolicy("rate-limit-headers", 1, nowMs);
  const auto& kv = setup.kv;
  const ApiKeyPolicy& policy = setup.policy;
  rt::Task<ApiKeyUsageReservation> reserving = reserve(kv, policy, "first-request", nowMs);
  co_await std::move(reserving);

  ReserveApiKeyUsageV3Options options;
  options.kv = kv;
  options.nowMs = nowMs;
  rt::Task<ApiKeyUsageReservationDecision> blocking = reserveApiKeyUsageV3(policy, "blocked-request", "responses", std::move(options));
  const ApiKeyUsageReservationDecision decision = co_await std::move(blocking);
  const auto* refused = std::get_if<ApiKeyUsageReservationDecisionNotOk>(&decision);
  if (refused == nullptr) throw std::runtime_error("second bounded request should be rate limited");

  EXPECT_EQ(refused->response.status(), 429);
  rt::Headers& headers = refused->response.headers();
  EXPECT_EQ(headers.get("RateLimit-Limit"), std::optional<std::string>("1"));
  EXPECT_EQ(headers.get("RateLimit-Remaining"), std::optional<std::string>("0"));
  EXPECT_EQ(headers.get("RateLimit-Policy"), std::optional<std::string>("\"api-key\";q=1;w=3600"));
  EXPECT(rt::Regex(R"(^"api-key";r=0;t=\d+$)").test(headers.get("RateLimit").value_or("")));
  EXPECT(rt::Regex(R"(^\d+$)").test(headers.get("RateLimit-Reset").value_or("")));
  EXPECT(rt::Regex(R"(^\d+$)").test(headers.get("Retry-After").value_or("")));
  rt::Task<json::value> reading = refused->response.json();
  const json::value body = co_await std::move(reading);
  EXPECT(sameJson(*rt::get(*rt::get(body, "error"), "code"), R"("rate_limit_exceeded")"));
  const rt::OrderedMap<std::string, std::string> policyHeaders = apiKeyRateLimitPolicyHeaders(&policy);
  EXPECT_EQ(policyHeaders.size(), std::size_t{1});
  const std::string* policyHeader = policyHeaders.get("RateLimit-Policy");
  EXPECT(policyHeader != nullptr && *policyHeader == "\"api-key\";q=1;w=3600");
}
