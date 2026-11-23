#include "uos/runtime/http_upgrade.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/runtime/websocket.hpp"

#include <exception>
#include <memory>
#include <utility>
#include <vector>

namespace uos::runtime::http_upgrade {
  namespace ws = rt::ws;
  using http::node_http::IncomingEvent;
  using http::node_http::IncomingListener;
  using http::node_http::IncomingMessage;

  namespace {
    rt::Task<void> refuseUpgrade(std::shared_ptr<UpgradeSocket> socket, rt::Response response) {
      auto reading = response.arrayBuffer();
      const rt::Bytes body = co_await std::move(reading);
      if (socket->destroyed()) co_return;
      rt::Headers headers(response.headers());
      headers.set("content-length", rt::numberToString(static_cast<double>(body.size())));
      headers.set("connection", "close");
      std::vector<std::string> lines{"HTTP/1.1 " + std::to_string(response.status()) + " " + (response.statusText().empty() ? std::string("Request Rejected") : response.statusText())};
      for (const auto& [name, value] : headers) {
        if (name != "set-cookie") lines.push_back(name + ": " + value);
      }
      for (const std::string& cookie : headers.getSetCookie()) lines.push_back("set-cookie: " + cookie);
      std::string head;
      for (std::size_t index = 0; index < lines.size(); ++index) head += (index ? "\r\n" : "") + lines[index];
      rt::Bytes bytes = rt::encodeUtf8(head + "\r\n\r\n");
      bytes.insert(bytes.end(), body.begin(), body.end());
      const rt::Promise<void> closing = rt::newPromise<void>([&](const rt::Resolve<void>& resolve, const rt::Reject&) {
        socket->once("close", IncomingListener([resolve](const IncomingEvent&) { resolve(); }));
        socket->end(std::move(bytes));
      });
      co_await closing.wait();
    }

    // `class PayloadLimitedSocket extends webSocketCtor`
    class PayloadLimitedSocket final : public ws::WebSocket {
    public:
      PayloadLimitedSocket(bool autoPong, std::optional<PayloadLimitClose> payloadLimitClose) : ws::WebSocket(nullptr, autoPong), payloadLimitClose_(std::move(payloadLimitClose)) {}

      using ws::WebSocket::close;
      void close(std::optional<int> code, std::optional<ws::Data> reason) override {
        // ws invokes a bare 1009 close for its receiver's maxPayload failure.
        // An explicit peer-propagated close retains its original code/reason.
        if (code == 1009 && !reason && payloadLimitClose_) {
          ws::WebSocket::close(static_cast<int>(payloadLimitClose_->code), ws::Data(payloadLimitClose_->reason));
        } else {
          ws::WebSocket::close(code, std::move(reason));
        }
      }

    private:
      std::optional<PayloadLimitClose> payloadLimitClose_;
    };

    // The state the `{ upgrade, shutdown }` closures share.
    struct Upgrades {
      ServeUpgradeHandler handler;
      RequestAdapter requestAdapter;
      // `const sockets = new Set<WebSocket>()`
      rt::OrderedMap<const ws::WebSocket*, std::shared_ptr<ws::WebSocket>> sockets;
      bool stopping = false;
    };

    ws::UpgradeRequest upgradeRequestOf(const IncomingMessage& incoming) {
      ws::UpgradeRequest request;
      request.method = incoming.method;
      request.url = incoming.url;
      for (std::size_t index = 0; index + 1 < incoming.rawHeaders.size(); index += 2) request.rawHeaders.emplace_back(incoming.rawHeaders[index], incoming.rawHeaders[index + 1]);
      return request;
    }

    // The `try` block of upgrade().
    rt::Task<void> authorizeAndUpgrade(std::shared_ptr<Upgrades> state, std::shared_ptr<IncomingMessage> incoming, std::shared_ptr<UpgradeSocket> socket, rt::Bytes head,
                                       rt::AbortController controller) {
      const rt::Request request = state->requestAdapter(incoming, controller.signal());
      std::optional<UpgradeResult> result;
      if (!state->handler || state->stopping) {
        rt::ResponseInit init;
        init.status = 426;
        result = rt::Response(rt::BodyInit(std::string("WebSocket upgrade unavailable")), std::move(init));
      } else {
        UpgradeInfo info{http::NetAddr{"tcp", incoming->socket()->remoteAddress().value_or(""), incoming->socket()->remotePort().value_or(0)}};
        const rt::Promise<UpgradeResult> handling(state->handler(request, std::move(info)));
        result = co_await handling.wait();
      }
      if (controller.signal().aborted() || socket->destroyed()) co_return;
      if (const rt::Response* refusal = std::get_if<rt::Response>(&*result)) {
        auto refusing = refuseUpgrade(socket, *refusal);
        co_await std::move(refusing);
        co_return;
      }
      std::optional<websocket::WebSocketModule> module;
      if (!state->stopping) {
        const rt::Promise<websocket::WebSocketModule> loading = websocket::loadWebSocketModule();
        auto loaded = co_await loading.wait();
        module.emplace(std::move(loaded));
        if (controller.signal().aborted() || socket->destroyed()) co_return;
      }
      // Even the cached module resumes on a microtask. Check before the synchronous handoff.
      if (state->stopping) {
        rt::ResponseInit init;
        init.status = 503;
        auto refusing = refuseUpgrade(socket, rt::Response(rt::BodyInit(std::string("Server shutting down")), std::move(init)));
        co_await std::move(refusing);
        co_return;
      }
      const WebSocketUpgrade accepted = std::get<WebSocketUpgrade>(*result);
      const std::optional<PayloadLimitClose> payloadLimitClose = accepted.payloadLimitClose;
      ws::ServerOptions options;
      options.noServer = true;
      options.clientTracking = false;
      options.perMessageDeflate = false;
      options.maxPayload = accepted.maxPayloadBytes;
      options.WebSocket = [payloadLimitClose](const ws::ServerOptions& serverOptions) -> std::shared_ptr<ws::WebSocket> {
        return std::make_shared<PayloadLimitedSocket>(serverOptions.autoPong, payloadLimitClose);
      };
      const std::shared_ptr<ws::WebSocketServer> upgradeServer = module->WebSocketServer(options);
      if (accepted.headers) {
        const rt::Headers headersInit = *accepted.headers;
        upgradeServer->once("headers", [headersInit](const ws::EventArgs& event) {
          const rt::Headers additional(headersInit);
          for (const auto& [name, value] : additional) {
            if (name != "set-cookie") event.headers->push_back(name + ": " + value);
          }
          for (const std::string& cookie : additional.getSetCookie()) event.headers->push_back("set-cookie: " + cookie);
        });
      }
      const ws::UpgradeRequest upgradeRequest = upgradeRequestOf(*incoming);
      const std::function<void(std::shared_ptr<ws::WebSocket>)> accept = accepted.accept;
      upgradeServer->handleUpgrade(upgradeRequest, socket->upgradeSocket(), std::move(head), [state, accept](std::shared_ptr<ws::WebSocket> webSocket, const ws::UpgradeRequest&) {
        state->sockets.set(webSocket.get(), webSocket);
        webSocket->on("error", [] {});
        const ws::WebSocket* key = webSocket.get();
        webSocket->once("close", [state, key] { state->sockets.delete_(key); });
        try {
          accept(webSocket);
        } catch (...) {
          webSocket->terminate();
        }
      });
      upgradeServer->close();
    }

    rt::Task<void> upgrade(std::shared_ptr<Upgrades> state, std::shared_ptr<IncomingMessage> incoming, std::shared_ptr<UpgradeSocket> socket, rt::Bytes head) {
      const rt::AbortController controller;
      const IncomingListener closed([controller](const IncomingEvent&) { controller.abort(rt::makeReason(rt::JsError("WebSocket upgrade disconnected"))); });
      socket->once("close", closed);
      socket->once("error", closed);
      rt::Reason failure;
      try {
        auto upgrading = authorizeAndUpgrade(state, incoming, socket, std::move(head), controller);
        co_await std::move(upgrading);
      } catch (...) {
        failure = std::current_exception();
      }
      rt::Reason refusalFailure;
      if (failure && !socket->destroyed()) {
        try {
          rt::ResponseInit init;
          init.status = 500;
          auto refusing = refuseUpgrade(socket, rt::Response(rt::BodyInit(std::string("WebSocket upgrade failed")), std::move(init)));
          co_await std::move(refusing);
        } catch (...) {
          refusalFailure = std::current_exception();
        }
      }
      socket->removeListener("close", closed);
      socket->removeListener("error", closed);
      if (refusalFailure) std::rethrow_exception(refusalFailure);
    }

    rt::Task<void> shutdown(std::shared_ptr<Upgrades> state) {
      state->stopping = true;
      std::vector<rt::Promise<void>> closing;
      for (const std::shared_ptr<ws::WebSocket>& socket : rt::arrayFrom(state->sockets.values())) {
        closing.push_back(rt::newPromise<void>([&socket](const rt::Resolve<void>& resolve, const rt::Reject&) {
          const std::weak_ptr<ws::WebSocket> weak = socket;
          const rt::Timeout timer = rt::setTimeout(
            [weak] {
              if (auto target = weak.lock()) target->terminate();
            },
            10'000);
          socket->once("close", [timer, resolve] {
            rt::clearTimeout(timer);
            resolve();
          });
          socket->close(1001, ws::Data(std::string("Server shutting down")));
        }));
      }
      const auto all = rt::all(std::move(closing));
      co_await all.wait();
    }
  }

  WebSocketUpgrades createWebSocketUpgrades(ServeUpgradeHandler handler, RequestAdapter requestAdapter) {
    auto state = std::make_shared<Upgrades>();
    state->handler = std::move(handler);
    state->requestAdapter = std::move(requestAdapter);
    WebSocketUpgrades upgrades;
    upgrades.upgrade = [state](std::shared_ptr<IncomingMessage> incoming, std::shared_ptr<UpgradeSocket> socket, rt::Bytes head) {
      return upgrade(state, std::move(incoming), std::move(socket), std::move(head));
    };
    upgrades.shutdown = [state] { return shutdown(state); };
    return upgrades;
  }
}
