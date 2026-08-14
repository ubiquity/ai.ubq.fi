import { type ApiKeyProviderDispatch, ApiKeyQuotaDispatchError } from "./api_key_policy.ts";
import { BUFFERED_INFERENCE_DEADLINE_MS } from "./inference_deadline.ts";
import { getString, isRecord } from "./utils.ts";

export const CEREBRAS_GPT_OSS_120B_MODEL = "cerebras/gpt-oss-120b";
export const CEREBRAS_CHAT_COMPLETIONS_URL = "https://api.cerebras.ai/v1/chat/completions";

const CEREBRAS_API_KEY_ENV = "CEREBRAS_API_KEY";
const MAX_CEREBRAS_PROVIDER_REQUEST_ID_LENGTH = 256;
const CEREBRAS_UNSUPPORTED_SCHEMA_FIELDS = new Set([
  "format",
  "maxItems",
  "maxLength",
  "maxProperties",
  "minItems",
  "minLength",
  "minProperties",
  "pattern",
  "uniqueItems",
]);
let cerebrasFetchTimeoutMs = BUFFERED_INFERENCE_DEADLINE_MS;

export type CerebrasFetch = (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>;

export type CerebrasErrorCode =
  | "cerebras_api_key_missing"
  | "cerebras_request_invalid"
  | "cerebras_upstream_unreachable"
  | "gateway_timeout";

export class CerebrasError extends Error {
  readonly code: CerebrasErrorCode;
  readonly status: number;

  constructor(message: string, code: CerebrasErrorCode, status: number) {
    super(message);
    this.name = "CerebrasError";
    this.code = code;
    this.status = status;
  }
}

export type CerebrasChatCompletionsOptions = Readonly<{
  apiKey?: string | null;
  fetcher?: CerebrasFetch;
  signal?: AbortSignal;
  beforeDispatch?: () => Promise<ApiKeyProviderDispatch | void>;
  onDispatch?: () => void;
  onHeaders?: () => void;
}>;

type NormalizationResult<T> =
  | Readonly<{ ok: true; value: T }>
  | Readonly<{ ok: false; message: string }>;

const nonEmptyString = (value: unknown): string | null => {
  const text = getString(value)?.trim();
  return text || null;
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
export const normalizeCerebrasProviderRequestId = (value: unknown): string | null => {
  const requestId = exactNonEmptyString(value);
  if (!requestId || requestId.length > MAX_CEREBRAS_PROVIDER_REQUEST_ID_LENGTH) return null;
  return /^[\x21-\x7e]+$/.test(requestId) ? requestId : null;
};

export const getCerebrasProviderRequestId = (response: Response): string | null =>
  normalizeCerebrasProviderRequestId(
    response.headers.get("X-Request-Id") ??
      response.headers.get("X-Api-Request-Id") ??
      response.headers.get("X-Cerebras-Request-Id"),
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

const timeoutError = (): CerebrasError =>
  new CerebrasError(
    "Upstream request exceeded the gateway deadline.",
    "gateway_timeout",
    504,
  );

const abortError = (signal: AbortSignal): Error =>
  signal.reason instanceof Error ? signal.reason : new DOMException("The request was aborted.", "AbortError");

const isTimeoutError = (error: unknown): boolean => error instanceof Error && error.name === "TimeoutError";

export const readCerebrasApiKey = (): string | null => nonEmptyString(getEnv(CEREBRAS_API_KEY_ENV));

export const setCerebrasFetchTimeoutMsForTest = (timeoutMs: number | null): void => {
  cerebrasFetchTimeoutMs = timeoutMs ?? BUFFERED_INFERENCE_DEADLINE_MS;
};

const requireCerebrasApiKey = (supplied: string | null | undefined): string => {
  const apiKey = supplied === undefined ? readCerebrasApiKey() : nonEmptyString(supplied);
  if (apiKey) return apiKey;
  throw new CerebrasError(
    "The requested model is not configured.",
    "cerebras_api_key_missing",
    503,
  );
};

/**
 * Cerebras accepts a documented subset of JSON Schema for strict native
 * tools. Keep the product's complete schema at the gateway boundary, then
 * project only the provider-bound copy: its server-side validation remains
 * the authoritative enforcement for omitted bounds.
 */
const projectCerebrasSchemaValue = (value: unknown): unknown => {
  if (Array.isArray(value)) return value.map(projectCerebrasSchemaValue);
  if (!isRecord(value) || Array.isArray(value)) return value;
  const projected: Record<string, unknown> = {};
  for (const [key, child] of Object.entries(value)) {
    if (CEREBRAS_UNSUPPORTED_SCHEMA_FIELDS.has(key)) continue;
    if (key === "const") {
      projected.enum = [projectCerebrasSchemaValue(child)];
      continue;
    }
    projected[key === "oneOf" ? "anyOf" : key] = projectCerebrasSchemaValue(child);
  }
  return projected;
};

const distinctSchemas = (values: readonly unknown[]): unknown[] => {
  const distinct = new Map<string, unknown>();
  for (const value of values) distinct.set(JSON.stringify(value), value);
  return [...distinct.values()];
};

/**
 * Cerebras requires every function parameter schema to have an object root.
 * Our read tool's exact server-side schema is a root union by operation ID;
 * present it to the model as one object with a constrained ID and nested
 * argument union, then retain exact validation after the response returns.
 */
const collapseCerebrasRootObjectUnion = (value: unknown): unknown => {
  if (!isRecord(value) || Array.isArray(value) || value.type === "object" || !Array.isArray(value.anyOf)) {
    return value;
  }
  const variants = value.anyOf;
  if (
    !variants.length ||
    variants.some((variant) =>
      !isRecord(variant) || Array.isArray(variant) || variant.type !== "object" ||
      !isRecord(variant.properties) || Array.isArray(variant.properties)
    )
  ) return value;

  const fields = new Map<string, unknown[]>();
  let requiredByEveryVariant: Set<string> | null = null;
  for (const variant of variants) {
    const properties = variant.properties as Record<string, unknown>;
    for (const [name, schema] of Object.entries(properties)) {
      const values = fields.get(name) ?? [];
      values.push(schema);
      fields.set(name, values);
    }
    const required = new Set<string>(
      Array.isArray(variant.required)
        ? variant.required.filter((name: unknown): name is string => typeof name === "string")
        : [],
    );
    requiredByEveryVariant = requiredByEveryVariant === null ? required : requiredByEveryVariant.intersection(required);
  }

  const properties: Record<string, unknown> = {};
  for (const [name, candidates] of fields) {
    const distinct = distinctSchemas(candidates);
    if (distinct.length === 1) {
      properties[name] = distinct[0];
      continue;
    }
    if (
      name === "operationId" &&
      distinct.every((candidate) => isRecord(candidate) && Array.isArray(candidate.enum))
    ) {
      properties[name] = {
        enum: distinctSchemas(
          distinct.flatMap((candidate) => (candidate as Record<string, unknown>).enum as unknown[]),
        ),
      };
      continue;
    }
    properties[name] = { anyOf: distinct };
  }
  return {
    type: "object",
    properties,
    required: [...(requiredByEveryVariant ?? [])].sort(),
    additionalProperties: false,
  };
};

export const projectCerebrasToolSchema = (value: unknown): unknown =>
  collapseCerebrasRootObjectUnion(projectCerebrasSchemaValue(value));

const projectCerebrasRequest = (body: Record<string, unknown>): Record<string, unknown> => {
  if (!Array.isArray(body.tools)) return body;
  return {
    ...body,
    tools: body.tools.map((tool) => {
      if (!isRecord(tool) || Array.isArray(tool) || !isRecord(tool.function) || Array.isArray(tool.function)) {
        return tool;
      }
      return {
        ...tool,
        function: {
          ...tool.function,
          ...(tool.function.parameters === undefined
            ? {}
            : { parameters: projectCerebrasToolSchema(tool.function.parameters) }),
        },
      };
    }),
  };
};

/**
 * Sends only canonical OpenAI Chat Completions JSON to Cerebras.  The caller
 * owns model selection; this transport never chooses or falls back to another
 * provider.
 */
export const fetchCerebrasChatCompletions = async (
  body: Record<string, unknown>,
  options: CerebrasChatCompletionsOptions = {},
): Promise<Response> => {
  let encodedBody: string;
  try {
    encodedBody = JSON.stringify(projectCerebrasRequest(body));
  } catch {
    throw new CerebrasError(
      "Chat Completions requests must use a JSON-serializable body.",
      "cerebras_request_invalid",
      400,
    );
  }
  if (typeof encodedBody !== "string") {
    throw new CerebrasError(
      "Chat Completions requests must use a JSON-serializable body.",
      "cerebras_request_invalid",
      400,
    );
  }

  const apiKey = requireCerebrasApiKey(options.apiKey);
  const headers = new Headers({
    Accept: "application/json",
    Authorization: `Bearer ${apiKey}`,
    "Content-Type": "application/json",
  });
  const deadline = new AbortController();
  const timer = setTimeout(
    () => deadline.abort(new DOMException("Cerebras response headers timed out.", "TimeoutError")),
    cerebrasFetchTimeoutMs,
  );
  const signal = options.signal ? AbortSignal.any([options.signal, deadline.signal]) : deadline.signal;

  try {
    // Keep this as the final awaited operation before provider transport so
    // API-key admission cannot be committed after a cancelled request.
    const dispatch = options.beforeDispatch ? await options.beforeDispatch() : undefined;
    if (signal.aborted) {
      await dispatch?.cancelBeforeTransport();
      if (options.signal?.aborted && isTimeoutError(abortError(options.signal))) throw timeoutError();
      if (deadline.signal.aborted) throw timeoutError();
      throw abortError(signal);
    }
    dispatch?.markTransportStarted();
    options.onDispatch?.();
    const response = await (options.fetcher ?? fetch)(CEREBRAS_CHAT_COMPLETIONS_URL, {
      method: "POST",
      headers,
      body: encodedBody,
      redirect: "manual",
      signal,
    });
    options.onHeaders?.();
    return response;
  } catch (error) {
    if (error instanceof ApiKeyQuotaDispatchError || error instanceof CerebrasError) throw error;
    if (options.signal?.aborted) {
      if (isTimeoutError(abortError(options.signal))) throw timeoutError();
      throw abortError(options.signal);
    }
    if (deadline.signal.aborted || isTimeoutError(error)) throw timeoutError();
    throw new CerebrasError(
      "Upstream request could not be completed.",
      "cerebras_upstream_unreachable",
      502,
    );
  } finally {
    clearTimeout(timer);
  }
};

const normalizeToolCall = (
  value: unknown,
  index: number,
): NormalizationResult<Record<string, unknown>> => {
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
  return {
    ok: true,
    value: {
      id,
      type: "function",
      function: { name, arguments: value.function.arguments },
    },
  };
};

const normalizeChoice = (
  value: unknown,
  index: number,
): NormalizationResult<Record<string, unknown>> => {
  if (!isRecord(value) || Array.isArray(value)) {
    return { ok: false, message: `Upstream choice ${index} is not an object.` };
  }
  const choiceIndex = nonNegativeInteger(value.index);
  if (choiceIndex === null) return { ok: false, message: `Upstream choice ${index} has an invalid index.` };
  if (!isRecord(value.message) || Array.isArray(value.message)) {
    return { ok: false, message: `Upstream choice ${index} is missing an assistant message.` };
  }
  const message = value.message;
  if (message.role !== "assistant") {
    return { ok: false, message: `Upstream choice ${index} does not contain an assistant message.` };
  }
  if (!(message.content === undefined || message.content === null || typeof message.content === "string")) {
    return { ok: false, message: `Upstream choice ${index} has unsupported message content.` };
  }

  let toolCalls: Record<string, unknown>[] | undefined;
  if (message.tool_calls !== undefined) {
    if (!Array.isArray(message.tool_calls)) {
      return { ok: false, message: `Upstream choice ${index} has invalid tool calls.` };
    }
    toolCalls = [];
    for (const [callIndex, call] of message.tool_calls.entries()) {
      const normalized = normalizeToolCall(call, callIndex);
      if (!normalized.ok) return normalized;
      toolCalls.push(normalized.value);
    }
  }
  if (message.content === undefined && !toolCalls?.length) {
    return { ok: false, message: `Upstream choice ${index} has neither content nor a tool call.` };
  }

  const finishReason = value.finish_reason;
  if (!(finishReason === undefined || finishReason === null || typeof finishReason === "string")) {
    return { ok: false, message: `Upstream choice ${index} has an invalid finish reason.` };
  }
  const normalizedMessage: Record<string, unknown> = {
    role: "assistant",
    content: message.content ?? (toolCalls?.length ? null : ""),
  };
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

const normalizeUsage = (value: unknown): NormalizationResult<Record<string, number> | null> => {
  if (value === undefined || value === null) return { ok: true, value: null };
  if (!isRecord(value) || Array.isArray(value)) return { ok: false, message: "Upstream usage is not an object." };
  const promptTokens = nonNegativeInteger(value.prompt_tokens);
  const completionTokens = nonNegativeInteger(value.completion_tokens);
  const totalTokens = nonNegativeInteger(value.total_tokens);
  if (promptTokens === null || completionTokens === null || totalTokens === null) {
    return { ok: false, message: "Upstream usage is incomplete." };
  }
  return {
    ok: true,
    value: {
      prompt_tokens: promptTokens,
      completion_tokens: completionTokens,
      total_tokens: totalTokens,
    },
  };
};

/**
 * Reduces a Cerebras success payload to the OpenAI Chat Completions shape the
 * Assistant consumes. Unknown provider fields, including diagnostics, are not
 * relayed or logged.
 */
export const normalizeCerebrasChatCompletion = (
  value: unknown,
  requestedModel: string,
): NormalizationResult<Record<string, unknown>> => {
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
  const upstreamModel = nonEmptyString(value.model);
  if (upstreamModel && upstreamModel !== requestedModel) {
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
      model: requestedModel,
      choices,
      ...(usage.value ? { usage: usage.value } : {}),
    },
  };
};
