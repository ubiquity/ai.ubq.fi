#pragma once
// Port of src/authority/native-codex-contracts.ts.
// Source-owned Codex routing DTOs and row codecs. No credentials or native state live here.
#include "uos/rt/kv.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::authority::native_codex_contracts {
  /**
   * Durable, slot-indexed routing state. It intentionally contains neither raw
   * account ids nor credentials; an opaque account-scope hash keeps a circuit
   * with the same account through a pool reorder without letting a replacement
   * account inherit it.
   */
  inline const rt::KvKey CODEX_ACCOUNT_ROUTING_KV_KEY{"uos_ai", "codex_account_routing", "v2"};
  /**
   * One opaque subscription selection shared by every ordinary Codex request.
   * It is separate from slot routing so an older quota/reset row stays intact
   * while the active account advances through the configured pool.
   */
  inline const rt::KvKey CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY{"uos_ai", "codex_active_account", "v1"};
  // Keep this local to avoid a runtime import cycle with codex.ts. The public
  // transport exports the same stable key for admin and test callers.
  inline const rt::KvKey CODEX_AUTH_POOL_KV_KEY{"ubq_ai", "codex_auth"};
  inline const rt::KvKey CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY{"uos_ai", "codex_capacity_routing", "v1", "observations"};
  inline constexpr double CODEX_HALF_OPEN_LEASE_MS = 30'000;
  /** Keep new requests away from an account after an ambiguous response-header timeout. */
  inline constexpr double CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS = 60'000;
  /** Capacity observations older than this cannot reopen a quota circuit. */
  inline constexpr double CODEX_CAPACITY_ROUTING_MAX_AGE_MS = 30 * 60'000;
  // A warm isolate avoids per-request routing reads, but it must eventually
  // observe circuits opened by another isolate. This bounded revalidation keeps
  // normal traffic off KV while limiting cross-isolate stale routing decisions.
  inline constexpr double ROUTING_CACHE_REVALIDATE_MS = 5'000;

  using CodexQuotaBlockSource = std::string;  // "body_resets_at" | "header_retry_after"
  using CodexProbeCircuit = std::string;      // "quota" | "upstream_timeout"
  using CodexQuotaClass = std::string;        // "spark" | "gpt_oss_120b" | "reserve" | "standard" | "unknown"
  // "quota_exhausted" | "credential_invalid" | "account_removed_or_replaced" | "model_unavailable" | "manual_reset"
  using CodexActiveAccountTransitionReason = std::string;

  /**
   * Durable active-account admission fence. Account identifiers stay hashed and
   * a pool reorder is resolved through that identity rather than the slot alone.
   */
  struct CodexActiveAccountSelection {
    double v = 1;
    std::string account_id_hash = {};
    std::string credential_version = {};
    /** Versionstamp of the auth-pool snapshot that admitted this identity. */
    std::string pool_versionstamp = {};
    double slot = 0;
    double routing_generation = 0;
    double generation = 0;
    /** Null denotes the initial deterministic bootstrap, not a transition. */
    std::optional<CodexActiveAccountTransitionReason> transition_reason = std::nullopt;
    double updated_at_ms = 0;
    friend bool operator==(const CodexActiveAccountSelection&, const CodexActiveAccountSelection&) = default;
  };

  /**
   * The active-selection row exactly as one strong read observed it. A banked
   * reset binds its inventory and consume fences to this snapshot --
   * including a validly absent row -- and only elects its verified account after
   * proving the snapshot was not superseded.
   */
  struct CodexActiveAccountSnapshot {
    std::optional<CodexActiveAccountSelection> selection = std::nullopt;
  };

  struct CodexQuotaClassBlock {
    double blocked_until_ms = 0;
    CodexQuotaBlockSource source = {};
    /** True when this entry is the synthetic fallback copied from legacy slot state. */
    bool legacy_fallback = false;
    std::optional<double> quota_signal_observed_at_ms = std::nullopt;
    std::optional<double> observed_reset_at_ms = std::nullopt;
    bool observed_reset_at_is_stable = false;
    bool banked_reset_generation_ambiguous = false;
    bool banked_reset_recovery_probe_pending = false;
    friend bool operator==(const CodexQuotaClassBlock&, const CodexQuotaClassBlock&) = default;
  };

  // `Partial<Record<CodexQuotaClass, CodexQuotaClassBlock>>`: an object whose members keep their
  // insertion order (class names are never array indexes).
  using CodexQuotaBlocksByClass = rt::OrderedMap<CodexQuotaClass, CodexQuotaClassBlock>;

  // `NonNullable<CodexRoutingSlot["probe_lease"]>`, and `CodexProbeLease` (whose quota_class is never
  // undefined). The circuit whose half-open request owns this lease. Missing on legacy records and
  // normalized to quota.
  struct CodexProbeLease {
    std::string token = {};
    double expires_at_ms = 0;
    double generation = 0;
    CodexProbeCircuit circuit = {};
    rt::Nullable<CodexQuotaClass> quota_class = rt::undefined;
    friend bool operator==(const CodexProbeLease&, const CodexProbeLease&) = default;
  };

  struct CodexRoutingSlot {
    /** Opaque account-scope hash; durable state never stores a raw account id. */
    std::optional<std::string> account_id_hash = std::nullopt;
    std::string credential_version = {};
    std::optional<double> quota_blocked_until_ms = std::nullopt;
    std::optional<CodexQuotaBlockSource> quota_block_source = std::nullopt;
    /** Separately metered model classes exhausted during the current quota circuit. */
    std::optional<std::vector<std::string>> quota_blocked_classes = std::nullopt;
    /** Independent deadlines for each separately metered model class. */
    std::optional<CodexQuotaBlocksByClass> quota_blocks_by_class = std::nullopt;
    std::optional<std::string> invalid_credential_version = std::nullopt;
    std::optional<double> primary_used_percent = std::nullopt;
    std::optional<double> secondary_used_percent = std::nullopt;
    /** Timestamp of the newest provider quota signal written by inference. */
    std::optional<double> quota_signal_observed_at_ms = std::nullopt;
    /** Timestamp of the newest capacity sampler observation applied to this slot. */
    std::optional<double> capacity_observed_at_ms = std::nullopt;
    /** A transport timeout circuit that must not be confused with a quota reset. */
    rt::Nullable<double> upstream_timeout_blocked_until_ms = rt::undefined;
    std::optional<double> observed_reset_at_ms = std::nullopt;
    /** True only when the upstream supplied a canonical absolute reset deadline. */
    bool observed_reset_at_is_stable = false;
    /**
     * A stable reset deadline plus any later conflicting or relative value cannot
     * be proved to name a new provider quota generation. While it remains true,
     * ordinary routing continues but banked-reset claims fail closed and
     * existing records are lookup-only.
     */
    bool banked_reset_generation_ambiguous = false;
    /** A verified banked reset is waiting for its bounded post-reset probe. */
    bool banked_reset_recovery_probe_pending = false;
    double generation = 0;
    /** The circuit whose half-open request owns this lease. Missing on legacy records and normalized to quota. */
    std::optional<CodexProbeLease> probe_lease = std::nullopt;
  };

  struct CodexAccountRoutingState {
    double v = 2;
    double updated_at_ms = 0;
    /** A pre-account-hash stable identity could not be matched to a current account. */
    bool banked_reset_legacy_identity_unresolved = false;
    std::vector<CodexRoutingSlot> slots = {};
  };

  struct CodexRoutingAccount {
    double slot = 0;
    /** Opaque account-scope hash used only to keep durable slots attached to an account across reordering. */
    std::string accountIdHash = {};
    std::string credentialVersion = {};
    std::optional<double> quotaHeadroom = std::nullopt;
    bool probeRequired = false;
    std::optional<double> probeGeneration = std::nullopt;
    std::optional<std::string> probeToken = std::nullopt;
    /** Circuit selected for a pending half-open probe. */
    rt::Nullable<CodexProbeCircuit> probeCircuit = rt::undefined;
    /**
     * Set only for the one bounded overage attempt that follows a blocked cohort
     * proving no redeemable credit. The probe-claim allowance honors it for that
     * attempt alone; it is never persisted.
     */
    std::optional<bool> overageFallback = std::nullopt;
    /** Durable routing generation observed when this account was selected. */
    std::optional<double> routingGeneration = std::nullopt;
    /** Global active-account generation that admitted this request. */
    std::optional<double> activeGeneration = std::nullopt;
    /** Auth-pool versionstamp bound to the active admission. */
    std::optional<std::string> activePoolVersionstamp = std::nullopt;
    /** Last durable reason that moved the active selection. */
    rt::Nullable<CodexActiveAccountTransitionReason> activeTransitionReason = rt::undefined;
    /** Exact incoming model id used to keep separately metered pools independent. */
    rt::Nullable<std::string> requestedModel = rt::undefined;
  };

  struct CodexCapacityRoutingWindow {
    std::optional<double> limit_window_seconds = std::nullopt;
    std::optional<double> used_percent = std::nullopt;
    std::optional<double> reset_at_ms = std::nullopt;
  };

  // `Readonly<{ primary: CodexCapacityRoutingWindow | null; secondary: CodexCapacityRoutingWindow | null }>`
  struct CodexCapacityRoutingWindows {
    std::optional<CodexCapacityRoutingWindow> primary = std::nullopt;
    std::optional<CodexCapacityRoutingWindow> secondary = std::nullopt;
  };

  struct CodexCapacityRoutingAdditionalRateLimit {
    std::string limit_name = {};
    std::optional<std::string> metered_feature = std::nullopt;
    CodexCapacityRoutingWindows windows = {};
  };

  /**
   * Control-plane input only. `account_id` is hashed before it enters durable
   * routing state; the dashboard and observation record never contain it.
   * `slot` is zero-based, matching the routing state slot index.
   */
  struct CodexCapacityRoutingObservationInput {
    double slot = 0;
    std::string account_id = {};
    std::string state = {};  // "available" | "stale" | "unavailable"
    std::optional<double> source_observed_at_ms = std::nullopt;
    double snapshot_at_ms = 0;
    CodexCapacityRoutingWindows windows = {};
    std::vector<CodexCapacityRoutingAdditionalRateLimit> additional_rate_limits = {};
  };

  struct CodexCapacityRoutingObservation {
    double slot = 0;
    std::string account_id_hash = {};
    std::string state = {};  // "available" | "stale" | "unavailable"
    std::optional<double> source_observed_at_ms = std::nullopt;
    double snapshot_at_ms = 0;
    CodexCapacityRoutingWindows windows = {};
    std::vector<CodexCapacityRoutingAdditionalRateLimit> additional_rate_limits = {};
  };

  /** A durable provider-deadline circuit that may have an existing reset record to reconcile. */
  // `CodexRoutingAccount & { quotaResetAtMs: number; routingGeneration: number }`: routingGeneration is
  // the base member, always set on these values.
  struct CodexBlockedAccountMetadata : CodexRoutingAccount {
    double quotaResetAtMs = 0;
  };

  // The members of the CodexRouteSelection union, by `kind`.
  struct CodexRouteSelectionEligible {
    static constexpr std::string_view kind = "eligible";
    std::vector<CodexRoutingAccount> accounts = {};
    std::vector<double> skippedSlots = {};
    /** Stable absolute quota fences omitted from ordinary eligible routing. */
    std::vector<CodexBlockedAccountMetadata> blockedAccounts = {};
  };
  struct CodexRouteSelectionQuotaBlocked {
    static constexpr std::string_view kind = "quota_blocked";
    std::vector<double> skippedSlots = {};
    std::optional<double> retryAtMs = std::nullopt;
    /** Stable absolute quota fences omitted from ordinary eligible routing. */
    std::vector<CodexBlockedAccountMetadata> blockedAccounts = {};
    /**
     * Every current pool account proved authoritative applicable quota or
     * capacity exhaustion. Credential failures never authorize quota resets.
     */
    bool fullCohortExhausted = false;
    /** Every current subscription is quota-exhausted or has invalid current credentials. */
    std::optional<bool> fullCohortUnavailable = std::nullopt;
    /** The active-selection row exactly as this strong read observed it. */
    CodexActiveAccountSnapshot activeSnapshot = {};
    /**
     * Canonical strong-read snapshots for pre-election reset fences. Ordered
     * pool membership, credential material, pool update identity, and the
     * stored capacity observations must all stay exactly as observed through
     * inventory and consume.
     */
    std::optional<std::string> poolSnapshotJson = std::nullopt;
    std::optional<std::string> capacitySnapshotJson = std::nullopt;
  };
  struct CodexRouteSelectionUpstreamBlocked {
    static constexpr std::string_view kind = "upstream_blocked";
    std::vector<double> skippedSlots = {};
    std::optional<double> retryAtMs = std::nullopt;
    std::vector<CodexBlockedAccountMetadata> blockedAccounts = {};  // always empty
  };
  /**
   * No configured account can serve the named model. This is neither a
   * quota nor a credential outcome: no fence was written, so the request is
   * answered as an unavailable model rather than a retryable failure.
   */
  struct CodexRouteSelectionModelUnavailable {
    static constexpr std::string_view kind = "model_unavailable";
    std::string model = {};
    /** Upstream's own detail text when a live rejection was observed. */
    std::optional<std::string> detail = std::nullopt;
    std::vector<double> skippedSlots = {};
  };
  struct CodexRouteSelectionCredentialsInvalid {
    static constexpr std::string_view kind = "credentials_invalid";
    std::vector<double> skippedSlots = {};
    std::optional<bool> fullCohortUnavailable = std::nullopt;
  };
  struct CodexRouteSelectionRoutingUnavailable {
    static constexpr std::string_view kind = "routing_unavailable";
  };
  using CodexRouteSelection = std::variant<CodexRouteSelectionEligible, CodexRouteSelectionQuotaBlocked, CodexRouteSelectionUpstreamBlocked,
                                           CodexRouteSelectionModelUnavailable, CodexRouteSelectionCredentialsInvalid, CodexRouteSelectionRoutingUnavailable>;

  // `value: unknown` (nullptr is undefined), or a number.
  bool isSafeMs(const json::value* value);
  bool isSafeMs(const json::value& value);
  bool isSafeMs(double value);
  bool isSafeMs(std::optional<double> value);

  bool isLegacyStableResetIdentity(const CodexRoutingSlot& slot);

  // nullptr is null.
  bool hasUnresolvedLegacyResetIdentity(const CodexAccountRoutingState* state);

  /** `null`, `undefined` and every non-canonical value mean "no absolute deadline was persisted". */
  std::optional<double> parseOptionalSafeMs(const json::value* value);
  std::optional<double> parseOptionalSafeMs(std::optional<double> value);

  std::optional<CodexAccountRoutingState> parseCodexAccountRoutingState(const json::value* value);
  std::optional<CodexAccountRoutingState> parseCodexAccountRoutingState(const json::value& value);

  bool isVersionstamp(const json::value* value);

  /** Invalid active rows fail closed; only an absent row is a bootstrap case. */
  std::optional<CodexActiveAccountSelection> parseCodexActiveAccountSelection(const json::value* value);
  std::optional<CodexActiveAccountSelection> parseCodexActiveAccountSelection(const json::value& value);

  /** Exact active-generation fence used by reset recovery and final dispatch. */

  /** Trusted metadata resolved by the native owner; credential fingerprints remain opaque. */
  struct CodexRoutingIdentity {
    double slot = 0;
    std::string accountIdHash = {};
    std::string credentialVersion = {};
  };
  // `Readonly<{ detail: string | null }>`
  struct CodexRoutingAccountModelUnavailable {
    std::optional<std::string> detail = std::nullopt;
  };
  struct CodexRoutingAccountPolicy {
    std::optional<CodexRoutingAccountModelUnavailable> modelUnavailable = std::nullopt;
    bool overageUsageAllowed = false;
  };
  struct CodexRoutingRowRevisions {
    std::optional<std::string> active = std::nullopt;
    std::optional<std::string> pool = std::nullopt;
    std::optional<std::string> routing = std::nullopt;
    std::optional<std::string> capacity = std::nullopt;
  };
  // `Readonly<{ routing: CodexAccountRoutingState | null; active: CodexActiveAccountSelection | null }>`
  struct CodexSerialAdmissionWrites {
    std::optional<CodexAccountRoutingState> routing = std::nullopt;
    std::optional<CodexActiveAccountSelection> active = std::nullopt;
  };
  /** The closed four-row native admission plan. It never accepts caller keys or arbitrary writes. */
  struct CodexSerialAdmissionPlan {
    static constexpr std::string_view kind = "serial_admission";
    CodexRoutingRowRevisions checks = {};
    CodexSerialAdmissionWrites writes = {};
    CodexRouteSelection selection = CodexRouteSelectionRoutingUnavailable{};
  };

  // `value: unknown`: nullptr is undefined.
  bool codexActiveAccountSelectionCurrent(const json::value* value, const CodexRoutingAccount& account, double activeGeneration);
  bool codexActiveAccountSelectionCurrent(const json::value& value, const CodexRoutingAccount& account, double activeGeneration);

  // The rows as JSON (what the TS stores and JSON.stringify prints): members in declaration order,
  // which is the order every constructor in these modules writes; undefined members are omitted.
  json::object toJson(const CodexQuotaClassBlock& block);
  json::object toJson(const CodexQuotaBlocksByClass& blocks);
  json::object toJson(const CodexProbeLease& lease);
  json::object toJson(const CodexRoutingSlot& slot);
  json::object toJson(const CodexAccountRoutingState& state);
  json::object toJson(const CodexActiveAccountSelection& selection);
  json::object toJson(const CodexCapacityRoutingWindow& window);
  json::object toJson(const CodexCapacityRoutingWindows& windows);
  json::object toJson(const CodexCapacityRoutingAdditionalRateLimit& limit);
  json::object toJson(const CodexCapacityRoutingObservation& observation);
}
