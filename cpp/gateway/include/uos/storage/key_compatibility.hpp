#pragma once
// Port of src/storage/key-compatibility.ts.
#include <cstdint>
#include <span>

namespace uos::storage::key_compatibility {
  /** Reject legacy tuple keys whose numeric identity JavaScriptCore cannot retain. */
  // Throws rt::TypeError.
  void assertPortableKvKeyBytes(std::span<const std::uint8_t> key);
}
