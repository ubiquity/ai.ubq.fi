// Links: tests/claude_header_meter_test.cpp
// src/provider/claude_code_usage.cpp
// src/admin/claude_code.cpp src/api_keys.cpp src/auth/relay.cpp src/authority/native_codex.cpp
// src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp
// src/authority/native_codex_mutations.cpp src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp src/authority/native_paid_policy.cpp
// src/bounded_response_body.cpp src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_read.cpp src/codex/account_routing.cpp
// src/codex/auth.cpp src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_provider.cpp
// src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp
// src/codex/native_auth.cpp src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp
// src/codex/reset_settings.cpp src/codex/responses_error.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp
// src/codex/routing_serial.cpp src/codex/routing_state.cpp src/config.cpp src/deepseek/flash_aliases.cpp
// src/deepseek/index.cpp src/deepseek/waterfall.cpp src/defaults.cpp src/health.cpp
// src/http.cpp src/kv.cpp src/metered_quota.cpp src/models/codex_models.cpp
// src/models/codex_models_availability.cpp src/models/codex_models_fetch.cpp src/models/openrouter_models.cpp src/openai_telemetry.cpp
// src/paid_fallback/claude_code_route.cpp src/paid_fallback/health.cpp src/paid_fallback/index.cpp src/paid_fallback/ledger_admission.cpp
// src/paid_fallback/ledger_backfill.cpp src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp
// src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp
// src/provider/anthropic_responses.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp
// src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp
// src/provider/cerebras.cpp src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp
// src/provider/claude_code.cpp src/provider/claude_code_access.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp
// src/provider/claude_code_models.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/health.cpp
// src/provider/lithos.cpp src/provider/lithos_models.cpp src/provider/lithos_rate_limits.cpp src/provider/metered.cpp
// src/provider/openrouter.cpp src/provider/removed_provider_telemetry.cpp src/provider/selection.cpp src/provider/surplus.cpp
// src/provider/surplus_claude_chat_request.cpp src/provider/surplus_claude_request.cpp src/release.cpp src/request.cpp
// src/request_policy.cpp src/responses_failover_stream.cpp src/responses_stream.cpp src/runtime/http.cpp
// src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp
// src/runtime/websocket.cpp src/runtime_config.cpp src/storage/kv.cpp
// src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/types.cpp src/upstream_wire.cpp
// src/utils.cpp tests/helpers/loopback_http.cpp
// Original header-meter cases retain a durable fixture store and real ephemeral loopback HTTP.
#include "uos/admin/claude_code.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/paid_fallback/claude_code_route.hpp"
#include "uos/provider/capacity.hpp"
#include "uos/provider/claude_capacity_history.hpp"
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/provider/claude_code_usage.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/runtime/http.hpp"
#include "uos/storage/kv.hpp"
#include "helpers/loopback_http.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace accounts = uos::provider::claude_code_accounts;
  namespace credentials = uos::provider::claude_code_credentials;
  namespace quota = uos::provider::claude_code_quota;
  namespace capacity = uos::provider::capacity;
  namespace history = uos::provider::claude_capacity_history;
  namespace http = uos::runtime::http;
  auto env() {
    return [](const std::string& name) -> std::optional<std::string> { return name == "CLAUDE_CODE_ALLOWED_KEY_IDS" ? std::optional<std::string>("header-owner") : std::nullopt; };
  }
  const std::string model = "claude-opus-5-5";
  std::string urlOf(const rt::RequestInfo& input) {
    return std::visit([](const auto& value) -> std::string {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, std::string>) return value;
      else if constexpr (std::is_same_v<T, rt::URL>) return value.href();
      else return value.url();
    }, input);
  }
  json::value message() { return rt::jsonParse(R"({"id":"msg_headers","type":"message","role":"assistant","model":"claude-opus-5-5","content":[{"type":"text","text":"OK"}],"stop_reason":"end_turn","stop_sequence":null,"usage":{"input_tokens":1,"output_tokens":1}})"); }
  std::string sse() {
    auto started = message().as_object(); started["content"] = json::array{}; started["stop_reason"] = nullptr;
    const json::array events{json::object{{"type", "message_start"}, {"message", started}},
      json::object{{"type", "content_block_start"}, {"index", 0}, {"content_block", json::object{{"type", "text"}, {"text", ""}}}},
      json::object{{"type", "content_block_delta"}, {"index", 0}, {"delta", json::object{{"type", "text_delta"}, {"text", "OK"}}}},
      json::object{{"type", "content_block_stop"}, {"index", 0}},
      json::object{{"type", "message_delta"}, {"delta", json::object{{"stop_reason", "end_turn"}, {"stop_sequence", nullptr}}}, {"usage", json::object{{"output_tokens", 1}}}},
      json::object{{"type", "message_stop"}}};
    std::string result; for (const auto& event : events) result += "event: " + std::string(*rt::getString(event, "type")) + "\ndata: " + rt::jsonStringify(event) + "\n\n"; return result;
  }
  struct Reply { double status = 200; rt::Headers headers; int requests = 0; };
  std::string headerFixtureTaskScratch() {
    const auto scratch = rt::env::get("TMPDIR");
    if (!scratch || scratch->empty()) throw rt::JsError("Header fixture requires the wrapper TMPDIR");
    return *scratch;
  }
  struct HeaderFixture {
    const std::string taskScratch = headerFixtureTaskScratch();
    rt::env::ScopedEnvironment isolation{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c1-header"}, {"TMPDIR", taskScratch}}};
    std::string savedConfig = uos::config::config.codexAuthJsonB64;
    uos::tests::helpers::loopback_http::ScopedLoopbackFetch loopback;
    std::shared_ptr<rt::Kv> store;
    std::optional<accounts::ClaudeCodeAccountBinding> account;
    std::shared_ptr<Reply> reply = std::make_shared<Reply>();
    std::optional<http::HttpServer> server;
    std::string directory, path;
    uos::openai_telemetry::UsageContext context;
    HeaderFixture() { uos::config::config.codexAuthJsonB64.clear(); context.keyId = "header-owner"; }
    ~HeaderFixture() { uos::config::config.codexAuthJsonB64 = savedConfig; }
    rt::Task<void> initialize() {
      EXPECT_EQ(rt::env::get("TMPDIR"), std::optional<std::string>(taskScratch));
      const auto scratchRoot = rt::path::resolve(taskScratch); EXPECT_EQ(rt::path::resolve(rt::os::tmpdir()), scratchRoot);
      directory = rt::fs::mkdtempSync(rt::os::tmpdir() + "/claude-header-meter-"); path = directory + "/fixture.sqlite3";
      EXPECT_EQ(rt::path::dirname(directory), scratchRoot);
      auto opening = uos::storage::kv::openKv(path); store = co_await std::move(opening); uos::kv::setKvForTest(store);
      uos::provider::selection::resetProviderSelectionCacheForTest(); uos::provider::health::resetProviderHealthThrottleForTest();
      credentials::resetClaudeCodeCredentialCacheForTest(); uos::provider::claude_code_models::resetClaudeCodeModelsCacheForTest();
      credentials::StoreClaudeCodeStoredCredentialInput input; input.secret = std::string("sk") + "-ant-oat-fixture-normal-header-traffic";
      auto storing = credentials::storeClaudeCodeStoredCredential(store, input); const auto stored = co_await std::move(storing); (void)stored;
      const rt::KvValue selection(json::object{{"provider_ids", json::array{"claude_code"}}, {"updated_at_ms", rt::dateNow()}});
      auto selecting = store->set(uos::provider::selection::PROVIDER_SELECTION_KV_KEY, selection); const auto selected = co_await std::move(selecting); (void)selected;
      auto resolving = accounts::resolveClaudeCodeAccount(store, env()); account = co_await std::move(resolving); EXPECT(account);
      http::ServeHttpOptions options; options.hostname = "127.0.0.1"; options.port = 0;
      const auto state = reply;
      const http::ServeHandler handler = [state](rt::Request request, http::ServeHandlerInfo) -> rt::Task<rt::Response> {
        ++state->requests; auto reading = request.json(); const auto body = co_await std::move(reading);
        if (state->status >= 400) co_return rt::Response::json(json::object{{"type", "error"}, {"error", json::object{{"type", "rate_limit_error"}, {"message", "fixture rejection"}}}}, {.status = state->status, .headers = state->headers});
        auto headers = state->headers;
        if (rt::getBool(body, "stream") == true) { headers.set("Content-Type", "text/event-stream"); co_return rt::Response(sse(), {.headers = headers}); }
        co_return rt::Response::json(message(), {.headers = headers});
      };
      server = http::serveHttp(options, handler); const auto ready = co_await server->ready.wait(); (void)ready;
    }
    rt::Task<double> infer(bool stream) {
      json::object body{{"model", model}, {"input", "ordinary fixture request"}, {"stream", stream}};
      const auto origin = "http://127.0.0.1:" + rt::numberToString(server->addr().port) + "/messages";
      uos::paid_fallback::claude_code_route::SubscriptionOptions options; options.model = model; options.env = env(); options.usageContext = &context;
      options.fetcher = rt::Fetch([origin](rt::RequestInfo input, rt::RequestInit init) { EXPECT_EQ(urlOf(input), "https://api.anthropic.com/v1/messages"); return rt::fetch(origin, std::move(init)); });
      auto inferring = uos::paid_fallback::claude_code_route::attemptClaudeCodeSubscription(body, options); const auto routed = co_await std::move(inferring); EXPECT(routed);
      auto reading = routed->response.text(); const auto delivered = co_await std::move(reading); (void)delivered; co_return routed->response.status();
    }
    rt::Task<uos::provider::capacity_contract::ProviderCapacityClaudeSource> view(std::optional<double> now = std::nullopt) { return capacity::claudeCapacitySource(store, now.value_or(rt::dateNow()), env()); }
    rt::Task<void> reopen() {
      store->close(); auto opening = uos::storage::kv::openKv(path); store = co_await std::move(opening); uos::kv::setKvForTest(store);
    }
    rt::Task<void> close() {
      if (server) { auto stopping = server->shutdown(); co_await stopping.wait(); server.reset(); }
      uos::kv::setKvForTest(nullptr); if (store) { store->close(); store.reset(); }
      if (!directory.empty()) rt::fs::rmSync(directory, {.recursive = true, .force = true});
    }
  };
  rt::Task<void> withFixture(std::function<rt::Task<void>(std::shared_ptr<HeaderFixture>)> run) {
    const auto fixture = std::make_shared<HeaderFixture>(); auto initializing = fixture->initialize(); co_await std::move(initializing);
    const auto cleanup = [fixture] { return fixture->close(); }; auto body = run(fixture); co_await rt::withFinally(std::move(body), cleanup);
  }
}
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)

ASYNC_TEST("normal streaming, nonstream and error HTTP headers update the meter without borrowing an old direct snapshot") {
  const auto run = [](std::shared_ptr<HeaderFixture> f) -> rt::Task<void> {
    const auto oldAt = rt::dateNow() - 60000; const auto direct = uos::provider::claude_code_usage::parseClaudeCodeUsagePayload(rt::jsonParse(R"({"five_hour":{"utilization":99,"resets_at":null}})"), oldAt); EXPECT(direct);
    quota::QuotaOutcome input; input.binding = f->account; input.observation = direct; input.nowMs = oldAt;
    input.usage = quota::ClaudeCodeUsageState{"oauth_usage", "scope_missing", 403, "historical", oldAt, std::nullopt};
    AWAIT(old, quota::recordClaudeCodeQuotaOutcome(f->store, input)); EXPECT(old); AWAIT(before, f->view()); EXPECT(!before.windows.five_hour);
    f->reply->headers = rt::Headers{{"anthropic-ratelimit-unified-5h-utilization", "0.4"}, {"anthropic-ratelimit-unified-7d-utilization", "0.2"}};
    AWAIT(streamStatus, f->infer(true)); EXPECT_EQ(streamStatus, 200); AWAIT(streaming, f->view());
    EXPECT(streaming.windows.five_hour && streaming.windows.seven_day); EXPECT_EQ(streaming.windows.five_hour->used_percent, 40); EXPECT_EQ(streaming.windows.seven_day->used_percent, 20);
    EXPECT_EQ(streaming.state, "available"); EXPECT(!streaming.usage); EXPECT(!streaming.usage_snapshot);
    f->reply->headers = rt::Headers{{"anthropic-ratelimit-unified-5h-utilization", "0.6"}}; AWAIT(bufferedStatus, f->infer(false)); EXPECT_EQ(bufferedStatus, 200);
    AWAIT(partial, f->view()); EXPECT(partial.windows.five_hour && partial.windows.seven_day); EXPECT_EQ(partial.windows.five_hour->used_percent, 60); EXPECT_EQ(partial.windows.seven_day->used_percent, 20);
    f->reply->headers = rt::Headers(); AWAIT(noHeadersStatus, f->infer(false)); EXPECT_EQ(noHeadersStatus, 200); AWAIT(noHeaders, f->view()); EXPECT_EQ(noHeaders.source_observed_at_ms, partial.source_observed_at_ms);
    f->reply->status = 429; f->reply->headers = rt::Headers{{"anthropic-ratelimit-unified-7d-utilization", "0.25"}, {"Retry-After", "60"}};
    AWAIT(rejectedStatus, f->infer(false)); EXPECT_EQ(rejectedStatus, 429); AWAIT(rejected, f->view()); EXPECT(rejected.windows.seven_day); EXPECT_EQ(rejected.windows.seven_day->used_percent, 25); EXPECT_EQ(rejected.state, "unavailable");
    AWAIT(state, quota::loadClaudeCodeQuotaState(f->store, f->account)); EXPECT(state); EXPECT(state->cooldown_until_ms); EXPECT(!state->blocked_until_ms); EXPECT_EQ(state->usage_snapshot, direct->usage_snapshot);
    AWAIT(points, history::readClaudeCapacityHistory(f->store, rt::dateNow())); bool observed = false;
    for (const auto& point : points.points) {
      if (point.source == "response_headers") { observed = true; EXPECT(!point.usage_snapshot); }
    }
    EXPECT(observed); EXPECT_EQ(f->reply->requests, 4);
  };
  auto testing = withFixture(run); co_await std::move(testing);
}
ASYNC_TEST("native reopen retains only proven header meter evidence and a replacement cohort starts awaiting headers") {
  const auto run = [](std::shared_ptr<HeaderFixture> f) -> rt::Task<void> {
    f->reply->headers = rt::Headers{{"anthropic-ratelimit-unified-5h-utilization", "0.3"}}; AWAIT(status, f->infer(false)); EXPECT_EQ(status, 200); AWAIT(before, f->view());
    auto reopening = f->reopen(); co_await std::move(reopening); AWAIT(after, f->view()); EXPECT_EQ(after.windows, before.windows); EXPECT_EQ(after.source_observed_at_ms, before.source_observed_at_ms);
    accounts::AccountUpdate input; input.accountId = f->account->account_id; input.credential = f->account->credential; input.credential->secret = "fixture-replacement";
    AWAIT(replacedAccount, accounts::updateClaudeCodeAccount(f->store, input)); (void)replacedAccount; AWAIT(replaced, f->view()); EXPECT(!replaced.source_observed_at_ms); EXPECT(!replaced.windows.five_hour);
  };
  auto testing = withFixture(run); co_await std::move(testing);
}
ASYNC_TEST("automatic and manual capacity reads make no Claude requests after ordinary inference") {
  const auto run = [](std::shared_ptr<HeaderFixture> f) -> rt::Task<void> {
    const auto oldAt = rt::dateNow() - 60000; const auto direct = uos::provider::claude_code_usage::parseClaudeCodeUsagePayload(rt::jsonParse(R"({"five_hour":{"utilization":98,"resets_at":null}})"), oldAt); EXPECT(direct);
    quota::QuotaOutcome input; input.binding = f->account; input.observation = direct; input.nowMs = oldAt; AWAIT(old, quota::recordClaudeCodeQuotaOutcome(f->store, input)); EXPECT(old);
    f->reply->headers = rt::Headers{{"anthropic-ratelimit-unified-5h-utilization", "0.15"}}; AWAIT(status, f->infer(false)); EXPECT_EQ(status, 200);
    int claudeCalls = 0; const rt::Fetch noClaude([&](rt::RequestInfo request, rt::RequestInit) {
      const auto url = urlOf(request); if (url.starts_with("https://api.anthropic.com/") || url.starts_with("https://platform.claude.com/")) ++claudeCalls;
      return rt::Response::json(json::object{}, {.status = 503});
    });
    const rt::GlobalFetchOverride overrideFetch(noClaude); AWAIT(before, quota::loadClaudeCodeQuotaState(f->store, f->account)); EXPECT(before && before->response_headers);
    const std::vector<std::string> queries{"", "", "?refresh=live"};
    for (const auto& query : queries) {
      rt::Request request("http://localhost/admin/providers/capacity" + query); uos::provider::capacity_contract::ProviderCapacitySnapshotOptions options; options.kv = f->store; options.fetcher = noClaude;
      AWAIT(response, capacity::handleProviderCapacity(request, options)); EXPECT_EQ(response.status(), 200); AWAIT(view, response.json());
      const auto* claude = rt::get(view, "claude"); EXPECT(claude); EXPECT_EQ(rt::getNumber(*rt::get(*rt::get(*claude, "windows"), "five_hour"), "used_percent"), std::optional<double>(15));
      EXPECT(!rt::hasOwn(claude->as_object(), "usage_snapshot")); EXPECT(rt::get(*claude, "usage")->is_null());
    }
    const auto payload = rt::jsonStringify(json::object{{"account_id", f->account->account_id}, {"account_cohort_id", f->account->account_cohort_id}});
    rt::Request request("http://localhost/admin/providers/claude-code/quota/refresh", {.method = "POST", .body = payload}); uos::admin::claude_code::ClaudeUsageRefreshOptions options; options.env = env(); options.fetcher = noClaude;
    AWAIT(response, uos::admin::claude_code::handleAdminClaudeCodeQuotaRefresh(request, options)); EXPECT_EQ(response.status(), 200); AWAIT(body, response.json());
    EXPECT_EQ(rt::getBool(body, "refreshed"), std::optional<bool>(false)); EXPECT_EQ(rt::getString(body, "reason"), std::optional<std::string_view>("response_headers")); EXPECT_EQ(rt::getNumber(body, "observed_at_ms"), std::optional<double>(before->response_headers->observed_at_ms));
    EXPECT_EQ(claudeCalls, 0); EXPECT_EQ(f->reply->requests, 1); AWAIT(after, quota::loadClaudeCodeQuotaState(f->store, f->account)); EXPECT_EQ(after, before);
  };
  auto testing = withFixture(run); co_await std::move(testing);
}
ASYNC_TEST("header family timestamps merge independently and unmarked canonical values are never relabeled") {
  const auto run = [](std::shared_ptr<HeaderFixture> f) -> rt::Task<void> {
    const auto at = rt::dateNow(); const auto observed = quota::observeClaudeCodeQuota(rt::Headers{{"anthropic-ratelimit-unified-5h-utilization", "0.8"}}, at); EXPECT(observed);
    quota::QuotaOutcome input; input.binding = f->account; input.observation = observed; input.nowMs = at; AWAIT(unmarked, quota::recordClaudeCodeQuotaOutcome(f->store, input)); EXPECT(unmarked); AWAIT(before, f->view(at)); EXPECT(!before.windows.five_hour);
    input.observation_source = "response_headers"; AWAIT(marked, quota::recordClaudeCodeQuotaOutcome(f->store, input)); EXPECT(marked);
    input.observation = quota::observeClaudeCodeQuota(rt::Headers{{"anthropic-ratelimit-unified-5h-utilization", "0.1"}, {"anthropic-ratelimit-requests-remaining", "49"}}, at - 10); EXPECT(input.observation);
    AWAIT(older, quota::recordClaudeCodeQuotaOutcome(f->store, input)); EXPECT(older); AWAIT(view, f->view(at)); EXPECT(view.windows.five_hour); EXPECT_EQ(view.windows.five_hour->used_percent, 80); EXPECT(!view.standard.empty()); EXPECT_EQ(view.standard.front().remaining, std::optional<double>(49)); EXPECT(!view.windows.seven_day);
  };
  auto testing = withFixture(run); co_await std::move(testing);
}
