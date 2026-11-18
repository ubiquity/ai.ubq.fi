#include "uos/rt/number.hpp"

#include "uos/rt/error.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <system_error>

#include <boost/multiprecision/cpp_int.hpp>
#include <iterator>
#include <memory>
#include <new>

namespace uos::rt {
  namespace {
    constexpr double kInfinity = std::numeric_limits<double>::infinity();
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    constexpr double kTwo53 = 9007199254740992.0;
    constexpr double kTwo32 = 4294967296.0;
    constexpr char kRadixDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    const std::string kOutOfMemory = "Out of memory: BigInt generated from this operation is too big";
    const std::string kRadixRange = "toString() radix argument must be between 2 and 36";

    // One code point of UTF-8 (lone surrogates in their 3-byte WTF-8 form allowed).
    // Malformed bytes decode as a one-byte U+FFFD, which is neither space nor digit.
    char32_t decodeCodePoint(std::string_view text, std::size_t index, std::size_t& length) {
      const auto lead = static_cast<unsigned char>(text[index]);
      length = 1;
      if (lead < 0x80) return lead;
      std::size_t need = 0;
      char32_t value = 0;
      char32_t minimum = 0;
      if (lead >= 0xC2 && lead <= 0xDF) {
        need = 1;
        value = lead & 0x1Fu;
        minimum = 0x80;
      } else if (lead >= 0xE0 && lead <= 0xEF) {
        need = 2;
        value = lead & 0x0Fu;
        minimum = 0x800;
      } else if (lead >= 0xF0 && lead <= 0xF4) {
        need = 3;
        value = lead & 0x07u;
        minimum = 0x10000;
      } else {
        return 0xFFFD;
      }
      for (std::size_t offset = 1; offset <= need; ++offset) {
        if (index + offset >= text.size()) return 0xFFFD;
        const auto next = static_cast<unsigned char>(text[index + offset]);
        if ((next & 0xC0u) != 0x80u) return 0xFFFD;
        value = (value << 6) | (next & 0x3Fu);
      }
      if (value < minimum || value > 0x10FFFF) return 0xFFFD;
      length = need + 1;
      return value;
    }

    std::size_t skipWhiteSpace(std::string_view text, std::size_t index) {
      while (index < text.size()) {
        std::size_t length = 0;
        if (!isStrWhiteSpace(decodeCodePoint(text, index, length))) break;
        index += length;
      }
      return index;
    }

    bool isDigit(char c) { return c >= '0' && c <= '9'; }

    int digitValue(char c, int radix) {
      int digit = 36;
      if (c >= '0' && c <= '9') digit = c - '0';
      else if (c >= 'a' && c <= 'z') digit = c - 'a' + 10;
      else if (c >= 'A' && c <= 'Z') digit = c - 'A' + 10;
      return digit < radix ? digit : -1;
    }

    // JSC parseIntOverflow: sums digit * radix^k from the least significant digit.
    double parseIntOverflow(std::string_view digits, int radix) {
      double number = 0.0;
      double multiplier = 1.0;
      for (std::size_t index = digits.size(); index-- > 0;) {
        if (multiplier == kInfinity) {
          if (digits[index] != '0') {
            number = kInfinity;
            break;
          }
        } else {
          const volatile double term = static_cast<double>(digitValue(digits[index], radix)) * multiplier;
          number += term;
        }
        multiplier *= static_cast<double>(radix);
      }
      return number;
    }

    // Correctly rounded decimal-to-double of an unsigned decimal literal body
    // (digits, optional point and fraction, optional exponent).
    double decimalToDouble(std::string_view body) {
      double value = 0.0;
      const auto [end, error] = std::from_chars(body.data(), body.data() + body.size(), value, std::chars_format::general);
      if (error == std::errc() && end == body.data() + body.size()) return value;
      // Out of range: decide between overflow and underflow from the decimal magnitude.
      std::size_t index = 0;
      long long position = 0;  // decimal exponent of the first significant digit, plus one
      bool seenSignificant = false;
      bool afterPoint = false;
      for (; index < body.size() && (isDigit(body[index]) || body[index] == '.'); ++index) {
        if (body[index] == '.') {
          afterPoint = true;
          continue;
        }
        if (!seenSignificant && body[index] == '0') {
          if (afterPoint) --position;
          continue;
        }
        if (!seenSignificant) seenSignificant = true;
        if (!afterPoint) ++position;
      }
      if (!seenSignificant) return 0.0;
      long long exponent = 0;
      if (index < body.size() && (body[index] == 'e' || body[index] == 'E')) {
        ++index;
        bool negativeExponent = false;
        if (index < body.size() && (body[index] == '+' || body[index] == '-')) negativeExponent = body[index++] == '-';
        for (; index < body.size() && isDigit(body[index]); ++index) exponent = std::min(exponent * 10 + (body[index] - '0'), 1000000000LL);
        if (negativeExponent) exponent = -exponent;
      }
      return position + exponent > 0 ? kInfinity : 0.0;
    }

    // WTF::parseDouble on a prefix: [+-]? (digits [. digits?] | . digits) ([eE][+-]?digits)?
    // Returns the parsed length (0 when nothing parses).
    std::size_t parseDecimalPrefix(std::string_view text, double& number) {
      std::size_t index = 0;
      bool negative = false;
      if (index < text.size() && (text[index] == '+' || text[index] == '-')) negative = text[index++] == '-';
      const std::size_t bodyStart = index;
      while (index < text.size() && isDigit(text[index])) ++index;
      const std::size_t integerDigits = index - bodyStart;
      if (index < text.size() && text[index] == '.') {
        std::size_t cursor = index + 1;
        while (cursor < text.size() && isDigit(text[cursor])) ++cursor;
        const std::size_t fractionDigits = cursor - index - 1;
        if (integerDigits == 0 && fractionDigits == 0) return 0;
        index = cursor;
      } else if (integerDigits == 0) {
        return 0;
      }
      if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
        std::size_t cursor = index + 1;
        if (cursor < text.size() && (text[cursor] == '+' || text[cursor] == '-')) ++cursor;
        const std::size_t exponentStart = cursor;
        while (cursor < text.size() && isDigit(text[cursor])) ++cursor;
        if (cursor > exponentStart) index = cursor;
      }
      const double magnitude = decimalToDouble(text.substr(bodyStart, index - bodyStart));
      number = negative ? -magnitude : magnitude;
      return index;
    }

    // jsStrDecimalLiteral: a decimal prefix, or [+-]?Infinity.
    std::size_t strDecimalLiteral(std::string_view text, double& number) {
      if (const std::size_t length = parseDecimalPrefix(text, number)) return length;
      constexpr std::string_view infinity = "Infinity";
      if (text.starts_with(infinity)) {
        number = kInfinity;
        return infinity.size();
      }
      if (text.size() > infinity.size() && (text[0] == '+' || text[0] == '-') && text.substr(1).starts_with(infinity)) {
        number = text[0] == '-' ? -kInfinity : kInfinity;
        return infinity.size() + 1;
      }
      number = kNaN;
      return 0;
    }

    // jsHexIntegerLiteral / jsOctalIntegerLiteral / jsBinaryIntegerLiteral after the prefix.
    std::size_t radixIntegerLiteral(std::string_view digits, int radix, double& number) {
      std::size_t index = 0;
      number = 0.0;
      while (index < digits.size()) {
        const int digit = digitValue(digits[index], radix);
        if (digit < 0) break;
        number = number * radix + digit;
        ++index;
      }
      if (number >= kTwo53) number = parseIntOverflow(digits.substr(0, index), radix);
      return index;
    }

    struct ShortestDigits {
      std::string digits;  // significant digits, no leading or trailing zeros
      int exponent = 0;    // value = 0.digits * 10^exponent (JS "n")
    };

    ShortestDigits shortestDigits(double magnitude) {
      char buffer[64];
      const auto result = std::to_chars(buffer, buffer + sizeof buffer, magnitude, std::chars_format::scientific);
      ShortestDigits out;
      const char* cursor = buffer;
      for (; cursor < result.ptr && *cursor != 'e'; ++cursor)
        if (*cursor != '.') out.digits.push_back(*cursor);
      int exponent = 0;
      std::from_chars(cursor + 1 + (cursor[1] == '+' ? 1 : 0), result.ptr, exponent);
      while (out.digits.size() > 1 && out.digits.back() == '0') out.digits.pop_back();
      out.exponent = exponent + 1;
      return out;
    }

    // ---- BigInt adapter ----
    namespace mp = boost::multiprecision;
    using mp::cpp_int;

    std::size_t bitLength(const cpp_int& value) {
      if (value == 0) return 0;
      return 1 + (value < 0 ? mp::msb(mp::abs(value)) : mp::msb(value));
    }

    cpp_int shiftBy(const cpp_int& value, const cpp_int& count, bool left) {
      if (value == 0 || count == 0) return value;
      const bool grows = left != (count < 0);
      constexpr auto limit = static_cast<int>(BigInt::maxLengthBits);
      if (count > limit || count < -limit) {
        if (grows) throw RangeError(kOutOfMemory);
        return value < 0 ? -1 : 0;
      }
      const auto bits = static_cast<unsigned>(std::abs(count.convert_to<int>()));
      if (!grows) return value >> bits;
      if (bitLength(value) + bits > BigInt::maxLengthBits) throw RangeError(kOutOfMemory);
      return value << bits;
    }

    std::string formatBigIntRadix(cpp_int magnitude, int radix) {
      // cpp_int::str provides bases 8, 10 and 16. Group the remaining
      // power-of-two digits with export_bits, and use Boost division for the rest.
      const auto base = static_cast<unsigned>(radix);
      std::string result;
      if (std::has_single_bit(base)) {
        mp::export_bits(magnitude, std::back_inserter(result), static_cast<unsigned>(std::countr_zero(base)));
        for (char& digit : result) digit = kRadixDigits[static_cast<unsigned char>(digit)];
        return result;
      }

      std::uint64_t chunkBase = base;
      std::size_t digitsPerChunk = 1;
      while (chunkBase <= std::numeric_limits<std::uint64_t>::max() / base) {
        chunkBase *= base;
        ++digitsPerChunk;
      }
      const cpp_int divisor(chunkBase);
      cpp_int quotient, remainder;
      do {
        mp::divide_qr(magnitude, divisor, quotient, remainder);
        char chunk[64];
        char* end = std::to_chars(chunk, chunk + sizeof(chunk), remainder.convert_to<std::uint64_t>(), radix).ptr;
        std::reverse(chunk, end);
        result.append(chunk, end);
        if (quotient != 0) result.append(digitsPerChunk - static_cast<std::size_t>(end - chunk), '0');
        magnitude.swap(quotient);
      } while (magnitude != 0);
      std::reverse(result.begin(), result.end());
      return result;
    }
  }

  bool isStrWhiteSpace(char32_t c) {
    switch (c) {
      case 0x09:
      case 0x0A:
      case 0x0B:
      case 0x0C:
      case 0x0D:
      case 0x20:
      case 0xA0:
      case 0x1680:
      case 0x2028:
      case 0x2029:
      case 0x202F:
      case 0x205F:
      case 0x3000:
      case 0xFEFF:
        return true;
      default:
        return c >= 0x2000 && c <= 0x200A;
    }
  }

  namespace Number {
    bool isFinite(double value) { return std::isfinite(value); }
    bool isNaN(double value) { return std::isnan(value); }
    bool isInteger(double value) { return std::isfinite(value) && std::trunc(value) == value; }
    bool isSafeInteger(double value) { return isInteger(value) && std::fabs(value) <= MAX_SAFE_INTEGER; }

    double parseInt(std::string_view text, std::int32_t radix) {
      std::size_t index = skipWhiteSpace(text, 0);
      double sign = 1.0;
      if (index < text.size()) {
        if (text[index] == '+') {
          ++index;
        } else if (text[index] == '-') {
          sign = -1.0;
          ++index;
        }
      }
      if ((radix == 0 || radix == 16) && text.size() - index >= 2 && text[index] == '0' && (text[index + 1] == 'x' || text[index + 1] == 'X')) {
        radix = 16;
        index += 2;
      } else if (radix == 0) {
        radix = 10;
      }
      if (radix < 2 || radix > 36) return kNaN;
      const std::size_t first = index;
      double number = 0.0;
      while (index < text.size()) {
        const int digit = digitValue(text[index], radix);
        if (digit < 0) break;
        const volatile double scaled = number * radix;
        number = scaled + digit;
        ++index;
      }
      if (index == first) return kNaN;
      if (number >= kTwo53) {
        const std::string_view digits = text.substr(first, index - first);
        if (radix == 10) number = decimalToDouble(digits);
        else if (radix == 2 || radix == 4 || radix == 8 || radix == 16 || radix == 32) number = parseIntOverflow(digits, radix);
      }
      return sign * number;
    }
  }

  bool isNaN(double value) { return std::isnan(value); }
  bool isFinite(double value) { return std::isfinite(value); }

  std::string numberToString(double value) {
    if (std::isnan(value)) return "NaN";
    if (value == 0) return "0";
    if (std::isinf(value)) return value > 0 ? "Infinity" : "-Infinity";
    const ShortestDigits shortest = shortestDigits(std::fabs(value));
    const std::string& digits = shortest.digits;
    const int k = static_cast<int>(digits.size());
    const int n = shortest.exponent;
    std::string out = value < 0 ? "-" : "";
    if (k <= n && n <= 21) {
      out += digits;
      out.append(static_cast<std::size_t>(n - k), '0');
    } else if (0 < n && n <= 21) {
      out += digits.substr(0, static_cast<std::size_t>(n));
      out += '.';
      out += digits.substr(static_cast<std::size_t>(n));
    } else if (-6 < n && n <= 0) {
      out += "0.";
      out.append(static_cast<std::size_t>(-n), '0');
      out += digits;
    } else {
      const int exponent = n - 1;
      out += digits[0];
      if (k > 1) {
        out += '.';
        out += digits.substr(1);
      }
      out += exponent < 0 ? "e-" : "e+";
      out += std::to_string(exponent < 0 ? -exponent : exponent);
    }
    return out;
  }

  double stringToNumber(std::string_view text) {
    const std::size_t start = skipWhiteSpace(text, 0);
    if (start == text.size()) return 0.0;
    const std::string_view rest = text.substr(start);
    double number = kNaN;
    std::size_t length = 0;
    const auto prefixed = [&](char letter, int radix) {
      return rest.size() > 2 && rest[0] == '0' && (rest[1] | 0x20) == letter && digitValue(rest[2], radix) >= 0;
    };
    if (prefixed('x', 16)) length = 2 + radixIntegerLiteral(rest.substr(2), 16, number);
    else if (prefixed('o', 8)) length = 2 + radixIntegerLiteral(rest.substr(2), 8, number);
    else if (prefixed('b', 2)) length = 2 + radixIntegerLiteral(rest.substr(2), 2, number);
    else length = strDecimalLiteral(rest, number);
    if (length == 0) return kNaN;
    return skipWhiteSpace(text, start + length) == text.size() ? number : kNaN;
  }

  std::int32_t toInt32(double value) { return static_cast<std::int32_t>(toUint32(value)); }

  std::uint32_t toUint32(double value) {
    if (!std::isfinite(value)) return 0;
    double modulo = std::fmod(std::trunc(value), kTwo32);
    if (modulo < 0) modulo += kTwo32;
    return static_cast<std::uint32_t>(modulo);
  }

  std::int32_t bitAnd(double a, double b) { return toInt32(a) & toInt32(b); }
  std::int32_t bitOr(double a, double b) { return toInt32(a) | toInt32(b); }
  std::int32_t bitXor(double a, double b) { return toInt32(a) ^ toInt32(b); }
  std::int32_t bitNot(double a) { return ~toInt32(a); }
  std::int32_t shiftLeft(double a, double b) { return static_cast<std::int32_t>(toUint32(a) << (toUint32(b) & 31u)); }
  std::int32_t shiftRight(double a, double b) { return toInt32(a) >> (toUint32(b) & 31u); }
  std::uint32_t shiftRightUnsigned(double a, double b) { return toUint32(a) >> (toUint32(b) & 31u); }

  double mod(double a, double b) { return std::fmod(a, b); }

  namespace Math {
    namespace detail {
      double truncRuntime(double value) { return std::trunc(value); }
      double floorRuntime(double value) { return std::floor(value); }
      double ceilRuntime(double value) { return std::ceil(value); }
      double absRuntime(double value) { return std::fabs(value); }
      double maxRuntime(std::span<const double> values) {
        double result = -kInfinity;
        bool sawNaN = false;
        for (const double value : values) {
          if (std::isnan(value)) sawNaN = true;
          else if (value > result || (value == 0 && result == 0 && !std::signbit(value))) result = value;
        }
        return sawNaN ? kNaN : result;
      }
      double minRuntime(std::span<const double> values) {
        double result = kInfinity;
        bool sawNaN = false;
        for (const double value : values) {
          if (std::isnan(value)) sawNaN = true;
          else if (value < result || (value == 0 && result == 0 && std::signbit(value))) result = value;
        }
        return sawNaN ? kNaN : result;
      }
    }

    double round(double value) {
      const double integer = std::ceil(value);
      return integer - (integer - 0.5 > value ? 1.0 : 0.0);
    }
    double sign(double value) {
      if (std::isnan(value) || value == 0) return value;
      return value > 0 ? 1.0 : -1.0;
    }

    double pow(double base, double exponent) {
      if (std::isnan(exponent)) return kNaN;
      if (std::isinf(exponent) && std::fabs(base) == 1) return kNaN;
      if (exponent >= 0 && exponent <= 2147483647.0 && std::trunc(exponent) == exponent) {
        auto remaining = static_cast<std::int32_t>(exponent);
        double result = 1.0;
        while (remaining) {
          if (remaining & 1) result *= base;
          base *= base;
          remaining >>= 1;
        }
        return result;
      }
      return std::pow(base, exponent);
    }

    std::int32_t imul(double a, double b) { return static_cast<std::int32_t>(toUint32(a) * toUint32(b)); }

  }

  // ---- BigInt ----

  BigInt::BigInt() noexcept {
    static_assert(sizeof(cpp_int) <= sizeof(storage_));
    static_assert(alignof(cpp_int) <= storageAlignment);
    std::construct_at(reinterpret_cast<cpp_int*>(storage_));
  }

  BigInt::BigInt(std::int64_t value) { std::construct_at(reinterpret_cast<cpp_int*>(storage_), value); }

  BigInt::BigInt(cpp_int value) {
    if (bitLength(value) > maxLengthBits) throw RangeError(kOutOfMemory);
    std::construct_at(reinterpret_cast<cpp_int*>(storage_), std::move(value));
  }

  BigInt::BigInt(const BigInt& other) { std::construct_at(reinterpret_cast<cpp_int*>(storage_), other.value()); }

  BigInt::BigInt(BigInt&& other) noexcept {
    std::construct_at(reinterpret_cast<cpp_int*>(storage_), std::move(other.value()));
    other.value() = 0;
  }

  BigInt& BigInt::operator=(const BigInt& other) {
    value() = other.value();
    return *this;
  }

  BigInt& BigInt::operator=(BigInt&& other) noexcept {
    if (this != &other) {
      value() = std::move(other.value());
      other.value() = 0;
    }
    return *this;
  }

  BigInt::~BigInt() { std::destroy_at(&value()); }

  cpp_int& BigInt::value() { return *std::launder(reinterpret_cast<cpp_int*>(storage_)); }

  const cpp_int& BigInt::value() const { return *std::launder(reinterpret_cast<const cpp_int*>(storage_)); }

  bool BigInt::isZero() const { return value() == 0; }

  bool BigInt::isNegative() const { return value() < 0; }

  BigInt BigInt::fromUint64(std::uint64_t value) { return BigInt(cpp_int(value)); }

  BigInt BigInt::fromNumber(double value) {
    if (!std::isfinite(value) || std::trunc(value) != value) throw RangeError("Not an integer");
    return BigInt(cpp_int(value));
  }

  std::optional<BigInt> BigInt::parse(std::string_view text) {
    text.remove_prefix(skipWhiteSpace(text, 0));
    if (text.empty()) return BigInt();

    const bool negative = text.front() == '-';
    int radix = 10;
    if (negative || text.front() == '+') {
      text.remove_prefix(1);
    } else if (text.size() >= 2 && text.front() == '0') {
      switch (text[1] | 0x20) {
        case 'b': radix = 2; break;
        case 'o': radix = 8; break;
        case 'x': radix = 16; break;
      }
      if (radix != 10) text.remove_prefix(2);
    }

    // Validate the entire input before passing a NUL-terminated token to Boost.
    std::size_t end = 0;
    while (end < text.size() && digitValue(text[end], radix) >= 0) ++end;
    if (end == 0 || skipWhiteSpace(text, end) != text.size()) return std::nullopt;
    auto digits = text.substr(0, end);
    const auto first = digits.find_first_not_of('0');
    if (first == std::string_view::npos) return BigInt();
    digits.remove_prefix(first);

    // Bound temporary work before any library allocation. The constructor also
    // checks the exact bit length for values on the decimal or octal boundary.
    constexpr auto maxDecimalDigits = maxLengthBits * 30103 / 100000 + 1;  // 30103/100000 > log10(2)
    const auto bitsPerDigit = static_cast<unsigned>(std::countr_zero(static_cast<unsigned>(radix)));
    const auto digitLimit = radix == 10 ? maxDecimalDigits : (maxLengthBits - 1) / bitsPerDigit + 1;
    if (digits.size() > digitLimit) throw RangeError(kOutOfMemory);

    if (radix == 2) {
      std::string bits(digits);
      for (char& digit : bits) digit = static_cast<char>(digit - '0');
      cpp_int value;
      mp::import_bits(value, bits.begin(), bits.end(), 1);
      return BigInt(std::move(value));
    }
    // Boost recognizes 0x for hex and 0 for octal. Decimal has no leading zeroes.
    std::string token = radix == 16 ? "0x" : radix == 8 ? "0" : negative ? "-" : "";
    token += digits;
    return BigInt(cpp_int(token));
  }

  BigInt BigInt::fromString(std::string_view text) {
    auto parsed = parse(text);
    if (!parsed) throw SyntaxError("Failed to parse String to BigInt");
    return std::move(*parsed);
  }

  BigInt BigInt::asUintN(std::uint64_t bits, const BigInt& value) {
    if (bits == 0 || value.isZero()) return BigInt();
    if (!value.isNegative() && bitLength(value.value()) <= bits) return value;
    if (bits > maxLengthBits) throw RangeError(kOutOfMemory);
    return BigInt(value.value() & ((cpp_int(1) << bits) - 1));
  }

  BigInt BigInt::asIntN(std::uint64_t bits, const BigInt& value) {
    if (bits > maxLengthBits) return value;
    BigInt result = asUintN(bits, value);
    if (bits != 0 && mp::bit_test(result.value(), static_cast<unsigned>(bits - 1))) result.value() -= cpp_int(1) << bits;
    return result;
  }

  std::string BigInt::toString(int radix) const {
    if (radix < 2 || radix > 36) throw RangeError(kRadixRange);
    if (radix == 10) return value().str();
    if (radix == 8 || radix == 16) {
      const auto format = radix == 8 ? std::ios_base::oct : std::ios_base::hex;
      if (!isNegative()) return value().str(0, format);
      return '-' + cpp_int(-value()).str(0, format);
    }
    auto result = formatBigIntRadix(mp::abs(value()), radix);
    if (isNegative()) result.insert(result.begin(), '-');
    return result;
  }

  double BigInt::toNumber() const { return value().convert_to<double>(); }

  std::uint64_t BigInt::toUint64() const {
    const cpp_int low = value() & std::numeric_limits<std::uint64_t>::max();
    return low.convert_to<std::uint64_t>();
  }

  std::int64_t BigInt::toInt64() const { return static_cast<std::int64_t>(toUint64()); }

  BigInt BigInt::pow(const BigInt& exponent) const {
    if (exponent.isNegative()) throw RangeError("Negative exponent is not allowed");
    if (exponent.isZero()) return BigInt(1);
    if (isZero() || value() == 1) return *this;
    if (value() == -1) return BigInt(mp::bit_test(exponent.value(), 0) ? -1 : 1);

    // |value| >= 2. Reject exponents whose result cannot fit before invoking pow.
    // This also bounds conversion to Boost's unsigned exponent.
    if (exponent.value() > (maxLengthBits - 1) / (bitLength(value()) - 1)) throw RangeError(kOutOfMemory);
    return BigInt(mp::pow(value(), exponent.value().convert_to<unsigned>()));
  }

  bool operator==(const BigInt& a, const BigInt& b) { return a.value() == b.value(); }

  std::strong_ordering operator<=>(const BigInt& a, const BigInt& b) { return a.value().compare(b.value()) <=> 0; }

  BigInt operator+(const BigInt& a, const BigInt& b) { return BigInt(a.value() + b.value()); }
  BigInt operator-(const BigInt& a, const BigInt& b) { return BigInt(a.value() - b.value()); }

  BigInt operator*(const BigInt& a, const BigInt& b) {
    if (bitLength(a.value()) + bitLength(b.value()) > BigInt::maxLengthBits + 1) throw RangeError(kOutOfMemory);
    return BigInt(a.value() * b.value());
  }

  BigInt operator/(const BigInt& a, const BigInt& b) {
    if (b.isZero()) throw RangeError("0 is an invalid divisor value.");
    return BigInt(a.value() / b.value());
  }

  BigInt operator%(const BigInt& a, const BigInt& b) {
    if (b.isZero()) throw RangeError("0 is an invalid divisor value.");
    return BigInt(a.value() % b.value());
  }

  BigInt operator&(const BigInt& a, const BigInt& b) { return BigInt(a.value() & b.value()); }
  BigInt operator|(const BigInt& a, const BigInt& b) { return BigInt(a.value() | b.value()); }
  BigInt operator^(const BigInt& a, const BigInt& b) { return BigInt(a.value() ^ b.value()); }
  BigInt operator<<(const BigInt& a, const BigInt& shift) { return BigInt(shiftBy(a.value(), shift.value(), true)); }
  BigInt operator>>(const BigInt& a, const BigInt& shift) { return BigInt(shiftBy(a.value(), shift.value(), false)); }

  BigInt BigInt::operator-() const { return BigInt(-value()); }
  BigInt BigInt::operator~() const { return BigInt(~value()); }
}
