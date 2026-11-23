// Port of tests/authority-admission.test.ts.
#include "uos/authority/admission_state.hpp"
#include "uos/authority/contracts.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {
  using namespace uos;
  using namespace uos::authority::contracts;
  using uos::authority::admission_state::createAuthorityState;
  using uos::authority::admission_state::PortableAuthority;
  using uos::authority::admission_state::Selection;

  // `await assert.rejects(promise, /pattern/)`: String(error) must match.
  template <class T> rt::Task<void> rejectsMatching(rt::Task<T> task, const char* pattern, const char* file, int line) {
    bool rejected = false;
    std::string text;
    try {
      co_await std::move(task);
    } catch (...) {
      rejected = true;
      text = rt::reasonToString(std::current_exception());
    }
    if (!rejected) rt::test::report(file, line, std::string("expected a rejection matching /") + pattern + "/", false);
    else if (!rt::Regex(pattern).test(text)) rt::test::report(file, line, std::string("expected a rejection matching /") + pattern + "/, got " + text, false);
  }
#define ASSERT_REJECTS(task, pattern) co_await rejectsMatching((task), pattern, __FILE__, __LINE__)

  const AuthorityScope scope{.namespace_ = "synthetic-admission", .epoch = 1};
  const double now = 1'000;
  AccountBinding account(const std::string& accountId) {
    return AccountBinding{
      .accountId = accountId,
      .fingerprint = accountId + "-fp",
      .credentialGeneration = 0,
      .routingGeneration = 1,
      .resetGeneration = 1,
      .overageVersion = 1,
      .overageEnabled = false,
      .enabled = true,
      .models = {"fixture"},
      .proof = std::nullopt,
    };
  }
  const PrincipalPolicy policy{
    .principalId = "p",
    .version = 1,
    .deleted = false,
    .windowResetAtMs = 10'000,
    .requestLimit = 2,
    .kernelLimit = 2,
    .concurrencyLimit = 2,
    .paidLimitMicrocredits = 100,
  };

  struct FixtureStore : AuthorityStore {
    AuthorityState state;
    double revision = 0;
    std::function<void(AuthorityState&)> conflict;
    rt::Promise<AuthoritySnapshot> read() override { return rt::Promise<AuthoritySnapshot>::resolve(AuthoritySnapshot{.revision = revision, .state = state}); }
    rt::Promise<bool> compareAndSwap(double expected, AuthorityState candidate) override {
      if (conflict) {
        conflict(state);
        conflict = nullptr;
        revision += 1;
        return rt::Promise<bool>::resolve(false);
      }
      if (expected != revision) return rt::Promise<bool>::resolve(false);
      state = std::move(candidate);
      revision += 1;
      return rt::Promise<bool>::resolve(true);
    }
  };

  struct Fixture {
    std::shared_ptr<FixtureStore> store;
    std::shared_ptr<PortableAuthority> authority;
    AdmissionHandle handle{};
    AuthorityState& state() const { return store->state; }
    void conflict(std::function<void(AuthorityState&)> action) const { store->conflict = std::move(action); }
  };

  Fixture fixture() {
    auto store = std::make_shared<FixtureStore>();
    PrincipalPolicy other = policy;
    other.principalId = "q";
    store->state = createAuthorityState(scope, {account("a"), account("b")}, {policy, other});
    return Fixture{.store = store, .authority = std::make_shared<PortableAuthority>(store, scope)};
  }

  AdmitInput input(const std::string& requestId, const std::string& principalId = "p") {
    return AdmitInput{
      .scope = scope,
      .requestId = requestId,
      .principalId = principalId,
      .policyVersion = 1,
      .digest = "digest",
      .route = "/v1/responses",
      .model = "fixture",
      .nowMs = now,
    };
  }

  std::vector<double> sortedActive(const AuthorityState& state) {
    std::vector<double> active;
    for (const PrincipalWindow* window : state.windows.values()) active.push_back(window->active);
    std::sort(active.begin(), active.end());
    return active;
  }

  double sumActive(const AuthorityState& state) {
    double sum = 0;
    for (const PrincipalWindow* window : state.windows.values()) sum += window->active;
    return sum;
  }

  const PrincipalWindow& firstWindow(const AuthorityState& state) { return *state.windows.values().at(0); }

  std::optional<std::string> accountIdOf(const std::optional<AccountBinding>& value) { return value ? std::optional<std::string>(value->accountId) : std::nullopt; }

  rt::Task<void> dispatchSubscription(Fixture f, AdmissionHandle handle, std::string attemptId) {
    rt::Task<Selection> selecting = f.authority->selection(handle);
    const Selection selected = co_await std::move(selecting);
    const AttemptInput attempt{.attemptId = attemptId, .provider = "codex", .maximumExposureMicrocredits = 0, .account = selected.account};
    rt::Task<void> reserving = f.authority->reserveAttempt(handle, attempt, now);
    co_await std::move(reserving);
    rt::Task<bool> beginning = f.authority->beginDispatch(handle, attemptId, now);
    co_await std::move(beginning);
    rt::Task<void> confirming = f.authority->confirmDispatched(handle, attemptId);
    co_await std::move(confirming);
  }

  rt::Task<Fixture> exhaustedSubscription() {
    Fixture f = fixture();
    PrincipalPolicy limited = policy;
    limited.requestLimit = 1;
    limited.kernelLimit = 1;
    limited.concurrencyLimit = 1;
    f.state().policies.set("p", limited);
    rt::Task<AdmissionHandle> admitting = f.authority->admit(input("failover"));
    const AdmissionHandle handle = co_await std::move(admitting);
    rt::Task<void> dispatching = dispatchSubscription(f, handle, "a-hop");
    co_await std::move(dispatching);
    const AccountProof proof{.kind = "quota_exhausted", .model = "fixture", .fingerprint = "a-fp", .observedAtMs = now, .validUntilMs = 2'000};
    rt::Task<void> proving = f.authority->observeProof(handle, "a-hop", proof);
    co_await std::move(proving);
    const TerminalEvidence evidence{.outcome = "failed", .providerRequestId = "a-upstream", .spendMicrocredits = 0, .fallback = "quota_exhausted"};
    rt::Task<void> terminating = f.authority->observeTerminal(handle, "a-hop", evidence, false);
    co_await std::move(terminating);
    f.handle = handle;
    co_return f;
  }

  template <class T> std::size_t fulfilledCount(const std::vector<rt::SettledResult<T>>& results) {
    return static_cast<std::size_t>(std::count_if(results.begin(), results.end(), [](const rt::SettledResult<T>& result) { return result.status == "fulfilled"; }));
  }
}

ASYNC_TEST("concurrent shared-principal admissions retry CAS without exceeding quota; another principal shares global active") {
  const Fixture f = fixture();
  std::vector<rt::Promise<AdmissionHandle>> admissions;
  admissions.push_back(rt::Promise(f.authority->admit(input("r1"))));
  admissions.push_back(rt::Promise(f.authority->admit(input("r2"))));
  admissions.push_back(rt::Promise(f.authority->admit(input("r3"))));
  const auto results = co_await rt::allSettled(std::move(admissions)).wait();
  EXPECT_EQ(fulfilledCount(results), 2u);
  rt::Task<AdmissionHandle> admitting = f.authority->admit(input("q1", "q"));
  const AdmissionHandle other = co_await std::move(admitting);
  rt::Task<Selection> selecting = f.authority->selection(other);
  const Selection selected = co_await std::move(selecting);
  EXPECT_EQ(accountIdOf(selected.account), std::optional<std::string>("a"));
  EXPECT_EQ(f.state().active ? std::optional<double>(f.state().active->generation) : std::nullopt, std::optional<double>(1));
  EXPECT_EQ(sortedActive(f.state()), std::vector<double>({1, 2}));
}

ASYNC_TEST("request identity, scope and handle fences fail closed while duplicate admission is idempotent") {
  const Fixture f = fixture();
  rt::Task<AdmissionHandle> admitting = f.authority->admit(input("r"));
  const AdmissionHandle handle = co_await std::move(admitting);
  rt::Task<AdmissionHandle> again = f.authority->admit(input("r"));
  const AdmissionHandle repeated = co_await std::move(again);
  EXPECT(repeated == handle);
  AdmitInput different = input("r");
  different.digest = "different";
  rt::Task<AdmissionHandle> conflicting = f.authority->admit(different);
  ASSERT_REJECTS(std::move(conflicting), "request_identity_conflict");
  AdmitInput otherEpoch = input("s");
  otherEpoch.scope.epoch = 2;
  rt::Task<AdmissionHandle> wrongScope = f.authority->admit(otherEpoch);
  ASSERT_REJECTS(std::move(wrongScope), "namespace_or_epoch_mismatch");
  AdmissionHandle forged = handle;
  forged.fence = 999;
  rt::Task<Selection> fenced = f.authority->selection(forged);
  ASSERT_REJECTS(std::move(fenced), "admission_fence_mismatch");
}

ASYNC_TEST("a CAS conflict rereads principal deletion and cannot return stale admission success") {
  const Fixture f = fixture();
  f.conflict([](AuthorityState& state) {
    PrincipalPolicy deleted = policy;
    deleted.deleted = true;
    state.policies.set("p", deleted);
  });
  rt::Task<AdmissionHandle> admitting = f.authority->admit(input("r"));
  ASSERT_REJECTS(std::move(admitting), "principal_policy_changed");
  EXPECT_EQ(f.state().admissions.keys().size(), 0u);
}

ASYNC_TEST("global active survives restart and reorder; replacement fences a reserved dispatch") {
  const Fixture f = fixture();
  rt::Task<AdmissionHandle> admitting = f.authority->admit(input("r"));
  const AdmissionHandle handle = co_await std::move(admitting);
  rt::Task<Selection> selecting = f.authority->selection(handle);
  const Selection selected = co_await std::move(selecting);
  const AttemptInput attempt{.attemptId = "a1", .provider = "codex", .maximumExposureMicrocredits = 0, .account = selected.account};
  rt::Task<void> reserving = f.authority->reserveAttempt(handle, attempt, now);
  co_await std::move(reserving);
  std::reverse(f.state().accounts.begin(), f.state().accounts.end());
  const auto restart = std::make_shared<PortableAuthority>(f.store, scope);
  rt::Task<AdmissionHandle> admittingOther = restart->admit(input("q", "q"));
  const AdmissionHandle other = co_await std::move(admittingOther);
  rt::Task<Selection> selectingOther = restart->selection(other);
  const Selection otherSelection = co_await std::move(selectingOther);
  EXPECT_EQ(accountIdOf(otherSelection.account), std::optional<std::string>("a"));
  for (AccountBinding& value : f.state().accounts) {
    if (value.accountId == "a") {
      value.fingerprint = "new";
      value.credentialGeneration = 1;
    }
  }
  rt::Task<bool> beginning = restart->beginDispatch(handle, "a1", now);
  ASSERT_REJECTS(std::move(beginning), "account_fence_mismatch");
}

ASYNC_TEST("transient, stale and mixed proof cannot authorize paid fallback or reset redemption") {
  const Fixture f = fixture();
  for (AccountBinding& value : f.state().accounts) {
    value.proof = AccountProof{.kind = value.accountId == "a" ? "quota_exhausted" : "transient", .model = "fixture", .fingerprint = value.fingerprint, .observedAtMs = 900, .validUntilMs = 2'000};
  }
  rt::Task<AdmissionHandle> refused = f.authority->admit(input("r"));
  ASSERT_REJECTS(std::move(refused), "routing_unavailable");
  for (AccountBinding& value : f.state().accounts) {
    value.proof = AccountProof{.kind = "credential_invalid", .model = "fixture", .fingerprint = value.fingerprint, .observedAtMs = 900, .validUntilMs = 2'000};
  }
  rt::Task<AdmissionHandle> admitting = f.authority->admit(input("paid"));
  const AdmissionHandle handle = co_await std::move(admitting);
  rt::Task<bool> checking = f.authority->resetEligible(handle, now);
  const bool eligible = co_await std::move(checking);
  EXPECT_EQ(eligible, false);
  const AttemptInput attempt{.attemptId = "paid1", .provider = "surplus", .maximumExposureMicrocredits = 50, .account = std::nullopt};
  rt::Task<void> reserving = f.authority->reserveAttempt(handle, attempt, now);
  co_await std::move(reserving);
  rt::Task<bool> beginning = f.authority->beginDispatch(handle, "paid1", 2'001);
  ASSERT_REJECTS(std::move(beginning), "paid_pool_proof_required");
}

ASYNC_TEST("a window rollover between reserve and dispatch is fenced") {
  const Fixture f = fixture();
  rt::Task<AdmissionHandle> admitting = f.authority->admit(input("r"));
  const AdmissionHandle handle = co_await std::move(admitting);
  rt::Task<Selection> selecting = f.authority->selection(handle);
  const Selection selected = co_await std::move(selecting);
  const AttemptInput attempt{.attemptId = "a1", .provider = "codex", .maximumExposureMicrocredits = 0, .account = selected.account};
  rt::Task<void> reserving = f.authority->reserveAttempt(handle, attempt, now);
  co_await std::move(reserving);
  PrincipalPolicy rolled = policy;
  rolled.windowResetAtMs = 20'000;
  f.state().policies.set("p", rolled);
  rt::Task<bool> beginning = f.authority->beginDispatch(handle, "a1", now);
  ASSERT_REJECTS(std::move(beginning), "quota_window_changed");
}

ASYNC_TEST("strong-proof subscription failover preserves one admission and reservation through CAS retry") {
  rt::Task<Fixture> exhausting = exhaustedSubscription();
  const Fixture f = co_await std::move(exhausting);
  rt::Task<AdmissionHandle> another = f.authority->admit(input("another"));
  ASSERT_REJECTS(std::move(another), "principal_quota_exceeded");
  const AttemptInput paid{.attemptId = "paid", .provider = "surplus", .maximumExposureMicrocredits = 10, .account = std::nullopt};
  rt::Task<void> reservingPaid = f.authority->reserveAttempt(f.handle, paid, now);
  ASSERT_REJECTS(std::move(reservingPaid), "paid_pool_proof_required");
  f.conflict([](AuthorityState& state) { std::reverse(state.accounts.begin(), state.accounts.end()); });
  rt::Task<std::optional<AccountBinding>> reselecting = f.authority->reselectSubscription(f.handle, now);
  const std::optional<AccountBinding> reselected = co_await std::move(reselecting);
  EXPECT_EQ(accountIdOf(reselected), std::optional<std::string>("b"));
  rt::Task<Selection> selecting = f.authority->selection(f.handle);
  const Selection selected = co_await std::move(selecting);
  EXPECT_EQ(accountIdOf(selected.account), std::optional<std::string>("b"));
  EXPECT(f.state().active == (ActiveAccount{.accountId = "b", .fingerprint = "b-fp", .generation = 2}));
  EXPECT_EQ(f.state().admissions.keys(), std::vector<std::string>({"failover"}));
  {
    const PrincipalWindow& window = firstWindow(f.state());
    EXPECT_EQ(window.reserved, 0);
    EXPECT_EQ(window.dispatched, 1);
    EXPECT_EQ(window.active, 1);
    EXPECT_EQ(window.kernelReserved, 1);
  }
  rt::Task<void> dispatching = dispatchSubscription(f, f.handle, "b-hop");
  co_await std::move(dispatching);
  const TerminalEvidence completed{.outcome = "completed", .providerRequestId = "b-upstream", .spendMicrocredits = 0, .fallback = std::nullopt};
  rt::Task<void> terminating = f.authority->observeTerminal(f.handle, "b-hop", completed);
  co_await std::move(terminating);
  rt::Task<void> delivering = f.authority->observeDelivery(f.handle, "unobserved");
  co_await std::move(delivering);
  rt::Task<std::string> settling = f.authority->settle(f.handle);
  const std::string settled = co_await std::move(settling);
  EXPECT_EQ(settled, "settled");
  const PrincipalWindow& window = firstWindow(f.state());
  EXPECT_EQ(window.dispatched, 1);
  EXPECT_EQ(window.active, 0);
  EXPECT_EQ(window.kernelReserved, 0);
  EXPECT_EQ(window.kernelCompleted, 1);
}

ASYNC_TEST("subscription reselection rejects pending terminals, weak proof and changed policy or binding fences") {
  const Fixture pending = fixture();
  rt::Task<AdmissionHandle> admitting = pending.authority->admit(input("pending"));
  const AdmissionHandle handle = co_await std::move(admitting);
  rt::Task<void> dispatching = dispatchSubscription(pending, handle, "hop");
  co_await std::move(dispatching);
  rt::Task<std::optional<AccountBinding>> early = pending.authority->reselectSubscription(handle, now);
  ASSERT_REJECTS(std::move(early), "attempt_outcome_pending");
  const std::vector<std::function<void(AccountProof&)>> proofs{
    [](AccountProof& proof) { proof.kind = "transient"; },
    [](AccountProof& proof) { proof.validUntilMs = now; },
    [](AccountProof& proof) { proof.model = "another-model"; },
    [](AccountProof& proof) { proof.fingerprint = "retired-fingerprint"; },
  };
  for (const auto& change : proofs) {
    rt::Task<Fixture> exhausting = exhaustedSubscription();
    const Fixture f = co_await std::move(exhausting);
    AccountBinding current = f.state().accounts[0];
    ASSERT(current.proof.has_value());
    change(*current.proof);
    f.state().accounts[0] = current;
    rt::Task<std::optional<AccountBinding>> reselecting = f.authority->reselectSubscription(f.handle, now);
    ASSERT_REJECTS(std::move(reselecting), "subscription_proof_required");
    EXPECT_EQ(f.state().active ? std::optional<std::string>(f.state().active->accountId) : std::nullopt, std::optional<std::string>("a"));
  }
  const std::vector<double AccountBinding::*> fields{&AccountBinding::credentialGeneration, &AccountBinding::routingGeneration, &AccountBinding::resetGeneration, &AccountBinding::overageVersion};
  for (double AccountBinding::*field : fields) {
    rt::Task<Fixture> exhausting = exhaustedSubscription();
    const Fixture f = co_await std::move(exhausting);
    f.conflict([field](AuthorityState& state) {
      AccountBinding changed = state.accounts[0];
      changed.*field = state.accounts[0].*field + 1;
      state.accounts[0] = changed;
    });
    rt::Task<std::optional<AccountBinding>> reselecting = f.authority->reselectSubscription(f.handle, now);
    ASSERT_REJECTS(std::move(reselecting), "account_fence_mismatch");
    EXPECT_EQ(accountIdOf(f.state().admissions.get("failover")->account), std::optional<std::string>("a"));
  }
  const std::vector<std::function<void(PrincipalPolicy&)>> changes{
    [](PrincipalPolicy& value) { value.deleted = true; },
    [](PrincipalPolicy& value) { value.version = 2; },
    [](PrincipalPolicy& value) { value.windowResetAtMs = 20'000; },
  };
  for (const auto& change : changes) {
    rt::Task<Fixture> exhausting = exhaustedSubscription();
    const Fixture f = co_await std::move(exhausting);
    PrincipalPolicy changed = *f.state().policies.get("p");
    change(changed);
    f.state().policies.set("p", changed);
    rt::Task<std::optional<AccountBinding>> reselecting = f.authority->reselectSubscription(f.handle, now);
    ASSERT_REJECTS(std::move(reselecting), "principal_policy_changed|quota_window_changed");
  }
}

ASYNC_TEST("quota rollover retains outstanding principal concurrency until the old admission settles") {
  const Fixture f = fixture();
  PrincipalPolicy single = policy;
  single.concurrencyLimit = 1;
  f.state().policies.set("p", single);
  rt::Task<AdmissionHandle> admitting = f.authority->admit(input("old-window"));
  const AdmissionHandle old = co_await std::move(admitting);
  rt::Task<void> dispatching = dispatchSubscription(f, old, "old-hop");
  co_await std::move(dispatching);
  PrincipalPolicy rolled = *f.state().policies.get("p");
  rolled.windowResetAtMs = 20'000;
  f.state().policies.set("p", rolled);
  const auto rollover = [](const std::string& requestId) {
    AdmitInput value = input(requestId);
    value.nowMs = 10'001;
    return value;
  };
  std::vector<rt::Promise<AdmissionHandle>> blockedAdmissions;
  blockedAdmissions.push_back(rt::Promise(f.authority->admit(rollover("blocked1"))));
  blockedAdmissions.push_back(rt::Promise(f.authority->admit(rollover("blocked2"))));
  const auto blocked = co_await rt::allSettled(std::move(blockedAdmissions)).wait();
  EXPECT(std::all_of(blocked.begin(), blocked.end(), [](const rt::SettledResult<AdmissionHandle>& result) {
    return result.status == "rejected" && rt::includes(rt::reasonToString(result.reason), "principal_quota_exceeded");
  }));
  EXPECT_EQ(sumActive(f.state()), 1);
  const TerminalEvidence completed{.outcome = "completed", .providerRequestId = "old-upstream", .spendMicrocredits = 0, .fallback = std::nullopt};
  rt::Task<void> terminating = f.authority->observeTerminal(old, "old-hop", completed);
  co_await std::move(terminating);
  rt::Task<void> delivering = f.authority->observeDelivery(old, "unobserved");
  co_await std::move(delivering);
  rt::Task<std::string> settling = f.authority->settle(old);
  const std::string settled = co_await std::move(settling);
  EXPECT_EQ(settled, "settled");
  std::vector<rt::Promise<AdmissionHandle>> admissions;
  admissions.push_back(rt::Promise(f.authority->admit(rollover("new1"))));
  admissions.push_back(rt::Promise(f.authority->admit(rollover("new2"))));
  const auto admitted = co_await rt::allSettled(std::move(admissions)).wait();
  EXPECT_EQ(fulfilledCount(admitted), 1u);
  EXPECT_EQ(sumActive(f.state()), 1);
}
