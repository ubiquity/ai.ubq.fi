#pragma once
// Port of src/codex/banked-reset.ts.
// Shared types, ledger keys, record parsers, policy and telemetry helpers of the banked Codex reset.
//
// Declaration order follows the TS except where C++ needs a type first: CodexBankedResetFence and
// CodexBankedResetCandidate precede readUsageGate, whose parameter names the candidate's nested type.
#include "uos/codex/banked_reset_provider.hpp"
#include "uos/codex/routing_mutations.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::codex::banked_reset {
  /**
   * A banked reset is an externally visible mutation. Its ledger deliberately
   * does not share the routing key: a routing update must never erase a record
   * describing a provider-side redemption that may still need reconciliation.
   */
  inline const rt::KvKey CODEX_RESET_REDEMPTION_KV_PREFIX{"uos_ai", "codex_reset_redemption", "v1"};
  /** Retained for rollback and audit only: the submission budget never reads this key after the per-account cutover. */
  inline const rt::KvKey CODEX_RESET_GLOBAL_DAILY_KV_PREFIX{"uos_ai", "codex_reset_redemption", "global_day", "v1"};
  inline const rt::KvKey CODEX_RESET_ACCOUNT_DAILY_KV_PREFIX{"uos_ai", "codex_reset_redemption", "account_day", "v1"};
  inline const rt::KvKey CODEX_RESET_SHADOW_DECISION_KV_PREFIX{"uos_ai", "codex_reset_shadow_decision", "v1"};
  inline constexpr double CODEX_BANKED_RESET_LEASE_MS = 30'000;
  /** Inventory is an authorization input for an external spend, not a cache. */
  inline constexpr double CODEX_BANKED_RESET_INVENTORY_MAX_AGE_MS = 30'000;
  /** A reset preflight may not hold an otherwise healthy fallback indefinitely. */
  inline constexpr double CODEX_BANKED_RESET_INVENTORY_TIMEOUT_MS = 5'000;
  inline constexpr double MAX_CAS_ATTEMPTS = 4;

  rt::KvKey codexResetRedemptionKey(const std::string& accountIdHash, const std::string& quotaGeneration);

  /** Retained for rollback and audit only; never enforced after the per-account cutover. */
  rt::KvKey codexResetGlobalDailyKey(const std::string& day);

  rt::KvKey codexResetAccountDailyKey(const std::string& accountIdHash, const std::string& day);

  rt::KvKey codexResetShadowDecisionKey(const std::string& episodeHash);

  using CodexBankedResetMode = std::string;  // "disabled" | "shadow" | "live"

  struct CodexBankedResetConfig {
    bool enabled;
    CodexBankedResetMode mode;
    double maxPerAccountPerDay;
    double maxPerAccountPerWindow;
    friend bool operator==(const CodexBankedResetConfig&, const CodexBankedResetConfig&) = default;
  };

  /**
   * Preserve the established live policy: one submission per account per UTC
   * day and quota window. Fresh per-account KV consent gates every submission.
   */
  CodexBankedResetConfig loadCodexBankedResetConfig();

  // "codex_reset_eligible" | "codex_reset_skipped_healthy_fallback" | "codex_reset_claimed" |
  // "codex_reset_submit_started" | "codex_reset_submitted" | "codex_reset_unknown" | "codex_reset_rejected" |
  // "codex_reset_verified" | "codex_reset_inference_retry" | "codex_reset_inference_retry_result" |
  // "codex_reset_duplicate_prevented" | "codex_reset_shadow_candidate"
  using CodexBankedResetEvent = std::string;

  // "codex_reset_eligible_total" | "codex_reset_shadow_candidates_total" | "codex_reset_submission_attempts_total" |
  // "codex_reset_verified_total" | "codex_reset_unknown_total" | "codex_reset_duplicate_prevented_total" |
  // "codex_reset_verification_latency_ms" | "codex_reset_post_retry_total" | "codex_reset_estimated_spend_total"
  using CodexBankedResetMetric = std::string;

  // `Readonly<Record<string, string | number | boolean | null>>`: a JSON object in insertion order whose
  // members are strings, numbers, booleans or null.
  using CodexBankedResetTelemetryFields = json::object;

  // `Readonly<{ event?: (event, fields) => void; metric?: (metric, value, fields) => void }>`; an empty
  // std::function is an absent member.
  struct CodexBankedResetTelemetry {
    std::function<void(CodexBankedResetEvent eventName, CodexBankedResetTelemetryFields fields)> event{};
    std::function<void(CodexBankedResetMetric metricName, double value, CodexBankedResetTelemetryFields fields)> metric{};
  };

  // `const defaultTelemetry: CodexBankedResetTelemetry` (exported through the list): the console.info
  // sink. Defined in src/codex/banked_reset.cpp; read only after main starts.
  extern const CodexBankedResetTelemetry defaultTelemetry;

  // `Readonly<{ key: KvKey; isCurrent: (value: unknown) => boolean }>`: the same shape as the
  // routing-mutations reconciliation fence, which production passes for it.
  using CodexBankedResetFence = routing_mutations::CodexQuotaResetReconciliationFence;

  struct CodexBankedResetCandidate {
    // `Readonly<{ kind: "manual_admin"; accountIdHash: string }>`
    struct Authorization {
      static constexpr std::string_view kind = "manual_admin";
      std::string accountIdHash;
      friend bool operator==(const Authorization&, const Authorization&) = default;
    };

    /** Raw account id is passed only to the provider; it is never persisted or logged. */
    std::string accountId;
    /** Server-created authorization for one authenticated manual action; never read from inference input. */
    std::optional<Authorization> authorization{};
    /**
     * An opaque routing credential fence. It is hashed before it reaches a
     * durable record, provider context, or telemetry sink.
     */
    std::string credentialVersion;
    double quotaResetAtMs;
    double routingGeneration;
    /**
     * Each fence is strongly read and atomically checked at both the initial
     * claim and the durable `claimed -> submitted` side-effect boundary.
     * Production passes routing and auth-pool fences; fakes use the same KV
     * shape without exposing a privileged bypass.
     */
    std::vector<CodexBankedResetFence> fences;
    std::optional<std::string> requestId;
    /**
     * A fresh, account-bound credit selected by the pool evaluator. It remains
     * in memory only; durable records retain its hash at most.
     */
    std::optional<banked_reset_provider::ResetInventoryCredit> selectedCredit{};
    std::optional<rt::AbortSignal> signal{};
  };

  // readUsageGate's result: `Readonly<{ kind: "allowed"; entries: readonly KvEntryMaybe<unknown>[] }> |
  // Readonly<{ kind: "failure"; code: string }>`.
  struct ReadUsageGateResultAllowed {
    static constexpr std::string_view kind = "allowed";
    std::vector<rt::KvEntryMaybe<>> entries;
  };
  struct ReadUsageGateResultFailure {
    static constexpr std::string_view kind = "failure";
    std::string code;
  };
  using ReadUsageGateResult = std::variant<ReadUsageGateResultAllowed, ReadUsageGateResultFailure>;

  /**
   * The usage gate that every path writing a durable submission passes through.
   * An unavailable configuration and a disabled subscription both stop the
   * caller. Reset settings are owned by the account, never by an API key.
   */
  // `authorization?: CodexBankedResetCandidate["authorization"]`: the overload without it, or std::nullopt,
  // is undefined.
  [[nodiscard]] rt::Task<ReadUsageGateResult> readUsageGate(std::shared_ptr<rt::Kv> kv, std::string accountIdHash);
  [[nodiscard]] rt::Task<ReadUsageGateResult> readUsageGate(std::shared_ptr<rt::Kv> kv, std::string accountIdHash,
                                                            std::optional<CodexBankedResetCandidate::Authorization> authorization);

  struct CodexBankedResetDependencies {
    CodexBankedResetConfig config;
    /** Re-read before a new submission so an operator kill switch wins mid-request. */
    std::function<CodexBankedResetConfig()> reloadConfig{};
    std::shared_ptr<banked_reset_provider::CodexUsageResetProvider> provider;
    /** `undefined` uses the production KV accessor; explicit null fails closed. */
    // std::nullopt is undefined; a null pointer is null.
    std::optional<std::shared_ptr<rt::Kv>> kv{};
    std::function<double()> now{};
    std::function<std::string()> newOwnerToken{};
    std::function<rt::Task<std::string>(std::string value)> hash{};
    std::optional<CodexBankedResetTelemetry> telemetry{};
  };

  struct CodexBankedResetOutcome {
    std::string kind;  // "verified" | "pending" | "rejected" | "skipped"
    std::string reason;
    std::optional<std::string> accountIdHash;
    std::optional<std::string> quotaGeneration;
    std::optional<std::string> idempotencyKeyHash;
    std::optional<::uos::types::CodexResetRedemptionRecord> record;
  };

  /** One currently fenced account supplied to the blocked-cohort evaluator. */
  struct CodexBankedResetPoolCandidate {
    double slot;
    CodexBankedResetCandidate candidate;
    std::shared_ptr<banked_reset_provider::CodexUsageResetProvider> provider;
  };

  /**
   * The evaluator returns the in-memory selected account only after the shadow
   * evidence or live reset path has succeeded. No raw account or credit value
   * is written by this return value.
   */
  struct CodexBankedResetPoolOutcome {
    std::string kind;  // "shadow" | "verified" | "skipped" | "pending" | "rejected"
    std::string reason;
    std::optional<CodexBankedResetPoolCandidate> selected;
    std::optional<CodexBankedResetOutcome> reset;
    bool allCandidatesExplicitlyDisabled = false;
  };

  // `type ResetContext` (exported through the type list).
  struct ResetContext {
    banked_reset_provider::ResetAccountContext account;
    std::string idempotencyKey;
    std::string idempotencyKeyHash;
    friend bool operator==(const ResetContext&, const ResetContext&) = default;
  };

  // `(value: unknown): value is number`: a non-negative safe integer. nullptr is undefined.
  bool isSafeMs(const json::value& value);
  bool isSafeMs(const json::value* value);
  bool isSafeMs(double value);

  // `(value: unknown): value is number`. nullptr is undefined.
  bool isSafeNonnegativeInteger(const json::value& value);
  bool isSafeNonnegativeInteger(const json::value* value);
  bool isSafeNonnegativeInteger(double value);

  // `(value: unknown, max = 512): value is string`: lengths in UTF-16 code units. nullptr is undefined;
  // the std::string_view overload is the typed call, and the array overload takes a string literal, which
  // would otherwise be ambiguous between the json::value and std::string_view overloads.
  bool isNonEmptyText(const json::value& value, double max = 512);
  bool isNonEmptyText(const json::value* value, double max = 512);
  bool isNonEmptyText(std::string_view value, double max = 512);
  template <std::size_t N>
  bool isNonEmptyText(const char (&value)[N], double max = 512) {
    return isNonEmptyText(std::string_view(value, N - 1), max);
  }

  // `value: unknown` (a KV row: convert it with toJson(), whose TypeError the callers' try covers):
  // nullptr is undefined.
  std::optional<::uos::types::CodexResetRedemptionRecord> parseCodexResetRedemptionRecord(const json::value& value);
  std::optional<::uos::types::CodexResetRedemptionRecord> parseCodexResetRedemptionRecord(const json::value* value);

  std::optional<::uos::types::CodexResetGlobalDailyRecord> parseAccountDayRecord(const json::value& value, const std::string& day);
  std::optional<::uos::types::CodexResetGlobalDailyRecord> parseAccountDayRecord(const json::value* value, const std::string& day);

  /** Parse only redacted, safe-to-return shadow decision evidence. */
  std::optional<::uos::types::CodexResetShadowDecisionRecord> parseCodexResetShadowDecisionRecord(const json::value& value);
  std::optional<::uos::types::CodexResetShadowDecisionRecord> parseCodexResetShadowDecisionRecord(const json::value* value);

  // `string | null`: the ISO date of `nowMs`.
  std::optional<std::string> utcDay(double nowMs);

  /**
   * The daily cap is a cap on externally visible submissions, not merely on
   * ledger claims. A claim that crosses UTC midnight must never carry an old
   * day's reservation into a new day, where it could bypass that day's cap.
   */
  bool claimedDuringCurrentUtcDay(const ::uos::types::CodexResetRedemptionRecord& record, double nowMs);

  // `kind: CodexBankedResetOutcome["kind"]`; `context` and `record` default to null.
  CodexBankedResetOutcome outcome(const std::string& kind, const std::string& reason, const std::optional<ResetContext>& context = std::nullopt,
                                  const std::optional<::uos::types::CodexResetRedemptionRecord>& record = std::nullopt);

  // `string | null`
  std::optional<std::string> safeOwnerToken(const std::function<std::string()>& source);

  // `string | null`
  std::optional<std::string> policyReason(const CodexBankedResetConfig& config, bool manual = false);

  /**
   * Live mode is exactly one redemption per account per UTC day. A provider whose
   * redeem outcome is final gets the terminal-specific message because a lost
   * response can never be reconciled; every other live provider still fails
   * closed instead of handing out a second daily submission.
   */
  // `provider` as the provider* checks take it (nullptr is an absent receiver).
  std::optional<std::string> providerPolicyReason(const CodexBankedResetConfig& config, const banked_reset_provider::CodexUsageResetProvider* provider,
                                                  bool manual = false);

  // `signal?: AbortSignal`
  rt::AbortSignal boundedInventorySignal(const std::optional<rt::AbortSignal>& signal = std::nullopt);

  // `extras = {}` is the overload without it.
  CodexBankedResetTelemetryFields telemetryFields(const ResetContext& context, const CodexBankedResetCandidate& candidate);
  CodexBankedResetTelemetryFields telemetryFields(const ResetContext& context, const CodexBankedResetCandidate& candidate,
                                                  const CodexBankedResetTelemetryFields& extras);

  void emit(const CodexBankedResetTelemetry& telemetry, const CodexBankedResetEvent& event, const CodexBankedResetTelemetryFields& fields);

  void metric(const CodexBankedResetTelemetry& telemetry, const CodexBankedResetMetric& name, double value, const CodexBankedResetTelemetryFields& fields);

  /** Allows gateway-level events and metrics to share the same injectable sink. */
  // `telemetry: CodexBankedResetTelemetry | undefined`
  void reportCodexBankedResetEvent(const std::optional<CodexBankedResetTelemetry>& telemetry, const CodexBankedResetEvent& event,
                                   const CodexBankedResetTelemetryFields& fields);

  /** Allows gateway-level post-retry metrics to share the default sink. */
  void reportCodexBankedResetMetric(const std::optional<CodexBankedResetTelemetry>& telemetry, const CodexBankedResetMetric& name, double value,
                                    const CodexBankedResetTelemetryFields& fields);
}
