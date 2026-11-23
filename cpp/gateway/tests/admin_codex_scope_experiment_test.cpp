// Links: tests/admin_codex_scope_experiment_test.cpp
// src/admin/codex.cpp src/http.cpp src/provider/presentation.cpp src/request.cpp
// src/cache/prompt_analytics_read.cpp src/cache/scope_experiment_targets.cpp tests/helpers/prompt_cache_scope_experiment_harness.cpp tests/helpers/codex_diff_kv.cpp
// src/cache/prompt_analytics_core.cpp src/debug_routing.cpp src/cache/scope_experiment_model.cpp src/deepseek/waterfall.cpp
// src/codex/auth.cpp src/config.cpp src/provider/selection.cpp src/codex/account_routing.cpp
// src/codex/routing_state.cpp src/models/catalog.cpp src/auth/relay.cpp src/models/metadata.cpp
// src/authority/native_codex_mutations.cpp src/input_normalization.cpp src/runtime_config.cpp src/codex/experiment.cpp
// src/codex/dispatch.cpp src/types.cpp src/authority/native_codex.cpp src/provider/cerebras.cpp
// src/authority/native_codex_capacity.cpp src/catalog/types.cpp src/utils.cpp src/request_policy.cpp
// src/provider/capacity_events.cpp src/codex/routing_evaluation.cpp src/models/codex_models.cpp src/authority/native_codex_contracts.cpp
// src/provider/lithos_models.cpp src/provider/metered.cpp src/provider/claude_code_models.cpp src/provider/surplus.cpp
// src/cache/telemetry_gate.cpp src/recent_model_context.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp
// src/authority/native_codex_evaluation.cpp src/codex/external_auth.cpp src/catalog/store.cpp src/provider/surplus_claude_request.cpp
// src/codex/capacity_routing.cpp src/kv.cpp src/deepseek/index.cpp src/catalog/index.cpp
// src/models/openrouter_models.cpp src/bounded_response_body.cpp src/provider/lithos.cpp src/cache/scope_targets.cpp
// src/provider/claude_code.cpp src/codex/overage_settings.cpp src/provider/claude_code_accounts.cpp src/provider/capacity_parse.cpp
// src/catalog/models.cpp src/responses_stream.cpp src/models/codex_models_whitelist.cpp tests/helpers/admin_auth_harness.cpp
// src/defaults.cpp src/provider/cerebras_rate_limits.cpp src/provider/anthropic_replay.cpp src/provider/claude_code_credentials.cpp
// src/codex/auth_refresh.cpp src/provider/openrouter.cpp src/cache/scope_experiment.cpp src/models/codex_models_fetch.cpp
// src/provider/capacity.cpp src/provider/capacity_rollups.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code_quota.cpp
// src/provider/capacity_history.cpp src/provider/lithos_rate_limits.cpp src/admin/api_keys.cpp src/models/codex_models_availability.cpp
// src/codex/banked_reset.cpp src/openai_telemetry.cpp src/api_key_policy.cpp src/provider/health.cpp
// src/provider/capacity_contract.cpp src/api_keys.cpp src/provider/claude_code_usage_schema.cpp src/authority/native_api_key_policy.cpp
// src/authority/native_api_key.cpp src/provider/surplus_claude_chat_request.cpp src/admin/api_key_audit.cpp src/codex/native_auth.cpp
// src/provider/claude_capacity_history.cpp src/codex/reset_settings.cpp src/paid_fallback/ledger_state.cpp src/authority/native_api_key_audit.cpp
// src/authority/native_paid_contracts.cpp src/provider/anthropic_chat_responses.cpp src/codex/banked_reset_pool.cpp src/provider/capacity_analytics.cpp
// src/metered_quota.cpp src/codex/rate_limit_429.cpp src/paid_fallback/ledger_stats.cpp src/paid_fallback/index.cpp
// src/codex/banked_reset_claim.cpp src/storage/kv.cpp src/paid_fallback/ledger_admission.cpp src/authority/native_paid.cpp
// src/provider/anthropic_responses.cpp src/authority/native_paid_policy.cpp src/paid_fallback/ledger_backfill.cpp src/provider/anthropic_response_wire.cpp
// src/responses_failover_stream.cpp src/catalog/promotion.cpp src/storage/sqlite_readonly.cpp src/codex/banked_reset_submission.cpp
// src/paid_fallback/ledger_settlement.cpp src/authority/native_codex_429.cpp src/provider/removed_provider_telemetry.cpp
// src/codex/banked_reset_provider.cpp src/codex/native_auth_transport.cpp src/provider/anthropic_chat_stream.cpp src/storage/native_kv.cpp
// src/codex/responses_error.cpp src/runtime/websocket.cpp
// Fixture-bound release metadata matches the isolated Bun fixture; this does not attest a release
// artifact or deployment. Omit src/release.cpp for this binary; all behavior/functions remain real.
#include "uos/admin/codex.hpp"
#include "uos/release.hpp"
#include "tests/helpers/prompt_cache_scope_experiment_harness.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/gzip.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/test_async.hpp"
namespace uos::release { const std::string_view RELEASE_GIT_SHA = "0123456789abcdef0123456789abcdef01234567"; }
namespace rt = uos::rt;
namespace json = uos::json;
namespace a = uos::admin::codex;
namespace h = uos::tests::helpers::prompt_cache_scope_experiment_harness;
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  const char* encoded =
#include "tests/admin_codex_scope_experiment_vectors.inc"
    ;
  const json::object& vectors() { static const auto value = rt::jsonParse(rt::decodeUtf8(rt::gzip::gunzipSync(rt::fromBase64(encoded)))); return value.as_object(); }
  rt::Request request() { rt::RequestInit init; init.method = "POST"; return rt::Request("https://ai.ubq.fi/admin/providers/codex/cache-scope-experiment", init); }
  rt::Task<void> observe(std::string name, rt::Response response, std::size_t inference, std::size_t refreshes) {
    const auto body = co_await response.json(); const auto* error = rt::getObject(body, "error");
    const auto* status = rt::get(body, "status"), *cycles = rt::get(body, "completed_cycles"); const auto* code = error ? rt::get(*error, "code") : nullptr;
    const json::object actual{{"name", name}, {"http_status", response.status()}, {"status", status ? *status : json::value(nullptr)}, {"completed_cycles", cycles ? *cycles : json::value(nullptr)}, {"code", code ? *code : json::value(nullptr)}, {"inference", inference}, {"refreshes", refreshes}};
    const auto* observations = rt::getArray(vectors(), "observations"); ASSERT(observations); const json::value* expected = nullptr;
    for (const auto& row : *observations) if (rt::getString(row, "name") == name) expected = &row;
    ASSERT(expected); EXPECT(rt::deepEqual(actual, *expected));
  }
  rt::Task<void> seeded() { h::seed(); auto task = h::seedStage0Baseline(std::string(h::MODEL)); co_await std::move(task); }
}
ASYNC_TEST("scope HTTP trigger requires current-release Stage 0 before outbound work") {
  h::HarnessScope fixture; h::seed(); h::ScopeTransport transport; const rt::GlobalFetchOverride fetch(transport.fetcher()); AWAIT(response, a::handleAdminCodexCacheScopeExperiment(request())); auto task = observe("not_ready-0", response, transport.requests.size(), transport.refreshes); co_await std::move(task);
}
ASYNC_TEST("scope HTTP trigger reports 202 for two cycles and 200 for completed scope") {
  h::HarnessScope fixture; co_await seeded(); h::ScopeTransport transport; transport.onInference = [](std::size_t index, const auto&, const auto&) { const auto step = static_cast<double>(index % 10); return h::sseCompleted(h::cycleValue(h::CACHE_READ_CYCLE, step), h::cycleValue(h::CACHE_WRITE_CYCLE_LEADING, step)); }; const rt::GlobalFetchOverride fetch(transport.fetcher());
  for (int i = 0; i < 3; ++i) { AWAIT(response, a::handleAdminCodexCacheScopeExperiment(request())); const auto name = "cycles-" + std::to_string(i); auto task = observe(name, response, transport.requests.size(), transport.refreshes); co_await std::move(task); }
  EXPECT(rt::includes(rt::jsonStringify(h::kv->row(h::CODEX_MODELS_KV_KEY)), "\"scope\""));
}
ASYNC_TEST("scope HTTP trigger maps a durable state failure to 503") {
  h::HarnessScope fixture; co_await seeded(); h::ScopeTransport transport; transport.onInference = [](std::size_t index, const auto&, const auto&) { if (index == 0) { auto state = h::kv->row(h::stateKey); state.as_object()["campaign_owner"] = "other"; h::kv->put(h::stateKey, rt::KvValue(state)); } return h::sseCompleted(0, 0); }; const rt::GlobalFetchOverride fetch(transport.fetcher()); AWAIT(response, a::handleAdminCodexCacheScopeExperiment(request())); auto task = observe("failed-0", response, transport.requests.size(), transport.refreshes); co_await std::move(task);
}
ASYNC_TEST("scope HTTP trigger maps malformed durable state to 503 before dispatch") {
  h::HarnessScope fixture; co_await seeded(); AWAIT(baseline, h::scopeBaselineFor()); const json::object state{{"v", 3}, {"target", json::value_from(baseline.target)}, {"next_cycle", 2}}; h::kv->put(h::stateKey, rt::KvValue(state)); h::ScopeTransport transport; const rt::GlobalFetchOverride fetch(transport.fetcher()); AWAIT(response, a::handleAdminCodexCacheScopeExperiment(request())); auto task = observe("malformed-0", response, transport.requests.size(), transport.refreshes); co_await std::move(task);
}
ASYNC_TEST("scope HTTP trigger suppresses unknown failure details in its body and logs") {
  h::HarnessScope fixture; co_await seeded(); const std::string secret = "private-scope-detail"; h::kv->beforeGet = [secret](auto) -> rt::Task<void> { throw rt::JsError(secret); co_return; }; h::ScopeTransport transport; const rt::GlobalFetchOverride fetch(transport.fetcher()); rt::console::Capture capture({rt::console::Method::error}); AWAIT(response, a::handleAdminCodexCacheScopeExperiment(request())); const auto clone = response.clone(); AWAIT(body, clone.text()); EXPECT(!rt::includes(body, secret)); for (const auto& call : capture.calls()) EXPECT(!rt::includes(call.text, secret)); auto task = observe("generic-0", response, transport.requests.size(), transport.refreshes); co_await std::move(task);
}
ASYNC_TEST("scope HTTP trigger maps concurrent campaign ownership to 409 and settles its first call") {
  h::HarnessScope fixture; co_await seeded(); auto [started, markStarted, rejectStarted] = rt::withResolvers<void>(); static_cast<void>(rejectStarted); auto [pendingResponse, releaseFirst, rejectFirst] = rt::withResolvers<rt::Response>(); static_cast<void>(rejectFirst); std::size_t calls = 0;
  rt::Fetch fetcher([&](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> { ++calls; markStarted(); auto response = co_await pendingResponse.wait(); co_return response; }); const rt::GlobalFetchOverride fetch(fetcher); rt::Promise<rt::Response> first(a::handleAdminCodexCacheScopeExperiment(request())); co_await started.wait();
  AWAIT(second, a::handleAdminCodexCacheScopeExperiment(request())); auto secondObservation = observe("busy", second, calls, 0); co_await std::move(secondObservation); releaseFirst(h::sseCompleted(0, 0)); const auto response = co_await first.wait(); auto task = observe("busy-first", response, calls, 0); co_await std::move(task);
}
