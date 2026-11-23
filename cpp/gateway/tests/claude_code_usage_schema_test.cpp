// Differential cases for src/provider/claude-code-usage-schema.ts recorded under Bun 1.4.0
// (tests/claude_code_usage_schema_vectors.inc); tests/claude_usage_meters_test.cpp ports the bun test.
#include "uos/provider/claude_code_usage_schema.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

#include <limits>
#include <optional>
#include <string>

#include "claude_code_usage_schema_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::provider::claude_code_usage_schema;

namespace {
  std::string show(const std::optional<json::object>& value) { return value ? rt::jsonStringify(json::value(*value)) : "null"; }
  std::string expectedText(const json::value& value) { return value.is_null() ? "null" : std::string(value.get_string()); }
  const double Infinity = std::numeric_limits<double>::infinity();
}

TEST("differential: parseClaudeCodeUsageSnapshot over payloads and clocks") {
  const json::value vectors = rt::jsonParse(kUsageVectors);
  const json::array& payloads = *rt::getArray(vectors, "payloads");
  const json::array& ats = *rt::getArray(vectors, "ats");
  const json::array& snapshots = *rt::getArray(vectors, "snapshots");
  ASSERT_EQ(payloads.size(), snapshots.size());
  for (std::size_t index = 0; index < payloads.size(); ++index) {
    json::value payload = payloads[index].is_null() ? json::value(nullptr) : rt::jsonParse(std::string(payloads[index].get_string()));
    if (index == 21) payload = json::object{{"five_hour", json::object{{"utilization", Infinity}, {"resets_at", nullptr}}}};
    for (std::size_t at = 0; at < ats.size(); ++at) {
      const std::optional<json::object> actual = parseClaudeCodeUsageSnapshot(payload, *rt::getNumber(ats[at]));
      EXPECT_EQ(show(actual), expectedText(snapshots[index].get_array()[at]));
    }
  }
  EXPECT_EQ(parseClaudeCodeUsageSnapshot(static_cast<const json::value*>(nullptr), 1), std::nullopt);
}

TEST("differential: readClaudeCodeUsageSnapshot") {
  const json::value vectors = rt::jsonParse(kUsageVectors);
  const std::vector<json::value> inputs = {
    rt::jsonParse(R"({"v":1,"observed_at_ms":1800000000000,"payload":{"five_hour":{"utilization":1,"resets_at":null}},"meters":[]})"),
    rt::jsonParse(R"({"v":1,"observed_at_ms":"1","payload":{"five_hour":{"utilization":1,"resets_at":null}}})"),
    rt::jsonParse(R"({"v":2,"observed_at_ms":5,"payload":{"five_hour":{"utilization":1,"resets_at":null}}})"),
    rt::jsonParse(R"({"v":1,"observed_at_ms":5.5,"payload":{"five_hour":{"utilization":1,"resets_at":null}}})"),
    rt::jsonParse(R"({"v":1,"observed_at_ms":5})"),
    json::array{},
    nullptr,
  };
  const json::array& reads = *rt::getArray(vectors, "reads");
  ASSERT_EQ(inputs.size(), reads.size());
  for (std::size_t index = 0; index < inputs.size(); ++index) EXPECT_EQ(show(readClaudeCodeUsageSnapshot(inputs[index])), expectedText(reads[index]));
  EXPECT_EQ(readClaudeCodeUsageSnapshot(static_cast<const json::value*>(nullptr)), std::nullopt);
}

TEST("differential: readClaudeCodeUsageMeter") {
  const json::value vectors = rt::jsonParse(kUsageVectors);
  const json::array& inputs = *rt::getArray(vectors, "meterInputs");
  const json::array& meters = *rt::getArray(vectors, "meters");
  ASSERT_EQ(inputs.size(), meters.size());
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    json::value input = rt::jsonParse(std::string(inputs[index].get_string()));
    if (index == 2) input.get_object()["used_percent"] = Infinity;
    EXPECT_EQ(show(readClaudeCodeUsageMeter(input)), expectedText(meters[index]));
  }
}
