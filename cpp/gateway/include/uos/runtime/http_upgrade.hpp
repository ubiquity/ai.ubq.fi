// src/runtime/http-upgrade.ts: WebSocket upgrades of the HTTP listener.
//
// http.ts and http-upgrade.ts import each other. http.hpp declares NetAddr and
// the node:http types and then includes this header; this header includes
// http.hpp first and keeps its declarations behind a classic include guard
// (not #pragma once), so including either header first sees both complete.
#include "uos/runtime/http.hpp"

#ifndef UOS_RUNTIME_HTTP_UPGRADE_HPP
#define UOS_RUNTIME_HTTP_UPGRADE_HPP

#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/websocket.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace uos::runtime::http {
  struct TransportRequestRejection;
}

namespace uos::runtime::http_upgrade {
  // Readonly<{ code: number; reason: string }>
  struct PayloadLimitClose {
    double code = 0;
    std::string reason;
  };

  // export type WebSocketUpgrade = Readonly<{ accept; maxPayloadBytes; headers?; payloadLimitClose? }>;
  struct WebSocketUpgrade {
    std::function<void(std::shared_ptr<rt::ws::WebSocket> socket)> accept;
    double maxPayloadBytes = 0;
    std::optional<rt::Headers> headers{};
    std::optional<PayloadLimitClose> payloadLimitClose{};
  };

  // Readonly<{ remoteAddr: NetAddr }>
  struct UpgradeInfo {
    http::NetAddr remoteAddr;
  };

  // Response | WebSocketUpgrade
  using UpgradeResult = std::variant<rt::Response, WebSocketUpgrade>;

  // export type ServeUpgradeHandler = (request, info) => Response | WebSocketUpgrade | Promise<Response | WebSocketUpgrade>;
  // An empty std::function is undefined.
  using ServeUpgradeHandler = std::function<rt::Task<UpgradeResult>(rt::Request request, UpgradeInfo info)>;

  // type UpgradeSocket = Duplex & { remoteAddress?: string; remotePort?: number };
  using UpgradeSocket = http::node_http::Socket;

  // type RequestAdapter = (incoming: IncomingMessage, signal: AbortSignal) => Request;
  using RequestAdapter = std::function<rt::Request(std::shared_ptr<http::node_http::IncomingMessage> incoming, rt::AbortSignal signal)>;

  // The `{ upgrade, shutdown }` object createWebSocketUpgrades returns.
  struct WebSocketUpgrades {
    std::function<rt::Task<void>(std::shared_ptr<http::node_http::IncomingMessage> incoming, std::shared_ptr<UpgradeSocket> socket, rt::Bytes head)> upgrade;
    std::function<rt::Task<void>()> shutdown;
  };

  /** Upgrade authorization finishes before ws writes the switching-protocols reply. */
  WebSocketUpgrades createWebSocketUpgrades(ServeUpgradeHandler handler, RequestAdapter requestAdapter,
    std::function<void(const http::TransportRequestRejection&)> onTransportRejection = {});
}

#endif
