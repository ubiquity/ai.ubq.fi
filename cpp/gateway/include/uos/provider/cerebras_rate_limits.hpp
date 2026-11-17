#pragma once
#include <array>
#include <string_view>

namespace uos::provider::cerebras_rate_limits {
  extern const std::array<std::string_view, 12> CEREBRAS_RATE_LIMIT_HEADERS;
}
