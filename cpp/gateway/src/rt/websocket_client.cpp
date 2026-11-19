// ws's initAsClient (lib/websocket.js) and the node:http ClientRequest it
// drives: Node's request head (header order, Host, Basic auth, header
// validation), the connection (TCP, Unix socket, TLS), the response head
// parser and the "response" / "upgrade" / "timeout" / "error" events.
#include "websocket_internal.hpp"

#include "fetch_transport_internal.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/url.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/websocket/detail/hybi13.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace asio = boost::asio;

namespace uos::rt::ws::detail {
  namespace {
    // node:http's default limit on a response head.
    constexpr std::size_t MAX_HEADER_SIZE = std::min<std::size_t>(16 * 1024, limits::kHttpHeaderBlockBytes);

    Reason parseError(std::string reason, std::string code) { return codedError("Parse Error: " + reason, std::move(code)); }

    bool isTokenChar(unsigned char c) {
      if (c >= '0' && c <= '9') return true;
      if (c >= 'a' && c <= 'z') return true;
      if (c >= 'A' && c <= 'Z') return true;
      switch (c) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
          return true;
        default:
          return false;
      }
    }

    // Wire bytes as the code points U+0000..U+00FF (node:http reads heads as Latin-1).
    std::string latin1(BytesView bytes) {
      std::string out;
      for (const std::uint8_t byte : bytes) appendCodePoint(out, byte);
      return out;
    }

    // A header string (code points up to U+00FF, validated) as Latin-1 bytes.
    void appendLatin1(Bytes& out, std::string_view text) {
      for (const std::uint32_t point : codePoints(text)) out.push_back(static_cast<std::uint8_t>(point));
    }

    // `checkIsHttpToken(name)`.
    bool validHeaderName(std::string_view name) {
      if (name.empty()) return false;
      return std::all_of(name.begin(), name.end(), [](char c) { return isTokenChar(static_cast<unsigned char>(c)); });
    }

    // `checkInvalidHeaderChar(value)`: /[^\t\x20-\x7e\x80-\xff]/
    bool validHeaderValue(std::string_view value) {
      for (const std::uint32_t point : codePoints(value)) {
        if (point == '\t') continue;
        if ((point >= 0x20 && point <= 0x7e) || (point >= 0x80 && point <= 0xff)) continue;
        return false;
      }
      return true;
    }

    std::string lowerAscii(std::string text) {
      for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      }
      return text;
    }

    // ClientRequest's outgoing headers: setHeader() replaces a name in place
    // (case-insensitively), getHeader() looks one up.
    struct OutgoingHeaders {
      std::vector<std::pair<std::string, std::string>> entries;
      void setHeader(const std::string& name, const std::string& value) {
        if (!validHeaderName(name)) {
          CodedError error("Header name must be a valid HTTP token [\"" + name + "\"]", "ERR_INVALID_HTTP_TOKEN", "TypeError");
          throw error;
        }
        if (!validHeaderValue(value)) {
          CodedError error("Invalid character in header content [\"" + name + "\"]", "ERR_INVALID_CHAR", "TypeError");
          throw error;
        }
        const std::string key = lowerAscii(name);
        for (auto& entry : entries) {
          if (lowerAscii(entry.first) == key) {
            entry = {name, value};
            return;
          }
        }
        entries.emplace_back(name, value);
      }
      bool hasHeader(const std::string& name) const {
        const std::string key = lowerAscii(name);
        return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) { return lowerAscii(entry.first) == key; });
      }
    };

    std::string hostWithPort(const asio::ip::address& address, int port) { return address.to_string() + ":" + std::to_string(port); }
  }

  // ---- response head -----------------------------------------------------------

  std::optional<ParsedResponse> parseResponseHead(BytesView data) {
    const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    namespace http = ::uos::rt::detail::fetch;
    auto parsedHead = http::parseResponseHead(text, MAX_HEADER_SIZE);
    if (parsedHead.status == http::HeadStatus::malformed) {
      const char* const code = llhttp_errno_name(parsedHead.error);
      rethrow(parseError(parsedHead.errorReason ? parsedHead.errorReason : code, code));
    }
    if (parsedHead.status == http::HeadStatus::incomplete) {
      if (data.size() >= MAX_HEADER_SIZE) rethrow(parseError("Header overflow", "HPE_HEADER_OVERFLOW"));
      return std::nullopt;
    }
    const auto asLatin1 = [](std::string value) {
      if (std::any_of(value.begin(), value.end(), [](unsigned char byte) { return byte >= 0x80; })) {
        return latin1(BytesView(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
      }
      return value;
    };
    ParsedResponse parsed;
    parsed.length = parsedHead.length;
    parsed.response.httpVersion = "1." + std::to_string(parsedHead.head.minorVersion);
    parsed.response.statusCode = parsedHead.head.status;
    parsed.response.statusMessage = asLatin1(std::move(parsedHead.head.reason));
    parsed.response.rawHeaders = std::move(parsedHead.head.fields);
    for (auto& field : parsed.response.rawHeaders) field.second = asLatin1(std::move(field.second));
    return parsed;
  }

  // ---- HandshakeRequest ----------------------------------------------------------

  HandshakeRequest::HandshakeRequest(Target target, std::string head, std::optional<double> timeout) : target_(std::move(target)), head_(std::move(head)), timeout_(timeout) {}

  HandshakeRequest::~HandshakeRequest() = default;

  void HandshakeRequest::armTimer() {
    if (!timeout_ || !(*timeout_ > 0) || !std::isfinite(*timeout_)) return;
    std::weak_ptr<HandshakeRequest> weak = weak_from_this();
    timer_ = rt::detail::createTimer(
      [weak] {
        auto self = weak.lock();
        if (!self || self->aborted) return;
        auto listener = self->onTimeout;
        if (listener) guarded([&] { listener(); });
      },
      *timeout_, false);
  }

  void HandshakeRequest::end() {
    armTimer();
    std::shared_ptr<HandshakeRequest> self = shared_from_this();
    if (target_.ipc) {
      auto local = std::make_shared<asio::local::stream_protocol::socket>(rt::executor());
      cancel_ = [local] {
        boost::system::error_code ignored;
        local->close(ignored);
      };
      local->async_connect(asio::local::stream_protocol::endpoint(target_.socketPath), [self, local](const boost::system::error_code& error) {
        guarded([&] {
          self->cancel_ = nullptr;
          if (self->aborted || self->finished_) return;
          if (error) return self->fail(systemError(error.value(), "connect", " " + self->target_.socketPath));
          self->connected(Socket::Stream(std::move(*local)));
        });
      });
      return;
    }
    // Connects to each address in turn, like net.connect with several records.
    auto attempt = std::make_shared<std::function<void(std::vector<asio::ip::tcp::endpoint>, std::size_t)>>();
    const std::weak_ptr<std::function<void(std::vector<asio::ip::tcp::endpoint>, std::size_t)>> weakAttempt = attempt;
    *attempt = [self, weakAttempt](std::vector<asio::ip::tcp::endpoint> endpoints, std::size_t index) {
      auto nextAttempt = weakAttempt.lock();
      if (!nextAttempt) return;
      auto tcp = std::make_shared<asio::ip::tcp::socket>(rt::executor());
      self->cancel_ = [tcp] {
        boost::system::error_code ignored;
        tcp->close(ignored);
      };
      const asio::ip::tcp::endpoint endpoint = endpoints[index];
      tcp->async_connect(endpoint, [self, attempt = std::move(nextAttempt), tcp, endpoints, index, endpoint](const boost::system::error_code& error) {
        guarded([&] {
          self->cancel_ = nullptr;
          if (self->aborted || self->finished_) {
            *attempt = nullptr;
            return;
          }
          if (error) {
            if (index + 1 < endpoints.size()) return (*attempt)(endpoints, index + 1);
            *attempt = nullptr;
            return self->fail(systemError(error.value(), "connect", " " + hostWithPort(endpoint.address(), endpoint.port())));
          }
          *attempt = nullptr;
          self->connected(Socket::Stream(std::move(*tcp)));
        });
      });
    };
    boost::system::error_code invalid;
    const asio::ip::address address = asio::ip::make_address(target_.host, invalid);
    if (!invalid) {
      std::vector<asio::ip::tcp::endpoint> endpoints{asio::ip::tcp::endpoint(address, static_cast<unsigned short>(target_.port))};
      (*attempt)(std::move(endpoints), 0);
      return;
    }
    auto resolver = std::make_shared<asio::ip::tcp::resolver>(rt::executor());
    cancel_ = [resolver] { resolver->cancel(); };
    resolver->async_resolve(target_.host, std::to_string(target_.port), [self, attempt, resolver](const boost::system::error_code& error, asio::ip::tcp::resolver::results_type results) {
      guarded([&] {
        self->cancel_ = nullptr;
        if (self->aborted || self->finished_) {
          *attempt = nullptr;
          return;
        }
        if (error || results.empty()) {
          *attempt = nullptr;
          const bool again = error == asio::error::host_not_found_try_again;
          CodedError failure("getaddrinfo " + std::string(again ? "EAI_AGAIN" : "ENOTFOUND") + " " + self->target_.host, again ? "EAI_AGAIN" : "ENOTFOUND");
          failure.errno_ = again ? -3001 : -3008;
          failure.syscall = "getaddrinfo";
          return self->fail(std::make_exception_ptr(std::move(failure)));
        }
        std::vector<asio::ip::tcp::endpoint> endpoints;
        for (const auto& entry : results) endpoints.push_back(entry.endpoint());
        (*attempt)(std::move(endpoints), 0);
      });
    });
  }

  void HandshakeRequest::connected(Socket::Stream stream) {
    socket = std::make_shared<Socket>(std::move(stream));
    std::weak_ptr<HandshakeRequest> weak = weak_from_this();
    socket->onActivity = [weak] {
      if (auto self = weak.lock()) {
        if (self->timer_) self->timer_.refresh();
      }
    };
    socket->onError = [weak](Reason error) {
      if (auto self = weak.lock()) self->fail(std::move(error));
    };
    socket->onClose = [weak] {
      auto self = weak.lock();
      if (!self) return;
      if (!self->finished_) self->fail(codedError("socket hang up", "ECONNRESET"));
      clearInterval(self->timer_);
      self->releaseListeners();
    };
    if (target_.secure) {
      socket->startTls(target_.tls, [weak](Reason error) {
        auto self = weak.lock();
        if (!self || self->aborted || self->finished_) return;
        if (error) return self->fail(std::move(error));
        self->secure();
      });
      return;
    }
    secure();
  }

  void HandshakeRequest::secure() {
    std::weak_ptr<HandshakeRequest> weak = weak_from_this();
    socket->onData = [weak](BytesView chunk) {
      if (auto self = weak.lock()) self->onData(chunk);
    };
    socket->onEnd = [weak] {
      if (auto self = weak.lock()) self->onEnd();
    };
    Bytes head;
    appendLatin1(head, head_);
    socket->write(std::move(head), nullptr);
    socket->resume();
  }

  void HandshakeRequest::onEnd() {
    if (!finished_) fail(codedError("socket hang up", "ECONNRESET"));
  }

  void HandshakeRequest::onData(BytesView chunk) {
    if (responded_ || finished_) return;
    received_.insert(received_.end(), chunk.begin(), chunk.end());
    std::optional<ParsedResponse> parsed;
    try {
      parsed = parseResponseHead(received_);
    } catch (...) {
      return fail(std::current_exception());
    }
    if (!parsed) return;
    responded_ = true;
    auto response = std::make_shared<const IncomingResponse>(std::move(parsed->response));
    Bytes rest(received_.begin() + static_cast<std::ptrdiff_t>(parsed->length), received_.end());
    received_.clear();
    // Bun's client reports an upgrade for an HTTP/1.1 101 response that names a protocol.
    if (response->statusCode == 101 && response->header("upgrade") && response->httpVersion == "1.1") {
      finished_ = true;
      clearInterval(timer_);
      std::shared_ptr<Socket> upgraded = socket;
      upgraded->onData = nullptr;
      upgraded->onEnd = nullptr;
      upgraded->onError = nullptr;
      upgraded->onClose = nullptr;
      upgraded->onActivity = nullptr;
      upgraded->pause();
      auto listener = onUpgrade;
      releaseListeners();
      if (listener) listener(response, upgraded, std::move(rest));
      return;
    }
    // The rest of the response is read and discarded; the request ends with the
    // connection. Bun's request stops its idle timer once the response arrived.
    finished_ = true;
    clearInterval(timer_);
    socket->onError = nullptr;
    socket->onActivity = nullptr;
    auto listener = onResponse;
    if (listener) listener(response);
  }

  void HandshakeRequest::fail(Reason error) {
    if (finished_) return;
    finished_ = true;
    clearInterval(timer_);
    if (socket && !socket->destroyed()) socket->destroy();
    auto listener = onErrorEvent;
    releaseListeners();
    if (listener && !aborted) listener(std::move(error));
  }

  void HandshakeRequest::abort() {
    aborted = true;
    clearInterval(timer_);
    if (auto cancel = std::move(cancel_)) {
      cancel_ = nullptr;
      cancel();
    }
    releaseListeners();
  }

  void HandshakeRequest::releaseListeners() {
    onTimeout = nullptr;
    onErrorEvent = nullptr;
    onResponse = nullptr;
    onUpgrade = nullptr;
  }

  // ---- initAsClient ----------------------------------------------------------------

  void initAsClient(const std::shared_ptr<WebSocket>& websocket, const std::string& address, ClientOptions options) {
    WebSocket::State& state = websocket->state();
    state.autoPong = options.autoPong;
    std::optional<URL> parsed;
    try {
      parsed.emplace(address);
    } catch (const TypeError&) {
      throw SyntaxError("Invalid URL: " + address);
    }
    URL& parsedUrl = *parsed;
    if (parsedUrl.protocol() == "http:") parsedUrl.protocol("ws:");
    else if (parsedUrl.protocol() == "https:") parsedUrl.protocol("wss:");
    state.url = parsedUrl.href();
    const bool isSecure = parsedUrl.protocol() == "wss:";
    const bool isIpcUrl = parsedUrl.protocol() == "ws+unix:";
    std::optional<std::string> invalidUrlMessage;
    if (parsedUrl.protocol() != "ws:" && !isSecure && !isIpcUrl) invalidUrlMessage = "The URL's protocol must be one of \"ws:\", \"wss:\", \"http:\", \"https:\", or \"ws+unix:\"";
    else if (isIpcUrl && parsedUrl.pathname().empty()) invalidUrlMessage = "The URL's pathname is empty";
    else if (!parsedUrl.hash().empty()) invalidUrlMessage = "The URL contains a fragment identifier";
    if (invalidUrlMessage) throw SyntaxError(*invalidUrlMessage);

    const int defaultPort = isSecure ? 443 : 80;
    boost::beast::websocket::detail::sec_ws_key_type nonce;
    boost::beast::websocket::detail::make_sec_ws_key(nonce);
    const std::string key(nonce.data(), nonce.size());
    std::string hostname = parsedUrl.hostname();
    if (!hostname.empty() && hostname.front() == '[') hostname = hostname.substr(1, hostname.size() - 2);
    const std::string portText = parsedUrl.port();
    const int port = portText.empty() ? defaultPort : std::stoi(portText);

    // `{ ...opts.headers, 'Sec-WebSocket-Version': 13, ... }`: a JS record.
    std::vector<std::pair<std::string, std::string>> record;
    const auto put = [&](const std::string& name, const std::string& value) {
      for (auto& entry : record) {
        if (entry.first == name) {
          entry.second = value;
          return;
        }
      }
      record.emplace_back(name, value);
    };
    for (const auto& [name, value] : options.headers) put(name, value);
    put("Sec-WebSocket-Version", "13");
    put("Sec-WebSocket-Key", key);
    put("Connection", "Upgrade");
    put("Upgrade", "websocket");
    if (options.origin && !options.origin->empty()) put("Origin", *options.origin);
    std::optional<std::string> auth;
    if (!parsedUrl.username().empty() || !parsedUrl.password().empty()) auth = parsedUrl.username() + ":" + parsedUrl.password();

    std::string path = parsedUrl.pathname() + parsedUrl.search();
    std::string socketPath;
    if (isIpcUrl) {
      const std::vector<std::string> parts = split(path, ":");
      socketPath = parts[0];
      path = parts.size() > 1 ? parts[1] : "";
    }
    if (path.empty()) path = "/";

    // http.request(opts): the request head.
    OutgoingHeaders headers;
    for (const auto& [name, value] : record) headers.setHeader(name, value);
    if (!hostname.empty() && !headers.hasHeader("host")) {
      std::string hostHeader = hostname;
      const std::size_t colon = hostHeader.find(':');
      if (colon != std::string::npos && hostHeader.find(':', colon + 1) != std::string::npos && hostHeader.front() != '[') hostHeader = "[" + hostHeader + "]";
      if (port != defaultPort) hostHeader += ":" + std::to_string(port);
      headers.setHeader("Host", hostHeader);
    }
    if (auth && !headers.hasHeader("authorization")) headers.setHeader("Authorization", "Basic " + toBase64(encodeUtf8(*auth)));
    for (const std::uint32_t point : codePoints(path)) {
      if (point < 0x21 || point > 0xff) throw CodedError("Request path contains unescaped characters", "ERR_UNESCAPED_CHARACTERS", "TypeError");
    }
    std::string requestHead = "GET " + path + " HTTP/1.1\r\n";
    for (const auto& [name, value] : headers.entries) requestHead += name + ": " + value + "\r\n";
    requestHead += "\r\n";

    HandshakeRequest::Target target;
    target.ipc = isIpcUrl;
    target.socketPath = socketPath;
    target.host = hostname;
    target.port = port;
    target.secure = isSecure;
    target.tls = Socket::TlsOptions{.host = hostname, .ca = options.ca, .rejectUnauthorized = options.rejectUnauthorized, .servername = options.servername};
    const std::optional<double> timeout = options.handshakeTimeout;
    auto request = std::make_shared<HandshakeRequest>(std::move(target), std::move(requestHead), timeout);
    state.req = request;
    std::weak_ptr<HandshakeRequest> weakRequest = request;
    const double maxPayload = options.maxPayload;
    const bool skipUTF8Validation = options.skipUTF8Validation;

    if (timeout && *timeout > 0 && std::isfinite(*timeout)) {
      request->onTimeout = [websocket, weakRequest] {
        if (auto current = weakRequest.lock()) abortHandshakeRequest(websocket, current, "Opening handshake has timed out");
      };
    }
    request->onErrorEvent = [websocket](Reason error) {
      WebSocket::State& current = websocket->state();
      if (!current.req || current.req->aborted) return;
      current.req = nullptr;
      emitErrorAndClose(websocket, std::move(error));
    };
    request->onResponse = [websocket, weakRequest](std::shared_ptr<const IncomingResponse> response) {
      auto current = weakRequest.lock();
      EventArgs args;
      args.response = response;
      bool listened = false;
      try {
        listened = websocket->emit("unexpected-response", args);
      } catch (...) {
        reportUncaught(std::current_exception());
        return;
      }
      if (!listened) abortHandshakeRequest(websocket, current, "Unexpected server response: " + std::to_string(response->statusCode));
    };
    request->onUpgrade = [websocket, key, maxPayload, skipUTF8Validation](std::shared_ptr<const IncomingResponse> response, std::shared_ptr<Socket> socket, Bytes head) {
      EventArgs args;
      args.response = response;
      guarded([&] { websocket->emit("upgrade", args); });
      // The user may have closed the connection from a listener of the
      // `'upgrade'` event.
      WebSocket::State& current = websocket->state();
      if (current.readyState != WebSocket::CONNECTING) return;
      current.req = nullptr;
      const std::optional<std::string> upgrade = response->header("upgrade");
      if (!upgrade || toLowerCase(*upgrade) != "websocket") return abortHandshakeSocket(websocket, socket, "Invalid Upgrade header");
      const std::string digest = handshakeAccept(key);
      if (response->header("sec-websocket-accept") != digest) return abortHandshakeSocket(websocket, socket, "Invalid Sec-WebSocket-Accept header");
      if (response->header("sec-websocket-protocol")) return abortHandshakeSocket(websocket, socket, "Server sent a subprotocol but none was requested");
      if (response->header("sec-websocket-extensions"))
        return abortHandshakeSocket(websocket, socket, "Server sent a Sec-WebSocket-Extensions header but no extension was requested");
      setSocket(websocket, socket, std::move(head), maxPayload, skipUTF8Validation);
    };
    request->end();
  }
}
