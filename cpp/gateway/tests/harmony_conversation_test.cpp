// Port of tests/harmony-conversation.test.ts.
// Links: tests/harmony_conversation_test.cpp src/harmony/conversation.cpp src/harmony/types.cpp
#include "uos/harmony/conversation.hpp"

#include "uos/harmony/types.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"

#include <string>
#include <variant>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::harmony::conversation;
  using namespace uos::harmony::types;

  // `assistantResponse(overrides)`: the base response; the test assigns its overrides.
  NormalizedAssistantResponse assistantResponse() {
    return NormalizedAssistantResponse{
      .turns = {},
      .analysis = {},
      .content = std::nullopt,
      .toolCalls = {},
      .finishReason = std::nullopt,
      .refusal = std::nullopt,
      .shape =
        {
          .contentPresent = false,
          .contentChars = 0,
          .reasoningField = "none",
          .reasoningChars = 0,
          .toolCallsField = false,
          .toolCallCount = 0,
          .finishReason = std::nullopt,
          .refusal = false,
        },
      .id = "cmpl-test",
      .model = "gpt-oss-120b",
      .created = 1,
    };
  }

  NormalizedAssistantResponse withCalls(std::vector<std::string> analysis, std::optional<std::string> content, std::vector<ToolCall> toolCalls) {
    NormalizedAssistantResponse response = assistantResponse();
    response.analysis = std::move(analysis);
    response.content = std::move(content);
    response.toolCalls = std::move(toolCalls);
    return response;
  }

  json::value asJson(const std::vector<json::object>& messages) {
    json::array out;
    for (const json::object& message : messages) out.push_back(message);
    return out;
  }

  std::string_view roleOf(const ConversationTurn& turn) {
    return std::visit([](const auto& value) -> std::string_view { return value.role; }, turn);
  }

  Conversation userWeather() { return createConversation({ConversationTurnUser{.content = "Weather?"}}); }
}

TEST("analysis is preserved across an unfinished tool turn") {
  const Conversation conversation = advanceConversation(
    userWeather(), withCalls({"Need to use function get_weather."}, std::nullopt, {{.id = "call-1", .name = "get_weather", .arguments = "{\"location\":\"SF\"}"}}));
  EXPECT_EQ(analysisLineCount(conversation), 1);
  EXPECT_EQ(pendingToolCallCount(conversation), 1);
  EXPECT_EQ(hasCompletedFinal(conversation), false);
  // The unfinished turn keeps its analysis under the replay policy.
  EXPECT_EQ(analysisLineCount(dropAnalysisBeforeCompletedFinal(conversation)), 1);
}

TEST("analysis is dropped after a completed final answer") {
  const Conversation withTool = advanceConversation(
    userWeather(), withCalls({"Need to use function get_weather."}, std::nullopt, {{.id = "call-1", .name = "get_weather", .arguments = "{\"location\":\"SF\"}"}}));
  const Conversation withResult = appendToolResult(withTool, "call-1", "get_weather", "{\"sunny\": true}");
  const Conversation withAnswer = advanceConversation(withResult, withCalls({"Summarize the weather."}, "It is sunny in San Francisco.", {}));
  EXPECT_EQ(analysisLineCount(withAnswer), 2);
  const Conversation replayed = dropAnalysisBeforeCompletedFinal(withAnswer);
  EXPECT_EQ(analysisLineCount(replayed), 0);
  EXPECT_EQ(hasCompletedFinal(withAnswer), true);
}

TEST("wire view never carries analysis and replays tool calls in OpenAI shape") {
  const Conversation conversation = advanceConversation(
    userWeather(),
    withCalls({"Need to use function get_weather."}, "Let me check the weather.", {{.id = "call-1", .name = "get_weather", .arguments = "{\"location\":\"SF\"}"}}));
  const std::vector<json::object> messages = wireMessagesFromConversation(conversation);
  const json::value expected = json::array{
    json::object{{"role", "user"}, {"content", "Weather?"}},
    json::object{
      {"role", "assistant"},
      {"content", "Let me check the weather."},
      {"tool_calls",
       json::array{json::object{
         {"id", "call-1"},
         {"type", "function"},
         {"function", json::object{{"name", "get_weather"}, {"arguments", "{\"location\":\"SF\"}"}}},
       }}},
    },
  };
  EXPECT(rt::deepEqual(asJson(messages), expected));
  EXPECT(!rt::includes(rt::jsonStringify(asJson(messages)), "Need to use function get_weather."));
}

TEST("wire view renders tool results with tool_call_id and no name field") {
  const Conversation conversation = appendToolResult(userWeather(), "call-1", "get_weather", "{\"sunny\": true}");
  const std::vector<json::object> messages = wireMessagesFromConversation(conversation);
  EXPECT(rt::deepEqual(json::value(messages.at(1)), json::object{{"role", "tool"}, {"tool_call_id", "call-1"}, {"content", "{\"sunny\": true}"}}));
}

TEST("wire view drops empty assistant turns and keeps empty user turns out") {
  const Conversation conversation = createConversation({
    AssistantTurn{.content = std::nullopt, .analysis = {"private"}, .toolCalls = {}, .finishReason = std::nullopt},
    ConversationTurnUser{.content = ""},
    ConversationTurnSystem{.content = "sys"},
  });
  const std::vector<json::object> messages = wireMessagesFromConversation(conversation);
  EXPECT(rt::deepEqual(asJson(messages), json::array{json::object{{"role", "system"}, {"content", "sys"}}}));
}

TEST("appendTurn/appendUser/appendToolResult compose turss immutably") {
  const Conversation base = createConversation();
  const Conversation one = appendUser(base, "hello");
  const Conversation two = appendTurn(one, ConversationTurnSystem{.content = "sys"});
  EXPECT_EQ(base.turns.size(), std::size_t{0});
  EXPECT_EQ(one.turns.size(), std::size_t{1});
  EXPECT_EQ(two.turns.size(), std::size_t{2});
  EXPECT_EQ(roleOf(two.turns[0]), std::string_view("user"));
  EXPECT_EQ(roleOf(two.turns[1]), std::string_view("system"));
}

TEST("pendingToolCallCount counts only calls without a matching result") {
  const Conversation conversation = appendToolResult(
    advanceConversation(appendToolResult(advanceConversation(createConversation({ConversationTurnUser{.content = "go"}}),
                                                             withCalls({}, std::nullopt,
                                                                       {
                                                                         {.id = "call-1", .name = "get_weather", .arguments = "{}"},
                                                                         {.id = "call-2", .name = "save_note", .arguments = "{}"},
                                                                       })),
                                         "call-1", "get_weather", "{}"),
                        withCalls({}, std::nullopt, {{.id = "call-1", .name = "get_weather", .arguments = "{}"}})),
    "call-1", "get_weather", "{}");
  EXPECT_EQ(pendingToolCallCount(conversation), 1);
}
