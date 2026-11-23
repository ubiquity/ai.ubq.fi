// Links: tests/codex_banked_reset_daily_capacity_incident_test.cpp tests/helpers/codex_account_routing_harness.cpp tests/helpers/counting_kv.cpp tests/helpers/codex_banked_reset_harness.cpp src/auth/relay.cpp src/authority/native_codex.cpp
//        src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp
//        src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/authority/native_paid.cpp
//        src/authority/native_paid_contracts.cpp src/bounded_response_body.cpp src/cache/prompt_analytics.cpp src/cache/prompt_analytics_core.cpp
//        src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp src/cache/telemetry_gate.cpp src/codex/account_routing.cpp
//        src/codex/auth.cpp src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_provider.cpp src/codex/capacity_routing.cpp
//        src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp
//        src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/routing_evaluation.cpp
//        src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/codex/routing_state.cpp src/config.cpp src/http.cpp
//        src/inference_deadline.cpp src/kv.cpp src/metered_quota.cpp src/models/codex_models_availability.cpp src/optional_telemetry_queue.cpp
//        src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp
//        src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp
//        src/provider/capacity_rollups.cpp src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp
//        src/provider/claude_capacity_rollups.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp
//        src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/selection.cpp
//        src/release.cpp src/runtime/http.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp
//        src/runtime/http_upgrade.cpp src/runtime/websocket.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
//        src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/types.cpp src/utils.cpp
//        src/codex/index.cpp src/codex/responses_state.cpp src/codex/responses_operations.cpp src/codex/responses_runners.cpp
//        src/codex/banked_reset_pool.cpp src/codex/banked_reset_submission.cpp src/codex/banked_reset_claim.cpp
// Port of tests/codex-banked-reset-daily-capacity-incident.test.ts; all 4 original cases, no deferred cases.
#include "tests/helpers/codex_account_routing_harness.hpp"
#include "tests/helpers/codex_banked_reset_harness.hpp"
#include "uos/codex/capacity_routing.hpp"
#include "uos/config.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/utils.hpp"
#include <algorithm>

#define AWAIT(name, expression) auto name##Pending = uos::rt::Promise(expression); auto name = co_await name##Pending.wait()
#define RUN(expression) do { auto pending = uos::rt::Promise(expression); co_await pending.wait(); } while (false)

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace routing = uos::codex::routing_state;
  namespace capacity = uos::codex::capacity_routing;
  namespace banked = uos::tests::helpers::codex_banked_reset_harness;
  using namespace uos::tests::helpers::codex_account_routing_harness;
  constexpr double DAY_MS = 24 * 60 * 60'000;
  const std::string LUNA = "gpt-5.6-luna";
  constexpr double WEEKLY_WINDOW_SECONDS = 604'800;
  struct Fixture {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/cpp-c2-daily"}}};
    std::string savedSeed = uos::config::config.codexAuthJsonB64;
    std::shared_ptr<RoutingKv> kv = std::make_shared<RoutingKv>();
    Fixture() { uos::config::config.codexAuthJsonB64.clear(); setKvForTest(kv); resetCodexAccountRoutingForTest(); resetProviderSelectionCacheForTest(); }
    ~Fixture() {
      setKvForTest(nullptr); resetCodexAccountRoutingForTest(); resetProviderSelectionCacheForTest(); uos::config::config.codexAuthJsonB64 = savedSeed;
    }
  };
  std::string utcDay(double now) { return rt::Date(now).toISOString().substr(0, 10); }
  rt::Task<std::optional<double>> accountDayCount(std::shared_ptr<banked::MemoryKv> kv, std::string accountId, std::string day) {
    AWAIT(hash, banked::testHash(accountId)); const auto key = banked::codexResetAccountDailyKey(hash, day);
    AWAIT(entry, kv->get(key)); const auto value = entry.value.toJson(); co_return rt::getNumber(value, "submission_count");
  }
  banked::CodexBankedResetConfig liveIncidentConfig() {
    return banked::config(banked::ConfigOverrides{.mode = "live", .maxPerAccountPerDay = 1, .maxPerAccountPerWindow = 1});
  }
  rt::Task<routing::CodexRoutingSlot> routingSlot(uos::types::CodexAuthState auth) {
    const std::string identity = std::string("uos_ai") + '\0' + "codex_routing_account" + '\0' + auth.account_id;
    AWAIT(hash, uos::utils::sha256Hex(identity)); AWAIT(credential, codexCredentialVersion(auth));
    routing::CodexRoutingSlot slot; slot.account_id_hash = hash; slot.credential_version = credential;
    slot.quota_blocked_classes = std::vector<std::string>{}; slot.quota_blocks_by_class = routing::CodexQuotaBlocksByClass{};
    slot.upstream_timeout_blocked_until_ms = nullptr; co_return slot;
  }
}

ASYNC_TEST("a capacity-100% class yields a blocked cohort identity, and only materialization arms its fence") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000; auto auth = singlePool.accounts[0]; auth.updated_at_ms = now;
  const CodexAuthPoolState authPool{.accounts = {auth}, .updated_at_ms = now}; RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(authPool))));
  AWAIT(slot, routingSlot(auth)); slot.primary_used_percent = 100; slot.observed_reset_at_ms = now - 7 * DAY_MS; slot.banked_reset_generation_ambiguous = true;
  const json::value row = json::object{{"v", 2}, {"updated_at_ms", now}, {"slots", json::array{routing::toJson(slot)}}}; RUN(kv->set(CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvValue(row)));
  const double resetAtMs = now + 7 * DAY_MS; routing::CodexCapacityRoutingObservationInput input;
  input.slot = 0; input.account_id = "one"; input.state = "available"; input.source_observed_at_ms = now; input.snapshot_at_ms = now;
  input.windows.primary = routing::CodexCapacityRoutingWindow{WEEKLY_WINDOW_SECONDS, 100, resetAtMs};
  RUN(recordCodexCapacityRoutingObservations(std::vector<routing::CodexCapacityRoutingObservationInput>{input}, now));
  AWAIT(selected, selectCodexRoutingAccountsStrong(singlePool, singlePool.accounts, now + 2, LUNA)); ASSERT(std::holds_alternative<routing::RouteSelectionQuotaBlocked>(selected));
  const auto& cohort = std::get<routing::RouteSelectionQuotaBlocked>(selected); EXPECT(cohort.fullCohortExhausted); ASSERT(!cohort.blockedAccounts.empty());
  const auto blocked = cohort.blockedAccounts[0]; EXPECT_EQ(blocked.quotaResetAtMs, resetAtMs); AWAIT(absentFence, getCodexQuotaBlockFence(blocked, resetAtMs)); EXPECT(!absentFence);
  AWAIT(observations, capacity::loadCodexCapacityRoutingObservations(singlePool, true)); std::optional<routing::CodexCapacityRoutingObservation> observation;
  for (const auto& item : observations) { if (item.account_id_hash == blocked.accountIdHash) { observation = item; break; } }
  ASSERT(observation); AWAIT(reconciled, capacity::reconcileCapacityQuotaBlock(blocked, *observation, now + 3)); EXPECT(reconciled);
  AWAIT(fence, getCodexQuotaBlockFence(blocked, resetAtMs)); EXPECT(fence);
  AWAIT(refreshed, selectCodexRoutingAccountsStrong(singlePool, singlePool.accounts, now + 4, LUNA)); ASSERT(std::holds_alternative<routing::RouteSelectionQuotaBlocked>(refreshed));
  const auto& refreshedCohort = std::get<routing::RouteSelectionQuotaBlocked>(refreshed); EXPECT(refreshedCohort.fullCohortExhausted); ASSERT(!refreshedCohort.blockedAccounts.empty());
  EXPECT_EQ(refreshedCohort.blockedAccounts[0].quotaResetAtMs, resetAtMs);
}

ASYNC_TEST("a capacity-blocked live episode consumes exactly once without an arming step") {
  Fixture fixture; const auto kv = std::make_shared<banked::MemoryKv>(); const auto clock = std::make_shared<banked::TestClock>();
  const auto reset = banked::candidate(banked::CandidateOverrides{.accountId = "incident-account-a", .routingGeneration = 7}); RUN(banked::seedFences(kv, reset));
  const auto provider = std::make_shared<banked::FakeCodexUsageResetProvider>(); provider->inventory = banked::inventory("incident-credit-a", clock->nowMs + 20'000);
  const std::vector<uos::codex::banked_reset::CodexBankedResetPoolCandidate> bankedPool{{0, reset, provider}};
  const auto deps = banked::dependencies(kv, provider, clock, liveIncidentConfig());
  AWAIT(consumed, banked::evaluateCodexBankedResetPool(bankedPool, deps)); EXPECT_EQ(consumed.kind, "verified"); ASSERT(consumed.selected); EXPECT_EQ(consumed.selected->slot, 0);
  ASSERT_EQ(provider->redeemInputs.size(), std::size_t{1}); EXPECT_EQ(provider->redeemInputs[0].creditId, "incident-credit-a"); EXPECT_EQ(provider->commitCount, 1);
  AWAIT(firstCount, accountDayCount(kv, "incident-account-a", utcDay(clock->nowMs))); EXPECT_EQ(firstCount, 1); ASSERT(consumed.reset);
  const auto redemptionKey = banked::codexResetRedemptionKey(banked::requiredHash(consumed.reset->accountIdHash), banked::requiredHash(consumed.reset->quotaGeneration));
  const auto redemption = kv->redemptionRecord(redemptionKey); ASSERT(redemption); EXPECT_EQ(rt::getString(*redemption, "state"), "verified");
  AWAIT(repeated, banked::evaluateCodexBankedResetPool(bankedPool, deps)); EXPECT_EQ(repeated.kind, "verified"); EXPECT_EQ(provider->redeemInputs.size(), std::size_t{1}); EXPECT_EQ(provider->commitCount, 1);
  AWAIT(repeatedCount, accountDayCount(kv, "incident-account-a", utcDay(clock->nowMs))); EXPECT_EQ(repeatedCount, 1);
}

ASYNC_TEST("two accounts may each redeem once per UTC day, and one account's second episode is refused") {
  Fixture fixture; const auto kv = std::make_shared<banked::MemoryKv>(); const auto clock = std::make_shared<banked::TestClock>(); const std::string day = utcDay(clock->nowMs);
  const auto provider = std::make_shared<banked::FakeCodexUsageResetProvider>(); const auto configured = liveIncidentConfig(); const auto deps = banked::dependencies(kv, provider, clock, configured);
  const auto accountFirst = banked::candidate(banked::CandidateOverrides{.accountId = "incident-account-a"});
  const auto accountB = banked::candidate(banked::CandidateOverrides{.accountId = "incident-account-b", .requestId = "incident-account-b"});
  const auto accountSecond = banked::candidate(banked::CandidateOverrides{.accountId = "incident-account-a", .quotaResetAtMs = clock->nowMs + 120'000, .requestId = "incident-account-a-second"});
  RUN(banked::seedFences(kv, accountFirst)); RUN(banked::seedFences(kv, accountB)); RUN(banked::seedFences(kv, accountSecond));
  AWAIT(first, banked::attemptCodexBankedReset(accountFirst, deps)); EXPECT_EQ(first.kind, "verified");
  AWAIT(firstA, accountDayCount(kv, "incident-account-a", day)); EXPECT_EQ(firstA, 1);
  AWAIT(second, banked::attemptCodexBankedReset(accountB, deps)); EXPECT_EQ(second.kind, "verified");
  AWAIT(secondA, accountDayCount(kv, "incident-account-a", day)); EXPECT_EQ(secondA, 1); AWAIT(secondB, accountDayCount(kv, "incident-account-b", day)); EXPECT_EQ(secondB, 1);
  AWAIT(refused, banked::attemptCodexBankedReset(accountSecond, deps)); EXPECT_EQ(refused.kind, "skipped"); EXPECT_EQ(refused.reason, "account_day_limit_reached");
  AWAIT(refusedCount, accountDayCount(kv, "incident-account-a", day)); EXPECT_EQ(refusedCount, 1);
  EXPECT_EQ(std::count_if(provider->redeemInputs.begin(), provider->redeemInputs.end(), [](const auto& input) { return input.accountId == "incident-account-a"; }), 1);
  EXPECT_EQ(std::count_if(provider->redeemInputs.begin(), provider->redeemInputs.end(), [](const auto& input) { return input.accountId == "incident-account-b"; }), 1); EXPECT_EQ(provider->commitCount, 2);
  const auto refusedKey = banked::codexResetRedemptionKey(banked::requiredHash(refused.accountIdHash), banked::requiredHash(refused.quotaGeneration));
  const auto refusedRecord = kv->redemptionRecord(refusedKey); ASSERT(refusedRecord); EXPECT_EQ(rt::getString(*refusedRecord, "state"), "claimed");
  AWAIT(global, kv->get(banked::codexResetGlobalDailyKey(day))); EXPECT(global.value.isNull());
}

ASYNC_TEST("the live banked-reset policy requires an exact per-account daily cap of one for a terminal provider") {
  Fixture fixture; auto contract = banked::provenContract(); contract["redeemOutcomeIsFinal"] = true;
  const auto terminalProvider = std::make_shared<banked::FakeCodexUsageResetProvider>(json::value(contract)); const auto explicitConfig = banked::config();
  EXPECT(!uos::codex::banked_reset::providerPolicyReason(explicitConfig, terminalProvider.get())); auto invalidConfig = explicitConfig; invalidConfig.maxPerAccountPerDay = 2;
  EXPECT_EQ(uos::codex::banked_reset::providerPolicyReason(invalidConfig, terminalProvider.get()), "terminal_outcome_account_day_limit_must_be_one");
  const auto kv = std::make_shared<banked::MemoryKv>(); const auto clock = std::make_shared<banked::TestClock>(); const auto reset = banked::candidate(banked::CandidateOverrides{.accountId = "incident-terminal-account"});
  RUN(banked::seedFences(kv, reset));
  const std::vector<uos::codex::banked_reset::CodexBankedResetPoolCandidate> bankedPool{{0, reset, terminalProvider}};
  const auto configured = banked::config(banked::ConfigOverrides{.mode = "live", .maxPerAccountPerDay = 2}); const auto deps = banked::dependencies(kv, terminalProvider, clock, configured);
  AWAIT(refused, banked::evaluateCodexBankedResetPool(bankedPool, deps)); EXPECT_EQ(refused.kind, "skipped");
  EXPECT_EQ(refused.reason, "terminal_outcome_account_day_limit_must_be_one"); EXPECT_EQ(terminalProvider->callCount(), 0);
}
