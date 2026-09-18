/** Reference timeout guidance for direct OpenAI clients. */
export const OPENAI_DEFAULT_REQUEST_TIMEOUT_MS = 10 * 60_000;
export const OPENAI_FLEX_REQUEST_TIMEOUT_MS = 15 * 60_000;

/**
 * Wall-clock budget for one inference attempt: provider dispatch, response
 * headers and the first SSE event must all arrive inside it. Once semantic
 * output starts, STREAM_INACTIVITY_DEADLINE_MS bounds the gaps between later
 * events instead. Both were raised from the original 125-second Cloudflare
 * proxy-read bound to 30 minutes so a long agent turn is not cut off at that
 * edge limit.
 */
export const STREAM_FIRST_EVENT_DEADLINE_MS = 1_800_000;
export const STREAM_FAILOVER_RESERVE_MS = 15_000;
export const STREAM_INACTIVITY_DEADLINE_MS = 1_800_000;

/**
 * Bounds dispatch plus response headers for exactly one paid-tier attempt, so a
 * stalled provider cannot consume the whole 30-minute stream deadline before
 * the next tier is tried.
 *
 * Measured first-headers latency for successful Surplus `gpt-6-astra` requests:
 * p50 4.3 s, p90 14.4 s, max 111.8 s. Observed stalls exceeded 60 s with zero
 * bytes and every one succeeded in about 2 s on immediate retry, so this is
 * transient saturation rather than a slow success. 120 s clears the slowest
 * measured success with margin and still bounds a stall at 15x below
 * STREAM_FIRST_EVENT_DEADLINE_MS, which is what lets the waterfall reach the
 * next enabled paid tier instead of abandoning the request.
 */
export const PAID_PROVIDER_FIRST_HEADERS_DEADLINE_MS = 120_000;

/**
 * Buffered inference shares the stream first-event budget. It is not bounded by
 * the original 125-second Cloudflare read limit; the caller's own request
 * signal still caps the whole request.
 */
export const INFERENCE_DEADLINE_MS = STREAM_FIRST_EVENT_DEADLINE_MS;
export const BUFFERED_INFERENCE_DEADLINE_MS = INFERENCE_DEADLINE_MS;

let streamFirstEventDeadlineMs = STREAM_FIRST_EVENT_DEADLINE_MS;

export const createInferenceSignal = (requestSignal: AbortSignal, timeoutMs = BUFFERED_INFERENCE_DEADLINE_MS): AbortSignal =>
  AbortSignal.any([requestSignal, AbortSignal.timeout(timeoutMs)]);

/**
 * Bounds the period before the first upstream SSE event, including provider
 * dispatch and response headers. Once the caller observes that first event it
 * must clear this deadline; the stream reader then owns renewable inactivity.
 */
export const createStreamFirstEventDeadline = (requestSignal: AbortSignal, timeoutMs = streamFirstEventDeadlineMs): StreamDeadline => {
  const deadline = new AbortController();
  const deadlineAtMs = performance.now() + timeoutMs;
  let active = true;
  const timer = setTimeout(() => {
    if (!active) return;
    deadline.abort(new DOMException("Upstream response headers or first SSE event timed out.", "TimeoutError"));
  }, timeoutMs);
  return {
    signal: AbortSignal.any([requestSignal, deadline.signal]),
    abort: (reason) => {
      deadline.abort(reason);
    },
    clear: () => {
      if (!active) return;
      active = false;
      clearTimeout(timer);
    },
    remainingMs: () => Math.max(0, deadlineAtMs - performance.now()),
  };
};

export type StreamDeadline = Readonly<{
  signal: AbortSignal;
  abort: (reason?: unknown) => void;
  clear: () => void;
  remainingMs: () => number;
}>;

/**
 * Bounds one provider attempt until semantic Responses output or a valid
 * terminal event. Each failover attempt gets its own controller and timer.
 */
export const createStreamSemanticDeadline = (requestSignal: AbortSignal, timeoutMs = streamFirstEventDeadlineMs): StreamDeadline => {
  const deadline = new AbortController();
  const deadlineAtMs = performance.now() + timeoutMs;
  let active = true;
  const timer = setTimeout(() => {
    if (!active) return;
    deadline.abort(new DOMException("Upstream response timed out before semantic output.", "TimeoutError"));
  }, timeoutMs);
  return {
    signal: AbortSignal.any([requestSignal, deadline.signal]),
    abort: (reason) => {
      deadline.abort(reason);
    },
    clear: () => {
      if (!active) return;
      active = false;
      clearTimeout(timer);
    },
    remainingMs: () => Math.max(0, deadlineAtMs - performance.now()),
  };
};

let paidProviderFirstHeadersDeadlineMs = PAID_PROVIDER_FIRST_HEADERS_DEADLINE_MS;

/**
 * Bounds one paid-provider attempt until response headers arrive. The caller
 * clears the timer once the attempt settles, so the delivered response body is
 * never tied to this deadline; request-level streaming keeps its own deadlines.
 */
export const createPaidProviderAttemptDeadline = (requestSignal?: AbortSignal): StreamDeadline => {
  const deadline = new AbortController();
  const deadlineAtMs = performance.now() + paidProviderFirstHeadersDeadlineMs;
  let active = true;
  const timer = setTimeout(() => {
    if (!active) return;
    deadline.abort(new DOMException("Paid provider response headers timed out.", "TimeoutError"));
  }, paidProviderFirstHeadersDeadlineMs);
  return {
    signal: requestSignal ? AbortSignal.any([requestSignal, deadline.signal]) : deadline.signal,
    abort: (reason) => {
      deadline.abort(reason);
    },
    clear: () => {
      if (!active) return;
      active = false;
      clearTimeout(timer);
    },
    remainingMs: () => Math.max(0, deadlineAtMs - performance.now()),
  };
};

export const setStreamFirstEventDeadlineMsForTest = (timeoutMs: number | null): void => {
  streamFirstEventDeadlineMs = timeoutMs ?? STREAM_FIRST_EVENT_DEADLINE_MS;
};

export const setPaidProviderFirstHeadersDeadlineMsForTest = (timeoutMs: number | null): void => {
  paidProviderFirstHeadersDeadlineMs = timeoutMs ?? PAID_PROVIDER_FIRST_HEADERS_DEADLINE_MS;
};
