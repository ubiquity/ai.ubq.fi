#pragma once
// Port of src/codex/responses-error.ts.
#include "uos/responses_stream.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <vector>

namespace uos::codex::responses_error {
  /** Codex ignores generic context errors; keep its response failure classification visible. */
  // `responseId: string | null`.
  responses_stream::ResponsesStreamEvent codexContextFailureEvent(const responses_stream::ResponsesStreamEvent& event, const std::optional<std::string>& responseId,
                                                                  const json::object& responseTemplate, const std::vector<json::object>& visibleOutput);
}
