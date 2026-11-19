// Links: tests/codex_reset_settings_migration_test.cpp
//        src/types.cpp src/cache/kv_migration_validation.cpp src/config.cpp src/cache/kv_migration_paid_fallback.cpp
//        src/authority/native_paid.cpp src/codex/manual_reset_state.cpp src/authority/native_api_key.cpp src/cache/kv_migration_usage.cpp
//        src/authority/native_paid_policy.cpp src/authority/native_api_key_policy.cpp src/api_keys.cpp src/codex/reset_settings.cpp
//        src/authority/native_kernel.cpp src/authority/native_kernel_contracts.cpp src/api_key_policy.cpp src/cache/kv_migration_base.cpp
//        src/kv_conversion/profile_json.cpp src/kv.cpp src/utils.cpp src/kernel/quota_v2.cpp
//        src/codex/auth.cpp src/kernel/quota_reservations.cpp src/codex/capacity_routing.cpp src/authority/native_codex_contracts.cpp
//        src/authority/native_codex_capacity.cpp src/codex/routing_state.cpp src/codex/banked_reset.cpp src/codex/routing_mutations.cpp
//        src/authority/native_codex_mutations.cpp src/runtime_config.cpp src/authority/native_codex_evaluation.cpp src/release.cpp
//        src/codex/native_auth.cpp src/models/codex_models.cpp src/codex/external_auth.cpp src/storage/kv.cpp
//        src/http.cpp src/storage/sqlite_readonly.cpp src/codex/rate_limit_429.cpp src/provider/health.cpp
//        src/defaults.cpp src/authority/native_codex_429.cpp src/codex/native_auth_transport.cpp src/storage/native_kv.cpp
//        src/codex/banked_reset_provider.cpp src/provider/cerebras_rate_limits.cpp src/auth/relay.cpp
//        src/bounded_response_body.cpp src/runtime/websocket.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp
// Port of tests/codex-reset-settings-migration.test.ts, all nine cases.
//
//
// `openSentinelTestKv()` is `openKv(":memory:")`, so the cases run on rt::MemoryKv. The racing wrapper
// of the last case is a ForwardingKv whose first atomic commit appends an account to the stored pool
// before committing. Credential isolation: HOME, CODEX_HOME and CODEX_AUTH_JSON_B64 are cleared (and
// config.codexAuthJsonB64) for every case.
#include "uos/cache/kv_migration_base.hpp"
#include "uos/cache/kv_migration_validation.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/reset_settings.hpp"
#include "uos/config.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/types.hpp"
#include "uos/utils.hpp"

#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace auth = uos::codex::auth;
using namespace uos::codex::reset_settings;
using uos::types::CodexAuthPoolState;
using uos::types::CodexAuthState;

#define AWAIT(name, expression)   \
  auto name##Task = (expression); \
  auto name = co_await std::move(name##Task)
#define AWAIT_DONE(name, expression) \
  auto name##Task = (expression);    \
  co_await std::move(name##Task)

namespace {
  struct IsolatedEnvironment {
    rt::env::ScopedOverride environment{{"HOME", std::nullopt}, {"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt}};
    std::string savedAuthJson = uos::config::config.codexAuthJsonB64;
    IsolatedEnvironment() { uos::config::config.codexAuthJsonB64.clear(); }
    ~IsolatedEnvironment() { uos::config::config.codexAuthJsonB64 = savedAuthJson; }
    IsolatedEnvironment(const IsolatedEnvironment&) = delete;
    IsolatedEnvironment& operator=(const IsolatedEnvironment&) = delete;
  };

  const std::string ACCOUNT_ID_PREFIX = "issue-286-account";
  constexpr double ACCOUNT_UPDATED_AT_MS = 1'700'000'000'000;

  std::string accountId(const std::string& suffix) { return ACCOUNT_ID_PREFIX + ":" + suffix; }

  rt::Task<std::string> accountHash(std::string suffix) {
    rt::Task<std::string> hashing = uos::utils::sha256Hex(accountId(suffix));
    std::string hash = co_await std::move(hashing);
    co_return hash;
  }

  CodexAuthState authAccount(const std::string& suffix) {
    CodexAuthState account;
    account.access_token = "access-token-" + suffix;
    account.refresh_token = "refresh-token-" + suffix;
    account.account_id = accountId(suffix);
    account.updated_at_ms = ACCOUNT_UPDATED_AT_MS;
    return account;
  }

  std::shared_ptr<rt::MemoryKv> openSentinelTestKv() { return std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); }); }

  /** Persists the exact strong auth-pool snapshot the completion enumerates. */
  rt::Task<void> setAuthPool(std::shared_ptr<rt::Kv> kv, std::vector<std::string> suffixes) {
    CodexAuthPoolState pool;
    for (const std::string& suffix : suffixes) pool.accounts.push_back(authAccount(suffix));
    pool.updated_at_ms = ACCOUNT_UPDATED_AT_MS + 1;
    const rt::KvValue value(json::value_from(pool));
    rt::Task<rt::KvCommitResult> setting = kv->set(auth::CODEX_AUTH_POOL_KV_KEY, value);
    co_await std::move(setting);
  }

  rt::Task<json::value> valueAt(std::shared_ptr<rt::Kv> kv, rt::KvKey key) {
    rt::Task<rt::KvEntryMaybe<>> reading = kv->get(std::move(key));
    const rt::KvEntryMaybe<> entry = co_await std::move(reading);
    co_return entry.value.toJson();
  }

  // `MigrationMarker | null`
  rt::Task<json::value> migrationMarker(std::shared_ptr<rt::Kv> kv) {
    rt::Task<json::value> reading = valueAt(kv, CODEX_RESET_USAGE_MIGRATION_KEY);
    json::value marker = co_await std::move(reading);
    co_return marker;
  }

  rt::Task<std::vector<rt::KvEntry<>>> migrationMarkers(std::shared_ptr<rt::Kv> kv) {
    rt::KvListSelector selector;
    selector.prefix = rt::KvKey{"uos_ai", "migrations"};
    std::unique_ptr<rt::KvListIterator> iterator = kv->list(std::move(selector));
    rt::Task<std::vector<rt::KvEntry<>>> listing = rt::kvListToArray(*iterator);
    std::vector<rt::KvEntry<>> markers = co_await std::move(listing);
    co_return markers;
  }

  rt::Task<bool> allowed(std::shared_ptr<rt::Kv> kv, std::string hash) {
    rt::Task<ReadCodexResetUsageResult> reading = readCodexResetUsage(std::move(kv), std::move(hash));
    const ReadCodexResetUsageResult usage = co_await std::move(reading);
    co_return usage.allowed;
  }

  json::value enabledObject(bool enabled) {
    json::object value;
    value["enabled"] = enabled;
    return value;
  }

  bool preservedOptOut(const json::value& marker) { return rt::getBool(marker, "preserved_opt_out") == true; }

  // The shared state of the racing wrapper (`injected` and the account it appends).
  struct InjectionState {
    rt::Kv& kv;
    std::string suffix;
    bool injected = false;
  };

  rt::Task<void> appendConfiguredAccount(std::shared_ptr<InjectionState> state) {
    const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
    rt::Task<rt::KvEntryMaybe<>> reading = state->kv.get(auth::CODEX_AUTH_POOL_KV_KEY, strong);
    const rt::KvEntryMaybe<> entry = co_await std::move(reading);
    const std::optional<CodexAuthPoolState> pool = auth::parseCodexAuthPool(entry.value.toJson());
    if (pool) {
      CodexAuthPoolState next;
      next.accounts = pool->accounts;
      next.accounts.push_back(authAccount(state->suffix));
      next.updated_at_ms = rt::dateNow();
      const rt::KvValue value(json::value_from(next));
      rt::Task<rt::KvCommitResult> setting = state->kv.set(auth::CODEX_AUTH_POOL_KV_KEY, value);
      co_await std::move(setting);
    }
  }

  class InjectingAtomicOperation : public rt::ForwardingAtomicOperation {
  public:
    InjectingAtomicOperation(std::unique_ptr<rt::AtomicOperation> inner_, std::shared_ptr<InjectionState> state_)
      : rt::ForwardingAtomicOperation(std::move(inner_)), state(std::move(state_)) {}
    rt::Task<rt::KvCommitResult> commit() override {
      if (!state->injected) {
        state->injected = true;
        rt::Task<void> appending = appendConfiguredAccount(state);
        co_await std::move(appending);
      }
      rt::Task<rt::KvCommitResult> committing = inner->commit();
      const rt::KvCommitResult result = co_await std::move(committing);
      co_return result;
    }

  private:
    std::shared_ptr<InjectionState> state;
  };

  /**
   * A KV whose first atomic commit appends a configured account to the persisted
   * auth pool before committing, so the account lands exactly between the
   * completion's enumeration read and its conditional commit attempt.
   */
  class AddAccountBeforeFirstCommit : public rt::ForwardingKv {
  public:
    AddAccountBeforeFirstCommit(rt::Kv& kv_, std::string suffix) : rt::ForwardingKv(kv_), state(std::make_shared<InjectionState>(InjectionState{kv_, std::move(suffix)})) {}
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<InjectingAtomicOperation>(inner.atomic(), state); }

  private:
    std::shared_ptr<InjectionState> state;
  };

  template <class... T> rt::Task<std::vector<std::string>> hashesOf(T... suffixes) {
    std::vector<rt::Promise<std::string>> hashing;
    (hashing.push_back(rt::Promise<std::string>(accountHash(std::string(suffixes)))), ...);
    auto all = rt::all(std::move(hashing));
    std::vector<std::string> hashes = co_await all.wait();
    co_return hashes;
  }
}

ASYNC_TEST("a persisted false opt-out is preserved while explicit current settings win") {
  IsolatedEnvironment isolation;
  const std::shared_ptr<rt::MemoryKv> kv = openSentinelTestKv();
  AWAIT(hashes, hashesOf("a", "b", "c"));
  const std::string hashA = hashes[0], hashB = hashes[1], hashC = hashes[2];
  AWAIT_DONE(legacy, kv->set(CODEX_BANKED_RESET_USAGE_LEGACY_KEY, rt::KvValue(false)));
  AWAIT_DONE(explicitB, kv->set(codexResetUsageKey(hashB), rt::KvValue(enabledObject(true))));
  AWAIT_DONE(pooled, setAuthPool(kv, {"a", "b"}));

  AWAIT(migration, migrateLegacyCodexResetOptOut(kv));
  EXPECT_EQ(migration, std::string("done"));

  AWAIT(valueA, valueAt(kv, codexResetUsageKey(hashA)));
  EXPECT(rt::deepEqual(valueA, enabledObject(false)));
  AWAIT(valueB, valueAt(kv, codexResetUsageKey(hashB)));
  EXPECT(rt::deepEqual(valueB, enabledObject(true)));
  AWAIT(marker, migrationMarker(kv));
  EXPECT(preservedOptOut(marker));
  AWAIT(markerAgain, migrationMarker(kv));
  EXPECT_EQ(rt::typeOf(rt::get(markerAgain, "completed_at_ms")), std::string("number"));
  AWAIT(legacyValue, valueAt(kv, CODEX_BANKED_RESET_USAGE_LEGACY_KEY));
  EXPECT(rt::deepEqual(legacyValue, json::value(false)));

  AWAIT(allowedA, allowed(kv, hashA));
  EXPECT_EQ(allowedA, false);
  AWAIT(allowedB, allowed(kv, hashB));
  EXPECT_EQ(allowedB, true);
  // A subscription that appears only after the completed migration follows current defaults.
  AWAIT(allowedC, allowed(kv, hashC));
  EXPECT_EQ(allowedC, true);
  kv->close();
}

ASYNC_TEST("a repeated migration never overwrites an operator decision") {
  IsolatedEnvironment isolation;
  const std::shared_ptr<rt::MemoryKv> kv = openSentinelTestKv();
  AWAIT(hashA, accountHash("a"));
  AWAIT_DONE(legacy, kv->set(CODEX_BANKED_RESET_USAGE_LEGACY_KEY, rt::KvValue(false)));
  AWAIT_DONE(pooled, setAuthPool(kv, {"a"}));
  AWAIT(first, migrateLegacyCodexResetOptOut(kv));
  EXPECT_EQ(first, std::string("done"));
  AWAIT(seeded, valueAt(kv, codexResetUsageKey(hashA)));
  EXPECT(rt::deepEqual(seeded, enabledObject(false)));

  AWAIT_DONE(enable, kv->set(codexResetUsageKey(hashA), rt::KvValue(enabledObject(true))));
  AWAIT(second, migrateLegacyCodexResetOptOut(kv));
  EXPECT_EQ(second, std::string("done"));
  AWAIT(kept, valueAt(kv, codexResetUsageKey(hashA)));
  EXPECT(rt::deepEqual(kept, enabledObject(true)));
  AWAIT(allowedA, allowed(kv, hashA));
  EXPECT_EQ(allowedA, true);
  kv->close();
}

ASYNC_TEST("a normal no-legacy startup materializes no settings and stays enabled") {
  IsolatedEnvironment isolation;
  {
    const std::shared_ptr<rt::MemoryKv> absentKv = openSentinelTestKv();
    AWAIT(hashA, accountHash("a"));
    AWAIT_DONE(pooled, setAuthPool(absentKv, {"a"}));
    AWAIT(migration, migrateLegacyCodexResetOptOut(absentKv));
    EXPECT_EQ(migration, std::string("done"));
    AWAIT(row, valueAt(absentKv, codexResetUsageKey(hashA)));
    EXPECT(row.is_null());
    AWAIT(marker, migrationMarker(absentKv));
    EXPECT(rt::getBool(marker, "preserved_opt_out") == false);
    AWAIT(allowedA, allowed(absentKv, hashA));
    EXPECT_EQ(allowedA, true);
    absentKv->close();
  }

  {
    const std::shared_ptr<rt::MemoryKv> enabledLegacyKv = openSentinelTestKv();
    AWAIT(hashA, accountHash("a"));
    AWAIT_DONE(legacy, enabledLegacyKv->set(CODEX_BANKED_RESET_USAGE_LEGACY_KEY, rt::KvValue(true)));
    AWAIT_DONE(pooled, setAuthPool(enabledLegacyKv, {"a"}));
    AWAIT(migration, migrateLegacyCodexResetOptOut(enabledLegacyKv));
    EXPECT_EQ(migration, std::string("done"));
    AWAIT(row, valueAt(enabledLegacyKv, codexResetUsageKey(hashA)));
    EXPECT(row.is_null());
    AWAIT(allowedA, allowed(enabledLegacyKv, hashA));
    EXPECT_EQ(allowedA, true);
    enabledLegacyKv->close();
  }
}

ASYNC_TEST("an empty account pool stays pending and guarded reads seed configured accounts") {
  IsolatedEnvironment isolation;
  const std::shared_ptr<rt::MemoryKv> kv = openSentinelTestKv();
  AWAIT(hashes, hashesOf("a", "b", "c"));
  const std::string hashA = hashes[0], hashB = hashes[1], hashC = hashes[2];
  AWAIT_DONE(legacy, kv->set(CODEX_BANKED_RESET_USAGE_LEGACY_KEY, rt::KvValue(false)));

  AWAIT(pending, migrateLegacyCodexResetOptOut(kv));
  EXPECT_EQ(pending, std::string("pending"));
  AWAIT(noMarker, migrationMarker(kv));
  EXPECT(noMarker.is_null());
  AWAIT(noRow, valueAt(kv, codexResetUsageKey(hashA)));
  EXPECT(noRow.is_null());

  // Accounts configured while the marker is still pending are seeded on read.
  AWAIT(allowedA, allowed(kv, hashA));
  EXPECT_EQ(allowedA, false);
  AWAIT(allowedB, allowed(kv, hashB));
  EXPECT_EQ(allowedB, false);
  AWAIT(seededA, valueAt(kv, codexResetUsageKey(hashA)));
  EXPECT(rt::deepEqual(seededA, enabledObject(false)));
  AWAIT(seededB, valueAt(kv, codexResetUsageKey(hashB)));
  EXPECT(rt::deepEqual(seededB, enabledObject(false)));

  AWAIT_DONE(pooled, setAuthPool(kv, {"a", "b"}));
  AWAIT(done, migrateLegacyCodexResetOptOut(kv));
  EXPECT_EQ(done, std::string("done"));
  AWAIT(marker, migrationMarker(kv));
  EXPECT(preservedOptOut(marker));
  // A subscription that first appears after the marker follows current defaults.
  AWAIT(allowedC, allowed(kv, hashC));
  EXPECT_EQ(allowedC, true);
  kv->close();
}

namespace {
  rt::Task<std::string> explicitWriteThenMigrate(std::shared_ptr<rt::Kv> kv, std::string hashB) {
    // An operator decision that races the seed always wins and can only
    // reject a transaction that read the row before it.
    rt::Task<rt::KvCommitResult> setting = kv->set(codexResetUsageKey(hashB), rt::KvValue(enabledObject(true)));
    co_await std::move(setting);
    rt::Promise<std::string> migrating(migrateLegacyCodexResetOptOut(kv));
    std::string migration = co_await migrating.wait();
    co_return migration;
  }
}

ASYNC_TEST("concurrent migrations and an interleaved explicit write converge on one marker") {
  IsolatedEnvironment isolation;
  const std::shared_ptr<rt::MemoryKv> kv = openSentinelTestKv();
  AWAIT(hashes, hashesOf("a", "b"));
  const std::string hashA = hashes[0], hashB = hashes[1];
  AWAIT_DONE(legacy, kv->set(CODEX_BANKED_RESET_USAGE_LEGACY_KEY, rt::KvValue(false)));
  AWAIT_DONE(pooled, setAuthPool(kv, {"a", "b"}));

  rt::Promise<std::string> firstMigration(migrateLegacyCodexResetOptOut(kv));
  rt::Promise<std::string> secondMigration(explicitWriteThenMigrate(kv, hashB));
  auto both = rt::all(std::move(firstMigration), std::move(secondMigration));
  const std::tuple<std::string, std::string> results = co_await both.wait();
  const std::string& first = std::get<0>(results);
  const std::string& second = std::get<1>(results);

  EXPECT(first == "done" || second == "done");
  AWAIT(valueA, valueAt(kv, codexResetUsageKey(hashA)));
  EXPECT(rt::deepEqual(valueA, enabledObject(false)));
  AWAIT(valueB, valueAt(kv, codexResetUsageKey(hashB)));
  EXPECT(rt::deepEqual(valueB, enabledObject(true)));
  AWAIT(markers, migrationMarkers(kv));
  EXPECT_EQ(markers.size(), std::size_t{1});
  kv->close();
}

ASYNC_TEST("a malformed legacy value stays fail-closed through the migration") {
  IsolatedEnvironment isolation;
  const std::shared_ptr<rt::MemoryKv> kv = openSentinelTestKv();
  AWAIT(hashes, hashesOf("a", "b"));
  const std::string hashA = hashes[0], hashB = hashes[1];
  AWAIT_DONE(pooled, setAuthPool(kv, {"a", "b"}));
  AWAIT_DONE(legacy, kv->set(CODEX_BANKED_RESET_USAGE_LEGACY_KEY, rt::KvValue("yes")));

  AWAIT(allowedBefore, allowed(kv, hashA));
  EXPECT_EQ(allowedBefore, false);
  AWAIT(migration, migrateLegacyCodexResetOptOut(kv));
  EXPECT_EQ(migration, std::string("done"));
  AWAIT(seededA, valueAt(kv, codexResetUsageKey(hashA)));
  EXPECT(rt::deepEqual(seededA, enabledObject(false)));
  AWAIT(marker, migrationMarker(kv));
  EXPECT(preservedOptOut(marker));
  AWAIT(legacyValue, valueAt(kv, CODEX_BANKED_RESET_USAGE_LEGACY_KEY));
  EXPECT(rt::deepEqual(legacyValue, json::value("yes")));
  AWAIT(allowedAfter, allowed(kv, hashA));
  EXPECT_EQ(allowedAfter, false);

  // A malformed current row stays fail-closed as well.
  json::object malformed;
  malformed["enabled"] = "yes";
  AWAIT_DONE(malformedRow, kv->set(codexResetUsageKey(hashB), rt::KvValue(malformed)));
  AWAIT(allowedB, allowed(kv, hashB));
  EXPECT_EQ(allowedB, false);
  kv->close();
}

ASYNC_TEST("a pre-existing row survives the completion commit and only absent rows are seeded") {
  IsolatedEnvironment isolation;
  const std::shared_ptr<rt::MemoryKv> kv = openSentinelTestKv();
  AWAIT(hashes, hashesOf("a", "b"));
  const std::string hashA = hashes[0], hashB = hashes[1];
  AWAIT_DONE(legacy, kv->set(CODEX_BANKED_RESET_USAGE_LEGACY_KEY, rt::KvValue(false)));
  AWAIT_DONE(pooled, setAuthPool(kv, {"a", "b"}));
  // As if a guarded read had already seeded A while the marker was absent.
  AWAIT_DONE(seeded, kv->set(codexResetUsageKey(hashA), rt::KvValue(enabledObject(false))));

  AWAIT(noMarker, migrationMarker(kv));
  EXPECT(noMarker.is_null());
  AWAIT(migration, migrateLegacyCodexResetOptOut(kv));
  EXPECT_EQ(migration, std::string("done"));
  AWAIT(valueA, valueAt(kv, codexResetUsageKey(hashA)));
  EXPECT(rt::deepEqual(valueA, enabledObject(false)));
  AWAIT(valueB, valueAt(kv, codexResetUsageKey(hashB)));
  EXPECT(rt::deepEqual(valueB, enabledObject(false)));
  AWAIT(marker, migrationMarker(kv));
  EXPECT(preservedOptOut(marker));
  kv->close();
}

ASYNC_TEST("an account configured between enumeration and the completion commit cannot escape the opt-out") {
  IsolatedEnvironment isolation;
  const std::shared_ptr<rt::MemoryKv> kv = openSentinelTestKv();
  AWAIT(hashes, hashesOf("a", "b", "c"));
  const std::string hashA = hashes[0], hashB = hashes[1], hashC = hashes[2];
  AWAIT_DONE(legacy, kv->set(CODEX_BANKED_RESET_USAGE_LEGACY_KEY, rt::KvValue(false)));
  AWAIT_DONE(pooled, setAuthPool(kv, {"a"}));
  const std::shared_ptr<rt::Kv> racing = std::make_shared<AddAccountBeforeFirstCommit>(*kv, "b");

  AWAIT(pending, migrateLegacyCodexResetOptOut(racing));
  EXPECT_EQ(pending, std::string("pending"));
  // The rejected transaction wrote nothing, so the marker is still absent and
  // neither account has escaped the persisted global opt-out yet.
  AWAIT(noMarker, migrationMarker(kv));
  EXPECT(noMarker.is_null());
  AWAIT(noRowA, valueAt(kv, codexResetUsageKey(hashA)));
  EXPECT(noRowA.is_null());
  AWAIT(noRowB, valueAt(kv, codexResetUsageKey(hashB)));
  EXPECT(noRowB.is_null());

  // The next guarded read retries the completion from a fresh snapshot and
  // seeds the account configured during the previous attempt.
  AWAIT(allowedB, allowed(racing, hashB));
  EXPECT_EQ(allowedB, false);
  AWAIT(seededA, valueAt(kv, codexResetUsageKey(hashA)));
  EXPECT(rt::deepEqual(seededA, enabledObject(false)));
  AWAIT(seededB, valueAt(kv, codexResetUsageKey(hashB)));
  EXPECT(rt::deepEqual(seededB, enabledObject(false)));
  AWAIT(marker, migrationMarker(kv));
  EXPECT(preservedOptOut(marker));
  // A subscription that appears after the completed migration follows current defaults.
  AWAIT(allowedC, allowed(kv, hashC));
  EXPECT_EQ(allowedC, true);
  kv->close();
}

ASYNC_TEST("the completion marker stays inside the migration validator's accepted namespace") {
  IsolatedEnvironment isolation;
  const auto kv = openSentinelTestKv(); auto closing = rt::scopeExit([kv] { kv->close(); });
  const rt::KvValue disabled(false); AWAIT_DONE(setting, kv->set(CODEX_BANKED_RESET_USAGE_LEGACY_KEY, disabled));
  const std::vector<std::string> accounts{"a"}; AWAIT_DONE(pool, setAuthPool(kv, accounts));
  AWAIT(migrated, migrateLegacyCodexResetOptOut(kv)); EXPECT_EQ(migrated, std::string("done"));
  const uos::cache::kv_migration_base::KvMigrationClassifyOptions options{.profile = "local", .includeCache = false, .includeLegacy = true};
  const auto decision = uos::cache::kv_migration_base::classifyKvMigrationKey(CODEX_RESET_USAGE_MIGRATION_KEY, options);
  EXPECT_EQ(decision.action, std::string("import")); EXPECT_EQ(decision.group, std::string("migrations"));
  AWAIT(validation, uos::cache::kv_migration_validation::validateKvMigrationTarget(kv));
  std::size_t resetErrors = 0; for (const auto& error : validation.errors) if (error.starts_with("codex reset usage")) ++resetErrors;
  EXPECT_EQ(resetErrors, 0u); EXPECT_EQ(validation.counts.codex_reset_usage, 1);
}
