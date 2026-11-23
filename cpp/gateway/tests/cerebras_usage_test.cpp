// Links: tests/cerebras_usage_test.cpp src/provider/cerebras.cpp src/utils.cpp
// Port of tests/cerebras-usage.test.ts (every case).
#include "uos/provider/cerebras.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

#include <string>
#include <string_view>
#include <variant>

namespace rt = uos::rt;
namespace json = uos::json;
namespace cerebras = uos::provider::cerebras;

namespace {
  json::value completion(std::string_view usage) {
    json::object value = rt::jsonParse(
                           R"({"id":"chatcmpl-cerebras-1","object":"chat.completion","created":1780000000,"model":"gpt-oss-120b",)"
                           R"("choices":[{"index":0,"message":{"role":"assistant","content":"OK"},"finish_reason":"stop"}]})")
                           .as_object();
    value["usage"] = rt::jsonParse(usage);
    return value;
  }

  json::value usageOf(std::string_view usage) {
    const cerebras::NormalizationResult<json::object> result = cerebras::normalizeCerebrasChatCompletion(completion(usage), "gpt-oss-120b");
    if (const auto* failure = std::get_if<cerebras::NormalizationResultNotOk>(&result)) {
      throw rt::JsError("expected the Cerebras usage to normalize: " + failure->message);
    }
    const json::value* normalized = rt::get(std::get<cerebras::NormalizationResultOk<json::object>>(result).value, "usage");
    return normalized ? *normalized : json::value(nullptr);
  }

  bool sameJson(const json::value& actual, std::string_view expected) { return rt::deepEqual(actual, rt::jsonParse(expected)); }
}

TEST("cerebras: relays the documented cache-read and reasoning counters under their official names") {
  // Shaped like a real gpt-oss-120b response: the provider nests the counters
  // beside prediction bookkeeping this gateway does not report.
  EXPECT(sameJson(usageOf(R"({"prompt_tokens":4472,"completion_tokens":31,"total_tokens":4503,)"
                          R"("prompt_tokens_details":{"cached_tokens":4352,"accepted_prediction_tokens":0,"rejected_prediction_tokens":0},)"
                          R"("completion_tokens_details":{"reasoning_tokens":20,"accepted_prediction_tokens":0,"rejected_prediction_tokens":0}})"),
                  R"({"prompt_tokens":4472,"completion_tokens":31,"total_tokens":4503,"prompt_tokens_details":{"cached_tokens":4352},"completion_tokens_details":{"reasoning_tokens":20}})"));
}

TEST("cerebras: an unreported counter stays absent instead of becoming a measured zero") {
  EXPECT(sameJson(usageOf(R"({"prompt_tokens":71,"completion_tokens":31,"total_tokens":102})"), R"({"prompt_tokens":71,"completion_tokens":31,"total_tokens":102})"));
  // An explicit provider zero is a measurement, so it is relayed as one.
  EXPECT(sameJson(usageOf(R"({"prompt_tokens":71,"completion_tokens":31,"total_tokens":102,"prompt_tokens_details":{"cached_tokens":0}})"),
                  R"({"prompt_tokens":71,"completion_tokens":31,"total_tokens":102,"prompt_tokens_details":{"cached_tokens":0}})"));
}

TEST("cerebras: a counter larger than the total it is a subset of is dropped as unreadable") {
  EXPECT(sameJson(usageOf(R"({"prompt_tokens":71,"completion_tokens":31,"total_tokens":102,"prompt_tokens_details":{"cached_tokens":72}})"),
                  R"({"prompt_tokens":71,"completion_tokens":31,"total_tokens":102})"));
  EXPECT(sameJson(usageOf(R"({"prompt_tokens":71,"completion_tokens":31,"total_tokens":102,"completion_tokens_details":{"reasoning_tokens":32}})"),
                  R"({"prompt_tokens":71,"completion_tokens":31,"total_tokens":102})"));
}
