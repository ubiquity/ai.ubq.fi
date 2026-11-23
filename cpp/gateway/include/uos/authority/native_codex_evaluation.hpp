#pragma once
// Port of src/authority/native-codex-evaluation.ts.
// Source-owned account/class/capacity evaluation. Native owners supply trusted capability and overage metadata.
#include "uos/authority/native_codex_contracts.hpp"
#include "uos/rt/ordered.hpp"

#include <optional>
#include <string>
#include <vector>

namespace uos::authority::native_codex_evaluation {
  /** A slot identity resolved from the pool before its durable routing state is read. */
  // `Readonly<{ slot; accountIdHash; credentialVersion }>`: the same shape as CodexRoutingIdentity,
  // which the TS passes for it.
  using CodexRoutingSlotIdentity = native_codex_contracts::CodexRoutingIdentity;

  /** A selectable account whose durable routing generation is already resolved. */
  // `CodexRoutingAccount & Readonly<{ routingGeneration: number }>`: routingGeneration is always set.
  using CodexRoutedAccount = native_codex_contracts::CodexRoutingAccount;

  struct CodexRoutingAccountEvaluation {
    // `Readonly<{ model: string; detail: string | null }>`
    struct ModelUnavailable {
      std::string model;
      std::optional<std::string> detail;
    };
    /** The resolved slot identity even when the account cannot serve now. */
    CodexRoutedAccount account;
    /** Set when the account can take traffic now. */
    std::optional<native_codex_contracts::CodexRoutingAccount> routedAccount;
    /** The 1-based slot number of a skipped account. */
    std::optional<double> skippedSlot;
    /** The circuit that recorded the skip, or null for a credential fence. */
    std::optional<native_codex_contracts::CodexProbeCircuit> blockedCircuit;
    std::optional<double> retryAtMs;
    std::optional<native_codex_contracts::CodexBlockedAccountMetadata> blockedAccount;
    /** Only these observed states may advance the global active selection. */
    // `Exclude<CodexActiveAccountTransitionReason, "account_removed_or_replaced"> | null`
    std::optional<native_codex_contracts::CodexActiveAccountTransitionReason> activeTransitionReason;
    /** A fresh, account-bound capacity sample proved the requested class empty. */
    bool capacityExhausted;
    /** Set only when catalog evidence or a learned rejection proves this account cannot serve the named model. */
    std::optional<ModelUnavailable> modelUnavailable;
  };

  // `CodexRoutingAccumulation` (Readonly) and `CodexRoutingAccumulator` (mutable) have the same
  // members; the TS passes an accumulator where an accumulation is expected, so both are this struct.
  struct CodexRoutingAccumulation {
    std::vector<native_codex_contracts::CodexRoutingAccount> available;
    std::vector<native_codex_contracts::CodexBlockedAccountMetadata> blockedAccounts;
    std::vector<double> skipped;
    std::optional<double> retryAt;
    bool hasQuotaBlock;
    bool hasUpstreamTimeoutBlock;
    /** The first account skipped because it cannot serve the requested model. */
    std::optional<CodexRoutingAccountEvaluation::ModelUnavailable> modelUnavailable;
  };
  using CodexRoutingAccumulator = CodexRoutingAccumulation;

  // `model: string | null`; `observationsByAccount: ReadonlyMap<string, CodexCapacityRoutingObservation>`.
  CodexRoutingAccountEvaluation evaluateCodexRoutingAccount(const native_codex_contracts::CodexAccountRoutingState& state, const CodexRoutingSlotIdentity& mapped,
                                                            const std::optional<std::string>& model,
                                                            const rt::OrderedMap<std::string, native_codex_contracts::CodexCapacityRoutingObservation>& observationsByAccount,
                                                            double now, const native_codex_contracts::CodexRoutingAccountPolicy& policy);

  /** Fold one evaluated account into the running selection accumulator. */
  void foldCodexRoutingEvaluation(CodexRoutingAccumulator& accumulator, const CodexRoutingAccountEvaluation& evaluated);

  // `activeSnapshot` defaults to `{ selection: null }`.
  native_codex_contracts::CodexRouteSelection classifyCodexRouteSelection(const CodexRoutingAccumulation& accumulated, bool fullCohortExhausted = false,
                                                                          const native_codex_contracts::CodexActiveAccountSnapshot& activeSnapshot = {},
                                                                          const std::optional<std::string>& poolSnapshotJson = std::nullopt,
                                                                          const std::optional<std::string>& capacitySnapshotJson = std::nullopt,
                                                                          bool fullCohortUnavailable = false);

  // `durable: CodexAccountRoutingState | null`: nullptr is null. The TS compares slots by object
  // identity (`slot !== normalized.slots[index]`); normalizeRoutingState returns a durable slot
  // itself exactly when it leaves it unchanged, and every slot it rewrites differs in value (a
  // generation step, an attached account hash or a different identity), so the C++ compares the
  // slots by value.
  bool routingStateChangedByNormalization(const native_codex_contracts::CodexAccountRoutingState* durable,
                                          const native_codex_contracts::CodexAccountRoutingState& normalized);
}
