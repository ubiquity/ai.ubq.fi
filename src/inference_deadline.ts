// Stay below both the unchanged 120-second router timeout and the runtime
// eviction window observed for queued provider requests. This leaves enough
// time to cancel upstream and return an attributed gateway error.
export const INFERENCE_DEADLINE_MS = 85_000;

export const createInferenceSignal = (
  requestSignal: AbortSignal,
  timeoutMs = INFERENCE_DEADLINE_MS,
): AbortSignal => AbortSignal.any([requestSignal, AbortSignal.timeout(timeoutMs)]);
