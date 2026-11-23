#include "uos/storage/key_compatibility.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/number.hpp"

#include <cstddef>
#include <string>

namespace uos::storage::key_compatibility {
  namespace {
    constexpr std::uint64_t SIGN_BIT = 0x8000000000000000ULL;
    constexpr std::uint64_t EXPONENT_BITS = 0x7ff0000000000000ULL;
    constexpr std::uint64_t FRACTION_BITS = 0x000fffffffffffffULL;
    constexpr std::uint64_t U64_MASK = 0xffffffffffffffffULL;
  }

  void assertPortableKvKeyBytes(std::span<const std::uint8_t> key) {
    if (key.empty()) throw rt::TypeError("A stored KV key cannot be empty");
    std::size_t offset = 0;
    while (offset < key.size()) {
      const std::uint8_t tag = key[offset++];
      if (tag == 0x01 || tag == 0x02) {
        bool terminated = false;
        while (offset < key.size()) {
          const std::uint8_t byte = key[offset++];
          if (byte != 0) continue;
          if (offset < key.size() && key[offset] == 0xff) {
            offset += 1;
            continue;
          }
          terminated = true;
          break;
        }
        if (!terminated) throw rt::TypeError("Truncated KV bytes or string part");
        continue;
      }
      if (tag >= 0x0b && tag <= 0x1d) {
        std::size_t length = tag >= 0x14 ? static_cast<std::size_t>(tag - 0x14) : static_cast<std::size_t>(0x14 - tag);
        if (tag == 0x0b || tag == 0x1d) {
          if (offset >= key.size()) throw rt::TypeError("Truncated KV bigint length");
          length = tag == 0x0b ? static_cast<std::size_t>(0xff - key[offset++]) : static_cast<std::size_t>(key[offset++]);
        }
        offset += length;
      } else if (tag == 0x21) {
        if (offset + 8 > key.size()) throw rt::TypeError("Truncated KV number part");
        std::uint64_t bits = 0;
        for (int index = 0; index < 8; index += 1) bits = (bits << 8U) | key[offset++];
        bits ^= (bits & SIGN_BIT) != 0 ? SIGN_BIT : U64_MASK;
        if ((bits & SIGN_BIT) != 0 && (bits & EXPONENT_BITS) == EXPONENT_BITS && (bits & FRACTION_BITS) != 0) {
          throw rt::TypeError("Negative NaN KV key parts cannot retain their identity in Bun; the source database was not converted");
        }
      } else if (tag != 0x26 && tag != 0x27) {
        throw rt::TypeError("Unsupported KV key part tag " + rt::numberToString(tag));
      }
      if (offset > key.size()) throw rt::TypeError("Truncated KV key part");
    }
  }
}
