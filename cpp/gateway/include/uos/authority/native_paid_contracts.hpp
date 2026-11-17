#pragma once
// Discriminated unions are std::variant over one struct per alternative, named
// <Union><Discriminant>; the discriminant is a static member. The snake_case records
// have the boost::json conversions described in uos/types.hpp.
#include "uos/rt/kv.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::authority::native_paid_contracts {
  using ::uos::types::PaidFallbackProvider;
  using ::uos::types::PaidFallbackRequestV3;
  using ::uos::types::PaidFallbackWindowV3;

  /** Private source snapshots. Revisions and absolute expiry come from trusted storage. */
  template <class T> struct NativePaidRow {
    std::optional<T> value;
    std::optional<std::string> versionstamp;
    rt::Nullable<double> expiresAtMs{};
    friend bool operator==(const NativePaidRow&, const NativePaidRow&) = default;
  };

  // readonly (Uint8Array | string | number | bigint | boolean)[]
  using NativePaidKey = rt::KvKey;

  struct PaidFallbackPendingV3 {
    double created_at_ms;
    double next_reconciliation_at_ms;
    friend bool operator==(const PaidFallbackPendingV3&, const PaidFallbackPendingV3&) = default;
  };

  struct PaidFallbackReconciliationGateV3 {
    std::optional<double> next_due_at_ms;
    friend bool operator==(const PaidFallbackReconciliationGateV3&, const PaidFallbackReconciliationGateV3&) = default;
  };

  struct PaidFallbackReconciliationLeaseV3 {
    std::string token;
    double expires_at_ms;
    friend bool operator==(const PaidFallbackReconciliationLeaseV3&, const PaidFallbackReconciliationLeaseV3&) = default;
  };

  struct PaidFallbackBackfillLeaseV3 {
    std::string token;
    double expires_at_ms;
    friend bool operator==(const PaidFallbackBackfillLeaseV3&, const PaidFallbackBackfillLeaseV3&) = default;
  };

  struct PaidFallbackBackfillStateV3 {
    double v = 1;
    double completed_at_ms;
    friend bool operator==(const PaidFallbackBackfillStateV3&, const PaidFallbackBackfillStateV3&) = default;
  };

  struct PaidFallbackDeletionGuardV3 {
    double created_at_ms;
    friend bool operator==(const PaidFallbackDeletionGuardV3&, const PaidFallbackDeletionGuardV3&) = default;
  };

  struct PaidFallbackAdmissionV3 {
    std::string key_id;
    std::string request_id;
    double created_at_ms;
    double reserved_microcredits;
    double quota_per_credit;
    double window_reset_at_ms;
    std::optional<double> quota_used_percent;
    friend bool operator==(const PaidFallbackAdmissionV3&, const PaidFallbackAdmissionV3&) = default;
  };

  struct PaidFallbackWindowProjectionV3 {
    std::string key_id;
    std::optional<std::string> policy_version;
    double window_reset_at_ms;
    double limit_microcredits;
    double settled_microcredits;
    double reserved_microcredits;
    double pending_count;
    std::optional<double> updated_at_ms;
    friend bool operator==(const PaidFallbackWindowProjectionV3&, const PaidFallbackWindowProjectionV3&) = default;
  };

  struct PaidFallbackOutstandingV3 {
    double pending_requests;
    double unresolved_requests;
    double pending_markers;
    bool has_outstanding;
    friend bool operator==(const PaidFallbackOutstandingV3&, const PaidFallbackOutstandingV3&) = default;
  };

  struct NativePaidAdmissionInput {
    std::string keyId;
    std::string requestId;
    double createdAtMs;
    std::string policyVersion;
    double limitMicrocredits;
    std::optional<double> maximumExposureMicrocredits;
    double initialSettledMicrocredits;
    double quotaPerCredit;
    double windowResetAtMs;
    std::string model;
    std::string route;
    std::string path;
    bool stream;
    std::optional<std::string> reasoning;
    std::optional<bool> dispatchIntent{};
    friend bool operator==(const NativePaidAdmissionInput&, const NativePaidAdmissionInput&) = default;
  };

  // Partial<Pick<PaidFallbackRequestV3, "provider" | "provider_request_id" | "dispatch_state" | "terminal_state">
  // & { reconciliation_attempts?, last_reconciliation_at_ms?, increment_reconciliation_attempts? }>
  struct PaidFallbackRequestLifecyclePatchV3 {
    std::optional<PaidFallbackProvider> provider{};
    rt::Nullable<std::string> provider_request_id{};
    std::optional<std::string> dispatch_state{};
    std::optional<std::string> terminal_state{};
    std::optional<double> reconciliation_attempts{};
    rt::Nullable<double> last_reconciliation_at_ms{};
    std::optional<bool> increment_reconciliation_attempts{};
    friend bool operator==(const PaidFallbackRequestLifecyclePatchV3&, const PaidFallbackRequestLifecyclePatchV3&) = default;
  };

  /** Lifecycle fields one patch resolves to, before they are compared to the row. */
  struct ResolvedRequestLifecycleV3 {
    std::optional<PaidFallbackProvider> provider;  // PaidFallbackProvider | undefined
    std::optional<std::string> providerRequestId;
    std::string dispatchState;  // PaidFallbackRequestV3["dispatch_state"]
    std::string terminalState;  // PaidFallbackRequestV3["terminal_state"]
    double reconciliationAttempts;
    std::optional<double> lastReconciliationAtMs;
    friend bool operator==(const ResolvedRequestLifecycleV3&, const ResolvedRequestLifecycleV3&) = default;
  };

  struct PaidFallbackUsageRollup {
    double v = 1;
    double bucket_start_at_ms;
    std::string model;
    std::string provider;
    double request_count;
    double quota_sum;
    double input_tokens;
    double cached_input_tokens;
    double output_tokens;
    double spend_microcredits;
    double first_request_at_ms;
    double last_request_at_ms;
    double updated_at_ms;
    friend bool operator==(const PaidFallbackUsageRollup&, const PaidFallbackUsageRollup&) = default;
  };

  struct PaidFallbackUsageRollupInput {
    double bucket_start_at_ms;
    std::string model;
    std::string provider;
    std::string request_id;
    double quota;
    double input_tokens;
    std::optional<double> cached_input_tokens;
    double output_tokens;
    double spend_microcredits;
    double request_created_at_ms;
    double updated_at_ms;
    friend bool operator==(const PaidFallbackUsageRollupInput&, const PaidFallbackUsageRollupInput&) = default;
  };

  /**
   * One terminal inference response observed by the gateway. `model` and
   * `provider` are nullable because a rejection that never dispatched an
   * upstream still reaches the terminal log; the writer skips those. Token
   * counts are nullable because an aborted, rejected or unmeasured response
   * never observed usage, and the writer skips an unobserved response instead
   * of recording fabricated zeros.
   */
  struct TerminalUsageRollupInput {
    std::optional<std::string> model;
    std::optional<std::string> provider;
    std::string request_id;
    std::optional<double> request_created_at_ms{};
    std::optional<double> input_tokens;
    std::optional<double> cached_input_tokens;
    std::optional<double> output_tokens;
    friend bool operator==(const TerminalUsageRollupInput&, const TerminalUsageRollupInput&) = default;
  };

  /** Transport-free provider facts; native code owns lookup and reservation identity. */
  struct NativePaidProviderLog {
    std::string request_id;
    double quota;
    double prompt_tokens;
    std::optional<double> cached_prompt_tokens{};
    std::optional<double> cache_write_prompt_tokens{};
    double completion_tokens;
    std::string model;
    double created_at;
    friend bool operator==(const NativePaidProviderLog&, const NativePaidProviderLog&) = default;
  };

  using NativePaidCorrelation = std::string;  // "provider_request_id" | "surplus_reservation"

  struct NativePaidAdmissionSnapshot {
    struct PolicyFence {
      std::optional<std::string> versionstamp;
      friend bool operator==(const PolicyFence&, const PolicyFence&) = default;
    };

    NativePaidRow<PaidFallbackRequestV3> request;
    NativePaidRow<PaidFallbackWindowV3> window;
    NativePaidRow<PaidFallbackDeletionGuardV3> deletionGuard;
    /** Only the authenticated native owner can reconstruct the H row. */
    std::optional<PolicyFence> policyFence{};
    friend bool operator==(const NativePaidAdmissionSnapshot&, const NativePaidAdmissionSnapshot&) = default;
  };

  struct NativePaidAdmissionPolicy {
    bool unlimited;
    std::optional<double> maximumExposure;
    friend bool operator==(const NativePaidAdmissionPolicy&, const NativePaidAdmissionPolicy&) = default;
  };

  struct NativePaidAdmissionPlanBlocked {
    static constexpr std::string_view kind = "blocked";
    std::string reason;  // "invalid_policy" | "limit_exceeded"
    friend bool operator==(const NativePaidAdmissionPlanBlocked&, const NativePaidAdmissionPlanBlocked&) = default;
  };
  struct NativePaidAdmissionPlanReserved {
    static constexpr std::string_view kind = "reserved";
    PaidFallbackAdmissionV3 reservation;
    friend bool operator==(const NativePaidAdmissionPlanReserved&, const NativePaidAdmissionPlanReserved&) = default;
  };
  struct NativePaidAdmissionPlanExhaustedPolicy {
    static constexpr std::string_view kind = "exhausted_policy";
    PaidFallbackWindowV3 window;
    double windowExpireIn;
    std::vector<std::string> checks;  // ("window" | "deletionGuard" | "policy")[]
    friend bool operator==(const NativePaidAdmissionPlanExhaustedPolicy&, const NativePaidAdmissionPlanExhaustedPolicy&) = default;
  };
  struct NativePaidAdmissionPlanWrite {
    static constexpr std::string_view kind = "write";
    PaidFallbackRequestV3 request;
    PaidFallbackPendingV3 pending;
    std::optional<PaidFallbackWindowV3> window;
    PaidFallbackAdmissionV3 reservation;
    bool checkGate;
    bool checkPolicy;
    double requestExpireIn;
    std::optional<double> windowExpireIn;
    std::vector<std::string> checks;  // ("request" | "deletionGuard" | "policy" | "gate" | "window")[]
    friend bool operator==(const NativePaidAdmissionPlanWrite&, const NativePaidAdmissionPlanWrite&) = default;
  };
  using NativePaidAdmissionPlan = std::variant<NativePaidAdmissionPlanBlocked, NativePaidAdmissionPlanReserved, NativePaidAdmissionPlanExhaustedPolicy, NativePaidAdmissionPlanWrite>;

  struct NativePaidLifecyclePlan {
    PaidFallbackRequestV3 request;
    bool readGate;
    bool dispatchBoundary;
    double requestExpireIn;
    std::vector<std::string> checks;  // ("request" | "gate")[]
    friend bool operator==(const NativePaidLifecyclePlan&, const NativePaidLifecyclePlan&) = default;
  };

  struct NativePaidReleaseSnapshot {
    NativePaidRow<PaidFallbackRequestV3> request;
    NativePaidRow<PaidFallbackWindowV3> window;
    NativePaidRow<PaidFallbackReconciliationGateV3> gate;
    friend bool operator==(const NativePaidReleaseSnapshot&, const NativePaidReleaseSnapshot&) = default;
  };

  struct NativePaidReleasePlan {
    PaidFallbackRequestV3 request;
    std::optional<PaidFallbackWindowV3> window;
    PaidFallbackReconciliationGateV3 gate;
    double requestExpireIn;
    std::optional<double> windowExpireIn;
    std::vector<std::string> checks;  // ("request" | "gate" | "window")[]
    friend bool operator==(const NativePaidReleasePlan&, const NativePaidReleasePlan&) = default;
  };

  struct NativePaidDeferralPlanNoop {
    static constexpr std::string_view kind = "noop";
    std::vector<std::string> checks{};  // readonly []
    friend bool operator==(const NativePaidDeferralPlanNoop&, const NativePaidDeferralPlanNoop&) = default;
  };
  struct NativePaidDeferralPlanCleanup {
    static constexpr std::string_view kind = "cleanup";
    std::vector<std::string> checks{"pending", "gate"};  // readonly ["pending", "gate"]
    friend bool operator==(const NativePaidDeferralPlanCleanup&, const NativePaidDeferralPlanCleanup&) = default;
  };
  struct NativePaidDeferralPlanWrite {
    static constexpr std::string_view kind = "write";
    PaidFallbackRequestV3 request;
    PaidFallbackPendingV3 pending;
    double delay;
    double requestExpireIn;
    std::vector<std::string> checks{"request", "pending"};  // readonly ["request", "pending"]
    friend bool operator==(const NativePaidDeferralPlanWrite&, const NativePaidDeferralPlanWrite&) = default;
  };
  using NativePaidDeferralPlan = std::variant<NativePaidDeferralPlanNoop, NativePaidDeferralPlanCleanup, NativePaidDeferralPlanWrite>;

  struct NativePaidRollupIdentity {
    double bucketStartAtMs;
    std::string model;
    std::string provider;
    double shard;
    friend bool operator==(const NativePaidRollupIdentity&, const NativePaidRollupIdentity&) = default;
  };

  struct NativePaidSettlementMetadata {
    NativePaidCorrelation correlation;
    double microcreditsPerCredit;
    std::optional<double> surplusBuyerCostMicro{};
    friend bool operator==(const NativePaidSettlementMetadata&, const NativePaidSettlementMetadata&) = default;
  };

  struct NativePaidSettlementEvaluationCleanup {
    static constexpr std::string_view kind = "cleanup";
    friend bool operator==(const NativePaidSettlementEvaluationCleanup&, const NativePaidSettlementEvaluationCleanup&) = default;
  };
  struct NativePaidSettlementEvaluationNoop {
    static constexpr std::string_view kind = "noop";
    friend bool operator==(const NativePaidSettlementEvaluationNoop&, const NativePaidSettlementEvaluationNoop&) = default;
  };
  struct NativePaidSettlementEvaluationDefer {
    static constexpr std::string_view kind = "defer";
    friend bool operator==(const NativePaidSettlementEvaluationDefer&, const NativePaidSettlementEvaluationDefer&) = default;
  };
  struct NativePaidSettlementEvaluationSettle {
    static constexpr std::string_view kind = "settle";
    double spend;
    friend bool operator==(const NativePaidSettlementEvaluationSettle&, const NativePaidSettlementEvaluationSettle&) = default;
  };
  using NativePaidSettlementEvaluation =
    std::variant<NativePaidSettlementEvaluationCleanup, NativePaidSettlementEvaluationNoop, NativePaidSettlementEvaluationDefer, NativePaidSettlementEvaluationSettle>;

  struct NativePaidSettlementSnapshot {
    NativePaidRow<PaidFallbackRequestV3> request;
    NativePaidRow<PaidFallbackPendingV3> pending;
    NativePaidRow<PaidFallbackReconciliationGateV3> gate;
    NativePaidRow<PaidFallbackWindowV3> window;
    NativePaidRow<PaidFallbackUsageRollup> rollup;
    friend bool operator==(const NativePaidSettlementSnapshot&, const NativePaidSettlementSnapshot&) = default;
  };

  // Omit<NativePaidReleasePlan, "checks"> & { checks }
  struct NativePaidSettlementPlan {
    PaidFallbackRequestV3 request;
    std::optional<PaidFallbackWindowV3> window;
    PaidFallbackReconciliationGateV3 gate;
    double requestExpireIn;
    std::optional<double> windowExpireIn;
    std::vector<std::string> checks;  // ("request" | "pending" | "rollup" | "gate" | "window")[]
    friend bool operator==(const NativePaidSettlementPlan&, const NativePaidSettlementPlan&) = default;
  };

  struct NativePaidTerminalUsage {
    std::string model;
    std::string provider;
    double inputTokens;
    double outputTokens;
    friend bool operator==(const NativePaidTerminalUsage&, const NativePaidTerminalUsage&) = default;
  };

  // ---- JSON conversions of the stored and reported records -------------------------
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackPendingV3& value);
  PaidFallbackPendingV3 tag_invoke(const json::value_to_tag<PaidFallbackPendingV3>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackReconciliationGateV3& value);
  PaidFallbackReconciliationGateV3 tag_invoke(const json::value_to_tag<PaidFallbackReconciliationGateV3>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackReconciliationLeaseV3& value);
  PaidFallbackReconciliationLeaseV3 tag_invoke(const json::value_to_tag<PaidFallbackReconciliationLeaseV3>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackBackfillLeaseV3& value);
  PaidFallbackBackfillLeaseV3 tag_invoke(const json::value_to_tag<PaidFallbackBackfillLeaseV3>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackBackfillStateV3& value);
  PaidFallbackBackfillStateV3 tag_invoke(const json::value_to_tag<PaidFallbackBackfillStateV3>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackDeletionGuardV3& value);
  PaidFallbackDeletionGuardV3 tag_invoke(const json::value_to_tag<PaidFallbackDeletionGuardV3>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackAdmissionV3& value);
  PaidFallbackAdmissionV3 tag_invoke(const json::value_to_tag<PaidFallbackAdmissionV3>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackWindowProjectionV3& value);
  PaidFallbackWindowProjectionV3 tag_invoke(const json::value_to_tag<PaidFallbackWindowProjectionV3>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackOutstandingV3& value);
  PaidFallbackOutstandingV3 tag_invoke(const json::value_to_tag<PaidFallbackOutstandingV3>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackRequestLifecyclePatchV3& value);
  PaidFallbackRequestLifecyclePatchV3 tag_invoke(const json::value_to_tag<PaidFallbackRequestLifecyclePatchV3>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackUsageRollup& value);
  PaidFallbackUsageRollup tag_invoke(const json::value_to_tag<PaidFallbackUsageRollup>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackUsageRollupInput& value);
  PaidFallbackUsageRollupInput tag_invoke(const json::value_to_tag<PaidFallbackUsageRollupInput>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const TerminalUsageRollupInput& value);
  TerminalUsageRollupInput tag_invoke(const json::value_to_tag<TerminalUsageRollupInput>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativePaidProviderLog& value);
  NativePaidProviderLog tag_invoke(const json::value_to_tag<NativePaidProviderLog>&, const json::value& value);
}
