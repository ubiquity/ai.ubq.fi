import { API_KEY_NO_USAGE_LIMIT, USAGE_RESET_PERIOD_MS, shouldResetUsage } from "./api_keys.ts";
import { openaiError } from "./http.ts";
import { kvPromise } from "./kv.ts";
import { getString, isRecord } from "./utils.ts";
import type { KernelAuthLimitRecord, KernelAuthUsageRecord, KernelOrgLimitRecord, KernelOrgUsageRecord } from "./types.ts";

export const KERNEL_AUTH_USAGE_PREFIX = ["ubq_ai", "kernel_auth", "usage"] as const;
export const KERNEL_AUTH_LIMIT_PREFIX = ["ubq_ai", "kernel_auth", "limits"] as const;
export const KERNEL_AUTH_ORG_USAGE_PREFIX = ["ubq_ai", "kernel_auth", "org_usage"] as const;
export const KERNEL_AUTH_ORG_LIMIT_PREFIX = ["ubq_ai", "kernel_auth", "org_limits"] as const;

const MAX_LABEL_LENGTH = 120;
const MAX_KV_RETRIES = 3;
const DEFAULT_KERNEL_AUTH_WINDOW_MS = USAGE_RESET_PERIOD_MS;

const normalizeWindowMs = (value: unknown, fallback: number): number => {
  if (typeof value !== "number" || !Number.isFinite(value)) return fallback;
  const windowMs = Math.trunc(value);
  if (windowMs <= 0) return fallback;
  return windowMs;
};

const calculateNextResetMsForWindow = (nowMs: number, windowMs: number): number => {
  return nowMs + windowMs;
};

const normalizeUsageLimitRequests = (value: unknown, fallback: number): number => {
  if (typeof value !== "number" || !Number.isFinite(value)) return fallback;
  const limit = Math.trunc(value);
  if (limit === API_KEY_NO_USAGE_LIMIT) return API_KEY_NO_USAGE_LIMIT;
  if (limit < 0) return fallback;
  return limit;
};

const DEFAULT_KERNEL_AUTH_USAGE_LIMIT_REQUESTS = API_KEY_NO_USAGE_LIMIT;

export const kernelUsageKey = (owner: string, repo: string) => [...KERNEL_AUTH_USAGE_PREFIX, owner, repo] as const;
export const kernelLimitKey = (owner: string, repo: string) => [...KERNEL_AUTH_LIMIT_PREFIX, owner, repo] as const;
export const kernelOrgUsageKey = (owner: string) => [...KERNEL_AUTH_ORG_USAGE_PREFIX, owner] as const;
export const kernelOrgLimitKey = (owner: string) => [...KERNEL_AUTH_ORG_LIMIT_PREFIX, owner] as const;

type KernelUsageDelta = Readonly<{
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

const normalizeOwnerRepo = (value: unknown, fallback: string): string => {
  const raw = getString(value);
  const trimmed = raw?.trim() ?? "";
  return trimmed || fallback;
};

const clampDelta = (value: unknown): number => {
  const num = coerceNumber(value, 0);
  return num > 0 ? num : 0;
};

const buildBaseUsageRecord = (owner: string, repo: string, nowMs: number): KernelAuthUsageRecord => ({
  owner,
  repo,
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

const normalizeUsageRecord = (
  value: unknown,
  owner: string,
  repo: string,
  nowMs: number,
): KernelAuthUsageRecord => {
  if (!isRecord(value)) return buildBaseUsageRecord(owner, repo, nowMs);
  return {
    owner: normalizeOwnerRepo(value.owner, owner),
    repo: normalizeOwnerRepo(value.repo, repo),
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

const applyDelta = (record: KernelAuthUsageRecord, delta: KernelUsageDelta, nowMs: number): KernelAuthUsageRecord => {
  const model = delta.model === undefined ? record.last_model : normalizeLabel(delta.model);
  const route = delta.route === undefined ? record.last_route : normalizeLabel(delta.route);
  const seenAt = typeof delta.seen_at_ms === "number" && Number.isFinite(delta.seen_at_ms)
    ? Math.trunc(delta.seen_at_ms)
    : nowMs;

  return {
    owner: record.owner,
    repo: record.repo,
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

export const recordKernelUsage = async (owner: string, repo: string, delta: KernelUsageDelta): Promise<void> => {
  try {
    const kv = await kvPromise;
    if (!kv) return;

    const key = kernelUsageKey(owner, repo);
    const nowMs = Date.now();

    for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt++) {
      const entry = await kv.get<KernelAuthUsageRecord>(key);
      const current = normalizeUsageRecord(entry.value, owner, repo, nowMs);
      const updated = applyDelta(current, delta, nowMs);
      const commit = await kv.atomic().check(entry).set(key, updated).commit();
      if (commit.ok) return;
    }

    console.warn("[ai.ubq.fi] Failed to update kernel auth usage after retries:", `${owner}/${repo}`);
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to record kernel auth usage:", error);
  }
};

export const getKernelUsage = async (owner: string, repo: string): Promise<KernelAuthUsageRecord | null> => {
  try {
    const kv = await kvPromise;
    if (!kv) return null;
    const entry = await kv.get<KernelAuthUsageRecord>(kernelUsageKey(owner, repo));
    if (!entry.value) return null;
    return normalizeUsageRecord(entry.value, owner, repo, Date.now());
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to load kernel auth usage:", error);
    return null;
  }
};

const buildBaseOrgUsageRecord = (owner: string, nowMs: number): KernelOrgUsageRecord => ({
  owner,
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

const normalizeOrgUsageRecord = (value: unknown, owner: string, nowMs: number): KernelOrgUsageRecord => {
  if (!isRecord(value)) return buildBaseOrgUsageRecord(owner, nowMs);
  return {
    owner: normalizeOwnerRepo(value.owner, owner),
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

const applyOrgDelta = (record: KernelOrgUsageRecord, delta: KernelUsageDelta, nowMs: number): KernelOrgUsageRecord => {
  const model = delta.model === undefined ? record.last_model : normalizeLabel(delta.model);
  const route = delta.route === undefined ? record.last_route : normalizeLabel(delta.route);
  const seenAt = typeof delta.seen_at_ms === "number" && Number.isFinite(delta.seen_at_ms)
    ? Math.trunc(delta.seen_at_ms)
    : nowMs;

  return {
    owner: record.owner,
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

export const recordKernelOrgUsage = async (owner: string, delta: KernelUsageDelta): Promise<void> => {
  try {
    const kv = await kvPromise;
    if (!kv) return;

    const key = kernelOrgUsageKey(owner);
    const nowMs = Date.now();

    for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt++) {
      const entry = await kv.get<KernelOrgUsageRecord>(key);
      const current = normalizeOrgUsageRecord(entry.value, owner, nowMs);
      const updated = applyOrgDelta(current, delta, nowMs);
      const commit = await kv.atomic().check(entry).set(key, updated).commit();
      if (commit.ok) return;
    }

    console.warn("[ai.ubq.fi] Failed to update kernel org usage after retries:", owner);
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to record kernel org usage:", error);
  }
};

export const getKernelOrgUsage = async (owner: string): Promise<KernelOrgUsageRecord | null> => {
  try {
    const kv = await kvPromise;
    if (!kv) return null;
    const entry = await kv.get<KernelOrgUsageRecord>(kernelOrgUsageKey(owner));
    if (!entry.value) return null;
    return normalizeOrgUsageRecord(entry.value, owner, Date.now());
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to load kernel org usage:", error);
    return null;
  }
};

const buildBaseLimitRecord = (
  owner: string,
  repo: string,
  nowMs: number,
  usageLimitRequests: number,
  windowMs: number,
): KernelAuthLimitRecord => ({
  owner,
  repo,
  usage_limit_requests: usageLimitRequests,
  usage_requests: 0,
  usage_reset_at_ms: calculateNextResetMsForWindow(nowMs, windowMs),
  window_ms: windowMs,
  created_at_ms: nowMs,
  updated_at_ms: nowMs,
});

const normalizeResetAtMs = (value: unknown, nowMs: number, windowMs: number): number => {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    return calculateNextResetMsForWindow(nowMs, windowMs);
  }
  const resetAtMs = Math.trunc(value);
  if (resetAtMs <= 0) return calculateNextResetMsForWindow(nowMs, windowMs);
  return resetAtMs;
};

const normalizeLimitRecord = (
  value: unknown,
  owner: string,
  repo: string,
  nowMs: number,
  defaultLimit: number,
): KernelAuthLimitRecord => {
  if (!isRecord(value)) {
    return buildBaseLimitRecord(owner, repo, nowMs, defaultLimit, DEFAULT_KERNEL_AUTH_WINDOW_MS);
  }
  const windowMs = normalizeWindowMs(value.window_ms, DEFAULT_KERNEL_AUTH_WINDOW_MS);
  return {
    owner: normalizeOwnerRepo(value.owner, owner),
    repo: normalizeOwnerRepo(value.repo, repo),
    usage_limit_requests: normalizeUsageLimitRequests(value.usage_limit_requests, defaultLimit),
    usage_requests: Math.max(0, coerceNumber(value.usage_requests, 0)),
    usage_reset_at_ms: normalizeResetAtMs(value.usage_reset_at_ms, nowMs, windowMs),
    window_ms: windowMs,
    created_at_ms: coerceNumber(value.created_at_ms, nowMs),
    updated_at_ms: coerceNumber(value.updated_at_ms, nowMs),
  };
};

export type KernelAuthLimitSnapshot = Readonly<{
  record: KernelAuthLimitRecord;
  source: "default" | "kv";
}>;

export const getKernelUsageLimitSnapshot = async (
  owner: string,
  repo: string,
): Promise<KernelAuthLimitSnapshot | null> => {
  try {
    const kv = await kvPromise;
    if (!kv) return null;
    const nowMs = Date.now();
    const entry = await kv.get<KernelAuthLimitRecord>(kernelLimitKey(owner, repo));
    const source = entry.value ? "kv" : "default";
    const record = normalizeLimitRecord(entry.value, owner, repo, nowMs, DEFAULT_KERNEL_AUTH_USAGE_LIMIT_REQUESTS);
    return { record, source };
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to load kernel auth usage limit:", error);
    return null;
  }
};

export const listKernelUsageLimits = async (): Promise<KernelAuthLimitRecord[] | null> => {
  try {
    const kv = await kvPromise;
    if (!kv) return null;
    const nowMs = Date.now();
    const records: KernelAuthLimitRecord[] = [];
    for await (const entry of kv.list<KernelAuthLimitRecord>({ prefix: KERNEL_AUTH_LIMIT_PREFIX })) {
      const keyOwner = typeof entry.key[3] === "string" ? entry.key[3] : "";
      const keyRepo = typeof entry.key[4] === "string" ? entry.key[4] : "";
      records.push(normalizeLimitRecord(entry.value, keyOwner, keyRepo, nowMs, DEFAULT_KERNEL_AUTH_USAGE_LIMIT_REQUESTS));
    }
    records.sort((a, b) => {
      const ownerCmp = a.owner.localeCompare(b.owner);
      if (ownerCmp !== 0) return ownerCmp;
      return a.repo.localeCompare(b.repo);
    });
    return records;
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to list kernel auth usage limits:", error);
    return null;
  }
};

export const setKernelUsageLimit = async (
  owner: string,
  repo: string,
  usageLimitRequests: number,
  options: { resetUsage?: boolean; windowMs?: number } = {},
): Promise<KernelAuthLimitRecord | null> => {
  try {
    const kv = await kvPromise;
    if (!kv) return null;
    const key = kernelLimitKey(owner, repo);
    const nowMs = Date.now();
    const entry = await kv.get<KernelAuthLimitRecord>(key);
    const current = normalizeLimitRecord(entry.value, owner, repo, nowMs, DEFAULT_KERNEL_AUTH_USAGE_LIMIT_REQUESTS);
    const windowMs = options.windowMs === undefined
      ? current.window_ms
      : normalizeWindowMs(options.windowMs, current.window_ms);
    const nextResetAtMs = options.resetUsage
      ? calculateNextResetMsForWindow(nowMs, windowMs)
      : current.usage_reset_at_ms;
    const nextUsageRequests = options.resetUsage ? 0 : current.usage_requests;
    const updated: KernelAuthLimitRecord = {
      ...current,
      usage_limit_requests: normalizeUsageLimitRequests(usageLimitRequests, current.usage_limit_requests),
      usage_requests: nextUsageRequests,
      usage_reset_at_ms: nextResetAtMs,
      window_ms: windowMs,
      created_at_ms: entry.value ? current.created_at_ms : nowMs,
      updated_at_ms: nowMs,
    };
    const commit = await kv.atomic().check(entry).set(key, updated).commit();
    if (!commit.ok) return null;
    return updated;
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to update kernel auth usage limit:", error);
    return null;
  }
};

export const deleteKernelUsageLimit = async (owner: string, repo: string): Promise<boolean | null> => {
  try {
    const kv = await kvPromise;
    if (!kv) return null;
    const key = kernelLimitKey(owner, repo);
    const entry = await kv.get<KernelAuthLimitRecord>(key);
    if (!entry.value) return false;
    const commit = await kv.atomic().check(entry).delete(key).commit();
    return commit.ok;
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to delete kernel auth usage limit:", error);
    return null;
  }
};

export const checkKernelUsageLimit = async (
  owner: string,
  repo: string,
): Promise<{ ok: true } | { ok: false; response: Response }> => {
  try {
    const kv = await kvPromise;
    if (!kv) return { ok: true };
    const key = kernelLimitKey(owner, repo);
    const nowMs = Date.now();

    for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt++) {
      const entry = await kv.get<KernelAuthLimitRecord>(key);
      let record = normalizeLimitRecord(entry.value, owner, repo, nowMs, DEFAULT_KERNEL_AUTH_USAGE_LIMIT_REQUESTS);

      if (record.usage_limit_requests === 0) {
        return {
          ok: false,
          response: openaiError(
            429,
            "Kernel auth usage limit is 0; update it via /admin/kernel-usage.",
            "rate_limit_exceeded",
          ),
        };
      }

      if (shouldResetUsage(record.usage_reset_at_ms, nowMs)) {
        record = {
          ...record,
          usage_requests: 0,
          usage_reset_at_ms: calculateNextResetMsForWindow(nowMs, record.window_ms),
          updated_at_ms: nowMs,
        };
        if (entry.value) {
          const commit = await kv.atomic().check(entry).set(key, record).commit();
          if (!commit.ok) continue;
        }
      }

      if (
        record.usage_limit_requests !== API_KEY_NO_USAGE_LIMIT &&
        record.usage_requests >= record.usage_limit_requests
      ) {
        return {
          ok: false,
          response: openaiError(
            429,
            `Usage limit exceeded (${record.usage_requests}/${record.usage_limit_requests}). Resets at ${
              new Date(record.usage_reset_at_ms).toISOString()
            }`,
            "rate_limit_exceeded",
          ),
        };
      }

      return { ok: true };
    }

    console.warn("[ai.ubq.fi] Failed to refresh kernel auth usage limit after retries:", `${owner}/${repo}`);
    return { ok: true };
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to check kernel auth usage limit:", error);
    return { ok: true };
  }
};

export const incrementKernelUsageLimit = async (owner: string, repo: string): Promise<void> => {
  try {
    const kv = await kvPromise;
    if (!kv) return;
    const key = kernelLimitKey(owner, repo);
    const nowMs = Date.now();

    for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt++) {
      const entry = await kv.get<KernelAuthLimitRecord>(key);
      const current = normalizeLimitRecord(entry.value, owner, repo, nowMs, DEFAULT_KERNEL_AUTH_USAGE_LIMIT_REQUESTS);
      const resetUsage = shouldResetUsage(current.usage_reset_at_ms, nowMs);
      const updated: KernelAuthLimitRecord = {
        ...current,
        usage_requests: (resetUsage ? 0 : current.usage_requests) + 1,
        usage_reset_at_ms: resetUsage
          ? calculateNextResetMsForWindow(nowMs, current.window_ms)
          : current.usage_reset_at_ms,
        created_at_ms: entry.value ? current.created_at_ms : nowMs,
        updated_at_ms: nowMs,
      };
      const commit = await kv.atomic().check(entry).set(key, updated).commit();
      if (commit.ok) return;
    }

    console.warn("[ai.ubq.fi] Failed to increment kernel auth usage after retries:", `${owner}/${repo}`);
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to increment kernel auth usage:", error);
  }
};

const buildBaseOrgLimitRecord = (
  owner: string,
  nowMs: number,
  usageLimitRequests: number,
  windowMs: number,
): KernelOrgLimitRecord => ({
  owner,
  usage_limit_requests: usageLimitRequests,
  usage_requests: 0,
  usage_reset_at_ms: calculateNextResetMsForWindow(nowMs, windowMs),
  window_ms: windowMs,
  created_at_ms: nowMs,
  updated_at_ms: nowMs,
});

const normalizeOrgLimitRecord = (
  value: unknown,
  owner: string,
  nowMs: number,
  defaultLimit: number,
): KernelOrgLimitRecord => {
  if (!isRecord(value)) {
    return buildBaseOrgLimitRecord(owner, nowMs, defaultLimit, DEFAULT_KERNEL_AUTH_WINDOW_MS);
  }
  const windowMs = normalizeWindowMs(value.window_ms, DEFAULT_KERNEL_AUTH_WINDOW_MS);
  return {
    owner: normalizeOwnerRepo(value.owner, owner),
    usage_limit_requests: normalizeUsageLimitRequests(value.usage_limit_requests, defaultLimit),
    usage_requests: Math.max(0, coerceNumber(value.usage_requests, 0)),
    usage_reset_at_ms: normalizeResetAtMs(value.usage_reset_at_ms, nowMs, windowMs),
    window_ms: windowMs,
    created_at_ms: coerceNumber(value.created_at_ms, nowMs),
    updated_at_ms: coerceNumber(value.updated_at_ms, nowMs),
  };
};

export type KernelOrgLimitSnapshot = Readonly<{
  record: KernelOrgLimitRecord;
  source: "default" | "kv";
}>;

export const getKernelOrgUsageLimitSnapshot = async (owner: string): Promise<KernelOrgLimitSnapshot | null> => {
  try {
    const kv = await kvPromise;
    if (!kv) return null;
    const nowMs = Date.now();
    const entry = await kv.get<KernelOrgLimitRecord>(kernelOrgLimitKey(owner));
    const source = entry.value ? "kv" : "default";
    const record = normalizeOrgLimitRecord(entry.value, owner, nowMs, DEFAULT_KERNEL_AUTH_USAGE_LIMIT_REQUESTS);
    return { record, source };
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to load kernel org usage limit:", error);
    return null;
  }
};

export const listKernelOrgUsageLimits = async (): Promise<KernelOrgLimitRecord[] | null> => {
  try {
    const kv = await kvPromise;
    if (!kv) return null;
    const nowMs = Date.now();
    const records: KernelOrgLimitRecord[] = [];
    for await (const entry of kv.list<KernelOrgLimitRecord>({ prefix: KERNEL_AUTH_ORG_LIMIT_PREFIX })) {
      const keyOwner = typeof entry.key[3] === "string" ? entry.key[3] : "";
      records.push(normalizeOrgLimitRecord(entry.value, keyOwner, nowMs, DEFAULT_KERNEL_AUTH_USAGE_LIMIT_REQUESTS));
    }
    records.sort((a, b) => a.owner.localeCompare(b.owner));
    return records;
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to list kernel org usage limits:", error);
    return null;
  }
};

export const setKernelOrgUsageLimit = async (
  owner: string,
  usageLimitRequests: number,
  options: { resetUsage?: boolean; windowMs?: number } = {},
): Promise<KernelOrgLimitRecord | null> => {
  try {
    const kv = await kvPromise;
    if (!kv) return null;
    const key = kernelOrgLimitKey(owner);
    const nowMs = Date.now();
    const entry = await kv.get<KernelOrgLimitRecord>(key);
    const current = normalizeOrgLimitRecord(entry.value, owner, nowMs, DEFAULT_KERNEL_AUTH_USAGE_LIMIT_REQUESTS);
    const windowMs = options.windowMs === undefined
      ? current.window_ms
      : normalizeWindowMs(options.windowMs, current.window_ms);
    const nextResetAtMs = options.resetUsage
      ? calculateNextResetMsForWindow(nowMs, windowMs)
      : current.usage_reset_at_ms;
    const nextUsageRequests = options.resetUsage ? 0 : current.usage_requests;
    const updated: KernelOrgLimitRecord = {
      ...current,
      usage_limit_requests: normalizeUsageLimitRequests(usageLimitRequests, current.usage_limit_requests),
      usage_requests: nextUsageRequests,
      usage_reset_at_ms: nextResetAtMs,
      window_ms: windowMs,
      created_at_ms: entry.value ? current.created_at_ms : nowMs,
      updated_at_ms: nowMs,
    };
    const commit = await kv.atomic().check(entry).set(key, updated).commit();
    if (!commit.ok) return null;
    return updated;
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to update kernel org usage limit:", error);
    return null;
  }
};

export const deleteKernelOrgUsageLimit = async (owner: string): Promise<boolean | null> => {
  try {
    const kv = await kvPromise;
    if (!kv) return null;
    const key = kernelOrgLimitKey(owner);
    const entry = await kv.get<KernelOrgLimitRecord>(key);
    if (!entry.value) return false;
    const commit = await kv.atomic().check(entry).delete(key).commit();
    return commit.ok;
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to delete kernel org usage limit:", error);
    return null;
  }
};

export const checkKernelOrgUsageLimit = async (
  owner: string,
): Promise<{ ok: true } | { ok: false; response: Response }> => {
  try {
    const kv = await kvPromise;
    if (!kv) return { ok: true };
    const key = kernelOrgLimitKey(owner);
    const nowMs = Date.now();

    for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt++) {
      const entry = await kv.get<KernelOrgLimitRecord>(key);
      let record = normalizeOrgLimitRecord(entry.value, owner, nowMs, DEFAULT_KERNEL_AUTH_USAGE_LIMIT_REQUESTS);

      if (record.usage_limit_requests === 0) {
        return {
          ok: false,
          response: openaiError(
            429,
            "Kernel org usage limit is 0; update it via /admin/kernel-usage.",
            "rate_limit_exceeded",
          ),
        };
      }

      if (shouldResetUsage(record.usage_reset_at_ms, nowMs)) {
        record = {
          ...record,
          usage_requests: 0,
          usage_reset_at_ms: calculateNextResetMsForWindow(nowMs, record.window_ms),
          updated_at_ms: nowMs,
        };
        if (entry.value) {
          const commit = await kv.atomic().check(entry).set(key, record).commit();
          if (!commit.ok) continue;
        }
      }

      if (
        record.usage_limit_requests !== API_KEY_NO_USAGE_LIMIT &&
        record.usage_requests >= record.usage_limit_requests
      ) {
        return {
          ok: false,
          response: openaiError(
            429,
            `Org usage limit exceeded (${record.usage_requests}/${record.usage_limit_requests}). Resets at ${
              new Date(record.usage_reset_at_ms).toISOString()
            }`,
            "rate_limit_exceeded",
          ),
        };
      }

      return { ok: true };
    }

    console.warn("[ai.ubq.fi] Failed to refresh kernel org usage limit after retries:", owner);
    return { ok: true };
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to check kernel org usage limit:", error);
    return { ok: true };
  }
};

export const incrementKernelOrgUsageLimit = async (owner: string): Promise<void> => {
  try {
    const kv = await kvPromise;
    if (!kv) return;
    const key = kernelOrgLimitKey(owner);
    const nowMs = Date.now();

    for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt++) {
      const entry = await kv.get<KernelOrgLimitRecord>(key);
      const current = normalizeOrgLimitRecord(entry.value, owner, nowMs, DEFAULT_KERNEL_AUTH_USAGE_LIMIT_REQUESTS);
      const resetUsage = shouldResetUsage(current.usage_reset_at_ms, nowMs);
      const updated: KernelOrgLimitRecord = {
        ...current,
        usage_requests: (resetUsage ? 0 : current.usage_requests) + 1,
        usage_reset_at_ms: resetUsage
          ? calculateNextResetMsForWindow(nowMs, current.window_ms)
          : current.usage_reset_at_ms,
        created_at_ms: entry.value ? current.created_at_ms : nowMs,
        updated_at_ms: nowMs,
      };
      const commit = await kv.atomic().check(entry).set(key, updated).commit();
      if (commit.ok) return;
    }

    console.warn("[ai.ubq.fi] Failed to increment kernel org usage after retries:", owner);
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to increment kernel org usage:", error);
  }
};
