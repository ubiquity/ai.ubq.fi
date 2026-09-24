// Chat completion to Responses payload rendering, split out of src/deepseek_responses.ts.

import { type ChatOnlyResponsesProfile, DEEPSEEK_RESPONSES_PROFILE, originalToolName } from "./responses.ts";
import { type DeepSeekFinishDisposition } from "./index.ts";
import { getString, isRecord } from "../utils.ts";

const responseMessageContent = (text: string, refusal: string): Record<string, unknown>[] => [
  ...(text ? [{ type: "output_text", text, annotations: [] }] : []),
  ...(refusal ? [{ type: "refusal", refusal }] : []),
];

const responseMessageItem = (id: string, text: string, refusal = ""): Record<string, unknown> => ({
  id,
  type: "message",
  status: "completed",
  role: "assistant",
  content: responseMessageContent(text, refusal),
});

export const reasoningItem = (id: string, text: string): Record<string, unknown> => ({
  id,
  type: "reasoning",
  status: "completed",
  summary: [{ type: "summary_text", text }],
});

export const functionCallItem = (id: string, callId: string, name: string, args: string): Record<string, unknown> => ({
  id,
  type: "function_call",
  status: "completed",
  call_id: callId,
  name,
  arguments: args,
});

/** Codex's freeform tool item; the text travels in `input`, not JSON arguments. */
export const customToolCallItem = (id: string, callId: string, name: string, input: string): Record<string, unknown> => ({
  id,
  type: "custom_tool_call",
  status: "completed",
  call_id: callId,
  name,
  input,
});

/**
 * Unwraps the single freeform `input` parameter back into the tool's raw text.
 * The provider answers with JSON arguments because Chat Completions has no
 * freeform tool shape, and the client expects the original text.
 */
export const freeformInputFromArguments = (args: string): string => {
  try {
    const parsed: unknown = JSON.parse(args);
    if (typeof parsed === "string") return parsed;
    if (isRecord(parsed) && !Array.isArray(parsed)) {
      const input = getString(parsed.input);
      if (input !== null) return input;
    }
  } catch {
    // Not JSON: the arguments already are the freeform input.
  }
  return args;
};

/**
 * Maps one Chat Completions usage object onto the Responses usage shape.
 *
 * The counters are the reason this is not a field-by-field copy: Codex reads
 * `input_tokens_details.cached_tokens` and `output_tokens_details.
 * reasoning_tokens`, and each provider publishes those measurements under its
 * own names (DeepSeek `prompt_cache_hit_tokens` / `completion_tokens_details.
 * reasoning_tokens`; LithosAI `prompt_tokens_details.cached_tokens` /
 * `completion_tokens_details.reasoning_tokens`), so the profile's counters are
 * the ones consulted. A counter the upstream did not report leaves its detail
 * object absent, so the client and the gateway telemetry both read an unknown
 * value instead of a measured zero.
 */
export const toResponsesUsage = (value: unknown, profile: ChatOnlyResponsesProfile = DEEPSEEK_RESPONSES_PROFILE): Record<string, unknown> | null => {
  if (!isRecord(value) || Array.isArray(value)) return null;
  const inputTokens = typeof value.prompt_tokens === "number" ? value.prompt_tokens : null;
  const outputTokens = typeof value.completion_tokens === "number" ? value.completion_tokens : null;
  if (inputTokens === null || outputTokens === null) return null;
  const totalTokens = typeof value.total_tokens === "number" ? value.total_tokens : inputTokens + outputTokens;
  const cachedTokens = profile.cachedPromptTokens(value, inputTokens);
  const reasoningTokens = profile.reasoningTokens(value, outputTokens);
  return {
    input_tokens: inputTokens,
    ...(cachedTokens === null ? {} : { input_tokens_details: { cached_tokens: cachedTokens } }),
    output_tokens: outputTokens,
    ...(reasoningTokens === null ? {} : { output_tokens_details: { reasoning_tokens: reasoningTokens } }),
    total_tokens: totalTokens,
  };
};

export type DeepSeekResponsesEcho = Readonly<{
  tools: unknown;
  tool_choice: unknown;
  parallel_tool_calls: unknown;
  instructions: string | null;
}>;

/** Fields Codex reads back off a Responses object; the request is echoed verbatim. */
export const responsesEnvelope = (
  responseId: string,
  requestedModel: string,
  createdAtSeconds: number,
  status: string,
  echo: DeepSeekResponsesEcho
): Record<string, unknown> => ({
  id: responseId,
  object: "response",
  created_at: createdAtSeconds,
  status,
  model: requestedModel,
  output: [],
  error: null,
  incomplete_details: null,
  instructions: echo.instructions,
  metadata: {},
  parallel_tool_calls: typeof echo.parallel_tool_calls === "boolean" ? echo.parallel_tool_calls : true,
  tool_choice: echo.tool_choice ?? "auto",
  tools: Array.isArray(echo.tools) ? echo.tools : [],
  temperature: null,
  top_p: null,
  max_output_tokens: null,
  previous_response_id: null,
  reasoning: null,
  store: false,
  truncation: "disabled",
  usage: null,
});

/**
 * The terminal event and status a DeepSeek `finish_reason` implies.
 *
 * The provider reports one vocabulary for both the Chat endpoint and its native
 * Responses endpoint; the official Responses schema defines only
 * `max_output_tokens`, `max_messages`, `content_filter` and `steered` as
 * `incomplete_details.reason` values, so the two incomplete-capable reasons map
 * onto the schema's own names. A resource interruption and an interruption of
 * unspecified cause are reported as a failed terminal rather than a clean
 * completion, because neither leaves a usable answer.
 */
export const deepSeekTerminalTypeForDisposition = (
  disposition: DeepSeekFinishDisposition
): "response.completed" | "response.incomplete" | "response.failed" => {
  if (disposition.kind === "completed") return "response.completed";
  if (disposition.kind === "incomplete") return "response.incomplete";
  return "response.failed";
};

/**
 * Builds the terminal Responses object for one DeepSeek finish disposition.
 * Only a completed disposition reports `completed`; an incomplete disposition
 * carries `incomplete_details.reason`, and every other disposition names the
 * provider's own reason as the error code so the cause stays visible.
 */
export const deepSeekTerminalEnvelope = (
  responseId: string,
  requestedModel: string,
  createdAtSeconds: number,
  status: string,
  echo: DeepSeekResponsesEcho,
  finishReason: unknown,
  profile: ChatOnlyResponsesProfile = DEEPSEEK_RESPONSES_PROFILE
): Readonly<{ type: "response.completed" | "response.incomplete" | "response.failed"; response: Record<string, unknown> }> => {
  const disposition = profile.finishDisposition(finishReason);
  const terminalType = deepSeekTerminalTypeForDisposition(disposition);
  if (disposition.kind === "completed") {
    return { type: terminalType as "response.completed", response: responsesEnvelope(responseId, requestedModel, createdAtSeconds, status, echo) };
  }
  if (disposition.kind === "incomplete") {
    const response = responsesEnvelope(responseId, requestedModel, createdAtSeconds, "incomplete", echo);
    response.incomplete_details = { reason: disposition.reason };
    return { type: "response.incomplete", response };
  }
  const code = disposition.kind === "failed" ? disposition.code : `unrecognized_finish_reason:${disposition.value}`;
  const response = responsesEnvelope(responseId, requestedModel, createdAtSeconds, "failed", echo);
  response.error = { code, message: `${profile.label} stopped generating: ${code}` };
  return { type: "response.failed", response };
};

/** Output items for one Chat choice: reasoning, message, then any tool calls. */
const outputItemsForChoice = (
  message: Record<string, unknown>,
  choiceIndex: number,
  responseId: string,
  toolNames: ReadonlyMap<string, string>,
  customToolNames: ReadonlySet<string>
): Record<string, unknown>[] => {
  const items: Record<string, unknown>[] = [];
  if (typeof message.reasoning_content === "string" && message.reasoning_content) {
    items.push(reasoningItem(`${responseId}_rs_${choiceIndex}`, message.reasoning_content));
  }
  const text = typeof message.content === "string" ? message.content : "";
  const refusal = typeof message.refusal === "string" ? message.refusal : "";
  if (text || refusal) items.push(responseMessageItem(`${responseId}_msg_${choiceIndex}`, text, refusal));
  const toolCalls = Array.isArray(message.tool_calls) ? message.tool_calls : [];
  for (const [callIndex, call] of toolCalls.entries()) {
    if (!isRecord(call) || Array.isArray(call) || !isRecord(call.function) || Array.isArray(call.function)) continue;
    const callId = getString(call.id) ?? `${responseId}_call_${callIndex}`;
    const chatName = getString(call.function.name) ?? "";
    const name = originalToolName(chatName, toolNames);
    const args = typeof call.function.arguments === "string" ? call.function.arguments : "";
    if (customToolNames.has(chatName)) {
      items.push(customToolCallItem(`${responseId}_ctc_${choiceIndex}_${callIndex}`, callId, name, freeformInputFromArguments(args)));
      continue;
    }
    items.push(functionCallItem(`${responseId}_fc_${choiceIndex}_${callIndex}`, callId, name, args));
  }
  return items;
};

/**
 * Builds the buffered Responses object for a completed Chat completion under
 * one provider profile. Chat tool calls become `function_call` output items,
 * and the provider's `reasoning_content` becomes a `reasoning` item so nothing
 * is silently lost.
 */
export const toDeepSeekResponsesPayload = (
  completion: Record<string, unknown>,
  requestedModel: string,
  responseId: string,
  echo: DeepSeekResponsesEcho,
  toolNames: ReadonlyMap<string, string> = new Map(),
  customToolNames: ReadonlySet<string> = new Set(),
  profile: ChatOnlyResponsesProfile = DEEPSEEK_RESPONSES_PROFILE
): Record<string, unknown> => {
  const created = typeof completion.created === "number" ? completion.created : Math.floor(Date.now() / 1000);
  const choices = Array.isArray(completion.choices) ? completion.choices : [];
  // A buffered completion still carries `finish_reason`, so the same mapping
  // applies: a single-choice truncation must not be returned as a completed
  // response just because the transport delivered the whole body.
  const firstChoice = choices.find((choice) => isRecord(choice) && !Array.isArray(choice));
  const finishReason = isRecord(firstChoice) && !Array.isArray(firstChoice) ? firstChoice.finish_reason : undefined;
  const payload = deepSeekTerminalEnvelope(responseId, requestedModel, created, "completed", echo, finishReason, profile).response;
  const output: Record<string, unknown>[] = [];
  for (const [index, choice] of choices.entries()) {
    if (!isRecord(choice) || Array.isArray(choice) || !isRecord(choice.message) || Array.isArray(choice.message)) continue;
    output.push(...outputItemsForChoice(choice.message, index, responseId, toolNames, customToolNames));
  }
  payload.output = output;
  payload.usage = toResponsesUsage(completion.usage, profile);
  return payload;
};
