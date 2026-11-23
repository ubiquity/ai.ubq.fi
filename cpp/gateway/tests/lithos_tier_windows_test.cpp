// Links: tests/lithos_tier_windows_test.cpp
// src/provider/lithos_rate_limits.cpp src/provider/capacity_events.cpp src/provider/capacity.cpp src/provider/claude_capacity_rollups.cpp
// src/http.cpp src/provider/capacity_parse.cpp src/provider/lithos_handlers.cpp src/request_policy.cpp
// src/openai_telemetry.cpp src/deepseek/context_admission_failure.cpp src/provider/capacity_rollups.cpp src/provider/claude_capacity_history.cpp
// src/provider/capacity_history.cpp src/provider/capacity_analytics.cpp src/provider/lithos.cpp src/config.cpp
// src/provider/claude_code_usage_schema.cpp src/provider/claude_code_accounts.cpp src/deepseek/forwarded_payload_policy.cpp src/openai.cpp
// src/models/codex_models.cpp src/upstream_wire.cpp src/openai_schema.cpp src/provider/claude_code_quota.cpp
// tests/helpers/loopback_http.cpp src/provider/claude_code_credentials.cpp src/defaults.cpp src/deepseek/responses.cpp
// src/provider/selection.cpp src/release.cpp src/provider/capacity_contract.cpp src/lib/jev_compaction/codex_items.cpp
// src/provider/health.cpp src/provider/claude_code_models.cpp src/deepseek/chat_projection.cpp src/provider/cerebras.cpp
// src/provider/lithos_streams.cpp src/runtime/http.cpp src/chat/stream_translation.cpp src/deepseek/index.cpp
// src/codex/banked_reset.cpp src/provider/surplus_claude_request.cpp src/utils.cpp src/cache/prompt_analytics_core.cpp
// src/deepseek/handlers.cpp src/responses_stream.cpp src/paid_fallback/ledger_stats.cpp src/provider/surplus.cpp
// src/metered_quota.cpp src/runtime_config.cpp src/types.cpp src/codex/banked_reset_provider.cpp
// src/provider/cerebras_rate_limits.cpp src/lib/jev_compaction/compact.cpp src/provider/claude_code.cpp src/kv.cpp
// src/auth/relay.cpp src/provider/removed_provider_telemetry.cpp src/runtime/http_tasks.cpp src/codex/capacity_routing.cpp
// src/provider/surplus_claude_chat_request.cpp src/runtime/http_early_sse.cpp src/codex/routing_state.cpp src/authority/native_codex_contracts.cpp
// src/runtime/http_body.cpp src/lib/jev_compaction/state.cpp src/inference_deadline.cpp src/bounded_response_body.cpp
// src/models/codex_models_fetch.cpp src/deepseek/stream.cpp src/codex/experiment.cpp src/codex/auth_refresh.cpp
// src/responses_failover_stream.cpp src/authority/native_codex_capacity.cpp src/deepseek/responses_payload.cpp src/cache/prompt_analytics_read.cpp
// src/provider/stream_relay.cpp src/runtime/http_upgrade.cpp src/models/codex_models_availability.cpp src/provider/metered.cpp
// src/codex/auth.cpp src/provider/anthropic_replay.cpp src/codex/external_auth.cpp src/codex/dispatch.cpp
// src/authority/native_codex_evaluation.cpp src/lib/jev_compaction/types.cpp src/codex/native_auth.cpp src/provider/anthropic_chat_responses.cpp
// src/deepseek/responses_stream.cpp src/codex/responses_error.cpp src/provider/anthropic_response_wire.cpp src/codex/reset_settings.cpp
// src/codex/native_auth_transport.cpp src/codex/routing_mutations.cpp src/provider/anthropic_responses.cpp src/codex/rate_limit_429.cpp
// src/runtime/websocket.cpp src/authority/native_codex_mutations.cpp src/lib/jev_compaction/request.cpp src/codex/account_routing.cpp
// src/authority/native_codex_429.cpp src/codex/routing_serial.cpp src/codex/routing_evaluation.cpp src/storage/kv.cpp
// src/provider/anthropic_chat_stream.cpp src/codex/overage_settings.cpp src/storage/sqlite_readonly.cpp src/authority/native_codex.cpp
// src/storage/native_kv.cpp
// Port of tests/lithos-tier-windows.test.ts. Fixture HTTP uses only task-owned port:0 listeners.
#include "uos/provider/lithos_handlers.hpp"
#include "uos/provider/lithos_rate_limits.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/runtime/http.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "helpers/loopback_http.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"
#include <array>
#include <algorithm>

namespace {
  namespace rt = uos::rt; namespace json = uos::json; namespace limits = uos::provider::lithos_rate_limits;
  namespace handlers = uos::provider::lithos_handlers; namespace http = uos::runtime::http;
  using namespace limits;
  const std::string ULTRA = "deepseek-ai/DeepSeek-V4.1-Flash-ultra", FAST = "deepseek-ai/DeepSeek-V4.1-Flash-fast", BASE = "deepseek-ai/DeepSeek-V4.1-Flash";
  const std::array<bool, 2> WIRES{true, false};
  const std::array<std::pair<double, double>, 2> RESET_ORDERS{{{60'000, 1'000}, {1'000, 60'000}}};
  struct CleanWindows { CleanWindows() { clearLithosFailoverWindows(); } ~CleanWindows() { clearLithosFailoverWindows(); } };
  rt::Response completion(const std::string& model) {
    json::object body{{"id", "chatcmpl-tier-window"}, {"object", "chat.completion"}, {"created", 1'790'160'326}, {"model", model},
      {"choices", json::array{json::object{{"index", 0}, {"message", json::object{{"role", "assistant"}, {"content", "fixture answer"}, {"reasoning_content", "fixture reasoning"}, {"tool_calls", nullptr}}}, {"finish_reason", "stop"}}}},
      {"usage", json::object{{"prompt_tokens", 11}, {"completion_tokens", 7}, {"total_tokens", 18}}}};
    return rt::Response(rt::jsonStringify(body), {.headers = rt::Headers{{"content-type", "application/json"}}});
  }
  rt::Response refusal(double waitMs, bool retryable = true) {
    rt::Headers headers{{"retry-after-ms", rt::numberToString(waitMs)}, {"content-type", "application/json"}}; if (!retryable) headers.set("x-should-retry", "false");
    return rt::Response(R"({"error":{"message":"Fixture input token limit","type":"input_tokens","code":"rate_limit_exceeded"}})", {.status = 429, .headers = headers});
  }
  struct HttpFixture {
    std::vector<std::string> models;
    std::function<rt::Response(const std::string&, std::size_t)> handler = [](const std::string& model, std::size_t) { return completion(model); };
  };
  rt::Task<rt::Response> serveFixture(std::shared_ptr<HttpFixture> fixture, rt::Request request) {
    EXPECT_EQ(request.headers().get("authorization"), "Bearer lith_sk_tier_window_fixture");
    auto body = co_await request.json(); auto model = std::string(rt::getString(body, "model").value()); fixture->models.push_back(model);
    co_return fixture->handler(model, fixture->models.size());
  }
  rt::Task<void> withHttpFixture(std::function<rt::Task<void>(std::shared_ptr<HttpFixture>)> run) {
    CleanWindows windows;
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/c1-lithos"}, {"LITHOSAI_API_KEY", "lith_sk_tier_window_fixture"}}};
    uos::config::config.codexAuthJsonB64.clear(); uos::kv::setKvForTest(std::make_shared<rt::MemoryKv>(std::function<double()>(rt::dateNow)));
    uos::provider::health::resetProviderHealthThrottleForTest();
    uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest([](const auto&) {});
    auto fixture = std::make_shared<HttpFixture>();
    http::ServeHttpOptions options; options.hostname = "127.0.0.1"; options.port = 0; options.onListen = [](const auto&) {};
    http::ServeHandler handler = [fixture](rt::Request request, http::ServeHandlerInfo) { return serveFixture(fixture, std::move(request)); };
    auto server = http::serveHttp(options, handler); auto address = co_await server.ready.wait(); EXPECT(address.port != 7999 && address.port != 8000);
    uos::tests::helpers::loopback_http::ScopedLoopbackFetch network; const auto original = rt::globalFetch();
    const auto fixtureUrl = "http://127.0.0.1:" + rt::numberToString(address.port) + "/chat/completions";
    rt::GlobalFetchOverride fetch(rt::Fetch([original, fixtureUrl](rt::RequestInfo input, rt::RequestInit init) {
      const auto url = std::visit([](const auto& value) -> std::string { using T = std::decay_t<decltype(value)>; if constexpr (std::is_same_v<T, rt::Request>) return value.url(); else if constexpr (std::is_same_v<T, rt::URL>) return value.href(); else return value; }, input);
      EXPECT_EQ(url, std::string(uos::provider::lithos::LITHOS_CHAT_COMPLETIONS_URL)); return original(fixtureUrl, std::move(init));
    }));
    rt::Reason failure; try { auto task = run(fixture); co_await std::move(task); } catch (...) { failure = std::current_exception(); }
    auto stopping = server.shutdown(); co_await stopping.wait(); co_await server.finished.wait();
    uos::kv::setKvForTest(nullptr); uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest({});
    if (failure) std::rethrow_exception(failure);
  }
  rt::Task<rt::Response> dispatch(bool chat, std::string model, bool stream = false, handlers::HandleLithosResponsesOptions options = {}) {
    json::object body{{"model", model}}; if (chat) body["messages"] = json::array{json::object{{"role", "user"}, {"content", "hi"}}}; else body["input"] = "hi";
    body["stream"] = stream; auto request = rt::Request("https://ai.ubq.fi/v1/" + std::string(chat ? "chat/completions" : "responses"), {.method = "POST", .headers = rt::Headers{{"content-type", "application/json"}}, .body = rt::jsonStringify(body)});
    auto task = chat ? handlers::handleLithosChatCompletions(request, body, model) : handlers::handleLithosResponses(request, body, model, nullptr, options);
    auto response = co_await std::move(task); co_return response;
  }
}
TEST("Lithos tier windows expire independently in both reset orders and model families") {
  CleanWindows cleanup; const std::array<std::string, 2> families{BASE, "moonshotai/Kimi-K3"};
  for (const auto& family : families) for (const auto& [ultraWait, fastWait] : RESET_ORDERS) {
    auto ultra = family + "-ultra", fast = family + "-fast"; clearLithosFailoverWindows();
    lithosOpenFailoverWindow(ultra, 1000, ultraWait, fast); lithosOpenFailoverWindow(ultra, 1000, fastWait, family);
    EXPECT_EQ(lithosFailoverTargetAt(ultra, 1999), std::optional<std::string>(family));
    EXPECT_EQ(lithosFailoverTargetAt(ultra, 2000), ultraWait == 1000 ? std::nullopt : std::optional<std::string>(fast));
    EXPECT_EQ(lithosFailoverTargetAt(fast, 2000), fastWait == 1000 ? std::nullopt : std::optional<std::string>(family));
    EXPECT(!lithosFailoverTargetAt(ultra, 61000)); EXPECT(!lithosFailoverTargetAt(fast, 61000));
  }
}
TEST("out-of-order shallow refusals cannot borrow the fast deadline and same-rung windows never shorten") {
  CleanWindows cleanup;
  lithosOpenFailoverWindow(ULTRA, 1000, 60000, BASE); lithosOpenFailoverWindow(ULTRA, 1500, 1000, FAST);
  EXPECT_EQ(lithosFailoverTargetAt(ULTRA, 2499), std::optional<std::string>(BASE)); EXPECT(!lithosFailoverTargetAt(ULTRA, 2500)); EXPECT_EQ(lithosFailoverTargetAt(FAST, 2500), std::optional<std::string>(BASE));
  lithosOpenFailoverWindow(FAST, 3000, 1000, BASE); EXPECT_EQ(lithosFailoverTargetAt(FAST, 60999), std::optional<std::string>(BASE)); EXPECT(!lithosFailoverTargetAt(FAST, 61000));
  lithosOpenFailoverWindow(FAST, 61000, 1000, BASE); lithosOpenFailoverWindow(ULTRA, 61000, 60000, FAST);
  EXPECT_EQ(lithosFailoverTargetAt(ULTRA, 61999), std::optional<std::string>(BASE)); EXPECT_EQ(lithosFailoverTargetAt(ULTRA, 62000), std::optional<std::string>(FAST));
  lithosOpenFailoverWindow(ULTRA, 62000, 1000, FAST); EXPECT_EQ(lithosFailoverTargetAt(ULTRA, 120999), std::optional<std::string>(FAST)); EXPECT(!lithosFailoverTargetAt(ULTRA, 121000));
}
TEST("illegal and nonfinite windows are ignored, base stays legal, and clear resets all rungs") {
  CleanWindows cleanup; const std::array<double, 5> waits{rt::Number::NaN, rt::Number::POSITIVE_INFINITY, rt::Number::NEGATIVE_INFINITY, 0, -1};
  for (auto wait : waits) lithosOpenFailoverWindow(ULTRA, 1000, wait, FAST);
  const std::array<double, 3> times{rt::Number::NaN, rt::Number::POSITIVE_INFINITY, rt::Number::NEGATIVE_INFINITY}; for (auto now : times) lithosOpenFailoverWindow(ULTRA, now, 1000, FAST);
  lithosOpenFailoverWindow(ULTRA, rt::Number::MAX_VALUE, rt::Number::MAX_VALUE, FAST);
  const std::array<std::string, 3> targets{ULTRA, "moonshotai/Kimi-K3", ULTRA + "-chat"}; for (const auto& target : targets) lithosOpenFailoverWindow(ULTRA, 1000, 60000, target);
  lithosOpenFailoverWindow(BASE, 1000, 60000, FAST); EXPECT(!lithosFailoverTargetAt(ULTRA, 1000)); EXPECT(!lithosFailoverTargetAt(BASE, 1000));
  lithosOpenFailoverWindow(ULTRA, 1000, 60000, FAST); lithosOpenFailoverWindow(FAST, 1000, 60000, BASE);
  EXPECT_EQ(lithosFailoverTargetAt(ULTRA, 1000), std::optional<std::string>(BASE)); EXPECT(!lithosFailoverTargetAt(BASE, 1000)); clearLithosFailoverWindows();
  EXPECT(!lithosFailoverTargetAt(ULTRA, 1000)); EXPECT(!lithosFailoverTargetAt(FAST, 1000));
}
ASYNC_TEST("a waterfall refusal returns immediately instead of spending the vendor reset window") {
  auto run = [](std::shared_ptr<HttpFixture> fixture) -> rt::Task<void> {
    fixture->handler = [](const std::string&, std::size_t) { return refusal(60000); }; double started = rt::performanceNow();
    handlers::HandleLithosResponsesOptions options{LITHOS_IMMEDIATE_FAILOVER_WAIT_POLICY}; auto task = dispatch(false, BASE, false, options); auto response = co_await std::move(task);
    EXPECT_EQ(response.status(), 429); EXPECT(rt::performanceNow() - started < 2000); EXPECT_EQ(fixture->models, std::vector<std::string>{BASE});
    if (auto body = response.body()) { auto cancellation = body->cancel(); co_await std::move(cancellation); }
  }; auto task = withHttpFixture(run); co_await std::move(task);
}
ASYNC_TEST("real Lithos Chat and Responses HTTP dispatch returns to each recovered rung at its own reset") {
  const auto originalNow = rt::globalDateNow();
  for (bool chat : WIRES) for (const auto& [ultraWait, fastWait] : RESET_ORDERS) {
    double nowMs = originalNow(); const double started = nowMs; rt::globalDateNow() = [&nowMs] { return nowMs; };
    auto run = [chat, ultraWait, fastWait, started, &nowMs](std::shared_ptr<HttpFixture> fixture) -> rt::Task<void> {
      fixture->handler = [ultraWait, fastWait](const std::string& model, std::size_t index) { if (index == 1) return refusal(ultraWait); if (index == 2) return refusal(fastWait); return completion(model); };
      auto task = dispatch(chat, ULTRA); auto first = co_await std::move(task); EXPECT_EQ(first.status(), 200); auto firstText = co_await first.text(); (void)firstText;
      const std::vector<std::string> expectedFirst{ULTRA, FAST, BASE}; EXPECT_EQ(fixture->models, expectedFirst);
      fixture->handler = [](const std::string& model, std::size_t) { return completion(model); };
      struct Case { double elapsed; std::string requested; std::string expected; };
      const std::array<Case, 4> cases{{{999, ULTRA, BASE}, {1000, ULTRA, ultraWait == 1000 ? ULTRA : FAST}, {1000, FAST, fastWait == 1000 ? FAST : BASE}, {60000, ULTRA, ULTRA}}};
      for (const auto& entry : cases) {
        nowMs = started + entry.elapsed; auto count = fixture->models.size(); auto dispatchTask = dispatch(chat, entry.requested); auto response = co_await std::move(dispatchTask);
        EXPECT_EQ(response.status(), 200); auto text = co_await response.text(); (void)text; EXPECT_EQ(fixture->models.size(), count + 1); EXPECT_EQ(fixture->models.back(), entry.expected);
      }
    }; auto task = withHttpFixture(run); co_await std::move(task);
  }
}
ASYNC_TEST("a no-retry tier is not dispatched again after a later tier names the wait window") {
  auto run = [](std::shared_ptr<HttpFixture> fixture) -> rt::Task<void> {
    fixture->handler = [](const std::string& model, std::size_t index) { if (index == 1) return refusal(1, false); if (index <= 3) return refusal(1); return completion(model); };
    auto task = dispatch(true, ULTRA); auto response = co_await std::move(task); EXPECT_EQ(response.status(), 200); auto text = co_await response.text(); (void)text;
    const std::vector<std::string> expected{ULTRA, FAST, BASE, FAST}; EXPECT_EQ(fixture->models, expected);
  }; auto task = withHttpFixture(run); co_await std::move(task);
}
ASYNC_TEST("real Lithos HTTP transient failure does not fail over to another tier or provider") {
  auto run = [](std::shared_ptr<HttpFixture> fixture) -> rt::Task<void> {
    fixture->handler = [](const std::string&, std::size_t) { return rt::Response(R"({"error":{"message":"Fixture upstream unavailable","type":"server_error","code":"upstream_unavailable"}})", {.status = 503, .headers = rt::Headers{{"content-type", "application/json"}}}); };
    auto task = dispatch(true, ULTRA); auto response = co_await std::move(task); EXPECT_EQ(response.status(), 503); auto text = co_await response.text(); (void)text; EXPECT_EQ(fixture->models, std::vector<std::string>{ULTRA});
  }; auto task = withHttpFixture(run); co_await std::move(task);
}
ASYNC_TEST("real Lithos HTTP deferred 504 is preserved in both streamed terminals") {
  for (bool chat : WIRES) {
    auto run = [chat](std::shared_ptr<HttpFixture> fixture) -> rt::Task<void> {
      fixture->handler = [](const std::string&, std::size_t index) { if (index <= 3) return refusal(1); return rt::Response(R"({"error":{"message":"provider gateway timed out","code":"provider_gateway_timeout"}})", {.status = 504, .headers = rt::Headers{{"content-type", "application/json"}}}); };
      auto task = dispatch(chat, ULTRA, true); auto response = co_await std::move(task); auto text = co_await response.text(); EXPECT_EQ(response.status(), 200);
      const std::vector<std::string> expected{ULTRA, FAST, BASE, ULTRA}; EXPECT_EQ(fixture->models, expected); EXPECT(text.find("provider_gateway_timeout") != std::string::npos); EXPECT(text.find("provider gateway timed out") != std::string::npos);
      if (!chat) EXPECT(text.find("\"type\":\"response.failed\"") != std::string::npos);
    }; auto task = withHttpFixture(run); co_await std::move(task);
  }
}
ASYNC_TEST("619 abort reasons and existing bounded retry waits survive independent tier windows") {
  const std::array<std::string, 2> names{"TimeoutError", "AbortError"};
  for (const auto& name : names) {
    rt::AbortController controller; auto reason = rt::makeReason(rt::DOMException("Tier window fixture", name)); controller.abort(reason);
    auto pending = waitForLithosRetry(10000, controller.signal()); EXPECT_REJECTS_REASON(pending.wait(), reason);
    rt::AbortController waiting; auto active = waitForLithosRetry(10000, waiting.signal()); waiting.abort(reason); EXPECT_REJECTS_REASON(active.wait(), reason);
  }
  rt::Headers headers{{"retry-after-ms", "75000"}}; EXPECT_EQ(lithosRefusalWait(headers, 0, 0)->waitMs, 75000);
  EXPECT(!lithosRefusalWait(rt::Headers{{"retry-after-ms", "75001"}}, 0, 0)); EXPECT(!lithosRefusalWait(headers, 45001, 0)); EXPECT(!lithosRefusalWait(headers, 0, 2));
  EXPECT_EQ(lithosRefusalWait(headers, 225000, 4, LITHOS_STREAMED_REFUSAL_WAIT_POLICY)->waitMs, 75000);
  EXPECT(!lithosRefusalWait(headers, 225001, 4, LITHOS_STREAMED_REFUSAL_WAIT_POLICY)); EXPECT(!lithosRefusalWait(headers, 0, 5, LITHOS_STREAMED_REFUSAL_WAIT_POLICY));
}
