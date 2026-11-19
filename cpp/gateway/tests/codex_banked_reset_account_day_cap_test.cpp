// Links: tests/codex_banked_reset_account_day_cap_test.cpp tests/helpers/codex_banked_reset_harness.cpp src/codex/reset_settings.cpp
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
// Port of tests/codex-banked-reset-account-day-cap.test.ts.

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

namespace {
  std::string utcDay(double nowMs) { return rt::Date(nowMs).toISOString().substr(0, 10); }
  rt::Task<std::optional<double>> accountDayCount(std::shared_ptr<MemoryKv> kv, std::string accountId, std::string day) {
    AWAIT(hash, testHash(accountId));
    const auto key = codexResetAccountDailyKey(hash, day);
    AWAIT(row, kv->get(key));
    if (row.value.isNull()) co_return std::nullopt;
    co_return row.value["submission_count"].asNumber();
  }
}
ASYNC_TEST("two different accounts can each redeem once in the same UTC day") {
  Isolation isolation;
  auto kv = std::make_shared<MemoryKv>(); auto provider = std::make_shared<FakeCodexUsageResetProvider>(); auto clock = std::make_shared<TestClock>();
  const auto first = candidate({.accountId = "day-account-a"});
  const auto second = candidate({.accountId = "day-account-b", .requestId = "day-account-b"});
  DONE(seedFences(kv, first)); DONE(seedFences(kv, second));
  const auto deps = dependencies(kv, provider, clock, h::config({.maxPerAccountPerDay = 1}));
  AWAIT(firstReset, attemptCodexBankedReset(first, deps)); AWAIT(secondReset, attemptCodexBankedReset(second, deps));
  EXPECT_EQ(firstReset.kind, "verified"); EXPECT_EQ(secondReset.kind, "verified");
  EXPECT_EQ(provider->redeemInputs.size(), 2u); EXPECT_EQ(provider->commitCount, 2);
  const auto day = utcDay(clock->nowMs);
  AWAIT(firstCount, accountDayCount(kv, "day-account-a", day)); AWAIT(secondCount, accountDayCount(kv, "day-account-b", day));
  EXPECT_EQ(firstCount, 1); EXPECT_EQ(secondCount, 1);
}
ASYNC_TEST("an account's second redemption in the same UTC day is refused with zero consume calls") {
  Isolation isolation;
  auto kv = std::make_shared<MemoryKv>(); auto provider = std::make_shared<FakeCodexUsageResetProvider>(); auto clock = std::make_shared<TestClock>();
  const auto firstWindow = candidate({.accountId = "day-account-a"});
  const auto secondWindow = candidate({.accountId = "day-account-a", .quotaResetAtMs = 1'700'000'120'000, .requestId = "second-window"});
  DONE(seedFences(kv, firstWindow)); DONE(seedFences(kv, secondWindow));
  const auto deps = dependencies(kv, provider, clock, h::config({.maxPerAccountPerDay = 1}));
  AWAIT(first, attemptCodexBankedReset(firstWindow, deps)); EXPECT_EQ(first.kind, "verified");
  AWAIT(refused, attemptCodexBankedReset(secondWindow, deps));
  EXPECT_EQ(refused.kind, "skipped"); EXPECT_EQ(refused.reason, "account_day_limit_reached");
  EXPECT_EQ(provider->redeemInputs.size(), 1u); EXPECT_EQ(provider->commitCount, 1);
  AWAIT(count, accountDayCount(kv, "day-account-a", utcDay(clock->nowMs))); EXPECT_EQ(count, 1);
  const auto refusedRecord = kv->redemptionRecord(codexResetRedemptionKey(requiredHash(refused.accountIdHash), requiredHash(refused.quotaGeneration)));
  ASSERT(refusedRecord); EXPECT_EQ(rt::getString(*refusedRecord, "state"), "claimed");
}
ASYNC_TEST("per-window exact-once still prevents a re-spend of the same window") {
  Isolation isolation;
  auto kv = std::make_shared<MemoryKv>(); auto provider = std::make_shared<FakeCodexUsageResetProvider>(); auto clock = std::make_shared<TestClock>();
  const auto reset = candidate({.accountId = "day-account-a"}); DONE(seedFences(kv, reset));
  const auto deps = dependencies(kv, provider, clock, h::config({.maxPerAccountPerDay = 1}));
  AWAIT(first, attemptCodexBankedReset(reset, deps)); EXPECT_EQ(first.kind, "verified");
  auto repeatCandidate = reset; repeatCandidate.requestId = "same-window-later-request";
  AWAIT(repeat, attemptCodexBankedReset(repeatCandidate, deps));
  EXPECT_EQ(repeat.kind, "verified"); EXPECT_EQ(repeat.reason, "previously_verified"); EXPECT_EQ(repeat.idempotencyKeyHash, first.idempotencyKeyHash);
  EXPECT_EQ(provider->redeemInputs.size(), 1u); EXPECT_EQ(provider->commitCount, 1);
  AWAIT(count, accountDayCount(kv, "day-account-a", utcDay(clock->nowMs))); EXPECT_EQ(count, 1);
}
ASYNC_TEST("live submissions require an exact per-account daily cap of one") {
  Isolation isolation;
  auto kv = std::make_shared<MemoryKv>(); auto provider = std::make_shared<FakeCodexUsageResetProvider>(); auto clock = std::make_shared<TestClock>();
  const auto reset = candidate({.accountId = "day-account-a"}); DONE(seedFences(kv, reset));
  AWAIT(refused, attemptCodexBankedReset(reset, dependencies(kv, provider, clock, h::config({.maxPerAccountPerDay = 2}))));
  EXPECT_EQ(refused.kind, "skipped"); EXPECT_EQ(refused.reason, "per_account_day_limit_invalid"); EXPECT_EQ(provider->callCount(), 0);
  auto contract = provenContract(); contract["redeemOutcomeIsFinal"] = true;
  auto terminalProvider = std::make_shared<FakeCodexUsageResetProvider>(contract);
  AWAIT(refusedTerminal, attemptCodexBankedReset(reset, dependencies(kv, terminalProvider, clock, h::config({.maxPerAccountPerDay = 2}))));
  EXPECT_EQ(refusedTerminal.kind, "skipped"); EXPECT_EQ(refusedTerminal.reason, "terminal_outcome_account_day_limit_must_be_one"); EXPECT_EQ(terminalProvider->callCount(), 0);
  AWAIT(zeroCap, attemptCodexBankedReset(reset, dependencies(kv, provider, clock, h::config({.maxPerAccountPerDay = 0}))));
  EXPECT_EQ(zeroCap.kind, "skipped"); EXPECT_EQ(zeroCap.reason, "per_account_day_limit_invalid");
  AWAIT(shadow, attemptCodexBankedReset(reset, dependencies(kv, provider, clock, h::config({.mode = "shadow", .maxPerAccountPerDay = 2}))));
  EXPECT_EQ(shadow.kind, "skipped"); EXPECT_EQ(shadow.reason, "shadow"); EXPECT_EQ(provider->callCount(), 0);
  AWAIT(hash, testHash(reset.accountId)); const auto key = codexResetAccountDailyKey(hash, utcDay(clock->nowMs));
  AWAIT(row, kv->get(key)); EXPECT(row.value.isNull());
}
ASYNC_TEST("a pre-existing nonzero legacy global_day record no longer blocks another account") {
  Isolation isolation;
  auto kv = std::make_shared<MemoryKv>(); auto provider = std::make_shared<FakeCodexUsageResetProvider>(); auto clock = std::make_shared<TestClock>();
  const auto day = utcDay(clock->nowMs);
  const json::object legacyRecord{{"v", 1}, {"day", day}, {"submission_count", 4}, {"updated_at_ms", rt::makeNumber(clock->nowMs)}};
  DONE(kv->set(codexResetGlobalDailyKey(day), rt::KvValue(legacyRecord)));
  const auto reset = candidate({.accountId = "day-account-a"}); DONE(seedFences(kv, reset));
  AWAIT(result, attemptCodexBankedReset(reset, dependencies(kv, provider, clock, h::config({.maxPerAccountPerDay = 1}))));
  EXPECT_EQ(result.kind, "verified"); EXPECT_EQ(provider->redeemInputs.size(), 1u);
  AWAIT(count, accountDayCount(kv, "day-account-a", day)); EXPECT_EQ(count, 1);
  AWAIT(legacy, kv->get(codexResetGlobalDailyKey(day))); EXPECT(rt::deepEqual(legacy.value.toJson(), legacyRecord));
}
