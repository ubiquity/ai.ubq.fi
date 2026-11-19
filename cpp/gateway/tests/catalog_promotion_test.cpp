// Links: tests/catalog_promotion_test.cpp
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
//        src/storage/value_compatibility.cpp src/catalog/promotion.cpp src/catalog/types.cpp src/models/codex_models_fetch.cpp
// Differential cases for src/catalog/promotion.ts, recorded under isolated Bun 1.4.0.
// Full prompt-cache-scope-experiment harness integration awaits its scope-experiment/admin dependency ports.
#include "uos/catalog/promotion.hpp"
#include "uos/runtime_config.hpp"
#include "uos/config.hpp"
#include "tests/helpers/codex_diff_kv.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/test_async.hpp"
namespace rt = uos::rt;
namespace json = uos::json;
namespace promotion = uos::catalog::promotion;
namespace diff = uos::tests::helpers::codex_diff_kv;
namespace {
  class PromotionKv;
  class Attempt : public rt::ForwardingAtomicOperation {
  public:
    Attempt(std::unique_ptr<rt::AtomicOperation> operation, int& remaining_, int& commits_) : rt::ForwardingAtomicOperation(std::move(operation)), remaining(remaining_), commits(commits_) {}
    rt::Task<rt::KvCommitResult> commit() override {
      ++commits;
      if (remaining-- > 0) { rt::KvCommitResult conflict; co_return conflict; }
      auto committing = rt::ForwardingAtomicOperation::commit(); const auto result = co_await std::move(committing); co_return result;
    }
  private:
    int& remaining;
    int& commits;
  };
  class PromotionKv : public diff::DiffKv {
  public:
    int remaining = 0;
    int commits = 0;
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<Attempt>(diff::DiffKv::atomic(), remaining, commits); }
  };
  rt::Task<void> replay(std::string scenario) {
    const rt::env::ScopedEnvironment environment{{{"HOME", "/nonexistent/w05c7-promotion"}}};
    const std::string saved = uos::config::config.codexAuthJsonB64; uos::config::config.codexAuthJsonB64.clear();
    const auto cleanup = rt::scopeExit([&] { uos::config::config.codexAuthJsonB64 = saved; uos::runtime_config::resetRuntimeConfigCacheForTest(); });
    rt::globalDateNow() = [] { return 1'700'000'000'000.0; };
    const auto vectors = rt::jsonParse(
#include "catalog_promotion_vectors.inc"
    );
    const json::value* selected = nullptr;
    for (const auto& entry : vectors.as_object().at("cases").as_array()) if (rt::getString(entry, "scenario") == scenario) selected = &entry;
    ASSERT(selected);
    auto kv = std::make_shared<PromotionKv>();
    for (const auto& row : selected->as_object().at("before").as_array()) {
      const auto key = rt::kvKeyFromValue(rt::KvValue(row.as_object().at("key")));
      const std::string stamp(*rt::getString(row, "versionstamp"));
      kv->rows.set(diff::keyOf(key), {key, rt::KvValue(row.as_object().at("value")), stamp});
      kv->version = rt::Math::max(kv->version, rt::stringToNumber(stamp));
    }
    kv->remaining = static_cast<int>(rt::toNumber(selected->as_object().at("failures")));
    const auto& inputValue = selected->as_object().at("input");
    const auto& lease = inputValue.as_object().at("lease");
    const promotion::PromptCacheScopePromotionInput input{.model = std::string(*rt::getString(inputValue, "model")), .scope = json::value_to<uos::models::codex_models::PromptCacheScope>(inputValue.as_object().at("scope")), .lease = {rt::kvKeyFromValue(rt::KvValue(lease.as_object().at("key"))), std::string(*rt::getString(lease, "owner"))}, .authPoolVersionstamp = std::string(*rt::getString(inputValue, "authPoolVersionstamp")), .catalogVersionstamp = std::string(*rt::getString(inputValue, "catalogVersionstamp")), .runtimeVersionstamp = std::string(*rt::getString(inputValue, "runtimeVersionstamp"))};
    auto promoting = promotion::promoteCodexPromptCacheScope(kv, input); const auto result = co_await std::move(promoting);
    json::object actual{{"status", result.status}}; if (result.reason) actual["reason"] = *result.reason;
    EXPECT(rt::deepEqual(actual, selected->as_object().at("result")));
    EXPECT_EQ(kv->commits, static_cast<int>(rt::toNumber(selected->as_object().at("commits"))));
    EXPECT_EQ(kv->rows.size(), selected->as_object().at("after").as_array().size());
    for (const auto& row : selected->as_object().at("after").as_array()) {
      const auto key = rt::kvKeyFromValue(rt::KvValue(row.as_object().at("key"))); const auto entry = kv->entryOf(key);
      EXPECT(rt::deepEqual(rt::storedJsonView(entry.value), row.as_object().at("value"))); EXPECT_EQ(entry.versionstamp, rt::getString(row, "versionstamp"));
    }
  }
}
ASYNC_TEST("promotion differential: promoted") { auto task = replay("promoted"); co_await std::move(task); }
ASYNC_TEST("promotion differential: non_default") { auto task = replay("non_default"); co_await std::move(task); }
ASYNC_TEST("promotion differential: invalid_scope") { auto task = replay("invalid_scope"); co_await std::move(task); }
ASYNC_TEST("promotion differential: lease_lost") { auto task = replay("lease_lost"); co_await std::move(task); }
ASYNC_TEST("promotion differential: auth_pool_drift") { auto task = replay("auth_pool_drift"); co_await std::move(task); }
ASYNC_TEST("promotion differential: snapshot_unavailable") { auto task = replay("snapshot_unavailable"); co_await std::move(task); }
ASYNC_TEST("promotion differential: catalog_drift") { auto task = replay("catalog_drift"); co_await std::move(task); }
ASYNC_TEST("promotion differential: model_drift") { auto task = replay("model_drift"); co_await std::move(task); }
ASYNC_TEST("promotion differential: capability_changed") { auto task = replay("capability_changed"); co_await std::move(task); }
ASYNC_TEST("promotion differential: runtime_unavailable") { auto task = replay("runtime_unavailable"); co_await std::move(task); }
ASYNC_TEST("promotion differential: runtime_drift") { auto task = replay("runtime_drift"); co_await std::move(task); }
ASYNC_TEST("promotion differential: cas_conflict") { auto task = replay("cas_conflict"); co_await std::move(task); }
ASYNC_TEST("promotion differential: cas_retry") { auto task = replay("cas_retry"); co_await std::move(task); }
