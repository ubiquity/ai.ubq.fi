// src/api-keys.ts has no direct bun test: differential cases recorded from the TS under Bun 1.4.0.
#include "uos/api_keys.hpp"

#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test.hpp"

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace k = uos::api_keys;

namespace {
  const double NaN = std::numeric_limits<double>::quiet_NaN();
  const double Inf = std::numeric_limits<double>::infinity();
  // Number expectations: nullopt is null; "-0" is distinguished from 0.
  struct NumberCase {
    double input;
    std::optional<double> normalized;
    std::optional<double> microcredits;
    double credits;
    double windowMs;
    double windowMsFallback5;
  };
  bool same(std::optional<double> actual, std::optional<double> expected) {
    if (!actual || !expected) return !actual && !expected;
    return rt::sameValue(*actual, *expected);
  }
}

TEST("differential: numeric normalizers over JS numbers") {
  const std::vector<NumberCase> cases = {
    {-1, std::nullopt, -1, -1, 604800000, 5},
    {-0.0, -0.0, -0.0, -0.0, 604800000, 5},
    {0, 0, 0, 0, 604800000, 5},
    {0.5, std::nullopt, 500000, 0, 604800000, 5},
    {1, 1, 1000000, 0.000001, 1, 1},
    {1.4999995, std::nullopt, 1500000, 0, 1, 1},
    {2.0000004, std::nullopt, 2000000, 0, 2, 2},
    {1e-7, std::nullopt, 0, 0, 604800000, 5},
    {1e9, 1e9, 1e15, 1000, 1e9, 1e9},
    {9007199254740991.0, 9007199254740991.0, std::nullopt, 9007199254.740992, 9007199254740991.0, 9007199254740991.0},
    {9007199254740992.0, std::nullopt, std::nullopt, 0, 9007199254740992.0, 9007199254740992.0},
    {9.007199254740993, std::nullopt, 9007199, 0, 9, 9},
    {-0.5, std::nullopt, std::nullopt, 0, 604800000, 5},
    {NaN, std::nullopt, std::nullopt, 0, 604800000, 5},
    {Inf, std::nullopt, std::nullopt, 0, 604800000, 5},
    {-Inf, std::nullopt, std::nullopt, 0, 604800000, 5},
  };
  for (const NumberCase& entry : cases) {
    EXPECT(same(k::normalizePaidFallbackMicrocredits(entry.input), entry.normalized));
    EXPECT(same(k::paidFallbackCreditsToMicrocredits(entry.input), entry.microcredits));
    EXPECT(rt::sameValue(k::paidFallbackMicrocreditsToCredits(entry.input), entry.credits));
    EXPECT(rt::sameValue(k::normalizeApiKeyWindowMs(entry.input), entry.windowMs));
    EXPECT(rt::sameValue(k::normalizeApiKeyWindowMs(entry.input, 5), entry.windowMsFallback5));
    const json::value asJson = rt::makeNumber(entry.input);
    EXPECT(same(k::normalizePaidFallbackMicrocredits(&asJson), entry.normalized));
    EXPECT(same(k::paidFallbackCreditsToMicrocredits(asJson), entry.microcredits));
    EXPECT(rt::sameValue(k::paidFallbackMicrocreditsToCredits(asJson), entry.credits));
    EXPECT(rt::sameValue(k::normalizeApiKeyWindowMs(asJson, 5), entry.windowMsFallback5));
  }
}

TEST("differential: numeric normalizers over non-numbers") {
  const std::vector<json::value> values = {nullptr, json::value("5"), json::value(true), json::array{}, json::object{}};
  for (const json::value& value : values) {
    EXPECT_EQ(k::normalizePaidFallbackMicrocredits(value), std::nullopt);
    EXPECT_EQ(k::paidFallbackCreditsToMicrocredits(value), std::nullopt);
    EXPECT(rt::sameValue(k::paidFallbackMicrocreditsToCredits(value), 0));
    EXPECT_EQ(k::normalizeApiKeyWindowMs(value), 604800000.0);
    EXPECT_EQ(k::normalizeApiKeyWindowMs(value, 5), 5.0);
  }
  const json::value* undefined = nullptr;
  EXPECT_EQ(k::normalizePaidFallbackMicrocredits(undefined), std::nullopt);
  EXPECT_EQ(k::paidFallbackCreditsToMicrocredits(undefined), std::nullopt);
  EXPECT(rt::sameValue(k::paidFallbackMicrocreditsToCredits(undefined), 0));
  EXPECT_EQ(k::normalizeApiKeyWindowMs(undefined), 604800000.0);
}

TEST("differential: record coercion") {
  struct RecordCase {
    std::string json;
    double expiresAtMs;
    double windowMs;
    double windowMsFallback7;
  };
  const std::vector<RecordCase> cases = {
    {"null", -1, 604800000, 7},
    {"5", -1, 604800000, 7},
    {"[]", -1, 604800000, 7},
    {"{}", -1, 604800000, 7},
    {R"({"expires_at_ms":10.9})", 10, 604800000, 7},
    {R"({"expires_at_ms":-1.5})", -1, 604800000, 7},
    {R"({"expires_at_ms":-0.5})", -0.0, 604800000, 7},
    {R"({"expires_at_ms":-2})", -1, 604800000, 7},
    {R"({"expires_at_ms":"5"})", -1, 604800000, 7},
    {R"({"window_ms":0.5})", -1, 604800000, 7},
    {R"({"window_ms":1.5})", -1, 1, 1},
    {R"({"window_ms":-3})", -1, 604800000, 7},
    {R"({"window_ms":60000.7})", -1, 60000, 60000},
    {R"({"expires_at_ms":1700000000000,"window_ms":3600000})", 1700000000000, 3600000, 3600000},
  };
  for (const RecordCase& entry : cases) {
    const json::value record = rt::jsonParse(entry.json);
    EXPECT(rt::sameValue(k::coerceApiKeyExpiresAtMs(record), entry.expiresAtMs));
    EXPECT(rt::sameValue(k::coerceApiKeyWindowMs(record), entry.windowMs));
    EXPECT(rt::sameValue(k::coerceApiKeyWindowMs(&record, 7), entry.windowMsFallback7));
    if (record.is_object()) {
      EXPECT(rt::sameValue(k::coerceApiKeyExpiresAtMs(record.get_object()), entry.expiresAtMs));
      EXPECT(rt::sameValue(k::coerceApiKeyWindowMs(record.get_object(), 7), entry.windowMsFallback7));
    }
  }
  json::object infinite;
  infinite["expires_at_ms"] = std::numeric_limits<double>::infinity();
  EXPECT_EQ(k::coerceApiKeyExpiresAtMs(infinite), -1.0);
  const json::value* undefined = nullptr;
  EXPECT_EQ(k::coerceApiKeyExpiresAtMs(undefined), -1.0);
  EXPECT_EQ(k::coerceApiKeyWindowMs(undefined), 604800000.0);
}

TEST("differential: keys, constants, clocks and token shape") {
  EXPECT_EQ(k::calculateNextResetMs(1000), 604801000.0);
  EXPECT_EQ(k::calculateNextResetMs(1000, 5), 1005.0);
  EXPECT_EQ(k::getDefaultExpiryMs(1700000000000), 1707776000000.0);
  EXPECT_EQ(k::USAGE_RESET_PERIOD_MS, 604800000.0);
  EXPECT(k::apiKeyIdKey("x") == (rt::KvKey{"ubq_ai", "api_keys", "id", "x"}));
  EXPECT(k::apiKeyHashKey("h") == (rt::KvKey{"ubq_ai", "api_keys", "hash", "h"}));
  static const rt::Regex TOKEN("^u_[0-9a-f]{64}$");
  const std::string token = k::generateApiKeyToken();
  EXPECT_EQ(token.size(), std::size_t{66});
  EXPECT(TOKEN.test(token));
  rt::crypto::ScopedRandomSource source([](std::span<std::uint8_t> bytes) {
    for (std::size_t index = 0; index < bytes.size(); ++index) bytes[index] = static_cast<std::uint8_t>(index);
  });
  EXPECT_EQ(k::generateApiKeyToken(), "u_000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
}
