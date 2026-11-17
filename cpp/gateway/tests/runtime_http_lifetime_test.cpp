// Links: src/runtime/http.cpp src/runtime/http_upgrade.cpp src/runtime/http_body.cpp
//        src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/websocket.cpp
#include "uos/runtime/http.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <string>
#include <utility>

namespace {
  namespace rt = uos::rt;
  namespace http = uos::runtime::http;

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
