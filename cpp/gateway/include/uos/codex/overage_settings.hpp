#pragma once
// Port of src/codex/overage-settings.ts.
// Per-account overage-usage setting: when an operator explicitly allows
// overage spending for a Codex subscription, an exhausted account may probe
// (and thereby serve through the paid backup balance) instead of waiting for a
// banked-reset redemption. The default is false: resets first and no overage
// spend, even when no redemption is currently available.
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/types.hpp"

#include <memory>
#include <string>

namespace uos::codex::overage_settings {
  inline const rt::KvKey CODEX_OVERAGE_USAGE_PREFIX{"uos_ai", "codex_overage_usage", "account", "v1"};

  /** The admin-facing account hash is the same unsalted hash the other settings surfaces use. */
  rt::KvKey codexOverageUsageKey(const std::string& accountIdHash);

  /** A strong read; an absent or malformed row never permits overage spending. */
  [[nodiscard]] rt::Task<bool> readOverageUsage(std::shared_ptr<rt::Kv> kv, std::string accountIdHash);

  [[nodiscard]] rt::Task<void> writeOverageUsage(std::shared_ptr<rt::Kv> kv, std::string accountIdHash, bool allow);

  /**
   * Refresh the routing-facing cache from the configured pool. The cache is keyed
   * by the routing identity hash because routing evaluation is synchronous; the
   * KV rows stay keyed by the admin-facing account hash. A KV failure keeps the
   * previous cache and its timestamp so the next request retries.
   */
  [[nodiscard]] rt::Task<void> loadOverageUsageSettings(::uos::types::CodexAuthPoolState pool, bool force = false);

  /** Routing evaluation's synchronous lookup. Absent cache entries are false. */
  bool overageUsageAllowedSync(const std::string& routingAccountIdHash);

  void resetOverageUsageCacheForTest();
}
