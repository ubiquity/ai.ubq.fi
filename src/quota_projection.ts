import type { MeteredQuotaSnapshot } from "./metered_quota.ts";
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
}>;

export const meteredQuotaRunwayView = (snapshot: MeteredQuotaSnapshot | null): MeteredQuotaRunwayView => {
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
  const windowStartMs = nowMs - windowDays * DAY_MS;
  const inWindow = buckets.filter((bucket) => bucket.bucket_start_at_ms >= windowStartMs);
  const requestCount = inWindow.reduce((sum, bucket) => sum + bucket.request_count, 0);
  const quotaSum = inWindow.reduce((sum, bucket) => sum + bucket.quota_sum, 0);
  const firstBucket = inWindow.length ? inWindow[0]?.bucket_start_at_ms ?? null : null;
  const lastBucket = inWindow.length ? inWindow[inWindow.length - 1]?.bucket_start_at_ms ?? null : null;
  const elapsedHours = Math.max(
    1,
    Math.round((nowMs - Math.max(windowStartMs, firstBucket ?? windowStartMs)) / HOUR_MS),
  );
  return {
    window_days: windowDays,
    bucket_count: inWindow.length,
    request_count: requestCount,
    quota_sum: quotaSum,
    spend_microcredits: inWindow.reduce((sum, bucket) => sum + bucket.spend_microcredits, 0),
    input_tokens: inWindow.reduce((sum, bucket) => sum + bucket.input_tokens, 0),
    cached_input_tokens: inWindow.reduce((sum, bucket) => sum + bucket.cached_input_tokens, 0),
    output_tokens: inWindow.reduce((sum, bucket) => sum + bucket.output_tokens, 0),
    avg_quota_per_request: requestCount > 0 ? quotaSum / requestCount : null,
    quota_per_hour: elapsedHours > 0 ? quotaSum / elapsedHours : null,
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
  requests_remaining: number | null;
  time_remaining_ms: number | null;
  exhausted_at_ms: number | null;
  percent_per_request_vs_balance: number | null;
  percent_per_request_vs_baseline: number | null;
}>;

const positiveFinite = (value: number | null | undefined): value is number =>
  typeof value === "number" && Number.isFinite(value) && value > 0;

/**
 * Balance the projection is drawn against. Wallet mode uses the live wallet
 * balance; token-usage mode uses the reported available-minus-used total when
 * the provider publishes an inventory; otherwise there is no authoritative
 * balance and estimates are unknown rather than fabricated.
 */
const runwayBalance = (quota: MeteredQuotaRunwayView): number | null => {
  if (quota.unlimited_quota) return null;
  if (quota.balance_quota !== null) return Math.max(0, quota.balance_quota);
  if (quota.total_available !== null && quota.total_used !== null) {
    return Math.max(0, quota.total_available - quota.total_used);
  }
  return null;
};

const runwayBaseline = (quota: MeteredQuotaRunwayView): number | null => {
  if (quota.unlimited_quota) return null;
  if (quota.baseline_quota !== null) return Math.max(0, quota.baseline_quota);
  return quota.total_available ?? null;
};

export const projectPaidFallbackRunway = (
  usage: PaidFallbackModelUsage,
  quota: MeteredQuotaRunwayView,
  nowMs: number,
): PaidFallbackRunwayEstimate[] => {
  const balanceQuota = runwayBalance(quota);
  const baselineQuota = runwayBaseline(quota);
  const unlimited = quota.unlimited_quota === true;
  return usage.windows.map((window) => {
    if (unlimited) {
      return {
        window_days: window.window_days,
        unlimited: true,
        requests_remaining: null,
        time_remaining_ms: null,
        exhausted_at_ms: null,
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
    return {
      window_days: window.window_days,
      unlimited: false,
      requests_remaining: requestsRemaining,
      time_remaining_ms: timeRemainingMs,
      exhausted_at_ms: timeRemainingMs !== null ? nowMs + timeRemainingMs : null,
      percent_per_request_vs_balance: balanceQuota !== null && positiveFinite(avgQuota) && balanceQuota > 0
        ? avgQuota / balanceQuota * 100
        : null,
      percent_per_request_vs_baseline: baselineQuota !== null && positiveFinite(avgQuota) && baselineQuota > 0
        ? avgQuota / baselineQuota * 100
        : null,
    };
  });
};
