#pragma once
// Port of src/kernel/usage.ts.
//
// The usage readers project stored values without trusting them: the private normalizers read
// rt::KvValue (a stored value may hold any structured-clone type), as the TS reads `unknown`.
#include "uos/kernel/quota_reservations.hpp"
#include "uos/kernel/quota_reserve.hpp"
#include "uos/kernel/quota_v2.hpp"
#include "uos/kernel/quota_write.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace uos::kernel::usage {
  inline const rt::KvKey KERNEL_AUTH_USAGE_PREFIX{"ubq_ai", "kernel_auth", "usage"};
  inline const rt::KvKey KERNEL_AUTH_USAGE_DAILY_PREFIX{"ubq_ai", "kernel_auth", "usage_daily"};
  inline const rt::KvKey KERNEL_AUTH_ORG_USAGE_PREFIX{"ubq_ai", "kernel_auth", "org_usage"};
  inline const rt::KvKey KERNEL_AUTH_ORG_USAGE_DAILY_PREFIX{"ubq_ai", "kernel_auth", "org_usage_daily"};

  rt::KvKey kernelUsageKey(const std::string& owner, const std::string& repo);
  rt::KvKey kernelUsageDailyKey(const std::string& owner, const std::string& repo);
  rt::KvKey kernelOrgUsageKey(const std::string& owner);
  rt::KvKey kernelOrgUsageDailyKey(const std::string& owner);

  // `options: { includeDaily?: boolean; dailyDays?: number }`
  struct KernelUsageReadOptions {
    std::optional<bool> includeDaily{};
    std::optional<double> dailyDays{};
  };

  // `KernelAuthUsageRecord & { daily_requests?: number[] }`
  struct KernelAuthUsageWithDaily : ::uos::types::KernelAuthUsageRecord {
    std::optional<std::vector<double>> daily_requests{};
    friend bool operator==(const KernelAuthUsageWithDaily&, const KernelAuthUsageWithDaily&) = default;
  };
  // `KernelOrgUsageRecord & { daily_requests?: number[] }`
  struct KernelOrgUsageWithDaily : ::uos::types::KernelOrgUsageRecord {
    std::optional<std::vector<double>> daily_requests{};
    friend bool operator==(const KernelOrgUsageWithDaily&, const KernelOrgUsageWithDaily&) = default;
  };

  // The record's members in declaration order, then `daily_requests` when present; value_to is
  // strict like the base record's and drops undeclared members.
  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelAuthUsageWithDaily& value);
  KernelAuthUsageWithDaily tag_invoke(const json::value_to_tag<KernelAuthUsageWithDaily>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelOrgUsageWithDaily& value);
  KernelOrgUsageWithDaily tag_invoke(const json::value_to_tag<KernelOrgUsageWithDaily>&, const json::value& value);

  // std::nullopt is null.
  rt::Task<std::optional<KernelAuthUsageWithDaily>> getKernelUsage(std::string owner, std::string repo);
  rt::Task<std::optional<KernelAuthUsageWithDaily>> getKernelUsage(std::string owner, std::string repo, KernelUsageReadOptions options);

  rt::Task<std::optional<KernelOrgUsageWithDaily>> getKernelOrgUsage(std::string owner);
  rt::Task<std::optional<KernelOrgUsageWithDaily>> getKernelOrgUsage(std::string owner, KernelUsageReadOptions options);

  rt::Task<std::optional<std::vector<KernelAuthUsageWithDaily>>> listKernelUsageRecords();
  rt::Task<std::optional<std::vector<KernelAuthUsageWithDaily>>> listKernelUsageRecords(KernelUsageReadOptions options);

  rt::Task<std::optional<std::vector<KernelOrgUsageWithDaily>>> listKernelOrgUsageRecords();
  rt::Task<std::optional<std::vector<KernelOrgUsageWithDaily>>> listKernelOrgUsageRecords(KernelUsageReadOptions options);

  // Runtime callers import the V2 split policy/window implementation through this
  // module.
  using quota_v2::getKernelOrgUsageLimitSnapshot;
  using quota_v2::getKernelUsageLimitSnapshot;
  using quota_v2::kernelLimitKey;
  using quota_v2::kernelOrgLimitKey;
  using quota_v2::listKernelOrgUsageLimits;
  using quota_v2::listKernelUsageLimits;
  using quota_write::deleteKernelOrgUsageLimit;
  using quota_write::deleteKernelUsageLimit;
  using quota_write::setKernelOrgUsageLimit;
  using quota_write::setKernelUsageLimit;
  using quota_reservations::resolveKernelQuotaPolicyState;
  using quota_reserve::reserveEffectiveKernelUsageLimit;
  using quota_v2::KernelQuotaReservation;
}
