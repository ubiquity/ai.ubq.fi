// Links: tests/codex_banked_reset_part1_test.cpp tests/helpers/codex_banked_reset_harness.cpp src/codex/reset_settings.cpp
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
// Port of tests/codex-banked-reset-part1.test.ts.
// Unexpressible: "a malformed terminal reconciliation capability fails closed without provider calls": json::value cannot carry a throwing property getter.

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
  struct Fixture {
    std::shared_ptr<MemoryKv> kv = std::make_shared<MemoryKv>();
    std::shared_ptr<FakeCodexUsageResetProvider> provider = std::make_shared<FakeCodexUsageResetProvider>();
    std::shared_ptr<TestClock> clock = std::make_shared<TestClock>();
    auto deps() const { return dependencies(kv, provider, clock); }
  };
  std::optional<std::string_view> stateOf(const uos::codex::banked_reset::CodexBankedResetOutcome& result) {
    return result.record ? std::optional<std::string_view>(uos::types::toString(result.record->state)) : std::nullopt;
  }
  json::object terminalContract() {
    auto value = provenContract();
    value["idempotency"] = json::object{{"callerSupplied", true}, {"retentionMs", nullptr}};
    value["lookup"] = json::object{{"byIdempotencyKey", false}, {"byProviderReceiptId", false}};
    value["verification"] = json::object{{"independentlyVerifiable", false}};
    value["redeemOutcomeIsFinal"] = true; value["receiptIdsSafeToPersistAndLog"] = false;
    return value;
  }
  rt::Task<rt::KvEntryMaybe<>> dailyRow(std::shared_ptr<MemoryKv> kv, std::string accountId, double nowMs) {
    AWAIT(hash, testHash(accountId));
    const auto key = codexResetAccountDailyKey(hash, rt::Date(nowMs).toISOString().substr(0, 10));
    AWAIT(row, kv->get(key)); co_return row;
  }
}
ASYNC_TEST("banked reset disabled, shadow, and invalid limits make zero provider calls") {
  Isolation isolation;

  auto clock = std::make_shared<TestClock>();
  struct Case { h::ConfigOverrides overrides; std::string reason; bool shadow; };
  const std::vector<Case> cases{{{.enabled=false}, "feature_disabled", false}, {{.mode="shadow", .maxPerAccountPerDay=0}, "shadow", true}, {{.mode="shadow", .maxPerAccountPerDay=0}, "shadow", true}, {{.maxPerAccountPerDay=0}, "per_account_day_limit_invalid", false}, {{.maxPerAccountPerWindow=2}, "per_account_window_limit_invalid", false}};
  for (const auto& entry : cases) {
    auto kv = std::make_shared<MemoryKv>(); auto provider = std::make_shared<FakeCodexUsageResetProvider>();
    std::vector<std::string> events;
    uos::codex::banked_reset::CodexBankedResetTelemetry telemetry;
    telemetry.event = [&](std::string event, json::object) { events.push_back(event); };
    AWAIT(result, attemptCodexBankedReset(candidate(), dependencies(kv, provider, clock, h::config(entry.overrides), telemetry)));
    EXPECT_EQ(result.kind, "skipped"); EXPECT_EQ(result.reason, entry.reason); EXPECT_EQ(provider->callCount(), 0); EXPECT_EQ(provider->commitCount, 0);
    EXPECT_EQ(std::find(events.begin(), events.end(), "codex_reset_shadow_candidate") != events.end(), entry.shadow);
  }
}

ASYNC_TEST("banked reset accepts any account ID or stable account hash") {
  Isolation isolation;

  Fixture f; const auto reset = candidate(); AWAIT(accountIdHash, testHash(reset.accountId));
  DONE(seedFences(f.kv, reset)); AWAIT(allowed, attemptCodexBankedReset(reset, dependencies(f.kv, f.provider, f.clock, h::config())));
  EXPECT_EQ(allowed.kind, "verified"); EXPECT_EQ(allowed.accountIdHash, accountIdHash); EXPECT_EQ(f.provider->commitCount, 1);
}

ASYNC_TEST("banked reset live happy path commits exactly once with a stable durable identity") {
  Isolation isolation;

  Fixture f; const auto deps = f.deps(); const auto reset = candidate(); DONE(seedFences(f.kv, reset));
  AWAIT(first, attemptCodexBankedReset(reset, deps));
  EXPECT_EQ(first.kind, "verified"); EXPECT_EQ(first.reason, "verified"); EXPECT_EQ(f.provider->commitCount, 1);
  EXPECT_EQ(f.provider->inventoryInputs.size(), 1u); EXPECT_EQ(f.provider->redeemInputs.size(), 1u); EXPECT_EQ(f.provider->verificationInputs.size(), 1u);
  ASSERT(first.accountIdHash); ASSERT(first.quotaGeneration); ASSERT(first.idempotencyKeyHash); ASSERT(first.record);
  EXPECT_EQ(stateOf(first), "verified"); EXPECT_EQ(first.record->idempotency_key_hash, first.idempotencyKeyHash);
  EXPECT_EQ(f.provider->redeemInputs[0].accountId, "test-account-a"); EXPECT(rt::Regex("^uos_ai_codex_reset_v1_").test(f.provider->redeemInputs[0].idempotencyKey));
  AWAIT(durable, f.kv->get(codexResetRedemptionKey(*first.accountIdHash, *first.quotaGeneration)));
  const auto record = parseCodexResetRedemptionRecord(durable.value.toJson()); ASSERT(record);
  EXPECT_EQ(uos::types::toString(record->state), "verified"); EXPECT_EQ(record->provider_receipt_id, "receipt-completed"); EXPECT_EQ(record->idempotency_key_hash, first.idempotencyKeyHash);
  AWAIT(daily, dailyRow(f.kv, reset.accountId, f.clock->nowMs)); EXPECT_EQ(daily.value["submission_count"].asNumber(), 1);
  AWAIT(duplicate, attemptCodexBankedReset(candidate({.requestId="same-window-later-request"}), deps));
  EXPECT_EQ(duplicate.kind, "verified"); EXPECT_EQ(duplicate.reason, "previously_verified"); EXPECT_EQ(duplicate.idempotencyKeyHash, first.idempotencyKeyHash);
  EXPECT_EQ(f.provider->redeemInputs.size(), 1u); EXPECT_EQ(f.provider->commitCount, 1);
}

ASYNC_TEST("a verified reset from an older routing generation is not reusable") {
  Isolation isolation;

  Fixture f; const auto firstCandidate=candidate(); DONE(seedFences(f.kv, firstCandidate));
  AWAIT(first, attemptCodexBankedReset(firstCandidate, f.deps())); EXPECT_EQ(first.kind, "verified");
  const auto laterCandidate=candidate({.routingGeneration=firstCandidate.routingGeneration+1});
  const json::object row{{"kind","routing"},{"routing_generation",rt::makeNumber(laterCandidate.routingGeneration)}};
  DONE(f.kv->set(routingFenceKey(laterCandidate.accountId), rt::KvValue(row)));
  AWAIT(later, attemptCodexBankedReset(laterCandidate, f.deps()));
  EXPECT_EQ(later.kind,"skipped"); EXPECT_EQ(later.reason,"verified_routing_generation_stale"); ASSERT(later.record);
  EXPECT_EQ(later.record->routing_generation, firstCandidate.routingGeneration); EXPECT_EQ(f.provider->redeemInputs.size(),1u); EXPECT_EQ(f.provider->commitCount,1);
}

ASYNC_TEST("banked reset production owner token generator is called with its Crypto receiver") {
  Isolation isolation;

  Fixture f; const auto reset=candidate(); auto deps=f.deps(); deps.newOwnerToken={}; DONE(seedFences(f.kv,reset));
  AWAIT(result,attemptCodexBankedReset(reset,deps)); EXPECT_EQ(result.kind,"verified"); ASSERT(result.record);
  EXPECT(rt::Regex("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$").test(result.record->owner_token)); EXPECT_EQ(f.provider->commitCount,1);
}

ASYNC_TEST("unknown provider outcome is recovered through lookup with the same key and no second redemption") {
  Isolation isolation;

  Fixture f; f.provider->redeemResult=json::object{{"kind","unknown"},{"providerReceiptId",nullptr}}; f.provider->commitOnRedeem=true;
  const auto deps=f.deps(); const auto reset=candidate(); DONE(seedFences(f.kv,reset));
  AWAIT(first,attemptCodexBankedReset(reset,deps)); EXPECT_EQ(first.kind,"pending"); EXPECT_EQ(first.reason,"provider_commit_unknown"); EXPECT_EQ(stateOf(first),"unknown");
  ASSERT_EQ(f.provider->redeemInputs.size(),1u); EXPECT_EQ(f.provider->commitCount,1); const auto stableKey=f.provider->redeemInputs[0].idempotencyKey; EXPECT(!stableKey.empty());
  f.provider->lookupResult=json::object{{"kind","already_redeemed"},{"providerReceiptId","receipt-recovered"}}; f.clock->advance(CODEX_BANKED_RESET_LEASE_MS+1);
  AWAIT(recovered,attemptCodexBankedReset(candidate({.requestId="recovery-request"}),deps));
  EXPECT_EQ(recovered.kind,"verified"); EXPECT_EQ(recovered.reason,"verified"); EXPECT_EQ(stateOf(recovered),"verified"); ASSERT(recovered.record); EXPECT_EQ(recovered.record->fence,3);
  EXPECT_EQ(f.provider->redeemInputs.size(),1u); EXPECT_EQ(f.provider->commitCount,1); ASSERT_EQ(f.provider->lookupInputs.size(),1u);
  EXPECT_EQ(f.provider->lookupInputs[0].idempotencyKey,stableKey); EXPECT(!f.provider->lookupInputs[0].providerReceiptId); EXPECT_EQ(recovered.idempotencyKeyHash,first.idempotencyKeyHash);
}

ASYNC_TEST("concurrent claims for one generation permit one provider submission and prevent the duplicate") {
  Isolation isolation;

  Fixture f; auto gate=std::make_shared<Deferred<>>(); auto entered=std::make_shared<Deferred<>>(); f.provider->redeemGate=gate->promise; f.provider->redeemEntered=entered;
  const auto deps=f.deps(); const auto reset=candidate(); DONE(seedFences(f.kv,reset));
  rt::Promise<uos::codex::banked_reset::CodexBankedResetOutcome> firstPromise(attemptCodexBankedReset(reset,deps)); co_await entered->promise.wait();
  AWAIT(duplicate,attemptCodexBankedReset(candidate({.requestId="concurrent-request"}),deps)); EXPECT_EQ(duplicate.kind,"pending"); EXPECT_EQ(duplicate.reason,"transaction_in_progress");
  EXPECT_EQ(f.provider->inventoryInputs.size(),1u); EXPECT_EQ(f.provider->redeemInputs.size(),1u); EXPECT_EQ(f.provider->commitCount,0);
  gate->resolve(); const auto first=co_await firstPromise.wait(); EXPECT_EQ(first.kind,"verified"); EXPECT_EQ(f.provider->commitCount,1); EXPECT_EQ(f.provider->redeemInputs.size(),1u);
}

ASYNC_TEST("an expired recovery lookup rejection stays unknown while the original redemption may be in flight") {
  Isolation isolation;

  Fixture f; const auto reset=candidate(); DONE(seedFences(f.kv,reset)); auto gate=std::make_shared<Deferred<>>(); auto entered=std::make_shared<Deferred<>>();
  f.provider->redeemGate=gate->promise; f.provider->redeemEntered=entered; const auto deps=f.deps();
  rt::Promise<uos::codex::banked_reset::CodexBankedResetOutcome> original(attemptCodexBankedReset(reset,deps)); co_await entered->promise.wait();
  f.clock->advance(CODEX_BANKED_RESET_LEASE_MS+1); f.provider->lookupResult=json::object{{"kind","rejected"},{"reason","not yet visible"}}; f.provider->verifyResult=false;
  AWAIT(recovery,reconcileCodexBankedReset(candidate({.requestId="in-flight-negative-lookup"}),deps)); EXPECT_EQ(recovery.kind,"pending"); EXPECT_EQ(recovery.reason,"verification_not_applied");
  EXPECT_EQ(stateOf(recovery),"unknown"); EXPECT_EQ(f.provider->redeemInputs.size(),1u); EXPECT_EQ(f.provider->lookupInputs.size(),1u);
  gate->resolve(); const auto originalOutcome=co_await original.wait(); EXPECT_EQ(originalOutcome.kind,"pending"); EXPECT_EQ(originalOutcome.reason,"receipt_cas_failed"); EXPECT_EQ(f.provider->redeemInputs.size(),1u);
  const auto stored=f.kv->redemptionRecord(codexResetRedemptionKey(requiredHash(recovery.accountIdHash),requiredHash(recovery.quotaGeneration))); ASSERT(stored); EXPECT_EQ(rt::getString(*stored,"state"),"unknown");
}

ASYNC_TEST("live claims require seeded current fences and CAS-check both routing and credential records") {
  Isolation isolation;

  Fixture f; const auto deps=f.deps(); const auto missingCandidate=candidate({.fences=std::vector<uos::codex::banked_reset::CodexBankedResetFence>{}});
  AWAIT(missing,attemptCodexBankedReset(missingCandidate,deps)); EXPECT_EQ(missing.kind,"skipped"); EXPECT_EQ(missing.reason,"routing_fence_missing"); EXPECT_EQ(f.provider->callCount(),0);
  const auto reset=candidate(); AWAIT(stale,attemptCodexBankedReset(reset,deps)); EXPECT_EQ(stale.kind,"skipped"); EXPECT_EQ(stale.reason,"routing_fence_stale"); EXPECT_EQ(f.provider->callCount(),0);
  DONE(seedFences(f.kv,reset)); AWAIT(verified,attemptCodexBankedReset(reset,deps)); EXPECT_EQ(verified.kind,"verified"); EXPECT_EQ(f.provider->commitCount,1);
  std::size_t count=0; for(const auto& batch:f.kv->atomicCheckBatches) if(hasKey(batch,routingFenceKey(reset.accountId))&&hasKey(batch,credentialFenceKey(reset.accountId))) ++count; EXPECT_EQ(count,3u);
}

ASYNC_TEST("a reloadable kill switch changed during inventory leaves the claim durable and makes no redemption") {
  Isolation isolation;

  Fixture f; const auto reset=candidate(); DONE(seedFences(f.kv,reset)); auto gate=std::make_shared<Deferred<>>(); auto entered=std::make_shared<Deferred<>>();
  f.provider->inventoryGate=gate->promise; f.provider->inventoryEntered=entered; auto currentConfig=h::config();
  const auto deps=dependencies(f.kv,f.provider,f.clock,currentConfig,{},[&]{return currentConfig;});
  rt::Promise<uos::codex::banked_reset::CodexBankedResetOutcome> pending(attemptCodexBankedReset(reset,deps)); co_await entered->promise.wait();
  currentConfig=h::config({.enabled=false}); gate->resolve(); const auto result=co_await pending.wait();
  EXPECT_EQ(result.kind,"pending"); EXPECT_EQ(result.reason,"new_submission_feature_disabled"); EXPECT_EQ(stateOf(result),"claimed"); EXPECT_EQ(f.provider->inventoryInputs.size(),1u); EXPECT_EQ(f.provider->redeemInputs.size(),0u); EXPECT_EQ(f.provider->commitCount,0);
}

ASYNC_TEST("inventory that returns after the quota deadline cannot cross the submission boundary") {
  Isolation isolation;

  Fixture f; const auto reset=candidate({.quotaResetAtMs=f.clock->nowMs+1}); DONE(seedFences(f.kv,reset)); auto gate=std::make_shared<Deferred<>>(); auto entered=std::make_shared<Deferred<>>();
  f.provider->inventoryGate=gate->promise; f.provider->inventoryEntered=entered;
  rt::Promise<uos::codex::banked_reset::CodexBankedResetOutcome> pending(attemptCodexBankedReset(reset,f.deps())); co_await entered->promise.wait(); f.clock->advance(1); f.provider->inventory.observedAtMs=f.clock->nowMs; gate->resolve();
  const auto result=co_await pending.wait(); EXPECT_EQ(result.kind,"rejected"); EXPECT_EQ(result.reason,"quota_window_expired"); EXPECT_EQ(stateOf(result),"rejected"); EXPECT_EQ(f.provider->inventoryInputs.size(),1u); EXPECT_EQ(f.provider->redeemInputs.size(),0u); EXPECT_EQ(f.provider->commitCount,0);
  std::size_t count=0; for(const auto& batch:f.kv->atomicCheckBatches) if(hasKey(batch,routingFenceKey(reset.accountId))&&hasKey(batch,credentialFenceKey(reset.accountId))) ++count; EXPECT_EQ(count,1u);
}

ASYNC_TEST("an already-expired quota window is never claimed or charged against the daily cap") {
  Isolation isolation;

  Fixture f; const auto reset=candidate({.quotaResetAtMs=f.clock->nowMs}); DONE(seedFences(f.kv,reset)); AWAIT(result,attemptCodexBankedReset(reset,f.deps()));
  EXPECT_EQ(result.kind,"skipped"); EXPECT_EQ(result.reason,"quota_window_expired"); EXPECT_EQ(f.provider->callCount(),0); EXPECT_EQ(f.provider->commitCount,0); EXPECT_EQ(f.kv->atomicCommitCount,0);
  AWAIT(daily,dailyRow(f.kv,reset.accountId,f.clock->nowMs)); EXPECT(daily.value.isNull());
}

ASYNC_TEST("a quota deadline crossing during submission preparation cannot reserve daily capacity") {
  Isolation isolation;

  Fixture f; const auto reset=candidate({.quotaResetAtMs=f.clock->nowMs+1}); DONE(seedFences(f.kv,reset)); AWAIT(hash,testHash(reset.accountId));
  const auto dailyKey=codexResetAccountDailyKey(hash,rt::Date(f.clock->nowMs).toISOString().substr(0,10));
  f.kv->beforeGet=[&](rt::KvKey key,double)->std::optional<rt::Promise<void>> {if(encodeKey(key)==encodeKey(dailyKey)){f.kv->beforeGet={}; f.clock->advance(1);}return std::nullopt;};
  AWAIT(result,attemptCodexBankedReset(reset,f.deps())); EXPECT_EQ(result.kind,"pending"); EXPECT_EQ(result.reason,"quota_window_expired"); EXPECT_EQ(stateOf(result),"claimed");
  EXPECT_EQ(f.provider->inventoryInputs.size(),1u); EXPECT_EQ(f.provider->redeemInputs.size(),0u); EXPECT_EQ(f.provider->commitCount,0); EXPECT_EQ(f.kv->atomicCommitCount,1);
  AWAIT(row,f.kv->get(dailyKey)); EXPECT(row.value.isNull());
}

ASYNC_TEST("a quota deadline crossing during the final renewal cannot reach redemption") {
  Isolation isolation;

  Fixture f; const auto reset=candidate({.quotaResetAtMs=f.clock->nowMs+1}); DONE(seedFences(f.kv,reset));
  f.kv->beforeAtomicCommit=[&](double commitNumber){if(commitNumber!=3)return; f.kv->beforeAtomicCommit={};f.clock->advance(1);};
  AWAIT(result,attemptCodexBankedReset(reset,f.deps())); EXPECT_EQ(result.kind,"pending"); EXPECT_EQ(result.reason,"quota_window_expired"); EXPECT_EQ(stateOf(result),"submitted"); EXPECT_EQ(f.provider->inventoryInputs.size(),1u); EXPECT_EQ(f.provider->redeemInputs.size(),0u); EXPECT_EQ(f.provider->commitCount,0); EXPECT_EQ(f.kv->atomicCommitCount,3);
}

ASYNC_TEST("a kill switch landing after the durable boundary still blocks provider redemption") {
  Isolation isolation;

  Fixture f; const auto reset=candidate(); DONE(seedFences(f.kv,reset)); auto currentConfig=h::config();
  const auto deps=dependencies(f.kv,f.provider,f.clock,currentConfig,{},[&]{return currentConfig;});
  f.kv->beforeAtomicCommit=[&](double commitNumber){if(commitNumber!=2)return; f.kv->beforeAtomicCommit={}; currentConfig=h::config({.enabled=false,.mode="disabled"});};
  AWAIT(result,attemptCodexBankedReset(reset,deps)); EXPECT_EQ(result.kind,"pending"); EXPECT_EQ(result.reason,"new_submission_feature_disabled"); EXPECT_EQ(stateOf(result),"submitted"); EXPECT_EQ(f.provider->inventoryInputs.size(),1u); EXPECT_EQ(f.provider->redeemInputs.size(),0u); EXPECT_EQ(f.provider->commitCount,0);
}

ASYNC_TEST("a kill switch landing during the final renewal still blocks provider redemption") {
  Isolation isolation;

  Fixture f; const auto reset=candidate(); DONE(seedFences(f.kv,reset)); auto currentConfig=h::config();
  const auto deps=dependencies(f.kv,f.provider,f.clock,currentConfig,{},[&]{return currentConfig;});
  f.kv->beforeAtomicCommit=[&](double commitNumber){if(commitNumber!=3)return; f.kv->beforeAtomicCommit={}; currentConfig=h::config({.enabled=false,.mode="disabled"});};
  AWAIT(result,attemptCodexBankedReset(reset,deps)); EXPECT_EQ(result.kind,"pending"); EXPECT_EQ(result.reason,"new_submission_feature_disabled"); EXPECT_EQ(stateOf(result),"submitted"); EXPECT_EQ(f.provider->inventoryInputs.size(),1u); EXPECT_EQ(f.provider->redeemInputs.size(),0u); EXPECT_EQ(f.provider->commitCount,0);
}

ASYNC_TEST("the persistent usage toggle blocks resets and re-enabling preserves normal redemption") {
  Isolation isolation;

  Fixture f; const auto reset=candidate(); DONE(seedFences(f.kv,reset)); AWAIT(hash,testHash(reset.accountId)); const auto key=codexResetUsageKey(hash);
  const std::vector<json::value> disabledValues{false,"true"};
  for(const auto& disabled:disabledValues){ const json::object row{{"id","key-a"},{"revoked_at_ms",nullptr},{"expires_at_ms",-1},{"enabled",disabled}}; DONE(f.kv->set(key,rt::KvValue(row))); AWAIT(result,attemptCodexBankedReset(reset,f.deps())); EXPECT_EQ(result.kind,"skipped"); EXPECT_EQ(f.provider->inventoryInputs.size(),0u); EXPECT_EQ(f.provider->redeemInputs.size(),0u); }
  const json::object row{{"id","key-a"},{"revoked_at_ms",nullptr},{"expires_at_ms",-1},{"enabled",true}}; DONE(f.kv->set(key,rt::KvValue(row)));
  AWAIT(result,attemptCodexBankedReset(reset,f.deps())); EXPECT_EQ(result.kind,"verified"); EXPECT_EQ(f.provider->redeemInputs.size(),1u);
}

ASYNC_TEST("a saved disable races with either submission transaction without spending a credit") {
  Isolation isolation;

  const std::vector<double> boundaries{2,3}; for(const auto boundary:boundaries){
    Fixture f; const auto reset=candidate(); DONE(seedFences(f.kv,reset)); AWAIT(hash,testHash(reset.accountId)); const auto key=codexResetUsageKey(hash);
    json::object row{{"id","key-a"},{"revoked_at_ms",nullptr},{"expires_at_ms",-1},{"enabled",true}}; DONE(f.kv->set(key,rt::KvValue(row)));
    f.kv->beforeAtomicCommit=[&](double commitNumber){if(commitNumber!=boundary)return; f.kv->beforeAtomicCommit={}; row["enabled"]=false; (void)f.kv->set(key,rt::KvValue(row));};
    AWAIT(result,attemptCodexBankedReset(reset,f.deps())); EXPECT_EQ(result.reason,"usage_disabled"); EXPECT_EQ(f.provider->redeemInputs.size(),0u); EXPECT_EQ(f.provider->commitCount,0);
  }
}

ASYNC_TEST("a disabled subscription does not block another subscription") {
  Isolation isolation;

  Fixture f; auto otherProvider=std::make_shared<FakeCodexUsageResetProvider>(); const auto reset=candidate(); const auto other=candidate({.accountId="other-account"});
  DONE(seedFences(f.kv,reset)); DONE(seedFences(f.kv,other)); AWAIT(hash,testHash(reset.accountId)); const json::object disabled{{"enabled",false}}; DONE(f.kv->set(codexResetUsageKey(hash),rt::KvValue(disabled)));
  const auto pool=fullPool(reset,f.provider,other,otherProvider); const auto deps=dependencies(f.kv,f.provider,f.clock,h::config({.mode="shadow",.maxPerAccountPerDay=1}));
  AWAIT(allowed,evaluateCodexBankedResetPool(pool,deps)); EXPECT_EQ(allowed.reason,"shadow_selected"); EXPECT_EQ(f.provider->inventoryInputs.size(),0u); EXPECT_EQ(otherProvider->inventoryInputs.size(),1u); EXPECT_EQ(otherProvider->redeemInputs.size(),0u);
  AWAIT(otherHash,testHash(other.accountId)); DONE(f.kv->set(codexResetUsageKey(otherHash),rt::KvValue(disabled))); AWAIT(blocked,evaluateCodexBankedResetPool(pool,deps)); EXPECT_EQ(blocked.reason,"usage_disabled");
}

ASYNC_TEST("disabling a subscription during inventory prevents a late decision or submission") {
  Isolation isolation;

  const std::vector<std::string> modes{"shadow","live"}; for(const auto& mode:modes){
    Fixture f; const auto reset=candidate(); DONE(seedFences(f.kv,reset)); AWAIT(hash,testHash(reset.accountId)); const auto settingsKey=codexResetUsageKey(hash);
    json::object key{{"id","key-a"},{"revoked_at_ms",nullptr},{"expires_at_ms",-1},{"enabled",true}}; DONE(f.kv->set(settingsKey,rt::KvValue(key)));
    auto gate=std::make_shared<Deferred<>>(); auto entered=std::make_shared<Deferred<>>(); f.provider->inventoryGate=gate->promise; f.provider->inventoryEntered=entered;
    const std::vector<uos::codex::banked_reset::CodexBankedResetPoolCandidate> pool{{.slot=0,.candidate=reset,.provider=f.provider}};
    const auto deps=dependencies(f.kv,f.provider,f.clock,h::config({.mode=mode,.maxPerAccountPerDay=1}));
    rt::Promise<uos::codex::banked_reset::CodexBankedResetPoolOutcome> pending(evaluateCodexBankedResetPool(pool,deps)); co_await entered->promise.wait(); key["enabled"]=false; DONE(f.kv->set(settingsKey,rt::KvValue(key))); gate->resolve();
    const auto result=co_await pending.wait(); EXPECT_EQ(result.kind,"skipped"); bool decision=false; for(const auto& entry:f.kv->entries.values())if(parseCodexResetShadowDecisionRecord(entry.value.toJson()))decision=true; EXPECT(!decision); EXPECT_EQ(f.provider->redeemInputs.size(),0u);
  }
}

ASYNC_TEST("an unapproved provider receipt stays out of the durable record and telemetry") {
  Isolation isolation;

  Fixture f; auto contract=provenContract(); contract["receiptIdsSafeToPersistAndLog"]=false; f.provider=std::make_shared<FakeCodexUsageResetProvider>(contract);
  const auto reset=candidate(); DONE(seedFences(f.kv,reset)); std::vector<json::object> submittedFields;
  uos::codex::banked_reset::CodexBankedResetTelemetry telemetry; telemetry.event=[&](std::string event,json::object fields){if(event=="codex_reset_submitted")submittedFields.push_back(fields);};
  AWAIT(result,attemptCodexBankedReset(reset,dependencies(f.kv,f.provider,f.clock,h::config(),telemetry))); EXPECT_EQ(result.kind,"verified"); ASSERT(result.record); EXPECT(!result.record->provider_receipt_id);
  const auto stored=f.kv->redemptionRecord(codexResetRedemptionKey(requiredHash(result.accountIdHash),requiredHash(result.quotaGeneration))); ASSERT(stored); const auto* receipt=rt::get(*stored,"provider_receipt_id"); ASSERT(receipt); EXPECT(receipt->is_null());
  ASSERT_EQ(submittedFields.size(),1u); EXPECT(submittedFields[0].at("provider_receipt_id").is_null());
}

ASYNC_TEST("documented terminal outcomes enable one-shot redemption and retain the exact safe result") {
  Isolation isolation;

  const std::vector<std::pair<std::string,std::string>> cases{{"completed","reset"},{"already_redeemed","already_redeemed"}};
  for(const auto& testCase:cases){
    Fixture f; f.provider=std::make_shared<FakeCodexUsageResetProvider>(terminalContract()); const auto reset=candidate({.requestId="terminal-"+testCase.second}); DONE(seedFences(f.kv,reset));
    f.provider->redeemResult=json::object{{"kind",testCase.first},{"providerReceiptId","must-not-persist"}}; f.provider->verifyResult=false; std::vector<json::object> verified;
    uos::codex::banked_reset::CodexBankedResetTelemetry telemetry; telemetry.event=[&](std::string event,json::object fields){if(event=="codex_reset_verified")verified.push_back(fields);};
    AWAIT(result,attemptCodexBankedReset(reset,dependencies(f.kv,f.provider,f.clock,h::config({.maxPerAccountPerDay=1}),telemetry)));
    EXPECT_EQ(result.kind,"verified"); EXPECT_EQ(result.reason,"redeem_outcome_"+testCase.second); EXPECT_EQ(stateOf(result),"verified"); ASSERT(result.record); EXPECT(!result.record->provider_receipt_id);
    ASSERT_EQ(verified.size(),1u); EXPECT_EQ(rt::getString(verified[0],"redeem_outcome"),testCase.second); EXPECT_EQ(f.provider->redeemInputs.size(),1u); EXPECT_EQ(f.provider->verificationInputs.size(),0u); EXPECT_EQ(f.provider->lookupInputs.size(),0u);
  }
}

ASYNC_TEST("a terminal-only provider requires an exact per-account daily cap of one") {
  Isolation isolation;

  const std::vector<double> limits{0,2}; for(const auto limit:limits){Fixture f; f.provider=std::make_shared<FakeCodexUsageResetProvider>(terminalContract()); const auto reset=candidate({.requestId="terminal-cap-"+rt::numberToString(limit)}); DONE(seedFences(f.kv,reset));
    AWAIT(result,attemptCodexBankedReset(reset,dependencies(f.kv,f.provider,f.clock,h::config({.maxPerAccountPerDay=limit})))); EXPECT_EQ(result.kind,"skipped"); EXPECT_EQ(result.reason,limit==0?"per_account_day_limit_invalid":"terminal_outcome_account_day_limit_must_be_one"); EXPECT_EQ(f.provider->callCount(),0);
  }
}

ASYNC_TEST("an ambiguous one-shot outcome stays unknown and never submits again") {
  Isolation isolation;

  Fixture f; f.provider=std::make_shared<FakeCodexUsageResetProvider>(terminalContract()); const auto reset=candidate({.requestId="one-shot-ambiguous"}); DONE(seedFences(f.kv,reset));
  f.provider->redeemResult=json::object{{"kind","unknown"},{"providerReceiptId",nullptr}}; f.provider->lookupResult=f.provider->redeemResult; f.provider->verifyResult=false;
  const auto deps=dependencies(f.kv,f.provider,f.clock,h::config({.maxPerAccountPerDay=1})); AWAIT(first,attemptCodexBankedReset(reset,deps));
  EXPECT_EQ(first.kind,"pending"); EXPECT_EQ(first.reason,"provider_commit_unknown"); EXPECT_EQ(stateOf(first),"unknown"); EXPECT_EQ(f.provider->redeemInputs.size(),1u);
  f.clock->advance(CODEX_BANKED_RESET_LEASE_MS+1); auto retry=reset; retry.requestId="one-shot-ambiguous-reconcile";
  AWAIT(reconciled,reconcileCodexBankedReset(retry,deps)); EXPECT_EQ(reconciled.kind,"pending"); EXPECT_EQ(reconciled.reason,"terminal_outcome_ambiguous"); EXPECT_EQ(stateOf(reconciled),"unknown"); EXPECT_EQ(f.provider->redeemInputs.size(),1u); EXPECT_EQ(f.provider->lookupInputs.size(),0u); EXPECT_EQ(f.provider->verificationInputs.size(),0u);
}

ASYNC_TEST("banked-reset telemetry retains only safe correlation fields") {
  Isolation isolation;

  Fixture f; auto contract=provenContract(); contract["receiptIdsSafeToPersistAndLog"]=false; f.provider=std::make_shared<FakeCodexUsageResetProvider>(contract);
  const auto reset=candidate({.accountId="raw-account-id-must-not-leak",.credentialVersion="raw-credential-version-must-not-leak",.requestId="safe-request-id"}); DONE(seedFences(f.kv,reset)); f.provider->redeemResult=json::object{{"kind","completed"},{"providerReceiptId","unapproved-provider-receipt"}};
  std::vector<std::pair<std::string,json::object>> events; std::vector<json::object> metrics; uos::codex::banked_reset::CodexBankedResetTelemetry telemetry;
  telemetry.event=[&](std::string event,json::object fields){events.emplace_back(event,fields);}; telemetry.metric=[&](std::string,double,json::object fields){metrics.push_back(fields);};
  AWAIT(result,attemptCodexBankedReset(reset,dependencies(f.kv,f.provider,f.clock,h::config(),telemetry))); EXPECT_EQ(result.kind,"verified"); ASSERT_EQ(f.provider->redeemInputs.size(),1u); EXPECT(!f.provider->redeemInputs[0].idempotencyKey.empty());
  const std::vector<std::string> expectedEvents{"codex_reset_eligible","codex_reset_claimed","codex_reset_submitted","codex_reset_verified"};
  for(const auto& expected:expectedEvents)EXPECT(std::any_of(events.begin(),events.end(),[&](const auto& event){return event.first==expected;}));
  for(const auto& event:events)EXPECT(rt::getString(event.second,"account_id_hash").has_value());
  const std::vector<std::string> rawValues{reset.accountId,reset.credentialVersion,f.provider->redeemInputs[0].idempotencyKey,"unapproved-provider-receipt"};
  for(const auto& event:events) { metrics.push_back(event.second); }
  for(const auto& fields:metrics){const auto serialized=rt::jsonStringify(fields);for(const auto& raw:rawValues)EXPECT_EQ(serialized.find(raw),std::string::npos);}
}

ASYNC_TEST("a lease-expired replacement takes over while the stale owner is blocked before redemption") {
  Isolation isolation;

  Fixture f; const auto reset=candidate(); DONE(seedFences(f.kv,reset)); auto gate=std::make_shared<Deferred<>>(); auto entered=std::make_shared<Deferred<>>(); f.provider->inventoryGate=gate->promise; f.provider->inventoryEntered=entered; const auto deps=f.deps();
  rt::Promise<uos::codex::banked_reset::CodexBankedResetOutcome> staleOwner(attemptCodexBankedReset(reset,deps)); co_await entered->promise.wait(); f.clock->advance(CODEX_BANKED_RESET_LEASE_MS+1); f.provider->inventory.observedAtMs=f.clock->nowMs;
  AWAIT(replacement,attemptCodexBankedReset(candidate({.requestId="lease-takeover"}),deps)); EXPECT_EQ(replacement.kind,"verified"); ASSERT(replacement.record); EXPECT_EQ(replacement.record->fence,3); EXPECT_EQ(f.provider->redeemInputs.size(),1u);
  gate->resolve(); const auto staleOutcome=co_await staleOwner.wait(); EXPECT_EQ(staleOutcome.kind,"pending"); EXPECT_EQ(staleOutcome.reason,"stale_owner"); EXPECT_EQ(f.provider->redeemInputs.size(),1u); EXPECT_EQ(f.provider->commitCount,1);
}

ASYNC_TEST("an expired claimed record cannot be taken over after its quota deadline") {
  Isolation isolation;

  Fixture f; const auto reset=candidate({.quotaResetAtMs=f.clock->nowMs+1}); DONE(seedFences(f.kv,reset)); auto gate=std::make_shared<Deferred<>>(); auto entered=std::make_shared<Deferred<>>(); f.provider->inventoryGate=gate->promise; f.provider->inventoryEntered=entered; const auto deps=f.deps();
  rt::Promise<uos::codex::banked_reset::CodexBankedResetOutcome> original(attemptCodexBankedReset(reset,deps)); co_await entered->promise.wait(); f.clock->advance(CODEX_BANKED_RESET_LEASE_MS+1); auto retry=reset; retry.requestId="expired-claim-takeover";
  AWAIT(takeover,attemptCodexBankedReset(retry,deps)); EXPECT_EQ(takeover.kind,"skipped"); EXPECT_EQ(takeover.reason,"quota_window_expired"); EXPECT_EQ(f.provider->inventoryInputs.size(),1u); EXPECT_EQ(f.provider->redeemInputs.size(),0u); EXPECT_EQ(f.provider->commitCount,0);
  AWAIT(daily,dailyRow(f.kv,reset.accountId,f.clock->nowMs)); EXPECT(daily.value.isNull()); gate->resolve(); const auto originalResult=co_await original.wait(); EXPECT_EQ(originalResult.kind,"rejected"); EXPECT_EQ(originalResult.reason,"quota_window_expired"); EXPECT_EQ(f.provider->redeemInputs.size(),0u);
}

ASYNC_TEST("a quota deadline crossing during claimed-takeover fence reads leaves the claim untouched") {
  Isolation isolation;

  Fixture f; const auto reset=candidate({.quotaResetAtMs=f.clock->nowMs+CODEX_BANKED_RESET_LEASE_MS+2}); DONE(seedFences(f.kv,reset)); auto gate=std::make_shared<Deferred<>>(); auto entered=std::make_shared<Deferred<>>(); f.provider->inventoryGate=gate->promise; f.provider->inventoryEntered=entered; const auto deps=f.deps();
  rt::Promise<uos::codex::banked_reset::CodexBankedResetOutcome> original(attemptCodexBankedReset(reset,deps)); co_await entered->promise.wait(); f.clock->advance(CODEX_BANKED_RESET_LEASE_MS+1);
  f.kv->beforeGet=[&](rt::KvKey key,double)->std::optional<rt::Promise<void>>{if(encodeKey(key)==encodeKey(credentialFenceKey(reset.accountId))){f.kv->beforeGet={};f.clock->advance(1);}return std::nullopt;};
  auto retry=reset; retry.requestId="claim-takeover-deadline"; AWAIT(takeover,attemptCodexBankedReset(retry,deps)); EXPECT_EQ(takeover.kind,"skipped"); EXPECT_EQ(takeover.reason,"quota_window_expired"); EXPECT_EQ(f.kv->atomicCommitCount,1); EXPECT_EQ(f.provider->inventoryInputs.size(),1u); EXPECT_EQ(f.provider->redeemInputs.size(),0u);
  gate->resolve(); const auto originalResult=co_await original.wait(); EXPECT_EQ(originalResult.kind,"rejected"); EXPECT_EQ(originalResult.reason,"quota_window_expired"); EXPECT_EQ(f.provider->redeemInputs.size(),0u);
}

ASYNC_TEST("a post-submission lease takeover fences the paused owner before provider redemption") {
  Isolation isolation;

  Fixture f; const auto reset=candidate(); DONE(seedFences(f.kv,reset)); auto gate=std::make_shared<Deferred<>>(); auto entered=std::make_shared<Deferred<>>();
  f.kv->beforeGet=[&](rt::KvKey,double)->std::optional<rt::Promise<void>>{if(f.kv->atomicCommitCount!=2)return std::nullopt;f.kv->beforeGet={};entered->resolve();return gate->promise;}; const auto deps=f.deps();
  rt::Promise<uos::codex::banked_reset::CodexBankedResetOutcome> pausedOwner(attemptCodexBankedReset(reset,deps)); co_await entered->promise.wait(); f.clock->advance(CODEX_BANKED_RESET_LEASE_MS+1); f.provider->lookupResult=json::object{{"kind","unknown"},{"providerReceiptId",nullptr}}; f.provider->verifyResult=false;
  AWAIT(takeover,reconcileCodexBankedReset(candidate({.requestId="post-submission-takeover"}),deps)); EXPECT_EQ(takeover.kind,"pending"); EXPECT_EQ(takeover.reason,"verification_not_applied"); EXPECT_EQ(stateOf(takeover),"unknown"); ASSERT(takeover.record); EXPECT_EQ(takeover.record->fence,2); EXPECT_EQ(f.provider->redeemInputs.size(),0u);
  gate->resolve(); const auto staleOutcome=co_await pausedOwner.wait(); EXPECT_EQ(staleOutcome.kind,"pending"); EXPECT_EQ(staleOutcome.reason,"stale_owner"); EXPECT_EQ(f.provider->redeemInputs.size(),0u); EXPECT_EQ(f.provider->commitCount,0);
}

ASYNC_TEST("a routing-fence change between the strong read and durable submission CAS prevents redemption") {
  Isolation isolation;

  Fixture f; const auto reset=candidate(); DONE(seedFences(f.kv,reset));
  f.kv->beforeAtomicCommit=[&](double commitNumber){if(commitNumber!=2)return;f.kv->beforeAtomicCommit={};const auto row=json::object{{"kind","routing"},{"routing_generation",rt::makeNumber(reset.routingGeneration+1)}};(void)f.kv->set(routingFenceKey(reset.accountId),rt::KvValue(row));};
  AWAIT(result,attemptCodexBankedReset(reset,f.deps())); EXPECT_EQ(result.kind,"pending"); EXPECT_EQ(result.reason,"routing_fence_stale"); EXPECT_EQ(stateOf(result),"claimed"); EXPECT_EQ(f.provider->redeemInputs.size(),0u); EXPECT_EQ(f.provider->commitCount,0); EXPECT_EQ(f.kv->atomicCommitCount,2);EXPECT(hasKey(f.kv->atomicCheckBatches[1],routingFenceKey(reset.accountId)));
}

ASYNC_TEST("a routing-fence change after submission is caught by the final renewal CAS") {
  Isolation isolation;

  Fixture f; const auto reset=candidate(); DONE(seedFences(f.kv,reset));
  f.kv->beforeAtomicCommit=[&](double commitNumber){if(commitNumber!=3)return;f.kv->beforeAtomicCommit={};const auto row=json::object{{"kind","routing"},{"routing_generation",rt::makeNumber(reset.routingGeneration+1)}};(void)f.kv->set(routingFenceKey(reset.accountId),rt::KvValue(row));};
  AWAIT(result,attemptCodexBankedReset(reset,f.deps())); EXPECT_EQ(result.kind,"pending"); EXPECT_EQ(result.reason,"routing_fence_stale"); EXPECT_EQ(stateOf(result),"submitted"); EXPECT_EQ(f.provider->redeemInputs.size(),0u); EXPECT_EQ(f.provider->commitCount,0); EXPECT(hasKey(f.kv->atomicCheckBatches[2],routingFenceKey(reset.accountId)));EXPECT(hasKey(f.kv->atomicCheckBatches[2],credentialFenceKey(reset.accountId)));
}

ASYNC_TEST("an auth fence change after submission is caught by the final renewal CAS") {
  Isolation isolation;

  Fixture f; const auto reset=candidate(); DONE(seedFences(f.kv,reset));
  f.kv->beforeAtomicCommit=[&](double commitNumber){if(commitNumber!=3)return;f.kv->beforeAtomicCommit={};const auto row=json::object{{"kind","credential"},{"credential_version","test-credential-v2-rotated"}};(void)f.kv->set(credentialFenceKey(reset.accountId),rt::KvValue(row));};
  AWAIT(result,attemptCodexBankedReset(reset,f.deps())); EXPECT_EQ(result.kind,"pending"); EXPECT_EQ(result.reason,"routing_fence_stale"); EXPECT_EQ(stateOf(result),"submitted"); EXPECT_EQ(f.provider->redeemInputs.size(),0u); EXPECT_EQ(f.provider->commitCount,0); EXPECT(hasKey(f.kv->atomicCheckBatches[2],routingFenceKey(reset.accountId)));EXPECT(hasKey(f.kv->atomicCheckBatches[2],credentialFenceKey(reset.accountId)));
}

ASYNC_TEST("a credential-rotation-style fence change before durable submission prevents redemption") {
  Isolation isolation;

  Fixture f; const auto reset=candidate(); DONE(seedFences(f.kv,reset));
  f.kv->beforeAtomicCommit=[&](double commitNumber){if(commitNumber!=2)return;f.kv->beforeAtomicCommit={};const auto row=json::object{{"kind","credential"},{"credential_version","test-credential-v2-rotated"}};(void)f.kv->set(credentialFenceKey(reset.accountId),rt::KvValue(row));};
  AWAIT(result,attemptCodexBankedReset(reset,f.deps())); EXPECT_EQ(result.kind,"pending"); EXPECT_EQ(result.reason,"routing_fence_stale"); EXPECT_EQ(stateOf(result),"claimed"); EXPECT_EQ(f.provider->redeemInputs.size(),0u); EXPECT_EQ(f.provider->commitCount,0); EXPECT(hasKey(f.kv->atomicCheckBatches[1],credentialFenceKey(reset.accountId)));
}

ASYNC_TEST("a durable unknown reset is reconciled while live configuration is disabled without a second redemption") {
  Isolation isolation;

  Fixture f; const auto reset=candidate(); DONE(seedFences(f.kv,reset)); f.provider->redeemResult=json::object{{"kind","unknown"},{"providerReceiptId",nullptr}}; f.provider->commitOnRedeem=true;
  AWAIT(first,attemptCodexBankedReset(reset,f.deps())); EXPECT_EQ(first.kind,"pending"); EXPECT_EQ(stateOf(first),"unknown"); EXPECT_EQ(f.provider->redeemInputs.size(),1u);
  f.clock->advance(CODEX_BANKED_RESET_LEASE_MS+1); f.provider->lookupResult=json::object{{"kind","completed"},{"providerReceiptId","receipt-recovered-while-disabled"}}; const auto disabledConfig=h::config({.enabled=false});
  const auto deps=dependencies(f.kv,f.provider,f.clock,disabledConfig,{},[disabledConfig]{return disabledConfig;}); AWAIT(recovered,reconcileCodexBankedReset(candidate({.requestId="disabled-recovery"}),deps));
  EXPECT_EQ(recovered.kind,"verified"); EXPECT_EQ(stateOf(recovered),"verified"); EXPECT_EQ(f.provider->redeemInputs.size(),1u); EXPECT_EQ(f.provider->lookupInputs.size(),1u); EXPECT_EQ(f.provider->commitCount,1);
}

ASYNC_TEST("seeded state-machine invariant: one account/window never reaches more than one fake provider commit") {
  Isolation isolation;

  std::uint32_t seed=0x5eedc0de; auto next=[&]{seed=seed*1'664'525u+1'013'904'223u; return seed;};
  for(int index=0;index<24;++index){ const auto caseSeed=next(); Fixture f;
    const auto reset=candidate({.accountId="property-account-"+std::to_string(index),.credentialVersion="property-credential-"+std::to_string(caseSeed),.quotaResetAtMs=1'700'000'060'000.0+index,.routingGeneration=static_cast<double>(caseSeed%1000),.requestId="property-"+std::to_string(caseSeed)});
    DONE(seedFences(f.kv,reset)); const auto deps=f.deps(); const auto scenario=caseSeed%3;
    if(scenario==1){f.provider->redeemResult=json::object{{"kind","unknown"},{"providerReceiptId",nullptr}};f.provider->commitOnRedeem=true; AWAIT(first,attemptCodexBankedReset(reset,deps)); EXPECT_EQ(first.kind,"pending"); f.clock->advance(CODEX_BANKED_RESET_LEASE_MS+1);f.provider->lookupResult=json::object{{"kind","completed"},{"providerReceiptId","receipt-"+std::to_string(caseSeed)}}; auto retry=reset;retry.requestId="recovery-"+std::to_string(caseSeed);DONE(reconcileCodexBankedReset(retry,deps));}
    else {if(scenario==2)f.provider->redeemResult=json::object{{"kind","rejected"},{"reason","rejected-"+std::to_string(caseSeed)}};DONE(attemptCodexBankedReset(reset,deps));auto retry=reset;retry.requestId="duplicate-"+std::to_string(caseSeed);DONE(attemptCodexBankedReset(retry,deps));}
    std::set<std::string> keys; for(const auto& input:f.provider->redeemInputs)keys.insert(input.idempotencyKey); EXPECT(f.provider->commitCount<=1); EXPECT(f.provider->redeemInputs.size()<=1); EXPECT(keys.size()<=1);
  }
}

ASYNC_TEST("a malformed truthy verification result remains unknown and cannot authorize a reset") {
  Isolation isolation;
  Fixture f; const auto reset=candidate(); DONE(seedFences(f.kv,reset));
  f.provider->verifyResult=json::object{{"applied",true}};
  AWAIT(result,attemptCodexBankedReset(reset,f.deps()));
  EXPECT_EQ(result.kind,"pending"); EXPECT_EQ(result.reason,"verification_response_invalid");
  EXPECT_EQ(stateOf(result),"unknown"); ASSERT(result.record);
  EXPECT_EQ(result.record->last_error_code,"verification_response_invalid");
  EXPECT_EQ(f.provider->commitCount,1); EXPECT_EQ(f.provider->redeemInputs.size(),1u); EXPECT_EQ(f.provider->verificationInputs.size(),1u);
}
