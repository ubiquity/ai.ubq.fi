#include "fuzz/support.hpp"
#include "uos/rt/headers.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > uos::fuzz::maxInput) return 0;
  const auto input = uos::fuzz::text(data, size);
  const auto separator = input.find('\n');
  const auto name = input.substr(0, separator);
  const auto value = separator == std::string_view::npos ? std::string_view{} : input.substr(separator + 1);
  uos::rt::Headers headers;
  uos::fuzz::malformed([&] {
    headers.append(name, value);
    headers.append(name, value);
    (void)headers.get(name);
    (void)headers.getSetCookie();
    (void)uos::rt::detail::headerWireFields(headers);
  });
  uos::fuzz::malformed([&] { headers.set(name, value); });
  return 0;
}
