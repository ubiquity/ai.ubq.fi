// Port of tests/responses-native-headers.test.ts: all nine source cases. Existing early-SSE tests preserved.
// Dispatch receipt identity is pinned by the shared closure effects and the unchanged quota ledger.
// Links the sources of tests/serve_delivery_http_test.cpp.
#include "tests/helpers/terminal_log_harness.hpp"
#include "tests/helpers/counting_kv.hpp"
#include "tests/helpers/loopback_http.hpp"
#include "uos/api_keys.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/http.hpp"
#include "uos/request.hpp"
#include "uos/responses_native_headers.hpp"
#include "uos/runtime/http.hpp"
#include "uos/runtime/http_early_sse.hpp"

using namespace uos;
namespace fixture = tests::helpers::terminal_log_harness;

namespace {
  namespace native = responses_native_headers;
  namespace early = runtime::http_early_sse;
  namespace wire = runtime::http;
  using tests::helpers::loopback_http::RawClient;
  using tests::helpers::loopback_http::ScopedLoopbackFetch;
  rt::Headers nativeHeaders() { return rt::Headers{{"x-codex-turn-metadata", R"({"session_id":"session","thread_id":"thread","turn_id":"turn"})"}}; }
  json::object nativeRaw() { return json::object{{"model", "deepseek-ai/DeepSeek-V4.1-Flash"}, {"input", "canary"}, {"stream", true}}; }
  openai_telemetry::UsageContext usage(api_key_policy::ApiKeyUsageReservation::BeforeProviderDispatch before = {}) {
    openai_telemetry::UsageContext result{.keyId = std::nullopt, .kernelRepo = std::nullopt, .kernelOrg = std::nullopt};
    result.requestId = "native-readiness-fixture";
    result.beforeProviderDispatch = std::move(before);
    result.responseTelemetry = openai_telemetry::createResponseTelemetryState();
    return result;
  }
  rt::Response errorResponse(double status, const std::string& code) {
    const json::object error{{"code", code}, {"message", "original " + code}, {"param", "input"}, {"type", "original_type"}};
    auto response = fixture::jsonResponse(rt::jsonStringify(json::object{{"error", error}}), status);
    response.headers().set("retry-after", "17");
    return response;
  }
  wire::ServeHttpOptions loopbackOptions() {
    wire::ServeHttpOptions result;
    result.hostname = "127.0.0.1";
    result.port = 0;
    result.onListen = [](const auto&) {};
    return result;
  }
  std::string origin(const wire::NetAddr& address) { return "http://127.0.0.1:" + rt::numberToString(address.port); }
  rt::Task<std::optional<api_key_policy::ApiKeyProviderDispatch>> dispatch(std::optional<openai_telemetry::UsageContext> context, std::string provider) {
    if (!context || !context->beforeProviderDispatch) co_return std::nullopt;
    const rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>> pending(context->beforeProviderDispatch(provider));
    auto result = co_await pending.wait();
    co_return result;
  }
  rt::RequestInit postInit() { return {.method = "POST", .headers = nativeHeaders(), .body = rt::BodyInit(std::string("{}"))}; }
}

ASYNC_TEST("native keyless dispatch preserves original body observation and aborts the same pending handler on disconnect") {
  fixture::Isolation isolation;
  std::string observedBody;
  std::optional<rt::Request> observedRequest;
  const auto aborted = rt::withResolvers<void>();
  const auto server = wire::serveHttp(loopbackOptions(), [&observedBody, &observedRequest, aborted](rt::Request req, wire::ServeHandlerInfo) -> rt::Task<rt::Response> {
    observedRequest = req;
    request::observeRawBodyOnce(req, [&observedBody](std::shared_ptr<rt::Bytes> bytes) { observedBody = rt::decodeUtf8(*bytes); }, [](const auto&) {});
    auto reading = request::readJsonBody(req);
    const auto parsed = co_await std::move(reading);
    EXPECT(rt::deepEqual(parsed, nativeRaw()));
    const auto context = native::nativeHeaderUsageContext(req, nativeRaw(), nullptr);
    EXPECT(!context);
    const auto source = usage();
    auto pending = dispatch(native::nativeHeaderUsageContext(req, nativeRaw(), &source), "lithos");
    const auto receipt = co_await std::move(pending);
    EXPECT(!receipt);
    EXPECT(early::earlySseHeadersCommitted(req));
    const rt::EventListener onAbort([aborted] { aborted.resolve(); });
    req.signal().addEventListener("abort", onAbort, {.once = true});
    co_await aborted.promise.wait();
    co_return rt::Response(nullptr);
  });
  const auto address = co_await server.ready.wait();
  const auto closed = rt::withResolvers<void>();
  std::string responseHead;
  auto socket = RawClient::connect(static_cast<int>(address.port));
  const auto body = rt::jsonStringify(nativeRaw());
  socket->once("connect", [socket, body](const auto&) {
    const auto headers = nativeHeaders();
    socket->write("POST /v1/responses HTTP/1.1\r\nHost: localhost\r\nx-codex-turn-metadata: " + *headers.get("x-codex-turn-metadata") +
      "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
  });
  socket->on("data", [socket, &responseHead](const std::string& data) { responseHead += data; if (rt::includes(responseHead, "\r\n\r\n")) socket->destroy(); });
  socket->once("error", [](const auto&) {});
  socket->once("close", [closed](const auto&) { closed.resolve(); });
  co_await closed.promise.wait();
  co_await aborted.promise.wait();
  EXPECT(rt::includes(responseHead, "200"));
  EXPECT(rt::includes(responseHead, "x-uos-request-id: native-readiness-fixture"));
  EXPECT_EQ(observedBody, body);
  ASSERT(observedRequest);
  EXPECT(observedRequest->signal().aborted());
  const auto stopping = server.shutdown();
  co_await stopping.wait();
}

ASYNC_TEST("failed dispatch admission and ineligible native requests keep original HTTP failures") {
  fixture::Isolation isolation;
  ScopedLoopbackFetch network;
  int admissions = 0;
  const auto server = wire::serveHttp(loopbackOptions(), [&admissions](rt::Request req, wire::ServeHandlerInfo) -> rt::Task<rt::Response> {
    const auto body = co_await req.text();
    EXPECT_EQ(body, std::string("{}"));
    const auto context = usage([&admissions](auto) -> rt::Task<std::optional<api_key_policy::ApiKeyProviderDispatch>> {
      ++admissions; throw rt::JsError("admission refused"); co_return std::nullopt;
    });
    auto admitting = dispatch(native::nativeHeaderUsageContext(req, nativeRaw(), &context), "lithos");
    EXPECT_REJECTS_MESSAGE(std::move(admitting), rt::JsError, "admission refused");
    EXPECT(!early::earlySseHeadersCommitted(req));
    const auto failure = errorResponse(429, "rate_limit_exceeded");
    auto encoding = native::nativeFailureAfterEarlyHeaders(req, failure, context);
    const auto encoded = co_await std::move(encoding);
    EXPECT(encoded == failure);
    co_return failure;
  });
  const auto address = co_await server.ready.wait();
  const auto init = postInit();
  auto fetching = rt::fetch(origin(address), init);
  const auto response = co_await std::move(fetching);
  EXPECT_EQ(response.status(), 429);
  EXPECT_EQ(response.headers().get("retry-after"), std::optional<std::string>("17"));
  const auto body = co_await response.text();
  EXPECT(rt::includes(body, "rate_limit_exceeded"));
  EXPECT_EQ(admissions, 1);
  const auto stopping = server.shutdown();
  co_await stopping.wait();
  const auto context = usage();
  std::vector<std::pair<rt::Headers, json::object>> ineligible;
  ineligible.push_back({rt::Headers(), nativeRaw()});
  ineligible.push_back({rt::Headers{{"x-codex-turn-metadata", "{}"}}, nativeRaw()});
  auto nonStream = nativeRaw(); nonStream["stream"] = false;
  ineligible.push_back({nativeHeaders(), nonStream});
  auto cerebras = nativeRaw(); cerebras["model"] = "gpt-oss-120b";
  ineligible.push_back({nativeHeaders(), cerebras});
  for (const auto& [headers, raw] : ineligible) {
    const rt::RequestInit requestInit{.method = "POST", .headers = headers};
    const rt::Request req("http://localhost/v1/responses", requestInit);
    const auto wrapped = native::nativeHeaderUsageContext(req, raw, &context);
    ASSERT(wrapped);
    EXPECT(wrapped->responseTelemetry == context.responseTelemetry);
    EXPECT(!wrapped->beforeProviderDispatch);
  }
}

namespace {
  rt::Task<void> selectedFailure(double status, std::string code) {
    fixture::Isolation isolation;
    ScopedLoopbackFetch network;
    std::optional<native::NativeHttpFailure> original;
    int admissions = 0;
    const auto context = usage([&admissions](auto) { ++admissions; return rt::fulfilledTask(std::optional<api_key_policy::ApiKeyProviderDispatch>()); });
    const auto server = wire::serveHttp(loopbackOptions(), [&original, context, status, code](rt::Request req, wire::ServeHandlerInfo) -> rt::Task<rt::Response> {
      const auto body = co_await req.text();
      EXPECT_EQ(body, std::string("{}"));
      auto admitting = dispatch(native::nativeHeaderUsageContext(req, nativeRaw(), &context), "lithos");
      const auto receipt = co_await std::move(admitting);
      EXPECT(!receipt);
      auto encoding = native::nativeFailureAfterEarlyHeaders(req, errorResponse(status, code), context);
      const auto result = co_await std::move(encoding);
      original = native::originalNativeHttpFailure(req);
      co_return result;
    });
    const auto address = co_await server.ready.wait();
    const auto init = postInit();
    auto fetching = rt::fetch(origin(address), init);
    const auto response = co_await std::move(fetching);
    EXPECT_EQ(response.status(), 200);
    const auto text = co_await response.text();
    std::vector<json::value> events;
    for (const auto& match : rt::matchAll(text, rt::Regex("^data: (.+)$", "gm"))) events.push_back(rt::jsonParse(*match[1]));
    ASSERT(events.size() == 2);
    EXPECT_EQ(rt::getString(events[0], "type"), std::optional<std::string>("response.created"));
    EXPECT_EQ(rt::getString(events[1], "type"), std::optional<std::string>("response.failed"));
    const auto* first = rt::get(events[0], "response");
    const auto* last = rt::get(events[1], "response");
    ASSERT(first && last);
    EXPECT_EQ(rt::getString(*first, "id"), rt::getString(*last, "id"));
    const json::object expected{{"code", code}, {"message", "original " + code}};
    EXPECT(rt::deepEqual(*rt::get(*last, "error"), expected));
    EXPECT(rt::get(*last, "usage")->is_null());
    EXPECT(rt::get(*last, "output")->as_array().empty());
    ASSERT(original);
    EXPECT_EQ(original->status, status);
    EXPECT_EQ(rt::getString(original->headers, "retry-after"), std::optional<std::string>("17"));
    const json::object originalError{{"code", code}, {"message", "original " + code}, {"param", "input"}, {"type", "original_type"}};
    EXPECT(rt::deepEqual(original->error, originalError));
    EXPECT(!context.responseTelemetry->completed);
    EXPECT(context.responseTelemetry->semanticOutputObserved == false);
    EXPECT(!context.responseTelemetry->usageObserved);
    EXPECT_EQ(admissions, 1);
    const auto stopping = server.shutdown();
    co_await stopping.wait();
  }
}
ASYNC_TEST("selected native HTTP401 failure keeps internal identity and emits only truthful standard failure events") { auto task = selectedFailure(401, "authentication_error"); co_await std::move(task); }
ASYNC_TEST("selected native HTTP429 failure keeps internal identity and emits only truthful standard failure events") { auto task = selectedFailure(429, "rate_limit_exceeded"); co_await std::move(task); }
ASYNC_TEST("selected native HTTP503 failure keeps internal identity and emits only truthful standard failure events") { auto task = selectedFailure(503, "server_error"); co_await std::move(task); }

ASYNC_TEST("native readiness retains one existing quota commitment and the transport's original compensation receipt") {
  fixture::Isolation isolation;
  const double now = rt::dateNow();
  const ::uos::types::ApiKeyHashRecord record{
    .id = "readiness-ledger", .expires_at_ms = -1, .revoked_at_ms = std::nullopt, .usage_limit_requests = 3, .usage_requests = 0,
    .usage_reset_at_ms = now + 3'600'000, .window_ms = 3'600'000, .usage_quota_version = 3, .paid_fallback_enabled = false,
    .paid_fallback_limit_microcredits = 0, .paid_fallback_spent_microcredits = 0, .paid_fallback_reserved_microcredits = 0,
    .paid_fallback_reservation_request_id = std::nullopt};
  const auto policy = api_key_policy::apiKeyPolicyFromHashRecord("readiness-ledger", record, now);
  ASSERT(policy);
  const auto store = std::make_shared<tests::helpers::counting_kv::CountingKv>();
  store->seed(api_keys::apiKeyHashKey("readiness-ledger"), rt::KvValue(json::value_from(record)));
  const api_key_policy::ReserveApiKeyUsageV3Options options{.kv = store, .nowMs = now};
  auto reserving = api_key_policy::reserveApiKeyUsageV3(*policy, "request", "responses", options);
  const auto admission = co_await std::move(reserving);
  const auto* admitted = std::get_if<api_key_policy::ApiKeyUsageReservationDecisionOk>(&admission);
  ASSERT(admitted);
  std::optional<api_key_policy::ApiKeyProviderDispatch> originalReceipt;
  const auto reservation = admitted->reservation;
  const auto context = usage([reservation, &originalReceipt](auto provider) -> rt::Task<std::optional<api_key_policy::ApiKeyProviderDispatch>> {
    const rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>> pending(reservation.beforeProviderDispatch(provider));
    originalReceipt = co_await pending.wait();
    co_return originalReceipt;
  });
  const rt::RequestInit init{.method = "POST", .headers = nativeHeaders()};
  const rt::Request req("http://localhost/v1/responses", init);
  const auto wrapped = native::nativeHeaderUsageContext(req, nativeRaw(), &context);
  auto dispatching = dispatch(wrapped, "lithos");
  const auto receipt = co_await std::move(dispatching);
  ASSERT(receipt && originalReceipt);
  receipt->markTransportStarted();
  auto duplicate = dispatch(wrapped, "deepseek");
  const auto second = co_await std::move(duplicate);
  EXPECT(!second);
  auto releasing = reservation.release(std::nullopt);
  co_await std::move(releasing);
  auto cancelling = receipt->cancelBeforeTransport();
  co_await std::move(cancelling);
  const auto* row = store->entries.get(tests::helpers::counting_kv::encodeKey(api_key_policy::apiKeyUsageV3WindowKey(*policy)));
  ASSERT(row);
  EXPECT_EQ(row->value["committed_requests"].asNumber(), 1.0);
  EXPECT_EQ(row->value["reserved_requests"].asNumber(), 0.0);
}

ASYNC_TEST("failed header callback cannot discard the transport compensation receipt or reencode existing SSE") {
  fixture::Isolation isolation;
  const rt::RequestInit init{.method = "POST", .headers = nativeHeaders()};
  const rt::Request req("http://localhost/v1/responses", init);
  int marked = 0;
  const api_key_policy::ApiKeyProviderDispatch receipt{.markTransportStarted = [&marked] { ++marked; }, .cancelBeforeTransport = [] { return rt::fulfilledTask(); }};
  const auto context = usage([receipt](auto) { return rt::fulfilledTask(std::optional(receipt)); });
  const auto remove = early::registerEarlySseHeaders(req, [](const auto&) -> bool { throw rt::JsError("fixture disconnected at flush"); });
  auto dispatching = dispatch(native::nativeHeaderUsageContext(req, nativeRaw(), &context), "lithos");
  const auto returned = co_await std::move(dispatching);
  ASSERT(returned);
  returned->markTransportStarted();
  EXPECT_EQ(marked, 1);
  EXPECT(!early::earlySseHeadersCommitted(req));
  remove();
  const auto removeReady = early::registerEarlySseHeaders(req, [](const auto&) { return true; });
  const auto cleanup = rt::scopeExit(removeReady);
  auto dispatchAgain = dispatch(native::nativeHeaderUsageContext(req, nativeRaw(), &context), "lithos");
  const auto again = co_await std::move(dispatchAgain);
  ASSERT(again);
  const rt::ResponseInit responseInit{.status = 503, .headers = rt::Headers{{"Content-Type", "text/event-stream"}}};
  const rt::Response source(std::string("event: response.failed\ndata: original\n\n"), responseInit);
  auto encoding = native::nativeFailureAfterEarlyHeaders(req, source, context);
  const auto encoded = co_await std::move(encoding);
  EXPECT(encoded == source);
  const auto text = co_await source.text();
  EXPECT_EQ(text, std::string("event: response.failed\ndata: original\n\n"));
}

ASYNC_TEST("bounded native error evidence reaches the real terminal log after CORS and Response cloning") {
  fixture::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  const rt::Headers headers{{"x-codex-turn-metadata", R"({"session_id":"session","thread_id":"thread","turn_id":"turn"})"}};
  const rt::RequestInit init{.method = "POST", .headers = headers};
  const rt::Request request("http://localhost/v1/responses", init);
  const auto remove = runtime::http_early_sse::registerEarlySseHeaders(request, [](const auto&) { return true; });
  const auto cleanup = rt::scopeExit(remove);
  openai_telemetry::UsageContext context{.keyId = std::nullopt, .kernelRepo = std::nullopt, .kernelOrg = std::nullopt};
  context.requestId = "native-readiness-fixture";
  context.responseTelemetry = openai_telemetry::createResponseTelemetryState();
  const json::object raw{{"model", "deepseek-ai/DeepSeek-V4.1-Flash"}, {"input", "canary"}, {"stream", true}};
  const auto native = responses_native_headers::nativeHeaderUsageContext(request, raw, &context);
  ASSERT(native && native->beforeProviderDispatch);
  auto admission = native->beforeProviderDispatch(std::string("lithos"));
  const auto receipt = co_await std::move(admission);
  EXPECT(!receipt.has_value());
  auto failure = fixture::jsonResponse(R"({"error":{"code":"server_error","message":"original server_error","param":"input","type":"original_type"}})", 503);
  failure.headers().set("retry-after", "17");
  failure.headers().set("set-cookie", "private-cookie");
  failure.headers().set("authorization", "private-auth");
  auto encoding = responses_native_headers::nativeFailureAfterEarlyHeaders(request, failure, context);
  auto encoded = co_await std::move(encoding);
  openai_telemetry::attachResponseTelemetry(encoded, context.responseTelemetry);
  auto cors = http::withCors(encoded, request);
  auto cloned = cors.clone();
  const auto unused = cors.body()->cancelPromise();
  auto options = fixture::terminalOptions("native-error-log-fixture");
  options.request = request;
  options.telemetryResponse = encoded;
  auto wrapped = handler::terminal_log::withTerminalRequestLog(cloned, options);
  auto response = co_await wrapped.wait();
  auto text = co_await response.text();
  EXPECT(rt::includes(text, "response.failed"));
  co_await unused.wait();
  const auto recorded = fixture::terminalPayloads(logs);
  ASSERT(recorded.size() == 1);
  EXPECT_EQ(rt::getNumber(recorded[0], "status"), std::optional<double>(200));
  const auto expected = rt::jsonParse(R"({"status":503,"headers":{"content-type":"application/json","retry-after":"17"},"headers_truncated":false,"error":{"code":"server_error","message":"original server_error","param":"input","type":"original_type"},"body_complete":true})");
  const auto* observed = rt::get(recorded[0], "native_http_failure");
  ASSERT(observed != nullptr);
  EXPECT(rt::deepEqual(*observed, expected));
  const auto serialized = rt::jsonStringify(recorded[0]);
  EXPECT(!rt::includes(serialized, "private-cookie"));
  EXPECT(!rt::includes(serialized, "private-auth"));
}

ASYNC_TEST("oversized selected HTTP error body is cancelled and never presented as complete original evidence") {
  fixture::Isolation isolation;
  const rt::RequestInit init{.method = "POST", .headers = nativeHeaders()};
  const rt::Request req("http://localhost/v1/responses", init);
  const auto remove = early::registerEarlySseHeaders(req, [](const auto&) { return true; });
  const auto cleanup = rt::scopeExit(remove);
  const auto context = usage();
  auto dispatching = dispatch(native::nativeHeaderUsageContext(req, nativeRaw(), &context), "lithos");
  const auto receipt = co_await std::move(dispatching);
  EXPECT(!receipt);
  int cancellations = 0;
  rt::UnderlyingSource<rt::ByteChunk> source;
  source.start = [](auto controller) {
    controller.enqueue(rt::encodeUtf8(rt::jsonStringify(json::object{{"error", json::object{{"code", "not_complete"}, {"message", std::string(65'537, 'x')}}}})));
  };
  source.cancel = [&cancellations](auto) { ++cancellations; };
  const rt::ByteStream body(source);
  const rt::ResponseInit responseInit{.status = 502, .headers = rt::Headers{{"Content-Type", "application/json"}}};
  const rt::Response failure(rt::BodyInit(body), responseInit);
  auto encoding = native::nativeFailureAfterEarlyHeaders(req, failure, context);
  const auto encoded = co_await std::move(encoding);
  EXPECT_EQ(cancellations, 1);
  const auto evidence = native::originalNativeHttpFailure(req);
  ASSERT(evidence);
  EXPECT_EQ(evidence->status, 502.0);
  EXPECT(!evidence->body_complete);
  EXPECT(evidence->error.is_null());
  const auto text = co_await encoded.text();
  EXPECT(!rt::includes(text, "not_complete"));
}
