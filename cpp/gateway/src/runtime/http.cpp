#include "uos/runtime/http.hpp"

#include "uos/rt/crypto.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/url.hpp"
#include "src/rt/url_limits.hpp"

#include "third_party/llhttp/llhttp.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/v6_only.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <deque>
#include <exception>
#include <utility>

namespace uos::runtime::http {
  namespace {
    void reportRejection(const std::function<void(const TransportRequestRejection&)>& observer, std::optional<int> status,
                         std::string_view reason, std::string_view omission) noexcept {
      if (!observer) return;
      try {
        observer({rt::crypto::randomUUID(), status, reason, omission});
      } catch (...) {
        // Observability cannot change rejection or socket behavior.
      }
    }
  }

  namespace node_http {
    namespace asio = boost::asio;
    namespace ws = rt::ws;

    // ---- node:events ------------------------------------------------------------------

    void Emitter::on(std::string_view event, const IncomingListener& listener) { registrations_.push_back(Registration{std::string(event), listener, false, nullptr}); }

    void Emitter::once(std::string_view event, const IncomingListener& listener) {
      registrations_.push_back(Registration{std::string(event), listener, true, std::make_shared<bool>(false)});
    }

    void Emitter::removeListener(std::string_view event, const IncomingListener& listener) {
      for (auto position = registrations_.rbegin(); position != registrations_.rend(); ++position) {
        if (position->event == event && position->listener == listener) {
          registrations_.erase(std::next(position).base());
          return;
        }
      }
    }

    std::size_t Emitter::listenerCount(std::string_view event) const {
      return static_cast<std::size_t>(std::count_if(registrations_.begin(), registrations_.end(), [event](const Registration& registration) { return registration.event == event; }));
    }

    bool Emitter::emit(std::string_view event, const IncomingEvent& value) {
      std::vector<Registration> snapshot;
      for (const Registration& registration : registrations_)
        if (registration.event == event) snapshot.push_back(registration);
      if (snapshot.empty()) return false;
      for (const Registration& registration : snapshot) {
        if (registration.once) {
          // node:events' onceWrapper: removed, then called once (a once listener removed
          // during this emit is still called, as the snapshot holds its wrapper).
          if (*registration.fired) continue;
          *registration.fired = true;
          for (auto position = registrations_.begin(); position != registrations_.end(); ++position) {
            if (position->fired == registration.fired) {
              registrations_.erase(position);
              break;
            }
          }
        }
        registration.listener(value);
      }
      return true;
    }

    // ---- status lines -------------------------------------------------------------------

    std::string_view reasonPhrase(int statusCode) {
      switch (statusCode) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 102: return "Processing";
        case 103: return "Early Hints";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 203: return "Non-Authoritative Information";
        case 204: return "No Content";
        case 205: return "Reset Content";
        case 206: return "Partial Content";
        case 207: return "Multi-Status";
        case 208: return "Already Reported";
        case 226: return "IM Used";
        case 300: return "Multiple Choices";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 305: return "Use Proxy";
        case 306: return "Switch Proxy";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 402: return "Payment Required";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 407: return "Proxy Authentication Required";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 412: return "Precondition Failed";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 416: return "Range Not Satisfiable";
        case 417: return "Expectation Failed";
        case 418: return "I'm a Teapot";
        case 421: return "Misdirected Request";
        case 422: return "Unprocessable Entity";
        case 423: return "Locked";
        case 424: return "Failed Dependency";
        case 425: return "Too Early";
        case 426: return "Upgrade Required";
        case 428: return "Precondition Required";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 451: return "Unavailable For Legal Reasons";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        case 506: return "Variant Also Negotiates";
        case 507: return "Insufficient Storage";
        case 508: return "Loop Detected";
        case 510: return "Not Extended";
        case 511: return "Network Authentication Required";
        default: return "HM";
      }
    }

    namespace {
      // Bun's parser limits (a head of at most 16408 bytes, at most 198 fields).
      constexpr std::size_t MAX_HEAD_BYTES = std::min<std::size_t>(16408, rt::limits::kHttpHeaderBlockBytes);
      constexpr std::size_t MAX_HEADER_FIELDS = std::min<std::size_t>(198, rt::limits::kHttpHeaderFields);
      constexpr std::size_t MAX_LENGTH_DIGITS = 15;
      constexpr std::string_view BAD_REQUEST = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
      constexpr std::string_view BODY_OVERFLOW = "HTTP/1.1 413 Payload Too Large\r\nConnection: close\r\n\r\n";
      constexpr std::string_view HEADER_OVERFLOW = "HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\n\r\n";
      // ServerResponse.writableHighWaterMark in Bun.
      constexpr std::size_t HIGH_WATER_MARK = 65536;
      // Bytes of a following request buffered while a response is in progress.
      constexpr std::size_t PIPELINE_LIMIT = 65536;
      constexpr std::size_t READ_SIZE = 65536;
      // How long a closing connection keeps reading (and discarding) after its last
      // response, so unread request bytes do not turn the close into a reset.
      constexpr double LINGER_MS = 2000;

      std::size_t requestBodyLimit(std::string_view method, std::string_view target, const std::optional<std::string_view>& contentType) {
        if (method != "POST") return rt::limits::kHttpBodyBytes;
        try {
          const auto path = rt::URL(target, "http://localhost").pathname();
          if (std::string_view(path).substr(0, path.find_last_not_of('/') + 1) == "/v1/images/edits") {
            const auto type = contentType.value_or("");
            if (rt::toLowerCase(rt::trim(type.substr(0, type.find(';')))) == "multipart/form-data") return rt::limits::kImageEditMultipartBytes;
            return rt::limits::kImageEditJsonBytes;
          }
        } catch (const rt::TypeError&) {
          // The request adapter reports an invalid target through its existing error path.
        }
        return rt::limits::kHttpBodyBytes;
      }

      bool isChatPost(std::string_view method, std::string_view target) {
        if (method != "POST") return false;
        try {
          const auto path = rt::URL(target, "http://localhost").pathname();
          return std::string_view(path).substr(0, path.find_last_not_of('/') + 1) == "/v1/chat/completions";
        } catch (const rt::TypeError&) {
          return false;
        }
      }

      bool isTokenChar(unsigned char character) {
        if (character >= '0' && character <= '9') return true;
        if ((character | 0x20) >= 'a' && (character | 0x20) <= 'z') return true;
        return std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(character)) != std::string_view::npos;
      }

      bool isToken(std::string_view text) {
        return !text.empty() && std::all_of(text.begin(), text.end(), [](char character) { return isTokenChar(static_cast<unsigned char>(character)); });
      }

      // Each wire byte b as the code point U+00b.
      std::string latin1(std::string_view bytes) {
        std::string out;
        out.reserve(bytes.size());
        for (const char byte : bytes) rt::appendCodePoint(out, static_cast<unsigned char>(byte));
        return out;
      }

      // A header string written to the wire: code points up to U+00FF as one byte each.
      std::string wireBytes(std::string_view text) {
        if (rt::isAscii(text)) return std::string(text);
        std::string out;
        for (const std::uint32_t codePoint : rt::codePoints(text)) {
          if (codePoint <= 0xFF) out.push_back(static_cast<char>(codePoint));
          else rt::appendCodePoint(out, codePoint);
        }
        return out;
      }

      std::string lowerAscii(std::string_view text) {
        std::string out(text);
        for (char& character : out)
          if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
        return out;
      }

      std::string trimOws(std::string_view text) {
        std::size_t start = 0;
        std::size_t end = text.size();
        while (start < end && (text[start] == ' ' || text[start] == '\t')) ++start;
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
        return std::string(text.substr(start, end - start));
      }

      // Whether the comma-separated header value lists `token` (case-insensitive).
      bool listsToken(std::string_view value, std::string_view token) {
        std::size_t start = 0;
        while (start <= value.size()) {
          const std::size_t comma = value.find(',', start);
          const std::string item = lowerAscii(trimOws(value.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start)));
          if (item == token) return true;
          if (comma == std::string_view::npos) break;
          start = comma + 1;
        }
        return false;
      }

      // node:http's checkInvalidHeaderChar: only HTAB, 0x20-0x7E and 0x80-0xFF.
      bool validHeaderValue(std::string_view value) {
        for (const std::uint32_t codePoint : rt::codePoints(value)) {
          if (codePoint == '\t') continue;
          if (codePoint < 0x20 || codePoint == 0x7F || codePoint > 0xFF) return false;
        }
        return true;
      }

      rt::Reason codedError(std::string message, std::string code, std::string name = "Error") { return rt::makeReason(ws::CodedError(std::move(message), std::move(code), std::move(name))); }

      std::string utcDate() { return rt::Date(rt::dateNow()).toUTCString(); }

      rt::Bytes bytesOf(std::string_view text) { return rt::Bytes(text.begin(), text.end()); }

      std::string hexSize(std::size_t size) {
        static const char* digits = "0123456789abcdef";
        if (size == 0) return "0";
        std::string out;
        while (size > 0) {
          out.insert(out.begin(), digits[size & 15]);
          size >>= 4;
        }
        return out;
      }
    }

    struct ServerResponse::Head {
      struct Field {
        std::string name;
        std::string lower;
        std::vector<std::string> values;
      };
      std::vector<Field> fields;
      bool headerSent = false;
      bool ended = false;
      bool finished = false;
      bool closeEmitted = false;
      bool destroyed = false;
      bool chunked = false;
      bool hasBody = true;
      bool isHeadRequest = false;
      bool http10 = false;
      // The connection closes after this response (node:http's `_last`).
      bool last = false;
      bool needDrain = false;
    };

    struct Server::Impl {
      Server* owner = nullptr;
      RequestListener listener;
      struct Registration {
        std::string event;
        ServerListener listener;
        bool once = false;
        std::shared_ptr<bool> fired;
      };
      std::vector<Registration> registrations;
      std::unique_ptr<asio::ip::tcp::acceptor> acceptor;
      std::optional<AddressInfo> bound;
      bool listening = false;
      bool closing = false;
      bool closeEmitted = false;
      bool referenced = true;
      bool applicationChatBodyRejection = false;
      std::function<void(const TransportRequestRejection&)> onTransportRejection;
      // The event loop's reference to a listening server (released by close()).
      std::shared_ptr<Server> keepAlive;
      rt::OrderedMap<const ConnectionState*, std::shared_ptr<ConnectionState>> connections;
      rt::Timeout retry;

      std::size_t listenerCount(std::string_view event) const;
      bool emit(std::string_view event, const ServerEventArgs& args);
      void emitRequest(const std::shared_ptr<IncomingMessage>& incoming, const std::shared_ptr<ServerResponse>& outgoing);
      void emitErrorLater(rt::Reason error);
      void connectionClosed(const std::shared_ptr<ConnectionState>& connection);
      void maybeEmitClose();
      void accept();
      void adopt(asio::ip::tcp::socket socket);
    };

    struct RequestParser {
      enum class Head { incomplete, complete, invalid, overflow };
      enum class ChunkPart { size, data, trailers, done };

      // The connection retains the head across reads, which may relocate its buffer.
      struct Span {
        std::uint16_t offset = 0;
        std::uint16_t size = 0;

        void append(const char* base, const char* at, std::size_t length) {
          if (size == 0) offset = static_cast<std::uint16_t>(at - base);
          size = static_cast<std::uint16_t>(size + length);
        }

        std::string_view view(std::string_view text) const { return text.substr(offset, size); }
      };

      struct Field {
        Span name;
        Span value;
      };

      RequestParser() {
        static const llhttp_settings_t settings = [] {
          llhttp_settings_t callbacks{};
          callbacks.on_url = [](llhttp_t* parser, const char* at, std::size_t length) {
            auto& self = from(parser);
            self.target.append(self.headBase, at, length);
            return 0;
          };
          callbacks.on_header_field = [](llhttp_t* parser, const char* at, std::size_t length) -> int {
            if (parser->flags & F_TRAILING) return 0;
            auto& self = from(parser);
            if (self.fieldCount == self.fields.size()) return HPE_USER;
            self.fields[self.fieldCount].name.append(self.headBase, at, length);
            return 0;
          };
          callbacks.on_header_field_complete = [](llhttp_t* parser) -> int {
            if ((parser->flags & F_TRAILING) && ++from(parser).trailerCount > MAX_HEADER_FIELDS) {
              llhttp_set_error_reason(parser, "Too many HTTP trailers");
              return HPE_USER;
            }
            return 0;
          };
          callbacks.on_header_value = [](llhttp_t* parser, const char* at, std::size_t length) {
            if (!(parser->flags & F_TRAILING)) {
              auto& self = from(parser);
              self.fields[self.fieldCount].value.append(self.headBase, at, length);
            }
            return 0;
          };
          callbacks.on_header_value_complete = [](llhttp_t* parser) {
            if (!(parser->flags & F_TRAILING)) {
              auto& self = from(parser);
              ++self.fieldCount;
              if (self.fieldCount < self.fields.size()) self.fields[self.fieldCount] = {};
            }
            return 0;
          };
          callbacks.on_headers_complete = [](llhttp_t*) -> int { return HPE_PAUSED; };
          callbacks.on_chunk_header = [](llhttp_t* parser) -> int {
            from(parser).chunkPart = parser->content_length == 0 ? ChunkPart::trailers : ChunkPart::data;
            return HPE_PAUSED;
          };
          callbacks.on_body = [](llhttp_t* parser, const char* at, std::size_t length) {
            from(parser).payload = {at, length};
            return 0;
          };
          callbacks.on_chunk_complete = [](llhttp_t* parser) -> int {
            auto& self = from(parser);
            if (self.chunkPart == ChunkPart::trailers) return 0;
            self.chunkPart = ChunkPart::size;
            return HPE_PAUSED;
          };
          callbacks.on_message_complete = [](llhttp_t* parser) -> int {
            from(parser).chunkPart = ChunkPart::done;
            return HPE_PAUSED;
          };
          return callbacks;
        }();
        llhttp_init(&wire, HTTP_REQUEST, &settings);
        wire.data = this;
      }

      RequestParser(const RequestParser&) = delete;
      RequestParser& operator=(const RequestParser&) = delete;

      Head parseHead(std::string_view text, std::size_t& consumed) {
        if (text.size() == buffered) return Head::incomplete;
        if (buffered == 0) {
          llhttp_reset(&wire);
          target = {};
          fields[0] = {};
          fieldCount = 0;
          trailerCount = 0;
          bodyTooLarge = false;
          chunkMetadataRemaining = rt::limits::kHttpChunkMetadataBytes;
          chunkPart = ChunkPart::size;
        }
        const std::size_t end = std::min(text.size(), MAX_HEAD_BYTES);
        headBase = text.data();
        const llhttp_errno_t result = llhttp_execute(&wire, headBase + buffered, end - buffered);
        headBase = nullptr;
        if (result == HPE_OK) {
          buffered = end;
          return end == MAX_HEAD_BYTES ? Head::overflow : Head::incomplete;
        }
        if (result == HPE_USER) return Head::overflow;
        if (result != HPE_PAUSED) return Head::invalid;
        consumed = static_cast<std::size_t>(llhttp_get_error_pos(&wire) - text.data());
        buffered = 0;
        return Head::complete;
      }

      // Return one payload fragment per step. The caller copies it before erasing
      // consumed input, then dispatches the data event outside the C callbacks.
      std::optional<std::size_t> decodeChunked(std::string_view text, std::string_view& chunk) {
        const ChunkPart part = chunkPart;
        const std::size_t limit = std::min(chunkMetadataRemaining, part == ChunkPart::size ? rt::limits::kHttpChunkLineBytes : MAX_HEAD_BYTES);
        const bool metadata = part == ChunkPart::size || part == ChunkPart::trailers;
        if (metadata && limit == 0) {
          error = HPE_USER;
          llhttp_set_error_reason(&wire, "Chunk metadata exceeds limit");
          return std::nullopt;
        }
        std::size_t end = metadata ? std::min(text.size(), limit) : text.size();
        // Deliver payload before validating its delimiter, regardless of how the
        // socket split the input. llhttp tracks the bytes left in this chunk.
        if (part == ChunkPart::data && wire.content_length != 0)
          end = static_cast<std::size_t>(std::min<std::uint64_t>(end, wire.content_length));
        if (end == buffered || part == ChunkPart::done) return 0;

        llhttp_resume(&wire);
        error = llhttp_execute(&wire, text.data() + buffered, end - buffered);
        chunk = std::exchange(payload, {});
        if (error != HPE_OK && error != HPE_PAUSED) return std::nullopt;
        if (metadata && error == HPE_OK) {
          if (end == limit) {
            error = HPE_USER;
            llhttp_set_error_reason(&wire, "Chunk metadata exceeds limit");
            return std::nullopt;
          }
          buffered = end;
          return 0;
        }
        const std::size_t consumed = error == HPE_PAUSED ? static_cast<std::size_t>(llhttp_get_error_pos(&wire) - text.data()) : end;
        if (metadata) chunkMetadataRemaining -= consumed;
        if (part == ChunkPart::size) {
          if (text.substr(0, consumed).find_first_not_of("0123456789abcdefABCDEF") > MAX_LENGTH_DIGITS) {
            error = HPE_INVALID_CHUNK_SIZE;
            llhttp_set_error_reason(&wire, "Chunk size exceeds digit limit");
            return std::nullopt;
          }
          if (wire.content_length > bodyBytesRemaining) {
            bodyTooLarge = true;
            error = HPE_USER;
            llhttp_set_error_reason(&wire, "HTTP request body exceeds limit");
            return std::nullopt;
          }
          bodyBytesRemaining -= static_cast<std::size_t>(wire.content_length);
        }
        buffered = 0;
        return consumed;
      }

      BufferedRequestBody snapshotChunked(std::string_view input, std::size_t limit) const {
        if (input.empty()) return {.omission = "body_unread"};
        // Only a parser paused at the untouched body boundary can be forked.
        // Rebind its callback context and stop at the first message end.
        RequestParser decoder;
        decoder.wire = wire;
        decoder.wire.data = &decoder;
        decoder.bodyBytesRemaining = std::min(bodyBytesRemaining, limit);
        auto bytes = std::make_shared<rt::Bytes>();
        bytes->reserve(std::min(input.size(), limit));
        while (!input.empty()) {
          std::string_view chunk;
          const auto consumed = decoder.decodeChunked(input, chunk);
          if (!consumed) return {.omission = decoder.bodyTooLarge ? "body_over_limit" : "body_partial"};
          if (!chunk.empty()) {
            const auto* first = reinterpret_cast<const std::uint8_t*>(chunk.data());
            bytes->insert(bytes->end(), first, first + chunk.size());
          }
          if (decoder.chunkPart == ChunkPart::done) return {.bytes = std::move(bytes)};
          if (*consumed == 0) break;
          input.remove_prefix(*consumed);
        }
        return {.omission = "body_partial", .receivedPrefix = bytes->empty() ? nullptr : std::move(bytes)};
      }

      llhttp_t wire;
      Span target;
      std::array<Field, MAX_HEADER_FIELDS> fields;
      std::size_t fieldCount = 0;
      std::size_t bodyBytesRemaining = rt::limits::kHttpBodyBytes;
      bool bodyTooLarge = false;
      ChunkPart chunkPart = ChunkPart::size;
      llhttp_errno_t error = HPE_OK;

    private:
      static RequestParser& from(llhttp_t* parser) { return *static_cast<RequestParser*>(parser->data); }
      static_assert(MAX_HEAD_BYTES <= UINT16_MAX);
      const char* headBase = nullptr;
      std::size_t buffered = 0;
      std::size_t trailerCount = 0;
      std::size_t chunkMetadataRemaining = rt::limits::kHttpChunkMetadataBytes;
      std::string_view payload;
    };

    // ---- the connection ---------------------------------------------------------------

    struct ConnectionState : std::enable_shared_from_this<ConnectionState> {
      enum class Phase { head, body, waiting, lingering, upgrade, closed };
      enum class Framing { none, length, chunked };

      std::shared_ptr<Server> server;
      ws::UpgradeSocket upgrade;

      explicit ConnectionState(asio::generic::stream_protocol::socket stream) : upgrade{std::move(stream), true, true} {}
      std::optional<std::string> remoteAddress;
      std::optional<double> remotePort;
      std::optional<std::string> remoteFamily;
      std::shared_ptr<Socket> socket;
      Emitter socketEvents;

      rt::Bytes input;
      std::vector<std::uint8_t> readBuffer = std::vector<std::uint8_t>(READ_SIZE);
      bool reading = false;
      bool readEnded = false;

      std::deque<rt::Bytes> output;
      std::size_t outputOffset = 0;
      std::size_t outputBytes = 0;
      bool writing = false;
      bool drainPosted = false;
      // Shut the sending side once the output is flushed (end of a closing connection).
      bool endAfterFlush = false;
      bool sendShut = false;

      Phase phase = Phase::head;
      std::shared_ptr<IncomingMessage> request;
      std::shared_ptr<ServerResponse> response;
      Framing framing = Framing::none;
      std::uint64_t remaining = 0;
      RequestParser parser;
      bool bodyComplete = false;
      bool chunkDecodingStarted = false;
      bool flowing = false;
      bool endScheduled = false;
      bool endEmitted = false;
      // The rest of a request body read and discarded after its response (node:http's req._dump()).
      bool dumping = false;
      bool keepAlive = false;
      std::size_t finishedResponses = 0;
      // Set once ws took the stream of an "upgrade" socket.
      bool handedOver = false;
      bool destroyed = false;

      double headStartedAt = 0;
      double idleSince = 0;
      double lastActivity = 0;
      double lingerUntil = 0;
      rt::Timeout timer;

      double now() const { return rt::performanceNow(); }

      // ---- reading ------------------------------------------------------------------

      void startRead() {
        if (reading || destroyed || !upgrade.stream.is_open()) return;
        reading = true;
        auto self = shared_from_this();
        upgrade.stream.async_read_some(asio::buffer(readBuffer), [self](const boost::system::error_code& error, std::size_t count) { self->onRead(error, count); });
      }

      void pump() {
        if (destroyed || reading || readEnded) return;
        switch (phase) {
          case Phase::head:
            startRead();
            break;
          case Phase::body:
            if (dumping || flowing) startRead();
            break;
          case Phase::waiting:
            if (input.size() < PIPELINE_LIMIT) startRead();
            break;
          case Phase::lingering:
            startRead();
            break;
          case Phase::upgrade:
          case Phase::closed:
            break;
        }
      }

      void onRead(const boost::system::error_code& error, std::size_t count) {
        reading = false;
        if (destroyed || phase == Phase::closed) return;
        if (error == asio::error::operation_aborted) return;
        if (error) {
          if (error == asio::error::eof) onEnd();
          else destroy(errnoReason(error, "read"));
          return;
        }
        lastActivity = now();
        if (phase == Phase::lingering) {
          pump();
          return;
        }
        if (phase == Phase::head && input.empty()) headStartedAt = now();
        input.insert(input.end(), readBuffer.begin(), readBuffer.begin() + static_cast<std::ptrdiff_t>(count));
        processInput();
        rearm();
        pump();
      }

      // The peer ended its side (node:http, httpAllowHalfOpen false: the request in
      // progress is aborted and the socket ends).
      void onEnd() {
        readEnded = true;
        upgrade.readable = false;
        if (phase == Phase::head && server && !input.empty()) {
          reportRejection(server->impl().onTransportRejection, std::nullopt, "transport_truncated_head", "body_unread");
        }
        if (phase == Phase::lingering || phase == Phase::head) {
          destroy(nullptr);
          return;
        }
        if (phase == Phase::body && dumping) {
          destroy(nullptr);
          return;
        }
        if (response && headOf(*response).ended && !headOf(*response).finished) {
          // The response is complete and still flushing: it is delivered, then the socket closes.
          headOf(*response).last = true;
          return;
        }
        destroy(nullptr);
      }

      static rt::Reason errnoReason(const boost::system::error_code& error, const char* syscall) {
        std::string code = "EIO";
        if (error == asio::error::connection_reset) code = "ECONNRESET";
        else if (error == asio::error::broken_pipe) code = "EPIPE";
        else if (error == asio::error::timed_out) code = "ETIMEDOUT";
        auto failure = ws::CodedError(code == "ECONNRESET" ? "read ECONNRESET" : std::string(syscall) + " " + code, code);
        failure.syscall = syscall;
        failure.errno_ = -static_cast<double>(error.value());
        return rt::makeReason(std::move(failure));
      }

      // ---- parsing ------------------------------------------------------------------

      void processInput() {
        for (;;) {
          if (destroyed) return;
          if (phase == Phase::head) {
            if (!parseHead()) return;
            continue;
          }
          if (phase == Phase::body) {
            if (dumping || flowing) decodeBody();
            return;
          }
          return;
        }
      }

      // Refuses the request before it reached the server, the way Bun's parser does.
      void refuse(std::string_view head) {
        phase = Phase::lingering;
        write(bytesOf(head));
        closeGracefully();
      }

      void refuseHead(std::string_view head, int status) {
        refuse(head);
        if (!server) return;
        if (status == 413) reportRejection(server->impl().onTransportRejection, status, "transport_body_limit", "body_over_limit");
        else reportRejection(server->impl().onTransportRejection, status,
                             status == 431 ? "transport_header_limit" : "transport_invalid_head", "body_unread");
      }

      // Parses one request head from `input`; true when a request was dispatched and the
      // remaining input should be processed in the new phase.
      bool parseHead() {
        const std::string_view text(reinterpret_cast<const char*>(input.data()), input.size());
        std::size_t consumed = 0;
        const RequestParser::Head parsed = parser.parseHead(text, consumed);
        if (parsed == RequestParser::Head::incomplete) return false;
        if (parsed != RequestParser::Head::complete) {
          if (parsed == RequestParser::Head::overflow) refuseHead(HEADER_OVERFLOW, 431);
          else refuseHead(BAD_REQUEST, 400);
          return false;
        }

        const auto methodCode = static_cast<llhttp_method_t>(llhttp_get_method(&parser.wire));
        if (methodCode == HTTP_CONNECT) {
          destroy(nullptr);
          return false;
        }
        const std::string_view method = llhttp_method_name(methodCode);
        const std::string_view target = parser.target.view(text);
        const bool http11 = llhttp_get_http_minor(&parser.wire) == 1;
        const auto suffix = http11 ? " HTTP/1.1\r\n" : " HTTP/1.0\r\n";
        // Require single spaces and CRLF in the request line. The gateway routes
        // origin and absolute targets and has no server-wide OPTIONS target.
        auto requestLine = text;
        while (requestLine.starts_with("\r\n")) requestLine.remove_prefix(2);
        const std::size_t targetStart = text.size() - requestLine.size() + method.size() + 1;
        if (target.empty() || target == "*" || parser.target.offset != targetStart || text[targetStart - 1] != ' ' ||
            !text.substr(parser.target.offset + target.size()).starts_with(suffix)) {
          refuseHead(BAD_REQUEST, 400);
          return false;
        }
        const auto targetPath = target.substr(0, target.find('#'));
        if (const auto query = targetPath.find('?'); query != std::string_view::npos && !rt::detail::boundedUrlQuery(targetPath.substr(query))) {
          refuseHead(BAD_REQUEST, 400);
          return false;
        }

        // llhttp checks conflicting lengths before pausing at the head. A
        // non-final or absent chunked coding cannot delimit a request body.
        const bool chunked = (parser.wire.flags & F_CHUNKED) != 0;
        if ((parser.wire.flags & F_TRANSFER_ENCODING) && !chunked) {
          refuseHead(BAD_REQUEST, 400);
          return false;
        }
        bool hasHost = false;
        bool isUpgrade = false;
        bool expectContinue = false;
        std::optional<std::string_view> contentType;
        std::vector<std::string> rawHeaders;
        rawHeaders.reserve(parser.fieldCount * 2);
        for (std::size_t i = 0; i < parser.fieldCount; ++i) {
          const auto name = parser.fields[i].name.view(text);
          auto value = parser.fields[i].value.view(text);
          const auto last = value.find_last_not_of(" \t");
          value = last == std::string_view::npos ? std::string_view{} : value.substr(0, last + 1);
          const std::string lower = lowerAscii(name);
          if (lower == "content-length" && value.size() > MAX_LENGTH_DIGITS) {
            refuseHead(BAD_REQUEST, 400);
            return false;
          }
          if (lower == "content-type") {
            if (!contentType) contentType = value;
            else if (contentType->find(';') == std::string_view::npos) contentType = std::string_view{};
          }
          hasHost = hasHost || lower == "host";
          expectContinue = expectContinue || (lower == "expect" && lowerAscii(value) == "100-continue");
          // Proxy-Connection can set llhttp's upgrade flag. Only Connection
          // has authority to switch away from HTTP on this server.
          isUpgrade = isUpgrade || (lower == "connection" && llhttp_get_upgrade(&parser.wire) && listsToken(value, "upgrade"));
          rawHeaders.emplace_back(name);
          rawHeaders.push_back(rt::isAscii(value) ? std::string(value) : latin1(value));
        }
        if (http11 && !hasHost) {
          refuseHead("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nDate: " + utcDate() + "\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 400);
          return false;
        }
        keepAlive = http11 && llhttp_should_keep_alive(&parser.wire);
        remaining = parser.wire.content_length;
        framing = chunked ? Framing::chunked : remaining > 0 ? Framing::length : Framing::none;
        parser.bodyBytesRemaining = chunked || remaining > rt::limits::kHttpBodyBytes
                                      ? requestBodyLimit(method, target, contentType) : rt::limits::kHttpBodyBytes;
        const bool oversized = remaining > parser.bodyBytesRemaining;
        if (oversized) {
          if (chunked || isUpgrade || !server || !server->impl().applicationChatBodyRejection || !isChatPost(method, target)) {
            refuseHead(BODY_OVERFLOW, 413);
            return false;
          }
          // Dispatch only the head. The application rejects Content-Length at the
          // same cap and supplies the authenticated Chat error response.
          keepAlive = false;
        }

        auto self = shared_from_this();
        auto incoming = std::make_shared<IncomingMessage>(self, socket);
        incoming->method = method;
        incoming->url = rt::decodeUtf8(target, {.ignoreBOM = true});
        incoming->httpVersion = http11 ? "1.1" : "1.0";
        incoming->rawHeaders = std::move(rawHeaders);
        if (oversized) input.clear();
        else input.erase(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(consumed));
        if (isUpgrade && server && server->impl().listenerCount("upgrade") > 0) {
          phase = Phase::upgrade;
          request = incoming;
          bodyComplete = true;
          incoming->complete_ = true;
          rt::Bytes trailing(input.begin(), input.end());
          input.clear();
          clearTimer();
          ServerEventArgs args;
          args.incoming = incoming;
          args.socket = socket;
          args.head = std::move(trailing);
          dispatchUpgrade(std::move(args));
          return false;
        }

        request = incoming;
        response = std::make_shared<ServerResponse>(self);
        ServerResponse::Head& state = headOf(*response);
        state.isHeadRequest = method == "HEAD";
        state.http10 = !http11;
        response->shouldKeepAlive = keepAlive;
        bodyComplete = oversized || framing == Framing::none;
        chunkDecodingStarted = false;
        incoming->complete_ = !oversized && bodyComplete;
        flowing = false;
        endScheduled = false;
        endEmitted = false;
        dumping = false;
        phase = bodyComplete ? Phase::waiting : Phase::body;
        if (!oversized && expectContinue && http11) write(bytesOf("HTTP/1.1 100 Continue\r\n\r\n"));
        rearm();
        if (server) server->impl().emitRequest(incoming, response);
        return phase != Phase::closed && !destroyed;
      }

      void dispatchUpgrade(ServerEventArgs args) {
        if (server) server->impl().emit("upgrade", args);
      }

      // ---- request bodies -------------------------------------------------------------

      BufferedRequestBody snapshotBody(const IncomingMessage& incoming, std::size_t limit) const {
        if (request.get() != &incoming || dumping || destroyed) {
          return {.omission = incoming.complete() ? "body_unread" : "body_partial"};
        }
        if (framing == Framing::none) return {.bytes = std::make_shared<rt::Bytes>()};
        if (framing == Framing::chunked) {
          if (chunkDecodingStarted) return {.omission = "body_partial"};
          return parser.snapshotChunked({reinterpret_cast<const char*>(input.data()), input.size()}, limit);
        }
        const auto length = parser.wire.content_length;
        if (length > limit || length > parser.bodyBytesRemaining) return {.omission = "body_over_limit"};
        if (remaining != length) return {.omission = "body_partial"};
        if (input.empty()) return {.omission = "body_unread"};
        const std::size_t available = static_cast<std::size_t>(std::min<std::uint64_t>(length, input.size()));
        auto bytes = std::make_shared<rt::Bytes>(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(available));
        if (available == length) return {.bytes = std::move(bytes)};
        return {.omission = "body_partial", .receivedPrefix = std::move(bytes)};
      }

      // Decodes and delivers buffered body bytes while the message flows (or is being
      // discarded after its response).
      void decodeBody() {
        while (!destroyed && phase == Phase::body && !bodyComplete && (dumping || flowing) && !input.empty()) {
          rt::ByteChunk chunk;
          if (framing == Framing::length) {
            const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, input.size()));
            chunk.assign(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(take));
            input.erase(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(take));
            remaining -= take;
            if (remaining == 0) bodyComplete = true;
          } else {
            const std::optional<bool> progressed = decodeChunked(chunk);
            if (!progressed) {
              bodyError();
              return;
            }
            if (!*progressed && chunk.empty()) return;
          }
          if (!chunk.empty() && !dumping) {
            auto incoming = request;
            incoming->events().emit("data", IncomingEvent(std::move(chunk)));
          }
        }
        if (!destroyed && phase == Phase::body && bodyComplete) onBodyComplete();
      }

      // One step of the chunked decoder over the existing connection buffer.
      std::optional<bool> decodeChunked(rt::ByteChunk& chunk) {
        chunkDecodingStarted = true;
        const std::string_view text(reinterpret_cast<const char*>(input.data()), input.size());
        std::string_view payload;
        const auto consumed = parser.decodeChunked(text, payload);
        if (!consumed) return std::nullopt;
        if (!payload.empty()) chunk.assign(payload.begin(), payload.end());
        input.erase(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(*consumed));
        bodyComplete = parser.chunkPart == RequestParser::ChunkPart::done;
        return *consumed > 0 || bodyComplete;
      }

      // A request body that cannot be decoded. The request still being read is aborted
      // (its body errors); the connection then answers 400 and closes, after a response
      // already written in full (as Bun does), or is destroyed mid-response.
      void bodyError() {
        const char* code = llhttp_errno_name(parser.error);
        const char* reason = llhttp_get_error_reason(&parser.wire);
        const rt::Reason error = codedError("Parse Error: " + std::string(reason ? reason : code), code);
        if (request && !dumping) {
          auto incoming = request;
          incoming->events().emit("aborted");
          if (incoming->events().listenerCount("error") > 0) incoming->events().emit("error", IncomingEvent(error));
        }
        if (destroyed) return;
        if (response && headOf(*response).headerSent && !headOf(*response).finished) {
          destroy(error);
          return;
        }
        destroyAfter(parser.bodyTooLarge ? BODY_OVERFLOW : BAD_REQUEST);
      }

      void onBodyComplete() {
        if (request) request->complete_ = true;
        if (dumping) {
          dumping = false;
          request.reset();
          phase = Phase::head;
          idleSince = now();
          return;
        }
        phase = Phase::waiting;
        scheduleEnd();
      }

      void scheduleEnd() {
        if (endScheduled || endEmitted || !bodyComplete || !flowing || !request) return;
        endScheduled = true;
        auto self = shared_from_this();
        auto incoming = request;
        rt::queueMicrotask([self, incoming] {
          if (self->request != incoming || self->endEmitted) return;
          self->endEmitted = true;
          incoming->readableEnded_ = true;
          incoming->events().emit("end");
        });
      }

      void resumeRequest(const IncomingMessage* incoming) {
        if (request.get() != incoming) return;
        flowing = true;
        auto self = shared_from_this();
        auto target = request;
        rt::queueMicrotask([self, target] {
          if (self->request != target || !self->flowing || self->destroyed) return;
          self->processInput();
          if (self->phase == Phase::waiting) self->scheduleEnd();
          self->pump();
        });
      }

      void pauseRequest(const IncomingMessage* incoming) {
        if (request.get() != incoming) return;
        flowing = false;
      }

      // ---- writing ------------------------------------------------------------------

      void write(rt::Bytes data) {
        if (destroyed || data.empty() || sendShut || !upgrade.stream.is_open()) return;
        outputBytes += data.size();
        output.push_back(std::move(data));
        lastActivity = now();
        flush();
      }

      void flush() {
        if (writing || destroyed || !upgrade.stream.is_open()) return;
        boost::system::error_code error;
        while (!output.empty()) {
          const rt::Bytes& front = output.front();
          const std::size_t written = upgrade.stream.write_some(asio::buffer(front.data() + outputOffset, front.size() - outputOffset), error);
          if (error == asio::error::would_block || error == asio::error::try_again) break;
          if (error) {
            destroy(errnoReason(error, "write"));
            return;
          }
          consume(written);
        }
        if (output.empty()) {
          postDrained();
          return;
        }
        writing = true;
        auto self = shared_from_this();
        const rt::Bytes& front = output.front();
        upgrade.stream.async_write_some(asio::buffer(front.data() + outputOffset, front.size() - outputOffset), [self](const boost::system::error_code& failure, std::size_t written) {
          self->writing = false;
          if (self->destroyed) {
            self->output.clear();
            return;
          }
          if (failure == asio::error::operation_aborted) return;
          if (failure) {
            self->destroy(errnoReason(failure, "write"));
            return;
          }
          self->lastActivity = self->now();
          self->consume(written);
          self->flush();
        });
      }

      void consume(std::size_t written) {
        outputBytes -= written;
        outputOffset += written;
        while (!output.empty() && outputOffset >= output.front().size()) {
          outputOffset -= output.front().size();
          output.pop_front();
        }
      }

      void postDrained() {
        if (drainPosted) return;
        drainPosted = true;
        auto self = shared_from_this();
        asio::post(rt::executor(), [self] {
          self->drainPosted = false;
          self->drained();
        });
      }

      // Everything written so far reached the kernel.
      void drained() {
        if (destroyed || !output.empty() || writing) return;
        if (response) {
          ServerResponse::Head& state = headOf(*response);
          if (state.needDrain) {
            state.needDrain = false;
            auto current = response;
            current->events().emit("drain");
          }
          if (response && headOf(*response).ended && !headOf(*response).finished) finishResponse();
        }
        if (endAfterFlush && !destroyed && output.empty() && !sendShut) {
          endAfterFlush = false;
          sendShut = true;
          upgrade.writable = false;
          boost::system::error_code ignored;
          upgrade.stream.shutdown(asio::socket_base::shutdown_send, ignored);
          if (readEnded) destroy(nullptr);
        }
      }

      // ---- responses ----------------------------------------------------------------

      static ServerResponse::Head& headOf(ServerResponse& target) { return target.head(); }

      void finishResponse() {
        auto finished = response;
        auto incoming = request;
        ServerResponse::Head& state = headOf(*finished);
        state.finished = true;
        finishedResponses += 1;
        const bool closeAfter = state.last || readEnded;
        finished->events().emit("finish");
        rt::queueMicrotask([finished, incoming] {
          ServerResponse::Head& closing = headOf(*finished);
          if (closing.closeEmitted) return;
          closing.closeEmitted = true;
          closing.destroyed = true;
          finished->events().emit("close");
          if (incoming) incoming->events().emit("close");
        });
        if (destroyed || response != finished) return;
        response.reset();
        if (closeAfter) {
          request.reset();
          closeGracefully();
          return;
        }
        if (phase == Phase::body && !bodyComplete) {
          dumping = true;
          flowing = false;
        } else {
          request.reset();
          phase = Phase::head;
        }
        idleSince = now();
        headStartedAt = now();
        processInput();
        rearm();
        pump();
      }

      // Ends the connection after its last response: the sending side is shut once
      // flushed and the connection reads (and discards) until the peer closes or the
      // linger time ends.
      void closeGracefully() {
        if (destroyed) return;
        phase = Phase::lingering;
        dumping = false;
        lingerUntil = now() + LINGER_MS;
        endAfterFlush = true;
        if (output.empty() && !writing) drained();
        rearm();
        pump();
      }

      void destroyAfter(std::string_view head) {
        request.reset();
        response.reset();
        refuse(head);
      }

      // ---- closing ------------------------------------------------------------------

      void destroy(rt::Reason error) {
        if (destroyed) return;
        destroyed = true;
        const Phase previous = phase;
        phase = Phase::closed;
        clearTimer();
        upgrade.readable = false;
        upgrade.writable = false;
        if (upgrade.stream.is_open()) {
          boost::system::error_code ignored;
          upgrade.stream.close(ignored);
        }
        if (writing) {
          // Asio still borrows the front buffer until the write callback runs.
          while (output.size() > 1) output.pop_back();
        } else {
          output.clear();
        }
        outputBytes = 0;
        auto self = shared_from_this();
        auto incoming = request;
        auto outgoing = response;
        const bool upgrading = previous == Phase::upgrade;
        rt::queueMicrotask([self, incoming, outgoing, error, upgrading] {
          if (incoming && !upgrading && outgoing) {
            if (!incoming->complete() || !incoming->readableEnded()) incoming->events().emit("aborted");
          }
          if (outgoing) {
            ServerResponse::Head& state = headOf(*outgoing);
            if (!state.closeEmitted) {
              state.closeEmitted = true;
              state.destroyed = true;
              outgoing->events().emit("close");
            }
          }
          if (incoming && !upgrading && outgoing) {
            if (incoming->events().listenerCount("error") > 0) incoming->events().emit("error", IncomingEvent(codedError("aborted", "ECONNRESET")));
            incoming->events().emit("close");
          }
          if (error && self->socketEvents.listenerCount("error") > 0) self->socketEvents.emit("error", IncomingEvent(error));
          self->socketEvents.emit("close");
          self->socket.reset();
        });
        request.reset();
        response.reset();
        if (server) {
          auto owner = server;
          server.reset();
          owner->impl().connectionClosed(self);
        }
      }

      // ---- timers -------------------------------------------------------------------

      void clearTimer() {
        if (timer) rt::clearInterval(timer);
        timer = rt::Timeout();
      }

      double setting(double Server::*member, double fallback) const { return server ? (*server).*member : fallback; }

      // The next deadline of this connection: the keep-alive idle time between
      // requests, the head and request deadlines, the inactivity timeout of a request
      // in progress, or the end of lingering.
      void rearm() {
        clearTimer();
        if (destroyed) return;
        double deadline = 0;
        const double keepAliveTimeout = setting(&Server::keepAliveTimeout, 5000);
        const double headersTimeout = setting(&Server::headersTimeout, 60000);
        const double requestTimeout = setting(&Server::requestTimeout, 300000);
        const double inactivity = setting(&Server::timeout, 0);
        const auto earliest = [&deadline](double candidate) {
          if (deadline == 0 || candidate < deadline) deadline = candidate;
        };
        switch (phase) {
          case Phase::head:
            if (input.empty() && finishedResponses > 0) {
              if (keepAliveTimeout > 0) earliest(idleSince + keepAliveTimeout);
            } else if (headersTimeout > 0) {
              earliest(headStartedAt + headersTimeout);
            }
            break;
          case Phase::body:
            if (dumping) {
              if (keepAliveTimeout > 0) earliest(idleSince + keepAliveTimeout);
              break;
            }
            if (requestTimeout > 0) earliest(headStartedAt + requestTimeout);
            if (inactivity > 0) earliest(lastActivity + inactivity);
            break;
          case Phase::waiting:
            if (inactivity > 0) earliest(lastActivity + inactivity);
            break;
          case Phase::lingering:
            earliest(lingerUntil);
            break;
          case Phase::upgrade:
          case Phase::closed:
            break;
        }
        if (deadline == 0) return;
        auto weak = weak_from_this();
        timer = rt::detail::createTimer(
          [weak] {
            if (auto self = weak.lock()) self->onTimer();
          },
          std::max(1.0, deadline - now()), false);
        timer.unref();
      }

      void onTimer() {
        timer = rt::Timeout();
        if (destroyed) return;
        const double current = now();
        const double keepAliveTimeout = setting(&Server::keepAliveTimeout, 5000);
        const double headersTimeout = setting(&Server::headersTimeout, 60000);
        const double requestTimeout = setting(&Server::requestTimeout, 300000);
        const double inactivity = setting(&Server::timeout, 0);
        switch (phase) {
          case Phase::head:
            if (input.empty() && finishedResponses > 0) {
              if (keepAliveTimeout > 0 && current >= idleSince + keepAliveTimeout) return destroy(nullptr);
            } else if (headersTimeout > 0 && current >= headStartedAt + headersTimeout) {
              return refuse("HTTP/1.1 408 Request Timeout\r\nConnection: close\r\n\r\n");
            }
            break;
          case Phase::body:
            if (dumping) {
              if (keepAliveTimeout > 0 && current >= idleSince + keepAliveTimeout) return destroy(nullptr);
              break;
            }
            if (requestTimeout > 0 && current >= headStartedAt + requestTimeout) {
              if (response && !headOf(*response).headerSent) return destroyAfter("HTTP/1.1 408 Request Timeout\r\nConnection: close\r\n\r\n");
              return destroy(nullptr);
            }
            if (inactivity > 0 && current >= lastActivity + inactivity) return destroy(nullptr);
            break;
          case Phase::waiting:
            if (inactivity > 0 && current >= lastActivity + inactivity) return destroy(nullptr);
            break;
          case Phase::lingering:
            if (current >= lingerUntil) return destroy(nullptr);
            break;
          case Phase::upgrade:
          case Phase::closed:
            return;
        }
        rearm();
      }

      // Whether an "upgrade" socket's stream was taken by ws.
      void checkHandover() {
        if (destroyed || handedOver || phase != Phase::upgrade || upgrade.stream.is_open()) return;
        handedOver = true;
        request.reset();
        socket.reset();
        if (server) {
          auto owner = server;
          server.reset();
          owner->impl().connectionClosed(shared_from_this());
        }
      }

      bool responseInProgress() const { return response && !headOf(*response).finished; }
    };

    // ---- the server ---------------------------------------------------------------------

    std::size_t Server::Impl::listenerCount(std::string_view event) const {
        return static_cast<std::size_t>(std::count_if(registrations.begin(), registrations.end(), [event](const Registration& registration) { return registration.event == event; }));
      }

    bool Server::Impl::emit(std::string_view event, const ServerEventArgs& args) {
        std::vector<Registration> snapshot;
        for (const Registration& registration : registrations)
          if (registration.event == event) snapshot.push_back(registration);
        if (snapshot.empty()) {
          if (event == "error" && args.error) rt::detail::reportUncaught(args.error, false);
          return false;
        }
        for (const Registration& registration : snapshot) {
          if (registration.once) {
            if (*registration.fired) continue;
            *registration.fired = true;
            for (auto position = registrations.begin(); position != registrations.end(); ++position) {
              if (position->fired == registration.fired) {
                registrations.erase(position);
                break;
              }
            }
          }
          registration.listener(args);
        }
        return true;
      }

    void Server::Impl::emitRequest(const std::shared_ptr<IncomingMessage>& incoming, const std::shared_ptr<ServerResponse>& outgoing) {
        if (!listener) return;
        try {
          listener(incoming, outgoing);
        } catch (...) {
          rt::detail::reportUncaught(std::current_exception(), false);
        }
      }

    void Server::Impl::emitErrorLater(rt::Reason error) {
        std::shared_ptr<Server> self = owner ? owner->shared_from_this() : nullptr;
        rt::queueMicrotask([self, error] {
          if (!self) return;
          ServerEventArgs args;
          args.error = error;
          self->impl().emit("error", args);
        });
      }

    void Server::Impl::connectionClosed(const std::shared_ptr<ConnectionState>& connection) {
        connections.delete_(connection.get());
        maybeEmitClose();
      }

    void Server::Impl::maybeEmitClose() {
        if (!closing || listening || closeEmitted || connections.size() != 0) return;
        closeEmitted = true;
        std::shared_ptr<Server> self = owner ? owner->shared_from_this() : nullptr;
        rt::queueMicrotask([self] {
          if (!self) return;
          Impl& impl = self->impl();
          impl.keepAlive.reset();
          impl.emit("close", ServerEventArgs{});
          // No event can follow "close": drop the listeners and whatever they hold.
          impl.registrations.clear();
          impl.listener = nullptr;
        });
      }

    void Server::Impl::accept() {
        if (!acceptor || !listening) return;
        std::shared_ptr<Server> self = owner->shared_from_this();
        acceptor->async_accept([self](const boost::system::error_code& error, asio::ip::tcp::socket socket) {
          Impl& impl = self->impl();
          if (error == asio::error::operation_aborted || !impl.listening) return;
          if (error) {
            // EMFILE and friends: try again shortly instead of spinning (Bun's
            // listener keeps accepting; node:http reports nothing for these either).
            impl.retry = rt::detail::createTimer([self] { self->impl().accept(); }, 100, false);
            impl.retry.unref();
            return;
          }
          impl.adopt(std::move(socket));
          impl.accept();
        });
      }

    void Server::Impl::adopt(asio::ip::tcp::socket socket) {
        boost::system::error_code ignored;
        socket.set_option(asio::ip::tcp::no_delay(true), ignored);
        boost::system::error_code endpointError;
        const asio::ip::tcp::endpoint endpoint = socket.remote_endpoint(endpointError);
        auto connection = std::make_shared<ConnectionState>(asio::generic::stream_protocol::socket(std::move(socket)));
        if (!endpointError) {
          connection->remoteAddress = endpoint.address().to_string();
          connection->remotePort = static_cast<double>(endpoint.port());
          connection->remoteFamily = endpoint.address().is_v4() ? "IPv4" : "IPv6";
        }
        connection->upgrade.stream.non_blocking(true, ignored);
        connection->server = owner->shared_from_this();
        connection->socket = std::make_shared<Socket>(connection);
        connection->headStartedAt = connection->now();
        connection->idleSince = connection->headStartedAt;
        connection->lastActivity = connection->headStartedAt;
        connections.set(connection.get(), connection);
        connection->rearm();
        connection->pump();
      }

    Server::Server(RequestListener listener) : impl_(std::make_unique<Impl>()) {
      impl_->owner = this;
      impl_->listener = std::move(listener);
    }

    Server::~Server() {
      if (impl_->acceptor) {
        boost::system::error_code ignored;
        impl_->acceptor->close(ignored);
      }
      impl_->owner = nullptr;
    }

    std::shared_ptr<Server> createServer(RequestListener listener) { return std::make_shared<Server>(std::move(listener)); }

    void Server::on(std::string_view event, ServerListener listener) { impl_->registrations.push_back(Impl::Registration{std::string(event), std::move(listener), false, nullptr}); }

    void Server::once(std::string_view event, ServerListener listener) {
      impl_->registrations.push_back(Impl::Registration{std::string(event), std::move(listener), true, std::make_shared<bool>(false)});
    }

    void Server::listen(double port, std::string hostname, std::function<void()> callback) {
      if (!rt::Number::isInteger(port)) {
        impl_->emitErrorLater(codedError("The value of \"options.port\" is out of range. It must be an integer. Received " + rt::numberToString(port), "ERR_OUT_OF_RANGE", "RangeError"));
        return;
      }
      if (port < 0 || port > 65535) {
        impl_->emitErrorLater(codedError("The value of \"options.port\" is out of range. It must be >= 0 and <= 65535. Received " + rt::numberToString(port), "ERR_OUT_OF_RANGE", "RangeError"));
        return;
      }
      boost::system::error_code error;
      asio::ip::address address;
      if (hostname.empty()) address = asio::ip::address_v6::any();
      else if (hostname == "localhost") address = asio::ip::address_v6::loopback();
      else address = asio::ip::make_address(hostname, error);
      if (error) {
        auto failure = ws::CodedError("ENXIO: no such device or address, listen", "ENXIO");
        failure.errno_ = -6;
        failure.syscall = "listen";
        impl_->emitErrorLater(rt::makeReason(std::move(failure)));
        return;
      }
      const asio::ip::tcp::endpoint endpoint(address, static_cast<unsigned short>(port));
      auto acceptor = std::make_unique<asio::ip::tcp::acceptor>(rt::executor());
      acceptor->open(endpoint.protocol(), error);
      if (!error && address.is_v6()) acceptor->set_option(asio::ip::v6_only(false), error);
      if (!error) acceptor->set_option(asio::socket_base::reuse_address(true), error);
      if (!error) acceptor->bind(endpoint, error);
      if (!error) acceptor->listen(511, error);
      if (error) {
        if (error == asio::error::address_in_use) {
          auto failure = ws::CodedError("Failed to start server. Is port " + rt::numberToString(port) + " in use?", "EADDRINUSE");
          failure.errno_ = 0;
          failure.syscall = "listen";
          impl_->emitErrorLater(rt::makeReason(std::move(failure)));
        } else {
          auto failure = ws::CodedError(error == asio::error::access_denied ? "EACCES: permission denied, listen" : "EADDRNOTAVAIL: address not available, listen",
                                        error == asio::error::access_denied ? "EACCES" : "EADDRNOTAVAIL");
          failure.errno_ = -static_cast<double>(error.value());
          failure.syscall = "listen";
          impl_->emitErrorLater(rt::makeReason(std::move(failure)));
        }
        return;
      }
      const asio::ip::tcp::endpoint local = acceptor->local_endpoint(error);
      impl_->acceptor = std::move(acceptor);
      impl_->bound = AddressInfo{local.address().to_string(), local.address().is_v4() ? "IPv4" : "IPv6", static_cast<double>(local.port())};
      impl_->listening = true;
      impl_->closing = false;
      impl_->closeEmitted = false;
      impl_->keepAlive = shared_from_this();
      std::shared_ptr<Server> self = shared_from_this();
      rt::queueMicrotask([self, callback] {
        if (callback) callback();
        self->impl().emit("listening", ServerEventArgs{});
      });
      impl_->accept();
    }

    std::optional<AddressInfo> Server::address() const { return impl_->listening ? impl_->bound : std::nullopt; }

    void Server::close() {
      if (impl_->acceptor) {
        boost::system::error_code ignored;
        impl_->acceptor->close(ignored);
        impl_->acceptor.reset();
      }
      if (impl_->retry) rt::clearInterval(impl_->retry);
      impl_->listening = false;
      impl_->closing = true;
      closeIdleConnections();
      impl_->maybeEmitClose();
    }

    void Server::closeIdleConnections() {
      for (const auto& connection : rt::arrayFrom(impl_->connections.values())) {
        if (connection->phase == ConnectionState::Phase::upgrade || connection->responseInProgress()) continue;
        connection->destroy(nullptr);
      }
    }

    void Server::closeAllConnections() {
      for (const auto& connection : rt::arrayFrom(impl_->connections.values())) {
        if (connection->phase == ConnectionState::Phase::upgrade) continue;
        connection->destroy(nullptr);
      }
    }

    void Server::ref() { impl_->referenced = true; }
    void Server::unref() { impl_->referenced = false; }
    bool Server::hasRef() const { return impl_->referenced; }

    // ---- Socket ---------------------------------------------------------------------------

    Socket::Socket(std::shared_ptr<ConnectionState> connection) : connection_(std::move(connection)) {}

    std::optional<std::string> Socket::remoteAddress() const { return connection_->destroyed ? std::nullopt : connection_->remoteAddress; }
    std::optional<double> Socket::remotePort() const { return connection_->destroyed ? std::nullopt : connection_->remotePort; }
    std::optional<std::string> Socket::remoteFamily() const { return connection_->destroyed ? std::nullopt : connection_->remoteFamily; }

    bool Socket::destroyed() const { return connection_->destroyed || connection_->handedOver || (connection_->phase == ConnectionState::Phase::upgrade && !connection_->upgrade.stream.is_open()); }

    void Socket::end(rt::Bytes data) {
      if (destroyed()) return;
      connection_->write(std::move(data));
      end();
    }

    void Socket::end() {
      if (destroyed()) return;
      connection_->closeGracefully();
    }

    void Socket::destroy() { connection_->destroy(nullptr); }

    void Socket::on(std::string_view event, const IncomingListener& listener) { connection_->socketEvents.on(event, listener); }
    void Socket::once(std::string_view event, const IncomingListener& listener) { connection_->socketEvents.once(event, listener); }
    void Socket::removeListener(std::string_view event, const IncomingListener& listener) { connection_->socketEvents.removeListener(event, listener); }

    rt::ws::UpgradeSocket& Socket::upgradeSocket() {
      auto connection = connection_;
      rt::queueMicrotask([connection] { connection->checkHandover(); });
      return connection_->upgrade;
    }

    // ---- IncomingMessage ------------------------------------------------------------------

    IncomingMessage::IncomingMessage(std::shared_ptr<ConnectionState> connection, std::shared_ptr<Socket> socket)
      : connection_(std::move(connection)), socket_(std::move(socket)) {}

    std::optional<std::string> IncomingMessage::header(std::string_view name) const {
      ws::UpgradeRequest view;
      for (std::size_t index = 0; index + 1 < rawHeaders.size(); index += 2) view.rawHeaders.emplace_back(rawHeaders[index], rawHeaders[index + 1]);
      return view.header(name);
    }

    bool IncomingMessage::complete() const { return complete_; }

    void IncomingMessage::pause() { connection_->pauseRequest(this); }
    void IncomingMessage::resume() { connection_->resumeRequest(this); }
    void IncomingMessage::on(std::string_view event, const IncomingListener& listener) { events_.on(event, listener); }
    void IncomingMessage::once(std::string_view event, const IncomingListener& listener) { events_.once(event, listener); }
    void IncomingMessage::removeListener(std::string_view event, const IncomingListener& listener) { events_.removeListener(event, listener); }
    bool IncomingMessage::readableEnded() const { return readableEnded_; }

    // ---- ServerResponse -------------------------------------------------------------------

    ServerResponse::ServerResponse(std::shared_ptr<ConnectionState> connection) : connection_(std::move(connection)), head_(std::make_shared<Head>()) {}

    namespace {
      bool isCurrent(const std::shared_ptr<ConnectionState>& connection, const ServerResponse* response) { return connection->response.get() == response; }

      // The head as Bun writes it: the user's fields in setHeader order, then Date,
      // Connection (and Keep-Alive) and the framing field, each unless the user set it.
      std::string buildHead(ServerResponse& response, ServerResponse::Head& state, const ConnectionState& connection, std::optional<std::size_t> contentLength) {
        const int status = response.statusCode;
        state.hasBody = !state.isHeadRequest && status != 204 && status != 304 && !(status >= 100 && status < 200);
        std::string head = "HTTP/1.1 " + std::to_string(status) + " " + wireBytes(response.statusMessage.empty() ? std::string(reasonPhrase(status)) : response.statusMessage) + "\r\n";
        bool userConnection = false;
        bool userDate = false;
        bool userKeepAlive = false;
        bool userContentLength = false;
        bool userTransferEncoding = false;
        for (const ServerResponse::Head::Field& field : state.fields) {
          for (const std::string& value : field.values) head += wireBytes(field.name) + ": " + wireBytes(value) + "\r\n";
          if (field.lower == "connection") {
            userConnection = true;
            for (const std::string& value : field.values)
              if (listsToken(lowerAscii(value), "close")) state.last = true;
          } else if (field.lower == "date") {
            userDate = true;
          } else if (field.lower == "keep-alive") {
            userKeepAlive = true;
          } else if (field.lower == "content-length") {
            userContentLength = true;
          } else if (field.lower == "transfer-encoding") {
            userTransferEncoding = true;
            for (const std::string& value : field.values)
              if (listsToken(lowerAscii(value), "chunked")) state.chunked = true;
          }
        }
        if (!userDate) head += "Date: " + utcDate() + "\r\n";
        const bool keepAlive = response.shouldKeepAlive && !state.http10;
        if (!userConnection) {
          if (keepAlive) {
            head += "Connection: keep-alive\r\n";
            const double keepAliveTimeout = connection.server ? connection.server->keepAliveTimeout : 5000;
            if (!userKeepAlive && keepAliveTimeout > 0) head += "Keep-Alive: timeout=" + rt::numberToString(std::floor(keepAliveTimeout / 1000)) + "\r\n";
          } else {
            head += "Connection: close\r\n";
            state.last = true;
          }
        }
        if (!userContentLength && !userTransferEncoding && state.hasBody) {
          if (contentLength) {
            head += "Content-Length: " + std::to_string(*contentLength) + "\r\n";
          } else if (!state.http10) {
            head += "Transfer-Encoding: chunked\r\n";
            state.chunked = true;
          } else {
            state.last = true;
          }
        }
        if (!state.hasBody) state.chunked = false;
        return head + "\r\n";
      }

      void writeBody(ConnectionState& connection, ServerResponse::Head& state, std::span<const std::uint8_t> chunk) {
        if (!state.hasBody || chunk.empty()) return;
        if (state.chunked) {
          rt::Bytes framed = bytesOf(hexSize(chunk.size()) + "\r\n");
          framed.insert(framed.end(), chunk.begin(), chunk.end());
          framed.push_back('\r');
          framed.push_back('\n');
          connection.write(std::move(framed));
        } else {
          connection.write(rt::Bytes(chunk.begin(), chunk.end()));
        }
      }

      void commitHead(ServerResponse& response, ServerResponse::Head& state, ConnectionState& connection, std::optional<std::size_t> contentLength) {
        if (state.headerSent) return;
        state.headerSent = true;
        connection.write(bytesOf(buildHead(response, state, connection, contentLength)));
      }
    }

    void ServerResponse::setHeader(std::string_view name, std::string_view value) { setHeader(name, std::vector<std::string>{std::string(value)}); }

    void ServerResponse::setHeader(std::string_view name, std::vector<std::string> values) {
      if (head_->headerSent) throw ws::CodedError("Cannot set headers after they are sent to the client", "ERR_HTTP_HEADERS_SENT");
      if (!isToken(name)) throw rt::CodedTypeError("Header name must be a valid HTTP token [\"" + std::string(name) + "\"]", "ERR_INVALID_HTTP_TOKEN");
      for (const std::string& value : values)
        if (!validHeaderValue(value)) throw rt::CodedTypeError("Invalid character in header content [\"" + std::string(name) + "\"]", "ERR_INVALID_CHAR");
      const std::string lower = lowerAscii(name);
      for (Head::Field& field : head_->fields) {
        if (field.lower == lower) {
          field.name = std::string(name);
          field.values = std::move(values);
          return;
        }
      }
      head_->fields.push_back(Head::Field{std::string(name), lower, std::move(values)});
    }

    std::optional<std::vector<std::string>> ServerResponse::getHeader(std::string_view name) const {
      const std::string lower = lowerAscii(name);
      for (const Head::Field& field : head_->fields)
        if (field.lower == lower) return field.values;
      return std::nullopt;
    }

    void ServerResponse::flushHeaders() {
      if (head_->headerSent || head_->destroyed || !isCurrent(connection_, this)) return;
      commitHead(*this, *head_, *connection_, std::nullopt);
    }

    bool ServerResponse::headersSent() const { return head_->headerSent; }

    bool ServerResponse::write(std::span<const std::uint8_t> chunk) {
      if (head_->ended || head_->destroyed || !isCurrent(connection_, this)) return false;
      if (chunk.empty()) return true;
      commitHead(*this, *head_, *connection_, std::nullopt);
      writeBody(*connection_, *head_, chunk);
      if (connection_->outputBytes < HIGH_WATER_MARK) return true;
      head_->needDrain = true;
      return false;
    }

    bool ServerResponse::write(std::string_view text) {
      const rt::Bytes bytes = rt::encodeUtf8(text);
      return write(std::span<const std::uint8_t>(bytes));
    }

    void ServerResponse::end() { end(std::span<const std::uint8_t>()); }

    void ServerResponse::end(std::span<const std::uint8_t> data) {
      if (head_->ended) return;
      head_->ended = true;
      if (head_->destroyed || !isCurrent(connection_, this)) return;
      commitHead(*this, *head_, *connection_, head_->isHeadRequest ? std::nullopt : std::optional<std::size_t>(data.size()));
      writeBody(*connection_, *head_, data);
      if (head_->chunked) connection_->write(bytesOf("0\r\n\r\n"));
      if (connection_->output.empty() && !connection_->writing) connection_->postDrained();
    }

    void ServerResponse::end(std::string_view text) {
      const rt::Bytes bytes = rt::encodeUtf8(text);
      end(std::span<const std::uint8_t>(bytes));
    }

    void ServerResponse::destroy() {
      if (head_->destroyed) return;
      if (isCurrent(connection_, this)) connection_->destroy(nullptr);
      else head_->destroyed = true;
    }

    bool ServerResponse::destroyed() const { return head_->destroyed || (isCurrent(connection_, this) && connection_->destroyed); }
    bool ServerResponse::writableEnded() const { return head_->ended; }
    bool ServerResponse::writableFinished() const { return head_->finished; }
    double ServerResponse::writableLength() const { return isCurrent(connection_, this) ? static_cast<double>(connection_->outputBytes) : 0; }
    void ServerResponse::on(std::string_view event, const IncomingListener& listener) { events_.on(event, listener); }
    void ServerResponse::once(std::string_view event, const IncomingListener& listener) { events_.once(event, listener); }
    void ServerResponse::removeListener(std::string_view event, const IncomingListener& listener) { events_.removeListener(event, listener); }
  }

  // ---- src/runtime/http.ts ------------------------------------------------------------------

  namespace {
    using node_http::IncomingEvent;
    using node_http::IncomingListener;
    using node_http::IncomingMessage;
    using node_http::ServerResponse;

    rt::Reason interrupted() { return rt::makeReason(rt::JsError("The client disconnected before its response was sent")); }
    // These external states can change during an awaited stream read.
    bool responseInterrupted(const ServerResponse& outgoing, const rt::AbortSignal& signal) { return signal.aborted() || outgoing.destroyed(); }
    struct RequestBodyState {
      http_body::IncomingBody body;
      std::weak_ptr<IncomingMessage> incoming;
    };
    rt::WeakMap<rt::Request, RequestBodyState> requestBodies;

    const rt::Regex EVENT_STREAM("^text\\/event-stream(?:\\s*;|$)", "i");

    bool isAdminRequest(const std::optional<rt::Request>& request) {
      if (!request) return false;
      try {
        const std::string path = rt::URL(request->url()).pathname();
        return path == "/admin" || path == "/admin.html" || rt::startsWith(path, "/admin/");
      } catch (...) {
        return false;
      }
    }

    rt::Headers incomingHeaders(const IncomingMessage& incoming) {
      rt::Headers headers;
      for (std::size_t index = 0; index < incoming.rawHeaders.size(); index += 2) {
        const std::string& name = incoming.rawHeaders[index];
        const std::string value = index + 1 < incoming.rawHeaders.size() ? incoming.rawHeaders[index + 1] : "undefined";
        if (rt::toLowerCase(name) == "cookie" && headers.has(name)) headers.set(name, headers.get(name).value_or("null") + "; " + value);
        else headers.append(name, value);
      }
      return headers;
    }

    rt::Request incomingRequest(std::shared_ptr<IncomingMessage> incoming, rt::AbortSignal signal) {
      const std::string host = incoming->header("host").value_or("localhost");
      // `incoming.method ?? "GET"`: a server request always has its method.
      const std::string method = incoming->method;
      rt::RequestInit init;
      init.method = method;
      init.headers = incomingHeaders(*incoming);
      init.signal = signal;
      std::optional<http_body::IncomingBody> body;
      if (method != "GET" && method != "HEAD") {
        body = http_body::createIncomingBody(incoming, signal);
        init.body = rt::BodyInit(body->stream);
        init.duplex = "half";
      }
      try {
        // `incoming.url ?? "/"`: a server request always has its target.
        const rt::URL url(incoming->url, "http://" + host);
        rt::Request request(url, std::move(init));
        if (body) requestBodies.set(request, {*body, incoming});
        return request;
      } catch (...) {
        if (body) body->dispose();
        throw;
      }
    }

    void responseHeaders(const rt::Response& response, ServerResponse& outgoing) {
      outgoing.statusCode = response.status();
      if (!response.statusText().empty()) outgoing.statusMessage = response.statusText();
      for (const auto& [name, value] : response.headers()) {
        if (name != "set-cookie") outgoing.setHeader(name, value);
      }
      std::vector<std::string> cookies = response.headers().getSetCookie();
      if (!cookies.empty()) outgoing.setHeader("set-cookie", std::move(cookies));
    }

    void prepareResponseHeaders(const rt::Request& request, const rt::Response& response, ServerResponse& outgoing) {
      const bool earlyHeaders = http_early_sse::earlySseHeadersCommitted(request);
      if (!earlyHeaders) responseHeaders(response, outgoing);
      const RequestBodyState* input = requestBodies.find(request);
      if (input && (input->body.cancelled() || !input->body.consumed())) {
        // Refusals can return without reading input, including after asynchronous
        // authentication/admission. Discard it before handoff and close only after
        // the response is written; an unread paused input otherwise holds shutdown.
        input->body.dispose();
        outgoing.shouldKeepAlive = false;
        if (!earlyHeaders) outgoing.setHeader("connection", "close");
      }
    }

    // Readonly<{ stop: () => void; remove: () => void }>
    struct ResponseReadiness {
      std::function<void()> stop;
      std::function<void()> remove;
    };

    // The state `registerResponseReadiness`'s closures share; `stop` is registered as an
    // abort listener and as a response listener, so it has one identity per registry.
    struct Readiness {
      bool pending = true;
      rt::AbortSignal signal;
      std::weak_ptr<ServerResponse> outgoing;
      rt::EventListener abortListener;
      IncomingListener eventListener;

      explicit Readiness(rt::AbortSignal signal_) : signal(std::move(signal_)) {}

      void stop() {
        pending = false;
        signal.removeEventListener("abort", abortListener);
        if (auto target = outgoing.lock()) {
          target->removeListener("close", eventListener);
          target->removeListener("error", eventListener);
        }
      }
    };

    ResponseReadiness registerResponseReadiness(rt::Request request, std::shared_ptr<ServerResponse> outgoing, rt::AbortSignal signal) {
      auto readiness = std::make_shared<Readiness>(signal);
      readiness->outgoing = outgoing;
      const std::weak_ptr<Readiness> weak = readiness;
      readiness->abortListener = rt::EventListener([weak] {
        if (auto target = weak.lock()) target->stop();
      });
      readiness->eventListener = IncomingListener([weak](const IncomingEvent&) {
        if (auto target = weak.lock()) target->stop();
      });
      const std::weak_ptr<ServerResponse> weakOutgoing = outgoing;
      const std::function<void()> unregister = http_early_sse::registerEarlySseHeaders(request, [readiness, weakOutgoing, signal, request](const http_early_sse::EarlySseHeaders& value) {
        const rt::Headers& headers = value.headers;
        const std::shared_ptr<ServerResponse> target = weakOutgoing.lock();
        const RequestBodyState* input = requestBodies.find(request);
        if (!readiness->pending || !target || responseInterrupted(*target, signal) || target->headersSent() || input == nullptr || !input->body.consumed()) return false;
        if (!EVENT_STREAM.test(headers.get("content-type").value_or(""))) return false;
        for (const char* name : {"content-length", "content-encoding", "transfer-encoding"})
          if (headers.has(name)) return false;
        rt::ResponseInit init;
        init.headers = headers;
        responseHeaders(rt::Response(nullptr, std::move(init)), *target);
        target->flushHeaders();
        return true;
      });
      signal.addEventListener("abort", readiness->abortListener, {.once = true});
      outgoing->once("close", readiness->eventListener);
      outgoing->once("error", readiness->eventListener);
      return ResponseReadiness{[readiness] { readiness->stop(); },
                               [readiness, unregister] {
                                 readiness->stop();
                                 unregister();
                               }};
    }

    // Registrations for one drain wait, owned by the waiting coroutine.
    struct DrainWait {
      std::shared_ptr<ServerResponse> outgoing;
      rt::AbortSignal signal;
      rt::Resolve<void> resolve;
      rt::Reject reject;
      IncomingListener drain;
      IncomingListener failed;
      IncomingListener closed;
      rt::EventListener closedOnAbort;

      DrainWait(std::shared_ptr<ServerResponse> outgoing_, rt::AbortSignal signal_, rt::Resolve<void> resolve_, rt::Reject reject_)
        : outgoing(std::move(outgoing_)), signal(std::move(signal_)), resolve(std::move(resolve_)), reject(std::move(reject_)) {}

      void cleanup() {
        outgoing->removeListener("drain", drain);
        outgoing->removeListener("error", failed);
        outgoing->removeListener("close", closed);
        signal.removeEventListener("abort", closedOnAbort);
        drain = IncomingListener();
        failed = IncomingListener();
        closed = IncomingListener();
        closedOnAbort = rt::EventListener();
      }
      void fail(rt::Reason error) {
        cleanup();
        reject(error);
      }
    };

    rt::Task<void> waitForDrain(std::shared_ptr<ServerResponse> outgoing, rt::AbortSignal signal) {
      const auto result = rt::withResolvers<void>();
      const auto wait = std::make_shared<DrainWait>(outgoing, signal, result.resolve, result.reject);
      const auto remove = rt::scopeExit([&] { wait->cleanup(); });
      const std::weak_ptr<DrainWait> weak = wait;
      wait->drain = IncomingListener([weak](const IncomingEvent&) {
        if (auto live = weak.lock()) {
          live->cleanup();
          live->resolve();
        }
      });
      wait->failed = IncomingListener([weak](const IncomingEvent& event) {
        if (auto live = weak.lock()) live->fail(std::get<rt::Reason>(event));
      });
      wait->closed = IncomingListener([weak](const IncomingEvent&) {
        if (auto live = weak.lock()) live->fail(interrupted());
      });
      wait->closedOnAbort = rt::EventListener([weak] {
        if (auto live = weak.lock()) live->fail(interrupted());
      });
      outgoing->once("drain", wait->drain);
      outgoing->once("error", wait->failed);
      outgoing->once("close", wait->closed);
      signal.addEventListener("abort", wait->closedOnAbort, {.once = true});
      if (signal.aborted() || outgoing->destroyed()) wait->fail(interrupted());
      co_await result.promise.wait();
    }

    /** EOF only ends the producer. The separate response finish event owns delivery. */
    rt::Task<void> sendResponse(rt::Request request, rt::Response response, std::shared_ptr<ServerResponse> outgoing, rt::AbortSignal signal) {
      if (responseInterrupted(*outgoing, signal)) {
        if (const std::optional<rt::ByteStream> body = response.body()) rt::spawn(body->cancel(signal.reason()), rt::ignoreRejection);
        co_return;
      }
      try {
        prepareResponseHeaders(request, response, *outgoing);
      } catch (...) {
        // The fallback writes its own body. Settle the original producer without waiting for it.
        if (const auto body = response.body()) (void)body->cancelPromise(std::current_exception()).catch_(rt::ignoreRejection);
        throw;
      }
      const std::optional<rt::ByteStream> body = response.body();
      if (!body || request.method() == "HEAD" || response.status() == 204 || response.status() == 205 || response.status() == 304) {
        if (body) rt::spawn(body->cancel(), rt::ignoreRejection);
        outgoing->end();
        co_return;
      }
      const rt::ReadableStreamDefaultReader<rt::ByteChunk> reader = body->getReader();
      const std::weak_ptr<rt::detail::AbortSignalState> weakSignal = signal.state();
      const auto cancel = [reader, weakSignal] {
        // Never await a pending provider pull to handle a disconnected client.
        if (auto live = weakSignal.lock()) {
          const rt::AbortSignal aborted(std::move(live));
          rt::spawn(reader.cancel(aborted.reason()), rt::ignoreRejection);
        }
      };
      const rt::EventListener cancelListener(cancel);
      auto removeCancel = rt::scopeExit([&] { signal.removeEventListener("abort", cancelListener); });
      signal.addEventListener("abort", cancelListener, {.once = true});
      rt::Reason failure;
      try {
        // SSE clients need the headers even while the producer is quiet.
        if (!outgoing->headersSent() && EVENT_STREAM.test(response.headers().get("content-type").value_or(""))) outgoing->flushHeaders();
        while (!responseInterrupted(*outgoing, signal)) {
          const rt::ReadableStreamReadResult<rt::ByteChunk> chunk = co_await reader.read();
          if (responseInterrupted(*outgoing, signal)) break;
          if (chunk.done) {
            outgoing->end();
            break;
          }
          if (!outgoing->write(std::span<const std::uint8_t>(*chunk.value))) {
            auto draining = waitForDrain(outgoing, signal);
            co_await std::move(draining);
          }
        }
      } catch (...) {
        failure = std::current_exception();
      }
      signal.removeEventListener("abort", cancelListener);
      removeCancel.release();
      if (responseInterrupted(*outgoing, signal)) cancel();
      reader.releaseLock();
      if (failure) std::rethrow_exception(failure);
    }

    // The state serveHttp's closures share.
    struct ServeState {
      ServeHttpOptions options;
      ServeHandler handler;
      // `const active = new Set<Promise<void>>()`
      rt::OrderedMap<const void*, rt::Promise<void>> active;
      http_upgrade::WebSocketUpgrades upgrades;
      std::optional<rt::Promise<void>> stopPromise;
      rt::Promise<NetAddr> ready;
      rt::Promise<void> finished;
      std::optional<rt::Resolve<NetAddr>> resolveReady;
      std::optional<rt::Reject> rejectReady;
      std::optional<rt::Resolve<void>> resolveFinished;
      std::optional<rt::Reject> rejectFinished;
      NetAddr address;
      std::shared_ptr<node_http::Server> server;
      rt::EventListener abort;

      ~ServeState() {
        if (options.signal) options.signal->removeEventListener("abort", abort);
      }
    };

    // `active.add(task); void task.finally(() => active.delete(task))` (`.catch(() => {})` too for upgrades).
    void track(const std::shared_ptr<ServeState>& state, const rt::Promise<void>& task, bool swallow) {
      const void* key = task.state().get();
      state->active.set(key, task);
      const std::weak_ptr<ServeState> weak = state;
      const rt::Promise<void> settled = task.finally_([weak, key] {
        if (auto live = weak.lock()) live->active.delete_(key);
      });
      if (swallow) (void)settled.catch_(rt::ignoreRejection);
    }

    // The per-request async task of the request listener.
    rt::Task<void> handleRequest(std::shared_ptr<ServeState> state, std::shared_ptr<IncomingMessage> incoming, std::shared_ptr<ServerResponse> outgoing, rt::AbortController controller,
                                 rt::Promise<void> completed, std::function<void(rt::Reason)> fail, std::function<rt::Promise<void>()> drainTasks, IncomingListener aborted,
                                 IncomingListener failListener) {
      std::optional<rt::Request> request;
      std::optional<ResponseReadiness> headerReadiness;
      auto removeReadiness = rt::scopeExit([&] {
        if (headerReadiness) headerReadiness->remove();
      });
      rt::Reason failure;
      try {
        request = incomingRequest(incoming, controller.signal());
        headerReadiness = registerResponseReadiness(*request, outgoing, controller.signal());
        ServeHandlerInfo info{NetAddr{"tcp", incoming->socket()->remoteAddress().value_or(""), incoming->socket()->remotePort().value_or(0)}, completed};
        const rt::Promise<rt::Response> handling(state->handler(*request, std::move(info)));
        const rt::Response response = co_await handling.wait();
        headerReadiness->stop();
        const rt::Promise<void> sending(sendResponse(*request, response, outgoing, controller.signal()));
        co_await sending.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) {
        if (headerReadiness) headerReadiness->stop();
        if (outgoing->headersSent() || outgoing->destroyed() || controller.signal().aborted()) {
          fail(rt::isError(failure) ? failure : rt::makeReason(rt::JsError("HTTP response failed")));
          outgoing->destroy();
        } else {
          outgoing->statusCode = 500;
          if (isAdminRequest(request)) outgoing->setHeader("cache-control", "no-store");
          outgoing->setHeader("content-type", "text/plain; charset=utf-8");
          outgoing->end("Internal Server Error");
          reportRejection(state->options.onTransportRejection, 500, "transport_handler_error", "body_unread");
        }
      }
      const rt::Promise<void> delivered = completed.catch_(rt::ignoreRejection);
      co_await delivered.wait();
      if (request) {
        if (const RequestBodyState* body = requestBodies.find(*request)) body->body.dispose();
        requestBodies.delete_(*request);
      }
      if (headerReadiness) headerReadiness->remove();
      removeReadiness.release();
      const rt::Promise<void> draining = drainTasks();
      co_await draining.wait();
      incoming->removeListener("aborted", aborted);
      incoming->removeListener("error", failListener);
    }

    void onRequest(const std::shared_ptr<ServeState>& state, const std::shared_ptr<IncomingMessage>& incoming, const std::shared_ptr<ServerResponse>& outgoing) {
      const rt::AbortController controller;
      const rt::Resolvers<void> resolvers = rt::withResolvers<void>();
      const rt::Promise<void> completed = resolvers.promise;
      const rt::Resolve<void> resolveCompleted = resolvers.resolve;
      const rt::Reject rejectCompleted = resolvers.reject;
      auto settled = std::make_shared<bool>(false);
      (void)completed.catch_(rt::ignoreRejection);
      const std::function<rt::Promise<void>()> drainTasks = http_tasks::createRequestTaskDrain(completed);
      const std::function<void(rt::Reason)> fail = [settled, controller, rejectCompleted](rt::Reason error) {
        if (*settled) return;
        *settled = true;
        controller.abort(error);
        rejectCompleted(error);
      };
      const IncomingListener aborted([fail](const IncomingEvent&) { fail(interrupted()); });
      const IncomingListener failListener([fail](const IncomingEvent& event) { fail(std::get<rt::Reason>(event)); });
      incoming->once("aborted", aborted);
      incoming->once("error", failListener);
      outgoing->once("error", failListener);
      ServerResponse* response = outgoing.get();
      outgoing->once("close", IncomingListener([response, aborted](const IncomingEvent& event) {
        if (!response->writableFinished()) aborted(event);
      }));
      const rt::Resolve<void> resolve = resolveCompleted;
      outgoing->once("finish", IncomingListener([settled, resolve](const IncomingEvent&) {
        if (*settled) return;
        *settled = true;
        resolve();
      }));
      const rt::Promise<void> task(handleRequest(state, incoming, outgoing, controller, completed, fail, drainTasks, aborted, failListener));
      track(state, task, false);
    }

    rt::Task<void> shutdownServer(std::shared_ptr<ServeState> state, std::function<void()> closeLingeringConnections) {
      const rt::Promise<NetAddr> ready = state->ready;
      co_await ready.wait();
      state->server->close();
      state->server->closeIdleConnections();
      closeLingeringConnections();
      auto stopping = state->upgrades.shutdown();
      co_await std::move(stopping);
      const rt::Promise<void> finished = state->finished;
      co_await finished.wait();
    }
  }

  BufferedRequestBody snapshotBufferedRequestBody(const rt::Request& request, std::size_t limit) {
    const auto* state = requestBodies.find(request);
    if (state) {
      const auto incoming = state->incoming.lock();
      if (incoming) return incoming->connection()->snapshotBody(*incoming, limit);
    }
    return {.omission = "body_unread"};
  }

  HttpServer serveHttp(ServeHandler handler) { return serveHttp(ServeHttpOptions{}, std::move(handler)); }

  HttpServer serveHttp(ServeHttpOptions options, ServeHandler handler) {
    if (!handler) throw rt::TypeError("An HTTP request handler is required");
    auto state = std::make_shared<ServeState>();
    state->options = std::move(options);
    state->handler = std::move(handler);
    state->upgrades = http_upgrade::createWebSocketUpgrades(state->options.upgrade, incomingRequest);
    auto readyResolvers = rt::withResolvers<NetAddr>();
    auto finishedResolvers = rt::withResolvers<void>();
    state->ready = readyResolvers.promise;
    state->finished = finishedResolvers.promise;
    state->resolveReady = readyResolvers.resolve;
    state->rejectReady = readyResolvers.reject;
    state->resolveFinished = finishedResolvers.resolve;
    state->rejectFinished = finishedResolvers.reject;
    // Consumers may wait for only ready or finished; neither rejection is unhandled.
    (void)state->ready.catch_(rt::ignoreRejection);
    (void)state->finished.catch_(rt::ignoreRejection);
    state->address = NetAddr{"tcp", state->options.hostname.value_or("0.0.0.0"), state->options.port.value_or(8000)};
    const std::weak_ptr<ServeState> weak = state;
    // The server's listeners hold the state like the JS closures do; the server drops them
    // after its "close" event.
    state->server = node_http::createServer([state](std::shared_ptr<IncomingMessage> incoming, std::shared_ptr<ServerResponse> outgoing) { onRequest(state, incoming, outgoing); });
    state->server->impl().applicationChatBodyRejection = state->options.applicationChatBodyRejection;
    state->server->impl().onTransportRejection = state->options.onTransportRejection;
    // Inference has application-owned deadlines. A quiet SSE or pending handler
    // must not inherit Bun.serve's ten-second idle timeout through node:http.
    state->server->timeout = 0;
    state->server->requestTimeout = 0;
    state->server->on("upgrade", [state](const node_http::ServerEventArgs& args) {
      const rt::Promise<void> task(state->upgrades.upgrade(args.incoming, args.socket, args.head));
      track(state, task, true);
    });
    state->server->once("error", [weak](const node_http::ServerEventArgs& args) {
      auto live = weak.lock();
      if (!live) return;
      (*live->rejectReady)(args.error);
      (*live->rejectFinished)(args.error);
      live->server->close();
    });
    state->server->once("close", [weak](const node_http::ServerEventArgs&) {
      auto live = weak.lock();
      if (!live) return;
      const rt::Resolve<void> resolveFinished = *live->resolveFinished;
      (void)rt::allSettled(rt::arrayFrom(live->active.values())).then([resolveFinished](const std::vector<rt::SettledResult<void>>&) { resolveFinished(); });
    });
    state->server->listen(state->address.port, state->address.hostname, [weak] {
      auto live = weak.lock();
      if (!live) return;
      const std::optional<node_http::AddressInfo> actual = live->server->address();
      // `typeof actual === "string"` (a pipe) cannot happen: the listener is always TCP.
      if (!actual) {
        const rt::Reason error = rt::makeReason(rt::JsError("The HTTP server did not create a TCP listener"));
        (*live->rejectReady)(error);
        (*live->rejectFinished)(error);
        live->server->close();
        return;
      }
      live->address = NetAddr{"tcp", actual->address, actual->port};
      try {
        if (live->options.onListen) live->options.onListen(live->address);
        (*live->resolveReady)(live->address);
      } catch (...) {
        const rt::Reason caught = std::current_exception();
        const rt::Reason failure = rt::isError(caught) ? caught : rt::makeReason(rt::JsError("HTTP listener setup failed"));
        (*live->rejectReady)(failure);
        (*live->rejectFinished)(failure);
        live->server->close();
      }
    });
    /**
     * Bun does not always release a keep-alive connection through
     * `closeIdleConnections`, which leaves `server.close()` waiting until systemd
     * kills the service. Close the remaining sockets once every active handler
     * has settled; active streams still receive their normal drain window.
     */
    auto closeLingeringConnections = std::make_shared<std::function<void()>>();
    const std::weak_ptr<std::function<void()>> weakClose = closeLingeringConnections;
    *closeLingeringConnections = [weak, weakClose] {
      auto live = weak.lock();
      auto self = weakClose.lock();
      if (!live || !self) return;
      if (live->active.size() == 0) {
        live->server->closeAllConnections();
        return;
      }
      rt::setTimeout([self] { (*self)(); }, 25).unref();
    };
    const auto requestShutdown = [closeLingeringConnections](const std::shared_ptr<ServeState>& live) -> rt::Promise<void> {
      if (live->stopPromise) return *live->stopPromise;
      const std::function<void()> closeLingering = [closeLingeringConnections] { (*closeLingeringConnections)(); };
      live->stopPromise = rt::Promise<void>(shutdownServer(live, closeLingering));
      return *live->stopPromise;
    };
    if (state->options.signal) {
      state->abort = rt::EventListener([weak, requestShutdown] {
        if (auto live = weak.lock()) (void)requestShutdown(live).catch_(rt::ignoreRejection);
      });
      state->options.signal->addEventListener("abort", state->abort, {.once = true});
      if (state->options.signal->aborted()) state->abort();
    }
    (void)state->finished
      .finally_([weak] {
        if (auto live = weak.lock()) {
          if (live->options.signal) live->options.signal->removeEventListener("abort", live->abort);
          live->abort = rt::EventListener{};
        }
      })
      .catch_(rt::ignoreRejection);
    HttpServer server;
    server.addr = [state] {
      const std::optional<node_http::AddressInfo> actual = state->server->address();
      return actual ? NetAddr{"tcp", actual->address, actual->port} : state->address;
    };
    server.ready = state->ready;
    server.finished = state->finished;
    server.shutdown = [state, requestShutdown] { return requestShutdown(state); };
    server.ref = [state] { state->server->ref(); };
    server.unref = [state] { state->server->unref(); };
    return server;
  }
}
