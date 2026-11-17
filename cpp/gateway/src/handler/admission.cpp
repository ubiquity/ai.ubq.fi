#include "uos/handler/admission.hpp"

#include "uos/http.hpp"
#include "uos/rt/number.hpp"

#include <string>
#include <utility>

namespace uos::handler::admission {
  namespace {
    const inference_admission::InferenceAdmissionController& sharedAdmissionController() {
      static const inference_admission::InferenceAdmissionController controller{
        .acquire = [](inference_admission::InferenceAdmissionRequest request) { return inference_admission::acquireInferenceAdmission(std::move(request)); },
        .snapshot = [] { return inference_admission::inferenceAdmissionSnapshot(); },
      };
      return controller;
    }

    std::optional<inference_admission::InferenceAdmissionController>& admissionControllerForTest() {
      static std::optional<inference_admission::InferenceAdmissionController> controller;
      return controller;
    }
  }

  void setInferenceAdmissionControllerForTest(std::optional<inference_admission::InferenceAdmissionController> controller) {
    admissionControllerForTest() = std::move(controller);
  }

  inference_admission::InferenceAdmissionController admissionController() {
    return admissionControllerForTest() ? *admissionControllerForTest() : sharedAdmissionController();
  }

  rt::Response localOverloadResponse(const inference_admission::InferenceAdmissionLocalOverloadCause& cause) {
    const double maxQueueWaitMs = inference_admission::DEFAULT_INFERENCE_ADMISSION_LIMITS.maxQueueWaitMs;
    const double retryAfterSeconds = rt::Math::max(1.0, rt::Math::ceil(maxQueueWaitMs / 1'000));
    const std::string message = cause == "queue_limit" ? std::string("The gateway is at its concurrent inference limit and its waiting queue is full.")
                                                       : "The gateway could not admit this request within " + rt::numberToString(maxQueueWaitMs) + "ms.";
    http::OpenaiErrorOptions options;
    options.type = "server_error";
    options.param = std::optional<std::string>(std::nullopt);
    options.headers = rt::Headers{{"Retry-After", rt::numberToString(retryAfterSeconds)}};
    return http::openaiError(503, message, std::string(LOCAL_INFERENCE_OVERLOAD_CODE), std::move(options));
  }
}
