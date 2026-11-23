// Port of all six original tests/passkeys-part2.test.ts registrations, in source order.
// Links: tests/passkeys_part2_test.cpp src/auth/passkeys.cpp tests/helpers/passkeys_harness.cpp src/utils.cpp src/config.cpp src/release.cpp src/handler/index.cpp src/admin/claude_code.cpp src/admin/api_keys.cpp src/static.cpp src/health.cpp src/admin/codex.cpp src/models/openrouter_models.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
// src/codex/auth.cpp src/types.cpp src/admin/codex_reset.cpp src/runtime_config.cpp src/live/handler.cpp src/admin/kernel.cpp src/handler/http.cpp src/provider/capacity_parse.cpp src/provider/presentation.cpp src/request.cpp src/http.cpp src/quota_projection.cpp src/api_key_policy.cpp src/provider/health.cpp
// src/catalog/store.cpp src/provider/claude_code_accounts.cpp src/catalog/types.cpp src/paid_fallback/ledger_state.cpp src/authority/native_paid_contracts.cpp src/codex/reset_settings.cpp src/provider/selection.cpp src/paid_fallback/ledger_backfill.cpp src/codex/manual_reset.cpp src/metered_quota.cpp
// src/codex/banked_reset.cpp src/models/catalog.cpp src/provider/claude_code_quota.cpp src/live/routes.cpp src/input_normalization.cpp src/api_keys.cpp src/paid_fallback/ledger_settlement.cpp src/live/sideband.cpp src/deepseek/waterfall.cpp src/models/codex_models_whitelist.cpp src/authority/native_paid.cpp
// src/kernel/quota_write.cpp src/admin/api_key_audit.cpp src/authority/native_codex_capacity.cpp src/provider/cerebras.cpp src/kernel/usage.cpp src/kernel/quota_v2.cpp src/provider/claude_code_credentials.cpp src/provider/metered.cpp src/codex/account_routing.cpp src/authority/native_kernel.cpp
// src/authority/native_api_key.cpp src/authority/native_codex_mutations.cpp src/provider/surplus.cpp src/codex/routing_serial.cpp src/provider/claude_code_models.cpp src/codex/overage_settings.cpp src/models/codex_models.cpp src/authority/native_api_key_policy.cpp src/codex/capacity_routing.cpp
// src/auth/passport.cpp src/provider/lithos.cpp src/codex/banked_reset_pool.cpp src/request_policy.cpp src/auth/index.cpp src/authority/native_codex.cpp src/provider/anthropic_chat_responses.cpp src/codex/native_auth.cpp src/codex/external_auth.cpp src/codex/routing_mutations.cpp
// src/kernel/quota_reservations.cpp src/codex/banked_reset_claim.cpp src/authority/native_codex_contracts.cpp src/agent_messages.cpp src/admin/api_key_mutations.cpp src/provider/surplus_claude_request.cpp src/paid_fallback/ledger_admission.cpp src/models/metadata.cpp src/auth/passport_sessions.cpp
// src/codex/experiment.cpp src/authority/native_paid_policy.cpp src/provider/openrouter.cpp src/codex/auth_refresh.cpp src/admin/defaults.cpp src/live/upstream.cpp src/codex/banked_reset_provider.cpp src/authority/native_codex_evaluation.cpp src/kernel/attestation.cpp src/models/codex_models_fetch.cpp
// src/provider/claude_code_access.cpp src/paid_fallback/ledger_stats.cpp src/paid_fallback/index.cpp src/provider/claude_code.cpp src/codex/rate_limit_429.cpp src/codex/dispatch.cpp src/cache/kv_migration_base.cpp src/codex/routing_evaluation.cpp src/provider/anthropic_chat_stream.cpp
// src/cache/prompt_analytics_read.cpp src/debug_routing.cpp src/cache/scope_experiment_targets.cpp src/authority/native_codex_429.cpp src/cache/prompt_analytics_core.cpp src/cache/scope_experiment_model.cpp src/codex/quota.cpp src/live/calls.cpp src/cache/telemetry_gate.cpp
// src/authority/native_api_key_audit.cpp src/provider/cerebras_rate_limits.cpp src/codex/banked_reset_submission.cpp src/admin/chart_source_proxy.cpp src/kv_conversion/profile_json.cpp src/provider/capacity.cpp src/auth/relay.cpp src/provider/capacity_history.cpp src/kv.cpp
// src/provider/anthropic_responses.cpp src/provider/capacity_analytics.cpp src/authority/native_kernel_contracts.cpp src/provider/anthropic_response_wire.cpp src/handler/terminal_route.cpp src/admin/capacity_observation_sync.cpp src/codex/supervisor_brief.cpp src/codex/supervisor.cpp src/catalog/index.cpp
// src/defaults.cpp src/systemone/handlers.cpp src/kernel/policy_queue.cpp src/openai_telemetry.cpp src/provider/capacity_rollups.cpp src/codex/routing_state.cpp src/provider/lithos_rate_limits.cpp src/provider/lithos_models.cpp src/cache/kv_migration_validation.cpp src/responses_failover_stream.cpp
// src/catalog/models.cpp src/sentinel/replay_observation.cpp src/sentinel/replay_admin.cpp src/handler/terminal_log.cpp src/recent_model_context.cpp src/auth/local_admin.cpp src/provider/surplus_claude_chat_request.cpp src/deepseek/index.cpp src/sentinel/replay_model.cpp
// src/models/codex_models_availability.cpp src/codex/supervisor_config.cpp src/responses_stream.cpp src/admin/error_log.cpp src/cache/kv_migration_paid_fallback.cpp src/sentinel/replay_read.cpp src/runtime/websocket.cpp src/admin/sentinel_incident.cpp src/chat/envelope.cpp src/codex/manual_reset_state.cpp
// src/auth/local_development_key.cpp src/cache/scope_experiment.cpp src/embeddings/jobs.cpp src/cache/scope_targets.cpp src/sentinel/replay_store.cpp src/openai.cpp src/codex/supervisor_secret.cpp src/provider/openrouter_handlers.cpp src/sentinel/replay_retention.cpp src/provider/claude_code_usage_schema.cpp
// src/provider/capacity_events.cpp src/cache/kv_migration_usage.cpp src/paid_fallback/rollups.cpp src/sentinel/replay_retention_schema.cpp src/codex/supervisor_follow.cpp src/embeddings/ledger.cpp src/sentinel/upstream_capture.cpp src/sentinel/incident_outbox.cpp src/paid_fallback/routing.cpp
// src/paid_fallback/health.cpp src/embeddings/voyage.cpp src/sentinel/replay_envelope.cpp src/codex/supervisor_log.cpp src/codex/native_auth_transport.cpp src/upstream_wire.cpp src/storage/kv.cpp src/provider/openrouter_streams.cpp src/embeddings/handlers.cpp src/responses_handler.cpp
// src/provider/capacity_contract.cpp src/codex/supervisor_transport.cpp src/decisions/handlers.cpp src/provider/claude_capacity_history.cpp src/responses_attempts.cpp src/handler/admission.cpp src/provider/anthropic_replay.cpp src/analytics.cpp src/provider/claude_capacity_rollups.cpp
// src/bounded_response_body.cpp src/codex/supervisor_inventory.cpp src/provider/openrouter_web_tools.cpp src/images.cpp src/sentinel/replay_retention_bootstrap.cpp src/responses_native_headers.cpp src/paid_fallback/removed_provider.cpp src/openai_schema.cpp src/provider/cerebras_handlers.cpp
// src/deepseek/flash_aliases.cpp src/paid_fallback/claude_code_route.cpp src/sentinel/replay_limits.cpp src/chat/stream_translation.cpp src/provider/removed_provider_circuit.cpp src/inference_deadline.cpp src/runtime/http_tasks.cpp src/sentinel/replay_retention_metadata.cpp
// src/provider/cerebras_responses.cpp src/responses_request.cpp src/deepseek/responses_payload.cpp src/provider/openrouter_request_controls.cpp src/provider/removed_provider_telemetry.cpp src/kernel/quota_reserve.cpp src/deepseek/responses.cpp src/catalog/promotion.cpp src/inference_admission.cpp
// src/provider/stream_relay.cpp src/chat/completions.cpp src/codex/responses_error.cpp src/runtime/http_early_sse.cpp src/deepseek/responses_stream.cpp src/provider/lithos_handlers.cpp src/cache/prompt_analytics_queue.cpp src/storage/native_kv.cpp src/deepseek/handlers.cpp
// src/codex/index.cpp src/optional_telemetry_queue.cpp src/deepseek/context_admission_failure.cpp src/storage/sqlite_readonly.cpp src/provider/lithos_streams.cpp src/lib/jev_compaction/codex_items.cpp src/responses_buffered.cpp src/deepseek/waterfall_handler.cpp src/deepseek/stream.cpp
// src/codex/responses_state.cpp src/deepseek/chat_projection.cpp src/deepseek/forwarded_payload_policy.cpp src/codex/responses_runners.cpp src/codex/responses_operations.cpp src/lib/jev_compaction/state.cpp src/lib/jev_compaction/compact.cpp src/lib/jev_compaction/types.cpp src/lib/jev_compaction/request.cpp
#include "tests/helpers/passkeys_harness.hpp"
#include "tests/helpers/responses_conformance_client.hpp"
#include "uos/config.hpp"
#include "uos/handler/index.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <string>
#include <vector>

#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace harness = uos::tests::helpers::passkeys_harness;
  namespace passkeys = uos::auth::passkeys;
  using uos::handler::index::handler;
  std::vector<std::pair<std::string, std::string>> isolatedEnvironment() {
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto* key : {"HOME", "TMPDIR"}) if (const auto value = rt::env::get(key)) result.emplace_back(key, *value);
    return result;
  }
  struct Fixture {
    uos::config::Config previous = uos::config::config;
    rt::env::ScopedEnvironment environment{isolatedEnvironment()};
    Fixture() { uos::config::config.codexAuthJsonB64.clear(); harness::resetHarness(); uos::tests::helpers::responses_conformance_client::stageGatewayAssets(); }
    ~Fixture() { harness::cleanupHarness(); uos::config::config = previous; }
  };
  rt::Request request(const std::string& path, rt::Headers headers, const std::string& method = "GET", std::optional<json::value> body = std::nullopt) {
    rt::RequestInit init;
    init.method = method;
    init.headers = std::move(headers);
    if (body) { init.headers->set("Content-Type", "application/json"); init.body = rt::BodyInit(rt::jsonStringify(*body)); }
    return rt::Request("https://ai.ubq.fi" + path, std::move(init));
  }
  std::string cookie(const std::string& token) { return std::string(passkeys::PASSKEY_RELAY_COOKIE_NAME) + "=" + rt::encodeURIComponent(token); }
  const json::value& member(const json::value& value, std::string_view key) { const auto* found = rt::get(value, key); ASSERT(found); return *found; }
}

ASYNC_TEST("allowlisted GitHub client bearers keep precedence over passkey cookies") {
  Fixture fixture;
  const auto seeded = harness::seedPasskeySession("uos_ai_session_client_cookie");
  const std::string githubToken = "ghp_allowlisted_client_token_1234567890abcdefghijklmnopqrstuvwxyz";
  uos::config::config.authTokens.insert(githubToken);
  AWAIT(response, harness::handleV1Auth(request("/uos/auth", rt::Headers{{"Authorization", "Bearer " + githubToken}, {"Cookie", cookie(seeded.token)}})));
  EXPECT_EQ(response.status(), 200);
  AWAIT(body, response.json());
  const auto& auth = member(body, "auth");
  EXPECT_EQ(rt::getString(member(auth, "method"), "kind"), std::optional<std::string_view>("auth_tokens_allowlist"));
  EXPECT_EQ(rt::getString(member(auth, "token"), "shape"), std::optional<std::string_view>("github_prefix"));
}

ASYNC_TEST("allowlisted GitHub admin bearers keep precedence over passkey cookies on /uos/auth") {
  Fixture fixture;
  const auto seeded = harness::seedPasskeySession("uos_ai_session_non_admin_cookie", {.isAdmin = false});
  const std::string githubToken = "ghp_allowlisted_admin_fixture";
  uos::config::config.adminTokens.insert(githubToken);
  AWAIT(response, harness::handleV1Auth(request("/uos/auth", rt::Headers{{"Authorization", "Bearer " + githubToken}, {"Cookie", cookie(seeded.token)}})));
  EXPECT_EQ(response.status(), 200);
  AWAIT(body, response.json());
  const auto& auth = member(body, "auth");
  EXPECT_EQ(rt::getString(member(auth, "method"), "kind"), std::optional<std::string_view>("admin_allowlist"));
  EXPECT_EQ(rt::getBool(auth, "is_admin"), std::optional(true));
  EXPECT_EQ(rt::getBool(auth, "is_super_admin"), std::optional(true));
  EXPECT_EQ(rt::getString(member(auth, "token"), "shape"), std::optional<std::string_view>("github_prefix"));
}

ASYNC_TEST("passkey lifecycle handlers prefer a relay cookie over a stale GitHub bearer") {
  Fixture fixture;
  const std::string audienceOrigin = "https://agent-worker-4d2p9cx7m1ab.ubiquity-os.deno.net";
  const auto seeded = harness::seedPasskeySession("uos_ai_session_lifecycle_cookie", {.audienceOrigin = audienceOrigin});
  const rt::Headers headers{{"Authorization", "Bearer ghp_stale_lifecycle_fixture"}, {"Cookie", cookie(seeded.token)}, {"Origin", audienceOrigin}};
  AWAIT(sessionResponse, handler(request("/api/auth/session", headers)));
  EXPECT_EQ(sessionResponse.status(), 200);
  AWAIT(sessionBody, sessionResponse.json());
  EXPECT_EQ(rt::getString(member(sessionBody, "user"), "id"), std::optional<std::string_view>(seeded.user.id));
  AWAIT(registerResponse, handler(request("/api/auth/register/start", headers, "POST", json::object{{"handle", seeded.user.handle}})));
  EXPECT_EQ(registerResponse.status(), 200);
  AWAIT(registerBody, registerResponse.json());
  const auto& user = member(member(registerBody, "publicKey"), "user");
  EXPECT_EQ(rt::getString(user, "id"), std::optional<std::string_view>(harness::encodeBase64Url(seeded.user.id)));
  EXPECT_EQ(rt::getString(user, "name"), std::optional<std::string_view>(seeded.user.handle));
  AWAIT(logoutResponse, handler(request("/api/auth/logout", headers, "POST")));
  EXPECT_EQ(logoutResponse.status(), 204);
  AWAIT(session, harness::kvStub()->get(passkeys::passkeySessionKey(seeded.token)));
  EXPECT(session.value.isNull());
}

ASYNC_TEST("passkey lifecycle handlers preserve configured bearer precedence") {
  Fixture fixture;
  const std::vector<std::string> configuredKinds{"client", "admin"};
  for (const auto& configuredKind : configuredKinds) {
    harness::kvStore.clear();
    const std::string audienceOrigin = "https://agent-worker-4d2p9cx7m1ab.ubiquity-os.deno.net";
    const auto seeded = harness::seedPasskeySession("uos_ai_session_" + configuredKind + "_lifecycle_cookie", {.audienceOrigin = audienceOrigin});
    const auto configuredToken = "ghp_configured_" + configuredKind + "_lifecycle_1234567890abcdefghijklmnopqrstuvwxyz";
    auto& configuredTokens = configuredKind == "client" ? uos::config::config.authTokens : uos::config::config.adminTokens;
    configuredTokens.insert(configuredToken);
    const auto cleanup = rt::scopeExit([&] { configuredTokens.erase(configuredToken); });
    const rt::Headers headers{{"Authorization", "Bearer " + configuredToken}, {"Cookie", cookie(seeded.token)}, {"Origin", audienceOrigin}};
    const auto registrationHandle = "configured-" + configuredKind + "-registration";
    AWAIT(registerResponse, handler(request("/api/auth/register/start", headers, "POST", json::object{{"handle", registrationHandle}})));
    EXPECT_EQ(registerResponse.status(), configuredKind == "client" ? 401 : 200);
    if (configuredKind == "admin") {
      AWAIT(registerBody, registerResponse.json());
      EXPECT_EQ(rt::getString(member(member(registerBody, "publicKey"), "user"), "name"), std::optional<std::string_view>(registrationHandle));
    }
    AWAIT(sessionResponse, handler(request("/api/auth/session", headers)));
    EXPECT_EQ(sessionResponse.status(), 401);
    AWAIT(logoutResponse, handler(request("/api/auth/logout", headers, "POST")));
    EXPECT_EQ(logoutResponse.status(), 204);
    AWAIT(session, harness::kvStub()->get(passkeys::passkeySessionKey(seeded.token)));
    EXPECT(!session.value.isNull());
  }
}

ASYNC_TEST("passkey logout preserves valid bearer precedence over a relay cookie") {
  Fixture fixture;
  const std::string audienceOrigin = "https://agent-worker-4d2p9cx7m1ab.ubiquity-os.deno.net";
  const auto bearer = harness::seedPasskeySession("uos_ai_session_bearer_precedence");
  const auto secondary = harness::seedPasskeySession("uos_ai_session_cookie_secondary", {.audienceOrigin = audienceOrigin});
  AWAIT(response, handler(request("/api/auth/logout", rt::Headers{{"Authorization", "Bearer " + bearer.token}, {"Cookie", cookie(secondary.token)}, {"Origin", audienceOrigin}}, "POST")));
  EXPECT_EQ(response.status(), 204);
  AWAIT(bearerSession, harness::kvStub()->get(passkeys::passkeySessionKey(bearer.token)));
  EXPECT(bearerSession.value.isNull());
  AWAIT(cookieSession, harness::kvStub()->get(passkeys::passkeySessionKey(secondary.token)));
  EXPECT(!cookieSession.value.isNull());
}

ASYNC_TEST("authenticated passkey token overrides remain bound to their relay audience") {
  Fixture fixture;
  const auto seeded = harness::seedPasskeySession("uos_ai_session_override_audience", {.audienceOrigin = "https://agent-worker-4d2p9cx7m1ab.ubiquity-os.deno.net"});
  const passkeys::PasskeyRegisterStartOptions options{.authenticatedPasskeyToken = seeded.token};
  AWAIT(response, passkeys::handlePasskeyRegisterStart(request("/api/auth/register/start", rt::Headers{{"Origin", "https://evil.example"}}, "POST", json::object()), options));
  EXPECT_EQ(response.status(), 401);
}
