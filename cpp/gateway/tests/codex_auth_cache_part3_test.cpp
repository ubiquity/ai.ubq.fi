// Links: tests/codex_auth_cache_part3_test.cpp tests/helpers/codex_auth_cache_harness.cpp src/auth/relay.cpp src/authority/native_codex.cpp
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
// Port of tests/codex-auth-cache-part3.test.ts: all 20 original cases and assertions.
#include "tests/helpers/codex_auth_cache_harness.hpp"
#include "tests/helpers/codex_banked_reset_harness.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace h = uos::tests::helpers::codex_auth_cache_harness;
  namespace ca = uos::codex::auth;
  namespace cr = uos::codex::account_routing;
  namespace rs = uos::codex::routing_state;
  namespace cd = uos::codex::dispatch;
  using namespace h;
  using uos::codex::index::fetchCodexResponses;
  using Deferred = uos::tests::helpers::codex_banked_reset_harness::Deferred<void>;
  rt::Response respond(std::string body, double status, rt::Headers headers = {}) {
    rt::ResponseInit init; init.status = status; init.headers = headers; return rt::Response(rt::BodyInit(std::move(body)), init);
  }
  rt::Response jsonResponse(const json::value& body, double status = 200, rt::Headers headers = {}) {
    headers.set("Content-Type", "application/json"); return respond(rt::jsonStringify(body), status, headers);
  }
  rt::Response quota(const std::string& retryAfter = stableBankedResetRetryAfter()) {
    return jsonResponse(json::object{{"error", json::object{{"type", "usage_limit_reached"}}}}, 429, rt::Headers{{"Retry-After", retryAfter}});
  }
  json::value input(const std::string& text) { return json::object{{"input", text}}; }
  std::optional<std::string_view> errorCode(const json::value& body) {
    const auto* error = rt::get(body, "error"); return error ? rt::getString(*error, "code") : std::nullopt;
  }
  std::string accountId(const rt::RequestInit& init) { return init.headers.value_or(rt::Headers{}).get("chatgpt-account-id").value_or(""); }
  cd::FetchCodexResponsesOptions resetOptions(const std::shared_ptr<ScriptedResetProvider>& provider, const std::string& owner,
                                             std::optional<std::string> requestId = std::nullopt) {
    auto options = bankedResetRequestOptions(requestId.value_or("")); options.requestId = requestId; options.clientVersion.reset();
    options.bankedReset->provider = provider; options.bankedReset->newOwnerToken = [owner] { return owner; }; return options;
  }
  std::optional<rs::CodexAccountRoutingState> routing() {
    const auto* entry = kv->extra.get(keyOf(cr::CODEX_ACCOUNT_ROUTING_KV_KEY));
    return entry ? cr::parseCodexAccountRoutingState(entry->value.toJson()) : std::nullopt;
  }
  const rs::RouteSelectionEligible& eligible(const rs::RouteSelection& selection) {
    const auto* value = std::get_if<rs::RouteSelectionEligible>(&selection);
    if (!value) throw rt::JsError("expected eligible routing selection", "AssertionError");
    return *value;
  }
  const rs::RoutingAccount& namedAccount(const rs::RouteSelection& selection, const std::string& account) {
    const auto& accounts = eligible(selection).accounts;
    const auto found = std::find_if(accounts.begin(), accounts.end(), [&](const auto& item) { return item.auth.account_id == account; });
    if (found == accounts.end()) throw rt::JsError("expected routing account", "AssertionError");
    return *found;
  }
  rt::Task<void> resolvedSleep(double) { return rt::Promise<void>::resolve().wait(); }
  template<class T> rt::Task<rt::Reason> failureOf(rt::Task<T> task) {
    rt::Reason failure; rt::Promise<T> pending(std::move(task));
    try { (void) co_await pending.wait(); } catch (...) { failure = std::current_exception(); }
    co_return failure;
  }
}
#define AWAIT(name, expression) rt::Promise name##Pending((expression)); auto name = co_await name##Pending.wait()
#define WAIT(expression) do { rt::Promise pending((expression)); co_await pending.wait(); } while (false)

ASYNC_TEST("a global active-account transition inside the final dispatch hook fences off a post-reset retry") {
  HarnessScope scope; kv->auth = pool(auth("one")); int inferenceCalls = 0, beforeDispatchCalls = 0;
  std::vector<int> startedDispatchGenerations, cancelledDispatchGenerations; const auto reset = scriptedResetProvider();
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { ++inferenceCalls; return quota(); }));
  auto options = resetOptions(reset, "owner-active-fence");
  options.beforeDispatch = std::function<rt::Task<std::optional<uos::api_key_policy::ApiKeyProviderDispatch>>()>([&]() -> rt::Task<std::optional<uos::api_key_policy::ApiKeyProviderDispatch>> {
    const int dispatchGeneration = ++beforeDispatchCalls;
    if (beforeDispatchCalls == 2) {
      const auto* entry = kv->extra.get(keyOf(cr::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY)); ASSERT(entry);
      auto active = cr::parseCodexActiveAccountSelection(entry->value.toJson()); ASSERT(active); ++active->generation; active->updated_at_ms = fixedStartMs;
      const auto value = rt::KvValue(rs::toJson(*active)); AWAIT(committed, kv->set(cr::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, value)); (void) committed;
    }
    uos::api_key_policy::ApiKeyProviderDispatch dispatch;
    dispatch.markTransportStarted = [&, dispatchGeneration] { startedDispatchGenerations.push_back(dispatchGeneration); };
    dispatch.cancelBeforeTransport = [&, dispatchGeneration] { cancelledDispatchGenerations.push_back(dispatchGeneration); return rt::Promise<void>::resolve().wait(); };
    const auto resolved = rt::Promise<std::optional<uos::api_key_policy::ApiKeyProviderDispatch>>::resolve(dispatch);
    auto value = co_await resolved.wait(); co_return value;
  });
  AWAIT(response, fetchCodexResponses(input("banked-reset-active-account-fence"), options));
  EXPECT_EQ(response.status(), 429); EXPECT_EQ(beforeDispatchCalls, 2); EXPECT_EQ(inferenceCalls, 1);
  EXPECT_EQ(startedDispatchGenerations, (std::vector<int>{1})); EXPECT_EQ(cancelledDispatchGenerations, (std::vector<int>{2}));
  EXPECT_EQ(reset->calls, (std::vector<std::string>{"inventory", "redeem", "verify"}));
}
ASYNC_TEST("an auth-pool slot reorder during a claimed reset fences submission before redemption") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); std::vector<std::string> upstreamAccounts;
  ScriptedResetOptions scripted; scripted.onInventory = [] { kv->auth = pool(auth("two"), auth("one")); ++kv->authVersion; return rt::fulfilledTask(); };
  const auto reset = scriptedResetProvider(scripted);
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) { upstreamAccounts.push_back(accountId(init)); return quota(); }));
  auto options = resetOptions(reset, "owner-slot-reorder"); AWAIT(response, fetchCodexResponses(input("banked-reset-slot-reorder"), options));
  EXPECT_EQ(response.status(), 429); EXPECT_EQ(upstreamAccounts, (std::vector<std::string>{"account-one", "account-two"}));
  EXPECT_EQ(reset->calls, (std::vector<std::string>{"inventory", "inventory"}));
}
ASYNC_TEST("the request that first discovers a healthy fallback does not spend before a fresh cohort read") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); std::vector<std::string> accountIds; const auto reset = scriptedResetProvider();
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
    accountIds.push_back(accountId(init)); return accountIds.size() == 1 ? quota() : jsonResponse(json::object{{"id", "fallback-success"}});
  }));
  const auto options = resetOptions(reset, "owner-fallback"); AWAIT(response, fetchCodexResponses(input("banked-reset-fallback"), options));
  EXPECT_EQ(response.status(), 200); EXPECT_EQ(accountIds, (std::vector<std::string>{"account-one", "account-two"})); EXPECT_EQ(reset->calls, std::vector<std::string>{});
}
ASYNC_TEST("a sibling re-blocked during reset inventory cannot be dispatched from the stale snapshot") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); std::vector<std::string> accountIds;
  WAIT(seedStableBankedResetBlock()); WAIT(seedStableBankedResetBlock("account-two"));
  ScriptedResetOptions scripted; scripted.onInventory = [] {
    const auto key = keyOf(cr::CODEX_ACCOUNT_ROUTING_KV_KEY); const auto* entry = kv->extra.get(key); ASSERT(entry);
    auto state = cr::parseCodexAccountRoutingState(entry->value.toJson()); ASSERT(state); ASSERT(state->slots.size() >= 2);
    const double version = entry->version; auto& sibling = state->slots[1]; const auto retryAtMs = rt::Date::parse(stableBankedResetRetryAfter());
    sibling.quota_blocked_until_ms = retryAtMs; sibling.quota_block_source = "header_retry_after"; sibling.observed_reset_at_ms = retryAtMs;
    sibling.observed_reset_at_is_stable = true; sibling.banked_reset_generation_ambiguous = false; ++sibling.generation; sibling.probe_lease.reset();
    state->updated_at_ms = fixedStartMs; kv->extra.set(key, AuthKv::ExtraEntry{rt::KvValue(rs::toJson(*state)), version + 1}); return rt::fulfilledTask();
  };
  const auto reset = scriptedResetProvider(scripted);
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) { accountIds.push_back(accountId(init)); return jsonResponse(json::object{{"id", "stale-sibling-fallback"}}); }));
  const auto options = resetOptions(reset, "owner-partial-preflight-sibling-blocked", "partial-preflight-sibling-blocked");
  AWAIT(response, fetchCodexResponses(input("partial-preflight-sibling-blocked"), options)); EXPECT_EQ(response.status(), 429);
  AWAIT(body, response.json()); EXPECT_EQ(errorCode(body), std::optional<std::string>("codex_quota_blocked"));
  auto inventory = reset->inventoryAccountIds; rt::sort(inventory, [](const auto& left, const auto& right) { return rt::localeCompare(left, right); });
  EXPECT_EQ(inventory, (std::vector<std::string>{"account-one", "account-two"})); EXPECT_EQ(reset->redeemAccountIds, std::vector<std::string>{});
  EXPECT_EQ(std::count(reset->calls.begin(), reset->calls.end(), "inventory"), 2); EXPECT_EQ(rt::includes(reset->calls, std::string("redeem")), false); EXPECT_EQ(accountIds, std::vector<std::string>{});
}
ASYNC_TEST("a legacy timeout mutation during reset inventory cannot open a stale fallback") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); std::vector<std::string> accountIds;
  WAIT(seedStableBankedResetBlock()); WAIT(seedStableBankedResetBlock("account-two"));
  ScriptedResetOptions scripted; scripted.onInventory = [] {
    const auto key = keyOf(cr::CODEX_ACCOUNT_ROUTING_KV_KEY); const auto* entry = kv->extra.get(key); ASSERT(entry);
    auto state = cr::parseCodexAccountRoutingState(entry->value.toJson()); ASSERT(state); ASSERT(state->slots.size() >= 2); const auto version = entry->version;
    state->slots[1].upstream_timeout_blocked_until_ms = fixedStartMs + cr::CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS; ++state->slots[1].generation;
    state->slots[1].probe_lease.reset(); state->updated_at_ms = fixedStartMs;
    kv->extra.set(key, AuthKv::ExtraEntry{rt::KvValue(rs::toJson(*state)), version + 1}); return rt::fulfilledTask();
  };
  const auto reset = scriptedResetProvider(scripted);
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) { accountIds.push_back(accountId(init)); return jsonResponse(json::object{{"id", "stale-timeout-fallback"}}); }));
  const auto options = resetOptions(reset, "owner-partial-preflight-sibling-timeout", "partial-preflight-sibling-timeout");
  AWAIT(response, fetchCodexResponses(input("partial-preflight-sibling-timeout"), options)); EXPECT_EQ(response.status(), 429);
  AWAIT(body, response.json()); EXPECT_EQ(errorCode(body), std::optional<std::string>("codex_quota_blocked"));
  EXPECT_EQ(reset->redeemAccountIds, std::vector<std::string>{}); EXPECT_EQ(rt::includes(reset->calls, std::string("redeem")), false); EXPECT_EQ(accountIds, std::vector<std::string>{});
}
ASYNC_TEST("a routing KV outage during reset inventory never dispatches a stale fallback") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); std::vector<std::string> accountIds;
  WAIT(seedStableBankedResetBlock()); WAIT(seedStableBankedResetBlock("account-two"));
  ScriptedResetOptions scripted; scripted.onInventory = [] {
    kv->onRoutingRead = [](double) -> rt::Task<void> { throw rt::JsError("routing KV unavailable"); }; return rt::fulfilledTask();
  };
  auto cleanup = rt::scopeExit([] { kv->onRoutingRead = {}; }); const auto reset = scriptedResetProvider(scripted);
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) { accountIds.push_back(accountId(init)); return jsonResponse(json::object{{"id", "cached-sibling-fallback"}}); }));
  const auto options = resetOptions(reset, "owner-reset-inventory-routing-unavailable", "reset-inventory-routing-unavailable");
  AWAIT(response, fetchCodexResponses(input("reset-inventory-routing-unavailable"), options)); EXPECT_EQ(response.status(), 429);
  AWAIT(body, response.json()); EXPECT_EQ(errorCode(body), std::optional<std::string>("codex_capacity_unavailable"));
  EXPECT_EQ(reset->redeemAccountIds, std::vector<std::string>{}); EXPECT_EQ(rt::includes(reset->calls, std::string("redeem")), false); EXPECT_EQ(accountIds, std::vector<std::string>{});
}
ASYNC_TEST("a sibling credential rotation during reset inventory fences the consume and serves on the next admission") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); std::vector<std::string> accountIds, authorizations;
  auto rotated = auth("two"); rotated.access_token = accessToken("two-rotated"); rotated.refresh_token = "refresh-two-rotated";
  WAIT(seedStableBankedResetBlock()); WAIT(seedStableBankedResetBlock("account-two"));
  ScriptedResetOptions scripted; scripted.onInventory = [rotated]() -> rt::Task<void> {
    const auto value = rt::KvValue(json::value_from(pool(auth("one"), rotated))); AWAIT(committed, kv->set(AUTH_KEY, value)); (void) committed;
  };
  const auto reset = scriptedResetProvider(scripted);
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
    accountIds.push_back(accountId(init)); authorizations.push_back(init.headers.value_or(rt::Headers{}).get("authorization").value_or(""));
    return jsonResponse(json::object{{"id", "rotated-sibling"}});
  }));
  const auto options = resetOptions(reset, "owner-reset-inventory-sibling-rotated", "reset-inventory-sibling-rotated");
  AWAIT(fenced, fetchCodexResponses(input("reset-inventory-sibling-rotated"), options)); EXPECT_EQ(fenced.status(), 429);
  AWAIT(body, fenced.json()); EXPECT_EQ(errorCode(body), std::optional<std::string>("codex_capacity_unavailable"));
  EXPECT_EQ(reset->redeemAccountIds, std::vector<std::string>{}); EXPECT_EQ(accountIds, std::vector<std::string>{});
  ca::resetCodexAuthCacheForTest(); cr::resetCodexAccountRoutingForTest(); AWAIT(served, fetchCodexResponses(input("rotated-sibling-after-fence")));
  EXPECT_EQ(served.status(), 200); EXPECT_EQ(accountIds, (std::vector<std::string>{"account-two"})); EXPECT_EQ(authorizations, (std::vector<std::string>{"Bearer " + rotated.access_token}));
  AWAIT(bytes, served.arrayBuffer()); (void) bytes;
}
ASYNC_TEST("a positive capacity observation during reset inventory fences the consume") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); std::vector<std::string> accountIds;
  WAIT(seedStableBankedResetBlock("account-one", fixedStartMs - 60'000)); WAIT(seedStableBankedResetBlock("account-two"));
  ScriptedResetOptions scripted; scripted.onInventory = [] {
    rs::CodexCapacityRoutingObservationInput observation; observation.slot = 0; observation.account_id = "account-one"; observation.state = "available";
    observation.source_observed_at_ms = fixedStartMs; observation.snapshot_at_ms = fixedStartMs;
    observation.windows.primary = rs::CodexCapacityRoutingWindow{10'800, 10, fixedStartMs + 10'800'000};
    std::vector<rs::CodexCapacityRoutingObservationInput> observations{observation};
    return cr::recordCodexCapacityRoutingObservations(observations, fixedStartMs);
  };
  const auto reset = scriptedResetProvider(scripted);
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) { accountIds.push_back(accountId(init)); return jsonResponse(json::object{{"id", "capacity-recovered"}}); }));
  const auto options = resetOptions(reset, "owner-reset-inventory-positive-capacity", "reset-inventory-positive-capacity");
  AWAIT(response, fetchCodexResponses(input("reset-inventory-positive-capacity"), options)); EXPECT_EQ(response.status(), 429);
  AWAIT(body, response.json()); EXPECT_EQ(errorCode(body), std::optional<std::string>("codex_capacity_unavailable"));
  EXPECT_EQ(reset->redeemAccountIds, std::vector<std::string>{}); EXPECT_EQ(rt::includes(reset->calls, std::string("redeem")), false); EXPECT_EQ(accountIds, std::vector<std::string>{});
  ca::resetCodexAuthCacheForTest(); cr::resetCodexAccountRoutingForTest(); AWAIT(served, fetchCodexResponses(input("capacity-recovered-next-admission")));
  EXPECT_EQ(served.status(), 200); ASSERT(!accountIds.empty()); EXPECT_EQ(accountIds.back(), "account-one"); AWAIT(bytes, served.arrayBuffer()); (void) bytes;
}
ASYNC_TEST("a whole-pool reorder during reset inventory fences the consume") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); std::vector<std::string> accountIds;
  WAIT(seedStableBankedResetBlock()); WAIT(seedStableBankedResetBlock("account-two")); ScriptedResetOptions scripted;
  scripted.onInventory = []() -> rt::Task<void> { const auto value = rt::KvValue(json::value_from(pool(auth("two"), auth("one")))); AWAIT(committed, kv->set(AUTH_KEY, value)); (void) committed; };
  const auto reset = scriptedResetProvider(scripted);
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) { accountIds.push_back(accountId(init)); return jsonResponse(json::object{{"id", "reordered-sibling-fallback"}}); }));
  const auto options = resetOptions(reset, "owner-reset-inventory-pool-reordered", "reset-inventory-pool-reordered");
  AWAIT(response, fetchCodexResponses(input("reset-inventory-pool-reordered"), options)); EXPECT_EQ(response.status(), 429);
  AWAIT(body, response.json()); EXPECT_EQ(errorCode(body), std::optional<std::string>("codex_quota_blocked"));
  EXPECT_EQ(reset->redeemAccountIds, std::vector<std::string>{}); EXPECT_EQ(rt::includes(reset->calls, std::string("redeem")), false); EXPECT_EQ(accountIds, std::vector<std::string>{});
}
ASYNC_TEST("definitive post-reset probe failures are returned directly without sibling replay") {
  const std::vector<double> statuses{401, 403, 429};
  for (const double status : statuses) {
    HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); std::vector<std::string> accountIds; const auto reset = scriptedResetProvider();
    WAIT(seedStableBankedResetBlock()); WAIT(seedStableBankedResetBlock("account-two"));
    rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
      accountIds.push_back(accountId(init)); rt::Headers headers; if (status == 429) headers.set("Retry-After", stableBankedResetRetryAfter());
      return jsonResponse(json::object{{"error", json::object{{"type", probeErrorType(status)}}}}, status, headers);
    }));
    const auto suffix = rt::numberToString(status); const auto options = resetOptions(reset, "owner-complete-probe-" + suffix, "complete-probe-" + suffix);
    AWAIT(response, fetchCodexResponses(input("complete-probe-" + suffix), options)); EXPECT_EQ(response.status(), status);
    EXPECT_EQ(accountIds.size(), 1U); EXPECT_EQ(reset->redeemAccountIds, accountIds); EXPECT_EQ(std::count(reset->calls.begin(), reset->calls.end(), "redeem"), 1);
    AWAIT(bytes, response.arrayBuffer()); (void) bytes;
  }
}
ASYNC_TEST("an ambiguous post-reset transport outcome never replays on a sibling") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); std::vector<std::string> accountIds; const auto reset = scriptedResetProvider();
  WAIT(seedStableBankedResetBlock()); WAIT(seedStableBankedResetBlock("account-two"));
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) -> rt::Task<rt::Response> {
    accountIds.push_back(accountId(init)); return rt::Promise<rt::Response>::reject(rt::makeReason(rt::DOMException("post-reset deadline elapsed", "TimeoutError"))).wait();
  }));
  const auto options = resetOptions(reset, "owner-complete-probe-transport-ambiguous", "complete-probe-transport-ambiguous");
  AWAIT(failure, failureOf(fetchCodexResponses(input("complete-probe-transport-ambiguous"), options))); EXPECT(failure);
  EXPECT_EQ(accountIds.size(), 1U); EXPECT_EQ(reset->redeemAccountIds, accountIds); EXPECT_EQ(std::count(reset->calls.begin(), reset->calls.end(), "redeem"), 1);
}
ASYNC_TEST("simultaneous all-exhausted requests share one consume and never replay it") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); Deferred redeemEntered, redeemGate;
  ScriptedResetOptions scripted; scripted.onRedeem = [&] { redeemEntered.resolve(); }; scripted.redeemGate = redeemGate.promise;
  const auto reset = scriptedResetProvider(scripted); std::vector<std::string> accountIds;
  WAIT(seedStableBankedResetBlock()); WAIT(seedStableBankedResetBlock("account-two"));
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
    const auto account = accountId(init); accountIds.push_back(account); return jsonResponse(json::object{{"id", "response-" + account}});
  }));
  auto firstOptions = resetOptions(reset, "owner-all-exhausted-concurrent", "all-exhausted-concurrent-first");
  rt::Promise<rt::Response> first(fetchCodexResponses(input("all-exhausted-concurrent-first"), firstOptions));
  co_await redeemEntered.promise.wait();
  const auto secondOptions = resetOptions(reset, "owner-all-exhausted-concurrent", "all-exhausted-concurrent-second");
  AWAIT(second, fetchCodexResponses(input("all-exhausted-concurrent-second"), secondOptions));
  EXPECT_EQ(second.status(), 429); EXPECT_EQ(reset->idempotencyKeys.size(), 1U); EXPECT_EQ(accountIds, std::vector<std::string>{});
  redeemGate.resolve(); auto firstResponse = co_await first.wait(); EXPECT_EQ(firstResponse.status(), 200); EXPECT_EQ(accountIds.size(), 1U);
  EXPECT_EQ(reset->idempotencyKeys.size(), 1U); EXPECT_EQ(reset->redeemAccountIds, accountIds);
}
ASYNC_TEST("a skipped half-open probe prevents a sibling banked-reset redemption") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); std::vector<std::string> accountIds; const auto reset = scriptedResetProvider();
  double now = fixedStartMs; rt::globalDateNow() = [&] { return now; };
  AWAIT(initial, cr::selectCodexRoutingAccounts(kv->auth, kv->auth.accounts, now)); const auto second = namedAccount(initial, "account-two");
  const auto expiredAtMs = now + 1'000; const auto expiredResponse = quota(rt::Date(expiredAtMs).toUTCString());
  WAIT(cr::markCodexQuotaBlocked(second, expiredResponse, now)); now = expiredAtMs + 1; cr::resetCodexAccountRoutingForTest();
  AWAIT(halfOpen, cr::selectCodexRoutingAccounts(kv->auth, kv->auth.accounts, now)); const auto foreignProbeCandidate = namedAccount(halfOpen, "account-two");
  ASSERT(foreignProbeCandidate.probeRequired); AWAIT(claimed, cr::claimCodexRoutingProbe(kv->auth, foreignProbeCandidate, now)); ASSERT(claimed);
  const auto first = namedAccount(initial, "account-one"); const auto blockedResponse = quota(); WAIT(cr::markCodexQuotaBlocked(first, blockedResponse, now));
  cr::resetCodexAccountRoutingForTest();
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) { accountIds.push_back(accountId(init)); return jsonResponse(json::object{{"id", "unexpected-dispatch"}}); }));
  auto options = resetOptions(reset, "owner-probe-unavailable"); options.bankedReset->now = [&] { return now; };
  AWAIT(response, fetchCodexResponses(input("banked-reset-probe-unavailable"), options));
  // A held recovery lease is retryable, never quota proof for redemption.
  EXPECT_EQ(response.status(), 429); EXPECT_EQ(reset->calls, std::vector<std::string>{}); EXPECT_EQ(accountIds, std::vector<std::string>{});
  AWAIT(bytes, response.arrayBuffer()); (void) bytes;
}
ASYNC_TEST("a 403 sibling blocks a full-pool banked reset") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); std::vector<std::string> accountIds; const auto reset = scriptedResetProvider();
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
    accountIds.push_back(accountId(init)); if (accountIds.size() == 1) return quota();
    if (accountIds.size() == 2) return jsonResponse(json::object{{"error", json::object{{"type", "forbidden"}}}}, 403);
    return jsonResponse(json::object{{"id", "response-after-reset"}});
  }));
  const auto options = resetOptions(reset, "owner-after-403"); AWAIT(response, fetchCodexResponses(input("banked-reset-after-403"), options));
  EXPECT_EQ(response.status(), 403); EXPECT_EQ(accountIds, (std::vector<std::string>{"account-one", "account-two"})); EXPECT_EQ(reset->calls, std::vector<std::string>{});
}
ASYNC_TEST("an earlier allowlisted exhausted account is redeemed after a later sibling also exhausts quota") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); std::vector<std::string> accountIds; const auto reset = scriptedResetProvider();
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
    accountIds.push_back(accountId(init)); return accountIds.size() <= 2 ? quota() : jsonResponse(json::object{{"id", "response-after-earlier-reset"}});
  }));
  const auto options = resetOptions(reset, "owner-earlier-allowlisted"); AWAIT(response, fetchCodexResponses(input("banked-reset-earlier-allowlisted"), options));
  EXPECT_EQ(response.status(), 200); EXPECT_EQ(accountIds, (std::vector<std::string>{"account-one", "account-two", "account-one"}));
  EXPECT_EQ(reset->redeemAccountIds, (std::vector<std::string>{"account-one"})); EXPECT_EQ(reset->calls, (std::vector<std::string>{"inventory", "inventory", "redeem", "verify"}));
}
ASYNC_TEST("a non-expired raw 403 during the bounded retry never quarantines credentials") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); std::vector<std::string> accountIds; const auto reset = scriptedResetProvider();
  const auto shortStableRetryAfter = rt::Date(fixedStartMs + 2'000).toUTCString();
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
    accountIds.push_back(accountId(init)); if (accountIds.size() <= 2) return quota(shortStableRetryAfter);
    if (accountIds.size() == 3) return jsonResponse(json::object{{"error", json::object{{"type", "forbidden"}}}}, 403);
    throw rt::JsError("a raw 403 is terminal and must not dispatch a fourth attempt");
  }));
  auto options = resetOptions(reset, "owner-bounded-retry-403"); options.retrySleep = resolvedSleep;
  AWAIT(response, fetchCodexResponses(input("banked-reset-bounded-retry-403"), options)); EXPECT_EQ(response.status(), 403);
  EXPECT_EQ(accountIds, (std::vector<std::string>{"account-one", "account-two", "account-two"})); EXPECT_EQ(reset->redeemAccountIds, std::vector<std::string>{});
  EXPECT_EQ(rt::includes(reset->calls, std::string("redeem")), false); const auto state = routing(); ASSERT(state); ASSERT(state->slots.size() >= 2);
  EXPECT_EQ(state->slots[0].invalid_credential_version, std::nullopt); EXPECT_EQ(state->slots[1].invalid_credential_version, std::nullopt);
}
ASYNC_TEST("a successful ordinary bounded retry never spends a banked reset") {
  HarnessScope scope; kv->auth = pool(auth("one")); const auto reset = scriptedResetProvider(); int inferenceCalls = 0;
  const auto shortStableRetryAfter = rt::Date(fixedStartMs + 2'000).toUTCString();
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) {
    ++inferenceCalls; return inferenceCalls == 1 ? quota(shortStableRetryAfter) : jsonResponse(json::object{{"id", "ordinary-retry-success"}});
  }));
  auto options = resetOptions(reset, "owner-ordinary-retry-success"); options.retrySleep = resolvedSleep;
  AWAIT(response, fetchCodexResponses(input("ordinary-retry-success"), options)); EXPECT_EQ(response.status(), 200); EXPECT_EQ(inferenceCalls, 2);
  EXPECT_EQ(reset->calls, std::vector<std::string>{});
}
ASYNC_TEST("only a complete stable usage-limit response can reach the banked-reset provider") {
  struct Case { std::string name; double status; std::string body; std::optional<std::string> retryAfter; double expectedStatus; };
  const auto stableRetryAfter = stableBankedResetRetryAfter(); const auto usage = std::string("{\"error\":{\"type\":\"usage_limit_reached\"}}");
  const std::vector<Case> cases{
    {"generic rate limit", 429, "{\"error\":{\"type\":\"rate_limit_error\"}}", stableRetryAfter, 429},
    {"burst throttling", 429, "{\"error\":{\"type\":\"requests_per_minute\"}}", stableRetryAfter, 429},
    {"overload", 429, "{\"error\":{\"type\":\"server_error\"}}", stableRetryAfter, 429},
    {"policy error", 429, "{\"error\":{\"type\":\"policy_error\"}}", stableRetryAfter, 429},
    {"invalid request", 429, "{\"error\":{\"type\":\"invalid_request_error\"}}", stableRetryAfter, 429},
    {"unknown future error type", 429, "{\"error\":{\"type\":\"future_quota_signal\"}}", stableRetryAfter, 429},
    {"malformed body", 429, "{not JSON", stableRetryAfter, 429},
    {"relative retry-after cannot name a reset window", 429, usage, "60", 429},
    {"invalid decimal retry-after", 429, usage, "0.5", 429},
    {"expired retry-after", 429, usage, rt::Date(fixedStartMs - 1'000).toUTCString(), 429},
    {"overflowing retry-after", 429, usage, "999999999999999999999999999999999999", 429},
    {"401", 401, "{\"error\":{\"type\":\"invalid_auth\"}}", std::nullopt, 401},
    {"403", 403, "{\"error\":{\"type\":\"forbidden\"}}", std::nullopt, 403}};
  for (const auto& testCase : cases) {
    HarnessScope scope; kv->auth = pool(auth("one")); const auto reset = scriptedResetProvider();
    rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo info, rt::RequestInit) {
      if (rt::includes(requestUrl(info), "oauth/token")) return jsonResponse(json::object{{"error", "invalid_grant"}}, 401);
      rt::Headers headers{{"Content-Type", "application/json"}}; if (testCase.retryAfter) headers.set("Retry-After", *testCase.retryAfter);
      return respond(testCase.body, testCase.status, headers);
    }));
    auto options = resetOptions(reset, "owner-nonqualifying-" + testCase.name); options.retrySleep = resolvedSleep;
    AWAIT(response, fetchCodexResponses(input("nonqualifying-" + testCase.name), options));
    EXPECT_EQ(response.status(), testCase.expectedStatus); EXPECT_EQ(reset->calls, std::vector<std::string>{});
  }
}
ASYNC_TEST("a later non-qualifying 429 clears an earlier banked-reset candidate") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); std::vector<std::string> accountIds; const auto reset = scriptedResetProvider();
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
    accountIds.push_back(accountId(init)); const auto type = accountIds.size() == 1 ? "usage_limit_reached" : "rate_limit_error";
    return jsonResponse(json::object{{"error", json::object{{"type", type}}}}, 429, rt::Headers{{"Retry-After", stableBankedResetRetryAfter()}});
  }));
  const auto options = resetOptions(reset, "owner-later-nonqualifying-429"); AWAIT(response, fetchCodexResponses(input("later-nonqualifying-429"), options));
  EXPECT_EQ(response.status(), 429); EXPECT_EQ(accountIds, (std::vector<std::string>{"account-one", "account-two"})); EXPECT_EQ(reset->calls, std::vector<std::string>{});
}
ASYNC_TEST("post-reset inference may return one normal 429 but never triggers a second redemption") {
  HarnessScope scope; kv->auth = pool(auth("one")); const auto reset = scriptedResetProvider(); int inferenceCalls = 0;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { ++inferenceCalls; return quota(); }));
  const auto options = resetOptions(reset, "owner-second-429"); AWAIT(response, fetchCodexResponses(input("banked-reset-second-429"), options));
  EXPECT_EQ(response.status(), 429); EXPECT_EQ(inferenceCalls, 2); EXPECT_EQ(reset->calls, (std::vector<std::string>{"inventory", "redeem", "verify"}));
}
