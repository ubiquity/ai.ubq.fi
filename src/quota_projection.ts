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
  /**
   * Optional retained samples supplied by a caller that already read them.
   * The normal snapshot view does not populate this field; keeping it
   * optional lets the pure projection function consume the same view in
   * callers that have a balance-history read available.
   */
  balance_history?: readonly MeteredQuotaBalanceSample[];
}>;

export const meteredQuotaRunwayView = (
  snapshot: MeteredQuotaSnapshot | null,
  balanceHistory?: readonly MeteredQuotaBalanceSample[],
): MeteredQuotaRunwayView => {
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
      ...(balanceHistory === undefined ? {} : { balance_history: balanceHistory }),
    };
  }
  const state = snapshot.state;
  const tokenUsage = snapshot.unlimited_quota || snapshot.total_available !== null || snapshot.total_used !== null;
  return {
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
    ...(balanceHistory === undefined ? {} : { balance_history: balanceHistory }),
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

export type PaidFallbackRunwayEstimate = Readonly<{
  window_days: QuotaProjectionWindowDays;
  unlimited: boolean;
  /** True when the balance snapshot is older than the freshness window. */
  stale_balance: boolean;
  /** Legacy fields remain the no-refill baseline for backwards compatibility. */
  requests_remaining: number | null;
  time_remaining_ms: number | null;
  exhausted_at_ms: number | null;
  percent_per_request_vs_balance: number | null;
  percent_per_request_vs_baseline: number | null;
  /** Always-visible no-refill contrast for the selected window. */
  no_refill_requests_remaining: number | null;
  no_refill_time_remaining_ms: number | null;
  no_refill_exhausted_at_ms: number | null;
  /** Refill-aware result and the facts used to calculate it. */
  refill_status: QuotaRefillStatus;
  refill_amount_quota: number | null;
  refill_amount_credits: number | null;
  refill_cadence_ms: number | null;
  refill_rate_quota_per_hour: number | null;
  next_refill_at_ms: number | null;
  with_refill_requests_remaining: number | null;
  with_refill_time_remaining_ms: number | null;
  with_refill_exhausted_at_ms: number | null;
  refill_projection_label: string | null;
  refill_projection: QuotaRefillProjection;
}>;

/**
 * `no_refill` preserves the original runway calculation. `with_refill` is a
 * finite runway after applying an observed periodic refill schedule,
 * `sustainable` means the average refill rate is at least the burn rate, and
 * `unknown` means there is not enough information to make either claim.
 */
export type QuotaRefillStatus =
  | "unknown"
  | "unlimited"
  | "no_refill"
  | "with_refill"
  | "sustainable";

export type MeteredQuotaRefillSchedule = Readonly<{
  amount_quota: number | null;
  amount_credits: number | null;
  cadence_ms: number | null;
  rate_quota_per_hour: number | null;
  next_refill_at_ms: number | null;
  observed_refill_count: number;
  source: "none" | "latest_refill" | "balance_history";
}>;

export type QuotaRefillProjection = Readonly<{
  status: QuotaRefillStatus;
  no_refill_exhausted_at_ms: number | null;
  with_refill_exhausted_at_ms: number | null;
  label: string | null;
  schedule: MeteredQuotaRefillSchedule;
}>;

export type QuotaProjectionOptions = Readonly<{
  balance_history?: readonly MeteredQuotaBalanceSample[];
  /** Camel-case alias is accepted for pure callers outside the HTTP layer. */
  balanceHistory?: readonly MeteredQuotaBalanceSample[];
}>;

const positiveFinite = (value: number | null | undefined): value is number =>
  typeof value === "number" && Number.isFinite(value) && value > 0;

const nonNegativeFinite = (value: number | null | undefined): value is number =>
  typeof value === "number" && Number.isFinite(value) && value >= 0;

const finiteTimestamp = (value: number | null | undefined): value is number =>
  typeof value === "number" && Number.isFinite(value) && value >= 0;

type RefillEvent = {
  at_ms: number;
  amount_quota: number;
  from_history: boolean;
};

const median = (values: readonly number[]): number | null => {
  const finite = values.filter((value) => positiveFinite(value)).sort((left, right) => left - right);
  if (!finite.length) return null;
  const middle = Math.floor(finite.length / 2);
  return finite.length % 2 === 1 ? finite[middle] ?? null : ((finite[middle - 1] ?? 0) + (finite[middle] ?? 0)) / 2;
};

const normalizedBalanceHistory = (
  history: readonly MeteredQuotaBalanceSample[] | undefined,
): MeteredQuotaBalanceSample[] => {
  const byObservedAt = new Map<number, MeteredQuotaBalanceSample>();
  for (const raw of history ?? []) {
    if (!raw || typeof raw !== "object") continue;
    const sample = raw as MeteredQuotaBalanceSample;
    if (
      !finiteTimestamp(sample.observed_at_ms) ||
      !finiteTimestamp(sample.bucket_start_at_ms) ||
      !nonNegativeFinite(sample.balance_quota) ||
      !nonNegativeFinite(sample.baseline_quota) ||
      !positiveFinite(sample.quota_per_credit) ||
      sample.unlimited_quota === true ||
      typeof sample.total_available === "number" ||
      typeof sample.total_granted === "number" ||
      typeof sample.total_used === "number"
    ) continue;
    const observedAtMs = Math.trunc(sample.observed_at_ms);
    const existing = byObservedAt.get(observedAtMs);
    if (
      !existing ||
      sample.bucket_start_at_ms >= existing.bucket_start_at_ms ||
      sample.observed_at_ms >= existing.observed_at_ms
    ) {
      byObservedAt.set(observedAtMs, { ...sample, observed_at_ms: observedAtMs });
    }
  }
  return [...byObservedAt.values()].sort((left, right) => left.observed_at_ms - right.observed_at_ms);
};

const emptyRefillSchedule = (): MeteredQuotaRefillSchedule => ({
  amount_quota: null,
  amount_credits: null,
  cadence_ms: null,
  rate_quota_per_hour: null,
  next_refill_at_ms: null,
  observed_refill_count: 0,
  source: "none",
});

/**
 * Infer a periodic refill from upward balance/baseline movements. A balance
 * increase is corrected for the burn observed over the gap, while a baseline
 * increase is accepted as a refill even when the refill exactly offsets the
 * burn. The latest refill metadata is authoritative for the latest amount and
 * supplies an event when the retained history does not contain that sample.
 */
export const estimateMeteredQuotaRefillSchedule = (
  quota: MeteredQuotaRunwayView,
  balanceHistory: readonly MeteredQuotaBalanceSample[] = quota.balance_history ?? [],
  nowMs = Date.now(),
  burnQuotaPerHour = 0,
): MeteredQuotaRefillSchedule => {
  const safeNowMs = finiteTimestamp(nowMs) ? Math.trunc(nowMs) : Date.now();
  const samples = normalizedBalanceHistory(balanceHistory);
  const quotaPerCredit = positiveFinite(quota.quota_per_credit)
    ? quota.quota_per_credit
    : median(samples.map((sample) => sample.quota_per_credit));
  const events: RefillEvent[] = [];
  const addEvent = (atMs: number, amountQuota: number, fromHistory: boolean): void => {
    if (
      !finiteTimestamp(atMs) ||
      !positiveFinite(amountQuota) ||
      Math.trunc(atMs) > safeNowMs
    ) return;
    events.push({
      at_ms: Math.trunc(atMs),
      amount_quota: amountQuota,
      from_history: fromHistory,
    });
  };

  for (let index = 1; index < samples.length; index += 1) {
    const previous = samples[index - 1]!;
    const current = samples[index]!;
    const elapsedMs = current.observed_at_ms - previous.observed_at_ms;
    if (!positiveFinite(elapsedMs)) continue;
    const burnDuringGap = positiveFinite(burnQuotaPerHour) ? burnQuotaPerHour * elapsedMs / HOUR_MS : 0;
    const balanceIncrease = current.balance_quota - previous.balance_quota;
    const baselineIncrease = current.baseline_quota - previous.baseline_quota;
    const adjustedBalanceIncrease = balanceIncrease > 0 ? balanceIncrease + burnDuringGap : 0;
    const amountQuota = Math.max(adjustedBalanceIncrease, baselineIncrease > 0 ? baselineIncrease : 0);
    addEvent(current.observed_at_ms, amountQuota, true);
  }

  const latestAmountCredits = positiveFinite(quota.latest_refill_amount_credits)
    ? quota.latest_refill_amount_credits
    : null;
  const latestAmountQuotaCandidate = latestAmountCredits !== null && quotaPerCredit !== null
    ? latestAmountCredits * quotaPerCredit
    : null;
  const latestAmountQuota = positiveFinite(latestAmountQuotaCandidate) ? latestAmountQuotaCandidate : null;
  const latestAtMs = finiteTimestamp(quota.latest_refill_completed_at_ms)
    ? Math.trunc(quota.latest_refill_completed_at_ms)
    : finiteTimestamp(quota.last_credit_at_ms)
    ? Math.trunc(quota.last_credit_at_ms)
    : null;

  if (latestAmountQuota !== null && latestAtMs !== null && latestAtMs <= safeNowMs) {
    const matchingEvent = events.find((event) => Math.abs(event.at_ms - latestAtMs) < HOUR_MS);
    if (matchingEvent) {
      matchingEvent.amount_quota = latestAmountQuota;
    } else {
      addEvent(latestAtMs, latestAmountQuota, false);
    }

    // Some upstream snapshots expose a cycle start separately from the
    // refill completion. When both are distinct, it is useful as the prior
    // cadence anchor even if the older sample has already left the response.
    const cycleStartedAtMs = finiteTimestamp(quota.cycle_started_at_ms) ? Math.trunc(quota.cycle_started_at_ms) : null;
    if (
      cycleStartedAtMs !== null &&
      cycleStartedAtMs < latestAtMs &&
      latestAtMs - cycleStartedAtMs >= HOUR_MS &&
      !events.some((event) => Math.abs(event.at_ms - cycleStartedAtMs) < HOUR_MS)
    ) {
      addEvent(cycleStartedAtMs, latestAmountQuota, false);
    }
  }

  events.sort((left, right) => left.at_ms - right.at_ms);
  const distinctEvents: RefillEvent[] = [];
  for (const event of events) {
    const previous = distinctEvents.at(-1);
    if (previous && event.at_ms - previous.at_ms < HOUR_MS) {
      previous.amount_quota = Math.max(previous.amount_quota, event.amount_quota);
      previous.from_history = previous.from_history || event.from_history;
    } else {
      distinctEvents.push({ ...event });
    }
  }

  const intervals: number[] = [];
  for (let index = 1; index < distinctEvents.length; index += 1) {
    const intervalMs = distinctEvents[index]!.at_ms - distinctEvents[index - 1]!.at_ms;
    if (positiveFinite(intervalMs)) intervals.push(intervalMs);
  }
  const cadenceMs = median(intervals);
  const amountQuota = latestAmountQuota ?? median(distinctEvents.map((event) => event.amount_quota));
  const amountCredits = amountQuota !== null && quotaPerCredit !== null
    ? amountQuota / quotaPerCredit
    : latestAmountCredits;
  const rateQuotaPerHourCandidate = amountQuota !== null && positiveFinite(cadenceMs)
    ? amountQuota / cadenceMs * HOUR_MS
    : null;
  const rateQuotaPerHour = positiveFinite(rateQuotaPerHourCandidate) ? rateQuotaPerHourCandidate : null;
  const source = distinctEvents.some((event) => event.from_history)
    ? "balance_history"
    : distinctEvents.length
    ? "latest_refill"
    : "none";

  let nextRefillAtMs: number | null = null;
  if (positiveFinite(cadenceMs) && distinctEvents.length) {
    const lastEventAtMs = distinctEvents[distinctEvents.length - 1]!.at_ms;
    const anchorAtMs = latestAtMs !== null ? Math.max(lastEventAtMs, latestAtMs) : lastEventAtMs;
    const periodsElapsed = safeNowMs >= anchorAtMs ? Math.floor((safeNowMs - anchorAtMs) / cadenceMs) + 1 : 0;
    const candidate = anchorAtMs + periodsElapsed * cadenceMs;
    if (finiteTimestamp(candidate)) nextRefillAtMs = Math.trunc(candidate);
  }

  return {
    amount_quota: amountQuota,
    amount_credits: amountCredits,
    cadence_ms: cadenceMs,
    rate_quota_per_hour: rateQuotaPerHour,
    next_refill_at_ms: nextRefillAtMs,
    observed_refill_count: distinctEvents.length,
    source,
  };
};

type RefillTimeProjection = Readonly<{
  time_remaining_ms: number | null;
  exhausted_at_ms: number | null;
}>;

/**
 * Apply discrete refills at the inferred cadence. This keeps the next
 * scheduled refill visible instead of smearing a large cycle credit across
 * time, which would make a soon-due refill look later than it is.
 */
const projectWithKnownRefill = (
  balanceQuota: number | null,
  quotaPerHour: number | null,
  schedule: MeteredQuotaRefillSchedule,
  nowMs: number,
): RefillTimeProjection => {
  if (
    balanceQuota === null ||
    !positiveFinite(quotaPerHour) ||
    !positiveFinite(schedule.amount_quota) ||
    !positiveFinite(schedule.cadence_ms) ||
    !positiveFinite(schedule.rate_quota_per_hour) ||
    schedule.next_refill_at_ms === null
  ) {
    return { time_remaining_ms: null, exhausted_at_ms: null };
  }
  const nextRefillAtMs = Math.max(nowMs, schedule.next_refill_at_ms);
  const timeUntilFirstRefillMs = Math.max(0, nextRefillAtMs - nowMs);
  const burnUntilFirstRefill = quotaPerHour * timeUntilFirstRefillMs / HOUR_MS;
  if (balanceQuota < burnUntilFirstRefill) {
    const timeRemainingMs = Math.trunc(balanceQuota / quotaPerHour * HOUR_MS);
    return {
      time_remaining_ms: timeRemainingMs,
      exhausted_at_ms: nowMs + timeRemainingMs,
    };
  }

  const balanceAfterFirstRefill = balanceQuota - burnUntilFirstRefill + schedule.amount_quota;
  const burnPerCadence = quotaPerHour * schedule.cadence_ms / HOUR_MS;
  const netLossPerCadence = burnPerCadence - schedule.amount_quota;
  if (!positiveFinite(netLossPerCadence)) return { time_remaining_ms: null, exhausted_at_ms: null };

  // The first refill has already been applied above. Find the first later
  // cadence interval that cannot reach its next refill without exhausting.
  const intervalsUntilExhaustion = Math.max(
    1,
    Math.floor((balanceAfterFirstRefill - burnPerCadence) / netLossPerCadence) + 2,
  );
  const balanceBeforeExhaustion = Math.max(
    0,
    balanceAfterFirstRefill - (intervalsUntilExhaustion - 1) * netLossPerCadence,
  );
  const exhaustedAtMs = nextRefillAtMs +
    (intervalsUntilExhaustion - 1) * schedule.cadence_ms +
    balanceBeforeExhaustion / quotaPerHour * HOUR_MS;
  if (!finiteTimestamp(exhaustedAtMs)) return { time_remaining_ms: null, exhausted_at_ms: null };
  return {
    time_remaining_ms: Math.max(0, Math.trunc(exhaustedAtMs - nowMs)),
    exhausted_at_ms: Math.trunc(exhaustedAtMs),
  };
};

const projectionLabel = (
  status: QuotaRefillStatus,
  noRefillExhaustedAtMs: number | null,
  withRefillExhaustedAtMs: number | null,
): string | null => {
  const dateText = (timestamp: number | null): string | null => {
    if (timestamp === null || !finiteTimestamp(timestamp)) return null;
    try {
      return new Date(timestamp).toISOString();
    } catch {
      return null;
    }
  };
  if (status === "sustainable") return "sustainable";
  if (status === "with_refill") {
    const date = dateText(withRefillExhaustedAtMs);
    return date === null
      ? "exhaustion unknown (with known refill schedule)"
      : "exhausts at " + date + " (with known refill schedule)";
  }
  if (status === "no_refill") {
    const date = dateText(noRefillExhaustedAtMs);
    return date === null ? "exhaustion unknown (no refill)" : "exhausts at " + date + " (no refill)";
  }
  return null;
};

/**
 * Balance the projection is drawn against. Wallet mode uses the live wallet
 * balance. Token-usage mode reports total_available as the remaining
 * inventory (Available tokens), so it is used directly. Otherwise there is no
 * authoritative balance and estimates are unknown rather than fabricated.
 */
const runwayBalance = (quota: MeteredQuotaRunwayView): number | null => {
  if (quota.unlimited_quota) return null;
  if (quota.balance_quota !== null) return Math.max(0, quota.balance_quota);
  if (quota.total_available !== null) return Math.max(0, quota.total_available);
  return null;
};

const runwayBaseline = (quota: MeteredQuotaRunwayView): number | null => {
  if (quota.unlimited_quota) return null;
  if (quota.baseline_quota !== null) return Math.max(0, quota.baseline_quota);
  return quota.total_granted !== null && quota.total_granted > 0 ? quota.total_granted : null;
};

/**
 * Projections are only meaningful for the provider whose balance is being
 * monitored. `METERED_API_KEY` monitors the OpenLux account, which serves
 * `provider: "metered"` rows; Surplus Intelligence has its own billing with
 * no quota snapshot in this gateway. Returning no estimate for other
 * providers avoids reporting Surplus history against the OpenLux balance.
 */

const isQuotaProjectionOptions = (value: unknown): value is QuotaProjectionOptions =>
  typeof value === "object" && value !== null && !Array.isArray(value);

export function projectPaidFallbackRunway(
  usage: PaidFallbackModelUsage,
  quota: MeteredQuotaRunwayView,
  nowMs: number,
  historyOrOptions?: readonly MeteredQuotaBalanceSample[] | QuotaProjectionOptions,
): PaidFallbackRunwayEstimate[];
export function projectPaidFallbackRunway(
  usage: PaidFallbackModelUsage,
  quota: MeteredQuotaRunwayView,
  balanceHistory: readonly MeteredQuotaBalanceSample[],
  nowMs: number,
): PaidFallbackRunwayEstimate[];
export function projectPaidFallbackRunway(
  usage: PaidFallbackModelUsage,
  quota: MeteredQuotaRunwayView,
  options: QuotaProjectionOptions,
  nowMs?: number,
): PaidFallbackRunwayEstimate[];
export function projectPaidFallbackRunway(
  usage: PaidFallbackModelUsage,
  quota: MeteredQuotaRunwayView,
  nowMsOrHistory: number | readonly MeteredQuotaBalanceSample[] | QuotaProjectionOptions,
  historyOrNow?: number | readonly MeteredQuotaBalanceSample[] | QuotaProjectionOptions,
): PaidFallbackRunwayEstimate[] {
  if (usage.provider !== "metered") return [];

  let nowMs = Date.now();
  let balanceHistory = quota.balance_history;
  const applyProjectionArgument = (
    argument: number | readonly MeteredQuotaBalanceSample[] | QuotaProjectionOptions | undefined,
  ): void => {
    if (typeof argument === "number") {
      nowMs = argument;
    } else if (Array.isArray(argument)) {
      balanceHistory = argument;
    } else if (isQuotaProjectionOptions(argument)) {
      balanceHistory = argument.balance_history ?? argument.balanceHistory ?? balanceHistory;
    }
  };
  applyProjectionArgument(nowMsOrHistory);
  applyProjectionArgument(historyOrNow);

  const safeNowMs = finiteTimestamp(nowMs) ? Math.trunc(nowMs) : Date.now();
  const balanceQuota = runwayBalance(quota);
  const baselineQuota = runwayBaseline(quota);
  const unlimited = quota.unlimited_quota === true;
  const staleBalance = quota.cache_state === "stale";

  return usage.windows.map((window) => {
    const avgQuota = window.avg_quota_per_request;
    const quotaPerHour = window.quota_per_hour;
    const noRefillRequestsRemaining = balanceQuota !== null && positiveFinite(avgQuota)
      ? Math.floor(balanceQuota / avgQuota)
      : null;
    const noRefillTimeRemainingMs = balanceQuota !== null && positiveFinite(quotaPerHour)
      ? Math.trunc(balanceQuota / quotaPerHour * HOUR_MS)
      : null;
    const noRefillExhaustedAtMs = noRefillTimeRemainingMs !== null ? safeNowMs + noRefillTimeRemainingMs : null;
    const schedule = unlimited
      ? emptyRefillSchedule()
      : estimateMeteredQuotaRefillSchedule(quota, balanceHistory, safeNowMs, quotaPerHour ?? 0);

    let refillStatus: QuotaRefillStatus;
    let withRefill: RefillTimeProjection = { time_remaining_ms: null, exhausted_at_ms: null };
    if (unlimited) {
      refillStatus = "unlimited";
    } else if (
      positiveFinite(schedule.amount_quota) &&
      positiveFinite(schedule.cadence_ms) &&
      positiveFinite(schedule.rate_quota_per_hour) &&
      positiveFinite(quotaPerHour) &&
      schedule.rate_quota_per_hour >= quotaPerHour
    ) {
      refillStatus = "sustainable";
    } else if (
      positiveFinite(schedule.amount_quota) &&
      positiveFinite(schedule.cadence_ms) &&
      positiveFinite(schedule.rate_quota_per_hour) &&
      positiveFinite(quotaPerHour) &&
      balanceQuota !== null
    ) {
      withRefill = projectWithKnownRefill(balanceQuota, quotaPerHour, schedule, safeNowMs);
      refillStatus = withRefill.time_remaining_ms !== null ? "with_refill" : "unknown";
    } else {
      refillStatus = noRefillTimeRemainingMs !== null ? "no_refill" : "unknown";
    }

    const withRefillRequestsRemaining = withRefill.time_remaining_ms !== null &&
        positiveFinite(avgQuota) &&
        positiveFinite(quotaPerHour)
      ? Math.floor(withRefill.time_remaining_ms / HOUR_MS * quotaPerHour / avgQuota)
      : null;
    const label = projectionLabel(refillStatus, noRefillExhaustedAtMs, withRefill.exhausted_at_ms);
    const refillProjection: QuotaRefillProjection = {
      status: refillStatus,
      no_refill_exhausted_at_ms: noRefillExhaustedAtMs,
      with_refill_exhausted_at_ms: withRefill.exhausted_at_ms,
      label,
      schedule,
    };

    return {
      window_days: window.window_days,
      unlimited,
      stale_balance: staleBalance,
      requests_remaining: noRefillRequestsRemaining,
      time_remaining_ms: noRefillTimeRemainingMs,
      exhausted_at_ms: noRefillExhaustedAtMs,
      percent_per_request_vs_balance: balanceQuota !== null && positiveFinite(avgQuota) && balanceQuota > 0
        ? avgQuota / balanceQuota * 100
        : null,
      percent_per_request_vs_baseline: baselineQuota !== null && positiveFinite(avgQuota) && baselineQuota > 0
        ? avgQuota / baselineQuota * 100
        : null,
      no_refill_requests_remaining: noRefillRequestsRemaining,
      no_refill_time_remaining_ms: noRefillTimeRemainingMs,
      no_refill_exhausted_at_ms: noRefillExhaustedAtMs,
      refill_status: refillStatus,
      refill_amount_quota: schedule.amount_quota,
      refill_amount_credits: schedule.amount_credits,
      refill_cadence_ms: schedule.cadence_ms,
      refill_rate_quota_per_hour: schedule.rate_quota_per_hour,
      next_refill_at_ms: schedule.next_refill_at_ms,
      with_refill_requests_remaining: withRefillRequestsRemaining,
      with_refill_time_remaining_ms: withRefill.time_remaining_ms,
      with_refill_exhausted_at_ms: withRefill.exhausted_at_ms,
      refill_projection_label: label,
      refill_projection: refillProjection,
    };
  });
}
