#pragma once
// Port of src/provider/openrouter-request-controls.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>

namespace uos::provider::openrouter_request_controls {
  inline const rt::OrderedSet<std::string> OPENROUTER_CLAUDE_REQUEST_EXTENSION_KEYS{"cache_control"};
  rt::Task<std::optional<rt::OrderedSet<std::string>>> openRouterPromptCacheRequestExtensions(const json::object& body);
  std::optional<rt::Response> validateOpenRouterPromptCacheRequestControl(const json::object& body);
}
