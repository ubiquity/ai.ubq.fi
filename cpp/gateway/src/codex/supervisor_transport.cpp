#include "uos/codex/supervisor_transport.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/runtime/http.hpp"
#include "uos/upstream_auth.hpp"

#include <memory>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace uos::codex::supervisor_transport {
  namespace ws = rt::ws;

  namespace {
    /**
     * Read-only JSON-RPC transport for a Codex app-server control socket.
     *
     * The method allowlist is enforced here, in code, so a caller cannot reach a
     * mutating method even by mistake. `initialize` plus the `initialized`
     * notification is the only handshake; nothing else is ever sent.
     */
    const std::vector<std::string> READ_ONLY_METHODS{"initialize", "thread/list", "thread/loaded/list", "thread/read", "thread/turns/list", "account/rateLimits/read"};

    const std::unordered_set<std::string> READ_ONLY_METHOD_SET(READ_ONLY_METHODS.begin(), READ_ONLY_METHODS.end());

    /** Bounded frame size; a larger frame closes the socket instead of growing memory. */
    constexpr double SUPERVISOR_MAX_PAYLOAD_BYTES = 1'048'576;

    constexpr double HANDSHAKE_TIMEOUT_MS = 3'000;
    constexpr double DEFAULT_CALL_TIMEOUT_MS = 4'000;
    constexpr double CLOSE_GRACE_MS = 250;

    struct PendingCall {
      rt::Resolve<SupervisorCallResult> resolve;
      rt::Reject reject;
      rt::Timeout timer;
      std::function<void()> cleanup;
    };

    /** `ws` readyState values (CONNECTING 0, OPEN 1, CLOSING 2, CLOSED 3). */
    constexpr int WS_READY_STATE_OPEN = 1;
    constexpr int WS_READY_STATE_CLOSED = 3;

    // `let webSocketConstructor: SupervisorWebSocketConstructor | null`: empty is null.
    ws::WebSocketConstructor webSocketConstructor;

    /**
     * Loads `ws` on first use.
     *
     * Importing it at module scope evaluates `ws/lib/buffer-util.js`, which reads
     * `WS_NO_BUFFER_UTIL` from the environment while the module is being evaluated;
     * repository tests import the handler under a strict env allowlist, so the
     * runtime load belongs inside the connection path that actually needs it. The
     * shared runtime loader resolves the installed pinned package without Bun replacement.
     */
    rt::Task<ws::WebSocketConstructor> loadWebSocketConstructor() {
      if (webSocketConstructor) co_return webSocketConstructor;
      const rt::Promise<runtime::websocket::WebSocketModule> loading = runtime::http::loadWebSocketModule();
      const runtime::websocket::WebSocketModule module = co_await loading.wait();
      // `isRecord(module) ? module.default : undefined`: the module is always an object.
      const ws::WebSocketConstructor candidate = module.default_;
      if (!candidate) throw rt::JsError("ws module did not expose a WebSocket constructor");
      webSocketConstructor = candidate;
      co_return candidate;
    }

    /** Message payloads arrive as Buffer, string, or Buffer[]; only decoded text carries JSON. */
    // rt::ws delivers a Buffer (Bytes) or, from a test double, a string: its binaryType is
    // always "nodebuffer", so the Buffer[] and other payload shapes cannot occur.
    std::string textOf(const ws::Data& raw) {
      if (const std::string* text = std::get_if<std::string>(&raw)) return *text;
      return rt::decodeUtf8(std::get<ws::Bytes>(raw));
    }

    // The state the TS closures share.
    struct ConnectionState : std::enable_shared_from_this<ConnectionState> {
      std::shared_ptr<ws::WebSocket> socket;
      rt::OrderedMap<double, PendingCall> pending;
      double serial = 0;
      bool closed = false;

      ~ConnectionState() {
        for (const auto& call : pending.values()) call.cleanup();
      }

      void settle(double id, rt::Reason error, SupervisorCallResult value = std::nullopt) {
        PendingCall* found = pending.get(id);
        if (!found) return;
        PendingCall call = *found;
        pending.delete_(id);
        rt::clearTimeout(call.timer);
        call.cleanup();
        if (error) call.reject(error);
        else call.resolve(std::move(value));
      }

      void failPending(const std::string& message) {
        for (const double id : rt::arrayFrom(pending.keys())) settle(id, rt::makeReason(rt::JsError(message)));
      }

      void handleMessage(const ws::Data& raw) {
        const std::string text = textOf(raw);
        if (text.empty()) return;
        json::value parsed;
        try {
          parsed = rt::jsonParse(text);
        } catch (...) {
          return;
        }
        if (!rt::isRecord(parsed)) return;
        const json::value* id = rt::get(parsed, "id");
        if (id == nullptr || !rt::isNumber(*id) || !pending.has(rt::toNumber(*id))) return;
        const double callId = rt::toNumber(*id);
        const json::value* error = rt::get(parsed, "error");
        if (rt::isRecord(error)) {
          const std::optional<std::string_view> described = rt::getString(*error, "message");
          const std::string message = described ? std::string(*described) : "app-server returned an error";
          settle(callId, rt::makeReason(rt::JsError(rt::slice(message, 0, 400))));
          return;
        }
        const json::value* result = rt::get(parsed, "result");
        settle(callId, nullptr, result ? SupervisorCallResult(*result) : std::nullopt);
      }

      rt::Promise<SupervisorCallResult> send(const std::string& method, const json::object& params, const std::optional<rt::AbortSignal>& callSignal) {
        if (socket->readyState() != WS_READY_STATE_OPEN) return rt::Promise<SupervisorCallResult>::reject(rt::makeReason(rt::JsError("app-server connection unavailable")));
        if (callSignal && callSignal->aborted()) return rt::Promise<SupervisorCallResult>::reject(rt::makeReason(rt::JsError("app-server call aborted")));
        serial += 1;
        const double id = serial;
        auto self = shared_from_this();
        return rt::newPromise<SupervisorCallResult>([&](const rt::Resolve<SupervisorCallResult>& resolve, const rt::Reject& reject) {
          const rt::EventListener onAbort([weak = weak_from_this(), id] {
            if (const auto state = weak.lock()) state->settle(id, rt::makeReason(rt::JsError("app-server call aborted")));
          });
          const rt::Timeout timer = rt::setTimeout([self, id] { self->settle(id, rt::makeReason(rt::JsError("app-server call timed out"))); }, DEFAULT_CALL_TIMEOUT_MS);
          std::function<void()> cleanup = [callSignal, onAbort] {
            if (callSignal) callSignal->removeEventListener("abort", onAbort);
          };
          pending.set(id, PendingCall{resolve, reject, timer, cleanup});
          if (callSignal) callSignal->addEventListener("abort", onAbort, {.once = true});
          try {
            json::object frame;
            frame["id"] = rt::makeNumber(id);
            frame["method"] = method;
            frame["params"] = params;
            socket->send(rt::jsonStringify(json::value(std::move(frame))));
          } catch (...) {
            settle(id, rt::makeReason(rt::JsError("app-server send failed")));
          }
        });
      }
    };

    rt::Task<SupervisorCallResult> callConnection(std::shared_ptr<ConnectionState> state, SupervisorMethod method, json::object params, std::optional<rt::AbortSignal> callSignal) {
      if (!READ_ONLY_METHOD_SET.contains(method)) throw rt::JsError("supervisor transport refused a non-read-only method");
      const rt::Promise<SupervisorCallResult> sent = state->send(method, params, callSignal);
      SupervisorCallResult result = co_await sent.wait();
      co_return result;
    }

    rt::Task<void> closeConnection(std::shared_ptr<ConnectionState> state) {
      if (state->closed) co_return;
      state->closed = true;
      state->failPending("app-server connection closed");
      if (state->socket->readyState() == WS_READY_STATE_CLOSED) co_return;
      try {
        state->socket->close(1000, ws::Data(std::string("supervisor sample complete")));
      } catch (...) {
        // The close frame is best effort; the grace period below is the guarantee.
      }
      co_await rt::sleep(CLOSE_GRACE_MS);
      try {
        // `terminate()` is a no-op on an already closed socket, so no second
        // readyState read is needed after the grace period.
        state->socket->terminate();
      } catch (...) {
        // A socket that cannot be terminated is already unusable.
      }
    }

    rt::Task<SupervisorConnection> openConnection(std::string socketPath, std::optional<rt::AbortSignal> signal) {
      // The app-server can use its own auth manager even for a read-only RPC.
      if (upstream_auth::followerMode()) throw rt::JsError("Codex app-server access is disabled by UOS_GATEWAY_UPSTREAM_AUTH_FOLLOWER=1.");
      if (signal && signal->aborted()) throw rt::JsError("app-server connection aborted");
      const rt::Promise<ws::WebSocketConstructor> loading(loadWebSocketConstructor());
      const ws::WebSocketConstructor constructor = co_await loading.wait();
      ws::ClientOptions options;
      options.headers = {{"Host", "localhost"}};
      options.perMessageDeflate = false;
      options.maxPayload = SUPERVISOR_MAX_PAYLOAD_BYTES;
      options.handshakeTimeout = HANDSHAKE_TIMEOUT_MS;
      auto state = std::make_shared<ConnectionState>();
      state->socket = constructor("ws+unix://" + socketPath + ":/", options);

      // Connections and pending calls own the state; socket listeners must not own their socket through it.
      const std::weak_ptr<ConnectionState> weak = state;
      state->socket->on("message", [weak](const ws::EventArgs& event) {
        if (auto live = weak.lock()) live->handleMessage(event.data);
      });
      state->socket->on("close", [weak] {
        if (auto live = weak.lock()) live->failPending("app-server connection closed");
      });
      state->socket->on("error", [weak] {
        if (auto live = weak.lock()) live->failPending("app-server connection failed");
      });

      const rt::Promise<void> opening = rt::newPromise<void>([&](const rt::Resolve<void>& resolve, const rt::Reject& reject) {
        auto opened = std::make_shared<bool>(false);
        state->socket->once("open", [opened, resolve] {
          if (*opened) return;
          *opened = true;
          resolve();
        });
        state->socket->once("error", [opened, reject] {
          if (*opened) return;
          *opened = true;
          reject(rt::makeReason(rt::JsError("app-server socket unavailable")));
        });
      });
      co_await opening.wait();

      rt::Reason failure;
      try {
        json::object clientInfo;
        clientInfo["name"] = "uos_supervisor";
        clientInfo["version"] = "1";
        json::object capabilities;
        capabilities["experimentalApi"] = true;
        json::object params;
        params["clientInfo"] = std::move(clientInfo);
        params["capabilities"] = std::move(capabilities);
        const std::optional<rt::AbortSignal> noSignal;
        const rt::Promise<SupervisorCallResult> initializing = state->send("initialize", params, noSignal);
        co_await initializing.wait();
        json::object initialized;
        initialized["method"] = "initialized";
        state->socket->send(rt::jsonStringify(json::value(std::move(initialized))));
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) {
        // The caller never receives a connection when the handshake fails, so this
        // exact socket is cleaned up here: pending callbacks and their timers
        // settle, the socket is terminated, and the original failure is rethrown so
        // the caller can report it and attempt a fresh connection later.
        state->closed = true;
        state->failPending("app-server connection closed");
        try {
          state->socket->terminate();
        } catch (...) {
          // A socket that cannot be terminated is already unusable.
        }
        std::rethrow_exception(failure);
      }

      SupervisorConnection connection;
      connection.call = [state](SupervisorMethod method, json::object params, std::optional<rt::AbortSignal> callSignal) {
        return callConnection(state, std::move(method), std::move(params), std::move(callSignal));
      };
      connection.close = [state] { return closeConnection(state); };
      co_return connection;
    }
  }

  void setSupervisorWebSocketConstructorForTest(ws::WebSocketConstructor candidate) { webSocketConstructor = candidate ? std::move(candidate) : ws::WebSocketConstructor(); }

  rt::Task<SupervisorConnection> openSupervisorConnection(std::string socketPath) { return openConnection(std::move(socketPath), std::nullopt); }

  rt::Task<SupervisorConnection> openSupervisorConnection(std::string socketPath, std::optional<rt::AbortSignal> signal) {
    return openConnection(std::move(socketPath), std::move(signal));
  }
}
