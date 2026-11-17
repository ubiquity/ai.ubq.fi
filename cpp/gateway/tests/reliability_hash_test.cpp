// Differential cases for src/harmony/reliability/hash.ts (no bun test targets
// it directly). Expected values were recorded from the TypeScript module under
// Bun 1.4.0.
#include "uos/harmony/reliability/hash.hpp"
#include "uos/rt/test.hpp"

#include <string>

namespace {
  using namespace uos::harmony::reliability::hash;

  void expectDigest(const std::string& text, const char* full, const char* shortDigest) {
    EXPECT_EQ(fnv1a64(text), full);
    EXPECT_EQ(digestShort(text), shortDigest);
  }
}

TEST("hash: fnv1a64 and digestShort match the TypeScript digests") {
  expectDigest("", "cbf29ce484222325", "cbf29ce48422");
  expectDigest("a", "af63dc4c8601ec8c", "af63dc4c8601");
  expectDigest("hello world", "779a65e7023cd2e7", "779a65e7023c");
  expectDigest("é😀中", "f38b217c83f5bbd7", "f38b217c83f5");
  expectDigest(std::string("line\nbreak") + std::string(1, '\0') + "nul", "b167c7a11c5e6e89", "b167c7a11c5e");
}

TEST("hash: lone surrogates digest as their TextEncoder replacement bytes") {
  expectDigest("\xED\xA0\x80x", "af46b2e0782edaf6", "af46b2e0782e");  // "\uD800x"
  expectDigest("x\xED\xB0\x80", "e9fd2056ca5b0508", "e9fd2056ca5b");  // "x\uDC00"
}
