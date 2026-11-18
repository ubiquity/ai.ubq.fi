#include "fuzz/support.hpp"
#include "uos/rt/date.hpp"

#include <cmath>
#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > uos::fuzz::maxInput) return 0;
  uos::fuzz::malformed([&] {
    const uos::rt::Date date(uos::fuzz::text(data, size));
    if (std::isfinite(date.getTime()) && uos::rt::Date::parse(date.toISOString()) != date.getTime()) std::abort();
  });
  return 0;
}
