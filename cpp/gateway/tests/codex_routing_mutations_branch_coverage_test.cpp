// Links: tests/codex_routing_mutations_branch_coverage_test.cpp tests/helpers/codex_account_routing_harness.cpp tests/helpers/counting_kv.cpp tests/helpers/codex_diff_kv.cpp src/auth/relay.cpp src/authority/native_codex.cpp
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
// Port of tests/codex-routing-mutations-branch-coverage.test.ts; all 9 original cases.
// The malformed fence's JS string key has no typed KvKey representation; its non-callable predicate is tested as an empty std::function.
#include "tests/helpers/codex_account_routing_harness.hpp"
#include "tests/helpers/codex_diff_kv.hpp"
#include "uos/codex/capacity_routing.hpp"
#include "uos/codex/routing_mutations.hpp"
#include "uos/config.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace routing = uos::codex::routing_state;
  namespace mutations = uos::codex::routing_mutations;
  using namespace uos::tests::helpers::codex_account_routing_harness;
  using uos::tests::helpers::codex_diff_kv::routingAccountJson;
  using routing::RoutingAccount;
  using routing::CodexRoutingSlot;
  using mutations::releaseCodexRoutingProbe;
  using mutations::isCodexQuotaBlockFenceCurrent;
  using mutations::CodexQuotaResetReconciliationInput;
  using mutations::CodexQuotaResetReconciliationFence;
  constexpr double NOW = 1'700'000'000'000;
  struct Fixture {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/cpp-c2-mutations"}}};
    std::string savedSeed = uos::config::config.codexAuthJsonB64;
    std::shared_ptr<RoutingKv> kv = std::make_shared<RoutingKv>();
    Fixture() { uos::config::config.codexAuthJsonB64.clear(); setKvForTest(kv); resetCodexAccountRoutingForTest(); }
    ~Fixture() { setKvForTest(nullptr); resetCodexAccountRoutingForTest(); uos::config::config.codexAuthJsonB64 = savedSeed; }
  };
  class FaultyRoutingKv : public rt::ForwardingKv {
  public:
    std::shared_ptr<RoutingKv> owner;
    bool throwOnGet = false;
    std::optional<std::string> throwForKey;
    bool failCommits = false;
    explicit FaultyRoutingKv(std::shared_ptr<RoutingKv> owner_) : rt::ForwardingKv(*owner_), owner(std::move(owner_)) {}
    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey kvKey, rt::KvGetOptions options = {}) override {
      if (throwOnGet || throwForKey == key(kvKey)) throw rt::JsError("routing kv get failed");
      return owner->get(std::move(kvKey), options);
    }
    class Atomic : public rt::ForwardingAtomicOperation {
    public:
      FaultyRoutingKv& owner;
      explicit Atomic(FaultyRoutingKv& owner_) : rt::ForwardingAtomicOperation(owner_.owner->atomic()), owner(owner_) {}
      rt::Task<rt::KvCommitResult> commit() override {
        if (owner.failCommits) co_return rt::KvCommitResult{};
        auto result = co_await inner->commit(); co_await rt::microtask(); co_return result;
      }
    };
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<Atomic>(*this); }
  };
  routing::CodexAccountRoutingState requiredState(const std::shared_ptr<RoutingKv>& kv) {
    return required(parseCodexAccountRoutingState(kv->row(CODEX_ACCOUNT_ROUTING_KV_KEY)), "a persisted routing state");
  }
  CodexRoutingSlot slotOf(const std::shared_ptr<RoutingKv>& kv, const RoutingAccount& account) {
    return requiredState(kv).slots.at(static_cast<std::size_t>(account.slot));
  }
  void seedSlot(const std::shared_ptr<RoutingKv>& kv, double index, CodexRoutingSlot value) {
    auto state = requiredState(kv); state.slots.at(static_cast<std::size_t>(index)) = std::move(value);
    kv->values.set(key(CODEX_ACCOUNT_ROUTING_KV_KEY), rt::KvValue(routing::toJson(state))); resetCodexAccountRoutingForTest();
  }
  rt::Task<std::vector<RoutingAccount>> selectedAccounts(double now = NOW) {
    rt::Promise<routing::RouteSelection> selection(selectCodexRoutingAccounts(pool, pool.accounts, now));
    const auto selected = co_await selection.wait();
    if (!std::holds_alternative<routing::RouteSelectionEligible>(selected)) throw rt::JsError("expected eligible accounts");
    co_return std::get<routing::RouteSelectionEligible>(selected).accounts;
  }
  routing::CodexProbeLease leaseFor(double generation, const std::string& token = "probe-token", const std::string& circuit = "quota") {
    return {token, NOW + CODEX_HALF_OPEN_LEASE_MS, generation, circuit, nullptr};
  }
  uos::types::CodexAuthState rotatedAuth(const std::string& accountId, const std::string& accessToken) {
    return {.access_token = accessToken, .refresh_token = "refresh-" + accessToken, .account_id = accountId, .updated_at_ms = NOW};
  }
  rt::Task<RoutingAccount> seedExpiredCircuit(RoutingAccount account, double at) {
    auto marking = rt::Promise(markCodexQuotaBlocked(account, httpDateQuotaResponse(at - 1'000), at - 10'000)); co_await marking.wait();
    resetCodexAccountRoutingForTest(); rt::Promise<std::vector<RoutingAccount>> selection(selectedAccounts(at)); const auto accounts = co_await selection.wait();
    for (const auto& candidate : accounts) if (candidate.probeRequired) co_return candidate;
    throw rt::JsError("expected an expired probe circuit");
  }
  struct BlockedFenceFixture { RoutingAccount account; double generation; routing::CodexAccountRoutingState state; };
  rt::Task<BlockedFenceFixture> blockedFenceFixture(std::shared_ptr<RoutingKv> kv, double resetAtMs) {
    rt::Promise<std::vector<RoutingAccount>> selecting(selectedAccounts()); auto accounts = co_await selecting.wait(); const auto account = accounts[0];
    auto marking = rt::Promise(markCodexQuotaBlocked(account, httpDateQuotaResponse(resetAtMs), NOW)); co_await marking.wait();
    rt::Promise<std::optional<double>> reading(getCodexQuotaBlockFence(account, resetAtMs)); const auto generation = co_await reading.wait();
    if (!generation) throw rt::JsError("unreachable");
    co_return BlockedFenceFixture{account, *generation, requiredState(kv)};
  }
  void expectSlot(const CodexRoutingSlot& actual, const CodexRoutingSlot& expected) { EXPECT(rt::deepEqual(routing::toJson(actual), routing::toJson(expected))); }
}
#define AWAIT(name, expression) auto name##Pending = rt::Promise(expression); auto name = co_await name##Pending.wait()
#define RUN(expression) do { auto pending = rt::Promise(expression); co_await pending.wait(); } while (false)
#define RECONCILE(name, account, reset, generation, ...) AWAIT(name, reconcileCodexQuotaAfterVerifiedReset(account, CodexQuotaResetReconciliationInput{reset, generation __VA_OPT__(,) __VA_ARGS__}))

ASYNC_TEST("upstream timeout marker refuses a foreign slot, a lease-only slot and a stale lease") {
  Fixture fixture; auto kv = fixture.kv; AWAIT(accounts, selectedAccounts()); const auto first = accounts[0], second = accounts[1];
  RUN(markCodexUpstreamTimeout(first, NOW)); const double blockedUntil = NOW + CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS;
  EXPECT(slotOf(kv, first).upstream_timeout_blocked_until_ms == blockedUntil); const auto afterSuccess = slotOf(kv, first);
  auto foreign = second; foreign.slot = first.slot; RUN(markCodexUpstreamTimeout(foreign, NOW)); expectSlot(slotOf(kv, first), afterSuccess);
  const double heldGeneration = afterSuccess.generation; auto seeded = afterSuccess; seeded.probe_lease = leaseFor(heldGeneration, "someone-elses-lease");
  seedSlot(kv, first.slot, seeded); const auto withLease = slotOf(kv, first);
  auto unclaimed = first; unclaimed.probeGeneration.reset(); unclaimed.probeToken.reset(); RUN(markCodexUpstreamTimeout(unclaimed, NOW)); expectSlot(slotOf(kv, first), withLease);
  auto stale = first; stale.probeGeneration = heldGeneration; stale.probeToken = "not-the-token"; stale.probeCircuit = "quota";
  RUN(markCodexUpstreamTimeout(stale, NOW)); expectSlot(slotOf(kv, first), withLease);
}

ASYNC_TEST("credential-invalid marker refuses a foreign slot and a foreign lease") {
  Fixture fixture; auto kv = fixture.kv; AWAIT(accounts, selectedAccounts()); const auto first = accounts[0], second = accounts[1];
  RUN(markCodexCredentialInvalid(first)); const auto afterSuccess = slotOf(kv, first); EXPECT_EQ(afterSuccess.invalid_credential_version, first.credentialVersion);
  auto foreign = second; foreign.slot = first.slot; RUN(markCodexCredentialInvalid(foreign)); expectSlot(slotOf(kv, first), afterSuccess);
  auto seeded = afterSuccess; seeded.probe_lease = leaseFor(afterSuccess.generation, "not-this-account"); seedSlot(kv, first.slot, seeded);
  const auto withForeignLease = slotOf(kv, first); auto wrongClaim = first; wrongClaim.probeGeneration = 99; wrongClaim.probeToken = "other"; wrongClaim.probeCircuit = "quota";
  RUN(markCodexCredentialInvalid(wrongClaim)); expectSlot(slotOf(kv, first), withForeignLease);
}

ASYNC_TEST("credential reconciliation keeps a matching claim and drops an unrenewable one") {
  Fixture fixture; auto kv = fixture.kv; AWAIT(accounts, selectedAccounts()); const auto account = accounts[0];
  RUN(markCodexQuotaBlocked(account, httpDateQuotaResponse(NOW + 60'000), NOW));
  AWAIT(unchanged, reconcileCodexRoutingAccount(account, account.auth)); EXPECT(rt::deepEqual(routingAccountJson(unchanged), routingAccountJson(account)));
  const double leaseGeneration = slotOf(kv, account).generation; const auto lease = leaseFor(leaseGeneration);
  auto seeded = slotOf(kv, account); seeded.account_id_hash = account.accountIdHash; seeded.probe_lease = lease; seedSlot(kv, account.slot, seeded);
  auto claimedAccount = account; claimedAccount.probeGeneration = lease.generation; claimedAccount.probeToken = lease.token; claimedAccount.probeCircuit = "quota";
  AWAIT(retained, reconcileCodexRoutingAccount(claimedAccount, rotatedAuth(account.auth.account_id, "rotated-access")));
  EXPECT(retained.credentialVersion != account.credentialVersion); const auto durable = slotOf(kv, account); ASSERT(durable.probe_lease);
  EXPECT_EQ(durable.credential_version, retained.credentialVersion); EXPECT_EQ(retained.probeGeneration, durable.generation);
  EXPECT_EQ(retained.probeGeneration, durable.probe_lease->generation); EXPECT_EQ(retained.probeToken, durable.probe_lease->token); EXPECT(retained.probeCircuit == "quota");
  AWAIT(newVersion, codexCredentialVersion(rotatedAuth(account.auth.account_id, "rotated-again")));
  seeded = slotOf(kv, account); seeded.credential_version = newVersion; seeded.account_id_hash = account.accountIdHash; seeded.probe_lease = lease; seedSlot(kv, account.slot, seeded);
  AWAIT(dropped, reconcileCodexRoutingAccount(claimedAccount, rotatedAuth(account.auth.account_id, "rotated-again")));
  EXPECT_EQ(dropped.credentialVersion, newVersion); EXPECT(!dropped.probeGeneration); EXPECT(!dropped.probeToken); EXPECT(dropped.probeCircuit == nullptr);
  seeded = slotOf(kv, account); seeded.probe_lease.reset(); seedSlot(kv, account.slot, seeded);
  AWAIT(replaced, reconcileCodexRoutingAccount(claimedAccount, rotatedAuth("different-account", "replacement")));
  EXPECT(!replaced.probeGeneration); EXPECT(!replaced.probeToken); EXPECT(replaced.accountIdHash != account.accountIdHash);
}

ASYNC_TEST("probe release refuses a slot that is not the claim and clears the claimed circuit") {
  Fixture fixture; auto kv = fixture.kv; AWAIT(accounts, selectedAccounts()); const auto account = accounts[0];
  RUN(markCodexQuotaBlocked(account, httpDateQuotaResponse(NOW - 1'000), NOW - 10'000)); resetCodexAccountRoutingForTest();
  AWAIT(expiredAccounts, selectedAccounts()); std::optional<RoutingAccount> expired;
  for (const auto& candidate : expiredAccounts) { if (candidate.probeRequired) { expired = candidate; break; } }
  ASSERT(expired);
  AWAIT(claimed, claimCodexRoutingProbe(pool, *expired, NOW)); ASSERT(claimed); EXPECT(claimed->probeToken);
  auto seeded = slotOf(kv, *claimed); seeded.probe_lease = leaseFor(seeded.generation, "foreign"); seedSlot(kv, claimed->slot, seeded);
  const auto foreign = slotOf(kv, *claimed); auto wrongClaim = *claimed; wrongClaim.probeToken = "not-the-lease";
  RUN(releaseCodexRoutingProbe(wrongClaim)); expectSlot(slotOf(kv, *claimed), foreign);
  seeded = foreign; seeded.probe_lease = leaseFor(foreign.generation, claimed->probeToken.value_or("probe-token")); seedSlot(kv, claimed->slot, seeded);
  RUN(releaseCodexRoutingProbe(*claimed)); const auto released = slotOf(kv, *claimed); EXPECT(!released.probe_lease);
  EXPECT(!released.quota_blocks_by_class || released.quota_blocks_by_class->size() == 0);
  const double timeoutTimeGeneration = released.generation; seeded = released; seeded.upstream_timeout_blocked_until_ms = NOW + 5'000;
  seeded.probe_lease = leaseFor(timeoutTimeGeneration, "timeout-token", "upstream_timeout"); seedSlot(kv, claimed->slot, seeded);
  const auto timeoutSeeded = slotOf(kv, *claimed); auto timeoutClaim = *claimed; timeoutClaim.probeCircuit = "upstream_timeout";
  timeoutClaim.probeToken = "timeout-token"; timeoutClaim.probeGeneration = timeoutTimeGeneration;
  RUN(releaseCodexRoutingProbe(timeoutClaim)); expectSlot(slotOf(kv, *claimed), timeoutSeeded);
}

ASYNC_TEST("quota block fence predicate and strong read reject invalid inputs and unreadable KV") {
  Fixture fixture; auto kv = fixture.kv; const double resetAtMs = NOW + 60'000;
  AWAIT(blocked, blockedFenceFixture(kv, resetAtMs)); const auto account = blocked.account; const double generation = blocked.generation;
  const json::value state = routing::toJson(blocked.state);
  EXPECT(isCodexQuotaBlockFenceCurrent(state, account, resetAtMs, generation));
  EXPECT(!isCodexQuotaBlockFenceCurrent(state, account, rt::Number::NaN, generation));
  EXPECT(!isCodexQuotaBlockFenceCurrent(state, account, resetAtMs, 1.5)); EXPECT(!isCodexQuotaBlockFenceCurrent(state, account, resetAtMs, -1));
  EXPECT(!isCodexQuotaBlockFenceCurrent(state, account, resetAtMs + 1, generation)); auto unresolved = blocked.state; unresolved.banked_reset_legacy_identity_unresolved = true;
  EXPECT(!isCodexQuotaBlockFenceCurrent(json::value(routing::toJson(unresolved)), account, resetAtMs, generation));
  AWAIT(nanFence, getCodexQuotaBlockFence(account, rt::Number::NaN)); EXPECT(!nanFence);
  AWAIT(negativeFence, getCodexQuotaBlockFence(account, -1)); EXPECT(!negativeFence);
  AWAIT(currentFence, getCodexQuotaBlockFence(account, resetAtMs)); EXPECT_EQ(currentFence, generation);
  const auto faulty = std::make_shared<FaultyRoutingKv>(kv); faulty->throwOnGet = true; setKvForTest(faulty);
  AWAIT(unreadableFence, getCodexQuotaBlockFence(account, resetAtMs)); EXPECT(!unreadableFence);
  setKvForTest(nullptr); AWAIT(missingFence, getCodexQuotaBlockFence(account, resetAtMs)); EXPECT(!missingFence);
}

ASYNC_TEST("verified-reset reconciliation rejects invalid input, stale fences and unreadable fences") {
  Fixture fixture; auto kv = fixture.kv; const double resetAtMs = NOW + 60'000;
  AWAIT(blocked, blockedFenceFixture(kv, resetAtMs)); const auto account = blocked.account; const double generation = blocked.generation;
  RECONCILE(nan, account, rt::Number::NaN, generation); EXPECT(!nan); RECONCILE(negative, account, resetAtMs, -1); EXPECT(!negative);
  const rt::KvKey fenceKey{"test", "reset-fence"};
  const std::vector<CodexQuotaResetReconciliationFence> malformed{{rt::KvKey{}, {}}};
  RECONCILE(malformedResult, account, resetAtMs, generation, malformed); EXPECT(!malformedResult);
  const std::vector<CodexQuotaResetReconciliationFence> stale{{fenceKey, [](const json::value&) { return false; }}};
  RECONCILE(staleResult, account, resetAtMs, generation, stale); EXPECT(!staleResult);
  const auto faulty = std::make_shared<FaultyRoutingKv>(kv); faulty->throwForKey = key(fenceKey); setKvForTest(faulty);
  const std::vector<CodexQuotaResetReconciliationFence> current{{fenceKey, [](const json::value&) { return true; }}};
  RECONCILE(unreadableResult, account, resetAtMs, generation, current); EXPECT(!unreadableResult);
  faulty->throwForKey.reset(); RECONCILE(claimed, account, resetAtMs, generation, current); ASSERT(claimed); setKvForTest(kv);
  const auto retrying = std::make_shared<FaultyRoutingKv>(kv); retrying->failCommits = true; setKvForTest(retrying); const auto before = requiredState(kv);
  RECONCILE(lostCommit, account, resetAtMs, generation); EXPECT(!lostCommit); EXPECT(rt::deepEqual(routing::toJson(requiredState(kv)), routing::toJson(before)));
  retrying->failCommits = false; retrying->throwOnGet = true; setKvForTest(kv);
  AWAIT(fenceBeforeThrow, blockedFenceFixture(kv, resetAtMs + 2'000)); setKvForTest(retrying);
  RECONCILE(readFailure, fenceBeforeThrow.account, resetAtMs + 2'000, fenceBeforeThrow.generation); EXPECT(!readFailure);
  retrying->throwOnGet = false; setKvForTest(nullptr); RECONCILE(noKv, account, resetAtMs, generation); EXPECT(!noKv);
}

ASYNC_TEST("verified-reset reconciliation refuses a generation it cannot advance") {
  Fixture fixture; auto kv = fixture.kv; const double resetAtMs = NOW + 60'000;
  AWAIT(blocked, blockedFenceFixture(kv, resetAtMs)); const double exhausted = rt::Number::MAX_SAFE_INTEGER;
  auto seeded = blocked.state.slots.at(static_cast<std::size_t>(blocked.account.slot)); seeded.generation = exhausted; seedSlot(kv, blocked.account.slot, seeded);
  RECONCILE(claimed, blocked.account, resetAtMs, exhausted); EXPECT(!claimed); EXPECT_EQ(slotOf(kv, blocked.account).generation, exhausted);
}

ASYNC_TEST("probe claim returns early, claims locally without KV and fails closed on lost commits") {
  Fixture fixture; auto kv = fixture.kv; AWAIT(accounts, selectedAccounts()); const auto account = accounts[0];
  auto notRequired = account; notRequired.probeRequired = false; AWAIT(unchanged, claimCodexRoutingProbe(pool, notRequired, NOW));
  EXPECT(rt::deepEqual(routingAccountJson(unchanged), routingAccountJson(notRequired)));
  const double leaseStep = CODEX_HALF_OPEN_LEASE_MS + 1, firstAt = NOW;
  AWAIT(expired, seedExpiredCircuit(account, firstAt)); AWAIT(claimed, claimCodexRoutingProbe(pool, expired, firstAt)); ASSERT(claimed);
  const auto durable = slotOf(kv, *claimed); ASSERT(durable.probe_lease); EXPECT_EQ(durable.probe_lease->token, claimed->probeToken);
  EXPECT_EQ(durable.probe_lease->circuit, "quota"); EXPECT_EQ(durable.probe_lease->generation, durable.generation);
  const double secondAt = NOW + leaseStep; AWAIT(retryExpired, seedExpiredCircuit(account, secondAt));
  const auto retrying = std::make_shared<FaultyRoutingKv>(kv); retrying->failCommits = true; setKvForTest(retrying);
  AWAIT(lostCommit, claimCodexRoutingProbe(pool, retryExpired, secondAt)); EXPECT(!lostCommit);
  const double thirdAt = NOW + 2 * leaseStep; AWAIT(localExpired, seedExpiredCircuit(account, thirdAt));
  RUN(uos::codex::capacity_routing::loadCodexAccountRouting(pool));
  const auto faulty = std::make_shared<FaultyRoutingKv>(kv); faulty->throwOnGet = true; setKvForTest(faulty); const auto durableBeforeLocal = slotOf(kv, localExpired);
  AWAIT(localFromFailure, claimCodexRoutingProbe(pool, localExpired, thirdAt)); ASSERT(localFromFailure);
  EXPECT_EQ(localFromFailure->probeGeneration, durableBeforeLocal.generation); EXPECT(localFromFailure->probeCircuit == "quota");
  expectSlot(slotOf(kv, localExpired), durableBeforeLocal);
  const auto durableAfter = slotOf(kv, localExpired);
  EXPECT(!durableAfter.probe_lease || durableAfter.probe_lease->token != localFromFailure->probeToken);
}

ASYNC_TEST("probe claim falls back to the cached state when KV is unavailable") {
  Fixture fixture; auto kv = fixture.kv; AWAIT(accounts, selectedAccounts()); const auto account = accounts[0];
  AWAIT(expired, seedExpiredCircuit(account, NOW)); RUN(uos::codex::capacity_routing::loadCodexAccountRouting(pool)); setKvForTest(nullptr);
  AWAIT(claimed, claimCodexRoutingProbe(pool, expired, NOW)); ASSERT(claimed); EXPECT(claimed->probeToken);
  EXPECT_EQ(claimed->probeGeneration, slotOf(kv, expired).generation);
  AWAIT(second, claimCodexRoutingProbe(pool, expired, NOW)); EXPECT(!second);
}
