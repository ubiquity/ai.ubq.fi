// Port of tests/harmony-parse.test.ts.
// Links: tests/harmony_parse_test.cpp src/harmony/parse.cpp src/harmony/types.cpp
#include "uos/harmony/parse.hpp"

#include "uos/harmony/types.hpp"
#include "uos/rt/test.hpp"

#include <string>
#include <variant>
#include <vector>

namespace {
  using namespace uos::harmony::parse;
  using namespace uos::harmony::types;

  std::string_view kindOf(const HarmonyTurn& turn) {
    return std::visit([](const auto& value) -> std::string_view { return value.kind; }, turn);
  }
}

TEST("parses the documented analysis + final example") {
  const HarmonyParseResult parsed = parseHarmonyOutput(
    "<|channel|>analysis<|message|>User asks: \"What is 2 + 2?\" Simple arithmetic. Provide answer.<|end|>\n"
    "<|start|>assistant<|channel|>final<|message|>2 + 2 = 4.<|return|>");
  EXPECT_EQ(parsed.truncated, false);
  EXPECT(parsed.turns == (std::vector<HarmonyTurn>{
                           HarmonyTurnReasoning{.text = "User asks: \"What is 2 + 2?\" Simple arithmetic. Provide answer."},
                           HarmonyTurnFinal{.text = "2 + 2 = 4."},
                         }));
}

TEST("parses the documented function-call example (recipient in the channel section)") {
  const HarmonyParseResult parsed = parseHarmonyOutput(
    "<|channel|>analysis<|message|>Need to use function get_weather.<|end|>"
    "<|start|>assistant<|channel|>commentary to=functions.get_weather <|constrain|>json<|message|>"
    "{\"location\":\"San Francisco\"}<|call|>");
  EXPECT(parsed.turns == (std::vector<HarmonyTurn>{
                           HarmonyTurnReasoning{.text = "Need to use function get_weather."},
                           HarmonyTurnToolCall{
                             .recipient = "functions.get_weather",
                             .name = "get_weather",
                             .arguments = "{\"location\":\"San Francisco\"}",
                           },
                         }));
}

TEST("parses the recipient in the role section (alternative header order)") {
  const HarmonyParseResult parsed = parseHarmonyOutput("<|start|>assistant to=functions.get_weather <|constrain|>json<|message|>{\"location\":\"SF\"}<|call|>");
  EXPECT_EQ(parsed.messages.size(), std::size_t{1});
  ASSERT_EQ(parsed.turns.size(), std::size_t{1});
  EXPECT(parsed.turns[0] == HarmonyTurn(HarmonyTurnToolCall{
                              .recipient = "functions.get_weather",
                              .name = "get_weather",
                              .arguments = "{\"location\":\"SF\"}",
                            }));
}

TEST("parses built-in tool recipients on the analysis channel") {
  const HarmonyParseResult parsed = parseHarmonyOutput("<|start|>assistant<|channel|>analysis to=browser.search<|message|>{\"query\":\"Harmony\"}<|call|>");
  EXPECT(parsed.turns == (std::vector<HarmonyTurn>{
                           HarmonyTurnToolCall{
                             .recipient = "browser.search",
                             .name = "browser.search",
                             .arguments = "{\"query\":\"Harmony\"}",
                           },
                         }));
}

TEST("parses commentary preambles followed by a call in one emission") {
  const HarmonyParseResult parsed = parseHarmonyOutput(
    "<|channel|>analysis<|message|>long chain of thought<|end|>"
    "<|start|>assistant<|channel|>commentary<|message|>**Action plan**:<|end|>"
    "<|start|>assistant<|channel|>commentary to=functions.generate_file <|constrain|>json<|message|>"
    "{\"path\":\"index.html\"}<|call|>");
  std::vector<std::string_view> kinds;
  for (const HarmonyTurn& turn : parsed.turns) kinds.push_back(kindOf(turn));
  EXPECT_EQ(kinds, (std::vector<std::string_view>{"reasoning", "commentary", "tool_call"}));
  const auto* call = parsed.turns.size() > 2 ? std::get_if<HarmonyTurnToolCall>(&parsed.turns[2]) : nullptr;
  EXPECT_EQ(call ? std::optional<std::string>(call->name) : std::nullopt, std::optional<std::string>("generate_file"));
}

TEST("marks truncated tails and keeps partial content") {
  const HarmonyParseResult parsed = parseHarmonyOutput("<|channel|>analysis<|message|>Need to use function");
  EXPECT_EQ(parsed.truncated, true);
  EXPECT(parsed.turns == (std::vector<HarmonyTurn>{HarmonyTurnReasoning{.text = "Need to use function"}}));
}

TEST("plain text without Harmony markers yields no turns") {
  const HarmonyParseResult parsed = parseHarmonyOutput("2 + 2 = 4.");
  EXPECT_EQ(parsed.truncated, true);
  EXPECT(parsed.turns.empty());
  EXPECT_EQ(parsed.messages.size(), std::size_t{1});
}

TEST("tool-result messages are retained as raw messages but never become assistant turns") {
  const HarmonyParseResult parsed = parseHarmonyOutput("<|start|>functions.get_weather to=assistant<|channel|>commentary<|message|>{\"sunny\": true}<|end|>");
  ASSERT_EQ(parsed.messages.size(), std::size_t{1});
  EXPECT_EQ(parsed.messages[0].role, "functions.get_weather");
  EXPECT(parsed.turns.empty());
}

TEST("harmonyTurnsFromMessages ignores non-assistant messages") {
  const std::vector<HarmonyTurn> turns = harmonyTurnsFromMessages({
    HarmonyRawMessage{
      .role = "functions.get_weather",
      .channel = "commentary",
      .recipient = "assistant",
      .constrain = std::nullopt,
      .content = "{\"sunny\": true}",
      .stoppedBy = "<|end|>",
    },
    HarmonyRawMessage{
      .role = "assistant",
      .channel = "final",
      .recipient = std::nullopt,
      .constrain = std::nullopt,
      .content = "The weather is sunny.",
      .stoppedBy = "<|return|>",
    },
  });
  EXPECT(turns == (std::vector<HarmonyTurn>{HarmonyTurnFinal{.text = "The weather is sunny."}}));
}

TEST("functionNameFromRecipient strips only the functions. namespace") {
  EXPECT_EQ(functionNameFromRecipient("functions.get_weather"), "get_weather");
  EXPECT_EQ(functionNameFromRecipient("browser.search"), "browser.search");
  EXPECT_EQ(functionNameFromRecipient("python"), "python");
}

TEST("normalizeToolArguments compacts valid JSON and keeps invalid JSON verbatim") {
  EXPECT_EQ(normalizeToolArguments("{\"location\": \"SF\", \"format\": \"celsius\"}"), "{\"location\":\"SF\",\"format\":\"celsius\"}");
  EXPECT_EQ(normalizeToolArguments("{not json"), "{not json");
  EXPECT_EQ(normalizeToolArguments(""), "");
}
