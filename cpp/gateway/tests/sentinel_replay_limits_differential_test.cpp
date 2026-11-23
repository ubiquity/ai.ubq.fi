// Differential cases for src/sentinel/replay-limits.ts (its direct bun tests mostly need unported
// replay modules): every exported limit and both charge functions over edge inputs, recorded from
// the TypeScript module under Bun 1.4.0 (tests/sentinel_replay_limits_differential_vectors.inc).
#include "uos/sentinel/replay_limits.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "sentinel_replay_limits_differential_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::sentinel::replay_limits;

namespace {
  // The derived limits are constant expressions that other modules' constants build on.
  static_assert(SENTINEL_REPLAY_METADATA_DERIVED_BYTES == 5'876'400);
  static_assert(SENTINEL_REPLAY_MAX_STORED_CHUNKS == 875);
  static_assert(SENTINEL_REPLAY_MAX_CIPHERTEXT_BYTES == 42'991'636);

  double numberFrom(const json::value& value) {
    if (value.is_string()) {
      const std::string text(value.get_string());
      if (text == "NaN") return rt::Number::NaN;
      return text == "Infinity" ? rt::Number::POSITIVE_INFINITY : rt::Number::NEGATIVE_INFINITY;
    }
    return *rt::getNumber(value);
  }

  void expectSameNumber(const std::string& label, double actual, double recorded) {
    if (!rt::sameValue(actual, recorded)) EXPECT_EQ(label + " " + rt::numberToString(actual), label + " " + rt::numberToString(recorded));
  }
}

TEST("differential: every exported limit equals the TypeScript value") {
  const json::value recorded = rt::jsonParse(kReplayLimitsVectorsJson);
  const json::object& constants = *rt::getObject(recorded, "constants");
  const std::vector<std::pair<std::string, double>> mine = {
    {"SENTINEL_REPLAY_MAX_REQUEST_BYTES", SENTINEL_REPLAY_MAX_REQUEST_BYTES},
    {"SENTINEL_REPLAY_MAX_UPSTREAM_BYTES", SENTINEL_REPLAY_MAX_UPSTREAM_BYTES},
    {"SENTINEL_REPLAY_MAX_UPSTREAM_CHUNKS", SENTINEL_REPLAY_MAX_UPSTREAM_CHUNKS},
    {"SENTINEL_REPLAY_MAX_UPSTREAM_ATTEMPTS", SENTINEL_REPLAY_MAX_UPSTREAM_ATTEMPTS},
    {"SENTINEL_REPLAY_MAX_DOWNSTREAM_BYTES", SENTINEL_REPLAY_MAX_DOWNSTREAM_BYTES},
    {"SENTINEL_REPLAY_STORAGE_CHUNK_BYTES", SENTINEL_REPLAY_STORAGE_CHUNK_BYTES},
    {"SENTINEL_REPLAY_JSON_ESCAPE_FACTOR", SENTINEL_REPLAY_JSON_ESCAPE_FACTOR},
    {"SENTINEL_REPLAY_METADATA_DERIVED_BYTES", SENTINEL_REPLAY_METADATA_DERIVED_BYTES},
    {"SENTINEL_REPLAY_MAX_METADATA_BYTES", SENTINEL_REPLAY_MAX_METADATA_BYTES},
    {"SENTINEL_REPLAY_MAX_PLAINTEXT_BYTES", SENTINEL_REPLAY_MAX_PLAINTEXT_BYTES},
    {"SENTINEL_REPLAY_MAX_CIPHERTEXT_BYTES", SENTINEL_REPLAY_MAX_CIPHERTEXT_BYTES},
    {"SENTINEL_REPLAY_MAX_STORED_CHUNKS", SENTINEL_REPLAY_MAX_STORED_CHUNKS},
    {"SENTINEL_REPLAY_REQUEST_FILE_MAX_BYTES", SENTINEL_REPLAY_REQUEST_FILE_MAX_BYTES},
    {"SENTINEL_REPLAY_UPSTREAM_FILE_MAX_BYTES", SENTINEL_REPLAY_UPSTREAM_FILE_MAX_BYTES},
    {"SENTINEL_REPLAY_BUDGET_BYTES", SENTINEL_REPLAY_BUDGET_BYTES},
    {"SENTINEL_REPLAY_MAX_RECORDS", SENTINEL_REPLAY_MAX_RECORDS},
    {"SENTINEL_REPLAY_METADATA_RESERVE_BYTES", SENTINEL_REPLAY_METADATA_RESERVE_BYTES},
    {"SENTINEL_REPLAY_MAX_STATUS_RECORDS", SENTINEL_REPLAY_MAX_STATUS_RECORDS},
    {"SENTINEL_REPLAY_RECORD_OVERHEAD_BYTES", SENTINEL_REPLAY_RECORD_OVERHEAD_BYTES},
    {"SENTINEL_REPLAY_LEGACY_METADATA_BYTES", SENTINEL_REPLAY_LEGACY_METADATA_BYTES},
  };
  EXPECT_EQ(mine.size(), constants.size());
  for (const auto& [name, value] : mine) {
    const json::value* expected = rt::get(constants, name);
    if (expected == nullptr) {
      EXPECT_EQ(name, std::string("<not exported by the TypeScript module>"));
      continue;
    }
    expectSameNumber(name, value, numberFrom(*expected));
  }
}

TEST("differential: stored charge and reservation over edge inputs") {
  const json::value recorded = rt::jsonParse(kReplayLimitsVectorsJson);
  std::vector<double> inputs;
  for (const json::value& input : *rt::getArray(recorded, "inputs")) inputs.push_back(numberFrom(input));
  const json::array& stored = *rt::getArray(recorded, "stored");
  const json::array& reservation = *rt::getArray(recorded, "reservation");
  ASSERT_EQ(stored.size(), inputs.size() * inputs.size());
  std::size_t index = 0;
  for (const double a : inputs)
    for (const double b : inputs) {
      const std::string label = "(" + rt::numberToString(a) + ", " + rt::numberToString(b) + ")";
      expectSameNumber("stored" + label, sentinelReplayStoredCharge(a, b), numberFrom(stored[index]));
      expectSameNumber("reservation" + label, sentinelReplayReservationBytes(a, b), numberFrom(reservation[index]));
      index += 1;
    }
}

TEST("differential: the constant-expression Math.ceil agrees with rt::Math::ceil") {
  std::vector<double> values = {0.0, -0.0, 0.5, -0.5, 1.0, -1.0, 1.5, -1.5, 2.0 / 3, -2.0 / 3, 4503599627370495.5, -4503599627370495.5, 4503599627370496.0,
                                9007199254740993.0, 1e300, -1e300, std::numeric_limits<double>::denorm_min(), -std::numeric_limits<double>::denorm_min(),
                                rt::Number::NaN, rt::Number::POSITIVE_INFINITY, rt::Number::NEGATIVE_INFINITY, 41943044.0 / 3, 42991636.0 / 49152};
  std::mt19937_64 random(429);
  std::uniform_real_distribution<double> spread(-1e6, 1e6);
  for (int count = 0; count < 20'000; ++count) values.push_back(spread(random));
  for (int count = 0; count < 2'000; ++count) values.push_back(static_cast<double>(static_cast<std::int64_t>(random() >> 11)) / 7.0);
  for (const double value : values) {
    if (!rt::sameValue(detail::ceil(value), rt::Math::ceil(value))) EXPECT_EQ(rt::numberToString(value) + " -> " + rt::numberToString(detail::ceil(value)), rt::numberToString(rt::Math::ceil(value)));
    if (!rt::sameValue(detail::base64Chars(value), rt::Math::ceil(value / 3) * 4)) EXPECT_EQ(rt::numberToString(value), std::string("base64Chars mismatch"));
  }
}
