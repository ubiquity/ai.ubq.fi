// src/defaults.ts has no direct bun test: differential cases recorded from the TS under Bun 1.4.0.
#include "uos/defaults.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace d = uos::defaults;

TEST("differential: normalizeReasoningEffort") {
  const std::vector<std::pair<std::string, std::optional<std::string>>> strings = {
    {"", std::nullopt},
    {"  ", std::nullopt},
    {" high ", "high"},
    {"medium", "medium"},
    {" low ", "low"},
    {"​x", "​x"},
  };
  for (const auto& [input, expected] : strings) EXPECT_EQ(d::normalizeReasoningEffort(json::value(input)), expected);
  const std::vector<json::value> others = {nullptr, json::value(5), json::value(true), json::array{}, json::object{}};
  for (const json::value& value : others) EXPECT_EQ(d::normalizeReasoningEffort(value), std::nullopt);
  EXPECT_EQ(d::normalizeReasoningEffort(static_cast<const json::value*>(nullptr)), std::nullopt);
}

TEST("differential: defaults constants") {
  EXPECT_EQ(d::DEFAULT_REASONING_EFFORT, "medium");
  EXPECT_EQ(d::DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS, -1.0);
  EXPECT_EQ(d::DEFAULT_KERNEL_POLICY_WINDOW_MS, 604800000.0);
  EXPECT(d::DEFAULT_MODEL_KEY == (rt::KvKey{"default", "model"}));
  EXPECT(d::DEFAULT_REASONING_EFFORT_KEY == (rt::KvKey{"default", "reasoning_effort"}));
  EXPECT(d::DEFAULT_KERNEL_POLICY_LIMIT_KEY == (rt::KvKey{"default", "kernel_policy_limit_requests"}));
  EXPECT(d::DEFAULT_KERNEL_POLICY_WINDOW_KEY == (rt::KvKey{"default", "kernel_policy_window_ms"}));
}
