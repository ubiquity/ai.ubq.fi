#pragma once
// Port of src/authority/native-codex.ts.
// Source-owned sticky serial selection and fixed A/P/R/C transition plans.
#include "uos/authority/native_codex_contracts.hpp"
#include "uos/authority/native_codex_evaluation.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::authority::native_codex {
  struct SerialRoutingEvaluations {
    native_codex_evaluation::CodexRoutingAccumulation accumulated;
    rt::OrderedMap<std::string, native_codex_evaluation::CodexRoutingAccountEvaluation> byAccountId;
    rt::OrderedMap<std::string, native_codex_evaluation::CodexRoutingSlotIdentity> identityByAccountId;
  };

  struct SerialSelectionDecision {
    native_codex_contracts::CodexRouteSelection selection;
    std::optional<native_codex_contracts::CodexActiveAccountSelection> nextActive;
  };

  /** Preserve the native four-row checks, including check-only hot admissions and write order R then A. */
  // `durableRouting: CodexAccountRoutingState | null`: nullptr is null.
  native_codex_contracts::CodexSerialAdmissionPlan planCodexSerialCommit(const native_codex_contracts::CodexRoutingRowRevisions& checks,
                                                                         const native_codex_contracts::CodexAccountRoutingState* durableRouting,
                                                                         const native_codex_contracts::CodexAccountRoutingState& normalized,
                                                                         const SerialSelectionDecision& decision);

  /** Credential-free coherent snapshot, populated only by the trusted native owner/authority adapter. */
  struct CodexSerialSnapshot {
    // `unknown` rows as read: null when absent.
    json::value active;
    json::value routing;
    json::value capacity;
    std::vector<native_codex_contracts::CodexRoutingIdentity> poolIdentities;
    std::vector<std::string> selectedAccountHashes;
    rt::OrderedMap<std::string, native_codex_contracts::CodexRoutingAccountPolicy> accountPolicies;
    native_codex_contracts::CodexRoutingRowRevisions revisions;
  };

  // `model: string | null`; std::nullopt is the TS null result.
  std::optional<native_codex_contracts::CodexSerialAdmissionPlan> selectCodex(const CodexSerialSnapshot& snapshot, double now, const std::optional<std::string>& model);

  // `transitionReason: CodexActiveAccountTransitionReason | null`.
  std::optional<native_codex_contracts::CodexActiveAccountSelection> activeSelectionForAccount(
    const native_codex_contracts::CodexRoutingAccount& account, const std::string& poolVersionstamp, double generation, double now,
    const std::optional<native_codex_contracts::CodexActiveAccountTransitionReason>& transitionReason);

  // `active: CodexActiveAccountSelection | null`.
  std::optional<SerialSelectionDecision> serialSelectionDecision(const std::optional<native_codex_contracts::CodexActiveAccountSelection>& active,
                                                                 const std::vector<native_codex_contracts::CodexRoutingIdentity>& pool,
                                                                 const SerialRoutingEvaluations& evaluations, const std::string& poolVersionstamp, double now,
                                                                 const std::optional<std::string>& poolSnapshotJson,
                                                                 const std::optional<std::string>& capacitySnapshotJson);

  // The value planCodexResetElection returns:
  // `{ kind: "reset_election", checks, writes: { routing: CodexAccountRoutingState | null; active } }`.
  struct CodexResetElectionPlan {
    struct Writes {
      std::optional<native_codex_contracts::CodexAccountRoutingState> routing;
      native_codex_contracts::CodexActiveAccountSelection active;
    };
    static constexpr std::string_view kind = "reset_election";
    native_codex_contracts::CodexRoutingRowRevisions checks;
    Writes writes;
  };

  /** A verified-reset election checks the same four native rows and always writes A after optional normalization R. */
  CodexResetElectionPlan planCodexResetElection(const native_codex_contracts::CodexRoutingRowRevisions& checks,
                                                const native_codex_contracts::CodexAccountRoutingState* durableRouting,
                                                const native_codex_contracts::CodexAccountRoutingState& normalized,
                                                const native_codex_contracts::CodexActiveAccountSelection& nextActive);
}
