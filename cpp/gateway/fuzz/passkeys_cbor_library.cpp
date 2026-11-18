#include "fuzz/support.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

namespace uos::auth::passkeys::cbor {
  std::pair<std::vector<std::uint8_t>, std::size_t> reencode(std::span<const std::uint8_t> input, bool requireComplete);
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  namespace cbor = uos::auth::passkeys::cbor;
  const std::span<const std::uint8_t> input(data, size);
  std::optional<std::size_t> prefix;
  uos::fuzz::malformed([&] {
    const auto [roundTrip, consumed] = cbor::reencode(input, false);
    if (consumed > input.size() || !std::ranges::equal(roundTrip, input.first(consumed))) std::abort();
    prefix = consumed;
  });

  bool complete = false;
  uos::fuzz::malformed([&] {
    const auto [roundTrip, consumed] = cbor::reencode(input, true);
    if (consumed != input.size() || !std::ranges::equal(roundTrip, input)) std::abort();
    complete = true;
  });
  if (complete != (prefix && *prefix == input.size())) std::abort();
  return 0;
}
