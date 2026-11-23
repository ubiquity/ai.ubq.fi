// Links: tests/admin_defaults_differential_test.cpp
// src/admin/defaults.cpp src/codex/auth.cpp src/types.cpp src/cache/kv_migration_validation.cpp
// src/cache/kv_migration_usage.cpp src/models/codex_models_fetch.cpp src/authority/native_kernel_contracts.cpp tests/helpers/admin_auth_harness.cpp
// src/authority/native_api_key.cpp src/codex/auth_refresh.cpp src/cache/kv_migration_paid_fallback.cpp src/runtime_config.cpp
// src/admin/api_keys.cpp src/paid_fallback/ledger_state.cpp src/codex/banked_reset.cpp src/api_keys.cpp
// src/http.cpp src/metered_quota.cpp src/admin/codex.cpp src/utils.cpp
// src/models/codex_models.cpp src/codex/dispatch.cpp src/admin/api_key_audit.cpp src/codex/routing_mutations.cpp
// src/cache/prompt_analytics_read.cpp src/authority/native_api_key_policy.cpp src/config.cpp src/authority/native_kernel.cpp
// src/kv.cpp src/provider/openrouter.cpp src/cache/scope_experiment.cpp src/authority/native_api_key_audit.cpp
// src/kernel/quota_v2.cpp src/authority/native_paid_contracts.cpp src/cache/scope_experiment_model.cpp src/cache/prompt_analytics_core.cpp
// src/kernel/quota_reservations.cpp src/codex/account_routing.cpp src/codex/routing_serial.cpp src/authority/native_codex_contracts.cpp
// src/authority/native_codex.cpp src/debug_routing.cpp src/codex/capacity_routing.cpp src/codex/experiment.cpp
// src/authority/native_codex_capacity.cpp src/defaults.cpp src/authority/native_paid.cpp src/codex/manual_reset_state.cpp
// src/models/codex_models_availability.cpp src/cache/scope_experiment_targets.cpp src/models/openrouter_models.cpp src/codex/banked_reset_provider.cpp
// src/authority/native_paid_policy.cpp src/authority/native_codex_mutations.cpp src/cache/kv_migration_base.cpp src/api_key_policy.cpp
// src/catalog/types.cpp src/paid_fallback/ledger_backfill.cpp src/codex/routing_evaluation.cpp src/models/catalog.cpp
// src/provider/capacity_events.cpp src/provider/metered.cpp src/cache/telemetry_gate.cpp src/catalog/promotion.cpp
// src/input_normalization.cpp src/kv_conversion/profile_json.cpp src/catalog/index.cpp src/request_policy.cpp
// src/auth/relay.cpp src/deepseek/waterfall.cpp src/models/codex_models_whitelist.cpp src/models/metadata.cpp
// src/catalog/store.cpp src/codex/rate_limit_429.cpp src/authority/native_codex_evaluation.cpp src/catalog/models.cpp
// src/provider/selection.cpp src/codex/routing_state.cpp src/recent_model_context.cpp src/codex/native_auth.cpp
// src/provider/surplus.cpp src/paid_fallback/index.cpp src/bounded_response_body.cpp src/paid_fallback/ledger_settlement.cpp
// src/provider/surplus_claude_request.cpp src/deepseek/index.cpp src/provider/cerebras.cpp src/provider/claude_code_models.cpp
// src/provider/anthropic_responses.cpp src/codex/external_auth.cpp src/responses_stream.cpp src/codex/banked_reset_pool.cpp
// src/codex/banked_reset_submission.cpp src/openai_telemetry.cpp src/authority/native_codex_429.cpp src/codex/banked_reset_claim.cpp
// src/request.cpp src/storage/kv.cpp src/provider/claude_code.cpp src/provider/claude_code_accounts.cpp
// src/codex/reset_settings.cpp src/provider/anthropic_replay.cpp src/provider/health.cpp src/provider/claude_code_credentials.cpp
// src/provider/cerebras_rate_limits.cpp src/provider/presentation.cpp src/paid_fallback/ledger_admission.cpp src/codex/overage_settings.cpp
// src/responses_failover_stream.cpp src/provider/capacity_parse.cpp src/codex/native_auth_transport.cpp src/provider/lithos_models.cpp
// src/provider/anthropic_chat_responses.cpp src/provider/surplus_claude_chat_request.cpp src/release.cpp src/provider/anthropic_response_wire.cpp
// src/provider/capacity_contract.cpp src/runtime/websocket.cpp src/provider/claude_code_quota.cpp src/cache/scope_targets.cpp
// src/paid_fallback/ledger_stats.cpp src/storage/native_kv.cpp src/provider/removed_provider_telemetry.cpp
// src/provider/anthropic_chat_stream.cpp src/storage/sqlite_readonly.cpp src/codex/responses_error.cpp src/provider/capacity.cpp
// src/provider/claude_capacity_history.cpp src/provider/capacity_rollups.cpp src/provider/lithos.cpp src/provider/capacity_history.cpp
// src/provider/claude_capacity_rollups.cpp src/provider/capacity_analytics.cpp src/provider/claude_code_usage_schema.cpp src/provider/lithos_rate_limits.cpp
//21 credential-free Bun-recorded scenarios for src/admin/defaults.ts. Byte and row expectations
// bind Date.now to1000000; all operations use the source admin-auth memory fixture, never disk KV.
#include "uos/admin/defaults.hpp"
#include "uos/codex/auth.hpp"
#include "uos/config.hpp"
#include "uos/defaults.hpp"
#include "uos/kv.hpp"
#include "uos/runtime_config.hpp"
#include "uos/metered_quota.hpp"
#include "tests/helpers/admin_auth_harness.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/gzip.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/test_async.hpp"
#include <array>
namespace rt = uos::rt;
namespace json = uos::json;
namespace a = uos::admin::defaults;
namespace h = uos::tests::helpers::admin_auth_harness;
namespace {
  const char* vectorsEncoded =
#include "tests/admin_defaults_differential_vectors.inc"
    ;
  const json::array& vectors() { static const auto value = rt::jsonParse(rt::decodeUtf8(rt::gzip::gunzipSync(rt::fromBase64(vectorsEncoded)))); return value.as_array(); }
  struct Fixture {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c4-defaults"}}};
    std::string saved = uos::config::config.codexAuthJsonB64;
    Fixture() { uos::config::config.codexAuthJsonB64.clear(); h::resetHarness(); uos::codex::auth::resetCodexAuthCacheForTest(); uos::runtime_config::resetRuntimeConfigCacheForTest(); }
    ~Fixture() { h::cleanupHarness(); uos::codex::auth::resetCodexAuthCacheForTest(); uos::runtime_config::resetRuntimeConfigCacheForTest(); uos::config::config.codexAuthJsonB64 = saved; }
  };
  // The source fake omits versionstamp. Its undefined revision satisfies !== null; an
  // empty string retains that truth value at the C++ nullable revision boundary.
  class FixtureRevisionKv : public rt::ForwardingKv {
  public:
    explicit FixtureRevisionKv(rt::Kv& store) : rt::ForwardingKv(store) {}
    using rt::Kv::get;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override {
      auto task = inner.get(key, options); auto entry = co_await std::move(task);
      if (!entry.value.isNullish() && !entry.versionstamp) entry.versionstamp = "";
      co_return entry;
    }
  };
  class RejectingDefaultsStore final : public rt::ForwardingKv {
  public:
    explicit RejectingDefaultsStore(rt::Kv& store) : rt::ForwardingKv(store) {}
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options) override {
      if (key == uos::defaults::DEFAULT_KERNEL_POLICY_LIMIT_KEY) throw rt::JsError("controlled defaults read failure");
      auto pending = inner.get(std::move(key), options);
      auto result = co_await std::move(pending);
      co_return result;
    }
  };
  void seedRuntime() {
    const json::object snapshot{{"source", "fixture"}, {"updated_at_ms", 1000000}, {"models", json::array{
      json::object{{"slug", "a"}, {"supported_reasoning_levels", json::array{"low", "high"}}, {"default_reasoning_level", "low"}},
      json::object{{"slug", "b"}, {"supported_reasoning_levels", json::array{"custom", "high"}}, {"default_reasoning_level", "custom"}},
    }}};
    h::kvStore.set(h::keyToString(uos::codex::auth::CODEX_MODELS_KV_KEY), rt::KvValue(snapshot));
    const auto runtime = uos::runtime_config::buildRuntimeConfig(snapshot);
    h::kvStore.set(h::keyToString(uos::runtime_config::RUNTIME_CONFIG_V2_KEY), rt::KvValue(uos::runtime_config::toJson(runtime)));
    uos::runtime_config::cacheRuntimeConfig(runtime);
  }
  rt::Task<void> runCase(std::size_t index) {
    Fixture fixture; rt::globalDateNow() = [] { return 1000000.0; }; const auto& row = vectors()[index]; const auto& scenario = row.at("scenario");
    if (rt::getString(scenario, "name") == "set_window") uos::kv::setKvForTest(std::make_shared<FixtureRevisionKv>(*h::kvStub()));
    if (rt::getBool(scenario, "seed").value_or(false)) seedRuntime();
    if (rt::getBool(scenario, "deny").value_or(false)) uos::kv::setKvForTest(nullptr);
    const auto method = std::string(rt::getString(scenario, "method").value_or("GET")); const bool migration = rt::getBool(scenario, "migration").value_or(false); const auto query = std::string(rt::getString(scenario, "query").value_or(""));
    rt::RequestInit init; init.method = method;
    if (method == "POST") { const auto* body = rt::get(scenario, "body"); init.body = body && body->is_string() ? std::string(body->as_string()) : rt::jsonStringify(body ? *body : json::value(nullptr)); }
    if (const auto length = rt::getString(scenario, "length")) init.headers = rt::Headers{{"content-length", std::string(*length)}};
    rt::Request request("https://ai.ubq.fi/admin/" + std::string(migration ? "kv/migration/import" : "defaults") + query, init); auto task = migration ? a::handleAdminKvMigrationImport(request) : a::handleAdminDefaults(request); auto response = co_await std::move(task);
    EXPECT_EQ(response.status(), rt::getNumber(row, "status").value()); const auto body = co_await response.json(); EXPECT(rt::deepEqual(body, row.at("body")));
    json::array rows; for (const auto& [key, value] : h::kvStore) rows.push_back(json::array{key, value.toJson()}); EXPECT(rt::deepEqual(rows, row.at("rows")));
  }
}
ASYNC_TEST("admin defaults reject an oversized model like an unknown model") {
  Fixture fixture;
  seedRuntime();
  const auto runtimeKey = h::keyToString(uos::runtime_config::RUNTIME_CONFIG_V2_KEY);
  const auto before = h::kvStore.get(runtimeKey)->toJson();
  for (std::size_t size : std::array{rt::limits::kModelIdCodeUnits + 1, rt::limits::kRegexSubjectBytes + 1}) {
    rt::RequestInit init;
    init.method = "POST";
    init.body = rt::jsonStringify(json::object{{"model", std::string(size, 'x')}});
    const rt::Request req("http://localhost/admin/defaults", init);
    auto task = a::handleAdminDefaults(req);
    const auto response = co_await std::move(task);
    EXPECT_EQ(response.status(), 400);
    const auto body = co_await response.json();
    EXPECT(rt::deepEqual(body, vectors()[12].at("body")));
  }
  EXPECT(rt::deepEqual(h::kvStore.get(runtimeKey)->toJson(), before));
}
ASYNC_TEST("defaults diagnostics survive a failed KV read") {
  Fixture fixture;
  const auto store = std::make_shared<RejectingDefaultsStore>(*h::kvStub());
  uos::kv::setKvForTest(store);
  const auto resume = rt::withResolvers<void>();
  const auto done = rt::withResolvers<void>();
  auto capture = std::make_shared<std::string>("defaults diagnostics callback");
  const std::weak_ptr observer = capture;
  a::AdminDefaultsDependencies options;
  options.getMeteredQuotaDiagnostics = [capture = std::move(capture), waiting = resume.promise, finish = done.resolve]()
    -> rt::Task<uos::metered_quota::MeteredQuotaDiagnostics> {
    co_await waiting.wait();
    EXPECT_EQ(*capture, "defaults diagnostics callback");
    finish();
    co_return uos::metered_quota::MeteredQuotaDiagnostics{};
  };
  const rt::Request request("https://ai.ubq.fi/admin/defaults");
  auto pending = a::handleAdminDefaults(request, std::move(options));
  EXPECT_REJECTS_MESSAGE(std::move(pending), rt::JsError, "controlled defaults read failure");
  ASSERT(!observer.expired());
  resume.resolve();
  co_await done.promise.wait();
  EXPECT(observer.expired());
}
ASYNC_TEST("admin defaults Bun differential: empty_get") { auto task = runCase(0); co_await std::move(task); }
ASYNC_TEST("admin defaults Bun differential: unsupported") { auto task = runCase(1); co_await std::move(task); }
ASYNC_TEST("admin defaults Bun differential: empty_post") { auto task = runCase(2); co_await std::move(task); }
ASYNC_TEST("admin defaults Bun differential: invalid_json") { auto task = runCase(3); co_await std::move(task); }
ASYNC_TEST("admin defaults Bun differential: unknown_field") { auto task = runCase(4); co_await std::move(task); }
ASYNC_TEST("admin defaults Bun differential: bad_limit") { auto task = runCase(5); co_await std::move(task); }
ASYNC_TEST("admin defaults Bun differential: bad_window") { auto task = runCase(6); co_await std::move(task); }
ASYNC_TEST("admin defaults Bun differential: set_limit") { auto task = runCase(7); co_await std::move(task); }
ASYNC_TEST("admin defaults Bun differential: set_window") { auto task = runCase(8); co_await std::move(task); }
ASYNC_TEST("admin defaults Bun differential: model_without_runtime") { auto task = runCase(9); co_await std::move(task); }
ASYNC_TEST("admin defaults Bun differential: model_update") { auto task = runCase(10); co_await std::move(task); }
ASYNC_TEST("admin defaults Bun differential: reasoning_update") { auto task = runCase(11); co_await std::move(task); }
ASYNC_TEST("admin defaults Bun differential: unknown_model") { auto task = runCase(12); co_await std::move(task); }
ASYNC_TEST("admin defaults Bun differential: atomic_validation") { auto task = runCase(13); co_await std::move(task); }
ASYNC_TEST("admin defaults Bun differential: migration_default") { auto task = runCase(14); co_await std::move(task); }
ASYNC_TEST("admin defaults Bun differential: migration_blank_profile") { auto task = runCase(15); co_await std::move(task); }
ASYNC_TEST("admin defaults Bun differential: migration_invalid_profile") { auto task = runCase(16); co_await std::move(task); }
ASYNC_TEST("admin defaults Bun differential: migration_conflicting_write") { auto task = runCase(17); co_await std::move(task); }
ASYNC_TEST("admin defaults Bun differential: migration_invalid_line") { auto task = runCase(18); co_await std::move(task); }
ASYNC_TEST("admin defaults Bun differential: migration_large_length") { auto task = runCase(19); co_await std::move(task); }
ASYNC_TEST("admin defaults Bun differential: no_kv") { auto task = runCase(20); co_await std::move(task); }
