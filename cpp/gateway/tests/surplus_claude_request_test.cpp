// Links: tests/surplus_claude_request_test.cpp src/provider/surplus_claude_request.cpp src/provider/anthropic_replay.cpp
// Port of tests/surplus-claude-request.test.ts, plus differential vectors from the TS module under bun
// (tests/surplus_claude_request_vectors.inc) that pin the exact JSON.stringify of the native body.
#include "uos/provider/surplus_claude_request.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace uos;
using provider::surplus_claude_request::buildSurplusClaudeRequest;
using provider::surplus_claude_request::SurplusClaudeRequestOptions;

namespace {
  json::object parse(std::string_view text) { return rt::jsonParse(text).as_object(); }

  json::object base() { return parse(R"({"model":"claude-opus-5.5","input":"hello","max_output_tokens":128,"stream":true,"store":false})"); }

  // `{ ...base(), ...overrides }`
  json::object withBase(std::string_view overrides) { return rt::objectSpread(base(), parse(overrides)); }

  void expectJson(const json::value& actual, std::string_view expected, const char* text, const char* file, int line) {
    const json::value wanted = rt::jsonParse(expected);
    if (!rt::deepEqual(actual, wanted))
      rt::test::report(file, line, std::string("expected ") + text + " to deep-equal " + json::serialize(wanted) + "\n      actual:   " + json::serialize(actual),
                       false);
  }
#define EXPECT_JSON(actual, expected) expectJson((actual), (expected), #actual, __FILE__, __LINE__)

  const json::value& member(const json::object& object, std::string_view key) {
    static const json::value missing;
    const json::value* found = rt::get(object, key);
    return found ? *found : missing;
  }

  struct Vector {
    std::string_view name;
    std::string_view body;
    std::optional<double> catalog;
    std::string_view options;
    std::optional<std::string> result;
  };

  const std::vector<Vector>& vectors() {
    static const std::vector<Vector> list{
#include "tests/surplus_claude_request_vectors.inc"
    };
    return list;
  }

  SurplusClaudeRequestOptions optionsFrom(std::string_view text) {
    const json::object object = parse(text);
    SurplusClaudeRequestOptions options;
    if (auto value = rt::getString(object, "provider")) options.provider = std::string(*value);
    if (auto value = rt::getString(object, "reasoningReplayScope")) options.reasoningReplayScope = std::string(*value);
    if (auto value = rt::getBool(object, "supportsNativeReasoningEffort")) options.supportsNativeReasoningEffort = *value;
    if (auto value = rt::getBool(object, "supportsNativeDisabledThinking")) options.supportsNativeDisabledThinking = *value;
    if (auto value = rt::getBool(object, "omitHostedWebSearch")) options.omitHostedWebSearch = *value;
    if (const json::array* efforts = rt::getArray(object, "nativeReasoningEfforts")) {
      std::vector<std::string> list;
      for (const json::value& effort : *efforts) list.emplace_back(effort.get_string());
      options.nativeReasoningEfforts = std::move(list);
    }
    return options;
  }
}

TEST("Surplus Claude native request keeps instructions and caller input unchanged") {
  const json::object body = withBase(
    R"({"instructions":"global instructions","input":[{"type":"message","role":"developer","content":[{"type":"input_text","text":"developer instructions"}]},{"type":"message","role":"user","content":[{"type":"input_text","text":"question"}]}],"prompt_cache_key":"caller-affinity-key"})");
  const std::string before = rt::jsonStringify(json::value(body));
  const auto result = buildSurplusClaudeRequest(body);
  ASSERT(result);
  EXPECT_JSON(json::value(result->body), R"({
    "model": "claude-opus-5.5",
    "max_tokens": 128,
    "messages": [{ "role": "user", "content": [{ "type": "text", "text": "question", "cache_control": { "type": "ephemeral" } }] }],
    "stream": true,
    "system": [
      { "type": "text", "text": "global instructions" },
      { "type": "text", "text": "developer instructions" }
    ],
    "prompt_cache_key": "caller-affinity-key"
  })");
  EXPECT_EQ(result->promptCacheMode, std::string("implicit"));
  EXPECT_EQ(result->explicitBreakpointCount, 0.0);
  EXPECT_EQ(rt::jsonStringify(json::value(body)), before);
}

TEST("Surplus Claude replay preserves function names, call IDs, arguments, result and order") {
  const auto result = buildSurplusClaudeRequest(withBase(R"({
    "input": [
      { "type": "message", "role": "user", "content": "Read the file" },
      { "type": "message", "role": "assistant", "content": [{ "type": "output_text", "text": "Reading" }] },
      { "type": "function_call", "call_id": "call-1", "name": "read_file", "arguments": "{\"path\":\"a.txt\",\"lines\":3}" },
      { "type": "function_call_output", "call_id": "call-1", "output": "file contents" }
    ],
    "tools": [{ "type": "function", "name": "read_file", "description": "Read a file", "parameters": { "type": "object", "properties": { "path": { "type": "string" } } } }],
    "parallel_tool_calls": false
  })"));
  ASSERT(result);
  EXPECT_JSON(member(result->body, "tools"),
              R"([{ "name": "read_file", "description": "Read a file", "input_schema": { "type": "object", "properties": { "path": { "type": "string" } } } }])");
  EXPECT_JSON(member(result->body, "messages"), R"([
    { "role": "user", "content": [{ "type": "text", "text": "Read the file" }] },
    {
      "role": "assistant",
      "content": [
        { "type": "text", "text": "Reading" },
        { "type": "tool_use", "id": "call-1", "name": "read_file", "input": { "path": "a.txt", "lines": 3 } }
      ]
    },
    { "role": "user", "content": [{ "type": "tool_result", "tool_use_id": "call-1", "content": "file contents", "cache_control": { "type": "ephemeral" } }] }
  ])");
  EXPECT_JSON(member(result->body, "tool_choice"), R"({ "type": "auto", "disable_parallel_tool_use": true })");
}

TEST("Surplus Claude flattens namespace tools and omits disabled hosted web search") {
  const auto result = buildSurplusClaudeRequest(withBase(R"({
    "input": [
      { "role": "user", "content": "What time is it?" },
      { "type": "function_call", "name": "curr_time", "namespace": "clock", "call_id": "call-1", "arguments": "{}" },
      { "type": "function_call_output", "call_id": "call-1", "output": "2026-11-04T22:00:00Z" }
    ],
    "tools": [
      {
        "type": "namespace",
        "name": "clock",
        "description": "Tools for reading time.",
        "tools": [
          { "type": "function", "name": "curr_time", "description": "Current time", "parameters": { "type": "object", "properties": {}, "additionalProperties": false } }
        ]
      },
      { "type": "function", "name": "read_file", "parameters": { "type": "object", "properties": { "path": { "type": "string" } } } },
      { "type": "web_search", "external_web_access": false, "user_location": { "type": "approximate", "country": "US" } }
    ],
    "tool_choice": { "type": "function", "name": "curr_time", "namespace": "clock" },
    "parallel_tool_calls": false
  })"));
  ASSERT(result);
  std::vector<std::string> names;
  for (const json::value& tool : member(result->body, "tools").as_array()) names.emplace_back(tool.at("name").as_string());
  EXPECT_EQ(names, (std::vector<std::string>{"clock_curr_time", "read_file"}));
  const auto* clock = result->toolNames.get("clock_curr_time");
  ASSERT(clock);
  EXPECT_EQ(clock->name, std::string("curr_time"));
  EXPECT_EQ(clock->namespace_, std::optional<std::string>("clock"));
  const auto* readFile = result->toolNames.get("read_file");
  ASSERT(readFile);
  EXPECT_EQ(readFile->name, std::string("read_file"));
  EXPECT_EQ(readFile->namespace_, std::optional<std::string>());
  EXPECT_JSON(member(result->body, "tool_choice"), R"({ "type": "tool", "name": "clock_curr_time", "disable_parallel_tool_use": true })");
  const json::array& messages = member(result->body, "messages").as_array();
  EXPECT_EQ(std::string(messages.at(1).at("content").at(0).at("name").as_string()), std::string("clock_curr_time"));
}

TEST("Surplus Claude preserves late developer messages in the native system channel") {
  const auto result = buildSurplusClaudeRequest(withBase(R"({
    "input": [
      { "role": "developer", "content": "first instruction" },
      { "role": "user", "content": "question" },
      { "role": "developer", "content": "later reminder" }
    ]
  })"));
  ASSERT(result);
  EXPECT_JSON(member(result->body, "system"), R"([
    { "type": "text", "text": "first instruction" },
    { "type": "text", "text": "later reminder" }
  ])");
  EXPECT_JSON(member(result->body, "messages"), R"([{ "role": "user", "content": [{ "type": "text", "text": "question", "cache_control": { "type": "ephemeral" } }] }])");
}

TEST("Surplus Claude images retain base64 media type and remote URL without fetching") {
  const auto result = buildSurplusClaudeRequest(withBase(R"({
    "input": [
      {
        "role": "user",
        "content": [
          { "type": "input_image", "image_url": "data:image/png;base64,aGVsbG8=" },
          { "type": "input_image", "image_url": "https://example.test/a.png" }
        ]
      }
    ]
  })"));
  ASSERT(result);
  EXPECT_JSON(member(result->body, "messages"), R"([
    {
      "role": "user",
      "content": [
        { "type": "image", "source": { "type": "base64", "media_type": "image/png", "data": "aGVsbG8=" } },
        { "type": "image", "source": { "type": "url", "url": "https://example.test/a.png" }, "cache_control": { "type": "ephemeral" } }
      ]
    }
  ])");
}

TEST("Surplus Claude explicit block policies suppress automatic markers and preserve lifetime") {
  const auto result = buildSurplusClaudeRequest(withBase(R"({
    "input": [
      { "role": "user", "content": [{ "type": "input_text", "text": "stable", "cache_control": { "type": "ephemeral", "ttl": "1h" } }] },
      { "role": "user", "content": [{ "type": "input_text", "text": "changing" }] }
    ]
  })"));
  ASSERT(result);
  EXPECT_EQ(result->promptCacheMode, std::string("explicit"));
  EXPECT_EQ(result->explicitBreakpointCount, 1.0);
  EXPECT_JSON(member(result->body, "messages"), R"([
    {
      "role": "user",
      "content": [
        { "type": "text", "text": "stable", "cache_control": { "type": "ephemeral", "ttl": "1h" } },
        { "type": "text", "text": "changing" }
      ]
    }
  ])");
}

TEST("Surplus Claude translates an explicit Responses breakpoint in a tool result") {
  const auto result = buildSurplusClaudeRequest(withBase(R"({
    "input": [
      { "role": "user", "content": "run" },
      { "type": "function_call", "name": "run", "call_id": "c", "arguments": "{}" },
      { "type": "function_call_output", "call_id": "c", "output": [{ "type": "input_text", "text": "result", "prompt_cache_breakpoint": { "mode": "explicit" } }] }
    ]
  })"));
  ASSERT(result);
  EXPECT_EQ(result->promptCacheMode, std::string("explicit"));
  EXPECT_EQ(result->explicitBreakpointCount, 1.0);
  const json::array& messages = member(result->body, "messages").as_array();
  EXPECT_JSON(messages.at(2).at("content"),
              R"([{ "type": "tool_result", "tool_use_id": "c", "content": [{ "type": "text", "text": "result", "cache_control": { "type": "ephemeral" } }] }])");
}

TEST("Surplus Claude keeps explicit cache breakpoints on tool call and result blocks") {
  const auto result = buildSurplusClaudeRequest(withBase(R"({
    "input": [
      { "role": "user", "content": "run" },
      { "type": "function_call", "name": "run", "call_id": "c", "arguments": "{}", "prompt_cache_breakpoint": { "mode": "explicit" } },
      { "type": "function_call_output", "call_id": "c", "output": "result", "cache_control": { "type": "ephemeral", "ttl": "1h" } }
    ]
  })"));
  ASSERT(result);
  EXPECT_EQ(result->promptCacheMode, std::string("explicit"));
  EXPECT_EQ(result->explicitBreakpointCount, 2.0);
  const json::array& messages = member(result->body, "messages").as_array();
  EXPECT_JSON(messages.at(1).at("content").at(0).at("cache_control"), R"({ "type": "ephemeral" })");
  EXPECT_JSON(messages.at(2).at("content").at(0).at("cache_control"), R"({ "type": "ephemeral", "ttl": "1h" })");
}

TEST("Surplus Claude explicit mode with no breakpoints stays without automatic caching") {
  const auto result = buildSurplusClaudeRequest(withBase(R"({ "prompt_cache_options": { "mode": "explicit" } })"));
  ASSERT(result);
  EXPECT_EQ(result->promptCacheMode, std::string("explicit"));
  EXPECT_JSON(member(result->body, "messages"), R"([{ "role": "user", "content": [{ "type": "text", "text": "hello" }] }])");
}

TEST("Surplus Claude reasoning preserves requested effort and the Codex ultra translation") {
  EXPECT(!buildSurplusClaudeRequest(withBase(R"({ "reasoning": { "effort": "max" } })")));
  const std::vector<std::pair<std::string, std::string>> pairs{
    {"low", "low"}, {"medium", "medium"}, {"high", "high"}, {"xhigh", "xhigh"}, {"max", "max"}, {"ultra", "max"},
  };
  for (const auto& [requested, expected] : pairs) {
    json::object body = base();
    json::object reasoning;
    reasoning["effort"] = requested;
    reasoning["summary"] = "none";
    body["reasoning"] = std::move(reasoning);
    const auto result = buildSurplusClaudeRequest(body, std::nullopt, SurplusClaudeRequestOptions{.supportsNativeReasoningEffort = true});
    ASSERT(result);
    EXPECT_JSON(member(result->body, "thinking"), R"({ "type": "adaptive" })");
    json::object outputConfig;
    outputConfig["effort"] = expected;
    EXPECT(rt::deepEqual(member(result->body, "output_config"), json::value(outputConfig)));
  }
}

TEST("Surplus Claude carries JSON-schema structured output with adaptive reasoning") {
  const auto result = buildSurplusClaudeRequest(withBase(R"({
      "reasoning": { "effort": "high", "summary": "concise" },
      "text": { "format": { "type": "json_schema", "strict": true, "schema": {
        "type": "object",
        "properties": { "title": { "type": "string", "minLength": 1, "maxLength": 36 } },
        "required": ["title"],
        "additionalProperties": false
      }, "name": "codex_output_schema" } }
    })"),
                                                std::nullopt, SurplusClaudeRequestOptions{.supportsNativeReasoningEffort = true});
  ASSERT(result);
  EXPECT_JSON(member(result->body, "output_config"), R"({
    "effort": "high",
    "format": { "type": "json_schema", "schema": {
      "type": "object",
      "properties": { "title": { "type": "string", "minLength": 1, "maxLength": 36 } },
      "required": ["title"],
      "additionalProperties": false
    } }
  })");
}

TEST("Surplus Claude accepts the Codex concise reasoning summary preference") {
  const auto result = buildSurplusClaudeRequest(withBase(R"({ "reasoning": { "effort": "high", "summary": "concise" } })"), std::nullopt,
                                                SurplusClaudeRequestOptions{.supportsNativeReasoningEffort = true});
  ASSERT(result);
  EXPECT_JSON(member(result->body, "thinking"), R"({ "type": "adaptive" })");
  EXPECT_JSON(member(result->body, "output_config"), R"({ "effort": "high" })");
}

TEST("native none requires independent disabled-thinking proof, never an effort or model-name guess") {
  const json::object body = withBase(R"({ "reasoning": { "effort": "none", "summary": "none" } })");
  const std::string before = rt::jsonStringify(json::value(body));
  EXPECT(!buildSurplusClaudeRequest(body));
  EXPECT(!buildSurplusClaudeRequest(body, std::nullopt, SurplusClaudeRequestOptions{.supportsNativeReasoningEffort = true}));
  EXPECT(!buildSurplusClaudeRequest(body, std::nullopt, SurplusClaudeRequestOptions{.supportsNativeDisabledThinking = false}));
  const auto result = buildSurplusClaudeRequest(
    body, std::nullopt, SurplusClaudeRequestOptions{.supportsNativeReasoningEffort = false, .supportsNativeDisabledThinking = true});
  ASSERT(result);
  EXPECT_JSON(member(result->body, "thinking"), R"({ "type": "disabled" })");
  EXPECT(!rt::get(result->body, "output_config"));
  EXPECT_EQ(rt::jsonStringify(json::value(body)), before);
  EXPECT(!buildSurplusClaudeRequest(withBase(R"({ "reasoning": { "effort": "high" } })"), std::nullopt,
                                    SurplusClaudeRequestOptions{.supportsNativeDisabledThinking = true}));
}

TEST("Surplus Claude has no invented output cap when the caller omitted one") {
  const json::object body = withBase(R"({ "max_output_tokens": null })");
  EXPECT(!buildSurplusClaudeRequest(body));
  const auto capped = buildSurplusClaudeRequest(body, 128000.0);
  ASSERT(capped);
  EXPECT_EQ(rt::getNumber(capped->body, "max_tokens"), std::optional<double>(128000));
  EXPECT(!buildSurplusClaudeRequest(body, -1.0));
}

TEST("Surplus native adapter returns incompatible inputs intact to the existing Responses route") {
  const std::vector<std::string_view> overrides{
    R"({ "model": "deepseek-v4.1-flash" })",
    R"({ "model": "gpt-6-sol" })",
    R"({ "input": [{ "type": "reasoning", "encrypted_content": "opaque" }] })",
    R"({ "input": [{ "type": "reasoning", "summary": [{ "type": "summary_text", "text": "summary" }] }] })",
    R"({ "input": [{ "type": "function_call", "name": "run", "call_id": "c", "arguments": "not-json" }] })",
    R"({ "tools": [{ "type": "web_search" }] })",
    R"({ "tools": [{ "type": "namespace", "name": "functions", "tools": [] }] })",
    R"({ "tools": [{ "type": "custom", "name": "apply_patch" }] })",
    R"({ "tools": [{ "type": "function", "name": "run", "strict": true }] })",
    R"({ "prompt_cache_retention": "24h" })",
    R"({ "prompt_cache_options": { "ttl": "30m" } })",
    R"({ "prompt_cache_options": { "mode": "disabled" } })",
    R"({ "cache_control": { "type": "unsupported" }, "input": [{ "role": "user", "content": [{ "type": "input_text", "text": "stable", "cache_control": { "type": "ephemeral" } }] }] })",
    R"({ "input": [{ "role": "user", "content": [{ "type": "input_text", "text": "stable", "cache_control": { "type": "ephemeral" }, "prompt_cache_breakpoint": {} }] }] })",
    R"({ "text": { "format": { "type": "json_schema" } } })",
    R"({ "text": { "format": "text" } })",
    R"({ "store": true })",
    R"({ "previous_response_id": "resp_other_provider" })",
    R"({ "input": [{ "role": "user", "content": [{ "type": "input_file", "file_id": "file_other_provider" }] }] })",
    R"({ "input": [{ "role": "user", "content": [{ "type": "input_image", "image_url": "https://example.test/a.png", "detail": "original" }] }] })",
  };
  for (const std::string_view override_ : overrides) {
    const json::object body = withBase(override_);
    const std::string before = rt::jsonStringify(json::value(body));
    if (buildSurplusClaudeRequest(body)) rt::test::report(__FILE__, __LINE__, "expected null for " + before, false);
    EXPECT_EQ(rt::jsonStringify(json::value(body)), before);
  }
}

TEST("Direct Claude Code omits enabled hosted web search while reseller adapters reject it") {
  const json::object body =
    withBase(R"({ "tools": [{ "type": "web_search", "external_web_access": true, "user_location": { "type": "approximate", "country": "US" } }] })");
  const auto direct =
    buildSurplusClaudeRequest(body, std::nullopt, SurplusClaudeRequestOptions{.provider = "claude_code", .omitHostedWebSearch = true});
  ASSERT(direct);
  EXPECT(!rt::get(direct->body, "tools"));
  EXPECT(!buildSurplusClaudeRequest(body));
}

TEST("differential: native bodies match the TS module byte for byte") {
  for (const Vector& vector : vectors()) {
    const json::object body = parse(vector.body);
    const auto result = buildSurplusClaudeRequest(body, vector.catalog, optionsFrom(vector.options));
    std::optional<std::string> actual;
    if (result) {
      json::array toolNames;
      for (const auto& [projected, original] : result->toolNames) {
        json::object name;
        name["name"] = original.name;
        name["namespace"] = original.namespace_ ? json::value(*original.namespace_) : json::value(nullptr);
        toolNames.push_back(json::array{json::value(projected), json::value(std::move(name))});
      }
      json::object summary;
      summary["body"] = result->body;
      summary["promptCacheMode"] = result->promptCacheMode;
      summary["explicitBreakpointCount"] = rt::makeNumber(result->explicitBreakpointCount);
      summary["toolNames"] = std::move(toolNames);
      actual = rt::jsonStringify(json::value(std::move(summary)));
    }
    if (actual != vector.result)
      rt::test::report(__FILE__, __LINE__,
                       std::string(vector.name) + "\n      actual:   " + actual.value_or("null") + "\n      expected: " + vector.result.value_or("null"), false);
  }
}

