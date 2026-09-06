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
  balance_history?: readonly MeteredQuotaBalanceSample[];
  refill_schedule?: MeteredQuotaRefillSchedule;
}>;

export type QuotaProjectionOptions = Readonly<{
  balance_history?: readonly MeteredQuotaBalanceSample[];
  /** Camel-case alias for callers outside the HTTP layer. */
  balanceHistory?: readonly MeteredQuotaBalanceSample[];
  /** Short alias for pure callers that already have a history series. */
  history?: readonly MeteredQuotaBalanceSample[];
}>;

type QuotaProjectionRefillInput = readonly MeteredQuotaBalanceSample[] | QuotaProjectionOptions;

const refillHistoryFromInput = (
  input: QuotaProjectionRefillInput | undefined,
): readonly MeteredQuotaBalanceSample[] => {
  if (!input) return [];
  if (Array.isArray(input)) return input as readonly MeteredQuotaBalanceSample[];
  const options = input as QuotaProjectionOptions;
  return options.balance_history ?? options.balanceHistory ?? options.history ?? [];
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
  if (tokenUsage) return { ...view, refill_schedule: EMPTY_REFILL_SCHEDULE };
  return {
    ...view,
    refill_schedule: estimateMeteredQuotaRefillSchedule(view, balanceHistory, state.observed_at_ms),
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
  | "known_refill_schedule"
  | "sustainable";

export type PaidFallbackRefillStatus = QuotaRefillStatus;

/** Facts inferred from retained wallet samples and the latest Metered refill metadata. */
export type MeteredQuotaRefillSchedule = Readonly<{
  /** Estimated amount added at each refill, in balance-quota units. */
  amount_quota: number | null;
  amount_credits: number | null;
  /** Estimated interval between refills. */
  cadence_ms: number | null;
  /** Estimated refill amount per hour. */
  rate_quota_per_hour: number | null;
  last_refill_at_ms: number | null;
  next_refill_at_ms: number | null;
  observed_refill_count: number;
  source: "none" | "latest_refill" | "balance_history";
  /** Compatibility alias for callers that use the older terminology. */
  known_refill_count: number;
}>;

const EMPTY_REFILL_SCHEDULE: MeteredQuotaRefillSchedule = {
  amount_quota: null,
  amount_credits: null,
  cadence_ms: null,
  rate_quota_per_hour: null,
  last_refill_at_ms: null,
  next_refill_at_ms: null,
  observed_refill_count: 0,
  source: "none",
  known_refill_count: 0,
};

export type QuotaRefillProjection = Readonly<{
  status: QuotaRefillStatus;
  no_refill_time_remaining_ms: number | null;
  no_refill_exhausted_at_ms: number | null;
  time_remaining_ms: number | null;
  exhausted_at_ms: number | null;
  next_refill_at_ms: number | null;
  schedule: MeteredQuotaRefillSchedule;
  label: string | null;
}>;

export type PaidFallbackRunwayEstimate = Readonly<{
  window_days: QuotaProjectionWindowDays;
  unlimited: boolean;
  /** True when the balance snapshot is older than the freshness window. */
  stale_balance: boolean;
  requests_remaining: number | null;
  time_remaining_ms: number | null;
  exhausted_at_ms: number | null;
  percent_per_request_vs_balance: number | null;
  percent_per_request_vs_baseline: number | null;
  /** Existing fields remain the conservative no-refill estimate. */
  no_refill_requests_remaining: number | null;
  no_refill_time_remaining_ms: number | null;
  no_refill_exhausted_at_ms: number | null;
  /** Refill-aware result and the schedule facts used to calculate it. */
  refill_status: QuotaRefillStatus;
  /** Compatibility code whose finite value is `known_refill_schedule`. */
  refill_status_code: QuotaRefillStatus;
  refill_requests_remaining: number | null;
  refill_time_remaining_ms: number | null;
  refill_exhausted_at_ms: number | null;
  with_refill_requests_remaining: number | null;
  with_refill_time_remaining_ms: number | null;
  with_refill_exhausted_at_ms: number | null;
  refill_amount_quota: number | null;
  refill_amount_credits: number | null;
  refill_cadence_ms: number | null;
  refill_rate_quota_per_hour: number | null;
  next_refill_at_ms: number | null;
  refill_projection_label: string | null;
  refill_schedule: MeteredQuotaRefillSchedule;
  refill_projection: QuotaRefillProjection;
}>;

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

type RefillEvent = {
  at_ms: number;
  amount_quota: number;
  from_history: boolean;
  authoritative: boolean;
};

const REFILL_EVENT_MATCH_TOLERANCE_MS = HOUR_MS;

const normalizedBalanceHistory = (
  history: readonly MeteredQuotaBalanceSample[] | undefined,
): MeteredQuotaBalanceSample[] => {
  const byBucket = new Map<number, MeteredQuotaBalanceSample>();
  for (const raw of history ?? []) {
    if (!raw || typeof raw !== "object") continue;
    const sample = raw as MeteredQuotaBalanceSample;
    if (
      (sample.v !== undefined && sample.v !== 1) ||
      !finiteTimestamp(sample.bucket_start_at_ms) ||
      !finiteTimestamp(sample.observed_at_ms) ||
      !nonNegativeFinite(sample.balance_quota) ||
      !nonNegativeFinite(sample.baseline_quota) ||
      !positiveFinite(sample.quota_per_credit) ||
      sample.unlimited_quota === true ||
      typeof sample.total_available === "number" ||
      typeof sample.total_granted === "number" ||
      typeof sample.total_used === "number"
    ) continue;
    const existing = byBucket.get(sample.bucket_start_at_ms);
    if (!existing || sample.observed_at_ms > existing.observed_at_ms) {
      byBucket.set(sample.bucket_start_at_ms, { ...sample });
    }
  }
  return [...byBucket.values()].sort(
    (left, right) =>
      left.observed_at_ms - right.observed_at_ms ||
      left.bucket_start_at_ms - right.bucket_start_at_ms,
  );
};

const refillAmountFromMetadata = (
  quota: Pick<MeteredQuotaRunwayView, "latest_refill_amount_credits" | "quota_per_credit">,
): { amount_quota: number; amount_credits: number } | null => {
  if (!positiveFinite(quota.latest_refill_amount_credits) || !positiveFinite(quota.quota_per_credit)) return null;
  const amountQuota = quota.latest_refill_amount_credits * quota.quota_per_credit;
  return Number.isFinite(amountQuota) && amountQuota > 0
    ? { amount_quota: amountQuota, amount_credits: quota.latest_refill_amount_credits }
    : null;
};

const normalizeRefillSchedule = (
  schedule: MeteredQuotaRefillSchedule | undefined,
): MeteredQuotaRefillSchedule => {
  if (!schedule) return EMPTY_REFILL_SCHEDULE;
  const amount = positiveFinite(schedule.amount_quota) ? schedule.amount_quota : null;
  const amountCredits = positiveFinite(schedule.amount_credits) ? schedule.amount_credits : null;
  const cadence = positiveFinite(schedule.cadence_ms) ? schedule.cadence_ms : null;
  const rate = amount !== null && cadence !== null ? amount / cadence * HOUR_MS : null;
  const last = finiteTimestamp(schedule.last_refill_at_ms) ? schedule.last_refill_at_ms : null;
  const next = finiteTimestamp(schedule.next_refill_at_ms) ? schedule.next_refill_at_ms : null;
  const observedCount = Number.isSafeInteger(schedule.observed_refill_count) && schedule.observed_refill_count > 0
    ? schedule.observed_refill_count
    : 0;
  const knownCount = Number.isSafeInteger(schedule.known_refill_count) && schedule.known_refill_count > 0
    ? schedule.known_refill_count
    : 0;
  return {
    amount_quota: amount,
    amount_credits: amountCredits,
    cadence_ms: cadence,
    rate_quota_per_hour: positiveFinite(rate) ? rate : null,
    last_refill_at_ms: last,
    next_refill_at_ms: next,
    observed_refill_count: Math.max(observedCount, knownCount),
    source: schedule.source === "balance_history" || schedule.source === "latest_refill" ? schedule.source : "none",
    known_refill_count: Math.max(observedCount, knownCount),
  };
};

const quotaIsTokenUsage = (quota: MeteredQuotaRunwayView): boolean =>
  quota.unlimited_quota || quota.total_available !== null || quota.total_used !== null;

/**
 * Infer a periodic refill schedule from the retained balance samples and
 * the latest refill metadata. Baseline increases are useful because they
 * remain visible when a refill is consumed during the sampling gap; a
 * positive balance increase is corrected by the selected burn rate.
 * A single latest refill is reported as a fact but does not invent a
 * cadence unless the cycle anchor or retained history supplies one.
 */
export const estimateMeteredQuotaRefillSchedule = (
  quota: MeteredQuotaRunwayView,
  balanceHistory: readonly MeteredQuotaBalanceSample[] = quota.balance_history ?? [],
  nowMs = Date.now(),
  burnQuotaPerHour = 0,
): MeteredQuotaRefillSchedule => {
  if (quotaIsTokenUsage(quota)) return EMPTY_REFILL_SCHEDULE;
  const safeNowMs = finiteTimestamp(nowMs) ? nowMs : Date.now();
  const samples = normalizedBalanceHistory(balanceHistory);
  const existing = normalizeRefillSchedule(quota.refill_schedule);
  const metadataAmount = refillAmountFromMetadata(quota);
  const quotaPerCredit = positiveFinite(quota.quota_per_credit)
    ? quota.quota_per_credit
    : medianPositive(samples.map((sample) => sample.quota_per_credit));
  const burnRate = positiveFinite(burnQuotaPerHour) ? burnQuotaPerHour : 0;
  const events: RefillEvent[] = [];

  const addEvent = (
    atMs: number,
    amountQuota: number,
    fromHistory: boolean,
    authoritative = false,
  ): void => {
    if (!finiteTimestamp(atMs) || atMs > safeNowMs || !positiveFinite(amountQuota)) return;
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
    const baselineIncrease = current.baseline_quota - previous.baseline_quota;
    const balanceIncrease = current.balance_quota - previous.balance_quota;
    const burnDuringGap = burnRate * elapsedMs / HOUR_MS;
    const adjustedBalanceIncrease = balanceIncrease > 0 ? balanceIncrease + burnDuringGap : 0;
    const amountQuota = Math.max(
      adjustedBalanceIncrease,
      baselineIncrease > 0 ? baselineIncrease : 0,
    );
    addEvent(current.observed_at_ms, amountQuota, true);
  }

  const metadataAtMs = metadataAmount === null
    ? null
    : finiteTimestamp(quota.latest_refill_completed_at_ms)
    ? quota.latest_refill_completed_at_ms
    : finiteTimestamp(quota.last_credit_at_ms)
    ? quota.last_credit_at_ms
    : finiteTimestamp(quota.cycle_started_at_ms)
    ? quota.cycle_started_at_ms
    : null;
  if (metadataAmount !== null && metadataAtMs !== null && metadataAtMs <= safeNowMs) {
    let closest: RefillEvent | null = null;
    let closestDistance = Number.POSITIVE_INFINITY;
    for (const event of events) {
      const distance = Math.abs(event.at_ms - metadataAtMs);
      if (distance < closestDistance) {
        closest = event;
        closestDistance = distance;
      }
    }
    if (closest && closestDistance <= REFILL_EVENT_MATCH_TOLERANCE_MS) {
      closest.at_ms = metadataAtMs;
      closest.amount_quota = metadataAmount.amount_quota;
      closest.authoritative = true;
    } else {
      addEvent(metadataAtMs, metadataAmount.amount_quota, false, true);
    }

    const cycleStartedAtMs = finiteTimestamp(quota.cycle_started_at_ms) ? quota.cycle_started_at_ms : null;
    if (
      cycleStartedAtMs !== null &&
      cycleStartedAtMs < metadataAtMs &&
      metadataAtMs - cycleStartedAtMs >= REFILL_EVENT_MATCH_TOLERANCE_MS
    ) {
      addEvent(cycleStartedAtMs, metadataAmount.amount_quota, false);
    }
  }

  events.sort((left, right) => left.at_ms - right.at_ms);
  const distinctEvents: RefillEvent[] = [];
  for (const event of events) {
    const previous = distinctEvents.at(-1);
    if (previous && event.at_ms - previous.at_ms < REFILL_EVENT_MATCH_TOLERANCE_MS) {
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

  const intervals: number[] = [];
  for (let index = 1; index < distinctEvents.length; index += 1) {
    const intervalMs = distinctEvents[index]!.at_ms - distinctEvents[index - 1]!.at_ms;
    if (positiveFinite(intervalMs)) intervals.push(intervalMs);
  }
  const cadence = medianPositive(intervals) ?? existing.cadence_ms;
  const latestEvent = distinctEvents.at(-1);
  const amountQuota = metadataAmount?.amount_quota ?? latestEvent?.amount_quota ?? existing.amount_quota;
  const amountCredits = metadataAmount?.amount_credits ??
    (amountQuota !== null && amountQuota !== undefined && positiveFinite(quotaPerCredit)
      ? amountQuota / quotaPerCredit
      : existing.amount_credits);
  const lastRefillAt = latestEvent?.at_ms ?? existing.last_refill_at_ms ?? metadataAtMs;
  const rateCandidate = amountQuota !== null && amountQuota !== undefined && positiveFinite(cadence)
    ? amountQuota / cadence * HOUR_MS
    : null;
  let nextRefillAtMs: number | null = null;
  if (positiveFinite(cadence) && finiteTimestamp(lastRefillAt)) {
    const candidate = lastRefillAt > safeNowMs
      ? lastRefillAt
      : lastRefillAt + (Math.floor((safeNowMs - lastRefillAt) / cadence) + 1) * cadence;
    if (finiteTimestamp(candidate)) nextRefillAtMs = candidate;
  }
  const source = distinctEvents.some((event) => event.from_history)
    ? "balance_history"
    : distinctEvents.length || amountQuota !== null
    ? "latest_refill"
    : existing.source;
  const observedCount = Math.max(
    distinctEvents.length,
    existing.observed_refill_count,
    existing.known_refill_count,
  );
  return {
    amount_quota: positiveFinite(amountQuota) ? amountQuota : null,
    amount_credits: positiveFinite(amountCredits) ? amountCredits : null,
    cadence_ms: positiveFinite(cadence) ? cadence : null,
    rate_quota_per_hour: positiveFinite(rateCandidate) ? rateCandidate : null,
    last_refill_at_ms: finiteTimestamp(lastRefillAt) ? lastRefillAt : null,
    next_refill_at_ms: finiteTimestamp(nextRefillAtMs) ? nextRefillAtMs : null,
    observed_refill_count: observedCount,
    source,
    known_refill_count: observedCount,
  };
};

export const estimateMeteredRefillSchedule = estimateMeteredQuotaRefillSchedule;

/**
 * Balance the projection is drawn against. Wallet mode uses the live wallet
 * balance. Token-usage mode reports `total_available` as the remaining
 * inventory ("Available tokens"), so it is used directly — subtracting
 * `total_used` would double-count consumption. Otherwise there is no
 * authoritative balance and estimates are unknown rather than fabricated.
 */
const runwayBalance = (quota: MeteredQuotaRunwayView): number | null => {
  if (quota.unlimited_quota) return null;
  if (nonNegativeFinite(quota.balance_quota)) return quota.balance_quota;
  if (nonNegativeFinite(quota.total_available)) return quota.total_available;
  return null;
};

const runwayBaseline = (quota: MeteredQuotaRunwayView): number | null => {
  if (quota.unlimited_quota) return null;
  if (quota.baseline_quota !== null) return Math.max(0, quota.baseline_quota);
  return quota.total_granted !== null && quota.total_granted > 0 ? quota.total_granted : null;
};

type RefillTimeProjection =
  | Readonly<{ kind: "unknown"; time_remaining_ms: null; exhausted_at_ms: null; affected_by_refill: false }>
  | Readonly<{ kind: "sustainable"; time_remaining_ms: null; exhausted_at_ms: null; affected_by_refill: true }>
  | Readonly<{ kind: "finite"; time_remaining_ms: number; exhausted_at_ms: number; affected_by_refill: boolean }>;

const nextScheduledRefillAt = (
  schedule: MeteredQuotaRefillSchedule,
  nowMs: number,
): number | null => {
  if (!finiteTimestamp(nowMs) || !positiveFinite(schedule.cadence_ms)) return null;
  if (finiteTimestamp(schedule.next_refill_at_ms) && schedule.next_refill_at_ms > nowMs) {
    return schedule.next_refill_at_ms;
  }
  const anchor = finiteTimestamp(schedule.last_refill_at_ms)
    ? schedule.last_refill_at_ms
    : finiteTimestamp(schedule.next_refill_at_ms)
    ? schedule.next_refill_at_ms - schedule.cadence_ms
    : null;
  if (!finiteTimestamp(anchor)) return null;
  const candidate = anchor > nowMs
    ? anchor
    : anchor + (Math.floor((nowMs - anchor) / schedule.cadence_ms) + 1) * schedule.cadence_ms;
  return finiteTimestamp(candidate) ? candidate : null;
};

/** Apply discrete future refills without smoothing a due refill into the burn rate. */
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
    return { kind: "unknown", time_remaining_ms: null, exhausted_at_ms: null, affected_by_refill: false };
  }
  const untilNextRefillMs = nextRefillAtMs - nowMs;
  const burnUntilNextRefill = quotaPerHour * untilNextRefillMs / HOUR_MS;
  if (!Number.isFinite(burnUntilNextRefill)) {
    return { kind: "unknown", time_remaining_ms: null, exhausted_at_ms: null, affected_by_refill: false };
  }
  // A refill arriving exactly when the balance reaches zero is still usable.
  if (balanceQuota < burnUntilNextRefill) {
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
    return { kind: "unknown", time_remaining_ms: null, exhausted_at_ms: null, affected_by_refill: false };
  }
  if (schedule.amount_quota >= burnPerCadence) {
    return { kind: "sustainable", time_remaining_ms: null, exhausted_at_ms: null, affected_by_refill: true };
  }
  const balanceAfterFirstRefill = balanceQuota - burnUntilNextRefill + schedule.amount_quota;
  const deficitPerCycle = burnPerCadence - schedule.amount_quota;
  if (!Number.isFinite(balanceAfterFirstRefill) || !positiveFinite(deficitPerCycle)) {
    return { kind: "unknown", time_remaining_ms: null, exhausted_at_ms: null, affected_by_refill: false };
  }
  // B_k is the balance immediately after the k-th future refill.
  const cyclesBeforeExhaustion = balanceAfterFirstRefill < burnPerCadence
    ? 0
    : Math.floor((balanceAfterFirstRefill - burnPerCadence) / deficitPerCycle) + 1;
  const balanceAtExhaustionCycle = balanceAfterFirstRefill - cyclesBeforeExhaustion * deficitPerCycle;
  const exhaustedAtMs = nextRefillAtMs +
    cyclesBeforeExhaustion * schedule.cadence_ms +
    Math.max(0, balanceAtExhaustionCycle) / quotaPerHour * HOUR_MS;
  if (!Number.isFinite(exhaustedAtMs)) {
    return { kind: "unknown", time_remaining_ms: null, exhausted_at_ms: null, affected_by_refill: false };
  }
  const exhaustedAt = Math.trunc(exhaustedAtMs);
  if (!finiteTimestamp(exhaustedAt) || exhaustedAt < nowMs) {
    return { kind: "unknown", time_remaining_ms: null, exhausted_at_ms: null, affected_by_refill: false };
  }
  return {
    kind: "finite",
    time_remaining_ms: exhaustedAt - nowMs,
    exhausted_at_ms: exhaustedAt,
    affected_by_refill: true,
  };
};

const projectionLabel = (
  status: QuotaRefillStatus,
  noRefillExhaustedAtMs: number | null,
  refillExhaustedAtMs: number | null,
): string | null => {
  const dateText = (timestamp: number | null): string | null => {
    if (!finiteTimestamp(timestamp)) return null;
    try {
      return new Date(timestamp).toISOString();
    } catch {
      return null;
    }
  };
  if (status === "sustainable") return "sustainable";
  if (status === "with_refill" || status === "known_refill_schedule") {
    const date = dateText(refillExhaustedAtMs);
    return date === null
      ? "exhaustion unknown (with known refill schedule)"
      : `exhausts at ${date} (with known refill schedule)`;
  }
  if (status === "no_refill") {
    const date = dateText(noRefillExhaustedAtMs);
    return date === null ? "exhaustion unknown (no refill)" : `exhausts at ${date} (no refill)`;
  }
  return null;
};

const isQuotaProjectionOptions = (value: unknown): value is QuotaProjectionOptions =>
  typeof value === "object" && value !== null && !Array.isArray(value);

/**
 * Projections are only meaningful for the provider whose balance is being
 * monitored. `METERED_API_KEY` monitors the OpenLux account, which serves
 * `provider: "metered"` rows; Surplus Intelligence has its own billing with
 * no quota snapshot in this gateway. Returning no estimate for other
 * providers avoids reporting Surplus history against the OpenLux balance.
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

  let projectionNowMs = Date.now();
  let refillInput: QuotaProjectionRefillInput | undefined;
  const applyArgument = (
    argument: number | QuotaProjectionRefillInput | undefined,
  ): void => {
    if (typeof argument === "number") {
      projectionNowMs = argument;
    } else if (Array.isArray(argument)) {
      refillInput = argument;
    } else if (isQuotaProjectionOptions(argument)) {
      refillInput = argument;
    }
  };
  applyArgument(nowMsOrInput);
  applyArgument(inputOrNow);

  const safeNowMs = finiteTimestamp(projectionNowMs) ? projectionNowMs : Date.now();
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
      const projection: QuotaRefillProjection = {
        status: "unlimited",
        no_refill_time_remaining_ms: null,
        no_refill_exhausted_at_ms: null,
        time_remaining_ms: null,
        exhausted_at_ms: null,
        next_refill_at_ms: null,
        schedule: EMPTY_REFILL_SCHEDULE,
        label: null,
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
        refill_requests_remaining: null,
        refill_time_remaining_ms: null,
        refill_exhausted_at_ms: null,
        with_refill_requests_remaining: null,
        with_refill_time_remaining_ms: null,
        with_refill_exhausted_at_ms: null,
        refill_amount_quota: null,
        refill_amount_credits: null,
        refill_cadence_ms: null,
        refill_rate_quota_per_hour: null,
        next_refill_at_ms: null,
        refill_projection_label: null,
        refill_schedule: EMPTY_REFILL_SCHEDULE,
        refill_projection: projection,
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
    const refillTimeRemainingMs = refillStatus === "no_refill"
      ? noRefillTimeRemainingMs
      : refillProjection.time_remaining_ms;
    const refillExhaustedAtMs = refillStatus === "no_refill" ? noRefillExhaustedAtMs : refillProjection.exhausted_at_ms;
    const refillRequestsRemaining = refillStatus === "sustainable"
      ? null
      : refillTimeRemainingMs !== null && positiveFinite(avgQuota) && positiveFinite(quotaPerHour)
      ? Math.floor(refillTimeRemainingMs / HOUR_MS * quotaPerHour / avgQuota)
      : refillStatus === "no_refill"
      ? noRefillRequestsRemaining
      : null;
    const label = projectionLabel(refillStatus, noRefillExhaustedAtMs, refillExhaustedAtMs);
    const projection: QuotaRefillProjection = {
      status: refillStatus,
      no_refill_time_remaining_ms: noRefillTimeRemainingMs,
      no_refill_exhausted_at_ms: noRefillExhaustedAtMs,
      time_remaining_ms: refillTimeRemainingMs,
      exhausted_at_ms: refillExhaustedAtMs,
      next_refill_at_ms: schedule.next_refill_at_ms,
      schedule,
      label,
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
      refill_requests_remaining: refillRequestsRemaining,
      refill_time_remaining_ms: refillTimeRemainingMs,
      refill_exhausted_at_ms: refillExhaustedAtMs,
      with_refill_requests_remaining: refillRequestsRemaining,
      with_refill_time_remaining_ms: refillTimeRemainingMs,
      with_refill_exhausted_at_ms: refillExhaustedAtMs,
      refill_amount_quota: schedule.amount_quota,
      refill_amount_credits: schedule.amount_credits,
      refill_cadence_ms: schedule.cadence_ms,
      refill_rate_quota_per_hour: schedule.rate_quota_per_hour,
      next_refill_at_ms: schedule.next_refill_at_ms,
      refill_projection_label: label,
      refill_schedule: schedule,
      refill_projection: projection,
    };
  });
}
