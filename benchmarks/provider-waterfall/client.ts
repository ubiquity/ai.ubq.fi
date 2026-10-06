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

/** Merge consecutive tool calls into chat assistant messages per the Chat Completions wire. */
export const projectEntryToChat = (entry: CorpusEntry): Readonly<{ messages: Json[]; tools: Json[] }> => {
  const messages: Json[] = [];
  const pushMessage = (message: Json): void => {
    messages.push(message);
  };
  let pendingToolCalls: Json[] = [];
  const flushToolCalls = (): void => {
    if (!pendingToolCalls.length) return;
    pushMessage({ role: "assistant", content: null, tool_calls: pendingToolCalls });
    pendingToolCalls = [];
  };
  for (const item of entry.request.input) {
    const type = typeof item.type === "string" ? item.type : "";
    if (type === "message") {
      flushToolCalls();
      const role = typeof item.role === "string" ? item.role : "user";
      const chatRole = role === "developer" ? "system" : role === "system" ? "system" : role === "assistant" ? "assistant" : "user";
      pushMessage({ role: chatRole, content: textOfContent(item.content) });
      continue;
    }
    if (type === "agent_message") {
      flushToolCalls();
      pushMessage({ role: "user", content: textOfContent(item.content) });
      continue;
    }
    if (type === "function_call" || type === "custom_tool_call") {
      const name = typeof item.name === "string" ? item.name : "function";
      const args = type === "function_call" ? (typeof item.arguments === "string" ? item.arguments : "{}") : JSON.stringify({ input: item.input ?? "" });
      const callId = typeof item.call_id === "string" ? item.call_id : `call_${messages.length}_${pendingToolCalls.length}`;
      pendingToolCalls.push({ id: callId, type: "function", function: { name, arguments: args } });
      continue;
    }
    if (type === "function_call_output" || type === "custom_tool_call_output") {
      flushToolCalls();
      const callId = typeof item.call_id === "string" ? item.call_id : "";
      pushMessage({ role: "tool", tool_call_id: callId, content: textOfContent(item.output) });
      continue;
    }
    // Any other item type (for example an unprojectable item) is dropped by the chat wire only.
  }
  flushToolCalls();
  const tools: Json[] = [];
  for (const tool of entry.request.tools) {
    if (tool.type === "function" && typeof tool.name === "string") {
      tools.push({
        type: "function",
        function: { name: tool.name, description: tool.description ?? "", parameters: tool.parameters ?? { type: "object", properties: {} } },
      });
      continue;
    }
    if (tool.type === "namespace" && typeof tool.name === "string" && Array.isArray(tool.tools)) {
      for (const nested of tool.tools) {
        if (!isRecord(nested) || nested.type !== "function" || typeof nested.name !== "string") continue;
        tools.push({
          type: "function",
          function: {
            name: `${tool.name}__${nested.name}`,
            description: nested.description ?? "",
            parameters: nested.parameters ?? { type: "object", properties: {} },
          },
        });
      }
    }
  }
  return { messages, tools };
};

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

const normalizeResponsesUsage = (raw: unknown): TokenUsage | null => {
  if (!isRecord(raw)) return null;
  const input = asNumber(raw.input_tokens);
  const output = asNumber(raw.output_tokens);
  if (input === null || output === null) return null;
  const inputDetails = isRecord(raw.input_tokens_details) ? raw.input_tokens_details : {};
  const outputDetails = isRecord(raw.output_tokens_details) ? raw.output_tokens_details : {};
  const cached = asNumber(inputDetails.cached_tokens) ?? asNumber(raw.cached_input_tokens) ?? 0;
  const cacheWrite = asNumber(inputDetails.cache_write_tokens) ?? asNumber(raw.cache_write_input_tokens) ?? 0;
  const reasoning = asNumber(outputDetails.reasoning_tokens) ?? asNumber(raw.reasoning_output_tokens) ?? 0;
  return {
    input_tokens: input,
    cached_input_tokens: cached,
    cache_write_input_tokens: cacheWrite,
    output_tokens: output,
    reasoning_tokens: reasoning,
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
    cached_input_tokens: asNumber(promptDetails.cached_tokens) ?? 0,
    cache_write_input_tokens: asNumber(promptDetails.cache_write_tokens) ?? 0,
    output_tokens: output,
    reasoning_tokens: asNumber(completionDetails.reasoning_tokens) ?? 0,
    total_tokens: asNumber(raw.total_tokens) ?? input + output,
  };
};

const applyResponsesEvent = (event: Json, now: number, state: StreamState): StreamState => {
  const type = typeof event.type === "string" ? event.type : "";
  const responseRecord = isRecord(event.response) ? event.response : null;
  if (responseRecord && typeof responseRecord.id === "string" && state.response_id === null) {
    state = { ...state, response_id: responseRecord.id };
  }
  const next: { -readonly [K in keyof StreamState]: StreamState[K] } = { ...state, events: state.events + 1 };
  if (next.first_event_at === null) next.first_event_at = now;
  switch (type) {
    case "response.output_text.delta":
    case "response.reasoning_summary_text.delta":
    case "response.reasoning_text.delta":
    case "response.function_call_arguments.delta": {
      const delta = typeof event.delta === "string" ? event.delta : "";
      if (delta.length && next.first_output_at === null) next.first_output_at = now;
      if (type === "response.output_text.delta") next.output_chars += delta.length;
      return next;
    }
    case "response.output_item.done": {
      if (next.first_item_at === null) next.first_item_at = now;
      const item = event.item;
      if (isRecord(item) && item.type === "function_call") next.tool_calls += 1;
      return next;
    }
    case "response.completed":
    case "response.incomplete":
    case "response.failed": {
      next.terminal_at = now;
      next.terminal_kind = type === "response.completed" ? "completed" : type === "response.incomplete" ? "incomplete" : "failed";
      const response = event.response;
      if (isRecord(response)) {
        const usage = normalizeResponsesUsage(response.usage);
        if (usage) next.usage = usage;
        const details = response.incomplete_details;
        if (isRecord(details) && typeof details.reason === "string") next.incomplete_reason = details.reason;
        const error = response.error;
        if (isRecord(error) && typeof error.message === "string") next.terminal_error = error.message;
      }
      return next;
    }
    default:
      return next;
  }
};

const applyChatChunk = (chunk: Json, now: number, state: StreamState): StreamState => {
  const next: { -readonly [K in keyof StreamState]: StreamState[K] } = { ...state, events: state.events + 1 };
  if (typeof chunk.id === "string" && next.response_id === null) next.response_id = chunk.id;
  if (next.first_event_at === null) next.first_event_at = now;
  const usage = normalizeChatUsage(chunk.usage);
  if (usage) next.usage = usage;
  const choices = Array.isArray(chunk.choices) ? chunk.choices : [];
  for (const choice of choices) {
    if (!isRecord(choice)) continue;
    const delta = isRecord(choice.delta) ? choice.delta : {};
    const contentDelta = typeof delta.content === "string" ? delta.content : "";
    const reasoningDelta = typeof delta.reasoning_content === "string" ? delta.reasoning_content : "";
    if ((contentDelta.length || reasoningDelta.length) && next.first_output_at === null) next.first_output_at = now;
    next.output_chars += contentDelta.length;
    if (Array.isArray(delta.tool_calls)) {
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
        if (hasPayload && next.first_output_at === null) next.first_output_at = now;
      }
      next.tool_call_indexes = seen;
    }
    if (typeof choice.finish_reason === "string" && choice.finish_reason.length) {
      next.terminal_at = now;
      next.terminal_kind = choice.finish_reason === "length" ? "incomplete" : "completed";
      if (choice.finish_reason === "length") next.incomplete_reason = "max_tokens";
    }
  }
  return next;
};

const classifyHttpFailure = (status: number, errorCode: string | null, message: string | null): FailureKind => {
  if (status === 429) return "http_429";
  if (status === 402) return "capacity_failure";
  if (status === 403 && (errorCode ?? "").toLowerCase().includes("quota")) return "capacity_failure";
  if (status >= 500) return "http_5xx";
  if (status >= 400 && /context[_ ]length|maximum context|too long|tokens? in the (?:messages|prompt)|max_tokens/i.test(message ?? ""))
    return "context_length_rejection";
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

export const runAttempt = async (options: AttemptOptions): Promise<AttemptRecord> => {
  const started_at = isoTimestamp();
  const started = performance.now();
  const headersForWire = { "content-type": "application/json", accept: "text/event-stream" };
  const url = options.provider.wire === "responses" ? `${options.gateway_base}/v1/responses` : `${options.gateway_base}/v1/chat/completions`;
  const body = options.provider.wire === "responses" ? buildResponsesBody(options) : buildChatBody(options);
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(new Error("benchmark_timeout")), options.timeout_ms);
  let httpStatus: number | null = null;
  let upstream: string | null = null;
  let providerRequestId: string | null = null;
  let failure: FailureKind | null = null;
  let errorCode: string | null = null;
  let errorMessage: string | null = null;
  let state = emptyState();
  let terminated = false;
  const timings = { headers: null as number | null, first_byte: null as number | null };
  try {
    const response = await fetch(url, { method: "POST", headers: headersForWire, body: JSON.stringify(body), signal: controller.signal });
    timings.headers = performance.now() - started;
    httpStatus = response.status;
    upstream = response.headers.get("x-uos-upstream");
    providerRequestId = response.headers.get("x-uos-provider-request-id");
    if (!response.ok) {
      const text = await readBoundedText(response);
      try {
        const parsed = JSON.parse(text) as unknown;
        if (isRecord(parsed) && isRecord(parsed.error)) {
          errorCode = typeof parsed.error.code === "string" ? parsed.error.code : null;
          errorMessage = typeof parsed.error.message === "string" ? parsed.error.message : text.slice(0, 400);
        } else {
          errorMessage = text.slice(0, 400);
        }
      } catch {
        errorMessage = text.slice(0, 400);
      }
      failure = classifyHttpFailure(response.status, errorCode, errorMessage);
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
        started_at,
        finished_at: isoTimestamp(),
        t_headers_ms: Math.round(timings.headers),
        t_first_byte_ms: null,
        t_first_event_ms: null,
        t_first_output_ms: null,
        t_first_item_ms: null,
        t_terminal_ms: null,
        t_end_ms: null,
        http_status: response.status,
        upstream,
        provider_request_id: providerRequestId,
        response_id: null,
        success: false,
        first_attempt: options.first_attempt,
        failure_kind: failure,
        error_code: errorCode,
        error_message: errorMessage,
        response_terminated: false,
        sse_events: 0,
        output_chars: 0,
        tool_calls: 0,
        usage: null,
        usage_reported: false,
        notes: null,
      };
    }
    const reader = response.body?.getReader();
    if (!reader) {
      failure = "connection_failure";
      errorMessage = "response body was empty";
    } else {
      const decoder = new TextDecoder();
      let buffer = "";
      let firstByte = false;
      let sawDone = false;
      for (;;) {
        const { done, value } = await reader.read();
        const now = performance.now() - started;
        if (done) {
          terminated = true;
          break;
        }
        if (!firstByte && value && value.length) {
          firstByte = true;
          timings.first_byte = now;
        }
        buffer += decoder.decode(value, { stream: true });
        const blocks = buffer.split(/\r?\n\r?\n/);
        buffer = blocks.pop() ?? "";
        for (const block of blocks) {
          const dataLines = block
            .split(/\r?\n/)
            .filter((line) => line.startsWith("data:"))
            .map((line) => line.slice(5).trim());
          if (!dataLines.length) continue;
          const payload = dataLines.join("\n");
          if (payload === "[DONE]") {
            sawDone = true;
            state = { ...state, terminal_at: state.terminal_at ?? now, terminal_kind: state.terminal_kind ?? "done" };
            continue;
          }
          let parsed: unknown;
          try {
            parsed = JSON.parse(payload) as unknown;
          } catch {
            state = { ...state, parse_failures: state.parse_failures + 1 };
            continue;
          }
          if (!isRecord(parsed)) continue;
          state = options.provider.wire === "responses" ? applyResponsesEvent(parsed, now, state) : applyChatChunk(parsed, now, state);
        }
      }
      if (sawDone && state.terminal_kind === null) state = { ...state, terminal_kind: "done", terminal_at: performance.now() - started };
    }
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    const name = error instanceof Error ? error.name : "";
    if (name === "AbortError" || /aborted|timeout/i.test(message)) {
      failure = "timeout";
      errorMessage = message;
    } else if (/dns|resolve|lookup/i.test(message)) {
      failure = "dns_network_failure";
      errorMessage = message;
    } else {
      failure = "connection_failure";
      errorMessage = message;
    }
  } finally {
    clearTimeout(timer);
  }
  const end = performance.now() - started;
  if (failure === null) {
    if (state.parse_failures > 0 && state.terminal_kind === null) failure = "malformed_sse";
    else if (!terminated) failure = "interrupted_stream";
    else if (state.terminal_kind === null) failure = "missing_completion";
    else if (state.terminal_kind === "failed") {
      failure = "http_5xx";
      errorMessage = state.terminal_error ?? "upstream reported a failed response";
    } else if (state.terminal_kind === "incomplete") {
      failure = "truncated_response";
      errorMessage = state.incomplete_reason ?? "response incomplete";
    } else if (state.output_chars === 0 && state.tool_calls === 0) {
      failure = "empty_response";
    } else if (state.usage === null) {
      failure = "usage_accounting_failure";
    }
  }
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
    started_at,
    finished_at: isoTimestamp(),
    t_headers_ms: timings.headers === null ? null : Math.round(timings.headers),
    t_first_byte_ms: timings.first_byte === null ? null : Math.round(timings.first_byte),
    t_first_event_ms: state.first_event_at === null ? null : Math.round(state.first_event_at),
    t_first_output_ms: state.first_output_at === null ? null : Math.round(state.first_output_at),
    t_first_item_ms: state.first_item_at === null ? null : Math.round(state.first_item_at),
    t_terminal_ms: state.terminal_at === null ? null : Math.round(state.terminal_at),
    t_end_ms: Math.round(end),
    http_status: httpStatus,
    upstream,
    provider_request_id: providerRequestId,
    response_id: state.response_id,
    success: failure === null,
    first_attempt: options.first_attempt,
    failure_kind: failure,
    error_code: errorCode,
    error_message: errorMessage,
    response_terminated: terminated,
    sse_events: state.events,
    output_chars: state.output_chars,
    tool_calls: state.tool_calls,
    usage: state.usage,
    usage_reported: state.usage !== null,
    notes: state.parse_failures > 0 ? `parse_failures=${state.parse_failures}` : null,
  };
};
