#include "fuzz/support.hpp"
#include "uos/rt/url.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > uos::fuzz::maxInput) return 0;
  const auto input = uos::fuzz::text(data, size);
  uos::fuzz::malformed([&] { (void)uos::rt::URL(input).href(); });
  uos::fuzz::malformed([&] {
    uos::rt::URL url(input, "https://fixture.invalid/base/path");
    (void)url.href();
    (void)url.searchParams().entries();
  });
  uos::fuzz::malformed([&] {
    uos::rt::URLSearchParams params(input);
    params.sort();
    (void)params.toString();
  });
  return 0;
}
