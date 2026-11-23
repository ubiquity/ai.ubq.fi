#include "fuzz/support.hpp"
#include "uos/rt/string.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > uos::fuzz::maxInput) return 0;
  const auto input = uos::fuzz::text(data, size);
  uos::fuzz::malformed([&] { (void)uos::rt::toLowerCase(input); });
  uos::fuzz::malformed([&] { (void)uos::rt::toUpperCase(input); });
  uos::fuzz::malformed([&] {
    const auto midpoint = size / 2;
    (void)uos::rt::localeCompare(input.substr(0, midpoint), input.substr(midpoint));
  });
  return 0;
}
