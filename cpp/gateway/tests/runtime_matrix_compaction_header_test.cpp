// Links: tests/runtime_matrix_compaction_header_test.cpp
//        src/lib/jev_compaction/state.cpp src/jev_compaction/compaction.cpp src/request.cpp src/jev_compaction/hybrid.cpp src/provider/cerebras.cpp src/config.cpp
//        src/openai_telemetry.cpp src/utils.cpp src/lib/jev_compaction/codex_items.cpp src/lib/jev_compaction/types.cpp src/provider/removed_provider_telemetry.cpp src/lib/jev_compaction/compact.cpp
//        src/responses_stream.cpp src/request_policy.cpp src/provider/selection.cpp src/release.cpp src/kv.cpp src/http.cpp
//        src/provider/openrouter.cpp src/models/codex_models_fetch.cpp src/codex/dispatch.cpp src/lib/jev_compaction/client.cpp src/defaults.cpp src/runtime_config.cpp
//        src/models/codex_models.cpp src/types.cpp src/provider/claude_code_models.cpp src/lib/jev_compaction/request.cpp src/models/openrouter_models.cpp src/provider/health.cpp
//        src/codex/auth_refresh.cpp src/codex/auth.cpp src/responses_failover_stream.cpp src/codex/routing_mutations.cpp src/authority/native_codex_capacity.cpp src/codex/responses_error.cpp
//        src/storage/kv.cpp src/storage/native_kv.cpp src/provider/surplus_claude_request.cpp src/provider/metered.cpp src/codex/capacity_routing.cpp src/provider/surplus.cpp
//        src/provider/claude_code_accounts.cpp src/authority/native_codex_mutations.cpp src/auth/relay.cpp src/provider/capacity_events.cpp src/authority/native_codex_contracts.cpp src/provider/cerebras_rate_limits.cpp
//        src/provider/claude_code_credentials.cpp src/codex/banked_reset.cpp src/storage/sqlite_readonly.cpp src/provider/claude_code.cpp src/codex/rate_limit_429.cpp src/codex/routing_state.cpp
//        src/provider/anthropic_replay.cpp src/authority/native_codex_evaluation.cpp src/codex/reset_settings.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp
//        src/models/codex_models_availability.cpp src/provider/anthropic_chat_responses.cpp src/provider/capacity_contract.cpp src/authority/native_codex_429.cpp src/codex/banked_reset_provider.cpp src/codex/native_auth.cpp
//        src/provider/anthropic_responses.cpp src/provider/anthropic_response_wire.cpp src/bounded_response_body.cpp src/codex/native_auth_transport.cpp src/codex/external_auth.cpp src/provider/surplus_claude_chat_request.cpp
//        src/provider/capacity.cpp src/provider/claude_capacity_history.cpp src/provider/capacity_history.cpp src/provider/claude_capacity_rollups.cpp src/provider/capacity_rollups.cpp src/provider/capacity_parse.cpp
//        src/provider/anthropic_chat_stream.cpp src/codex/experiment.cpp src/cache/prompt_analytics_read.cpp src/cache/prompt_analytics_core.cpp src/paid_fallback/ledger_stats.cpp src/runtime/websocket.cpp
//        src/provider/capacity_analytics.cpp src/metered_quota.cpp src/codex/account_routing.cpp src/codex/routing_serial.cpp src/codex/routing_evaluation.cpp src/authority/native_codex.cpp
//        src/codex/overage_settings.cpp
// Port of the library and standalone gateway-module controls in tests/runtime-matrix-compaction-header.test.ts.
// Deferred tools/context-replay.ts source-spy harness cases (outside this module manifest):
// "compaction header controls follow the original source model and predicate while refusing weakened admission, dispatch or Jev evidence" (line99);
// "fallback setup retains exact original-source question pairs, waterfall, selection and request bytes" (line324);
// "exact source question names reject zero-based, missing, extra, swapped and regrouped pairs" (line328).
#include "uos/jev_compaction/compaction.hpp"
#include "uos/config.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/console.hpp"
#include "uos/lib/jev_compaction/codex_items.hpp"
#include "uos/lib/jev_compaction/compact.hpp"
#include "uos/lib/jev_compaction/state.hpp"
#include "uos/lib/jev_compaction/types.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace uos;
namespace jc = uos::lib::jev_compaction;
using jc::codex_items::isResponsesCompaction;
using jc::codex_items::parseTurnMetadata;
using jc::types::JevAsker;
using jc::types::JevQuestions;
using jc::types::JevResponse;
using jc::types::JevState;

namespace {
  std::string digest(std::string_view text) { return rt::crypto::createHash("sha256").update(text).digest("hex"); }

  json::value message(const std::string& role, const std::string& text) {
    json::object part;
    part["type"] = "input_text";
    part["text"] = text;
    json::object item;
    item["type"] = "message";
    item["role"] = role;
    item["content"] = json::array{json::value(std::move(part))};
    return item;
  }

  json::value functionCall(const std::string& callId, const std::string& arguments) {
    json::object item;
    item["type"] = "function_call";
    item["call_id"] = callId;
    item["name"] = "shell";
    item["arguments"] = arguments;
    return item;
  }

  json::value functionOutput(const std::string& callId, const std::string& output) {
    json::object item;
    item["type"] = "function_call_output";
    item["call_id"] = callId;
    item["output"] = output;
    return item;
  }

  // `compactionInput(marker)` of tools/cloudflare/runtime-matrix/context-replay.ts.
  json::value compactionInput(const std::string& marker) {
    const std::string keptCompactionText = "retain_compaction_fallback_artifact";
    const std::string oldCompactionResult = rt::repeat("Obsolete tool result with its original evidence. ", 100);
    json::array items;
    items.push_back(message("user", "Keep " + keptCompactionText + " " + marker + " verbatim while completing the release."));
    items.push_back(message("assistant", "Inspecting old evidence."));
    items.push_back(functionCall("old_a", R"({"command":"read old evidence"})"));
    items.push_back(functionOutput("old_a", oldCompactionResult));
    items.push_back(functionCall("old_b", R"({"command":"read more old evidence"})"));
    items.push_back(functionOutput("old_b", oldCompactionResult));
    items.push_back(message("assistant", "The old investigation is finished."));
    items.push_back(message("user", "First recent follow-up: draft the release outline."));
    items.push_back(message("assistant", "Drafted."));
    items.push_back(message("user", "Second recent follow-up: check the artifact."));
    items.push_back(message("assistant", "Checked."));
    items.push_back(message("user", "Final goal: retain " + keptCompactionText + " " + marker + "."));
    items.push_back(message("user", "You are performing a CONTEXT CHECKPOINT COMPACTION. Create a handoff summary."));
    return items;
  }

  jc::types::CompactOptions compactionOptions() { return uos::jev_compaction::compaction::COMPACTION_OPTIONS; }

  // The asker of `originalQuestions`: records every batch's question names, then fails
  // ("failure") or answers every name but the first with `noul: 0.1` ("partial").
  class QuestionAsker : public JevAsker {
  public:
    explicit QuestionAsker(std::string mode_) : mode(std::move(mode_)) {}
    std::vector<std::vector<std::string>> batches;
    rt::Task<JevResponse> ask(JevState, JevQuestions questions) override {
      std::vector<std::string> names;
      for (const auto& [name, question] : questions) names.push_back(name);
      batches.push_back(names);
      if (mode == "failure") throw rt::JsError("controlled original-source decision failure");
      json::object answers;
      for (std::size_t index = 1; index < names.size(); ++index) {
        json::object answer;
        answer["noul"] = 0.1;
        answers[names[index]] = std::move(answer);
      }
      JevResponse response;
      response["answers"] = std::move(answers);
      co_return response;
    }

  private:
    std::string mode;
  };

  rt::Task<std::vector<std::string>> originalQuestions(json::value input, std::string mode) {
    const std::string original = rt::jsonStringify(input);
    const std::optional<jc::codex_items::CodexTranscript> transcript = jc::codex_items::parseCodexInput(input);
    if (!transcript) throw rt::JsError("transcript did not parse");
    EXPECT_EQ(transcript->messages.size(), std::size_t(12));
    const std::vector<jc::types::ToolCall> calls = jc::state::collectToolCalls(transcript->messages, 6);
    EXPECT_EQ(calls.size(), std::size_t(2));
    if (calls.size() == 2) {
      EXPECT_EQ(calls[0].id, std::string("t1"));
      EXPECT_EQ(calls[0].tool_use_id, std::string("old_a"));
      EXPECT_EQ(calls[0].tool, std::string("shell"));
      EXPECT(rt::deepEqual(json::value(calls[0].input), rt::jsonParse(R"({"command":"read old evidence"})")));
      EXPECT_EQ(calls[0].callIndex, std::int64_t(2));
      EXPECT_EQ(calls[0].resultIndex, std::int64_t(3));
      EXPECT_EQ(calls[0].pinned, false);
      EXPECT_EQ(calls[1].id, std::string("t2"));
      EXPECT_EQ(calls[1].tool_use_id, std::string("old_b"));
      EXPECT(rt::deepEqual(json::value(calls[1].input), rt::jsonParse(R"({"command":"read more old evidence"})")));
      EXPECT_EQ(calls[1].callIndex, std::int64_t(4));
      EXPECT_EQ(calls[1].resultIndex, std::int64_t(5));
      EXPECT_EQ(calls[1].pinned, false);
    }
    for (const jc::types::ToolCall& entry : calls) {
      const auto callIndex = static_cast<std::size_t>(entry.callIndex);
      const auto resultIndex = static_cast<std::size_t>(entry.resultIndex);
      ASSERT(callIndex < transcript->messages.size() && resultIndex < transcript->messages.size());
      const jc::types::Message& toolMessage = transcript->messages[callIndex];
      const jc::types::Message& resultMessage = transcript->messages[resultIndex];
      ASSERT(!toolMessage.toolUses.empty() && resultMessage.toolResults && !resultMessage.toolResults->empty());
      EXPECT_EQ(toolMessage.toolUses[0].tool_use_id, entry.tool_use_id);
      EXPECT_EQ(resultMessage.toolResults->at(0).tool_use_id, entry.tool_use_id);
      EXPECT_EQ(rt::length(resultMessage.toolResults->at(0).text), std::int64_t(4900));
      EXPECT_EQ(digest(resultMessage.toolResults->at(0).text), std::string("4103c9be01eb1089fdeb25375b6d644663e32e228b5ee1593e6a49399667d22d"));
    }
    auto asker = std::make_shared<QuestionAsker>(mode);
    bool rejected = false;
    try {
      auto task = jc::compact::compact(transcript->messages, asker, compactionOptions());
      (void)co_await std::move(task);
    } catch (const rt::JsError&) {
      rejected = true;
    }
    EXPECT(rejected);
    const std::vector<std::vector<std::string>> expectedBatches = {{"call_t1", "result_t1", "call_t2", "result_t2"}};
    EXPECT_EQ(asker->batches, expectedBatches);
    EXPECT_EQ(rt::jsonStringify(input), original);
    std::vector<std::string> names = asker->batches.empty() ? std::vector<std::string>() : asker->batches[0];
    co_return names;
  }
}

TEST("compaction header controls follow the original source model and predicate while refusing weakened admission, dispatch or Jev evidence (lib predicate)") {
  EXPECT(isResponsesCompaction(parseTurnMetadata(std::string(R"({"request_kind":"compaction","compaction":{"implementation":"responses"}})"))));
  // The four ordinary-route controls: absent, malformed, wrong implementation, ordinary turn.
  EXPECT(!isResponsesCompaction(parseTurnMetadata(std::nullopt)));
  EXPECT(!isResponsesCompaction(parseTurnMetadata(std::string("{not json"))));
  EXPECT(!isResponsesCompaction(parseTurnMetadata(std::string(R"({"request_kind":"compaction","compaction":{"implementation":"responses_compaction_v2"}})"))));
  EXPECT(!isResponsesCompaction(parseTurnMetadata(std::string(R"({"request_kind":"turn","compaction":{"implementation":"responses"}})"))));
}

ASYNC_TEST("fallback setup retains exact original-source question pairs, waterfall, selection and request bytes (lib question pairs)") {
  const std::vector<std::string> expected = {"call_t1", "result_t1", "call_t2", "result_t2"};
  const std::vector<std::string> modes = {"failure", "partial"};
  for (const std::string& mode : modes) {
    const json::value input = compactionInput("fixture:matrix:compaction:quota-" + mode);
    auto task = originalQuestions(input, mode);
    const std::vector<std::string> names = co_await std::move(task);
    EXPECT_EQ(names, expected);
  }
}

TEST("gateway compaction header predicate preserves exact matrix controls without reading bodies") {
  namespace gateway = uos::jev_compaction::compaction;
  const auto serialized = rt::jsonStringify(compactionInput("fixture:matrix:controls"));
  const std::vector<std::optional<std::string>> metadata{std::nullopt, "{not json", R"({"request_kind":"compaction","compaction":{"implementation":"responses_compaction_v2"}})", R"({"request_kind":"turn","compaction":{"implementation":"responses"}})", R"({"request_kind":"compaction","compaction":{"implementation":"responses"}})"};
  for (std::size_t index = 0; index < metadata.size(); ++index) {
    rt::Headers headers; if (metadata[index]) headers.set("x-codex-turn-metadata", *metadata[index]);
    const rt::Request request("https://fixture.example.invalid/v1/responses", rt::RequestInit{.method = "POST", .headers = headers, .body = serialized});
    EXPECT_EQ(gateway::isJevCompactionRequest(request), index == 4); EXPECT(!request.bodyUsed());
  }
}

ASYNC_TEST("standalone matrix failure and partial decisions retain exact input bytes and fail closed") {
  namespace gateway = uos::jev_compaction::compaction;
  const rt::env::ScopedEnvironment isolation{std::vector<std::pair<std::string,std::string>>{{"HOME","/nonexistent/w07c7"}}};
  const auto previous = config::config.codexAuthJsonB64; config::config.codexAuthJsonB64.clear(); const auto restore = rt::scopeExit([previous] { config::config.codexAuthJsonB64 = previous; });
  rt::console::Capture capture({rt::console::Method::info}); const std::vector<std::string> modes{"failure", "partial"};
  for (const auto& mode : modes) {
    const auto input = compactionInput("fixture:matrix:compaction:quota-" + mode); const auto serialized = rt::jsonStringify(input); const auto inputHash = digest(serialized);
    const rt::Request request("https://fixture.example.invalid/v1/responses", rt::RequestInit{.method = "POST", .body = rt::jsonStringify(json::object{{"input", input}, {"stream", true}})});
    gateway::JevCompactionDeps deps; deps.asker = std::make_shared<QuestionAsker>(mode); auto handling = gateway::handleJevResponsesCompaction(request, deps); auto response = co_await std::move(handling); EXPECT_EQ(response.status(), 502); EXPECT(!response.headers().has("x-jev-compaction"));
    auto reading = response.text(); const auto body = co_await std::move(reading); EXPECT(rt::includes(body, "jev_compaction_unavailable")); EXPECT(!rt::includes(body, "response.completed")); EXPECT_EQ(digest(rt::jsonStringify(input)), inputHash); EXPECT_EQ(rt::jsonStringify(input), serialized);
  }
}
