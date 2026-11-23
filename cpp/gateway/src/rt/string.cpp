#include "uos/rt/string.hpp"
#include "uos/rt/limits.hpp"
#include "third_party/simdutf/simdutf.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

// ICU headers from a staged prefix can enter through a regular include path.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include <unicode/casemap.h>
#include <unicode/ucol.h>
#include <unicode/ustring.h>
#pragma GCC diagnostic pop

namespace uos::rt {
  namespace {
    using Byte = unsigned char;

    // JSC's String::MaxLength; longer results throw RangeError("Out of memory").
    constexpr double MAX_STRING_LENGTH = 2147483647.0;

    // ---- WTF-8 reading -------------------------------------------------------------
    struct Element {
      std::uint32_t value;  // code point, lone surrogate, or U+FFFD for ill-formed bytes
      std::size_t size;     // bytes consumed
      bool verbatim;        // the consumed bytes are already the canonical encoding
    };

    // One code unit-level element: a scalar value, an encoded surrogate, or one maximal
    // ill-formed subpart (WHATWG UTF-8 decoder rule) read as U+FFFD.
    inline Element decodeUnit(const Byte* p, const Byte* end) {
      const unsigned lead = p[0];
      if (lead < 0x80) return {lead, 1, true};
      std::size_t need = 0;
      std::uint32_t value = 0;
      unsigned lower = 0x80, upper = 0xBF;
      if (lead >= 0xC2 && lead <= 0xDF) {
        need = 1;
        value = lead & 0x1F;
      } else if (lead >= 0xE0 && lead <= 0xEF) {
        need = 2;
        value = lead & 0x0F;
        if (lead == 0xE0) lower = 0xA0;
      } else if (lead >= 0xF0 && lead <= 0xF4) {
        need = 3;
        value = lead & 0x07;
        if (lead == 0xF0) lower = 0x90;
        else if (lead == 0xF4) upper = 0x8F;
      } else {
        return {0xFFFD, 1, false};
      }
      for (std::size_t index = 1; index <= need; ++index) {
        if (p + index >= end) return {0xFFFD, index, false};
        const unsigned next = p[index];
        if (next < lower || next > upper) return {0xFFFD, index, false};
        lower = 0x80;
        upper = 0xBF;
        value = (value << 6) | (next & 0x3F);
      }
      return {value, need + 1, true};
    }

    constexpr bool isHighSurrogate(std::uint32_t value) { return value >= 0xD800 && value <= 0xDBFF; }
    constexpr bool isLowSurrogate(std::uint32_t value) { return value >= 0xDC00 && value <= 0xDFFF; }
    constexpr bool isSurrogate(std::uint32_t value) { return value >= 0xD800 && value <= 0xDFFF; }

    // One code point-level element: like decodeUnit, but a split pair (encoded high
    // surrogate directly followed by an encoded low surrogate) reads as the pair.
    inline Element decodePoint(const Byte* p, const Byte* end) {
      Element element = decodeUnit(p, end);
      if (isHighSurrogate(element.value) && p + 3 < end && p[3] == 0xED) {
        const Element next = decodeUnit(p + 3, end);
        if (isLowSurrogate(next.value)) {
          return {0x10000 + ((element.value - 0xD800) << 10) + (next.value - 0xDC00), 6, false};
        }
      }
      return element;
    }

    inline const Byte* bytes(std::string_view text) { return reinterpret_cast<const Byte*>(text.data()); }

    void encodeUtf8(std::string& out, std::uint32_t value) {
      if (value < 0x80) {
        out.push_back(static_cast<char>(value));
      } else if (value < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (value >> 6)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
      } else if (value < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (value >> 12)));
        out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
      } else {
        out.push_back(static_cast<char>(0xF0 | (value >> 18)));
        out.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
      }
    }

    // True when `out` ends with an encoded lone high surrogate.
    inline bool endsWithHighSurrogate(const std::string& out) {
      const std::size_t size = out.size();
      return size >= 3 && static_cast<Byte>(out[size - 3]) == 0xED && static_cast<Byte>(out[size - 2]) >= 0xA0 && static_cast<Byte>(out[size - 2]) <= 0xAF;
    }
    inline std::uint32_t trailingHighSurrogate(const std::string& out) {
      const std::size_t size = out.size();
      return 0xD000 | ((static_cast<std::uint32_t>(static_cast<Byte>(out[size - 2])) & 0x3F) << 6) | (static_cast<std::uint32_t>(static_cast<Byte>(out[size - 1])) & 0x3F);
    }

    // Appends one element read from source bytes, keeping the output canonical.
    inline void appendElement(std::string& out, const Byte* p, const Element& element) {
      if (element.verbatim && !isLowSurrogate(element.value)) {
        out.append(reinterpret_cast<const char*>(p), element.size);
      } else {
        appendCodePoint(out, element.value);
      }
    }

    enum class Shape { Ascii, Utf8, Other };
    // Ascii: every byte < 0x80. Utf8: well-formed UTF-8 without encoded surrogates (so
    // byte search and byte slicing at code point boundaries are exact). Other: anything else.
    Shape shapeOf(std::string_view text) {
      if (text.size() > limits::kTextInputBytes) throw RangeError("String input exceeds limit");
      if (isAscii(text)) return Shape::Ascii;
      return simdutf::validate_utf8(text.data(), text.size()) ? Shape::Utf8 : Shape::Other;
    }

    // The UTF-8 validator rejects every surrogate. WTF-8 admits only an encoded surrogate here.
    bool validWtf8(std::string_view text, bool canonical) noexcept {
      if (text.size() > limits::kTextInputBytes) return false;
      bool afterHigh = false;
      while (!text.empty()) {
        const auto prefix = simdutf::validate_utf8_with_errors(text.data(), text.size());
        if (prefix.is_ok()) return true;
        if (prefix.error != simdutf::SURROGATE) return false;
        const bool high = static_cast<Byte>(text[prefix.count + 1]) < 0xB0;
        if (canonical && !high && afterHigh && prefix.count == 0) return false;
        afterHigh = high;
        text.remove_prefix(prefix.count + 3);
      }
      return true;
    }

    // ---- ToIntegerOrInfinity and friends -------------------------------------------------
    double toIntegerOrInfinity(double value) {
      if (std::isnan(value)) return 0;
      if (std::isinf(value)) return value;
      return std::trunc(value) + 0.0;
    }
    std::int64_t clampIndex(double value, std::int64_t low, std::int64_t high) {
      const double integer = toIntegerOrInfinity(value);
      if (integer <= static_cast<double>(low)) return low;
      if (integer >= static_cast<double>(high)) return high;
      return static_cast<std::int64_t>(integer);
    }
    // Relative index (slice/at): negative counts from the end, then clamp to [0, length].
    std::int64_t relativeIndex(double value, std::int64_t length) {
      const double integer = toIntegerOrInfinity(value);
      if (integer < 0) return std::max<std::int64_t>(0, static_cast<std::int64_t>(std::max(integer + static_cast<double>(length), -1.0)));
      return integer >= static_cast<double>(length) ? length : static_cast<std::int64_t>(integer);
    }

    // ---- UTF-16 index <-> byte offset for Shape::Utf8 and Shape::Ascii text ---------------
    // Byte offset of the code point boundary at or after UTF-16 index `index`, and whether
    // `index` falls between the two halves of a supplementary character.
    struct Locate {
      std::size_t byte;
      bool inside;
    };
    Locate locateUtf8(std::string_view text, std::int64_t index) {
      const Byte* begin = bytes(text);
      const Byte* p = begin;
      const Byte* end = begin + text.size();
      std::int64_t units = 0;
      while (p < end && units < index) {
        const Byte lead = *p;
        if (lead < 0x80) {
          ++p;
          ++units;
        } else if (lead < 0xE0) {
          p += 2;
          ++units;
        } else if (lead < 0xF0) {
          p += 3;
          ++units;
        } else {
          if (units + 1 == index) return {static_cast<std::size_t>(p - begin), true};
          p += 4;
          units += 2;
        }
      }
      return {static_cast<std::size_t>(p - begin), false};
    }
    std::int64_t unitsOfUtf8(const Byte* p, const Byte* end) {
      std::int64_t units = 0;
      for (; p < end; ++p) {
        const Byte value = *p;
        if ((value & 0xC0) != 0x80) units += value >= 0xF0 ? 2 : 1;
      }
      return units;
    }

    // Substring by UTF-16 code unit range [from, to) of any input, canonical output.
    std::string unitRange(std::string_view text, std::int64_t from, std::int64_t to) {
      if (from >= to) return {};
      const Byte* begin = bytes(text);
      const Byte* p = begin;
      const Byte* end = begin + text.size();
      // ASCII prefix: one byte per unit.
      const Byte* asciiEnd = p;
      while (asciiEnd < end && *asciiEnd < 0x80) ++asciiEnd;
      if (to <= asciiEnd - begin) return std::string(text.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from)));
      std::string out;
      std::int64_t unit = 0;
      if (from < asciiEnd - begin) {
        out.assign(text.data() + from, static_cast<std::size_t>(asciiEnd - begin - from));
      }
      unit = asciiEnd - begin;
      p = asciiEnd;
      while (p < end && unit < to) {
        if (*p < 0x80) {
          if (unit >= from) out.push_back(static_cast<char>(*p));
          ++p;
          ++unit;
          continue;
        }
        const Element element = decodePoint(p, end);
        if (element.value >= 0x10000) {
          const std::uint32_t offset = element.value - 0x10000;
          const auto high = static_cast<std::uint32_t>(0xD800 + (offset >> 10));
          const auto low = static_cast<std::uint32_t>(0xDC00 + (offset & 0x3FF));
          const bool takeHigh = unit >= from && unit < to;
          const bool takeLow = unit + 1 >= from && unit + 1 < to;
          if (takeHigh && takeLow) {
            if (element.verbatim) out.append(reinterpret_cast<const char*>(p), element.size);
            else encodeUtf8(out, element.value);
          } else if (takeHigh) {
            appendCodePoint(out, high);
          } else if (takeLow) {
            appendCodePoint(out, low);
          }
          unit += 2;
        } else {
          if (unit >= from) appendElement(out, p, element);
          ++unit;
        }
        p += element.size;
      }
      return out;
    }

    // ---- Whitespace ------------------------------------------------------------------
    constexpr bool isAsciiWhitespace(Byte value) { return value == 0x20 || (value >= 0x09 && value <= 0x0D); }

    // ICU accepts signed 32-bit lengths. Preserve WTF-8 through the UTF-16 boundary.
    std::int32_t icuLength(std::size_t length) {
      if (length > limits::kIcuInputBytes) {
        throw RangeError("String exceeds ICU length limit");
      }
      return static_cast<std::int32_t>(length);
    }

    using Utf8CaseMap = std::int32_t (*)(const char*, std::uint32_t, const char*, std::int32_t, char*, std::int32_t, icu::Edits*, UErrorCode&);

    std::string mapCase(std::string_view text, decltype(&u_strToUpper) mapUtf16, Utf8CaseMap mapUtf8) {
      if (text.empty()) return {};
      const auto inputLength = icuLength(text.size());
      UErrorCode status = U_ZERO_ERROR;
      if (isAscii(text)) {
        // Root casing of ASCII keeps the byte length unchanged.
        std::string result(text.size(), '\0');
        mapUtf8("", 0, text.data(), inputLength, result.data(), inputLength, nullptr, status);
        if (U_FAILURE(status)) throw JsError(u_errorName(status));
        return result;
      }

      const std::u16string source = toUtf16(text);
      const std::int32_t length = icuLength(source.size());
      std::array<char16_t, 64> buffer;
      const std::int32_t capacity = icuLength(buffer.size());
      const std::int32_t count = mapUtf16(buffer.data(), capacity, source.data(), length, "", &status);
      if (status == U_BUFFER_OVERFLOW_ERROR) {
        std::u16string expanded(static_cast<std::size_t>(count), u'\0');
        status = U_ZERO_ERROR;
        mapUtf16(expanded.data(), count, source.data(), length, "", &status);
        if (U_FAILURE(status)) throw JsError(u_errorName(status));
        return fromUtf16(expanded);
      }
      if (U_FAILURE(status)) throw JsError(u_errorName(status));
      return fromUtf16({buffer.data(), static_cast<std::size_t>(count)});
    }

    // ---- GetSubstitution for string patterns --------------------------------------------
    void appendSubstitution(std::string& out, std::u16string_view text, std::size_t position, std::size_t matchLength, std::u16string_view replacement) {
      std::u16string result;
      for (std::size_t index = 0; index < replacement.size(); ++index) {
        const char16_t unit = replacement[index];
        if (unit != u'$' || index + 1 >= replacement.size()) {
          result.push_back(unit);
          continue;
        }
        const char16_t next = replacement[index + 1];
        if (next == u'$') {
          result.push_back(u'$');
          ++index;
        } else if (next == u'&') {
          result.append(text.substr(position, matchLength));
          ++index;
        } else if (next == u'`') {
          result.append(text.substr(0, position));
          ++index;
        } else if (next == u'\'') {
          const std::size_t tail = std::min(position + matchLength, text.size());
          result.append(text.substr(tail));
          ++index;
        } else {
          result.push_back(unit);
        }
      }
      append(out, fromUtf16(result));
    }

    bool hasSubstitutionPattern(std::string_view replacement) { return replacement.find('$') != std::string_view::npos; }

    // ---- URI coding ----------------------------------------------------------------------
    constexpr std::string_view URI_UNRESERVED_MARKS = "-_.!~*'()";
    constexpr std::string_view URI_RESERVED = ";/?:@&=+$,#";
    constexpr char HEX_UPPER[] = "0123456789ABCDEF";

    bool isAsciiAlphanumeric(std::uint32_t value) { return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z'); }

    std::string encodeUri(std::string_view text, std::string_view extraUnescaped) {
      std::string out;
      out.reserve(text.size());
      const Byte* p = bytes(text);
      const Byte* end = p + text.size();
      while (p < end) {
        const Element element = decodePoint(p, end);
        const std::uint32_t value = element.value;
        if (value < 0x80 && (isAsciiAlphanumeric(value) || URI_UNRESERVED_MARKS.find(static_cast<char>(value)) != std::string_view::npos || extraUnescaped.find(static_cast<char>(value)) != std::string_view::npos)) {
          out.push_back(static_cast<char>(value));
        } else {
          if (isSurrogate(value)) throw URIError("String contained an illegal UTF-16 sequence.");
          std::string encoded;
          encodeUtf8(encoded, value);
          for (const char byte : encoded) {
            const auto octet = static_cast<Byte>(byte);
            out.push_back('%');
            out.push_back(HEX_UPPER[octet >> 4]);
            out.push_back(HEX_UPPER[octet & 0x0F]);
          }
        }
        p += element.size;
      }
      return out;
    }

    int hexValue(char16_t unit) {
      if (unit >= u'0' && unit <= u'9') return unit - u'0';
      if (unit >= u'a' && unit <= u'f') return unit - u'a' + 10;
      if (unit >= u'A' && unit <= u'F') return unit - u'A' + 10;
      return -1;
    }

    std::string decodeUri(std::string_view text, std::string_view reserved) {
      if (text.find('%') == std::string_view::npos) return canonicalWtf8(text);
      const std::u16string units = toUtf16(text);
      const std::size_t length = units.size();
      std::u16string out;
      out.reserve(length);
      const auto octetAt = [&](std::size_t index) -> int {
        if (units[index] != u'%') return -1;
        const int high = hexValue(units[index + 1]);
        const int low = hexValue(units[index + 2]);
        return high < 0 || low < 0 ? -1 : (high << 4) | low;
      };
      for (std::size_t k = 0; k < length; ++k) {
        const char16_t unit = units[k];
        if (unit != u'%') {
          out.push_back(unit);
          continue;
        }
        const std::size_t start = k;
        if (k + 2 >= length) throw URIError("URI error");
        const int first = octetAt(k);
        if (first < 0) throw URIError("URI error");
        k += 2;
        if ((first & 0x80) == 0) {
          const auto character = static_cast<char16_t>(first);
          if (reserved.find(static_cast<char>(first)) != std::string_view::npos) out.append(units, start, k - start + 1);
          else out.push_back(character);
          continue;
        }
        int count = 0;
        while (count < 8 && (first & (0x80 >> count))) ++count;
        if (count == 1 || count > 4) throw URIError("URI error");
        if (k + 3 * static_cast<std::size_t>(count - 1) >= length) throw URIError("URI error");
        std::uint32_t value = static_cast<std::uint32_t>(first) & (0xFFu >> (count + 1));
        for (int index = 1; index < count; ++index) {
          ++k;
          const int octet = octetAt(k);
          if (octet < 0) throw URIError("URI error");
          if ((octet & 0xC0) != 0x80) throw URIError("URI error");
          k += 2;
          value = (value << 6) | (static_cast<std::uint32_t>(octet) & 0x3F);
        }
        const std::uint32_t minimum = count == 2 ? 0x80 : count == 3 ? 0x800 : 0x10000;
        if (value < minimum || value > 0x10FFFF || isSurrogate(value)) throw URIError("URI error");
        if (value >= 0x10000) {
          out.push_back(static_cast<char16_t>(0xD800 + ((value - 0x10000) >> 10)));
          out.push_back(static_cast<char16_t>(0xDC00 + ((value - 0x10000) & 0x3FF)));
        } else {
          out.push_back(static_cast<char16_t>(value));
        }
      }
      return fromUtf16(out);
    }

    const UCollator* rootCollator() {
      // ICU permits concurrent comparisons on a collator that is only read.
      static const auto collator = [] {
        UErrorCode status = U_ZERO_ERROR;
        std::unique_ptr<UCollator, decltype(&ucol_close)> opened(ucol_open("root", &status), ucol_close);
        if (U_FAILURE(status)) throw JsError(u_errorName(status));
        ucol_setAttribute(opened.get(), UCOL_STRENGTH, UCOL_TERTIARY, &status);
        ucol_setAttribute(opened.get(), UCOL_NORMALIZATION_MODE, UCOL_ON, &status);
        if (U_FAILURE(status)) throw JsError(u_errorName(status));
        return opened;
      }();
      return collator.get();
    }
  }

  // ---- Encoding layer --------------------------------------------------------------------
  bool isAscii(std::string_view text) noexcept {
    return text.size() <= limits::kTextInputBytes && (text.empty() || simdutf::validate_ascii(text.data(), text.size()));
  }

  std::u16string toUtf16(std::string_view text) {
    if (text.size() > limits::kTextInputBytes) throw RangeError("String input exceeds limit");
    std::u16string units;
    units.reserve(text.size());
    while (!text.empty()) {
      const auto prefix = simdutf::validate_utf8_with_counts(text.data(), text.size());
      if (prefix.input_count != 0) {
        const auto start = units.size();
        units.resize(start + prefix.utf16_length());
        units.resize(start + simdutf::convert_valid_utf8_to_utf16(text.data(), prefix.input_count, units.data() + start));
        text.remove_prefix(prefix.input_count);
      }
      if (text.empty()) break;
      const Element element = decodeUnit(bytes(text), bytes(text) + text.size());
      // The valid prefix already handled every scalar value. This is a surrogate or U+FFFD.
      units.push_back(static_cast<char16_t>(element.value));
      text.remove_prefix(element.size);
    }
    return units;
  }

  std::string fromUtf16(std::u16string_view units) {
    if (units.size() > limits::kTextInputBytes) throw RangeError("String input exceeds limit");
    if (units.empty()) return {};
    std::string out;
    if (simdutf::validate_utf16(units.data(), units.size())) {
      out.resize(simdutf::utf8_length_from_utf16(units.data(), units.size()));
      out.resize(simdutf::convert_valid_utf16_to_utf8(units.data(), units.size(), out.data()));
    } else {
      out.reserve(units.size());
      for (const char16_t unit : units) appendCodePoint(out, unit);
    }
    return out;
  }

  void appendCodePoint(std::string& out, std::uint32_t codePoint) {
    if (isLowSurrogate(codePoint) && endsWithHighSurrogate(out)) {
      const std::uint32_t high = trailingHighSurrogate(out);
      out.resize(out.size() - 3);
      encodeUtf8(out, 0x10000 + ((high - 0xD800) << 10) + (codePoint - 0xDC00));
      return;
    }
    encodeUtf8(out, codePoint);
  }

  void append(std::string& out, std::string_view tail) {
    if (tail.size() >= 3 && static_cast<Byte>(tail[0]) == 0xED && static_cast<Byte>(tail[1]) >= 0xB0 && endsWithHighSurrogate(out)) {
      const Element low = decodeUnit(bytes(tail), bytes(tail) + tail.size());
      if (isLowSurrogate(low.value)) {
        appendCodePoint(out, low.value);
        out.append(tail.substr(3));
        return;
      }
    }
    out.append(tail);
  }

  std::string concat(std::string_view left, std::string_view right) {
    std::string out;
    out.reserve(left.size() + right.size());
    out.append(left);
    append(out, right);
    return out;
  }

  bool isWtf8(std::string_view text) noexcept { return validWtf8(text, false); }

  bool isCanonicalWtf8(std::string_view text) noexcept { return validWtf8(text, true); }

  std::string canonicalWtf8(std::string_view text) {
    if (text.size() > limits::kTextInputBytes) throw RangeError("String input exceeds limit");
    if (isCanonicalWtf8(text)) return std::string(text);
    std::string out;
    out.reserve(text.size());
    const Byte* p = bytes(text);
    const Byte* end = p + text.size();
    while (p < end) {
      const Element element = decodePoint(p, end);
      appendElement(out, p, element);
      p += element.size;
    }
    return out;
  }

  bool isWellFormed(std::string_view text) noexcept {
    const Byte* p = bytes(text);
    const Byte* end = p + text.size();
    while (p < end) {
      if (*p < 0x80) {
        ++p;
        continue;
      }
      const Element element = decodePoint(p, end);
      if (isSurrogate(element.value)) return false;
      p += element.size;
    }
    return true;
  }

  std::string toWellFormed(std::string_view text) {
    if (text.size() > limits::kTextInputBytes) throw RangeError("String input exceeds limit");
    std::string out;
    out.reserve(text.size());
    const Byte* p = bytes(text);
    const Byte* end = p + text.size();
    while (p < end) {
      const Element element = decodePoint(p, end);
      if (isSurrogate(element.value)) encodeUtf8(out, 0xFFFD);
      else appendElement(out, p, element);
      p += element.size;
    }
    return out;
  }

  // ---- Length, access, slicing ---------------------------------------------------------------
  std::int64_t length(std::string_view text) noexcept {
    if (text.empty()) return 0;
    // Keep the exact length for callers of this noexcept helper. For strings
    // beyond the conversion budget, count directly without handing them to SIMD.
    const Byte* p = bytes(text);
    const Byte* end = bytes(text) + text.size();
    std::int64_t units = 0;
    if (text.size() <= limits::kTextInputBytes) {
      const auto prefix = simdutf::validate_utf8_with_counts(text.data(), text.size());
      p += prefix.input_count;
      units = static_cast<std::int64_t>(prefix.utf16_length());
    }
    while (p < end) {
      const Element element = decodeUnit(p, end);
      units += element.value >= 0x10000 ? 2 : 1;
      p += element.size;
    }
    return units;
  }

  std::string charAt(std::string_view text, NumberArg position) {
    const double index = toIntegerOrInfinity(position.value);
    const std::int64_t size = length(text);
    if (index < 0 || index >= static_cast<double>(size)) return {};
    const auto unit = static_cast<std::int64_t>(index);
    return unitRange(text, unit, unit + 1);
  }

  double charCodeAt(std::string_view text, NumberArg position) {
    const double index = toIntegerOrInfinity(position.value);
    if (index < 0) return std::numeric_limits<double>::quiet_NaN();
    const std::string unit = charAt(text, index);
    if (unit.empty()) return std::numeric_limits<double>::quiet_NaN();
    return static_cast<double>(toUtf16(unit)[0]);
  }

  std::optional<std::string> at(std::string_view text, NumberArg index) {
    const std::int64_t size = length(text);
    const double relative = toIntegerOrInfinity(index.value);
    const double resolved = relative >= 0 ? relative : static_cast<double>(size) + relative;
    if (resolved < 0 || resolved >= static_cast<double>(size)) return std::nullopt;
    const auto unit = static_cast<std::int64_t>(resolved);
    return unitRange(text, unit, unit + 1);
  }

  std::string slice(std::string_view text, NumberArg start) {
    const std::int64_t size = length(text);
    return unitRange(text, relativeIndex(start.value, size), size);
  }

  std::string slice(std::string_view text, NumberArg start, NumberArg end) {
    const std::int64_t size = length(text);
    return unitRange(text, relativeIndex(start.value, size), relativeIndex(end.value, size));
  }

  std::string substring(std::string_view text, NumberArg start) {
    const std::int64_t size = length(text);
    return unitRange(text, clampIndex(start.value, 0, size), size);
  }

  std::string substring(std::string_view text, NumberArg start, NumberArg end) {
    const std::int64_t size = length(text);
    const std::int64_t from = clampIndex(start.value, 0, size);
    const std::int64_t to = clampIndex(end.value, 0, size);
    return unitRange(text, std::min(from, to), std::max(from, to));
  }

  std::string substr(std::string_view text, NumberArg start) {
    const std::int64_t size = length(text);
    return unitRange(text, relativeIndex(start.value, size), size);
  }

  std::string substr(std::string_view text, NumberArg start, NumberArg count) {
    const std::int64_t size = length(text);
    const std::int64_t from = relativeIndex(start.value, size);
    const std::int64_t span = clampIndex(count.value, 0, size - from);
    return unitRange(text, from, from + span);
  }

  // ---- Search ------------------------------------------------------------------------------
  std::int64_t indexOf(std::string_view text, std::string_view search, NumberArg position) {
    const Shape textShape = shapeOf(text);
    if (textShape == Shape::Ascii) {
      const auto from = static_cast<std::size_t>(clampIndex(position.value, 0, static_cast<std::int64_t>(text.size())));
      const std::size_t found = text.find(search, from);
      return found == std::string_view::npos ? -1 : static_cast<std::int64_t>(found);
    }
    if (textShape == Shape::Utf8 && shapeOf(search) != Shape::Other) {
      const std::int64_t size = length(text);
      const std::int64_t from = clampIndex(position.value, 0, size);
      if (search.empty()) return from;
      // A well-formed needle cannot begin between the halves of a supplementary character.
      const Locate start = locateUtf8(text, from);
      const std::size_t found = text.find(search, start.inside ? start.byte + 4 : start.byte);
      if (found == std::string_view::npos) return -1;
      return unitsOfUtf8(bytes(text), bytes(text) + found);
    }
    const std::u16string units = toUtf16(text);
    const auto from = static_cast<std::size_t>(clampIndex(position.value, 0, static_cast<std::int64_t>(units.size())));
    const std::size_t found = std::u16string_view(units).find(toUtf16(search), from);
    return found == std::u16string_view::npos ? -1 : static_cast<std::int64_t>(found);
  }

  std::int64_t lastIndexOf(std::string_view text, std::string_view search) { return lastIndexOf(text, search, std::numeric_limits<double>::infinity()); }

  std::int64_t lastIndexOf(std::string_view text, std::string_view search, NumberArg position) {
    const double requested = std::isnan(position.value) ? std::numeric_limits<double>::infinity() : position.value;
    if (isAscii(text)) {
      const auto from = static_cast<std::size_t>(clampIndex(requested, 0, static_cast<std::int64_t>(text.size())));
      const std::size_t found = text.rfind(search, from);
      return found == std::string_view::npos ? -1 : static_cast<std::int64_t>(found);
    }
    const std::u16string units = toUtf16(text);
    const auto from = static_cast<std::size_t>(clampIndex(requested, 0, static_cast<std::int64_t>(units.size())));
    const std::size_t found = std::u16string_view(units).rfind(toUtf16(search), from);
    return found == std::u16string_view::npos ? -1 : static_cast<std::int64_t>(found);
  }

  bool includes(std::string_view text, std::string_view search, NumberArg position) {
    if (position.value == 0 || std::isnan(position.value)) {
      const Shape searchShape = shapeOf(search);
      // Decoding malformed bytes or surrogates cannot consume or produce ASCII bytes.
      if (searchShape == Shape::Ascii || (searchShape == Shape::Utf8 && shapeOf(text) != Shape::Other)) return text.find(search) != std::string_view::npos;
    }
    return indexOf(text, search, position) >= 0;
  }

  bool startsWith(std::string_view text, std::string_view search, NumberArg position) {
    if (search.empty()) return true;
    if (position.value == 0 && isAscii(search)) return text.starts_with(search);
    const Shape textShape = shapeOf(text);
    if (textShape == Shape::Ascii) {
      const auto from = static_cast<std::size_t>(clampIndex(position.value, 0, static_cast<std::int64_t>(text.size())));
      return text.substr(from).starts_with(search);
    }
    if (textShape == Shape::Utf8 && shapeOf(search) != Shape::Other) {
      const Locate start = locateUtf8(text, clampIndex(position.value, 0, length(text)));
      return !start.inside && text.substr(start.byte).starts_with(search);
    }
    const std::u16string units = toUtf16(text);
    const auto from = static_cast<std::size_t>(clampIndex(position.value, 0, static_cast<std::int64_t>(units.size())));
    return std::u16string_view(units).substr(from).starts_with(toUtf16(search));
  }

  bool endsWith(std::string_view text, std::string_view search) {
    const Shape searchShape = shapeOf(search);
    if (searchShape == Shape::Ascii || (searchShape == Shape::Utf8 && shapeOf(text) != Shape::Other)) return text.ends_with(search);
    return endsWith(text, search, std::numeric_limits<double>::infinity());
  }

  bool endsWith(std::string_view text, std::string_view search, NumberArg endPosition) {
    const std::u16string units = toUtf16(text);
    const auto end = static_cast<std::size_t>(clampIndex(endPosition.value, 0, static_cast<std::int64_t>(units.size())));
    return std::u16string_view(units).substr(0, end).ends_with(toUtf16(search));
  }

  // ---- Transformations ---------------------------------------------------------------------
  namespace {
    std::string pad(std::string_view text, NumberArg maxLength, std::string_view fill, bool atStart) {
      const double requested = toIntegerOrInfinity(maxLength.value);
      const std::int64_t size = length(text);
      if (requested <= static_cast<double>(size) || fill.empty()) return canonicalWtf8(text);
      if (requested > MAX_STRING_LENGTH) throw RangeError("Out of memory");
      const auto fillLength = static_cast<std::size_t>(static_cast<std::int64_t>(requested) - size);
      const std::u16string fillUnits = toUtf16(fill);
      std::u16string filler;
      filler.reserve(fillLength);
      while (filler.size() < fillLength) filler.append(fillUnits, 0, std::min(fillUnits.size(), fillLength - filler.size()));
      std::string out;
      if (atStart) {
        out = fromUtf16(filler);
        append(out, canonicalWtf8(text));
      } else {
        out = canonicalWtf8(text);
        append(out, fromUtf16(filler));
      }
      return out;
    }
  }

  std::string padStart(std::string_view text, NumberArg maxLength, std::string_view fill) { return pad(text, maxLength, fill, true); }
  std::string padEnd(std::string_view text, NumberArg maxLength, std::string_view fill) { return pad(text, maxLength, fill, false); }

  std::string repeat(std::string_view text, NumberArg count) {
    const double times = toIntegerOrInfinity(count.value);
    if (times < 0 || std::isinf(times)) throw RangeError("String.prototype.repeat argument must be greater than or equal to 0 and not be Infinity");
    if (times == 0 || text.empty()) return {};
    const std::string unit = canonicalWtf8(text);
    if (static_cast<double>(length(unit)) * times > MAX_STRING_LENGTH) throw RangeError("Out of memory");
    const auto repetitions = static_cast<std::size_t>(times);
    std::string out;
    out.reserve(unit.size() * repetitions);
    for (std::size_t index = 0; index < repetitions; ++index) append(out, unit);
    return out;
  }

  bool isJsWhitespace(std::uint32_t value) noexcept {
    if (value < 0x80) return isAsciiWhitespace(static_cast<Byte>(value));
    return value == 0xA0 || value == 0x1680 || (value >= 0x2000 && value <= 0x200A) || value == 0x2028 || value == 0x2029 || value == 0x202F || value == 0x205F || value == 0x3000 || value == 0xFEFF;
  }

  namespace {
    std::string trimImpl(std::string_view text, bool leading) {
      const Byte* begin = bytes(text);
      const Byte* end = begin + text.size();
      const Byte* first = leading ? end : begin;
      const Byte* last = begin;
      for (const Byte* p = begin; p < end;) {
        const Element element = decodeUnit(p, end);
        if (!isJsWhitespace(element.value)) {
          if (first == end) first = p;
          last = p + element.size;
        }
        p += element.size;
      }
      if (first >= last) return {};
      return canonicalWtf8(text.substr(static_cast<std::size_t>(first - begin), static_cast<std::size_t>(last - first)));
    }
  }

  std::string trim(std::string_view text) { return trimImpl(text, true); }
  std::string trimEnd(std::string_view text) { return trimImpl(text, false); }

  std::vector<std::string> split(std::string_view text) { return {canonicalWtf8(text)}; }

  std::vector<std::string> split(std::string_view text, std::string_view separator) { return split(text, separator, 4294967295.0); }

  std::vector<std::string> split(std::string_view text, std::string_view separator, NumberArg limit) {
    // ToUint32(limit)
    double integer = toIntegerOrInfinity(limit.value);
    if (std::isinf(integer)) integer = 0;
    double wrapped = std::fmod(integer, 4294967296.0);
    if (wrapped < 0) wrapped += 4294967296.0;
    const auto maxCount = static_cast<std::uint64_t>(wrapped);
    std::vector<std::string> parts;
    if (maxCount == 0) return parts;
    if (separator.empty()) {
      const std::u16string units = toUtf16(text);
      const std::size_t count = std::min<std::uint64_t>(units.size(), maxCount);
      parts.reserve(count);
      for (std::size_t index = 0; index < count; ++index) parts.push_back(fromUtf16(std::u16string_view(units).substr(index, 1)));
      return parts;
    }
    if (text.empty()) {
      parts.emplace_back();
      return parts;
    }
    const Shape textShape = shapeOf(text);
    const Shape separatorShape = shapeOf(separator);
    if (textShape != Shape::Other && separatorShape != Shape::Other) {
      std::size_t from = 0;
      while (true) {
        const std::size_t found = text.find(separator, from);
        if (found == std::string_view::npos) break;
        parts.emplace_back(text.substr(from, found - from));
        if (parts.size() == maxCount) return parts;
        from = found + separator.size();
      }
      parts.emplace_back(text.substr(from));
      return parts;
    }
    const std::u16string units = toUtf16(text);
    const std::u16string needle = toUtf16(separator);
    const std::u16string_view view(units);
    std::size_t from = 0;
    while (true) {
      const std::size_t found = view.find(needle, from);
      if (found == std::u16string_view::npos) break;
      parts.push_back(fromUtf16(view.substr(from, found - from)));
      if (parts.size() == maxCount) return parts;
      from = found + needle.size();
    }
    parts.push_back(fromUtf16(view.substr(from)));
    return parts;
  }

  std::string replace(std::string_view text, std::string_view pattern, std::string_view replacement) {
    const Shape textShape = shapeOf(text);
    const Shape patternShape = shapeOf(pattern);
    if (textShape != Shape::Other && patternShape != Shape::Other && !hasSubstitutionPattern(replacement)) {
      const std::size_t found = text.find(pattern);
      if (found == std::string_view::npos) return std::string(text);
      std::string out(text.substr(0, found));
      append(out, canonicalWtf8(replacement));
      append(out, text.substr(found + pattern.size()));
      return out;
    }
    const std::u16string units = toUtf16(text);
    const std::u16string needle = toUtf16(pattern);
    const std::u16string_view view(units);
    const std::size_t found = view.find(needle);
    if (found == std::u16string_view::npos) return fromUtf16(view);
    std::string out = fromUtf16(view.substr(0, found));
    appendSubstitution(out, view, found, needle.size(), toUtf16(replacement));
    append(out, fromUtf16(view.substr(found + needle.size())));
    return out;
  }

  std::string replaceAll(std::string_view text, std::string_view pattern, std::string_view replacement) {
    const Shape textShape = shapeOf(text);
    const Shape patternShape = shapeOf(pattern);
    if (textShape != Shape::Other && patternShape != Shape::Other && !pattern.empty() && !hasSubstitutionPattern(replacement)) {
      const std::string replacementText = canonicalWtf8(replacement);
      std::string out;
      std::size_t from = 0;
      while (true) {
        const std::size_t found = text.find(pattern, from);
        if (found == std::string_view::npos) break;
        append(out, text.substr(from, found - from));
        append(out, replacementText);
        from = found + pattern.size();
      }
      append(out, text.substr(from));
      return out;
    }
    const std::u16string units = toUtf16(text);
    const std::u16string needle = toUtf16(pattern);
    const std::u16string replacementUnits = toUtf16(replacement);
    const std::u16string_view view(units);
    const std::size_t advance = std::max<std::size_t>(1, needle.size());
    std::string out;
    std::size_t endOfLastMatch = 0;
    for (std::size_t match = view.find(needle); match != std::u16string_view::npos; match = view.find(needle, match + advance)) {
      append(out, fromUtf16(view.substr(endOfLastMatch, match - endOfLastMatch)));
      appendSubstitution(out, view, match, needle.size(), replacementUnits);
      endOfLastMatch = match + needle.size();
    }
    if (endOfLastMatch < view.size()) append(out, fromUtf16(view.substr(endOfLastMatch)));
    return out;
  }

  std::string toLowerCase(std::string_view text) { return mapCase(text, u_strToLower, icu::CaseMap::utf8ToLower); }

  std::string toUpperCase(std::string_view text) { return mapCase(text, u_strToUpper, icu::CaseMap::utf8ToUpper); }

  // ---- Construction ------------------------------------------------------------------------
  std::string fromCharCode(std::uint16_t unit) {
    std::string out;
    encodeUtf8(out, unit);
    return out;
  }

  std::string fromCharCode(std::span<const std::uint16_t> units) { return fromUtf16(std::u16string_view(reinterpret_cast<const char16_t*>(units.data()), units.size())); }

  std::string fromCharCode(std::span<const std::uint8_t> bytes_) {
    std::string out;
    out.reserve(bytes_.size());
    for (const std::uint8_t byte : bytes_) encodeUtf8(out, byte);
    return out;
  }

  std::string fromCodePoint(NumberArg codePoint) {
    const double value = codePoint.value;
    if (!(value >= 0 && value <= 0x10FFFF) || std::trunc(value) != value) throw RangeError("Arguments contain a value that is out of range of code points");
    std::string out;
    encodeUtf8(out, static_cast<std::uint32_t>(value));
    return out;
  }

  std::string fromCodePoint(std::span<const std::uint32_t> codePoints_) {
    std::string out;
    for (const std::uint32_t value : codePoints_) {
      if (value > 0x10FFFF) throw RangeError("Arguments contain a value that is out of range of code points");
      appendCodePoint(out, value);
    }
    return out;
  }

  // ---- Comparison ------------------------------------------------------------------------
  int compare(std::string_view left, std::string_view right) {
    const auto plain = [](std::string_view text) {
      for (const char byte : text) {
        if (static_cast<Byte>(byte) >= 0xED) return false;
      }
      return isWtf8(text);
    };
    if (plain(left) && plain(right)) {
      const int order = left.compare(right);
      return order < 0 ? -1 : order > 0 ? 1 : 0;
    }
    const std::u16string a = toUtf16(left);
    const std::u16string b = toUtf16(right);
    const int order = std::u16string_view(a).compare(b);
    return order < 0 ? -1 : order > 0 ? 1 : 0;
  }

  bool lessThan(std::string_view left, std::string_view right) { return compare(left, right) < 0; }

  int localeCompare(std::string_view left, std::string_view right) {
    if (left.size() > limits::kIcuInputBytes || right.size() > limits::kIcuInputBytes) throw RangeError("String exceeds ICU length limit");
    const std::u16string a = toUtf16(left);
    const std::u16string b = toUtf16(right);
    return ucol_strcoll(rootCollator(), a.data(), icuLength(a.size()), b.data(), icuLength(b.size()));
  }

  // ---- URI coding ------------------------------------------------------------------------
  std::string encodeURIComponent(std::string_view text) { return encodeUri(text, {}); }
  std::string encodeURI(std::string_view text) { return encodeUri(text, URI_RESERVED); }
  std::string decodeURIComponent(std::string_view text) { return decodeUri(text, {}); }
  std::string decodeURI(std::string_view text) { return decodeUri(text, URI_RESERVED); }

  // ---- Iteration ---------------------------------------------------------------------------
  void CodePoints::iterator::decode() {
    if (position_ >= end_) {
      size_ = 0;
      value_ = 0;
      return;
    }
    const auto* p = reinterpret_cast<const Byte*>(position_);
    if (*p < 0x80) {
      size_ = 1;
      value_ = *p;
      return;
    }
    const Element element = decodePoint(p, reinterpret_cast<const Byte*>(end_));
    size_ = element.size;
    value_ = element.value;
  }

  std::vector<std::string> codePointStrings(std::string_view text) {
    std::vector<std::string> out;
    const Byte* p = bytes(text);
    const Byte* end = p + text.size();
    while (p < end) {
      const Element element = decodePoint(p, end);
      std::string item;
      appendElement(item, p, element);
      out.push_back(std::move(item));
      p += element.size;
    }
    return out;
  }
}
