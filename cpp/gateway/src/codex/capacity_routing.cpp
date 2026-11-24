#include "uos/codex/capacity_routing.hpp"

#include "uos/authority/native_codex_evaluation.hpp"
#include "uos/codex/banked_reset.hpp"
#include "uos/kv.hpp"
#include "uos/provider/capacity_contract.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <exception>
#include <tuple>
#include <utility>

namespace uos::codex::capacity_routing {
  namespace {
    namespace capacityDecisions = authority::native_codex_capacity;
    using routing_state::CODEX_ACCOUNT_ROUTING_KV_KEY;
    using routing_state::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY;
    using routing_state::CodexAccountRoutingState;
    using routing_state::CodexCapacityRoutingAdditionalRateLimit;
    using routing_state::CodexCapacityRoutingObservation;
    using routing_state::CodexCapacityRoutingObservationInput;
    using routing_state::CodexCapacityRoutingWindow;
    using routing_state::CodexProbeCircuit;
    using routing_state::CodexRoutingIdentity;
    using routing_state::CodexRoutingSlot;
    using routing_state::isSafeMs;
    using routing_state::nativeCodexAccountMetadata;
    using routing_state::parseCodexAccountRoutingState;
    using routing_state::parseOptionalSafeMs;
    using routing_state::ROUTING_CACHE_REVALIDATE_MS;
    using routing_state::RoutingAccount;
    using routing_state::routingStateCache;

    rt::KvGetOptions strongRead() { return rt::KvGetOptions{.consistency = rt::KvConsistencyLevel::strong}; }

    rt::Task<std::string> codexRoutingAccountIdHashForId(std::string accountId) {
      rt::Promise<std::string> hashing(utils::sha256Hex(std::string("uos_ai") + '\0' + "codex_routing_account" + '\0' + accountId));
      std::string hash = co_await hashing.wait();
      co_return hash;
    }

    rt::Task<std::string> codexRoutingAccountIdHash(::uos::types::CodexAuthState auth) {
      rt::Promise<std::string> hashing(codexRoutingAccountIdHashForId(auth.account_id));
      std::string hash = co_await hashing.wait();
      co_return hash;
    }

    // `undefined` means this isolate only has synthesized/local state, so it
    // must refresh before attempting a durable compare-and-set. `null` is a real
    // KV versionstamp for an absent routing record and is safe to check directly.

    std::vector<CodexCapacityRoutingObservation> cachedCapacityObservations;
    double cachedCapacityObservationsLoadedAtMs = 0;

    // `withoutLegacyTimeoutCircuits(state) !== state`: the decision rewrites a slot exactly when it
    // carries a legacy timeout fence or timeout lease.
    bool hasLegacyTimeoutCircuit(const CodexAccountRoutingState& state) {
      for (const CodexRoutingSlot& slot : state.slots) {
        const bool hasTimeoutLease = slot.probe_lease && slot.probe_lease->circuit == "upstream_timeout";
        const bool timeoutFence = slot.upstream_timeout_blocked_until_ms.hasValue() && rt::truthy(*slot.upstream_timeout_blocked_until_ms);
        if (timeoutFence || hasTimeoutLease) return true;
      }
      return false;
    }

    /**
     * Commit a checked transition and refresh this isolate's cache. `null` means the
     * compare-and-set lost the race, so the caller must re-read the durable record
     * before retrying.
     */
    rt::Task<std::optional<CodexAccountRoutingState>> commitRoutingCandidate(std::unique_ptr<rt::AtomicOperation> operation, CodexAccountRoutingState next) {
      const rt::KvValue row(routing_state::toJson(next));
      operation->set(CODEX_ACCOUNT_ROUTING_KV_KEY, row);
      auto committing = operation->commit();
      const rt::KvCommitResult committed = co_await std::move(committing);
      if (!committed.ok) co_return std::nullopt;
      routingStateCache.state = next;
      routingStateCache.versionstamp = committed.versionstamp;
      routingStateCache.loadedAtMs = rt::dateNow();
      co_return next;
    }

    /** Every attempt re-reads the committed row before its compare-and-set. */
    rt::Task<std::optional<CodexAccountRoutingState>> commitRoutingTransitionWithStrongRead(std::shared_ptr<rt::Kv> kv, CodexRoutingTransform transform) {
      for (int attempt = 0; attempt < 3; attempt += 1) {
        auto reading = kv->get(CODEX_ACCOUNT_ROUTING_KV_KEY, strongRead());
        const rt::KvEntryMaybe<> entry = co_await std::move(reading);
        const std::optional<CodexAccountRoutingState> durable = parseCodexAccountRoutingState(entry.value.toJson());
        const std::optional<CodexAccountRoutingState> rawBase = durable ? durable : routingStateCache.state;
        const std::optional<CodexAccountRoutingState> base =
          rawBase ? std::optional<CodexAccountRoutingState>(capacityDecisions::withoutLegacyTimeoutCircuits(*rawBase)) : std::nullopt;
        if (!base) co_return std::nullopt;
        const std::optional<CodexAccountRoutingState> next = transform(*base);
        if (!next) {
          routingStateCache.state = base;
          routingStateCache.versionstamp = entry.versionstamp;
          routingStateCache.loadedAtMs = rt::dateNow();
          co_return base;
        }
        std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        operation->check(entry);
        rt::Promise<std::optional<CodexAccountRoutingState>> committing(commitRoutingCandidate(std::move(operation), *next));
        const std::optional<CodexAccountRoutingState> committed = co_await committing.wait();
        if (committed) co_return committed;
      }
      co_return std::nullopt;
    }

    json::value windowValue(const std::optional<CodexCapacityRoutingWindow>& window) {
      return window ? json::value(routing_state::toJson(*window)) : json::value(nullptr);
    }

    json::value additionalRateLimitsValue(const std::vector<CodexCapacityRoutingAdditionalRateLimit>& limits) {
      json::array out;
      for (const CodexCapacityRoutingAdditionalRateLimit& limit : limits) out.push_back(json::value(routing_state::toJson(limit)));
      return json::value(std::move(out));
    }

    rt::Task<std::optional<CodexCapacityRoutingObservation>> capacityObservationFromInput(CodexCapacityRoutingObservationInput input) {
      if (!rt::Number::isInteger(input.slot) || input.slot < 0 || input.slot > 1 || rt::trim(input.account_id).empty() || !isSafeMs(input.snapshot_at_ms))
        co_return std::nullopt;
      const json::value stateValue(input.state);
      const std::optional<std::string> state = capacityDecisions::capacityState(&stateValue);
      if (!state) co_return std::nullopt;
      const std::optional<double> sourceObservedAtMs =
        !input.source_observed_at_ms || isSafeMs(input.source_observed_at_ms) ? input.source_observed_at_ms : std::nullopt;
      rt::Promise<std::string> hashing(codexRoutingAccountIdHashForId(rt::trim(input.account_id)));
      const std::string accountIdHash = co_await hashing.wait();
      const json::value primary = windowValue(input.windows.primary);
      const json::value secondary = windowValue(input.windows.secondary);
      const json::value limits = additionalRateLimitsValue(input.additional_rate_limits);
      CodexCapacityRoutingObservation observation;
      observation.slot = input.slot;
      observation.account_id_hash = accountIdHash;
      observation.state = *state;
      observation.source_observed_at_ms = sourceObservedAtMs;
      observation.snapshot_at_ms = input.snapshot_at_ms;
      observation.windows.primary = capacityDecisions::normalizeCapacityWindow(&primary);
      observation.windows.secondary = capacityDecisions::normalizeCapacityWindow(&secondary);
      observation.additional_rate_limits = capacityDecisions::normalizeCapacityAdditionalRateLimits(&limits);
      co_return observation;
    }

    /**
     * One legacy snapshot source, re-expressed as an account-hash-bound observation.
     * A source without a usable state, window set or snapshot timestamp is dropped
     * rather than guessed at.
     */
    // `source: Record<string, unknown>`
    rt::Task<std::optional<CodexCapacityRoutingObservation>> legacyCapacityObservationForSlot(::uos::types::CodexAuthPoolState pool, json::value source, double slot,
                                                                                           double fallbackSnapshotAtMs) {
      const std::optional<std::string> state = capacityDecisions::capacityState(rt::get(source, "state"));
      const double sourceSnapshotAtMs = isSafeMs(rt::get(source, "snapshot_at_ms")) ? *rt::getNumber(source, "snapshot_at_ms") : fallbackSnapshotAtMs;
      const json::value* windows = utils::isRecord(rt::get(source, "windows")) ? rt::get(source, "windows") : nullptr;
      if (!state || windows == nullptr || !isSafeMs(sourceSnapshotAtMs)) co_return std::nullopt;
      const ::uos::types::CodexAuthState* slotAccount = rt::at(pool.accounts, slot);
      if (slotAccount == nullptr) co_return std::nullopt;
      rt::Promise<std::string> hashing(codexRoutingAccountIdHashForId(slotAccount->account_id));
      const std::string accountIdHash = co_await hashing.wait();
      CodexCapacityRoutingObservation observation;
      observation.slot = slot;
      observation.account_id_hash = accountIdHash;
      observation.state = *state;
      observation.source_observed_at_ms = parseOptionalSafeMs(rt::get(source, "source_observed_at_ms"));
      observation.snapshot_at_ms = sourceSnapshotAtMs;
      observation.windows.primary = capacityDecisions::normalizeCapacityWindow(rt::get(*windows, "primary"));
      observation.windows.secondary = capacityDecisions::normalizeCapacityWindow(rt::get(*windows, "secondary"));
      observation.additional_rate_limits = capacityDecisions::normalizeCapacityAdditionalRateLimits(rt::get(source, "additional_rate_limits"));
      co_return observation;
    }

    rt::Task<std::vector<CodexCapacityRoutingObservation>> readLegacyProviderCapacityObservations(::uos::types::CodexAuthPoolState pool, std::shared_ptr<rt::Kv> kv) {
      try {
        auto reading = kv->get(provider::capacity_contract::PROVIDER_CAPACITY_SNAPSHOT_KEY, strongRead());
        const rt::KvEntryMaybe<> entry = co_await std::move(reading);
        const json::value value = entry.value.toJson();
        const json::value* sources = rt::get(value, "sources");
        if (!utils::isRecord(value) || !isSafeMs(rt::get(value, "snapshot_at_ms")) || sources == nullptr || !sources->is_array())
          co_return std::vector<CodexCapacityRoutingObservation>{};
        // A slot-only snapshot is safe to reuse only when the auth pool existed no
        // later than the sample. New observations are account-hash bound; this
        // guard is the migration boundary for snapshots written before that key.
        const double snapshotAtMs = *rt::getNumber(value, "snapshot_at_ms");
        if (!isSafeMs(pool.updated_at_ms) || pool.updated_at_ms > snapshotAtMs) co_return std::vector<CodexCapacityRoutingObservation>{};
        std::vector<CodexCapacityRoutingObservation> observations;
        for (double slot = 0; slot < rt::Math::min(2.0, static_cast<double>(pool.accounts.size())); slot += 1) {
          const json::value* source = nullptr;
          for (const json::value& candidate : sources->get_array()) {
            if (utils::isRecord(candidate) && rt::getString(candidate, "source") == std::optional<std::string_view>("codex") &&
                rt::getNumber(candidate, "slot") == std::optional<double>(slot + 1)) {
              source = &candidate;
              break;
            }
          }
          if (!utils::isRecord(source)) continue;
          rt::Promise<std::optional<CodexCapacityRoutingObservation>> legacy(legacyCapacityObservationForSlot(pool, *source, slot, snapshotAtMs));
          std::optional<CodexCapacityRoutingObservation> observation = co_await legacy.wait();
          if (observation) observations.push_back(std::move(*observation));
        }
        co_return observations;
      } catch (...) {
      }
      co_return std::vector<CodexCapacityRoutingObservation>{};
    }

    /**
     * Persist the merged observation set with compare-and-set, re-reading the durable
     * store on every attempt. `null` means the merge could not be committed, so the
     * caller must not treat this isolate's view as durable.
     */
    rt::Task<std::optional<std::vector<CodexCapacityRoutingObservation>>> persistCapacityObservations(std::shared_ptr<rt::Kv> kv,
                                                                                                       std::vector<CodexCapacityRoutingObservation> observations,
                                                                                                       double observationNow) {
      for (int attempt = 0; attempt < 3; attempt += 1) {
        try {
          auto reading = kv->get(CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY, strongRead());
          const rt::KvEntryMaybe<> entry = co_await std::move(reading);
          const std::vector<CodexCapacityRoutingObservation> durable = capacityDecisions::parseStoredCapacityObservationStore(entry.value.toJson());
          const capacityDecisions::CodexCapacityObservationStorePlan plan =
            capacityDecisions::planCapacityObservationStore(entry.versionstamp, durable, observations, observationNow);
          const rt::KvValue row(capacityDecisions::toJson(plan.writes.capacity));
          std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
          operation->check(rt::AtomicCheck(entry.key, plan.checks.capacity)).set(CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY, row);
          auto committing = operation->commit();
          const rt::KvCommitResult committed = co_await std::move(committing);
          if (committed.ok) co_return plan.writes.capacity.observations;
        } catch (...) {
          co_return std::nullopt;
        }
      }
      co_return std::nullopt;
    }
  }

  rt::Task<CodexAccountRoutingState> normalizeRoutingState(std::optional<CodexAccountRoutingState> raw, ::uos::types::CodexAuthPoolState pool) {
    return normalizeRoutingState(std::move(raw), std::move(pool), rt::dateNow(), false);
  }

  rt::Task<CodexAccountRoutingState> normalizeRoutingState(std::optional<CodexAccountRoutingState> raw, ::uos::types::CodexAuthPoolState pool, double now,
                                                           bool discardLegacyTimeoutCircuit) {
    std::vector<rt::Promise<CodexRoutingAccountIdentity>> pending;
    for (const ::uos::types::CodexAuthState& account : pool.accounts) pending.push_back(rt::Promise<CodexRoutingAccountIdentity>(routingAccountIdentity(account)));
    rt::Promise<std::vector<CodexRoutingAccountIdentity>> resolving = rt::all(std::move(pending));
    const std::vector<CodexRoutingAccountIdentity> identities = co_await resolving.wait();
    std::vector<CodexRoutingIdentity> slotted;
    for (std::size_t slot = 0; slot < identities.size(); ++slot) {
      CodexRoutingIdentity identity;
      identity.slot = static_cast<double>(slot);
      identity.accountIdHash = identities[slot].accountIdHash;
      identity.credentialVersion = identities[slot].credentialVersion;
      slotted.push_back(std::move(identity));
    }
    co_return capacityDecisions::normalizeRoutingState(raw ? &*raw : nullptr, slotted, now, discardLegacyTimeoutCircuit);
  }

  CodexRoutingSlot slotFor(const CodexAccountRoutingState& state, const RoutingAccount& account) {
    return capacityDecisions::slotFor(state, nativeCodexAccountMetadata(account));
  }

  bool slotMatchesRoutingAccount(const CodexRoutingSlot& slot, const RoutingAccount& account) {
    return capacityDecisions::slotMatchesCodexRoutingAccount(slot, nativeCodexAccountMetadata(account));
  }

  CodexProbeCircuit routingProbeCircuit(const RoutingAccount& account) { return capacityDecisions::routingProbeCircuit(nativeCodexAccountMetadata(account)); }

  bool probeLeaseMatchesRoutingAccount(const CodexRoutingSlot& slot, const RoutingAccount& account) {
    return capacityDecisions::probeLeaseMatchesCodexRoutingAccount(slot, nativeCodexAccountMetadata(account));
  }

  CodexAccountRoutingState withSlot(const CodexAccountRoutingState& state, double index, const CodexRoutingSlot& slot) {
    return capacityDecisions::withSlot(state, index, slot, rt::dateNow());
  }

  rt::Task<std::string> codexCredentialVersion(::uos::types::CodexAuthState auth) {
    rt::Promise<std::string> hashing(utils::sha256Hex(auth.account_id + '\0' + auth.access_token + '\0' + auth.refresh_token));
    std::string version = co_await hashing.wait();
    co_return version;
  }

  rt::Task<CodexRoutingAccountIdentity> routingAccountIdentity(::uos::types::CodexAuthState auth) {
    rt::Promise<std::string> accountIdHash(codexRoutingAccountIdHash(auth));
    rt::Promise<std::string> credentialVersion(codexCredentialVersion(auth));
    auto both = rt::all(accountIdHash, credentialVersion);
    const std::tuple<std::string, std::string> values = co_await both.wait();
    CodexRoutingAccountIdentity identity;
    identity.accountIdHash = std::get<0>(values);
    identity.credentialVersion = std::get<1>(values);
    co_return identity;
  }

  void resetCodexAccountRoutingForTest() {
    routingStateCache.state = std::nullopt;
    routingStateCache.versionstamp = std::nullopt;
    routingStateCache.loadedAtMs = 0;
    cachedCapacityObservations = {};
    cachedCapacityObservationsLoadedAtMs = 0;
  }

  /** Loads routing alongside a cold auth read.  KV failure deliberately fails open. */
  rt::Task<CodexAccountRoutingState> loadCodexAccountRouting(::uos::types::CodexAuthPoolState pool) {
    if (routingStateCache.state && rt::dateNow() - routingStateCache.loadedAtMs < ROUTING_CACHE_REVALIDATE_MS) {
      rt::Promise<CodexAccountRoutingState> normalizing(normalizeRoutingState(routingStateCache.state, pool));
      const CodexAccountRoutingState normalized = co_await normalizing.wait();
      // `normalizeRoutingState` preserves matching slot references, so this is
      // a cheap way to spot an auth-pool rotation without persisting anything
      // until a later routing transition actually needs to write it.
      if (!routingStateCache.state) throw rt::TypeError("null is not an object (evaluating 'cached.banked_reset_legacy_identity_unresolved')");
      const CodexAccountRoutingState cached = *routingStateCache.state;
      const bool routingStateChanged = authority::native_codex_evaluation::routingStateChangedByNormalization(&cached, normalized);
      if (routingStateChanged) routingStateCache.state = normalized;
      co_return *routingStateCache.state;
    }
    try {
      rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (!kv) {
        rt::Promise<CodexAccountRoutingState> normalizing(normalizeRoutingState(routingStateCache.state, pool));
        const CodexAccountRoutingState normalized = co_await normalizing.wait();
        routingStateCache.state = normalized;
        routingStateCache.versionstamp = std::nullopt;
        routingStateCache.loadedAtMs = rt::dateNow();
        co_return normalized;
      }
      auto reading = kv->get(CODEX_ACCOUNT_ROUTING_KV_KEY, strongRead());
      const rt::KvEntryMaybe<> entry = co_await std::move(reading);
      std::optional<CodexAccountRoutingState> durable = parseCodexAccountRoutingState(entry.value.toJson());
      rt::Promise<CodexAccountRoutingState> normalizing(normalizeRoutingState(std::move(durable), pool, rt::dateNow(), true));
      const CodexAccountRoutingState normalized = co_await normalizing.wait();
      routingStateCache.state = normalized;
      routingStateCache.versionstamp = entry.versionstamp;
      routingStateCache.loadedAtMs = rt::dateNow();
      co_return *routingStateCache.state;
    } catch (...) {
    }
    rt::Promise<CodexAccountRoutingState> normalizing(normalizeRoutingState(routingStateCache.state, pool));
    const CodexAccountRoutingState normalized = co_await normalizing.wait();
    routingStateCache.state = normalized;
    routingStateCache.versionstamp = std::nullopt;
    routingStateCache.loadedAtMs = rt::dateNow();
    co_return normalized;
  }

  /**
   * Apply a small state transition against the latest durable record. This is
   * deliberately compare-and-set rather than a blind `set`: independent slot
   * transitions must not erase each other when different isolates observe
   * failures at the same time.
   */
  rt::Task<std::optional<CodexAccountRoutingState>> updateRoutingState(CodexRoutingTransform transform) {
    const auto applyLocally = [&transform]() -> std::optional<CodexAccountRoutingState> {
      if (!routingStateCache.state) return std::nullopt;
      const bool cleaned = hasLegacyTimeoutCircuit(*routingStateCache.state);
      const CodexAccountRoutingState current = capacityDecisions::withoutLegacyTimeoutCircuits(*routingStateCache.state);
      const std::optional<CodexAccountRoutingState> next = transform(current);
      if (next) {
        routingStateCache.state = next;
        // The local update was not committed, so its old versionstamp can no
        // longer safely authorize a future write.
        routingStateCache.versionstamp = std::nullopt;
        routingStateCache.loadedAtMs = rt::dateNow();
      } else if (cleaned) {
        routingStateCache.state = current;
        routingStateCache.versionstamp = std::nullopt;
        routingStateCache.loadedAtMs = rt::dateNow();
      }
      return next ? next : routingStateCache.state;
    };

    std::shared_ptr<rt::Kv> kv;
    bool opened = false;
    try {
      rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
      kv = co_await opening.wait();
      opened = true;
    } catch (...) {
    }
    if (!opened) co_return applyLocally();
    if (!kv) co_return applyLocally();

    // A cold auth hydration already obtained this record. Reuse that
    // versionstamp for the first transition instead of paying a second routing
    // read on a request that just received a 401/429. A concurrent writer makes
    // this CAS fail, after which the strong-read retry below preserves its work.
    const std::optional<std::string> cachedStamp = routingStateCache.versionstamp;
    if (routingStateCache.state && cachedStamp) {
      const CodexAccountRoutingState current = capacityDecisions::withoutLegacyTimeoutCircuits(*routingStateCache.state);
      const std::optional<CodexAccountRoutingState> next = transform(current);
      if (!next) {
        routingStateCache.state = current;
        co_return current;
      }
      bool failed = false;
      try {
        std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        operation->check(rt::AtomicCheck(CODEX_ACCOUNT_ROUTING_KV_KEY, cachedStamp));
        rt::Promise<std::optional<CodexAccountRoutingState>> committing(commitRoutingCandidate(std::move(operation), *next));
        const std::optional<CodexAccountRoutingState> committed = co_await committing.wait();
        if (committed) co_return committed;
      } catch (...) {
        failed = true;
      }
      if (failed) co_return applyLocally();
    }

    try {
      rt::Promise<std::optional<CodexAccountRoutingState>> retrying(commitRoutingTransitionWithStrongRead(kv, transform));
      const std::optional<CodexAccountRoutingState> result = co_await retrying.wait();
      co_return result;
    } catch (...) {
    }
    co_return applyLocally();
  }

  namespace {
    std::vector<double> previousResetDeadlines(const CodexRoutingSlot& slot, double resetAtMs) {
      std::vector<double> deadlines;
      const auto addDeadline = [&](const std::optional<double>& deadline, bool stable) {
        if (deadline && stable && *deadline != resetAtMs && std::find(deadlines.begin(), deadlines.end(), *deadline) == deadlines.end())
          deadlines.push_back(*deadline);
      };
      addDeadline(slot.observed_reset_at_ms, slot.observed_reset_at_is_stable);
      if (slot.quota_blocks_by_class) {
        for (const auto& [key, block] : *slot.quota_blocks_by_class) {
          addDeadline(block.observed_reset_at_ms, block.observed_reset_at_is_stable);
        }
      }
      return deadlines;
    }

    bool resetWasNeverSubmitted(const rt::KvEntryMaybe<>& entry, const std::string& accountHash, const std::string& generation) {
      if (entry.value.isNull()) return true;
      const json::value record = entry.value.toJson();
      const auto* submitted = rt::get(record, "submitted_at_ms");
      return rt::getNumber(record, "v") == 1 && rt::getString(record, "account_id_hash") == accountHash &&
             rt::getString(record, "quota_generation") == generation && rt::getString(record, "state") == "claimed" &&
             submitted != nullptr && submitted->is_null();
    }

    rt::Task<bool> checkPreviousResetWindows(std::shared_ptr<rt::Kv> kv, rt::AtomicOperation& operation,
                                             const std::string& accountId, const std::vector<double>& deadlines) {
      if (deadlines.empty()) co_return true;
      if (accountId.empty()) co_return false;
      const auto accountHash = co_await utils::sha256Hex(accountId);
      for (const double deadline : deadlines) {
        const auto hash = co_await utils::sha256Hex(banked_reset::codexResetQuotaGenerationInput(accountHash, deadline));
        const std::string generation = "v1:" + hash;
        auto reading = kv->get(banked_reset::codexResetRedemptionKey(accountHash, generation), strongRead());
        const auto entry = co_await std::move(reading);
        if (!resetWasNeverSubmitted(entry, accountHash, generation)) co_return false;
        operation.check(entry);
      }
      co_return true;
    }
  }

  rt::Task<bool> reconcileCapacityQuotaBlock(RoutingAccount account, CodexCapacityRoutingObservation observation) {
    return reconcileCapacityQuotaBlock(std::move(account), std::move(observation), rt::dateNow());
  }

  rt::Task<bool> reconcileCapacityQuotaBlock(RoutingAccount account, CodexCapacityRoutingObservation observation, double now) {
    const auto metadata = nativeCodexAccountMetadata(account);
    const auto resetAtMs = capacityDecisions::capacityExhaustedResetAtMs(observation, capacityDecisions::requestedModelView(account.requestedModel), now);
    if (!resetAtMs) co_return false;
    try {
      const auto kv = co_await openRoutingKv();
      if (!kv) co_return false;
      for (int attempt = 0; attempt < 3; ++attempt) {
        auto reading = kv->get(CODEX_ACCOUNT_ROUTING_KV_KEY, strongRead());
        const auto entry = co_await std::move(reading);
        const auto durable = parseCodexAccountRoutingState(entry.value.toJson());
        if (!durable) co_return false;
        const auto current = capacityDecisions::withoutLegacyTimeoutCircuits(*durable);
        const auto next = capacityDecisions::applyCapacityQuotaBlock(current, metadata, observation, now, rt::dateNow());
        if (!next) co_return false;
        const auto slot = capacityDecisions::slotFor(current, metadata);
        const auto deadlines = previousResetDeadlines(slot, *resetAtMs);
        auto operation = kv->atomic();
        operation->check(entry);
        // An upgrade may correct an earlier primary deadline to the weekly
        // deadline for the same provider spend, even after the first date passed.
        // The routing write and old ledger revision must move together.
        if (!(co_await checkPreviousResetWindows(kv, *operation, account.auth.account_id, deadlines))) co_return false;
        const auto committed = co_await commitRoutingCandidate(std::move(operation), *next);
        if (committed) co_return true;
      }
    } catch (...) {
    }
    co_return false;
  }

  /** Routing state reads and writes fail closed when the KV binding itself is unavailable. */
  rt::Task<std::shared_ptr<rt::Kv>> openRoutingKv() {
    try {
      rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
      std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      co_return kv;
    } catch (...) {
    }
    co_return nullptr;
  }

  rt::Task<std::vector<CodexCapacityRoutingObservation>> loadCodexCapacityRoutingObservations(::uos::types::CodexAuthPoolState pool, bool forceKv) {
    const double now = rt::dateNow();
    if (!forceKv && now - cachedCapacityObservationsLoadedAtMs < ROUTING_CACHE_REVALIDATE_MS) co_return cachedCapacityObservations;
    try {
      rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (!kv) {
        cachedCapacityObservationsLoadedAtMs = now;
        co_return cachedCapacityObservations;
      }
      rt::Promise<rt::KvEntryMaybe<>> observationRead(kv->get(CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY, strongRead()));
      rt::Promise<std::vector<CodexCapacityRoutingObservation>> legacyRead(readLegacyProviderCapacityObservations(pool, kv));
      auto both = rt::all(observationRead, legacyRead);
      const std::tuple<rt::KvEntryMaybe<>, std::vector<CodexCapacityRoutingObservation>> results = co_await both.wait();
      const std::vector<CodexCapacityRoutingObservation> durable = capacityDecisions::parseStoredCapacityObservationStore(std::get<0>(results).value.toJson());
      cachedCapacityObservations = capacityDecisions::mergeCapacityObservations(durable, std::get<1>(results));
      cachedCapacityObservationsLoadedAtMs = now;
      co_return cachedCapacityObservations;
    } catch (...) {
    }
    cachedCapacityObservationsLoadedAtMs = now;
    co_return cachedCapacityObservations;
  }

  /**
   * The slot after a positive capacity observation. Clearing the requested class
   * circuit keeps a pending reset claim or half-open lease intact, and only the
   * last remaining class circuit releases the slot-wide reset scope.
   */
  rt::Task<CodexAccountRoutingState> reconcileCapacityRoutingState(CodexAccountRoutingState state, std::vector<CodexCapacityRoutingObservation> observations, double now,
                                                                   std::optional<std::string> model) {
    CodexRoutingTransform transform = [&observations, now, &model](const CodexAccountRoutingState& base) -> std::optional<CodexAccountRoutingState> {
      bool changed = false;
      CodexAccountRoutingState result = base;
      for (const CodexCapacityRoutingObservation& observation : observations) {
        const std::optional<std::string_view> modelView = model ? std::optional<std::string_view>(*model) : std::nullopt;
        std::optional<CodexAccountRoutingState> applied = capacityDecisions::applyCapacityObservation(result, observation, now, modelView, rt::dateNow());
        if (applied) {
          result = std::move(*applied);
          changed = true;
        }
      }
      return changed ? std::optional<CodexAccountRoutingState>(std::move(result)) : std::nullopt;
    };
    rt::Promise<std::optional<CodexAccountRoutingState>> updating(updateRoutingState(std::move(transform)));
    const std::optional<CodexAccountRoutingState> next = co_await updating.wait();
    co_return next ? *next : state;
  }

  /**
   * Persist the redacted, account-hash-bound sampler result and immediately
   * reconcile any matching routing slot. A positive observation clears only an
   * older quota signal; a newer inference 429 remains authoritative.
   */
  rt::Task<void> recordCodexCapacityRoutingObservations(std::vector<CodexCapacityRoutingObservationInput> inputs) {
    return recordCodexCapacityRoutingObservations(std::move(inputs), rt::dateNow());
  }

  rt::Task<void> recordCodexCapacityRoutingObservations(std::vector<CodexCapacityRoutingObservationInput> inputs, double now) {
    std::vector<rt::Promise<std::optional<CodexCapacityRoutingObservation>>> pending;
    for (const CodexCapacityRoutingObservationInput& input : inputs)
      pending.push_back(rt::Promise<std::optional<CodexCapacityRoutingObservation>>(capacityObservationFromInput(input)));
    rt::Promise<std::vector<std::optional<CodexCapacityRoutingObservation>>> resolving = rt::all(std::move(pending));
    const std::vector<std::optional<CodexCapacityRoutingObservation>> candidates = co_await resolving.wait();
    std::vector<CodexCapacityRoutingObservation> observations;
    for (const std::optional<CodexCapacityRoutingObservation>& observation : candidates)
      if (observation) observations.push_back(*observation);
    if (observations.empty()) co_return;
    const double observationNow = isSafeMs(now) ? now : rt::dateNow();
    std::vector<CodexCapacityRoutingObservation> merged =
      capacityDecisions::retainRecentCapacityObservations(capacityDecisions::mergeCapacityObservations(cachedCapacityObservations, observations), observationNow);
    std::shared_ptr<rt::Kv> kv;
    try {
      rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
      kv = co_await opening.wait();
    } catch (...) {
      kv = nullptr;
    }
    if (kv) {
      rt::Promise<std::optional<std::vector<CodexCapacityRoutingObservation>>> persisting(persistCapacityObservations(kv, observations, observationNow));
      const std::optional<std::vector<CodexCapacityRoutingObservation>> persisted = co_await persisting.wait();
      if (!persisted) co_return;
      merged = *persisted;
      cachedCapacityObservations = merged;
      cachedCapacityObservationsLoadedAtMs = rt::dateNow();
    } else {
      cachedCapacityObservations = merged;
      cachedCapacityObservationsLoadedAtMs = rt::dateNow();
    }

    // The direct transition is useful for a dashboard refresh that runs before
    // the next inference request. Selection repeats this reconciliation and can
    // still override a stale local circuit if a concurrent CAS loses a race.
    if (routingStateCache.state) {
      rt::Promise<CodexAccountRoutingState> reconciling(reconcileCapacityRoutingState(*routingStateCache.state, merged, observationNow, std::nullopt));
      co_await reconciling.wait();
    }
  }
}
