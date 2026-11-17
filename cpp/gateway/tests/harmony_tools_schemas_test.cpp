// The last three cases are differential (no TS test covers them): values recorded from Bun 1.4.0 for
// tool names and argument keys that a plain-object lookup resolves on Object.prototype.
// Links: tests/harmony_tools_schemas_test.cpp src/harmony/tools/schemas.cpp src/harmony/types.cpp src/harmony/adapter.cpp
// src/harmony/conversation.cpp src/harmony/parse.cpp src/harmony/render.cpp src/provider/cerebras.cpp src/sentinel/upstream_capture.cpp src/utils.cpp
#include "uos/harmony/tools/schemas.hpp"
#include "uos/harmony/adapter.hpp"

#include "uos/harmony/types.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::harmony::tools::schemas;
  using uos::harmony::types::ToolDefinition;

  bool isValid(const ValidateToolArgumentsResult& result) { return std::holds_alternative<ValidateToolArgumentsResultValid>(result); }

  std::string reasonOf(const ValidateToolArgumentsResult& result) {
    const auto* invalid = std::get_if<ValidateToolArgumentsResultInvalid>(&result);
    return invalid ? invalid->reason : std::string();
  }

  std::string typeErrorOf(std::string_view tool, const json::value& args) {
    try {
      (void)validateToolArguments(tool, args);
    } catch (const rt::TypeError& error) {
      return error.toString();
    }
    return "no error";
  }

  // `validExamples`, in the TS object's key order.
  const std::vector<std::pair<std::string, json::object>> validExamples{
    {"filesystem.read", json::object{{"path", "src/main.ts"}}},
    {"filesystem.find", json::object{{"path", "src"}, {"pattern", "**/*.ts"}}},
    {"filesystem.search", json::object{{"path", "src"}, {"query", "TODO"}}},
    {"browser.search", json::object{{"query", "harmony format"}}},
    {"browser.open", json::object{{"url", "https://example.com/index.html"}}},
    {"browser.find", json::object{{"query", "tool"}}},
    {"shell.exec", json::object{{"command", "sh tests/run.sh"}}},
    {"editor.apply_patch", json::object{{"path", "src/config.txt"}, {"old", "PORT = 8000"}, {"new", "PORT = 9000"}}},
    {"task.update_plan", json::object{{"plan", json::array{"read the spec", "implement", "verify"}}}},
  };
}

TEST("tool schemas: the canonical surface is exactly the nine compact tools") {
  std::vector<std::string> names(CANONICAL_TOOL_NAMES.begin(), CANONICAL_TOOL_NAMES.end());
  rt::sort(names, [](const std::string& left, const std::string& right) { return static_cast<double>(rt::localeCompare(left, right)); });
  EXPECT_EQ(names, (std::vector<std::string>{
                     "browser.find",
                     "browser.open",
                     "browser.search",
                     "editor.apply_patch",
                     "filesystem.find",
                     "filesystem.read",
                     "filesystem.search",
                     "shell.exec",
                     "task.update_plan",
                   }));
  const CanonicalToolSchema* shell = lookupToolSchema("shell.exec");
  EXPECT_EQ(shell ? std::optional<std::string>(shell->name) : std::nullopt, std::optional<std::string>("shell.exec"));
  EXPECT(lookupToolSchema("shell.execx") == nullptr);
}

TEST("tool schemas: every schema is well-formed, bounded and strict-compatible") {
  const AssertCanonicalToolSchemasResult proof = assertCanonicalToolSchemas(true);
  // Optional parameters (apply_patch old/new/add, filesystem.find pattern) are
  // intentional, so the strict-mode proof must fail exactly on those two.
  const auto* failed = std::get_if<AssertCanonicalToolSchemasResultNotOk>(&proof);
  ASSERT(failed != nullptr);
  EXPECT(failed->name == "editor.apply_patch" || failed->name == "filesystem.find");

  const AssertCanonicalToolSchemasResult nonStrict = assertCanonicalToolSchemas(false);
  const auto* proven = std::get_if<AssertCanonicalToolSchemasResultOk>(&nonStrict);
  ASSERT(proven != nullptr);
  EXPECT_EQ(proven->names.size(), std::size_t{9});

  for (std::string_view tool : CANONICAL_TOOL_NAMES) {
    const CanonicalToolSchema* schema = TOOL_SCHEMAS.get(tool);
    ASSERT(schema != nullptr);
    EXPECT_EQ(schema->name, std::string(tool));
    EXPECT(!schema->description.empty());
    const json::object& params = schema->parameters;
    EXPECT(params.at("type") == "object");
    EXPECT(params.at("additionalProperties") == false);
    const json::object& properties = params.at("properties").as_object();
    for (const json::key_value_pair* entry : rt::objectEntries(properties)) {
      const json::value& type = entry->value().at("type");
      if (type == "string" || type == "boolean") continue;
      if (type == "array") {
        EXPECT(entry->value().at("items").at("type") == "string");
        continue;
      }
      EXPECT(false);
    }
    const uos::authority::contracts::Record<std::string> types = toolParameterTypes(*schema);
    for (const std::string& key : rt::objectKeys(properties)) {
      const std::string* type = types.get(key);
      EXPECT(type != nullptr && (*type == "string" || *type == "boolean" || *type == "string[]"));
    }
  }
}

TEST("tool schemas: definitions expose m01 ToolDefinitions with uniform strictness") {
  const std::vector<ToolDefinition> definitions = toolDefinitions();
  EXPECT_EQ(definitions.size(), std::size_t{9});
  for (const ToolDefinition& definition : definitions) {
    EXPECT_EQ(definition.strict, std::optional<bool>(CANONICAL_TOOL_DEFAULT_STRICTNESS));
    const CanonicalToolSchema* registered = TOOL_SCHEMAS.get(definition.name);
    ASSERT(registered != nullptr);
    EXPECT(rt::deepEqual(json::value(definition.parameters), json::value(registered->parameters)));
  }
  std::set<std::optional<bool>> strictValues;
  for (const ToolDefinition& definition : definitions) strictValues.insert(definition.strict);
  EXPECT_EQ(strictValues.size(), std::size_t{1});

  const std::vector<ToolDefinition> strict = toolDefinitions(ToolDefinitionsOptions{.strict = true});
  EXPECT(std::all_of(strict.begin(), strict.end(), [](const ToolDefinition& definition) { return definition.strict == true; }));

  // Direct compat with the m01 adapter's normalization helper.
  const auto normalized = uos::harmony::adapter::normalizeToolStrictness(definitions, true);
  EXPECT(std::all_of(normalized.begin(), normalized.end(), [](const ToolDefinition& definition) { return definition.strict == true; }));

  // The definitions are assignable to the m01 model-facing ToolDefinition type.
  const std::vector<ToolDefinition>& asM01 = definitions;
  EXPECT_EQ(asM01.size(), std::size_t{9});
}

TEST("tool schemas: every canonical tool validates its documented argument shape") {
  for (const auto& [tool, args] : validExamples) {
    const ValidateToolArgumentsResult result = validateToolArguments(tool, json::value(args));
    EXPECT(isValid(result));
    const auto* valid = std::get_if<ValidateToolArgumentsResultValid>(&result);
    EXPECT(valid != nullptr && rt::deepEqual(json::value(valid->arguments), json::value(args)));
  }
  // Optional parameters stay optional.
  EXPECT(isValid(validateToolArguments("editor.apply_patch", json::value(json::object{{"path", "a.txt"}, {"add", true}, {"new", "x"}}))));
  EXPECT(isValid(validateToolArguments("filesystem.find", json::value(json::object{{"path", "docs"}}))));
  EXPECT(isValid(validateToolArguments("task.update_plan", json::value(json::object{{"plan", json::array{}}}))));
  // Empty old/new are meaningful (start-of-file insert), so they are allowed.
  EXPECT(isValid(validateToolArguments("editor.apply_patch", json::value(json::object{{"path", "a.txt"}, {"old", ""}, {"new", "x"}}))));
}

TEST("tool schemas: validation rejects unknown tools, unknown args, missing and mistyped args") {
  struct Case {
    std::string tool;
    json::value args;
    std::string reason;
  };
  const std::vector<Case> cases{
    {"ghost.action", json::object{}, "unknown tool"},
    {"filesystem.read", 42, "arguments must be an object"},
    {"filesystem.read", json::object{{"path", "a.txt"}, {"magic", 1}}, "unexpected argument"},
    {"filesystem.read", json::object{}, "missing required argument"},
    {"filesystem.read", json::object{{"path", 42}}, "must be a string"},
    {"filesystem.read", json::object{{"path", ""}}, "must be a non-empty string"},
    {"filesystem.find", json::object{{"path", "docs"}, {"pattern", 7}}, "must be a string"},
    {"shell.exec", json::object{{"command", ""}}, "must be a non-empty string"},
    {"editor.apply_patch", json::object{{"path", "a.txt"}, {"add", "yes"}}, "must be a boolean"},
    {"task.update_plan", json::object{{"plan", json::array{"ok", ""}}}, "array of non-empty strings"},
    {"task.update_plan", json::object{{"plan", "nope"}}, "array of non-empty strings"},
  };
  for (const Case& entry : cases) {
    const ValidateToolArgumentsResult result = validateToolArguments(entry.tool, entry.args);
    EXPECT_EQ(isValid(result), false);
    EXPECT(rt::includes(reasonOf(result), entry.reason));
  }
}

TEST("differential: an Object.prototype tool name passes the unknown-tool check and then throws") {
  for (std::string_view tool : {"constructor", "__proto__", "toString", "hasOwnProperty"}) {
    EXPECT(lookupToolSchema(tool) == nullptr);
    EXPECT_EQ(reasonOf(validateToolArguments(tool, json::value(5))), "arguments must be an object");
    EXPECT_EQ(reasonOf(validateToolArguments(tool, json::value(nullptr))), "arguments must be an object");
    EXPECT_EQ(typeErrorOf(tool, json::value(json::object{})), "TypeError: undefined is not an object (evaluating 'toolSchema.parameters.properties')");
    EXPECT_EQ(typeErrorOf(tool, json::value(json::object{{"path", "x"}})), "TypeError: undefined is not an object (evaluating 'toolSchema.parameters.properties')");
  }
  EXPECT_EQ(reasonOf(validateToolArguments("ghost", nullptr)), "unknown tool \"ghost\"");
  EXPECT_EQ(reasonOf(validateToolArguments("filesystem.read", nullptr)), "arguments must be an object");
}

TEST("differential: argument keys that Object.prototype provides are never unexpected") {
  const json::object inherited{{"path", "x"}, {"toString", 1}, {"constructor", "y"}};
  const ValidateToolArgumentsResult first = validateToolArguments("filesystem.read", json::value(inherited));
  EXPECT(isValid(first));
  const json::value proto = rt::jsonParse("{\"path\":\"x\",\"__proto__\":5}");
  const ValidateToolArgumentsResult second = validateToolArguments("filesystem.read", proto);
  const auto* valid = std::get_if<ValidateToolArgumentsResultValid>(&second);
  ASSERT(valid != nullptr);
  EXPECT_EQ(rt::jsonStringify(json::value(valid->arguments)), "{\"path\":\"x\",\"__proto__\":5}");
}

TEST("differential: toolParameterTypes reads properties as Object.entries does") {
  const auto typesOf = [](const json::object& parameters) {
    return rt::jsonStringify([&] {
      json::object out;
      const uos::authority::contracts::Record<std::string> types = toolParameterTypes(CanonicalToolSchema{.name = "x", .description = "", .parameters = parameters});
      for (const std::string& key : types.keys()) out[key] = *types.get(key);
      return json::value(std::move(out));
    }());
  };
  const auto errorOf = [&](const json::object& parameters) {
    try {
      return typesOf(parameters);
    } catch (const rt::TypeError& error) {
      return error.toString();
    }
  };
  EXPECT_EQ(errorOf(json::object{}), "TypeError: Object.entries requires that input parameter not be null or undefined");
  EXPECT_EQ(errorOf(json::object{{"properties", nullptr}}), "TypeError: Object.entries requires that input parameter not be null or undefined");
  EXPECT_EQ(typesOf(json::object{{"properties", "ab"}}), "{\"0\":\"string\",\"1\":\"string\"}");
  EXPECT_EQ(typesOf(json::object{{"properties", 5}}), "{}");
  EXPECT_EQ(typesOf(json::object{{"properties", json::array{json::object{{"type", "boolean"}}, json::object{{"type", "array"}}, 3}}}),
            "{\"0\":\"boolean\",\"1\":\"string[]\",\"2\":\"string\"}");
  EXPECT_EQ(typesOf(rt::jsonParse("{\"properties\":{\"__proto__\":{\"type\":\"boolean\"},\"10\":{\"type\":\"array\"},\"2\":{\"type\":\"boolean\"},\"a\":{}}}").as_object()),
            "{\"2\":\"boolean\",\"10\":\"string[]\",\"a\":\"string\"}");
  EXPECT_EQ(typesOf(json::object{{"properties", true}}), "{}");
}
