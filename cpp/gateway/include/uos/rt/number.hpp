#pragma once
// JavaScript number semantics (Bun 1.4 / JavaScriptCore). Every function here
// reproduces the oracle bit for bit; see docs/rt/values.md for the porting table.
#include <bit>
#include <compare>
#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace uos::rt {
  // `Number.*` statics. The global `parseInt` is the same function.
  namespace Number {
    inline constexpr double MAX_SAFE_INTEGER = 9007199254740991.0;
    inline constexpr double MIN_SAFE_INTEGER = -9007199254740991.0;
    inline constexpr double EPSILON = 0x1p-52;
    inline constexpr double MAX_VALUE = std::numeric_limits<double>::max();
    inline constexpr double MIN_VALUE = std::numeric_limits<double>::denorm_min();
    inline constexpr double POSITIVE_INFINITY = std::numeric_limits<double>::infinity();
    inline constexpr double NEGATIVE_INFINITY = -std::numeric_limits<double>::infinity();
    inline constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

    bool isFinite(double value);
    bool isNaN(double value);
    bool isInteger(double value);
    bool isSafeInteger(double value);
    // parseInt(string, radix): `radix` is the already ToInt32-converted argument; 0 means absent.
    double parseInt(std::string_view text, std::int32_t radix = 0);
  }
  using Number::parseInt;
  // Global isNaN/isFinite applied to a number (they ToNumber their argument first:
  // for a string use isNaN(stringToNumber(s))).
  bool isNaN(double value);
  bool isFinite(double value);

  // String(n), `${n}`, n.toString(), and every number JSON.stringify prints.
  std::string numberToString(double value);
  // Number(string) / unary + on a string: StringToNumber with JSC's literal rounding.
  double stringToNumber(std::string_view text);
  // StrWhiteSpaceChar: what String.prototype.trim and Number() strip (one code point).
  bool isStrWhiteSpace(char32_t codePoint);

  // ToInt32 / ToUint32 (the conversions behind the bitwise operators).
  std::int32_t toInt32(double value);
  std::uint32_t toUint32(double value);
  // a & b, a | b, a ^ b, ~a, a << b, a >> b, a >>> b on numbers.
  std::int32_t bitAnd(double a, double b);
  std::int32_t bitOr(double a, double b);
  std::int32_t bitXor(double a, double b);
  std::int32_t bitNot(double a);
  std::int32_t shiftLeft(double a, double b);
  std::int32_t shiftRight(double a, double b);
  std::uint32_t shiftRightUnsigned(double a, double b);
  // a % b (truncating remainder; sign of the dividend; -0 preserved).
  double mod(double a, double b);

  // rt::Math::trunc, floor, ceil, abs, max and min are constexpr (usable in `inline constexpr`
  // constants): at run time they call the same library cores as before; in a constant
  // expression a bit-exact twin (detail::*Constant) computes the same result.
#define UOS_RT_MATH_CONSTEXPR 1
  namespace Math {
    namespace detail {
      // The run-time cores (src/rt/number.cpp).
      double truncRuntime(double value);
      double floorRuntime(double value);
      double ceilRuntime(double value);
      double absRuntime(double value);
      double maxRuntime(std::span<const double> values);
      double minRuntime(std::span<const double> values);

      // Their constant-expression twins (tests/rt/number_test.cpp checks them against the
      // cores and the Bun vectors). Every double of magnitude 2^52 or more is an integer,
      // and so are NaN and the infinities, which pass through.
      constexpr bool signBit(double value) { return (std::bit_cast<std::uint64_t>(value) >> 63) != 0; }
      constexpr bool integralAlready(double value) { return !(value == value) || value >= 0x1p52 || value <= -0x1p52; }
      constexpr double truncConstant(double value) {
        if (integralAlready(value)) return value;
        const double truncated = static_cast<double>(static_cast<std::int64_t>(value));
        if (truncated == 0) return signBit(value) ? -0.0 : 0.0;
        return truncated;
      }
      constexpr double floorConstant(double value) {
        if (integralAlready(value)) return value;
        const double truncated = static_cast<double>(static_cast<std::int64_t>(value));
        if (truncated == value) return value;
        return value < 0 ? truncated - 1 : truncated;
      }
      constexpr double ceilConstant(double value) {
        if (integralAlready(value)) return value;
        const double truncated = static_cast<double>(static_cast<std::int64_t>(value));
        if (truncated == value) return value;
        if (value > 0) return truncated + 1;
        return truncated == 0 ? -0.0 : truncated;
      }
      constexpr double absConstant(double value) { return std::bit_cast<double>(std::bit_cast<std::uint64_t>(value) & 0x7fff'ffff'ffff'ffffULL); }
      constexpr double maxConstant(std::span<const double> values) {
        double result = -std::numeric_limits<double>::infinity();
        bool sawNaN = false;
        for (const double value : values) {
          if (!(value == value)) sawNaN = true;
          else if (value > result || (value == 0 && result == 0 && !signBit(value))) result = value;
        }
        return sawNaN ? std::numeric_limits<double>::quiet_NaN() : result;
      }
      constexpr double minConstant(std::span<const double> values) {
        double result = std::numeric_limits<double>::infinity();
        bool sawNaN = false;
        for (const double value : values) {
          if (!(value == value)) sawNaN = true;
          else if (value < result || (value == 0 && result == 0 && signBit(value))) result = value;
        }
        return sawNaN ? std::numeric_limits<double>::quiet_NaN() : result;
      }
    }

    double round(double value);  // halves round toward +Infinity; -0.5..-0 gives -0
    constexpr double trunc(double value) {
      if (std::is_constant_evaluated()) return detail::truncConstant(value);
      return detail::truncRuntime(value);
    }
    constexpr double floor(double value) {
      if (std::is_constant_evaluated()) return detail::floorConstant(value);
      return detail::floorRuntime(value);
    }
    constexpr double ceil(double value) {
      if (std::is_constant_evaluated()) return detail::ceilConstant(value);
      return detail::ceilRuntime(value);
    }
    double sign(double value);
    constexpr double abs(double value) {
      if (std::is_constant_evaluated()) return detail::absConstant(value);
      return detail::absRuntime(value);
    }
    // a ** b and Math.pow(a, b): JSC uses repeated squaring for non-negative int32 exponents.
    double pow(double base, double exponent);
    std::int32_t imul(double a, double b);
    // Math.max(...values) / Math.min(...values): NaN wins, +0 > -0, empty gives -Infinity / +Infinity.
    constexpr double max(std::span<const double> values) {
      if (std::is_constant_evaluated()) return detail::maxConstant(values);
      return detail::maxRuntime(values);
    }
    constexpr double min(std::span<const double> values) {
      if (std::is_constant_evaluated()) return detail::minConstant(values);
      return detail::minRuntime(values);
    }
    template <class... T>
      requires(std::is_arithmetic_v<T> && ...)
    constexpr double max(T... values) {
      const double list[] = {static_cast<double>(values)..., 0.0};
      return max(std::span<const double>(list, sizeof...(T)));
    }
    template <class... T>
      requires(std::is_arithmetic_v<T> && ...)
    constexpr double min(T... values) {
      const double list[] = {static_cast<double>(values)..., 0.0};
      return min(std::span<const double>(list, sizeof...(T)));
    }
  }
}

#include <boost/multiprecision/fwd.hpp>
#include <cstddef>

namespace uos::rt {
  // Arbitrary-precision integer with JavaScript's signed bit operations.
  class BigInt {
  public:
    BigInt() noexcept;
    explicit BigInt(std::int64_t value);
    BigInt(const BigInt& other);
    BigInt(BigInt&& other) noexcept;
    BigInt& operator=(const BigInt& other);
    BigInt& operator=(BigInt&& other) noexcept;
    ~BigInt();
    static BigInt fromUint64(std::uint64_t value);
    // BigInt(number). RangeError "Not an integer" for NaN, infinities and fractions.
    static BigInt fromNumber(double value);
    // BigInt(string). SyntaxError "Failed to parse String to BigInt".
    static BigInt fromString(std::string_view text);
    // Returns std::nullopt for invalid text. Throws RangeError for an oversized value.
    static std::optional<BigInt> parse(std::string_view text);
    static BigInt asUintN(std::uint64_t bits, const BigInt& value);
    static BigInt asIntN(std::uint64_t bits, const BigInt& value);

    // String(b) / b.toString(radix). RangeError for a radix outside 2..36.
    std::string toString(int radix = 10) const;
    // Number(b): rounds to nearest, ties to even; Infinity past the double range.
    double toNumber() const;
    // BigInt.asIntN(64, b) / BigInt.asUintN(64, b) as native integers.
    std::int64_t toInt64() const;
    std::uint64_t toUint64() const;
    bool isZero() const;
    bool isNegative() const;
    // b ** exponent. RangeError "Negative exponent is not allowed".
    BigInt pow(const BigInt& exponent) const;

    friend bool operator==(const BigInt& a, const BigInt& b);
    friend std::strong_ordering operator<=>(const BigInt& a, const BigInt& b);
    friend BigInt operator+(const BigInt& a, const BigInt& b);
    friend BigInt operator-(const BigInt& a, const BigInt& b);
    friend BigInt operator*(const BigInt& a, const BigInt& b);
    // Truncating division; RangeError "0 is an invalid divisor value." on a zero divisor.
    friend BigInt operator/(const BigInt& a, const BigInt& b);
    friend BigInt operator%(const BigInt& a, const BigInt& b);
    friend BigInt operator&(const BigInt& a, const BigInt& b);
    friend BigInt operator|(const BigInt& a, const BigInt& b);
    friend BigInt operator^(const BigInt& a, const BigInt& b);
    friend BigInt operator<<(const BigInt& a, const BigInt& shift);
    friend BigInt operator>>(const BigInt& a, const BigInt& shift);
    BigInt operator-() const;
    BigInt operator~() const;
    BigInt& operator+=(const BigInt& other) { return *this = *this + other; }
    BigInt& operator-=(const BigInt& other) { return *this = *this - other; }
    BigInt& operator*=(const BigInt& other) { return *this = *this * other; }
    BigInt& operator/=(const BigInt& other) { return *this = *this / other; }
    BigInt& operator%=(const BigInt& other) { return *this = *this % other; }
    BigInt& operator&=(const BigInt& other) { return *this = *this & other; }
    BigInt& operator|=(const BigInt& other) { return *this = *this | other; }
    BigInt& operator^=(const BigInt& other) { return *this = *this ^ other; }
    BigInt& operator<<=(const BigInt& shift) { return *this = *this << shift; }
    BigInt& operator>>=(const BigInt& shift) { return *this = *this >> shift; }

    // Limit the arithmetic and allocation requested by untrusted inputs.
    static constexpr std::uint64_t maxLengthBits = std::uint64_t{1} << 20;

  private:
    explicit BigInt(boost::multiprecision::cpp_int value);
    boost::multiprecision::cpp_int& value();
    const boost::multiprecision::cpp_int& value() const;

    // Keep Boost's implementation header private without allocating for small integers.
    static constexpr std::size_t storageAlignment = 16;
    alignas(storageAlignment) std::byte storage_[32];
  };
}
