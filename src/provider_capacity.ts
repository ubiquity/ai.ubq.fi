import { config } from "./config.ts";
import { type CodexCapacityAccount, getCodexCapacityAccounts } from "./codex.ts";
import { json } from "./http.ts";
import { getKv } from "./kv.ts";
import {
  type CodexCapacityRoutingObservationInput,
  recordCodexCapacityRoutingObservations,
} from "./codex_account_routing.ts";
import {
  listProviderCapacityDowntimeEvents,
  listProviderCapacityRateLimitResetEvents,
  listProviderCapacityResetEvents,
  PROVIDER_CAPACITY_RESET_EVENT_RETENTION_MS,
  type ProviderCapacityDowntimeEvent,
  type ProviderCapacityRateLimitResetEvent,
  providerCapacityRateLimitResetEventKey,
  type ProviderCapacityResetEvent,
} from "./provider_capacity_events.ts";
import { PROVIDER_CAPACITY_SNAPSHOT_KEY } from "./provider_capacity_contract.ts";
import { getConfiguredYunwuQuotaSnapshot, YUNWU_QUOTA_FRESH_MS, type YunwuQuotaSnapshot } from "./yunwu_quota.ts";
import { isRecord } from "./utils.ts";

export { PROVIDER_CAPACITY_SNAPSHOT_KEY } from "./provider_capacity_contract.ts";
export const PROVIDER_CAPACITY_LEASE_KEY = ["uos_ai", "provider_capacity", "v1", "lease"] as const;
export const PROVIDER_CAPACITY_HISTORY_KEY_PREFIX = ["uos_ai", "provider_capacity", "v1", "history"] as const;
const PROVIDER_CAPACITY_LAST_AVAILABLE_KEY_PREFIX = ["uos_ai", "provider_capacity", "v1", "last_available"] as const;
export const PROVIDER_CAPACITY_HISTORY_BUCKET_MS = 15 * 60_000;
export const PROVIDER_CAPACITY_HISTORY_RETENTION_MS = 7 * 24 * 60 * 60_000;
// Keep this name for callers that used the old snapshot retention constant.
export const PROVIDER_CAPACITY_SNAPSHOT_RETENTION_MS = PROVIDER_CAPACITY_HISTORY_RETENTION_MS;
export { PROVIDER_CAPACITY_RESET_EVENT_RETENTION_MS } from "./provider_capacity_events.ts";
export const PROVIDER_CAPACITY_LEASE_MS = 10_000;
export const PROVIDER_CAPACITY_COLD_WAIT_MS = 2_000;
export const PROVIDER_CAPACITY_SOURCE_STALE_MS = 30 * 60_000;
export const PROVIDER_CAPACITY_CODEX_TIMEOUT_MS = 8_000;
export const PROVIDER_CAPACITY_RATE_LIMIT_RESET_MIN_GAIN_PERCENTAGE_POINTS = 25;

const ADDITIONAL_WINDOW_UNANCHORED_TOLERANCE_MS = 60_000;
const CODEX_SPARK_LIMIT_NAME = "GPT-5.3-Codex-Spark";

const codexAccountLabel = (slot: number, email: string | null = null): string =>
  email?.trim() || `Codex account ${slot}`;

type CapacityState = "available" | "stale" | "unavailable";
export type ProviderCapacityViewState = "live" | "persisted" | "stale" | "unavailable";
export type ProviderCapacityFailureKind =
  | "not_configured"
  | "http_error"
  | "upstream_error"
  | "unreachable"
  | "invalid_response";

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

/** Provider capacity observations for the two active Codex routing slots. */
export type ProviderCapacitySource =
  | Readonly<{
    source: "codex";
    label: string;
    slot: 1 | 2;
    state: CapacityState;
    source_observed_at_ms: number | null;
    snapshot_at_ms: number;
    failure_kind: ProviderCapacityFailureKind | null;
    failure_status: number | null;
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
    rate_limit_reset_events: readonly ProviderCapacityRateLimitResetEvent[];
    downtime_events: readonly ProviderCapacityDowntimeEvent[];
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
type StoredRateLimitObservation = Readonly<{
  sampled_at_ms: number;
  source: ProviderCapacityCodexSource;
}>;

const providerCapacityLastAvailableKey = (slot: 1 | 2): Deno.KvKey => [
  ...PROVIDER_CAPACITY_LAST_AVAILABLE_KEY_PREFIX,
  slot,
];

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

const parseCodexAdditionalRateLimit = (value: unknown): ProviderCapacityAdditionalRateLimit | null => {
  if (!isRecord(value)) return null;
  const limitName = typeof value.limit_name === "string" ? value.limit_name.trim() : "";
  if (!limitName) return null;
  const meteredFeature = typeof value.metered_feature === "string" ? value.metered_feature.trim() || null : null;
  const rateLimit = isRecord(value.rate_limit) ? value.rate_limit : null;
  const parsedPrimary = parseCodexWindow(rateLimit?.primary_window);
  const parsedSecondary = parseCodexWindow(rateLimit?.secondary_window);
  if (!parsedPrimary && !parsedSecondary) return null;
  return {
    limit_name: limitName,
    metered_feature: meteredFeature,
    windows: {
      // Keep OpenAI's unused model window for the admin card. Routing filters
      // this unanchored value separately because its reset is not evidence of
      // a shared quota cycle.
      primary: parsedPrimary,
      secondary: parsedSecondary,
    },
  };
};

const additionalRateLimitsForRouting = (
  limits: readonly ProviderCapacityAdditionalRateLimit[],
  snapshotAtMs: number,
): readonly ProviderCapacityAdditionalRateLimit[] =>
  limits.flatMap((limit) => {
    const primary = isUnanchoredAdditionalWindow(limit.windows.primary, snapshotAtMs) ? null : limit.windows.primary;
    const secondary = isUnanchoredAdditionalWindow(limit.windows.secondary, snapshotAtMs)
      ? null
      : limit.windows.secondary;
    if (!primary && !secondary) return [];
    return [{
      ...limit,
      windows: { primary, secondary },
    }];
  });

const isCodexSparkLimit = (limit: ProviderCapacityAdditionalRateLimit): boolean =>
  limit.limit_name.trim().toLowerCase() === CODEX_SPARK_LIMIT_NAME.toLowerCase();

/**
 * OpenAI can omit the named Spark window from one account in a pool even when
 * a reachable sibling reports it. Keep the admin snapshot's model rows
 * aligned for the account pool. Routing continues to use only the account's
 * own upstream observation, so this display projection cannot change model
 * selection or quota admission.
 */
const fillMissingCodexSparkLimitForAdmin = (
  sources: readonly ProviderCapacityCodexSource[],
): readonly ProviderCapacityCodexSource[] => {
  const sharedSparkLimit = sources
    .filter((source) => source.state !== "unavailable")
    .flatMap((source) => source.additional_rate_limits)
    .find(isCodexSparkLimit);
  if (!sharedSparkLimit) return sources;
  return sources.map((source) => {
    if (
      source.state === "unavailable" ||
      source.additional_rate_limits.some(isCodexSparkLimit)
    ) return source;
    return {
      ...source,
      additional_rate_limits: [...source.additional_rate_limits, sharedSparkLimit],
    };
  });
};

const parseCodexUsage = (value: unknown):
  | Readonly<{
    primary: ProviderCapacityWindow | null;
    secondary: ProviderCapacityWindow | null;
    additional_rate_limits: readonly ProviderCapacityAdditionalRateLimit[];
  }>
  | null => {
  if (!isRecord(value) || !isRecord(value.rate_limit)) return null;
  const additionalRateLimits = Array.isArray(value.additional_rate_limits)
    ? value.additional_rate_limits.flatMap((candidate) => {
      const parsed = parseCodexAdditionalRateLimit(candidate);
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

const unavailableCodexSource = (
  slot: 1 | 2,
  snapshotAtMs: number,
  failureKind: ProviderCapacityFailureKind = "not_configured",
  failureStatus: number | null = null,
  observed = false,
  label = codexAccountLabel(slot),
): ProviderCapacityCodexSource => ({
  source: "codex",
  label,
  slot,
  state: "unavailable",
  source_observed_at_ms: observed ? snapshotAtMs : null,
  snapshot_at_ms: snapshotAtMs,
  failure_kind: failureKind,
  failure_status: failureStatus,
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
      return unavailableCodexSource(
        account.slot as 1 | 2,
        snapshotAtMs,
        response.status >= 500 ? "upstream_error" : "http_error",
        response.status,
        true,
        codexAccountLabel(account.slot, account.email),
      );
    }
    let payload: unknown;
    try {
      payload = await response.json();
    } catch {
      return unavailableCodexSource(
        account.slot as 1 | 2,
        snapshotAtMs,
        "invalid_response",
        response.status,
        true,
        codexAccountLabel(account.slot, account.email),
      );
    }
    const windows = parseCodexUsage(payload);
    if (!windows) {
      return unavailableCodexSource(
        account.slot as 1 | 2,
        snapshotAtMs,
        "invalid_response",
        response.status,
        true,
        codexAccountLabel(account.slot, account.email),
      );
    }
    return {
      source: "codex",
      label: codexAccountLabel(account.slot, account.email),
      slot: account.slot as 1 | 2,
      state: "available",
      source_observed_at_ms: snapshotAtMs,
      snapshot_at_ms: snapshotAtMs,
      failure_kind: null,
      failure_status: null,
      windows: {
        primary: windows.primary,
        secondary: windows.secondary,
      },
      additional_rate_limits: windows.additional_rate_limits,
    };
  } catch {
    return unavailableCodexSource(
      account.slot as 1 | 2,
      snapshotAtMs,
      "unreachable",
      null,
      true,
      codexAccountLabel(account.slot, account.email),
    );
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
      additional_rate_limits: additionalRateLimitsForRouting(source.additional_rate_limits, snapshotAtMs),
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
  const failureKind = value.failure_kind === null || value.failure_kind === undefined
    ? state === "unavailable" ? "not_configured" : null
    : value.failure_kind;
  const failureStatus = value.failure_status === null || value.failure_status === undefined
    ? null
    : typeof value.failure_status === "number" && Number.isSafeInteger(value.failure_status) &&
        value.failure_status >= 100 && value.failure_status <= 599
    ? value.failure_status
    : null;
  if (
    !(failureKind === null || failureKind === "not_configured" || failureKind === "http_error" ||
      failureKind === "upstream_error" || failureKind === "unreachable" || failureKind === "invalid_response") ||
    (state === "available" && (failureKind !== null || failureStatus !== null))
  ) return null;
  const windows = isRecord(value.windows) ? value.windows : null;
  if (!windows) return null;
  return {
    source: "codex",
    label: typeof value.label === "string" && value.label.trim() ? value.label.trim() : codexAccountLabel(value.slot),
    slot: value.slot,
    state,
    source_observed_at_ms: observed,
    snapshot_at_ms: snapshotAtMs,
    failure_kind: state === "available" ? null : failureKind,
    failure_status: state === "available" ? null : failureStatus,
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
  rateLimitResetEvents: readonly ProviderCapacityRateLimitResetEvent[],
  downtimeEvents: readonly ProviderCapacityDowntimeEvent[],
  nowMs: number,
): ProviderCapacityView => {
  const current = staleProviderSnapshot(snapshot, nowMs);
  const codexSources = fillMissingCodexSparkLimitForAdmin(
    current.sources.filter((source): source is ProviderCapacityCodexSource => source.source === "codex"),
  );
  const projected = {
    ...current,
    sources: [codexSources[0] ?? current.sources[0], codexSources[1] ?? current.sources[1], current.sources[2]],
  } as ProviderCapacitySnapshot;
  const stale = projected.sources.some((source) => source.state === "stale");
  return {
    ...projected,
    cache_state: stale ? "stale" : requestedState,
    history,
    reset_events: resetEvents,
    rate_limit_reset_events: rateLimitResetEvents,
    downtime_events: downtimeEvents,
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
  rateLimitResetEvents: readonly ProviderCapacityRateLimitResetEvent[],
  downtimeEvents: readonly ProviderCapacityDowntimeEvent[],
): ProviderCapacityView => ({
  ...unavailableSnapshot(snapshotAtMs),
  cache_state: "unavailable",
  history,
  reset_events: resetEvents,
  rate_limit_reset_events: rateLimitResetEvents,
  downtime_events: downtimeEvents,
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

const codexAvailabilityTransitionObserved = (
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
      previousSource && currentSource &&
      (previousSource.state === "unavailable") !== (currentSource.state === "unavailable")
    ) return true;
  }
  return false;
};

const readStoredRateLimitObservation = (value: unknown): StoredRateLimitObservation | null => {
  if (!isRecord(value) || !isSafeTimestamp(value.sampled_at_ms)) return null;
  const source = readStoredCodexSource(value.source, value.sampled_at_ms);
  return source?.state === "available" ? { sampled_at_ms: value.sampled_at_ms, source } : null;
};

const latestAvailableRateLimitObservation = (
  history: readonly ProviderCapacityHistoryPoint[],
  slot: 1 | 2,
  beforeSampledAtMs: number,
): StoredRateLimitObservation | null => {
  const observations = history.flatMap((point) => {
    if (point.sampled_at_ms >= beforeSampledAtMs) return [];
    const source = point.sources.find(
      (candidate): candidate is ProviderCapacityCodexSource =>
        candidate.source === "codex" && candidate.slot === slot && candidate.state !== "unavailable",
    );
    return source ? [{ sampled_at_ms: point.sampled_at_ms, source }] : [];
  });
  return observations.sort((left, right) => right.sampled_at_ms - left.sampled_at_ms)[0] ?? null;
};

const observedRateLimitResetEvents = (
  previous: ProviderCapacitySnapshot | null,
  current: ProviderCapacitySnapshot,
  lastAvailableObservations: readonly StoredRateLimitObservation[] = [],
): ProviderCapacityRateLimitResetEvent[] => {
  if (previous && previous.snapshot_at_ms >= current.snapshot_at_ms) return [];
  const events: ProviderCapacityRateLimitResetEvent[] = [];
  for (const slot of [1, 2] as const) {
    const currentSource = current.sources.find(
      (source): source is ProviderCapacityCodexSource => source.source === "codex" && source.slot === slot,
    );
    if (!currentSource || currentSource.state === "unavailable") continue;
    const previousSource = previous?.sources.find(
      (source): source is ProviderCapacityCodexSource => source.source === "codex" && source.slot === slot,
    );
    const previousObservation = previous && previousSource && previousSource.state !== "unavailable"
      ? { sampled_at_ms: previous.snapshot_at_ms, source: previousSource }
      : lastAvailableObservations.find((observation) =>
        observation.source.slot === slot && observation.sampled_at_ms < current.snapshot_at_ms
      ) ?? null;
    if (!previousObservation) continue;
    for (const window of ["primary", "secondary"] as const) {
      const previousWindow = previousObservation.source.windows[window];
      const currentWindow = currentSource.windows[window];
      if (
        typeof previousWindow?.used_percent !== "number" || typeof currentWindow?.used_percent !== "number" ||
        typeof previousWindow.reset_at_ms !== "number" || typeof currentWindow.reset_at_ms !== "number" ||
        currentWindow.reset_at_ms <= previousWindow.reset_at_ms
      ) continue;
      const capacityGain = previousWindow.used_percent - currentWindow.used_percent;
      if (capacityGain < PROVIDER_CAPACITY_RATE_LIMIT_RESET_MIN_GAIN_PERCENTAGE_POINTS) continue;
      events.push({
        v: 1,
        event_id: `openai-${slot}-${window}-${previousObservation.sampled_at_ms}-${current.snapshot_at_ms}`,
        provider: "openai",
        slot,
        window,
        observed_at_ms: current.snapshot_at_ms,
        previous_sampled_at_ms: previousObservation.sampled_at_ms,
        previous_reset_at_ms: previousWindow.reset_at_ms,
        reset_at_ms: currentWindow.reset_at_ms,
        previous_used_percent: previousWindow.used_percent,
        current_used_percent: currentWindow.used_percent,
        capacity_gain_percentage_points: capacityGain,
      });
    }
  }
  return events;
};

const observedHistoricalRateLimitResetEvents = (
  history: readonly ProviderCapacityHistoryPoint[],
): ProviderCapacityRateLimitResetEvent[] => {
  const events: ProviderCapacityRateLimitResetEvent[] = [];
  const sortedHistory = [...history].sort((left, right) => left.sampled_at_ms - right.sampled_at_ms);
  for (const slot of [1, 2] as const) {
    let previousAvailable: StoredRateLimitObservation | null = null;
    let outageObserved = false;
    for (const point of sortedHistory) {
      const source = point.sources.find(
        (candidate): candidate is ProviderCapacityCodexSource =>
          candidate.source === "codex" && candidate.slot === slot,
      );
      if (!source || source.state === "unavailable") {
        if (previousAvailable) outageObserved = true;
        continue;
      }
      if (previousAvailable && outageObserved) {
        for (const window of ["primary", "secondary"] as const) {
          const previousWindow = previousAvailable.source.windows[window];
          const currentWindow = source.windows[window];
          if (
            typeof previousWindow?.used_percent !== "number" || typeof currentWindow?.used_percent !== "number" ||
            typeof previousWindow.reset_at_ms !== "number" || typeof currentWindow.reset_at_ms !== "number" ||
            currentWindow.reset_at_ms <= previousWindow.reset_at_ms
          ) continue;
          const capacityGain = previousWindow.used_percent - currentWindow.used_percent;
          if (capacityGain < PROVIDER_CAPACITY_RATE_LIMIT_RESET_MIN_GAIN_PERCENTAGE_POINTS) continue;
          events.push({
            v: 1,
            event_id: `openai-${slot}-${window}-${previousAvailable.sampled_at_ms}-${point.sampled_at_ms}`,
            provider: "openai",
            slot,
            window,
            observed_at_ms: point.sampled_at_ms,
            previous_sampled_at_ms: previousAvailable.sampled_at_ms,
            previous_reset_at_ms: previousWindow.reset_at_ms,
            reset_at_ms: currentWindow.reset_at_ms,
            previous_used_percent: previousWindow.used_percent,
            current_used_percent: currentWindow.used_percent,
            capacity_gain_percentage_points: capacityGain,
          });
        }
      }
      previousAvailable = source.state === "available"
        ? { sampled_at_ms: point.sampled_at_ms, source }
        : previousAvailable;
      outageObserved = false;
    }
  }
  return events;
};

const mergeHistoricalRateLimitResetEvents = async (
  kv: Deno.Kv,
  history: readonly ProviderCapacityHistoryPoint[],
  events: readonly ProviderCapacityRateLimitResetEvent[],
): Promise<readonly ProviderCapacityRateLimitResetEvent[]> => {
  const merged = new Map(events.map((event) => [event.event_id, event]));
  for (const event of observedHistoricalRateLimitResetEvents(history)) {
    if (merged.has(event.event_id)) continue;
    try {
      await kv.set(providerCapacityRateLimitResetEventKey(event.event_id), event, {
        expireIn: PROVIDER_CAPACITY_RESET_EVENT_RETENTION_MS,
      });
    } catch {
      // The chart can still show a validated inference if a best-effort marker write fails.
    }
    merged.set(event.event_id, event);
  }
  return [...merged.values()].sort((left, right) =>
    left.observed_at_ms - right.observed_at_ms || left.event_id.localeCompare(right.event_id)
  );
};

const persistCapacitySnapshot = async (
  kv: Deno.Kv,
  leaseEntry: Deno.KvEntryMaybe<CapacityLease>,
  snapshot: ProviderCapacitySnapshot,
): Promise<boolean> => {
  const history = historyPointForSnapshot(snapshot);
  let previousSnapshot: ProviderCapacitySnapshot | null = null;
  let previousHistory: ProviderCapacityHistoryPoint | null = null;
  let lastAvailableObservations: StoredRateLimitObservation[] = [];
  try {
    const [storedSnapshot, storedHistory, ...storedObservations] = await Promise.all([
      kv.get<unknown>(PROVIDER_CAPACITY_SNAPSHOT_KEY, { consistency: "strong" }),
      kv.get<unknown>(providerCapacityHistoryKey(history.bucket_start_at_ms)),
      ...([1, 2] as const).map((slot) => kv.get<unknown>(providerCapacityLastAvailableKey(slot))),
    ]);
    previousSnapshot = readStoredSnapshot(storedSnapshot.value);
    previousHistory = readStoredHistoryPoint(storedHistory.value);
    lastAvailableObservations = storedObservations.flatMap((entry) => {
      const observation = readStoredRateLimitObservation(entry.value);
      return observation ? [observation] : [];
    });
  } catch {
    // A missing prior point should not prevent the live snapshot from being stored.
  }
  const recoverySlots = snapshot.sources.flatMap<1 | 2>((source) => {
    if (
      source.source !== "codex" || source.state === "unavailable" ||
      (source.slot !== 1 && source.slot !== 2)
    ) return [];
    const previousSource = previousSnapshot?.sources.find(
      (candidate): candidate is ProviderCapacityCodexSource =>
        candidate.source === "codex" && candidate.slot === source.slot,
    );
    return previousSource?.state === "unavailable" &&
        !lastAvailableObservations.some((observation) => observation.source.slot === source.slot)
      ? [source.slot]
      : [];
  });
  if (recoverySlots.length > 0) {
    const retainedHistory = await readCapacityHistory(kv, snapshot.snapshot_at_ms).catch(() => []);
    for (const slot of recoverySlots) {
      const observation = latestAvailableRateLimitObservation(retainedHistory, slot, snapshot.snapshot_at_ms);
      if (observation) lastAvailableObservations.push(observation);
    }
  }
  const rateLimitResetEvents = observedRateLimitResetEvents(previousSnapshot, snapshot, lastAvailableObservations);
  const preserveTransition = rateLimitResetEvents.length > 0 ||
    codexResetTransitionObserved(previousHistory, history) ||
    codexAvailabilityTransitionObserved(previousHistory, history);
  let operation = kv.atomic()
    .check(leaseEntry)
    .set(PROVIDER_CAPACITY_SNAPSHOT_KEY, snapshot, { expireIn: PROVIDER_CAPACITY_SNAPSHOT_RETENTION_MS });
  for (const event of rateLimitResetEvents) {
    operation = operation.set(providerCapacityRateLimitResetEventKey(event.event_id), event, {
      expireIn: PROVIDER_CAPACITY_RESET_EVENT_RETENTION_MS,
    });
  }
  for (const source of snapshot.sources) {
    if (
      source.source !== "codex" || source.state !== "available" ||
      (source.slot !== 1 && source.slot !== 2)
    ) continue;
    operation = operation.set(
      providerCapacityLastAvailableKey(source.slot),
      { sampled_at_ms: snapshot.snapshot_at_ms, source },
      { expireIn: PROVIDER_CAPACITY_RESET_EVENT_RETENTION_MS },
    );
  }
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
  if (!kv) return unavailableView(nowMs, [], [], [], []);
  const [snapshot, history, resetEvents, rateLimitResetEvents, downtimeEvents] = await Promise.all([
    readCapacitySnapshot(kv),
    readCapacityHistory(kv, nowMs),
    listProviderCapacityResetEvents({ kv, now: () => nowMs }),
    listProviderCapacityRateLimitResetEvents({ kv, now: () => nowMs }),
    listProviderCapacityDowntimeEvents({ kv, now: () => nowMs }),
  ]);
  const mergedRateLimitResetEvents = await mergeHistoricalRateLimitResetEvents(kv, history, rateLimitResetEvents);
  return snapshot
    ? toCapacityView(snapshot, "persisted", history, resetEvents, mergedRateLimitResetEvents, downtimeEvents, nowMs)
    : unavailableView(nowMs, history, resetEvents, mergedRateLimitResetEvents, downtimeEvents);
};

export const refreshProviderCapacity = async (
  options: ProviderCapacitySnapshotOptions = {},
): Promise<ProviderCapacityView> => {
  const nowMs = safeNow(options.now ?? Date.now);
  const kv = options.kv === undefined ? await getKv() : options.kv;
  if (!kv) {
    const snapshot = await captureProviderCapacitySnapshot(options, nowMs, null);
    return toCapacityView(snapshot, "live", [historyPointForSnapshot(snapshot)], [], [], [], nowMs);
  }

  const [cached, historyBefore, resetEventsBefore, rateLimitResetEventsBefore, downtimeEventsBefore] = await Promise
    .all([
      readCapacitySnapshot(kv),
      readCapacityHistory(kv, nowMs),
      listProviderCapacityResetEvents({ kv, now: () => nowMs }),
      listProviderCapacityRateLimitResetEvents({ kv, now: () => nowMs }),
      listProviderCapacityDowntimeEvents({ kv, now: () => nowMs }),
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
    const rateLimitResetEvents = await listProviderCapacityRateLimitResetEvents({ kv, now: () => nowMs }).catch(() =>
      rateLimitResetEventsBefore
    );
    const downtimeEvents = await listProviderCapacityDowntimeEvents({ kv, now: () => nowMs }).catch(() =>
      downtimeEventsBefore
    );
    const mergedRateLimitResetEvents = await mergeHistoricalRateLimitResetEvents(kv, history, rateLimitResetEvents);
    return snapshot
      ? toCapacityView(snapshot, "persisted", history, resetEvents, mergedRateLimitResetEvents, downtimeEvents, nowMs)
      : unavailableView(nowMs, history, resetEvents, mergedRateLimitResetEvents, downtimeEvents);
  }

  try {
    const snapshot = await captureProviderCapacitySnapshot(options, nowMs, kv);
    const persisted = await persistCapacitySnapshot(kv, lease.entry, snapshot).catch(() => false);
    const history = await readCapacityHistory(kv, nowMs).catch(() => historyBefore);
    const resetEvents = await listProviderCapacityResetEvents({ kv, now: () => nowMs }).catch(() => resetEventsBefore);
    const rateLimitResetEvents = await listProviderCapacityRateLimitResetEvents({ kv, now: () => nowMs }).catch(() =>
      rateLimitResetEventsBefore
    );
    const downtimeEvents = await listProviderCapacityDowntimeEvents({ kv, now: () => nowMs }).catch(() =>
      downtimeEventsBefore
    );
    const mergedRateLimitResetEvents = await mergeHistoricalRateLimitResetEvents(kv, history, rateLimitResetEvents);
    return toCapacityView(
      snapshot,
      "live",
      persisted ? history : mergeHistoryPoints(history, historyPointForSnapshot(snapshot)),
      resetEvents,
      mergedRateLimitResetEvents,
      downtimeEvents,
      nowMs,
    );
  } finally {
    await releaseCapacityLease(kv, owner);
  }
};

/**
 * Persist one scheduled capacity sample without building the admin projection.
 *
 * The deploy cron does not consume a ProviderCapacityView, so scanning the
 * seven-day history and reset-event ledgers before and after every sample only
 * pays to construct a discarded response. Keep the capture, routing
 * observations, lease, snapshot/history write, and same-bucket reset
 * transition exactly on the durable sampler path. Admin callers continue to
 * use refreshProviderCapacity() when they need the full projection.
 */
export const sampleProviderCapacityForCron = async (
  options: ProviderCapacitySnapshotOptions = {},
): Promise<void> => {
  const nowMs = safeNow(options.now ?? Date.now);
  const kv = options.kv === undefined ? await getKv() : options.kv;
  // A scheduled sample without durable storage would only create provider
  // traffic while leaving routing and history state stale.
  if (!kv) return;

  const owner = (options.createLeaseOwner ?? (() => crypto.randomUUID()))();
  const lease = await acquireCapacityLease(kv, owner, nowMs).catch(() => ({
    acquired: false,
    entry: { key: PROVIDER_CAPACITY_LEASE_KEY, value: null, versionstamp: null },
  } as { acquired: boolean; entry: Deno.KvEntryMaybe<CapacityLease> }));
  // The scheduled caller has no view to return. A competing live refresh or
  // cron owns the only probe, so avoid both a duplicate probe and the old
  // coalesced-view polling loop.
  if (!lease.acquired) return;

  let persisted = false;
  try {
    const snapshot = await captureProviderCapacitySnapshot(options, nowMs, kv);
    persisted = await persistCapacitySnapshot(kv, lease.entry, snapshot).catch(() => false);
  } finally {
    // A successful atomic persist deletes the checked lease. Only failed or
    // interrupted persistence needs the conservative owner-checked release.
    if (!persisted) await releaseCapacityLease(kv, owner);
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
    return json(200, unavailableView(Date.now(), [], [], [], []), { "Cache-Control": "no-store" });
  }
};
