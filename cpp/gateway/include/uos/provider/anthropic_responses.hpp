#pragma once
// Port of src/provider/anthropic-responses.ts.
// Native Anthropic Messages output translated into the public Responses contract.
#include "uos/provider/anthropic_response_wire.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>

namespace uos::provider::anthropic_responses {
  struct AnthropicResponsesOptions {
    std::string model;
    std::optional<std::string> requestId = std::nullopt;
    std::optional<rt::AbortSignal> signal = std::nullopt;
    std::optional<std::string> reasoningSummary = std::nullopt;
    std::optional<std::string> reasoningReplayScope = std::nullopt;
    std::optional<std::string> provider = std::nullopt;
    std::optional<rt::OrderedMap<std::string, anthropic_response_wire::ClaudeToolName>> toolNames = std::nullopt;
  };

  /**
   * Messages counts input_tokens after subtracting cache hits and cache writes.
   * Responses counts the whole input, with caching as a subset. Optional native
   * counters remain absent when unreported; an invalid counter never becomes 0.
   * The original buyer cost is retained for authoritative paid-route settlement.
   */
  // `value: unknown` (nullptr is undefined); `JsonRecord | null`.
  std::optional<json::object> toAnthropicResponsesUsage(const json::value* value);
  std::optional<json::object> toAnthropicResponsesUsage(const json::value& value);

  /** Preserve upstream HTTP status and provenance; normalize JSON and progressive SSE. */
  [[nodiscard]] rt::Task<rt::Response> normalizeAnthropicResponsesResponse(rt::Response response, AnthropicResponsesOptions options);
}
