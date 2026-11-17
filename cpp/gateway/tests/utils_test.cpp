// Port of tests/utils.test.ts, plus differential cases recorded from src/utils.ts under Bun 1.4.0.
#include "uos/utils.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using Bytes = std::vector<std::uint8_t>;

TEST("base64 helpers tolerate whitespace") {
  EXPECT_EQ(uos::utils::decodeBase64ToString(" aG\nk= "), "hi");
  EXPECT_EQ(rt::decodeUtf8(uos::utils::base64UrlDecode(" aG\nk ")), "hi");
}

TEST("differential: decodeBase64ToString") {
  EXPECT_EQ(uos::utils::decodeBase64ToString("aGk="), "hi");
  EXPECT_EQ(uos::utils::decodeBase64ToString(" aGk=　"), "hi");
  EXPECT_EQ(uos::utils::decodeBase64ToString("4pyTIMOgIGxhIG1vZGU="), "✓ à la mode");
  EXPECT_EQ(uos::utils::decodeBase64ToString("/w=="), "�");
  EXPECT_EQ(uos::utils::decodeBase64ToString("aGk"), "hi");
  EXPECT_EQ(uos::utils::decodeBase64ToString("  "), "");
  EXPECT_EQ(uos::utils::decodeBase64ToString(""), "");
  EXPECT_THROWS_MESSAGE(uos::utils::decodeBase64ToString("a"), rt::InvalidCharacterError, "The string contains invalid characters.");
  EXPECT_THROWS_MESSAGE(uos::utils::decodeBase64ToString("a==="), rt::InvalidCharacterError, "The string contains invalid characters.");
  EXPECT_THROWS_MESSAGE(uos::utils::decodeBase64ToString("aGk=x"), rt::InvalidCharacterError, "The string contains invalid characters.");
}

TEST("differential: encodeHex, base64UrlEncode and bytesToBase64") {
  EXPECT_EQ(uos::utils::encodeHex(Bytes{}), "");
  EXPECT_EQ(uos::utils::encodeHex(Bytes{0}), "00");
  EXPECT_EQ(uos::utils::encodeHex(Bytes{1, 255, 16, 171}), "01ff10ab");
  EXPECT_EQ(uos::utils::base64UrlEncode(Bytes{}), "");
  EXPECT_EQ(uos::utils::base64UrlEncode(Bytes{251}), "-w");
  EXPECT_EQ(uos::utils::base64UrlEncode(Bytes{251, 255}), "-_8");
  EXPECT_EQ(uos::utils::base64UrlEncode(Bytes{251, 255, 191}), "-_-_");
  EXPECT_EQ(uos::utils::base64UrlEncode(Bytes{0, 1, 2, 3, 4}), "AAECAwQ");
  EXPECT_EQ(uos::utils::base64UrlEncode(Bytes{255, 254, 253, 252, 251, 250}), "__79_Pv6");
  EXPECT_EQ(uos::utils::bytesToBase64(Bytes{}), "");
  EXPECT_EQ(uos::utils::bytesToBase64(Bytes{0}), "AA==");
  EXPECT_EQ(uos::utils::bytesToBase64(Bytes{255, 0, 128}), "/wCA");
  EXPECT_EQ(uos::utils::bytesToBase64(Bytes{0, 1, 2, 3, 4, 5, 6}), "AAECAwQFBg==");
  Bytes large(70000);
  for (std::size_t index = 0; index < large.size(); ++index) large[index] = static_cast<std::uint8_t>(index % 251);
  EXPECT_EQ(uos::utils::bytesToBase64(large), rt::toBase64(large));
}

TEST("differential: base64UrlDecode") {
  EXPECT_EQ(uos::utils::base64UrlDecode("-_8"), (Bytes{251, 255}));
  EXPECT_EQ(uos::utils::base64UrlDecode("-_-_"), (Bytes{251, 255, 191}));
  EXPECT_EQ(uos::utils::base64UrlDecode(""), Bytes{});
  EXPECT_EQ(uos::utils::base64UrlDecode("  YQ  "), Bytes{97});
  EXPECT_EQ(uos::utils::base64UrlDecode("YQ=="), Bytes{97});
  EXPECT_EQ(uos::utils::base64UrlDecode("YQ="), Bytes{97});
  EXPECT_EQ(uos::utils::base64UrlDecode("Y-_Q_-Y"), (Bytes{99, 239, 208, 255, 230}));
  EXPECT_THROWS(uos::utils::base64UrlDecode("abcde"), rt::InvalidCharacterError);
  EXPECT_THROWS(uos::utils::base64UrlDecode("a"), rt::InvalidCharacterError);
  EXPECT_THROWS(uos::utils::base64UrlDecode("YéQ"), rt::InvalidCharacterError);
}

TEST("differential: base64ToBytes") {
  EXPECT_EQ(uos::utils::base64ToBytes("YQ=="), std::optional<Bytes>(Bytes{97}));
  EXPECT_EQ(uos::utils::base64ToBytes("YQ"), std::optional<Bytes>(Bytes{97}));
  EXPECT_EQ(uos::utils::base64ToBytes("Y"), std::nullopt);
  EXPECT_EQ(uos::utils::base64ToBytes("Y Q = ="), std::optional<Bytes>(Bytes{97}));
  EXPECT_EQ(uos::utils::base64ToBytes("@@"), std::nullopt);
  EXPECT_EQ(uos::utils::base64ToBytes(""), std::optional<Bytes>(Bytes{}));
  EXPECT_EQ(uos::utils::base64ToBytes("ÿQ=="), std::nullopt);
}

TEST("differential: isRecord and getString") {
  EXPECT(!uos::utils::isRecord(json::value(nullptr)));
  EXPECT(!uos::utils::isRecord(json::value(1)));
  EXPECT(!uos::utils::isRecord(json::value("x")));
  EXPECT(uos::utils::isRecord(json::value(json::array{})));
  EXPECT(uos::utils::isRecord(json::value(json::object{})));
  EXPECT(!uos::utils::isRecord(json::value(true)));
  EXPECT(!uos::utils::isRecord(static_cast<const json::value*>(nullptr)));
  EXPECT_EQ(uos::utils::getString(json::value(nullptr)), std::nullopt);
  EXPECT_EQ(uos::utils::getString(json::value(1)), std::nullopt);
  EXPECT_EQ(uos::utils::getString(json::value("x")), std::optional<std::string>("x"));
  EXPECT_EQ(uos::utils::getString(json::value(json::array{})), std::nullopt);
  EXPECT_EQ(uos::utils::getString(static_cast<const json::value*>(nullptr)), std::nullopt);
}

ASYNC_TEST("differential: sha256Hex and sha256Base64Url") {
  const std::vector<std::vector<std::string>> vectors = {
    {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "47DEQpj8HBSa-_TImW-5JCeuQeRkm5NMpJWZG3hSuFU"},
    {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "ungWv48Bz-pBQUDeXa4iI7ADYaOWF3qctBD_YfIAFa0"},
    {"héllo \U0001F600", "a53c56966616f0ec3ed9db7ea07a6034430149528bbaba8966c284f9373669c2", "pTxWlmYW8Ow-2dt-oHpgNEMBSVKLurqJZsKE-Tc2acI"},
    {std::string("\xED\xA0\x80x"), "a87ce46793cc659b79bd187e998f8488635f707d5be0957e1d565981efd2b04b", "qHzkZ5PMZZt5vRh-mY-EiGNfcH1b4JV-HVZZge_SsEs"},
  };
  for (const auto& vector : vectors) {
    const std::string input = vector[0];
    const std::string hex = co_await uos::utils::sha256Hex(input);
    const std::string url = co_await uos::utils::sha256Base64Url(input);
    EXPECT_EQ(hex, vector[1]);
    EXPECT_EQ(url, vector[2]);
  }
}
