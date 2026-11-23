#pragma once
// Port of src/authority/native-kernel.ts.
//
// The normalizers read `unknown` row values as json::value (KernelQuotaRead's value). They return
// the contract structs: a stored row's undeclared members (and an org row's `repo: null`, which
// `repo?: string` cannot hold) do not survive, where the TS reservation and cutover normalizers
// return the stored object itself.
#include "uos/authority/native_kernel_contracts.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace uos::authority::native_kernel {
  using native_kernel_contracts::KernelDefaultWindowCutoverV2;
  using native_kernel_contracts::KernelQuotaConstants;
  using native_kernel_contracts::KernelQuotaCutoverPlan;
  using native_kernel_contracts::KernelQuotaDefaults;
  using native_kernel_contracts::KernelQuotaEffectivePolicy;
  using native_kernel_contracts::KernelQuotaEffectiveWindow;
  using native_kernel_contracts::KernelQuotaEligibilityPlan;
  using native_kernel_contracts::KernelQuotaEligibilitySnapshot;
  using native_kernel_contracts::KernelQuotaIdentity;
  using native_kernel_contracts::KernelQuotaPolicySetOptions;
  using native_kernel_contracts::KernelQuotaPolicyV2;
  using native_kernel_contracts::KernelQuotaPolicyWritePlan;
  using native_kernel_contracts::KernelQuotaRead;
  using native_kernel_contracts::KernelQuotaReservationIdentity;
  using native_kernel_contracts::KernelQuotaReservationRowV2;
  using native_kernel_contracts::KernelQuotaReservePlan;
  using native_kernel_contracts::KernelQuotaScope;
  using native_kernel_contracts::KernelQuotaSettlePlan;
  using native_kernel_contracts::KernelQuotaTerminalPlan;
  using native_kernel_contracts::KernelQuotaWindowV2;
  using native_kernel_contracts::KernelReservationSettlement;

  inline const rt::KvKey KERNEL_QUOTA_V2_PREFIX{"uos_ai", "kernel_quota", "v2"};
  inline const rt::KvKey KERNEL_REPO_POLICY_V2_PREFIX{KERNEL_QUOTA_V2_PREFIX, "repo_policy"};
  inline const rt::KvKey KERNEL_ORG_POLICY_V2_PREFIX{KERNEL_QUOTA_V2_PREFIX, "org_policy"};
  inline const rt::KvKey KERNEL_REPO_WINDOW_V2_PREFIX{KERNEL_QUOTA_V2_PREFIX, "repo_window"};
  inline const rt::KvKey KERNEL_ORG_WINDOW_V2_PREFIX{KERNEL_QUOTA_V2_PREFIX, "org_window"};
  inline const rt::KvKey KERNEL_REPO_RESERVATION_V2_PREFIX{KERNEL_QUOTA_V2_PREFIX, "repo_reservation"};
  inline const rt::KvKey KERNEL_ORG_RESERVATION_V2_PREFIX{KERNEL_QUOTA_V2_PREFIX, "org_reservation"};
  inline const rt::KvKey KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY{KERNEL_QUOTA_V2_PREFIX, "default_window_cutover"};
  inline constexpr double KERNEL_QUOTA_RESERVATION_LEASE_MS = 5 * 60'000;
  inline constexpr double KERNEL_QUOTA_RESERVATION_RENEWAL_MS = 60'000;
  inline constexpr double KERNEL_QUOTA_RESERVATION_RETENTION_MS = 7 * 24 * 60 * 60'000.0;
  inline constexpr double KERNEL_DEFAULT_WINDOW_CUTOVER_LEASE_MS = 5 * 60'000;
  inline constexpr double KERNEL_QUOTA_SETTLEMENT_RETRY_MS = 1'000;

  inline constexpr double MAX_KV_RETRIES = 3;

  rt::KvKey kernelRepoPolicyKey(const std::string& owner, const std::string& repo);
  rt::KvKey kernelOrgPolicyKey(const std::string& owner);
  rt::KvKey kernelRepoWindowKey(const std::string& owner, const std::string& repo);
  rt::KvKey kernelOrgWindowKey(const std::string& owner);
  rt::KvKey kernelRepoReservationKey(const std::string& owner, const std::string& repo, double windowCreatedAtMs, const std::string& requestId);
  rt::KvKey kernelOrgReservationKey(const std::string& owner, double windowCreatedAtMs, const std::string& requestId);

  /**
   * Repo-scope keys are only ever built once a repo is known to be present. The
   * invariant is asserted here so a missing repo fails loudly instead of reaching
   * Deno KV as an `undefined` key part.
   */
  // `repo: string | undefined`
  std::string kernelRepoPart(const std::optional<std::string>& repo);

  rt::KvKey kernelScopedPolicyKey(const KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo);

  rt::KvKey kernelScopedWindowKey(const KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo);

  // `Readonly<{ policyKey: KvKey; windowKey: KvKey }>`
  struct KernelPolicyKeys {
    rt::KvKey policyKey;
    rt::KvKey windowKey;
  };
  KernelPolicyKeys kernelPolicyKeys(const KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo);

  /** A window that still holds reservations that have not passed their reset time. */
  bool hasLiveKernelReservations(const std::optional<KernelQuotaWindowV2>& window, double nowMs);

  // `value: unknown`: nullptr is undefined.
  bool positiveSafeInteger(const json::value* value);
  bool positiveSafeInteger(const json::value& value);
  bool positiveSafeInteger(double value);
  bool nonNegativeSafeInteger(const json::value* value);
  bool nonNegativeSafeInteger(const json::value& value);
  bool nonNegativeSafeInteger(double value);

  // `value: unknown` (a string is read as Number(value)): nullptr is undefined.
  double normalizeUsageLimit(const json::value* value, double fallback, double noUsageLimit);
  double normalizeUsageLimit(double value, double fallback, double noUsageLimit);

  double normalizeWindow(const json::value* value, double fallback);
  double normalizeWindow(double value, double fallback);

  double normalizeExpiration(const json::value* value, double noExpirationMs);
  double normalizeExpiration(double value, double noExpirationMs);

  // `value: Record<string, unknown>`; `repo: string | undefined`.
  bool validIdentity(const KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo, const json::object& value);

  // std::nullopt is null.
  std::optional<KernelQuotaPolicyV2> normalizeKernelQuotaPolicyV2(const json::value& value, const KernelQuotaScope& scope, const std::string& owner,
                                                                  const std::optional<std::string>& repo, const KernelQuotaConstants& constants);

  std::optional<KernelQuotaWindowV2> normalizeKernelQuotaWindowV2(const json::value& value, const KernelQuotaScope& scope, const std::string& owner,
                                                                  const std::optional<std::string>& repo);

  KernelQuotaWindowV2 newWindow(const KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo, double windowMs, double nowMs);

  KernelQuotaEffectiveWindow windowForEffectivePolicy(const std::optional<KernelQuotaWindowV2>& existing, const KernelQuotaScope& scope, const std::string& owner,
                                                      const std::optional<std::string>& repo, double effectiveWindowMs, double nowMs);

  // Throws Error "kernel quota policy is malformed" for a present row that does not normalize.
  KernelQuotaEffectivePolicy policyFor(const KernelQuotaRead<>& entry, const KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo,
                                       const KernelQuotaDefaults& defaults, const KernelQuotaConstants& constants);

  bool isExpired(double expiresAtMs, double nowMs, double noExpirationMs);

  double kernelReservationRetentionMs(double windowResetAtMs, double nowMs);

  rt::KvKey kernelReservationKey(const KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo, double windowCreatedAtMs,
                                 const std::string& requestId);

  rt::KvKey kernelReservationWindowPrefix(const KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo, double windowCreatedAtMs);

  std::optional<KernelQuotaReservationRowV2> normalizeKernelQuotaReservationRowV2(const json::value& value, const KernelQuotaScope& scope, const std::string& owner,
                                                                                  const std::optional<std::string>& repo);

  /**
   * True when the stored row is the reservation this request owns, identified by its
   * request id, route, and window.
   */
  // `reservation: KernelQuotaReservationRowV2 | null`
  bool kernelReservationMatchesRequest(const std::optional<KernelQuotaReservationRowV2>& reservation, const std::string& requestId, const std::string& route,
                                       double windowCreatedAtMs);

  /**
   * Reads one reservation row from a listing. A row whose key, request id, or window
   * disagrees with the listing is malformed rather than merely unknown.
   */
  KernelQuotaReservationRowV2 requireKernelReservationRow(const rt::KvKey& key, const json::value& value, const KernelQuotaScope& scope, const std::string& owner,
                                                          const std::optional<std::string>& repo, double windowCreatedAtMs);

  /**
   * Terminal-state precondition for a known reservation row: a row owned by another
   * request is invalid, an already-terminal row is settled or mismatched.
   */
  // `terminalState: "committed" | "released"`; std::nullopt is null.
  std::optional<KernelReservationSettlement> kernelReservationPrecondition(const KernelQuotaReservationRowV2& reservation, const std::string& requestId,
                                                                           const std::string& route, double windowCreatedAtMs, const std::string& terminalState);

  /** Builds the terminal row of a settled reservation, mirroring the terminal state. */
  KernelQuotaReservationRowV2 settledKernelReservationRow(const KernelQuotaReservationRowV2& reservation, const std::string& terminalState, const std::string& reason,
                                                          double nowMs);

  std::optional<KernelDefaultWindowCutoverV2> normalizeKernelDefaultWindowCutoverV2(const json::value& value);

  /** Owner and repo encoded in a listed window key, validated against the scope. */
  // `Readonly<{ owner: string; repo: string | undefined }>`
  struct KernelWindowScopeTarget {
    std::string owner;
    std::optional<std::string> repo;
  };
  // `prefix: readonly string[]` (only its length is read).
  KernelWindowScopeTarget kernelWindowScopeTarget(const KernelQuotaScope& scope, const rt::KvKey& key, const rt::KvKey& prefix);

  // `snapshot: Readonly<{ repoPolicy: KernelQuotaRead; orgPolicy: KernelQuotaRead }>`
  struct KernelQuotaScopeSnapshot {
    KernelQuotaRead<> repoPolicy;
    KernelQuotaRead<> orgPolicy;
  };
  // `{ limit_scope: "repo" | "org"; has_policy: boolean }`
  struct KernelQuotaScopeDecision {
    std::string limit_scope;
    bool has_policy;
    friend bool operator==(const KernelQuotaScopeDecision&, const KernelQuotaScopeDecision&) = default;
  };
  KernelQuotaScopeDecision decideKernelQuotaScope(const KernelQuotaScopeSnapshot& snapshot, const std::string& owner, const std::string& repo,
                                                  const KernelQuotaConstants& constants);

  KernelQuotaEligibilityPlan decideKernelQuotaEligibility(const KernelQuotaIdentity& identity, const KernelQuotaEligibilitySnapshot& snapshot,
                                                          const KernelQuotaDefaults& defaults, const KernelQuotaConstants& constants, double nowMs);

  KernelQuotaReservePlan decideKernelQuotaReserve(const KernelQuotaIdentity& identity, const KernelQuotaEffectivePolicy& effective, const KernelQuotaWindowV2& window,
                                                  const KernelQuotaRead<>& reservationEntry, const std::string& requestId, const std::string& route, double noUsageLimit,
                                                  double nowMs);

  KernelQuotaTerminalPlan decideKernelQuotaTerminalIntent(const KernelQuotaReservationIdentity& identity, const KernelQuotaRead<>& snapshot,
                                                          const std::string& terminalState, double nowMs);

  // `snapshot: Readonly<{ window: KernelQuotaWindowV2 | null; windowPresent: boolean }>`
  struct KernelQuotaSettlementSnapshot {
    std::optional<KernelQuotaWindowV2> window;
    bool windowPresent;
  };
  /** The native adapter reconciles legacy window cardinality before this decision. */
  KernelQuotaSettlePlan decideKernelQuotaSettlement(const KernelQuotaReservationRowV2& reservation, const KernelQuotaSettlementSnapshot& snapshot,
                                                    const std::string& terminalState, const std::string& reason, double nowMs);

  // `{ reservation, expireIn, leaseExpiresAtMs }`
  struct KernelQuotaRenewal {
    KernelQuotaReservationRowV2 reservation;
    double expireIn;
    double leaseExpiresAtMs;
  };
  KernelQuotaRenewal decideKernelQuotaRenewal(const KernelQuotaReservationIdentity& identity, const KernelQuotaRead<>& snapshot, double nowMs);

  // `"committed" | "released" | null`
  std::optional<std::string> decideKernelQuotaReclaim(const KernelQuotaReservationRowV2& reservation, double nowMs);

  KernelQuotaCutoverPlan decideKernelDefaultWindowCutover(const KernelQuotaRead<>& snapshot, const std::string& id, double nowMs);

  bool kernelDefaultWindowCutoverAcquired(const KernelQuotaRead<>& snapshot, const std::string& id);

  // `{ windowMs, expiresAtMs }`
  struct KernelPolicyTiming {
    double windowMs;
    double expiresAtMs;
    friend bool operator==(const KernelPolicyTiming&, const KernelPolicyTiming&) = default;
  };
  KernelPolicyTiming resolveKernelPolicyTiming(const KernelQuotaPolicySetOptions& options, const KernelQuotaEffectivePolicy& current, double noExpirationMs);

  // `Readonly<{ kind: "abort" | "proceed" }> | Readonly<{ kind: "reclaim"; window: KernelQuotaWindowV2 }>`
  struct KernelPolicyWindowGuardAbort {
    static constexpr std::string_view kind = "abort";
  };
  struct KernelPolicyWindowGuardProceed {
    static constexpr std::string_view kind = "proceed";
  };
  struct KernelPolicyWindowGuardReclaim {
    static constexpr std::string_view kind = "reclaim";
    KernelQuotaWindowV2 window;
  };
  using KernelPolicyWindowGuard = std::variant<KernelPolicyWindowGuardAbort, KernelPolicyWindowGuardProceed, KernelPolicyWindowGuardReclaim>;
  KernelPolicyWindowGuard decideKernelPolicyWindowGuard(const std::optional<KernelQuotaWindowV2>& window, bool windowPresent, bool reset, double nowMs);

  // `timing: Readonly<{ windowMs: number; expiresAtMs: number }>`
  KernelQuotaPolicyV2 makeKernelQuotaPolicy(const KernelQuotaIdentity& identity, const KernelQuotaEffectivePolicy& current, double usageLimitRequests,
                                            const KernelPolicyTiming& timing, double noUsageLimit, double nowMs);

  KernelQuotaPolicyWritePlan decideKernelQuotaPolicyWrite(const KernelQuotaIdentity& identity, const KernelQuotaPolicyV2& policy,
                                                          const std::optional<KernelQuotaWindowV2>& currentWindow, bool reset, double nowMs);

  KernelQuotaWindowV2 decideKernelQuotaPolicyDeleteWindow(const KernelQuotaIdentity& identity, const KernelQuotaEffectivePolicy& current, const KernelQuotaDefaults& defaults,
                                                          const std::optional<KernelQuotaWindowV2>& oldWindow, double nowMs);

  bool kernelQuotaDefaultCutoverBlocks(const KernelQuotaEffectivePolicy& current, const KernelQuotaRead<>& marker);

  bool kernelQuotaPolicyResetRequested(const KernelQuotaPolicySetOptions& options, double currentWindowMs, double nextWindowMs);

  // `snapshot: Readonly<{ policy: KernelQuotaRead; defaultCutover: KernelQuotaRead }>`
  struct KernelQuotaPolicyDeleteSnapshot {
    KernelQuotaRead<> policy;
    KernelQuotaRead<> defaultCutover;
  };
  // `"absent" | "conflict" | "proceed"`
  std::string decideKernelQuotaPolicyDeletePrecondition(const KernelQuotaPolicyDeleteSnapshot& snapshot);
}
