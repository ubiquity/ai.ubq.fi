// Port of tests/runtime-http-sse-headers.test.ts. node:http's client (`request` with
// `agent: false`, which sends `Connection: close`) is a raw loopback socket here and Bun's
// fetch the loopback transport of tests/helpers/loopback_http.
// Links: src/runtime/http.cpp src/runtime/http_upgrade.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp
//        src/runtime/http_tasks.cpp src/runtime/websocket.cpp tests/helpers/loopback_http.cpp
#include "uos/runtime/http.hpp"

#include "tests/helpers/loopback_http.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <memory>
#include <string>

namespace {
  namespace rt = uos::rt;
  using uos::runtime::http::HttpServer;
  using uos::runtime::http::NetAddr;
  using uos::runtime::http::ServeHandlerInfo;
  using uos::runtime::http::ServeHttpOptions;
  using uos::runtime::http::serveHttp;
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

  // The parts of node:http's IncomingMessage the test reads.
  struct ResponseHead {
    int statusCode = 0;
    std::string statusMessage;
    std::string contentType;
    std::string fixture;
    std::size_t readableLength = 0;
  };

  ResponseHead parseHead(const std::string& wire) {
    ResponseHead head;
    const std::size_t end = wire.find("\r\n\r\n");
    const std::vector<std::string> lines = rt::split(wire.substr(0, end), "\r\n");
    head.statusCode = std::stoi(lines[0].substr(9, 3));
    head.statusMessage = lines[0].substr(13);
    for (std::size_t index = 1; index < lines.size(); ++index) {
      const std::size_t colon = lines[index].find(':');
      const std::string name = rt::toLowerCase(lines[index].substr(0, colon));
      const std::string value = rt::trim(lines[index].substr(colon + 1));
      if (name == "content-type") head.contentType = value;
      if (name == "x-fixture") head.fixture = value;
    }
    head.readableLength = wire.size() - end - 4;
    return head;
  }

  // The chunked body after the head, once its last chunk arrived.
  std::optional<std::string> chunkedBody(const std::string& wire) {
    std::size_t position = wire.find("\r\n\r\n");
    if (position == std::string::npos) return std::nullopt;
    position += 4;
    std::string body;
    for (;;) {
      const std::size_t lineEnd = wire.find("\r\n", position);
      if (lineEnd == std::string::npos) return std::nullopt;
      const std::size_t size = std::stoul(wire.substr(position, lineEnd - position), nullptr, 16);
      if (size == 0) return wire.find("\r\n", lineEnd + 2) == std::string::npos ? std::nullopt : std::optional<std::string>(body);
      if (wire.size() < lineEnd + 2 + size + 2) return std::nullopt;
      body += wire.substr(lineEnd + 2, size);
      position = lineEnd + 2 + size + 2;
    }
  }
}

ASYNC_TEST("HTTP sends SSE headers before the client releases the first body chunk") {
  auto release = rt::withResolvers<void>();
  auto pulling = rt::withResolvers<void>();
  struct Receipt {
    rt::Promise<void> completed;
  };
  auto receipt = rt::withResolvers<Receipt>();
  auto produced = std::make_shared<bool>(false);
  auto delivered = std::make_shared<bool>(false);
  const rt::Promise<void> releasePromise = release.promise;
  const rt::Resolve<void> resolvePulling = pulling.resolve;
  const rt::Resolve<Receipt> resolveReceipt = receipt.resolve;
  const HttpServer server = serveHttp(loopbackOptions(), [=](rt::Request, ServeHandlerInfo info) -> rt::Task<rt::Response> {
    resolveReceipt(Receipt{info.completed});
    (void)info.completed.then([delivered] { *delivered = true; }, [](rt::Reason) {});
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.pull = [=](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) -> rt::Task<void> {
      resolvePulling();
      co_await releasePromise.wait();
      *produced = true;
      controller.enqueue(rt::encodeUtf8("data: released\n\n"));
      controller.close();
    };
    rt::ResponseInit init;
    init.status = 202;
    init.statusText = "Accepted";
    init.headers = rt::Headers{{"content-type", "text/event-stream; charset=utf-8"}, {"x-fixture", "delayed-body"}};
    co_return rt::Response(rt::BodyInit(rt::ByteStream(std::move(source))), std::move(init));
  });
  const NetAddr address = co_await server.ready.wait();
  // This only bounds a deadlock: the producer is released by observed headers,
  // never by a timer or a race against elapsed time.
  const std::shared_ptr<RawClient> client = RawClient::connect(static_cast<int>(address.port));
  auto headers = rt::withResolvers<std::string>();
  auto body = rt::withResolvers<std::string>();
  const rt::Resolve<std::string> resolveHeaders = headers.resolve;
  const rt::Reject rejectHeaders = headers.reject;
  const rt::Resolve<std::string> resolveBody = body.resolve;
  auto wire = std::make_shared<std::string>();
  client->once("connect", [client](const std::string&) { client->write("GET /delayed HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"); });
  client->on("data", [wire, resolveHeaders, resolveBody](const std::string& chunk) {
    *wire += chunk;
    if (wire->find("\r\n\r\n") != std::string::npos) resolveHeaders(*wire);
    if (const std::optional<std::string> text = chunkedBody(*wire)) resolveBody(*text);
  });
  client->once("error", [rejectHeaders](const std::string& error) { rejectHeaders(rt::makeReason(rt::JsError(error))); });
  const rt::Timeout watchdog = rt::setTimeout([client, rejectHeaders] {
    rejectHeaders(rt::makeReason(rt::JsError("SSE headers were withheld until the first body chunk")));
    client->destroy();
  }, 1000);
  const std::string headWire = co_await headers.promise.wait();
  rt::clearTimeout(watchdog);
  co_await pulling.promise.wait();
  const ResponseHead response = parseHead(headWire);
  EXPECT_EQ(response.statusCode, 202);
  EXPECT_EQ(response.statusMessage, "Accepted");
  EXPECT_EQ(response.contentType, "text/event-stream; charset=utf-8");
  EXPECT_EQ(response.fixture, "delayed-body");
  EXPECT_EQ(response.readableLength, 0u);
  EXPECT_EQ(*produced, false);
  EXPECT_EQ(*delivered, false);
  release.resolve();
  const std::string text = co_await body.promise.wait();
  EXPECT_EQ(text, "data: released\n\n");
  const Receipt delivery = co_await receipt.promise.wait();
  co_await delivery.completed.wait();
  EXPECT_EQ(*delivered, true);
  client->destroy();
  auto stopping = shutdown(server);
  co_await std::move(stopping);
}

ASYNC_TEST("HTTP preserves a non-SSE failure status before any body is sent") {
  const ScopedLoopbackFetch loopback;
  const HttpServer server = serveHttp(loopbackOptions(), [](rt::Request, ServeHandlerInfo) -> rt::Task<rt::Response> {
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.pull = [](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) { controller.error(rt::makeReason(rt::JsError("fixture body failed"))); };
    rt::ResponseInit init;
    init.status = 202;
    init.headers = rt::Headers{{"content-type", "application/json"}};
    co_return rt::Response(rt::BodyInit(rt::ByteStream(std::move(source))), std::move(init));
  });
  const NetAddr address = co_await server.ready.wait();
  auto fetching = rt::fetch("http://127.0.0.1:" + rt::numberToString(address.port));
  const rt::Response response = co_await std::move(fetching);
  EXPECT_EQ(response.status(), 500);
  auto reading = response.text();
  const std::string text = co_await std::move(reading);
  EXPECT_EQ(text, "Internal Server Error");
  auto stopping = shutdown(server);
  co_await std::move(stopping);
}
