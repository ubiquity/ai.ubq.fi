#pragma once
// Port of src/deepseek/context-admission-failure.ts.
#include "uos/deepseek/responses.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/rt/http.hpp"

namespace uos::deepseek::context_admission_failure {
  bool isCodexNativeRequest(const rt::Request& req);
  bool isLocalContextAdmissionFailure(const rt::Response& response);
  rt::Response chatResponsesProjectionFailure(const rt::Request& req, const json::object& rawRecord,
                                              const responses::DeepSeekResponsesFailure& failure, const openai_telemetry::UsageContext* context = nullptr);
}
