/** Reference timeout guidance for direct OpenAI clients. */
export const OPENAI_DEFAULT_REQUEST_TIMEOUT_MS = 10 * 60_000;
export const OPENAI_FLEX_REQUEST_TIMEOUT_MS = 15 * 60_000;

/**
 * Cloudflare's default proxy-read timeout is 125 seconds. Return stream
 * headers and the first SSE event before that edge limit. Once semantic output
 * starts, downstream keepalives keep the connection active while the provider's
 * normal request timeout bounds post-semantic inactivity.
 */
export const STREAM_FIRST_EVENT_DEADLINE_MS = 120_000;
export const STREAM_FAILOVER_RESERVE_MS = 15_000;
export const STREAM_INACTIVITY_DEADLINE_MS = OPENAI_DEFAULT_REQUEST_TIMEOUT_MS;

/** Buffered responses must finish before Cloudflare's 125-second read limit. */
export const INFERENCE_DEADLINE_MS = STREAM_FIRST_EVENT_DEADLINE_MS;
export const BUFFERED_INFERENCE_DEADLINE_MS = INFERENCE_DEADLINE_MS;

let streamFirstEventDeadlineMs = STREAM_FIRST_EVENT_DEADLINE_MS;

export const createInferenceSignal = (
  requestSignal: AbortSignal,
  timeoutMs = BUFFERED_INFERENCE_DEADLINE_MS,
): AbortSignal => AbortSignal.any([requestSignal, AbortSignal.timeout(timeoutMs)]);

/**
 * Bounds the period before the first upstream SSE event, including provider
 * dispatch and response headers. Once the caller observes that first event it
 * must clear this deadline; the stream reader then owns renewable inactivity.
 */
export const createStreamFirstEventDeadline = (
  requestSignal: AbortSignal,
  timeoutMs = streamFirstEventDeadlineMs,
): StreamDeadline => {
  const deadline = new AbortController();
  const deadlineAtMs = performance.now() + timeoutMs;
  let active = true;
  const timer = setTimeout(() => {
    if (!active) return;
    deadline.abort(new DOMException("Upstream response headers or first SSE event timed out.", "TimeoutError"));
  }, timeoutMs);
  return {
    signal: AbortSignal.any([requestSignal, deadline.signal]),
    abort: (reason) => deadline.abort(reason),
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
export const createStreamSemanticDeadline = (
  requestSignal: AbortSignal,
  timeoutMs = streamFirstEventDeadlineMs,
): StreamDeadline => {
  const deadline = new AbortController();
  const deadlineAtMs = performance.now() + timeoutMs;
  let active = true;
  const timer = setTimeout(() => {
    if (!active) return;
    deadline.abort(new DOMException("Upstream response timed out before semantic output.", "TimeoutError"));
  }, timeoutMs);
  return {
    signal: AbortSignal.any([requestSignal, deadline.signal]),
    abort: (reason) => deadline.abort(reason),
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
