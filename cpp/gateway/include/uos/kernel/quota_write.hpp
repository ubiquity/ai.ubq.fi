#pragma once
// Port of src/kernel/quota-write.ts (kernel quota v2 policy write path, split out of
// src/kernel_quota_v2.ts). Conventions: uos/kernel/quota_v2.hpp.
#include "uos/kernel/quota_v2.hpp"
#include "uos/kernel/quota_reservations.hpp"

#include "uos/authority/native_kernel_contracts.hpp"
#include "uos/rt/async.hpp"
#include "uos/types.hpp"

#include <optional>
#include <string>
#include <variant>

namespace uos::kernel::quota_write {
  // `options: { resetUsage?: boolean; windowMs?: number; expiresAtMs?: number }` is the module's
  // KernelQuotaPolicySetOptions, the shape of the native-kernel contract type of that name.
  // std::nullopt is null.
  rt::Task<std::optional<::uos::types::KernelAuthLimitRecord>> setKernelUsageLimit(std::string owner, std::string repo, double usageLimitRequests);
  rt::Task<std::optional<::uos::types::KernelAuthLimitRecord>> setKernelUsageLimit(std::string owner, std::string repo, double usageLimitRequests,
                                                                                    authority::native_kernel_contracts::KernelQuotaPolicySetOptions options);

  rt::Task<std::optional<::uos::types::KernelOrgLimitRecord>> setKernelOrgUsageLimit(std::string owner, double usageLimitRequests);
  rt::Task<std::optional<::uos::types::KernelOrgLimitRecord>> setKernelOrgUsageLimit(std::string owner, double usageLimitRequests,
                                                                                     authority::native_kernel_contracts::KernelQuotaPolicySetOptions options);

  // `boolean | "conflict" | null`: std::nullopt is null; the string alternative is "conflict".
  using KernelQuotaDeleteResult = std::optional<std::variant<bool, std::string>>;

  rt::Task<KernelQuotaDeleteResult> deleteKernelUsageLimit(std::string owner, std::string repo);
  rt::Task<KernelQuotaDeleteResult> deleteKernelOrgUsageLimit(std::string owner);
}

namespace uos::kernel::quota_v2 {
  // `export * from "./quota-write.ts"` (src/kernel/quota-v2.ts).
  using quota_write::setKernelUsageLimit;
  using quota_write::setKernelOrgUsageLimit;
  using quota_write::deleteKernelUsageLimit;
  using quota_write::deleteKernelOrgUsageLimit;
}
