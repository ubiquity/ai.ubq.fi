// Inference admission controller glue, split out of src/handler.ts.

import { openaiError } from "./http.ts";
import {
  acquireInferenceAdmission,
  DEFAULT_INFERENCE_ADMISSION_LIMITS,
  type InferenceAdmissionController,
  type InferenceAdmissionLocalOverloadCause,
  inferenceAdmissionSnapshot,
} from "./inference_admission.ts";

/**
 * Routes that hold process resources - an upstream transport, its retained
 * response buffer and the downstream body - for as long as the request lives.
 * Catalog reads (`/v1/models`) are deliberately absent: they never dispatch
 * provider inference and retain no provider response, so they must not consume
 * a permit. `embeddings.jobs.get` is present even though it also serves
 * completed-job reads, because a queued job poll calls
 * `runEmbeddingsJobAttempt`, which can dispatch `fetchVoyageEmbeddings`;
 * separating that work from an ordinary read would require job-state inspection
 * shared into the embeddings module, so every job poll is admitted instead of
 * letting a work-producing poll bypass the bound. The finite guard is the
 * merged internal controller, not caller-lane admission: it holds no per-caller
 * lease and caps no principal.
 */
const ADMISSION_ROUTES: ReadonlySet<string> = new Set([
  "embeddings",
  "embeddings.jobs.create",
  "embeddings.jobs.get",
  "chat.completions",
  "responses",
  "images.generations",
  "images.edits",
]);

/** The explicit local-overload error code; never a provider quota or fallback signal. */
export const LOCAL_INFERENCE_OVERLOAD_CODE = "local_inference_overload";

const sharedAdmissionController: InferenceAdmissionController = {
  acquire: acquireInferenceAdmission,
  snapshot: inferenceAdmissionSnapshot,
};

let admissionControllerForTest: InferenceAdmissionController | null = null;

/**
 * Internal test seam: install a small deterministic guard so an HTTP fixture
 * can reach the active and waiting bounds without 192 live requests. It is not
 * a runtime configuration surface, and null restores the shared controller.
 */
export const setInferenceAdmissionControllerForTest = (controller: InferenceAdmissionController | null): void => {
  admissionControllerForTest = controller;
};

const admissionController = (): InferenceAdmissionController => admissionControllerForTest ?? sharedAdmissionController;

/**
 * The local finite-overload refusal. It is a gateway decision, so it carries a
 * dedicated error code and a bounded Retry-After instead of borrowing provider
 * quota, capacity or paid-fallback vocabulary. The queue wait stays internal
 * telemetry rather than a response header.
 */
const localOverloadResponse = (cause: InferenceAdmissionLocalOverloadCause): Response => {
  const retryAfterSeconds = Math.max(1, Math.ceil(DEFAULT_INFERENCE_ADMISSION_LIMITS.maxQueueWaitMs / 1_000));
  const message =
    cause === "queue_limit"
      ? "The gateway is at its concurrent inference limit and its waiting queue is full."
      : `The gateway could not admit this request within ${DEFAULT_INFERENCE_ADMISSION_LIMITS.maxQueueWaitMs}ms.`;
  return openaiError(503, message, LOCAL_INFERENCE_OVERLOAD_CODE, {
    type: "server_error",
    param: null,
    headers: { "Retry-After": String(retryAfterSeconds) },
  });
};

// ---------------------------------------------------------------------------
// Request routing groups.  Each group returns the response it owns without CORS
// decoration, or null when the request belongs to a later group; the default
// export keeps the original order between the groups.
// ---------------------------------------------------------------------------

/** A route whose HTTP method and path are matched exactly. */

export { ADMISSION_ROUTES, admissionController, localOverloadResponse };
