// Links: tests/kv_migration_test.cpp src/cache/kv_migration.cpp
//        tests/helpers/counting_kv.cpp src/cache/kv_migration_projection.cpp src/paid_fallback/ledger_state.cpp src/authority/native_paid_contracts.cpp
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
// Port of tests/kv-migration.test.ts. The HTTP CLI appendBooleanParam case is deferred: its
// scripts/kv-migrate.ts module is outside the gateway manifest and has no C++ interface yet.
#include "uos/analytics.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/api_keys.hpp"
#include "uos/cache/kv_migration.hpp"
#include "uos/codex/reset_settings.hpp"
#include "uos/config.hpp"
#include "uos/kv_conversion/profile_json.hpp"
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/runtime_config.hpp"
#include "tests/helpers/counting_kv.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace migration = uos::cache::kv_migration;
namespace base = uos::cache::kv_migration_base;
namespace policy = uos::api_key_policy;
namespace reset = uos::codex::reset_settings;
namespace profile = uos::kv_conversion::profile_json;
namespace counting = uos::tests::helpers::counting_kv;
namespace paid = uos::paid_fallback::ledger_state;

#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
#define AWAIT_DONE(name, expression) auto name##Task = (expression); co_await std::move(name##Task)

namespace {
  constexpr double NOW = 1'800'000'000'000;
  constexpr double MINUTE = 60'000;
  const std::string HASH(43, 'd');
  const rt::KvKey CATALOG_KEY{"ubq_ai", "codex_models"};
  const rt::KvKey DEFAULT_MODEL_KEY{"default", "model"};
  const rt::KvKey DEFAULT_REASONING_KEY{"default", "reasoning_effort"};
  struct CredentialScope {
    rt::env::ScopedEnvironment environment{{{"HOME", "/nonexistent/w06c6-isolated"}}};
    std::string saved = uos::config::config.codexAuthJsonB64;
    CredentialScope() { uos::config::config.codexAuthJsonB64.clear(); rt::globalDateNow() = [] { return NOW; }; }
    ~CredentialScope() { uos::config::config.codexAuthJsonB64 = saved; }
  };
  rt::KvValue row(const std::string& text) { return rt::KvValue(rt::jsonParse(text)); }
  rt::KvValue patch(rt::KvValue value, const json::object& changes) {
    for (const auto& member : changes) value.asObject().set(std::string(member.key()), rt::KvValue(member.value()));
    return value;
  }
  class MapKv;
  class MapAtomic final : public rt::ForwardingAtomicOperation {
  public:
    MapAtomic(MapKv& store_, std::unique_ptr<rt::AtomicOperation> inner_) : rt::ForwardingAtomicOperation(std::move(inner_)), store(store_) {}
    rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override {
      if (options.expireIn) expiries.emplace_back(counting::encodeKey(key), *options.expireIn);
      writes.push_back(key); inner->set(std::move(key), std::move(value), options); return *this;
    }
    rt::Task<rt::KvCommitResult> commit() override;
  private:
    MapKv& store;
    std::vector<std::pair<std::string, double>> expiries;
    std::vector<rt::KvKey> writes;
  };
  class MapKv : public counting::CountingKv {
  public:
    rt::OrderedMap<std::string, double> expireIn;
    std::vector<rt::KvKey> directWrites;
    std::vector<rt::KvKey> atomicWrites;
    int commits = 0;
    std::function<bool(int, const std::vector<rt::KvKey>&)> refuseCommit{};
    std::function<void(const rt::KvKey&, int)> beforeGet{};
    int gets = 0;
    rt::KvValue value(const rt::KvKey& key) const {
      const auto* entry = entries.get(counting::encodeKey(key)); return entry ? entry->value : rt::KvValue(nullptr);
    }
    bool has(const rt::KvKey& key) const { return entries.has(counting::encodeKey(key)); }
    void erase(const rt::KvKey& key) { entries.delete_(counting::encodeKey(key)); }
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override {
      ++gets; if (beforeGet) beforeGet(key, gets);
      return counting::CountingKv::get(std::move(key), options);
    }
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value_, rt::KvSetOptions options = {}) override {
      directWrites.push_back(key); if (options.expireIn) expireIn.set(counting::encodeKey(key), *options.expireIn);
      return counting::CountingKv::set(std::move(key), std::move(value_), options);
    }
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<MapAtomic>(*this, counting::CountingKv::atomic()); }
  };
  rt::Task<rt::KvCommitResult> MapAtomic::commit() {
    ++store.commits;
    if (store.refuseCommit && store.refuseCommit(store.commits, writes)) { co_await rt::microtask(); co_return rt::KvCommitResult{}; }
    const auto result = co_await inner->commit();
    if (result.ok) {
      for (const auto& [key, expiry] : expiries) store.expireIn.set(key, expiry);
      for (const auto& key : writes) store.atomicWrites.push_back(key);
    }
    co_return result;
  }
  base::KvMigrationImportOptions options(bool overwrite = true, bool dryRun = false) { return {{"prod", false, false}, overwrite, dryRun}; }
  std::string entryLine(const rt::KvKey& key, const rt::KvValue& value) {
    return rt::jsonStringify(json::object{{"key", json::value_from(profile::keyToJson(key))}, {"value", profile::valueToJson(value)}, {"versionstamp", "00000000000000000000"}});
  }
  std::string errors(const base::KvMigrationValidationResult& result) { return rt::join(result.errors, "\n"); }
  rt::Task<void> rejectsMatching(rt::Task<migration::KvReadIncidentV2MigrationResult> task, std::string pattern) {
    bool rejected = false;
    try { co_await std::move(task); } catch (...) {
      rejected = true; EXPECT(rt::Regex(pattern).test(rt::reasonToString(std::current_exception())));
    }
    EXPECT(rejected);
  }
  void seedCatalog(MapKv& kv, const std::string& model = "gpt-5.5") {
    kv.seed(CATALOG_KEY, rt::KvValue(json::object{{"models", json::array{json::object{{"slug", model}, {"supported_reasoning_levels", json::array{"none", "medium"}}}}}, {"source", "chatgpt_codex"}, {"updated_at_ms", NOW}}));
    kv.seed(DEFAULT_MODEL_KEY, model); kv.seed(DEFAULT_REASONING_KEY, "medium");
  }
  struct KeyPair { std::string id; std::string hash; rt::KvKey idKey; rt::KvKey hashKey; };
  KeyPair seedApiKey(MapKv& kv, const std::string& id = "unlimited-paid-fallback-key", const std::string& hash = HASH,
    const json::object& sharedChanges = {}, const json::object& keyChanges = {}) {
    seedCatalog(kv);
    const rt::KvValue shared = patch(rt::KvValue(json::object{{"id", id}, {"expires_at_ms", -1}, {"revoked_at_ms", nullptr},
      {"usage_limit_requests", -1}, {"usage_requests", 0}, {"usage_reset_at_ms", NOW + MINUTE}, {"window_ms", MINUTE},
      {"paid_fallback_enabled", true}, {"paid_fallback_limit_microcredits", -1}, {"paid_fallback_spent_microcredits", 2'000'000},
      {"paid_fallback_reserved_microcredits", 0}, {"paid_fallback_reservation_request_id", "pending-unlimited-request"}}), sharedChanges);
    const auto record = patch(patch(shared, {{"name", "Unlimited paid fallback"}, {"prefix", "u_1234567890"}, {"hash", hash},
      {"created_at_ms", NOW - MINUTE}, {"paid_fallback_model_ids", json::array{"gpt-5.5"}}, {"paid_fallback_quota_per_credit", 500'000}, {"paid_fallback_pricing_checked_at_ms", NOW}}), keyChanges);
    KeyPair result{id, hash, uos::api_keys::apiKeyIdKey(id), uos::api_keys::apiKeyHashKey(hash)};
    kv.seed(result.idKey, record); kv.seed(result.hashKey, shared); return result;
  }
  void patchPair(MapKv& kv, const KeyPair& pair, const json::object& changes) {
    kv.seed(pair.idKey, patch(kv.value(pair.idKey), changes)); kv.seed(pair.hashKey, patch(kv.value(pair.hashKey), changes));
  }
  rt::KvValue pendingLedger(const std::string& keyId, const std::string& requestId = "pending-unlimited-request") {
    return rt::KvValue(json::object{{"id", requestId}, {"key_id", keyId}, {"provider", "metered"}, {"billing_status", "pending"}, {"created_at_ms", NOW}});
  }
  rt::KvKey ledgerKey(const std::string& keyId, const std::string& requestId, bool legacy = false, double created = NOW) {
    if (legacy) return {"ubq_ai", "api_keys", "request_log", keyId, created, requestId};
    return {"uos_ai", "paid_fallback", "ledger", keyId, created, requestId};
  }
  rt::KvValue kernelWindow(const std::string& owner, const std::string& scope = "org") {
    auto result = rt::KvValue(json::object{{"v", 2}, {"scope", scope}, {"owner", owner}, {"usage_requests", 0},
      {"usage_reset_at_ms", NOW + MINUTE}, {"applied_window_ms", MINUTE}, {"created_at_ms", NOW}, {"updated_at_ms", NOW}});
    if (scope == "repo") result.asObject().set("repo", "repo");
    return result;
  }
  rt::KvValue kernelReservation(const std::string& owner, const std::string& requestId) {
    return rt::KvValue(json::object{{"v", 2}, {"scope", "org"}, {"owner", owner}, {"request_id", requestId}, {"route", "responses"},
      {"window_created_at_ms", NOW}, {"window_reset_at_ms", NOW + MINUTE}, {"state", "reserved"}, {"terminal_intent", nullptr},
      {"reserved_at_ms", NOW}, {"lease_expires_at_ms", NOW + 30'000}, {"committed_at_ms", nullptr}, {"released_at_ms", nullptr}, {"release_reason", nullptr}});
  }
}

TEST("KV migration classifies v2 incident state and skips the transient circuit") {
  const base::KvMigrationClassifyOptions opts{"prod", false, false};
  const auto classify = [&](const rt::KvKey& key) { return migration::classifyKvMigrationKey(key, opts); };
  EXPECT_EQ(classify({"uos_ai", "api_key_usage", "v2", "id"}).action, "import");
  EXPECT_EQ(classify({"uos_ai", "paid_fallback", "ledger", "id"}).action, "import");
  const std::vector<std::pair<rt::KvKey, std::string>> groups{
    {{"uos_ai", "paid_fallback", "v3", "window", "id", 123}, "paid_fallback_v3_windows"},
    {{"uos_ai", "paid_fallback", "v3", "request", "id", "request"}, "paid_fallback_v3_requests"},
    {{"uos_ai", "paid_fallback", "v3", "pending", "id", "request"}, "paid_fallback_v3_pending"},
    {{"uos_ai", "paid_fallback", "v3", "reconciliation_lease", "id"}, "paid_fallback_v3_reconciliation_leases"},
    {{"uos_ai", "paid_fallback", "v3", "deletion_guard", "id"}, "paid_fallback_v3_deletion_guards"},
    {{"uos_ai", "kernel_quota", "v2", "repo_reservation", "owner", "repo", 123, "request"}, "kernel_quota_v2_repo_reservation"},
    {{"uos_ai", "kernel_quota", "v2", "org_reservation", "owner", 123, "request"}, "kernel_quota_v2_org_reservation"}};
  for (const auto& [key, group] : groups) EXPECT_EQ(classify(key).group, group);
  EXPECT_EQ(classify({"uos_ai", "runtime_config", "v2"}).action, "import");
  EXPECT_EQ(classify(reset::codexResetUsageKey("account-hash")).action, "import");
  EXPECT_EQ(classify(reset::codexResetUsageKey("account-hash")).group, "codex_reset_usage");
  EXPECT_EQ(classify({"uos_ai", "codex_rate_limit"}).group, "unknown");
}

ASYNC_TEST("KV migration restores bounded TTLs and skips expired usage rows") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); const auto opts = options();
  const rt::KvKey freshKey{"uos_ai", "api_key_usage", "v3", "window", "ttl-key", "policy", NOW};
  const rt::KvKey expiredKey{"uos_ai", "api_key_usage", "v3", "window", "expired-key", "policy", NOW - 1};
  std::vector<std::string> lines{entryLine(freshKey, rt::KvValue(json::object{{"window_reset_at_ms", NOW + MINUTE}}))};
  AWAIT(fresh, migration::importKvMigrationLines(kv, lines, opts)); EXPECT_EQ(fresh.imported, 1); EXPECT(*kv->expireIn.get(counting::encodeKey(freshKey)) > 0);
  lines = {entryLine(expiredKey, rt::KvValue(json::object{{"window_reset_at_ms", NOW - policy::API_KEY_USAGE_V3_RETENTION_MS - 1}}))};
  AWAIT(expired, migration::importKvMigrationLines(kv, lines, opts)); EXPECT_EQ(expired.imported, 0); EXPECT_EQ(expired.skipped, 1); EXPECT(!kv->has(expiredKey));
}

ASYNC_TEST("KV migration anchors V3 request TTLs to their usage window") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); const auto opts = options();
  const double windowMs = 30 * 24 * 60 * MINUTE; const double start = NOW - 8 * 24 * 60 * MINUTE;
  const rt::KvKey key{"uos_ai", "api_key_usage", "v3", "request", "ttl-key", "v3:" + rt::numberToString(windowMs), start, "request"};
  std::vector<std::string> lines{entryLine(key, rt::KvValue(json::object{{"reserved_at_ms", start}}))};
  AWAIT(result, migration::importKvMigrationLines(kv, lines, opts)); EXPECT_EQ(result.imported, 1); EXPECT(*kv->expireIn.get(counting::encodeKey(key)) > policy::API_KEY_USAGE_V3_RETENTION_MS);
}

ASYNC_TEST("KV migration anchors legacy request-log TTLs and skips stale or undated rows") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); const auto opts = options(); const double retention = uos::analytics::API_KEY_REQUEST_LOG_RETENTION_MS;
  const auto freshKey = ledgerKey("migration-legacy-log", "fresh", true, NOW - MINUTE);
  const auto staleKey = ledgerKey("migration-legacy-log", "stale", true, NOW - retention - 1);
  const auto undatedKey = ledgerKey("migration-legacy-log", "undated", true);
  std::vector<std::string> lines{entryLine(freshKey, rt::KvValue(json::object{{"created_at_ms", NOW - MINUTE}})),
    entryLine(staleKey, rt::KvValue(json::object{{"created_at_ms", NOW - retention - 1}})), entryLine(undatedKey, rt::KvValue(json::object{}))};
  AWAIT(result, migration::importKvMigrationLines(kv, lines, opts)); EXPECT_EQ(*kv->expireIn.get(counting::encodeKey(freshKey)), retention - MINUTE);
  EXPECT_EQ(result.imported, 1); EXPECT_EQ(result.skipped, 2); EXPECT_EQ(result.errors, 0);
  EXPECT_EQ(kv->value(freshKey), rt::KvValue(json::object{{"created_at_ms", NOW - MINUTE}})); EXPECT(!kv->has(staleKey)); EXPECT(!kv->has(undatedKey));
}

ASYNC_TEST("KV migration preserves skipped Kernel windows and reconciles only newly imported windows") {
  CredentialScope scope; const std::vector<std::string> scopes{"repo", "org"}; const auto opts = options(false);
  for (const auto& kind : scopes) {
    auto kv = std::make_shared<rt::MemoryKv>([] { return NOW; }); const std::string owner = "migration-no-overwrite-" + kind;
    const rt::KvKey key = kind == "repo" ? rt::KvKey{"uos_ai", "kernel_quota", "v2", "repo_window", owner, "repo"} : rt::KvKey{"uos_ai", "kernel_quota", "v2", "org_window", owner};
    const auto existing = patch(kernelWindow(owner, kind), {{"usage_requests", 7}}); const rt::KvSetOptions expiry{.expireIn = 120'000};
    AWAIT_DONE(seed, kv->set(key, existing, expiry)); AWAIT(before, kv->get(key));
    const auto expiryBefore = kv->database()->row(key)->expirationMs;
    std::vector<std::string> lines{entryLine(key, patch(existing, {{"usage_requests", 0}}))};
    AWAIT(result, migration::importKvMigrationLines(kv, lines, opts)); EXPECT_EQ(result.skipped, 1); EXPECT_EQ(result.imported, 0); EXPECT_EQ(result.errors, 0);
    AWAIT(after, kv->get(key)); EXPECT_EQ(after, before); EXPECT_EQ(kv->database()->row(key)->expirationMs, expiryBefore);
    AWAIT_DONE(remove, kv->delete_(key)); lines = {entryLine(key, existing)};
    AWAIT(imported, migration::importKvMigrationLines(kv, lines, opts)); EXPECT_EQ(imported.imported, 1); EXPECT_EQ(imported.errors, 0);
    AWAIT(final, kv->get(key)); EXPECT_EQ(final.value, patch(existing, {{"reserved_requests", 0}}));
  }
}

ASYNC_TEST("KV migration reconciles an imported Kernel quota reservation aggregate") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); const auto opts = options(); const std::string owner = "migration-reconcile-org";
  const rt::KvKey windowKey{"uos_ai", "kernel_quota", "v2", "org_window", owner};
  const rt::KvKey reservationKey{"uos_ai", "kernel_quota", "v2", "org_reservation", owner, NOW, "request"};
  std::vector<std::string> lines{entryLine(reservationKey, kernelReservation(owner, "request")), entryLine(windowKey, kernelWindow(owner))};
  AWAIT(result, migration::importKvMigrationLines(kv, lines, opts)); EXPECT_EQ(result.imported, 2); EXPECT_EQ(kv->value(windowKey)["reserved_requests"].asNumber(), 1);
}

ASYNC_TEST("KV migration validates Kernel quota reservation aggregates") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); const std::string owner = "migration-reservation-org"; const std::string requestId = "migration-reservation-request";
  const rt::KvKey windowKey{"uos_ai", "kernel_quota", "v2", "org_window", owner};
  const rt::KvKey reservationKey{"uos_ai", "kernel_quota", "v2", "org_reservation", owner, NOW, requestId};
  const auto window = patch(kernelWindow(owner), {{"reserved_requests", 1}}); const auto reservation = kernelReservation(owner, requestId);
  kv->seed(windowKey, window); kv->seed(reservationKey, reservation);
  AWAIT(valid, migration::validateKvMigrationTarget(kv)); EXPECT_EQ(valid.counts.kernel_v2_org_windows, 1); EXPECT_EQ(valid.counts.kernel_v2_org_reservations, 1); EXPECT(!rt::includes(errors(valid), "kernel quota V2"));
  kv->erase(windowKey); AWAIT(activeMissing, migration::validateKvMigrationTarget(kv)); EXPECT(rt::includes(errors(activeMissing), "kernel quota V2 active reservation has no window"));
  kv->seed(reservationKey, patch(reservation, {{"state", "committed"}, {"committed_at_ms", NOW + 1}}));
  AWAIT(committed, migration::validateKvMigrationTarget(kv)); EXPECT(!rt::includes(errors(committed), "active reservation has no window"));
  kv->seed(reservationKey, patch(reservation, {{"state", "released"}, {"released_at_ms", NOW + 1}, {"release_reason", "request_cancelled"}}));
  AWAIT(released, migration::validateKvMigrationTarget(kv)); EXPECT(!rt::includes(errors(released), "active reservation has no window"));
  kv->seed(windowKey, window); kv->seed(reservationKey, reservation); auto legacy = window; legacy.asObject().erase("reserved_requests"); kv->seed(windowKey, legacy);
  AWAIT(compatible, migration::validateKvMigrationTarget(kv)); EXPECT(!rt::includes(errors(compatible), "kernel quota V2"));
  kv->seed(windowKey, window); kv->erase(reservationKey); AWAIT(missing, migration::validateKvMigrationTarget(kv)); EXPECT(rt::includes(errors(missing), "kernel quota V2 reserved aggregate is inconsistent"));
}

ASYNC_TEST("prod KV migration imports only modern durable rows by default") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); const auto opts = options();
  const rt::KvKey legacy{"key", "config", "1"}; const rt::KvKey session{"uos_ai", "auth", "sessions", "session-id"};
  std::vector<std::string> lines{entryLine(DEFAULT_MODEL_KEY, "gpt-5.4"), entryLine(CATALOG_KEY, row(R"({"models":[{"slug":"gpt-5.4"}]})")),
    entryLine(legacy, row(R"({"apiKey":"legacy"})")), entryLine(session, row(R"({"user_id":"user-id"})"))};
  AWAIT(result, migration::importKvMigrationLines(kv, lines, opts)); EXPECT_EQ(result.total, 4); EXPECT_EQ(result.imported, 1); EXPECT_EQ(result.skipped, 3); EXPECT_EQ(result.errors, 0);
  EXPECT_EQ(kv->value(DEFAULT_MODEL_KEY).asString(), "gpt-5.4"); EXPECT(!kv->has(CATALOG_KEY)); EXPECT(!kv->has(legacy)); EXPECT(!kv->has(session));
}

ASYNC_TEST("local KV migration keeps legacy and Codex bootstrap rows for replay") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); const base::KvMigrationImportOptions opts{{"local", false, true}, true, false}; const rt::KvKey healthKey{"key", "health", "1"};
  std::vector<std::string> lines{entryLine(CATALOG_KEY, row(R"({"models":[{"slug":"gpt-5.4"}]})")), entryLine(healthKey, row(R"({"status":"ok"})"))};
  AWAIT(result, migration::importKvMigrationLines(kv, lines, opts)); EXPECT_EQ(result.imported, 2); EXPECT(kv->has(CATALOG_KEY)); EXPECT(kv->has(healthKey));
}

ASYNC_TEST("KV migration preserves embeddings idempotency ledgers and response chunks") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); const auto opts = options();
  const rt::KvKey ledger{"embeddings", "idempotency", "v1", "principal-hash", "idempotency-key-hash"};
  const rt::KvKey response{"embeddings", "idempotency", "v1", "response", "principal-hash", "idempotency-key-hash", "response-generation", 0};
  const rt::KvKey job{"embeddings", "jobs", "v2", "token-hash", "profile", "job-id"}; const auto value = row(R"({"v":1,"state":"succeeded","response_chunk_count":1})");
  std::vector<std::string> lines{entryLine(ledger, value), entryLine(response, "{\"data\":[]}"), entryLine(job, row(R"({"status":"succeeded"})"))};
  AWAIT(result, migration::importKvMigrationLines(kv, lines, opts)); EXPECT_EQ(result.imported, 2); EXPECT_EQ(result.skipped, 1);
  EXPECT_EQ(*result.groups.get("embeddings_idempotency"), 1); EXPECT_EQ(*result.groups.get("embeddings_idempotency_responses"), 1); EXPECT_EQ(*result.groups.get("embeddings_jobs"), 1);
  EXPECT_EQ(kv->value(ledger), value); EXPECT_EQ(kv->value(response).asString(), "{\"data\":[]}"); EXPECT(!kv->has(job));
}

ASYNC_TEST("KV migration imports only the option-aware v2 embedding cache when requested") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); const base::KvMigrationImportOptions opts{{"prod", true, false}, true, false};
  const rt::KvKey v1{"embeddings", "v1", "legacy-model", "text-hash"}; const rt::KvKey v2{"embeddings", "v2", "voyage-4-large|document|1024|float|float|false", "text-hash"};
  std::vector<std::string> lines{entryLine(v1, row(R"({"embedding":[1]})")), entryLine(v2, row(R"({"embedding":[2]})"))};
  AWAIT(result, migration::importKvMigrationLines(kv, lines, opts)); EXPECT_EQ(result.imported, 1); EXPECT_EQ(result.skipped, 1); EXPECT(!kv->has(v1)); EXPECT_EQ(kv->value(v2), row(R"({"embedding":[2]})"));
}

ASYNC_TEST("KV migration dry-run reports destination collisions like writes") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); const auto opts = options(false, true); kv->seed(DEFAULT_MODEL_KEY, "existing-model");
  std::vector<std::string> lines{entryLine(DEFAULT_MODEL_KEY, "new-model")}; AWAIT(result, migration::importKvMigrationLines(kv, lines, opts));
  EXPECT_EQ(result.imported, 0); EXPECT_EQ(result.skipped, 1); EXPECT_EQ(kv->value(DEFAULT_MODEL_KEY).asString(), "existing-model");
}

ASYNC_TEST("KV migration imports a missing row with an atomic destination check") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); const auto opts = options(false);
  std::vector<std::string> lines{entryLine(DEFAULT_MODEL_KEY, "gpt-5.6"), entryLine(DEFAULT_MODEL_KEY, "gpt-5.7")}; AWAIT(result, migration::importKvMigrationLines(kv, lines, opts));
  EXPECT_EQ(result.imported, 1); EXPECT_EQ(result.skipped, 1); EXPECT_EQ(kv->value(DEFAULT_MODEL_KEY).asString(), "gpt-5.6");
}

ASYNC_TEST("KV migration validation rejects unusable codex model snapshots for configured defaults") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); kv->seed(DEFAULT_MODEL_KEY, "gpt-5.5"); kv->seed(CATALOG_KEY, row(R"({"models":[]})"));
  AWAIT(result, migration::validateKvMigrationTarget(kv)); EXPECT(rt::includes(errors(result), "codex model snapshot is empty or malformed"));
}

ASYNC_TEST("KV migration imports and validates the complete paid fallback V3 state") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); const auto opts = options();
  const std::string id = "paid-fallback-v3-migration"; const std::string requestId = "request-v3-migration"; const std::string pendingId = "pending-request-v3-migration";
  const double resetAt = NOW + MINUTE;
  const auto request = patch(row(R"({"v":3,"policy_version":"policy-v3","route":"responses","path":"/v1/responses","model":"gpt-5.5","stream":true,"reasoning":"high","reserved_microcredits":50000,"quota_per_credit":500000,"provider_request_id":"provider-v3-migration","provider_quota":12.5,"input_tokens":10,"output_tokens":5,"dispatch_state":"dispatched","terminal_state":"completed","spend_microcredits":25,"billing_state":"settled","reconciliation_attempts":1})"),
    {{"key_id", id}, {"request_id", requestId}, {"window_reset_at_ms", resetAt}, {"last_reconciliation_at_ms", NOW}, {"dispatched_at_ms", NOW - 500},
      {"terminal_at_ms", NOW - 100}, {"settled_at_ms", NOW}, {"created_at_ms", NOW - 1000}, {"updated_at_ms", NOW}});
  const auto pending = patch(request, {{"request_id", pendingId}, {"provider_request_id", "provider-v3-pending"}, {"provider_quota", nullptr},
    {"input_tokens", nullptr}, {"output_tokens", nullptr}, {"terminal_state", "pending"}, {"spend_microcredits", nullptr}, {"billing_state", "pending"},
    {"reconciliation_attempts", 0}, {"last_reconciliation_at_ms", nullptr}, {"terminal_at_ms", nullptr}, {"settled_at_ms", nullptr}, {"updated_at_ms", NOW - 100}});
  const rt::KvValue window(json::object{{"v", 3}, {"key_id", id}, {"policy_version", "policy-v3"}, {"window_reset_at_ms", resetAt},
    {"limit_microcredits", 2'000'000}, {"settled_microcredits", 25}, {"reserved_microcredits", 50'000}, {"pending_count", 1}, {"updated_at_ms", NOW}});
  const rt::KvKey windowKey{"uos_ai", "paid_fallback", "v3", "window", id, resetAt};
  const rt::KvKey requestKey{"uos_ai", "paid_fallback", "v3", "request", id, requestId};
  const rt::KvKey pendingRequestKey{"uos_ai", "paid_fallback", "v3", "request", id, pendingId};
  const rt::KvKey pendingKey{"uos_ai", "paid_fallback", "v3", "pending", id, pendingId};
  const rt::KvKey leaseKey{"uos_ai", "paid_fallback", "v3", "reconciliation_lease", id};
  const rt::KvKey deletionKey{"uos_ai", "paid_fallback", "v3", "deletion_guard", "deleted-key-v3-migration"};
  const rt::KvValue marker(json::object{{"created_at_ms", NOW - 1000}, {"next_reconciliation_at_ms", NOW + 5000}});
  const rt::KvValue lease(json::object{{"token", "lease-v3-migration"}, {"expires_at_ms", NOW + MINUTE}});
  const rt::KvValue deletion(json::object{{"created_at_ms", NOW}});
  std::vector<std::string> lines{entryLine(windowKey, window), entryLine(requestKey, request), entryLine(pendingRequestKey, pending),
    entryLine(pendingKey, marker), entryLine(leaseKey, lease), entryLine(deletionKey, deletion)};
  AWAIT(imported, migration::importKvMigrationLines(kv, lines, opts)); EXPECT_EQ(imported.imported, 6);
  const rt::OrderedMap<std::string, double> expectedGroups{{"paid_fallback_v3_deletion_guards", 1}, {"paid_fallback_v3_pending", 1},
    {"paid_fallback_v3_reconciliation_leases", 1}, {"paid_fallback_v3_requests", 2}, {"paid_fallback_v3_windows", 1}};
  for (const auto& [group, count] : expectedGroups) EXPECT_EQ(*imported.groups.get(group), count);
  EXPECT_EQ(imported.groups.size(), expectedGroups.size()); EXPECT_EQ(kv->value(requestKey), request);
  kv->clearData(); seedApiKey(*kv, id, HASH, {{"paid_fallback_reservation_request_id", nullptr}});
  AWAIT_DONE(migrate, migration::migrateKvReadIncidentV2(kv));
  kv->seed(windowKey, window); kv->seed(requestKey, request); kv->seed(pendingRequestKey, pending); kv->seed(pendingKey, marker); kv->seed(leaseKey, lease); kv->seed(deletionKey, deletion);
  AWAIT(valid, migration::validateKvMigrationTarget(kv)); EXPECT(valid.errors.empty()); EXPECT_EQ(valid.counts.paid_fallback_v3_windows, 1);
  EXPECT_EQ(valid.counts.paid_fallback_v3_requests, 2); EXPECT_EQ(valid.counts.paid_fallback_v3_pending, 1); EXPECT_EQ(valid.counts.paid_fallback_v3_reconciliation_leases, 1); EXPECT_EQ(valid.counts.paid_fallback_v3_deletion_guards, 1);
  kv->erase(windowKey); kv->erase(pendingRequestKey); kv->erase(pendingKey); kv->seed(requestKey, patch(request, {{"policy_version", "v3:60000"}, {"reserved_microcredits", 0}}));
  AWAIT(unlimited, migration::validateKvMigrationTarget(kv)); EXPECT(unlimited.errors.empty());
  AWAIT_DONE(repeat, migration::migrateKvReadIncidentV2(kv)); EXPECT(!kv->has(windowKey));
  kv->seed(requestKey, patch(request, {{"billing_state", "pending"}})); AWAIT(missing, migration::validateKvMigrationTarget(kv)); EXPECT(rt::includes(errors(missing), "request is missing its pending marker"));
}

ASYNC_TEST("KV incident migration rejects an active reservation without a pending ledger before mutation") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); const auto pair = seedApiKey(*kv); const auto before = kv->entries;
  AWAIT(validation, migration::validateKvMigrationTarget(kv)); EXPECT(rt::includes(errors(validation), "reservation has no pending ledger record: " + pair.id));
  auto task = migration::migrateKvReadIncidentV2(kv); std::string pattern = "Paid fallback ledger validation failed: paid fallback reservation has no pending ledger record";
  AWAIT_DONE(failure, rejectsMatching(std::move(task), pattern));
  for (const auto& [key, entry] : before) EXPECT_EQ(kv->entries.get(key)->value, entry.value);
  EXPECT_EQ(kv->entries.size(), before.size()); EXPECT(!kv->has(uos::runtime_config::RUNTIME_CONFIG_V2_KEY)); EXPECT(!kv->has(migration::KV_READ_INCIDENT_V2_MIGRATION_KEY));
}

ASYNC_TEST("KV incident migration preserves a valid unlimited reservation and requires its ledger to stay pending") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); const auto pair = seedApiKey(*kv); const std::string requestId = "pending-unlimited-request";
  const auto legacy = ledgerKey(pair.id, requestId, true); const auto ledger = ledgerKey(pair.id, requestId); const auto pending = pendingLedger(pair.id, requestId);
  kv->seed(legacy, pending); AWAIT(migrated, migration::migrateKvReadIncidentV2(kv)); EXPECT_EQ(migrated.handoff_phase, "no_bounded_keys"); EXPECT_EQ(migrated.paid_fallback_records, 1); EXPECT_EQ(kv->value(ledger), pending);
  AWAIT(valid, migration::validateKvMigrationTarget(kv)); EXPECT(valid.errors.empty());
  kv->seed(ledger, patch(pending, {{"billing_status", "reconciled"}})); AWAIT(terminal, migration::validateKvMigrationTarget(kv)); EXPECT(rt::includes(errors(terminal), "reservation has no pending ledger record"));
}

ASYNC_TEST("KV incident migration permits matching terminal ledgers but rejects destination identity collisions") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); const auto pair = seedApiKey(*kv, "unlimited-paid-fallback-key", HASH, {{"paid_fallback_reservation_request_id", nullptr}});
  const std::string requestId = "legacy-pending-request"; const auto legacy = ledgerKey(pair.id, requestId, true); const auto ledger = ledgerKey(pair.id, requestId); const auto pending = pendingLedger(pair.id, requestId);
  kv->seed(legacy, pending); kv->seed(ledger, patch(pending, {{"id", "different-request"}, {"billing_status", "reconciled"}})); const auto before = kv->entries;
  auto task = migration::migrateKvReadIncidentV2(kv); std::string pattern = "paid fallback ledger destination conflicts with legacy record"; AWAIT_DONE(failure, rejectsMatching(std::move(task), pattern));
  for (const auto& [key, entry] : before) EXPECT_EQ(kv->entries.get(key)->value, entry.value);
  EXPECT_EQ(kv->entries.size(), before.size()); EXPECT(!kv->has(uos::runtime_config::RUNTIME_CONFIG_V2_KEY)); EXPECT(!kv->has(migration::KV_READ_INCIDENT_V2_MIGRATION_KEY));
  const auto reconciled = patch(pending, {{"billing_status", "reconciled"}}); kv->seed(ledger, reconciled);
  AWAIT(migrated, migration::migrateKvReadIncidentV2(kv)); EXPECT_EQ(migrated.paid_fallback_records, 1); EXPECT_EQ(kv->value(ledger), reconciled);
}

ASYNC_TEST("KV incident migration projects settled spend and unresolved exposure exactly once") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); const std::string id = "bounded-paid-fallback-key"; const std::string pendingId = "unresolved-paid-fallback-request";
  const std::string settledId = "settled-paid-fallback-request"; const double resetAt = NOW + MINUTE;
  seedApiKey(*kv, id, std::string(43, 'e'), {{"paid_fallback_limit_microcredits", 2'000'000}, {"paid_fallback_spent_microcredits", 0},
    {"paid_fallback_reserved_microcredits", 100'000}, {"paid_fallback_reservation_request_id", pendingId}}, {{"paid_fallback_max_exposure_microcredits", json::object{{"gpt-5.6-sol", 100'000}}}});
  const auto settled = patch(row(R"({"provider":"metered","route":"responses","path":"/v1/responses","method":"POST","status_code":200,"stream":false,"model":"gpt-5.6-sol","reasoning":null,"fallback_reason":"primary_429","provider_request_id":"provider-settled","input_tokens":10,"output_tokens":5,"provider_quota":25,"quota_per_credit":500000,"spend_microcredits":25000,"billing_status":"reconciled"})"),
    {{"id", settledId}, {"key_id", id}, {"created_at_ms", NOW - 2000}, {"completed_at_ms", NOW - 1000}, {"paid_fallback_window_reset_at_ms", resetAt}});
  const auto unresolved = patch(settled, {{"id", pendingId}, {"provider_request_id", nullptr}, {"status_code", 502}, {"completed_at_ms", nullptr}, {"input_tokens", nullptr},
    {"output_tokens", nullptr}, {"provider_quota", nullptr}, {"spend_microcredits", nullptr}, {"billing_status", "unresolved"}});
  kv->seed(ledgerKey(id, settledId, true, NOW - 2000), settled); kv->seed(ledgerKey(id, pendingId, true, NOW - 2000), unresolved);
  AWAIT(first, migration::migrateKvReadIncidentV2(kv)); EXPECT_EQ(first.paid_fallback_records, 1);
  const rt::KvKey windowKey{"uos_ai", "paid_fallback", "v3", "window", id, resetAt};
  const rt::KvKey requestKey{"uos_ai", "paid_fallback", "v3", "request", id, pendingId}; const rt::KvKey pendingKey{"uos_ai", "paid_fallback", "v3", "pending", id, pendingId};
  const auto projected = kv->value(windowKey); EXPECT_EQ(projected["settled_microcredits"].asNumber(), 25'000); EXPECT_EQ(projected["reserved_microcredits"].asNumber(), 100'000); EXPECT_EQ(projected["pending_count"].asNumber(), 1);
  EXPECT_EQ(kv->value(requestKey)["billing_state"].asString(), "unresolved"); EXPECT_EQ(kv->value(requestKey)["reserved_microcredits"].asNumber(), 100'000); EXPECT(kv->has(pendingKey));
  EXPECT(kv->value(paid::paidFallbackReconciliationGateV3Key())["next_due_at_ms"].asNumber() <= NOW);
  AWAIT(second, migration::migrateKvReadIncidentV2(kv)); EXPECT_EQ(second.paid_fallback_records, 1); EXPECT_EQ(kv->value(windowKey), projected);
  kv->erase(windowKey); kv->erase(pendingKey); kv->seed(paid::paidFallbackReconciliationGateV3Key(), rt::KvValue(json::object{{"next_due_at_ms", NOW + MINUTE}}));
  AWAIT_DONE(repair, migration::migrateKvReadIncidentV2(kv)); const auto repaired = kv->value(windowKey);
  EXPECT_EQ(repaired["settled_microcredits"].asNumber(), 25'000); EXPECT_EQ(repaired["reserved_microcredits"].asNumber(), 100'000); EXPECT_EQ(repaired["pending_count"].asNumber(), 1);
  EXPECT(kv->has(pendingKey)); EXPECT(kv->value(paid::paidFallbackReconciliationGateV3Key())["next_due_at_ms"].asNumber() <= NOW);
  AWAIT(valid, migration::validateKvMigrationTarget(kv)); EXPECT(valid.errors.empty());
  kv->seed(windowKey, patch(repaired, {{"settled_microcredits", 25'001}})); AWAIT(mismatch, migration::validateKvMigrationTarget(kv)); EXPECT(rt::includes(errors(mismatch), "window aggregate is inconsistent"));
}

ASYNC_TEST("KV incident migration resumes concurrent phase one and retains postdeploy counter versions") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); const std::string id = "bounded-key"; const std::string hash(43, 'a');
  const auto pair = seedApiKey(*kv, id, hash, {{"usage_limit_requests", 10}, {"usage_requests", 4}, {"paid_fallback_limit_microcredits", 1'000'000},
    {"paid_fallback_spent_microcredits", 100'000}, {"paid_fallback_reserved_microcredits", 0}, {"paid_fallback_reservation_request_id", nullptr}});
  const rt::KvKey staleKey{"uos_ai", "api_key_usage", "v2", id, "10:60000:" + rt::numberToString(NOW + MINUTE), NOW};
  const rt::KvKey expiredKey{"uos_ai", "api_key_usage", "v2", id, "10:60000:" + rt::numberToString(NOW), NOW - MINUTE};
  const rt::KvKey currentKey{"uos_ai", "api_key_usage", "v2", id, "60000", NOW};
  const rt::KvKey baselineKey{migration::KV_READ_INCIDENT_V2_MIGRATION_KEY, "api_key_usage_baseline", id, "v3:60000", NOW};
  const rt::KvKey windowKey{"uos_ai", "api_key_usage", "v3", "window", id, "v3:60000", NOW};
  kv->seed(staleKey, rt::KvU64(7)); kv->seed(expiredKey, rt::KvU64(99)); kv->seed(currentKey, rt::KvU64(8));
  kv->seed(migration::KV_READ_INCIDENT_V2_MIGRATION_KEY, rt::KvValue(json::object{{"version", 2}, {"completed_at_ms", NOW - 1}}));
  kv->seed(ledgerKey(id, "pending", true), pendingLedger(id, "pending")); kv->seed(ledgerKey(id, "done", true), patch(pendingLedger(id, "done"), {{"billing_status", "reconciled"}}));
  AWAIT(result, migration::migrateKvReadIncidentV2(kv));
  const migration::KvReadIncidentV2MigrationResult expected{.api_keys = 1, .bounded_counters = 1, .handoff_phase = "predeploy_seed", .bounded_baselines_created = 1,
    .bounded_baselines_reconciled = 0, .legacy_usage_delta_applied = 8, .kernel_repo_records = 0, .kernel_org_records = 0, .paid_fallback_records = 1, .runtime_config_written = true};
  EXPECT_EQ(result, expected); EXPECT(kv->has(uos::runtime_config::RUNTIME_CONFIG_V2_KEY)); EXPECT(kv->has(staleKey)); EXPECT(kv->has(expiredKey)); EXPECT_EQ(kv->value(currentKey).asU64().value, std::uint64_t(8));
  EXPECT_EQ(kv->value(windowKey)["committed_requests"].asNumber(), 8); const auto dedicated = ledgerKey(id, "pending"); EXPECT(kv->has(dedicated)); EXPECT(!kv->has(ledgerKey(id, "done")));
  AWAIT(validation, migration::validateKvMigrationTarget(kv)); EXPECT(validation.errors.empty()); EXPECT_EQ(validation.counts.api_key_bounded_counter_baselines_v2, 1); EXPECT_EQ(validation.counts.api_key_bounded_counter_reconciled_baselines_v2, 0);
  kv->erase(migration::KV_READ_INCIDENT_V2_MIGRATION_KEY); patchPair(*kv, pair, {{"usage_requests", 5}}); kv->seed(currentKey, rt::KvU64(9));
  AWAIT(phaseOne, migration::migrateKvReadIncidentV2(kv)); EXPECT_EQ(phaseOne.handoff_phase, "predeploy_seed"); EXPECT_EQ(phaseOne.bounded_baselines_created, 0); EXPECT_EQ(phaseOne.bounded_baselines_reconciled, 0); EXPECT_EQ(phaseOne.legacy_usage_delta_applied, 1);
  EXPECT_EQ(kv->value(currentKey).asU64().value, std::uint64_t(9)); const auto caught = kv->value(baselineKey); EXPECT_EQ(caught["last_legacy_usage_requests"].asNumber(), 9); EXPECT(caught["reconciled_at_ms"].isNull()); EXPECT_EQ(caught["reconciliation_runs"].asNumber(), 0);
  const auto reconciled = patch(kv->value(dedicated), {{"billing_status", "reconciled"}, {"spend_microcredits", 123}}); kv->seed(dedicated, reconciled); patchPair(*kv, pair, {{"usage_requests", 7}}); kv->seed(currentKey, rt::KvU64(11));
  const rt::KvKey nextCounter{"uos_ai", "api_key_usage", "v2", id, "60000", NOW + MINUTE};
  const rt::KvKey oldBaseline{migration::KV_READ_INCIDENT_V2_MIGRATION_KEY, "api_key_usage_baseline", id, "legacy-policy", NOW - MINUTE};
  kv->seed(nextCounter, rt::KvU64(2)); kv->seed(oldBaseline, rt::KvValue(json::object{{"version", 1}, {"key_id", id}, {"policy_version", "legacy-policy"},
    {"window_start_ms", NOW - MINUTE}, {"last_legacy_usage_requests", 1}, {"seeded_at_ms", NOW}, {"reconciled_at_ms", NOW}, {"reconciliation_runs", 1}}));
  AWAIT(rerun, migration::migrateKvReadIncidentV2(kv)); EXPECT_EQ(rerun.handoff_phase, "postdeploy_reconcile"); EXPECT_EQ(rerun.bounded_baselines_created, 0); EXPECT_EQ(rerun.bounded_baselines_reconciled, 1); EXPECT_EQ(rerun.legacy_usage_delta_applied, 2); EXPECT_EQ(rerun.paid_fallback_records, 1);
  EXPECT_EQ(kv->value(dedicated), reconciled); EXPECT_EQ(kv->value(currentKey).asU64().value, std::uint64_t(11)); EXPECT_EQ(kv->value(windowKey)["committed_requests"].asNumber(), 11); EXPECT_EQ(kv->value(nextCounter).asU64().value, std::uint64_t(2)); EXPECT(kv->has(oldBaseline));
  AWAIT(repeated, migration::migrateKvReadIncidentV2(kv)); EXPECT_EQ(repeated.handoff_phase, "postdeploy_reconcile"); EXPECT_EQ(repeated.legacy_usage_delta_applied, 0); EXPECT_EQ(kv->value(currentKey).asU64().value, std::uint64_t(11));
  AWAIT(reconciledValidation, migration::validateKvMigrationTarget(kv)); EXPECT(reconciledValidation.errors.empty()); EXPECT_EQ(reconciledValidation.counts.api_key_bounded_counters_v2, 4); EXPECT_EQ(reconciledValidation.counts.api_key_bounded_counter_baselines_v2, 2); EXPECT_EQ(reconciledValidation.counts.api_key_bounded_counter_reconciled_baselines_v2, 2); EXPECT_EQ(reconciledValidation.counts.api_key_usage_v3_windows, 1);
  const auto idRecord = kv->value(pair.idKey); kv->erase(pair.idKey); const rt::KvKey wrongKey{"ubq_ai", "api_keys", "id", "wrong-key-suffix"}; kv->seed(wrongKey, idRecord);
  AWAIT(wrongId, migration::validateKvMigrationTarget(kv)); EXPECT(rt::includes(errors(wrongId), "id key suffix does not match record id"));
  auto wrongTask = migration::migrateKvReadIncidentV2(kv); std::string wrongPattern = "id key suffix does not match record id"; AWAIT_DONE(wrongFailure, rejectsMatching(std::move(wrongTask), wrongPattern)); kv->erase(wrongKey); kv->seed(pair.idKey, idRecord);
  const auto hashRecord = kv->value(pair.hashKey); const rt::KvKey orphanKey{"ubq_ai", "api_keys", "hash", std::string(43, 'c')}; kv->seed(orphanKey, patch(hashRecord, {{"id", "orphan-key"}}));
  AWAIT(orphan, migration::validateKvMigrationTarget(kv)); EXPECT(rt::includes(errors(orphan), "hash policy is orphaned")); kv->erase(orphanKey);
  kv->seed(pair.hashKey, patch(hashRecord, {{"usage_limit_requests", 11}})); AWAIT(drift, migration::validateKvMigrationTarget(kv)); EXPECT(rt::includes(errors(drift), "hash policy is missing or inconsistent"));
}

ASYNC_TEST("KV incident migration starts a fresh counter after an expired legacy window and rejects malformed policy") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); const std::string id = "expired-bounded-key"; const double resetAt = NOW - 1;
  const auto pair = seedApiKey(*kv, id, std::string(43, 'b'), {{"usage_limit_requests", 10}, {"usage_requests", 99}, {"usage_reset_at_ms", resetAt},
    {"paid_fallback_enabled", false}, {"paid_fallback_limit_microcredits", 0}, {"paid_fallback_spent_microcredits", 0}, {"paid_fallback_reserved_microcredits", 0}, {"paid_fallback_reservation_request_id", nullptr}},
    {{"paid_fallback_model_ids", json::array{}}, {"paid_fallback_quota_per_credit", 0}, {"paid_fallback_pricing_checked_at_ms", nullptr}});
  const rt::KvKey expiredKey{"uos_ai", "api_key_usage", "v2", id, "10:60000:" + rt::numberToString(resetAt), resetAt - MINUTE}; kv->seed(expiredKey, rt::KvU64(99));
  AWAIT_DONE(migrate, migration::migrateKvReadIncidentV2(kv)); EXPECT(kv->has(expiredKey));
  int windows = 0;
  for (const auto& [key, entry] : kv->entries) {
    if (!rt::includes(key, "\"api_key_usage\",\"v3\",\"window\",\"" + id + "\"")) continue;
    ++windows; EXPECT_EQ(entry.value["committed_requests"].asNumber(), 0);
  }
  EXPECT_EQ(windows, 1); AWAIT(expiredValidation, migration::validateKvMigrationTarget(kv)); EXPECT(expiredValidation.errors.empty()); EXPECT_EQ(expiredValidation.counts.api_key_bounded_counter_baselines_v2, 1);
  patchPair(*kv, pair, {{"usage_requests", 2}, {"usage_reset_at_ms", NOW + MINUTE}});
  const rt::KvKey rolledKey{"uos_ai", "api_key_usage", "v2", id, "60000", NOW}; kv->seed(rolledKey, rt::KvU64(3));
  AWAIT(stale, migration::validateKvMigrationTarget(kv)); EXPECT(rt::includes(errors(stale), "baseline is missing, stale, or invalid"));
  AWAIT(rolled, migration::migrateKvReadIncidentV2(kv)); EXPECT_EQ(rolled.handoff_phase, "postdeploy_reconcile"); EXPECT_EQ(rolled.bounded_baselines_created, 1); EXPECT_EQ(rolled.bounded_baselines_reconciled, 1); EXPECT_EQ(rolled.legacy_usage_delta_applied, 3); EXPECT_EQ(kv->value(rolledKey).asU64().value, std::uint64_t(3));
  AWAIT(rolledValidation, migration::validateKvMigrationTarget(kv)); EXPECT(rolledValidation.errors.empty()); AWAIT(repeated, migration::migrateKvReadIncidentV2(kv)); EXPECT_EQ(repeated.legacy_usage_delta_applied, 0); EXPECT_EQ(kv->value(rolledKey).asU64().value, std::uint64_t(3));
  patchPair(*kv, pair, {{"usage_limit_requests", "bad"}}); AWAIT(malformed, migration::validateKvMigrationTarget(kv)); EXPECT(rt::Regex("invalid core fields|invalid v2 policy").test(errors(malformed)));
  auto task = migration::migrateKvReadIncidentV2(kv); std::string pattern = "API key policy validation failed"; AWAIT_DONE(failure, rejectsMatching(std::move(task), pattern));
}

ASYNC_TEST("KV incident validation retains historical revoked V3 windows") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); const std::string id = "revoked-bounded-key"; const double resetAt = NOW - policy::API_KEY_USAGE_V3_RETENTION_MS - MINUTE;
  seedApiKey(*kv, id, std::string(43, 'c'), {{"revoked_at_ms", NOW - 10'000}, {"usage_limit_requests", 20}, {"usage_requests", 4}, {"usage_reset_at_ms", resetAt},
    {"paid_fallback_enabled", false}, {"paid_fallback_limit_microcredits", 0}, {"paid_fallback_spent_microcredits", 0}, {"paid_fallback_reserved_microcredits", 0}, {"paid_fallback_reservation_request_id", nullptr}},
    {{"paid_fallback_model_ids", json::array{}}, {"paid_fallback_quota_per_credit", 0}, {"paid_fallback_pricing_checked_at_ms", nullptr}});
  seedCatalog(*kv, "gpt-5.6-sol-max"); kv->seed(DEFAULT_REASONING_KEY, "none"); AWAIT_DONE(migrate, migration::migrateKvReadIncidentV2(kv));
  bool checked = false;
  for (const auto& [key, expiry] : kv->expireIn) {
    if (!rt::includes(key, "\"api_key_usage\",\"v3\",\"window\"")) continue;
    checked = true; EXPECT_EQ(expiry, policy::API_KEY_USAGE_V3_RETENTION_MS);
  }
  EXPECT(checked); AWAIT(validation, migration::validateKvMigrationTarget(kv)); EXPECT(validation.errors.empty());
}

ASYNC_TEST("KV migration preserves Codex reset usage fences and agrees with validation counts") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>(); const auto opts = options(); const std::string disabledHash = "reset-disabled-account-hash"; const std::string enabledHash = "reset-enabled-account-hash";
  const auto disabledKey = reset::codexResetUsageKey(disabledHash); const auto enabledKey = reset::codexResetUsageKey(enabledHash);
  const rt::KvKey promptKey{"uos_ai", "codex_instructions", "prompt-a"}; const rt::KvKey transientKey{"uos_ai", "auth", "sessions", "session-a"};
  seedApiKey(*kv, "unlimited-paid-fallback-key", HASH, {{"paid_fallback_reservation_request_id", nullptr}}); AWAIT_DONE(migrate, migration::migrateKvReadIncidentV2(kv));
  std::vector<std::string> lines{entryLine(disabledKey, row(R"({"enabled":false})")), entryLine(enabledKey, row(R"({"enabled":true})")),
    entryLine(promptKey, row(R"({"prompt":"keep"})")), entryLine(transientKey, row(R"({"token":"skip"})"))};
  AWAIT(result, migration::importKvMigrationLines(kv, lines, opts)); EXPECT_EQ(result.total, 4); EXPECT_EQ(result.imported, 3); EXPECT_EQ(result.skipped, 1);
  EXPECT_EQ(*result.groups.get("codex_reset_usage"), 2); EXPECT_EQ(*result.groups.get("codex_prompts"), 1); EXPECT_EQ(*result.groups.get("passkey_sessions"), 1);
  EXPECT_EQ(kv->value(disabledKey), row(R"({"enabled":false})")); EXPECT_EQ(kv->value(enabledKey), row(R"({"enabled":true})")); EXPECT_EQ(kv->value(promptKey), row(R"({"prompt":"keep"})")); EXPECT(!kv->has(transientKey));
  AWAIT(disabled, reset::readCodexResetUsage(kv, disabledHash)); EXPECT(!disabled.allowed); AWAIT(enabled, reset::readCodexResetUsage(kv, enabledHash)); EXPECT(enabled.allowed);
  AWAIT(validation, migration::validateKvMigrationTarget(kv)); EXPECT(validation.errors.empty()); EXPECT_EQ(validation.counts.codex_reset_usage, 2);
}

ASYNC_TEST("KV migration validation rejects malformed Codex reset usage rows") {
  CredentialScope scope; auto kv = std::make_shared<MapKv>();
  kv->seed({"uos_ai", "codex_reset_usage", "bad_key"}, row(R"({"enabled":false})"));
  kv->seed({"uos_ai", "codex_reset_usage", "account", "v1", "bad_value"}, row(R"({"enabled":"not_a_boolean"})"));
  kv->seed({"uos_ai", "codex_reset_usage", "account", "v1", "valid_value"}, row(R"({"enabled":true})"));
  AWAIT(result, migration::validateKvMigrationTarget(kv)); int count = 0; std::string resetErrors;
  for (const auto& error : result.errors) if (rt::startsWith(error, "codex reset usage")) { ++count; resetErrors += error + "\n"; }
  EXPECT_EQ(result.counts.codex_reset_usage, 3); EXPECT_EQ(count, 2); EXPECT(rt::includes(resetErrors, "codex reset usage key is malformed")); EXPECT(rt::includes(resetErrors, "codex reset usage record is malformed"));
}

namespace {
  const json::value& oracleVectors() {
    static const auto value = rt::jsonParse(R"ORACLE([{"name":"inherited Error model name","mode":"","rows":[{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"codex_models"}],"value":{"type":"object","value":{"models":{"type":"Array","value":[{"type":"Error","value":{"message":"synthetic catalog entry","stack":"Error: synthetic catalog entry"}}]},"source":{"type":"string","value":"chatgpt_codex"},"updated_at_ms":{"type":"number","value":1800000000000}}}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"model"}],"value":{"type":"string","value":"Error"}}],"expected":{"value":{"api_keys":0,"bounded_counters":0,"handoff_phase":"no_bounded_keys","bounded_baselines_created":0,"bounded_baselines_reconciled":0,"legacy_usage_delta_applied":0,"kernel_repo_records":0,"kernel_org_records":0,"paid_fallback_records":0,"runtime_config_written":true}},"digest":"844f47dd3aa372f55cae21a37885b64bc21e30a66e095ec74c5b90af69e70764"},{"name":"ledger CAS missing destination","mode":"ledger-missing","rows":[{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"codex_models"}],"value":{"type":"object","value":{"models":{"type":"Array","value":[{"type":"object","value":{"slug":{"type":"string","value":"gpt-5.5"},"supported_reasoning_levels":{"type":"Array","value":[{"type":"string","value":"none"},{"type":"string","value":"medium"}]}}}]},"source":{"type":"string","value":"chatgpt_codex"},"updated_at_ms":{"type":"number","value":1800000000000}}}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"model"}],"value":{"type":"string","value":"gpt-5.5"}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"reasoning_effort"}],"value":{"type":"string","value":"medium"}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"id"},{"type":"string","value":"key-alpha"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"number","value":-1},"usage_requests":{"type":"number","value":0},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null},"name":{"type":"string","value":"Synthetic"},"prefix":{"type":"string","value":"u_1234567890"},"hash":{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"},"created_at_ms":{"type":"number","value":1799999940000},"paid_fallback_model_ids":{"type":"Array","value":[]},"paid_fallback_quota_per_credit":{"type":"number","value":0},"paid_fallback_pricing_checked_at_ms":{"type":"null","value":null}}}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"hash"},{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"number","value":-1},"usage_requests":{"type":"number","value":0},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null}}}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"request_log"},{"type":"string","value":"key-alpha"},{"type":"number","value":1800000000000},{"type":"string","value":"pending"}],"value":{"type":"object","value":{"id":{"type":"string","value":"pending"},"key_id":{"type":"string","value":"key-alpha"},"provider":{"type":"string","value":"metered"},"billing_status":{"type":"string","value":"pending"},"created_at_ms":{"type":"number","value":1800000000000}}}}],"expected":{"error":{"name":"Error","message":"Paid fallback ledger changed during migration: pending"}},"digest":"c7f866ff8c6bfd0e82cf287fc508db7bec7a94f5e7b457b4a4c984f545ac52aa"},{"name":"ledger CAS concurrent winner","mode":"ledger-winner","rows":[{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"codex_models"}],"value":{"type":"object","value":{"models":{"type":"Array","value":[{"type":"object","value":{"slug":{"type":"string","value":"gpt-5.5"},"supported_reasoning_levels":{"type":"Array","value":[{"type":"string","value":"none"},{"type":"string","value":"medium"}]}}}]},"source":{"type":"string","value":"chatgpt_codex"},"updated_at_ms":{"type":"number","value":1800000000000}}}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"model"}],"value":{"type":"string","value":"gpt-5.5"}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"reasoning_effort"}],"value":{"type":"string","value":"medium"}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"id"},{"type":"string","value":"key-alpha"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"number","value":-1},"usage_requests":{"type":"number","value":0},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null},"name":{"type":"string","value":"Synthetic"},"prefix":{"type":"string","value":"u_1234567890"},"hash":{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"},"created_at_ms":{"type":"number","value":1799999940000},"paid_fallback_model_ids":{"type":"Array","value":[]},"paid_fallback_quota_per_credit":{"type":"number","value":0},"paid_fallback_pricing_checked_at_ms":{"type":"null","value":null}}}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"hash"},{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"number","value":-1},"usage_requests":{"type":"number","value":0},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null}}}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"request_log"},{"type":"string","value":"key-alpha"},{"type":"number","value":1800000000000},{"type":"string","value":"pending"}],"value":{"type":"object","value":{"id":{"type":"string","value":"pending"},"key_id":{"type":"string","value":"key-alpha"},"provider":{"type":"string","value":"metered"},"billing_status":{"type":"string","value":"pending"},"created_at_ms":{"type":"number","value":1800000000000}}}}],"expected":{"error":{"name":"Error","message":"Legacy paid fallback request has no valid pricing: key-alpha/pending"}},"digest":"042da44505ec1167e9baa16486c84b122a8b844c4939cc89e95cc5efd7ec2d87"},{"name":"empty inventory","mode":"","rows":[{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"codex_models"}],"value":{"type":"object","value":{"models":{"type":"Array","value":[{"type":"object","value":{"slug":{"type":"string","value":"gpt-5.5"},"supported_reasoning_levels":{"type":"Array","value":[{"type":"string","value":"none"},{"type":"string","value":"medium"}]}}}]},"source":{"type":"string","value":"chatgpt_codex"},"updated_at_ms":{"type":"number","value":1800000000000}}}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"model"}],"value":{"type":"string","value":"gpt-5.5"}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"reasoning_effort"}],"value":{"type":"string","value":"medium"}}],"expected":{"value":{"api_keys":0,"bounded_counters":0,"handoff_phase":"no_bounded_keys","bounded_baselines_created":0,"bounded_baselines_reconciled":0,"legacy_usage_delta_applied":0,"kernel_repo_records":0,"kernel_org_records":0,"paid_fallback_records":0,"runtime_config_written":true}},"digest":"aa5ec707590e7daa9ebed3e099fe799ec39fa3d4d93be552bd534448099aec2c"},{"name":"unlimited pair","mode":"","rows":[{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"codex_models"}],"value":{"type":"object","value":{"models":{"type":"Array","value":[{"type":"object","value":{"slug":{"type":"string","value":"gpt-5.5"},"supported_reasoning_levels":{"type":"Array","value":[{"type":"string","value":"none"},{"type":"string","value":"medium"}]}}}]},"source":{"type":"string","value":"chatgpt_codex"},"updated_at_ms":{"type":"number","value":1800000000000}}}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"model"}],"value":{"type":"string","value":"gpt-5.5"}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"reasoning_effort"}],"value":{"type":"string","value":"medium"}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"id"},{"type":"string","value":"key-alpha"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"number","value":-1},"usage_requests":{"type":"number","value":0},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null},"name":{"type":"string","value":"Synthetic"},"prefix":{"type":"string","value":"u_1234567890"},"hash":{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"},"created_at_ms":{"type":"number","value":1799999940000},"paid_fallback_model_ids":{"type":"Array","value":[]},"paid_fallback_quota_per_credit":{"type":"number","value":0},"paid_fallback_pricing_checked_at_ms":{"type":"null","value":null}}}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"hash"},{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"number","value":-1},"usage_requests":{"type":"number","value":0},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null}}}}],"expected":{"value":{"api_keys":1,"bounded_counters":0,"handoff_phase":"no_bounded_keys","bounded_baselines_created":0,"bounded_baselines_reconciled":0,"legacy_usage_delta_applied":0,"kernel_repo_records":0,"kernel_org_records":0,"paid_fallback_records":0,"runtime_config_written":true}},"digest":"3422415b3a29b9af280459333eafb189817fa3e4cb251b9b6f90c7e0f7b1d5b6"},{"name":"bounded seed","mode":"","rows":[{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"codex_models"}],"value":{"type":"object","value":{"models":{"type":"Array","value":[{"type":"object","value":{"slug":{"type":"string","value":"gpt-5.5"},"supported_reasoning_levels":{"type":"Array","value":[{"type":"string","value":"none"},{"type":"string","value":"medium"}]}}}]},"source":{"type":"string","value":"chatgpt_codex"},"updated_at_ms":{"type":"number","value":1800000000000}}}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"model"}],"value":{"type":"string","value":"gpt-5.5"}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"reasoning_effort"}],"value":{"type":"string","value":"medium"}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"id"},{"type":"string","value":"key-alpha"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"number","value":10},"usage_requests":{"type":"number","value":4},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null},"name":{"type":"string","value":"Synthetic"},"prefix":{"type":"string","value":"u_1234567890"},"hash":{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"},"created_at_ms":{"type":"number","value":1799999940000},"paid_fallback_model_ids":{"type":"Array","value":[]},"paid_fallback_quota_per_credit":{"type":"number","value":0},"paid_fallback_pricing_checked_at_ms":{"type":"null","value":null}}}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"hash"},{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"number","value":10},"usage_requests":{"type":"number","value":4},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null}}}}],"expected":{"value":{"api_keys":1,"bounded_counters":1,"handoff_phase":"predeploy_seed","bounded_baselines_created":1,"bounded_baselines_reconciled":0,"legacy_usage_delta_applied":4,"kernel_repo_records":0,"kernel_org_records":0,"paid_fallback_records":0,"runtime_config_written":true}},"digest":"fdfde6c3200802344ca8a1cb8e23dda82942e8cf6340474d6673ebd77262c0da"},{"name":"bounded reconcile","mode":"","rows":[{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"codex_models"}],"value":{"type":"object","value":{"models":{"type":"Array","value":[{"type":"object","value":{"slug":{"type":"string","value":"gpt-5.5"},"supported_reasoning_levels":{"type":"Array","value":[{"type":"string","value":"none"},{"type":"string","value":"medium"}]}}}]},"source":{"type":"string","value":"chatgpt_codex"},"updated_at_ms":{"type":"number","value":1800000000000}}}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"model"}],"value":{"type":"string","value":"gpt-5.5"}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"reasoning_effort"}],"value":{"type":"string","value":"medium"}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"id"},{"type":"string","value":"key-alpha"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"number","value":10},"usage_requests":{"type":"number","value":4},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null},"name":{"type":"string","value":"Synthetic"},"prefix":{"type":"string","value":"u_1234567890"},"hash":{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"},"created_at_ms":{"type":"number","value":1799999940000},"paid_fallback_model_ids":{"type":"Array","value":[]},"paid_fallback_quota_per_credit":{"type":"number","value":0},"paid_fallback_pricing_checked_at_ms":{"type":"null","value":null}}}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"hash"},{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"number","value":10},"usage_requests":{"type":"number","value":4},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null}}}},{"key":[{"type":"string","value":"uos_ai"},{"type":"string","value":"migrations"},{"type":"string","value":"kv_read_incident_v2"}],"value":{"type":"object","value":{"version":{"type":"number","value":2},"api_key_quota_v3_handoff_version":{"type":"number","value":1}}}}],"expected":{"value":{"api_keys":1,"bounded_counters":1,"handoff_phase":"postdeploy_reconcile","bounded_baselines_created":1,"bounded_baselines_reconciled":1,"legacy_usage_delta_applied":4,"kernel_repo_records":0,"kernel_org_records":0,"paid_fallback_records":0,"runtime_config_written":true}},"digest":"7fc8c5baba7f0204addb889c04df9b7204826a911b20bbda0742dffec87db8da"},{"name":"raw stored spreads","mode":"","rows":[{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"codex_models"}],"value":{"type":"object","value":{"models":{"type":"Array","value":[{"type":"object","value":{"slug":{"type":"string","value":"gpt-5.5"},"supported_reasoning_levels":{"type":"Array","value":[{"type":"string","value":"none"},{"type":"string","value":"medium"}]}}}]},"source":{"type":"string","value":"chatgpt_codex"},"updated_at_ms":{"type":"number","value":1800000000000}}}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"model"}],"value":{"type":"string","value":"gpt-5.5"}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"reasoning_effort"}],"value":{"type":"string","value":"medium"}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"id"},{"type":"string","value":"key-alpha"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"number","value":-1},"usage_requests":{"type":"number","value":0},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null},"name":{"type":"string","value":"Synthetic"},"prefix":{"type":"string","value":"u_1234567890"},"hash":{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"},"created_at_ms":{"type":"number","value":1799999940000},"paid_fallback_model_ids":{"type":"Array","value":[]},"paid_fallback_quota_per_credit":{"type":"number","value":0},"paid_fallback_pricing_checked_at_ms":{"type":"null","value":null},"opaque":{"type":"Uint8Array","value":"AP8D","byteLength":3},"big":{"type":"bigint","value":"1329227995784915872903807060280344576"},"missing":{"type":"undefined"},"usage_quota_version":{"type":"string","value":"3"}}}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"hash"},{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"number","value":-1},"usage_requests":{"type":"number","value":0},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null},"opaque":{"type":"Uint8Array","value":"AQI","byteLength":2},"big":{"type":"bigint","value":"-7"},"missing":{"type":"undefined"},"usage_quota_version":{"type":"string","value":"3"}}}}],"expected":{"value":{"api_keys":1,"bounded_counters":0,"handoff_phase":"no_bounded_keys","bounded_baselines_created":0,"bounded_baselines_reconciled":0,"legacy_usage_delta_applied":0,"kernel_repo_records":0,"kernel_org_records":0,"paid_fallback_records":0,"runtime_config_written":true}},"digest":"1b9ccb44a1f48f78ab87ca41456ff793ead8e49031edb797b62d84d5ab169600"},{"name":"missing snapshot","mode":"","rows":[],"expected":{"error":{"name":"Error","message":"Codex model snapshot is missing"}},"digest":"4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945"},{"name":"truthy invalid snapshot","mode":"","rows":[{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"codex_models"}],"value":{"type":"boolean","value":true}}],"expected":{"error":{"name":"RuntimeConfigError","message":"runtime config requires a non-empty Codex model catalog"}},"digest":"2f82ae4ebcfec465536cde4e784e40fce6cdfb27f9e7a82e6a607e67e8c63324"},{"name":"malformed default","mode":"","rows":[{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"codex_models"}],"value":{"type":"object","value":{"models":{"type":"Array","value":[{"type":"object","value":{"slug":{"type":"string","value":"gpt-5.5"},"supported_reasoning_levels":{"type":"Array","value":[{"type":"string","value":"none"},{"type":"string","value":"medium"}]}}}]},"source":{"type":"string","value":"chatgpt_codex"},"updated_at_ms":{"type":"number","value":1800000000000}}}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"reasoning_effort"}],"value":{"type":"string","value":"medium"}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"model"}],"value":{"type":"number","value":1}}],"expected":{"error":{"name":"TypeError","message":"options.defaultModel?.trim is not a function. (In 'options.defaultModel?.trim()', 'options.defaultModel?.trim' is undefined)"}},"digest":"086c5262cf795191378d3900b924005640f5d2c78a2fa3fc2e375e96a5658c36"},{"name":"changed id pair","mode":"changed","rows":[{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"codex_models"}],"value":{"type":"object","value":{"models":{"type":"Array","value":[{"type":"object","value":{"slug":{"type":"string","value":"gpt-5.5"},"supported_reasoning_levels":{"type":"Array","value":[{"type":"string","value":"none"},{"type":"string","value":"medium"}]}}}]},"source":{"type":"string","value":"chatgpt_codex"},"updated_at_ms":{"type":"number","value":1800000000000}}}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"model"}],"value":{"type":"string","value":"gpt-5.5"}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"reasoning_effort"}],"value":{"type":"string","value":"medium"}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"id"},{"type":"string","value":"key-alpha"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"number","value":-1},"usage_requests":{"type":"number","value":0},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null},"name":{"type":"string","value":"Synthetic"},"prefix":{"type":"string","value":"u_1234567890"},"hash":{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"},"created_at_ms":{"type":"number","value":1799999940000},"paid_fallback_model_ids":{"type":"Array","value":[]},"paid_fallback_quota_per_credit":{"type":"number","value":0},"paid_fallback_pricing_checked_at_ms":{"type":"null","value":null}}}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"hash"},{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"number","value":-1},"usage_requests":{"type":"number","value":0},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null}}}}],"expected":{"error":{"name":"Error","message":"API key changed during quota V3 migration: key-alpha"}},"digest":"b6308370a51f1b29ab002681328bc49c05070d6df19caf4cfa7b5c2688dbc34c"},{"name":"failed pair CAS","mode":"pair-cas","rows":[{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"codex_models"}],"value":{"type":"object","value":{"models":{"type":"Array","value":[{"type":"object","value":{"slug":{"type":"string","value":"gpt-5.5"},"supported_reasoning_levels":{"type":"Array","value":[{"type":"string","value":"none"},{"type":"string","value":"medium"}]}}}]},"source":{"type":"string","value":"chatgpt_codex"},"updated_at_ms":{"type":"number","value":1800000000000}}}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"model"}],"value":{"type":"string","value":"gpt-5.5"}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"reasoning_effort"}],"value":{"type":"string","value":"medium"}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"id"},{"type":"string","value":"key-alpha"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"number","value":-1},"usage_requests":{"type":"number","value":0},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null},"name":{"type":"string","value":"Synthetic"},"prefix":{"type":"string","value":"u_1234567890"},"hash":{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"},"created_at_ms":{"type":"number","value":1799999940000},"paid_fallback_model_ids":{"type":"Array","value":[]},"paid_fallback_quota_per_credit":{"type":"number","value":0},"paid_fallback_pricing_checked_at_ms":{"type":"null","value":null}}}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"hash"},{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"number","value":-1},"usage_requests":{"type":"number","value":0},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null}}}}],"expected":{"error":{"name":"Error","message":"API key changed during quota V3 migration: key-alpha"}},"digest":"1dbcb99b20b1cb7db6cc8f72eae2a00806248abb0527a3626e1f73569aae6cec"},{"name":"failed terminal commit is source success","mode":"final-cas","rows":[{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"codex_models"}],"value":{"type":"object","value":{"models":{"type":"Array","value":[{"type":"object","value":{"slug":{"type":"string","value":"gpt-5.5"},"supported_reasoning_levels":{"type":"Array","value":[{"type":"string","value":"none"},{"type":"string","value":"medium"}]}}}]},"source":{"type":"string","value":"chatgpt_codex"},"updated_at_ms":{"type":"number","value":1800000000000}}}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"model"}],"value":{"type":"string","value":"gpt-5.5"}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"reasoning_effort"}],"value":{"type":"string","value":"medium"}}],"expected":{"value":{"api_keys":0,"bounded_counters":0,"handoff_phase":"no_bounded_keys","bounded_baselines_created":0,"bounded_baselines_reconciled":0,"legacy_usage_delta_applied":0,"kernel_repo_records":0,"kernel_org_records":0,"paid_fallback_records":0,"runtime_config_written":true}},"digest":"a26fd58d05a11fd5a6875cb0ee1944ab78dfeb5959543292991484cdf4bdd798"},{"name":"already V3 pair avoids upgrade","mode":"","rows":[{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"codex_models"}],"value":{"type":"object","value":{"models":{"type":"Array","value":[{"type":"object","value":{"slug":{"type":"string","value":"gpt-5.5"},"supported_reasoning_levels":{"type":"Array","value":[{"type":"string","value":"none"},{"type":"string","value":"medium"}]}}}]},"source":{"type":"string","value":"chatgpt_codex"},"updated_at_ms":{"type":"number","value":1800000000000}}}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"model"}],"value":{"type":"string","value":"gpt-5.5"}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"reasoning_effort"}],"value":{"type":"string","value":"medium"}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"id"},{"type":"string","value":"key-alpha"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"number","value":-1},"usage_requests":{"type":"number","value":0},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null},"name":{"type":"string","value":"Synthetic"},"prefix":{"type":"string","value":"u_1234567890"},"hash":{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"},"created_at_ms":{"type":"number","value":1799999940000},"paid_fallback_model_ids":{"type":"Array","value":[]},"paid_fallback_quota_per_credit":{"type":"number","value":0},"paid_fallback_pricing_checked_at_ms":{"type":"null","value":null},"usage_quota_version":{"type":"number","value":3}}}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"hash"},{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"number","value":-1},"usage_requests":{"type":"number","value":0},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null},"usage_quota_version":{"type":"number","value":3}}}}],"expected":{"value":{"api_keys":1,"bounded_counters":0,"handoff_phase":"no_bounded_keys","bounded_baselines_created":0,"bounded_baselines_reconciled":0,"legacy_usage_delta_applied":0,"kernel_repo_records":0,"kernel_org_records":0,"paid_fallback_records":0,"runtime_config_written":true}},"digest":"3422415b3a29b9af280459333eafb189817fa3e4cb251b9b6f90c7e0f7b1d5b6"},{"name":"legacy handoff marker remains phase one","mode":"","rows":[{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"codex_models"}],"value":{"type":"object","value":{"models":{"type":"Array","value":[{"type":"object","value":{"slug":{"type":"string","value":"gpt-5.5"},"supported_reasoning_levels":{"type":"Array","value":[{"type":"string","value":"none"},{"type":"string","value":"medium"}]}}}]},"source":{"type":"string","value":"chatgpt_codex"},"updated_at_ms":{"type":"number","value":1800000000000}}}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"model"}],"value":{"type":"string","value":"gpt-5.5"}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"reasoning_effort"}],"value":{"type":"string","value":"medium"}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"id"},{"type":"string","value":"key-alpha"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"number","value":10},"usage_requests":{"type":"number","value":4},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null},"name":{"type":"string","value":"Synthetic"},"prefix":{"type":"string","value":"u_1234567890"},"hash":{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"},"created_at_ms":{"type":"number","value":1799999940000},"paid_fallback_model_ids":{"type":"Array","value":[]},"paid_fallback_quota_per_credit":{"type":"number","value":0},"paid_fallback_pricing_checked_at_ms":{"type":"null","value":null}}}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"hash"},{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"number","value":10},"usage_requests":{"type":"number","value":4},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null}}}},{"key":[{"type":"string","value":"uos_ai"},{"type":"string","value":"migrations"},{"type":"string","value":"kv_read_incident_v2"}],"value":{"type":"object","value":{"version":{"type":"number","value":2},"counter_handoff_version":{"type":"number","value":1}}}}],"expected":{"value":{"api_keys":1,"bounded_counters":1,"handoff_phase":"predeploy_seed","bounded_baselines_created":1,"bounded_baselines_reconciled":0,"legacy_usage_delta_applied":4,"kernel_repo_records":0,"kernel_org_records":0,"paid_fallback_records":0,"runtime_config_written":true}},"digest":"fdfde6c3200802344ca8a1cb8e23dda82942e8cf6340474d6673ebd77262c0da"},{"name":"invalid policy is read-only","mode":"","rows":[{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"codex_models"}],"value":{"type":"object","value":{"models":{"type":"Array","value":[{"type":"object","value":{"slug":{"type":"string","value":"gpt-5.5"},"supported_reasoning_levels":{"type":"Array","value":[{"type":"string","value":"none"},{"type":"string","value":"medium"}]}}}]},"source":{"type":"string","value":"chatgpt_codex"},"updated_at_ms":{"type":"number","value":1800000000000}}}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"model"}],"value":{"type":"string","value":"gpt-5.5"}},{"key":[{"type":"string","value":"default"},{"type":"string","value":"reasoning_effort"}],"value":{"type":"string","value":"medium"}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"id"},{"type":"string","value":"key-alpha"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"string","value":"bad"},"usage_requests":{"type":"number","value":0},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null},"name":{"type":"string","value":"Synthetic"},"prefix":{"type":"string","value":"u_1234567890"},"hash":{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"},"created_at_ms":{"type":"number","value":1799999940000},"paid_fallback_model_ids":{"type":"Array","value":[]},"paid_fallback_quota_per_credit":{"type":"number","value":0},"paid_fallback_pricing_checked_at_ms":{"type":"null","value":null}}}},{"key":[{"type":"string","value":"ubq_ai"},{"type":"string","value":"api_keys"},{"type":"string","value":"hash"},{"type":"string","value":"ddddddddddddddddddddddddddddddddddddddddddd"}],"value":{"type":"object","value":{"id":{"type":"string","value":"key-alpha"},"expires_at_ms":{"type":"number","value":-1},"revoked_at_ms":{"type":"null","value":null},"usage_limit_requests":{"type":"string","value":"bad"},"usage_requests":{"type":"number","value":0},"usage_reset_at_ms":{"type":"number","value":1800000060000},"window_ms":{"type":"number","value":60000},"paid_fallback_enabled":{"type":"boolean","value":false},"paid_fallback_limit_microcredits":{"type":"number","value":0},"paid_fallback_spent_microcredits":{"type":"number","value":0},"paid_fallback_reserved_microcredits":{"type":"number","value":0},"paid_fallback_reservation_request_id":{"type":"null","value":null}}}}],"expected":{"error":{"name":"Error","message":"API key policy validation failed: api key hash policy has invalid core fields: ddddddddddddddddddddddddddddddddddddddddddd; api key has invalid v2 policy: key-alpha"}},"digest":"a3d8f69907990db6e3011e4b57e5f1e1a5c842e7595eb97b4b56e323a3a19629"}])ORACLE");
    return value;
  }
  std::size_t oracleScenario = 0;
  rt::Task<void> runOracleScenario() {
    CredentialScope scope; auto kv = std::make_shared<MapKv>();
    const auto& item = oracleVectors().as_array()[oracleScenario];
    const std::string name(*rt::getString(item, "name")); const std::string mode(*rt::getString(item, "mode"));
    for (const auto& entry : item.at("rows").as_array()) kv->seed(profile::toKey(entry.at("key").as_array()), profile::toValue(entry.at("value")));
    bool changed = false;
    kv->beforeGet = [&](const rt::KvKey& key, int) {
      if (mode == "changed" && key.size() > 2 && key[2].isString() && key[2].string() == "id" && !changed) { changed = true; kv->erase(key); }
    };
    kv->refuseCommit = [&](int, const std::vector<rt::KvKey>& writes) {
      if (rt::startsWith(mode, "ledger-")) {
        for (const auto& key : writes) {
          if (key.size() > 2 && key[1].isString() && key[1].string() == "paid_fallback" && key[2].isString() && key[2].string() == "ledger") {
            if (mode == "ledger-winner") for (const auto& write : writes) kv->seed(write, patch(pendingLedger("key-alpha", "pending"), {{"billing_status", "reconciled"}}));
            return true;
          }
        }
      }
      if (mode == "pair-cas") return true;
      if (mode == "final-cas") for (const auto& key : writes) if (key.size() > 1 && key[1].isString() && key[1].string() == "runtime_config") return true;
      return false;
    };
    json::value actual;
    try {
      AWAIT(result, migration::migrateKvReadIncidentV2(kv)); actual = json::object{{"value", json::value_from(result)}};
    } catch (...) {
      const auto reason = std::current_exception(); actual = json::object{{"error", json::object{{"name", rt::errorName(reason).value_or("Error")}, {"message", rt::errorMessage(reason).value_or(rt::reasonToString(reason))}}}};
    }
    if (!rt::deepEqual(actual, item.at("expected"))) rt::test::report(__FILE__, __LINE__, name + " actual " + rt::jsonStringify(actual) + " expected " + rt::jsonStringify(item.at("expected")), false);
    std::vector<json::value> rows;
    for (const auto& [key, entry] : kv->entries) { (void)key; rows.emplace_back(json::object{{"key", json::value_from(profile::keyToJson(entry.key))}, {"value", profile::valueToJson(entry.value)}}); }
    rt::sort(rows, [](const json::value& a, const json::value& b) { return static_cast<double>(rt::localeCompare(rt::jsonStringify(a.at("key")), rt::jsonStringify(b.at("key")))); });
    json::array durableRows; for (const auto& entry : rows) durableRows.push_back(entry);
    const auto digest = rt::crypto::createHash("sha256").update(rt::jsonStringify(durableRows)).digest("hex"); EXPECT_EQ(digest, std::string(*rt::getString(item, "digest")));
  }
  const bool registeredOracleScenarios = [] {
    const auto& vectors = oracleVectors().as_array();
    for (std::size_t index = 0; index < vectors.size(); ++index) {
      const std::string name = "Bun orchestrator differential: " + std::string(*rt::getString(vectors[index], "name"));
      rt::test::registry().push_back({name, [index] { oracleScenario = index; rt::test::runAsync(runOracleScenario); }});
    }
    return true;
  }();
}
