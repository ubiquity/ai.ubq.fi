// Port of tests/harmony-render.test.ts, including the adapter normalization case.
// Links: tests/harmony_render_test.cpp src/harmony/render.cpp src/harmony/types.cpp src/harmony/adapter.cpp
// src/harmony/conversation.cpp src/harmony/parse.cpp src/provider/cerebras.cpp src/sentinel/upstream_capture.cpp src/utils.cpp
#include "uos/harmony/adapter.hpp"
#include "uos/harmony/probes.hpp"
#include "uos/harmony/render.hpp"

#include "uos/harmony/types.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"

#include <string>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::harmony::render;
  using uos::harmony::types::NativeResponseFormat;
  using uos::harmony::types::ToolDefinition;

  using uos::harmony::probes::WEATHER_TOOL;
  using uos::harmony::probes::ANSWER_SCHEMA;

  bool matches(const std::string& text, std::string_view pattern) { return rt::Regex(pattern, "").test(text); }
}

TEST("system message renders identity, dates, reasoning effort, channels and the tool note") {
  const std::string rendered = renderSystemMessage(SystemRenderOptions{.currentDate = "2025-06-28", .reasoningEffort = "high"});
  EXPECT(matches(rendered, "^You are ChatGPT, a large language model trained by OpenAI\\."));
  EXPECT(matches(rendered, "Knowledge cutoff: 2024-06"));
  EXPECT(matches(rendered, "Current date: 2025-06-28"));
  EXPECT(matches(rendered, "Reasoning: high"));
  EXPECT(matches(rendered, "# Valid channels: analysis, commentary, final\\. Channel must be included for every message\\."));
  EXPECT(matches(rendered, "Calls to these tools must go to the commentary channel: 'functions'\\."));
}

TEST("system message omits the function-call note when no tools are exposed") {
  const std::string rendered = renderSystemMessage(SystemRenderOptions{.currentDate = "2025-06-28", .reasoningEffort = "low", .toolNamespace = nullptr});
  EXPECT(matches(rendered, "Reasoning: low"));
  EXPECT(!matches(rendered, "must go to the commentary channel"));
}

TEST("developer message renders Harmony TypeScript tool definitions") {
  const std::string rendered = renderDeveloperMessage(DeveloperRenderOptions{.instructions = "You are a weather assistant.", .tools = std::vector<ToolDefinition>{WEATHER_TOOL}});
  EXPECT(matches(rendered, "# Instructions\\n\\nYou are a weather assistant\\."));
  EXPECT(matches(rendered, "# Tools\\n\\n## functions\\n\\nnamespace functions \\{"));
  EXPECT(matches(rendered, "\\/\\/ Gets the current weather in the provided location\\."));
  EXPECT(matches(rendered, "type get_weather = \\(_: \\{"));
  EXPECT(matches(rendered, "\\/\\/ The city and state, e\\.g\\. San Francisco, CA"));
  EXPECT(matches(rendered, "location: string,"));
  EXPECT(matches(rendered, "\\} \\/\\/ namespace functions"));
}

TEST("developer message renders optional fields, enums and defaults") {
  const std::string rendered = renderDeveloperMessage(DeveloperRenderOptions{
    .instructions = "Use tools.",
    .tools =
      std::vector<ToolDefinition>{
        ToolDefinition{
          .name = "get_current_weather",
          .description = "Gets the current weather in the provided location.",
          .parameters =
            json::object{
              {"type", "object"},
              {"properties",
               json::object{
                 {"location", json::object{{"type", "string"}, {"description", "The city and state, e.g. San Francisco, CA"}}},
                 {"format", json::object{{"type", "string"}, {"enum", json::array{"celsius", "fahrenheit"}}, {"default", "celsius"}}},
               }},
              {"required", json::array{"location"}},
            },
        },
      },
  });
  EXPECT(matches(rendered, "location: string,"));
  EXPECT(matches(rendered, "format\\?: \"celsius\" \\| \"fahrenheit\", \\/\\/ default: \"celsius\""));
}

TEST("developer message renders a response format schema section") {
  const std::string rendered = renderDeveloperMessage(DeveloperRenderOptions{
    .instructions = "Return JSON.",
    .responseFormat = NativeResponseFormat{.formatName = "answer_response", .description = "The computed answer", .schema = ANSWER_SCHEMA},
  });
  EXPECT(matches(rendered, "# Response Formats"));
  EXPECT(matches(rendered, "## answer_response"));
  EXPECT(matches(rendered, "\\/\\/ The computed answer"));
  EXPECT(matches(rendered, rt::slice(rt::jsonStringify(json::value(ANSWER_SCHEMA)), 0, 24)));
}

TEST("harmonyTypeFromJsonSchema covers enums, arrays, anyOf and nested objects") {
  EXPECT_EQ(harmonyTypeFromJsonSchema(json::value(json::object{{"type", "integer"}})), "integer");
  EXPECT_EQ(harmonyTypeFromJsonSchema(json::value(json::object{{"type", "string"}, {"enum", json::array{"a", "b"}}})), "\"a\" | \"b\"");
  EXPECT_EQ(harmonyTypeFromJsonSchema(json::value(json::object{{"type", "array"}, {"items", json::object{{"type", "string"}}}})), "string[]");
  EXPECT_EQ(harmonyTypeFromJsonSchema(json::value(json::object{{"anyOf", json::array{json::object{{"type", "string"}}, json::object{{"type", "integer"}}}}})),
            "string | integer");
  const std::string nested = harmonyTypeFromJsonSchema(json::value(json::object{
    {"type", "object"},
    {"properties", json::object{{"inner", json::object{{"type", "object"}, {"properties", json::object{{"value", json::object{{"type", "boolean"}}}}}, {"required", json::array{"value"}}}}}},
    {"required", json::array{"inner"}},
  }));
  EXPECT_EQ(nested, "{\n  inner: {\n    value: boolean,\n  },\n}");
}

TEST("unknown schema constructs degrade to any, never crash") {
  EXPECT_EQ(harmonyTypeFromJsonSchema(json::value("not a schema")), "any");
  EXPECT_EQ(harmonyTypeFromJsonSchema(json::value(json::object{{"type", "object"}})), "any");
  EXPECT_EQ(harmonyTypeFromJsonSchema(json::value(json::object{{"type", "object"}, {"properties", json::object{}}})), "{}");
  EXPECT_EQ(harmonyTypeFromJsonSchema(json::value(nullptr)), "any");
}

TEST("quoteString escapes Harmony TS string literals") {
  EXPECT_EQ(quoteString("say \"hi\""), "\"say \\\"hi\\\"\"");
  EXPECT_EQ(quoteString("a\\b"), "\"a\\\\b\"");
}

TEST("normalizeToolStrictness forces one strictness value on every tool") {
  auto weather = WEATHER_TOOL;
  weather.strict = true;
  const std::vector<ToolDefinition> mixed{weather, uos::harmony::probes::NOTE_TOOL};
  const auto allFalse = uos::harmony::adapter::normalizeToolStrictness(mixed, false);
  EXPECT_EQ(allFalse.size(), std::size_t{2});
  for (const auto& tool : allFalse) EXPECT(tool.strict == false);
  const auto allTrue = uos::harmony::adapter::normalizeToolStrictness(mixed, true);
  for (const auto& tool : allTrue) EXPECT(tool.strict == true);
}
