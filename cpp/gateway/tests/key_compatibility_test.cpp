// src/storage/key-compatibility.ts has no direct bun test: differential cases recorded from the
// TS under Bun 1.4.0 (null: accepted; otherwise the TypeError message).
#include "uos/storage/key_compatibility.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/test.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rt = uos::rt;

TEST("differential: assertPortableKvKeyBytes") {
  const std::string nan = "Negative NaN KV key parts cannot retain their identity in Bun; the source database was not converted";
  const std::vector<std::pair<std::vector<std::uint8_t>, std::optional<std::string>>> cases = {
    {{}, "A stored KV key cannot be empty"},
    {{1, 97, 0}, std::nullopt},
    {{2, 97, 98, 0}, std::nullopt},
    {{2, 97, 0, 255, 98, 0}, std::nullopt},
    {{2, 97}, "Truncated KV bytes or string part"},
    {{2, 0, 255}, "Truncated KV bytes or string part"},
    {{2, 0, 255, 0}, std::nullopt},
    {{20}, std::nullopt},
    {{21, 1}, std::nullopt},
    {{21}, "Truncated KV key part"},
    {{22, 1}, "Truncated KV key part"},
    {{19, 254}, std::nullopt},
    {{12, 1, 2, 3, 4, 5, 6, 7, 8}, std::nullopt},
    {{11}, "Truncated KV bigint length"},
    {{11, 253, 1}, "Truncated KV key part"},
    {{11, 254, 1}, std::nullopt},
    {{29}, "Truncated KV bigint length"},
    {{29, 2, 1, 2}, std::nullopt},
    {{29, 3, 1, 2}, "Truncated KV key part"},
    {{33, 191, 248, 0, 0, 0, 0, 0, 0}, std::nullopt},
    {{33, 63, 255, 255, 255, 255, 255, 255, 255}, std::nullopt},
    {{33, 0, 15, 255, 255, 255, 255, 255, 255}, std::nullopt},
    {{33, 0, 7, 255, 255, 255, 255, 255, 254}, nan},
    {{33, 255, 248, 0, 0, 0, 0, 0, 1}, std::nullopt},
    {{33, 1, 2, 3}, "Truncated KV number part"},
    {{38}, std::nullopt},
    {{39}, std::nullopt},
    {{40}, "Unsupported KV key part tag 40"},
    {{32}, "Unsupported KV key part tag 32"},
    {{5}, "Unsupported KV key part tag 5"},
    {{2, 97, 0, 33, 127, 255, 255, 255, 255, 255, 255, 255}, std::nullopt},
    {{38, 39, 20, 2, 0}, std::nullopt},
    {{29, 0}, std::nullopt},
    {{11, 255}, std::nullopt},
  };
  for (const auto& [key, expected] : cases) {
    std::optional<std::string> actual;
    try {
      uos::storage::key_compatibility::assertPortableKvKeyBytes(key);
    } catch (const rt::TypeError& error) {
      actual = error.message;
    }
    EXPECT_EQ(actual, expected);
  }
}
