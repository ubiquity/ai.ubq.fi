// Port of tests/native-api-key-conformance.test.ts (all three cases): the shared vectors, the pure-plan
// test and the two "native API V3 wrapper" tests over tests/helpers/counting-kv.ts.
// Links: tests/native_api_key_conformance_test.cpp tests/helpers/counting_kv.cpp src/api_key_policy.cpp src/api_keys.cpp
//        src/authority/native_api_key_policy.cpp src/authority/native_api_key.cpp src/authority/native_contracts.cpp
//        src/authority/native_paid_policy.cpp src/http.cpp src/kv.cpp src/types.cpp src/utils.cpp src/config.cpp src/release.cpp
//        src/auth/relay.cpp src/provider/cerebras_rate_limits.cpp src/storage/kv.cpp src/storage/key_compatibility.cpp
// src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp
#include "uos/authority/native_api_key.hpp"
#include "uos/authority/native_contracts.hpp"

#include "tests/helpers/counting_kv.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/api_keys.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/types.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace apiKeyV3 = uos::authority::native_api_key;
using uos::authority::native_contracts::ApiKeyPolicy;
using uos::authority::native_contracts::ApiKeyUsageDispatchPlan;
using uos::authority::native_contracts::ApiKeyUsageReleasePlan;
using uos::authority::native_contracts::ApiKeyUsageReleasePlanMissingInvalidSettled;
using uos::authority::native_contracts::ApiKeyUsageReservationPlan;
using uos::authority::native_contracts::ApiKeyUsageReservationPlanFull;
using uos::authority::native_contracts::ApiKeyUsageReservationPlanUnavailable;
using uos::authority::native_contracts::ApiKeyUsageSnapshot;
using uos::authority::native_contracts::ApiKeyUsageWritePlan;
using uos::types::ApiKeyUsageRequestV3;

namespace {
  // The plans as the objects the TS returns: `{ kind, ...members }`.
  json::object kindOnly(std::string_view kind) {
    json::object object;
    rt::objectDefine(object, "kind", json::value(json::string(kind)));
    return object;
  }
  json::value planJson(const ApiKeyUsageWritePlan& plan) {
    json::array checks;
    for (const std::string& check : plan.checks) checks.push_back(json::value(json::string(check)));
    json::object object = kindOnly(ApiKeyUsageWritePlan::kind);
    rt::objectDefine(object, "checks", std::move(checks));
    rt::objectDefine(object, "request", json::value_from(plan.request));
    rt::objectDefine(object, "window", json::value_from(plan.window));
    rt::objectDefine(object, "expireIn", rt::makeNumber(plan.expireIn));
    return object;
  }
  json::value planJson(const ApiKeyUsageReservationPlan& plan) {
    return std::visit(
      [](const auto& alternative) -> json::value {
        using T = std::decay_t<decltype(alternative)>;
        if constexpr (std::is_same_v<T, ApiKeyUsageWritePlan>) {
          return planJson(alternative);
        } else if constexpr (std::is_same_v<T, ApiKeyUsageReservationPlanFull>) {
          json::object object = kindOnly(T::kind);
          rt::objectDefine(object, "window", json::value_from(alternative.window));
          return object;
        } else if constexpr (std::is_same_v<T, ApiKeyUsageReservationPlanUnavailable>) {
          json::object object = kindOnly(T::kind);
          rt::objectDefine(object, "reason", json::value(json::string(alternative.reason)));
          return object;
        } else {
          return kindOnly(T::kind);
        }
      },
      plan);
  }
  json::value planJson(const ApiKeyUsageDispatchPlan& plan) {
    return std::visit(
      [](const auto& alternative) -> json::value {
        using T = std::decay_t<decltype(alternative)>;
        if constexpr (std::is_same_v<T, ApiKeyUsageWritePlan>) return planJson(alternative);
        else return kindOnly(T::kind);
      },
      plan);
  }
  json::value planJson(const ApiKeyUsageReleasePlan& plan) {
    if (const auto* write = std::get_if<ApiKeyUsageWritePlan>(&plan)) return planJson(*write);
    return kindOnly(std::get<ApiKeyUsageReleasePlanMissingInvalidSettled>(plan).kind);
  }
  std::string kindOf(const json::value& plan) { return std::string(rt::get(plan, "kind")->get_string()); }
  json::value snapshotJson(const ApiKeyUsageSnapshot& snapshot) {
    json::object object;
    rt::objectDefine(object, "request", snapshot.request);
    rt::objectDefine(object, "window", snapshot.window);
    return object;
  }

  // BEGIN SHARED_API_KEY_VECTORS
  /** This exact synthetic vector function also runs against the real pure module in workerd. */
  struct ConformanceResults {
    std::vector<apiKeyV3::ApiKeyUsageWindowBounds> window;
    std::string version;
    std::vector<double> retention;
    ApiKeyUsageWritePlan reserved;
    ApiKeyUsageReservationPlan unlimited;
    ApiKeyUsageReservationPlan zero;
    ApiKeyUsageReservationPlan full;
    ApiKeyUsageReservationPlan reuse;
    ApiKeyUsageReservationPlan consumed;
    ApiKeyUsageReservationPlan identityConflict;
    ApiKeyUsageReservationPlan malformedRequest;
    ApiKeyUsageReservationPlan malformedWindow;
    ApiKeyUsageWritePlan dispatched;
    ApiKeyUsageDispatchPlan repeatedDispatch;
    ApiKeyUsageDispatchPlan expiredDispatch;
    ApiKeyUsageWritePlan cancelled;
    ApiKeyUsageReleasePlan repeatedCancellation;
    ApiKeyUsageReleasePlan foreignProvider;
    ApiKeyUsageReleasePlan released;
    ApiKeyUsageReleasePlan releaseDispatched;
    ApiKeyUsageReleasePlan missingRelease;
    std::vector<bool> lease;
    std::vector<bool> providerNormalization;
    std::optional<ApiKeyUsageRequestV3> unknownProvider;
    bool snapshotUnchanged;
  };

  ConformanceResults runApiKeyConformanceVectors() {
    const double now = 10'000'000;
    const ApiKeyPolicy policy{
      .token_hash = "synthetic",
      .key_id = "key",
      .expires_at_ms = -1,
      .usage_limit_requests = 1,
      .window_ms = 60'000,
      .window_start_ms = now,
      .usage_reset_at_ms = now + 60'000,
      .policy_version = "v3:60000",
      .paid_fallback_enabled = false,
      .paid_fallback_limit_microcredits = 0,
      .paid_fallback_spent_microcredits = 0,
      .paid_fallback_reserved_microcredits = 0,
      .paid_fallback_reservation_request_id = std::nullopt,
    };
    const ApiKeyUsageSnapshot empty{nullptr, nullptr};
    const ApiKeyUsageReservationPlan reservedPlan = apiKeyV3::planApiKeyUsageReservation(policy, "request", "responses", empty, now);
    const auto* reserved = std::get_if<ApiKeyUsageWritePlan>(&reservedPlan);
    if (reserved == nullptr) throw std::runtime_error("Synthetic reservation was not writable");
    const ApiKeyUsageSnapshot snapshot{json::value_from(reserved->request), json::value_from(reserved->window)};
    const std::string before = rt::jsonStringify(snapshotJson(snapshot));
    const ApiKeyUsageDispatchPlan dispatchedPlan = apiKeyV3::planApiKeyUsageDispatch(policy, "request", "responses", "claude_code", snapshot, now + 1);
    const auto* dispatched = std::get_if<ApiKeyUsageWritePlan>(&dispatchedPlan);
    if (dispatched == nullptr) throw std::runtime_error("Synthetic dispatch was not writable");
    const ApiKeyUsageSnapshot dispatchedSnapshot{json::value_from(dispatched->request), json::value_from(dispatched->window)};
    const ApiKeyUsageReleasePlan cancelledPlan =
      apiKeyV3::planApiKeyUsageDispatchCancellation(policy, "request", "responses", "claude_code", dispatchedSnapshot, now + 2);
    const auto* cancelled = std::get_if<ApiKeyUsageWritePlan>(&cancelledPlan);
    if (cancelled == nullptr) throw std::runtime_error("Synthetic cancellation was not writable");
    const ApiKeyUsageReleasePlan released = apiKeyV3::planApiKeyUsageReservedRelease(policy, "request", "lease_expired", snapshot, now + 300'000);

    ApiKeyPolicy unlimitedPolicy = policy;
    unlimitedPolicy.usage_limit_requests = -1;
    json::value usedWindow = json::value_from(reserved->window);
    rt::objectSet(usedWindow.as_object(), "committed_requests", rt::makeNumber(100.0));
    ApiKeyPolicy zeroPolicy = policy;
    zeroPolicy.usage_limit_requests = 0;
    std::vector<bool> providerNormalization;
    for (const std::string_view provider : apiKeyV3::API_KEY_USAGE_PROVIDERS) {
      json::value request = json::value_from(dispatched->request);
      rt::objectSet(request.as_object(), "provider", json::value(json::string(provider)));
      providerNormalization.push_back(apiKeyV3::normalizeApiKeyUsageRequestV3(request).has_value());
    }
    EXPECT(std::find(apiKeyV3::API_KEY_USAGE_PROVIDERS.begin(), apiKeyV3::API_KEY_USAGE_PROVIDERS.end(), "gemini") !=
           apiKeyV3::API_KEY_USAGE_PROVIDERS.end());
    json::value unknownProviderRequest = json::value_from(dispatched->request);
    rt::objectSet(unknownProviderRequest.as_object(), "provider", json::value("openlux"));

    return ConformanceResults{
      .window = {apiKeyV3::currentApiKeyUsageWindow(now + 60'000, 60'000, now), apiKeyV3::currentApiKeyUsageWindow(now + 60'000, 60'000, now + 60'000)},
      .version = apiKeyV3::apiKeyUsageV3PolicyVersion(60'000),
      .retention =
        {
          apiKeyV3::apiKeyUsageV3RetentionMs(policy.usage_reset_at_ms, now),
          apiKeyV3::apiKeyUsageV3RetentionMs(policy.usage_reset_at_ms, now + 1),
          apiKeyV3::apiKeyUsageV3RetentionMs(policy.usage_reset_at_ms, now + 700'000'000),
        },
      .reserved = *reserved,
      .unlimited = apiKeyV3::planApiKeyUsageReservation(unlimitedPolicy, "unlimited", "responses", ApiKeyUsageSnapshot{nullptr, usedWindow}, now),
      .zero = apiKeyV3::planApiKeyUsageReservation(zeroPolicy, "zero", "responses", empty, now),
      .full = apiKeyV3::planApiKeyUsageReservation(policy, "other", "responses", ApiKeyUsageSnapshot{nullptr, json::value_from(reserved->window)}, now),
      .reuse = apiKeyV3::planApiKeyUsageReservation(policy, "request", "responses", ApiKeyUsageSnapshot{json::value_from(reserved->request), json::object{}}, now),
      .consumed = apiKeyV3::planApiKeyUsageReservation(policy, "request", "responses", dispatchedSnapshot, now),
      .identityConflict = apiKeyV3::planApiKeyUsageReservation(policy, "request", "chat.completions", snapshot, now),
      .malformedRequest = apiKeyV3::planApiKeyUsageReservation(policy, "request", "responses", ApiKeyUsageSnapshot{json::object{}, nullptr}, now),
      .malformedWindow = apiKeyV3::planApiKeyUsageReservation(policy, "request", "responses", ApiKeyUsageSnapshot{nullptr, json::object{}}, now),
      .dispatched = *dispatched,
      .repeatedDispatch = apiKeyV3::planApiKeyUsageDispatch(policy, "request", "responses", "surplus", dispatchedSnapshot, now + 2),
      .expiredDispatch = apiKeyV3::planApiKeyUsageDispatch(policy, "request", "responses", "chatgpt_codex", snapshot, now + 300'001),
      .cancelled = *cancelled,
      .repeatedCancellation = apiKeyV3::planApiKeyUsageDispatchCancellation(
        policy, "request", "responses", "claude_code", ApiKeyUsageSnapshot{json::value_from(cancelled->request), json::value_from(cancelled->window)}, now + 3),
      .foreignProvider = apiKeyV3::planApiKeyUsageDispatchCancellation(policy, "request", "responses", "surplus", dispatchedSnapshot, now + 2),
      .released = released,
      .releaseDispatched = apiKeyV3::planApiKeyUsageReservedRelease(policy, "request", "done", dispatchedSnapshot, now + 2),
      .missingRelease = apiKeyV3::planApiKeyUsageReservedRelease(policy, "request", "done", empty, now),
      .lease =
        {
          apiKeyV3::isApiKeyUsageReservationExpired(reserved->request, now + 299'999),
          apiKeyV3::isApiKeyUsageReservationExpired(reserved->request, now + 300'000),
        },
      .providerNormalization = providerNormalization,
      .unknownProvider = apiKeyV3::normalizeApiKeyUsageRequestV3(unknownProviderRequest),
      .snapshotUnchanged = before == rt::jsonStringify(snapshotJson(snapshot)),
    };
  }
  // END SHARED_API_KEY_VECTORS

  bool deepEquals(const json::value& actual, std::string_view expected) { return rt::deepEqual(actual, rt::jsonParse(expected)); }
}

TEST("native API V3 pure plans preserve policy identity, named write sets, lease and dispatch transitions") {
  const ConformanceResults result = runApiKeyConformanceVectors();
  EXPECT_EQ(result.version, "v3:60000");
  const std::vector<apiKeyV3::ApiKeyUsageWindowBounds> windows = {{10'000'000, 10'060'000}, {10'060'000, 10'120'000}};
  EXPECT(result.window == windows);
  EXPECT(result.retention == (std::vector<double>{604'860'000, 604'859'999, 1}));
  EXPECT(result.reserved.checks == (std::vector<std::string>{"policy", "window", "request"}));
  EXPECT_EQ(result.reserved.window.reserved_requests, 1.0);
  EXPECT_EQ(result.reserved.request.lease_expires_at_ms - result.reserved.request.reserved_at_ms, 300'000.0);
  EXPECT_EQ(kindOf(planJson(result.unlimited)), "write");
  EXPECT_EQ(kindOf(planJson(result.zero)), "full");
  EXPECT_EQ(kindOf(planJson(result.full)), "full");
  EXPECT(deepEquals(planJson(result.reuse), R"({"kind":"reuse"})"));
  EXPECT(deepEquals(planJson(result.consumed), R"({"kind":"unavailable","reason":"identity_consumed"})"));
  EXPECT(deepEquals(planJson(result.identityConflict), R"({"kind":"unavailable","reason":"identity_conflict"})"));
  EXPECT(deepEquals(planJson(result.malformedRequest), R"({"kind":"unavailable","reason":"request_malformed"})"));
  EXPECT(deepEquals(planJson(result.malformedWindow), R"({"kind":"unavailable","reason":"window_malformed"})"));
  const std::vector<std::string> dispatchedChecks{"request", "window"};
  EXPECT(result.dispatched.checks == dispatchedChecks);
  EXPECT_EQ(result.dispatched.window.reserved_requests, 0.0);
  EXPECT_EQ(result.dispatched.window.committed_requests, 1.0);
  EXPECT(deepEquals(planJson(result.repeatedDispatch), R"({"kind":"already_dispatched"})"));
  // Native dispatch does not add a new lease-expiry or hash-policy fence after reservation.
  EXPECT_EQ(kindOf(planJson(result.expiredDispatch)), "write");
  const std::vector<std::string> cancelledChecks{"request", "window"};
  EXPECT(result.cancelled.checks == cancelledChecks);
  EXPECT_EQ(result.cancelled.window.committed_requests, 0.0);
  EXPECT(!result.cancelled.request.provider.has_value());
  EXPECT_EQ(result.cancelled.request.release_reason, std::optional<std::string>("transport_cancelled_before_fetch"));
  EXPECT(deepEquals(planJson(result.repeatedCancellation), R"({"kind":"settled"})"));
  EXPECT(deepEquals(planJson(result.foreignProvider), R"({"kind":"invalid"})"));
  EXPECT_EQ(kindOf(planJson(result.released)), "write");
  EXPECT(deepEquals(planJson(result.releaseDispatched), R"({"kind":"settled"})"));
  EXPECT(deepEquals(planJson(result.missingRelease), R"({"kind":"missing"})"));
  EXPECT(result.lease == (std::vector<bool>{false, true}));
  EXPECT(std::all_of(result.providerNormalization.begin(), result.providerNormalization.end(), [](bool ok) { return ok; }));
  EXPECT(!result.unknownProvider.has_value());
  EXPECT_EQ(result.snapshotUnchanged, true);
}

namespace {
  using uos::tests::helpers::counting_kv::CountingKv;
  using uos::tests::helpers::counting_kv::KvCommandRecord;
  namespace api_key_policy = uos::api_key_policy;

  uos::types::ApiKeyHashRecord syntheticRecord() {
    return uos::types::ApiKeyHashRecord{
      .id = "source-cas",
      .expires_at_ms = -1,
      .revoked_at_ms = std::nullopt,
      .usage_limit_requests = 1,
      .usage_requests = 0,
      .usage_reset_at_ms = rt::dateNow() + 3'600'000,
      .window_ms = 3'600'000,
      .usage_quota_version = 3,
      .paid_fallback_enabled = false,
      .paid_fallback_limit_microcredits = 0,
      .paid_fallback_spent_microcredits = 0,
      .paid_fallback_reserved_microcredits = 0,
      .paid_fallback_reservation_request_id = std::nullopt,
    };
  }

  rt::KvValue stored(const uos::types::ApiKeyHashRecord& record) { return rt::KvValue(json::value_from(record)); }

  // `operation.commit = () => { this.attempts += 1; return Promise.resolve({ ok: false }); }`
  class ConflictOperation final : public rt::ForwardingAtomicOperation {
  public:
    ConflictOperation(std::unique_ptr<rt::AtomicOperation> inner_, double& attempts_) : rt::ForwardingAtomicOperation(std::move(inner_)), attempts(attempts_) {}
    rt::Task<rt::KvCommitResult> commit() override {
      attempts += 1;
      rt::KvCommitResult result;
      result.ok = false;
      co_await rt::microtask();
      co_return result;
    }

  private:
    double& attempts;
  };

  class ConflictKv final : public CountingKv {
  public:
    double attempts = 0;
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<ConflictOperation>(CountingKv::atomic(), attempts); }
  };

  // `operation.commit = () => { if (!this.changed) { this.changed = true; this.seed(...) } return commit(); }`
  class PolicyRaceKv;
  class PolicyRaceOperation final : public rt::ForwardingAtomicOperation {
  public:
    PolicyRaceOperation(std::unique_ptr<rt::AtomicOperation> inner_, PolicyRaceKv& kv_) : rt::ForwardingAtomicOperation(std::move(inner_)), kv(kv_) {}
    rt::Task<rt::KvCommitResult> commit() override;

  private:
    PolicyRaceKv& kv;
  };

  class PolicyRaceKv final : public CountingKv {
  public:
    explicit PolicyRaceKv(uos::types::ApiKeyHashRecord record_) : record(std::move(record_)) {}
    bool changed = false;
    uos::types::ApiKeyHashRecord record;
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<PolicyRaceOperation>(CountingKv::atomic(), *this); }
  };

  rt::Task<rt::KvCommitResult> PolicyRaceOperation::commit() {
    if (!kv.changed) {
      kv.changed = true;
      uos::types::ApiKeyHashRecord zero = kv.record;
      zero.usage_limit_requests = 0;
      kv.seed(uos::api_keys::apiKeyHashKey("synthetic"), stored(zero));
    }
    rt::Task<rt::KvCommitResult> committing = inner->commit();
    const rt::KvCommitResult result = co_await std::move(committing);
    co_return result;
  }

  // `key[2] === "hash" ? "policy" : key[3]`
  std::string keyRole(const rt::KvKey& key) {
    if (key.size() > 2 && key[2].isString() && key[2].string() == "hash") return "policy";
    return key.size() > 3 ? key[3].toString() : "undefined";
  }
}

ASYNC_TEST("native API V3 wrapper retains five CAS attempts and leaves a failed reservation uncreated") {
  const auto kv = std::make_shared<ConflictKv>();
  const uos::types::ApiKeyHashRecord record = syntheticRecord();
  const std::optional<ApiKeyPolicy> policy = api_key_policy::apiKeyPolicyFromHashRecord("synthetic", record, rt::dateNow());
  EXPECT(policy.has_value());
  kv->seed(uos::api_keys::apiKeyHashKey("synthetic"), stored(record));
  api_key_policy::ReserveApiKeyUsageV3Options options;
  options.kv = kv;
  rt::Task<api_key_policy::ApiKeyUsageReservationDecision> reserving = api_key_policy::reserveApiKeyUsageV3(*policy, "request", "responses", std::move(options));
  const api_key_policy::ApiKeyUsageReservationDecision decision = co_await std::move(reserving);
  const auto* refused = std::get_if<api_key_policy::ApiKeyUsageReservationDecisionNotOk>(&decision);
  EXPECT(refused != nullptr);
  EXPECT_EQ(refused->response.status(), 503);
  EXPECT_EQ(kv->attempts, 5.0);
  EXPECT_EQ(kv->entries.size(), std::size_t{1});
  const auto hashReads = std::count_if(kv->commands.begin(), kv->commands.end(), [](const KvCommandRecord& command) {
    return command.command == "get" && !command.keys.empty() && command.keys[0].size() > 2 && command.keys[0][2].isString() && command.keys[0][2].string() == "hash";
  });
  EXPECT_EQ(hashReads, 5);
}

ASYNC_TEST("native API V3 wrapper keeps the hash snapshot in reservation CAS and rereads a racing zero limit") {
  const uos::types::ApiKeyHashRecord record = syntheticRecord();
  const auto kv = std::make_shared<PolicyRaceKv>(record);
  const std::optional<ApiKeyPolicy> policy = api_key_policy::apiKeyPolicyFromHashRecord("synthetic", record, rt::dateNow());
  EXPECT(policy.has_value());
  kv->seed(uos::api_keys::apiKeyHashKey("synthetic"), stored(record));
  api_key_policy::ReserveApiKeyUsageV3Options options;
  options.kv = kv;
  rt::Task<api_key_policy::ApiKeyUsageReservationDecision> reserving = api_key_policy::reserveApiKeyUsageV3(*policy, "request", "responses", std::move(options));
  const api_key_policy::ApiKeyUsageReservationDecision decision = co_await std::move(reserving);
  const auto* refused = std::get_if<api_key_policy::ApiKeyUsageReservationDecisionNotOk>(&decision);
  EXPECT(refused != nullptr);
  EXPECT_EQ(refused->response.status(), 429);
  EXPECT_EQ(kv->entries.size(), std::size_t{1});
  const auto lostCommit = std::find_if(kv->commands.begin(), kv->commands.end(), [](const KvCommandRecord& command) { return command.command == "atomic.commit"; });
  EXPECT(lostCommit != kv->commands.end());
  EXPECT_EQ(lostCommit->atomicResult, std::optional<std::string>("conflict"));
  EXPECT_EQ(lostCommit->atomicChecks, 3.0);
  EXPECT_EQ(lostCommit->atomicMutations, 2.0);
  std::vector<std::string> roles;
  for (std::size_t index = 0; index < 3 && index < lostCommit->keys.size(); ++index) roles.push_back(keyRole(lostCommit->keys[index]));
  EXPECT(roles == (std::vector<std::string>{"policy", "window", "request"}));
}
