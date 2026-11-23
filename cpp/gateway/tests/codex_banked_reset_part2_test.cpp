// Links: tests/codex_banked_reset_part2_test.cpp tests/helpers/codex_banked_reset_harness.cpp src/codex/reset_settings.cpp
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
// Port of tests/codex-banked-reset-part2.test.ts.
// The stalled-inventory case observes the real static AbortSignal.timeout deadline and signal,
// rather than the JS-only replacement-property identity; all resulting state/call assertions are retained.

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
  using Outcome = uos::codex::banked_reset::CodexBankedResetOutcome;
  using PoolOutcome = uos::codex::banked_reset::CodexBankedResetPoolOutcome;
  using PoolCandidate = uos::codex::banked_reset::CodexBankedResetPoolCandidate;
  struct Fixture {
    std::shared_ptr<MemoryKv> kv = std::make_shared<MemoryKv>();
    std::shared_ptr<FakeCodexUsageResetProvider> provider = std::make_shared<FakeCodexUsageResetProvider>();
    std::shared_ptr<TestClock> clock = std::make_shared<TestClock>();
    auto deps() const { return dependencies(kv, provider, clock); }
  };
  std::optional<std::string_view> stateOf(const Outcome& result) {
    return result.record ? std::optional<std::string_view>(uos::types::toString(result.record->state)) : std::nullopt;
  }
  rt::Task<rt::KvEntryMaybe<>> dailyRow(std::shared_ptr<MemoryKv> kv, std::string accountId, double nowMs) {
    AWAIT(hash, testHash(accountId));
    const auto key = codexResetAccountDailyKey(hash, rt::Date(nowMs).toISOString().substr(0, 10));
    AWAIT(row, kv->get(key)); co_return row;
  }
  std::vector<PoolCandidate> singlePool(const uos::codex::banked_reset::CodexBankedResetCandidate& reset, std::shared_ptr<FakeCodexUsageResetProvider> provider) {
    return std::vector<PoolCandidate>{{.slot=0,.candidate=reset,.provider=std::move(provider)}};
  }
}
ASYNC_TEST("generated banked-reset event sequences retain the durable state-machine invariants") {
  Isolation isolation;

  std::uint32_t seed=0x41c60de5; auto next=[&]{seed=seed*1'103'515'245u+12'345u;return seed;};
  for(int sequenceIndex=0;sequenceIndex<48;++sequenceIndex){
    const auto sequenceSeed=next(); auto random=sequenceSeed; auto choose=[&]{random=random*1'664'525u+1'013'904'223u;return random;};
    auto sequence=BANKED_RESET_GENERATED_EVENTS; for(std::size_t index=sequence.size()-1;index>0;--index){const auto swap=choose()%(index+1);std::swap(sequence[index],sequence[swap]);}
    for(int index=0;index<11;++index)sequence.push_back(BANKED_RESET_GENERATED_EVENTS[choose()%BANKED_RESET_GENERATED_EVENTS.size()]);
    Fixture f; const auto reset=candidate({.accountId="generated-account-"+std::to_string(sequenceIndex),.credentialVersion="generated-credential-"+std::to_string(sequenceSeed),.quotaResetAtMs=1'700'000'060'000.0+sequenceIndex,.routingGeneration=static_cast<double>(sequenceSeed%1000),.requestId="generated-"+std::to_string(sequenceSeed)});
    const std::vector<std::string> modes{"live","shadow","disabled"};const auto mode=modes[sequenceSeed%modes.size()];const auto configured=h::config({.enabled=mode=="live"||mode=="shadow",.mode=mode});
    const auto deps=dependencies(f.kv,f.provider,f.clock,configured); DONE(seedFences(f.kv,reset)); f.provider->redeemResult=sanitizedProviderFixtures().rate_limit;f.provider->commitOnRedeem=true;
    auto scenario=std::make_shared<GeneratedResetScenario>(GeneratedResetScenario{.reset=reset,.deps=deps,.provider=f.provider,.kv=f.kv,.clock=f.clock,.sequenceSeed=static_cast<double>(sequenceSeed),.qualifyingObservationIsCurrent=false,.lastOutcome=std::nullopt,.postResetInferenceRetries=0,.credentialWasRotated=false});
    for(const auto event:sequence){const auto label="seed "+std::to_string(sequenceSeed)+" event "+std::string(h::toString(event));const auto before=static_cast<double>(f.provider->redeemInputs.size());DONE(applyBankedResetEvent(scenario,event,label));assertBankedResetInvariants(*scenario,label,before,mode);}
  }
}

ASYNC_TEST("two different accounts can each redeem once in the same UTC day") {
  Isolation isolation;

  Fixture f;const auto deps=dependencies(f.kv,f.provider,f.clock,h::config({.maxPerAccountPerDay=1}));const auto firstCandidate=candidate();const auto secondCandidate=candidate({.accountId="test-account-b",.requestId="second-account"});DONE(seedFences(f.kv,firstCandidate));DONE(seedFences(f.kv,secondCandidate));
  AWAIT(first,attemptCodexBankedReset(firstCandidate,deps));AWAIT(second,attemptCodexBankedReset(secondCandidate,deps));EXPECT_EQ(first.kind,"verified");EXPECT_EQ(second.kind,"verified");EXPECT_EQ(f.provider->redeemInputs.size(),2u);EXPECT_EQ(f.provider->commitCount,2);
  AWAIT(firstDaily,dailyRow(f.kv,firstCandidate.accountId,f.clock->nowMs));AWAIT(secondDaily,dailyRow(f.kv,secondCandidate.accountId,f.clock->nowMs));EXPECT_EQ(firstDaily.value["submission_count"].asNumber(),1);EXPECT_EQ(secondDaily.value["submission_count"].asNumber(),1);
}

ASYNC_TEST("an inventory failure leaves the account's daily submission budget available") {
  Isolation isolation;

  Fixture f; const auto configured=h::config({.maxPerAccountPerDay=1});const auto firstCandidate=candidate();const auto secondCandidate=candidate({.accountId="test-account-b",.requestId="after-inventory-failure"});DONE(seedFences(f.kv,firstCandidate));DONE(seedFences(f.kv,secondCandidate));f.provider->inventoryFailure=rt::makeReason(rt::JsError("inventory unavailable"));
  AWAIT(first,attemptCodexBankedReset(firstCandidate,dependencies(f.kv,f.provider,f.clock,configured)));EXPECT_EQ(first.kind,"rejected");EXPECT_EQ(first.reason,"inventory_unavailable");AWAIT(firstDaily,dailyRow(f.kv,firstCandidate.accountId,f.clock->nowMs));EXPECT(firstDaily.value.isNull());
  f.provider->inventoryFailure=nullptr;AWAIT(second,attemptCodexBankedReset(secondCandidate,dependencies(f.kv,f.provider,f.clock,configured)));EXPECT_EQ(second.kind,"verified");EXPECT_EQ(f.provider->redeemInputs.size(),1u);AWAIT(secondDaily,dailyRow(f.kv,secondCandidate.accountId,f.clock->nowMs));EXPECT_EQ(secondDaily.value["submission_count"].asNumber(),1);
}

ASYNC_TEST("a claim held across UTC midnight cannot bypass the next day's per-account redemption cap") {
  Isolation isolation;

  Fixture f; const double dayMs=24*60*60*1000;const auto firstCandidate=candidate({.quotaResetAtMs=f.clock->nowMs+2*dayMs});const auto secondCandidate=candidate({.accountId="test-account-a",.quotaResetAtMs=f.clock->nowMs+dayMs+60'000,.requestId="next-day-account"});DONE(seedFences(f.kv,firstCandidate));DONE(seedFences(f.kv,secondCandidate));const auto deps=dependencies(f.kv,f.provider,f.clock,h::config({.maxPerAccountPerDay=1}));
  auto gate=std::make_shared<Deferred<>>();auto entered=std::make_shared<Deferred<>>();f.provider->inventoryGate=gate->promise;f.provider->inventoryEntered=entered;
  rt::Promise<Outcome> heldClaim(attemptCodexBankedReset(firstCandidate,deps));co_await entered->promise.wait();f.clock->advance(dayMs);f.provider->inventory.observedAtMs=f.clock->nowMs;gate->resolve();const auto first=co_await heldClaim.wait();
  EXPECT_EQ(first.kind,"rejected");EXPECT_EQ(first.reason,"claim_day_elapsed");EXPECT_EQ(f.provider->redeemInputs.size(),0u);AWAIT(second,attemptCodexBankedReset(secondCandidate,deps));EXPECT_EQ(second.kind,"verified");EXPECT_EQ(f.provider->redeemInputs.size(),1u);EXPECT_EQ(f.provider->commitCount,1);AWAIT(daily,dailyRow(f.kv,secondCandidate.accountId,f.clock->nowMs));EXPECT_EQ(daily.value["submission_count"].asNumber(),1);
}

ASYNC_TEST("empty or unsupported inventory and provider rejection become durable terminal rejections") {
  Isolation isolation;

  const std::vector<std::string> reasons{"inventory_empty","inventory_no_eligible_codex_credit","provider_rejected"};
  for(std::size_t index=0;index<reasons.size();++index){Fixture f; if(index==0)f.provider->inventory={.availableCount=0,.observedAtMs=1'700'000'000'000,.credits={}};else if(index==1)f.provider->inventory={.availableCount=1,.observedAtMs=1'700'000'000'000,.credits={{.id="unreviewed-credit",.status="available",.resetType="unreviewed_reset",.expiresAtMs=std::nullopt}}};else f.provider->redeemResult=json::object{{"kind","rejected"},{"reason","provider validation rejected the request"}};
    const auto deps=f.deps();const auto reset=candidate();DONE(seedFences(f.kv,reset));AWAIT(first,attemptCodexBankedReset(reset,deps));AWAIT(repeat,attemptCodexBankedReset(candidate({.requestId="repeat-rejection"}),deps));
    EXPECT_EQ(first.kind,"rejected");EXPECT_EQ(first.reason,reasons[index]);EXPECT_EQ(stateOf(first),"rejected");EXPECT_EQ(repeat.kind,"rejected");EXPECT_EQ(f.provider->redeemInputs.size(),index==2?1u:0u);EXPECT_EQ(f.provider->commitCount,0);
  }
}

ASYNC_TEST("stale or future inventory cannot authorize a redemption") {
  Isolation isolation;

  const std::vector<double> deltas{-(CODEX_BANKED_RESET_INVENTORY_MAX_AGE_MS+1),1};for(const auto delta:deltas){Fixture f;const auto reset=candidate({.requestId=delta==1?"inventory-future":"inventory-stale"});DONE(seedFences(f.kv,reset));f.provider->inventory.observedAtMs=f.clock->nowMs+delta;
    AWAIT(result,attemptCodexBankedReset(reset,f.deps()));EXPECT_EQ(result.kind,"rejected");EXPECT_EQ(result.reason,"inventory_response_invalid_or_unsupported");EXPECT_EQ(f.provider->inventoryInputs.size(),1u);EXPECT_EQ(f.provider->redeemInputs.size(),0u);EXPECT_EQ(f.provider->commitCount,0);
  }
}

ASYNC_TEST("KV outage and claim CAS exhaustion fail closed before any provider interaction") {
  Isolation isolation;

  Fixture f;const auto reset=candidate();DONE(seedFences(f.kv,reset));f.kv->getFailure=rt::makeReason(rt::JsError("in-memory KV unavailable"));AWAIT(unavailable,attemptCodexBankedReset(reset,f.deps()));EXPECT_EQ(unavailable.kind,"skipped");EXPECT_EQ(unavailable.reason,"kv_unavailable");EXPECT_EQ(f.provider->callCount(),0);EXPECT_EQ(f.provider->commitCount,0);
  Fixture cas; for(double n=1;n<=4;++n) { cas.kv->failCommitNumbers.add(n); }const auto casCandidate=candidate();DONE(seedFences(cas.kv,casCandidate));AWAIT(exhausted,attemptCodexBankedReset(casCandidate,dependencies(cas.kv,cas.provider,f.clock)));EXPECT_EQ(exhausted.kind,"skipped");EXPECT_EQ(exhausted.reason,"kv_cas_exhausted");EXPECT_EQ(cas.kv->atomicCommitCount,4);EXPECT_EQ(cas.provider->callCount(),0);EXPECT_EQ(cas.provider->commitCount,0);
}

ASYNC_TEST("a transient CAS conflict at every happy-path transition retains one logical redemption") {
  Isolation isolation;

  for(double failedCommit=1;failedCommit<=5;++failedCommit){Fixture f;const auto reset=candidate({.requestId="transient-cas-"+rt::numberToString(failedCommit)});DONE(seedFences(f.kv,reset));f.kv->failCommitNumbers.add(failedCommit);AWAIT(result,attemptCodexBankedReset(reset,f.deps()));EXPECT_EQ(result.kind,"verified");EXPECT_EQ(f.provider->commitCount,1);EXPECT_EQ(f.provider->redeemInputs.size(),1u);std::set<std::string> keys;for(const auto& input:f.provider->redeemInputs) { keys.insert(input.idempotencyKey); }EXPECT_EQ(keys.size(),1u);}
}

ASYNC_TEST("KV unavailability after claim, after submission, and during verification never opens a second spend path") {
  Isolation isolation;

  {Fixture f;const auto reset=candidate({.requestId="kv-after-claim"});DONE(seedFences(f.kv,reset));auto gate=std::make_shared<Deferred<>>();auto entered=std::make_shared<Deferred<>>();f.provider->inventoryEntered=entered;f.provider->inventoryGate=gate->promise;rt::Promise<Outcome> pending(attemptCodexBankedReset(reset,f.deps()));co_await entered->promise.wait();f.kv->getFailure=rt::makeReason(rt::JsError("KV failed after claim"));gate->resolve();const auto outcome=co_await pending.wait();EXPECT_EQ(outcome.kind,"pending");EXPECT_EQ(outcome.reason,"kv_unavailable");EXPECT_EQ(stateOf(outcome),"claimed");EXPECT_EQ(f.provider->redeemInputs.size(),0u);}
  {Fixture f;const auto reset=candidate({.requestId="kv-after-submission"});DONE(seedFences(f.kv,reset));f.kv->beforeAtomicCommit=[&](double number){if(number==2)f.kv->getFailure=rt::makeReason(rt::JsError("KV failed after submitted transition"));};AWAIT(outcome,attemptCodexBankedReset(reset,f.deps()));EXPECT_EQ(outcome.kind,"pending");EXPECT_EQ(outcome.reason,"kv_unavailable");EXPECT_EQ(stateOf(outcome),"submitted");EXPECT_EQ(f.provider->redeemInputs.size(),0u);}
  {Fixture f;const auto reset=candidate({.requestId="kv-during-verification"});DONE(seedFences(f.kv,reset));auto gate=std::make_shared<Deferred<>>();auto entered=std::make_shared<Deferred<>>();f.provider->verifyEntered=entered;f.provider->verifyGate=gate->promise;rt::Promise<Outcome> pending(attemptCodexBankedReset(reset,f.deps()));co_await entered->promise.wait();f.kv->getFailure=rt::makeReason(rt::JsError("KV failed while finalizing verification"));gate->resolve();const auto outcome=co_await pending.wait();EXPECT_EQ(outcome.kind,"pending");EXPECT_EQ(outcome.reason,"verification_cas_failed");EXPECT_EQ(stateOf(outcome),"submitted");EXPECT_EQ(f.provider->redeemInputs.size(),1u);EXPECT_EQ(f.provider->commitCount,1);}

}

ASYNC_TEST("a stale owner cannot finalize verified after a lease-takeover reconciliation") {
  Isolation isolation;

  Fixture f;const auto reset=candidate({.requestId="stale-owner-verify"});DONE(seedFences(f.kv,reset));auto gate=std::make_shared<Deferred<>>();auto entered=std::make_shared<Deferred<>>();f.provider->verifyEntered=entered;f.provider->verifyGate=gate->promise;const auto deps=f.deps();rt::Promise<Outcome> original(attemptCodexBankedReset(reset,deps));co_await entered->promise.wait();
  f.clock->advance(CODEX_BANKED_RESET_LEASE_MS+1);f.provider->lookupResult=sanitizedProviderFixtures().lookup_pending;f.provider->verifyResult=false;AWAIT(takeover,reconcileCodexBankedReset(candidate({.requestId="stale-owner-verify-takeover"}),deps));EXPECT_EQ(takeover.kind,"pending");EXPECT_EQ(stateOf(takeover),"unknown");ASSERT(takeover.record);EXPECT_EQ(takeover.record->fence,3);
  f.provider->verifyResult=true;gate->resolve();const auto stale=co_await original.wait();EXPECT_EQ(stale.kind,"pending");EXPECT_EQ(stale.reason,"verification_cas_failed");EXPECT_EQ(f.provider->redeemInputs.size(),1u);const auto row=f.kv->redemptionRecord(codexResetRedemptionKey(requiredHash(takeover.accountIdHash),requiredHash(takeover.quotaGeneration)));ASSERT(row);EXPECT_EQ(rt::getString(*row,"state"),"unknown");
}

ASYNC_TEST("receipt CAS loss leaves the transaction pending and recovery uses lookup instead of resubmission") {
  Isolation isolation;

  Fixture f;for(double commit=4;commit<=7;++commit) { f.kv->failCommitNumbers.add(commit); }const auto deps=f.deps();const auto reset=candidate();DONE(seedFences(f.kv,reset));AWAIT(first,attemptCodexBankedReset(reset,deps));EXPECT_EQ(first.kind,"pending");EXPECT_EQ(first.reason,"receipt_cas_failed");EXPECT_EQ(f.provider->redeemInputs.size(),1u);EXPECT_EQ(f.provider->commitCount,1);EXPECT_EQ(f.provider->verificationInputs.size(),0u);
  f.clock->advance(CODEX_BANKED_RESET_LEASE_MS+1);f.provider->lookupResult=json::object{{"kind","completed"},{"providerReceiptId","receipt-after-cas-recovery"}};AWAIT(recovered,attemptCodexBankedReset(candidate({.requestId="cas-recovery"}),deps));EXPECT_EQ(recovered.kind,"verified");EXPECT_EQ(f.provider->redeemInputs.size(),1u);EXPECT_EQ(f.provider->lookupInputs.size(),1u);EXPECT_EQ(f.provider->commitCount,1);
}

ASYNC_TEST("sanitized provider fixtures accept only complete known boundary results") {
  Isolation isolation;

  const auto& fixtures=sanitizedProviderFixtures();
  struct Case {std::string name;json::value result;double expectedCommitCount;std::string expectedReceipt;};
  const std::vector<Case> cases{{"completed",fixtures.redemption_completed,1,"fixture-completed"},{"accepted",fixtures.redemption_accepted,1,"fixture-accepted"},{"idempotent replay",fixtures.redemption_already_redeemed,0,"fixture-replay"}};
  for(const auto& fixture:cases){Fixture f;const auto reset=candidate({.requestId="fixture-"+fixture.name});DONE(seedFences(f.kv,reset));f.provider->inventory=fixtures.inventory_available;f.provider->redeemResult=fixture.result;AWAIT(outcome,attemptCodexBankedReset(reset,f.deps()));EXPECT_EQ(outcome.kind,"verified");ASSERT(outcome.record);EXPECT_EQ(outcome.record->provider_receipt_id,fixture.expectedReceipt);EXPECT_EQ(f.provider->commitCount,fixture.expectedCommitCount);EXPECT_EQ(f.provider->redeemInputs.size(),1u);}
  const std::vector<json::value> malformed{fixtures.malformed_success,fixtures.schema_drift};
  for(const auto& fixture:malformed){Fixture f;const auto reset=candidate({.requestId="fixture-malformed-"+std::string(*rt::getString(fixture,"kind"))});DONE(seedFences(f.kv,reset));f.provider->redeemResult=fixture;AWAIT(outcome,attemptCodexBankedReset(reset,f.deps()));EXPECT_EQ(outcome.kind,"pending");EXPECT_EQ(outcome.reason,"submit_response_invalid");EXPECT_EQ(stateOf(outcome),"unknown");EXPECT_EQ(f.provider->verificationInputs.size(),0u);}
  const std::vector<json::value> lookups{fixtures.lookup_pending,fixtures.lookup_rejected,fixtures.lookup_not_found};
  for(const auto& fixture:lookups){Fixture f;const auto kind=std::string(*rt::getString(fixture,"kind"));const auto reset=candidate({.requestId="fixture-lookup-"+kind});DONE(seedFences(f.kv,reset));f.provider->redeemResult=fixtures.rate_limit;f.provider->commitOnRedeem=true;AWAIT(first,attemptCodexBankedReset(reset,f.deps()));EXPECT_EQ(first.kind,"pending");ASSERT_EQ(f.provider->redeemInputs.size(),1u);const auto stableKey=f.provider->redeemInputs[0].idempotencyKey;f.clock->advance(CODEX_BANKED_RESET_LEASE_MS+1);f.provider->lookupResult=fixture;f.provider->verifyResult=false;AWAIT(recovered,reconcileCodexBankedReset(candidate({.requestId="fixture-lookup-recovery-"+kind}),f.deps()));EXPECT_EQ(recovered.kind,"pending");EXPECT_EQ(stateOf(recovered),"unknown");EXPECT_EQ(f.provider->redeemInputs.size(),1u);ASSERT_EQ(f.provider->lookupInputs.size(),1u);EXPECT_EQ(f.provider->lookupInputs[0].idempotencyKey,stableKey);}

}

ASYNC_TEST("provider rejection matrix keeps definitive failures terminal and ambiguous failures unknown") {
  Isolation isolation;

  const std::vector<std::string> names_{"reset allowance exhausted","provider authentication failure","provider validation failure","provider rate limit with unknown commit status","provider server failure with unknown commit status"};
  for(std::size_t index=0;index<names_.size();++index){Fixture f;const auto reset=candidate({.requestId="rejection-"+names_[index]});DONE(seedFences(f.kv,reset));const auto& fixtures=sanitizedProviderFixtures();
    if(index==0)f.provider->inventory=fixtures.inventory_empty;else if(index==1)f.provider->redeemResult=fixtures.authentication_error;else if(index==2)f.provider->redeemResult=json::object{{"kind","rejected"},{"reason","fixture-provider-validation-failure"}};else{f.provider->redeemResult=index==3?fixtures.rate_limit:fixtures.server_error;f.provider->commitOnRedeem=true;}
    const auto deps=f.deps();AWAIT(first,attemptCodexBankedReset(reset,deps));AWAIT(repeat,attemptCodexBankedReset(candidate({.requestId="rejection-repeat-"+names_[index]}),deps));
    EXPECT_EQ(first.kind,index<3?"rejected":"pending");EXPECT_EQ(first.reason,index==0?"inventory_empty":index<3?"provider_rejected":"provider_commit_unknown");EXPECT_EQ(stateOf(first),index<3?"rejected":"unknown");EXPECT_EQ(f.provider->redeemInputs.size(),index==0?0u:1u);EXPECT(repeat.kind!="verified");EXPECT_EQ(f.provider->redeemInputs.size(),index==0?0u:1u);
  }
}

ASYNC_TEST("timeout and crash matrix never creates a second logical redemption") {
  Isolation isolation;

  const std::vector<std::string> names_{"timeout before provider receives a commit","provider commits and its response is lost","verification times out after a confirmed submission"};
  for(std::size_t index=0;index<names_.size();++index){Fixture f;const auto reset=candidate({.requestId="timeout-"+names_[index]});DONE(seedFences(f.kv,reset));
    if(index==0)f.provider->redeemFailure=rt::makeReason(rt::DOMException("timeout before provider commit","TimeoutError"));else if(index==1)f.provider->redeemFailureAfterCommit=rt::makeReason(rt::DOMException("response lost","TimeoutError"));else f.provider->verifyFailure=rt::makeReason(rt::DOMException("verification timeout","TimeoutError"));
    const auto deps=f.deps();AWAIT(first,attemptCodexBankedReset(reset,deps));EXPECT_EQ(first.kind,"pending");EXPECT_EQ(first.reason,index<2?"submit_transport_unknown":"verification_unavailable");EXPECT_EQ(stateOf(first),"unknown");EXPECT_EQ(f.provider->commitCount,index==0?0:1);ASSERT_EQ(f.provider->redeemInputs.size(),1u);const auto stableKey=f.provider->redeemInputs[0].idempotencyKey;
    const auto submission=std::find_if(f.provider->calls.begin(),f.provider->calls.end(),[](const auto& call){return call.method=="redeem";});ASSERT(submission!=f.provider->calls.end());const std::optional<std::string> expectedStage=index<2?std::optional<std::string>(index==0?"before_provider_commit":"after_provider_commit"):std::nullopt;EXPECT_EQ(submission->timeoutStage,expectedStage);
    f.provider->redeemFailure=nullptr;f.provider->redeemFailureAfterCommit=nullptr;f.provider->verifyFailure=nullptr;f.provider->lookupResult=sanitizedProviderFixtures().lookup_completed;f.provider->verifyResult=true;f.clock->advance(CODEX_BANKED_RESET_LEASE_MS+1);AWAIT(recovered,reconcileCodexBankedReset(candidate({.requestId="timeout-recovery-"+names_[index]}),deps));EXPECT_EQ(recovered.kind,"verified");EXPECT_EQ(f.provider->redeemInputs.size(),1u);ASSERT(!f.provider->lookupInputs.empty());EXPECT_EQ(f.provider->lookupInputs.back().idempotencyKey,stableKey);EXPECT_EQ(f.provider->commitCount,index==0?0:1);
  }
  Fixture f;const auto reset=candidate({.requestId="receipt-persisted-then-crash"});DONE(seedFences(f.kv,reset));auto gate=std::make_shared<Deferred<>>();auto entered=std::make_shared<Deferred<>>();f.provider->verifyGate=gate->promise;f.provider->verifyEntered=entered;const auto deps=f.deps();rt::Promise<Outcome> original(attemptCodexBankedReset(reset,deps));co_await entered->promise.wait();
  std::optional<json::value> persisted;for(const auto& entry:f.kv->entries.values()){const auto value=entry.value.toJson();if(h::isRecord(value)&&rt::getString(value,"state")=="submitted"){persisted=value;break;}}ASSERT(persisted);EXPECT_EQ(rt::getString(*persisted,"provider_receipt_id"),"receipt-completed");EXPECT_EQ(f.provider->commitCount,1);EXPECT_EQ(f.provider->redeemInputs.size(),1u);
  f.clock->advance(CODEX_BANKED_RESET_LEASE_MS+1);f.provider->lookupResult=sanitizedProviderFixtures().lookup_completed;f.provider->verifyResult=false;AWAIT(recovery,reconcileCodexBankedReset(candidate({.requestId="receipt-persisted-recovery"}),deps));EXPECT_EQ(recovery.kind,"pending");EXPECT_EQ(f.provider->redeemInputs.size(),1u);f.provider->verifyResult=true;gate->resolve();const auto stale=co_await original.wait();EXPECT_EQ(stale.kind,"pending");EXPECT_EQ(stale.reason,"verification_cas_failed");EXPECT_EQ(f.provider->redeemInputs.size(),1u);
}

ASYNC_TEST("client aborts before submission and after a possible commit fail closed without resubmission") {
  Isolation isolation;

  Fixture beforeF;auto beforeReset=candidate({.requestId="abort-before-submission"});DONE(seedFences(beforeF.kv,beforeReset));const rt::AbortController beforeController;beforeController.abort(rt::makeReason(rt::DOMException("client cancelled","AbortError")));beforeReset.signal=beforeController.signal();AWAIT(before,attemptCodexBankedReset(beforeReset,beforeF.deps()));EXPECT_EQ(before.kind,"rejected");EXPECT_EQ(before.reason,"client_aborted_before_submission");EXPECT_EQ(beforeF.provider->redeemInputs.size(),0u);
  Fixture afterF;auto afterReset=candidate({.requestId="abort-after-possible-commit"});DONE(seedFences(afterF.kv,afterReset));const rt::AbortController afterController;auto gate=std::make_shared<Deferred<>>();auto entered=std::make_shared<Deferred<>>();afterF.provider->redeemEntered=entered;afterF.provider->redeemGate=gate->promise;const auto afterDeps=afterF.deps();afterReset.signal=afterController.signal();rt::Promise<Outcome> original(attemptCodexBankedReset(afterReset,afterDeps));co_await entered->promise.wait();afterController.abort(rt::makeReason(rt::DOMException("client cancelled after submit","AbortError")));gate->resolve();const auto after=co_await original.wait();EXPECT_EQ(after.kind,"pending");EXPECT_EQ(stateOf(after),"submitted");EXPECT_EQ(afterF.provider->redeemInputs.size(),1u);
  afterF.clock->advance(CODEX_BANKED_RESET_LEASE_MS+1);afterF.provider->lookupResult=sanitizedProviderFixtures().lookup_completed;AWAIT(recovered,reconcileCodexBankedReset(candidate({.requestId="abort-after-recovery"}),afterDeps));EXPECT_EQ(recovered.kind,"verified");EXPECT_EQ(afterF.provider->redeemInputs.size(),1u);
}

ASYNC_TEST("a stalled blocked-cohort inventory is bounded before healthy routing resumes") {
  Isolation isolation;

  // AbortSignal.timeout is a static API, so observe its actual deadline instead of replacing the JS property.
  Fixture f;const auto reset=candidate();DONE(seedFences(f.kv,reset));auto entered=std::make_shared<Deferred<>>();std::optional<rt::AbortSignal> observedSignal;
  f.provider->readInventoryReplacement=[&](uos::codex::banked_reset_provider::ResetAccountContext,rt::AbortSignal signal)->rt::Task<uos::codex::banked_reset_provider::ResetInventory>{observedSignal=signal;entered->resolve();auto resolvers=rt::withResolvers<uos::codex::banked_reset_provider::ResetInventory>();rt::EventListener listener([signal,reject=resolvers.reject]{auto reason=signal.reason();if(!rt::isError(reason))reason=rt::makeReason(rt::DOMException("Inventory timed out","TimeoutError"));reject(reason);});signal.addEventListener("abort",listener,{.once=true});return resolvers.promise.wait();};
  const auto pool=singlePool(reset,f.provider);const auto deps=dependencies(f.kv,f.provider,f.clock,h::config({.mode="shadow",.maxPerAccountPerDay=1}));const auto started=rt::performanceNow();rt::Promise<PoolOutcome> pending(evaluateCodexBankedResetPool(pool,deps));co_await entered->promise.wait();ASSERT(observedSignal);EXPECT(!observedSignal->aborted());
  // The production timeout is unreferenced; a referenced keeper lets the fake test loop reach it.
  auto keeper=rt::setTimeout([]{},CODEX_BANKED_RESET_INVENTORY_TIMEOUT_MS+1);const auto result=co_await pending.wait();rt::clearTimeout(keeper);
  EXPECT(observedSignal->aborted());EXPECT_EQ(rt::performanceNow()-started,CODEX_BANKED_RESET_INVENTORY_TIMEOUT_MS);EXPECT_EQ(result.kind,"skipped");EXPECT_EQ(result.reason,"inventory_unavailable");EXPECT_EQ(f.provider->redeemInputs.size(),0u);
}

ASYNC_TEST("full-pool shadow reads each account inventory, selects the earliest exact credit, and persists one redacted decision") {
  Isolation isolation;

  Fixture f;const auto first=candidate({.accountId="test-account-a",.routingGeneration=7});const auto second=candidate({.accountId="test-account-b",.routingGeneration=8});DONE(seedFences(f.kv,first));DONE(seedFences(f.kv,second));auto secondProvider=std::make_shared<FakeCodexUsageResetProvider>();f.provider->inventory=inventory("credit-a-later",f.clock->nowMs+40'000);secondProvider->inventory=inventory("credit-b-earlier",f.clock->nowMs+20'000);std::vector<std::string> events;uos::codex::banked_reset::CodexBankedResetTelemetry telemetry;telemetry.event=[&](std::string event,json::object){events.push_back(event);};
  AWAIT(result,evaluateCodexBankedResetPool(fullPool(first,f.provider,second,secondProvider),dependencies(f.kv,f.provider,f.clock,h::config({.mode="shadow",.maxPerAccountPerDay=1}),telemetry)));EXPECT_EQ(result.kind,"shadow");EXPECT_EQ(result.reason,"shadow_selected");ASSERT(result.selected);EXPECT_EQ(result.selected->slot,1);EXPECT_EQ(f.provider->inventoryInputs.size(),1u);EXPECT_EQ(secondProvider->inventoryInputs.size(),1u);EXPECT_EQ(f.provider->redeemInputs.size(),0u);EXPECT_EQ(secondProvider->redeemInputs.size(),0u);EXPECT(std::find(events.begin(),events.end(),"codex_reset_shadow_candidate")!=events.end());
  const auto decision=shadowDecisionFrom(*f.kv);EXPECT_EQ(decision.decision_reason,"selected");AWAIT(hash,testHash(std::string("test-account-b")));EXPECT_EQ(decision.selected_account_id_hash,hash);EXPECT(decision.selected_credit_id_hash!="credit-b-earlier");EXPECT_EQ(decision.selected_credit_expires_at_ms,f.clock->nowMs+20'000);const auto serialized=rt::jsonStringify(json::value_from(decision));EXPECT_EQ(serialized.find("test-account-a"),std::string::npos);EXPECT_EQ(serialized.find("credit-b-earlier"),std::string::npos);
}

ASYNC_TEST("a new persistent-live episode consumes exactly once without an arming step") {
  Isolation isolation;

  Fixture f;const auto reset=candidate({.accountId="test-account-a",.routingGeneration=7});DONE(seedFences(f.kv,reset));f.provider->inventory=inventory("expiring-credit",f.clock->nowMs+20'000);const auto pool=singlePool(reset,f.provider);std::vector<std::string> events;uos::codex::banked_reset::CodexBankedResetTelemetry telemetry;telemetry.event=[&](std::string event,json::object){events.push_back(event);};const auto deps=dependencies(f.kv,f.provider,f.clock,h::config({.mode="live",.maxPerAccountPerDay=1}),telemetry);
  AWAIT(consumed,evaluateCodexBankedResetPool(pool,deps));EXPECT_EQ(consumed.kind,"verified");ASSERT(consumed.selected);EXPECT_EQ(consumed.selected->slot,0);EXPECT_EQ(f.provider->inventoryInputs.size(),1u);ASSERT_EQ(f.provider->redeemInputs.size(),1u);EXPECT_EQ(f.provider->redeemInputs[0].creditId,"expiring-credit");EXPECT_EQ(f.provider->commitCount,1);EXPECT(std::find(events.begin(),events.end(),"codex_reset_eligible")!=events.end());AWAIT(daily,dailyRow(f.kv,reset.accountId,f.clock->nowMs));EXPECT_EQ(daily.value["submission_count"].asNumber(),1);
  AWAIT(repeated,evaluateCodexBankedResetPool(pool,deps));EXPECT_EQ(repeated.kind,"verified");EXPECT_EQ(f.provider->redeemInputs.size(),1u);EXPECT_EQ(f.provider->commitCount,1);
}

ASYNC_TEST("concurrent initial persistent-live evaluations produce one redemption") {
  Isolation isolation;

  Fixture f;const auto reset=candidate({.accountId="test-account-a",.routingGeneration=7});DONE(seedFences(f.kv,reset));f.provider->inventory=inventory("expiring-credit",f.clock->nowMs+20'000);auto entered=std::make_shared<Deferred<>>();auto gate=std::make_shared<Deferred<>>();int entrances=0;
  f.provider->readInventoryReplacement=[&](uos::codex::banked_reset_provider::ResetAccountContext input,rt::AbortSignal signal){++entrances;if(entrances==2)entered->resolve();return rt::async([&,input,signal]()->rt::Task<uos::codex::banked_reset_provider::ResetInventory>{co_await gate->promise.wait();auto replacement=f.provider->readInventoryReplacement;f.provider->readInventoryReplacement={};rt::Promise<uos::codex::banked_reset_provider::ResetInventory> reading(f.provider->readInventory(input,signal));f.provider->readInventoryReplacement=replacement;auto result=co_await reading.wait();co_await rt::microtask();co_return result;});};
  const auto pool=singlePool(reset,f.provider);const auto deps=dependencies(f.kv,f.provider,f.clock,h::config({.mode="live",.maxPerAccountPerDay=1}));rt::Promise<PoolOutcome> first(evaluateCodexBankedResetPool(pool,deps));rt::Promise<PoolOutcome> second(evaluateCodexBankedResetPool(pool,deps));co_await entered->promise.wait();EXPECT_EQ(f.provider->redeemInputs.size(),0u);gate->resolve();const auto initial=co_await rt::all(first,second).wait();std::vector<std::string> kinds{std::get<0>(initial).kind,std::get<1>(initial).kind};std::sort(kinds.begin(),kinds.end());EXPECT_EQ(kinds,std::vector<std::string>({"pending","verified"}));EXPECT_EQ(f.provider->redeemInputs.size(),1u);EXPECT_EQ(f.provider->commitCount,1);AWAIT(daily,dailyRow(f.kv,reset.accountId,f.clock->nowMs));EXPECT_EQ(daily.value["submission_count"].asNumber(),1);
  AWAIT(settled,evaluateCodexBankedResetPool(pool,deps));EXPECT_EQ(settled.kind,"verified");EXPECT_EQ(f.provider->redeemInputs.size(),1u);EXPECT_EQ(f.provider->commitCount,1);
}

ASYNC_TEST("invalid or ineligible live inventory cannot consume") {
  Isolation isolation;

  const std::vector<std::string> cases{"invalid","ineligible"};for(const auto& name:cases){Fixture f;const auto reset=candidate({.accountId="test-account-a",.routingGeneration=7});DONE(seedFences(f.kv,reset));f.provider->inventory={.availableCount=name=="invalid"?2.0:1.0,.observedAtMs=1'700'000'000'000,.credits={{.id=name=="invalid"?"incomplete-credit":"unsupported-credit",.status="available",.resetType=name=="invalid"?"codex_rate_limits":"unsupported_reset",.expiresAtMs=std::nullopt}}};std::vector<std::string> events;uos::codex::banked_reset::CodexBankedResetTelemetry telemetry;telemetry.event=[&](std::string event,json::object){events.push_back(event);};
    AWAIT(result,evaluateCodexBankedResetPool(singlePool(reset,f.provider),dependencies(f.kv,f.provider,f.clock,h::config({.mode="live",.maxPerAccountPerDay=1}),telemetry)));EXPECT_EQ(result.kind,"skipped");EXPECT_EQ(result.reason,name=="invalid"?"inventory_response_invalid_or_expired":"inventory_no_eligible_codex_credit");EXPECT_EQ(f.provider->inventoryInputs.size(),1u);EXPECT_EQ(f.provider->redeemInputs.size(),0u);EXPECT_EQ(f.provider->commitCount,0);EXPECT(std::find(events.begin(),events.end(),"codex_reset_shadow_candidate")==events.end());EXPECT_EQ(f.kv->entries.size(),2u);
  }
}

ASYNC_TEST("sequential shadow duplicates skip inventory only after current strong fences pass") {
  Isolation isolation;

  Fixture f;const auto first=candidate({.accountId="test-account-a",.routingGeneration=7});const auto second=candidate({.accountId="test-account-b",.routingGeneration=8});DONE(seedFences(f.kv,first));DONE(seedFences(f.kv,second));auto secondProvider=std::make_shared<FakeCodexUsageResetProvider>();f.provider->inventory=inventory("credit-a",f.clock->nowMs+40'000);secondProvider->inventory=inventory("credit-b",f.clock->nowMs+20'000);const auto pool=fullPool(first,f.provider,second,secondProvider);const auto deps=dependencies(f.kv,f.provider,f.clock,h::config({.mode="shadow",.maxPerAccountPerDay=1}));
  AWAIT(initial,evaluateCodexBankedResetPool(pool,deps));EXPECT_EQ(initial.reason,"shadow_selected");EXPECT_EQ(f.provider->inventoryInputs.size(),1u);EXPECT_EQ(secondProvider->inventoryInputs.size(),1u);AWAIT(duplicate,evaluateCodexBankedResetPool(pool,deps));EXPECT_EQ(duplicate.reason,"already_would_spend_once");EXPECT_EQ(f.provider->inventoryInputs.size(),1u);EXPECT_EQ(secondProvider->inventoryInputs.size(),1u);
  const json::object row{{"kind","routing"},{"routing_generation",rt::makeNumber(second.routingGeneration+1)}};DONE(f.kv->set(routingFenceKey(second.accountId),rt::KvValue(row)));AWAIT(stale,evaluateCodexBankedResetPool(pool,deps));EXPECT_EQ(stale.kind,"skipped");EXPECT_EQ(stale.reason,"routing_fence_stale");EXPECT_EQ(f.provider->inventoryInputs.size(),1u);EXPECT_EQ(secondProvider->inventoryInputs.size(),1u);
}

ASYNC_TEST("an unreadable settings record on the shadow duplicate path returns configuration_unavailable") {
  Isolation isolation;

  Fixture f;const auto reset=candidate({.accountId="test-account-a",.routingGeneration=7});DONE(seedFences(f.kv,reset));f.provider->inventory=inventory("credit-a",f.clock->nowMs+40'000);const auto pool=singlePool(reset,f.provider);const auto deps=dependencies(f.kv,f.provider,f.clock,h::config({.mode="shadow",.maxPerAccountPerDay=1}));AWAIT(initial,evaluateCodexBankedResetPool(pool,deps));EXPECT_EQ(initial.reason,"shadow_selected");EXPECT_EQ(f.provider->inventoryInputs.size(),1u);
  AWAIT(hash,testHash(reset.accountId));const auto settingsKey=encodeKey(codexResetUsageKey(hash));int settingsReads=0;f.kv->beforeGet=[&](rt::KvKey key,double)->std::optional<rt::Promise<void>>{if(encodeKey(key)!=settingsKey)return std::nullopt;++settingsReads;if(settingsReads>1)throw rt::JsError("settings read failed");return std::nullopt;};
  AWAIT(duplicate,evaluateCodexBankedResetPool(pool,deps));EXPECT_EQ(duplicate.kind,"skipped");EXPECT_EQ(duplicate.reason,"configuration_unavailable");EXPECT(!duplicate.selected);EXPECT(!duplicate.reset);EXPECT_EQ(settingsReads,2);EXPECT_EQ(f.provider->inventoryInputs.size(),1u);EXPECT_EQ(f.provider->redeemInputs.size(),0u);EXPECT_EQ(f.provider->commitCount,0);EXPECT_EQ(shadowDecisionFrom(*f.kv).selected_account_id_hash,hash);
}

ASYNC_TEST("a subscription disabled after its shadow decision stays fenced on the duplicate path") {
  Isolation isolation;

  Fixture f;const auto reset=candidate({.accountId="test-account-a",.routingGeneration=7});DONE(seedFences(f.kv,reset));f.provider->inventory=inventory("credit-a",f.clock->nowMs+40'000);const auto pool=singlePool(reset,f.provider);const auto deps=dependencies(f.kv,f.provider,f.clock,h::config({.mode="shadow",.maxPerAccountPerDay=1}));AWAIT(initial,evaluateCodexBankedResetPool(pool,deps));EXPECT_EQ(initial.reason,"shadow_selected");EXPECT_EQ(f.provider->inventoryInputs.size(),1u);
  AWAIT(hash,testHash(reset.accountId));const auto settingsKey=codexResetUsageKey(hash);const auto encodedSettingsKey=encodeKey(settingsKey);int settingsReads=0;
  f.kv->beforeGet=[&](rt::KvKey key,double)->std::optional<rt::Promise<void>>{return rt::Promise<void>(rt::async([&,key]()->rt::Task<void>{if(encodeKey(key)!=encodedSettingsKey)co_return;++settingsReads;if(settingsReads>1){const json::object row{{"enabled",false}};rt::Promise<rt::KvCommitResult> setting(f.kv->set(settingsKey,rt::KvValue(row)));(void)co_await setting.wait();}}));};
  AWAIT(duplicate,evaluateCodexBankedResetPool(pool,deps));EXPECT_EQ(duplicate.kind,"skipped");EXPECT_EQ(duplicate.reason,"usage_disabled");EXPECT(!duplicate.selected);EXPECT(!duplicate.reset);EXPECT_EQ(settingsReads,2);EXPECT_EQ(f.provider->inventoryInputs.size(),1u);EXPECT_EQ(f.provider->redeemInputs.size(),0u);EXPECT_EQ(f.provider->commitCount,0);EXPECT_EQ(shadowDecisionFrom(*f.kv).selected_account_id_hash,hash);
}

ASYNC_TEST("concurrent shadow observations deduplicate one episode, while live uses fresh inventory") {
  Isolation isolation;

  Fixture f;const auto first=candidate({.accountId="test-account-a",.routingGeneration=7});const auto second=candidate({.accountId="test-account-b",.routingGeneration=8});DONE(seedFences(f.kv,first));DONE(seedFences(f.kv,second));auto secondProvider=std::make_shared<FakeCodexUsageResetProvider>();f.provider->inventory=inventory("credit-a",f.clock->nowMs+40'000);secondProvider->inventory=inventory("credit-b",f.clock->nowMs+20'000);const auto pool=fullPool(first,f.provider,second,secondProvider);std::vector<std::string> telemetryEvents;uos::codex::banked_reset::CodexBankedResetTelemetry telemetry;telemetry.event=[&](std::string event,json::object){telemetryEvents.push_back(event);};const auto deps=dependencies(f.kv,f.provider,f.clock,h::config({.mode="shadow",.maxPerAccountPerDay=1}),telemetry);
  rt::Promise<PoolOutcome> onePromise(evaluateCodexBankedResetPool(pool,deps));rt::Promise<PoolOutcome> twoPromise(evaluateCodexBankedResetPool(pool,deps));const auto both=co_await rt::all(onePromise,twoPromise).wait();const auto& one=std::get<0>(both);const auto& two=std::get<1>(both);EXPECT_EQ(one.kind,"shadow");EXPECT_EQ(two.kind,"shadow");EXPECT(one.reason=="already_would_spend_once"||two.reason=="already_would_spend_once");EXPECT(std::find(telemetryEvents.begin(),telemetryEvents.end(),"codex_reset_duplicate_prevented")!=telemetryEvents.end());EXPECT_EQ(f.provider->redeemInputs.size()+secondProvider->redeemInputs.size(),0u);const auto decision=shadowDecisionFrom(*f.kv);
  const auto live=h::config({.mode="live",.maxPerAccountPerDay=1});AWAIT(liveResult,evaluateCodexBankedResetPool(pool,dependencies(f.kv,f.provider,f.clock,live)));EXPECT_EQ(liveResult.kind,"verified");ASSERT(liveResult.selected);EXPECT_EQ(liveResult.selected->slot,1);EXPECT_EQ(f.provider->redeemInputs.size(),0u);ASSERT_EQ(secondProvider->redeemInputs.size(),1u);EXPECT_EQ(secondProvider->redeemInputs[0].creditId,"credit-b");EXPECT_EQ(secondProvider->commitCount,1);EXPECT_EQ(shadowDecisionFrom(*f.kv).episode_hash,decision.episode_hash);
  AWAIT(repeat,evaluateCodexBankedResetPool(pool,dependencies(f.kv,f.provider,f.clock,live)));EXPECT_EQ(repeat.kind,"verified");EXPECT_EQ(secondProvider->redeemInputs.size(),1u);
}

ASYNC_TEST("an existing shadow decision does not gate one live redemption and one daily-cap increment") {
  Isolation isolation;

  Fixture f;const auto reset=candidate({.accountId="test-account-a",.routingGeneration=7});DONE(seedFences(f.kv,reset));f.provider->inventory=inventory("expiring-credit",f.clock->nowMs+20'000);const auto pool=singlePool(reset,f.provider);
  AWAIT(audited,evaluateCodexBankedResetPool(pool,dependencies(f.kv,f.provider,f.clock,h::config({.mode="shadow",.maxPerAccountPerDay=1}))));EXPECT_EQ(audited.kind,"shadow");EXPECT_EQ(audited.reason,"shadow_selected");ASSERT(audited.selected);EXPECT_EQ(audited.selected->slot,0);
  auto entered=std::make_shared<Deferred<>>();auto gate=std::make_shared<Deferred<>>();f.provider->redeemEntered=entered;f.provider->redeemGate=gate->promise;const auto liveDependencies=dependencies(f.kv,f.provider,f.clock,h::config({.mode="live",.maxPerAccountPerDay=1}));rt::Promise<PoolOutcome> firstLive(evaluateCodexBankedResetPool(pool,liveDependencies));co_await entered->promise.wait();AWAIT(duplicateLive,evaluateCodexBankedResetPool(pool,liveDependencies));EXPECT_EQ(duplicateLive.kind,"pending");EXPECT_EQ(duplicateLive.reason,"transaction_in_progress");EXPECT_EQ(f.provider->redeemInputs.size(),1u);
  gate->resolve();const auto completed=co_await firstLive.wait();EXPECT_EQ(completed.kind,"verified");EXPECT_EQ(f.provider->redeemInputs.size(),1u);EXPECT_EQ(f.provider->commitCount,1);AWAIT(daily,dailyRow(f.kv,reset.accountId,f.clock->nowMs));EXPECT_EQ(daily.value["submission_count"].asNumber(),1);
}

ASYNC_TEST("invalid inventories never consume, and live uses only fresh eligible inventory") {
  Isolation isolation;

  const std::vector<std::string> cases{"incomplete","duplicate","expired","changed"};for(const auto& scenario:cases){Fixture f;const auto first=candidate({.accountId="test-account-a",.routingGeneration=7});const auto second=candidate({.accountId="test-account-b",.routingGeneration=8});DONE(seedFences(f.kv,first));DONE(seedFences(f.kv,second));auto secondProvider=std::make_shared<FakeCodexUsageResetProvider>();f.provider->inventory=inventory("credit-a",f.clock->nowMs+40'000);secondProvider->inventory=inventory("credit-b",f.clock->nowMs+20'000);const auto pool=fullPool(first,f.provider,second,secondProvider);
    if(scenario=="incomplete"||scenario=="duplicate"){secondProvider->inventory={.availableCount=scenario=="incomplete"?2.0:1.0,.observedAtMs=f.clock->nowMs,.credits={{.id="credit-b",.status="available",.resetType="codex_rate_limits",.expiresAtMs=std::nullopt}}};if(scenario=="duplicate")secondProvider->inventory.credits.push_back({.id="credit-b",.status="unavailable",.resetType="codex_rate_limits",.expiresAtMs=std::nullopt});}
    else if(scenario=="expired"){f.provider->inventory=inventory("credit-a",f.clock->nowMs-1);secondProvider->inventory=inventory("credit-b",f.clock->nowMs-1);}else secondProvider->inventory=inventory("credit-b-changed",f.clock->nowMs+20'000);
    AWAIT(result,evaluateCodexBankedResetPool(pool,dependencies(f.kv,f.provider,f.clock,h::config({.mode="live",.maxPerAccountPerDay=1}))));
    if(scenario=="changed"){EXPECT_EQ(result.kind,"verified");ASSERT(result.selected);EXPECT_EQ(result.selected->slot,1);EXPECT_EQ(f.provider->redeemInputs.size(),0u);ASSERT_EQ(secondProvider->redeemInputs.size(),1u);EXPECT_EQ(secondProvider->redeemInputs[0].creditId,"credit-b-changed");EXPECT_EQ(secondProvider->commitCount,1);}else{EXPECT_EQ(result.kind,"skipped");EXPECT_EQ(result.reason,"inventory_response_invalid_or_expired");EXPECT_EQ(f.provider->redeemInputs.size()+secondProvider->redeemInputs.size(),0u);}
  }
}
