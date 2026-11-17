#include "uos/rt/headers.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::string_view_literals;

namespace uos::rt {
  namespace {
    bool isTokenCharacter(char c) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
      return std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
    }

    // Validates a name and returns it lowercased.
    std::string normalizedName(std::string_view name, bool forHas = false) {
      if (name.empty() || !std::all_of(name.begin(), name.end(), isTokenCharacter))
        throw TypeError("Invalid header name: '" + std::string(name) + (forHas ? "\"" : "'"));
      std::string lower(name);
      for (char& c : lower)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
      return lower;
    }

    // WebKit's HTTPHeaderNames table, sorted by lowercased name. append()
    // reports an invalid value under the table's spelling of the name.
    constexpr std::array kCommonHeaderNames = {
      "Accept"sv, "Accept-Charset"sv, "Accept-Encoding"sv, "Accept-Language"sv, "Accept-Ranges"sv, "Access-Control-Allow-Credentials"sv,
      "Access-Control-Allow-Headers"sv, "Access-Control-Allow-Methods"sv, "Access-Control-Allow-Origin"sv, "Access-Control-Expose-Headers"sv,
      "Access-Control-Max-Age"sv, "Access-Control-Request-Headers"sv, "Access-Control-Request-Method"sv, "Age"sv, "Authorization"sv,
      "Cache-Control"sv, "Connection"sv, "Content-Disposition"sv, "Content-Encoding"sv, "Content-Language"sv, "Content-Length"sv,
      "Content-Location"sv, "Content-Range"sv, "Content-Security-Policy"sv, "Content-Security-Policy-Report-Only"sv, "Content-Type"sv, "Cookie"sv,
      "Cookie2"sv, "Cross-Origin-Embedder-Policy"sv, "Cross-Origin-Embedder-Policy-Report-Only"sv, "Cross-Origin-Opener-Policy"sv,
      "Cross-Origin-Opener-Policy-Report-Only"sv, "Cross-Origin-Resource-Policy"sv, "Date"sv, "Default-Style"sv, "DNT"sv, "ETag"sv, "Expect"sv,
      "Expires"sv, "Host"sv, "Icy-Metadata"sv, "Icy-MetaInt"sv, "If-Match"sv, "If-Modified-Since"sv, "If-None-Match"sv, "If-Range"sv,
      "If-Unmodified-Since"sv, "Keep-Alive"sv, "Last-Event-ID"sv, "Last-Modified"sv, "Link"sv, "Location"sv, "Origin"sv, "Ping-From"sv, "Ping-To"sv,
      "Pragma"sv, "Proxy-Authorization"sv, "Proxy-Connection"sv, "Purpose"sv, "Range"sv, "Referer"sv, "Referrer-Policy"sv, "Refresh"sv,
      "Report-To"sv, "Sec-Fetch-Dest"sv, "Sec-Fetch-Mode"sv, "Sec-WebSocket-Accept"sv, "Sec-WebSocket-Extensions"sv, "Sec-WebSocket-Key"sv,
      "Sec-WebSocket-Protocol"sv, "Sec-WebSocket-Version"sv, "Server-Timing"sv, "Service-Worker"sv, "Service-Worker-Allowed"sv,
      "Service-Worker-Navigation-Preload"sv, "Set-Cookie"sv, "Set-Cookie2"sv, "SourceMap"sv, "Strict-Transport-Security"sv, "TE"sv,
      "Timing-Allow-Origin"sv, "Trailer"sv, "Transfer-Encoding"sv, "Upgrade"sv, "Upgrade-Insecure-Requests"sv, "User-Agent"sv, "Vary"sv, "Via"sv,
      "X-Content-Type-Options"sv, "X-DNS-Prefetch-Control"sv, "X-Frame-Options"sv, "X-SourceMap"sv, "X-Temp-Tablet"sv, "X-XSS-Protection"sv};

    std::string_view commonSpelling(std::string_view lowercase) {
      auto position = std::lower_bound(kCommonHeaderNames.begin(), kCommonHeaderNames.end(), lowercase, [](std::string_view entry, std::string_view target) {
        for (std::size_t index = 0; index < entry.size() && index < target.size(); ++index) {
          const char left = entry[index] >= 'A' && entry[index] <= 'Z' ? static_cast<char>(entry[index] + 32) : entry[index];
          if (left != target[index]) return left < target[index];
        }
        return entry.size() < target.size();
      });
      if (position == kCommonHeaderNames.end() || position->size() != lowercase.size()) return {};
      for (std::size_t index = 0; index < lowercase.size(); ++index) {
        const char c = (*position)[index];
        if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c) != lowercase[index]) return {};
      }
      return *position;
    }

    bool isHttpWhitespace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

    // Strips HTTP whitespace and validates the value: no NUL, LF or CR, and no
    // code point above U+00FF (a WTF-8 lead byte past 0xC3).
    std::string normalizedValue(std::string_view name, std::string_view value) {
      std::size_t start = 0, end = value.size();
      while (start < end && isHttpWhitespace(value[start])) ++start;
      while (end > start && isHttpWhitespace(value[end - 1])) --end;
      const std::string_view stripped = value.substr(start, end - start);
      for (std::size_t index = 0; index < stripped.size(); ++index) {
        const auto byte = static_cast<unsigned char>(stripped[index]);
        bool valid = byte != 0x00 && byte != 0x0A && byte != 0x0D;
        if (byte >= 0x80) {
          valid = (byte == 0xC2 || byte == 0xC3) && index + 1 < stripped.size() && (static_cast<unsigned char>(stripped[index + 1]) & 0xC0) == 0x80;
          ++index;
        }
        if (!valid) throw TypeError("Header '" + std::string(name) + "' has invalid value: '" + std::string(stripped) + "'");
      }
      return std::string(stripped);
    }

    constexpr std::string_view kSetCookie = "set-cookie";
    constexpr std::string_view kCookie = "cookie";
  }

  Headers::Headers(std::initializer_list<Entry> init) {
    for (const auto& [name, value] : init) append(name, value);
  }

  Headers::Headers(const std::vector<Entry>& init) {
    for (const auto& [name, value] : init) append(name, value);
  }

  Headers Headers::fromSequence(const std::vector<std::vector<std::string>>& sequence) {
    Headers headers;
    for (const auto& item : sequence) {
      if (item.size() != 2) throw TypeError("Header sub-sequence must contain exactly two items");
      headers.append(item[0], item[1]);
    }
    return headers;
  }

  void Headers::append(std::string_view name, std::string_view value) {
    std::string key = normalizedName(name);
    const std::string_view spelling = commonSpelling(key);
    std::string normalized = normalizedValue(spelling.empty() ? name : spelling, value);
    if (key == kSetCookie) {
      cookies_.push_back(std::move(normalized));
      return;
    }
    auto position = std::lower_bound(fields_.begin(), fields_.end(), key, [](const Entry& entry, const std::string& target) { return entry.first < target; });
    const auto index = static_cast<std::size_t>(position - fields_.begin());
    if (position != fields_.end() && position->first == key) {
      if (key == kCookie) cookieWire_ += "; " + normalized;
      position->second += key == kCookie ? "; " : ", ";
      position->second += normalized;
    } else {
      if (key == kCookie) cookieWire_ = normalized;
      fields_.insert(position, Entry(std::move(key), std::move(normalized)));
      spellings_.insert(spellings_.begin() + static_cast<std::ptrdiff_t>(index), std::string(name));
    }
  }

  void Headers::delete_(std::string_view name) {
    const std::string key = normalizedName(name);
    if (key == kSetCookie) {
      cookies_.clear();
      return;
    }
    auto position = std::lower_bound(fields_.begin(), fields_.end(), key, [](const Entry& entry, const std::string& target) { return entry.first < target; });
    if (position != fields_.end() && position->first == key) {
      spellings_.erase(spellings_.begin() + (position - fields_.begin()));
      fields_.erase(position);
      if (key == kCookie) cookieWire_.clear();
    }
  }

  std::optional<std::string> Headers::get(std::string_view name) const {
    const std::string key = normalizedName(name);
    if (key == kSetCookie) {
      if (cookies_.empty()) return std::nullopt;
      std::string joined = cookies_.front();
      for (std::size_t index = 1; index < cookies_.size(); ++index) {
        joined += ", ";
        joined += cookies_[index];
      }
      return joined;
    }
    auto position = std::lower_bound(fields_.begin(), fields_.end(), key, [](const Entry& entry, const std::string& target) { return entry.first < target; });
    if (position != fields_.end() && position->first == key) return position->second;
    return std::nullopt;
  }

  std::vector<std::string> Headers::getSetCookie() const { return cookies_; }

  bool Headers::has(std::string_view name) const {
    const std::string key = normalizedName(name, true);
    if (key == kSetCookie) return !cookies_.empty();
    return std::binary_search(fields_.begin(), fields_.end(), Entry(key, std::string()), [](const Entry& a, const Entry& b) { return a.first < b.first; });
  }

  void Headers::set(std::string_view name, std::string_view value) {
    std::string key = normalizedName(name);
    std::string normalized = normalizedValue(name, value);
    if (key == kSetCookie) {
      cookies_.assign(1, std::move(normalized));
      return;
    }
    auto position = std::lower_bound(fields_.begin(), fields_.end(), key, [](const Entry& entry, const std::string& target) { return entry.first < target; });
    if (key == kCookie) cookieWire_ = normalized;
    if (position != fields_.end() && position->first == key) {
      position->second = std::move(normalized);
    } else {
      const auto index = position - fields_.begin();
      fields_.insert(position, Entry(std::move(key), std::move(normalized)));
      spellings_.insert(spellings_.begin() + index, std::string(name));
    }
  }

  Headers::Entry Headers::entryAt(std::size_t index) const {
    if (index < fields_.size()) return fields_[index];
    return Entry(std::string(kSetCookie), cookies_[index - fields_.size()]);
  }

  std::vector<Headers::Entry> Headers::entries() const {
    std::vector<Entry> out;
    out.reserve(count());
    for (std::size_t index = 0; index < count(); ++index) out.push_back(entryAt(index));
    return out;
  }

  std::vector<std::string> Headers::keys() const {
    std::vector<std::string> out;
    out.reserve(count());
    for (const auto& field : fields_) out.push_back(field.first);
    out.insert(out.end(), cookies_.size(), std::string(kSetCookie));
    return out;
  }

  std::vector<std::string> Headers::values() const {
    std::vector<std::string> out;
    out.reserve(count());
    for (const auto& field : fields_) out.push_back(field.second);
    out.insert(out.end(), cookies_.begin(), cookies_.end());
    return out;
  }

  namespace detail {
    std::vector<std::pair<std::string, std::string>> headerWireFields(const Headers& headers) {
      std::vector<std::pair<std::string, std::string>> out;
      out.reserve(headers.fields_.size());
      for (std::size_t index = 0; index < headers.fields_.size(); ++index) {
        const Headers::Entry& field = headers.fields_[index];
        out.emplace_back(headers.spellings_[index], field.first == kCookie ? headers.cookieWire_ : field.second);
      }
      return out;
    }
  }

  void Headers::forEach(const std::function<void(const std::string& value, const std::string& name)>& callback) const {
    for (std::size_t index = 0; index < count(); ++index) {
      const Entry entry = entryAt(index);
      callback(entry.second, entry.first);
    }
  }
}
