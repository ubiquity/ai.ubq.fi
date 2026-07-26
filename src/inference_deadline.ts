// Stay below both the unchanged 120-second router timeout and the runtime
// eviction window observed for queued provider requests. This leaves enough
// time to cancel upstream and return an attributed gateway error.
export const INFERENCE_DEADLINE_MS = 85_000;
export const STREAM_FIRST_EVENT_DEADLINE_MS = 85_000;
export const STREAM_INACTIVITY_DEADLINE_MS = 85_000;
export const BUFFERED_INFERENCE_DEADLINE_MS = 85_000;

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
): Readonly<{ signal: AbortSignal; clear: () => void }> => {
  const deadline = new AbortController();
  let active = true;
  const timer = setTimeout(() => {
    if (!active) return;
    deadline.abort(new DOMException("Upstream response headers or first SSE event timed out.", "TimeoutError"));
  }, timeoutMs);
  return {
    signal: AbortSignal.any([requestSignal, deadline.signal]),
    clear: () => {
      if (!active) return;
      active = false;
      clearTimeout(timer);
    },
  };
};

export const setStreamFirstEventDeadlineMsForTest = (timeoutMs: number | null): void => {
  streamFirstEventDeadlineMs = timeoutMs ?? STREAM_FIRST_EVENT_DEADLINE_MS;
};
