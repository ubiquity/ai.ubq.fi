#include "uos/harmony/tools/schemas.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace uos::harmony::tools::schemas {
  namespace {
    // `extra = {}` defaults are never used: every call passes `extra`.
    json::object property(std::string_view type, const json::object& extra) {
      json::object head;
      head["type"] = type;
      return rt::objectSpread(head, extra);
    }

    json::object stringProperty(const json::object& extra) { return property("string", extra); }

    json::object nonEmptyStringProperty(const json::object& extra) {
      json::object minLength;
      minLength["minLength"] = 1;
      return property("string", rt::objectSpread(minLength, extra));
    }

    json::object described(std::string_view description) {
      json::object out;
      out["description"] = description;
      return out;
    }

    CanonicalToolSchema schema(std::string_view name, std::string_view description, json::object properties, const std::vector<std::string>& required) {
      json::array requiredCopy;
      for (const std::string& key : required) requiredCopy.push_back(json::value(key));
      json::object parameters;
      parameters["type"] = "object";
      parameters["properties"] = std::move(properties);
      parameters["required"] = std::move(requiredCopy);
      parameters["additionalProperties"] = false;
      return CanonicalToolSchema{.name = std::string(name), .description = std::string(description), .parameters = std::move(parameters)};
    }

    const std::vector<std::string>& nonEmptyParams(const std::string& name) {
      static const std::unordered_map<std::string, std::vector<std::string>> NON_EMPTY_PARAMS{
        {"filesystem.read", {"path"}},
        {"filesystem.find", {"path", "pattern"}},
        {"filesystem.search", {"path", "query"}},
        {"browser.search", {"query"}},
        {"browser.open", {"url"}},
        {"browser.find", {"query"}},
        {"shell.exec", {"command"}},
        {"editor.apply_patch", {"path"}},
        {"task.update_plan", {}},
      };
      return NON_EMPTY_PARAMS.at(name);
    }

    // The own property names of Object.prototype in Bun. A plain-object read (`TOOL_SCHEMAS[tool]`,
    // `key in parameters`) of one of them finds the inherited member.
    bool isObjectPrototypeName(std::string_view key) {
      static constexpr std::string_view NAMES[]{
        "toString",         "toLocaleString",   "valueOf",          "hasOwnProperty", "propertyIsEnumerable", "isPrototypeOf",
        "__defineGetter__", "__defineSetter__", "__lookupGetter__", "__lookupSetter__", "__proto__",          "constructor",
      };
      return std::find(std::begin(NAMES), std::end(NAMES), key) != std::end(NAMES);
    }

    // `key in object` on a plain object.
    bool inPlainObject(const json::object& object, std::string_view key) { return rt::hasOwn(object, key) || isObjectPrototypeName(key); }

    // ExpectedParameterType: "string" | "boolean" | "string[]"
    std::string describeExpected(std::string_view type) { return type == "string[]" ? std::string("an array of non-empty strings") : "a " + std::string(type); }

    /** Returns the first argument key the schema does not declare, or null. */
    std::optional<std::string> unexpectedArgumentReason(const json::object& args, const json::object& parameters) {
      for (const std::string& key : rt::objectKeys(args)) {
        if (!inPlainObject(parameters, key)) return "unexpected argument " + rt::jsonQuote(key);
      }
      return std::nullopt;
    }

    /** Returns the first missing required argument, or null. */
    std::optional<std::string> missingRequiredArgumentReason(const json::object& args, const json::array& required) {
      for (const json::value& entry : required) {
        const std::string key(entry.get_string());
        // `args[key] === undefined` holds only for an absent member: an inherited one is a function or Object.prototype.
        if (!inPlainObject(args, key)) {
          return "missing required argument " + rt::jsonQuote(key);
        }
      }
      return std::nullopt;
    }

    /** Validates one string parameter against its declared non-empty constraint. */
    std::optional<std::string> stringArgumentReason(const std::string& key, const json::value& value, bool nonEmpty) {
      if (!value.is_string()) return "argument " + rt::jsonQuote(key) + " must be a string";
      if (nonEmpty && value.get_string().empty()) return "argument " + rt::jsonQuote(key) + " must be a non-empty string";
      return std::nullopt;
    }

    /** Validates one array-of-non-empty-strings parameter. */
    std::optional<std::string> arrayArgumentReason(const std::string& key, const json::value& value) {
      const json::array* items = value.if_array();
      const bool bad = !items || std::any_of(items->begin(), items->end(), [](const json::value& item) { return !item.is_string() || item.get_string().empty(); });
      if (bad) return "argument " + rt::jsonQuote(key) + " must be " + describeExpected("string[]");
      return std::nullopt;
    }

    /** Validates one present argument against the type its schema declares. */
    // `value` is never undefined here (Object.entries of a JSON object); nullptr stands for it.
    std::optional<std::string> argumentValueReason(const std::string& key, const json::value* value, const json::object& parameters, const std::vector<std::string>& nonEmpty) {
      if (!value) return "argument " + rt::jsonQuote(key) + " must not be undefined";
      // An inherited Object.prototype member has no `type`: the reason is null.
      const json::value* param = rt::get(parameters, key);
      const std::optional<std::string_view> declared = param ? rt::getString(*param, "type") : std::nullopt;
      const std::string type = declared ? std::string(*declared) : std::string();
      if (type == "string") return stringArgumentReason(key, *value, rt::includes(nonEmpty, key));
      if (type == "boolean") return value->is_bool() ? std::nullopt : std::optional<std::string>("argument " + rt::jsonQuote(key) + " must be a boolean");
      if (type == "array") return arrayArgumentReason(key, *value);
      return std::nullopt;
    }

    bool isStrictSchemaNode(const json::value& value) {
      if (!value.is_object() && !value.is_array()) return true;
      if (const json::array* items = value.if_array()) {
        return std::all_of(items->begin(), items->end(), [](const json::value& item) { return isStrictSchemaNode(item); });
      }
      const json::object& record = value.get_object();
      for (const std::string& key : rt::objectKeys(record)) {
        if (key == "default" || key == "anyOf" || key == "oneOf" || key == "not") return false;
        if (key == "type") {
          const json::value& type = *rt::get(record, "type");
          if (type.is_array() || (type.is_string() && (type.get_string() == "null" || type.get_string() == "integer"))) {
            return false;
          }
        }
        if (!isStrictSchemaNode(*rt::get(record, key))) return false;
      }
      return true;
    }

    /** Returns the first well-formedness violation of one canonical schema, or null. */
    std::optional<std::string> canonicalSchemaReason(const CanonicalToolSchema& toolSchema, bool strict) {
      const json::object& parameters = toolSchema.parameters;
      const json::value* rootType = rt::get(parameters, "type");
      if (!rootType || !rootType->is_string() || rootType->get_string() != "object") return "parameters root must be an object schema";
      const json::value* additionalProperties = rt::get(parameters, "additionalProperties");
      if (!additionalProperties || !additionalProperties->is_bool() || additionalProperties->get_bool()) return "additionalProperties must be false";
      const json::value* properties = rt::get(parameters, "properties");
      if (!properties || !properties->is_object()) return "properties must be an object";
      const json::object& declared = properties->get_object();
      const std::vector<std::string> propertyNames = rt::objectKeys(declared);
      const json::array* requiredArray = rt::getArray(parameters, "required");
      const json::array required = requiredArray ? *requiredArray : json::array();
      if (std::any_of(required.begin(), required.end(), [&](const json::value& key) { return !key.is_string() || !inPlainObject(declared, std::string_view(key.get_string())); })) {
        return "required entries must name declared properties";
      }
      if (strict && required.size() != propertyNames.size()) {
        return "strict mode requires every property to be required";
      }
      for (const std::string& key : propertyNames) {
        if (!isStrictSchemaNode(*rt::get(declared, key))) return "property " + key + " is not strict-mode compatible";
      }
      if (!isStrictSchemaNode(json::value(parameters))) return "parameters schema contains non-strict constructs";
      return std::nullopt;
    }
  }

  namespace detail {
    authority::contracts::Record<CanonicalToolSchema> buildToolSchemas() {
      authority::contracts::Record<CanonicalToolSchema> schemas;
      const auto define = [&schemas](CanonicalToolSchema value) {
        const std::string name = value.name;
        schemas.define(name, std::move(value));
      };
      const auto properties = [](std::initializer_list<std::pair<std::string_view, json::object>> entries) {
        json::object out;
        for (const auto& [key, value] : entries) out[key] = value;
        return out;
      };
      define(schema("filesystem.read",
                    "Read the UTF-8 text of a file inside the workspace. Returns the file content, or a not_found error when the path does not exist.",
                    properties({{"path", nonEmptyStringProperty(described("Workspace-relative file path."))}}), {"path"}));
      define(schema("filesystem.find",
                    "List workspace-relative paths of files under a directory, optionally filtered by a glob pattern ('*' one segment, '**' across segments, '?' one character).",
                    properties({
                      {"path", nonEmptyStringProperty(described("Workspace-relative directory path to list."))},
                      {"pattern", nonEmptyStringProperty(described("Glob pattern; defaults to ** (all files under the directory)."))},
                    }),
                    {"path"}));
      define(schema("filesystem.search",
                    "Case-insensitive substring search inside the contents of files under a directory. Returns at most 200 matches as path:line:content.",
                    properties({
                      {"path", nonEmptyStringProperty(described("Workspace-relative directory path to search."))},
                      {"query", nonEmptyStringProperty(described("Text to search for."))},
                    }),
                    {"path", "query"}));
      define(schema("browser.search", "Search the web and return matching result titles, URLs and snippets. Returns (no results) when nothing matches.",
                    properties({{"query", nonEmptyStringProperty(described("Search query."))}}), {"query"}));
      define(schema("browser.open", "Open a URL and return the page title and text content. The opened page becomes the current page for browser.find.",
                    properties({{"url", nonEmptyStringProperty(described("URL to open, e.g. https://example.com/page."))}}), {"url"}));
      define(schema("browser.find", "Case-insensitive search for text in the currently-open browser page. Returns at most 200 matches as line:content.",
                    properties({{"query", nonEmptyStringProperty(described("Text to find on the current page."))}}), {"query"}));
      define(schema("shell.exec",
                    "Run a shell command in the workspace with sh -c and return its exit code, standard output and standard error. Non-zero exit codes are errors.",
                    properties({{"command", nonEmptyStringProperty(described("Shell command to execute."))}}), {"command"}));
      define(schema("editor.apply_patch",
                    "Replace the unique occurrence of old with new in a file, or create a new file when add is true. old may be empty to insert at the start.",
                    properties({
                      {"path", nonEmptyStringProperty(described("Workspace-relative file path."))},
                      {"old", stringProperty(described("Exact text to replace; must occur exactly once (empty inserts at the start)."))},
                      {"new", stringProperty(described("Replacement text, or the file content when add is true."))},
                      {"add", property("boolean", described("Create the file instead of patching an existing one."))},
                    }),
                    {"path"}));
      json::object minLength;
      minLength["minLength"] = 1;
      json::object plan;
      plan["type"] = "array";
      plan["items"] = stringProperty(minLength);
      plan["description"] = "Ordered plan steps; each entry must be a non-empty string.";
      define(schema("task.update_plan", "Replace the current task plan with the given ordered list of step strings.", properties({{"plan", plan}}), {"plan"}));
      return schemas;
    }
  }

  const CanonicalToolSchema* lookupToolSchema(std::string_view tool) { return TOOL_SCHEMAS.get(tool); }

  ValidateToolArgumentsResult validateToolArguments(std::string_view tool, const json::value& args) { return validateToolArguments(tool, &args); }

  ValidateToolArgumentsResult validateToolArguments(std::string_view tool, const json::value* args) {
    const CanonicalToolSchema* toolSchema = lookupToolSchema(tool);
    // An Object.prototype name reads the inherited (non-null) member and passes this check.
    if (!toolSchema && !isObjectPrototypeName(tool)) return ValidateToolArgumentsResultInvalid{.reason = "unknown tool " + rt::jsonQuote(tool)};
    if (!rt::isRecord(args)) return ValidateToolArgumentsResultInvalid{.reason = "arguments must be an object"};
    if (!toolSchema) throw rt::TypeError("undefined is not an object (evaluating 'toolSchema.parameters.properties')");
    const json::object& argsObject = args->get_object();

    const json::object& parameters = rt::get(toolSchema->parameters, "properties")->get_object();
    const json::array& required = rt::get(toolSchema->parameters, "required")->get_array();
    const std::vector<std::string>& nonEmpty = nonEmptyParams(toolSchema->name);

    std::optional<std::string> structuralReason = unexpectedArgumentReason(argsObject, parameters);
    if (!structuralReason) structuralReason = missingRequiredArgumentReason(argsObject, required);
    if (structuralReason) return ValidateToolArgumentsResultInvalid{.reason = std::move(*structuralReason)};

    for (const json::key_value_pair* entry : rt::objectEntries(argsObject)) {
      std::optional<std::string> reason = argumentValueReason(std::string(entry->key()), &entry->value(), parameters, nonEmpty);
      if (reason) return ValidateToolArgumentsResultInvalid{.reason = std::move(*reason)};
    }
    return ValidateToolArgumentsResultValid{.arguments = argsObject};
  }

  authority::contracts::Record<std::string> toolParameterTypes(const CanonicalToolSchema& toolSchema) {
    const json::value* parameters = rt::get(toolSchema.parameters, "properties");
    authority::contracts::Record<std::string> out;
    // Object.entries(parameters)
    if (rt::isNullish(parameters)) throw rt::TypeError("Object.entries requires that input parameter not be null or undefined");
    const auto typeOf = [](const json::value* param) -> std::string {
      const std::optional<std::string_view> type = param ? rt::getString(*param, "type") : std::nullopt;
      if (type == "boolean") return "boolean";
      if (type == "array") return "string[]";
      return "string";
    };
    if (const json::object* object = parameters->if_object()) {
      for (const json::key_value_pair* entry : rt::objectEntries(*object)) out.set(std::string(entry->key()), typeOf(&entry->value()));
    } else if (const json::array* array = parameters->if_array()) {
      for (std::size_t index = 0; index < array->size(); index += 1) out.set(std::to_string(index), typeOf(&(*array)[index]));
    } else if (parameters->is_string()) {
      // One entry per UTF-16 code unit, each a one-unit string with no `type`.
      const std::int64_t length = rt::length(std::string_view(parameters->get_string()));
      for (std::int64_t index = 0; index < length; index += 1) out.set(std::to_string(index), "string");
    }
    return out;
  }

  std::vector<types::ToolDefinition> toolDefinitions() { return toolDefinitions(ToolDefinitionsOptions{}); }

  std::vector<types::ToolDefinition> toolDefinitions(const ToolDefinitionsOptions& opts) {
    const bool strict = opts.strict.value_or(CANONICAL_TOOL_DEFAULT_STRICTNESS);
    std::vector<types::ToolDefinition> definitions;
    for (std::string_view name : CANONICAL_TOOL_NAMES) {
      const CanonicalToolSchema& toolSchema = *TOOL_SCHEMAS.get(name);
      definitions.push_back(types::ToolDefinition{
        .name = toolSchema.name,
        .description = toolSchema.description,
        .parameters = toolSchema.parameters,
        .strict = strict,
      });
    }
    return definitions;
  }

  AssertCanonicalToolSchemasResult assertCanonicalToolSchemas(bool strict) {
    for (const CanonicalToolSchema* toolSchema : TOOL_SCHEMAS.values()) {
      std::optional<std::string> reason = canonicalSchemaReason(*toolSchema, strict);
      if (reason) return AssertCanonicalToolSchemasResultNotOk{.name = toolSchema->name, .reason = std::move(*reason)};
    }
    return AssertCanonicalToolSchemasResultOk{.names = std::vector<CanonicalToolName>(CANONICAL_TOOL_NAMES.begin(), CANONICAL_TOOL_NAMES.end())};
  }
}
