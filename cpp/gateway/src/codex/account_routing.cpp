// Codex account routing: probes, recovery, serial selection (remainder of the original module), split out of src/codex_account_routing.ts.
#include "uos/codex/account_routing.hpp"

#include "uos/authority/native_codex.hpp"
#include "uos/authority/native_codex_mutations.hpp"
#include "uos/codex/routing_evaluation.hpp"
#include "uos/codex/routing_serial.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"

#include <exception>
#include <utility>
#include <variant>

namespace uos::codex::account_routing {
  namespace {
    namespace routingDecisions = authority::native_codex_mutations;
    using ::uos::types::CodexAuthPoolState;
    using ::uos::types::CodexAuthState;
    using capacity_routing::CodexRoutingAccountIdentity;

    /**
     * A ledger record that predates later routing transitions cannot satisfy the
     * exact current-generation fence; the same proof the stale reconciliation is
     * about to apply is sufficient here.
     */
    bool staleResetRoutingIdentityCurrent(const json::value& value, const RoutingAccount& account, double quotaResetAtMs) {
      return routingDecisions::staleResetRoutingIdentityCurrent(value, nativeCodexAccountMetadata(account), quotaResetAtMs);
    }

    /** The same account keeps its active generation; an account change advances it. */
    double nextResetElectionGeneration(const std::optional<CodexActiveAccountSelection>& active, const RoutingAccount& candidate) {
      return routingDecisions::nextResetElectionGeneration(active, nativeCodexAccountMetadata(candidate));
    }

    /** The exact pool credential an elected slot must still carry. */
    bool routingAuthMatchesAccount(const CodexAuthState& currentAuth, const RoutingAccount& account) {
      return currentAuth.account_id == account.auth.account_id && currentAuth.access_token == account.auth.access_token &&
             currentAuth.refresh_token == account.auth.refresh_token;
    }

    /** The normalized slot an election candidate must still own, including its quota fence. */
    bool resetCandidateSlotMatches(const CodexRoutingSlot& currentSlot, const RoutingAccount& account, bool routingIdentityCurrent) {
      return routingDecisions::resetCandidateSlotMatches(currentSlot, nativeCodexAccountMetadata(account), routingIdentityCurrent);
    }

    // `isVersionstamp(entry.versionstamp)` over a `string | null` versionstamp.
    bool isVersionstampValue(const std::optional<std::string>& versionstamp) {
      const json::value value = versionstamp ? json::value(*versionstamp) : json::value(nullptr);
      return isVersionstamp(&value);
    }

    using Rows = routing_serial::ReadStrongCodexRoutingRowsResult;

    // `type CodexResetElectionPreparation`
    struct CodexResetElectionPreparation {
      CodexAccountRoutingState normalized;
      std::optional<CodexAccountRoutingState> routing;
      RoutingAccount routedCandidate;
      CodexActiveAccountSelection next;
    };

    /**
     * Prove the original active snapshot, exact credential, routing fence, and
     * complete exhausted cohort, then build the next active row. Normalization,
     * evaluation, and the row itself keep their separate clock reads.
     */
    rt::Task<std::optional<CodexResetElectionPreparation>> prepareCodexResetRecoveryElection(RoutingAccount account, double routingGeneration,
                                                                                            CodexActiveAccountSnapshot originalActive, double quotaResetAtMs,
                                                                                            bool staleVerified, Rows rows) {
      if (!isCodexActiveAccountSnapshotCurrent(rows.activeEntry.value, originalActive)) co_return std::nullopt;
      const std::optional<CodexActiveAccountSelection> active = originalActive.selection;
      const std::optional<CodexAuthPoolState> pool =
        rows.authEntry.value.is_null() ? std::nullopt : routing_evaluation::parseCodexAuthPoolSnapshot(rows.authEntry.value);
      const std::optional<std::string> poolVersionstamp = rows.authEntry.versionstamp;
      if (!pool || !isVersionstampValue(poolVersionstamp)) co_return std::nullopt;
      const CodexAuthState* currentAuthPointer = rt::at(pool->accounts, account.slot);
      if (currentAuthPointer == nullptr || !routingAuthMatchesAccount(*currentAuthPointer, account)) co_return std::nullopt;
      const CodexAuthState currentAuth = *currentAuthPointer;
      const std::optional<CodexAccountRoutingState> routing =
        rows.routingEntry.value.is_null() ? std::nullopt : parseCodexAccountRoutingState(rows.routingEntry.value);
      if (!rows.routingEntry.value.is_null() && !routing) co_return std::nullopt;
      rt::Promise<CodexAccountRoutingState> normalizing(normalizeRoutingState(routing, *pool, rt::dateNow(), true));
      const CodexAccountRoutingState normalized = co_await normalizing.wait();
      const CodexRoutingSlot* currentSlot = rt::at(normalized.slots, account.slot);
      const bool routingIdentityCurrent = staleVerified ? staleResetRoutingIdentityCurrent(rows.routingEntry.value, account, quotaResetAtMs)
                                                        : isCodexQuotaBlockFenceCurrent(rows.routingEntry.value, account, quotaResetAtMs, routingGeneration);
      if (currentSlot == nullptr || !resetCandidateSlotMatches(*currentSlot, account, routingIdentityCurrent)) co_return std::nullopt;
      const CodexRoutingSlot slot = *currentSlot;
      rt::Promise<CodexAuthPoolState> narrowing(routing_serial::selectedCodexSubscriptionPool(*pool));
      const CodexAuthPoolState selectedPool = co_await narrowing.wait();
      bool selected = false;
      for (const CodexAuthState& auth : selectedPool.accounts) selected = selected || auth.account_id == currentAuth.account_id;
      if (!selected) co_return std::nullopt;
      const std::optional<std::string> accountIdHash = slot.account_id_hash;
      if (!accountIdHash) co_return std::nullopt;
      const std::vector<CodexCapacityRoutingObservation> observations = parseStoredCapacityObservationStore(rows.capacityEntry.value);
      rt::Promise<routing_serial::SerialRoutingEvaluations> evaluating(
        routing_serial::evaluateSerialRoutingAccounts(normalized, selectedPool, account.requestedModel.toOptional(), observations, rt::dateNow(), *pool));
      const routing_serial::SerialRoutingEvaluations evaluations = co_await evaluating.wait();
      bool fullCohortExhausted = !selectedPool.accounts.empty();
      for (const CodexAuthState& auth : selectedPool.accounts) {
        const routing_evaluation::CodexRoutingAccountEvaluation* evaluation = evaluations.byAccountId.get(auth.account_id);
        fullCohortExhausted = fullCohortExhausted && evaluation != nullptr && evaluation->activeTransitionReason == std::optional<std::string>("quota_exhausted");
      }
      if (!fullCohortExhausted) co_return std::nullopt;
      RoutingAccount routedCandidate = account;
      routedCandidate.accountIdHash = *accountIdHash;
      routedCandidate.credentialVersion = slot.credential_version;
      routedCandidate.routingGeneration = slot.generation;
      const std::optional<CodexActiveAccountSelection> next = routing_serial::activeSelectionForAccount(
        routedCandidate, *poolVersionstamp, nextResetElectionGeneration(active, routedCandidate), rt::dateNow(), std::string("quota_exhausted"));
      if (!next) co_return std::nullopt;
      CodexResetElectionPreparation prepared{normalized, routing, routedCandidate, *next};
      co_return prepared;
    }

    // `RoutingAccount | "conflict"`: std::nullopt is "conflict".
    /** Commit the prepared election, writing routing before the active row. */
    rt::Task<std::optional<RoutingAccount>> commitCodexResetRecoveryElection(std::shared_ptr<rt::Kv> kv, Rows rows, CodexResetElectionPreparation prepared) {
      CodexRoutingRowRevisions revisions;
      revisions.active = rows.activeEntry.versionstamp;
      revisions.pool = rows.authEntry.versionstamp;
      revisions.routing = rows.routingEntry.versionstamp;
      revisions.capacity = rows.capacityEntry.versionstamp;
      const authority::native_codex::CodexResetElectionPlan plan =
        authority::native_codex::planCodexResetElection(revisions, prepared.routing ? &*prepared.routing : nullptr, prepared.normalized, prepared.next);
      const bool routingChanged = plan.writes.routing.has_value();
      std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(rows.activeEntry).check(rows.authEntry).check(rows.routingEntry).check(rows.capacityEntry);
      if (plan.writes.routing) operation->set(CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvValue(toJson(*plan.writes.routing)));
      operation->set(CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, rt::KvValue(toJson(plan.writes.active)));
      auto committing = operation->commit();
      const rt::KvCommitResult committed = co_await std::move(committing);
      if (!committed.ok) co_return std::nullopt;
      setRoutingStateCache(prepared.normalized, routingChanged ? std::optional<std::string>(committed.versionstamp) : rows.routingEntry.versionstamp);
      RoutingAccount elected = prepared.routedCandidate;
      elected.activeGeneration = prepared.next.generation;
      elected.activePoolVersionstamp = prepared.next.pool_versionstamp;
      elected.activeTransitionReason = rt::orNull(prepared.next.transition_reason);
      co_return elected;
    }

    /** The routing generation an admission refresh must bind to, or null when its slot cannot be proven. */
    // `routingValue: unknown`
    rt::Task<std::optional<double>> resolveAdmissionRefreshGeneration(RoutingAccount account, CodexAuthPoolState pool, json::value routingValue) {
      const std::optional<CodexAccountRoutingState> routing = parseCodexAccountRoutingState(routingValue);
      if (!routingValue.is_null() && !routing) co_return std::nullopt;
      std::optional<CodexAccountRoutingState> normalized;
      if (routing) {
        rt::Promise<CodexAccountRoutingState> normalizing(normalizeRoutingState(routing, pool, rt::dateNow(), true));
        normalized = co_await normalizing.wait();
      }
      co_return routingDecisions::admissionRefreshRoutingGeneration(normalized ? &*normalized : nullptr, nativeCodexAccountMetadata(account));
    }

    // `CodexAdmissionRefreshPreparation`: `{ kind: "current" } | { kind: "refresh"; next } | { kind: "reject" }`.
    struct CodexAdmissionRefreshReject {};
    using CodexAdmissionRefreshPreparation =
      std::variant<routingDecisions::CodexAdmissionRefreshPlanCurrent, routingDecisions::CodexAdmissionRefreshPlanRefresh, CodexAdmissionRefreshReject>;

    /**
     * Prove the admitted active identity and generation, then prepare a same-identity
     * fence refresh. A current row returns without any write; every conflict rejects
     * so the caller reloads all four rows.
     */
    rt::Task<CodexAdmissionRefreshPreparation> prepareCodexActiveAccountAdmissionRefresh(RoutingAccount account, Rows rows) {
      const std::optional<CodexActiveAccountSelection> active =
        rows.activeEntry.value.is_null() ? std::nullopt : parseCodexActiveAccountSelection(rows.activeEntry.value);
      if (!active) co_return CodexAdmissionRefreshReject{};
      if (!account.activeGeneration || active->generation != *account.activeGeneration) co_return CodexAdmissionRefreshReject{};
      if (active->account_id_hash != account.accountIdHash || active->slot != account.slot) co_return CodexAdmissionRefreshReject{};
      const std::optional<CodexAuthPoolState> pool =
        rows.authEntry.value.is_null() ? std::nullopt : routing_evaluation::parseCodexAuthPoolSnapshot(rows.authEntry.value);
      const std::optional<std::string> poolVersionstamp = rows.authEntry.versionstamp;
      if (!pool || !isVersionstampValue(poolVersionstamp)) co_return CodexAdmissionRefreshReject{};
      const CodexAuthState* currentAuth = rt::at(pool->accounts, account.slot);
      if (currentAuth == nullptr || !routingAuthMatchesAccount(*currentAuth, account)) co_return CodexAdmissionRefreshReject{};
      rt::Promise<std::optional<double>> resolving(resolveAdmissionRefreshGeneration(account, *pool, rows.routingEntry.value));
      const std::optional<double> routingGeneration = co_await resolving.wait();
      if (!routingGeneration) co_return CodexAdmissionRefreshReject{};
      const routingDecisions::CodexAdmissionRefreshPlan prepared =
        routingDecisions::prepareCodexAdmissionRefresh(*active, nativeCodexAccountMetadata(account), *poolVersionstamp, *routingGeneration);
      if (const auto* refresh = std::get_if<routingDecisions::CodexAdmissionRefreshPlanRefresh>(&prepared)) {
        routingDecisions::CodexAdmissionRefreshPlanRefresh stamped = *refresh;
        stamped.next.updated_at_ms = rt::dateNow();
        co_return stamped;
      }
      co_return std::get<routingDecisions::CodexAdmissionRefreshPlanCurrent>(prepared);
    }

    rt::Task<RouteSelection> selectCodexRoutingAccountsFromState(CodexAccountRoutingState state, CodexAuthPoolState pool, std::vector<CodexAuthState> orderedAccounts,
                                                                 double now, std::optional<std::string> model,
                                                                 std::vector<CodexCapacityRoutingObservation> capacityObservations) {
      std::vector<rt::Promise<CodexRoutingAccountIdentity>> pending;
      for (const CodexAuthState& auth : pool.accounts) pending.push_back(rt::Promise<CodexRoutingAccountIdentity>(routingAccountIdentity(auth)));
      rt::Promise<std::vector<CodexRoutingAccountIdentity>> resolving = rt::all(std::move(pending));
      const std::vector<CodexRoutingAccountIdentity> identities = co_await resolving.wait();
      rt::OrderedMap<std::string, routing_evaluation::CodexRoutingSlotIdentity> byId;
      for (std::size_t slot = 0; slot < pool.accounts.size(); ++slot) {
        const CodexRoutingAccountIdentity* identity = rt::at(identities, static_cast<double>(slot));
        if (identity == nullptr) continue;
        routing_evaluation::CodexRoutingSlotIdentity mapped;
        mapped.slot = static_cast<double>(slot);
        mapped.accountIdHash = identity->accountIdHash;
        mapped.credentialVersion = identity->credentialVersion;
        byId.set(pool.accounts[slot].account_id, mapped);
      }
      rt::OrderedMap<std::string, CodexCapacityRoutingObservation> observationsByAccount;
      for (const CodexCapacityRoutingObservation& observation : capacityObservations) observationsByAccount.set(observation.account_id_hash, observation);
      co_return routing_evaluation::classifyCodexRouteSelection(
        routing_evaluation::accumulateCodexRoutingAccounts(state, byId, orderedAccounts, model, observationsByAccount, now));
    }

    /** Strong-read recheck loop: three compare-and-set attempts, then give up. */
    rt::Task<bool> recheckCodexRoutingSlotFromKv(std::shared_ptr<rt::Kv> kv, double slotNumber, double recheckAtMs) {
      for (int attempt = 0; attempt < 3; attempt += 1) {
        const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
        auto reading = kv->get(CODEX_ACCOUNT_ROUTING_KV_KEY, strong);
        const rt::KvEntryMaybe<> entry = co_await std::move(reading);
        const std::optional<CodexAccountRoutingState> state = parseCodexAccountRoutingState(entry.value.toJson());
        if (!state) co_return false;
        const std::optional<routingDecisions::CodexLocatedRoutingSlot> located = routingDecisions::locateRoutingSlot(*state, slotNumber);
        if (!located) co_return false;
        if (!rt::truthy(located->slot.quota_blocked_until_ms)) {
          setRoutingStateCache(state, entry.versionstamp);
          co_return true;
        }
        const CodexAccountRoutingState next = withSlot(*state, located->index, routingDecisions::recheckSlotTransition(located->slot, recheckAtMs));
        std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        operation->check(entry).set(CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvValue(toJson(next)));
        auto committing = operation->commit();
        const rt::KvCommitResult committed = co_await std::move(committing);
        if (committed.ok) {
          setRoutingStateCache(next, committed.versionstamp);
          co_return true;
        }
      }
      co_return false;
    }

    /** Recheck against the cached record when KV is unavailable. */
    bool recheckCodexRoutingSlotLocally(double slotNumber, double recheckAtMs) {
      const std::optional<CodexAccountRoutingState> state = getCachedRoutingState();
      if (!state) return false;
      const std::optional<routingDecisions::CodexLocatedRoutingSlot> located = routingDecisions::locateRoutingSlot(*state, slotNumber);
      if (!located) return false;
      if (!rt::truthy(located->slot.quota_blocked_until_ms)) return true;
      setRoutingStateCache(withSlot(*state, located->index, routingDecisions::recheckSlotTransition(located->slot, recheckAtMs)));
      return true;
    }
  }

  bool isCodexActiveAccountSnapshotCurrent(const json::value& value, const CodexActiveAccountSnapshot& snapshot) {
    return routingDecisions::isCodexActiveAccountSnapshotCurrent(value, snapshot);
  }

  bool isCodexActiveAccountSnapshotCurrent(const json::value* value, const CodexActiveAccountSnapshot& snapshot) {
    return routingDecisions::isCodexActiveAccountSnapshotCurrent(value, snapshot);
  }

  rt::Task<std::optional<RoutingAccount>> electCodexResetRecoveryAccount(RoutingAccount account, CodexActiveAccountSnapshot originalActive, double quotaResetAtMs,
                                                                         bool staleVerified) {
    if (!account.routingGeneration || !isSafeMs(quotaResetAtMs)) co_return std::nullopt;
    const double routingGeneration = *account.routingGeneration;
    try {
      rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (!kv) co_return std::nullopt;
      for (int attempt = 0; attempt < 3; attempt += 1) {
        rt::Promise<Rows> reading(routing_serial::readStrongCodexRoutingRows(kv));
        const Rows rows = co_await reading.wait();
        rt::Promise<std::optional<CodexResetElectionPreparation>> preparing(
          prepareCodexResetRecoveryElection(account, routingGeneration, originalActive, quotaResetAtMs, staleVerified, rows));
        const std::optional<CodexResetElectionPreparation> prepared = co_await preparing.wait();
        if (!prepared) co_return std::nullopt;
        rt::Promise<std::optional<RoutingAccount>> committing(commitCodexResetRecoveryElection(kv, rows, *prepared));
        std::optional<RoutingAccount> elected = co_await committing.wait();
        if (!elected) continue;
        co_return elected;
      }
    } catch (...) {
      co_return std::nullopt;
    }
    co_return std::nullopt;
  }

  /**
   * Final pre-transport admission. The active identity and generation must still
   * match the admitted request; the request's own probe claim or same-identity
   * credential refresh may have advanced the credential/routing fences, so those
   * are refreshed atomically without changing the active identity or generation.
   * A concurrent active switch, pool rotation, or conflicting writer fails.
   */
  rt::Task<bool> refreshCodexActiveAccountAdmission(RoutingAccount account) {
    if (!account.activeGeneration || !rt::Number::isSafeInteger(*account.activeGeneration) || *account.activeGeneration < 1) co_return false;
    try {
      rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (!kv) co_return false;
      std::function<rt::Task<bool>()> action = [kv, account]() -> rt::Task<bool> {
        for (int attempt = 0; attempt < 3; attempt += 1) {
          rt::Promise<Rows> reading(routing_serial::readStrongCodexRoutingRows(kv));
          const Rows rows = co_await reading.wait();
          rt::Promise<CodexAdmissionRefreshPreparation> preparing(prepareCodexActiveAccountAdmissionRefresh(account, rows));
          const CodexAdmissionRefreshPreparation prepared = co_await preparing.wait();
          if (std::holds_alternative<CodexAdmissionRefreshReject>(prepared)) co_return false;
          if (std::holds_alternative<routingDecisions::CodexAdmissionRefreshPlanCurrent>(prepared)) co_return true;
          const CodexActiveAccountSelection& next = std::get<routingDecisions::CodexAdmissionRefreshPlanRefresh>(prepared).next;
          std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
          operation->check(rows.activeEntry).check(rows.authEntry).check(rows.routingEntry).check(rows.capacityEntry);
          operation->set(CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, rt::KvValue(toJson(next)));
          auto committing = operation->commit();
          const rt::KvCommitResult committed = co_await std::move(committing);
          if (!committed.ok) continue;
          co_return true;
        }
        co_return false;
      };
      rt::Promise<bool> admitting(routing_serial::withCodexAdmission<bool>(kv, std::move(action)));
      const bool admitted = co_await admitting.wait();
      co_return admitted;
    } catch (...) {
    }
    co_return false;
  }

  /**
   * Slot-scoped eligibility for the internal prompt-cache experiment and other
   * pinned callers that already chose their account. It hydrates the shared
   * routing/capacity caches and reconciles existing capacity observations exactly
   * as the pre-cutover helper did, so a following transition CAS is based on the
   * row this read returned. It never reads or writes the global active-selection
   * row; ordinary inference admissions must use `selectCodexRoutingAccountsStrong`.
   */
  rt::Task<RouteSelection> selectCodexRoutingAccounts(CodexAuthPoolState pool, std::vector<CodexAuthState> orderedAccounts) {
    return selectCodexRoutingAccounts(std::move(pool), std::move(orderedAccounts), rt::dateNow(), std::nullopt);
  }

  rt::Task<RouteSelection> selectCodexRoutingAccounts(CodexAuthPoolState pool, std::vector<CodexAuthState> orderedAccounts, double now) {
    return selectCodexRoutingAccounts(std::move(pool), std::move(orderedAccounts), now, std::nullopt);
  }

  rt::Task<RouteSelection> selectCodexRoutingAccounts(CodexAuthPoolState pool, std::vector<CodexAuthState> orderedAccounts, double now, std::optional<std::string> model) {
    rt::Promise<CodexAccountRoutingState> loading(loadCodexAccountRouting(pool));
    const CodexAccountRoutingState state = co_await loading.wait();
    rt::Promise<std::vector<CodexCapacityRoutingObservation>> observing(loadCodexCapacityRoutingObservations(pool));
    const std::vector<CodexCapacityRoutingObservation> observations = co_await observing.wait();
    rt::Promise<CodexAccountRoutingState> reconciling(reconcileCapacityRoutingState(state, observations, now, model));
    const CodexAccountRoutingState reconciled = co_await reconciling.wait();
    rt::Promise<RouteSelection> selecting(selectCodexRoutingAccountsFromState(reconciled, pool, orderedAccounts, now, model, observations));
    RouteSelection selection = co_await selecting.wait();
    co_return selection;
  }

  rt::Task<RouteSelection> selectCodexRoutingAccountsStrong(CodexAuthPoolState pool, std::vector<CodexAuthState> orderedAccounts) {
    return selectCodexRoutingAccountsStrong(std::move(pool), std::move(orderedAccounts), rt::dateNow(), std::nullopt);
  }

  rt::Task<RouteSelection> selectCodexRoutingAccountsStrong(CodexAuthPoolState pool, std::vector<CodexAuthState> orderedAccounts, double now) {
    return selectCodexRoutingAccountsStrong(std::move(pool), std::move(orderedAccounts), now, std::nullopt);
  }

  rt::Task<RouteSelection> selectCodexRoutingAccountsStrong(CodexAuthPoolState pool, std::vector<CodexAuthState> orderedAccounts, double now,
                                                            std::optional<std::string> model) {
    rt::Promise<RouteSelection> selecting(routing_serial::selectSerialCodexRoutingAccounts(std::move(pool), std::move(orderedAccounts), now, std::move(model)));
    RouteSelection selection = co_await selecting.wait();
    co_return selection;
  }

  /**
   * Makes a manually redeemed reset eligible for one normal-request probe.
   * This never clears reset-generation ambiguity: only a successful probe can
   * prove recovery and authorize a later provisional identity.
   */
  rt::Task<bool> recheckCodexRoutingSlot(double slotNumber) {
    if (!rt::Number::isInteger(slotNumber) || slotNumber < 1) co_return false;
    const double recheckAtMs = rt::dateNow();

    // Rechecks are rare, administrative transitions. Always use a strong read
    // when available so a cold isolate can release a persisted circuit and a
    // stale local cache cannot overwrite a newer quota deadline.
    rt::Promise<std::shared_ptr<rt::Kv>> opening(openRoutingKv());
    const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
    if (!kv) co_return recheckCodexRoutingSlotLocally(slotNumber, recheckAtMs);
    rt::Promise<bool> rechecking(recheckCodexRoutingSlotFromKv(kv, slotNumber, recheckAtMs));
    const bool rechecked = co_await rechecking.wait();
    co_return rechecked;
  }
}
