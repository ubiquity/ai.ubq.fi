// Fetch's HTTP/1.x request writer and llhttp response parser.
#include "fetch_transport_internal.hpp"

#include "uos/rt/string.hpp"

#include <algorithm>
#include <array>

using namespace std::string_view_literals;

namespace uos::rt::detail::fetch {
  namespace {
    char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

    bool equalsIgnoringCase(std::string_view left, std::string_view right) {
      if (left.size() != right.size()) return false;
      for (std::size_t index = 0; index < left.size(); ++index) {
        if (lowerAscii(left[index]) != lowerAscii(right[index])) return false;
      }
      return true;
    }

    std::string_view trimSpaces(std::string_view text) {
      while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
      while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
      return text;
    }

    std::vector<std::string_view> splitTokens(std::string_view text) {
      std::vector<std::string_view> tokens;
      std::size_t start = 0;
      for (;;) {
        const std::size_t comma = text.find(',', start);
        tokens.push_back(trimSpaces(text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start)));
        if (comma == std::string_view::npos) return tokens;
        start = comma + 1;
      }
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
    if (position != kCommonHeaderNames.end() && equalsIgnoringCase(*position, name)) return std::string(*position);
    return name;
  }

  std::string requestHead(const std::string& method, const URL& url, const Headers& headers, RequestFraming framing, std::uint64_t length, const std::string& userAgent,
                          const std::string& acceptEncoding) {
    std::string target = url.pathname() + url.search();
    if (target.empty()) target = "/";
    std::string head = method + " " + target + " HTTP/1.1\r\n";
    std::vector<std::pair<std::string, std::string>> lines;
    std::vector<std::string> cookies;
    for (const auto& [name, value] : detail::headerWireFields(headers)) {
      if (equalsIgnoringCase(name, "content-length") || equalsIgnoringCase(name, "transfer-encoding")) continue;
      lines.emplace_back(wireName(name), latin1Bytes(value));
    }
    for (const std::string& value : headers.getSetCookie()) cookies.push_back(latin1Bytes(value));
    std::sort(lines.begin(), lines.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
    for (const auto& [name, value] : lines) head += name + ": " + value + "\r\n";
    for (const std::string& cookie : cookies) head += "set-cookie: " + cookie + "\r\n";
    if (!headers.has("connection")) head += "Connection: keep-alive\r\n";
    if (!headers.has("user-agent")) head += "User-Agent: " + latin1Bytes(userAgent) + "\r\n";
    if (!headers.has("accept")) head += "Accept: */*\r\n";
    if (!headers.has("host")) head += "Host: " + url.host() + "\r\n";
    if (!headers.has("accept-encoding")) head += "Accept-Encoding: " + acceptEncoding + "\r\n";
    switch (framing) {
      case RequestFraming::length: head += "Content-Length: " + std::to_string(length) + "\r\n"; break;
      case RequestFraming::chunked: head += "Transfer-Encoding: chunked\r\n"; break;
      case RequestFraming::none:
        if (method != "GET" && method != "HEAD" && method != "TRACE") head += "Content-Length: 0\r\n";
        break;
    }
    head += "\r\n";
    return head;
  }

  // ---- response head ------------------------------------------------------------

  std::vector<std::string> ResponseHead::values(std::string_view lowercaseName) const {
    std::vector<std::string> found;
    for (const auto& [name, value] : fields) {
      if (equalsIgnoringCase(name, lowercaseName)) found.push_back(value);
    }
    return found;
  }

  std::optional<std::string> ResponseHead::field(std::string_view lowercaseName) const {
    const std::vector<std::string> found = values(lowercaseName);
    if (found.empty()) return std::nullopt;
    std::string joined = found.front();
    for (std::size_t index = 1; index < found.size(); ++index) joined += ", " + found[index];
    return joined;
  }

  bool ResponseHead::connectionHas(std::string_view token) const {
    for (const std::string& value : values("connection")) {
      for (const std::string_view item : splitTokens(value)) {
        if (equalsIgnoringCase(item, token)) return true;
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
    settings.on_headers_complete = [](llhttp_t*) { return static_cast<int>(HPE_PAUSED); };
    settings.on_body = [](llhttp_t* parser, const char* data, std::size_t size) {
      auto& self = *static_cast<ResponseParser*>(parser->data);
      return storeParserBytes(parser, [&] { self.body_->append(data, size); });
    };
    settings.on_message_complete = [](llhttp_t*) { return static_cast<int>(HPE_PAUSED); };
    return settings;
  }();

  ResponseParser::ResponseParser(std::size_t maxFields) : maxFields_(maxFields) {
    llhttp_init(&parser_, HTTP_RESPONSE, &settings_);
    parser_.data = this;
  }

  llhttp_errno_t ResponseParser::execute(std::string_view input, std::size_t& consumed) {
    consumed = 0;
    if (const auto previous = llhttp_get_errno(&parser_); previous != HPE_OK) return previous;
    const char* const bytes = input.empty() ? "" : input.data();
    const auto result = llhttp_execute(&parser_, bytes, input.size());
    if (result == HPE_OK) consumed = input.size();
    if (result == HPE_PAUSED) consumed = static_cast<std::size_t>(llhttp_get_error_pos(&parser_) - bytes);
    return result;
  }

  llhttp_errno_t ResponseParser::readHead(std::string_view input, std::size_t& consumed) {
    const bool invalidPrefix = firstHeadBytes_ && !input.empty() && input.front() != 'H';
    firstHeadBytes_ = firstHeadBytes_ && input.empty();
    const auto result = execute(input, consumed);
    // llhttp also accepts leading blank lines, RTSP and ICE. The client
    // transports expect the response to start directly with HTTP.
    if (invalidPrefix && (result == HPE_OK || result == HPE_PAUSED)) {
      consumed = 0;
      llhttp_set_error_reason(&parser_, "Expected an HTTP response");
      return HPE_INVALID_CONSTANT;
    }
    if (result != HPE_PAUSED) return result;
    if (llhttp_get_http_major(&parser_) != 1) {
      llhttp_set_error_reason(&parser_, "Expected HTTP/1.x");
      return HPE_INVALID_VERSION;
    }
    head_.status = llhttp_get_status_code(&parser_);
    head_.minorVersion = llhttp_get_http_minor(&parser_);
    if (llhttp_message_needs_eof(&parser_)) {
      head_.framing.framing = Framing::close;
    } else if (parser_.flags & F_CHUNKED) {
      head_.framing.framing = Framing::chunked;
    } else if (parser_.flags & F_CONTENT_LENGTH) {
      head_.framing = {Framing::length, parser_.content_length, std::nullopt};
    }
    return result;
  }

  llhttp_errno_t ResponseParser::readBody(std::string_view input, std::size_t& consumed, std::string& out) {
    body_ = &out;
    const auto result = execute(input, consumed);
    body_ = nullptr;
    return result;
  }

  HeadParse parseResponseHead(std::string_view data, std::size_t maxBytes, std::size_t maxFields) {
    ResponseParser parser(maxFields);
    HeadParse parsed;
    parsed.error = parser.readHead(data.substr(0, maxBytes), parsed.length);
    if (parsed.error == HPE_OK) {
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
    // llhttp accepts repeated or non-final chunked coding in responses. Keep
    // rejecting these ambiguous transfer codings across all header fields.
    bool sawChunked = false;
    for (const auto& [name, value] : head.fields) {
      if (!equalsIgnoringCase(name, "transfer-encoding")) continue;
      std::string_view codings = value;
      for (;;) {
        const auto comma = codings.find(',');
        const auto token = trimSpaces(codings.substr(0, comma));
        if (!token.empty()) {
          if (sawChunked) return {Framing::empty, 0, "UnsupportedTransferEncoding"};
          sawChunked = equalsIgnoringCase(token, "chunked");
        }
        if (comma == std::string_view::npos) break;
        codings.remove_prefix(comma + 1);
      }
    }
    if (method == "HEAD" || method == "TRACE" || head.status == 204 || head.status == 304) return {};
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
