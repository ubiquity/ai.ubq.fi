// OpenRouter upstream provider.
//
// OpenRouter serves text models on both OpenAI-compatible wires and typed
// decision models on its alpha Decisions API. `/v1/decisions` is the
// first-class surface for every decision model; `/v1/systemone` remains the
// Jev-only compatibility surface. Neither decision wire is OpenAI-shaped chat,
// so they share a bounded JSON transport while keeping the gateway's normal
// admission, quota, provider health, and telemetry treatment. Model ids are
// resolved from the same cached catalogue that drives the listing routes, so a
// text model cannot be sent to the decision wire or the reverse.

import { fetchOpenRouterModels, openRouterModelSupportsDecisions, openRouterModelSupportsText, openRouterModelsSnapshot } from "../models/openrouter-models.ts";
import type { ApiKeyProviderDispatch } from "../api-key-policy.ts";
import type { SentinelUpstreamRecorder } from "../sentinel/upstream-capture.ts";
import { isRecord } from "../utils.ts";

export const OPENROUTER_BASE_URL = "https://openrouter.ai/api/v1";
export const OPENROUTER_API_KEY_ENV = "OPENROUTER_API_KEY";
export const OPENROUTER_SYSTEMONE_URL = `${OPENROUTER_BASE_URL}/systemone`;
export const OPENROUTER_DECISIONS_URL = "https://openrouter.ai/api/alpha/decisions";
export const OPENROUTER_CHAT_COMPLETIONS_URL = `${OPENROUTER_BASE_URL}/chat/completions`;
export const OPENROUTER_RESPONSES_URL = `${OPENROUTER_BASE_URL}/responses`;
export const OPENROUTER_FETCH_TIMEOUT_MS = 20_000;
export const OPENROUTER_DECISIONS_TIMEOUT_MS = 60_000;

export type OpenRouterErrorCode =
  "openrouter_api_key_missing" | "openrouter_upstream_unreachable" | "openrouter_upstream_error" | "openrouter_upstream_invalid_response";

/** A bounded upstream failure; `status` is the client-facing HTTP status. */
export class OpenRouterError extends Error {
  constructor(
    readonly code: OpenRouterErrorCode,
    /** Client-facing status, normalized (e.g. an upstream 401 answers 502). */
    readonly status: number,
    /** The raw upstream status when a response arrived; null otherwise. */
    readonly upstreamStatus: number | null = null,
    /** True when a local dispatch hook prevented transport from starting. */
    readonly beforeTransport = false
  ) {
    super(code);
    this.name = "OpenRouterError";
  }
}

export type OpenRouterFetch = (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>;

/**
 * The text-output OpenRouter model ids this gateway serves on Chat Completions
 * and Responses, read from the cached public catalogue. Decision models use
 * their own resolver and `/v1/decisions`; they must never be accepted by an
 * OpenAI wire just because both sets share one discovery snapshot.
 */
export const openRouterTextModelIds = (): readonly string[] =>
  (openRouterModelsSnapshot()?.models ?? []).filter(openRouterModelSupportsText).map((model) => model.id);

/** The decision-output OpenRouter model ids served by `/v1/decisions`. */
export const openRouterDecisionModelIds = (): readonly string[] =>
  (openRouterModelsSnapshot()?.models ?? []).filter(openRouterModelSupportsDecisions).map((model) => model.id);

/** The upstream text id for a requested model, or null when OpenRouter does not serve it there. */
export const openRouterUpstreamModelFor = (model: string): string | null => {
  const trimmed = model.trim();
  if (!trimmed) return null;
  return openRouterTextModelIds().includes(trimmed) ? trimmed : null;
};

/**
 * The decision id for a request. `typesafe/jev-latest` remains the gateway's
 * stable Jev alias even though OpenRouter publishes the rolling row with a
 * leading tilde.
 */
export const openRouterDecisionUpstreamModelFor = (model: string): string | null => {
  const trimmed = model.trim();
  if (!trimmed) return null;
  if (openRouterDecisionModelIds().includes(trimmed)) return trimmed;
  if (trimmed === "typesafe/jev-latest" && openRouterDecisionModelIds().includes("~typesafe/jev-latest")) return "~typesafe/jev-latest";
  return null;
};

const OPENROUTER_MODEL_ID_PATTERN = /^[^/\s]+\/[^/\s]+$/;

/**
 * Resolve an id before the first catalogue refresh after a restart. A warm
 * snapshot is authoritative; a cold one refreshes once and is re-read. The
 * refresh may also be coalesced with a background warm request.
 */
const resolveOpenRouterModel = async (model: string, resolveWarm: (model: string) => string | null): Promise<string | null> => {
  const resolved = resolveWarm(model);
  const trimmed = model.trim();
  if (resolved !== null || openRouterModelsSnapshot() !== null || !OPENROUTER_MODEL_ID_PATTERN.test(trimmed)) return resolved;
  if (readOpenRouterApiKey() === null) return null;
  await fetchOpenRouterModels().catch(() => null);
  return resolveWarm(model);
};

export const resolveOpenRouterUpstreamModel = async (model: string): Promise<string | null> => await resolveOpenRouterModel(model, openRouterUpstreamModelFor);

export const resolveOpenRouterDecisionUpstreamModel = async (model: string): Promise<string | null> =>
  await resolveOpenRouterModel(model, openRouterDecisionUpstreamModelFor);

export type OpenRouterDispatchHooks = Readonly<{
  beforeDispatch?: () => Promise<ApiKeyProviderDispatch | undefined>;
  onDispatch?: () => void;
  onHeaders?: () => void;
  /** The request-owned Sentinel recorder, exactly as the other providers take it. */
  sentinelUpstreamRecorder?: SentinelUpstreamRecorder;
}>;

const dispatchOpenRouter = async (
  url: string,
  body: Readonly<Record<string, unknown>>,
  init: Readonly<{ signal: AbortSignal; hooks?: OpenRouterDispatchHooks }>
): Promise<Response> => {
  const apiKey = readOpenRouterApiKey();
  if (!apiKey) throw new OpenRouterError("openrouter_api_key_missing", 503);
  const dispatch = init.hooks?.beforeDispatch ? await init.hooks.beforeDispatch() : undefined;
  if (init.signal.aborted) {
    await dispatch?.cancelBeforeTransport();
    throw new DOMException("Aborted", "AbortError");
  }
  dispatch?.markTransportStarted();
  init.hooks?.onDispatch?.();
  const upstreamAttempt = init.hooks?.sentinelUpstreamRecorder?.startAttempt("openrouter") ?? null;
  let response: Response;
  try {
    response = await fetch(url, {
      method: "POST",
      headers: {
        accept: "application/json",
        authorization: `Bearer ${apiKey}`,
        "content-type": "application/json",
      },
      body: JSON.stringify(body),
      signal: init.signal,
    });
  } catch (error) {
    upstreamAttempt?.recordFetchError();
    if (error instanceof DOMException && error.name === "AbortError") throw error;
    throw new OpenRouterError("openrouter_upstream_unreachable", 502);
  }
  init.hooks?.onHeaders?.();
  return upstreamAttempt ? upstreamAttempt.wrap(response) : response;
};

export const fetchOpenRouterChatCompletions = async (
  body: Readonly<Record<string, unknown>>,
  init: Readonly<{ signal: AbortSignal; hooks?: OpenRouterDispatchHooks }>
): Promise<Response> => await dispatchOpenRouter(OPENROUTER_CHAT_COMPLETIONS_URL, body, init);

export const fetchOpenRouterResponses = async (
  body: Readonly<Record<string, unknown>>,
  init: Readonly<{ signal: AbortSignal; hooks?: OpenRouterDispatchHooks }>
): Promise<Response> => await dispatchOpenRouter(OPENROUTER_RESPONSES_URL, body, init);

export const readOpenRouterApiKey = (): string | null => {
  try {
    const value = process.env[OPENROUTER_API_KEY_ENV]?.trim();
    if (!value) return null;
    return value;
  } catch {
    return null;
  }
};

/**
 * One System One decision call. The body is forwarded verbatim (`model`,
 * `state`, `questions`); the answer shape belongs to the caller, which also
 * owns bounds on the questions it sends.
 *
 * Dispatch hooks follow the shared OpenRouter dispatcher: the API-key
 * reservation commits before transport and the hook call sits outside the
 * transport catch, so a quota refusal stays itself instead of being reported
 * as an unreachable upstream.
 */
type OpenRouterDecisionInput = Readonly<{
  body: Readonly<Record<string, unknown>>;
  apiKey?: string | null;
  fetcher?: OpenRouterFetch;
  timeoutMs?: number;
  hooks?: OpenRouterDispatchHooks;
  /** Caller cancellation, combined with the transport deadline. */
  signal?: AbortSignal;
}>;

const fetchOpenRouterDecisionPayload = async (url: string, input: OpenRouterDecisionInput): Promise<Record<string, unknown>> => {
  const apiKey = input.apiKey === undefined ? readOpenRouterApiKey() : input.apiKey;
  if (!apiKey) throw new OpenRouterError("openrouter_api_key_missing", 503);
  const deadline = AbortSignal.timeout(input.timeoutMs ?? OPENROUTER_FETCH_TIMEOUT_MS);
  const signal = input.signal ? AbortSignal.any([input.signal, deadline]) : deadline;
  const dispatch = input.hooks?.beforeDispatch ? await input.hooks.beforeDispatch() : undefined;
  if (signal.aborted) {
    await dispatch?.cancelBeforeTransport();
    throw new OpenRouterError("openrouter_upstream_unreachable", 502, null, true);
  }
  dispatch?.markTransportStarted();
  input.hooks?.onDispatch?.();
  const fetcher = input.fetcher ?? fetch;
  let response: Response;
  try {
    response = await fetcher(url, {
      method: "POST",
      headers: {
        accept: "application/json",
        authorization: `Bearer ${apiKey}`,
        "content-type": "application/json",
      },
      body: JSON.stringify(input.body),
      signal,
    });
  } catch (error) {
    if (input.signal?.aborted) throw error;
    throw new OpenRouterError("openrouter_upstream_unreachable", 502);
  }
  input.hooks?.onHeaders?.();
  if (!response.ok) {
    await response.body?.cancel().catch(() => {});
    const status = response.status === 429 || response.status === 400 || response.status === 413 ? response.status : 502;
    throw new OpenRouterError("openrouter_upstream_error", status, response.status);
  }
  const payload = await response.json().catch(() => null);
  if (!isRecord(payload) || Array.isArray(payload)) {
    void response.body?.cancel().catch(() => {});
    throw new OpenRouterError("openrouter_upstream_invalid_response", 502);
  }
  return payload;
};

/** One System One (Jev compatibility) decision call. */
export const fetchOpenRouterSystemOne = async (input: OpenRouterDecisionInput): Promise<Record<string, unknown>> =>
  await fetchOpenRouterDecisionPayload(OPENROUTER_SYSTEMONE_URL, input);

/** One Decisions API call, including the model families beyond Jev. */
export const fetchOpenRouterDecisions = async (input: OpenRouterDecisionInput): Promise<Record<string, unknown>> =>
  await fetchOpenRouterDecisionPayload(OPENROUTER_DECISIONS_URL, {
    ...input,
    timeoutMs: input.timeoutMs ?? OPENROUTER_DECISIONS_TIMEOUT_MS,
  });
