import { projectCerebrasToolSchema } from "./cerebras.ts";
import type { ResponseInputItem } from "./types.ts";
import { getString, isRecord } from "./utils.ts";

/**
 * The Cerebras GPT-OSS endpoint speaks Chat Completions.  Codex speaks
 * Responses, so this module contains only the lossless, text/tool subset that
 * the preview bridge can translate between those two wire contracts.
 */

type NormalizationResult<T> =
  | Readonly<{ ok: true; value: T }>
  | Readonly<{ ok: false; message: string; param?: string }>;

type ChatMessage = Readonly<Record<string, unknown>>;

const textFromContent = (
  content: unknown,
  param: string,
): NormalizationResult<string> => {
  if (!Array.isArray(content)) return { ok: false, message: `${param} must be an array`, param };
  const chunks: string[] = [];
  for (const [index, part] of content.entries()) {
    const partParam = `${param}[${index}]`;
    if (!isRecord(part) || Array.isArray(part)) {
      return { ok: false, message: `${partParam} must be an object`, param: partParam };
    }
    const type = getString(part.type);
    if (type !== "input_text" && type !== "output_text") {
      return {
        ok: false,
        message: `${partParam}.type is not supported by GPT-OSS; this model accepts text only`,
        param: `${partParam}.type`,
      };
    }
    if (typeof part.text !== "string") {
      return { ok: false, message: `${partParam}.text must be a string`, param: `${partParam}.text` };
    }
    chunks.push(part.text);
  }
  return { ok: true, value: chunks.join("") };
};

const functionCallToChatToolCall = (
  value: Record<string, unknown>,
  param: string,
): NormalizationResult<Record<string, unknown>> => {
  const callId = getString(value.call_id)?.trim();
  const name = getString(value.name)?.trim();
  if (!callId) return { ok: false, message: `${param}.call_id must be a non-empty string`, param: `${param}.call_id` };
  if (!name) return { ok: false, message: `${param}.name must be a non-empty string`, param: `${param}.name` };
  if (typeof value.arguments !== "string") {
    return { ok: false, message: `${param}.arguments must be a string`, param: `${param}.arguments` };
  }
  return {
    ok: true,
    value: {
      id: callId,
      type: "function",
      function: { name, arguments: value.arguments },
    },
  };
};

const functionCallOutputToChatMessage = (
  value: Record<string, unknown>,
  param: string,
): NormalizationResult<ChatMessage> => {
  const callId = getString(value.call_id)?.trim();
  if (!callId) return { ok: false, message: `${param}.call_id must be a non-empty string`, param: `${param}.call_id` };
  const output = value.output;
  if (typeof output === "string") return { ok: true, value: { role: "tool", tool_call_id: callId, content: output } };
  if (!Array.isArray(output)) {
    return { ok: false, message: `${param}.output must be a string or array`, param: `${param}.output` };
  }
  const normalized = textFromContent(output, `${param}.output`);
  if (!normalized.ok) return normalized;
  return { ok: true, value: { role: "tool", tool_call_id: callId, content: normalized.value } };
};

const appendAssistantToolCall = (
  messages: ChatMessage[],
  toolCall: Record<string, unknown>,
): void => {
  const previous = messages[messages.length - 1];
  if (previous && previous.role === "assistant") {
    const existing = Array.isArray(previous.tool_calls) ? previous.tool_calls : [];
    messages[messages.length - 1] = {
      ...previous,
      content: previous.content ?? null,
      tool_calls: [...existing, toolCall],
    };
    return;
  }
  messages.push({ role: "assistant", content: null, tool_calls: [toolCall] });
};

/** Convert normalized Responses input items into native Chat messages. */
export const responsesInputToChatMessages = (
  input: readonly ResponseInputItem[],
  instructions: string | undefined,
): NormalizationResult<ChatMessage[]> => {
  const messages: ChatMessage[] = [];
  if (instructions !== undefined) messages.push({ role: "developer", content: instructions });

  for (const [index, raw] of input.entries()) {
    const param = `input[${index}]`;
    if (!isRecord(raw) || Array.isArray(raw)) {
      return { ok: false, message: `${param} must be an object`, param };
    }
    const type = getString(raw.type);
    if (type === "message") {
      const role = getString(raw.role);
      if (role !== "user" && role !== "assistant" && role !== "developer" && role !== "system") {
        return { ok: false, message: `${param}.role is not supported by GPT-OSS`, param: `${param}.role` };
      }
      const content = textFromContent(raw.content, `${param}.content`);
      if (!content.ok) return content;
      const chatRole = role;
      const previous = messages[messages.length - 1];
      // Keep a text assistant message adjacent to a function_call item in the
      // same Chat assistant turn. This preserves the Responses output order.
      if (chatRole === "assistant" && previous?.role === "assistant" && previous.tool_calls) {
        messages.push({ role: chatRole, content: content.value });
      } else {
        messages.push({ role: chatRole, content: content.value });
      }
      continue;
    }
    if (type === "function_call") {
      const toolCall = functionCallToChatToolCall(raw, param);
      if (!toolCall.ok) return toolCall;
      appendAssistantToolCall(messages, toolCall.value);
      continue;
    }
    if (type === "function_call_output") {
      const toolMessage = functionCallOutputToChatMessage(raw, param);
      if (!toolMessage.ok) return toolMessage;
      messages.push(toolMessage.value);
      continue;
    }
    // Reasoning items are model-private state. GPT-OSS does not accept a
    // Responses reasoning item in Chat Completions, and replaying it would
    // leak hidden content across turns, so omit it deliberately.
    if (type === "reasoning") continue;
    if (type === "item_reference") {
      return {
        ok: false,
        message: `${param}.type 'item_reference' is not supported by the GPT-OSS Responses bridge`,
        param: `${param}.type`,
      };
    }
    return {
      ok: false,
      message: `${param}.type '${type ?? ""}' is not supported by the GPT-OSS Responses bridge`,
      param: `${param}.type`,
    };
  }

  if (!messages.length) messages.push({ role: "user", content: "" });
  return { ok: true, value: messages };
};

const normalizeFunctionTool = (
  value: unknown,
  param: string,
): NormalizationResult<Record<string, unknown>> => {
  if (!isRecord(value) || Array.isArray(value)) return { ok: false, message: `${param} must be an object`, param };
  if (getString(value.type) !== "function") {
    return { ok: false, message: `${param}.type is not supported by the GPT-OSS bridge`, param: `${param}.type` };
  }
  if (Object.prototype.hasOwnProperty.call(value, "function")) {
    return {
      ok: false,
      message: `${param}.function is not valid in the Responses function tool shape`,
      param: `${param}.function`,
    };
  }
  const name = getString(value.name)?.trim();
  if (!name) return { ok: false, message: `${param}.name must be a non-empty string`, param: `${param}.name` };
  const parameters = value.parameters;
  if (parameters !== undefined && (!isRecord(parameters) || Array.isArray(parameters))) {
    return { ok: false, message: `${param}.parameters must be an object`, param: `${param}.parameters` };
  }
  if (value.description !== undefined && typeof value.description !== "string") {
    return {
      ok: false,
      message: `${param}.description must be a string`,
      param: `${param}.description`,
    };
  }
  const description = getString(value.description);
  const strict = value.strict;
  if (strict !== undefined && typeof strict !== "boolean") {
    return { ok: false, message: `${param}.strict must be a boolean`, param: `${param}.strict` };
  }
  if (
    strict === true && parameters !== undefined &&
    JSON.stringify(projectCerebrasToolSchema(parameters)) !== JSON.stringify(parameters)
  ) {
    return {
      ok: false,
      message: `${param}.parameters contains constraints that Cerebras cannot enforce in strict mode`,
      param: `${param}.parameters`,
    };
  }
  return {
    ok: true,
    value: {
      type: "function",
      function: {
        name,
        ...(description === null ? {} : { description }),
        ...(parameters === undefined ? {} : { parameters }),
        ...(strict === undefined ? {} : { strict }),
      },
    },
  };
};

/** Convert Responses function tools to the nested Chat tool schema. */
export const responsesToolsToChatTools = (
  value: unknown,
): NormalizationResult<Record<string, unknown>[] | undefined> => {
  if (value === undefined) return { ok: true, value: undefined };
  if (!Array.isArray(value)) return { ok: false, message: "tools must be an array", param: "tools" };
  const tools: Record<string, unknown>[] = [];
  for (const [index, tool] of value.entries()) {
    const normalized = normalizeFunctionTool(tool, `tools[${index}]`);
    if (!normalized.ok) return normalized;
    tools.push(normalized.value);
  }
  return { ok: true, value: tools };
};

export const responsesToolChoiceToChatToolChoice = (
  value: unknown,
): NormalizationResult<unknown> => {
  if (value === undefined) return { ok: true, value: undefined };
  if (value === "auto" || value === "none" || value === "required") return { ok: true, value };
  if (!isRecord(value) || Array.isArray(value)) {
    return {
      ok: false,
      message: "tool_choice must be auto, none, required, or a function object",
      param: "tool_choice",
    };
  }
  if (getString(value.type) !== "function") {
    return { ok: false, message: "tool_choice.type must be function", param: "tool_choice.type" };
  }
  if (value.function !== undefined) {
    return {
      ok: false,
      message: "tool_choice.function is not supported; use the flat Responses function choice shape",
      param: "tool_choice.function",
    };
  }
  const name = getString(value.name)?.trim();
  if (!name) return { ok: false, message: "tool_choice.name must be a non-empty string", param: "tool_choice.name" };
  return { ok: true, value: { type: "function", function: { name } } };
};

export type CerebrasResponsesTranslation = Readonly<{
  messages: ChatMessage[];
  tools?: Record<string, unknown>[];
  toolChoice?: unknown;
}>;

export const buildCerebrasResponsesTranslation = (
  input: readonly ResponseInputItem[],
  instructions: string | undefined,
  rawRecord: Record<string, unknown>,
): NormalizationResult<CerebrasResponsesTranslation> => {
  const messages = responsesInputToChatMessages(input, instructions);
  if (!messages.ok) return messages;
  const tools = responsesToolsToChatTools(rawRecord.tools);
  if (!tools.ok) return tools;
  const toolChoice = responsesToolChoiceToChatToolChoice(rawRecord.tool_choice);
  if (!toolChoice.ok) return toolChoice;
  return { ok: true, value: { messages: messages.value, tools: tools.value, toolChoice: toolChoice.value } };
};

type ChatCompletionChoice = Readonly<{
  message?: unknown;
  finish_reason?: unknown;
}>;

export type CerebrasResponsesBody = Readonly<Record<string, unknown>>;

export type CerebrasResponseRequestMetadata = Readonly<{
  instructions?: string;
  maxOutputTokens?: number;
  parallelToolCalls?: boolean;
  reasoningEffort?: string;
  temperature?: number;
  toolChoice?: unknown;
  tools?: readonly unknown[];
  topP?: number;
  metadata?: Record<string, unknown>;
}>;

const responseUsage = (usage: unknown): Record<string, unknown> | undefined => {
  if (!isRecord(usage) || Array.isArray(usage)) return undefined;
  const inputTokens = typeof usage.prompt_tokens === "number" ? usage.prompt_tokens : null;
  const outputTokens = typeof usage.completion_tokens === "number" ? usage.completion_tokens : null;
  const totalTokens = typeof usage.total_tokens === "number" ? usage.total_tokens : null;
  if (inputTokens === null || outputTokens === null || totalTokens === null) return undefined;
  const promptDetails = isRecord(usage.prompt_tokens_details) && !Array.isArray(usage.prompt_tokens_details)
    ? usage.prompt_tokens_details
    : {};
  const completionDetails = isRecord(usage.completion_tokens_details) && !Array.isArray(usage.completion_tokens_details)
    ? usage.completion_tokens_details
    : {};
  const cachedTokens = typeof promptDetails.cached_tokens === "number" ? promptDetails.cached_tokens : 0;
  const reasoningTokens = typeof completionDetails.reasoning_tokens === "number"
    ? completionDetails.reasoning_tokens
    : 0;
  return {
    input_tokens: inputTokens,
    input_tokens_details: { cached_tokens: cachedTokens, cache_write_tokens: 0 },
    output_tokens: outputTokens,
    output_tokens_details: { reasoning_tokens: reasoningTokens },
    total_tokens: totalTokens,
  };
};

const responseMessageItem = (id: string, text: string): Record<string, unknown> => ({
  id,
  type: "message",
  role: "assistant",
  status: "completed",
  content: [{ type: "output_text", text, annotations: [] }],
});

const responseFunctionCallItem = (
  id: string,
  callId: string,
  name: string,
  args: string,
): Record<string, unknown> => ({
  id,
  type: "function_call",
  status: "completed",
  call_id: callId,
  name,
  arguments: args,
});

/** Translate one normalized Chat completion into a buffered Responses body. */
export const chatCompletionToCerebrasResponse = (
  completion: Record<string, unknown>,
  requestedModel: string,
  request: CerebrasResponseRequestMetadata = {},
): NormalizationResult<CerebrasResponsesBody> => {
  const id = getString(completion.id)?.trim();
  if (!id) return { ok: false, message: "Upstream Chat Completion is missing an id." };
  const created = typeof completion.created === "number" && Number.isFinite(completion.created)
    ? Math.trunc(completion.created)
    : Math.floor(Date.now() / 1000);
  const completed = Math.floor(Date.now() / 1000);
  if (!Array.isArray(completion.choices) || completion.choices.length === 0) {
    return { ok: false, message: "Upstream Chat Completion has no choices." };
  }
  const choice = completion.choices[0] as ChatCompletionChoice;
  if (!isRecord(choice.message) || Array.isArray(choice.message)) {
    return { ok: false, message: "Upstream Chat Completion is missing its assistant message." };
  }
  const incompleteReason = choice.finish_reason === "length"
    ? "max_output_tokens"
    : choice.finish_reason === "content_filter"
    ? "content_filter"
    : null;
  const incomplete = incompleteReason !== null;
  const message = choice.message;
  const output: Record<string, unknown>[] = [];
  const text = typeof message.content === "string" ? message.content : "";
  if (text || !Array.isArray(message.tool_calls) || message.tool_calls.length === 0) {
    output.push(responseMessageItem(`msg_${id}`, text));
  }
  if (Array.isArray(message.tool_calls)) {
    for (const [index, rawCall] of message.tool_calls.entries()) {
      if (!isRecord(rawCall) || Array.isArray(rawCall)) {
        return { ok: false, message: `Upstream tool call ${index} is not an object.` };
      }
      const callId = getString(rawCall.id)?.trim();
      const fn = isRecord(rawCall.function) && !Array.isArray(rawCall.function) ? rawCall.function : null;
      const name = getString(fn?.name)?.trim();
      if (!callId || !name || typeof fn?.arguments !== "string") {
        return { ok: false, message: `Upstream tool call ${index} is malformed.` };
      }
      output.push(responseFunctionCallItem(`fc_${callId}`, callId, name, fn.arguments));
    }
  }
  const terminalOutput = incomplete ? output.map((item) => ({ ...item, status: "incomplete" })) : output;
  const usage = responseUsage(completion.usage);
  return {
    ok: true,
    value: {
      id: `resp_${id.replace(/^chatcmpl_/, "")}`,
      object: "response",
      created_at: created,
      completed_at: completed,
      background: false,
      error: null,
      instructions: request.instructions ?? null,
      max_output_tokens: request.maxOutputTokens ?? null,
      max_tool_calls: null,
      model: requestedModel,
      status: incomplete ? "incomplete" : "completed",
      output: terminalOutput,
      output_text: text,
      parallel_tool_calls: request.parallelToolCalls ?? true,
      previous_response_id: null,
      reasoning: { effort: request.reasoningEffort ?? null, summary: null },
      service_tier: "default",
      store: false,
      temperature: request.temperature ?? 1,
      text: { format: { type: "text" } },
      tool_choice: request.toolChoice ?? "auto",
      tools: request.tools ? [...request.tools] : [],
      top_logprobs: 0,
      top_p: request.topP ?? 1,
      truncation: "disabled",
      usage: usage ?? null,
      user: null,
      metadata: request.metadata ?? {},
      incomplete_details: incomplete ? { reason: incompleteReason } : null,
    },
  };
};

/** Emit a complete, buffered Responses SSE sequence for a synthetic response. */
export const cerebrasResponseSse = (response: CerebrasResponsesBody): string => {
  const id = getString(response.id) ?? `resp_${crypto.randomUUID().replace(/-/g, "")}`;
  const events: string[] = [];
  let sequenceNumber = 0;
  const add = (value: Record<string, unknown>): void => {
    events.push(`data: ${JSON.stringify({ ...value, sequence_number: sequenceNumber })}\n\n`);
    sequenceNumber += 1;
  };
  add({
    type: "response.created",
    response: {
      ...response,
      status: "in_progress",
      completed_at: null,
      output: [],
      output_text: "",
      usage: null,
      incomplete_details: null,
    },
  });
  const output = Array.isArray(response.output) ? response.output : [];
  for (const [index, item] of output.entries()) {
    if (!isRecord(item) || Array.isArray(item)) continue;
    const inProgressItem = item.type === "message"
      ? { ...item, status: "in_progress", content: [] }
      : item.type === "function_call"
      ? { ...item, status: "in_progress", arguments: "" }
      : { ...item, status: "in_progress" };
    add({ type: "response.output_item.added", response_id: id, output_index: index, item: inProgressItem });
    if (item.type === "message" && Array.isArray(item.content)) {
      for (const [contentIndex, content] of item.content.entries()) {
        if (!isRecord(content) || getString(content.type) !== "output_text") continue;
        const text = getString(content.text) ?? "";
        add({
          type: "response.content_part.added",
          response_id: id,
          item_id: item.id,
          output_index: index,
          content_index: contentIndex,
          part: { type: "output_text", text: "", annotations: [] },
        });
        add({
          type: "response.output_text.delta",
          response_id: id,
          item_id: item.id,
          output_index: index,
          content_index: contentIndex,
          delta: text,
        });
        add({
          type: "response.output_text.done",
          response_id: id,
          item_id: item.id,
          output_index: index,
          content_index: contentIndex,
          text,
        });
        add({
          type: "response.content_part.done",
          response_id: id,
          item_id: item.id,
          output_index: index,
          content_index: contentIndex,
          part: content,
        });
      }
    }
    if (item.type === "function_call") {
      const args = getString(item.arguments) ?? "";
      add({
        type: "response.function_call_arguments.delta",
        response_id: id,
        item_id: item.id,
        output_index: index,
        delta: args,
      });
      add({
        type: "response.function_call_arguments.done",
        response_id: id,
        item_id: item.id,
        output_index: index,
        arguments: args,
      });
    }
    add({ type: "response.output_item.done", response_id: id, output_index: index, item });
  }
  add({ type: response.status === "incomplete" ? "response.incomplete" : "response.completed", response });
  return events.join("");
};
