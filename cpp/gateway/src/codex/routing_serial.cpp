// Codex serial admission selection, split out of src/codex_account_routing.ts.
#include "uos/codex/routing_serial.hpp"

#include "uos/authority/native_codex.hpp"
#include "uos/authority/native_codex_evaluation.hpp"
#include "uos/codex/capacity_routing.hpp"
#include "uos/kv.hpp"
#include "uos/models/codex_models_availability.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/weak.hpp"

#include <exception>
#include <unordered_set>
#include <utility>
#include <variant>

namespace uos::codex::routing_serial {
  namespace {
    namespace serialDecisions = authority::native_codex;
    namespace evaluationDecisions = authority::native_codex_evaluation;
    using routing_evaluation::CodexRoutingAccountEvaluation;
    using routing_evaluation::CodexRoutingAccumulator;
    using routing_evaluation::CodexRoutingSlotIdentity;
    using routing_state::CODEX_ACCOUNT_ROUTING_KV_KEY;
    using routing_state::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY;
    using routing_state::CODEX_AUTH_POOL_KV_KEY;
    using routing_state::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY;
    using routing_state::CodexAccountRoutingState;
    using routing_state::CodexActiveAccountSelection;
    using routing_state::CodexCapacityRoutingObservation;
    using routing_state::CodexRoutingIdentity;
    using routing_state::RouteSelection;
    using routing_state::RoutingAccount;
    using ::uos::types::CodexAuthPoolState;
    using ::uos::types::CodexAuthState;

    // `type SerialSelectionDecision = Readonly<{ selection: RouteSelection; nextActive; metadata }>`
    struct SerialSelectionDecision {
      RouteSelection selection;
      std::optional<CodexActiveAccountSelection> nextActive;
      serialDecisions::SerialSelectionDecision metadata;
    };

    std::optional<SerialSelectionDecision> serialSelectionDecision(const std::optional<CodexActiveAccountSelection>& active, const CodexAuthPoolState& pool,
                                                                   const SerialRoutingEvaluations& evaluations, const std::string& poolVersionstamp, double now,
                                                                   const std::string& poolSnapshotJson, const std::string& capacitySnapshotJson) {
      serialDecisions::SerialRoutingEvaluations metadataInput;
      for (const auto& [accountId, evaluation] : evaluations.byAccountId)
        metadataInput.byAccountId.set(evaluation.account.accountIdHash, routing_evaluation::codexEvaluationMetadata(evaluation));
      for (const auto& [accountId, identity] : evaluations.identityByAccountId) metadataInput.identityByAccountId.set(identity.accountIdHash, identity);
      evaluationDecisions::CodexRoutingAccumulator accumulated;
      accumulated.retryAt = std::nullopt;
      accumulated.hasQuotaBlock = false;
      accumulated.hasUpstreamTimeoutBlock = false;
      accumulated.modelUnavailable = std::nullopt;
      for (const auto& [accountIdHash, evaluation] : metadataInput.byAccountId) evaluationDecisions::foldCodexRoutingEvaluation(accumulated, evaluation);
      metadataInput.accumulated = accumulated;
      std::vector<CodexRoutingIdentity> cohort;
      for (const CodexAuthState& auth : pool.accounts) {
        const CodexRoutingSlotIdentity* identity = evaluations.identityByAccountId.get(auth.account_id);
        if (identity != nullptr) cohort.push_back(*identity);
      }
      std::optional<serialDecisions::SerialSelectionDecision> metadata =
        serialDecisions::serialSelectionDecision(active, cohort, metadataInput, poolVersionstamp, now, std::nullopt, std::nullopt);
      if (!metadata) return std::nullopt;
      std::vector<RoutingAccount> accounts;
      for (const auto& [accountId, evaluation] : evaluations.byAccountId) accounts.push_back(evaluation.account);
      RouteSelection selection = routing_evaluation::hydrateCodexRouteSelection(metadata->selection, accounts, poolSnapshotJson, capacitySnapshotJson);
      std::optional<CodexActiveAccountSelection> nextActive = metadata->nextActive;
      return SerialSelectionDecision{std::move(selection), std::move(nextActive), std::move(*metadata)};
    }

    // `type CodexSerialAdmissionRows`
    struct CodexSerialAdmissionRows {
      std::optional<CodexActiveAccountSelection> active;
      CodexAuthPoolState durablePool;
      std::optional<CodexAccountRoutingState> durableRouting;
      CodexAccountRoutingState normalized;
      std::string poolVersionstamp;
      SerialRoutingEvaluations evaluations;
      std::string poolSnapshotJson;
      std::string capacitySnapshotJson;
    };

    // `isVersionstamp(entry.versionstamp)` over a `string | null` versionstamp.
    bool isVersionstampValue(const std::optional<std::string>& versionstamp) {
      const json::value value = versionstamp ? json::value(*versionstamp) : json::value(nullptr);
      return routing_state::isVersionstamp(&value);
    }

    /**
     * Parse one strong-read snapshot. Any malformed or unavailable durable row
     * fails the admission before any decision or write.
     */
    rt::Task<std::optional<CodexSerialAdmissionRows>> prepareCodexSerialAdmissionRows(ReadStrongCodexRoutingRowsResult rows, double now,
                                                                                       std::optional<std::string> model) {
      const std::optional<CodexActiveAccountSelection> active =
        rows.activeEntry.value.is_null() ? std::nullopt : routing_state::parseCodexActiveAccountSelection(rows.activeEntry.value);
      if (!rows.activeEntry.value.is_null() && !active) co_return std::nullopt;
      const std::optional<CodexAuthPoolState> durablePool =
        rows.authEntry.value.is_null() ? std::nullopt : routing_evaluation::parseCodexAuthPoolSnapshot(rows.authEntry.value);
      const std::optional<std::string> poolVersionstamp = rows.authEntry.versionstamp;
      if (!durablePool || !isVersionstampValue(poolVersionstamp)) co_return std::nullopt;
      const std::optional<CodexAccountRoutingState> durableRouting =
        rows.routingEntry.value.is_null() ? std::nullopt : routing_state::parseCodexAccountRoutingState(rows.routingEntry.value);
      if (!rows.routingEntry.value.is_null() && !durableRouting) co_return std::nullopt;
      rt::Promise<CodexAccountRoutingState> normalizing(capacity_routing::normalizeRoutingState(durableRouting, *durablePool, now, true));
      const CodexAccountRoutingState normalized = co_await normalizing.wait();
      const std::vector<CodexCapacityRoutingObservation> observations = capacity_routing::parseStoredCapacityObservationStore(rows.capacityEntry.value);
      // The admission pool and the decision pool must be the same pool, or a
      // switched-off subscription could still be elected as the active account.
      // Slot identity stays the full pool's, so the narrowed cohort cannot
      // renumber the slot the decision commits and the final fence resolves.
      rt::Promise<CodexAuthPoolState> narrowing(selectedCodexSubscriptionPool(*durablePool));
      const CodexAuthPoolState routingPool = co_await narrowing.wait();
      rt::Promise<SerialRoutingEvaluations> evaluating(evaluateSerialRoutingAccounts(normalized, routingPool, model, observations, now, *durablePool));
      SerialRoutingEvaluations evaluations = co_await evaluating.wait();
      CodexSerialAdmissionRows prepared;
      prepared.active = active;
      prepared.durablePool = routingPool;
      prepared.durableRouting = durableRouting;
      prepared.normalized = normalized;
      prepared.poolVersionstamp = *poolVersionstamp;
      prepared.evaluations = std::move(evaluations);
      prepared.poolSnapshotJson = rt::jsonStringify(rows.authEntry.value);
      prepared.capacitySnapshotJson = rt::jsonStringify(rows.capacityEntry.value);
      co_return prepared;
    }

    /**
     * Commit one admission decision against the exact rows it was derived from.
     * The routing write precedes the active write, and the isolate cache is
     * refreshed only after a successful commit. A conflict reloads every row.
     */
    rt::Task<bool> commitCodexSerialAdmission(std::shared_ptr<rt::Kv> kv, ReadStrongCodexRoutingRowsResult rows, CodexSerialAdmissionRows prepared,
                                              SerialSelectionDecision decision) {
      routing_state::CodexRoutingRowRevisions revisions;
      revisions.active = rows.activeEntry.versionstamp;
      revisions.pool = rows.authEntry.versionstamp;
      revisions.routing = rows.routingEntry.versionstamp;
      revisions.capacity = rows.capacityEntry.versionstamp;
      const routing_state::CodexSerialAdmissionPlan plan =
        serialDecisions::planCodexSerialCommit(revisions, prepared.durableRouting ? &*prepared.durableRouting : nullptr, prepared.normalized, decision.metadata);
      std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(rt::AtomicCheck(rows.activeEntry.key, plan.checks.active))
        .check(rt::AtomicCheck(rows.authEntry.key, plan.checks.pool))
        .check(rt::AtomicCheck(rows.routingEntry.key, plan.checks.routing))
        .check(rt::AtomicCheck(rows.capacityEntry.key, plan.checks.capacity));
      if (plan.writes.routing) operation->set(CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvValue(routing_state::toJson(*plan.writes.routing)));
      if (plan.writes.active) operation->set(CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, rt::KvValue(routing_state::toJson(*plan.writes.active)));
      auto committing = operation->commit();
      const rt::KvCommitResult committed = co_await std::move(committing);
      if (!committed.ok) co_return false;
      routing_state::setRoutingStateCache(prepared.normalized, plan.writes.routing ? std::optional<std::string>(committed.versionstamp) : rows.routingEntry.versionstamp);
      co_return true;
    }

    /**
     * FIFO admission tail per KV instance. Concurrent routing admissions in one
     * isolate otherwise snapshot the same pre-bootstrap rows and exhaust their
     * three CAS attempts as conflicts, so only the short strong-selection and
     * same-identity fence decisions are serialized; auth refresh, transport,
     * inference, and streams stay concurrent. Stored promises are release-only and
     * never reject.
     */
    rt::WeakMap<rt::Kv, rt::Promise<void>> admissionTails;

    RouteSelection routingUnavailable() { return routing_state::RouteSelectionRoutingUnavailable{}; }
  }

  std::optional<CodexActiveAccountSelection> activeSelectionForAccount(const RoutingAccount& account, const std::string& poolVersionstamp, double generation, double now,
                                                                       const std::optional<routing_state::CodexActiveAccountTransitionReason>& transitionReason) {
    return serialDecisions::activeSelectionForAccount(routing_state::nativeCodexAccountMetadata(account), poolVersionstamp, generation, now, transitionReason);
  }

  rt::Task<SerialRoutingEvaluations> evaluateSerialRoutingAccounts(CodexAccountRoutingState state, CodexAuthPoolState pool, std::optional<std::string> model,
                                                                   std::vector<CodexCapacityRoutingObservation> capacityObservations, double now) {
    CodexAuthPoolState slotPool = pool;
    return evaluateSerialRoutingAccounts(std::move(state), std::move(pool), std::move(model), std::move(capacityObservations), now, std::move(slotPool));
  }

  /** Evaluate every configured account, preserving pool order without headroom ranking. */
  rt::Task<SerialRoutingEvaluations> evaluateSerialRoutingAccounts(CodexAccountRoutingState state, CodexAuthPoolState pool, std::optional<std::string> model,
                                                                   std::vector<CodexCapacityRoutingObservation> capacityObservations, double now,
                                                                   CodexAuthPoolState slotPool) {
    // Slot identity is a durable position in the full configured pool. Resolving
    // it from a narrowed admission cohort would renumber the surviving accounts,
    // commit an active row no full-pool slot owns, and fail the final fence.
    std::vector<rt::Promise<capacity_routing::CodexRoutingAccountIdentity>> pending;
    for (const CodexAuthState& auth : slotPool.accounts)
      pending.push_back(rt::Promise<capacity_routing::CodexRoutingAccountIdentity>(capacity_routing::routingAccountIdentity(auth)));
    rt::Promise<std::vector<capacity_routing::CodexRoutingAccountIdentity>> resolving = rt::all(std::move(pending));
    const std::vector<capacity_routing::CodexRoutingAccountIdentity> identities = co_await resolving.wait();
    rt::OrderedMap<std::string, CodexRoutingSlotIdentity> byId;
    for (std::size_t slot = 0; slot < slotPool.accounts.size(); ++slot) {
      const capacity_routing::CodexRoutingAccountIdentity* identity = rt::at(identities, static_cast<double>(slot));
      if (identity == nullptr) continue;
      CodexRoutingSlotIdentity mapped;
      mapped.slot = static_cast<double>(slot);
      mapped.accountIdHash = identity->accountIdHash;
      mapped.credentialVersion = identity->credentialVersion;
      byId.set(slotPool.accounts[slot].account_id, mapped);
    }
    rt::OrderedMap<std::string, CodexCapacityRoutingObservation> observationsByAccount;
    for (const CodexCapacityRoutingObservation& observation : capacityObservations) observationsByAccount.set(observation.account_id_hash, observation);
    CodexRoutingAccumulator accumulator;
    accumulator.retryAt = std::nullopt;
    accumulator.hasQuotaBlock = false;
    accumulator.hasUpstreamTimeoutBlock = false;
    accumulator.modelUnavailable = std::nullopt;
    rt::OrderedMap<std::string, CodexRoutingAccountEvaluation> byAccountId;
    std::vector<std::string> accountIds;
    for (const CodexAuthState& auth : pool.accounts) accountIds.push_back(auth.account_id);
    const rt::OrderedMap<std::string, std::optional<std::string>> unavailableAccounts = models::codex_models_availability::codexModelUnavailableAccounts(model, accountIds);
    for (const CodexAuthState& auth : pool.accounts) {
      const CodexRoutingSlotIdentity* mapped = byId.get(auth.account_id);
      if (mapped == nullptr) continue;
      CodexRoutingAccountEvaluation evaluated =
        routing_evaluation::evaluateCodexRoutingAccount(state, auth, *mapped, model, observationsByAccount, now, unavailableAccounts);
      if (evaluated.routedAccount && evaluated.routedAccount->quotaUsageProbeRequired) {
        const auto kv = co_await capacity_routing::openRoutingKv();
        const auto policy = co_await capacity_routing::readQuotaUsageProbePolicy(kv, auth.account_id);
        if (policy == false) evaluated.routedAccount->quotaUsageProbeRequired = false;
      }
      byAccountId.set(auth.account_id, evaluated);
      routing_evaluation::foldCodexRoutingEvaluation(accumulator, evaluated);
    }
    SerialRoutingEvaluations evaluations;
    evaluations.accumulated = accumulator;
    evaluations.byAccountId = std::move(byAccountId);
    evaluations.identityByAccountId = std::move(byId);
    co_return evaluations;
  }

  /** One coherent strong snapshot of the four rows every admission decision linearizes on, in a fixed order. */
  rt::Task<ReadStrongCodexRoutingRowsResult> readStrongCodexRoutingRows(std::shared_ptr<rt::Kv> kv) {
    std::vector<rt::KvKey> keys{CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, CODEX_AUTH_POOL_KV_KEY, CODEX_ACCOUNT_ROUTING_KV_KEY, CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY};
    const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
    auto reading = kv->getMany<json::value>(std::move(keys), strong);
    const std::vector<rt::KvEntryMaybe<json::value>> entries = co_await std::move(reading);
    ReadStrongCodexRoutingRowsResult rows{entries.at(0), entries.at(1), entries.at(2), entries.at(3)};
    co_return rows;
  }

  rt::Task<CodexAuthPoolState> selectedCodexSubscriptionPool(CodexAuthPoolState pool) {
    rt::Promise<std::optional<provider::selection::ProviderSelection>> loading(provider::selection::loadProviderSelectionCached());
    const std::optional<provider::selection::ProviderSelection> selection = co_await loading.wait();
    const provider::selection::CodexAccountEligibility eligibility = provider::selection::codexAccountEligibility(selection);
    if (std::holds_alternative<provider::selection::CodexAccountEligibilityAll>(eligibility)) co_return pool;
    if (std::holds_alternative<provider::selection::CodexAccountEligibilityNone>(eligibility)) {
      if (pool.accounts.empty()) co_return pool;
      CodexAuthPoolState none = pool;
      none.accounts.clear();
      co_return none;
    }
    const auto& only = std::get<provider::selection::CodexAccountEligibilityOnly>(eligibility);
    const std::unordered_set<std::string> enabled(only.hashes.begin(), only.hashes.end());
    std::vector<CodexAuthState> accounts;
    for (const CodexAuthState& account : pool.accounts) {
      rt::Promise<std::string> hashing(provider::selection::codexSubscriptionHash(account.account_id));
      const std::string hash = co_await hashing.wait();
      if (enabled.contains(hash)) accounts.push_back(account);
    }
    if (accounts.size() == pool.accounts.size()) co_return pool;
    CodexAuthPoolState narrowed = pool;
    narrowed.accounts = std::move(accounts);
    co_return narrowed;
  }

  template <class T> rt::Task<T> withCodexAdmission(std::shared_ptr<rt::Kv> kv, std::function<rt::Task<T>()> action) {
    const rt::Promise<void> priorAdmission = admissionTails.get(*kv).value_or(rt::Promise<void>::resolve());
    auto [admissionRelease, releaseAdmission, rejectAdmission] = rt::withResolvers<void>();
    (void)rejectAdmission;
    admissionTails.set(*kv, admissionRelease);
    rt::Reason failure;
    std::optional<T> result;
    try {
      co_await priorAdmission.wait();
      rt::Promise<T> acting(action());
      T value = co_await acting.wait();
      result.emplace(std::move(value));
    } catch (...) {
      failure = std::current_exception();
    }
    releaseAdmission();
    if (const rt::Promise<void>* tail = admissionTails.find(*kv); tail != nullptr && *tail == admissionRelease) admissionTails.delete_(*kv);
    if (failure) std::rethrow_exception(failure);
    co_return std::move(*result);
  }

  template rt::Task<RouteSelection> withCodexAdmission<RouteSelection>(std::shared_ptr<rt::Kv> kv, std::function<rt::Task<RouteSelection>()> action);
  template rt::Task<bool> withCodexAdmission<bool>(std::shared_ptr<rt::Kv> kv, std::function<rt::Task<bool>()> action);

  /**
   * Every ordinary admission strongly reads the active selection, auth pool,
   * and routing state. A missing active row is the one bootstrap case; any
   * malformed or unavailable durable state fails before upstream dispatch.
   */
  rt::Task<RouteSelection> selectSerialCodexRoutingAccounts(CodexAuthPoolState _suppliedPool, std::vector<CodexAuthState> _orderedAccounts, double now,
                                                            std::optional<std::string> model) {
    (void)_suppliedPool;
    (void)_orderedAccounts;
    try {
      rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (!kv) co_return routingUnavailable();
      std::function<rt::Task<RouteSelection>()> action = [kv, now, model]() -> rt::Task<RouteSelection> {
        for (int attempt = 0; attempt < 3; attempt += 1) {
          rt::Promise<ReadStrongCodexRoutingRowsResult> reading(readStrongCodexRoutingRows(kv));
          const ReadStrongCodexRoutingRowsResult rows = co_await reading.wait();
          rt::Promise<std::optional<CodexSerialAdmissionRows>> preparing(prepareCodexSerialAdmissionRows(rows, now, model));
          const std::optional<CodexSerialAdmissionRows> prepared = co_await preparing.wait();
          if (!prepared) co_return routingUnavailable();
          const std::optional<SerialSelectionDecision> decision = serialSelectionDecision(
            prepared->active, prepared->durablePool, prepared->evaluations, prepared->poolVersionstamp, now, prepared->poolSnapshotJson, prepared->capacitySnapshotJson);
          if (!decision) co_return routingUnavailable();
          rt::Promise<bool> committing(commitCodexSerialAdmission(kv, rows, *prepared, *decision));
          const bool committed = co_await committing.wait();
          if (!committed) continue;
          co_return decision->selection;
        }
        co_return routingUnavailable();
      };
      rt::Promise<RouteSelection> admitting(withCodexAdmission<RouteSelection>(kv, std::move(action)));
      RouteSelection selection = co_await admitting.wait();
      co_return selection;
    } catch (...) {
    }
    co_return routingUnavailable();
  }
}
