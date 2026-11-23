#pragma once
// Port of src/provider/anthropic-chat-responses.ts.
// Claude's Chat transport preserves explicit effort and provider prompt caching.
// Public output uses standard Responses items; signed Chat replay stays private.
#include "uos/provider/anthropic_responses.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>

namespace uos::provider::anthropic_chat_responses {
  // AnthropicResponsesOptions & Readonly<{ toolNames?: ReadonlyMap<string, ClaudeToolName>; customToolNames?: ReadonlySet<string> }>
  // (toolNames is the inherited member).
  struct AnthropicChatResponsesOptions : anthropic_responses::AnthropicResponsesOptions {
    std::optional<rt::OrderedSet<std::string>> customToolNames = std::nullopt;
  };

  /** Chat already reports gross input: never add cache-read or cache-write again. */
  // `usage: unknown` (nullptr is undefined); `JsonRecord | null`.
  std::optional<json::object> toAnthropicChatResponsesUsage(const json::value* usage);
  std::optional<json::object> toAnthropicChatResponsesUsage(const json::value& usage);

  /** Normalize the proven Claude Chat route while preserving status and cost headers. */
  [[nodiscard]] rt::Task<rt::Response> normalizeAnthropicChatResponsesResponse(rt::Response response, AnthropicChatResponsesOptions options);
}
