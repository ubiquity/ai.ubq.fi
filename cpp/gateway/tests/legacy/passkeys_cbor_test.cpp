// Links the sources of tests/passkeys_test.cpp but passkeys.cpp.
#include "src/auth/passkeys.cpp"
#include "uos/rt/test.hpp"

namespace passkeys = uos::auth::passkeys;
namespace rt = uos::rt;

using passkeys::Cbor;
using passkeys::isoCBOR::decodeFirst;
using passkeys::isoCBOR::encode;

TEST("libcbor reads the first complete item and serializes its wire representation") {
  const auto decoded = passkeys::isoCBOR::decode(rt::fromHex("a2012603636b6579ff"), passkeys::isoCBOR::Use::standalone);
  EXPECT_EQ(decoded.read, std::size_t{8});
  EXPECT_EQ(decoded.item.get(1).number(), -7.0);
  EXPECT_EQ(decoded.item.get(3).text(), "key");
  EXPECT_EQ(rt::toHex(encode(decoded.item)), "a2012603636b6579");
}

TEST("libcbor owns child values after releasing their containing map") {
  Cbor child = decodeFirst(rt::fromHex("a1616b824101f5")).get("k");
  EXPECT_EQ(child.arraySize(), std::size_t{2});
  EXPECT_EQ(rt::toHex(child.at(0).bytes()), "01");
  EXPECT(child.at(1).boolean());
  EXPECT(!decodeFirst(rt::fromHex("a0")).get("missing"));
}

TEST("libcbor preserves float widths and counts UTF-8 bytes in ordinary CBOR text") {
  for (const auto* hex : {"fa3fc00000", "fb3ff8000000000000"}) {
    const Cbor number = decodeFirst(rt::fromHex(hex));
    EXPECT_EQ(number.number(), 1.5);
    EXPECT_EQ(rt::toHex(encode(number)), hex);
  }
  EXPECT_EQ(rt::toHex(encode(decodeFirst(rt::fromHex("f97e00")))), "f97e00");
  EXPECT_EQ(rt::toHex(encode(decodeFirst(rt::fromHex("62c3a9")))), "62c3a9");
  EXPECT_EQ(rt::toHex(encode(decodeFirst(rt::fromHex("c101")))), "c101");
  EXPECT_EQ(rt::toHex(encode(decodeFirst(rt::fromHex("190018")))), "190018");
}

TEST("passkey decoding rejects duplicate and unsupported map keys at any nesting level") {
  for (const auto* hex : {"a201020103", "a20102fa3f80000003", "a20002fa8000000003",
                          "a2f97e0002fa7fc0000003", "81c1a2616102616103", "a1f401"}) {
    EXPECT_THROWS(decodeFirst(rt::fromHex(hex)), rt::JsError);
  }
}

TEST("passkey decoding rejects unsupported CBOR forms and invalid text") {
  for (const auto* hex : {"", "1817", "3817", "5817", "7800", "9800", "b800", "d81700",
                          "1b0020000000000000", "3b001fffffffffffff", "9f01ff",
                          "bf0102ff", "5f4100ff", "7f6161ff", "f820", "f93e00", "f97e01",
                          "61ff", "64efbbbf61", "430102", "a101", "a101430102",
                          "9bffffffffffffffff", "bb7fffffffffffffff"}) {
    EXPECT_THROWS(decodeFirst(rt::fromHex(hex)), rt::JsError);
  }
  const auto safe = decodeFirst(rt::fromHex("1b001fffffffffffff"));
  EXPECT_EQ(safe.number(), 9007199254740991.0);
}

TEST("passkey decoding bounds nesting before using the tree recursively") {
  passkeys::Bytes tags(64, 0xc1);
  tags.push_back(1);
  EXPECT_EQ(encode(decodeFirst(tags)), tags);
  tags.insert(tags.begin(), 0xc1);
  EXPECT_THROWS(decodeFirst(tags), rt::JsError);
}

TEST("authenticator data consumes the credential and extension at their wire offsets") {
  passkeys::Bytes authData(55, 0);
  authData[32] = 0xc1;
  const auto cose = rt::fromHex("a3010203262001");
  const auto extensions = rt::fromHex("a16375766d80");
  authData.insert(authData.end(), cose.begin(), cose.end());
  authData.insert(authData.end(), extensions.begin(), extensions.end());
  const auto parsed = passkeys::parseAuthenticatorData(authData);
  EXPECT_EQ(parsed.credentialPublicKey, std::optional<passkeys::Bytes>(cose));
  EXPECT(parsed.flags.ed);
  authData.push_back(0);
  EXPECT_THROWS(passkeys::parseAuthenticatorData(authData), rt::JsError);
}

TEST("authenticator data refuses representations whose wire and legacy boundaries could disagree") {
  for (const auto* hex : {"a1616b190018", "a1616bfa3f800000", "a162c3a901"}) {
    passkeys::Bytes authData(37, 0);
    authData[32] = 0x80;
    const auto extension = rt::fromHex(hex);
    authData.insert(authData.end(), extension.begin(), extension.end());
    EXPECT_THROWS(passkeys::parseAuthenticatorData(authData), rt::JsError);
  }
}
