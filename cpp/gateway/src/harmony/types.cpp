#include "uos/harmony/types.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/record.hpp"

// src/harmony/types.ts declares types, one constant and the error class (all in the
// header). The Boost.JSON pair of the snake_case wire record ResponseFormatParam is a
// C++ addition (docs/PORT-NOTES.md, records).
namespace uos::harmony::types {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ResponseFormatParamJsonSchema::JsonSchema& value) {
    out = Writer()
            .required("name", value.name)
            .orUndefined("strict", value.strict)
            .required("schema", value.schema)
            .orUndefined("description", value.description)
            .done();
  }

  ResponseFormatParamJsonSchema::JsonSchema tag_invoke(const json::value_to_tag<ResponseFormatParamJsonSchema::JsonSchema>&, const json::value& value) {
    const Reader in(value, "ResponseFormatParam.json_schema");
    return {
      .name = in.required<std::string>("name"),
      .strict = in.orUndefined<bool>("strict"),
      .schema = in.required<json::object>("schema"),
      .description = in.orUndefined<std::string>("description"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ResponseFormatParam& value) {
    if (const auto* schema = std::get_if<ResponseFormatParamJsonSchema>(&value)) {
      out = Writer().required("type", std::string(ResponseFormatParamJsonSchema::type)).required("json_schema", schema->json_schema).done();
      return;
    }
    out = Writer().required("type", std::string(ResponseFormatParamJsonObject::type)).done();
  }

  ResponseFormatParam tag_invoke(const json::value_to_tag<ResponseFormatParam>&, const json::value& value) {
    const Reader in(value, "ResponseFormatParam");
    const std::string type = in.required<std::string>("type");
    if (type == ResponseFormatParamJsonObject::type) return ResponseFormatParamJsonObject{};
    if (type == ResponseFormatParamJsonSchema::type) {
      return ResponseFormatParamJsonSchema{.json_schema = in.required<ResponseFormatParamJsonSchema::JsonSchema>("json_schema")};
    }
    in.fail("type", rt::get(value, "type"), "\"json_object\" | \"json_schema\"");
  }
}
