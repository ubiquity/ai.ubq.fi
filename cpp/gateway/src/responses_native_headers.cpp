// Port of src/responses-native-headers.ts.
#include "uos/responses_native_headers.hpp"
#include "uos/runtime/http_early_sse.hpp"
#include "uos/deepseek/context_admission_failure.hpp"
#include "uos/http.hpp"
#include "uos/openai.hpp"
#include "uos/provider/cerebras.hpp"
#include "uos/utils.hpp"
#include "uos/bounded_response_body.hpp"
#include "uos/sentinel/upstream_capture.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/record.hpp"
namespace uos::responses_native_headers {
namespace {
  rt::WeakMap<rt::Request, NativeHttpFailure> originalFailures;
  rt::Task<std::optional<api_key_policy::ApiKeyProviderDispatch>> beforeNativeDispatch(rt::Request req,
    rt::Headers knownHeaders, std::optional<openai_telemetry::UsageContext> context,
    api_key_policy::ApiKeyUsageReservation::BeforeProviderDispatch beforeDispatch, std::string provider) {
    std::optional<api_key_policy::ApiKeyProviderDispatch> receipt;
    if (beforeDispatch) { auto task = beforeDispatch(provider); rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>> pending(std::move(task)); receipt = co_await pending.wait(); }
    else co_await rt::microtask();
    if (!openai::downstreamSignalFor(req, context ? &*context : nullptr).aborted()) {
      const runtime::http_early_sse::EarlySseHeaders value{knownHeaders}; (void)runtime::http_early_sse::tryCommitEarlySseHeaders(req, value);
    }
    co_return receipt;
  }
}
void tag_invoke(const json::value_from_tag&, json::value& out, const NativeHttpFailure& value) {
  out = rt::record::Writer().required("status", value.status).required("headers", value.headers).required("headers_truncated", value.headers_truncated)
    .required("error", value.error).required("body_complete", value.body_complete).done();
}
NativeHttpFailure tag_invoke(const json::value_to_tag<NativeHttpFailure>&, const json::value& value) {
  const rt::record::Reader in(value, "NativeHttpFailure");
  return {in.required<double>("status"), in.required<json::object>("headers"), in.required<bool>("headers_truncated"), in.required<json::value>("error"), in.required<bool>("body_complete")};
}
std::optional<NativeHttpFailure> originalNativeHttpFailure(std::optional<rt::Request> request) {
  if (!request) return std::nullopt;
  return originalFailures.get(*request);
}
std::optional<openai_telemetry::UsageContext> nativeHeaderUsageContext(rt::Request req, const json::object& raw, const openai_telemetry::UsageContext* context) {
  const auto model = rt::trim(rt::getString(raw, "model").value_or(""));
  if (!context) return std::nullopt;
  if (rt::getBool(raw, "stream") != true || !deepseek::context_admission_failure::isCodexNativeRequest(req) || provider::cerebras::cerebrasUpstreamModelFor(model)) return *context;
  rt::Headers headers{{"content-type", "text/event-stream"}, {"cache-control", "no-store"}};
  if (context->requestId && !context->requestId->empty()) headers.set("x-uos-request-id", *context->requestId);
  const auto knownHeaders = http::withCors(rt::Response(nullptr, {.headers = headers}), req).headers();
  auto result = *context;
  const auto beforeDispatch = context->beforeProviderDispatch;
  result.beforeProviderDispatch = [req, knownHeaders, source = std::optional(*context), beforeDispatch](std::string provider) {
    return beforeNativeDispatch(req, knownHeaders, source, beforeDispatch, std::move(provider));
  };
  return result;
}
rt::Task<rt::Response> nativeFailureAfterEarlyHeaders(rt::Request req, rt::Response response, openai_telemetry::UsageContext context) {
  const rt::Regex contentType("^text/event-stream(?:\\s*;|$)", "i");
  if (response.ok() || contentType.test(response.headers().get("content-type").value_or("")) || !runtime::http_early_sse::earlySseHeadersCommitted(req) || req.signal().aborted()) co_return response;
  bounded_response_body::ReadBoundedResponseBodyOptions options;
  options.signal = req.signal(); options.cancellationReason = "Selected native HTTP error body incomplete";
  auto task = bounded_response_body::readBoundedResponseBody(response, options); const auto captured = co_await std::move(task); co_await rt::microtask();
  const auto text = captured.complete ? rt::TextDecoder().decode(captured.bytes) : std::string();
  json::value payload = nullptr; try { payload = rt::jsonParse(text); } catch (...) {}
  const auto* errorValue = rt::get(payload, "error");
  const json::value originalError = utils::isRecord(payload) && utils::isRecord(errorValue) ? *errorValue : json::value(json::object{{"message", text.empty() ? "Upstream request failed with HTTP " + rt::numberToString(response.status()) + "." : text}});
  const std::string code(rt::getString(originalError, "code").value_or("server_error"));
  const auto rawMessage = rt::getString(originalError, "message");
  const std::string message = rawMessage ? std::string(*rawMessage) : "Upstream request failed with HTTP " + rt::numberToString(response.status()) + ".";
  const auto id = "resp_" + rt::replaceAll(rt::crypto::randomUUID(), "-", "");
  const json::object envelope{{"id", id}, {"object", "response"}, {"created_at", rt::makeNumber(rt::Math::floor(rt::dateNow() / 1000))}, {"status", "in_progress"}, {"error", nullptr}, {"output", json::array{}}, {"usage", nullptr}};
  auto failed = envelope; failed["status"] = "failed"; failed["error"] = json::object{{"code", code}, {"message", message}};
  const std::vector<json::object> events{{{"type", "response.created"}, {"sequence_number", 0}, {"response", envelope}}, {{"type", "response.failed"}, {"sequence_number", 1}, {"response", failed}}};
  std::string body; for (const auto& event : events) body += "event: " + std::string(*rt::getString(event, "type")) + "\ndata: " + rt::jsonStringify(event) + "\n\n";
  rt::Response result(body, {.headers = rt::Headers{{"content-type", "text/event-stream"}, {"cache-control", "no-store"}}});
  json::object headers; bool truncated = false;
  for (const auto name : sentinel::upstream_capture::SENTINEL_UPSTREAM_SAFE_RESPONSE_HEADER_NAMES) {
    const auto value = response.headers().get(name); if (!value) continue;
    headers[std::string(name)] = rt::slice(*value, 0, sentinel::upstream_capture::SENTINEL_UPSTREAM_MAX_HEADER_VALUE_CHARS);
    truncated = truncated || rt::length(*value) > sentinel::upstream_capture::SENTINEL_UPSTREAM_MAX_HEADER_VALUE_CHARS;
  }
  originalFailures.set(req, NativeHttpFailure{static_cast<double>(response.status()), headers, truncated, captured.complete ? originalError : json::value(nullptr), captured.complete});
  const auto telemetry = context.responseTelemetry;
  if (telemetry) {
    telemetry->completed = false; telemetry->semanticOutputObserved = false; telemetry->stream = true; telemetry->streamTerminalType = "response.failed";
    if (!telemetry->failureKind) telemetry->failureKind = code;
    telemetry->syntheticTerminalType = "response.failed"; telemetry->responseCreatedObserved = true; openai_telemetry::recordStreamTerminal(&context);
  }
  co_return result;
}
}
