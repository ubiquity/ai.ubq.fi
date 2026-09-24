// LithosAI rate-limit handling, split out of src/provider/lithos-handlers.ts.

import type { UsageContext } from "../openai-telemetry.ts";

/**
 * The sibling tier a refused model is load-balanced onto, for one request.
 *
 * Both ids are the same 552B weights behind separate per-model rate-limit
 * buckets, so a refusal on one tier says nothing about the other. The target is
 * the `-fast` tier, NOT `-ultra-chat`: LithosAI told the owner on 2026-09-24
 * that `-ultra-chat` is tuned for short context windows and loses accuracy on
 * the large-context sessions this route serves. Probed against the live vendor
 * the same day: `-fast` reports its own `x-ratelimit-remaining-*` counters
 * (independent of ultra's), answers `reasoning_effort` none..max, returns
 * `get_weather` tool calls, and answered a 106k-token needle prompt correctly.
 */
const LITHOS_SIBLING_MODELS: ReadonlyMap<string, string> = new Map([["deepseek-ai/DeepSeek-V4.1-Flash-ultra", "deepseek-ai/DeepSeek-V4.1-Flash-fast"]]);

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

/**
 * The opt-in switch for waiting out a refusal instead of relaying it.
 *
 * Off by default: a refused request is load-balanced once onto its sibling tier
 * and, if that refuses too, the refusal is relayed immediately. With the switch
 * on, a refusal whose own headers name a retry window is waited out and the
 * SAME model id is retried, bounded by the caps below.
 */
export const LITHOS_RATE_LIMIT_WAIT_ENV = "LITHOSAI_RATE_LIMIT_WAIT";

/** Reads that switch; an absent, unreadable or other value means no waiting. */
const lithosRateLimitWaitEnabled = (): boolean => {
  let raw: string | undefined;
  try {
    raw = Deno.env.get(LITHOS_RATE_LIMIT_WAIT_ENV);
  } catch {
    return false;
  }
  const value = raw?.trim().toLowerCase();
  return value === "1" || value === "true" || value === "yes" || value === "on";
};

/**
 * The per-attempt cap keeps one pause far below the client's 10-minute default
 * request timeout, the gateway's first-event budget and the ~100-second
 * proxied-origin read bound, so a wait can never be mistaken for a dead
 * connection. The total bound keeps a second wait from making a single request
 * arbitrarily slow, and the dispatch cap bounds how often a window is re-asked.
 */
const LITHOS_RATE_LIMIT_WAIT_CAP_MS = 75_000;
const LITHOS_RATE_LIMIT_TOTAL_WAIT_CAP_MS = 90_000;
const LITHOS_RATE_LIMIT_MAX_DISPATCHES = 3;

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

/**
 * The wait this refusal is owed, or null when it is relayed instead: the switch
 * is off, the attempt has no budget left, the window exceeds the per-attempt
 * cap, or the total budget cannot cover one more pause.
 */
export const lithosPlannedWait = (upstream: Response, attempt: number, waitedMs: number): LithosRateLimitWait | null => {
  if (!lithosRateLimitWaitEnabled()) return null;
  if (upstream.status !== 429 || attempt >= LITHOS_RATE_LIMIT_MAX_DISPATCHES) return null;
  const planned = lithosRateLimitWait(upstream.headers, Date.now());
  if (planned === null || planned.waitMs > LITHOS_RATE_LIMIT_WAIT_CAP_MS) return null;
  if (waitedMs + planned.waitMs > LITHOS_RATE_LIMIT_TOTAL_WAIT_CAP_MS) return null;
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
