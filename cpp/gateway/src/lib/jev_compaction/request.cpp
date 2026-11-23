// Port of lib/jev_compaction/request.ts.
#include "uos/lib/jev_compaction/request.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"

namespace uos::lib::jev_compaction::request {
  namespace {
    // `container[name]` on an unknown value: an own member of an object, or an
    // element of an array when `name` is a canonical index.
    const json::value* memberOf(const json::value& container, std::string_view name) {
      if (container.is_object()) return rt::get(container.as_object(), name);
      if (container.is_array() && rt::isArrayIndex(name)) {
        const json::array& array = container.as_array();
        const auto index = static_cast<std::size_t>(rt::stringToNumber(name));
        return index < array.size() ? &array[index] : nullptr;
      }
      return nullptr;
    }
  }

  JevRequest buildJevRequest(const BuildJevRequestParams& params, const types::JevState& state, const types::JevQuestions& questions) {
    json::object body;
    rt::objectDefine(body, "model", json::value(params.model.value_or(std::string(DEFAULT_MODEL))));
    rt::objectDefine(body, "state", state);
    rt::objectDefine(body, "questions", types::toJson(questions));
    JevRequest request;
    request.url = params.baseUrl.value_or(std::string(SYSTEM_ONE_URL));
    request.method = "POST";
    request.headers.set("authorization", "Bearer " + params.apiKey);
    request.headers.set("content-type", "application/json");
    request.body = rt::jsonStringify(json::value(std::move(body)));
    return request;
  }

  types::JevResponse parseJevResponse(double status, bool ok, std::string_view text) {
    if (!ok) {
      throw rt::JsError("Jev request failed (" + rt::numberToString(status) + "): " + rt::slice(text, 0, 200));
    }
    json::value parsed;
    try {
      parsed = rt::jsonParse(text);
    } catch (const rt::JsError&) {
      throw rt::JsError("Jev returned malformed JSON");
    }
    // `typeof parsed.answers !== "object"` also admits an array, as in the TS.
    const json::value* answers = rt::get(parsed, "answers");
    if (!parsed.is_object() || answers == nullptr || answers->is_null() || !(answers->is_object() || answers->is_array())) {
      throw rt::JsError("Jev response is missing answers");
    }
    return parsed.as_object();
  }

  double noulAnswer(const json::value* answers, std::string_view name) {
    if (answers == nullptr) throw rt::TypeError("undefined is not an object (evaluating 'answers[name]')");
    if (answers->is_null()) throw rt::TypeError("null is not an object (evaluating 'answers[name]')");
    const json::value* answer = memberOf(*answers, name);
    if (answer == nullptr || !(answer->is_object() || answer->is_array()) || memberOf(*answer, "noul") == nullptr) {
      throw rt::JsError("Invalid Jev answer for " + std::string(name));
    }
    const std::optional<double> value = rt::getNumber(memberOf(*answer, "noul"));
    if (!value || !rt::Number::isFinite(*value)) {
      throw rt::JsError("Invalid Jev answer for " + std::string(name));
    }
    return *value;
  }
}
