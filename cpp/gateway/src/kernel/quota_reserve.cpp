#include "uos/kernel/quota_reserve.hpp"

#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"

#include <exception>
#include <functional>
#include <string_view>
#include <utility>
#include <variant>

namespace uos::kernel::quota_reserve {
  namespace {
    namespace contracts = authority::native_kernel_contracts;
    namespace native_kernel = authority::native_kernel;
    using contracts::KernelQuotaScope;
    using contracts::KernelQuotaWindowV2;
    using quota_reservations::kernelQuotaUnavailable;
    using quota_v2::KernelQuotaReservationDecision;

    // `{ consistency: "strong" }`
    const rt::KvGetOptions STRONG{.consistency = rt::KvConsistencyLevel::strong};

    // `kernelLimitBlocked(message, code = "rate_limit_exceeded")`
    KernelQuotaReservationDecision kernelLimitBlocked(const std::string& message, const std::string& code = "rate_limit_exceeded") {
      return quota_v2::KernelQuotaReservationDecisionNotOk{.response = http::openaiError(429, message, code)};
    }

    KernelQuotaReservationDecision quotaExceeded(const KernelQuotaWindowV2& window, double limit) {
      const double admitted = window.usage_requests + window.reserved_requests;
      return kernelLimitBlocked("Usage limit exceeded (" + rt::numberToString(admitted) + "/" + rt::numberToString(limit) + "). Resets at " +
                                rt::Date(window.usage_reset_at_ms).toISOString());
    }

    struct KernelQuotaReserveOptions {
      std::optional<std::shared_ptr<rt::Kv>> kv{};
      std::optional<double> nowMs{};
      std::optional<double> renewalIntervalMs{};
      std::optional<std::string> expectedRepoPolicy{};  // "present" | "absent"
      std::optional<std::string> guardedRepo{};
    };

    /** Every entry one reservation attempt compares against. */
    struct KernelQuotaReservationEntries {
      quota_v2::KernelDefaults defaults;
      rt::KvEntryMaybe<> policyEntry;
      rt::KvEntryMaybe<> windowEntry;
      std::optional<rt::KvEntryMaybe<>> repoGuardEntry;  // null when the attempt reads no repo guard
      rt::KvEntryMaybe<> defaultCutoverEntry;
      rt::KvKey windowKey;
    };

    /** The loaded entries together with the policy and window this attempt reserves against. */
    struct KernelQuotaReservationTarget : KernelQuotaReservationEntries {
      contracts::KernelQuotaEffectivePolicy effective;
      contracts::KernelQuotaEffectiveWindow resolved;
    };

    // `Readonly<{ kind: "decision"; decision }> | Readonly<{ kind: "eligible"; target }>`
    struct KernelQuotaEligibilityDecision {
      [[maybe_unused]] static constexpr std::string_view kind = "decision";
      KernelQuotaReservationDecision decision;
    };
    struct KernelQuotaEligibilityEligible {
      [[maybe_unused]] static constexpr std::string_view kind = "eligible";
      KernelQuotaReservationTarget target;
    };
    using KernelQuotaEligibility = std::variant<KernelQuotaEligibilityDecision, KernelQuotaEligibilityEligible>;

    // `reclaimedCapacity` distinguishes a reclaimed expired lease from a lost
    // compare-and-set: only a reclaim must stop the next attempt from reclaiming twice.
    struct KernelQuotaCapacityOutcomeDecision {
      [[maybe_unused]] static constexpr std::string_view kind = "decision";
      KernelQuotaReservationDecision decision;
    };
    struct KernelQuotaCapacityOutcomeRetry {
      [[maybe_unused]] static constexpr std::string_view kind = "retry";
      bool reclaimedCapacity;
    };
    using KernelQuotaCapacityOutcome = std::variant<KernelQuotaCapacityOutcomeDecision, KernelQuotaCapacityOutcomeRetry>;

    // The repo-guard read of loadKernelQuotaReservationEntries (null when it is not read).
    rt::Task<std::optional<rt::KvEntryMaybe<>>> readRepoGuard(std::shared_ptr<rt::Kv> kv, rt::KvKey key) {
      rt::KvEntryMaybe<> entry = co_await kv->get(std::move(key), STRONG);
      co_return entry;
    }

    /** Loads every entry one reservation attempt compares against. */
    rt::Task<KernelQuotaReservationEntries> loadKernelQuotaReservationEntries(std::shared_ptr<rt::Kv> kv, native_kernel::KernelPolicyKeys keys, std::string owner,
                                                                              KernelQuotaReserveOptions options) {
      rt::Promise<quota_v2::KernelDefaults> defaultsRead(quota_v2::loadDefaults(kv));
      rt::Promise<rt::KvEntryMaybe<>> policyRead(kv->get(keys.policyKey, STRONG));
      rt::Promise<rt::KvEntryMaybe<>> windowRead(kv->get(keys.windowKey, STRONG));
      rt::Promise<std::optional<rt::KvEntryMaybe<>>> repoGuardRead =
        options.expectedRepoPolicy == "absent" && rt::truthy(options.guardedRepo)
          ? rt::Promise<std::optional<rt::KvEntryMaybe<>>>(readRepoGuard(kv, native_kernel::kernelRepoPolicyKey(owner, *options.guardedRepo)))
          : rt::Promise<std::optional<rt::KvEntryMaybe<>>>::resolve(std::nullopt);
      rt::Promise<rt::KvEntryMaybe<>> cutoverRead(kv->get(native_kernel::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY, STRONG));
      auto reads = rt::all(defaultsRead, policyRead, windowRead, repoGuardRead, cutoverRead);
      auto readEntries = co_await reads.wait();
      auto& [defaults, policyEntry, windowEntry, repoGuardEntry, defaultCutoverEntry] = readEntries;
      KernelQuotaReservationEntries entries{
        .defaults = std::move(defaults),
        .policyEntry = std::move(policyEntry),
        .windowEntry = std::move(windowEntry),
        .repoGuardEntry = std::move(repoGuardEntry),
        .defaultCutoverEntry = std::move(defaultCutoverEntry),
        .windowKey = keys.windowKey,
      };
      co_return entries;
    }

    /**
     * Applies the policy scope, cutover, expiry, and aggregate guards. Returns the
     * effective policy and resolved window when the request may reserve capacity.
     */
    rt::Task<KernelQuotaEligibility> evaluateKernelQuotaEligibility(std::shared_ptr<rt::Kv> kv, KernelQuotaReservationEntries entries, KernelQuotaScope scope,
                                                                    std::string owner, std::optional<std::string> repo, KernelQuotaReserveOptions options, double nowMs) {
      const contracts::KernelQuotaEligibilitySnapshot snapshot{
        .policy = quota_v2::kernelQuotaRead(entries.policyEntry),
        .repoGuard = entries.repoGuardEntry ? std::optional<contracts::KernelQuotaRead<>>(quota_v2::kernelQuotaRead(*entries.repoGuardEntry)) : std::nullopt,
        .defaultCutover = quota_v2::kernelQuotaRead(entries.defaultCutoverEntry),
        .expectedRepoPolicy = options.expectedRepoPolicy,
      };
      const contracts::KernelQuotaIdentity identity{.scope = scope, .owner = owner, .repo = repo};
      const contracts::KernelQuotaEligibilityPlan plan =
        native_kernel::decideKernelQuotaEligibility(identity, snapshot, entries.defaults, quota_v2::KERNEL_QUOTA_CONSTANTS, nowMs);
      if (const auto* refused = std::get_if<contracts::KernelQuotaRefusal>(&plan)) {
        co_return KernelQuotaEligibilityDecision{.decision = refused->status == 503 ? kernelQuotaUnavailable(refused->message) : kernelLimitBlocked(refused->message)};
      }
      const contracts::KernelQuotaEffectivePolicy effective = std::get<contracts::KernelQuotaEligibilityPlanEligible>(plan).effective;
      rt::Task<std::optional<KernelQuotaWindowV2>> reconciling =
        quota_reservations::reconcileKernelQuotaWindowReservations(kv, entries.windowEntry, scope, owner, repo);
      const std::optional<KernelQuotaWindowV2> existingWindow = co_await std::move(reconciling);
      if (!entries.windowEntry.value.isNull() && !existingWindow) {
        co_return KernelQuotaEligibilityDecision{.decision = kernelQuotaUnavailable("Kernel quota aggregate is malformed")};
      }
      KernelQuotaReservationTarget target{std::move(entries), effective, quota_v2::windowForEffectivePolicy(existingWindow, scope, owner, repo, effective.windowMs, nowMs)};
      co_return KernelQuotaEligibilityEligible{.target = std::move(target)};
    }

    /**
     * Frees capacity for an over-limit window by reclaiming an expired lease first.
     * Returns "retry" once capacity was reclaimed, so the attempt is repeated.
     */
    rt::Task<KernelQuotaCapacityOutcome> reclaimKernelQuotaCapacity(std::shared_ptr<rt::Kv> kv, KernelQuotaScope scope, std::string owner,
                                                                    std::optional<std::string> repo, KernelQuotaWindowV2 window, double limit, double nowMs,
                                                                    bool reclaimedExpiredReservation) {
      if (reclaimedExpiredReservation) co_return KernelQuotaCapacityOutcomeDecision{.decision = quotaExceeded(window, limit)};
      rt::Reason failure;
      try {
        rt::Task<bool> reclaiming = quota_reservations::reclaimExpiredKernelReservationUnlocked(kv, scope, owner, repo, window, nowMs);
        const bool reclaimed = co_await std::move(reclaiming);
        if (reclaimed) co_return KernelQuotaCapacityOutcomeRetry{.reclaimedCapacity = true};
        co_return KernelQuotaCapacityOutcomeDecision{.decision = quotaExceeded(window, limit)};
      } catch (...) {
        failure = std::current_exception();
      }
      rt::console::warn("[ai.ubq.fi] Failed to reclaim kernel quota lease:", failure);
      co_return KernelQuotaCapacityOutcomeDecision{.decision = kernelQuotaUnavailable()};
    }

    rt::Task<KernelQuotaCapacityOutcome> commitKernelQuotaReservation(std::shared_ptr<rt::Kv> kv, KernelQuotaScope scope, std::string owner,
                                                                      std::optional<std::string> repo, std::string requestId, std::string route,
                                                                      KernelQuotaReserveOptions options, KernelQuotaReservationTarget target, double nowMs,
                                                                      bool reclaimedExpiredReservation) {
      const rt::KvKey reservationKey = native_kernel::kernelReservationKey(scope, owner, repo, target.resolved.window.created_at_ms, requestId);
      const rt::KvEntryMaybe<> reservationEntry = co_await kv->get(reservationKey, STRONG);
      const contracts::KernelQuotaIdentity identity{.scope = scope, .owner = owner, .repo = repo};
      const contracts::KernelQuotaReservePlan plan =
        native_kernel::decideKernelQuotaReserve(identity, target.effective, target.resolved.window, quota_v2::kernelQuotaRead(reservationEntry), requestId, route,
                                                quota_v2::KERNEL_QUOTA_CONSTANTS.noUsageLimit, nowMs);
      if (const auto* refused = std::get_if<contracts::KernelQuotaRefusal>(&plan)) {
        co_return KernelQuotaCapacityOutcomeDecision{.decision = kernelQuotaUnavailable(refused->message)};
      }
      if (const auto* full = std::get_if<contracts::KernelQuotaReservePlanFull>(&plan)) {
        rt::Task<KernelQuotaCapacityOutcome> reclaiming = reclaimKernelQuotaCapacity(kv, scope, owner, repo, full->window, full->limit, nowMs, reclaimedExpiredReservation);
        KernelQuotaCapacityOutcome outcome = co_await std::move(reclaiming);
        co_return outcome;
      }
      const contracts::KernelQuotaReservePlanReserve& reserve = std::get<contracts::KernelQuotaReservePlanReserve>(plan);
      const contracts::KernelQuotaReservationRowV2& reservation = reserve.reservation;
      const KernelQuotaWindowV2& reservedWindow = reserve.window;
      const double retentionMs = reserve.expireIn;
      const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      atomic->check(target.policyEntry).check(target.windowEntry).check(reservationEntry);
      if (target.repoGuardEntry) atomic->check(*target.repoGuardEntry);
      if (target.effective.source == "default") {
        atomic->check(target.defaults.limitEntry).check(target.defaults.windowEntry).check(target.defaultCutoverEntry);
      }
      rt::KvSetOptions retention;
      retention.expireIn = retentionMs;
      atomic->set(target.windowKey, rt::KvValue(json::value_from(reservedWindow)), retention)
        .set(reservationKey, rt::KvValue(json::value_from(reservation)), retention);
      const rt::KvCommitResult committed = co_await atomic->commit();
      if (!committed.ok) co_return KernelQuotaCapacityOutcomeRetry{.reclaimedCapacity = false};
      co_return KernelQuotaCapacityOutcomeDecision{
        .decision = quota_v2::KernelQuotaReservationDecisionOk{
          .reservation = quota_reservations::kernelReservationContext(kv, scope, owner, repo, reservedWindow.created_at_ms, requestId, route,
                                                                      reservation.lease_expires_at_ms, options.renewalIntervalMs),
        },
      };
    }

    rt::Task<KernelQuotaCapacityOutcome> attemptReserveKernelQuota(std::shared_ptr<rt::Kv> kv, KernelQuotaScope scope, std::string owner,
                                                                   std::optional<std::string> repo, std::string requestId, std::string route,
                                                                   KernelQuotaReserveOptions options, double nowMs, bool reclaimedExpiredReservation) {
      rt::Task<KernelQuotaReservationEntries> loading = loadKernelQuotaReservationEntries(kv, quota_v2::kernelPolicyKeys(scope, owner, repo), owner, options);
      KernelQuotaReservationEntries entries = co_await std::move(loading);
      rt::Task<KernelQuotaEligibility> evaluating = evaluateKernelQuotaEligibility(kv, std::move(entries), scope, owner, repo, options, nowMs);
      KernelQuotaEligibility eligibility = co_await std::move(evaluating);
      if (auto* decided = std::get_if<KernelQuotaEligibilityDecision>(&eligibility)) {
        co_return KernelQuotaCapacityOutcomeDecision{.decision = std::move(decided->decision)};
      }
      rt::Task<KernelQuotaCapacityOutcome> committing =
        commitKernelQuotaReservation(kv, scope, owner, repo, requestId, route, options, std::move(std::get<KernelQuotaEligibilityEligible>(eligibility).target), nowMs,
                                     reclaimedExpiredReservation);
      KernelQuotaCapacityOutcome outcome = co_await std::move(committing);
      co_return outcome;
    }

    rt::Task<KernelQuotaReservationDecision> reserveLimit(KernelQuotaScope scope, std::string owner, std::optional<std::string> repo, std::string requestId,
                                                          std::string route, KernelQuotaReserveOptions options) {
      rt::Reason failure;
      try {
        std::shared_ptr<rt::Kv> kv;
        if (options.kv) {
          kv = *options.kv;
        } else {
          auto opening = ::uos::kv::getKv();
          kv = co_await opening.wait();
        }
        if (!kv) co_return kernelQuotaUnavailable();
        if (requestId.empty() || route.empty()) co_return kernelQuotaUnavailable("Kernel quota reservation requires a request id and route");
        std::function<rt::Task<KernelQuotaReservationDecision>()> operation = [kv, scope, owner, repo, requestId, route,
                                                                              options]() -> rt::Task<KernelQuotaReservationDecision> {
          bool reclaimedExpiredReservation = false;
          for (double attempt = 0; attempt < quota_v2::MAX_KV_RETRIES; attempt += 1) {
            const double nowMs = options.nowMs ? *options.nowMs : rt::dateNow();
            rt::Task<KernelQuotaCapacityOutcome> attempting =
              attemptReserveKernelQuota(kv, scope, owner, repo, requestId, route, options, nowMs, reclaimedExpiredReservation);
            KernelQuotaCapacityOutcome outcome = co_await std::move(attempting);
            if (const auto* retry = std::get_if<KernelQuotaCapacityOutcomeRetry>(&outcome)) {
              if (retry->reclaimedCapacity) reclaimedExpiredReservation = true;
              continue;
            }
            KernelQuotaReservationDecision decision = std::move(std::get<KernelQuotaCapacityOutcomeDecision>(outcome).decision);
            co_return decision;
          }
          co_return kernelQuotaUnavailable("Kernel quota changed concurrently; retry");
        };
        rt::Task<KernelQuotaReservationDecision> locked =
          quota_reservations::withKernelQuotaLock<KernelQuotaReservationDecision>(scope, owner, repo, std::move(operation));
        KernelQuotaReservationDecision decision = co_await std::move(locked);
        co_return decision;
      } catch (...) {
        failure = std::current_exception();
      }
      rt::console::warn("[ai.ubq.fi] Failed to reserve kernel quota:", failure);
      co_return kernelQuotaUnavailable();
    }
  }

  rt::Task<KernelQuotaReservationDecision> reserveKernelOrgUsageLimit(std::string owner, std::string requestId, std::string route) {
    return reserveKernelOrgUsageLimit(std::move(owner), std::move(requestId), std::move(route), ReserveKernelUsageLimitOptions{});
  }

  rt::Task<KernelQuotaReservationDecision> reserveKernelOrgUsageLimit(std::string owner, std::string requestId, std::string route, ReserveKernelUsageLimitOptions options) {
    KernelQuotaReserveOptions limitOptions{.kv = std::move(options.kv), .nowMs = options.nowMs, .renewalIntervalMs = options.renewalIntervalMs};
    rt::Task<KernelQuotaReservationDecision> reserving = reserveLimit("org", std::move(owner), std::nullopt, std::move(requestId), std::move(route), std::move(limitOptions));
    KernelQuotaReservationDecision decision = co_await std::move(reserving);
    co_return decision;
  }

  rt::Task<KernelQuotaReservationDecision> reserveEffectiveKernelUsageLimit(std::string owner, std::string repo, std::string requestId, std::string route) {
    return reserveEffectiveKernelUsageLimit(std::move(owner), std::move(repo), std::move(requestId), std::move(route), ReserveKernelUsageLimitOptions{});
  }

  rt::Task<KernelQuotaReservationDecision> reserveEffectiveKernelUsageLimit(std::string owner, std::string repo, std::string requestId, std::string route,
                                                                            ReserveKernelUsageLimitOptions options) {
    rt::Reason failure;
    try {
      std::shared_ptr<rt::Kv> kv;
      if (options.kv) {
        kv = *options.kv;
      } else {
        auto opening = ::uos::kv::getKv();
        kv = co_await opening.wait();
      }
      if (!kv) co_return kernelQuotaUnavailable();
      rt::Task<quota_reservations::KernelQuotaPolicyState> reading = quota_reservations::readKernelQuotaPolicyState(kv, owner, repo);
      const quota_reservations::KernelQuotaPolicyState state = co_await std::move(reading);
      if (state.limit_scope == "repo") {
        KernelQuotaReserveOptions repoOptions{.kv = kv, .nowMs = options.nowMs, .renewalIntervalMs = options.renewalIntervalMs, .expectedRepoPolicy = "present"};
        rt::Task<KernelQuotaReservationDecision> reserving = reserveLimit("repo", owner, repo, requestId, route, std::move(repoOptions));
        KernelQuotaReservationDecision decision = co_await std::move(reserving);
        co_return decision;
      }
      KernelQuotaReserveOptions orgOptions{
        .kv = kv, .nowMs = options.nowMs, .renewalIntervalMs = options.renewalIntervalMs, .expectedRepoPolicy = "absent", .guardedRepo = repo};
      rt::Task<KernelQuotaReservationDecision> reserving = reserveLimit("org", owner, std::nullopt, requestId, route, std::move(orgOptions));
      KernelQuotaReservationDecision decision = co_await std::move(reserving);
      co_return decision;
    } catch (...) {
      failure = std::current_exception();
    }
    rt::console::warn("[ai.ubq.fi] Failed to reserve effective kernel quota:", failure);
    co_return kernelQuotaUnavailable();
  }
}
