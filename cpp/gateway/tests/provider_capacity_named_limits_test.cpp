// Links: tests/provider_capacity_named_limits_test.cpp tests/helpers/provider_capacity_harness.cpp tests/helpers/counting_kv.cpp src/auth/relay.cpp src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/authority/native_codex.cpp src/authority/native_paid.cpp src/bounded_response_body.cpp src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp src/cache/prompt_analytics.cpp src/cache/telemetry_gate.cpp src/codex/account_routing.cpp src/codex/auth_refresh.cpp src/codex/auth.cpp src/codex/banked_reset_provider.cpp src/codex/banked_reset.cpp src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp src/codex/native_auth_transport.cpp src/codex/native_auth.cpp src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/codex/routing_state.cpp src/config.cpp src/http.cpp src/inference_deadline.cpp src/kv.cpp src/metered_quota.cpp src/models/codex_models_availability.cpp src/optional_telemetry_queue.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp src/provider/capacity.cpp src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/selection.cpp src/release.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp src/runtime/http.cpp src/runtime/websocket.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/utils.cpp src/authority/native_paid_contracts.cpp src/types.cpp
// Port of tests/provider-capacity-named-limits.test.ts; all 3 original cases, no deferred cases.
#include "tests/helpers/provider_capacity_harness.hpp"
#include "uos/codex/account_routing.hpp"
#include "uos/provider/capacity.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"
#include <string_view>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace capacity = uos::provider::capacity;
  using namespace uos::tests::helpers::provider_capacity_harness;
  json::value account(const json::value& view, double slot) {
    const auto* sources = rt::getArray(view, "sources");
    if (sources) for (const auto& source : *sources)
      if (rt::getString(source, "source") == "codex" && rt::getNumber(source, "slot") == slot) return source;
    throw rt::JsError("expected a Codex capacity account");
  }
  json::value additional(double used, double resetAt) {
    return json::array{json::object{{"limit_name", "GPT-5.3-Codex-Spark"}, {"metered_feature", "codex_bengalfox"},
      {"windows", json::object{{"primary", json::object{{"limit_window_seconds", 18'000}, {"used_percent", used}, {"reset_at_ms", resetAt}}},
                                {"secondary", nullptr}}}}};
  }
  json::value stored(const rt::KvKey& key) {
    const auto* row = kvStore.get(keyToString(key));
    if (!row) throw rt::JsError("expected stored capacity row");
    return row->value.toJson();
  }
  const json::value& member(const json::value& value, std::string_view name) {
    const auto* result = rt::get(value, name);
    if (!result) throw rt::JsError("expected capacity member " + std::string(name));
    return *result;
  }
}
#define AWAIT(name, expression) auto name##Pending = rt::Promise(expression); auto name = co_await name##Pending.wait()

ASYNC_TEST("sampler carries named Codex model limits alongside null secondary windows") {
  HarnessScope scope; seed(); std::vector<FetchCall> calls;
  capacity::ProviderCapacitySnapshotOptions options;
  options.kv = kvStub; options.fetcher = createFetcher(calls, std::nullopt, {}, {}, true); options.now = [] { return nowMs; };
  AWAIT(live, capacity::refreshProviderCapacity(options));
  const json::value liveJson = json::value_from(live);
  const auto accountOne = account(liveJson, 1), accountTwo = account(liveJson, 2);
  EXPECT(member(member(accountOne, "windows"), "secondary").is_null());
  EXPECT(rt::deepEqual(member(accountOne, "additional_rate_limits"), additional(38, 1'800'011'000'000)));
  EXPECT(rt::deepEqual(member(accountTwo, "additional_rate_limits"), additional(81.25, 1'800'011'000'000)));
  AWAIT(persisted, capacity::getPersistedProviderCapacityView(options));
  const json::value persistedJson = json::value_from(persisted);
  EXPECT(rt::deepEqual(member(account(persistedJson, 1), "additional_rate_limits"), member(accountOne, "additional_rate_limits")));
  EXPECT(rt::deepEqual(member(account(persistedJson, 2), "additional_rate_limits"), member(accountTwo, "additional_rate_limits")));
  EXPECT(rt::jsonStringify(liveJson).find("must-not-escape") == std::string::npos);
  const std::string routingObservation = rt::jsonStringify(stored(uos::codex::account_routing::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY));
  EXPECT(routingObservation.find("account-one") == std::string::npos);
  EXPECT(routingObservation.find("account-two") == std::string::npos);
  EXPECT(routingObservation.find("GPT-5.3-Codex-Spark") != std::string::npos);
}

ASYNC_TEST("sampler keeps unused additional limits visible while deferring them from routing") {
  HarnessScope scope; seed(); std::vector<FetchCall> calls;
  capacity::ProviderCapacitySnapshotOptions options;
  options.kv = kvStub;
  options.fetcher = createFetcher(calls, std::nullopt, {}, [](auto) { return std::array<double, 2>{12.5, 0}; }, true, nowMs / 1'000 + 18'000);
  options.now = [] { return nowMs; };
  AWAIT(live, capacity::refreshProviderCapacity(options));
  const json::value liveJson = json::value_from(live);
  EXPECT(rt::deepEqual(member(account(liveJson, 1), "additional_rate_limits"), additional(0, nowMs + 18'000'000)));
  const std::string routingObservation = rt::jsonStringify(stored(uos::codex::account_routing::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY));
  EXPECT(routingObservation.find("GPT-5.3-Codex-Spark") == std::string::npos);
}

ASYNC_TEST("sampler keeps named limits on the reporting account only") {
  HarnessScope scope; seed(); std::vector<FetchCall> calls;
  capacity::ProviderCapacitySnapshotOptions options;
  options.kv = kvStub;
  options.fetcher = createFetcher(calls, std::nullopt, {}, {}, true, 1'800'011'000, 503, [](auto id) { return id == "account-one"; });
  options.now = [] { return nowMs; };
  AWAIT(live, capacity::refreshProviderCapacity(options));
  const json::value liveJson = json::value_from(live);
  EXPECT_EQ(member(account(liveJson, 1), "additional_rate_limits").as_array().size(), std::size_t{1});
  EXPECT(rt::deepEqual(member(account(liveJson, 2), "additional_rate_limits"), json::array{}));
  const auto snapshot = stored(capacity::PROVIDER_CAPACITY_SNAPSHOT_KEY);
  EXPECT(rt::deepEqual(member(account(snapshot, 2), "additional_rate_limits"), json::array{}));
  AWAIT(persisted, capacity::getPersistedProviderCapacityView(options));
  const json::value persistedJson = json::value_from(persisted);
  EXPECT(rt::deepEqual(member(account(persistedJson, 2), "additional_rate_limits"), json::array{}));
  const auto routingStore = stored(uos::codex::account_routing::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY);
  const auto& observations = member(routingStore, "observations").as_array();
  json::array actual;
  for (const auto& observation : observations)
    actual.push_back(json::array{member(observation, "slot"), static_cast<std::int64_t>(member(observation, "additional_rate_limits").as_array().size())});
  EXPECT(rt::deepEqual(actual, json::array{json::array{0, 1}, json::array{1, 0}}));
}
