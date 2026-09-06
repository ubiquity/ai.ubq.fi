import type { MeteredQuotaBalanceSample, MeteredQuotaSnapshot } from "./metered_quota.ts";
import type { PaidFallbackUsageRollup } from "./paid_fallback_rollups.ts";

/**
 * Pure projection math for the admin "quota runway" view. No KV access here:
 * callers feed settled-usage rollups and a Metered quota snapshot and receive
 * per-model consumption statistics plus exhaustion estimates. Keeping this
 * module free of I/O lets the whole projection be unit-tested deterministically.
 */

export const QUOTA_PROJECTION_WINDOW_DAYS = [7, 30, 90] as const;
export type QuotaProjectionWindowDays = (typeof QUOTA_PROJECTION_WINDOW_DAYS)[number];

const DAY_MS = 24 * 60 * 60 * 1_000;
const HOUR_MS = 60 * 60 * 1_000;

const positiveFinite = (value: number | null | undefined): value is number =>
  typeof value === "number" && Number.isFinite(value) && value > 0;

const nonNegativeFinite = (value: number | null | undefined): value is number =>
  typeof value === "number" && Number.isFinite(value) && value >= 0;

export type MeteredQuotaRefillSchedule = Readonly<{
  /** Estimated amount added by each refill, in the same quota units as balance_quota. */
  amount_quota: number | null;
  /** Estimated time between refills. */
  cadence_ms: number | null;
  /** Estimated refill rate in quota units per hour. */
  rate_quota_per_hour: number | null;
  /** Most recent observed refill, used to anchor the next scheduled refill. */
  last_refill_at_ms: number | null;
  /** Number of distinct refill observations used for the estimate. */
  known_refill_count: number;
}>;

const EMPTY_REFILL_SCHEDULE: MeteredQuotaRefillSchedule = {
  amount_quota: null,
  cadence_ms: null,
  rate_quota_per_hour: null,
  last_refill_at_ms: null,
  known_refill_count: 0,
};

export type MeteredQuotaRunwayView = Readonly<{
  configured: boolean;
  available: boolean;
  cache_state: string | null;
  confidence: MeteredQuotaSnapshot["state"]["confidence"] | null;
  unlimited_quota: boolean;
  // Wallet mode fields. Token-usage mode keeps these neutral (zero) per the
  // Metered contract and reports totals instead.
  balance_quota: number | null;
  baseline_quota: number | null;
  quota_per_credit: number | null;
  balance_credits: number | null;
  baseline_credits: number | null;
  remaining_percent: number | null;
  used_percent: number | null;
  // Token-usage mode fields.
  total_available: number | null;
  total_granted: number | null;
  total_used: number | null;
  observed_at_ms: number | null;
  cycle_started_at_ms: number | null;
  last_credit_at_ms: number | null;
  latest_refill_amount_credits: number | null;
  latest_refill_completed_at_ms: number | null;
  refill_schedule?: MeteredQuotaRefillSchedule;
}>;

type RefillEvent = Readonly<{
  at_ms: number;
  amount_quota: number;
}>;

type QuotaProjectionRefillInput =
  | readonly MeteredQuotaBalanceSample[]
  | Readonly<{
    balance_history?: readonly MeteredQuotaBalanceSample[];
    balanceHistory?: readonly MeteredQuotaBalanceSample[];
  }>;

const refillHistoryFromInput = (
  input: QuotaProjectionRefillInput | undefined,
): readonly MeteredQuotaBalanceSample[] => {
  if (!input) return [];
  if (Array.isArray(input)) return input as readonly MeteredQuotaBalanceSample[];
  const options = input as Readonly<{
    balance_history?: readonly MeteredQuotaBalanceSample[];
    balanceHistory?: readonly MeteredQuotaBalanceSample[];
  }>;
  return options.balance_history ?? options.balanceHistory ?? [];
};

const median = (values: readonly number[]): number | null => {
  if (values.length === 0) return null;
  const sorted = [...values].sort((left, right) => left - right);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 === 0 ? (sorted[middle - 1]! + sorted[middle]!) / 2 : sorted[middle]!;
};

const normalizeRefillSchedule = (
  schedule: MeteredQuotaRefillSchedule | undefined,
): MeteredQuotaRefillSchedule => {
  if (!schedule) return EMPTY_REFILL_SCHEDULE;
  const amount = positiveFinite(schedule.amount_quota) ? schedule.amount_quota : null;
  const cadence = positiveFinite(schedule.cadence_ms) ? schedule.cadence_ms : null;
  const rate = amount !== null && cadence !== null ? amount / cadence * HOUR_MS : null;
  return {
    amount_quota: amount,
    cadence_ms: cadence,
    rate_quota_per_hour: positiveFinite(rate) ? rate : null,
    last_refill_at_ms: nonNegativeFinite(schedule.last_refill_at_ms) ? schedule.last_refill_at_ms : null,
    known_refill_count: Number.isSafeInteger(schedule.known_refill_count) && schedule.known_refill_count > 0
      ? schedule.known_refill_count
      : 0,
  };
};

/**
 * Infer a recurring refill schedule from hourly balance samples and the
 * refill facts carried by the current Metered state. Baseline increases are
 * preferred because they are not reduced by concurrent usage; a balance
 * increase is accepted as a fallback when the baseline was unchanged.
 *
 * A single refill is deliberately not treated as a schedule. Without two
 * refill observations there is no evidence for a cadence, so callers retain
 * the existing no-refill estimate instead of inventing a cycle length.
 */
export const estimateMeteredRefillSchedule = (
  quota:
    & Pick<
      MeteredQuotaRunwayView,
      | "quota_per_credit"
      | "cycle_started_at_ms"
      | "latest_refill_amount_credits"
      | "latest_refill_completed_at_ms"
    >
    & Partial<Pick<MeteredQuotaRunwayView, "refill_schedule">>,
  balanceHistory: readonly MeteredQuotaBalanceSample[] = [],
): MeteredQuotaRefillSchedule => {
  const eventsByTime = new Map<number, number>();
  const addEvent = (event: RefillEvent, overwrite = false): void => {
    if (!nonNegativeFinite(event.at_ms) || !positiveFinite(event.amount_quota)) return;
    if (overwrite || !eventsByTime.has(event.at_ms)) eventsByTime.set(event.at_ms, event.amount_quota);
  };

  const samplesByBucket = new Map<number, MeteredQuotaBalanceSample>();
  for (const sample of balanceHistory) {
    if (
      !nonNegativeFinite(sample.bucket_start_at_ms) ||
      !nonNegativeFinite(sample.observed_at_ms) ||
      !nonNegativeFinite(sample.balance_quota) ||
      !nonNegativeFinite(sample.baseline_quota)
    ) continue;
    const existing = samplesByBucket.get(sample.bucket_start_at_ms);
    if (
      !existing ||
      sample.observed_at_ms > existing.observed_at_ms ||
      (sample.observed_at_ms === existing.observed_at_ms && sample.bucket_start_at_ms > existing.bucket_start_at_ms)
    ) {
      samplesByBucket.set(sample.bucket_start_at_ms, sample);
    }
  }
  const samples = [...samplesByBucket.values()].sort((left, right) =>
    left.observed_at_ms - right.observed_at_ms || left.bucket_start_at_ms - right.bucket_start_at_ms
  );
  for (let index = 1; index < samples.length; index += 1) {
    const previous = samples[index - 1]!;
    const current = samples[index]!;
    const baselineIncrease = current.baseline_quota - previous.baseline_quota;
    if (baselineIncrease > 0) {
      addEvent({ at_ms: current.observed_at_ms, amount_quota: baselineIncrease });
      continue;
    }
    // Some provider responses expose the refill in the live balance before
    // the post-refill baseline is updated. Keep this fallback conservative:
    // it is useful for cadence detection, while the latest refill metadata is
    // preferred for the amount whenever it is available.
    const balanceIncrease = current.balance_quota - previous.balance_quota;
    if (balanceIncrease > 0 && current.baseline_quota >= previous.baseline_quota) {
      addEvent({ at_ms: current.observed_at_ms, amount_quota: balanceIncrease });
    }
  }

  const latestAmount = positiveFinite(quota.latest_refill_amount_credits) &&
      positiveFinite(quota.quota_per_credit)
    ? quota.latest_refill_amount_credits * quota.quota_per_credit
    : null;
  const cycleStartedAt = nonNegativeFinite(quota.cycle_started_at_ms) ? quota.cycle_started_at_ms : null;
  const latestCompletedAt = nonNegativeFinite(quota.latest_refill_completed_at_ms)
    ? quota.latest_refill_completed_at_ms
    : null;
  if (latestAmount !== null) {
    // When the two cycle fields differ they provide an additional interval
    // even if the retained samples contain only one visible refill. Equal
    // timestamps collapse to one event in the map.
    if (cycleStartedAt !== null) addEvent({ at_ms: cycleStartedAt, amount_quota: latestAmount }, true);
    if (latestCompletedAt !== null) {
      // The live refill observation is authoritative when it shares a
      // sample's timestamp, so it replaces an inferred balance-jump amount.
      addEvent({ at_ms: latestCompletedAt, amount_quota: latestAmount }, true);
    }
  }

  const events = [...eventsByTime.entries()]
    .map(([at_ms, amount_quota]) => ({ at_ms, amount_quota }))
    .sort((left, right) => left.at_ms - right.at_ms);
  const existing = normalizeRefillSchedule(quota.refill_schedule);
  const intervals = events.slice(1).map((event, index) => event.at_ms - events[index]!.at_ms)
    .filter(positiveFinite);
  const cadence = median(intervals) ?? existing.cadence_ms;
  const latest = events.at(-1);
  const amount = latest?.amount_quota ?? existing.amount_quota;
  const lastRefillAt = latest?.at_ms ?? existing.last_refill_at_ms;
  const rate = amount !== undefined && amount !== null && cadence !== null ? amount / cadence * HOUR_MS : null;
  return {
    amount_quota: positiveFinite(amount) ? amount : null,
    cadence_ms: positiveFinite(cadence) ? cadence : null,
    rate_quota_per_hour: positiveFinite(rate) ? rate : null,
    last_refill_at_ms: nonNegativeFinite(lastRefillAt) ? lastRefillAt : null,
    known_refill_count: Math.max(events.length, existing.known_refill_count),
  };
};

export const meteredQuotaRunwayView = (
  snapshot: MeteredQuotaSnapshot | null,
  refillInput?: QuotaProjectionRefillInput,
): MeteredQuotaRunwayView => {
  const balanceHistory = refillHistoryFromInput(refillInput);
  if (!snapshot) {
    return {
      configured: false,
      available: false,
      cache_state: null,
      confidence: null,
      unlimited_quota: false,
      balance_quota: null,
      baseline_quota: null,
      quota_per_credit: null,
      balance_credits: null,
      baseline_credits: null,
      remaining_percent: null,
      used_percent: null,
      total_available: null,
      total_granted: null,
      total_used: null,
      observed_at_ms: null,
      cycle_started_at_ms: null,
      last_credit_at_ms: null,
      latest_refill_amount_credits: null,
      latest_refill_completed_at_ms: null,
      refill_schedule: EMPTY_REFILL_SCHEDULE,
    };
  }
  const state = snapshot.state;
  const tokenUsage = snapshot.unlimited_quota || snapshot.total_available !== null || snapshot.total_used !== null;
  const view = {
    configured: true,
    available: true,
    cache_state: snapshot.cache_state,
    confidence: tokenUsage ? null : state.confidence,
    unlimited_quota: tokenUsage ? snapshot.unlimited_quota === true : false,
    balance_quota: tokenUsage ? null : state.current_balance_quota,
    baseline_quota: tokenUsage ? null : state.post_refill_baseline_quota,
    quota_per_credit: state.quota_per_credit,
    balance_credits: snapshot.balance_credits,
    baseline_credits: snapshot.baseline_credits,
    remaining_percent: snapshot.remaining_percent,
    used_percent: snapshot.used_percent,
    total_available: tokenUsage ? snapshot.total_available : null,
    total_granted: tokenUsage ? snapshot.total_granted : null,
    total_used: tokenUsage ? snapshot.total_used : null,
    observed_at_ms: state.observed_at_ms,
    cycle_started_at_ms: tokenUsage ? null : state.cycle_started_at_ms,
    last_credit_at_ms: tokenUsage ? null : state.last_credit_at_ms,
    latest_refill_amount_credits: state.latest_refill_amount_credits,
    latest_refill_completed_at_ms: state.latest_refill_completed_at_ms,
    refill_schedule: EMPTY_REFILL_SCHEDULE,
  } satisfies MeteredQuotaRunwayView;
  return {
    ...view,
    refill_schedule: estimateMeteredRefillSchedule(view, balanceHistory),
  };
};

export type PaidFallbackModelSeries = Readonly<{
  model: string;
  provider: string;
  buckets: readonly PaidFallbackUsageRollup[];
}>;

export const groupPaidFallbackUsageRollups = (
  rollups: readonly PaidFallbackUsageRollup[],
): PaidFallbackModelSeries[] => {
  const byModelProvider = new Map<string, PaidFallbackUsageRollup[]>();
  for (const rollup of rollups) {
    const identity = `${rollup.model}\u0000${rollup.provider}`;
    const existing = byModelProvider.get(identity);
    if (existing) existing.push(rollup);
    else byModelProvider.set(identity, [rollup]);
  }
  return [...byModelProvider.entries()]
    .map(([identity, buckets]) => {
      const [model, provider] = identity.split("\u0000");
      return {
        model: model ?? "",
        provider: provider ?? "",
        buckets: buckets.sort((left, right) => left.bucket_start_at_ms - right.bucket_start_at_ms),
      } satisfies PaidFallbackModelSeries;
    })
    .sort((left, right) => left.model.localeCompare(right.model) || left.provider.localeCompare(right.provider));
};

export type PaidFallbackModelUsageWindow = Readonly<{
  window_days: QuotaProjectionWindowDays;
  bucket_count: number;
  request_count: number;
  quota_sum: number;
  spend_microcredits: number;
  input_tokens: number;
  cached_input_tokens: number;
  output_tokens: number;
  avg_quota_per_request: number | null;
  quota_per_hour: number | null;
  first_bucket_start_at_ms: number | null;
  last_bucket_start_at_ms: number | null;
}>;

const summarizeWindow = (
  buckets: readonly PaidFallbackUsageRollup[],
  windowDays: QuotaProjectionWindowDays,
  nowMs: number,
): PaidFallbackModelUsageWindow => {
  // Floor to the hour boundary so the window includes the bucket that
  // contains its start (hourly bucket-level precision is documented).
  const windowStartMs = Math.floor((nowMs - windowDays * DAY_MS) / HOUR_MS) * HOUR_MS;
  const inWindow = buckets.filter((bucket) => bucket.bucket_start_at_ms >= windowStartMs);
  const requestCount = inWindow.reduce((sum, bucket) => sum + bucket.request_count, 0);
  const quotaSum = inWindow.reduce((sum, bucket) => sum + bucket.quota_sum, 0);
  const firstBucket = inWindow.length ? inWindow[0]?.bucket_start_at_ms ?? null : null;
  const lastBucket = inWindow.length ? inWindow[inWindow.length - 1]?.bucket_start_at_ms ?? null : null;
  // The rate covers the entire selected window, idle hours included: a model
  // used once in the final day of a 30-day window is one request per 30 days,
  // not a daily rate. Labeling it otherwise would overstate the runway.
  const windowHours = windowDays * 24;
  return {
    window_days: windowDays,
    // Shards of the same hour count once.
    bucket_count: new Set(inWindow.map((bucket) => bucket.bucket_start_at_ms)).size,
    request_count: requestCount,
    quota_sum: quotaSum,
    spend_microcredits: inWindow.reduce((sum, bucket) => sum + bucket.spend_microcredits, 0),
    input_tokens: inWindow.reduce((sum, bucket) => sum + bucket.input_tokens, 0),
    cached_input_tokens: inWindow.reduce((sum, bucket) => sum + bucket.cached_input_tokens, 0),
    output_tokens: inWindow.reduce((sum, bucket) => sum + bucket.output_tokens, 0),
    avg_quota_per_request: requestCount > 0 ? quotaSum / requestCount : null,
    quota_per_hour: windowHours > 0 ? quotaSum / windowHours : null,
    first_bucket_start_at_ms: firstBucket,
    last_bucket_start_at_ms: lastBucket,
  };
};

export type PaidFallbackModelUsage = Readonly<{
  model: string;
  provider: string;
  windows: readonly [
    PaidFallbackModelUsageWindow,
    PaidFallbackModelUsageWindow,
    PaidFallbackModelUsageWindow,
  ];
}>;

export const summarizePaidFallbackUsage = (
  series: readonly PaidFallbackModelSeries[],
  nowMs: number,
): PaidFallbackModelUsage[] =>
  series.map((entry) => ({
    model: entry.model,
    provider: entry.provider,
    windows: QUOTA_PROJECTION_WINDOW_DAYS.map((windowDays) =>
      summarizeWindow(entry.buckets, windowDays, nowMs)
    ) as unknown as PaidFallbackModelUsage["windows"],
  }));

export type PaidFallbackRefillStatus =
  | "unavailable"
  | "no_refill"
  | "sustainable"
  | "known_refill_schedule";

export type PaidFallbackRunwayEstimate = Readonly<{
  window_days: QuotaProjectionWindowDays;
  unlimited: boolean;
  /** True when the balance snapshot is older than the freshness window. */
  stale_balance: boolean;
  requests_remaining: number | null;
  /** Refill-aware request capacity; null means unknown or sustainable. */
  refill_requests_remaining: number | null;
  /** The conservative estimate assuming no future refill. */
  time_remaining_ms: number | null;
  /** The conservative exhaustion timestamp assuming no future refill. */
  exhausted_at_ms: number | null;
  /** Explicit aliases that keep the no-refill comparison visible beside the refill-aware result. */
  no_refill_time_remaining_ms: number | null;
  no_refill_exhausted_at_ms: number | null;
  /** Refill-aware result: no_refill, sustainable, or a known schedule. */
  refill_status: PaidFallbackRefillStatus;
  refill_time_remaining_ms: number | null;
  refill_exhausted_at_ms: number | null;
  refill_amount_quota: number | null;
  refill_cadence_ms: number | null;
  refill_rate_quota_per_hour: number | null;
  next_refill_at_ms: number | null;
  percent_per_request_vs_balance: number | null;
  percent_per_request_vs_baseline: number | null;
}>;

type RefillProjection = Readonly<{
  status: PaidFallbackRefillStatus;
  time_remaining_ms: number | null;
  exhausted_at_ms: number | null;
  next_refill_at_ms: number | null;
}>;

const nextScheduledRefillAt = (
  schedule: MeteredQuotaRefillSchedule,
  nowMs: number,
): number | null => {
  if (
    !positiveFinite(schedule.cadence_ms) ||
    !nonNegativeFinite(schedule.last_refill_at_ms) ||
    !Number.isFinite(nowMs)
  ) return null;
  if (schedule.last_refill_at_ms > nowMs) return schedule.last_refill_at_ms;
  const elapsed = nowMs - schedule.last_refill_at_ms;
  const intervals = Math.floor(elapsed / schedule.cadence_ms) + 1;
  const next = schedule.last_refill_at_ms + intervals * schedule.cadence_ms;
  return nonNegativeFinite(next) ? next : null;
};

const durationTo = (atMs: number | null, nowMs: number): number | null =>
  atMs !== null && Number.isFinite(atMs) && atMs >= nowMs && Number.isFinite(atMs - nowMs)
    ? Math.trunc(atMs - nowMs)
    : null;

const refillProjection = (
  balanceQuota: number | null,
  quotaPerHour: number | null,
  nowMs: number,
  noRefillTimeRemainingMs: number | null,
  noRefillExhaustedAtMs: number | null,
  schedule: MeteredQuotaRefillSchedule,
): RefillProjection => {
  const nextRefillAtMs = nextScheduledRefillAt(schedule, nowMs);
  if (balanceQuota === null || !nonNegativeFinite(balanceQuota) || !positiveFinite(quotaPerHour)) {
    return {
      status: "unavailable",
      time_remaining_ms: null,
      exhausted_at_ms: null,
      next_refill_at_ms: nextRefillAtMs,
    };
  }
  if (
    !positiveFinite(schedule.amount_quota) ||
    !positiveFinite(schedule.cadence_ms) ||
    nextRefillAtMs === null
  ) {
    return {
      status: "no_refill",
      time_remaining_ms: noRefillTimeRemainingMs,
      exhausted_at_ms: noRefillExhaustedAtMs,
      next_refill_at_ms: nextRefillAtMs,
    };
  }

  const untilNextRefillMs = nextRefillAtMs - nowMs;
  const consumptionBeforeRefill = quotaPerHour * untilNextRefillMs / HOUR_MS;
  if (!Number.isFinite(consumptionBeforeRefill)) {
    return {
      status: "unavailable",
      time_remaining_ms: null,
      exhausted_at_ms: null,
      next_refill_at_ms: nextRefillAtMs,
    };
  }
  // A refill arriving at the same instant that the balance reaches zero is
  // still usable. Only exhaustion strictly before the next refill defeats the
  // schedule.
  if (balanceQuota < consumptionBeforeRefill) {
    return {
      status: "known_refill_schedule",
      time_remaining_ms: noRefillTimeRemainingMs,
      exhausted_at_ms: noRefillExhaustedAtMs,
      next_refill_at_ms: nextRefillAtMs,
    };
  }

  const balanceAfterRefill = balanceQuota - consumptionBeforeRefill + schedule.amount_quota;
  const cycleConsumption = quotaPerHour * schedule.cadence_ms / HOUR_MS;
  if (!Number.isFinite(balanceAfterRefill) || !positiveFinite(cycleConsumption)) {
    return {
      status: "unavailable",
      time_remaining_ms: null,
      exhausted_at_ms: null,
      next_refill_at_ms: nextRefillAtMs,
    };
  }
  if (schedule.amount_quota >= cycleConsumption) {
    return {
      status: "sustainable",
      time_remaining_ms: null,
      exhausted_at_ms: null,
      next_refill_at_ms: nextRefillAtMs,
    };
  }

  // After the first scheduled refill, each completed cycle changes the
  // balance by the same negative amount. Find the first cycle whose balance
  // would be exhausted before its next refill without iterating potentially
  // thousands of cycles.
  const deficitPerCycle = cycleConsumption - schedule.amount_quota;
  const balanceBeforeFailingCycle = balanceAfterRefill - cycleConsumption;
  const completedCyclesBeforeFailure = balanceBeforeFailingCycle < 0
    ? 0
    : Math.floor(balanceBeforeFailingCycle / deficitPerCycle) + 1;
  const balanceAtFailingCycleStart = balanceAfterRefill - completedCyclesBeforeFailure * deficitPerCycle;
  const exhaustionAtMs = nextRefillAtMs + completedCyclesBeforeFailure * schedule.cadence_ms +
    Math.max(0, balanceAtFailingCycleStart) / quotaPerHour * HOUR_MS;
  if (!Number.isFinite(exhaustionAtMs) || exhaustionAtMs < nowMs) {
    return {
      status: "unavailable",
      time_remaining_ms: null,
      exhausted_at_ms: null,
      next_refill_at_ms: nextRefillAtMs,
    };
  }
  return {
    status: "known_refill_schedule",
    time_remaining_ms: durationTo(exhaustionAtMs, nowMs),
    exhausted_at_ms: exhaustionAtMs,
    next_refill_at_ms: nextRefillAtMs,
  };
};

/**
 * Balance the projection is drawn against. Wallet mode uses the live wallet
 * balance. Token-usage mode reports total_available as the remaining
 * inventory, so it is used directly — subtracting total_used would
 * double-count consumption. Otherwise there is no authoritative balance.
 */
const runwayBalance = (quota: MeteredQuotaRunwayView): number | null => {
  if (quota.unlimited_quota) return null;
  if (nonNegativeFinite(quota.balance_quota)) return quota.balance_quota;
  if (nonNegativeFinite(quota.total_available)) return quota.total_available;
  return null;
};

const runwayBaseline = (quota: MeteredQuotaRunwayView): number | null => {
  if (quota.unlimited_quota) return null;
  if (nonNegativeFinite(quota.baseline_quota)) return quota.baseline_quota;
  return quota.total_granted !== null && quota.total_granted > 0 ? quota.total_granted : null;
};

/**
 * Projections are only meaningful for the provider whose balance is being
 * monitored. `METERED_API_KEY` monitors the OpenLux account, which serves
 * `provider: "metered"` rows; Surplus Intelligence has its own billing.
 * The optional retained balance history enables the refill-aware branch while
 * preserving the existing no-refill fields for contrast.
 */
export const projectPaidFallbackRunway = (
  usage: PaidFallbackModelUsage,
  quota: MeteredQuotaRunwayView,
  nowMs: number,
  refillInput?: QuotaProjectionRefillInput,
): PaidFallbackRunwayEstimate[] => {
  if (usage.provider !== "metered") return [];
  const balanceQuota = runwayBalance(quota);
  const baselineQuota = runwayBaseline(quota);
  const unlimited = quota.unlimited_quota === true;
  const staleBalance = quota.cache_state === "stale";
  const schedule = estimateMeteredRefillSchedule(quota, refillHistoryFromInput(refillInput));
  return usage.windows.map((window) => {
    if (unlimited) {
      return {
        window_days: window.window_days,
        unlimited: true,
        stale_balance: staleBalance,
        requests_remaining: null,
        refill_requests_remaining: null,
        time_remaining_ms: null,
        exhausted_at_ms: null,
        no_refill_time_remaining_ms: null,
        no_refill_exhausted_at_ms: null,
        refill_status: "unavailable",
        refill_time_remaining_ms: null,
        refill_exhausted_at_ms: null,
        refill_amount_quota: null,
        refill_cadence_ms: null,
        refill_rate_quota_per_hour: null,
        next_refill_at_ms: null,
        percent_per_request_vs_balance: null,
        percent_per_request_vs_baseline: null,
      };
    }
    const avgQuota = window.avg_quota_per_request;
    const quotaPerHour = window.quota_per_hour;
    const requestsRemaining = balanceQuota !== null && positiveFinite(avgQuota)
      ? Math.floor(balanceQuota / avgQuota)
      : null;
    const timeRemainingMs = balanceQuota !== null && positiveFinite(quotaPerHour)
      ? Math.trunc(balanceQuota / quotaPerHour * HOUR_MS)
      : null;
    const exhaustedAtMs = timeRemainingMs !== null ? nowMs + timeRemainingMs : null;
    const refill = refillProjection(
      balanceQuota,
      quotaPerHour,
      nowMs,
      timeRemainingMs,
      exhaustedAtMs,
      schedule,
    );
    const refillRequestsRemaining = refill.status === "sustainable"
      ? null
      : refill.status === "no_refill"
      ? requestsRemaining
      : refill.time_remaining_ms !== null && positiveFinite(avgQuota) && positiveFinite(quotaPerHour)
      ? Math.floor(refill.time_remaining_ms / HOUR_MS * quotaPerHour / avgQuota)
      : null;
    return {
      window_days: window.window_days,
      unlimited: false,
      stale_balance: staleBalance,
      requests_remaining: requestsRemaining,
      refill_requests_remaining: refillRequestsRemaining,
      // These two fields are intentionally the no-refill result. The
      // refill-aware result is exposed separately below.
      time_remaining_ms: timeRemainingMs,
      exhausted_at_ms: exhaustedAtMs,
      no_refill_time_remaining_ms: timeRemainingMs,
      no_refill_exhausted_at_ms: exhaustedAtMs,
      refill_status: refill.status,
      refill_time_remaining_ms: refill.time_remaining_ms,
      refill_exhausted_at_ms: refill.exhausted_at_ms,
      refill_amount_quota: schedule.amount_quota,
      refill_cadence_ms: schedule.cadence_ms,
      refill_rate_quota_per_hour: schedule.rate_quota_per_hour,
      next_refill_at_ms: refill.next_refill_at_ms,
      percent_per_request_vs_balance: balanceQuota !== null && positiveFinite(avgQuota) && balanceQuota > 0
        ? avgQuota / balanceQuota * 100
        : null,
      percent_per_request_vs_baseline: baselineQuota !== null && positiveFinite(avgQuota) && baselineQuota > 0
        ? avgQuota / baselineQuota * 100
        : null,
    };
  });
};
