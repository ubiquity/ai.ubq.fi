// Links: tests/codex_auth_cache_part2_test.cpp tests/helpers/codex_auth_cache_harness.cpp src/auth/relay.cpp src/authority/native_codex.cpp
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
// Port of tests/codex-auth-cache-part2.test.ts: all 20 original cases and assertions.
#include "tests/helpers/codex_auth_cache_harness.hpp"
#include "tests/helpers/codex_banked_reset_harness.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <set>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace h = uos::tests::helpers::codex_auth_cache_harness;
  namespace ca = uos::codex::auth;
  namespace cr = uos::codex::account_routing;
  namespace rs = uos::codex::routing_state;
  namespace cd = uos::codex::dispatch;
  namespace ce = uos::codex::experiment;
  using namespace h;
  using uos::codex::index::fetchCodexResponses;
  using Deferred = uos::tests::helpers::codex_banked_reset_harness::Deferred<void>;
  using ca::resetCodexAuthCacheForTest;
  using cr::resetCodexAccountRoutingForTest;

  rt::Response respond(const std::string& body = "{}", double status = 200, rt::Headers headers = {}) {
    rt::ResponseInit init; init.status = status; init.headers = headers; return rt::Response(rt::BodyInit(body), init);
  }
  rt::Response jsonResponse(const json::value& body, double status = 200, rt::Headers headers = {}) {
    headers.set("Content-Type", "application/json"); return respond(rt::jsonStringify(body), status, headers);
  }
  rt::Response quota(const std::string& retryAfter = "1") {
    return jsonResponse(json::object{{"error", json::object{{"type", "usage_limit_reached"}}}}, 429, rt::Headers{{"Retry-After", retryAfter}});
  }
  json::value input(const std::string& text) { return json::object{{"input", text}}; }
  rt::Headers requestHeaders(const rt::RequestInit& init) { return init.headers.value_or(rt::Headers{}); }
  std::string accountId(const rt::RequestInit& init) { return requestHeaders(init).get("chatgpt-account-id").value_or(""); }
  bool oauth(const rt::RequestInfo& info) { return rt::includes(requestUrl(info), "auth.openai.com/oauth/token"); }
  std::string serialized(const rt::RequestInit& init) {
    if (!init.body || !std::holds_alternative<std::string>(*init.body)) throw rt::JsError("Expected serialized request body.");
    return std::get<std::string>(*init.body);
  }
  template<class T> rt::Task<rt::Reason> failureOf(rt::Task<T> task) {
    rt::Promise<T> pending(std::move(task));
    try { (void) co_await pending.wait(); } catch (...) { co_return std::current_exception(); }
    co_return nullptr;
  }
  const rs::RouteSelectionEligible& eligible(const rs::RouteSelection& selection) {
    if (const auto* result = std::get_if<rs::RouteSelectionEligible>(&selection)) return *result;
    throw rt::JsError("expected eligible routing selection", "AssertionError");
  }
  rt::Task<void> resolvedSleep(double) { return rt::Promise<void>::resolve().wait(); }
  cd::FetchCodexResponsesOptions resetOptions(const std::shared_ptr<ScriptedResetProvider>& provider, const std::string& owner,
                                             std::optional<std::string> requestId = std::nullopt) {
    auto result = bankedResetRequestOptions(requestId.value_or(""));
    result.requestId = requestId; result.clientVersion.reset();
    result.bankedReset->provider = provider; result.bankedReset->newOwnerToken = [owner] { return owner; };
    return result;
  }
  std::optional<rs::CodexRoutingSlot> routingSlot() {
    const auto* entry = kv->extra.get(keyOf(cr::CODEX_ACCOUNT_ROUTING_KV_KEY));
    if (!entry) return std::nullopt;
    const auto state = cr::parseCodexAccountRoutingState(entry->value.toJson());
    if (!state || state->slots.empty()) return std::nullopt;
    return state->slots[0];
  }
  bool tombstone() {
    const auto slot = routingSlot(); return slot && slot->banked_reset_generation_ambiguous && slot->observed_reset_at_ms;
  }
}
#define AWAIT(name, expression) rt::Promise name##Pending((expression)); auto name = co_await name##Pending.wait()
#define WAIT(expression) do { rt::Promise pending((expression)); co_await pending.wait(); } while (false)

ASYNC_TEST("a malformed successful refresh is transient and does not quarantine the credential") {
  HarnessScope scope; kv->auth = pool(auth("one")); int refreshCalls = 0, inferenceCalls = 0;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo info, rt::RequestInit) {
    if (oauth(info)) {
      ++refreshCalls;
      if (refreshCalls == 1) return jsonResponse(json::object{{"refresh_token", "refresh-one"}});
      return jsonResponse(json::object{{"access_token", accessToken("recovered")}, {"refresh_token", "refresh-recovered"}});
    }
    ++inferenceCalls; return respond("{}", inferenceCalls <= 2 ? 401 : 200);
  }));
  AWAIT(error, failureOf(fetchCodexResponses(input("malformed-refresh"))));
  const auto* codexError = ca::codexErrorOf(error); ASSERT(codexError); EXPECT_EQ(codexError->status, 503); EXPECT(rt::includes(codexError->message, "missing access_token"));
  AWAIT(recovered, fetchCodexResponses(input("valid-refresh"))); EXPECT_EQ(recovered.status(), 200); EXPECT_EQ(refreshCalls, 2); EXPECT_EQ(inferenceCalls, 3);
}

ASYNC_TEST("direct failures release quota probes and timeouts do not gate the next request") {
  HarnessScope scope; double now = fixedStartMs; rt::globalDateNow() = [&] { return now; };
  struct Case { std::string name; std::optional<double> status; bool timeout; };
  const std::vector<Case> cases{{"403", 403, false}, {"invalid 400", 400, false}, {"500", 500, false}, {"network", std::nullopt, false}, {"timeout", std::nullopt, true}};
  for (const auto& testCase : cases) {
    now = fixedStartMs; kv->auth = pool(auth("one")); kv->extra.clear(); resetCodexAuthCacheForTest();
    AWAIT(initial, cr::selectCodexRoutingAccounts(kv->auth, kv->auth.accounts, now));
    auto blocked = quota(); WAIT(cr::markCodexQuotaBlocked(eligible(initial).accounts[0], blocked, now));
    now += 1001; int codexCalls = 0; rt::AbortController controller;
    rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) -> rt::Task<rt::Response> {
      ++codexCalls;
      if (codexCalls > 1) return rt::fulfilledTask(respond());
      if (testCase.timeout) {
        const auto reason = rt::makeReason(rt::DOMException("timed out", "TimeoutError")); controller.abort(reason);
        return rt::Promise<rt::Response>::reject(abortReasonError(init.signal ? init.signal->reason() : reason)).wait();
      }
      if (!testCase.status) return rt::Promise<rt::Response>::reject(rt::makeReason(rt::TypeError("network fixture"))).wait();
      return rt::fulfilledTask(respond("{}", *testCase.status));
    }));
    if (!testCase.status) {
      cd::FetchCodexResponsesOptions options; if (testCase.timeout) options.signal = controller.signal();
      AWAIT(error, failureOf(fetchCodexResponses(input(testCase.name), options)));
      const auto* codexError = ca::codexErrorOf(error); ASSERT(codexError); EXPECT_EQ(codexError->status, testCase.timeout ? 504 : 502);
    } else {
      AWAIT(direct, fetchCodexResponses(input(testCase.name))); EXPECT_EQ(direct.status(), *testCase.status);
    }
    AWAIT(second, fetchCodexResponses(input(testCase.name + "-second")));
    EXPECT_EQ(second.status(), testCase.timeout ? 503 : 200); EXPECT_EQ(codexCalls, testCase.timeout ? 1 : 2);
  }
}

ASYNC_TEST("cache-scope dispatch timeouts remain request-scoped") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); rt::AbortController controller; int inferenceCalls = 0;
  AWAIT(session, ce::beginCodexCacheScopeExperiment());
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
    ++inferenceCalls; controller.abort(rt::makeReason(rt::DOMException("cache-scope timeout", "TimeoutError")));
    return rt::Promise<rt::Response>::reject(abortReasonError(init.signal ? init.signal->reason() : controller.signal().reason())).wait();
  }));
  ce::FetchCodexResponsesForCacheScopeExperimentOptions options{session, 1, "cache-scope-timeout-conversation"}; options.signal = controller.signal();
  AWAIT(error, failureOf(ce::fetchCodexResponsesForCacheScopeExperiment(input("cache-scope-timeout"), options)));
  const auto* codexError = ca::codexErrorOf(error); ASSERT(codexError); EXPECT_EQ(codexError->code, "gateway_timeout"); EXPECT_EQ(codexError->status, 504);
  resetCodexAccountRoutingForTest(); std::vector<uos::types::CodexAuthState> one{kv->auth.accounts[0]};
  AWAIT(selected, cr::selectCodexRoutingAccounts(kv->auth, one, fixedStartMs)); EXPECT(!eligible(selected).accounts[0].probeRequired); EXPECT_EQ(inferenceCalls, 1);
}

ASYNC_TEST("a legacy timeout probe cannot block provider transport") {
  HarnessScope scope; kv->auth = pool(auth("one")); int inferenceCalls = 0;
  AWAIT(initial, cr::selectCodexRoutingAccounts(kv->auth, kv->auth.accounts, fixedStartMs));
  WAIT(cr::markCodexUpstreamTimeout(eligible(initial).accounts[0], fixedStartMs - cr::CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS - 1));
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { ++inferenceCalls; return respond("transport proceeds"); }));
  AWAIT(response, fetchCodexResponses(input("timeout-probe-race"))); EXPECT_EQ(response.status(), 200);
  AWAIT(text, response.text()); EXPECT_EQ(text, "transport proceeds"); EXPECT_EQ(inferenceCalls, 1);
  resetCodexAccountRoutingForTest();
  AWAIT(reseeded, cr::selectCodexRoutingAccounts(kv->auth, kv->auth.accounts, fixedStartMs));
  WAIT(cr::markCodexUpstreamTimeout(eligible(reseeded).accounts[0], fixedStartMs - cr::CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS - 1));
  resetCodexAccountRoutingForTest(); kv->routingCommitFailures = 3;
  AWAIT(blocked, fetchCodexResponses(input("routing-commit-unavailable"))); EXPECT_EQ(blocked.status(), 503); EXPECT_EQ(inferenceCalls, 1); WAIT(blocked.arrayBuffer());
  kv->routingCommitFailures = 0;
}

ASYNC_TEST("a bounded retry that proves quota keeps its quota retry classification") {
  HarnessScope scope; kv->auth = pool(auth("one")); int inferenceCalls = 0; std::optional<double> concurrentStatus;
  AWAIT(initial, cr::selectCodexRoutingAccounts(kv->auth, kv->auth.accounts, fixedStartMs));
  WAIT(cr::markCodexUpstreamTimeout(eligible(initial).accounts[0], fixedStartMs - cr::CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS - 1));
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> {
    ++inferenceCalls;
    if (inferenceCalls == 1) co_return quota();
    if (inferenceCalls == 2) {
      AWAIT(concurrent, fetchCodexResponses(input("timeout-probe-race-concurrent"))); concurrentStatus = concurrent.status(); co_return quota("60");
    }
    co_return respond("unexpected extra transport");
  }));
  cd::FetchCodexResponsesOptions options; options.retrySleep = resolvedSleep;
  AWAIT(response, fetchCodexResponses(input("timeout-probe-quota-retry"), options)); EXPECT_EQ(response.status(), 429); EXPECT_EQ(concurrentStatus, std::optional<double>(429)); EXPECT_EQ(inferenceCalls, 2);
}

ASYNC_TEST("a timeout during bounded retry refresh preserves only the quota fence") {
  HarnessScope scope; kv->auth = pool(auth("one")); rt::AbortController controller; int inferenceCalls = 0, refreshCalls = 0;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo info, rt::RequestInit init) -> rt::Task<rt::Response> {
    if (oauth(info)) { ++refreshCalls; return rt::fulfilledTask(jsonResponse(json::object{{"access_token", accessToken("one")}, {"refresh_token", "refresh-one"}})); }
    ++inferenceCalls;
    if (inferenceCalls == 1) return rt::fulfilledTask(quota());
    if (inferenceCalls == 2) return rt::fulfilledTask(respond("{}", 401));
    controller.abort(rt::makeReason(rt::DOMException("bounded retry timeout", "TimeoutError")));
    return rt::Promise<rt::Response>::reject(abortReasonError(init.signal ? init.signal->reason() : controller.signal().reason())).wait();
  }));
  cd::FetchCodexResponsesOptions options; options.signal = controller.signal(); options.retrySleep = resolvedSleep;
  AWAIT(error, failureOf(fetchCodexResponses(input("bounded-retry-refresh-timeout"), options)));
  const auto* codexError = ca::codexErrorOf(error); ASSERT(codexError); EXPECT_EQ(codexError->code, "gateway_timeout"); EXPECT_EQ(codexError->status, 504);
  resetCodexAccountRoutingForTest(); AWAIT(selected, cr::selectCodexRoutingAccounts(kv->auth, kv->auth.accounts, fixedStartMs));
  EXPECT(std::holds_alternative<rs::RouteSelectionQuotaBlocked>(selected)); EXPECT_EQ(inferenceCalls, 3); EXPECT_EQ(refreshCalls, 1);
}

ASYNC_TEST("an already-aborted timeout signal does not open or dispatch an account circuit") {
  HarnessScope scope; kv->auth = pool(auth("one")); rt::AbortController controller; int inferenceCalls = 0;
  controller.abort(rt::makeReason(rt::DOMException("deadline elapsed before dispatch", "TimeoutError")));
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) {
    ++inferenceCalls; return rt::Promise<rt::Response>::reject(rt::makeReason(rt::DOMException("transport should not start", "TimeoutError"))).wait();
  }));
  cd::FetchCodexResponsesOptions options; options.signal = controller.signal();
  AWAIT(error, failureOf(fetchCodexResponses(input("pre-dispatch-timeout"), options)));
  const auto* codexError = ca::codexErrorOf(error); ASSERT(codexError); EXPECT_EQ(codexError->code, "gateway_timeout"); EXPECT_EQ(codexError->status, 504);
  resetCodexAccountRoutingForTest(); AWAIT(selected, cr::selectCodexRoutingAccounts(kv->auth, kv->auth.accounts, fixedStartMs)); (void) eligible(selected); EXPECT_EQ(inferenceCalls, 0);
}

ASYNC_TEST("a 429 retry that proves invalid credentials remains quarantined") {
  HarnessScope scope; kv->auth = pool(auth("one")); double now = fixedStartMs; int inferenceCalls = 0, refreshCalls = 0;
  rt::globalDateNow() = [&] { return now; };
  AWAIT(initial, cr::selectCodexRoutingAccounts(kv->auth, kv->auth.accounts, now)); auto blocked = quota();
  WAIT(cr::markCodexQuotaBlocked(eligible(initial).accounts[0], blocked, now)); now += 1001;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo info, rt::RequestInit) {
    if (oauth(info)) { ++refreshCalls; return respond("{\"error\":\"invalid_grant\"}", 401); }
    ++inferenceCalls; return inferenceCalls == 1 ? quota() : respond("{}", 401);
  }));
  cd::FetchCodexResponsesOptions options; options.retrySleep = resolvedSleep;
  AWAIT(first, fetchCodexResponses(input("retry-invalid"), options)); EXPECT_EQ(first.status(), 401);
  AWAIT(second, fetchCodexResponses(input("retry-invalid-again"))); EXPECT_EQ(second.status(), 401); EXPECT_EQ(inferenceCalls, 2); EXPECT_EQ(refreshCalls, 1);
}

ASYNC_TEST("a 401 after proactive refresh does not refresh the same account twice") {
  HarnessScope scope; kv->auth = pool(staleAuth("one")); int refreshCalls = 0, inferenceCalls = 0;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo info, rt::RequestInit) {
    if (oauth(info)) { ++refreshCalls; return jsonResponse(json::object{{"access_token", accessToken("refreshed-once")}, {"refresh_token", "refresh-refreshed-once"}}); }
    ++inferenceCalls; return respond("{}", 401);
  }));
  AWAIT(response, fetchCodexResponses(input("one-refresh-only"))); EXPECT_EQ(response.status(), 401); EXPECT_EQ(refreshCalls, 1); EXPECT_EQ(inferenceCalls, 1);
}

ASYNC_TEST("every admission strongly reads the current durable credentials") {
  HarnessScope scope; kv->auth = pool(auth("old")); std::vector<std::string> authorizations, accountIds;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
    authorizations.push_back(requestHeaders(init).get("authorization").value_or("")); accountIds.push_back(accountId(init)); return respond();
  }));
  AWAIT(cold, fetchCodexResponses(input("cold"))); EXPECT_EQ(authorizations.back(), "Bearer " + accessToken("old")); EXPECT_EQ(accountIds.back(), "account-old");
  WAIT(kv->set(AUTH_KEY, rt::KvValue(json::value_from(pool(auth("rotated"))))));
  AWAIT(rotated, fetchCodexResponses(input("rotated"))); EXPECT_EQ(authorizations.back(), "Bearer " + accessToken("rotated")); EXPECT_EQ(accountIds.back(), "account-rotated");
  std::vector<rt::Promise<rt::Response>> requests;
  for (int index = 0; index < 4; ++index) requests.emplace_back(fetchCodexResponses(input("concurrent-" + rt::numberToString(index))));
  auto responses = co_await rt::all(std::move(requests)).wait(); (void) responses;
  EXPECT_EQ((std::vector<std::string>(authorizations.end() - 4, authorizations.end())), std::vector<std::string>(4, "Bearer " + accessToken("rotated")));
  EXPECT_EQ((std::vector<std::string>(accountIds.end() - 4, accountIds.end())), std::vector<std::string>(4, "account-rotated"));
  WAIT(kv->set(AUTH_KEY, rt::KvValue(json::value_from(pool(auth("replacement"))))));
  AWAIT(replacement, fetchCodexResponses(input("replacement"))); EXPECT_EQ(authorizations.back(), "Bearer " + accessToken("replacement")); EXPECT_EQ(accountIds.back(), "account-replacement");
}

ASYNC_TEST("a valid persisted Codex pool is not overlaid by a local configured seed") {
  HarnessScope scope; const auto persisted = auth("persisted"); auto localSeed = auth("local-stale"); localSeed.account_id = persisted.account_id;
  uos::config::config.isDeploy = false;
  uos::config::config.codexAuthJsonB64 = rt::btoa(rt::jsonStringify(json::object{{"tokens", json::object{{"access_token", localSeed.access_token}, {"refresh_token", localSeed.refresh_token}, {"account_id", localSeed.account_id}}}}));
  kv->auth = pool(persisted); const double versionBefore = kv->authVersion; std::vector<std::string> authorizations;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) { authorizations.push_back(requestHeaders(init).get("authorization").value_or("")); return respond(); }));
  AWAIT(response, fetchCodexResponses(input("persisted-authority"))); EXPECT_EQ(response.status(), 200); EXPECT_EQ(authorizations, (std::vector<std::string>{"Bearer " + persisted.access_token}));
  EXPECT(rt::deepEqual(json::value_from(kv->auth), json::value_from(pool(persisted)))); EXPECT_EQ(kv->authVersion, versionBefore);
}

ASYNC_TEST("banked reset exhausts normal routing, verifies, and retries the redeemed account once") {
  HarnessScope scope; kv->auth = pool(auth("one")); const auto reset = scriptedResetProvider(); std::vector<std::string> accountIds;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
    accountIds.push_back(accountId(init)); return accountIds.size() == 1 ? quota(stableBankedResetRetryAfter()) : jsonResponse(json::object{{"id", "response-after-reset"}});
  }));
  auto options = resetOptions(reset, "owner-happy", "banked-reset-happy");
  AWAIT(response, fetchCodexResponses(input("banked-reset-happy"), options)); EXPECT_EQ(response.status(), 200);
  EXPECT_EQ(accountIds, (std::vector<std::string>{"account-one", "account-one"})); EXPECT_EQ(reset->calls, (std::vector<std::string>{"inventory", "redeem", "verify"})); EXPECT_EQ(reset->idempotencyKeys.size(), std::size_t{1});
}

ASYNC_TEST("a partially blocked cohort is served by ordinary capacity with no reset-provider contact") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); uos::config::config.codexBaseUrl = "https://upstream-reset.test/backend-api/codex";
  std::vector<std::string> requests; int responseCount = 0;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo info, rt::RequestInit init) {
    const auto url = requestUrl(info); const auto id = accountId(init); const std::string method = init.method.value_or("GET");
    requests.push_back(method + " " + rt::URL(url).pathname() + " " + id);
    if (rt::endsWith(url, "/backend-api/codex/responses")) {
      ++responseCount;
      if (id == "account-one" && responseCount == 1) return jsonResponse(json::object{{"error", json::object{{"type", "usage_limit_reached"}, {"resets_at", (fixedStartMs + 60'000) / 1000}}}}, 429);
      return jsonResponse(json::object{{"id", "response-" + id}});
    }
    if (rt::endsWith(url, "/backend-api/wham/rate-limit-reset-credits")) return jsonResponse(json::object{{"available_count", 1}, {"credits", json::array{json::object{{"id", "expiring-credit"}, {"status", "available"}, {"reset_type", "codex_rate_limits"}, {"expires_at", nullptr}}}}});
    if (rt::endsWith(url, "/backend-api/wham/rate-limit-reset-credits/consume")) return jsonResponse(json::object{{"code", "reset"}, {"windows_reset", 1}});
    throw rt::JsError("unexpected request " + method + " " + url);
  }));
  auto options1 = bankedResetRequestOptions("seed-partial-block"), options2 = bankedResetRequestOptions("shadow-partial-block"), options3 = bankedResetRequestOptions("shadow-partial-block-duplicate");
  AWAIT(seeded, fetchCodexResponses(input("seed-partial-block"), options1)); EXPECT_EQ(seeded.status(), 200);
  AWAIT(shadowed, fetchCodexResponses(input("shadow-partial-block"), options2)); EXPECT_EQ(shadowed.status(), 200);
  AWAIT(duplicateShadow, fetchCodexResponses(input("shadow-partial-block-duplicate"), options3)); EXPECT_EQ(duplicateShadow.status(), 200);
  EXPECT_EQ(requests, (std::vector<std::string>{"POST /backend-api/codex/responses account-one", "POST /backend-api/codex/responses account-two", "POST /backend-api/codex/responses account-two", "POST /backend-api/codex/responses account-two"}));
}

ASYNC_TEST("persistent live serves a partially blocked cohort without reset-provider contact") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); uos::config::config.codexBaseUrl = "https://upstream-reset.test/backend-api/codex";
  WAIT(seedStableBankedResetBlock());
  std::vector<std::string> inventoryAccountIds, consumeAccountIds, inferenceAccountIds;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo info, rt::RequestInit init) {
    const auto url = requestUrl(info); const auto id = accountId(init);
    if (rt::endsWith(url, "/backend-api/codex/responses")) { inferenceAccountIds.push_back(id); return jsonResponse(json::object{{"id", "response-" + id}}); }
    if (rt::endsWith(url, "/backend-api/wham/rate-limit-reset-credits")) {
      inventoryAccountIds.push_back(id);
      return jsonResponse(json::object{{"available_count", 1}, {"credits", json::array{json::object{{"id", "credit-" + id}, {"status", "available"}, {"reset_type", "codex_rate_limits"}, {"expires_at", nullptr}}}}});
    }
    if (rt::endsWith(url, "/backend-api/wham/rate-limit-reset-credits/consume")) { consumeAccountIds.push_back(id); return jsonResponse(json::object{{"code", "reset"}, {"windows_reset", 1}}); }
    throw rt::JsError("unexpected request " + url);
  }));
  auto options1 = bankedResetRequestOptions("persistent-live-partial-arm"), options2 = bankedResetRequestOptions("persistent-live-partial-consume");
  AWAIT(first, fetchCodexResponses(input("persistent-live-partial-arm"), options1)); EXPECT_EQ(first.status(), 200);
  AWAIT(second, fetchCodexResponses(input("persistent-live-partial-consume"), options2)); EXPECT_EQ(second.status(), 200);
  EXPECT_EQ(inferenceAccountIds, (std::vector<std::string>{"account-two", "account-two"})); EXPECT(inventoryAccountIds.empty()); EXPECT(consumeAccountIds.empty());
}

ASYNC_TEST("persistent live consumes an all-blocked cohort in one request and retries once") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); uos::config::config.codexBaseUrl = "https://upstream-reset.test/backend-api/codex";
  WAIT(seedStableBankedResetBlock(std::string("account-one"))); WAIT(seedStableBankedResetBlock(std::string("account-two")));
  std::vector<std::string> inventoryAccountIds, consumeAccountIds, inferenceAccountIds; std::vector<json::value> consumeBodies;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo info, rt::RequestInit init) {
    const auto url = requestUrl(info); const auto id = accountId(init);
    if (rt::endsWith(url, "/backend-api/codex/responses")) { inferenceAccountIds.push_back(id); return jsonResponse(json::object{{"id", "response-" + id}}); }
    if (rt::endsWith(url, "/backend-api/wham/rate-limit-reset-credits")) {
      inventoryAccountIds.push_back(id); return jsonResponse(json::object{{"available_count", 1}, {"credits", json::array{json::object{{"id", "credit-" + id}, {"status", "available"}, {"reset_type", "codex_rate_limits"}, {"expires_at", nullptr}}}}});
    }
    if (rt::endsWith(url, "/backend-api/wham/rate-limit-reset-credits/consume")) {
      consumeAccountIds.push_back(id); consumeBodies.push_back(rt::jsonParse(serialized(init))); return jsonResponse(json::object{{"code", "reset"}, {"windows_reset", 1}});
    }
    throw rt::JsError("unexpected request " + url);
  }));
  auto options = bankedResetRequestOptions("persistent-live-all-blocked");
  AWAIT(consumed, fetchCodexResponses(input("persistent-live-all-blocked"), options)); EXPECT_EQ(consumed.status(), 200); EXPECT(ca::getCodexRoutingProbe(consumed));
  std::sort(inventoryAccountIds.begin(), inventoryAccountIds.end()); EXPECT_EQ(inventoryAccountIds, (std::vector<std::string>{"account-one", "account-two"}));
  EXPECT_EQ(consumeAccountIds, (std::vector<std::string>{"account-one"})); EXPECT_EQ(inferenceAccountIds, (std::vector<std::string>{"account-one"}));
  ASSERT_EQ(consumeBodies.size(), std::size_t{1}); EXPECT_EQ(rt::getString(consumeBodies[0], "credit_id"), std::optional<std::string_view>("credit-account-one")); EXPECT(rt::getString(consumeBodies[0], "redeem_request_id"));
}

ASYNC_TEST("post-reset response probes retain their tombstone until an explicit completed outcome") {
  HarnessScope scope; uos::provider::health::resetProviderHealthThrottleForTest();
  auto fetchPostResetResponse = [&](std::string owner) -> rt::Task<rt::Response> {
    const auto reset = scriptedResetProvider(); int inferenceCalls = 0;
    kv->auth = pool(auth("one")); kv->extra.clear(); resetCodexAuthCacheForTest(); resetCodexAccountRoutingForTest();
    rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) {
      ++inferenceCalls;
      return inferenceCalls == 1 ? quota(stableBankedResetRetryAfter()) : respond("data: {\"type\":\"response.completed\",\"response\":{\"output\":[]}}\n\n", 200, rt::Headers{{"Content-Type", "text/event-stream"}});
    }));
    auto options = resetOptions(reset, owner);
    AWAIT(response, fetchCodexResponses(input("post-reset-probe-" + owner), options));
    EXPECT_EQ(response.status(), 200); EXPECT_EQ(inferenceCalls, 2); EXPECT_EQ(reset->calls, (std::vector<std::string>{"inventory", "redeem", "verify"})); EXPECT(ca::getCodexRoutingProbe(response)); EXPECT(tombstone());
    co_return response;
  };
  const std::vector<std::string> outcomes{"failed", "incomplete", "cancelled"};
  for (const auto& outcome : outcomes) {
    AWAIT(response, fetchPostResetResponse("owner-post-reset-" + outcome));
    WAIT(ca::releaseCodexResponseProbe(response)); EXPECT(!ca::getCodexRoutingProbe(response)); EXPECT(tombstone());
    const auto slot = routingSlot(); ASSERT(slot); EXPECT(!slot->probe_lease);
  }
  AWAIT(completed, fetchPostResetResponse(std::string("owner-post-reset-completed")));
  Deferred commitGate, commitEntered, committed;
  kv->providerHealthSuccessCommitGate = commitGate.promise;
  kv->onProviderHealthSuccessCommit = [&] { commitEntered.resolve(); };
  kv->onProviderHealthSuccessCommitted = [&] { committed.resolve(); };
  bool completionSettled = false;
  rt::Promise<void> completion = rt::Promise<void>(ca::markCodexResponseCompleted(completed)).then([&] { completionSettled = true; });
  EXPECT(!ca::getCodexRoutingProbe(completed));
  co_await commitEntered.promise.wait(); co_await rt::sleep(0); EXPECT(completionSettled); co_await completion.wait();
  const auto slot = routingSlot(); ASSERT(slot); EXPECT(!slot->banked_reset_generation_ambiguous); EXPECT(!slot->observed_reset_at_ms); EXPECT(!slot->probe_lease);
  commitGate.resolve(); co_await committed.promise.wait();
  kv->providerHealthSuccessCommitGate.reset(); kv->onProviderHealthSuccessCommit = {}; kv->onProviderHealthSuccessCommitted = {};
  uos::provider::health::resetProviderHealthThrottleForTest();
}

ASYNC_TEST("simultaneous gateway requests share one durable banked-reset submission") {
  HarnessScope scope; kv->auth = pool(auth("one")); Deferred redeemEntered, redeemGate;
  ScriptedResetOptions scripted; scripted.onRedeem = [&] { redeemEntered.resolve(); }; scripted.redeemGate = redeemGate.promise;
  const auto reset = scriptedResetProvider(scripted); int inferenceCalls = 0;
  auto cleanup = rt::scopeExit([&] { redeemGate.resolve(); });
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) {
    ++inferenceCalls; return inferenceCalls == 1 ? quota(stableBankedResetRetryAfter()) : jsonResponse(json::object{{"id", "response-after-concurrent-reset"}});
  }));
  auto options1 = resetOptions(reset, "owner-concurrent-gateway-reset", "first-concurrent-banked-reset"), options2 = resetOptions(reset, "owner-concurrent-gateway-reset", "second-concurrent-banked-reset");
  rt::Promise<rt::Response> first(fetchCodexResponses(input("first-concurrent-banked-reset"), options1)); co_await redeemEntered.promise.wait();
  AWAIT(second, fetchCodexResponses(input("second-concurrent-banked-reset"), options2)); EXPECT_EQ(second.status(), 429); EXPECT_EQ(inferenceCalls, 1);
  EXPECT_EQ(reset->calls, (std::vector<std::string>{"inventory", "redeem"})); EXPECT_EQ(reset->idempotencyKeys.size(), std::size_t{1});
  redeemGate.resolve(); auto firstResponse = co_await first.wait(); EXPECT_EQ(firstResponse.status(), 200); EXPECT_EQ(inferenceCalls, 2);
  EXPECT_EQ(reset->calls, (std::vector<std::string>{"inventory", "redeem", "verify"})); EXPECT_EQ(reset->idempotencyKeys.size(), std::size_t{1}); EXPECT_EQ((std::set<std::string>(reset->idempotencyKeys.begin(), reset->idempotencyKeys.end()).size()), std::size_t{1});
}

ASYNC_TEST("an already-redeemed reset is independently verified before one same-account retry") {
  HarnessScope scope; kv->auth = pool(auth("one")); std::vector<std::string> accountIds;
  ScriptedResetOptions scripted; scripted.redeemKind = "already_redeemed"; const auto reset = scriptedResetProvider(scripted);
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
    accountIds.push_back(accountId(init)); return accountIds.size() == 1 ? quota(stableBankedResetRetryAfter()) : jsonResponse(json::object{{"id", "response-after-already-redeemed"}});
  }));
  auto options = resetOptions(reset, "owner-already-redeemed", "banked-reset-already-redeemed");
  AWAIT(response, fetchCodexResponses(input("banked-reset-already-redeemed"), options)); EXPECT_EQ(response.status(), 200);
  EXPECT_EQ(accountIds, (std::vector<std::string>{"account-one", "account-one"})); EXPECT_EQ(reset->calls, (std::vector<std::string>{"inventory", "redeem", "verify"})); EXPECT_EQ(reset->idempotencyKeys.size(), std::size_t{1});
}

ASYNC_TEST("an auth rotation after verification fences off the post-reset retry") {
  HarnessScope scope; kv->auth = pool(auth("one")); int inferenceCalls = 0;
  ScriptedResetOptions scripted; scripted.onVerify = [] {
    auto replacement = auth("one"); replacement.access_token = accessToken("one-rotated"); replacement.refresh_token = "refresh-one-rotated";
    kv->auth = pool(replacement); ++kv->authVersion;
  };
  const auto reset = scriptedResetProvider(scripted);
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { ++inferenceCalls; return quota(stableBankedResetRetryAfter()); }));
  auto options = resetOptions(reset, "owner-auth-rotation");
  AWAIT(response, fetchCodexResponses(input("banked-reset-auth-rotation"), options)); EXPECT_EQ(response.status(), 429); EXPECT_EQ(inferenceCalls, 1); EXPECT_EQ(reset->calls, (std::vector<std::string>{"inventory", "redeem", "verify"}));
}

ASYNC_TEST("an auth rotation inside the final dispatch hook fences off a post-reset retry") {
  HarnessScope scope; kv->auth = pool(auth("one")); int inferenceCalls = 0, beforeDispatchCalls = 0;
  std::vector<int> startedDispatchGenerations, cancelledDispatchGenerations; const auto reset = scriptedResetProvider();
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { ++inferenceCalls; return quota(stableBankedResetRetryAfter()); }));
  auto options = resetOptions(reset, "owner-dispatch-race");
  options.beforeDispatch = std::function<rt::Task<std::optional<uos::api_key_policy::ApiKeyProviderDispatch>>()>([&] {
    const int generation = ++beforeDispatchCalls;
    if (generation == 2) {
      auto replacement = auth("one"); replacement.access_token = accessToken("one-rotated-during-dispatch"); replacement.refresh_token = "refresh-one-rotated-during-dispatch";
      kv->auth = pool(replacement); ++kv->authVersion;
    }
    uos::api_key_policy::ApiKeyProviderDispatch dispatch;
    dispatch.markTransportStarted = [&, generation] { startedDispatchGenerations.push_back(generation); };
    dispatch.cancelBeforeTransport = [&, generation] { cancelledDispatchGenerations.push_back(generation); return rt::Promise<void>::resolve().wait(); };
    return rt::Promise<std::optional<uos::api_key_policy::ApiKeyProviderDispatch>>::resolve(dispatch).wait();
  });
  AWAIT(response, fetchCodexResponses(input("banked-reset-dispatch-race"), options)); EXPECT_EQ(response.status(), 429); EXPECT_EQ(beforeDispatchCalls, 2); EXPECT_EQ(inferenceCalls, 1);
  EXPECT_EQ(startedDispatchGenerations, (std::vector<int>{1})); EXPECT_EQ(cancelledDispatchGenerations, (std::vector<int>{2})); EXPECT_EQ(reset->calls, (std::vector<std::string>{"inventory", "redeem", "verify"}));
}
