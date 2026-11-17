#pragma once
// Port of src/kernel/quota-reservations.ts (kernel quota v2 reservations and settlement, split
// out of src/kernel_quota_v2.ts). Conventions: uos/kernel/quota_v2.hpp.
#include "uos/kernel/quota_v2.hpp"

#include "uos/authority/native_kernel.hpp"
#include "uos/authority/native_kernel_contracts.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace uos::kernel::quota_reservations {
  using authority::native_kernel::normalizeKernelQuotaReservationRowV2;

  // Exported at the end of the TS module. `kernelQuotaUnavailable(message = "Kernel quota is unavailable")`.
  quota_v2::KernelQuotaReservationDecision kernelQuotaUnavailable();
  quota_v2::KernelQuotaReservationDecision kernelQuotaUnavailable(std::string message);

  // What readKernelQuotaPolicyState resolves: `Readonly<{ limit_scope; has_policy; repo_entry;
  // org_entry }>`.
  struct KernelQuotaPolicyState {
    authority::native_kernel_contracts::KernelQuotaScope limit_scope;
    bool has_policy;
    rt::KvEntryMaybe<> repo_entry;
    rt::KvEntryMaybe<> org_entry;
  };

  // Exported at the end of the TS module. Throws Error "Kernel repo quota policy is malformed" /
  // "Kernel org quota policy is malformed" for a stored policy that does not normalize.
  rt::Task<KernelQuotaPolicyState> readKernelQuotaPolicyState(std::shared_ptr<rt::Kv> kv, std::string owner, std::string repo);

  // `options: Readonly<{ kv?: Kv | null }>`
  struct KernelQuotaPolicyStateOptions {
    std::optional<std::shared_ptr<rt::Kv>> kv{};
  };
  rt::Task<quota_v2::KernelQuotaPolicyStateDecision> resolveKernelQuotaPolicyState(std::string owner, std::string repo);
  rt::Task<quota_v2::KernelQuotaPolicyStateDecision> resolveKernelQuotaPolicyState(std::string owner, std::string repo, KernelQuotaPolicyStateOptions options);

  /**
   * Repair the reservation aggregate after a pre-reservation writer rewrites a
   * window without the new field. The window CAS at every mutating caller makes
   * the row count stable for admission: reservation cardinality changes always
   * update the same window atomically, while lease-only row updates do not.
   */
  // std::nullopt is null.
  rt::Task<std::optional<authority::native_kernel_contracts::KernelQuotaWindowV2>> reconcileKernelQuotaWindowReservations(
    std::shared_ptr<rt::Kv> kv, rt::KvEntryMaybe<> entry, authority::native_kernel_contracts::KernelQuotaScope scope, std::string owner, std::optional<std::string> repo);

  namespace detail {
    // One turn of withKernelQuotaLock: the lock key, the promise of the previous holder (if any)
    // and this holder's promise with its `release` resolver.
    struct KernelQuotaLockTurn {
      std::string lockKey;
      std::optional<rt::Promise<void>> previous;
      rt::Promise<void> current;
      rt::Resolve<void> release;
    };
    // `JSON.stringify(kernelScopedWindowKey(scope, owner, repo))`, the previous holder, and this
    // holder registered as the latest one (`kernelQuotaLocks.set(lockKey, current)`).
    KernelQuotaLockTurn enterKernelQuotaLock(const authority::native_kernel_contracts::KernelQuotaScope& scope, const std::string& owner,
                                             const std::optional<std::string>& repo);
    // The `finally`: `release(); if (kernelQuotaLocks.get(lockKey) === current) kernelQuotaLocks.delete(lockKey);`
    void leaveKernelQuotaLock(const KernelQuotaLockTurn& turn);
  }

  // Exported at the end of the TS module. Runs `operation` once every earlier holder of the same
  // scope window lock has finished, holding it until `operation` settles.
  template <class T>
  rt::Task<T> withKernelQuotaLock(authority::native_kernel_contracts::KernelQuotaScope scope, std::string owner, std::optional<std::string> repo,
                                  std::function<rt::Task<T>()> operation) {
    const detail::KernelQuotaLockTurn turn = detail::enterKernelQuotaLock(scope, owner, repo);
    if (turn.previous) co_await turn.previous->wait();
    rt::Reason failure;
    if constexpr (std::is_void_v<T>) {
      try {
        co_await operation();
      } catch (...) {
        failure = std::current_exception();
      }
      detail::leaveKernelQuotaLock(turn);
      if (failure) std::rethrow_exception(failure);
    } else {
      std::optional<T> result;
      try {
        result = co_await operation();
      } catch (...) {
        failure = std::current_exception();
      }
      detail::leaveKernelQuotaLock(turn);
      if (failure) std::rethrow_exception(failure);
      co_return std::move(*result);
    }
  }

  // Exported at the end of the TS module. `renewalIntervalMs = KERNEL_QUOTA_RESERVATION_RENEWAL_MS`:
  // std::nullopt (undefined) takes the default.
  quota_v2::KernelQuotaReservation kernelReservationContext(std::shared_ptr<rt::Kv> kv, authority::native_kernel_contracts::KernelQuotaScope scope, std::string owner,
                                                            std::optional<std::string> repo, double windowCreatedAtMs, std::string requestId, std::string route,
                                                            double initialLeaseExpiresAtMs, std::optional<double> renewalIntervalMs = std::nullopt);

  // Exported at the end of the TS module. True once an expired lease was settled; throws Error
  // "Kernel quota reservation changed concurrently" when every expired row lost its
  // compare-and-set.
  rt::Task<bool> reclaimExpiredKernelReservationUnlocked(std::shared_ptr<rt::Kv> kv, authority::native_kernel_contracts::KernelQuotaScope scope, std::string owner,
                                                         std::optional<std::string> repo, authority::native_kernel_contracts::KernelQuotaWindowV2 window, double nowMs);

  rt::Task<void> releaseKernelDefaultWindowCutover(std::shared_ptr<rt::Kv> kv, quota_v2::KernelDefaultWindowCutoverGuard guard);

  // `expectedLimitEntry: KvEntryMaybe<number>`, `expectedWindowEntry: KvEntryMaybe<number>`: the
  // entries are compare-and-set checks.
  rt::Task<quota_v2::KernelDefaultWindowCutoverDecision> acquireKernelDefaultWindowCutover(std::shared_ptr<rt::Kv> kv, rt::KvEntryMaybe<> expectedLimitEntry,
                                                                                           rt::KvEntryMaybe<> expectedWindowEntry);

  using authority::native_kernel::kernelReservationKey;
}

namespace uos::kernel::quota_v2 {
  // `export * from "./quota-reservations.ts"` (src/kernel/quota-v2.ts).
  using quota_reservations::normalizeKernelQuotaReservationRowV2;
  using quota_reservations::resolveKernelQuotaPolicyState;
  using quota_reservations::reconcileKernelQuotaWindowReservations;
  using quota_reservations::releaseKernelDefaultWindowCutover;
  using quota_reservations::acquireKernelDefaultWindowCutover;
  using quota_reservations::kernelQuotaUnavailable;
  using quota_reservations::kernelReservationContext;
  using quota_reservations::kernelReservationKey;
  using quota_reservations::readKernelQuotaPolicyState;
  using quota_reservations::reclaimExpiredKernelReservationUnlocked;
  using quota_reservations::withKernelQuotaLock;
}
