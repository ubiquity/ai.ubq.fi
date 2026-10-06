// Measurement client: sends one frozen corpus entry through the benchmark
// gateway instance, records stage timestamps from the client side and
// classifies the terminal outcome. WIRE PROJECTION: the responses wire sends
// the corpus verbatim; the chat wire projects the same content into chat
// messages and function tools.

import type { ProviderSpec } from "./providers.ts";
import type { AttemptRecord, CorpusEntry, FailureKind, TokenUsage } from "./types.ts";

export type AttemptOptions = Readonly<{
  gateway_base: string;
  provider: ProviderSpec;
  entry: CorpusEntry;
  run_id: string;
  period: string;
  batch: number;
  attempt: number;
  first_attempt: boolean;
  timeout_ms: number;
}>;

type Json = Record<string, unknown>;

const isRecord = (value: unknown): value is Json => typeof value === "object" && value !== null && !Array.isArray(value);

const asNumber = (value: unknown): number | null => (typeof value === "number" && Number.isFinite(value) ? value : null);

const textOfContent = (content: unknown): string => {
  if (typeof content === "string") return content;
  if (!Array.isArray(content)) return "";
  return content.map((part) => (isRecord(part) && typeof part.text === "string" ? part.text : "")).join("");
};

const isoTimestamp = (): string => new Date().toISOString();

// ── Chat wire projection ─────────────────────────────────────────────────────

const CHAT_ROLE_BY_RESPONSES_ROLE: Readonly<Record<string, string>> = {
  developer: "system",
  system: "system",
  assistant: "assistant",
  user: "user",
};

const roleToChatRole = (role: string): string => CHAT_ROLE_BY_RESPONSES_ROLE[role] ?? "user";

const functionToolEntry = (name: string, description: unknown, parameters: unknown): Json => ({
  type: "function",
  function: {
    name,
    description: typeof description === "string" ? description : "",
    parameters: isRecord(parameters) ? parameters : { type: "object", properties: {} },
  },
});

const projectNamespaceTools = (namespace: Json): Json[] => {
  if (typeof namespace.name !== "string" || !Array.isArray(namespace.tools)) return [];
  const entries: Json[] = [];
  for (const nested of namespace.tools) {
    if (!isRecord(nested) || nested.type !== "function" || typeof nested.name !== "string") continue;
    entries.push(functionToolEntry(`${namespace.name}__${nested.name}`, nested.description, nested.parameters));
  }
  return entries;
};

const projectChatTools = (tools: readonly Json[]): Json[] => {
  const entries: Json[] = [];
  for (const tool of tools) {
    if (tool.type === "function" && typeof tool.name === "string") {
      entries.push(functionToolEntry(tool.name, tool.description, tool.parameters));
      continue;
    }
    if (tool.type === "namespace") entries.push(...projectNamespaceTools(tool));
  }
  return entries;
};

const chatMessageItem = (item: Json): Json => {
  const role = typeof item.role === "string" ? item.role : "user";
  return { role: roleToChatRole(role), content: textOfContent(item.content) };
};

const customToolCallArguments = (item: Json): string => JSON.stringify({ input: item.input ?? "" });

const functionCallArguments = (item: Json): string => (typeof item.arguments === "string" ? item.arguments : "{}");

const toolCallItem = (item: Json, type: string, fallbackId: string): Json => {
  const name = typeof item.name === "string" ? item.name : "function";
  const args = type === "function_call" ? functionCallArguments(item) : customToolCallArguments(item);
  const callId = typeof item.call_id === "string" ? item.call_id : fallbackId;
  return { id: callId, type: "function", function: { name, arguments: args } };
};

const toolOutputItem = (item: Json): Json => ({
  role: "tool",
  tool_call_id: typeof item.call_id === "string" ? item.call_id : "",
  content: textOfContent(item.output),
});

const TOOL_CALL_ITEM_TYPES = new Set(["function_call", "custom_tool_call"]);
const TOOL_OUTPUT_ITEM_TYPES = new Set(["function_call_output", "custom_tool_call_output"]);

/** Merge consecutive tool calls into chat assistant messages per the Chat Completions wire. */
export const projectEntryToChat = (entry: CorpusEntry): Readonly<{ messages: Json[]; tools: Json[] }> => {
  const messages: Json[] = [];
  let pendingToolCalls: Json[] = [];
  const flushToolCalls = (): void => {
    if (!pendingToolCalls.length) return;
    messages.push({ role: "assistant", content: null, tool_calls: pendingToolCalls });
    pendingToolCalls = [];
  };
  for (const item of entry.request.input) {
    const type = typeof item.type === "string" ? item.type : "";
    if (type === "message") {
      flushToolCalls();
      messages.push(chatMessageItem(item));
      continue;
    }
    if (type === "agent_message") {
      flushToolCalls();
      messages.push({ role: "user", content: textOfContent(item.content) });
      continue;
    }
    if (TOOL_CALL_ITEM_TYPES.has(type)) {
      pendingToolCalls.push(toolCallItem(item, type, `call_${messages.length}_${pendingToolCalls.length}`));
      continue;
    }
    if (TOOL_OUTPUT_ITEM_TYPES.has(type)) {
      flushToolCalls();
      messages.push(toolOutputItem(item));
    }
    // Any other item type (for example an unprojectable item) is dropped by the chat wire only.
  }
  flushToolCalls();
  return { messages, tools: projectChatTools(entry.request.tools) };
};

// ── Request bodies ───────────────────────────────────────────────────────────

const buildResponsesBody = (options: AttemptOptions): Json => ({
  model: options.provider.model,
  input: options.entry.request.input,
  tools: options.entry.request.tools,
  tool_choice: options.entry.request.tool_choice,
  parallel_tool_calls: options.entry.request.parallel_tool_calls,
  ...(options.entry.request.reasoning ? { reasoning: options.entry.request.reasoning } : {}),
  max_output_tokens: options.entry.request.max_output_tokens,
  store: false,
  stream: true,
  include: ["reasoning.encrypted_content"],
});

const buildChatBody = (options: AttemptOptions): Json => {
  const projected = projectEntryToChat(options.entry);
  return {
    model: options.provider.model,
    messages: projected.messages,
    tools: projected.tools,
    tool_choice: options.entry.request.tool_choice,
    ...(options.entry.request.reasoning ? { reasoning_effort: options.entry.request.reasoning.effort } : {}),
    max_tokens: options.entry.request.max_output_tokens,
    stream: true,
    stream_options: { include_usage: true },
  };
};

// ── Stream state ─────────────────────────────────────────────────────────────

type StreamState = Readonly<{
  events: number;
  output_chars: number;
  tool_calls: number;
  tool_call_indexes: ReadonlySet<number>;
  first_event_at: number | null;
  first_output_at: number | null;
  first_item_at: number | null;
  terminal_at: number | null;
  terminal_kind: "completed" | "incomplete" | "failed" | "done" | null;
  terminal_error: string | null;
  incomplete_reason: string | null;
  usage: TokenUsage | null;
  parse_failures: number;
  response_id: string | null;
}>;

const emptyState = (): StreamState => ({
  events: 0,
  output_chars: 0,
  tool_calls: 0,
  tool_call_indexes: new Set<number>(),
  first_event_at: null,
  first_output_at: null,
  first_item_at: null,
  terminal_at: null,
  terminal_kind: null,
  terminal_error: null,
  incomplete_reason: null,
  usage: null,
  parse_failures: 0,
  response_id: null,
});

const firstNumber = (values: readonly (number | null)[]): number => values.find((value) => value !== null) ?? 0;

const normalizeResponsesUsage = (raw: unknown): TokenUsage | null => {
  if (!isRecord(raw)) return null;
  const input = asNumber(raw.input_tokens);
  const output = asNumber(raw.output_tokens);
  if (input === null || output === null) return null;
  const inputDetails = isRecord(raw.input_tokens_details) ? raw.input_tokens_details : {};
  const outputDetails = isRecord(raw.output_tokens_details) ? raw.output_tokens_details : {};
  return {
    input_tokens: input,
    cached_input_tokens: firstNumber([asNumber(inputDetails.cached_tokens), asNumber(raw.cached_input_tokens)]),
    cache_write_input_tokens: firstNumber([asNumber(inputDetails.cache_write_tokens), asNumber(raw.cache_write_input_tokens)]),
    output_tokens: output,
    reasoning_tokens: firstNumber([asNumber(outputDetails.reasoning_tokens), asNumber(raw.reasoning_output_tokens)]),
    total_tokens: asNumber(raw.total_tokens) ?? input + output,
  };
};

const normalizeChatUsage = (raw: unknown): TokenUsage | null => {
  if (!isRecord(raw)) return null;
  const input = asNumber(raw.prompt_tokens);
  const output = asNumber(raw.completion_tokens);
  if (input === null || output === null) return null;
  const promptDetails = isRecord(raw.prompt_tokens_details) ? raw.prompt_tokens_details : {};
  const completionDetails = isRecord(raw.completion_tokens_details) ? raw.completion_tokens_details : {};
  return {
    input_tokens: input,
    cached_input_tokens: firstNumber([asNumber(promptDetails.cached_tokens)]),
    cache_write_input_tokens: firstNumber([asNumber(promptDetails.cache_write_tokens)]),
    output_tokens: output,
    reasoning_tokens: firstNumber([asNumber(completionDetails.reasoning_tokens)]),
    total_tokens: asNumber(raw.total_tokens) ?? input + output,
  };
};

type MutableState = { -readonly [K in keyof StreamState]: StreamState[K] };

const beginEvent = (event: Json, now: number, state: StreamState): MutableState => {
  const next: MutableState = { ...state, events: state.events + 1 };
  next.first_event_at ??= now;
  const responseRecord = isRecord(event.response) ? event.response : null;
  if (responseRecord && typeof responseRecord.id === "string") next.response_id ??= responseRecord.id;
  return next;
};

const RESPONSES_DELTA_TYPES = new Set([
  "response.output_text.delta",
  "response.reasoning_summary_text.delta",
  "response.reasoning_text.delta",
  "response.function_call_arguments.delta",
]);

const applyResponsesDelta = (event: Json, now: number, next: MutableState, type: string): MutableState => {
  const delta = typeof event.delta === "string" ? event.delta : "";
  if (delta.length) next.first_output_at ??= now;
  if (type === "response.output_text.delta") next.output_chars += delta.length;
  return next;
};

const applyResponsesItemDone = (event: Json, now: number, next: MutableState): MutableState => {
  next.first_item_at ??= now;
  if (isRecord(event.item) && event.item.type === "function_call") next.tool_calls += 1;
  return next;
};

const RESPONSES_TERMINAL_KIND: Readonly<Record<string, "completed" | "incomplete" | "failed">> = {
  "response.completed": "completed",
  "response.incomplete": "incomplete",
  "response.failed": "failed",
};

const applyResponsesTerminal = (event: Json, now: number, next: MutableState, type: string): MutableState => {
  next.terminal_at = now;
  next.terminal_kind = RESPONSES_TERMINAL_KIND[type] ?? "failed";
  const response = event.response;
  if (!isRecord(response)) return next;
  const usage = normalizeResponsesUsage(response.usage);
  if (usage) next.usage = usage;
  const details = response.incomplete_details;
  if (isRecord(details) && typeof details.reason === "string") next.incomplete_reason = details.reason;
  const error = response.error;
  if (isRecord(error) && typeof error.message === "string") next.terminal_error = error.message;
  return next;
};

const applyResponsesEvent = (event: Json, now: number, state: StreamState): StreamState => {
  const type = typeof event.type === "string" ? event.type : "";
  const next = beginEvent(event, now, state);
  if (RESPONSES_DELTA_TYPES.has(type)) return applyResponsesDelta(event, now, next, type);
  if (type === "response.output_item.done") return applyResponsesItemDone(event, now, next);
  if (type === "response.completed" || type === "response.incomplete" || type === "response.failed") {
    return applyResponsesTerminal(event, now, next, type);
  }
  return next;
};

const applyChatToolCalls = (delta: Json, now: number, next: MutableState): void => {
  if (!Array.isArray(delta.tool_calls)) return;
  const seen = new Set(next.tool_call_indexes);
  for (const call of delta.tool_calls) {
    if (!isRecord(call)) continue;
    const index = typeof call.index === "number" ? call.index : seen.size;
    const fn = isRecord(call.function) ? call.function : {};
    const hasPayload = typeof call.id === "string" || typeof fn.name === "string" || (typeof fn.arguments === "string" && fn.arguments.length > 0);
    if (!seen.has(index)) {
      seen.add(index);
      next.tool_calls += 1;
    }
    if (hasPayload) next.first_output_at ??= now;
  }
  next.tool_call_indexes = seen;
};

const applyChatChoice = (choice: Json, now: number, next: MutableState): void => {
  const delta = isRecord(choice.delta) ? choice.delta : {};
  const contentDelta = typeof delta.content === "string" ? delta.content : "";
  const reasoningDelta = typeof delta.reasoning_content === "string" ? delta.reasoning_content : "";
  if (contentDelta.length || reasoningDelta.length) next.first_output_at ??= now;
  next.output_chars += contentDelta.length;
  applyChatToolCalls(delta, now, next);
  if (typeof choice.finish_reason !== "string" || !choice.finish_reason.length) return;
  next.terminal_at = now;
  next.terminal_kind = choice.finish_reason === "length" ? "incomplete" : "completed";
  if (choice.finish_reason === "length") next.incomplete_reason = "max_tokens";
};

const applyChatChunk = (chunk: Json, now: number, state: StreamState): StreamState => {
  const next = beginEvent(chunk, now, state);
  if (typeof chunk.id === "string") next.response_id ??= chunk.id;
  const usage = normalizeChatUsage(chunk.usage);
  if (usage) next.usage = usage;
  const choices = Array.isArray(chunk.choices) ? chunk.choices : [];
  for (const choice of choices) if (isRecord(choice)) applyChatChoice(choice, now, next);
  return next;
};

// ── HTTP failure classification ──────────────────────────────────────────────

const classifyHttpFailure = (status: number, errorCode: string | null, message: string | null): FailureKind => {
  if (status === 429) return "http_429";
  if (status === 402) return "capacity_failure";
  if (status === 403 && (errorCode ?? "").toLowerCase().includes("quota")) return "capacity_failure";
  if (status >= 500) return "http_5xx";
  if (status >= 400 && /context[_ ]length|maximum context|too long|tokens? in the (?:messages|prompt)|max_tokens/i.test(message ?? "")) {
    return "context_length_rejection";
  }
  if (status >= 400) return "http_4xx";
  return "http_5xx";
};

const readBoundedText = async (response: Response, limit = 64_000): Promise<string> => {
  try {
    const text = await response.text();
    return text.slice(0, limit);
  } catch {
    return "";
  }
};

const parseHttpError = (text: string): Readonly<{ code: string | null; message: string | null }> => {
  try {
    const parsed = JSON.parse(text) as unknown;
    if (isRecord(parsed) && isRecord(parsed.error)) {
      return {
        code: typeof parsed.error.code === "string" ? parsed.error.code : null,
        message: typeof parsed.error.message === "string" ? parsed.error.message : text.slice(0, 400),
      };
    }
  } catch {
    // Non-JSON error body; the truncated text is the evidence.
  }
  return { code: null, message: text.slice(0, 400) };
};

// ── Attempt execution ────────────────────────────────────────────────────────

type TimingState = {
  headers: number | null;
  first_byte: number | null;
  first_event: number | null;
  first_output: number | null;
  first_item: number | null;
  terminal: number | null;
};

type AttemptFields = Readonly<{
  startedAtIso: string;
  httpStatus: number | null;
  upstream: string | null;
  providerRequestId: string | null;
  responseId: string | null;
  success: boolean;
  failure: FailureKind | null;
  errorCode: string | null;
  errorMessage: string | null;
  terminated: boolean;
  state: StreamState;
  endMs: number | null;
  timings: TimingState;
}>;

const buildAttemptRecord = (options: AttemptOptions, fields: AttemptFields): AttemptRecord => {
  const state = fields.state;
  return {
    run_id: options.run_id,
    provider: options.provider.id,
    corpus_id: options.entry.id,
    cls: options.entry.cls,
    period: options.period,
    batch: options.batch,
    attempt: options.attempt,
    wire: options.provider.wire,
    model: options.provider.model,
    selection: options.provider.selection,
    gateway: options.gateway_base,
    started_at: fields.startedAtIso,
    finished_at: isoTimestamp(),
    t_headers_ms: fields.timings.headers === null ? null : Math.round(fields.timings.headers),
    t_first_byte_ms: fields.timings.first_byte === null ? null : Math.round(fields.timings.first_byte),
    t_first_event_ms: state.first_event_at === null ? null : Math.round(state.first_event_at),
    t_first_output_ms: state.first_output_at === null ? null : Math.round(state.first_output_at),
    t_first_item_ms: state.first_item_at === null ? null : Math.round(state.first_item_at),
    t_terminal_ms: state.terminal_at === null ? null : Math.round(state.terminal_at),
    t_end_ms: fields.endMs,
    http_status: fields.httpStatus,
    upstream: fields.upstream,
    provider_request_id: fields.providerRequestId,
    response_id: fields.responseId,
    success: fields.success,
    first_attempt: options.first_attempt,
    failure_kind: fields.failure,
    error_code: fields.errorCode,
    error_message: fields.errorMessage,
    response_terminated: fields.terminated,
    sse_events: state.events,
    output_chars: state.output_chars,
    tool_calls: state.tool_calls,
    usage: state.usage,
    usage_reported: state.usage !== null,
    notes: state.parse_failures > 0 ? `parse_failures=${state.parse_failures}` : null,
  };
};

const transportFailure = (error: unknown): Readonly<{ failure: FailureKind; message: string }> => {
  const message = error instanceof Error ? error.message : String(error);
  const name = error instanceof Error ? error.name : "";
  if (name === "AbortError" || /aborted|timeout/i.test(message)) return { failure: "timeout", message };
  if (/dns|resolve|lookup/i.test(message)) return { failure: "dns_network_failure", message };
  return { failure: "connection_failure", message };
};

const classifyStreamOutcome = (state: StreamState, terminated: boolean): Readonly<{ failure: FailureKind | null; message: string | null }> => {
  if (state.parse_failures > 0 && state.terminal_kind === null) return { failure: "malformed_sse", message: null };
  if (!terminated) return { failure: "interrupted_stream", message: null };
  if (state.terminal_kind === null) return { failure: "missing_completion", message: null };
  if (state.terminal_kind === "failed") return { failure: "http_5xx", message: state.terminal_error ?? "upstream reported a failed response" };
  if (state.terminal_kind === "incomplete") return { failure: "truncated_response", message: state.incomplete_reason ?? "response incomplete" };
  if (state.output_chars === 0 && state.tool_calls === 0) return { failure: "empty_response", message: null };
  if (state.usage === null) return { failure: "usage_accounting_failure", message: null };
  return { failure: null, message: null };
};

const consumeSseBlock = (block: string, wire: "responses" | "chat", now: number, state: StreamState): Readonly<{ state: StreamState; sawDone: boolean }> => {
  const dataLines = block
    .split(/\r?\n/)
    .filter((line) => line.startsWith("data:"))
    .map((line) => line.slice(5).trim());
  if (!dataLines.length) return { state, sawDone: false };
  const payload = dataLines.join("\n");
  if (payload === "[DONE]") {
    const next = { ...state, terminal_at: state.terminal_at ?? now, terminal_kind: state.terminal_kind ?? "done" } as StreamState;
    return { state: next, sawDone: true };
  }
  let parsed: unknown;
  try {
    parsed = JSON.parse(payload) as unknown;
  } catch {
    return { state: { ...state, parse_failures: state.parse_failures + 1 }, sawDone: false };
  }
  if (!isRecord(parsed)) return { state, sawDone: false };
  return { state: wire === "responses" ? applyResponsesEvent(parsed, now, state) : applyChatChunk(parsed, now, state), sawDone: false };
};

const readStream = async (
  reader: ReadableStreamDefaultReader<Uint8Array>,
  wire: "responses" | "chat",
  started: number,
  timings: TimingState
): Promise<Readonly<{ state: StreamState; terminated: boolean }>> => {
  const decoder = new TextDecoder();
  let buffer = "";
  let firstByte = false;
  let sawDone = false;
  let state = emptyState();
  for (;;) {
    const { done, value } = await reader.read();
    const now = performance.now() - started;
    if (done) return { state, terminated: true };
    if (!firstByte && value.length > 0) {
      firstByte = true;
      timings.first_byte = now;
    }
    buffer += decoder.decode(value, { stream: true });
    const blocks = buffer.split(/\r?\n\r?\n/);
    buffer = blocks.pop() ?? "";
    for (const block of blocks) {
      const consumed = consumeSseBlock(block, wire, now, state);
      state = consumed.state;
      sawDone = sawDone || consumed.sawDone;
    }
    if (sawDone && state.terminal_kind === null) state = { ...state, terminal_kind: "done", terminal_at: performance.now() - started };
  }
};

export const runAttempt = async (options: AttemptOptions): Promise<AttemptRecord> => {
  const startedAtIso = isoTimestamp();
  const started = performance.now();
  const headers = { "content-type": "application/json", accept: "text/event-stream" };
  const url = options.provider.wire === "responses" ? `${options.gateway_base}/v1/responses` : `${options.gateway_base}/v1/chat/completions`;
  const body = options.provider.wire === "responses" ? buildResponsesBody(options) : buildChatBody(options);
  const controller = new AbortController();
  const timer = setTimeout(() => {
    controller.abort(new Error("benchmark_timeout"));
  }, options.timeout_ms);
  const timings: TimingState = { headers: null, first_byte: null, first_event: null, first_output: null, first_item: null, terminal: null };
  let httpStatus: number | null = null;
  let upstream: string | null = null;
  let providerRequestId: string | null = null;
  let failure: FailureKind | null = null;
  let errorCode: string | null = null;
  let errorMessage: string | null = null;
  let state = emptyState();
  let terminated = false;
  let streamAttempted = false;
  try {
    const response = await fetch(url, { method: "POST", headers, body: JSON.stringify(body), signal: controller.signal });
    timings.headers = performance.now() - started;
    httpStatus = response.status;
    upstream = response.headers.get("x-uos-upstream");
    providerRequestId = response.headers.get("x-uos-provider-request-id");
    if (!response.ok) {
      const parsed = parseHttpError(await readBoundedText(response));
      errorCode = parsed.code;
      errorMessage = parsed.message;
      failure = classifyHttpFailure(response.status, errorCode, errorMessage);
    } else {
      streamAttempted = true;
      const reader = response.body?.getReader();
      if (!reader) {
        failure = "connection_failure";
        errorMessage = "response body was empty";
      } else {
        const streamed = await readStream(reader, options.provider.wire, started, timings);
        state = streamed.state;
        terminated = streamed.terminated;
      }
    }
  } catch (error) {
    const transport = transportFailure(error);
    failure = transport.failure;
    errorMessage = transport.message;
  } finally {
    clearTimeout(timer);
  }
  const endMs = streamAttempted ? Math.round(performance.now() - started) : null;
  if (failure === null) {
    const outcome = classifyStreamOutcome(state, terminated);
    failure = outcome.failure;
    if (outcome.message !== null) errorMessage = outcome.message;
  }
  return buildAttemptRecord(options, {
    startedAtIso,
    httpStatus,
    upstream,
    providerRequestId,
    responseId: state.response_id,
    success: failure === null,
    failure,
    errorCode,
    errorMessage,
    terminated,
    state,
    endMs,
    timings,
  });
};
