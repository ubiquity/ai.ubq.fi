// Differential cases for the harmony modules (parse, render, classifier, tools/schemas through
// reliability/feedback, reliability/context): inputs chosen to reach every branch the TS tests leave
// open (header orders, truncation, surrogate halves split by a marker, schema shapes, defaults,
// issue ordering, compaction policies and windows). Expected values were recorded from the TS
// modules under Bun 1.4.0 in tests/harmony_differential_vectors.inc.
// Links: tests/harmony_differential_test.cpp src/harmony/parse.cpp src/harmony/render.cpp src/harmony/classifier.cpp
//        src/harmony/conversation.cpp src/harmony/types.cpp src/harmony/tools/schemas.cpp src/harmony/reliability/feedback.cpp
//        src/harmony/reliability/context.cpp src/harmony/reliability/state.cpp src/harmony/reliability/verify.cpp
//        src/harmony/reliability/loops.cpp src/harmony/reliability/hash.cpp
#include "uos/harmony/classifier.hpp"
#include "uos/harmony/conversation.hpp"
#include "uos/harmony/parse.hpp"
#include "uos/harmony/reliability/context.hpp"
#include "uos/harmony/reliability/feedback.hpp"
#include "uos/harmony/reliability/state.hpp"
#include "uos/harmony/render.hpp"
#include "uos/harmony/types.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include "tests/harmony_differential_vectors.inc"

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace types = uos::harmony::types;
  namespace parse = uos::harmony::parse;
  namespace render = uos::harmony::render;
  namespace classifier = uos::harmony::classifier;
  namespace conversation = uos::harmony::conversation;
  namespace context = uos::harmony::reliability::context;
  namespace feedback = uos::harmony::reliability::feedback;
  namespace state = uos::harmony::reliability::state;
  namespace loops = uos::harmony::reliability::loops;

  const json::array& vectors() {
    static const json::value parsed = rt::jsonParse(HARMONY_DIFFERENTIAL_VECTORS);
    return parsed.as_array();
  }

  template <class Check> void forEach(std::string_view kind, Check check) {
    int count = 0;
    for (const json::value& vector : vectors()) {
      if (vector.at("kind").as_string() != kind) continue;
      count += 1;
      check(vector.as_object());
    }
    EXPECT(count > 0);
  }

  void expectSame(const json::value& actual, const json::value& expected, const json::value& input) {
    if (rt::deepEqual(actual, expected)) {
      EXPECT(true);
      return;
    }
    std::fprintf(stderr, "    input: %s\n    actual: %s\n    expected: %s\n", rt::jsonStringify(input).c_str(), rt::jsonStringify(actual).c_str(), rt::jsonStringify(expected).c_str());
    EXPECT(false);
  }

  json::value orNull(const std::optional<std::string>& value) { return value ? json::value(*value) : json::value(nullptr); }

  std::string text(const json::value& value) { return std::string(value.as_string()); }

  std::optional<std::string> optionalText(const json::object& object, std::string_view key) {
    const std::optional<std::string_view> value = rt::getString(object, key);
    return value ? std::optional<std::string>(std::string(*value)) : std::nullopt;
  }

  json::value turnJson(const types::HarmonyTurn& turn) {
    if (const auto* call = std::get_if<types::HarmonyTurnToolCall>(&turn)) {
      return json::object{{"kind", call->kind}, {"recipient", call->recipient}, {"name", call->name}, {"arguments", call->arguments}};
    }
    return std::visit(
      [](const auto& value) -> json::value {
        if constexpr (requires { value.text; }) return json::object{{"kind", value.kind}, {"text", value.text}};
        else return nullptr;
      },
      turn);
  }

  json::value parseResultJson(const types::HarmonyParseResult& result) {
    json::array messages;
    for (const types::HarmonyRawMessage& message : result.messages) {
      messages.push_back(json::object{
        {"role", message.role},
        {"channel", orNull(message.channel)},
        {"recipient", orNull(message.recipient)},
        {"constrain", orNull(message.constrain)},
        {"content", message.content},
        {"stoppedBy", message.stoppedBy},
      });
    }
    json::array turns;
    for (const types::HarmonyTurn& turn : result.turns) turns.push_back(turnJson(turn));
    return json::object{{"messages", std::move(messages)}, {"turns", std::move(turns)}, {"truncated", result.truncated}};
  }

  types::ConversationTurn turnFromJson(const json::object& turn) {
    const std::string role = text(turn.at("role"));
    if (role == "system") return types::ConversationTurnSystem{.content = text(turn.at("content"))};
    if (role == "developer") return types::ConversationTurnDeveloper{.content = text(turn.at("content"))};
    if (role == "user") return types::ConversationTurnUser{.content = text(turn.at("content"))};
    if (role == "tool") return types::ToolResultTurn{.toolCallId = text(turn.at("toolCallId")), .name = text(turn.at("name")), .content = text(turn.at("content"))};
    types::AssistantTurn assistant{.content = optionalText(turn, "content"), .analysis = {}, .toolCalls = {}, .finishReason = optionalText(turn, "finishReason")};
    for (const json::value& line : turn.at("analysis").as_array()) assistant.analysis.push_back(text(line));
    for (const json::value& call : turn.at("toolCalls").as_array()) {
      assistant.toolCalls.push_back({.id = text(call.at("id")), .name = text(call.at("name")), .arguments = text(call.at("arguments"))});
    }
    return assistant;
  }

  conversation::Conversation conversationFromJson(const json::value& turns) {
    std::vector<types::ConversationTurn> out;
    for (const json::value& turn : turns.as_array()) out.push_back(turnFromJson(turn.as_object()));
    return conversation::createConversation(std::move(out));
  }

  json::value conversationTurnJson(const types::ConversationTurn& turn) {
    if (const auto* assistant = std::get_if<types::AssistantTurn>(&turn)) {
      json::array analysis;
      for (const std::string& line : assistant->analysis) analysis.push_back(json::value(line));
      json::array calls;
      for (const types::ToolCall& call : assistant->toolCalls) calls.push_back(json::object{{"id", call.id}, {"name", call.name}, {"arguments", call.arguments}});
      return json::object{{"role", "assistant"}, {"content", orNull(assistant->content)}, {"analysis", analysis}, {"toolCalls", calls}, {"finishReason", orNull(assistant->finishReason)}};
    }
    if (const auto* tool = std::get_if<types::ToolResultTurn>(&turn)) {
      return json::object{{"role", "tool"}, {"toolCallId", tool->toolCallId}, {"name", tool->name}, {"content", tool->content}};
    }
    return std::visit(
      [](const auto& value) -> json::value {
        if constexpr (requires { value.toolCallId; } || requires { value.toolCalls; }) return nullptr;
        else return json::object{{"role", value.role}, {"content", value.content}};
      },
      turn);
  }

  json::value parsedJson(const std::optional<context::ParsedToolResult>& parsed) { return parsed ? json::value_from(*parsed) : json::value(nullptr); }

  json::value compactionJson(const context::CompactionResult& result) {
    json::array turns;
    for (const types::ConversationTurn& turn : result.conversation.turns) turns.push_back(conversationTurnJson(turn));
    json::array drops;
    for (const context::CompactionDrop& drop : result.drops) drops.push_back(json::object{{"kind", drop.kind}, {"detail", drop.detail}});
    return json::object{
      {"conversation", json::object{{"turns", std::move(turns)}}},
      {"drops", std::move(drops)},
      {"estimatedTokens", rt::makeNumber(result.estimatedTokens)},
      {"metBudget", result.metBudget},
      {"droppedTurnCount", result.droppedTurnCount},
    };
  }

  rt::Nullable<std::string> nullableText(const json::object& object, std::string_view key) {
    const json::value* value = rt::get(object, key);
    if (!value) return rt::undefined;
    if (value->is_null()) return nullptr;
    return text(*value);
  }
}

TEST("differential: parseHarmonyOutput") {
  forEach("parse", [](const json::object& vector) {
    expectSame(parseResultJson(parse::parseHarmonyOutput(text(vector.at("input")))), vector.at("output"), vector.at("input"));
  });
}

TEST("differential: normalizeToolArguments") {
  forEach("normalizeToolArguments", [](const json::object& vector) {
    expectSame(json::value(parse::normalizeToolArguments(text(vector.at("input")))), vector.at("output"), vector.at("input"));
  });
}

TEST("differential: harmonyTypeFromJsonSchema and quoteString") {
  forEach("harmonyType", [](const json::object& vector) {
    expectSame(json::value(render::harmonyTypeFromJsonSchema(vector.at("input"))), vector.at("output"), vector.at("input"));
  });
  forEach("quoteString", [](const json::object& vector) {
    expectSame(json::value(render::quoteString(text(vector.at("input")))), vector.at("output"), vector.at("input"));
  });
}

TEST("differential: renderSystemMessage") {
  forEach("renderSystem", [](const json::object& vector) {
    const json::object& input = vector.at("input").as_object();
    render::SystemRenderOptions options{.currentDate = text(input.at("currentDate")), .reasoningEffort = text(input.at("reasoningEffort"))};
    options.identity = optionalText(input, "identity");
    options.knowledgeCutoff = optionalText(input, "knowledgeCutoff");
    if (const json::array* channels = rt::getArray(input, "channels")) {
      options.channels = std::vector<std::string>{};
      for (const json::value& channel : *channels) options.channels->push_back(text(channel));
    }
    options.toolNamespace = nullableText(input, "toolNamespace");
    expectSame(json::value(render::renderSystemMessage(options)), vector.at("output"), vector.at("input"));
  });
}

TEST("differential: renderDeveloperMessage") {
  forEach("renderDeveloper", [](const json::object& vector) {
    const json::object& input = vector.at("input").as_object();
    render::DeveloperRenderOptions options{.instructions = text(input.at("instructions"))};
    if (const json::array* tools = rt::getArray(input, "tools")) {
      options.tools = std::vector<types::ToolDefinition>{};
      for (const json::value& tool : *tools) {
        options.tools->push_back(types::ToolDefinition{
          .name = text(tool.at("name")),
          .description = text(tool.at("description")),
          .parameters = tool.at("parameters").as_object(),
          .strict = rt::getBool(tool, "strict"),
        });
      }
    }
    if (const json::object* format = rt::getObject(input, "responseFormat")) {
      options.responseFormat = types::NativeResponseFormat{
        .formatName = text(format->at("formatName")),
        .description = optionalText(*format, "description"),
        .schema = format->at("schema").as_object(),
      };
    }
    options.namespace_ = optionalText(input, "namespace");
    expectSame(json::value(render::renderDeveloperMessage(options)), vector.at("output"), vector.at("input"));
  });
}

TEST("differential: validateToolArgumentsDetailed, renderValidationFeedback and invalidCallLabel") {
  forEach("feedback", [](const json::object& vector) {
    const std::string tool = text(vector.at("tool"));
    const feedback::DetailedValidationResult result = feedback::validateToolArgumentsDetailed(tool, vector.at("args"));
    json::array issues;
    for (const feedback::ToolArgumentIssue& issue : result.issues) {
      issues.push_back(json::object{{"code", issue.code}, {"location", issue.location}, {"message", issue.message}, {"hint", issue.hint}});
    }
    const json::value actual = json::object{
      {"result", json::object{{"valid", result.valid}, {"arguments", result.arguments}, {"issues", std::move(issues)}}},
      {"rendered", feedback::renderValidationFeedback(tool, result)},
      {"label", feedback::invalidCallLabel(result)},
    };
    expectSame(actual, vector.at("output"), vector.at("args"));
  });
}

TEST("differential: token estimates and tool-result lines") {
  forEach("estimateTokens", [](const json::object& vector) {
    expectSame(rt::makeNumber(context::estimateTokens(text(vector.at("input")))), vector.at("output"), vector.at("input"));
  });
  forEach("parseToolResult", [](const json::object& vector) {
    expectSame(parsedJson(context::parseToolResultContent(text(vector.at("input")))), vector.at("output"), vector.at("input"));
  });
  forEach("conversationTokens", [](const json::object& vector) {
    expectSame(rt::makeNumber(context::estimateConversationTokens(conversationFromJson(vector.at("input")))), vector.at("output"), json::value("conversation"));
  });
  forEach("requestTokens", [](const json::object& vector) {
    expectSame(rt::makeNumber(context::estimateRequestTokens(vector.at("input").as_object())), vector.at("output"), vector.at("input"));
  });
}

TEST("differential: extractPairs and compactTranscript") {
  forEach("extractPairs", [](const json::object& vector) {
    json::array pairs;
    for (const context::ConversationPair& pair : context::extractPairs(conversationFromJson(vector.at("input")))) {
      pairs.push_back(json::object{
        {"assistantIndex", pair.assistantIndex},
        {"resultIndex", pair.resultIndex},
        {"callId", pair.callId},
        {"tool", pair.tool},
        {"args", pair.args},
        {"parsed", parsedJson(pair.parsed)},
      });
    }
    expectSame(pairs, vector.at("output"), json::value("pairs"));
  });
  forEach("compact", [](const json::object& vector) {
    const json::object& input = vector.at("input").as_object();
    context::CompactOptions options{.budget = *context::parseContextBudgetKind(text(input.at("budget")))};
    if (const json::object* policy = rt::getObject(input, "policy")) {
      options.policy = context::CompactionPolicy{
        .kind = *context::parseContextBudgetKind(text(policy->at("kind"))),
        .targetTokens = *rt::getNumber(*policy, "targetTokens"),
        .readsPerPath = *rt::getNumber(*policy, "readsPerPath"),
        .keepRecentPairs = *rt::getNumber(*policy, "keepRecentPairs"),
        .dropOldExplore = policy->at("dropOldExplore").as_bool(),
        .analysisWindow = *rt::getNumber(*policy, "analysisWindow"),
      };
    }
    expectSame(compactionJson(context::compactTranscript(conversationFromJson(input.at("turns")), options)), vector.at("output"), input.at("budget"));
  });
}

TEST("differential: renderStructuredContext") {
  forEach("structured", [](const json::object& vector) {
    const json::object& input = vector.at("input").as_object();
    std::vector<state::ToolObservation> observations;
    for (const json::value& entry : input.at("observations").as_array()) {
      const json::object& result = entry.at("result").as_object();
      observations.push_back(state::ToolObservation{
        .seq = *rt::getNumber(entry, "seq"),
        .tool = text(entry.at("tool")),
        .args = entry.at("args").as_object(),
        .valid = entry.at("valid").as_bool(),
        .result = loops::ResultLike{.ok = result.at("ok").as_bool(), .output = text(result.at("output"))},
      });
    }
    const state::StructuredTaskState taskState =
      state::deriveStateWithMeta(state::ReliabilityRun{.observations = observations, .finals = {}, .modelCalls = 1}, state::replayMeta(observations));
    const conversation::Conversation conv = conversationFromJson(input.at("turns"));
    const json::value& maxTailTurns = input.at("maxTailTurns");
    const std::string rendered = maxTailTurns.is_null() ? context::renderStructuredContext(taskState, conv)
                                                        : context::renderStructuredContext(taskState, conv, context::StructuredContextOptions{.maxTailTurns = rt::getNumber(maxTailTurns)});
    expectSame(json::value(rendered), vector.at("output"), maxTailTurns);
  });
}

TEST("differential: buildBootstrapClassifierRequest and parseBootstrapClassifierVerdict") {
  forEach("classifierRequest", [](const json::object& vector) {
    const json::object& input = vector.at("input").as_object();
    const json::object& observation = input.at("observation").as_object();
    const classifier::BootstrapClassifierRequestOptions options{
      .observation =
        {
          .runId = text(observation.at("runId")),
          .generation = rt::getNumber(observation, "generation"),
          .phase = optionalText(observation, "phase"),
          .milestone = optionalText(observation, "milestone"),
          .failureFingerprint = optionalText(observation, "failureFingerprint"),
          .gitSha = optionalText(observation, "gitSha"),
          .ledgerVersion = rt::getNumber(observation, "ledgerVersion"),
          .retryState = optionalText(observation, "retryState"),
          .verificationEvidence = optionalText(observation, "verificationEvidence"),
        },
      .decisionDefinition = text(input.at("decisionDefinition")),
      .reasoningEffort = text(input.at("reasoningEffort")),
      .maxCompletionTokens = rt::getNumber(input, "maxCompletionTokens"),
    };
    // Byte-exact: the user message is JSON.stringify of the observation in its key order.
    EXPECT_EQ(rt::jsonStringify(json::value(classifier::buildBootstrapClassifierRequest(options))), rt::jsonStringify(vector.at("output")));
  });
  forEach("verdict", [](const json::object& vector) {
    const classifier::BootstrapClassifierVerdict verdict = classifier::parseBootstrapClassifierVerdict(vector.at("input"));
    json::value actual;
    if (const auto* unknown = std::get_if<classifier::BootstrapClassifierVerdictUnknown>(&verdict)) {
      actual = json::object{{"verdict", unknown->verdict}, {"raw", orNull(unknown->raw)}, {"reason", unknown->reason}};
    } else if (const auto* yes = std::get_if<classifier::BootstrapClassifierVerdictTrue>(&verdict)) {
      actual = json::object{{"verdict", yes->verdict}, {"raw", yes->raw}};
    } else {
      const auto& no = std::get<classifier::BootstrapClassifierVerdictFalse>(verdict);
      actual = json::object{{"verdict", no.verdict}, {"raw", no.raw}};
    }
    expectSame(actual, vector.at("output"), vector.at("input"));
  });
}
