import { isRecord } from "./utils.ts";

/**
 * Compact per-hour usage rollups for settled paid-fallback requests.
 *
 * Raw paid-fallback request rows are retained for a bounded window
 * (PAID_FALLBACK_REQUEST_LOG_RETENTION_MS); these hour×model×provider
 * aggregates are retained indefinitely so long-run research and quota-runway
 * estimates survive row expiry. Every settled request contributes exactly one
 * rollup update, so the sums here are authoritative for the settled traffic
 * of the paid fallback provider.
 *
 * The hourly key is sharded by request id so concurrent settlements of the
 * same model/provider never contend on one KV key inside the settlement
 * atomic; readers sum all shards when aggregating.
 */
export const PAID_FALLBACK_USAGE_ROLLUP_PREFIX = ["uos_ai", "paid_fallback", "v3", "usage_rollup"] as const;
export const PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS = 60 * 60 * 1_000;
export const PAID_FALLBACK_USAGE_ROLLUP_SHARD_COUNT = 16;

const FNV_PRIME = 0x01000193;

/**
 * Deterministic shard for a request id: spreads concurrent settlements of one
 * model/provider across PAID_FALLBACK_USAGE_ROLLUP_SHARD_COUNT keys while
 * keeping every settlement of the same request on one key.
 */
export const paidFallbackUsageRollupShard = (requestId: string): number => {
  let hash = 0x811c9dc5;
  for (let index = 0; index < requestId.length; index += 1) {
    hash ^= requestId.charCodeAt(index);
    hash = Math.imul(hash, FNV_PRIME) >>> 0;
  }
  return hash % PAID_FALLBACK_USAGE_ROLLUP_SHARD_COUNT;
};

export type PaidFallbackUsageRollup = Readonly<{
  v: 1;
  bucket_start_at_ms: number;
  model: string;
  provider: string;
  request_count: number;
  quota_sum: number;
  input_tokens: number;
  cached_input_tokens: number;
  output_tokens: number;
  spend_microcredits: number;
  first_request_at_ms: number;
  last_request_at_ms: number;
  updated_at_ms: number;
}>;

export type PaidFallbackUsageRollupInput = Readonly<{
  bucket_start_at_ms: number;
  model: string;
  provider: string;
  request_id: string;
  quota: number;
  input_tokens: number;
  cached_input_tokens: number | null;
  output_tokens: number;
  spend_microcredits: number;
  request_created_at_ms: number;
  updated_at_ms: number;
}>;

const safeInteger = (value: unknown, min = 0): value is number => typeof value === "number" && Number.isSafeInteger(value) && value >= min;

const nonNegativeFinite = (value: unknown): value is number => typeof value === "number" && Number.isFinite(value) && value >= 0;

export const isPaidFallbackUsageRollup = (value: unknown): value is PaidFallbackUsageRollup => {
  if (!isRecord(value)) return false;
  return (
    value.v === 1 &&
    safeInteger(value.bucket_start_at_ms) &&
    typeof value.model === "string" &&
    value.model.length > 0 &&
    typeof value.provider === "string" &&
    value.provider.length > 0 &&
    safeInteger(value.request_count) &&
    nonNegativeFinite(value.quota_sum) &&
    safeInteger(value.input_tokens) &&
    safeInteger(value.cached_input_tokens) &&
    safeInteger(value.output_tokens) &&
    nonNegativeFinite(value.spend_microcredits) &&
    safeInteger(value.first_request_at_ms) &&
    safeInteger(value.last_request_at_ms) &&
    safeInteger(value.updated_at_ms)
  );
};

export const paidFallbackUsageRollupKey = (bucketStartAtMs: number, model: string, provider: string, shard: number): Deno.KvKey => [
  ...PAID_FALLBACK_USAGE_ROLLUP_PREFIX,
  bucketStartAtMs,
  model,
  provider,
  shard,
];

/**
 * Merges one settled request into an existing/hour bucket. Pure so tests can
 * assert sums without a KV backend.
 */
export const mergePaidFallbackUsageRollup = (existing: PaidFallbackUsageRollup | null, input: PaidFallbackUsageRollupInput): PaidFallbackUsageRollup => {
  const firstRequestAtMs = existing ? Math.min(existing.first_request_at_ms, input.request_created_at_ms) : input.request_created_at_ms;
  const lastRequestAtMs = existing ? Math.max(existing.last_request_at_ms, input.request_created_at_ms) : input.request_created_at_ms;
  return {
    v: 1,
    bucket_start_at_ms: existing?.bucket_start_at_ms ?? input.bucket_start_at_ms,
    model: existing?.model ?? input.model,
    provider: existing?.provider ?? input.provider,
    request_count: (existing?.request_count ?? 0) + 1,
    quota_sum: (existing?.quota_sum ?? 0) + input.quota,
    input_tokens: (existing?.input_tokens ?? 0) + input.input_tokens,
    cached_input_tokens: (existing?.cached_input_tokens ?? 0) + (input.cached_input_tokens ?? 0),
    output_tokens: (existing?.output_tokens ?? 0) + input.output_tokens,
    spend_microcredits: (existing?.spend_microcredits ?? 0) + input.spend_microcredits,
    first_request_at_ms: firstRequestAtMs,
    last_request_at_ms: lastRequestAtMs,
    updated_at_ms: input.updated_at_ms,
  };
};

export const listPaidFallbackUsageRollups = async (
  kv: Deno.Kv | null,
  options: Readonly<{ sinceMs: number; nowMs: number }>
): Promise<PaidFallbackUsageRollup[]> => {
  if (!kv) return [];
  const entries: PaidFallbackUsageRollup[] = [];
  // Floor the scan start to the hour boundary so the bucket that contains the
  // window start is never excluded (bucket-level precision is documented).
  const start: Deno.KvKey = [
    ...PAID_FALLBACK_USAGE_ROLLUP_PREFIX,
    Math.floor(Math.max(0, Math.trunc(options.sinceMs)) / PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS) * PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS,
  ];
  const end: Deno.KvKey = [...PAID_FALLBACK_USAGE_ROLLUP_PREFIX, Math.trunc(options.nowMs) + PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS];
  // Deno KV rejects selectors that combine a prefix with both range bounds.
  // These bounds already include the complete rollup namespace.
  for await (const entry of kv.list<PaidFallbackUsageRollup>({ start, end })) {
    const rollup = isPaidFallbackUsageRollup(entry.value) ? entry.value : null;
    if (rollup) entries.push(rollup);
  }
  return entries;
};
