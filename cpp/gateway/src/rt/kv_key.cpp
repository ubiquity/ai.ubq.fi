// Key parts, the tuple key codec (kv_key.js with the repository's number patch),
// versionstamps and list cursors (kv_util.js).
#include "kv_internal.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/limits.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace uos::rt {
  namespace kv_internal {
    namespace {
      void appendUtf8(std::string& out, std::uint32_t codePoint) {
        if (codePoint < 0x80) {
          out += static_cast<char>(codePoint);
        } else if (codePoint < 0x800) {
          out += static_cast<char>(0xC0 | (codePoint >> 6));
          out += static_cast<char>(0x80 | (codePoint & 0x3F));
        } else if (codePoint < 0x10000) {
          out += static_cast<char>(0xE0 | (codePoint >> 12));
          out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
          out += static_cast<char>(0x80 | (codePoint & 0x3F));
        } else {
          out += static_cast<char>(0xF0 | (codePoint >> 18));
          out += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
          out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
          out += static_cast<char>(0x80 | (codePoint & 0x3F));
        }
      }

      bool isAscii(std::string_view text) {
        return std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
      }

      [[noreturn]] void throwInvalidByte(std::uint8_t byte) {
        std::string text = "Invalid byte '";
        appendUtf8(text, byte);
        throw TypeError(text + "'");
      }

      std::uint8_t fromHexChar(std::uint8_t byte) {
        if (byte >= '0' && byte <= '9') return static_cast<std::uint8_t>(byte - '0');
        if (byte >= 'a' && byte <= 'f') return static_cast<std::uint8_t>(byte - 'a' + 10);
        if (byte >= 'A' && byte <= 'F') return static_cast<std::uint8_t>(byte - 'A' + 10);
        throwInvalidByte(byte);
      }
    }

    std::u16string utf16Units(std::string_view text) {
      std::u16string units;
      units.reserve(text.size());
      std::size_t index = 0;
      while (index < text.size()) {
        const auto lead = static_cast<std::uint8_t>(text[index]);
        if (lead < 0x80) {
          units += static_cast<char16_t>(lead);
          ++index;
          continue;
        }
        std::size_t length = 0;
        std::uint8_t lower = 0x80, upper = 0xBF;
        std::uint32_t codePoint = 0;
        if (lead >= 0xC2 && lead <= 0xDF) {
          length = 2;
          codePoint = lead & 0x1F;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
          length = 3;
          codePoint = lead & 0x0F;
          if (lead == 0xE0) lower = 0xA0;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
          length = 4;
          codePoint = lead & 0x07;
          if (lead == 0xF0) lower = 0x90;
          if (lead == 0xF4) upper = 0x8F;
        } else {
          units += u'�';
          ++index;
          continue;
        }
        std::size_t consumed = 1;
        bool valid = true;
        for (; consumed < length; ++consumed) {
          if (index + consumed >= text.size()) {
            valid = false;
            break;
          }
          const auto byte = static_cast<std::uint8_t>(text[index + consumed]);
          const std::uint8_t low = consumed == 1 ? lower : std::uint8_t{0x80};
          const std::uint8_t high = consumed == 1 ? upper : std::uint8_t{0xBF};
          if (byte < low || byte > high) {
            valid = false;
            break;
          }
          codePoint = (codePoint << 6) | (byte & 0x3F);
        }
        index += consumed;
        if (!valid) {
          units += u'�';
          continue;
        }
        if (codePoint >= 0x10000) {
          codePoint -= 0x10000;
          units += static_cast<char16_t>(0xD800 + (codePoint >> 10));
          units += static_cast<char16_t>(0xDC00 + (codePoint & 0x3FF));
        } else {
          units += static_cast<char16_t>(codePoint);
        }
      }
      return units;
    }

    std::string textEncode(std::string_view text) {
      if (isAscii(text)) return std::string(text);
      const std::u16string units = utf16Units(text);
      std::string out;
      out.reserve(text.size());
      for (std::size_t index = 0; index < units.size(); ++index) {
        const std::uint32_t unit = units[index];
        if (unit >= 0xD800 && unit <= 0xDBFF && index + 1 < units.size() && units[index + 1] >= 0xDC00 && units[index + 1] <= 0xDFFF) {
          appendUtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (units[index + 1] - 0xDC00u));
          ++index;
        } else if (unit >= 0xD800 && unit <= 0xDFFF) {
          appendUtf8(out, 0xFFFD);
        } else {
          appendUtf8(out, unit);
        }
      }
      return out;
    }

    std::string textDecode(std::span<const std::uint8_t> bytes) {
      std::string out;
      out.reserve(bytes.size());
      std::uint32_t codePoint = 0;
      int needed = 0, seen = 0;
      std::uint8_t lower = 0x80, upper = 0xBF;
      bool first = true;
      const auto emit = [&](std::uint32_t value) {
        if (first) {
          first = false;
          if (value == 0xFEFF) return;
        }
        appendUtf8(out, value);
      };
      std::size_t index = 0;
      while (index < bytes.size()) {
        const std::uint8_t byte = bytes[index];
        if (needed == 0) {
          ++index;
          if (byte <= 0x7F) {
            emit(byte);
          } else if (byte >= 0xC2 && byte <= 0xDF) {
            needed = 1;
            codePoint = byte & 0x1F;
          } else if (byte >= 0xE0 && byte <= 0xEF) {
            if (byte == 0xE0) lower = 0xA0;
            if (byte == 0xED) upper = 0x9F;
            needed = 2;
            codePoint = byte & 0x0F;
          } else if (byte >= 0xF0 && byte <= 0xF4) {
            if (byte == 0xF0) lower = 0x90;
            if (byte == 0xF4) upper = 0x8F;
            needed = 3;
            codePoint = byte & 0x07;
          } else {
            emit(0xFFFD);
          }
          continue;
        }
        if (byte < lower || byte > upper) {
          codePoint = 0;
          needed = seen = 0;
          lower = 0x80;
          upper = 0xBF;
          emit(0xFFFD);
          continue;
        }
        ++index;
        lower = 0x80;
        upper = 0xBF;
        codePoint = (codePoint << 6) | (byte & 0x3F);
        if (++seen == needed) {
          emit(codePoint);
          codePoint = 0;
          needed = seen = 0;
        }
      }
      if (needed != 0) emit(0xFFFD);
      return out;
    }

    std::string encodeHex(std::span<const std::uint8_t> bytes) {
      static constexpr char digits[] = "0123456789abcdef";
      std::string out;
      out.reserve(bytes.size() * 2);
      for (const std::uint8_t byte : bytes) {
        out += digits[byte >> 4];
        out += digits[byte & 0x0F];
      }
      return out;
    }

    KvBytes decodeHex(std::string_view text) {
      const std::string encoded = textEncode(text);
      const auto* u8 = reinterpret_cast<const std::uint8_t*>(encoded.data());
      KvBytes out(encoded.size() / 2);
      for (std::size_t index = 0; index < out.size(); ++index) {
        const std::uint8_t a = fromHexChar(u8[index * 2]);
        const std::uint8_t b = fromHexChar(u8[index * 2 + 1]);
        out[index] = static_cast<std::uint8_t>((a << 4) | b);
      }
      if (encoded.size() % 2 == 1) {
        fromHexChar(u8[out.size() * 2]);
        throw RangeError("Odd length hex string");
      }
      return out;
    }

    KvBytes bigintMagnitude(const BigInt& value) {
      if (value.isZero()) return {};
      std::string hex = (value.isNegative() ? -value : value).toString(16);
      if (hex.size() % 2 == 1) hex.insert(hex.begin(), '0');
      KvBytes out(hex.size() / 2);
      for (std::size_t index = 0; index < out.size(); ++index)
        out[index] = static_cast<std::uint8_t>((fromHexChar(static_cast<std::uint8_t>(hex[index * 2])) << 4) | fromHexChar(static_cast<std::uint8_t>(hex[index * 2 + 1])));
      return out;
    }

    BigInt bigintFromMagnitude(std::span<const std::uint8_t> magnitude, bool negative) {
      if (magnitude.empty()) return BigInt(0);
      BigInt value = BigInt::fromString("0x" + encodeHex(magnitude));
      return negative ? -value : value;
    }
  }

  using kv_internal::encodeHex;

  namespace {
    enum Typecode : std::uint8_t {
      ByteString = 1,
      UnicodeString = 2,
      IntegerArbitraryByteNegative = 11,
      IntegerOneByteNegative = 19,
      IntegerZero = 20,
      IntegerOneBytePositive = 21,
      IntegerArbitraryBytePositive = 29,
      FloatingPointDouble = 33,
      False = 38,
      True = 39,
    };

    std::string_view partKind(std::size_t index) {
      static constexpr std::string_view names[] = {"Uint8Array", "string", "number", "bigint", "boolean"};
      return names[index];
    }

    [[noreturn]] void throwPartMismatch(const KvKeyPart& part, std::size_t expected) {
      throw TypeError("KV key part is " + std::string(partKind(part.value.index())) + ", not " + std::string(partKind(expected)));
    }

    void appendEscaped(KvBytes& out, std::span<const std::uint8_t> bytes) {
      for (const std::uint8_t byte : bytes) {
        out.push_back(byte);
        if (byte == 0) out.push_back(0xFF);
      }
    }

    std::string joinBytes(std::span<const std::uint8_t> bytes, std::string_view separator) {
      std::string out;
      for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index) out += separator;
        out += std::to_string(bytes[index]);
      }
      return out;
    }

    // `BigInt(undefined)` / `BigInt(NaN)` for a byte read past the end of a key.
    [[noreturn]] void throwTruncatedBigInt(bool negative) {
      if (negative) throw RangeError("Not an integer");
      throw TypeError("Invalid argument type in ToBigInt operation");
    }

  }

  const KvBytes& KvKeyPart::bytes() const {
    if (const auto* found = std::get_if<KvBytes>(&value)) return *found;
    throwPartMismatch(*this, 0);
  }
  const std::string& KvKeyPart::string() const {
    if (const auto* found = std::get_if<std::string>(&value)) return *found;
    throwPartMismatch(*this, 1);
  }
  double KvKeyPart::number() const {
    if (const auto* found = std::get_if<double>(&value)) return *found;
    throwPartMismatch(*this, 2);
  }
  const BigInt& KvKeyPart::bigint() const {
    if (const auto* found = std::get_if<BigInt>(&value)) return *found;
    throwPartMismatch(*this, 3);
  }
  bool KvKeyPart::boolean() const {
    if (const auto* found = std::get_if<bool>(&value)) return *found;
    throwPartMismatch(*this, 4);
  }

  std::string KvKeyPart::toString() const {
    switch (value.index()) {
      case 0: return joinBytes(std::get<KvBytes>(value), ",");
      case 1: return std::get<std::string>(value);
      case 2: return numberToString(std::get<double>(value));
      case 3: return std::get<BigInt>(value).toString();
      default: return std::get<bool>(value) ? "true" : "false";
    }
  }

  bool operator==(const KvKeyPart& left, const KvKeyPart& right) { return packKeyPart(left) == packKeyPart(right); }

  std::strong_ordering operator<=>(const KvKeyPart& left, const KvKeyPart& right) {
    const KvBytes a = packKeyPart(left), b = packKeyPart(right);
    return std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(), b.end());
  }

  KvBytes packKeyPart(const KvKeyPart& part) {
    KvBytes out;
    switch (part.value.index()) {
      case 0: {
        out.push_back(ByteString);
        appendEscaped(out, std::get<KvBytes>(part.value));
        out.push_back(0);
        return out;
      }
      case 1: {
        const std::string encoded = kv_internal::textEncode(std::get<std::string>(part.value));
        out.push_back(UnicodeString);
        appendEscaped(out, std::span(reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size()));
        out.push_back(0);
        return out;
      }
      case 2: {
        const double number = std::get<double>(part.value);
        std::uint64_t bits = std::isnan(number) ? 0x7FF8000000000000ull : std::bit_cast<std::uint64_t>(number);
        bits = (bits & 0x8000000000000000ull) ? ~bits : bits ^ 0x8000000000000000ull;
        out.push_back(FloatingPointDouble);
        for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(bits >> shift));
        return out;
      }
      case 3: {
        const BigInt& bigint = std::get<BigInt>(part.value);
        const bool negative = bigint.isNegative();
        const KvBytes magnitude = kv_internal::bigintMagnitude(bigint);
        const std::size_t count = magnitude.size();
        std::uint8_t typecode;
        if (negative)
          typecode = count <= 8 ? static_cast<std::uint8_t>(IntegerOneByteNegative - count + 1) : std::uint8_t{IntegerArbitraryByteNegative};
        else
          typecode = count <= 8 ? static_cast<std::uint8_t>(IntegerOneBytePositive + count - 1) : std::uint8_t{IntegerArbitraryBytePositive};
        out.push_back(typecode);
        // Like the SDK's Uint8Array conversion, a length above 255 keeps its low byte.
        if (count > 8) out.push_back(static_cast<std::uint8_t>(count & 0xFF));
        out.insert(out.end(), magnitude.begin(), magnitude.end());
        if (negative)
          for (std::size_t index = 1; index < out.size(); ++index) out[index] = static_cast<std::uint8_t>(0xFF - out[index]);
        return out;
      }
      default:
        out.push_back(std::get<bool>(part.value) ? True : False);
        return out;
    }
  }

  KvBytes packKey(const KvKey& key) {
    KvBytes out;
    for (const KvKeyPart& part : key) {
      const KvBytes packed = packKeyPart(part);
      out.insert(out.end(), packed.begin(), packed.end());
    }
    return out;
  }

  KvKey unpackKey(std::span<const std::uint8_t> bytes) {
    KvKey parts;
    std::size_t position = 0;
    const auto partsText = [&] {
      std::string text;
      for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index) text += ", ";
        text += parts[index].toString();
      }
      return text;
    };
    while (position < bytes.size()) {
      const std::uint8_t typecode = bytes[position++];
      if (typecode == ByteString || typecode == UnicodeString) {
        KvBytes collected;
        while (position < bytes.size()) {
          const std::uint8_t byte = bytes[position++];
          if (byte == 0 && position < bytes.size() && bytes[position] == 0xFF) ++position;
          else if (byte == 0) break;
          collected.push_back(byte);
        }
        if (typecode == UnicodeString) parts.emplace_back(kv_internal::textDecode(collected));
        else parts.emplace_back(std::move(collected));
      } else if (typecode >= IntegerArbitraryByteNegative && typecode <= IntegerArbitraryBytePositive) {
        const bool negative = typecode < IntegerZero;
        std::size_t count;
        if (typecode == IntegerArbitraryBytePositive || typecode == IntegerArbitraryByteNegative) {
          if (position >= bytes.size()) throwTruncatedBigInt(negative);
          const std::uint8_t length = bytes[position++];
          count = negative ? static_cast<std::size_t>(0xFF - length) : length;
        } else {
          count = static_cast<std::size_t>(typecode > IntegerZero ? typecode - IntegerZero : IntegerZero - typecode);
        }
        KvBytes magnitude;
        for (std::size_t index = 0; index < count; ++index) {
          if (position >= bytes.size()) throwTruncatedBigInt(negative);
          const std::uint8_t byte = bytes[position++];
          magnitude.push_back(negative ? static_cast<std::uint8_t>(0xFF - byte) : byte);
        }
        const auto first = std::find_if(magnitude.begin(), magnitude.end(), [](std::uint8_t byte) { return byte != 0; });
        parts.emplace_back(kv_internal::bigintFromMagnitude(std::span(first, magnitude.end()), negative));
      } else if (typecode == FloatingPointDouble) {
        if (bytes.size() - position < 8) throw RangeError("Out of bounds access");
        std::uint64_t bits = 0;
        for (int index = 0; index < 8; ++index) bits = (bits << 8) | bytes[position + static_cast<std::size_t>(index)];
        bits = (bits & 0x8000000000000000ull) ? bits ^ 0x8000000000000000ull : ~bits;
        position += 8;
        parts.emplace_back(std::bit_cast<double>(bits));
      } else if (typecode == False) {
        parts.emplace_back(false);
      } else if (typecode == True) {
        parts.emplace_back(true);
      } else {
        throw JsError("Unsupported typecode: " + std::to_string(typecode) + " in key: [" + joinBytes(bytes, ", ") + "] after " + partsText());
      }
    }
    if (position < bytes.size()) throw JsError("Unexpected trailing bytes: " + std::to_string(bytes.size() - position));
    return parts;
  }

  int compareKvKeys(const KvKey& left, const KvKey& right) {
    const KvBytes a = packKey(left), b = packKey(right);
    const auto order = std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(), b.end());
    return order < 0 ? -1 : order > 0 ? 1 : 0;
  }

  std::string formatVersionstamp(std::uint64_t version) {
    char text[24];
    std::snprintf(text, sizeof text, "%016llx0000", static_cast<unsigned long long>(version));
    return text;
  }

  std::string packCursor(std::span<const std::uint8_t> lastYieldedKeyBytes) {
    const auto text = "{\"lastYieldedKeyBytes\":\"" + encodeHex(lastYieldedKeyBytes) + "\"}";
    return toBase64({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
  }

  KvBytes unpackCursor(std::string_view cursor) {
    if (cursor.size() > limits::kKvCursorBytes) throw JsError("Invalid cursor");
    try {
      boost::system::error_code error;
      const json::value parsed = json::parse(kv_internal::textDecode(atobBytes(cursor)), error);
      if (!error && parsed.is_object())
        if (const json::value* field = parsed.as_object().if_contains("lastYieldedKeyBytes"); field && field->is_string())
          return kv_internal::decodeHex(std::string_view(field->as_string()));
    } catch (const JsError&) {
    }
    throw JsError("Invalid cursor");
  }
}
