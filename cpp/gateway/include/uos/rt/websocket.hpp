#pragma once
// The `ws` npm package (8.18.3, the version the gateway pins) on the rt loop, as
// the gateway uses it: an RFC 6455 client for `ws:`, `wss:` and
// `ws+unix:<socket>:<path>` URLs, and the server side of an HTTP upgrade
// (`new WebSocketServer({ noServer: true })` + `handleUpgrade`). Beast supplies
// wire primitives. The adapter supplies the gateway's socket and event API.
//
// Objects are JS-like handles: a WebSocket or WebSocketServer always lives in a
// std::shared_ptr (clients come from WebSocket::create or the constructor()
// value, server sockets from WebSocketServer, a WebSocketServer from
// std::make_shared). An open connection keeps its WebSocket alive, as the event
// loop does in JS.
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include <boost/asio/generic/stream_protocol.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace uos::rt::ws {
  // `Buffer` contents.
  using Bytes = std::vector<std::uint8_t>;

  class WebSocket;
  class WebSocketServer;
  struct UpgradeRequest;

  // A payload as a listener receives it. ws hands every frame read from the
  // wire over as a Buffer (Bytes), text messages included; a string appears
  // where JS has one: `MessageEvent.data` of a text message, a string passed to
  // send(), or what a test double emits.
  using Data = std::variant<Bytes, std::string>;

  // An Error or RangeError with Node's extra properties: ws's frame errors
  // (`code` "WS_ERR_*"), system errors (`connect ECONNREFUSED 127.0.0.1:1`:
  // code, errno, syscall), llhttp parse errors ("HPE_*"), TLS errors
  // ("DEPTH_ZERO_SELF_SIGNED_CERT", "ERR_TLS_CERT_ALTNAME_INVALID") and
  // header validation (TypeError "ERR_INVALID_HTTP_TOKEN"). An empty `code`
  // is undefined.
  struct CodedError : JsError {
    std::string code;
    std::optional<double> errno_;
    std::optional<std::string> syscall;
    CodedError(std::string message_, std::string code_, std::string name_ = "Error") : JsError(std::move(message_), std::move(name_)), code(std::move(code_)) {}
  };

  // node:http's IncomingMessage for the handshake response, what the "upgrade"
  // and "unexpected-response" listeners receive.
  struct IncomingResponse {
    int statusCode = 0;
    std::string statusMessage;
    std::string httpVersion;
    // `response.rawHeaders` as name/value pairs: wire order and case, each byte
    // as the code point U+00xx (Latin-1).
    std::vector<std::pair<std::string, std::string>> rawHeaders;
    // `response.headers[name]` for a lower-case name, with node:http's rules:
    // repeated headers joined by ", " ("cookie" by "; "), the first value kept
    // for host, authorization, content-type, content-length, location and the
    // other single-value fields; std::nullopt when absent. Set-Cookie values
    // are joined by ", " here (JS has an array: use rawHeaders).
    std::optional<std::string> header(std::string_view name) const;
    // `response.resume()`. The body of a refused handshake is always read and
    // discarded, so resuming has no further effect.
    void resume() const noexcept {}
  };

  // The arguments of one `emit(event, ...args)`; each event sets its own:
  //   "open"                 -
  //   "message"              data (Bytes), isBinary
  //   "ping", "pong"         data (Bytes)
  //   "close"                code, reason (Bytes)
  //   "error"                error
  //   "upgrade"              response (client)
  //   "unexpected-response"  response (client; ws's first argument, the
  //                          ClientRequest, is not modelled)
  //   "headers"              headers (WebSocketServer: the response lines,
  //                          which the listener may change), request
  //   "close" (server)       -
  struct EventArgs {
    Data data;
    bool isBinary = false;
    int code = 0;
    Bytes reason;
    Reason error;
    std::shared_ptr<const IncomingResponse> response;
    std::vector<std::string>* headers = nullptr;
    const UpgradeRequest* request = nullptr;
  };

  // A node:events listener with identity, the C++ form of a named JS function:
  // removeListener() removes exactly the listener that was added, copies share
  // the identity, and two listeners made from separate lambdas differ. Callable
  // as `(const EventArgs&)` or `()` (JS listeners ignore extra arguments).
  class Listener {
  public:
    Listener() = default;
    template <class F>
      requires(!std::is_same_v<std::decay_t<F>, Listener> &&
               (std::is_invocable_v<std::decay_t<F>&, const EventArgs&> || std::is_invocable_v<std::decay_t<F>&>))
    Listener(F&& callback) {
      using Callback = std::decay_t<F>;
      if constexpr (std::is_invocable_v<Callback&, const EventArgs&>) callback_ = std::make_shared<std::function<void(const EventArgs&)>>(std::forward<F>(callback));
      else callback_ = std::make_shared<std::function<void(const EventArgs&)>>([inner = Callback(std::forward<F>(callback))](const EventArgs&) mutable { inner(); });
    }
    void operator()(const EventArgs& args) const { (*callback_)(args); }
    explicit operator bool() const noexcept { return callback_ != nullptr; }
    friend bool operator==(const Listener& left, const Listener& right) noexcept { return left.callback_ == right.callback_; }

  private:
    std::shared_ptr<std::function<void(const EventArgs&)>> callback_;
  };

  // The node:events EventEmitter surface ws inherits. Every method is virtual
  // so a test double can stand in for a WebSocket.
  class EventEmitter {
  public:
    EventEmitter() = default;
    EventEmitter(const EventEmitter&) = delete;
    EventEmitter& operator=(const EventEmitter&) = delete;
    virtual ~EventEmitter() = default;

    // `emitter.on(event, listener)` / `addListener`: appended; the same
    // listener added twice is called twice.
    virtual EventEmitter& on(std::string_view event, Listener listener);
    EventEmitter& addListener(std::string_view event, Listener listener) { return on(event, std::move(listener)); }
    // `emitter.once(event, listener)`: removed just before its first call.
    virtual EventEmitter& once(std::string_view event, Listener listener);
    // `emitter.removeListener(event, listener)` / `off`: removes the most
    // recently added registration of exactly this listener (once or not).
    virtual EventEmitter& removeListener(std::string_view event, const Listener& listener);
    EventEmitter& off(std::string_view event, const Listener& listener) { return removeListener(event, listener); }
    // `emitter.removeAllListeners()` / `removeAllListeners(event)`.
    virtual EventEmitter& removeAllListeners();
    virtual EventEmitter& removeAllListeners(std::string_view event);
    // `emitter.listenerCount(event)`.
    virtual std::size_t listenerCount(std::string_view event) const;
    // `emitter.emit(event, ...args)`: calls the listeners registered when it
    // starts, in order; a listener that throws stops the emit and the
    // exception propagates (JS semantics). Returns whether there were
    // listeners. "error" without a listener throws `args.error`.
    virtual bool emit(std::string_view event, const EventArgs& args);
    bool emit(std::string_view event) { return emit(event, EventArgs{}); }

    // One registered listener.
    struct Registration {
      std::string event;
      Listener listener;
      bool once = false;
      // Set for the wrapper an `onmessage = ...` style attribute installs.
      bool attribute = false;
      // The handler an attribute wrapper stands for (what the getter returns).
      std::shared_ptr<const void> attributeHandler;
      // A once registration's "fired" flag (shared with emit's snapshot).
      std::shared_ptr<bool> fired;
    };

  protected:
    virtual std::shared_ptr<const void> pinEventReceiver() const { return {}; }
    std::vector<Registration> registrations_;
  };

  // The events the `onopen` / `onmessage` / `onclose` / `onerror` attributes
  // receive (ws's event-target.js classes). `target` is the socket.
  struct Event {
    std::string type;
    WebSocket* target = nullptr;
  };
  struct MessageEvent {
    std::string type = "message";
    // A string for a text message (the Buffer decoded as UTF-8), Bytes for binary.
    Data data;
    WebSocket* target = nullptr;
  };
  struct CloseEvent {
    std::string type = "close";
    int code = 0;
    std::string reason;
    // The close frame was both received and sent.
    bool wasClean = false;
    WebSocket* target = nullptr;
  };
  struct ErrorEvent {
    std::string type = "error";
    Reason error;
    // `error.message`.
    std::string message;
    WebSocket* target = nullptr;
  };

  // The options of `new WebSocket(address, options)` (ws's defaults).
  struct ClientOptions {
    // `headers`: a JS record, names as written, in insertion order. ws adds
    // Sec-WebSocket-Version/Key, Connection, Upgrade and, unless given, Host.
    std::vector<std::pair<std::string, std::string>> headers{};
    // `perMessageDeflate` (ws: true). The extension is never offered: see the
    // limits in docs/rt/websocket.md; true and false behave alike.
    bool perMessageDeflate = true;
    // `maxPayload`: the largest message accepted (ToInt32; 0 = unlimited).
    double maxPayload = 104857600;
    // `handshakeTimeout`: the opening handshake fails ("Opening handshake has
    // timed out") after this many milliseconds without socket activity. Unset
    // or 0: no limit.
    std::optional<double> handshakeTimeout{};
    // `autoPong`: answer pings with a pong carrying the same data.
    bool autoPong = true;
    // `origin`: sent as the Origin header.
    std::optional<std::string> origin{};
    // Source compatibility. Received text and close reasons always require UTF-8.
    bool skipUTF8Validation = false;
    // tls.connect options for `wss:`. `ca` (PEM) replaces the default trust
    // store; `rejectUnauthorized: false` accepts any certificate; `servername`
    // overrides the SNI name (default: the host, empty for an IP address).
    std::optional<std::string> ca{};
    bool rejectUnauthorized = true;
    std::optional<std::string> servername{};
  };

  // The options of `ws.send(data, options, cb)`.
  struct SendOptions {
    // Default: data is not a string.
    std::optional<bool> binary{};
    // Default: masked from a client, unmasked from a server.
    std::optional<bool> mask{};
    // Accepted and ignored (permessage-deflate is never negotiated).
    std::optional<bool> compress{};
    // False sends a fragment; later sends continue the message until one has fin.
    bool fin = true;
  };

  // A send/ping/pong callback: called once with a null Reason after the frame
  // was written, or with the error.
  using Callback = std::function<void(Reason error)>;

  // `typeof WebSocket` held as a value: `new ctor(address, options)`. An empty
  // function is "not a function". Test doubles derive from WebSocket.
  using WebSocketConstructor = std::function<std::shared_ptr<WebSocket>(std::string address, ClientOptions options)>;

  class WebSocket : public EventEmitter, public std::enable_shared_from_this<WebSocket> {
  public:
    // `WebSocket.CONNECTING` ... `WebSocket.CLOSED` (readyState values).
    static constexpr int CONNECTING = 0;
    static constexpr int OPEN = 1;
    static constexpr int CLOSING = 2;
    static constexpr int CLOSED = 3;

    // `new WebSocket(address, options)`: validates the URL and options
    // (SyntaxError "Invalid URL: <address>", ... and TypeError for invalid
    // header names/values, thrown here) and starts connecting.
    static std::shared_ptr<WebSocket> create(std::string address, ClientOptions options);
    static std::shared_ptr<WebSocket> create(std::string address);
    // The class itself as a constructor value (what `import WebSocket from "ws"` binds).
    static WebSocketConstructor constructor();

    // `new WebSocket(null, undefined, { autoPong })`: an unconnected server-side
    // socket. WebSocketServer creates these (through ServerOptions::WebSocket)
    // and attaches the upgraded connection; test doubles use it as their base.
    explicit WebSocket(std::nullptr_t, bool autoPong = true);
    ~WebSocket() override;

    // `ws.readyState`.
    virtual int readyState() const;
    // `ws.bufferedAmount`: bytes accepted by send/ping/pong and not yet handed
    // to the kernel, plus what was "sent" after the socket started closing.
    // Goes through bufferedAmountGetter().
    virtual double bufferedAmount() const;
    // `ws.url` (std::nullopt on the server side, where JS has undefined).
    virtual std::optional<std::string> url() const;
    // `ws.protocol` (the negotiated subprotocol, "" when none), `ws.extensions` (always "").
    virtual std::string protocol() const;
    virtual std::string extensions() const;
    // `ws.binaryType`: always "nodebuffer" (binary messages are Buffers).
    std::string binaryType() const { return "nodebuffer"; }

    // `ws.send(data[, options][, cb])`. Error "WebSocket is not open:
    // readyState 0 (CONNECTING)" while connecting; after close() started the
    // data only counts towards bufferedAmount and `cb` receives Error
    // "WebSocket is not open: readyState 2 (CLOSING)" (or 3 (CLOSED)).
    virtual void send(Data data, SendOptions options, Callback cb);
    void send(Data data) { send(std::move(data), SendOptions{}, Callback{}); }
    void send(Data data, Callback cb) { send(std::move(data), SendOptions{}, std::move(cb)); }
    void send(Data data, SendOptions options) { send(std::move(data), std::move(options), Callback{}); }
    // `ws.close([code[, reason]])`. Valid codes are 1000-1014 except 1004-1006
    // and 3000-4999 (TypeError "First argument must be a valid error code
    // number"); the reason is at most 123 UTF-8 bytes (RangeError "The message
    // must not be greater than 123 bytes"). As in ws, readyState is already
    // CLOSING when either error is thrown. Internal closes (a received close
    // frame, a receiver error with its status code) call this virtual method,
    // so a subclass can rewrite them.
    virtual void close(std::optional<int> code, std::optional<Data> reason);
    void close() { close(std::nullopt, std::nullopt); }
    void close(int code) { close(code, std::nullopt); }
    void close(int code, Data reason) { close(code, std::optional<Data>(std::move(reason))); }
    // `ws.ping([data[, mask]][, cb])` / `ws.pong(...)`: data up to 125 bytes
    // (RangeError "The data size must not be greater than 125 bytes").
    virtual void ping(std::optional<Data> data, std::optional<bool> mask, Callback cb);
    void ping() { ping(std::nullopt, std::nullopt, Callback{}); }
    void ping(Data data) { ping(std::optional<Data>(std::move(data)), std::nullopt, Callback{}); }
    virtual void pong(std::optional<Data> data, std::optional<bool> mask, Callback cb);
    void pong() { pong(std::nullopt, std::nullopt, Callback{}); }
    void pong(Data data) { pong(std::optional<Data>(std::move(data)), std::nullopt, Callback{}); }
    // `ws.terminate()`: destroys the connection at once ("close" follows with
    // 1006). While connecting: "error" (WebSocket was closed before the
    // connection was established) then "close".
    virtual void terminate();

    // `ws.onopen = handler` / `ws.onopen` and the message, close and error
    // twins: ws stores the handler as a listener, so it runs in registration
    // order with the on()/once() listeners; setting replaces (and moves) the
    // previous handler, nullptr removes it.
    void onopen(std::function<void(const Event&)> handler);
    void onmessage(std::function<void(const MessageEvent&)> handler);
    void onclose(std::function<void(const CloseEvent&)> handler);
    void onerror(std::function<void(const ErrorEvent&)> handler);
    std::function<void(const Event&)> onopen() const;
    std::function<void(const MessageEvent&)> onmessage() const;
    std::function<void(const CloseEvent&)> onclose() const;
    std::function<void(const ErrorEvent&)> onerror() const;

    // `Object.defineProperty(WebSocket.prototype, "bufferedAmount", { get })`:
    // the process-wide getter every real WebSocket's bufferedAmount() uses.
    // Tests save it, install a replacement and restore the saved one; the
    // default getter returns ownBufferedAmount().
    using BufferedAmountGetter = std::function<double(const WebSocket& socket)>;
    static BufferedAmountGetter& bufferedAmountGetter();
    // The value of ws's own bufferedAmount getter.
    double ownBufferedAmount() const;

    // Implementation state (src/rt/websocket*.cpp).
    struct State;
    State& state() const noexcept { return *state_; }

  private:
    struct PrivateTag {};

  public:
    explicit WebSocket(PrivateTag);

  private:
    friend class WebSocketServer;
    std::shared_ptr<const void> pinEventReceiver() const override { return weak_from_this().lock(); }
    std::unique_ptr<State> state_;
  };

  // The upgrade request as node:http hands it to ws (the `req` of the server's
  // "upgrade" event): what the HTTP server stage passes to handleUpgrade.
  struct UpgradeRequest {
    // `req.method` ("GET").
    std::string method;
    // `req.url`: the request target as received ("/v1/live/x?y=1").
    std::string url;
    // `req.rawHeaders` as name/value pairs: wire order and case, each byte as
    // the code point U+00xx (Latin-1).
    std::vector<std::pair<std::string, std::string>> rawHeaders;
    // `req.headers[name]` for a lower-case name, node:http's join rules (see
    // IncomingResponse::header).
    std::optional<std::string> header(std::string_view name) const;
  };

  // The connection an upgrade request arrived on (the `socket` of the
  // "upgrade" event), handed over to ws together with the bytes already read
  // past the request head (`head`).
  struct UpgradeSocket {
    // A connected TCP or Unix-domain stream on the current rt loop's
    // io_context (a tcp::socket or local::stream_protocol::socket converts by
    // move). TLS-terminated servers are not provided.
    boost::asio::generic::stream_protocol::socket stream;
    // `socket.readable`: false once the HTTP stage has read the client's FIN.
    bool readable = true;
    // `socket.writable`: false once the HTTP stage ended or destroyed its side.
    bool writable = true;
  };

  // The options of `new WebSocketServer(options)` the gateway uses (ws's
  // defaults). Only noServer mode is provided: the HTTP server stage accepts
  // connections and calls handleUpgrade.
  struct ServerOptions {
    // `noServer`: must be true (TypeError "One and only one of the "port",
    // "server", or "noServer" options must be specified" otherwise).
    bool noServer = false;
    // `clientTracking`: keep the open sockets in clients().
    bool clientTracking = true;
    // `perMessageDeflate`: must be false (the extension is never negotiated).
    bool perMessageDeflate = false;
    // `maxPayload`: the largest message accepted (ToInt32; 0 = unlimited).
    double maxPayload = 104857600;
    bool autoPong = true;
    // Source compatibility. Received text and close reasons always require UTF-8.
    bool skipUTF8Validation = false;
    // `path`: only requests whose pathname equals it are handled (400 otherwise).
    std::optional<std::string> path{};
    // `handleProtocols(protocols, req)`: picks the subprotocol (std::nullopt or
    // "" for none); default: the first one offered.
    std::function<std::optional<std::string>(const std::vector<std::string>& protocols, const UpgradeRequest& request)> handleProtocols{};
    // `WebSocket`: the class instantiated for each connection, `new
    // options.WebSocket(null, undefined, options)`; a subclass may override
    // close() and the other virtual methods. Default: WebSocket itself.
    std::function<std::shared_ptr<ws::WebSocket>(const ServerOptions& options)> WebSocket{};
    // Gateway observer receives only the fixed status and can return a request ID.
    std::function<std::optional<std::string>(int)> onHandshakeRejection{};
  };

  // `handleUpgrade`'s callback: `(ws, req) => ...`.
  using UpgradeCallback = std::function<void(std::shared_ptr<WebSocket> socket, const UpgradeRequest& request)>;
  // `typeof WebSocketServer` held as a value: `new ctor(options)`.
  using WebSocketServerConstructor = std::function<std::shared_ptr<WebSocketServer>(ServerOptions options)>;

  class WebSocketServer : public EventEmitter, public std::enable_shared_from_this<WebSocketServer> {
  public:
    // `new WebSocketServer(options)`. Create it with std::make_shared.
    explicit WebSocketServer(ServerOptions options);
    ~WebSocketServer() override;
    // The class itself as a constructor value (what `import { WebSocketServer } from "ws"` binds).
    static WebSocketServerConstructor constructor();

    // `server.handleUpgrade(req, socket, head, cb)`, synchronous like ws:
    // validates the request (writing ws's 405/400 response and closing, cb not
    // called), emits "headers", writes the 101 response, creates the socket
    // (already open) and calls cb. `head` reaches the socket as its first data,
    // one tick later, after cb attached its listeners. ws takes `socket.stream`
    // over (moves it out) when it answers or destroys the connection; an
    // exception from a "headers" listener propagates before that, leaving the
    // stream with the caller, as the JS socket is.
    void handleUpgrade(const UpgradeRequest& request, UpgradeSocket& socket, Bytes head, UpgradeCallback cb);
    // `server.shouldHandle(req)`.
    bool shouldHandle(const UpgradeRequest& request) const;
    // `server.close([cb])`: stops accepting upgrades (later ones get 503);
    // "close" is emitted once no tracked client is left (on the next tick in
    // noServer mode without tracking). `cb` is a one-time "close" listener;
    // closing a closed server calls it with Error "The server is not running".
    void close();
    void close(std::function<void(Reason error)> cb);
    // `server.clients` (clientTracking only; empty otherwise).
    std::vector<std::shared_ptr<WebSocket>> clients() const;
    // `server.options`.
    const ServerOptions& options() const noexcept { return options_; }

  private:
    std::shared_ptr<const void> pinEventReceiver() const override { return weak_from_this().lock(); }
    void rejectHandshake(UpgradeSocket& socket, int status, std::string message = "", std::vector<std::pair<std::string, std::string>> headers = {});
    void completeUpgrade(const std::string& key, const std::vector<std::string>& protocols, const UpgradeRequest& request, UpgradeSocket& socket, Bytes head, UpgradeCallback cb);
    void emitClose();
    ServerOptions options_;
    int state_ = 0;
    bool shouldEmitClose_ = false;
    std::vector<std::shared_ptr<WebSocket>> clients_;
  };
}
