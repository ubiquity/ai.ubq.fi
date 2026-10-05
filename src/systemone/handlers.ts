// System One (Typesafe Jev) decisions route.
//
// Served as a terminal inference route (`POST /v1/systemone`) so it carries the
// same API-key authentication, admission, kernel quota route, telemetry, and
// usage accounting as every other provider call. The upstream is the OpenRouter
// provider (`src/provider/openrouter.ts`), which holds the credential.
//
// Bounds: the request body is capped, only `state`/`questions`/`model` are
// accepted, every question must carry a known primitive type, and `model` is
// constrained to the typesafe namespace so a shared credential cannot be used
// to reach unrelated OpenRouter models.

import { ApiKeyQuotaDispatchError } from "../api-key-policy.ts";
import { openaiError } from "../http.ts";
import {
  attachResponseTelemetry,
  createResponseTelemetryState,
  extractUsageTokens,
  recordCompletionUsage,
  recordFirstProviderDispatch,
  recordFirstProviderHeaders,
  recordRequestUsage,
  type UsageContext,
} from "../openai-telemetry.ts";
import { fetchOpenRouterSystemOne, OpenRouterError, type OpenRouterFetch } from "../provider/openrouter.ts";
import { recordOpenRouterProviderHealth } from "../provider/health.ts";
import { isProviderEnabled, loadProviderSelectionCached } from "../provider/selection.ts";
import { readJsonBody } from "../request.ts";
import { apiKeyQuotaDispatchErrorResponse } from "../upstream-wire.ts";
import { getString, isRecord } from "../utils.ts";

export const SYSTEMONE_DEFAULT_MODEL = "~typesafe/jev-latest";
export const SYSTEMONE_MAX_BODY_BYTES = 262_144;
export const SYSTEMONE_MAX_QUESTIONS = 24;
export const SYSTEMONE_MAX_MODEL_LENGTH = 80;

const SYSTEMONE_REQUEST_KEYS = ["state", "questions", "model"] as const;
const SYSTEMONE_QUESTION_TYPES = new Set(["noul", "choice", "score"]);
const SYSTEMONE_MODEL_PATTERN = /^~?typesafe\/[a-z0-9][a-z0-9._-]{0,63}$/u;

export type SystemOneHandlerDeps = Readonly<{
  fetcher?: OpenRouterFetch;
  apiKey?: () => string | null;
}>;

const validQuestion = (value: unknown): boolean => isRecord(value) && typeof value.type === "string" && SYSTEMONE_QUESTION_TYPES.has(value.type);

const unsupportedKeyError = (raw: Record<string, unknown>): Response | null => {
  const unknown = Object.keys(raw).find((key) => !(SYSTEMONE_REQUEST_KEYS as readonly string[]).includes(key));
  return unknown === undefined ? null : openaiError(400, `Unsupported key: ${unknown}`, "invalid_request_error");
};

const questionsError = (questions: unknown): Response | null => {
  if (!isRecord(questions)) {
    return openaiError(400, "questions must be an object", "invalid_request_error");
  }
  const entries = Object.entries(questions);
  if (entries.length === 0 || entries.length > SYSTEMONE_MAX_QUESTIONS) {
    return openaiError(400, "questions must hold between 1 and 24 questions", "invalid_request_error");
  }
  for (const [name, question] of entries) {
    if (!name.trim() || name.length > 120 || !validQuestion(question)) {
      return openaiError(400, "questions must be typed noul, choice, or score objects", "invalid_request_error");
    }
  }
  return null;
};

const modelError = (raw: Record<string, unknown>): Response | null => {
  if (raw.model === undefined) return null;
  const requested = getString(raw.model);
  if (requested === null) {
    return openaiError(400, "model must be a string", "invalid_request_error");
  }
  if (requested.length > SYSTEMONE_MAX_MODEL_LENGTH || !SYSTEMONE_MODEL_PATTERN.test(requested)) {
    return openaiError(400, "model must be a typesafe model id", "invalid_request_error");
  }
  return null;
};

const modelOf = (raw: Record<string, unknown>): string => getString(raw.model) ?? SYSTEMONE_DEFAULT_MODEL;

/** OpenRouter requires the tilde-prefixed alias; the catalog advertises the same id without it. */
const upstreamModelFor = (model: string): string => (model.startsWith("~") ? model : `~${model}`);

/**
 * Normalizes OpenRouter's SystemOne usage onto the gateway's canonical token
 * shape so the shared telemetry and metering read it. System One reports no
 * prompt caching, so the cache-read counter is an explicit zero rather than a
 * missing field that would downgrade otherwise complete telemetry.
 */
const normalizedUsage = (value: unknown): Record<string, unknown> | null => {
  if (!isRecord(value)) return null;
  const input = value.input_tokens;
  const output = value.output_tokens;
  if (typeof input !== "number" || typeof output !== "number") return null;
  const usage: Record<string, unknown> = {
    input_tokens: input,
    output_tokens: output,
    total_tokens: input + output,
    input_tokens_details: { cached_tokens: 0 },
  };
  if (typeof value.cost === "number") usage.cost = value.cost;
  return usage;
};

type SystemOneRequest = Readonly<{
  model: string;
  state: Record<string, unknown>;
  questions: Record<string, unknown>;
}>;

/** Mirrors the other providers' classification so the admin badge reflects reality. */
export const recordSystemOneResponseHealth = (status: number): void => {
  if (status === 401 || status === 403) {
    void recordOpenRouterProviderHealth("auth_invalid", status, Date.now);
    return;
  }
  if (status === 402 || status === 429) {
    void recordOpenRouterProviderHealth("quota_exhausted", status, Date.now);
    return;
  }
  if (status >= 500) {
    void recordOpenRouterProviderHealth("upstream_error", status, Date.now);
    return;
  }
  if (status >= 400) {
    void recordOpenRouterProviderHealth("reachable", status, Date.now);
    return;
  }
  void recordOpenRouterProviderHealth("success", status, Date.now);
};

const SYSTEMONE_ERROR_MESSAGES: Readonly<Record<OpenRouterError["code"], string>> = Object.freeze({
  openrouter_api_key_missing: "System One upstream is not configured",
  openrouter_upstream_unreachable: "System One upstream unreachable",
  openrouter_upstream_error: "System One upstream error",
  openrouter_upstream_invalid_response: "System One upstream invalid response",
});

/**
 * Maps one System One dispatch failure. The API-key reservation refusal is a
 * quota answer with its own status and quota headers and must not fall through
 * to the generic upstream mapping; an upstream OpenRouter failure keeps its
 * existing health classification. Anything else is rethrown unchanged.
 */
const systemOneDispatchFailure = (error: unknown): Response => {
  if (error instanceof ApiKeyQuotaDispatchError) return apiKeyQuotaDispatchErrorResponse(error);
  if (error instanceof OpenRouterError) {
    if (error.code === "openrouter_upstream_error" && error.upstreamStatus !== null) {
      recordSystemOneResponseHealth(error.upstreamStatus);
    } else if (!error.beforeTransport && error.code !== "openrouter_api_key_missing") {
      // Unreachable transport or an unusable body: not healthy, and there is
      // no upstream status to classify.
      void recordOpenRouterProviderHealth("upstream_error", null, Date.now);
    }
    return openaiError(error.status, SYSTEMONE_ERROR_MESSAGES[error.code], error.code);
  }
  throw error;
};

/** Validates the bounded request envelope; a `Response` is the client-facing refusal. */
const parseSystemOneRequest = (raw: Record<string, unknown>): SystemOneRequest | Response => {
  const unsupported = unsupportedKeyError(raw);
  if (unsupported) return unsupported;
  if (!isRecord(raw.state)) return openaiError(400, "state must be an object", "invalid_request_error");
  const questionFailure = questionsError(raw.questions);
  if (questionFailure) return questionFailure;
  const invalidModel = modelError(raw);
  if (invalidModel) return invalidModel;
  return {
    model: modelOf(raw),
    state: raw.state,
    questions: raw.questions as Record<string, unknown>,
  };
};

export const handleSystemOne = async (req: Request, usageContext?: UsageContext, deps: SystemOneHandlerDeps = {}): Promise<Response> => {
  const raw = await readJsonBody(req, SYSTEMONE_MAX_BODY_BYTES);
  if (!isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");
  const request = parseSystemOneRequest(raw);
  if (request instanceof Response) return request;
  const { model } = request;

  // The terminal wrapper reads its log and quota decoration off the response,
  // so the handler attaches the telemetry state it records into. The provider
  // label is authoritative here: the upstream is OpenRouter, whatever header
  // the transport happens to carry.
  // The operator's provider selection is authoritative: an unchecked
  // OpenRouter is refused here the same way a deselected provider leaves the
  // chat waterfall, instead of silently serving decisions.
  if (!isProviderEnabled("openrouter", await loadProviderSelectionCached())) {
    return openaiError(503, "System One provider is disabled by the provider selection", "systemone_provider_disabled");
  }

  const telemetry = usageContext?.responseTelemetry ?? createResponseTelemetryState();
  telemetry.provider = "openrouter";
  const context: UsageContext = usageContext
    ? { ...usageContext, responseTelemetry: telemetry }
    : { keyId: null, kernelRepo: null, kernelOrg: null, responseTelemetry: telemetry };

  await recordRequestUsage(context, { model, route: "systemone", stream: false, reasoning: null });

  let payload: Record<string, unknown>;
  try {
    payload = await fetchOpenRouterSystemOne({
      body: { model: upstreamModelFor(model), state: request.state, questions: request.questions },
      ...(deps.apiKey ? { apiKey: deps.apiKey() } : {}),
      ...(deps.fetcher ? { fetcher: deps.fetcher } : {}),
      hooks: {
        beforeDispatch: () => context.beforeProviderDispatch?.("openrouter") ?? Promise.resolve(undefined),
        onDispatch: () => {
          recordFirstProviderDispatch(context);
        },
        onHeaders: () => {
          recordFirstProviderHeaders(context);
        },
      },
    });
  } catch (error) {
    return systemOneDispatchFailure(error);
  }

  if (!isRecord(payload.answers)) {
    void recordOpenRouterProviderHealth("upstream_error", null, Date.now);
    return openaiError(502, "System One upstream invalid response", "openrouter_upstream_invalid_response");
  }
  recordSystemOneResponseHealth(200);

  const usage = extractUsageTokens(normalizedUsage(payload.usage));
  await recordCompletionUsage(context, usage);
  return attachResponseTelemetry(Response.json(payload), telemetry);
};
