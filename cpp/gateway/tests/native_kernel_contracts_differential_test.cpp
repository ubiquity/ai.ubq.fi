// Differential cases for src/authority/native-kernel-contracts.ts recorded from Bun 1.4.0.
// The module declares types only; tests/native-kernel-conformance.test.ts drives them
// through src/authority/native-kernel.ts (not ported yet). These cases pin the C++ form:
// the JSON of each stored row against JSON.stringify of the same object literal under Bun,
// value_to of that JSON, and the snapshot and plan shapes the conformance vectors build.
#include "uos/authority/native_kernel_contracts.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

using namespace uos;
using namespace uos::authority::native_kernel_contracts;

namespace {
  constexpr std::string_view KernelQuotaPolicyV2Json = R"({"v":2,"scope":"repo","owner":"ubiquity","repo":"ai.ubq.fi","usage_limit_requests":100,"window_ms":86400000,"expires_at_ms":-1,"created_at_ms":1700000000000,"updated_at_ms":1700000000000})";
  constexpr std::string_view KernelQuotaPolicyV2OrgJson = R"({"v":2,"scope":"org","owner":"ubiquity","usage_limit_requests":-1,"window_ms":3600000,"expires_at_ms":1700000000001,"created_at_ms":1700000000000,"updated_at_ms":1700000000000})";
  constexpr std::string_view KernelQuotaWindowV2Json = R"({"v":2,"scope":"repo","owner":"o","repo":"r","usage_requests":3,"reserved_requests":1,"usage_reset_at_ms":1700000000001,"applied_window_ms":60000,"created_at_ms":1700000000000,"updated_at_ms":1700000000000})";
  constexpr std::string_view KernelQuotaReservationRowV2Json = R"({"v":2,"scope":"org","owner":"o","request_id":"req","route":"responses","window_created_at_ms":1700000000000,"window_reset_at_ms":1700000060000,"state":"reserved","terminal_intent":null,"reserved_at_ms":1700000000000,"lease_expires_at_ms":1700000030000,"committed_at_ms":null,"released_at_ms":null,"release_reason":null})";
  constexpr std::string_view KernelQuotaReservationRowV2ReleasedJson = R"({"v":2,"scope":"repo","owner":"o","repo":"r","request_id":"req","route":"chat","window_created_at_ms":1700000000000,"window_reset_at_ms":1700000060000,"state":"released","terminal_intent":"released","reserved_at_ms":1700000000000,"lease_expires_at_ms":1700000030000,"committed_at_ms":null,"released_at_ms":1700000000005,"release_reason":"client_abort"})";
  constexpr std::string_view KernelDefaultWindowCutoverV2Json = R"({"v":2,"id":"8f3c","created_at_ms":1700000000000,"expires_at_ms":1700086400000})";

  template <class T> void checkJson(const T& value, std::string_view expected, const char* label) {
    const std::string written = rt::jsonStringify(json::value_from(value));
    if (written != expected) rt::test::report(__FILE__, __LINE__, std::string(label) + " value_from\n      actual:   " + written + "\n      expected: " + std::string(expected), false);
    const T parsed = json::value_to<T>(rt::jsonParse(expected));
    if (!(parsed == value)) rt::test::report(__FILE__, __LINE__, std::string(label) + " value_to differs from the record", false);
  }

  template <class T> std::string rejection(std::string_view text) {
    try {
      (void)json::value_to<T>(rt::jsonParse(text));
    } catch (const rt::TypeError& error) {
      return error.message;
    }
    return "<accepted>";
  }

  // The conformance vectors' `row(value: unknown, revision = "opaque.rev:0x1")`.
  KernelQuotaRead<> row(json::value value, std::optional<std::string> revision = "opaque.rev:0x1") {
    return KernelQuotaRead<>{.value = std::move(value), .revision = std::move(revision), .expiresAtMs = nullptr};
  }

  constexpr double now = 1'700'000'000'000;
}

TEST("stored kernel quota rows") {
  checkJson(KernelQuotaPolicyV2{
              .scope = "repo", .owner = "ubiquity", .repo = "ai.ubq.fi", .usage_limit_requests = 100, .window_ms = 86'400'000, .expires_at_ms = -1, .created_at_ms = now, .updated_at_ms = now},
            KernelQuotaPolicyV2Json, "KernelQuotaPolicyV2");
  checkJson(KernelQuotaPolicyV2{.scope = "org", .owner = "ubiquity", .usage_limit_requests = -1, .window_ms = 3'600'000, .expires_at_ms = now + 1, .created_at_ms = now, .updated_at_ms = now},
            KernelQuotaPolicyV2OrgJson, "KernelQuotaPolicyV2Org");
  checkJson(KernelQuotaWindowV2{
              .scope = "repo",
              .owner = "o",
              .repo = "r",
              .usage_requests = 3,
              .reserved_requests = 1,
              .usage_reset_at_ms = now + 1,
              .applied_window_ms = 60'000,
              .created_at_ms = now,
              .updated_at_ms = now,
            },
            KernelQuotaWindowV2Json, "KernelQuotaWindowV2");
  checkJson(KernelQuotaReservationRowV2{
              .scope = "org",
              .owner = "o",
              .request_id = "req",
              .route = "responses",
              .window_created_at_ms = now,
              .window_reset_at_ms = now + 60'000,
              .state = "reserved",
              .terminal_intent = std::nullopt,
              .reserved_at_ms = now,
              .lease_expires_at_ms = now + 30'000,
              .committed_at_ms = std::nullopt,
              .released_at_ms = std::nullopt,
              .release_reason = std::nullopt,
            },
            KernelQuotaReservationRowV2Json, "KernelQuotaReservationRowV2");
  checkJson(KernelQuotaReservationRowV2{
              .scope = "repo",
              .owner = "o",
              .repo = "r",
              .request_id = "req",
              .route = "chat",
              .window_created_at_ms = now,
              .window_reset_at_ms = now + 60'000,
              .state = "released",
              .terminal_intent = "released",
              .reserved_at_ms = now,
              .lease_expires_at_ms = now + 30'000,
              .committed_at_ms = std::nullopt,
              .released_at_ms = now + 5,
              .release_reason = "client_abort",
            },
            KernelQuotaReservationRowV2ReleasedJson, "KernelQuotaReservationRowV2Released");
  checkJson(KernelDefaultWindowCutoverV2{.id = "8f3c", .created_at_ms = now, .expires_at_ms = now + 86'400'000}, KernelDefaultWindowCutoverV2Json, "KernelDefaultWindowCutoverV2");
  EXPECT_EQ(rejection<KernelQuotaPolicyV2>(R"({"v":2,"scope":"repo","owner":"o","repo":null})"), "KernelQuotaPolicyV2.repo is null, not string or undefined");
  EXPECT_EQ(rejection<KernelDefaultWindowCutoverV2>(R"({"v":"2"})"), "KernelDefaultWindowCutoverV2.v is string, not number");
}

TEST("reads default to unknown values") {
  static_assert(std::is_same_v<decltype(KernelQuotaRead<>::value), json::value>);
  static_assert(std::is_same_v<decltype(KernelQuotaRead<KernelQuotaPolicyV2>::value), std::optional<KernelQuotaPolicyV2>>);
  const KernelQuotaRead<> missing = row(nullptr, std::nullopt);
  EXPECT(missing.value.is_null() && !missing.revision && missing.expiresAtMs == nullptr);
  const KernelQuotaRead<> unread{.value = json::value(), .revision = std::nullopt};
  EXPECT(unread.expiresAtMs == rt::undefined);
  const KernelQuotaRead<> present = row(rt::jsonParse(KernelQuotaPolicyV2Json));
  EXPECT(present.revision == std::optional<std::string>("opaque.rev:0x1"));
  const KernelQuotaEligibilitySnapshot snapshot{.policy = present, .repoGuard = std::nullopt, .defaultCutover = missing};
  EXPECT(snapshot.expectedRepoPolicy == std::nullopt);
}

TEST("identities, options and plans") {
  const KernelQuotaReservationIdentity identity{{.scope = "repo", .owner = "o", .repo = "r"}, "request", "responses", now};
  const KernelQuotaIdentity& base = identity;
  EXPECT_EQ(base.scope, "repo");
  EXPECT(base.repo == std::optional<std::string>("r"));
  const KernelQuotaIdentity org{.scope = "org", .owner = "o", .repo = std::nullopt};
  EXPECT(!org.repo);
  const KernelQuotaPolicySetOptions options{};
  EXPECT(!options.resetUsage && !options.windowMs && !options.expiresAtMs);

  const auto kindOf = [](const auto& plan) { return std::visit([](const auto& alternative) { return std::string(alternative.kind); }, plan); };
  const KernelQuotaReservePlan refused = KernelQuotaRefusal{.status = 429, .message = "Kernel quota exceeded"};
  EXPECT_EQ(kindOf(refused), "refused");
  const KernelQuotaEligibilityPlan eligible = KernelQuotaEligibilityPlanEligible{.effective = {.policy = std::nullopt, .limit = 1, .windowMs = 2, .expiresAtMs = -1, .source = "default"}};
  EXPECT_EQ(kindOf(eligible), "eligible");
  static_assert(std::is_same_v<KernelQuotaSettlePlanUnchanged, KernelQuotaTerminalPlanUnchanged>);
  const KernelQuotaTerminalPlan unchanged = KernelQuotaTerminalPlanUnchanged{.outcome = "missing"};
  const KernelQuotaSettlePlan settleUnchanged = std::get<KernelQuotaTerminalPlanUnchanged>(unchanged);
  EXPECT_EQ(kindOf(settleUnchanged), "unchanged");
  EXPECT_EQ(kindOf(KernelQuotaCutoverPlan(KernelQuotaCutoverPlanRefused{.reason = "concurrent_change"})), "refused");
  EXPECT_EQ(kindOf(KernelQuotaCutoverPlan(KernelQuotaCutoverPlanAcquire{.marker = {.id = "id", .created_at_ms = now, .expires_at_ms = now + 1}, .expireIn = 1})), "acquire");
}
