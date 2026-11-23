#pragma once
// src/runtime/http.ts: the gateway's HTTP/1.1 listener over node:http, with
// the node:http server it runs on (rt provides none).
//
// src/runtime/http.ts and src/runtime/http-upgrade.ts import each other
// (`createWebSocketUpgrades` one way, `NetAddr` and the node:http types the
// other). This header declares those types first and only then includes
// http_upgrade.hpp, which includes this header before its own include guard, so
// either header may be included first.
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/websocket.hpp"
#include "uos/runtime/http_body.hpp"
#include "uos/runtime/http_early_sse.hpp"
#include "uos/runtime/http_tasks.hpp"
#include "uos/runtime/websocket.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::runtime::http {
  // ---- `import { createServer, type IncomingMessage, type ServerResponse } from "node:http"` ----
  // node:http's server as Bun 1.4 runs it, for this module and http-upgrade.ts:
  // HTTP/1.1 requests on TCP connections (Bun's parser rules: the known request
  // methods, a 16 KiB head and 198 header fields, Content-Length or chunked
  // bodies, `Expect: 100-continue`, pipelining, keep-alive with a 5 s idle
  // timeout), responses framed and headed as Bun writes them (user headers in
  // setHeader order, then Date, Connection and Keep-Alive, then Content-Length
  // or Transfer-Encoding), backpressure through write()/"drain", and the
  // "upgrade" event with the socket handed over to rt::ws. The wire behaviour
  // was recorded from Bun 1.4.0 (tests/runtime_http_differential_test.cpp).
  namespace node_http {
    using http_body::IncomingEvent;
    using http_body::IncomingListener;

    struct ConnectionState;
    class Server;

    // node:events emitter of the node:http objects: "data" carries a
    // rt::ByteChunk, "error" a rt::Reason, every other event nothing. emit()
    // calls the listeners registered when it starts, a once listener is removed
    // before its call, removeListener() removes the most recently added
    // registration of exactly that listener.
    class Emitter {
    public:
      void on(std::string_view event, const IncomingListener& listener);
      void once(std::string_view event, const IncomingListener& listener);
      void removeListener(std::string_view event, const IncomingListener& listener);
      std::size_t listenerCount(std::string_view event) const;
      // True when there were listeners.
      bool emit(std::string_view event, const IncomingEvent& value = IncomingEvent());

    private:
      struct Registration {
        std::string event;
        IncomingListener listener;
        bool once = false;
        std::shared_ptr<bool> fired;
      };
      std::vector<Registration> registrations_;
    };

    // node:net's Socket as node:http hands it out: `incoming.socket` and the
    // socket of the "upgrade" event. Events: "close" (once the connection is
    // closed) and "error".
    class Socket : public std::enable_shared_from_this<Socket> {
    public:
      explicit Socket(std::shared_ptr<ConnectionState> connection);
      // `socket.remoteAddress` / `socket.remotePort` / `socket.remoteFamily`
      // (std::nullopt where node:net has undefined: after the socket closed).
      std::optional<std::string> remoteAddress() const;
      std::optional<double> remotePort() const;
      std::optional<std::string> remoteFamily() const;
      // `socket.destroyed`
      bool destroyed() const;
      // `socket.end(data)`: writes `data`, ends the writable side and waits for
      // the peer to end its side; "close" follows. Only for an "upgrade" socket
      // still held by its listener.
      void end(rt::Bytes data);
      void end();
      // `socket.destroy()`
      void destroy();
      void on(std::string_view event, const IncomingListener& listener);
      void once(std::string_view event, const IncomingListener& listener);
      void removeListener(std::string_view event, const IncomingListener& listener);

      // The connection of an "upgrade" socket as ws's handleUpgrade takes it
      // (no JS equivalent: node:http hands ws the socket object itself). ws
      // moves the stream out when it answers; the socket is then ws's.
      rt::ws::UpgradeSocket& upgradeSocket();

      const std::shared_ptr<ConnectionState>& connection() const noexcept { return connection_; }

    private:
      std::shared_ptr<ConnectionState> connection_;
    };

    // `IncomingMessage`: one request as the server received it. Body bytes are
    // read from the connection only while the message flows (resume()), so a
    // paused message applies backpressure to the client.
    class IncomingMessage final : public http_body::IncomingMessage, public std::enable_shared_from_this<IncomingMessage> {
    public:
      IncomingMessage(std::shared_ptr<ConnectionState> connection, std::shared_ptr<Socket> socket);

      // `incoming.method`, `incoming.url` (the request target as received, UTF-8
      // decoded), `incoming.httpVersion` ("1.1", "1.0").
      std::string method;
      std::string url;
      std::string httpVersion;
      // `incoming.rawHeaders`: name, value, name, value, ... in wire order and
      // case; each value byte b is the code point U+00b (Latin-1).
      std::vector<std::string> rawHeaders;
      // `incoming.headers[name]` for a lower-case name, node:http's join rules
      // (repeated fields joined with ", ", "cookie" with "; ", the first value
      // kept for host, authorization and the other single-value fields).
      std::optional<std::string> header(std::string_view name) const;
      // `incoming.socket`
      const std::shared_ptr<Socket>& socket() const noexcept { return socket_; }
      // `incoming.complete`: the whole body was received.
      bool complete() const;

      // http_body::IncomingMessage: flowing-mode control ("data" events flow
      // after resume(), on a later microtask), listeners, readableEnded ("end"
      // was emitted). Events: "data", "end", "aborted" (the connection closed
      // before the body was complete), "error" (only emitted to listeners),
      // "close".
      void pause() override;
      void resume() override;
      void on(std::string_view event, const IncomingListener& listener) override;
      void once(std::string_view event, const IncomingListener& listener) override;
      void removeListener(std::string_view event, const IncomingListener& listener) override;
      bool readableEnded() const override;

      Emitter& events() noexcept { return events_; }
      const std::shared_ptr<ConnectionState>& connection() const noexcept { return connection_; }

    private:
      friend struct ConnectionState;
      std::shared_ptr<ConnectionState> connection_;
      std::shared_ptr<Socket> socket_;
      Emitter events_;
      bool complete_ = false;
      bool readableEnded_ = false;
    };

    // `ServerResponse`. Events: "finish" (the last byte was handed to the
    // kernel), "close" (after "finish", or when the connection closed first),
    // "drain", "error".
    class ServerResponse final : public std::enable_shared_from_this<ServerResponse> {
    public:
      explicit ServerResponse(std::shared_ptr<ConnectionState> connection);

      // `res.statusCode`, `res.statusMessage` ("" writes the code's own reason).
      int statusCode = 200;
      std::string statusMessage;
      // `res.shouldKeepAlive`: false closes the connection after this response.
      bool shouldKeepAlive = true;

      // `res.setHeader(name, value)` / `res.setHeader(name, [values])`: names
      // keep the case of the latest call and the position of the first;
      // TypeError ERR_INVALID_HTTP_TOKEN / ERR_INVALID_CHAR for an invalid name
      // or value, Error ERR_HTTP_HEADERS_SENT once the head was written.
      void setHeader(std::string_view name, std::string_view value);
      void setHeader(std::string_view name, std::vector<std::string> values);
      // `res.getHeader(name)`: the values (one for a single value).
      std::optional<std::vector<std::string>> getHeader(std::string_view name) const;
      // `res.flushHeaders()`: writes the head now.
      void flushHeaders();
      // `res.headersSent`
      bool headersSent() const;
      // `res.write(chunk)`: false once 64 KiB or more wait for the kernel
      // ("drain" follows). An empty chunk writes nothing (not even the head).
      bool write(std::span<const std::uint8_t> chunk);
      bool write(std::string_view text);
      // `res.end()` / `res.end(data)`: data given before the head was written
      // becomes its Content-Length.
      void end();
      void end(std::span<const std::uint8_t> data);
      void end(std::string_view text);
      // `res.destroy()`: closes the connection at once.
      void destroy();
      // `res.destroyed`, `res.writableEnded`, `res.writableFinished`, `res.writableLength`.
      bool destroyed() const;
      bool writableEnded() const;
      bool writableFinished() const;
      double writableLength() const;
      void on(std::string_view event, const IncomingListener& listener);
      void once(std::string_view event, const IncomingListener& listener);
      void removeListener(std::string_view event, const IncomingListener& listener);

      Emitter& events() noexcept { return events_; }

      // Response state the connection reads (no JS equivalent).
      struct Head;
      Head& head() noexcept { return *head_; }

    private:
      std::shared_ptr<ConnectionState> connection_;
      std::shared_ptr<Head> head_;
      Emitter events_;
    };

    // `server.address()` of a TCP listener: { address, family, port }.
    struct AddressInfo {
      std::string address;
      std::string family;
      double port = 0;
      friend bool operator==(const AddressInfo&, const AddressInfo&) = default;
    };

    // What a server event hands its listener: "upgrade" (incoming, socket,
    // head), "error" (error), "close" and "listening" (nothing).
    struct ServerEventArgs {
      std::shared_ptr<IncomingMessage> incoming;
      std::shared_ptr<Socket> socket;
      rt::Bytes head;
      rt::Reason error;
    };
    using ServerListener = std::function<void(const ServerEventArgs& args)>;

    // `createServer((incoming, outgoing) => ...)`'s listener.
    using RequestListener = std::function<void(std::shared_ptr<IncomingMessage> incoming, std::shared_ptr<ServerResponse> outgoing)>;

    // `http.Server`. Create it with createServer().
    class Server : public std::enable_shared_from_this<Server> {
    public:
      explicit Server(RequestListener listener);
      ~Server();
      Server(const Server&) = delete;
      Server& operator=(const Server&) = delete;

      // `server.timeout`: a connection with a request in progress and no
      // socket activity for this long is destroyed (0: never).
      double timeout = 0;
      // `server.requestTimeout`: a request whose head and body did not arrive
      // within this long gets 408 and the connection closes (0: never).
      double requestTimeout = 300000;
      // `server.headersTimeout`: the same for the head alone.
      double headersTimeout = 60000;
      // `server.keepAliveTimeout`: an idle keep-alive connection closes after this long.
      double keepAliveTimeout = 5000;

      // `server.on(event, listener)` / `server.once(event, listener)` for
      // "upgrade", "error", "close" and "listening". No event can follow
      // "close", so the server then drops its listeners (the request listener
      // too) and whatever they hold.
      void on(std::string_view event, ServerListener listener);
      void once(std::string_view event, ServerListener listener);
      // `server.listen(port, hostname, callback)`: binds a TCP listener (a
      // numeric address; "localhost" is ::1 and "" the dual-stack "::" as Bun
      // binds them; another name fails with ENXIO). Failures are "error"
      // events: RangeError ERR_OUT_OF_RANGE for a bad port, Error EADDRINUSE
      // "Failed to start server. Is port <n> in use?", ...
      void listen(double port, std::string hostname, std::function<void()> callback);
      // `server.address()`: std::nullopt before listening and after close().
      std::optional<AddressInfo> address() const;
      // `server.close()`: stops accepting and closes idle connections; "close"
      // follows once every connection is gone.
      void close();
      // `server.closeIdleConnections()`: closes connections without a response in progress.
      void closeIdleConnections();
      // `server.closeAllConnections()`: closes every HTTP connection.
      void closeAllConnections();
      // `server.ref()` / `server.unref()`. rt has no unreferenced IO (every
      // pending Asio operation keeps the loop running), so an unreferenced
      // listener still keeps rt::Loop::run alive; the flag is only reported.
      void ref();
      void unref();
      bool hasRef() const;

      // Implementation state (src/runtime/http.cpp).
      struct Impl;
      Impl& impl() noexcept { return *impl_; }

    private:
      std::unique_ptr<Impl> impl_;
    };

    // `createServer(listener)`
    std::shared_ptr<Server> createServer(RequestListener listener);

    // node:http's `STATUS_CODES` as Bun writes the reason phrase of a status
    // line ("HM" for a code it does not know).
    std::string_view reasonPhrase(int statusCode);
  }

  // export type NetAddr = Readonly<{ transport: "tcp" | "udp"; hostname: string; port: number }>;
  struct NetAddr {
    std::string transport;  // "tcp" | "udp"
    std::string hostname;
    double port = 0;
    friend bool operator==(const NetAddr&, const NetAddr&) = default;
  };

  // The `Readonly<{ transport: "unix"; path: string }>` member of Addr.
  struct AddrUnix {
    static constexpr std::string_view transport = "unix";
    std::string path;
    friend bool operator==(const AddrUnix&, const AddrUnix&) = default;
  };

  // export type Addr = NetAddr | Readonly<{ transport: "unix"; path: string }>;
  using Addr = std::variant<NetAddr, AddrUnix>;
}

#include "uos/runtime/http_upgrade.hpp"

namespace uos::runtime::http {
  // export { earlySseHeadersCommitted, tryCommitEarlySseHeaders } from "./http-early-sse.ts";
  using http_early_sse::earlySseHeadersCommitted;
  using http_early_sse::tryCommitEarlySseHeaders;
  // export { trackRequestTask } from "./http-tasks.ts";
  using http_tasks::trackRequestTask;
  // export { loadWebSocketModule } from "./websocket.ts";
  using websocket::loadWebSocketModule;
  // export type { ServeUpgradeHandler, WebSocketUpgrade } from "./http-upgrade.ts";
  using http_upgrade::ServeUpgradeHandler;
  using http_upgrade::WebSocketUpgrade;

  // export type ServeHandlerInfo<TAddress extends Addr = Addr> = Readonly<{ remoteAddr: TAddress; completed: Promise<void> }>;
  struct ServeHandlerInfo {
    Addr remoteAddr;
    rt::Promise<void> completed;
  };

  // export type ServeHandler = (request: Request, info: ServeHandlerInfo) => Response | Promise<Response>;
  // A handler returning a Response at once returns an already completed Task.
  using ServeHandler = std::function<rt::Task<rt::Response>(rt::Request request, ServeHandlerInfo info)>;

  // export type ServeDefaultExport = Readonly<{ fetch: ServeHandler; onListen?: (address: NetAddr) => void; upgrade?: ServeUpgradeHandler }>;
  // An empty std::function is undefined.
  struct ServeDefaultExport {
    ServeHandler fetch;
    std::function<void(const NetAddr& address)> onListen{};
    ServeUpgradeHandler upgrade{};
  };

  // export type ServeHttpOptions = Readonly<{ hostname?; port?; onListen?; signal?; upgrade? }>;
  // An empty std::function is undefined.
  struct ServeHttpOptions {
    std::optional<std::string> hostname{};
    std::optional<double> port{};
    std::function<void(const NetAddr& address)> onListen{};
    std::optional<rt::AbortSignal> signal{};
    ServeUpgradeHandler upgrade{};
    // Let the gateway format its usual Chat rejection for a refused Content-Length.
    bool applicationChatBodyRejection = false;
  };

  // export type HttpServer = Readonly<{ addr: NetAddr; ready; finished; shutdown; ref; unref }>;
  // `addr` is a getter: `server.addr()`.
  struct HttpServer {
    std::function<NetAddr()> addr;
    rt::Promise<NetAddr> ready;
    rt::Promise<void> finished;
    std::function<rt::Promise<void>()> shutdown;
    std::function<void()> ref;
    std::function<void()> unref;
  };

  // export function serveHttp(handler: ServeHandler): HttpServer;
  // export function serveHttp(options: ServeHttpOptions, handler: ServeHandler): HttpServer;
  // An empty handler throws TypeError "An HTTP request handler is required".
  HttpServer serveHttp(ServeHandler handler);
  HttpServer serveHttp(ServeHttpOptions options, ServeHandler handler);
}
