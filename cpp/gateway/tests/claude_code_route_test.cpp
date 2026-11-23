// Links: tests/claude_code_route_test.cpp
// src/api_keys.cpp src/auth/relay.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp
// src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp
// src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp src/authority/native_paid_policy.cpp src/bounded_response_body.cpp
// src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_read.cpp src/codex/account_routing.cpp src/codex/auth.cpp
// src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_provider.cpp src/codex/capacity_routing.cpp
// src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp src/codex/native_auth.cpp
// src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp
// src/codex/responses_error.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp
// src/codex/routing_state.cpp src/config.cpp src/deepseek/flash_aliases.cpp src/deepseek/index.cpp
// src/defaults.cpp src/http.cpp src/kv.cpp src/metered_quota.cpp
// src/models/codex_models.cpp src/models/codex_models_availability.cpp src/models/codex_models_fetch.cpp src/models/openrouter_models.cpp
// src/openai_telemetry.cpp src/paid_fallback/claude_code_route.cpp src/paid_fallback/health.cpp src/paid_fallback/index.cpp
// src/paid_fallback/ledger_admission.cpp src/paid_fallback/ledger_backfill.cpp src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_state.cpp
// src/paid_fallback/ledger_stats.cpp src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp src/provider/anthropic_replay.cpp
// src/provider/anthropic_response_wire.cpp src/provider/anthropic_responses.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp
// src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp
// src/provider/capacity_rollups.cpp src/provider/cerebras.cpp src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp
// src/provider/claude_capacity_rollups.cpp src/provider/claude_code.cpp src/provider/claude_code_access.cpp src/provider/claude_code_accounts.cpp
// src/provider/claude_code_credentials.cpp src/provider/claude_code_models.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp
// src/provider/health.cpp src/provider/lithos.cpp src/provider/lithos_models.cpp src/provider/lithos_rate_limits.cpp
// src/provider/metered.cpp src/provider/removed_provider_telemetry.cpp src/provider/selection.cpp src/provider/surplus.cpp
// src/provider/surplus_claude_chat_request.cpp src/provider/surplus_claude_request.cpp src/release.cpp src/request_policy.cpp
// src/responses_failover_stream.cpp src/responses_stream.cpp src/runtime/websocket.cpp src/runtime_config.cpp
// src/storage/kv.cpp src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp
// src/types.cpp src/upstream_wire.cpp src/utils.cpp
// Full source route cases use fixture credentials, memory KV and mocked upstream transports.
#include "uos/paid_fallback/claude_code_route.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace route = uos::paid_fallback::claude_code_route;
  namespace accounts = uos::provider::claude_code_accounts;
  namespace credentials = uos::provider::claude_code_credentials;
  namespace quota = uos::provider::claude_code_quota;
  const std::string model = "claude-opus-5-5";
  const std::string workspace = "wrkspc_01ErorW3iFLRvQy55cpNWtEt";
  std::string requestUrl(const rt::RequestInfo& input) {
    return std::visit([](const auto& value) -> std::string {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, std::string>) return value;
      else if constexpr (std::is_same_v<T, rt::URL>) return value.href();
      else return value.url();
    }, input);
  }
  std::string sse() {
    return "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-opus-5-5\",\"content\":[],\"stop_reason\":null,\"stop_sequence\":null,\"usage\":{\"input_tokens\":5,\"output_tokens\":0}}}\n\n"
      "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
      "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"OK\"}}\n\n"
      "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
      "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":2}}\n\n"
      "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
  }
  rt::Response success() { return rt::Response(sse(), {.status = 200, .headers = rt::Headers{{"content-type", "text/event-stream"}, {"request-id", "req_1"}}}); }
  struct Captured { std::string url; rt::Headers headers; json::object body; };
  struct Fixture {
    rt::env::ScopedEnvironment isolation{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c1-route"}}};
    std::string savedConfig = uos::config::config.codexAuthJsonB64;
    std::shared_ptr<rt::MemoryKv> store = std::make_shared<rt::MemoryKv>(rt::dateNow);
    uos::openai_telemetry::UsageContext usage;
    std::vector<Captured> captured;
    bool apiKey = false;
    bool configured = true;
    Fixture() {
      uos::config::config.codexAuthJsonB64.clear(); usage.keyId = "owner-key";
      uos::kv::setKvForTest(store); uos::provider::selection::resetProviderSelectionCacheForTest();
      uos::provider::health::resetProviderHealthThrottleForTest(); credentials::resetClaudeCodeCredentialCacheForTest();
      uos::provider::claude_code_models::resetClaudeCodeModelsCacheForTest();
    }
    ~Fixture() { uos::kv::setKvForTest(nullptr); uos::config::config.codexAuthJsonB64 = savedConfig; }
    uos::provider::claude_code::ClaudeCodeEnvLookup env() const {
      const bool useApi = apiKey, enabled = configured;
      return [useApi, enabled](const std::string& name) -> std::optional<std::string> {
        if (!enabled) return std::nullopt;
        if (name == "CLAUDE_CODE_ALLOWED_KEY_IDS") return "owner-key";
        if (useApi && name == "CLAUDE_API_KEY") return "fixture-api-key";
        if (!useApi && name == "CLAUDE_CODE_OAUTH_TOKEN") return "test-token";
        return std::nullopt;
      };
    }
    void capture(const rt::RequestInfo& input, const rt::RequestInit& init) {
      const auto* body = init.body ? std::get_if<std::string>(&*init.body) : nullptr;
      captured.push_back({requestUrl(input), init.headers.value_or(rt::Headers()), rt::jsonParse(body ? *body : "{}").as_object()});
    }
    rt::Task<std::optional<uos::openai_telemetry::RoutedResponsesUpstream>> call(json::object body, rt::Fetch fetcher = {}) {
      if (!fetcher) fetcher = rt::Fetch([this](rt::RequestInfo input, rt::RequestInit init) { capture(input, init); return success(); });
      route::SubscriptionOptions options; options.model = rt::getString(body, "model").value_or(model);
      options.usageContext = &usage; options.env = env(); options.fetcher = fetcher;
      return route::attemptClaudeCodeSubscription(std::move(body), options);
    }
  };
  rt::Response workspaceRequired() {
    return rt::Response::json(json::object{{"type", "error"}, {"error", json::object{{"type", "invalid_request_error"},
      {"message", "This API key is not scoped to a workspace, so this request must include the anthropic-workspace-id header with the ID of the workspace to use."}}}}, {.status = 400});
  }
  rt::Response workspaces() {
    return rt::Response::json(json::object{{"data", json::array{
      json::object{{"id", workspace}, {"type", "workspace"}, {"name", "Default"}, {"created_at", "2026-02-02T22:25:14.167463Z"}, {"archived_at", nullptr}},
      json::object{{"id", "wrkspc_01FBEp5VyCHs7ghEnJqk6giF"}, {"type", "workspace"}, {"name", "Claude Code"}, {"created_at", "2025-03-22T10:01:42Z"}, {"archived_at", nullptr}}}}, {"has_more", false}});
  }
  json::object body(bool stream = true) { return {{"model", model}, {"input", "hello"}, {"stream", stream}}; }
  std::vector<std::string> systemText(const json::object& request) {
    std::vector<std::string> texts;
    if (const auto* blocks = rt::getArray(request, "system")) for (const auto& block : *blocks) texts.emplace_back(rt::getString(block, "text").value_or(""));
    return texts;
  }
}
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)

ASYNC_TEST("an eligible owner request is served by the subscription with the documented headers and defaults") {
  Fixture fixture; auto request = body(); request["instructions"] = "be brief";
  AWAIT(routed, fixture.call(request)); EXPECT(routed); EXPECT_EQ(routed->provider, "claude_code"); EXPECT(!routed->paidFallback); EXPECT(!routed->gatewayResponse);
  EXPECT_EQ(fixture.captured.size(), 1u); const auto& sent = fixture.captured.front();
  EXPECT_EQ(sent.url, "https://api.anthropic.com/v1/messages"); EXPECT_EQ(sent.headers.get("authorization"), std::optional<std::string>("Bearer test-token"));
  EXPECT_EQ(sent.headers.get("anthropic-version"), std::optional<std::string>("2023-06-01")); EXPECT_EQ(sent.headers.get("anthropic-beta"), std::optional<std::string>("oauth-2025-04-20")); EXPECT(!sent.headers.get("x-api-key"));
  EXPECT_EQ(systemText(sent.body).front(), "You are Claude Code, Anthropic's official CLI for Claude."); EXPECT_EQ(rt::getString(sent.body, "model"), std::optional<std::string>(model));
  EXPECT_EQ(rt::getNumber(sent.body, "max_tokens"), std::optional<double>(32000)); EXPECT_EQ(rt::getBool(sent.body, "stream"), std::optional<bool>(true));
  AWAIT(delivered, routed->response.text()); EXPECT(delivered.find("response.completed") != std::string::npos);
}
ASYNC_TEST("a Codex TUI request omits enabled hosted web search and still serves namespaced tools with late developer context") {
  Fixture fixture;
  auto request = rt::jsonParse(R"({"model":"claude-opus-5-5","stream":true,"store":false,"prompt_cache_key":"tui-conversation","reasoning":{"summary":"concise"},"include":["reasoning.encrypted_content"],"parallel_tool_calls":true,"tool_choice":"auto","tools":[{"type":"namespace","name":"clock","description":"Tools for reading time.","tools":[{"type":"function","name":"curr_time","strict":false,"parameters":{"type":"object","properties":{},"additionalProperties":false}}]},{"type":"web_search","external_web_access":true,"user_location":{"type":"approximate","country":"US"}}],"input":[{"type":"message","role":"developer","content":[{"type":"input_text","text":"global instructions"}]},{"type":"message","role":"user","content":[{"type":"input_text","text":"Reply with TUI_READY"}]},{"type":"message","role":"developer","content":[{"type":"input_text","text":"later hook reminder"}]}]})").as_object();
  AWAIT(routed, fixture.call(request)); EXPECT(routed); EXPECT_EQ(fixture.captured.size(), 1u);
  const auto& sent = fixture.captured.front(); const auto expected = rt::jsonParse(R"([{"name":"clock_curr_time","input_schema":{"type":"object","properties":{},"additionalProperties":false}}])");
  EXPECT(rt::deepEqual(*rt::get(sent.body, "tools"), expected)); EXPECT(rt::jsonStringify(sent.body).find("web_search") == std::string::npos);
  const std::vector<std::string> systems{"You are Claude Code, Anthropic's official CLI for Claude.", "global instructions", "later hook reminder"}; EXPECT_EQ(systemText(sent.body), systems);
  AWAIT(delivered, routed->response.text()); EXPECT(delivered.find("response.completed") != std::string::npos);
}
ASYNC_TEST("an ineligible request never reaches the subscription transport") {
  Fixture fixture; fixture.configured = false; auto request = body();
  AWAIT(noToken, fixture.call(request)); EXPECT(!noToken); fixture.configured = true; fixture.usage.keyId = "someone-else";
  AWAIT(wrongPrincipal, fixture.call(request)); EXPECT(!wrongPrincipal); fixture.usage.keyId = "owner-key"; request["model"] = "gpt-5.6-sol";
  AWAIT(nonClaude, fixture.call(request)); EXPECT(!nonClaude); EXPECT(fixture.captured.empty());
}
ASYNC_TEST("a durable subscription rejection falls through and blocks the tier until the reset margin") {
  Fixture fixture; const auto nowSeconds = rt::Math::trunc(rt::dateNow() / 1000);
  const rt::Fetch rejected([nowSeconds](rt::RequestInfo, rt::RequestInit) {
    return rt::Response::json(json::object{{"type", "error"}, {"error", json::object{{"type", "rate_limit_error"}, {"message", "limit reached"}}}},
      {.status = 429, .headers = rt::Headers{{"anthropic-ratelimit-unified-5h-status", "limited"}, {"anthropic-ratelimit-unified-5h-utilization", "1"}, {"anthropic-ratelimit-unified-5h-reset", rt::numberToString(nowSeconds + 3600)}}});
  });
  auto request = body(); AWAIT(first, fixture.call(request, rejected)); EXPECT(!first);
  AWAIT(account, accounts::resolveClaudeCodeAccount(fixture.store)); EXPECT(account); AWAIT(state, quota::loadClaudeCodeQuotaState(fixture.store, account));
  EXPECT(state); EXPECT_EQ(state->block_window, std::optional<std::string>("5h")); EXPECT(state->blocked_until_ms && *state->blocked_until_ms > rt::dateNow() + 3600000);
  AWAIT(second, fixture.call(request)); EXPECT(!second); EXPECT(fixture.captured.empty());
}
ASYNC_TEST("a local lossless-build refusal cannot authorize paid continuation or an upstream health event") {
  Fixture fixture; auto request = body(); request["previous_response_id"] = "resp_123";
  AWAIT(rejected, fixture.call(request)); EXPECT(rejected); EXPECT_EQ(rejected->response.status(), 400);
  AWAIT(payload, rejected->response.json()); EXPECT_EQ(rt::getString(*rt::get(payload, "error"), "code"), std::optional<std::string>("claude_code_request_invalid"));
  EXPECT(fixture.captured.empty());
  for (const auto& row : fixture.store->database()->rows()) for (const auto& part : row.key)
    if (const auto* text = std::get_if<std::string>(&part.value)) EXPECT(*text != "provider_health");
}
ASYNC_TEST("a transient rate limit retains its original failure without authorizing paid continuation") {
  Fixture fixture; const json::value expected = json::object{{"type", "error"}, {"error", json::object{{"type", "rate_limit_error"}}}};
  const rt::Fetch transient([expected](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(expected, {.status = 429, .headers = rt::Headers{{"retry-after", "30"}}}); });
  auto request = body(); AWAIT(rejected, fixture.call(request, transient)); EXPECT(rejected); EXPECT_EQ(rejected->response.status(), 429);
  AWAIT(payload, rejected->response.json()); EXPECT(rt::deepEqual(payload, expected));
  AWAIT(account, accounts::resolveClaudeCodeAccount(fixture.store)); EXPECT(account); AWAIT(state, quota::loadClaudeCodeQuotaState(fixture.store, account));
  EXPECT(state); EXPECT(!state->blocked_until_ms); EXPECT(state->cooldown_until_ms && *state->cooldown_until_ms > rt::dateNow());
}
ASYNC_TEST("a CLAUDE_API_KEY credential travels as x-api-key without the OAuth beta or a Bearer header") {
  Fixture fixture; fixture.apiKey = true; auto request = body(false);
  AWAIT(routed, fixture.call(request)); EXPECT(routed); EXPECT_EQ(fixture.captured.size(), 1u); const auto& sent = fixture.captured.front();
  EXPECT_EQ(sent.headers.get("x-api-key"), std::optional<std::string>("fixture-api-key")); EXPECT(!sent.headers.get("authorization")); EXPECT(!sent.headers.get("anthropic-beta"));
  EXPECT_EQ(sent.headers.get("anthropic-version"), std::optional<std::string>("2023-06-01")); EXPECT(!sent.headers.get("anthropic-workspace-id"));
  const auto systems = systemText(sent.body); EXPECT(std::find(systems.begin(), systems.end(), "You are Claude Code, Anthropic's official CLI for Claude.") == systems.end());
}
ASYNC_TEST("an organization-scoped key discovers the workspace once and the retry carries the header") {
  Fixture fixture; fixture.apiKey = true; int discovery = 0;
  const rt::Fetch fetcher([&](rt::RequestInfo input, rt::RequestInit init) {
    fixture.capture(input, init); if (requestUrl(input).ends_with("/v1/organizations/workspaces")) { ++discovery; return workspaces(); }
    return init.headers && init.headers->get("anthropic-workspace-id") ? success() : workspaceRequired();
  });
  auto request = body(); AWAIT(routed, fixture.call(request, fetcher)); EXPECT(routed); EXPECT_EQ(discovery, 1); EXPECT_EQ(fixture.captured.size(), 3u);
  EXPECT(!fixture.captured.front().headers.get("anthropic-workspace-id")); EXPECT_EQ(fixture.captured.back().headers.get("anthropic-workspace-id"), std::optional<std::string>(workspace));
  AWAIT(stored, credentials::resolveClaudeCodeWorkspace(fixture.store)); EXPECT(stored); EXPECT_EQ(stored->id, workspace); EXPECT_EQ(stored->source, "discovered");
  fixture.captured.clear(); AWAIT(second, fixture.call(request, fetcher)); EXPECT(second); EXPECT_EQ(discovery, 1); EXPECT_EQ(fixture.captured.size(), 1u);
  EXPECT_EQ(fixture.captured.front().headers.get("anthropic-workspace-id"), std::optional<std::string>(workspace));
}
ASYNC_TEST("the workspace discovery retry keeps the single-use dispatch reservation to one claim") {
  Fixture fixture; fixture.apiKey = true; int claims = 0;
  fixture.usage.beforeProviderDispatch = [&](std::string) { ++claims; return rt::fulfilledTask(std::optional<uos::api_key_policy::ApiKeyProviderDispatch>{}); };
  const rt::Fetch fetcher([&](rt::RequestInfo input, rt::RequestInit init) {
    fixture.capture(input, init); if (requestUrl(input).ends_with("/v1/organizations/workspaces")) return workspaces();
    return init.headers && init.headers->get("anthropic-workspace-id") ? success() : workspaceRequired();
  });
  auto request = body(); AWAIT(routed, fixture.call(request, fetcher)); EXPECT(routed); EXPECT_EQ(claims, 1);
  EXPECT_EQ(std::count_if(fixture.captured.begin(), fixture.captured.end(), [](const auto& entry) { return entry.url.ends_with("/v1/messages"); }), 2);
}
ASYNC_TEST("a billing rejection becomes an operator-visible configuration state and skips the transport until cleared") {
  Fixture fixture; fixture.apiKey = true;
  const rt::Fetch billing([&](rt::RequestInfo input, rt::RequestInit init) {
    fixture.capture(input, init); return rt::Response::json(json::object{{"type", "error"}, {"error", json::object{{"type", "invalid_request_error"},
      {"message", "Your credit balance is too low to access the Anthropic API. Please go to Plans & Billing to upgrade or purchase credits."}}}}, {.status = 400});
  });
  auto request = body(); AWAIT(rejected, fixture.call(request, billing)); EXPECT(rejected); EXPECT_EQ(rejected->response.status(), 400);
  AWAIT(account, accounts::resolveClaudeCodeAccount(fixture.store)); EXPECT(account); AWAIT(state, quota::loadClaudeCodeQuotaState(fixture.store, account));
  EXPECT(state && state->last_error && state->last_error->message); EXPECT_EQ(state->last_error->kind, "billing_blocked");
  EXPECT(state->last_error->message->find("credit balance") != std::string::npos); EXPECT(!state->blocked_until_ms); EXPECT(state->cooldown_until_ms && *state->cooldown_until_ms > rt::dateNow());
  const auto calls = fixture.captured.size(); AWAIT(second, fixture.call(request, billing)); EXPECT(second); EXPECT_EQ(second->response.status(), 400); EXPECT_EQ(fixture.captured.size(), calls);
  AWAIT(cleared, quota::clearClaudeCodeQuotaGuards(fixture.store, account)); (void)cleared;
  AWAIT(afterClear, fixture.call(request, billing)); EXPECT(afterClear); EXPECT_EQ(afterClear->response.status(), 400); EXPECT_EQ(fixture.captured.size(), calls + 1);
}
