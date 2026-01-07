import { kvPromise } from "./kv.ts";
import { getString, isRecord } from "./utils.ts";
import type { ApiKeyUsageRecord } from "./types.ts";

export const API_KEY_USAGE_PREFIX = ["ubq_ai", "api_keys", "usage"] as const;

export const apiKeyUsageKey = (id: string) => [...API_KEY_USAGE_PREFIX, id] as const;

type ApiKeyUsageDelta = Readonly<{
  request_count?: number;
  stream_request_count?: number;
  non_stream_request_count?: number;
  completed_request_count?: number;
  error_request_count?: number;
  input_tokens?: number;
  output_tokens?: number;
  total_tokens?: number;
  model?: string | null;
  route?: string | null;
  seen_at_ms?: number;
}>;

const MAX_LABEL_LENGTH = 120;
const MAX_KV_RETRIES = 3;

const coerceNumber = (value: unknown, fallback = 0): number => {
  if (typeof value !== "number" || !Number.isFinite(value)) return fallback;
  return Math.trunc(value);
};

const normalizeLabel = (value: unknown): string | null => {
  const raw = getString(value);
  if (!raw) return null;
  const trimmed = raw.trim();
  if (!trimmed) return null;
  if (trimmed.length > MAX_LABEL_LENGTH) return trimmed.slice(0, MAX_LABEL_LENGTH);
  return trimmed;
};

const normalizeKeyId = (value: unknown, fallback: string): string => {
  const raw = getString(value);
  const trimmed = raw?.trim() ?? "";
  return trimmed || fallback;
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
  last_route: null,
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
    last_route: normalizeLabel(value.last_route),
  };
};

const applyDelta = (record: ApiKeyUsageRecord, delta: ApiKeyUsageDelta, nowMs: number): ApiKeyUsageRecord => {
  const model = delta.model === undefined ? record.last_model : normalizeLabel(delta.model);
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
    last_route: route,
  };
};

export const recordApiKeyUsage = async (keyId: string, delta: ApiKeyUsageDelta): Promise<void> => {
  try {
    const kv = await kvPromise;
    if (!kv) return;

    const key = apiKeyUsageKey(keyId);
    const nowMs = Date.now();

    for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt++) {
      const entry = await kv.get<ApiKeyUsageRecord>(key);
      const current = normalizeUsageRecord(entry.value, keyId, nowMs);
      const updated = applyDelta(current, delta, nowMs);
      const commit = await kv.atomic().check(entry).set(key, updated).commit();
      if (commit.ok) return;
    }

    console.warn("[ai.ubq.fi] Failed to update api key usage after retries:", keyId);
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to record api key usage:", error);
  }
};

export const getApiKeyUsage = async (keyId: string): Promise<ApiKeyUsageRecord | null> => {
  try {
    const kv = await kvPromise;
    if (!kv) return null;
    const entry = await kv.get<ApiKeyUsageRecord>(apiKeyUsageKey(keyId));
    if (!entry.value) return null;
    return normalizeUsageRecord(entry.value, keyId, Date.now());
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to load api key usage:", error);
    return null;
  }
};
