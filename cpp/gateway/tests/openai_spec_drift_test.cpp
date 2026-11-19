// Port of tests/openai-spec-drift.test.ts, limited to the assertions on
// src/openai-schema.ts. Everything else in that file tests
// scripts/check-openai-spec-drift.ts (pin parsing, surface extraction, the CI
// verification flows), a TypeScript tool that is not part of the C++ port; that
// includes this case's `checkRuntimeAgainstPin(pin)` assertion, and the pin is read
// here directly instead of through the script's `readOpenAiSpecPin`.
#include "uos/openai_schema.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;

  std::vector<std::string> sorted(std::vector<std::string> values) {
    rt::sort(values, [](const std::string& left, const std::string& right) { return rt::localeCompare(left, right); });
    return values;
  }

  template <class Keys>
  std::vector<std::string> strings(const Keys& keys) {
    std::vector<std::string> out;
    for (const auto key : keys) out.emplace_back(key);
    return out;
  }

  std::vector<std::string> surface(const json::object& surfaces, std::string_view name) {
    std::vector<std::string> out;
    const json::array* values = rt::getArray(surfaces, name);
    if (!values) return out;
    for (const json::value& value : *values) out.emplace_back(rt::getString(value).value_or("<not a string>"));
    return out;
  }
}

TEST("the runtime request allowlists equal the pinned official surfaces") {
  const std::string pinPath = rt::path::resolve(rt::path::dirname(__FILE__), "fixtures/openai-spec-pin.json");
  const json::value pin = rt::jsonParse(rt::fs::readFileSync(pinPath, "utf8"));
  const json::object* surfaces = rt::getObject(pin, "surfaces");
  ASSERT(surfaces != nullptr);
  EXPECT_EQ(sorted(surface(*surfaces, "CreateChatCompletionRequest")), sorted(strings(uos::openai_schema::CHAT_COMPLETIONS_REQUEST_KEYS)));
  EXPECT_EQ(sorted(surface(*surfaces, "CreateResponse")), sorted(strings(uos::openai_schema::RESPONSES_REQUEST_KEYS)));
}
