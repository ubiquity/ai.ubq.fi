#include "uos/harmony/reliability/hash.hpp"

#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <cstdint>

namespace uos::harmony::reliability::hash {
  // `BigInt.asUintN(64, hash * prime)` keeps exactly the low 64 bits, which is
  // unsigned 64-bit wrap-around arithmetic.
  std::string fnv1a64(std::string_view text) {
    std::uint64_t value = 0xcbf29ce484222325ULL;
    for (const std::uint8_t byte : rt::encodeUtf8(text)) {
      value ^= byte;
      value *= 0x100000001b3ULL;
    }
    return rt::padStart(rt::BigInt::fromUint64(value).toString(16), 16, "0");
  }

  std::string digestShort(std::string_view text) { return rt::slice(fnv1a64(text), 0, 12); }
}
