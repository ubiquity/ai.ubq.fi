#pragma once
// Internals of the rt WebSocket component (uos/rt/websocket.hpp): the node:net
// socket model ws runs on, ws's Receiver and Sender, the node:http request of
// the opening handshake, and the WebSocket state.
#include "uos/rt/clock.hpp"
#include "uos/rt/websocket.hpp"
#include <boost/asio/generic/stream_protocol.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uos::rt::ws::detail {
  using BytesView = std::span<const std::uint8_t>;

  class WebSocketRegistry;
  void trackWebSocket(const std::shared_ptr<WebSocket>& websocket);

  // An exception escaping a JS event listener or callback is uncaught: it goes
  // to the loop's UncaughtPolicy, and the caller carries on.
  void reportUncaught(Reason reason) noexcept;
  template <class F> void guarded(F&& function) noexcept {
    try {
      function();
    } catch (...) {
      reportUncaught(std::current_exception());
    }
  }
  // `process.nextTick(f)`: the next microtask.
  void nextTick(std::function<void()> function);

  // Error texts and codes shared by the implementation files.
  Reason codedError(std::string message, std::string code, std::string name = "Error");
  // A failed system call the way Node reports it: "<syscall> <CODE><suffix>"
  // with code, errno (negative) and syscall set.
  Reason systemError(int errorNumber, std::string syscall, std::string suffix);
  std::string errnoName(int errorNumber);

  // The "<value>" of node:http's join rules for a header map built from raw
  // name/value pairs (see IncomingResponse::header).
  std::optional<std::string> joinedHeader(const std::vector<std::pair<std::string, std::string>>& rawHeaders, std::string_view name);
  // RFC 6455 accept token for a generated or validated handshake key.
  std::string handshakeAccept(std::string_view key);

  // A node:net Socket (allowHalfOpen false, so the socket ends its side when
  // the peer does and is destroyed once both sides are done) over a connected
  // Asio stream, with an optional TLS client layer. What ws's code calls
  // `socket`; its listeners are these hooks, each called as its own event, and
  // all released once "close" has been emitted.
  class Socket : public std::enable_shared_from_this<Socket> {
  public:
    using Stream = boost::asio::generic::stream_protocol::socket;
    explicit Socket(Stream stream, bool readable = true, bool writable = true);
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    std::function<void(BytesView chunk)> onData;
    std::function<void()> onEnd;
    std::function<void(Reason error)> onError;
    std::function<void()> onClose;
    std::function<void()> onFinish;
    // Every read and completed write (what socket.setTimeout measures).
    std::function<void()> onActivity;

    // `socket.on("data")` / `socket.resume()`: delivers data as it arrives;
    // data read earlier (and what unshift() put back) follows one tick later.
    void resume();
    // `socket.pause()`: data read from now on is held until resume().
    void pause();
    // `socket.unshift(chunk)`
    void unshift(Bytes chunk);
    // `socket.write(data, cb)`: in order; cb(null) once the kernel took it
    // (one tick later when that happened at once), cb(error) when the socket
    // is destroyed first.
    void write(Bytes data, Callback cb);
    // `socket._writableState.length`: accepted bytes not yet handed to the kernel.
    std::size_t writableLength() const noexcept;
    // `socket.end()`: FIN (TLS: close_notify first) once the queue is written.
    void end();
    // `socket.destroy([error])`: closes at once and drops what is queued;
    // "error" (when given) and "close" follow as one macrotask.
    void destroy(Reason error = nullptr);
    // Loop teardown abandons pending events and releases their captured owners.
    void shutdown();
    bool destroyed() const noexcept { return destroyed_; }
    // `socket.readable` / `socket.writable`.
    bool readable() const noexcept;
    bool writable() const noexcept;
    // `socket.setNoDelay()` (TCP only).
    void setNoDelay();

    // tls.connect's client side on this connected stream: handshake, then the
    // chain check (unless rejectUnauthorized is false) and checkServerIdentity.
    // `done(null)` once secure; `done(error)` otherwise.
    struct TlsOptions {
      std::string host;
      std::optional<std::string> ca;
      bool rejectUnauthorized = true;
      std::optional<std::string> servername;
    };
    void startTls(TlsOptions options, std::function<void(Reason error)> done);

    struct Tls;

  private:
    struct Pending {
      Bytes data;
      std::size_t offset = 0;
      // The plaintext size this chunk accounts for in writableLength().
      std::size_t accounted = 0;
      Callback callback;
    };
    void startReading();
    void readMore();
    void onRead(const boost::system::error_code& error, std::size_t count);
    void deliver(BytesView chunk);
    void handleEof();
    void queueRaw(Bytes data, std::size_t accounted, Callback callback);
    void flush();
    void onWritten(const boost::system::error_code& error, std::size_t count, Bytes data);
    void finishEnd();
    void maybeDestroy();
    void emitCloseEvents(Reason error);
    void releaseListeners();
    void pumpTls();
    void drainTlsOutput();
    void tlsHandshakeStep();
    void tlsWritePlain(Bytes data, Callback callback);

    Stream stream_;
    std::vector<std::uint8_t> readBuffer_;
    std::deque<Bytes> held_;
    std::deque<Pending> queue_;
    std::size_t queued_ = 0;
    bool flowing_ = false;
    bool reading_ = false;
    bool writing_ = false;
    bool readEnded_ = false;
    bool writeEnded_ = false;
    bool writeFinished_ = false;
    bool destroyed_ = false;
    bool deliveryScheduled_ = false;
    // EOF read while data was held: "end" follows that data.
    bool endHeld_ = false;
    bool readableAtStart_ = true;
    bool writableAtStart_ = true;
    std::unique_ptr<Tls> tls_;
  };

  // ws's Receiver: a Writable that parses frames synchronously and emits.
  class Receiver : public std::enable_shared_from_this<Receiver> {
  public:
    struct Options {
      bool isServer = false;
      double maxPayload = 0;
      bool skipUTF8Validation = false;
    };
    explicit Receiver(Options options);

    // The listeners ws attaches in setSocket ("conclude", "error", "message",
    // "ping", "pong"), and the one socketOnClose adds for "finish"/"error".
    std::function<void(int code, Bytes reason)> onConclude;
    // "error" with the close status ws sends for it (`err[kStatusCode]`).
    std::function<void(Reason error, int statusCode)> onError;
    std::function<void(Bytes data, bool isBinary)> onMessage;
    std::function<void(Bytes data)> onPing;
    std::function<void(Bytes data)> onPong;
    std::function<void()> onFinish;

    // `receiver.write(chunk)`. A frame error destroys the receiver; its
    // "error" event follows one tick later, as Writable emits it.
    void write(BytesView chunk);
    // `receiver.end()`: "finish" one tick later unless destroyed.
    void end();
    bool finished() const noexcept { return finished_; }
    bool errorEmitted() const noexcept { return errorEmitted_; }
    void removeAllListeners();

  private:
    enum class Step { getInfo, getPayloadLength16, getPayloadLength64, getMask, getData };
    BytesView consume(std::size_t count);
    void startLoop();
    void getInfo();
    void getPayloadLength16();
    void getPayloadLength64();
    void haveLength();
    void getMask();
    void getData();
    void dataMessage();
    void controlMessage(Bytes data);
    void fail(std::string name, std::string message, bool prefix, int statusCode, std::string errorCode);

    std::size_t maxPayload_;
    bool isServer_;
    Bytes buffer_;
    std::size_t bufferOffset_ = 0;
    std::uint64_t payloadLength_ = 0;
    std::array<std::uint8_t, 4> mask_{};
    int fragmented_ = 0;
    bool masked_ = false;
    bool fin_ = false;
    int opcode_ = 0;
    std::size_t totalPayloadLength_ = 0;
    std::size_t dataFrames_ = 0;
    std::vector<Bytes> fragments_;
    bool errored_ = false;
    bool loop_ = false;
    Step state_ = Step::getInfo;
    bool ending_ = false;
    bool finished_ = false;
    bool errorEmitted_ = false;
  };

  // ws's Sender: frames, masks and writes to the socket (permessage-deflate and
  // Blob data, the only sources of its queue, are not provided).
  class Sender {
  public:
    explicit Sender(std::shared_ptr<Socket> socket);
    // `Sender.frame(data, options)`: the header followed by the (masked) payload.
    static Bytes frame(BytesView data, int opcode, bool fin, bool rsv1, bool mask);
    // `sender.close(code, data, mask, cb)`: TypeError / RangeError for an
    // invalid code or a reason over 123 bytes.
    void close(std::optional<int> code, const std::optional<Data>& data, bool mask, Callback cb);
    void ping(const Data& data, bool mask, Callback cb);
    void pong(const Data& data, bool mask, Callback cb);
    void send(const Data& data, bool binary, bool mask, bool fin, Callback cb);
    // `sender._bufferedBytes`.
    double bufferedBytes = 0;

  private:
    void sendFrame(Bytes frame, Callback cb);
    std::shared_ptr<Socket> socket_;
    bool firstFragment_ = true;
    std::size_t messageBytes_ = 0;
    std::size_t dataFrames_ = 0;
  };

  // node:http's ClientRequest for ws's opening handshake: connects (TCP, Unix
  // or TLS), writes the request head, parses the response head and reports
  // "response" or "upgrade". socket.setTimeout(timeout) is its idle timer.
  class HandshakeRequest : public std::enable_shared_from_this<HandshakeRequest> {
  public:
    struct Target {
      bool ipc = false;
      std::string socketPath;
      std::string host;
      int port = 0;
      bool secure = false;
      Socket::TlsOptions tls;
    };
    HandshakeRequest(Target target, std::string head, std::optional<double> timeout);
    ~HandshakeRequest();

    std::function<void()> onTimeout;
    std::function<void(Reason error)> onErrorEvent;
    std::function<void(std::shared_ptr<const IncomingResponse> response)> onResponse;
    std::function<void(std::shared_ptr<const IncomingResponse> response, std::shared_ptr<Socket> socket, Bytes head)> onUpgrade;

    // `req.end()`: starts connecting.
    void end();
    // `req.abort()`: no further events; the caller destroys `socket`.
    void abort();
    // `req[kAborted]`
    bool aborted = false;
    // `req.socket` (null until connected).
    std::shared_ptr<Socket> socket;

  private:
    void connected(Socket::Stream stream);
    void secure();
    void fail(Reason error);
    void onData(BytesView chunk);
    void onEnd();
    void armTimer();
    void releaseListeners();

    Target target_;
    std::string head_;
    std::optional<double> timeout_;
    Timeout timer_;
    Bytes received_;
    bool responded_ = false;
    bool finished_ = false;
    // Cancels the pending resolve or connect.
    std::function<void()> cancel_;
  };

  // Parses an HTTP/1.x response head the way the client's parser rejects it
  // (CodedError "Parse Error: ..." with an "HPE_*" code). std::nullopt while
  // the head is incomplete; otherwise the response and the head's length.
  struct ParsedResponse {
    IncomingResponse response;
    std::size_t length = 0;
  };
  std::optional<ParsedResponse> parseResponseHead(BytesView data);

  // The listeners ws attaches to an upgraded socket and the ws internals the
  // server and the client both reach (websocket.cpp).
  void setSocket(const std::shared_ptr<WebSocket>& websocket, std::shared_ptr<Socket> socket, Bytes head, double maxPayload, bool skipUTF8Validation);
  void emitErrorAndClose(const std::shared_ptr<WebSocket>& websocket, Reason error);
  void emitClose(const std::shared_ptr<WebSocket>& websocket);
  void abortHandshakeRequest(const std::shared_ptr<WebSocket>& websocket, const std::shared_ptr<HandshakeRequest>& request, std::string message);
  void abortHandshakeSocket(const std::shared_ptr<WebSocket>& websocket, const std::shared_ptr<Socket>& socket, std::string message);
  // ws's initAsClient (websocket_client.cpp).
  void initAsClient(const std::shared_ptr<WebSocket>& websocket, const std::string& address, ClientOptions options);
}

namespace uos::rt::ws {
  struct WebSocket::State {
    detail::WebSocketRegistry* registry = nullptr;
    int closeCode = 1006;
    bool closeFrameReceived = false;
    bool closeFrameSent = false;
    Bytes closeMessage;
    Timeout closeTimer;
    Timeout idleTimer;
    bool errorEmitted = false;
    std::string protocol;
    int readyState = WebSocket::CONNECTING;
    std::shared_ptr<detail::Receiver> receiver;
    std::unique_ptr<detail::Sender> sender;
    std::shared_ptr<detail::Socket> socket;
    // `socket[kWebSocket]` is set (cleared by socketOnClose).
    bool socketAttached = false;
    // `socketOnData` is attached (removed when a close frame or an error ends reading).
    bool feeding = false;
    // `_bufferedAmount`: what was "sent" before a socket was attached.
    double bufferedAmount = 0;
    bool isServer = true;
    std::optional<std::string> url;
    std::shared_ptr<detail::HandshakeRequest> req;
    bool autoPong = true;
  };
}
