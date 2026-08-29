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

import type {
  AssistantTurn,
  ConversationTurn,
  NormalizedAssistantResponse,
  ToolCall,
  ToolResultTurn,
} from "./types.ts";

export type Conversation = Readonly<{ turns: readonly ConversationTurn[] }>;

export const createConversation = (turns: readonly ConversationTurn[] = []): Conversation => ({ turns });

export const appendTurn = (conversation: Conversation, turn: ConversationTurn): Conversation => ({
  turns: [...conversation.turns, turn],
});

export const appendUser = (conversation: Conversation, content: string): Conversation =>
  appendTurn(conversation, { role: "user", content });

export const appendToolResult = (
  conversation: Conversation,
  toolCallId: string,
  name: string,
  content: string,
): Conversation => {
  const result: ToolResultTurn = { role: "tool", toolCallId, name, content };
  return appendTurn(conversation, result);
};

/** Appends one normalized assistant turn produced by the adapter. */
export const advanceConversation = (
  conversation: Conversation,
  response: NormalizedAssistantResponse,
): Conversation => {
  const turn: AssistantTurn = {
    role: "assistant",
    content: response.content,
    analysis: response.analysis,
    toolCalls: response.toolCalls,
    finishReason: response.finishReason,
  };
  return appendTurn(conversation, turn);
};

const foldToolCalls = (turns: readonly ConversationTurn[]): readonly ToolCall[] =>
  turns.flatMap((turn) => (turn.role === "assistant" ? turn.toolCalls : []));

/**
 * Returns the conversation with analysis dropped from every assistant turn
 * that precedes the last completed final answer (Harmony replay rule).
 * Analysis on the in-progress final segment is preserved.
 */
export const dropAnalysisBeforeCompletedFinal = (conversation: Conversation): Conversation => {
  let lastCompletedIndex = -1;
  for (let i = 0; i < conversation.turns.length; i += 1) {
    const turn = conversation.turns[i];
    if (turn.role === "assistant" && turn.toolCalls.length === 0 && turn.content !== null) {
      lastCompletedIndex = i;
    }
  }
  if (lastCompletedIndex === -1) return conversation;
  return {
    turns: conversation.turns.map((turn, index) => {
      if (turn.role === "assistant" && turn.analysis.length > 0 && index <= lastCompletedIndex) {
        return { ...turn, analysis: [] as const } as AssistantTurn;
      }
      return turn;
    }),
  };
};

/**
 * The OpenAI Chat Completions wire shape for the adapter's conversation view.
 * Analysis is never emitted: Cerebras rejects `reasoning_content` in request
 * history, and the plan requires replaying only the normalized final state.
 */
export const wireMessagesFromConversation = (conversation: Conversation): readonly Record<string, unknown>[] => {
  const messages: Record<string, unknown>[] = [];
  for (const turn of conversation.turns) {
    switch (turn.role) {
      case "system":
      case "developer":
      case "user":
        if (turn.content) messages.push({ role: turn.role, content: turn.content });
        break;
      case "assistant": {
        const hasCalls = turn.toolCalls.length > 0;
        if (turn.content === null && !hasCalls) break;
        const message: Record<string, unknown> = {
          role: "assistant",
          content: turn.content ?? (hasCalls ? null : ""),
        };
        if (hasCalls) {
          message.tool_calls = turn.toolCalls.map((call) => ({
            id: call.id,
            type: "function",
            function: { name: call.name, arguments: call.arguments },
          }));
        }
        messages.push(message);
        break;
      }
      case "tool": {
        const message: Record<string, unknown> = {
          role: "tool",
          tool_call_id: turn.toolCallId,
          content: turn.content,
        };
        messages.push(message);
        break;
      }
    }
  }
  return messages;
};

/** Counts analysis lines carried in the conversation state. */
export const analysisLineCount = (conversation: Conversation): number =>
  conversation.turns.reduce(
    (count, turn) => count + (turn.role === "assistant" ? turn.analysis.length : 0),
    0,
  );

/** Counts tool-call turns still waiting for a result (unfinished turns). */
export const pendingToolCallCount = (conversation: Conversation): number => {
  const calls = foldToolCalls(conversation.turns);
  const results = new Set(
    conversation.turns.filter((turn): turn is ToolResultTurn => turn.role === "tool").map((turn) => turn.toolCallId),
  );
  return calls.filter((call) => !results.has(call.id)).length;
};

/** True when the conversation contains a completed final answer. */
export const hasCompletedFinal = (conversation: Conversation): boolean =>
  conversation.turns.some(
    (turn) => turn.role === "assistant" && turn.toolCalls.length === 0 && turn.content !== null,
  );
