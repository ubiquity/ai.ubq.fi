// HTTP/1.1 wire rules of Bun's fetch client: error texts, the request head,
// the response head parser (picohttpparser's grammar as Bun applies it), body
// framing and chunked decoding.
#include "fetch_transport_internal.hpp"

#include "uos/rt/string.hpp"

#include <algorithm>
#include <array>
#include <limits>

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

    // RFC 9110 tchar: the bytes picohttpparser accepts in a field name.
    bool isTokenByte(unsigned char c) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
      return std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string_view::npos;
    }

    // picohttpparser's get_token_to_eol: the bytes up to the line end (CRLF or
    // LF). Control bytes other than tab end the token; one that is not a line
    // end is an error.
    enum class LineResult : std::uint8_t { complete, incomplete, invalid };
    LineResult tokenToEol(std::string_view data, std::size_t& position, std::string_view& token) {
      const std::size_t start = position;
      for (;;) {
        if (position >= data.size()) return LineResult::incomplete;
        const auto byte = static_cast<unsigned char>(data[position]);
        if ((byte < 0x20 && byte != '\t') || byte == 0x7F) break;
        ++position;
      }
      const auto byte = static_cast<unsigned char>(data[position]);
      if (byte == '\r') {
        if (position + 1 >= data.size()) return LineResult::incomplete;
        if (data[position + 1] != '\n') return LineResult::invalid;
        token = data.substr(start, position - start);
        position += 2;
        return LineResult::complete;
      }
      if (byte == '\n') {
        token = data.substr(start, position - start);
        position += 1;
        return LineResult::complete;
      }
      return LineResult::invalid;
    }

    HeadParse incomplete() { return HeadParse{}; }
    HeadParse malformed() {
      HeadParse result;
      result.status = HeadStatus::malformed;
      return result;
    }

    std::optional<std::uint64_t> parseContentLength(std::string_view value) {
      const std::string_view digits = trimSpaces(value);
      if (digits.empty()) return std::nullopt;
      std::uint64_t length = 0;
      for (const char c : digits) {
        if (c < '0' || c > '9') return std::nullopt;
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (length > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return std::nullopt;
        length = length * 10 + digit;
      }
      return length;
    }

    int hexValue(char c) {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
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

  HeadParse parseResponseHead(std::string_view data) {
    HeadParse result;
    std::size_t position = 0;
    // "HTTP/1." and one digit.
    constexpr std::string_view prefix = "HTTP/1.";
    for (std::size_t index = 0; index < prefix.size(); ++index) {
      if (position >= data.size()) return incomplete();
      if (data[position] != prefix[index]) return malformed();
      ++position;
    }
    if (position >= data.size()) return incomplete();
    if (data[position] < '0' || data[position] > '9') return malformed();
    result.head.minorVersion = data[position] - '0';
    ++position;
    // One or more spaces.
    if (position >= data.size()) return incomplete();
    if (data[position] != ' ') return malformed();
    while (position < data.size() && data[position] == ' ') ++position;
    // Three digits and the next byte are needed before the status is judged.
    if (data.size() - position < 4) return incomplete();
    int status = 0;
    for (int digit = 0; digit < 3; ++digit) {
      const char c = data[position++];
      if (c < '0' || c > '9') return malformed();
      status = status * 10 + (c - '0');
    }
    result.head.status = status;
    std::string_view reason;
    const LineResult reasonLine = tokenToEol(data, position, reason);
    if (reasonLine == LineResult::incomplete) return incomplete();
    if (reasonLine == LineResult::invalid) return malformed();
    if (!reason.empty()) {
      if (reason.front() != ' ') return malformed();
      while (!reason.empty() && reason.front() == ' ') reason.remove_prefix(1);
    }
    result.head.reason = std::string(reason);
    for (;;) {
      if (position >= data.size()) return incomplete();
      if (data[position] == '\r') {
        if (position + 1 >= data.size()) return incomplete();
        if (data[position + 1] != '\n') return malformed();
        position += 2;
        break;
      }
      if (data[position] == '\n') {
        ++position;
        break;
      }
      if (result.head.fields.size() == kMaxResponseFields) return malformed();
      // A continuation line (obs-fold) or an empty name is rejected.
      const std::size_t nameStart = position;
      for (;;) {
        if (position >= data.size()) return incomplete();
        const auto byte = static_cast<unsigned char>(data[position]);
        if (byte == ':') break;
        if (!isTokenByte(byte)) return malformed();
        ++position;
      }
      if (position == nameStart) return malformed();
      const std::string_view name = data.substr(nameStart, position - nameStart);
      ++position;
      while (position < data.size() && (data[position] == ' ' || data[position] == '\t')) ++position;
      std::string_view value;
      const LineResult valueLine = tokenToEol(data, position, value);
      if (valueLine == LineResult::incomplete) return incomplete();
      if (valueLine == LineResult::invalid) return malformed();
      while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
      result.head.fields.emplace_back(std::string(name), std::string(value));
    }
    result.status = HeadStatus::complete;
    result.length = position;
    return result;
  }

  // ---- response body ------------------------------------------------------------

  FramingRule responseFraming(const std::string& method, const ResponseHead& head) {
    FramingRule rule;
    bool chunked = false;
    std::optional<std::uint64_t> length;
    std::vector<std::string_view> transferCodings;
    for (const auto& [name, value] : head.fields) {
      if (equalsIgnoringCase(name, "content-length")) {
        const std::optional<std::uint64_t> parsed = parseContentLength(value);
        if (!parsed || (length && *length != *parsed)) {
          rule.error = "InvalidContentLength";
          return rule;
        }
        length = parsed;
      } else if (equalsIgnoringCase(name, "transfer-encoding")) {
        for (const std::string_view token : splitTokens(value)) {
          if (!token.empty()) transferCodings.push_back(token);
        }
        std::size_t count = 0;
        for (const std::string_view token : transferCodings) {
          if (equalsIgnoringCase(token, "chunked")) ++count;
        }
        if (count > 1 || (count == 1 && !equalsIgnoringCase(transferCodings.back(), "chunked"))) {
          rule.error = "UnsupportedTransferEncoding";
          return rule;
        }
        chunked = count == 1;
      }
    }
    if (method == "HEAD" || method == "TRACE" || head.status == 204 || head.status == 304) {
      rule.framing = Framing::empty;
    } else if (chunked) {
      rule.framing = Framing::chunked;
    } else if (length) {
      rule.framing = Framing::length;
      rule.length = *length;
    } else {
      rule.framing = Framing::close;
    }
    return rule;
  }

  ChunkedDecoder::Result ChunkedDecoder::decode(std::string_view input, std::size_t& offset, std::string& out) {
    for (;;) {
      switch (step_) {
        case Step::size: {
          // Hex digits (at most 16), then anything up to the LF.
          std::size_t position = offset;
          std::uint64_t size = 0;
          std::size_t digits = 0;
          for (;;) {
            if (position >= input.size()) return Result::more;
            const int value = hexValue(input[position]);
            if (value < 0) break;
            if (digits == 16) return Result::invalid;
            size = size * 16 + static_cast<std::uint64_t>(value);
            ++digits;
            ++position;
          }
          if (digits == 0) return Result::invalid;
          const std::size_t lineEnd = input.find('\n', position);
          if (lineEnd == std::string_view::npos) return Result::more;
          offset = lineEnd + 1;
          remaining_ = size;
          step_ = size == 0 ? Step::trailer : Step::data;
          break;
        }
        case Step::data: {
          if (offset >= input.size()) return Result::more;
          const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(remaining_, input.size() - offset));
          out.append(input.data() + offset, take);
          offset += take;
          remaining_ -= take;
          if (remaining_ == 0) step_ = Step::dataEnd;
          break;
        }
        case Step::dataEnd: {
          // CRs, then the LF ending the chunk data.
          std::size_t position = offset;
          while (position < input.size() && input[position] == '\r') ++position;
          if (position >= input.size()) return Result::more;
          if (input[position] != '\n') return Result::invalid;
          offset = position + 1;
          step_ = Step::size;
          break;
        }
        case Step::trailer: {
          // Trailer lines up to an empty line.
          std::size_t position = offset;
          while (position < input.size() && input[position] == '\r') ++position;
          if (position >= input.size()) return Result::more;
          if (input[position] == '\n') {
            offset = position + 1;
            return Result::done;
          }
          const std::size_t lineEnd = input.find('\n', position);
          if (lineEnd == std::string_view::npos) return Result::more;
          offset = lineEnd + 1;
          break;
        }
      }
    }
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
