export const INFERENCE_DEADLINE_MS = 110_000;

export const createInferenceSignal = (
  requestSignal: AbortSignal,
  timeoutMs = INFERENCE_DEADLINE_MS,
): AbortSignal => AbortSignal.any([requestSignal, AbortSignal.timeout(timeoutMs)]);
