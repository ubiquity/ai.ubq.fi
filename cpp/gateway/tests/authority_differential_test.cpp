// Differential cases for src/authority/admission-state.ts on states the ported
// bun tests never build: an admission whose principal window is missing makes
// the TypeScript throw JavaScriptCore's TypeError at the first member access.
// Expected texts were recorded from the TypeScript module under Bun 1.4.0.
#include "uos/authority/admission_state.hpp"
#include "uos/authority/contracts.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"

#include <functional>
#include <memory>
#include <string>

namespace {
  using namespace uos;
  using namespace uos::authority::contracts;
  using uos::authority::admission_state::createAuthorityState;
  using uos::authority::admission_state::PortableAuthority;
  using uos::authority::admission_state::Selection;
  using uos::authority::admission_state::Usage;

  const AuthorityScope scope{.namespace_ = "s", .epoch = 1};

  struct Store : AuthorityStore {
    AuthorityState state;
    double revision = 0;
    rt::Promise<AuthoritySnapshot> read() override { return rt::Promise<AuthoritySnapshot>::resolve(AuthoritySnapshot{.revision = revision, .state = state}); }
    rt::Promise<bool> compareAndSwap(double expected, AuthorityState candidate) override {
      if (expected != revision) return rt::Promise<bool>::resolve(false);
      state = std::move(candidate);
      revision += 1;
      return rt::Promise<bool>::resolve(true);
    }
  };

  struct Fixture {
    std::shared_ptr<Store> store;
    std::shared_ptr<PortableAuthority> authority;
    void drop() const { store->state.windows = {}; }
  };

  Fixture make(bool paid) {
    auto store = std::make_shared<Store>();
    const AccountBinding account{
      .accountId = "a",
      .fingerprint = "fp",
      .credentialGeneration = 1,
      .routingGeneration = 1,
      .resetGeneration = 1,
      .overageVersion = 1,
      .overageEnabled = false,
      .enabled = true,
      .models = {"m"},
      .proof = paid ? std::optional<AccountProof>(AccountProof{.kind = "quota_exhausted", .model = "m", .fingerprint = "fp", .observedAtMs = 1, .validUntilMs = 10'000})
                    : std::nullopt,
    };
    const PrincipalPolicy policy{
      .principalId = "p",
      .version = 1,
      .deleted = false,
      .windowResetAtMs = 10'000,
      .requestLimit = 9,
      .kernelLimit = 9,
      .concurrencyLimit = 9,
      .paidLimitMicrocredits = 100,
    };
    store->state = createAuthorityState(scope, {account}, {policy});
    return Fixture{.store = store, .authority = std::make_shared<PortableAuthority>(store, scope)};
  }

  const AdmitInput request{.scope = scope, .requestId = "r", .principalId = "p", .policyVersion = 1, .digest = "d", .route = "/v1/responses", .model = "m", .nowMs = 100};
  const AttemptInput surplus{.attemptId = "x", .provider = "surplus", .maximumExposureMicrocredits = 1, .account = std::nullopt};

  rt::Task<void> expectTypeError(rt::Task<void> task, std::string expected, const char* file, int line) {
    std::string actual = "no TypeError";
    try {
      co_await std::move(task);
    } catch (const rt::TypeError& error) {
      actual = error.message;
    } catch (...) {
      actual = rt::reasonToString(std::current_exception());
    }
    if (actual != expected) rt::test::report(file, line, "expected TypeError \"" + expected + "\", got " + actual, false);
  }
#define EXPECT_TYPE_ERROR(task, text) co_await expectTypeError((task), std::string(text), __FILE__, __LINE__)

  template <class T> rt::Task<void> discard(rt::Task<T> task) { co_await std::move(task); }

  rt::Task<AdmissionHandle> dispatchedPaid(Fixture f) {
    rt::Task<AdmissionHandle> admitting = f.authority->admit(request);
    const AdmissionHandle handle = co_await std::move(admitting);
    rt::Task<void> reserving = f.authority->reserveAttempt(handle, surplus, 100);
    co_await std::move(reserving);
    rt::Task<bool> beginning = f.authority->beginDispatch(handle, "x", 100);
    co_await std::move(beginning);
    co_return handle;
  }
}

ASYNC_TEST("differential: a missing principal window throws JavaScriptCore's TypeError at each access site") {
  {
    const Fixture f = make(true);
    rt::Task<AdmissionHandle> admitting = f.authority->admit(request);
    const AdmissionHandle handle = co_await std::move(admitting);
    f.drop();
    rt::Task<void> call1 = f.authority->reserveAttempt(handle, surplus, 100);
    EXPECT_TYPE_ERROR(std::move(call1), "undefined is not an object (evaluating 'window.paidReservedMicrocredits')");
  }
  {
    const Fixture f = make(true);
    rt::Task<AdmissionHandle> admitting = f.authority->admit(request);
    const AdmissionHandle handle = co_await std::move(admitting);
    rt::Task<void> reserving = f.authority->reserveAttempt(handle, surplus, 100);
    co_await std::move(reserving);
    f.drop();
    rt::Task<void> call2 = f.authority->releaseBeforeDispatch(handle, "x");
    EXPECT_TYPE_ERROR(std::move(call2), "undefined is not an object (evaluating 'state.windows[request.windowKey].paidReservedMicrocredits')");
    rt::Task<void> call3 = f.authority->cancelUndispatched(handle);
    EXPECT_TYPE_ERROR(std::move(call3), "undefined is not an object (evaluating 'state.windows[request.windowKey].paidReservedMicrocredits')");
  }
  {
    const Fixture f = make(true);
    rt::Task<AdmissionHandle> preparing = dispatchedPaid(f);
    const AdmissionHandle handle = co_await std::move(preparing);
    f.drop();
    rt::Task<void> call4 = f.authority->confirmDispatched(handle, "x");
    EXPECT_TYPE_ERROR(std::move(call4), "undefined is not an object (evaluating 'state.windows[request.windowKey].reserved')");
  }
  {
    const Fixture f = make(true);
    rt::Task<AdmissionHandle> preparing = dispatchedPaid(f);
    const AdmissionHandle handle = co_await std::move(preparing);
    rt::Task<void> confirming = f.authority->confirmDispatched(handle, "x");
    co_await std::move(confirming);
    f.drop();
    const TerminalEvidence pending{.outcome = "completed", .providerRequestId = "p", .spendMicrocredits = std::nullopt, .fallback = std::nullopt};
    rt::Task<void> call5 = f.authority->observeTerminal(handle, "x", pending);
    EXPECT_TYPE_ERROR(std::move(call5), "undefined is not an object (evaluating 'state.windows[request.windowKey].kernelReserved')");
    const TerminalEvidence billed{.outcome = "completed", .providerRequestId = "p", .spendMicrocredits = 1, .fallback = std::nullopt};
    rt::Task<void> call6 = f.authority->observeTerminal(handle, "x", billed, false);
    EXPECT_TYPE_ERROR(std::move(call6), "undefined is not an object (evaluating 'window.paidReservedMicrocredits')");
  }
  {
    const Fixture f = make(true);
    rt::Task<AdmissionHandle> preparing = dispatchedPaid(f);
    const AdmissionHandle handle = co_await std::move(preparing);
    rt::Task<void> confirming = f.authority->confirmDispatched(handle, "x");
    co_await std::move(confirming);
    const TerminalEvidence failed{.outcome = "failed", .providerRequestId = "p", .spendMicrocredits = std::nullopt, .fallback = std::nullopt};
    rt::Task<void> terminating = f.authority->observeTerminal(handle, "x", failed);
    co_await std::move(terminating);
    rt::Task<void> delivering = f.authority->observeDelivery(handle, "unobserved");
    co_await std::move(delivering);
    f.drop();
    rt::Task<void> call7 = f.authority->observeUsage(handle, "x", Usage{.providerRequestId = "p", .spendMicrocredits = 1});
    EXPECT_TYPE_ERROR(std::move(call7), "undefined is not an object (evaluating 'window.paidReservedMicrocredits')");
    rt::Task<void> call8 = discard(f.authority->settle(handle));
    EXPECT_TYPE_ERROR(std::move(call8), "undefined is not an object (evaluating 'window.active')");
  }
  {
    const Fixture f = make(false);
    rt::Task<AdmissionHandle> admitting = f.authority->admit(request);
    const AdmissionHandle handle = co_await std::move(admitting);
    rt::Task<void> delivering = f.authority->observeDelivery(handle, "interrupted");
    co_await std::move(delivering);
    f.drop();
    rt::Task<void> call9 = f.authority->cancelUndispatched(handle);
    EXPECT_TYPE_ERROR(std::move(call9), "undefined is not an object (evaluating 'window.reserved')");
  }
}

ASYNC_TEST("differential: a settled subscription request leaves the window JSON the TypeScript stores") {
  const Fixture f = make(false);
  rt::Task<AdmissionHandle> admitting = f.authority->admit(request);
  const AdmissionHandle handle = co_await std::move(admitting);
  rt::Task<Selection> selecting = f.authority->selection(handle);
  const Selection selected = co_await std::move(selecting);
  const AttemptInput codex{.attemptId = "c", .provider = "codex", .maximumExposureMicrocredits = 0, .account = selected.account};
  rt::Task<void> reserving = f.authority->reserveAttempt(handle, codex, 100);
  co_await std::move(reserving);
  rt::Task<bool> beginning = f.authority->beginDispatch(handle, "c", 100);
  co_await std::move(beginning);
  rt::Task<void> confirming = f.authority->confirmDispatched(handle, "c");
  co_await std::move(confirming);
  const TerminalEvidence completed{.outcome = "completed", .providerRequestId = std::nullopt, .spendMicrocredits = std::nullopt, .fallback = std::nullopt};
  rt::Task<void> terminating = f.authority->observeTerminal(handle, "c", completed);
  co_await std::move(terminating);
  rt::Task<void> delivering = f.authority->observeDelivery(handle, "unobserved");
  co_await std::move(delivering);
  EXPECT_EQ(rt::jsonStringify(toJson(f.store->state).at("windows")),
            R"({"[\"p\",10000]":{"reserved":0,"dispatched":1,"kernelReserved":0,"kernelCompleted":1,"active":1,"paidReservedMicrocredits":0,"paidSettledMicrocredits":0}})");
}

ASYNC_TEST("differential: a \"__proto__\" request id reserves quota but stores no admission; ids enumerate in JS key order") {
  const Fixture f = make(false);
  AdmitInput proto = request;
  proto.requestId = "__proto__";
  rt::Task<AdmissionHandle> admitting = f.authority->admit(proto);
  const AdmissionHandle handle = co_await std::move(admitting);
  EXPECT_EQ(rt::jsonStringify(toJson(handle)), R"({"scope":{"namespace":"s","epoch":1},"requestId":"__proto__","fence":1})");
  EXPECT(f.store->state.admissions.keys().empty());
  EXPECT_EQ(rt::jsonStringify(toJson(f.store->state).at("windows")),
            R"({"[\"p\",10000]":{"reserved":1,"dispatched":0,"kernelReserved":1,"kernelCompleted":0,"active":1,"paidReservedMicrocredits":0,"paidSettledMicrocredits":0}})");
  EXPECT_EQ(f.store->state.sequence, 1);
  std::string refusal = "no error";
  try {
    rt::Task<Selection> selecting = f.authority->selection(handle);
    co_await std::move(selecting);
  } catch (const rt::JsError& error) {
    refusal = error.toString();
  }
  EXPECT_EQ(refusal, "AuthorityError: admission_fence_mismatch");
  const std::vector<std::string> ids{"7", "b", "2"};
  for (const std::string& id : ids) {
    AdmitInput next = request;
    next.requestId = id;
    rt::Task<AdmissionHandle> admittingNext = f.authority->admit(next);
    co_await std::move(admittingNext);
  }
  EXPECT_EQ(f.store->state.admissions.keys(), std::vector<std::string>({"2", "7", "b"}));
  std::vector<std::string> stateKeys;
  const json::value stateJson = toJson(f.store->state);
  for (const auto& member : stateJson.as_object()) stateKeys.emplace_back(member.key());
  EXPECT_EQ(stateKeys, std::vector<std::string>({"scope", "accounts", "policies", "sequence", "active", "windows", "admissions", "publications"}));
}
