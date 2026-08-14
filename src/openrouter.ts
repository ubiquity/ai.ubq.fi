import { type ApiKeyProviderDispatch, ApiKeyQuotaDispatchError } from "./api_key_policy.ts";
import {
  buildOpenRouterRequestProjection,
  OpenRouterProjectionError,
  type OpenRouterProjectionRegistry,
} from "./openrouter_projection.ts";
import { getString, isRecord, sha256Base64Url } from "./utils.ts";

export const OPENROUTER_RESPONSES_URL = "https://openrouter.ai/api/v1/responses";
export const OPENROUTER_AUTO_MODEL = "openrouter/auto";
export const OPENROUTER_API_KEY_ENV = "OPENROUTER_API_KEY";
export const OPENROUTER_EXCLUDED_MODELS = [
  "openai/*",
  "~openai/*",
  "anthropic/*",
  "~anthropic/*",
  "*/gpt-*",
  "*/claude-*",
] as const;

export type OpenRouterResponsesResult = Readonly<{
  response: Response;
  requestBody: Record<string, unknown>;
  projection: OpenRouterProjectionRegistry;
}>;

export type OpenRouterResponseTiming = Readonly<{
  onDispatch?: () => void;
  onHeaders?: () => void;
}>;

export class OpenRouterError extends Error {
  readonly code: string;
  readonly status: number;
  readonly param: string | null;

  constructor(message: string, code: string, status = 502, options?: ErrorOptions & { param?: string | null }) {
    super(message, options);
    this.name = "OpenRouterError";
    this.code = code;
    this.status = status;
    this.param = options?.param ?? null;
  }
}

const getEnv = (name: string): string | null => {
  try {
    return Deno.env.get(name)?.trim() || null;
  } catch {
    return null;
  }
};

let openRouterApiKeyForTest: string | null | undefined;

export const setOpenRouterApiKeyForTest = (value: string | null | undefined): void => {
  openRouterApiKeyForTest = value;
};

export const readOpenRouterApiKey = (): string | null =>
  openRouterApiKeyForTest === undefined ? getEnv(OPENROUTER_API_KEY_ENV) : openRouterApiKeyForTest;

const OPENROUTER_REQUEST_KEYS = new Set([
  "include",
  "input",
  "instructions",
  "max_tool_calls",
  "max_output_tokens",
  "metadata",
  "parallel_tool_calls",
  "provider",
  "reasoning",
  "safety_identifier",
  "service_tier",
  "stream",
  "temperature",
  "text",
  "tool_choice",
  "tools",
  "top_p",
  "truncation",
  "user",
]);

const validateRequestValue = (key: string, value: unknown): void => {
  if (value === undefined) return;
  if (key === "input") {
    if (!Array.isArray(value) && typeof value !== "string") {
      throw new OpenRouterError("OpenRouter input translation is invalid.", "openrouter_translation_invalid", 400);
    }
    return;
  }
  if (key === "instructions") {
    if (value !== null && typeof value !== "string") {
      throw new OpenRouterError(
        "OpenRouter instructions translation is invalid.",
        "openrouter_translation_invalid",
        400,
      );
    }
    return;
  }
  if (key === "stream") {
    if (value !== true) {
      throw new OpenRouterError("OpenRouter transport requires streaming.", "openrouter_translation_invalid", 400);
    }
    return;
  }
  if (key === "max_output_tokens") {
    if (value === null) return;
    if (typeof value !== "number" || !Number.isSafeInteger(value) || value <= 0) {
      throw new OpenRouterError("OpenRouter output-token limit is invalid.", "openrouter_translation_invalid", 400);
    }
    return;
  }
  if (key === "parallel_tool_calls" && typeof value !== "boolean") {
    throw new OpenRouterError("OpenRouter parallel-tool setting is invalid.", "openrouter_translation_invalid", 400);
  }
  if (key === "provider") {
    if (!isRecord(value) || Array.isArray(value)) {
      throw new OpenRouterError(
        "OpenRouter provider routing constraint is invalid.",
        "openrouter_translation_invalid",
        400,
      );
    }
    if (value.require_parameters !== undefined && typeof value.require_parameters !== "boolean") {
      throw new OpenRouterError(
        "OpenRouter provider.require_parameters must be a boolean.",
        "openrouter_translation_invalid",
        400,
      );
    }
    return;
  }
  if (key === "max_tool_calls") {
    if (value === null) return;
    if (typeof value !== "number" || !Number.isSafeInteger(value) || value <= 0) {
      throw new OpenRouterError("OpenRouter max-tool-call limit is invalid.", "openrouter_translation_invalid", 400);
    }
    return;
  }
  if (key === "metadata") {
    if (value === null) return;
    if (!isRecord(value) || Array.isArray(value)) {
      throw new OpenRouterError("OpenRouter metadata is invalid.", "openrouter_translation_invalid", 400);
    }
    return;
  }
  if (key === "user" || key === "safety_identifier") {
    if (typeof value !== "string") {
      throw new OpenRouterError(`OpenRouter ${key} is invalid.`, "openrouter_translation_invalid", 400);
    }
    return;
  }
  if (key === "temperature") {
    if (value === null) return;
    if (typeof value !== "number" || !Number.isFinite(value) || value < 0 || value > 2) {
      throw new OpenRouterError("OpenRouter temperature is invalid.", "openrouter_translation_invalid", 400);
    }
    return;
  }
  if (key === "top_p") {
    if (value === null) return;
    if (typeof value !== "number" || !Number.isFinite(value) || value < 0 || value > 1) {
      throw new OpenRouterError("OpenRouter top-p value is invalid.", "openrouter_translation_invalid", 400);
    }
    return;
  }
  if (key === "truncation" && value !== "auto" && value !== "disabled") {
    throw new OpenRouterError("OpenRouter truncation setting is invalid.", "openrouter_translation_invalid", 400);
  }
};

const normalizeOpenRouterReasoning = (value: unknown): unknown => {
  if (!isRecord(value) || Array.isArray(value)) return value;
  const effort = typeof value.effort === "string" ? value.effort : null;
  if (!effort) return undefined;
  return { effort: effort === "ultra" ? "max" : effort };
};

export const buildOpenRouterResponsesRequest = (
  canonical: Record<string, unknown>,
  sessionId?: string | null,
): Record<string, unknown> => {
  return buildOpenRouterResponsesRequestWithProjection(canonical, sessionId).requestBody;
};

export const buildOpenRouterResponsesRequestWithProjection = (
  canonical: Record<string, unknown>,
  sessionId?: string | null,
): Readonly<{ requestBody: Record<string, unknown>; projection: OpenRouterProjectionRegistry }> => {
  const translated: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(canonical)) {
    if (!OPENROUTER_REQUEST_KEYS.has(key)) continue;
    validateRequestValue(key, value);
    const normalized = key === "reasoning" ? normalizeOpenRouterReasoning(value) : value;
    if (normalized !== undefined) translated[key] = normalized;
  }
  if (!("input" in translated) || translated.stream !== true) {
    throw new OpenRouterError("OpenRouter request translation is incomplete.", "openrouter_translation_invalid", 400);
  }
  const projection = (() => {
    try {
      return buildOpenRouterRequestProjection(translated);
    } catch (error) {
      if (error instanceof OpenRouterProjectionError) {
        throw new OpenRouterError(error.message, error.code, error.status, { cause: error, param: error.param });
      }
      throw error;
    }
  })();
  translated.input = projection.input;
  if (projection.tools !== undefined) translated.tools = projection.tools;
  else delete translated.tools;
  if (projection.toolChoice !== undefined) translated.tool_choice = projection.toolChoice;
  else delete translated.tool_choice;
  // The current Responses endpoint catalog does not advertise these fields
  // for the eligible Auto Router endpoints. With require_parameters enabled,
  // forwarding them would make the entire route ineligible before dispatch.
  delete translated.include;
  delete translated.parallel_tool_calls;
  delete translated.text;
  translated.provider = { require_parameters: true };
  translated.model = OPENROUTER_AUTO_MODEL;
  translated.plugins = [{
    id: "auto-router",
    cost_tier: "max",
    excluded_models: [...OPENROUTER_EXCLUDED_MODELS],
  }];
  if (sessionId?.trim()) translated.session_id = sessionId.trim();
  return { requestBody: translated, projection: projection.registry };
};

export const deriveOpenRouterSessionId = async (
  idempotencyPrincipal: string | null | undefined,
  clientMetadata: unknown,
): Promise<string | null> => {
  const principal = idempotencyPrincipal?.trim();
  if (!principal || !isRecord(clientMetadata) || Array.isArray(clientMetadata)) return null;
  const rawSessionId = getString(clientMetadata.session_id)?.trim();
  if (!rawSessionId) return null;
  return `uos_${await sha256Base64Url(`openrouter-session-v1\u0000${principal}\u0000${rawSessionId}`)}`;
};

const tokenParts = (value: string): string[] => value.toLowerCase().split(/[^a-z0-9]+/).filter(Boolean);

export const isEligibleOpenRouterModel = (value: unknown): value is string => {
  if (typeof value !== "string") return false;
  const model = value.trim();
  if (!model || model.length > 256 || [...model].some((character) => character < " " || character === "\u007f")) {
    return false;
  }
  if (model.toLowerCase() === OPENROUTER_AUTO_MODEL) return false;
  const separator = model.indexOf("/");
  if (separator <= 0 || separator === model.length - 1) return false;
  const publisher = model.slice(0, separator).toLowerCase();
  if (publisher === "openai" || publisher === "~openai" || publisher === "anthropic" || publisher === "~anthropic") {
    return false;
  }
  const familyParts = tokenParts(model.slice(separator + 1));
  return !familyParts.includes("gpt") && !familyParts.includes("claude");
};

export const openRouterModelFromEvent = (value: Record<string, unknown>): string | null => {
  const direct = getString(value.model)?.trim();
  if (direct) return direct;
  if (!isRecord(value.response) || Array.isArray(value.response)) return null;
  return getString(value.response.model)?.trim() || null;
};

export const openRouterTaskTypeFromResponse = (value: unknown): string | null => {
  if (!isRecord(value) || Array.isArray(value) || !isRecord(value.openrouter_metadata)) return null;
  const pipeline = value.openrouter_metadata.pipeline;
  if (!Array.isArray(pipeline)) return null;
  for (const stage of pipeline) {
    if (!isRecord(stage) || !isRecord(stage.data)) continue;
    const taskType = getString(stage.data.task_type)?.trim();
    if (taskType) return taskType.slice(0, 128);
  }
  return null;
};

export const stripOpenRouterMetadata = (value: Record<string, unknown>): Record<string, unknown> => {
  if (!isRecord(value.response) || Array.isArray(value.response)) return value;
  if (!Object.prototype.hasOwnProperty.call(value.response, "openrouter_metadata")) return value;
  const response = { ...value.response };
  delete response.openrouter_metadata;
  return { ...value, response };
};

export const fetchOpenRouterResponses = async (
  canonical: Record<string, unknown>,
  options: Readonly<{
    apiKey?: string | null;
    sessionId?: string | null;
    signal?: AbortSignal;
    timing?: OpenRouterResponseTiming;
    beforeDispatch?: () => Promise<ApiKeyProviderDispatch | void>;
  }> = {},
): Promise<OpenRouterResponsesResult> => {
  const apiKey = options.apiKey?.trim() || readOpenRouterApiKey();
  if (!apiKey) {
    throw new OpenRouterError("OpenRouter is not configured.", "openrouter_not_configured", 503);
  }
  const { requestBody, projection } = buildOpenRouterResponsesRequestWithProjection(canonical, options.sessionId);
  const dispatch = options.beforeDispatch ? await options.beforeDispatch() : undefined;
  if (options.signal?.aborted) {
    await dispatch?.cancelBeforeTransport();
    throw options.signal.reason ?? new DOMException("The request was aborted.", "AbortError");
  }
  try {
    try {
      options.timing?.onDispatch?.();
    } catch {
      // Telemetry must not affect provider routing.
    }
    dispatch?.markTransportStarted();
    const response = await fetch(OPENROUTER_RESPONSES_URL, {
      method: "POST",
      headers: {
        Authorization: `Bearer ${apiKey}`,
        "Content-Type": "application/json",
        Accept: "text/event-stream",
        "X-OpenRouter-Metadata": "enabled",
      },
      body: JSON.stringify(requestBody),
      redirect: "manual",
      signal: options.signal,
    });
    try {
      options.timing?.onHeaders?.();
    } catch {
      // Telemetry must not affect provider routing.
    }
    return { response, requestBody, projection };
  } catch (cause) {
    if (cause instanceof ApiKeyQuotaDispatchError) throw cause;
    if (options.signal?.aborted) throw options.signal.reason ?? cause;
    throw new OpenRouterError(
      "OpenRouter request could not reach the upstream service.",
      "openrouter_upstream_unreachable",
      502,
      { cause },
    );
  }
};
