import { kvPromise } from "./kv.ts";
import { apiKeyIdKey } from "./api_keys.ts";
import { getString, isRecord } from "./utils.ts";
import type { ApiKeyRequestLogRecord, ApiKeyUsageDailyRecord, ApiKeyUsageDay, ApiKeyUsageRecord } from "./types.ts";

export const API_KEY_USAGE_PREFIX = ["ubq_ai", "api_keys", "usage"] as const;
export const API_KEY_USAGE_DAILY_PREFIX = ["ubq_ai", "api_keys", "usage_daily"] as const;
export const API_KEY_REQUEST_LOG_PREFIX = ["ubq_ai", "api_keys", "request_log"] as const;

export const apiKeyUsageKey = (id: string) => [...API_KEY_USAGE_PREFIX, id] as const;
export const apiKeyUsageDailyKey = (id: string) => [...API_KEY_USAGE_DAILY_PREFIX, id] as const;
export const apiKeyRequestLogPrefix = (id: string) => [...API_KEY_REQUEST_LOG_PREFIX, id] as const;
export const apiKeyRequestLogKey = (id: string, createdAtMs: number, requestId: string) =>
  [...apiKeyRequestLogPrefix(id), createdAtMs, requestId] as const;

export type ApiKeyUsageDelta = Readonly<{
  request_count?: number;
  stream_request_count?: number;
  non_stream_request_count?: number;
  completed_request_count?: number;
  error_request_count?: number;
  input_tokens?: number;
  output_tokens?: number;
  total_tokens?: number;
  model?: string | null;
  reasoning?: string | null;
  route?: string | null;
  seen_at_ms?: number;
  yunwu_fallback_requests?: number;
  yunwu_input_tokens?: number;
  yunwu_output_tokens?: number;
  yunwu_total_tokens?: number;
  yunwu_spend_microcredits?: number;
}>;

export type ApiKeyRequestLogInput = Readonly<{
  id?: string;
  route: string;
  path: string;
  method: string;
  status_code: number;
  stream: boolean;
  model?: string | null;
  reasoning?: string | null;
  created_at_ms: number;
  provider?: "chatgpt_codex" | "voyage" | "yunwu";
  fallback_reason?: string | null;
  provider_request_id?: string | null;
  completed_at_ms?: number | null;
  latency_ms?: number | null;
  input_tokens?: number | null;
  output_tokens?: number | null;
  provider_quota?: number | null;
  quota_per_credit?: number | null;
  spend_microcredits?: number | null;
  paid_fallback_window_reset_at_ms?: number | null;
  billing_status?: ApiKeyRequestLogRecord["billing_status"];
}>;

export type ApiKeyRequestLogPatch = Partial<Omit<ApiKeyRequestLogInput, "id" | "created_at_ms">>;

const MAX_LABEL_LENGTH = 120;
const MAX_KV_RETRIES = 3;
const DAILY_SERIES_DAYS = 30;
const DAILY_HISTORY_DAYS = 365;
const DAY_MS = 24 * 60 * 60 * 1000;
export const API_KEY_REQUEST_LOG_RETENTION_MS = 365 * DAY_MS;
const MAX_REQUEST_LOGS = 100;
const MAX_PATH_LENGTH = 1024;
const MAX_METHOD_LENGTH = 16;

const coerceNumber = (value: unknown, fallback = 0): number => {
  if (typeof value !== "number" || !Number.isFinite(value)) return fallback;
  return Math.trunc(value);
};

const normalizeNullableNonNegativeNumber = (value: unknown): number | null => {
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  const normalized = Math.trunc(value);
  return normalized >= 0 ? normalized : null;
};

const normalizeLabel = (value: unknown): string | null => {
  const raw = getString(value);
  if (!raw) return null;
  const trimmed = raw.trim();
  if (!trimmed) return null;
  if (trimmed.length > MAX_LABEL_LENGTH) return trimmed.slice(0, MAX_LABEL_LENGTH);
  return trimmed;
};

const normalizeBoundedString = (value: unknown, maxLength: number, fallback = ""): string => {
  const raw = getString(value)?.trim() ?? "";
  const normalized = raw || fallback;
  return normalized.length > maxLength ? normalized.slice(0, maxLength) : normalized;
};

const normalizeRequestLogRecord = (
  value: unknown,
  keyId: string,
  fallbackId: string,
  fallbackCreatedAtMs: number,
): ApiKeyRequestLogRecord | null => {
  if (!isRecord(value)) return null;
  const createdAtMs = coerceNumber(value.created_at_ms, fallbackCreatedAtMs);
  const statusCode = coerceNumber(value.status_code);
  const provider = value.provider === "voyage" ? "voyage" : value.provider === "yunwu" ? "yunwu" : "chatgpt_codex";
  const billingStatus = value.billing_status;
  const normalizedBillingStatus: ApiKeyRequestLogRecord["billing_status"] =
    billingStatus === "pending" || billingStatus === "reconciled" || billingStatus === "not_billed" ||
      billingStatus === "unresolved"
      ? billingStatus
      : "not_applicable";
  const completedAtMs = normalizeNullableNonNegativeNumber(value.completed_at_ms);
  return {
    id: normalizeBoundedString(value.id, MAX_LABEL_LENGTH, fallbackId),
    key_id: normalizeKeyId(value.key_id, keyId),
    route: normalizeBoundedString(value.route, MAX_LABEL_LENGTH, "unknown"),
    path: normalizeBoundedString(value.path, MAX_PATH_LENGTH),
    method: normalizeBoundedString(value.method, MAX_METHOD_LENGTH, "UNKNOWN").toUpperCase(),
    status_code: Math.max(0, Math.min(599, statusCode)),
    stream: value.stream === true,
    model: normalizeLabel(value.model),
    reasoning: normalizeLabel(value.reasoning),
    created_at_ms: createdAtMs > 0 ? createdAtMs : fallbackCreatedAtMs,
    provider,
    fallback_reason: normalizeLabel(value.fallback_reason),
    provider_request_id: normalizeLabel(value.provider_request_id),
    completed_at_ms: completedAtMs,
    latency_ms: normalizeNullableNonNegativeNumber(value.latency_ms),
    input_tokens: normalizeNullableNonNegativeNumber(value.input_tokens),
    output_tokens: normalizeNullableNonNegativeNumber(value.output_tokens),
    provider_quota: normalizeNullableNonNegativeNumber(value.provider_quota),
    quota_per_credit: normalizeNullableNonNegativeNumber(value.quota_per_credit),
    spend_microcredits: normalizeNullableNonNegativeNumber(value.spend_microcredits),
    paid_fallback_window_reset_at_ms: normalizeNullableNonNegativeNumber(value.paid_fallback_window_reset_at_ms),
    billing_status: normalizedBillingStatus,
  };
};

const normalizeKeyId = (value: unknown, fallback: string): string => {
  const raw = getString(value);
  const trimmed = raw?.trim() ?? "";
  return trimmed || fallback;
};

const pad2 = (value: number): string => String(value).padStart(2, "0");

const startOfDayUtcMs = (ms: number): number => {
  const date = new Date(ms);
  return Date.UTC(date.getUTCFullYear(), date.getUTCMonth(), date.getUTCDate());
};

const dayKeyFromMs = (ms: number): string => {
  const date = new Date(ms);
  return `${date.getUTCFullYear()}-${pad2(date.getUTCMonth() + 1)}-${pad2(date.getUTCDate())}`;
};

const dayKeyToMs = (value: string): number | null => {
  if (typeof value !== "string") return null;
  const trimmed = value.trim();
  if (!/^\d{4}-\d{2}-\d{2}$/.test(trimmed)) return null;
  const [yearStr, monthStr, dayStr] = trimmed.split("-");
  const year = Number(yearStr);
  const month = Number(monthStr);
  const day = Number(dayStr);
  if (!Number.isFinite(year) || !Number.isFinite(month) || !Number.isFinite(day)) return null;
  return Date.UTC(year, month - 1, day);
};

const clampDelta = (value: unknown): number => {
  const num = coerceNumber(value, 0);
  return num > 0 ? num : 0;
};

const buildBaseUsageRecord = (keyId: string, nowMs: number): ApiKeyUsageRecord => ({
  key_id: keyId,
  total_requests: 0,
  stream_requests: 0,
  non_stream_requests: 0,
  completed_requests: 0,
  error_requests: 0,
  input_tokens: 0,
  output_tokens: 0,
  total_tokens: 0,
  first_seen_at_ms: nowMs,
  last_seen_at_ms: nowMs,
  last_model: null,
  last_reasoning: null,
  last_route: null,
  yunwu_fallback_requests: 0,
  yunwu_input_tokens: 0,
  yunwu_output_tokens: 0,
  yunwu_total_tokens: 0,
  yunwu_spend_microcredits: 0,
});

const normalizeUsageRecord = (value: unknown, keyId: string, nowMs: number): ApiKeyUsageRecord => {
  if (!isRecord(value)) return buildBaseUsageRecord(keyId, nowMs);
  return {
    key_id: normalizeKeyId(value.key_id, keyId),
    total_requests: coerceNumber(value.total_requests),
    stream_requests: coerceNumber(value.stream_requests),
    non_stream_requests: coerceNumber(value.non_stream_requests),
    completed_requests: coerceNumber(value.completed_requests),
    error_requests: coerceNumber(value.error_requests),
    input_tokens: coerceNumber(value.input_tokens),
    output_tokens: coerceNumber(value.output_tokens),
    total_tokens: coerceNumber(value.total_tokens),
    first_seen_at_ms: coerceNumber(value.first_seen_at_ms, nowMs),
    last_seen_at_ms: coerceNumber(value.last_seen_at_ms, nowMs),
    last_model: normalizeLabel(value.last_model),
    last_reasoning: normalizeLabel(value.last_reasoning),
    last_route: normalizeLabel(value.last_route),
    yunwu_fallback_requests: Math.max(0, coerceNumber(value.yunwu_fallback_requests)),
    yunwu_input_tokens: Math.max(0, coerceNumber(value.yunwu_input_tokens)),
    yunwu_output_tokens: Math.max(0, coerceNumber(value.yunwu_output_tokens)),
    yunwu_total_tokens: Math.max(0, coerceNumber(value.yunwu_total_tokens)),
    yunwu_spend_microcredits: Math.max(0, coerceNumber(value.yunwu_spend_microcredits)),
  };
};

const normalizeDailyUsageDay = (value: unknown): ApiKeyUsageDay | null => {
  if (!isRecord(value)) return null;
  const day = typeof value.day === "string" ? value.day.trim() : "";
  if (!day) return null;
  if (dayKeyToMs(day) === null) return null;
  const requestCount = Math.max(0, coerceNumber(value.request_count, 0));
  return {
    day,
    request_count: requestCount,
    yunwu_fallback_requests: Math.max(0, coerceNumber(value.yunwu_fallback_requests)),
    yunwu_spend_microcredits: Math.max(0, coerceNumber(value.yunwu_spend_microcredits)),
  };
};

const normalizeDailyUsageRecord = (
  value: unknown,
  keyId: string,
  nowMs: number,
): ApiKeyUsageDailyRecord => {
  if (!isRecord(value)) return { key_id: keyId, days: [], updated_at_ms: nowMs };
  const daysRaw = Array.isArray(value.days) ? value.days : [];
  const days: ApiKeyUsageDay[] = [];
  for (const item of daysRaw) {
    const normalized = normalizeDailyUsageDay(item);
    if (normalized) days.push(normalized);
  }
  return {
    key_id: normalizeKeyId(value.key_id, keyId),
    days,
    updated_at_ms: coerceNumber(value.updated_at_ms, nowMs),
  };
};

const pruneDailyUsageDays = (days: ApiKeyUsageDay[], nowMs: number): ApiKeyUsageDay[] => {
  const cutoffMs = startOfDayUtcMs(nowMs) - (DAILY_HISTORY_DAYS - 1) * DAY_MS;
  return days
    .filter((entry) => {
      const dayMs = dayKeyToMs(entry.day);
      return dayMs !== null && dayMs >= cutoffMs;
    })
    .sort((a, b) => a.day.localeCompare(b.day));
};

const buildDailySeries = (
  record: ApiKeyUsageDailyRecord,
  nowMs: number,
  days: number,
  field: keyof Pick<ApiKeyUsageDay, "request_count" | "yunwu_fallback_requests" | "yunwu_spend_microcredits">,
): number[] => {
  const seriesDays = Math.max(1, Math.trunc(days));
  const startMs = startOfDayUtcMs(nowMs) - (seriesDays - 1) * DAY_MS;
  const countsByDay = new Map<string, number>();
  for (const entry of record.days) {
    countsByDay.set(entry.day, entry[field]);
  }
  const series: number[] = [];
  for (let i = 0; i < seriesDays; i += 1) {
    const dayMs = startMs + i * DAY_MS;
    const dayKey = dayKeyFromMs(dayMs);
    series.push(countsByDay.get(dayKey) ?? 0);
  }
  return series;
};

const applyDelta = (record: ApiKeyUsageRecord, delta: ApiKeyUsageDelta, nowMs: number): ApiKeyUsageRecord => {
  const model = delta.model === undefined ? record.last_model : normalizeLabel(delta.model);
  const reasoning = delta.reasoning === undefined ? record.last_reasoning : normalizeLabel(delta.reasoning);
  const route = delta.route === undefined ? record.last_route : normalizeLabel(delta.route);
  const seenAt = typeof delta.seen_at_ms === "number" && Number.isFinite(delta.seen_at_ms)
    ? Math.trunc(delta.seen_at_ms)
    : nowMs;

  return {
    key_id: record.key_id,
    total_requests: record.total_requests + clampDelta(delta.request_count),
    stream_requests: record.stream_requests + clampDelta(delta.stream_request_count),
    non_stream_requests: record.non_stream_requests + clampDelta(delta.non_stream_request_count),
    completed_requests: record.completed_requests + clampDelta(delta.completed_request_count),
    error_requests: record.error_requests + clampDelta(delta.error_request_count),
    input_tokens: record.input_tokens + clampDelta(delta.input_tokens),
    output_tokens: record.output_tokens + clampDelta(delta.output_tokens),
    total_tokens: record.total_tokens + clampDelta(delta.total_tokens),
    first_seen_at_ms: record.first_seen_at_ms > 0 ? record.first_seen_at_ms : seenAt,
    last_seen_at_ms: seenAt,
    last_model: model,
    last_reasoning: reasoning,
    last_route: route,
    yunwu_fallback_requests: record.yunwu_fallback_requests + clampDelta(delta.yunwu_fallback_requests),
    yunwu_input_tokens: record.yunwu_input_tokens + clampDelta(delta.yunwu_input_tokens),
    yunwu_output_tokens: record.yunwu_output_tokens + clampDelta(delta.yunwu_output_tokens),
    yunwu_total_tokens: record.yunwu_total_tokens + clampDelta(delta.yunwu_total_tokens),
    yunwu_spend_microcredits: record.yunwu_spend_microcredits + clampDelta(delta.yunwu_spend_microcredits),
  };
};

const applyDailyDelta = (
  record: ApiKeyUsageDailyRecord,
  keyId: string,
  delta: ApiKeyUsageDelta,
  nowMs: number,
): ApiKeyUsageDailyRecord | null => {
  const requestCount = clampDelta(delta.request_count);
  const yunwuFallbackRequests = clampDelta(delta.yunwu_fallback_requests);
  const yunwuSpendMicrocredits = clampDelta(delta.yunwu_spend_microcredits);
  if (requestCount === 0 && yunwuFallbackRequests === 0 && yunwuSpendMicrocredits === 0) return null;

  const seenAt = typeof delta.seen_at_ms === "number" && Number.isFinite(delta.seen_at_ms)
    ? Math.trunc(delta.seen_at_ms)
    : nowMs;
  const dayKey = dayKeyFromMs(seenAt);
  const existing = record.days.find((item) => item.day === dayKey);
  const nextDays = existing
    ? record.days.map((item) =>
      item.day === dayKey
        ? {
          ...item,
          request_count: item.request_count + requestCount,
          yunwu_fallback_requests: item.yunwu_fallback_requests + yunwuFallbackRequests,
          yunwu_spend_microcredits: item.yunwu_spend_microcredits + yunwuSpendMicrocredits,
        }
        : item
    )
    : [...record.days, {
      day: dayKey,
      request_count: requestCount,
      yunwu_fallback_requests: yunwuFallbackRequests,
      yunwu_spend_microcredits: yunwuSpendMicrocredits,
    }];
  return {
    key_id: keyId,
    days: pruneDailyUsageDays(nextDays, nowMs),
    updated_at_ms: nowMs,
  };
};

export type ApiKeyUsageAtomicMutation = Readonly<{
  usage_key: Deno.KvKey;
  usage_entry: Deno.KvEntryMaybe<ApiKeyUsageRecord>;
  usage_record: ApiKeyUsageRecord;
  daily_key: Deno.KvKey;
  daily_entry: Deno.KvEntryMaybe<ApiKeyUsageDailyRecord>;
  daily_record: ApiKeyUsageDailyRecord | null;
}>;

export const prepareApiKeyUsageAtomicMutation = async (
  kv: Deno.Kv,
  keyId: string,
  delta: ApiKeyUsageDelta,
  nowMs = Date.now(),
): Promise<ApiKeyUsageAtomicMutation> => {
  const usageKey = apiKeyUsageKey(keyId);
  const usageEntry = await kv.get<ApiKeyUsageRecord>(usageKey);
  const seenAt = typeof delta.seen_at_ms === "number" && Number.isFinite(delta.seen_at_ms)
    ? Math.trunc(delta.seen_at_ms)
    : nowMs;
  const usageRecord = applyDelta(
    normalizeUsageRecord(usageEntry.value, keyId, seenAt),
    delta,
    nowMs,
  );

  const dailyKey = apiKeyUsageDailyKey(keyId);
  const dailyEntry = await kv.get<ApiKeyUsageDailyRecord>(dailyKey);
  const dailyRecord = applyDailyDelta(
    normalizeDailyUsageRecord(dailyEntry.value, keyId, nowMs),
    keyId,
    delta,
    nowMs,
  );
  return {
    usage_key: usageKey,
    usage_entry: usageEntry,
    usage_record: usageRecord,
    daily_key: dailyKey,
    daily_entry: dailyEntry,
    daily_record: dailyRecord,
  };
};

export const recordApiKeyUsage = async (keyId: string, delta: ApiKeyUsageDelta): Promise<void> => {
  try {
    const kv = await kvPromise;
    if (!kv) return;

    const key = apiKeyUsageKey(keyId);
    const nowMs = Date.now();
    let usageUpdated = false;

    for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt++) {
      const keyEntry = await kv.get(apiKeyIdKey(keyId));
      if (!keyEntry.value) return;
      const entry = await kv.get<ApiKeyUsageRecord>(key);
      const current = normalizeUsageRecord(entry.value, keyId, nowMs);
      const updated = applyDelta(current, delta, nowMs);
      const commit = await kv.atomic().check(keyEntry).check(entry).set(key, updated).commit();
      if (commit.ok) {
        usageUpdated = true;
        break;
      }
    }

    if (!usageUpdated) {
      console.warn("[ai.ubq.fi] Failed to update api key usage after retries:", keyId);
      return;
    }

    const requestCount = clampDelta(delta.request_count);
    const yunwuFallbackRequests = clampDelta(delta.yunwu_fallback_requests);
    const yunwuSpendMicrocredits = clampDelta(delta.yunwu_spend_microcredits);
    if (requestCount > 0 || yunwuFallbackRequests > 0 || yunwuSpendMicrocredits > 0) {
      const seenAt = typeof delta.seen_at_ms === "number" && Number.isFinite(delta.seen_at_ms)
        ? Math.trunc(delta.seen_at_ms)
        : nowMs;
      const dayKey = dayKeyFromMs(seenAt);
      for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt++) {
        const keyEntry = await kv.get(apiKeyIdKey(keyId));
        if (!keyEntry.value) return;
        const entry = await kv.get<ApiKeyUsageDailyRecord>(apiKeyUsageDailyKey(keyId));
        const current = normalizeDailyUsageRecord(entry.value, keyId, nowMs);
        let nextDays = [...current.days];
        const existing = nextDays.find((item) => item.day === dayKey);
        if (existing) {
          nextDays = nextDays.map((item) =>
            item.day === dayKey
              ? {
                ...item,
                request_count: item.request_count + requestCount,
                yunwu_fallback_requests: item.yunwu_fallback_requests + yunwuFallbackRequests,
                yunwu_spend_microcredits: item.yunwu_spend_microcredits + yunwuSpendMicrocredits,
              }
              : item
          );
        } else {
          nextDays = [...nextDays, {
            day: dayKey,
            request_count: requestCount,
            yunwu_fallback_requests: yunwuFallbackRequests,
            yunwu_spend_microcredits: yunwuSpendMicrocredits,
          }];
        }
        const updated: ApiKeyUsageDailyRecord = {
          key_id: keyId,
          days: pruneDailyUsageDays(nextDays, nowMs),
          updated_at_ms: nowMs,
        };
        const commit = await kv.atomic()
          .check(keyEntry)
          .check(entry)
          .set(apiKeyUsageDailyKey(keyId), updated)
          .commit();
        if (commit.ok) break;
      }
    }
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to record api key usage:", error);
  }
};

export const getApiKeyUsage = async (
  keyId: string,
  options: { includeDaily?: boolean; dailyDays?: number } = {},
): Promise<
  | (ApiKeyUsageRecord & {
    daily_requests?: number[];
    daily_yunwu_fallback_requests?: number[];
    daily_yunwu_spend_microcredits?: number[];
  })
  | null
> => {
  try {
    const kv = await kvPromise;
    if (!kv) return null;
    const nowMs = Date.now();
    const entry = await kv.get<ApiKeyUsageRecord>(apiKeyUsageKey(keyId));
    if (!entry.value) return null;
    const usage = normalizeUsageRecord(entry.value, keyId, nowMs);
    if (!options.includeDaily) return usage;
    const dailyEntry = await kv.get<ApiKeyUsageDailyRecord>(apiKeyUsageDailyKey(keyId));
    if (!dailyEntry.value) {
      return {
        ...usage,
        daily_requests: [],
        daily_yunwu_fallback_requests: [],
        daily_yunwu_spend_microcredits: [],
      };
    }
    const dailyRecord = normalizeDailyUsageRecord(dailyEntry.value, keyId, nowMs);
    const seriesDays = options.dailyDays ?? DAILY_SERIES_DAYS;
    return {
      ...usage,
      daily_requests: buildDailySeries(dailyRecord, nowMs, seriesDays, "request_count"),
      daily_yunwu_fallback_requests: buildDailySeries(
        dailyRecord,
        nowMs,
        seriesDays,
        "yunwu_fallback_requests",
      ),
      daily_yunwu_spend_microcredits: buildDailySeries(
        dailyRecord,
        nowMs,
        seriesDays,
        "yunwu_spend_microcredits",
      ),
    };
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to load api key usage:", error);
    return null;
  }
};

export const recordApiKeyRequestLog = async (
  keyId: string,
  input: ApiKeyRequestLogInput,
  kvOverride?: Deno.Kv | null,
): Promise<void> => {
  try {
    const normalizedKeyId = keyId.trim();
    if (!normalizedKeyId) return;
    const kv = kvOverride === undefined ? await kvPromise : kvOverride;
    if (!kv) return;

    const nowMs = Date.now();
    const requestId = normalizeBoundedString(input.id, MAX_LABEL_LENGTH, crypto.randomUUID());
    const createdAtMs = coerceNumber(input.created_at_ms, nowMs);
    const key = apiKeyRequestLogKey(normalizedKeyId, createdAtMs, requestId);
    for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt++) {
      const keyEntry = await kv.get(apiKeyIdKey(normalizedKeyId));
      if (!keyEntry.value) return;
      const entry = await kv.get<ApiKeyRequestLogRecord>(key);
      const base = entry.value ? normalizeRequestLogRecord(entry.value, normalizedKeyId, requestId, createdAtMs) : null;
      const patch = Object.fromEntries(
        Object.entries(input).filter(([, value]) => value !== undefined),
      );
      const record = normalizeRequestLogRecord(
        {
          ...(base ?? {}),
          ...patch,
          id: requestId,
          key_id: normalizedKeyId,
          created_at_ms: createdAtMs,
        },
        normalizedKeyId,
        requestId,
        createdAtMs,
      );
      if (!record) return;
      const expireIn = Math.max(1, createdAtMs + API_KEY_REQUEST_LOG_RETENTION_MS - nowMs);
      const commit = await kv.atomic()
        .check(keyEntry)
        .check(entry)
        .set(key, record, { expireIn })
        .commit();
      if (commit.ok) return;
    }
    console.warn("[ai.ubq.fi] Failed to update api key request log after retries:", requestId);
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to record api key request log:", error);
  }
};

export const getApiKeyRequestLog = async (
  keyId: string,
  createdAtMs: number,
  requestId: string,
  kvOverride?: Deno.Kv | null,
): Promise<ApiKeyRequestLogRecord | null> => {
  const normalizedKeyId = keyId.trim();
  const normalizedRequestId = requestId.trim();
  if (!normalizedKeyId || !normalizedRequestId) return null;
  const kv = kvOverride === undefined ? await kvPromise : kvOverride;
  if (!kv) return null;
  const entry = await kv.get<ApiKeyRequestLogRecord>(
    apiKeyRequestLogKey(normalizedKeyId, createdAtMs, normalizedRequestId),
  );
  return normalizeRequestLogRecord(entry.value, normalizedKeyId, normalizedRequestId, createdAtMs);
};

export const updateApiKeyRequestLog = async (
  keyId: string,
  createdAtMs: number,
  requestId: string,
  patch: ApiKeyRequestLogPatch,
  kvOverride?: Deno.Kv | null,
): Promise<void> => {
  const existing = await getApiKeyRequestLog(keyId, createdAtMs, requestId, kvOverride);
  if (!existing) return;
  await recordApiKeyRequestLog(keyId, {
    ...existing,
    ...patch,
    id: requestId,
    created_at_ms: createdAtMs,
  }, kvOverride);
};

export const listApiKeyRequestLogs = async (
  keyId: string,
  options: { limit?: number; kv?: Deno.Kv | null } = {},
): Promise<ApiKeyRequestLogRecord[]> => {
  const normalizedKeyId = keyId.trim();
  if (!normalizedKeyId) return [];
  const kv = options.kv === undefined ? await kvPromise : options.kv;
  if (!kv) return [];

  const requestedLimit = typeof options.limit === "number" && Number.isFinite(options.limit)
    ? Math.trunc(options.limit)
    : 20;
  const limit = Math.max(1, Math.min(MAX_REQUEST_LOGS, requestedLimit));
  const records: ApiKeyRequestLogRecord[] = [];
  for await (
    const entry of kv.list<ApiKeyRequestLogRecord>(
      { prefix: apiKeyRequestLogPrefix(normalizedKeyId) },
      { reverse: true, limit },
    )
  ) {
    const keyRequestId = getString(entry.key.at(-1)) ?? "request";
    const keyCreatedAtMs = coerceNumber(entry.key.at(-2), Date.now());
    const record = normalizeRequestLogRecord(
      entry.value,
      normalizedKeyId,
      keyRequestId,
      keyCreatedAtMs,
    );
    if (record) records.push(record);
  }
  return records;
};
