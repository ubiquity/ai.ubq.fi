// Links: tests/codex_banked_reset_capacity_recovery_test.cpp tests/helpers/codex_account_routing_harness.cpp tests/helpers/counting_kv.cpp src/auth/relay.cpp src/authority/native_codex.cpp
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
// Port of tests/codex-banked-reset-capacity-recovery.test.ts; all 12 original cases, no deferred cases.
#include "tests/helpers/codex_account_routing_harness.hpp"
#include "uos/codex/banked_reset_claim.hpp"
#include "uos/codex/capacity_routing.hpp"
#include "uos/codex/overage_settings.hpp"
#include "uos/config.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/utils.hpp"

#define AWAIT(name, expression) auto name##Pending = uos::rt::Promise(expression); auto name = co_await name##Pending.wait()
#define RUN(expression) do { auto pending = uos::rt::Promise(expression); co_await pending.wait(); } while (false)
#define SELECT(name, poolValue, nowValue) AWAIT(name, selectCodexRoutingAccounts(poolValue, (poolValue).accounts, nowValue, LUNA))
#define STRONG(name, poolValue, nowValue) AWAIT(name, selectCodexRoutingAccountsStrong(poolValue, (poolValue).accounts, nowValue, LUNA))

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace routing = uos::codex::routing_state;
  namespace capacity = uos::codex::capacity_routing;
  namespace banked = uos::codex::banked_reset;
  namespace overage = uos::codex::overage_settings;
  using namespace uos::tests::helpers::codex_account_routing_harness;
  using routing::RoutingAccount;
  using RoutingAuth = uos::types::CodexAuthState;
  constexpr double DAY_MS = 24 * 60 * 60'000;
  const std::string LUNA = "gpt-5.6-luna";
  constexpr double WEEKLY_WINDOW_SECONDS = 604'800;
  struct Fixture {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/cpp-c2-recovery"}}};
    std::string savedSeed = uos::config::config.codexAuthJsonB64;
    std::shared_ptr<RoutingKv> kv = std::make_shared<RoutingKv>();
    Fixture() {
      uos::config::config.codexAuthJsonB64.clear(); setKvForTest(kv); resetCodexAccountRoutingForTest(); resetProviderSelectionCacheForTest();
      overage::resetOverageUsageCacheForTest();
    }
    ~Fixture() {
      setKvForTest(nullptr); resetCodexAccountRoutingForTest(); resetProviderSelectionCacheForTest(); overage::resetOverageUsageCacheForTest();
      uos::config::config.codexAuthJsonB64 = savedSeed;
    }
  };
  std::string kind(const routing::RouteSelection& value) { return std::visit([](const auto& item) { return std::string(item.kind); }, value); }
  const std::vector<RoutingAccount>& accounts(const routing::RouteSelection& value) { return std::get<routing::RouteSelectionEligible>(value).accounts; }
  routing::CodexAccountRoutingState state(const std::shared_ptr<RoutingKv>& kv) {
    return required(parseCodexAccountRoutingState(kv->row(CODEX_ACCOUNT_ROUTING_KV_KEY)), "routing state");
  }
  rt::Task<std::string> routingAccountIdHash(std::string accountId) {
    const std::string value = std::string("uos_ai") + '\0' + "codex_routing_account" + '\0' + accountId;
    AWAIT(hash, uos::utils::sha256Hex(value)); co_return hash;
  }
  RoutingAuth harnessAuth(double now) { auto auth = singlePool.accounts[0]; auth.updated_at_ms = now; return auth; }
  rt::Task<json::object> routingSlot(RoutingAuth auth, json::object overrides = {}) {
    AWAIT(hash, routingAccountIdHash(auth.account_id)); AWAIT(credential, codexCredentialVersion(auth));
    json::object slot{{"account_id_hash", hash}, {"credential_version", credential}, {"quota_blocked_until_ms", nullptr}, {"quota_block_source", nullptr},
      {"quota_blocked_classes", json::array{}}, {"quota_blocks_by_class", json::object{}}, {"invalid_credential_version", nullptr},
      {"primary_used_percent", nullptr}, {"secondary_used_percent", nullptr}, {"quota_signal_observed_at_ms", nullptr}, {"capacity_observed_at_ms", nullptr},
      {"upstream_timeout_blocked_until_ms", nullptr}, {"observed_reset_at_ms", nullptr}, {"observed_reset_at_is_stable", false},
      {"banked_reset_generation_ambiguous", false}, {"banked_reset_recovery_probe_pending", false}, {"generation", 0}, {"probe_lease", nullptr}};
    rt::objectAssign(slot, overrides); co_return slot;
  }
  rt::Task<void> seedRoutingState(std::shared_ptr<RoutingKv> kv, RoutingAuth auth, double now, json::object overrides = {}) {
    AWAIT(slot, routingSlot(auth, overrides)); const json::value row = json::object{{"v", 2}, {"updated_at_ms", now}, {"slots", json::array{slot}}};
    RUN(kv->set(CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvValue(row)));
  }
  rt::Task<CodexAuthPoolState> seedAuthPool(std::shared_ptr<RoutingKv> kv, RoutingAuth auth, double now) {
    CodexAuthPoolState authPool{.accounts = {auth}, .updated_at_ms = now}; RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(authPool))));
    co_return authPool;
  }
  routing::CodexCapacityRoutingObservationInput capacitySample(double slot, const std::string& accountId, double at, std::optional<double> reset, double used = 100) {
    routing::CodexCapacityRoutingObservationInput sample;
    sample.slot = slot; sample.account_id = accountId; sample.state = "available"; sample.source_observed_at_ms = at; sample.snapshot_at_ms = at;
    sample.windows.primary = routing::CodexCapacityRoutingWindow{WEEKLY_WINDOW_SECONDS, used, reset}; return sample;
  }
  rt::Task<void> seedCapacityObservation(double snapshotAtMs, std::optional<double> resetAtMs, double usedPercent = 100) {
    const std::vector<routing::CodexCapacityRoutingObservationInput> inputs{capacitySample(0, "one", snapshotAtMs, resetAtMs, usedPercent)};
    RUN(recordCodexCapacityRoutingObservations(inputs, snapshotAtMs));
  }
  rt::Task<void> seedCapacityExhaustedProbe(std::shared_ptr<RoutingKv> kv, double now) {
    const auto auth = harnessAuth(now); RUN(seedAuthPool(kv, auth, now));
    const json::object overrides{{"primary_used_percent", 100}, {"observed_reset_at_ms", now - 7 * DAY_MS},
      {"observed_reset_at_is_stable", true}, {"banked_reset_generation_ambiguous", true}};
    RUN(seedRoutingState(kv, auth, now, overrides)); RUN(seedCapacityObservation(now + 1, std::nullopt));
  }
  struct FreshBlock { RoutingAccount account; double freshDeadline; routing::CodexQuotaClassBlock classBlock; };
  rt::Task<FreshBlock> markFreshStableQuotaBlock(std::shared_ptr<RoutingKv> kv, double now, json::object overrides) {
    const auto auth = harnessAuth(now); RUN(seedRoutingState(kv, auth, now, overrides)); SELECT(selection, singlePool, now);
    if (kind(selection) != "eligible") throw rt::JsError("expected eligible selection");
    const auto account = accounts(selection)[0];
    const double freshDeadline = now + 7 * DAY_MS; RUN(markCodexQuotaBlocked(account, httpDateQuotaResponse(freshDeadline), now + 1));
    const auto persisted = state(kv);
    const std::string classBlocksMessage = "class blocks";
    const auto& blocks = required(persisted.slots[0].quota_blocks_by_class, classBlocksMessage);
    const auto classBlock = required(blocks.get("standard"), "standard class block"); co_return FreshBlock{account, freshDeadline, classBlock};
  }
  routing::CodexQuotaClassBlock standardBlock(const std::shared_ptr<RoutingKv>& kv, std::size_t index) {
    const auto persisted = state(kv); return required(required(persisted.slots[index].quota_blocks_by_class, "class blocks").get("standard"), "standard class block");
  }
  json::object exhaustedSlot(double deadline, double observedAt, bool ambiguous) {
    json::object block{{"blocked_until_ms", deadline}, {"source", "body_resets_at"}, {"legacy_fallback", false}, {"quota_signal_observed_at_ms", observedAt},
      {"observed_reset_at_ms", deadline}, {"observed_reset_at_is_stable", true}, {"banked_reset_generation_ambiguous", ambiguous}, {"banked_reset_recovery_probe_pending", false}};
    return {{"primary_used_percent", 100}, {"quota_blocked_until_ms", deadline}, {"quota_block_source", "body_resets_at"}, {"quota_blocked_classes", json::array{"standard"}},
      {"quota_blocks_by_class", json::object{{"standard", block}}}, {"quota_signal_observed_at_ms", observedAt}, {"observed_reset_at_ms", deadline},
      {"observed_reset_at_is_stable", true}, {"banked_reset_generation_ambiguous", ambiguous}};
  }
  RoutingAccount accountOf(const RoutingAuth& auth, double slot, const std::string& hash, const std::string& credential) {
    RoutingAccount account; account.auth = auth; account.slot = slot; account.accountIdHash = hash; account.credentialVersion = credential;
    account.quotaHeadroom = 0; account.probeCircuit = nullptr; account.requestedModel = LUNA; return account;
  }
}

ASYNC_TEST("a capacity-exhausted subscription with no class circuit admits a bounded probe instead of a local dead end") {
  Fixture fixture; const double now = 1'700'000'000'000; RUN(seedCapacityExhaustedProbe(fixture.kv, now));
  STRONG(selected, singlePool, now + 2); ASSERT_EQ(kind(selected), "eligible"); const auto probeAccount = accounts(selected)[0];
  EXPECT(probeAccount.probeRequired); EXPECT(probeAccount.probeCircuit == "quota"); EXPECT_EQ(probeAccount.quotaHeadroom, 0);
  EXPECT_EQ(std::get<routing::RouteSelectionEligible>(selected).blockedAccounts.size(), std::size_t{0});
  AWAIT(claimed, claimCodexRoutingProbe(singlePool, probeAccount, now + 3)); ASSERT(claimed); EXPECT(claimed->probeGeneration); EXPECT(claimed->probeToken); EXPECT(claimed->probeCircuit == "quota");
  AWAIT(second, claimCodexRoutingProbe(singlePool, probeAccount, now + 4)); EXPECT(!second);
}

ASYNC_TEST("a capacity exhaustion with a future reset deadline is the blocked cohort identity") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000; const auto auth = harnessAuth(now); RUN(seedAuthPool(kv, auth, now));
  const json::object overrides{{"primary_used_percent", 100}, {"observed_reset_at_ms", now - 7 * DAY_MS},
    {"observed_reset_at_is_stable", true}, {"banked_reset_generation_ambiguous", true}};
  RUN(seedRoutingState(kv, auth, now, overrides)); const double resetAtMs = now + 7 * DAY_MS; RUN(seedCapacityObservation(now + 1, resetAtMs));
  STRONG(selected, singlePool, now + 2); ASSERT_EQ(kind(selected), "quota_blocked"); const auto& blocked = std::get<routing::RouteSelectionQuotaBlocked>(selected);
  EXPECT(blocked.fullCohortExhausted); ASSERT_EQ(blocked.blockedAccounts.size(), std::size_t{1}); EXPECT_EQ(blocked.blockedAccounts[0].quotaResetAtMs, resetAtMs);
}

ASYNC_TEST("a successful claimed probe clears the stuck reset ambiguity") {
  Fixture fixture; const double now = 1'700'000'000'000; RUN(seedCapacityExhaustedProbe(fixture.kv, now));
  STRONG(selected, singlePool, now + 2); ASSERT_EQ(kind(selected), "eligible"); AWAIT(claimed, claimCodexRoutingProbe(singlePool, accounts(selected)[0], now + 3));
  ASSERT(claimed); EXPECT(claimed->probeGeneration); EXPECT(claimed->probeToken); RUN(markCodexSuccess(*claimed));
  const auto slot = state(fixture.kv).slots[0]; EXPECT(!slot.banked_reset_generation_ambiguous); EXPECT(!slot.observed_reset_at_ms);
  EXPECT(!slot.observed_reset_at_is_stable); EXPECT(!slot.probe_lease);
}

ASYNC_TEST("a newer capacity sample reconciles both sibling blocks so the full cohort stays reset-fencable") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000; auto first = pool.accounts[0], second = pool.accounts[1]; first.updated_at_ms = now; second.updated_at_ms = now;
  const CodexAuthPoolState authPool{.accounts = {first, second}, .updated_at_ms = now};
  AWAIT(firstHash, routingAccountIdHash(first.account_id)); AWAIT(secondHash, routingAccountIdHash(second.account_id));
  AWAIT(firstCredential, codexCredentialVersion(first)); AWAIT(secondCredential, codexCredentialVersion(second));
  const double oldDeadline = now + 2 * DAY_MS, newDeadline = now + 7 * DAY_MS;
  RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(authPool))));
  AWAIT(firstSlot, routingSlot(first, json::object{{"primary_used_percent", 100}, {"banked_reset_generation_ambiguous", true}}));
  AWAIT(secondSlot, routingSlot(second, exhaustedSlot(oldDeadline, now - 1, true)));
  const json::value row = json::object{{"v", 2}, {"updated_at_ms", now}, {"banked_reset_legacy_identity_unresolved", false}, {"slots", json::array{firstSlot, secondSlot}}};
  RUN(kv->set(CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvValue(row)));
  const std::vector<routing::CodexCapacityRoutingObservationInput> samples{capacitySample(0, first.account_id, now + 1, newDeadline), capacitySample(1, second.account_id, now + 1, newDeadline)};
  RUN(recordCodexCapacityRoutingObservations(samples, now + 1)); AWAIT(observations, capacity::loadCodexCapacityRoutingObservations(authPool, true));
  const std::vector<RoutingAccount> routed{accountOf(first, 0, firstHash, firstCredential), accountOf(second, 1, secondHash, secondCredential)};
  for (const auto& account : routed) {
    std::optional<routing::CodexCapacityRoutingObservation> observation;
    for (const auto& item : observations) { if (item.account_id_hash == account.accountIdHash) { observation = item; break; } }
    ASSERT(observation);
    AWAIT(reconciled, capacity::reconcileCapacityQuotaBlock(account, *observation, now + 2)); EXPECT(reconciled);
    AWAIT(fence, getCodexQuotaBlockFence(account, newDeadline)); EXPECT(fence); AWAIT(oldFence, getCodexQuotaBlockFence(account, oldDeadline)); EXPECT(!oldFence);
  }
  const auto persisted = state(kv); EXPECT(!persisted.slots[0].banked_reset_generation_ambiguous); EXPECT(!persisted.slots[1].banked_reset_generation_ambiguous);
  EXPECT_EQ(standardBlock(kv, 1).observed_reset_at_ms, newDeadline);
  STRONG(selected, authPool, now + 3); ASSERT_EQ(kind(selected), "quota_blocked"); const auto& blocked = std::get<routing::RouteSelectionQuotaBlocked>(selected);
  EXPECT(blocked.fullCohortExhausted); EXPECT_EQ(blocked.blockedAccounts.size(), std::size_t{2});
  for (const auto& account : blocked.blockedAccounts) { AWAIT(fence, getCodexQuotaBlockFence(account, newDeadline)); EXPECT(fence); }
}

ASYNC_TEST("a corrected weekly deadline distinguishes an unsubmitted inventory rejection from a submitted record") {
  for (const bool submitted : {false, true}) {
    Fixture fixture;
    const double now = 1'700'000'000'000;
    const double priorDeadline = now + 60'000;
    const double weeklyDeadline = now + 7 * DAY_MS;
    const auto auth = harnessAuth(now);
    const auto kv = fixture.kv;
    RUN(seedAuthPool(kv, auth, now));
    RUN(seedRoutingState(kv, auth, now, exhaustedSlot(priorDeadline, now - 1, false)));
    AWAIT(hash, routingAccountIdHash(auth.account_id));
    AWAIT(credential, codexCredentialVersion(auth));
    const auto account = accountOf(auth, 0, hash, credential);
    const auto generation = state(kv).slots[0].generation;
    const banked::CodexBankedResetCandidate previous{
      .accountId = auth.account_id, .credentialVersion = credential, .quotaResetAtMs = priorDeadline,
      .routingGeneration = generation, .fences = {}, .requestId = std::nullopt};
    AWAIT(context, uos::codex::banked_reset_claim::makeResetContext(previous, uos::utils::sha256Hex));
    ASSERT(context);
    const auto key = banked::codexResetRedemptionKey(context->account.accountIdHash, context->account.quotaGeneration);
    const uos::types::CodexResetRedemptionRecord record{
      .v = 1, .account_id_hash = context->account.accountIdHash, .credential_version = context->account.credentialVersion,
      .quota_generation = context->account.quotaGeneration, .routing_generation = generation, .idempotency_key_hash = context->idempotencyKeyHash,
      .state = uos::types::CodexResetRedemptionState::rejected, .owner_token = "fixture-owner", .fence = 1,
      .lease_expires_at_ms = now + 30'000, .provider_receipt_id = std::nullopt, .created_at_ms = now - 3,
      .updated_at_ms = now - 1, .submitted_at_ms = submitted ? std::optional<double>(now - 2) : std::nullopt,
      .verified_at_ms = std::nullopt, .last_error_code = "inventory_empty"};
    const auto storedRecord = json::value_from(record);
    RUN(kv->set(key, rt::KvValue(storedRecord)));
    auto sample = capacitySample(0, auth.account_id, now + 1, priorDeadline, 25);
    sample.windows.secondary = routing::CodexCapacityRoutingWindow{WEEKLY_WINDOW_SECONDS, 100, weeklyDeadline};
    const std::vector samples{sample};
    RUN(recordCodexCapacityRoutingObservations(samples, now + 1));
    AWAIT(observations, capacity::loadCodexCapacityRoutingObservations(singlePool, true));
    ASSERT_EQ(observations.size(), 1U);
    AWAIT(reconciled, capacity::reconcileCapacityQuotaBlock(account, observations.front(), now + 2));
    EXPECT_EQ(reconciled, !submitted);
    EXPECT_EQ(standardBlock(kv, 0).observed_reset_at_ms, submitted ? priorDeadline : weeklyDeadline);
    EXPECT(rt::deepEqual(kv->row(key), storedRecord));
  }
}

ASYNC_TEST("an older capacity sample cannot replace a newer live quota signal") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000; const auto auth = harnessAuth(now);
  AWAIT(hash, routingAccountIdHash(auth.account_id)); AWAIT(credential, codexCredentialVersion(auth));
  const double priorDeadline = now + 2 * DAY_MS, freshCapacityDeadline = now + 7 * DAY_MS;
  RUN(seedAuthPool(kv, auth, now)); RUN(seedRoutingState(kv, auth, now, exhaustedSlot(priorDeadline, now + 10, false)));
  const auto account = accountOf(auth, 0, hash, credential); routing::CodexCapacityRoutingObservation observation;
  observation.slot = 0; observation.account_id_hash = hash; observation.state = "available"; observation.source_observed_at_ms = now; observation.snapshot_at_ms = now;
  observation.windows.primary = routing::CodexCapacityRoutingWindow{WEEKLY_WINDOW_SECONDS, 100, freshCapacityDeadline};
  AWAIT(reconciled, capacity::reconcileCapacityQuotaBlock(account, observation, now + 1)); EXPECT(!reconciled);
  const auto standard = standardBlock(kv, 0); EXPECT_EQ(standard.blocked_until_ms, priorDeadline); EXPECT_EQ(standard.observed_reset_at_ms, priorDeadline);
}

ASYNC_TEST("a fresh stable 429 supersedes an absent observation so the reset fence can arm") {
  Fixture fixture; const double now = 1'700'000'000'000;
  AWAIT(result, markFreshStableQuotaBlock(fixture.kv, now, json::object{{"banked_reset_generation_ambiguous", true}}));
  EXPECT(!result.classBlock.banked_reset_generation_ambiguous); EXPECT_EQ(result.classBlock.blocked_until_ms, result.freshDeadline);
  EXPECT_EQ(result.classBlock.observed_reset_at_ms, result.freshDeadline); EXPECT(result.classBlock.observed_reset_at_is_stable);
  AWAIT(fence, getCodexQuotaBlockFence(result.account, result.freshDeadline)); EXPECT(fence);
}

ASYNC_TEST("a fresh stable 429 supersedes an expired non-stable observation so the reset fence can arm") {
  Fixture fixture; const double now = 1'700'000'000'000;
  const json::object overrides{{"observed_reset_at_ms", now - 7 * DAY_MS}, {"observed_reset_at_is_stable", false}, {"banked_reset_generation_ambiguous", true}};
  AWAIT(result, markFreshStableQuotaBlock(fixture.kv, now, overrides)); EXPECT(!result.classBlock.banked_reset_generation_ambiguous);
  EXPECT_EQ(result.classBlock.blocked_until_ms, result.freshDeadline); EXPECT_EQ(result.classBlock.observed_reset_at_ms, result.freshDeadline);
  EXPECT(result.classBlock.observed_reset_at_is_stable); AWAIT(fence, getCodexQuotaBlockFence(result.account, result.freshDeadline)); EXPECT(fence);
}

ASYNC_TEST("an unresolved prior observation keeps the reset fence conservative") {
  Fixture fixture; const double now = 1'700'000'000'000, priorDeadline = now + 60'000;
  const json::object overrides{{"observed_reset_at_ms", priorDeadline}, {"observed_reset_at_is_stable", true}, {"banked_reset_generation_ambiguous", true}};
  AWAIT(result, markFreshStableQuotaBlock(fixture.kv, now, overrides)); EXPECT(result.classBlock.banked_reset_generation_ambiguous);
  EXPECT_EQ(result.classBlock.observed_reset_at_ms, priorDeadline); AWAIT(fence, getCodexQuotaBlockFence(result.account, result.freshDeadline)); EXPECT(!fence);
}

ASYNC_TEST("an expired stable observation stays lookup-only instead of arming a new reset fence") {
  Fixture fixture; const double now = 1'700'000'000'000, expiredDeadline = now - 7 * DAY_MS;
  const json::object overrides{{"observed_reset_at_ms", expiredDeadline}, {"observed_reset_at_is_stable", true}, {"banked_reset_generation_ambiguous", true}};
  AWAIT(result, markFreshStableQuotaBlock(fixture.kv, now, overrides)); EXPECT(result.classBlock.banked_reset_generation_ambiguous);
  EXPECT_EQ(result.classBlock.observed_reset_at_ms, expiredDeadline); AWAIT(fence, getCodexQuotaBlockFence(result.account, result.freshDeadline)); EXPECT(!fence);
  AWAIT(expiredFence, getCodexQuotaBlockFence(result.account, expiredDeadline)); EXPECT(!expiredFence);
}

ASYNC_TEST("a capacity observation that is not fresh never fabricates a probe identity") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000; const auto auth = harnessAuth(now); RUN(seedAuthPool(kv, auth, now));
  RUN(seedRoutingState(kv, auth, now, json::object{{"primary_used_percent", 100}})); RUN(seedCapacityObservation(now, now + 7 * DAY_MS));
  STRONG(selected, singlePool, now + CODEX_CAPACITY_ROUTING_MAX_AGE_MS + 1); ASSERT_EQ(kind(selected), "eligible");
  EXPECT(!accounts(selected)[0].probeRequired); EXPECT(accounts(selected)[0].probeCircuit == nullptr);
}

ASYNC_TEST("an overage-allowed account stays quota blocked on its exhausted weekly window") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000; const auto auth = harnessAuth(now); RUN(seedAuthPool(kv, auth, now));
  const json::object overrides{{"primary_used_percent", 100}, {"observed_reset_at_ms", now - 7 * DAY_MS}, {"observed_reset_at_is_stable", true}, {"banked_reset_generation_ambiguous", true}};
  RUN(seedRoutingState(kv, auth, now, overrides));
  const double resetAtMs = now + 7 * DAY_MS;
  auto sample = capacitySample(0, "one", now + 1, now + 60'000, 24);
  sample.windows.secondary = routing::CodexCapacityRoutingWindow{WEEKLY_WINDOW_SECONDS, 100, resetAtMs};
  const std::vector samples{sample};
  RUN(recordCodexCapacityRoutingObservations(samples, now + 1));
  AWAIT(plainHash, uos::utils::sha256Hex(auth.account_id)); RUN(overage::writeOverageUsage(kv, plainHash, true)); overage::resetOverageUsageCacheForTest();
  RUN(overage::loadOverageUsageSettings(singlePool, true)); AWAIT(routingHash, routingAccountIdHash(auth.account_id)); EXPECT(overage::overageUsageAllowedSync(routingHash));
  STRONG(selected, singlePool, now + 2); ASSERT_EQ(kind(selected), "quota_blocked");
  const auto& blocked = std::get<routing::RouteSelectionQuotaBlocked>(selected);
  EXPECT(blocked.fullCohortExhausted); ASSERT_EQ(blocked.blockedAccounts.size(), 1U);
  const auto& blockedAccount = blocked.blockedAccounts.front();
  EXPECT_EQ(blockedAccount.quotaResetAtMs, resetAtMs);
  AWAIT(observations, capacity::loadCodexCapacityRoutingObservations(singlePool, true)); ASSERT_EQ(observations.size(), 1U);
  AWAIT(materialized, capacity::reconcileCapacityQuotaBlock(blockedAccount, observations.front(), now + 2)); EXPECT(materialized);
  AWAIT(fence, getCodexQuotaBlockFence(blockedAccount, resetAtMs)); EXPECT(fence);
  AWAIT(primaryFence, getCodexQuotaBlockFence(blockedAccount, now + 60'000)); EXPECT(!primaryFence);
}

ASYNC_TEST("one subscription's overage allowance never changes its sibling's blocked identity") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000; auto first = pool.accounts[0], second = pool.accounts[1]; first.updated_at_ms = now; second.updated_at_ms = now;
  const CodexAuthPoolState twoPool{.accounts = {first, second}, .updated_at_ms = now}; RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(twoPool))));
  const json::object overrides{{"primary_used_percent", 100}, {"observed_reset_at_ms", now - 7 * DAY_MS}, {"observed_reset_at_is_stable", true}, {"banked_reset_generation_ambiguous", true}};
  AWAIT(firstSlot, routingSlot(first, overrides)); AWAIT(secondSlot, routingSlot(second, overrides));
  const json::value row = json::object{{"v", 2}, {"updated_at_ms", now}, {"slots", json::array{firstSlot, secondSlot}}}; RUN(kv->set(CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvValue(row)));
  const double resetAtMs = now + 7 * DAY_MS; const std::vector<routing::CodexCapacityRoutingObservationInput> samples{
    capacitySample(0, "one", now + 1, resetAtMs), capacitySample(1, "two", now + 1, resetAtMs)}; RUN(recordCodexCapacityRoutingObservations(samples, now + 1));
  AWAIT(plainHash, uos::utils::sha256Hex(std::string("one"))); RUN(overage::writeOverageUsage(kv, plainHash, true));
  overage::resetOverageUsageCacheForTest(); RUN(overage::loadOverageUsageSettings(twoPool, true));
  STRONG(selected, twoPool, now + 2); ASSERT_EQ(kind(selected), "quota_blocked");
  const auto& blocked = std::get<routing::RouteSelectionQuotaBlocked>(selected).blockedAccounts; ASSERT_EQ(blocked.size(), std::size_t{2});
  EXPECT_EQ(blocked[0].auth.account_id, "one"); EXPECT_EQ(blocked[0].quotaResetAtMs, resetAtMs);
  EXPECT_EQ(blocked[1].auth.account_id, "two"); EXPECT_EQ(blocked[1].quotaResetAtMs, resetAtMs);
  const std::vector recovered{capacitySample(1, "two", now + 3, resetAtMs, 50)};
  RUN(recordCodexCapacityRoutingObservations(recovered, now + 3));
  STRONG(withHeadroom, twoPool, now + 4); ASSERT_EQ(kind(withHeadroom), "eligible");
  ASSERT_EQ(accounts(withHeadroom).size(), 1U); EXPECT_EQ(accounts(withHeadroom).front().auth.account_id, "two");
}

TEST("capacity reset identity follows the exact model and expires with the exhausted window") {
  namespace decisions = uos::authority::native_codex_capacity;
  const double now = 1'700'000'000'000;
  routing::CodexCapacityRoutingObservation observation;
  observation.state = "available";
  observation.snapshot_at_ms = now;
  observation.windows.primary = routing::CodexCapacityRoutingWindow{18'000, 100, now - 1};
  observation.windows.secondary = routing::CodexCapacityRoutingWindow{WEEKLY_WINDOW_SECONDS, 12, now + 7 * DAY_MS};
  const double scopedReset = now + DAY_MS;
  observation.additional_rate_limits.push_back({"gpt-scoped", std::nullopt, {
    routing::CodexCapacityRoutingWindow{18'000, 100, now + 60'000},
    routing::CodexCapacityRoutingWindow{WEEKLY_WINDOW_SECONDS, 100, scopedReset},
  }});
  EXPECT_EQ(decisions::capacityExhaustedResetAtMs(observation, std::nullopt, now), std::nullopt);
  EXPECT_EQ(decisions::capacityExhaustedResetAtMs(observation, "gpt-scoped", now), scopedReset);
  auto conflicting = observation.additional_rate_limits.front();
  conflicting.windows.secondary->reset_at_ms = scopedReset + DAY_MS;
  observation.additional_rate_limits.push_back(conflicting);
  EXPECT_EQ(decisions::capacityExhaustedResetAtMs(observation, "gpt-scoped", now), std::nullopt);
  observation.additional_rate_limits.pop_back();
  observation.additional_rate_limits.front().windows.primary->reset_at_ms = std::nullopt;
  EXPECT_EQ(decisions::capacityExhaustedResetAtMs(observation, "gpt-scoped", now), std::nullopt);
}
