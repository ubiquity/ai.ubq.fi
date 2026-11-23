#include "uos/kernel/policy_queue.hpp"

#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"

#include <cstddef>
#include <exception>
#include <memory>
#include <utility>

namespace uos::kernel::policy_queue {
  namespace {
    using ::uos::types::KernelPolicyQueueItem;

    rt::KvKey UOS_KERNEL_POLICY_QUEUE_KEY() { return rt::KvKey{"uos_ai", "kernel_policy_queue"}; }
    constexpr std::size_t MAX_KERNEL_POLICY_QUEUE_ITEMS = 200;
    constexpr int MAX_KV_RETRIES = 3;

    double toNumber(const rt::KvValue& value, double fallback) {
      if (!value.isNumber() || !rt::Number::isFinite(value.asNumber())) return fallback;
      return rt::Math::trunc(value.asNumber());
    }

    // `getString(value)?.trim() ?? ""`
    std::string normalizeOwnerRepo(const rt::KvValue& value) {
      if (!value.isString()) return "";
      return rt::trim(value.asString());
    }
    std::string normalizeOwnerRepo(const std::string& value) { return rt::trim(value); }

    // `isRecord(value)` (src/utils.ts) on a stored value: `typeof value === "object" && value !==
    // null`, so arrays, Maps, Sets, Dates, typed arrays and KvU64 instances count.
    bool isRecord(const rt::KvValue& value) {
      return !(value.isUndefined() || value.isNull() || value.isBool() || value.isNumber() || value.isString() || value.isBigInt() ||
               value.is<rt::KvHole>());
    }

    // std::nullopt is null.
    std::optional<KernelPolicyQueueItem> normalizeQueueItem(const rt::KvValue& value, double nowMs) {
      if (!isRecord(value)) return std::nullopt;
      std::string owner = normalizeOwnerRepo(value["owner"]);
      std::string repo = normalizeOwnerRepo(value["repo"]);
      if (owner.empty() || repo.empty()) return std::nullopt;
      const double requestCount = rt::Math::max(0.0, toNumber(value["request_count"], 0));
      const double firstSeen = toNumber(value["first_seen_at_ms"], nowMs);
      const double lastSeen = toNumber(value["last_seen_at_ms"], nowMs);
      std::string lastRouteRaw = normalizeOwnerRepo(value["last_route"]);
      return KernelPolicyQueueItem{
        .owner = std::move(owner),
        .repo = std::move(repo),
        .request_count = requestCount,
        .first_seen_at_ms = firstSeen,
        .last_seen_at_ms = lastSeen,
        .last_route = !lastRouteRaw.empty() ? std::optional<std::string>(std::move(lastRouteRaw)) : std::nullopt,
      };
    }

    // `(Array.isArray(entry.value) ? entry.value : []).map(normalizeQueueItem).filter(Boolean)`
    std::vector<KernelPolicyQueueItem> normalizeQueue(const rt::KvValue& stored, double nowMs) {
      std::vector<KernelPolicyQueueItem> normalized;
      if (!stored.isArray()) return normalized;
      for (const rt::KvValue& item : stored.asArray()) {
        if (std::optional<KernelPolicyQueueItem> next = normalizeQueueItem(item, nowMs)) normalized.push_back(std::move(*next));
      }
      return normalized;
    }

    void sortByLastSeen(std::vector<KernelPolicyQueueItem>& items) {
      rt::sort(items, [](const KernelPolicyQueueItem& a, const KernelPolicyQueueItem& b) { return b.last_seen_at_ms - a.last_seen_at_ms; });
    }

    rt::KvValue toStoredQueue(const std::vector<KernelPolicyQueueItem>& items) {
      rt::KvArray stored;
      stored.reserve(items.size());
      for (const KernelPolicyQueueItem& item : items) stored.emplace_back(json::value_from(item));
      return rt::KvValue(std::move(stored));
    }
  }

  rt::Task<void> recordKernelPolicyQueue(std::string owner, std::string repo, std::string route) {
    rt::Reason failure;
    try {
      const std::string ownerName = normalizeOwnerRepo(owner);
      const std::string repoName = normalizeOwnerRepo(repo);
      if (ownerName.empty() || repoName.empty()) co_return;
      const std::string routeValue = normalizeOwnerRepo(route);
      const std::optional<std::string> routeEntry = !routeValue.empty() ? std::optional<std::string>(routeValue) : std::nullopt;

      auto opening = ::uos::kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (!kv) co_return;
      const double nowMs = rt::dateNow();
      const rt::KvKey queueKey = UOS_KERNEL_POLICY_QUEUE_KEY();

      for (int attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
        const rt::KvEntryMaybe<> entry = co_await kv->get(queueKey);
        const std::vector<KernelPolicyQueueItem> normalized = normalizeQueue(entry.value, nowMs);
        std::ptrdiff_t index = -1;
        for (std::size_t position = 0; position < normalized.size(); ++position) {
          if (normalized[position].owner == ownerName && normalized[position].repo == repoName) {
            index = static_cast<std::ptrdiff_t>(position);
            break;
          }
        }
        std::vector<KernelPolicyQueueItem> next = normalized;

        if (index >= 0) {
          const KernelPolicyQueueItem& current = normalized[static_cast<std::size_t>(index)];
          KernelPolicyQueueItem updated = current;
          updated.request_count = current.request_count + 1;
          updated.last_seen_at_ms = nowMs;
          updated.last_route = routeEntry ? routeEntry : current.last_route;
          next[static_cast<std::size_t>(index)] = std::move(updated);
        } else {
          next.clear();
          next.push_back(KernelPolicyQueueItem{
            .owner = ownerName,
            .repo = repoName,
            .request_count = 1,
            .first_seen_at_ms = nowMs,
            .last_seen_at_ms = nowMs,
            .last_route = routeEntry,
          });
          next.insert(next.end(), normalized.begin(), normalized.end());
        }

        sortByLastSeen(next);
        if (next.size() > MAX_KERNEL_POLICY_QUEUE_ITEMS) next.resize(MAX_KERNEL_POLICY_QUEUE_ITEMS);

        const rt::KvValue stored = toStoredQueue(next);
        const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        operation->check(entry).set(queueKey, stored);
        const rt::KvCommitResult commit = co_await operation->commit();
        if (commit.ok) co_return;
      }

      rt::console::warn("[ai.ubq.fi] Failed to record kernel policy queue after retries:", ownerName + "/" + repoName);
      co_return;
    } catch (...) {
      failure = std::current_exception();
    }
    rt::console::warn("[ai.ubq.fi] Failed to record kernel policy queue:", failure);
  }

  rt::Task<std::optional<std::vector<KernelPolicyQueueItem>>> listKernelPolicyQueue() {
    rt::Reason failure;
    try {
      auto opening = ::uos::kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (!kv) co_return std::nullopt;
      const double nowMs = rt::dateNow();
      const rt::KvKey queueKey = UOS_KERNEL_POLICY_QUEUE_KEY();
      const rt::KvEntryMaybe<> entry = co_await kv->get(queueKey);
      std::vector<KernelPolicyQueueItem> normalized = normalizeQueue(entry.value, nowMs);
      sortByLastSeen(normalized);
      co_return normalized;
    } catch (...) {
      failure = std::current_exception();
    }
    rt::console::warn("[ai.ubq.fi] Failed to list kernel policy queue:", failure);
    co_return std::nullopt;
  }
}
