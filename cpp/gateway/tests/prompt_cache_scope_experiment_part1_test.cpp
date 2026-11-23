// Links: tests/prompt_cache_scope_experiment_part1_test.cpp
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
// Port of runnable cases in tests/prompt-cache-scope-experiment-part1.test.ts.
// Deferred to admin/codex.ts owner: diagnostic HTTP projection/redaction, hidden reader failures,
// request-field rejection, public current-release refusal, and the malformed-state HTTP envelope.
#include "tests/helpers/prompt_cache_scope_experiment_harness.hpp"
#include "uos/cache/scope_experiment_targets.hpp"
#include "uos/kv.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include <set>
namespace h = uos::tests::helpers::prompt_cache_scope_experiment_harness;
namespace m = uos::cache::scope_experiment_model;
namespace rt = uos::rt;
namespace json = uos::json;
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  m::PromptCacheScopeExperimentTelemetryBaselineTestOptions baselineOptions() { return {.kv = h::kv, .release = std::string(h::TELEMETRY_RELEASE)}; }
  void put(const rt::KvKey& key, const json::value& value) { h::kv->put(key, rt::KvValue(value)); }
  std::string rowText(const rt::KvKey& key) { return rt::jsonStringify(h::kv->row(key)); }
  bool hasScope() { return rt::includes(rowText(h::CODEX_MODELS_KV_KEY), "\"scope\""); }
  rt::Task<void> seedBaseline(std::string model = std::string(h::MODEL)) { auto task = h::seedStage0Baseline(model); co_await std::move(task); }
}
ASYNC_TEST("prompt-cache Stage 0 diagnostic is read-only and redacts the server-selected target") {
  h::HarnessScope scope; h::seed();
  auto seedTask = seedBaseline(); co_await std::move(seedTask);
  std::vector<rt::KvKey> reads; h::kv->beforeGet = [&reads](rt::KvKey key) { reads.push_back(key); return rt::fulfilledTask(); };
  h::ScopeTransport transport; transport.onInference = [](auto, const auto&, const auto&) -> rt::Response { throw rt::JsError("Stage 0 diagnostics must not contact upstream"); };
  const rt::GlobalFetchOverride fetch(transport.fetcher()); const auto options = baselineOptions();
  AWAIT(baseline, h::readPromptCacheScopeExperimentTelemetryBaseline(options));
  EXPECT_EQ(baseline.status, "eligible"); EXPECT(!rt::includes(rt::jsonStringify(uos::cache::telemetry_gate::toJson(baseline)), h::MODEL));
  EXPECT(std::any_of(reads.begin(), reads.end(), [](const auto& key) { return key.size() > 1 && key[0].isString() && key[0].string() == "uos_ai" && key[1].isString() && key[1].string() == "prompt_cache_telemetry_gate"; }));
  EXPECT_EQ(h::kv->atomicCalls, 0); EXPECT_EQ(transport.requests.size(), 0u); EXPECT_EQ(transport.refreshes, 0u);
}
ASYNC_TEST("a missing or mismatched pinned slot is an inconclusive scope result") {
  h::HarnessScope scope; const auto response = h::sseCompleted(0, 2560); const rt::AbortController controller;
  AWAIT(result, h::readPromptCacheScopeExperimentCompletedUsage(response, 1, std::string(h::MODEL), rt::performanceNow(), controller.signal()));
  ASSERT(std::holds_alternative<m::ReadSampleResultInconclusive>(result)); EXPECT_EQ(std::get<m::ReadSampleResultInconclusive>(result).reason, "slot_drift");
}
ASYNC_TEST("prompt-cache scope uses three fixed cycles, publishes canonical scope, and stores only redacted evidence") {
  h::HarnessScope scope; h::seed(); h::ScopeTransport transport;
  transport.onInference = [](std::size_t index, const auto&, const auto&) { const auto step = static_cast<double>(index % 10); return h::sseCompleted(h::cycleValue(h::CACHE_READ_CYCLE, step), h::cycleValue(h::CACHE_WRITE_CYCLE_LEADING, step)); };
  const rt::GlobalFetchOverride fetch(transport.fetcher());
  AWAIT(first, h::runExperiment());
  const std::vector<std::string> accountIds{"account-one", "account-two"};
  for (const auto& accountId : accountIds) { AWAIT(health, h::waitForCodexHealth(accountId)); EXPECT_EQ(health.state, "healthy"); EXPECT_EQ(health.last_event, std::optional<std::string>("success")); EXPECT(health.last_success_at_ms.has_value()); }
  AWAIT(second, h::runExperiment()); AWAIT(third, h::runExperiment());
  EXPECT_EQ(first.status, "in_progress"); EXPECT_EQ(first.completed_cycles, 1); EXPECT_EQ(second.status, "in_progress"); EXPECT_EQ(second.completed_cycles, 2);
  EXPECT_EQ(third.status, "completed"); ASSERT(third.scope);
  const m::ConcreteScopeObservation expected{"responses_implicit_input_text_keyed_cycle_isolated_v5", "account_scoped", "preserved", "independent", std::string(h::MODEL)};
  EXPECT_EQ(*third.scope, expected); EXPECT_EQ(transport.requests.size(), 30u); EXPECT_EQ(transport.refreshes, 3u);
  std::set<std::string> cycleNonces;
  for (std::size_t cycle = 0; cycle < 3; ++cycle) {
    const auto offset = cycle * 10; const auto& firstRow = transport.requests[offset];
    for (std::size_t index = 0; index < 10; ++index) { const auto& row = transport.requests[offset + index]; EXPECT_EQ(row.account, std::optional<std::string>(index == 2 || index == 3 ? "account-two" : "account-one")); EXPECT_EQ(row.body, firstRow.body); }
    const auto body = rt::jsonParse(firstRow.body); auto keys = rt::objectKeys(body.as_object()); rt::sort(keys);
    const std::vector<std::string> expectedKeys{"input", "model", "prompt_cache_key", "reasoning", "store", "stream"}; EXPECT_EQ(keys, expectedKeys);
    EXPECT_EQ(rt::getString(body, "model"), std::optional<std::string>(std::string(h::MODEL))); EXPECT_EQ(rt::getBool(body, "store"), std::optional(false)); EXPECT_EQ(rt::getBool(body, "stream"), std::optional(true));
    EXPECT(rt::deepEqual(*rt::get(body, "reasoning"), json::object{{"effort", "none"}}));
    const auto cacheKey = rt::getString(body, "prompt_cache_key"); ASSERT(cacheKey); EXPECT(rt::startsWith(*cacheKey, "uos-cache-scope-v5-")); EXPECT_EQ(rt::length(*cacheKey), 55u);
    const auto* input = rt::getArray(body, "input"); ASSERT(input); EXPECT_EQ(input->size(), 2u);
    const auto& developer = (*input)[0]; EXPECT_EQ(rt::getString(developer, "type"), std::optional<std::string>("message")); EXPECT_EQ(rt::getString(developer, "role"), std::optional<std::string>("developer"));
    const auto* content = rt::getArray(developer, "content"); ASSERT(content); ASSERT(!content->empty()); EXPECT_EQ(rt::getString((*content)[0], "type"), std::optional<std::string>("input_text"));
    EXPECT(!rt::hasOwn((*content)[0].as_object(), "prompt_cache_breakpoint")); const auto text = rt::getString((*content)[0], "text"); ASSERT(text); const auto parts = rt::split(*text, "\n\n"); ASSERT(parts.size() == 2); EXPECT(rt::startsWith(parts[0], "cache-scope-cycle:")); cycleNonces.insert(parts[0]); EXPECT_EQ(rt::split(parts[1], " ").size(), 2560u);
    EXPECT_EQ(rt::getString((*input)[1], "role"), std::optional<std::string>("user")); EXPECT(rt::deepEqual(*rt::get((*input)[1], "content"), json::array{json::object{{"type", "input_text"}, {"text", "Reply with exactly: cache scope experiment."}}}));
    EXPECT_EQ(firstRow.conversation, transport.requests[offset + 6].conversation); EXPECT(firstRow.conversation != transport.requests[offset + 7].conversation); EXPECT_EQ(firstRow.conversation, transport.requests[offset + 9].conversation);
  }
  EXPECT_EQ(cycleNonces.size(), 3u); EXPECT(hasScope());
  const auto runtime = h::kv->row(h::RUNTIME_CONFIG_V2_KEY); const auto* runtimeModels = rt::get(runtime, "codex_models"); ASSERT(runtimeModels); EXPECT(!rt::includes(rt::jsonStringify(*runtimeModels), "\"scope\""));
  json::object refresh{{"source", "admin-refresh"}, {"updated_at_ms", rt::makeNumber(rt::dateNow())}, {"models", json::array{h::modelRecord(std::string(h::MODEL))}}};
  AWAIT(stored, h::storeCodexModelsSnapshot(refresh)); EXPECT(stored); EXPECT(hasScope());
  const auto evidence = m::parseEvidence(rt::KvValue(h::kv->row(h::evidenceKey))); ASSERT(evidence); EXPECT_EQ(evidence->outcome, "completed"); EXPECT_EQ(evidence->cycles.size(), 3u);
  std::size_t samples = 0; for (const auto& cycle : evidence->cycles) for (const auto& sample : cycle.samples) { ++samples; EXPECT_EQ(sample.raw_usage, sample.usage); }
  EXPECT_EQ(samples, 30u); const auto serialized = rowText(h::evidenceKey);
  EXPECT(!rt::includes(serialized, "account-one")); EXPECT(!rt::includes(serialized, "uos-cache-scope-v5")); EXPECT(!rt::includes(serialized, "cache-scope-cycle")); EXPECT(!rt::includes(serialized, *transport.requests[0].conversation));
}
ASYNC_TEST("prompt-cache scope makes a sample with neither read nor write telemetry inconclusive") {
  h::HarnessScope scope; h::seed(); h::ScopeTransport transport; transport.onInference = [](auto, const auto&, const auto&) { return h::sseCompleted(0, 0); };
  const rt::GlobalFetchOverride fetch(transport.fetcher()); AWAIT(result, h::runExperiment());
  EXPECT_EQ(result.status, "inconclusive"); EXPECT_EQ(result.inconclusive_reason, std::optional<std::string>("invalid_cache_signal")); EXPECT_EQ(transport.requests.size(), 1u); EXPECT_EQ(transport.refreshes, 0u); EXPECT(!hasScope());
}
ASYNC_TEST("prompt-cache scope stops on an unexpected warm cache read") {
  h::HarnessScope scope; h::seed(); h::ScopeTransport transport; transport.onInference = [](auto, const auto&, const auto&) { return h::sseCompleted(2560, 0); };
  const rt::GlobalFetchOverride fetch(transport.fetcher()); AWAIT(result, h::runExperiment());
  EXPECT_EQ(result.status, "inconclusive"); EXPECT_EQ(result.inconclusive_reason, std::optional<std::string>("invalid_cache_signal")); EXPECT_EQ(transport.requests.size(), 1u); EXPECT_EQ(transport.refreshes, 0u); EXPECT(!hasScope());
}
ASYNC_TEST("prompt-cache scope rejects sub-prefix, inconsistent, or impossible counter evidence before account-slot classification") {
  h::HarnessScope scope; const std::vector<std::pair<double, double>> counters{{0, 1}, {1, 2560}, {0, 2048}, {2048, 0}, {0, 3001}};
  for (const auto& [cached, written] : counters) {
    h::seed(); h::ScopeTransport transport; transport.onInference = [cached, written](std::size_t index, const auto&, const auto&) { if (index == 0) return h::sseCompleted(0, 2560); if (index == 1) return h::sseCompleted(2560, 2560); if (index == 2) return h::sseCompleted(cached, written); throw rt::JsError("scope experiment should stop on the discriminating account-slot sample"); };
    const rt::GlobalFetchOverride fetch(transport.fetcher()); AWAIT(result, h::runExperiment()); EXPECT_EQ(result.status, "inconclusive"); EXPECT_EQ(result.inconclusive_reason, std::optional<std::string>("invalid_cache_signal")); EXPECT_EQ(transport.requests.size(), 3u); EXPECT_EQ(transport.refreshes, 0u); EXPECT(!hasScope());
  }
}
ASYNC_TEST("prompt-cache scope rejects mixed cache evidence on every discriminator sample") {
  h::HarnessScope scope; const std::vector<std::size_t> indexes{2, 5, 7};
  for (const auto stop : indexes) {
    h::seed(); h::ScopeTransport transport; transport.onInference = [stop](std::size_t index, const auto&, const auto&) { if (index > stop) throw rt::JsError("scope experiment should stop at the mixed discriminator sample"); const auto step = static_cast<double>(index); return h::sseCompleted(index == stop ? 2560 : h::cycleValue(h::CACHE_READ_CYCLE, step), index == stop ? 2560 : h::cycleValue(h::CACHE_WRITE_CYCLE_LEADING, step)); };
    const rt::GlobalFetchOverride fetch(transport.fetcher()); AWAIT(result, h::runExperiment()); EXPECT_EQ(result.status, "inconclusive"); EXPECT_EQ(result.inconclusive_reason, std::optional<std::string>("invalid_cache_signal")); EXPECT_EQ(transport.requests.size(), stop + 1); EXPECT_EQ(transport.refreshes, stop >= 5 ? 1u : 0u); EXPECT(!hasScope());
  }
}
ASYNC_TEST("a target-scoped cycle lease blocks a concurrent same-target invocation") {
  h::HarnessScope scope; h::seed(); auto [dispatched, markDispatched, rejectDispatched] = rt::withResolvers<void>(); static_cast<void>(rejectDispatched);
  auto [response, release, rejectResponse] = rt::withResolvers<rt::Response>(); static_cast<void>(rejectResponse); std::size_t inferenceCalls = 0;
  const rt::Fetch fetcher([&](rt::RequestInfo input, rt::RequestInit) -> rt::Task<rt::Response> { if (h::fetchInputUrl(input) == "https://auth.openai.com/oauth/token") throw rt::JsError("refresh must not run"); ++inferenceCalls; markDispatched(); const auto result = co_await response.wait(); co_return result; });
  const rt::GlobalFetchOverride fetch(fetcher); rt::Promise<m::PromptCacheScopeExperimentResult> first(h::runExperiment()); co_await dispatched.wait();
  auto second = h::runExperiment(); EXPECT_REJECTS(std::move(second), h::PromptCacheScopeExperimentBusyError); EXPECT_EQ(inferenceCalls, 1u);
  release(h::sseCompleted(0, 0)); const auto result = co_await first.wait(); EXPECT_EQ(result.status, "inconclusive"); EXPECT_EQ(result.inconclusive_reason, std::optional<std::string>("invalid_cache_signal"));
}
ASYNC_TEST("a provider campaign lease blocks a sibling target between cycles and prevents OAuth interleaving") {
  h::HarnessScope scope; const std::string modelA = "gpt-5.5-cache-scope-target-a", modelB = "gpt-5.6-cache-scope-target-b";
  h::SeedOptions options{.models = std::vector<std::string>{modelB, modelA}, .defaultModel = modelB}; h::seed(options); h::ScopeTransport transport; const rt::GlobalFetchOverride fetch(transport.fetcher());
  AWAIT(first, h::runExperiment(modelA)); EXPECT_EQ(first.status, "in_progress"); EXPECT_EQ(first.completed_cycles, 1);
  const auto state = rowText(h::stateKeyFor(modelA)), evidence = rowText(h::evidenceKeyFor(modelA));
  auto sibling = h::runExperiment(modelB); EXPECT_REJECTS(std::move(sibling), h::PromptCacheScopeExperimentBusyError);
  EXPECT_EQ(transport.requests.size(), 10u); EXPECT_EQ(transport.refreshes, 1u); EXPECT(h::kv->row(h::stateKeyFor(modelB)).is_null()); EXPECT(h::kv->row(h::evidenceKeyFor(modelB)).is_null()); EXPECT_EQ(rowText(h::stateKeyFor(modelA)), state); EXPECT_EQ(rowText(h::evidenceKeyFor(modelA)), evidence);
  AWAIT(second, h::runExperiment(modelA)); EXPECT_EQ(second.status, "in_progress"); EXPECT_EQ(second.completed_cycles, 2); EXPECT_EQ(transport.requests.size(), 20u); EXPECT_EQ(transport.refreshes, 2u);
}
ASYNC_TEST("a v4 campaign lease cannot block the v5 plain-key experiment") {
  h::HarnessScope scope; h::seed(); const rt::KvKey legacy{"uos_ai", "prompt_cache_scope_experiment", "v4", "campaign", "codex_chatgpt", "chatgpt_codex", "responses_implicit_input_text_keyed"};
  put(legacy, json::object{{"owner", "legacy-owner"}, {"target_id", "legacy-target"}, {"inventory_fingerprint", "legacy-inventory"}, {"lease_until_ms", rt::makeNumber(rt::dateNow() + 60000)}});
  h::ScopeTransport transport; transport.onInference = [](auto, const auto&, const auto&) { return h::sseCompleted(0, 0); }; const rt::GlobalFetchOverride fetch(transport.fetcher());
  AWAIT(result, h::runExperiment()); EXPECT_EQ(result.status, "inconclusive"); EXPECT_EQ(result.inconclusive_reason, std::optional<std::string>("invalid_cache_signal")); EXPECT_EQ(transport.requests.size(), 1u); EXPECT(h::kv->rows.has(h::encodeKey(legacy)));
}
ASYNC_TEST("an expired target session clears its prior evidence before starting a fresh cycle") {
  h::HarnessScope scope; h::seed(); h::ScopeTransport transport; const rt::GlobalFetchOverride fetch(transport.fetcher()); AWAIT(first, h::runExperiment()); EXPECT_EQ(first.status, "in_progress");
  auto prior = h::kv->row(h::stateKey); auto evidence = h::kv->row(h::evidenceKey); const auto oldOwner = rt::getString(prior, "campaign_owner"); const auto stale = rt::dateNow() - 10000;
  prior.as_object()["started_at_ms"] = rt::makeNumber(stale); prior.as_object()["expires_at_ms"] = rt::makeNumber(stale + 1); evidence.as_object()["started_at_ms"] = rt::makeNumber(stale);
  put(h::stateKey, prior); put(h::evidenceKey, evidence); const auto& target = prior.as_object().at("target");
  put(h::campaignLeaseKey, json::object{{"owner", *oldOwner}, {"target_id", *rt::getString(target, "id")}, {"inventory_fingerprint", *rt::getString(target, "inventory_fingerprint")}, {"lease_until_ms", rt::makeNumber(stale + 1)}});
  AWAIT(restarted, h::runExperiment()); EXPECT_EQ(restarted.status, "in_progress"); EXPECT_EQ(restarted.completed_cycles, 1); EXPECT_EQ(transport.requests.size(), 20u); EXPECT_EQ(transport.refreshes, 2u);
  const auto freshState = h::kv->row(h::stateKey), freshEvidence = h::kv->row(h::evidenceKey); EXPECT(rt::getString(freshState, "campaign_owner") != oldOwner); EXPECT(rt::getNumber(freshState, "started_at_ms") != std::optional(stale)); EXPECT_EQ(rt::getNumber(freshEvidence, "started_at_ms"), rt::getNumber(freshState, "started_at_ms")); EXPECT_EQ(rt::getArray(freshEvidence, "cycles")->size(), 1u);
}
ASYNC_TEST("a same-account credential rotation during forced refresh is inconclusive auth-pool drift") {
  h::HarnessScope scope; h::seed(); h::ScopeTransport transport;
  transport.onRefresh = [](auto) { auto pool = h::kv->row(h::CODEX_AUTH_POOL_KV_KEY); auto& account = pool.as_object().at("accounts").as_array()[0].as_object(); account["access_token"] = "externally-rotated-access"; account["refresh_token"] = "externally-rotated-refresh"; account["updated_at_ms"] = rt::makeNumber(rt::dateNow()); put(h::CODEX_AUTH_POOL_KV_KEY, pool); return rt::Response::json(json::object{{"access_token", "experiment-refresh-access"}, {"refresh_token", "experiment-refresh-refresh"}}); };
  const rt::GlobalFetchOverride fetch(transport.fetcher()); AWAIT(result, h::runExperiment()); EXPECT_EQ(result.status, "inconclusive"); EXPECT_EQ(result.inconclusive_reason, std::optional<std::string>("auth_pool_drift")); EXPECT_EQ(transport.requests.size(), 5u); EXPECT_EQ(transport.refreshes, 1u); EXPECT(!hasScope());
}
ASYNC_TEST("malformed v3 target state or evidence fails closed before any outbound work") {
  h::HarnessScope scope; h::ScopeTransport transport; const rt::GlobalFetchOverride fetch(transport.fetcher());
  for (int kind = 0; kind < 2; ++kind) {
    h::seed(); AWAIT(baseline, h::scopeBaselineFor()); const auto key = kind == 0 ? h::stateKey : h::evidenceKey;
    const json::object malformed = kind == 0 ? json::object{{"v", 3}, {"target", json::value_from(baseline.target)}, {"next_cycle", 2}} : json::object{{"v", 3}, {"target", json::value_from(baseline.target)}, {"outcome", "completed"}, {"started_at_ms", rt::makeNumber(rt::dateNow())}, {"verified_at_ms", rt::makeNumber(rt::dateNow())}, {"cycles", json::array{}}};
    put(key, malformed); const auto before = rowText(key); auto task = h::runPromptCacheScopeExperiment(baseline); EXPECT_REJECTS(std::move(task), h::PromptCacheScopeExperimentUnavailableError); EXPECT_EQ(rowText(key), before);
  }
  EXPECT_EQ(transport.requests.size(), 0u); EXPECT_EQ(transport.refreshes, 0u);
}
ASYNC_TEST("semantically malformed v3 timestamps fail closed before any outbound work") {
  h::HarnessScope scope; h::ScopeTransport transport; const rt::GlobalFetchOverride fetch(transport.fetcher());
  for (int kind = 0; kind < 3; ++kind) {
    h::seed(); AWAIT(baseline, h::scopeBaselineFor()); const auto now = rt::dateNow(); const auto key = kind == 0 ? h::stateKey : h::evidenceKey;
    const json::object malformed = kind == 0 ? json::object{{"v", 3}, {"target", json::value_from(baseline.target)}, {"campaign_owner", "timestamp-owner"}, {"started_at_ms", rt::makeNumber(now + 60000)}, {"expires_at_ms", rt::makeNumber(now + 120000)}, {"auth_pool_versionstamp", baseline.target.auth_pool_versionstamp}, {"next_cycle", 1}, {"classifications", json::array{}}} : json::object{{"v", 3}, {"target", json::value_from(baseline.target)}, {"outcome", "failed"}, {"started_at_ms", rt::makeNumber(kind == 1 ? now : now + 60000)}, {"verified_at_ms", rt::makeNumber(kind == 1 ? now - 1 : now + 120000)}, {"cycles", json::array{}}};
    put(key, malformed); if (kind == 0) put(h::campaignLeaseKey, json::object{{"owner", "timestamp-owner"}, {"target_id", baseline.target.id}, {"inventory_fingerprint", baseline.target.inventory_fingerprint}, {"lease_until_ms", rt::makeNumber(now + 120000)}});
    const auto before = rowText(key); const auto options = baselineOptions(); auto gate = h::assertPromptCacheScopeExperimentTelemetryBaseline(options); EXPECT_REJECTS(std::move(gate), h::PromptCacheScopeExperimentUnavailableError); auto task = h::runPromptCacheScopeExperiment(baseline); EXPECT_REJECTS(std::move(task), h::PromptCacheScopeExperimentUnavailableError); EXPECT_EQ(rowText(key), before);
  }
  EXPECT_EQ(transport.requests.size(), 0u); EXPECT_EQ(transport.refreshes, 0u);
}
ASYNC_TEST("semantically inconsistent ready-to-promote state cannot publish a scope") {
  h::HarnessScope scope; h::seed(); AWAIT(baseline, h::scopeBaselineFor()); const auto now = rt::dateNow();
  const m::ConcreteScopeObservation observation{"responses_implicit_input_text_keyed_cycle_isolated_v5", "account_scoped", "preserved", "independent", std::string(h::MODEL)};
  auto disagreement = observation; disagreement.account_slots = "shared";
  const m::ExperimentState state{3, baseline.target, "inconsistent-ready-owner", now, now + 60000, baseline.target.auth_pool_versionstamp, 4, {observation, disagreement, observation}, observation};
  put(h::stateKey, json::value_from(state)); put(h::campaignLeaseKey, json::object{{"owner", state.campaign_owner}, {"target_id", baseline.target.id}, {"inventory_fingerprint", baseline.target.inventory_fingerprint}, {"lease_until_ms", rt::makeNumber(now + 60000)}});
  h::ScopeTransport transport; const rt::GlobalFetchOverride fetch(transport.fetcher()); auto task = h::runPromptCacheScopeExperiment(baseline); EXPECT_REJECTS(std::move(task), h::PromptCacheScopeExperimentUnavailableError); EXPECT_EQ(transport.requests.size(), 0u); EXPECT_EQ(transport.refreshes, 0u); EXPECT(!hasScope());
}
ASYNC_TEST("a completed target advances the bodyless campaign to the next exact model") {
  h::HarnessScope scope; const std::string modelA = "gpt-5.5-cache-scope-completed-a", modelB = "gpt-5.6-cache-scope-pending-b";
  h::SeedOptions options{.models = std::vector<std::string>{modelB, modelA}, .defaultModel = modelB}; h::seed(options); h::ScopeTransport transport; const rt::GlobalFetchOverride fetch(transport.fetcher());
  AWAIT(first, h::runExperiment(modelA)); AWAIT(second, h::runExperiment(modelA)); AWAIT(third, h::runExperiment(modelA)); EXPECT_EQ(first.status, "in_progress"); EXPECT_EQ(second.status, "in_progress"); EXPECT_EQ(third.status, "completed"); EXPECT_EQ(transport.requests.size(), 30u); EXPECT_EQ(transport.refreshes, 3u);
  EXPECT_EQ(rt::getString(h::kv->row(h::evidenceKeyFor(modelA)), "outcome"), std::optional<std::string>("completed")); auto seeding = seedBaseline(modelB); co_await std::move(seeding); const auto baselineOpts = baselineOptions(); AWAIT(baseline, h::assertPromptCacheScopeExperimentTelemetryBaseline(baselineOpts)); EXPECT_EQ(baseline.target.model, modelB);
}
ASYNC_TEST("an inconclusive target is retried before the campaign advances to another model") {
  h::HarnessScope scope; const std::string modelA = "gpt-5.5-cache-scope-retry-a", modelB = "gpt-5.6-cache-scope-pending-b";
  h::SeedOptions options{.models = std::vector<std::string>{modelB, modelA}, .defaultModel = modelB}; h::seed(options); h::ScopeTransport transport; transport.onInference = [modelA](auto, const auto&, const auto&) { return h::sseCompleted(0, 0, modelA); }; const rt::GlobalFetchOverride fetch(transport.fetcher());
  AWAIT(result, h::runExperiment(modelA)); EXPECT_EQ(result.status, "inconclusive"); auto seedA = seedBaseline(modelA); co_await std::move(seedA); auto seedB = seedBaseline(modelB); co_await std::move(seedB); const auto baselineOpts = baselineOptions(); AWAIT(baseline, h::assertPromptCacheScopeExperimentTelemetryBaseline(baselineOpts)); EXPECT_EQ(baseline.target.model, modelA);
}
ASYNC_TEST("completed evidence survives token rotation but is reprobeable after an account-pool membership change") {
  h::HarnessScope scope; h::seed(); h::ScopeTransport transport; const rt::GlobalFetchOverride fetch(transport.fetcher()); AWAIT(first, h::runExperiment()); AWAIT(second, h::runExperiment()); AWAIT(third, h::runExperiment()); EXPECT_EQ(third.status, "completed"); static_cast<void>(first); static_cast<void>(second);
  auto pool = h::kv->row(h::CODEX_AUTH_POOL_KV_KEY); auto& firstAccount = pool.as_object().at("accounts").as_array()[0].as_object(); firstAccount["access_token"] = "token-only-rotation-access"; firstAccount["refresh_token"] = "token-only-rotation-refresh"; put(h::CODEX_AUTH_POOL_KV_KEY, pool);
  AWAIT(tokenBaseline, h::scopeBaselineFor()); const auto evidence = m::parseEvidence(rt::KvValue(h::kv->row(h::evidenceKey))); ASSERT(evidence); EXPECT_EQ(evidence->target.auth_pool_identity_fingerprint, tokenBaseline.target.auth_pool_identity_fingerprint);
  auto seeding = seedBaseline(); co_await std::move(seeding); const auto options = baselineOptions(); auto gate = h::assertPromptCacheScopeExperimentTelemetryBaseline(options); EXPECT_REJECTS(std::move(gate), h::PromptCacheScopeExperimentUnavailableError); EXPECT_EQ(transport.requests.size(), 30u);
  pool = h::kv->row(h::CODEX_AUTH_POOL_KV_KEY); auto& member = pool.as_object().at("accounts").as_array()[0].as_object(); member["account_id"] = "replacement-account-id"; member["access_token"] = "membership-change-access"; member["refresh_token"] = "membership-change-refresh"; put(h::CODEX_AUTH_POOL_KV_KEY, pool);
  AWAIT(membership, h::assertPromptCacheScopeExperimentTelemetryBaseline(options)); EXPECT(membership.target.auth_pool_identity_fingerprint != tokenBaseline.target.auth_pool_identity_fingerprint); AWAIT(restarted, h::runPromptCacheScopeExperiment(membership)); EXPECT_EQ(restarted.status, "in_progress"); EXPECT_EQ(transport.requests.size(), 40u);
}
ASYNC_TEST("prompt-cache scope Stage 0 baseline uses the Codex telemetry provider identity") {
  h::HarnessScope scope; h::seed(); auto seeding = seedBaseline(); co_await std::move(seeding); const auto options = baselineOptions(); AWAIT(baseline, h::assertPromptCacheScopeExperimentTelemetryBaseline(options)); EXPECT_EQ(baseline.target.telemetry_provider, "chatgpt_codex");
}
ASYNC_TEST("prompt-cache scope Stage 0 baseline rejects reported usage without cache-write fields before any scope traffic") {
  h::HarnessScope scope; h::seed(); const uos::cache::telemetry_gate::PromptCacheTelemetryBaselineTarget target{"chatgpt_codex", std::string(h::MODEL)}; const uos::cache::telemetry_gate::PromptCacheTelemetryGateOptions keyOptions{.release = std::string(h::TELEMETRY_RELEASE)};
  AWAIT(keys, h::resolvePromptCacheTelemetryCounterKeys(target, keyOptions)); ASSERT(keys); for (const auto& route : keys->routes) { h::kv->put(route.completed, rt::KvU64(5000)); h::kv->put(route.reported, rt::KvU64(5000)); }
  h::ScopeTransport transport; const rt::GlobalFetchOverride fetch(transport.fetcher()); const auto options = baselineOptions(); auto task = h::assertPromptCacheScopeExperimentTelemetryBaseline(options); EXPECT_REJECTS(std::move(task), h::PromptCacheScopeExperimentUnavailableError); EXPECT_EQ(transport.requests.size(), 0u); EXPECT_EQ(transport.refreshes, 0u);
}
ASYNC_TEST("prompt-cache scope Stage 0 baseline requires Responses telemetry for its paid transport") {
  h::HarnessScope scope; h::seed(); const uos::cache::telemetry_gate::PromptCacheTelemetryBaselineTarget target{"chatgpt_codex", std::string(h::MODEL)}; const uos::cache::telemetry_gate::PromptCacheTelemetryGateOptions keyOptions{.release = std::string(h::TELEMETRY_RELEASE)};
  AWAIT(keys, h::resolvePromptCacheTelemetryCounterKeys(target, keyOptions)); ASSERT(keys); for (const auto& route : keys->routes) if (route.route == "chat.completions") { h::kv->put(route.completed, rt::KvU64(10000)); h::kv->put(route.reported, rt::KvU64(10000)); h::kv->put(route.cache_write_reported, rt::KvU64(10000)); }
  h::ScopeTransport transport; const rt::GlobalFetchOverride fetch(transport.fetcher()); const auto options = baselineOptions(); auto task = h::assertPromptCacheScopeExperimentTelemetryBaseline(options); EXPECT_REJECTS(std::move(task), h::PromptCacheScopeExperimentUnavailableError); EXPECT_EQ(transport.requests.size(), 0u); EXPECT_EQ(transport.refreshes, 0u);
}
ASYNC_TEST("a catalog roster change after the Stage 0 gate stops before paid scope traffic") {
  h::HarnessScope scope; h::seed(); auto seeding = seedBaseline(); co_await std::move(seeding); const auto options = baselineOptions(); AWAIT(baseline, h::assertPromptCacheScopeExperimentTelemetryBaseline(options));
  auto snapshot = h::kv->row(h::CODEX_MODELS_KV_KEY); snapshot.as_object().at("models").as_array().push_back(h::modelRecord(std::string(h::MODEL) + "-alternate-after-gate")); put(h::CODEX_MODELS_KV_KEY, snapshot);
  h::ScopeTransport transport; const rt::GlobalFetchOverride fetch(transport.fetcher()); auto task = h::runPromptCacheScopeExperiment(baseline); EXPECT_REJECTS(std::move(task), h::PromptCacheScopeExperimentUnavailableError); EXPECT_EQ(transport.requests.size(), 0u); EXPECT_EQ(transport.refreshes, 0u); EXPECT(h::kv->row(h::stateKey).is_null());
}
ASYNC_TEST("a runtime default-only switch preserves a selected non-default target") {
  h::HarnessScope scope; const std::string targetModel = "gpt-5.5-cache-scope-non-default", firstDefault = "gpt-5.7-cache-scope-default-one", secondDefault = "gpt-5.8-cache-scope-default-two";
  h::SeedOptions seedOptions{.models = std::vector<std::string>{targetModel, firstDefault, secondDefault}, .defaultModel = firstDefault}; h::seed(seedOptions); auto seeding = seedBaseline(targetModel); co_await std::move(seeding); const auto options = baselineOptions(); AWAIT(baseline, h::assertPromptCacheScopeExperimentTelemetryBaseline(options)); EXPECT_EQ(baseline.target.model, targetModel);
  h::ScopeTransport transport; transport.onInference = [secondDefault, targetModel](std::size_t index, const auto&, const auto&) { if (index == 0) { auto runtime = h::kv->row(h::RUNTIME_CONFIG_V2_KEY); runtime.as_object()["default_model"] = secondDefault; put(h::RUNTIME_CONFIG_V2_KEY, runtime); } const auto step = static_cast<double>(index % 10); return h::sseCompleted(h::cycleValue(h::CACHE_READ_CYCLE, step), h::cycleValue(h::CACHE_WRITE_CYCLE_ALTERNATING, step), targetModel); };
  const rt::GlobalFetchOverride fetch(transport.fetcher()); AWAIT(result, h::runPromptCacheScopeExperiment(baseline)); EXPECT_EQ(result.status, "in_progress"); EXPECT_EQ(result.model, targetModel); EXPECT_EQ(transport.requests.size(), 10u); EXPECT_EQ(transport.refreshes, 1u); for (const auto& request : transport.requests) EXPECT_EQ(rt::getString(rt::jsonParse(request.body), "model"), std::optional(targetModel)); EXPECT_EQ(rt::getString(h::kv->row(h::RUNTIME_CONFIG_V2_KEY), "default_model"), std::optional(secondDefault));
}
ASYNC_TEST("a catalog roster drift during a scope cycle stops before the next paid sample") {
  h::HarnessScope scope; h::seed(); h::ScopeTransport transport; transport.onInference = [](auto, const auto&, const auto&) { auto snapshot = h::kv->row(h::CODEX_MODELS_KV_KEY); snapshot.as_object().at("models").as_array().push_back(h::modelRecord(std::string(h::MODEL) + "-alternate-during-cycle")); put(h::CODEX_MODELS_KV_KEY, snapshot); auto runtime = h::kv->row(h::RUNTIME_CONFIG_V2_KEY); put(h::RUNTIME_CONFIG_V2_KEY, runtime); return h::sseCompleted(0, 2560); };
  const rt::GlobalFetchOverride fetch(transport.fetcher()); AWAIT(result, h::runExperiment()); EXPECT_EQ(result.status, "inconclusive"); EXPECT_EQ(result.inconclusive_reason, std::optional<std::string>("inventory_drift")); EXPECT_EQ(transport.requests.size(), 1u); EXPECT_EQ(transport.refreshes, 0u); EXPECT(!hasScope());
}
ASYNC_TEST("a target capability drift stops before the next paid sample") {
  h::HarnessScope scope; h::seed(); h::ScopeTransport transport; transport.onInference = [](auto, const auto&, const auto&) { auto snapshot = h::kv->row(h::CODEX_MODELS_KV_KEY); auto controls = h::cacheControls(); controls["expected_usage_fields"] = json::array{"cached_tokens"}; snapshot.as_object()["models"] = json::array{h::modelRecord(std::string(h::MODEL), controls)}; put(h::CODEX_MODELS_KV_KEY, snapshot); return h::sseCompleted(0, 2560); };
  const rt::GlobalFetchOverride fetch(transport.fetcher()); AWAIT(result, h::runExperiment()); EXPECT_EQ(result.status, "inconclusive"); EXPECT_EQ(result.inconclusive_reason, std::optional<std::string>("target_catalog_drift")); EXPECT_EQ(transport.requests.size(), 1u); EXPECT_EQ(transport.refreshes, 0u); EXPECT(!hasScope());
}
