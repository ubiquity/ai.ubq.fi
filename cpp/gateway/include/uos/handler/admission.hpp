#pragma once
// Port of src/handler/admission.ts.
// Inference admission controller glue, split out of src/handler.ts.
#include "uos/inference_admission.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/ordered.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace uos::handler::admission {
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
  inline const rt::OrderedSet<std::string> ADMISSION_ROUTES{
    "embeddings", "embeddings.jobs.create", "embeddings.jobs.get", "chat.completions", "responses", "decisions", "systemone", "images.generations", "images.edits",
  };

  /** The explicit local-overload error code; never a provider quota or fallback signal. */
  inline constexpr std::string_view LOCAL_INFERENCE_OVERLOAD_CODE = "local_inference_overload";

  /**
   * Internal test seam: install a small deterministic guard so an HTTP fixture
   * can reach the active and waiting bounds without 192 live requests. It is not
   * a runtime configuration surface, and null restores the shared controller.
   */
  void setInferenceAdmissionControllerForTest(std::optional<inference_admission::InferenceAdmissionController> controller);

  inference_admission::InferenceAdmissionController admissionController();

  /**
   * The local finite-overload refusal. It is a gateway decision, so it carries a
   * dedicated error code and a bounded Retry-After instead of borrowing provider
   * quota, capacity or paid-fallback vocabulary. The queue wait stays internal
   * telemetry rather than a response header.
   */
  rt::Response localOverloadResponse(const inference_admission::InferenceAdmissionLocalOverloadCause& cause);
}
