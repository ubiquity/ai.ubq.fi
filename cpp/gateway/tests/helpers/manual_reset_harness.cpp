#include "tests/helpers/manual_reset_harness.hpp"
#include "uos/codex/reset_settings.hpp"
#include "uos/codex/routing_state.hpp"
#include "uos/provider/selection.hpp"
#include "uos/kv.hpp"
#include "uos/utils.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
namespace uos::tests::helpers::manual_reset_harness {
  rt::Task<std::vector<rt::KvEntryMaybe<>>> ManualKv::getMany(std::vector<rt::KvKey> keys, rt::KvGetOptions options) {
    std::vector<rt::Promise<rt::KvEntryMaybe<>>> reads;
    for (const auto& key : keys) reads.emplace_back(get(key, options));
    auto result = co_await rt::all(std::move(reads)).wait(); co_return result;
  }
  rt::Task<std::shared_ptr<ManualFixture>> manualFixture() {
    namespace contracts = ::uos::authority::native_codex_contracts;
    auto fixture = std::make_shared<ManualFixture>(); fixture->now = 1'700'000'000'000; fixture->kv = std::make_shared<ManualKv>();
    const double now = fixture->now; fixture->pool.updated_at_ms = now;
    for (const std::string name : {"first", "target"}) fixture->pool.accounts.push_back({.access_token = name + "-access", .refresh_token = name + "-refresh", .account_id = name, .updated_at_ms = now});
    const rt::KvValue poolValue(json::value_from(fixture->pool)); co_await fixture->kv->set(contracts::CODEX_AUTH_POOL_KV_KEY, poolValue);
    rt::Promise hashing(::uos::utils::sha256Hex(std::string("target"))); fixture->targetHash = co_await hashing.wait();
    std::vector<rt::Promise<codex::capacity_routing::CodexRoutingAccountIdentity>> identityReads;
    for (const auto& account : fixture->pool.accounts) identityReads.emplace_back(codex::capacity_routing::routingAccountIdentity(account));
    fixture->identities = co_await rt::all(std::move(identityReads)).wait();
    rt::Promise normalizing(codex::capacity_routing::normalizeRoutingState(std::nullopt, fixture->pool, now)); auto routing = co_await normalizing.wait();
    contracts::CodexQuotaClassBlock block; block.blocked_until_ms = now + 3'600'000; block.source = "body_resets_at"; block.legacy_fallback = false; block.quota_signal_observed_at_ms = now - 1'000; block.observed_reset_at_ms = now + 3'600'000; block.observed_reset_at_is_stable = true; block.banked_reset_generation_ambiguous = false; block.banked_reset_recovery_probe_pending = false;
    auto& slot = routing.slots[1]; slot.quota_blocked_until_ms = block.blocked_until_ms; slot.quota_block_source = block.source; slot.quota_blocked_classes = {"standard", "reserve"};
    contracts::CodexQuotaBlocksByClass blocks; blocks.set("standard", block); blocks.set("reserve", block); slot.quota_blocks_by_class = blocks; slot.quota_signal_observed_at_ms = now - 1'000;
    const rt::KvValue routingValue(contracts::toJson(routing)); co_await fixture->kv->set(contracts::CODEX_ACCOUNT_ROUTING_KV_KEY, routingValue);
    fixture->active = {{"v", 1}, {"account_id_hash", fixture->identities[0].accountIdHash}, {"credential_version", fixture->identities[0].credentialVersion}, {"pool_versionstamp", *fixture->kv->versionstamp(contracts::CODEX_AUTH_POOL_KV_KEY)}, {"slot", 0}, {"routing_generation", 0}, {"generation", 7}, {"transition_reason", nullptr}, {"updated_at_ms", rt::makeNumber(now)}};
    const rt::KvValue activeValue(fixture->active); co_await fixture->kv->set(contracts::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, activeValue);
    const rt::KvValue selected(json::object{{"provider_ids", json::array{"codex"}}, {"updated_at_ms", rt::makeNumber(now)}}); co_await fixture->kv->set(provider::selection::PROVIDER_SELECTION_KV_KEY, selected);
    const rt::KvValue off(json::object{{"enabled", false}}); co_await fixture->kv->set(codex::reset_settings::codexResetUsageKey(fixture->targetHash), off);
    auto contract = banked::provenContract(); contract["lookup"] = json::object{{"byIdempotencyKey", false}, {"byProviderReceiptId", false}}; contract["verification"] = json::object{{"independentlyVerifiable", false}}; contract["redeemOutcomeIsFinal"] = true;
    fixture->provider = std::make_shared<banked::FakeCodexUsageResetProvider>(contract); fixture->restored = std::make_shared<bool>(true); fixture->usageCallCount = std::make_shared<double>(0);
    const auto provider = fixture->provider; const auto restored = fixture->restored; const auto calls = fixture->usageCallCount;
    const rt::Fetch fetcher = [provider, restored, calls, now](rt::RequestInfo, rt::RequestInit) {
      ++*calls; const double used = !provider->redeemInputs.empty() && *restored ? 1 : 100;
      const auto window = [now](double amount, double reset, double seconds) { return json::object{{"used_percent", rt::makeNumber(amount)}, {"reset_at", rt::makeNumber((now + reset) / 1000)}, {"limit_window_seconds", rt::makeNumber(seconds)}}; };
      json::object payload{{"rate_limit", json::object{{"primary_window", window(used, 60'000, 18'000)}, {"secondary_window", window(used, 3'600'000, 604'800)}}}, {"additional_rate_limits", json::array{json::object{{"limit_name", "gpt-reserve"}, {"rate_limit", json::object{{"primary_window", window(100, 3'600'000, 18'000)}}}}}}};
      return rt::fulfilledTask(rt::Response::json(payload));
    };
    contracts::CodexCapacityRoutingObservation stale; stale.slot = 1; stale.account_id_hash = fixture->identities[1].accountIdHash; stale.state = "available"; stale.source_observed_at_ms = now - 500; stale.snapshot_at_ms = now - 500; stale.windows.primary = contracts::CodexCapacityRoutingWindow{.used_percent = 100, .reset_at_ms = now + 60'000}; stale.windows.secondary = contracts::CodexCapacityRoutingWindow{.used_percent = 100, .reset_at_ms = now + 3'600'000};
    const rt::KvValue staleValue(json::object{{"v", 1}, {"updated_at_ms", rt::makeNumber(now - 500)}, {"observations", json::array{contracts::toJson(stale)}}}); co_await fixture->kv->set(contracts::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY, staleValue);
    codex::capacity_routing::resetCodexAccountRoutingForTest(); provider::selection::resetProviderSelectionCacheForTest(); ::uos::kv::setKvForTest(fixture->kv);
    fixture->dependencies.kv = fixture->kv; fixture->dependencies.reloadConfig = [] { return banked::config({.mode = "shadow"}); }; fixture->dependencies.provider = [provider](::uos::types::CodexAuthState) { return provider; }; fixture->dependencies.fetcher = fetcher; fixture->dependencies.now = [now] { return now; };
    co_return fixture;
  }
  void cleanupManualFixture() { ::uos::kv::setKvForTest(nullptr); codex::capacity_routing::resetCodexAccountRoutingForTest(); provider::selection::resetProviderSelectionCacheForTest(); }
}
