#include "uos/kernel/quota_v2.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/defaults.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"

#include <cstddef>
#include <exception>
#include <type_traits>
#include <utility>
#include <variant>

namespace uos::kernel::quota_v2 {
  namespace {
    constexpr rt::StoredJsonViewOptions STORED_JSON_VIEW_OPTIONS{
      .undefinedMember = rt::StoredJsonViewOptions::UndefinedView::omit,
      .undefinedElement = rt::StoredJsonViewOptions::UndefinedView::null,
      .hole = rt::StoredJsonViewOptions::UndefinedView::null,
      .topLevelUndefined = rt::StoredJsonViewOptions::UndefinedView::emptyObject,
      .bigint = rt::StoredJsonViewOptions::BigIntView::emptyObject,
      .kvU64Members = false,
      .typedArrayMembers = false,
    };
    namespace contracts = authority::native_kernel_contracts;
    namespace native_kernel = authority::native_kernel;
    using ::uos::types::KernelAuthLimitRecord;
    using ::uos::types::KernelOrgLimitRecord;

    // `KernelAuthLimitRecord | KernelOrgLimitRecord`
    using KernelLimitRecord = std::variant<KernelAuthLimitRecord, KernelOrgLimitRecord>;

    // `{ record: KernelAuthLimitRecord | KernelOrgLimitRecord; source: "default" | "kv" }`
    struct KernelLimitSnapshot {
      KernelLimitRecord record;
      std::string source;
    };

    // `{ consistency: "strong" }`
    const rt::KvGetOptions STRONG{.consistency = rt::KvConsistencyLevel::strong};


    KernelPolicySummary summaryOf(const KernelQuotaEffectivePolicy& policy) {
      return KernelPolicySummary{.limit = policy.limit, .windowMs = policy.windowMs, .expiresAtMs = policy.expiresAtMs};
    }

    const std::string& ownerOf(const KernelLimitRecord& record) {
      return std::visit([](const auto& row) -> const std::string& { return row.owner; }, record);
    }

    rt::Task<std::optional<KernelLimitSnapshot>> getSnapshot(KernelQuotaScope scope, std::string owner, std::optional<std::string> repo) {
      rt::Reason failure;
      try {
        auto opening = ::uos::kv::getKv();
        const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
        if (!kv) co_return std::nullopt;
        const native_kernel::KernelPolicyKeys keys = kernelPolicyKeys(scope, owner, repo);
        rt::Promise<KernelDefaults> defaultsRead(loadDefaults(kv));
        rt::Promise<rt::KvEntryMaybe<>> policyRead(kv->get(keys.policyKey, STRONG));
        rt::Promise<rt::KvEntryMaybe<>> windowRead(kv->get(keys.windowKey, STRONG));
        auto reads = rt::all(defaultsRead, policyRead, windowRead);
        const auto readEntries = co_await reads.wait();
        const auto& [defaults, policyEntry, windowEntry] = readEntries;
        const KernelQuotaEffectivePolicy effective = policyFor(policyEntry, scope, owner, repo, defaults);
        const KernelQuotaWindowV2 window =
          windowForEffectivePolicy(normalizeKernelQuotaWindowV2(kernelQuotaValue(windowEntry.value), scope, owner, repo), scope, owner, repo, effective.windowMs,
                                   rt::dateNow())
            .window;
        KernelLimitSnapshot snapshot{.record = scope == "repo" ? KernelLimitRecord(repoRecord(owner, kernelRepoPart(repo), summaryOf(effective), window, rt::dateNow()))
                                                               : KernelLimitRecord(orgRecord(owner, summaryOf(effective), window, rt::dateNow())),
                                     .source = effective.source};
        co_return snapshot;
      } catch (...) {
        failure = std::current_exception();
      }
      rt::console::warn("[ai.ubq.fi] Failed to load kernel quota snapshot:", failure);
      co_return std::nullopt;
    }

    /** Orders listed policy records by owner, then by repo for repo-scope records. */
    double compareKernelPolicyRows(const KernelLimitRecord& a, const KernelLimitRecord& b) {
      const int byOwner = rt::localeCompare(ownerOf(a), ownerOf(b));
      if (byOwner != 0) return byOwner;
      const auto* repoA = std::get_if<KernelAuthLimitRecord>(&a);
      const auto* repoB = std::get_if<KernelAuthLimitRecord>(&b);
      return repoA != nullptr && repoB != nullptr ? rt::localeCompare(repoA->repo, repoB->repo) : 0;
    }

    /**
     * Projects one listed policy entry into its usage record, or null when the entry
     * does not describe a policy for the requested scope.
     */
    // `prefix: readonly string[]` (only its length is read).
    rt::Task<std::optional<KernelLimitRecord>> kernelPolicyRow(std::shared_ptr<rt::Kv> kv, KernelQuotaScope scope, rt::KvKey prefix, rt::KvEntry<> entry) {
      const std::size_t ownerIndex = prefix.size();
      if (ownerIndex >= entry.key.size() || !entry.key[ownerIndex].isString() || entry.key[ownerIndex].string().empty()) co_return std::nullopt;
      const std::string owner = entry.key[ownerIndex].string();
      const std::size_t repoIndex = ownerIndex + 1;
      const std::optional<std::string> repo =
        scope == "repo" && repoIndex < entry.key.size() && entry.key[repoIndex].isString() && !entry.key[repoIndex].string().empty()
          ? std::optional<std::string>(entry.key[repoIndex].string())
          : std::nullopt;
      if (scope == "repo" && !repo) co_return std::nullopt;
      const std::optional<KernelQuotaPolicyV2> policy = normalizeKernelQuotaPolicyV2(kernelQuotaValue(entry.value), scope, owner, repo);
      if (!policy) co_return std::nullopt;
      auto windowKey = kernelScopedWindowKey(scope, owner, repo);
      auto reading = kv->get(std::move(windowKey), STRONG);
      const rt::KvEntryMaybe<> windowEntry = co_await std::move(reading);
      const KernelQuotaWindowV2 window =
        windowForEffectivePolicy(normalizeKernelQuotaWindowV2(kernelQuotaValue(windowEntry.value), scope, owner, repo), scope, owner, repo, policy->window_ms, rt::dateNow())
          .window;
      const KernelPolicySummary summary{.limit = policy->usage_limit_requests, .windowMs = policy->window_ms, .expiresAtMs = policy->expires_at_ms};
      if (scope == "repo") co_return KernelLimitRecord(repoRecord(owner, kernelRepoPart(repo), summary, window, rt::dateNow()));
      co_return KernelLimitRecord(orgRecord(owner, summary, window, rt::dateNow()));
    }

    rt::Task<std::optional<std::vector<KernelLimitRecord>>> listPolicies(KernelQuotaScope scope) {
      rt::Reason failure;
      try {
        auto opening = ::uos::kv::getKv();
        const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
        if (!kv) co_return std::nullopt;
        const rt::KvKey prefix = scope == "repo" ? KERNEL_REPO_POLICY_V2_PREFIX : KERNEL_ORG_POLICY_V2_PREFIX;
        std::vector<KernelLimitRecord> rows;
        const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = prefix});
        while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
          rt::Task<std::optional<KernelLimitRecord>> projecting = kernelPolicyRow(kv, scope, prefix, std::move(*entry));
          std::optional<KernelLimitRecord> row = co_await std::move(projecting);
          if (row) rows.push_back(std::move(*row));
        }
        rt::sort(rows, compareKernelPolicyRows);
        co_return rows;
      } catch (...) {
        failure = std::current_exception();
      }
      rt::console::warn("[ai.ubq.fi] Failed to list kernel quota policies:", failure);
      co_return std::nullopt;
    }
  }

  json::value kernelQuotaValue(const rt::KvValue& value) { return rt::storedJsonView(value, STORED_JSON_VIEW_OPTIONS); }

  contracts::KernelQuotaRead<> kernelQuotaRead(const rt::KvEntryMaybe<>& entry) {
    return contracts::KernelQuotaRead<>{.value = kernelQuotaValue(entry.value), .revision = entry.versionstamp};
  }

  double normalizeUsageLimit(const json::value* value, double fallback) {
    return native_kernel::normalizeUsageLimit(value, fallback, api_keys::API_KEY_NO_USAGE_LIMIT);
  }
  double normalizeUsageLimit(const json::value& value, double fallback) { return normalizeUsageLimit(&value, fallback); }
  double normalizeUsageLimit(double value, double fallback) { return native_kernel::normalizeUsageLimit(value, fallback, api_keys::API_KEY_NO_USAGE_LIMIT); }

  double normalizeExpiration(const json::value* value) { return native_kernel::normalizeExpiration(value, api_keys::API_KEY_NO_EXPIRATION_MS); }
  double normalizeExpiration(const json::value& value) { return normalizeExpiration(&value); }
  double normalizeExpiration(double value) { return native_kernel::normalizeExpiration(value, api_keys::API_KEY_NO_EXPIRATION_MS); }

  std::optional<KernelQuotaPolicyV2> normalizeKernelQuotaPolicyV2(const json::value& value, const KernelQuotaScope& scope, const std::string& owner,
                                                                  const std::optional<std::string>& repo) {
    return native_kernel::normalizeKernelQuotaPolicyV2(value, scope, owner, repo, KERNEL_QUOTA_CONSTANTS);
  }

  KernelQuotaEffectivePolicy policyFor(const rt::KvEntryMaybe<>& entry, const KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo,
                                       const KernelDefaults& defaults) {
    return native_kernel::policyFor(kernelQuotaRead(entry), scope, owner, repo, defaults, KERNEL_QUOTA_CONSTANTS);
  }

  rt::Task<KernelDefaults> loadDefaults(std::shared_ptr<rt::Kv> kv) {
    rt::Promise<rt::KvEntryMaybe<>> limitRead(kv->get(defaults::DEFAULT_KERNEL_POLICY_LIMIT_KEY, STRONG));
    rt::Promise<rt::KvEntryMaybe<>> windowRead(kv->get(defaults::DEFAULT_KERNEL_POLICY_WINDOW_KEY, STRONG));
    auto reads = rt::all(limitRead, windowRead);
    auto readEntries = co_await reads.wait();
    auto& [limitEntry, windowEntry] = readEntries;
    const json::value limitValue = kernelQuotaValue(limitEntry.value);
    const json::value windowValue = kernelQuotaValue(windowEntry.value);
    KernelDefaults loaded;
    loaded.limit = normalizeUsageLimit(limitValue, defaults::DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS);
    loaded.windowMs = normalizeWindow(&windowValue, defaults::DEFAULT_KERNEL_POLICY_WINDOW_MS);
    loaded.limitEntry = std::move(limitEntry);
    loaded.windowEntry = std::move(windowEntry);
    co_return loaded;
  }

  KernelAuthLimitRecord repoRecord(const std::string& owner, const std::string& repo, const KernelPolicySummary& policy, const KernelQuotaWindowV2& window, double nowMs) {
    return KernelAuthLimitRecord{
      .owner = owner,
      .repo = repo,
      .usage_limit_requests = policy.limit,
      .usage_requests = window.usage_requests,
      .usage_reset_at_ms = window.usage_reset_at_ms,
      .window_ms = policy.windowMs,
      .expires_at_ms = policy.expiresAtMs,
      .created_at_ms = rt::truthy(window.created_at_ms) ? window.created_at_ms : nowMs,
      .updated_at_ms = rt::Math::max(window.updated_at_ms, nowMs),
    };
  }

  KernelOrgLimitRecord orgRecord(const std::string& owner, const KernelPolicySummary& policy, const KernelQuotaWindowV2& window, double nowMs) {
    return KernelOrgLimitRecord{
      .owner = owner,
      .usage_limit_requests = policy.limit,
      .usage_requests = window.usage_requests,
      .usage_reset_at_ms = window.usage_reset_at_ms,
      .window_ms = policy.windowMs,
      .expires_at_ms = policy.expiresAtMs,
      .created_at_ms = rt::truthy(window.created_at_ms) ? window.created_at_ms : nowMs,
      .updated_at_ms = rt::Math::max(window.updated_at_ms, nowMs),
    };
  }

  rt::Task<std::optional<KernelAuthLimitSnapshot>> getKernelUsageLimitSnapshot(std::string owner, std::string repo) {
    std::optional<std::string> scopedRepo = std::move(repo);
    rt::Task<std::optional<KernelLimitSnapshot>> loading = getSnapshot("repo", std::move(owner), std::move(scopedRepo));
    std::optional<KernelLimitSnapshot> snapshot = co_await std::move(loading);
    if (!snapshot) co_return std::nullopt;
    KernelAuthLimitSnapshot typed{.record = std::get<KernelAuthLimitRecord>(snapshot->record), .source = snapshot->source};
    co_return typed;
  }

  rt::Task<std::optional<KernelOrgLimitSnapshot>> getKernelOrgUsageLimitSnapshot(std::string owner) {
    rt::Task<std::optional<KernelLimitSnapshot>> loading = getSnapshot("org", std::move(owner), std::nullopt);
    std::optional<KernelLimitSnapshot> snapshot = co_await std::move(loading);
    if (!snapshot) co_return std::nullopt;
    KernelOrgLimitSnapshot typed{.record = std::get<KernelOrgLimitRecord>(snapshot->record), .source = snapshot->source};
    co_return typed;
  }

  rt::Task<std::optional<std::vector<KernelAuthLimitRecord>>> listKernelUsageLimits() {
    rt::Task<std::optional<std::vector<KernelLimitRecord>>> listing = listPolicies("repo");
    std::optional<std::vector<KernelLimitRecord>> rows = co_await std::move(listing);
    if (!rows) co_return std::nullopt;
    std::vector<KernelAuthLimitRecord> records;
    records.reserve(rows->size());
    for (KernelLimitRecord& row : *rows) records.push_back(std::get<KernelAuthLimitRecord>(std::move(row)));
    co_return records;
  }

  rt::Task<std::optional<std::vector<KernelOrgLimitRecord>>> listKernelOrgUsageLimits() {
    rt::Task<std::optional<std::vector<KernelLimitRecord>>> listing = listPolicies("org");
    std::optional<std::vector<KernelLimitRecord>> rows = co_await std::move(listing);
    if (!rows) co_return std::nullopt;
    std::vector<KernelOrgLimitRecord> records;
    records.reserve(rows->size());
    for (KernelLimitRecord& row : *rows) records.push_back(std::get<KernelOrgLimitRecord>(std::move(row)));
    co_return records;
  }
}
