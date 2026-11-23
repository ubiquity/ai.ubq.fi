// Links: tests/codex_auth_cache_part4_test.cpp tests/helpers/codex_auth_cache_harness.cpp src/auth/relay.cpp src/authority/native_codex.cpp
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
// Port of tests/codex-auth-cache-part4.test.ts: all 5 original cases and assertions.
#include "tests/helpers/codex_auth_cache_harness.hpp"
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
  rt::Response jsonResponse(const json::value& body, double status = 200, rt::Headers headers = {}) {
    headers.set("Content-Type", "application/json");
    rt::ResponseInit init; init.status = status; init.headers = headers;
    return rt::Response(rt::BodyInit(rt::jsonStringify(body)), init);
  }
  rt::Response quota(const std::string& retryAfter) {
    return jsonResponse(json::object{{"error", json::object{{"type", "usage_limit_reached"}}}}, 429, rt::Headers{{"Retry-After", retryAfter}});
  }
  json::value input(const std::string& text) { return json::object{{"input", text}}; }
  std::string accountId(const rt::RequestInit& init) { return init.headers.value_or(rt::Headers{}).get("chatgpt-account-id").value_or(""); }
  cd::FetchCodexResponsesOptions resetOptions(const std::shared_ptr<ScriptedResetProvider>& provider, const std::string& owner) {
    auto options = bankedResetRequestOptions(""); options.requestId.reset(); options.clientVersion.reset();
    options.bankedReset->provider = provider; options.bankedReset->newOwnerToken = [owner] { return owner; }; return options;
  }
  std::optional<rs::CodexAccountRoutingState> routing() {
    const auto* entry = kv->extra.get(keyOf(cr::CODEX_ACCOUNT_ROUTING_KV_KEY));
    return entry ? cr::parseCodexAccountRoutingState(entry->value.toJson()) : std::nullopt;
  }
  class ActiveSwitchProvider : public ScriptedResetProvider {
  public:
    ActiveSwitchProvider() : ScriptedResetProvider(ScriptedResetOptions{}) {}
    rt::Task<json::value> verifyApplied(uos::codex::banked_reset_provider::ResetAccountContext, rt::AbortSignal) override {
      calls.push_back("verify");
      const auto* entry = kv->extra.get(keyOf(cr::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY));
      if (!entry) throw rt::JsError("expected a durable active selection");
      auto active = cr::parseCodexActiveAccountSelection(entry->value.toJson());
      if (!active) throw rt::JsError("expected a durable active selection");
      ++active->generation; active->updated_at_ms = fixedStartMs;
      auto next = rt::KvValue(rs::toJson(*active));
      rt::Promise pending(kv->set(cr::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, next));
      (void) co_await pending.wait();
      co_return true;
    }
  };
}
#define AWAIT(name, expression) rt::Promise name##Pending((expression)); auto name = co_await name##Pending.wait()
#define WAIT(expression) do { rt::Promise pending((expression)); co_await pending.wait(); } while (false)

ASYNC_TEST("a failed banked-reset probe gets a bounded retry and eventually reopens the account") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two"));
  const auto reset = scriptedResetProvider(); std::vector<std::string> inferenceAccounts;
  const double delayedPropagationResetAtMs = fixedStartMs + 7 * 24 * 60 * 60'000;
  const auto delayedPropagationRetryAfter = rt::Date(delayedPropagationResetAtMs).toUTCString();
  double now = fixedStartMs; rt::globalDateNow() = [&] { return now; };
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit init) {
    inferenceAccounts.push_back(accountId(init));
    return inferenceAccounts.size() > 5 ? jsonResponse(json::object{{"id", "recovered-after-propagation"}}) : quota(delayedPropagationRetryAfter);
  }));
  auto options = resetOptions(reset, "owner-failed-probe"); options.bankedReset->now = [&] { return now; };
  AWAIT(first, fetchCodexResponses(input("banked-reset-probe-fails"), options));
  EXPECT_EQ(first.status(), 429);
  EXPECT_EQ(inferenceAccounts, (std::vector<std::string>{"account-one", "account-two", "account-one"}));
  EXPECT_EQ(std::count(reset->calls.begin(), reset->calls.end(), "redeem"), 1);
  EXPECT_EQ(std::count(reset->calls.begin(), reset->calls.end(), "verify"), 1);
  const auto inferenceCountAfterFirst = inferenceAccounts.size(); const auto resetCallsAfterFirst = reset->calls;
  AWAIT(held, fetchCodexResponses(input("after-failed-bank-reset-probe"), options));
  EXPECT_EQ(held.status(), 429); EXPECT_EQ(inferenceAccounts.size(), inferenceCountAfterFirst); EXPECT_EQ(reset->calls, resetCallsAfterFirst);
  AWAIT(heldBytes, held.arrayBuffer()); (void) heldBytes;
  std::optional<rt::Response> recovered;
  for (int attempt = 0; attempt < 4; ++attempt) {
    now += cr::CODEX_HALF_OPEN_LEASE_MS + 1;
    AWAIT(response, fetchCodexResponses(input("during-reset-propagation-" + std::to_string(attempt)), options));
    if (response.status() == 200) { recovered = response; break; }
    EXPECT_EQ(response.status(), 429); ASSERT(!inferenceAccounts.empty()); EXPECT_EQ(inferenceAccounts.back(), "account-one");
    EXPECT_EQ(reset->calls, resetCallsAfterFirst);
    AWAIT(entry, kv->get(cr::CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvGetOptions{.consistency = rt::KvConsistencyLevel::strong}));
    const auto routingAfterProbe = cr::parseCodexAccountRoutingState(entry.value.toJson()); ASSERT(routingAfterProbe); ASSERT(!routingAfterProbe->slots.empty());
    EXPECT_EQ(routingAfterProbe->slots[0].quota_blocked_until_ms, std::optional(now + cr::CODEX_HALF_OPEN_LEASE_MS));
    EXPECT_EQ(routingAfterProbe->slots[0].banked_reset_generation_ambiguous, true);
    AWAIT(bytes, response.arrayBuffer()); (void) bytes;
  }
  ASSERT(recovered); EXPECT_EQ(reset->calls, resetCallsAfterFirst); WAIT(ca::markCodexResponseCompleted(*recovered));
  AWAIT(entry, kv->get(cr::CODEX_ACCOUNT_ROUTING_KV_KEY, rt::KvGetOptions{.consistency = rt::KvConsistencyLevel::strong}));
  const auto state = cr::parseCodexAccountRoutingState(entry.value.toJson()); ASSERT(state); ASSERT(state->slots.size() >= 2);
  EXPECT_EQ(state->slots[0].quota_blocked_until_ms, std::nullopt); EXPECT_EQ(state->slots[0].banked_reset_generation_ambiguous, false);
  EXPECT_EQ(state->slots[1].quota_blocked_until_ms, std::optional(delayedPropagationResetAtMs));
}

ASYNC_TEST("a stale verified reset recovers the existing account without another reset") {
  HarnessScope scope; kv->auth = pool(auth("one")); const auto reset = scriptedResetProvider(); int inferenceCalls = 0;
  const double delayedResetAtMs = fixedStartMs + 7 * 24 * 60 * 60'000;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) {
    ++inferenceCalls; return inferenceCalls <= 2 ? quota(stableBankedResetRetryAfter()) : jsonResponse(json::object{{"id", "recovered-stale-reset"}});
  }));
  auto options = resetOptions(reset, "owner-stale-verified-reset");
  AWAIT(first, fetchCodexResponses(input("seed-stale-verified-reset"), options));
  EXPECT_EQ(first.status(), 429); EXPECT_EQ(inferenceCalls, 2); EXPECT_EQ(reset->calls, (std::vector<std::string>{"inventory", "redeem", "verify"}));
  auto current = routing(); ASSERT(current); ASSERT(!current->slots.empty());
  current->slots.resize(1); current->slots[0].quota_blocked_until_ms = delayedResetAtMs;
  ++current->slots[0].generation; current->slots[0].probe_lease.reset(); current->slots[0].banked_reset_generation_ambiguous = true;
  auto next = rt::KvValue(rs::toJson(*current)); AWAIT(committed, kv->set(cr::CODEX_ACCOUNT_ROUTING_KV_KEY, next)); (void) committed;
  cr::resetCodexAccountRoutingForTest();
  AWAIT(recovered, fetchCodexResponses(input("recover-stale-verified-reset"), options));
  EXPECT_EQ(recovered.status(), 200); EXPECT_EQ(inferenceCalls, 3); EXPECT_EQ(reset->calls, (std::vector<std::string>{"inventory", "redeem", "verify"}));
  WAIT(ca::markCodexResponseCompleted(recovered));
}

ASYNC_TEST("all-blocked routing recovers an unknown reset while new submissions are disabled") {
  HarnessScope scope; kv->auth = pool(auth("one")); ScriptedResetOptions scripted; scripted.redeemKind = "unknown";
  const auto reset = scriptedResetProvider(scripted); double now = fixedStartMs; bool live = true; int inferenceCalls = 0;
  rt::globalDateNow() = [&] { return now; };
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) {
    ++inferenceCalls; return inferenceCalls == 1 ? quota(stableBankedResetRetryAfter()) : jsonResponse(json::object{{"id", "recovered-after-unknown"}});
  }));
  auto options = resetOptions(reset, "owner-recovery"); const auto liveConfig = liveBankedResetConfig();
  auto disabledConfig = liveConfig; disabledConfig.enabled = false; disabledConfig.mode = "disabled";
  options.bankedReset->reloadConfig = [&] { return live ? liveConfig : disabledConfig; }; options.bankedReset->now = [&] { return now; };
  AWAIT(first, fetchCodexResponses(input("unknown-reset"), options));
  EXPECT_EQ(first.status(), 429); EXPECT_EQ(reset->calls, (std::vector<std::string>{"inventory", "redeem"}));
  now += uos::codex::banked_reset::CODEX_BANKED_RESET_LEASE_MS + 1; live = false;
  AWAIT(recovered, fetchCodexResponses(input("recover-reset"), options));
  EXPECT_EQ(recovered.status(), 200); EXPECT_EQ(inferenceCalls, 2);
  EXPECT_EQ(reset->calls, (std::vector<std::string>{"inventory", "redeem", "lookup", "verify"}));
}

ASYNC_TEST("an all-blocked cohort with credit only on the inactive sibling redeems it in one request") {
  HarnessScope scope; kv->auth = pool(auth("one"), auth("two")); std::vector<std::string> consumeAccountIds, inferenceAccountIds;
  uos::config::config.codexBaseUrl = "https://upstream-reset.test/backend-api/codex";
  WAIT(seedStableBankedResetBlock("account-one"));
  AWAIT(bootstrapped, cr::selectCodexRoutingAccountsStrong(kv->auth, kv->auth.accounts, fixedStartMs));
  const auto* eligible = std::get_if<rs::RouteSelectionEligible>(&bootstrapped); ASSERT(eligible); ASSERT(!eligible->accounts.empty());
  EXPECT_EQ(eligible->accounts[0].auth.account_id, "account-two"); WAIT(seedStableBankedResetBlock("account-two"));
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo info, rt::RequestInit init) {
    const auto url = requestUrl(info); const auto account = accountId(init);
    if (rt::endsWith(url, "/backend-api/codex/responses")) {
      inferenceAccountIds.push_back(account); return jsonResponse(json::object{{"id", "response-" + account}});
    }
    if (rt::endsWith(url, "/backend-api/wham/rate-limit-reset-credits")) {
      json::array credits;
      if (account == "account-one") credits.push_back(json::object{{"id", "credit-account-one"}, {"status", "available"}, {"reset_type", "codex_rate_limits"}, {"expires_at", nullptr}});
      return jsonResponse(json::object{{"available_count", account == "account-one" ? 1 : 0}, {"credits", credits}});
    }
    if (rt::endsWith(url, "/backend-api/wham/rate-limit-reset-credits/consume")) {
      consumeAccountIds.push_back(account); return jsonResponse(json::object{{"code", "reset"}, {"windows_reset", 1}});
    }
    throw rt::JsError("unexpected request " + init.method.value_or("GET") + " " + url);
  }));
  const auto options = bankedResetRequestOptions("inactive-sibling-credit");
  AWAIT(consumed, fetchCodexResponses(input("inactive-sibling-credit"), options));
  EXPECT_EQ(consumed.status(), 200); EXPECT(ca::getCodexRoutingProbe(consumed));
  EXPECT_EQ(consumeAccountIds, (std::vector<std::string>{"account-one"})); EXPECT_EQ(inferenceAccountIds, (std::vector<std::string>{"account-one"}));
  const auto* entry = kv->extra.get(keyOf(cr::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY)); ASSERT(entry);
  const auto active = cr::parseCodexActiveAccountSelection(entry->value.toJson()); ASSERT(active);
  EXPECT_EQ(active->slot, 0); EXPECT_EQ(active->generation, 2); WAIT(ca::markCodexResponseCompleted(consumed));
}

ASYNC_TEST("a concurrent active switch during a verified reset prevents stale recovery inference") {
  HarnessScope scope; kv->auth = pool(auth("one")); const auto reset = std::make_shared<ActiveSwitchProvider>(); int inferenceCalls = 0;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { ++inferenceCalls; return quota(stableBankedResetRetryAfter()); }));
  auto options = resetOptions(reset, "owner-concurrent-active-switch"); options.requestId = "concurrent-active-switch";
  AWAIT(response, fetchCodexResponses(input("concurrent-active-switch"), options));
  EXPECT_EQ(response.status(), 429); EXPECT_EQ(ca::getCodexRoutingError(response), std::optional<std::string>(ca::CODEX_QUOTA_BLOCKED_ERROR_CODE));
  EXPECT_EQ(inferenceCalls, 1); EXPECT_EQ(ca::getCodexRoutingProbe(response), std::nullopt);
  EXPECT_EQ(reset->calls, (std::vector<std::string>{"inventory", "redeem", "verify"})); AWAIT(bytes, response.arrayBuffer()); (void) bytes;
}
