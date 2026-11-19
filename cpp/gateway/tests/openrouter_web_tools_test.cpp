// Links: tests/openrouter_web_tools_test.cpp
// src/provider/capacity_events.cpp src/config.cpp src/release.cpp src/kv.cpp
// src/provider/openrouter_web_tools.cpp src/utils.cpp src/provider/capacity_contract.cpp src/models/openrouter_models.cpp
// src/provider/health.cpp src/codex/banked_reset.cpp src/storage/kv.cpp
// src/provider/openrouter_handlers.cpp src/types.cpp src/provider/openrouter.cpp src/openai_telemetry.cpp
// src/upstream_wire.cpp src/codex/reset_settings.cpp src/provider/capacity.cpp src/provider/capacity_rollups.cpp
// src/provider/openrouter_streams.cpp src/storage/native_kv.cpp src/http.cpp src/codex/banked_reset_provider.cpp
// src/provider/capacity_history.cpp src/provider/claude_code_accounts.cpp src/codex/capacity_routing.cpp src/provider/claude_capacity_rollups.cpp
// src/provider/claude_code_quota.cpp src/storage/sqlite_readonly.cpp src/authority/native_codex_capacity.cpp src/provider/capacity_parse.cpp
// src/provider/claude_code_credentials.cpp src/provider/capacity_analytics.cpp src/authority/native_codex_contracts.cpp src/chat/stream_translation.cpp
// src/provider/claude_capacity_history.cpp src/provider/cerebras.cpp src/cache/prompt_analytics_core.cpp src/deepseek/index.cpp
// src/provider/lithos.cpp src/responses_failover_stream.cpp src/responses_stream.cpp src/authority/native_codex_evaluation.cpp
// src/openai.cpp src/codex/routing_state.cpp src/openai_schema.cpp src/provider/cerebras_rate_limits.cpp
// src/provider/claude_code_usage_schema.cpp src/provider/lithos_rate_limits.cpp src/request_policy.cpp src/defaults.cpp
// src/models/codex_models_fetch.cpp src/paid_fallback/ledger_stats.cpp src/codex/auth_refresh.cpp src/runtime_config.cpp
// src/codex/auth.cpp src/provider/removed_provider_telemetry.cpp src/codex/experiment.cpp src/bounded_response_body.cpp
// src/inference_deadline.cpp src/codex/dispatch.cpp src/models/codex_models.cpp src/codex/responses_error.cpp
// src/metered_quota.cpp src/provider/stream_relay.cpp src/codex/account_routing.cpp src/cache/prompt_analytics_read.cpp
// src/auth/relay.cpp src/codex/native_auth.cpp src/provider/metered.cpp src/authority/native_codex_mutations.cpp
// src/codex/routing_serial.cpp src/codex/routing_mutations.cpp src/models/codex_models_availability.cpp src/codex/external_auth.cpp
// src/provider/claude_code_models.cpp src/deepseek/responses_stream.cpp src/provider/surplus_claude_request.cpp src/provider/selection.cpp
// src/codex/rate_limit_429.cpp src/authority/native_codex.cpp src/deepseek/responses_payload.cpp src/deepseek/responses.cpp
// src/provider/surplus.cpp src/codex/native_auth_transport.cpp src/codex/routing_evaluation.cpp src/authority/native_codex_429.cpp
// src/runtime/websocket.cpp src/provider/claude_code.cpp src/provider/anthropic_responses.cpp src/provider/surplus_claude_chat_request.cpp
// src/provider/anthropic_replay.cpp src/codex/overage_settings.cpp src/provider/anthropic_chat_responses.cpp src/provider/anthropic_response_wire.cpp
// src/provider/anthropic_chat_stream.cpp
// src/provider/openrouter-web-tools.ts has no direct bun test (tests/openrouter-web-tools.test.ts
// drives it through openrouter-handlers): differential cases recorded from the TS under Bun 1.4.0.
#include "uos/provider/openrouter_web_tools.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/provider/openrouter_handlers.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/text_codec.hpp"

#include <string>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;

TEST("differential: withOpenRouterOpenAiWebTools") {
  struct Case {
    std::string body;
    std::string model;
    std::string expected;
  };
  const std::string both = R"([{"type":"openrouter:web_search"},{"type":"openrouter:web_fetch"}])";
  const std::vector<Case> cases = {
    {R"({"model":"x"})", "openai/gpt-5", R"({"model":"x","tools":)" + both + "}"},
    {R"({"model":"x"})", "~openai/gpt-5", R"({"model":"x","tools":)" + both + "}"},
    {R"({"model":"x"})", "anthropic/claude", R"({"model":"x"})"},
    {R"({"model":"x"})", "openai/gpt-5:batch", R"({"model":"x"})"},
    {R"({"model":"x"})", "openai/gpt-5:batched", R"({"model":"x","tools":)" + both + "}"},
    {R"({"model":"x","tool_choice":"auto"})", "openai/gpt-5", R"({"model":"x","tool_choice":"auto","tools":)" + both + "}"},
    {R"({"model":"x","tool_choice":"none"})", "openai/gpt-5", R"({"model":"x","tool_choice":"none"})"},
    {R"({"model":"x","tool_choice":null})", "openai/gpt-5", R"({"model":"x","tool_choice":null})"},
    {R"({"model":"x","functions":[]})", "openai/gpt-5", R"({"model":"x","functions":[]})"},
    {R"({"model":"x","function_call":null})", "openai/gpt-5", R"({"model":"x","function_call":null})"},
    {R"({"model":"x","tools":[]})", "openai/gpt-5", R"({"model":"x","tools":[]})"},
    {R"({"model":"x","tools":{}})", "openai/gpt-5", R"({"model":"x","tools":{}})"},
    {R"({"model":"x","tools":null})", "openai/gpt-5", R"({"model":"x","tools":null})"},
    {R"({"tools":[{"type":"function","function":{"name":"f"}}],"model":"x"})", "openai/gpt-5",
     R"({"tools":[{"type":"function","function":{"name":"f"}},{"type":"openrouter:web_search"},{"type":"openrouter:web_fetch"}],"model":"x"})"},
    {R"({"tools":[{"type":"web_search"}],"model":"x"})", "openai/gpt-5", R"({"tools":[{"type":"web_search"},{"type":"openrouter:web_fetch"}],"model":"x"})"},
    {R"({"tools":[{"type":"web_search_preview"},{"type":"openrouter:web_fetch"}],"model":"x"})", "openai/gpt-5",
     R"({"tools":[{"type":"web_search_preview"},{"type":"openrouter:web_fetch"}],"model":"x"})"},
    {R"({"tools":[{"type":"web_fetch_2025"},{"type":"openrouter:web_search"}]})", "openai/gpt-5",
     R"({"tools":[{"type":"web_fetch_2025"},{"type":"openrouter:web_search"}]})"},
    {R"({"tools":[[],"x",5,{"type":7},null]})", "openai/gpt-5",
     R"({"tools":[[],"x",5,{"type":7},null,{"type":"openrouter:web_search"},{"type":"openrouter:web_fetch"}]})"},
    {R"({"2":"index","tools":[{"type":"web_searchx"}],"a":1})", "openai/o3",
     R"({"2":"index","tools":[{"type":"web_searchx"},{"type":"openrouter:web_search"},{"type":"openrouter:web_fetch"}],"a":1})"},
    {R"({})", "openai/", R"({"tools":)" + both + "}"},
    {R"({})", "openai", R"({})"},
    {R"({})", "OpenAI/gpt", R"({})"},
  };
  for (const Case& entry : cases) {
    const json::value body = rt::jsonParse(entry.body);
    const json::object result = uos::provider::openrouter_web_tools::withOpenRouterOpenAiWebTools(body.get_object(), entry.model);
    EXPECT_EQ(rt::jsonStringify(result), entry.expected);
  }
}

namespace {
  namespace handlers = uos::provider::openrouter_handlers;
  namespace catalog = uos::models::openrouter_models;
  const std::array<bool, 2> WIRES{true, false};
  const std::array<bool, 2> STREAM_MODES{false, true};
  const std::string PRO_MODEL = "openai/gpt-6-astra-pro";
  const json::array DEFAULT_TOOLS{json::object{{"type", "openrouter:web_search"}}, json::object{{"type", "openrouter:web_fetch"}}};
  const std::vector<std::string> MODEL_IDS{PRO_MODEL, "openai/gpt-4o", PRO_MODEL + ":batch", "~openai/gpt-6.1-sol-pro", "vendor/alpha", "openaiish/model"};
  struct Fixture {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/c1-openrouter"}}};
    Fixture() {
      uos::config::config.codexAuthJsonB64.clear(); uos::kv::setKvForTest(std::make_shared<rt::MemoryKv>(std::function<double()>(rt::dateNow)));
      uos::provider::health::resetProviderHealthThrottleForTest(); catalog::resetOpenRouterModelsCacheForTest();
      uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest([](const auto&) {});
      catalog::setOpenRouterModelsFetchForTest(rt::Fetch([](rt::RequestInfo, rt::RequestInit) {
        json::array rows; for (const auto& id : MODEL_IDS) rows.push_back(json::object{{"id", id}});
        return rt::Response(rt::jsonStringify(json::object{{"data", rows}}), {.headers = rt::Headers{{"content-type", "application/json"}}});
      }));
    }
    ~Fixture() { catalog::setOpenRouterModelsFetchForTest({}); catalog::resetOpenRouterModelsCacheForTest(); uos::kv::setKvForTest(nullptr); uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest({}); }
  };
  rt::Task<void> servedCatalogue() { auto snapshot = co_await catalog::fetchOpenRouterModels(); EXPECT(snapshot); EXPECT_EQ(snapshot->models.size(), MODEL_IDS.size()); }
  rt::Response upstreamAnswer(bool chat, bool stream) {
    json::object usage = chat ? json::object{{"prompt_tokens", 3}, {"completion_tokens", 2}, {"total_tokens", 5}} : json::object{{"input_tokens", 3}, {"output_tokens", 2}, {"total_tokens", 5}};
    if (!stream) {
      json::object body = chat ? rt::jsonParse(R"({"id":"gen-tools","object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":"research ready"},"finish_reason":"stop"}]})").as_object()
        : rt::jsonParse(R"({"id":"resp_tools","object":"response","status":"completed","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"research ready"}]}]})").as_object();
      body["usage"] = usage; return rt::Response(rt::jsonStringify(body), {.headers = rt::Headers{{"content-type", "application/json"}}});
    }
    std::string frames;
    if (chat) frames = "data: " + rt::jsonStringify(rt::jsonParse(R"({"id":"gen-tools","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":"research ready"},"finish_reason":null}]})")) + "\n\n" +
      "data: " + rt::jsonStringify(json::object{{"id", "gen-tools"}, {"object", "chat.completion.chunk"}, {"choices", json::array{}}, {"usage", usage}}) + "\n\ndata: [DONE]\n\n";
    else frames = "event: response.output_text.delta\ndata: {\"type\":\"response.output_text.delta\",\"delta\":\"research ready\"}\n\nevent: response.completed\ndata: " +
      rt::jsonStringify(json::object{{"type", "response.completed"}, {"response", json::object{{"id", "resp_tools"}, {"usage", usage}}}}) + "\n\n";
    return rt::Response(frames, {.headers = rt::Headers{{"content-type", "text/event-stream"}}});
  }
  rt::Task<json::object> dispatch(bool chat, std::string model, json::object fields = {}) {
    json::object raw{{"model", model}};
    if (chat) raw["messages"] = json::array{json::object{{"role", "user"}, {"content", "research"}}}; else raw["input"] = "research";
    raw["stream"] = false; rt::objectAssign(raw, fields); const auto original = raw;
    struct Captured { std::optional<json::object> body; int calls = 0; }; auto captured = std::make_shared<Captured>();
    handlers::OpenRouterRouteTransport transport = [captured, chat, raw](const json::object& body, const uos::provider::openrouter::OpenRouterDispatchInit&) {
      ++captured->calls; captured->body = body; return rt::fulfilledTask(upstreamAnswer(chat, rt::getBool(raw, "stream") == true));
    };
    handlers::OpenRouterHandlerDeps deps{transport, transport};
    auto request = rt::Request("https://ai.ubq.fi" + std::string(chat ? "/v1/chat/completions" : "/v1/responses"), {.method = "POST"});
    auto task = chat ? handlers::handleOpenRouterChatCompletions(request, raw, model, nullptr, deps) : handlers::handleOpenRouterResponses(request, raw, model, nullptr, deps);
    auto response = co_await std::move(task); EXPECT_EQ(response.status(), 200); EXPECT_EQ(response.headers().get("x-uos-upstream"), "openrouter");
    auto text = co_await response.text(); EXPECT(text.find("research ready") != std::string::npos);
    EXPECT_EQ(captured->calls, 1); EXPECT(captured->body); EXPECT_EQ(rt::getString(*captured->body, "model"), model); EXPECT(rt::deepEqual(raw, original));
    co_return *captured->body;
  }
  json::object functionTool(bool chat) {
    json::object definition{{"name", "lookup_record"}, {"description", "Look up a private record"}, {"parameters", json::object{{"type", "object"}, {"properties", json::object{}}}}};
    if (chat) return json::object{{"type", "function"}, {"function", definition}};
    json::object result{{"type", "function"}}; rt::objectAssign(result, definition); return result;
  }
  void equalField(const json::object& value, const char* key, const json::value& expected) { const auto* actual = rt::get(value, key); EXPECT(actual); EXPECT(rt::deepEqual(*actual, expected)); }
}
ASYNC_TEST("OpenRouter dispatch gives current and older OpenAI models web tools on both wires and stream modes") {
  Fixture fixture; co_await servedCatalogue(); const std::vector<std::string> models{PRO_MODEL, "openai/gpt-4o", "~openai/gpt-6.1-sol-pro"};
  for (bool chat : WIRES) for (const auto& model : models) for (bool stream : STREAM_MODES) {
    json::object controls = chat ? json::object{{"reasoning_effort", "xhigh"}, {"max_completion_tokens", 4096}} : json::object{{"reasoning", json::object{{"effort", "xhigh"}}}, {"max_output_tokens", 4096}};
    auto fields = controls; fields["stream"] = stream; auto task = dispatch(chat, model, fields); auto body = co_await std::move(task);
    equalField(body, "tools", DEFAULT_TOOLS); EXPECT_EQ(rt::getBool(body, "stream"), stream); EXPECT(!rt::hasOwn(body, "tool_choice"));
    for (const auto& entry : controls) equalField(body, entry.key().data(), entry.value());
  }
}
ASYNC_TEST("OpenRouter web defaults retain client function tools, schemas, and automatic tool controls") {
  Fixture fixture; co_await servedCatalogue();
  for (bool chat : WIRES) {
    auto clientTool = functionTool(chat); json::array tools{clientTool};
    json::object fields{{"tools", tools}, {"tool_choice", "auto"}, {"parallel_tool_calls", false}}; if (!chat) fields["max_tool_calls"] = 7;
    auto task = dispatch(chat, PRO_MODEL, fields); auto body = co_await std::move(task); json::array expected{clientTool}; for (const auto& tool : DEFAULT_TOOLS) expected.push_back(tool);
    equalField(body, "tools", expected); EXPECT(rt::deepEqual((*rt::getArray(body, "tools"))[0], clientTool));
    equalField(body, "tool_choice", "auto"); equalField(body, "parallel_tool_calls", false);
    if (!chat) equalField(body, "max_tool_calls", 7); else EXPECT(!rt::hasOwn(body, "max_tool_calls"));
    body["tools"].as_array()[0] = nullptr; EXPECT(rt::deepEqual(tools[0], clientTool));
  }
}
ASYNC_TEST("OpenRouter retains configured server and native web tools without adding duplicate equivalents") {
  Fixture fixture; co_await servedCatalogue();
  const std::vector<std::string> searchTypes{"openrouter:web_search", "web_search", "web_search_preview", "web_search_2025_08_26", "web_search_preview_2025_03_11"};
  const std::vector<std::string> fetchTypes{"openrouter:web_fetch", "web_fetch", "web_fetch_20250910"};
  for (bool chat : WIRES) {
    for (const auto& type : searchTypes) {
      json::object search{{"type", type}, {"parameters", json::object{{"engine", "exa"}, {"allowed_domains", json::array{"example.com"}}, {"max_results", 2}}}};
      json::object fields{{"tools", json::array{search}}}; auto task = dispatch(chat, PRO_MODEL, fields); auto searchOnly = co_await std::move(task);
      equalField(searchOnly, "tools", json::array{search, json::object{{"type", "openrouter:web_fetch"}}});
      for (const auto& fetchType : fetchTypes) {
        json::object fetchTool{{"type", fetchType}, {"parameters", json::object{{"max_uses", 3}, {"max_content_tokens", 500}}}};
        json::array tools{search, fetchTool}; json::object both{{"tools", tools}}; auto bothTask = dispatch(chat, PRO_MODEL, both); auto complete = co_await std::move(bothTask); equalField(complete, "tools", tools);
      }
    }
    json::object fetchTool{{"type", "openrouter:web_fetch"}, {"parameters", json::object{{"blocked_domains", json::array{"example.com"}}}}};
    json::object fields{{"tools", json::array{fetchTool}}}; auto task = dispatch(chat, PRO_MODEL, fields); auto body = co_await std::move(task);
    equalField(body, "tools", json::array{fetchTool, json::object{{"type", "openrouter:web_search"}}});
  }
}
ASYNC_TEST("OpenRouter defaults respect empty tools and disabled, required, or constrained tool choices") {
  Fixture fixture; co_await servedCatalogue();
  for (bool chat : WIRES) {
    json::object emptyFields{{"tools", json::array{}}}; auto emptyTask = dispatch(chat, PRO_MODEL, emptyFields); auto empty = co_await std::move(emptyTask); equalField(empty, "tools", json::array{});
    json::array tools{functionTool(chat)}; json::object forced = chat ? json::object{{"type", "function"}, {"function", json::object{{"name", "lookup_record"}}}} : json::object{{"type", "function"}, {"name", "lookup_record"}};
    const json::array choices{"none", "required", forced, json::object{{"type", "allowed_tools"}, {"mode", "auto"}, {"tools", json::array{forced}}}, nullptr, "invalid-choice"};
    for (const auto& choice : choices) { json::object fields{{"tools", tools}, {"tool_choice", choice}}; auto task = dispatch(chat, PRO_MODEL, fields); auto body = co_await std::move(task); equalField(body, "tools", tools); equalField(body, "tool_choice", choice); }
    json::object fields{{"tool_choice", "required"}}; auto task = dispatch(chat, PRO_MODEL, fields); auto body = co_await std::move(task); EXPECT(!rt::hasOwn(body, "tools"));
  }
}
ASYNC_TEST("OpenRouter defaults preserve deprecated function controls and malformed tools for upstream validation") {
  Fixture fixture; co_await servedCatalogue();
  const std::vector<json::object> legacy{json::object{{"functions", json::array{json::object{{"name", "legacy_lookup"}}}}}, json::object{{"functions", json::array{}}},
    json::object{{"function_call", "none"}}, json::object{{"function_call", json::object{{"name", "legacy_lookup"}}}}};
  for (bool chat : WIRES) {
    for (const auto& fields : legacy) { auto task = dispatch(chat, PRO_MODEL, fields); auto body = co_await std::move(task); EXPECT(!rt::hasOwn(body, "tools")); for (const auto& entry : fields) equalField(body, entry.key().data(), entry.value()); }
    const json::array malformed{nullptr, "not-an-array", json::object{{"type", "function"}}, 42};
    for (const auto& tools : malformed) { json::object fields{{"tools", tools}}; auto task = dispatch(chat, PRO_MODEL, fields); auto body = co_await std::move(task); equalField(body, "tools", tools); }
  }
}
ASYNC_TEST("OpenRouter leaves batch and other vendors unchanged while retaining their explicit tools") {
  Fixture fixture; co_await servedCatalogue(); const std::vector<std::string> models{PRO_MODEL + ":batch", "vendor/alpha", "openaiish/model"};
  for (bool chat : WIRES) for (const auto& model : models) {
    auto task = dispatch(chat, model); auto body = co_await std::move(task); EXPECT(!rt::hasOwn(body, "tools"));
    json::array tools{functionTool(chat)}; json::object fields{{"tools", tools}}; auto controlled = dispatch(chat, model, fields); auto result = co_await std::move(controlled); equalField(result, "tools", tools);
  }
}
