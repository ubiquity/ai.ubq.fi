#include "uos/harmony/reliability/feedback.hpp"

#include "uos/harmony/tools/schemas.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace uos::harmony::reliability::feedback {
  namespace {
    using tools::schemas::CANONICAL_TOOL_NAMES;
    using tools::schemas::CanonicalToolSchema;
    using tools::schemas::TOOL_SCHEMAS;

    bool isRecord(const json::value* value) { return rt::isRecord(value); }

    std::string typeLabel(std::string_view type) { return type == "array" ? std::string("an array of non-empty strings") : "a " + std::string(type); }

    ToolArgumentIssue issue(ArgumentIssueCode code, std::string location, std::string message, std::string hint) {
      return ToolArgumentIssue{.code = std::move(code), .location = std::move(location), .message = std::move(message), .hint = std::move(hint)};
    }

    /** Declared parameters of one canonical tool schema. */
    using SchemaProperties = json::object;

    /**
     * Code-unit ascending order: the exact order `Array.prototype.sort` already
     * applies by default.  Stated explicitly so the ordering stays deterministic
     * (and locale independent) rather than depending on the engine default.
     */
    double compareCodeUnits(const std::string& a, const std::string& b) {
      if (rt::lessThan(a, b)) return -1;
      if (rt::lessThan(b, a)) return 1;
      return 0;
    }

    // The own property names of Object.prototype in Bun. A plain-object read of one of them
    // (`TOOL_SCHEMAS[tool]`, `key in args`, `args[key]`) finds the inherited member.
    bool isObjectPrototypeName(std::string_view key) {
      static constexpr std::string_view NAMES[]{
        "toString",         "toLocaleString",   "valueOf",          "hasOwnProperty", "propertyIsEnumerable", "isPrototypeOf",
        "__defineGetter__", "__defineSetter__", "__lookupGetter__", "__lookupSetter__", "__proto__",          "constructor",
      };
      return std::find(std::begin(NAMES), std::end(NAMES), key) != std::end(NAMES);
    }

    // `key in object` on a plain object.
    bool inPlainObject(const json::object& object, std::string_view key) { return rt::hasOwn(object, key) || isObjectPrototypeName(key); }

    // The value `args[key]` reads when it is not undefined: an own member, or an inherited
    // Object.prototype member (a function, or Object.prototype itself for "__proto__"), which
    // no JSON value represents.
    struct ArgumentValue {
      const json::value* own;
      std::string_view key;
      std::string typeOf() const {
        if (own) return std::string(rt::typeOf(*own));
        return key == "__proto__" ? "object" : "function";
      }
    };

    // `args[key]`, std::nullopt when it reads undefined.
    std::optional<ArgumentValue> readArgument(const json::object& args, std::string_view key) {
      if (const json::value* own = rt::get(args, key)) return ArgumentValue{.own = own, .key = key};
      if (isObjectPrototypeName(key)) return ArgumentValue{.own = nullptr, .key = key};
      return std::nullopt;
    }

    /** Declared JSON-Schema `type` of one parameter, defaulting to `string`. */
    // `param` nullptr stands for the `{}` fallback (and for an inherited member, which has no `type`).
    std::string schemaTypeOf(const json::value* param) {
      const std::optional<std::string_view> type = param ? rt::getString(*param, "type") : std::nullopt;
      return type ? std::string(*type) : std::string("string");
    }

    // `properties[key] ?? {}`: nullptr stands for `{}` (and for an inherited member, which reads no member).
    const json::value* declaredParam(const SchemaProperties& properties, std::string_view key) {
      const json::value* param = rt::get(properties, key);
      return rt::isNullish(param) ? nullptr : param;
    }

    /** Unknown keys (stable order: input order, sorted for determinism). */
    void collectUnexpectedArguments(const json::object& args, const SchemaProperties& properties, const std::vector<std::string>& ordered, std::vector<ToolArgumentIssue>& issues) {
      std::vector<std::string> keys = rt::objectKeys(args);
      rt::sort(keys, compareCodeUnits);
      std::string allowed = rt::join(ordered, ", ");
      if (allowed.empty()) allowed = "(none)";
      for (const std::string& key : keys) {
        if (inPlainObject(properties, key)) continue;
        issues.push_back(issue("unexpected_argument", "arguments." + key, "unexpected argument " + rt::jsonQuote(key), "remove " + rt::jsonQuote(key) + "; allowed: " + allowed));
      }
    }

    /** Missing required arguments (declared order). */
    void collectMissingRequired(const json::object& args, const SchemaProperties& properties, const std::vector<std::string>& required, std::vector<ToolArgumentIssue>& issues) {
      for (const std::string& key : required) {
        if (!readArgument(args, key)) {
          const json::value* param = declaredParam(properties, key);
          issues.push_back(issue("missing_required", "arguments." + key, "missing required argument " + rt::jsonQuote(key),
                                 "provide " + rt::jsonQuote(key) + " (" + typeLabel(schemaTypeOf(param)) + ")"));
        }
      }
    }

    void collectStringIssue(const std::string& key, const ArgumentValue& value, bool isNonEmpty, std::vector<ToolArgumentIssue>& issues) {
      if (!value.own || !value.own->is_string()) {
        issues.push_back(issue("wrong_type", "arguments." + key, "argument " + rt::jsonQuote(key) + " must be a string, got " + value.typeOf(),
                               "pass a string for " + rt::jsonQuote(key)));
      } else if (isNonEmpty && value.own->get_string().empty()) {
        issues.push_back(issue("non_empty", "arguments." + key, "argument " + rt::jsonQuote(key) + " must be a non-empty string",
                               "pass a non-empty string for " + rt::jsonQuote(key)));
      }
    }

    void collectBooleanIssue(const std::string& key, const ArgumentValue& value, std::vector<ToolArgumentIssue>& issues) {
      if (!value.own || !value.own->is_bool()) {
        issues.push_back(issue("wrong_type", "arguments." + key, "argument " + rt::jsonQuote(key) + " must be a boolean, got " + value.typeOf(),
                               "pass true or false for " + rt::jsonQuote(key)));
      }
    }

    void collectArrayIssues(const std::string& key, const ArgumentValue& value, std::vector<ToolArgumentIssue>& issues) {
      const json::array* items = value.own ? value.own->if_array() : nullptr;
      if (!items) {
        issues.push_back(issue("wrong_type", "arguments." + key, "argument " + rt::jsonQuote(key) + " must be an array of non-empty strings, got " + value.typeOf(),
                               "pass an array of non-empty strings for " + rt::jsonQuote(key)));
        return;
      }
      for (std::size_t index = 0; index < items->size(); index += 1) {
        const json::value& item = (*items)[index];
        if (!item.is_string() || item.get_string().empty()) {
          const std::string position = "[" + std::to_string(index) + "]";
          issues.push_back(issue("bad_array_item", "arguments." + key + position, "argument " + rt::jsonQuote(key) + position + " must be a non-empty string",
                                 "pass an array of non-empty strings for " + rt::jsonQuote(key)));
        }
      }
    }

    /** One value problem (declared-property order is handled by the caller). */
    void collectValueIssue(const std::string& key, const ArgumentValue& value, const json::value* param, std::vector<ToolArgumentIssue>& issues) {
      const std::string type = schemaTypeOf(param);
      if (type == "string") {
        const std::optional<double> minLength = param ? rt::getNumber(*param, "minLength") : std::nullopt;
        collectStringIssue(key, value, minLength == 1.0, issues);
      } else if (type == "boolean") {
        collectBooleanIssue(key, value, issues);
      } else if (type == "array") {
        collectArrayIssues(key, value, issues);
      }
    }
  }

  DetailedValidationResult validateToolArgumentsDetailed(std::string_view tool, const json::value& args) { return validateToolArgumentsDetailed(tool, &args); }

  DetailedValidationResult validateToolArgumentsDetailed(std::string_view tool, const json::value* args) {
    // `TOOL_SCHEMAS[tool]`: an Object.prototype name reads the inherited member, which is not undefined.
    const CanonicalToolSchema* schema = TOOL_SCHEMAS.get(tool);
    if (!schema && !isObjectPrototypeName(tool)) {
      std::vector<ToolArgumentIssue> issues{
        ToolArgumentIssue{
          .code = "unknown_tool",
          .location = "tool",
          .message = "unknown tool " + rt::jsonQuote(tool),
          .hint = "use one of: " + rt::join(std::vector<std::string>(CANONICAL_TOOL_NAMES.begin(), CANONICAL_TOOL_NAMES.end()), ", "),
        },
      };
      return DetailedValidationResult{.valid = false, .arguments = {}, .issues = std::move(issues)};
    }
    if (!isRecord(args)) {
      return DetailedValidationResult{
        .valid = false,
        .arguments = {},
        .issues = {issue("not_an_object", "arguments", "arguments must be a JSON object", "pass a JSON object with the tool's arguments")},
      };
    }
    if (!schema) throw rt::TypeError("undefined is not an object (evaluating 'schema.parameters.properties')");
    const json::object& argsObject = args->get_object();

    std::vector<ToolArgumentIssue> issues;
    const SchemaProperties& properties = rt::get(schema->parameters, "properties")->get_object();
    std::vector<std::string> required;
    if (const json::array* declared = rt::getArray(schema->parameters, "required")) {
      for (const json::value& key : *declared) required.push_back(std::string(key.get_string()));
    }
    const std::vector<std::string> ordered = rt::objectKeys(properties);

    collectUnexpectedArguments(argsObject, properties, ordered, issues);
    collectMissingRequired(argsObject, properties, required, issues);
    for (const std::string& key : ordered) {
      if (!readArgument(argsObject, key)) continue;
      // Argument values are untrusted, so `value` stays `unknown` here even
      // though the guard above already skipped the `undefined` it observed.
      const std::optional<ArgumentValue> value = readArgument(argsObject, key);
      const json::value* param = declaredParam(properties, key);
      if (!value) {
        issues.push_back(issue("undefined_value", "arguments." + key, "argument " + rt::jsonQuote(key) + " must not be undefined",
                               "omit " + rt::jsonQuote(key) + " or pass a " + typeLabel(schemaTypeOf(param))));
      } else {
        collectValueIssue(key, *value, param, issues);
      }
    }

    const bool valid = issues.empty();
    return DetailedValidationResult{.valid = valid, .arguments = argsObject, .issues = std::move(issues)};
  }

  std::string renderValidationFeedback(std::string_view tool, const DetailedValidationResult& result) {
    if (result.valid) return "";
    std::vector<std::string> parts;
    for (std::size_t index = 0; index < result.issues.size(); index += 1) {
      const ToolArgumentIssue& i = result.issues[index];
      parts.push_back("#" + std::to_string(index + 1) + ": " + i.code + " " + i.location + " — " + i.hint);
    }
    return "invalid arguments (" + std::to_string(result.issues.size()) + " issue(s)) for " + rt::jsonQuote(tool) + ": " + rt::join(parts, "; ");
  }

  std::string invalidCallLabel(const DetailedValidationResult& result) {
    // A valid result carries an empty issue list, so the head is genuinely optional.
    if (result.issues.empty()) return "invalid_args";
    const ToolArgumentIssue& first = result.issues.front();
    return first.code + ":" + first.location;
  }
}
