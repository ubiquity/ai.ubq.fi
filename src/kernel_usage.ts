import { getKv } from "./kv.ts";
import { getString, isRecord } from "./utils.ts";
import type {
  KernelAuthUsageDailyRecord,
  KernelAuthUsageDay,
  KernelAuthUsageRecord,
  KernelOrgUsageDailyRecord,
  KernelOrgUsageDay,
  KernelOrgUsageRecord,
} from "./types.ts";

export const KERNEL_AUTH_USAGE_PREFIX = ["ubq_ai", "kernel_auth", "usage"] as const;
export const KERNEL_AUTH_USAGE_DAILY_PREFIX = ["ubq_ai", "kernel_auth", "usage_daily"] as const;
export const KERNEL_AUTH_ORG_USAGE_PREFIX = ["ubq_ai", "kernel_auth", "org_usage"] as const;
export const KERNEL_AUTH_ORG_USAGE_DAILY_PREFIX = ["ubq_ai", "kernel_auth", "org_usage_daily"] as const;

const MAX_LABEL_LENGTH = 120;
const DAILY_SERIES_DAYS = 30;
const DAY_MS = 24 * 60 * 60 * 1000;

export const kernelUsageKey = (owner: string, repo: string) => [...KERNEL_AUTH_USAGE_PREFIX, owner, repo] as const;
export const kernelUsageDailyKey = (owner: string, repo: string) => [...KERNEL_AUTH_USAGE_DAILY_PREFIX, owner, repo] as const;
export const kernelOrgUsageKey = (owner: string) => [...KERNEL_AUTH_ORG_USAGE_PREFIX, owner] as const;
export const kernelOrgUsageDailyKey = (owner: string) => [...KERNEL_AUTH_ORG_USAGE_DAILY_PREFIX, owner] as const;

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

const normalizeUsageRecord = (value: unknown, owner: string, repo: string, nowMs: number): KernelAuthUsageRecord => {
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

const normalizeDailyUsageRecord = (value: unknown, owner: string, repo: string, nowMs: number): KernelAuthUsageDailyRecord => {
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

const normalizeDailyOrgUsageRecord = (value: unknown, owner: string, nowMs: number): KernelOrgUsageDailyRecord => {
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

const buildDailySeries = (record: { days: { day: string; request_count: number }[] }, nowMs: number, days: number): number[] => {
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

export const getKernelUsage = async (
  owner: string,
  repo: string,
  options: { includeDaily?: boolean; dailyDays?: number } = {}
): Promise<(KernelAuthUsageRecord & { daily_requests?: number[] }) | null> => {
  try {
    const kv = await getKv();
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

export const getKernelOrgUsage = async (
  owner: string,
  options: { includeDaily?: boolean; dailyDays?: number } = {}
): Promise<(KernelOrgUsageRecord & { daily_requests?: number[] }) | null> => {
  try {
    const kv = await getKv();
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

export const listKernelUsageRecords = async (
  options: { includeDaily?: boolean; dailyDays?: number } = {}
): Promise<(KernelAuthUsageRecord & { daily_requests?: number[] })[] | null> => {
  try {
    const kv = await getKv();
    if (!kv) return null;
    const nowMs = Date.now();
    const records: (KernelAuthUsageRecord & { daily_requests?: number[] })[] = [];
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

export const listKernelOrgUsageRecords = async (
  options: { includeDaily?: boolean; dailyDays?: number } = {}
): Promise<(KernelOrgUsageRecord & { daily_requests?: number[] })[] | null> => {
  try {
    const kv = await getKv();
    if (!kv) return null;
    const nowMs = Date.now();
    const records: (KernelOrgUsageRecord & { daily_requests?: number[] })[] = [];
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

// Runtime callers import the V2 split policy/window implementation through this
// module.
export {
  deleteKernelOrgUsageLimit,
  deleteKernelUsageLimit,
  getKernelOrgUsageLimitSnapshot,
  getKernelUsageLimitSnapshot,
  kernelLimitKey,
  kernelOrgLimitKey,
  listKernelOrgUsageLimits,
  listKernelUsageLimits,
  reserveEffectiveKernelUsageLimit,
  resolveKernelQuotaPolicyState,
  setKernelOrgUsageLimit,
  setKernelUsageLimit,
} from "./kernel_quota_v2.ts";
export type { KernelQuotaReservation } from "./kernel_quota_v2.ts";
