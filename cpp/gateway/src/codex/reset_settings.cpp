// Port of src/codex/reset-settings.ts.
#include "uos/codex/reset_settings.hpp"

#include "uos/codex/auth.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/types.hpp"
#include "uos/utils.hpp"

#include <optional>
#include <tuple>
#include <utility>

namespace uos::codex::reset_settings {
  namespace {
    rt::KvGetOptions strongRead() { return rt::KvGetOptions{.consistency = rt::KvConsistencyLevel::strong}; }

    rt::Task<rt::KvEntryMaybe<>> readWithStore(std::shared_ptr<rt::Kv> kv, rt::KvKey key) {
      auto read = kv->get(std::move(key), strongRead());
      return rt::detail::keepAlive(std::move(kv), std::move(read));
    }

    // A stored row as the TS reads it (`unknown`): JSON-shaped parts as they are, an undefined member
    // absent and an undefined element or hole null; any other value (a Date, Map, Uint8Array, KvU64,
    // bigint, ...) a member-less object. parseCodexAuthPool rejects that object exactly where it
    // rejects the original value (it reads only arrays, strings, finite numbers and records).
    json::value storedJson(const rt::KvValue& value) {
      if (value.isNull() || value.isUndefined() || value.is<rt::KvHole>()) return nullptr;
      if (value.isBool()) return value.asBool();
      if (value.isNumber()) return rt::makeNumber(value.asNumber());
      if (value.isString()) return json::string(value.asString());
      if (value.isArray()) {
        json::array items;
        for (const rt::KvValue& item : value.asArray()) items.push_back(storedJson(item));
        return items;
      }
      if (value.isObject()) {
        json::object members;
        for (const auto& [key, member] : value.asObject()) {
          if (member.isUndefined()) continue;
          rt::objectDefine(members, key, storedJson(member));
        }
        return members;
      }
      return json::object();
    }

    /**
     * The pre-migration switch was fail-closed: only an explicit `true` or an
     * absent row allowed a reset, so an explicit `false` and every malformed value
     * are an opt-out the migration must preserve.
     */
    bool isLegacyOptOut(const rt::KvValue& value) { return (value.isBool() && !value.asBool()) || (!value.isNull() && !value.isBool()); }

    rt::KvValue disabledUsage() {
      json::object usage;
      usage["enabled"] = false;
      return rt::KvValue(usage);
    }
  }

  rt::KvKey codexResetUsageKey(const std::string& accountIdHash) { return rt::KvKey{CODEX_RESET_USAGE_PREFIX, "account", "v1", accountIdHash}; }

  rt::Task<std::string> migrateLegacyCodexResetOptOut(std::shared_ptr<rt::Kv> kv) {
    rt::Promise<rt::KvEntryMaybe<>> markerRead(kv->get(CODEX_RESET_USAGE_MIGRATION_KEY, strongRead()));
    const rt::KvEntryMaybe<> marker = co_await markerRead.wait();
    if (!marker.value.isNull()) co_return std::string("done");

    rt::Promise<rt::KvEntryMaybe<>> legacyRead(readWithStore(kv, CODEX_BANKED_RESET_USAGE_LEGACY_KEY));
    rt::Promise<rt::KvEntryMaybe<>> poolRead(readWithStore(kv, auth::CODEX_AUTH_POOL_KV_KEY));
    auto reads = rt::all(std::move(legacyRead), std::move(poolRead));
    const std::tuple<rt::KvEntryMaybe<>, rt::KvEntryMaybe<>> both = co_await reads.wait();
    const rt::KvEntryMaybe<>& legacy = std::get<0>(both);
    const rt::KvEntryMaybe<>& poolEntry = std::get<1>(both);
    const json::value poolValue = storedJson(poolEntry.value);
    const std::optional<::uos::types::CodexAuthPoolState> pool = auth::parseCodexAuthPool(poolValue);
    if (!pool) co_return std::string("pending");

    const bool preservedOptOut = isLegacyOptOut(legacy.value);
    std::vector<std::string> accountIdHashes;
    if (preservedOptOut) {
      std::vector<rt::Promise<std::string>> hashing;
      for (const ::uos::types::CodexAuthState& account : pool->accounts) hashing.push_back(rt::Promise<std::string>(utils::sha256Hex(account.account_id)));
      auto hashed = rt::all(std::move(hashing));
      accountIdHashes = co_await hashed.wait();
    }
    std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
    atomic->check(poolEntry).check(legacy).check(marker);
    for (const std::string& accountIdHash : accountIdHashes) {
      const rt::KvKey key = codexResetUsageKey(accountIdHash);
      rt::Promise<rt::KvEntryMaybe<>> reading(kv->get(key, strongRead()));
      const rt::KvEntryMaybe<> entry = co_await reading.wait();
      if (entry.value.isNull()) atomic->check(entry).set(key, disabledUsage());
    }
    json::object completion;
    completion["completed_at_ms"] = rt::makeNumber(rt::dateNow());
    completion["preserved_opt_out"] = preservedOptOut;
    atomic->set(CODEX_RESET_USAGE_MIGRATION_KEY, rt::KvValue(completion));
    rt::Promise<rt::KvCommitResult> committing(atomic->commit());
    const rt::KvCommitResult committed = co_await committing.wait();
    co_return std::string(committed.ok ? "done" : "pending");
  }

  namespace {
    /**
     * Admission guard for the read path while the completion marker is absent.
     * If completion is still `pending` (empty pool or a snapshot conflict), this
     * seeds the account only when the persisted global opt-out applies and it has
     * no explicit per-subscription row. The seed is fenced on the marker staying
     * absent, so an account that first appears after a completed migration follows
     * current defaults instead of inheriting a seeded opt-out.
     */
    rt::Task<void> ensureLegacyGlobalOptOutMigratedForAccount(std::shared_ptr<rt::Kv> kv, std::string accountIdHash) {
      rt::Promise<rt::KvEntryMaybe<>> markerRead(readWithStore(kv, CODEX_RESET_USAGE_MIGRATION_KEY));
      rt::Promise<rt::KvEntryMaybe<>> legacyRead(readWithStore(kv, CODEX_BANKED_RESET_USAGE_LEGACY_KEY));
      auto reads = rt::all(std::move(markerRead), std::move(legacyRead));
      const std::tuple<rt::KvEntryMaybe<>, rt::KvEntryMaybe<>> both = co_await reads.wait();
      const rt::KvEntryMaybe<>& marker = std::get<0>(both);
      const rt::KvEntryMaybe<>& legacy = std::get<1>(both);
      if (!marker.value.isNull() || !isLegacyOptOut(legacy.value)) co_return;
      const rt::KvKey key = codexResetUsageKey(accountIdHash);
      std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      atomic->check(marker).check(rt::AtomicCheck(key, std::nullopt)).set(key, disabledUsage());
      rt::Promise<rt::KvCommitResult> committing(atomic->commit());
      co_await committing.wait();
    }

    // `isRecord(value) && value.enabled === true`: only a plain object can carry an own `enabled`.
    bool enabledIsTrue(const rt::KvValue& value) {
      if (!value.isObject()) return false;
      const rt::KvValue& enabled = value["enabled"];
      return enabled.isBool() && enabled.asBool();
    }
  }

  rt::Task<ReadCodexResetUsageResult> readCodexResetUsage(std::shared_ptr<rt::Kv> kv, std::string accountIdHash) {
    // A guarded read retries the bounded completion, so a snapshot conflict is
    // resolved by the next read instead of pinning the legacy switch forever.
    rt::Promise<std::string> migrating(migrateLegacyCodexResetOptOut(kv));
    const std::string migration = co_await migrating.wait();
    if (migration != "done") {
      rt::Promise<void> seeding(ensureLegacyGlobalOptOutMigratedForAccount(kv, accountIdHash));
      co_await seeding.wait();
    }
    rt::Promise<rt::KvEntryMaybe<>> reading(kv->get(codexResetUsageKey(accountIdHash), strongRead()));
    rt::KvEntryMaybe<> entry = co_await reading.wait();
    ReadCodexResetUsageResult result;
    result.allowed = entry.value.isNull() || enabledIsTrue(entry.value);
    result.entries.push_back(std::move(entry));
    co_return result;
  }
}
