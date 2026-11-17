#include "uos/harmony/render.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"

#include <unordered_set>

namespace uos::harmony::render {
  namespace {
    std::string channelList(const std::vector<std::string>& channels) { return rt::join(channels, ", "); }

    std::string jsonLiteral(const json::value& value) {
      if (value.is_string()) return quoteString(std::string_view(value.get_string()));
      return rt::jsonStringify(value);
    }

    std::string indent(std::string_view text, int level) {
      std::vector<std::string> lines = rt::split(text, "\n");
      for (std::string& line : lines) {
        if (!line.empty()) line = rt::repeat("  ", level) + line;
      }
      return rt::join(lines, "\n");
    }

    std::optional<std::string> commentPrefix(std::string_view text) {
      const std::string comment = rt::trim(text);
      if (comment.empty()) return std::nullopt;
      return indent("// " + comment, 1);
    }

    std::optional<std::string> enumUnionType(const json::object& schema) {
      const json::value* constEnum = rt::get(schema, "enum");
      if (!constEnum || !constEnum->is_array() || constEnum->get_array().empty()) return std::nullopt;
      std::vector<std::string> parts;
      for (const json::value& entry : constEnum->get_array()) parts.push_back(jsonLiteral(entry));
      return rt::join(parts, " | ");
    }

    std::optional<std::string> anyOfUnionType(const json::object& schema) {
      const json::value* anyOfMember = rt::get(schema, "anyOf");
      const json::value* candidate = anyOfMember && anyOfMember->is_array() ? anyOfMember : rt::get(schema, "oneOf");
      const json::array* anyOf = candidate && candidate->is_array() ? &candidate->get_array() : nullptr;
      if (!anyOf || anyOf->empty()) return std::nullopt;
      std::vector<std::string> parts;
      for (const json::value& entry : *anyOf) parts.push_back(harmonyTypeFromJsonSchema(entry));
      return rt::join(parts, " | ");
    }

    /** One or two lines: the optional `// comment` and the rendered field. */
    std::vector<std::string> objectFieldLines(const std::string& name, const json::value* field, const std::unordered_set<std::string>& required) {
      // An array field reads every member as undefined, like the `{}` fallback.
      const json::object* fieldSchema = field && field->is_object() ? &field->get_object() : nullptr;
      const std::optional<std::string_view> descriptionText = fieldSchema ? rt::getString(*fieldSchema, "description") : std::nullopt;
      const std::optional<std::string> description = commentPrefix(descriptionText ? *descriptionText : std::string_view());
      const json::value* defaultValue = fieldSchema ? rt::get(*fieldSchema, "default") : nullptr;
      const std::string suffix = !defaultValue || defaultValue->is_null() ? std::string(",") : ", // default: " + jsonLiteral(*defaultValue);
      const std::string line = name + (required.contains(name) ? "" : "?") + ": " + harmonyTypeFromJsonSchema(field) + suffix;
      if (description) return {*description, line};
      return {line};
    }

    std::string objectTypeFromJsonSchema(const json::object& schema) {
      const json::value* properties = rt::get(schema, "properties");
      std::unordered_set<std::string> required;
      if (const json::array* names = rt::getArray(schema, "required")) {
        for (const json::value& name : *names) {
          if (name.is_string()) required.insert(std::string(name.get_string()));
        }
      }
      if (!rt::truthy(properties) || rt::typeOf(properties) != "object" || properties->is_array()) return "any";
      const std::vector<const json::key_value_pair*> fields = rt::objectEntries(properties->get_object());
      if (fields.empty()) return "{}";
      std::vector<std::string> lines;
      for (const json::key_value_pair* field : fields) {
        for (std::string& line : objectFieldLines(std::string(field->key()), &field->value(), required)) lines.push_back(std::move(line));
      }
      return "{\n" + indent(rt::join(lines, "\n"), 1) + "\n}";
    }
  }

  std::string renderSystemMessage(const SystemRenderOptions& options) {
    const std::string identity = options.identity.value_or("You are ChatGPT, a large language model trained by OpenAI.");
    std::vector<std::string> lines{identity};
    lines.push_back("Knowledge cutoff: " + options.knowledgeCutoff.value_or("2024-06"));
    lines.push_back("Current date: " + options.currentDate);
    lines.push_back("");
    lines.push_back("Reasoning: " + options.reasoningEffort);
    lines.push_back("");
    const std::vector<std::string> channels =
      options.channels ? *options.channels : std::vector<std::string>(HARMONY_ASSISTANT_CHANNELS.begin(), HARMONY_ASSISTANT_CHANNELS.end());
    lines.push_back("# Valid channels: " + channelList(channels) + ". " + "Channel must be included for every message.");
    const rt::Nullable<std::string> toolNamespace = options.toolNamespace.isUndefined() ? rt::Nullable<std::string>(std::string("functions")) : options.toolNamespace;
    if (!toolNamespace.isNull()) {
      lines.push_back("Calls to these tools must go to the commentary channel: '" + *toolNamespace + "'.");
    }
    return rt::join(lines, "\n");
  }

  std::string quoteString(std::string_view value) {
    return "\"" + rt::replaceAll(rt::replaceAll(rt::replaceAll(value, "\\", "\\\\"), "\"", "\\\""), "\n", "\\n") + "\"";
  }

  std::string harmonyTypeFromJsonSchema(const json::value& schemaValue) { return harmonyTypeFromJsonSchema(&schemaValue); }

  std::string harmonyTypeFromJsonSchema(const json::value* schemaValue) {
    if (!rt::truthy(schemaValue) || rt::typeOf(schemaValue) != "object") return "any";
    // An array passes the object check and then reads every member as undefined: "any".
    if (!schemaValue->is_object()) return "any";
    const json::object& schema = schemaValue->get_object();

    const std::optional<std::string> enumType = enumUnionType(schema);
    if (enumType) return *enumType;

    const std::optional<std::string> unionType = anyOfUnionType(schema);
    if (unionType) return *unionType;

    const std::optional<std::string_view> type = rt::getString(schema, "type");
    if (type == "object" || (!type && rt::truthy(rt::get(schema, "properties")))) return objectTypeFromJsonSchema(schema);
    if (type == "array" || (!type && rt::truthy(rt::get(schema, "items")))) return harmonyTypeFromJsonSchema(rt::get(schema, "items")) + "[]";
    return type ? std::string(*type) : std::string("any");
  }

  std::string renderToolDefinition(const types::ToolDefinition& tool) {
    const std::optional<std::string> description = commentPrefix(tool.description);
    const std::string& name = tool.name;
    const json::value parameters(tool.parameters);
    const std::string prefix = description ? *description + "\n" : "";
    const std::string type = harmonyTypeFromJsonSchema(parameters);
    if (type == "any") {
      return prefix + "type " + name + " = () => any;";
    }
    return prefix + "type " + name + " = (_: " + type + ") => any;";
  }

  std::string renderDeveloperMessage(const DeveloperRenderOptions& options) {
    std::vector<std::string> lines{"# Instructions", "", options.instructions};
    if (options.tools && !options.tools->empty()) {
      const std::string namespace_ = options.namespace_.value_or("functions");
      for (std::string line : {std::string(), std::string("# Tools"), std::string(), "## " + namespace_, std::string(), "namespace " + namespace_ + " {", std::string()})
        lines.push_back(std::move(line));
      for (const types::ToolDefinition& tool : *options.tools) {
        lines.push_back(renderToolDefinition(tool));
        lines.push_back("");
      }
      lines.push_back("} // namespace " + namespace_);
    }
    if (options.responseFormat) {
      for (std::string line : {std::string(), std::string("# Response Formats"), std::string(), "## " + options.responseFormat->formatName}) lines.push_back(std::move(line));
      if (rt::truthy(options.responseFormat->description)) {
        lines.push_back("");
        lines.push_back("// " + *options.responseFormat->description);
      }
      lines.push_back("");
      lines.push_back(rt::jsonStringify(json::value(options.responseFormat->schema)));
    }
    return rt::join(lines, "\n");
  }
}
