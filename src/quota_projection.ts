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

const finiteTimestamp = (value: number | null | undefined): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const medianPositive = (values: readonly number[]): number | null => {
  const sorted = values.filter(positiveFinite).sort((left, right) => left - right);
  if (!sorted.length) return null;
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 === 1 ? sorted[middle] ?? null : ((sorted[middle - 1] ?? 0) + (sorted[middle] ?? 0)) / 2;
};

/**
 * Facts inferred from the retained wallet samples. All quota values use the
 * same units as the Metered wallet balance; credit values are provided for
 * the admin view as a convenience.
 */
export type MeteredQuotaRefillSchedule = Readonly<{
  amount_quota: number | null;
  amount_credits: number | null;
  cadence_ms: number | null;
  rate_quota_per_hour: number | null;
  next_refill_at_ms: number | null;
  observed_refill_count: number;
  source: "none" | "latest_refill" | "balance_history";
  /** Timestamp of the latest observed refill, when one is known. */
  last_refill_at_ms?: number | null;
  /** Compatibility alias for observed_refill_count. */
  known_refill_count?: number;
}>;

const EMPTY_REFILL_SCHEDULE: MeteredQuotaRefillSchedule = {
  amount_quota: null,
  amount_credits: null,
  cadence_ms: null,
  rate_quota_per_hour: null,
  next_refill_at_ms: null,
  observed_refill_count: 0,
  source: "none",
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
  /** Retained wallet samples supplied by a caller that already read them. */
  balance_history?: readonly MeteredQuotaBalanceSample[];
  /** Refill facts inferred from the retained samples and snapshot metadata. */
  refill_schedule?: MeteredQuotaRefillSchedule;
}>;

export type QuotaProjectionOptions = Readonly<{
  balance_history?: readonly MeteredQuotaBalanceSample[];
  /** Camel-case alias for callers outside the HTTP layer. */
  balanceHistory?: readonly MeteredQuotaBalanceSample[];
  /** Short alias kept for pure callers that already call this a history. */
  history?: readonly MeteredQuotaBalanceSample[];
}>;

type QuotaProjectionRefillInput = readonly MeteredQuotaBalanceSample[] | QuotaProjectionOptions;

const refillHistoryFromInput = (
  input: QuotaProjectionRefillInput | undefined,
): readonly MeteredQuotaBalanceSample[] => {
  if (!input) return [];
  if (Array.isArray(input)) return input;
  const options = input as QuotaProjectionOptions;
  return options.balance_history ?? options.balanceHistory ?? options.history ?? [];
};

type RefillEvent = {
  at_ms: number;
  amount_quota: number;
  from_history: boolean;
  authoritative: boolean;
};

const REFILL_EVENT_MATCH_TOLERANCE_MS = 30 * 60 * 1_000;

const normalizedBalanceHistory = (
  history: readonly MeteredQuotaBalanceSample[] | undefined,
): MeteredQuotaBalanceSample[] => {
  const byBucket = new Map<number, MeteredQuotaBalanceSample>();
  for (const raw of history ?? []) {
    if (!raw || typeof raw !== "object") continue;
    const sample = raw as MeteredQuotaBalanceSample;
    if (
      sample.v !== 1 ||
      !finiteTimestamp(sample.bucket_start_at_ms) ||
      !finiteTimestamp(sample.observed_at_ms) ||
      !nonNegativeFinite(sample.balance_quota) ||
      !nonNegativeFinite(sample.baseline_quota) ||
      !positiveFinite(sample.quota_per_credit) ||
      sample.unlimited_quota === true ||
      sample.total_available !== undefined && sample.total_available !== null ||
      sample.total_granted !== undefined && sample.total_granted !== null ||
      sample.total_used !== undefined && sample.total_used !== null
    ) continue;
    const existing = byBucket.get(sample.bucket_start_at_ms);
    if (!existing || sample.observed_at_ms > existing.observed_at_ms) {
      byBucket.set(sample.bucket_start_at_ms, sample);
    }
  }
  return [...byBucket.values()].sort(
    (left, right) =>
      left.observed_at_ms - right.observed_at_ms ||
      left.bucket_start_at_ms - right.bucket_start_at_ms,
  );
};

const normalizeRefillSchedule = (
  schedule: MeteredQuotaRefillSchedule | undefined,
): MeteredQuotaRefillSchedule => {
  if (!schedule) return EMPTY_REFILL_SCHEDULE;
  const amount = positiveFinite(schedule.amount_quota) ? schedule.amount_quota : null;
  const amountCredits = positiveFinite(schedule.amount_credits) ? schedule.amount_credits : null;
  const cadence = positiveFinite(schedule.cadence_ms) ? schedule.cadence_ms : null;
  const rate = amount !== null && cadence !== null ? amount / cadence * HOUR_MS : null;
  const next = finiteTimestamp(schedule.next_refill_at_ms) ? schedule.next_refill_at_ms : null;
  const last = finiteTimestamp(schedule.last_refill_at_ms) ? schedule.last_refill_at_ms : null;
  const observedCount = Number.isSafeInteger(schedule.observed_refill_count) &&
      schedule.observed_refill_count > 0
    ? schedule.observed_refill_count
    : 0;
  const knownRefillCount = schedule.known_refill_count;
  const knownCount =
    typeof knownRefillCount === "number" && Number.isSafeInteger(knownRefillCount) && knownRefillCount > 0
      ? knownRefillCount
      : 0;
  return {
    amount_quota: amount,
    amount_credits: amountCredits,
    cadence_ms: cadence,
    rate_quota_per_hour: positiveFinite(rate) ? rate : null,
    next_refill_at_ms: next,
    observed_refill_count: Math.max(observedCount, knownCount),
    source: schedule.source === "balance_history" || schedule.source === "latest_refill" ? schedule.source : "none",
    last_refill_at_ms: last,
    known_refill_count: Math.max(observedCount, knownCount),
  };
};

const refillAmountFromMetadata = (
  quota: Pick<MeteredQuotaRunwayView, "latest_refill_amount_credits" | "quota_per_credit">,
): { amount_quota: number; amount_credits: number } | null => {
  if (!positiveFinite(quota.latest_refill_amount_credits) || !positiveFinite(quota.quota_per_credit)) {
    return null;
  }
  const amountQuota = quota.latest_refill_amount_credits * quota.quota_per_credit;
  return Number.isFinite(amountQuota) && amountQuota > 0
    ? {
      amount_quota: amountQuota,
      amount_credits: quota.latest_refill_amount_credits,
    }
    : null;
};

const closestEvent = (
  events: readonly RefillEvent[],
  atMs: number,
): { event: RefillEvent; distance_ms: number } | null => {
  let closest: RefillEvent | null = null;
  let distance = Number.POSITIVE_INFINITY;
  for (const event of events) {
    const candidateDistance = Math.abs(event.at_ms - atMs);
    if (candidateDistance < distance) {
      closest = event;
      distance = candidateDistance;
    }
  }
  return closest ? { event: closest, distance_ms: distance } : null;
};

/**
 * Infer a periodic refill schedule from retained hourly balance samples and
 * the latest refill facts in the Metered snapshot. Baseline increases are a
 * refill signal that is not obscured by concurrent debits. Balance increases
 * are also accepted, after restoring the burn observed between samples.
 *
 * A cadence is only reported when at least two refill observations establish
 * an interval. A single refill amount remains visible, but is not projected
 * into the future as a made-up schedule.
 */
export const estimateMeteredQuotaRefillSchedule = (
  quota: MeteredQuotaRunwayView,
  balanceHistory: readonly MeteredQuotaBalanceSample[] = quota.balance_history ?? [],
  nowMs = Date.now(),
  burnQuotaPerHour = 0,
): MeteredQuotaRefillSchedule => {
  const safeNowMs = finiteTimestamp(nowMs) ? nowMs : Date.now();
  const samples = normalizedBalanceHistory(balanceHistory);
  const existing = normalizeRefillSchedule(quota.refill_schedule);
  const burnRate = positiveFinite(burnQuotaPerHour) ? burnQuotaPerHour : 0;
  const events: RefillEvent[] = [];

  const addEvent = (
    atMs: number,
    amountQuota: number,
    fromHistory: boolean,
    authoritative = false,
  ): void => {
    if (
      !finiteTimestamp(atMs) ||
      atMs > safeNowMs ||
      !positiveFinite(amountQuota) ||
      !Number.isFinite(amountQuota)
    ) return;
    events.push({
      at_ms: atMs,
      amount_quota: amountQuota,
      from_history: fromHistory,
      authoritative,
    });
  };

  for (let index = 1; index < samples.length; index += 1) {
    const previous = samples[index - 1]!;
    const current = samples[index]!;
    const elapsedMs = current.observed_at_ms - previous.observed_at_ms;
    if (!positiveFinite(elapsedMs)) continue;
    const balanceIncrease = current.balance_quota - previous.balance_quota;
    const baselineIncrease = current.baseline_quota - previous.baseline_quota;
    const burnDuringGap = burnRate * elapsedMs / HOUR_MS;
    const adjustedBalanceIncrease = balanceIncrease > 0 ? balanceIncrease + burnDuringGap : 0;
    const amountQuota = Math.max(
      adjustedBalanceIncrease,
      baselineIncrease > 0 ? baselineIncrease : 0,
    );
    addEvent(current.observed_at_ms, amountQuota, true);
  }

  const metadataAmount = refillAmountFromMetadata(quota);
  const metadataAtMs = finiteTimestamp(quota.latest_refill_completed_at_ms)
    ? quota.latest_refill_completed_at_ms
    : finiteTimestamp(quota.last_credit_at_ms)
    ? quota.last_credit_at_ms
    : finiteTimestamp(quota.cycle_started_at_ms)
    ? quota.cycle_started_at_ms
    : null;

  if (metadataAmount && metadataAtMs !== null && metadataAtMs <= safeNowMs) {
    const match = closestEvent(events, metadataAtMs);
    if (match && match.distance_ms <= REFILL_EVENT_MATCH_TOLERANCE_MS) {
      match.event.at_ms = metadataAtMs;
      match.event.amount_quota = metadataAmount.amount_quota;
      match.event.authoritative = true;
    } else {
      addEvent(metadataAtMs, metadataAmount.amount_quota, false, true);
    }

    // A distinct cycle start can be the previous cycle's refill anchor when
    // the provider exposes it separately from completion.
    const cycleStartedAtMs = finiteTimestamp(quota.cycle_started_at_ms) ? quota.cycle_started_at_ms : null;
    if (
      cycleStartedAtMs !== null &&
      cycleStartedAtMs < metadataAtMs &&
      metadataAtMs - cycleStartedAtMs >= HOUR_MS
    ) {
      const cycleMatch = closestEvent(events, cycleStartedAtMs);
      if (!cycleMatch || cycleMatch.distance_ms > REFILL_EVENT_MATCH_TOLERANCE_MS) {
        addEvent(cycleStartedAtMs, metadataAmount.amount_quota, false, false);
      }
    }
  }

  events.sort((left, right) => left.at_ms - right.at_ms);
  const distinctEvents: RefillEvent[] = [];
  for (const event of events) {
    const previous = distinctEvents.at(-1);
    if (previous && event.at_ms - previous.at_ms <= REFILL_EVENT_MATCH_TOLERANCE_MS) {
      if (event.authoritative || !previous.authoritative) {
        previous.amount_quota = event.amount_quota;
        previous.at_ms = event.at_ms;
      }
      previous.from_history ||= event.from_history;
      previous.authoritative ||= event.authoritative;
    } else {
      distinctEvents.push({ ...event });
    }
  }

  const intervals = distinctEvents.slice(1).map(
    (event, index) => event.at_ms - distinctEvents[index]!.at_ms,
  );
  const cadenceFromHistory = medianPositive(intervals);
  const cadence = cadenceFromHistory ?? existing.cadence_ms;
  const latestEvent = distinctEvents.at(-1);
  const amount = metadataAmount?.amount_quota ?? latestEvent?.amount_quota ?? existing.amount_quota;
  const amountCredits = metadataAmount?.amount_credits ??
    (amount !== null && amount !== undefined && positiveFinite(quota.quota_per_credit)
      ? amount / quota.quota_per_credit
      : existing.amount_credits);
  const lastRefillAt = latestEvent?.at_ms ?? existing.last_refill_at_ms ??
    (existing.next_refill_at_ms !== null && cadence !== null ? existing.next_refill_at_ms - cadence : null);
  const rate = amount !== null && amount !== undefined && cadence !== null ? amount / cadence * HOUR_MS : null;

  let nextRefillAtMs = existing.next_refill_at_ms;
  if (cadence !== null && nonNegativeFinite(lastRefillAt)) {
    if (lastRefillAt > safeNowMs) {
      nextRefillAtMs = lastRefillAt;
    } else {
      const periodsElapsed = Math.floor((safeNowMs - lastRefillAt) / cadence) + 1;
      const candidate = lastRefillAt + periodsElapsed * cadence;
      nextRefillAtMs = finiteTimestamp(candidate) ? candidate : null;
    }
  }

  const source = distinctEvents.some((event) => event.from_history)
    ? "balance_history"
    : distinctEvents.length || amount !== null
    ? "latest_refill"
    : existing.source;
  const observedRefillCount = Math.max(
    distinctEvents.length,
    existing.observed_refill_count,
    existing.known_refill_count ?? 0,
  );
  return {
    amount_quota: positiveFinite(amount) ? amount : null,
    amount_credits: positiveFinite(amountCredits) ? amountCredits : null,
    cadence_ms: positiveFinite(cadence) ? cadence : null,
    rate_quota_per_hour: positiveFinite(rate) ? rate : null,
    next_refill_at_ms: finiteTimestamp(nextRefillAtMs) ? nextRefillAtMs : null,
    observed_refill_count: observedRefillCount,
    source,
    last_refill_at_ms: finiteTimestamp(lastRefillAt) ? lastRefillAt : null,
    known_refill_count: observedRefillCount,
  };
};

// Shorter alias for callers that do not need the historical name.
export const estimateMeteredRefillSchedule = estimateMeteredQuotaRefillSchedule;
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
      ...(refillInput === undefined ? {} : { balance_history: balanceHistory }),
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
    ...(refillInput === undefined ? {} : { balance_history: balanceHistory }),
  } satisfies MeteredQuotaRunwayView;
  return {
    ...view,
    refill_schedule: tokenUsage ? EMPTY_REFILL_SCHEDULE : estimateMeteredQuotaRefillSchedule(
      view,
      balanceHistory,
      state.observed_at_ms,
    ),
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

export type QuotaRefillStatus =
  | "unknown"
  | "unlimited"
  | "no_refill"
  | "with_refill"
  | "sustainable"
  /** Compatibility spelling for callers that describe the schedule explicitly. */
  | "known_refill_schedule"
  | "unavailable";

export type PaidFallbackRefillStatus = QuotaRefillStatus;

export type QuotaRefillProjection = Readonly<{
  status: QuotaRefillStatus;
  no_refill_exhausted_at_ms: number | null;
  with_refill_exhausted_at_ms: number | null;
  label: string | null;
  schedule: MeteredQuotaRefillSchedule;
}>;

export type PaidFallbackRunwayEstimate = Readonly<{
  window_days: QuotaProjectionWindowDays;
  unlimited: boolean;
  /** True when the balance snapshot is older than the freshness window. */
  stale_balance: boolean;
  /** Existing fields remain the conservative no-refill result. */
  requests_remaining: number | null;
  time_remaining_ms: number | null;
  exhausted_at_ms: number | null;
  percent_per_request_vs_balance: number | null;
  percent_per_request_vs_baseline: number | null;
  no_refill_requests_remaining: number | null;
  no_refill_time_remaining_ms: number | null;
  no_refill_exhausted_at_ms: number | null;
  /** Result after applying the inferred refill schedule. */
  refill_status: QuotaRefillStatus;
  refill_status_code: QuotaRefillStatus;
  refill_amount_quota: number | null;
  refill_amount_credits: number | null;
  refill_cadence_ms: number | null;
  refill_rate_quota_per_hour: number | null;
  next_refill_at_ms: number | null;
  with_refill_requests_remaining: number | null;
  with_refill_time_remaining_ms: number | null;
  with_refill_exhausted_at_ms: number | null;
  refill_projection_label: string | null;
  refill_schedule: MeteredQuotaRefillSchedule;
  refill_projection: QuotaRefillProjection;
  /** Short aliases for clients that call the second result refill-aware. */
  refill_requests_remaining: number | null;
  refill_time_remaining_ms: number | null;
  refill_exhausted_at_ms: number | null;
}>;

type RefillTimeProjection =
  | Readonly<{
    kind: "unknown";
    time_remaining_ms: null;
    exhausted_at_ms: null;
    affected_by_refill: false;
  }>
  | Readonly<{
    kind: "sustainable";
    time_remaining_ms: null;
    exhausted_at_ms: null;
    affected_by_refill: true;
  }>
  | Readonly<{
    kind: "finite";
    time_remaining_ms: number;
    exhausted_at_ms: number;
    affected_by_refill: boolean;
  }>;

const nextScheduledRefillAt = (
  schedule: MeteredQuotaRefillSchedule,
  nowMs: number,
): number | null => {
  if (!finiteTimestamp(nowMs) || !positiveFinite(schedule.cadence_ms)) return null;
  const explicitNext = finiteTimestamp(schedule.next_refill_at_ms) ? schedule.next_refill_at_ms : null;
  if (explicitNext !== null && explicitNext > nowMs) return explicitNext;
  const anchor = finiteTimestamp(schedule.last_refill_at_ms)
    ? schedule.last_refill_at_ms
    : explicitNext !== null
    ? Math.trunc(explicitNext - schedule.cadence_ms)
    : null;
  if (anchor === null) return null;
  if (anchor > nowMs) return anchor;
  const periodsElapsed = Math.floor((nowMs - anchor) / schedule.cadence_ms) + 1;
  const candidate = anchor + periodsElapsed * schedule.cadence_ms;
  return finiteTimestamp(Math.trunc(candidate)) ? Math.trunc(candidate) : null;
};

/**
 * Apply a discrete recurring refill schedule. The balance is allowed to
 * reach zero exactly at a refill boundary because the refill is available at
 * that instant. Exhaustion before the next refill remains a no-refill result.
 */
const projectWithKnownRefill = (
  balanceQuota: number | null,
  quotaPerHour: number | null,
  schedule: MeteredQuotaRefillSchedule,
  nowMs: number,
): RefillTimeProjection => {
  const nextRefillAtMs = nextScheduledRefillAt(schedule, nowMs);
  if (
    balanceQuota === null ||
    !nonNegativeFinite(balanceQuota) ||
    !positiveFinite(quotaPerHour) ||
    !positiveFinite(schedule.amount_quota) ||
    !positiveFinite(schedule.cadence_ms) ||
    nextRefillAtMs === null
  ) {
    return {
      kind: "unknown",
      time_remaining_ms: null,
      exhausted_at_ms: null,
      affected_by_refill: false,
    };
  }

  const untilNextRefillMs = nextRefillAtMs - nowMs;
  const burnBeforeRefill = quotaPerHour * untilNextRefillMs / HOUR_MS;
  if (!Number.isFinite(burnBeforeRefill)) {
    return {
      kind: "unknown",
      time_remaining_ms: null,
      exhausted_at_ms: null,
      affected_by_refill: false,
    };
  }
  if (balanceQuota < burnBeforeRefill) {
    const timeRemainingMs = Math.max(0, Math.trunc(balanceQuota / quotaPerHour * HOUR_MS));
    return {
      kind: "finite",
      time_remaining_ms: timeRemainingMs,
      exhausted_at_ms: nowMs + timeRemainingMs,
      affected_by_refill: false,
    };
  }

  const burnPerCadence = quotaPerHour * schedule.cadence_ms / HOUR_MS;
  if (!positiveFinite(burnPerCadence)) {
    return {
      kind: "unknown",
      time_remaining_ms: null,
      exhausted_at_ms: null,
      affected_by_refill: false,
    };
  }
  if (schedule.amount_quota >= burnPerCadence) {
    return {
      kind: "sustainable",
      time_remaining_ms: null,
      exhausted_at_ms: null,
      affected_by_refill: true,
    };
  }

  const balanceAfterRefill = balanceQuota - burnBeforeRefill + schedule.amount_quota;
  const deficitPerCycle = burnPerCadence - schedule.amount_quota;
  if (!Number.isFinite(balanceAfterRefill) || !positiveFinite(deficitPerCycle)) {
    return {
      kind: "unknown",
      time_remaining_ms: null,
      exhausted_at_ms: null,
      affected_by_refill: false,
    };
  }

  // B_k is the balance immediately after the k-th refill following the first
  // one. Find the first k for which the next cadence cannot be reached.
  const cyclesBeforeExhaustion = balanceAfterRefill < burnPerCadence
    ? 0
    : Math.floor((balanceAfterRefill - burnPerCadence) / deficitPerCycle) + 1;
  const balanceAtExhaustionCycle = balanceAfterRefill - cyclesBeforeExhaustion * deficitPerCycle;
  const exhaustedAtMs = nextRefillAtMs +
    cyclesBeforeExhaustion * schedule.cadence_ms +
    Math.max(0, balanceAtExhaustionCycle) / quotaPerHour * HOUR_MS;
  if (!Number.isFinite(exhaustedAtMs) || exhaustedAtMs < nowMs) {
    return {
      kind: "unknown",
      time_remaining_ms: null,
      exhausted_at_ms: null,
      affected_by_refill: false,
    };
  }
  const exhaustedAt = Math.trunc(exhaustedAtMs);
  return finiteTimestamp(exhaustedAt)
    ? {
      kind: "finite",
      time_remaining_ms: Math.max(0, exhaustedAt - nowMs),
      exhausted_at_ms: exhaustedAt,
      affected_by_refill: true,
    }
    : {
      kind: "unknown",
      time_remaining_ms: null,
      exhausted_at_ms: null,
      affected_by_refill: false,
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
 * The optional retained balance history is accepted in either argument order
 * so the original (usage, quota, now) call remains source compatible.
 */
export function projectPaidFallbackRunway(
  usage: PaidFallbackModelUsage,
  quota: MeteredQuotaRunwayView,
  nowMs: number,
  refillInput?: QuotaProjectionRefillInput,
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
  nowMsOrInput: number | QuotaProjectionRefillInput,
  inputOrNow?: number | QuotaProjectionRefillInput,
): PaidFallbackRunwayEstimate[] {
  if (usage.provider !== "metered") return [];

  let nowMs = Date.now();
  let refillInput: QuotaProjectionRefillInput | undefined;
  const applyArgument = (argument: number | QuotaProjectionRefillInput | undefined): void => {
    if (typeof argument === "number") nowMs = argument;
    else if (argument !== undefined) refillInput = argument;
  };
  applyArgument(nowMsOrInput);
  applyArgument(inputOrNow);

  const safeNowMs = finiteTimestamp(nowMs) ? nowMs : Date.now();
  const balanceHistory = refillHistoryFromInput(refillInput ?? quota.balance_history);
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
    const noRefillExhaustedAtMs = noRefillTimeRemainingMs === null ? null : safeNowMs + noRefillTimeRemainingMs;

    if (unlimited) {
      const emptyProjection: QuotaRefillProjection = {
        status: "unlimited",
        no_refill_exhausted_at_ms: null,
        with_refill_exhausted_at_ms: null,
        label: null,
        schedule: EMPTY_REFILL_SCHEDULE,
      };
      return {
        window_days: window.window_days,
        unlimited: true,
        stale_balance: staleBalance,
        requests_remaining: null,
        time_remaining_ms: null,
        exhausted_at_ms: null,
        percent_per_request_vs_balance: null,
        percent_per_request_vs_baseline: null,
        no_refill_requests_remaining: null,
        no_refill_time_remaining_ms: null,
        no_refill_exhausted_at_ms: null,
        refill_status: "unlimited",
        refill_status_code: "unlimited",
        refill_amount_quota: null,
        refill_amount_credits: null,
        refill_cadence_ms: null,
        refill_rate_quota_per_hour: null,
        next_refill_at_ms: null,
        with_refill_requests_remaining: null,
        with_refill_time_remaining_ms: null,
        with_refill_exhausted_at_ms: null,
        refill_projection_label: null,
        refill_schedule: EMPTY_REFILL_SCHEDULE,
        refill_projection: emptyProjection,
        refill_requests_remaining: null,
        refill_time_remaining_ms: null,
        refill_exhausted_at_ms: null,
      };
    }

    const schedule = estimateMeteredQuotaRefillSchedule(
      quota,
      balanceHistory,
      safeNowMs,
      quotaPerHour ?? 0,
    );
    const refillProjection = projectWithKnownRefill(
      balanceQuota,
      quotaPerHour,
      schedule,
      safeNowMs,
    );
    const refillStatus: QuotaRefillStatus = refillProjection.kind === "sustainable"
      ? "sustainable"
      : refillProjection.kind === "finite" && refillProjection.affected_by_refill
      ? "with_refill"
      : noRefillTimeRemainingMs !== null
      ? "no_refill"
      : "unknown";
    const withRefillTimeRemainingMs = refillProjection.time_remaining_ms;
    const withRefillExhaustedAtMs = refillProjection.exhausted_at_ms;
    const withRefillRequestsRemaining = refillProjection.kind === "sustainable"
      ? null
      : withRefillTimeRemainingMs !== null && positiveFinite(avgQuota) && positiveFinite(quotaPerHour)
      ? Math.floor(withRefillTimeRemainingMs / HOUR_MS * quotaPerHour / avgQuota)
      : refillStatus === "no_refill"
      ? noRefillRequestsRemaining
      : null;
    const label = projectionLabel(
      refillStatus,
      noRefillExhaustedAtMs,
      withRefillExhaustedAtMs,
    );
    const refillProjectionView: QuotaRefillProjection = {
      status: refillStatus,
      no_refill_exhausted_at_ms: noRefillExhaustedAtMs,
      with_refill_exhausted_at_ms: withRefillExhaustedAtMs,
      label,
      schedule,
    };
    return {
      window_days: window.window_days,
      unlimited: false,
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
      refill_status_code: refillStatus === "with_refill" ? "known_refill_schedule" : refillStatus,
      refill_amount_quota: schedule.amount_quota,
      refill_amount_credits: schedule.amount_credits,
      refill_cadence_ms: schedule.cadence_ms,
      refill_rate_quota_per_hour: schedule.rate_quota_per_hour,
      next_refill_at_ms: schedule.next_refill_at_ms,
      with_refill_requests_remaining: withRefillRequestsRemaining,
      with_refill_time_remaining_ms: withRefillTimeRemainingMs,
      with_refill_exhausted_at_ms: withRefillExhaustedAtMs,
      refill_projection_label: label,
      refill_schedule: schedule,
      refill_projection: refillProjectionView,
      refill_requests_remaining: withRefillRequestsRemaining,
      refill_time_remaining_ms: withRefillTimeRemainingMs,
      refill_exhausted_at_ms: withRefillExhaustedAtMs,
    };
  });
}
