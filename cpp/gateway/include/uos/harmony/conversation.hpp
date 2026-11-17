#pragma once
// Port of src/harmony/conversation.ts.
/**
 * Harmony conversation state machine (plan m01).
 *
 * Keeps the canonical agent conversation: user turns, assistant turns
 * (content + private analysis + normalized tool calls), and tool results.
 *
 * Analysis retention follows the Harmony guidance:
 * - the analysis (chain of thought) of an *unfinished* tool turn is preserved
 *   in state until the tool result is answered with a final message;
 * - once a completed final answer exists, analysis that precedes it is
 *   dropped from the replayable state.
 *
 * The Cerebras chat boundary rejects `reasoning_content` in request history
 * (observed upstream: HTTP 400 `messages.N.assistant.reasoning_content` is
 * unsupported), so the wire view never carries analysis; the same view is the
 * "normalized final state" the plan requires after a completed answer.
 */
#include "uos/harmony/types.hpp"
#include "uos/rt/prelude.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace uos::harmony::conversation {
  struct Conversation {
    std::vector<types::ConversationTurn> turns;
    friend bool operator==(const Conversation&, const Conversation&) = default;
  };

  // `turns = []` is the overload without it.
  Conversation createConversation();
  Conversation createConversation(std::vector<types::ConversationTurn> turns);

  Conversation appendTurn(const Conversation& conversation, types::ConversationTurn turn);

  Conversation appendUser(const Conversation& conversation, std::string content);

  Conversation appendToolResult(const Conversation& conversation, std::string toolCallId, std::string name, std::string content);

  /** Appends one normalized assistant turn produced by the adapter. */
  Conversation advanceConversation(const Conversation& conversation, const types::NormalizedAssistantResponse& response);

  /**
   * Returns the conversation with analysis dropped from every assistant turn
   * that precedes the last completed final answer (Harmony replay rule).
   * Analysis on the in-progress final segment is preserved.
   */
  Conversation dropAnalysisBeforeCompletedFinal(const Conversation& conversation);

  /**
   * The OpenAI Chat Completions wire shape for the adapter's conversation view.
   * Analysis is never emitted: Cerebras rejects `reasoning_content` in request
   * history, and the plan requires replaying only the normalized final state.
   */
  std::vector<json::object> wireMessagesFromConversation(const Conversation& conversation);

  /** Counts analysis lines carried in the conversation state. */
  std::int64_t analysisLineCount(const Conversation& conversation);

  /** Counts tool-call turns still waiting for a result (unfinished turns). */
  std::int64_t pendingToolCallCount(const Conversation& conversation);

  /** True when the conversation contains a completed final answer. */
  bool hasCompletedFinal(const Conversation& conversation);
}
