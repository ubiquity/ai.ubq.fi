#include "fuzz/support.hpp"
#include "uos/rt/regex.hpp"
#include <algorithm>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 2 || size > 1024) return 0;
  const std::size_t patternSize = std::min<std::size_t>(data[0], size - 2);
  constexpr std::string_view flags[] = {"", "u", "i", "g", "m", "s", "y", "v"};
  uos::fuzz::malformed([&] {
    uos::rt::Regex regex(uos::fuzz::text(data + 2, patternSize), flags[data[1] % 8]);
    const auto subject = uos::fuzz::text(data + 2 + patternSize, size - 2 - patternSize);
    (void)regex.exec(subject);
    regex.lastIndex = 0;
    (void)regex.test(subject);
  });
  return 0;
}
