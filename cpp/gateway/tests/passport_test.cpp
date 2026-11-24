// Port of all seven original tests/passport.test.ts registrations, in source order.
// Links: tests/passport_test.cpp src/auth/passport.cpp src/agent_messages.cpp src/auth/passport_sessions.cpp tests/helpers/admin_auth_harness.cpp src/utils.cpp src/config.cpp src/models/catalog.cpp src/input_normalization.cpp src/models/codex_models.cpp src/release.cpp src/handler/index.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
// src/provider/cerebras.cpp src/admin/claude_code.cpp src/static.cpp src/health.cpp src/deepseek/waterfall.cpp src/admin/codex.cpp src/models/openrouter_models.cpp src/codex/auth.cpp src/types.cpp src/admin/codex_reset.cpp src/runtime_config.cpp src/live/handler.cpp src/admin/kernel.cpp src/handler/http.cpp
// src/provider/capacity_parse.cpp src/provider/presentation.cpp src/auth/passkeys.cpp src/request.cpp src/http.cpp src/admin/api_keys.cpp src/quota_projection.cpp src/api_key_policy.cpp src/provider/health.cpp src/catalog/store.cpp src/provider/claude_code_accounts.cpp src/catalog/types.cpp
// src/paid_fallback/ledger_state.cpp src/kv_conversion/profile_json.cpp src/authority/native_paid_contracts.cpp src/codex/reset_settings.cpp src/provider/selection.cpp src/models/metadata.cpp src/paid_fallback/ledger_backfill.cpp src/codex/manual_reset.cpp src/metered_quota.cpp src/codex/banked_reset.cpp
// src/provider/claude_code_quota.cpp src/live/routes.cpp src/api_keys.cpp src/paid_fallback/ledger_settlement.cpp src/live/sideband.cpp src/models/codex_models_whitelist.cpp src/authority/native_paid.cpp src/kernel/quota_write.cpp src/admin/api_key_audit.cpp src/authority/native_codex_capacity.cpp
// src/kernel/usage.cpp src/kernel/quota_v2.cpp src/provider/claude_code_credentials.cpp src/provider/metered.cpp src/codex/account_routing.cpp src/authority/native_kernel.cpp src/authority/native_api_key.cpp src/authority/native_codex_mutations.cpp src/provider/surplus.cpp src/codex/routing_serial.cpp
// src/provider/claude_code_models.cpp src/codex/overage_settings.cpp src/authority/native_api_key_policy.cpp src/codex/capacity_routing.cpp src/provider/lithos.cpp src/codex/banked_reset_pool.cpp src/request_policy.cpp src/auth/index.cpp src/authority/native_codex.cpp
// src/provider/anthropic_chat_responses.cpp src/codex/native_auth.cpp src/codex/external_auth.cpp src/codex/routing_mutations.cpp src/kernel/quota_reservations.cpp src/codex/banked_reset_claim.cpp src/authority/native_codex_contracts.cpp src/admin/api_key_mutations.cpp src/provider/surplus_claude_request.cpp
// src/paid_fallback/ledger_admission.cpp src/codex/experiment.cpp src/authority/native_paid_policy.cpp src/provider/openrouter.cpp src/codex/auth_refresh.cpp src/admin/defaults.cpp src/live/upstream.cpp src/codex/banked_reset_provider.cpp src/authority/native_codex_evaluation.cpp src/kernel/attestation.cpp
// src/models/codex_models_fetch.cpp src/provider/claude_code_access.cpp src/paid_fallback/ledger_stats.cpp src/paid_fallback/index.cpp src/provider/claude_code.cpp src/codex/rate_limit_429.cpp src/codex/dispatch.cpp src/cache/kv_migration_base.cpp src/codex/routing_evaluation.cpp
// src/provider/anthropic_chat_stream.cpp src/cache/prompt_analytics_read.cpp src/debug_routing.cpp src/cache/scope_experiment_targets.cpp src/authority/native_codex_429.cpp src/cache/prompt_analytics_core.cpp src/cache/scope_experiment_model.cpp src/codex/quota.cpp src/live/calls.cpp
// src/cache/telemetry_gate.cpp src/authority/native_api_key_audit.cpp src/provider/cerebras_rate_limits.cpp src/codex/banked_reset_submission.cpp src/admin/chart_source_proxy.cpp src/provider/capacity.cpp src/auth/relay.cpp src/provider/capacity_history.cpp src/kv.cpp src/provider/anthropic_responses.cpp
// src/provider/capacity_analytics.cpp src/authority/native_kernel_contracts.cpp src/provider/anthropic_response_wire.cpp src/handler/terminal_route.cpp src/admin/capacity_observation_sync.cpp src/codex/supervisor_brief.cpp src/codex/supervisor.cpp src/catalog/index.cpp src/defaults.cpp
// src/systemone/handlers.cpp src/kernel/policy_queue.cpp src/openai_telemetry.cpp src/provider/capacity_rollups.cpp src/codex/routing_state.cpp src/provider/lithos_rate_limits.cpp src/provider/lithos_models.cpp src/cache/kv_migration_validation.cpp src/responses_failover_stream.cpp src/catalog/models.cpp
// src/sentinel/replay_observation.cpp src/sentinel/replay_admin.cpp src/handler/terminal_log.cpp src/recent_model_context.cpp src/auth/local_admin.cpp src/provider/surplus_claude_chat_request.cpp src/deepseek/index.cpp src/sentinel/replay_model.cpp src/models/codex_models_availability.cpp
// src/codex/supervisor_config.cpp src/responses_stream.cpp src/admin/error_log.cpp src/cache/kv_migration_paid_fallback.cpp src/sentinel/replay_read.cpp src/runtime/websocket.cpp src/admin/sentinel_incident.cpp src/chat/envelope.cpp src/codex/manual_reset_state.cpp src/auth/local_development_key.cpp
// src/cache/scope_experiment.cpp src/embeddings/jobs.cpp src/cache/scope_targets.cpp src/sentinel/replay_store.cpp src/codex/supervisor_secret.cpp src/openai.cpp src/paid_fallback/health.cpp src/sentinel/replay_retention.cpp src/provider/claude_code_usage_schema.cpp src/provider/capacity_events.cpp
// src/cache/kv_migration_usage.cpp src/paid_fallback/rollups.cpp src/sentinel/replay_retention_schema.cpp src/codex/supervisor_follow.cpp src/embeddings/ledger.cpp src/sentinel/upstream_capture.cpp src/sentinel/incident_outbox.cpp src/paid_fallback/routing.cpp src/embeddings/voyage.cpp
// src/sentinel/replay_envelope.cpp src/codex/supervisor_log.cpp src/codex/native_auth_transport.cpp src/storage/kv.cpp src/embeddings/handlers.cpp src/responses_handler.cpp src/provider/capacity_contract.cpp src/codex/supervisor_transport.cpp src/upstream_wire.cpp src/decisions/handlers.cpp
// src/provider/claude_capacity_history.cpp src/responses_attempts.cpp src/handler/admission.cpp src/provider/anthropic_replay.cpp src/analytics.cpp src/provider/claude_capacity_rollups.cpp src/bounded_response_body.cpp src/codex/supervisor_inventory.cpp src/images.cpp
// src/sentinel/replay_retention_bootstrap.cpp src/responses_native_headers.cpp src/paid_fallback/removed_provider.cpp src/openai_schema.cpp src/provider/cerebras_handlers.cpp src/deepseek/flash_aliases.cpp src/paid_fallback/claude_code_route.cpp src/sentinel/replay_limits.cpp src/chat/stream_translation.cpp
// src/provider/removed_provider_circuit.cpp src/inference_deadline.cpp src/runtime/http_tasks.cpp src/sentinel/replay_retention_metadata.cpp src/provider/cerebras_responses.cpp src/responses_request.cpp src/deepseek/responses_payload.cpp src/provider/openrouter_handlers.cpp
// src/provider/openrouter_request_controls.cpp src/provider/removed_provider_telemetry.cpp src/kernel/quota_reserve.cpp src/deepseek/responses.cpp src/catalog/promotion.cpp src/inference_admission.cpp src/chat/completions.cpp src/codex/responses_error.cpp src/runtime/http_early_sse.cpp
// src/deepseek/responses_stream.cpp src/provider/lithos_handlers.cpp src/cache/prompt_analytics_queue.cpp src/storage/native_kv.cpp src/deepseek/handlers.cpp src/codex/index.cpp src/optional_telemetry_queue.cpp src/deepseek/context_admission_failure.cpp
// src/provider/stream_relay.cpp src/storage/sqlite_readonly.cpp src/provider/lithos_streams.cpp src/lib/jev_compaction/codex_items.cpp src/responses_buffered.cpp src/deepseek/waterfall_handler.cpp src/deepseek/stream.cpp src/provider/openrouter_streams.cpp src/codex/responses_state.cpp
// src/deepseek/chat_projection.cpp src/deepseek/forwarded_payload_policy.cpp src/provider/openrouter_web_tools.cpp src/codex/responses_runners.cpp src/codex/responses_operations.cpp src/lib/jev_compaction/state.cpp src/lib/jev_compaction/compact.cpp src/lib/jev_compaction/types.cpp
// src/lib/jev_compaction/request.cpp
#include "tests/helpers/admin_auth_harness.hpp"
#include "tests/helpers/responses_conformance_client.hpp"
#include "uos/auth/index.hpp"
#include "uos/auth/passport.hpp"
#include "uos/config.hpp"
#include "uos/handler/http.hpp"
#include "uos/handler/index.hpp"
#include "uos/kv.hpp"
#include "uos/utils.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <string>
#include <vector>

#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace harness = uos::tests::helpers::admin_auth_harness;
  namespace auth = uos::auth::index;
  namespace passport = uos::auth::passport;
  using uos::handler::index::handler;
  const std::string ORIGIN = "https://ai.ubq.fi";
  const std::string ISSUER = "https://auth.ubq.fi";
  const std::string SESSION_COOKIE = "__Host-uos_ai_passport_session";
  const std::string STATE_COOKIE = "__Host-uos_ai_passport_state";
  const std::string VERIFIER_COOKIE = "__Host-uos_ai_passport_verifier";
  const json::object appGrant{{"token", "uos_app_session_" + std::string(43, 'a')}, {"subject", "fixture-admin"}, {"clientId", "ai"}, {"audience", "ai"}, {"origin", ORIGIN}, {"redirectUri", ORIGIN + "/auth/callback"}, {"createdAt", "2026-01-01T00:00:00.000Z"}};
  const rt::crypto::CryptoKeyPair& keys() {
    static const auto value = rt::crypto::subtle::generateKey({.name = "RSASSA-PKCS1-v1_5", .hash = "SHA-256", .modulusLength = 2048, .publicExponent = rt::Bytes{1, 0, 1}}, true, {"sign", "verify"});
    return value;
  }
  std::string encode(const json::value& value) { const auto text = rt::jsonStringify(value); return uos::utils::base64UrlEncode(rt::crypto::bytesOf(text)); }
  std::string sign(const json::object& overrides = {}, const json::object& headerOverrides = {}) {
    const auto now = rt::Math::floor(rt::dateNow() / 1000);
    auto header = rt::objectSpread(json::object{{"alg", "RS256"}, {"typ", "JWT"}, {"kid", "fixture-passport"}}, headerOverrides);
    auto body = rt::objectSpread(json::object{{"iss", ISSUER}, {"sub", "fixture-admin"}, {"handle", "administrator"}, {"isAdmin", true}, {"scopes", json::array()}, {"aud", "ai"}, {"clientId", "ai"}, {"auth", "sso"}, {"iat", now}, {"exp", now + 3600}, {"jti", "fixture-id"}}, overrides);
    const std::string input = encode(header) + "." + encode(body);
    const auto signature = rt::crypto::subtle::sign("RSASSA-PKCS1-v1_5", keys().privateKey, rt::crypto::bytesOf(input));
    return input + "." + uos::utils::base64UrlEncode(signature);
  }
  rt::Request request(const std::string& path, const std::string& token = "", rt::RequestInit init = {}) {
    auto headers = init.headers.value_or(rt::Headers());
    if (!token.empty()) headers.set("Cookie", SESSION_COOKIE + "=" + token);
    init.headers = headers;
    return rt::Request(ORIGIN + path, std::move(init));
  }
  std::vector<std::pair<std::string, std::string>> isolatedEnvironment() {
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto* key : {"HOME", "TMPDIR"}) if (const auto value = rt::env::get(key)) result.emplace_back(key, *value);
    return result;
  }
  const json::value& member(const json::value& value, std::string_view key) { const auto* result = rt::get(value, key); ASSERT(result); return *result; }
  bool hasCookie(const rt::Response& response, const std::string& prefix) {
    const auto cookies = response.headers().getSetCookie();
    return std::any_of(cookies.begin(), cookies.end(), [&](const auto& value) { return rt::startsWith(value, prefix); });
  }
  void refused(const auth::AdminAuthResult& result, int status) { const auto* error = std::get_if<auth::AdminAuthResultNotOk>(&result); ASSERT(error); EXPECT_EQ(error->response.status(), status); }
  struct BeginResult { std::string state; std::string codeVerifier; std::string stateCookie; };
  struct Fixture {
    uos::config::Config previous = uos::config::config;
    rt::env::ScopedEnvironment environment{isolatedEnvironment()};
    std::string exchangedToken;
    json::array exchanges;
    int jwksStatus = 200;
    int exchangeStatus = 200;
    rt::Reason exchangeFailure;
    std::optional<rt::GlobalFetchOverride> fetch;
    Fixture() {
      uos::config::config.codexAuthJsonB64.clear(); harness::resetHarness(); passport::resetPassportCacheForTest(); uos::tests::helpers::responses_conformance_client::stageGatewayAssets();
      fetch.emplace(rt::Fetch([this](rt::RequestInfo input, rt::RequestInit init) -> rt::Task<rt::Response> {
        const auto url = harness::urlOf(input);
        if (url == ISSUER + "/api/auth/jwks") {
          auto publicKey = rt::crypto::subtle::exportJwk(keys().publicKey);
          publicKey["kid"] = "fixture-passport"; publicKey["alg"] = "RS256"; publicKey["use"] = "sig";
          const json::object body{{"keys", json::array{publicKey}}};
          const rt::ResponseInit responseInit{.status = jwksStatus};
          co_return rt::Response::json(body, responseInit);
        }
        if (url == ISSUER + "/api/auth/sso/exchange") {
          EXPECT_EQ(init.method, std::optional<std::string>("POST")); EXPECT_EQ(init.redirect, std::optional<std::string>("error"));
          ASSERT(init.signal.has_value());
          const rt::Response reader(init.body.value_or(rt::BodyInit(nullptr)));
          AWAIT(body, reader.json()); exchanges.push_back(body);
          if (exchangeFailure) std::rethrow_exception(exchangeFailure);
          const rt::ResponseInit responseInit{.status = static_cast<double>(exchangeStatus)};
          if (exchangeStatus != 200) co_return rt::Response::json(json::object{{"error", "Exchange failed"}}, responseInit);
          const json::object exchanged{{"accessToken", exchangedToken}, {"appSession", appGrant}};
          co_return rt::Response::json(exchanged, responseInit);
        }
        if (url == ISSUER + "/api/auth/sso/session") {
          const json::object body{{"authenticated", true}, {"session", json::object{{"subject", "fixture-admin"}, {"clientId", "ai"}, {"audience", "ai"}, {"origin", ORIGIN}, {"redirectUri", ORIGIN + "/auth/callback"}, {"createdAt", "2026-01-01T00:00:00.000Z"}}}, {"user", json::object{{"id", "fixture-admin"}, {"handle", "administrator"}, {"isAdmin", true}, {"scopes", json::array()}}}};
          co_return rt::Response::json(body);
        }
        if (url == ISSUER + "/api/auth/sso/session/revoke") co_return rt::Response(nullptr, rt::ResponseInit{.status = 204});
        throw rt::JsError("Unexpected fetch: " + url);
      }));
    }
    ~Fixture() { fetch.reset(); passport::resetPassportCacheForTest(); harness::cleanupHarness(); uos::config::config = previous; }
    rt::Task<BeginResult> begin(std::string returnTo = "/admin") {
      AWAIT(response, handler(request("/auth/login?return_to=" + rt::encodeURIComponent(returnTo))));
      EXPECT_EQ(response.status(), 302);
      const rt::URL authorize(response.headers().get("Location").value_or(""));
      EXPECT_EQ(authorize.origin(), ISSUER); EXPECT_EQ(authorize.pathname(), "/authorize");
      EXPECT_EQ(authorize.searchParams().get("client_id"), std::optional<std::string>("ai"));
      EXPECT_EQ(authorize.searchParams().get("audience"), std::optional<std::string>("ai"));
      EXPECT_EQ(authorize.searchParams().get("origin"), std::optional<std::string>(ORIGIN));
      EXPECT_EQ(authorize.searchParams().get("redirect_uri"), std::optional<std::string>(ORIGIN + "/auth/callback"));
      const auto state = authorize.searchParams().get("state").value_or("");
      EXPECT(rt::Regex("^[A-Za-z0-9_-]{43}\\.(?:admin|chat)$").test(state));
      EXPECT_EQ(authorize.searchParams().get("code_challenge_method"), std::optional<std::string>("S256"));
      const auto responseCookies = response.headers().getSetCookie(); ASSERT(!responseCookies.empty());
      const auto verifier = std::find_if(responseCookies.begin(), responseCookies.end(), [](const auto& value) { return rt::startsWith(value, VERIFIER_COOKIE + "="); });
      const auto codeVerifier = verifier == responseCookies.end() ? "" : rt::split(*verifier, ";").front().substr(VERIFIER_COOKIE.size() + 1);
      EXPECT(rt::Regex("^[A-Za-z0-9_-]{43}$").test(codeVerifier));
      EXPECT_EQ(authorize.searchParams().get("code_challenge"), std::optional<std::string>(uos::utils::base64UrlEncode(rt::crypto::subtle::digest("SHA-256", rt::crypto::bytesOf(codeVerifier)))));
      EXPECT(rt::includes(responseCookies.front(), "HttpOnly; Secure; SameSite=Lax"));
      EXPECT_EQ(response.headers().get("Cache-Control"), std::optional<std::string>("no-store"));
      std::vector<std::string> cookies; for (const auto& value : responseCookies) cookies.push_back(rt::split(value, ";").front());
      co_return BeginResult{state, codeVerifier, rt::join(cookies, "; ")};
    }
  };

  rt::Task<void> checkExchangeFailure(Fixture& fixture, std::string code, int status, std::string errorCode) {
    AWAIT(begin, fixture.begin());
    const rt::RequestInit init{.headers = rt::Headers{{"Cookie", begin.stateCookie}}};
    const auto callback = request("/auth/callback?state=" + begin.state + "&code=" + code, "", init);
    AWAIT(response, handler(callback));
    EXPECT_EQ(fixture.exchanges.size(), 1u);
    const std::vector<std::string> expectedCookies{
      STATE_COOKIE + "=; Max-Age=0; Path=/; HttpOnly; Secure; SameSite=Lax",
      VERIFIER_COOKIE + "=; Max-Age=0; Path=/; HttpOnly; Secure; SameSite=Lax",
    };
    EXPECT_EQ(response.headers().getSetCookie(), expectedCookies);
    EXPECT_EQ(response.headers().get("Cache-Control"), std::optional<std::string>("no-store"));
    EXPECT_EQ(response.status(), status);
    AWAIT(body, response.json());
    EXPECT_EQ(rt::getString(member(body, "error"), "code"), errorCode);
  }
}

ASYNC_TEST("Bun Passport button destination completes exchange, sets host cookies, returns to admin and authenticates protected reads") {
  Fixture fixture; fixture.exchangedToken = sign(); AWAIT(begin, fixture.begin());
  AWAIT(response, handler(request("/auth/callback?state=" + begin.state + "&code=single-use-code", "", {.headers = rt::Headers{{"Cookie", begin.stateCookie}}})));
  EXPECT_EQ(response.status(), 302); EXPECT_EQ(response.headers().get("Location"), std::optional<std::string>("/admin"));
  const json::array expected{json::object{{"code", "single-use-code"}, {"clientId", "ai"}, {"audience", "ai"}, {"origin", ORIGIN}, {"redirectUri", ORIGIN + "/auth/callback"}, {"codeVerifier", begin.codeVerifier}, {"appSession", true}}};
  EXPECT(rt::deepEqual(fixture.exchanges, expected));
  const auto cookies = response.headers().getSetCookie(); EXPECT_EQ(cookies.size(), std::size_t(3));
  const auto found = std::find_if(cookies.begin(), cookies.end(), [](const auto& value) { return rt::startsWith(value, SESSION_COOKIE + "=uos_ai_passport_session_"); });
  ASSERT(found != cookies.end()); EXPECT(rt::endsWith(*found, "HttpOnly; Secure; SameSite=Lax"));
  const auto sessionToken = rt::split(*found, ";").front().substr(SESSION_COOKIE.size() + 1);
  EXPECT(hasCookie(response, STATE_COOKIE + "=; Max-Age=0;")); EXPECT(hasCookie(response, VERIFIER_COOKIE + "=; Max-Age=0;"));
  AWAIT(whoami, handler(request("/uos/auth", sessionToken))); EXPECT_EQ(whoami.status(), 200); AWAIT(payload, whoami.json());
  const auto& authBody = member(payload, "auth"); const auto& method = member(authBody, "method");
  EXPECT_EQ(rt::getBool(authBody, "is_admin"), std::optional(true)); EXPECT_EQ(rt::getString(authBody, "principal"), std::optional<std::string_view>("passport-user:fixture-admin"));
  EXPECT_EQ(rt::getString(method, "kind"), std::optional<std::string_view>("passport_session")); EXPECT_EQ(rt::getBool(member(authBody, "token"), "present"), std::optional(false));
  EXPECT(member(member(method, "user"), "exp").is_null()); EXPECT(member(member(method, "user"), "jti").is_null());
  EXPECT(hasCookie(whoami, SESSION_COOKIE + "=" + sessionToken + "; Max-Age="));
  AWAIT(protectedRead, handler(request("/admin/api-keys", sessionToken))); EXPECT_EQ(protectedRead.status(), 200);
  AWAIT(client, auth::authenticateClient(request("/v1/models", sessionToken))); const auto* accepted = std::get_if<auth::AuthenticateClientResultOk>(&client); ASSERT(accepted);
  AWAIT(principal, uos::handler::http::resolveIdempotencyPrincipal(*accepted)); EXPECT_EQ(principal, "passport-user:fixture-admin");
}

ASYNC_TEST("Passport callback rejects absent, mismatched and duplicated state before code exchange") {
  Fixture fixture; AWAIT(begin, fixture.begin());
  const std::vector<std::pair<std::string, std::string>> variants{
    {"state=" + begin.state + "&code=code", ""}, {"state=" + begin.state + "&code=code", STATE_COOKIE + "=" + begin.state}, {"state=" + begin.state + "&code=code", STATE_COOKIE + "=other"},
    {"state=" + begin.state + "&state=" + begin.state + "&code=code", begin.stateCookie}, {"state=" + begin.state + "&code=one&code=two", begin.stateCookie}, {"state=" + begin.state, begin.stateCookie}};
  for (const auto& [query, value] : variants) {
    AWAIT(response, handler(request("/auth/callback?" + query, "", {.headers = rt::Headers{{"Cookie", value}}})));
    EXPECT_EQ(response.status(), 401); EXPECT(hasCookie(response, STATE_COOKIE + "=; Max-Age=0;"));
  }
  EXPECT_EQ(fixture.exchanges.size(), std::size_t(0));
}

ASYNC_TEST("Passport callback returns 401 and clears state for rejected exchange statuses") {
  const std::vector<int> statuses{400, 401, 403, 422, 429};
  for (const int status : statuses) {
    Fixture fixture;
    fixture.exchangeStatus = status;
    auto checking = checkExchangeFailure(fixture, "rejected-code", 401, "invalid_api_key");
    co_await std::move(checking);
  }
}

ASYNC_TEST("Passport callback retains 502 and clears state for upstream exchange failures") {
  const std::vector<int> statuses{500, 502, 503, 504};
  for (const int status : statuses) {
    Fixture fixture;
    fixture.exchangeStatus = status;
    auto checking = checkExchangeFailure(fixture, "valid-code", 502, "passport_unavailable");
    co_await std::move(checking);
  }
}

ASYNC_TEST("Passport callback reports exchange network failures and timeouts as unavailable") {
  const std::vector<rt::Reason> failures{
    std::make_exception_ptr(rt::TypeError("fixture exchange connection failed")),
    std::make_exception_ptr(rt::DOMException("fixture exchange timed out", "TimeoutError")),
  };
  for (const auto& failure : failures) {
    Fixture fixture;
    fixture.exchangeFailure = failure;
    auto checking = checkExchangeFailure(fixture, "fixture-code", 502, "passport_unavailable");
    co_await std::move(checking);
  }
}

ASYNC_TEST("Passport callback rejects invalid exchanged credentials and clears state") {
  Fixture fixture;
  fixture.exchangedToken = "invalid-token";
  auto checking = checkExchangeFailure(fixture, "fixture-code", 401, "invalid_api_key");
  co_await std::move(checking);
}

ASYNC_TEST("failed Passport callback clears only transient cookies and never creates a session from an agent") {
  Fixture fixture; fixture.exchangedToken = sign({{"agentTokenId", "agent-id"}}); AWAIT(begin, fixture.begin());
  AWAIT(response, handler(request("/auth/callback?state=" + begin.state + "&code=code", "", {.headers = rt::Headers{{"Cookie", begin.stateCookie + "; " + SESSION_COOKIE + "=existing-session"}}})));
  EXPECT_EQ(response.status(), 401);
  std::vector<std::string> names; for (const auto& value : response.headers().getSetCookie()) names.push_back(rt::split(value, "=").front());
  const std::vector<std::string> expected{STATE_COOKIE, VERIFIER_COOKIE}; EXPECT_EQ(names, expected);
}

ASYNC_TEST("Passport callback only returns to fixed local destinations") {
  Fixture fixture; fixture.exchangedToken = sign(); AWAIT(begin, fixture.begin("https://evil.example/admin"));
  AWAIT(response, handler(request("/auth/callback?state=" + begin.state + "&code=code", "", {.headers = rt::Headers{{"Cookie", begin.stateCookie}}})));
  EXPECT_EQ(response.headers().get("Location"), std::optional<std::string>("/chat"));
}

ASYNC_TEST("Passport admin rejects ordinary users, agent cookies, JWT bearers and cross-origin mutations") {
  Fixture fixture;
  AWAIT(nonAdmin, auth::authenticateAdmin(request("/admin/api-keys", sign({{"isAdmin", false}})))); refused(nonAdmin, 403);
  AWAIT(agent, auth::authenticateAdmin(request("/admin/api-keys", sign({{"agentTokenId", "agent-id"}})))); refused(agent, 401);
  const auto admin = sign(); const std::vector<std::string> tokens{"", admin};
  for (const auto& token : tokens) { AWAIT(bearer, auth::authenticateAdmin(request("/admin/api-keys", token, {.headers = rt::Headers{{"Authorization", "Bearer " + admin}}}))); refused(bearer, 401); }
  const std::vector<rt::Headers> headers{rt::Headers{{"Origin", "https://evil.example"}}, rt::Headers{{"Sec-Fetch-Site", "cross-site"}}};
  for (const auto& value : headers) { AWAIT(foreign, handler(request("/admin/api-keys", admin, {.method = "POST", .headers = value, .body = rt::BodyInit(std::string("{}"))}))); EXPECT_EQ(foreign.status(), 403); }
  AWAIT(local, auth::authenticateAdmin(request("/admin/api-keys", admin, {.method = "POST", .headers = rt::Headers{{"Origin", ORIGIN}, {"Sec-Fetch-Site", "same-origin"}}})));
  EXPECT(std::holds_alternative<auth::AdminAuthResultOk>(local));
}

ASYNC_TEST("Passport verifies signature, expiry, issuer, audience, client, algorithm and key source") {
  Fixture fixture; const auto now = rt::Math::floor(rt::dateNow() / 1000);
  const std::vector<json::object> claims{{{"iss", "https://passport.ubq.fi"}}, {{"iss", "https://evil.example"}}, {{"aud", "other"}}, {{"clientId", "other"}}, {{"exp", now - 1}}, {{"iat", now + 120}}, {{"isAdmin", "true"}}};
  for (const auto& value : claims) { AWAIT(result, auth::authenticateAdmin(request("/admin/api-keys", sign(value)))); refused(result, 401); }
  const std::vector<json::object> headers{{{"alg", "HS256"}}, {{"jku", "https://evil.example/keys"}}, {{"jwk", rt::crypto::subtle::exportJwk(keys().publicKey)}}};
  for (const auto& value : headers) { AWAIT(result, auth::authenticateAdmin(request("/admin/api-keys", sign({}, value)))); refused(result, 401); }
  const auto token = sign(); auto parts = rt::split(token, "."); parts[1] = encode(json::object{{"isAdmin", true}});
  AWAIT(forged, auth::authenticateAdmin(request("/admin/api-keys", rt::join(parts, ".")))); refused(forged, 401);
  auto signature = rt::split(token, "."); signature[2] = std::string(rt::startsWith(signature[2], "A") ? "B" : "A") + signature[2].substr(1);
  AWAIT(badSignature, auth::authenticateAdmin(request("/admin/api-keys", rt::join(signature, ".")))); refused(badSignature, 401);
}

ASYNC_TEST("Passport unavailable keys fail closed, proxy origin stays fixed and logout clears only its cookie") {
  Fixture fixture; fixture.jwksStatus = 503; const auto token = sign();
  AWAIT(unavailable, auth::authenticateAdmin(request("/admin/api-keys", token))); refused(unavailable, 502);
  rt::URL proxyUrl("/auth/login?return_to=%2Fadmin", ORIGIN); proxyUrl.protocol("http:");
  AWAIT(proxy, handler(rt::Request(proxyUrl.href(), {.headers = rt::Headers{{"X-Forwarded-Proto", "https"}, {"X-Forwarded-Host", "evil.example"}}})));
  EXPECT_EQ(proxy.status(), 302); EXPECT_EQ(rt::URL(proxy.headers().get("Location").value_or("")).searchParams().get("origin"), std::optional<std::string>(ORIGIN));
  AWAIT(foreignHost, passport::handlePassportRoute(rt::Request("https://evil.example/auth/login"), "/auth/login")); ASSERT(foreignHost); EXPECT_EQ(foreignHost->status(), 401);
  AWAIT(foreignLogout, handler(request("/auth/logout", token, {.method = "POST", .headers = rt::Headers{{"Origin", "https://evil.example"}}}))); EXPECT_EQ(foreignLogout.status(), 403);
  AWAIT(logout, handler(request("/auth/logout", token, {.method = "POST", .headers = rt::Headers{{"Origin", ORIGIN}}}))); EXPECT_EQ(logout.status(), 204);
  const std::vector<std::string> expected{SESSION_COOKIE + "=; Max-Age=0; Path=/; HttpOnly; Secure; SameSite=Lax"}; EXPECT_EQ(logout.headers().getSetCookie(), expected);
  uos::kv::setKvForTest(harness::kvStub()); AWAIT(protectedRead, handler(request("/admin/api-keys"))); EXPECT_EQ(protectedRead.status(), 401);
}
