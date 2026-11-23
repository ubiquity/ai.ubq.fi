#pragma once
// Port of src/provider/surplus-claude-request.ts.
// `JsonRecord` (`Record<string, unknown>`) is json::object.
#include "uos/openai_telemetry.hpp"
#include "uos/provider/anthropic_replay.hpp"
#include "uos/provider/anthropic_response_wire.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::provider::surplus_claude_request {
  struct SurplusClaudeRequest {
    json::object body;
    openai_telemetry::PromptCacheMode promptCacheMode;
    double explicitBreakpointCount;
    // ReadonlyMap<string, ClaudeToolName>, in insertion order.
    rt::OrderedMap<std::string, anthropic_response_wire::ClaudeToolName> toolNames;
  };

  // `Readonly<{ type: "original"; item: JsonRecord }> | Readonly<{ type: "native_assistant"; blocks: JsonRecord[] }>`:
  // the same shape as anthropic-replay's NativeReplayEntry, which restoreAnthropicReplay returns
  // and this module consumes as ClaudeNativeReplayEntry, so the C++ type is that variant.
  using ClaudeNativeReplayEntry = anthropic_replay::NativeReplayEntry;

  struct SurplusClaudeRequestOptions {
    std::optional<std::string> provider{};  // "surplus" | "metered" | "claude_code"
    std::optional<std::string> reasoningReplayScope{};
    /** A provider-specific contract proof is required before translating an explicit effort. */
    std::optional<bool> supportsNativeReasoningEffort{};
    /** Independent, exact-model proof that native disabled thinking is supported. */
    std::optional<bool> supportsNativeDisabledThinking{};
    /** Exact observed effort names for a generation-bound direct native account, not reseller inference. */
    std::optional<std::vector<std::string>> nativeReasoningEfforts{};
    /** The direct Claude lane omits Codex's unsupported hosted web-search declaration instead of rejecting the turn. */
    std::optional<bool> omitHostedWebSearch{};
    friend bool operator==(const SurplusClaudeRequestOptions&, const SurplusClaudeRequestOptions&) = default;
  };

  // `(model: unknown): model is string`: nullptr is undefined. The std::string_view overload is a
  // known string; the array overload takes a string literal, which would otherwise be ambiguous
  // between the json::value and std::string_view overloads.
  bool isSurplusClaudeModel(const json::value* model);
  bool isSurplusClaudeModel(const json::value& model);
  bool isSurplusClaudeModel(std::string_view model);
  template <std::size_t N>
  bool isSurplusClaudeModel(const char (&model)[N]) {
    return isSurplusClaudeModel(std::string_view(model, N - 1));
  }

  /**
   * Claude's Surplus Responses bridge drops cache policy, while native Messages
   * preserves explicit block markers. Return null instead of dropping an input
   * or a control that has no lossless native representation.
   */
  // `catalogMaxOutputTokens?: number` (std::nullopt is undefined), `options = {}`. std::nullopt is null.
  std::optional<SurplusClaudeRequest> buildSurplusClaudeRequest(const json::object& body);
  std::optional<SurplusClaudeRequest> buildSurplusClaudeRequest(const json::object& body, std::optional<double> catalogMaxOutputTokens);
  std::optional<SurplusClaudeRequest> buildSurplusClaudeRequest(const json::object& body, std::optional<double> catalogMaxOutputTokens,
                                                                const SurplusClaudeRequestOptions& options);
}
