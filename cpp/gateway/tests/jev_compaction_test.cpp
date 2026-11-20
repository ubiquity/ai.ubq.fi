// Links: tests/jev_compaction_test.cpp
//        src/lib/jev_compaction/state.cpp src/lib/jev_compaction/compact.cpp src/jev_compaction/compaction.cpp src/lib/jev_compaction/request.cpp src/http.cpp src/request_policy.cpp
//        src/lib/jev_compaction/types.cpp src/lib/jev_compaction/codex_items.cpp src/config.cpp src/jev_compaction/hybrid.cpp src/provider/openrouter.cpp src/openai_telemetry.cpp
//        src/request.cpp src/defaults.cpp src/provider/claude_code_models.cpp src/provider/claude_code_accounts.cpp src/provider/surplus_claude_request.cpp src/provider/claude_code_credentials.cpp
//        src/models/openrouter_models.cpp src/provider/selection.cpp src/models/codex_models_fetch.cpp src/auth/relay.cpp src/runtime_config.cpp src/codex/auth_refresh.cpp
//        src/provider/metered.cpp src/models/codex_models.cpp src/provider/anthropic_responses.cpp src/provider/anthropic_replay.cpp src/lib/jev_compaction/client.cpp src/responses_stream.cpp
//        src/codex/dispatch.cpp src/codex/auth.cpp src/provider/cerebras.cpp src/utils.cpp src/provider/surplus.cpp src/responses_failover_stream.cpp
//        src/bounded_response_body.cpp src/provider/anthropic_response_wire.cpp src/codex/routing_mutations.cpp src/models/codex_models_availability.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_mutations.cpp
//        src/provider/capacity_events.cpp src/provider/removed_provider_telemetry.cpp src/release.cpp src/kv.cpp src/codex/native_auth.cpp src/types.cpp
//        src/codex/rate_limit_429.cpp src/authority/native_codex_capacity.cpp src/provider/claude_code.cpp src/codex/capacity_routing.cpp src/codex/routing_state.cpp src/provider/claude_code_quota.cpp
//        src/provider/surplus_claude_chat_request.cpp src/codex/native_auth_transport.cpp src/codex/external_auth.cpp src/codex/banked_reset.cpp src/authority/native_codex_429.cpp src/provider/anthropic_chat_responses.cpp
//        src/provider/cerebras_rate_limits.cpp src/runtime/websocket.cpp src/provider/capacity_contract.cpp src/codex/responses_error.cpp src/codex/reset_settings.cpp src/authority/native_codex_evaluation.cpp
//        src/provider/capacity.cpp src/provider/capacity_analytics.cpp src/provider/capacity_history.cpp src/provider/health.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp
//        src/cache/prompt_analytics_core.cpp src/provider/claude_code_usage_schema.cpp src/codex/experiment.cpp src/codex/account_routing.cpp src/storage/kv.cpp
//        src/provider/claude_capacity_rollups.cpp src/provider/claude_capacity_history.cpp src/provider/anthropic_chat_stream.cpp src/authority/native_codex.cpp src/codex/banked_reset_provider.cpp src/codex/routing_serial.cpp
//        src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/paid_fallback/ledger_stats.cpp src/cache/prompt_analytics_read.cpp src/codex/routing_evaluation.cpp src/codex/overage_settings.cpp
//        src/metered_quota.cpp
#include "uos/jev_compaction/compaction.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/config.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/lib/jev_compaction/codex_items.hpp"
#include "uos/lib/jev_compaction/compact.hpp"
#include "uos/lib/jev_compaction/state.hpp"
#include "uos/lib/jev_compaction/types.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "helpers/advancing_regex_clock.hpp"
#include "helpers/live_allocations.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace uos;
namespace jc = uos::lib::jev_compaction;
using jc::types::JevAsker;
using jc::types::JevQuestions;
using jc::types::JevResponse;
using jc::types::JevState;
using jc::types::Message;
using jc::types::ToolCall;
using jc::types::ToolResult;
using jc::types::ToolUse;

namespace {
  const std::string COMPACTION_PROMPT = "You are performing a CONTEXT CHECKPOINT COMPACTION. Create a handoff summary for another LLM.";
  const std::string KEPT_MARKER = "KEEP_MARKER_release_notes_artifact";

  json::value text(const std::string& role, const std::string& value) {
    json::object part;
    part["type"] = "input_text";
    part["text"] = value;
    json::object item;
    item["type"] = "message";
    item["role"] = role;
    item["content"] = json::array{json::value(std::move(part))};
    return item;
  }

  json::value call(const std::string& callId, const std::string& command) {
    json::object arguments;
    arguments["command"] = command;
    json::object item;
    item["type"] = "function_call";
    item["call_id"] = callId;
    item["name"] = "shell";
    item["arguments"] = rt::jsonStringify(json::value(std::move(arguments)));
    return item;
  }

  json::value output(const std::string& callId, const std::string& value) {
    json::object item;
    item["type"] = "function_call_output";
    item["call_id"] = callId;
    item["output"] = value;
    return item;
  }

  // Three obsolete unpinned call/result pairs, a pinned keep marker and the injected prompt.
  json::value transcript() {
    json::array items;
    items.push_back(text("user", "Goal: finish the release notes and keep " + KEPT_MARKER + " verbatim."));
    items.push_back(text("assistant", "Starting with the old build logs."));
    items.push_back(call("old_a", "tail -n 400 /tmp/old/build.log"));
    items.push_back(output("old_a", rt::repeat("RESULT_A obsolete build log line. ", 60)));
    items.push_back(call("old_b", "grep -c deprecated /tmp/old/metrics.csv"));
    items.push_back(output("old_b", rt::repeat("RESULT_B obsolete metric line. ", 60)));
    items.push_back(call("old_c", "ls -la /tmp/old/cache"));
    items.push_back(output("old_c", rt::repeat("RESULT_C obsolete cache line. ", 60)));
    items.push_back(text("assistant", "The old investigation is finished; those logs are obsolete."));
    items.push_back(text("user", "Recent follow-up one: draft the release notes outline."));
    items.push_back(text("assistant", "Drafted the outline."));
    items.push_back(text("user", "Recent follow-up two: check the artifact wording."));
    items.push_back(text("assistant", "Checked."));
    items.push_back(text("user", "Final goal: " + KEPT_MARKER + " stays verbatim."));
    items.push_back(text("user", COMPACTION_PROMPT));
    return items;
  }

  json::object exec(const std::string& command) {
    json::object input;
    input["cmd"] = command;
    return input;
  }

  // An asker answering every question with `noul: 0.1`; it records each state it was sent.
  class LowRelevanceAsker : public JevAsker {
  public:
    std::vector<std::string> states;
    rt::Task<JevResponse> ask(JevState state, JevQuestions questions) override {
      states.push_back(rt::jsonStringify(state));
      json::object answers;
      for (const auto& [name, question] : questions) {
        json::object answer;
        answer["noul"] = 0.1;
        answers[name] = std::move(answer);
      }
      JevResponse response;
      response["answers"] = std::move(answers);
      co_return response;
    }
  };
}

TEST("renderer marks kept calls and never rewrites kept text") {
  const std::optional<jc::codex_items::CodexTranscript> parsed = jc::codex_items::parseCodexInput(transcript());
  ASSERT(parsed.has_value());
  const std::vector<ToolCall> calls = jc::state::collectToolCalls(parsed->messages, 6);
  jc::codex_items::RenderOptions options;
  for (const ToolCall& entry : calls) {
    options.decisions.push_back(jc::types::CallDecision{entry.id, entry.tool, 1, 1, "keep", "pinned"});
    options.callIds.set(entry.id, entry.tool_use_id);
  }
  options.headChars = 300;
  const std::string summary = jc::codex_items::renderSummary(*parsed, options);
  EXPECT(!rt::includes(summary, "no decision (kept)"));
  EXPECT(rt::includes(summary, KEPT_MARKER));
  EXPECT(rt::includes(summary, "[tool call old_c] shell — keep"));
}

ASYNC_TEST("windowed decisions keep local state when a long transcript degrades the global fit") {
  std::vector<Message> messages;
  for (int index = 0; index < 400; index += 1) {
    const std::string id = "call_" + std::to_string(index);
    messages.push_back(Message{"assistant", "", {ToolUse{id, "exec_command", exec("command " + std::to_string(index)), std::nullopt, std::nullopt}}, std::nullopt});
    messages.push_back(Message{"user", "", {}, std::vector<ToolResult>{ToolResult{id, rt::repeat("result " + std::to_string(index) + " ", 50), std::nullopt}}});
  }
  messages.push_back(Message{"user", "Final goal.", {}, std::nullopt});
  auto asker = std::make_shared<LowRelevanceAsker>();
  jc::types::CompactOptions options;
  options.preserveRecentMessages = 2;
  options.maxStateTokens = 800;
  options.maxRequestTokens = 4000;
  options.keepThreshold = 0.5;
  auto task = jc::compact::compact(messages, asker, options);
  const jc::types::CompactResult result = co_await std::move(task);
  EXPECT(result.stats.stateStage.starts_with("windowed"));
  EXPECT(result.stats.windows > 1);
  EXPECT_EQ(result.decisions.size(), std::size_t(400));
  for (const std::string& state : asker->states) EXPECT(jc::state::estimateTokens(state) <= 1200);
}

ASYNC_TEST("short transcripts keep the single global state path") {
  const std::vector<Message> messages = {
    Message{"user", "Goal.", {}, std::nullopt},
    Message{"assistant", "", {ToolUse{"call_a", "exec_command", exec("one"), std::nullopt, std::nullopt}}, std::nullopt},
    Message{"user", "", {}, std::vector<ToolResult>{ToolResult{"call_a", "first result", std::nullopt}}},
    Message{"assistant", "", {ToolUse{"call_b", "exec_command", exec("two"), std::nullopt, std::nullopt}}, std::nullopt},
    Message{"user", "", {}, std::vector<ToolResult>{ToolResult{"call_b", "second result", std::nullopt}}},
    Message{"user", "Final goal.", {}, std::nullopt},
  };
  auto asker = std::make_shared<LowRelevanceAsker>();
  jc::types::CompactOptions options;
  options.preserveRecentMessages = 1;
  options.keepThreshold = 0.5;
  auto task = jc::compact::compact(messages, asker, options);
  const jc::types::CompactResult result = co_await std::move(task);
  EXPECT_EQ(result.stats.windows, std::int64_t(0));
  EXPECT_EQ(result.stats.stateStage, std::string("full"));
}

TEST("fitState drops the oldest merged call runs instead of failing on very long sessions") {
  std::vector<Message> messages = {Message{"user", "Goal: keep the current state.", {}, std::nullopt}};
  for (int index = 0; index < 400; index += 1) {
    const std::string id = "call_" + std::to_string(index);
    messages.push_back(Message{"assistant", "", {ToolUse{id, "exec_command", exec("command " + std::to_string(index)), std::nullopt, std::nullopt}}, std::nullopt});
    messages.push_back(Message{"user", "", {}, std::vector<ToolResult>{ToolResult{id, "result " + std::to_string(index), std::nullopt}}});
  }
  messages.push_back(Message{"user", "Final goal: keep the current state.", {}, std::nullopt});
  const std::vector<ToolCall> calls = jc::state::collectToolCalls(messages, 2);
  jc::types::ResolvedCompactOptions options;
  options.maxStateTokens = 800;
  options.preserveRecentMessages = 2;
  options.goal = "";
  const jc::types::FittedState fitted = jc::state::fitState(messages, calls, options);
  EXPECT(fitted.tokens <= 800);
  EXPECT_EQ(fitted.stage, std::string("old call runs left out"));
}

namespace gateway = uos::jev_compaction::compaction;
namespace {
  const std::string COMPACTION_METADATA = R"({"request_kind":"compaction","compaction":{"implementation":"responses"}})";
  class DecisionAsker : public JevAsker {
  public:
    std::map<std::string, std::pair<double, double>> verdicts;
    bool partial = false;
    std::optional<std::string> failure;
    std::function<void()> onAsk;
    rt::Task<JevResponse> ask(JevState, JevQuestions questions) override {
      if (onAsk) onAsk();
      if (failure) throw rt::JsError(*failure);
      json::object answers;
      for (const auto& [name, question] : questions) {
        (void)question;
        const auto separator = name.find('_');
        const auto kind = name.substr(0, separator);
        if (partial && kind == "result") continue;
        const auto id = name.substr(separator + 1);
        const auto found = verdicts.find(id);
        const auto verdict = found == verdicts.end() ? std::pair<double, double>{1, 1} : found->second;
        answers[name] = json::object{{"noul", rt::makeNumber(kind == "call" ? verdict.first : verdict.second)}};
      }
      return rt::fulfilledTask(JevResponse{{"answers", answers}});
    }
  };
  std::shared_ptr<DecisionAsker> dropping() {
    auto asker = std::make_shared<DecisionAsker>();
    for (const auto* id : {"t1", "t2", "t3"}) asker->verdicts[id] = {0.1, 0.1};
    return asker;
  }
  json::value body(const json::value& input, bool stream = true) { return json::object{{"model", "deepseek-flash"}, {"stream", stream}, {"input", input}}; }
  rt::Request compactionRequest(const json::value& input, bool stream = true, const std::optional<std::string>& metadata = COMPACTION_METADATA) {
    rt::Headers headers{{"content-type", "application/json"}};
    if (metadata) headers.set("x-codex-turn-metadata", *metadata);
    return rt::Request("https://fixture.example.invalid/v1/responses", rt::RequestInit{.method = "POST", .headers = headers, .body = rt::jsonStringify(body(input, stream))});
  }
  rt::Task<gateway::CompactionOutcome> building(const json::value& input, std::shared_ptr<JevAsker> asker, bool stream = true) {
    const gateway::BuildCompactionResponseOptions options{stream};
    return gateway::buildCompactionResponse(body(input, stream), std::move(asker), options);
  }
  rt::Task<std::string> rejectedKind(rt::Task<gateway::CompactionOutcome> pending) {
    std::string kind;
    try { const auto ignored = co_await std::move(pending); (void)ignored; }
    catch (const gateway::CompactionUnavailable& error) { kind = error.kind; }
    co_return kind;
  }
  std::string summaryOf(const std::string& completion) {
    const auto parsed = rt::jsonParse(completion);
    const auto* output = rt::getArray(parsed, "output");
    const auto* content = rt::getArray((*output)[0], "content");
    return std::string(*rt::getString((*content)[0], "text"));
  }
  std::string largeText(double chars) {
    const std::string base = "alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima mike november oscar papa quebec romeo sierra tango uniform victor whiskey xray yankee zulu ";
    return rt::slice(rt::repeat(base, rt::Math::ceil(chars / static_cast<double>(rt::length(base)))), 0, chars);
  }
  json::value largeTranscript(double textChars, double resultChars) {
    json::array input{text("user", "Goal: finish the large-session check."), call("big", "cat /tmp/big.log"), output("big", largeText(resultChars))};
    for (int index = 0; index < 8; ++index) input.push_back(text("user", largeText(rt::Math::floor(textChars / 8))));
    input.push_back(text("user", "Final goal: keep the exact state."));
    input.push_back(text("user", COMPACTION_PROMPT));
    return input;
  }
  struct Isolation {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c7"}}};
    std::string original = config::config.codexAuthJsonB64;
    Isolation() { config::config.codexAuthJsonB64.clear(); }
    ~Isolation() { config::config.codexAuthJsonB64 = original; gateway::setJevCompactionAskerForTest(nullptr); }
  };
}

ASYNC_TEST("jev compaction predicate is header-only and exact") {
  const Isolation isolation;
  const auto input = transcript();
  auto unmarked = compactionRequest(input, true, std::nullopt);
  EXPECT(!gateway::isJevCompactionRequest(unmarked));
  auto read = unmarked.text(); const auto original = co_await std::move(read); EXPECT_EQ(original, rt::jsonStringify(body(input)));
  const std::vector<std::string> rejected{"{not json", R"({"request_kind":"compaction","compaction":{"implementation":"responses_compaction_v2"}})", R"({"request_kind":"turn","compaction":{"implementation":"responses"}})"};
  for (const auto& metadata : rejected) EXPECT(!gateway::isJevCompactionRequest(compactionRequest(input, true, metadata)));
  auto marked = compactionRequest(input); EXPECT(gateway::isJevCompactionRequest(marked));
  auto reading = marked.text(); const auto untouched = co_await std::move(reading); EXPECT_EQ(untouched, rt::jsonStringify(body(input)));
}

ASYNC_TEST("renderer applies Jev decisions verbatim and degrades dropped content to bounded provenance") {
  const Isolation isolation; rt::console::Capture capture({rt::console::Method::info});
  auto asker = std::make_shared<DecisionAsker>(); asker->verdicts = {{"t1", {0.1, 0.1}}, {"t2", {0.9, 0.1}}, {"t3", {0.9, 0.9}}};
  auto pending = building(transcript(), asker); const auto outcome = co_await std::move(pending);
  EXPECT_EQ(outcome.status, 200); EXPECT_EQ(outcome.contentType, std::string("text/event-stream"));
  EXPECT(rt::includes(*outcome.headers.get("x-jev-compaction"), "calls_dropped=1")); EXPECT(rt::includes(*outcome.headers.get("x-jev-compaction"), "results_dropped=1"));
  const auto& summary = outcome.body;
  for (const auto* marker : {"# fast-jev-compaction memory summary", "KEEP_MARKER_release_notes_artifact", "[tool result old_c — kept verbatim]", "[tool result old_b — truncated, original was", "[tool call old_a]", "[tool result old_a — truncated (call dropped), original was", "RESULT_A", "fast-jev-compaction truncated", "event: response.completed"}) EXPECT(rt::includes(summary, marker));
  EXPECT(!rt::includes(summary, rt::repeat("RESULT_A obsolete build log line. ", 20))); EXPECT(!rt::includes(summary, "You are performing"));
  auto buffered = building(transcript(), asker, false); const auto jsonOutcome = co_await std::move(buffered);
  EXPECT_EQ(jsonOutcome.contentType, std::string("application/json")); const auto parsed = rt::jsonParse(jsonOutcome.body);
  EXPECT_EQ(rt::getString(parsed, "status"), std::optional<std::string_view>("completed")); EXPECT(rt::includes(summaryOf(jsonOutcome.body), KEPT_MARKER));
}

ASYNC_TEST("floor-bound summary ships for a roughly 1.5-million-token input that cannot shrink further") {
  const Isolation isolation; rt::console::Capture capture({rt::console::Method::info}); const auto input = largeTranscript(5'100'000, 3'000'000);
  uos::test::AdvancingRegexClock slow(std::chrono::milliseconds(1));
  EXPECT(jc::state::estimateTokens(rt::jsonStringify(input)) > 1'500'000);
  EXPECT(slow.elapsed() > std::chrono::milliseconds(1800));
  auto pending = building(input, dropping(), false); const auto outcome = co_await std::move(pending);
  EXPECT_EQ(outcome.status, 200); EXPECT(rt::includes(outcome.body, jc::codex_items::SUMMARY_MARKER)); EXPECT(static_cast<double>(rt::length(outcome.body)) > gateway::SUMMARY_CHAR_CAP);
  EXPECT(rt::length(outcome.body) < rt::length(rt::jsonStringify(input))); EXPECT(rt::includes(*outcome.headers.get("x-jev-compaction"), "floor=1")); EXPECT(rt::includes(*outcome.headers.get("x-jev-compaction"), "calls_dropped=1"));
}

ASYNC_TEST("a floor that is not measurably smaller than its input still fails closed") {
  const Isolation isolation; const auto input = largeTranscript(3'900'000, 250'000); auto pending = rejectedKind(building(input, dropping(), false)); const auto kind = co_await std::move(pending); EXPECT_EQ(kind, std::string("summary-too-large"));
}

ASYNC_TEST("compaction fails closed on malformed input, no candidates and no reduction") {
  const Isolation isolation; const gateway::BuildCompactionResponseOptions options{true}; const json::value invalid = json::object{{"model", "deepseek-flash"}};
  auto malformed = rejectedKind(gateway::buildCompactionResponse(invalid, dropping(), options)); const auto malformedKind = co_await std::move(malformed); EXPECT_EQ(malformedKind, std::string("missing-input"));
  const json::array shortInput{text("user", "hello"), text("assistant", "hi")}; auto shortTask = rejectedKind(building(shortInput, dropping())); const auto shortKind = co_await std::move(shortTask); EXPECT_EQ(shortKind, std::string("no-candidates"));
  auto keep = rejectedKind(building(transcript(), std::make_shared<DecisionAsker>())); const auto keepKind = co_await std::move(keep); EXPECT_EQ(keepKind, std::string("no-reduction"));
  const json::array empty; auto emptyTask = rejectedKind(building(empty, dropping())); const auto emptyKind = co_await std::move(emptyTask); EXPECT_EQ(emptyKind, std::string("empty-transcript"));
}

ASYNC_TEST("compaction fails closed on Jev error or a partial decision set") {
  const Isolation isolation; auto failed = dropping(); failed->failure = "Jev request failed (500): upstream";
  auto failing = rejectedKind(building(transcript(), failed)); const auto failure = co_await std::move(failing); EXPECT_EQ(failure, std::string("jev-failed"));
  auto partial = dropping(); partial->partial = true; auto incomplete = rejectedKind(building(transcript(), partial)); const auto kind = co_await std::move(incomplete); EXPECT_EQ(kind, std::string("jev-failed"));
}

ASYNC_TEST("compaction fails closed on an oversized summary") {
  const Isolation isolation; const json::array input{text("user", "Goal: " + rt::repeat("x", gateway::SUMMARY_CHAR_CAP + 1000)), call("old", "echo old"), output("old", rt::repeat("RESULT_OBSOLETE ", 40)), text("user", "recent one"), text("assistant", "recent two"), text("user", "recent three"), text("assistant", "recent four"), text("user", "recent five"), text("assistant", "recent six"), text("user", COMPACTION_PROMPT)};
  auto pending = rejectedKind(building(input, dropping())); const auto kind = co_await std::move(pending); EXPECT_EQ(kind, std::string("summary-too-large"));
}

ASYNC_TEST("compaction fits kept results under the cap by dropping the lowest relevance first") {
  const Isolation isolation; rt::console::Capture capture({rt::console::Method::info}); json::array input{text("user", "Goal: keep the release evidence.")};
  for (const auto* id : {"a", "b", "c", "d", "e"}) { const std::string suffix(id); input.push_back(call("old_" + suffix, "cat /tmp/" + suffix)); input.push_back(output("old_" + suffix, rt::repeat(rt::toUpperCase(suffix), 360'000))); }
  input.push_back(text("user", COMPACTION_PROMPT)); auto asker = std::make_shared<DecisionAsker>();
  asker->verdicts = {{"t1", {0.9, 0.6}}, {"t2", {0.9, 0.7}}, {"t3", {0.9, 0.8}}, {"t4", {0.9, 0.9}}, {"t5", {0.9, 0.95}}};
  auto pending = building(input, asker, false); const auto outcome = co_await std::move(pending); EXPECT_EQ(outcome.status, 200);
  EXPECT(rt::includes(*outcome.headers.get("x-jev-compaction"), "fitted=")); const auto summary = summaryOf(outcome.body); EXPECT(static_cast<double>(rt::length(summary)) <= gateway::SUMMARY_CHAR_CAP);
  EXPECT(rt::includes(summary, "[tool result old_e — kept verbatim]")); EXPECT(rt::includes(summary, "[tool result old_a — truncated"));
}

ASYNC_TEST("gateway handler fails closed without a credential and on caller cancellation") {
  const Isolation isolation; rt::console::Capture capture({rt::console::Method::info}); gateway::JevCompactionDeps deps; deps.apiKey = nullptr;
  auto missing = gateway::handleJevResponsesCompaction(compactionRequest(transcript()), deps); auto response = co_await std::move(missing); EXPECT_EQ(response.status(), 503);
  auto reading = response.json(); const auto failure = co_await std::move(reading); const auto* error = rt::get(failure, "error"); EXPECT_EQ(rt::getString(*error, "code"), std::optional<std::string_view>("jev_compaction_unavailable")); EXPECT(rt::includes(*rt::getString(*error, "message"), "missing-key"));
  deps.signal = rt::AbortSignal::abort(); deps.asker = dropping(); auto aborted = gateway::handleJevResponsesCompaction(compactionRequest(transcript()), deps); auto cancelled = co_await std::move(aborted); EXPECT_EQ(cancelled.status(), 499);
  auto cancelledRead = cancelled.json(); const auto cancelledBody = co_await std::move(cancelledRead); EXPECT_EQ(rt::getString(*rt::get(cancelledBody, "error"), "code"), std::optional<std::string_view>("request_cancelled"));
}

ASYNC_TEST("gateway handler answers a marked request through the injected asker") {
  const Isolation isolation; rt::console::Capture capture({rt::console::Method::info}); gateway::JevCompactionDeps deps; deps.apiKey = "test-key"; deps.asker = dropping();
  auto handling = gateway::handleJevResponsesCompaction(compactionRequest(transcript()), deps); auto response = co_await std::move(handling); EXPECT_EQ(response.status(), 200); EXPECT_EQ(response.headers().get("content-type"), std::optional<std::string>("text/event-stream")); EXPECT(response.headers().has("x-jev-compaction"));
  auto reading = response.text(); const auto result = co_await std::move(reading); EXPECT(rt::includes(result, jc::codex_items::SUMMARY_MARKER)); EXPECT(rt::includes(result, KEPT_MARKER));
}

ASYNC_TEST("completion telemetry marks a local success without inventing usage") {
  const Isolation isolation; rt::console::Capture capture({rt::console::Method::info}); gateway::JevCompactionDeps deps; deps.apiKey = "test-key"; deps.asker = dropping();
  auto handling = gateway::handleJevResponsesCompaction(compactionRequest(transcript()), deps); const auto response = co_await std::move(handling); EXPECT_EQ(response.status(), 200);
  const auto telemetry = openai_telemetry::getResponseTelemetry(response); EXPECT(telemetry); EXPECT(telemetry->completed); EXPECT_EQ(telemetry->semanticOutputObserved, std::optional<bool>(true)); EXPECT_EQ(telemetry->stream, std::optional<bool>(true)); EXPECT_EQ(telemetry->streamTerminalType, std::optional<std::string>("response.completed")); EXPECT_EQ(telemetry->provider, std::string("gateway")); EXPECT(!telemetry->failureKind); EXPECT(!telemetry->usageObserved); EXPECT(!telemetry->inputTokens); EXPECT(!telemetry->outputTokens); EXPECT(!telemetry->totalTokens);
  const json::array shortInput{text("user", "short"), text("assistant", "short answer")}; auto refusing = gateway::handleJevResponsesCompaction(compactionRequest(shortInput), deps); const auto refused = co_await std::move(refusing); EXPECT(refused.status() >= 400); EXPECT(!openai_telemetry::getResponseTelemetry(refused));
}

ASYNC_TEST("test asker seam routes a handler compaction through the injected asker") {
  const Isolation isolation; rt::console::Capture capture({rt::console::Method::info}); gateway::setJevCompactionAskerForTest(dropping()); gateway::JevCompactionDeps deps; deps.apiKey = "test-seam";
  auto handling = gateway::handleJevResponsesCompaction(compactionRequest(transcript()), deps); auto response = co_await std::move(handling); EXPECT_EQ(response.status(), 200); auto reading = response.text(); const auto result = co_await std::move(reading); EXPECT(rt::includes(result, jc::codex_items::SUMMARY_MARKER));
}

ASYNC_TEST("Jev timeout fetch bounds headers and body while sanitizing transport cancellation") {
  const Isolation isolation;
  auto hanging = rt::Fetch([](rt::RequestInfo, rt::RequestInit init) {
    auto [pending, resolve, reject] = rt::withResolvers<rt::Response>(); (void)resolve;
    const auto signal = *init.signal; const rt::EventListener abort([signal, reject] { reject(signal.reason()); });
    if (signal.aborted()) reject(signal.reason()); else signal.addEventListener("abort", abort, {.once = true});
    return pending.wait();
  });
  auto bounded = gateway::createJevTimeoutFetch(hanging, 12); const rt::RequestInfo url = std::string("https://fixture.example.invalid"); const rt::RequestInit init;
  auto headers = bounded(url, init); bool timedOut = false;
  try { const auto ignored = co_await std::move(headers); (void)ignored; } catch (const rt::JsError& error) { timedOut = error.message == "Jev request timed out after 12ms"; } EXPECT(timedOut);
  rt::UnderlyingSource<rt::ByteChunk> source; source.start = [](rt::ReadableStreamDefaultController<rt::ByteChunk>) {};
  const rt::ByteStream never(source); auto bodyFetcher = rt::Fetch([never](rt::RequestInfo, rt::RequestInit) { return rt::Response(rt::BodyInit(never), rt::ResponseInit{.status = 201, .statusText = "Made", .headers = rt::Headers{{"x-fixture", "yes"}}}); });
  auto bodyBound = gateway::createJevTimeoutFetch(bodyFetcher, 14); auto fetching = bodyBound(url, init); auto response = co_await std::move(fetching); EXPECT_EQ(response.status(), 201); EXPECT_EQ(response.headers().get("x-fixture"), std::optional<std::string>("yes"));
  auto reading = response.text(); bool bodyTimedOut = false; try { const auto ignored = co_await std::move(reading); (void)ignored; } catch (const rt::JsError& error) { bodyTimedOut = error.message == "Jev request timed out after 14ms"; } EXPECT(bodyTimedOut);
  rt::AbortController controller; rt::RequestInit abortInit; abortInit.signal = controller.signal(); auto aborting = gateway::createJevTimeoutFetch(hanging, 100); rt::Promise<rt::Response> waiting(aborting(url, abortInit)); controller.abort(rt::makeReason(rt::JsError("private synthetic abort")));
  bool cancelled = false; try { const auto ignored = co_await waiting.wait(); (void)ignored; } catch (const rt::JsError& error) { cancelled = error.message == "Jev request aborted"; } EXPECT(cancelled);
}

ASYNC_TEST("Jev timeout fetch releases timers on EOF absent bodies and explicit cancellation") {
  const Isolation isolation; const rt::RequestInfo url = std::string("https://fixture.example.invalid"); const rt::RequestInit init;
  auto immediate = gateway::createJevTimeoutFetch(rt::Fetch([](rt::RequestInfo, rt::RequestInit) { return rt::Response("complete"); }), 100); auto fetching = immediate(url, init); auto response = co_await std::move(fetching); auto reading = response.text(); const auto textBody = co_await std::move(reading); EXPECT_EQ(textBody, std::string("complete"));
  auto empty = gateway::createJevTimeoutFetch(rt::Fetch([](rt::RequestInfo, rt::RequestInit) { return rt::Response(nullptr); }), 100); auto fetchingEmpty = empty(url, init); const auto emptyResponse = co_await std::move(fetchingEmpty); EXPECT(!emptyResponse.body());
  const auto cancelled = std::make_shared<int>(0); rt::UnderlyingSource<rt::ByteChunk> source; source.cancel = [cancelled](rt::Reason) { ++*cancelled; }; const rt::ByteStream stream(source);
  auto cancellable = gateway::createJevTimeoutFetch(rt::Fetch([stream](rt::RequestInfo, rt::RequestInit) { return rt::Response(rt::BodyInit(stream)); }), 100); auto fetchingStream = cancellable(url, init); const auto streamResponse = co_await std::move(fetchingStream); const auto bodyStream = *streamResponse.body(); auto cancelling = bodyStream.cancel(); co_await std::move(cancelling); EXPECT_EQ(*cancelled, 1);
  auto throwing = gateway::createJevTimeoutFetch(rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> { throw rt::JsError("bounded synthetic failure"); co_return rt::Response(); }), 100);
  auto failed = throwing(url, init); bool sanitized = false; try { const auto ignored = co_await std::move(failed); (void)ignored; } catch (const rt::JsError& error) { sanitized = error.message == "Jev transport error: bounded synthetic failure"; } EXPECT(sanitized);
}

ASYNC_TEST("discarded Jev response bodies detach from the reusable caller signal") {
  const Isolation isolation;
  const rt::AbortController caller;
  rt::RequestInit init;
  init.signal = caller.signal();
  const rt::RequestInfo url = std::string("https://fixture.example.invalid");
  const auto bounded = gateway::createJevTimeoutFetch(rt::Fetch([](rt::RequestInfo, rt::RequestInit) {
    return rt::Response("buffered piece");
  }), 100);
  const auto discard = [bounded, url, init]() -> rt::Task<void> {
    auto fetching = bounded(url, init);
    const auto response = co_await std::move(fetching);
    EXPECT_EQ(response.status(), 200);
    co_await rt::microtask();
  };
  auto checking = uos::tests::helpers::expectStableLiveAllocations(discard, 64, 101);
  co_await std::move(checking);
  EXPECT(!caller.signal().aborted());
}

ASYNC_TEST("Compaction complete wire bodies and structural headers match exact isolated Bun records") {
  const Isolation isolation; rt::console::Capture capture({rt::console::Method::info}); const rt::crypto::ScopedRandomSource random([](std::span<std::uint8_t> bytes) { std::fill(bytes.begin(), bytes.end(), std::uint8_t{0}); });
  const auto vectors = rt::jsonParse(R"oracle([{"suite":"compaction","stream":false,"status":200,"contentType":"application/json","headers":{"x-jev-compaction":"summary; kept=1; results_dropped=1; calls_dropped=1; pinned=0; chars_before=6201; chars_after=4120"},"body":"{\"id\":\"resp_jev_0000000000004000\",\"object\":\"response\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"id\":\"msg_jev_0000000000004000\",\"content\":[{\"type\":\"output_text\",\"text\":\"# fast-jev-compaction memory summary\\nTool calls and results below were classified by TypeSafe Jev (keep / truncate).\\nA dropped call keeps its provenance line and a bounded head of its result; nothing is erased\\noutright. Text kept verbatim is copied from the original transcript; this memory is text only, so\\nstructured tool-call replay and opaque items (encrypted reasoning or compaction payloads) are not restored.\\n\\n[user]\\nGoal: finish the release notes and keep KEEP_MARKER_release_notes_artifact verbatim.\\n\\n[assistant]\\nStarting with the old build logs.\\n\\n[tool call old_a] shell — drop_call\\ninput: {\\\"command\\\":\\\"tail -n 400 /tmp/old/build.log\\\"}\\n\\n[tool result old_a — truncated (call dropped), original was 2040 chars]\\nRESULT_A obsolete build log line. RESULT_A obsolete build log line. RESULT_A obsolete build log line. RESULT_A obsolete build log line. RESULT_A obsolete build log line. RESULT_A obsolete build log line. RESULT_A obsolete build log line. RESULT_A obsolete build log line. RESULT_A obsolete build log \\n[fast-jev-compaction truncated 1740 chars of this tool result; re-run the tool if needed]\\n\\n[tool call old_b] shell — drop_result\\ninput: {\\\"command\\\":\\\"grep -c deprecated /tmp/old/metrics.csv\\\"}\\n\\n[tool result old_b — truncated, original was 1860 chars]\\nRESULT_B obsolete metric line. RESULT_B obsolete metric line. RESULT_B obsolete metric line. RESULT_B obsolete metric line. RESULT_B obsolete metric line. RESULT_B obsolete metric line. RESULT_B obsolete metric line. RESULT_B obsolete metric line. RESULT_B obsolete metric line. RESULT_B obsolete met\\n[fast-jev-compaction truncated 1560 chars of this tool result; re-run the tool if needed]\\n\\n[tool call old_c] shell — keep\\ninput: {\\\"command\\\":\\\"ls -la /tmp/old/cache\\\"}\\n\\n[tool result old_c — kept verbatim]\\nRESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. \\n\\n[assistant]\\nThe old investigation is finished; those logs are obsolete.\\n\\n[user]\\nRecent follow-up one: draft the release notes outline.\\n\\n[assistant]\\nDrafted the outline.\\n\\n[user]\\nRecent follow-up two: check the artifact wording.\\n\\n[assistant]\\nChecked.\\n\\n[user]\\nFinal goal: KEEP_MARKER_release_notes_artifact stays verbatim.\\n\\n[fast-jev-compaction stats] items=14 excluded_prompt_chars=93 excluded_prompt_reason=known-prompt opaque_items=0 calls=3 kept=1 results_dropped=1 calls_dropped=1 pinned=0 chars_before=6201 chars_after=2647 requests=1 stage=full\"}]}]}"},{"suite":"compaction","stream":true,"status":200,"contentType":"text/event-stream","headers":{"x-jev-compaction":"summary; kept=1; results_dropped=1; calls_dropped=1; pinned=0; chars_before=6201; chars_after=4120"},"body":"event: response.output_item.done\ndata: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"message\",\"role\":\"assistant\",\"id\":\"msg_jev_0000000000004000\",\"content\":[{\"type\":\"output_text\",\"text\":\"# fast-jev-compaction memory summary\\nTool calls and results below were classified by TypeSafe Jev (keep / truncate).\\nA dropped call keeps its provenance line and a bounded head of its result; nothing is erased\\noutright. Text kept verbatim is copied from the original transcript; this memory is text only, so\\nstructured tool-call replay and opaque items (encrypted reasoning or compaction payloads) are not restored.\\n\\n[user]\\nGoal: finish the release notes and keep KEEP_MARKER_release_notes_artifact verbatim.\\n\\n[assistant]\\nStarting with the old build logs.\\n\\n[tool call old_a] shell — drop_call\\ninput: {\\\"command\\\":\\\"tail -n 400 /tmp/old/build.log\\\"}\\n\\n[tool result old_a — truncated (call dropped), original was 2040 chars]\\nRESULT_A obsolete build log line. RESULT_A obsolete build log line. RESULT_A obsolete build log line. RESULT_A obsolete build log line. RESULT_A obsolete build log line. RESULT_A obsolete build log line. RESULT_A obsolete build log line. RESULT_A obsolete build log line. RESULT_A obsolete build log \\n[fast-jev-compaction truncated 1740 chars of this tool result; re-run the tool if needed]\\n\\n[tool call old_b] shell — drop_result\\ninput: {\\\"command\\\":\\\"grep -c deprecated /tmp/old/metrics.csv\\\"}\\n\\n[tool result old_b — truncated, original was 1860 chars]\\nRESULT_B obsolete metric line. RESULT_B obsolete metric line. RESULT_B obsolete metric line. RESULT_B obsolete metric line. RESULT_B obsolete metric line. RESULT_B obsolete metric line. RESULT_B obsolete metric line. RESULT_B obsolete metric line. RESULT_B obsolete metric line. RESULT_B obsolete met\\n[fast-jev-compaction truncated 1560 chars of this tool result; re-run the tool if needed]\\n\\n[tool call old_c] shell — keep\\ninput: {\\\"command\\\":\\\"ls -la /tmp/old/cache\\\"}\\n\\n[tool result old_c — kept verbatim]\\nRESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. RESULT_C obsolete cache line. \\n\\n[assistant]\\nThe old investigation is finished; those logs are obsolete.\\n\\n[user]\\nRecent follow-up one: draft the release notes outline.\\n\\n[assistant]\\nDrafted the outline.\\n\\n[user]\\nRecent follow-up two: check the artifact wording.\\n\\n[assistant]\\nChecked.\\n\\n[user]\\nFinal goal: KEEP_MARKER_release_notes_artifact stays verbatim.\\n\\n[fast-jev-compaction stats] items=14 excluded_prompt_chars=93 excluded_prompt_reason=known-prompt opaque_items=0 calls=3 kept=1 results_dropped=1 calls_dropped=1 pinned=0 chars_before=6201 chars_after=2647 requests=1 stage=full\"}]}}\n\nevent: response.completed\ndata: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_jev_0000000000004000\",\"status\":\"completed\"}}\n\n"}])oracle");
  for (const auto& row : vectors.as_array()) {
    auto asker = std::make_shared<DecisionAsker>(); asker->verdicts = {{"t1", {0.1,0.1}}, {"t2", {0.9,0.1}}, {"t3", {0.9,0.9}}};
    auto pending = building(transcript(), asker, *rt::getBool(row,"stream")); const auto outcome = co_await std::move(pending); EXPECT_EQ(outcome.status,*rt::getNumber(row,"status")); EXPECT_EQ(outcome.contentType,std::string(*rt::getString(row,"contentType"))); EXPECT_EQ(outcome.body,std::string(*rt::getString(row,"body"))); const auto* headers = rt::get(row,"headers"); EXPECT_EQ(*outcome.headers.get("x-jev-compaction"),std::string(*rt::getString(*headers,"x-jev-compaction")));
  }
}
TEST("floor summary token bound matches exact Bun numbers") {
  const auto values = rt::jsonParse(R"oracle([[-1,-1],[0,0],[1,0],[10,8],[101,85],[1000000,850000]])oracle");
  for (const auto& row : values.as_array()) EXPECT_EQ(gateway::maxFloorSummaryTokens(rt::toNumber(row.as_array()[0])),rt::toNumber(row.as_array()[1]));
}

ASYNC_TEST("compaction handler preserves parse failures cancellation phases and nonsecret structural logs") {
  const Isolation isolation; rt::console::Capture capture({rt::console::Method::info}); gateway::JevCompactionDeps deps; deps.asker = dropping();
  const rt::Request malformed("https://fixture.example.invalid/v1/responses", rt::RequestInit{.method = "POST", .body = "{not json"}); auto malformedTask = gateway::handleJevResponsesCompaction(malformed, deps); auto malformedResponse = co_await std::move(malformedTask); EXPECT_EQ(malformedResponse.status(), 400); auto malformedRead = malformedResponse.text(); const auto malformedBody = co_await std::move(malformedRead); EXPECT(rt::includes(malformedBody, "unparsable-body"));
  auto raw = body(transcript()).as_object(); raw["stream"] = "true"; const rt::Request invalid("https://fixture.example.invalid/v1/responses", rt::RequestInit{.method = "POST", .body = rt::jsonStringify(raw)}); auto invalidTask = gateway::handleJevResponsesCompaction(invalid, deps); auto invalidResponse = co_await std::move(invalidTask); EXPECT_EQ(invalidResponse.status(), 400); auto invalidRead = invalidResponse.json(); const auto invalidBody = co_await std::move(invalidRead); EXPECT_EQ(rt::getString(*rt::get(invalidBody, "error"), "param"), std::optional<std::string_view>("stream"));
  rt::AbortController aborting; auto failingAsker = dropping(); failingAsker->onAsk = [aborting]() mutable { aborting.abort(); }; failingAsker->failure = "private mock transcript must not reach logs"; deps.asker = failingAsker; deps.signal = aborting.signal();
  auto cancelledTask = gateway::handleJevResponsesCompaction(compactionRequest(transcript()), deps); const auto cancelled = co_await std::move(cancelledTask); EXPECT_EQ(cancelled.status(), 499); EXPECT(!openai_telemetry::getResponseTelemetry(cancelled));
  rt::AbortController afterSuccess; auto successfulAsker = dropping(); successfulAsker->onAsk = [afterSuccess]() mutable { afterSuccess.abort(); }; deps.asker = successfulAsker; deps.signal = afterSuccess.signal(); auto lateTask = gateway::handleJevResponsesCompaction(compactionRequest(transcript()), deps); const auto late = co_await std::move(lateTask); EXPECT_EQ(late.status(), 499); EXPECT(!openai_telemetry::getResponseTelemetry(late));
  const std::vector<std::pair<std::string, std::string>> failures{{"Jev request failed (503): private mock transcript must not reach logs", "http=503"}, {"Jev request timed out after 30000ms", "timeout"}, {"Jev request aborted", "aborted"}, {"upstream returned malformed JSON", "malformed-response"}, {"Invalid Jev answer private mock transcript must not reach logs", "invalid-answer"}, {"no room for questions", "state-over-budget"}, {"history too large for Jev", "state-too-large"}};
  deps.signal.reset();
  for (const auto& [message, expected] : failures) { auto asker = dropping(); asker->failure = message; deps.asker = asker; auto failing = gateway::handleJevResponsesCompaction(compactionRequest(transcript()), deps); const auto response = co_await std::move(failing); EXPECT_EQ(response.status(), 502); const auto& last = capture.calls().back(); const auto log = rt::jsonParse(last.args[1].string()); EXPECT_EQ(rt::getString(log, "detail"), std::optional<std::string_view>(expected)); }
  for (const auto& entry : capture.calls()) EXPECT(!rt::includes(entry.text, "private mock transcript must not reach logs"));
}

ASYNC_TEST("default Jev asker uses only the mocked System One endpoint and resolved provider key") {
  const Isolation isolation; rt::console::Capture capture({rt::console::Method::info, rt::console::Method::warn}); int calls = 0; gateway::JevCompactionDeps deps; deps.apiKey = "resolved-fixture-key";
  const rt::GlobalFetchOverride blocked(rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Response { throw rt::JsError("unmocked external fetch prohibited"); }));
  deps.fetch = rt::Fetch([&calls](rt::RequestInfo url, rt::RequestInit init) { ++calls; EXPECT_EQ(std::get<std::string>(url), std::string("https://openrouter.ai/api/v1/systemone")); EXPECT_EQ(init.headers->get("authorization"), std::optional<std::string>("Bearer resolved-fixture-key")); const auto sent = rt::jsonParse(std::get<std::string>(*init.body)); EXPECT_EQ(rt::getString(sent, "model"), std::optional<std::string_view>("~typesafe/jev-latest")); const auto* questions = rt::getObject(sent, "questions"); EXPECT(questions); json::object answers; for (const auto* entry : rt::objectEntries(*questions)) answers[entry->key()] = json::object{{"noul", 0.1}}; return rt::Response::json(json::object{{"answers", answers}}); });
  auto pending = gateway::handleJevResponsesCompaction(compactionRequest(transcript(), false), deps); const auto response = co_await std::move(pending); EXPECT_EQ(response.status(), 200); EXPECT(calls > 0); EXPECT_EQ(response.headers().get("content-type"), std::optional<std::string>("application/json")); EXPECT_EQ(response.headers().get("cache-control"), std::optional<std::string>("no-store"));
}

ASYNC_TEST("compaction structural refusal kinds and full messages match exact isolated Bun records") {
  const Isolation isolation; const auto vectors=rt::jsonParse(R"oracle([{"suite":"failure","input":null,"kind":"missing-input","message":"compaction request has no input items array"},{"suite":"failure","input":[],"kind":"empty-transcript","message":"compaction request has no conversation content"},{"suite":"failure","input":[{"type":"message","role":"user","content":[{"type":"input_text","text":"hello"}]},{"type":"message","role":"assistant","content":[{"type":"input_text","text":"hi"}]}],"kind":"no-candidates","message":"no unpinned tool call/result pairs to decide"}])oracle");
  const gateway::BuildCompactionResponseOptions options{false};
  for(const auto& row:vectors.as_array()){const json::value inputBody=json::object{{"input",*rt::get(row,"input")}};auto pending=gateway::buildCompactionResponse(inputBody,dropping(),options);std::string kind,message;try{const auto ignored=co_await std::move(pending);(void)ignored;}catch(const gateway::CompactionUnavailable& error){kind=error.kind;message=error.message;}EXPECT_EQ(kind,std::string(*rt::getString(row,"kind")));EXPECT_EQ(message,std::string(*rt::getString(row,"message")));}
}
