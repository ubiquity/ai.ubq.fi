#include "fuzz/support.hpp"
#include "uos/rt/env.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > uos::fuzz::maxInput) return 0;
  uos::fuzz::malformed([&] { (void)uos::rt::env::parseEnvFile(uos::fuzz::text(data, size)); });
  return 0;
}
