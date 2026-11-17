// Differential tests for src/codex/responses-error.ts called directly (its only
// bun test, tests/codex_responses_error_test.cpp, reaches it through the owned
// stream). Inputs and outputs were recorded by running the TS module under Bun
// 1.4.0; `same` is the TS `result === event` identity check.
// Links: src/codex/responses_error.cpp src/responses_failover_stream.cpp src/responses_stream.cpp src/utils.cpp
//        src/inference_deadline.cpp
#include "uos/codex/responses_error.hpp"

#include "uos/responses_failover_stream.hpp"
#include "uos/responses_stream.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using uos::codex::responses_error::codexContextFailureEvent;
using uos::responses_failover_stream::responseEventFromValue;
using uos::responses_stream::ResponsesStreamEvent;

namespace {
  const json::value& vectors() {
    static const json::value parsed = rt::jsonParse(
#include "codex_responses_error_differential_vectors.inc"
    );
    return parsed;
  }
}

TEST("codexContextFailureEvent matches Bun on direct inputs") {
  const json::array& cases = rt::get(vectors(), "cases")->as_array();
  const json::object& outputs = rt::get(vectors(), "out")->as_object();
  ASSERT_EQ(cases.size(), std::size_t{10});
  for (const json::value& entry : cases) {
    const std::string name(rt::getString(entry, "name").value_or(""));
    const ResponsesStreamEvent event = responseEventFromValue(rt::get(entry, "value")->as_object());
    const json::value& responseIdValue = *rt::get(entry, "responseId");
    const std::optional<std::string> responseId = responseIdValue.is_null() ? std::nullopt : std::optional<std::string>(std::string(responseIdValue.get_string()));
    std::vector<json::object> visible;
    for (const json::value& item : rt::get(entry, "visible")->as_array()) visible.push_back(item.as_object());
    const ResponsesStreamEvent result = codexContextFailureEvent(event, responseId, rt::get(entry, "template")->as_object(), visible);
    const json::value& expected = *rt::get(outputs, name);
    json::object actual;
    actual["same"] = result.identity() == event.identity();
    actual["type"] = result.type;
    actual["terminal"] = result.terminal;
    actual["raw"] = result.raw;
    actual["value"] = result.value;
    const bool equal = rt::deepEqual(json::value(actual), expected);
    if (!equal) std::fprintf(stderr, "    %s\n    actual:   %s\n    expected: %s\n", name.c_str(), rt::jsonStringify(json::value(actual)).c_str(), rt::jsonStringify(expected).c_str());
    EXPECT(equal);
  }
}
