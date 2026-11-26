// Port of tests/runtime-http.test.ts. Bun's fetch, node:http's client (Agent) and node:net are
// replaced by the loopback peers of tests/helpers/loopback_http (rt has no network fetch
// transport); the ws client is rt::ws. Every server listens on 127.0.0.1 with an OS-assigned port.
// "HTTP shutdown closes an idle keep-alive socket" keeps its keep-alive connection open with a
// raw HTTP/1.1 request (what a keep-alive Agent does). "HTTP sends fragmented streams ..." clears
// the provider credential variables first (the agent environment has some set).
// Links: src/runtime/http.cpp src/runtime/http_upgrade.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp
//        src/runtime/http_tasks.cpp src/runtime/websocket.cpp tests/helpers/loopback_http.cpp
#include "uos/runtime/http.hpp"

#include "tests/helpers/loopback_http.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/url.hpp"
#include "uos/rt/websocket.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace ws = uos::rt::ws;
  using uos::runtime::http::HttpServer;
  using uos::runtime::http::NetAddr;
  using uos::runtime::http::ServeHandler;
  using uos::runtime::http::ServeHandlerInfo;
  using uos::runtime::http::ServeHttpOptions;
  using uos::runtime::http::serveHttp;
  using uos::runtime::http::trackRequestTask;
  using uos::runtime::http::WebSocketUpgrade;
  using uos::runtime::http_upgrade::UpgradeInfo;
  using uos::runtime::http_upgrade::UpgradeResult;
  using uos::tests::helpers::loopback_http::RawClient;
  using uos::tests::helpers::loopback_http::ScopedLoopbackFetch;

  ServeHttpOptions loopbackOptions() {
    ServeHttpOptions options;
    options.hostname = "127.0.0.1";
    options.port = 0;
    return options;
  }

  std::string origin(const NetAddr& address) { return "http://127.0.0.1:" + rt::numberToString(address.port); }

  rt::Task<rt::Response> fetchUrl(std::string url) {
    auto calling = rt::fetch(url);
    rt::Response response = co_await std::move(calling);
    co_return response;
  }

  rt::Task<rt::Response> fetchWith(std::string url, rt::RequestInit init) {
    auto calling = rt::fetch(url, std::move(init));
    rt::Response response = co_await std::move(calling);
    co_return response;
  }

  rt::Task<std::string> textOf(rt::Response response) {
    auto reading = response.text();
    std::string text = co_await std::move(reading);
    co_return text;
  }

  // `await server.shutdown()`
  rt::Task<void> shutdown(HttpServer server) {
    const rt::Promise<void> stopping = server.shutdown();
    co_await stopping.wait();
  }

  rt::ByteChunk bytes(std::string_view text) { return rt::encodeUtf8(text); }

  rt::Response streamResponse(rt::UnderlyingSource<rt::ByteChunk> source, rt::ResponseInit init) { return rt::Response(rt::BodyInit(rt::ByteStream(std::move(source))), std::move(init)); }

  template <class T> rt::Task<std::optional<std::string>> rejectionOf(rt::Promise<T> promise) {
    rt::Reason failure;
    try {
      co_await promise.wait();
    } catch (...) {
      failure = std::current_exception();
    }
    if (!failure) co_return std::nullopt;
    co_return rt::errorMessage(failure).value_or(rt::reasonToString(failure));
  }
}

ASYNC_TEST("RawClient releases self-referencing listeners after a peer closes") {
  namespace asio = boost::asio;
  asio::ip::tcp::acceptor acceptor(rt::executor(), asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  auto client = RawClient::connect(static_cast<int>(acceptor.local_endpoint().port()));
  auto peer = acceptor.accept();
  const std::weak_ptr<RawClient> weak = client;
  const auto closed = rt::withResolvers<void>();
  client->on("data", [client](const std::string&) {});
  client->once("close", [client, resolve = closed.resolve](const std::string&) { resolve(); });
  peer.close();
  client.reset();
  co_await closed.promise.wait();
  EXPECT(weak.expired());
}

ASYNC_TEST("RawClient destroy releases listeners and preserves deferred close registration order") {
  auto client = std::make_shared<RawClient>(boost::asio::ip::tcp::socket(rt::executor()));
  const std::weak_ptr<RawClient> weak = client;
  const auto closed = rt::withResolvers<void>();
  std::vector<std::string> events;
  client->on("data", [client](const std::string&) {});
  client->once("close", [client, &events, resolve = closed.resolve](const std::string&) {
    events.push_back("original");
    resolve();
  });
  client->destroy();
  client->on("close", [client, &events](const std::string&) { events.push_back("late"); });
  client.reset();
  co_await closed.promise.wait();
  EXPECT_EQ(events, (std::vector<std::string>{"original", "late"}));
  EXPECT(weak.expired());
}

TEST("RawClient loop teardown releases an abandoned close without emitting it") {
  std::weak_ptr<RawClient> weak;
  bool closed = false;
  {
    rt::FakeClock clock;
    rt::Loop loop(clock);
    rt::LoopScope scope(loop);
    auto client = std::make_shared<RawClient>(boost::asio::ip::tcp::socket(rt::executor()));
    weak = client;
    client->on("data", [client](const std::string&) {});
    client->once("close", [client, &closed](const std::string&) { closed = true; });
    client->destroy();
    client->on("close", [client, &closed](const std::string&) { closed = true; });
    client.reset();
    EXPECT(!weak.expired());
  }
  EXPECT(weak.expired());
  EXPECT(!closed);
}

ASYNC_TEST("HTTP admin handler failures are marked no-store") {
  const ScopedLoopbackFetch loopback;
  const HttpServer server = serveHttp(loopbackOptions(), [](rt::Request, ServeHandlerInfo) -> rt::Task<rt::Response> { throw rt::JsError("fixture handler failure"); });
  const NetAddr address = co_await server.ready.wait();
  const std::vector<std::string> paths{"/admin/fixture-failure", "/admin.html"};
  for (const std::string& path : paths) {
    auto pending1_ = fetchUrl(origin(address) + path);
    const rt::Response response = co_await std::move(pending1_);
    EXPECT_EQ(response.status(), 500);
    EXPECT_EQ(response.headers().get("cache-control"), std::optional<std::string>("no-store"));
    auto pending2_ = textOf(response);
    const std::string text = co_await std::move(pending2_);
    EXPECT_EQ(text, "Internal Server Error");
  }
  auto stopping1_ = shutdown(server);
  co_await std::move(stopping1_);
}

ASYNC_TEST("HTTP ingress delivers the exact body limit and sends 413 above it") {
  const ScopedLoopbackFetch loopback;
  int dispatched = 0;
  const HttpServer server = serveHttp(loopbackOptions(), [&dispatched](rt::Request request, ServeHandlerInfo) -> rt::Task<rt::Response> {
    ++dispatched;
    auto reading = request.bytes();
    const auto bytes = co_await std::move(reading);
    co_return rt::Response(rt::BodyInit(std::to_string(bytes.size())));
  });
  const auto address = co_await server.ready.wait();
  const auto url = origin(address) + "/body";
  for (const std::size_t extra : {0u, 1u}) {
    rt::RequestInit post;
    post.method = "POST";
    post.body = rt::BodyInit(rt::Bytes(rt::limits::kHttpBodyBytes + extra, 'a'));
    auto sending = fetchWith(url, std::move(post));
    const auto response = co_await std::move(sending);
    EXPECT_EQ(response.status(), extra == 0 ? 200 : 413);
    if (extra == 0) {
      auto reading = response.text();
      const auto text = co_await std::move(reading);
      EXPECT_EQ(text, std::to_string(rt::limits::kHttpBodyBytes));
    }
  }
  EXPECT_EQ(dispatched, 1);
  auto stopping = shutdown(server);
  co_await std::move(stopping);
}

ASYNC_TEST("HTTP shutdown closes an idle keep-alive socket instead of waiting for systemd") {
  const HttpServer server = serveHttp(loopbackOptions(), [](rt::Request, ServeHandlerInfo) -> rt::Task<rt::Response> { co_return rt::Response(rt::BodyInit(std::string("ok"))); });
  const NetAddr address = co_await server.ready.wait();
  const std::shared_ptr<RawClient> agent = RawClient::connect(static_cast<int>(address.port));
  auto [responded, resolveResponded, rejectResponded] = rt::withResolvers<void>();
  auto wire = std::make_shared<std::string>();
  const rt::Resolve<void> resolve = resolveResponded;
  agent->on("data", [wire, resolve](const std::string& chunk) {
    *wire += chunk;
    if (rt::endsWith(*wire, "0\r\n\r\n")) resolve();
  });
  agent->once("connect", [agent](const std::string&) { agent->write("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"); });
  co_await responded.wait();
  EXPECT(rt::startsWith(*wire, "HTTP/1.1 200 OK\r\n"));
  EXPECT(rt::includes(*wire, "Connection: keep-alive\r\n"));
  const double started = rt::performanceNow();
  const rt::Promise<void> stopping = server.shutdown();
  const rt::Promise<void> deadline(rt::async([]() -> rt::Task<void> {
    co_await rt::sleep(1'000);
    throw rt::JsError("HTTP shutdown did not close the idle keep-alive socket");
  }));
  std::vector<rt::Promise<void>> racers{stopping, deadline};
  const rt::Promise<void> race = rt::race(std::move(racers));
  auto pending3_ = rejectionOf(race);
  const std::optional<std::string> failure = co_await std::move(pending3_);
  EXPECT(!failure.has_value());
  EXPECT(rt::performanceNow() - started < 1'000);
  agent->destroy();
  auto stopping2_ = shutdown(server);
  co_await std::move(stopping2_);
}

ASYNC_TEST("HTTP sends fragmented streams, preserves cookies and observes transport completion") {
  const rt::env::ScopedOverride credentials{{"UOS_AI_TOKEN", std::nullopt},     {"DENO_DEPLOY_TOKEN", std::nullopt},  {"CODEX_AUTH_JSON_B64", std::nullopt},
                                            {"OPENROUTER_API_KEY", std::nullopt}, {"SURPLUS_API_KEY", std::nullopt}, {"METERED_API_KEY", std::nullopt}};
  bool credentialsPresent = false;
  for (const char* name : {"UOS_AI_TOKEN", "DENO_DEPLOY_TOKEN", "CODEX_AUTH_JSON_B64", "OPENROUTER_API_KEY", "SURPLUS_API_KEY", "METERED_API_KEY"})
    if (rt::truthy(rt::env::get(name))) credentialsPresent = true;
  EXPECT_EQ(credentialsPresent, false);
  const ScopedLoopbackFetch loopback;
  struct Observation {
    rt::Request request;
    std::string peer;
    rt::Promise<void> completed;
  };
  auto received = rt::withResolvers<Observation>();
  const rt::Resolve<Observation> resolveReceived = received.resolve;
  const HttpServer server = serveHttp(loopbackOptions(), [resolveReceived](rt::Request request, ServeHandlerInfo info) -> rt::Task<rt::Response> {
    const auto* peer = std::get_if<NetAddr>(&info.remoteAddr);
    resolveReceived(Observation{request, peer && peer->transport == "tcp" ? peer->hostname : "", info.completed});
    auto index = std::make_shared<int>(0);
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.pull = [index](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) -> rt::Task<void> {
      if (*index == 3) {
        controller.close();
        co_return;
      }
      co_await rt::sleep(15);
      controller.enqueue(bytes("data: " + std::to_string((*index)++) + "\n\n"));
    };
    rt::ResponseInit init;
    init.headers = rt::Headers{{"content-type", "text/event-stream"}, {"set-cookie", "first=1; Path=/"}};
    co_return streamResponse(std::move(source), std::move(init));
  });
  const NetAddr address = co_await server.ready.wait();
  rt::RequestInit init;
  init.headers = rt::Headers{{"cookie", "fixture=1"}};
  auto pending4_ = fetchWith(origin(address) + "/fragmented", std::move(init));
  const rt::Response response = co_await std::move(pending4_);
  const Observation observation = co_await received.promise.wait();
  EXPECT_EQ(observation.peer, "127.0.0.1");
  EXPECT_EQ(observation.request.headers().get("cookie"), std::optional<std::string>("fixture=1"));
  EXPECT_EQ(response.headers().getSetCookie(), (std::vector<std::string>{"first=1; Path=/"}));
  auto pending5_ = textOf(response);
  const std::string text = co_await std::move(pending5_);
  EXPECT_EQ(text, "data: 0\n\ndata: 1\n\ndata: 2\n\n");
  co_await observation.completed.wait();
  auto stopping3_ = shutdown(server);
  co_await std::move(stopping3_);
}

ASYNC_TEST("HTTP disconnect rejects delivery and cancels a pending response pull") {
  auto cancelled = rt::withResolvers<void>();
  struct Receipt {
    rt::Promise<void> completed;
    rt::AbortSignal signal;
  };
  auto receipt = rt::withResolvers<Receipt>();
  auto pending = rt::withResolvers<void>();
  const rt::Resolve<void> resolveCancelled = cancelled.resolve;
  const rt::Resolve<Receipt> resolveReceipt = receipt.resolve;
  const rt::Promise<void> pendingPromise = pending.promise;
  const HttpServer server = serveHttp(loopbackOptions(), [resolveCancelled, resolveReceipt, pendingPromise](rt::Request request, ServeHandlerInfo info) -> rt::Task<rt::Response> {
    resolveReceipt(Receipt{info.completed, request.signal()});
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.start = [](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) { controller.enqueue(bytes("data: first\n\n")); };
    source.pull = [pendingPromise](rt::ReadableStreamDefaultController<rt::ByteChunk>) -> rt::Task<void> { co_await pendingPromise.wait(); };
    source.cancel = [resolveCancelled](rt::Reason) { resolveCancelled(); };
    rt::ResponseInit init;
    init.headers = rt::Headers{{"content-type", "text/event-stream"}};
    co_return streamResponse(std::move(source), std::move(init));
  });
  const NetAddr address = co_await server.ready.wait();
  auto closed = rt::withResolvers<void>();
  const rt::Resolve<void> resolveClosed = closed.resolve;
  const std::shared_ptr<RawClient> socket = RawClient::connect(static_cast<int>(address.port));
  socket->once("connect", [socket](const std::string&) { socket->write("GET /disconnect HTTP/1.1\r\nHost: localhost\r\n\r\n"); });
  socket->once("data", [socket](const std::string&) { socket->destroy(); });
  socket->once("error", [](const std::string&) {});
  socket->once("close", [resolveClosed](const std::string&) { resolveClosed(); });
  co_await closed.promise.wait();
  co_await cancelled.promise.wait();
  const Receipt observation = co_await receipt.promise.wait();
  auto pending6_ = rejectionOf(observation.completed);
  const std::optional<std::string> rejection = co_await std::move(pending6_);
  EXPECT(rejection && rt::Regex("disconnected", "").test(*rejection));
  EXPECT_EQ(observation.signal.aborted(), true);
  pending.resolve();
  auto stopping4_ = shutdown(server);
  co_await std::move(stopping4_);
}

ASYNC_TEST("HTTP retains POST bodies and strips HEAD and bodyless response bodies") {
  const ScopedLoopbackFetch loopback;
  const HttpServer server = serveHttp(loopbackOptions(), [](rt::Request request, ServeHandlerInfo) -> rt::Task<rt::Response> {
    if (request.method() == "POST") {
      auto reading = request.text();
      const std::string text = co_await std::move(reading);
      co_return rt::Response(rt::BodyInit(text));
    }
    if (rt::URL(request.url()).pathname() == "/empty") {
      rt::ResponseInit init;
      init.status = 204;
      co_return rt::Response(nullptr, std::move(init));
    }
    rt::Headers headers{{"content-type", "text/plain"}, {"content-length", "7"}};
    headers.append("set-cookie", "one=1; Path=/");
    headers.append("set-cookie", "two=2; Path=/");
    rt::ResponseInit init;
    init.headers = headers;
    co_return rt::Response(rt::BodyInit(std::string("fixture")), std::move(init));
  });
  const NetAddr address = co_await server.ready.wait();
  const std::string base = origin(address);
  rt::RequestInit post;
  post.method = "POST";
  post.body = rt::BodyInit(std::string("posted body"));
  auto pending7_ = fetchWith(base, std::move(post));
  const rt::Response posted = co_await std::move(pending7_);
  auto pending8_ = textOf(posted);
  const std::string postedText = co_await std::move(pending8_);
  EXPECT_EQ(postedText, "posted body");
  rt::RequestInit headInit;
  headInit.method = "HEAD";
  auto pending9_ = fetchWith(base, std::move(headInit));
  const rt::Response head = co_await std::move(pending9_);
  auto pending10_ = textOf(head);
  const std::string headText = co_await std::move(pending10_);
  EXPECT_EQ(headText, "");
  EXPECT_EQ(head.headers().get("content-length"), std::optional<std::string>("7"));
  EXPECT_EQ(head.headers().getSetCookie(), (std::vector<std::string>{"one=1; Path=/", "two=2; Path=/"}));
  auto pending11_ = fetchUrl(base + "/empty");
  const rt::Response empty = co_await std::move(pending11_);
  auto pending12_ = textOf(empty);
  const std::string emptyText = co_await std::move(pending12_);
  EXPECT_EQ(emptyText, "");
  auto stopping5_ = shutdown(server);
  co_await std::move(stopping5_);
}

ASYNC_TEST("HTTP gracefully drains an active stream before finished resolves") {
  const ScopedLoopbackFetch loopback;
  auto release = rt::withResolvers<void>();
  auto started = rt::withResolvers<void>();
  const rt::Promise<void> releasePromise = release.promise;
  const rt::Resolve<void> resolveStarted = started.resolve;
  const HttpServer server = serveHttp(loopbackOptions(), [releasePromise, resolveStarted](rt::Request, ServeHandlerInfo) -> rt::Task<rt::Response> {
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.start = [releasePromise, resolveStarted](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) -> rt::Task<void> {
      controller.enqueue(bytes("first"));
      resolveStarted();
      co_await releasePromise.wait();
      controller.enqueue(bytes("last"));
      controller.close();
    };
    co_return streamResponse(std::move(source), rt::ResponseInit{});
  });
  const NetAddr address = co_await server.ready.wait();
  auto pending13_ = fetchUrl(origin(address));
  const rt::Response response = co_await std::move(pending13_);
  co_await started.promise.wait();
  auto stopped = std::make_shared<bool>(false);
  const rt::Promise<void> stopping = server.shutdown().then([stopped] { *stopped = true; });
  co_await rt::sleep(20);
  EXPECT_EQ(*stopped, false);
  release.resolve();
  auto pending14_ = textOf(response);
  const std::string text = co_await std::move(pending14_);
  EXPECT_EQ(text, "firstlast");
  co_await stopping.wait();
  EXPECT_EQ(*stopped, true);
  auto stopping6_ = shutdown(server);
  co_await std::move(stopping6_);
}

ASYNC_TEST("HTTP shutdown waits for delivery-triggered durable application writes") {
  const ScopedLoopbackFetch loopback;
  auto writeStarted = rt::withResolvers<void>();
  auto releaseWrite = rt::withResolvers<void>();
  const rt::Resolve<void> resolveWriteStarted = writeStarted.resolve;
  const rt::Promise<void> releasePromise = releaseWrite.promise;
  const HttpServer server = serveHttp(loopbackOptions(), [resolveWriteStarted, releasePromise](rt::Request, ServeHandlerInfo info) -> rt::Task<rt::Response> {
    const rt::Promise<void> write = info.completed.then([resolveWriteStarted, releasePromise]() -> rt::Task<void> {
      resolveWriteStarted();
      co_await releasePromise.wait();
    });
    trackRequestTask(info.completed, write);
    co_return rt::Response(rt::BodyInit(std::string("delivered")));
  });
  const NetAddr address = co_await server.ready.wait();
  auto pending15_ = fetchUrl(origin(address));
  const rt::Response response = co_await std::move(pending15_);
  auto pending16_ = textOf(response);
  const std::string text = co_await std::move(pending16_);
  EXPECT_EQ(text, "delivered");
  co_await writeStarted.promise.wait();
  auto stopped = std::make_shared<bool>(false);
  const rt::Promise<void> stopping = server.shutdown().then([stopped] { *stopped = true; });
  co_await rt::sleep(25);
  EXPECT_EQ(*stopped, false);
  releaseWrite.resolve();
  co_await stopping.wait();
  EXPECT_EQ(*stopped, true);
  auto stopping7_ = shutdown(server);
  co_await std::move(stopping7_);
}

ASYNC_TEST("HTTP quiet streams survive Bun's native ten-second default idle timeout") {
  const ScopedLoopbackFetch loopback;
  const HttpServer server = serveHttp(loopbackOptions(), [](rt::Request, ServeHandlerInfo) -> rt::Task<rt::Response> {
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.start = [](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) -> rt::Task<void> {
      controller.enqueue(bytes("first"));
      co_await rt::sleep(10'500);
      controller.enqueue(bytes("last"));
      controller.close();
    };
    co_return streamResponse(std::move(source), rt::ResponseInit{});
  });
  const NetAddr address = co_await server.ready.wait();
  auto pending17_ = fetchUrl(origin(address));
  const rt::Response response = co_await std::move(pending17_);
  auto pending18_ = textOf(response);
  const std::string text = co_await std::move(pending18_);
  EXPECT_EQ(text, "firstlast");
  auto stopping8_ = shutdown(server);
  co_await std::move(stopping8_);
}

ASYNC_TEST("WebSocket authorization refuses before upgrade and preserves real bidirectional frames") {
  auto accepted = rt::withResolvers<void>();
  const rt::Resolve<void> resolveAccepted = accepted.resolve;
  ServeHttpOptions options = loopbackOptions();
  options.upgrade = [resolveAccepted](rt::Request request, UpgradeInfo info) -> rt::Task<UpgradeResult> {
    EXPECT_EQ(info.remoteAddr.hostname, "127.0.0.1");
    if (request.headers().get("authorization") != std::optional<std::string>("Bearer fixture-token")) {
      rt::ResponseInit init;
      init.status = 401;
      co_return UpgradeResult(rt::Response(rt::BodyInit(std::string("unauthorized")), std::move(init)));
    }
    WebSocketUpgrade upgrade;
    upgrade.maxPayloadBytes = 1024;
    upgrade.accept = [resolveAccepted](std::shared_ptr<ws::WebSocket> socket) {
      ws::WebSocket* raw = socket.get();
      socket->on("message", [raw](const ws::EventArgs& event) { raw->send(event.data); });
      resolveAccepted();
    };
    co_return UpgradeResult(upgrade);
  };
  const HttpServer server = serveHttp(options, [](rt::Request, ServeHandlerInfo) -> rt::Task<rt::Response> { co_return rt::Response(rt::BodyInit(std::string("ordinary HTTP"))); });
  std::vector<std::shared_ptr<ws::WebSocket>> clients;
  const NetAddr address = co_await server.ready.wait();
  const std::string url = "ws://127.0.0.1:" + rt::numberToString(address.port) + "/upgrade";
  auto refusal = rt::withResolvers<int>();
  const rt::Resolve<int> resolveRefusal = refusal.resolve;
  const std::shared_ptr<ws::WebSocket> denied = ws::WebSocket::create(url);
  clients.push_back(denied);
  denied->on("error", [] {});
  ws::WebSocket* deniedRaw = denied.get();
  denied->on("unexpected-response", [resolveRefusal, deniedRaw](const ws::EventArgs& event) {
    resolveRefusal(event.response->statusCode);
    event.response->resume();
    deniedRaw->terminate();
  });
  const int refusalStatus = co_await refusal.promise.wait();
  EXPECT_EQ(refusalStatus, 401);
  auto echoed = rt::withResolvers<std::string>();
  const rt::Resolve<std::string> resolveEchoed = echoed.resolve;
  const rt::Reject rejectEchoed = echoed.reject;
  ws::ClientOptions clientOptions;
  clientOptions.headers = {{"authorization", "Bearer fixture-token"}};
  const std::shared_ptr<ws::WebSocket> client = ws::WebSocket::create(url, clientOptions);
  clients.push_back(client);
  ws::WebSocket* clientRaw = client.get();
  client->once("open", [clientRaw] { clientRaw->send(std::string("fixture frame")); });
  client->once("message", [resolveEchoed](const ws::EventArgs& event) { resolveEchoed(rt::decodeUtf8(std::get<ws::Bytes>(event.data), {.ignoreBOM = true})); });
  client->once("error", [rejectEchoed](const ws::EventArgs& event) { rejectEchoed(event.error); });
  co_await accepted.promise.wait();
  const std::string echo = co_await echoed.promise.wait();
  EXPECT_EQ(echo, "fixture frame");
  auto closed = rt::withResolvers<int>();
  const rt::Resolve<int> resolveClosed = closed.resolve;
  client->once("close", [resolveClosed](const ws::EventArgs& event) { resolveClosed(event.code); });
  auto stopping9_ = shutdown(server);
  co_await std::move(stopping9_);
  const int code = co_await closed.promise.wait();
  EXPECT_EQ(code, 1001);
  for (const auto& entry : clients) entry->terminate();
  auto stopping10_ = shutdown(server);
  co_await std::move(stopping10_);
}

ASYNC_TEST("WebSocket hard payload limit preserves the route's failure close contract") {
  auto messages = std::make_shared<int>(0);
  ServeHttpOptions options = loopbackOptions();
  options.upgrade = [messages](rt::Request, UpgradeInfo) -> rt::Task<UpgradeResult> {
    WebSocketUpgrade upgrade;
    upgrade.maxPayloadBytes = 1024;
    upgrade.payloadLimitClose = uos::runtime::http_upgrade::PayloadLimitClose{1011, "fixture payload limit"};
    upgrade.accept = [messages](std::shared_ptr<ws::WebSocket> socket) {
      socket->on("message", [messages] { (*messages)++; });
      socket->on("error", [] {});
    };
    co_return UpgradeResult(upgrade);
  };
  const HttpServer server = serveHttp(options, [](rt::Request, ServeHandlerInfo) -> rt::Task<rt::Response> { co_return rt::Response(rt::BodyInit(std::string("ordinary HTTP"))); });
  const NetAddr address = co_await server.ready.wait();
  struct Closed {
    int code;
    std::string reason;
  };
  auto closed = rt::withResolvers<Closed>();
  const rt::Resolve<Closed> resolveClosed = closed.resolve;
  const rt::Reject rejectClosed = closed.reject;
  const rt::Timeout timeout = rt::setTimeout([rejectClosed, messages] { rejectClosed(rt::makeReason(rt::JsError("Payload fixture did not close: messages=" + std::to_string(*messages)))); }, 1000);
  (void)closed.promise.finally_([timeout] { rt::clearTimeout(timeout); }).catch_(rt::ignoreRejection);
  const std::shared_ptr<ws::WebSocket> client = ws::WebSocket::create("ws://127.0.0.1:" + rt::numberToString(address.port));
  ws::WebSocket* raw = client.get();
  client->once("open", [raw] { raw->send(ws::Bytes(1025, 0)); });
  client->once("close", [resolveClosed](const ws::EventArgs& event) { resolveClosed(Closed{event.code, rt::decodeUtf8(event.reason, {.ignoreBOM = true})}); });
  client->once("error", [rejectClosed](const ws::EventArgs& event) { rejectClosed(event.error); });
  const Closed result = co_await closed.promise.wait();
  EXPECT_EQ(result.code, 1011);
  EXPECT_EQ(result.reason, "fixture payload limit");
  EXPECT_EQ(*messages, 0);
  client->terminate();
  auto stopping11_ = shutdown(server);
  co_await std::move(stopping11_);
}

ASYNC_TEST("HTTP bounds producer reads when a real socket applies backpressure") {
  struct Receipt {
    rt::Promise<void> completed;
  };
  auto observed = rt::withResolvers<Receipt>();
  const rt::Resolve<Receipt> resolveObserved = observed.resolve;
  auto pulls = std::make_shared<int>(0);
  const HttpServer server = serveHttp(loopbackOptions(), [resolveObserved, pulls](rt::Request, ServeHandlerInfo info) -> rt::Task<rt::Response> {
    resolveObserved(Receipt{info.completed});
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.pull = [pulls](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
      if (*pulls == 512) controller.close();
      else {
        (*pulls)++;
        controller.enqueue(rt::ByteChunk(64 * 1024));
      }
    };
    co_return streamResponse(std::move(source), rt::ResponseInit{});
  });
  const NetAddr address = co_await server.ready.wait();
  const std::shared_ptr<RawClient> socket = RawClient::connect(static_cast<int>(address.port));
  socket->on("error", [](const std::string&) {});
  socket->pause();
  socket->once("connect", [socket](const std::string&) { socket->write("GET /slow HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"); });
  const Receipt receipt = co_await observed.promise.wait();
  const rt::Promise<void> completed = receipt.completed;
  auto settled = std::make_shared<bool>(false);
  (void)completed.then([settled] { *settled = true; }, [settled](rt::Reason) { *settled = true; });
  co_await rt::sleep(50);
  EXPECT(*pulls > 0);
  EXPECT(*pulls < 512);
  EXPECT_EQ(*settled, false);
  socket->on("data", [](const std::string&) {});
  socket->resume();
  co_await completed.wait();
  socket->destroy();
  auto stopping12_ = shutdown(server);
  co_await std::move(stopping12_);
}

ASYNC_TEST("HTTP cancelled request body sends an early 413 while the upload is unfinished") {
  struct Receipt {
    rt::Promise<void> completed;
    rt::AbortSignal signal;
  };
  auto observed = rt::withResolvers<Receipt>();
  const rt::Resolve<Receipt> resolveObserved = observed.resolve;
  const std::string body = rt::jsonStringify(json::value(json::object{{"error", "payload_too_large"}}));
  const HttpServer server = serveHttp(loopbackOptions(), [resolveObserved, body](rt::Request request, ServeHandlerInfo info) -> rt::Task<rt::Response> {
    resolveObserved(Receipt{info.completed, request.signal()});
    if (rt::stringToNumber(request.headers().get("content-length").value_or("")) > 1024) {
      if (const std::optional<rt::ByteStream> stream = request.body()) {
        auto cancelling = stream->cancel();
        co_await std::move(cancelling);
      }
      rt::ResponseInit init;
      init.status = 413;
      init.headers = rt::Headers{{"content-type", "application/json"}, {"content-length", std::to_string(rt::utf8ByteLength(body))}};
      co_return rt::Response(rt::BodyInit(body), std::move(init));
    }
    auto reading = request.text();
    const std::string text = co_await std::move(reading);
    co_return rt::Response(rt::BodyInit(text));
  });
  const NetAddr address = co_await server.ready.wait();
  const std::shared_ptr<RawClient> socket = RawClient::connect(static_cast<int>(address.port));
  auto response = rt::withResolvers<std::string>();
  const rt::Resolve<std::string> resolveResponse = response.resolve;
  const rt::Reject rejectResponse = response.reject;
  const rt::Timeout timer = rt::setTimeout([rejectResponse] { rejectResponse(rt::makeReason(rt::JsError("The cancelled upload did not receive its early response"))); }, 1500);
  auto wire = std::make_shared<std::string>();
  socket->once("connect", [socket](const std::string&) { socket->write("POST /oversize HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1025\r\nConnection: close\r\n\r\nx"); });
  socket->on("data", [wire, body, resolveResponse](const std::string& chunk) {
    *wire += chunk;
    if (rt::includes(*wire, body)) resolveResponse(*wire);
  });
  socket->once("error", [rejectResponse](const std::string& error) { rejectResponse(rt::makeReason(rt::JsError(error))); });
  const std::string received = co_await response.promise.wait();
  const Receipt receipt = co_await observed.promise.wait();
  EXPECT(rt::startsWith(received, "HTTP/1.1 413"));
  EXPECT(rt::includes(received, "application/json"));
  EXPECT(rt::includes(received, body));
  co_await receipt.completed.wait();
  EXPECT_EQ(receipt.signal.aborted(), false);
  rt::clearTimeout(timer);
  socket->destroy();
  auto stopping13_ = shutdown(server);
  co_await std::move(stopping13_);
}

ASYNC_TEST("HTTP unfinished POST disconnect interrupts input and rejects delivery") {
  struct Receipt {
    rt::Promise<void> completed;
    rt::AbortSignal signal;
  };
  auto observed = rt::withResolvers<Receipt>();
  const rt::Resolve<Receipt> resolveObserved = observed.resolve;
  const HttpServer server = serveHttp(loopbackOptions(), [resolveObserved](rt::Request request, ServeHandlerInfo info) -> rt::Task<rt::Response> {
    resolveObserved(Receipt{info.completed, request.signal()});
    auto reading = request.text();
    const std::string text = co_await std::move(reading);
    co_return rt::Response(rt::BodyInit(text));
  });
  const NetAddr address = co_await server.ready.wait();
  const std::shared_ptr<RawClient> socket = RawClient::connect(static_cast<int>(address.port));
  socket->on("error", [](const std::string&) {});
  socket->once("connect", [socket](const std::string&) { socket->write("POST /disconnect HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1024\r\n\r\nx"); });
  const Receipt receipt = co_await observed.promise.wait();
  socket->destroy();
  auto pending19_ = rejectionOf(receipt.completed);
  const std::optional<std::string> rejection = co_await std::move(pending19_);
  EXPECT(rejection.has_value());
  EXPECT_EQ(receipt.signal.aborted(), true);
  auto stopping14_ = shutdown(server);
  co_await std::move(stopping14_);
}

#if defined(__linux__)
ASYNC_TEST("HTTP TCP reset interrupts a paused POST without draining its upload") {
  namespace asio = boost::asio;
  struct Observation {
    rt::Request request;
    rt::Promise<void> completed;
  };
  const auto observed = rt::withResolvers<Observation>();
  const auto respond = rt::withResolvers<void>();
  const auto server = serveHttp(loopbackOptions(), [observed, ready = respond.promise](rt::Request request, ServeHandlerInfo info) -> rt::Task<rt::Response> {
    observed.resolve(Observation{request, info.completed});
    co_await ready.wait();
    co_return rt::Response("fixture response");
  });
  const auto address = co_await server.ready.wait();
  asio::ip::tcp::socket client(rt::executor());
  client.connect({asio::ip::address_v4::loopback(), static_cast<unsigned short>(address.port)});
  client.set_option(asio::socket_base::send_buffer_size(4096));
  constexpr std::size_t payloadBytes = 8 * 1024 * 1024;
  auto wire = std::make_shared<std::string>("POST /reset HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(payloadBytes) + "\r\n\r\n");
  wire->append(payloadBytes, 'x');
  const auto uploadCompleted = std::make_shared<bool>(false);
  asio::async_write(client, asio::buffer(*wire), [wire, uploadCompleted](const boost::system::error_code& error, std::size_t) {
    if (!error) *uploadCompleted = true;
  });
  const auto observation = co_await observed.promise.wait();
  co_await rt::sleep(20);
  EXPECT(!observation.request.bodyUsed());
  EXPECT(!*uploadCompleted);

  client.set_option(asio::socket_base::linger(true, 0));
  client.close();
  const auto disconnected = observation.completed.then([] { return false; }, [](rt::Reason) { return true; });
  const auto expired = rt::withResolvers<bool>();
  const auto timer = rt::setTimeout([expired] { expired.resolve(false); }, 1'000);
  const auto interrupted = co_await rt::race(disconnected, expired.promise).wait();
  rt::clearTimeout(timer);
  EXPECT(interrupted);
  EXPECT(observation.request.signal().aborted());
  EXPECT(!observation.request.bodyUsed());
  respond.resolve();
  co_await shutdown(server);
}
#endif

ASYNC_TEST("HTTP a paused POST can answer a client that half-closes its upload") {
  namespace asio = boost::asio;
  const auto observed = rt::withResolvers<rt::AbortSignal>();
  const auto respond = rt::withResolvers<void>();
  const auto server = serveHttp(loopbackOptions(), [observed, ready = respond.promise](rt::Request request, ServeHandlerInfo) -> rt::Task<rt::Response> {
    observed.resolve(request.signal());
    co_await ready.wait();
    co_return rt::Response("fixture response");
  });
  const auto address = co_await server.ready.wait();
  asio::ip::tcp::socket client(rt::executor());
  client.connect({asio::ip::address_v4::loopback(), static_cast<unsigned short>(address.port)});
  std::string_view wire = "POST /half-close HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\nConnection: close\r\n\r\nbody";
  asio::write(client, asio::buffer(wire));
  client.shutdown(asio::socket_base::shutdown_send);
  const auto signal = co_await observed.promise.wait();
  co_await rt::sleep(20);
  EXPECT(!signal.aborted());
  respond.resolve();
  std::string reply;
  co_await asio::async_read_until(client, asio::dynamic_buffer(reply), "fixture response", asio::use_awaitable);
  EXPECT(reply.starts_with("HTTP/1.1 200"));
  EXPECT(!signal.aborted());
  client.close();
  co_await shutdown(server);
}

ASYNC_TEST("HTTP unread POST refusal completes its response and server shutdown") {
  const ScopedLoopbackFetch loopback;
  struct Receipt {
    rt::Promise<void> completed;
    rt::AbortSignal signal;
  };
  auto observed = rt::withResolvers<Receipt>();
  const rt::Resolve<Receipt> resolveObserved = observed.resolve;
  const HttpServer server = serveHttp(loopbackOptions(), [resolveObserved](rt::Request request, ServeHandlerInfo info) -> rt::Task<rt::Response> {
    if (rt::URL(request.url()).pathname() == "/accept") {
      auto reading = request.text();
      const std::string text = co_await std::move(reading);
      co_return rt::Response(rt::BodyInit(text));
    }
    resolveObserved(Receipt{info.completed, request.signal()});
    co_await rt::sleep(25);
    rt::ResponseInit init;
    init.status = 400;
    co_return rt::Response(rt::BodyInit(std::string("content type refused")), std::move(init));
  });
  const NetAddr address = co_await server.ready.wait();
  rt::RequestInit refuse;
  refuse.method = "POST";
  refuse.body = rt::BodyInit(std::string("not multipart"));
  auto pending20_ = fetchWith(origin(address) + "/refuse", std::move(refuse));
  const rt::Response response = co_await std::move(pending20_);
  EXPECT_EQ(response.status(), 400);
  auto pending21_ = textOf(response);
  const std::string refusedText = co_await std::move(pending21_);
  EXPECT_EQ(refusedText, "content type refused");
  rt::RequestInit accept;
  accept.method = "POST";
  accept.body = rt::BodyInit(std::string("second request body"));
  auto pending22_ = fetchWith(origin(address) + "/accept", std::move(accept));
  const rt::Response accepted = co_await std::move(pending22_);
  EXPECT_EQ(accepted.status(), 200);
  auto pending23_ = textOf(accepted);
  const std::string acceptedText = co_await std::move(pending23_);
  EXPECT_EQ(acceptedText, "second request body");
  const Receipt receipt = co_await observed.promise.wait();
  co_await receipt.completed.wait();
  EXPECT_EQ(receipt.signal.aborted(), false);
  auto stopping15_ = shutdown(server);
  co_await std::move(stopping15_);
}
