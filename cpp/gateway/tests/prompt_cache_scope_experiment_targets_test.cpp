// Links: tests/prompt_cache_scope_experiment_targets_test.cpp
// src/provider/removed_provider_telemetry.cpp src/responses_failover_stream.cpp src/config.cpp src/types.cpp
// src/kv.cpp src/utils.cpp src/codex/banked_reset.cpp src/runtime_config.cpp
// src/release.cpp src/models/codex_models.cpp src/storage/kv.cpp src/http.cpp
// src/storage/sqlite_readonly.cpp src/defaults.cpp src/storage/native_kv.cpp src/codex/banked_reset_provider.cpp
// src/codex/reset_settings.cpp src/provider/cerebras_rate_limits.cpp src/auth/relay.cpp
// src/codex/auth.cpp src/codex/capacity_routing.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_capacity.cpp
// src/codex/routing_state.cpp src/codex/routing_mutations.cpp src/authority/native_codex_mutations.cpp src/authority/native_codex_evaluation.cpp
// src/codex/native_auth.cpp src/codex/external_auth.cpp src/codex/rate_limit_429.cpp src/provider/health.cpp
// src/authority/native_codex_429.cpp src/codex/native_auth_transport.cpp src/bounded_response_body.cpp src/runtime/websocket.cpp
// src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp tests/helpers/codex_diff_kv.cpp src/authority/native_codex.cpp
// src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_read.cpp src/cache/telemetry_gate.cpp src/codex/account_routing.cpp
// src/codex/auth_refresh.cpp src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/overage_settings.cpp
// src/codex/routing_evaluation.cpp src/codex/routing_serial.cpp src/metered_quota.cpp src/models/codex_models_availability.cpp
// src/paid_fallback/ledger_stats.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp
// src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp
// src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp
// src/provider/selection.cpp src/catalog/promotion.cpp src/catalog/types.cpp src/models/codex_models_fetch.cpp
// src/cache/scope_experiment.cpp src/cache/scope_experiment_targets.cpp src/cache/scope_experiment_model.cpp src/cache/scope_targets.cpp
// tests/helpers/prompt_cache_scope_experiment_harness.cpp src/responses_stream.cpp src/openai_telemetry.cpp src/codex/responses_error.cpp
// Differential scenarios recorded from the real src/cache/scope-experiment-targets.ts under isolated Bun 1.4.0.
#include "tests/helpers/prompt_cache_scope_experiment_harness.hpp"
#include "uos/cache/scope_experiment_targets.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
namespace h = uos::tests::helpers::prompt_cache_scope_experiment_harness;
namespace m = uos::cache::scope_experiment_model;
namespace t = uos::cache::scope_experiment_targets;
namespace rt = uos::rt;
namespace json = uos::json;
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  void put(const rt::KvKey& key, const json::value& value) { h::kv->put(key, rt::KvValue(value)); }
  rt::Task<void> replay(std::string name) {
    h::HarnessScope scope; rt::globalDateNow() = [] { return 1'700'000'000'000.0; }; h::seed(); AWAIT(baseline, h::scopeBaselineFor()); const auto target = baseline.target;
    const auto vectors = rt::jsonParse(
#include "prompt_cache_scope_experiment_targets_vectors.inc"
    );
    const json::value* expected = nullptr; for (const auto& row : vectors.as_array()) if (rt::getString(row, "name") == name) expected = &row; ASSERT(expected);
    json::value result;
    try {
      if (name == "core_fields") {
        json::object values; const auto binding = json::value_from(target);
        for (const auto& key : rt::objectKeys(binding.as_object())) { auto changed = binding; changed.as_object()[key] = key == "catalog_client_version" ? json::value(nullptr) : json::value("changed"); const auto right = json::value_to<m::PromptCacheScopeTargetBinding>(changed); values[key] = t::sameTargetCore(target, right); } result = values;
      } else if (rt::startsWith(name, "acquire_") || rt::startsWith(name, "renew_")) {
        std::optional<t::AcquiredCycle> acquired;
        if (name != "acquire_new" && name != "acquire_busy_campaign" && name != "acquire_malformed_state" && name != "acquire_malformed_evidence" && name != "acquire_conflict") { AWAIT(first, t::acquireCycle(h::kv, target, target.auth_pool_versionstamp)); acquired = first; }
        if (name == "acquire_resume") { auto removing = h::kv->delete_(h::leaseKey(std::string(h::MODEL))); co_await std::move(removing); }
        if (name == "acquire_busy_campaign") put(h::campaignLeaseKey, json::object{{"owner", "other"}, {"target_id", "other"}, {"inventory_fingerprint", "other"}, {"lease_until_ms", rt::makeNumber(rt::dateNow() + 60000)}});
        if (name == "acquire_malformed_state") put(h::stateKey, json::object{{"v", 3}});
        if (name == "acquire_malformed_evidence") put(h::evidenceKey, json::object{{"v", 3}});
        if (rt::endsWith(name, "_conflict")) h::kv->beforeAtomicCommit = [name](const auto&) { if (name == "acquire_conflict") put(h::stateKey, nullptr); else { const auto value = h::kv->entryOf(h::leaseKey(std::string(h::MODEL))).value; h::kv->put(h::leaseKey(std::string(h::MODEL)), value); } };
        if (name == "renew_cycle_lost") put(h::leaseKey(std::string(h::MODEL)), json::object{{"owner", "other"}, {"lease_until_ms", rt::makeNumber(rt::dateNow() + 60000)}});
        if (name == "renew_campaign_lost") put(h::campaignLeaseKey, json::object{{"owner", "other"}, {"target_id", target.id}, {"inventory_fingerprint", target.inventory_fingerprint}, {"lease_until_ms", rt::makeNumber(rt::dateNow() + 60000)}});
        if (rt::startsWith(name, "renew_")) { ASSERT(acquired); auto renewing = t::renewLease(h::kv, acquired->state, acquired->cycle_owner); co_await std::move(renewing); result = json::object{{"renewed", true}}; }
        else { AWAIT(current, t::acquireCycle(h::kv, target, target.auth_pool_versionstamp)); result = json::object{{"state", json::value_from(current.state)}, {"cycle_owner", current.cycle_owner}}; }
      } else {
        if (name == "runtime_default") { auto runtime = h::kv->row(h::RUNTIME_CONFIG_V2_KEY); runtime.as_object()["default_model"] = "other-default"; put(h::RUNTIME_CONFIG_V2_KEY, runtime); }
        if (name == "catalog_stamp") h::kv->put(h::CODEX_MODELS_KV_KEY, h::kv->entryOf(h::CODEX_MODELS_KV_KEY).value);
        if (name == "auth_stamp") h::kv->put(h::CODEX_AUTH_POOL_KV_KEY, h::kv->entryOf(h::CODEX_AUTH_POOL_KV_KEY).value);
        if (name == "auth_membership") { auto pool = h::kv->row(h::CODEX_AUTH_POOL_KV_KEY); pool.as_object().at("accounts").as_array()[0].as_object()["account_id"] = "replacement"; put(h::CODEX_AUTH_POOL_KV_KEY, pool); }
        if (name == "inventory_roster" || name == "capability" || name == "client_version" || name == "model_removed") {
          auto snapshot = h::kv->row(h::CODEX_MODELS_KV_KEY);
          if (name == "inventory_roster") snapshot.as_object().at("models").as_array().push_back(h::modelRecord(std::string(h::MODEL) + "-sibling"));
          if (name == "capability") { auto controls = h::cacheControls(); controls["expected_usage_fields"] = json::array{"cached_tokens"}; snapshot.as_object()["models"] = json::array{h::modelRecord(std::string(h::MODEL), controls)}; }
          if (name == "client_version") snapshot.as_object()["client_version"] = "0.202.0";
          if (name == "model_removed") snapshot.as_object()["models"] = json::array{h::modelRecord("replacement-model")};
          put(h::CODEX_MODELS_KV_KEY, snapshot);
        }
        if (name == "runtime_missing") { auto removing = h::kv->delete_(h::RUNTIME_CONFIG_V2_KEY); co_await std::move(removing); }
        if (name == "runtime_malformed") put(h::RUNTIME_CONFIG_V2_KEY, json::object{{"version", 2}});
        AWAIT(resolution, t::resolveBoundTarget(h::kv, target));
        if (const auto* current = std::get_if<t::ScopeTargetResolutionResolved>(&resolution)) result = json::object{{"status", "resolved"}, {"value", json::object{{"binding", json::value_from(current->value.binding)}}}};
        else result = json::object{{"status", "inconclusive"}, {"reason", std::get<t::ScopeTargetResolutionInconclusive>(resolution).reason}};
      }
    } catch (const rt::JsError& error) { result = json::object{{"error", json::object{{"name", error.name}, {"message", error.message}}}}; }
    h::kv->beforeAtomicCommit = {};
    json::array rows; const std::vector<rt::KvKey> keys{h::stateKey, h::evidenceKey, h::campaignLeaseKey, h::leaseKey(std::string(h::MODEL))}; const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
    for (const auto& key : keys) { AWAIT(row, h::kv->get(key, strong)); rows.push_back(json::object{{"key", rt::jsonParse(h::encodeKey(key))}, {"value", row.value.toJson()}, {"versionstamp", row.versionstamp ? json::value(*row.versionstamp) : json::value(nullptr)}}); }
    const json::object actual{{"name", name}, {"result", uos::tests::helpers::codex_diff_kv::plain(result)}, {"atomicCalls", rt::makeNumber(h::kv->atomicCalls)}, {"rows", uos::tests::helpers::codex_diff_kv::plain(rows)}};
    EXPECT(uos::tests::helpers::codex_diff_kv::sameVector(expected->as_object(), actual, "result")); EXPECT(uos::tests::helpers::codex_diff_kv::sameVector(expected->as_object(), actual, "rows")); EXPECT(uos::tests::helpers::codex_diff_kv::sameVector(expected->as_object(), actual, "atomicCalls"));
  }
}
ASYNC_TEST("scope target differential: core_fields") { auto task = replay("core_fields"); co_await std::move(task); }
ASYNC_TEST("scope target differential: stable") { auto task = replay("stable"); co_await std::move(task); }
ASYNC_TEST("scope target differential: runtime_default") { auto task = replay("runtime_default"); co_await std::move(task); }
ASYNC_TEST("scope target differential: catalog_stamp") { auto task = replay("catalog_stamp"); co_await std::move(task); }
ASYNC_TEST("scope target differential: auth_stamp") { auto task = replay("auth_stamp"); co_await std::move(task); }
ASYNC_TEST("scope target differential: auth_membership") { auto task = replay("auth_membership"); co_await std::move(task); }
ASYNC_TEST("scope target differential: inventory_roster") { auto task = replay("inventory_roster"); co_await std::move(task); }
ASYNC_TEST("scope target differential: capability") { auto task = replay("capability"); co_await std::move(task); }
ASYNC_TEST("scope target differential: client_version") { auto task = replay("client_version"); co_await std::move(task); }
ASYNC_TEST("scope target differential: model_removed") { auto task = replay("model_removed"); co_await std::move(task); }
ASYNC_TEST("scope target differential: runtime_missing") { auto task = replay("runtime_missing"); co_await std::move(task); }
ASYNC_TEST("scope target differential: runtime_malformed") { auto task = replay("runtime_malformed"); co_await std::move(task); }
ASYNC_TEST("scope target differential: acquire_new") { auto task = replay("acquire_new"); co_await std::move(task); }
ASYNC_TEST("scope target differential: acquire_resume") { auto task = replay("acquire_resume"); co_await std::move(task); }
ASYNC_TEST("scope target differential: acquire_busy_cycle") { auto task = replay("acquire_busy_cycle"); co_await std::move(task); }
ASYNC_TEST("scope target differential: acquire_busy_campaign") { auto task = replay("acquire_busy_campaign"); co_await std::move(task); }
ASYNC_TEST("scope target differential: acquire_malformed_state") { auto task = replay("acquire_malformed_state"); co_await std::move(task); }
ASYNC_TEST("scope target differential: acquire_malformed_evidence") { auto task = replay("acquire_malformed_evidence"); co_await std::move(task); }
ASYNC_TEST("scope target differential: acquire_conflict") { auto task = replay("acquire_conflict"); co_await std::move(task); }
ASYNC_TEST("scope target differential: renew_owned") { auto task = replay("renew_owned"); co_await std::move(task); }
ASYNC_TEST("scope target differential: renew_cycle_lost") { auto task = replay("renew_cycle_lost"); co_await std::move(task); }
ASYNC_TEST("scope target differential: renew_campaign_lost") { auto task = replay("renew_campaign_lost"); co_await std::move(task); }
ASYNC_TEST("scope target differential: renew_conflict") { auto task = replay("renew_conflict"); co_await std::move(task); }
