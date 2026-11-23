#pragma once
// Port of src/provider/surplus-claude-chat-request.ts.
// `JsonRecord` (`Record<string, unknown>`) is json::object.
#include "uos/deepseek/responses.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>

namespace uos::provider::surplus_claude_chat_request {
  struct SurplusClaudeChatRequest {
    json::object body;
    openai_telemetry::PromptCacheMode promptCacheMode;
    double explicitBreakpointCount;
    // ReadonlyMap<string, OriginalToolName> / ReadonlySet<string>, in insertion order.
    rt::OrderedMap<std::string, deepseek::responses::OriginalToolName> toolNames;
    rt::OrderedSet<std::string> customToolNames;
  };

  // `Readonly<{ provider?: string; reasoningReplayScope?: string }>`
  struct ClaudeChatRequestOptions {
    std::optional<std::string> provider{};
    std::optional<std::string> reasoningReplayScope{};
    friend bool operator==(const ClaudeChatRequestOptions&, const ClaudeChatRequestOptions&) = default;
  };

  /** Claude's canonical Chat route preserves both explicit block caching and requested effort. */
  // `catalogMaxOutputTokens?: number` (std::nullopt is undefined), `options = {}`. std::nullopt is null.
  std::optional<SurplusClaudeChatRequest> buildSurplusClaudeChatRequest(const json::object& body);
  std::optional<SurplusClaudeChatRequest> buildSurplusClaudeChatRequest(const json::object& body, std::optional<double> catalogMaxOutputTokens);
  std::optional<SurplusClaudeChatRequest> buildSurplusClaudeChatRequest(const json::object& body, std::optional<double> catalogMaxOutputTokens,
                                                                        const ClaudeChatRequestOptions& options);
}
