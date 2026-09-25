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

/**
 * The in-process failover windows: a tier whose refusal named a reset instant
 * keeps sending its requests to the sibling until that instant passes, and then
 * returns to the requested tier. Scoped to the mapped pair, so a model without a
 * configured sibling never accumulates state.
 *
 * The state is per process on purpose: it mirrors what this gateway instance has
 * been told by the vendor, and a restart re-learns it from the first refusal.
 */
const lithosFailoverDeadlines = new Map<string, number>();

/** The sibling this tier's requests must use right now, or null once the window has passed. */
export const lithosFailoverSiblingAt = (modelRaw: string, nowMs: number): string | null => {
  const deadline = lithosFailoverDeadlines.get(modelRaw);
  if (deadline === undefined) return null;
  if (nowMs >= deadline) {
    lithosFailoverDeadlines.delete(modelRaw);
    return null;
  }
  return lithosSiblingModelFor(modelRaw);
};

/** Opens (or extends) that window: the refusal's own reset instant, in milliseconds from now. */
export const lithosOpenFailoverWindow = (modelRaw: string, nowMs: number, waitMs: number): void => {
  if (lithosSiblingModelFor(modelRaw) === null) return;
  lithosFailoverDeadlines.set(modelRaw, nowMs + waitMs);
};

/** Test seam: drop every window so fixtures cannot leak into each other. */
export const clearLithosFailoverWindows = (): void => {
  lithosFailoverDeadlines.clear();
};

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

/** One refusal hint parsed from the vendor's own headers. */
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
