// Links: tests/codex_account_routing_part2_test.cpp tests/helpers/codex_account_routing_harness.cpp tests/helpers/counting_kv.cpp src/auth/relay.cpp src/authority/native_codex.cpp
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
// Port of tests/codex-account-routing-part2.test.ts: all 31 original cases; no deferred cases.
#include "tests/helpers/codex_account_routing_harness.hpp"
#include "uos/config.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
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

ASYNC_TEST("conflicting absolute body and header deadlines permanently fail closed") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000, headerResetAtMs = now + 120'000;
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible");
  AWAIT(classified, markCodexQuotaBlocked(accounts(initial)[0], quotaBody(now + 60'000, rt::Date(headerResetAtMs).toUTCString()), now));
  EXPECT_EQ(classified.retryAtMs, headerResetAtMs); EXPECT_EQ(classified.quotaBlockSource, "body_resets_at");
  EXPECT_EQ(classified.resetDeadlineIsStable, false); EXPECT_EQ(classified.resetDeadlineConflict, true);
  EXPECT_EQ(slot(kv).banked_reset_generation_ambiguous, true);
  AWAIT(fence, getCodexQuotaBlockFence(accounts(initial)[0], headerResetAtMs)); EXPECT(!fence);
  resetCodexAccountRoutingForTest(); SELECT(selected, singlePool, now + 1); ASSERT_EQ(kind(selected), "quota_blocked"); EXPECT(blocked(selected).empty());
}

ASYNC_TEST("body resets_at preserves longer relative routing delays without authorizing redemption") {
  const double now = 1'700'000'000'000;
  struct Case { std::string name; std::string retryAfter; double expectedRetryAtMs; bool stable; bool conflict; };
  const std::vector<Case> cases{{"shorter", "30", now + 60'000, true, false}, {"equal", "60", now + 60'000, true, false}, {"longer", "120", now + 120'000, false, true}};
  for (const auto& testCase : cases) {
    AWAIT(classified, readCodex429(quotaBody(now + 60'000, testCase.retryAfter), now));
    EXPECT_EQ(classified.retryAtMs, testCase.expectedRetryAtMs); EXPECT_EQ(classified.quotaBlockSource, "body_resets_at");
    EXPECT_EQ(classified.resetDeadlineIsStable, testCase.stable); EXPECT_EQ(classified.resetDeadlineConflict, testCase.conflict);
  }
}

ASYNC_TEST("valid delta Retry-After durably blocks a fully parsed usage limit") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000;
  const std::string payload = "{\"error\":{\"type\":\"usage_limit_reached\"}}";
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible");
  AWAIT(classified, markCodexQuotaBlocked(accounts(initial)[0], deltaSecondsQuotaResponse("60"), now));
  EXPECT_EQ(classified.usageLimitReached, true); EXPECT_EQ(classified.retryAtMs, now + 60'000);
  AWAIT(text, classified.response.text()); EXPECT_EQ(text, payload);
  const auto raw = kv->row(CODEX_ACCOUNT_ROUTING_KV_KEY); EXPECT_EQ(rt::getNumber(raw, "v"), 2.0);
  const auto& rawSlot = raw.at("slots").as_array()[0]; EXPECT_EQ(rt::getNumber(rawSlot, "quota_blocked_until_ms"), now + 60'000);
  EXPECT_EQ(rt::getString(rawSlot, "quota_block_source"), "header_retry_after");
  resetCodexAccountRoutingForTest(); SELECT(selected, singlePool, now + 1); EXPECT_EQ(kind(selected), "quota_blocked");
}

ASYNC_TEST("relative Retry-After blocks ordinary routing but cannot mint a banked-reset fence") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000;
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible"); const auto account = accounts(initial)[0];
  AWAIT(classified, markCodexQuotaBlocked(account, deltaSecondsQuotaResponse("60"), now));
  EXPECT_EQ(classified.retryAtMs, now + 60'000); EXPECT_EQ(classified.resetDeadlineIsStable, false);
  AWAIT(fence, getCodexQuotaBlockFence(account, now + 60'000)); EXPECT(!fence);
  SELECT(selected, singlePool, now + 1); ASSERT_EQ(kind(selected), "quota_blocked"); EXPECT(blocked(selected).empty());
}

ASYNC_TEST("valid HTTP-date Retry-After durably blocks a fully parsed usage limit") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000, retryAtMs = now + 120'000;
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible");
  AWAIT(classified, markCodexQuotaBlocked(accounts(initial)[0], httpDateQuotaResponse(retryAtMs), now));
  EXPECT_EQ(classified.usageLimitReached, true); EXPECT_EQ(classified.retryAtMs, retryAtMs);
  resetCodexAccountRoutingForTest(); SELECT(selected, singlePool, now + 1); EXPECT_EQ(kind(selected), "quota_blocked");
}

ASYNC_TEST("eligible routing exposes only stable blocked siblings to the banked-reset cohort") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000, retryAtMs = now + 120'000;
  SELECT(initial, pool, now); ASSERT_EQ(kind(initial), "eligible");
  AWAIT(classified, markCodexQuotaBlocked(accounts(initial)[0], httpDateQuotaResponse(retryAtMs), now)); EXPECT_EQ(classified.resetDeadlineIsStable, true);
  resetCodexAccountRoutingForTest(); SELECT(selected, pool, now + 1); ASSERT_EQ(kind(selected), "eligible");
  EXPECT_EQ(slots(selected), (std::vector<double>{1})); EXPECT_EQ(accounts(selected)[0].probeRequired, false);
  const auto cohort = blocked(selected); ASSERT_EQ(cohort.size(), 1u); EXPECT_EQ(cohort[0].slot, 0.0); EXPECT_EQ(cohort[0].quotaResetAtMs, retryAtMs);
}

ASYNC_TEST("quota circuits skip blocked slots and synthesize direct-fallback eligibility") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow();
  SELECT(initial, pool, now); ASSERT_EQ(kind(initial), "eligible"); const auto first = accounts(initial)[0];
  AWAIT(replayable, markCodexQuotaBlocked(first, deltaSecondsQuotaResponse("60"), now)); EXPECT_EQ(replayable.response.status(), 429);
  AWAIT(text, replayable.response.text()); EXPECT(rt::Regex("usage_limit_reached").test(text));
  SELECT(afterOne, pool, now + 1); ASSERT_EQ(kind(afterOne), "eligible");
  std::vector<std::string> afterOneIds; for (const auto& account : accounts(afterOne)) afterOneIds.push_back(account.auth.account_id);
  EXPECT_EQ(afterOneIds, (std::vector<std::string>{"two"})); EXPECT(blocked(afterOne).empty());
  RUN(markCodexQuotaBlocked(accounts(afterOne)[0], deltaSecondsQuotaResponse("60"), now + 1));
  SELECT(allBlocked, pool, now + 2); EXPECT_EQ(kind(allBlocked), "quota_blocked");
  AWAIT(rechecked, recheckCodexRoutingSlot(1)); EXPECT_EQ(rechecked, true);
  SELECT(halfOpen, pool, rt::dateNow()); ASSERT_EQ(kind(halfOpen), "eligible"); ASSERT_EQ(accounts(halfOpen).size(), 1u);
  EXPECT_EQ(accounts(halfOpen)[0].probeRequired, true); EXPECT(blocked(halfOpen).empty());
}

ASYNC_TEST("429 classification reads fragmented JSON and preserves the response") {
  const double now = 1'700'000'000'000; const std::string payload = "{\"error\":{\"type\":\"usage_limit_reached\"}}";
  rt::UnderlyingSource<rt::ByteChunk> source;
  source.start = [payload](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
    const auto bytes = rt::encodeUtf8(payload);
    controller.enqueue(rt::ByteChunk(bytes.begin(), bytes.begin() + 17)); controller.enqueue(rt::ByteChunk(bytes.begin() + 17, bytes.end())); controller.close();
  };
  rt::ByteStream stream(source); rt::ResponseInit init; init.status = 429; init.headers = rt::Headers{{"Content-Type", "application/json"}, {"Retry-After", "1"}};
  rt::Response response(rt::BodyInit(stream), init);
  AWAIT(parsed, readCodex429(response, now)); EXPECT_EQ(parsed.usageLimitReached, true); EXPECT_EQ(parsed.retryAtMs, now + 1'000);
  AWAIT(text, parsed.response.text()); EXPECT_EQ(text, payload);
}

ASYNC_TEST("429 classification rejects malformed UTF-8 before recognizing a usage-limit body") {
  const double now = 1'700'000'000'000;
  auto bytes = rt::encodeUtf8("{\"error\":{\"type\":\"usage_limit_reached\",\"message\":\""); const auto suffix = rt::encodeUtf8("\"}}");
  bytes.push_back(0x80); bytes.insert(bytes.end(), suffix.begin(), suffix.end());
  rt::ResponseInit init; init.status = 429; init.headers = rt::Headers{{"Content-Type", "application/json"}, {"Retry-After", rt::Date(now + 60'000).toUTCString()}};
  rt::Response response(rt::BodyInit(bytes), init);
  AWAIT(parsed, readCodex429(response, now)); EXPECT_EQ(parsed.usageLimitReached, false); EXPECT_EQ(parsed.retryAtMs, now + 60'000);
  AWAIT(readBack, parsed.response.arrayBuffer()); EXPECT_EQ(readBack, bytes);
}

ASYNC_TEST("429 classification rejects duplicate error keys before JSON last-key resolution") {
  const double now = 1'700'000'000'000; const std::string payload = "{\"error\":{\"type\":\"rate_limit_error\",\"type\":\"usage_limit_reached\"}}";
  rt::ResponseInit init; init.status = 429; init.headers = rt::Headers{{"Content-Type", "application/json"}, {"Retry-After", rt::Date(now + 60'000).toUTCString()}};
  rt::Response response(rt::BodyInit(payload), init);
  AWAIT(parsed, readCodex429(response, now)); EXPECT_EQ(parsed.usageLimitReached, false); AWAIT(text, parsed.response.text()); EXPECT_EQ(text, payload);
}

ASYNC_TEST("429 classification returns a valid error when capture is truncated") {
  const auto response = json429(json::object{{"error", json::object{{"type", "usage_limit_reached"}, {"detail", std::string(70 * 1'024, 'x')}}}});
  AWAIT(parsed, readCodex429(response)); EXPECT_EQ(parsed.usageLimitReached, false); EXPECT(!parsed.retryAtMs);
  AWAIT(body, parsed.response.json());
  const json::value expected = json::object{{"error", json::object{{"message", "Codex returned an oversized or incomplete rate-limit response."},
    {"type", "rate_limit_error"}, {"code", "codex_rate_limit_response_truncated"}, {"param", nullptr}}}};
  EXPECT(rt::deepEqual(body, expected));
}

ASYNC_TEST("ordinary 429 clears only the current fenced probe from an expired circuit") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000;
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible"); RUN(markCodexQuotaBlocked(accounts(initial)[0], deltaSecondsQuotaResponse("1"), now));
  resetCodexAccountRoutingForTest(); SELECT(firstSelection, singlePool, now + 1'001); ASSERT_EQ(kind(firstSelection), "eligible");
  AWAIT(staleProbe, claimCodexRoutingProbe(singlePool, accounts(firstSelection)[0], now + 1'001)); ASSERT(staleProbe);
  resetCodexAccountRoutingForTest(); SELECT(secondSelection, singlePool, now + 1'001 + 30'001); ASSERT_EQ(kind(secondSelection), "eligible");
  AWAIT(currentProbe, claimCodexRoutingProbe(singlePool, accounts(secondSelection)[0], now + 1'001 + 30'001)); ASSERT(currentProbe);
  EXPECT(staleProbe->probeToken != currentProbe->probeToken);
  RUN(markCodexQuotaBlocked(*staleProbe, json429(json::object{{"error", json::object{{"type", "rate_limit_error"}}}}), now + 1'001 + 30'002));
  resetCodexAccountRoutingForTest(); SELECT(afterStale, singlePool, now + 1'001 + 30'002); EXPECT_EQ(kind(afterStale), "quota_blocked");
  RUN(markCodexQuotaBlocked(*currentProbe, json429(json::object{{"error", json::object{{"type", "rate_limit_error"}}}}), now + 1'001 + 30'003));
  resetCodexAccountRoutingForTest(); SELECT(released, singlePool, now + 1'001 + 30'003); ASSERT_EQ(kind(released), "eligible"); EXPECT(!accounts(released)[0].probeToken);
}

ASYNC_TEST("a stale ordinary usage-limit 429 cannot overwrite a foreign half-open lease") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000;
  SELECT(staleSelection, singlePool, now); ASSERT_EQ(kind(staleSelection), "eligible"); const auto staleAccount = accounts(staleSelection)[0]; EXPECT(!staleAccount.probeGeneration);
  resetCodexAccountRoutingForTest(); SELECT(blockingSelection, singlePool, now); ASSERT_EQ(kind(blockingSelection), "eligible");
  RUN(markCodexQuotaBlocked(accounts(blockingSelection)[0], deltaSecondsQuotaResponse("1"), now));
  const double probeAt = now + 1'001; resetCodexAccountRoutingForTest(); SELECT(probeSelection, singlePool, probeAt); ASSERT_EQ(kind(probeSelection), "eligible");
  AWAIT(currentProbe, claimCodexRoutingProbe(singlePool, accounts(probeSelection)[0], probeAt)); ASSERT(currentProbe);
  const auto beforeStale429 = slot(kv); ASSERT(beforeStale429.probe_lease); EXPECT_EQ(beforeStale429.probe_lease->token, *currentProbe->probeToken);
  resetCodexAccountRoutingForTest(); const double stale429At = probeAt + 1;
  RUN(markCodexQuotaBlocked(staleAccount, deltaSecondsQuotaResponse("1"), stale429At));
  EXPECT(rt::deepEqual(routing::toJson(slot(kv)), routing::toJson(beforeStale429)));
  resetCodexAccountRoutingForTest(); SELECT(secondSelection, singlePool, stale429At + 1'001);
  std::optional<RoutingAccount> secondProbe;
  if (kind(secondSelection) == "eligible") {
    AWAIT(candidate, claimCodexRoutingProbe(singlePool, accounts(secondSelection)[0], stale429At + 1'001)); secondProbe = candidate;
  }
  EXPECT(!secondProbe); EXPECT_EQ(kind(secondSelection), "quota_blocked");
}

ASYNC_TEST("expired circuits grant one fenced probe and reject stale completion") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow();
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible"); RUN(markCodexQuotaBlocked(accounts(initial)[0], deltaSecondsQuotaResponse("60"), now));
  const double expiry = now + 61'000; resetCodexAccountRoutingForTest();
  std::vector<rt::Promise<std::optional<RoutingAccount>>> pendingProbes;
  for (int index = 0; index < 50; ++index) {
    auto attempt = [expiry]() -> rt::Task<std::optional<RoutingAccount>> {
      SELECT(selection, singlePool, expiry);
      if (kind(selection) != "eligible") co_return std::nullopt;
      AWAIT(probe, claimCodexRoutingProbe(singlePool, accounts(selection)[0], expiry)); co_return probe;
    };
    pendingProbes.emplace_back(rt::async(attempt));
  }
  auto allPending = rt::all(std::move(pendingProbes)); const auto results = co_await allPending.wait();
  std::vector<RoutingAccount> probes; for (const auto& result : results) if (result) probes.push_back(*result);
  ASSERT_EQ(probes.size(), 1u); const auto firstProbe = probes[0];
  resetCodexAccountRoutingForTest(); SELECT(second, singlePool, expiry + 30'001); ASSERT_EQ(kind(second), "eligible");
  AWAIT(secondProbe, claimCodexRoutingProbe(singlePool, accounts(second)[0], expiry + 30'001)); ASSERT(secondProbe); EXPECT(firstProbe.probeToken != secondProbe->probeToken);
  RUN(markCodexSuccess(firstProbe)); resetCodexAccountRoutingForTest(); SELECT(afterStaleSuccess, singlePool, expiry + 30'002); EXPECT_EQ(kind(afterStaleSuccess), "quota_blocked");
  RUN(markCodexSuccess(*secondProbe)); resetCodexAccountRoutingForTest(); SELECT(recovered, singlePool, expiry + 30'003); EXPECT_EQ(kind(recovered), "eligible");
}

ASYNC_TEST("an expired circuit receives one half-open probe before a healthy sibling") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow();
  SELECT(initial, pool, now); ASSERT_EQ(kind(initial), "eligible");
  const auto found = std::find_if(accounts(initial).begin(), accounts(initial).end(), [](const auto& account) { return account.auth.account_id == "one"; });
  ASSERT(found != accounts(initial).end()); RUN(markCodexQuotaBlocked(*found, deltaSecondsQuotaResponse("60"), now));
  SELECT(selection, pool, now + 61'000); ASSERT_EQ(kind(selection), "eligible"); ASSERT_EQ(accounts(selection).size(), 2u);
  EXPECT_EQ(accounts(selection)[0].auth.account_id, "one"); EXPECT_EQ(accounts(selection)[0].probeRequired, true);
  AWAIT(probe, claimCodexRoutingProbe(pool, accounts(selection)[0], now + 61'000)); ASSERT(probe); EXPECT(probe->probeToken);
  EXPECT_EQ(accounts(selection)[1].auth.account_id, "two");
}

ASYNC_TEST("an expired second-account circuit never jumps the healthy first account") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow();
  SELECT(initial, pool, now); ASSERT_EQ(kind(initial), "eligible");
  const auto found = std::find_if(accounts(initial).begin(), accounts(initial).end(), [](const auto& account) { return account.auth.account_id == "two"; });
  ASSERT(found != accounts(initial).end()); RUN(markCodexQuotaBlocked(*found, deltaSecondsQuotaResponse("60"), now));
  SELECT(selection, pool, now + 61'000); ASSERT_EQ(kind(selection), "eligible");
  std::vector<std::string> ids; for (const auto& account : accounts(selection)) ids.push_back(account.auth.account_id);
  EXPECT_EQ(ids, (std::vector<std::string>{"one", "two"}));
  EXPECT(!accounts(selection)[0].probeToken); EXPECT_EQ(accounts(selection)[0].probeRequired, false);
  EXPECT_EQ(accounts(selection)[1].probeRequired, true); EXPECT(!accounts(selection)[1].probeToken); EXPECT(!slot(kv, 1).probe_lease);
  AWAIT(claimed, claimCodexRoutingProbe(pool, accounts(selection)[1], now + 61'000)); ASSERT(claimed); EXPECT(claimed->probeToken);
}

ASYNC_TEST("unchanged auth reconciliation preserves a single-account half-open success fence") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow();
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible"); RUN(markCodexQuotaBlocked(accounts(initial)[0], deltaSecondsQuotaResponse("60"), now));
  SELECT(selection, singlePool, now + 61'000); ASSERT_EQ(kind(selection), "eligible");
  AWAIT(probe, claimCodexRoutingProbe(singlePool, accounts(selection)[0], now + 61'000)); ASSERT(probe); EXPECT(probe->probeGeneration); EXPECT(probe->probeToken);
  AWAIT(reconciled, reconcileCodexRoutingAccount(*probe, probe->auth));
  EXPECT_EQ(reconciled.probeGeneration, probe->probeGeneration); EXPECT_EQ(reconciled.probeToken, probe->probeToken); RUN(markCodexSuccess(reconciled));
  resetCodexAccountRoutingForTest(); SELECT(recovered, singlePool, now + 61'001); ASSERT_EQ(kind(recovered), "eligible");
  EXPECT(!accounts(recovered)[0].probeGeneration); EXPECT(!accounts(recovered)[0].probeToken);
}

ASYNC_TEST("credential rotation clears only the matching invalid circuit state") {
  Fixture fixture; auto kv = fixture.kv;
  AWAIT(initial, selectCodexRoutingAccounts(singlePool, singlePool.accounts)); ASSERT_EQ(kind(initial), "eligible");
  RUN(markCodexCredentialInvalid(accounts(initial)[0]));
  AWAIT(invalid, selectCodexRoutingAccounts(singlePool, singlePool.accounts)); EXPECT_EQ(kind(invalid), "credentials_invalid");
  auto rotated = singlePool; rotated.accounts[0].access_token = "rotated-access"; rotated.accounts[0].updated_at_ms = rt::dateNow(); rotated.updated_at_ms = rt::dateNow();
  resetCodexAccountRoutingForTest(); AWAIT(selected, selectCodexRoutingAccounts(rotated, rotated.accounts)); EXPECT_EQ(kind(selected), "eligible");
}

ASYNC_TEST("fresh capacity cannot reopen a credential marked invalid") {
  Fixture fixture; auto kv = fixture.kv; const double observedAtMs = rt::dateNow();
  SELECT(initial, singlePool, observedAtMs, "gpt-5.6-luna"); ASSERT_EQ(kind(initial), "eligible"); RUN(markCodexCredentialInvalid(accounts(initial)[0]));
  auto input = observation(observedAtMs, 50, observedAtMs + 604'800'000);
  RUN(recordCodexCapacityRoutingObservations(std::vector<CodexCapacityRoutingObservationInput>{input}, observedAtMs));
  SELECT(selected, singlePool, observedAtMs + 1, "gpt-5.6-luna"); EXPECT_EQ(kind(selected), "credentials_invalid");
}

ASYNC_TEST("same-account credential rotation discards a legacy timeout circuit") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000;
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible"); RUN(markCodexUpstreamTimeout(accounts(initial)[0], now));
  auto rotated = singlePool; rotated.accounts[0].access_token = "rotated-access"; rotated.accounts[0].updated_at_ms = now + 1; rotated.updated_at_ms = now + 1;
  resetCodexAccountRoutingForTest(); SELECT(selected, rotated, now + 1); ASSERT_EQ(kind(selected), "eligible"); EXPECT_EQ(accounts(selected)[0].probeRequired, false);
}

ASYNC_TEST("credential refresh does not transfer a legacy timeout probe") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000;
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible");
  RUN(markCodexUpstreamTimeout(accounts(initial)[0], now - CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS - 1)); resetCodexAccountRoutingForTest();
  SELECT(eligible, singlePool, now); ASSERT_EQ(kind(eligible), "eligible"); EXPECT_EQ(accounts(eligible)[0].probeRequired, false);
  auto rotated = singlePool.accounts[0]; rotated.access_token = "rotated-access"; rotated.updated_at_ms = now + 1;
  AWAIT(reconciled, reconcileCodexRoutingAccount(accounts(eligible)[0], rotated));
  EXPECT_EQ(reconciled.probeCircuit, nullptr); EXPECT(!reconciled.probeToken); EXPECT(!reconciled.probeGeneration);
  const auto transferred = slot(kv); EXPECT_EQ(transferred.upstream_timeout_blocked_until_ms, nullptr); EXPECT(!transferred.probe_lease);
  RUN(markCodexSuccess(reconciled)); const auto recovered = slot(kv); EXPECT_EQ(recovered.upstream_timeout_blocked_until_ms, nullptr); EXPECT(!recovered.probe_lease);
}

ASYNC_TEST("a stale refresh reconciliation cannot overwrite a newer credential version") {
  Fixture fixture; auto kv = fixture.kv;
  AWAIT(initial, selectCodexRoutingAccounts(singlePool, singlePool.accounts)); ASSERT_EQ(kind(initial), "eligible"); const auto accountA = accounts(initial)[0];
  auto authB = accountA.auth; authB.access_token = "refresh-b"; authB.updated_at_ms = 2;
  auto authC = accountA.auth; authC.access_token = "rotation-c"; authC.updated_at_ms = 3;
  AWAIT(credentialC, codexCredentialVersion(authC));
  const json::object legacy{{"credential_version", credentialC}, {"quota_blocked_until_ms", nullptr}, {"quota_block_source", nullptr},
    {"invalid_credential_version", credentialC}, {"primary_used_percent", nullptr}, {"secondary_used_percent", nullptr},
    {"observed_reset_at_ms", nullptr}, {"generation", 7}, {"probe_lease", nullptr}};
  const json::value stateC = json::object{{"v", 2}, {"updated_at_ms", rt::dateNow()}, {"slots", json::array{legacy}}};
  RUN(kv->set(CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvValue(stateC)));
  AWAIT(reconciled, reconcileCodexRoutingAccount(accountA, authB)); AWAIT(credentialB, codexCredentialVersion(authB)); EXPECT_EQ(reconciled.credentialVersion, credentialB);
  const auto durable = slot(kv); EXPECT_EQ(durable.credential_version, credentialC); EXPECT_EQ(durable.invalid_credential_version, credentialC); EXPECT_EQ(durable.generation, 7.0);
}

ASYNC_TEST("a revised active stable Retry-After fails closed rather than minting a second reset identity") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000;
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible"); const auto account = accounts(initial)[0];
  const double firstResetAtMs = now + 60'000; RUN(markCodexQuotaBlocked(account, httpDateQuotaResponse(firstResetAtMs), now));
  AWAIT(firstFence, getCodexQuotaBlockFence(account, firstResetAtMs)); ASSERT(firstFence);
  // A changed stable HTTP-date is not a provider-proven new quota generation.
  const double latestResetAtMs = now + 120'000; RUN(markCodexQuotaBlocked(account, httpDateQuotaResponse(latestResetAtMs), now + 1));
  AWAIT(oldFence, getCodexQuotaBlockFence(account, firstResetAtMs)); EXPECT(!oldFence);
  AWAIT(latestFence, getCodexQuotaBlockFence(account, latestResetAtMs)); EXPECT(!latestFence);
  const auto revised = slot(kv); EXPECT_EQ(revised.quota_blocked_until_ms, latestResetAtMs); EXPECT_EQ(revised.observed_reset_at_ms, firstResetAtMs); EXPECT_EQ(revised.banked_reset_generation_ambiguous, true);
  SELECT(selection, singlePool, now + 2); ASSERT_EQ(kind(selection), "quota_blocked"); const auto cohort = blocked(selection); ASSERT_EQ(cohort.size(), 1u);
  EXPECT_EQ(cohort[0].quotaResetAtMs, firstResetAtMs); EXPECT_EQ(cohort[0].routingGeneration, revised.generation);
  AWAIT(staleReset, reconcileCodexQuotaAfterVerifiedReset(account, CodexQuotaResetReconciliationInput{firstResetAtMs, *firstFence})); EXPECT(!staleReset);
  AWAIT(currentReset, reconcileCodexQuotaAfterVerifiedReset(account, CodexQuotaResetReconciliationInput{firstResetAtMs, revised.generation})); EXPECT(!currentReset);
}

ASYNC_TEST("a post-reset recovery probe fences delayed 429s and clears ambiguity only after success") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000;
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible");
  // This request remains in flight while the first qualifying 429 is reconciled.
  const auto account = accounts(initial)[0]; const double firstResetAtMs = now + 60'000;
  RUN(markCodexQuotaBlocked(account, httpDateQuotaResponse(firstResetAtMs), now));
  AWAIT(firstFence, getCodexQuotaBlockFence(account, firstResetAtMs)); ASSERT(firstFence);
  AWAIT(recoveryProbe, reconcileCodexQuotaAfterVerifiedReset(account, CodexQuotaResetReconciliationInput{firstResetAtMs, *firstFence})); ASSERT(recoveryProbe);
  EXPECT_EQ(recoveryProbe->probeGeneration, *firstFence + 1); EXPECT(recoveryProbe->probeToken && !recoveryProbe->probeToken->empty());
  SELECT(released, singlePool, now + 1); ASSERT_EQ(kind(released), "quota_blocked");
  const auto retryAtMs = std::get<routing::RouteSelectionQuotaBlocked>(released).retryAtMs; EXPECT(retryAtMs && *retryAtMs > now + 1);
  const double revisedResetAtMs = now + 120'000; RUN(markCodexQuotaBlocked(account, httpDateQuotaResponse(revisedResetAtMs), now + 1));
  AWAIT(oldFence, getCodexQuotaBlockFence(account, firstResetAtMs)); EXPECT(!oldFence);
  AWAIT(revisedFence, getCodexQuotaBlockFence(account, revisedResetAtMs)); EXPECT(!revisedFence);
  const auto delayed = slot(kv); EXPECT(!delayed.quota_blocked_until_ms); EXPECT_EQ(delayed.observed_reset_at_ms, firstResetAtMs);
  EXPECT_EQ(delayed.observed_reset_at_is_stable, true); EXPECT_EQ(delayed.banked_reset_generation_ambiguous, true);
  ASSERT(delayed.probe_lease); EXPECT_EQ(delayed.probe_lease->token, *recoveryProbe->probeToken);
  RUN(markCodexSuccess(*recoveryProbe)); const auto recovered = slot(kv); EXPECT(!recovered.observed_reset_at_ms); EXPECT_EQ(recovered.banked_reset_generation_ambiguous, false);
  SELECT(postSuccess, singlePool, now + 2); ASSERT_EQ(kind(postSuccess), "eligible");
  const double nextResetAtMs = now + 180'000; RUN(markCodexQuotaBlocked(accounts(postSuccess)[0], httpDateQuotaResponse(nextResetAtMs), now + 2));
  AWAIT(nextFence, getCodexQuotaBlockFence(accounts(postSuccess)[0], nextResetAtMs)); EXPECT(nextFence);
}

ASYNC_TEST("a relative Retry-After after a stable deadline cannot mint a later reset identity") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000;
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible"); const auto account = accounts(initial)[0]; const double firstDeadline = now + 120'000;
  RUN(markCodexQuotaBlocked(account, deltaSecondsQuotaResponse(rt::Date(firstDeadline).toUTCString()), now));
  AWAIT(firstFence, getCodexQuotaBlockFence(account, firstDeadline)); ASSERT(firstFence);
  // A delta timeout may extend routing's block but cannot revise a stable reset identity.
  const double relativeDeadline = now + 180'001; RUN(markCodexQuotaBlocked(account, deltaSecondsQuotaResponse("180"), now + 1));
  AWAIT(oldFence, getCodexQuotaBlockFence(account, firstDeadline)); EXPECT(!oldFence);
  AWAIT(relativeFence, getCodexQuotaBlockFence(account, relativeDeadline)); EXPECT(!relativeFence);
  const double laterDeadline = now + 240'000; RUN(markCodexQuotaBlocked(account, deltaSecondsQuotaResponse(rt::Date(laterDeadline).toUTCString()), now + 2));
  AWAIT(laterFence, getCodexQuotaBlockFence(account, laterDeadline)); EXPECT(!laterFence);
  const auto durable = slot(kv); EXPECT_EQ(durable.quota_blocked_until_ms, laterDeadline); EXPECT_EQ(durable.observed_reset_at_ms, firstDeadline);
  EXPECT_EQ(durable.observed_reset_at_is_stable, true); EXPECT_EQ(durable.banked_reset_generation_ambiguous, true);
}

ASYNC_TEST("only a successful recovery probe clears reset-generation ambiguity") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::Math::floor(rt::dateNow() / 1'000) * 1'000;
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible"); const auto account = accounts(initial)[0];
  const double firstDeadline = now + 60'000, revisedDeadline = now + 120'000;
  RUN(markCodexQuotaBlocked(account, deltaSecondsQuotaResponse(rt::Date(firstDeadline).toUTCString()), now));
  RUN(markCodexQuotaBlocked(account, deltaSecondsQuotaResponse(rt::Date(revisedDeadline).toUTCString()), now + 1));
  // Administrative recheck cannot clear generation ambiguity.
  AWAIT(rechecked, recheckCodexRoutingSlot(1)); EXPECT_EQ(rechecked, true);
  SELECT(afterRecheck, singlePool, rt::dateNow()); ASSERT_EQ(kind(afterRecheck), "eligible"); EXPECT_EQ(accounts(afterRecheck)[0].probeRequired, true);
  AWAIT(firstProbe, claimCodexRoutingProbe(singlePool, accounts(afterRecheck)[0], rt::dateNow())); ASSERT(firstProbe);
  const double thirdNow = rt::Math::floor(rt::dateNow() / 1'000) * 1'000, laterDeadline = thirdNow + 180'000;
  RUN(markCodexQuotaBlocked(*firstProbe, deltaSecondsQuotaResponse(rt::Date(laterDeadline).toUTCString()), thirdNow));
  AWAIT(laterFence, getCodexQuotaBlockFence(*firstProbe, laterDeadline)); EXPECT(!laterFence);
  const auto stillAmbiguous = slot(kv); EXPECT_EQ(stillAmbiguous.observed_reset_at_ms, firstDeadline); EXPECT_EQ(stillAmbiguous.banked_reset_generation_ambiguous, true);
  // Only an actual successful half-open probe resets the provisional identity.
  AWAIT(recheckedAgain, recheckCodexRoutingSlot(1)); EXPECT_EQ(recheckedAgain, true);
  SELECT(afterSecondRecheck, singlePool, rt::dateNow()); ASSERT_EQ(kind(afterSecondRecheck), "eligible");
  AWAIT(successfulProbe, claimCodexRoutingProbe(singlePool, accounts(afterSecondRecheck)[0], rt::dateNow())); ASSERT(successfulProbe);
  RUN(markCodexSuccess(*successfulProbe)); const auto recovered = slot(kv); EXPECT(!recovered.observed_reset_at_ms); EXPECT_EQ(recovered.banked_reset_generation_ambiguous, false);
  SELECT(postSuccess, singlePool, rt::dateNow()); ASSERT_EQ(kind(postSuccess), "eligible");
  const double freshNow = rt::Math::floor(rt::dateNow() / 1'000) * 1'000, freshDeadline = freshNow + 240'000;
  RUN(markCodexQuotaBlocked(accounts(postSuccess)[0], deltaSecondsQuotaResponse(rt::Date(freshDeadline).toUTCString()), freshNow));
  AWAIT(freshFence, getCodexQuotaBlockFence(accounts(postSuccess)[0], freshDeadline)); EXPECT(freshFence);
}

ASYNC_TEST("same-account credential rotation and pool reordering retain reset ambiguity") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000;
  AWAIT(initial, selectCodexRoutingAccounts(pool, std::vector<uos::types::CodexAuthState>{pool.accounts[0]}, now)); ASSERT_EQ(kind(initial), "eligible");
  const auto account = accounts(initial)[0]; const double firstDeadline = now + 60'000, revisedDeadline = now + 120'000;
  RUN(markCodexQuotaBlocked(account, deltaSecondsQuotaResponse(rt::Date(firstDeadline).toUTCString()), now));
  RUN(markCodexQuotaBlocked(account, deltaSecondsQuotaResponse(rt::Date(revisedDeadline).toUTCString()), now + 1));
  auto rotatedAccount = pool.accounts[0]; rotatedAccount.access_token = "rotated-access"; rotatedAccount.updated_at_ms = now + 2;
  CodexAuthPoolState reorderedPool{.accounts = {pool.accounts[1], rotatedAccount}, .updated_at_ms = now + 2};
  // Fresh-isolate normalization sees both the changed order and credential.
  resetCodexAccountRoutingForTest(); SELECT(rotated, reorderedPool, now + 2); ASSERT_EQ(kind(rotated), "eligible");
  std::vector<std::string> ids; for (const auto& candidate : accounts(rotated)) ids.push_back(candidate.auth.account_id);
  EXPECT_EQ(ids, (std::vector<std::string>{"two", "one"}));
  EXPECT(std::all_of(accounts(rotated).begin(), accounts(rotated).end(), [](const auto& candidate) { return !candidate.probeRequired; }));
  const auto found = std::find_if(accounts(rotated).begin(), accounts(rotated).end(), [](const auto& candidate) { return candidate.auth.account_id == "one"; }); ASSERT(found != accounts(rotated).end());
  const double laterDeadline = revisedDeadline + 60'000; RUN(markCodexQuotaBlocked(*found, deltaSecondsQuotaResponse(rt::Date(laterDeadline).toUTCString()), now + 2));
  AWAIT(fence, getCodexQuotaBlockFence(*found, laterDeadline)); EXPECT(!fence);
  const auto durable = slot(kv, 1); EXPECT_EQ(durable.observed_reset_at_ms, firstDeadline); EXPECT_EQ(durable.banked_reset_generation_ambiguous, true);
  AWAIT(credential, codexCredentialVersion(rotatedAccount)); EXPECT_EQ(durable.credential_version, credential);
}

ASYNC_TEST("an unmappable legacy stable identity cannot migrate to a replacement account") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000, legacyDeadline = now + 60'000;
  AWAIT(oldCredentialVersion, codexCredentialVersion(singlePool.accounts[0]));
  const json::object legacy{{"credential_version", oldCredentialVersion}, {"quota_blocked_until_ms", legacyDeadline}, {"quota_block_source", "header_retry_after"},
    {"invalid_credential_version", nullptr}, {"primary_used_percent", nullptr}, {"secondary_used_percent", nullptr}, {"observed_reset_at_ms", legacyDeadline},
    {"observed_reset_at_is_stable", true}, {"banked_reset_generation_ambiguous", true}, {"generation", 1}, {"probe_lease", nullptr}};
  const json::value legacyState = json::object{{"v", 2}, {"updated_at_ms", now}, {"slots", json::array{legacy}}};
  RUN(kv->set(CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvValue(legacyState)));
  uos::types::CodexAuthState replacement{.access_token = "replacement-access", .refresh_token = "replacement-refresh", .account_id = "replacement", .updated_at_ms = now + 1};
  CodexAuthPoolState replacementPool{.accounts = {replacement}, .updated_at_ms = now + 1};
  SELECT(selected, replacementPool, now + 1); EXPECT_EQ(kind(selected), "eligible");
  // The global legacy guard must survive the warm-cache normalization path.
  SELECT(warmSelected, replacementPool, now + 2); ASSERT_EQ(kind(warmSelected), "eligible"); const auto replacementRouting = accounts(warmSelected)[0];
  const double replacementDeadline = now + 120'000; RUN(markCodexQuotaBlocked(replacementRouting, httpDateQuotaResponse(replacementDeadline), now + 1));
  AWAIT(fence, getCodexQuotaBlockFence(replacementRouting, replacementDeadline)); EXPECT(!fence);
  const auto durable = state(kv); ASSERT(durable); EXPECT_EQ(durable->banked_reset_legacy_identity_unresolved, true); EXPECT_EQ(durable->slots[0].observed_reset_at_ms, replacementDeadline);
}

ASYNC_TEST("an administrative recheck fences a stable reset identity until a successful probe") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000;
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible"); const auto account = accounts(initial)[0]; const double deadline = now + 60'000;
  RUN(markCodexQuotaBlocked(account, httpDateQuotaResponse(deadline), now));
  AWAIT(before, getCodexQuotaBlockFence(account, deadline)); EXPECT(before);
  AWAIT(rechecked, recheckCodexRoutingSlot(1)); EXPECT_EQ(rechecked, true);
  AWAIT(after, getCodexQuotaBlockFence(account, deadline)); EXPECT(!after);
  const auto durable = slot(kv); EXPECT_EQ(durable.observed_reset_at_ms, deadline); EXPECT_EQ(durable.banked_reset_generation_ambiguous, true);
}

ASYNC_TEST("fresh Spark capacity reconciles a blocked account without reordering configured accounts") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000;
  SELECT(initial, pool, now, "gpt-5.3-codex-spark"); ASSERT_EQ(kind(initial), "eligible");
  const auto found = std::find_if(accounts(initial).begin(), accounts(initial).end(), [](const auto& account) { return account.auth.account_id == "two"; }); ASSERT(found != accounts(initial).end());
  const double blockedUntil = now + 60'000; RUN(markCodexQuotaBlocked(*found, httpDateQuotaResponse(blockedUntil), now));
  auto input = observation(now + 1, 100, now + 604'800'000); input.slot = 1; input.account_id = "two";
  input.additional_rate_limits = {{"GPT-5.3-Codex-Spark", std::string("codex_bengalfox"), {{routing::CodexCapacityRoutingWindow{18'000, 50, now + 18'000'000}}, std::nullopt}}};
  RUN(recordCodexCapacityRoutingObservations(std::vector<CodexCapacityRoutingObservationInput>{input}, now + 1));
  SELECT(selected, pool, now + 2, "gpt-5.3-codex-spark"); ASSERT_EQ(kind(selected), "eligible");
  // Configured order is retained after fresh capacity reopens the account.
  EXPECT_EQ(accounts(selected)[0].auth.account_id, "one");
  const auto reopened = std::find_if(accounts(selected).begin(), accounts(selected).end(), [](const auto& account) { return account.auth.account_id == "two"; }); ASSERT(reopened != accounts(selected).end());
  EXPECT_EQ(reopened->quotaHeadroom, 50.0); EXPECT(blocked(selected).empty());
  const auto persisted = slot(kv, 1); EXPECT(!persisted.quota_blocked_until_ms); EXPECT_EQ(persisted.primary_used_percent, 100.0); EXPECT_EQ(persisted.capacity_observed_at_ms, now + 1);
  const std::string storedObservation = rt::jsonStringify(kv->row(CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY));
  EXPECT(rt::includes(storedObservation, "account_id")); EXPECT(!rt::includes(storedObservation, "\"two\""));
}

ASYNC_TEST("class capacity freshness ignores a newer independent quota signal") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000, sparkDeadline = now + 60'000, standardDeadline = now + 120'000;
  SELECT(initial, singlePool, now, "gpt-5.3-codex-spark"); ASSERT_EQ(kind(initial), "eligible");
  RUN(markCodexQuotaBlocked(accounts(initial)[0], httpDateQuotaResponse(sparkDeadline), now));
  SELECT(standard, singlePool, now + 1, "gpt-5.6-luna"); ASSERT_EQ(kind(standard), "eligible");
  RUN(markCodexQuotaBlocked(accounts(standard)[0], httpDateQuotaResponse(standardDeadline), now + 1));
  auto input = observation(now + 2, 100, sparkDeadline);
  input.additional_rate_limits = {{"GPT-5.3-Codex-Spark", std::string("codex_bengalfox"), {{routing::CodexCapacityRoutingWindow{604'800, 50, sparkDeadline}}, std::nullopt}}};
  RUN(recordCodexCapacityRoutingObservations(std::vector<CodexCapacityRoutingObservationInput>{input}, now + 2));
  SELECT(sparkAfterCapacity, singlePool, now + 3, "gpt-5.3-codex-spark"); EXPECT_EQ(kind(sparkAfterCapacity), "eligible");
  SELECT(luna, singlePool, now + 3, "gpt-5.6-luna"); EXPECT_EQ(kind(luna), "quota_blocked");
}
