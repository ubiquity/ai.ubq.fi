import { config } from "./config.ts";
import { type CodexCapacityAccount, getCodexCapacityAccounts } from "./codex.ts";
import { json } from "./http.ts";
import { getKv } from "./kv.ts";
import {
  type CodexCapacityRoutingObservationInput,
  recordCodexCapacityRoutingObservations,
} from "./codex_account_routing.ts";
import { listProviderCapacityResetEvents, type ProviderCapacityResetEvent } from "./provider_capacity_events.ts";
import { PROVIDER_CAPACITY_SNAPSHOT_KEY } from "./provider_capacity_contract.ts";
import { getConfiguredYunwuQuotaSnapshot, YUNWU_QUOTA_FRESH_MS, type YunwuQuotaSnapshot } from "./yunwu_quota.ts";
import { isRecord } from "./utils.ts";

export { PROVIDER_CAPACITY_SNAPSHOT_KEY } from "./provider_capacity_contract.ts";
export const PROVIDER_CAPACITY_LEASE_KEY = ["uos_ai", "provider_capacity", "v1", "lease"] as const;
export const PROVIDER_CAPACITY_HISTORY_KEY_PREFIX = ["uos_ai", "provider_capacity", "v1", "history"] as const;
export const PROVIDER_CAPACITY_HISTORY_BUCKET_MS = 15 * 60_000;
export const PROVIDER_CAPACITY_HISTORY_RETENTION_MS = 7 * 24 * 60 * 60_000;
// Keep this name for callers that used the old snapshot retention constant.
export const PROVIDER_CAPACITY_SNAPSHOT_RETENTION_MS = PROVIDER_CAPACITY_HISTORY_RETENTION_MS;
export { PROVIDER_CAPACITY_RESET_EVENT_RETENTION_MS } from "./provider_capacity_events.ts";
export const PROVIDER_CAPACITY_LEASE_MS = 10_000;
export const PROVIDER_CAPACITY_COLD_WAIT_MS = 2_000;
export const PROVIDER_CAPACITY_SOURCE_STALE_MS = 30 * 60_000;
export const PROVIDER_CAPACITY_CODEX_TIMEOUT_MS = 8_000;

const ADDITIONAL_WINDOW_UNANCHORED_TOLERANCE_MS = 60_000;

type CapacityState = "available" | "stale" | "unavailable";
export type ProviderCapacityViewState = "live" | "persisted" | "stale" | "unavailable";

export type ProviderCapacityWindow = Readonly<{
  limit_window_seconds: number | null;
  used_percent: number | null;
  reset_at_ms: number | null;
}>;

export type ProviderCapacityAdditionalRateLimit = Readonly<{
  limit_name: string;
  metered_feature: string | null;
  windows: Readonly<{
    primary: ProviderCapacityWindow | null;
    secondary: ProviderCapacityWindow | null;
  }>;
}>;

export type ProviderCapacitySource =
  | Readonly<{
    source: "codex";
    label: string;
    slot: 1 | 2;
    state: CapacityState;
    source_observed_at_ms: number | null;
    snapshot_at_ms: number;
    windows: Readonly<{
      primary: ProviderCapacityWindow | null;
      secondary: ProviderCapacityWindow | null;
    }>;
    additional_rate_limits: readonly ProviderCapacityAdditionalRateLimit[];
  }>
  | Readonly<{
    source: "yunwu";
    label: "YunWu fallback";
    state: CapacityState;
    source_observed_at_ms: number | null;
    snapshot_at_ms: number;
    wallet: Readonly<{
      balance_credits: number | null;
      baseline_credits: number | null;
      refill_cycle_remaining_percent: number | null;
      refill_cycle_used_percent: number | null;
      cycle_started_at_ms: number | null;
      last_credit_at_ms: number | null;
      confidence: YunwuQuotaSnapshot["state"]["confidence"] | null;
      cache_state: YunwuQuotaSnapshot["cache_state"] | null;
      reset_at_ms: null;
    }>;
  }>;

export type ProviderCapacityCodexSource = Extract<ProviderCapacitySource, { source: "codex" }>;
export type ProviderCapacityYunwuSource = Extract<ProviderCapacitySource, { source: "yunwu" }>;

export type ProviderCapacitySnapshot = Readonly<{
  snapshot_at_ms: number;
  stale_after_ms: number;
  sources: readonly [ProviderCapacitySource, ProviderCapacitySource, ProviderCapacitySource];
}>;

export type ProviderCapacityHistoryPoint = Readonly<{
  bucket_start_at_ms: number;
  sampled_at_ms: number;
  sources: readonly [ProviderCapacityCodexSource, ProviderCapacityCodexSource, ProviderCapacityYunwuSource];
}>;

export type ProviderCapacityView = Readonly<
  ProviderCapacitySnapshot & {
    cache_state: ProviderCapacityViewState;
    history: readonly ProviderCapacityHistoryPoint[];
    reset_events: readonly ProviderCapacityResetEvent[];
  }
>;

export type ProviderCapacityFetch = (
  input: RequestInfo | URL,
  init?: RequestInit,
) => Promise<Response>;

export type ProviderCapacitySnapshotOptions = Readonly<{
  kv?: Deno.Kv | null;
  fetcher?: ProviderCapacityFetch;
  now?: () => number;
  signal?: AbortSignal;
  createLeaseOwner?: () => string;
}>;

type CapacityLease = Readonly<{
  owner: string;
  lease_until_ms: number;
}>;

type StoredHistoryPoint = ProviderCapacityHistoryPoint;

const capacityState = (value: unknown): CapacityState | null =>
  value === "available" || value === "stale" || value === "unavailable" ? value : null;

const safeNow = (now: () => number): number => {
  const value = Math.trunc(now());
  return Number.isSafeInteger(value) && value >= 0 ? value : Date.now();
};

const isSafeTimestamp = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const codexUsageUrl = (): string => new URL("/backend-api/wham/usage", config.codexBaseUrl).toString();

const parsePercent = (value: unknown): number | null =>
  typeof value === "number" && Number.isFinite(value) && value >= 0 && value <= 100 ? value : null;

const parseWindowSeconds = (value: unknown): number | null =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0 ? value : null;

const parseResetAtMs = (value: unknown): number | null => {
  if (typeof value !== "number" || !Number.isSafeInteger(value) || value < 0) return null;
  const milliseconds = value * 1_000;
  return Number.isSafeInteger(milliseconds) ? milliseconds : null;
};

const parseCodexWindow = (value: unknown): ProviderCapacityWindow | null => {
  if (!isRecord(value)) return null;
  const limitWindowSeconds = parseWindowSeconds(value.limit_window_seconds);
  const usedPercent = parsePercent(value.used_percent);
  const resetAtMs = parseResetAtMs(value.reset_at);
  if (limitWindowSeconds === null && usedPercent === null && resetAtMs === null) return null;
  return {
    limit_window_seconds: limitWindowSeconds,
    used_percent: usedPercent,
    reset_at_ms: resetAtMs,
  };
};

const isUnanchoredAdditionalWindow = (window: ProviderCapacityWindow | null, snapshotAtMs: number): boolean => {
  if (
    !window ||
    window.used_percent !== 0 ||
    window.limit_window_seconds === null ||
    window.reset_at_ms === null
  ) return false;
  const fullWindowMs = window.limit_window_seconds * 1_000;
  if (!Number.isSafeInteger(fullWindowMs)) return false;
  const expectedResetAtMs = snapshotAtMs + fullWindowMs;
  return Number.isSafeInteger(expectedResetAtMs) &&
    Math.abs(window.reset_at_ms - expectedResetAtMs) <= ADDITIONAL_WINDOW_UNANCHORED_TOLERANCE_MS;
};

const parseCodexAdditionalRateLimit = (
  value: unknown,
  snapshotAtMs: number,
): ProviderCapacityAdditionalRateLimit | null => {
  if (!isRecord(value)) return null;
  const limitName = typeof value.limit_name === "string" ? value.limit_name.trim() : "";
  if (!limitName) return null;
  const meteredFeature = typeof value.metered_feature === "string" ? value.metered_feature.trim() || null : null;
  const rateLimit = isRecord(value.rate_limit) ? value.rate_limit : null;
  const parsedPrimary = parseCodexWindow(rateLimit?.primary_window);
  const parsedSecondary = parseCodexWindow(rateLimit?.secondary_window);
  const primary = isUnanchoredAdditionalWindow(parsedPrimary, snapshotAtMs) ? null : parsedPrimary;
  const secondary = isUnanchoredAdditionalWindow(parsedSecondary, snapshotAtMs) ? null : parsedSecondary;
  if (!primary && !secondary) return null;
  return {
    limit_name: limitName,
    metered_feature: meteredFeature,
    windows: {
      primary,
      secondary,
    },
  };
};

const parseCodexUsage = (value: unknown, snapshotAtMs: number):
  | Readonly<{
    primary: ProviderCapacityWindow | null;
    secondary: ProviderCapacityWindow | null;
    additional_rate_limits: readonly ProviderCapacityAdditionalRateLimit[];
  }>
  | null => {
  if (!isRecord(value) || !isRecord(value.rate_limit)) return null;
  const additionalRateLimits = Array.isArray(value.additional_rate_limits)
    ? value.additional_rate_limits.flatMap((candidate) => {
      const parsed = parseCodexAdditionalRateLimit(candidate, snapshotAtMs);
      return parsed ? [parsed] : [];
    })
    : [];
  return {
    primary: parseCodexWindow(value.rate_limit.primary_window),
    secondary: parseCodexWindow(value.rate_limit.secondary_window),
    additional_rate_limits: additionalRateLimits,
  };
};

const emptyWindows = (): Readonly<{
  primary: null;
  secondary: null;
}> => ({ primary: null, secondary: null });

const unavailableCodexSource = (slot: 1 | 2, snapshotAtMs: number): ProviderCapacityCodexSource => ({
  source: "codex",
  label: "Codex account " + slot,
  slot,
  state: "unavailable",
  source_observed_at_ms: null,
  snapshot_at_ms: snapshotAtMs,
  windows: emptyWindows(),
  additional_rate_limits: [],
});

const unavailableYunwuSource = (snapshotAtMs: number): ProviderCapacityYunwuSource => ({
  source: "yunwu",
  label: "YunWu fallback",
  state: "unavailable",
  source_observed_at_ms: null,
  snapshot_at_ms: snapshotAtMs,
  wallet: {
    balance_credits: null,
    baseline_credits: null,
    refill_cycle_remaining_percent: null,
    refill_cycle_used_percent: null,
    cycle_started_at_ms: null,
    last_credit_at_ms: null,
    confidence: null,
    cache_state: null,
    reset_at_ms: null,
  },
});

const cancelResponseBody = (response: Response): void => {
  try {
    const cancellation = response.body?.cancel();
    if (cancellation) void Promise.resolve(cancellation).catch(() => {});
  } catch {
    // The capacity response is already classified as unavailable.
  }
};

const fetchCodexCapacitySource = async (
  account: CodexCapacityAccount,
  snapshotAtMs: number,
  fetcher: ProviderCapacityFetch,
  signal: AbortSignal,
): Promise<ProviderCapacityCodexSource> => {
  const headers = new Headers({
    Accept: "application/json",
    Authorization: "Bearer " + account.access_token,
    "ChatGPT-Account-ID": account.account_id,
    "User-Agent": "codex_cli_rs/0.100.0 (ai.ubq.fi)",
  });
  try {
    const response = await fetcher(codexUsageUrl(), {
      method: "GET",
      headers,
      redirect: "manual",
      signal,
    });
    if (!response.ok) {
      cancelResponseBody(response);
      return unavailableCodexSource(account.slot as 1 | 2, snapshotAtMs);
    }
    let payload: unknown;
    try {
      payload = await response.json();
    } catch {
      return unavailableCodexSource(account.slot as 1 | 2, snapshotAtMs);
    }
    const windows = parseCodexUsage(payload, snapshotAtMs);
    if (!windows) return unavailableCodexSource(account.slot as 1 | 2, snapshotAtMs);
    return {
      source: "codex",
      label: "Codex account " + account.slot,
      slot: account.slot as 1 | 2,
      state: "available",
      source_observed_at_ms: snapshotAtMs,
      snapshot_at_ms: snapshotAtMs,
      windows: {
        primary: windows.primary,
        secondary: windows.secondary,
      },
      additional_rate_limits: windows.additional_rate_limits,
    };
  } catch {
    return unavailableCodexSource(account.slot as 1 | 2, snapshotAtMs);
  }
};

const yunwuCapacitySource = (
  snapshot: YunwuQuotaSnapshot | null,
  snapshotAtMs: number,
): ProviderCapacitySource => {
  if (!snapshot) return unavailableYunwuSource(snapshotAtMs);
  const sourceObservedAtMs = snapshot.state.observed_at_ms;
  const stale = snapshot.cache_state === "stale" || snapshotAtMs - sourceObservedAtMs >= YUNWU_QUOTA_FRESH_MS;
  return {
    source: "yunwu",
    label: "YunWu fallback",
    state: stale ? "stale" : "available",
    source_observed_at_ms: sourceObservedAtMs,
    snapshot_at_ms: snapshotAtMs,
    wallet: {
      balance_credits: snapshot.balance_credits,
      baseline_credits: snapshot.baseline_credits,
      refill_cycle_remaining_percent: snapshot.remaining_percent,
      refill_cycle_used_percent: snapshot.used_percent,
      cycle_started_at_ms: snapshot.state.cycle_started_at_ms,
      last_credit_at_ms: snapshot.state.last_credit_at_ms,
      confidence: snapshot.state.confidence,
      cache_state: snapshot.cache_state,
      // YunWu exposes a refill cycle, not a scheduled reset window.
      reset_at_ms: null,
    },
  };
};

const captureProviderCapacitySnapshot = async (
  options: ProviderCapacitySnapshotOptions,
  snapshotAtMs: number,
  kv: Deno.Kv | null,
): Promise<ProviderCapacitySnapshot> => {
  let accounts: readonly CodexCapacityAccount[] = [];
  try {
    accounts = await getCodexCapacityAccounts();
  } catch {
    // Missing or malformed auth is represented by redacted unavailable slots.
  }

  const fetcher = options.fetcher ?? ((input: RequestInfo | URL, init?: RequestInit) => globalThis.fetch(input, init));
  const timeout = AbortSignal.timeout(PROVIDER_CAPACITY_CODEX_TIMEOUT_MS);
  const signal = options.signal ? AbortSignal.any([options.signal, timeout]) : timeout;
  const codexPromise = Promise.all(([1, 2] as const).map(async (slot) => {
    const account = accounts.find((candidate) => candidate.slot === slot);
    return account
      ? await fetchCodexCapacitySource(account, snapshotAtMs, fetcher, signal)
      : unavailableCodexSource(slot, snapshotAtMs);
  }));
  const yunwuPromise = getConfiguredYunwuQuotaSnapshot({
    kv,
    fetcher,
    now: () => snapshotAtMs,
    signal,
    forceRefresh: true,
    createLeaseOwner: options.createLeaseOwner,
  }).catch(() => null);
  const [codexSources, yunwuSnapshot] = await Promise.all([codexPromise, yunwuPromise]);

  const routingObservations: CodexCapacityRoutingObservationInput[] = [];
  for (const account of accounts) {
    const source = codexSources.find((candidate) => candidate.slot === account.slot);
    if (!source) continue;
    routingObservations.push({
      slot: account.slot - 1,
      account_id: account.account_id,
      state: source.state,
      source_observed_at_ms: source.source_observed_at_ms,
      snapshot_at_ms: source.snapshot_at_ms,
      windows: source.windows,
      additional_rate_limits: source.additional_rate_limits,
    });
  }
  await recordCodexCapacityRoutingObservations(routingObservations, snapshotAtMs);

  return {
    snapshot_at_ms: snapshotAtMs,
    stale_after_ms: PROVIDER_CAPACITY_SOURCE_STALE_MS,
    sources: [codexSources[0], codexSources[1], yunwuCapacitySource(yunwuSnapshot, snapshotAtMs)],
  };
};

const isStoredWindow = (value: unknown): value is ProviderCapacityWindow => {
  if (!isRecord(value)) return false;
  return (value.limit_window_seconds === null || parseWindowSeconds(value.limit_window_seconds) !== null) &&
    (value.used_percent === null || parsePercent(value.used_percent) !== null) &&
    (value.reset_at_ms === null || isSafeTimestamp(value.reset_at_ms));
};

const readStoredWindow = (value: unknown): ProviderCapacityWindow | null =>
  isStoredWindow(value)
    ? {
      limit_window_seconds: value.limit_window_seconds,
      used_percent: value.used_percent,
      reset_at_ms: value.reset_at_ms,
    }
    : null;

const readStoredAdditionalRateLimit = (value: unknown): ProviderCapacityAdditionalRateLimit | null => {
  if (!isRecord(value) || typeof value.limit_name !== "string" || !value.limit_name.trim()) return null;
  const windows = isRecord(value.windows) ? value.windows : null;
  if (!windows) return null;
  const meteredFeature = value.metered_feature;
  if (meteredFeature !== null && typeof meteredFeature !== "string") return null;
  return {
    limit_name: value.limit_name.trim(),
    metered_feature: typeof meteredFeature === "string" ? meteredFeature.trim() || null : null,
    windows: {
      primary: readStoredWindow(windows.primary),
      secondary: readStoredWindow(windows.secondary),
    },
  };
};

const readStoredAdditionalRateLimits = (value: unknown): readonly ProviderCapacityAdditionalRateLimit[] =>
  Array.isArray(value)
    ? value.flatMap((candidate) => {
      const parsed = readStoredAdditionalRateLimit(candidate);
      return parsed ? [parsed] : [];
    })
    : [];

const readStoredCodexSource = (
  value: unknown,
  fallbackSnapshotAtMs: number,
): ProviderCapacityCodexSource | null => {
  if (!isRecord(value) || value.source !== "codex" || (value.slot !== 1 && value.slot !== 2)) return null;
  const state = capacityState(value.state);
  const observed = value.source_observed_at_ms;
  const snapshotAtMs = value.snapshot_at_ms === undefined ? fallbackSnapshotAtMs : value.snapshot_at_ms;
  if (!state || !(observed === null || isSafeTimestamp(observed)) || !isSafeTimestamp(snapshotAtMs)) return null;
  const windows = isRecord(value.windows) ? value.windows : null;
  if (!windows) return null;
  return {
    source: "codex",
    label: "Codex account " + value.slot,
    slot: value.slot,
    state,
    source_observed_at_ms: observed,
    snapshot_at_ms: snapshotAtMs,
    windows: {
      primary: readStoredWindow(windows.primary),
      secondary: readStoredWindow(windows.secondary),
    },
    additional_rate_limits: readStoredAdditionalRateLimits(value.additional_rate_limits),
  };
};

const readStoredYunwuSource = (
  value: unknown,
  snapshotAtMs: number,
): ProviderCapacityYunwuSource | null => {
  if (!isRecord(value) || value.source !== "yunwu") return null;
  const state = capacityState(value.state);
  const observed = value.source_observed_at_ms;
  const wallet = isRecord(value.wallet) ? value.wallet : null;
  if (!state || !wallet || !(observed === null || isSafeTimestamp(observed))) return null;
  const optionalNumber = (candidate: unknown): number | null =>
    candidate === null ? null : typeof candidate === "number" && Number.isFinite(candidate) ? candidate : null;
  const optionalTimestamp = (candidate: unknown): number | null =>
    candidate === null ? null : isSafeTimestamp(candidate) ? candidate : null;
  const confidence = wallet.confidence === null || wallet.confidence === "provisional" ||
      wallet.confidence === "refill_observed" || wallet.confidence === "inferred_adjustment"
    ? wallet.confidence
    : null;
  const cacheState =
    wallet.cache_state === null || wallet.cache_state === "fresh" || wallet.cache_state === "refreshed" ||
      wallet.cache_state === "stale" || wallet.cache_state === "wait"
      ? wallet.cache_state
      : null;
  return {
    source: "yunwu",
    label: "YunWu fallback",
    state,
    source_observed_at_ms: observed,
    snapshot_at_ms: snapshotAtMs,
    wallet: {
      balance_credits: optionalNumber(wallet.balance_credits),
      baseline_credits: optionalNumber(wallet.baseline_credits),
      refill_cycle_remaining_percent: optionalNumber(wallet.refill_cycle_remaining_percent),
      refill_cycle_used_percent: optionalNumber(wallet.refill_cycle_used_percent),
      cycle_started_at_ms: optionalTimestamp(wallet.cycle_started_at_ms),
      last_credit_at_ms: optionalTimestamp(wallet.last_credit_at_ms),
      confidence,
      cache_state: cacheState,
      reset_at_ms: null,
    },
  };
};

const readStoredSnapshot = (value: unknown): ProviderCapacitySnapshot | null => {
  if (!isRecord(value) || !isSafeTimestamp(value.snapshot_at_ms) || !Array.isArray(value.sources)) return null;
  const snapshotAtMs = value.snapshot_at_ms;
  const codexOne = value.sources.find((source) => isRecord(source) && source.source === "codex" && source.slot === 1);
  const codexTwo = value.sources.find((source) => isRecord(source) && source.source === "codex" && source.slot === 2);
  const yunwu = value.sources.find((source) => isRecord(source) && source.source === "yunwu");
  const sources = [
    readStoredCodexSource(codexOne, snapshotAtMs),
    readStoredCodexSource(codexTwo, snapshotAtMs),
    readStoredYunwuSource(yunwu, snapshotAtMs),
  ];
  if (!sources[0] || !sources[1] || !sources[2]) return null;
  return {
    snapshot_at_ms: snapshotAtMs,
    // Old records used a shorter value. All records now follow the sampler
    // freshness boundary so a missed 15-minute run is tolerated once.
    stale_after_ms: PROVIDER_CAPACITY_SOURCE_STALE_MS,
    sources: [sources[0], sources[1], sources[2]],
  };
};

const readCapacitySnapshot = async (kv: Deno.Kv): Promise<ProviderCapacitySnapshot | null> => {
  try {
    return readStoredSnapshot((await kv.get(PROVIDER_CAPACITY_SNAPSHOT_KEY)).value);
  } catch {
    return null;
  }
};

const readStoredHistoryPoint = (value: unknown): StoredHistoryPoint | null => {
  if (!isRecord(value) || !isSafeTimestamp(value.bucket_start_at_ms) || !isSafeTimestamp(value.sampled_at_ms)) {
    return null;
  }
  if (!Array.isArray(value.sources)) return null;
  const sourceOne = value.sources.find((source) => isRecord(source) && source.source === "codex" && source.slot === 1);
  const sourceTwo = value.sources.find((source) => isRecord(source) && source.source === "codex" && source.slot === 2);
  const sourceYunwu = value.sources.find((source) => isRecord(source) && source.source === "yunwu");
  const codexSourceOne = readStoredCodexSource(sourceOne, value.sampled_at_ms);
  const codexSourceTwo = readStoredCodexSource(sourceTwo, value.sampled_at_ms);
  if (!codexSourceOne || !codexSourceTwo) return null;
  const yunwuSource = readStoredYunwuSource(sourceYunwu, value.sampled_at_ms) ??
    unavailableYunwuSource(value.sampled_at_ms);
  return {
    bucket_start_at_ms: value.bucket_start_at_ms,
    sampled_at_ms: value.sampled_at_ms,
    sources: [codexSourceOne, codexSourceTwo, yunwuSource],
  };
};

export const providerCapacityHistoryKey = (bucketStartAtMs: number): Deno.KvKey => [
  ...PROVIDER_CAPACITY_HISTORY_KEY_PREFIX,
  bucketStartAtMs,
];

// A live refresh can observe a banked reset inside the same 15-minute bucket
// as the exhausted sample. Keep that earlier point under a sibling key so the
// chart can show the zero-to-refill transition without turning every refresh
// into an unbounded history stream.
const providerCapacityHistoryTransitionKey = (
  bucketStartAtMs: number,
  previousSampledAtMs: number,
): Deno.KvKey => [
  ...PROVIDER_CAPACITY_HISTORY_KEY_PREFIX,
  bucketStartAtMs,
  "transition",
  previousSampledAtMs,
];

const readCapacityHistory = async (kv: Deno.Kv, nowMs: number): Promise<ProviderCapacityHistoryPoint[]> => {
  const cutoffMs = Math.max(0, nowMs - PROVIDER_CAPACITY_HISTORY_RETENTION_MS);
  const newestBucketMs = Math.floor(nowMs / PROVIDER_CAPACITY_HISTORY_BUCKET_MS) * PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
  const points: ProviderCapacityHistoryPoint[] = [];
  try {
    for await (const entry of kv.list<unknown>({ prefix: PROVIDER_CAPACITY_HISTORY_KEY_PREFIX })) {
      const keyBucket = entry.key[PROVIDER_CAPACITY_HISTORY_KEY_PREFIX.length];
      if (typeof keyBucket !== "number" || keyBucket < cutoffMs || keyBucket > newestBucketMs) continue;
      const point = readStoredHistoryPoint(entry.value);
      if (!point || point.bucket_start_at_ms !== keyBucket) continue;
      points.push(point);
    }
  } catch {
    return [];
  }
  return points.sort((left, right) =>
    left.bucket_start_at_ms - right.bucket_start_at_ms || left.sampled_at_ms - right.sampled_at_ms
  );
};

const historyBucketStartAtMs = (snapshotAtMs: number): number =>
  Math.floor(snapshotAtMs / PROVIDER_CAPACITY_HISTORY_BUCKET_MS) * PROVIDER_CAPACITY_HISTORY_BUCKET_MS;

const historyPointForSnapshot = (snapshot: ProviderCapacitySnapshot): ProviderCapacityHistoryPoint => {
  const sourceForSlot = (slot: 1 | 2): ProviderCapacityCodexSource => {
    const source = snapshot.sources.find((candidate) => candidate.source === "codex" && candidate.slot === slot);
    return source?.source === "codex" ? source : unavailableCodexSource(slot, snapshot.snapshot_at_ms);
  };
  const sourceForYunwu = (): ProviderCapacityYunwuSource => {
    const source = snapshot.sources.find((candidate) => candidate.source === "yunwu");
    return source?.source === "yunwu" ? source : unavailableYunwuSource(snapshot.snapshot_at_ms);
  };
  return {
    bucket_start_at_ms: historyBucketStartAtMs(snapshot.snapshot_at_ms),
    sampled_at_ms: snapshot.snapshot_at_ms,
    sources: [sourceForSlot(1), sourceForSlot(2), sourceForYunwu()],
  };
};

const mergeHistoryPoints = (
  points: readonly ProviderCapacityHistoryPoint[],
  addition: ProviderCapacityHistoryPoint,
): ProviderCapacityHistoryPoint[] => {
  const bySample = new Map<string, ProviderCapacityHistoryPoint>();
  for (const point of points) bySample.set(`${point.bucket_start_at_ms}:${point.sampled_at_ms}`, point);
  bySample.set(`${addition.bucket_start_at_ms}:${addition.sampled_at_ms}`, addition);
  return [...bySample.values()].sort((left, right) =>
    left.bucket_start_at_ms - right.bucket_start_at_ms || left.sampled_at_ms - right.sampled_at_ms
  );
};

const staleProviderSnapshot = (snapshot: ProviderCapacitySnapshot, nowMs: number): ProviderCapacitySnapshot => {
  return {
    ...snapshot,
    sources: snapshot.sources.map((source) =>
      source.state !== "available"
        ? source
        : source.source === "codex" && nowMs >= source.snapshot_at_ms + snapshot.stale_after_ms
        ? { ...source, state: "stale" as const }
        : source.source === "yunwu" &&
            (source.wallet.cache_state === "stale" || source.source_observed_at_ms === null ||
              nowMs - source.source_observed_at_ms >= YUNWU_QUOTA_FRESH_MS)
        ? { ...source, state: "stale" as const }
        : source
    ) as [ProviderCapacitySource, ProviderCapacitySource, ProviderCapacitySource],
  };
};

const toCapacityView = (
  snapshot: ProviderCapacitySnapshot,
  requestedState: Exclude<ProviderCapacityViewState, "unavailable">,
  history: readonly ProviderCapacityHistoryPoint[],
  resetEvents: readonly ProviderCapacityResetEvent[],
  nowMs: number,
): ProviderCapacityView => {
  const current = staleProviderSnapshot(snapshot, nowMs);
  const stale = current.sources.some((source) => source.state === "stale");
  return {
    ...current,
    cache_state: stale ? "stale" : requestedState,
    history,
    reset_events: resetEvents,
  };
};

const unavailableSnapshot = (snapshotAtMs: number): ProviderCapacitySnapshot => ({
  snapshot_at_ms: snapshotAtMs,
  stale_after_ms: PROVIDER_CAPACITY_SOURCE_STALE_MS,
  sources: [
    unavailableCodexSource(1, snapshotAtMs),
    unavailableCodexSource(2, snapshotAtMs),
    unavailableYunwuSource(snapshotAtMs),
  ],
});

const unavailableView = (
  snapshotAtMs: number,
  history: readonly ProviderCapacityHistoryPoint[],
  resetEvents: readonly ProviderCapacityResetEvent[],
): ProviderCapacityView => ({
  ...unavailableSnapshot(snapshotAtMs),
  cache_state: "unavailable",
  history,
  reset_events: resetEvents,
});

const acquireCapacityLease = async (
  kv: Deno.Kv,
  owner: string,
  nowMs: number,
): Promise<{ acquired: boolean; entry: Deno.KvEntryMaybe<CapacityLease> }> => {
  const entry = await kv.get<CapacityLease>(PROVIDER_CAPACITY_LEASE_KEY);
  if (entry.value && entry.value.lease_until_ms > nowMs) return { acquired: false, entry };
  const lease: CapacityLease = { owner, lease_until_ms: nowMs + PROVIDER_CAPACITY_LEASE_MS };
  const committed = await kv.atomic()
    .check(entry)
    .set(PROVIDER_CAPACITY_LEASE_KEY, lease, { expireIn: PROVIDER_CAPACITY_LEASE_MS * 2 })
    .commit();
  if (!committed.ok) return { acquired: false, entry };
  const acquiredEntry = await kv.get<CapacityLease>(PROVIDER_CAPACITY_LEASE_KEY);
  return acquiredEntry.value?.owner === owner ? { acquired: true, entry: acquiredEntry } : {
    acquired: false,
    entry: acquiredEntry,
  };
};

const releaseCapacityLease = async (kv: Deno.Kv, owner: string): Promise<void> => {
  try {
    const entry = await kv.get<CapacityLease>(PROVIDER_CAPACITY_LEASE_KEY);
    if (entry.value?.owner !== owner) return;
    await kv.atomic().check(entry).delete(PROVIDER_CAPACITY_LEASE_KEY).commit();
  } catch {
    // A short-lived lease can expire without affecting the redacted snapshot.
  }
};

const codexResetTransitionObserved = (
  previous: ProviderCapacityHistoryPoint | null,
  current: ProviderCapacityHistoryPoint,
): boolean => {
  if (!previous || previous.sampled_at_ms >= current.sampled_at_ms) return false;
  for (const slot of [1, 2] as const) {
    const previousSource = previous.sources.find(
      (source): source is ProviderCapacityCodexSource => source.source === "codex" && source.slot === slot,
    );
    const currentSource = current.sources.find(
      (source): source is ProviderCapacityCodexSource => source.source === "codex" && source.slot === slot,
    );
    if (
      !previousSource || !currentSource || previousSource.state === "unavailable" ||
      currentSource.state === "unavailable"
    ) {
      continue;
    }
    for (const windowKey of ["primary", "secondary"] as const) {
      const previousWindow = previousSource.windows[windowKey];
      const currentWindow = currentSource.windows[windowKey];
      if (
        previousWindow?.used_percent !== null && currentWindow?.used_percent !== null &&
        typeof previousWindow?.used_percent === "number" && typeof currentWindow?.used_percent === "number" &&
        previousWindow.used_percent >= 90 && currentWindow.used_percent <= 20 &&
        previousWindow.reset_at_ms === currentWindow.reset_at_ms
      ) return true;
    }
  }
  return false;
};

const persistCapacitySnapshot = async (
  kv: Deno.Kv,
  leaseEntry: Deno.KvEntryMaybe<CapacityLease>,
  snapshot: ProviderCapacitySnapshot,
): Promise<boolean> => {
  const history = historyPointForSnapshot(snapshot);
  let previousHistory: ProviderCapacityHistoryPoint | null = null;
  try {
    previousHistory = readStoredHistoryPoint(
      (await kv.get<unknown>(providerCapacityHistoryKey(history.bucket_start_at_ms))).value,
    );
  } catch {
    // A missing prior point should not prevent the live snapshot from being stored.
  }
  const preserveTransition = codexResetTransitionObserved(previousHistory, history);
  let operation = kv.atomic()
    .check(leaseEntry)
    .set(PROVIDER_CAPACITY_SNAPSHOT_KEY, snapshot, { expireIn: PROVIDER_CAPACITY_SNAPSHOT_RETENTION_MS });
  if (preserveTransition && previousHistory) {
    operation = operation.set(
      providerCapacityHistoryTransitionKey(history.bucket_start_at_ms, previousHistory.sampled_at_ms),
      previousHistory,
      { expireIn: PROVIDER_CAPACITY_HISTORY_RETENTION_MS },
    );
  }
  const committed = await operation
    .set(providerCapacityHistoryKey(history.bucket_start_at_ms), history, {
      expireIn: PROVIDER_CAPACITY_HISTORY_RETENTION_MS,
    })
    .delete(PROVIDER_CAPACITY_LEASE_KEY)
    .commit();
  return committed.ok;
};

const waitForCapacitySnapshot = async (
  kv: Deno.Kv,
  previousSnapshotAtMs: number | null,
): Promise<ProviderCapacitySnapshot | null> => {
  const deadline = Date.now() + PROVIDER_CAPACITY_COLD_WAIT_MS;
  while (Date.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, 50));
    const [snapshot, lease] = await Promise.all([
      readCapacitySnapshot(kv),
      kv.get<CapacityLease>(PROVIDER_CAPACITY_LEASE_KEY).catch(() => null),
    ]);
    const leaseFinished = !lease?.value || lease.value.lease_until_ms <= Date.now();
    if (
      snapshot && (previousSnapshotAtMs === null || snapshot.snapshot_at_ms !== previousSnapshotAtMs || leaseFinished)
    ) {
      return snapshot;
    }
    if (!snapshot && leaseFinished) return null;
  }
  return null;
};

export const getPersistedProviderCapacityView = async (
  options: Pick<ProviderCapacitySnapshotOptions, "kv" | "now"> = {},
): Promise<ProviderCapacityView> => {
  const nowMs = safeNow(options.now ?? Date.now);
  const kv = options.kv === undefined ? await getKv() : options.kv;
  if (!kv) return unavailableView(nowMs, [], []);
  const [snapshot, history, resetEvents] = await Promise.all([
    readCapacitySnapshot(kv),
    readCapacityHistory(kv, nowMs),
    listProviderCapacityResetEvents({ kv, now: () => nowMs }),
  ]);
  return snapshot
    ? toCapacityView(snapshot, "persisted", history, resetEvents, nowMs)
    : unavailableView(nowMs, history, resetEvents);
};

export const refreshProviderCapacity = async (
  options: ProviderCapacitySnapshotOptions = {},
): Promise<ProviderCapacityView> => {
  const nowMs = safeNow(options.now ?? Date.now);
  const kv = options.kv === undefined ? await getKv() : options.kv;
  if (!kv) {
    const snapshot = await captureProviderCapacitySnapshot(options, nowMs, null);
    return toCapacityView(snapshot, "live", [historyPointForSnapshot(snapshot)], [], nowMs);
  }

  const [cached, historyBefore, resetEventsBefore] = await Promise.all([
    readCapacitySnapshot(kv),
    readCapacityHistory(kv, nowMs),
    listProviderCapacityResetEvents({ kv, now: () => nowMs }),
  ]);
  const owner = (options.createLeaseOwner ?? (() => crypto.randomUUID()))();
  const lease = await acquireCapacityLease(kv, owner, nowMs).catch(() => ({
    acquired: false,
    entry: { key: PROVIDER_CAPACITY_LEASE_KEY, value: null, versionstamp: null },
  } as { acquired: boolean; entry: Deno.KvEntryMaybe<CapacityLease> }));
  if (!lease.acquired) {
    const coalesced = await waitForCapacitySnapshot(kv, cached?.snapshot_at_ms ?? null).catch(() => null);
    const snapshot = coalesced ?? cached;
    const history = await readCapacityHistory(kv, nowMs).catch(() => historyBefore);
    const resetEvents = await listProviderCapacityResetEvents({ kv, now: () => nowMs }).catch(() => resetEventsBefore);
    return snapshot
      ? toCapacityView(snapshot, "persisted", history, resetEvents, nowMs)
      : unavailableView(nowMs, history, resetEvents);
  }

  try {
    const snapshot = await captureProviderCapacitySnapshot(options, nowMs, kv);
    const persisted = await persistCapacitySnapshot(kv, lease.entry, snapshot).catch(() => false);
    const history = await readCapacityHistory(kv, nowMs).catch(() => historyBefore);
    const resetEvents = await listProviderCapacityResetEvents({ kv, now: () => nowMs }).catch(() => resetEventsBefore);
    return toCapacityView(
      snapshot,
      "live",
      persisted ? history : mergeHistoryPoints(history, historyPointForSnapshot(snapshot)),
      resetEvents,
      nowMs,
    );
  } finally {
    await releaseCapacityLease(kv, owner);
  }
};

// Preserve the old direct helper as an explicit live refresh. HTTP callers use
// getPersistedProviderCapacityView unless they ask for refresh=live.
export const getProviderCapacitySnapshot = async (
  options: ProviderCapacitySnapshotOptions = {},
): Promise<ProviderCapacityView> => await refreshProviderCapacity(options);

export const handleProviderCapacity = async (
  request: Request = new Request("https://ai.ubq.fi/admin/providers/capacity"),
  options: ProviderCapacitySnapshotOptions = {},
): Promise<Response> => {
  try {
    const live = new URL(request.url).searchParams.get("refresh") === "live";
    const view = live ? await refreshProviderCapacity(options) : await getPersistedProviderCapacityView(options);
    return json(200, view, { "Cache-Control": "no-store" });
  } catch {
    return json(200, unavailableView(Date.now(), [], []), { "Cache-Control": "no-store" });
  }
};
