// Links: src/runtime/http.cpp src/runtime/http_upgrade.cpp src/runtime/http_body.cpp
//        src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/websocket.cpp
#include "uos/runtime/http.hpp"
#include "uos/rt/test_async.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#if defined(__SANITIZE_ADDRESS__)
#define UOS_HTTP_LIFETIME_ASAN
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UOS_HTTP_LIFETIME_ASAN
#endif
#endif
#ifdef UOS_HTTP_LIFETIME_ASAN
#include <sanitizer/allocator_interface.h>
#include <sanitizer/asan_interface.h>
#endif

#include <functional>
#include <istream>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace {
  namespace rt = uos::rt;
  namespace http = uos::runtime::http;
  namespace asio = boost::asio;

  constexpr std::size_t payloadSize = 8 * 1024 * 1024;
#ifdef UOS_HTTP_LIFETIME_ASAN
  thread_local const volatile void** observedAllocation = nullptr;
#endif

  rt::Task<void> checkServerLifetime(bool withSignal, bool signalShutdown, bool alreadyAborted) {
    auto payload = std::make_shared<std::string>("server lifetime");
    const std::weak_ptr<std::string> weakPayload = payload;
    const auto listened = std::make_shared<int>(0);
    rt::AbortController controller;
    if (alreadyAborted) controller.abort();
    http::ServeHttpOptions options;
    options.hostname = "127.0.0.1";
    options.port = 0;
    if (withSignal) options.signal = controller.signal();
    options.onListen = [listened](const http::NetAddr&) { ++*listened; };
    http::ServeHandler handler = [payload](rt::Request, http::ServeHandlerInfo) {
      return rt::fulfilledTask(rt::Response(*payload));
    };
    http::HttpServer server = http::serveHttp(std::move(options), std::move(handler));
    handler = {};
    payload.reset();
    auto ready = server.ready;
    auto finished = server.finished;
    const http::NetAddr address = co_await ready.wait();
    EXPECT(address.port > 0);
    EXPECT_EQ(*listened, 1);
    EXPECT(!weakPayload.expired());

    auto shutdown = server.shutdown;
    if (signalShutdown && !alreadyAborted) controller.abort();
    auto stopping = shutdown();
    auto repeated = shutdown();
    EXPECT(stopping == repeated);
    co_await stopping.wait();
    co_await finished.wait();
    EXPECT(!weakPayload.expired());

    server = {};
    EXPECT(!weakPayload.expired());
    shutdown = {};
    ready = {};
    finished = {};
    stopping = {};
    repeated = {};
    co_await rt::microtask();
    co_await rt::microtask();
    EXPECT(weakPayload.expired());
    if (withSignal) controller.abort();
  }

  rt::Task<void> leaveHttpHandlerPending(std::shared_ptr<std::weak_ptr<const void>> observedRequest) {
    struct Receipt {
      rt::Promise<void> completed;
    };
    const auto started = rt::withResolvers<Receipt>();
    const rt::Promise<rt::Response> response;
    http::ServeHttpOptions options;
    options.hostname = "127.0.0.1";
    options.port = 0;
    const auto server = http::serveHttp(std::move(options), [observedRequest, response, resolve = started.resolve](rt::Request request, http::ServeHandlerInfo info) {
      *observedRequest = request.weakIdentity();
      resolve(Receipt{info.completed});
      return response.wait();
    });
    const auto address = co_await server.ready.wait();
    asio::ip::tcp::socket client(rt::executor());
    const asio::ip::tcp::endpoint endpoint(asio::ip::address_v4::loopback(), static_cast<unsigned short>(address.port));
    client.connect(endpoint);
    const std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    asio::write(client, asio::buffer(request));
    const auto receipt = co_await started.promise.wait();
    client.close();
    const auto disconnected = receipt.completed.catch_(rt::ignoreRejection);
    co_await disconnected.wait();
    EXPECT(receipt.completed.rejected());
    const auto stopping = server.shutdown();
    co_await rt::microtask();
    EXPECT(stopping.pending());
    EXPECT(!observedRequest->expired());
  }
}

#ifdef UOS_HTTP_LIFETIME_ASAN
extern "C" void __sanitizer_malloc_hook(const volatile void* address, std::size_t size) {
  if (observedAllocation && size == payloadSize) *observedAllocation = address;
}
#endif

ASYNC_TEST("HTTP close retains a backpressured write buffer until cancellation completes") {
  struct Write {
    bool backpressured = false;
    double queued = 0;
    bool finished = false;
#ifdef UOS_HTTP_LIFETIME_ASAN
    const volatile void* allocation = nullptr;
    bool liveAtClose = false;
#endif
  };
  const auto write = std::make_shared<Write>();
  const auto closed = rt::withResolvers<void>();
  const auto server = http::node_http::createServer([write, resolve = closed.resolve](const auto&, const auto& outgoing) {
    outgoing->once("close", http::node_http::IncomingListener([resolve](const auto&) { resolve(); }));
    outgoing->setHeader("Content-Length", std::to_string(payloadSize));
    const rt::Bytes payload(payloadSize, 'x');
#ifdef UOS_HTTP_LIFETIME_ASAN
    // Observe the connection's copy, allocated by write() after the fixture's payload.
    // Closing can cancel the OS operation without ever reading freed bytes in user space.
    const auto resetObserver = rt::scopeExit([] { observedAllocation = nullptr; });
    observedAllocation = &write->allocation;
#endif
    write->backpressured = !outgoing->write(std::span<const std::uint8_t>(payload));
#ifdef UOS_HTTP_LIFETIME_ASAN
    observedAllocation = nullptr;
#endif
    write->queued = outgoing->writableLength();
    outgoing->destroy();
    write->finished = outgoing->writableFinished();
#ifdef UOS_HTTP_LIFETIME_ASAN
    write->liveAtClose = write->allocation && !__asan_address_is_poisoned(write->allocation);
#endif
  });
  server->listen(0, "127.0.0.1", {});
  asio::ip::tcp::socket client(rt::executor(), asio::ip::tcp::v4());
  client.set_option(asio::socket_base::receive_buffer_size(4096));
  const asio::ip::tcp::endpoint endpoint(asio::ip::address_v4::loopback(), static_cast<unsigned short>(server->address()->port));
  client.connect(endpoint);
  const std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
  asio::write(client, asio::buffer(request));
  co_await closed.promise.wait();
  server->close();
  client.close();
  EXPECT(write->backpressured);
  EXPECT(write->queued > 0);
  EXPECT(!write->finished);
#ifdef UOS_HTTP_LIFETIME_ASAN
  EXPECT(write->allocation != nullptr);
  EXPECT(write->liveAtClose);
#endif
}

ASYNC_TEST("WebSocket authorization cannot finish an upgrade after the shutdown snapshot") {
  namespace upgrade = uos::runtime::http_upgrade;
  const auto accepted = std::make_shared<bool>(false);
  const auto stop = std::make_shared<std::function<void()>>();
  const std::weak_ptr<std::function<void()>> weakStop = stop;
  http::ServeHttpOptions options;
  options.hostname = "127.0.0.1";
  options.port = 0;
  options.upgrade = [accepted, weakStop](rt::Request, upgrade::UpgradeInfo) {
    // Shutdown's ready wait resumes between accepted authorization and the ws module wait.
    rt::queueMicrotask([weakStop] {
      if (const auto callback = weakStop.lock()) (*callback)();
    });
    upgrade::WebSocketUpgrade result;
    result.accept = [accepted](const auto&) { *accepted = true; };
    return rt::fulfilledTask(upgrade::UpgradeResult(std::move(result)));
  };
  const auto server = http::serveHttp(std::move(options), [](rt::Request, http::ServeHandlerInfo) { return rt::fulfilledTask(rt::Response()); });
  *stop = [shutdown = server.shutdown] { (void)shutdown(); };
  const auto address = co_await server.ready.wait();
  asio::ip::tcp::socket client(rt::executor());
  const asio::ip::tcp::endpoint endpoint(asio::ip::address_v4::loopback(), static_cast<unsigned short>(address.port));
  client.connect(endpoint);
  const std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
                              "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
  asio::write(client, asio::buffer(request));
  asio::streambuf wire;
  auto reading = asio::async_read_until(client, wire, "\r\n", asio::use_awaitable);
  co_await std::move(reading);
  std::istream input(&wire);
  std::string status;
  std::getline(input, status);
  client.close();
  const auto stopping = server.shutdown();
  co_await stopping.wait();
  EXPECT(status.starts_with("HTTP/1.1 503 "));
  EXPECT(!*accepted);
}

ASYNC_TEST("HTTP normal shutdown releases state after public shutdown handles are dropped") {
  auto checking = checkServerLifetime(false, false, false);
  co_await std::move(checking);
}

ASYNC_TEST("HTTP normal shutdown removes a retained external signal and releases state") {
  auto checking = checkServerLifetime(true, false, false);
  co_await std::move(checking);
}

ASYNC_TEST("HTTP signal shutdown preserves idempotent completion and releases state") {
  auto checking = checkServerLifetime(true, true, false);
  co_await std::move(checking);
}

ASYNC_TEST("HTTP already-aborted startup completes normal shutdown and releases state") {
  auto checking = checkServerLifetime(true, true, true);
  co_await std::move(checking);
}

ASYNC_TEST("HTTP failed bind releases its handler and preserves rejected server receipts") {
  auto handlerOwner = std::make_shared<int>(0);
  const std::weak_ptr<int> weakHandler = handlerOwner;
  http::ServeHttpOptions options;
  options.hostname = "127.0.0.1";
  options.port = -1;
  auto server = http::serveHttp(std::move(options), [handlerOwner](rt::Request, http::ServeHandlerInfo) { return rt::fulfilledTask(rt::Response(std::to_string(*handlerOwner))); });
  handlerOwner.reset();
  const auto ready = server.ready.catch_(rt::ignoreRejection);
  co_await ready.wait();
  ASSERT(server.ready.rejected());
  EXPECT_EQ(rt::errorName(server.ready.reason()), "RangeError");
  EXPECT(server.finished.rejected());
  EXPECT(server.finished.reason() == server.ready.reason());
  const auto stopping = server.shutdown();
  const auto stopped = stopping.catch_(rt::ignoreRejection);
  co_await stopped.wait();
  EXPECT(stopping.rejected());
  EXPECT(stopping.reason() == server.ready.reason());
  server = {};
  EXPECT(weakHandler.expired());
}

TEST("HTTP frame teardown releases the early SSE registry entry for a pending handler") {
  const auto observedRequest = std::make_shared<std::weak_ptr<const void>>();
  {
    rt::Loop loop;
    auto pending = leaveHttpHandlerPending(observedRequest);
    rt::blockOn(loop, std::move(pending));
  }
  EXPECT(observedRequest->expired());
}
