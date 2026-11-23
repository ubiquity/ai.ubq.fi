#pragma once
// Port of src/authority/native-codex-mutations.ts.
// Source-owned routing/probe/reset-fence transitions. Native owners inject clocks and lease tokens.
#include "uos/authority/native_codex_capacity.hpp"
#include "uos/authority/native_codex_contracts.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace uos::authority::native_codex_mutations {
  // Every `CodexAccountRoutingState | null` result is std::nullopt for null.

  std::optional<native_codex_contracts::CodexAccountRoutingState> markCodexUpstreamTimeout(const native_codex_contracts::CodexAccountRoutingState& state,
                                                                                           const native_codex_contracts::CodexRoutingAccount& account, double now,
                                                                                           double updatedAtMs);

  std::optional<native_codex_contracts::CodexAccountRoutingState> markCodexCredentialInvalid(const native_codex_contracts::CodexAccountRoutingState& state,
                                                                                             const native_codex_contracts::CodexRoutingAccount& account, double now,
                                                                                             double updatedAtMs);

  std::optional<native_codex_contracts::CodexAccountRoutingState> releaseCodexRoutingProbe(const native_codex_contracts::CodexAccountRoutingState& state,
                                                                                           const native_codex_contracts::CodexRoutingAccount& account, double updatedAtMs);

  std::optional<native_codex_contracts::CodexAccountRoutingState> markCodexSuccess(const native_codex_contracts::CodexAccountRoutingState& state,
                                                                                   const native_codex_contracts::CodexRoutingAccount& account, double updatedAtMs);

  // `identity: Readonly<{ accountIdHash: string; credentialVersion: string }>`: a CodexRoutingIdentity also fits.
  std::optional<native_codex_contracts::CodexAccountRoutingState> reconcileRoutingCredential(const native_codex_contracts::CodexAccountRoutingState& state,
                                                                                             const native_codex_contracts::CodexRoutingAccount& account,
                                                                                             const native_codex_capacity::CodexRoutingAccountIdentity& identity,
                                                                                             bool sameAccount, double updatedAtMs);
  std::optional<native_codex_contracts::CodexAccountRoutingState> reconcileRoutingCredential(const native_codex_contracts::CodexAccountRoutingState& state,
                                                                                             const native_codex_contracts::CodexRoutingAccount& account,
                                                                                             const native_codex_contracts::CodexRoutingIdentity& identity,
                                                                                             bool sameAccount, double updatedAtMs);

  // `value: unknown`: nullptr is undefined.
  bool isCodexQuotaBlockFenceCurrent(const json::value* value, const native_codex_contracts::CodexRoutingAccount& account, double quotaResetAtMs,
                                     double routingGeneration);
  bool isCodexQuotaBlockFenceCurrent(const json::value& value, const native_codex_contracts::CodexRoutingAccount& account, double quotaResetAtMs,
                                     double routingGeneration);

  // Throws rt::TypeError, as the TS does, when `account.slot` names no slot of `state`.
  native_codex_contracts::CodexAccountRoutingState recoveryProbeTransition(const native_codex_contracts::CodexAccountRoutingState& state,
                                                                           const native_codex_contracts::CodexRoutingAccount& account,
                                                                           const native_codex_contracts::CodexProbeLease& recoveryLease, double updatedAtMs);

  // The value buildExpiredProbeClaim returns: `Readonly<{ next: CodexAccountRoutingState; lease: CodexProbeLease }>`.
  struct CodexExpiredProbeClaim {
    native_codex_contracts::CodexAccountRoutingState next;
    native_codex_contracts::CodexProbeLease lease;
  };

  std::optional<CodexExpiredProbeClaim> buildExpiredProbeClaim(const native_codex_contracts::CodexAccountRoutingState& base,
                                                               const native_codex_contracts::CodexRoutingAccount& account, double now, const std::string& token,
                                                               double updatedAtMs);

  // `input: Readonly<{ quotaResetAtMs: number; routingGeneration: number }>`
  struct CodexStaleVerifiedResetProbeInput {
    double quotaResetAtMs;
    double routingGeneration;
  };

  bool staleVerifiedResetProbeEligible(const native_codex_contracts::CodexAccountRoutingState& state, const native_codex_contracts::CodexRoutingSlot& current,
                                       const native_codex_contracts::CodexRoutingAccount& account, const CodexStaleVerifiedResetProbeInput& input, double nowMs);

  // `value: unknown`: nullptr is undefined.
  bool isCodexActiveAccountSnapshotCurrent(const json::value* value, const native_codex_contracts::CodexActiveAccountSnapshot& snapshot);
  bool isCodexActiveAccountSnapshotCurrent(const json::value& value, const native_codex_contracts::CodexActiveAccountSnapshot& snapshot);

  // `value: unknown`: nullptr is undefined.
  bool staleResetRoutingIdentityCurrent(const json::value* value, const native_codex_contracts::CodexRoutingAccount& account, double quotaResetAtMs);
  bool staleResetRoutingIdentityCurrent(const json::value& value, const native_codex_contracts::CodexRoutingAccount& account, double quotaResetAtMs);

  // `active: CodexActiveAccountSelection | null`
  double nextResetElectionGeneration(const std::optional<native_codex_contracts::CodexActiveAccountSelection>& active,
                                     const native_codex_contracts::CodexRoutingAccount& candidate);

  bool resetCandidateSlotMatches(const native_codex_contracts::CodexRoutingSlot& currentSlot, const native_codex_contracts::CodexRoutingAccount& account,
                                 bool routingIdentityCurrent);

  native_codex_contracts::CodexRoutingSlot recheckSlotTransition(const native_codex_contracts::CodexRoutingSlot& current, double recheckAtMs);

  // The value locateRoutingSlot returns: `Readonly<{ index: number; slot: CodexRoutingSlot }>`.
  struct CodexLocatedRoutingSlot {
    double index;
    native_codex_contracts::CodexRoutingSlot slot;
  };

  std::optional<CodexLocatedRoutingSlot> locateRoutingSlot(const native_codex_contracts::CodexAccountRoutingState& state, double slotNumber);

  /** A source admission fence binds normalized slot metadata without inventing a missing routing row. */
  // `normalized: CodexAccountRoutingState | null`: nullptr is null. The result is null as std::nullopt.
  std::optional<double> admissionRefreshRoutingGeneration(const native_codex_contracts::CodexAccountRoutingState* normalized,
                                                          const native_codex_contracts::CodexRoutingAccount& account);

  // The prepareCodexAdmissionRefresh result union, by `kind`.
  struct CodexAdmissionRefreshPlanCurrent {
    static constexpr std::string_view kind = "current";
  };
  struct CodexAdmissionRefreshPlanRefresh {
    static constexpr std::string_view kind = "refresh";
    native_codex_contracts::CodexActiveAccountSelection next;
  };
  using CodexAdmissionRefreshPlan = std::variant<CodexAdmissionRefreshPlanCurrent, CodexAdmissionRefreshPlanRefresh>;

  CodexAdmissionRefreshPlan prepareCodexAdmissionRefresh(const native_codex_contracts::CodexActiveAccountSelection& active,
                                                         const native_codex_contracts::CodexRoutingAccount& account, const std::string& poolVersionstamp,
                                                         double routingGeneration);
}
