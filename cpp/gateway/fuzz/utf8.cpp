#include "fuzz/support.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > uos::fuzz::maxInput) return 0;
  namespace rt = uos::rt;
  const auto bytes = uos::fuzz::text(data, size);
  const auto decoded = rt::decodeUtf8(bytes);
  const auto split = size / 2;
  const std::string_view view(bytes);
  rt::TextDecoder decoder;
  auto streamed = decoder.decode(view.substr(0, split), {.stream = true});
  streamed += decoder.decode(view.substr(split));
  if (decoded != streamed || !rt::isUtf8(decoded)) std::abort();

  const auto units = rt::toUtf16(bytes);
  if (rt::fromUtf16(units) != rt::canonicalWtf8(bytes)) std::abort();
  uos::fuzz::malformed([&] { (void)rt::decodeUtf8(bytes, {.fatal = true}); });
  return 0;
}
