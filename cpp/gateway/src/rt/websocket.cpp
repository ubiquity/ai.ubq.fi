// ws's WebSocket (lib/websocket.js) and WebSocketServer
// (lib/websocket-server.js) in noServer mode, the EventEmitter surface they
// inherit, and the helpers the other websocket_*.cpp files share.
#include "websocket_internal.hpp"
#include "uos/rt/limits.hpp"

#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/beast/http/rfc7230.hpp>
#include <boost/beast/websocket/detail/hybi13.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

namespace uos::rt::ws {
  namespace detail {
    // IO normally keeps open WebSockets alive. Dropped completion handlers at loop
    // teardown cannot emit close, so release the remaining callback cycles there.
    class WebSocketRegistry : public boost::asio::execution_context::service {
    public:
      static boost::asio::execution_context::id id;
      explicit WebSocketRegistry(boost::asio::execution_context& context) : service(context) {}
      void add(const std::shared_ptr<WebSocket>& websocket) { sockets_[websocket.get()] = websocket; }
      void remove(WebSocket* websocket) { sockets_.erase(websocket); }

    private:
      void shutdown() override {
        auto sockets = std::move(sockets_);
        sockets_.clear();
        for (const auto& [pointer, weak] : sockets) {
          (void)pointer;
          auto websocket = weak.lock();
          if (!websocket) continue;
          WebSocket::State& state = websocket->state();
          state.registry = nullptr;
          auto request = std::exchange(state.req, nullptr);
          if (request) {
            request->abort();
            if (request->socket) request->socket->shutdown();
          }
          if (state.socket) {
            state.bufferedAmount = websocket->ownBufferedAmount();
            state.socket->shutdown();
          }
          state.sender.reset();
          state.socket.reset();
          state.socketAttached = false;
          state.readyState = WebSocket::CLOSED;
          if (state.receiver) state.receiver->removeAllListeners();
          websocket->removeAllListeners();
        }
      }
      std::unordered_map<WebSocket*, std::weak_ptr<WebSocket>> sockets_;
    };

    boost::asio::execution_context::id WebSocketRegistry::id;

    void trackWebSocket(const std::shared_ptr<WebSocket>& websocket) {
      if (websocket->state().registry) return;
      auto& registry = boost::asio::use_service<WebSocketRegistry>(static_cast<boost::asio::execution_context&>(rt::loop().context()));
      registry.add(websocket);
      websocket->state().registry = &registry;
    }

    void reportUncaught(Reason reason) noexcept {
      try {
        rt::detail::reportUncaught(std::move(reason), false);
      } catch (...) {
      }
    }

    void nextTick(std::function<void()> function) { rt::queueMicrotask(std::move(function)); }

    Reason codedError(std::string message, std::string code, std::string name) { return std::make_exception_ptr(CodedError(std::move(message), std::move(code), std::move(name))); }

    std::string handshakeAccept(std::string_view key) {
      boost::beast::websocket::detail::sec_ws_accept_type accept;
      boost::beast::websocket::detail::make_sec_ws_accept(accept, key);
      return {accept.data(), accept.size()};
    }

    std::string errnoName(int errorNumber) {
      switch (errorNumber) {
        case EPERM: return "EPERM";
        case ENOENT: return "ENOENT";
        case EINTR: return "EINTR";
        case EIO: return "EIO";
        case EBADF: return "EBADF";
        case EAGAIN: return "EAGAIN";
        case ENOMEM: return "ENOMEM";
        case EACCES: return "EACCES";
        case EFAULT: return "EFAULT";
        case EBUSY: return "EBUSY";
        case EEXIST: return "EEXIST";
        case ENOTDIR: return "ENOTDIR";
        case EISDIR: return "EISDIR";
        case EINVAL: return "EINVAL";
        case ENFILE: return "ENFILE";
        case EMFILE: return "EMFILE";
        case ENOSPC: return "ENOSPC";
        case EPIPE: return "EPIPE";
        case ENAMETOOLONG: return "ENAMETOOLONG";
        case ELOOP: return "ELOOP";
        case ENOTSOCK: return "ENOTSOCK";
        case EPROTOTYPE: return "EPROTOTYPE";
        case EPROTONOSUPPORT: return "EPROTONOSUPPORT";
        case EOPNOTSUPP: return "ENOTSUP";
        case EAFNOSUPPORT: return "EAFNOSUPPORT";
        case EADDRINUSE: return "EADDRINUSE";
        case EADDRNOTAVAIL: return "EADDRNOTAVAIL";
        case ENETDOWN: return "ENETDOWN";
        case ENETUNREACH: return "ENETUNREACH";
        case ECONNABORTED: return "ECONNABORTED";
        case ECONNRESET: return "ECONNRESET";
        case ENOBUFS: return "ENOBUFS";
        case EISCONN: return "EISCONN";
        case ENOTCONN: return "ENOTCONN";
        case ETIMEDOUT: return "ETIMEDOUT";
        case ECONNREFUSED: return "ECONNREFUSED";
        case EHOSTUNREACH: return "EHOSTUNREACH";
        case EALREADY: return "EALREADY";
        case EINPROGRESS: return "EINPROGRESS";
        case ECANCELED: return "ECANCELED";
        default: return "UNKNOWN";
      }
    }

    Reason systemError(int errorNumber, std::string syscall, std::string suffix) {
      std::string code = errnoName(errorNumber);
      CodedError error(syscall + " " + code + suffix, code);
      error.errno_ = -static_cast<double>(errorNumber);
      error.syscall = std::move(syscall);
      return std::make_exception_ptr(std::move(error));
    }

    namespace {
      std::string lowerAscii(std::string_view text) {
        std::string out(text);
        for (char& c : out) {
          if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return out;
      }
      // node:http's matchKnownFields: the fields whose repeats are dropped.
      bool keepsFirst(std::string_view name) {
        static const std::string_view names[] = {"age",          "host",         "from",          "etag",       "server",           "referer",
                                                  "expires",      "location",     "user-agent",    "retry-after", "content-type",     "max-forwards",
                                                  "authorization", "last-modified", "content-length", "if-modified-since", "proxy-authorization", "if-unmodified-since"};
        return std::find(std::begin(names), std::end(names), name) != std::end(names);
      }
    }

    std::optional<std::string> joinedHeader(const std::vector<std::pair<std::string, std::string>>& rawHeaders, std::string_view name) {
      const std::string wanted = lowerAscii(name);
      const std::string_view separator = wanted == "cookie" ? "; " : ", ";
      std::optional<std::string> value;
      for (const auto& [field, text] : rawHeaders) {
        if (lowerAscii(field) != wanted) continue;
        if (!value) value = text;
        else if (!keepsFirst(wanted)) *value += std::string(separator) + text;
      }
      return value;
    }
  }

  std::optional<std::string> IncomingResponse::header(std::string_view name) const { return detail::joinedHeader(rawHeaders, name); }
  std::optional<std::string> UpgradeRequest::header(std::string_view name) const { return detail::joinedHeader(rawHeaders, name); }

  // ---- EventEmitter ----------------------------------------------------------------

  EventEmitter& EventEmitter::on(std::string_view event, Listener listener) {
    registrations_.push_back(Registration{std::string(event), std::move(listener), false, false, nullptr, nullptr});
    return *this;
  }

  EventEmitter& EventEmitter::once(std::string_view event, Listener listener) {
    registrations_.push_back(Registration{std::string(event), std::move(listener), true, false, nullptr, std::make_shared<bool>(false)});
    return *this;
  }

  EventEmitter& EventEmitter::removeListener(std::string_view event, const Listener& listener) {
    for (auto entry = registrations_.rbegin(); entry != registrations_.rend(); ++entry) {
      if (entry->event == event && !entry->attribute && entry->listener == listener) {
        registrations_.erase(std::next(entry).base());
        break;
      }
    }
    return *this;
  }

  EventEmitter& EventEmitter::removeAllListeners() {
    registrations_.clear();
    return *this;
  }

  EventEmitter& EventEmitter::removeAllListeners(std::string_view event) {
    std::erase_if(registrations_, [&](const Registration& entry) { return entry.event == event; });
    return *this;
  }

  std::size_t EventEmitter::listenerCount(std::string_view event) const {
    return static_cast<std::size_t>(std::count_if(registrations_.begin(), registrations_.end(), [&](const Registration& entry) { return entry.event == event; }));
  }

  bool EventEmitter::emit(std::string_view event, const EventArgs& args) {
    const auto receiver = pinEventReceiver();
    std::vector<Registration> listeners;
    for (const Registration& entry : registrations_) {
      if (entry.event == event) listeners.push_back(entry);
    }
    if (listeners.empty()) {
      if (event == "error") rethrow(args.error);
      return false;
    }
    for (const Registration& entry : listeners) {
      if (entry.once) {
        if (*entry.fired) continue;
        *entry.fired = true;
        std::erase_if(registrations_, [&](const Registration& candidate) { return candidate.fired == entry.fired; });
      }
      entry.listener(args);
    }
    return true;
  }

  // ---- WebSocket -------------------------------------------------------------------

  namespace detail {
    namespace {
      constexpr const char* READY_STATES[] = {"CONNECTING", "OPEN", "CLOSING", "CLOSED"};
      constexpr double CLOSE_TIMEOUT = 30 * 1000;

      void safeEmit(const std::shared_ptr<WebSocket>& websocket, std::string_view event, const EventArgs& args) {
        guarded([&] { websocket->emit(event, args); });
      }

      std::string notOpenMessage(int readyState) { return "WebSocket is not open: readyState " + std::to_string(readyState) + " (" + READY_STATES[readyState] + ")"; }

      // `Buffer#toString()`: UTF-8 with U+FFFD for bad sequences, a BOM kept.
      std::string bufferText(const Data& data) {
        if (const auto* text = std::get_if<std::string>(&data)) return *text;
        return decodeUtf8(std::span<const std::uint8_t>(std::get<Bytes>(data)), {.ignoreBOM = true});
      }

      std::size_t byteLength(const Data& data) {
        if (const auto* text = std::get_if<std::string>(&data)) return utf8ByteLength(*text);
        return std::get<Bytes>(data).size();
      }

      // `if (data)`: an empty string is falsy, a Buffer never is.
      bool truthy(const std::optional<Data>& data) {
        if (!data) return false;
        if (const auto* text = std::get_if<std::string>(&*data)) return !text->empty();
        return true;
      }

      // Handles `ping()`, `pong()` and `send()` once the socket is closing or closed.
      void sendAfterClose(WebSocket& websocket, const std::optional<Data>& data, Callback cb) {
        WebSocket::State& state = websocket.state();
        if (truthy(data)) {
          const auto length = static_cast<double>(byteLength(*data));
          if (state.socket) state.sender->bufferedBytes += length;
          else state.bufferedAmount += length;
        }
        if (cb) {
          Reason error = std::make_exception_ptr(JsError(notOpenMessage(state.readyState)));
          nextTick([cb = std::move(cb), error] { cb(error); });
        }
      }

      void setCloseTimer(WebSocket& websocket) {
        WebSocket::State& state = websocket.state();
        state.closeTimer = rt::setTimeout([socket = state.socket] { socket->destroy(); }, CLOSE_TIMEOUT);
      }

      void receiverOnConclude(const std::shared_ptr<WebSocket>& websocket, int code, Bytes reason) {
        WebSocket::State& state = websocket->state();
        state.closeFrameReceived = true;
        state.closeMessage = reason;
        state.closeCode = code;
        if (!state.socketAttached) return;
        state.feeding = false;
        if (code == 1005) websocket->close();
        else websocket->close(code, Data(std::move(reason)));
      }

      void receiverOnError(const std::shared_ptr<WebSocket>& websocket, Reason error, int statusCode) {
        WebSocket::State& state = websocket->state();
        if (state.socketAttached) {
          state.feeding = false;
          websocket->close(statusCode, std::nullopt);
        }
        if (!state.errorEmitted) {
          state.errorEmitted = true;
          EventArgs args;
          args.error = error;
          safeEmit(websocket, "error", args);
        }
      }

      void receiverOnPing(const std::shared_ptr<WebSocket>& websocket, Bytes data) {
        WebSocket::State& state = websocket->state();
        if (state.autoPong) websocket->pong(Data(data), !state.isServer, [](Reason) {});
        EventArgs args;
        args.data = std::move(data);
        safeEmit(websocket, "ping", args);
      }

      void socketOnClose(const std::shared_ptr<WebSocket>& websocket) {
        WebSocket::State& state = websocket->state();
        state.readyState = WebSocket::CLOSING;
        state.receiver->end();
        state.socketAttached = false;
        clearTimeout(state.closeTimer);
        clearTimeout(state.idleTimer);
        if (state.receiver->finished() || state.receiver->errorEmitted()) {
          emitClose(websocket);
        } else {
          // The receiver's "finish" listener holds the socket until it fires.
          state.receiver->onFinish = [websocket] { emitClose(websocket); };
        }
      }

      void socketOnEnd(const std::shared_ptr<WebSocket>& websocket) {
        WebSocket::State& state = websocket->state();
        state.readyState = WebSocket::CLOSING;
        state.receiver->end();
        state.socket->end();
      }

      void socketOnError(const std::shared_ptr<WebSocket>& websocket) {
        WebSocket::State& state = websocket->state();
        if (state.socketAttached) {
          state.readyState = WebSocket::CLOSING;
          state.socket->destroy();
        }
      }
    }

    void setSocket(const std::shared_ptr<WebSocket>& websocket, std::shared_ptr<Socket> socket, Bytes head, double maxPayload, bool skipUTF8Validation) {
      trackWebSocket(websocket);
      WebSocket::State& state = websocket->state();
      auto receiver = std::make_shared<Receiver>(Receiver::Options{.isServer = state.isServer, .maxPayload = maxPayload, .skipUTF8Validation = skipUTF8Validation});
      state.receiver = receiver;
      state.sender = std::make_unique<Sender>(socket);
      state.socket = socket;
      state.socketAttached = true;
      std::weak_ptr<WebSocket> weak = websocket;
      receiver->onConclude = [weak](int code, Bytes reason) {
        if (auto target = weak.lock()) receiverOnConclude(target, code, std::move(reason));
      };
      receiver->onError = [weak](Reason error, int statusCode) {
        if (auto target = weak.lock()) receiverOnError(target, std::move(error), statusCode);
      };
      receiver->onMessage = [weak](Bytes data, bool isBinary) {
        if (auto target = weak.lock()) {
          EventArgs args;
          args.data = std::move(data);
          args.isBinary = isBinary;
          safeEmit(target, "message", args);
        }
      };
      receiver->onPing = [weak](Bytes data) {
        if (auto target = weak.lock()) receiverOnPing(target, std::move(data));
      };
      receiver->onPong = [weak](Bytes data) {
        if (auto target = weak.lock()) {
          EventArgs args;
          args.data = std::move(data);
          safeEmit(target, "pong", args);
        }
      };
      constexpr auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(limits::kWebSocketIdleTimeout).count();
      state.idleTimer = rt::detail::createTimer([weak] {
        if (auto target = weak.lock(); target && target->state().socketAttached) target->state().socket->destroy();
      }, static_cast<double>(idleMs), false);
      socket->onActivity = [idle = state.idleTimer]() mutable { idle.refresh(); };
      socket->onFinish = nullptr;
      socket->setNoDelay();
      socket->unshift(std::move(head));
      socket->onClose = [websocket] { socketOnClose(websocket); };
      socket->onData = [websocket](BytesView chunk) {
        WebSocket::State& current = websocket->state();
        if (current.feeding && current.socketAttached) current.receiver->write(chunk);
      };
      socket->onEnd = [websocket] { socketOnEnd(websocket); };
      socket->onError = [websocket](Reason) { socketOnError(websocket); };
      state.feeding = true;
      socket->resume();
      state.readyState = WebSocket::OPEN;
      safeEmit(websocket, "open", EventArgs{});
    }

    void emitClose(const std::shared_ptr<WebSocket>& websocket) {
      WebSocket::State& state = websocket->state();
      if (state.socket) state.receiver->removeAllListeners();
      state.readyState = WebSocket::CLOSED;
      EventArgs args;
      args.code = state.closeCode;
      args.reason = state.closeMessage;
      safeEmit(websocket, "close", args);
      // No event can follow "close": dropping the listeners releases what they
      // captured (often this socket or its peer), which JS would collect.
      websocket->removeAllListeners();
      state.req = nullptr;
    }

    void emitErrorAndClose(const std::shared_ptr<WebSocket>& websocket, Reason error) {
      WebSocket::State& state = websocket->state();
      state.readyState = WebSocket::CLOSING;
      state.errorEmitted = true;
      EventArgs args;
      args.error = std::move(error);
      safeEmit(websocket, "error", args);
      emitClose(websocket);
    }

    void abortHandshakeRequest(const std::shared_ptr<WebSocket>& websocket, const std::shared_ptr<HandshakeRequest>& request, std::string message) {
      websocket->state().readyState = WebSocket::CLOSING;
      Reason error = std::make_exception_ptr(JsError(std::move(message)));
      // ws reads `stream.setHeader` of a missing request.
      if (!request) throw TypeError("undefined is not an object (evaluating 'stream.setHeader')");
      request->aborted = true;
      request->abort();
      if (request->socket && !request->socket->destroyed()) request->socket->destroy();
      nextTick([websocket, error] { emitErrorAndClose(websocket, error); });
    }

    void abortHandshakeSocket(const std::shared_ptr<WebSocket>& websocket, const std::shared_ptr<Socket>& socket, std::string message) {
      websocket->state().readyState = WebSocket::CLOSING;
      Reason error = std::make_exception_ptr(JsError(std::move(message)));
      socket->onData = nullptr;
      socket->onEnd = nullptr;
      socket->onFinish = nullptr;
      socket->onActivity = nullptr;
      socket->onError = [websocket](Reason failure) {
        EventArgs args;
        args.error = std::move(failure);
        safeEmit(websocket, "error", args);
      };
      socket->onClose = [websocket] { emitClose(websocket); };
      socket->destroy(error);
    }
  }

  WebSocket::WebSocket(std::nullptr_t, bool autoPong) : state_(std::make_unique<State>()) {
    state_->isServer = true;
    state_->autoPong = autoPong;
  }

  WebSocket::WebSocket(PrivateTag) : state_(std::make_unique<State>()) { state_->isServer = false; }

  WebSocket::~WebSocket() {
    if (state_->registry) state_->registry->remove(this);
  }

  std::shared_ptr<WebSocket> WebSocket::create(std::string address, ClientOptions options) {
    auto websocket = std::make_shared<WebSocket>(PrivateTag{});
    detail::trackWebSocket(websocket);
    try {
      detail::initAsClient(websocket, address, std::move(options));
    } catch (...) {
      auto request = std::exchange(websocket->state().req, nullptr);
      if (request) request->abort();
      throw;
    }
    return websocket;
  }

  std::shared_ptr<WebSocket> WebSocket::create(std::string address) { return create(std::move(address), ClientOptions{}); }

  WebSocketConstructor WebSocket::constructor() {
    return [](std::string address, ClientOptions options) { return WebSocket::create(std::move(address), std::move(options)); };
  }

  int WebSocket::readyState() const { return state_->readyState; }

  WebSocket::BufferedAmountGetter& WebSocket::bufferedAmountGetter() {
    static BufferedAmountGetter getter = [](const WebSocket& socket) { return socket.ownBufferedAmount(); };
    return getter;
  }

  double WebSocket::bufferedAmount() const { return bufferedAmountGetter()(*this); }

  double WebSocket::ownBufferedAmount() const {
    if (!state_->socket) return state_->bufferedAmount;
    return static_cast<double>(state_->socket->writableLength()) + state_->sender->bufferedBytes;
  }

  std::optional<std::string> WebSocket::url() const { return state_->url; }
  std::string WebSocket::protocol() const { return state_->protocol; }
  std::string WebSocket::extensions() const { return ""; }

  void WebSocket::send(Data data, SendOptions options, Callback cb) {
    if (state_->readyState == CONNECTING) throw JsError("WebSocket is not open: readyState 0 (CONNECTING)");
    if (state_->readyState != OPEN) {
      detail::sendAfterClose(*this, std::optional<Data>(std::move(data)), std::move(cb));
      return;
    }
    const bool binary = options.binary.value_or(!std::holds_alternative<std::string>(data));
    const bool mask = options.mask.value_or(!state_->isServer);
    state_->sender->send(data, binary, mask, options.fin, std::move(cb));
  }

  void WebSocket::close(std::optional<int> code, std::optional<Data> reason) {
    State& state = *state_;
    if (state.readyState == CLOSED) return;
    if (state.readyState == CONNECTING) {
      detail::abortHandshakeRequest(shared_from_this(), state.req, "WebSocket was closed before the connection was established");
      return;
    }
    if (state.readyState == CLOSING) {
      if (state.closeFrameSent && (state.closeFrameReceived || state.receiver->errorEmitted())) state.socket->end();
      return;
    }
    state.readyState = CLOSING;
    std::weak_ptr<WebSocket> weak = weak_from_this();
    state.sender->close(code, reason, !state.isServer, [weak](Reason error) {
      // This error is handled by the `'error'` listener on the socket. We only
      // want to know if the close frame has been sent here.
      if (error) return;
      auto self = weak.lock();
      if (!self) return;
      State& current = self->state();
      current.closeFrameSent = true;
      if (current.closeFrameReceived || current.receiver->errorEmitted()) current.socket->end();
    });
    detail::setCloseTimer(*this);
  }

  void WebSocket::ping(std::optional<Data> data, std::optional<bool> mask, Callback cb) {
    if (state_->readyState == CONNECTING) throw JsError("WebSocket is not open: readyState 0 (CONNECTING)");
    if (state_->readyState != OPEN) {
      detail::sendAfterClose(*this, data, std::move(cb));
      return;
    }
    const Data payload = detail::truthy(data) ? *data : Data(Bytes{});
    state_->sender->ping(payload, mask.value_or(!state_->isServer), std::move(cb));
  }

  void WebSocket::pong(std::optional<Data> data, std::optional<bool> mask, Callback cb) {
    if (state_->readyState == CONNECTING) throw JsError("WebSocket is not open: readyState 0 (CONNECTING)");
    if (state_->readyState != OPEN) {
      detail::sendAfterClose(*this, data, std::move(cb));
      return;
    }
    const Data payload = detail::truthy(data) ? *data : Data(Bytes{});
    state_->sender->pong(payload, mask.value_or(!state_->isServer), std::move(cb));
  }

  void WebSocket::terminate() {
    State& state = *state_;
    if (state.readyState == CLOSED) return;
    if (state.readyState == CONNECTING) {
      detail::abortHandshakeRequest(shared_from_this(), state.req, "WebSocket was closed before the connection was established");
      return;
    }
    if (state.socket) {
      state.readyState = CLOSING;
      state.socket->destroy();
    }
  }

  // ---- onopen / onmessage / onclose / onerror ------------------------------------------

  namespace detail {
    namespace {
      // The `set` of ws's on<event> attribute: drops the current attribute
      // listener, then appends the new one.
      template <class Handler, class Wrap> void setAttribute(std::vector<WebSocket::Registration>& registrations, std::string_view event, Handler handler, Wrap wrap) {
        for (auto entry = registrations.begin(); entry != registrations.end(); ++entry) {
          if (entry->attribute && entry->event == event) {
            registrations.erase(entry);
            break;
          }
        }
        if (!handler) return;
        auto stored = std::make_shared<const Handler>(std::move(handler));
        registrations.push_back(WebSocket::Registration{std::string(event), Listener(wrap(stored)), false, true, stored, nullptr});
      }

      template <class Handler> Handler getAttribute(const std::vector<WebSocket::Registration>& registrations, std::string_view event) {
        for (const auto& entry : registrations) {
          if (entry.attribute && entry.event == event) return *std::static_pointer_cast<const Handler>(entry.attributeHandler);
        }
        return nullptr;
      }
    }
  }

  void WebSocket::onopen(std::function<void(const Event&)> handler) {
    detail::setAttribute(registrations_, "open", std::move(handler), [this](std::shared_ptr<const std::function<void(const Event&)>> stored) {
      return [this, stored](const EventArgs&) {
        Event event{"open", this};
        (*stored)(event);
      };
    });
  }

  void WebSocket::onmessage(std::function<void(const MessageEvent&)> handler) {
    detail::setAttribute(registrations_, "message", std::move(handler), [this](std::shared_ptr<const std::function<void(const MessageEvent&)>> stored) {
      return [this, stored](const EventArgs& args) {
        MessageEvent event;
        event.data = args.isBinary ? args.data : Data(detail::bufferText(args.data));
        event.target = this;
        (*stored)(event);
      };
    });
  }

  void WebSocket::onclose(std::function<void(const CloseEvent&)> handler) {
    detail::setAttribute(registrations_, "close", std::move(handler), [this](std::shared_ptr<const std::function<void(const CloseEvent&)>> stored) {
      return [this, stored](const EventArgs& args) {
        CloseEvent event;
        event.code = args.code;
        event.reason = detail::bufferText(Data(args.reason));
        event.wasClean = state_->closeFrameReceived && state_->closeFrameSent;
        event.target = this;
        (*stored)(event);
      };
    });
  }

  void WebSocket::onerror(std::function<void(const ErrorEvent&)> handler) {
    detail::setAttribute(registrations_, "error", std::move(handler), [this](std::shared_ptr<const std::function<void(const ErrorEvent&)>> stored) {
      return [this, stored](const EventArgs& args) {
        ErrorEvent event;
        event.error = args.error;
        event.message = errorMessage(args.error).value_or("");
        event.target = this;
        (*stored)(event);
      };
    });
  }

  std::function<void(const Event&)> WebSocket::onopen() const { return detail::getAttribute<std::function<void(const Event&)>>(registrations_, "open"); }
  std::function<void(const MessageEvent&)> WebSocket::onmessage() const { return detail::getAttribute<std::function<void(const MessageEvent&)>>(registrations_, "message"); }
  std::function<void(const CloseEvent&)> WebSocket::onclose() const { return detail::getAttribute<std::function<void(const CloseEvent&)>>(registrations_, "close"); }
  std::function<void(const ErrorEvent&)> WebSocket::onerror() const { return detail::getAttribute<std::function<void(const ErrorEvent&)>>(registrations_, "error"); }

  // ---- WebSocketServer -------------------------------------------------------------------

  namespace {
    constexpr int RUNNING = 0;
    constexpr int CLOSING_STATE = 1;
    constexpr int CLOSED_STATE = 2;

    bool validKey(std::string_view key) {
      namespace base64 = boost::beast::detail::base64;
      if (key.size() != 24 || !key.ends_with("==")) return false;
      std::array<std::uint8_t, base64::decoded_size(24)> nonce;
      const auto [size, consumed] = base64::decode(nonce.data(), key.data(), key.size());
      return size == 16 && consumed == 22;
    }

    std::optional<std::vector<std::string>> parseSubprotocols(std::string_view header) {
      constexpr std::string_view whitespace = " \t";
      if (header.empty() || header.size() > limits::kHttpHeaderBlockBytes || whitespace.find(header.front()) != whitespace.npos ||
          whitespace.find(header.back()) != whitespace.npos) return std::nullopt;
      const boost::beast::http::opt_token_list offered(header);
      std::vector<std::string> protocols;
      std::set<std::string_view> seen;
      const auto end = offered.end();
      auto it = offered.begin();
      for (; !it.error() && it != end; ++it) {
        const auto token = *it;
        const std::string_view protocol(token.data(), token.size());
        if (!seen.insert(protocol).second) return std::nullopt;
        protocols.emplace_back(protocol);
      }
      // HTTP token lists permit empty entries, which are invalid subprotocol offers.
      if (it.error() || protocols.empty() || std::ranges::count(header, ',') != static_cast<std::ptrdiff_t>(protocols.size() - 1)) return std::nullopt;
      return protocols;
    }

    std::string statusText(int code) {
      switch (code) {
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 405: return "Method Not Allowed";
        default: return "Service Unavailable";
      }
    }

    std::shared_ptr<detail::Socket> takeSocket(UpgradeSocket& socket) {
      return std::make_shared<detail::Socket>(std::move(socket.stream), socket.readable, socket.writable);
    }

    // websocket-server.js `abortHandshake(socket, code, message, headers)`:
    // the HTTP error response, then the socket is ended and destroyed.
    void abortHandshake(UpgradeSocket& upgrade, int code, std::string message = "", std::vector<std::pair<std::string, std::string>> extra = {}) {
      auto socket = takeSocket(upgrade);
      if (message.empty()) message = statusText(code);
      std::vector<std::pair<std::string, std::string>> headers{{"Connection", "close"}, {"Content-Type", "text/html"}, {"Content-Length", std::to_string(utf8ByteLength(message))}};
      for (auto& entry : extra) headers.push_back(std::move(entry));
      std::string response = "HTTP/1.1 " + std::to_string(code) + " " + statusText(code) + "\r\n";
      for (std::size_t index = 0; index < headers.size(); ++index) response += (index ? "\r\n" : "") + headers[index].first + ": " + headers[index].second;
      response += "\r\n\r\n" + message;
      std::weak_ptr<detail::Socket> weak = socket;
      socket->onFinish = [weak] {
        if (auto target = weak.lock()) target->destroy();
      };
      socket->write(encodeUtf8(response), nullptr);
      socket->end();
    }
  }

  WebSocketServer::WebSocketServer(ServerOptions options) : options_(std::move(options)) {
    if (!options_.noServer) throw TypeError("One and only one of the \"port\", \"server\", or \"noServer\" options must be specified");
    if (options_.perMessageDeflate) throw std::logic_error("rt::ws: permessage-deflate is not provided");
    if (!options_.WebSocket) options_.WebSocket = [](const ServerOptions& serverOptions) { return std::make_shared<ws::WebSocket>(nullptr, serverOptions.autoPong); };
  }

  WebSocketServer::~WebSocketServer() = default;

  WebSocketServerConstructor WebSocketServer::constructor() {
    return [](ServerOptions options) { return std::make_shared<WebSocketServer>(std::move(options)); };
  }

  bool WebSocketServer::shouldHandle(const UpgradeRequest& request) const {
    if (options_.path) {
      const std::size_t index = request.url.find('?');
      const std::string pathname = index != std::string::npos ? request.url.substr(0, index) : request.url;
      if (pathname != *options_.path) return false;
    }
    return true;
  }

  void WebSocketServer::rejectHandshake(UpgradeSocket& socket, int status, std::string message,
                                        std::vector<std::pair<std::string, std::string>> headers) {
    if (options_.onHandshakeRejection) {
      try {
        auto requestId = options_.onHandshakeRejection(status);
        const auto idCharacter = [](char ch) { return ch == '-' || (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); };
        if (requestId && requestId->size() == 36 && std::all_of(requestId->begin(), requestId->end(), idCharacter)) {
          headers.emplace_back("x-uos-request-id", std::move(*requestId));
        }
      } catch (...) {
        // The transport observer never changes handshake behavior.
      }
    }
    abortHandshake(socket, status, std::move(message), std::move(headers));
  }

  void WebSocketServer::handleUpgrade(const UpgradeRequest& request, UpgradeSocket& socket, Bytes head, UpgradeCallback cb) {
    const std::optional<std::string> key = request.header("sec-websocket-key");
    const std::optional<std::string> upgrade = request.header("upgrade");
    const std::optional<std::string> versionHeader = request.header("sec-websocket-version");
    const double version = versionHeader ? stringToNumber(*versionHeader) : std::nan("");
    if (request.method != "GET") return rejectHandshake(socket, 405, "Invalid HTTP method");
    if (!upgrade || toLowerCase(*upgrade) != "websocket") return rejectHandshake(socket, 400, "Invalid Upgrade header");
    if (!key || !validKey(*key)) return rejectHandshake(socket, 400, "Missing or invalid Sec-WebSocket-Key header");
    if (version != 13 && version != 8) return rejectHandshake(socket, 400, "Missing or invalid Sec-WebSocket-Version header", {{"Sec-WebSocket-Version", "13, 8"}});
    if (!shouldHandle(request)) return rejectHandshake(socket, 400);
    std::vector<std::string> protocols;
    if (const auto secWebSocketProtocol = request.header("sec-websocket-protocol")) {
      auto offered = parseSubprotocols(*secWebSocketProtocol);
      if (!offered) return rejectHandshake(socket, 400, "Invalid Sec-WebSocket-Protocol header");
      protocols = std::move(*offered);
    }
    completeUpgrade(*key, protocols, request, socket, std::move(head), std::move(cb));
  }

  void WebSocketServer::completeUpgrade(const std::string& key, const std::vector<std::string>& protocols, const UpgradeRequest& request, UpgradeSocket& socket, Bytes head, UpgradeCallback cb) {
    const auto self = weak_from_this().lock();
    // Destroy the socket if the client has already sent a FIN packet.
    if (!socket.readable || !socket.writable) {
      takeSocket(socket)->destroy();
      return;
    }
    if (state_ > RUNNING) return rejectHandshake(socket, 503);
    const std::string digest = detail::handshakeAccept(key);
    std::vector<std::string> headers{"HTTP/1.1 101 Switching Protocols", "Upgrade: websocket", "Connection: Upgrade", "Sec-WebSocket-Accept: " + digest};
    std::shared_ptr<ws::WebSocket> websocket = options_.WebSocket(options_);
    if (!protocols.empty()) {
      const std::optional<std::string> protocol = options_.handleProtocols ? options_.handleProtocols(protocols, request) : std::optional<std::string>(protocols.front());
      if (protocol && !protocol->empty()) {
        headers.push_back("Sec-WebSocket-Protocol: " + *protocol);
        websocket->state().protocol = *protocol;
      }
    }
    // Allow external modification/inspection of handshake headers.
    EventArgs args;
    args.headers = &headers;
    args.request = &request;
    emit("headers", args);
    auto stream = takeSocket(socket);
    std::string response;
    for (const std::string& line : headers) response += line + "\r\n";
    response += "\r\n";
    stream->write(encodeUtf8(response), nullptr);
    detail::setSocket(websocket, stream, std::move(head), options_.maxPayload, options_.skipUTF8Validation);
    if (options_.clientTracking) {
      clients_.push_back(websocket);
      std::weak_ptr<WebSocketServer> weakServer = weak_from_this();
      std::weak_ptr<ws::WebSocket> weakSocket = websocket;
      websocket->on("close", [weakServer, weakSocket] {
        auto server = weakServer.lock();
        if (!server) return;
        auto closed = weakSocket.lock();
        std::erase_if(server->clients_, [&](const std::shared_ptr<ws::WebSocket>& client) { return client == closed; });
        if (server->shouldEmitClose_ && server->clients_.empty()) detail::nextTick([server] { server->emitClose(); });
      });
    }
    cb(websocket, request);
  }

  void WebSocketServer::emitClose() {
    state_ = CLOSED_STATE;
    detail::guarded([&] { emit("close"); });
  }

  void WebSocketServer::close() { close(std::function<void(Reason)>{}); }

  void WebSocketServer::close(std::function<void(Reason error)> cb) {
    std::shared_ptr<WebSocketServer> self = weak_from_this().lock();
    if (state_ == CLOSED_STATE) {
      if (cb) once("close", [cb] { cb(std::make_exception_ptr(JsError("The server is not running"))); });
      if (self) detail::nextTick([self] { self->emitClose(); });
      return;
    }
    if (cb) once("close", [cb] { cb(nullptr); });
    if (state_ == CLOSING_STATE) return;
    state_ = CLOSING_STATE;
    if (options_.clientTracking && !clients_.empty()) {
      shouldEmitClose_ = true;
      return;
    }
    if (self) detail::nextTick([self] { self->emitClose(); });
  }

  std::vector<std::shared_ptr<WebSocket>> WebSocketServer::clients() const { return clients_; }
}
