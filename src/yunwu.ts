import { STREAM_FIRST_EVENT_DEADLINE_MS } from "./inference_deadline.ts";
import { ApiKeyQuotaDispatchError } from "./api_key_policy.ts";

export const YUNWU_BASE_URL = "https://yunwu.ai";

const YUNWU_API_KEY_ENV = "YUNWU_API_KEY";
const YUNWU_RATIO_CONFIG_URL = `${YUNWU_BASE_URL}/api/ratio_config`;
const YUNWU_STATUS_URL = `${YUNWU_BASE_URL}/api/status`;
const YUNWU_RESPONSES_URL = `${YUNWU_BASE_URL}/v1/responses`;
const YUNWU_TOKEN_LOGS_URL = `${YUNWU_BASE_URL}/api/log/token`;
// Billing reconciliation runs after the client response and must not hold a
// queue delivery indefinitely when the provider stalls.
export const YUNWU_FETCH_TIMEOUT_MS = 10_000;
export const YUNWU_TOKEN_LOG_FETCH_TIMEOUT_MS = YUNWU_FETCH_TIMEOUT_MS;

export type YunwuFetch = (
  input: RequestInfo | URL,
  init?: RequestInit,
) => Promise<Response>;

export type YunwuErrorCode =
  | "yunwu_api_key_missing"
  | "yunwu_pricing_unavailable"
  | "yunwu_pricing_invalid"
  | "yunwu_status_unavailable"
  | "yunwu_status_invalid"
  | "yunwu_request_invalid"
  | "yunwu_upstream_unreachable"
  | "yunwu_logs_unavailable"
  | "yunwu_logs_invalid";

export class YunwuError extends Error {
  readonly code: YunwuErrorCode;
  readonly status: number;
  readonly upstream_status: number | null;

  constructor(
    message: string,
    code: YunwuErrorCode,
    status: number,
    upstreamStatus: number | null = null,
  ) {
    super(message);
    this.name = "YunwuError";
    this.code = code;
    this.status = status;
    this.upstream_status = upstreamStatus;
  }
}

export type YunwuPricingSnapshot = Readonly<{
  eligible_model_ids: readonly string[];
  quota_per_credit: number;
  model_quota_coefficients: Readonly<Record<string, number>>;
  checked_at_ms: number;
}>;

export type InitializeYunwuPricingOptions = Readonly<{
  codexModelIds: readonly string[];
  fetcher?: YunwuFetch;
  now?: () => number;
  signal?: AbortSignal;
}>;

export type YunwuAuthenticatedFetchOptions = Readonly<{
  apiKey?: string | null;
  fetcher?: YunwuFetch;
  signal?: AbortSignal;
  beforeDispatch?: () => Promise<void>;
}>;

export type YunwuTokenLogFetchOptions =
  & YunwuAuthenticatedFetchOptions
  & Readonly<{
    requestIds?: readonly string[];
    startAtMs?: number;
    endAtMs?: number;
  }>;

export type YunwuResponsesResult = Readonly<{
  response: Response;
  request_id: string | null;
}>;

const YUNWU_REASONING_SUFFIX_MODELS = new Set(["gpt-5.6-sol"]);

const toYunwuResponsesBody = (body: JsonRecord): JsonRecord => {
  const model = nonEmptyString(body.model);
  const reasoning = isRecord(body.reasoning) ? nonEmptyString(body.reasoning.effort) : null;
  if (!model || !reasoning || !YUNWU_REASONING_SUFFIX_MODELS.has(model)) return body;

  const suffix = reasoning === "none" || reasoning === "minimal" ? "low" : reasoning === "ultra" ? "max" : reasoning;
  if (!["low", "medium", "high", "xhigh", "max"].includes(suffix)) return body;

  const result: JsonRecord = { ...body, model: `${model}-${suffix}` };
  delete result.reasoning;
  return result;
};

export type YunwuTokenLogEntry = Readonly<{
  request_id: string;
  quota: number;
  prompt_tokens: number;
  completion_tokens: number;
  model: string;
  created_at: number;
}>;

type JsonRecord = Record<string, unknown>;

const isRecord = (value: unknown): value is JsonRecord =>
  value !== null && typeof value === "object" && !Array.isArray(value);

const isPositiveFiniteNumber = (value: unknown): value is number =>
  typeof value === "number" && Number.isFinite(value) && value > 0;

const isNonNegativeSafeInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const nonEmptyString = (value: unknown): string | null => {
  if (typeof value !== "string") return null;
  const trimmed = value.trim();
  return trimmed || null;
};

const rethrowCancellation = (error: unknown, signal: AbortSignal | undefined): void => {
  if (signal?.aborted || (error instanceof Error && error.name === "AbortError")) throw error;
};

const boundedTokenLogSignal = (signal: AbortSignal | undefined): AbortSignal => {
  const timeout = AbortSignal.timeout(YUNWU_FETCH_TIMEOUT_MS);
  return signal ? AbortSignal.any([signal, timeout]) : timeout;
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

export const readYunwuApiKey = (): string | null => {
  try {
    return nonEmptyString(Deno.env.get(YUNWU_API_KEY_ENV));
  } catch {
    return null;
  }
};

const requireYunwuApiKey = (supplied: string | null | undefined): string => {
  const apiKey = supplied === undefined ? readYunwuApiKey() : nonEmptyString(supplied);
  if (apiKey) return apiKey;
  throw new YunwuError(
    "YunWu paid fallback is unavailable because YUNWU_API_KEY is not configured.",
    "yunwu_api_key_missing",
    503,
  );
};

const metadataHeaders = (): Headers => {
  const headers = new Headers();
  headers.set("Accept", "application/json");
  return headers;
};

const authenticatedHeaders = (apiKey: string, accept: string): Headers => {
  const headers = new Headers();
  headers.set("Authorization", `Bearer ${apiKey}`);
  headers.set("Accept", accept);
  return headers;
};

const fetchMetadataJson = async (
  url: string,
  fetcher: YunwuFetch,
  signal: AbortSignal | undefined,
  unavailableCode: "yunwu_pricing_unavailable" | "yunwu_status_unavailable",
  invalidCode: "yunwu_pricing_invalid" | "yunwu_status_invalid",
): Promise<unknown> => {
  let response: Response;
  try {
    response = await fetcher(url, {
      method: "GET",
      headers: metadataHeaders(),
      redirect: "manual",
      signal,
    });
  } catch (error) {
    rethrowCancellation(error, signal);
    throw new YunwuError(
      "YunWu pricing initialization could not reach the metadata service.",
      unavailableCode,
      502,
    );
  }

  if (!response.ok) {
    throw new YunwuError(
      "YunWu pricing initialization received an unsuccessful metadata response.",
      unavailableCode,
      502,
      response.status,
    );
  }

  try {
    return await response.json() as unknown;
  } catch {
    throw new YunwuError(
      "YunWu pricing initialization received invalid metadata.",
      invalidCode,
      502,
      response.status,
    );
  }
};

const unwrapSuccessfulEnvelope = (
  value: unknown,
  code: "yunwu_pricing_invalid" | "yunwu_status_invalid",
): JsonRecord => {
  if (!isRecord(value) || value.success !== true || !isRecord(value.data)) {
    throw new YunwuError(
      "YunWu pricing initialization received an invalid metadata envelope.",
      code,
      502,
    );
  }
  return value.data;
};

const pricedModelIds = (pricing: JsonRecord): Set<string> => {
  const ratioMap = pricing.model_ratio;
  const fixedPriceMap = pricing.model_price;
  if (!isRecord(ratioMap) || !isRecord(fixedPriceMap)) {
    throw new YunwuError(
      "YunWu pricing initialization received an invalid pricing configuration.",
      "yunwu_pricing_invalid",
      502,
    );
  }

  const result = new Set<string>();
  for (const pricingMap of [ratioMap, fixedPriceMap]) {
    for (const [modelId, value] of Object.entries(pricingMap)) {
      if (nonEmptyString(modelId) && isPositiveFiniteNumber(value)) result.add(modelId);
    }
  }
  return result;
};

const modelQuotaCoefficients = (pricing: JsonRecord): Record<string, number> => {
  const ratios = pricing.model_ratio;
  const fixedPrices = pricing.model_price;
  const completionRatios = pricing.completion_ratio;
  if (!isRecord(ratios) || !isRecord(fixedPrices)) {
    throw new YunwuError(
      "YunWu pricing initialization received an invalid pricing configuration.",
      "yunwu_pricing_invalid",
      502,
    );
  }
  const result: Record<string, number> = {};
  for (const [model, ratio] of Object.entries(ratios)) {
    if (!isPositiveFiniteNumber(ratio)) continue;
    const completion = isRecord(completionRatios) && isPositiveFiniteNumber(completionRatios[model])
      ? completionRatios[model]
      : 1;
    const coefficient = ratio * (1 + completion);
    if (Number.isFinite(coefficient) && coefficient > 0) result[model] = coefficient;
  }
  for (const [model, price] of Object.entries(fixedPrices)) {
    if (isPositiveFiniteNumber(price)) result[model] = price;
  }
  return result;
};

const normalizeCodexModelIds = (value: readonly string[]): string[] => {
  if (!Array.isArray(value)) {
    throw new YunwuError(
      "YunWu pricing initialization requires the current Codex model catalog.",
      "yunwu_pricing_invalid",
      502,
    );
  }

  const result: string[] = [];
  const seen = new Set<string>();
  for (const rawModelId of value) {
    const modelId = nonEmptyString(rawModelId);
    if (!modelId) {
      throw new YunwuError(
        "YunWu pricing initialization received an invalid Codex model identifier.",
        "yunwu_pricing_invalid",
        502,
      );
    }
    if (seen.has(modelId)) continue;
    seen.add(modelId);
    result.push(modelId);
  }
  return result;
};

export const initializeYunwuPricing = async (
  options: InitializeYunwuPricingOptions,
): Promise<YunwuPricingSnapshot> => {
  const fetcher = options.fetcher ?? fetch;
  const [pricingEnvelope, statusEnvelope] = await Promise.all([
    fetchMetadataJson(
      YUNWU_RATIO_CONFIG_URL,
      fetcher,
      options.signal,
      "yunwu_pricing_unavailable",
      "yunwu_pricing_invalid",
    ),
    fetchMetadataJson(
      YUNWU_STATUS_URL,
      fetcher,
      options.signal,
      "yunwu_status_unavailable",
      "yunwu_status_invalid",
    ),
  ]);

  const pricing = unwrapSuccessfulEnvelope(pricingEnvelope, "yunwu_pricing_invalid");
  const status = unwrapSuccessfulEnvelope(statusEnvelope, "yunwu_status_invalid");
  if (
    status.setup !== true || !isPositiveFiniteNumber(status.quota_per_unit) ||
    !Number.isSafeInteger(status.quota_per_unit)
  ) {
    throw new YunwuError(
      "YunWu pricing initialization received an invalid quota conversion.",
      "yunwu_status_invalid",
      502,
    );
  }

  const availableModelIds = pricedModelIds(pricing);
  const coefficients = modelQuotaCoefficients(pricing);
  const eligibleModelIds = normalizeCodexModelIds(options.codexModelIds)
    .filter((modelId) => availableModelIds.has(modelId));
  const checkedAtMs = Math.trunc((options.now ?? Date.now)());
  if (!Number.isSafeInteger(checkedAtMs) || checkedAtMs < 0) {
    throw new YunwuError(
      "YunWu pricing initialization received an invalid clock value.",
      "yunwu_status_invalid",
      502,
    );
  }

  return {
    eligible_model_ids: eligibleModelIds,
    quota_per_credit: status.quota_per_unit,
    model_quota_coefficients: Object.fromEntries(
      eligibleModelIds.flatMap((model) => coefficients[model] ? [[model, coefficients[model]]] : []),
    ),
    checked_at_ms: checkedAtMs,
  };
};

export const fetchYunwuResponses = async (
  body: unknown,
  options: YunwuAuthenticatedFetchOptions = {},
): Promise<YunwuResponsesResult> => {
  if (!isRecord(body)) {
    throw new YunwuError(
      "YunWu Responses requests must use a canonical JSON object body.",
      "yunwu_request_invalid",
      400,
    );
  }

  let encodedBody: string;
  try {
    encodedBody = JSON.stringify(toYunwuResponsesBody(body));
  } catch {
    throw new YunwuError(
      "YunWu Responses requests must use a JSON-serializable body.",
      "yunwu_request_invalid",
      400,
    );
  }
  if (typeof encodedBody !== "string") {
    throw new YunwuError(
      "YunWu Responses requests must use a JSON-serializable body.",
      "yunwu_request_invalid",
      400,
    );
  }

  const apiKey = requireYunwuApiKey(options.apiKey);
  const headers = authenticatedHeaders(apiKey, "text/event-stream");
  headers.set("Content-Type", "application/json");

  let response: Response;
  const headersDeadline = new AbortController();
  const headersTimer = setTimeout(
    () => headersDeadline.abort(new DOMException("YunWu response headers timed out.", "TimeoutError")),
    STREAM_FIRST_EVENT_DEADLINE_MS,
  );
  const signal = options.signal ? AbortSignal.any([options.signal, headersDeadline.signal]) : headersDeadline.signal;
  try {
    // Dispatch-based quota accounting must settle immediately before this
    // real provider transport, after all local validation has succeeded.
    // Do not `await undefined`: that would insert a microtask boundary before
    // a normal fetch and let an immediately aborted caller race past a custom
    // transport's abort listener. When a quota hook exists it remains the
    // final awaited operation before transport.
    if (options.beforeDispatch) await options.beforeDispatch();
    if (signal.aborted) {
      throw signal.reason ?? new DOMException("The request was aborted.", "AbortError");
    }
    response = await (options.fetcher ?? fetch)(YUNWU_RESPONSES_URL, {
      method: "POST",
      headers,
      body: encodedBody,
      redirect: "manual",
      signal,
    });
  } catch (error) {
    if (error instanceof ApiKeyQuotaDispatchError) throw error;
    if (options.signal?.aborted) throw options.signal.reason ?? error;
    if (headersDeadline.signal.aborted) throw headersDeadline.signal.reason ?? error;
    rethrowCancellation(error, signal);
    throw new YunwuError(
      "YunWu Responses request could not reach the upstream service.",
      "yunwu_upstream_unreachable",
      502,
    );
  } finally {
    // The deadline covers only request dispatch and response headers. Once a
    // streaming body exists, the shared SSE reader owns renewable inactivity.
    clearTimeout(headersTimer);
  }

  return {
    response,
    request_id: nonEmptyString(
      response.headers.get("X-Api-Request-Id") ??
        response.headers.get("X-Oneapi-Request-Id"),
    ),
  };
};

const normalizeTokenLogEntry = (value: unknown): YunwuTokenLogEntry | null => {
  if (!isRecord(value)) return null;
  let requestId = nonEmptyString(value.request_id);
  if (!requestId && typeof value.other === "string") {
    try {
      const other = JSON.parse(value.other) as unknown;
      if (isRecord(other)) requestId = nonEmptyString(other.request_id);
    } catch {
      // Ignore malformed provider metadata.
    }
  }
  const model = nonEmptyString(value.model_name);
  if (
    !requestId ||
    !model ||
    !isNonNegativeSafeInteger(value.quota) ||
    !isNonNegativeSafeInteger(value.prompt_tokens) ||
    !isNonNegativeSafeInteger(value.completion_tokens) ||
    !isNonNegativeSafeInteger(value.created_at)
  ) {
    return null;
  }

  return {
    request_id: requestId,
    quota: value.quota,
    prompt_tokens: value.prompt_tokens,
    completion_tokens: value.completion_tokens,
    model,
    created_at: value.created_at,
  };
};

export const fetchYunwuTokenLogs = async (
  options: YunwuTokenLogFetchOptions = {},
): Promise<readonly YunwuTokenLogEntry[]> => {
  const apiKey = requireYunwuApiKey(options.apiKey);
  const signal = boundedTokenLogSignal(options.signal);
  const requestedIds = new Set(options.requestIds?.map((id) => id.trim()).filter(Boolean) ?? []);
  const foundIds = new Set<string>();
  const logs: YunwuTokenLogEntry[] = [];
  const pageSize = 100;

  for (let page = 1; page <= 100; page += 1) {
    const url = new URL(YUNWU_TOKEN_LOGS_URL);
    url.searchParams.set("key", apiKey);
    url.searchParams.set("page", String(page));
    url.searchParams.set("page_size", String(pageSize));
    if (Number.isFinite(options.startAtMs)) {
      url.searchParams.set("start_timestamp", String(Math.max(0, Math.floor(options.startAtMs! / 1_000))));
    }
    if (Number.isFinite(options.endAtMs)) {
      url.searchParams.set("end_timestamp", String(Math.max(0, Math.ceil(options.endAtMs! / 1_000))));
    }

    let response: Response;
    try {
      const responsePromise = (options.fetcher ?? fetch)(url, {
        method: "GET",
        headers: metadataHeaders(),
        redirect: "manual",
        signal,
      });
      response = await awaitWithAbort(
        responsePromise,
        signal,
      );
    } catch (error) {
      rethrowCancellation(error, signal);
      throw new YunwuError(
        "YunWu billing logs could not be reached.",
        "yunwu_logs_unavailable",
        502,
      );
    }

    if (!response.ok) {
      throw new YunwuError(
        "YunWu billing logs returned an unsuccessful response.",
        "yunwu_logs_unavailable",
        502,
        response.status,
      );
    }

    let envelope: unknown;
    try {
      envelope = await awaitWithAbort(response.json(), signal) as unknown;
    } catch (error) {
      rethrowCancellation(error, signal);
      throw new YunwuError(
        "YunWu billing logs returned invalid JSON.",
        "yunwu_logs_invalid",
        502,
        response.status,
      );
    }
    if (!isRecord(envelope) || envelope.success !== true) {
      throw new YunwuError(
        "YunWu billing logs returned an invalid response envelope.",
        "yunwu_logs_invalid",
        502,
        response.status,
      );
    }
    const data = envelope.data;
    const items = Array.isArray(data) ? data : isRecord(data) && Array.isArray(data.items) ? data.items : null;
    if (!items) {
      throw new YunwuError(
        "YunWu billing logs returned an invalid response envelope.",
        "yunwu_logs_invalid",
        502,
        response.status,
      );
    }
    for (const item of items) {
      const log = normalizeTokenLogEntry(item);
      if (!log) continue;
      logs.push(log);
      if (requestedIds.has(log.request_id)) foundIds.add(log.request_id);
    }
    if (requestedIds.size > 0 && foundIds.size === requestedIds.size) break;
    const total = isRecord(data) && isNonNegativeSafeInteger(data.total) ? data.total : items.length;
    if (page * pageSize >= total || items.length === 0) break;
  }
  return logs;
};
