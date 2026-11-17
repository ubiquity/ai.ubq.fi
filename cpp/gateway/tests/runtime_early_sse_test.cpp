// Port of tests/runtime-early-sse.test.ts. Bun's fetch is the loopback transport of
// tests/helpers/loopback_http and node:net a raw loopback socket.
// Links: src/runtime/http.cpp src/runtime/http_upgrade.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp
//        src/runtime/http_tasks.cpp src/runtime/websocket.cpp tests/helpers/loopback_http.cpp
#include "uos/runtime/http.hpp"

#include "tests/helpers/loopback_http.hpp"
#include "uos/runtime/http_early_sse.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  using uos::runtime::http::HttpServer;
  using uos::runtime::http::NetAddr;
  using uos::runtime::http::ServeHandlerInfo;
  using uos::runtime::http::ServeHttpOptions;
  using uos::runtime::http::serveHttp;
  using uos::runtime::http_early_sse::earlySseHeadersCommitted;
  using uos::runtime::http_early_sse::EarlySseHeaders;
  using uos::runtime::http_early_sse::tryCommitEarlySseHeaders;
  using uos::tests::helpers::loopback_http::RawClient;
  using uos::tests::helpers::loopback_http::ScopedLoopbackFetch;

  ServeHttpOptions loopbackOptions() {
    ServeHttpOptions options;
    options.hostname = "127.0.0.1";
    options.port = 0;
    return options;
  }

  rt::Task<void> shutdown(HttpServer server) {
    const rt::Promise<void> stopping = server.shutdown();
    co_await stopping.wait();
  }

  rt::Headers headers() { return rt::Headers{{"content-type", "text/event-stream"}, {"cache-control", "no-store"}, {"x-uos-request-id", "readiness-fixture"}}; }

  rt::RequestInit postBody() {
    rt::RequestInit init;
    init.method = "POST";
    init.body = rt::BodyInit(std::string("{}"));
    return init;
  }

  rt::Task<std::string> textOf(rt::Request request) {
    auto reading = request.text();
    std::string text = co_await std::move(reading);
    co_return text;
  }

  std::string decode(const rt::ReadableStreamReadResult<rt::ByteChunk>& step) { return step.value ? rt::decodeUtf8(*step.value) : std::string(); }
}

ASYNC_TEST("admitted SSE headers arrive while the original handler remains held without body bytes") {
  const ScopedLoopbackFetch loopback;
  auto release = rt::withResolvers<void>();
  auto selected = std::make_shared<bool>(false);
  auto completed = std::make_shared<bool>(false);
  const rt::Promise<void> releasePromise = release.promise;
  const HttpServer server = serveHttp(loopbackOptions(), [=](rt::Request request, ServeHandlerInfo info) -> rt::Task<rt::Response> {
    auto reading = textOf(request);
    co_await std::move(reading);
    (void)info.completed.then([completed] { *completed = true; }, [](rt::Reason) {});
    tryCommitEarlySseHeaders(request, EarlySseHeaders{headers()});
    co_await releasePromise.wait();
    *selected = true;
    rt::ResponseInit init;
    init.headers = headers();
    co_return rt::Response(rt::BodyInit(std::string("data: selected\n\n")), std::move(init));
  });
  const NetAddr address = co_await server.ready.wait();
  const rt::Promise<rt::Response> fetching(rt::fetch("http://127.0.0.1:" + rt::numberToString(address.port) + "/v1/responses", postBody()));
  const rt::Promise<rt::Response> timeout(rt::async([]() -> rt::Task<rt::Response> {
    co_await rt::sleep(400);
    throw rt::JsError("headers waited for the original handler");
  }));
  std::vector<rt::Promise<rt::Response>> racers{fetching, timeout};
  const rt::Response response = co_await rt::race(std::move(racers)).wait();
  EXPECT_EQ(response.status(), 200);
  EXPECT_EQ(response.headers().get("x-uos-request-id"), std::optional<std::string>("readiness-fixture"));
  EXPECT_EQ(*selected, false);
  EXPECT_EQ(*completed, false);
  const std::optional<rt::ByteStream> body = response.body();
  ASSERT(body.has_value());
  const rt::ReadableStreamDefaultReader<rt::ByteChunk> reader = body->getReader();
  auto bodyArrived = std::make_shared<bool>(false);
  const rt::Promise<rt::ReadableStreamReadResult<rt::ByteChunk>> first = rt::Promise<rt::ReadableStreamReadResult<rt::ByteChunk>>(reader.read()).then([bodyArrived](const rt::ReadableStreamReadResult<rt::ByteChunk>& value) {
    *bodyArrived = true;
    return value;
  });
  co_await rt::sleep(20);
  EXPECT_EQ(*bodyArrived, false);
  release.resolve();
  const rt::ReadableStreamReadResult<rt::ByteChunk> firstValue = co_await first.wait();
  EXPECT_EQ(decode(firstValue), "data: selected\n\n");
  const rt::ReadableStreamReadResult<rt::ByteChunk> last = co_await reader.read();
  EXPECT_EQ(last.done, true);
  auto stopping = shutdown(server);
  co_await std::move(stopping);
}

ASYNC_TEST("early SSE refuses unread bodies and framing conflicts, commits once and removes its physical registration") {
  const ScopedLoopbackFetch loopback;
  auto observed = std::make_shared<std::optional<rt::Request>>();
  const HttpServer server = serveHttp(loopbackOptions(), [observed](rt::Request request, ServeHandlerInfo) -> rt::Task<rt::Response> {
    *observed = request;
    EXPECT_EQ(tryCommitEarlySseHeaders(request, EarlySseHeaders{headers()}), false);
    auto reading = textOf(request);
    co_await std::move(reading);
    std::vector<rt::Headers> invalid{rt::Headers{{"content-type", "application/json"}}};
    for (const char* name : {"content-length", "content-encoding", "transfer-encoding"}) invalid.push_back(rt::Headers{{"content-type", "text/event-stream"}, {name, "0"}});
    for (const rt::Headers& candidate : invalid) EXPECT_EQ(tryCommitEarlySseHeaders(request, EarlySseHeaders{candidate}), false);
    EXPECT_EQ(tryCommitEarlySseHeaders(request, EarlySseHeaders{headers()}), true);
    EXPECT_EQ(tryCommitEarlySseHeaders(request, EarlySseHeaders{headers()}), true);
    EXPECT_EQ(earlySseHeadersCommitted(request), true);
    rt::ResponseInit init;
    init.headers = headers();
    co_return rt::Response(rt::BodyInit(std::string("data: done\n\n")), std::move(init));
  });
  const NetAddr address = co_await server.ready.wait();
  auto fetching = rt::fetch("http://127.0.0.1:" + rt::numberToString(address.port), postBody());
  const rt::Response response = co_await std::move(fetching);
  auto reading = response.text();
  const std::string text = co_await std::move(reading);
  EXPECT_EQ(text, "data: done\n\n");
  auto stopping = shutdown(server);
  co_await std::move(stopping);
  ASSERT(observed->has_value());
  EXPECT_EQ(earlySseHeadersCommitted(**observed), false);
  EXPECT_EQ(tryCommitEarlySseHeaders(**observed, EarlySseHeaders{headers()}), false);
}

ASYNC_TEST("pending selection sends no bytes and forwards the exact fragmented selected API event") {
  const ScopedLoopbackFetch loopback;
  auto select = rt::withResolvers<void>();
  auto finish = rt::withResolvers<void>();
  const rt::Promise<void> selectPromise = select.promise;
  const rt::Promise<void> finishPromise = finish.promise;
  const HttpServer server = serveHttp(loopbackOptions(), [=](rt::Request request, ServeHandlerInfo) -> rt::Task<rt::Response> {
    auto reading = textOf(request);
    co_await std::move(reading);
    tryCommitEarlySseHeaders(request, EarlySseHeaders{headers()});
    co_await selectPromise.wait();
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.start = [finishPromise](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) -> rt::Task<void> {
      controller.enqueue(rt::encodeUtf8("data: {\"text\":\"first"));
      co_await finishPromise.wait();
      controller.enqueue(rt::encodeUtf8("last\"}\n\n"));
      controller.close();
    };
    rt::ResponseInit init;
    init.headers = headers();
    co_return rt::Response(rt::BodyInit(rt::ByteStream(std::move(source))), std::move(init));
  });
  const NetAddr address = co_await server.ready.wait();
  auto fetching = rt::fetch("http://127.0.0.1:" + rt::numberToString(address.port), postBody());
  const rt::Response response = co_await std::move(fetching);
  const std::optional<rt::ByteStream> body = response.body();
  ASSERT(body.has_value());
  const rt::ReadableStreamDefaultReader<rt::ByteChunk> reader = body->getReader();
  auto preselectionBytes = std::make_shared<bool>(false);
  const rt::Promise<rt::ReadableStreamReadResult<rt::ByteChunk>> first = rt::Promise<rt::ReadableStreamReadResult<rt::ByteChunk>>(reader.read()).then([preselectionBytes](const rt::ReadableStreamReadResult<rt::ByteChunk>& value) {
    *preselectionBytes = true;
    return value;
  });
  co_await rt::sleep(120);
  EXPECT_EQ(*preselectionBytes, false);  // pending selection must remain silent
  select.resolve();
  const rt::ReadableStreamReadResult<rt::ByteChunk> firstValue = co_await first.wait();
  EXPECT_EQ(decode(firstValue), "data: {\"text\":\"first");
  auto inserted = std::make_shared<bool>(false);
  const rt::Promise<rt::ReadableStreamReadResult<rt::ByteChunk>> next = rt::Promise<rt::ReadableStreamReadResult<rt::ByteChunk>>(reader.read()).then([inserted](const rt::ReadableStreamReadResult<rt::ByteChunk>& value) {
    *inserted = true;
    return value;
  });
  co_await rt::sleep(120);
  EXPECT_EQ(*inserted, false);  // a pending selected body must not receive transport bytes
  finish.resolve();
  const rt::ReadableStreamReadResult<rt::ByteChunk> nextValue = co_await next.wait();
  EXPECT_EQ(decode(nextValue), "last\"}\n\n");
  const rt::ReadableStreamReadResult<rt::ByteChunk> last = co_await reader.read();
  EXPECT_EQ(last.done, true);
  auto stopping = shutdown(server);
  co_await std::move(stopping);
}

ASYNC_TEST("completed POST input stays live until a real disconnect aborts the held response without body bytes") {
  auto release = rt::withResolvers<void>();
  auto headersArrived = rt::withResolvers<void>();
  auto aborted = rt::withResolvers<void>();
  const std::string posted = rt::repeat("x", 52 * 1024);
  auto physicalRequest = std::make_shared<std::optional<rt::Request>>();
  auto delivery = std::make_shared<std::optional<rt::Promise<void>>>();
  const rt::Promise<void> releasePromise = release.promise;
  const rt::Resolve<void> resolveAborted = aborted.resolve;
  const HttpServer server = serveHttp(loopbackOptions(), [=](rt::Request request, ServeHandlerInfo info) -> rt::Task<rt::Response> {
    auto reading = textOf(request);
    const std::string text = co_await std::move(reading);
    EXPECT(text == posted);
    *physicalRequest = request;
    *delivery = info.completed;
    request.signal().addEventListener("abort", rt::EventListener([resolveAborted] { resolveAborted(); }), {.once = true});
    EXPECT_EQ(tryCommitEarlySseHeaders(request, EarlySseHeaders{headers()}), true);
    co_await releasePromise.wait();
    rt::ResponseInit init;
    init.headers = headers();
    co_return rt::Response(rt::BodyInit(std::string("data: cleanup\n\n")), std::move(init));
  });
  const NetAddr address = co_await server.ready.wait();
  const std::shared_ptr<RawClient> socket = RawClient::connect(static_cast<int>(address.port));
  auto wire = std::make_shared<std::string>();
  const rt::Resolve<void> resolveHeaders = headersArrived.resolve;
  socket->on("error", [](const std::string&) {});
  socket->once("connect", [socket, posted](const std::string&) {
    socket->write("POST /v1/responses HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(posted.size()) + "\r\n\r\n" + posted);
  });
  socket->on("data", [wire, resolveHeaders](const std::string& chunk) {
    *wire += chunk;
    if (wire->find("\r\n\r\n") != std::string::npos) resolveHeaders();
  });
  const rt::Promise<void> headerDeadline(rt::async([]() -> rt::Task<void> {
    co_await rt::sleep(1000);
    throw rt::JsError("admitted headers did not arrive");
  }));
  std::vector<rt::Promise<void>> headerRacers{headersArrived.promise, headerDeadline};
  co_await rt::race(std::move(headerRacers)).wait();
  ASSERT(physicalRequest->has_value());
  ASSERT(delivery->has_value());
  EXPECT_EQ((*physicalRequest)->signal().aborted(), false);  // input EOF must not abort the response
  co_await rt::sleep(120);
  EXPECT_EQ((*physicalRequest)->signal().aborted(), false);  // a quiet open response must stay live after input EOF
  EXPECT_EQ(wire->substr(wire->find("\r\n\r\n") + 4), "");  // no response body bytes may expose abandonment
  socket->destroy();
  const rt::Promise<void> abortDeadline(rt::async([]() -> rt::Task<void> {
    co_await rt::sleep(500);
    throw rt::JsError("completed-input disconnect did not abort the original request");
  }));
  std::vector<rt::Promise<void>> abortRacers{aborted.promise, abortDeadline};
  co_await rt::race(std::move(abortRacers)).wait();
  rt::Reason failure;
  try {
    co_await (*delivery)->wait();
  } catch (...) {
    failure = std::current_exception();
  }
  ASSERT(failure != nullptr);
  EXPECT(rt::Regex("disconnected", "").test(rt::errorMessage(failure).value_or("")));
  release.resolve();
  auto stopping = shutdown(server);
  co_await std::move(stopping);
}
