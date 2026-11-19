// Links: tests/codex_banked_reset_part3_test.cpp tests/helpers/codex_banked_reset_harness.cpp src/codex/reset_settings.cpp
//        src/codex/banked_reset.cpp src/codex/banked_reset_claim.cpp src/codex/banked_reset_submission.cpp src/codex/banked_reset_pool.cpp
//        src/codex/banked_reset_provider.cpp src/codex/auth.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp
//        src/codex/external_auth.cpp src/codex/capacity_routing.cpp src/codex/rate_limit_429.cpp src/codex/routing_mutations.cpp
//        src/codex/routing_state.cpp src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp
//        src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp
//        src/bounded_response_body.cpp src/config.cpp src/kv.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp
//        src/provider/health.cpp src/release.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp
//        src/runtime/http_upgrade.cpp src/runtime/http.cpp src/runtime/websocket.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
// src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp
//        src/utils.cpp src/types.cpp
// Port of tests/codex-banked-reset-part3.test.ts.

#include "tests/helpers/codex_banked_reset_harness.hpp"
#include "uos/config.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
namespace rt = uos::rt;
namespace json = uos::json;
namespace h = uos::tests::helpers::codex_banked_reset_harness;
using namespace uos::tests::helpers::codex_banked_reset_harness;
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
#define DONE(expression) do { auto task_ = (expression); co_await std::move(task_); } while (false)
namespace {
  struct Isolation {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{}};
    std::string savedAuthJson = uos::config::config.codexAuthJsonB64;
    Isolation() { uos::config::config.codexAuthJsonB64.clear(); }
    ~Isolation() { uos::config::config.codexAuthJsonB64 = savedAuthJson; }
  };
}

ASYNC_TEST("the fixed live policy preserves account budgets and durable-record parsing stays strict") {
  Isolation isolation;
  const auto defaults = loadCodexBankedResetConfig();
  EXPECT(defaults.enabled); EXPECT_EQ(defaults.mode, "live"); EXPECT_EQ(defaults.maxPerAccountPerDay, 1); EXPECT_EQ(defaults.maxPerAccountPerWindow, 1);
  const double submittedAtMs = 1'700'000'000'001;
  const json::value validRecord = rt::jsonParse(R"({"v":1,"account_id_hash":"account-hash","credential_version":"credential-v1","quota_generation":"generation-v1","routing_generation":1,"idempotency_key_hash":"idempotency-hash","state":"submitted","owner_token":"owner","fence":1,"lease_expires_at_ms":1700000030000,"provider_receipt_id":"receipt","created_at_ms":1700000000000,"updated_at_ms":1700000000001,"submitted_at_ms":1700000000001,"verified_at_ms":null,"last_error_code":null})");
  const auto parsed = parseCodexResetRedemptionRecord(validRecord); ASSERT(parsed); EXPECT(rt::deepEqual(json::value_from(*parsed), validRecord));
  std::vector<json::object> invalids;
  auto invalid = validRecord.as_object(); invalid["fence"] = -1; invalids.push_back(invalid);
  invalid = validRecord.as_object(); invalid["provider_receipt_id"] = ""; invalids.push_back(invalid);
  invalid = validRecord.as_object(); invalid["state"] = "future_state"; invalids.push_back(invalid);
  invalid = validRecord.as_object(); invalid["state"] = "claimed"; invalid["submitted_at_ms"] = 1'700'000'000'000LL; invalid["provider_receipt_id"] = nullptr; invalids.push_back(invalid);
  invalid = validRecord.as_object(); invalid["state"] = "claimed"; invalid["submitted_at_ms"] = nullptr; invalids.push_back(invalid);
  invalid = validRecord.as_object(); invalid["submitted_at_ms"] = nullptr; invalids.push_back(invalid);
  invalid = validRecord.as_object(); invalid["state"] = "unknown"; invalid["last_error_code"] = nullptr; invalids.push_back(invalid);
  invalid = validRecord.as_object(); invalid["state"] = "verified"; invalids.push_back(invalid);
  invalid = validRecord.as_object(); invalid["state"] = "verified"; invalid["verified_at_ms"] = rt::makeNumber(submittedAtMs - 1); invalids.push_back(invalid);
  invalid = validRecord.as_object(); invalid["state"] = "rejected"; invalid["submitted_at_ms"] = nullptr; invalid["provider_receipt_id"] = nullptr; invalids.push_back(invalid);
  invalid = validRecord.as_object(); invalid["updated_at_ms"] = 1'699'999'999'999LL; invalids.push_back(invalid);
  for (const auto& value : invalids) EXPECT(!parseCodexResetRedemptionRecord(json::value(value)));
  auto unprovenContract = provenContract();
  unprovenContract["idempotency"] = json::object{{"callerSupplied", false}, {"retentionMs", nullptr}};
  unprovenContract["lookup"] = json::object{{"byIdempotencyKey", false}, {"byProviderReceiptId", false}};
  unprovenContract["verification"] = json::object{{"independentlyVerifiable", false}};
  unprovenContract["supportedResetTypes"] = json::array{};
  auto normalProvider = std::make_shared<FakeCodexUsageResetProvider>();
  auto provider = std::make_shared<FakeCodexUsageResetProvider>(unprovenContract);
  EXPECT(providerSupportsLiveRedemption(normalProvider.get())); EXPECT(!providerSupportsLiveRedemption(unavailableCodexUsageResetProvider.get())); EXPECT(!providerSupportsLiveRedemption(provider.get()));
  EXPECT(providerSupportsResetType(normalProvider.get(), json::value("codex_rate_limits")));
  auto malformedContractProvider = std::make_shared<FakeCodexUsageResetProvider>();
  malformedContractProvider->contract = json::object{{"idempotency", nullptr}, {"supportedResetTypes", "not-an-array"}};
  EXPECT(!providerSupportsLiveRedemption(malformedContractProvider.get())); EXPECT(!providerSupportsResetType(malformedContractProvider.get(), json::value("codex_rate_limits")));
  auto kv = std::make_shared<MemoryKv>(); auto clock = std::make_shared<TestClock>();
  AWAIT(result, attemptCodexBankedReset(candidate(), dependencies(kv, provider, clock)));
  EXPECT_EQ(result.kind, "skipped"); EXPECT_EQ(result.reason, "provider_contract_unproven"); EXPECT_EQ(provider->callCount(), 0); EXPECT_EQ(provider->commitCount, 0);
  AWAIT(malformedResult, attemptCodexBankedReset(candidate({.requestId = "malformed-provider-contract"}), dependencies(std::make_shared<MemoryKv>(), malformedContractProvider, std::make_shared<TestClock>())));
  EXPECT_EQ(malformedResult.kind, "skipped"); EXPECT_EQ(malformedResult.reason, "provider_contract_unproven"); EXPECT_EQ(malformedContractProvider->callCount(), 0);
}
