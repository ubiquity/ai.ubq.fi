#pragma once
#include <array>
#include <string_view>

namespace uos::openai_schema {
  extern const std::array<std::string_view, 37> CHAT_COMPLETIONS_REQUEST_KEYS;

  extern const std::array<std::string_view, 31> RESPONSES_REQUEST_KEYS;
}
