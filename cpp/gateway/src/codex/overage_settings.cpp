// Per-account overage-usage setting: when an operator explicitly allows
// overage spending for a Codex subscription, an exhausted account may probe
// (and thereby serve through the paid backup balance) instead of waiting for a
// banked-reset redemption. The default is false: resets first and no overage
// spend, even when no redemption is currently available.
#include "uos/codex/overage_settings.hpp"
#include "uos/rt/memory.hpp"

#include "uos/codex/capacity_routing.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/utils.hpp"

#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace uos::codex::overage_settings {
  namespace {
    /** The routing path evaluates this setting synchronously, so a bounded cache mirrors the KV rows. */
    constexpr double OVERAGE_USAGE_CACHE_TTL_MS = 5'000;

    std::unordered_set<std::string> allowedRoutingHashes;
    double cacheLoadedAtMs = 0;

    // `typeof value === "object" && value !== null` over a stored value.
    bool isRecord(const rt::KvValue& value) {
      return !(value.isUndefined() || value.isNull() || value.isBool() || value.isNumber() || value.isString() || value.isBigInt() || value.is<rt::KvHole>());
    }

    struct OverageRow {
      std::string routingHash;
      bool allow;
    };
  }

  /** The admin-facing account hash is the same unsalted hash the other settings surfaces use. */
  rt::KvKey codexOverageUsageKey(const std::string& accountIdHash) { return rt::KvKey{CODEX_OVERAGE_USAGE_PREFIX, accountIdHash}; }

  /** A strong read; an absent or malformed row never permits overage spending. */
  rt::Task<bool> readOverageUsage(std::shared_ptr<rt::Kv> kv, std::string accountIdHash) {
    const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
    auto reading = kv->get(codexOverageUsageKey(accountIdHash), strong);
    const rt::KvEntryMaybe<> entry = co_await std::move(reading);
    const rt::KvValue& allow = entry.value["allow"];
    co_return isRecord(entry.value) && allow.isBool() && allow.asBool();
  }

  rt::Task<void> writeOverageUsage(std::shared_ptr<rt::Kv> kv, std::string accountIdHash, bool allow) {
    json::object row;
    row["allow"] = allow;
    const rt::KvValue value(row);
    auto writing = kv->set(codexOverageUsageKey(accountIdHash), value);
    co_await std::move(writing);
  }

  /**
   * Refresh the routing-facing cache from the configured pool. The cache is keyed
   * by the routing identity hash because routing evaluation is synchronous; the
   * KV rows stay keyed by the admin-facing account hash. A KV failure keeps the
   * previous cache and its timestamp so the next request retries.
   */
  rt::Task<void> loadOverageUsageSettings(::uos::types::CodexAuthPoolState pool, bool force) {
    const double now = rt::dateNow();
    if (!force && now - cacheLoadedAtMs < OVERAGE_USAGE_CACHE_TTL_MS) co_return;
    try {
      rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (!kv) co_return;
      std::vector<rt::Promise<OverageRow>> pending;
      for (const ::uos::types::CodexAuthState& account : pool.accounts) {
        pending.push_back(rt::Promise<OverageRow>(rt::async([kv, account]() -> rt::Task<OverageRow> {
          rt::Promise<std::string> adminHashing(utils::sha256Hex(account.account_id));
          rt::Promise<capacity_routing::CodexRoutingAccountIdentity> identifying(capacity_routing::routingAccountIdentity(account));
          auto both = rt::all(adminHashing, identifying);
          const std::tuple<std::string, capacity_routing::CodexRoutingAccountIdentity> resolved = co_await both.wait();
          rt::Promise<bool> reading(readOverageUsage(kv, std::get<0>(resolved)));
          const bool allow = co_await reading.wait();
          OverageRow row;
          row.routingHash = std::get<1>(resolved).accountIdHash;
          row.allow = allow;
          co_return row;
        })));
      }
      rt::Promise<std::vector<OverageRow>> resolving = rt::all(std::move(pending));
      const std::vector<OverageRow> rows = co_await resolving.wait();
      std::unordered_set<std::string> allowed;
      for (const OverageRow& row : rows)
        if (row.allow) allowed.insert(row.routingHash);
      std::size_t bytes = 0;
      for (const auto& hash : allowed) bytes += sizeof(hash) + rt::memory::bytes(hash);
      allowedRoutingHashes = std::move(allowed);
      rt::memory::setCache(rt::memory::Cache::codex_overage, allowedRoutingHashes.size(), bytes);
      cacheLoadedAtMs = now;
    } catch (...) {
      // Keep the previous cache so an unreadable KV cannot silently disable a
      // setting mid-flight; the timestamp stays old so the next request retries.
    }
  }

  /** Routing evaluation's synchronous lookup. Absent cache entries are false. */
  bool overageUsageAllowedSync(const std::string& routingAccountIdHash) { return allowedRoutingHashes.contains(routingAccountIdHash); }

  void resetOverageUsageCacheForTest() {
    allowedRoutingHashes = {};
    cacheLoadedAtMs = 0;
  }
}
