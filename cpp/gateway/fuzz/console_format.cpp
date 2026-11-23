#include "fuzz/support.hpp"
#include "uos/rt/console.hpp"

#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > uos::fuzz::maxInput) return 0;
  namespace console = uos::rt::console;
  const auto input = uos::fuzz::text(data, size);
  uos::fuzz::malformed([&] {
    console::Value raw(input);
    console::Value structured = console::Value::object({{std::string(input.substr(0, 32)), raw}, {"items", console::Value::array({raw})}});
    (void)console::format(console::Method::log, {raw, structured});
    (void)console::format(console::Method::error, {"%s %d %f %o %j", raw, raw, raw, structured, structured});
    (void)console::format(console::Method::warn, {console::Value(uos::rt::TypeError(std::string(input)))});
  });
  return 0;
}
