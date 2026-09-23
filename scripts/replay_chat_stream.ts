// Recorded DeepSeek Chat stream replay, split out of scripts/replay.ts.

import { normalizeDeepSeekChatCompletion } from "../src/deepseek.ts";
import { type DeepSeekStreamFrame, DeepSeekStreamError, iterateDeepSeekChatCompletionStream } from "../src/deepseek_stream.ts";
import { isAnswerBearingCompletion } from "../src/upstream_wire.ts";
import { isPlainRecord, parseJson, unavailable } from "./replay.ts";

type ChatStreamEvidence = {
  normalizedChunks: number;
  doneFrames: number;
  malformed: boolean;
  failureKind: string | null;
  answerBearing: boolean;
  terminalKind: "completed" | "premature_eof" | "read_error" | "other_failure" | "unavailable";
};

const emptyChatStreamEvidence = (): ChatStreamEvidence => ({
  normalizedChunks: 0,
  doneFrames: 0,
  malformed: false,
  failureKind: null,
  answerBearing: false,
  terminalKind: "unavailable",
});

/** Record one normalized choice's answer-bearing contribution without reimplementing the rule. */
const observeChatChoice = (choice: unknown, evidence: ChatStreamEvidence): void => {
  if (!isPlainRecord(choice) || !isPlainRecord(choice.delta)) return;
  const toolCalls = Array.isArray(choice.delta.tool_calls) ? choice.delta.tool_calls.length : 0;
  if (
    isAnswerBearingCompletion({
      text: typeof choice.delta.content === "string" ? choice.delta.content : "",
      refusal: typeof choice.delta.refusal === "string" ? choice.delta.refusal : "",
      toolCallCount: toolCalls,
    })
  ) {
    evidence.answerBearing = true;
  }
};

/** Record one validated DeepSeek stream frame; comments stay non-terminal and uncounted. */
const observeChatFrame = (frame: DeepSeekStreamFrame, evidence: ChatStreamEvidence): void => {
  if (frame.kind === "done") {
    evidence.doneFrames += 1;
    evidence.terminalKind = "completed";
    return;
  }
  if (frame.kind !== "chunk") return;
  evidence.normalizedChunks += 1;
  const choices = Array.isArray(frame.value.choices) ? frame.value.choices : [];
  for (const choice of choices) observeChatChoice(choice, evidence);
};

/** The fixed terminal kind recorded for one DeepSeek stream failure kind. */
const chatFailureTerminalKind = (kind: string): ChatStreamEvidence["terminalKind"] => {
  if (kind === "premature_eof") return "premature_eof";
  if (kind === "read_error") return "read_error";
  return "other_failure";
};

/** Record why the real DeepSeek stream iterator stopped. */
const noteChatStreamFailure = (error: unknown, evidence: ChatStreamEvidence): void => {
  if (!(error instanceof DeepSeekStreamError)) {
    evidence.terminalKind = "other_failure";
    return;
  }
  evidence.failureKind = error.kind;
  evidence.terminalKind = chatFailureTerminalKind(error.kind);
  if (error.kind === "malformed_event" || error.kind === "invalid_chunk") evidence.malformed = true;
};

/**
 * The real DeepSeek Chat Completions stream consumer: the gateway's own SSE
 * iterator parses and normalizes every frame, and the gateway's own
 * answer-bearing rule decides whether the accumulated output is usable.
 * Nothing here reimplements either.
 */
const consumeDeepSeekChatStream = async (response: Response, model: string): Promise<ChatStreamEvidence> => {
  const evidence = emptyChatStreamEvidence();
  if (!response.body) unavailable();
  try {
    for await (const frame of iterateDeepSeekChatCompletionStream(response, model)) {
      observeChatFrame(frame, evidence);
    }
  } catch (error) {
    noteChatStreamFailure(error, evidence);
  }
  return evidence;
};

/** The buffered DeepSeek Chat Completions consumer: the real normalizer plus the real validity rule. */
const consumeDeepSeekBufferedChat = async (response: Response, model: string): Promise<ConversationOutcome> => {
  const parsed = parseJson(await response.text());
  const normalized = normalizeDeepSeekChatCompletion(parsed, model);
  if (!normalized.ok) return "invalid_completion";
  const choices = Array.isArray(normalized.value.choices) ? normalized.value.choices : [];
  const answerBearing = choices.some((choice) => {
    if (!isPlainRecord(choice) || !isPlainRecord(choice.message)) return false;
    const toolCalls = Array.isArray(choice.message.tool_calls) ? choice.message.tool_calls.length : 0;
    return isAnswerBearingCompletion({
      text: typeof choice.message.content === "string" ? choice.message.content : "",
      refusal: typeof choice.message.refusal === "string" ? choice.message.refusal : "",
      toolCallCount: toolCalls,
    });
  });
  return answerBearing ? "completed" : "empty_completion";
};

/** Buffered Chat Completions verdict from the real normalizer and validity rule. */
type ConversationOutcome = "completed" | "invalid_completion" | "empty_completion";

export { consumeDeepSeekBufferedChat, consumeDeepSeekChatStream };
