// LithosAI rate-limit wait policy, split out of src/provider/lithos_handlers.ts.

import type { UsageContext } from "../openai-telemetry.ts";

/**
 * Rate-limit wait policy for the direct LithosAI route.
 *
 * The vendor's per-minute budgets refill continuously, so a 429 carries a short
 * deadline: `retry-after-ms`, `retry-after`, or the `x-ratelimit-reset-*`
 * deltas of the budget that refused (observed as sub-minute deltas, e.g.
 * `3.23s`). A refusal inside this budget is waited out and retried on the SAME
 * model id instead of failing the request. This route still never races or
 * falls back to another provider: no other provider serves these model ids,
 * and substituting one silently is exactly what this route refuses to do.
 *
 * A refusal that names no window is relayed unchanged rather than guessed at:
 * the wait exists to honor the vendor's own retry time, not to invent one.
 *
 * The per-attempt cap keeps one pause far below the client's 10-minute default
 * request timeout, the gateway's 30-minute first-event budget and the
 * ~100-second proxied-origin read bound, so a wait can never be mistaken for a
 * dead connection. The total bound keeps a second wait from making a single
 * request arbitrarily slow.
 */
const LITHOS_RATE_LIMIT_WAIT_CAP_MS = 75_000;
const LITHOS_RATE_LIMIT_TOTAL_WAIT_CAP_MS = 90_000;
/**
 * A streamed request may absorb far more: a saturated organization window lasts
 * minutes, and an open stream holds the client with inert `: keepalive` frames
 * while it waits, which a buffered request cannot. Buffered requests keep the
 * smaller total so no silent hold sits behind an edge proxy's read limit.
 */
const LITHOS_RATE_LIMIT_STREAM_TOTAL_WAIT_CAP_MS = 300_000;
/** Total dispatches for one buffered request: the first attempt plus two waits. */
const LITHOS_RATE_LIMIT_MAX_DISPATCHES = 3;
/** Safety cap for streamed requests; the total wait budget is what ends them. */
const LITHOS_RATE_LIMIT_STREAM_MAX_DISPATCHES = 20;
/** Spreads retries from requests that were refused the same window. */
const LITHOS_RATE_LIMIT_JITTER_MS = 750;

/** One wait policy: how long a window may buy, and how many dispatches may ask. */
export type LithosWaitPolicy = Readonly<{ perAttemptCapMs: number; totalWaitCapMs: number; maxDispatches: number }>;

const LITHOS_BUFFERED_WAIT_POLICY: LithosWaitPolicy = {
  perAttemptCapMs: LITHOS_RATE_LIMIT_WAIT_CAP_MS,
  totalWaitCapMs: LITHOS_RATE_LIMIT_TOTAL_WAIT_CAP_MS,
  maxDispatches: LITHOS_RATE_LIMIT_MAX_DISPATCHES,
};

const LITHOS_STREAM_WAIT_POLICY: LithosWaitPolicy = {
  perAttemptCapMs: LITHOS_RATE_LIMIT_WAIT_CAP_MS,
  totalWaitCapMs: LITHOS_RATE_LIMIT_STREAM_TOTAL_WAIT_CAP_MS,
  maxDispatches: LITHOS_RATE_LIMIT_STREAM_MAX_DISPATCHES,
};

/** Internal test seam: shrink the wait policy and the keepalive interval. */
type LithosRateLimitTestOverride = Readonly<{
  perAttemptCapMs?: number;
  bufferedTotalWaitCapMs?: number;
  streamTotalWaitCapMs?: number;
  maxDispatches?: number;
  keepaliveIntervalMs?: number;
}>;

let lithosRateLimitTestOverride: LithosRateLimitTestOverride | null = null;

/**
 * Internal test seam: a fixture exhausting a five-minute budget or waiting for
 * a fifteen-second heartbeat would cost minutes, so the policy and the
 * keepalive interval are overridable here. Null restores the shipped policy.
 * It is not a runtime configuration surface.
 */
export const setLithosRateLimitTestOverride = (override: LithosRateLimitTestOverride | null): void => {
  lithosRateLimitTestOverride = override;
};

export const lithosWaitPolicy = (streaming: boolean): LithosWaitPolicy => {
  const base = streaming ? LITHOS_STREAM_WAIT_POLICY : LITHOS_BUFFERED_WAIT_POLICY;
  const override = lithosRateLimitTestOverride;
  if (override === null) return base;
  return {
    perAttemptCapMs: override.perAttemptCapMs ?? base.perAttemptCapMs,
    totalWaitCapMs: (streaming ? override.streamTotalWaitCapMs : override.bufferedTotalWaitCapMs) ?? base.totalWaitCapMs,
    maxDispatches: override.maxDispatches ?? base.maxDispatches,
  };
};

export const lithosKeepaliveIntervalMs = (): number | undefined => lithosRateLimitTestOverride?.keepaliveIntervalMs;

/** One retry hint, named so the wait is logged with the header it came from. */
export type LithosRateLimitWait = Readonly<{ waitMs: number; source: string }>;

const lithosIntegerHeader = (raw: string | null): number | null => {
  if (raw === null) return null;
  const value = raw.trim();
  return /^\d+$/.test(value) ? Number(value) : null;
};

/** The duration units the vendor's reset headers use, in milliseconds. */
const LITHOS_DURATION_UNITS_MS: ReadonlyMap<string, number> = new Map([
  ["ms", 1],
  ["s", 1_000],
  ["m", 60_000],
  ["h", 3_600_000],
]);

/** Digits and the decimal point a duration magnitude may carry. */
const isDurationDigit = (character: string): boolean => (character >= "0" && character <= "9") || character === ".";

/**
 * `x-ratelimit-reset-*` carry deltas like `3.23s`, `1s`, `250ms` or `1m30s`.
 * Scanned rather than matched: the shapes are tiny and a linear scan keeps the
 * parse free of the backtracking a repeated unit pattern invites.
 */
const lithosDurationMs = (raw: string | null): number | null => {
  if (raw === null) return null;
  const value = raw.trim().replace(/\s+/g, "");
  if (!value) return null;
  let total = 0;
  let cursor = 0;
  while (cursor < value.length) {
    let digitsEnd = cursor;
    while (digitsEnd < value.length && isDurationDigit(value.charAt(digitsEnd))) digitsEnd += 1;
    if (digitsEnd === cursor) return null;
    const magnitude = Number(value.slice(cursor, digitsEnd));
    if (!Number.isFinite(magnitude)) return null;
    const unitKey = value.startsWith("ms", digitsEnd) ? "ms" : value.charAt(digitsEnd);
    const unitMs = LITHOS_DURATION_UNITS_MS.get(unitKey);
    if (unitMs === undefined) return null;
    total += magnitude * unitMs;
    cursor = digitsEnd + unitKey.length;
  }
  return Number.isFinite(total) ? total : null;
};

/**
 * Uniform value in `[0, 1)` from the platform CSPRNG, so concurrent requests
 * refused the same window do not retry in lockstep. Predictability would cost
 * nothing here; matching the reconnect jitter in `src/kv.ts` keeps one pattern
 * for jitter across the gateway.
 */
const randomUnitInterval = (): number => {
  const [high = 0, low = 0] = crypto.getRandomValues(new Uint32Array(2));
  return (high * 2 ** 21 + (low >>> 11)) / 2 ** 53;
};

export const lithosRetryJitterMs = (): number => Math.round(randomUnitInterval() * LITHOS_RATE_LIMIT_JITTER_MS);

/** `retry-after` is either an integer number of seconds or an HTTP date. */
const lithosRetryAfterMs = (raw: string | null, nowMs: number): number | null => {
  const seconds = lithosIntegerHeader(raw);
  if (seconds !== null) return seconds * 1_000;
  if (raw === null) return null;
  const parsed = Date.parse(raw.trim());
  return Number.isFinite(parsed) && parsed > nowMs ? parsed - nowMs : null;
};

/**
 * The wait one 429 asks for, in the vendor's own precedence: `retry-after-ms`
 * first, then `retry-after`, then the later of the two `x-ratelimit-reset-*`
 * deltas (the budget that refills last is the one that binds). A vendor
 * instruction never to retry wins over every hint, and a refusal that names no
 * window at all returns null so it is relayed instead of guessed at. Exported
 * because it is the whole header contract in one pure function.
 */
export const lithosRateLimitWait = (headers: Headers, nowMs: number): LithosRateLimitWait | null => {
  if (headers.get("x-should-retry")?.trim().toLowerCase() === "false") return null;
  const retryAfterMs = lithosIntegerHeader(headers.get("retry-after-ms"));
  if (retryAfterMs !== null) return { waitMs: retryAfterMs, source: "retry-after-ms" };
  const retryAfter = lithosRetryAfterMs(headers.get("retry-after"), nowMs);
  if (retryAfter !== null) return { waitMs: retryAfter, source: "retry-after" };
  let latest: LithosRateLimitWait | null = null;
  for (const header of ["x-ratelimit-reset-tokens", "x-ratelimit-reset-requests"]) {
    const waitMs = lithosDurationMs(headers.get(header));
    if (waitMs === null) continue;
    if (latest === null || waitMs > latest.waitMs) latest = { waitMs, source: header };
  }
  if (latest !== null) return latest;
  return null;
};

/** The window this refusal names, or null when the wait policy forbids it. */
export const lithosPlannedWait = (upstream: Response, attempt: number, waitedMs: number, policy: LithosWaitPolicy): LithosRateLimitWait | null => {
  if (upstream.status !== 429 || attempt >= policy.maxDispatches) return null;
  const planned = lithosRateLimitWait(upstream.headers, Date.now());
  if (planned === null || planned.waitMs > policy.perAttemptCapMs) return null;
  if (waitedMs + planned.waitMs > policy.totalWaitCapMs) return null;
  return planned;
};

/** Records the absorbed wait total so a terminal can report it. */
export const recordLithosRateLimitWaitMs = (usageContext: UsageContext | undefined, waitedMs: number): void => {
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.rateLimitWaitMs = waitedMs;
};

export const logLithosRateLimitWait = (fields: Readonly<Record<string, string | number | null>>): void => {
  try {
    console.info("[ai.ubq.fi] lithos_rate_limit_wait", JSON.stringify(fields));
  } catch {
    // Telemetry must never change routing or delivery.
  }
};

const lithosWaitAbortReason = (signal: AbortSignal): Error => {
  const reason = signal.reason;
  return reason instanceof Error ? reason : new DOMException("The request was aborted while waiting for the LithosAI rate limit to reset.", "AbortError");
};

/** Abort-aware sleep: an abandoned request never keeps waiting for a provider window. */
export const waitForLithosRetry = (milliseconds: number, signal: AbortSignal): Promise<void> => {
  if (milliseconds <= 0) return Promise.resolve();
  if (signal.aborted) return Promise.reject(lithosWaitAbortReason(signal));
  return new Promise<void>((resolve, reject) => {
    // The timer handle type is not portable across the lint project's type
    // environment, so it is named through the global rather than as `number`.
    let timer: ReturnType<typeof setTimeout> | null = null;
    const onAbort = (): void => {
      if (timer !== null) clearTimeout(timer);
      reject(lithosWaitAbortReason(signal));
    };
    timer = setTimeout(() => {
      signal.removeEventListener("abort", onAbort);
      resolve();
    }, milliseconds);
    signal.addEventListener("abort", onAbort, { once: true });
  });
};

/**
 * The sibling tier a saturated model fails over to for one request.
 *
 * Both ids are the same 552B weights behind separate per-model rate-limit
 * buckets - probed 2026-09-24: independent `x-ratelimit-remaining-*` counters,
 * and `-ultra-chat` returned the same `get_weather` tool call on the raw vendor
 * wire and through this gateway's Chat and Responses routes with reasoning
 * enabled - so a refusal on one tier says nothing about the other.
 */
const LITHOS_SIBLING_MODELS: ReadonlyMap<string, string> = new Map([["deepseek-ai/DeepSeek-V4.1-Flash-ultra", "deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat"]]);

/** The sibling tier for a requested model, or null when that tier has none. */
export const lithosSiblingModelFor = (modelRaw: string): string | null => LITHOS_SIBLING_MODELS.get(modelRaw) ?? null;

/** Records the sibling that served a request whose own tier refused it. */
export const recordLithosFailoverModel = (usageContext: UsageContext | undefined, model: string): void => {
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.rateLimitFailoverModel = model;
};

export const logLithosRateLimitFailover = (fields: Readonly<Record<string, string | number | null>>): void => {
  try {
    console.info("[ai.ubq.fi] lithos_rate_limit_failover", JSON.stringify(fields));
  } catch {
    // Telemetry must never change routing or delivery.
  }
};
