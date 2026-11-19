#include "fuzz/support.hpp"
#include "uos/rt/fs.hpp"

#include <filesystem>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > uos::fuzz::maxInput) return 0;
  namespace path = uos::rt::path;
  const auto input = uos::fuzz::text(data, size);
  const auto first = input.substr(0, size / 2);
  const auto second = input.substr(size / 2);
  uos::fuzz::malformed([&] {
    (void)path::join(input);
    (void)path::isAbsolute(input);
    (void)path::dirname(input);
    const std::filesystem::path native(input);
    (void)native.filename();
    (void)native.extension();
    (void)path::join(first, second);
    (void)path::resolve("/fuzz", first, second);
    (void)path::relative("/" + std::string(first), "/" + std::string(second));
  });
  return 0;
}
