#include "uos/rt/url.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#ifdef UOS_GATEWAY_NATIVE_LIBRARIES
#include <memory>
#include <stdexcept>
#include <unicode/uidna.h>
#endif

namespace uos::rt {
  namespace {
    using detail::URLRecord;

    constexpr char32_t kEof = 0x110000;
    constexpr char32_t kReplacement = 0xFFFD;

    bool isAsciiAlpha(char32_t c) { return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z'); }
    bool isAsciiDigit(char32_t c) { return c >= U'0' && c <= U'9'; }
    bool isAsciiHexDigit(char32_t c) { return isAsciiDigit(c) || (c >= U'a' && c <= U'f') || (c >= U'A' && c <= U'F'); }
    bool isAsciiAlphanumeric(char32_t c) { return isAsciiAlpha(c) || isAsciiDigit(c); }
    char32_t asciiLower(char32_t c) { return c >= U'A' && c <= U'Z' ? c + 32 : c; }
    unsigned hexValue(char32_t c) {
      if (c >= U'0' && c <= U'9') return static_cast<unsigned>(c - U'0');
      if (c >= U'a' && c <= U'f') return static_cast<unsigned>(c - U'a' + 10);
      return static_cast<unsigned>(c - U'A' + 10);
    }

    // ---- UTF-8 / WTF-8 ------------------------------------------------------

    // One step of the WHATWG UTF-8 decoder over `bytes` starting at `index`.
    // With `wtf8`, three-byte surrogate encodings (ED A0..BF xx) are accepted
    // and returned as surrogate code points. Ill-formed input yields U+FFFD for
    // each maximal subpart.
    char32_t decodeStep(std::string_view bytes, std::size_t& index, bool wtf8) {
      const auto lead = static_cast<unsigned char>(bytes[index++]);
      if (lead < 0x80) return lead;
      int needed = 0;
      char32_t codePoint = 0;
      unsigned char lower = 0x80, upper = 0xBF;
      if (lead >= 0xC2 && lead <= 0xDF) {
        needed = 1;
        codePoint = lead & 0x1Fu;
      } else if (lead >= 0xE0 && lead <= 0xEF) {
        if (lead == 0xE0) lower = 0xA0;
        if (lead == 0xED && !wtf8) upper = 0x9F;
        needed = 2;
        codePoint = lead & 0x0Fu;
      } else if (lead >= 0xF0 && lead <= 0xF4) {
        if (lead == 0xF0) lower = 0x90;
        if (lead == 0xF4) upper = 0x8F;
        needed = 3;
        codePoint = lead & 0x07u;
      } else {
        return kReplacement;
      }
      for (int seen = 0; seen < needed; ++seen) {
        if (index >= bytes.size()) return kReplacement;
        const auto next = static_cast<unsigned char>(bytes[index]);
        if (next < lower || next > upper) return kReplacement;
        lower = 0x80;
        upper = 0xBF;
        codePoint = (codePoint << 6) | (next & 0x3Fu);
        ++index;
      }
      return codePoint;
    }

    // WTF-8 (a JS string) to its USVString code points: a lone surrogate becomes
    // U+FFFD and a split surrogate pair is joined.
    std::u32string toUsv(std::string_view text) {
      std::u32string out;
      out.reserve(text.size());
      std::size_t index = 0;
      while (index < text.size()) {
        char32_t codePoint = decodeStep(text, index, true);
        if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
          std::size_t lookahead = index;
          if (lookahead < text.size()) {
            const char32_t next = decodeStep(text, lookahead, true);
            if (next >= 0xDC00 && next <= 0xDFFF) {
              index = lookahead;
              out.push_back(0x10000 + ((codePoint - 0xD800) << 10) + (next - 0xDC00));
              continue;
            }
          }
          codePoint = kReplacement;
        } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
          codePoint = kReplacement;
        }
        out.push_back(codePoint);
      }
      return out;
    }

    void appendUtf8(std::string& out, char32_t codePoint) {
      if (codePoint < 0x80) {
        out.push_back(static_cast<char>(codePoint));
      } else if (codePoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
      } else if (codePoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
      } else {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
      }
    }

    std::string utf8(const std::u32string& codePoints) {
      std::string out;
      out.reserve(codePoints.size());
      for (char32_t codePoint : codePoints) appendUtf8(out, codePoint);
      return out;
    }

    bool isAscii(std::string_view text) {
      return std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    }

    // USVString conversion of a WTF-8 string, returned as UTF-8.
    std::string usvString(std::string_view text) {
      if (isAscii(text)) return std::string(text);
      return utf8(toUsv(text));
    }

    // UTF-8 decode without BOM (replacement on error), as code points.
    std::u32string utf8DecodeWithoutBom(std::string_view bytes) {
      std::u32string out;
      std::size_t index = 0;
      while (index < bytes.size()) out.push_back(decodeStep(bytes, index, false));
      return out;
    }

    // ---- percent-encoding ---------------------------------------------------

    enum class EncodeSet { C0Control, Fragment, Query, SpecialQuery, Path, Userinfo, Component, FormUrlencoded, FilePath };

    bool inSet(char32_t c, EncodeSet set) {
      switch (set) {
        case EncodeSet::C0Control:
          return c < 0x20 || c > 0x7E;
        case EncodeSet::Fragment:
          return inSet(c, EncodeSet::C0Control) || c == U' ' || c == U'"' || c == U'<' || c == U'>' || c == U'`';
        case EncodeSet::Query:
          return inSet(c, EncodeSet::C0Control) || c == U' ' || c == U'"' || c == U'#' || c == U'<' || c == U'>';
        case EncodeSet::SpecialQuery:
          return inSet(c, EncodeSet::Query) || c == U'\'';
        case EncodeSet::Path:
          return inSet(c, EncodeSet::Query) || c == U'?' || c == U'^' || c == U'`' || c == U'{' || c == U'}';
        case EncodeSet::Userinfo:
          return inSet(c, EncodeSet::Path) || c == U'/' || c == U':' || c == U';' || c == U'=' || c == U'@' || (c >= U'[' && c <= U'^') || c == U'|';
        case EncodeSet::Component:
          return inSet(c, EncodeSet::Userinfo) || (c >= U'$' && c <= U'&') || c == U'+' || c == U',';
        case EncodeSet::FormUrlencoded:
          return inSet(c, EncodeSet::Component) || c == U'!' || (c >= U'\'' && c <= U')') || c == U'~';
        case EncodeSet::FilePath:
          // pathToFileURL's own escapes; the remaining C0 controls are left to
          // the URL parser (escaped inside the path, trimmed at its end).
          return c == 0x00 || c == 0x09 || c == 0x0A || c == 0x0D || c >= 0x7F || c == U' ' || c == U'"' || c == U'#' || c == U'%' || c == U'<' || c == U'>' ||
                 c == U'?' || c == U'[' || c == U'\\' || c == U']' || c == U'^' || c == U'`' || c == U'{' || c == U'|' || c == U'}' || c == U'~';
      }
      return false;
    }

    void appendPercentByte(std::string& out, unsigned char byte) {
      static constexpr char kHex[] = "0123456789ABCDEF";
      out.push_back('%');
      out.push_back(kHex[byte >> 4]);
      out.push_back(kHex[byte & 0x0F]);
    }

    void percentEncodeInto(std::string& out, char32_t c, EncodeSet set) {
      if (!inSet(c, set)) {
        out.push_back(static_cast<char>(c));
        return;
      }
      std::string bytes;
      appendUtf8(bytes, c);
      for (char byte : bytes) appendPercentByte(out, static_cast<unsigned char>(byte));
    }

    std::string percentEncode(const std::u32string& input, EncodeSet set) {
      std::string out;
      out.reserve(input.size());
      for (char32_t c : input) percentEncodeInto(out, c, set);
      return out;
    }

    // Percent-decode bytes; malformed sequences are kept literally.
    std::string percentDecode(std::string_view input) {
      std::string out;
      out.reserve(input.size());
      for (std::size_t index = 0; index < input.size(); ++index) {
        const char c = input[index];
        if (c == '%' && index + 2 < input.size() && isAsciiHexDigit(static_cast<unsigned char>(input[index + 1])) &&
            isAsciiHexDigit(static_cast<unsigned char>(input[index + 2]))) {
          out.push_back(static_cast<char>((hexValue(static_cast<unsigned char>(input[index + 1])) << 4) | hexValue(static_cast<unsigned char>(input[index + 2]))));
          index += 2;
        } else {
          out.push_back(c);
        }
      }
      return out;
    }

    // ---- schemes ------------------------------------------------------------

    bool isSpecialScheme(std::string_view scheme) {
      return scheme == "http" || scheme == "https" || scheme == "ws" || scheme == "wss" || scheme == "ftp" || scheme == "file";
    }

    std::optional<std::uint16_t> defaultPort(std::string_view scheme) {
      if (scheme == "http" || scheme == "ws") return std::uint16_t{80};
      if (scheme == "https" || scheme == "wss") return std::uint16_t{443};
      if (scheme == "ftp") return std::uint16_t{21};
      return std::nullopt;
    }

    bool includesCredentials(const URLRecord& url) { return !url.username.empty() || !url.password.empty(); }
    bool cannotHaveUsernamePasswordPort(const URLRecord& url) { return !url.host || url.host->empty() || url.scheme == "file"; }

    // ---- hosts --------------------------------------------------------------

    bool isForbiddenHostCodePoint(char32_t c) {
      switch (c) {
        case 0x00: case 0x09: case 0x0A: case 0x0D: case U' ': case U'#': case U'/': case U':': case U'<': case U'>': case U'?': case U'@': case U'[': case U'\\':
        case U']': case U'^': case U'|':
          return true;
        default:
          return false;
      }
    }

    bool isForbiddenDomainCodePoint(char32_t c) { return isForbiddenHostCodePoint(c) || c <= 0x1F || c == U'%' || c == 0x7F; }

    std::optional<std::array<std::uint16_t, 8>> parseIPv6(const std::u32string& input) {
      std::array<std::uint16_t, 8> address{};
      std::size_t pieceIndex = 0;
      std::optional<std::size_t> compress;
      std::size_t pointer = 0;
      auto at = [&](std::size_t index) -> char32_t { return index < input.size() ? input[index] : kEof; };
      if (at(pointer) == U':') {
        if (at(pointer + 1) != U':') return std::nullopt;
        pointer += 2;
        ++pieceIndex;
        compress = pieceIndex;
      }
      while (at(pointer) != kEof) {
        if (pieceIndex == 8) return std::nullopt;
        if (at(pointer) == U':') {
          if (compress) return std::nullopt;
          ++pointer;
          ++pieceIndex;
          compress = pieceIndex;
          continue;
        }
        unsigned value = 0;
        std::size_t length = 0;
        while (length < 4 && isAsciiHexDigit(at(pointer))) {
          value = value * 0x10 + hexValue(at(pointer));
          ++pointer;
          ++length;
        }
        if (at(pointer) == U'.') {
          if (length == 0) return std::nullopt;
          pointer -= length;
          if (pieceIndex > 6) return std::nullopt;
          int numbersSeen = 0;
          while (at(pointer) != kEof) {
            std::optional<unsigned> ipv4Piece;
            if (numbersSeen > 0) {
              if (at(pointer) == U'.' && numbersSeen < 4) ++pointer;
              else return std::nullopt;
            }
            if (!isAsciiDigit(at(pointer))) return std::nullopt;
            while (isAsciiDigit(at(pointer))) {
              const unsigned number = static_cast<unsigned>(at(pointer) - U'0');
              if (!ipv4Piece) ipv4Piece = number;
              else if (*ipv4Piece == 0) return std::nullopt;
              else ipv4Piece = *ipv4Piece * 10 + number;
              if (*ipv4Piece > 255) return std::nullopt;
              ++pointer;
            }
            address[pieceIndex] = static_cast<std::uint16_t>(address[pieceIndex] * 0x100 + *ipv4Piece);
            ++numbersSeen;
            if (numbersSeen == 2 || numbersSeen == 4) ++pieceIndex;
          }
          if (numbersSeen != 4) return std::nullopt;
          break;
        } else if (at(pointer) == U':') {
          ++pointer;
          if (at(pointer) == kEof) return std::nullopt;
        } else if (at(pointer) != kEof) {
          return std::nullopt;
        }
        address[pieceIndex] = static_cast<std::uint16_t>(value);
        ++pieceIndex;
      }
      if (compress) {
        std::size_t swaps = pieceIndex - *compress;
        pieceIndex = 7;
        while (pieceIndex != 0 && swaps > 0) {
          std::swap(address[pieceIndex], address[*compress + swaps - 1]);
          --pieceIndex;
          --swaps;
        }
      } else if (pieceIndex != 8) {
        return std::nullopt;
      }
      return address;
    }

    std::string serializeIPv6(const std::array<std::uint16_t, 8>& address) {
      std::optional<std::size_t> compress;
      std::size_t bestLength = 1;
      for (std::size_t start = 0; start < 8;) {
        if (address[start] != 0) {
          ++start;
          continue;
        }
        std::size_t end = start;
        while (end < 8 && address[end] == 0) ++end;
        if (end - start > bestLength) {
          bestLength = end - start;
          compress = start;
        }
        start = end;
      }
      static constexpr char kHex[] = "0123456789abcdef";
      std::string out = "[";
      bool ignore0 = false;
      for (std::size_t pieceIndex = 0; pieceIndex < 8; ++pieceIndex) {
        if (ignore0 && address[pieceIndex] == 0) continue;
        ignore0 = false;
        if (compress && *compress == pieceIndex) {
          out += pieceIndex == 0 ? "::" : ":";
          ignore0 = true;
          continue;
        }
        std::string piece;
        unsigned value = address[pieceIndex];
        do {
          piece.insert(piece.begin(), kHex[value & 0xF]);
          value >>= 4;
        } while (value != 0);
        out += piece;
        if (pieceIndex != 7) out += ':';
      }
      return out + "]";
    }

    // IPv4 number parser: nullopt on failure; values saturate above 2^32.
    std::optional<std::uint64_t> parseIPv4Number(std::string_view input) {
      if (input.empty()) return std::nullopt;
      unsigned radix = 10;
      if (input.size() >= 2 && input[0] == '0' && (input[1] == 'x' || input[1] == 'X')) {
        input.remove_prefix(2);
        radix = 16;
      } else if (input.size() >= 2 && input[0] == '0') {
        input.remove_prefix(1);
        radix = 8;
      }
      if (input.empty()) return 0;
      std::uint64_t value = 0;
      for (char c : input) {
        const auto code = static_cast<unsigned char>(c);
        unsigned digit;
        if (radix == 16 && isAsciiHexDigit(code)) digit = hexValue(code);
        else if (radix == 10 && isAsciiDigit(code)) digit = static_cast<unsigned>(code - '0');
        else if (radix == 8 && code >= '0' && code <= '7') digit = static_cast<unsigned>(code - '0');
        else return std::nullopt;
        value = value * radix + digit;
        if (value > (std::uint64_t{1} << 40)) value = std::uint64_t{1} << 40;
      }
      return value;
    }

    std::vector<std::string_view> splitOnDot(std::string_view input) {
      std::vector<std::string_view> parts;
      std::size_t start = 0;
      for (;;) {
        const std::size_t dot = input.find('.', start);
        if (dot == std::string_view::npos) {
          parts.push_back(input.substr(start));
          return parts;
        }
        parts.push_back(input.substr(start, dot - start));
        start = dot + 1;
      }
    }

    bool endsInANumber(std::string_view input) {
      auto parts = splitOnDot(input);
      if (parts.back().empty()) {
        if (parts.size() == 1) return false;
        parts.pop_back();
      }
      const std::string_view last = parts.back();
      if (!last.empty() && std::all_of(last.begin(), last.end(), [](char c) { return isAsciiDigit(static_cast<unsigned char>(c)); })) return true;
      return parseIPv4Number(last).has_value();
    }

    std::optional<std::string> parseIPv4(std::string_view input) {
      auto parts = splitOnDot(input);
      if (parts.back().empty() && parts.size() > 1) parts.pop_back();
      if (parts.size() > 4) return std::nullopt;
      std::vector<std::uint64_t> numbers;
      for (auto part : parts) {
        auto number = parseIPv4Number(part);
        if (!number) return std::nullopt;
        numbers.push_back(*number);
      }
      for (std::size_t index = 0; index + 1 < numbers.size(); ++index)
        if (numbers[index] > 255) return std::nullopt;
      if (numbers.back() >= (std::uint64_t{1} << (8 * (5 - numbers.size())))) return std::nullopt;
      std::uint64_t ipv4 = numbers.back();
      for (std::size_t index = 0; index + 1 < numbers.size(); ++index) ipv4 += numbers[index] << (8 * (3 - index));
      std::string out;
      for (int shift = 24; shift >= 0; shift -= 8) {
        if (shift != 24) out += '.';
        out += std::to_string((ipv4 >> shift) & 0xFF);
      }
      return out;
    }

    // ---- IDNA (UTS #46 as ICU applies it for WebKit's URL parser) ------------

#ifndef UOS_GATEWAY_NATIVE_LIBRARIES
    // Mapping of a code point in the subset this runtime decides exactly: ASCII,
    // Latin-1 and the two non-Latin-1 code points their mappings produce.
    enum class IdnaStatus { Valid, Mapped, Ignored, Disallowed, Unsupported };
    struct IdnaMapping {
      IdnaStatus status;
      std::u32string mapped;
    };

    IdnaMapping idnaMapping(char32_t c) {
      if (c < 0x80) {
        if (c >= U'A' && c <= U'Z') return {IdnaStatus::Mapped, std::u32string(1, c + 32)};
        return {IdnaStatus::Valid, {}};
      }
      if (c <= 0x9F) return {IdnaStatus::Disallowed, {}};
      switch (c) {
        case 0xA0: return {IdnaStatus::Mapped, U" "};
        case 0xA8: return {IdnaStatus::Mapped, U" ̈"};
        case 0xAA: return {IdnaStatus::Mapped, U"a"};
        case 0xAD: return {IdnaStatus::Ignored, {}};
        case 0xAF: return {IdnaStatus::Mapped, U" ̄"};
        case 0xB2: return {IdnaStatus::Mapped, U"2"};
        case 0xB3: return {IdnaStatus::Mapped, U"3"};
        case 0xB4: return {IdnaStatus::Mapped, U" ́"};
        case 0xB5: return {IdnaStatus::Mapped, U"μ"};
        case 0xB8: return {IdnaStatus::Mapped, U" ̧"};
        case 0xB9: return {IdnaStatus::Mapped, U"1"};
        case 0xBA: return {IdnaStatus::Mapped, U"o"};
        case 0xBC: return {IdnaStatus::Mapped, U"1⁄4"};
        case 0xBD: return {IdnaStatus::Mapped, U"1⁄2"};
        case 0xBE: return {IdnaStatus::Mapped, U"3⁄4"};
        case 0xD7: return {IdnaStatus::Valid, {}};
        case 0x03BC: return {IdnaStatus::Valid, {}};
        case 0x2044: return {IdnaStatus::Valid, {}};
        default: break;
      }
      if (c >= 0xC0 && c <= 0xDE) return {IdnaStatus::Mapped, std::u32string(1, c + 0x20)};
      if (c <= 0xFF) return {IdnaStatus::Valid, {}};
      return {IdnaStatus::Unsupported, {}};
    }

    constexpr std::uint32_t kPunyBase = 36, kPunyTMin = 1, kPunyTMax = 26, kPunySkew = 38, kPunyDamp = 700, kPunyInitialBias = 72, kPunyInitialN = 128;

    std::uint32_t punycodeAdapt(std::uint32_t delta, std::uint32_t numPoints, bool firstTime) {
      delta = firstTime ? delta / kPunyDamp : delta / 2;
      delta += delta / numPoints;
      std::uint32_t k = 0;
      while (delta > ((kPunyBase - kPunyTMin) * kPunyTMax) / 2) {
        delta /= kPunyBase - kPunyTMin;
        k += kPunyBase;
      }
      return k + (kPunyBase - kPunyTMin + 1) * delta / (delta + kPunySkew);
    }

    char punycodeDigit(std::uint32_t digit) { return static_cast<char>(digit < 26 ? 'a' + digit : '0' + (digit - 26)); }

    std::optional<std::string> punycodeEncode(const std::u32string& input) {
      std::string output;
      for (char32_t c : input)
        if (c < 0x80) output.push_back(static_cast<char>(c));
      const auto basicCount = static_cast<std::uint32_t>(output.size());
      std::uint32_t handled = basicCount;
      if (basicCount > 0) output.push_back('-');
      std::uint32_t n = kPunyInitialN, delta = 0, bias = kPunyInitialBias;
      while (handled < input.size()) {
        char32_t next = 0x10FFFF + 1;
        for (char32_t c : input)
          if (c >= n && c < next) next = c;
        const std::uint64_t increase = static_cast<std::uint64_t>(next - n) * (handled + 1);
        if (increase > std::numeric_limits<std::uint32_t>::max() - delta) return std::nullopt;
        delta += static_cast<std::uint32_t>(increase);
        n = next;
        for (char32_t c : input) {
          if (c < n && ++delta == 0) return std::nullopt;
          if (c == n) {
            std::uint32_t q = delta;
            for (std::uint32_t k = kPunyBase;; k += kPunyBase) {
              const std::uint32_t t = k <= bias ? kPunyTMin : (k >= bias + kPunyTMax ? kPunyTMax : k - bias);
              if (q < t) break;
              output.push_back(punycodeDigit(t + (q - t) % (kPunyBase - t)));
              q = (q - t) / (kPunyBase - t);
            }
            output.push_back(punycodeDigit(q));
            bias = punycodeAdapt(delta, handled + 1, handled == basicCount);
            delta = 0;
            ++handled;
          }
        }
        ++delta;
        ++n;
      }
      return output;
    }

    std::optional<std::u32string> punycodeDecode(std::string_view input) {
      std::u32string output;
      const std::size_t delimiter = input.rfind('-');
      std::size_t in = 0;
      if (delimiter != std::string_view::npos) {
        for (std::size_t index = 0; index < delimiter; ++index) {
          const auto c = static_cast<unsigned char>(input[index]);
          if (c >= 0x80) return std::nullopt;
          output.push_back(c);
        }
        in = delimiter + 1;
      }
      std::uint32_t n = kPunyInitialN, i = 0, bias = kPunyInitialBias;
      while (in < input.size()) {
        const std::uint32_t oldI = i;
        std::uint32_t w = 1;
        for (std::uint32_t k = kPunyBase;; k += kPunyBase) {
          if (in >= input.size()) return std::nullopt;
          const auto c = static_cast<unsigned char>(input[in++]);
          std::uint32_t digit;
          if (c >= '0' && c <= '9') digit = static_cast<std::uint32_t>(c - '0' + 26);
          else if (c >= 'a' && c <= 'z') digit = static_cast<std::uint32_t>(c - 'a');
          else if (c >= 'A' && c <= 'Z') digit = static_cast<std::uint32_t>(c - 'A');
          else return std::nullopt;
          if (digit > (std::numeric_limits<std::uint32_t>::max() - i) / w) return std::nullopt;
          i += digit * w;
          const std::uint32_t t = k <= bias ? kPunyTMin : (k >= bias + kPunyTMax ? kPunyTMax : k - bias);
          if (digit < t) break;
          if (w > std::numeric_limits<std::uint32_t>::max() / (kPunyBase - t)) return std::nullopt;
          w *= kPunyBase - t;
        }
        const auto length = static_cast<std::uint32_t>(output.size() + 1);
        bias = punycodeAdapt(i - oldI, length, oldI == 0);
        if (i / length > std::numeric_limits<std::uint32_t>::max() - n) return std::nullopt;
        n += i / length;
        i %= length;
        if (n > 0x10FFFF || (n >= 0xD800 && n <= 0xDFFF)) return std::nullopt;
        output.insert(output.begin() + i, static_cast<char32_t>(n));
        ++i;
      }
      return output;
    }

    // Validates a decoded Punycode label: every code point must already be a
    // valid UTS #46 code point (ICU's isNormalized check).
    bool decodedLabelIsValid(const std::u32string& label) {
      for (char32_t c : label) {
        if (c == U'.') return false;
        if (idnaMapping(c).status != IdnaStatus::Valid) return false;
      }
      return true;
    }

#endif

    // WebKit URLParser::domainToASCII: an all-ASCII domain without an "xn--"
    // label is lowercased; anything else goes through UTS #46 ToASCII
    // (CheckBidi, CheckJoiners, nontransitional, no STD3, hyphen and length
    // errors tolerated).
    std::optional<std::string> domainToAscii(const std::u32string& domain) {
      auto startsWithXn = [](std::u32string_view label) {
        return label.size() >= 4 && asciiLower(label[0]) == U'x' && asciiLower(label[1]) == U'n' && label[2] == U'-' && label[3] == U'-';
      };
      bool allAscii = std::all_of(domain.begin(), domain.end(), [](char32_t c) { return c < 0x80; });
      bool hasXn = false;
      for (std::size_t start = 0; start <= domain.size();) {
        std::size_t dot = domain.find(U'.', start);
        if (dot == std::u32string::npos) dot = domain.size();
        if (startsWithXn(std::u32string_view(domain).substr(start, dot - start))) hasXn = true;
        start = dot + 1;
      }
      if (allAscii && !hasXn) {
        std::string out;
        out.reserve(domain.size());
        for (char32_t c : domain) out.push_back(static_cast<char>(asciiLower(c)));
        return out;
      }
#ifdef UOS_GATEWAY_NATIVE_LIBRARIES
      const std::string source = utf8(domain);
      if (source.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) return std::nullopt;
      // These options and tolerated errors preserve WebKit's non-strict URL
      // host processing, including nontransitional deviations and empty labels.
      static const std::unique_ptr<UIDNA, decltype(&uidna_close)> idna([] {
        UErrorCode status = U_ZERO_ERROR;
        UIDNA* result = uidna_openUTS46(UIDNA_CHECK_BIDI | UIDNA_CHECK_CONTEXTJ | UIDNA_NONTRANSITIONAL_TO_ASCII | UIDNA_NONTRANSITIONAL_TO_UNICODE, &status);
        if (U_FAILURE(status)) throw std::runtime_error(std::string("ICU UTS #46 initialization failed: ") + u_errorName(status));
        return result;
      }(), uidna_close);
      UIDNAInfo info = UIDNA_INFO_INITIALIZER;
      UErrorCode status = U_ZERO_ERROR;
      std::array<char, 256> buffer{};
      std::int32_t length = uidna_nameToASCII_UTF8(idna.get(), source.data(), static_cast<std::int32_t>(source.size()), buffer.data(),
                                                 static_cast<std::int32_t>(buffer.size()), &info, &status);
      std::string out;
      if (status == U_BUFFER_OVERFLOW_ERROR) {
        out.resize(static_cast<std::size_t>(length));
        info = UIDNA_INFO_INITIALIZER;
        status = U_ZERO_ERROR;
        length = uidna_nameToASCII_UTF8(idna.get(), source.data(), static_cast<std::int32_t>(source.size()), out.data(), length, &info, &status);
      } else if (U_SUCCESS(status) && length > 0) {
        out.assign(buffer.data(), static_cast<std::size_t>(length));
      }
      constexpr std::uint32_t toleratedErrors = UIDNA_ERROR_EMPTY_LABEL | UIDNA_ERROR_LABEL_TOO_LONG | UIDNA_ERROR_DOMAIN_NAME_TOO_LONG |
                                                 UIDNA_ERROR_LEADING_HYPHEN | UIDNA_ERROR_TRAILING_HYPHEN | UIDNA_ERROR_HYPHEN_3_4;
      if (U_FAILURE(status) || length <= 0 || (info.errors & ~toleratedErrors) != 0) return std::nullopt;
      return out;
#else
      std::u32string mapped;
      for (char32_t c : domain) {
        const IdnaMapping mapping = idnaMapping(c);
        switch (mapping.status) {
          case IdnaStatus::Valid: mapped.push_back(c); break;
          case IdnaStatus::Mapped: mapped += mapping.mapped; break;
          case IdnaStatus::Ignored: break;
          case IdnaStatus::Disallowed:
          case IdnaStatus::Unsupported: return std::nullopt;
        }
      }
      std::string out;
      for (std::size_t start = 0; start <= mapped.size();) {
        std::size_t dot = mapped.find(U'.', start);
        const bool last = dot == std::u32string::npos;
        if (last) dot = mapped.size();
        const std::u32string label = mapped.substr(start, dot - start);
        if (startsWithXn(label)) {
          if (label.size() == 4 || (label.size() > 5 && label.back() == U'-')) return std::nullopt;
          std::string ace = utf8(label);
          auto decoded = punycodeDecode(std::string_view(ace).substr(4));
          if (!decoded || !decodedLabelIsValid(*decoded)) return std::nullopt;
          out += ace;
        } else if (std::any_of(label.begin(), label.end(), [](char32_t c) { return c >= 0x80; })) {
          auto encoded = punycodeEncode(label);
          if (!encoded) return std::nullopt;
          out += "xn--" + *encoded;
        } else {
          out += utf8(label);
        }
        if (last) break;
        out += '.';
        start = dot + 1;
      }
      if (out.empty()) return std::nullopt;
      return out;
#endif
    }

    std::optional<std::string> parseOpaqueHost(const std::u32string& input) {
      for (char32_t c : input)
        if (isForbiddenHostCodePoint(c)) return std::nullopt;
      return percentEncode(input, EncodeSet::C0Control);
    }

    std::optional<std::string> parseHost(const std::u32string& input, bool isOpaque) {
      if (!input.empty() && input.front() == U'[') {
        if (input.back() != U']') return std::nullopt;
        auto address = parseIPv6(input.substr(1, input.size() - 2));
        if (!address) return std::nullopt;
        return serializeIPv6(*address);
      }
      if (isOpaque) return parseOpaqueHost(input);
      const std::u32string domain = utf8DecodeWithoutBom(percentDecode(utf8(input)));
      auto asciiDomain = domainToAscii(domain);
      if (!asciiDomain) return std::nullopt;
      for (char c : *asciiDomain)
        if (isForbiddenDomainCodePoint(static_cast<unsigned char>(c))) return std::nullopt;
      if (endsInANumber(*asciiDomain)) return parseIPv4(*asciiDomain);
      return asciiDomain;
    }

    // ---- the basic URL parser -------------------------------------------------

    enum class State {
      SchemeStart, Scheme, NoScheme, SpecialRelativeOrAuthority, PathOrAuthority, Relative, RelativeSlash, SpecialAuthoritySlashes, SpecialAuthorityIgnoreSlashes,
      Authority, Host, Port, File, FileSlash, FileHost, PathStart, Path, OpaquePath, Query, Fragment
    };

    bool isWindowsDriveLetter(std::u32string_view text) { return text.size() == 2 && isAsciiAlpha(text[0]) && (text[1] == U':' || text[1] == U'|'); }
    bool isWindowsDriveLetter(std::string_view text) {
      return text.size() == 2 && isAsciiAlpha(static_cast<unsigned char>(text[0])) && (text[1] == ':' || text[1] == '|');
    }
    bool isNormalizedWindowsDriveLetter(std::string_view text) {
      return text.size() == 2 && isAsciiAlpha(static_cast<unsigned char>(text[0])) && text[1] == ':';
    }
    bool startsWithWindowsDriveLetter(std::u32string_view text) {
      if (text.size() < 2 || !isWindowsDriveLetter(text.substr(0, 2))) return false;
      if (text.size() == 2) return true;
      const char32_t third = text[2];
      return third == U'/' || third == U'\\' || third == U'?' || third == U'#';
    }

    bool equalsIgnoringAsciiCase(std::string_view text, std::string_view lower) {
      if (text.size() != lower.size()) return false;
      for (std::size_t index = 0; index < text.size(); ++index)
        if (static_cast<char>(asciiLower(static_cast<unsigned char>(text[index]))) != lower[index]) return false;
      return true;
    }
    bool isSingleDotSegment(std::string_view segment) { return segment == "." || equalsIgnoringAsciiCase(segment, "%2e"); }
    bool isDoubleDotSegment(std::string_view segment) {
      return segment == ".." || equalsIgnoringAsciiCase(segment, ".%2e") || equalsIgnoringAsciiCase(segment, "%2e.") || equalsIgnoringAsciiCase(segment, "%2e%2e");
    }

    // WebKit URLParser::shouldPopPath: a file URL's first segment survives when
    // it is a drive letter in either form ("C:" or "C|").
    void shortenPath(URLRecord& url) {
      if (url.scheme == "file" && url.path.size() == 1 && isWindowsDriveLetter(std::string_view(url.path[0]))) return;
      if (!url.path.empty()) url.path.pop_back();
    }

    std::string asciiOf(const std::u32string& text) {
      std::string out;
      out.reserve(text.size());
      for (char32_t c : text) out.push_back(static_cast<char>(c));
      return out;
    }

    std::u32string removeTabAndNewline(std::u32string input) {
      input.erase(std::remove_if(input.begin(), input.end(), [](char32_t c) { return c == 0x09 || c == 0x0A || c == 0x0D; }), input.end());
      return input;
    }

    // https://url.spec.whatwg.org/#concept-basic-url-parser as WebKit's
    // URLParser runs it (no state override: WebKit's setters reparse a spliced
    // href instead). `input` is already stripped as the caller requires.
    bool basicParse(const std::u32string& input, const URLRecord* base, URLRecord& url) {
      State state = State::SchemeStart;
      std::u32string buffer;
      bool atSignSeen = false, insideBrackets = false, passwordTokenSeen = false;
      // WebKit only rewrites a leading "C|" path segment to "C:" when the input
      // did not spell out a non-empty file host ("file://h/C|" keeps the "|").
      bool fileHostWritten = false;
      const auto length = static_cast<std::ptrdiff_t>(input.size());
      std::ptrdiff_t pointer = 0;
      auto at = [&](std::ptrdiff_t index) -> char32_t { return index >= 0 && index < length ? input[static_cast<std::size_t>(index)] : kEof; };
      auto special = [&] { return isSpecialScheme(url.scheme); };
      auto fromPointer = [&] { return std::u32string_view(input).substr(static_cast<std::size_t>(std::min(std::max<std::ptrdiff_t>(pointer, 0), length))); };
      for (;;) {
        const char32_t c = at(pointer);
        switch (state) {
          case State::SchemeStart:
            if (isAsciiAlpha(c)) {
              buffer.push_back(asciiLower(c));
              state = State::Scheme;
            } else {
              state = State::NoScheme;
              --pointer;
            }
            break;
          case State::Scheme:
            if (isAsciiAlphanumeric(c) || c == U'+' || c == U'-' || c == U'.') {
              buffer.push_back(asciiLower(c));
            } else if (c == U':') {
              url.scheme = asciiOf(buffer);
              buffer.clear();
              if (url.scheme == "file") {
                state = State::File;
              } else if (special() && base && base->scheme == url.scheme) {
                state = State::SpecialRelativeOrAuthority;
              } else if (special()) {
                state = State::SpecialAuthoritySlashes;
              } else if (at(pointer + 1) == U'/') {
                state = State::PathOrAuthority;
                ++pointer;
              } else {
                url.hasOpaquePath = true;
                url.opaquePath.clear();
                state = State::OpaquePath;
              }
            } else {
              buffer.clear();
              state = State::NoScheme;
              pointer = -1;
            }
            break;
          case State::NoScheme:
            if (!base || (base->hasOpaquePath && c != U'#')) return false;
            if (base->hasOpaquePath && c == U'#') {
              url.scheme = base->scheme;
              url.hasOpaquePath = true;
              url.opaquePath = base->opaquePath;
              url.path = base->path;
              url.query = base->query;
              url.fragment = std::string();
              state = State::Fragment;
            } else if (base->scheme != "file") {
              state = State::Relative;
              --pointer;
            } else {
              state = State::File;
              --pointer;
            }
            break;
          case State::SpecialRelativeOrAuthority:
            if (c == U'/' && at(pointer + 1) == U'/') {
              state = State::SpecialAuthorityIgnoreSlashes;
              ++pointer;
            } else {
              state = State::Relative;
              --pointer;
            }
            break;
          case State::PathOrAuthority:
            if (c == U'/') {
              state = State::Authority;
            } else {
              state = State::Path;
              --pointer;
            }
            break;
          case State::Relative:
            url.scheme = base->scheme;
            if (c == U'/' || (special() && c == U'\\')) {
              state = State::RelativeSlash;
            } else {
              url.username = base->username;
              url.password = base->password;
              url.host = base->host;
              url.port = base->port;
              url.path = base->path;
              url.query = base->query;
              if (c == U'?') {
                url.query = std::string();
                state = State::Query;
              } else if (c == U'#') {
                url.fragment = std::string();
                state = State::Fragment;
              } else if (c != kEof) {
                url.query.reset();
                shortenPath(url);
                state = State::Path;
                --pointer;
              }
            }
            break;
          case State::RelativeSlash:
            if (special() && (c == U'/' || c == U'\\')) {
              state = State::SpecialAuthorityIgnoreSlashes;
            } else if (c == U'/') {
              state = State::Authority;
            } else {
              url.username = base->username;
              url.password = base->password;
              url.host = base->host;
              url.port = base->port;
              state = State::Path;
              --pointer;
            }
            break;
          case State::SpecialAuthoritySlashes:
            if (c == U'/' && at(pointer + 1) == U'/') {
              state = State::SpecialAuthorityIgnoreSlashes;
              ++pointer;
            } else {
              state = State::SpecialAuthorityIgnoreSlashes;
              --pointer;
            }
            break;
          case State::SpecialAuthorityIgnoreSlashes:
            if (c != U'/' && c != U'\\') {
              state = State::Authority;
              --pointer;
            }
            break;
          case State::Authority:
            if (c == U'@') {
              if (atSignSeen) buffer.insert(0, U"%40");
              atSignSeen = true;
              for (char32_t codePoint : buffer) {
                if (codePoint == U':' && !passwordTokenSeen) {
                  passwordTokenSeen = true;
                  continue;
                }
                percentEncodeInto(passwordTokenSeen ? url.password : url.username, codePoint, EncodeSet::Userinfo);
              }
              buffer.clear();
            } else if (c == kEof || c == U'/' || c == U'?' || c == U'#' || (special() && c == U'\\')) {
              if (atSignSeen && buffer.empty()) return false;
              pointer -= static_cast<std::ptrdiff_t>(buffer.size()) + 1;
              buffer.clear();
              state = State::Host;
            } else {
              buffer.push_back(c);
            }
            break;
          case State::Host:
            if (c == U':' && !insideBrackets) {
              if (buffer.empty()) return false;
              auto parsed = parseHost(buffer, !special());
              if (!parsed) return false;
              url.host = std::move(*parsed);
              buffer.clear();
              state = State::Port;
            } else if (c == kEof || c == U'/' || c == U'?' || c == U'#' || (special() && c == U'\\')) {
              --pointer;
              if (special() && buffer.empty()) return false;
              auto parsed = parseHost(buffer, !special());
              if (!parsed) return false;
              url.host = std::move(*parsed);
              buffer.clear();
              state = State::PathStart;
              // WebKit quirk: a non-special authority that contained "@" and ends
              // directly in "?" or "#" gets the path "/".
              if (!special() && atSignSeen && (c == U'?' || c == U'#')) url.path.emplace_back();
            } else {
              if (c == U'[') insideBrackets = true;
              if (c == U']') insideBrackets = false;
              buffer.push_back(c);
            }
            break;
          case State::Port:
            if (isAsciiDigit(c)) {
              buffer.push_back(c);
            } else if (c == kEof || c == U'/' || c == U'?' || c == U'#' || (special() && c == U'\\')) {
              if (!buffer.empty()) {
                std::uint32_t value = 0;
                for (char32_t digit : buffer) {
                  value = value * 10 + static_cast<std::uint32_t>(digit - U'0');
                  if (value > 65535) return false;
                }
                const auto portValue = static_cast<std::uint16_t>(value);
                if (defaultPort(url.scheme) == portValue) url.port.reset();
                else url.port = portValue;
                buffer.clear();
              }
              if (!special() && atSignSeen && (c == U'?' || c == U'#')) url.path.emplace_back();
              state = State::PathStart;
              --pointer;
            } else {
              return false;
            }
            break;
          case State::File:
            url.scheme = "file";
            url.host = std::string();
            if (c == U'/' || c == U'\\') {
              state = State::FileSlash;
            } else if (base && base->scheme == "file") {
              url.host = base->host;
              url.path = base->path;
              url.query = base->query;
              if (c == U'?') {
                url.query = std::string();
                state = State::Query;
              } else if (c == U'#') {
                url.fragment = std::string();
                state = State::Fragment;
              } else if (c != kEof) {
                url.query.reset();
                if (!startsWithWindowsDriveLetter(fromPointer())) shortenPath(url);
                else url.path.clear();
                state = State::Path;
                --pointer;
              }
            } else {
              state = State::Path;
              --pointer;
            }
            break;
          case State::FileSlash:
            if (c == U'/' || c == U'\\') {
              state = State::FileHost;
            } else {
              if (base && base->scheme == "file") {
                url.host = base->host;
                if (!startsWithWindowsDriveLetter(fromPointer()) && !base->path.empty() && isNormalizedWindowsDriveLetter(base->path[0]))
                  url.path.push_back(base->path[0]);
              }
              state = State::Path;
              --pointer;
            }
            break;
          case State::FileHost:
            if (c == kEof || c == U'/' || c == U'\\' || c == U'?' || c == U'#') {
              --pointer;
              if (isWindowsDriveLetter(buffer)) {
                // WebKit quirk: "file://C:?q" keeps the drive letter as if a "/"
                // followed it, so the path becomes "/C:/".
                if (c == U'?' || c == U'#') {
                  std::string letter = asciiOf(buffer);
                  letter[1] = ':';
                  url.path.push_back(std::move(letter));
                  buffer.clear();
                }
                state = State::Path;
              } else if (buffer.empty()) {
                url.host = std::string();
                state = State::PathStart;
              } else {
                auto parsed = parseHost(buffer, !special());
                if (!parsed) return false;
                if (*parsed == "localhost") parsed->clear();
                url.host = std::move(*parsed);
                fileHostWritten = true;
                buffer.clear();
                state = State::PathStart;
              }
            } else {
              buffer.push_back(c);
            }
            break;
          case State::PathStart:
            if (special()) {
              state = State::Path;
              if (c != U'/' && c != U'\\') --pointer;
            } else if (c == U'?') {
              url.query = std::string();
              state = State::Query;
            } else if (c == U'#') {
              url.fragment = std::string();
              state = State::Fragment;
            } else if (c != kEof) {
              state = State::Path;
              if (c != U'/') --pointer;
            }
            break;
          case State::Path:
            if (c == kEof || c == U'/' || (special() && c == U'\\') || c == U'?' || c == U'#') {
              std::string segment = asciiOf(buffer);
              const bool slash = c == U'/' || (special() && c == U'\\');
              if (isDoubleDotSegment(segment)) {
                shortenPath(url);
                if (!slash) url.path.emplace_back();
              } else if (isSingleDotSegment(segment) && !slash) {
                url.path.emplace_back();
              } else if (!isSingleDotSegment(segment)) {
                if (url.scheme == "file" && url.path.empty() && !fileHostWritten && isWindowsDriveLetter(std::string_view(segment))) segment[1] = ':';
                url.path.push_back(std::move(segment));
              }
              buffer.clear();
              if (c == U'?') {
                url.query = std::string();
                state = State::Query;
              }
              if (c == U'#') {
                url.fragment = std::string();
                state = State::Fragment;
              }
            } else {
              std::string encoded;
              percentEncodeInto(encoded, c, EncodeSet::Path);
              for (char byte : encoded) buffer.push_back(static_cast<unsigned char>(byte));
            }
            break;
          case State::OpaquePath:
            if (c == U'?') {
              url.query = std::string();
              state = State::Query;
            } else if (c == U'#') {
              url.fragment = std::string();
              state = State::Fragment;
            } else if (c == U' ') {
              const char32_t next = at(pointer + 1);
              url.opaquePath += next == U'?' || next == U'#' ? "%20" : " ";
            } else if (c != kEof) {
              percentEncodeInto(url.opaquePath, c, EncodeSet::C0Control);
            }
            break;
          case State::Query:
            if (c == U'#' || c == kEof) {
              *url.query += percentEncode(buffer, special() ? EncodeSet::SpecialQuery : EncodeSet::Query);
              buffer.clear();
              if (c == U'#') {
                url.fragment = std::string();
                state = State::Fragment;
              }
            } else {
              buffer.push_back(c);
            }
            break;
          case State::Fragment:
            if (c != kEof) percentEncodeInto(*url.fragment, c, EncodeSet::Fragment);
            break;
        }
        if (pointer >= length) break;
        ++pointer;
      }
      return true;
    }

    std::u32string stripC0ControlOrSpace(std::u32string input) {
      std::size_t start = 0, end = input.size();
      while (start < end && input[start] <= 0x20) ++start;
      while (end > start && input[end - 1] <= 0x20) --end;
      return input.substr(start, end - start);
    }

    std::optional<URLRecord> parseRecord(std::string_view input, const URLRecord* base) {
      URLRecord url;
      if (!basicParse(removeTabAndNewline(stripC0ControlOrSpace(toUsv(input))), base, url)) return std::nullopt;
      return url;
    }


    std::string serializePath(const URLRecord& url) {
      if (url.hasOpaquePath) return url.opaquePath;
      std::string out;
      for (const auto& segment : url.path) {
        out += '/';
        out += segment;
      }
      return out;
    }

    // The href cut at WebKit's component offsets (hostStart, hostEnd,
    // pathStart, pathEnd, queryEnd).
    struct HrefParts {
      std::string beforeHost;  // "scheme:", plus "//" and "user:pass@" when there is a host
      std::string host;
      std::string port;  // ":81" or empty
      std::string dotSlash;  // "/." before a path starting "//" when there is no host; WebKit's pathStart follows it
      std::string path;
      std::string query;  // "?q" or empty
      std::string fragment;  // "#f" or empty
    };

    HrefParts hrefParts(const URLRecord& url) {
      HrefParts parts;
      parts.beforeHost = url.scheme + ":";
      if (url.host) {
        parts.beforeHost += "//";
        if (includesCredentials(url)) {
          parts.beforeHost += url.username;
          if (!url.password.empty()) parts.beforeHost += ":" + url.password;
          parts.beforeHost += '@';
        }
        parts.host = *url.host;
        if (url.port) parts.port = ":" + std::to_string(*url.port);
      }
      if (!url.host && !url.hasOpaquePath && url.path.size() > 1 && url.path[0].empty()) parts.dotSlash = "/.";
      parts.path = serializePath(url);
      if (url.query) parts.query = "?" + *url.query;
      if (url.fragment) parts.fragment = "#" + *url.fragment;
      return parts;
    }

    std::string serialize(const URLRecord& url) {
      const HrefParts parts = hrefParts(url);
      return parts.beforeHost + parts.host + parts.port + parts.dotSlash + parts.path + parts.query + parts.fragment;
    }

    // WebKit's setters splice the new component into the href and parse the
    // result; DOMURL::setFullURL then hands that URL's serialization to
    // setHref, which parses it once more ("file://h/C|" given the host
    // "localhost" ends as "file:///C:").
    std::optional<URLRecord> reparseSpliced(std::string_view href) {
      auto first = parseRecord(href, nullptr);
      if (!first) return std::nullopt;
      return parseRecord(serialize(*first), nullptr);
    }

    // The pathname, search and hash setters keep C0 controls and spaces (which
    // the reparse would otherwise trim at the end of the href) by escaping
    // them first; tabs and newlines are still dropped by the parser.
    void appendEscapingC0OrSpace(std::string& out, char c) {
      const auto byte = static_cast<unsigned char>(c);
      if (byte <= 0x20 && byte != 0x09 && byte != 0x0A && byte != 0x0D) appendPercentByte(out, byte);
      else out.push_back(c);
    }

    std::size_t utf16Length(std::string_view text) {
      std::size_t length = 0;
      for (char32_t c : toUsv(text)) length += c >= 0x10000 ? 2 : 1;
      return length;
    }

    // URL::setHost and URL::setHostAndPort drop everything from the first
    // character that would end a host.
    std::string_view cutAtHostTerminator(std::string_view value, bool special) {
      const std::size_t index = value.find_first_of(special ? std::string_view("/\\?#") : std::string_view("/?#"));
      return index == std::string_view::npos ? value : value.substr(0, index);
    }

    // URL::appendEncodedHostname: ASCII (or longer than 2048 UTF-16 units) is
    // kept as written; anything else goes through UTS #46 ToASCII as is,
    // without percent-decoding.
    std::optional<std::string> encodedHostname(std::string_view host) {
      if (isAscii(host) || utf16Length(host) > 2048) return std::string(host);
      return domainToAscii(toUsv(host));
    }

    std::optional<std::uint16_t> parseDecimalPort(std::string_view digits) {
      std::uint32_t value = 0;
      if (digits.empty()) return std::nullopt;
      for (char c : digits) {
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
        if (value > 65535) return std::nullopt;
      }
      return static_cast<std::uint16_t>(value);
    }

    // URL::setHost.
    std::optional<URLRecord> withHost(const URLRecord& url, std::string_view newHost) {
      const bool special = isSpecialScheme(url.scheme);
      newHost = cutAtHostTerminator(newHost, special);
      if (newHost.find(':') != std::string_view::npos && newHost.front() != '[') return std::nullopt;
      if (newHost.find('@') != std::string_view::npos) return std::nullopt;
      std::string host(newHost);
      if (special) {
        auto encoded = encodedHostname(newHost);
        if (!encoded) return std::nullopt;
        host = std::move(*encoded);
      }
      const HrefParts parts = hrefParts(url);
      return reparseSpliced(parts.beforeHost + (url.host ? "" : "//") + host + parts.port + parts.path + parts.query + parts.fragment);
    }

    // URL::setHostAndPort: a port that does not fit 16 bits drops the port.
    std::optional<URLRecord> withHostAndPort(const URLRecord& url, std::string_view hostAndPort) {
      const bool special = isSpecialScheme(url.scheme);
      hostAndPort = cutAtHostTerminator(hostAndPort, special);
      const std::size_t colon = hostAndPort.rfind(':');
      if (colon == 0) return std::nullopt;
      const std::size_t bracket = hostAndPort.rfind(']');
      if (colon == std::string_view::npos || (bracket != std::string_view::npos && bracket > colon)) return withHost(url, hostAndPort);
      const std::string_view hostName = hostAndPort.substr(0, colon);
      if (hostName.find(':') != std::string_view::npos && bracket == std::string_view::npos) return std::nullopt;
      const std::optional<std::uint16_t> port = parseDecimalPort(hostAndPort.substr(colon + 1));
      if (hostName.find('@') != std::string_view::npos) return std::nullopt;
      std::string host(hostName);
      if (special) {
        auto encoded = encodedHostname(hostName);
        if (!encoded) return std::nullopt;
        host = std::move(*encoded);
      }
      const HrefParts parts = hrefParts(url);
      return reparseSpliced(parts.beforeHost + (url.host ? "" : "//") + host + (port ? ":" + std::to_string(*port) : std::string()) + parts.path + parts.query +
                            parts.fragment);
    }

    std::string tupleOrigin(const URLRecord& url) {
      std::string out = url.scheme + ":";
      if (!url.host) return out;
      out += "//" + *url.host;
      if (url.port) out += ":" + std::to_string(*url.port);
      return out;
    }

    // WebKit's SecurityOrigin serialization: special non-file schemes and
    // "ftps" are tuple origins; blob: uses its inner URL (whose file: origin
    // serializes as "file://" + host); everything else is opaque.
    std::string originOf(const URLRecord& url) {
      auto hasTupleOrigin = [](const std::string& scheme) { return (isSpecialScheme(scheme) && scheme != "file") || scheme == "ftps"; };
      if (url.scheme == "blob") {
        auto inner = parseRecord(serializePath(url), nullptr);
        if (!inner) return "null";
        if (inner->scheme == "file") return "file://" + inner->host.value_or("");
        if (hasTupleOrigin(inner->scheme)) return tupleOrigin(*inner);
        return "null";
      }
      if (hasTupleOrigin(url.scheme)) return tupleOrigin(url);
      return "null";
    }

    // ---- application/x-www-form-urlencoded --------------------------------------

    std::vector<URLSearchParams::Entry> formUrlencodedParse(std::string_view input) {
      std::vector<URLSearchParams::Entry> output;
      const std::string bytes = usvString(input);
      std::size_t start = 0;
      while (start <= bytes.size()) {
        std::size_t end = bytes.find('&', start);
        if (end == std::string::npos) end = bytes.size();
        std::string_view sequence = std::string_view(bytes).substr(start, end - start);
        if (!sequence.empty()) {
          std::string name, value;
          const std::size_t equals = sequence.find('=');
          if (equals == std::string_view::npos) {
            name = std::string(sequence);
          } else {
            name = std::string(sequence.substr(0, equals));
            value = std::string(sequence.substr(equals + 1));
          }
          std::replace(name.begin(), name.end(), '+', ' ');
          std::replace(value.begin(), value.end(), '+', ' ');
          output.emplace_back(utf8(utf8DecodeWithoutBom(percentDecode(name))), utf8(utf8DecodeWithoutBom(percentDecode(value))));
        }
        start = end + 1;
      }
      return output;
    }

    void formEncodeInto(std::string& out, std::string_view text) {
      for (char32_t c : toUsv(text)) {
        if (c == U' ') out.push_back('+');
        else percentEncodeInto(out, c, EncodeSet::FormUrlencoded);
      }
    }

    std::u16string toUtf16(std::string_view text) {
      std::u16string out;
      for (char32_t c : toUsv(text)) {
        if (c >= 0x10000) {
          out.push_back(static_cast<char16_t>(0xD800 + ((c - 0x10000) >> 10)));
          out.push_back(static_cast<char16_t>(0xDC00 + ((c - 0x10000) & 0x3FF)));
        } else {
          out.push_back(static_cast<char16_t>(c));
        }
      }
      return out;
    }

    // ---- node:url helpers ---------------------------------------------------------

    std::string currentWorkingDirectory() {
      std::string buffer(256, '\0');
      for (;;) {
        if (::getcwd(buffer.data(), buffer.size()) != nullptr) {
          buffer.resize(std::char_traits<char>::length(buffer.c_str()));
          return buffer;
        }
        if (errno != ERANGE) return "/";
        buffer.resize(buffer.size() * 2);
      }
    }

    // path.posix.resolve(cwd, path) for one argument.
    std::string posixResolve(std::string_view path) {
      std::string joined = !path.empty() && path.front() == '/' ? std::string(path) : currentWorkingDirectory() + "/" + std::string(path);
      std::vector<std::string> segments;
      std::size_t start = 0;
      while (start <= joined.size()) {
        std::size_t slash = joined.find('/', start);
        if (slash == std::string::npos) slash = joined.size();
        const std::string segment = joined.substr(start, slash - start);
        if (segment == "..") {
          if (!segments.empty()) segments.pop_back();
        } else if (!segment.empty() && segment != ".") {
          segments.push_back(segment);
        }
        start = slash + 1;
      }
      std::string out;
      for (const auto& segment : segments) out += "/" + segment;
      return out.empty() ? "/" : out;
    }

    bool isValidUtf8(std::string_view bytes) {
      std::size_t index = 0;
      while (index < bytes.size()) {
        const std::size_t before = index;
        const char32_t c = decodeStep(bytes, index, false);
        if (c == kReplacement && !(index - before == 3 && static_cast<unsigned char>(bytes[before]) == 0xEF &&
                                   static_cast<unsigned char>(bytes[before + 1]) == 0xBF && static_cast<unsigned char>(bytes[before + 2]) == 0xBD))
          return false;
      }
      return true;
    }
  }

  // ---- URLSearchParams --------------------------------------------------------------

  URLSearchParams::URLSearchParams(std::string_view init) {
    if (!init.empty() && init.front() == '?') init.remove_prefix(1);
    list_ = formUrlencodedParse(init);
  }

  URLSearchParams::URLSearchParams(std::vector<Entry> sequence) {
    list_.reserve(sequence.size());
    for (auto& [name, value] : sequence) list_.emplace_back(usvString(name), usvString(value));
  }

  URLSearchParams URLSearchParams::fromSequence(const std::vector<std::vector<std::string>>& sequence) {
    URLSearchParams params;
    for (const auto& pair : sequence) {
      if (pair.size() != 2) throw TypeError("Type error");
      params.list_.emplace_back(usvString(pair[0]), usvString(pair[1]));
    }
    return params;
  }

  URLSearchParams URLSearchParams::fromRecord(const std::vector<Entry>& record) {
    auto arrayIndex = [](const std::string& key) -> std::optional<std::uint64_t> {
      if (key.empty() || key.size() > 10 || (key.size() > 1 && key[0] == '0')) return std::nullopt;
      std::uint64_t value = 0;
      for (char c : key) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
      }
      if (value > 4294967294u) return std::nullopt;
      return value;
    };
    std::vector<Entry> ordered;
    for (const auto& [key, value] : record) {
      const std::string name = usvString(key);
      auto existing = std::find_if(ordered.begin(), ordered.end(), [&](const Entry& entry) { return entry.first == name; });
      if (existing != ordered.end()) existing->second = usvString(value);
      else ordered.emplace_back(name, usvString(value));
    }
    std::stable_partition(ordered.begin(), ordered.end(), [&](const Entry& entry) { return arrayIndex(entry.first).has_value(); });
    auto firstNonIndex = std::find_if(ordered.begin(), ordered.end(), [&](const Entry& entry) { return !arrayIndex(entry.first).has_value(); });
    std::stable_sort(ordered.begin(), firstNonIndex, [&](const Entry& a, const Entry& b) { return *arrayIndex(a.first) < *arrayIndex(b.first); });
    URLSearchParams params;
    params.list_ = std::move(ordered);
    return params;
  }

  URLSearchParams& URLSearchParams::operator=(const URLSearchParams& other) {
    if (this != &other) {
      list_ = other.list_;
      update();
    }
    return *this;
  }

  URLSearchParams& URLSearchParams::operator=(URLSearchParams&& other) noexcept {
    if (this != &other) {
      list_ = std::move(other.list_);
      update();
    }
    return *this;
  }

  void URLSearchParams::update() {
    if (url_ == nullptr) return;
    std::string serialized = toString();
    if (serialized.empty()) url_->record_.query.reset();
    else url_->record_.query = std::move(serialized);
  }

  void URLSearchParams::append(std::string_view name, std::string_view value) {
    list_.emplace_back(usvString(name), usvString(value));
    update();
  }

  void URLSearchParams::delete_(std::string_view name) {
    const std::string key = usvString(name);
    list_.erase(std::remove_if(list_.begin(), list_.end(), [&](const Entry& entry) { return entry.first == key; }), list_.end());
    update();
  }

  void URLSearchParams::delete_(std::string_view name, std::string_view value) {
    const std::string key = usvString(name), expected = usvString(value);
    list_.erase(std::remove_if(list_.begin(), list_.end(), [&](const Entry& entry) { return entry.first == key && entry.second == expected; }), list_.end());
    update();
  }

  std::optional<std::string> URLSearchParams::get(std::string_view name) const {
    const std::string key = usvString(name);
    for (const auto& entry : list_)
      if (entry.first == key) return entry.second;
    return std::nullopt;
  }

  std::vector<std::string> URLSearchParams::getAll(std::string_view name) const {
    const std::string key = usvString(name);
    std::vector<std::string> out;
    for (const auto& entry : list_)
      if (entry.first == key) out.push_back(entry.second);
    return out;
  }

  bool URLSearchParams::has(std::string_view name) const {
    const std::string key = usvString(name);
    return std::any_of(list_.begin(), list_.end(), [&](const Entry& entry) { return entry.first == key; });
  }

  bool URLSearchParams::has(std::string_view name, std::string_view value) const {
    const std::string key = usvString(name), expected = usvString(value);
    return std::any_of(list_.begin(), list_.end(), [&](const Entry& entry) { return entry.first == key && entry.second == expected; });
  }

  void URLSearchParams::set(std::string_view name, std::string_view value) {
    const std::string key = usvString(name);
    auto first = std::find_if(list_.begin(), list_.end(), [&](const Entry& entry) { return entry.first == key; });
    if (first == list_.end()) {
      list_.emplace_back(key, usvString(value));
    } else {
      first->second = usvString(value);
      list_.erase(std::remove_if(std::next(first), list_.end(), [&](const Entry& entry) { return entry.first == key; }), list_.end());
    }
    update();
  }

  void URLSearchParams::sort() {
    std::vector<std::pair<std::u16string, Entry>> keyed;
    keyed.reserve(list_.size());
    for (auto& entry : list_) keyed.emplace_back(toUtf16(entry.first), std::move(entry));
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    list_.clear();
    for (auto& item : keyed) list_.push_back(std::move(item.second));
    update();
  }

  std::string URLSearchParams::toString() const {
    std::string out;
    for (std::size_t index = 0; index < list_.size(); ++index) {
      if (index) out.push_back('&');
      formEncodeInto(out, list_[index].first);
      out.push_back('=');
      formEncodeInto(out, list_[index].second);
    }
    return out;
  }

  std::vector<std::string> URLSearchParams::keys() const {
    std::vector<std::string> out;
    for (const auto& entry : list_) out.push_back(entry.first);
    return out;
  }

  std::vector<std::string> URLSearchParams::values() const {
    std::vector<std::string> out;
    for (const auto& entry : list_) out.push_back(entry.second);
    return out;
  }

  void URLSearchParams::forEach(const std::function<void(const std::string& value, const std::string& key)>& callback) const {
    for (std::size_t index = 0; index < list_.size(); ++index) {
      const Entry entry = list_[index];
      callback(entry.second, entry.first);
    }
  }

  // ---- URL ------------------------------------------------------------------------------

  URL::URL(std::string_view url) {
    auto parsed = parseRecord(url, nullptr);
    if (!parsed) throw InvalidURLError(std::string(url));
    adoptRecord(std::move(*parsed));
  }

  URL::URL(std::string_view url, std::string_view base) {
    auto parsedBase = parseRecord(base, nullptr);
    if (!parsedBase) throw InvalidURLError(std::string(url));
    auto parsed = parseRecord(url, &*parsedBase);
    if (!parsed) throw InvalidURLError(std::string(url));
    adoptRecord(std::move(*parsed));
  }

  URL::URL(std::string_view url, const URL& base) {
    auto parsed = parseRecord(url, &base.record_);
    if (!parsed) throw InvalidURLError(std::string(url));
    adoptRecord(std::move(*parsed));
  }

  URL::URL(const URL& other) : record_(other.record_), searchParams_(other.searchParams_) { searchParams_.url_ = this; }

  URL::URL(URL&& other) noexcept : record_(std::move(other.record_)), searchParams_(std::move(other.searchParams_)) { searchParams_.url_ = this; }

  URL& URL::operator=(const URL& other) {
    if (this != &other) {
      record_ = other.record_;
      searchParams_.list_ = other.searchParams_.list_;
      searchParams_.url_ = this;
    }
    return *this;
  }

  URL& URL::operator=(URL&& other) noexcept {
    if (this != &other) {
      record_ = std::move(other.record_);
      searchParams_.list_ = std::move(other.searchParams_.list_);
      searchParams_.url_ = this;
    }
    return *this;
  }

  void URL::adoptRecord(detail::URLRecord record) {
    record_ = std::move(record);
    resetSearchParams();
  }

  void URL::resetSearchParams() {
    searchParams_.url_ = this;
    searchParams_.list_ = record_.query ? formUrlencodedParse(*record_.query) : std::vector<URLSearchParams::Entry>{};
  }

  bool URL::canParse(std::string_view url) { return parseRecord(url, nullptr).has_value(); }

  bool URL::canParse(std::string_view url, std::string_view base) {
    auto parsedBase = parseRecord(base, nullptr);
    return parsedBase && parseRecord(url, &*parsedBase).has_value();
  }

  std::optional<URL> URL::parse(std::string_view url) {
    auto parsed = parseRecord(url, nullptr);
    if (!parsed) return std::nullopt;
    URL out;
    out.adoptRecord(std::move(*parsed));
    return out;
  }

  std::optional<URL> URL::parse(std::string_view url, std::string_view base) {
    auto parsedBase = parseRecord(base, nullptr);
    if (!parsedBase) return std::nullopt;
    auto parsed = parseRecord(url, &*parsedBase);
    if (!parsed) return std::nullopt;
    URL out;
    out.adoptRecord(std::move(*parsed));
    return out;
  }

  std::string URL::href() const { return serialize(record_); }

  void URL::href(std::string_view value) {
    auto parsed = parseRecord(value, nullptr);
    if (!parsed) throw InvalidURLError(std::string(value));
    adoptRecord(std::move(*parsed));
  }

  std::string URL::origin() const { return originOf(record_); }

  std::string URL::protocol() const { return record_.scheme + ":"; }

  // The setters below follow WebKit's URLDecomposition and URL setters: each
  // splices its value into the href and keeps the reparsed result only when it
  // is valid, so parser quirks apply to the outcome. Every committed result
  // also re-reads the linked URLSearchParams, as DOMURL::setHref does.
  void URL::protocol(std::string_view value) {
    std::string scheme;
    for (char32_t c : removeTabAndNewline(toUsv(value))) {
      if (c == U':') break;
      if (scheme.empty() ? !isAsciiAlpha(c) : !(isAsciiAlphanumeric(c) || c == U'+' || c == U'-' || c == U'.')) return;
      scheme.push_back(static_cast<char>(asciiLower(c)));
    }
    if (scheme.empty() || isSpecialScheme(record_.scheme) != isSpecialScheme(scheme)) return;
    if ((includesCredentials(record_) || record_.port) && scheme == "file") return;
    if (record_.scheme == "file" && record_.host && record_.host->empty()) return;
    if (auto updated = reparseSpliced(scheme + serialize(record_).substr(record_.scheme.size()))) adoptRecord(std::move(*updated));
  }

  void URL::username(std::string_view value) {
    if (cannotHaveUsernamePasswordPort(record_)) return;
    URLRecord candidate = record_;
    candidate.username = percentEncode(toUsv(value), EncodeSet::Userinfo);
    if (auto updated = reparseSpliced(serialize(candidate))) adoptRecord(std::move(*updated));
  }

  void URL::password(std::string_view value) {
    if (cannotHaveUsernamePasswordPort(record_)) return;
    URLRecord candidate = record_;
    candidate.password = percentEncode(toUsv(value), EncodeSet::Userinfo);
    if (auto updated = reparseSpliced(serialize(candidate))) adoptRecord(std::move(*updated));
  }

  std::string URL::host() const {
    if (!record_.host) return "";
    if (!record_.port) return *record_.host;
    return *record_.host + ":" + std::to_string(*record_.port);
  }

  // URLDecomposition::setHost: the text after the last ':' counts as a port
  // only through its leading digits; the scheme's default port keeps the
  // current port, and an out-of-range port drops it.
  void URL::host(std::string_view value) {
    if (value.empty() && record_.scheme != "file" && isSpecialScheme(record_.scheme)) return;
    const std::size_t separator = value.rfind(':');
    if (separator == 0 || record_.hasOpaquePath) return;
    const std::size_t bracket = value.rfind(']');
    std::optional<URLRecord> updated;
    if (separator == std::string_view::npos || (bracket != std::string_view::npos && bracket > separator)) {
      updated = withHost(record_, value);
    } else {
      if (value.find(':') != separator && bracket == std::string_view::npos) return;
      std::size_t digits = 0;
      while (separator + 1 + digits < value.size() && isAsciiDigit(static_cast<unsigned char>(value[separator + 1 + digits]))) ++digits;
      if (digits == 0) {
        updated = withHost(record_, value.substr(0, separator));
      } else {
        const std::optional<std::uint16_t> port = parseDecimalPort(value.substr(separator + 1, digits));
        if (port && defaultPort(record_.scheme) == *port) updated = withHostAndPort(record_, value.substr(0, separator));
        else updated = withHostAndPort(record_, value.substr(0, separator + 1 + digits));
      }
    }
    if (updated) adoptRecord(std::move(*updated));
  }

  std::string URL::hostname() const { return record_.host.value_or(""); }

  void URL::hostname(std::string_view value) {
    if (value.empty() && record_.scheme != "file" && isSpecialScheme(record_.scheme)) return;
    if (record_.hasOpaquePath) return;
    if (auto updated = withHost(record_, value)) adoptRecord(std::move(*updated));
  }

  std::string URL::port() const { return record_.port ? std::to_string(*record_.port) : std::string(); }

  // URLDecomposition::setPort: tabs and newlines are skipped, digits run up to
  // the first other character, and no digits at all (or the default port)
  // removes the port.
  void URL::port(std::string_view value) {
    if (cannotHaveUsernamePasswordPort(record_)) return;
    std::uint32_t number = 0;
    bool foundDigit = false;
    for (const char c : value) {
      if (c == '\t' || c == '\n' || c == '\r') continue;
      if (isAsciiDigit(static_cast<unsigned char>(c))) {
        number = number * 10 + static_cast<std::uint32_t>(c - '0');
        foundDigit = true;
        if (number > 65535) return;
        continue;
      }
      if (!foundDigit) return;
      break;
    }
    const HrefParts parts = hrefParts(record_);
    std::string href = parts.beforeHost + parts.host;
    if (foundDigit && defaultPort(record_.scheme) != number) href += ":" + std::to_string(number);
    href += parts.path + parts.query + parts.fragment;
    if (auto updated = reparseSpliced(href)) adoptRecord(std::move(*updated));
  }

  std::string URL::pathname() const { return serializePath(record_); }

  // URL::setPath: "?", "#" and non-ASCII are percent-encoded before splicing.
  void URL::pathname(std::string_view value) {
    if (record_.hasOpaquePath) return;
    const bool special = isSpecialScheme(record_.scheme);
    const HrefParts parts = hrefParts(record_);
    std::string href = parts.beforeHost + parts.host + parts.port + parts.dotSlash;
    const bool startsWithSlash = !value.empty() && (value.front() == '/' || (value.front() == '\\' && special));
    if (!startsWithSlash && !(!special && value.empty() && record_.host)) href += '/';
    if (!special && (!record_.host || record_.host->empty()) && value.size() > 2 && value.substr(0, 2) == "//") href += "/.";
    for (char32_t c : toUsv(value)) {
      if (c == U'?' || c == U'#' || c >= 0x80) {
        std::string bytes;
        appendUtf8(bytes, c);
        for (char byte : bytes) appendPercentByte(href, static_cast<unsigned char>(byte));
      } else {
        appendEscapingC0OrSpace(href, static_cast<char>(c));
      }
    }
    href += parts.query + parts.fragment;
    if (auto updated = reparseSpliced(href)) adoptRecord(std::move(*updated));
  }

  std::string URL::search() const { return record_.query && !record_.query->empty() ? "?" + *record_.query : std::string(); }

  // URLDecomposition::setSearch escapes "#" so it cannot start a fragment.
  void URL::search(std::string_view value) {
    const HrefParts parts = hrefParts(record_);
    std::string href = parts.beforeHost + parts.host + parts.port + parts.dotSlash + parts.path;
    if (!value.empty()) {
      if (value.front() != '?') href += '?';
      for (const char c : value) {
        if (c == '#') href += "%23";
        else appendEscapingC0OrSpace(href, c);
      }
    }
    href += parts.fragment;
    if (auto updated = reparseSpliced(href)) adoptRecord(std::move(*updated));
  }

  std::string URL::hash() const { return record_.fragment && !record_.fragment->empty() ? "#" + *record_.fragment : std::string(); }

  void URL::hash(std::string_view value) {
    const HrefParts parts = hrefParts(record_);
    std::string href = parts.beforeHost + parts.host + parts.port + parts.dotSlash + parts.path + parts.query;
    if (!value.empty()) {
      if (value.front() == '#') value.remove_prefix(1);
      href += '#';
      for (const char c : value) appendEscapingC0OrSpace(href, c);
    }
    if (auto updated = reparseSpliced(href)) adoptRecord(std::move(*updated));
  }

  // ---- node:url ------------------------------------------------------------------------

  URL pathToFileURL(std::string_view path) {
    std::string resolved = posixResolve(path);
    if (!path.empty() && path.back() == '/' && resolved.back() != '/') resolved += '/';
    std::string encoded = "file://";
    for (char32_t c : toUsv(resolved)) {
      if (inSet(c, EncodeSet::FilePath)) {
        std::string bytes;
        appendUtf8(bytes, c);
        for (char byte : bytes) appendPercentByte(encoded, static_cast<unsigned char>(byte));
      } else {
        encoded.push_back(static_cast<char>(c));
      }
    }
    return URL(encoded);
  }

  std::string fileURLToPath(const URL& url) {
    if (url.protocol() != "file:") throw NodeURLError("The URL must be of scheme file", "ERR_INVALID_URL_SCHEME");
    if (!url.hostname().empty()) throw NodeURLError("File URL host must be \"localhost\" or empty on linux", "ERR_INVALID_FILE_URL_HOST");
    const std::string pathname = url.pathname();
    for (std::size_t index = 0; index + 2 < pathname.size(); ++index) {
      if (pathname[index] == '%' && pathname[index + 1] == '2' && (pathname[index + 2] | 0x20) == 'f')
        throw NodeURLError("File URL path must not include encoded / characters", "ERR_INVALID_FILE_URL_PATH");
    }
    std::string decoded = percentDecode(pathname);
    if (!isValidUtf8(decoded)) return "";
    return decoded;
  }

  std::string fileURLToPath(std::string_view url) {
    auto parsed = URL::parse(url);
    if (!parsed) throw NodeURLError("The URL must be of scheme file", "ERR_INVALID_URL_SCHEME");
    return fileURLToPath(*parsed);
  }
}
