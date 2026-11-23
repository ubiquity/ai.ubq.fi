#include "uos/runtime/websocket.hpp"

#include <optional>

namespace uos::runtime::websocket {
  namespace {
    std::optional<rt::Promise<WebSocketModule>> modulePromise;

    /** Bun replaces bare ws imports, including when the exact npm package is installed.
     * Resolve its supported package.json export and load the package's declared ESM
     * target so Unix sockets, handshake limits and receiver limits use pinned ws.
     */
    // The pinned ws (8.18.3) is linked into the binary as uos::rt::ws: there is
    // no npm package to resolve at run time, so the TS checks of the installed
    // package.json export and of the loaded module's two constructors have
    // nothing to inspect, and the load completes without an asynchronous step.
    rt::Task<WebSocketModule> loadInstalledWebSocketModule() {
      WebSocketModule module{rt::ws::WebSocket::constructor(), rt::ws::WebSocketServer::constructor()};
      co_return module;
    }
  }

  rt::Promise<WebSocketModule> loadWebSocketModule() {
    if (!modulePromise) modulePromise.emplace(loadInstalledWebSocketModule());
    return *modulePromise;
  }
}
