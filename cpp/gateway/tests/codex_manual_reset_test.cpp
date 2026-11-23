// Links: tests/codex_manual_reset_test.cpp
//        src/config.cpp src/cache/kv_migration_validation.cpp src/codex/manual_reset_state.cpp src/cache/kv_migration_paid_fallback.cpp
//        src/authority/native_api_key.cpp tests/helpers/counting_kv.cpp src/cache/kv_migration_usage.cpp src/authority/native_paid_policy.cpp
//        src/kv_conversion/profile_json.cpp src/authority/native_api_key_policy.cpp src/api_keys.cpp src/authority/native_kernel.cpp
//        src/authority/native_kernel_contracts.cpp src/api_key_policy.cpp src/types.cpp src/cache/kv_migration_base.cpp
//        src/kv.cpp src/utils.cpp src/kernel/quota_v2.cpp src/kernel/quota_reservations.cpp
//        src/codex/banked_reset.cpp src/runtime_config.cpp src/release.cpp src/authority/native_paid.cpp
//        src/models/codex_models.cpp src/storage/kv.cpp src/http.cpp src/storage/sqlite_readonly.cpp
//        src/defaults.cpp src/storage/native_kv.cpp src/codex/banked_reset_provider.cpp src/codex/reset_settings.cpp
//        src/provider/cerebras_rate_limits.cpp src/auth/relay.cpp src/codex/auth.cpp
//        src/codex/capacity_routing.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_capacity.cpp src/codex/routing_state.cpp
//        src/codex/routing_mutations.cpp src/authority/native_codex_mutations.cpp src/authority/native_codex_evaluation.cpp src/codex/native_auth.cpp
//        src/codex/external_auth.cpp src/codex/rate_limit_429.cpp src/provider/health.cpp src/authority/native_codex_429.cpp
//        src/codex/native_auth_transport.cpp src/bounded_response_body.cpp src/runtime/websocket.cpp src/provider/claude_code_accounts.cpp
//        src/provider/claude_code_credentials.cpp tests/helpers/codex_diff_kv.cpp src/authority/native_codex.cpp src/authority/native_paid_contracts.cpp
//        src/cache/prompt_analytics.cpp src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp
//        src/cache/telemetry_gate.cpp src/codex/account_routing.cpp src/codex/auth_refresh.cpp src/codex/dispatch.cpp
//        src/codex/experiment.cpp src/codex/overage_settings.cpp src/codex/routing_evaluation.cpp src/codex/routing_serial.cpp
//        src/inference_deadline.cpp src/metered_quota.cpp src/models/codex_models_availability.cpp src/optional_telemetry_queue.cpp
//        src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp
//        src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp
//        src/provider/capacity_rollups.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code_quota.cpp
//        src/provider/claude_code_usage_schema.cpp src/provider/selection.cpp src/runtime/http.cpp src/runtime/http_body.cpp
//        src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp src/storage/key_compatibility.cpp
//        src/storage/value_compatibility.cpp tests/helpers/codex_banked_reset_harness.cpp src/codex/banked_reset_claim.cpp src/codex/banked_reset_submission.cpp
//        src/codex/banked_reset_pool.cpp src/codex/manual_reset.cpp tests/helpers/manual_reset_harness.cpp
// Port of all thirteen cases in tests/codex-manual-reset.test.ts.
#include "tests/helpers/manual_reset_harness.hpp"
#include "uos/codex/manual_reset.hpp"
#include "uos/codex/account_routing.hpp"
#include "uos/codex/reset_settings.hpp"
#include "uos/codex/routing_state.hpp"
#include "uos/codex/banked_reset_submission.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/test_async.hpp"
namespace rt = uos::rt;
namespace json = uos::json;
namespace harness = uos::tests::helpers::manual_reset_harness;
namespace banked = uos::tests::helpers::codex_banked_reset_harness;
namespace reset = uos::codex::manual_reset;
namespace routing = uos::codex::routing_state;
namespace selection = uos::provider::selection;
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
#define WRITE(store, key, value) do { const rt::KvValue writeValue(value); auto writeTask = (store)->set((key), writeValue); co_await std::move(writeTask); } while (false)
namespace {
  rt::Task<reset::ManualResetError> failure(rt::Task<reset::ManualResetResult> task) {
    std::optional<reset::ManualResetError> error;
    try { co_await std::move(task); } catch (const reset::ManualResetError& value) { error = value; }
    if (!error) throw rt::JsError("expected ManualResetError");
    co_return *error;
  }
  json::value stored(const rt::KvEntryMaybe<>& entry) { return rt::storedJsonView(entry.value); }
  rt::Fetch modifyUsage(rt::Fetch fetcher, std::function<void(json::value&)> modify) {
    return [fetcher, modify](rt::RequestInfo input, rt::RequestInit init) -> rt::Task<rt::Response> {
      rt::Promise fetching(fetcher(input, init)); auto response = co_await fetching.wait();
      auto payload = co_await response.json(); modify(payload); co_return rt::Response::json(payload);
    };
  }
}
ASYNC_TEST("manual click works with automatic off and shadow, promotes target and immediately serves the next eligible model") {
  harness::CredentialScope scope; AWAIT(f, harness::manualFixture()); AWAIT(result, reset::consumeManualCodexReset(f->targetHash, f->dependencies));
  EXPECT_EQ(result.active_account_id_hash, f->targetHash); EXPECT_EQ(result.reset_status, std::string("verified")); EXPECT_EQ(f->provider->redeemInputs.size(), 1u);
  AWAIT(selected, uos::codex::account_routing::selectCodexRoutingAccountsStrong(f->pool, f->pool.accounts, f->now, std::string("gpt-6-astra")));
  const auto* eligible = std::get_if<routing::RouteSelectionEligible>(&selected); ASSERT(eligible); ASSERT(!eligible->accounts.empty()); EXPECT_EQ(eligible->accounts[0].auth.account_id, std::string("target"));
  AWAIT(row, f->kv->get(routing::CODEX_ACCOUNT_ROUTING_KV_KEY)); const auto state = routing::parseCodexAccountRoutingState(stored(row)); ASSERT(state);
  EXPECT(!uos::authority::native_codex_capacity::quotaBlockForClass(state->slots[1], "standard")); EXPECT(uos::authority::native_codex_capacity::quotaBlockForClass(state->slots[1], "reserve"));
  AWAIT(observed, f->kv->get(routing::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY)); EXPECT_EQ(rt::getNumber(stored(observed).as_object().at("observations").as_array()[0].as_object().at("windows").as_object().at("primary"), "used_percent"), std::optional<double>(1));
  AWAIT(usage, f->kv->get(uos::codex::reset_settings::codexResetUsageKey(f->targetHash))); EXPECT(rt::deepEqual(stored(usage), rt::jsonParse(R"({"enabled":false})")));
  AWAIT(providers, f->kv->get(selection::PROVIDER_SELECTION_KV_KEY)); const json::value expected(json::object{{"provider_ids", json::array{"codex"}}, {"updated_at_ms", rt::makeNumber(f->now)}}); EXPECT(rt::deepEqual(stored(providers), expected));
  AWAIT(overage, f->kv->get(rt::KvKey{"uos_ai", "codex_overage_usage", "account", "v1", f->targetHash})); EXPECT(overage.value.isNull());
}
ASYNC_TEST("global disabled controls make zero usage, inventory or consume calls") {
  harness::CredentialScope scope;
  const std::vector<banked::CodexBankedResetConfig> configs{banked::config({.enabled = false}), banked::config({.mode = "disabled"})};
  for (const auto& configured : configs) { AWAIT(f, harness::manualFixture()); auto dependencies = f->dependencies; dependencies.reloadConfig = [configured] { return configured; }; AWAIT(error, failure(reset::consumeManualCodexReset(f->targetHash, dependencies))); EXPECT_EQ(error.status, 409); EXPECT_EQ(f->usageCalls(), 0); EXPECT_EQ(f->provider->callCount(), 0); }
}
ASYNC_TEST("unknown consumption never repeats or changes account priority") {
  harness::CredentialScope scope; AWAIT(f, harness::manualFixture()); f->provider->redeemResult = json::object{{"kind", "unknown"}, {"providerReceiptId", nullptr}};
  AWAIT(first, failure(reset::consumeManualCodexReset(f->targetHash, f->dependencies))); AWAIT(second, failure(reset::consumeManualCodexReset(f->targetHash, f->dependencies))); EXPECT_EQ(first.status, 409); EXPECT_EQ(second.status, 409);
  EXPECT_EQ(f->provider->redeemInputs.size(), 1u); AWAIT(active, f->kv->get(routing::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY)); EXPECT(rt::deepEqual(stored(active), f->active));
}
ASYNC_TEST("unpropagated reset stays pending; explicit reconciliation promotes without consuming again") {
  harness::CredentialScope scope; AWAIT(f, harness::manualFixture()); f->restore(false); AWAIT(error, failure(reset::consumeManualCodexReset(f->targetHash, f->dependencies))); EXPECT_EQ(error.resetStatus, std::optional<std::string>("verified"));
  AWAIT(active, f->kv->get(routing::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY)); EXPECT(rt::deepEqual(stored(active), f->active)); f->restore(true); AWAIT(result, reset::consumeManualCodexReset(f->targetHash, f->dependencies)); EXPECT_EQ(result.reset_status, std::string("verified")); EXPECT_EQ(f->provider->redeemInputs.size(), 1u);
}
ASYNC_TEST("duplicate completed action cannot restore an old priority after a later transition") {
  harness::CredentialScope scope; AWAIT(f, harness::manualFixture()); AWAIT(first, reset::consumeManualCodexReset(f->targetHash, f->dependencies)); EXPECT_EQ(first.reset_status, std::string("verified")); auto later = f->active; later["generation"] = 20;
  WRITE(f->kv, routing::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, later); AWAIT(error, failure(reset::consumeManualCodexReset(f->targetHash, f->dependencies))); EXPECT_EQ(error.code, std::string("codex_reset_already_completed")); AWAIT(active, f->kv->get(routing::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY)); EXPECT(rt::deepEqual(stored(active), later)); EXPECT_EQ(f->provider->redeemInputs.size(), 1u);
}
ASYNC_TEST("concurrent clicks consume only once") {
  harness::CredentialScope scope; AWAIT(f, harness::manualFixture()); std::vector<rt::Promise<reset::ManualResetResult>> pending; pending.emplace_back(reset::consumeManualCodexReset(f->targetHash, f->dependencies)); pending.emplace_back(reset::consumeManualCodexReset(f->targetHash, f->dependencies)); AWAIT(results, rt::allSettled(std::move(pending)).wait());
  int fulfilled = 0; for (const auto& result : results) if (result.status == "fulfilled") ++fulfilled; EXPECT_EQ(fulfilled, 1); EXPECT_EQ(f->provider->redeemInputs.size(), 1u);
}
ASYNC_TEST("daily cap and disabled selection reject before provider reads") {
  harness::CredentialScope scope; AWAIT(f, harness::manualFixture()); const std::string day = rt::slice(rt::Date(f->now).toISOString(), 0, 10); const json::object budget{{"v", 1}, {"day", day}, {"submission_count", 1}, {"updated_at_ms", rt::makeNumber(f->now)}};
  WRITE(f->kv, banked::codexResetAccountDailyKey(f->targetHash, day), budget); AWAIT(first, failure(reset::consumeManualCodexReset(f->targetHash, f->dependencies))); EXPECT_EQ(first.code, std::string("account_day_limit_reached"));
  const json::object selected{{"provider_ids", json::array{"openlux"}}, {"updated_at_ms", rt::makeNumber(f->now)}}; WRITE(f->kv, selection::PROVIDER_SELECTION_KV_KEY, selected); AWAIT(second, failure(reset::consumeManualCodexReset(f->targetHash, f->dependencies))); EXPECT_EQ(second.code, std::string("codex_provider_disabled")); EXPECT_EQ(f->usageCalls(), 0); EXPECT_EQ(f->provider->callCount(), 0);
}
ASYNC_TEST("credential replacement during preflight cannot consume a reset") {
  harness::CredentialScope scope; AWAIT(f, harness::manualFixture()); const auto fetcher = f->dependencies.fetcher; auto dependencies = f->dependencies;
  dependencies.fetcher = [f, fetcher](rt::RequestInfo input, rt::RequestInit init) -> rt::Task<rt::Response> { auto pool = f->pool; for (auto& auth : pool.accounts) auth.access_token += "-new"; const rt::KvValue value(json::value_from(pool)); co_await f->kv->set(routing::CODEX_AUTH_POOL_KV_KEY, value); rt::Promise fetching(fetcher(input, init)); auto response = co_await fetching.wait(); co_return response; };
  AWAIT(error, failure(reset::consumeManualCodexReset(f->targetHash, dependencies))); EXPECT_EQ(error.status, 409); EXPECT(f->provider->redeemInputs.empty());
}
ASYNC_TEST("missing credits and definitive rejection leave the prior active subscription unchanged") {
  harness::CredentialScope scope; const std::vector<bool> cases{true, false};
  for (const bool missing : cases) { AWAIT(f, harness::manualFixture()); if (missing) { f->provider->inventory.availableCount = 0; f->provider->inventory.credits.clear(); } else f->provider->redeemResult = json::object{{"kind", "rejected"}, {"reason", "no_credit"}};
    AWAIT(error, failure(reset::consumeManualCodexReset(f->targetHash, f->dependencies))); EXPECT_EQ(error.status, 422); AWAIT(active, f->kv->get(routing::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY)); EXPECT(rt::deepEqual(stored(active), f->active)); EXPECT_EQ(f->provider->redeemInputs.size(), missing ? 0u : 1u); }
}
ASYNC_TEST("verified promotion retains named-model exhaustion from the fresh usage result") {
  harness::CredentialScope scope; AWAIT(f, harness::manualFixture()); auto dependencies = f->dependencies;
  dependencies.fetcher = modifyUsage(dependencies.fetcher, [f](json::value& payload) { payload.as_object().at("additional_rate_limits").as_array().push_back(json::object{{"limit_name", "gpt-6-astra"}, {"rate_limit", json::object{{"primary_window", json::object{{"used_percent", 100}, {"reset_at", rt::makeNumber((f->now + 3'600'000) / 1000)}, {"limit_window_seconds", 18'000}}}}}}); });
  AWAIT(result, reset::consumeManualCodexReset(f->targetHash, dependencies)); EXPECT_EQ(result.reset_status, std::string("verified")); AWAIT(selected, uos::codex::account_routing::selectCodexRoutingAccountsStrong(f->pool, f->pool.accounts, f->now, std::string("gpt-6-astra"))); const auto* eligible = std::get_if<routing::RouteSelectionEligible>(&selected); ASSERT(eligible); ASSERT(!eligible->accounts.empty()); EXPECT_EQ(eligible->accounts[0].auth.account_id, std::string("first"));
}
ASYNC_TEST("manual and automatic concurrent submissions share the same one-per-day allowance") {
  harness::CredentialScope scope; AWAIT(f, harness::manualFixture()); const json::object enabled{{"enabled", true}}; WRITE(f->kv, uos::codex::reset_settings::codexResetUsageKey(f->targetHash), enabled);
  uos::codex::banked_reset::CodexBankedResetCandidate automatic{}; automatic.accountId = "target"; automatic.credentialVersion = f->identities[1].credentialVersion; automatic.quotaResetAtMs = f->now + 7'200'000; automatic.routingGeneration = 0; automatic.requestId = "automatic-race";
  automatic.fences.push_back({routing::CODEX_AUTH_POOL_KV_KEY, [f](const json::value& value) { return rt::jsonStringify(value) == rt::jsonStringify(json::value_from(f->pool)); }});
  const uos::codex::banked_reset::CodexBankedResetDependencies deps{.config = banked::config(), .provider = f->provider, .kv = f->kv, .now = [f] { return f->now; }};
  rt::Promise manual(reset::consumeManualCodexReset(f->targetHash, f->dependencies)); rt::Promise other(uos::codex::banked_reset_submission::attemptCodexBankedReset(automatic, deps)); auto swallowed = manual.catch_([](rt::Reason) { return reset::ManualResetResult{}; }); auto manualResult = co_await swallowed.wait(); auto automaticResult = co_await other.wait(); (void)manualResult; (void)automaticResult;
  EXPECT_EQ(f->provider->redeemInputs.size(), 1u); AWAIT(budget, f->kv->get(banked::codexResetAccountDailyKey(f->targetHash, rt::slice(rt::Date(f->now).toISOString(), 0, 10)))); EXPECT_EQ(rt::getNumber(stored(budget), "submission_count"), std::optional<double>(1));
}
ASYNC_TEST("an old completed quota window cannot be promoted again on a later UTC day") {
  harness::CredentialScope scope; AWAIT(f, harness::manualFixture()); auto clock = std::make_shared<double>(f->now); auto dependencies = f->dependencies; dependencies.now = [clock] { return *clock; };
  dependencies.fetcher = modifyUsage(dependencies.fetcher, [f](json::value& payload) { payload.as_object().at("rate_limit").as_object().at("secondary_window").as_object()["reset_at"] = rt::makeNumber((f->now + 7 * 86'400'000) / 1000); });
  AWAIT(first, reset::consumeManualCodexReset(f->targetHash, dependencies)); EXPECT_EQ(first.reset_status, std::string("verified")); auto later = f->active; later["generation"] = 20; WRITE(f->kv, routing::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, later); *clock += 2 * 3'600'000;
  AWAIT(error, failure(reset::consumeManualCodexReset(f->targetHash, dependencies))); EXPECT_EQ(error.code, std::string("codex_reset_already_completed")); AWAIT(active, f->kv->get(routing::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY)); EXPECT(rt::deepEqual(stored(active), later)); EXPECT_EQ(f->provider->redeemInputs.size(), 1u);
}
ASYNC_TEST("account removal after consumption retains verified reset evidence without promotion") {
  harness::CredentialScope scope; AWAIT(f, harness::manualFixture()); auto dependencies = f->dependencies; const auto fetcher = dependencies.fetcher;
  dependencies.fetcher = [f, fetcher](rt::RequestInfo input, rt::RequestInit init) -> rt::Task<rt::Response> { rt::Promise fetching(fetcher(input, init)); auto response = co_await fetching.wait(); if (!f->provider->redeemInputs.empty()) { auto pool = f->pool; pool.accounts.resize(1); const rt::KvValue value(json::value_from(pool)); co_await f->kv->set(routing::CODEX_AUTH_POOL_KV_KEY, value); } co_return response; };
  AWAIT(error, failure(reset::consumeManualCodexReset(f->targetHash, dependencies))); EXPECT_EQ(error.resetStatus, std::optional<std::string>("verified")); AWAIT(active, f->kv->get(routing::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY)); EXPECT(rt::deepEqual(stored(active), f->active)); EXPECT_EQ(f->provider->redeemInputs.size(), 1u);
}
