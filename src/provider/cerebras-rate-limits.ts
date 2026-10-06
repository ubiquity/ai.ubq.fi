import type { UsageContext } from "../openai-telemetry.ts";

export const CEREBRAS_RATE_LIMIT_HEADERS = [
  "x-ratelimit-limit-requests-minute",
  "x-ratelimit-remaining-requests-minute",
  "x-ratelimit-reset-requests-minute",
  "x-ratelimit-limit-tokens-minute",
  "x-ratelimit-remaining-tokens-minute",
  "x-ratelimit-reset-tokens-minute",
  "x-ratelimit-limit-requests-day",
  "x-ratelimit-remaining-requests-day",
  "x-ratelimit-reset-requests-day",
  "x-ratelimit-limit-tokens-day",
  "x-ratelimit-remaining-tokens-day",
  "x-ratelimit-reset-tokens-day",
] as const;

/**
 * Bounded absorb for a Cerebras 429, mirroring the LithosAI policy: a refusal
 * whose own headers name a short reset window is waited out and retried instead
 * of being surfaced as an HTTP 429 the Codex client reads as "retry, then fail
 * the turn". Cerebras names its minute budgets in `x-ratelimit-reset-requests-minute`
 * and `x-ratelimit-reset-tokens-minute`; `retry-after` wins when present, and a
 * vendor `x-should-retry: false` is never waited out.
 *
 * The caps match the buffered Lithos policy: one pause stays far below the
 * client's default request timeout, and the total stays bounded so a single
 * request cannot wait arbitrarily long.
 */
export type CerebrasRateLimitWait = Readonly<{ waitMs: number; source: string }>;

export const CEREBRAS_REFUSAL_WAIT_CAP_MS = 75_000;
export const CEREBRAS_REFUSAL_WAIT_TOTAL_CAP_MS = 120_000;
export const CEREBRAS_REFUSAL_WAIT_MAX_WAITS = 2;

const DURATION_UNITS_MS: ReadonlyMap<string, number> = new Map([
  ["ms", 1],
  ["s", 1_000],
  ["m", 60_000],
  ["h", 3_600_000],
]);

const isDurationDigit = (character: string): boolean => (character >= "0" && character <= "9") || character === ".";

/**
 * The duration shapes Cerebras uses in its reset headers: a bare number is
 * seconds (`12`), and a suffixed shape is read as written (`0.4s`, `500ms`,
 * `1m30s`). Anything else returns null so no wait is invented.
 */
export const cerebrasDurationMs = (raw: string | null): number | null => {
  if (raw === null) return null;
  const value = raw.trim().replace(/\s+/g, "");
  if (!value) return null;
  if (/^\d+(?:\.\d+)?$/.test(value)) return Number(value) * 1_000;
  let total = 0;
  let cursor = 0;
  while (cursor < value.length) {
    let digitsEnd = cursor;
    while (digitsEnd < value.length && isDurationDigit(value.charAt(digitsEnd))) digitsEnd += 1;
    if (digitsEnd === cursor) return null;
    const magnitude = Number(value.slice(cursor, digitsEnd));
    if (!Number.isFinite(magnitude)) return null;
    const unitKey = value.startsWith("ms", digitsEnd) ? "ms" : value.charAt(digitsEnd);
    const unitMs = DURATION_UNITS_MS.get(unitKey);
    if (unitMs === undefined) return null;
    total += magnitude * unitMs;
    cursor = digitsEnd + unitKey.length;
  }
  return Number.isFinite(total) ? total : null;
};

/** `retry-after` is either an integer number of seconds or an HTTP date. */
const retryAfterMs = (raw: string | null, nowMs: number): number | null => {
  if (raw === null) return null;
  const value = raw.trim();
  if (/^\d+$/.test(value)) return Number(value) * 1_000;
  const parsed = Date.parse(value);
  return Number.isFinite(parsed) && parsed > nowMs ? parsed - nowMs : null;
};

/** The pause one refusal asks for, in the vendor's own header precedence. */
export const cerebrasRateLimitWait = (headers: Headers, nowMs: number): CerebrasRateLimitWait | null => {
  if (headers.get("x-should-retry")?.trim().toLowerCase() === "false") return null;
  const retryAfter = retryAfterMs(headers.get("retry-after"), nowMs);
  if (retryAfter !== null) return { waitMs: retryAfter, source: "retry-after" };
  let latest: CerebrasRateLimitWait | null = null;
  for (const header of ["x-ratelimit-reset-requests-minute", "x-ratelimit-reset-tokens-minute"]) {
    const waitMs = cerebrasDurationMs(headers.get(header));
    if (waitMs === null) continue;
    if (latest === null || waitMs > latest.waitMs) latest = { waitMs, source: header };
  }
  return latest;
};

/** The pause this refusal is owed, bounded by the per-wait, count and total caps. */
export const cerebrasRefusalWait = (headers: Headers, waitedMs: number, waits: number): CerebrasRateLimitWait | null => {
  if (waits >= CEREBRAS_REFUSAL_WAIT_MAX_WAITS) return null;
  const planned = cerebrasRateLimitWait(headers, Date.now());
  if (planned === null) return null;
  if (planned.waitMs > CEREBRAS_REFUSAL_WAIT_CAP_MS) return null;
  if (waitedMs + planned.waitMs > CEREBRAS_REFUSAL_WAIT_TOTAL_CAP_MS) return null;
  return planned;
};

/** One absorbed pause, logged with the header it came from. */
export const logCerebrasRateLimitWait = (fields: Readonly<Record<string, string | number | null>>): void => {
  try {
    console.info("[ai.ubq.fi] cerebras_rate_limit_wait", JSON.stringify(fields));
  } catch {
    // Telemetry must never change routing or delivery.
  }
};

/** Records the absorbed wait total so the terminal can report it. */
export const recordCerebrasRateLimitWaitMs = (usageContext: UsageContext | undefined, waitedMs: number): void => {
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.rateLimitWaitMs = waitedMs;
};

/** Abort-aware sleep: an abandoned request never keeps waiting for a provider window. */
export const waitForCerebrasRetry = (milliseconds: number, signal: AbortSignal): Promise<void> => {
  if (milliseconds <= 0) return Promise.resolve();
  if (signal.aborted) return Promise.reject(new DOMException("The request was aborted while waiting for the Cerebras rate limit to reset.", "AbortError"));
  return new Promise<void>((resolve, reject) => {
    const timer = setTimeout(() => {
      signal.removeEventListener("abort", onAbort);
      resolve();
    }, milliseconds);
    function onAbort(): void {
      clearTimeout(timer);
      reject(new DOMException("The request was aborted while waiting for the Cerebras rate limit to reset.", "AbortError"));
    }
    signal.addEventListener("abort", onAbort, { once: true });
  });
};
