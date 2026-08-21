import { STREAM_FIRST_EVENT_DEADLINE_MS } from "./inference_deadline.ts";
import { type ApiKeyProviderDispatch, ApiKeyQuotaDispatchError } from "./api_key_policy.ts";

export const SURPLUS_BASE_URL = "https://api.surplusintelligence.ai";
export const SURPLUS_API_KEY_ENV = "SURPLUS_API_KEY";
const SURPLUS_MODELS_URL = `${SURPLUS_BASE_URL}/v1/models`;
const SURPLUS_RESPONSES_URL = `${SURPLUS_BASE_URL}/v1/responses`;
export const SURPLUS_FETCH_TIMEOUT_MS = 10_000;
export const SURPLUS_MODELS_CACHE_TTL_MS = 5 * 60_000;
const SURPLUS_MODELS_FAILURE_BACKOFF_MS = 30_000;

export type SurplusModel = Readonly<{
  id: string;
  object: "model";
  created: number;
  owned_by: string;
  supported_endpoint_types: readonly string[];
  description?: string;
  input_price_per_token?: number;
  output_price_per_token?: number;
  cache_read_price_per_token?: number;
  cache_write_price_per_token?: number;
}>;

export type SurplusModelsSnapshot = Readonly<{
  models: readonly SurplusModel[];
  updated_at_ms: number;
}>;

export type SurplusFetch = (
  input: RequestInfo | URL,
  init?: RequestInit,
) => Promise<Response>;

export type SurplusErrorCode =
  | "surplus_api_key_missing"
  | "surplus_request_invalid"
  | "surplus_upstream_unreachable";

export class SurplusError extends Error {
  readonly code: SurplusErrorCode;
  readonly status: number;
  readonly upstream_status: number | null;

  constructor(
    message: string,
    code: SurplusErrorCode,
    status: number,
    upstreamStatus: number | null = null,
  ) {
    super(message);
    this.name = "SurplusError";
    this.code = code;
    this.status = status;
    this.upstream_status = upstreamStatus;
  }
}

export type SurplusAuthenticatedFetchOptions = Readonly<{
  apiKey?: string | null;
  fetcher?: SurplusFetch;
  signal?: AbortSignal;
  beforeDispatch?: () => Promise<ApiKeyProviderDispatch | void>;
  onDispatch?: () => void;
}>;

export type SurplusResponsesResult = Readonly<{
  response: Response;
  request_id: string | null;
}>;

type JsonRecord = Record<string, unknown>;

const isRecord = (value: unknown): value is JsonRecord =>
  value !== null && typeof value === "object" && !Array.isArray(value);

const nonEmptyString = (value: unknown): string | null => {
  if (typeof value !== "string") return null;
  const trimmed = value.trim();
  return trimmed || null;
};

const isNonNegativeSafeInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const nonNegativeNumber = (value: unknown): number | null => {
  if (typeof value === "number" && Number.isFinite(value) && value >= 0) return value;
  if (typeof value !== "string" || !value.trim()) return null;
  const parsed = Number(value);
  return Number.isFinite(parsed) && parsed >= 0 ? parsed : null;
};

const readTextModalities = (value: unknown): {
  inputKnown: boolean;
  outputKnown: boolean;
  inputText: boolean;
  outputText: boolean;
} => {
  if (!isRecord(value)) return { inputKnown: false, outputKnown: false, inputText: false, outputText: false };
  let inputKnown = false;
  let outputKnown = false;
  let inputText = false;
  let outputText = false;
  const inputModalities = Array.isArray(value.input_modalities)
    ? value.input_modalities.filter((entry): entry is string => typeof entry === "string")
    : [];
  if (inputModalities.length) {
    inputKnown = true;
    inputText = inputModalities.includes("text");
  }
  const outputModalities = Array.isArray(value.output_modalities)
    ? value.output_modalities.filter((entry): entry is string => typeof entry === "string")
    : [];
  if (outputModalities.length) {
    outputKnown = true;
    outputText = outputModalities.includes("text");
  }
  const modality = nonEmptyString(value.modality);
  if (modality) {
    const hasTextModality = (part: string): boolean => part.split(/[+,]/).map((entry) => entry.trim()).includes("text");
    const arrow = modality.lastIndexOf("->");
    if (arrow >= 0) {
      inputKnown = true;
      outputKnown = true;
      inputText = hasTextModality(modality.slice(0, arrow));
      outputText = hasTextModality(modality.slice(arrow + 2));
    } else {
      inputKnown = true;
      outputKnown = true;
      inputText = outputText = hasTextModality(modality);
    }
  }
  return { inputKnown, outputKnown, inputText, outputText };
};

const modelSupportsTextOutput = (value: JsonRecord): boolean => {
  const architecture = value.architecture;
  if (!isRecord(architecture)) return true;
  const modalities = readTextModalities(architecture);
  return !modalities.outputKnown || modalities.outputText;
};

const modelSupportsTextInput = (value: JsonRecord): boolean => {
  const architecture = value.architecture;
  if (!isRecord(architecture)) return true;
  const modalities = readTextModalities(architecture);
  return !modalities.inputKnown || modalities.inputText;
};

const readPricing = (value: JsonRecord): Readonly<{
  input_price_per_token?: number;
  output_price_per_token?: number;
  cache_read_price_per_token?: number;
  cache_write_price_per_token?: number;
}> => {
  if (!isRecord(value.pricing)) return {};
  const input = nonNegativeNumber(value.pricing.prompt ?? value.pricing.input);
  const output = nonNegativeNumber(value.pricing.completion ?? value.pricing.output);
  const cacheRead = nonNegativeNumber(
    value.pricing.input_cache_read ?? value.pricing.cache_read ?? value.pricing.cache_read_input,
  );
  const cacheWrite = nonNegativeNumber(
    value.pricing.input_cache_write ?? value.pricing.cache_write ?? value.pricing.cache_write_input,
  );
  return {
    ...(input === null ? {} : { input_price_per_token: input }),
    ...(output === null ? {} : { output_price_per_token: output }),
    ...(cacheRead === null ? {} : { cache_read_price_per_token: cacheRead }),
    ...(cacheWrite === null ? {} : { cache_write_price_per_token: cacheWrite }),
  };
};

const readSurplusModel = (value: unknown): SurplusModel | null => {
  if (!isRecord(value) || !modelSupportsTextInput(value) || !modelSupportsTextOutput(value)) return null;
  const id = nonEmptyString(value.id);
  if (!id) return null;
  const created = isNonNegativeSafeInteger(value.created) ? value.created : 0;
  const ownedBy = nonEmptyString(value.provider) ?? nonEmptyString(value.owned_by) ?? "surplus";
  const description = nonEmptyString(value.description);
  return {
    id,
    object: "model",
    created,
    owned_by: ownedBy,
    supported_endpoint_types: ["openai", "openai-response"],
    ...(description ? { description } : {}),
    ...readPricing(value),
  };
};

let surplusModelsCache: SurplusModelsSnapshot | null = null;
let surplusModelsFetchInFlight: Promise<SurplusModelsSnapshot | null> | null = null;
let surplusModelsFetchGeneration = 0;
let surplusModelsCacheGeneration = 0;
let surplusModelsRetryAfterMs = 0;

export const readSurplusApiKey = (): string | null => {
  try {
    return nonEmptyString(Deno.env.get(SURPLUS_API_KEY_ENV));
  } catch {
    return null;
  }
};

const boundedModelsSignal = (signal: AbortSignal | undefined): AbortSignal => {
  const timeout = AbortSignal.timeout(SURPLUS_FETCH_TIMEOUT_MS);
  return signal ? AbortSignal.any([signal, timeout]) : timeout;
};

const markSurplusModelsFetchFailure = (requestGeneration: number): void => {
  if (requestGeneration < surplusModelsCacheGeneration) return;
  surplusModelsRetryAfterMs = Date.now() + SURPLUS_MODELS_FAILURE_BACKOFF_MS;
};

export const fetchSurplusModels = async (
  options: Readonly<{
    apiKey?: string | null;
    fetcher?: SurplusFetch;
    signal?: AbortSignal;
    force?: boolean;
    cachedOnly?: boolean;
    requireApiKey?: boolean;
  }> = {},
): Promise<SurplusModelsSnapshot | null> => {
  const apiKey = options.apiKey === undefined ? readSurplusApiKey() : nonEmptyString(options.apiKey);
  if ((options.requireApiKey ?? true) && !apiKey) return null;
  if (
    !options.force && surplusModelsCache &&
    Date.now() - surplusModelsCache.updated_at_ms < SURPLUS_MODELS_CACHE_TTL_MS
  ) return surplusModelsCache;
  if (!options.force && Date.now() < surplusModelsRetryAfterMs) return surplusModelsCache;
  if (options.cachedOnly) return surplusModelsCache;
  // Only the ordinary discovery path shares an upstream request. Callers that
  // supply a signal, fetcher, API key, or force a refresh retain their own
  // request semantics and do not join another caller's request.
  const shouldCoalesce = !options.force && !options.cachedOnly && options.apiKey === undefined &&
    options.fetcher === undefined && options.signal === undefined && options.requireApiKey === undefined;
  if (shouldCoalesce && surplusModelsFetchInFlight) return await surplusModelsFetchInFlight;

  const requestGeneration = ++surplusModelsFetchGeneration;
  const request = (async (): Promise<SurplusModelsSnapshot | null> => {
    const signal = boundedModelsSignal(options.signal);
    try {
      const response = await (options.fetcher ?? fetch)(SURPLUS_MODELS_URL, {
        method: "GET",
        headers: { Accept: "application/json" },
        redirect: "manual",
        signal,
      });
      if (!response.ok) {
        markSurplusModelsFetchFailure(requestGeneration);
        return surplusModelsCache;
      }
      const payload = await response.json() as unknown;
      if (!isRecord(payload) || !Array.isArray(payload.data)) {
        markSurplusModelsFetchFailure(requestGeneration);
        return surplusModelsCache;
      }
      const models: SurplusModel[] = [];
      const seen = new Set<string>();
      for (const value of payload.data) {
        const model = readSurplusModel(value);
        if (!model || seen.has(model.id)) continue;
        models.push(model);
        seen.add(model.id);
      }
      const snapshot = { models, updated_at_ms: Date.now() } satisfies SurplusModelsSnapshot;
      if (requestGeneration > surplusModelsCacheGeneration) {
        surplusModelsCacheGeneration = requestGeneration;
        surplusModelsCache = snapshot;
        surplusModelsRetryAfterMs = 0;
      }
      return surplusModelsCache;
    } catch (error) {
      if (signal.aborted && options.signal?.aborted) throw error;
      markSurplusModelsFetchFailure(requestGeneration);
      return surplusModelsCache;
    }
  })();
  if (!shouldCoalesce) return await request;
  surplusModelsFetchInFlight = request;
  try {
    return await request;
  } finally {
    if (surplusModelsFetchInFlight === request) surplusModelsFetchInFlight = null;
  }
};

export const resetSurplusModelsCacheForTest = (): void => {
  surplusModelsFetchInFlight = null;
  surplusModelsFetchGeneration += 1;
  surplusModelsCacheGeneration = surplusModelsFetchGeneration;
  surplusModelsRetryAfterMs = 0;
  surplusModelsCache = null;
};

const requireSurplusApiKey = (supplied: string | null | undefined): string => {
  const apiKey = supplied === undefined ? readSurplusApiKey() : nonEmptyString(supplied);
  if (apiKey) return apiKey;
  throw new SurplusError(
    "Surplus paid fallback is unavailable because SURPLUS_API_KEY is not configured.",
    "surplus_api_key_missing",
    503,
  );
};

const awaitWithAbort = <T>(operation: PromiseLike<T>, signal: AbortSignal): Promise<T> =>
  new Promise<T>((resolve, reject) => {
    let settled = false;
    const finish = (callback: () => void): void => {
      if (settled) return;
      settled = true;
      signal.removeEventListener("abort", onAbort);
      callback();
    };
    const onAbort = (): void => {
      finish(() => reject(signal.reason ?? new DOMException("Aborted", "AbortError")));
    };
    if (signal.aborted) {
      onAbort();
      return;
    }
    signal.addEventListener("abort", onAbort, { once: true });
    Promise.resolve(operation).then(
      (value) => finish(() => resolve(value)),
      (error) => finish(() => reject(error)),
    );
  });

const authenticatedHeaders = (apiKey: string): Headers => {
  const headers = new Headers({
    Authorization: `Bearer ${apiKey}`,
    Accept: "text/event-stream",
    "Content-Type": "application/json",
  });
  return headers;
};

const responseRequestId = (response: Response): string | null =>
  nonEmptyString(
    response.headers.get("X-Request-Id") ??
      response.headers.get("X-Api-Request-Id") ??
      response.headers.get("X-Oneapi-Request-Id"),
  );

const toSurplusResponsesBody = (body: JsonRecord): JsonRecord => {
  if (!isRecord(body.reasoning) || body.reasoning.effort !== "ultra") return body;
  return {
    ...body,
    reasoning: { ...body.reasoning, effort: "max" },
  };
};

export const fetchSurplusResponses = async (
  body: unknown,
  options: SurplusAuthenticatedFetchOptions = {},
): Promise<SurplusResponsesResult> => {
  if (!isRecord(body)) {
    throw new SurplusError(
      "Surplus Responses requests must use a canonical JSON object body.",
      "surplus_request_invalid",
      400,
    );
  }

  let encodedBody: string;
  try {
    encodedBody = JSON.stringify(toSurplusResponsesBody(body));
  } catch {
    throw new SurplusError(
      "Surplus Responses requests must use a JSON-serializable body.",
      "surplus_request_invalid",
      400,
    );
  }
  if (typeof encodedBody !== "string") {
    throw new SurplusError(
      "Surplus Responses requests must use a JSON-serializable body.",
      "surplus_request_invalid",
      400,
    );
  }

  const apiKey = requireSurplusApiKey(options.apiKey);
  const headers = authenticatedHeaders(apiKey);
  const headersDeadline = new AbortController();
  const headersTimer = setTimeout(
    () => headersDeadline.abort(new DOMException("Surplus response headers timed out.", "TimeoutError")),
    STREAM_FIRST_EVENT_DEADLINE_MS,
  );
  const signal = options.signal ? AbortSignal.any([options.signal, headersDeadline.signal]) : headersDeadline.signal;
  let response: Response;
  try {
    const dispatch = options.beforeDispatch ? await options.beforeDispatch() : undefined;
    if (signal.aborted) {
      await dispatch?.cancelBeforeTransport();
      throw signal.reason ?? new DOMException("The request was aborted.", "AbortError");
    }
    dispatch?.markTransportStarted();
    options.onDispatch?.();
    response = await awaitWithAbort(
      (options.fetcher ?? fetch)(SURPLUS_RESPONSES_URL, {
        method: "POST",
        headers,
        body: encodedBody,
        redirect: "manual",
        signal,
      }),
      signal,
    );
  } catch (error) {
    if (error instanceof ApiKeyQuotaDispatchError) throw error;
    if (options.signal?.aborted) throw options.signal.reason ?? error;
    if (headersDeadline.signal.aborted) throw headersDeadline.signal.reason ?? error;
    if (error instanceof Error && error.name === "AbortError") throw error;
    throw new SurplusError(
      "Surplus Responses request could not reach the upstream service.",
      "surplus_upstream_unreachable",
      502,
    );
  } finally {
    clearTimeout(headersTimer);
  }

  return { response, request_id: responseRequestId(response) };
};
