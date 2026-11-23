#pragma once

#include <boost/json/object.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace uos::rt { class Timeout; }

namespace uos::rt::memory {
  enum class Cache {
    api_key_policy,
    codex_auth_pool,
    codex_routing,
    codex_catalog,
    codex_account_models,
    runtime_config,
    provider_selection,
    subscription_hashes,
    provider_health,
    static_assets,
    json_parser,
    sqlite_pages,
    sqlite_statements,
    github_auth,
    deploy_admin_auth,
    passport_jwks,
    passkey_crls,
    kernel_attestation_ids,
    kernel_public_keys,
    codex_overage,
    codex_supervisor_inventory,
    openrouter_models,
    openlux_models,
    surplus_models,
    lithos_models,
    lithos_rate_windows,
    claude_models,
    claude_credentials,
    deepseek_flash_aliases,
    codex_learned_model_details,
    sentinel_replay_owners,
    runtime_timers,
    runtime_pending_promises,
    count
  };

  // One owner can publish totals directly. Use ScopedCache for contributions
  // from several independently owned instances, such as SQLite connections.
  void setCache(Cache cache, std::size_t entries, std::size_t bytes) noexcept;

  class ScopedCache {
  public:
    explicit ScopedCache(Cache cache) noexcept : cache_(cache) {}
    ~ScopedCache() { set(0, 0); }
    ScopedCache(ScopedCache&& other) noexcept;
    ScopedCache(const ScopedCache&) = delete;
    ScopedCache& operator=(const ScopedCache&) = delete;
    void set(std::size_t entries, std::size_t bytes) noexcept;

  private:
    Cache cache_;
    std::size_t entries_ = 0;
    std::size_t bytes_ = 0;
  };

  // Retained payload and container storage estimates, sampled only on mutation.
  std::size_t bytes(const boost::json::value& value) noexcept;
  std::size_t bytes(const boost::json::object& value) noexcept;
  std::size_t bytes(const boost::json::array& value) noexcept;
  std::size_t bytes(const std::string& value) noexcept;
  std::size_t bytes(const std::vector<std::string>& value) noexcept;

  // The health route uses rollup for precise anonymous RSS. Terminals use
  // the kernel's fast statm counters and never walk the process mappings.
  boost::json::object snapshot(bool useSmaps, boost::json::storage_ptr storage = {});

  // Unreferenced maintenance timer owned by the native serving lifetime.
  Timeout startHeapReclamation();
}
