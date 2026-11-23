#pragma once
// Port of src/kernel/quota-reserve.ts (kernel quota v2 reservation entry points, split out of
// src/kernel_quota_v2.ts). Conventions: uos/kernel/quota_v2.hpp.
#include "uos/kernel/quota_v2.hpp"
#include "uos/kernel/quota_reservations.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"

#include <memory>
#include <optional>
#include <string>

namespace uos::kernel::quota_reserve {
  // `options: Readonly<{ kv?: Kv | null; nowMs?: number; renewalIntervalMs?: number }>`
  struct ReserveKernelUsageLimitOptions {
    std::optional<std::shared_ptr<rt::Kv>> kv{};
    std::optional<double> nowMs{};
    std::optional<double> renewalIntervalMs{};
  };

  rt::Task<quota_v2::KernelQuotaReservationDecision> reserveKernelOrgUsageLimit(std::string owner, std::string requestId, std::string route);
  rt::Task<quota_v2::KernelQuotaReservationDecision> reserveKernelOrgUsageLimit(std::string owner, std::string requestId, std::string route,
                                                                               ReserveKernelUsageLimitOptions options);

  rt::Task<quota_v2::KernelQuotaReservationDecision> reserveEffectiveKernelUsageLimit(std::string owner, std::string repo, std::string requestId, std::string route);
  rt::Task<quota_v2::KernelQuotaReservationDecision> reserveEffectiveKernelUsageLimit(std::string owner, std::string repo, std::string requestId, std::string route,
                                                                                     ReserveKernelUsageLimitOptions options);
}

namespace uos::kernel::quota_v2 {
  // `export * from "./quota-reserve.ts"` (src/kernel/quota-v2.ts).
  using quota_reserve::reserveKernelOrgUsageLimit;
  using quota_reserve::reserveEffectiveKernelUsageLimit;
}
