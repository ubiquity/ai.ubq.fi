// Links: tests/codex_overage_fallback_test.cpp tests/helpers/codex_auth_cache_harness.cpp src/auth/relay.cpp src/authority/native_codex.cpp
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
// Port of tests/codex-overage-fallback.test.ts: all 8 original cases and assertions.
#include "tests/helpers/codex_auth_cache_harness.hpp"
#include "uos/codex/overage_settings.hpp"
#include "uos/provider/selection.hpp"
#include "uos/utils.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <functional>
#include <utility>
#include <variant>

#define AWAIT(name, expression) rt::Promise name##Pending((expression)); auto name = co_await name##Pending.wait()
#define WAIT(expression) do { rt::Promise pending((expression)); co_await pending.wait(); } while (false)
namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace h = uos::tests::helpers::codex_auth_cache_harness;
  namespace ca = uos::codex::auth;
  namespace cr = uos::codex::account_routing;
  namespace rs = uos::codex::routing_state;
  namespace os = uos::codex::overage_settings;
  namespace br = uos::codex::banked_reset;
  namespace selection = uos::provider::selection;
  using namespace h;
  using uos::codex::index::fetchCodexResponses;
  struct UpstreamCall {
    std::string kind;
    std::string accountId;
    friend bool operator==(const UpstreamCall&, const UpstreamCall&) = default;
  };
  struct BlockedRun { std::vector<rt::Response> responses; std::vector<UpstreamCall> calls; std::vector<std::string> logs; };
  struct RunOptions {
    std::variant<bool, std::vector<bool>> allowOverage = false;
    std::string mode = "live";
    bool seedUnsubmittedReset = false;
    bool holdClaimLease = false;
    bool includeSibling = false;
    bool siblingSubmitUnknown = false;
    bool firstDayCapped = false;
    bool expandSelectionDuringInventory = false;
    std::function<rt::Response()> firstInventory;
    bool parallel = false;
    std::function<rt::Response()> inferenceResponse;
    bool allows(std::size_t index) const {
      if (const auto* allow = std::get_if<bool>(&allowOverage)) return *allow;
      const auto& allow = std::get<std::vector<bool>>(allowOverage); return index < allow.size() ? allow[index] : false;
    }
  };
  rt::Response jsonResponse(const json::value& body) { return rt::Response::json(body); }
  rt::Response emptyInventory() { return jsonResponse(json::object{{"available_count", 0}, {"credits", json::array{}}}); }
  rt::Task<rt::Response> enableAllSubscriptionsThenReturnEmptyInventory() {
    std::vector<std::string> ids{"codex"};
    const auto saved = co_await selection::storeProviderSelection(kv, std::move(ids));
    ASSERT(saved);
    co_return emptyInventory();
  }
  rt::Response availableInventory() {
    return jsonResponse(json::object{{"available_count", 1}, {"credits", json::array{json::object{{"id", "credit-account-one"}, {"status", "available"}, {"reset_type", "codex_rate_limits"}, {"expires_at", nullptr}}}}});
  }
  rt::Response creditInventory(const std::string& id, double expiresAtMs) {
    return jsonResponse(json::object{{"available_count", 1},
      {"credits", json::array{json::object{{"id", id}, {"status", "available"}, {"reset_type", "codex_rate_limits"},
                                         {"expires_at", rt::Date(expiresAtMs).toISOString()}}}}});
  }
  rt::Task<BlockedRun> runBlockedSequence(std::vector<std::string> requestIds, std::function<rt::Response()> inventoryResponse, RunOptions options) {
    HarnessScope scope; uos::config::config.codexBaseUrl = "https://upstream-reset.test/backend-api/codex";
    kv->auth = options.includeSibling ? pool(auth("one"), auth("two")) : pool(auth("one")); os::resetOverageUsageCacheForTest();
    auto cleanup = rt::scopeExit([] {
      os::resetOverageUsageCacheForTest();
      selection::resetProviderSelectionCacheForTest();
    });
    std::vector<UpstreamCall> calls; WAIT(seedStableBankedResetBlock("account-one"));
    if (options.includeSibling) WAIT(seedStableBankedResetBlock("account-two"));
    if (options.expandSelectionDuringInventory) {
      AWAIT(hash, selection::codexSubscriptionHash("account-one"));
      std::vector ids{selection::codexSubscriptionSelectionId(hash)};
      AWAIT(saved, selection::storeProviderSelection(kv, std::move(ids)));
      ASSERT(saved);
    }
    if (options.firstDayCapped) {
      AWAIT(accountHash, uos::utils::sha256Hex("account-one"));
      const auto day = br::utcDay(fixedStartMs);
      ASSERT(day);
      const auto key = br::codexResetAccountDailyKey(accountHash, *day);
      const json::object budget{{"v", 1}, {"day", *day}, {"submission_count", 1}, {"updated_at_ms", fixedStartMs - 1'000}};
      WAIT(kv->set(key, rt::KvValue(budget)));
    }
    if (options.seedUnsubmittedReset) {
      AWAIT(routing, cr::selectCodexRoutingAccountsStrong(kv->auth, kv->auth.accounts, fixedStartMs));
      const auto* quotaBlocked = std::get_if<rs::RouteSelectionQuotaBlocked>(&routing); ASSERT(quotaBlocked); ASSERT(!quotaBlocked->blockedAccounts.empty());
      for (const auto& blocked : quotaBlocked->blockedAccounts) {
        if (blocked.slot == 1 && !options.siblingSubmitUnknown) continue;
        const bool unknown = blocked.slot == 1;
        ASSERT(blocked.routingGeneration); AWAIT(accountHash, uos::utils::sha256Hex(blocked.auth.account_id));
        const std::string separator(1, '\0');
        const auto credentialVersionInput = "uos_ai" + separator + "codex_reset_credential_version" + separator + blocked.credentialVersion;
        AWAIT(credentialHash, uos::utils::sha256Hex(credentialVersionInput)); const auto credentialVersion = "v1:" + credentialHash;
        const auto quotaGenerationInput = "uos_ai" + separator + "codex_reset_generation" + separator + accountHash + separator + rt::numberToString(blocked.quotaResetAtMs);
        AWAIT(quotaHash, uos::utils::sha256Hex(quotaGenerationInput)); const auto quotaGeneration = "v1:" + quotaHash;
        const auto idempotencyInput = "uos_ai" + separator + "codex_reset_idempotency" + separator + accountHash + separator + quotaGeneration;
        AWAIT(idempotencyHash, uos::utils::sha256Hex(idempotencyInput)); const auto idempotencyKey = "uos_ai_codex_reset_v1_" + idempotencyHash;
        AWAIT(idempotencyKeyHash, uos::utils::sha256Hex(idempotencyKey));
        uos::types::CodexResetRedemptionRecord record{
          .v = 1, .account_id_hash = accountHash, .credential_version = credentialVersion, .quota_generation = quotaGeneration,
          .routing_generation = *blocked.routingGeneration, .idempotency_key_hash = idempotencyKeyHash,
          .state = unknown ? uos::types::CodexResetRedemptionState::unknown : uos::types::CodexResetRedemptionState::claimed,
          .owner_token = "fixture-stale-owner", .fence = 1,
          .lease_expires_at_ms = fixedStartMs + (unknown || options.holdClaimLease ? 30'000 : -1),
          .provider_receipt_id = std::nullopt, .created_at_ms = fixedStartMs - 1'000,
          .updated_at_ms = fixedStartMs - 1'000, .submitted_at_ms = unknown ? std::optional<double>(fixedStartMs - 1'000) : std::nullopt,
          .verified_at_ms = std::nullopt, .last_error_code = unknown ? std::optional<std::string>("provider_commit_unknown") : std::nullopt};
        const auto key = br::codexResetRedemptionKey(accountHash, quotaGeneration); const auto value = rt::KvValue(json::value_from(record));
        AWAIT(committed, kv->set(key, value)); (void) committed;
      }
    }
    bool selectionExpanded = false;
    rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo info, rt::RequestInit init) -> rt::Task<rt::Response> {
      const auto url = requestUrl(info); const auto account = init.headers.value_or(rt::Headers{}).get("chatgpt-account-id").value_or("");
      if (rt::endsWith(url, "/backend-api/codex/responses")) {
        calls.push_back({"inference", account});
        return rt::fulfilledTask(options.inferenceResponse ? options.inferenceResponse() : jsonResponse(json::object{{"id", "response-" + account}}));
      }
      if (rt::endsWith(url, "/backend-api/wham/rate-limit-reset-credits")) {
        calls.push_back({"inventory", account});
        if (options.includeSibling && account == "account-one") {
          if (options.expandSelectionDuringInventory && !std::exchange(selectionExpanded, true))
            return enableAllSubscriptionsThenReturnEmptyInventory();
          return rt::fulfilledTask(options.firstInventory ? options.firstInventory() : emptyInventory());
        }
        return rt::fulfilledTask(inventoryResponse());
      }
      if (rt::endsWith(url, "/backend-api/wham/rate-limit-reset-credits/consume")) {
        calls.push_back({"consume", account});
        return rt::fulfilledTask(jsonResponse(json::object{{"code", "reset"}, {"windows_reset", 1}}));
      }
      throw rt::JsError("unexpected request " + init.method.value_or("GET") + " " + url);
    }));
    rt::console::Capture capture({rt::console::Method::info});
    auto requestOptionsFor = [&](const std::string& requestId) { auto result = bankedResetRequestOptions(requestId); result.bankedReset->config->mode = options.mode; return result; };
    std::vector<rt::Response> responses;
    if (options.parallel) {
      AWAIT(accountHash, uos::utils::sha256Hex("account-one")); WAIT(os::writeOverageUsage(kv, accountHash, options.allows(0)));
      WAIT(os::loadOverageUsageSettings(kv->auth, true)); std::vector<rt::Promise<rt::Response>> pending;
      for (const auto& requestId : requestIds) { auto body = json::value(json::object{{"input", requestId}}); auto requestOptions = requestOptionsFor(requestId); pending.emplace_back(fetchCodexResponses(body, requestOptions)); }
      responses = co_await rt::all(std::move(pending)).wait();
    } else {
      for (std::size_t index = 0; index < requestIds.size(); ++index) {
        AWAIT(accountHash, uos::utils::sha256Hex("account-one")); WAIT(os::writeOverageUsage(kv, accountHash, options.allows(index)));
        WAIT(os::loadOverageUsageSettings(kv->auth, true)); const auto body = json::value(json::object{{"input", requestIds[index]}}); const auto requestOptions = requestOptionsFor(requestIds[index]);
        AWAIT(response, fetchCodexResponses(body, requestOptions)); responses.push_back(response);
      }
    }
    std::vector<std::string> logs; for (const auto& call : capture.calls()) logs.push_back(call.text);
    co_return BlockedRun{std::move(responses), std::move(calls), std::move(logs)};
  }
  rt::Task<BlockedRun> runBlockedRequest(std::string requestId, std::function<rt::Response()> inventory, bool allowOverage = false) {
    std::vector<std::string> ids{std::move(requestId)}; RunOptions options; options.allowOverage = allowOverage;
    return runBlockedSequence(std::move(ids), std::move(inventory), std::move(options));
  }
  std::vector<UpstreamCall> callsOf(const BlockedRun& run, const std::string& kind) {
    std::vector<UpstreamCall> selected; for (const auto& call : run.calls) if (call.kind == kind) selected.push_back(call); return selected;
  }
  std::vector<std::string> accountsOf(const BlockedRun& run, const std::string& kind) {
    std::vector<std::string> selected; for (const auto& call : run.calls) if (call.kind == kind) selected.push_back(call.accountId); return selected;
  }
  std::vector<double> statuses(const BlockedRun& run) {
    std::vector<double> result; for (const auto& response : run.responses) result.push_back(response.status()); return result;
  }
  bool logged(const BlockedRun& run, const std::string& event, const std::optional<std::string>& reason = std::nullopt) {
    return std::any_of(run.logs.begin(), run.logs.end(), [&](const auto& line) { return rt::includes(line, event) && (!reason || rt::includes(line, "\"reason\":\"" + *reason + "\"")); });
  }
}

ASYNC_TEST("the default off setting refuses overage even when no reset is redeemable") {
  AWAIT(run, runBlockedRequest("overage-fallback-empty-inventory", emptyInventory)); ASSERT(!run.responses.empty());
  EXPECT_EQ(run.responses[0].status(), 429); EXPECT_EQ(ca::getCodexRoutingError(run.responses[0]), std::optional<std::string>("codex_quota_blocked"));
  EXPECT_EQ(callsOf(run, "inference"), std::vector<UpstreamCall>{}); EXPECT_EQ(callsOf(run, "consume"), std::vector<UpstreamCall>{});
  EXPECT_EQ(logged(run, "codex_overage_served"), false);
}
ASYNC_TEST("an explicitly allowed blocked cohort still falls back to exactly one bounded overage probe") {
  AWAIT(run, runBlockedRequest("overage-fallback-allowed-empty-inventory", emptyInventory, true)); ASSERT(!run.responses.empty());
  EXPECT_EQ(run.responses[0].status(), 200); EXPECT_EQ(accountsOf(run, "inference"), (std::vector<std::string>{"account-one"}));
  EXPECT_EQ(callsOf(run, "consume"), std::vector<UpstreamCall>{}); EXPECT(logged(run, "codex_overage_served", "no_eligible_credit"));
}
ASYNC_TEST("a selected shadow reset allows the enabled bounded overage probe") {
  RunOptions options; options.allowOverage = true; options.mode = "shadow"; const std::vector<std::string> ids{"overage-shadow-first"};
  AWAIT(run, runBlockedSequence(ids, availableInventory, options)); EXPECT_EQ(statuses(run), (std::vector<double>{200}));
  EXPECT_EQ(accountsOf(run, "inference"), (std::vector<std::string>{"account-one"})); EXPECT_EQ(callsOf(run, "consume"), std::vector<UpstreamCall>{});
  EXPECT(logged(run, "codex_overage_served", "shadow_selected"));
}
ASYNC_TEST("a duplicate shadow decision allows the enabled bounded overage probe") {
  RunOptions options; options.allowOverage = std::vector<bool>{false, true}; options.mode = "shadow";
  const std::vector<std::string> ids{"overage-shadow-seed", "overage-shadow-duplicate"}; AWAIT(run, runBlockedSequence(ids, availableInventory, options));
  EXPECT_EQ(statuses(run), (std::vector<double>{429, 200})); EXPECT_EQ(accountsOf(run, "inference"), (std::vector<std::string>{"account-one"}));
  EXPECT_EQ(callsOf(run, "consume"), std::vector<UpstreamCall>{}); EXPECT(logged(run, "codex_overage_served", "already_would_spend_once"));
}
ASYNC_TEST("an unsubmitted stale reset claim allows the enabled bounded overage probe") {
  RunOptions options; options.allowOverage = true; options.seedUnsubmittedReset = true; const std::vector<std::string> ids{"overage-unsubmitted-reset"};
  AWAIT(run, runBlockedSequence(ids, availableInventory, options)); ASSERT(!run.responses.empty()); EXPECT_EQ(run.responses[0].status(), 200);
  EXPECT_EQ(accountsOf(run, "inference"), (std::vector<std::string>{"account-one"})); EXPECT_EQ(callsOf(run, "consume"), std::vector<UpstreamCall>{});
  EXPECT(logged(run, "codex_overage_served", "unsubmitted_transaction"));
}
ASYNC_TEST("concurrent enabled overage requests do not contend for one probe lease") {
  RunOptions options; options.allowOverage = true; options.seedUnsubmittedReset = true; options.parallel = true;
  const std::vector<std::string> ids{"overage-concurrent-one", "overage-concurrent-two"}; AWAIT(run, runBlockedSequence(ids, availableInventory, options));
  EXPECT_EQ(statuses(run), (std::vector<double>{200, 200})); EXPECT_EQ(accountsOf(run, "inference"), (std::vector<std::string>{"account-one", "account-one"}));
  EXPECT_EQ(std::count_if(run.logs.begin(), run.logs.end(), [](const auto& line) { return rt::includes(line, "codex_overage_served"); }), 2);
}
ASYNC_TEST("an unsubmitted reset claim on one account leaves a sibling credit eligible") {
  RunOptions options; options.allowOverage = true; options.seedUnsubmittedReset = true; options.includeSibling = true;
  options.firstInventory = [] { return creditInventory("earlier-credit-one", fixedStartMs + 20'000); };
  const std::vector<std::string> ids{"overage-unsubmitted-sibling-credit"};
  AWAIT(run, runBlockedSequence(ids, [] { return creditInventory("later-credit-two", fixedStartMs + 40'000); }, options));
  EXPECT_EQ(statuses(run), (std::vector<double>{200}));
  EXPECT_EQ(accountsOf(run, "inventory"), (std::vector<std::string>{"account-two"}));
  EXPECT_EQ(accountsOf(run, "consume"), (std::vector<std::string>{"account-two"}));
  EXPECT_EQ(accountsOf(run, "inference"), (std::vector<std::string>{"account-two"}));
  EXPECT(!logged(run, "codex_overage_served"));
}
ASYNC_TEST("a live local claim prevents sibling redemption and overage") {
  RunOptions options;
  options.allowOverage = true;
  options.seedUnsubmittedReset = true;
  options.holdClaimLease = true;
  options.includeSibling = true;
  const std::vector<std::string> ids{"overage-live-claim-sibling"};
  AWAIT(run, runBlockedSequence(ids, availableInventory, options));
  EXPECT_EQ(statuses(run), (std::vector<double>{429}));
  EXPECT(run.calls.empty());
  EXPECT(!logged(run, "codex_overage_served"));
}
ASYNC_TEST("an earlier credit on a day-capped account yields to the redeemable sibling before overage") {
  RunOptions options;
  options.allowOverage = true;
  options.includeSibling = true;
  options.firstDayCapped = true;
  options.firstInventory = [] { return creditInventory("earlier-credit-one", fixedStartMs + 20'000); };
  const std::vector<std::string> ids{"overage-day-capped-sibling-credit"};
  AWAIT(run, runBlockedSequence(ids, [] { return creditInventory("later-credit-two", fixedStartMs + 40'000); }, options));
  EXPECT_EQ(statuses(run), (std::vector<double>{200}));
  const std::vector<UpstreamCall> expected{{"inventory", "account-two"}, {"consume", "account-two"}, {"inference", "account-two"}};
  EXPECT_EQ(run.calls, expected);
  EXPECT(!logged(run, "codex_overage_served"));
}
ASYNC_TEST("an unsubmitted reset claim does not bypass a sibling's unknown submission") {
  RunOptions options; options.allowOverage = true; options.seedUnsubmittedReset = true; options.includeSibling = true; options.siblingSubmitUnknown = true;
  const std::vector<std::string> ids{"overage-unsubmitted-sibling-unknown"};
  AWAIT(run, runBlockedSequence(ids, availableInventory, options));
  EXPECT_EQ(statuses(run), (std::vector<double>{429}));
  EXPECT(run.calls.empty());
  EXPECT(!logged(run, "codex_overage_served"));
}
ASYNC_TEST("shadow-only reset outcomes still refuse overage when the account setting is off") {
  RunOptions options; options.mode = "shadow"; const std::vector<std::string> ids{"overage-shadow-disabled"};
  AWAIT(run, runBlockedSequence(ids, availableInventory, options)); ASSERT(!run.responses.empty()); EXPECT_EQ(run.responses[0].status(), 429);
  EXPECT_EQ(callsOf(run, "inference"), std::vector<UpstreamCall>{}); EXPECT_EQ(callsOf(run, "consume"), std::vector<UpstreamCall>{});
  EXPECT(logged(run, "codex_banked_reset_preflight", "shadow_selected")); EXPECT_EQ(logged(run, "codex_overage_served"), false);
}
ASYNC_TEST("a redeemable credit is consumed before overage") {
  AWAIT(run, runBlockedRequest("overage-fallback-live-credit", availableInventory, true)); ASSERT(!run.responses.empty()); EXPECT_EQ(run.responses[0].status(), 200);
  EXPECT_EQ(callsOf(run, "consume"), (std::vector<UpstreamCall>{{"consume", "account-one"}}));
  EXPECT_EQ(accountsOf(run, "inference"), (std::vector<std::string>{"account-one"})); EXPECT(logged(run, "codex_reset_verified"));
  EXPECT_EQ(logged(run, "codex_overage_served"), false);
}

ASYNC_TEST("a transient upstream overage failure cannot authorize paid fallback") {
  RunOptions options;
  options.allowOverage = true;
  options.inferenceResponse = [] { return rt::Response("subscription upstream unavailable", rt::ResponseInit{.status = 503}); };
  const std::vector<std::string> ids{"overage-transient-failure"};
  AWAIT(run, runBlockedSequence(ids, emptyInventory, options));
  ASSERT(run.responses.size() == 1);
  EXPECT_EQ(run.responses.front().status(), 503);
  EXPECT(!ca::getCodexRoutingError(run.responses.front()));
  AWAIT(body, run.responses.front().text());
  EXPECT_EQ(body, "subscription upstream unavailable");
  EXPECT_EQ(accountsOf(run, "inference"), (std::vector<std::string>{"account-one"}));
  EXPECT(!logged(run, "codex_overage_served"));
}

ASYNC_TEST("a sibling selected during inventory gets its reset before overage") {
  RunOptions options;
  options.allowOverage = true;
  options.includeSibling = true;
  options.expandSelectionDuringInventory = true;
  const std::vector<std::string> ids{"overage-newly-selected-sibling"};
  AWAIT(run, runBlockedSequence(ids, availableInventory, options));
  ASSERT(!run.calls.empty());
  EXPECT_EQ(run.calls.front(), (UpstreamCall{"inventory", "account-one"}));
  EXPECT_EQ(statuses(run), (std::vector<double>{200}));
  EXPECT_EQ(accountsOf(run, "consume"), (std::vector<std::string>{"account-two"}));
  EXPECT_EQ(accountsOf(run, "inference"), (std::vector<std::string>{"account-two"}));
  EXPECT(!logged(run, "codex_overage_served"));
}
