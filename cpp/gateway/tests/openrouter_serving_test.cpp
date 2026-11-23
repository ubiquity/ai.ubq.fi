// Links: tests/openrouter_serving_test.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
// src/openai_schema.cpp src/agent_messages.cpp src/request.cpp src/auth/index.cpp src/kernel/quota_reservations.cpp src/authority/native_kernel_contracts.cpp src/openai.cpp src/utils.cpp
// src/authority/native_kernel.cpp src/provider/metered.cpp src/api_key_policy.cpp src/models/codex_models.cpp src/authority/native_api_key.cpp src/kv.cpp src/auth/passport.cpp src/inference_deadline.cpp
// src/provider/surplus_claude_request.cpp src/kernel/quota_v2.cpp src/kernel/attestation.cpp src/api_keys.cpp src/authority/native_api_key_policy.cpp src/models/catalog.cpp src/config.cpp src/http.cpp
// src/storage/kv.cpp src/defaults.cpp src/kernel/policy_queue.cpp src/input_normalization.cpp src/provider/lithos.cpp src/request_policy.cpp src/provider/claude_code_models.cpp src/storage/native_kv.cpp
// src/models/metadata.cpp src/models/codex_models_fetch.cpp src/provider/surplus.cpp src/responses_request.cpp src/handler/index.cpp src/bounded_response_body.cpp src/types.cpp src/provider/cerebras.cpp
// src/auth/passkeys.cpp src/admin/kernel.cpp src/codex/auth_refresh.cpp src/admin/api_keys.cpp src/admin/api_key_audit.cpp src/models/openrouter_models.cpp src/codex/dispatch.cpp
// src/admin/codex.cpp src/auth/local_admin.cpp src/handler/terminal_route.cpp src/admin/claude_code.cpp src/release.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_backfill.cpp src/provider/selection.cpp
// src/storage/sqlite_readonly.cpp src/provider/capacity_parse.cpp src/codex/reset_settings.cpp src/kernel/usage.cpp src/codex/auth.cpp src/codex/banked_reset_pool.cpp src/authority/native_paid_contracts.cpp src/handler/admission.cpp
// src/admin/api_key_mutations.cpp src/codex/banked_reset_claim.cpp src/paid_fallback/ledger_stats.cpp src/provider/claude_code_credentials.cpp src/codex/supervisor_brief.cpp src/paid_fallback/ledger_settlement.cpp src/provider/claude_code.cpp src/codex/banked_reset.cpp
// src/authority/native_paid.cpp src/provider/capacity_events.cpp src/codex/overage_settings.cpp src/codex/banked_reset_provider.cpp src/provider/capacity_contract.cpp src/sentinel/replay_observation.cpp src/health.cpp src/kernel/quota_write.cpp
// src/provider/claude_code_quota.cpp src/provider/capacity.cpp src/auth/relay.cpp src/openai_telemetry.cpp src/inference_admission.cpp src/paid_fallback/health.cpp src/provider/capacity_history.cpp src/provider/lithos_models.cpp
// src/responses_failover_stream.cpp src/metered_quota.cpp src/handler/terminal_log.cpp src/provider/claude_code_accounts.cpp src/systemone/handlers.cpp src/deepseek/flash_aliases.cpp src/admin/codex_reset.cpp src/codex/routing_mutations.cpp
// src/cache/prompt_analytics_read.cpp src/codex/capacity_routing.cpp src/static.cpp src/handler/http.cpp src/decisions/handlers.cpp src/responses_native_headers.cpp src/cache/prompt_analytics_queue.cpp src/catalog/store.cpp
// src/provider/openrouter.cpp src/admin/error_log.cpp src/sentinel/replay_store.cpp src/runtime_config.cpp src/authority/native_codex_capacity.cpp src/sentinel/replay_read.cpp src/sentinel/replay_envelope.cpp src/provider/surplus_claude_chat_request.cpp
// src/provider/claude_code_access.cpp src/provider/capacity_rollups.cpp src/admin/defaults.cpp src/cache/scope_experiment_targets.cpp src/authority/native_codex_evaluation.cpp src/provider/health.cpp src/codex/external_auth.cpp src/sentinel/replay_retention.cpp
// src/models/codex_models_availability.cpp src/authority/native_codex_mutations.cpp src/sentinel/replay_retention_bootstrap.cpp src/sentinel/replay_model.cpp src/sentinel/incident_outbox.cpp src/codex/supervisor_config.cpp src/quota_projection.cpp src/cache/kv_migration_validation.cpp
// src/models/codex_models_whitelist.cpp src/cache/prompt_analytics_core.cpp src/provider/anthropic_replay.cpp src/embeddings/jobs.cpp src/auth/passport_sessions.cpp src/paid_fallback/index.cpp src/codex/supervisor.cpp src/sentinel/replay_retention_metadata.cpp
// src/authority/native_paid_policy.cpp src/embeddings/handlers.cpp src/authority/native_codex_contracts.cpp src/sentinel/upstream_capture.cpp src/codex/banked_reset_submission.cpp src/cache/kv_migration_usage.cpp src/catalog/types.cpp src/paid_fallback/rollups.cpp
// src/sentinel/replay_retention_schema.cpp src/cache/kv_migration_paid_fallback.cpp src/provider/cerebras_rate_limits.cpp src/embeddings/ledger.cpp src/cache/scope_experiment_model.cpp src/optional_telemetry_queue.cpp src/live/handler.cpp src/analytics.cpp
// src/provider/anthropic_chat_responses.cpp src/codex/experiment.cpp src/codex/supervisor_secret.cpp src/authority/native_api_key_audit.cpp src/upstream_wire.cpp src/catalog/index.cpp src/codex/account_routing.cpp src/codex/routing_state.cpp
// src/embeddings/voyage.cpp src/sentinel/replay_admin.cpp src/codex/manual_reset_state.cpp src/provider/claude_capacity_history.cpp src/admin/sentinel_incident.cpp src/runtime/http_tasks.cpp src/auth/local_development_key.cpp src/provider/anthropic_response_wire.cpp
// src/provider/claude_capacity_rollups.cpp src/cache/kv_migration_base.cpp src/debug_routing.cpp src/responses_stream.cpp src/codex/native_auth.cpp src/codex/manual_reset.cpp src/catalog/models.cpp src/provider/claude_code_usage_schema.cpp
// src/deepseek/index.cpp src/deepseek/waterfall.cpp src/kernel/quota_reserve.cpp src/codex/routing_evaluation.cpp src/codex/rate_limit_429.cpp src/codex/supervisor_follow.cpp src/codex/routing_serial.cpp src/provider/removed_provider_telemetry.cpp
// src/provider/presentation.cpp src/live/sideband.cpp src/provider/capacity_analytics.cpp src/paid_fallback/ledger_admission.cpp src/kv_conversion/profile_json.cpp src/sentinel/replay_limits.cpp src/images.cpp src/live/routes.cpp
// src/recent_model_context.cpp src/responses_handler.cpp src/authority/native_codex.cpp src/codex/supervisor_inventory.cpp src/provider/lithos_rate_limits.cpp src/paid_fallback/removed_provider.cpp src/provider/cerebras_responses.cpp src/cache/scope_targets.cpp
// src/responses_attempts.cpp src/cache/scope_experiment.cpp src/admin/capacity_observation_sync.cpp src/provider/anthropic_chat_stream.cpp src/provider/anthropic_responses.cpp src/provider/openrouter_request_controls.cpp src/responses_buffered.cpp src/live/upstream.cpp
// src/cache/telemetry_gate.cpp src/deepseek/context_admission_failure.cpp src/admin/chart_source_proxy.cpp src/runtime/http_early_sse.cpp src/codex/supervisor_transport.cpp src/provider/removed_provider_circuit.cpp src/codex/responses_error.cpp src/codex/native_auth_transport.cpp
// src/provider/cerebras_handlers.cpp src/provider/openrouter_handlers.cpp src/authority/native_codex_429.cpp src/codex/supervisor_log.cpp src/deepseek/waterfall_handler.cpp src/chat/envelope.cpp src/provider/lithos_handlers.cpp src/catalog/promotion.cpp
// src/provider/lithos_streams.cpp src/chat/stream_translation.cpp src/codex/quota.cpp src/runtime/websocket.cpp src/chat/completions.cpp src/deepseek/chat_projection.cpp src/paid_fallback/routing.cpp src/provider/stream_relay.cpp
// src/live/calls.cpp src/deepseek/responses.cpp src/deepseek/forwarded_payload_policy.cpp src/provider/openrouter_streams.cpp src/deepseek/handlers.cpp src/paid_fallback/claude_code_route.cpp src/deepseek/responses_stream.cpp src/lib/jev_compaction/state.cpp
// src/deepseek/responses_payload.cpp src/provider/openrouter_web_tools.cpp src/deepseek/stream.cpp src/lib/jev_compaction/codex_items.cpp src/lib/jev_compaction/types.cpp src/codex/index.cpp src/lib/jev_compaction/compact.cpp src/codex/responses_operations.cpp
// src/lib/jev_compaction/request.cpp src/codex/responses_runners.cpp src/codex/responses_state.cpp tests/helpers/counting_kv.cpp
// Port of all 23 source registrations plus six preserved additional direct/stream traces.
#include "uos/provider/openrouter.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/string.hpp"
#include "uos/sentinel/upstream_capture.hpp"
#include "uos/provider/openrouter_streams.hpp"
#include "uos/provider/health.hpp"
#include "uos/models/catalog.hpp"
#include "uos/catalog/models.hpp"
#include "uos/models/codex_models_whitelist.hpp"
#include "uos/provider/selection.hpp"
#include "uos/runtime_config.hpp"
#include "uos/codex/index.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/provider/openrouter_handlers.hpp"
#include "uos/handler/terminal_route.hpp"
#include "uos/api_keys.hpp"
#include "uos/utils.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/rt/crypto.hpp"
#include "tests/helpers/counting_kv.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace upstream = uos::provider::openrouter;
  namespace models = uos::models::openrouter_models;
  struct Scope {
    rt::env::ScopedEnvironment env{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w05c2"}, {"OPENROUTER_API_KEY", "fixture-openrouter-key"}}};
    uos::config::Config saved = uos::config::config;
    Scope() { uos::config::config.codexAuthJsonB64.clear(); uos::kv::setKvForTest(nullptr); models::resetOpenRouterModelsCacheForTest(); }
    ~Scope() { models::setOpenRouterModelsFetchForTest({}); models::resetOpenRouterModelsCacheForTest(); uos::kv::setKvForTest(nullptr); uos::config::config = saved; }
  };
  rt::Response catalogue() { return rt::Response::json(json::object{{"data", json::array{json::object{{"id", "vendor/alpha"}}, json::object{{"id", "vendor/beta"}}}}}); }
  rt::Task<void> warm() {
    models::setOpenRouterModelsFetchForTest(rt::Fetch([](rt::RequestInfo, rt::RequestInit) { return catalogue(); }));
    const models::FetchOpenRouterModelsOptions options;
    auto fetching = models::fetchOpenRouterModels(options); const auto result = co_await std::move(fetching); EXPECT(result);
  }
  rt::Task<rt::Response> textRequest(bool chat, const json::object& body, const upstream::OpenRouterDispatchInit& init) {
    return chat ? upstream::fetchOpenRouterChatCompletions(body, init) : upstream::fetchOpenRouterResponses(body, init);
  }
}

ASYNC_TEST("openrouter serves every cached catalogue id and refuses the rest") {
  Scope isolation; co_await warm();
  EXPECT_EQ(upstream::openRouterUpstreamModelFor("vendor/alpha"), std::optional<std::string>("vendor/alpha"));
  EXPECT_EQ(upstream::openRouterUpstreamModelFor("vendor/beta"), std::optional<std::string>("vendor/beta"));
  EXPECT(!upstream::openRouterUpstreamModelFor("vendor/gamma"));
  EXPECT_EQ(upstream::openRouterUpstreamModelFor("  vendor/alpha  "), std::optional<std::string>("vendor/alpha"));
  EXPECT(!upstream::openRouterUpstreamModelFor("   "));
}

ASYNC_TEST("openrouter resolves a served id from a cold catalogue and stays cache-authoritative once warm") {
  Scope isolation; int fetches = 0;
  models::setOpenRouterModelsFetchForTest(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { ++fetches; return catalogue(); }));
  auto codex = upstream::resolveOpenRouterUpstreamModel("gpt-5-codex"); const auto codexMissing = co_await std::move(codex); EXPECT(!codexMissing); EXPECT_EQ(fetches, 0);
  auto first = upstream::resolveOpenRouterUpstreamModel("vendor/alpha"); const auto resolved = co_await std::move(first); EXPECT_EQ(resolved, std::optional<std::string>("vendor/alpha")); EXPECT_EQ(fetches, 1);
  auto other = upstream::resolveOpenRouterUpstreamModel("vendor/gamma"); const auto missing = co_await std::move(other); EXPECT(!missing); EXPECT_EQ(fetches, 1);
  rt::env::remove("OPENROUTER_API_KEY"); models::resetOpenRouterModelsCacheForTest();
  auto absent = upstream::resolveOpenRouterUpstreamModel("vendor/alpha"); const auto unconfigured = co_await std::move(absent); EXPECT(!unconfigured); EXPECT_EQ(fetches, 1);
}

ASYNC_TEST("openrouter direct text transports preserve body headers dispatch order and passive capture") {
  Scope isolation;
  for (int route = 0; route < 2; ++route) {
    const bool chat = route == 0; std::vector<std::string> order;
    const json::object body{{"model", "vendor/alpha"}, {"cache_control", json::object{{"type", "ephemeral"}, {"ttl", "1h"}}}, {"nullable", nullptr}};
    auto recorder = uos::sentinel::upstream_capture::createSentinelUpstreamRecorder();
    upstream::OpenRouterDispatchHooks hooks;
    hooks.beforeDispatch = [&]() { order.push_back("admission"); uos::api_key_policy::ApiKeyProviderDispatch dispatch;
      dispatch.markTransportStarted = [&] { order.push_back("started"); };
      dispatch.cancelBeforeTransport = [] { return rt::fulfilledTask(); };
      return rt::fulfilledTask(std::optional<uos::api_key_policy::ApiKeyProviderDispatch>(dispatch)); };
    hooks.onDispatch = [&] { order.push_back("dispatch"); }; hooks.onHeaders = [&] { order.push_back("headers"); }; hooks.sentinelUpstreamRecorder = recorder;
    upstream::OpenRouterDispatchInit init{rt::AbortController().signal(), hooks};
    const rt::Fetch fake([&](rt::RequestInfo url, rt::RequestInit options) {
      order.push_back("fetch"); EXPECT_EQ(std::get<std::string>(url), std::string(chat ? upstream::OPENROUTER_CHAT_COMPLETIONS_URL : upstream::OPENROUTER_RESPONSES_URL));
      EXPECT_EQ(options.method, std::optional<std::string>("POST")); EXPECT(options.signal); EXPECT(options.headers);
      EXPECT_EQ(options.headers->get("authorization"), std::optional<std::string>("Bearer fixture-openrouter-key"));
      EXPECT_EQ(options.headers->get("accept"), std::optional<std::string>("application/json")); EXPECT_EQ(options.headers->get("content-type"), std::optional<std::string>("application/json"));
      EXPECT(options.body); EXPECT(rt::deepEqual(rt::jsonParse(std::get<std::string>(*options.body)), body));
      return rt::Response::json(json::object{{"answer", "fixture"}});
    });
    const rt::GlobalFetchOverride fetch(fake); auto requesting = textRequest(chat, body, init); const auto response = co_await std::move(requesting);
    const std::vector<std::string> expected{"admission", "started", "dispatch", "fetch", "headers"}; EXPECT_EQ(order, expected);
    auto reading = response.text(); const auto payload = co_await std::move(reading); EXPECT_EQ(payload, "{\"answer\":\"fixture\"}");
    const auto trace = recorder.snapshotAndSeal(); EXPECT_EQ(trace.attempts.size(), 1U); EXPECT_EQ(trace.attempts[0].provider, "openrouter"); EXPECT_EQ(trace.attempts[0].terminal, "eof"); recorder.dispose();
  }
}

ASYNC_TEST("openrouter direct text dispatch preserves hook refusal identity and aborts before transport") {
  Scope isolation; const auto refusal = rt::makeReason(rt::JsError("quota hook refused")); int calls = 0; int cancellations = 0;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { ++calls; return rt::Response(); }));
  const json::object body{{"model", "vendor/alpha"}};
  for (int route = 0; route < 2; ++route) {
    upstream::OpenRouterDispatchHooks hooks; hooks.beforeDispatch = [refusal]() -> rt::Task<std::optional<uos::api_key_policy::ApiKeyProviderDispatch>> { std::rethrow_exception(refusal); };
    upstream::OpenRouterDispatchInit init{rt::AbortController().signal(), hooks}; auto requesting = textRequest(route == 0, body, init);
    EXPECT_REJECTS_REASON(std::move(requesting), refusal);
    rt::AbortController controller; controller.abort();
    hooks.beforeDispatch = [&] { uos::api_key_policy::ApiKeyProviderDispatch dispatch;
      dispatch.markTransportStarted = [&] { ++calls; }; dispatch.cancelBeforeTransport = [&] { ++cancellations; return rt::fulfilledTask(); };
      return rt::fulfilledTask(std::optional<uos::api_key_policy::ApiKeyProviderDispatch>(dispatch)); };
    init = {controller.signal(), hooks}; auto aborted = textRequest(route == 0, body, init);
    EXPECT_REJECTS_MESSAGE(std::move(aborted), rt::DOMException, "Aborted");
  }
  EXPECT_EQ(calls, 0); EXPECT_EQ(cancellations, 2);
}

ASYNC_TEST("openrouter direct text transport errors and missing credentials keep exact error contracts") {
  Scope isolation; const json::object body{}; const upstream::OpenRouterDispatchInit init{rt::AbortController().signal(), std::nullopt};
  for (int route = 0; route < 2; ++route) {
    auto recorder = uos::sentinel::upstream_capture::createSentinelUpstreamRecorder();
    upstream::OpenRouterDispatchHooks hooks; hooks.sentinelUpstreamRecorder = recorder; const upstream::OpenRouterDispatchInit recording{init.signal, hooks};
    rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Response { throw rt::JsError("network failure"); }));
    auto requesting = textRequest(route == 0, body, recording); bool rejected = false;
    try { auto ignored = co_await std::move(requesting); (void)ignored; } catch (const upstream::OpenRouterError& error) { rejected = error.code == "openrouter_upstream_unreachable" && error.status == 502 && !error.upstreamStatus; }
    EXPECT(rejected); const auto trace = recorder.snapshotAndSeal(); EXPECT_EQ(trace.attempts.size(), 1U); EXPECT_EQ(trace.attempts[0].terminal, "fetch_error"); recorder.dispose();
  }
  rt::env::remove("OPENROUTER_API_KEY"); auto missing = upstream::fetchOpenRouterResponses(body, init);
  bool rejected = false; try { auto ignored = co_await std::move(missing); (void)ignored; } catch (const upstream::OpenRouterError& error) { rejected = error.code == "openrouter_api_key_missing" && error.status == 503; } EXPECT(rejected);
}

ASYNC_TEST("openrouter direct text transport preserves AbortError and headers hook failures") {
  Scope isolation; const json::object body{}; const upstream::OpenRouterDispatchInit init{rt::AbortController().signal(), std::nullopt};
  {
    rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Response { throw rt::DOMException("cancelled", "AbortError"); }));
    auto requesting = upstream::fetchOpenRouterResponses(body, init); EXPECT_REJECTS_MESSAGE(std::move(requesting), rt::DOMException, "cancelled");
  }
  {
    const auto failure = rt::makeReason(rt::JsError("headers hook failed")); upstream::OpenRouterDispatchHooks hooks; hooks.onHeaders = [failure] { std::rethrow_exception(failure); };
    const upstream::OpenRouterDispatchInit hooked{init.signal, hooks}; rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo, rt::RequestInit) { return rt::Response(); }));
    auto requesting = upstream::fetchOpenRouterResponses(body, hooked); EXPECT_REJECTS_REASON(std::move(requesting), failure);
  }
}

namespace {
  namespace telemetry = uos::openai_telemetry;
  namespace streams = uos::provider::openrouter_streams;
  telemetry::UsageContext streamUsageContext(const std::shared_ptr<telemetry::ResponseTelemetryState>& state) {
    telemetry::UsageContext context{.keyId = std::nullopt, .kernelRepo = std::nullopt, .kernelOrg = std::nullopt};
    context.requestId = "openrouter-stream-test"; context.startedAtMs = rt::dateNow(); context.startedAtMonotonicMs = rt::performanceNow(); context.responseTelemetry = state;
    return context;
  }
  rt::Response sseResponse(const std::vector<std::string>& frames) {
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.start = [frames](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
      for (const auto& frame : frames) controller.enqueue(rt::encodeUtf8(frame));
      controller.close();
    };
    const rt::ResponseInit init{.status = 200, .headers = rt::Headers{{"content-type", "text/event-stream"}}};
    return rt::Response(rt::ByteStream(source), init);
  }
  std::vector<std::string> idsFrom(const json::value& payload) {
    std::vector<std::string> ids;
    if (const auto* rows = rt::getArray(payload, "data")) for (const auto& row : *rows) if (const auto id = rt::getString(row, "id")) ids.emplace_back(*id);
    return ids;
  }
}

ASYNC_TEST("the public catalogue lists only operator-enabled ids while an empty whitelist filters none") {
  Scope isolation; uos::config::config.codexAuthJsonB64.clear();
  const auto kv = std::make_shared<rt::MemoryKv>(rt::dateNow);
  const json::object snapshot{{"source", "chatgpt_codex"}, {"client_version", "0.125.0"}, {"updated_at_ms", rt::makeNumber(rt::dateNow())},
    {"models", json::array{json::object{{"slug", "hidden-codex-id"}}, json::object{{"slug", "listed-codex-id"}}}}};
  const rt::KvValue storedSnapshot(snapshot);
  auto seedSnapshot = kv->set(uos::codex::index::CODEX_MODELS_KV_KEY, storedSnapshot); co_await std::move(seedSnapshot);
  const rt::KvValue config(json::object{{"version", 2}, {"default_model", "listed-codex-id"}, {"default_reasoning_effort", "low"}, {"codex_models", snapshot}, {"updated_at_ms", rt::makeNumber(rt::dateNow())}});
  auto seedConfig = kv->set(uos::runtime_config::RUNTIME_CONFIG_V2_KEY, config); co_await std::move(seedConfig);
  const rt::KvValue whitelist(json::object{{"model_ids", json::array{"listed-codex-id", "vendor/alpha"}}, {"updated_at_ms", 1}});
  auto seedWhitelist = kv->set(uos::models::codex_models_whitelist::CODEX_MODELS_WHITELIST_KV_KEY, whitelist); co_await std::move(seedWhitelist);
  uos::provider::selection::resetProviderSelectionCacheForTest(); uos::runtime_config::resetRuntimeConfigCacheForTest(); uos::kv::setKvForTest(kv);
  const auto cleanup = rt::scopeExit([] { uos::kv::setKvForTest(nullptr); uos::provider::selection::resetProviderSelectionCacheForTest(); uos::runtime_config::resetRuntimeConfigCacheForTest(); });
  co_await warm();
  auto listing = uos::models::catalog::handleModels(); const auto list = co_await std::move(listing); EXPECT_EQ(list.status(), 200);
  const auto listed = co_await list.json(); const auto listedIds = idsFrom(listed);
  EXPECT(rt::includes(listedIds, std::string("listed-codex-id"))); EXPECT(!rt::includes(listedIds, std::string("hidden-codex-id")));
  EXPECT(rt::includes(listedIds, std::string("vendor/alpha"))); EXPECT(rt::includes(listedIds, std::string("vendor/beta"))); EXPECT(rt::includes(listedIds, std::string("typesafe/jev-latest")));
  auto loadingCapabilities = uos::models::catalog::handleModelCapabilities(); const auto capabilities = co_await std::move(loadingCapabilities); EXPECT_EQ(capabilities.status(), 200);
  const auto capabilityPayload = co_await capabilities.json(); const auto capabilityIds = idsFrom(capabilityPayload);
  EXPECT(rt::includes(capabilityIds, std::string("vendor/alpha"))); EXPECT(rt::includes(capabilityIds, std::string("vendor/beta"))); EXPECT(!rt::includes(capabilityIds, std::string("hidden-codex-id")));
  auto loadingPublic = uos::models::catalog::handlePublicModelCatalog(); const auto catalog = co_await std::move(loadingPublic); EXPECT_EQ(catalog.status(), 200);
  const auto publicPayload = co_await catalog.json(); const auto publicIds = idsFrom(publicPayload);
  const auto* entries = rt::getArray(publicPayload, "data"); ASSERT(entries);
  const json::value* alpha = nullptr; for (const auto& entry : *entries) if (rt::getString(entry, "id") == "vendor/alpha") alpha = &entry;
  ASSERT(alpha); const auto* providers = rt::getArray(*alpha, "providers"); ASSERT(providers);
  std::vector<std::string> providerIds; for (const auto& provider : *providers) if (const auto id = rt::getString(provider, "id")) providerIds.emplace_back(*id);
  const std::vector<std::string> expectedProviders{"openrouter"}; EXPECT_EQ(providerIds, expectedProviders);
  EXPECT(rt::includes(publicIds, std::string("listed-codex-id"))); EXPECT(!rt::includes(publicIds, std::string("hidden-codex-id"))); EXPECT(!rt::includes(publicIds, std::string("vendor/beta")));
  const rt::KvValue emptyWhitelist(json::object{{"model_ids", json::array{}}, {"updated_at_ms", 2}});
  auto clearWhitelist = kv->set(uos::models::codex_models_whitelist::CODEX_MODELS_WHITELIST_KV_KEY, emptyWhitelist); co_await std::move(clearWhitelist);
  auto loadingUnfiltered = uos::models::catalog::handlePublicModelCatalog(); const auto unfiltered = co_await std::move(loadingUnfiltered);
  const auto unfilteredPayload = co_await unfiltered.json(); const auto unfilteredIds = idsFrom(unfilteredPayload);
  EXPECT(rt::includes(unfilteredIds, std::string("vendor/beta"))); EXPECT(rt::includes(unfilteredIds, std::string("hidden-codex-id")));
}

ASYNC_TEST("openrouter codex rows carry only source-stated metadata") {
  Scope isolation; co_await warm();
  const json::array stored{json::object{{"slug", "vendor/alpha"}, {"display_name", "stored alpha"}}};
  const auto rows = uos::catalog::models::withOpenRouterModels(stored);
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rt::getString(rows[0], "slug"), std::optional<std::string_view>("vendor/alpha")); EXPECT_EQ(rt::getString(rows[1], "slug"), std::optional<std::string_view>("vendor/beta"));
  EXPECT_EQ(rt::getString(rows[0], "display_name"), std::optional<std::string_view>("stored alpha"));
  const auto& beta = rows[1]; EXPECT_EQ(rt::getString(beta, "display_name"), std::optional<std::string_view>("vendor/beta")); EXPECT_EQ(rt::getString(beta, "owned_by"), std::optional<std::string_view>("vendor"));
  const auto endpoints = rt::jsonParse(R"(["openai-response","openai-chat"])"); const auto levels = rt::jsonParse(R"([{"effort":"none","description":"No reasoning"}])");
  EXPECT(rt::deepEqual(*rt::get(beta, "supported_endpoint_types"), endpoints)); EXPECT(rt::deepEqual(*rt::get(beta, "supported_reasoning_levels"), levels));
  EXPECT_EQ(rt::getString(beta, "default_reasoning_level"), std::optional<std::string_view>("none"));
}

ASYNC_TEST("openrouter responses records a shared inactivity timeout as a deadline") {
  Scope isolation;
  const auto state = telemetry::createResponseTelemetryState(); const auto context = streamUsageContext(state);
  const uos::responses_stream::ResponsesStreamErrorOptions options{.kind = "inactivity_timeout"};
  const auto failure = rt::makeReason(uos::responses_stream::ResponsesStreamError("inactive", options));
  rt::UnderlyingSource<rt::ByteChunk> source; source.start = [failure](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) { controller.error(failure); };
  const rt::ResponseInit init{.status = 200, .headers = rt::Headers{{"content-type", "text/event-stream"}}};
  const auto response = streams::streamOpenRouterResponses(rt::Response(rt::ByteStream(source), init), std::nullopt, &context, rt::AbortController().signal(), rt::AbortController().signal());
  const auto text = co_await response.text(); (void)text;
  EXPECT_EQ(state->streamTerminalType, std::optional<std::string>("deadline")); EXPECT_EQ(state->failureKind, std::optional<std::string>("inactivity_timeout"));
}

ASYNC_TEST("openrouter responses records a shared premature EOF as eof") {
  Scope isolation;
  const auto state = telemetry::createResponseTelemetryState(); const auto context = streamUsageContext(state);
  const auto upstreamResponse = sseResponse({"event: response.created\ndata: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_1\"}}\n\n"});
  const auto response = streams::streamOpenRouterResponses(upstreamResponse, std::nullopt, &context, rt::AbortController().signal(), rt::AbortController().signal());
  const auto text = co_await response.text(); (void)text;
  EXPECT_EQ(state->streamTerminalType, std::optional<std::string>("eof")); EXPECT_EQ(state->failureKind, std::optional<std::string>("premature_eof"));
}

ASYNC_TEST("OpenRouter parser matches fifteen isolated Bun source traces") {
  Scope isolation;
  const auto golden = rt::jsonParse(R"GOLDEN([{"name":"no-body","frames":[],"error":{"name":"OpenRouterStreamError","message":"OpenRouter returned no Chat Completions stream body.","kind":"premature_eof"}},{"name":"empty","frames":[],"error":{"name":"OpenRouterStreamError","message":"OpenRouter Chat Completions stream ended before [DONE].","kind":"premature_eof"}},{"name":"null","frames":[],"error":{"name":"OpenRouterStreamError","message":"OpenRouter emitted a non-object Chat Completions chunk.","kind":"invalid_chunk"}},{"name":"array","frames":[],"error":{"name":"OpenRouterStreamError","message":"OpenRouter emitted a Chat Completions chunk without choices.","kind":"invalid_chunk"}},{"name":"no-choices","frames":[],"error":{"name":"OpenRouterStreamError","message":"OpenRouter emitted a Chat Completions chunk without choices.","kind":"invalid_chunk"}},{"name":"malformed","frames":[],"error":{"name":"OpenRouterStreamError","message":"OpenRouter emitted malformed Chat Completions SSE JSON.","kind":"malformed_event"}},{"name":"error-string","frames":[],"error":{"name":"OpenRouterStreamError","message":"provider broke","kind":"upstream_error"}},{"name":"empty-error","frames":[],"error":{"name":"OpenRouterStreamError","message":"OpenRouter reported an upstream stream error.","kind":"upstream_error"}},{"name":"null-error","frames":[],"error":{"name":"OpenRouterStreamError","message":"OpenRouter reported an upstream stream error.","kind":"upstream_error"}},{"name":"empty-data","frames":[],"error":{"name":"OpenRouterStreamError","message":"OpenRouter emitted malformed Chat Completions SSE JSON.","kind":"malformed_event"}},{"name":"done","frames":[{"kind":"done"}]},{"name":"frames","frames":[{"kind":"comment","text":": PROCESSING\n: still waiting"},{"kind":"chunk","value":{"choices":[]}},{"kind":"done"}]},{"name":"unterminated","frames":[],"error":{"name":"OpenRouterStreamError","message":"OpenRouter Chat Completions stream ended before [DONE].","kind":"premature_eof"}},{"name":"at-limit","frames":[],"error":{"name":"OpenRouterStreamError","message":"OpenRouter Chat Completions stream ended before [DONE].","kind":"premature_eof"}},{"name":"over-limit","frames":[],"error":{"name":"OpenRouterStreamError","message":"OpenRouter SSE frame exceeded the gateway bound.","kind":"frame_too_large"}}])GOLDEN");
  const std::vector<std::pair<std::string, std::optional<std::string>>> inputs{
    {"no-body", std::nullopt}, {"empty", ""}, {"null", "data: null\n\n"}, {"array", "data: []\n\n"}, {"no-choices", "data: {}\n\n"},
    {"malformed", "data: {not json}\n\n"}, {"error-string", "data: {\"error\":\"provider broke\"}\n\n"},
    {"empty-error", "data: {\"error\":{\"message\":\"\"}}\n\n"}, {"null-error", "data: {\"error\":null}\n\n"},
    {"empty-data", "data\n\n"}, {"done", "data:  [DONE]\n\ndata: {not json}\n\n"},
    {"frames", ": PROCESSING\r\n: still waiting\r\n\r\nevent: ignored\r\nid: 1\r\nretry: 2\r\n\r\ndata: {\"choices\":\r\ndata: []}\r\n\r\ndata: [DONE]\r\n\r\n"},
    {"unterminated", "data: {\"choices\":[]}"}, {"at-limit", ":" + std::string(4 * 1024 * 1024 - 1, 'x')}, {"over-limit", ":" + std::string(4 * 1024 * 1024, 'x')}
  };
  json::array actual;
  for (const auto& [name, text] : inputs) {
    const auto upstreamResponse = text ? rt::Response(*text) : rt::Response(nullptr);
    const auto iterator = streams::iterateOpenRouterChatCompletionStream(upstreamResponse);
    json::array frames; std::optional<json::object> error;
    try {
      for (;;) {
        const auto next = co_await iterator.next(); if (next.done) break;
        if (const auto* comment = std::get_if<uos::deepseek::stream::DeepSeekStreamFrameComment>(&*next.value)) frames.push_back(json::object{{"kind", "comment"}, {"text", comment->text}});
        else if (const auto* chunk = std::get_if<uos::deepseek::stream::DeepSeekStreamFrameChunk>(&*next.value)) frames.push_back(json::object{{"kind", "chunk"}, {"value", chunk->value}});
        else frames.push_back(json::object{{"kind", "done"}});
      }
    } catch (const streams::OpenRouterStreamError& failure) { error = json::object{{"name", failure.name}, {"message", failure.message}, {"kind", failure.kind}}; }
    json::object row{{"name", name}, {"frames", frames}}; if (error) rt::objectDefine(row, "error", *error); actual.push_back(row);
  }
  EXPECT(rt::deepEqual(actual, golden));
}

ASYNC_TEST("OpenRouter stream wrappers preserve chat usage and native terminal health") {
  Scope isolation; uos::config::config.codexAuthJsonB64.clear();
  const auto kv = std::make_shared<rt::MemoryKv>(rt::dateNow); uos::kv::setKvForTest(kv);
  const auto cleanup = rt::scopeExit([] { uos::kv::setKvForTest(nullptr); uos::provider::health::resetProviderHealthThrottleForTest(); });
  const auto signal = rt::AbortController().signal();
  const auto state = telemetry::createResponseTelemetryState(); const auto context = streamUsageContext(state);
  const auto upstreamResponse = sseResponse({"data: {\"id\":\"gen-1\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"pi\"},\"finish_reason\":null}]}\n\n",
    "data: {\"id\":\"gen-1\",\"object\":\"chat.completion.chunk\",\"choices\":[],\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":2,\"total_tokens\":5}}\n\n", "data: [DONE]\n\n"});
  const auto response = streams::streamOpenRouterChatCompletion(upstreamResponse, std::nullopt, &context, signal, signal, "vendor/alpha");
  EXPECT_EQ(response.status(), 200); const auto text = co_await response.text(); EXPECT(rt::includes(text, "\"content\":\"pi\"")); EXPECT(rt::includes(text, "data: [DONE]"));
  EXPECT_EQ(state->inputTokens, std::optional<double>(3)); EXPECT_EQ(state->outputTokens, std::optional<double>(2)); EXPECT(state->usageObserved);
  EXPECT_EQ(state->streamTerminalType, std::optional<std::string>("response.completed"));
  co_await rt::sleep(1);
  const auto malformedState = telemetry::createResponseTelemetryState(); const auto malformedContext = streamUsageContext(malformedState);
  const auto malformed = streams::streamOpenRouterChatCompletion(sseResponse({"data: {not json}\n\n", "data: [DONE]\n\n"}), std::nullopt, &malformedContext, signal, signal, "vendor/alpha");
  const auto failureText = co_await malformed.text(); EXPECT(rt::includes(failureText, "openrouter_upstream_stream_error"));
  EXPECT_EQ(malformedState->streamTerminalType, std::optional<std::string>("error")); EXPECT_EQ(malformedState->failureKind, std::optional<std::string>("malformed_event"));
  co_await rt::sleep(1);
  const std::vector<std::string> terminals{"response.completed", "response.failed", "response.incomplete"};
  for (const auto& type : terminals) {
    uos::provider::health::resetProviderHealthThrottleForTest();
    const auto nativeState = telemetry::createResponseTelemetryState(); const auto nativeContext = streamUsageContext(nativeState);
    json::object payload{{"id", "resp_1"}, {"status", type == "response.completed" ? "completed" : type == "response.failed" ? "failed" : "incomplete"},
      {"usage", json::object{{"input_tokens", 4}, {"output_tokens", 1}, {"total_tokens", 5}}}};
    if (type == "response.incomplete") rt::objectDefine(payload, "incomplete_details", json::object{{"reason", "provider_overloaded"}});
    const json::object event{{"type", type}, {"response", payload}};
    const auto nativeUpstream = sseResponse({"event: response.created\ndata: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_1\"}}\n\n",
      "event: response.output_text.delta\ndata: {\"type\":\"response.output_text.delta\",\"delta\":\"ok\"}\n\n", "event: " + type + "\ndata: " + rt::jsonStringify(event) + "\n\n"});
    const auto native = streams::streamOpenRouterResponses(nativeUpstream, std::nullopt, &nativeContext, signal, signal);
    EXPECT_EQ(native.status(), 200); const auto nativeText = co_await native.text(); EXPECT(rt::includes(nativeText, "response.output_text.delta")); EXPECT(rt::includes(nativeText, type));
    EXPECT_EQ(nativeState->streamTerminalType, std::optional<std::string>(type)); EXPECT_EQ(nativeState->outputTokens, std::optional<double>(1));
    co_await rt::sleep(1);
    const auto observed = co_await uos::provider::health::getOpenRouterProviderHealth();
    EXPECT_EQ(observed.last_event, std::optional<std::string>(type == "response.completed" ? "success" : "upstream_error"));
    EXPECT_EQ(observed.last_status, std::optional<double>(200)); EXPECT_EQ(observed.state, type == "response.completed" ? "healthy" : "degraded");
  }
}

#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  namespace routes = uos::provider::openrouter_handlers;
  namespace quota = uos::api_key_policy;
  namespace counting = uos::tests::helpers::counting_kv;
  std::string routePath(bool chat) { return chat ? "/v1/chat/completions" : "/v1/responses"; }
  json::object routeBody(bool chat, const std::string& model = "vendor/alpha") {
    json::object body{{"model", model}};
    if (chat) rt::objectDefine(body, "messages", json::array{json::object{{"role", "user"}, {"content", "hi"}}});
    else rt::objectDefine(body, "input", "hi");
    return body;
  }
  rt::Request routeRequest(bool chat) { return rt::Request("https://ai.ubq.fi" + routePath(chat), rt::RequestInit{.method = "POST"}); }
  rt::Task<rt::Response> routeCall(bool chat, json::object body, std::string model, const telemetry::UsageContext* context = nullptr, routes::OpenRouterHandlerDeps deps = {}) {
    return chat ? routes::handleOpenRouterChatCompletions(routeRequest(chat), std::move(body), std::move(model), context, std::move(deps))
                : routes::handleOpenRouterResponses(routeRequest(chat), std::move(body), std::move(model), context, std::move(deps));
  }
  rt::Response jsonResponse(const json::value& body, int status = 200) { return rt::Response(rt::jsonStringify(body), rt::ResponseInit{.status = status, .headers = rt::Headers{{"content-type", "application/json"}}}); }
  json::object responsePayload(const std::string& status = "completed", const std::optional<json::object>& usage = std::nullopt) {
    auto payload = rt::jsonParse(R"({"id":"resp_1","object":"response","status":"completed","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"ok"}]}]})").as_object();
    payload["status"] = status;
    if (usage) rt::objectDefine(payload, "usage", *usage);
    return payload;
  }
  rt::Task<void> warmCacheCatalogue() {
    models::setOpenRouterModelsFetchForTest(rt::Fetch([](rt::RequestInfo, rt::RequestInit) {
      return jsonResponse(json::object{{"data", json::array{json::object{{"id", "~anthropic/claude-opus-latest"}}, json::object{{"id", "anthropic/claude-sonnet-4"}}, json::object{{"id", "vendor/alpha"}}}}});
    }));
    const models::FetchOpenRouterModelsOptions options;
    AWAIT(result, models::fetchOpenRouterModels(options)); EXPECT(result);
  }
  json::object cacheUsageFor(bool chat, std::optional<json::object> details = std::nullopt) {
    json::object usage = chat ? json::object{{"prompt_tokens", 1000}, {"completion_tokens", 20}, {"total_tokens", 1020}}
                              : json::object{{"input_tokens", 1000}, {"output_tokens", 20}, {"total_tokens", 1020}};
    if (details) rt::objectDefine(usage, chat ? "prompt_tokens_details" : "input_tokens_details", *details);
    return usage;
  }
  struct CacheResponse { rt::Response response; json::object payload; };
  CacheResponse cacheResponseFor(bool chat, bool stream, const json::object& usage) {
    auto payload = chat ? rt::jsonParse(R"({"id":"gen_cache_fixture","object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":"cached reply"},"finish_reason":"stop"}]})").as_object()
      : rt::jsonParse(R"({"id":"resp_cache_fixture","object":"response","status":"completed","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"cached reply"}]}]})").as_object();
    payload["usage"] = usage;
    if (!stream) return {jsonResponse(payload), payload};
    std::vector<std::string> frames;
    if (chat) {
      auto delta = rt::jsonParse(R"({"id":"gen_cache_fixture","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":"cached reply"},"finish_reason":"stop"}]})").as_object();
      const json::object counters{{"id", payload.at("id")}, {"object", "chat.completion.chunk"}, {"choices", json::array{}}, {"usage", usage}};
      frames.push_back("data: " + rt::jsonStringify(delta) + "\n\n");
      frames.push_back("data: " + rt::jsonStringify(counters) + "\n\n"); frames.push_back("data: [DONE]\n\n");
    } else {
      const std::vector<json::object> events{{{"type", "response.created"}, {"response", json::object{{"id", payload.at("id")}}}}, {{"type", "response.output_text.delta"}, {"delta", "cached reply"}}, {{"type", "response.completed"}, {"response", payload}}};
      for (const auto& event : events) frames.push_back("event: " + std::string(*rt::getString(event, "type")) + "\ndata: " + rt::jsonStringify(event) + "\n\n");
    }
    return {sseResponse(frames), payload};
  }

}

ASYNC_TEST("openrouter chat forwards the client body with the served model") {
  Scope isolation; co_await warm(); std::optional<json::object> captured;
  routes::OpenRouterHandlerDeps deps;
  deps.fetchChat = [&](const json::object& body, const upstream::OpenRouterDispatchInit&) { captured = body; return rt::fulfilledTask(jsonResponse(json::object{{"id", "gen-1"}, {"object", "chat.completion"}, {"choices", json::array{json::object{{"index", 0}, {"message", json::object{{"role", "assistant"}, {"content", "hello"}}}, {"finish_reason", "stop"}}}}, {"usage", json::object{{"prompt_tokens", 3}, {"completion_tokens", 2}, {"total_tokens", 5}}}})); };
  const auto raw = routeBody(true); AWAIT(response, routeCall(true, raw, "vendor/alpha", nullptr, deps)); EXPECT_EQ(response.status(), 200); ASSERT(captured); EXPECT_EQ(rt::getString(*captured, "model"), "vendor/alpha"); EXPECT_EQ(response.headers().get("x-uos-upstream"), "openrouter");
  AWAIT(body, response.json()); const auto* choices = rt::getArray(body, "choices"); ASSERT(choices && !choices->empty()); const auto* message = rt::get((*choices)[0], "message"); ASSERT(message); EXPECT_EQ(rt::getString(*message, "content"), "hello");
}
ASYNC_TEST("openrouter refuses a model outside the catalogue on both wires") {
  Scope isolation; co_await warm();
  for (int wire = 0; wire < 2; ++wire) { const auto raw = routeBody(wire == 0, "vendor/gamma"); AWAIT(response, routeCall(wire == 0, raw, "vendor/gamma")); EXPECT_EQ(response.status(), 400); }
}
ASYNC_TEST("openrouter responses forwards the client body and reports the upstream") {
  Scope isolation; co_await warm(); std::optional<json::object> captured; routes::OpenRouterHandlerDeps deps;
  deps.fetchResponses = [&](const json::object& body, const upstream::OpenRouterDispatchInit&) { captured = body; return rt::fulfilledTask(jsonResponse(responsePayload("completed", json::object{{"input_tokens", 4}, {"output_tokens", 1}, {"total_tokens", 5}}))); };
  const auto raw = routeBody(false, "vendor/beta"); AWAIT(response, routeCall(false, raw, "vendor/beta", nullptr, deps)); EXPECT_EQ(response.status(), 200); ASSERT(captured); EXPECT_EQ(rt::getString(*captured, "model"), "vendor/beta"); EXPECT_EQ(response.headers().get("x-uos-upstream"), "openrouter");
}
ASYNC_TEST("openrouter buffered Responses fails an empty completion closed without masking truncation") {
  Scope isolation; co_await warm(); const std::vector<json::value> outputs{json::array{}, nullptr, json::array{json::object{{"type", "reasoning"}, {"summary", json::array{}}}}};
  for (const auto& output : outputs) {
    const auto state = telemetry::createResponseTelemetryState(); const auto context = streamUsageContext(state); routes::OpenRouterHandlerDeps deps;
    deps.fetchResponses = [output](const json::object&, const upstream::OpenRouterDispatchInit&) { return rt::fulfilledTask(jsonResponse(json::object{{"status", "completed"}, {"output", output}, {"usage", json::object{{"input_tokens", 2}, {"output_tokens", 1}, {"total_tokens", 3}}}})); };
    const auto raw = routeBody(false, "vendor/beta"); AWAIT(response, routeCall(false, raw, "vendor/beta", &context, deps)); EXPECT_EQ(response.status(), 502); AWAIT(body, response.json()); EXPECT_EQ(rt::getString(*rt::get(body, "error"), "code"), "empty_upstream_completion"); EXPECT(!state->completed); EXPECT_EQ(state->failureKind, "empty_upstream_completion"); EXPECT_EQ(state->streamTerminalType, "response.failed");
  }
  const auto state = telemetry::createResponseTelemetryState(); const auto context = streamUsageContext(state); routes::OpenRouterHandlerDeps deps;
  deps.fetchResponses = [](const json::object&, const upstream::OpenRouterDispatchInit&) { return rt::fulfilledTask(jsonResponse(json::object{{"status", "incomplete"}, {"output", nullptr}})); };
  const auto raw = routeBody(false, "vendor/beta"); AWAIT(response, routeCall(false, raw, "vendor/beta", &context, deps)); EXPECT_EQ(response.status(), 200); EXPECT(!state->completed); EXPECT(!state->failureKind); EXPECT_EQ(state->streamTerminalType, "response.incomplete");
}
ASYNC_TEST("openrouter buffered Responses preserves failed upstream terminals without answer output") {
  Scope isolation; co_await warm(); const auto state = telemetry::createResponseTelemetryState(); const auto context = streamUsageContext(state);
  const json::object payload{{"id", "resp_failed"}, {"object", "response"}, {"status", "failed"}, {"error", json::object{{"code", "upstream_error"}, {"message", "provider failed"}}}, {"output", json::array{}}, {"usage", json::object{{"input_tokens", 2}, {"output_tokens", 0}, {"total_tokens", 2}}}};
  routes::OpenRouterHandlerDeps deps; deps.fetchResponses = [payload](const json::object&, const upstream::OpenRouterDispatchInit&) { return rt::fulfilledTask(jsonResponse(payload)); };
  const auto raw = routeBody(false, "vendor/beta"); AWAIT(response, routeCall(false, raw, "vendor/beta", &context, deps)); EXPECT_EQ(response.status(), 200); AWAIT(body, response.json()); EXPECT(rt::deepEqual(body, payload)); EXPECT(!state->completed); EXPECT_EQ(state->streamTerminalType, "response.failed");
}
ASYNC_TEST("openrouter rethrows local quota-hook refusals before either transport starts") {
  Scope isolation; co_await warm(); const auto kv = std::make_shared<counting::CountingKv>(); uos::kv::setKvForTest(kv); uos::provider::health::resetProviderHealthThrottleForTest(); unsigned fetchCalls = 0;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) -> rt::Response { ++fetchCalls; throw rt::JsError("a local quota refusal must never fetch"); }));
  for (int wire = 0; wire < 2; ++wire) for (int stream = 0; stream < 2; ++stream) {
    const auto state = telemetry::createResponseTelemetryState(); auto context = streamUsageContext(state);
    quota::ApiKeyQuotaDispatchErrorOptions options; options.status = 429; options.code = "rate_limit_exceeded"; options.errorType = "rate_limit_error"; options.headers = rt::OrderedMap<std::string,std::string>{{"Retry-After", "17"}, {"ratelimit-limit", "1"}, {"ratelimit-remaining", "0"}};
    const auto refusal = rt::makeReason(quota::ApiKeyQuotaDispatchError("fixture quota exhausted", options)); unsigned hookCalls = 0;
    context.beforeProviderDispatch = [&, refusal](const std::string& provider) -> rt::Task<std::optional<quota::ApiKeyProviderDispatch>> { EXPECT_EQ(provider, "openrouter"); ++hookCalls; std::rethrow_exception(refusal); };
    auto raw = routeBody(wire == 0); raw["stream"] = stream != 0; auto requesting = routeCall(wire == 0, raw, "vendor/alpha", &context); EXPECT_REJECTS_REASON(std::move(requesting), refusal);
    EXPECT_EQ(hookCalls, 1U); EXPECT_EQ(fetchCalls, 0U); EXPECT(state->attemptedProviders.empty()); EXPECT(!state->firstProviderDispatchMs); EXPECT(!state->firstProviderHeadersMs); AWAIT(health, uos::provider::health::getOpenRouterProviderHealth()); EXPECT_EQ(health.state, "unknown");
  }
  uos::provider::health::resetProviderHealthThrottleForTest();
}
ASYNC_TEST("openrouter terminal routes retain real deferred-quota 429 bodies and headers with zero dispatch") {
  Scope isolation; co_await warm(); unsigned fetchCalls = 0;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) -> rt::Response { ++fetchCalls; throw rt::JsError("an exhausted key must never fetch"); }));
  for (int wire = 0; wire < 2; ++wire) {
    const bool chat = wire == 0; const auto kv = std::make_shared<counting::CountingKv>(); const auto token = "u_" + std::string(64, chat ? 'a' : 'b');
    AWAIT(hash, uos::utils::sha256Base64Url(token)); const auto now = rt::dateNow();
    const json::object record{{"id", "openrouter-quota-" + rt::crypto::randomUUID()}, {"expires_at_ms", -1}, {"revoked_at_ms", nullptr}, {"usage_limit_requests", 1}, {"usage_requests", 0}, {"usage_reset_at_ms", now + 60000}, {"window_ms", 60000}, {"usage_quota_version", 3}, {"paid_fallback_enabled", false}, {"paid_fallback_limit_microcredits", 0}, {"paid_fallback_spent_microcredits", 0}, {"paid_fallback_reserved_microcredits", 0}, {"paid_fallback_reservation_request_id", nullptr}};
    kv->seed(uos::api_keys::apiKeyHashKey(hash), record); uos::kv::setKvForTest(kv); uos::provider::selection::resetProviderSelectionCacheForTest(); uos::provider::health::resetProviderHealthThrottleForTest();
    const auto policy = uos::authority::native_api_key_policy::apiKeyPolicyFromHashRecord(hash, record, now); ASSERT(policy);
    quota::ReserveApiKeyUsageV3Options options; options.kv = kv; const auto path = routePath(chat);
    AWAIT(charged, quota::reserveApiKeyUsageV3(*policy, "already-dispatched", path, options)); ASSERT(std::holds_alternative<quota::ApiKeyUsageReservationDecisionOk>(charged));
    const auto reservation = std::get<quota::ApiKeyUsageReservationDecisionOk>(charged).reservation; const std::string provider = "openrouter";
    AWAIT(dispatch, reservation.beforeProviderDispatch(provider)); if (dispatch && dispatch->markTransportStarted) dispatch->markTransportStarted();
    const rt::Request req("https://ai.ubq.fi" + path, rt::RequestInit{.method = "POST", .headers = rt::Headers{{"authorization", "Bearer " + token}, {"content-type", "application/json"}}, .body = rt::jsonStringify(routeBody(chat))});
    const std::string requestId = "quota-" + rt::crypto::randomUUID();
    AWAIT(response, uos::handler::terminal_route::handleTerminalRoute(req, path, std::nullopt, requestId, now, rt::performanceNow())); EXPECT_EQ(response.status(), 429); AWAIT(body, response.json()); const auto* error = rt::get(body, "error"); ASSERT(error); EXPECT_EQ(rt::getString(*error, "code"), "rate_limit_exceeded"); EXPECT_EQ(rt::getString(*error, "type"), "rate_limit_error"); EXPECT(rt::stringToNumber(response.headers().get("retry-after").value_or("")) > 0); EXPECT_EQ(response.headers().get("ratelimit-limit"), "1"); EXPECT_EQ(response.headers().get("ratelimit-remaining"), "0"); EXPECT_EQ(fetchCalls, 0U); AWAIT(health, uos::provider::health::getOpenRouterProviderHealth()); EXPECT_EQ(health.state, "unknown");
    auto releasing = reservation.release(std::nullopt); co_await std::move(releasing);
  }
  uos::provider::selection::resetProviderSelectionCacheForTest(); uos::provider::health::resetProviderHealthThrottleForTest();
}
ASYNC_TEST("openrouter keeps ordinary transport failures as upstream 502s on both wires") {
  Scope isolation; co_await warm();
  for (int wire = 0; wire < 2; ++wire) {
    const auto kv = std::make_shared<counting::CountingKv>(); uos::kv::setKvForTest(kv); uos::provider::health::resetProviderHealthThrottleForTest(); unsigned fetchCalls = 0;
    rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) -> rt::Response { ++fetchCalls; throw rt::TypeError("fixture network refusal"); }));
    const auto state = telemetry::createResponseTelemetryState(); const auto context = streamUsageContext(state); const auto raw = routeBody(wire == 0);
    // Source awaits race optional health persistence: keep their Promise turns.
    rt::Promise<rt::Response> requesting(routeCall(wire == 0, raw, "vendor/alpha", &context)); AWAIT(response, requesting.wait()); EXPECT_EQ(response.status(), 502); rt::Promise<json::value> reading(response.json()); AWAIT(body, reading.wait()); EXPECT_EQ(rt::getString(*rt::get(body, "error"), "code"), "openrouter_upstream_unreachable"); EXPECT_EQ(fetchCalls, 1U); const std::vector<std::string> attempted{"openrouter"}; EXPECT_EQ(state->attemptedProviders, attempted); EXPECT(state->firstProviderDispatchMs); rt::Promise<uos::provider::health::ProviderHealthView> observing(uos::provider::health::getOpenRouterProviderHealth()); AWAIT(health, observing.wait()); EXPECT_EQ(health.last_event, "upstream_error");
  }
  uos::provider::health::resetProviderHealthThrottleForTest();
}
ASYNC_TEST("openrouter buffered responses derives terminal telemetry from payload status") {
  Scope isolation; co_await warm(); const std::vector<std::string> statuses{"completed", "incomplete", "failed"};
  for (const auto& status : statuses) {
    const auto state = telemetry::createResponseTelemetryState(); auto context = streamUsageContext(state); std::optional<bool> callbackCompleted;
    context.onTerminalUsage = [&](std::optional<telemetry::UsageTokens>, bool completed) { callbackCompleted = completed; };
    auto payload = responsePayload(status, json::object{{"input_tokens", 4}, {"output_tokens", 1}, {"total_tokens", 5}}); payload["id"] = "resp_status";
    routes::OpenRouterHandlerDeps deps; deps.fetchResponses = [payload](const json::object&, const upstream::OpenRouterDispatchInit&) { return rt::fulfilledTask(jsonResponse(payload)); };
    const auto raw = routeBody(false, "vendor/beta"); AWAIT(response, routeCall(false, raw, "vendor/beta", &context, deps)); EXPECT_EQ(response.status(), 200); EXPECT_EQ(state->streamTerminalType, "response." + status); EXPECT_EQ(state->completed, status == "completed"); EXPECT_EQ(callbackCompleted, std::optional<bool>(status == "completed"));
  }
}
ASYNC_TEST("openrouter chat relays validated stream frames and records usage") {
  Scope isolation; co_await warm(); const auto state = telemetry::createResponseTelemetryState(); const auto context = streamUsageContext(state); routes::OpenRouterHandlerDeps deps;
  deps.fetchChat = [](const json::object& body, const upstream::OpenRouterDispatchInit&) {
    EXPECT_EQ(rt::getBool(body, "stream"), true); EXPECT(rt::deepEqual(*rt::get(body, "stream_options"), json::object{{"include_usage", true}}));
    return rt::fulfilledTask(sseResponse({"data: {\"id\":\"gen-1\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"pi\"},\"finish_reason\":null}]}\n\n", "data: {\"id\":\"gen-1\",\"object\":\"chat.completion.chunk\",\"choices\":[],\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":2,\"total_tokens\":5}}\n\n", "data: [DONE]\n\n"}));
  };
  auto raw = routeBody(true); raw["stream"] = true; AWAIT(response, routeCall(true, raw, "vendor/alpha", &context, deps)); EXPECT_EQ(response.status(), 200); AWAIT(body, response.text()); EXPECT(rt::includes(body, "\"content\":\"pi\"")); EXPECT(rt::includes(body, "data: [DONE]")); EXPECT_EQ(state->inputTokens, 3); EXPECT_EQ(state->outputTokens, 2); EXPECT(state->usageObserved); EXPECT_EQ(state->streamTerminalType, "response.completed");
}
ASYNC_TEST("openrouter chat reports a malformed stream frame as a stream error") {
  Scope isolation; co_await warm(); const auto state = telemetry::createResponseTelemetryState(); const auto context = streamUsageContext(state); routes::OpenRouterHandlerDeps deps;
  deps.fetchChat = [](const json::object&, const upstream::OpenRouterDispatchInit&) { return rt::fulfilledTask(sseResponse({"data: {not json}\n\n", "data: [DONE]\n\n"})); };
  auto raw = routeBody(true); raw["stream"] = true; AWAIT(response, routeCall(true, raw, "vendor/alpha", &context, deps)); EXPECT_EQ(response.status(), 200); AWAIT(body, response.text()); EXPECT(rt::includes(body, "openrouter_upstream_stream_error")); EXPECT_EQ(state->streamTerminalType, "error"); EXPECT_EQ(state->failureKind, "malformed_event");
}
ASYNC_TEST("openrouter responses relays native events and records the terminal") {
  Scope isolation; co_await warm(); const auto state = telemetry::createResponseTelemetryState(); const auto context = streamUsageContext(state); routes::OpenRouterHandlerDeps deps;
  deps.fetchResponses = [](const json::object&, const upstream::OpenRouterDispatchInit&) { return rt::fulfilledTask(sseResponse({"event: response.created\ndata: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_1\"}}\n\n", "event: response.output_text.delta\ndata: {\"type\":\"response.output_text.delta\",\"delta\":\"ok\"}\n\n", "event: response.completed\ndata: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\",\"usage\":{\"input_tokens\":4,\"output_tokens\":1,\"total_tokens\":5}}}\n\n"})); };
  auto raw = routeBody(false, "vendor/beta"); raw["stream"] = true; AWAIT(response, routeCall(false, raw, "vendor/beta", &context, deps)); EXPECT_EQ(response.status(), 200); AWAIT(body, response.text()); EXPECT(rt::includes(body, "response.output_text.delta")); EXPECT(rt::includes(body, "response.completed")); EXPECT_EQ(state->streamTerminalType, "response.completed"); EXPECT_EQ(state->outputTokens, 1);
}
namespace {
  rt::Task<void> originalProviderFault(bool incomplete) {
    Scope isolation; co_await warm(); co_await rt::sleep(1); const auto kv = std::make_shared<counting::CountingKv>(); uos::kv::setKvForTest(kv); uos::provider::health::resetProviderHealthThrottleForTest();
    const auto type = std::string(incomplete ? "response.incomplete" : "response.failed");
    json::object payload{{"id", "resp_1"}, {"status", incomplete ? "incomplete" : "failed"}};
    if (incomplete) rt::objectDefine(payload, "incomplete_details", json::object{{"reason", "provider_overloaded"}});
    const json::object event{{"type", type}, {"response", payload}}; const auto frame = "event: " + type + "\ndata: " + rt::jsonStringify(event) + "\n\n";
    routes::OpenRouterHandlerDeps deps; deps.fetchResponses = [frame](const json::object&, const upstream::OpenRouterDispatchInit&) { return rt::fulfilledTask(sseResponse({frame})); };
    const auto state = telemetry::createResponseTelemetryState(); const auto context = streamUsageContext(state); auto raw = routeBody(false, "vendor/beta"); raw["stream"] = true;
    AWAIT(response, routeCall(false, raw, "vendor/beta", &context, deps)); EXPECT_EQ(response.status(), 200); AWAIT(body, response.text()); (void)body; co_await rt::sleep(1);
    AWAIT(health, uos::provider::health::getOpenRouterProviderHealth()); EXPECT_EQ(health.last_event, "upstream_error"); EXPECT_EQ(health.last_status, 200); EXPECT_EQ(health.state, "degraded"); co_await rt::sleep(1); uos::provider::health::resetProviderHealthThrottleForTest();
  }
}
ASYNC_TEST("openrouter responses records a failed terminal as upstream provider health error") { co_await originalProviderFault(false); }
ASYNC_TEST("openrouter responses records a provider-fault incomplete terminal as upstream provider health error") { co_await originalProviderFault(true); }
ASYNC_TEST("openrouter Claude caching keeps native request and response shapes on both streamed and buffered wires") {
  Scope isolation; co_await warmCacheCatalogue(); const std::vector<std::string> modelsToCheck{"~anthropic/claude-opus-latest", "anthropic/claude-sonnet-4"};
  for (int wire = 0; wire < 2; ++wire) for (const auto& model : modelsToCheck) for (int streaming = 0; streaming < 2; ++streaming) {
    const bool chat = wire == 0, stream = streaming != 0; const auto state = telemetry::createResponseTelemetryState(); const auto context = streamUsageContext(state); auto raw = routeBody(chat, model); raw["stream"] = stream; raw["prompt_cache_key"] = "stable-session-key";
    if (chat) raw["reasoning_effort"] = "max"; else { raw["instructions"] = "Stable developer instructions"; raw["reasoning"] = json::object{{"effort", "max"}}; }
    raw["tools"] = rt::jsonParse(R"([{"type":"function","name":"lookup","parameters":{"type":"object","properties":{"cache_control":{"type":"string"}}}}])");
    const auto original = rt::jsonStringify(raw); const auto usage = cacheUsageFor(chat, json::object{{"cached_tokens", 200}, {"cache_write_tokens", 300}}); const auto fixture = cacheResponseFor(chat, stream, usage); unsigned calls = 0;
    routes::OpenRouterRouteTransport transport = [&](const json::object& body, const upstream::OpenRouterDispatchInit&) { ++calls; auto expected = raw; expected["cache_control"] = json::object{{"type", "ephemeral"}}; if (chat && stream) expected["stream_options"] = json::object{{"include_usage", true}}; EXPECT(rt::deepEqual(body, expected)); EXPECT_EQ(rt::jsonStringify(raw), original); return rt::fulfilledTask(fixture.response); };
    const routes::OpenRouterHandlerDeps deps{transport, transport}; AWAIT(response, routeCall(chat, raw, model, &context, deps)); EXPECT_EQ(calls, 1U); EXPECT_EQ(response.status(), 200);
    if (stream) { AWAIT(body, response.text()); EXPECT(rt::includes(body, rt::jsonStringify(usage))); EXPECT(rt::includes(body, "cached reply")); } else { AWAIT(body, response.json()); EXPECT(rt::deepEqual(body, fixture.payload)); }
    EXPECT_EQ(state->promptCacheMode, "implicit"); EXPECT_EQ(state->reasoning, "max"); EXPECT(state->promptCacheKeyPresent); EXPECT_EQ(state->explicitBreakpointCount, 0); EXPECT_EQ(state->cachedInputTokens, 200); EXPECT_EQ(state->cacheWriteInputTokens, 300); EXPECT_EQ(state->usageTelemetryStatus, "reported");
  }
}
ASYNC_TEST("openrouter preserves explicit cache controls and records their mode and count") {
  Scope isolation; co_await warmCacheCatalogue();
  for (int wire = 0; wire < 2; ++wire) {
    const bool chat = wire == 0; const auto inputsKey = chat ? "messages" : "input"; const auto contentType = chat ? "text" : "input_text";
    struct Control { json::object body; std::string mode; double count; };
    const std::vector<Control> controls{
      {{{"cache_control", json::object{{"type", "ephemeral"}, {"ttl", "1h"}}}}, "implicit", 0},
      {{{inputsKey, json::array{json::object{{"role", "user"}, {"content", json::array{json::object{{"type", contentType}, {"text", "static prefix"}, {"cache_control", json::object{{"type", "ephemeral"}, {"ttl", "1h"}}}}}}}}}}, "explicit", 1},
      {{{inputsKey, json::array{json::object{{"role", "user"}, {"content", json::array{json::object{{"type", contentType}, {"text", "static prefix"}, {"prompt_cache_breakpoint", json::object{{"mode", "explicit"}}}}}}}}}}, "explicit", 1},
      {{{"tools", json::array{json::object{{"type", "function"}, {"name", "lookup"}, {"cache_control", json::object{{"type", "ephemeral"}}}}}}}, "explicit", 1},
      {{{"prompt_cache_options", json::object{{"mode", "explicit"}}}}, "explicit", 0}};
    for (const auto& control : controls) {
      const std::string model = "~anthropic/claude-opus-latest"; auto raw = routeBody(chat, model); rt::objectAssign(raw, control.body); const auto original = rt::jsonStringify(raw); const auto state = telemetry::createResponseTelemetryState(); const auto context = streamUsageContext(state);
      routes::OpenRouterRouteTransport transport = [&](const json::object& body, const upstream::OpenRouterDispatchInit&) { auto expected = raw; if (chat) expected["stream"] = false; EXPECT(rt::deepEqual(body, expected)); return rt::fulfilledTask(cacheResponseFor(chat, false, cacheUsageFor(chat)).response); };
      const routes::OpenRouterHandlerDeps deps{transport, transport}; AWAIT(response, routeCall(chat, raw, model, &context, deps)); AWAIT(body, response.json()); (void)body; EXPECT_EQ(rt::jsonStringify(raw), original); EXPECT_EQ(state->promptCacheMode, control.mode); EXPECT_EQ(state->explicitBreakpointCount, control.count); EXPECT(!state->promptCacheKeyPresent);
    }
  }
}
ASYNC_TEST("openrouter leaves other models unchanged and missing cache counters unknown") {
  Scope isolation; co_await warmCacheCatalogue();
  for (int wire = 0; wire < 2; ++wire) for (int streaming = 0; streaming < 2; ++streaming) for (int present = 0; present < 2; ++present) {
    const bool chat = wire == 0, stream = streaming != 0; auto raw = routeBody(chat); raw["stream"] = stream; const auto state = telemetry::createResponseTelemetryState(); const auto context = streamUsageContext(state);
    std::optional<json::object> details; if (present) details = json::object{{"cached_tokens", 0}};
    routes::OpenRouterRouteTransport transport = [&](const json::object& body, const upstream::OpenRouterDispatchInit&) { auto expected = raw; if (chat && stream) expected["stream_options"] = json::object{{"include_usage", true}}; EXPECT(rt::deepEqual(body, expected)); return rt::fulfilledTask(cacheResponseFor(chat, stream, cacheUsageFor(chat, details)).response); };
    const routes::OpenRouterHandlerDeps deps{transport, transport}; AWAIT(response, routeCall(chat, raw, "vendor/alpha", &context, deps)); AWAIT(body, response.text()); (void)body; EXPECT_EQ(state->promptCacheMode, "unspecified"); EXPECT_EQ(state->cachedInputTokens, present ? std::optional<double>(0) : std::nullopt); EXPECT(!state->cacheWriteInputTokens); EXPECT_EQ(state->usageTelemetryStatus, present ? "reported" : "partial");
  }
}
