#pragma once

#include "uos/rt/json.hpp"

namespace uos::codex::banked_reset::record_fields {
  // `value === null` on a member that may be absent.
  inline bool isNull(const json::value* value) { return value != nullptr && value->is_null(); }

  // A member already proven to be a string or null.
  inline std::optional<std::string> textOrNull(const json::value* value) {
    if (isNull(value)) return std::nullopt;
    return std::string(value->get_string());
  }

  // A member already proven to be a number or null.
  inline std::optional<double> numberOrNull(const json::value* value) {
    if (isNull(value)) return std::nullopt;
    return rt::toNumber(*value);
  }

  inline std::string text(const json::value* value) { return std::string(value->get_string()); }

  // `value.v !== 1`
  inline bool versionIsNotOne(const json::value& value) {
    const json::value* version = rt::get(value, "v");
    return version == nullptr || !rt::isNumber(*version) || rt::toNumber(*version) != 1;
  }
}
