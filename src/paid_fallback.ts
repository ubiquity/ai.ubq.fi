import {
  API_KEY_REQUEST_LOG_RETENTION_MS,
  apiKeyRequestLogKey,
  apiKeyRequestLogPrefix,
  prepareApiKeyUsageAtomicMutation,
  updateApiKeyRequestLog,
} from "./analytics.ts";
import {
  API_KEY_ID_PREFIX,
  apiKeyHashKey,
  apiKeyIdKey,
  MICROCREDITS_PER_CREDIT,
  PAID_FALLBACK_NO_LIMIT,
} from "./api_keys.ts";
import { loadCodexModelsSnapshot } from "./codex.ts";
import { kvPromise } from "./kv.ts";
import type { ApiKeyHashRecord, ApiKeyRecord, ApiKeyRequestLogRecord } from "./types.ts";
import { getString, isRecord } from "./utils.ts";
import { fetchYunwuTokenLogs, initializeYunwuPricing, readYunwuApiKey, YunwuError } from "./yunwu.ts";

const PAID_FALLBACK_MIGRATION_KEY = ["uos_ai", "migrations", "api_key_paid_fallback_v1"] as const;
const MAX_RESERVATION_RELEASE_RETRIES = 3;

export type PaidFallbackPolicyFields = Pick<
  ApiKeyRecord,
  | "paid_fallback_enabled"
  | "paid_fallback_limit_microcredits"
  | "paid_fallback_spent_microcredits"
  | "paid_fallback_reserved_microcredits"
  | "paid_fallback_reservation_request_id"
  | "paid_fallback_model_ids"
  | "paid_fallback_quota_per_credit"
  | "paid_fallback_pricing_checked_at_ms"
>;

export type PaidFallbackReservation = Readonly<{
  key_id: string;
  request_id: string;
  created_at_ms: number;
  reserved_microcredits: number;
  quota_per_credit: number;
  window_reset_at_ms: number;
}>;

export type PaidFallbackReservationDecision =
  | { kind: "reserved"; reservation: PaidFallbackReservation }
  | {
    kind: "skip";
    reason: "disabled" | "provider_unconfigured" | "model_not_priced";
  }
  | {
    kind: "blocked";
    reason: "limit_exceeded" | "reconciliation_pending" | "invalid_policy" | "concurrent_update";
    reset_at_ms: number | null;
  };

export const defaultPaidFallbackPolicy = (): PaidFallbackPolicyFields => ({
  paid_fallback_enabled: false,
  paid_fallback_limit_microcredits: 0,
  paid_fallback_spent_microcredits: 0,
  paid_fallback_reserved_microcredits: 0,
  paid_fallback_reservation_request_id: null,
  paid_fallback_model_ids: [],
  paid_fallback_quota_per_credit: 0,
  paid_fallback_pricing_checked_at_ms: null,
});

const isNonNegativeSafeInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const isPositiveSafeInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value > 0;

const isPaidFallbackLimit = (value: unknown): value is number =>
  value === PAID_FALLBACK_NO_LIMIT || isNonNegativeSafeInteger(value);

const isNullableString = (value: unknown): value is string | null =>
  value === null || (typeof value === "string" && value.trim().length > 0);

export const hasStrictPaidFallbackPolicy = (value: unknown): boolean => {
  if (!isRecord(value)) return false;
  if (typeof value.paid_fallback_enabled !== "boolean") return false;
  if (!isPaidFallbackLimit(value.paid_fallback_limit_microcredits)) return false;
  if (!isNonNegativeSafeInteger(value.paid_fallback_spent_microcredits)) return false;
  if (!isNonNegativeSafeInteger(value.paid_fallback_reserved_microcredits)) return false;
  if (!isNullableString(value.paid_fallback_reservation_request_id)) return false;
  return true;
};

export const hasStrictPaidFallbackKeyPolicy = (value: unknown): boolean => {
  if (!hasStrictPaidFallbackPolicy(value) || !isRecord(value)) return false;
  if (
    !Array.isArray(value.paid_fallback_model_ids) ||
    value.paid_fallback_model_ids.some((model) => typeof model !== "string" || !model.trim())
  ) {
    return false;
  }
  if (!isNonNegativeSafeInteger(value.paid_fallback_quota_per_credit)) return false;
  if (
    value.paid_fallback_pricing_checked_at_ms !== null &&
    !isNonNegativeSafeInteger(value.paid_fallback_pricing_checked_at_ms)
  ) {
    return false;
  }
  return true;
};

export const paidFallbackHashFields = (record: ApiKeyRecord): Pick<
  ApiKeyHashRecord,
  | "paid_fallback_enabled"
  | "paid_fallback_limit_microcredits"
  | "paid_fallback_spent_microcredits"
  | "paid_fallback_reserved_microcredits"
  | "paid_fallback_reservation_request_id"
> => ({
  paid_fallback_enabled: record.paid_fallback_enabled,
  paid_fallback_limit_microcredits: record.paid_fallback_limit_microcredits,
  paid_fallback_spent_microcredits: record.paid_fallback_spent_microcredits,
  paid_fallback_reserved_microcredits: record.paid_fallback_reserved_microcredits,
  paid_fallback_reservation_request_id: record.paid_fallback_reservation_request_id,
});

export const initializePaidFallbackPolicy = async (signal?: AbortSignal): Promise<
  Pick<
    PaidFallbackPolicyFields,
    "paid_fallback_model_ids" | "paid_fallback_quota_per_credit" | "paid_fallback_pricing_checked_at_ms"
  >
> => {
  if (!readYunwuApiKey()) {
    throw new YunwuError(
      "YunWu paid fallback cannot be enabled because YUNWU_API_KEY is not configured.",
      "yunwu_api_key_missing",
      503,
    );
  }

  const snapshot = await loadCodexModelsSnapshot();
  const modelIds: string[] = [];
  const seen = new Set<string>();
  for (const model of snapshot?.models ?? []) {
    const id = getString(model.slug) ?? getString(model.id) ?? getString(model.model) ?? getString(model.name);
    const normalized = id?.trim();
    if (!normalized || seen.has(normalized)) continue;
    seen.add(normalized);
    modelIds.push(normalized);
  }
  if (!modelIds.length) {
    throw new YunwuError(
      "YunWu paid fallback cannot be enabled before the Codex model catalog is initialized.",
      "yunwu_pricing_invalid",
      503,
    );
  }

  const pricing = await initializeYunwuPricing({ codexModelIds: modelIds, signal });
  if (!pricing.eligible_model_ids.length) {
    throw new YunwuError(
      "YunWu paid fallback found no priced models in the current Codex catalog.",
      "yunwu_pricing_invalid",
      503,
    );
  }
  return {
    paid_fallback_model_ids: [...pricing.eligible_model_ids],
    paid_fallback_quota_per_credit: pricing.quota_per_credit,
    paid_fallback_pricing_checked_at_ms: pricing.checked_at_ms,
  };
};

let backfillInFlight: Promise<void> | null = null;
let backfillComplete = false;

const runPaidFallbackBackfill = async (kv: Deno.Kv): Promise<void> => {
  const marker = await kv.get<{ version?: number }>(PAID_FALLBACK_MIGRATION_KEY);
  if (marker.value?.version === 1) return;

  for await (const entry of kv.list<ApiKeyRecord>({ prefix: API_KEY_ID_PREFIX })) {
    if (!entry.value) continue;
    const hashKey = apiKeyHashKey(entry.value.hash);
    const hashEntry = await kv.get<ApiKeyHashRecord>(hashKey);
    if (!hashEntry.value) {
      throw new Error(`API key ${entry.value.id} is missing its hash record`);
    }

    const record = hasStrictPaidFallbackKeyPolicy(entry.value)
      ? entry.value
      : { ...entry.value, ...defaultPaidFallbackPolicy() };
    const hashRecord: ApiKeyHashRecord = {
      ...hashEntry.value,
      ...paidFallbackHashFields(record),
    };
    const commit = await kv.atomic()
      .check(entry)
      .check(hashEntry)
      .set(entry.key, record)
      .set(hashKey, hashRecord)
      .commit();
    if (!commit.ok) {
      throw new Error(`API key ${entry.value.id} changed during paid fallback backfill`);
    }
  }

  await kv.set(PAID_FALLBACK_MIGRATION_KEY, { version: 1, completed_at_ms: Date.now() });
};

export const ensurePaidFallbackBackfill = async (kvOverride?: Deno.Kv | null): Promise<void> => {
  const kv = kvOverride === undefined ? await kvPromise : kvOverride;
  if (!kv) return;
  if (kvOverride !== undefined) {
    await runPaidFallbackBackfill(kv);
    return;
  }
  if (backfillComplete) return;
  backfillInFlight ??= runPaidFallbackBackfill(kv).finally(() => {
    backfillInFlight = null;
  });
  await backfillInFlight;
  backfillComplete = true;
};

const loadStrictKeyPair = async (
  kv: Deno.Kv,
  keyId: string,
): Promise<
  | {
    idKey: Deno.KvKey;
    hashKey: Deno.KvKey;
    idEntry: Deno.KvEntryMaybe<ApiKeyRecord>;
    hashEntry: Deno.KvEntryMaybe<ApiKeyHashRecord>;
    record: ApiKeyRecord;
    hashRecord: ApiKeyHashRecord;
  }
  | null
> => {
  const idKey = apiKeyIdKey(keyId);
  const idEntry = await kv.get<ApiKeyRecord>(idKey);
  if (!idEntry.value || !hasStrictPaidFallbackKeyPolicy(idEntry.value)) return null;
  const hashKey = apiKeyHashKey(idEntry.value.hash);
  const hashEntry = await kv.get<ApiKeyHashRecord>(hashKey);
  if (!hashEntry.value || !hasStrictPaidFallbackPolicy(hashEntry.value)) return null;
  return {
    idKey,
    hashKey,
    idEntry,
    hashEntry,
    record: idEntry.value,
    hashRecord: hashEntry.value,
  };
};

export const reservePaidFallback = async (
  input: Readonly<{
    keyId: string;
    requestId: string;
    createdAtMs: number;
    model: string;
    route: string;
    path: string;
    stream: boolean;
    reasoning: string | null;
    reason: "primary_429" | "primary_rate_limit_cached";
  }>,
): Promise<PaidFallbackReservationDecision> => {
  const kv = await kvPromise;
  if (!kv) return { kind: "blocked", reason: "invalid_policy", reset_at_ms: null };
  const pair = await loadStrictKeyPair(kv, input.keyId);
  if (!pair) return { kind: "blocked", reason: "invalid_policy", reset_at_ms: null };
  const record = pair.record;

  if (!record.paid_fallback_enabled) return { kind: "skip", reason: "disabled" };
  if (!readYunwuApiKey()) return { kind: "skip", reason: "provider_unconfigured" };
  if (!record.paid_fallback_model_ids.includes(input.model)) {
    return { kind: "skip", reason: "model_not_priced" };
  }
  const unlimited = record.paid_fallback_limit_microcredits === PAID_FALLBACK_NO_LIMIT;
  if (
    (!unlimited && !isPositiveSafeInteger(record.paid_fallback_limit_microcredits)) ||
    !isPositiveSafeInteger(record.paid_fallback_quota_per_credit)
  ) {
    return { kind: "blocked", reason: "invalid_policy", reset_at_ms: record.usage_reset_at_ms };
  }
  if (record.paid_fallback_reserved_microcredits > 0 || record.paid_fallback_reservation_request_id) {
    return { kind: "blocked", reason: "reconciliation_pending", reset_at_ms: record.usage_reset_at_ms };
  }

  const remaining = unlimited ? 0 : record.paid_fallback_limit_microcredits - record.paid_fallback_spent_microcredits;
  if (!unlimited && remaining <= 0) {
    return { kind: "blocked", reason: "limit_exceeded", reset_at_ms: record.usage_reset_at_ms };
  }

  const updated: ApiKeyRecord = {
    ...record,
    paid_fallback_reserved_microcredits: remaining,
    paid_fallback_reservation_request_id: input.requestId,
  };
  const updatedHash: ApiKeyHashRecord = {
    ...pair.hashRecord,
    ...paidFallbackHashFields(updated),
  };
  const logKey = apiKeyRequestLogKey(input.keyId, input.createdAtMs, input.requestId);
  const logEntry = await kv.get<ApiKeyRequestLogRecord>(logKey);
  const requestLog: ApiKeyRequestLogRecord = {
    ...(logEntry.value ?? {
      id: input.requestId,
      key_id: input.keyId,
      route: input.route,
      path: input.path,
      method: "POST",
      status_code: 0,
      stream: input.stream,
      model: input.model,
      reasoning: input.reasoning,
      created_at_ms: input.createdAtMs,
      provider: "chatgpt_codex",
      fallback_reason: null,
      provider_request_id: null,
      completed_at_ms: null,
      latency_ms: null,
      input_tokens: null,
      output_tokens: null,
      provider_quota: null,
      quota_per_credit: null,
      spend_microcredits: null,
      paid_fallback_window_reset_at_ms: null,
      billing_status: "not_applicable",
    }),
    provider: "yunwu",
    fallback_reason: input.reason,
    provider_request_id: null,
    quota_per_credit: record.paid_fallback_quota_per_credit,
    paid_fallback_window_reset_at_ms: record.usage_reset_at_ms,
    billing_status: "pending",
  };
  const expireIn = Math.max(
    1,
    input.createdAtMs + API_KEY_REQUEST_LOG_RETENTION_MS - Date.now(),
  );
  const commit = await kv.atomic()
    .check(pair.idEntry)
    .check(pair.hashEntry)
    .check(logEntry)
    .set(pair.idKey, updated)
    .set(pair.hashKey, updatedHash)
    .set(logKey, requestLog, { expireIn })
    .commit();
  if (!commit.ok) {
    return { kind: "blocked", reason: "concurrent_update", reset_at_ms: record.usage_reset_at_ms };
  }

  return {
    kind: "reserved",
    reservation: {
      key_id: input.keyId,
      request_id: input.requestId,
      created_at_ms: input.createdAtMs,
      reserved_microcredits: remaining,
      quota_per_credit: record.paid_fallback_quota_per_credit,
      window_reset_at_ms: record.usage_reset_at_ms,
    },
  };
};

const clearReservation = async (
  reservation: PaidFallbackReservation,
  billingStatus: "not_billed" | "unresolved",
  patch: Readonly<{
    providerRequestId?: string | null;
    statusCode?: number;
    clear: boolean;
  }>,
): Promise<void> => {
  const kv = await kvPromise;
  let cleared = !patch.clear;
  if (kv && patch.clear) {
    for (let attempt = 0; attempt < MAX_RESERVATION_RELEASE_RETRIES; attempt += 1) {
      const pair = await loadStrictKeyPair(kv, reservation.key_id);
      if (!pair) break;
      if (pair.record.paid_fallback_reservation_request_id === null) {
        cleared = true;
        break;
      }
      if (pair.record.paid_fallback_reservation_request_id !== reservation.request_id) break;
      const updated: ApiKeyRecord = {
        ...pair.record,
        paid_fallback_reserved_microcredits: 0,
        paid_fallback_reservation_request_id: null,
      };
      const updatedHash: ApiKeyHashRecord = {
        ...pair.hashRecord,
        ...paidFallbackHashFields(updated),
      };
      const commit = await kv.atomic()
        .check(pair.idEntry)
        .check(pair.hashEntry)
        .set(pair.idKey, updated)
        .set(pair.hashKey, updatedHash)
        .commit();
      if (commit.ok) {
        cleared = true;
        break;
      }
    }
  }
  const resolvedBillingStatus = patch.clear && !cleared ? "unresolved" : billingStatus;
  const completedAtMs = Date.now();
  await updateApiKeyRequestLog(
    reservation.key_id,
    reservation.created_at_ms,
    reservation.request_id,
    {
      provider: "yunwu",
      provider_request_id: patch.providerRequestId,
      status_code: patch.statusCode,
      completed_at_ms: completedAtMs,
      latency_ms: Math.max(0, completedAtMs - reservation.created_at_ms),
      spend_microcredits: resolvedBillingStatus === "not_billed" ? 0 : null,
      billing_status: resolvedBillingStatus,
    },
  );
};

export const recordYunwuUpstreamResponse = async (
  reservation: PaidFallbackReservation,
  response: Response,
  providerRequestId: string | null,
): Promise<void> => {
  if (!response.ok) {
    if (providerRequestId) {
      await updateApiKeyRequestLog(
        reservation.key_id,
        reservation.created_at_ms,
        reservation.request_id,
        {
          provider: "yunwu",
          provider_request_id: providerRequestId,
          status_code: response.status,
          completed_at_ms: Date.now(),
          latency_ms: Math.max(0, Date.now() - reservation.created_at_ms),
          billing_status: "pending",
        },
      );
      return;
    }
    await clearReservation(reservation, "not_billed", {
      providerRequestId: null,
      statusCode: response.status,
      clear: true,
    });
    return;
  }
  if (!providerRequestId) {
    await updateApiKeyRequestLog(
      reservation.key_id,
      reservation.created_at_ms,
      reservation.request_id,
      {
        provider: "yunwu",
        provider_request_id: null,
        status_code: response.status,
        billing_status: "unresolved",
      },
    );
    return;
  }
  await updateApiKeyRequestLog(
    reservation.key_id,
    reservation.created_at_ms,
    reservation.request_id,
    {
      provider: "yunwu",
      provider_request_id: providerRequestId,
      status_code: response.status,
      billing_status: "pending",
    },
  );
};

export const recordYunwuAmbiguousFailure = async (reservation: PaidFallbackReservation): Promise<void> => {
  await clearReservation(reservation, "unresolved", {
    statusCode: 502,
    clear: false,
  });
};

export const recordYunwuUndispatchedCancellation = async (
  reservation: PaidFallbackReservation,
): Promise<void> => {
  await clearReservation(reservation, "not_billed", {
    statusCode: 499,
    clear: true,
  });
};

const settlePaidFallback = async (
  keyId: string,
  request: ApiKeyRequestLogRecord,
  providerLog: Readonly<{
    quota: number;
    prompt_tokens: number;
    completion_tokens: number;
  }>,
): Promise<boolean> => {
  const kv = await kvPromise;
  if (!kv || request.billing_status === "reconciled") return false;
  const quotaPerCredit = request.quota_per_credit;
  if (!isPositiveSafeInteger(quotaPerCredit)) return false;
  const spendMicrocredits = Math.round(providerLog.quota * MICROCREDITS_PER_CREDIT / quotaPerCredit);
  if (!isNonNegativeSafeInteger(spendMicrocredits)) return false;

  const pair = await loadStrictKeyPair(kv, keyId);
  if (!pair) return false;
  const logKey = apiKeyRequestLogKey(keyId, request.created_at_ms, request.id);
  const logEntry = await kv.get<ApiKeyRequestLogRecord>(logKey);
  if (!logEntry.value || logEntry.value.billing_status === "reconciled") return false;

  const ownsReservation = pair.record.paid_fallback_reservation_request_id === request.id;
  const sameWindow = request.paid_fallback_window_reset_at_ms === pair.record.usage_reset_at_ms;
  const updated: ApiKeyRecord = {
    ...pair.record,
    paid_fallback_spent_microcredits: sameWindow
      ? pair.record.paid_fallback_spent_microcredits + spendMicrocredits
      : pair.record.paid_fallback_spent_microcredits,
    paid_fallback_reserved_microcredits: ownsReservation ? 0 : pair.record.paid_fallback_reserved_microcredits,
    paid_fallback_reservation_request_id: ownsReservation ? null : pair.record.paid_fallback_reservation_request_id,
  };
  const updatedHash: ApiKeyHashRecord = {
    ...pair.hashRecord,
    ...paidFallbackHashFields(updated),
  };
  const reconciledAtMs = Date.now();
  const completedAtMs = logEntry.value.completed_at_ms ?? reconciledAtMs;
  const updatedLog: ApiKeyRequestLogRecord = {
    ...logEntry.value,
    completed_at_ms: completedAtMs,
    latency_ms: logEntry.value.latency_ms ?? Math.max(0, completedAtMs - request.created_at_ms),
    input_tokens: providerLog.prompt_tokens,
    output_tokens: providerLog.completion_tokens,
    provider_quota: providerLog.quota,
    spend_microcredits: spendMicrocredits,
    billing_status: "reconciled",
  };
  const usageMutation = await prepareApiKeyUsageAtomicMutation(kv, keyId, {
    yunwu_fallback_requests: 1,
    yunwu_input_tokens: providerLog.prompt_tokens,
    yunwu_output_tokens: providerLog.completion_tokens,
    yunwu_total_tokens: providerLog.prompt_tokens + providerLog.completion_tokens,
    yunwu_spend_microcredits: spendMicrocredits,
    seen_at_ms: request.created_at_ms,
  }, reconciledAtMs);
  const expireIn = Math.max(
    1,
    request.created_at_ms + API_KEY_REQUEST_LOG_RETENTION_MS - reconciledAtMs,
  );
  const atomic = kv.atomic()
    .check(pair.idEntry)
    .check(pair.hashEntry)
    .check(logEntry)
    .check(usageMutation.usage_entry)
    .check(usageMutation.daily_entry)
    .set(pair.idKey, updated)
    .set(pair.hashKey, updatedHash)
    .set(logKey, updatedLog, { expireIn })
    .set(usageMutation.usage_key, usageMutation.usage_record);
  if (usageMutation.daily_record) {
    atomic.set(usageMutation.daily_key, usageMutation.daily_record);
  }
  const commit = await atomic.commit();
  if (!commit.ok) return false;
  return true;
};

export const reconcileApiKeyPaidFallbacks = async (keyId: string): Promise<number> => {
  const kv = await kvPromise;
  if (!kv) return 0;
  const pending: ApiKeyRequestLogRecord[] = [];
  for await (
    const entry of kv.list<ApiKeyRequestLogRecord>(
      { prefix: apiKeyRequestLogPrefix(keyId) },
      { reverse: true },
    )
  ) {
    const request = entry.value;
    if (
      request?.provider === "yunwu" &&
      (request.billing_status === "pending" || request.billing_status === "unresolved") &&
      Boolean(request.provider_request_id)
    ) {
      pending.push(request);
    }
  }
  if (!pending.length) return 0;

  let providerLogs;
  try {
    providerLogs = await fetchYunwuTokenLogs();
  } catch (error) {
    console.warn(
      "[ai.ubq.fi] Failed to reconcile YunWu billing:",
      error instanceof Error ? error.message : String(error),
    );
    return 0;
  }
  const logsByRequestId = new Map(providerLogs.map((entry) => [entry.request_id, entry]));
  let settled = 0;
  for (const request of pending) {
    const providerRequestId = request.provider_request_id;
    if (!providerRequestId) continue;
    const providerLog = logsByRequestId.get(providerRequestId);
    if (!providerLog) continue;
    if (await settlePaidFallback(keyId, request, providerLog)) settled += 1;
  }
  return settled;
};
