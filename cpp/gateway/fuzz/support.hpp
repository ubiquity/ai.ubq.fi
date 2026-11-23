#pragma once
#include "uos/rt/error.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace uos::fuzz {
  inline constexpr std::size_t maxInput = 16 * 1024;
  inline std::string_view text(const std::uint8_t* data, std::size_t size) {
    return {reinterpret_cast<const char*>(data), size};
  }
  // A grammar's documented JavaScript exception is an ordinary rejected input.
  // All other C++ exceptions, sanitizer failures and signals remain fatal.
  template <class Function> void malformed(Function&& function) {
    try {
      function();
    } catch (const rt::JsError&) {
    }
  }
}
