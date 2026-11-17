#pragma once
// Port of src/defaults.ts.
#include "uos/api_keys.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>

namespace uos::defaults {
  using ReasoningEffort = std::string;

  inline const ReasoningEffort DEFAULT_REASONING_EFFORT = "medium";
  inline constexpr double DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS = api_keys::API_KEY_NO_USAGE_LIMIT;
  inline constexpr double DEFAULT_KERNEL_POLICY_WINDOW_MS = api_keys::USAGE_RESET_PERIOD_MS;

  inline const rt::KvKey DEFAULT_MODEL_KEY{"default", "model"};
  inline const rt::KvKey DEFAULT_REASONING_EFFORT_KEY{"default", "reasoning_effort"};
  inline const rt::KvKey DEFAULT_KERNEL_POLICY_LIMIT_KEY{"default", "kernel_policy_limit_requests"};
  inline const rt::KvKey DEFAULT_KERNEL_POLICY_WINDOW_KEY{"default", "kernel_policy_window_ms"};

  // `value: unknown`: nullptr is undefined.
  std::optional<ReasoningEffort> normalizeReasoningEffort(const json::value* value);
  std::optional<ReasoningEffort> normalizeReasoningEffort(const json::value& value);
}
