// Differential cases for src/authority/native-paid-contracts.ts recorded from Bun 1.4.0.
// The module declares types only; tests/native-paid-conformance.test.ts drives them
// through src/authority/native-paid.ts (not ported yet). These cases pin the C++ form:
// the JSON of each stored or reported record against JSON.stringify of the same object
// literal under Bun, value_to of that JSON, and the shapes the conformance vectors build.
#include "uos/authority/native_paid_contracts.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

using namespace uos;
using namespace uos::authority::native_paid_contracts;

namespace {
  constexpr std::string_view PaidFallbackPendingV3Json = R"({"created_at_ms":1700000000000,"next_reconciliation_at_ms":1700000005000})";
  constexpr std::string_view PaidFallbackReconciliationGateV3Json = R"({"next_due_at_ms":1700000100000})";
  constexpr std::string_view PaidFallbackReconciliationGateV3NullJson = R"({"next_due_at_ms":null})";
  constexpr std::string_view PaidFallbackReconciliationLeaseV3Json = R"({"token":"lease","expires_at_ms":1700000060000})";
  constexpr std::string_view PaidFallbackBackfillLeaseV3Json = R"({"token":"backfill","expires_at_ms":1700000300000})";
  constexpr std::string_view PaidFallbackBackfillStateV3Json = R"({"v":1,"completed_at_ms":1700000000000})";
  constexpr std::string_view PaidFallbackDeletionGuardV3Json = R"({"created_at_ms":1700000000000})";
  constexpr std::string_view PaidFallbackAdmissionV3Json = R"({"key_id":"key","request_id":"request","created_at_ms":1700000000000,"reserved_microcredits":80,"quota_per_credit":500000,"window_reset_at_ms":1700000060000,"quota_used_percent":12.5})";
  constexpr std::string_view PaidFallbackAdmissionV3NullJson = R"({"key_id":"key","request_id":"request","created_at_ms":1700000000000,"reserved_microcredits":0,"quota_per_credit":500000,"window_reset_at_ms":1700000060000,"quota_used_percent":null})";
  constexpr std::string_view PaidFallbackWindowProjectionV3Json = R"({"key_id":"key","policy_version":null,"window_reset_at_ms":1700000000000,"limit_microcredits":-1,"settled_microcredits":0,"reserved_microcredits":0,"pending_count":0,"updated_at_ms":null})";
  constexpr std::string_view PaidFallbackOutstandingV3Json = R"({"pending_requests":0,"unresolved_requests":1,"pending_markers":1,"has_outstanding":true})";
  constexpr std::string_view PaidFallbackRequestLifecyclePatchV3Json = R"({"dispatch_state":"dispatched","provider":"metered","provider_request_id":"provider-id"})";
  constexpr std::string_view PaidFallbackRequestLifecyclePatchV3FullJson = R"({"provider":"surplus","provider_request_id":null,"dispatch_state":"reserved","terminal_state":"failed","reconciliation_attempts":2,"last_reconciliation_at_ms":null,"increment_reconciliation_attempts":true})";
  constexpr std::string_view PaidFallbackRequestLifecyclePatchV3EmptyJson = R"({})";
  constexpr std::string_view PaidFallbackUsageRollupJson = R"({"v":1,"bucket_start_at_ms":1700000000000,"model":"model","provider":"metered","request_count":1,"quota_sum":400000,"input_tokens":12,"cached_input_tokens":3,"output_tokens":4,"spend_microcredits":800000,"first_request_at_ms":1700000000000,"last_request_at_ms":1700000000000,"updated_at_ms":1700000000002})";
  constexpr std::string_view PaidFallbackUsageRollupInputJson = R"({"bucket_start_at_ms":1700000000000,"model":"model","provider":"surplus","request_id":"r","quota":0,"input_tokens":1,"cached_input_tokens":null,"output_tokens":2,"spend_microcredits":69,"request_created_at_ms":1700000000000,"updated_at_ms":1700000000000})";
  constexpr std::string_view TerminalUsageRollupInputJson = R"({"model":" model ","provider":"chatgpt_codex","request_id":"terminal","request_created_at_ms":1700000000000,"input_tokens":12,"cached_input_tokens":null,"output_tokens":4})";
  constexpr std::string_view TerminalUsageRollupInputNullsJson = R"({"model":null,"provider":null,"request_id":"t2","input_tokens":null,"cached_input_tokens":null,"output_tokens":null})";
  constexpr std::string_view NativePaidProviderLogJson = R"({"request_id":"provider-id","quota":400000,"prompt_tokens":12,"cached_prompt_tokens":3,"completion_tokens":4,"model":"model","created_at":1700000000})";
  constexpr std::string_view NativePaidProviderLogCacheWriteJson = R"({"request_id":"p2","quota":1.5,"prompt_tokens":0,"cache_write_prompt_tokens":7,"completion_tokens":0,"model":"m","created_at":1700000000.123})";

  template <class T> void checkJson(const T& value, std::string_view expected, const char* label) {
    const std::string written = rt::jsonStringify(json::value_from(value));
    if (written != expected) rt::test::report(__FILE__, __LINE__, std::string(label) + " value_from\n      actual:   " + written + "\n      expected: " + std::string(expected), false);
    const T parsed = json::value_to<T>(rt::jsonParse(expected));
    if (!(parsed == value)) rt::test::report(__FILE__, __LINE__, std::string(label) + " value_to differs from the record", false);
  }

  template <class T> std::string rejection(std::string_view text) {
    try {
      (void)json::value_to<T>(rt::jsonParse(text));
    } catch (const rt::TypeError& error) {
      return error.message;
    }
    return "<accepted>";
  }

  // The conformance vectors' `row(value, versionstamp = "opaque-revision")`.
  template <class T> NativePaidRow<T> row(std::optional<T> value, std::optional<std::string> versionstamp = "opaque-revision") {
    return NativePaidRow<T>{.value = std::move(value), .versionstamp = std::move(versionstamp)};
  }

  constexpr double now = 1'700'000'000'000;

  PaidFallbackRequestV3 admittedRequest() {
    return {
      .key_id = "key",
      .request_id = "request",
      .policy_version = "policy-v3",
      .route = "responses",
      .path = "/v1/responses",
      .model = "model",
      .stream = false,
      .reasoning = std::nullopt,
      .window_reset_at_ms = now + 60'000,
      .reserved_microcredits = 80,
      .quota_per_credit = 500'000,
      .provider_request_id = std::nullopt,
      .provider_quota = std::nullopt,
      .input_tokens = std::nullopt,
      .output_tokens = std::nullopt,
      .dispatch_state = "reserved",
      .terminal_state = "pending",
      .spend_microcredits = std::nullopt,
      .billing_state = "pending",
      .reconciliation_attempts = 0,
      .last_reconciliation_at_ms = std::nullopt,
      .dispatched_at_ms = std::nullopt,
      .terminal_at_ms = std::nullopt,
      .settled_at_ms = std::nullopt,
      .created_at_ms = now,
      .updated_at_ms = now,
    };
  }
}

TEST("re-exported types are the types.ts records") {
  static_assert(std::is_same_v<PaidFallbackRequestV3, types::PaidFallbackRequestV3>);
  static_assert(std::is_same_v<PaidFallbackWindowV3, types::PaidFallbackWindowV3>);
  static_assert(std::is_same_v<PaidFallbackProvider, types::PaidFallbackProvider>);
  static_assert(std::is_same_v<NativePaidKey, rt::KvKey>);
  const NativePaidKey key{"uos_ai", "paid_fallback", "v3", "request", "key", "request"};
  EXPECT_EQ(key.size(), std::size_t{6});
}

TEST("stored paid-fallback rows") {
  checkJson(PaidFallbackPendingV3{.created_at_ms = now, .next_reconciliation_at_ms = now + 5'000}, PaidFallbackPendingV3Json, "PaidFallbackPendingV3");
  checkJson(PaidFallbackReconciliationGateV3{.next_due_at_ms = now + 100'000}, PaidFallbackReconciliationGateV3Json, "PaidFallbackReconciliationGateV3");
  checkJson(PaidFallbackReconciliationGateV3{.next_due_at_ms = std::nullopt}, PaidFallbackReconciliationGateV3NullJson, "PaidFallbackReconciliationGateV3Null");
  checkJson(PaidFallbackReconciliationLeaseV3{.token = "lease", .expires_at_ms = now + 60'000}, PaidFallbackReconciliationLeaseV3Json, "PaidFallbackReconciliationLeaseV3");
  checkJson(PaidFallbackBackfillLeaseV3{.token = "backfill", .expires_at_ms = now + 300'000}, PaidFallbackBackfillLeaseV3Json, "PaidFallbackBackfillLeaseV3");
  checkJson(PaidFallbackBackfillStateV3{.completed_at_ms = now}, PaidFallbackBackfillStateV3Json, "PaidFallbackBackfillStateV3");
  checkJson(PaidFallbackDeletionGuardV3{.created_at_ms = now}, PaidFallbackDeletionGuardV3Json, "PaidFallbackDeletionGuardV3");
  checkJson(PaidFallbackUsageRollup{
              .bucket_start_at_ms = now,
              .model = "model",
              .provider = "metered",
              .request_count = 1,
              .quota_sum = 400'000,
              .input_tokens = 12,
              .cached_input_tokens = 3,
              .output_tokens = 4,
              .spend_microcredits = 800'000,
              .first_request_at_ms = now,
              .last_request_at_ms = now,
              .updated_at_ms = now + 2,
            },
            PaidFallbackUsageRollupJson, "PaidFallbackUsageRollup");
}

TEST("reported paid-fallback projections") {
  checkJson(PaidFallbackAdmissionV3{
              .key_id = "key", .request_id = "request", .created_at_ms = now, .reserved_microcredits = 80, .quota_per_credit = 500'000, .window_reset_at_ms = now + 60'000, .quota_used_percent = 12.5},
            PaidFallbackAdmissionV3Json, "PaidFallbackAdmissionV3");
  checkJson(PaidFallbackAdmissionV3{
              .key_id = "key", .request_id = "request", .created_at_ms = now, .reserved_microcredits = 0, .quota_per_credit = 500'000, .window_reset_at_ms = now + 60'000, .quota_used_percent = std::nullopt},
            PaidFallbackAdmissionV3NullJson, "PaidFallbackAdmissionV3Null");
  checkJson(PaidFallbackWindowProjectionV3{
              .key_id = "key",
              .policy_version = std::nullopt,
              .window_reset_at_ms = now,
              .limit_microcredits = -1,
              .settled_microcredits = 0,
              .reserved_microcredits = 0,
              .pending_count = 0,
              .updated_at_ms = std::nullopt,
            },
            PaidFallbackWindowProjectionV3Json, "PaidFallbackWindowProjectionV3");
  checkJson(PaidFallbackOutstandingV3{.pending_requests = 0, .unresolved_requests = 1, .pending_markers = 1, .has_outstanding = true}, PaidFallbackOutstandingV3Json, "PaidFallbackOutstandingV3");
}

TEST("rollup inputs and provider logs") {
  checkJson(PaidFallbackUsageRollupInput{
              .bucket_start_at_ms = now,
              .model = "model",
              .provider = "surplus",
              .request_id = "r",
              .quota = 0,
              .input_tokens = 1,
              .cached_input_tokens = std::nullopt,
              .output_tokens = 2,
              .spend_microcredits = 69,
              .request_created_at_ms = now,
              .updated_at_ms = now,
            },
            PaidFallbackUsageRollupInputJson, "PaidFallbackUsageRollupInput");
  checkJson(TerminalUsageRollupInput{
              .model = " model ", .provider = "chatgpt_codex", .request_id = "terminal", .request_created_at_ms = now, .input_tokens = 12, .cached_input_tokens = std::nullopt, .output_tokens = 4},
            TerminalUsageRollupInputJson, "TerminalUsageRollupInput");
  checkJson(TerminalUsageRollupInput{
              .model = std::nullopt, .provider = std::nullopt, .request_id = "t2", .input_tokens = std::nullopt, .cached_input_tokens = std::nullopt, .output_tokens = std::nullopt},
            TerminalUsageRollupInputNullsJson, "TerminalUsageRollupInputNulls");
  checkJson(NativePaidProviderLog{.request_id = "provider-id", .quota = 400'000, .prompt_tokens = 12, .cached_prompt_tokens = 3, .completion_tokens = 4, .model = "model", .created_at = now / 1000},
            NativePaidProviderLogJson, "NativePaidProviderLog");
  checkJson(NativePaidProviderLog{.request_id = "p2", .quota = 1.5, .prompt_tokens = 0, .cache_write_prompt_tokens = 7, .completion_tokens = 0, .model = "m", .created_at = 1'700'000'000.123},
            NativePaidProviderLogCacheWriteJson, "NativePaidProviderLogCacheWrite");
}

TEST("lifecycle patches keep undefined and null apart") {
  checkJson(PaidFallbackRequestLifecyclePatchV3{
              .provider = "surplus",
              .provider_request_id = nullptr,
              .dispatch_state = "reserved",
              .terminal_state = "failed",
              .reconciliation_attempts = 2,
              .last_reconciliation_at_ms = nullptr,
              .increment_reconciliation_attempts = true,
            },
            PaidFallbackRequestLifecyclePatchV3FullJson, "PaidFallbackRequestLifecyclePatchV3Full");
  checkJson(PaidFallbackRequestLifecyclePatchV3{}, PaidFallbackRequestLifecyclePatchV3EmptyJson, "PaidFallbackRequestLifecyclePatchV3Empty");
  // The conformance vector writes this literal as { dispatch_state, provider, provider_request_id };
  // a struct writes its members in declaration order, so only the members are compared here.
  const PaidFallbackRequestLifecyclePatchV3 dispatched{.provider = "metered", .provider_request_id = std::string("provider-id"), .dispatch_state = "dispatched"};
  EXPECT(json::value_to<PaidFallbackRequestLifecyclePatchV3>(rt::jsonParse(PaidFallbackRequestLifecyclePatchV3Json)) == dispatched);
  EXPECT(rt::deepEqual(json::value_from(dispatched), rt::jsonParse(PaidFallbackRequestLifecyclePatchV3Json)));
  EXPECT_EQ(rejection<PaidFallbackRequestLifecyclePatchV3>(R"({"provider":null})"), "PaidFallbackRequestLifecyclePatchV3.provider is null, not string or undefined");
  EXPECT_EQ(rejection<PaidFallbackReconciliationGateV3>("{}"), "PaidFallbackReconciliationGateV3.next_due_at_ms is undefined, not number or null");
}

TEST("snapshots and plans built the way the conformance vectors build them") {
  const NativePaidAdmissionSnapshot empty{
    .request = row<PaidFallbackRequestV3>(std::nullopt, std::nullopt),
    .window = row<PaidFallbackWindowV3>(std::nullopt, std::nullopt),
    .deletionGuard = row<PaidFallbackDeletionGuardV3>(std::nullopt, std::nullopt),
    .policyFence = NativePaidAdmissionSnapshot::PolicyFence{.versionstamp = "H:opaque"},
  };
  EXPECT(!empty.request.value && !empty.request.versionstamp && empty.request.expiresAtMs == rt::undefined);
  EXPECT(empty.policyFence->versionstamp == std::optional<std::string>("H:opaque"));
  const NativePaidRow<PaidFallbackReconciliationGateV3> gate = row<PaidFallbackReconciliationGateV3>(PaidFallbackReconciliationGateV3{.next_due_at_ms = now + 100'000});
  EXPECT(gate.versionstamp == std::optional<std::string>("opaque-revision"));

  const NativePaidAdmissionInput input{
    .keyId = "key",
    .requestId = "request",
    .createdAtMs = now,
    .policyVersion = "policy-v3",
    .limitMicrocredits = 100,
    .maximumExposureMicrocredits = 80,
    .initialSettledMicrocredits = 0,
    .quotaPerCredit = 500'000,
    .windowResetAtMs = now + 60'000,
    .model = " model ",
    .route = "responses",
    .path = "/v1/responses",
    .stream = false,
    .reasoning = std::nullopt,
  };
  EXPECT(input.dispatchIntent == std::nullopt);

  const auto kindOf = [](const auto& plan) { return std::visit([](const auto& alternative) { return std::string(alternative.kind); }, plan); };
  const NativePaidAdmissionPlan blocked = NativePaidAdmissionPlanBlocked{.reason = "invalid_policy"};
  EXPECT_EQ(kindOf(blocked), "blocked");
  EXPECT(blocked == NativePaidAdmissionPlan(NativePaidAdmissionPlanBlocked{.reason = "invalid_policy"}));
  EXPECT(!(blocked == NativePaidAdmissionPlan(NativePaidAdmissionPlanBlocked{.reason = "limit_exceeded"})));
  EXPECT_EQ(kindOf(NativePaidAdmissionPlan(NativePaidAdmissionPlanExhaustedPolicy{.window = {}, .windowExpireIn = 1, .checks = {"window", "deletionGuard", "policy"}})), "exhausted_policy");

  const NativePaidDeferralPlan noop = NativePaidDeferralPlanNoop{};
  const NativePaidDeferralPlan cleanup = NativePaidDeferralPlanCleanup{};
  EXPECT_EQ(kindOf(noop), "noop");
  EXPECT_EQ(std::get<NativePaidDeferralPlanNoop>(noop).checks, std::vector<std::string>{});
  EXPECT_EQ(kindOf(cleanup), "cleanup");
  EXPECT_EQ(std::get<NativePaidDeferralPlanCleanup>(cleanup).checks, (std::vector<std::string>{"pending", "gate"}));
  const NativePaidDeferralPlanWrite deferred{.request = admittedRequest(), .pending = {.created_at_ms = now, .next_reconciliation_at_ms = now}, .delay = 21'600'000, .requestExpireIn = 1};
  EXPECT_EQ(deferred.checks, (std::vector<std::string>{"request", "pending"}));

  const NativePaidSettlementEvaluation settle = NativePaidSettlementEvaluationSettle{.spend = 800'000};
  EXPECT_EQ(kindOf(settle), "settle");
  EXPECT_EQ(kindOf(NativePaidSettlementEvaluation(NativePaidSettlementEvaluationDefer{})), "defer");
  EXPECT(settle == NativePaidSettlementEvaluation(NativePaidSettlementEvaluationSettle{.spend = 800'000}));

  const NativePaidSettlementMetadata metadata{.correlation = "provider_request_id", .microcreditsPerCredit = 1'000'000};
  EXPECT(metadata.surplusBuyerCostMicro == std::nullopt);
  PaidFallbackRequestV3 legacy = admittedRequest();
  legacy.provider = std::nullopt;
  legacy.usage_rollup_at_ms = rt::undefined;
  EXPECT_EQ(rt::jsonStringify(json::value_from(legacy)).find("provider\""), std::string::npos);
}
