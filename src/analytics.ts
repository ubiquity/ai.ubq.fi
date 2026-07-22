import { kvPromise } from "./kv.ts";
import type { ApiKeyRequestLogRecord } from "./types.ts";
import { getString, isRecord } from "./utils.ts";

// v1 aggregates remain named for deletion and migration only. The serving path never reads or writes them.
export const API_KEY_USAGE_PREFIX = ["ubq_ai", "api_keys", "usage"] as const;
export const API_KEY_USAGE_DAILY_PREFIX = ["ubq_ai", "api_keys", "usage_daily"] as const;
export const API_KEY_REQUEST_LOG_PREFIX = ["uos_ai", "paid_fallback", "ledger"] as const;

export const apiKeyUsageKey = (id: string) => [...API_KEY_USAGE_PREFIX, id] as const;
export const apiKeyUsageDailyKey = (id: string) => [...API_KEY_USAGE_DAILY_PREFIX, id] as const;
export const apiKeyRequestLogPrefix = (id: string) => [...API_KEY_REQUEST_LOG_PREFIX, id] as const;
export const apiKeyRequestLogKey = (id: string, createdAtMs: number, requestId: string) =>
  [...apiKeyRequestLogPrefix(id), createdAtMs, requestId] as const;

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

const DAY_MS = 24 * 60 * 60 * 1000;
export const API_KEY_REQUEST_LOG_RETENTION_MS = 365 * DAY_MS;
const MAX_REQUEST_LOGS = 100;

const integer = (value: unknown, fallback = 0): number =>
  typeof value === "number" && Number.isFinite(value) ? Math.trunc(value) : fallback;
const nullableInteger = (value: unknown): number | null => {
  const normalized = integer(value, -1);
  return normalized >= 0 ? normalized : null;
};
const text = (value: unknown, max = 120, fallback = ""): string => {
  const normalized = getString(value)?.trim() || fallback;
  return normalized.slice(0, max);
};

const normalize = (
  value: unknown,
  keyId: string,
  requestId: string,
  createdAtMs: number,
): ApiKeyRequestLogRecord | null => {
  if (!isRecord(value)) return null;
  const billing = value.billing_status;
  return {
    id: text(value.id, 120, requestId),
    key_id: text(value.key_id, 200, keyId),
    route: text(value.route, 120, "unknown"),
    path: text(value.path, 1024),
    method: text(value.method, 16, "UNKNOWN").toUpperCase(),
    status_code: Math.max(0, Math.min(599, integer(value.status_code))),
    stream: value.stream === true,
    model: text(value.model) || null,
    reasoning: text(value.reasoning) || null,
    created_at_ms: integer(value.created_at_ms, createdAtMs),
    provider: value.provider === "voyage" ? "voyage" : value.provider === "yunwu" ? "yunwu" : "chatgpt_codex",
    fallback_reason: text(value.fallback_reason) || null,
    provider_request_id: text(value.provider_request_id) || null,
    completed_at_ms: nullableInteger(value.completed_at_ms),
    latency_ms: nullableInteger(value.latency_ms),
    input_tokens: nullableInteger(value.input_tokens),
    output_tokens: nullableInteger(value.output_tokens),
    provider_quota: nullableInteger(value.provider_quota),
    quota_per_credit: nullableInteger(value.quota_per_credit),
    spend_microcredits: nullableInteger(value.spend_microcredits),
    paid_fallback_window_reset_at_ms: nullableInteger(value.paid_fallback_window_reset_at_ms),
    billing_status: billing === "pending" || billing === "reconciled" || billing === "not_billed" ||
        billing === "unresolved"
      ? billing
      : "not_applicable",
  };
};

export const recordApiKeyRequestLog = async (
  keyId: string,
  input: ApiKeyRequestLogInput,
  kvOverride?: Deno.Kv | null,
): Promise<void> => {
  const kv = kvOverride === undefined ? await kvPromise : kvOverride;
  if (!kv || !keyId.trim()) return;
  const nowMs = Date.now();
  const requestId = text(input.id, 120, crypto.randomUUID());
  const createdAtMs = integer(input.created_at_ms, nowMs);
  const key = apiKeyRequestLogKey(keyId, createdAtMs, requestId);
  const entry = await kv.get<ApiKeyRequestLogRecord>(key, { consistency: "strong" });
  const record = normalize(
    { ...(entry.value ?? {}), ...input, id: requestId, key_id: keyId },
    keyId,
    requestId,
    createdAtMs,
  );
  if (!record) return;
  const expireIn = Math.max(1, createdAtMs + API_KEY_REQUEST_LOG_RETENTION_MS - nowMs);
  const committed = await kv.atomic().check(entry).set(key, record, { expireIn }).commit();
  if (!committed.ok) throw new Error(`paid fallback ledger changed concurrently: ${requestId}`);
};

export const getApiKeyRequestLog = async (
  keyId: string,
  createdAtMs: number,
  requestId: string,
  kvOverride?: Deno.Kv | null,
): Promise<ApiKeyRequestLogRecord | null> => {
  const kv = kvOverride === undefined ? await kvPromise : kvOverride;
  if (!kv) return null;
  const entry = await kv.get<ApiKeyRequestLogRecord>(apiKeyRequestLogKey(keyId, createdAtMs, requestId));
  return normalize(entry.value, keyId, requestId, createdAtMs);
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
  await recordApiKeyRequestLog(keyId, { ...existing, ...patch, id: requestId, created_at_ms: createdAtMs }, kvOverride);
};

export const listApiKeyRequestLogs = async (
  keyId: string,
  options: { limit?: number; kv?: Deno.Kv | null } = {},
): Promise<ApiKeyRequestLogRecord[]> => {
  const kv = options.kv === undefined ? await kvPromise : options.kv;
  if (!kv || !keyId.trim()) return [];
  const limit = Math.max(1, Math.min(MAX_REQUEST_LOGS, Math.trunc(options.limit ?? 20)));
  const records: ApiKeyRequestLogRecord[] = [];
  for await (
    const entry of kv.list<ApiKeyRequestLogRecord>({ prefix: apiKeyRequestLogPrefix(keyId) }, { reverse: true, limit })
  ) {
    const record = normalize(
      entry.value,
      keyId,
      text(entry.key.at(-1), 120, "request"),
      integer(entry.key.at(-2), Date.now()),
    );
    if (record) records.push(record);
  }
  return records;
};
