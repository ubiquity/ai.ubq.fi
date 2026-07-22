import {
  API_KEY_NO_EXPIRATION_MS,
  API_KEY_NO_USAGE_LIMIT,
  apiKeyHashKey,
  normalizeApiKeyWindowMs,
} from "./api_keys.ts";
import { openaiError } from "./http.ts";
import { kvPromise } from "./kv.ts";
import { hasStrictPaidFallbackPolicy } from "./paid_fallback.ts";
import type { ApiKeyHashRecord } from "./types.ts";
import { sha256Base64Url } from "./utils.ts";

export const API_KEY_POLICY_CACHE_TTL_MS = 30_000;
export const API_KEY_USAGE_V2_PREFIX = ["uos_ai", "api_key_usage", "v2"] as const;

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
  | Readonly<{ ok: true; policy: ApiKeyPolicy; usage_requests: number }>
  | Readonly<{ ok: false; response: Response }>;

type CachedPolicy = Readonly<{ policy: ApiKeyPolicy; expires_at_ms: number }>;

const policyCache = new Map<string, CachedPolicy>();

export const looksLikeUosApiKey = (token: string): boolean => /^u_[0-9a-f]{64}$/.test(token.trim());

const currentWindow = (
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

const policyVersion = (record: ApiKeyHashRecord): string => String(normalizeApiKeyWindowMs(record.window_ms));

export const apiKeyPolicyFromHashRecord = (
  tokenHash: string,
  record: ApiKeyHashRecord,
  nowMs: number,
): ApiKeyPolicy | null => {
  if (!hasStrictPaidFallbackPolicy(record)) return null;
  const windowMs = normalizeApiKeyWindowMs(record.window_ms);
  const window = currentWindow(record.usage_reset_at_ms, windowMs, nowMs);
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

export const apiKeyUsageV2Key = (policy: Pick<ApiKeyPolicy, "key_id" | "policy_version" | "window_start_ms">) =>
  [...API_KEY_USAGE_V2_PREFIX, policy.key_id, policy.policy_version, policy.window_start_ms] as const;

const readCounter = async (kv: Deno.Kv, policy: ApiKeyPolicy): Promise<number> => {
  const entry = await kv.get<Deno.KvU64>(apiKeyUsageV2Key(policy), { consistency: "strong" });
  return entry.value ? Number(entry.value.value) : 0;
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
  const kv = options.kv === undefined ? await kvPromise : options.kv;
  if (!kv) {
    return { ok: false, response: openaiError(401, "Unauthorized", "invalid_api_key") };
  }

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
        response: openaiError(503, "API key migration is incomplete", "server_error", { type: "server_error" }),
      };
    }
    policyCache.set(tokenHash, { policy, expires_at_ms: nowMs + API_KEY_POLICY_CACHE_TTL_MS });
  } else if (nowMs >= policy.usage_reset_at_ms) {
    const window = currentWindow(policy.usage_reset_at_ms, policy.window_ms, nowMs);
    policy = { ...policy, window_start_ms: window.start, usage_reset_at_ms: window.reset };
    policyCache.set(tokenHash, { policy, expires_at_ms: nowMs + API_KEY_POLICY_CACHE_TTL_MS });
  }

  if (policy.expires_at_ms !== API_KEY_NO_EXPIRATION_MS && policy.expires_at_ms <= nowMs) {
    policyCache.delete(tokenHash);
    return { ok: false, response: openaiError(401, "Unauthorized", "invalid_api_key") };
  }
  if (policy.usage_limit_requests === API_KEY_NO_USAGE_LIMIT) {
    return { ok: true, policy, usage_requests: 0 };
  }

  // This is intentionally a strong snapshot admission check followed by one
  // contention-free sum after a successful completion. Strictly reserving
  // concurrent capacity would require a pre-dispatch conditional write/CAS (and
  // rollback on failure), which conflicts with success-only accounting and the
  // one-read/one-sum incident budget.
  const usageRequests = await readCounter(kv, policy);
  if (usageRequests >= policy.usage_limit_requests) {
    return {
      ok: false,
      response: openaiError(
        429,
        `Usage limit exceeded (${usageRequests}/${policy.usage_limit_requests}). Resets at ${
          new Date(policy.usage_reset_at_ms).toISOString()
        }`,
        "rate_limit_exceeded",
      ),
    };
  }
  return { ok: true, policy, usage_requests: usageRequests };
};

export const incrementApiKeyUsageV2 = async (policy: ApiKeyPolicy): Promise<void> => {
  if (policy.usage_limit_requests === API_KEY_NO_USAGE_LIMIT) return;
  const kv = await kvPromise;
  if (!kv) return;
  await kv.atomic().sum(apiKeyUsageV2Key(policy), 1n).commit();
};

export const getApiKeyUsageV2 = async (
  policy: ApiKeyPolicy,
  kvOverride?: Deno.Kv | null,
): Promise<number> => {
  if (policy.usage_limit_requests === API_KEY_NO_USAGE_LIMIT) return 0;
  const kv = kvOverride === undefined ? await kvPromise : kvOverride;
  return kv ? await readCounter(kv, policy) : 0;
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
