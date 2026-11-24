#include "uos/authority/native_codex.hpp"

#include "uos/authority/native_codex_capacity.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"

#include <algorithm>
#include <utility>

namespace uos::authority::native_codex {
  using namespace native_codex_contracts;
  using native_codex_evaluation::CodexRoutingAccountEvaluation;
  using native_codex_evaluation::CodexRoutingAccumulator;

  namespace {
    // `isVersionstamp(value)` on a `string | null` revision.
    bool isVersionstampText(const std::optional<std::string>& value) {
      const json::value text = value ? json::value(*value) : json::value(nullptr);
      return isVersionstamp(&text);
    }

    bool activeSelectionNeedsRefresh(const CodexActiveAccountSelection& active, const CodexRoutingAccount& account, const std::string& poolVersionstamp) {
      return active.account_id_hash != account.accountIdHash || active.credential_version != account.credentialVersion || active.pool_versionstamp != poolVersionstamp ||
             active.slot != account.slot || !account.routingGeneration.has_value() || active.routing_generation != *account.routingGeneration;
    }

    struct SerialSelectionContext {
      const SerialRoutingEvaluations& evaluations;
      std::string poolVersionstamp;
      double now;
      std::optional<std::string> poolSnapshotJson;
      std::optional<std::string> capacitySnapshotJson;
      std::vector<CodexRoutingAccount> eligibleInPoolOrder;
      bool fullCohortExhausted;
      bool fullCohortUnavailable;
      // `CodexRoutingAccountEvaluation | null`
      const CodexRoutingAccountEvaluation* activeEvaluation;
    };

    /** One eligible account admitted under a committed active row. */
    CodexRouteSelection eligibleSerialSelection(const CodexRoutingAccount& account, const CodexActiveAccountSelection& selection, const SerialRoutingEvaluations& evaluations) {
      CodexRoutingAccount admitted = account;
      admitted.activeGeneration = selection.generation;
      admitted.activePoolVersionstamp = selection.pool_versionstamp;
      admitted.activeTransitionReason =
        selection.transition_reason ? rt::Nullable<CodexActiveAccountTransitionReason>(*selection.transition_reason) : rt::Nullable<CodexActiveAccountTransitionReason>(nullptr);
      CodexRouteSelectionEligible eligible;
      eligible.accounts.push_back(std::move(admitted));
      eligible.skippedSlots = evaluations.accumulated.skipped;
      eligible.blockedAccounts = evaluations.accumulated.blockedAccounts;
      return eligible;
    }

    /**
     * The blocked/credential-invalid result bound to the active row as this
     * decision leaves it: a same-identity fence refresh the decision commits is
     * the observed active row, not a stale pre-refresh value.
     */
    CodexRouteSelection blockedSerialSelection(const SerialSelectionContext& context, const std::optional<CodexActiveAccountSelection>& current) {
      CodexActiveAccountSnapshot snapshot;
      snapshot.selection = current;
      return native_codex_evaluation::classifyCodexRouteSelection(context.evaluations.accumulated, context.fullCohortExhausted, snapshot, context.poolSnapshotJson,
                                                                  context.capacitySnapshotJson, context.fullCohortUnavailable);
    }

    /** A missing active row bootstraps the first account eligible under the requested model. */
    std::optional<SerialSelectionDecision> serialBootstrapDecision(const SerialSelectionContext& context) {
      if (context.eligibleInPoolOrder.empty()) return SerialSelectionDecision{blockedSerialSelection(context, std::nullopt), std::nullopt};
      const CodexRoutingAccount& bootstrap = context.eligibleInPoolOrder.front();
      std::optional<CodexActiveAccountSelection> nextActive = activeSelectionForAccount(bootstrap, context.poolVersionstamp, 1, context.now, std::nullopt);
      if (!nextActive) return std::nullopt;
      CodexRouteSelection selection = eligibleSerialSelection(bootstrap, *nextActive, context.evaluations);
      return SerialSelectionDecision{std::move(selection), std::move(nextActive)};
    }

    /**
     * A pool reorder preserves the opaque identity; removal or replacement is the
     * only membership observation that may select a sibling.
     */
    std::optional<SerialSelectionDecision> serialReplacementDecision(const SerialSelectionContext& context, const CodexActiveAccountSelection& active) {
      if (context.eligibleInPoolOrder.empty()) return SerialSelectionDecision{blockedSerialSelection(context, active), std::nullopt};
      const CodexRoutingAccount& replacement = context.eligibleInPoolOrder.front();
      std::optional<CodexActiveAccountSelection> nextActive =
        activeSelectionForAccount(replacement, context.poolVersionstamp, active.generation + 1, context.now, CodexActiveAccountTransitionReason("account_removed_or_replaced"));
      if (!nextActive) return std::nullopt;
      CodexRouteSelection selection = eligibleSerialSelection(replacement, *nextActive, context.evaluations);
      return SerialSelectionDecision{std::move(selection), std::move(nextActive)};
    }

    /** The active account is still eligible; only its same-identity fences may refresh. */
    std::optional<SerialSelectionDecision> serialCurrentActiveDecision(const SerialSelectionContext& context, const CodexActiveAccountSelection& active,
                                                                       const CodexRoutingAccount& current) {
      const bool needsRefresh = activeSelectionNeedsRefresh(active, current, context.poolVersionstamp);
      std::optional<CodexActiveAccountSelection> refreshed =
        needsRefresh ? activeSelectionForAccount(current, context.poolVersionstamp, active.generation, context.now, active.transition_reason) : std::nullopt;
      if (needsRefresh && !refreshed) return std::nullopt;
      CodexRouteSelection selection = eligibleSerialSelection(current, refreshed ? *refreshed : active, context.evaluations);
      return SerialSelectionDecision{std::move(selection), std::move(refreshed)};
    }

    /** The active account is blocked, invalid, or probing: move only for an authoritative reason. */
    std::optional<SerialSelectionDecision> serialBlockedActiveDecision(const SerialSelectionContext& context, const CodexActiveAccountSelection& active,
                                                                       const CodexRoutingAccountEvaluation& activeEvaluation) {
      // A recovery-probe lease or transient circuit is not capacity evidence. It
      // must fail retryably without sending traffic to a healthy sibling, and it
      // must not unlock reset or paid fallback paths.
      if (!activeEvaluation.activeTransitionReason) return SerialSelectionDecision{CodexRouteSelectionRoutingUnavailable{}, std::nullopt};
      if (context.eligibleInPoolOrder.empty()) {
        const bool needsRefresh = activeSelectionNeedsRefresh(active, activeEvaluation.account, context.poolVersionstamp);
        std::optional<CodexActiveAccountSelection> refreshed =
          needsRefresh ? activeSelectionForAccount(activeEvaluation.account, context.poolVersionstamp, active.generation, context.now, active.transition_reason) : std::nullopt;
        if (needsRefresh && !refreshed) return std::nullopt;
        CodexRouteSelection selection = blockedSerialSelection(context, refreshed ? *refreshed : active);
        return SerialSelectionDecision{std::move(selection), std::move(refreshed)};
      }
      const CodexRoutingAccount& replacement = context.eligibleInPoolOrder.front();
      std::optional<CodexActiveAccountSelection> nextActive =
        activeSelectionForAccount(replacement, context.poolVersionstamp, active.generation + 1, context.now, activeEvaluation.activeTransitionReason);
      if (!nextActive) return std::nullopt;
      CodexRouteSelection selection = eligibleSerialSelection(replacement, *nextActive, context.evaluations);
      return SerialSelectionDecision{std::move(selection), std::move(nextActive)};
    }
  }

  std::optional<CodexActiveAccountSelection> activeSelectionForAccount(const CodexRoutingAccount& account, const std::string& poolVersionstamp, double generation, double now,
                                                                       const std::optional<CodexActiveAccountTransitionReason>& transitionReason) {
    if (!account.routingGeneration.has_value() || !isVersionstampText(poolVersionstamp) || !rt::Number::isSafeInteger(generation) || generation < 1) return std::nullopt;
    CodexActiveAccountSelection selection;
    selection.v = 1;
    selection.account_id_hash = account.accountIdHash;
    selection.credential_version = account.credentialVersion;
    selection.pool_versionstamp = poolVersionstamp;
    selection.slot = account.slot;
    selection.routing_generation = *account.routingGeneration;
    selection.generation = generation;
    selection.transition_reason = transitionReason;
    selection.updated_at_ms = now;
    return selection;
  }

  std::optional<SerialSelectionDecision> serialSelectionDecision(const std::optional<CodexActiveAccountSelection>& active, const std::vector<CodexRoutingIdentity>& pool,
                                                                 const SerialRoutingEvaluations& evaluations, const std::string& poolVersionstamp, double now,
                                                                 const std::optional<std::string>& poolSnapshotJson,
                                                                 const std::optional<std::string>& capacitySnapshotJson) {
    std::vector<CodexRoutingAccount> eligibleInPoolOrder;
    for (const CodexRoutingIdentity& auth : pool) {
      const CodexRoutingAccountEvaluation* evaluation = evaluations.byAccountId.get(auth.accountIdHash);
      if (evaluation != nullptr && evaluation->routedAccount) eligibleInPoolOrder.push_back(*evaluation->routedAccount);
    }
    if (!eligibleInPoolOrder.empty() && eligibleInPoolOrder.front().quotaUsageProbeRequired) {
      const auto included = std::find_if(eligibleInPoolOrder.begin(), eligibleInPoolOrder.end(), [](const CodexRoutingAccount& account) {
        return account.hasFreshIncludedCapacity;
      });
      if (included != eligibleInPoolOrder.end()) std::rotate(eligibleInPoolOrder.begin(), included, included + 1);
    }
    const CodexRoutingIdentity* accountForActive = nullptr;
    if (active) {
      const auto found = std::find_if(pool.begin(), pool.end(), [&](const CodexRoutingIdentity& account) {
        const native_codex_evaluation::CodexRoutingSlotIdentity* identity = evaluations.identityByAccountId.get(account.accountIdHash);
        return identity != nullptr && identity->accountIdHash == active->account_id_hash;
      });
      if (found != pool.end()) accountForActive = &*found;
    }
    const CodexRoutingAccountEvaluation* activeEvaluation = accountForActive != nullptr ? evaluations.byAccountId.get(accountForActive->accountIdHash) : nullptr;
    const auto reasonOf = [&evaluations](const CodexRoutingIdentity& auth) -> std::optional<CodexActiveAccountTransitionReason> {
      const CodexRoutingAccountEvaluation* evaluation = evaluations.byAccountId.get(auth.accountIdHash);
      return evaluation != nullptr ? evaluation->activeTransitionReason : std::nullopt;
    };
    const bool fullCohortExhausted =
      !pool.empty() && std::all_of(pool.begin(), pool.end(), [&reasonOf](const CodexRoutingIdentity& auth) { return reasonOf(auth) == "quota_exhausted"; });
    const bool fullCohortUnavailable = !pool.empty() && std::all_of(pool.begin(), pool.end(), [&reasonOf](const CodexRoutingIdentity& auth) {
      const std::optional<CodexActiveAccountTransitionReason> reason = reasonOf(auth);
      return reason == "quota_exhausted" || reason == "credential_invalid";
    });
    const SerialSelectionContext context{
      .evaluations = evaluations,
      .poolVersionstamp = poolVersionstamp,
      .now = now,
      .poolSnapshotJson = poolSnapshotJson,
      .capacitySnapshotJson = capacitySnapshotJson,
      .eligibleInPoolOrder = std::move(eligibleInPoolOrder),
      .fullCohortExhausted = fullCohortExhausted,
      .fullCohortUnavailable = fullCohortUnavailable,
      .activeEvaluation = activeEvaluation,
    };
    if (!active) return serialBootstrapDecision(context);
    if (activeEvaluation == nullptr) return serialReplacementDecision(context, *active);
    if (activeEvaluation->routedAccount) return serialCurrentActiveDecision(context, *active, *activeEvaluation->routedAccount);
    return serialBlockedActiveDecision(context, *active, *activeEvaluation);
  }

  CodexSerialAdmissionPlan planCodexSerialCommit(const CodexRoutingRowRevisions& checks, const CodexAccountRoutingState* durableRouting,
                                                 const CodexAccountRoutingState& normalized, const SerialSelectionDecision& decision) {
    CodexSerialAdmissionPlan plan;
    plan.checks = checks;
    if (native_codex_evaluation::routingStateChangedByNormalization(durableRouting, normalized)) plan.writes.routing = normalized;
    plan.writes.active = decision.nextActive;
    plan.selection = decision.selection;
    return plan;
  }

  std::optional<CodexSerialAdmissionPlan> selectCodex(const CodexSerialSnapshot& snapshot, double now, const std::optional<std::string>& model) {
    const std::optional<CodexActiveAccountSelection> active =
      snapshot.active.is_null() ? std::nullopt : parseCodexActiveAccountSelection(snapshot.active);
    if (!snapshot.active.is_null() && !active) return std::nullopt;
    if (!isVersionstampText(snapshot.revisions.pool)) return std::nullopt;
    const std::optional<CodexAccountRoutingState> durable = snapshot.routing.is_null() ? std::nullopt : parseCodexAccountRoutingState(snapshot.routing);
    if (!snapshot.routing.is_null() && !durable) return std::nullopt;
    const CodexAccountRoutingState* durableRow = durable ? &*durable : nullptr;
    const CodexAccountRoutingState normalized = native_codex_capacity::normalizeRoutingState(durableRow, snapshot.poolIdentities, now, true);
    const rt::OrderedSet<std::string> selected(snapshot.selectedAccountHashes.begin(), snapshot.selectedAccountHashes.end());
    std::vector<CodexRoutingIdentity> cohort;
    for (const CodexRoutingIdentity& identity : snapshot.poolIdentities)
      if (selected.has(identity.accountIdHash)) cohort.push_back(identity);
    rt::OrderedMap<std::string, CodexCapacityRoutingObservation> observations;
    for (CodexCapacityRoutingObservation& observation : native_codex_capacity::parseStoredCapacityObservationStore(snapshot.capacity)) {
      const std::string accountIdHash = observation.account_id_hash;
      observations.set(accountIdHash, std::move(observation));
    }
    CodexRoutingAccumulator accumulator{
      .available = {},
      .blockedAccounts = {},
      .skipped = {},
      .retryAt = std::nullopt,
      .hasQuotaBlock = false,
      .hasUpstreamTimeoutBlock = false,
      .modelUnavailable = std::nullopt,
    };
    rt::OrderedMap<std::string, CodexRoutingAccountEvaluation> byAccountId;
    rt::OrderedMap<std::string, native_codex_evaluation::CodexRoutingSlotIdentity> identityByAccountId;
    for (const CodexRoutingIdentity& identity : snapshot.poolIdentities) identityByAccountId.set(identity.accountIdHash, identity);
    for (const CodexRoutingIdentity& identity : cohort) {
      const CodexRoutingAccountPolicy* policy = snapshot.accountPolicies.get(identity.accountIdHash);
      if (policy == nullptr) return std::nullopt;
      CodexRoutingAccountEvaluation evaluated = native_codex_evaluation::evaluateCodexRoutingAccount(normalized, identity, model, observations, now, *policy);
      byAccountId.set(identity.accountIdHash, evaluated);
      native_codex_evaluation::foldCodexRoutingEvaluation(accumulator, evaluated);
    }
    const SerialRoutingEvaluations evaluations{
      .accumulated = std::move(accumulator),
      .byAccountId = std::move(byAccountId),
      .identityByAccountId = std::move(identityByAccountId),
    };
    const std::optional<SerialSelectionDecision> decision = serialSelectionDecision(active, cohort, evaluations, *snapshot.revisions.pool, now, std::nullopt, std::nullopt);
    if (!decision) return std::nullopt;
    return planCodexSerialCommit(snapshot.revisions, durableRow, normalized, *decision);
  }

  CodexResetElectionPlan planCodexResetElection(const CodexRoutingRowRevisions& checks, const CodexAccountRoutingState* durableRouting, const CodexAccountRoutingState& normalized,
                                                const CodexActiveAccountSelection& nextActive) {
    CodexResetElectionPlan plan{.checks = checks, .writes = {.routing = std::nullopt, .active = nextActive}};
    if (native_codex_evaluation::routingStateChangedByNormalization(durableRouting, normalized)) plan.writes.routing = normalized;
    return plan;
  }
}
