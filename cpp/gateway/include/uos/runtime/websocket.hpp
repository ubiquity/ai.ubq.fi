#pragma once
// src/runtime/websocket.ts: the `ws` module the runtime and the Codex/live
// transports load on first use.
#include "uos/rt/async.hpp"
#include "uos/rt/websocket.hpp"

namespace uos::runtime::websocket {
  // `Readonly<{ default: typeof WebSocket; WebSocketServer: typeof WebSocketServer }>`
  struct WebSocketModule {
    // `module.default` (`default` is a C++ keyword).
    rt::ws::WebSocketConstructor default_;
    rt::ws::WebSocketServerConstructor WebSocketServer;
  };

  // The same promise on every call (`modulePromise ??= ...`).
  rt::Promise<WebSocketModule> loadWebSocketModule();
}
