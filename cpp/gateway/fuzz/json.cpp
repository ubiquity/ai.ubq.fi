#include "fuzz/support.hpp"
#include "uos/rt/json.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > uos::fuzz::maxInput) return 0;
  uos::fuzz::malformed([&] {
    const auto value = uos::rt::jsonParse(uos::fuzz::text(data, size));
    const auto serialized = uos::rt::jsonStringify(value);
    (void)uos::rt::jsonParse(serialized);
  });
  return 0;
}
