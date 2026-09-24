import {
  API_KEY_NO_EXPIRATION_MS,
  API_KEY_NO_USAGE_LIMIT,
  PAID_FALLBACK_NO_LIMIT,
  USAGE_RESET_PERIOD_MS,
  apiKeyHashKey,
  apiKeyIdKey,
  calculateNextResetMs,
  generateApiKeyToken,
} from "../api-keys.ts";
import { type ApiKeyPolicy, apiKeyPolicyFromHashRecord, apiKeyUsageV3RetentionMs, apiKeyUsageV3WindowKey, makeApiKeyUsageWindowV3 } from "../api-key-policy.ts";
import { defaultPaidFallbackPolicy, initializePaidFallbackPolicy, paidFallbackHashFields } from "../paid-fallback/index.ts";
import { readMeteredApiKey } from "../provider/metered.ts";
import { readSurplusApiKey } from "../provider/surplus.ts";
import type { ApiKeyHashRecord, ApiKeyRecord } from "../types.ts";
import { sha256Base64Url } from "../utils.ts";

/**
 * The loopback development principal (`--disable-admin-auth` / the Mac service)
 * authenticates every local request as this one API key. It exists so a local
 * request is a *super-admin* principal with no limits rather than a
 * policy-free one: without a key record the paid-provider admission path has no
 * policy to read, so every model that routes directly to a paid provider is
 * refused with `paid_fallback_disabled`.
 *
 * The record is created only by a local loopback server at boot. Its token is
 * minted, stored, and never printed: nothing authenticates with it as a bearer
 * credential, and the bypass never reads it back.
 */
export const LOCAL_DEVELOPMENT_KEY_ID = "local-development";

/** The single account the loopback development server provisions for itself. */
export const LOCAL_DEVELOPMENT_KEY_NAME = "Local development (loopback)";

export type LocalDevelopmentKeyStatus = "created" | "present" | "revoked" | "unconfigured" | "unavailable" | "conflict";

type LocalDevelopmentKeyMaterial = Readonly<{
  record: ApiKeyRecord;
  hashKey: Deno.KvKey;
  hashRecord: ApiKeyHashRecord;
  policy: ApiKeyPolicy;
}>;

/** The pricing fields an unlimited local key inherits; injectable for tests. */
export type LocalDevelopmentPricingInitializer = (
  signal?: AbortSignal
) => Promise<
  Pick<
    ReturnType<typeof defaultPaidFallbackPolicy>,
    "paid_fallback_model_ids" | "paid_fallback_quota_per_credit" | "paid_fallback_pricing_checked_at_ms" | "paid_fallback_max_exposure_microcredits"
  >
>;

const buildLocalDevelopmentKey = async (
  nowMs: number,
  initializePolicy: LocalDevelopmentPricingInitializer,
  signal?: AbortSignal
): Promise<LocalDevelopmentKeyMaterial> => {
  // Reuse the operator-facing pricing initializer so the local key's paid
  // policy is byte-for-byte the policy an unlimited admin key would receive.
  const policyFields = {
    ...defaultPaidFallbackPolicy(),
    ...(await initializePolicy(signal)),
    paid_fallback_enabled: true,
    paid_fallback_limit_microcredits: PAID_FALLBACK_NO_LIMIT,
  };
  // Minted per provisioning and never exposed: the record's only purpose is to
  // be the principal local requests are admitted as.
  const token = generateApiKeyToken();
  const hash = await sha256Base64Url(token);
  const usageResetAtMs = calculateNextResetMs(nowMs, USAGE_RESET_PERIOD_MS);
  const record: ApiKeyRecord = {
    id: LOCAL_DEVELOPMENT_KEY_ID,
    name: LOCAL_DEVELOPMENT_KEY_NAME,
    prefix: token.slice(0, 12),
    hash,
    created_at_ms: nowMs,
    expires_at_ms: API_KEY_NO_EXPIRATION_MS,
    revoked_at_ms: null,
    usage_limit_requests: API_KEY_NO_USAGE_LIMIT,
    usage_requests: 0,
    usage_reset_at_ms: usageResetAtMs,
    window_ms: USAGE_RESET_PERIOD_MS,
    usage_quota_version: 3,
    ...policyFields,
  };
  const hashRecord: ApiKeyHashRecord = {
    id: LOCAL_DEVELOPMENT_KEY_ID,
    expires_at_ms: API_KEY_NO_EXPIRATION_MS,
    revoked_at_ms: null,
    usage_limit_requests: API_KEY_NO_USAGE_LIMIT,
    usage_requests: 0,
    usage_reset_at_ms: usageResetAtMs,
    window_ms: USAGE_RESET_PERIOD_MS,
    usage_quota_version: 3,
    ...paidFallbackHashFields(record),
  };
  const policy = apiKeyPolicyFromHashRecord(hash, hashRecord, nowMs);
  if (!policy) throw new Error("[ai.ubq.fi] Local development key policy is not strict");
  return { record, hashKey: apiKeyHashKey(hash), hashRecord, policy };
};

/**
 * Creates the loopback development key when it is absent. Idempotent and
 * best-effort: an existing record is never rewritten, so an operator can revoke
 * it to switch local paid routing off, or adjust it in the console. Deleting it
 * re-provisions a fresh unlimited key (with current pricing) at the next local
 * start.
 */
export const ensureLocalDevelopmentApiKey = async (
  kv: Deno.Kv,
  options: Readonly<{ signal?: AbortSignal; initializePolicy?: LocalDevelopmentPricingInitializer }> = {}
): Promise<LocalDevelopmentKeyStatus> => {
  const idKey = apiKeyIdKey(LOCAL_DEVELOPMENT_KEY_ID);
  const existing = await kv.get<ApiKeyRecord>(idKey, { consistency: "strong" });
  if (existing.value) return existing.value.revoked_at_ms === null ? "present" : "revoked";
  if (!readMeteredApiKey() && !readSurplusApiKey()) {
    console.warn("[ai.ubq.fi] Local development key was not provisioned: no paid provider API key is configured.");
    return "unconfigured";
  }

  const nowMs = Date.now();
  let material: LocalDevelopmentKeyMaterial;
  try {
    material = await buildLocalDevelopmentKey(nowMs, options.initializePolicy ?? initializePaidFallbackPolicy, options.signal);
  } catch (error) {
    console.warn(
      "[ai.ubq.fi] Local development key was not provisioned; paid providers stay unavailable locally:",
      error instanceof Error ? error.message : String(error)
    );
    return "unavailable";
  }

  const hashEntry = await kv.get<ApiKeyHashRecord>(material.hashKey);
  const window = makeApiKeyUsageWindowV3(material.policy, nowMs);
  const committed = await kv
    .atomic()
    .check(existing)
    .check(hashEntry)
    .set(idKey, material.record)
    .set(material.hashKey, material.hashRecord)
    .set(apiKeyUsageV3WindowKey(material.policy), window, {
      expireIn: apiKeyUsageV3RetentionMs(window.window_reset_at_ms, nowMs),
    })
    .commit();
  if (!committed.ok) return "conflict";
  return "created";
};

/**
 * The policy every loopback development request is authenticated as, or null
 * when the local development key is absent, revoked, malformed, or expired.
 * A null result degrades the local principal to the policy-free `disabled`
 * method, which is the pre-existing behavior.
 */
export const resolveLocalDevelopmentApiKeyPolicy = async (kv: Deno.Kv | null, nowMs = Date.now()): Promise<ApiKeyPolicy | null> => {
  if (!kv) return null;
  const record = (await kv.get<ApiKeyRecord>(apiKeyIdKey(LOCAL_DEVELOPMENT_KEY_ID), { consistency: "strong" })).value;
  if (record?.revoked_at_ms !== null) return null;
  if (typeof record.hash !== "string" || !record.hash) return null;
  const hashRecord = (await kv.get<ApiKeyHashRecord>(apiKeyHashKey(record.hash), { consistency: "strong" })).value;
  if (hashRecord?.revoked_at_ms !== null) return null;
  const policy = apiKeyPolicyFromHashRecord(record.hash, hashRecord, nowMs);
  if (!policy) return null;
  if (policy.expires_at_ms !== API_KEY_NO_EXPIRATION_MS && policy.expires_at_ms <= nowMs) return null;
  return policy;
};
