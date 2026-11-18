// Links: tests/gemini_subscription_test.cpp src/admin/api_key_audit.cpp src/admin/api_key_mutations.cpp src/admin/api_keys.cpp src/admin/capacity_observation_sync.cpp src/admin/chart_source_proxy.cpp src/admin/claude_code.cpp
// src/admin/codex.cpp src/admin/codex_reset.cpp src/admin/defaults.cpp src/admin/error_log.cpp src/admin/gemini_quota.cpp src/admin/index.cpp src/admin/kernel.cpp src/admin/sentinel_incident.cpp src/agent_messages.cpp src/analytics.cpp
// src/api_key_policy.cpp src/api_keys.cpp src/auth/index.cpp src/auth/local_admin.cpp src/auth/local_development_key.cpp src/auth/passkeys.cpp src/auth/passport.cpp src/auth/passport_sessions.cpp src/auth/relay.cpp
// src/authority/contracts.cpp src/authority/native_api_key.cpp src/authority/native_api_key_audit.cpp src/authority/native_api_key_policy.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp
// src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/authority/native_contracts.cpp src/authority/native_kernel.cpp
// src/authority/native_kernel_contracts.cpp src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp src/authority/native_paid_policy.cpp src/bounded_response_body.cpp src/cache/kv_migration_base.cpp
// src/cache/kv_migration_paid_fallback.cpp src/cache/kv_migration_usage.cpp src/cache/kv_migration_validation.cpp src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp
// src/cache/scope_experiment.cpp src/cache/scope_experiment_model.cpp src/cache/scope_experiment_targets.cpp src/cache/scope_targets.cpp src/cache/telemetry_gate.cpp src/catalog/index.cpp src/catalog/models.cpp src/catalog/promotion.cpp
// src/catalog/store.cpp src/catalog/types.cpp src/chat/completions.cpp src/chat/envelope.cpp src/chat/stream_translation.cpp src/codex/account_routing.cpp src/codex/auth.cpp src/codex/auth_refresh.cpp src/codex/banked_reset.cpp
// src/codex/banked_reset_claim.cpp src/codex/banked_reset_pool.cpp src/codex/banked_reset_provider.cpp src/codex/banked_reset_submission.cpp src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp
// src/codex/external_auth.cpp src/codex/index.cpp src/codex/manual_reset.cpp src/codex/manual_reset_state.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp src/codex/quota.cpp
// src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/responses_error.cpp src/codex/responses_operations.cpp src/codex/responses_runners.cpp src/codex/responses_state.cpp src/codex/routing_evaluation.cpp
// src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/codex/routing_state.cpp src/codex/supervisor.cpp src/codex/supervisor_brief.cpp src/codex/supervisor_config.cpp src/codex/supervisor_follow.cpp
// src/codex/supervisor_inventory.cpp src/codex/supervisor_log.cpp src/codex/supervisor_secret.cpp src/codex/supervisor_transport.cpp src/config.cpp src/debug_routing.cpp src/decisions/handlers.cpp src/deepseek/chat_projection.cpp
// src/deepseek/context_admission_failure.cpp src/deepseek/flash_aliases.cpp src/deepseek/forwarded_payload_policy.cpp src/deepseek/handlers.cpp src/deepseek/index.cpp src/deepseek/responses.cpp src/deepseek/responses_payload.cpp
// src/deepseek/responses_stream.cpp src/deepseek/stream.cpp src/deepseek/waterfall.cpp src/deepseek/waterfall_handler.cpp src/defaults.cpp src/embeddings/handlers.cpp src/embeddings/jobs.cpp src/embeddings/ledger.cpp
// src/embeddings/voyage.cpp src/handler/admission.cpp src/handler/http.cpp src/handler/index.cpp src/handler/terminal_log.cpp src/handler/terminal_route.cpp src/health.cpp src/http.cpp src/images.cpp src/inference_admission.cpp
// src/inference_deadline.cpp src/input_normalization.cpp src/kernel/attestation.cpp src/kernel/policy_queue.cpp src/kernel/quota_reservations.cpp src/kernel/quota_reserve.cpp src/kernel/quota_v2.cpp src/kernel/quota_write.cpp
// src/kernel/usage.cpp src/kv.cpp src/kv_conversion/profile_json.cpp src/lib/jev_compaction/codex_items.cpp src/lib/jev_compaction/compact.cpp src/lib/jev_compaction/request.cpp src/lib/jev_compaction/state.cpp
// src/lib/jev_compaction/types.cpp src/live/calls.cpp src/live/handler.cpp src/live/routes.cpp src/live/sideband.cpp src/live/upstream.cpp src/metered_quota.cpp src/models/catalog.cpp src/models/codex_models.cpp
// src/models/codex_models_availability.cpp src/models/codex_models_fetch.cpp src/models/codex_models_whitelist.cpp src/models/metadata.cpp src/models/openrouter_models.cpp src/openai.cpp src/openai_schema.cpp src/openai_telemetry.cpp
// src/optional_telemetry_queue.cpp src/paid_fallback/claude_code_route.cpp src/paid_fallback/health.cpp src/paid_fallback/index.cpp src/paid_fallback/ledger_admission.cpp src/paid_fallback/ledger_backfill.cpp
// src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/paid_fallback/removed_provider.cpp src/paid_fallback/rollups.cpp src/paid_fallback/routing.cpp
// src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp src/provider/anthropic_responses.cpp src/provider/capacity.cpp
// src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp src/provider/cerebras.cpp
// src/provider/cerebras_handlers.cpp src/provider/cerebras_rate_limits.cpp src/provider/cerebras_responses.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code.cpp
// src/provider/claude_code_access.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_models.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp
// src/provider/gemini.cpp src/provider/health.cpp src/provider/lithos.cpp src/provider/lithos_handlers.cpp src/provider/lithos_models.cpp src/provider/lithos_rate_limits.cpp src/provider/lithos_streams.cpp src/provider/metered.cpp
// src/provider/openrouter.cpp src/provider/openrouter_handlers.cpp src/provider/openrouter_request_controls.cpp src/provider/openrouter_streams.cpp src/provider/openrouter_web_tools.cpp src/provider/presentation.cpp
// src/provider/removed_provider_circuit.cpp src/provider/removed_provider_telemetry.cpp src/provider/selection.cpp src/provider/stream_relay.cpp src/provider/surplus.cpp src/provider/surplus_claude_chat_request.cpp
// src/provider/surplus_claude_request.cpp src/quota_projection.cpp src/recent_model_context.cpp src/release.cpp src/request.cpp src/request_policy.cpp src/responses_attempts.cpp src/responses_buffered.cpp src/responses_failover_stream.cpp
// src/responses_handler.cpp src/responses_native_headers.cpp src/responses_request.cpp src/responses_stream.cpp src/runtime/http.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp
// src/runtime/http_upgrade.cpp src/runtime/websocket.cpp src/runtime_config.cpp src/sentinel/incident_outbox.cpp src/sentinel/replay_admin.cpp src/sentinel/replay_capture.cpp src/sentinel/replay_envelope.cpp src/sentinel/replay_limits.cpp
// src/sentinel/replay_model.cpp src/sentinel/replay_observation.cpp src/sentinel/replay_read.cpp src/sentinel/replay_retention.cpp src/sentinel/replay_retention_bootstrap.cpp src/sentinel/replay_retention_metadata.cpp
// src/sentinel/replay_retention_schema.cpp src/sentinel/replay_store.cpp src/sentinel/upstream_capture.cpp src/static.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp src/storage/native_addon.cpp src/storage/native_kv.cpp
// src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/systemone/handlers.cpp src/types.cpp src/upstream_wire.cpp src/utils.cpp
#include "uos/admin/gemini_quota.hpp"
#include "uos/config.hpp"
#include "uos/handler/index.hpp"
#include "uos/kv.hpp"
#include "uos/input_normalization.hpp"
#include "uos/provider/gemini.hpp"
#include "uos/provider/presentation.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/test_async.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <variant>

namespace rt = uos::rt;
namespace json = uos::json;
namespace gemini = uos::provider::gemini;
namespace quota = uos::admin::gemini_quota;
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)

namespace {
  json::object usageResult() {
    return json::object{{"conversation_id", "80d7987f-f2e8-4ecf-a410-1d34184732a7"}, {"status", "SUCCESS"}, {"response", "GEMINI_OK"},
      {"usage", json::object{{"input_tokens", 8692}, {"output_tokens", 7}, {"thinking_tokens", 0}, {"cache_read_tokens", 0}, {"total_tokens", 8699}}}};
  }

  gemini::GeminiCliRunner turnRunner() {
    return [](const gemini::GeminiCliOptions& command) -> rt::Task<gemini::GeminiCliTurn> {
      const bool usage = command.prompt == "/usage";
      EXPECT_EQ(command.mode, usage ? std::string() : std::string("plan"));
      EXPECT_EQ(command.disableSlashCommands, !usage);
      EXPECT_EQ(command.streamJson, !usage);
      gemini::GeminiCliTurn turn;
      turn.response = "GEMINI_OK";
      turn.conversationId = "80d7987f-f2e8-4ecf-a410-1d34184732a7";
      turn.usage = json::object{{"input_tokens", 8692}, {"output_tokens", 7}, {"cache_read_tokens", 0}, {"total_tokens", 8699}};
      turn.rawResult = usageResult();
      co_return turn;
    };
  }

  json::value usageCommand() {
    return rt::jsonParse(R"({"command":{"name":"usage","data":{"groups":[{"name":"Gemini Models","buckets":[{"id":"gemini-weekly","remaining_fraction":0.986355185508728,"reset_time":"2026-11-25T19:04:49Z"}]},{"name":"Claude and GPT models","buckets":[{"id":"3p-weekly","remaining_fraction":1,"reset_time":"2026-11-25T19:38:03Z"}]}]}}})");
  }
}

TEST("Gemini model ids and subscription token accounting are explicit") {
  EXPECT_EQ(gemini::geminiUpstreamModelFor("gemini-3.8-flash-low").value_or(""), "gemini-3.8-flash-low");
  EXPECT_EQ(gemini::geminiUpstreamModelFor("google/gemini-3.8-flash-low").has_value(), false);
  const auto usage = gemini::geminiUsageTokens(json::object{{"input_tokens", 100}, {"output_tokens", 5}, {"cache_read_tokens", 80}, {"total_tokens", 105}});
  ASSERT(usage.has_value());
  EXPECT_EQ(usage->inputTokens, std::optional<double>(100));
  EXPECT_EQ(usage->cachedInputTokens, std::optional<double>(80));
  EXPECT_EQ(usage->outputTokens, std::optional<double>(5));
  EXPECT_EQ(usage->totalTokens, std::optional<double>(105));
  EXPECT_EQ(usage->status, "reported");
}

TEST("Gemini is an enabled subscription provider with both OpenAI text wires") {
  EXPECT(uos::provider::selection::isProviderEnabled("gemini", std::nullopt));
  EXPECT(uos::provider::selection::isProviderEnabled("gemini", uos::provider::selection::ProviderSelection{.provider_ids = {"gemini"}, .updated_at_ms = 1}));
  const auto& presentation = *uos::provider::presentation::PROVIDER_PRESENTATION.get("gemini");
  EXPECT_EQ(presentation.label, "Gemini");
  EXPECT_EQ(presentation.tier, "subscription");
  EXPECT_EQ(presentation.health_key, "gemini");
  EXPECT_EQ(presentation.endpoints, (std::vector<std::string>{"/v1/responses", "/v1/chat/completions"}));
}

ASYNC_TEST("Gemini CLI /usage projects the exact refreshable subscription buckets") {
  const double observed = 1795031902000;
  const auto snapshot = quota::projectUsageCommand(usageCommand(), observed);
  ASSERT(snapshot.has_value());
  EXPECT_EQ(snapshot->status, "observed");
  EXPECT_EQ(snapshot->observed_at_ms, observed);
  EXPECT_EQ(snapshot->plan_tier, std::optional<std::string>("Antigravity subscription"));
  ASSERT(snapshot->quota.get("gemini-weekly"));
  EXPECT_EQ(snapshot->quota.get("gemini-weekly")->remaining_fraction, std::optional<double>(0.986355185508728));
  EXPECT_EQ(snapshot->quota.get("gemini-weekly")->reset_time, std::optional<std::string>("2026-11-25T19:04:49.000Z"));
  ASSERT(snapshot->quota.get("3p-weekly"));
  EXPECT_EQ(snapshot->quota.get("3p-weekly")->remaining_fraction, std::optional<double>(1));
  co_return;
}

ASYNC_TEST("Gemini quota refresh writes a private snapshot through the CLI usage command") {
  const std::string root = rt::fs::mkdtempSync("/tmp/gemini-quota-");
  const std::string data = rt::path::join(root, ".data");
  EXPECT_EQ(rt::path::isAbsolute(data), true);
  EXPECT_EQ(std::filesystem::path(data).filename(), std::filesystem::path(".data"));
  rt::fs::mkdirSync(data, {.mode = 0700});
  auto cleanup = rt::scopeExit([root] { rt::fs::rmSync(root, {.recursive = true, .force = true}); });
  quota::GeminiQuotaOptions options;
  options.runner = [](const gemini::GeminiCliOptions& command) -> rt::Task<gemini::GeminiCliTurn> {
    EXPECT_EQ(command.prompt, "/usage");
    EXPECT_EQ(command.disableSlashCommands, false);
    EXPECT_EQ(command.mode, "");
    gemini::GeminiCliTurn turn;
    turn.rawResult = usageCommand().as_object();
    co_return turn;
  };
  const double now = 1795031902000;
  AWAIT(snapshot, quota::refreshSnapshot(data, options, now));
  EXPECT_EQ(snapshot.status, "observed");
  AWAIT(loaded, quota::readSnapshot(data, now + 1000));
  EXPECT_EQ(loaded.observed_at_ms, now);
  EXPECT_EQ(loaded.quota.size(), std::size_t(2));
  const auto info = rt::fs::lstatSync(rt::path::join(data, std::string(quota::GEMINI_QUOTA_FILE)));
  EXPECT_EQ(info.mode & 07777, 0600u);
  co_return;
}

ASYNC_TEST("Gemini explicit quota refresh bypasses a fresh snapshot but normal reads remain coalesced") {
  const std::string root = rt::fs::mkdtempSync("/tmp/gemini-quota-force-");
  const std::string data = rt::path::join(root, ".data");
  rt::fs::mkdirSync(data, {.mode = 0700});
  auto cleanup = rt::scopeExit([root] { rt::fs::rmSync(root, {.recursive = true, .force = true}); });
  int calls = 0;
  quota::GeminiQuotaOptions options;
  options.runner = [&calls](const gemini::GeminiCliOptions&) -> rt::Task<gemini::GeminiCliTurn> {
    ++calls;
    gemini::GeminiCliTurn turn;
    turn.rawResult = usageCommand().as_object();
    co_return turn;
  };
  const double now = 1795031902000;
  AWAIT(first, quota::refreshSnapshot(data, options, now));
  AWAIT(cached, quota::refreshSnapshot(data, options, now + 1000));
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(cached.observed_at_ms, first.observed_at_ms);
  options.force_refresh = true;
  AWAIT(forced, quota::refreshSnapshot(data, options, now + 2000));
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(forced.observed_at_ms, now + 2000);
  const auto publicSnapshot = quota::snapshotJson(forced, now + 2000);
  EXPECT_EQ(rt::getBool(publicSnapshot, "refresh_supported"), std::optional<bool>(true));
  co_return;
}

ASYNC_TEST("Gemini rejects caller tool declarations before dispatching the subscription CLI") {
  rt::RequestInit init;
  init.method = "POST";
  const rt::Request request("https://ai.ubq.fi/v1/responses", init);
  bool dispatched = false;
  gemini::GeminiHandlerOptions options{.refreshQuota = false};
  options.runner = [&dispatched](const gemini::GeminiCliOptions&) -> rt::Task<gemini::GeminiCliTurn> {
    dispatched = true;
    co_return gemini::GeminiCliTurn{};
  };
  const json::object raw{{"model", "gemini-3.8-flash-low"}, {"input", "Use the supplied tool."},
    {"tools", json::array{json::object{{"type", "function"}, {"name", "fixture"}}}}};
  AWAIT(response, gemini::handleGeminiResponses(request, raw, "gemini-3.8-flash-low", nullptr, options));
  EXPECT_EQ(response.status(), 400);
  EXPECT_EQ(dispatched, false);
  AWAIT(body, response.json());
  const auto* error = rt::getObject(body, "error");
  ASSERT(error);
  EXPECT_EQ(rt::getString(*error, "code"), std::optional<std::string_view>("gemini_tools_unsupported"));
  co_return;
}

ASYNC_TEST("authenticated Gemini Responses route returns the provider answer and measured usage") {
  rt::RequestInit init;
  init.method = "POST";
  const rt::Request request("https://ai.ubq.fi/v1/responses", init);
  std::optional<uos::openai_telemetry::UsageTokens> terminal;
  uos::openai_telemetry::UsageContext context;
  context.responseTelemetry = uos::openai_telemetry::createResponseTelemetryState();
  context.onTerminalUsage = [&terminal](std::optional<uos::openai_telemetry::UsageTokens> usage, bool) { terminal = std::move(usage); };
  gemini::GeminiHandlerOptions options{.runner = turnRunner(), .refreshQuota = false};
  const json::object raw{{"model", "gemini-3.8-flash-low"}, {"input", "Reply exactly GEMINI_OK."}};
  AWAIT(response, gemini::handleGeminiResponses(request, raw, "gemini-3.8-flash-low", &context, options));
  EXPECT_EQ(response.status(), 200);
  EXPECT_EQ(response.headers().get("x-uos-upstream"), std::optional<std::string>("gemini"));
  AWAIT(body, response.json());
  EXPECT_EQ(rt::getString(body, "model").value_or(""), "gemini-3.8-flash-low");
  EXPECT_EQ(rt::getString(body, "status").value_or(""), "completed");
  ASSERT(terminal.has_value());
  EXPECT_EQ(terminal->inputTokens, std::optional<double>(8692));
  EXPECT_EQ(terminal->outputTokens, std::optional<double>(7));
  EXPECT_EQ(context.responseTelemetry->provider, std::optional<std::string>("gemini"));
  EXPECT_EQ(context.responseTelemetry->providerRequestId, std::optional<std::string>("80d7987f-f2e8-4ecf-a410-1d34184732a7"));
  co_return;
}

ASYNC_TEST("Gemini Chat Completions streams the same provider answer and usage") {
  rt::RequestInit init;
  init.method = "POST";
  const rt::Request request("https://ai.ubq.fi/v1/chat/completions", init);
  gemini::GeminiHandlerOptions options{.runner = turnRunner(), .refreshQuota = false};
  const json::object raw{{"model", "gemini-3.8-flash-low"}, {"messages", json::array{json::object{{"role", "user"}, {"content", "Reply exactly GEMINI_OK."}}}},
    {"stream", true}, {"stream_options", json::object{{"include_usage", true}}}};
  AWAIT(response, gemini::handleGeminiChatCompletions(request, raw, "gemini-3.8-flash-low", nullptr, options));
  EXPECT_EQ(response.status(), 200);
  AWAIT(body, response.text());
  EXPECT(body.find("GEMINI_OK") != std::string::npos);
  EXPECT(body.find("\"usage\":{\"prompt_tokens\":8692") != std::string::npos);
  EXPECT(body.ends_with("data: [DONE]\n\n"));
  co_return;
}

ASYNC_TEST("authenticated gateway request routes to Gemini and records the subscription spend") {
  const auto savedConfig = uos::config::config;
  uos::config::config.authTokens = {"fixture-gateway-key"};
  uos::config::config.adminTokens.clear();
  uos::config::config.codexAuthJsonB64.clear();
  uos::kv::setKvForTest(std::make_shared<rt::MemoryKv>(rt::dateNow));
  gemini::setGeminiCliRunnerForTest(turnRunner());
  auto restore = rt::scopeExit([savedConfig] {
    gemini::setGeminiCliRunnerForTest({});
    uos::kv::setKvForTest(nullptr);
    uos::config::config = savedConfig;
  });
  const json::object body{{"model", "gemini-3.8-flash-low"}, {"input", "Reply exactly GEMINI_OK."}, {"stream", false}};
  rt::RequestInit init;
  init.method = "POST";
  init.headers = rt::Headers{{"Authorization", "Bearer fixture-gateway-key"}, {"Content-Type", "application/json"}};
  init.body = rt::jsonStringify(body);
  const rt::Request request("https://ai.ubq.fi/v1/responses", init);
  AWAIT(response, uos::handler::index::handler(request));
  EXPECT_EQ(response.status(), 200);
  EXPECT_EQ(response.headers().get("x-uos-upstream"), std::optional<std::string>("gemini"));
  const auto telemetry = uos::openai_telemetry::getResponseTelemetry(response);
  ASSERT(telemetry.has_value());
  EXPECT_EQ(telemetry->provider, "gemini");
  EXPECT_EQ(telemetry->totalTokens, std::optional<double>(8699));
  EXPECT_EQ(telemetry->completed, true);
  co_return;
}

ASYNC_TEST("manual live Antigravity acceptance uses an authenticated gateway request and proves quota spend") {
  if (rt::env::get("GEMINI_LIVE_ACCEPTANCE") != std::optional<std::string>("1")) co_return;
  const std::string originalDirectory = rt::process::cwd();
  const std::string root = rt::fs::mkdtempSync("/tmp/gemini-live-");
  const std::string data = rt::path::join(root, ".data");
  rt::fs::mkdirSync(data, {.mode = 0700});
  const auto savedConfig = uos::config::config;
  uos::kv::setKvForTest(std::make_shared<rt::MemoryKv>(rt::dateNow));
  gemini::setGeminiCliRunnerForTest({});
  auto restore = rt::scopeExit([originalDirectory, root, savedConfig] {
    gemini::setGeminiCliRunnerForTest({});
    uos::kv::setKvForTest(nullptr);
    uos::config::config = savedConfig;
    if (::chdir(originalDirectory.c_str()) != 0) std::abort();
    rt::fs::rmSync(root, {.recursive = true, .force = true});
  });
  ASSERT(::chdir(root.c_str()) == 0);
  quota::GeminiQuotaOptions quotaOptions;
  AWAIT(before, quota::refreshSnapshot(data, quotaOptions, rt::dateNow()));
  ASSERT(before.quota.get("gemini-weekly"));
  const auto beforeFraction = *before.quota.get("gemini-weekly")->remaining_fraction;
  uos::config::config.authTokens = {"fixture-live-gateway-key"};
  uos::config::config.adminTokens.clear();
  uos::config::config.codexAuthJsonB64.clear();
  const json::object body{{"model", "gemini-3.8-flash-low"}, {"input", "Reply exactly GATEWAY_CANDIDATE_OK and do not use tools."}, {"stream", false}};
  rt::RequestInit init;
  init.method = "POST";
  init.headers = rt::Headers{{"Authorization", "Bearer fixture-live-gateway-key"}, {"Content-Type", "application/json"}};
  init.body = rt::jsonStringify(body);
  const rt::Request request("https://ai.ubq.fi/v1/responses", init);
  AWAIT(response, uos::handler::index::handler(request));
  EXPECT_EQ(response.status(), 200);
  EXPECT_EQ(response.headers().get("x-uos-upstream"), std::optional<std::string>("gemini"));
  const auto telemetry = uos::openai_telemetry::getResponseTelemetry(response);
  ASSERT(telemetry.has_value());
  EXPECT_EQ(telemetry->provider, "gemini");
  ASSERT(telemetry->totalTokens.has_value());
  EXPECT(*telemetry->totalTokens > 0);
  AWAIT(serialized, response.text());
  EXPECT(serialized.find("GATEWAY_CANDIDATE_OK") != std::string::npos);
  const rt::AbortSignal pause = rt::AbortSignal::timeout(11'000);
  try {
    auto sleeping = rt::sleep(11'000, pause);
    co_await std::move(sleeping);
  } catch (...) {
  }
  AWAIT(after, quota::refreshSnapshot(data, quotaOptions, rt::dateNow()));
  ASSERT(after.quota.get("gemini-weekly"));
  const auto afterFraction = *after.quota.get("gemini-weekly")->remaining_fraction;
  EXPECT(afterFraction < beforeFraction);
  const json::object receipt{{"status", "passed"}, {"request_status", 200}, {"provider", telemetry->provider},
    {"provider_request_id", telemetry->providerRequestId ? json::value(*telemetry->providerRequestId) : json::value(nullptr)},
    {"input_tokens", telemetry->inputTokens ? json::value(*telemetry->inputTokens) : json::value(nullptr)},
    {"output_tokens", telemetry->outputTokens ? json::value(*telemetry->outputTokens) : json::value(nullptr)},
    {"total_tokens", telemetry->totalTokens ? json::value(*telemetry->totalTokens) : json::value(nullptr)},
    {"gemini_weekly_before", beforeFraction}, {"gemini_weekly_after", afterFraction}, {"observed_at_ms", rt::makeNumber(after.observed_at_ms)}};
  rt::fs::writeFileSync("/tmp/gemini-live-acceptance.json", rt::jsonStringify(receipt) + "\n", {.mode = 0600});
  co_return;
}
