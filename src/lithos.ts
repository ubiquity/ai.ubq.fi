import { getString, isRecord } from "./utils.ts";

/**
 * LithosAI API transport — a chat-completions-only upstream provider.
 *
 * Like Cerebras and DeepSeek this is a "special upstream provider": a request
 * addressed to one of its model ids is dispatched straight to the provider's
 * documented OpenAI-compatible Chat Completions endpoint and never races or
 * falls back to another provider.
 *
 * Provider facts (probed live against `https://api.lithosai.cloud/v1`,
 * 2026-09-23):
 * - base URL `https://api.lithosai.cloud/v1`, Chat Completions at
 *   `/chat/completions`. There is NO `/v1/responses` endpoint (probed: 404 with
 *   an empty body), so this provider is chat-only and can never serve the
 *   Responses route.
 * - auth is `Authorization: Bearer $LITHOSAI_API_KEY`; keys are prefixed
 *   `lith_sk_`.
 * - `GET /v1/models` returned exactly the eight ids in `LITHOS_MODEL_IDS`,
 *   carrying only `id`/`object`/`created`/`owned_by` — no context, capability or
 *   price metadata. Every one of the eight answered HTTP 200 on 2026-09-23.
 * - the context window is 1,048,576 tokens for all eight.
 * - `reasoning_effort` accepted exactly none/minimal/low/medium/high/xhigh/max
 *   (each probed at HTTP 200 by varying only that field; `ultra`, `bogus` and
 *   the empty string were refused with 400). They are sent VERBATIM: no wire
 *   map is applied, and the provider's own default for an omitted effort was
 *   not probed.
 * - Kimi K3 requires `n: 1`, `presence_penalty: 0.0` and
 *   `frequency_penalty: 0.0`; `top_k` is accepted as a non-standard parameter.
 *
 * Observed wire behavior (2026-09-23 probes):
 * - a buffered success carries `message.reasoning_content` beside `content`,
 *   present and `null` when the model produced no reasoning, and
 *   `message.tool_calls`, present and `null` when there were no calls.
 *   `prompt_tokens_details` is likewise present and `null`, while
 *   `completion_tokens_details.reasoning_tokens` is a real integer.
 * - streaming frames are `data: {...}` lines terminated by `data: [DONE]`, and
 *   usage is reported UNCONDITIONALLY rather than behind
 *   `stream_options.include_usage`. The authoritative totals arrive on a
 *   trailing chunk whose `choices` is an EMPTY ARRAY, which is why the chunk
 *   normalizer below accepts one.
 * - tool-call ids look like `name:index`; they are opaque strings on this wire
 *   and are relayed unchanged.
 * - every admitted response and every refusal carries the six `x-ratelimit-*`
 *   headers in `LITHOS_RATE_LIMIT_HEADERS`, and there is no provider
 *   request-id header at all.
 * - the error envelope is not uniform: auth/validation/model failures use
 *   `{"error":{...}}`, while an engine parameter violation uses
 *   `{"object":"error","message":...,"type":"BadRequestError","code":400}` with
 *   an INTEGER `code`. No single reader may assume one shape.
 */

/** The eight ids `GET /v1/models` returned verbatim on 2026-09-23, case preserved. */
export const LITHOS_MODEL_IDS = [
  "deepseek-ai/DeepSeek-V4.1-Flash",
  "deepseek-ai/DeepSeek-V4.1-Flash-fast",
  "deepseek-ai/DeepSeek-V4.1-Flash-ultra",
  "deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat",
  "moonshotai/Kimi-K3",
  "moonshotai/Kimi-K3-fast",
  "moonshotai/Kimi-K3-ultra",
  "moonshotai/Kimi-K3-ultra-chat",
] as const;

/** Operator-facing names, kept beside the ids they describe. */
export const LITHOS_DISPLAY_NAMES: Readonly<Record<string, string>> = {
  "deepseek-ai/DeepSeek-V4.1-Flash": "LithosAI DeepSeek V4.1 Flash",
  "deepseek-ai/DeepSeek-V4.1-Flash-fast": "LithosAI DeepSeek V4.1 Flash (fast)",
  "deepseek-ai/DeepSeek-V4.1-Flash-ultra": "LithosAI DeepSeek V4.1 Flash (ultra)",
  "deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat": "LithosAI DeepSeek V4.1 Flash (ultra, chat)",
  "moonshotai/Kimi-K3": "LithosAI Kimi K3",
  "moonshotai/Kimi-K3-fast": "LithosAI Kimi K3 (fast)",
  "moonshotai/Kimi-K3-ultra": "LithosAI Kimi K3 (ultra)",
  "moonshotai/Kimi-K3-ultra-chat": "LithosAI Kimi K3 (ultra, chat)",
};

export const LITHOS_API_KEY_ENV = "LITHOSAI_API_KEY";
export const LITHOS_CHAT_COMPLETIONS_URL = "https://api.lithosai.cloud/v1/chat/completions";
export const LITHOS_CONTEXT_WINDOW_TOKENS = 1_048_576;
export const LITHOS_EFFECTIVE_CONTEXT_WINDOW_PERCENT = 95;

/**
 * The seven `reasoning_effort` values the API accepted on 2026-09-23, in the
 * order they were advertised. They are sent verbatim, so this is the whole
 * advertised tier list: no value was added, reordered into a ladder that was
 * not observed, or omitted.
 */
export const LITHOS_REASONING_LEVELS = ["none", "minimal", "low", "medium", "high", "xhigh", "max"] as const;

export type LithosReasoningLevel = (typeof LITHOS_REASONING_LEVELS)[number];

/**
 * Gateway default when a request omits `reasoning_effort`. The provider's own
 * default was NOT probed (every probe passed an explicit tier), so this is a
 * local choice — the middle verified tier — and not a vendor-documented value.
 */
export const LITHOS_DEFAULT_REASONING_EFFORT: LithosReasoningLevel = "medium";

/**
 * The six rate-limit response headers every LithosAI response and refusal
 * carried on 2026-09-23. They are the same names the Sentinel upstream capture
 * allowlist already retains; there is deliberately no provider request-id
 * header in this list, because the provider sends none.
 */
export const LITHOS_RATE_LIMIT_HEADERS = [
  "x-ratelimit-limit-requests",
  "x-ratelimit-remaining-requests",
  "x-ratelimit-reset-requests",
  "x-ratelimit-limit-tokens",
  "x-ratelimit-remaining-tokens",
  "x-ratelimit-reset-tokens",
] as const;

export type LithosErrorCode = "lithos_api_key_missing" | "lithos_request_invalid" | "lithos_upstream_unreachable" | "gateway_timeout";

export class LithosError extends Error {
  readonly code: LithosErrorCode;
  readonly status: number;

  constructor(message: string, code: LithosErrorCode, status: number) {
    super(message);
    this.name = "LithosError";
    this.code = code;
    this.status = status;
  }
}

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

const nonNegativeInteger = (value: unknown): number | null => {
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  const normalized = Math.trunc(value);
  return normalized >= 0 ? normalized : null;
};

/** Upstream `null`, `undefined` and strings are the only accepted nullable text values. */
const isAbsentOrString = (value: unknown): boolean => value === undefined || value === null || typeof value === "string";

/** Retains an Optional OpenAI wire field only when the upstream frame actually carries it. */
const optionalField = <T>(value: unknown, normalize: (input: unknown) => T | null, message: string): NormalizationResult<T | undefined> => {
  if (value === undefined || value === null) return { ok: true, value: undefined };
  const normalized = normalize(value);
  return normalized === null ? { ok: false, message } : { ok: true, value: normalized };
};

const getEnv = (key: string): string | undefined => {
  try {
    return Deno.env.get(key);
  } catch {
    return undefined;
  }
};

export const readLithosApiKey = (): string | null => nonEmptyString(getEnv(LITHOS_API_KEY_ENV));

/**
 * Resolves the key for one dispatch. A supplied empty or whitespace-only value
 * is treated as absent rather than as an override that switches the provider
 * off, exactly as the Cerebras and DeepSeek seams do.
 */
export const requireLithosApiKey = (supplied?: string | null): string => {
  const apiKey = supplied === undefined ? readLithosApiKey() : nonEmptyString(supplied);
  if (apiKey) return apiKey;
  throw new LithosError("The requested model is not configured.", "lithos_api_key_missing", 503);
};

/**
 * Canonical upstream id for every client-facing id the provider publishes.
 *
 * This is an identity map: all eight ids are sent verbatim, and no alias,
 * casing or legacy spelling was observed to be rewritten by the API. It exists
 * so the dispatch seam has one table to read, exactly as the DeepSeek map does,
 * and so a future alias has one place to be declared.
 */
export const LITHOS_UPSTREAM_MODEL_BY_ID: Readonly<Record<string, string>> = Object.freeze(
  Object.fromEntries(LITHOS_MODEL_IDS.map((model) => [model, model] as const))
);

/**
 * Lowercase lookup index over the same identity map, so the table a later
 * dispatch seam reads and the lookup it calls can never drift apart. The
 * provider's own ids are mixed-case, so a client that spells one differently
 * must still resolve to the canonical id that goes upstream. A Map is used
 * rather than an object so an inherited property name (`constructor`) can never
 * resolve as a model.
 */
const LITHOS_UPSTREAM_MODEL_BY_LOWERCASE_ID = new Map<string, string>(
  Object.entries(LITHOS_UPSTREAM_MODEL_BY_ID).map(([id, canonical]) => [id.toLowerCase(), canonical] as const)
);

/**
 * Resolves the canonical upstream model for a client-facing model id, or null
 * when the id is not a LithosAI model. Comparison is case-insensitive, matching
 * the gateway's model normalization elsewhere.
 */
export const lithosUpstreamModelFor = (model: string): string | null => LITHOS_UPSTREAM_MODEL_BY_LOWERCASE_ID.get(model.trim().toLowerCase()) ?? null;

/**
 * The upstream echoes the model it served. "Normalizing" an echo means trimming
 * it and stripping a leading `/models/` route prefix, because this vendor's
 * route shape is `/models/{author}/{slug}`. Three things are accepted, and
 * anything else describes a different model than the one the client asked for:
 *
 * 1. No echo at all (`undefined`/`null`, or nothing but whitespace). The
 *    provider said nothing about the model, which is not a mismatch.
 * 2. A normalized echo that equals the canonical id for the requested model,
 *    case-insensitively. This is the ordinary case: seven of the eight ids echo
 *    themselves verbatim.
 * 3. A normalized echo that equals the requested id's trailing path segment
 *    (slug), case-insensitively — or a `-`-delimited leading part of that slug.
 *    This is NOT a special-case table: the rule is derived from the requested id
 *    and holds for any id. Live probes on 2026-09-23 answered EVERY completion
 *    for `deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat` with
 *    `model: "/models/DeepSeek-V4.1-Flash"` (five sequential probes, identical;
 *    a longer prompt changed nothing), which is the engine folding that tier
 *    onto the base `DeepSeek-V4.1-Flash` slug while
 *    `GET /v1/models/deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat` still reports
 *    the full id. That echo is a stable property of the wire, so an
 *    exact-equality check would reject every real response for one of the eight
 *    advertised models. Do not tighten this back into equality; re-probe the
 *    `-ultra-chat` tier before narrowing it.
 */
const acceptUpstreamModel = (value: unknown, requestedModel: string): boolean => {
  // Rule 1: a missing or text-free echo carries no model claim to compare.
  if (value === undefined || value === null) return true;
  const echo =
    typeof value === "string"
      ? value
          .trim()
          .replace(/^\/models\//, "")
          .trim()
          .toLowerCase()
      : "";
  if (echo === "") return true;
  // The id the client asked for, in this provider's canonical spelling.
  const requested = (lithosUpstreamModelFor(requestedModel) ?? requestedModel).trim().toLowerCase();
  if (echo === requested) return true; // rule 2
  const slug = requested.slice(requested.lastIndexOf("/") + 1);
  return echo === slug || slug.startsWith(`${echo}-`); // rule 3
};

const normalizeLithosToolCall = (value: unknown, index: number): NormalizationResult<Record<string, unknown>> => {
  if (!isRecord(value) || Array.isArray(value)) return { ok: false, message: `Upstream tool call ${index} is not an object.` };
  const id = exactNonEmptyString(value.id);
  if (!id) return { ok: false, message: `Upstream tool call ${index} is missing an id.` };
  if (value.type !== "function") return { ok: false, message: `Upstream tool call ${index} has an unsupported type.` };
  if (!isRecord(value.function) || Array.isArray(value.function)) {
    return { ok: false, message: `Upstream tool call ${index} is missing its function.` };
  }
  const name = exactNonEmptyString(value.function.name);
  if (!name) return { ok: false, message: `Upstream tool call ${index} is missing a function name.` };
  if (typeof value.function.arguments !== "string") {
    return { ok: false, message: `Upstream tool call ${index} has non-string arguments.` };
  }
  // Tool-call ids arrive as `name:index` on this provider. They are opaque
  // OpenAI wire values, so they are relayed unchanged: nothing here parses or
  // repairs the provider-specific id shape, and OpenAI's Chat contract
  // represents function arguments as an opaque string that is not reparsed.
  return { ok: true, value: { id, type: "function", function: { name, arguments: value.function.arguments } } };
};

/** Normalizes the `function` object of one streaming tool-call delta. */
const normalizeLithosToolCallDeltaFunction = (value: unknown, label: string): NormalizationResult<Record<string, unknown>> => {
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
const normalizeLithosToolCallDelta = (value: unknown, index: number): NormalizationResult<Record<string, unknown>> => {
  const label = `Upstream tool call delta ${index}`;
  if (!isRecord(value) || Array.isArray(value)) return { ok: false, message: `${label} is not an object.` };
  const delta: Record<string, unknown> = {};
  const callIndex = optionalField(value.index, nonNegativeInteger, `${label} has an invalid index.`);
  if (!callIndex.ok) return callIndex;
  if (callIndex.value !== undefined) delta.index = callIndex.value;
  const id = optionalField(value.id, exactNonEmptyString, `${label} has an invalid id.`);
  if (!id.ok) return id;
  if (id.value !== undefined) delta.id = id.value;
  const type = optionalField(value.type, (input) => (input === "function" ? "function" : null), `${label} has an unsupported type.`);
  if (!type.ok) return type;
  if (type.value !== undefined) delta.type = type.value;
  if (value.function !== undefined && value.function !== null) {
    const fn = normalizeLithosToolCallDeltaFunction(value.function, label);
    if (!fn.ok) return fn;
    if (Object.keys(fn.value).length) delta.function = fn.value;
  }
  if (!Object.keys(delta).length) return { ok: false, message: `${label} carries no fields.` };
  return { ok: true, value: delta };
};

/**
 * Normalizes the tool calls of one message or delta. `undefined` means the
 * frame carried no tool calls at all; callers pass `?? undefined` because this
 * provider reports "none" as an explicit `null` rather than an absent field.
 */
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
 * The official Chat chunk/message schema types `refusal` as an optional string
 * or `null`. A string is payload this gateway must carry (a refusal is
 * answer-bearing output); `null` is absence; any other type is malformed input
 * and is rejected rather than silently dropped.
 */
const normalizeRefusal = (value: unknown, label: string): NormalizationResult<string | null> => {
  if (value === undefined || value === null) return { ok: true, value: null };
  if (typeof value === "string") return { ok: true, value };
  return { ok: false, message: `${label} has an invalid refusal.` };
};

/**
 * One nested usage counter, validated against the total it is defined as a
 * subset of. An absent or impossible value stays absent, so no reader
 * downstream can mistake a missing measurement for a measured zero.
 */
const usageDetailCounter = (container: unknown, key: string, ceiling: number): number | null => {
  if (!isRecord(container) || Array.isArray(container)) return null;
  const counter = nonNegativeInteger(container[key]);
  if (counter === null || counter > ceiling) return null;
  return counter;
};

/**
 * Reduces a LithosAI usage object to the OpenAI Chat Completions usage shape
 * the Assistant consumes.
 *
 * The provider reports `prompt_tokens_details` as an explicit `null` when it
 * measured nothing, and an explicit `null` is NOT a measured zero: a null (or
 * absent) details object leaves the matching detail object out of the
 * projection entirely. `completion_tokens_details.reasoning_tokens` is a real
 * integer on this wire and is relayed under its official OpenAI name.
 */
const normalizeUsage = (value: unknown): NormalizationResult<Record<string, unknown> | null> => {
  if (value === undefined || value === null) return { ok: true, value: null };
  if (!isRecord(value) || Array.isArray(value)) return { ok: false, message: "Upstream usage is not an object." };
  const promptTokens = nonNegativeInteger(value.prompt_tokens);
  const completionTokens = nonNegativeInteger(value.completion_tokens);
  const totalTokens = nonNegativeInteger(value.total_tokens);
  if (promptTokens === null || completionTokens === null || totalTokens === null) {
    return { ok: false, message: "Upstream usage is incomplete." };
  }
  const cachedTokens = usageDetailCounter(value.prompt_tokens_details, "cached_tokens", promptTokens);
  const reasoningTokens = usageDetailCounter(value.completion_tokens_details, "reasoning_tokens", completionTokens);
  return {
    ok: true,
    value: {
      prompt_tokens: promptTokens,
      completion_tokens: completionTokens,
      total_tokens: totalTokens,
      ...(cachedTokens === null ? {} : { prompt_tokens_details: { cached_tokens: cachedTokens } }),
      ...(reasoningTokens === null ? {} : { completion_tokens_details: { reasoning_tokens: reasoningTokens } }),
    },
  };
};

const choiceHasNoPayload = (content: unknown, reasoning: string | null, refusal: string | null, toolCalls: readonly unknown[] | undefined): boolean =>
  content === undefined && reasoning === null && refusal === null && !toolCalls?.length;

/** Mirrors the upstream `content` field: absent content becomes explicit JSON `null` only when a tool call exists. */
const choiceContent = (content: unknown, toolCalls: readonly unknown[] | undefined): unknown => {
  if (content !== undefined && content !== null) return content;
  return toolCalls?.length ? null : "";
};

const normalizeChoiceMessage = (
  message: Record<string, unknown>,
  index: number
): NormalizationResult<Readonly<{ content: unknown; reasoning: string | null; refusal: string | null; toolCalls: Record<string, unknown>[] | undefined }>> => {
  if (message.role !== "assistant") {
    return { ok: false, message: `Upstream choice ${index} does not contain an assistant message.` };
  }
  if (!isAbsentOrString(message.content)) {
    return { ok: false, message: `Upstream choice ${index} has unsupported message content.` };
  }
  if (!isAbsentOrString(message.reasoning_content)) {
    return { ok: false, message: `Upstream choice ${index} has invalid reasoning content.` };
  }
  // An explicit upstream `null` means "no reasoning produced", so it stays
  // absent from the projection rather than becoming an empty string.
  const reasoning = typeof message.reasoning_content === "string" ? message.reasoning_content : null;
  const refusal = normalizeRefusal(message.refusal, `Upstream choice ${index}`);
  if (!refusal.ok) return refusal;
  // `tool_calls` is present and `null` on an ordinary response from this
  // provider, so a null is absence, not malformed input.
  const toolCallsResult = normalizeToolCalls(message.tool_calls ?? undefined, `Upstream choice ${index}`, normalizeLithosToolCall);
  if (!toolCallsResult.ok) return toolCallsResult;
  const toolCalls = toolCallsResult.value;
  if (choiceHasNoPayload(message.content, reasoning, refusal.value, toolCalls)) {
    return { ok: false, message: `Upstream choice ${index} has neither content nor a tool call.` };
  }
  return { ok: true, value: { content: message.content, reasoning, refusal: refusal.value, toolCalls } };
};

const normalizeChoice = (value: unknown, index: number): NormalizationResult<Record<string, unknown>> => {
  if (!isRecord(value) || Array.isArray(value)) return { ok: false, message: `Upstream choice ${index} is not an object.` };
  const choiceIndex = nonNegativeInteger(value.index);
  if (choiceIndex === null) return { ok: false, message: `Upstream choice ${index} has an invalid index.` };
  if (!isRecord(value.message) || Array.isArray(value.message)) {
    return { ok: false, message: `Upstream choice ${index} is missing an assistant message.` };
  }
  const normalizedMessagePart = normalizeChoiceMessage(value.message, index);
  if (!normalizedMessagePart.ok) return normalizedMessagePart;
  const { content, reasoning, refusal, toolCalls } = normalizedMessagePart.value;

  const finishReason = value.finish_reason;
  if (!isAbsentOrString(finishReason)) return { ok: false, message: `Upstream choice ${index} has an invalid finish reason.` };
  const normalizedMessage: Record<string, unknown> = {
    role: "assistant",
    content: choiceContent(content, toolCalls),
  };
  if (reasoning !== null) normalizedMessage.reasoning_content = reasoning;
  if (refusal !== null) normalizedMessage.refusal = refusal;
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
 * Reduces a LithosAI buffered success payload to the OpenAI Chat Completions
 * shape the Assistant consumes. Unknown provider fields, including diagnostics,
 * are not relayed or logged.
 */
export const normalizeLithosChatCompletion = (value: unknown, requestedModel: string): NormalizationResult<Record<string, unknown>> => {
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
  if (!acceptUpstreamModel(value.model, requestedModel)) {
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
      // The client asked for one of the provider's own ids; echo that id rather
      // than relaying the upstream spelling back at it.
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
  const refusal = normalizeRefusal(value.refusal, label);
  if (!refusal.ok) return refusal;
  if (refusal.value !== null) normalizedDelta.refusal = refusal.value;
  const toolCalls = normalizeToolCalls(value.tool_calls ?? undefined, label, normalizeLithosToolCallDelta);
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
 * Reduces one LithosAI streaming chunk to the OpenAI `chat.completion.chunk`
 * shape. Chunks are relayed as they arrive, so this validates the frame rather
 * than buffering it.
 *
 * This is the deliberate delta from the DeepSeek-shaped chunk normalizer: this
 * provider reports usage UNCONDITIONALLY and its authoritative totals arrive on
 * a trailing chunk whose `choices` is an EMPTY ARRAY. Such a chunk is valid
 * here and is never rejected, and usage is relayed whenever the frame carries
 * it — it is never gated on `stream_options.include_usage`, which this
 * provider's wire contract does not use.
 */
export const normalizeLithosChatCompletionChunk = (value: unknown, requestedModel: string): NormalizationResult<Record<string, unknown>> => {
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
  if (!acceptUpstreamModel(value.model, requestedModel)) {
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
