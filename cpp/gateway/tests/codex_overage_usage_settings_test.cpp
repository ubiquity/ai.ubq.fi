// Links: tests/codex_overage_usage_settings_test.cpp
// tests/helpers/counting_kv.cpp src/kernel/usage.cpp src/codex/reset_settings.cpp src/kv.cpp
// src/types.cpp src/metered_quota.cpp src/kernel/quota_v2.cpp src/authority/native_kernel.cpp
// src/admin/kernel.cpp src/quota_projection.cpp src/provider/capacity_events.cpp src/codex/banked_reset_provider.cpp
// src/kernel/policy_queue.cpp src/admin/api_keys.cpp src/utils.cpp src/paid_fallback/index.cpp
// src/authority/native_paid_contracts.cpp src/paid_fallback/ledger_stats.cpp src/codex/banked_reset.cpp src/api_key_policy.cpp
// src/request.cpp src/codex/auth.cpp src/paid_fallback/ledger_admission.cpp src/authority/native_api_key.cpp
// src/authority/native_paid.cpp src/admin/api_key_audit.cpp src/api_keys.cpp src/codex/overage_settings.cpp
// src/authority/native_api_key_audit.cpp src/paid_fallback/ledger_backfill.cpp src/provider/metered.cpp src/authority/native_api_key_policy.cpp
// src/codex/external_auth.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/rollups.cpp src/codex/routing_mutations.cpp
// src/authority/native_codex_mutations.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/codex/capacity_routing.cpp
// src/kernel/quota_write.cpp src/authority/native_paid_policy.cpp src/defaults.cpp src/config.cpp
// src/provider/capacity_parse.cpp src/kernel/quota_reservations.cpp src/codex/routing_state.cpp src/provider/surplus.cpp
// src/responses_stream.cpp src/paid_fallback/ledger_settlement.cpp src/provider/health.cpp src/provider/anthropic_chat_responses.cpp
// src/provider/anthropic_response_wire.cpp src/codex/native_auth.cpp src/provider/capacity.cpp src/provider/capacity_history.cpp
// src/provider/capacity_contract.cpp src/release.cpp src/cache/prompt_analytics_read.cpp src/codex/rate_limit_429.cpp
// src/provider/capacity_rollups.cpp src/storage/kv.cpp src/provider/surplus_claude_request.cpp src/codex/experiment.cpp
// src/codex/dispatch.cpp src/provider/claude_capacity_rollups.cpp src/http.cpp src/authority/native_codex_429.cpp
// src/provider/anthropic_replay.cpp src/provider/anthropic_responses.cpp src/kernel/attestation.cpp src/provider/anthropic_chat_stream.cpp
// src/provider/cerebras_rate_limits.cpp src/authority/native_kernel_contracts.cpp src/provider/capacity_analytics.cpp src/provider/claude_code_quota.cpp
// src/provider/claude_code_accounts.cpp src/provider/surplus_claude_chat_request.cpp src/provider/claude_capacity_history.cpp src/models/codex_models_fetch.cpp
// src/cache/prompt_analytics_core.cpp src/provider/claude_code_credentials.cpp src/codex/account_routing.cpp src/codex/routing_serial.cpp
// src/auth/relay.cpp src/codex/auth_refresh.cpp src/provider/selection.cpp src/authority/native_codex_evaluation.cpp
// src/codex/native_auth_transport.cpp src/bounded_response_body.cpp src/provider/claude_code_usage_schema.cpp
// src/models/codex_models.cpp src/codex/routing_evaluation.cpp src/models/codex_models_availability.cpp src/authority/native_codex.cpp
// src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp src/runtime_config.cpp src/runtime/websocket.cpp
// Port of all five cases from tests/codex-overage-usage-settings.test.ts.
// Calls the actual admin/index exports; handler registration/auth remain with handler/index.
#include "tests/helpers/counting_kv.hpp"
#include "uos/admin/index.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/capacity_routing.hpp"
#include "uos/codex/overage_settings.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/utils.hpp"
#include <memory>

#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  namespace rt = uos::rt; namespace json = uos::json; namespace overage = uos::codex::overage_settings;
  namespace admin = uos::admin::index; namespace auth = uos::codex::auth;
  const rt::KvKey CODEX_AUTH_POOL_KEY{"ubq_ai", "codex_auth"};
  const std::string OVERAGE_URL = "https://ai.ubq.fi/admin/providers/codex/overage-usage";
  struct Fixture {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>()};
    std::string saved = uos::config::config.codexAuthJsonB64;
    std::shared_ptr<uos::tests::helpers::counting_kv::CountingKv> kv = std::make_shared<uos::tests::helpers::counting_kv::CountingKv>();
    rt::GlobalFetchOverride offline{rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> { throw rt::JsError("offline fixture forbids live provider calls"); co_return rt::Response(); })};
    Fixture() { uos::config::config.codexAuthJsonB64.clear(); auth::resetCodexAuthCacheForTest(); overage::resetOverageUsageCacheForTest(); uos::kv::setKvForTest(kv); uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest([](const uos::provider::capacity_events::ProviderCapacitySampleTriggerOptions&) {}); }
    ~Fixture() { uos::kv::setKvForTest(nullptr); auth::resetCodexAuthCacheForTest(); overage::resetOverageUsageCacheForTest(); uos::config::config.codexAuthJsonB64 = saved; uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest(nullptr); }
  };
  json::object codexPoolEntry(const std::vector<std::string>& accountIds) {
    const auto payload = rt::toBase64Url(rt::encodeUtf8(rt::jsonStringify(json::object{{"exp", rt::makeNumber(rt::Math::floor((rt::dateNow() + 3600000) / 1000))}})));
    json::array accounts; for (const auto& id : accountIds) accounts.push_back(json::object{{"access_token", "h." + payload + ".s"}, {"refresh_token", "refresh"}, {"account_id", id}, {"updated_at_ms", rt::makeNumber(rt::dateNow())}});
    return {{"accounts", accounts}, {"updated_at_ms", rt::makeNumber(rt::dateNow())}};
  }
  rt::Request patchRequest(const json::value& body) { return rt::Request(OVERAGE_URL, rt::RequestInit{.method = "PATCH", .headers = rt::Headers{{"Content-Type", "application/json"}}, .body = rt::BodyInit(body.is_string() ? std::string(body.as_string()) : rt::jsonStringify(body))}); }
  const json::value& field(const json::value& value, std::string_view key) { const auto* result = rt::get(value, key); if (!result) throw rt::JsError("missing test field: " + std::string(key)); return *result; }
  const json::array& rows(const json::value& body) { return field(body, "data").as_array(); }
}
ASYNC_TEST("the overage usage store round-trips and absent rows read as false") {
  Fixture f; AWAIT(hash, uos::utils::sha256Hex("overage-store-account")); AWAIT(absent, overage::readOverageUsage(f.kv, hash)); EXPECT(!absent); AWAIT(absentRow, f.kv->get(overage::codexOverageUsageKey(hash))); EXPECT(absentRow.value.isNull());
  co_await overage::writeOverageUsage(f.kv, hash, true); AWAIT(enabledRow, f.kv->get(overage::codexOverageUsageKey(hash))); EXPECT(rt::deepEqual(enabledRow.value.toJson(), json::object{{"allow", true}})); AWAIT(enabled, overage::readOverageUsage(f.kv, hash)); EXPECT(enabled);
  co_await overage::writeOverageUsage(f.kv, hash, false); AWAIT(disabledRow, f.kv->get(overage::codexOverageUsageKey(hash))); EXPECT(rt::deepEqual(disabledRow.value.toJson(), json::object{{"allow", false}})); AWAIT(disabled, overage::readOverageUsage(f.kv, hash)); EXPECT(!disabled); f.kv->seed(overage::codexOverageUsageKey(hash), json::object{{"allow", "yes"}}); AWAIT(malformed, overage::readOverageUsage(f.kv, hash)); EXPECT(!malformed);
}
ASYNC_TEST("the admin overage surface lists every account, persists a patch, and refreshes the routing cache") {
  Fixture f; f.kv->seed(CODEX_AUTH_POOL_KEY, codexPoolEntry({"overage-account-one"})); AWAIT(hash, uos::utils::sha256Hex("overage-account-one")); AWAIT(cohort, uos::utils::sha256Hex(std::string("uos-prompt-cache-account-cohort-v1\0", 35) + "overage-account-one")); AWAIT(listed, admin::handleAdminCodexOverageUsage(rt::Request(OVERAGE_URL))); EXPECT_EQ(listed.status(), 200); EXPECT_EQ(listed.headers().get("Cache-Control"), std::optional<std::string>("no-store")); AWAIT(body, listed.json()); ASSERT_EQ(rows(body).size(), std::size_t(1)); const auto& row = rows(body)[0]; EXPECT_EQ(rt::getNumber(row, "slot"), std::optional(1.0)); EXPECT_EQ(rt::getString(row, "account_id_hash"), std::optional<std::string_view>(hash)); EXPECT_EQ(rt::getString(row, "account_cohort_id"), std::optional<std::string_view>(cohort)); EXPECT_EQ(rt::getBool(row, "allow"), std::optional(false)); EXPECT(!rt::hasOwn(row.as_object(), "account_id")); EXPECT(!rt::hasOwn(row.as_object(), "access_token"));
  const json::object patch{{"account_id_hash", hash}, {"allow", true}}; AWAIT(patched, admin::handleAdminCodexOverageUsage(patchRequest(patch))); EXPECT_EQ(patched.status(), 200); AWAIT(patchBody, patched.json()); EXPECT(rt::deepEqual(patchBody, patch)); AWAIT(stored, f.kv->get(overage::codexOverageUsageKey(hash))); EXPECT(rt::deepEqual(stored.value.toJson(), json::object{{"allow", true}})); AWAIT(poolRow, f.kv->get(CODEX_AUTH_POOL_KEY)); const auto pool = auth::parseCodexAuthPool(poolRow.value.toJson()); ASSERT(pool); AWAIT(identity, uos::codex::capacity_routing::routingAccountIdentity(pool->accounts[0])); EXPECT(overage::overageUsageAllowedSync(identity.accountIdHash)); AWAIT(relisted, admin::handleAdminCodexOverageUsage(rt::Request(OVERAGE_URL))); AWAIT(relistedBody, relisted.json()); EXPECT_EQ(rt::getBool(rows(relistedBody)[0], "allow"), std::optional(true));
}
ASYNC_TEST("the admin overage surface rejects malformed bodies and stale subscriptions") {
  Fixture f; f.kv->seed(CODEX_AUTH_POOL_KEY, codexPoolEntry({"overage-account-one"})); AWAIT(hash, uos::utils::sha256Hex("overage-account-one")); AWAIT(nonJson, admin::handleAdminCodexOverageUsage(patchRequest("{"))); EXPECT_EQ(nonJson.status(), 400); const std::vector<json::value> bad{json::object(), json::object{{"account_id_hash", hash}}, json::object{{"account_id_hash", hash}, {"allow", "yes"}}, json::object{{"allow", true}}}; for (const auto& input : bad) { AWAIT(invalid, admin::handleAdminCodexOverageUsage(patchRequest(input))); EXPECT_EQ(invalid.status(), 400); }
  AWAIT(missingHash, uos::utils::sha256Hex("missing-account")); AWAIT(stale, admin::handleAdminCodexOverageUsage(patchRequest(json::object{{"account_id_hash", missingHash}, {"allow", true}}))); EXPECT_EQ(stale.status(), 409); AWAIT(staleBody, stale.json()); EXPECT_EQ(rt::getString(field(staleBody, "error"), "message"), std::optional<std::string_view>("Subscription changed. Reload Providers.")); AWAIT(stored, f.kv->get(overage::codexOverageUsageKey(hash))); EXPECT(stored.value.isNull());
}
ASYNC_TEST("the admin overage surface exposes the upstream balance and limit, or usage unavailable") {
  Fixture f; f.kv->seed(CODEX_AUTH_POOL_KEY, codexPoolEntry({"overage-account-one"})); json::value payload = json::object{{"credits", json::object{{"balance", "12.50"}, {"overage_limit_reached", false}}}, {"plan_type", "prolite"}}; bool failure = false;
  rt::GlobalFetchOverride mock{rt::Fetch([&](rt::RequestInfo input, rt::RequestInit init) {
    const auto url = std::visit([](const auto& item) -> std::string { using T = std::decay_t<decltype(item)>; if constexpr (std::is_same_v<T, std::string>) return item; else if constexpr (std::is_same_v<T, rt::URL>) return item.href(); else return item.url(); }, input);
    EXPECT(rt::endsWith(url, "/backend-api/wham/usage")); EXPECT_EQ(init.method, std::optional<std::string>("GET")); EXPECT_EQ(init.redirect, std::optional<std::string>("manual")); ASSERT(init.headers); EXPECT_EQ(init.headers->get("Accept"), std::optional<std::string>("application/json")); EXPECT_EQ(init.headers->get("ChatGPT-Account-ID"), std::optional<std::string>("overage-account-one")); EXPECT_EQ(init.headers->get("User-Agent"), std::optional<std::string>("codex_cli_rs/0.100.0 (ai.ubq.fi)"));
    return failure ? rt::Response(nullptr, rt::ResponseInit{.status = 500}) : rt::Response::json(payload);
  })};
  AWAIT(listed, admin::handleAdminCodexOverageUsage(rt::Request(OVERAGE_URL))); EXPECT_EQ(listed.status(), 200); AWAIT(body, listed.json()); const auto& overageView = field(rows(body)[0], "overage"); EXPECT(rt::deepEqual(overageView, json::object{{"balance", "12.50"}, {"overage_limit_reached", false}})); EXPECT_EQ(rt::objectKeys(overageView.as_object()), std::vector<std::string>({"balance", "overage_limit_reached"})); payload = json::object{{"credits", json::object{{"balance", "3.00"}, {"overage_limit_reached", true}}}};
  AWAIT(reached, admin::handleAdminCodexOverageUsage(rt::Request(OVERAGE_URL))); AWAIT(reachedBody, reached.json()); EXPECT_EQ(rt::getBool(field(rows(reachedBody)[0], "overage"), "overage_limit_reached"), std::optional(true));
  failure = true; AWAIT(unavailable, admin::handleAdminCodexOverageUsage(rt::Request(OVERAGE_URL))); EXPECT_EQ(unavailable.status(), 200); AWAIT(unavailableBody, unavailable.json()); EXPECT(field(rows(unavailableBody)[0], "overage").is_null()); failure = false; payload = json::object{{"credits", json::object{{"balance", 12.5}}}}; AWAIT(malformed, admin::handleAdminCodexOverageUsage(rt::Request(OVERAGE_URL))); EXPECT_EQ(malformed.status(), 200); AWAIT(malformedBody, malformed.json()); EXPECT(field(rows(malformedBody)[0], "overage").is_null());
}
ASYNC_TEST("one subscription's overage choice never changes another account's value") {
  Fixture f; f.kv->seed(CODEX_AUTH_POOL_KEY, codexPoolEntry({"overage-account-one", "overage-account-two"})); AWAIT(firstHash, uos::utils::sha256Hex("overage-account-one")); AWAIT(secondHash, uos::utils::sha256Hex("overage-account-two")); AWAIT(patched, admin::handleAdminCodexOverageUsage(patchRequest(json::object{{"account_id_hash", firstHash}, {"allow", true}}))); EXPECT_EQ(patched.status(), 200); AWAIT(listed, admin::handleAdminCodexOverageUsage(rt::Request(OVERAGE_URL))); AWAIT(body, listed.json()); json::array projected; for (const auto& row : rows(body)) projected.push_back(json::object{{"hash", field(row, "account_id_hash")}, {"allow", field(row, "allow")}}); EXPECT(rt::deepEqual(projected, json::array{json::object{{"hash", firstHash}, {"allow", true}}, json::object{{"hash", secondHash}, {"allow", false}}}));
  AWAIT(poolRow, f.kv->get(CODEX_AUTH_POOL_KEY)); const auto pool = auth::parseCodexAuthPool(poolRow.value.toJson()); ASSERT(pool); std::vector<rt::Promise<uos::codex::capacity_routing::CodexRoutingAccountIdentity>> identitiesPending; for (const auto& account : pool->accounts) identitiesPending.emplace_back(uos::codex::capacity_routing::routingAccountIdentity(account)); const auto identities = co_await rt::all(std::move(identitiesPending)).wait(); auto loadTask = overage::loadOverageUsageSettings(*pool, true); co_await std::move(loadTask); ASSERT_EQ(identities.size(), std::size_t(2)); EXPECT(overage::overageUsageAllowedSync(identities[0].accountIdHash)); EXPECT(!overage::overageUsageAllowedSync(identities[1].accountIdHash)); overage::resetOverageUsageCacheForTest(); EXPECT(!overage::overageUsageAllowedSync(identities[0].accountIdHash)); EXPECT(!overage::overageUsageAllowedSync(identities[1].accountIdHash));
}
