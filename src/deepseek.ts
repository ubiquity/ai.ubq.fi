import { type ApiKeyProviderDispatch, ApiKeyQuotaDispatchError } from "./api_key_policy.ts";
import { STREAM_FIRST_EVENT_DEADLINE_MS, STREAM_INACTIVITY_DEADLINE_MS } from "./inference_deadline.ts";
import type { SentinelUpstreamRecorder } from "./sentinel_upstream_capture.ts";
import { getString, isRecord } from "./utils.ts";

/**
 * Official DeepSeek API transport.
 *
 * This is a "special upstream provider" in the same sense as Cerebras: a
 * request addressed to one of its model ids is dispatched straight to the
 * provider's documented OpenAI-compatible Chat Completions endpoint and never
 * races or falls back to another provider.
 *
 * Provider facts (https://api-docs.deepseek.com, read 2026-09-16):
 * - base URL `https://api.deepseek.com`, Chat Completions at `/chat/completions`
 * - model ids `deepseek-flash` (DeepSeek-V4.1-Flash) and `deepseek-v4-pro`
 * - `deepseek-v4-flash` is a retired-but-accepted interchangeable alias that the
 *   API still serves with the DeepSeek-V4.1-Flash model
 * - `reasoning_effort` accepts none/low/high/max, plus the documented
 *   compatibility aliases minimal (→low), medium and xhigh (→high)
 * - thinking mode is ENABLED BY DEFAULT and its output arrives as
 *   `message.reasoning_content` / `delta.reasoning_content`
 * - the documented output cap is `max_tokens`, and `temperature` has no effect
 *   while thinking mode is active
 * - streaming is native SSE (`data: {...}` frames terminated by `data: [DONE]`),
 *   with `: keep-alive` comment frames while the model is thinking
 */
export const DEEPSEEK_FLASH_MODEL = "deepseek-flash";
/**
 * Interchangeable legacy id for the same official model. It is still accepted
 * by the API and is the id the local Codex model catalog already advertises,
 * so both ids must route here.
 */
export const DEEPSEEK_V4_FLASH_MODEL = "deepseek-v4-flash";
export const DEEPSEEK_OFFICIAL_MODEL_IDS = [DEEPSEEK_FLASH_MODEL, DEEPSEEK_V4_FLASH_MODEL] as const;
export const DEEPSEEK_CHAT_COMPLETIONS_URL = "https://api.deepseek.com/chat/completions";
export const DEEPSEEK_CONTEXT_WINDOW_TOKENS = 1_000_000;
/** Documented reasoning tiers, in ascending order. */
export const DEEPSEEK_REASONING_LEVELS = ["none", "low", "high", "max"] as const;
/** Documented default when a request omits `reasoning_effort` (thinking mode on, effort high). */
export const DEEPSEEK_DEFAULT_REASONING_EFFORT = "high";

const DEEPSEEK_API_KEY_ENV = "DEEPSEEK_API_KEY";
const MAX_DEEPSEEK_PROVIDER_REQUEST_ID_LENGTH = 256;
/** One SSE frame is a single generated chunk; bound it so a peer cannot pin memory. */
const MAX_DEEPSEEK_SSE_FRAME_BYTES = 4 * 1024 * 1024;
const SSE_FRAME_BOUNDARY = /\r\n\r\n|\n\n|\r\r/;
const SSE_LINE_BOUNDARY = /\r\n|\r|\n/;
const DEEPSEEK_OFFICIAL_MODEL_ID_SET = new Set<string>(DEEPSEEK_OFFICIAL_MODEL_IDS);
/**
 * Advanced Codex CLI presets are translated to the documented DeepSeek tier at
 * the provider boundary, exactly as the Codex Responses bridge does.
 */
const DEEPSEEK_WIRE_EFFORT_MAP: Readonly<Record<string, string>> = { ultra: "max" };

let deepSeekFetchTimeoutMs = STREAM_FIRST_EVENT_DEADLINE_MS;

export type DeepSeekFetch = (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>;

export type DeepSeekErrorCode = "deepseek_api_key_missing" | "deepseek_request_invalid" | "deepseek_upstream_unreachable" | "gateway_timeout";

export class DeepSeekError extends Error {
  readonly code: DeepSeekErrorCode;
  readonly status: number;

  constructor(message: string, code: DeepSeekErrorCode, status: number) {
    super(message);
    this.name = "DeepSeekError";
    this.code = code;
    this.status = status;
  }
}

export type DeepSeekChatCompletionsOptions = Readonly<{
  apiKey?: string | null;
  fetcher?: DeepSeekFetch;
  signal?: AbortSignal;
  /**
   * Resolves the API-key dispatch admission for this attempt. Both the
   * "claimed a dispatch" and the "nothing to claim" shapes are accepted, which
   * is why this is a union of function types rather than a union that puts
   * `void` inside `Promise<...>`.
   */
  beforeDispatch?: (() => Promise<ApiKeyProviderDispatch>) | (() => void);
  onDispatch?: () => void;
  onHeaders?: () => void;
  /** Request-owned passive recorder; best effort, never required. */
  sentinelUpstreamRecorder?: SentinelUpstreamRecorder;
}>;

type NormalizationResult<T> = Readonly<{ ok: true; value: T }> | Readonly<{ ok: false; message: string }>;

const nonEmptyString = (value: unknown): string | null => {
  const text = getString(value)?.trim() ?? "";
  return text === "" ? null : text;
};

/** Tool IDs and names are opaque OpenAI wire values, not display strings. */
const exactNonEmptyString = (value: unknown): string | null => {
  if (typeof value !== "string" || value.length === 0 || value.trim() !== value) return null;
  return value;
};

/**
 * Provider request IDs are opaque support-correlation values. Keep only a
 * bounded, header-safe value so an upstream cannot make the gateway reflect
 * arbitrary metadata.
 */
export const normalizeDeepSeekProviderRequestId = (value: unknown): string | null => {
  const requestId = exactNonEmptyString(value);
  if (!requestId || requestId.length > MAX_DEEPSEEK_PROVIDER_REQUEST_ID_LENGTH) return null;
  return /^[\x21-\x7e]+$/.test(requestId) ? requestId : null;
};

export const getDeepSeekProviderRequestId = (response: Response): string | null =>
  normalizeDeepSeekProviderRequestId(
    response.headers.get("X-Request-Id") ?? response.headers.get("X-Ds-Request-Id") ?? response.headers.get("X-Deepseek-Request-Id")
  );

const nonNegativeInteger = (value: unknown): number | null => {
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  const normalized = Math.trunc(value);
  return normalized >= 0 ? normalized : null;
};

const getEnv = (key: string): string | undefined => {
  try {
    return Deno.env.get(key);
  } catch {
    return undefined;
  }
};

const timeoutError = (): DeepSeekError => new DeepSeekError("Upstream request exceeded the gateway deadline.", "gateway_timeout", 504);

const abortError = (signal: AbortSignal): Error =>
  signal.reason instanceof Error ? signal.reason : new DOMException("The request was aborted.", "AbortError");

const isTimeoutError = (error: unknown): boolean => error instanceof Error && error.name === "TimeoutError";

export const readDeepSeekApiKey = (): string | null => nonEmptyString(getEnv(DEEPSEEK_API_KEY_ENV));

export const setDeepSeekFetchTimeoutMsForTest = (timeoutMs: number | null): void => {
  deepSeekFetchTimeoutMs = timeoutMs ?? STREAM_FIRST_EVENT_DEADLINE_MS;
};

const requireDeepSeekApiKey = (supplied: string | null | undefined): string => {
  const apiKey = supplied === undefined ? readDeepSeekApiKey() : nonEmptyString(supplied);
  if (apiKey) return apiKey;
  throw new DeepSeekError("The requested model is not configured.", "deepseek_api_key_missing", 503);
};

/**
 * Resolves the canonical upstream model for a client-facing model id, or null
 * when the id is not an official DeepSeek model. Comparison is
 * case-insensitive, matching the gateway's model normalization elsewhere.
 */
export const deepSeekUpstreamModelFor = (model: string): string | null => {
  const normalized = model.trim().toLowerCase();
  return DEEPSEEK_OFFICIAL_MODEL_ID_SET.has(normalized) ? DEEPSEEK_FLASH_MODEL : null;
};

/** Maps a requested reasoning tier onto the documented DeepSeek wire tier. */
export const projectDeepSeekReasoningEffort = (effort: string): string => DEEPSEEK_WIRE_EFFORT_MAP[effort.trim().toLowerCase()] ?? effort;

/**
 * Projects the official Chat Completions body onto DeepSeek's documented wire
 * contract. Only two provider necessities are applied — everything else is
 * forwarded unchanged:
 *
 * 1. `model` becomes the canonical official id, so the interchangeable legacy
 *    alias reaches the API as the model it actually serves.
 * 2. `max_completion_tokens` becomes DeepSeek's documented `max_tokens`. The
 *    gateway's Chat contract only accepts the OpenAI field name, and DeepSeek
 *    documents no `max_completion_tokens`, so leaving it in place would both
 *    lose the cap and send an unrecognized parameter.
 */
export const projectDeepSeekRequest = (body: Record<string, unknown>, requestedModel: string): Record<string, unknown> => {
  const upstreamModel = deepSeekUpstreamModelFor(requestedModel);
  if (!upstreamModel) throw new DeepSeekError("The requested model is not configured.", "deepseek_request_invalid", 400);
  const projected: Record<string, unknown> = { ...body, model: upstreamModel };
  if (projected.max_completion_tokens !== undefined && projected.max_completion_tokens !== null) {
    projected.max_tokens = projected.max_completion_tokens;
  }
  delete projected.max_completion_tokens;
  if (typeof projected.reasoning_effort === "string") projected.reasoning_effort = projectDeepSeekReasoningEffort(projected.reasoning_effort);
  return projected;
};

/**
 * Classifies one aborted dispatch, before any upstream response exists. The
 * request signal decides first because its reason carries the caller's own
 * abort/timeout identity.
 */
const abortedTransportError = (signal: AbortSignal, requestSignal: AbortSignal | undefined, deadlineSignal: AbortSignal): Error => {
  if (requestSignal && isTimeoutError(abortError(requestSignal))) return timeoutError();
  if (deadlineSignal.aborted) return timeoutError();
  return abortError(signal);
};

/**
 * Classifies a failed attempt: an aborted request signal keeps its own
 * identity, a deadline or timeout failure becomes the gateway timeout, and
 * everything else is an unreachable upstream.
 */
const transportFailureError = (error: unknown, requestSignal: AbortSignal | undefined, deadlineSignal: AbortSignal): Error => {
  if (requestSignal?.aborted) return isTimeoutError(abortError(requestSignal)) ? timeoutError() : abortError(requestSignal);
  if (deadlineSignal.aborted || isTimeoutError(error)) return timeoutError();
  return new DeepSeekError("Upstream request could not be completed.", "deepseek_upstream_unreachable", 502);
};

/**
 * Sends only canonical OpenAI Chat Completions JSON to the official DeepSeek
 * API. The caller owns model selection and streaming policy; this transport
 * never chooses or falls back to another provider, and it never rewrites the
 * caller's `stream` flag.
 *
 * The deadline covers response headers only. Once headers arrive the caller
 * owns the stream's inactivity deadline, so a long thinking turn is not killed
 * by the buffered-inference budget.
 */
export const fetchDeepSeekChatCompletions = async (
  body: Record<string, unknown>,
  requestedModel: string,
  options: DeepSeekChatCompletionsOptions = {}
): Promise<Response> => {
  let encodedBody: string;
  try {
    encodedBody = JSON.stringify(projectDeepSeekRequest(body, requestedModel));
  } catch (error) {
    if (error instanceof DeepSeekError) throw error;
    throw new DeepSeekError("Chat Completions requests must use a JSON-serializable body.", "deepseek_request_invalid", 400);
  }
  if (typeof encodedBody !== "string") {
    throw new DeepSeekError("Chat Completions requests must use a JSON-serializable body.", "deepseek_request_invalid", 400);
  }

  const apiKey = requireDeepSeekApiKey(options.apiKey);
  const headers = new Headers({
    Accept: "application/json",
    Authorization: `Bearer ${apiKey}`,
    "Content-Type": "application/json",
  });
  const deadline = new AbortController();
  const timer = setTimeout(() => {
    deadline.abort(new DOMException("DeepSeek response headers timed out.", "TimeoutError"));
  }, deepSeekFetchTimeoutMs);
  const signal = options.signal ? AbortSignal.any([options.signal, deadline.signal]) : deadline.signal;

  let upstreamAttempt: ReturnType<SentinelUpstreamRecorder["startAttempt"]> | null = null;
  try {
    // Keep this as the final awaited operation before provider transport so
    // API-key admission cannot be committed after a cancelled request.
    const dispatch = options.beforeDispatch ? await options.beforeDispatch() : undefined;
    if (signal.aborted) {
      await dispatch?.cancelBeforeTransport();
      throw abortedTransportError(signal, options.signal, deadline.signal);
    }
    dispatch?.markTransportStarted();
    options.onDispatch?.();
    upstreamAttempt = options.sentinelUpstreamRecorder?.startAttempt("deepseek") ?? null;
    const response = await (options.fetcher ?? fetch)(DEEPSEEK_CHAT_COMPLETIONS_URL, {
      method: "POST",
      headers,
      body: encodedBody,
      redirect: "manual",
      signal,
    });
    options.onHeaders?.();
    return upstreamAttempt ? upstreamAttempt.wrap(response) : response;
  } catch (error) {
    upstreamAttempt?.recordFetchError();
    if (error instanceof ApiKeyQuotaDispatchError || error instanceof DeepSeekError) throw error;
    throw transportFailureError(error, options.signal, deadline.signal);
  } finally {
    clearTimeout(timer);
  }
};

/** Upstream `null`, `undefined` and strings are the only accepted nullable text values. */
const isAbsentOrString = (value: unknown): boolean => value === undefined || value === null || typeof value === "string";

const acceptUpstreamModel = (value: unknown): boolean => {
  const upstreamModel = nonEmptyString(value);
  return upstreamModel === null || DEEPSEEK_OFFICIAL_MODEL_ID_SET.has(upstreamModel.toLowerCase());
};

const normalizeToolCall = (value: unknown, index: number): NormalizationResult<Record<string, unknown>> => {
  if (!isRecord(value) || Array.isArray(value)) {
    return { ok: false, message: `Upstream tool call ${index} is not an object.` };
  }
  const id = exactNonEmptyString(value.id);
  if (!id) return { ok: false, message: `Upstream tool call ${index} is missing an id.` };
  if (value.type !== "function") {
    return { ok: false, message: `Upstream tool call ${index} has an unsupported type.` };
  }
  if (!isRecord(value.function) || Array.isArray(value.function)) {
    return { ok: false, message: `Upstream tool call ${index} is missing its function.` };
  }
  const name = exactNonEmptyString(value.function.name);
  if (!name) return { ok: false, message: `Upstream tool call ${index} is missing a function name.` };
  if (typeof value.function.arguments !== "string") {
    return { ok: false, message: `Upstream tool call ${index} has non-string arguments.` };
  }
  // OpenAI's Chat contract represents function arguments as an opaque string.
  // Do not parse or reserialize it: the Assistant validates it in its shared
  // application pipeline after this provider adapter returns.
  return { ok: true, value: { id, type: "function", function: { name, arguments: value.function.arguments } } };
};

/** Retains an Optional OpenAI wire field only when the upstream frame actually carries it. */
const optionalField = <T>(value: unknown, normalize: (input: unknown) => T | null, message: string): NormalizationResult<T | undefined> => {
  if (value === undefined || value === null) return { ok: true, value: undefined };
  const normalized = normalize(value);
  return normalized === null ? { ok: false, message } : { ok: true, value: normalized };
};

/** Normalizes the `function` object of one streaming tool-call delta. */
const normalizeToolCallDeltaFunction = (value: unknown, label: string): NormalizationResult<Record<string, unknown>> => {
  if (!isRecord(value) || Array.isArray(value)) return { ok: false, message: `${label} has an invalid function.` };
  const fn: Record<string, unknown> = {};
  const name = optionalField(value.name, exactNonEmptyString, `${label} has an invalid function name.`);
  if (!name.ok) return name;
  if (name.value !== undefined) fn.name = name.value;
  const args = optionalField(value.arguments, (input) => (typeof input === "string" ? input : null), `${label} has non-string arguments.`);
  if (!args.ok) return args;
  if (args.value !== undefined) fn.arguments = args.value;
  return { ok: true, value: fn };
};

/**
 * Normalizes a STREAMING tool-call delta. Unlike a buffered tool call, a delta
 * legitimately omits `id`, `type` and even `function.name` on continuation
 * frames, so only the fields this frame actually carries are retained.
 */
const normalizeToolCallDelta = (value: unknown, index: number): NormalizationResult<Record<string, unknown>> => {
  const label = `Upstream tool call delta ${index}`;
  if (!isRecord(value) || Array.isArray(value)) return { ok: false, message: `${label} is not an object.` };
  const delta: Record<string, unknown> = {};
  const callIndex = optionalField(value.index, nonNegativeInteger, `${label} has an invalid index.`);
  if (!callIndex.ok) return callIndex;
  if (callIndex.value !== undefined) delta.index = callIndex.value;
  const id = optionalField(value.id, exactNonEmptyString, `${label} has an invalid id.`);
  if (!id.ok) return id;
  if (id.value !== undefined) delta.id = id.value;
  const type = optionalField(value.type, (input) => (input === "function" ? (input as string) : null), `${label} has an unsupported type.`);
  if (!type.ok) return type;
  if (type.value !== undefined) delta.type = type.value;
  if (value.function !== undefined && value.function !== null) {
    const fn = normalizeToolCallDeltaFunction(value.function, label);
    if (!fn.ok) return fn;
    if (Object.keys(fn.value).length) delta.function = fn.value;
  }
  if (!Object.keys(delta).length) return { ok: false, message: `${label} carries no fields.` };
  return { ok: true, value: delta };
};

const normalizeToolCalls = (
  rawToolCalls: unknown,
  label: string,
  normalizeOne: (value: unknown, index: number) => NormalizationResult<Record<string, unknown>>
): NormalizationResult<Record<string, unknown>[] | undefined> => {
  if (rawToolCalls === undefined) return { ok: true, value: undefined };
  if (!Array.isArray(rawToolCalls)) return { ok: false, message: `${label} has invalid tool calls.` };
  const toolCalls: Record<string, unknown>[] = [];
  for (const [callIndex, call] of rawToolCalls.entries()) {
    const normalized = normalizeOne(call, callIndex);
    if (!normalized.ok) return normalized;
    toolCalls.push(normalized.value);
  }
  return { ok: true, value: toolCalls };
};

/**
 * Reduces a DeepSeek usage object to the OpenAI Chat Completions usage shape
 * the Assistant consumes. DeepSeek's extra cache/reasoning breakdowns are not
 * part of that contract and are not relayed.
 */
const normalizeUsage = (value: unknown): NormalizationResult<Record<string, number> | null> => {
  if (value === undefined || value === null) return { ok: true, value: null };
  if (!isRecord(value) || Array.isArray(value)) return { ok: false, message: "Upstream usage is not an object." };
  const promptTokens = nonNegativeInteger(value.prompt_tokens);
  const completionTokens = nonNegativeInteger(value.completion_tokens);
  const totalTokens = nonNegativeInteger(value.total_tokens);
  if (promptTokens === null || completionTokens === null || totalTokens === null) {
    return { ok: false, message: "Upstream usage is incomplete." };
  }
  return { ok: true, value: { prompt_tokens: promptTokens, completion_tokens: completionTokens, total_tokens: totalTokens } };
};

const choiceHasNoPayload = (content: unknown, reasoning: unknown, toolCalls: readonly unknown[] | undefined): boolean =>
  content === undefined && reasoning === undefined && !toolCalls?.length;

/** Mirrors the upstream `content` field: absent content becomes explicit JSON `null` only when another payload exists. */
const choiceContent = (content: unknown, toolCalls: readonly unknown[] | undefined): unknown => {
  if (content !== undefined && content !== null) return content;
  return toolCalls?.length ? null : "";
};

const normalizeChoiceMessage = (
  message: Record<string, unknown>,
  index: number
): NormalizationResult<Readonly<{ content: unknown; reasoning: string | null; toolCalls: Record<string, unknown>[] | undefined }>> => {
  if (message.role !== "assistant") {
    return { ok: false, message: `Upstream choice ${index} does not contain an assistant message.` };
  }
  if (!isAbsentOrString(message.content)) {
    return { ok: false, message: `Upstream choice ${index} has unsupported message content.` };
  }
  if (!isAbsentOrString(message.reasoning_content)) {
    return { ok: false, message: `Upstream choice ${index} has invalid reasoning content.` };
  }
  const toolCallsResult = normalizeToolCalls(message.tool_calls, `Upstream choice ${index}`, normalizeToolCall);
  if (!toolCallsResult.ok) return toolCallsResult;
  const toolCalls = toolCallsResult.value;
  const reasoning = typeof message.reasoning_content === "string" ? message.reasoning_content : null;
  if (choiceHasNoPayload(message.content, message.reasoning_content, toolCalls)) {
    return { ok: false, message: `Upstream choice ${index} has neither content nor a tool call.` };
  }
  return { ok: true, value: { content: message.content, reasoning, toolCalls } };
};

const normalizeChoice = (value: unknown, index: number): NormalizationResult<Record<string, unknown>> => {
  if (!isRecord(value) || Array.isArray(value)) {
    return { ok: false, message: `Upstream choice ${index} is not an object.` };
  }
  const choiceIndex = nonNegativeInteger(value.index);
  if (choiceIndex === null) return { ok: false, message: `Upstream choice ${index} has an invalid index.` };
  if (!isRecord(value.message) || Array.isArray(value.message)) {
    return { ok: false, message: `Upstream choice ${index} is missing an assistant message.` };
  }
  const normalizedMessagePart = normalizeChoiceMessage(value.message, index);
  if (!normalizedMessagePart.ok) return normalizedMessagePart;
  const { content, reasoning, toolCalls } = normalizedMessagePart.value;

  const finishReason = value.finish_reason;
  if (!isAbsentOrString(finishReason)) {
    return { ok: false, message: `Upstream choice ${index} has an invalid finish reason.` };
  }
  const normalizedMessage: Record<string, unknown> = { role: "assistant", content: choiceContent(content, toolCalls) };
  // DeepSeek thinking mode returns the chain of thought beside `content`.
  // Relay it 1:1 rather than dropping or logging it.
  if (reasoning !== null) normalizedMessage.reasoning_content = reasoning;
  if (toolCalls?.length) normalizedMessage.tool_calls = toolCalls;
  return {
    ok: true,
    value: {
      index: choiceIndex,
      message: normalizedMessage,
      finish_reason: finishReason ?? (toolCalls?.length ? "tool_calls" : "stop"),
    },
  };
};

/**
 * Reduces a DeepSeek buffered success payload to the OpenAI Chat Completions
 * shape the Assistant consumes. Unknown provider fields, including the
 * `system_fingerprint` and usage breakdowns, are not relayed or logged.
 */
export const normalizeDeepSeekChatCompletion = (value: unknown, requestedModel: string): NormalizationResult<Record<string, unknown>> => {
  if (!isRecord(value) || Array.isArray(value)) {
    return { ok: false, message: "Upstream did not return a Chat Completions object." };
  }
  const id = nonEmptyString(value.id);
  if (!id) return { ok: false, message: "Upstream Chat Completion is missing an id." };
  const created = nonNegativeInteger(value.created);
  if (created === null) return { ok: false, message: "Upstream Chat Completion has an invalid creation time." };
  if (value.object !== undefined && value.object !== "chat.completion") {
    return { ok: false, message: "Upstream did not return a Chat Completion." };
  }
  if (!acceptUpstreamModel(value.model)) {
    return { ok: false, message: "Upstream returned a different model than requested." };
  }
  if (!Array.isArray(value.choices) || value.choices.length === 0) {
    return { ok: false, message: "Upstream Chat Completion has no choices." };
  }
  const choices: Record<string, unknown>[] = [];
  for (const [index, choice] of value.choices.entries()) {
    const normalized = normalizeChoice(choice, index);
    if (!normalized.ok) return normalized;
    choices.push(normalized.value);
  }
  const usage = normalizeUsage(value.usage);
  if (!usage.ok) return usage;
  return {
    ok: true,
    value: {
      id,
      object: "chat.completion",
      created,
      // The client asked for one of the interchangeable official ids; echo that
      // id rather than the canonical upstream name.
      model: requestedModel,
      choices,
      ...(usage.value ? { usage: usage.value } : {}),
    },
  };
};

/** Normalizes the `delta` object of one streaming chunk choice. */
const normalizeChunkDelta = (value: unknown, label: string): NormalizationResult<Record<string, unknown>> => {
  if (!isRecord(value) || Array.isArray(value)) return { ok: false, message: `${label} has an invalid delta.` };
  const normalizedDelta: Record<string, unknown> = {};
  const role = optionalField(value.role, (input) => (typeof input === "string" ? input : null), `${label} has an invalid delta role.`);
  if (!role.ok) return role;
  if (role.value !== undefined) normalizedDelta.role = role.value;
  for (const field of ["content", "reasoning_content"] as const) {
    const text = optionalField(value[field], (input) => (typeof input === "string" ? input : null), `${label} has invalid ${field}.`);
    if (!text.ok) return text;
    if (text.value !== undefined) normalizedDelta[field] = text.value;
  }
  const toolCalls = normalizeToolCalls(value.tool_calls, label, normalizeToolCallDelta);
  if (!toolCalls.ok) return toolCalls;
  if (toolCalls.value?.length) normalizedDelta.tool_calls = toolCalls.value;
  return { ok: true, value: normalizedDelta };
};

const normalizeChoiceDelta = (value: unknown, index: number): NormalizationResult<Record<string, unknown>> => {
  const label = `Upstream chunk choice ${index}`;
  if (!isRecord(value) || Array.isArray(value)) return { ok: false, message: `${label} is not an object.` };
  const choiceIndex = nonNegativeInteger(value.index);
  if (choiceIndex === null) return { ok: false, message: `${label} has an invalid index.` };
  const finishReason = value.finish_reason;
  if (!isAbsentOrString(finishReason)) return { ok: false, message: `${label} has an invalid finish reason.` };
  const normalizedChoice: Record<string, unknown> = { index: choiceIndex };
  const delta = optionalField(value.delta, (input) => input, `${label} has an invalid delta.`);
  if (!delta.ok) return delta;
  if (delta.value !== undefined) {
    const normalizedDelta = normalizeChunkDelta(delta.value, label);
    if (!normalizedDelta.ok) return normalizedDelta;
    normalizedChoice.delta = normalizedDelta.value;
  }
  if (finishReason !== undefined) normalizedChoice.finish_reason = finishReason;
  return { ok: true, value: normalizedChoice };
};

/**
 * Reduces one DeepSeek streaming chunk to the OpenAI `chat.completion.chunk`
 * shape. Chunks are relayed as they arrive, so this validates the frame rather
 * than buffering it.
 */
export const normalizeDeepSeekChatCompletionChunk = (value: unknown, requestedModel: string): NormalizationResult<Record<string, unknown>> => {
  if (!isRecord(value) || Array.isArray(value)) {
    return { ok: false, message: "Upstream did not return a Chat Completions chunk." };
  }
  const id = nonEmptyString(value.id);
  if (!id) return { ok: false, message: "Upstream Chat Completion chunk is missing an id." };
  const created = nonNegativeInteger(value.created);
  if (created === null) return { ok: false, message: "Upstream Chat Completion chunk has an invalid creation time." };
  if (value.object !== undefined && value.object !== "chat.completion.chunk") {
    return { ok: false, message: "Upstream did not return a Chat Completion chunk." };
  }
  if (!acceptUpstreamModel(value.model)) {
    return { ok: false, message: "Upstream returned a different model than requested." };
  }
  if (!Array.isArray(value.choices)) {
    return { ok: false, message: "Upstream Chat Completion chunk has no choices array." };
  }
  const choices: Record<string, unknown>[] = [];
  for (const [index, choice] of value.choices.entries()) {
    const normalized = normalizeChoiceDelta(choice, index);
    if (!normalized.ok) return normalized;
    choices.push(normalized.value);
  }
  const usage = normalizeUsage(value.usage);
  if (!usage.ok) return usage;
  return {
    ok: true,
    value: {
      id,
      object: "chat.completion.chunk",
      created,
      model: requestedModel,
      choices,
      ...(usage.value ? { usage: usage.value } : {}),
    },
  };
};

/** True when a normalized chunk carries output a client can display. */
export const deepSeekChunkHasSemanticOutput = (chunk: Record<string, unknown>): boolean => {
  const choices = chunk.choices;
  if (!Array.isArray(choices)) return false;
  for (const choice of choices) {
    if (!isRecord(choice) || Array.isArray(choice) || !isRecord(choice.delta) || Array.isArray(choice.delta)) continue;
    const delta = choice.delta;
    if (typeof delta.content === "string" && delta.content.length > 0) return true;
    // Reasoning is streamed to the client as it is generated, so it is real
    // semantic output even when the final answer has not started yet.
    if (typeof delta.reasoning_content === "string" && delta.reasoning_content.length > 0) return true;
    if (Array.isArray(delta.tool_calls) && delta.tool_calls.length > 0) return true;
  }
  return false;
};

export type DeepSeekStreamFailureKind = "malformed_event" | "invalid_chunk" | "frame_too_large" | "premature_eof" | "read_error" | "inactivity_timeout";

export class DeepSeekStreamError extends Error {
  readonly kind: DeepSeekStreamFailureKind;

  constructor(message: string, options?: ErrorOptions & { kind?: DeepSeekStreamFailureKind }) {
    super(message, options);
    this.name = "DeepSeekStreamError";
    this.kind = options?.kind ?? "read_error";
  }
}

export type DeepSeekStreamFrame =
  Readonly<{ kind: "comment"; text: string }> | Readonly<{ kind: "chunk"; value: Record<string, unknown> }> | Readonly<{ kind: "done" }>;

export type DeepSeekStreamOptions = Readonly<{
  /** Request-level cancellation, composed with the stream deadlines. */
  signal?: AbortSignal;
  /** Bounds the wait for the first upstream frame. */
  firstEventTimeoutMs?: number;
  /** Bounds the gap between subsequent upstream frames. */
  inactivityTimeoutMs?: number;
}>;

/**
 * Parses one complete SSE event block. `data:` payloads are the OpenAI Chat
 * Completions stream contract; DeepSeek's `: keep-alive` comments are relayed
 * verbatim because they are what keeps a long thinking turn from looking like
 * an idle connection to an edge proxy. Any other SSE field (`event:`, `id:`,
 * `retry:`) is not part of the Chat Completions contract and is not relayed.
 */
const parseSseEventBlock = (raw: string, requestedModel: string): DeepSeekStreamFrame | null => {
  const data: string[] = [];
  const comments: string[] = [];
  for (const line of raw.split(SSE_LINE_BOUNDARY)) {
    if (!line) continue;
    if (line.startsWith(":")) comments.push(line);
    else if (line === "data") data.push("");
    else if (line.startsWith("data:")) data.push(line.slice(5).replace(/^ /, ""));
  }
  if (!data.length) return comments.length ? { kind: "comment", text: comments.join("\n") } : null;
  const payload = data.join("\n");
  if (payload.trim() === "[DONE]") return { kind: "done" };
  let parsed: unknown;
  try {
    parsed = JSON.parse(payload);
  } catch (cause) {
    throw new DeepSeekStreamError("Upstream emitted malformed Chat Completions SSE JSON.", { cause, kind: "malformed_event" });
  }
  const normalized = normalizeDeepSeekChatCompletionChunk(parsed, requestedModel);
  if (!normalized.ok) {
    throw new DeepSeekStreamError(`Upstream emitted an invalid Chat Completions chunk: ${normalized.message}`, { kind: "invalid_chunk" });
  }
  return { kind: "chunk", value: normalized.value };
};

/**
 * Splits an incremental SSE byte stream into relayable frames, retaining the
 * unterminated tail between reads. Frames that carry neither `data:` nor a
 * comment are skipped, and the byte bound is enforced on the retained tail.
 */
const createDeepSeekFrameReader = (
  requestedModel: string
): Readonly<{ push: (incoming: Uint8Array | null) => void; shift: () => DeepSeekStreamFrame | null }> => {
  const decoder = new TextDecoder();
  const buffered = { text: "" };
  const push = (incoming: Uint8Array | null): void => {
    buffered.text += incoming === null ? decoder.decode() : decoder.decode(incoming, { stream: true });
    if (buffered.text.length > MAX_DEEPSEEK_SSE_FRAME_BYTES) {
      throw new DeepSeekStreamError("Upstream SSE frame exceeded the gateway bound.", { kind: "frame_too_large" });
    }
  };
  const shift = (): DeepSeekStreamFrame | null => {
    for (;;) {
      const match = SSE_FRAME_BOUNDARY.exec(buffered.text);
      if (!match) return null;
      const raw = buffered.text.slice(0, match.index);
      buffered.text = buffered.text.slice(match.index + match[0].length);
      const frame = parseSseEventBlock(raw, requestedModel);
      if (frame) return frame;
    }
  };
  return { push, shift };
};

/**
 * Owns one upstream SSE read loop: the reader lock, the first-event and
 * inactivity watchdogs, and the frame queue. Kept outside the generator so
 * each concern is independently readable.
 */
const createDeepSeekStreamSession = (
  reader: ReadableStreamDefaultReader<Uint8Array>,
  requestedModel: string,
  deadline: AbortController,
  options: DeepSeekStreamOptions
): Readonly<{ next: () => Promise<DeepSeekStreamFrame | null>; finish: () => void }> => {
  const signal = options.signal ? AbortSignal.any([options.signal, deadline.signal]) : deadline.signal;
  const frames = createDeepSeekFrameReader(requestedModel);
  const state = { eof: false, released: false, sawFrame: false, watchdog: null as ReturnType<typeof setTimeout> | null };

  const stopWatchdog = (): void => {
    if (state.watchdog !== null) clearTimeout(state.watchdog);
  };
  const resetWatchdog = (): void => {
    stopWatchdog();
    const timeoutMs = state.sawFrame
      ? (options.inactivityTimeoutMs ?? STREAM_INACTIVITY_DEADLINE_MS)
      : (options.firstEventTimeoutMs ?? STREAM_FIRST_EVENT_DEADLINE_MS);
    state.watchdog = setTimeout(() => {
      deadline.abort(new DOMException("DeepSeek Chat Completions stream stalled.", "TimeoutError"));
    }, timeoutMs);
  };
  const releaseReaderLock = (): void => {
    try {
      reader.releaseLock();
    } catch {
      // The reader may already be released by a failing upstream stream.
    }
  };
  const finish = (): void => {
    stopWatchdog();
    if (state.released) return;
    state.released = true;
    try {
      const cancellation = reader.cancel("DeepSeek Chat Completions stream finished");
      releaseReaderLock();
      void cancellation.catch(() => {});
    } catch {
      releaseReaderLock();
    }
  };

  resetWatchdog();
  const next = async (): Promise<DeepSeekStreamFrame | null> => {
    for (;;) {
      const frame = frames.shift();
      if (frame) {
        state.sawFrame = true;
        resetWatchdog();
        return frame;
      }
      if (state.eof) return null;
      const result = await raceReaderRead(reader, signal);
      state.eof = result.done;
      frames.push(result.done ? null : result.value);
    }
  };
  return { next, finish };
};

/** Races one upstream read against the composed cancellation/deadline signal. */
const raceReaderRead = async (reader: ReadableStreamDefaultReader<Uint8Array>, signal: AbortSignal): Promise<ReadableStreamReadResult<Uint8Array>> => {
  let onAbort = (): void => {};
  const aborted = new Promise<never>((_, reject) => {
    onAbort = () => {
      reject(signal.reason instanceof Error ? signal.reason : new DOMException("The stream was aborted.", "AbortError"));
    };
    signal.addEventListener("abort", onAbort, { once: true });
    if (signal.aborted) onAbort();
  });
  try {
    return await Promise.race([reader.read(), aborted]);
  } finally {
    signal.removeEventListener("abort", onAbort);
  }
};

/**
 * Reads the upstream Chat Completions SSE body and yields validated frames.
 *
 * `data:` frames are the OpenAI Chat Completions stream contract and are
 * normalized before they leave this module. DeepSeek's documented
 * `: keep-alive` comment frames are forwarded verbatim, because they are what
 * keeps a long thinking turn from looking like an idle connection to an edge
 * proxy. Any other SSE field (`event:`, `id:`, `retry:`) is not part of the
 * Chat Completions contract and is not relayed.
 */
export async function* iterateDeepSeekChatCompletionStream(
  response: Response,
  requestedModel: string,
  options: DeepSeekStreamOptions = {}
): AsyncGenerator<DeepSeekStreamFrame, void, unknown> {
  const reader = response.body?.getReader();
  if (!reader) throw new DeepSeekStreamError("Upstream returned no Chat Completions stream body.", { kind: "premature_eof" });

  const deadline = new AbortController();
  const session = createDeepSeekStreamSession(reader, requestedModel, deadline, options);
  try {
    for (;;) {
      const frame = await session.next();
      if (!frame) throw new DeepSeekStreamError("Upstream Chat Completions stream ended before [DONE].", { kind: "premature_eof" });
      yield frame;
      if (frame.kind === "done") return;
    }
  } catch (error) {
    if (error instanceof DeepSeekStreamError) throw error;
    if (deadline.signal.aborted) {
      throw new DeepSeekStreamError("DeepSeek Chat Completions stream stalled.", { cause: error, kind: "inactivity_timeout" });
    }
    if (isTimeoutError(error) || (error instanceof Error && error.name === "AbortError")) throw error;
    throw new DeepSeekStreamError("DeepSeek Chat Completions stream failed.", { cause: error, kind: "read_error" });
  } finally {
    session.finish();
  }
}
