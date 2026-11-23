// Links: tests/cache_kv_migration_usage_coverage_test.cpp
//        src/api_key_policy.cpp src/cache/kv_migration_usage.cpp src/authority/native_kernel.cpp tests/helpers/counting_kv.cpp
//        src/utils.cpp src/authority/native_api_key.cpp src/types.cpp src/kernel/quota_v2.cpp
//        src/cache/kv_migration_paid_fallback.cpp src/kernel/quota_reservations.cpp src/authority/native_api_key_policy.cpp src/kv.cpp
//        src/api_keys.cpp src/authority/native_paid_policy.cpp src/storage/kv.cpp src/authority/native_kernel_contracts.cpp
//        src/http.cpp src/storage/sqlite_readonly.cpp src/authority/native_paid.cpp src/config.cpp
//        src/authority/native_contracts.cpp src/provider/cerebras_rate_limits.cpp src/storage/native_kv.cpp
//        src/auth/relay.cpp src/release.cpp
// Port of tests/cache-kv-migration-usage-coverage.test.ts. MapKv extends the shared deterministic
// stand-in with the source fixture's expiry observations, refused commits and racing atomic hook.
#include "uos/api_key_policy.hpp"
#include "uos/api_keys.hpp"
#include "uos/cache/kv_migration_usage.hpp"
#include "uos/config.hpp"
#include "uos/defaults.hpp"
#include "uos/kernel/quota_v2.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "tests/helpers/counting_kv.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace usage = uos::cache::kv_migration_usage;
namespace policy = uos::api_key_policy;
namespace keys = uos::api_keys;
namespace quota = uos::kernel::quota_v2;
namespace defaults = uos::defaults;
namespace counting = uos::tests::helpers::counting_kv;

namespace {
  constexpr double NOW = 1800000000000;
  constexpr double WINDOW_MS = 60000;
  const std::string API_KEY_HASH(43, 'h');
  const std::string OTHER_HASH(43, 'i');
  struct CredentialScope {
    rt::env::ScopedEnvironment env{{{"HOME", "/nonexistent/w05c8-isolated"}}};
    std::string saved = uos::config::config.codexAuthJsonB64;
    CredentialScope() { uos::config::config.codexAuthJsonB64.clear(); }
    ~CredentialScope() { uos::config::config.codexAuthJsonB64 = saved; }
  };
  rt::KvValue patch(rt::KvValue value, const json::object& overrides) {
    for (const auto& item : overrides) value.asObject().set(std::string(item.key()), rt::KvValue(item.value()));
    return value;
  }
  class MapKv;
  class MapAtomic final : public rt::ForwardingAtomicOperation {
  public:
    MapAtomic(MapKv& store_, std::unique_ptr<rt::AtomicOperation> inner_) : rt::ForwardingAtomicOperation(std::move(inner_)), store(store_) {}
    rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override {
      if (options.expireIn) expiries.set(counting::encodeKey(key), *options.expireIn);
      inner->set(std::move(key), std::move(value), options); return *this;
    }
    rt::AtomicOperation& delete_(rt::KvKey key) override { deletions.push_back(counting::encodeKey(key)); inner->delete_(std::move(key)); return *this; }
    rt::Task<rt::KvCommitResult> commit() override;
  private:
    MapKv& store;
    rt::OrderedMap<std::string, double> expiries;
    std::vector<std::string> deletions;
  };
  class MapKv : public counting::CountingKv {
  public:
    bool failCommits = false;
    rt::OrderedMap<std::string, double> expireIn;
    std::function<void()> beforeAtomic{};
    rt::KvValue value(const rt::KvKey& key) const { const auto* entry = entries.get(counting::encodeKey(key)); return entry ? entry->value : rt::KvValue(nullptr); }
    std::unique_ptr<rt::AtomicOperation> atomic() override {
      if (beforeAtomic) beforeAtomic();
      return std::make_unique<MapAtomic>(*this, counting::CountingKv::atomic());
    }
  };
  rt::Task<rt::KvCommitResult> MapAtomic::commit() {
    if (store.failCommits) { co_await rt::microtask(); co_return rt::KvCommitResult{}; }
    const auto result = co_await inner->commit();
    if (result.ok) { for (const auto& key : deletions) store.expireIn.delete_(key); for (const auto& [key, value] : expiries) store.expireIn.set(key, value); }
    co_return result;
  }
  policy::ApiKeyPolicy policyFor(const json::object& overrides = {}) {
    const policy::ApiKeyPolicy base{.token_hash = API_KEY_HASH, .key_id = "key-alpha", .expires_at_ms = -1,
      .usage_limit_requests = 10, .window_ms = WINDOW_MS, .window_start_ms = NOW - WINDOW_MS,
      .usage_reset_at_ms = NOW, .policy_version = "v3:60000", .paid_fallback_enabled = false,
      .paid_fallback_limit_microcredits = 0, .paid_fallback_spent_microcredits = 0,
      .paid_fallback_reserved_microcredits = 0, .paid_fallback_reservation_request_id = std::nullopt};
    const json::value value = rt::objectSpread(json::value_from(base).as_object(), overrides);
    return json::value_to<policy::ApiKeyPolicy>(value);
  }
  rt::KvValue baselineRecord(const policy::ApiKeyPolicy& p, const json::object& overrides = {}) {
    return patch(rt::KvValue(json::object{{"version", 1}, {"key_id", p.key_id}, {"policy_version", p.policy_version},
      {"window_start_ms", p.window_start_ms}, {"last_legacy_usage_requests", 0}, {"seeded_at_ms", NOW},
      {"reconciled_at_ms", nullptr}, {"reconciliation_runs", 0}}), overrides);
  }
  rt::KvValue windowRecord(const policy::ApiKeyPolicy& p, const json::object& overrides = {}) {
    return patch(rt::KvValue(json::object{{"v", 3}, {"key_id", p.key_id}, {"policy_version", p.policy_version},
      {"window_start_ms", p.window_start_ms}, {"window_reset_at_ms", p.usage_reset_at_ms}, {"committed_requests", 0},
      {"reserved_requests", 0}, {"updated_at_ms", NOW}}), overrides);
  }
  rt::KvValue requestRecord(const policy::ApiKeyPolicy& p, const std::string& requestId, const json::object& overrides = {}) {
    return patch(rt::KvValue(json::object{{"v", 3}, {"key_id", p.key_id}, {"request_id", requestId}, {"route", "/v1/responses"},
      {"state", "reserved"}, {"reserved_at_ms", NOW - 1000}, {"lease_expires_at_ms", NOW + 60000}, {"provider", nullptr},
      {"dispatched_at_ms", nullptr}, {"released_at_ms", nullptr}, {"release_reason", nullptr}}), overrides);
  }
  rt::KvValue kvU64(const rt::BigInt& value) { return rt::KvValue::object({{"value", value}}); }
  std::vector<std::string> sorted(std::vector<std::string> values) {
    rt::sort(values, [](const std::string& a, const std::string& b) { return static_cast<double>(rt::localeCompare(a, b)); }); return values;
  }
  std::string encoded(const rt::KvKey& key) { return *rt::kvJsonStringify(rt::kvValueFromKey(key)); }
  void expectErrors(const std::vector<std::string>& actual, const std::vector<std::string>& expected) { EXPECT_EQ(actual, expected); }
  void expectHandoff(const usage::BoundedCounterHandoffResult& value, bool created, bool reconciled, double delta) {
    const usage::BoundedCounterHandoffResult expected{.baseline_created = created, .baseline_reconciled = reconciled, .legacy_usage_delta_applied = delta}; EXPECT_EQ(value, expected);
  }
}

ASYNC_TEST("bounded counter handoff: seeds the baseline and V3 window from the legacy usage count") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const auto p = policyFor();
  auto task = usage::migrateBoundedCounterHandoff(stub, p, 4, NOW, false); const auto handoff = co_await std::move(task); expectHandoff(handoff, true, false, 4);
  EXPECT_EQ(stub->value(usage::apiKeyUsageV2MigrationBaselineKey(p)), baselineRecord(p, {{"last_legacy_usage_requests", 4}}));
  EXPECT_EQ(stub->value(policy::apiKeyUsageV3WindowKey(p)), windowRecord(p, {{"committed_requests", 4}}));
  EXPECT_EQ(*stub->expireIn.get(encoded(policy::apiKeyUsageV3WindowKey(p))), policy::API_KEY_USAGE_V3_RETENTION_MS);
}
ASYNC_TEST("bounded counter handoff: a stored legacy counter outweighs the caller count and never lowers the window") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const auto p = policyFor();
  stub->seed(policy::apiKeyUsageV2Key(p), kvU64(rt::BigInt(9))); stub->seed(policy::apiKeyUsageV3WindowKey(p), windowRecord(p, {{"committed_requests", 12}}));
  auto task = usage::migrateBoundedCounterHandoff(stub, p, 4, NOW, false); const auto handoff = co_await std::move(task); expectHandoff(handoff, true, false, 9);
  EXPECT_EQ(stub->value(usage::apiKeyUsageV2MigrationBaselineKey(p))["last_legacy_usage_requests"].asNumber(), 9);
  EXPECT_EQ(stub->value(policy::apiKeyUsageV3WindowKey(p))["committed_requests"].asNumber(), 12);
}
ASYNC_TEST("bounded counter handoff: reconcile updates apply only the legacy delta and count reconciliation runs") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const auto p = policyFor();
  auto task = usage::migrateBoundedCounterHandoff(stub, p, 4, NOW, true); const auto seeded = co_await std::move(task); expectHandoff(seeded, true, true, 4);
  auto baseline = stub->value(usage::apiKeyUsageV2MigrationBaselineKey(p)); EXPECT_EQ(baseline["reconciled_at_ms"].asNumber(), NOW); EXPECT_EQ(baseline["reconciliation_runs"].asNumber(), 1);
  auto raising = usage::migrateBoundedCounterHandoff(stub, p, 10, NOW + 1000, true); const auto raised = co_await std::move(raising); expectHandoff(raised, false, true, 6);
  baseline = stub->value(usage::apiKeyUsageV2MigrationBaselineKey(p)); EXPECT_EQ(baseline["last_legacy_usage_requests"].asNumber(), 10); EXPECT_EQ(baseline["reconciled_at_ms"].asNumber(), NOW + 1000); EXPECT_EQ(baseline["reconciliation_runs"].asNumber(), 2);
  EXPECT_EQ(stub->value(policy::apiKeyUsageV3WindowKey(p)), windowRecord(p, {{"committed_requests", 10}, {"updated_at_ms", NOW + 1000}}));
  auto lowering = usage::migrateBoundedCounterHandoff(stub, p, 3, NOW + 2000, true); const auto lowered = co_await std::move(lowering); expectHandoff(lowered, false, true, 0);
  EXPECT_EQ(stub->value(policy::apiKeyUsageV3WindowKey(p))["committed_requests"].asNumber(), 10);
}
ASYNC_TEST("bounded counter handoff: an unreconciled update clears the reconciliation state") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const auto p = policyFor();
  auto task = usage::migrateBoundedCounterHandoff(stub, p, 4, NOW, true); co_await std::move(task);
  auto updating = usage::migrateBoundedCounterHandoff(stub, p, 6, NOW + 5000, false); const auto result = co_await std::move(updating); expectHandoff(result, false, false, 2);
  const auto baseline = stub->value(usage::apiKeyUsageV2MigrationBaselineKey(p)); EXPECT(baseline["reconciled_at_ms"].isNull()); EXPECT_EQ(baseline["reconciliation_runs"].asNumber(), 0);
}
ASYNC_TEST("bounded counter handoff: rejects an out-of-range legacy V2 counter") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const auto p = policyFor(); stub->seed(policy::apiKeyUsageV2Key(p), kvU64(rt::BigInt::fromString("9007199254740992")));
  auto task = usage::migrateBoundedCounterHandoff(stub, p, 5, NOW, false); EXPECT_REJECTS_MESSAGE(std::move(task), rt::JsError, "Legacy V2 API-key counter is out of range: key-alpha"); EXPECT(stub->value(policy::apiKeyUsageV3WindowKey(p)).isNull());
}
ASYNC_TEST("bounded counter handoff: rejects a legacy V2 counter that is not a U64") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const auto p = policyFor(); stub->seed(policy::apiKeyUsageV2Key(p), rt::KvValue::object({{"value", "not-a-counter"}}));
  auto task = usage::migrateBoundedCounterHandoff(stub, p, 5, NOW, false); EXPECT_REJECTS_MESSAGE(std::move(task), rt::JsError, "Legacy V2 API-key counter is invalid: key-alpha");
}
ASYNC_TEST("bounded counter handoff: rejects a V3 window whose identity does not match the policy") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const auto p = policyFor(); stub->seed(policy::apiKeyUsageV3WindowKey(p), windowRecord(p, {{"policy_version", "v3:3600000"}}));
  auto task = usage::migrateBoundedCounterHandoff(stub, p, 5, NOW, false); EXPECT_REJECTS_MESSAGE(std::move(task), rt::JsError, "V3 API-key window identity is invalid: key-alpha"); EXPECT(stub->value(usage::apiKeyUsageV2MigrationBaselineKey(p)).isNull());
}
ASYNC_TEST("bounded counter handoff: rejects an unusable stored baseline") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const auto p = policyFor(); stub->seed(usage::apiKeyUsageV2MigrationBaselineKey(p), rt::KvValue::object({{"version", 1}, {"key_id", p.key_id}})); stub->seed(policy::apiKeyUsageV3WindowKey(p), windowRecord(p));
  auto task = usage::migrateBoundedCounterHandoff(stub, p, 5, NOW, false); EXPECT_REJECTS_MESSAGE(std::move(task), rt::JsError, "Bounded counter handoff baseline is invalid: key-alpha");
}
ASYNC_TEST("bounded counter handoff: rejects an existing baseline whose V3 window is missing") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const auto p = policyFor(); stub->seed(usage::apiKeyUsageV2MigrationBaselineKey(p), baselineRecord(p, {{"last_legacy_usage_requests", 2}}));
  auto task = usage::migrateBoundedCounterHandoff(stub, p, 5, NOW, false); EXPECT_REJECTS_MESSAGE(std::move(task), rt::JsError, "V3 API-key window is missing: key-alpha");
}
ASYNC_TEST("bounded counter handoff: five lost commits fail closed instead of writing a partial seed") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const auto p = policyFor(); stub->failCommits = true;
  auto task = usage::migrateBoundedCounterHandoff(stub, p, 5, NOW, false); EXPECT_REJECTS_MESSAGE(std::move(task), rt::JsError, "Bounded counter handoff changed concurrently: key-alpha"); EXPECT(stub->value(policy::apiKeyUsageV3WindowKey(p)).isNull());
}
ASYNC_TEST("bounded counter handoff: a conflicting update commit fails closed as well") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const auto p = policyFor(); stub->seed(usage::apiKeyUsageV2MigrationBaselineKey(p), baselineRecord(p, {{"last_legacy_usage_requests", 2}})); stub->seed(policy::apiKeyUsageV3WindowKey(p), windowRecord(p, {{"committed_requests", 2}})); stub->failCommits = true;
  auto task = usage::migrateBoundedCounterHandoff(stub, p, 7, NOW, false); EXPECT_REJECTS_MESSAGE(std::move(task), rt::JsError, "Bounded counter handoff changed concurrently: key-alpha"); EXPECT_EQ(stub->value(usage::apiKeyUsageV2MigrationBaselineKey(p))["last_legacy_usage_requests"].asNumber(), 2);
}
ASYNC_TEST("bounded counter handoff: a lost CAS retries and keeps the competing writer's higher count") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const auto p = policyFor(); bool raced = false;
  stub->beforeAtomic = [stub, p, &raced] { if (!raced) { raced = true; stub->seed(policy::apiKeyUsageV3WindowKey(p), windowRecord(p, {{"committed_requests", 99}})); } };
  auto task = usage::migrateBoundedCounterHandoff(stub, p, 5, NOW, false); const auto handoff = co_await std::move(task); expectHandoff(handoff, true, false, 5); stub->beforeAtomic = {};
  EXPECT_EQ(stub->value(policy::apiKeyUsageV3WindowKey(p))["committed_requests"].asNumber(), 99); EXPECT_EQ(stub->value(usage::apiKeyUsageV2MigrationBaselineKey(p))["last_legacy_usage_requests"].asNumber(), 5);
}
TEST("baseline validation: accepts both reconciliation states and rejects every malformed field") {
  const auto p = policyFor(); const auto valid = baselineRecord(p); const auto normalized = usage::normalizeApiKeyUsageV2MigrationBaseline(valid); ASSERT(normalized); EXPECT_EQ(rt::KvValue(json::value_from(*normalized)), valid);
  const auto reconciled = baselineRecord(p, {{"reconciled_at_ms", NOW + 1}, {"reconciliation_runs", 3}}); const auto parsed = usage::normalizeApiKeyUsageV2MigrationBaseline(reconciled); ASSERT(parsed); EXPECT_EQ(rt::KvValue(json::value_from(*parsed)), reconciled);
  const std::vector<rt::KvValue> rejects{nullptr, "baseline", rt::KvValue::array({}), patch(valid, {{"version", 2}}), patch(valid, {{"key_id", ""}}), patch(valid, {{"policy_version", ""}}), patch(valid, {{"policy_version", 7}}), patch(valid, {{"window_start_ms", -1}}), patch(valid, {{"last_legacy_usage_requests", -1}}), patch(valid, {{"seeded_at_ms", 0}}), patch(valid, {{"reconciled_at_ms", 0}}), patch(valid, {{"reconciled_at_ms", "now"}}), patch(valid, {{"reconciliation_runs", -1}})};
  for (const auto& value : rejects) EXPECT(!usage::normalizeApiKeyUsageV2MigrationBaseline(value));
}
TEST("legacy usage helpers: only an unexpired, unrevoked record keeps its counter") {
  const auto active = rt::KvValue::object({{"usage_requests", 7}, {"usage_reset_at_ms", NOW + 1}, {"revoked_at_ms", nullptr}});
  EXPECT_EQ(usage::currentLegacyUsage(active, NOW), 7); EXPECT_EQ(usage::migrationPolicyNow(active, NOW), NOW); EXPECT_EQ(usage::currentLegacyUsage(patch(active, {{"usage_reset_at_ms", NOW}}), NOW), 0);
  const auto revoked = patch(active, {{"revoked_at_ms", NOW - 5000}}); EXPECT_EQ(usage::migrationPolicyNow(revoked, NOW), NOW); EXPECT_EQ(usage::migrationPolicyNow(patch(revoked, {{"usage_reset_at_ms", NOW - 5000}}), NOW), NOW - 5001); EXPECT_EQ(usage::migrationPolicyNow(patch(revoked, {{"usage_reset_at_ms", 0}}), NOW), 0);
}
ASYNC_TEST("countReconciledBoundedCounterBaselines: counts only reconciled, well-formed baselines") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>();
  const auto reconciled = policyFor({{"key_id", "key-reconciled"}}); const auto pending = policyFor({{"key_id", "key-pending"}}); const auto malformed = policyFor({{"key_id", "key-malformed"}});
  stub->seed(usage::apiKeyUsageV2MigrationBaselineKey(reconciled), baselineRecord(reconciled, {{"reconciled_at_ms", NOW}})); stub->seed(usage::apiKeyUsageV2MigrationBaselineKey(pending), baselineRecord(pending)); stub->seed(usage::apiKeyUsageV2MigrationBaselineKey(malformed), rt::KvValue::object({{"version", 1}, {"key_id", malformed.key_id}}));
  const auto count = co_await usage::countReconciledBoundedCounterBaselines(stub); EXPECT_EQ(count, 1);
}

namespace {
  template <class T> rt::Task<void> rejects(rt::Task<T> task, std::string text) {
    rt::Reason error;
    try { co_await std::move(task); } catch (...) { error = std::current_exception(); }
    EXPECT(static_cast<bool>(error)); if (error) EXPECT(rt::includes(rt::reasonToString(error), text));
  }
  rt::KvValue kernelPolicyRow(const std::string& scope, const std::string& owner, const std::string& repo = "", const json::object& overrides = {}) {
    json::object value{{"v", 2}, {"scope", scope}, {"owner", owner}}; if (scope == "repo") value["repo"] = repo;
    value["usage_limit_requests"] = 50; value["window_ms"] = WINDOW_MS; value["expires_at_ms"] = -1;
    value["created_at_ms"] = NOW - 10000; value["updated_at_ms"] = NOW - 10000; return patch(rt::KvValue(value), overrides);
  }
  rt::KvValue kernelWindowRow(const std::string& scope, const std::string& owner, const std::string& repo = "", const json::object& overrides = {}) {
    json::object value{{"v", 2}, {"scope", scope}, {"owner", owner}}; if (scope == "repo") value["repo"] = repo;
    value["usage_requests"] = 0; value["reserved_requests"] = 0; value["usage_reset_at_ms"] = NOW + WINDOW_MS;
    value["applied_window_ms"] = WINDOW_MS; value["created_at_ms"] = NOW - 10000; value["updated_at_ms"] = NOW - 10000;
    return patch(rt::KvValue(value), overrides);
  }
  rt::KvValue kernelReservationRow(const std::string& scope, const std::string& owner, const std::string& repo,
                                    const std::string& requestId, double created, const json::object& overrides = {}) {
    json::object value{{"v", 2}, {"scope", scope}, {"owner", owner}}; if (scope == "repo") value["repo"] = repo;
    value["request_id"] = requestId; value["route"] = "/v1/chat/completions"; value["window_created_at_ms"] = created;
    value["window_reset_at_ms"] = NOW + WINDOW_MS; value["state"] = "reserved"; value["terminal_intent"] = nullptr;
    value["reserved_at_ms"] = NOW - 1000; value["lease_expires_at_ms"] = NOW + 60000; value["committed_at_ms"] = nullptr;
    value["released_at_ms"] = nullptr; value["release_reason"] = nullptr; return patch(rt::KvValue(value), overrides);
  }
  rt::KvKey legacyRepoLimitKey(const std::string& owner, const std::string& repo) { return {"ubq_ai", "kernel_auth", "limits", owner, repo}; }
  rt::KvKey legacyOrgLimitKey(const std::string& owner) { return {"ubq_ai", "kernel_auth", "org_limits", owner}; }
  rt::KvValue legacyLimitRecord(const json::object& overrides = {}) {
    return patch(rt::KvValue(json::object{{"usage_limit_requests", 25}, {"usage_requests", 3}, {"usage_reset_at_ms", NOW + WINDOW_MS},
      {"window_ms", WINDOW_MS}, {"expires_at_ms", -1}, {"created_at_ms", NOW - 30000}, {"updated_at_ms", NOW - 20000}}), overrides);
  }
  void expectMigrated(const usage::KernelQuotaV2MigrationResult& actual, double repo, double org) {
    const usage::KernelQuotaV2MigrationResult expected{.repo = repo, .org = org}; EXPECT_EQ(actual, expected);
  }
  std::size_t errorsIncluding(const std::vector<std::string>& errors, const std::string& text) {
    std::size_t result = 0; for (const auto& error : errors) if (rt::includes(error, text)) ++result; return result;
  }
}
ASYNC_TEST("kernel quota migration: an empty store reports zero migrated scopes") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const auto result = co_await usage::migrateKernelQuotaV2(stub, NOW); expectMigrated(result, 0, 0);
}
ASYNC_TEST("kernel quota migration: legacy repo and org limits become V2 policy and window rows") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); stub->seed(legacyRepoLimitKey("ubq", "repo-a"), legacyLimitRecord()); stub->seed(legacyOrgLimitKey("ubq"), legacyLimitRecord({{"usage_limit_requests", -1}, {"usage_requests", 8}}));
  const auto result = co_await usage::migrateKernelQuotaV2(stub, NOW); expectMigrated(result, 1, 1);
  EXPECT(stub->value(legacyRepoLimitKey("ubq", "repo-a")).isNull()); EXPECT(stub->value(legacyOrgLimitKey("ubq")).isNull());
  EXPECT_EQ(stub->value(quota::kernelRepoPolicyKey("ubq", "repo-a")), kernelPolicyRow("repo", "ubq", "repo-a", {{"usage_limit_requests", 25}, {"created_at_ms", NOW - 30000}, {"updated_at_ms", NOW}}));
  EXPECT_EQ(stub->value(quota::kernelRepoWindowKey("ubq", "repo-a")), kernelWindowRow("repo", "ubq", "repo-a", {{"usage_requests", 3}, {"created_at_ms", NOW - 30000}, {"updated_at_ms", NOW}}));
  EXPECT_EQ(stub->value(quota::kernelOrgPolicyKey("ubq")), kernelPolicyRow("org", "ubq", "", {{"usage_limit_requests", -1}, {"created_at_ms", NOW - 30000}, {"updated_at_ms", NOW}}));
  EXPECT_EQ(stub->value(quota::kernelOrgWindowKey("ubq"))["usage_requests"].asNumber(), 8);
}
ASYNC_TEST("kernel quota migration: an expired legacy window rolls forward with zero usage") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); stub->seed(legacyRepoLimitKey("ubq", "repo-b"), legacyLimitRecord({{"usage_reset_at_ms", NOW - 1000}, {"usage_requests", 5}}));
  const auto result = co_await usage::migrateKernelQuotaV2(stub, NOW); expectMigrated(result, 1, 0); const auto window = stub->value(quota::kernelRepoWindowKey("ubq", "repo-b"));
  EXPECT_EQ(window["usage_requests"].asNumber(), 0); EXPECT_EQ(window["usage_reset_at_ms"].asNumber(), NOW + WINDOW_MS); EXPECT_EQ(window["applied_window_ms"].asNumber(), WINDOW_MS);
}
ASYNC_TEST("kernel quota migration: a default-backed legacy row drops the redundant policy and uses the stored default window") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); stub->seed(defaults::DEFAULT_KERNEL_POLICY_LIMIT_KEY, -1); stub->seed(defaults::DEFAULT_KERNEL_POLICY_WINDOW_KEY, 30000);
  stub->seed(legacyRepoLimitKey("ubq", "repo-c"), legacyLimitRecord({{"usage_limit_requests", -1}, {"window_ms", 30000}, {"expires_at_ms", -1}, {"usage_reset_at_ms", NOW - 1}}));
  const auto result = co_await usage::migrateKernelQuotaV2(stub, NOW); expectMigrated(result, 1, 0); EXPECT(stub->value(quota::kernelRepoPolicyKey("ubq", "repo-c")).isNull());
  const auto window = stub->value(quota::kernelRepoWindowKey("ubq", "repo-c")); EXPECT_EQ(window["applied_window_ms"].asNumber(), 30000); EXPECT_EQ(window["usage_reset_at_ms"].asNumber(), NOW + 30000); EXPECT(stub->value(legacyRepoLimitKey("ubq", "repo-c")).isNull());
}
ASYNC_TEST("kernel quota migration: an unusable stored default falls back to the built-in limit and window") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); stub->seed(defaults::DEFAULT_KERNEL_POLICY_LIMIT_KEY, -5); stub->seed(defaults::DEFAULT_KERNEL_POLICY_WINDOW_KEY, 0);
  stub->seed(legacyRepoLimitKey("ubq", "repo-defaults"), legacyLimitRecord({{"usage_limit_requests", defaults::DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS}, {"window_ms", defaults::DEFAULT_KERNEL_POLICY_WINDOW_MS}, {"expires_at_ms", -1}, {"usage_reset_at_ms", NOW - 1}}));
  const auto result = co_await usage::migrateKernelQuotaV2(stub, NOW); expectMigrated(result, 1, 0); const auto window = stub->value(quota::kernelRepoWindowKey("ubq", "repo-defaults"));
  EXPECT_EQ(window["applied_window_ms"].asNumber(), defaults::DEFAULT_KERNEL_POLICY_WINDOW_MS); EXPECT(stub->value(quota::kernelRepoPolicyKey("ubq", "repo-defaults")).isNull());
}
ASYNC_TEST("kernel quota migration: an existing V2 policy row only triggers deletion of the legacy row") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); stub->seed(quota::kernelRepoPolicyKey("ubq", "repo-d"), kernelPolicyRow("repo", "ubq", "repo-d")); stub->seed(quota::kernelRepoWindowKey("ubq", "repo-d"), kernelWindowRow("repo", "ubq", "repo-d", {{"usage_requests", 11}})); stub->seed(legacyRepoLimitKey("ubq", "repo-d"), legacyLimitRecord({{"usage_requests", 99}}));
  const auto result = co_await usage::migrateKernelQuotaV2(stub, NOW); expectMigrated(result, 1, 0); EXPECT(stub->value(legacyRepoLimitKey("ubq", "repo-d")).isNull()); EXPECT_EQ(stub->value(quota::kernelRepoPolicyKey("ubq", "repo-d")), kernelPolicyRow("repo", "ubq", "repo-d"));
  const auto window = stub->value(quota::kernelRepoWindowKey("ubq", "repo-d")); EXPECT_EQ(window["usage_requests"].asNumber(), 11); EXPECT_EQ(window["updated_at_ms"].asNumber(), NOW - 10000);
}
ASYNC_TEST("kernel quota migration: a matching V2 window only raises its usage to the legacy high-water mark") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); stub->seed(quota::kernelRepoWindowKey("ubq", "repo-e"), kernelWindowRow("repo", "ubq", "repo-e", {{"usage_requests", 4}, {"created_at_ms", NOW - 500000}})); stub->seed(legacyRepoLimitKey("ubq", "repo-e"), legacyLimitRecord({{"usage_requests", 9}}));
  const auto result = co_await usage::migrateKernelQuotaV2(stub, NOW); expectMigrated(result, 1, 0); const auto window = stub->value(quota::kernelRepoWindowKey("ubq", "repo-e")); EXPECT_EQ(window["usage_requests"].asNumber(), 9); EXPECT_EQ(window["created_at_ms"].asNumber(), NOW - 500000); EXPECT_EQ(window["updated_at_ms"].asNumber(), NOW);
  EXPECT_EQ(stub->value(quota::kernelRepoPolicyKey("ubq", "repo-e")), kernelPolicyRow("repo", "ubq", "repo-e", {{"usage_limit_requests", 25}, {"created_at_ms", NOW - 30000}, {"updated_at_ms", NOW}}));
}
ASYNC_TEST("kernel quota migration: string and unusable legacy numbers are coerced or defaulted") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); stub->seed(legacyRepoLimitKey("ubq", "repo-f"), legacyLimitRecord({{"usage_limit_requests", "40"}, {"usage_requests", "6"}, {"window_ms", "not-a-number"}, {"usage_reset_at_ms", rt::Number::POSITIVE_INFINITY}, {"created_at_ms", "abc"}}));
  const auto result = co_await usage::migrateKernelQuotaV2(stub, NOW); expectMigrated(result, 1, 0); const auto p = stub->value(quota::kernelRepoPolicyKey("ubq", "repo-f")); EXPECT_EQ(p["usage_limit_requests"].asNumber(), 40); EXPECT_EQ(p["window_ms"].asNumber(), defaults::DEFAULT_KERNEL_POLICY_WINDOW_MS); EXPECT_EQ(p["created_at_ms"].asNumber(), NOW);
  const auto window = stub->value(quota::kernelRepoWindowKey("ubq", "repo-f")); EXPECT_EQ(window["usage_requests"].asNumber(), 6); EXPECT_EQ(window["created_at_ms"].asNumber(), NOW);
}
ASYNC_TEST("kernel quota migration: a malformed legacy value fails loudly and writes nothing") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); stub->seed(legacyRepoLimitKey("ubq", "repo-g"), rt::KvValue::object({{"usage_limit_requests", 5}, {"window_ms", 0}}));
  auto rejected = rejects(usage::migrateKernelQuotaV2(stub, NOW), "legacy kernel quota value is malformed"); co_await std::move(rejected);
  EXPECT(stub->value(quota::kernelRepoWindowKey("ubq", "repo-g")).isNull()); EXPECT(!stub->value(legacyRepoLimitKey("ubq", "repo-g")).isNull());
}
ASYNC_TEST("kernel quota migration: a malformed legacy key fails loudly") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); stub->seed(rt::KvKey{"ubq_ai", "kernel_auth", "limits", "ubq"}, legacyLimitRecord()); auto rejected = rejects(usage::migrateKernelQuotaV2(stub, NOW), "legacy kernel quota key is malformed"); co_await std::move(rejected);
}
ASYNC_TEST("kernel quota migration: a lost CAS aborts the migration instead of splitting a policy") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); stub->seed(legacyRepoLimitKey("ubq", "repo-h"), legacyLimitRecord()); stub->failCommits = true;
  auto rejected = rejects(usage::migrateKernelQuotaV2(stub, NOW), "legacy kernel quota changed concurrently: ubq/repo-h"); co_await std::move(rejected); EXPECT(!stub->value(legacyRepoLimitKey("ubq", "repo-h")).isNull());
}
ASYNC_TEST("API key usage V3 inventory: valid rows report counts and consistent reservation aggregates") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const auto p = policyFor(); stub->seed(policy::apiKeyUsageV3WindowKey(p), windowRecord(p, {{"committed_requests", 2}, {"reserved_requests", 1}})); stub->seed(policy::apiKeyUsageV3RequestKey(p, "req-1"), requestRecord(p, "req-1")); stub->seed(policy::apiKeyUsageV3RequestKey(p, "req-2"), requestRecord(p, "req-2", {{"state", "released"}, {"released_at_ms", NOW - 10}, {"release_reason", "completed"}}));
  rt::OrderedSet<std::string> known{p.key_id}; auto task = usage::inspectApiKeyUsageV3(stub, known); const auto inventory = co_await std::move(task);
  EXPECT_EQ(inventory.windows, 1); EXPECT_EQ(inventory.requests, 2); EXPECT(inventory.errors.empty());
}
ASYNC_TEST("API key usage V3 inventory: reserved aggregates are cross-checked and complete terminal states pass") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const auto p = policyFor(); stub->seed(policy::apiKeyUsageV3WindowKey(p), windowRecord(p, {{"reserved_requests", 4}})); stub->seed(policy::apiKeyUsageV3RequestKey(p, "req-1"), requestRecord(p, "req-1"));
  stub->seed(policy::apiKeyUsageV3RequestKey(p, "req-2"), requestRecord(p, "req-2", {{"state", "dispatched"}, {"provider", "lithos"}, {"dispatched_at_ms", NOW}})); stub->seed(policy::apiKeyUsageV3RequestKey(p, "req-3"), requestRecord(p, "req-3", {{"state", "released"}, {"released_at_ms", NOW - 10}, {"release_reason", "completed"}})); stub->seed(policy::apiKeyUsageV3RequestKey(p, "req-4"), requestRecord(p, "req-4", {{"state", "dispatched"}, {"provider", nullptr}, {"dispatched_at_ms", NOW}}));
  rt::OrderedSet<std::string> known{p.key_id}; auto task = usage::inspectApiKeyUsageV3(stub, known); const auto inventory = co_await std::move(task); EXPECT_EQ(inventory.requests, 4);
  expectErrors(inventory.errors, {"API key usage V3 request is malformed: " + encoded(policy::apiKeyUsageV3RequestKey(p, "req-4")), "API key usage V3 reserved aggregate is inconsistent: " + encoded(rt::KvKey{p.key_id, p.policy_version, p.window_start_ms})});
}
ASYNC_TEST("API key usage V3 inventory: malformed window rows are rejected and orphaned windows are reported") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const auto p = policyFor(); stub->seed(policy::apiKeyUsageV3WindowKey(p), windowRecord(p, {{"window_start_ms", p.window_start_ms + 1}})); stub->seed(rt::KvKey{policy::API_KEY_USAGE_V3_WINDOW_PREFIX, p.key_id, p.policy_version, p.window_start_ms, "extra"}, windowRecord(p)); stub->seed(rt::KvKey{policy::API_KEY_USAGE_V3_WINDOW_PREFIX, 7, p.policy_version, p.window_start_ms}, windowRecord(p)); const auto orphan = policyFor({{"key_id", "key-orphan"}}); stub->seed(policy::apiKeyUsageV3WindowKey(orphan), windowRecord(orphan));
  rt::OrderedSet<std::string> known{p.key_id}; auto task = usage::inspectApiKeyUsageV3(stub, known); const auto inventory = co_await std::move(task); EXPECT_EQ(inventory.windows, 4); EXPECT_EQ(inventory.requests, 0); EXPECT_EQ(errorsIncluding(inventory.errors, "API key usage V3 window is malformed"), 3u); EXPECT(rt::includes(inventory.errors, std::string("API key usage V3 window is orphaned: key-orphan")));
}
ASYNC_TEST("API key usage V3 inventory: malformed and windowless request rows are reported") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const auto p = policyFor(); stub->seed(policy::apiKeyUsageV3WindowKey(p), windowRecord(p)); stub->seed(policy::apiKeyUsageV3RequestKey(p, "req-bad-value"), rt::KvValue::object({{"v", 3}})); stub->seed(rt::KvKey{policy::API_KEY_USAGE_V3_REQUEST_PREFIX, p.key_id, p.policy_version, p.window_start_ms, "req-extra", "tail"}, requestRecord(p, "req-extra")); stub->seed(rt::KvKey{policy::API_KEY_USAGE_V3_REQUEST_PREFIX, p.key_id, p.policy_version, p.window_start_ms, 12}, requestRecord(p, "req-number")); stub->seed(policy::apiKeyUsageV3RequestKey(p, "req-shifted"), requestRecord(p, "req-shifted", {{"key_id", "other-key"}})); const auto windowless = policyFor({{"key_id", "key-windowless"}}); stub->seed(policy::apiKeyUsageV3RequestKey(windowless, "req-windowless"), requestRecord(windowless, "req-windowless"));
  rt::OrderedSet<std::string> known{p.key_id, windowless.key_id}; auto task = usage::inspectApiKeyUsageV3(stub, known); const auto inventory = co_await std::move(task); EXPECT_EQ(inventory.windows, 1); EXPECT_EQ(inventory.requests, 5); EXPECT_EQ(errorsIncluding(inventory.errors, "API key usage V3 request is malformed"), 4u); EXPECT_EQ(errorsIncluding(inventory.errors, "API key usage V3 request has no window:"), 1u);
}
ASYNC_TEST("kernel quota V2 inventory: consistent rows are counted without errors") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>();
  stub->seed(quota::kernelRepoPolicyKey("ubq", "repo-a"), kernelPolicyRow("repo", "ubq", "repo-a")); stub->seed(quota::kernelOrgPolicyKey("ubq"), kernelPolicyRow("org", "ubq")); stub->seed(quota::kernelRepoWindowKey("ubq", "repo-a"), kernelWindowRow("repo", "ubq", "repo-a", {{"reserved_requests", 1}})); stub->seed(quota::kernelOrgWindowKey("ubq"), kernelWindowRow("org", "ubq")); stub->seed(quota::kernelRepoReservationKey("ubq", "repo-a", NOW - 10000, "req-1"), kernelReservationRow("repo", "ubq", "repo-a", "req-1", NOW - 10000)); stub->seed(quota::kernelOrgReservationKey("ubq", NOW - 10000, "req-2"), kernelReservationRow("org", "ubq", "", "req-2", NOW - 10000, {{"state", "committed"}, {"terminal_intent", "committed"}, {"committed_at_ms", NOW - 500}}));
  const auto inventory = co_await usage::inspectKernelQuotaV2(stub);
  const usage::KernelQuotaV2Inventory expected{.repoPolicies = 1, .orgPolicies = 1, .repoWindows = 1, .orgWindows = 1, .repoReservations = 1, .orgReservations = 1, .errors = {}}; EXPECT_EQ(inventory, expected);
}
ASYNC_TEST("kernel quota V2 inventory: malformed policy and window rows are reported with their scope") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>();
  const rt::KvKey repoPolicy{"uos_ai", "kernel_quota", "v2", "repo_policy", "ubq", "repo-bad"}; const rt::KvKey orgPolicy{"uos_ai", "kernel_quota", "v2", "org_policy", "ubq", "extra"}; const auto repoWindow = quota::kernelRepoWindowKey("ubq", "repo-b"); const rt::KvKey orgWindow{"uos_ai", "kernel_quota", "v2", "org_window", 5};
  stub->seed(repoPolicy, rt::KvValue::object({{"v", 2}})); stub->seed(orgPolicy, kernelPolicyRow("org", "ubq")); stub->seed(repoWindow, rt::KvValue::object({{"v", 2}, {"scope", "repo"}, {"owner", "ubq"}, {"repo", "repo-b"}})); stub->seed(orgWindow, kernelWindowRow("org", "ubq"));
  const auto inventory = co_await usage::inspectKernelQuotaV2(stub);
  const usage::KernelQuotaV2Inventory expected{.repoPolicies = 1, .orgPolicies = 1, .repoWindows = 1, .orgWindows = 1, .repoReservations = 0, .orgReservations = 0,
    .errors = {"kernel quota V2 policy is malformed: " + encoded(repoPolicy), "kernel quota V2 policy is malformed: " + encoded(orgPolicy), "kernel quota V2 window is malformed: " + encoded(repoWindow), "kernel quota V2 window is malformed: " + encoded(orgWindow)}}; EXPECT_EQ(inventory, expected);
}
ASYNC_TEST("kernel quota V2 inventory: malformed reservation rows are reported and identities checked") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const double created = NOW - 10000;
  stub->seed(rt::KvKey{"uos_ai", "kernel_quota", "v2", "repo_reservation", "ubq", "repo-a", created, 9}, kernelReservationRow("repo", "ubq", "repo-a", "req-1", created)); stub->seed(quota::kernelRepoReservationKey("ubq", "repo-a", created, "req-2"), rt::KvValue::object({{"v", 2}, {"scope", "repo"}})); stub->seed(quota::kernelRepoReservationKey("ubq", "repo-a", created, "req-3"), kernelReservationRow("repo", "ubq", "repo-a", "req-mismatched", created)); stub->seed(rt::KvKey{quota::kernelOrgReservationKey("ubq", created, "req-4"), "tail"}, kernelReservationRow("org", "ubq", "", "req-4", created));
  const auto inventory = co_await usage::inspectKernelQuotaV2(stub); EXPECT_EQ(inventory.repoReservations, 3); EXPECT_EQ(inventory.orgReservations, 1); EXPECT_EQ(errorsIncluding(inventory.errors, "kernel quota V2 reservation is malformed"), 4u);
}
ASYNC_TEST("kernel quota V2 inventory: reserved aggregates and windowless reservations are cross-checked") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const double created = NOW - 10000;
  stub->seed(quota::kernelRepoWindowKey("ubq", "repo-a"), kernelWindowRow("repo", "ubq", "repo-a", {{"reserved_requests", 1}, {"created_at_ms", created}})); stub->seed(quota::kernelRepoReservationKey("ubq", "repo-a", created, "req-1"), kernelReservationRow("repo", "ubq", "repo-a", "req-1", created)); stub->seed(quota::kernelRepoReservationKey("ubq", "repo-a", created, "req-2"), kernelReservationRow("repo", "ubq", "repo-a", "req-2", created)); stub->seed(quota::kernelOrgReservationKey("ubq", NOW - 20000, "req-3"), kernelReservationRow("org", "ubq", "", "req-3", NOW - 20000));
  const auto inventory = co_await usage::inspectKernelQuotaV2(stub); EXPECT_EQ(inventory.repoReservations, 2); EXPECT_EQ(inventory.orgReservations, 1);
  expectErrors(inventory.errors, {"kernel quota V2 reserved aggregate is inconsistent: " + encoded(rt::KvKey{"repo", "ubq", "repo-a", created}), "kernel quota V2 active reservation has no window: " + encoded(rt::KvKey{"org", "ubq", NOW - 20000})});
}
ASYNC_TEST("kernel quota V2 inventory: a window without the reservation aggregate field is not cross-checked") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const double created = NOW - 10000;
  auto window = kernelWindowRow("repo", "ubq", "repo-a", {{"created_at_ms", created}}); window.asObject().erase("reserved_requests"); stub->seed(quota::kernelRepoWindowKey("ubq", "repo-a"), window); stub->seed(quota::kernelRepoReservationKey("ubq", "repo-a", created, "req-1"), kernelReservationRow("repo", "ubq", "repo-a", "req-1", created));
  const auto inventory = co_await usage::inspectKernelQuotaV2(stub); EXPECT(inventory.errors.empty()); EXPECT_EQ(inventory.repoWindows, 1);
}
ASYNC_TEST("kernel quota migration: a non-record legacy value fails as malformed") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); stub->seed(legacyOrgLimitKey("ubq"), "not-a-record"); auto rejected = rejects(usage::migrateKernelQuotaV2(stub, NOW), "legacy kernel quota value is malformed"); co_await std::move(rejected);
}
ASYNC_TEST("kernel quota migration: a lost CAS while dropping a redundant legacy row aborts the migration") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); stub->seed(quota::kernelOrgPolicyKey("ubq"), kernelPolicyRow("org", "ubq")); stub->seed(legacyOrgLimitKey("ubq"), legacyLimitRecord()); stub->failCommits = true;
  auto rejected = rejects(usage::migrateKernelQuotaV2(stub, NOW), "legacy kernel quota changed concurrently: ubq/"); co_await std::move(rejected); EXPECT(!stub->value(legacyOrgLimitKey("ubq")).isNull());
}
ASYNC_TEST("API key usage V3 inventory: an unusable window value and an orphaned request are reported") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const auto p = policyFor(); const auto orphan = policyFor({{"key_id", "key-unknown"}});
  stub->seed(policy::apiKeyUsageV3WindowKey(p), rt::KvValue::object({{"v", 3}})); stub->seed(policy::apiKeyUsageV3WindowKey(orphan), windowRecord(orphan)); stub->seed(policy::apiKeyUsageV3RequestKey(orphan, "req-orphan"), requestRecord(orphan, "req-orphan"));
  rt::OrderedSet<std::string> known{p.key_id}; auto task = usage::inspectApiKeyUsageV3(stub, known); const auto inventory = co_await std::move(task);
  const std::vector<std::string> expected{"API key usage V3 window is malformed: " + encoded(policy::apiKeyUsageV3WindowKey(p)), "API key usage V3 window is orphaned: key-unknown", "API key usage V3 request is orphaned: key-unknown/req-orphan", "API key usage V3 reserved aggregate is inconsistent: " + encoded(rt::KvKey{"key-unknown", orphan.policy_version, orphan.window_start_ms})}; expectErrors(sorted(inventory.errors), sorted(expected));
}
namespace {
  rt::KvValue apiKeyHashRecord(const json::object& overrides = {}) {
    return patch(rt::KvValue(json::object{{"id", "key-alpha"}, {"expires_at_ms", -1}, {"revoked_at_ms", nullptr}, {"usage_limit_requests", 10},
      {"usage_requests", 2}, {"usage_reset_at_ms", NOW + WINDOW_MS}, {"window_ms", WINDOW_MS}, {"usage_quota_version", 3},
      {"paid_fallback_enabled", false}, {"paid_fallback_limit_microcredits", 0}, {"paid_fallback_spent_microcredits", 0},
      {"paid_fallback_reserved_microcredits", 0}, {"paid_fallback_reservation_request_id", nullptr}}), overrides);
  }
  rt::KvValue apiKeyRecord(const json::object& overrides = {}) {
    json::object value{{"id", "key-alpha"}, {"name", "alpha"}, {"prefix", "u_0123456789"}, {"hash", API_KEY_HASH}, {"created_at_ms", NOW - 10000},
      {"expires_at_ms", -1}, {"revoked_at_ms", nullptr}, {"usage_limit_requests", 10}, {"usage_requests", 2}, {"usage_reset_at_ms", NOW + WINDOW_MS},
      {"window_ms", WINDOW_MS}, {"usage_quota_version", 3}, {"paid_fallback_enabled", false}, {"paid_fallback_limit_microcredits", 0},
      {"paid_fallback_spent_microcredits", 0}, {"paid_fallback_reserved_microcredits", 0}, {"paid_fallback_reservation_request_id", nullptr},
      {"paid_fallback_model_ids", json::array{}}, {"paid_fallback_quota_per_credit", 0}, {"paid_fallback_pricing_checked_at_ms", nullptr}};
    return patch(rt::KvValue(value), overrides);
  }
}
ASYNC_TEST("strict api key pairs: a consistent id and hash row is paired") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const auto record = apiKeyRecord(); stub->seed(keys::apiKeyIdKey(record["id"].asString()), record); stub->seed(keys::apiKeyHashKey(API_KEY_HASH), apiKeyHashRecord());
  const auto inventory = co_await usage::inspectStrictApiKeyPairs(stub); EXPECT(inventory.errors.empty()); ASSERT(inventory.pairs.size() == 1); EXPECT_EQ(inventory.pairs[0].record["id"].asString(), record["id"].asString()); EXPECT_EQ(inventory.pairs[0].hashRecord["usage_requests"].asNumber(), 2);
}
ASYNC_TEST("strict api key pairs: unusable hash rows are reported and left unpaired") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const rt::KvKey invalid{keys::API_KEY_HASH_PREFIX, "short"}; stub->seed(invalid, apiKeyHashRecord()); stub->seed(keys::apiKeyHashKey(OTHER_HASH), rt::KvValue::object({{"id", "key-beta"}}));
  const auto inventory = co_await usage::inspectStrictApiKeyPairs(stub); EXPECT(inventory.pairs.empty()); const std::vector<std::string> expected{"api key hash entry has an invalid key: " + encoded(invalid), "api key hash policy has invalid core fields: " + OTHER_HASH}; expectErrors(sorted(inventory.errors), sorted(expected));
}
ASYNC_TEST("strict api key pairs: unusable id rows are reported individually") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); stub->seed(keys::apiKeyHashKey(API_KEY_HASH), apiKeyHashRecord()); const rt::KvKey invalid{keys::API_KEY_ID_PREFIX, 42}; stub->seed(invalid, apiKeyRecord()); stub->seed(keys::apiKeyIdKey("key-shifted"), apiKeyRecord()); stub->seed(keys::apiKeyIdKey("key-thin"), rt::KvValue::object({{"id", "key-thin"}}));
  const auto inventory = co_await usage::inspectStrictApiKeyPairs(stub); EXPECT(inventory.pairs.empty()); const std::vector<std::string> expected{"api key id entry has an invalid key: " + encoded(invalid), "api key id key suffix does not match record id: key=key-shifted record=key-alpha", "api key has invalid v2 policy: key-thin", "api key hash policy is orphaned: " + API_KEY_HASH}; expectErrors(sorted(inventory.errors), sorted(expected));
}
ASYNC_TEST("strict api key pairs: a missing or inconsistent hash policy blocks the pair") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); const auto record = apiKeyRecord(); stub->seed(keys::apiKeyIdKey("key-alpha"), record);
  const auto missing = co_await usage::inspectStrictApiKeyPairs(stub); EXPECT(missing.pairs.empty()); expectErrors(missing.errors, {"api key hash policy is missing or inconsistent: key-alpha"});
  auto inconsistent = std::make_shared<MapKv>(); inconsistent->seed(keys::apiKeyIdKey("key-alpha"), record); inconsistent->seed(keys::apiKeyHashKey(API_KEY_HASH), apiKeyHashRecord({{"usage_requests", 9}}));
  const auto inventory = co_await usage::inspectStrictApiKeyPairs(inconsistent); EXPECT(inventory.pairs.empty()); const std::vector<std::string> expected{"api key hash policy is missing or inconsistent: key-alpha", "api key hash policy is orphaned: " + API_KEY_HASH}; expectErrors(sorted(inventory.errors), sorted(expected));
}
ASYNC_TEST("strict api key pairs: a second id row claiming the same hash is inconsistent, not a duplicate reference") {
  CredentialScope scope; auto stub = std::make_shared<MapKv>(); stub->seed(keys::apiKeyIdKey("key-alpha"), apiKeyRecord({{"id", "key-alpha"}})); stub->seed(keys::apiKeyIdKey("key-beta"), apiKeyRecord({{"id", "key-beta"}})); stub->seed(keys::apiKeyHashKey(API_KEY_HASH), apiKeyHashRecord());
  const auto inventory = co_await usage::inspectStrictApiKeyPairs(stub); ASSERT(inventory.pairs.size() == 1); EXPECT_EQ(inventory.pairs[0].record["id"].asString(), std::string("key-alpha")); expectErrors(inventory.errors, {"api key hash policy is missing or inconsistent: key-beta"});
}
