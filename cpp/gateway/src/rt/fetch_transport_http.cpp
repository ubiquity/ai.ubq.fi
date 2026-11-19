// Fetch's HTTP/1.x request writer and llhttp response parser.
#include "fetch_transport_internal.hpp"

#include "uos/rt/string.hpp"

#include <boost/beast/http/rfc7230.hpp>

#include <algorithm>
#include <array>

using namespace std::string_view_literals;

namespace uos::rt::detail::fetch {
  namespace {
    char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

    using boost::beast::iequals;

    std::string_view trimSpaces(std::string_view text) {
      while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
      while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
      return text;
    }

    // A JS string holding only code points up to U+00FF, as wire bytes.
    std::string latin1Bytes(std::string_view text) {
      std::string out;
      out.reserve(text.size());
      for (const std::uint32_t codePoint : codePoints(text)) out.push_back(static_cast<char>(codePoint & 0xFF));
      return out;
    }

    // WebKit's HTTPHeaderNames table in the spelling Bun writes on the wire (four
    // names differ from WebKit's: Icy-Metaint, Last-Event-Id, Sourcemap, X-Sourcemap),
    // sorted by lowercased name.
    constexpr std::array kCommonHeaderNames = {
      "Accept"sv, "Accept-Charset"sv, "Accept-Encoding"sv, "Accept-Language"sv, "Accept-Ranges"sv, "Access-Control-Allow-Credentials"sv,
      "Access-Control-Allow-Headers"sv, "Access-Control-Allow-Methods"sv, "Access-Control-Allow-Origin"sv, "Access-Control-Expose-Headers"sv,
      "Access-Control-Max-Age"sv, "Access-Control-Request-Headers"sv, "Access-Control-Request-Method"sv, "Age"sv, "Authorization"sv,
      "Cache-Control"sv, "Connection"sv, "Content-Disposition"sv, "Content-Encoding"sv, "Content-Language"sv, "Content-Length"sv,
      "Content-Location"sv, "Content-Range"sv, "Content-Security-Policy"sv, "Content-Security-Policy-Report-Only"sv, "Content-Type"sv, "Cookie"sv,
      "Cookie2"sv, "Cross-Origin-Embedder-Policy"sv, "Cross-Origin-Embedder-Policy-Report-Only"sv, "Cross-Origin-Opener-Policy"sv,
      "Cross-Origin-Opener-Policy-Report-Only"sv, "Cross-Origin-Resource-Policy"sv, "Date"sv, "Default-Style"sv, "DNT"sv, "ETag"sv, "Expect"sv,
      "Expires"sv, "Host"sv, "Icy-Metadata"sv, "Icy-Metaint"sv, "If-Match"sv, "If-Modified-Since"sv, "If-None-Match"sv, "If-Range"sv,
      "If-Unmodified-Since"sv, "Keep-Alive"sv, "Last-Event-Id"sv, "Last-Modified"sv, "Link"sv, "Location"sv, "Origin"sv, "Ping-From"sv, "Ping-To"sv,
      "Pragma"sv, "Proxy-Authorization"sv, "Proxy-Connection"sv, "Purpose"sv, "Range"sv, "Referer"sv, "Referrer-Policy"sv, "Refresh"sv,
      "Report-To"sv, "Sec-Fetch-Dest"sv, "Sec-Fetch-Mode"sv, "Sec-WebSocket-Accept"sv, "Sec-WebSocket-Extensions"sv, "Sec-WebSocket-Key"sv,
      "Sec-WebSocket-Protocol"sv, "Sec-WebSocket-Version"sv, "Server-Timing"sv, "Service-Worker"sv, "Service-Worker-Allowed"sv,
      "Service-Worker-Navigation-Preload"sv, "Set-Cookie"sv, "Set-Cookie2"sv, "Sourcemap"sv, "Strict-Transport-Security"sv, "TE"sv,
      "Timing-Allow-Origin"sv, "Trailer"sv, "Transfer-Encoding"sv, "Upgrade"sv, "Upgrade-Insecure-Requests"sv, "User-Agent"sv, "Vary"sv, "Via"sv,
      "X-Content-Type-Options"sv, "X-DNS-Prefetch-Control"sv, "X-Frame-Options"sv, "X-Sourcemap"sv, "X-Temp-Tablet"sv, "X-XSS-Protection"sv};

    bool lowercaseLess(std::string_view entry, std::string_view target) {
      const std::size_t shared = std::min(entry.size(), target.size());
      for (std::size_t index = 0; index < shared; ++index) {
        const char left = lowerAscii(entry[index]);
        if (left != target[index]) return left < target[index];
      }
      return entry.size() < target.size();
    }

    template<typename Store>
    int storeParserBytes(llhttp_t* parser, Store store) noexcept {
      try {
        store();
        return 0;
      } catch (...) {
        llhttp_set_error_reason(parser, "Could not store HTTP response bytes");
        return HPE_USER;
      }
    }

    // llhttp permits responses with repeated or non-final chunked coding.
    // Reject ambiguous framing across all Transfer-Encoding fields.
    bool validTransferEncoding(const ResponseHead& head, bool chunked) {
      bool sawChunked = false;
      for (const auto& [name, value] : head.fields) {
        if (!iequals(name, "transfer-encoding")) continue;
        if (sawChunked && value.find_first_not_of(" ,\t") != std::string::npos) return false;
        if (iequals(value, "chunked")) {
          sawChunked = true;
          continue;
        }
        for (const auto& extension : boost::beast::http::ext_list(value)) {
          const auto coding = extension.first;
          if (!iequals(coding, "chunked")) continue;
          // The field value is already OWS-trimmed. Any suffix after
          // chunked is another coding, a parameter, or a malformed tail.
          if (coding.data() + coding.size() != value.data() + value.size()) return false;
          sawChunked = true;
        }
      }
      // A trailing comma can make llhttp use EOF despite seeing chunked.
      return sawChunked == chunked;
    }
  }

  // ---- errors -------------------------------------------------------------------

  FetchError verboseError(std::string_view code, const std::string& url) {
    return FetchError(std::string(code) + " fetching \"" + url + "\". For more information, pass `verbose: true` in the second argument to fetch()", std::string(code), url);
  }

  FetchError closedError(const std::string& url) {
    return FetchError("The socket connection was closed unexpectedly. For more information, pass `verbose: true` in the second argument to fetch()", "ECONNRESET", url);
  }

  FetchError connectError(const std::string& url) { return FetchError("Unable to connect. Is the computer able to access the url?", "ConnectionRefused", url); }

  FetchError dnsError(const std::string& host, const std::string& url) {
    FetchError error("getaddrinfo ENOTFOUND " + host, "ENOTFOUND", url);
    error.errno_ = 4;
    error.syscall = "getaddrinfo";
    error.hostname = host;
    return error;
  }

  // ---- request head -------------------------------------------------------------

  std::string wireName(const std::string& name) {
    std::string lowercase(name);
    for (char& c : lowercase) c = lowerAscii(c);
    const auto position = std::lower_bound(kCommonHeaderNames.begin(), kCommonHeaderNames.end(), std::string_view(lowercase), lowercaseLess);
    if (position != kCommonHeaderNames.end() && iequals(*position, name)) return std::string(*position);
    return name;
  }

  std::string requestHead(const std::string& method, const URL& url, const Headers& headers, RequestFraming framing, std::uint64_t length, const std::string& userAgent,
                          const std::string& acceptEncoding) {
    std::string target = url.pathname() + url.search();
    if (target.size() > limits::kHttpHeaderBlockBytes || method.size() > limits::kHttpHeaderBlockBytes)
      throw verboseError("RequestHeadersTooLarge", url.href());
    if (target.empty()) target = "/";
    std::string head = method + " " + target + " HTTP/1.1\r\n";
    std::vector<std::pair<std::string, std::string>> lines;
    std::vector<std::string> cookies;
    std::size_t fieldCount = 0;
    std::size_t bytes = head.size();
    const auto charge = [&](std::size_t nameBytes, std::size_t valueBytes) {
      if (++fieldCount > limits::kHttpHeaderFields || bytes >= limits::kHttpHeaderBlockBytes ||
          nameBytes + valueBytes + 4 > limits::kHttpHeaderBlockBytes - bytes)
        throw verboseError("RequestHeadersTooLarge", url.href());
      bytes += nameBytes + valueBytes + 4;
    };
    for (const auto& [name, value] : detail::headerWireFields(headers)) {
      if (name.size() > limits::kHttpHeaderBlockBytes || value.size() > 2 * limits::kHttpHeaderBlockBytes)
        throw verboseError("RequestHeadersTooLarge", url.href());
      if (iequals(name, "content-length") || iequals(name, "transfer-encoding")) continue;
      auto encoded = latin1Bytes(value);
      charge(name.size(), encoded.size());
      lines.emplace_back(wireName(name), std::move(encoded));
    }
    for (const std::string& value : headers.getSetCookie()) {
      if (value.size() > 2 * limits::kHttpHeaderBlockBytes) throw verboseError("RequestHeadersTooLarge", url.href());
      auto encoded = latin1Bytes(value);
      charge("set-cookie"sv.size(), encoded.size());
      cookies.push_back(std::move(encoded));
    }
    std::sort(lines.begin(), lines.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
    for (const auto& [name, value] : lines) head += name + ": " + value + "\r\n";
    for (const std::string& cookie : cookies) head += "set-cookie: " + cookie + "\r\n";
    const auto appendField = [&](std::string_view name, std::string_view value) {
      charge(name.size(), value.size());
      head.append(name).append(": ").append(value).append("\r\n");
    };
    if (!headers.has("connection")) appendField("Connection", "keep-alive");
    if (!headers.has("user-agent")) {
      if (userAgent.size() > 2 * limits::kHttpHeaderBlockBytes) throw verboseError("RequestHeadersTooLarge", url.href());
      appendField("User-Agent", latin1Bytes(userAgent));
    }
    if (!headers.has("accept")) appendField("Accept", "*/*");
    if (!headers.has("host")) appendField("Host", url.host());
    if (!headers.has("accept-encoding")) appendField("Accept-Encoding", acceptEncoding);
    switch (framing) {
      case RequestFraming::length: appendField("Content-Length", std::to_string(length)); break;
      case RequestFraming::chunked: appendField("Transfer-Encoding", "chunked"); break;
      case RequestFraming::none:
        if (method != "GET" && method != "HEAD" && method != "TRACE") appendField("Content-Length", "0");
        break;
    }
    if (head.size() > limits::kHttpHeaderBlockBytes - 2) throw verboseError("RequestHeadersTooLarge", url.href());
    head += "\r\n";
    return head;
  }

  // ---- response head ------------------------------------------------------------

  std::optional<std::string> ResponseHead::field(std::string_view lowercaseName) const {
    std::optional<std::string> joined;
    for (const auto& [name, value] : fields) {
      if (!iequals(name, lowercaseName)) continue;
      if (joined) joined->append(", ").append(value);
      else joined = value;
    }
    return joined;
  }

  bool ResponseHead::connectionHas(std::string_view token) const {
    for (const auto& [name, value] : fields) {
      if (!iequals(name, "connection")) continue;
      std::string_view tokens = value;
      for (;;) {
        const auto comma = tokens.find(',');
        if (iequals(trimSpaces(tokens.substr(0, comma)), token)) return true;
        if (comma == std::string_view::npos) break;
        tokens.remove_prefix(comma + 1);
      }
    }
    return false;
  }

  const llhttp_settings_t ResponseParser::settings_ = [] {
    llhttp_settings_t settings{};
    settings.on_status = [](llhttp_t* parser, const char* data, std::size_t size) {
      if (std::any_of(data, data + size, [](unsigned char c) { return (c < 0x20 && c != '\t') || c == 0x7f; })) {
        llhttp_set_error_reason(parser, "Invalid HTTP reason phrase");
        return static_cast<int>(HPE_INVALID_STATUS);
      }
      auto& self = *static_cast<ResponseParser*>(parser->data);
      return storeParserBytes(parser, [&] { self.head_.reason.append(data, size); });
    };
    settings.on_header_field = [](llhttp_t* parser, const char* data, std::size_t size) {
      if (parser->flags & F_TRAILING) return 0;
      auto& self = *static_cast<ResponseParser*>(parser->data);
      if (self.startField_ && self.head_.fields.size() == self.maxFields_) {
        llhttp_set_error_reason(parser, "Too many HTTP response headers");
        return static_cast<int>(HPE_INVALID_HEADER_TOKEN);
      }
      return storeParserBytes(parser, [&] {
        if (self.startField_) {
          if (self.head_.fields.empty()) self.head_.fields.reserve(std::min<std::size_t>(self.maxFields_, 4));
          self.head_.fields.emplace_back();
          self.startField_ = false;
        }
        self.head_.fields.back().first.append(data, size);
      });
    };
    settings.on_header_value = [](llhttp_t* parser, const char* data, std::size_t size) {
      if (parser->flags & F_TRAILING) return 0;
      auto& self = *static_cast<ResponseParser*>(parser->data);
      return storeParserBytes(parser, [&] { self.head_.fields.back().second.append(data, size); });
    };
    settings.on_header_value_complete = [](llhttp_t* parser) {
      if (parser->flags & F_TRAILING) return 0;
      auto& self = *static_cast<ResponseParser*>(parser->data);
      std::string& value = self.head_.fields.back().second;
      const auto last = value.find_last_not_of(" \t");
      value.erase(last == std::string::npos ? 0 : last + 1);
      self.startField_ = true;
      return 0;
    };
    settings.on_header_field_complete = [](llhttp_t* parser) -> int {
      auto& self = *static_cast<ResponseParser*>(parser->data);
      if ((parser->flags & F_TRAILING) && ++self.trailerCount_ > self.maxFields_) {
        llhttp_set_error_reason(parser, "Too many HTTP response trailers");
        return HPE_USER;
      }
      return 0;
    };
    settings.on_headers_complete = [](llhttp_t*) { return static_cast<int>(HPE_PAUSED); };
    settings.on_chunk_header = [](llhttp_t* parser) -> int {
      auto& self = *static_cast<ResponseParser*>(parser->data);
      if (parser->content_length > self.bodyBytesRemaining_) {
        self.bodyLimitExceeded_ = true;
        llhttp_set_error_reason(parser, "HTTP response body exceeds limit");
        return HPE_USER;
      }
      self.chunkPart_ = parser->content_length == 0 ? ChunkPart::trailers : ChunkPart::data;
      return HPE_PAUSED;
    };
    settings.on_chunk_complete = [](llhttp_t* parser) -> int {
      auto& self = *static_cast<ResponseParser*>(parser->data);
      if (self.chunkPart_ == ChunkPart::trailers) return HPE_OK;
      self.chunkPart_ = ChunkPart::size;
      return HPE_PAUSED;
    };
    settings.on_body = [](llhttp_t* parser, const char* data, std::size_t size) {
      if (size == 0) return 0;
      auto& self = *static_cast<ResponseParser*>(parser->data);
      self.bodyBytesRemaining_ -= size;
      if (auto* out = self.bodyBytes_) {
        const auto* first = reinterpret_cast<const std::uint8_t*>(data);
        return storeParserBytes(parser, [&] {
          if (out->empty()) out->reserve(self.bodyInputSize_);
          out->insert(out->end(), first, first + size);
        });
      }
      if (self.body_) return storeParserBytes(parser, [&] { self.body_->append(data, size); });
      return 0;
    };
    settings.on_message_complete = [](llhttp_t* parser) -> int {
      static_cast<ResponseParser*>(parser->data)->bodyComplete_ = true;
      return HPE_PAUSED;
    };
    return settings;
  }();

  ResponseParser::ResponseParser(std::size_t maxFields, std::size_t maxHeadBytes)
      : maxFields_(std::min(maxFields, limits::kHttpHeaderFields)), maxHeadBytes_(std::min(maxHeadBytes, limits::kFetchResponseHeaderBlockBytes)) {
    llhttp_init(&parser_, HTTP_RESPONSE, &settings_);
    parser_.data = this;
  }

  llhttp_errno_t ResponseParser::execute(std::string_view input, std::size_t& consumed) {
    consumed = 0;
    if (parser_.error != HPE_OK) return static_cast<llhttp_errno_t>(parser_.error);
    const char* const bytes = input.empty() ? "" : input.data();
    const auto result = llhttp_execute(&parser_, bytes, input.size());
    if (result == HPE_OK) consumed = input.size();
    if (result == HPE_PAUSED) consumed = static_cast<std::size_t>(llhttp_get_error_pos(&parser_) - bytes);
    return result;
  }

  llhttp_errno_t ResponseParser::readHead(std::string_view input, std::size_t& consumed) {
    const bool invalidPrefix = firstHeadBytes_ && !input.empty() && input.front() != 'H';
    firstHeadBytes_ = firstHeadBytes_ && input.empty();
    const auto result = execute(input.substr(0, maxHeadBytes_ - headBytes_), consumed);
    headBytes_ += consumed;
    // llhttp also accepts leading blank lines, RTSP and ICE. The client
    // transports expect the response to start directly with HTTP.
    if (invalidPrefix && (result == HPE_OK || result == HPE_PAUSED)) {
      consumed = 0;
      llhttp_set_error_reason(&parser_, "Expected an HTTP response");
      return HPE_INVALID_CONSTANT;
    }
    if (result == HPE_OK && headBytes_ == maxHeadBytes_) {
      llhttp_set_error_reason(&parser_, "HTTP response head exceeds limit");
      return HPE_USER;
    }
    if (result != HPE_PAUSED) return result;
    if (llhttp_get_http_major(&parser_) != 1) {
      llhttp_set_error_reason(&parser_, "Expected HTTP/1.x");
      return HPE_INVALID_VERSION;
    }
    head_.status = llhttp_get_status_code(&parser_);
    head_.minorVersion = llhttp_get_http_minor(&parser_);
    const bool chunked = parser_.flags & F_CHUNKED;
    if (needsEof()) {
      head_.framing.framing = Framing::close;
    } else if (chunked) {
      head_.framing.framing = Framing::chunked;
    } else if (parser_.flags & F_CONTENT_LENGTH) {
      head_.framing = {Framing::length, parser_.content_length};
    }
    if ((parser_.flags & F_TRANSFER_ENCODING) && !validTransferEncoding(head_, chunked)) head_.framing.error = "UnsupportedTransferEncoding";
    return result;
  }

  llhttp_errno_t ResponseParser::beginBody(bool headRequest) {
    // llhttp handles 204, 304 and common 1xx statuses. Mark HEAD and all
    // informational responses so unknown 1xx codes also terminate at the head.
    if (headRequest || parser_.status_code / 100 == 1) parser_.flags |= F_SKIPBODY;
    llhttp_resume(&parser_);
    std::size_t consumed;
    const auto result = execute({}, consumed);
    if (result != HPE_OK) return result;
    chunked_ = head_.framing.framing == Framing::chunked;
    if (head_.framing.framing == Framing::length && head_.framing.length > bodyBytesRemaining_) {
      bodyLimitExceeded_ = true;
      llhttp_set_error_reason(&parser_, "HTTP response body exceeds limit");
      return HPE_USER;
    }
    return result;
  }

  void ResponseParser::reset() {
    llhttp_reset(&parser_);
    head_ = {};
    headBytes_ = 0;
    bodyBytesRemaining_ = limits::kHttpBodyBytes;
    metadataBytes_ = 0;
    metadataRemaining_ = limits::kHttpChunkMetadataBytes;
    trailerCount_ = 0;
    chunkPart_ = ChunkPart::size;
    bodyComplete_ = false;
    bodyLimitExceeded_ = false;
    chunked_ = false;
    firstHeadBytes_ = true;
    startField_ = true;
  }

  llhttp_errno_t ResponseParser::readBody(std::string_view input, std::size_t& consumed, ByteChunk* out) {
    bodyBytes_ = out;
    bodyInputSize_ = std::min(input.size(), bodyBytesRemaining_);
    consumed = 0;
    auto result = static_cast<llhttp_errno_t>(parser_.error);
    while (!input.empty() && result == HPE_OK) {
      std::size_t capacity = bodyBytesRemaining_;
      const bool metadata = chunked_ && chunkPart_ != ChunkPart::data;
      if (metadata) {
        const auto limit = chunkPart_ == ChunkPart::size ? limits::kHttpChunkLineBytes : std::min(maxHeadBytes_, limits::kHttpHeaderBlockBytes);
        capacity = std::min(limit - metadataBytes_, metadataRemaining_);
      } else if (chunked_) {
        // A finished chunk still has its CRLF delimiter to consume.
        capacity = parser_.content_length == 0 ? 2 : static_cast<std::size_t>(parser_.content_length);
      }
      if (capacity == 0) {
        bodyLimitExceeded_ = !metadata;
        llhttp_set_error_reason(&parser_, metadata ? "HTTP response chunk metadata exceeds limit" : "HTTP response body exceeds limit");
        result = HPE_USER;
        break;
      }
      std::size_t step = 0;
      result = execute(input.substr(0, capacity), step);
      consumed += step;
      input.remove_prefix(step);
      if (metadata) {
        metadataBytes_ += step;
        metadataRemaining_ -= step;
      }
      if (result == HPE_PAUSED && !bodyComplete_) {
        metadataBytes_ = 0;
        llhttp_resume(&parser_);
        result = HPE_OK;
      }
    }
    bodyBytes_ = nullptr;
    return result;
  }

  llhttp_errno_t ResponseParser::readBody(std::string_view input, std::size_t& consumed, std::string& out) {
    body_ = &out;
    const auto result = readBody(input, consumed);
    body_ = nullptr;
    return result;
  }

  HeadParse parseResponseHead(std::string_view data, std::size_t maxBytes, std::size_t maxFields) {
    ResponseParser parser(maxFields, maxBytes);
    HeadParse parsed;
    parsed.error = parser.readHead(data, parsed.length);
    if (parsed.error == HPE_OK || (parsed.error == HPE_USER && parser.headLimitReached())) {
      parsed.length = 0;
    } else if (parsed.error == HPE_PAUSED) {
      parsed.head = parser.takeHead();
      parsed.status = HeadStatus::complete;
    } else {
      parsed.length = 0;
      parsed.status = HeadStatus::malformed;
      parsed.errorReason = parser.errorReason();
    }
    return parsed;
  }

  FramingRule responseFraming(const std::string& method, const ResponseHead& head) {
    if (head.framing.error) return head.framing;
    if (method == "HEAD" || head.status / 100 == 1 || head.status == 204 || head.status == 304) return {};
    return head.framing;
  }

  ChunkedDecoder::Result ChunkedDecoder::decode(std::string_view input, std::size_t& offset, std::string& out) {
    if (!started_) {
      // Establish the framing for callers that supply an already identified
      // chunked body and have no HTTP head to feed into llhttp.
      constexpr auto head = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"sv;
      std::size_t consumed = 0;
      if (parser_.readHead(head, consumed) != HPE_PAUSED) return Result::invalid;
      parser_.beginBody();
      started_ = true;
    }
    std::size_t consumed = 0;
    const auto result = parser_.readBody(input.substr(offset), consumed, out);
    offset += consumed;
    if (result == HPE_OK) return Result::more;
    return result == HPE_PAUSED ? Result::done : Result::invalid;
  }

  // ---- byte queue -----------------------------------------------------------------

  void ByteQueue::consume(std::size_t count) {
    begin_ += std::min(count, size());
    if (begin_ == data_.size()) {
      data_.clear();
      begin_ = 0;
    } else if (begin_ >= 65536 && begin_ * 2 >= data_.size()) {
      data_.erase(0, begin_);
      begin_ = 0;
    }
  }

  void ByteQueue::append(const char* bytes, std::size_t count) { data_.append(bytes, count); }

  void ByteQueue::clear() noexcept {
    data_.clear();
    begin_ = 0;
  }
}
