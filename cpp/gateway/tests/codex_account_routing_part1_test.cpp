// Links: tests/codex_account_routing_part1_test.cpp tests/helpers/codex_account_routing_harness.cpp tests/helpers/counting_kv.cpp src/auth/relay.cpp src/authority/native_codex.cpp
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
// Port of tests/codex-account-routing-part1.test.ts: all 25 original cases plus one helper differential; no deferred cases.
#include "tests/helpers/codex_account_routing_harness.hpp"
#include "uos/config.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace routing = uos::codex::routing_state;
  using namespace uos::tests::helpers::codex_account_routing_harness;
  using routing::RoutingAccount;
  using routing::RouteSelection;
  using routing::CodexRoutingSlot;
  using routing::CodexQuotaClassBlock;
  using routing::CodexCapacityRoutingObservationInput;
  using uos::codex::routing_mutations::CodexQuotaResetReconciliationInput;

  struct Fixture {
    rt::env::ScopedEnvironment environment{{"HOME", "/nonexistent/cpp-c1-routing"}};
    std::string savedSeed = uos::config::config.codexAuthJsonB64;
    std::shared_ptr<RoutingKv> kv = std::make_shared<RoutingKv>();
    Fixture() {
      uos::config::config.codexAuthJsonB64.clear();
      setKvForTest(kv);
      resetCodexAccountRoutingForTest();
      resetCodexAuthCacheForTest();
      resetProviderSelectionCacheForTest();
    }
    ~Fixture() {
      setKvForTest(nullptr);
      resetCodexAccountRoutingForTest();
      resetCodexAuthCacheForTest();
      resetProviderSelectionCacheForTest();
      uos::config::config.codexAuthJsonB64 = savedSeed;
    }
  };
  std::string kind(const RouteSelection& value) { return std::visit([](const auto& item) { return std::string(item.kind); }, value); }
  const std::vector<RoutingAccount>& accounts(const RouteSelection& value) { return std::get<routing::RouteSelectionEligible>(value).accounts; }
  std::vector<double> slots(const RouteSelection& value) {
    std::vector<double> out;
    for (const auto& account : accounts(value)) out.push_back(account.slot);
    return out;
  }
  std::optional<routing::CodexAccountRoutingState> state(const std::shared_ptr<RoutingKv>& kv) {
    return parseCodexAccountRoutingState(kv->row(CODEX_ACCOUNT_ROUTING_KV_KEY));
  }
  CodexRoutingSlot slot(const std::shared_ptr<RoutingKv>& kv, std::size_t index = 0) { return required(state(kv), "persisted routing state").slots.at(index); }
  CodexQuotaClassBlock block(const CodexRoutingSlot& value, const std::string& name) {
    return required(required(value.quota_blocks_by_class, "class blocks").get(name), "class block " + name);
  }
  rt::Response quotaBody(double deadline, std::optional<std::string> retryAfter = std::nullopt) {
    json::object error{{"type", "usage_limit_reached"}, {"resets_at", rt::makeNumber(rt::Math::floor(deadline / 1'000))}};
    rt::ResponseInit init;
    init.status = 429;
    init.headers = rt::Headers{{"Content-Type", "application/json"}};
    if (retryAfter) init.headers->set("Retry-After", *retryAfter);
    return rt::Response(rt::BodyInit(rt::jsonStringify(json::object{{"error", error}})), init);
  }
  rt::Response json429(const json::value& body, std::optional<std::string> retryAfter = std::nullopt) {
    rt::ResponseInit init;
    init.status = 429;
    init.headers = rt::Headers{{"Content-Type", "application/json"}};
    if (retryAfter) init.headers->set("Retry-After", *retryAfter);
    return rt::Response(rt::BodyInit(rt::jsonStringify(body)), init);
  }
  json::object legacySlot(const RoutingAccount& account, double deadline, double now) {
    return {{"account_id_hash", account.accountIdHash}, {"credential_version", account.credentialVersion},
            {"quota_blocked_until_ms", rt::makeNumber(deadline)}, {"quota_block_source", "header_retry_after"},
            {"quota_blocked_classes", json::array{"standard"}}, {"invalid_credential_version", nullptr},
            {"primary_used_percent", nullptr}, {"secondary_used_percent", nullptr}, {"quota_signal_observed_at_ms", rt::makeNumber(now)},
            {"capacity_observed_at_ms", nullptr}, {"upstream_timeout_blocked_until_ms", nullptr}, {"observed_reset_at_ms", nullptr},
            {"observed_reset_at_is_stable", false}, {"banked_reset_generation_ambiguous", false},
            {"banked_reset_recovery_probe_pending", false}, {"generation", 1}, {"probe_lease", nullptr}};
  }
  json::value stateWith(const json::object& value, double now, bool unresolved = false) {
    return json::object{{"v", 2}, {"updated_at_ms", rt::makeNumber(now)}, {"banked_reset_legacy_identity_unresolved", unresolved}, {"slots", json::array{value}}};
  }
  CodexCapacityRoutingObservationInput observation(double now, double used, double deadline) {
    CodexCapacityRoutingObservationInput out;
    out.slot = 0; out.account_id = "one"; out.state = "available";
    out.source_observed_at_ms = now; out.snapshot_at_ms = now;
    out.windows.primary = routing::CodexCapacityRoutingWindow{604'800, used, deadline};
    return out;
  }
  std::vector<routing::CodexBlockedRoutingAccount> blocked(const RouteSelection& value) {
    if (const auto* eligible = std::get_if<routing::RouteSelectionEligible>(&value)) return eligible->blockedAccounts;
    return std::get<routing::RouteSelectionQuotaBlocked>(value).blockedAccounts;
  }
}
#define AWAIT(name, expression) auto name##Pending = rt::Promise(expression); auto name = co_await name##Pending.wait()
#define RUN(expression) do { auto pending = rt::Promise(expression); co_await pending.wait(); } while (false)
#define SELECT(name, poolValue, nowValue, ...) AWAIT(name, selectCodexRoutingAccounts(poolValue, (poolValue).accounts, nowValue __VA_OPT__(,) __VA_ARGS__))

ASYNC_TEST("v2 routing ignores the v1 key and rejects v1 payloads") {
  Fixture fixture; auto kv = fixture.kv;
  const double now = 1'700'000'000'000;
  AWAIT(credentialVersion, codexCredentialVersion(singlePool.accounts[0]));
  const json::value v1State = json::object{{"v", 1}, {"updated_at_ms", now}, {"slots", json::array{json::object{
    {"credential_version", credentialVersion}, {"quota_blocked_until_ms", now + 60'000}, {"quota_block_source", "cooldown"},
    {"invalid_credential_version", credentialVersion}, {"primary_used_percent", nullptr}, {"secondary_used_percent", nullptr},
    {"observed_reset_at_ms", nullptr}, {"generation", 1}, {"probe_lease", nullptr}}}}};
  RUN(kv->set(rt::KvKey{"uos_ai", "codex_account_routing", "v1"}, rt::KvValue(v1State)));
  EXPECT(!parseCodexAccountRoutingState(v1State));
  SELECT(selected, singlePool, now);
  EXPECT_EQ(kind(selected), "eligible");
  EXPECT(!kv->values.has(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
}

ASYNC_TEST("quota circuits isolate Spark, GPT-OSS, and standard model pools") {
  Fixture fixture; auto kv = fixture.kv;
  const double now = 1'700'000'000'000;
  const double sparkDeadline = now + 7 * 24 * 60 * 60'000, standardDeadline = now + 60 * 60'000;
  SELECT(spark, singlePool, now, "gpt-5.3-codex-spark"); ASSERT_EQ(kind(spark), "eligible");
  RUN(markCodexQuotaBlocked(accounts(spark)[0], httpDateQuotaResponse(sparkDeadline), now));
  SELECT(sparkBlocked, singlePool, now + 1, "gpt-5.3-codex-spark"); EXPECT_EQ(kind(sparkBlocked), "quota_blocked");
  SELECT(luna, singlePool, now + 1, "gpt-5.6-luna"); ASSERT_EQ(kind(luna), "eligible");
  RUN(markCodexQuotaBlocked(accounts(luna)[0], httpDateQuotaResponse(standardDeadline), now + 1));
  AWAIT(sparkFence, getCodexQuotaBlockFence(accounts(spark)[0], sparkDeadline)); EXPECT(sparkFence.has_value());
  AWAIT(lunaFence, getCodexQuotaBlockFence(accounts(luna)[0], standardDeadline)); EXPECT(lunaFence.has_value());
  SELECT(terra, singlePool, now + 2, "gpt-5.6-terra"); EXPECT_EQ(kind(terra), "quota_blocked");
  SELECT(oss, singlePool, now + 2, "gpt-oss-120b"); EXPECT_EQ(kind(oss), "eligible");
  SELECT(afterStandardReset, singlePool, standardDeadline + 1, "gpt-5.6-luna"); EXPECT_EQ(kind(afterStandardReset), "eligible");
  SELECT(stillSpark, singlePool, standardDeadline + 1, "gpt-5.3-codex-spark"); EXPECT_EQ(kind(stillSpark), "quota_blocked");
  auto input = observation(now + 2, 50, standardDeadline);
  input.additional_rate_limits = {{"GPT-5.3-Codex-Spark", std::string("codex_bengalfox"), {{routing::CodexCapacityRoutingWindow{604'800, 100, sparkDeadline}}, std::nullopt}}};
  RUN(recordCodexCapacityRoutingObservations(std::vector<CodexCapacityRoutingObservationInput>{input}, now + 2));
  SELECT(recoveredLuna, singlePool, now + 3, "gpt-5.6-luna"); EXPECT_EQ(kind(recoveredLuna), "eligible");
  SELECT(exhaustedSpark, singlePool, now + 3, "gpt-5.3-codex-spark"); EXPECT_EQ(kind(exhaustedSpark), "quota_blocked");
}

ASYNC_TEST("reserve-class quota circuits do not fence the standard class on the same account") {
  Fixture fixture; auto kv = fixture.kv;
  const double now = 1'700'000'000'000, standardDeadline = now + 60'000, reserveDeadline = standardDeadline + 60'000;
  SELECT(standard, singlePool, now, "gpt-5.6-luna"); ASSERT_EQ(kind(standard), "eligible");
  RUN(markCodexQuotaBlocked(accounts(standard)[0], httpDateQuotaResponse(standardDeadline), now));
  SELECT(reserveUnfenced, singlePool, now + 1, "gpt-reserve"); EXPECT_EQ(kind(reserveUnfenced), "eligible");
  SELECT(terra, singlePool, now + 1, "gpt-5.6-terra"); EXPECT_EQ(kind(terra), "quota_blocked");
  SELECT(recovered, singlePool, standardDeadline + 1, "gpt-5.6-luna"); EXPECT_EQ(kind(recovered), "eligible");
  SELECT(reserve, singlePool, standardDeadline + 1, "gpt-reserve"); ASSERT_EQ(kind(reserve), "eligible");
  RUN(markCodexQuotaBlocked(accounts(reserve)[0], httpDateQuotaResponse(reserveDeadline), standardDeadline + 1));
  SELECT(blockedReserve, singlePool, standardDeadline + 2, "gpt-reserve"); EXPECT_EQ(kind(blockedReserve), "quota_blocked");
  SELECT(luna, singlePool, standardDeadline + 2, "gpt-5.6-luna"); EXPECT_EQ(kind(luna), "eligible");
  SELECT(terraRecovered, singlePool, standardDeadline + 2, "gpt-5.6-terra"); EXPECT_EQ(kind(terraRecovered), "eligible");
  const auto persisted = state(kv); ASSERT(persisted);
  EXPECT_EQ(block(persisted->slots[0], "standard").blocked_until_ms, standardDeadline);
  EXPECT_EQ(block(persisted->slots[0], "reserve").blocked_until_ms, reserveDeadline);
}

ASYNC_TEST("an unrelated class 429 is recorded while another class owns a probe lease") {
  Fixture fixture; auto kv = fixture.kv;
  const double now = 1'700'000'000'000, sparkDeadline = now + 60'000, standardDeadline = sparkDeadline + 60'000;
  SELECT(spark, singlePool, now, "gpt-5.3-codex-spark"); ASSERT_EQ(kind(spark), "eligible");
  RUN(markCodexQuotaBlocked(accounts(spark)[0], quotaBody(sparkDeadline), now));
  SELECT(expiredSpark, singlePool, sparkDeadline + 1, "gpt-5.3-codex-spark"); ASSERT_EQ(kind(expiredSpark), "eligible");
  AWAIT(claimed, claimCodexRoutingProbe(singlePool, accounts(expiredSpark)[0], sparkDeadline + 1)); ASSERT(claimed);
  SELECT(standard, singlePool, sparkDeadline + 2, "gpt-5.6-luna"); ASSERT_EQ(kind(standard), "eligible");
  RUN(markCodexQuotaBlocked(accounts(standard)[0], quotaBody(standardDeadline), sparkDeadline + 2));
  const auto persisted = slot(kv);
  EXPECT_EQ(block(persisted, "standard").blocked_until_ms, standardDeadline);
  ASSERT(persisted.probe_lease); EXPECT_EQ(persisted.probe_lease->quota_class, "spark");
}

ASYNC_TEST("legacy named class blocks remain enforced until migrated") {
  Fixture fixture; auto kv = fixture.kv;
  const double now = 1'700'000'000'000, deadline = now + 60'000;
  SELECT(initial, singlePool, now, "gpt-5.6-luna"); ASSERT_EQ(kind(initial), "eligible");
  RUN(kv->set(CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvValue(stateWith(legacySlot(accounts(initial)[0], deadline, now), now))));
  resetCodexAccountRoutingForTest();
  SELECT(luna, singlePool, now + 1, "gpt-5.6-luna"); EXPECT_EQ(kind(luna), "quota_blocked");
  SELECT(spark, singlePool, now + 1, "gpt-5.3-codex-spark"); EXPECT_EQ(kind(spark), "eligible");
}

ASYNC_TEST("a legacy class block survives migration when another class is exhausted") {
  Fixture fixture; auto kv = fixture.kv;
  const double now = 1'700'000'000'000, standardDeadline = now + 60'000, sparkDeadline = now + 120'000;
  SELECT(initial, singlePool, now, "gpt-5.6-luna"); ASSERT_EQ(kind(initial), "eligible");
  auto legacy = legacySlot(accounts(initial)[0], standardDeadline, now); legacy["quota_blocks_by_class"] = json::object{};
  RUN(kv->set(CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvValue(stateWith(legacy, now)))); resetCodexAccountRoutingForTest();
  SELECT(spark, singlePool, now + 1, "gpt-5.3-codex-spark"); ASSERT_EQ(kind(spark), "eligible");
  RUN(markCodexQuotaBlocked(accounts(spark)[0], httpDateQuotaResponse(sparkDeadline), now + 1));
  const auto migrated = slot(kv);
  EXPECT_EQ(block(migrated, "standard").blocked_until_ms, standardDeadline); EXPECT_EQ(block(migrated, "spark").blocked_until_ms, sparkDeadline);
  SELECT(luna, singlePool, now + 2, "gpt-5.6-luna"); EXPECT_EQ(kind(luna), "quota_blocked");
}

ASYNC_TEST("an unclassified legacy block stays on other classes after capacity clears one") {
  Fixture fixture; auto kv = fixture.kv;
  const double now = 1'700'000'000'000, legacyDeadline = now + 60'000;
  SELECT(initial, singlePool, now, "gpt-5.6-luna"); ASSERT_EQ(kind(initial), "eligible");
  auto legacy = legacySlot(accounts(initial)[0], legacyDeadline, now);
  legacy["quota_blocked_classes"] = json::array{}; legacy["quota_blocks_by_class"] = json::object{};
  RUN(kv->set(CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvValue(stateWith(legacy, now)))); resetCodexAccountRoutingForTest();
  auto input = observation(now + 1, 100, legacyDeadline);
  input.additional_rate_limits = {{"gpt-5.3-codex-spark", std::string("codex_bengalfox"), {{routing::CodexCapacityRoutingWindow{604'800, 50, legacyDeadline}}, std::nullopt}}};
  RUN(recordCodexCapacityRoutingObservations(std::vector<CodexCapacityRoutingObservationInput>{input}, now + 1));
  SELECT(spark, singlePool, now + 2, "gpt-5.3-codex-spark"); EXPECT_EQ(kind(spark), "eligible");
  const auto migrated = slot(kv); ASSERT(migrated.quota_blocks_by_class);
  EXPECT(!migrated.quota_blocks_by_class->has("spark")); EXPECT(!migrated.quota_blocks_by_class->has("unknown"));
  EXPECT_EQ(block(migrated, "standard").blocked_until_ms, legacyDeadline); EXPECT_EQ(block(migrated, "gpt_oss_120b").blocked_until_ms, legacyDeadline);
  SELECT(luna, singlePool, now + 2, "gpt-5.6-luna"); EXPECT_EQ(kind(luna), "quota_blocked");
}

ASYNC_TEST("migrates an unmarked synthetic unknown block from the prior class release") {
  Fixture fixture; auto kv = fixture.kv;
  const double now = 1'700'000'000'000, legacyDeadline = now + 120'000, standardDeadline = now + 180'000;
  SELECT(initial, singlePool, now, "gpt-5.6-luna"); ASSERT_EQ(kind(initial), "eligible");
  RUN(markCodexQuotaBlocked(accounts(initial)[0], httpDateQuotaResponse(standardDeadline), now));
  const auto persisted = state(kv); ASSERT(persisted);
  auto changed = routing::toJson(*persisted);
  auto& changedSlot = changed.at("slots").as_array()[0].as_object();
  changedSlot["quota_blocked_until_ms"] = standardDeadline; changedSlot["quota_block_source"] = "header_retry_after";
  changedSlot["quota_blocked_classes"] = json::array{"spark", "gpt_oss_120b", "standard", "unknown"};
  json::object blocks;
  for (const std::string name : {"spark", "gpt_oss_120b", "standard", "unknown"}) blocks[name] = syntheticLegacyBlock(legacyDeadline, now);
  changedSlot["quota_blocks_by_class"] = blocks; changedSlot["quota_signal_observed_at_ms"] = now;
  RUN(kv->set(CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvValue(changed))); resetCodexAccountRoutingForTest();
  auto input = observation(now + 1, 50, standardDeadline);
  RUN(recordCodexCapacityRoutingObservations(std::vector<CodexCapacityRoutingObservationInput>{input}, now + 1));
  SELECT(selected, singlePool, now + 2, "gpt-5.6-luna"); (void)selected;
  const auto migrated = slot(kv); ASSERT(migrated.quota_blocks_by_class);
  EXPECT(!migrated.quota_blocks_by_class->has("standard")); EXPECT(!migrated.quota_blocks_by_class->has("unknown"));
  EXPECT_EQ(block(migrated, "spark").blocked_until_ms, legacyDeadline); EXPECT_EQ(block(migrated, "gpt_oss_120b").blocked_until_ms, legacyDeadline);
}

TEST("does not mark a coincidentally matching unknown block as synthetic") {
  const double now = 1'700'000'000'000, matchingDeadline = now + 60'000;
  const json::value value = json::object{{"v", 2}, {"updated_at_ms", now}, {"slots", json::array{json::object{
    {"credential_version", "credential"}, {"quota_block_source", nullptr}, {"probe_lease", nullptr}, {"quota_blocks_by_class", json::object{
      {"spark", syntheticLegacyBlock(matchingDeadline, now)}, {"gpt_oss_120b", syntheticLegacyBlock(matchingDeadline + 1, now)},
      {"standard", syntheticLegacyBlock(matchingDeadline + 2, now)}, {"unknown", syntheticLegacyBlock(matchingDeadline, now)}}}}}}};
  const auto parsed = parseCodexAccountRoutingState(value); ASSERT(parsed);
  EXPECT_EQ(block(parsed->slots[0], "unknown").legacy_fallback, false);
}

ASYNC_TEST("an administrative recheck marks every class reset fence ambiguous") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000, resetAtMs = now + 60'000;
  SELECT(initial, singlePool, now, "gpt-5.3-codex-spark"); ASSERT_EQ(kind(initial), "eligible");
  RUN(markCodexQuotaBlocked(accounts(initial)[0], quotaBody(resetAtMs), now));
  EXPECT_EQ(block(slot(kv), "spark").banked_reset_generation_ambiguous, false);
  AWAIT(rechecked, recheckCodexRoutingSlot(1)); EXPECT_EQ(rechecked, true);
  EXPECT_EQ(block(slot(kv), "spark").banked_reset_generation_ambiguous, true);
  AWAIT(fence, getCodexQuotaBlockFence(accounts(initial)[0], resetAtMs)); EXPECT(!fence);
}

ASYNC_TEST("a successful class probe does not leave recovery pending on another class") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000, sparkDeadline = now + 60'000, standardDeadline = now + 120'000;
  SELECT(spark, singlePool, now, "gpt-5.3-codex-spark"); ASSERT_EQ(kind(spark), "eligible");
  RUN(markCodexQuotaBlocked(accounts(spark)[0], httpDateQuotaResponse(sparkDeadline), now));
  SELECT(standard, singlePool, now + 1, "gpt-5.6-luna"); ASSERT_EQ(kind(standard), "eligible");
  RUN(markCodexQuotaBlocked(accounts(standard)[0], httpDateQuotaResponse(standardDeadline), now + 1));
  AWAIT(routingGeneration, getCodexQuotaBlockFence(accounts(spark)[0], sparkDeadline)); ASSERT(routingGeneration);
  AWAIT(recovery, reconcileCodexQuotaAfterVerifiedReset(accounts(spark)[0], CodexQuotaResetReconciliationInput{sparkDeadline, *routingGeneration})); ASSERT(recovery);
  RUN(markCodexSuccess(*recovery)); EXPECT_EQ(slot(kv).banked_reset_recovery_probe_pending, false);
  const double nextDeadline = standardDeadline + 60'000;
  SELECT(standardProbe, singlePool, standardDeadline + 1, "gpt-5.6-luna"); ASSERT_EQ(kind(standardProbe), "eligible");
  AWAIT(claimed, claimCodexRoutingProbe(singlePool, accounts(standardProbe)[0], standardDeadline + 1)); ASSERT(claimed);
  RUN(markCodexQuotaBlocked(*claimed, httpDateQuotaResponse(nextDeadline), standardDeadline + 1));
  EXPECT_EQ(block(slot(kv), "standard").blocked_until_ms, nextDeadline);
}

ASYNC_TEST("legacy response-header timeout fences are ignored by live routing") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000;
  SELECT(initial, pool, now); ASSERT_EQ(kind(initial), "eligible");
  RUN(markCodexUpstreamTimeout(accounts(initial)[0], now));
  EXPECT_EQ(slot(kv).upstream_timeout_blocked_until_ms, now + CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS);
  resetCodexAccountRoutingForTest(); SELECT(sibling, pool, now + 1); ASSERT_EQ(kind(sibling), "eligible");
  EXPECT_EQ(slots(sibling), (std::vector<double>{0, 1})); EXPECT(std::get<routing::RouteSelectionEligible>(sibling).skippedSlots.empty());
  RUN(markCodexUpstreamTimeout(accounts(sibling)[1], now + 1)); resetCodexAccountRoutingForTest();
  SELECT(stillEligible, pool, now + 2); ASSERT_EQ(kind(stillEligible), "eligible"); EXPECT_EQ(slots(stillEligible), (std::vector<double>{0, 1}));
}

ASYNC_TEST("a held legacy timeout probe is discarded before live routing") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000;
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible"); const auto account = accounts(initial)[0];
  RUN(markCodexUpstreamTimeout(account, now - CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS - 1));
  auto persisted = state(kv); ASSERT(persisted); const double probeExpiresAtMs = now + 1'000;
  auto changed = routing::toJson(*persisted); changed["updated_at_ms"] = now;
  auto& changedSlot = changed.at("slots").as_array()[0].as_object();
  changedSlot["quota_blocked_until_ms"] = now - 1; changedSlot["quota_block_source"] = "header_retry_after";
  changedSlot["upstream_timeout_blocked_until_ms"] = now - 1;
  changedSlot["probe_lease"] = json::object{{"token", "held-timeout-probe"}, {"expires_at_ms", probeExpiresAtMs}, {"generation", persisted->slots[0].generation}, {"circuit", "upstream_timeout"}};
  RUN(kv->set(CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvValue(changed))); resetCodexAccountRoutingForTest();
  SELECT(selected, singlePool, now); ASSERT_EQ(kind(selected), "eligible");
  EXPECT_EQ(accounts(selected)[0].probeRequired, true); EXPECT_EQ(accounts(selected)[0].probeCircuit, "quota");
}

ASYNC_TEST("a held quota probe does not misclassify a stale timeout as upstream blocked") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000;
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible"); const auto account = accounts(initial)[0];
  RUN(markCodexUpstreamTimeout(account, now - CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS - 1));
  const auto persisted = state(kv); ASSERT(persisted); const double probeExpiresAtMs = now + 1'000;
  auto changed = routing::toJson(*persisted); changed["updated_at_ms"] = now;
  auto& changedSlot = changed.at("slots").as_array()[0].as_object();
  changedSlot["quota_blocked_until_ms"] = now - 1; changedSlot["quota_block_source"] = "header_retry_after";
  changedSlot["upstream_timeout_blocked_until_ms"] = now - 1;
  changedSlot["probe_lease"] = json::object{{"token", "held-quota-probe"}, {"expires_at_ms", probeExpiresAtMs}, {"generation", persisted->slots[0].generation}, {"circuit", "quota"}};
  RUN(kv->set(CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvValue(changed))); resetCodexAccountRoutingForTest();
  SELECT(selected, singlePool, now); ASSERT_EQ(kind(selected), "quota_blocked");
  EXPECT_EQ(std::get<routing::RouteSelectionQuotaBlocked>(selected).retryAtMs, probeExpiresAtMs);
}

ASYNC_TEST("a legacy timeout fence cannot hide a quota-blocked sibling") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000;
  SELECT(initial, pool, now); ASSERT_EQ(kind(initial), "eligible");
  const auto one = std::find_if(accounts(initial).begin(), accounts(initial).end(), [](const auto& account) { return account.auth.account_id == "one"; });
  const auto two = std::find_if(accounts(initial).begin(), accounts(initial).end(), [](const auto& account) { return account.auth.account_id == "two"; });
  ASSERT(one != accounts(initial).end()); ASSERT(two != accounts(initial).end());
  RUN(markCodexQuotaBlocked(*one, deltaSecondsQuotaResponse("60"), now)); RUN(markCodexUpstreamTimeout(*two, now));
  resetCodexAccountRoutingForTest(); SELECT(selected, pool, now + 1); ASSERT_EQ(kind(selected), "eligible");
  EXPECT_EQ(slots(selected), (std::vector<double>{1})); EXPECT_EQ(std::get<routing::RouteSelectionEligible>(selected).skippedSlots, (std::vector<double>{1}));
}

ASYNC_TEST("fetchCodexResponses does not gate the next request after a timeout") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow();
  auto authPool = pool; authPool.updated_at_ms = now;
  for (auto& account : authPool.accounts) account.updated_at_ms = now;
  std::vector<std::string> calls; int dispatches = 0; rt::AbortController timeoutController;
  RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(authPool))));
  rt::Fetch mock([&](rt::RequestInfo input, rt::RequestInit init) -> rt::Task<rt::Response> {
    const std::string url = requestUrl(input);
    if (!rt::endsWith(url, "/responses")) throw rt::JsError("Unexpected Codex URL: " + url);
    calls.push_back(init.headers.value_or(rt::Headers()).get("ChatGPT-Account-ID").value_or(""));
    if (calls.size() == 1) {
      timeoutController.abort(rt::makeReason(rt::DOMException("Codex fixture timeout", "TimeoutError")));
      return rt::Promise<rt::Response>::reject(timeoutController.signal().reason()).wait();
    }
    rt::ResponseInit responseInit; responseInit.status = 200;
    return rt::fulfilledTask(rt::Response(nullptr, responseInit));
  });
  rt::GlobalFetchOverride fetchScope(mock);
  uos::codex::dispatch::FetchCodexResponsesOptions options;
  options.signal = timeoutController.signal();
  options.beforeDispatch = std::function<rt::Task<std::optional<uos::api_key_policy::ApiKeyProviderDispatch>>()>([&] {
    ++dispatches;
    return rt::fulfilledTask(std::optional<uos::api_key_policy::ApiKeyProviderDispatch>{});
  });
  bool rejected = false;
  try {
    RUN(fetchCodexResponses(json::object{{"model", "gpt-5-routing"}, {"input", "timeout"}}, options));
  } catch (const CodexError& error) {
    rejected = true; EXPECT_EQ(error.code, "gateway_timeout"); EXPECT_EQ(error.status, 504);
  }
  EXPECT(rejected); EXPECT_EQ(dispatches, 1); resetCodexAccountRoutingForTest();
  AWAIT(response, fetchCodexResponses(json::object{{"model", "gpt-5-routing"}, {"input", "next request"}}));
  EXPECT_EQ(response.status(), 200); EXPECT_EQ(calls, (std::vector<std::string>{"one", "one"})); RUN(response.arrayBuffer());
  for (int turn = 0; turn < 60; ++turn) co_await rt::microtask();
}

ASYNC_TEST("fetchCodexResponses preserves a JSON 429 without Retry-After when no retry time is known") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow();
  auto authPool = singlePool; authPool.updated_at_ms = now; authPool.accounts[0].updated_at_ms = now;
  int calls = 0;
  RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(authPool))));
  const json::value expected = json::object{{"error", json::object{{"message", "Codex is temporarily rate limited."},
    {"type", "rate_limit_error"}, {"code", "codex_rate_limited"}, {"param", nullptr}}}};
  rt::Fetch mock([&](rt::RequestInfo input, rt::RequestInit) {
    const std::string url = requestUrl(input);
    if (!rt::endsWith(url, "/responses")) throw rt::JsError("Unexpected Codex URL: " + url);
    ++calls; return rt::fulfilledTask(json429(expected));
  });
  rt::GlobalFetchOverride fetchScope(mock);
  uos::codex::dispatch::FetchCodexResponsesOptions options;
  options.retrySleep = [](double) { return rt::fulfilledTask(); };
  AWAIT(response, fetchCodexResponses(json::object{{"model", "gpt-5-routing"}, {"input", "rate-limit"}}, options));
  EXPECT_EQ(calls, 2); EXPECT_EQ(response.status(), 429);
  EXPECT_EQ(response.headers().get("content-type"), "application/json"); EXPECT(!response.headers().get("Retry-After"));
  AWAIT(body, response.json()); EXPECT(rt::deepEqual(body, expected));
  for (int turn = 0; turn < 60; ++turn) co_await rt::microtask();
}

ASYNC_TEST("ordinary and incomplete 429 variants never persist a quota block") {
  const double now = 1'700'000'000'000, futureResetSeconds = rt::Math::floor(now / 1'000) + 120;
  struct Case { std::string name; std::function<rt::Response()> response; bool usageLimitReached; std::optional<double> retryAtMs; };
  const json::value usage = json::object{{"error", json::object{{"type", "usage_limit_reached"}}}};
  const auto withReset = [&](json::value reset) { return json::value(json::object{{"error", json::object{{"type", "usage_limit_reached"}, {"resets_at", reset}}}}); };
  const std::vector<Case> cases{
    {"bare", [] { rt::ResponseInit init; init.status = 429; return rt::Response(nullptr, init); }, false, std::nullopt},
    {"generic with valid Retry-After", [] { return json429(json::object{{"error", json::object{{"type", "rate_limit_error"}}}}, "60"); }, false, now + 60'000},
    {"usage limit with string body reset", [&] { return json429(withReset(json::value(rt::numberToString(futureResetSeconds)))); }, true, std::nullopt},
    {"usage limit with fractional body reset", [&] { return json429(withReset(rt::makeNumber(futureResetSeconds + 0.5))); }, true, std::nullopt},
    {"usage limit with expired body reset", [&] { return json429(withReset(rt::makeNumber(rt::Math::floor(now / 1'000) - 1))); }, true, std::nullopt},
    {"usage limit with overflowing body reset", [&] { return json429(withReset(rt::makeNumber(rt::Number::MAX_SAFE_INTEGER))); }, true, std::nullopt},
    {"usage limit without Retry-After", [&] { return json429(usage); }, true, std::nullopt},
    {"usage limit with invalid decimal Retry-After", [&] { return json429(usage, "1.5"); }, true, std::nullopt},
    {"truncated usage limit with valid Retry-After", [] { return json429(json::object{{"error", json::object{{"type", "usage_limit_reached"}, {"detail", std::string(70 * 1'024, 'x')}}}}, "60"); }, false, now + 60'000},
  };
  for (const auto& testCase : cases) {
    Fixture fixture; auto kv = fixture.kv;
    SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible");
    AWAIT(classified, markCodexQuotaBlocked(accounts(initial)[0], testCase.response(), now));
    EXPECT_EQ(classified.response.status(), 429); EXPECT_EQ(classified.usageLimitReached, testCase.usageLimitReached);
    EXPECT_EQ(classified.retryAtMs, testCase.retryAtMs); EXPECT(!kv->values.has(key(CODEX_ACCOUNT_ROUTING_KV_KEY)));
    resetCodexAccountRoutingForTest(); SELECT(selected, singlePool, now + 1); EXPECT_EQ(kind(selected), "eligible");
  }
}

ASYNC_TEST("exact future body resets_at durably identifies the Codex quota window") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000;
  const double resetAtSeconds = rt::Math::floor(now / 1'000) + 120, resetAtMs = resetAtSeconds * 1'000;
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible");
  auto response = json429(json::object{{"error", json::object{{"type", "usage_limit_reached"}, {"resets_at", resetAtSeconds}, {"resets_in_seconds", 120}}}});
  response.headers().set("x-codex-primary-reset-at", rt::numberToString(resetAtSeconds));
  AWAIT(classified, markCodexQuotaBlocked(accounts(initial)[0], response, now));
  EXPECT_EQ(classified.usageLimitReached, true); EXPECT_EQ(classified.retryAtMs, resetAtMs); EXPECT_EQ(classified.quotaBlockSource, "body_resets_at");
  EXPECT_EQ(classified.resetDeadlineIsStable, true); EXPECT_EQ(classified.resetDeadlineConflict, false);
  const auto persisted = slot(kv);
  EXPECT_EQ(persisted.quota_blocked_until_ms, resetAtMs); EXPECT_EQ(persisted.quota_block_source, "body_resets_at"); EXPECT_EQ(persisted.observed_reset_at_ms, resetAtMs);
  EXPECT_EQ(persisted.observed_reset_at_is_stable, true); EXPECT_EQ(persisted.banked_reset_generation_ambiguous, false);
  resetCodexAccountRoutingForTest(); SELECT(selected, singlePool, now + 1); ASSERT_EQ(kind(selected), "quota_blocked");
  const auto cohort = blocked(selected); ASSERT_EQ(cohort.size(), 1u);
  EXPECT_EQ(cohort[0].quotaResetAtMs, resetAtMs); EXPECT_EQ(cohort[0].routingGeneration, persisted.generation);
}

ASYNC_TEST("an ordinary ambiguous deadline does not use the bounded recovery-probe lease") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000, firstResetAtMs = now + 60'000, conflictingResetAtMs = now + 120'000;
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible");
  RUN(markCodexQuotaBlocked(accounts(initial)[0], quotaBody(firstResetAtMs, rt::Date(conflictingResetAtMs).toUTCString()), now));
  const auto conflicted = slot(kv); EXPECT_EQ(conflicted.banked_reset_generation_ambiguous, true); EXPECT_EQ(conflicted.banked_reset_recovery_probe_pending, false);
  SELECT(halfOpen, singlePool, conflictingResetAtMs + 1); ASSERT_EQ(kind(halfOpen), "eligible");
  AWAIT(claimed, claimCodexRoutingProbe(singlePool, accounts(halfOpen)[0], conflictingResetAtMs + 1)); ASSERT(claimed);
  const double longResetAtMs = conflictingResetAtMs + 120'000;
  RUN(markCodexQuotaBlocked(*claimed, httpDateQuotaResponse(longResetAtMs), conflictingResetAtMs + 1));
  EXPECT_EQ(slot(kv).quota_blocked_until_ms, longResetAtMs); EXPECT_EQ(slot(kv).banked_reset_recovery_probe_pending, false);
}

ASYNC_TEST("a failed verified-reset probe uses a bounded retry instead of the old quota deadline") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000, resetAtMs = now + 60'000;
  SELECT(initial, singlePool, now, "gpt-5.3-codex-spark"); ASSERT_EQ(kind(initial), "eligible");
  RUN(markCodexQuotaBlocked(accounts(initial)[0], httpDateQuotaResponse(resetAtMs), now));
  const auto beforeReset = state(kv); ASSERT(beforeReset); const double routingGeneration = beforeReset->slots[0].generation;
  AWAIT(recovery, reconcileCodexQuotaAfterVerifiedReset(accounts(initial)[0], CodexQuotaResetReconciliationInput{resetAtMs, routingGeneration})); ASSERT(recovery);
  EXPECT_EQ(recovery->probeCircuit, "quota"); ASSERT(slot(kv).probe_lease); EXPECT_EQ(slot(kv).probe_lease->quota_class, "spark");
  AWAIT(failedProbe, markCodexRecoveryProbeQuotaBlocked(*recovery, httpDateQuotaResponse(resetAtMs), now)); EXPECT_EQ(failedProbe.retryAtMs, resetAtMs);
  const auto afterProbe = slot(kv); EXPECT_EQ(afterProbe.quota_blocked_until_ms, now + CODEX_HALF_OPEN_LEASE_MS);
  EXPECT_EQ(afterProbe.banked_reset_generation_ambiguous, true); EXPECT_EQ(afterProbe.banked_reset_recovery_probe_pending, true);
}

ASYNC_TEST("a non-quota verified-reset probe clears recovery-pending evidence") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000, resetAtMs = now + 60'000;
  SELECT(initial, singlePool, now, "gpt-5.3-codex-spark"); ASSERT_EQ(kind(initial), "eligible");
  RUN(markCodexQuotaBlocked(accounts(initial)[0], httpDateQuotaResponse(resetAtMs), now));
  const auto beforeReset = state(kv); ASSERT(beforeReset); const double routingGeneration = beforeReset->slots[0].generation;
  AWAIT(recovery, reconcileCodexQuotaAfterVerifiedReset(accounts(initial)[0], CodexQuotaResetReconciliationInput{resetAtMs, routingGeneration})); ASSERT(recovery);
  AWAIT(released, markCodexRecoveryProbeQuotaBlocked(*recovery, json429(json::object{{"error", json::object{{"type", "rate_limit_error"}}}}), now));
  EXPECT_EQ(released.usageLimitReached, false); const auto afterProbe = slot(kv);
  EXPECT(!afterProbe.quota_blocked_until_ms); EXPECT_EQ(afterProbe.banked_reset_recovery_probe_pending, false); EXPECT(!afterProbe.probe_lease);
}

ASYNC_TEST("a stale verified reset opens a fenced probe without spending another reset") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000, resetAtMs = now + 60'000;
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible");
  RUN(markCodexQuotaBlocked(accounts(initial)[0], httpDateQuotaResponse(resetAtMs), now));
  const auto beforeStaleRecovery = state(kv); ASSERT(beforeStaleRecovery); const double staleGeneration = beforeStaleRecovery->slots[0].generation + 1;
  auto changed = routing::toJson(*beforeStaleRecovery); auto& changedSlot = changed.at("slots").as_array()[0].as_object();
  changedSlot["generation"] = staleGeneration; changedSlot["banked_reset_generation_ambiguous"] = true;
  RUN(kv->set(CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvValue(changed))); resetCodexAccountRoutingForTest();
  AWAIT(recovery, reconcileCodexQuotaAfterStaleVerifiedReset(accounts(initial)[0], CodexQuotaResetReconciliationInput{resetAtMs, staleGeneration})); ASSERT(recovery);
  EXPECT_EQ(recovery->probeGeneration, staleGeneration + 1); const auto afterRecovery = slot(kv);
  EXPECT(!afterRecovery.quota_blocked_until_ms); EXPECT_EQ(afterRecovery.banked_reset_generation_ambiguous, true); EXPECT(afterRecovery.probe_lease);
}

ASYNC_TEST("legacy neutral v2 state permits its first canonical body resets_at fence") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000;
  const double resetAtSeconds = rt::Math::floor(now / 1'000) + 120, resetAtMs = resetAtSeconds * 1'000;
  AWAIT(credentialVersion, codexCredentialVersion(singlePool.accounts[0]));
  const json::value legacy = json::object{{"v", 2}, {"updated_at_ms", now}, {"slots", json::array{json::object{
    {"credential_version", credentialVersion}, {"quota_blocked_until_ms", nullptr}, {"quota_block_source", nullptr},
    {"invalid_credential_version", nullptr}, {"primary_used_percent", nullptr}, {"secondary_used_percent", nullptr},
    {"observed_reset_at_ms", nullptr}, {"generation", 0}, {"probe_lease", nullptr}}}}};
  RUN(kv->set(CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvValue(legacy)));
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible");
  RUN(markCodexQuotaBlocked(accounts(initial)[0], quotaBody(resetAtMs), now));
  const auto persisted = slot(kv); EXPECT_EQ(persisted.generation, 1.0); EXPECT_EQ(persisted.quota_block_source, "body_resets_at");
  EXPECT_EQ(persisted.observed_reset_at_ms, resetAtMs); EXPECT_EQ(persisted.observed_reset_at_is_stable, true); EXPECT_EQ(persisted.banked_reset_generation_ambiguous, false);
  AWAIT(fence, getCodexQuotaBlockFence(accounts(initial)[0], resetAtMs)); EXPECT_EQ(fence, 1.0);
  resetCodexAccountRoutingForTest(); SELECT(selected, singlePool, now + 1); ASSERT_EQ(kind(selected), "quota_blocked");
  const auto cohort = blocked(selected); ASSERT_EQ(cohort.size(), 1u); EXPECT_EQ(cohort[0].quotaResetAtMs, resetAtMs); EXPECT_EQ(cohort[0].routingGeneration, 1.0);
}

ASYNC_TEST("only the exact contaminated first body fence repairs legacy ambiguity") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000, resetAtMs = now + 120'000;
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible"); const auto account = accounts(initial)[0];
  const json::object contaminatedSlot{{"account_id_hash", account.accountIdHash}, {"credential_version", account.credentialVersion},
    {"quota_blocked_until_ms", resetAtMs}, {"quota_block_source", "body_resets_at"}, {"invalid_credential_version", nullptr},
    {"primary_used_percent", 100}, {"secondary_used_percent", 0}, {"observed_reset_at_ms", resetAtMs},
    {"observed_reset_at_is_stable", true}, {"banked_reset_generation_ambiguous", true}, {"generation", 1}, {"probe_lease", nullptr}};
  auto repaired = parseCodexAccountRoutingState(stateWith(contaminatedSlot, now)); ASSERT(repaired); EXPECT_EQ(repaired->slots[0].banked_reset_generation_ambiguous, false);
  RUN(kv->set(CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvValue(stateWith(contaminatedSlot, now))));
  AWAIT(fence, getCodexQuotaBlockFence(account, resetAtMs)); EXPECT_EQ(fence, 1.0);
  auto missingInvalidCredential = contaminatedSlot; rt::objectDelete(missingInvalidCredential, "invalid_credential_version");
  std::vector<std::pair<std::string, json::object>> nearMisses;
  const auto add = [&](const std::string& name, const std::string& member, const json::value& value) {
    auto changed = contaminatedSlot; changed[member] = value; nearMisses.emplace_back(name, std::move(changed));
  };
  add("later generation", "generation", 2); add("header source", "quota_block_source", "header_retry_after");
  add("deadline mismatch", "quota_blocked_until_ms", rt::makeNumber(resetAtMs + 1)); add("unstable observation", "observed_reset_at_is_stable", false);
  add("active probe", "probe_lease", json::object{{"token", "probe"}, {"expires_at_ms", now + 30'000}, {"generation", 1}});
  add("invalid credential", "invalid_credential_version", json::value(account.credentialVersion));
  nearMisses.emplace_back("missing invalid credential field", missingInvalidCredential);
  add("malformed invalid credential field", "invalid_credential_version", 1); add("missing account identity", "account_id_hash", nullptr);
  for (const auto& testCase : nearMisses) {
    const auto parsed = parseCodexAccountRoutingState(stateWith(testCase.second, now)); ASSERT(parsed); EXPECT_EQ(parsed->slots[0].banked_reset_generation_ambiguous, true);
  }
  const auto globallyUnresolved = parseCodexAccountRoutingState(stateWith(contaminatedSlot, now, true)); ASSERT(globallyUnresolved);
  EXPECT_EQ(globallyUnresolved->slots[0].banked_reset_generation_ambiguous, true);
}

ASYNC_TEST("routing harness matches the isolated Bun fixture transcript") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000;
  rt::globalDateNow() = [now] { return now; };
  const rt::KvKey x{"fixture", "x"}, y{"fixture", "y"};
  AWAIT(set1, kv->set(x, rt::KvValue(1))); AWAIT(set2, kv->set(x, rt::KvValue(2))); AWAIT(setY, kv->set(y, rt::KvValue(3)));
  const std::vector<rt::KvKey> keys{x, y}; auto capturedSnapshot = kv->getMany(keys);
  RUN(kv->set(x, rt::KvValue(4))); AWAIT(snapshot, std::move(capturedSnapshot));
  auto staleOperation = kv->atomic(); staleOperation->check(snapshot[0]).set(x, rt::KvValue(99)); AWAIT(stale, staleOperation->commit());
  auto emptyOperation = kv->atomic(); AWAIT(empty, emptyOperation->commit());
  auto multipleOperation = kv->atomic(); multipleOperation->set(x, rt::KvValue(5)).set(x, rt::KvValue(6)); AWAIT(multiple, multipleOperation->commit());
  AWAIT(current, kv->get(x)); RUN(seedSubscriptionSelection(kv, std::vector<std::string>{"two", "one", "two"}));
  AWAIT(selection, kv->get(PROVIDER_SELECTION_KV_KEY));
  auto http = httpDateQuotaResponse(now + 60'000), delta = deltaSecondsQuotaResponse("60");
  AWAIT(httpBody, http.text()); AWAIT(deltaBody, delta.text());
  std::string requiredError;
  try { (void)required(std::optional<int>{}, "fixture"); } catch (const rt::JsError& error) { requiredError = error.what(); }
  const auto commitJson = [](const rt::KvCommitResult& result) {
    json::object out{{"ok", result.ok}}; if (result.ok) out["versionstamp"] = result.versionstamp; return out;
  };
  const auto entryJson = [](const rt::KvEntryMaybe<>& entry) {
    return json::object{{"key", rt::jsonParse(key(entry.key))}, {"value", entry.value.toJson()},
      {"versionstamp", entry.versionstamp ? json::value(*entry.versionstamp) : json::value(nullptr)}};
  };
  json::array snapshotJson; for (const auto& entry : snapshot) snapshotJson.push_back(entryJson(entry));
  const json::value actual = json::object{{"set1", commitJson(set1)}, {"set2", commitJson(set2)}, {"setY", commitJson(setY)},
    {"snapshot", snapshotJson}, {"stale", commitJson(stale)}, {"empty", commitJson(empty)}, {"multiple", commitJson(multiple)},
    {"current", entryJson(current)}, {"selection", entryJson(selection)},
    {"http", json::object{{"status", http.status()}, {"retry", *http.headers().get("Retry-After")}, {"body", httpBody}}},
    {"delta", json::object{{"status", delta.status()}, {"retry", *delta.headers().get("Retry-After")}, {"body", deltaBody}}},
    {"legacy", syntheticLegacyBlock(now + 60'000, now)},
    {"urls", json::array{requestUrl(std::string("https://fixture.test/a")), requestUrl(rt::URL("https://fixture.test/b")), requestUrl(rt::Request("https://fixture.test/c"))}},
    {"requiredError", requiredError}};
  const auto expected = rt::jsonParse(R"C1ORACLE({"set1":{"ok":true,"versionstamp":"00000000000000000001"},"set2":{"ok":true,"versionstamp":"00000000000000000002"},"setY":{"ok":true,"versionstamp":"00000000000000000001"},"snapshot":[{"key":["fixture","x"],"value":2,"versionstamp":"00000000000000000002"},{"key":["fixture","y"],"value":3,"versionstamp":"00000000000000000001"}],"stale":{"ok":false},"empty":{"ok":true,"versionstamp":"00000000000000000000"},"multiple":{"ok":true,"versionstamp":"00000000000000000005"},"current":{"key":["fixture","x"],"value":6,"versionstamp":"00000000000000000005"},"selection":{"key":["uos_ai","provider_selection","v1"],"value":{"provider_ids":["codex:3fc4ccfe745870e2c0d99f71f30ff0656c8dedd41cc1d7d3d376b0dbe685e2f3","codex:7692c3ad3540bb803c020b3aee66cd8887123234ea0c6e7143c0add73ff431ed","codex:3fc4ccfe745870e2c0d99f71f30ff0656c8dedd41cc1d7d3d376b0dbe685e2f3"],"updated_at_ms":1700000000000},"versionstamp":"00000000000000000001"},"http":{"status":429,"retry":"Tue, 14 Nov 2023 22:14:20 GMT","body":"{\"error\":{\"type\":\"usage_limit_reached\"}}"},"delta":{"status":429,"retry":"60","body":"{\"error\":{\"type\":\"usage_limit_reached\"}}"},"legacy":{"blocked_until_ms":1700000060000,"source":"header_retry_after","quota_signal_observed_at_ms":1700000000000,"observed_reset_at_ms":null,"observed_reset_at_is_stable":false,"banked_reset_generation_ambiguous":false,"banked_reset_recovery_probe_pending":false},"urls":["https://fixture.test/a","https://fixture.test/b","https://fixture.test/c"],"requiredError":"expected fixture to be present"})C1ORACLE");
  EXPECT(rt::deepEqual(actual, expected));
}
