// Links: tests/codex_manual_reset_state_test.cpp
//        src/config.cpp src/cache/kv_migration_validation.cpp src/codex/manual_reset_state.cpp src/cache/kv_migration_paid_fallback.cpp
//        src/authority/native_api_key.cpp tests/helpers/counting_kv.cpp src/cache/kv_migration_usage.cpp src/authority/native_paid_policy.cpp
//        src/kv_conversion/profile_json.cpp src/authority/native_api_key_policy.cpp src/api_keys.cpp src/authority/native_kernel.cpp
//        src/authority/native_kernel_contracts.cpp src/api_key_policy.cpp src/types.cpp src/cache/kv_migration_base.cpp
//        src/kv.cpp src/utils.cpp src/kernel/quota_v2.cpp src/kernel/quota_reservations.cpp
//        src/codex/banked_reset.cpp src/runtime_config.cpp src/release.cpp src/authority/native_paid.cpp
//        src/models/codex_models.cpp src/storage/kv.cpp src/http.cpp src/storage/sqlite_readonly.cpp
//        src/defaults.cpp src/storage/native_kv.cpp src/codex/banked_reset_provider.cpp src/codex/reset_settings.cpp
//        src/provider/cerebras_rate_limits.cpp src/auth/relay.cpp src/codex/auth.cpp
//        src/codex/capacity_routing.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_capacity.cpp src/codex/routing_state.cpp
//        src/codex/routing_mutations.cpp src/authority/native_codex_mutations.cpp src/authority/native_codex_evaluation.cpp src/codex/native_auth.cpp
//        src/codex/external_auth.cpp src/codex/rate_limit_429.cpp src/provider/health.cpp src/authority/native_codex_429.cpp
//        src/codex/native_auth_transport.cpp src/bounded_response_body.cpp src/runtime/websocket.cpp src/provider/claude_code_accounts.cpp
//        src/provider/claude_code_credentials.cpp tests/helpers/codex_diff_kv.cpp src/authority/native_codex.cpp src/authority/native_paid_contracts.cpp
//        src/cache/prompt_analytics.cpp src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp
//        src/cache/telemetry_gate.cpp src/codex/account_routing.cpp src/codex/auth_refresh.cpp src/codex/dispatch.cpp
//        src/codex/experiment.cpp src/codex/overage_settings.cpp src/codex/routing_evaluation.cpp src/codex/routing_serial.cpp
//        src/inference_deadline.cpp src/metered_quota.cpp src/models/codex_models_availability.cpp src/optional_telemetry_queue.cpp
//        src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp
//        src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp
//        src/provider/capacity_rollups.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code_quota.cpp
//        src/provider/claude_code_usage_schema.cpp src/provider/selection.cpp src/runtime/http.cpp src/runtime/http_body.cpp
//        src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp src/storage/key_compatibility.cpp
//        src/storage/value_compatibility.cpp tests/helpers/codex_banked_reset_harness.cpp src/codex/banked_reset_claim.cpp src/codex/banked_reset_submission.cpp
//        src/codex/banked_reset_pool.cpp src/codex/manual_reset.cpp tests/helpers/manual_reset_harness.cpp
// Port of all four cases in tests/codex-manual-reset-state.test.ts.
#include "uos/codex/manual_reset_state.hpp"
#include "uos/codex/manual_reset.hpp"
#include "tests/helpers/manual_reset_harness.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/cache/kv_migration_base.hpp"
#include "uos/cache/kv_migration_validation.hpp"
#include "uos/config.hpp"
#include "uos/kv_conversion/profile_json.hpp"
#include "uos/rt/env.hpp"
#include "tests/helpers/counting_kv.hpp"

#include <string>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::codex::manual_reset_state;

namespace {
  json::object receipt() {
    json::object value;
    value["v"] = 1;
    value["account_id_hash"] = std::string(64, 'a');
    value["credential_version"] = std::string(64, 'b');
    value["quota_reset_at_ms"] = rt::makeNumber(1700000060000.0);
    value["routing_generation"] = 2;
    value["created_at_ms"] = rt::makeNumber(1700000000000.0);
    value["promoted_at_ms"] = rt::makeNumber(1700000001000.0);
    value["quota_signal_at_ms"] = nullptr;
    return value;
  }
  // `{ ...receipt, [key]: value }`
  json::value with(std::string_view key, json::value value) {
    json::object out = receipt();
    rt::objectDefine(out, key, std::move(value));
    return out;
  }
}

TEST("manual reset recovery refuses malformed stored identity or promotion evidence") {
  const std::vector<json::value> values = {
    json::value(nullptr),
    with("account_id_hash", "other"),
    with("credential_version", "token"),
    with("access_token", "unapproved"),
    with("routing_generation", -1),
    with("promoted_at_ms", rt::makeNumber(1700000000000.0 - 1)),
    with("quota_reset_at_ms", "later"),
  };
  for (const json::value& value : values) EXPECT(!parseManualResetIntent(value).has_value());
}

namespace {
  namespace base = uos::cache::kv_migration_base;
  namespace validation = uos::cache::kv_migration_validation;
  namespace profile = uos::kv_conversion::profile_json;
  using FakeKv = uos::tests::helpers::counting_kv::CountingKv;
  struct CredentialScope {
    rt::env::ScopedEnvironment environment{{{"HOME", "/nonexistent/w05c8-isolated"}}};
    std::string saved = uos::config::config.codexAuthJsonB64;
    CredentialScope() { uos::config::config.codexAuthJsonB64.clear(); }
    ~CredentialScope() { uos::config::config.codexAuthJsonB64 = saved; }
  };
}
ASYNC_TEST("manual reset intent is durable across prod export/import and exact-value roundtrip") {
  CredentialScope scope;
  const rt::KvKey key{CODEX_MANUAL_RESET_PREFIX, std::string(64, 'a')};
  const base::KvMigrationImportOptions options{{"prod", false, false}, false, false};
  EXPECT_EQ(base::classifyKvMigrationKey(key, options).action, std::string("import"));
  const rt::KvEntry<> row{.key = key, .value = rt::KvValue(receipt()), .versionstamp = "1"};
  const std::string line = rt::jsonStringify(json::value_from(profile::entryToJson(row)));
  EXPECT_EQ(base::parseKvMigrationEntryLine(line).value, rt::KvValue(receipt()));
  auto kv = std::make_shared<FakeKv>(); const std::vector<std::string> lines{line};
  auto importing = base::importKvMigrationLines(kv, lines, options); const auto result = co_await std::move(importing); EXPECT_EQ(result.imported, 1);
  const auto stored = co_await kv->get(key); const auto intent = parseManualResetIntent(stored.value.toJson()); ASSERT(intent);
  EXPECT(rt::deepEqual(json::value_from(*intent), receipt()));
}
ASYNC_TEST("migration validation audits malformed manual intent keys and values") {
  CredentialScope scope;
  auto kv = std::make_shared<FakeKv>(); kv->seed(rt::KvKey{CODEX_MANUAL_RESET_PREFIX, std::string(64, 'a')}, rt::KvValue(receipt()));
  std::vector<std::string> errors; co_await validation::auditManualResetIntents(kv, errors); EXPECT(errors.empty());
  kv->seed(rt::KvKey{CODEX_MANUAL_RESET_PREFIX, "wrong"}, rt::KvValue(receipt()));
  kv->seed(rt::KvKey{CODEX_MANUAL_RESET_PREFIX, std::string(64, 'c')}, rt::KvValue(with("credential_version", "token")));
  co_await validation::auditManualResetIntents(kv, errors); EXPECT_EQ(errors.size(), 2u);
}
ASYNC_TEST("export/import retains manual intent, consumed window and shared daily allowance together") {
  namespace manual = uos::tests::helpers::manual_reset_harness;
  manual::CredentialScope scope;
  auto creating = manual::manualFixture(); const auto fixture = co_await std::move(creating);
  auto consuming = uos::codex::manual_reset::consumeManualCodexReset(fixture->targetHash, fixture->dependencies); const auto consumed = co_await std::move(consuming); EXPECT_EQ(consumed.reset_status, std::string("verified"));
  std::vector<rt::KvEntry<>> rows;
  for (const auto& [encoded, entry] : fixture->kv->entries) {
    (void)encoded;
    if (entry.key.size() > 1 && (entry.key[1] == rt::KvKeyPart("codex_manual_reset") || entry.key[1] == rt::KvKeyPart("codex_reset_redemption"))) rows.push_back({.key = entry.key, .value = entry.value, .versionstamp = entry.versionstamp});
  }
  EXPECT_EQ(rows.size(), 3u);
  const base::KvMigrationImportOptions options{{"prod", false, false}, false, false};
  std::vector<std::string> lines;
  for (const auto& row : rows) lines.push_back(rt::jsonStringify(json::value_from(profile::entryToJson(row))));
  auto destination = std::make_shared<FakeKv>(); auto importing = base::importKvMigrationLines(destination, lines, options); const auto imported = co_await std::move(importing); EXPECT_EQ(imported.imported, 3);
  for (const auto& row : rows) { const auto storedRow = co_await destination->get(row.key); EXPECT_EQ(storedRow.value, row.value); }
  std::vector<std::string> errors; co_await validation::auditCodexResetRedemptions(destination, errors); co_await validation::auditManualResetIntents(destination, errors); EXPECT(errors.empty());
  const rt::KvKey malformed{"uos_ai", "codex_reset_redemption", "account_day", "v1", fixture->targetHash, "bad"}; const rt::KvValue budget(json::object{{"submission_count", 0}}); co_await destination->set(malformed, budget); co_await validation::auditCodexResetRedemptions(destination, errors); EXPECT_EQ(errors.size(), 1u);
}
