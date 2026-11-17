#pragma once

#include "uos/rt/json.hpp"

namespace uos::rt {
  // Route-specific decoded byte allowance for one complete JSON document.
  json::value jsonParseWithLimit(std::string_view text, std::size_t maxDocumentBytes);
}
