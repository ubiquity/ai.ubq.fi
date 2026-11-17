#include "uos/kernel/quota_write.hpp"

#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"

#include <exception>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>

namespace uos::kernel::quota_write {
  namespace {
    namespace contracts = authority::native_kernel_contracts;
    namespace native_kernel = authority::native_kernel;
    using ::uos::types::KernelAuthLimitRecord;
    using ::uos::types::KernelOrgLimitRecord;
    using contracts::KernelQuotaPolicySetOptions;
    using contracts::KernelQuotaScope;
    using contracts::KernelQuotaWindowV2;

    // `KernelAuthLimitRecord | KernelOrgLimitRecord`
    using KernelLimitRecord = std::variant<KernelAuthLimitRecord, KernelOrgLimitRecord>;

    // `{ consistency: "strong" }`
    const rt::KvGetOptions STRONG{.consistency = rt::KvConsistencyLevel::strong};

    struct KernelQuotaPolicyRequest {
      KernelQuotaScope scope;
      std::string owner;
      std::optional<std::string> repo;
      double usageLimitRequests;
      KernelQuotaPolicySetOptions options;
    };

    /** Every KV entry one policy write compares against. */
    struct KernelQuotaPolicyWriteEntries {
      quota_v2::KernelDefaults defaults;
      rt::KvEntryMaybe<> policyEntry;
      rt::KvEntryMaybe<> windowEntry;
      std::optional<rt::KvEntryMaybe<>> orgPolicyEntry;  // null outside the repo scope
      std::optional<rt::KvEntryMaybe<>> orgWindowEntry;  // null outside the repo scope
      rt::KvEntryMaybe<> defaultCutoverEntry;
    };

    // `Readonly<{ outcome: "written"; record }> | Readonly<{ outcome: "retry" }> | Readonly<{ outcome: "abort" }>`
    struct KernelQuotaPolicyWriteAttempt {
      std::string outcome;  // "written" | "retry" | "abort"
      std::optional<KernelLimitRecord> record{};
    };

    // `plan.kind` of a KernelPolicyWindowGuard.
    std::string guardKind(const native_kernel::KernelPolicyWindowGuard& plan) {
      return std::visit([](const auto& alternative) { return std::string(std::decay_t<decltype(alternative)>::kind); }, plan);
    }

    // The org reads of attemptSetKernelQuotaPolicy (null outside the repo scope).
    rt::Task<std::optional<rt::KvEntryMaybe<>>> readEntry(std::shared_ptr<rt::Kv> kv, rt::KvKey key) {
      rt::KvEntryMaybe<> entry = co_await kv->get(std::move(key), STRONG);
      co_return entry;
    }

    /**
     * A repo policy created on top of an inherited org policy must not orphan live org
     * reservations: reconcile the org aggregate and reclaim an expired lease first.
     */
    // `"proceed" | "retry" | "abort"`
    rt::Task<std::string> reconcileKernelPolicyOrgInheritance(std::shared_ptr<rt::Kv> kv, KernelQuotaScope scope, std::string owner,
                                                              KernelQuotaPolicyWriteEntries entries, double nowMs) {
      if (scope != "repo") co_return std::string("proceed");
      if (!entries.policyEntry.value.isNull()) co_return std::string("proceed");
      if (!entries.orgPolicyEntry || !entries.orgWindowEntry) co_return std::string("proceed");
      quota_v2::policyFor(*entries.orgPolicyEntry, "org", owner, std::nullopt, entries.defaults);
      rt::Task<std::optional<KernelQuotaWindowV2>> reconciling =
        quota_reservations::reconcileKernelQuotaWindowReservations(kv, *entries.orgWindowEntry, "org", owner, std::nullopt);
      const std::optional<KernelQuotaWindowV2> inheritedOrgWindow = co_await std::move(reconciling);
      const native_kernel::KernelPolicyWindowGuard plan =
        native_kernel::decideKernelPolicyWindowGuard(inheritedOrgWindow, !entries.orgWindowEntry->value.isNull(), true, nowMs);
      const auto* reclaim = std::get_if<native_kernel::KernelPolicyWindowGuardReclaim>(&plan);
      if (reclaim == nullptr) co_return guardKind(plan);
      rt::Task<bool> reclaiming = quota_reservations::reclaimExpiredKernelReservationUnlocked(kv, "org", owner, std::nullopt, reclaim->window, nowMs);
      const bool reclaimed = co_await std::move(reclaiming);
      co_return std::string(reclaimed ? "retry" : "abort");
    }

    /** A reset that would discard live reservations must first reclaim an expired lease. */
    // `"proceed" | "retry" | "abort"`
    rt::Task<std::string> guardKernelPolicyWindowReset(std::shared_ptr<rt::Kv> kv, KernelQuotaScope scope, std::string owner, std::optional<std::string> repo,
                                                       std::optional<KernelQuotaWindowV2> currentWindow, double nowMs, bool reset) {
      const native_kernel::KernelPolicyWindowGuard plan = native_kernel::decideKernelPolicyWindowGuard(currentWindow, false, reset, nowMs);
      const auto* reclaim = std::get_if<native_kernel::KernelPolicyWindowGuardReclaim>(&plan);
      if (reclaim == nullptr) co_return guardKind(plan);
      rt::Task<bool> reclaiming = quota_reservations::reclaimExpiredKernelReservationUnlocked(kv, scope, owner, repo, reclaim->window, nowMs);
      const bool reclaimed = co_await std::move(reclaiming);
      co_return std::string(reclaimed ? "retry" : "abort");
    }

    /** The compare-and-set guards that make the policy write atomic for every reader. */
    std::unique_ptr<rt::AtomicOperation> kernelPolicyWriteAtomic(const std::shared_ptr<rt::Kv>& kv, const KernelQuotaScope& scope,
                                                                 const KernelQuotaPolicyWriteEntries& entries, const contracts::KernelQuotaEffectivePolicy& current) {
      std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      atomic->check(entries.policyEntry).check(entries.windowEntry);
      if (current.source == "default") {
        atomic->check(entries.defaults.limitEntry).check(entries.defaults.windowEntry).check(entries.defaultCutoverEntry);
      }
      if (scope == "repo" && entries.policyEntry.value.isNull() && entries.orgPolicyEntry && entries.orgWindowEntry) {
        atomic->check(*entries.orgPolicyEntry).check(*entries.orgWindowEntry);
      }
      return atomic;
    }

    rt::Task<KernelQuotaPolicyWriteAttempt> attemptSetKernelQuotaPolicy(std::shared_ptr<rt::Kv> kv, KernelQuotaPolicyRequest request,
                                                                        native_kernel::KernelPolicyKeys keys) {
      const KernelQuotaScope& scope = request.scope;
      const std::string& owner = request.owner;
      const std::optional<std::string>& repo = request.repo;
      const KernelQuotaPolicySetOptions& options = request.options;
      const double nowMs = rt::dateNow();
      rt::Promise<quota_v2::KernelDefaults> defaultsRead(quota_v2::loadDefaults(kv));
      rt::Promise<rt::KvEntryMaybe<>> policyRead(kv->get(keys.policyKey, STRONG));
      rt::Promise<rt::KvEntryMaybe<>> windowRead(kv->get(keys.windowKey, STRONG));
      rt::Promise<std::optional<rt::KvEntryMaybe<>>> orgPolicyRead = scope == "repo"
                                                                        ? rt::Promise<std::optional<rt::KvEntryMaybe<>>>(readEntry(kv, native_kernel::kernelOrgPolicyKey(owner)))
                                                                        : rt::Promise<std::optional<rt::KvEntryMaybe<>>>::resolve(std::nullopt);
      rt::Promise<std::optional<rt::KvEntryMaybe<>>> orgWindowRead = scope == "repo"
                                                                        ? rt::Promise<std::optional<rt::KvEntryMaybe<>>>(readEntry(kv, native_kernel::kernelOrgWindowKey(owner)))
                                                                        : rt::Promise<std::optional<rt::KvEntryMaybe<>>>::resolve(std::nullopt);
      rt::Promise<rt::KvEntryMaybe<>> cutoverRead(kv->get(native_kernel::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY, STRONG));
      auto reads = rt::all(defaultsRead, policyRead, windowRead, orgPolicyRead, orgWindowRead, cutoverRead);
      auto readEntries = co_await reads.wait();
      auto& [defaults, policyEntry, windowEntry, orgPolicyEntry, orgWindowEntry, defaultCutoverEntry] = readEntries;
      const KernelQuotaPolicyWriteEntries entries{
        .defaults = std::move(defaults),
        .policyEntry = std::move(policyEntry),
        .windowEntry = std::move(windowEntry),
        .orgPolicyEntry = std::move(orgPolicyEntry),
        .orgWindowEntry = std::move(orgWindowEntry),
        .defaultCutoverEntry = std::move(defaultCutoverEntry),
      };
      const contracts::KernelQuotaEffectivePolicy current = quota_v2::policyFor(entries.policyEntry, scope, owner, repo, entries.defaults);
      if (native_kernel::kernelQuotaDefaultCutoverBlocks(current, quota_v2::kernelQuotaRead(entries.defaultCutoverEntry))) {
        co_return KernelQuotaPolicyWriteAttempt{.outcome = "abort"};
      }
      const native_kernel::KernelPolicyTiming timing = native_kernel::resolveKernelPolicyTiming(options, current, quota_v2::KERNEL_QUOTA_CONSTANTS.noExpirationMs);
      const contracts::KernelQuotaIdentity identity{.scope = scope, .owner = owner, .repo = repo};
      const contracts::KernelQuotaPolicyV2 policy =
        native_kernel::makeKernelQuotaPolicy(identity, current, request.usageLimitRequests, timing, quota_v2::KERNEL_QUOTA_CONSTANTS.noUsageLimit, nowMs);
      rt::Task<std::optional<KernelQuotaWindowV2>> reconciling =
        quota_reservations::reconcileKernelQuotaWindowReservations(kv, entries.windowEntry, scope, owner, repo);
      const std::optional<KernelQuotaWindowV2> currentWindow = co_await std::move(reconciling);
      if (!entries.windowEntry.value.isNull() && !currentWindow) co_return KernelQuotaPolicyWriteAttempt{.outcome = "abort"};
      rt::Task<std::string> inheriting = reconcileKernelPolicyOrgInheritance(kv, scope, owner, entries, nowMs);
      const std::string inheritance = co_await std::move(inheriting);
      if (inheritance != "proceed") co_return KernelQuotaPolicyWriteAttempt{.outcome = inheritance};
      const bool reset = native_kernel::kernelQuotaPolicyResetRequested(options, current.windowMs, timing.windowMs);
      rt::Task<std::string> guarding = guardKernelPolicyWindowReset(kv, scope, owner, repo, currentWindow, nowMs, reset);
      const std::string resetGuard = co_await std::move(guarding);
      if (resetGuard != "proceed") co_return KernelQuotaPolicyWriteAttempt{.outcome = resetGuard};
      const contracts::KernelQuotaPolicyWritePlan plan = native_kernel::decideKernelQuotaPolicyWrite(identity, policy, currentWindow, reset, nowMs);
      const KernelQuotaWindowV2& window = plan.window;
      const std::unique_ptr<rt::AtomicOperation> atomic = kernelPolicyWriteAtomic(kv, scope, entries, current);
      atomic->set(keys.policyKey, rt::KvValue(json::value_from(policy))).set(keys.windowKey, rt::KvValue(json::value_from(window)));
      const rt::KvCommitResult committed = co_await atomic->commit();
      if (!committed.ok) co_return KernelQuotaPolicyWriteAttempt{.outcome = "retry"};
      const quota_v2::KernelPolicySummary summary{.limit = policy.usage_limit_requests, .windowMs = timing.windowMs, .expiresAtMs = timing.expiresAtMs};
      co_return KernelQuotaPolicyWriteAttempt{
        .outcome = "written",
        .record = scope == "repo" ? KernelLimitRecord(quota_v2::repoRecord(owner, quota_v2::kernelRepoPart(repo), summary, window, nowMs))
                                  : KernelLimitRecord(quota_v2::orgRecord(owner, summary, window, nowMs)),
      };
    }

    rt::Task<std::optional<KernelLimitRecord>> setPolicy(KernelQuotaScope scope, std::string owner, std::optional<std::string> repo, double usageLimitRequests,
                                                         KernelQuotaPolicySetOptions options) {
      rt::Reason failure;
      try {
        auto opening = ::uos::kv::getKv();
        const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
        if (!kv) co_return std::nullopt;
        const KernelQuotaPolicyRequest request{.scope = scope, .owner = owner, .repo = repo, .usageLimitRequests = usageLimitRequests, .options = options};
        const native_kernel::KernelPolicyKeys keys = quota_v2::kernelPolicyKeys(scope, owner, repo);
        std::function<rt::Task<std::optional<KernelLimitRecord>>()> operation = [kv, request, keys]() -> rt::Task<std::optional<KernelLimitRecord>> {
          for (double attempt = 0; attempt < quota_v2::MAX_KV_RETRIES; attempt += 1) {
            rt::Task<KernelQuotaPolicyWriteAttempt> attempting = attemptSetKernelQuotaPolicy(kv, request, keys);
            KernelQuotaPolicyWriteAttempt result = co_await std::move(attempting);
            if (result.outcome == "retry") continue;
            if (result.outcome == "abort") co_return std::nullopt;
            co_return std::move(result.record);
          }
          co_return std::nullopt;
        };
        rt::Task<std::optional<KernelLimitRecord>> locked =
          quota_reservations::withKernelQuotaLock<std::optional<KernelLimitRecord>>(scope, owner, repo, std::move(operation));
        std::optional<KernelLimitRecord> record = co_await std::move(locked);
        co_return record;
      } catch (...) {
        failure = std::current_exception();
      }
      rt::console::warn("[ai.ubq.fi] Failed to set kernel quota policy:", failure);
      co_return std::nullopt;
    }
  }

  rt::Task<std::optional<KernelAuthLimitRecord>> setKernelUsageLimit(std::string owner, std::string repo, double usageLimitRequests) {
    return setKernelUsageLimit(std::move(owner), std::move(repo), usageLimitRequests, KernelQuotaPolicySetOptions{});
  }

  rt::Task<std::optional<KernelAuthLimitRecord>> setKernelUsageLimit(std::string owner, std::string repo, double usageLimitRequests, KernelQuotaPolicySetOptions options) {
    std::optional<std::string> scopedRepo = std::move(repo);
    rt::Task<std::optional<KernelLimitRecord>> setting = setPolicy("repo", std::move(owner), std::move(scopedRepo), usageLimitRequests, options);
    std::optional<KernelLimitRecord> record = co_await std::move(setting);
    if (!record) co_return std::nullopt;
    co_return std::get<KernelAuthLimitRecord>(std::move(*record));
  }

  rt::Task<std::optional<KernelOrgLimitRecord>> setKernelOrgUsageLimit(std::string owner, double usageLimitRequests) {
    return setKernelOrgUsageLimit(std::move(owner), usageLimitRequests, KernelQuotaPolicySetOptions{});
  }

  rt::Task<std::optional<KernelOrgLimitRecord>> setKernelOrgUsageLimit(std::string owner, double usageLimitRequests, KernelQuotaPolicySetOptions options) {
    rt::Task<std::optional<KernelLimitRecord>> setting = setPolicy("org", std::move(owner), std::nullopt, usageLimitRequests, options);
    std::optional<KernelLimitRecord> record = co_await std::move(setting);
    if (!record) co_return std::nullopt;
    co_return std::get<KernelOrgLimitRecord>(std::move(*record));
  }

  namespace {
    /** One delete attempt: true when deleted, false when absent, "retry"/"abort" to steer the loop. */
    // `boolean | "conflict" | "retry" | "abort"`
    using KernelQuotaPolicyDeleteOutcome = std::variant<bool, std::string>;

    rt::Task<KernelQuotaPolicyDeleteOutcome> attemptDeleteKernelQuotaPolicy(std::shared_ptr<rt::Kv> kv, KernelQuotaScope scope, std::string owner,
                                                                            std::optional<std::string> repo, native_kernel::KernelPolicyKeys keys) {
      const double nowMs = rt::dateNow();
      rt::Promise<quota_v2::KernelDefaults> defaultsRead(quota_v2::loadDefaults(kv));
      rt::Promise<rt::KvEntryMaybe<>> policyRead(kv->get(keys.policyKey, STRONG));
      rt::Promise<rt::KvEntryMaybe<>> windowRead(kv->get(keys.windowKey, STRONG));
      rt::Promise<rt::KvEntryMaybe<>> cutoverRead(kv->get(native_kernel::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY, STRONG));
      auto reads = rt::all(defaultsRead, policyRead, windowRead, cutoverRead);
      auto readEntries = co_await reads.wait();
      auto& [defaults, policyEntry, windowEntry, defaultCutoverEntry] = readEntries;
      const native_kernel::KernelQuotaPolicyDeleteSnapshot snapshot{.policy = quota_v2::kernelQuotaRead(policyEntry),
                                                                   .defaultCutover = quota_v2::kernelQuotaRead(defaultCutoverEntry)};
      const std::string precondition = native_kernel::decideKernelQuotaPolicyDeletePrecondition(snapshot);
      if (precondition == "absent") co_return KernelQuotaPolicyDeleteOutcome(false);
      if (precondition == "conflict") co_return KernelQuotaPolicyDeleteOutcome(std::string("conflict"));
      const contracts::KernelQuotaEffectivePolicy current = quota_v2::policyFor(policyEntry, scope, owner, repo, defaults);
      rt::Task<std::optional<KernelQuotaWindowV2>> reconciling = quota_reservations::reconcileKernelQuotaWindowReservations(kv, windowEntry, scope, owner, repo);
      const std::optional<KernelQuotaWindowV2> oldWindow = co_await std::move(reconciling);
      const native_kernel::KernelPolicyWindowGuard guard = native_kernel::decideKernelPolicyWindowGuard(oldWindow, !windowEntry.value.isNull(), true, nowMs);
      if (std::holds_alternative<native_kernel::KernelPolicyWindowGuardAbort>(guard)) co_return KernelQuotaPolicyDeleteOutcome(std::string("abort"));
      if (const auto* reclaim = std::get_if<native_kernel::KernelPolicyWindowGuardReclaim>(&guard)) {
        rt::Task<bool> reclaiming = quota_reservations::reclaimExpiredKernelReservationUnlocked(kv, scope, owner, repo, reclaim->window, nowMs);
        const bool reclaimed = co_await std::move(reclaiming);
        co_return KernelQuotaPolicyDeleteOutcome(std::string(reclaimed ? "retry" : "conflict"));
      }
      const contracts::KernelQuotaIdentity identity{.scope = scope, .owner = owner, .repo = repo};
      const KernelQuotaWindowV2 nextWindow = native_kernel::decideKernelQuotaPolicyDeleteWindow(identity, current, defaults, oldWindow, nowMs);
      const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      atomic->check(policyEntry)
        .check(windowEntry)
        .check(defaults.limitEntry)
        .check(defaults.windowEntry)
        .check(defaultCutoverEntry)
        .delete_(keys.policyKey)
        .set(keys.windowKey, rt::KvValue(json::value_from(nextWindow)));
      const rt::KvCommitResult committed = co_await atomic->commit();
      co_return committed.ok ? KernelQuotaPolicyDeleteOutcome(true) : KernelQuotaPolicyDeleteOutcome(std::string("retry"));
    }

    rt::Task<KernelQuotaDeleteResult> deletePolicy(KernelQuotaScope scope, std::string owner, std::optional<std::string> repo) {
      rt::Reason failure;
      try {
        auto opening = ::uos::kv::getKv();
        const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
        if (!kv) co_return std::nullopt;
        const native_kernel::KernelPolicyKeys keys = quota_v2::kernelPolicyKeys(scope, owner, repo);
        std::function<rt::Task<KernelQuotaDeleteResult>()> operation = [kv, scope, owner, repo, keys]() -> rt::Task<KernelQuotaDeleteResult> {
          for (double attempt = 0; attempt < quota_v2::MAX_KV_RETRIES; attempt += 1) {
            rt::Task<KernelQuotaPolicyDeleteOutcome> attempting = attemptDeleteKernelQuotaPolicy(kv, scope, owner, repo, keys);
            const KernelQuotaPolicyDeleteOutcome outcome = co_await std::move(attempting);
            if (const auto* text = std::get_if<std::string>(&outcome)) {
              if (*text == "retry") continue;
              if (*text == "abort") co_return std::nullopt;
            }
            co_return KernelQuotaDeleteResult(outcome);
          }
          co_return std::nullopt;
        };
        rt::Task<KernelQuotaDeleteResult> locked = quota_reservations::withKernelQuotaLock<KernelQuotaDeleteResult>(scope, owner, repo, std::move(operation));
        KernelQuotaDeleteResult result = co_await std::move(locked);
        co_return result;
      } catch (...) {
        failure = std::current_exception();
      }
      rt::console::warn("[ai.ubq.fi] Failed to delete kernel quota policy:", failure);
      co_return std::nullopt;
    }
  }

  rt::Task<KernelQuotaDeleteResult> deleteKernelUsageLimit(std::string owner, std::string repo) {
    std::optional<std::string> scopedRepo = std::move(repo);
    rt::Task<KernelQuotaDeleteResult> deleting = deletePolicy("repo", std::move(owner), std::move(scopedRepo));
    KernelQuotaDeleteResult result = co_await std::move(deleting);
    co_return result;
  }

  rt::Task<KernelQuotaDeleteResult> deleteKernelOrgUsageLimit(std::string owner) {
    rt::Task<KernelQuotaDeleteResult> deleting = deletePolicy("org", std::move(owner), std::nullopt);
    KernelQuotaDeleteResult result = co_await std::move(deleting);
    co_return result;
  }
}
