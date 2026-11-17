#include "uos/harmony/probe_scenarios.hpp"

#include "uos/harmony/conversation.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"

#include <array>

namespace uos::harmony::probe_scenarios {
  namespace {
    inline constexpr double MAX_TOKENS = 256;
    inline constexpr std::string_view WEATHER_QUESTION = "Call get_weather with location San Francisco, then report the weather in one sentence.";
    inline constexpr std::string_view WEATHER_RESULT = "{\"temperature\": 20, \"description\": \"sunny\"}";
    inline const classifier::BootstrapObservation CLASSIFIER_OBSERVATION{
      .runId = "probe-run-42", .generation = 2, .phase = "apply", .milestone = "implementation-verified", .failureFingerprint = "verification-failed-v1",
      .gitSha = "abc1234def5678", .ledgerVersion = 7, .retryState = "retry-2", .verificationEvidence = "verification command passed",
    };
    inline constexpr std::string_view CLASSIFIER_DECISION =
      "Decide whether the run made material progress since the previous evaluation. "
      "Return true only if new durable evidence is present: a later verified milestone, "
      "a new accepted Git identity, or a monotonic ledger advance tied to useful work. "
      "Return false when nothing durable changed or the run cycles without advancement. "
      "Answer with exactly one word: true or false.";

    probes::ProbeOutcome classifyOutcome(const std::vector<probes::ProbeTurnRun>& runs) {
      for (const auto& run : runs) if (run.record.outcome != "ok") return run.record.outcome;
      return "ok";
    }
    probes::ProbeScenarioResult scenarioResult(std::size_t index, const probes::ProbeContext& context, const rt::Date& startedAt,
      const std::vector<probes::ProbeTurnRun>& runs, std::vector<std::string> notes = {}, std::optional<std::string> failure = {}) {
      const auto& scenario = PROBE_SCENARIOS[index];
      std::vector<probes::ProbeTurnRecord> turns;
      std::optional<classifier::BootstrapClassifierVerdict> verdict;
      for (const auto& run : runs) { turns.push_back(run.record); if (!verdict && run.verdict) verdict = run.verdict; }
      return {.id = scenario.id, .group = scenario.group, .style = scenario.style, .description = scenario.description, .expectedOutcome = scenario.expectedOutcome,
        .outcome = failure && !failure->empty() ? "failed" : classifyOutcome(runs), .startedAt = startedAt.toISOString(),
        .durationMs = context.now().getTime() - startedAt.getTime(), .turns = std::move(turns), .verdict = verdict, .failure = std::move(failure), .notes = std::move(notes)};
    }
    std::optional<types::ToolCall> firstToolCallFrom(const probes::ProbeTurnRun& run) {
      if (!run.normalized || run.normalized->toolCalls.empty()) return std::nullopt;
      return run.normalized->toolCalls[0];
    }
    std::string count(double number) { return rt::numberToString(number); }
    std::string boolean(bool value) { return value ? "true" : "false"; }

    rt::Task<probes::ProbeScenarioResult> runScenario(std::size_t index, probes::ProbeContext context) {
      const auto started = context.now();
      std::vector<probes::ProbeTurnRun> runs;
      if (index >= 18) {
        classifier::BootstrapClassifierRequestOptions options{.observation = CLASSIFIER_OBSERVATION, .decisionDefinition = std::string(CLASSIFIER_DECISION), .reasoningEffort = index == 18 ? "low" : "medium"};
        rt::Promise<probes::ProbeTurnRun> pending(context.runClassifier(options));
        auto turn = co_await pending.wait();
        runs.push_back(std::move(turn));
        co_return scenarioResult(index, context, started, runs, {"maxOutputTokens=128"});
      }
      const bool native = index == 6 || index == 7 || index == 14 || index == 16;
      probes::ProbeTurnOptions options{.style = native ? "native" : "generic"};
      options.maxCompletionTokens = MAX_TOKENS;
      if (!native) options.reasoningEffort = index <= 2 ? std::array<std::string, 3>{"low", "medium", "high"}[index] : "low";
      std::string prompt = "What is 2 + 2? Answer with only the number.";
      if (index >= 5 && index <= 11) prompt = WEATHER_QUESTION;
      if (index == 8) prompt += " After you receive the weather, call save_note with the result.";
      if (index >= 12 && index <= 14) prompt = "Return a JSON object with an integer field \"answer\" equal to 4.";
      if (index == 15) prompt = "Call get_weather for San Francisco, then return a JSON object with the temperature.";
      if (index == 16 || index == 17) prompt = "Call get_weather twice in the same response: location San Francisco and location Tokyo.";
      if ((index >= 5 && index <= 11) || index >= 15) options.tools = std::vector<types::ToolDefinition>{probes::WEATHER_TOOL};
      if (index == 8 || index == 9 || index == 10 || index == 11) options.tools->push_back(probes::NOTE_TOOL);
      if (index == 6 || index == 7 || index == 16) options.instructions = "You are a weather assistant with access to get_weather.";
      if (index == 7) options.nativeToolResultStyle = "user-role";
      if (index == 9) { options.tools->at(0).strict = true; options.tools->at(1).strict = false; options.toolStrictnessMode = "preserve"; }
      if (index == 11) options.toolStrictnessMode = "normalize-true";
      if (index == 12) options.responseFormat = types::ResponseFormatParamJsonObject{};
      if (index == 13 || index == 15) options.responseFormat = types::ResponseFormatParamJsonSchema{
        .json_schema = {.name = index == 13 ? "answer_response" : "weather_answer", .strict = true, .schema = probes::ANSWER_SCHEMA}};
      if (index == 14) { options.nativeResponseFormat = types::NativeResponseFormat{.formatName = "answer_response", .description = "The computed answer", .schema = probes::ANSWER_SCHEMA}; options.instructions = "You are a math assistant. Use the Response Format below."; }
      if (index == 15) options.combinationPolicy = "probe";
      if (index == 17) options.parallelToolCalls = true;
      auto conversation = probes::user(prompt);
      rt::Promise<probes::ProbeTurnRun> first(context.runTurn(conversation, options));
      auto t1 = co_await first.wait();
      runs.push_back(t1);
      if (index <= 2) co_return scenarioResult(index, context, started, runs, {"effort=" + *options.reasoningEffort});
      if (index == 3) {
        if (!t1.normalized) co_return scenarioResult(index, context, started, runs);
        const auto withAnswer = conversation::advanceConversation(conversation, *t1.normalized);
        const auto next = conversation::appendUser(withAnswer, "What is 9 / 2? Answer with only the number.");
        const auto replayed = conversation::dropAnalysisBeforeCompletedFinal(next);
        rt::Promise<probes::ProbeTurnRun> second(context.runTurn(replayed, options));
        auto t2 = co_await second.wait();
        runs.push_back(std::move(t2));
        co_return scenarioResult(index, context, started, runs, {"analysisLinesAfterFinal=" + count(static_cast<double>(conversation::analysisLineCount(replayed))), "wireAnalysisEcho=false"});
      }
      if (index == 4) {
        if (!t1.normalized) co_return scenarioResult(index, context, started, runs);
        json::object assistant{{"role", "assistant"}, {"content", t1.normalized->content.value_or("")}};
        if (!t1.normalized->analysis.empty()) assistant["reasoning_content"] = rt::join(t1.normalized->analysis, "\n");
        json::object body{{"model", types::HARMONY_CEREBRAS_MODEL}, {"messages", json::array{
          json::object{{"role", "user"}, {"content", "What is 2 + 2? Answer with only the number."}}, std::move(assistant),
          json::object{{"role", "user"}, {"content", "What is 9 / 2? Answer with only the number."}}}}, {"stream", false}, {"max_completion_tokens", MAX_TOKENS}};
        rt::Promise<probes::ProbeTurnRun> second(context.runRaw(body, conversation));
        auto t2 = co_await second.wait();
        const auto status = t2.record.status ? count(*t2.record.status) : "null";
        const std::string note = t2.record.outcome == "upstream_rejected" ? "boundary evidence: reasoning_content rejected with status " + status : "boundary evidence: reasoning_content accepted (status " + status + ")";
        runs.push_back(std::move(t2));
        co_return scenarioResult(index, context, started, runs, {note});
      }
      if (index >= 5 && index <= 8) {
        const auto call = firstToolCallFrom(t1);
        if (!call || !t1.normalized) co_return scenarioResult(index, context, started, runs, {index == 8 ? "no first tool call" : "no tool call returned"});
        conversation = conversation::appendToolResult(conversation::advanceConversation(conversation, *t1.normalized), call->id, call->name, std::string(WEATHER_RESULT));
        rt::Promise<probes::ProbeTurnRun> second(context.runTurn(conversation, options));
        auto t2 = co_await second.wait();
        runs.push_back(t2);
        if (index == 5) co_return scenarioResult(index, context, started, runs, {"call.id='" + call->id + "'", "call.name='" + call->name + "'", "call.argumentsJsonValid=" + boolean(probes::argsJsonValid(call->arguments)), "toolResultTurns=1"});
        if (index == 6) {
          const auto calls = t1.record.response ? t1.record.response->toolCalls.size() : 0;
          const bool parsedFromContent = t1.record.response && t1.record.response->toolCalls.empty();
          co_return scenarioResult(index, context, started, runs, {"toolCallsFromWire=" + count(static_cast<double>(calls)), "toolCallsParsedFromContent=" + boolean(parsedFromContent), "resultReplayStyle=tool-role"});
        }
        if (index == 7) co_return scenarioResult(index, context, started, runs, {"resultReplayStyle=user-role"});
        const auto call2 = firstToolCallFrom(t2);
        if (!call2 || !t2.normalized) co_return scenarioResult(index, context, started, runs, {"no second tool call"});
        conversation = conversation::appendToolResult(conversation::advanceConversation(conversation, *t2.normalized), call2->id, call2->name, "{\"text\": \"San Francisco: sunny, 20C\"}");
        rt::Promise<probes::ProbeTurnRun> third(context.runTurn(conversation, options));
        auto t3 = co_await third.wait();
        runs.push_back(std::move(t3));
        co_return scenarioResult(index, context, started, runs, {"analysisLinesState=" + count(static_cast<double>(conversation::analysisLineCount(conversation))), "pendingToolCallsAtEnd=" + count(static_cast<double>(conversation::pendingToolCallCount(conversation))), "secondCall.name='" + call2->name + "'"});
      }
      if (index == 9) {
        std::vector<std::string> values;
        if (t1.record.request) for (const bool value : t1.record.request->toolStrictnessValues) values.push_back(boolean(value));
        co_return scenarioResult(index, context, started, runs, {"strictnessValues=" + rt::join(values, ",")});
      }
      if (index == 10) co_return scenarioResult(index, context, started, runs, {"strictness=all-false"});
      if (index == 11) co_return scenarioResult(index, context, started, runs, {"strictness=all-true"});
      if (index == 12) co_return scenarioResult(index, context, started, runs, {"contentJsonValid=" + boolean(probes::argsJsonValid(t1.record.response ? t1.record.response->contentPreview.value_or("") : ""))});
      if (index == 13) co_return scenarioResult(index, context, started, runs, {"schema=answer_response"});
      if (index == 14) co_return scenarioResult(index, context, started, runs, {"responseFormat=developer"});
      if (index == 15) co_return scenarioResult(index, context, started, runs, {"combinationPolicy=probe"});
      const auto calls = t1.record.response ? t1.record.response->toolCalls.size() : 0;
      std::vector<std::string> notes{"toolCallCount=" + count(static_cast<double>(calls))};
      if (index == 17) notes.push_back("parallelToolCalls=true");
      co_return scenarioResult(index, context, started, runs, std::move(notes));
    }
  }
  std::vector<ProbeScenario> makeProbeScenarios() {
    struct Definition { std::string_view id; std::string_view group; std::string_view style; std::string_view description; std::string_view expectedOutcome; };
    const std::array<Definition, 20> definitions{{
      {"reasoning.effort.low", "reasoning", "generic", "Reasoning effort 'low': does gpt-oss return reasoning plus final content?", "ok"},
      {"reasoning.effort.medium", "reasoning", "generic", "Reasoning effort 'medium' (the model default): reasoning plus final content?", "ok"},
      {"reasoning.effort.high", "reasoning", "generic", "Reasoning effort 'high': reasoning plus final content?", "ok"},
      {"reasoning.replay.after-final", "replay", "generic", "After a completed final answer a follow-up replays normalized state without old analysis.", "ok"},
      {"reasoning.replay.echo", "replay", "generic", "Deliberate echo of reasoning_content in request history: accepted or rejected at the boundary?", "upstream_rejected"},
      {"tools.generic.sequence", "tools", "generic", "Generic tools: one call, normalized tool-call shape, result replay, then a final answer.", "ok"},
      {"tools.native.sequence", "tools", "native", "Native Harmony: tools rendered as Harmony types in the developer message, result replayed via tool role.", "ok"},
      {"tools.native.user-result", "tools", "native", "Native Harmony with the tool result replayed as a user message (no tool role).", "ok"},
      {"tools.generic.consecutive", "tools", "generic", "Two consecutive tool turns: call, result, call, result, then a final answer.", "ok"},
      {"strictness.mixed", "strictness", "generic", "Two tools with different strict values in one request: accepted or rejected?", "upstream_rejected"},
      {"strictness.all-false", "strictness", "generic", "All tools normalized to strict=false: accepted?", "ok"},
      {"strictness.all-true", "strictness", "generic", "All tools normalized to strict=true: accepted?", "ok"},
      {"structured.json-object", "structured", "generic", "Structured output alone with response_format json_object: JSON in final content?", "ok"},
      {"structured.json-schema", "structured", "generic", "Structured output alone with response_format json_schema: JSON matching the schema?", "ok"},
      {"structured.native-formats", "structured", "native", "Native Harmony: schema rendered as a Response Format in the developer message.", "ok"},
      {"structured.with-tools", "structured", "generic", "Tools combined with response_format json_schema: supported or rejected?", "upstream_rejected"},
      {"parallel.native", "parallel", "native", "Native Harmony: prompt asks for two parallel calls in one turn; how many calls are returned?", "ok"},
      {"parallel.generic-flag", "parallel", "generic", "Generic with parallel_tool_calls=true: accepted? how many calls are returned?", "ok"},
      {"classifier.low", "classifier", "classifier", "Zero-tool bounded classifier request at reasoning effort low: literal true/false verdict?", "ok"},
      {"classifier.medium", "classifier", "classifier", "Same zero-tool bounded classifier request at reasoning effort medium: literal true/false verdict?", "ok"},
    }};
    std::vector<ProbeScenario> scenarios;
    for (std::size_t index = 0; index < definitions.size(); ++index) {
      const auto& d = definitions[index];
      scenarios.push_back({.id = std::string(d.id), .group = std::string(d.group), .style = std::string(d.style), .description = std::string(d.description),
        .expectedOutcome = std::string(d.expectedOutcome), .run = [index](probes::ProbeContext context) { return runScenario(index, std::move(context)); }});
    }
    return scenarios;
  }
}
