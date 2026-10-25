// First-class OpenRouter Decisions API route.
//
// Decision models answer typed questions about `state` instead of generating
// text. They use OpenRouter's alpha Decisions API, not Chat Completions or
// Responses, so this handler validates the documented envelope and dispatches
// through the shared OpenRouter credential, admission, quota, and telemetry
// treatment. `/v1/systemone` remains the Jev-only compatibility surface.

import { ApiKeyQuotaDispatchError } from "../api-key-policy.ts";
import { openaiError } from "../http.ts";
import {
  attachResponseTelemetry,
  createResponseTelemetryState,
  extractUsageTokens,
  recordAttemptedProvider,
  recordCompletionUsage,
  recordFirstProviderDispatch,
  recordFirstProviderHeaders,
  recordRequestUsage,
  type UsageContext,
} from "../openai-telemetry.ts";
import { fetchOpenRouterDecisions, OpenRouterError, resolveOpenRouterDecisionUpstreamModel, type OpenRouterFetch } from "../provider/openrouter.ts";
import { inferenceSignal } from "../openai.ts";
import { recordOpenRouterProviderHealth } from "../provider/health.ts";
import { isProviderEnabled, loadProviderSelectionCached } from "../provider/selection.ts";
import { MAX_ACCEPTED_JSON_BODY_BYTES, readJsonBodyWithLimit } from "../request.ts";
import { apiKeyQuotaDispatchErrorResponse } from "../upstream-wire.ts";
import { isRecord } from "../utils.ts";

const DECISIONS_MAX_QUESTIONS = 24;
const DECISIONS_MAX_MODEL_LENGTH = 200;
const DECISIONS_MAX_METADATA_LENGTH = 256;
const DECISIONS_MAX_QUESTION_NAME_LENGTH = 120;

const DECISIONS_REQUEST_KEYS = ["model", "state", "questions", "provider", "session_id", "trace", "user"] as const;
const DECISIONS_QUESTION_KEYS = new Set(["type", "instructions", "criteria"]);
const DECISIONS_QUESTION_TYPES = new Set(["noul", "choice", "score", "box"]);
const DECISIONS_MODEL_PATTERN = /^~?[^/\s]+\/[^/\s]+$/u;

export type DecisionsHandlerDeps = Readonly<{
  fetcher?: OpenRouterFetch;
  apiKey?: () => string | null;
}>;

type DecisionsRequest = Readonly<{
  model: string;
  state: string | Record<string, unknown> | readonly unknown[];
  questions: Record<string, unknown>;
  provider?: Record<string, unknown> | null;
  session_id?: string;
  trace?: Record<string, unknown>;
  user?: string;
}>;

const isPlainRecord = (value: unknown): value is Record<string, unknown> => isRecord(value) && !Array.isArray(value);

/** A string, object, or array is the documented structured-guidance value. */
const isGuidanceValue = (value: unknown): boolean => typeof value === "string" || isPlainRecord(value) || Array.isArray(value);

const invalid = (message: string, param?: string): Response =>
  openaiError(400, message, "decisions_request_invalid", param === undefined ? { type: "invalid_request_error" } : { type: "invalid_request_error", param });

const unsupportedKeyError = (raw: Record<string, unknown>): Response | null => {
  const unknown = Object.keys(raw).find((key) => !(DECISIONS_REQUEST_KEYS as readonly string[]).includes(key));
  return unknown === undefined ? null : invalid(`Unsupported key: ${unknown}`);
};

const modelError = (raw: Record<string, unknown>): Response | null => {
  if (typeof raw.model !== "string") return invalid("model must be a string", "model");
  const model = raw.model.trim();
  if (!model || model.length > DECISIONS_MAX_MODEL_LENGTH || !DECISIONS_MODEL_PATTERN.test(model)) {
    return invalid("model must be an OpenRouter decision model id", "model");
  }
  return null;
};

const stateError = (state: unknown): Response | null =>
  typeof state === "string" || isRecord(state) || Array.isArray(state) ? null : invalid("state must be a string, object, or array", "state");

const instructionsError = (question: Record<string, unknown>, name: string): Response | null =>
  isGuidanceValue(question.instructions) ? null : invalid(`questions.${name}.instructions must be a string, object, or array`, "questions");

const noulCriteriaError = (criteria: unknown, name: string): Response | null => {
  if (criteria === undefined) return null;
  if (!isPlainRecord(criteria)) return invalid(`questions.${name}.criteria must be an object`, "questions");
  const keys = Object.keys(criteria);
  if (keys.some((key) => key !== "true" && key !== "false") || !keys.includes("true") || !keys.includes("false")) {
    return invalid(`questions.${name}.criteria must contain exactly true and false`, "questions");
  }
  if (!isGuidanceValue(criteria.true) || !isGuidanceValue(criteria.false)) {
    return invalid(`questions.${name}.criteria values must be strings, objects, or arrays`, "questions");
  }
  return null;
};

const choiceCriteriaError = (criteria: unknown, name: string): Response | null => {
  if (!isPlainRecord(criteria) || Object.keys(criteria).length === 0) {
    return invalid(`questions.${name}.criteria must be a non-empty object`, "questions");
  }
  for (const [choice, guidance] of Object.entries(criteria)) {
    if (!choice.trim() || (guidance !== null && !isGuidanceValue(guidance))) {
      return invalid(`questions.${name}.criteria entries must have names and string, object, array, or null guidance`, "questions");
    }
  }
  return null;
};

const scoreCriteriaError = (criteria: unknown, name: string): Response | null => {
  if (!Array.isArray(criteria) || criteria.length === 0) {
    return invalid(`questions.${name}.criteria must be a non-empty array`, "questions");
  }
  if (criteria.some((entry) => !isGuidanceValue(entry))) {
    return invalid(`questions.${name}.criteria entries must be strings, objects, or arrays`, "questions");
  }
  return null;
};

const questionError = (name: string, value: unknown): Response | null => {
  if (!isPlainRecord(value)) return invalid(`questions.${name} must be an object`, "questions");
  if (Object.keys(value).some((key) => !DECISIONS_QUESTION_KEYS.has(key))) {
    return invalid(`questions.${name} contains an unsupported field`, "questions");
  }
  if (typeof value.type !== "string" || !DECISIONS_QUESTION_TYPES.has(value.type)) {
    return invalid(`questions.${name}.type must be noul, choice, score, or box`, "questions");
  }
  const instructionFailure = instructionsError(value, name);
  if (instructionFailure) return instructionFailure;
  if (value.type === "noul") return noulCriteriaError(value.criteria, name);
  if (value.type === "choice") return choiceCriteriaError(value.criteria, name);
  if (value.type === "score") return scoreCriteriaError(value.criteria, name);
  return value.criteria === undefined ? null : invalid(`questions.${name}.criteria is not valid for box questions`, "questions");
};

const questionsError = (questions: unknown): Response | null => {
  if (!isPlainRecord(questions)) return invalid("questions must be an object", "questions");
  const entries = Object.entries(questions);
  if (entries.length === 0 || entries.length > DECISIONS_MAX_QUESTIONS) {
    return invalid(`questions must hold between 1 and ${DECISIONS_MAX_QUESTIONS} questions`, "questions");
  }
  for (const [name, question] of entries) {
    if (!name.trim() || name.length > DECISIONS_MAX_QUESTION_NAME_LENGTH) {
      return invalid("question names must be non-empty and at most 120 characters", "questions");
    }
    const failure = questionError(name, question);
    if (failure) return failure;
  }
  return null;
};

const metadataError = (raw: Record<string, unknown>): Response | null => {
  if (raw.provider !== undefined && raw.provider !== null && !isPlainRecord(raw.provider)) {
    return invalid("provider must be an object or null", "provider");
  }
  if (raw.trace !== undefined && !isPlainRecord(raw.trace)) return invalid("trace must be an object", "trace");
  for (const key of ["session_id", "user"] as const) {
    const value = raw[key];
    if (value === undefined) continue;
    if (typeof value !== "string" || !value || value.length > DECISIONS_MAX_METADATA_LENGTH) {
      return invalid(`${key} must be a non-empty string of at most ${DECISIONS_MAX_METADATA_LENGTH} characters`, key);
    }
  }
  return null;
};

/** Validates the documented Decisions envelope; a Response is the client-facing refusal. */
const parseDecisionsRequest = (raw: Record<string, unknown>): DecisionsRequest | Response => {
  const unsupported = unsupportedKeyError(raw);
  if (unsupported) return unsupported;
  const modelFailure = modelError(raw);
  if (modelFailure) return modelFailure;
  const stateFailure = stateError(raw.state);
  if (stateFailure) return stateFailure;
  const questionsFailure = questionsError(raw.questions);
  if (questionsFailure) return questionsFailure;
  const metadataFailure = metadataError(raw);
  if (metadataFailure) return metadataFailure;
  return {
    model: (raw.model as string).trim(),
    state: raw.state as DecisionsRequest["state"],
    questions: raw.questions as Record<string, unknown>,
    ...(raw.provider === undefined ? {} : { provider: raw.provider as Record<string, unknown> | null }),
    ...(raw.session_id === undefined ? {} : { session_id: raw.session_id as string }),
    ...(raw.trace === undefined ? {} : { trace: raw.trace as Record<string, unknown> }),
    ...(raw.user === undefined ? {} : { user: raw.user as string }),
  };
};

/** Normalize Decisions token usage onto the gateway's canonical shape. */
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

const forwardBody = (request: DecisionsRequest, model: string): Record<string, unknown> => ({
  model,
  state: request.state,
  questions: request.questions,
  ...(request.provider === undefined ? {} : { provider: request.provider }),
  ...(request.session_id === undefined ? {} : { session_id: request.session_id }),
  ...(request.trace === undefined ? {} : { trace: request.trace }),
  ...(request.user === undefined ? {} : { user: request.user }),
});

const recordDecisionsResponseHealth = (status: number): void => {
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

const DECISIONS_ERROR_MESSAGES: Readonly<Record<OpenRouterError["code"], string>> = Object.freeze({
  openrouter_api_key_missing: "Decisions upstream is not configured",
  openrouter_upstream_unreachable: "Decisions upstream unreachable",
  openrouter_upstream_error: "Decisions upstream error",
  openrouter_upstream_invalid_response: "Decisions upstream invalid response",
});

const decisionsDispatchFailure = (error: unknown): Response => {
  if (error instanceof ApiKeyQuotaDispatchError) return apiKeyQuotaDispatchErrorResponse(error);
  if (error instanceof Error && (error.name === "AbortError" || error.name === "TimeoutError")) {
    return openaiError(499, "Request was cancelled.", "request_cancelled", { type: "server_error", param: null });
  }
  if (error instanceof OpenRouterError) {
    if (error.code === "openrouter_upstream_error" && error.upstreamStatus !== null) {
      recordDecisionsResponseHealth(error.upstreamStatus);
    } else if (!error.beforeTransport && error.code !== "openrouter_api_key_missing") {
      void recordOpenRouterProviderHealth("upstream_error", null, Date.now);
    }
    return openaiError(error.status, DECISIONS_ERROR_MESSAGES[error.code], error.code);
  }
  throw error;
};

export const handleDecisions = async (req: Request, usageContext?: UsageContext, deps: DecisionsHandlerDeps = {}): Promise<Response> => {
  const read = await readJsonBodyWithLimit(req, MAX_ACCEPTED_JSON_BODY_BYTES);
  if (!read.ok) {
    return read.kind === "too_large"
      ? openaiError(413, "The Decisions request body is too large", "decisions_request_too_large")
      : openaiError(400, "Invalid JSON body", "invalid_request_error");
  }
  if (!isRecord(read.value)) return openaiError(400, "Invalid JSON body", "invalid_request_error");
  const request = parseDecisionsRequest(read.value);
  if (request instanceof Response) return request;

  if (!isProviderEnabled("openrouter", await loadProviderSelectionCached())) {
    return openaiError(503, "Decisions provider is disabled by the provider selection", "decisions_provider_disabled");
  }

  const upstreamModel = await resolveOpenRouterDecisionUpstreamModel(request.model);
  if (!upstreamModel) {
    return invalid("The requested model is not served by the OpenRouter Decisions API", "model");
  }

  const telemetry = usageContext?.responseTelemetry ?? createResponseTelemetryState();
  telemetry.provider = "openrouter";
  const context: UsageContext = usageContext
    ? { ...usageContext, responseTelemetry: telemetry }
    : { keyId: null, kernelRepo: null, kernelOrg: null, responseTelemetry: telemetry };

  await recordRequestUsage(context, { model: request.model, route: "decisions", stream: false, reasoning: null });

  let payload: Record<string, unknown>;
  try {
    payload = await fetchOpenRouterDecisions({
      body: forwardBody(request, upstreamModel),
      ...(deps.apiKey ? { apiKey: deps.apiKey() } : {}),
      ...(deps.fetcher ? { fetcher: deps.fetcher } : {}),
      signal: inferenceSignal(req, usageContext),
      hooks: {
        beforeDispatch: () => context.beforeProviderDispatch?.("openrouter") ?? Promise.resolve(undefined),
        onDispatch: () => {
          recordAttemptedProvider(context, "openrouter");
          recordFirstProviderDispatch(context);
        },
        onHeaders: () => {
          recordFirstProviderHeaders(context);
        },
      },
    });
  } catch (error) {
    return decisionsDispatchFailure(error);
  }

  if (!isPlainRecord(payload.answers)) {
    void recordOpenRouterProviderHealth("upstream_error", null, Date.now);
    return openaiError(502, "Decisions upstream invalid response", "openrouter_upstream_invalid_response");
  }
  recordDecisionsResponseHealth(200);
  const usage = extractUsageTokens(normalizedUsage(payload.usage));
  await recordCompletionUsage(context, usage);
  return attachResponseTelemetry(Response.json(payload, { headers: { "x-uos-upstream": "openrouter" } }), telemetry);
};
