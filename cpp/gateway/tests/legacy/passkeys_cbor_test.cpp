// Exercise the production codec used by attestation and authenticator data.
// Links: tests/helpers/passkeys_harness.cpp src/auth/passkeys.cpp src/auth/relay.cpp src/http.cpp src/request.cpp
//   src/utils.cpp src/kv.cpp src/storage/kv.cpp src/release.cpp src/storage/sqlite_readonly.cpp
//   src/storage/native_kv.cpp src/config.cpp src/provider/cerebras_rate_limits.cpp
#include "uos/rt/base64.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/test.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace uos::auth::passkeys::cbor {
  std::pair<std::vector<std::uint8_t>, std::size_t> reencode(std::span<const std::uint8_t> input, bool requireComplete);
}

namespace cbor = uos::auth::passkeys::cbor;
namespace rt = uos::rt;

namespace {
  void rejects(std::string_view hex) {
    const auto bytes = rt::fromHex(hex);
    EXPECT_THROWS(cbor::reencode(bytes, true), rt::JsError);
  }
}

TEST("CTAP2 sorts integer and text labels by major type, length and bytes") {
  const auto bytes = rt::fromHex("a501f438186022406162f5627a7af4");
  // The sequence is 1, -25, -3 only if sorted by numeric value. CTAP2 orders -3 before -25.
  EXPECT_THROWS(cbor::reencode(bytes, true), rt::JsError);
  const auto canonical = rt::fromHex("a501f422403818606162f5627a7af4");
  const auto [encoded, consumed] = cbor::reencode(canonical, true);
  EXPECT_EQ(consumed, canonical.size());
  EXPECT_EQ(encoded, canonical);
}

TEST("text lengths count UTF-8 bytes") {
  const auto bytes = rt::fromHex("a162c3a963e282ac");
  EXPECT_EQ(cbor::reencode(bytes, true).first, bytes);
}

TEST("rejects indefinite containers and strings") {
  rejects("9f00ff");
  rejects("bf0000ff");
  rejects("5f4100ff");
  rejects("7f6161ff");
  rejects("a16161819fff");
}

TEST("rejects non-minimal integer arguments") {
  rejects("1817");
  rejects("3817");
  rejects("190018");
  rejects("1a00000100");
  rejects("1b0000000000010000");
  rejects("a1181700");
}

TEST("rejects non-minimal byte, text, array and map lengths") {
  rejects("5800");
  rejects("780161");
  rejects("980100");
  rejects("b8010000");
  rejects("a178016100");
}

TEST("rejects duplicate map labels, including in nested maps") {
  rejects("a201000101");
  rejects("a2616100616101");
  rejects("81a220002001");
  rejects("a261610064efbbbf6101");
}

TEST("rejects out-of-order map labels") {
  rejects("a220000100");
  rejects("a202000100");
  rejects("a2616200616100");
  rejects("a262616100617a00");
  rejects("81a26161000100");
}

TEST("rejects tags, unused simple values and floats on the COSE alg label") {
  rejects("c000");
  rejects("d8184100");
  rejects("a1c00100");
  rejects("f6");
  rejects("f7");
  rejects("f820");
  rejects("a103f9c700");
  rejects("a103fac0e00000");
  rejects("a103fbc01c000000000000");
}

TEST("rejects malformed, truncated and unsupported items") {
  rejects("");
  rejects("4100ff");
  rejects("4200");
  rejects("6261");
  rejects("61c3");
  rejects("a100");
  rejects("a14000");
  rejects("a1810000");
  rejects("1b0020000000000000");
  rejects("3b0020000000000000");
  rejects("1bffffffffffffffff");
  rejects("1c");
}

TEST("accepts eight nested containers and rejects nine") {
  const auto eight = rt::fromHex("8181818181818180");
  EXPECT_EQ(cbor::reencode(eight, true).first, eight);
  rejects("818181818181818180");
  rejects("8181818181818181a0");
}

TEST("bounds the input before a partial or full decode") {
  auto atLimit = rt::fromHex("5a000ffffb");
  atLimit.resize(rt::limits::kCborBytes);
  EXPECT_EQ(cbor::reencode(atLimit, true).first, atLimit);
  atLimit.push_back(0);
  EXPECT_THROWS(cbor::reencode(atLimit, false), rt::JsError);
}

TEST("partial decoding leaves the next item and full decoding rejects trailing data") {
  const auto bytes = rt::fromHex("a10326a16161f5");
  const auto [key, consumed] = cbor::reencode(bytes, false);
  ASSERT_EQ(consumed, std::size_t{3});
  EXPECT_EQ(key, rt::fromHex("a10326"));
  const auto extensions = cbor::reencode(std::span(bytes).subspan(consumed), true);
  EXPECT_EQ(extensions.first, rt::fromHex("a16161f5"));
  EXPECT_EQ(extensions.second, std::size_t{4});
  EXPECT_THROWS(cbor::reencode(bytes, true), rt::JsError);
}

TEST("round-trips integers at width and supported range boundaries") {
  const auto bytes = rt::fromHex("8917181818ff19010019ffff1a000100001b00000001000000001b001fffffffffffff3b001fffffffffffff");
  EXPECT_EQ(cbor::reencode(bytes, true).first, bytes);
}
