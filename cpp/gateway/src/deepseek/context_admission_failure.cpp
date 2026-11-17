#include "uos/deepseek/context_admission_failure.hpp"

#include "uos/http.hpp"
#include "uos/lib/jev_compaction/codex_items.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/weak.hpp"

namespace uos::deepseek::context_admission_failure {
  namespace {
    rt::WeakSet<rt::Response>& localContextFailures() { static rt::WeakSet<rt::Response> values; return values; }
  }
  bool isCodexNativeRequest(const rt::Request& req) {
    const auto metadata = ::uos::lib::jev_compaction::codex_items::parseTurnMetadata(req.headers().get("x-codex-turn-metadata"));
    if (!metadata) return false;
    for (const auto key : {"session_id", "thread_id", "turn_id"}) {
      const auto value = rt::getString(*metadata, key); if (!value || rt::trim(*value).empty()) return false;
    }
    return true;
  }
  bool isLocalContextAdmissionFailure(const rt::Response& response) { return localContextFailures().has(response); }
  rt::Response chatResponsesProjectionFailure(const rt::Request& req, const json::object& rawRecord, const responses::DeepSeekResponsesFailure& failure,
                                              const openai_telemetry::UsageContext* context) {
    if (rt::getBool(rawRecord, "stream") != true || failure.kind != "aggregate_context_overflow" || !isCodexNativeRequest(req)) {
      http::OpenaiErrorOptions options; options.param = std::optional<std::string>(failure.param);
      return http::openaiError(400, failure.message, failure.code.value_or("invalid_request_error"), options);
    }
    const auto id = "resp_" + rt::replaceAll(rt::crypto::randomUUID(), "-", "");
    json::object response{{"id", id}, {"object", "response"}, {"created_at", rt::makeNumber(rt::Math::floor(rt::dateNow() / 1000))},
                          {"status", "in_progress"}, {"error", nullptr}, {"output", json::array()}, {"usage", nullptr}};
    json::object error; rt::objectSet(error, "code", failure.code); error["message"] = failure.message; error["param"] = failure.param;
    auto failed = response; failed["status"] = "failed"; failed["error"] = error;
    const json::array events{json::object{{"type", "response.created"}, {"sequence_number", 0}, {"response", response}},
                             json::object{{"type", "response.failed"}, {"sequence_number", 1}, {"response", failed}}};
    std::string body;
    for (const auto& event : events) body += "event: " + std::string(*rt::getString(event, "type")) + "\ndata: " + rt::jsonStringify(event) + "\n\n";
    rt::Response result(body, rt::ResponseInit{.status = 200, .headers = rt::Headers{{"content-type", "text/event-stream"}, {"cache-control", "no-store"}}});
    const auto state = context && context->responseTelemetry ? context->responseTelemetry : openai_telemetry::createResponseTelemetryState();
    state->completed = false; state->semanticOutputObserved = false; state->stream = true; state->streamTerminalType = "response.failed";
    state->failureKind = "aggregate_context_overflow"; state->syntheticTerminalType = "response.failed"; state->responseCreatedObserved = true;
    openai_telemetry::recordTerminalUsage(context, std::nullopt, false); localContextFailures().add(result);
    return openai_telemetry::attachResponseTelemetry(result, state);
  }
}
