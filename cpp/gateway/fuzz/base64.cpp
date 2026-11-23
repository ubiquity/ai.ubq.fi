#include "fuzz/support.hpp"
#include "uos/rt/base64.hpp"

#include <cstdlib>
#include <algorithm>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > uos::fuzz::maxInput) return 0;
  namespace rt = uos::rt;
  const std::span bytes(data, size);
  const auto text = uos::fuzz::text(data, size);
  const auto equal = [&](const auto& decoded) { return std::ranges::equal(decoded, bytes); };
  if (!equal(rt::atobBytes(rt::toBase64(bytes))) || !equal(rt::fromBase64(rt::toBase64Url(bytes))) || !equal(rt::fromHex(rt::toHex(bytes)))) std::abort();
  if (rt::fromBase64(text).size() > size || rt::fromHex(text).size() > size / 2) std::abort();
  uos::fuzz::malformed([&] { if (rt::atobBytes(text).size() > size) std::abort(); });
  uos::fuzz::malformed([&] { (void)rt::btoa(text); });
  return 0;
}
