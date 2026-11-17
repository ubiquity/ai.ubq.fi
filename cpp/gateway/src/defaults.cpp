#include "uos/defaults.hpp"

#include "uos/rt/string.hpp"

namespace uos::defaults {
  std::optional<ReasoningEffort> normalizeReasoningEffort(const json::value* value) {
    if (value == nullptr || !value->is_string()) return std::nullopt;
    const json::string& text = value->get_string();
    std::string normalized = rt::trim(std::string_view(text.data(), text.size()));
    if (normalized.empty()) return std::nullopt;
    return normalized;
  }

  std::optional<ReasoningEffort> normalizeReasoningEffort(const json::value& value) { return normalizeReasoningEffort(&value); }
}
