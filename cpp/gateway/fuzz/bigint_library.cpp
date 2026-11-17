#include "fuzz/support.hpp"
#include "uos/rt/number.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0 || size > uos::fuzz::maxInput) return 0;
  using uos::rt::BigInt;
  using uos::fuzz::malformed;

  const auto input = uos::fuzz::text(data, size);
  malformed([&] { (void)BigInt::fromString(input); });

  std::uint64_t native = 0;
  std::memcpy(&native, data, std::min(size, sizeof(native)));
  const auto separator = input.find('\n');
  const auto first = BigInt::parse(input.substr(0, separator));
  const auto second = separator == input.npos ? std::nullopt : BigInt::parse(input.substr(separator + 1));
  const BigInt a = first.value_or(BigInt(std::bit_cast<std::int64_t>(native)));
  const BigInt b = second.value_or(BigInt(static_cast<std::int64_t>(data[size - 1]) - 128));

  if (BigInt::fromString(a.toString()) != a || (a + b) - b != a || ((a ^ b) ^ b) != a) std::abort();
  (void)(a * b);
  (void)(a & b);
  (void)(a | b);
  (void)(~a);
  (void)(a <=> b);
  malformed([&] {
    const auto quotient = a / b;
    const auto remainder = a % b;
    if (quotient * b + remainder != a) std::abort();
  });
  malformed([&] { (void)(a << b); });
  malformed([&] { (void)(a >> b); });
  malformed([&] { (void)a.pow(BigInt(static_cast<std::int64_t>(data[0] % 16) - 2)); });
  malformed([&] { (void)a.toString(static_cast<int>(data[0] % 40)); });

  const auto width = static_cast<std::uint64_t>(data[size - 1]);
  (void)BigInt::asUintN(width, a);
  (void)BigInt::asIntN(width, a);
  (void)a.toUint64();
  (void)a.toInt64();
  (void)a.toNumber();
  malformed([&] { (void)BigInt::fromNumber(std::bit_cast<double>(native)); });
  return 0;
}
