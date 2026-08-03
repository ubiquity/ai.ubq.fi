import { config } from "./config.ts";
import { type CodexCapacityAccount, getCodexCapacityAccounts } from "./codex.ts";
import { json } from "./http.ts";
import { getKv } from "./kv.ts";
import { getCachedConfiguredYunwuQuotaSnapshot, YUNWU_QUOTA_FRESH_MS, type YunwuQuotaSnapshot } from "./yunwu_quota.ts";
import { isRecord } from "./utils.ts";

export const PROVIDER_CAPACITY_SNAPSHOT_KEY = ["uos_ai", "provider_capacity", "v1", "snapshot"] as const;
export const PROVIDER_CAPACITY_LEASE_KEY = ["uos_ai", "provider_capacity", "v1", "lease"] as const;
export const PROVIDER_CAPACITY_HISTORY_KEY_PREFIX = ["uos_ai", "provider_capacity", "v1", "history"] as const;
export const PROVIDER_CAPACITY_HISTORY_BUCKET_MS = 15 * 60_000;
export const PROVIDER_CAPACITY_HISTORY_RETENTION_MS = 7 * 24 * 60 * 60_000;
// Keep this name for callers that used the old snapshot retention constant.
export const PROVIDER_CAPACITY_SNAPSHOT_RETENTION_MS = PROVIDER_CAPACITY_HISTORY_RETENTION_MS;
export const PROVIDER_CAPACITY_LEASE_MS = 10_000;
export const PROVIDER_CAPACITY_COLD_WAIT_MS = 2_000;
export const PROVIDER_CAPACITY_SOURCE_STALE_MS = 30 * 60_000;
export const PROVIDER_CAPACITY_CODEX_TIMEOUT_MS = 8_000;

type CapacityState = "available" | "stale" | "unavailable";
export type ProviderCapacityViewState = "live" | "persisted" | "stale" | "unavailable";

export type ProviderCapacityWindow = Readonly<{
  limit_window_seconds: number | null;
  used_percent: number | null;
  reset_at_ms: number | null;
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

export type ProviderCapacitySnapshot = Readonly<{
  snapshot_at_ms: number;
  stale_after_ms: number;
  sources: readonly [ProviderCapacitySource, ProviderCapacitySource, ProviderCapacitySource];
}>;

export type ProviderCapacityHistoryPoint = Readonly<{
  bucket_start_at_ms: number;
  sampled_at_ms: number;
  sources: readonly [ProviderCapacityCodexSource, ProviderCapacityCodexSource];
}>;

export type ProviderCapacityView = Readonly<
  ProviderCapacitySnapshot & {
    cache_state: ProviderCapacityViewState;
    history: readonly ProviderCapacityHistoryPoint[];
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

const parseCodexUsage = (value: unknown):
  | Readonly<{
    primary: ProviderCapacityWindow | null;
    secondary: ProviderCapacityWindow | null;
  }>
  | null => {
  if (!isRecord(value) || !isRecord(value.rate_limit)) return null;
  return {
    primary: parseCodexWindow(value.rate_limit.primary_window),
    secondary: parseCodexWindow(value.rate_limit.secondary_window),
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
});

const unavailableYunwuSource = (snapshotAtMs: number): ProviderCapacitySource => ({
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
    const windows = parseCodexUsage(payload);
    if (!windows) return unavailableCodexSource(account.slot as 1 | 2, snapshotAtMs);
    return {
      source: "codex",
      label: "Codex account " + account.slot,
      slot: account.slot as 1 | 2,
      state: "available",
      source_observed_at_ms: snapshotAtMs,
      snapshot_at_ms: snapshotAtMs,
      windows,
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
  const yunwuPromise = getCachedConfiguredYunwuQuotaSnapshot({ kv }).catch(() => null);
  const [codexSources, yunwuSnapshot] = await Promise.all([codexPromise, yunwuPromise]);

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
  };
};

const readStoredYunwuSource = (
  value: unknown,
  snapshotAtMs: number,
): ProviderCapacitySource | null => {
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
  const sources = [
    readStoredCodexSource(sourceOne, value.sampled_at_ms),
    readStoredCodexSource(sourceTwo, value.sampled_at_ms),
  ];
  if (!sources[0] || !sources[1]) return null;
  return {
    bucket_start_at_ms: value.bucket_start_at_ms,
    sampled_at_ms: value.sampled_at_ms,
    sources: [sources[0], sources[1]],
  };
};

export const providerCapacityHistoryKey = (bucketStartAtMs: number): Deno.KvKey => [
  ...PROVIDER_CAPACITY_HISTORY_KEY_PREFIX,
  bucketStartAtMs,
];

const readCapacityHistory = async (kv: Deno.Kv, nowMs: number): Promise<ProviderCapacityHistoryPoint[]> => {
  const cutoffMs = Math.max(0, nowMs - PROVIDER_CAPACITY_HISTORY_RETENTION_MS);
  const newestBucketMs = Math.floor(nowMs / PROVIDER_CAPACITY_HISTORY_BUCKET_MS) * PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
  const points: ProviderCapacityHistoryPoint[] = [];
  try {
    for await (
      const entry of kv.list<unknown>({
        start: providerCapacityHistoryKey(cutoffMs),
        end: providerCapacityHistoryKey(newestBucketMs + 1),
      })
    ) {
      const keyBucket = entry.key[entry.key.length - 1];
      if (typeof keyBucket !== "number" || keyBucket < cutoffMs || keyBucket > newestBucketMs) continue;
      const point = readStoredHistoryPoint(entry.value);
      if (!point || point.bucket_start_at_ms !== keyBucket) continue;
      points.push(point);
    }
  } catch {
    return [];
  }
  return points.sort((left, right) => left.bucket_start_at_ms - right.bucket_start_at_ms);
};

const historyBucketStartAtMs = (snapshotAtMs: number): number =>
  Math.floor(snapshotAtMs / PROVIDER_CAPACITY_HISTORY_BUCKET_MS) * PROVIDER_CAPACITY_HISTORY_BUCKET_MS;

const historyPointForSnapshot = (snapshot: ProviderCapacitySnapshot): ProviderCapacityHistoryPoint => {
  const sourceForSlot = (slot: 1 | 2): ProviderCapacityCodexSource => {
    const source = snapshot.sources.find((candidate) => candidate.source === "codex" && candidate.slot === slot);
    return source?.source === "codex" ? source : unavailableCodexSource(slot, snapshot.snapshot_at_ms);
  };
  return {
    bucket_start_at_ms: historyBucketStartAtMs(snapshot.snapshot_at_ms),
    sampled_at_ms: snapshot.snapshot_at_ms,
    sources: [sourceForSlot(1), sourceForSlot(2)],
  };
};

const mergeHistoryPoints = (
  points: readonly ProviderCapacityHistoryPoint[],
  addition: ProviderCapacityHistoryPoint,
): ProviderCapacityHistoryPoint[] => {
  const byBucket = new Map<number, ProviderCapacityHistoryPoint>();
  for (const point of points) byBucket.set(point.bucket_start_at_ms, point);
  byBucket.set(addition.bucket_start_at_ms, addition);
  return [...byBucket.values()].sort((left, right) => left.bucket_start_at_ms - right.bucket_start_at_ms);
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
  nowMs: number,
): ProviderCapacityView => {
  const current = staleProviderSnapshot(snapshot, nowMs);
  const stale = current.sources.some((source) => source.state === "stale");
  return {
    ...current,
    cache_state: stale ? "stale" : requestedState,
    history,
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
): ProviderCapacityView => ({
  ...unavailableSnapshot(snapshotAtMs),
  cache_state: "unavailable",
  history,
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

const persistCapacitySnapshot = async (
  kv: Deno.Kv,
  leaseEntry: Deno.KvEntryMaybe<CapacityLease>,
  snapshot: ProviderCapacitySnapshot,
): Promise<boolean> => {
  const history = historyPointForSnapshot(snapshot);
  const committed = await kv.atomic()
    .check(leaseEntry)
    .set(PROVIDER_CAPACITY_SNAPSHOT_KEY, snapshot, { expireIn: PROVIDER_CAPACITY_SNAPSHOT_RETENTION_MS })
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
  if (!kv) return unavailableView(nowMs, []);
  const [snapshot, history] = await Promise.all([
    readCapacitySnapshot(kv),
    readCapacityHistory(kv, nowMs),
  ]);
  return snapshot ? toCapacityView(snapshot, "persisted", history, nowMs) : unavailableView(nowMs, history);
};

export const refreshProviderCapacity = async (
  options: ProviderCapacitySnapshotOptions = {},
): Promise<ProviderCapacityView> => {
  const nowMs = safeNow(options.now ?? Date.now);
  const kv = options.kv === undefined ? await getKv() : options.kv;
  if (!kv) {
    const snapshot = await captureProviderCapacitySnapshot(options, nowMs, null);
    return toCapacityView(snapshot, "live", [historyPointForSnapshot(snapshot)], nowMs);
  }

  const [cached, historyBefore] = await Promise.all([
    readCapacitySnapshot(kv),
    readCapacityHistory(kv, nowMs),
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
    return snapshot ? toCapacityView(snapshot, "persisted", history, nowMs) : unavailableView(nowMs, history);
  }

  try {
    const snapshot = await captureProviderCapacitySnapshot(options, nowMs, kv);
    const persisted = await persistCapacitySnapshot(kv, lease.entry, snapshot).catch(() => false);
    const history = await readCapacityHistory(kv, nowMs).catch(() => historyBefore);
    return toCapacityView(
      snapshot,
      "live",
      persisted ? history : mergeHistoryPoints(history, historyPointForSnapshot(snapshot)),
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
    return json(200, unavailableView(Date.now(), []), { "Cache-Control": "no-store" });
  }
};
