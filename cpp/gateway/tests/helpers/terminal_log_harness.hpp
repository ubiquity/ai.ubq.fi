#pragma once
// Synthetic callable-terminal fixtures; the wrapper supplies empty HOME and task TMPDIR.
#include "uos/config.hpp"
#include "uos/handler/terminal_log.hpp"
#include "uos/cache/prompt_analytics_queue.hpp"
#include "uos/kv.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <algorithm>

namespace uos::tests::helpers::terminal_log_harness {
  namespace terminal = handler::terminal_log;
  namespace replay = sentinel::replay_observation;
  namespace model = sentinel::replay_model;

  struct Isolation {
    std::string previousAuth = config::config.codexAuthJsonB64;
    std::string fixtureHome = rt::env::get("HOME").value_or("/tmp/claude-1002/wave/w09c3/empty-home");
    std::string fixtureTmp = rt::env::get("TMPDIR").value_or("/tmp/claude-1002/wave/w09c3");
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", fixtureHome}, {"TMPDIR", fixtureTmp}}};
    Isolation() {
      config::config.codexAuthJsonB64.clear();
      kv::setKvForTest(nullptr);
      cache::prompt_analytics_queue::resetOptionalPromptCacheAnalyticsForTest();
    }
    ~Isolation() {
      cache::prompt_analytics_queue::resetOptionalPromptCacheAnalyticsForTest();
      kv::setKvForTest(nullptr);
      config::config.codexAuthJsonB64 = previousAuth;
    }
  };

  inline terminal::WithTerminalRequestLogInput terminalOptions(std::string requestId) {
    terminal::WithTerminalRequestLogInput options{.route = "responses", .startedAtMonotonicMs = rt::performanceNow(), .requestId = std::move(requestId)};
    options.recordTelemetry = [](cache::telemetry_gate::PromptCacheTelemetryEvent) {
      return rt::Promise<cache::telemetry_gate::PromptCacheTelemetryRecordResult>::resolve(
        cache::telemetry_gate::PromptCacheTelemetryRecordResult{"ignored", "unknown_release", std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    };
    options.recordCacheAnalytics = [](cache::prompt_analytics_core::PromptCacheAnalyticsEvent) {
      return rt::Promise<cache::prompt_analytics_core::PromptCacheAnalyticsRecordResult>::resolve(
        cache::prompt_analytics_core::PromptCacheAnalyticsRecordResult{"ignored", "unknown_release", std::nullopt});
    };
    options.recordAdminError = [](admin::error_log::AdminErrorInput) { return rt::Promise<void>::resolve(); };
    return options;
  }

  inline rt::Response jsonResponse(std::string body, double status = 200, std::optional<std::string> provider = std::nullopt) {
    rt::Headers headers{{"Content-Type", "application/json"}};
    if (provider) headers.set("x-uos-upstream", *provider);
    const rt::ResponseInit init{.status = status, .headers = headers};
    return rt::Response(rt::BodyInit(std::move(body)), init);
  }

  inline rt::Response codexResponseWithUsage() {
    auto response = jsonResponse("complete", 200, "chatgpt_codex");
    auto state = openai_telemetry::createResponseTelemetryState();
    state->provider = "chatgpt_codex";
    state->model = "gpt-5.6-sol";
    state->inputTokens = 120;
    state->cachedInputTokens = 40;
    state->outputTokens = 30;
    state->usageObserved = true;
    state->usageTelemetryStatus = "reported";
    return openai_telemetry::attachResponseTelemetry(response, state);
  }

  inline model::AcceptedSentinelReplayInput acceptedInput() {
    return {.endpoint = "/v1/responses", .method = "POST", .body = std::make_shared<rt::Bytes>(rt::encodeUtf8("{\"model\":\"fixture\"}")),
      .content_type = "application/json", .compatibility_headers = {}, .request_id = "handler-replay-background", .git_sha = "fixture-git-sha",
      .deno_revision = "fixture-deno-revision"};
  }

  inline bool zeroed(const std::shared_ptr<rt::Bytes>& bytes) {
    return bytes && std::all_of(bytes->begin(), bytes->end(), [](std::uint8_t byte) { return byte == 0; });
  }

  inline std::vector<json::value> terminalPayloads(const rt::console::Capture& logs, std::optional<std::string> requestId = std::nullopt) {
    std::vector<json::value> results;
    for (const auto& call : logs.calls()) {
      if (call.args.size() < 2 || !(call.args[0] == "[ai.ubq.fi] request_terminal")) continue;
      auto payload = rt::jsonParse(call.args[1].string());
      if (!requestId || rt::getString(payload, "request_id") == requestId) results.push_back(std::move(payload));
    }
    return results;
  }
}
