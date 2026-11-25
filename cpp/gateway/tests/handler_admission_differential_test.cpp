// Differential tests for src/handler/admission.ts (its bun tests drive the whole
// handler and HTTP server, which are not ported yet). Expected values were recorded
// by running the TS module under Bun 1.4.0 on the same inputs.
// Links: src/handler/admission.cpp src/http.cpp src/inference_admission.cpp src/auth/relay.cpp
//        src/config.cpp src/provider/cerebras_rate_limits.cpp src/release.cpp
#include "uos/handler/admission.hpp"

#include "uos/inference_admission.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"

#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace admission = uos::handler::admission;
namespace inference_admission = uos::inference_admission;

namespace {
  rt::Task<json::value> describe(rt::Response response) {
    json::object out;
    out["status"] = response.status();
    out["statusText"] = response.statusText();
    json::array headers;
    for (const auto& [name, value] : response.headers().entries()) headers.push_back(json::array{json::string(name), json::string(value)});
    out["headers"] = std::move(headers);
    const std::string body = co_await response.text();
    out["body"] = body;
    co_return json::value(std::move(out));
  }

  json::value snapshotOf(const inference_admission::InferenceAdmissionController& controller) {
    const inference_admission::InferenceAdmissionSnapshot snapshot = controller.snapshot();
    json::object out;
    out["active"] = rt::makeNumber(snapshot.active);
    out["waiting"] = rt::makeNumber(snapshot.waiting);
    out["principals"] = rt::makeNumber(snapshot.principals);
    out["maxActive"] = rt::makeNumber(snapshot.maxActive);
    out["maxWaiting"] = rt::makeNumber(snapshot.maxWaiting);
    return out;
  }

  bool same(const json::value& actual, std::string_view wanted) {
    const bool equal = rt::deepEqual(actual, rt::jsonParse(wanted));
    if (!equal) std::fprintf(stderr, "    actual:   %s\n    expected: %.*s\n", rt::jsonStringify(actual).c_str(), static_cast<int>(wanted.size()), wanted.data());
    return equal;
  }
}

TEST("ADMISSION_ROUTES and LOCAL_INFERENCE_OVERLOAD_CODE match the TS constants") {
  json::array routes;
  for (const std::string& route : admission::ADMISSION_ROUTES) routes.push_back(json::string(route));
  EXPECT(same(json::value(routes),
              R"(["embeddings","embeddings.jobs.create","embeddings.jobs.get","chat.completions","responses","decisions","systemone","images.generations","images.edits"])"));
  EXPECT_EQ(admission::LOCAL_INFERENCE_OVERLOAD_CODE, std::string_view("local_inference_overload"));
  EXPECT(admission::ADMISSION_ROUTES.has(std::string("responses")));
  EXPECT(!admission::ADMISSION_ROUTES.has(std::string("models")));
}

ASYNC_TEST("localOverloadResponse matches Bun for both overload causes") {
  const rt::Response queueLimit = admission::localOverloadResponse("queue_limit");
  const json::value queueLimitDescribed = co_await describe(queueLimit);
  EXPECT(same(
    queueLimitDescribed,
    R"({"status":503,"statusText":"","headers":[["content-type","application/json"],["retry-after","5"]],"body":"{\"error\":{\"message\":\"The gateway is at its concurrent inference limit and its waiting queue is full.\",\"type\":\"server_error\",\"code\":\"local_inference_overload\",\"param\":null}}"})"));
  const rt::Response queueWaitTimeout = admission::localOverloadResponse("queue_wait_timeout");
  const json::value queueWaitTimeoutDescribed = co_await describe(queueWaitTimeout);
  EXPECT(same(
    queueWaitTimeoutDescribed,
    R"({"status":503,"statusText":"","headers":[["content-type","application/json"],["retry-after","5"]],"body":"{\"error\":{\"message\":\"The gateway could not admit this request within 5000ms.\",\"type\":\"server_error\",\"code\":\"local_inference_overload\",\"param\":null}}"})"));
}

ASYNC_TEST("admissionController switches between the shared and the test controller") {
  EXPECT(same(snapshotOf(admission::admissionController()), R"({"active":0,"waiting":0,"principals":0,"maxActive":512,"maxWaiting":128})"));
  const inference_admission::InferenceAdmissionController custom =
    inference_admission::createInferenceAdmissionController({.maxActive = 1, .maxWaiting = 2, .maxQueueWaitMs = 50});
  admission::setInferenceAdmissionControllerForTest(custom);
  EXPECT(same(snapshotOf(admission::admissionController()), R"({"active":0,"waiting":0,"principals":0,"maxActive":1,"maxWaiting":2})"));
  inference_admission::InferenceAdmissionRequest firstRequest;
  firstRequest.principal = "a";
  const rt::Promise<inference_admission::InferenceAdmissionResult> firstAcquire = admission::admissionController().acquire(std::move(firstRequest));
  const inference_admission::InferenceAdmissionResult first = co_await firstAcquire.wait();
  EXPECT(std::holds_alternative<inference_admission::InferenceAdmissionGranted>(first));
  EXPECT(same(snapshotOf(admission::admissionController()), R"({"active":1,"waiting":0,"principals":0,"maxActive":1,"maxWaiting":2})"));
  if (const auto* granted = std::get_if<inference_admission::InferenceAdmissionGranted>(&first)) granted->release();
  admission::setInferenceAdmissionControllerForTest(std::nullopt);
  EXPECT(same(snapshotOf(admission::admissionController()), R"({"active":0,"waiting":0,"principals":0,"maxActive":512,"maxWaiting":128})"));
  inference_admission::InferenceAdmissionRequest sharedRequest;
  const rt::Promise<inference_admission::InferenceAdmissionResult> sharedAcquire = admission::admissionController().acquire(std::move(sharedRequest));
  const inference_admission::InferenceAdmissionResult shared = co_await sharedAcquire.wait();
  EXPECT(std::holds_alternative<inference_admission::InferenceAdmissionGranted>(shared));
  EXPECT(same(snapshotOf(admission::admissionController()), R"({"active":1,"waiting":0,"principals":0,"maxActive":512,"maxWaiting":128})"));
  if (const auto* granted = std::get_if<inference_admission::InferenceAdmissionGranted>(&shared)) granted->release();
}
