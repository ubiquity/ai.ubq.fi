#include "uos/codex/responses_error.hpp"

#include "uos/rt/json.hpp"
#include "uos/utils.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace uos::codex::responses_error {
  responses_stream::ResponsesStreamEvent codexContextFailureEvent(const responses_stream::ResponsesStreamEvent& event, const std::optional<std::string>& responseId,
                                                                  const json::object& responseTemplate, const std::vector<json::object>& visibleOutput) {
    if (event.type != "error" || !rt::truthy(responseId)) return event;
    const json::value* nested = rt::get(event.value, "error");
    json::object error;
    if (utils::isRecord(nested) && !nested->is_array()) {
      error = nested->get_object();
    } else {
      for (const json::key_value_pair* entry : rt::objectEntries(event.value)) {
        if (entry->key() == "type" || entry->key() == "sequence_number") continue;
        rt::objectDefine(error, entry->key(), entry->value());
      }
    }
    const json::value* code = rt::get(error, "code");
    if (!code || !code->is_string() || code->get_string() != "context_length_exceeded") return event;
    json::array output;
    if (const json::array* templateOutput = rt::getArray(responseTemplate, "output")) output = *templateOutput;
    for (const json::object& item : visibleOutput) {
      const std::optional<std::string> id = utils::getString(rt::get(item, "id"));
      std::int64_t index = -1;
      if (rt::truthy(id)) {
        for (std::size_t position = 0; position < output.size(); ++position) {
          const json::value* existingId = utils::isRecord(output[position]) ? rt::get(output[position], "id") : nullptr;
          if (existingId && existingId->is_string() && existingId->get_string() == *id) {
            index = static_cast<std::int64_t>(position);
            break;
          }
        }
      }
      if (index < 0) output.push_back(item);
      else output[static_cast<std::size_t>(index)] = item;
    }
    json::object response = rt::objectSpread(responseTemplate);
    rt::objectDefine(response, "id", json::string(*responseId));
    rt::objectDefine(response, "object", json::string(utils::getString(rt::get(responseTemplate, "object")).value_or("response")));
    rt::objectDefine(response, "status", json::string("failed"));
    rt::objectDefine(response, "error", rt::objectSpread(error));
    rt::objectDefine(response, "output", std::move(output));
    json::object value;
    rt::objectDefine(value, "type", json::string("response.failed"));
    if (const json::value* sequence = rt::get(event.value, "sequence_number")) rt::objectDefine(value, "sequence_number", *sequence);
    rt::objectDefine(value, "response", std::move(response));
    std::string raw = "event: response.failed\ndata: " + rt::jsonStringify(json::value(value)) + "\n\n";
    return responses_stream::ResponsesStreamEvent{.raw = std::move(raw), .value = std::move(value), .type = "response.failed", .terminal = true};
  }
}
