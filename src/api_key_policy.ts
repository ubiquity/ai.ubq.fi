import {
  API_KEY_NO_EXPIRATION_MS,
  API_KEY_NO_USAGE_LIMIT,
  apiKeyHashKey,
  normalizeApiKeyWindowMs,
} from "./api_keys.ts";
import { openaiError, STANDARD_RATE_LIMIT_HEADERS } from "./http.ts";
import { getKv, KV_OPERATION_TIMEOUT_MS, KvCircuitOpenError, KvOperationTimeoutError, type KvOperationName, withKvOperation } from "./kv.ts";
import { hasStrictPaidFallbackPolicy } from "./paid_fallback.ts";
import type { ApiKeyHashRecord, ApiKeyUsageRequestV3, ApiKeyUsageWindowV3 } from "./types.ts";
import { isRecord, sha256Base64Url } from "./utils.ts";

export const API_KEY_POLICY_CACHE_TTL_MS = 30_000;
/** Retained only so the incident migration can consume old-isolate counters. */
export const API_KEY_USAGE_V2_PREFIX = ["uos_ai", "api_key_usage", "v2"] as const;
export const API_KEY_USAGE_V3_PREFIX = ["uos_ai", "api_key_usage", "v3"] as const;
export const API_KEY_USAGE_V3_WINDOW_PREFIX = [...API_KEY_USAGE_V3_PREFIX, "window"] as const;
export const API_KEY_USAGE_V3_REQUEST_PREFIX = [...API_KEY_USAGE_V3_PREFIX, "request"] as const;
export const API_KEY_USAGE_V3_EXPIRY_PREFIX = [...API_KEY_USAGE_V3_PREFIX, "expiry"] as const;
export const API_KEY_USAGE_V3_RESERVATION_LEASE_MS = 5 * 60_000;
export const API_KEY_USAGE_V3_RETENTION_MS = 7 * 24 * 60 * 60_000;
export const API_KEY_QUOTA_ADMISSION_DEADLINE_MS = 1_500;
const API_KEY_USAGE_V3_RECLAIM_BATCH_SIZE = 32;
const MAX_KV_RETRIES = 5;

export type ApiKeyUsageProvider =
  | "cerebras"
  | "chatgpt_codex"
  | "removed_provider"
  | "metered"
  | "surplus"
  | "voyage";

export type ApiKeyProviderDispatch = Readonly<{
  markTransportStarted: () => void;
  cancelBeforeTransport: () => Promise<void>;
}>;

export type ApiKeyPolicy = Readonly<{
  token_hash: string;
  key_id: string;
  expires_at_ms: number;
  usage_limit_requests: number;
  window_ms: number;
  window_start_ms: number;
  usage_reset_at_ms: number;
  policy_version: string;
  paid_fallback_enabled: boolean;
  paid_fallback_limit_microcredits: number;
  paid_fallback_spent_microcredits: number;
  paid_fallback_reserved_microcredits: number;
  paid_fallback_reservation_request_id: string | null;
}>;

export type ApiKeyPolicyDecision =
  | Readonly<{ ok: true; policy: ApiKeyPolicy }>
  | Readonly<{ ok: false; response: Response }>;

export type ApiKeyUsageReservation = Readonly<{
  policy: ApiKeyPolicy;
  request_id: string;
  route: string;
  beforeProviderDispatch: (provider: ApiKeyUsageProvider) => Promise<ApiKeyProviderDispatch | void>;
  release: (reason?: string) => Promise<void>;
}>;

export type ApiKeyUsageReservationDecision =
  | Readonly<{ ok: true; reservation: ApiKeyUsageReservation }>
  | Readonly<{ ok: false; response: Response }>;

export class ApiKeyQuotaDispatchError extends Error {
  readonly status: number;
  readonly code: string;
  readonly errorType: string;
  readonly retryAfter: string | null;
  readonly headers: Readonly<Record<string, string>>;

  constructor(
    message = "API key quota reservation is no longer available",
    options: Readonly<{
      status?: number;
      code?: string;
      errorType?: string;
      retryAfter?: string | null;
      headers?: Readonly<Record<string, string>>;
    }> = {},
  ) {
    super(message);
    this.name = "ApiKeyQuotaDispatchError";
    this.status = options.status ?? 503;
    this.code = options.code ?? "api_key_quota_reservation_unavailable";
    this.errorType = options.errorType ?? "server_error";
    const headers = { ...(options.headers ?? {}) };
    this.retryAfter = options.retryAfter ?? headers["Retry-After"] ?? null;
    if (this.retryAfter && !headers["Retry-After"]) headers["Retry-After"] = this.retryAfter;
    this.headers = headers;
  }
}

type CachedPolicy = Readonly<{ policy: ApiKeyPolicy; expires_at_ms: number }>;
const policyCache = new Map<string, CachedPolicy>();

export const looksLikeUosApiKey = (token: string): boolean => /^u_[0-9a-f]{64}$/.test(token.trim());

export const currentApiKeyUsageWindow = (
  usageResetAtMs: number,
  windowMs: number,
  nowMs: number,
): { start: number; reset: number } => {
  const initialStart = usageResetAtMs - windowMs;
  if (nowMs < usageResetAtMs) return { start: initialStart, reset: usageResetAtMs };
  const elapsedWindows = Math.floor((nowMs - initialStart) / windowMs);
  const start = initialStart + elapsedWindows * windowMs;
  return { start, reset: start + windowMs };
};

const hasUsageQuotaV3 = (record: unknown): boolean => isRecord(record) && record.usage_quota_version === 3;

const policyVersion = (record: ApiKeyHashRecord): string => `v3:${normalizeApiKeyWindowMs(record.window_ms)}`;

export const apiKeyPolicyFromHashRecord = (
  tokenHash: string,
  record: ApiKeyHashRecord,
  nowMs: number,
): ApiKeyPolicy | null => {
  if (!hasStrictPaidFallbackPolicy(record) || !hasUsageQuotaV3(record)) return null;
  const windowMs = normalizeApiKeyWindowMs(record.window_ms);
  const window = currentApiKeyUsageWindow(record.usage_reset_at_ms, windowMs, nowMs);
  return {
    token_hash: tokenHash,
    key_id: record.id,
    expires_at_ms: Number.isFinite(record.expires_at_ms) ? Math.trunc(record.expires_at_ms) : API_KEY_NO_EXPIRATION_MS,
    usage_limit_requests: record.usage_limit_requests,
    window_ms: windowMs,
    window_start_ms: window.start,
    usage_reset_at_ms: window.reset,
    policy_version: policyVersion(record),
    paid_fallback_enabled: record.paid_fallback_enabled,
    paid_fallback_limit_microcredits: record.paid_fallback_limit_microcredits,
    paid_fallback_spent_microcredits: record.paid_fallback_spent_microcredits,
    paid_fallback_reserved_microcredits: record.paid_fallback_reserved_microcredits,
    paid_fallback_reservation_request_id: record.paid_fallback_reservation_request_id,
  };
};

/** Legacy key for one-way V2-to-V3 incident reconciliation only. */
export const apiKeyUsageV2Key = (policy: Pick<ApiKeyPolicy, "key_id" | "policy_version" | "window_start_ms">) =>
  [
    ...API_KEY_USAGE_V2_PREFIX,
    policy.key_id,
    policy.policy_version.startsWith("v3:") ? policy.policy_version.slice(3) : policy.policy_version,
    policy.window_start_ms,
  ] as const;

export const apiKeyUsageV3WindowKey = (
  policy: Pick<ApiKeyPolicy, "key_id" | "policy_version" | "window_start_ms">,
) => [...API_KEY_USAGE_V3_WINDOW_PREFIX, policy.key_id, policy.policy_version, policy.window_start_ms] as const;

export const apiKeyUsageV3RequestKey = (
  policy: Pick<ApiKeyPolicy, "key_id" | "policy_version" | "window_start_ms">,
  requestId: string,
) =>
  [
    ...API_KEY_USAGE_V3_REQUEST_PREFIX,
    policy.key_id,
    policy.policy_version,
    policy.window_start_ms,
    requestId,
  ] as const;

export const apiKeyUsageV3ExpiryKey = (
  policy: Pick<ApiKeyPolicy, "key_id" | "policy_version" | "window_start_ms">,
  leaseExpiresAtMs: number,
  requestId: string,
) => [...API_KEY_USAGE_V3_EXPIRY_PREFIX, policy.key_id, policy.policy_version, policy.window_start_ms, leaseExpiresAtMs, requestId] as const;

export const apiKeyUsageV3RetentionMs = (windowResetAtMs: number, nowMs = Date.now()): number =>
  Math.max(1, windowResetAtMs + API_KEY_USAGE_V3_RETENTION_MS - nowMs);

export const makeApiKeyUsageWindowV3 = (
  policy: Pick<ApiKeyPolicy, "key_id" | "policy_version" | "window_start_ms" | "usage_reset_at_ms">,
  nowMs = Date.now(),
): ApiKeyUsageWindowV3 => ({
  v: 3,
  key_id: policy.key_id,
  policy_version: policy.policy_version,
  window_start_ms: policy.window_start_ms,
  window_reset_at_ms: policy.usage_reset_at_ms,
  committed_requests: 0,
  reserved_requests: 0,
  updated_at_ms: nowMs,
});

const isSafeNonNegativeInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

export const normalizeApiKeyUsageWindowV3 = (value: unknown): ApiKeyUsageWindowV3 | null => {
  if (!isRecord(value)) return null;
  if (
    value.v !== 3 || typeof value.key_id !== "string" || !value.key_id || typeof value.policy_version !== "string" ||
    !value.policy_version || !isSafeNonNegativeInteger(value.window_start_ms) ||
    !isSafeNonNegativeInteger(value.window_reset_at_ms) || value.window_reset_at_ms <= value.window_start_ms ||
    !isSafeNonNegativeInteger(value.committed_requests) || !isSafeNonNegativeInteger(value.reserved_requests) ||
    !isSafeNonNegativeInteger(value.updated_at_ms)
  ) return null;
  return value as ApiKeyUsageWindowV3;
};

export const normalizeApiKeyUsageRequestV3 = (value: unknown): ApiKeyUsageRequestV3 | null => {
  if (!isRecord(value)) return null;
  if (
    value.v !== 3 || typeof value.key_id !== "string" || !value.key_id || typeof value.request_id !== "string" ||
    !value.request_id || typeof value.route !== "string" || !value.route ||
    (value.state !== "reserved" && value.state !== "dispatched" && value.state !== "released") ||
    !isSafeNonNegativeInteger(value.reserved_at_ms) || !isSafeNonNegativeInteger(value.lease_expires_at_ms) ||
    !(value.provider === null || value.provider === "cerebras" || value.provider === "chatgpt_codex" ||
      value.provider === "removed_provider" || value.provider === "metered" || value.provider === "surplus" ||
      value.provider === "voyage") ||
    !(value.dispatched_at_ms === null || isSafeNonNegativeInteger(value.dispatched_at_ms)) ||
    !(value.released_at_ms === null || isSafeNonNegativeInteger(value.released_at_ms)) ||
    !(value.release_reason === null || typeof value.release_reason === "string")
  ) return null;
  if (
    (value.state === "reserved" &&
      (value.provider !== null || value.dispatched_at_ms !== null || value.released_at_ms !== null ||
        value.release_reason !== null)) ||
    (value.state === "dispatched" &&
      (value.provider === null || value.dispatched_at_ms === null || value.released_at_ms !== null ||
        value.release_reason !== null)) ||
    (value.state === "released" &&
      (value.provider !== null || value.dispatched_at_ms !== null || value.released_at_ms === null ||
        value.release_reason === null))
  ) return null;
  return value as ApiKeyUsageRequestV3;
};

const quotaUnavailable = (
  message = "API key quota ledger is unavailable",
  retryAfterSeconds = 1,
): ApiKeyUsageReservationDecision => ({
  ok: false,
  response: openaiError(503, message, "server_error", {
    type: "server_error",
    headers: { "Retry-After": String(Math.max(1, Math.trunc(retryAfterSeconds))) },
  }),
});

const expiredPolicyResponse = (): ApiKeyUsageReservationDecision => ({
  ok: false,
  response: openaiError(401, "Unauthorized", "invalid_api_key"),
});

export const apiKeyRateLimitPolicyHeaders = (policy: ApiKeyPolicy | null): Record<string, string> => {
  if (!policy || policy.usage_limit_requests === API_KEY_NO_USAGE_LIMIT) return {};
  return {
    "RateLimit-Policy": `"api-key";q=${policy.usage_limit_requests};w=${
      Math.max(1, Math.ceil(policy.window_ms / 1000))
    }`,
  };
};

const rateLimitExceededHeaders = (window: ApiKeyUsageWindowV3, policy: ApiKeyPolicy): Record<string, string> => {
  const retryAfterSeconds = Math.max(1, Math.ceil(Math.max(0, window.window_reset_at_ms - Date.now()) / 1000));
  const remaining = Math.max(0, policy.usage_limit_requests - window.committed_requests - window.reserved_requests);
  const windowSeconds = Math.max(1, Math.ceil((window.window_reset_at_ms - window.window_start_ms) / 1000));
  return {
    "Retry-After": String(retryAfterSeconds),
    "RateLimit": `"api-key";r=${remaining};t=${retryAfterSeconds}`,
    "RateLimit-Policy": `"api-key";q=${policy.usage_limit_requests};w=${windowSeconds}`,
    // Older API clients still look for this de facto field family. Keep it in
    // addition to the active HTTPAPI RateLimit Internet-Draft fields.
    "RateLimit-Limit": String(policy.usage_limit_requests),
    "RateLimit-Remaining": String(remaining),
    "RateLimit-Reset": String(retryAfterSeconds),
  };
};

const quotaExceededResponse = (window: ApiKeyUsageWindowV3, policy: ApiKeyPolicy): ApiKeyUsageReservationDecision => {
  return {
    ok: false,
    response: openaiError(
      429,
      `Usage limit exceeded (${window.committed_requests}/${policy.usage_limit_requests}). Resets at ${
        new Date(window.window_reset_at_ms).toISOString()
      }`,
      "rate_limit_exceeded",
      { type: "rate_limit_error", headers: rateLimitExceededHeaders(window, policy) },
    ),
  };
};

const livePolicyFromEntry = (
  tokenHash: string,
  entry: Deno.KvEntryMaybe<ApiKeyHashRecord>,
  nowMs: number,
): ApiKeyPolicy | null => {
  if (!entry.value || entry.value.revoked_at_ms !== null) return null;
  const policy = apiKeyPolicyFromHashRecord(tokenHash, entry.value, nowMs);
  if (!policy) return null;
  if (policy.expires_at_ms !== API_KEY_NO_EXPIRATION_MS && policy.expires_at_ms <= nowMs) return null;
  return policy;
};

const matchingWindow = (
  value: unknown,
  policy: ApiKeyPolicy,
): ApiKeyUsageWindowV3 | null => {
  const window = normalizeApiKeyUsageWindowV3(value);
  if (!window) return null;
  return window.key_id === policy.key_id && window.policy_version === policy.policy_version &&
      window.window_start_ms === policy.window_start_ms && window.window_reset_at_ms === policy.usage_reset_at_ms
    ? window
    : null;
};

/**
 * A same-isolate contention optimization for the aggregate mutations below.
 *
 * This never replaces the Deno KV checks in those mutations: another isolate
 * (or a deployment replacement) does not share this map, so KV CAS remains
 * the correctness boundary. Keeping the queue scoped to a window lets a burst
 * of local requests make forward progress instead of repeatedly colliding on
 * the same aggregate until MAX_KV_RETRIES is exhausted.
 */
const apiKeyUsageWindowLocks = new Map<string, Promise<void>>();

const apiKeyUsageWindowLockKey = (
  policy: Pick<ApiKeyPolicy, "key_id" | "policy_version" | "window_start_ms">,
): string => JSON.stringify(apiKeyUsageV3WindowKey(policy));

const withApiKeyUsageWindowLock = async <T>(
  policy: Pick<ApiKeyPolicy, "key_id" | "policy_version" | "window_start_ms">,
  operation: () => Promise<T>,
): Promise<T> => {
  const lockKey = apiKeyUsageWindowLockKey(policy);
  const previous = apiKeyUsageWindowLocks.get(lockKey);
  let release!: () => void;
  const current = new Promise<void>((resolve) => {
    release = resolve;
  });
  apiKeyUsageWindowLocks.set(lockKey, current);
  if (previous) await previous;

  try {
    return await operation();
  } finally {
    release();
    if (apiKeyUsageWindowLocks.get(lockKey) === current) apiKeyUsageWindowLocks.delete(lockKey);
  }
};

const releaseReservedRequest = async (
  kv: Deno.Kv,
  policy: ApiKeyPolicy,
  requestId: string,
  reason: string,
  nowMs: number,
  expectedRoute?: string,
): Promise<"released" | "settled" | "missing" | "invalid" | "conflict"> => {
  const requestKey = apiKeyUsageV3RequestKey(policy, requestId);
  const windowKey = apiKeyUsageV3WindowKey(policy);
  const [requestEntry, windowEntry] = await Promise.all([
    kv.get<ApiKeyUsageRequestV3>(requestKey, { consistency: "strong" }),
    kv.get<ApiKeyUsageWindowV3>(windowKey, { consistency: "strong" }),
  ]);
  const request = normalizeApiKeyUsageRequestV3(requestEntry.value);
  if (!request) return requestEntry.value === null ? "missing" : "invalid";
  if (
    request.key_id !== policy.key_id || request.request_id !== requestId ||
    (expectedRoute !== undefined && request.route !== expectedRoute)
  ) return "invalid";
  if (request.state !== "reserved") return "settled";
  const window = matchingWindow(windowEntry.value, policy);
  if (!window || window.reserved_requests < 1) return "invalid";
  const released: ApiKeyUsageRequestV3 = {
    ...request,
    state: "released",
    released_at_ms: nowMs,
    release_reason: reason,
  };
  const updatedWindow: ApiKeyUsageWindowV3 = {
    ...window,
    reserved_requests: window.reserved_requests - 1,
    updated_at_ms: nowMs,
  };
  const committed = await kv.atomic()
    .check(requestEntry)
    .check(windowEntry)
    .set(requestKey, released, { expireIn: apiKeyUsageV3RetentionMs(window.window_reset_at_ms, nowMs) })
    .set(windowKey, updatedWindow, { expireIn: apiKeyUsageV3RetentionMs(window.window_reset_at_ms, nowMs) })
    .commit();
  return committed.ok ? "released" : "conflict";
};

const releaseDispatchedRequest = async (
  kv: Deno.Kv,
  policy: ApiKeyPolicy,
  requestId: string,
  route: string,
  provider: ApiKeyUsageProvider,
  nowMs: number,
): Promise<"released" | "settled" | "missing" | "invalid" | "conflict"> => {
  const requestKey = apiKeyUsageV3RequestKey(policy, requestId);
  const windowKey = apiKeyUsageV3WindowKey(policy);
  const [requestEntry, windowEntry] = await Promise.all([
    kv.get<ApiKeyUsageRequestV3>(requestKey, { consistency: "strong" }),
    kv.get<ApiKeyUsageWindowV3>(windowKey, { consistency: "strong" }),
  ]);
  const request = normalizeApiKeyUsageRequestV3(requestEntry.value);
  if (!request) return requestEntry.value === null ? "missing" : "invalid";
  if (request.key_id !== policy.key_id || request.request_id !== requestId || request.route !== route) return "invalid";
  if (request.state !== "dispatched") return "settled";
  if (request.provider !== provider) return "invalid";
  const window = matchingWindow(windowEntry.value, policy);
  if (!window || window.committed_requests < 1) return "invalid";
  const released: ApiKeyUsageRequestV3 = {
    ...request,
    state: "released",
    provider: null,
    dispatched_at_ms: null,
    released_at_ms: nowMs,
    release_reason: "transport_cancelled_before_fetch",
  };
  const updatedWindow: ApiKeyUsageWindowV3 = {
    ...window,
    committed_requests: window.committed_requests - 1,
    updated_at_ms: nowMs,
  };
  const committed = await kv.atomic()
    .check(requestEntry)
    .check(windowEntry)
    .set(requestKey, released, { expireIn: apiKeyUsageV3RetentionMs(window.window_reset_at_ms, nowMs) })
    .set(windowKey, updatedWindow, { expireIn: apiKeyUsageV3RetentionMs(window.window_reset_at_ms, nowMs) })
    .commit();
  return committed.ok ? "released" : "conflict";
};

const providerDispatchContext = (
  kv: Deno.Kv,
  policy: ApiKeyPolicy,
  requestId: string,
  route: string,
  provider: ApiKeyUsageProvider,
): ApiKeyProviderDispatch => {
  let transportStarted = false;
  let cancellation: Promise<void> | null = null;
  return {
    markTransportStarted: () => {
      transportStarted = true;
    },
    cancelBeforeTransport: async () => {
      if (transportStarted) return;
      if (!cancellation) {
        cancellation = (async () => {
          try {
            await withApiKeyUsageWindowLock(policy, async () => {
              for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
                const outcome = await releaseDispatchedRequest(
                  kv,
                  policy,
                  requestId,
                  route,
                  provider,
                  Date.now(),
                );
                if (outcome === "released" || outcome === "settled" || outcome === "missing") return;
                if (outcome === "invalid") {
                  throw new ApiKeyQuotaDispatchError("API key quota reservation is malformed");
                }
              }
              throw new ApiKeyQuotaDispatchError("API key quota reservation changed concurrently");
            });
          } catch (error) {
            if (error instanceof ApiKeyQuotaDispatchError) throw error;
            console.warn("[ai.ubq.fi] Failed to compensate API key quota dispatch:", error);
            throw new ApiKeyQuotaDispatchError("API key quota reservation is unavailable");
          }
        })();
      }
      await cancellation;
    },
  };
};

/**
 * Expired leases are released before admission. This is intentionally a
 * separate, retryable pass: Deno KV cannot list a dynamic set of request keys
 * inside the aggregate's atomic check.
 */
const reclaimExpiredApiKeyUsageReservationsV3Unlocked = async (
  kv: Deno.Kv,
  policy: ApiKeyPolicy,
  nowMs = Date.now(),
): Promise<void> => {
  const prefix = [...API_KEY_USAGE_V3_REQUEST_PREFIX, policy.key_id, policy.policy_version, policy.window_start_ms];
  const expired: string[] = [];
  for await (const entry of kv.list<ApiKeyUsageRequestV3>({ prefix })) {
    const request = normalizeApiKeyUsageRequestV3(entry.value);
    const requestId = entry.key.at(-1);
    if (
      !request || typeof requestId !== "string" || request.key_id !== policy.key_id ||
      request.request_id !== requestId
    ) {
      throw new Error("API key quota request is malformed");
    }
    if (request.state === "reserved" && request.lease_expires_at_ms <= nowMs) expired.push(request.request_id);
  }
  for (const requestId of expired) {
    let settled = false;
    for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
      const outcome = await releaseReservedRequest(kv, policy, requestId, "lease_expired", nowMs);
      if (outcome === "conflict") continue;
      if (outcome === "invalid") throw new Error("API key quota request is malformed");
      settled = true;
      break;
    }
    if (!settled) throw new Error("API key quota lease changed concurrently");
  }
};

export const reclaimExpiredApiKeyUsageReservationsV3 = async (
  kv: Deno.Kv,
  policy: ApiKeyPolicy,
  nowMs = Date.now(),
): Promise<void> => {
  await withApiKeyUsageWindowLock(
    policy,
    () => reclaimExpiredApiKeyUsageReservationsV3Unlocked(kv, policy, nowMs),
  );
};

const reservationContext = (
  kv: Deno.Kv,
  policy: ApiKeyPolicy,
  requestId: string,
  route: string,
): ApiKeyUsageReservation => {
  // `release()` runs after every route. Once this exact reservation has
  // durably moved from reserved to dispatched, it cannot release a
  // reservation: the request row is no longer reserved. Remembering that
  // local transition avoids rereading the request and aggregate merely to
  // rediscover the already-settled state. This is deliberately not shared
  // across requests or isolates; KV remains the authority before dispatch.
  let dispatchedByThisReservation = false;
  return {
    policy,
    request_id: requestId,
    route,
    beforeProviderDispatch: async (provider: ApiKeyUsageProvider): Promise<ApiKeyProviderDispatch | void> => {
      let dispatchedHere = false;
      try {
        await withApiKeyUsageWindowLock(policy, async () => {
          const requestKey = apiKeyUsageV3RequestKey(policy, requestId);
          const windowKey = apiKeyUsageV3WindowKey(policy);
          for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
            const [requestEntry, windowEntry] = await Promise.all([
              kv.get<ApiKeyUsageRequestV3>(requestKey, { consistency: "strong" }),
              kv.get<ApiKeyUsageWindowV3>(windowKey, { consistency: "strong" }),
            ]);
            const request = normalizeApiKeyUsageRequestV3(requestEntry.value);
            if (
              !request || request.key_id !== policy.key_id || request.request_id !== requestId ||
              request.route !== route ||
              request.state === "released"
            ) throw new ApiKeyQuotaDispatchError();
            if (request.state === "dispatched") return;
            const window = matchingWindow(windowEntry.value, policy);
            if (!window || window.reserved_requests < 1) throw new ApiKeyQuotaDispatchError();
            const nowMs = Date.now();
            const dispatched: ApiKeyUsageRequestV3 = {
              ...request,
              state: "dispatched",
              provider,
              dispatched_at_ms: nowMs,
            };
            const committedWindow: ApiKeyUsageWindowV3 = {
              ...window,
              committed_requests: window.committed_requests + 1,
              reserved_requests: window.reserved_requests - 1,
              updated_at_ms: nowMs,
            };
            const committed = await kv.atomic()
              .check(requestEntry)
              .check(windowEntry)
              .set(requestKey, dispatched, { expireIn: apiKeyUsageV3RetentionMs(window.window_reset_at_ms, nowMs) })
              .set(windowKey, committedWindow, { expireIn: apiKeyUsageV3RetentionMs(window.window_reset_at_ms, nowMs) })
              .commit();
            if (committed.ok) {
              dispatchedHere = true;
              dispatchedByThisReservation = true;
              return;
            }
          }
          throw new ApiKeyQuotaDispatchError("API key quota reservation changed concurrently");
        });
      } catch (error) {
        if (error instanceof ApiKeyQuotaDispatchError) throw error;
        console.warn("[ai.ubq.fi] Failed to commit API key quota dispatch:", error);
        throw new ApiKeyQuotaDispatchError("API key quota reservation is unavailable");
      }
      return dispatchedHere ? providerDispatchContext(kv, policy, requestId, route, provider) : undefined;
    },
    release: async (reason = "route_completed_without_provider_dispatch"): Promise<void> => {
      if (dispatchedByThisReservation) return;
      try {
        await withApiKeyUsageWindowLock(policy, async () => {
          for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
            const outcome = await releaseReservedRequest(kv, policy, requestId, reason, Date.now(), route);
            if (outcome === "released" || outcome === "settled" || outcome === "missing") return;
            if (outcome === "invalid") throw new ApiKeyQuotaDispatchError("API key quota reservation is malformed");
          }
          throw new ApiKeyQuotaDispatchError("API key quota reservation changed concurrently");
        });
      } catch (error) {
        if (error instanceof ApiKeyQuotaDispatchError) throw error;
        console.warn("[ai.ubq.fi] Failed to release API key quota reservation:", error);
        throw new ApiKeyQuotaDispatchError("API key quota reservation is unavailable");
      }
    },
  };
};

const quotaDispatchErrorFromResponse = async (response: Response): Promise<ApiKeyQuotaDispatchError> => {
  const payload = await response.clone().json().catch(() => null) as unknown;
  const error = isRecord(payload) && isRecord(payload.error) ? payload.error : null;
  const message = error && typeof error.message === "string"
    ? error.message
    : "API key quota reservation is no longer available";
  const headers: Record<string, string> = {};
  for (const name of ["Retry-After", ...STANDARD_RATE_LIMIT_HEADERS]) {
    const value = response.headers.get(name);
    if (value) headers[name] = value;
  }
  return new ApiKeyQuotaDispatchError(message, {
    status: response.status,
    code: error && typeof error.code === "string"
      ? error.code
      : response.status === 429
      ? "rate_limit_exceeded"
      : "api_key_quota_reservation_unavailable",
    errorType: error && typeof error.type === "string"
      ? error.type
      : response.status === 429
      ? "rate_limit_error"
      : "server_error",
    retryAfter: response.headers.get("Retry-After"),
    headers,
  });
};

const deferredReservationContext = (
  kv: Deno.Kv,
  policy: ApiKeyPolicy,
  requestId: string,
  route: string,
): ApiKeyUsageReservation => {
  let active: ApiKeyUsageReservation | null = null;
  let admission: Promise<ApiKeyUsageReservation> | null = null;
  const requireAdmission = async (): Promise<ApiKeyUsageReservation> => {
    if (active) return active;
    if (!admission) {
      admission = (async () => {
        const decision = await reserveApiKeyUsageV3(policy, requestId, route, { kv });
        if (!decision.ok) throw await quotaDispatchErrorFromResponse(decision.response);
        active = decision.reservation;
        return active;
      })();
    }
    return await admission;
  };
  return {
    policy,
    request_id: requestId,
    route,
    beforeProviderDispatch: async (provider) => (await requireAdmission()).beforeProviderDispatch(provider),
    release: async (reason) => {
      if (active) await active.release(reason);
    },
  };
};

/**
 * Auth only proves that a key is valid. Route admission is a separate V3
 * reservation so non-inference endpoints never consume or block on quota.
 */
export const reserveApiKeyUsageV3 = async (
  policy: ApiKeyPolicy,
  requestId: string,
  route: string,
  options: Readonly<{ kv?: Deno.Kv | null; nowMs?: number; deferWhenFull?: boolean }> = {},
): Promise<ApiKeyUsageReservationDecision> => {
  try {
    const kv = options.kv === undefined ? await getKv() : options.kv;
    if (!kv) return quotaUnavailable();
    if (!requestId || !route) return quotaUnavailable("API key quota reservation requires a request id and route");

    return await withApiKeyUsageWindowLock(policy, async () => {
      let reclaimedExpiredReservations = false;
      for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
        const nowMs = options.nowMs ?? Date.now();
        const policyEntry = await kv.get<ApiKeyHashRecord>(apiKeyHashKey(policy.token_hash), { consistency: "strong" });
        const livePolicy = livePolicyFromEntry(policy.token_hash, policyEntry, nowMs);
        if (!livePolicy) {
          if (
            policyEntry.value?.revoked_at_ms !== null ||
            (typeof policyEntry.value?.expires_at_ms === "number" && policyEntry.value.expires_at_ms <= nowMs)
          ) {
            return expiredPolicyResponse();
          }
          return quotaUnavailable("API key quota policy is incomplete");
        }

        const windowKey = apiKeyUsageV3WindowKey(livePolicy);
        const requestKey = apiKeyUsageV3RequestKey(livePolicy, requestId);
        const [windowEntry, requestEntry] = await Promise.all([
          kv.get<ApiKeyUsageWindowV3>(windowKey, { consistency: "strong" }),
          kv.get<ApiKeyUsageRequestV3>(requestKey, { consistency: "strong" }),
        ]);
        const existingRequest = normalizeApiKeyUsageRequestV3(requestEntry.value);
        if (requestEntry.value !== null && !existingRequest) {
          return quotaUnavailable("API key quota request is malformed");
        }
        if (existingRequest) {
          if (
            existingRequest.key_id !== livePolicy.key_id || existingRequest.request_id !== requestId ||
            existingRequest.route !== route
          ) return quotaUnavailable("API key quota request identity conflicts");
          return { ok: true, reservation: reservationContext(kv, livePolicy, requestId, route) };
        }

        const window = windowEntry.value === null
          ? makeApiKeyUsageWindowV3(livePolicy, nowMs)
          : matchingWindow(windowEntry.value, livePolicy);
        if (!window) return quotaUnavailable("API key quota aggregate is malformed");
        if (
          livePolicy.usage_limit_requests !== API_KEY_NO_USAGE_LIMIT &&
          window.committed_requests + window.reserved_requests >= livePolicy.usage_limit_requests
        ) {
          if (reclaimedExpiredReservations) {
            if (options.deferWhenFull) {
              return { ok: true, reservation: deferredReservationContext(kv, livePolicy, requestId, route) };
            }
            return quotaExceededResponse(window, livePolicy);
          }
          try {
            // Unlimited policies never need this list. Bounded policies scan
            // only after the aggregate appears full, so an expired lease is
            // always reclaimed before we return the capacity 429.
            await reclaimExpiredApiKeyUsageReservationsV3Unlocked(kv, livePolicy, nowMs);
          } catch (error) {
            console.warn("[ai.ubq.fi] Failed to reclaim API key quota leases:", error);
            return quotaUnavailable();
          }
          reclaimedExpiredReservations = true;
          continue;
        }

        const request: ApiKeyUsageRequestV3 = {
          v: 3,
          key_id: livePolicy.key_id,
          request_id: requestId,
          route,
          state: "reserved",
          reserved_at_ms: nowMs,
          lease_expires_at_ms: nowMs + API_KEY_USAGE_V3_RESERVATION_LEASE_MS,
          provider: null,
          dispatched_at_ms: null,
          released_at_ms: null,
          release_reason: null,
        };
        const reservedWindow: ApiKeyUsageWindowV3 = {
          ...window,
          reserved_requests: window.reserved_requests + 1,
          updated_at_ms: nowMs,
        };
        const committed = await kv.atomic()
          .check(policyEntry)
          .check(windowEntry)
          .check(requestEntry)
          .set(windowKey, reservedWindow, {
            expireIn: apiKeyUsageV3RetentionMs(reservedWindow.window_reset_at_ms, nowMs),
          })
          .set(requestKey, request, { expireIn: apiKeyUsageV3RetentionMs(reservedWindow.window_reset_at_ms, nowMs) })
          .commit();
        if (committed.ok) return { ok: true, reservation: reservationContext(kv, livePolicy, requestId, route) };
      }
      return quotaUnavailable("API key quota ledger changed concurrently");
    });
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to reserve API key quota:", error);
    return quotaUnavailable();
  }
};

export const getApiKeyUsageV3 = async (
  policy: ApiKeyPolicy,
  kvOverride?: Deno.Kv | null,
): Promise<number> => {
  const kv = kvOverride === undefined ? await getKv() : kvOverride;
  if (!kv) return 0;
  const entry = await kv.get<ApiKeyUsageWindowV3>(apiKeyUsageV3WindowKey(policy), { consistency: "strong" });
  return matchingWindow(entry.value, policy)?.committed_requests ?? 0;
};

export const initializeApiKeyUsageWindowV3 = async (
  kv: Deno.Kv,
  policy: ApiKeyPolicy,
  options: Readonly<{ overwrite?: boolean; nowMs?: number }> = {},
): Promise<boolean> => {
  const nowMs = options.nowMs ?? Date.now();
  const key = apiKeyUsageV3WindowKey(policy);
  const entry = await kv.get<ApiKeyUsageWindowV3>(key, { consistency: "strong" });
  if (entry.value !== null && !options.overwrite) return Boolean(matchingWindow(entry.value, policy));
  const window = makeApiKeyUsageWindowV3(policy, nowMs);
  const committed = await kv.atomic()
    .check(entry)
    .set(key, window, { expireIn: apiKeyUsageV3RetentionMs(window.window_reset_at_ms, nowMs) })
    .commit();
  return committed.ok;
};

export const hasLiveApiKeyUsageReservationsV3 = async (
  kv: Deno.Kv,
  keyId: string,
  nowMs = Date.now(),
): Promise<boolean> => {
  for await (const entry of kv.list<ApiKeyUsageRequestV3>({ prefix: [...API_KEY_USAGE_V3_REQUEST_PREFIX, keyId] })) {
    const request = normalizeApiKeyUsageRequestV3(entry.value);
    if (request?.state === "reserved" && request.lease_expires_at_ms > nowMs) return true;
  }
  return false;
};

export const reclaimApiKeyUsageReservationsForKeyV3 = async (
  kv: Deno.Kv,
  keyId: string,
  nowMs = Date.now(),
): Promise<void> => {
  const policies = new Map<string, ApiKeyPolicy>();
  for await (const entry of kv.list<ApiKeyUsageRequestV3>({ prefix: [...API_KEY_USAGE_V3_REQUEST_PREFIX, keyId] })) {
    const request = normalizeApiKeyUsageRequestV3(entry.value);
    if (!request || request.state !== "reserved" || request.lease_expires_at_ms > nowMs) continue;
    const key = entry.key;
    const policyVersionValue = key[API_KEY_USAGE_V3_REQUEST_PREFIX.length + 1];
    const windowStartValue = key[API_KEY_USAGE_V3_REQUEST_PREFIX.length + 2];
    if (typeof policyVersionValue !== "string" || !isSafeNonNegativeInteger(windowStartValue)) continue;
    // Only fields used by releaseReservedRequest are required here.
    const policy: ApiKeyPolicy = {
      token_hash: "",
      key_id: keyId,
      expires_at_ms: API_KEY_NO_EXPIRATION_MS,
      usage_limit_requests: API_KEY_NO_USAGE_LIMIT,
      window_ms: 1,
      window_start_ms: windowStartValue,
      usage_reset_at_ms: 0,
      policy_version: policyVersionValue,
      paid_fallback_enabled: false,
      paid_fallback_limit_microcredits: 0,
      paid_fallback_spent_microcredits: 0,
      paid_fallback_reserved_microcredits: 0,
      paid_fallback_reservation_request_id: null,
    };
    const windowEntry = await kv.get<ApiKeyUsageWindowV3>(apiKeyUsageV3WindowKey(policy), { consistency: "strong" });
    const window = normalizeApiKeyUsageWindowV3(windowEntry.value);
    if (!window) continue;
    policies.set(`${policy.policy_version}:${policy.window_start_ms}`, {
      ...policy,
      usage_reset_at_ms: window.window_reset_at_ms,
    });
  }
  for (const policy of policies.values()) await reclaimExpiredApiKeyUsageReservationsV3(kv, policy, nowMs);
};

export const deleteApiKeyUsageV3 = async (kv: Deno.Kv, keyId: string): Promise<void> => {
  const keys: Deno.KvKey[] = [];
  for await (const entry of kv.list({ prefix: [...API_KEY_USAGE_V3_WINDOW_PREFIX, keyId] })) keys.push(entry.key);
  for await (const entry of kv.list({ prefix: [...API_KEY_USAGE_V3_REQUEST_PREFIX, keyId] })) keys.push(entry.key);
  for (const key of keys) await kv.delete(key);
};

export const authenticateApiKeyToken = async (
  token: string,
  options: Readonly<{ kv?: Deno.Kv | null; nowMs?: number }> = {},
): Promise<ApiKeyPolicyDecision> => {
  if (!looksLikeUosApiKey(token)) {
    return { ok: false, response: openaiError(401, "Unauthorized", "invalid_api_key") };
  }
  const nowMs = options.nowMs ?? Date.now();
  const tokenHash = await sha256Base64Url(token);
  const kv = options.kv === undefined ? await getKv() : options.kv;
  if (!kv) return { ok: false, response: openaiError(401, "Unauthorized", "invalid_api_key") };

  let policy = policyCache.get(tokenHash)?.expires_at_ms! > nowMs ? policyCache.get(tokenHash)!.policy : null;
  if (!policy) {
    policyCache.delete(tokenHash);
    const entry = await kv.get<ApiKeyHashRecord>(apiKeyHashKey(tokenHash), { consistency: "strong" });
    if (!entry.value || entry.value.revoked_at_ms !== null) {
      return { ok: false, response: openaiError(401, "Unauthorized", "invalid_api_key") };
    }
    policy = apiKeyPolicyFromHashRecord(tokenHash, entry.value, nowMs);
    if (!policy) {
      return {
        ok: false,
        response: openaiError(503, "API key quota migration is incomplete", "server_error", { type: "server_error" }),
      };
    }
    policyCache.set(tokenHash, { policy, expires_at_ms: nowMs + API_KEY_POLICY_CACHE_TTL_MS });
  } else if (nowMs >= policy.usage_reset_at_ms) {
    const window = currentApiKeyUsageWindow(policy.usage_reset_at_ms, policy.window_ms, nowMs);
    policy = { ...policy, window_start_ms: window.start, usage_reset_at_ms: window.reset };
    policyCache.set(tokenHash, { policy, expires_at_ms: nowMs + API_KEY_POLICY_CACHE_TTL_MS });
  }

  if (policy.expires_at_ms !== API_KEY_NO_EXPIRATION_MS && policy.expires_at_ms <= nowMs) {
    policyCache.delete(tokenHash);
    return { ok: false, response: openaiError(401, "Unauthorized", "invalid_api_key") };
  }
  // Authentication remains quota-free so non-inference endpoints (including
  // /v1/models) never depend on an aggregate read or an inference reservation.
  // /uos/auth performs its own committed-usage projection after auth succeeds.
  return { ok: true, policy };
};

export const invalidateApiKeyPolicy = (keyId?: string): void => {
  if (!keyId) {
    policyCache.clear();
    return;
  }
  for (const [hash, cached] of policyCache) {
    if (cached.policy.key_id === keyId) policyCache.delete(hash);
  }
};

export const resetApiKeyPolicyCacheForTest = (): void => policyCache.clear();

export const apiKeyQuotaUsedPercent = (policy: ApiKeyPolicy | null): number | null => {
  if (!policy?.paid_fallback_enabled || policy.paid_fallback_limit_microcredits <= 0) return null;
  const used = policy.paid_fallback_spent_microcredits + policy.paid_fallback_reserved_microcredits;
  return Math.min(100, Math.max(0, used * 100 / policy.paid_fallback_limit_microcredits));
};
