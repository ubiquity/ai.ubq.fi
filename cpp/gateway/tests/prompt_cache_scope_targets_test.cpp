// Links: tests/prompt_cache_scope_targets_test.cpp
// src/auth/relay.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp
// src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp
// src/authority/native_codex_mutations.cpp src/bounded_response_body.cpp src/cache/prompt_analytics_core.cpp
// src/cache/prompt_analytics_read.cpp src/codex/account_routing.cpp src/codex/auth.cpp
// src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_provider.cpp
// src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp
// src/codex/external_auth.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp
// src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp
// src/codex/responses_error.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp
// src/codex/routing_serial.cpp src/codex/routing_state.cpp src/config.cpp
// src/deepseek/index.cpp src/deepseek/responses.cpp src/defaults.cpp
// src/http.cpp src/kv.cpp src/metered_quota.cpp
// src/models/codex_models.cpp src/models/codex_models_availability.cpp src/models/codex_models_fetch.cpp
// src/openai_telemetry.cpp src/paid_fallback/ledger_stats.cpp src/provider/anthropic_chat_responses.cpp
// src/provider/anthropic_chat_stream.cpp src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp
// src/provider/anthropic_responses.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp
// src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp
// src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp src/provider/cerebras.cpp
// src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp
// src/provider/claude_code.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp
// src/provider/claude_code_models.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp
// src/provider/health.cpp src/provider/lithos.cpp src/provider/lithos_rate_limits.cpp
// src/provider/metered.cpp src/provider/removed_provider_telemetry.cpp src/provider/selection.cpp
// src/provider/surplus.cpp src/provider/surplus_claude_chat_request.cpp src/provider/surplus_claude_request.cpp
// src/release.cpp src/request_policy.cpp src/responses_failover_stream.cpp
// src/responses_stream.cpp src/runtime/websocket.cpp src/runtime_config.cpp
// src/storage/kv.cpp src/storage/native_kv.cpp
// src/storage/sqlite_readonly.cpp src/types.cpp src/utils.cpp
// src/cache/scope_targets.cpp src/deepseek/forwarded_payload_policy.cpp src/deepseek/responses_payload.cpp
// src/deepseek/chat_projection.cpp src/deepseek/responses_stream.cpp src/deepseek/flash_aliases.cpp
// src/provider/lithos_models.cpp src/lib/jev_compaction/state.cpp
// src/models/openrouter_models.cpp src/lib/jev_compaction/types.cpp
// Owned API parity from the original Bun tests. Upstream/handler assertions remain separate.
#include "helpers/deepseek_port_replay.hpp"
namespace {
#include "prompt_cache_scope_targets_vectors.inc"
  const bool recorded = uos::tests::helpers::deepseek_port_replay::registerReplay(recordedVectors);
}

#include "uos/codex/auth.hpp"
namespace {
  namespace st = uos::cache::scope_targets;
  namespace rt = uos::rt;
  namespace json = uos::json;
  class ReadOnlyKv : public rt::ForwardingKv {
  public:
    explicit ReadOnlyKv(rt::Kv& store) : rt::ForwardingKv(store) {}
    using rt::Kv::get;
    std::vector<rt::KvKey> reads;
    double listCalls = 0;
    double writeCalls = 0;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override {
      EXPECT(options.consistency == rt::KvConsistencyLevel::strong);
      reads.push_back(key);
      auto value = co_await inner.get(key, options); co_return value;
    }
    rt::Task<rt::KvCommitResult> set(rt::KvKey, rt::KvValue, rt::KvSetOptions = {}) override { ++writeCalls; throw rt::JsError("target inventory must not write KV records"); co_return rt::KvCommitResult{}; }
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector, rt::KvListOptions = {}) override { ++listCalls; throw rt::JsError("target inventory must not list KV records"); }
  };
}
ASYNC_TEST("the inventory loader has a two-key read-only KV footprint") {
  rt::env::ScopedEnvironment environment({{"HOME", "/nonexistent/w05c6-scope"}});
  const auto previous = uos::config::config.codexAuthJsonB64; uos::config::config.codexAuthJsonB64.clear();
  const auto restore = rt::scopeExit([previous] { uos::config::config.codexAuthJsonB64 = previous; });
  auto store = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
  const auto snapshot = rt::jsonParse(R"({"source":"chatgpt_codex","updated_at_ms":1,"client_version":"0.201.0","models":[{"slug":"gpt-5.6","prompt_cache":{"version":1,"providers":[{"id":"codex_chatgpt","controls":{"key":true,"expected_usage_fields":["cached_tokens","cache_write_tokens"],"source":"catalog","verified_at_ms":1}}]}}]})");
  const auto pool = rt::jsonParse(R"({"accounts":[{"access_token":"fixture-access-1","refresh_token":"fixture-refresh-1","account_id":"fixture-account-1","updated_at_ms":1},{"access_token":"fixture-access-2","refresh_token":"fixture-refresh-2","account_id":"fixture-account-2","updated_at_ms":1}],"updated_at_ms":1})");
  auto catalogValue = rt::KvValue(snapshot), poolValue = rt::KvValue(pool);
  auto catalogTask = store->set(uos::codex::auth::CODEX_MODELS_KV_KEY, catalogValue); co_await std::move(catalogTask);
  auto poolTask = store->set(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY, poolValue); co_await std::move(poolTask);
  auto proxy = std::make_shared<ReadOnlyKv>(*store);
  st::LoadPromptCacheScopeTargetInventoryOptions options{.kv = proxy};
  auto task = st::loadPromptCacheScopeTargetInventory(options); const auto inventory = co_await std::move(task);
  EXPECT_EQ(inventory.status, "ready"); EXPECT_EQ(proxy->reads.size(), 2u);
  EXPECT(std::find(proxy->reads.begin(), proxy->reads.end(), uos::codex::auth::CODEX_MODELS_KV_KEY) != proxy->reads.end());
  EXPECT(std::find(proxy->reads.begin(), proxy->reads.end(), uos::codex::auth::CODEX_AUTH_POOL_KV_KEY) != proxy->reads.end());
  EXPECT_EQ(proxy->listCalls, 0); EXPECT_EQ(proxy->writeCalls, 0);
  st::LoadPromptCacheScopeTargetInventoryOptions unavailable{.kv = std::shared_ptr<rt::Kv>{}};
  auto missingTask = st::loadPromptCacheScopeTargetInventory(unavailable); const auto missing = co_await std::move(missingTask);
  EXPECT_EQ(missing.reason, "kv_unavailable");
}
namespace {
  struct PendingInventoryRead { rt::Promise<void> promise; rt::Resolve<void> resolve; bool resumed = false; };
  class PendingReadKv : public rt::ForwardingKv {
  public:
    PendingReadKv(rt::Kv& store, std::shared_ptr<PendingInventoryRead> state_) : rt::ForwardingKv(store), state(std::move(state_)) {}
    using rt::Kv::get;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override {
      EXPECT(options.consistency == rt::KvConsistencyLevel::strong);
      if (key == uos::codex::auth::CODEX_AUTH_POOL_KV_KEY) throw rt::JsError("fixture auth read failed");
      co_await state->promise.wait();
      state->resumed = true;
      co_return rt::KvEntryMaybe<>{key, rt::KvValue(nullptr), std::nullopt};
    }
  private:
    std::shared_ptr<PendingInventoryRead> state;
  };
}
ASYNC_TEST("an inventory read failure keeps the other read's KV alive until settlement") {
  rt::env::ScopedEnvironment environment({{"HOME", "/nonexistent/w05c6-scope-failure"}});
  const auto previous = uos::config::config.codexAuthJsonB64; uos::config::config.codexAuthJsonB64.clear();
  const auto restore = rt::scopeExit([previous] { uos::config::config.codexAuthJsonB64 = previous; });
  auto store = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
  auto [promise, resolve, reject] = rt::withResolvers<void>(); static_cast<void>(reject);
  auto state = std::make_shared<PendingInventoryRead>(PendingInventoryRead{promise, resolve, false});
  auto proxy = std::make_shared<PendingReadKv>(*store, state);
  const std::weak_ptr<PendingReadKv> weak = proxy;
  st::LoadPromptCacheScopeTargetInventoryOptions options{.kv = proxy};
  auto task = st::loadPromptCacheScopeTargetInventory(options); auto value = co_await std::move(task);
  EXPECT_EQ(value.reason, "auth_pool_unavailable");
  options.kv.reset(); proxy.reset();
  EXPECT(!weak.expired());
  state->resolve();
  co_await rt::sleep(1);
  EXPECT(state->resumed);
  EXPECT(weak.expired());
}
