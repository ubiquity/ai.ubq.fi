#include "uos/rt/number.hpp"

#include "uos/rt/error.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <system_error>

namespace uos::rt {
  struct BigIntAccess {
    using Limbs = std::vector<std::uint32_t>;
    static const Limbs& limbs(const BigInt& value) { return value.magnitude_; }
    static bool negative(const BigInt& value) { return value.negative_; }
    static BigInt make(bool negative, Limbs magnitude);
  };

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

    // ---- BigInt magnitude arithmetic (little-endian 32-bit limbs) ----
    using Limbs = std::vector<std::uint32_t>;

    void trim(Limbs& limbs) {
      while (!limbs.empty() && limbs.back() == 0) limbs.pop_back();
    }

    std::uint64_t bitLength(const Limbs& limbs) {
      if (limbs.empty()) return 0;
      return (limbs.size() - 1) * 32 + static_cast<std::uint64_t>(32 - std::countl_zero(limbs.back()));
    }

    int compareMagnitude(const Limbs& a, const Limbs& b) {
      if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
      for (std::size_t index = a.size(); index-- > 0;)
        if (a[index] != b[index]) return a[index] < b[index] ? -1 : 1;
      return 0;
    }

    Limbs addMagnitude(const Limbs& a, const Limbs& b) {
      const Limbs& longer = a.size() >= b.size() ? a : b;
      const Limbs& shorter = a.size() >= b.size() ? b : a;
      Limbs out(longer.size() + 1, 0);
      std::uint64_t carry = 0;
      for (std::size_t index = 0; index < longer.size(); ++index) {
        const std::uint64_t sum = std::uint64_t{longer[index]} + (index < shorter.size() ? shorter[index] : 0u) + carry;
        out[index] = static_cast<std::uint32_t>(sum);
        carry = sum >> 32;
      }
      out[longer.size()] = static_cast<std::uint32_t>(carry);
      trim(out);
      return out;
    }

    // a - b with a >= b.
    Limbs subtractMagnitude(const Limbs& a, const Limbs& b) {
      Limbs out(a.size(), 0);
      std::int64_t borrow = 0;
      for (std::size_t index = 0; index < a.size(); ++index) {
        std::int64_t difference = std::int64_t{a[index]} - (index < b.size() ? std::int64_t{b[index]} : 0) - borrow;
        borrow = difference < 0 ? 1 : 0;
        if (difference < 0) difference += std::int64_t{1} << 32;
        out[index] = static_cast<std::uint32_t>(difference);
      }
      trim(out);
      return out;
    }

    Limbs multiplyMagnitude(const Limbs& a, const Limbs& b) {
      if (a.empty() || b.empty()) return {};
      Limbs out(a.size() + b.size(), 0);
      for (std::size_t i = 0; i < a.size(); ++i) {
        std::uint64_t carry = 0;
        for (std::size_t j = 0; j < b.size(); ++j) {
          const std::uint64_t product = std::uint64_t{a[i]} * b[j] + out[i + j] + carry;
          out[i + j] = static_cast<std::uint32_t>(product);
          carry = product >> 32;
        }
        std::size_t index = i + b.size();
        while (carry) {
          const std::uint64_t sum = std::uint64_t{out[index]} + carry;
          out[index] = static_cast<std::uint32_t>(sum);
          carry = sum >> 32;
          ++index;
        }
      }
      trim(out);
      return out;
    }

    void multiplySmallAdd(Limbs& limbs, std::uint32_t factor, std::uint32_t addend) {
      std::uint64_t carry = addend;
      for (auto& limb : limbs) {
        const std::uint64_t product = std::uint64_t{limb} * factor + carry;
        limb = static_cast<std::uint32_t>(product);
        carry = product >> 32;
      }
      if (carry) limbs.push_back(static_cast<std::uint32_t>(carry));
    }

    // Divides in place by a single limb; returns the remainder.
    std::uint32_t divideSmall(Limbs& limbs, std::uint32_t divisor) {
      std::uint64_t remainder = 0;
      for (std::size_t index = limbs.size(); index-- > 0;) {
        const std::uint64_t current = (remainder << 32) | limbs[index];
        limbs[index] = static_cast<std::uint32_t>(current / divisor);
        remainder = current % divisor;
      }
      trim(limbs);
      return static_cast<std::uint32_t>(remainder);
    }

    Limbs shiftLeftMagnitude(const Limbs& limbs, std::uint64_t bits) {
      if (limbs.empty()) return {};
      const std::size_t limbShift = static_cast<std::size_t>(bits / 32);
      const unsigned bitShift = static_cast<unsigned>(bits % 32);
      Limbs out(limbs.size() + limbShift + 1, 0);
      for (std::size_t index = 0; index < limbs.size(); ++index) {
        const std::uint64_t shifted = std::uint64_t{limbs[index]} << bitShift;
        out[index + limbShift] |= static_cast<std::uint32_t>(shifted);
        out[index + limbShift + 1] |= static_cast<std::uint32_t>(shifted >> 32);
      }
      trim(out);
      return out;
    }

    // Floor shift; reports whether any one bits were shifted out.
    Limbs shiftRightMagnitude(const Limbs& limbs, std::uint64_t bits, bool& lostBits) {
      lostBits = false;
      const std::uint64_t limbShift64 = bits / 32;
      if (limbShift64 >= limbs.size()) {
        lostBits = !limbs.empty();
        return {};
      }
      const std::size_t limbShift = static_cast<std::size_t>(limbShift64);
      const unsigned bitShift = static_cast<unsigned>(bits % 32);
      for (std::size_t index = 0; index < limbShift; ++index)
        if (limbs[index]) lostBits = true;
      if (bitShift && (limbs[limbShift] & ((1u << bitShift) - 1u))) lostBits = true;
      Limbs out(limbs.size() - limbShift, 0);
      for (std::size_t index = 0; index < out.size(); ++index) {
        std::uint64_t window = limbs[index + limbShift];
        if (index + limbShift + 1 < limbs.size()) window |= std::uint64_t{limbs[index + limbShift + 1]} << 32;
        out[index] = static_cast<std::uint32_t>(window >> bitShift);
      }
      trim(out);
      return out;
    }

    // Knuth algorithm D. Requires a non-empty divisor.
    void divideMagnitude(const Limbs& dividend, const Limbs& divisor, Limbs& quotient, Limbs& remainder) {
      if (compareMagnitude(dividend, divisor) < 0) {
        quotient.clear();
        remainder = dividend;
        return;
      }
      if (divisor.size() == 1) {
        quotient = dividend;
        const std::uint32_t rest = divideSmall(quotient, divisor[0]);
        remainder.clear();
        if (rest) remainder.push_back(rest);
        return;
      }
      const int shift = std::countl_zero(divisor.back());
      Limbs normalizedDivisor = shiftLeftMagnitude(divisor, static_cast<std::uint64_t>(shift));
      Limbs normalizedDividend = shiftLeftMagnitude(dividend, static_cast<std::uint64_t>(shift));
      normalizedDividend.resize(dividend.size() + 1, 0);
      const std::size_t n = normalizedDivisor.size();
      const std::size_t m = normalizedDividend.size() - n;
      quotient.assign(m, 0);
      const std::uint64_t base = std::uint64_t{1} << 32;
      for (std::size_t j = m; j-- > 0;) {
        const std::uint64_t top = (std::uint64_t{normalizedDividend[j + n]} << 32) | normalizedDividend[j + n - 1];
        std::uint64_t estimate = top / normalizedDivisor[n - 1];
        std::uint64_t rest = top % normalizedDivisor[n - 1];
        while (estimate >= base || estimate * normalizedDivisor[n - 2] > ((rest << 32) | normalizedDividend[j + n - 2])) {
          --estimate;
          rest += normalizedDivisor[n - 1];
          if (rest >= base) break;
        }
        std::int64_t borrow = 0;
        std::uint64_t carry = 0;
        for (std::size_t i = 0; i < n; ++i) {
          const std::uint64_t product = estimate * normalizedDivisor[i] + carry;
          carry = product >> 32;
          const std::int64_t difference = std::int64_t{normalizedDividend[i + j]} - static_cast<std::int64_t>(product & 0xFFFFFFFFu) - borrow;
          normalizedDividend[i + j] = static_cast<std::uint32_t>(difference);
          borrow = difference < 0 ? 1 : 0;
        }
        const std::int64_t difference = std::int64_t{normalizedDividend[j + n]} - static_cast<std::int64_t>(carry) - borrow;
        normalizedDividend[j + n] = static_cast<std::uint32_t>(difference);
        if (difference < 0) {
          --estimate;
          std::uint64_t addCarry = 0;
          for (std::size_t i = 0; i < n; ++i) {
            const std::uint64_t sum = std::uint64_t{normalizedDividend[i + j]} + normalizedDivisor[i] + addCarry;
            normalizedDividend[i + j] = static_cast<std::uint32_t>(sum);
            addCarry = sum >> 32;
          }
          normalizedDividend[j + n] = static_cast<std::uint32_t>(std::uint64_t{normalizedDividend[j + n]} + addCarry);
        }
        quotient[j] = static_cast<std::uint32_t>(estimate);
      }
      trim(quotient);
      normalizedDividend.resize(n);
      trim(normalizedDividend);
      bool lost = false;
      remainder = shiftRightMagnitude(normalizedDividend, static_cast<std::uint64_t>(shift), lost);
    }

    void checkSize(const Limbs& limbs) {
      if (bitLength(limbs) > BigInt::maxLengthBits) throw RangeError(kOutOfMemory);
    }

    // Two's complement of a signed magnitude, truncated or extended to `width` limbs.
    Limbs twosComplement(bool negative, const Limbs& magnitude, std::size_t width) {
      Limbs out(width, 0);
      if (!negative) {
        std::copy_n(magnitude.begin(), std::min(width, magnitude.size()), out.begin());
        return out;
      }
      // ~(|x| - 1)
      const Limbs one{1};
      const Limbs decremented = subtractMagnitude(magnitude, one);
      for (std::size_t index = 0; index < width; ++index) out[index] = ~(index < decremented.size() ? decremented[index] : 0u);
      return out;
    }

    BigInt fromTwosComplement(Limbs limbs) {
      const bool negative = !limbs.empty() && (limbs.back() & 0x80000000u);
      if (!negative) {
        trim(limbs);
        return BigIntAccess::make(false, std::move(limbs));
      }
      for (auto& limb : limbs) limb = ~limb;
      trim(limbs);
      return BigIntAccess::make(true, addMagnitude(limbs, Limbs{1}));
    }

    template <class Operation>
    BigInt bitwise(const BigInt& a, const BigInt& b, Operation operation) {
      const auto& left = BigIntAccess::limbs(a);
      const auto& right = BigIntAccess::limbs(b);
      const std::size_t width = std::max(left.size(), right.size()) + 1;
      const Limbs x = twosComplement(BigIntAccess::negative(a), left, width);
      const Limbs y = twosComplement(BigIntAccess::negative(b), right, width);
      Limbs out(width, 0);
      for (std::size_t index = 0; index < width; ++index) out[index] = operation(x[index], y[index]);
      return fromTwosComplement(std::move(out));
    }

    // Shift count as a signed 64-bit value, saturated beyond the BigInt size limit.
    std::int64_t shiftCount(const BigInt& shift) {
      const auto& limbs = BigIntAccess::limbs(shift);
      const std::int64_t limit = static_cast<std::int64_t>(BigInt::maxLengthBits) + 1;
      std::int64_t magnitude = 0;
      if (bitLength(limbs) > 40) magnitude = limit;
      else
        for (std::size_t index = limbs.size(); index-- > 0;) magnitude = (magnitude << 32) | limbs[index];
      magnitude = std::min(magnitude, limit);
      return BigIntAccess::negative(shift) ? -magnitude : magnitude;
    }

    BigInt shiftBy(const BigInt& value, std::int64_t count) {
      const auto& limbs = BigIntAccess::limbs(value);
      if (limbs.empty() || count == 0) return value;
      const bool negative = BigIntAccess::negative(value);
      if (count > 0) {
        if (static_cast<std::uint64_t>(count) > BigInt::maxLengthBits) throw RangeError(kOutOfMemory);
        Limbs shifted = shiftLeftMagnitude(limbs, static_cast<std::uint64_t>(count));
        checkSize(shifted);
        return BigIntAccess::make(negative, std::move(shifted));
      }
      bool lost = false;
      Limbs shifted = shiftRightMagnitude(limbs, static_cast<std::uint64_t>(-count), lost);
      // Arithmetic shift rounds toward -Infinity.
      if (negative && lost) shifted = addMagnitude(shifted, Limbs{1});
      return BigIntAccess::make(negative, std::move(shifted));
    }

    // Exact value of a finite non-negative double scaled by 2^kFractionBits.
    constexpr std::uint64_t kFractionBits = 1076;

    BigInt scaledExact(double value) {
      int exponent = 0;
      const double mantissa = std::frexp(value, &exponent);  // value = mantissa * 2^exponent, mantissa in [0.5, 1)
      const auto integer = static_cast<std::int64_t>(std::ldexp(mantissa, 53));
      const std::int64_t shift = static_cast<std::int64_t>(kFractionBits) + exponent - 53;
      return shiftBy(BigInt(integer), shift);
    }
  }

  BigInt BigIntAccess::make(bool negative, Limbs magnitude) {
    trim(magnitude);
    BigInt out;
    out.negative_ = negative && !magnitude.empty();
    out.magnitude_ = std::move(magnitude);
    return out;
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

    double parseFloat(std::string_view text) {
      if (text.size() == 1) return isDigit(text[0]) ? text[0] - '0' : kNaN;
      const std::size_t index = skipWhiteSpace(text, 0);
      if (index == text.size()) return kNaN;
      double number = kNaN;
      strDecimalLiteral(text.substr(index), number);
      return number;
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

  std::string numberToString(double value, int radix) {
    if (radix < 2 || radix > 36) throw RangeError(kRadixRange);
    if (radix == 10 || !std::isfinite(value)) return numberToString(value);
    const bool negative = value < 0;
    const double number = std::fabs(value);
    double integerPart = std::floor(number);
    const double fractionPart = number - integerPart;
    std::string fraction;
    if (fractionPart != 0) {
      // JSC toStringWithRadixInternal: emit digits until the remainder is within
      // half the distance to the neighbouring doubles, rounding half to even.
      const bool integerPartIsOdd = integerPart <= 9007199254740991.0 && (static_cast<std::int64_t>(integerPart) & 1);
      bool isOddInOddRadix = integerPartIsOdd;
      std::uint32_t digit = integerPartIsOdd ? 1u : 0u;
      const BigInt one = BigInt(1) << BigInt(static_cast<std::int64_t>(kFractionBits));
      const BigInt half = BigInt(1) << BigInt(static_cast<std::int64_t>(kFractionBits - 1));
      const BigInt radixValue(radix);
      BigInt remainder = scaledExact(fractionPart);
      const double next = std::nextafter(number, kInfinity);
      const double previous = std::nextafter(number, -kInfinity);
      BigInt halfDeltaNext = scaledExact(next - number) >> BigInt(1);
      BigInt halfDeltaLast = scaledExact(number - previous) >> BigInt(1);
      bool needsRoundingUp = false;
      fraction = ".";
      while (true) {
        const auto comparison = remainder <=> half;
        const bool odd = (radix & 1) ? isOddInOddRadix : (digit & 1u) != 0;
        if (comparison > 0 || (comparison == 0 && odd)) {
          if (remainder + halfDeltaNext > one) {
            needsRoundingUp = true;
            break;
          }
        } else if (remainder < halfDeltaLast) {
          break;
        }
        remainder *= radixValue;
        const BigInt whole = remainder >> BigInt(static_cast<std::int64_t>(kFractionBits));
        digit = static_cast<std::uint32_t>(whole.toUint64());
        remainder -= whole << BigInt(static_cast<std::int64_t>(kFractionBits));
        fraction += kRadixDigits[digit];
        if (digit & 1u) isOddInOddRadix = !isOddInOddRadix;
        halfDeltaNext *= radixValue;
        halfDeltaLast *= radixValue;
      }
      if (needsRoundingUp) {
        while (fraction.back() == kRadixDigits[radix - 1]) fraction.pop_back();
        if (fraction.back() == '9') {
          fraction.back() = 'a';
        } else if (fraction.back() != '.') {
          ++fraction.back();
        } else {
          fraction.clear();
          integerPart += 1;
        }
      } else {
        while (fraction.back() == '0') fraction.pop_back();
      }
    }
    std::string out = BigInt::fromNumber(integerPart).toString(radix) + fraction;
    return negative ? "-" + out : out;
  }

  std::string toFixed(double value, int fractionDigits) {
    if (fractionDigits < 0 || fractionDigits > 100) throw RangeError("toFixed() argument must be between 0 and 100");
    if (!std::isfinite(value) || std::fabs(value) >= 1e21) return numberToString(value);
    // The exact decimal expansion of a double never needs more than 1074 fraction digits.
    std::string exact(1200, '\0');
    const auto result = std::to_chars(exact.data(), exact.data() + exact.size(), std::fabs(value), std::chars_format::fixed, 1074);
    exact.resize(static_cast<std::size_t>(result.ptr - exact.data()));
    const std::size_t point = exact.find('.');
    std::string digits = exact.substr(0, point) + exact.substr(point + 1, static_cast<std::size_t>(fractionDigits));
    if (exact[point + 1 + static_cast<std::size_t>(fractionDigits)] >= '5') {
      std::size_t index = digits.size();
      while (index > 0) {
        --index;
        if (digits[index] == '9') {
          digits[index] = '0';
          continue;
        }
        ++digits[index];
        break;
      }
      if (index == 0 && digits[0] == '0') digits.insert(digits.begin(), '1');
    }
    const std::size_t integerDigits = digits.size() - static_cast<std::size_t>(fractionDigits);
    std::string out = value < 0 ? "-" : "";
    out += digits.substr(0, integerDigits);
    if (fractionDigits > 0) {
      out += '.';
      out += digits.substr(integerDigits);
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

  BigInt::BigInt(std::int64_t value) {
    negative_ = value < 0;
    std::uint64_t magnitude = negative_ ? std::uint64_t{0} - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
    while (magnitude) {
      magnitude_.push_back(static_cast<std::uint32_t>(magnitude));
      magnitude >>= 32;
    }
  }

  BigInt BigInt::fromUint64(std::uint64_t value) {
    Limbs limbs;
    while (value) {
      limbs.push_back(static_cast<std::uint32_t>(value));
      value >>= 32;
    }
    return BigIntAccess::make(false, std::move(limbs));
  }

  BigInt BigInt::fromNumber(double value) {
    if (!std::isfinite(value) || std::trunc(value) != value) throw RangeError("Not an integer");
    if (value == 0) return BigInt();
    int exponent = 0;
    const double mantissa = std::frexp(std::fabs(value), &exponent);
    const auto integer = static_cast<std::int64_t>(std::ldexp(mantissa, 53));
    const BigInt magnitude = shiftBy(BigInt(integer), static_cast<std::int64_t>(exponent) - 53);
    return value < 0 ? -magnitude : magnitude;
  }

  std::optional<BigInt> BigInt::parse(std::string_view text) {
    // Code points; whitespace and digits are all in the BMP, so indexes need not be UTF-16 exact.
    std::vector<char32_t> characters;
    for (std::size_t index = 0; index < text.size();) {
      std::size_t length = 0;
      characters.push_back(decodeCodePoint(text, index, length));
      index += length;
    }
    std::size_t index = 0;
    while (index < characters.size() && isStrWhiteSpace(characters[index])) ++index;
    const auto digitsFrom = [&](std::size_t start, int radix, bool negative, bool allowEmpty) -> std::optional<BigInt> {
      if (!allowEmpty && start == characters.size()) return std::nullopt;
      std::size_t cursor = start;
      while (cursor < characters.size() && characters[cursor] == '0') ++cursor;
      std::size_t end = characters.size();
      while (end > cursor && isStrWhiteSpace(characters[end - 1])) --end;
      Limbs limbs;
      for (; cursor < end; ++cursor) {
        const char32_t c = characters[cursor];
        const int digit = c < 0x80 ? digitValue(static_cast<char>(c), radix) : -1;
        if (digit < 0) return std::nullopt;
        multiplySmallAdd(limbs, static_cast<std::uint32_t>(radix), static_cast<std::uint32_t>(digit));
        trim(limbs);
        checkSize(limbs);
      }
      return BigIntAccess::make(negative, std::move(limbs));
    };
    if (index + 1 < characters.size() && characters[index] == '0') {
      const char32_t letter = characters[index + 1] | 0x20;
      if (letter == 'b') return digitsFrom(index + 2, 2, false, false);
      if (letter == 'x') return digitsFrom(index + 2, 16, false, false);
      if (letter == 'o') return digitsFrom(index + 2, 8, false, false);
    }
    bool negative = false;
    if (index < characters.size()) {
      if (characters[index] == '+') {
        ++index;
      } else if (characters[index] == '-') {
        negative = true;
        ++index;
      }
    }
    return digitsFrom(index, 10, negative, true);
  }

  BigInt BigInt::fromString(std::string_view text) {
    auto parsed = parse(text);
    if (!parsed) throw SyntaxError("Failed to parse String to BigInt");
    return std::move(*parsed);
  }

  BigInt BigInt::asUintN(std::uint64_t bits, const BigInt& value) {
    if (bits == 0 || value.isZero()) return BigInt();
    if (!value.negative_ && bitLength(value.magnitude_) <= bits) return value;
    if (value.negative_ && bits > maxLengthBits) throw RangeError(kOutOfMemory);
    // Here bits <= maxLengthBits: either the value is negative or it is wider than `bits`.
    Limbs limbs = twosComplement(value.negative_, value.magnitude_, static_cast<std::size_t>((bits + 31) / 32));
    const unsigned topBits = static_cast<unsigned>(bits % 32);
    if (topBits) limbs.back() &= (1u << topBits) - 1u;
    return BigIntAccess::make(false, std::move(limbs));
  }

  BigInt BigInt::asIntN(std::uint64_t bits, const BigInt& value) {
    if (bits == 0 || value.isZero()) return BigInt();
    if (bits > maxLengthBits) return value;
    BigInt unsignedValue = asUintN(bits, value);
    if (bitLength(unsignedValue.magnitude_) == bits) return unsignedValue - (BigInt(1) << BigInt(static_cast<std::int64_t>(bits)));
    return unsignedValue;
  }

  std::string BigInt::toString(int radix) const {
    if (radix < 2 || radix > 36) throw RangeError(kRadixRange);
    if (magnitude_.empty()) return "0";
    // Peel off the largest power of the radix that fits a limb at a time.
    std::uint32_t chunk = static_cast<std::uint32_t>(radix);
    int chunkDigits = 1;
    while (std::uint64_t{chunk} * static_cast<std::uint64_t>(radix) <= 0xFFFFFFFFu) {
      chunk *= static_cast<std::uint32_t>(radix);
      ++chunkDigits;
    }
    Limbs rest = magnitude_;
    std::string reversed;
    while (!rest.empty()) {
      std::uint32_t part = divideSmall(rest, chunk);
      for (int count = 0; count < chunkDigits && (part || !rest.empty()); ++count) {
        reversed += kRadixDigits[part % static_cast<std::uint32_t>(radix)];
        part /= static_cast<std::uint32_t>(radix);
      }
    }
    if (negative_) reversed += '-';
    return {reversed.rbegin(), reversed.rend()};
  }

  double BigInt::toNumber() const {
    const std::uint64_t bits = bitLength(magnitude_);
    if (bits == 0) return 0.0;
    double magnitude = 0.0;
    if (bits <= 64) {
      std::uint64_t value = 0;
      for (std::size_t index = magnitude_.size(); index-- > 0;) value = (value << 32) | magnitude_[index];
      magnitude = static_cast<double>(value);
    } else if (bits > 1025) {
      magnitude = kInfinity;
    } else {
      bool sticky = false;
      const Limbs top = shiftRightMagnitude(magnitude_, bits - 64, sticky);
      const std::uint64_t window = (std::uint64_t{top.size() > 1 ? top[1] : 0u} << 32) | top[0];
      std::uint64_t mantissa = window >> 11;
      const std::uint64_t guard = window & 0x7FFu;
      if (guard > 0x400u || (guard == 0x400u && (sticky || (mantissa & 1u)))) ++mantissa;
      magnitude = std::ldexp(static_cast<double>(mantissa), static_cast<int>(bits) - 53);
    }
    return negative_ ? -magnitude : magnitude;
  }

  std::uint64_t BigInt::toUint64() const {
    const Limbs limbs = twosComplement(negative_, magnitude_, std::max<std::size_t>(magnitude_.size(), 2) + 1);
    return (std::uint64_t{limbs[1]} << 32) | limbs[0];
  }

  std::int64_t BigInt::toInt64() const { return static_cast<std::int64_t>(toUint64()); }

  BigInt BigInt::pow(const BigInt& exponent) const {
    if (exponent.negative_) throw RangeError("Negative exponent is not allowed");
    if (exponent.isZero()) return BigInt(1);
    if (isZero()) return *this;
    if (magnitude_.size() == 1 && magnitude_[0] == 1) {
      if (negative_ && !(exponent.magnitude_[0] & 1u)) return -*this;
      return *this;
    }
    if (exponent.magnitude_.size() > 2 || (exponent.magnitude_.size() == 2 && exponent.magnitude_[1])) throw RangeError(kOutOfMemory);
    const std::uint64_t power = exponent.magnitude_[0];
    if (power == 1) return *this;
    if (power >= maxLengthBits) throw RangeError(kOutOfMemory);
    BigInt result(1);
    BigInt square = *this;
    std::uint64_t remaining = power;
    while (true) {
      if (remaining & 1u) result *= square;
      remaining >>= 1;
      if (!remaining) break;
      square *= square;
    }
    return result;
  }

  std::strong_ordering operator<=>(const BigInt& a, const BigInt& b) {
    if (a.negative_ != b.negative_) return a.negative_ ? std::strong_ordering::less : std::strong_ordering::greater;
    const int magnitude = compareMagnitude(a.magnitude_, b.magnitude_);
    const int ordered = a.negative_ ? -magnitude : magnitude;
    return ordered < 0 ? std::strong_ordering::less : ordered > 0 ? std::strong_ordering::greater : std::strong_ordering::equal;
  }

  BigInt operator+(const BigInt& a, const BigInt& b) {
    if (a.negative_ == b.negative_) {
      Limbs sum = addMagnitude(a.magnitude_, b.magnitude_);
      checkSize(sum);
      return BigIntAccess::make(a.negative_, std::move(sum));
    }
    const int comparison = compareMagnitude(a.magnitude_, b.magnitude_);
    if (comparison == 0) return BigInt();
    if (comparison > 0) return BigIntAccess::make(a.negative_, subtractMagnitude(a.magnitude_, b.magnitude_));
    return BigIntAccess::make(b.negative_, subtractMagnitude(b.magnitude_, a.magnitude_));
  }

  BigInt operator-(const BigInt& a, const BigInt& b) { return a + (-b); }

  BigInt operator*(const BigInt& a, const BigInt& b) {
    if (bitLength(a.magnitude_) + bitLength(b.magnitude_) > BigInt::maxLengthBits + 1) throw RangeError(kOutOfMemory);
    Limbs product = multiplyMagnitude(a.magnitude_, b.magnitude_);
    checkSize(product);
    return BigIntAccess::make(a.negative_ != b.negative_, std::move(product));
  }

  BigInt operator/(const BigInt& a, const BigInt& b) {
    if (b.isZero()) throw RangeError("0 is an invalid divisor value.");
    Limbs quotient, remainder;
    divideMagnitude(a.magnitude_, b.magnitude_, quotient, remainder);
    return BigIntAccess::make(a.negative_ != b.negative_, std::move(quotient));
  }

  BigInt operator%(const BigInt& a, const BigInt& b) {
    if (b.isZero()) throw RangeError("0 is an invalid divisor value.");
    Limbs quotient, remainder;
    divideMagnitude(a.magnitude_, b.magnitude_, quotient, remainder);
    return BigIntAccess::make(a.negative_, std::move(remainder));
  }

  BigInt operator&(const BigInt& a, const BigInt& b) {
    return bitwise(a, b, [](std::uint32_t x, std::uint32_t y) { return x & y; });
  }
  BigInt operator|(const BigInt& a, const BigInt& b) {
    return bitwise(a, b, [](std::uint32_t x, std::uint32_t y) { return x | y; });
  }
  BigInt operator^(const BigInt& a, const BigInt& b) {
    return bitwise(a, b, [](std::uint32_t x, std::uint32_t y) { return x ^ y; });
  }
  BigInt operator<<(const BigInt& a, const BigInt& shift) { return shiftBy(a, shiftCount(shift)); }
  BigInt operator>>(const BigInt& a, const BigInt& shift) { return shiftBy(a, -shiftCount(shift)); }

  BigInt BigInt::operator-() const { return BigIntAccess::make(!negative_, magnitude_); }
  BigInt BigInt::operator~() const { return -*this - BigInt(1); }
}
