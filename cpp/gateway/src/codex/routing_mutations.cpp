// Codex routing account mutations and probe claiming, split out of src/codex_account_routing.ts.
#include "uos/codex/routing_mutations.hpp"

#include "uos/authority/native_codex_mutations.hpp"
#include "uos/codex/capacity_routing.hpp"
#include "uos/codex/rate_limit_429.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"

#include <utility>
#include <variant>

namespace uos::codex::routing_mutations {
  namespace {
    namespace routingDecisions = authority::native_codex_mutations;
    using authority::native_codex_429::Codex429Classification;
    using authority::native_codex_429::CodexKnownProbeLease;
    using routing_state::CODEX_ACCOUNT_ROUTING_KV_KEY;
    using routing_state::CODEX_HALF_OPEN_LEASE_MS;
    using routing_state::CodexAccountRoutingState;
    using routing_state::CodexRoutingSlot;
    using routing_state::isSafeMs;
    using routing_state::nativeCodexAccountMetadata;
    using routing_state::parseCodexAccountRoutingState;
    using routing_state::RoutingAccount;
    using routing_state::setRoutingStateCache;

    rt::KvGetOptions strongRead() { return rt::KvGetOptions{.consistency = rt::KvConsistencyLevel::strong}; }

    rt::Task<std::optional<std::vector<rt::KvEntryMaybe<>>>> readCurrentReconciliationFences(std::shared_ptr<rt::Kv> kv,
                                                                                           std::vector<CodexQuotaResetReconciliationFence> fences) {
      for (const CodexQuotaResetReconciliationFence& fence : fences)
        if (!fence.isCurrent) co_return std::nullopt;
      std::vector<rt::KvEntryMaybe<>> entries;
      for (const CodexQuotaResetReconciliationFence& fence : fences) {
        rt::KvEntryMaybe<> entry;
        try {
          auto reading = kv->get(fence.key, strongRead());
          entry = co_await std::move(reading);
          if (!fence.isCurrent(entry.value.toJson())) co_return std::nullopt;
        } catch (...) {
          co_return std::nullopt;
        }
        entries.push_back(std::move(entry));
      }
      co_return entries;
    }

    rt::AtomicOperation& withReconciliationFences(rt::AtomicOperation& operation, const std::vector<rt::KvEntryMaybe<>>& entries) {
      rt::AtomicOperation* next = &operation;
      for (const rt::KvEntryMaybe<>& entry : entries) next = &next->check(entry);
      return *next;
    }

    // `(state: CodexAccountRoutingState, current: CodexRoutingSlot, nowMs: number) => boolean`
    using CodexQuotaProbeEligibility = std::function<bool(const CodexAccountRoutingState& state, const CodexRoutingSlot& current, double nowMs)>;

    /** Apply the fenced half-open claim a verified reset just persisted. */
    std::optional<RoutingAccount> applyRecoveryProbeLease(const RoutingAccount& account, const CodexAccountRoutingState& next, const CodexKnownProbeLease& lease) {
      const CodexRoutingSlot* claimedSlot = rt::at(next.slots, account.slot);
      if (claimedSlot == nullptr) return std::nullopt;
      RoutingAccount applied = account;
      applied.quotaHeadroom = capacity_routing::quotaHeadroomFor(*claimedSlot);
      applied.probeRequired = false;
      applied.probeGeneration = lease.generation;
      applied.probeToken = lease.token;
      applied.probeCircuit = lease.circuit;
      return applied;
    }

    // `RoutingAccount | "retry" | null`: std::monostate is null.
    struct RecoveryRetry {};
    using RecoveryProbeAttempt = std::variant<std::monostate, RecoveryRetry, RoutingAccount>;

    /**
     * One strong-read compare-and-set attempt for a verified-reset recovery probe.
     * Returns "retry" when a concurrent writer won the CAS, and null when the
     * attempt must fail closed.
     */
    rt::Task<RecoveryProbeAttempt> attemptCodexQuotaRecoveryProbe(std::shared_ptr<rt::Kv> kv, RoutingAccount account, CodexQuotaResetReconciliationInput input,
                                                                  CodexQuotaProbeEligibility isEligible) {
      try {
        auto reading = kv->get(CODEX_ACCOUNT_ROUTING_KV_KEY, strongRead());
        const rt::KvEntryMaybe<> entry = co_await std::move(reading);
        const std::optional<CodexAccountRoutingState> state = parseCodexAccountRoutingState(entry.value.toJson());
        const CodexRoutingSlot* current = state ? rt::at(state->slots, account.slot) : nullptr;
        const double nowMs = rt::dateNow();
        if (!state || current == nullptr || !isEligible(*state, *current, nowMs)) co_return std::monostate{};
        std::optional<std::vector<rt::KvEntryMaybe<>>> fenceEntries = std::vector<rt::KvEntryMaybe<>>{};
        if (input.fences) {
          rt::Promise<std::optional<std::vector<rt::KvEntryMaybe<>>>> fencing(readCurrentReconciliationFences(kv, *input.fences));
          fenceEntries = co_await fencing.wait();
        }
        if (!fenceEntries) co_return std::monostate{};
        const double nextGeneration = current->generation + 1;
        const double probeExpiresAtMs = nowMs + CODEX_HALF_OPEN_LEASE_MS;
        if (!rt::Number::isSafeInteger(nextGeneration) || !isSafeMs(nowMs) || !isSafeMs(probeExpiresAtMs)) co_return std::monostate{};
        const routing_state::CodexQuotaClass requestedQuotaClass = capacity_routing::quotaClass(account.requestedModel);
        CodexKnownProbeLease recoveryLease;
        recoveryLease.token = rt::crypto::randomUUID();
        recoveryLease.expires_at_ms = probeExpiresAtMs;
        recoveryLease.generation = nextGeneration;
        recoveryLease.circuit = "quota";
        const auto scope = authority::native_codex_capacity::quotaBlockKeyForClass(
          *current, requestedQuotaClass, authority::native_codex_capacity::requestedModelView(account.requestedModel));
        recoveryLease.quota_class = scope && authority::native_codex_contracts::isCodexCapacityModelScope(*scope) ? *scope : requestedQuotaClass;
        const CodexAccountRoutingState next = routingDecisions::recoveryProbeTransition(*state, nativeCodexAccountMetadata(account), recoveryLease, rt::dateNow());
        const rt::KvValue row(routing_state::toJson(next));
        std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        withReconciliationFences(operation->check(entry), *fenceEntries).set(CODEX_ACCOUNT_ROUTING_KV_KEY, row);
        auto committing = operation->commit();
        const rt::KvCommitResult committed = co_await std::move(committing);
        if (!committed.ok) co_return RecoveryRetry{};
        setRoutingStateCache(next, committed.versionstamp);
        std::optional<RoutingAccount> applied = applyRecoveryProbeLease(account, next, recoveryLease);
        if (!applied) co_return std::monostate{};
        co_return std::move(*applied);
      } catch (...) {
      }
      co_return std::monostate{};
    }

    /**
     * Claim a fenced recovery probe after a verified reset. The eligibility
     * predicate is kept separate because stale ledger records need a different
     * proof than the current-generation fence, while the CAS/lease transition
     * must remain identical.
     */
    rt::Task<std::optional<RoutingAccount>> reconcileCodexQuotaAfterProbe(RoutingAccount account, CodexQuotaResetReconciliationInput input,
                                                                          CodexQuotaProbeEligibility isEligible) {
      if (!isSafeMs(input.quotaResetAtMs) || !rt::Number::isSafeInteger(input.routingGeneration) || input.routingGeneration < 0) co_return std::nullopt;
      rt::Promise<std::shared_ptr<rt::Kv>> opening(capacity_routing::openRoutingKv());
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (!kv) co_return std::nullopt;
      for (int attempt = 0; attempt < 3; attempt += 1) {
        rt::Promise<RecoveryProbeAttempt> attempting(attemptCodexQuotaRecoveryProbe(kv, account, input, isEligible));
        RecoveryProbeAttempt claimed = co_await attempting.wait();
        if (std::holds_alternative<std::monostate>(claimed)) co_return std::nullopt;
        if (RoutingAccount* routed = std::get_if<RoutingAccount>(&claimed)) co_return std::move(*routed);
      }
      co_return std::nullopt;
    }

    /** Adopt a lease this isolate just minted as the account's half-open claim. */
    RoutingAccount applyClaimedProbeLease(const RoutingAccount& account, const CodexKnownProbeLease& lease) {
      RoutingAccount applied = account;
      applied.probeRequired = false;
      applied.probeGeneration = lease.generation;
      applied.probeToken = lease.token;
      applied.probeCircuit = lease.circuit;
      return applied;
    }

    std::optional<routingDecisions::CodexExpiredProbeClaim> buildExpiredProbeClaim(const CodexAccountRoutingState& base, const RoutingAccount& account, double now) {
      const routing_state::CodexRoutingAccount metadata = nativeCodexAccountMetadata(account);
      if (!routingDecisions::buildExpiredProbeClaim(base, metadata, now, "eligibility-only", now)) return std::nullopt;
      return routingDecisions::buildExpiredProbeClaim(base, metadata, now, rt::crypto::randomUUID(), rt::dateNow());
    }

    /**
     * Claim locally. This retains availability when KV is unavailable, but not
     * cross-isolate coordination.
     */
    std::optional<RoutingAccount> applyLocalExpiredProbeClaim(const CodexAccountRoutingState& state, const RoutingAccount& account, double now) {
      const std::optional<routingDecisions::CodexExpiredProbeClaim> claimed = buildExpiredProbeClaim(state, account, now);
      if (!claimed) return std::nullopt;
      setRoutingStateCache(claimed->next);
      return applyClaimedProbeLease(account, claimed->lease);
    }

    rt::Task<std::optional<RoutingAccount>> claimExpiredProbeFromKv(std::shared_ptr<rt::Kv> kv, RoutingAccount account, CodexAccountRoutingState state, double now) {
      for (int attempt = 0; attempt < 3; attempt += 1) {
        auto reading = kv->get(CODEX_ACCOUNT_ROUTING_KV_KEY, strongRead());
        const rt::KvEntryMaybe<> entry = co_await std::move(reading);
        const std::optional<CodexAccountRoutingState> durable = parseCodexAccountRoutingState(entry.value.toJson());
        const std::optional<routingDecisions::CodexExpiredProbeClaim> claimed = buildExpiredProbeClaim(durable ? *durable : state, account, now);
        if (!claimed) co_return std::nullopt;
        const rt::KvValue row(routing_state::toJson(claimed->next));
        std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        operation->check(entry).set(CODEX_ACCOUNT_ROUTING_KV_KEY, row);
        auto committing = operation->commit();
        const rt::KvCommitResult commit = co_await std::move(committing);
        if (!commit.ok) continue;
        setRoutingStateCache(claimed->next, commit.versionstamp);
        co_return applyClaimedProbeLease(account, claimed->lease);
      }
      co_return std::nullopt;
    }

    rt::Task<std::optional<RoutingAccount>> claimExpiredProbe(CodexAccountRoutingState state, RoutingAccount account, double now) {
      try {
        rt::Promise<std::shared_ptr<rt::Kv>> opening(capacity_routing::openRoutingKv());
        const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
        if (kv == nullptr) co_return applyLocalExpiredProbeClaim(state, account, now);
        rt::Promise<std::optional<RoutingAccount>> claiming(claimExpiredProbeFromKv(kv, account, state, now));
        std::optional<RoutingAccount> claimed = co_await claiming.wait();
        co_return claimed;
      } catch (...) {
        // Fail open if KV itself is unavailable. This retains availability but not cross-isolate coordination.
      }
      co_return applyLocalExpiredProbeClaim(state, account, now);
    }
  }

  rt::Task<Codex429Classification> markCodexRecoveryProbeQuotaBlocked(RoutingAccount account, rt::Response response) {
    return markCodexRecoveryProbeQuotaBlocked(std::move(account), std::move(response), rt::dateNow());
  }

  rt::Task<Codex429Classification> markCodexRecoveryProbeQuotaBlocked(RoutingAccount account, rt::Response response, double now) {
    rt::Promise<Codex429Classification> marking(rate_limit_429::markCodexQuotaBlockedWithMode(std::move(account), std::move(response), now, true));
    const Codex429Classification parsed = co_await marking.wait();
    co_return parsed;
  }

  /**
   * Legacy transition retained for state-migration tests. Live inference no
   * longer calls it; normalized durable reads discard the resulting fence.
   */
  rt::Task<void> markCodexUpstreamTimeout(RoutingAccount account) { return markCodexUpstreamTimeout(std::move(account), rt::dateNow()); }

  rt::Task<void> markCodexUpstreamTimeout(RoutingAccount account, double now) {
    capacity_routing::CodexRoutingTransform transform = [&account, now](const CodexAccountRoutingState& state) {
      return routingDecisions::markCodexUpstreamTimeout(state, nativeCodexAccountMetadata(account), now, rt::dateNow());
    };
    rt::Promise<std::optional<CodexAccountRoutingState>> updating(capacity_routing::updateRoutingState(std::move(transform)));
    co_await updating.wait();
  }

  rt::Task<void> markCodexCredentialInvalid(RoutingAccount account) {
    const double now = rt::dateNow();
    capacity_routing::CodexRoutingTransform transform = [&account, now](const CodexAccountRoutingState& state) {
      return routingDecisions::markCodexCredentialInvalid(state, nativeCodexAccountMetadata(account), now, rt::dateNow());
    };
    rt::Promise<std::optional<CodexAccountRoutingState>> updating(capacity_routing::updateRoutingState(std::move(transform)));
    co_await updating.wait();
  }

  /**
   * OAuth refresh can replace a token while an inference attempt is in flight.
   * Treat the token as a new credential version while retaining the account's
   * stable reset observation, so a refresh cannot manufacture a second key.
   */
  rt::Task<RoutingAccount> reconcileCodexRoutingAccount(RoutingAccount account, ::uos::types::CodexAuthState auth) {
    rt::Promise<capacity_routing::CodexRoutingAccountIdentity> identifying(capacity_routing::routingAccountIdentity(auth));
    const capacity_routing::CodexRoutingAccountIdentity identity = co_await identifying.wait();
    const std::string& accountIdHash = identity.accountIdHash;
    const std::string& credentialVersion = identity.credentialVersion;
    // The normal refresh check can return unchanged credentials. Preserve any
    // half-open probe fence so a successful response can clear the quota
    // circuit claimed for this request.
    if (credentialVersion == account.credentialVersion) {
      RoutingAccount unchanged = account;
      unchanged.auth = auth;
      unchanged.accountIdHash = accountIdHash;
      co_return unchanged;
    }

    RoutingAccount reconciled = account;
    reconciled.auth = auth;
    reconciled.accountIdHash = accountIdHash;
    reconciled.credentialVersion = credentialVersion;
    reconciled.probeRequired = false;
    reconciled.probeGeneration = std::nullopt;
    reconciled.probeToken = std::nullopt;
    reconciled.probeCircuit = nullptr;

    // Credential rotation is exceptional. For the same account it releases
    // ordinary routing but retains a stable reset identity as lookup-only; only
    // a genuinely different account starts a neutral reset scope.
    const bool sameAccount = auth.account_id == account.auth.account_id;
    capacity_routing::CodexRoutingTransform transform = [&account, &identity, sameAccount](const CodexAccountRoutingState& state) {
      return routingDecisions::reconcileRoutingCredential(state, nativeCodexAccountMetadata(account), identity, sameAccount, rt::dateNow());
    };
    rt::Promise<std::optional<CodexAccountRoutingState>> updating(capacity_routing::updateRoutingState(std::move(transform)));
    const std::optional<CodexAccountRoutingState> nextState = co_await updating.wait();
    const CodexRoutingSlot* nextSlot = nextState ? rt::at(nextState->slots, account.slot) : nullptr;
    const std::optional<routing_state::CodexProbeLease> nextProbeLease = nextSlot != nullptr ? nextSlot->probe_lease : std::nullopt;
    const bool retainedProbe = auth.account_id == account.auth.account_id && account.probeGeneration.has_value() && account.probeToken.has_value() &&
                               nextSlot != nullptr && nextSlot->credential_version == credentialVersion && nextProbeLease.has_value() &&
                               nextProbeLease->token == *account.probeToken && nextProbeLease->circuit == capacity_routing::routingProbeCircuit(account) &&
                               nextProbeLease->generation == nextSlot->generation;
    if (!retainedProbe) co_return reconciled;
    RoutingAccount retained = reconciled;
    retained.probeGeneration = nextSlot->generation;
    retained.probeToken = nextProbeLease->token;
    retained.probeCircuit = nextProbeLease->circuit;
    co_return retained;
  }

  rt::Task<void> releaseCodexRoutingProbe(RoutingAccount account) {
    if (!account.probeGeneration || !rt::truthy(account.probeToken)) co_return;
    capacity_routing::CodexRoutingTransform transform = [&account](const CodexAccountRoutingState& state) {
      return routingDecisions::releaseCodexRoutingProbe(state, nativeCodexAccountMetadata(account), rt::dateNow());
    };
    rt::Promise<std::optional<CodexAccountRoutingState>> updating(capacity_routing::updateRoutingState(std::move(transform)));
    co_await updating.wait();
  }

  /** A successful recovery probe is the only trusted way to clear reset ambiguity. */
  rt::Task<void> markCodexSuccess(RoutingAccount account) {
    if (!account.probeGeneration || !rt::truthy(account.probeToken)) co_return;
    capacity_routing::CodexRoutingTransform transform = [&account](const CodexAccountRoutingState& state) {
      return routingDecisions::markCodexSuccess(state, nativeCodexAccountMetadata(account), rt::dateNow());
    };
    rt::Promise<std::optional<CodexAccountRoutingState>> updating(capacity_routing::updateRoutingState(std::move(transform)));
    co_await updating.wait();
  }

  /**
   * Pure exact-fence predicate used by both strong-read helpers and the banked
   * reset's atomic KV checks. Equality (rather than `>=`) prevents a shorter
   * later Retry-After from authorizing a reset against a longer existing block;
   * an unresolved legacy account association denies every new claim.
   */
  bool isCodexQuotaBlockFenceCurrent(const json::value& value, const RoutingAccount& account, double quotaResetAtMs, double routingGeneration) {
    return routingDecisions::isCodexQuotaBlockFenceCurrent(value, nativeCodexAccountMetadata(account), quotaResetAtMs, routingGeneration);
  }

  bool isCodexQuotaBlockFenceCurrent(const json::value* value, const RoutingAccount& account, double quotaResetAtMs, double routingGeneration) {
    return routingDecisions::isCodexQuotaBlockFenceCurrent(value, nativeCodexAccountMetadata(account), quotaResetAtMs, routingGeneration);
  }

  /**
   * Returns a durable routing fence for a fully persisted quota observation.
   * Account routing otherwise fails open when KV is down; banked redemption is
   * stricter and must refuse to start without this strong-read proof.
   */
  rt::Task<std::optional<double>> getCodexQuotaBlockFence(RoutingAccount account, double quotaResetAtMs) {
    if (!isSafeMs(quotaResetAtMs)) co_return std::nullopt;
    std::shared_ptr<rt::Kv> kv;
    bool opened = false;
    try {
      rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
      kv = co_await opening.wait();
      opened = true;
    } catch (...) {
    }
    if (!opened) co_return std::nullopt;
    if (!kv) co_return std::nullopt;
    try {
      auto reading = kv->get(CODEX_ACCOUNT_ROUTING_KV_KEY, strongRead());
      const rt::KvEntryMaybe<> entry = co_await std::move(reading);
      const json::value value = entry.value.toJson();
      const std::optional<CodexAccountRoutingState> state = parseCodexAccountRoutingState(value);
      const CodexRoutingSlot* slot = state ? rt::element(state->slots, account.slot) : nullptr;
      const std::optional<double> generation = slot != nullptr ? std::optional<double>(slot->generation) : std::nullopt;
      if (generation && isCodexQuotaBlockFenceCurrent(value, account, quotaResetAtMs, *generation)) co_return generation;
      co_return std::nullopt;
    } catch (...) {
    }
    co_return std::nullopt;
  }

  /**
   * A verified reset may release only the exact circuit observation that caused
   * it. Credential and generation checks prevent an old provider transaction
   * from admitting a newly rotated credential or a later quota window.
   */
  rt::Task<std::optional<RoutingAccount>> reconcileCodexQuotaAfterVerifiedReset(RoutingAccount account, CodexQuotaResetReconciliationInput input) {
    CodexQuotaProbeEligibility isEligible = [account, input](const CodexAccountRoutingState& state, const CodexRoutingSlot&, double) {
      return isCodexQuotaBlockFenceCurrent(json::value(routing_state::toJson(state)), account, input.quotaResetAtMs, input.routingGeneration);
    };
    rt::Promise<std::optional<RoutingAccount>> reconciling(reconcileCodexQuotaAfterProbe(account, input, std::move(isEligible)));
    std::optional<RoutingAccount> reconciled = co_await reconciling.wait();
    co_return reconciled;
  }

  /**
   * Recover an account whose verified reset record predates later routing
   * transitions. The ledger proves the reset was already spent, so this path
   * only fences a new inference probe; it never submits another reset.
   */
  rt::Task<std::optional<RoutingAccount>> reconcileCodexQuotaAfterStaleVerifiedReset(RoutingAccount account, CodexQuotaResetReconciliationInput input) {
    CodexQuotaProbeEligibility isEligible = [account, input](const CodexAccountRoutingState& state, const CodexRoutingSlot& current, double nowMs) {
      const routingDecisions::CodexStaleVerifiedResetProbeInput stale{input.quotaResetAtMs, input.routingGeneration};
      return routingDecisions::staleVerifiedResetProbeEligible(state, current, nativeCodexAccountMetadata(account), stale, nowMs);
    };
    rt::Promise<std::optional<RoutingAccount>> reconciling(reconcileCodexQuotaAfterProbe(account, input, std::move(isEligible)));
    std::optional<RoutingAccount> reconciled = co_await reconciling.wait();
    co_return reconciled;
  }

  rt::Task<std::optional<RoutingAccount>> claimCodexRoutingProbe(::uos::types::CodexAuthPoolState pool, RoutingAccount account) {
    return claimCodexRoutingProbe(std::move(pool), std::move(account), rt::dateNow());
  }

  rt::Task<std::optional<RoutingAccount>> claimCodexRoutingProbe(::uos::types::CodexAuthPoolState pool, RoutingAccount account, double now) {
    if (!account.probeRequired) co_return account;
    rt::Promise<CodexAccountRoutingState> loading(capacity_routing::loadCodexAccountRouting(pool));
    const CodexAccountRoutingState state = co_await loading.wait();
    rt::Promise<std::optional<RoutingAccount>> claiming(claimExpiredProbe(state, account, now));
    std::optional<RoutingAccount> claimed = co_await claiming.wait();
    co_return claimed;
  }
}
