#pragma once

#include "uos/rt/limits.hpp"

#include <ranges>
#include <string_view>

namespace uos::rt::detail {
  inline bool boundedUrlQuery(std::string_view query) {
    if (query.starts_with('?')) query.remove_prefix(1);
    if (query.size() > limits::kUrlQueryBytes) return false;
    std::size_t count = 0;
    for (const auto part : query | std::views::split('&')) {
      if (!part.empty() && ++count > limits::kUrlQueryPairs) return false;
    }
    return true;
  }
}
