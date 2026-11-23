// Links: tests/codex_account_routing_part3_test.cpp tests/helpers/codex_account_routing_harness.cpp tests/helpers/counting_kv.cpp src/auth/relay.cpp src/authority/native_codex.cpp
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
// Port of tests/codex-account-routing-part3.test.ts; all 23 original cases, no deferred cases.
#include "tests/helpers/codex_account_routing_harness.hpp"
#include "uos/config.hpp"
#include "uos/codex/routing_mutations.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"
#include <algorithm>
#include <functional>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace routing = uos::codex::routing_state;
  using namespace uos::tests::helpers::codex_account_routing_harness;
  using routing::RoutingAccount;
  using routing::RouteSelection;
  using routing::CodexCapacityRoutingObservationInput;
  struct Fixture {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/cpp-c2-routing"}}};
    std::string savedSeed = uos::config::config.codexAuthJsonB64;
    std::shared_ptr<RoutingKv> kv = std::make_shared<RoutingKv>();
    Fixture() {
      uos::config::config.codexAuthJsonB64.clear(); setKvForTest(kv);
      resetCodexAccountRoutingForTest(); resetCodexAuthCacheForTest(); resetProviderSelectionCacheForTest();
    }
    ~Fixture() {
      setKvForTest(nullptr); resetCodexAccountRoutingForTest(); resetCodexAuthCacheForTest(); resetProviderSelectionCacheForTest();
      uos::config::config.codexAuthJsonB64 = savedSeed;
    }
  };
  std::string kind(const RouteSelection& value) { return std::visit([](const auto& item) { return std::string(item.kind); }, value); }
  const std::vector<RoutingAccount>& accounts(const RouteSelection& value) { return std::get<routing::RouteSelectionEligible>(value).accounts; }
  RoutingAccount findAccount(const RouteSelection& selection, const std::string& id) {
    for (const auto& account : accounts(selection)) if (account.auth.account_id == id) return account;
    throw rt::JsError("expected routing account " + id);
  }
  bool hasAccount(const RouteSelection& selection, const std::string& id) {
    return std::any_of(accounts(selection).begin(), accounts(selection).end(), [&](const auto& account) { return account.auth.account_id == id; });
  }
  bool hasBlocked(const RouteSelection& selection, const std::string& id) {
    const auto& blocked = std::get<routing::RouteSelectionEligible>(selection).blockedAccounts;
    return std::any_of(blocked.begin(), blocked.end(), [&](const auto& account) { return account.auth.account_id == id; });
  }
  routing::CodexAccountRoutingState state(const std::shared_ptr<RoutingKv>& kv) {
    return required(parseCodexAccountRoutingState(kv->row(CODEX_ACCOUNT_ROUTING_KV_KEY)), "persisted routing state");
  }
  routing::CodexActiveAccountSelection active(const std::shared_ptr<RoutingKv>& kv) {
    return required(parseCodexActiveAccountSelection(kv->row(CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY)), "active routing state");
  }
  CodexAuthPoolState freshPool(double now, bool single = false) {
    auto out = single ? singlePool : pool;
    out.updated_at_ms = now; for (auto& auth : out.accounts) auth.updated_at_ms = now;
    return out;
  }
  CodexCapacityRoutingObservationInput observation(double slot, const std::string& accountId, double at, double seconds, double used, double reset) {
    CodexCapacityRoutingObservationInput out;
    out.slot = slot; out.account_id = accountId; out.state = "available"; out.source_observed_at_ms = at; out.snapshot_at_ms = at;
    out.windows.primary = routing::CodexCapacityRoutingWindow{seconds, used, reset};
    return out;
  }
  json::object dashboardSource(double slot, double at, bool spark) {
    json::object out{{"source", "codex"}, {"slot", slot}, {"state", "available"}, {"source_observed_at_ms", at}, {"snapshot_at_ms", at},
      {"windows", json::object{{"primary", json::object{{"limit_window_seconds", spark ? 604'800 : 10'800}, {"used_percent", spark ? 100 : 50},
        {"reset_at_ms", at - (spark ? 1 : 0) + (spark ? 604'800'000 : 10'800'000)}}}, {"secondary", nullptr}}},
      {"additional_rate_limits", json::array{}}};
    if (spark) out["additional_rate_limits"] = json::array{json::object{{"limit_name", "GPT-5.3-Codex-Spark"}, {"metered_feature", "codex_bengalfox"},
      {"windows", json::object{{"primary", json::object{{"limit_window_seconds", 18'000}, {"used_percent", 50}, {"reset_at_ms", at - 1 + 18'000'000}}},
                               {"secondary", nullptr}}}}};
    return out;
  }
  rt::Fetch countingUpstreamFetch(std::string marker, std::function<void()> onDispatch) {
    return rt::Fetch([marker, onDispatch](rt::RequestInfo input, rt::RequestInit init) -> rt::Task<rt::Response> {
      const rt::Request request = std::holds_alternative<rt::Request>(input) ? std::get<rt::Request>(input) : rt::Request(requestUrl(input), init);
      std::string body;
      try { auto clone = request.clone(); body = co_await clone.text(); } catch (...) {}
      if (body.find(marker) != std::string::npos) onDispatch();
      rt::ResponseInit responseInit; responseInit.status = 200;
      co_return rt::Response(nullptr, responseInit);
    });
  }
  uos::codex::dispatch::FetchCodexResponsesOptions noRetryWait() {
    uos::codex::dispatch::FetchCodexResponsesOptions options;
    options.retrySleep = [](double) { return rt::fulfilledTask(); }; return options;
  }
  rt::Task<std::vector<RouteSelection>> concurrentStrong(CodexAuthPoolState authPool, double now) {
    std::vector<rt::Promise<RouteSelection>> pending;
    for (int i = 0; i < 6; ++i) pending.emplace_back(selectCodexRoutingAccountsStrong(authPool, authPool.accounts, now));
    auto all = rt::all(std::move(pending)); auto result = co_await all.wait(); co_return result;
  }
}
#define AWAIT(name, expression) auto name##Pending = rt::Promise(expression); auto name = co_await name##Pending.wait()
#define RUN(expression) do { auto pending = rt::Promise(expression); co_await pending.wait(); } while (false)
#define SELECT(name, poolValue, nowValue, ...) AWAIT(name, selectCodexRoutingAccounts(poolValue, (poolValue).accounts, nowValue __VA_OPT__(,) __VA_ARGS__))
#define STRONG(name, poolValue, nowValue, ...) AWAIT(name, selectCodexRoutingAccountsStrong(poolValue, (poolValue).accounts, nowValue __VA_OPT__(,) __VA_ARGS__))

ASYNC_TEST("clearing a class preserves an independent unknown quota fence") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000, standardDeadline = now + 60'000, unknownDeadline = now + 120'000;
  SELECT(initial, singlePool, now, "gpt-5.6-luna"); ASSERT_EQ(kind(initial), "eligible");
  RUN(markCodexQuotaBlocked(accounts(initial)[0], httpDateQuotaResponse(standardDeadline), now));
  auto seeded = state(kv); auto& slot = seeded.slots[0];
  routing::CodexQuotaClassBlock standard, unknown;
  standard.blocked_until_ms = standardDeadline; standard.source = "header_retry_after"; standard.quota_signal_observed_at_ms = now;
  unknown = standard; unknown.blocked_until_ms = unknownDeadline;
  slot.quota_blocked_until_ms = unknownDeadline; slot.quota_block_source = "header_retry_after";
  slot.quota_blocked_classes = std::vector<std::string>{"standard", "unknown"};
  slot.quota_blocks_by_class = routing::CodexQuotaBlocksByClass{{"standard", standard}, {"unknown", unknown}}; slot.quota_signal_observed_at_ms = now;
  RUN(kv->set(CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvValue(routing::toJson(seeded)))); resetCodexAccountRoutingForTest();
  auto sample = observation(0, "one", now + 1, 604'800, 50, standardDeadline);
  RUN(recordCodexCapacityRoutingObservations(std::vector<CodexCapacityRoutingObservationInput>{sample}, now + 1));
  SELECT(selected, singlePool, now + 2, "gpt-5.6-luna");
  const auto migrated = state(kv); ASSERT(migrated.slots[0].quota_blocks_by_class);
  EXPECT(!migrated.slots[0].quota_blocks_by_class->has("standard"));
  const std::string unknownBlockMessage = "unknown block";
  const auto& unknownBlock = required(migrated.slots[0].quota_blocks_by_class->get("unknown"), unknownBlockMessage);
  EXPECT_EQ(unknownBlock.blocked_until_ms, unknownDeadline); EXPECT(!unknownBlock.legacy_fallback);
}

ASYNC_TEST("a newer inference 429 remains authoritative over an older positive capacity sample") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000;
  SELECT(initial, pool, now); ASSERT_EQ(kind(initial), "eligible");
  auto sample = observation(1, "two", now + 1, 10'800, 50, now + 10'800'000);
  sample.windows.secondary = routing::CodexCapacityRoutingWindow{86'400, 50, now + 86'400'000};
  RUN(recordCodexCapacityRoutingObservations(std::vector<CodexCapacityRoutingObservationInput>{sample}, now + 1));
  SELECT(positive, pool, now + 2); ASSERT_EQ(kind(positive), "eligible");
  const auto accountTwo = findAccount(positive, "two"); const double blockedUntil = now + 60'000;
  RUN(markCodexQuotaBlocked(accountTwo, httpDateQuotaResponse(blockedUntil), now + 2));
  SELECT(selected, pool, now + 3); ASSERT_EQ(kind(selected), "eligible");
  EXPECT(!hasAccount(selected, "two")); EXPECT(hasBlocked(selected, "two"));
  const auto persisted = state(kv); EXPECT_EQ(persisted.slots[1].quota_blocked_until_ms, blockedUntil);
  EXPECT_EQ(persisted.slots[1].quota_signal_observed_at_ms, now + 2);
}

ASYNC_TEST("stale positive capacity cannot reopen a quota circuit") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000, deadline = now + 60'000;
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible");
  RUN(markCodexQuotaBlocked(accounts(initial)[0], httpDateQuotaResponse(deadline), now));
  auto sample = observation(0, "one", now - CODEX_CAPACITY_ROUTING_MAX_AGE_MS - 1, 10'800, 50, now + 10'800'000);
  RUN(recordCodexCapacityRoutingObservations(std::vector<CodexCapacityRoutingObservationInput>{sample}, now));
  SELECT(selected, singlePool, now + 1); EXPECT_EQ(kind(selected), "quota_blocked");
  EXPECT_EQ(state(kv).slots[0].quota_blocked_until_ms, deadline);
}

ASYNC_TEST("the persisted analytics snapshot reopens the matching account for its model") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000, deadline = now + 60'000;
  SELECT(initial, pool, now, "gpt-5.3-codex-spark"); ASSERT_EQ(kind(initial), "eligible");
  RUN(markCodexQuotaBlocked(findAccount(initial, "two"), httpDateQuotaResponse(deadline), now));
  const json::value snapshot = json::object{{"snapshot_at_ms", now + 1}, {"sources", json::array{dashboardSource(2, now + 1, true)}}};
  RUN(kv->set(PROVIDER_CAPACITY_SNAPSHOT_KEY, rt::KvValue(snapshot))); resetCodexAccountRoutingForTest();
  SELECT(selected, pool, now + 2, "gpt-5.3-codex-spark"); ASSERT_EQ(kind(selected), "eligible");
  EXPECT_EQ(accounts(selected)[0].auth.account_id, "one"); EXPECT_EQ(findAccount(selected, "two").quotaHeadroom, 50);
  EXPECT(!state(kv).slots[1].quota_blocked_until_ms);
}

ASYNC_TEST("a legacy slot-only dashboard snapshot cannot attach to a replacement account") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000, deadline = now + 60'000;
  const uos::types::CodexAuthState replacement{.access_token = "replacement-access", .refresh_token = "replacement-refresh", .account_id = "replacement", .updated_at_ms = now + 1};
  const CodexAuthPoolState replacementPool{.accounts = {replacement}, .updated_at_ms = now + 1};
  SELECT(initial, replacementPool, now); ASSERT_EQ(kind(initial), "eligible");
  RUN(markCodexQuotaBlocked(accounts(initial)[0], httpDateQuotaResponse(deadline), now + 1));
  const json::value snapshot = json::object{{"snapshot_at_ms", now}, {"sources", json::array{dashboardSource(1, now, false)}}};
  RUN(kv->set(PROVIDER_CAPACITY_SNAPSHOT_KEY, rt::KvValue(snapshot))); resetCodexAccountRoutingForTest();
  SELECT(selected, replacementPool, now + 2); EXPECT_EQ(kind(selected), "quota_blocked"); EXPECT_EQ(state(kv).slots[0].quota_blocked_until_ms, deadline);
}

ASYNC_TEST("capacity reconciliation uses the requested model instead of any additional headroom") {
  Fixture fixture; const double now = 1'700'000'000'000, blockedUntil = now + 60'000;
  SELECT(initial, pool, now); ASSERT_EQ(kind(initial), "eligible");
  RUN(markCodexQuotaBlocked(findAccount(initial, "two"), httpDateQuotaResponse(blockedUntil), now));
  auto sample = observation(1, "two", now + 1, 10'800, 100, now + 10'800'000);
  sample.additional_rate_limits = {{"GPT-5.3-Codex-Spark", std::string("codex_bengalfox"),
    {routing::CodexCapacityRoutingWindow{18'000, 50, now + 18'000'000}, std::nullopt}}};
  RUN(recordCodexCapacityRoutingObservations(std::vector<CodexCapacityRoutingObservationInput>{sample}, now + 1));
  SELECT(nonSpark, pool, now + 2, "gpt-5.2-codex"); ASSERT_EQ(kind(nonSpark), "eligible");
  EXPECT(!hasAccount(nonSpark, "two")); EXPECT(hasBlocked(nonSpark, "two"));
  SELECT(spark, pool, now + 2, "gpt-5.3-codex-spark"); ASSERT_EQ(kind(spark), "eligible");
  EXPECT_EQ(accounts(spark)[0].auth.account_id, "one"); EXPECT_EQ(findAccount(spark, "two").quotaHeadroom, 50);
}

ASYNC_TEST("capacity reconciliation preserves reset ambiguity and an active recovery probe") {
  Fixture fixture; auto kv = fixture.kv; const double now = 1'700'000'000'000, resetAtMs = now + 60'000;
  SELECT(initial, singlePool, now); ASSERT_EQ(kind(initial), "eligible"); const auto account = accounts(initial)[0];
  RUN(markCodexQuotaBlocked(account, httpDateQuotaResponse(resetAtMs), now));
  AWAIT(routingGeneration, getCodexQuotaBlockFence(account, resetAtMs)); ASSERT(routingGeneration);
  uos::codex::routing_mutations::CodexQuotaResetReconciliationInput input{resetAtMs, *routingGeneration};
  AWAIT(recoveryProbe, reconcileCodexQuotaAfterVerifiedReset(account, input)); ASSERT(recoveryProbe);
  const auto before = state(kv); EXPECT(before.slots[0].banked_reset_generation_ambiguous); EXPECT(before.slots[0].banked_reset_recovery_probe_pending);
  ASSERT(before.slots[0].probe_lease); EXPECT_EQ(before.slots[0].probe_lease->token, recoveryProbe->probeToken);
  auto sample = observation(0, "one", now + 1, 10'800, 50, now + 10'800'000);
  RUN(recordCodexCapacityRoutingObservations(std::vector<CodexCapacityRoutingObservationInput>{sample}, now + 1));
  const auto after = state(kv); EXPECT(after.slots[0].banked_reset_generation_ambiguous); EXPECT(after.slots[0].banked_reset_recovery_probe_pending);
  EXPECT_EQ(after.slots[0].observed_reset_at_ms, resetAtMs); ASSERT(after.slots[0].probe_lease);
  EXPECT_EQ(after.slots[0].probe_lease->token, recoveryProbe->probeToken); EXPECT_EQ(after.slots[0].generation, before.slots[0].generation);
}

ASYNC_TEST("capacity observations expire old account identities from durable routing state") {
  Fixture fixture; auto kv = fixture.kv; const double firstAtMs = 1'700'000'000'000, secondAtMs = firstAtMs + CODEX_CAPACITY_ROUTING_MAX_AGE_MS + 1;
  auto first = observation(0, "replaced-account", firstAtMs, 10'800, 20, firstAtMs + 10'800'000);
  auto second = observation(0, "current-account", secondAtMs, 10'800, 40, secondAtMs + 10'800'000);
  RUN(recordCodexCapacityRoutingObservations(std::vector<CodexCapacityRoutingObservationInput>{first}, firstAtMs));
  RUN(recordCodexCapacityRoutingObservations(std::vector<CodexCapacityRoutingObservationInput>{second}, secondAtMs));
  const auto row = kv->row(CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY); const auto* observations = rt::getArray(row, "observations");
  ASSERT(observations); ASSERT_EQ(observations->size(), std::size_t{1});
  EXPECT_EQ(rt::getNumber((*observations)[0], "snapshot_at_ms"), secondAtMs);
  const auto* windows = rt::get((*observations)[0], "windows"); ASSERT(windows); const auto* primary = rt::get(*windows, "primary"); ASSERT(primary);
  EXPECT_EQ(rt::getNumber(*primary, "used_percent"), 40);
}

ASYNC_TEST("a relative Retry-After quota answer moves the active account to the sibling") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow(); const auto authPool = freshPool(now); std::vector<std::string> calls;
  RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(authPool))));
  rt::GlobalFetchOverride fetch(rt::Fetch([&calls](rt::RequestInfo input, rt::RequestInit init) -> rt::Response {
    const auto url = requestUrl(input); if (!url.ends_with("/responses")) throw rt::JsError("Unexpected Codex URL: " + url);
    calls.push_back(init.headers.value_or(rt::Headers()).get("ChatGPT-Account-ID").value_or(""));
    if (calls.size() == 1) return deltaSecondsQuotaResponse("3600");
    rt::ResponseInit result; result.status = 200; return rt::Response(rt::BodyInit(std::string("{\"id\":\"served-by-sibling\"}")), result);
  }));
  const json::value body = json::object{{"model", "gpt-5-routing"}, {"input", "relative-quota"}}; const auto options = noRetryWait();
  AWAIT(response, fetchCodexResponses(body, options)); EXPECT_EQ(response.status(), 200); EXPECT_EQ(calls, (std::vector<std::string>{"one", "two"}));
  AWAIT(responseJson, response.json()); EXPECT_EQ(rt::getString(responseJson, "id"), "served-by-sibling");
}

ASYNC_TEST("a generic 429 retries the same account and never switches siblings") {
  Fixture fixture; auto kv = fixture.kv; const auto authPool = freshPool(rt::dateNow()); std::vector<std::string> calls;
  RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(authPool))));
  rt::GlobalFetchOverride fetch(rt::Fetch([&calls](rt::RequestInfo input, rt::RequestInit init) -> rt::Response {
    const auto url = requestUrl(input); if (!url.ends_with("/responses")) throw rt::JsError("Unexpected Codex URL: " + url);
    calls.push_back(init.headers.value_or(rt::Headers()).get("ChatGPT-Account-ID").value_or(""));
    rt::ResponseInit result;
    if (calls.size() == 1) { result.status = 429; result.headers = rt::Headers{{"Content-Type", "application/json"}, {"Retry-After", "1"}};
      return rt::Response(rt::BodyInit(std::string("{\"error\":{\"type\":\"rate_limit_error\",\"code\":\"codex_rate_limited\"}}")), result); }
    result.status = 200; return rt::Response(rt::BodyInit(std::string("{\"id\":\"same-account-retry\"}")), result);
  }));
  const json::value body = json::object{{"model", "gpt-5-routing"}, {"input", "generic-429"}}; const auto options = noRetryWait();
  AWAIT(response, fetchCodexResponses(body, options)); EXPECT_EQ(response.status(), 200); EXPECT_EQ(calls, (std::vector<std::string>{"one", "one"}));
  RUN(response.arrayBuffer());
}

ASYNC_TEST("a mixed quota and current-invalid-credential cohort authorizes paid fallback") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow(); const auto authPool = freshPool(now); int dispatches = 0;
  RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(authPool))));
  rt::GlobalFetchOverride fetch(countingUpstreamFetch("mixed-cohort", [&] { ++dispatches; }));
  SELECT(initial, authPool, now, "gpt-5-routing"); ASSERT_EQ(kind(initial), "eligible");
  RUN(markCodexQuotaBlocked(accounts(initial)[0], httpDateQuotaResponse(now + 60'000), now)); RUN(markCodexCredentialInvalid(accounts(initial)[1]));
  resetCodexAccountRoutingForTest(); STRONG(selected, authPool, rt::dateNow(), "gpt-5-routing"); ASSERT_EQ(kind(selected), "quota_blocked");
  const auto& blocked = std::get<routing::RouteSelectionQuotaBlocked>(selected); EXPECT_EQ(blocked.fullCohortUnavailable, true); EXPECT(!blocked.fullCohortExhausted);
  const json::value body = json::object{{"model", "gpt-5-routing"}, {"input", "mixed-cohort"}}; const auto options = noRetryWait();
  AWAIT(response, fetchCodexResponses(body, options)); EXPECT_EQ(response.status(), 503); EXPECT_EQ(getCodexRoutingError(response), "codex_subscriptions_unavailable");
  EXPECT(!response.headers().get("retry-after")); EXPECT_EQ(dispatches, 0); RUN(response.arrayBuffer());
}

ASYNC_TEST("a genuinely all-exhausted relative cohort preserves paid fallback eligibility") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow(); const auto authPool = freshPool(now); int dispatches = 0;
  RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(authPool))));
  rt::GlobalFetchOverride fetch(countingUpstreamFetch("relative-exhausted", [&] { ++dispatches; }));
  SELECT(initial, pool, now); ASSERT_EQ(kind(initial), "eligible");
  RUN(markCodexQuotaBlocked(accounts(initial)[0], deltaSecondsQuotaResponse("3600"), now));
  RUN(markCodexQuotaBlocked(accounts(initial)[1], deltaSecondsQuotaResponse("3600"), now)); resetCodexAccountRoutingForTest();
  const json::value body = json::object{{"model", "gpt-5-routing"}, {"input", "relative-exhausted"}}; const auto options = noRetryWait();
  AWAIT(response, fetchCodexResponses(body, options)); EXPECT_EQ(response.status(), 429); EXPECT_EQ(getCodexRoutingError(response), "codex_quota_blocked");
  EXPECT_EQ(dispatches, 0); RUN(response.arrayBuffer());
}

ASYNC_TEST("a held recovery lease alone cannot open paid fallback") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow(); const auto authPool = freshPool(now); int dispatches = 0;
  RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(authPool))));
  rt::GlobalFetchOverride fetch(countingUpstreamFetch("lease-only", [&] { ++dispatches; }));
  SELECT(initial, pool, now); ASSERT_EQ(kind(initial), "eligible");
  RUN(markCodexQuotaBlocked(accounts(initial)[0], httpDateQuotaResponse(now + 60'000), now));
  RUN(markCodexQuotaBlocked(accounts(initial)[1], httpDateQuotaResponse(now - 1'000), now - 10'000)); resetCodexAccountRoutingForTest();
  SELECT(expired, pool, now); ASSERT_EQ(kind(expired), "eligible"); const auto probeAccount = findAccount(expired, "two"); EXPECT(probeAccount.probeRequired);
  AWAIT(claimed, claimCodexRoutingProbe(pool, probeAccount)); ASSERT(claimed); resetCodexAccountRoutingForTest();
  const json::value body = json::object{{"model", "gpt-5-routing"}, {"input", "lease-only"}}; const auto options = noRetryWait();
  AWAIT(response, fetchCodexResponses(body, options)); EXPECT_EQ(response.status(), 429); EXPECT(!getCodexRoutingError(response)); EXPECT_EQ(dispatches, 0); RUN(response.arrayBuffer());
}

ASYNC_TEST("a claimed expired-circuit probe refreshes its own admission fence exactly once") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow(); const auto authPool = freshPool(now, true);
  RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(authPool))));
  STRONG(initial, authPool, now); ASSERT_EQ(kind(initial), "eligible");
  RUN(markCodexQuotaBlocked(accounts(initial)[0], httpDateQuotaResponse(now - 1'000), now - 10'000)); resetCodexAccountRoutingForTest();
  STRONG(expired, authPool, now); ASSERT_EQ(kind(expired), "eligible"); const auto probeRequired = accounts(expired)[0]; EXPECT(probeRequired.probeRequired);
  AWAIT(claimed, claimCodexRoutingProbe(authPool, probeRequired)); ASSERT(claimed);
  AWAIT(refreshed, refreshCodexActiveAccountAdmission(*claimed)); EXPECT(refreshed);
  const auto current = active(kv); const auto persisted = state(kv); EXPECT_EQ(current.generation, probeRequired.activeGeneration);
  EXPECT_EQ(current.routing_generation, persisted.slots[0].generation); EXPECT(persisted.slots[0].probe_lease);
  resetCodexAccountRoutingForTest(); STRONG(held, authPool, now); EXPECT_EQ(kind(held), "routing_unavailable");
}

ASYNC_TEST("same-account credential rotation refreshes admission without changing the active generation") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow(); const auto authPool = freshPool(now, true);
  RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(authPool)))); STRONG(initial, authPool, now); ASSERT_EQ(kind(initial), "eligible");
  const auto routed = accounts(initial)[0]; auto rotated = routed.auth; rotated.access_token = "rotated-access-token"; rotated.updated_at_ms = now + 1;
  const CodexAuthPoolState rotatedPool{.accounts = {rotated}, .updated_at_ms = now + 1}; RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(rotatedPool))));
  AWAIT(reconciled, reconcileCodexRoutingAccount(routed, rotated)); AWAIT(refreshed, refreshCodexActiveAccountAdmission(reconciled)); EXPECT(refreshed);
  const auto current = active(kv); EXPECT_EQ(current.generation, routed.activeGeneration); EXPECT_EQ(current.credential_version, reconciled.credentialVersion);
  EXPECT_EQ(current.routing_generation, state(kv).slots[0].generation);
}

ASYNC_TEST("a stale admission cannot dispatch after a concurrent active switch") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow(); const auto authPool = freshPool(now);
  RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(authPool)))); STRONG(initial, authPool, now); ASSERT_EQ(kind(initial), "eligible");
  const auto admitted = accounts(initial)[0]; EXPECT_EQ(admitted.auth.account_id, "one");
  RUN(markCodexQuotaBlocked(admitted, httpDateQuotaResponse(now + 60'000), now)); STRONG(switched, authPool, now); ASSERT_EQ(kind(switched), "eligible");
  EXPECT_EQ(accounts(switched)[0].auth.account_id, "two"); EXPECT(accounts(switched)[0].activeGeneration != admitted.activeGeneration);
  AWAIT(refreshed, refreshCodexActiveAccountAdmission(admitted)); EXPECT(!refreshed);
}

ASYNC_TEST("a stale admission cannot dispatch after concurrent credential invalidation") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow(); const auto authPool = freshPool(now);
  RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(authPool)))); STRONG(initial, authPool, now); ASSERT_EQ(kind(initial), "eligible");
  const auto admitted = accounts(initial)[0]; EXPECT_EQ(admitted.auth.account_id, "one"); RUN(markCodexCredentialInvalid(admitted));
  AWAIT(refreshed, refreshCodexActiveAccountAdmission(admitted)); EXPECT(!refreshed);
}

ASYNC_TEST("concurrent cold admission converges on one durable active generation") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow(); const auto authPool = freshPool(now);
  RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(authPool)))); AWAIT(selections, concurrentStrong(authPool, now));
  std::vector<RoutingAccount> eligible;
  for (const auto& selection : selections) if (kind(selection) == "eligible") eligible.push_back(accounts(selection)[0]);
  ASSERT(!eligible.empty()); for (const auto& account : eligible) EXPECT_EQ(account.auth.account_id, "one");
  EXPECT_EQ(active(kv).generation, 1); EXPECT_EQ(active(kv).slot, 0);
  RUN(markCodexQuotaBlocked(eligible[0], httpDateQuotaResponse(now + 60'000), now)); AWAIT(transitions, concurrentStrong(authPool, now));
  std::vector<RoutingAccount> transitioned;
  for (const auto& selection : transitions) if (kind(selection) == "eligible") transitioned.push_back(accounts(selection)[0]);
  ASSERT(!transitioned.empty()); for (const auto& account : transitioned) EXPECT_EQ(account.auth.account_id, "two");
  const auto moved = active(kv); EXPECT_EQ(moved.generation, 2); EXPECT_EQ(moved.slot, 1); EXPECT_EQ(moved.transition_reason, "quota_exhausted");
}

ASYNC_TEST("the durable active account follows authoritative transitions without late rollback") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow(); const auto authPool = freshPool(now);
  RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(authPool))));
  STRONG(first, authPool, now); ASSERT_EQ(kind(first), "eligible"); EXPECT_EQ(accounts(first)[0].auth.account_id, "one"); EXPECT_EQ(active(kv).generation, 1);
  RUN(markCodexQuotaBlocked(accounts(first)[0], httpDateQuotaResponse(now + 1'000), now));
  STRONG(movedToB, authPool, now + 1); ASSERT_EQ(kind(movedToB), "eligible"); EXPECT_EQ(accounts(movedToB)[0].auth.account_id, "two");
  EXPECT_EQ(active(kv).generation, 2); const double generationB = active(kv).generation;
  STRONG(stillB, authPool, now + 2'000); ASSERT_EQ(kind(stillB), "eligible"); EXPECT_EQ(accounts(stillB)[0].auth.account_id, "two"); EXPECT_EQ(active(kv).generation, generationB);
  RUN(markCodexQuotaBlocked(accounts(movedToB)[0], httpDateQuotaResponse(now + 10'000), now + 2'000));
  STRONG(backToA, authPool, now + 3'001); ASSERT_EQ(kind(backToA), "eligible"); EXPECT_EQ(accounts(backToA)[0].auth.account_id, "one"); EXPECT_EQ(active(kv).generation, generationB + 1);
  auto late = accounts(movedToB)[0]; late.activeGeneration = generationB; AWAIT(refreshed, refreshCodexActiveAccountAdmission(late)); EXPECT(!refreshed);
  const CodexAuthPoolState reorderedPool{.accounts = {authPool.accounts[1], authPool.accounts[0]}, .updated_at_ms = now + 4'000};
  RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(reorderedPool))));
  STRONG(reordered, authPool, now + 4'000); ASSERT_EQ(kind(reordered), "eligible"); EXPECT_EQ(accounts(reordered)[0].auth.account_id, "one");
  EXPECT_EQ(active(kv).slot, 1); EXPECT_EQ(active(kv).generation, generationB + 1);
  const auto hashBeforeReplacement = active(kv).account_id_hash; auto replacement = authPool.accounts[0]; replacement.account_id = "account-three";
  replacement.access_token = "access-three"; replacement.refresh_token = "refresh-three";
  const CodexAuthPoolState replacedPool{.accounts = {replacement, authPool.accounts[1]}, .updated_at_ms = now + 5'000};
  RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(replacedPool))));
  STRONG(replaced, authPool, now + 5'000); ASSERT_EQ(kind(replaced), "eligible"); EXPECT_EQ(accounts(replaced)[0].auth.account_id, "account-three");
  EXPECT_EQ(active(kv).generation, generationB + 2); EXPECT_EQ(active(kv).slot, 0); EXPECT(active(kv).account_id_hash != hashBeforeReplacement);
}

ASYNC_TEST("malformed durable active state fails retryably and preserves the record") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow(); const auto authPool = freshPool(now);
  RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(authPool)))); STRONG(bootstrapped, authPool, now); ASSERT_EQ(kind(bootstrapped), "eligible");
  const json::value malformed = json::object{{"v", 1}, {"account_id_hash", "not-an-opaque-hash"}};
  RUN(kv->set(CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, rt::KvValue(malformed))); STRONG(failed, authPool, now); EXPECT_EQ(kind(failed), "routing_unavailable");
  EXPECT(rt::deepEqual(kv->row(CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY), malformed));
  kv->values.delete_(key(CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY)); resetCodexAccountRoutingForTest();
  STRONG(rebootstrapped, authPool, now); EXPECT_EQ(kind(rebootstrapped), "eligible"); EXPECT_EQ(active(kv).generation, 1); EXPECT_EQ(active(kv).slot, 0);
}

ASYNC_TEST("a selected Codex subscription restricts which account may serve inference") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow(); const auto authPool = freshPool(now);
  RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(authPool)))); RUN(seedSubscriptionSelection(kv, std::vector<std::string>{"two"}));
  STRONG(restricted, authPool, now); ASSERT_EQ(kind(restricted), "eligible"); ASSERT_EQ(accounts(restricted).size(), std::size_t{1}); EXPECT_EQ(accounts(restricted)[0].auth.account_id, "two");
  RUN(seedSubscriptionSelection(kv, std::vector<std::string>{"one"})); STRONG(moved, authPool, now); ASSERT_EQ(kind(moved), "eligible");
  ASSERT_EQ(accounts(moved).size(), std::size_t{1}); EXPECT_EQ(accounts(moved)[0].auth.account_id, "one");
  const std::vector<json::array> providerIds{json::array{"codex"}, json::array{}};
  for (const auto& ids : providerIds) {
    const json::value selection = json::object{{"provider_ids", ids}, {"updated_at_ms", rt::dateNow()}};
    RUN(kv->set(PROVIDER_SELECTION_KV_KEY, rt::KvValue(selection))); resetProviderSelectionCacheForTest();
    STRONG(unrestricted, authPool, now); ASSERT_EQ(kind(unrestricted), "eligible"); EXPECT_EQ(accounts(unrestricted)[0].auth.account_id, "one");
  }
}

ASYNC_TEST("reset election uses the selected exhausted cohort") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow(); const auto authPool = freshPool(now);
  RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(authPool)))); RUN(seedSubscriptionSelection(kv, std::vector<std::string>{"one"}));
  STRONG(initial, authPool, now, "gpt-5-routing"); ASSERT_EQ(kind(initial), "eligible"); EXPECT_EQ(accounts(initial)[0].auth.account_id, "one");
  RUN(markCodexQuotaBlocked(accounts(initial)[0], httpDateQuotaResponse(now + 60'000), now)); STRONG(blocked, authPool, now + 1, "gpt-5-routing");
  ASSERT_EQ(kind(blocked), "quota_blocked"); const auto& cohort = std::get<routing::RouteSelectionQuotaBlocked>(blocked); EXPECT(cohort.fullCohortExhausted);
  ASSERT_EQ(cohort.blockedAccounts.size(), std::size_t{1}); const auto candidate = cohort.blockedAccounts[0];
  AWAIT(elected, electCodexResetRecoveryAccount(candidate, cohort.activeSnapshot, candidate.quotaResetAtMs)); ASSERT(elected); EXPECT_EQ(elected->auth.account_id, "one");
}

ASYNC_TEST("a selection that names no configured subscription leaves no eligible Codex account") {
  Fixture fixture; auto kv = fixture.kv; const double now = rt::dateNow(); const auto authPool = freshPool(now);
  RUN(kv->set(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(authPool)))); RUN(seedSubscriptionSelection(kv, std::vector<std::string>{"retired-account"}));
  STRONG(selected, authPool, now); EXPECT(kind(selected) != "eligible");
}
