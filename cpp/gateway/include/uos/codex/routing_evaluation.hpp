#pragma once
// Port of src/codex/routing-evaluation.ts.
// Native auth/model/overage boundaries around source-owned pure evaluation.
#include "uos/authority/native_codex_evaluation.hpp"
#include "uos/codex/routing_state.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace uos::codex::routing_evaluation {
  // `decisions.CodexRoutingSlotIdentity`
  using CodexRoutingSlotIdentity = authority::native_codex_evaluation::CodexRoutingSlotIdentity;

  // `Omit<decisions.CodexRoutingAccountEvaluation, "account" | "routedAccount" | "blockedAccount"> &
  //  Readonly<{ account: RoutingAccount & Readonly<{ routingGeneration: number }>; routedAccount: RoutingAccount | null;
  //  blockedAccount: CodexBlockedRoutingAccount | null }>`: decisions' members in their order with the three
  // accounts hydrated. `account.routingGeneration` (the base member) is always set.
  struct CodexRoutingAccountEvaluation {
    using ModelUnavailable = authority::native_codex_evaluation::CodexRoutingAccountEvaluation::ModelUnavailable;
    /** The resolved slot identity even when the account cannot serve now. */
    routing_state::RoutingAccount account;
    /** Set when the account can take traffic now. */
    std::optional<routing_state::RoutingAccount> routedAccount;
    /** The 1-based slot number of a skipped account. */
    std::optional<double> skippedSlot;
    /** The circuit that recorded the skip, or null for a credential fence. */
    std::optional<routing_state::CodexProbeCircuit> blockedCircuit;
    std::optional<double> retryAtMs;
    std::optional<routing_state::CodexBlockedRoutingAccount> blockedAccount;
    /** Only these observed states may advance the global active selection. */
    std::optional<routing_state::CodexActiveAccountTransitionReason> activeTransitionReason;
    /** A fresh, account-bound capacity sample proved the requested class empty. */
    bool capacityExhausted;
    /** Set only when catalog evidence or a learned rejection proves this account cannot serve the named model. */
    std::optional<ModelUnavailable> modelUnavailable;
  };

  // `Omit<decisions.CodexRoutingAccumulator, "available" | "blockedAccounts"> &
  //  { available: RoutingAccount[]; blockedAccounts: CodexBlockedRoutingAccount[] }`
  struct CodexRoutingAccumulator {
    std::vector<routing_state::RoutingAccount> available;
    std::vector<routing_state::CodexBlockedRoutingAccount> blockedAccounts;
    std::vector<double> skipped;
    std::optional<double> retryAt;
    bool hasQuotaBlock;
    bool hasUpstreamTimeoutBlock;
    /** The first account skipped because it cannot serve the requested model. */
    std::optional<CodexRoutingAccountEvaluation::ModelUnavailable> modelUnavailable;
  };

  // `Readonly<CodexRoutingAccumulator>`
  using CodexRoutingAccumulation = CodexRoutingAccumulator;

  authority::native_codex_evaluation::CodexRoutingAccountEvaluation codexEvaluationMetadata(const CodexRoutingAccountEvaluation& evaluated);

  // `poolSnapshotJson`, `capacitySnapshotJson`: `string | null = null`. Throws rt::JsError
  // "Native Codex account metadata lost its trusted pool binding" as the TS does.
  routing_state::RouteSelection hydrateCodexRouteSelection(const routing_state::CodexRouteSelection& selection,
                                                           const std::vector<routing_state::RoutingAccount>& accounts,
                                                           const std::optional<std::string>& poolSnapshotJson = std::nullopt,
                                                           const std::optional<std::string>& capacitySnapshotJson = std::nullopt);

  // `model: string | null`; `observationsByAccount: ReadonlyMap<string, CodexCapacityRoutingObservation>`;
  // `modelUnavailable: ReadonlyMap<string, string | null>` (codexModelUnavailableAccounts' result).
  CodexRoutingAccountEvaluation evaluateCodexRoutingAccount(
    const routing_state::CodexAccountRoutingState& state, const ::uos::types::CodexAuthState& auth, const CodexRoutingSlotIdentity& mapped,
    const std::optional<std::string>& model, const rt::OrderedMap<std::string, routing_state::CodexCapacityRoutingObservation>& observationsByAccount,
    double now, const rt::OrderedMap<std::string, std::optional<std::string>>& modelUnavailable);

  void foldCodexRoutingEvaluation(CodexRoutingAccumulator& accumulator, const CodexRoutingAccountEvaluation& evaluated);

  // `activeSnapshot` defaults to `{ selection: null }`; the snapshot JSON strings to null.
  routing_state::RouteSelection classifyCodexRouteSelection(const CodexRoutingAccumulation& accumulated, bool fullCohortExhausted = false,
                                                            const routing_state::CodexActiveAccountSnapshot& activeSnapshot = {},
                                                            const std::optional<std::string>& poolSnapshotJson = std::nullopt,
                                                            const std::optional<std::string>& capacitySnapshotJson = std::nullopt,
                                                            bool fullCohortUnavailable = false);

  /** Accumulate every ordered account's routing outcome, preserving selection order. */
  // `byId: ReadonlyMap<string, CodexRoutingSlotIdentity>`; `model: string | null`.
  CodexRoutingAccumulation accumulateCodexRoutingAccounts(const routing_state::CodexAccountRoutingState& state,
                                                          const rt::OrderedMap<std::string, CodexRoutingSlotIdentity>& byId,
                                                          const std::vector<::uos::types::CodexAuthState>& orderedAccounts,
                                                          const std::optional<std::string>& model,
                                                          const rt::OrderedMap<std::string, routing_state::CodexCapacityRoutingObservation>& observationsByAccount,
                                                          double now);

  // `value: unknown`: nullptr is undefined. A malformed native owner throws CodexError (through
  // parseCodexAuthPool), as the TS does.
  std::optional<::uos::types::CodexAuthPoolState> parseCodexAuthPoolSnapshot(const json::value& value);
  std::optional<::uos::types::CodexAuthPoolState> parseCodexAuthPoolSnapshot(const json::value* value);
}
