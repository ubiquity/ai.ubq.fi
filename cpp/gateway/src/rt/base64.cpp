#include "uos/rt/base64.hpp"

#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include "third_party/simdutf/simdutf.h"

#include <limits>
#include <stdexcept>

namespace uos::rt {
  namespace {
    using Byte = std::uint8_t;

    constexpr char STANDARD_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    constexpr char URL_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    constexpr char HEX_DIGITS[] = "0123456789abcdef";

    // The forgiving-base64 alphabet (atob): -1 outside it.
    int standardSextet(Byte value) {
      if (value >= 'A' && value <= 'Z') return value - 'A';
      if (value >= 'a' && value <= 'z') return value - 'a' + 26;
      if (value >= '0' && value <= '9') return value - '0' + 52;
      if (value == '+') return 62;
      if (value == '/') return 63;
      return -1;
    }
    // Buffer's decoder accepts both alphabets.
    int anySextet(Byte value) {
      if (value == '-') return 62;
      if (value == '_') return 63;
      return standardSextet(value);
    }
    int hexValue(Byte value) {
      if (value >= '0' && value <= '9') return value - '0';
      if (value >= 'a' && value <= 'f') return value - 'a' + 10;
      if (value >= 'A' && value <= 'F') return value - 'A' + 10;
      return -1;
    }
    bool isAsciiWhitespace(Byte value) { return value == 0x09 || value == 0x0A || value == 0x0C || value == 0x0D || value == 0x20; }

    std::string encode(std::span<const Byte> bytes, simdutf::base64_options options) {
      if (bytes.empty()) return {};
      if (bytes.size() < 3) {
        const bool padded = options == simdutf::base64_default;
        const char* alphabet = padded ? STANDARD_ALPHABET : URL_ALPHABET;
        const auto first = static_cast<unsigned>(bytes[0]);
        const auto second = bytes.size() == 2 ? static_cast<unsigned>(bytes[1]) : 0;
        const char tail[] = {
          alphabet[first >> 2],
          alphabet[((first & 3) << 4) | (second >> 4)],
          bytes.size() == 2 ? alphabet[(second & 15) << 2] : '=',
          '=',
        };
        return {tail, padded ? sizeof(tail) : bytes.size() + 1};
      }
      // The upstream encoded length is calculated in size_t before std::string checks its capacity.
      if (bytes.size() > std::numeric_limits<std::size_t>::max() / 4 * 3) throw std::length_error("Base64 output is too large");
      std::string out(simdutf::base64_length_from_binary(bytes.size(), options), '\0');
      simdutf::binary_to_base64(reinterpret_cast<const char*>(bytes.data()), bytes.size(), out.data(), options);
      return out;
    }

    // The low byte of every UTF-16 code unit of a JS string (Buffer's one-byte view).
    std::vector<Byte> lowBytes(std::string_view text) {
      if (isAscii(text)) return {reinterpret_cast<const Byte*>(text.data()), reinterpret_cast<const Byte*>(text.data()) + text.size()};
      const std::u16string units = toUtf16(text);
      std::vector<Byte> out;
      out.reserve(units.size());
      for (const char16_t unit : units) out.push_back(static_cast<Byte>(unit & 0xFF));
      return out;
    }

    // The bytes of a binary string; false when a code unit is above U+00FF.
    bool binaryBytes(std::string_view data, std::vector<Byte>& out) {
      out.reserve(data.size());
      const auto* p = reinterpret_cast<const Byte*>(data.data());
      const Byte* end = p + data.size();
      while (p < end) {
        if (*p < 0x80) {
          out.push_back(*p++);
          continue;
        }
        if ((p[0] == 0xC2 || p[0] == 0xC3) && p + 1 < end && (p[1] & 0xC0) == 0x80) {
          out.push_back(static_cast<Byte>(((p[0] & 0x1F) << 6) | (p[1] & 0x3F)));
          p += 2;
          continue;
        }
        return false;
      }
      return true;
    }

    std::vector<Byte> forgivingBase64(std::string_view data) {
      std::string cleaned;
      cleaned.reserve(data.size());
      for (const char value : data) {
        if (!isAsciiWhitespace(static_cast<Byte>(value))) cleaned.push_back(value);
      }
      if (cleaned.size() % 4 == 0) {
        if (cleaned.size() >= 2 && cleaned.compare(cleaned.size() - 2, 2, "==") == 0) cleaned.resize(cleaned.size() - 2);
        else if (!cleaned.empty() && cleaned.back() == '=') cleaned.pop_back();
      }
      if (cleaned.size() % 4 == 1) throw InvalidCharacterError();
      std::vector<Byte> out;
      out.reserve(cleaned.size() / 4 * 3 + 2);
      std::uint32_t buffer = 0;
      int bits = 0;
      for (const char value : cleaned) {
        const int sextet = standardSextet(static_cast<Byte>(value));
        if (sextet < 0) throw InvalidCharacterError();
        buffer = (buffer << 6) | static_cast<std::uint32_t>(sextet);
        bits += 6;
        if (bits >= 8) {
          bits -= 8;
          out.push_back(static_cast<Byte>((buffer >> bits) & 0xFF));
        }
      }
      return out;
    }

    std::string lowered(std::string_view name) {
      std::string out(name);
      for (char& value : out) {
        if (value >= 'A' && value <= 'Z') value = static_cast<char>(value + 32);
      }
      return out;
    }
  }

  std::string btoa(std::string_view data) {
    std::vector<Byte> bytes;
    if (!binaryBytes(data, bytes)) throw InvalidCharacterError();
    return toBase64(bytes);
  }

  std::string atob(std::string_view data) { return binaryString(forgivingBase64(data)); }

  std::vector<std::uint8_t> atobBytes(std::string_view data) { return forgivingBase64(data); }

  std::string binaryString(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (const Byte value : bytes) {
      if (value < 0x80) {
        out.push_back(static_cast<char>(value));
      } else {
        out.push_back(static_cast<char>(0xC0 | (value >> 6)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
      }
    }
    return out;
  }

  std::string toBase64(std::span<const std::uint8_t> bytes) { return encode(bytes, simdutf::base64_default); }

  std::string toBase64Url(std::span<const std::uint8_t> bytes) { return encode(bytes, simdutf::base64_url); }

  std::string toHex(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const Byte value : bytes) {
      out.push_back(HEX_DIGITS[value >> 4]);
      out.push_back(HEX_DIGITS[value & 0x0F]);
    }
    return out;
  }

  std::vector<std::uint8_t> fromBase64(std::string_view text) {
    const std::vector<Byte> units = lowBytes(text);
    std::vector<Byte> out;
    out.reserve(units.size() / 4 * 3 + 2);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const Byte unit : units) {
      if (unit == '=') break;
      const int sextet = anySextet(unit);
      if (sextet < 0) continue;
      buffer = (buffer << 6) | static_cast<std::uint32_t>(sextet);
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        out.push_back(static_cast<Byte>((buffer >> bits) & 0xFF));
      }
    }
    return out;
  }

  std::vector<std::uint8_t> fromHex(std::string_view text) {
    const std::vector<Byte> units = lowBytes(text);
    std::vector<Byte> out;
    out.reserve(units.size() / 2);
    for (std::size_t index = 0; index + 1 < units.size(); index += 2) {
      const int high = hexValue(units[index]);
      const int low = hexValue(units[index + 1]);
      if (high < 0 || low < 0) break;
      out.push_back(static_cast<Byte>((high << 4) | low));
    }
    return out;
  }

  BufferEncoding bufferEncoding(std::string_view name) {
    const std::string key = lowered(name);
    if (key == "utf8" || key == "utf-8") return BufferEncoding::utf8;
    if (key == "ucs2" || key == "ucs-2" || key == "utf16le" || key == "utf-16le") return BufferEncoding::utf16le;
    if (key == "latin1" || key == "binary") return BufferEncoding::latin1;
    if (key == "ascii") return BufferEncoding::ascii;
    if (key == "base64") return BufferEncoding::base64;
    if (key == "base64url") return BufferEncoding::base64url;
    if (key == "hex") return BufferEncoding::hex;
    throw TypeError("Unknown encoding: " + std::string(name));
  }

  std::vector<std::uint8_t> bufferFrom(std::string_view text, BufferEncoding encoding) {
    switch (encoding) {
      case BufferEncoding::utf8:
        return encodeUtf8(text);
      case BufferEncoding::utf16le: {
        const std::u16string units = toUtf16(text);
        std::vector<Byte> out;
        out.reserve(units.size() * 2);
        for (const char16_t unit : units) {
          out.push_back(static_cast<Byte>(unit & 0xFF));
          out.push_back(static_cast<Byte>(unit >> 8));
        }
        return out;
      }
      case BufferEncoding::latin1:
      case BufferEncoding::ascii:
        return lowBytes(text);
      case BufferEncoding::base64:
      case BufferEncoding::base64url:
        return fromBase64(text);
      case BufferEncoding::hex:
        return fromHex(text);
    }
    return {};
  }

  std::vector<std::uint8_t> bufferFrom(std::string_view text, std::string_view encoding) { return bufferFrom(text, bufferEncoding(encoding)); }

  std::string bufferToString(std::span<const std::uint8_t> bytes, BufferEncoding encoding) {
    switch (encoding) {
      case BufferEncoding::utf8:
        return decodeUtf8(bytes, {.ignoreBOM = true});
      case BufferEncoding::utf16le: {
        std::u16string units;
        units.reserve(bytes.size() / 2);
        for (std::size_t index = 0; index + 1 < bytes.size(); index += 2) units.push_back(static_cast<char16_t>(bytes[index] | (bytes[index + 1] << 8)));
        return fromUtf16(units);
      }
      case BufferEncoding::latin1:
        return binaryString(bytes);
      case BufferEncoding::ascii: {
        std::string out;
        out.reserve(bytes.size());
        for (const Byte value : bytes) out.push_back(static_cast<char>(value & 0x7F));
        return out;
      }
      case BufferEncoding::base64:
        return toBase64(bytes);
      case BufferEncoding::base64url:
        return toBase64Url(bytes);
      case BufferEncoding::hex:
        return toHex(bytes);
    }
    return {};
  }

  std::string bufferToString(std::span<const std::uint8_t> bytes, std::string_view encoding) { return bufferToString(bytes, bufferEncoding(encoding)); }
}
