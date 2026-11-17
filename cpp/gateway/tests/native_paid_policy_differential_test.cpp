// Differential cases for src/authority/native-paid-policy.ts, recorded from Bun 1.4.0
// running the TypeScript module (tests/native_paid_policy_differential_vectors.inc).
//
// Its bun test file, tests/native-principal-policy.test.ts, also needs
// src/authority/native-api-key-policy.ts, src/api-key-policy.ts, src/admin/api-keys.ts and
// src/paid-fallback/index.ts (not ported), so neither of its cases is ported yet. The
// native-paid-policy assertions of its shared vectors are reproduced below, with the records
// those modules build (apiKeyHashRecordFor, buildApiKeyCreateRecords) recorded from Bun.
#include "uos/authority/native_paid_policy.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"
#include "uos/types.hpp"

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "native_paid_policy_differential_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
namespace types = uos::types;
using namespace uos::authority::native_paid_policy;

namespace {
  constexpr double NOW = 1'800'000'000'000;

  types::ApiKeyRecord policyFixture() {
    return {
      .id = "synthetic-key",
      .name = "original",
      .prefix = "u_synthetic",
      .hash = "synthetic-hash",
      .created_at_ms = NOW,
      .expires_at_ms = -1,
      .revoked_at_ms = std::nullopt,
      .usage_limit_requests = 3,
      .usage_requests = 2,
      .usage_reset_at_ms = 1'800'000'060'000,
      .window_ms = 60'000,
      .usage_quota_version = 3,
      .paid_fallback_enabled = false,
      .paid_fallback_limit_microcredits = 0,
      .paid_fallback_spent_microcredits = 0,
      .paid_fallback_reserved_microcredits = 0,
      .paid_fallback_reservation_request_id = std::nullopt,
      .paid_fallback_model_ids = {},
      .paid_fallback_quota_per_credit = 0,
      .paid_fallback_max_exposure_microcredits = json::object(),
      .paid_fallback_pricing_checked_at_ms = std::nullopt,
    };
  }

  // policy.apiKeyHashRecordFor(policyFixture())
  const json::value HASH = rt::jsonParse(
    R"({"id":"synthetic-key","expires_at_ms":-1,"revoked_at_ms":null,"usage_limit_requests":3,"usage_requests":2,"usage_reset_at_ms":1800000060000,)"
    R"("window_ms":60000,"usage_quota_version":3,"paid_fallback_enabled":false,"paid_fallback_limit_microcredits":0,"paid_fallback_spent_microcredits":0,)"
    R"("paid_fallback_reserved_microcredits":0,"paid_fallback_reservation_request_id":null})");

  // policy.buildApiKeyCreateRecords(identity, fields, { ...paidFields, ...ID-only policy }, now) of the shared vectors.
  const json::value CREATED_RECORD = rt::jsonParse(
    R"({"id":"synthetic-key","name":"original","prefix":"u_synthetic","hash":"synthetic-hash","created_at_ms":1800000000000,"expires_at_ms":-1,)"
    R"("revoked_at_ms":null,"usage_limit_requests":0,"usage_requests":0,"usage_reset_at_ms":1800000060000,"window_ms":60000,"usage_quota_version":3,)"
    R"("paid_fallback_enabled":false,"paid_fallback_limit_microcredits":0,"paid_fallback_spent_microcredits":0,"paid_fallback_reserved_microcredits":0,)"
    R"("paid_fallback_reservation_request_id":null,"paid_fallback_model_ids":[],"paid_fallback_quota_per_credit":0,)"
    R"("paid_fallback_max_exposure_microcredits":{},"paid_fallback_pricing_checked_at_ms":null})");
  const json::value CREATED_HASH_RECORD = rt::jsonParse(
    R"({"id":"synthetic-key","expires_at_ms":-1,"revoked_at_ms":null,"usage_limit_requests":0,"usage_requests":0,"usage_reset_at_ms":1800000060000,)"
    R"("window_ms":60000,"usage_quota_version":3,"paid_fallback_enabled":false,"paid_fallback_limit_microcredits":0,"paid_fallback_spent_microcredits":0,)"
    R"("paid_fallback_reserved_microcredits":0,"paid_fallback_reservation_request_id":null})");

  // { ...base, key: value }
  json::value with(const json::value& base, std::string_view key, json::value value) {
    json::object copy = rt::objectSpread(base.as_object());
    rt::objectDefine(copy, key, std::move(value));
    return copy;
  }

  bool holdsNegativeZero(const json::value& value) {
    if (value.is_double()) return value.get_double() == 0 && std::signbit(value.get_double());
    if (const json::object* object = value.if_object()) {
      for (const auto& member : *object)
        if (holdsNegativeZero(member.value())) return true;
    }
    if (const json::array* array = value.if_array()) {
      for (const json::value& item : *array)
        if (holdsNegativeZero(item)) return true;
    }
    return false;
  }
}

TEST("paidFallbackHashFields projects the five hash-row fields in the TS literal's order") {
  const PaidFallbackHashFields fields = paidFallbackHashFields(policyFixture());
  EXPECT_EQ(rt::jsonStringify(json::value_from(fields)),
            std::string(R"({"paid_fallback_enabled":false,"paid_fallback_limit_microcredits":0,"paid_fallback_spent_microcredits":0,)"
                        R"("paid_fallback_reserved_microcredits":0,"paid_fallback_reservation_request_id":null})"));
  EXPECT(json::value_to<PaidFallbackHashFields>(json::value_from(fields)) == fields);

  types::ApiKeyRecord populated = policyFixture();
  populated.paid_fallback_enabled = true;
  populated.paid_fallback_limit_microcredits = -1;
  populated.paid_fallback_spent_microcredits = 5;
  populated.paid_fallback_reserved_microcredits = 7.5;
  populated.paid_fallback_reservation_request_id = "req";
  EXPECT_EQ(rt::jsonStringify(json::value_from(paidFallbackHashFields(populated))),
            std::string(R"({"paid_fallback_enabled":true,"paid_fallback_limit_microcredits":-1,"paid_fallback_spent_microcredits":5,)"
                        R"("paid_fallback_reserved_microcredits":7.5,"paid_fallback_reservation_request_id":"req"})"));
  EXPECT_THROWS(json::value_to<PaidFallbackHashFields>(rt::jsonParse(R"({"paid_fallback_enabled":"false"})")), rt::TypeError);
}

TEST("apiKeyHashRecordFor spreads the projection after the quota fields") {
  // `{ id, expires_at_ms, ..., usage_quota_version, ...paidFallbackHashFields(record) }`
  const types::ApiKeyRecord record = policyFixture();
  const PaidFallbackHashFields paid = paidFallbackHashFields(record);
  const types::ApiKeyHashRecord hash{
    .id = record.id,
    .expires_at_ms = record.expires_at_ms,
    .revoked_at_ms = record.revoked_at_ms,
    .usage_limit_requests = record.usage_limit_requests,
    .usage_requests = record.usage_requests,
    .usage_reset_at_ms = record.usage_reset_at_ms,
    .window_ms = record.window_ms,
    .usage_quota_version = record.usage_quota_version,
    .paid_fallback_enabled = paid.paid_fallback_enabled,
    .paid_fallback_limit_microcredits = paid.paid_fallback_limit_microcredits,
    .paid_fallback_spent_microcredits = paid.paid_fallback_spent_microcredits,
    .paid_fallback_reserved_microcredits = paid.paid_fallback_reserved_microcredits,
    .paid_fallback_reservation_request_id = paid.paid_fallback_reservation_request_id,
  };
  EXPECT_EQ(rt::jsonStringify(json::value_from(hash)), rt::jsonStringify(HASH));
}

TEST("shared principal vectors: source-paid validation of created and unpriced records") {
  // "source-paid validation changed"
  EXPECT(hasStrictPaidFallbackPolicy(CREATED_HASH_RECORD) && hasStrictPaidFallbackKeyPolicy(CREATED_RECORD));
  // "unpriced enabled record accepted"
  const json::value unpriced = with(with(json::value_from(policyFixture()), "paid_fallback_enabled", true), "paid_fallback_limit_microcredits", -1);
  EXPECT(!hasStrictPaidFallbackKeyPolicy(unpriced));
  EXPECT_EQ(hasStrictPaidFallbackKeyPolicy(json::value_from(policyFixture())), true);
}

TEST("shared principal vectors: hash rows the projection parser checks with the paid policy") {
  // nativePaid.hasStrictPaidFallbackPolicy(candidate) === paid.hasStrictPaidFallbackPolicy(candidate) for
  // [hash, { ...hash, revoked_at_ms: now }, { ...hash, usage_quota_version: 2 }, {}]: the paid-fallback
  // facade re-exports this function, so both sides are these values.
  EXPECT_EQ(hasStrictPaidFallbackPolicy(HASH), true);
  EXPECT_EQ(hasStrictPaidFallbackPolicy(with(HASH, "revoked_at_ms", NOW)), true);
  EXPECT_EQ(hasStrictPaidFallbackPolicy(with(HASH, "usage_quota_version", 2)), true);
  EXPECT_EQ(hasStrictPaidFallbackPolicy(json::object()), false);

  // The parsing vector's failures: the paid policy alone rejects all but the quota-version row,
  // which the projection parser's own V3 check refuses.
  EXPECT_EQ(hasStrictPaidFallbackPolicy(json::value(nullptr)), false);
  EXPECT_EQ(hasStrictPaidFallbackPolicy(with(HASH, "paid_fallback_enabled", "false")), false);
  EXPECT_EQ(hasStrictPaidFallbackPolicy(with(HASH, "paid_fallback_spent_microcredits", -1)), false);
  EXPECT_EQ(hasStrictPaidFallbackPolicy(with(HASH, "paid_fallback_reserved_microcredits", 0.5)), false);
  EXPECT_EQ(hasStrictPaidFallbackPolicy(with(HASH, "paid_fallback_reservation_request_id", " ")), false);
  EXPECT_EQ(hasStrictPaidFallbackPolicy(with(with(HASH, "paid_fallback_enabled", true), "paid_fallback_limit_microcredits", 0)), false);
  // "unlimited sentinel changed"
  EXPECT_EQ(
    hasStrictPaidFallbackPolicy(with(with(with(HASH, "usage_limit_requests", -1), "paid_fallback_enabled", true), "paid_fallback_limit_microcredits", -1)), true);
}

TEST("hasStrictPaidFallbackPolicy and hasStrictPaidFallbackKeyPolicy match Bun on recorded inputs") {
  const json::value vectors = rt::jsonParse(NATIVE_PAID_POLICY_DIFFERENTIAL_VECTORS);
  std::size_t negativeZeros = 0;
  for (const json::value& vector : vectors.as_array()) {
    const json::value& input = vector.at("input");
    if (holdsNegativeZero(input)) ++negativeZeros;
    const bool policy = hasStrictPaidFallbackPolicy(input);
    const bool key = hasStrictPaidFallbackKeyPolicy(input);
    if (policy != vector.at("policy").as_bool() || key != vector.at("key").as_bool()) {
      ::uos::rt::test::report(__FILE__, __LINE__, "mismatch for " + rt::jsonStringify(vector), false);
    }
  }
  EXPECT_EQ(vectors.as_array().size(), std::size_t{233});
  EXPECT(negativeZeros > 0);
}

TEST("an undefined value is not a strict paid policy") {
  EXPECT_EQ(hasStrictPaidFallbackPolicy(nullptr), false);
  EXPECT_EQ(hasStrictPaidFallbackKeyPolicy(nullptr), false);
}
