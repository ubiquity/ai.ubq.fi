import { config } from "./config.ts";
import { type CodexCapacityAccount, getCodexCapacityAccounts } from "./codex.ts";
import { json } from "./http.ts";
import { getKv } from "./kv.ts";
import { getCachedConfiguredYunwuQuotaSnapshot, YUNWU_QUOTA_FRESH_MS, type YunwuQuotaSnapshot } from "./yunwu_quota.ts";
import { isRecord } from "./utils.ts";

export const PROVIDER_CAPACITY_SNAPSHOT_KEY = ["uos_ai", "provider_capacity", "v1", "snapshot"] as const;
export const PROVIDER_CAPACITY_LEASE_KEY = ["uos_ai", "provider_capacity", "v1", "lease"] as const;
export const PROVIDER_CAPACITY_SNAPSHOT_TTL_MS = 25_000;
export const PROVIDER_CAPACITY_SNAPSHOT_RETENTION_MS = 10 * 60_000;
export const PROVIDER_CAPACITY_LEASE_MS = 10_000;
export const PROVIDER_CAPACITY_COLD_WAIT_MS = 2_000;
export const PROVIDER_CAPACITY_SOURCE_STALE_MS = 5 * 60_000;
export const PROVIDER_CAPACITY_CODEX_TIMEOUT_MS = 8_000;

type CapacityState = "available" | "stale" | "unavailable";
type CapacityCacheState = "fresh" | "refreshed" | "stale";

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

export type ProviderCapacitySnapshot = Readonly<{
  snapshot_at_ms: number;
  stale_after_ms: number;
  cache_state: CapacityCacheState;
  sources: readonly [ProviderCapacitySource, ProviderCapacitySource, ProviderCapacitySource];
}>;

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

const capacityState = (value: unknown): CapacityState | null =>
  value === "available" || value === "stale" || value === "unavailable" ? value : null;

const capacityCacheState = (value: unknown): CapacityCacheState | null =>
  value === "fresh" || value === "refreshed" || value === "stale" ? value : null;

const safeNow = (now: () => number): number => {
  const value = Math.trunc(now());
  return Number.isSafeInteger(value) && value >= 0 ? value : Date.now();
};

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

const unavailableCodexSource = (slot: 1 | 2, snapshotAtMs: number): ProviderCapacitySource => ({
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
): Promise<ProviderCapacitySource> => {
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
  nowMs: number,
): ProviderCapacitySource => {
  if (!snapshot) return unavailableYunwuSource(snapshotAtMs);
  const sourceObservedAtMs = snapshot.state.observed_at_ms;
  const stale = snapshot.cache_state === "stale" || nowMs - sourceObservedAtMs >= YUNWU_QUOTA_FRESH_MS;
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

  const now = options.now ?? Date.now;
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
    cache_state: "refreshed",
    sources: [codexSources[0], codexSources[1], yunwuCapacitySource(yunwuSnapshot, snapshotAtMs, safeNow(now))],
  };
};

const isStoredWindow = (value: unknown): value is ProviderCapacityWindow => {
  if (!isRecord(value)) return false;
  return (value.limit_window_seconds === null || parseWindowSeconds(value.limit_window_seconds) !== null) &&
    (value.used_percent === null || parsePercent(value.used_percent) !== null) &&
    (value.reset_at_ms === null ||
      (typeof value.reset_at_ms === "number" && Number.isSafeInteger(value.reset_at_ms) && value.reset_at_ms >= 0));
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
  snapshotAtMs: number,
): ProviderCapacitySource | null => {
  if (!isRecord(value) || value.source !== "codex" || (value.slot !== 1 && value.slot !== 2)) return null;
  const state = capacityState(value.state);
  const observed = value.source_observed_at_ms;
  if (
    !state || !(observed === null || (typeof observed === "number" && Number.isSafeInteger(observed) && observed >= 0))
  ) {
    return null;
  }
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
  if (
    !state || !wallet ||
    !(observed === null || (typeof observed === "number" && Number.isSafeInteger(observed) && observed >= 0))
  ) return null;
  const optionalNumber = (candidate: unknown): number | null =>
    candidate === null ? null : typeof candidate === "number" && Number.isFinite(candidate) ? candidate : null;
  const optionalTimestamp = (candidate: unknown): number | null =>
    candidate === null
      ? null
      : typeof candidate === "number" && Number.isSafeInteger(candidate) && candidate >= 0
      ? candidate
      : null;
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
  if (!isRecord(value)) return null;
  const snapshotAtMs = value.snapshot_at_ms;
  const staleAfterMs = value.stale_after_ms;
  const cacheState = capacityCacheState(value.cache_state);
  if (
    !cacheState || typeof snapshotAtMs !== "number" || !Number.isSafeInteger(snapshotAtMs) || snapshotAtMs < 0 ||
    typeof staleAfterMs !== "number" || !Number.isSafeInteger(staleAfterMs) || staleAfterMs < 0 ||
    !Array.isArray(value.sources)
  ) return null;
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
    stale_after_ms: staleAfterMs,
    cache_state: cacheState,
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

const markSnapshotStale = (snapshot: ProviderCapacitySnapshot): ProviderCapacitySnapshot => ({
  ...snapshot,
  cache_state: "stale",
  sources: snapshot.sources.map((source) => source.state === "available" ? { ...source, state: "stale" } : source) as [
    ProviderCapacitySource,
    ProviderCapacitySource,
    ProviderCapacitySource,
  ],
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
  return { acquired: true, entry: acquiredEntry };
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
  const stored = { ...snapshot, cache_state: "refreshed" as const };
  return (await kv.atomic()
    .check(leaseEntry)
    .set(PROVIDER_CAPACITY_SNAPSHOT_KEY, stored, { expireIn: PROVIDER_CAPACITY_SNAPSHOT_RETENTION_MS })
    .delete(PROVIDER_CAPACITY_LEASE_KEY)
    .commit()).ok;
};

const waitForCapacitySnapshot = async (kv: Deno.Kv): Promise<ProviderCapacitySnapshot | null> => {
  const deadline = Date.now() + PROVIDER_CAPACITY_COLD_WAIT_MS;
  while (Date.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, 50));
    const snapshot = await readCapacitySnapshot(kv);
    if (snapshot) return snapshot;
  }
  return null;
};

const unavailableSnapshot = (snapshotAtMs: number): ProviderCapacitySnapshot => ({
  snapshot_at_ms: snapshotAtMs,
  stale_after_ms: PROVIDER_CAPACITY_SOURCE_STALE_MS,
  cache_state: "stale",
  sources: [
    unavailableCodexSource(1, snapshotAtMs),
    unavailableCodexSource(2, snapshotAtMs),
    unavailableYunwuSource(snapshotAtMs),
  ],
});

export const getProviderCapacitySnapshot = async (
  options: ProviderCapacitySnapshotOptions = {},
): Promise<ProviderCapacitySnapshot> => {
  const now = options.now ?? Date.now;
  const nowMs = safeNow(now);
  const kv = options.kv === undefined ? await getKv() : options.kv;
  if (!kv) return await captureProviderCapacitySnapshot(options, nowMs, null);

  const cached = await readCapacitySnapshot(kv);
  if (cached && nowMs - cached.snapshot_at_ms < PROVIDER_CAPACITY_SNAPSHOT_TTL_MS) {
    return { ...cached, cache_state: "fresh" };
  }

  const owner = (options.createLeaseOwner ?? (() => crypto.randomUUID()))();
  const lease = await acquireCapacityLease(kv, owner, nowMs).catch(() => ({
    acquired: false,
    entry: { key: PROVIDER_CAPACITY_LEASE_KEY, value: null, versionstamp: null },
  } as { acquired: boolean; entry: Deno.KvEntryMaybe<CapacityLease> }));
  if (!lease.acquired) {
    const waited = await waitForCapacitySnapshot(kv).catch(() => null);
    return waited ? markSnapshotStale(waited) : cached ? markSnapshotStale(cached) : unavailableSnapshot(nowMs);
  }

  try {
    const snapshot = await captureProviderCapacitySnapshot(options, nowMs, kv);
    const persisted = await persistCapacitySnapshot(kv, lease.entry, snapshot).catch(() => false);
    if (persisted) return snapshot;
    return snapshot;
  } finally {
    await releaseCapacityLease(kv, owner);
  }
};

export const handleProviderCapacity = async (): Promise<Response> => {
  try {
    const snapshot = await getProviderCapacitySnapshot();
    return json(200, snapshot, { "Cache-Control": "no-store" });
  } catch {
    return json(200, unavailableSnapshot(Date.now()), { "Cache-Control": "no-store" });
  }
};
