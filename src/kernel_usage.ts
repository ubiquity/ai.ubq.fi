import { API_KEY_NO_USAGE_LIMIT, USAGE_RESET_PERIOD_MS, shouldResetUsage } from "./api_keys.ts";
import { openaiError } from "./http.ts";
import { kvPromise } from "./kv.ts";
import { getString, isRecord } from "./utils.ts";
import type {
  KernelAuthLimitRecord,
  KernelAuthUsageDailyRecord,
  KernelAuthUsageDay,
  KernelAuthUsageRecord,
  KernelOrgLimitRecord,
  KernelOrgUsageDailyRecord,
  KernelOrgUsageDay,
  KernelOrgUsageRecord,
} from "./types.ts";

export const KERNEL_AUTH_USAGE_PREFIX = ["ubq_ai", "kernel_auth", "usage"] as const;
export const KERNEL_AUTH_USAGE_DAILY_PREFIX = ["ubq_ai", "kernel_auth", "usage_daily"] as const;
export const KERNEL_AUTH_LIMIT_PREFIX = ["ubq_ai", "kernel_auth", "limits"] as const;
export const KERNEL_AUTH_ORG_USAGE_PREFIX = ["ubq_ai", "kernel_auth", "org_usage"] as const;
export const KERNEL_AUTH_ORG_USAGE_DAILY_PREFIX = ["ubq_ai", "kernel_auth", "org_usage_daily"] as const;
export const KERNEL_AUTH_ORG_LIMIT_PREFIX = ["ubq_ai", "kernel_auth", "org_limits"] as const;

const MAX_LABEL_LENGTH = 120;
const MAX_KV_RETRIES = 3;
const DEFAULT_KERNEL_AUTH_WINDOW_MS = USAGE_RESET_PERIOD_MS;
const DAILY_SERIES_DAYS = 30;
const DAILY_HISTORY_DAYS = 30;
const DAY_MS = 24 * 60 * 60 * 1000;

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
export const kernelUsageDailyKey = (owner: string, repo: string) => [...KERNEL_AUTH_USAGE_DAILY_PREFIX, owner, repo] as const;
export const kernelLimitKey = (owner: string, repo: string) => [...KERNEL_AUTH_LIMIT_PREFIX, owner, repo] as const;
export const kernelOrgUsageKey = (owner: string) => [...KERNEL_AUTH_ORG_USAGE_PREFIX, owner] as const;
export const kernelOrgUsageDailyKey = (owner: string) => [...KERNEL_AUTH_ORG_USAGE_DAILY_PREFIX, owner] as const;
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
  reasoning?: string | null;
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
  last_reasoning: null,
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
    last_reasoning: normalizeLabel(value.last_reasoning),
    last_route: normalizeLabel(value.last_route),
  };
};

const normalizeDailyUsageDay = (value: unknown): KernelAuthUsageDay | null => {
  if (!isRecord(value)) return null;
  const day = typeof value.day === "string" ? value.day.trim() : "";
  if (!day) return null;
  if (dayKeyToMs(day) === null) return null;
  const requestCount = Math.max(0, coerceNumber(value.request_count, 0));
  return { day, request_count: requestCount };
};

const normalizeDailyUsageRecord = (
  value: unknown,
  owner: string,
  repo: string,
  nowMs: number,
): KernelAuthUsageDailyRecord => {
  if (!isRecord(value)) return { owner, repo, days: [], updated_at_ms: nowMs };
  const daysRaw = Array.isArray(value.days) ? value.days : [];
  const days: KernelAuthUsageDay[] = [];
  for (const item of daysRaw) {
    const normalized = normalizeDailyUsageDay(item);
    if (normalized) days.push(normalized);
  }
  return {
    owner: normalizeOwnerRepo(value.owner, owner),
    repo: normalizeOwnerRepo(value.repo, repo),
    days,
    updated_at_ms: coerceNumber(value.updated_at_ms, nowMs),
  };
};

const normalizeDailyOrgUsageRecord = (
  value: unknown,
  owner: string,
  nowMs: number,
): KernelOrgUsageDailyRecord => {
  if (!isRecord(value)) return { owner, days: [], updated_at_ms: nowMs };
  const daysRaw = Array.isArray(value.days) ? value.days : [];
  const days: KernelOrgUsageDay[] = [];
  for (const item of daysRaw) {
    const normalized = normalizeDailyUsageDay(item);
    if (normalized) days.push(normalized);
  }
  return {
    owner: normalizeOwnerRepo(value.owner, owner),
    days,
    updated_at_ms: coerceNumber(value.updated_at_ms, nowMs),
  };
};

const pruneDailyUsageDays = <T extends { day: string; request_count: number }>(
  days: T[],
  nowMs: number,
): T[] => {
  const cutoffMs = startOfDayUtcMs(nowMs) - (DAILY_HISTORY_DAYS - 1) * DAY_MS;
  return days
    .filter((entry) => {
      const dayMs = dayKeyToMs(entry.day);
      return dayMs !== null && dayMs >= cutoffMs;
    })
    .sort((a, b) => a.day.localeCompare(b.day));
};

const buildDailySeries = (
  record: { days: Array<{ day: string; request_count: number }> },
  nowMs: number,
  days: number,
): number[] => {
  const seriesDays = Math.max(1, Math.trunc(days));
  const startMs = startOfDayUtcMs(nowMs) - (seriesDays - 1) * DAY_MS;
  const countsByDay = new Map<string, number>();
  for (const entry of record.days) {
    countsByDay.set(entry.day, entry.request_count);
  }
  const series: number[] = [];
  for (let i = 0; i < seriesDays; i += 1) {
    const dayMs = startMs + i * DAY_MS;
    const dayKey = dayKeyFromMs(dayMs);
    series.push(countsByDay.get(dayKey) ?? 0);
  }
  return series;
};

const applyDelta = (record: KernelAuthUsageRecord, delta: KernelUsageDelta, nowMs: number): KernelAuthUsageRecord => {
  const model = delta.model === undefined ? record.last_model : normalizeLabel(delta.model);
  const reasoning = delta.reasoning === undefined ? record.last_reasoning : normalizeLabel(delta.reasoning);
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
    last_reasoning: reasoning,
    last_route: route,
  };
};

export const recordKernelUsage = async (owner: string, repo: string, delta: KernelUsageDelta): Promise<void> => {
  try {
    const kv = await kvPromise;
    if (!kv) return;

    const key = kernelUsageKey(owner, repo);
    const nowMs = Date.now();
    let usageUpdated = false;

    for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt++) {
      const entry = await kv.get<KernelAuthUsageRecord>(key);
      const current = normalizeUsageRecord(entry.value, owner, repo, nowMs);
      const updated = applyDelta(current, delta, nowMs);
      const commit = await kv.atomic().check(entry).set(key, updated).commit();
      if (commit.ok) {
        usageUpdated = true;
        break;
      }
    }

    if (!usageUpdated) {
      console.warn("[ai.ubq.fi] Failed to update kernel auth usage after retries:", `${owner}/${repo}`);
      return;
    }

    const requestCount = clampDelta(delta.request_count);
    if (requestCount > 0) {
      const seenAt = typeof delta.seen_at_ms === "number" && Number.isFinite(delta.seen_at_ms)
        ? Math.trunc(delta.seen_at_ms)
        : nowMs;
      const dayKey = dayKeyFromMs(seenAt);
      for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt++) {
        const entry = await kv.get<KernelAuthUsageDailyRecord>(kernelUsageDailyKey(owner, repo));
        const current = normalizeDailyUsageRecord(entry.value, owner, repo, nowMs);
        const nextDays = [...current.days];
        const existing = nextDays.find((item) => item.day === dayKey);
        if (existing) {
          existing.request_count += requestCount;
        } else {
          nextDays.push({ day: dayKey, request_count: requestCount });
        }
        const updated: KernelAuthUsageDailyRecord = {
          owner,
          repo,
          days: pruneDailyUsageDays(nextDays, nowMs),
          updated_at_ms: nowMs,
        };
        const commit = await kv.atomic().check(entry).set(kernelUsageDailyKey(owner, repo), updated).commit();
        if (commit.ok) break;
      }
    }
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to record kernel auth usage:", error);
  }
};

export const getKernelUsage = async (
  owner: string,
  repo: string,
  options: { includeDaily?: boolean; dailyDays?: number } = {},
): Promise<(KernelAuthUsageRecord & { daily_requests?: number[] }) | null> => {
  try {
    const kv = await kvPromise;
    if (!kv) return null;
    const nowMs = Date.now();
    const entry = await kv.get<KernelAuthUsageRecord>(kernelUsageKey(owner, repo));
    if (!entry.value) return null;
    const usage = normalizeUsageRecord(entry.value, owner, repo, nowMs);
    if (!options.includeDaily) return usage;
    const dailyEntry = await kv.get<KernelAuthUsageDailyRecord>(kernelUsageDailyKey(owner, repo));
    const dailyRecord = normalizeDailyUsageRecord(dailyEntry.value, owner, repo, nowMs);
    return {
      ...usage,
      daily_requests: buildDailySeries(dailyRecord, nowMs, options.dailyDays ?? DAILY_SERIES_DAYS),
    };
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
  last_reasoning: null,
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
    last_reasoning: normalizeLabel(value.last_reasoning),
    last_route: normalizeLabel(value.last_route),
  };
};

const applyOrgDelta = (record: KernelOrgUsageRecord, delta: KernelUsageDelta, nowMs: number): KernelOrgUsageRecord => {
  const model = delta.model === undefined ? record.last_model : normalizeLabel(delta.model);
  const reasoning = delta.reasoning === undefined ? record.last_reasoning : normalizeLabel(delta.reasoning);
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
    last_reasoning: reasoning,
    last_route: route,
  };
};

export const recordKernelOrgUsage = async (owner: string, delta: KernelUsageDelta): Promise<void> => {
  try {
    const kv = await kvPromise;
    if (!kv) return;

    const key = kernelOrgUsageKey(owner);
    const nowMs = Date.now();
    let usageUpdated = false;

    for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt++) {
      const entry = await kv.get<KernelOrgUsageRecord>(key);
      const current = normalizeOrgUsageRecord(entry.value, owner, nowMs);
      const updated = applyOrgDelta(current, delta, nowMs);
      const commit = await kv.atomic().check(entry).set(key, updated).commit();
      if (commit.ok) {
        usageUpdated = true;
        break;
      }
    }

    if (!usageUpdated) {
      console.warn("[ai.ubq.fi] Failed to update kernel org usage after retries:", owner);
      return;
    }

    const requestCount = clampDelta(delta.request_count);
    if (requestCount > 0) {
      const seenAt = typeof delta.seen_at_ms === "number" && Number.isFinite(delta.seen_at_ms)
        ? Math.trunc(delta.seen_at_ms)
        : nowMs;
      const dayKey = dayKeyFromMs(seenAt);
      for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt++) {
        const entry = await kv.get<KernelOrgUsageDailyRecord>(kernelOrgUsageDailyKey(owner));
        const current = normalizeDailyOrgUsageRecord(entry.value, owner, nowMs);
        const nextDays = [...current.days];
        const existing = nextDays.find((item) => item.day === dayKey);
        if (existing) {
          existing.request_count += requestCount;
        } else {
          nextDays.push({ day: dayKey, request_count: requestCount });
        }
        const updated: KernelOrgUsageDailyRecord = {
          owner,
          days: pruneDailyUsageDays(nextDays, nowMs),
          updated_at_ms: nowMs,
        };
        const commit = await kv.atomic().check(entry).set(kernelOrgUsageDailyKey(owner), updated).commit();
        if (commit.ok) break;
      }
    }
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to record kernel org usage:", error);
  }
};

export const getKernelOrgUsage = async (
  owner: string,
  options: { includeDaily?: boolean; dailyDays?: number } = {},
): Promise<(KernelOrgUsageRecord & { daily_requests?: number[] }) | null> => {
  try {
    const kv = await kvPromise;
    if (!kv) return null;
    const nowMs = Date.now();
    const entry = await kv.get<KernelOrgUsageRecord>(kernelOrgUsageKey(owner));
    if (!entry.value) return null;
    const usage = normalizeOrgUsageRecord(entry.value, owner, nowMs);
    if (!options.includeDaily) return usage;
    const dailyEntry = await kv.get<KernelOrgUsageDailyRecord>(kernelOrgUsageDailyKey(owner));
    const dailyRecord = normalizeDailyOrgUsageRecord(dailyEntry.value, owner, nowMs);
    return {
      ...usage,
      daily_requests: buildDailySeries(dailyRecord, nowMs, options.dailyDays ?? DAILY_SERIES_DAYS),
    };
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

export const listKernelUsageRecords = async (
  options: { includeDaily?: boolean; dailyDays?: number } = {},
): Promise<(KernelAuthUsageRecord & { daily_requests?: number[] })[] | null> => {
  try {
    const kv = await kvPromise;
    if (!kv) return null;
    const nowMs = Date.now();
    const records: Array<KernelAuthUsageRecord & { daily_requests?: number[] }> = [];
    for await (const entry of kv.list<KernelAuthUsageRecord>({ prefix: KERNEL_AUTH_USAGE_PREFIX })) {
      const keyOwner = typeof entry.key[3] === "string" ? entry.key[3] : "";
      const keyRepo = typeof entry.key[4] === "string" ? entry.key[4] : "";
      const usage = normalizeUsageRecord(entry.value, keyOwner, keyRepo, nowMs);
      if (options.includeDaily) {
        const dailyEntry = await kv.get<KernelAuthUsageDailyRecord>(kernelUsageDailyKey(usage.owner, usage.repo));
        const dailyRecord = normalizeDailyUsageRecord(dailyEntry.value, usage.owner, usage.repo, nowMs);
        records.push({
          ...usage,
          daily_requests: buildDailySeries(dailyRecord, nowMs, options.dailyDays ?? DAILY_SERIES_DAYS),
        });
      } else {
        records.push(usage);
      }
    }
    records.sort((a, b) => {
      const ownerCmp = a.owner.localeCompare(b.owner);
      if (ownerCmp !== 0) return ownerCmp;
      return a.repo.localeCompare(b.repo);
    });
    return records;
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to list kernel auth usage records:", error);
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
    const nextResetAtMs = calculateNextResetMsForWindow(nowMs, windowMs);
    const nextUsageRequests = 0;
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

export const listKernelOrgUsageRecords = async (
  options: { includeDaily?: boolean; dailyDays?: number } = {},
): Promise<(KernelOrgUsageRecord & { daily_requests?: number[] })[] | null> => {
  try {
    const kv = await kvPromise;
    if (!kv) return null;
    const nowMs = Date.now();
    const records: Array<KernelOrgUsageRecord & { daily_requests?: number[] }> = [];
    for await (const entry of kv.list<KernelOrgUsageRecord>({ prefix: KERNEL_AUTH_ORG_USAGE_PREFIX })) {
      const keyOwner = typeof entry.key[3] === "string" ? entry.key[3] : "";
      const usage = normalizeOrgUsageRecord(entry.value, keyOwner, nowMs);
      if (options.includeDaily) {
        const dailyEntry = await kv.get<KernelOrgUsageDailyRecord>(kernelOrgUsageDailyKey(usage.owner));
        const dailyRecord = normalizeDailyOrgUsageRecord(dailyEntry.value, usage.owner, nowMs);
        records.push({
          ...usage,
          daily_requests: buildDailySeries(dailyRecord, nowMs, options.dailyDays ?? DAILY_SERIES_DAYS),
        });
      } else {
        records.push(usage);
      }
    }
    records.sort((a, b) => a.owner.localeCompare(b.owner));
    return records;
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to list kernel org usage records:", error);
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
    const nextResetAtMs = calculateNextResetMsForWindow(nowMs, windowMs);
    const nextUsageRequests = 0;
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
