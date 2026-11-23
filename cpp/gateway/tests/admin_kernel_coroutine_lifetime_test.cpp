// Links: tests/admin_kernel_coroutine_lifetime_test.cpp
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
// Post-fix functional ownership regression for the actual policy-queue handler.
#include "tests/helpers/counting_kv.hpp"
#include "uos/admin/kernel.hpp"
#include "uos/kernel/quota_v2.hpp"
#include "uos/kv.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test_async.hpp"
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace quota = uos::kernel::quota_v2;
  using Entry = rt::KvEntryMaybe<>;
  using CountingKv = uos::tests::helpers::counting_kv::CountingKv;
  enum class Phase { owners, repos };

  rt::Task<rt::Response> forbidFetch(rt::RequestInfo, rt::RequestInit) {
    throw rt::JsError("functional fixture forbids provider requests");
    co_return rt::Response();
  }
  struct FixtureBoundary {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>()};
    rt::GlobalFetchOverride offline{rt::Fetch(forbidFetch)};
    ~FixtureBoundary() { uos::kv::setKvForTest(nullptr); }
  };
  struct ReadState {
    Phase phase;
    CountingKv backing;
    rt::Resolvers<Entry> first = rt::withResolvers<Entry>();
    rt::Resolvers<Entry> second = rt::withResolvers<Entry>();
    rt::Resolvers<void> bothStarted = rt::withResolvers<void>();
    rt::Resolvers<void> secondCompleted = rt::withResolvers<void>();
    std::vector<std::string> order;
    std::size_t completed = 0;
    explicit ReadState(Phase phase_) : phase(phase_) {}
  };
  rt::KvKey policyKey(Phase phase, bool second) {
    const std::string owner = second ? "owner-b" : "owner-a";
    if (phase == Phase::owners) return quota::kernelOrgLimitKey(owner);
    return quota::kernelLimitKey(owner, second ? "repo-b" : "repo-a");
  }
  rt::Task<Entry> controlledRead(std::shared_ptr<ReadState> state, rt::KvKey key, rt::KvGetOptions options) {
    const bool isFirst = key == policyKey(state->phase, false);
    const bool isSecond = key == policyKey(state->phase, true);
    if (!isFirst && !isSecond) {
      auto reading = state->backing.get(key, options);
      auto value = co_await std::move(reading);
      co_return value;
    }
    state->order.emplace_back(isFirst ? "first" : "second");
    if (state->order.size() == 2) state->bothStarted.resolve();
    if (isFirst) {
      auto value = co_await state->first.promise.wait();
      ++state->completed;
      co_return value;
    }
    auto value = co_await state->second.promise.wait();
    ++state->completed;
    state->secondCompleted.resolve();
    co_return value;
  }
  class DeferredKv final : public rt::ForwardingKv {
  public:
    explicit DeferredKv(std::shared_ptr<ReadState> state_) : rt::ForwardingKv(state_->backing), state(std::move(state_)) {}
    using rt::Kv::get;
    rt::Task<Entry> get(rt::KvKey key, rt::KvGetOptions options = {}) override { return controlledRead(state, std::move(key), options); }
  private:
    std::shared_ptr<ReadState> state;
  };
  uos::json::object queuedItem(std::string owner, std::string repo, double seen) {
    return {{"owner", std::move(owner)}, {"repo", std::move(repo)}, {"request_count", 3}, {"first_seen_at_ms", 1},
            {"last_seen_at_ms", seen}, {"last_route", "/v1/responses"}};
  }
  std::shared_ptr<DeferredKv> installStore(const std::shared_ptr<ReadState>& state) {
    const rt::KvKey queueKey{"uos_ai", "kernel_policy_queue"};
    const uos::json::array records{queuedItem("owner-a", "repo-a", 2), queuedItem("owner-b", "repo-b", 1)};
    state->backing.seed(queueKey, records);
    auto store = std::make_shared<DeferredKv>(state);
    uos::kv::setKvForTest(store);
    return store;
  }
  Entry resultEntry(Phase phase, bool second, bool present) {
    Entry result;
    result.key = policyKey(phase, second);
    if (present) {
      result.value = rt::KvValue(true);
      result.versionstamp = "00000000000000000001";
    } else result.value = rt::KvValue(nullptr);
    return result;
  }
  rt::Task<void> rejectThenCompleteSibling(Phase phase) {
    FixtureBoundary boundary;
    auto state = std::make_shared<ReadState>(phase);
    auto store = installStore(state);
    std::weak_ptr<DeferredKv> weakStore = store;
    auto handling = uos::admin::kernel::handleAdminKernelPolicyQueueList();
    rt::Promise<rt::Response> response(std::move(handling));
    co_await state->bothStarted.promise.wait();
    EXPECT_EQ(state->order, std::vector<std::string>({"first", "second"}));
    const rt::Reason expected = rt::makeReason(rt::JsError("controlled policy lookup failure"));
    state->first.reject(expected);
    rt::Reason actual;
    try { auto value = co_await response.wait(); (void)value; }
    catch (...) { actual = std::current_exception(); }
    EXPECT(actual == expected);
    EXPECT_EQ(state->completed, std::size_t(0));
    uos::kv::setKvForTest(nullptr);
    store.reset();
    EXPECT(!weakStore.expired());
    const Entry later = resultEntry(phase, true, true);
    state->second.resolve(later);
    co_await state->secondCompleted.promise.wait();
    co_await rt::microtask();
    EXPECT_EQ(state->completed, std::size_t(1));
    EXPECT(weakStore.expired());
  }
  rt::Task<void> preservesQueueOrder() {
    FixtureBoundary boundary;
    auto state = std::make_shared<ReadState>(Phase::repos);
    auto store = installStore(state);
    auto handling = uos::admin::kernel::handleAdminKernelPolicyQueueList();
    rt::Promise<rt::Response> response(std::move(handling));
    co_await state->bothStarted.promise.wait();
    const Entry second = resultEntry(Phase::repos, true, false);
    const Entry first = resultEntry(Phase::repos, false, false);
    state->second.resolve(second);
    co_await state->secondCompleted.promise.wait();
    state->first.resolve(first);
    auto value = co_await response.wait();
    EXPECT_EQ(value.status(), 200);
    auto body = co_await value.json();
    const auto* data = rt::get(body, "data");
    ASSERT(data && data->is_array());
    ASSERT_EQ(data->as_array().size(), std::size_t(2));
    EXPECT_EQ(rt::getString(data->as_array()[0], "owner"), std::optional<std::string_view>("owner-a"));
    EXPECT_EQ(rt::getString(data->as_array()[1], "owner"), std::optional<std::string_view>("owner-b"));
    EXPECT_EQ(state->order, std::vector<std::string>({"first", "second"}));
    EXPECT_EQ(state->completed, std::size_t(2));
    (void)store;
  }
}

ASYNC_TEST("policy queue retains a pending owner lookup after fail-fast rejection") {
  auto task = rejectThenCompleteSibling(Phase::owners);
  co_await std::move(task);
}
ASYNC_TEST("policy queue retains a pending repo lookup after fail-fast rejection") {
  auto task = rejectThenCompleteSibling(Phase::repos);
  co_await std::move(task);
}
ASYNC_TEST("policy queue preserves record order when lookups finish in reverse order") {
  auto task = preservesQueueOrder();
  co_await std::move(task);
}
