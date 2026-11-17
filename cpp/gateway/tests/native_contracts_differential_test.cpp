// Differential cases for src/authority/native-contracts.ts recorded from Bun 1.4.0.
// The module declares types only; tests/native-api-key-conformance.test.ts drives them
// through src/authority/native-api-key.ts and src/api-key-policy.ts (not ported yet).
// These cases pin the C++ form: the JSON of the policy projection against JSON.stringify
// of the same object literal under Bun, value_to of it, and the plan shapes.
#include "uos/authority/native_contracts.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

using namespace uos;
using namespace uos::authority::native_contracts;

namespace {
  constexpr std::string_view ApiKeyPolicyJson = R"({"token_hash":"th","key_id":"k","expires_at_ms":-1,"usage_limit_requests":-1,"window_ms":2592000000,"window_start_ms":1700000000000,"usage_reset_at_ms":1702592000000,"policy_version":"v3:1","paid_fallback_enabled":true,"paid_fallback_limit_microcredits":1000000,"paid_fallback_spent_microcredits":0,"paid_fallback_reserved_microcredits":80,"paid_fallback_reservation_request_id":null})";
  constexpr double now = 1'700'000'000'000;
}

TEST("the policy projection") {
  const ApiKeyPolicy policy{
    .token_hash = "th",
    .key_id = "k",
    .expires_at_ms = -1,
    .usage_limit_requests = -1,
    .window_ms = 2'592'000'000,
    .window_start_ms = now,
    .usage_reset_at_ms = now + 2'592'000'000,
    .policy_version = "v3:1",
    .paid_fallback_enabled = true,
    .paid_fallback_limit_microcredits = 1'000'000,
    .paid_fallback_spent_microcredits = 0,
    .paid_fallback_reserved_microcredits = 80,
    .paid_fallback_reservation_request_id = std::nullopt,
  };
  EXPECT_EQ(rt::jsonStringify(json::value_from(policy)), std::string(ApiKeyPolicyJson));
  EXPECT(json::value_to<ApiKeyPolicy>(rt::jsonParse(ApiKeyPolicyJson)) == policy);
  std::string message;
  try {
    (void)json::value_to<ApiKeyPolicy>(rt::jsonParse(R"({"token_hash":"th","key_id":7})"));
  } catch (const rt::TypeError& error) {
    message = error.message;
  }
  EXPECT_EQ(message, "ApiKeyPolicy.key_id is number, not string");
}

TEST("re-exports, snapshots and plans") {
  static_assert(std::is_same_v<ApiKeyUsageRequestV3, types::ApiKeyUsageRequestV3>);
  static_assert(std::is_same_v<ApiKeyUsageWindowV3, types::ApiKeyUsageWindowV3>);
  const ApiKeyUsageSnapshot snapshot{.request = nullptr, .window = rt::jsonParse(R"({"v":3})")};
  EXPECT(snapshot.request.is_null());

  const auto kindOf = [](const auto& plan) { return std::visit([](const auto& alternative) { return std::string(alternative.kind); }, plan); };
  const ApiKeyUsageWritePlan write{
    .checks = {"policy", "window", "request"},
    .request = {.key_id = "k", .request_id = "r", .route = "responses", .state = "reserved", .reserved_at_ms = now, .lease_expires_at_ms = now + 1, .provider = std::nullopt, .dispatched_at_ms = std::nullopt, .released_at_ms = std::nullopt, .release_reason = std::nullopt},
    .window = {.key_id = "k", .policy_version = "v3:1", .window_start_ms = now, .window_reset_at_ms = now + 1, .committed_requests = 0, .reserved_requests = 1, .updated_at_ms = now},
    .expireIn = 60'000,
  };
  EXPECT_EQ(kindOf(ApiKeyUsageReservationPlan(write)), "write");
  EXPECT_EQ(kindOf(ApiKeyUsageReservationPlan(ApiKeyUsageReservationPlanReuse{})), "reuse");
  EXPECT_EQ(kindOf(ApiKeyUsageReservationPlan(ApiKeyUsageReservationPlanFull{.window = write.window})), "full");
  EXPECT_EQ(kindOf(ApiKeyUsageReservationPlan(ApiKeyUsageReservationPlanUnavailable{.reason = "identity_conflict"})), "unavailable");
  EXPECT_EQ(kindOf(ApiKeyUsageDispatchPlan(ApiKeyUsageDispatchPlanAlreadyDispatched{})), "already_dispatched");
  EXPECT_EQ(kindOf(ApiKeyUsageDispatchPlan(ApiKeyUsageDispatchPlanInvalid{})), "invalid");
  EXPECT_EQ(kindOf(ApiKeyUsageReleasePlan(ApiKeyUsageReleasePlanMissingInvalidSettled{.kind = "settled"})), "settled");
  EXPECT(ApiKeyUsageReleasePlan(write) == ApiKeyUsageReleasePlan(write));
}
