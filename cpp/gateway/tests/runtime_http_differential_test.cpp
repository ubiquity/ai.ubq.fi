// Differential cases for the node:http server src/runtime/http.ts runs on (declared in
// include/uos/runtime/http.hpp, namespace node_http) and for the wire behaviour of serveHttp
// itself, with src/runtime/http-upgrade.ts's refusals and 101 responses. The vectors were recorded
// from Bun 1.4.0: the same raw requests sent to src/runtime/http.ts's serveHttp with the handler
// below ("serve"), to a serveHttp with the upgrade handler below ("upgrade") and to a node:http
// createServer listener with the same listener as below ("node"); every received byte within
// 400 ms (Latin-1), then <CLOSED> or <TIMEOUT>, Date values written as <DATE>. The status
// reason table, listen errors and address() were recorded the same way.
// Links: src/runtime/http.cpp src/runtime/http_upgrade.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp
//        src/runtime/http_tasks.cpp src/runtime/websocket.cpp tests/helpers/loopback_http.cpp
#include "uos/runtime/http.hpp"

#include "tests/helpers/loopback_http.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/url.hpp"
#include "uos/rt/websocket.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace ws = uos::rt::ws;
  namespace node_http = uos::runtime::http::node_http;
  using uos::runtime::http::HttpServer;
  using uos::runtime::http::NetAddr;
  using uos::runtime::http::ServeHandlerInfo;
  using uos::runtime::http::ServeHttpOptions;
  using uos::runtime::http::serveHttp;
  using uos::tests::helpers::loopback_http::RawClient;

#include "tests/runtime_http_differential_vectors.inc"

  rt::ByteChunk bytes(std::string_view text) { return rt::encodeUtf8(text); }

  // The serveHttp handler of the recording.
  rt::Task<rt::Response> serveHandler(rt::Request request, ServeHandlerInfo) {
    const std::string path = rt::URL(request.url()).pathname();
    if (path == "/text") co_return rt::Response(rt::BodyInit(std::string("hello")));
    if (path == "/json") {
      rt::ResponseInit init;
      init.status = 201;
      init.statusText = "Made";
      co_return rt::Response::json(json::object{{"a", 1}}, std::move(init));
    }
    if (path == "/headers") {
      rt::Headers headers{{"X-Upper", "Value"}, {"content-type", "text/plain"}};
      headers.append("set-cookie", "a=1");
      headers.append("set-cookie", "b=2");
      rt::ResponseInit init;
      init.headers = headers;
      co_return rt::Response(rt::BodyInit(std::string("body")), std::move(init));
    }
    if (path == "/stream") {
      rt::UnderlyingSource<rt::ByteChunk> source;
      source.start = [](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
        controller.enqueue(bytes("one"));
        controller.enqueue(bytes("two"));
        controller.close();
      };
      co_return rt::Response(rt::BodyInit(rt::ByteStream(std::move(source))));
    }
    if (path == "/empty") {
      rt::ResponseInit init;
      init.status = 204;
      co_return rt::Response(nullptr, std::move(init));
    }
    if (path == "/null") co_return rt::Response(nullptr);
    if (path == "/emptystring") co_return rt::Response(rt::BodyInit(std::string()));
    if (path == "/304" || path == "/205") {
      rt::ResponseInit init;
      init.status = path == "/304" ? 304 : 205;
      co_return rt::Response(rt::BodyInit(std::string("x")), std::move(init));
    }
    if (path == "/throw" || path == "/admin/throw") throw rt::JsError("boom");
    if (path == "/echo") {
      auto reading = request.text();
      const std::string text = co_await std::move(reading);
      co_return rt::Response(rt::BodyInit(text));
    }
    if (path == "/ignore") co_return rt::Response(rt::BodyInit(std::string("ignored body")));
    if (path == "/req") {
      json::array headers;
      for (const auto& [name, value] : request.headers().entries()) headers.push_back(json::array{json::string(name), json::string(value)});
      json::object out;
      out["method"] = request.method();
      out["url"] = request.url();
      out["headers"] = headers;
      co_return rt::Response::json(out);
    }
    if (path == "/sse") {
      rt::UnderlyingSource<rt::ByteChunk> source;
      source.start = [](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
        controller.enqueue(bytes("data: x\n\n"));
        controller.close();
      };
      rt::ResponseInit init;
      init.headers = rt::Headers{{"content-type", "text/event-stream"}};
      co_return rt::Response(rt::BodyInit(rt::ByteStream(std::move(source))), std::move(init));
    }
    if (path == "/cl") {
      rt::ResponseInit init;
      init.headers = rt::Headers{{"content-length", "3"}};
      co_return rt::Response(rt::BodyInit(std::string("abc")), std::move(init));
    }
    if (path == "/latin") {
      rt::ResponseInit init;
      init.headers = rt::Headers{{"x-latin", "caf\xc3\xa9"}};
      co_return rt::Response(rt::BodyInit(std::string("l")), std::move(init));
    }
    if (path == "/status") {
      rt::ResponseInit init;
      init.status = 418;
      co_return rt::Response(rt::BodyInit(std::string("s")), std::move(init));
    }
    co_return rt::Response(rt::BodyInit("other " + path));
  }

  using uos::runtime::http::WebSocketUpgrade;
  using uos::runtime::http_upgrade::UpgradeInfo;
  using uos::runtime::http_upgrade::UpgradeResult;

  // The upgrade handler of the recording's "upgrade" server.
  rt::Task<UpgradeResult> upgradeHandler(rt::Request request, UpgradeInfo) {
    const std::string path = rt::URL(request.url()).pathname();
    if (path == "/refuse") {
      rt::Headers headers{{"x-reason", "fixture"}, {"content-type", "text/plain"}};
      headers.append("set-cookie", "a=1");
      headers.append("set-cookie", "b=2");
      rt::ResponseInit init;
      init.status = 403;
      init.statusText = "Nope";
      init.headers = headers;
      co_return UpgradeResult(rt::Response(rt::BodyInit(std::string("refused body")), std::move(init)));
    }
    if (path == "/refuse-plain") {
      rt::ResponseInit init;
      init.status = 401;
      co_return UpgradeResult(rt::Response(nullptr, std::move(init)));
    }
    if (path == "/throw") throw rt::JsError("handler failed");
    if (path == "/async-refuse") {
      co_await rt::microtask();
      rt::ResponseInit init;
      init.status = 429;
      co_return UpgradeResult(rt::Response(rt::BodyInit(std::string("later")), std::move(init)));
    }
    WebSocketUpgrade upgrade;
    upgrade.maxPayloadBytes = 64;
    if (path == "/headers") {
      rt::Headers headers{{"x-extra", "1"}, {"X-Second", "two"}};
      headers.append("set-cookie", "s=1");
      upgrade.headers = headers;
    }
    upgrade.accept = [](std::shared_ptr<ws::WebSocket> socket) { socket->send(std::string("hello from server")); };
    co_return UpgradeResult(upgrade);
  }

  using node_http::IncomingEvent;
  using node_http::IncomingListener;
  using node_http::IncomingMessage;
  using node_http::ServerResponse;

  // The node:http listener of the recording.
  void nodeListener(std::shared_ptr<IncomingMessage> req, std::shared_ptr<ServerResponse> res) {
    const std::string& u = req->url;
    if (u == "/205") {
      res->statusCode = 205;
      res->end();
      return;
    }
    if (u == "/te") {
      res->setHeader("transfer-encoding", "chunked");
      res->write(std::string_view("ab"));
      res->end();
      return;
    }
    if (u == "/conn-ka") {
      res->setHeader("connection", "keep-alive");
      res->end("x");
      return;
    }
    if (u == "/conn-close") {
      res->setHeader("connection", "close");
      res->shouldKeepAlive = false;
      res->end("x");
      return;
    }
    if (u == "/conn-close-only-header") {
      res->setHeader("connection", "close");
      res->end("x");
      return;
    }
    if (u == "/ska-false") {
      res->shouldKeepAlive = false;
      res->end("x");
      return;
    }
    if (u == "/ska-false-write") {
      res->shouldKeepAlive = false;
      res->write(std::string_view("x"));
      res->end();
      return;
    }
    if (u == "/date") {
      res->setHeader("date", "custom");
      res->end();
      return;
    }
    if (u == "/keepalive-hdr") {
      res->setHeader("keep-alive", "timeout=9");
      res->end();
      return;
    }
    if (u == "/head-write") {
      res->write(std::string_view("abc"));
      res->end();
      return;
    }
    if (u == "/flush") {
      res->setHeader("content-type", "text/event-stream");
      res->flushHeaders();
      rt::setTimeout([res] { res->end(); }, 100);
      return;
    }
    if (u == "/flush-then-cl") {
      res->setHeader("content-length", "2");
      res->flushHeaders();
      res->write(std::string_view("ok"));
      res->end();
      return;
    }
    if (u == "/flush-close") {
      res->setHeader("connection", "close");
      res->shouldKeepAlive = false;
      res->flushHeaders();
      rt::setTimeout([res] { res->end(); }, 50);
      return;
    }
    if (u == "/statusmsg") {
      res->statusCode = 404;
      res->statusMessage = "Nope Here";
      res->end();
      return;
    }
    if (u == "/setcase") {
      res->setHeader("X-Case", "1");
      res->setHeader("x-case", "2");
      res->setHeader("A", "a");
      res->end();
      return;
    }
    if (u == "/empty-write") {
      res->write(std::string_view(""));
      res->end();
      return;
    }
    if (u == "/write-uint8") {
      const rt::Bytes first{0x41, 0x42};
      const rt::Bytes last{0x43};
      res->write(std::span<const std::uint8_t>(first));
      res->end(std::span<const std::uint8_t>(last));
      return;
    }
    if (u == "/destroy") {
      res->destroy();
      return;
    }
    if (u == "/destroy-after-flush") {
      res->flushHeaders();
      rt::setTimeout([res] { res->destroy(); }, 30);
      return;
    }
    if (u == "/500reset") {
      res->statusCode = 202;
      res->setHeader("content-type", "application/json");
      res->setHeader("x-keep", "1");
      res->statusCode = 500;
      res->setHeader("content-type", "text/plain; charset=utf-8");
      res->end("Internal Server Error");
      return;
    }
    if (u == "/cl-mismatch") {
      res->setHeader("content-length", "10");
      res->end("abc");
      return;
    }
    if (u == "/arr") {
      res->setHeader("set-cookie", std::vector<std::string>{"a=1", "b=2"});
      res->setHeader("X-Mixed", "v");
      res->end();
      return;
    }
    if (u == "/endstr") {
      res->end("abc");
      return;
    }
    if (u == "/status299") {
      res->statusCode = 299;
      res->end("x");
      return;
    }
    if (u == "/noread") {
      res->end("n");
      return;
    }
    if (u == "/info") {
      json::array rawHeaders;
      for (const std::string& entry : req->rawHeaders) rawHeaders.push_back(json::string(entry));
      json::object out;
      out["method"] = req->method;
      out["url"] = req->url;
      out["httpVersion"] = req->httpVersion;
      out["rawHeaders"] = rawHeaders;
      res->end(rt::jsonStringify(json::value(out)));
      return;
    }
    auto body = std::make_shared<std::string>();
    req->on("data", IncomingListener([body](const IncomingEvent& event) {
      const rt::ByteChunk& chunk = std::get<rt::ByteChunk>(event);
      *body += rt::decodeUtf8(chunk);
    }));
    req->on("end", IncomingListener([res, body](const IncomingEvent&) { res->end("got:" + *body); }));
    // `req.on("data", ...)` switches a node:http request into flowing mode.
    req->resume();
  }

  // Code points up to U+00FF as bytes (how the recording wrote each part).
  std::string latin1Bytes(std::string_view text) {
    std::string out;
    for (const std::uint32_t codePoint : rt::codePoints(text)) out.push_back(static_cast<char>(codePoint & 0xFF));
    return out;
  }

  std::string normalizeDates(const std::string& wire) {
    static const rt::Regex DATE("Date: [^\\r]*\\r\\n", "g");
    return rt::replaceAll(wire, DATE, "Date: <DATE>\r\n");
  }

  rt::Task<std::string> exchange(int port, std::vector<std::string> parts) {
    const std::shared_ptr<RawClient> client = RawClient::connect(port);
    auto wire = std::make_shared<std::string>();
    auto closed = rt::withResolvers<void>();
    const rt::Resolve<void> resolveClosed = closed.resolve;
    client->on("data", [wire](const std::string& chunk) { *wire += chunk; });
    client->on("error", [](const std::string&) {});
    client->once("close", [resolveClosed](const std::string&) { resolveClosed(); });
    auto connected = rt::withResolvers<void>();
    const rt::Resolve<void> resolveConnected = connected.resolve;
    client->once("connect", [resolveConnected](const std::string&) { resolveConnected(); });
    co_await connected.promise.wait();
    for (const std::string& part : parts) {
      client->write(latin1Bytes(part));
      co_await rt::sleep(50);
    }
    const double deadline = 400 - 50 * static_cast<double>(parts.size());
    const rt::Promise<int> timeout(rt::async([deadline]() -> rt::Task<int> {
      co_await rt::sleep(deadline);
      co_return 0;
    }));
    const rt::Promise<int> closing = closed.promise.then([] { return 1; });
    std::vector<rt::Promise<int>> racers{timeout, closing};
    const int outcome = co_await rt::race(std::move(racers)).wait();
    const std::string suffix = outcome == 1 ? "<CLOSED>" : "<TIMEOUT>";
    const std::string received = *wire;
    client->destroy();
    co_return normalizeDates(received) + suffix;
  }
}

ASYNC_TEST("differential: serveHttp and node:http wire bytes") {
  ServeHttpOptions options;
  options.hostname = "127.0.0.1";
  options.port = 0;
  const HttpServer serve = serveHttp(options, serveHandler);
  const NetAddr serveAddress = co_await serve.ready.wait();
  ServeHttpOptions upgradeOptions = options;
  upgradeOptions.upgrade = upgradeHandler;
  const HttpServer upgrading = serveHttp(upgradeOptions, [](rt::Request, ServeHandlerInfo) -> rt::Task<rt::Response> { co_return rt::Response(rt::BodyInit(std::string("plain"))); });
  const NetAddr upgradeAddress = co_await upgrading.ready.wait();
  const std::shared_ptr<node_http::Server> node = node_http::createServer(nodeListener);
  node->timeout = 0;
  node->requestTimeout = 0;
  node->on("upgrade", [](const node_http::ServerEventArgs& args) {
    json::object out;
    out["method"] = args.incoming->method;
    out["url"] = args.incoming->url;
    out["head"] = rt::binaryString(args.head);
    args.socket->end(rt::encodeUtf8("HTTP/1.1 418 Teapot\r\n\r\n" + rt::jsonStringify(json::value(out))));
  });
  auto listening = rt::withResolvers<void>();
  const rt::Resolve<void> resolveListening = listening.resolve;
  node->listen(0, "127.0.0.1", [resolveListening] { resolveListening(); });
  co_await listening.promise.wait();
  const int nodePort = static_cast<int>(node->address()->port);
  const json::array vectors = rt::jsonParse(WIRE_VECTORS).as_array();
  int compared = 0;
  for (const json::value& vector : vectors) {
    const std::string server(vector.at("server").as_string());
    const std::string name(vector.at("name").as_string());
    std::vector<std::string> parts;
    for (const json::value& part : vector.at("parts").as_array()) parts.emplace_back(part.as_string());
    const int port = server == "serve" ? static_cast<int>(serveAddress.port) : server == "upgrade" ? static_cast<int>(upgradeAddress.port) : nodePort;
    auto exchanging = exchange(port, parts);
    const std::string actual = co_await std::move(exchanging);
    // Both sides hold each received byte as the code point U+00xx.
    const std::string expected(vector.at("wire").as_string());
    if (actual != expected) std::fprintf(stderr, "      case: %s %s\n", server.c_str(), name.c_str());
    EXPECT_EQ(actual, expected);
    compared += 1;
  }
  EXPECT_EQ(compared, 107);
  node->close();
  auto stopping = serve.shutdown();
  co_await stopping.wait();
  auto stoppingUpgrades = upgrading.shutdown();
  co_await stoppingUpgrades.wait();
}

TEST("differential: status reason phrases match Bun's status lines") {
  // `HTTP/1.1 <code> <reason>` as Bun wrote them for res.statusCode = code.
  const std::vector<std::pair<int, std::string>> expected{
    {200, "OK"}, {201, "Created"}, {202, "Accepted"}, {203, "Non-Authoritative Information"}, {204, "No Content"}, {205, "Reset Content"},
    {206, "Partial Content"}, {207, "Multi-Status"}, {208, "Already Reported"}, {226, "IM Used"}, {300, "Multiple Choices"}, {301, "Moved Permanently"},
    {302, "Found"}, {303, "See Other"}, {304, "Not Modified"}, {305, "Use Proxy"}, {306, "Switch Proxy"}, {307, "Temporary Redirect"},
    {308, "Permanent Redirect"}, {400, "Bad Request"}, {401, "Unauthorized"}, {402, "Payment Required"}, {403, "Forbidden"}, {404, "Not Found"},
    {405, "Method Not Allowed"}, {406, "Not Acceptable"}, {407, "Proxy Authentication Required"}, {408, "Request Timeout"}, {409, "Conflict"},
    {410, "Gone"}, {411, "Length Required"}, {412, "Precondition Failed"}, {413, "Payload Too Large"}, {414, "URI Too Long"},
    {415, "Unsupported Media Type"}, {416, "Range Not Satisfiable"}, {417, "Expectation Failed"}, {418, "I'm a Teapot"}, {421, "Misdirected Request"},
    {422, "Unprocessable Entity"}, {423, "Locked"}, {424, "Failed Dependency"}, {425, "Too Early"}, {426, "Upgrade Required"},
    {428, "Precondition Required"}, {429, "Too Many Requests"}, {431, "Request Header Fields Too Large"}, {451, "Unavailable For Legal Reasons"},
    {500, "Internal Server Error"}, {501, "Not Implemented"}, {502, "Bad Gateway"}, {503, "Service Unavailable"}, {504, "Gateway Timeout"},
    {505, "HTTP Version Not Supported"}, {506, "Variant Also Negotiates"}, {507, "Insufficient Storage"}, {508, "Loop Detected"}, {509, "HM"},
    {510, "Not Extended"}, {511, "Network Authentication Required"}, {299, "HM"}, {420, "HM"}, {599, "HM"}, {230, "HM"},
  };
  for (const auto& [code, reason] : expected) {
    if (node_http::reasonPhrase(code) != reason) std::fprintf(stderr, "      code: %d\n", code);
    EXPECT_EQ(std::string(node_http::reasonPhrase(code)), reason);
  }
}

namespace {
  // The message and code of the "error" event a listen attempt emits.
  rt::Task<std::string> listenError(std::shared_ptr<node_http::Server> server, double port, std::string hostname) {
    auto failed = rt::withResolvers<std::string>();
    const rt::Resolve<std::string> resolveFailed = failed.resolve;
    server->once("error", [resolveFailed](const node_http::ServerEventArgs& args) {
      std::string text = rt::errorName(args.error).value_or("") + ": " + rt::errorMessage(args.error).value_or("");
      try {
        std::rethrow_exception(args.error);
      } catch (const ws::CodedError& error) {
        text += " [" + error.code + "]";
      } catch (...) {
      }
      resolveFailed(text);
    });
    server->listen(port, hostname, [resolveFailed] { resolveFailed("listening"); });
    std::string outcome = co_await failed.promise.wait();
    co_return outcome;
  }
}

ASYNC_TEST("differential: listen validation, address() and listen errors match Bun") {
  const auto make = [] { return node_http::createServer([](std::shared_ptr<IncomingMessage>, std::shared_ptr<ServerResponse>) {}); };
  auto first = listenError(make(), 70000, "127.0.0.1");
  const std::string outcome1 = co_await std::move(first);
  EXPECT_EQ(outcome1, "RangeError: The value of \"options.port\" is out of range. It must be >= 0 and <= 65535. Received 70000 [ERR_OUT_OF_RANGE]");
  auto second = listenError(make(), -1, "127.0.0.1");
  const std::string outcome2 = co_await std::move(second);
  EXPECT_EQ(outcome2, "RangeError: The value of \"options.port\" is out of range. It must be >= 0 and <= 65535. Received -1 [ERR_OUT_OF_RANGE]");
  auto third = listenError(make(), 1.5, "127.0.0.1");
  const std::string outcome3 = co_await std::move(third);
  EXPECT_EQ(outcome3, "RangeError: The value of \"options.port\" is out of range. It must be an integer. Received 1.5 [ERR_OUT_OF_RANGE]");
  auto fourth = listenError(make(), rt::Number::NaN, "127.0.0.1");
  const std::string outcome4 = co_await std::move(fourth);
  EXPECT_EQ(outcome4, "RangeError: The value of \"options.port\" is out of range. It must be an integer. Received NaN [ERR_OUT_OF_RANGE]");
  auto fifth = listenError(make(), 0, "no-such-host.invalid");
  const std::string outcome5 = co_await std::move(fifth);
  EXPECT_EQ(outcome5, "Error: ENXIO: no such device or address, listen [ENXIO]");

  const std::shared_ptr<node_http::Server> bound = make();
  auto listening = listenError(bound, 0, "127.0.0.1");
  const std::string outcome6 = co_await std::move(listening);
  EXPECT_EQ(outcome6, "listening");
  const std::optional<node_http::AddressInfo> address = bound->address();
  ASSERT(address.has_value());
  EXPECT_EQ(address->address, "127.0.0.1");
  EXPECT_EQ(address->family, "IPv4");
  EXPECT(address->port > 0);
  auto taken = listenError(make(), address->port, "127.0.0.1");
  const std::string outcome7 = co_await std::move(taken);
  EXPECT_EQ(outcome7, "Error: Failed to start server. Is port " + rt::numberToString(address->port) + " in use? [EADDRINUSE]");
  bound->close();
  EXPECT(!bound->address().has_value());
}

TEST("differential: node:events emitter semantics of the node:http objects") {
  node_http::Emitter emitter;
  std::vector<std::string> calls;
  const IncomingListener first([&calls](const IncomingEvent&) { calls.push_back("first"); });
  const IncomingListener second([&calls](const IncomingEvent&) { calls.push_back("second"); });
  emitter.on("x", first);
  emitter.once("x", second);
  emitter.on("x", first);
  EXPECT_EQ(emitter.listenerCount("x"), 3u);
  EXPECT(emitter.emit("x"));
  EXPECT_EQ(calls, (std::vector<std::string>{"first", "second", "first"}));
  EXPECT_EQ(emitter.listenerCount("x"), 2u);
  // removeListener drops the most recent registration of exactly that listener.
  emitter.removeListener("x", first);
  EXPECT_EQ(emitter.listenerCount("x"), 1u);
  calls.clear();
  // A once listener removed during the emit that snapshot it is still called (onceWrapper).
  const IncomingListener late([&calls](const IncomingEvent&) { calls.push_back("late"); });
  emitter.once("y", IncomingListener([&](const IncomingEvent&) {
    calls.push_back("remover");
    emitter.removeListener("y", late);
  }));
  emitter.once("y", late);
  EXPECT(emitter.emit("y"));
  EXPECT_EQ(calls, (std::vector<std::string>{"remover", "late"}));
  EXPECT(!emitter.emit("y"));
}

// node:http semantics of the server's timers (not recorded from Bun, whose native timers tick
// coarsely): an idle keep-alive connection closes after keepAliveTimeout, an unfinished head
// gets 408 after headersTimeout, an unfinished request 408 after requestTimeout, and a request
// without socket activity for `timeout` is destroyed.
ASYNC_TEST("node:http semantics: keep-alive, head, request and inactivity timeouts") {
  const auto never = [](std::shared_ptr<IncomingMessage> req, std::shared_ptr<ServerResponse> res) {
    if (req->url == "/answer") res->end("ok");
    else req->resume();
  };
  struct Case {
    std::string name;
    double keepAliveTimeout;
    double headersTimeout;
    double requestTimeout;
    double timeout;
    std::vector<std::string> parts;
    std::string expected;
  };
  const std::vector<Case> cases{
    {"keep-alive idle", 150, 0, 0, 0, {"GET /answer HTTP/1.1\r\nHost: x\r\n\r\n"},
     "HTTP/1.1 200 OK\r\nDate: <DATE>\r\nConnection: keep-alive\r\nKeep-Alive: timeout=0\r\nContent-Length: 2\r\n\r\nok<CLOSED>"},
    {"head timeout", 0, 150, 0, 0, {"GET /answer HTTP/1.1\r\nHost: x\r\n"}, "HTTP/1.1 408 Request Timeout\r\nConnection: close\r\n\r\n<CLOSED>"},
    {"request timeout", 0, 0, 150, 0, {"POST /hold HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nx"}, "HTTP/1.1 408 Request Timeout\r\nConnection: close\r\n\r\n<CLOSED>"},
    {"inactivity", 0, 0, 0, 150, {"GET /hold HTTP/1.1\r\nHost: x\r\n\r\n"}, "<CLOSED>"},
    {"no timeouts", 0, 0, 0, 0, {"GET /hold HTTP/1.1\r\nHost: x\r\n\r\n"}, "<TIMEOUT>"},
  };
  for (const Case& entry : cases) {
    const std::shared_ptr<node_http::Server> server = node_http::createServer(never);
    server->keepAliveTimeout = entry.keepAliveTimeout;
    server->headersTimeout = entry.headersTimeout;
    server->requestTimeout = entry.requestTimeout;
    server->timeout = entry.timeout;
    auto listening = rt::withResolvers<void>();
    const rt::Resolve<void> resolveListening = listening.resolve;
    server->listen(0, "127.0.0.1", [resolveListening] { resolveListening(); });
    co_await listening.promise.wait();
    auto exchanging = exchange(static_cast<int>(server->address()->port), entry.parts);
    const std::string actual = co_await std::move(exchanging);
    if (actual != entry.expected) std::fprintf(stderr, "      case: %s\n", entry.name.c_str());
    EXPECT_EQ(actual, entry.expected);
    server->closeAllConnections();
    server->close();
  }
}
