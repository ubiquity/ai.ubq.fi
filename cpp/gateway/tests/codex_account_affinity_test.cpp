// Links: tests/codex_account_affinity_test.cpp tests/helpers/codex_account_routing_harness.cpp tests/helpers/counting_kv.cpp src/auth/relay.cpp
//        src/authority/native_codex.cpp src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp
//        src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp
//        src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp src/bounded_response_body.cpp src/cache/prompt_analytics.cpp
//        src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp
//        src/cache/telemetry_gate.cpp src/codex/account_routing.cpp src/codex/auth.cpp src/codex/auth_refresh.cpp src/codex/banked_reset.cpp
//        src/codex/banked_reset_provider.cpp src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp
//        src/codex/external_auth.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp
//        src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp
//        src/codex/routing_serial.cpp src/codex/routing_state.cpp src/config.cpp src/http.cpp src/inference_deadline.cpp src/kv.cpp
//        src/metered_quota.cpp src/models/codex_models_availability.cpp src/optional_telemetry_queue.cpp src/paid_fallback/ledger_state.cpp
//        src/paid_fallback/ledger_stats.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp
//        src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp
//        src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp
//        src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_quota.cpp
//        src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/selection.cpp src/release.cpp src/runtime/http.cpp
//        src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp
//        src/runtime/websocket.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp src/storage/native_kv.cpp
//        src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/types.cpp src/utils.cpp src/codex/index.cpp
//        src/codex/responses_state.cpp src/codex/responses_operations.cpp src/codex/responses_runners.cpp src/codex/banked_reset_pool.cpp
//        src/codex/banked_reset_submission.cpp src/codex/banked_reset_claim.cpp
// Port of tests/codex-account-affinity.test.ts: all three original cases and assertions.
// C1's RoutingKv already preserves the fixture's per-key decimal versions and synchronous getMany snapshots.
#include "tests/helpers/codex_account_routing_harness.hpp"
#include "uos/config.hpp"
#include "uos/models/codex_models_availability.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/utils.hpp"

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace harness = uos::tests::helpers::codex_account_routing_harness;
  namespace codex = uos::codex::index;
  using uos::types::CodexAuthState;
  using uos::types::CodexAuthPoolState;
  const rt::KvKey OLD_AFFINITY_PREFIX{"uos_ai", "codex_account_affinity", "v1"};

  class AffinityKv final : public harness::RoutingKv {
  public:
    void put(const rt::KvKey& kvKey, const rt::KvValue& value) {
      const std::string encoded = harness::key(kvKey);
      const double* version = versions.get(encoded);
      const double next = (version ? *version : 0) + 1;
      values.set(encoded, value);
      versions.set(encoded, next);
    }
    std::string stored(const rt::KvKey& kvKey) const {
      const std::string encoded = harness::key(kvKey);
      const auto* value = values.get(encoded);
      const auto* version = versions.get(encoded);
      if (!value || !version) return "undefined";
      return rt::jsonStringify(json::object{{"value", value->toJson()}, {"version", rt::makeNumber(*version)}});
    }
  };
  CodexAuthState account(const std::string& label) {
    return {.access_token = "access-" + label, .refresh_token = "refresh-" + label, .account_id = "account-" + label, .updated_at_ms = rt::dateNow()};
  }
  json::object oldAffinityRow(const std::string& targetAccountId) {
    const std::string cohort = std::string("uos-prompt-cache-account-cohort-v1") + '\0' + targetAccountId;
    return {{"account_cohort_hash", cohort}, {"expires_at_ms", rt::makeNumber(rt::dateNow() + 60 * 60'000)}};
  }
  rt::Task<rt::KvKey> oldRowKey() {
    const std::vector<std::string> parts{"uos_ai", "codex_account_affinity_identity_v1", "fixture-principal", "fixture-key"};
    const std::string identity = rt::join(parts, std::string(1, '\0'));
    rt::Promise<std::string> hashing(uos::utils::sha256Hex(identity));
    const std::string hashed = co_await hashing.wait();
    const rt::KvKey key{OLD_AFFINITY_PREFIX, hashed};
    co_return key;
  }
  rt::Response jsonResponse(const json::object& value) {
    return rt::Response(rt::BodyInit(rt::jsonStringify(value)), rt::ResponseInit{.status = 200});
  }
  struct Scope {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{}};
    std::string savedSeed = uos::config::config.codexAuthJsonB64;
    bool savedDeploy = uos::config::config.isDeploy;
    std::shared_ptr<AffinityKv> kv = std::make_shared<AffinityKv>();
    Scope() {
      uos::config::config.codexAuthJsonB64.clear();
      uos::config::config.isDeploy = true;
      uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest([](const uos::provider::capacity_events::ProviderCapacitySampleTriggerOptions&) {});
      uos::kv::setKvForTest(kv);
      codex::resetCodexAuthCacheForTest();
      harness::resetCodexAccountRoutingForTest();
      uos::models::codex_models_availability::resetCodexAccountModelsCacheForTest();
    }
    ~Scope() {
      uos::kv::setKvForTest(nullptr);
      codex::resetCodexAuthCacheForTest();
      harness::resetCodexAccountRoutingForTest();
      uos::models::codex_models_availability::resetCodexAccountModelsCacheForTest();
      uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest({});
      uos::config::config.codexAuthJsonB64 = savedSeed;
      uos::config::config.isDeploy = savedDeploy;
    }
    void seedPool() {
      const CodexAuthPoolState pool{.accounts = {account("one"), account("two")}, .updated_at_ms = rt::dateNow()};
      kv->put(codex::CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(pool)));
    }
  };
  void expectActive(const rt::Response& response, double generation, const std::optional<std::string>& reason) {
    const auto telemetry = codex::getCodexResponseActiveTelemetry(response);
    EXPECT_EQ(telemetry.activeGeneration, std::optional<double>(generation));
    EXPECT_EQ(telemetry.activeTransitionReason, reason);
  }
  uos::codex::dispatch::FetchCodexResponsesOptions requestOptions() {
    uos::codex::dispatch::FetchCodexResponsesOptions options; options.cacheScope = "fixture-principal"; return options;
  }
  rt::Task<void> completeResponse(rt::Response response) {
    rt::Promise<void> marking(codex::markCodexResponseCompleted(response)); co_await marking.wait();
    auto reading = response.arrayBuffer(); co_await std::move(reading);
  }
}

ASYNC_TEST("opposing old persisted affinity rows never split ordinary traffic") {
  Scope scope;
  scope.seedPool();
  rt::Promise<rt::KvKey> identifying(oldRowKey()); const auto key = co_await identifying.wait();
  const auto oldRow = oldAffinityRow("account-two"); scope.kv->put(key, rt::KvValue(oldRow));
  const std::string storedBefore = scope.kv->stored(key);
  std::vector<std::string> calls;
  std::vector<std::optional<std::string>> sessions;
  const rt::Fetch mock([&](rt::RequestInfo input, rt::RequestInit init) {
    if (!rt::endsWith(harness::requestUrl(input), "/responses")) {
      const rt::Response unexpected(rt::BodyInit(std::string("unexpected")), rt::ResponseInit{.status = 404});
      return rt::Promise<rt::Response>::resolve(unexpected).wait();
    }
    const rt::Headers headers = init.headers.value_or(rt::Headers());
    calls.push_back(headers.get("ChatGPT-Account-ID").value_or("")); sessions.push_back(headers.get("session-id"));
    return rt::Promise<rt::Response>::resolve(jsonResponse(json::object{{"id", "response-" + std::to_string(calls.size())}})).wait();
  });
  const rt::GlobalFetchOverride fetch(mock);
  const std::vector<json::value> bodies{
    json::object{{"model", "gpt-affinity-fixture"}, {"input", "same-key-1"}, {"prompt_cache_key", "fixture-key"}},
    json::object{{"model", "gpt-affinity-fixture"}, {"input", "same-key-2"}, {"prompt_cache_key", "fixture-key"}},
    json::object{{"model", "gpt-affinity-fixture"}, {"input", "different-key"}, {"prompt_cache_key", "other-key"}},
    json::object{{"model", "gpt-affinity-fixture"}, {"input", "unkeyed"}}};
  const auto options = requestOptions();
  for (const auto& body : bodies) {
    rt::Promise<rt::Response> fetching(codex::fetchCodexResponses(body, options)); const auto response = co_await fetching.wait();
    EXPECT_EQ(response.status(), 200); expectActive(response, 1, std::nullopt); EXPECT_EQ(codex::getCodexResponseSlot(response), std::optional<double>(1));
    auto completing = completeResponse(response); co_await std::move(completing);
  }
  EXPECT_EQ(calls, (std::vector<std::string>{"account-one", "account-one", "account-one", "account-one"}));
  ASSERT_EQ(sessions.size(), std::size_t{4}); EXPECT_EQ(sessions[0], sessions[1]); EXPECT(sessions[1] != sessions[2]); EXPECT_EQ(sessions[3], std::nullopt);
  EXPECT_EQ(scope.kv->stored(key), storedBefore);
}

ASYNC_TEST("a retired affinity row cannot keep traffic on an authoritatively exhausted account") {
  Scope scope; scope.seedPool();
  rt::Promise<rt::KvKey> identifying(oldRowKey()); const auto key = co_await identifying.wait();
  scope.kv->put(key, rt::KvValue(oldAffinityRow("account-one"))); const auto storedBefore = scope.kv->stored(key);
  std::vector<std::string> calls;
  const rt::Fetch mock([&](rt::RequestInfo input, rt::RequestInit init) {
    if (!rt::endsWith(harness::requestUrl(input), "/responses")) {
      const rt::Response unexpected(rt::BodyInit(std::string("unexpected")), rt::ResponseInit{.status = 404}); return rt::Promise<rt::Response>::resolve(unexpected).wait();
    }
    const auto accountId = init.headers.value_or(rt::Headers()).get("ChatGPT-Account-ID").value_or(""); calls.push_back(accountId);
    if (accountId == "account-one") return rt::Promise<rt::Response>::resolve(harness::httpDateQuotaResponse(rt::dateNow() + 3'600'000)).wait();
    return rt::Promise<rt::Response>::resolve(jsonResponse(json::object{{"id", "served-by-sibling"}})).wait();
  });
  const rt::GlobalFetchOverride fetch(mock);
  const json::value body = json::object{{"model", "gpt-affinity-fixture"}, {"input", "exhaust"}, {"prompt_cache_key", "fixture-key"}};
  const auto options = requestOptions();
  rt::Promise<rt::Response> fetching(codex::fetchCodexResponses(body, options)); const auto first = co_await fetching.wait();
  EXPECT_EQ(first.status(), 200); EXPECT_EQ(calls, (std::vector<std::string>{"account-one", "account-two"}));
  expectActive(first, 2, std::string("quota_exhausted")); auto completing = completeResponse(first); co_await std::move(completing);
  const json::value nextBody = json::object{{"model", "gpt-affinity-fixture"}, {"input", "after-switch"}, {"prompt_cache_key", "fixture-key"}};
  rt::Promise<rt::Response> nextFetching(codex::fetchCodexResponses(nextBody, options)); const auto second = co_await nextFetching.wait();
  EXPECT_EQ(second.status(), 200); ASSERT(!calls.empty()); EXPECT_EQ(calls.back(), "account-two");
  auto nextCompleting = completeResponse(second); co_await std::move(nextCompleting); EXPECT_EQ(scope.kv->stored(key), storedBefore);
}

ASYNC_TEST("a late retired affinity row cannot restore the former active account") {
  Scope scope; scope.seedPool(); std::vector<std::string> calls;
  const rt::Fetch mock([&](rt::RequestInfo input, rt::RequestInit init) {
    if (!rt::endsWith(harness::requestUrl(input), "/responses")) {
      const rt::Response unexpected(rt::BodyInit(std::string("unexpected")), rt::ResponseInit{.status = 404}); return rt::Promise<rt::Response>::resolve(unexpected).wait();
    }
    const auto accountId = init.headers.value_or(rt::Headers()).get("ChatGPT-Account-ID").value_or(""); calls.push_back(accountId);
    if (accountId == "account-one") return rt::Promise<rt::Response>::resolve(harness::httpDateQuotaResponse(rt::dateNow() + 60'000)).wait();
    return rt::Promise<rt::Response>::resolve(jsonResponse(json::object{{"id", "sibling"}})).wait();
  });
  const rt::GlobalFetchOverride fetch(mock);
  const json::value body = json::object{{"model", "gpt-affinity-fixture"}, {"input", "switch"}};
  rt::Promise<rt::Response> fetching(codex::fetchCodexResponses(body)); const auto switched = co_await fetching.wait(); EXPECT_EQ(switched.status(), 200);
  auto completing = completeResponse(switched); co_await std::move(completing); EXPECT_EQ(calls, (std::vector<std::string>{"account-one", "account-two"}));
  const std::string lateIdentity = "late-affinity-identity";
  rt::Promise<std::string> hashing(uos::utils::sha256Hex(lateIdentity)); const auto digest = co_await hashing.wait();
  const rt::KvKey lateKey{OLD_AFFINITY_PREFIX, digest}; scope.kv->put(lateKey, rt::KvValue(oldAffinityRow("account-one")));
  const json::value nextBody = json::object{{"model", "gpt-affinity-fixture"}, {"input", "after-late-row"}};
  rt::Promise<rt::Response> nextFetching(codex::fetchCodexResponses(nextBody)); const auto next = co_await nextFetching.wait(); EXPECT_EQ(next.status(), 200);
  ASSERT(!calls.empty()); EXPECT_EQ(calls.back(), "account-two"); expectActive(next, 2, std::string("quota_exhausted"));
  auto nextCompleting = completeResponse(next); co_await std::move(nextCompleting);
}
