// Port of tests/authority-settlement.test.ts. The TypeScript writes the state
// to a temporary JSON file and reads it back; the C++ round-trips the same
// JSON text through rt::fs in a temporary directory.
#include "uos/authority/admission_state.hpp"
#include "uos/authority/contracts.hpp"
#include "uos/authority/delivery_outbox.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <optional>
#include <string>

namespace {
  using namespace uos;
  using namespace uos::authority::contracts;
  using uos::authority::admission_state::createAuthorityState;
  using uos::authority::admission_state::PortableAuthority;
  using uos::authority::admission_state::Usage;
  using uos::authority::delivery_outbox::DeliveryOutbox;

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

  const AuthorityScope scope{.namespace_ = "synthetic-settlement", .epoch = 1};

  struct FixtureStore : AuthorityStore {
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
    std::shared_ptr<PortableAuthority> authority;
    std::shared_ptr<FixtureStore> store;
    AuthorityState& state() const { return store->state; }
  };

  Fixture fixture(std::optional<AuthorityState> saved = std::nullopt) {
    const AccountBinding account{
      .accountId = "a",
      .fingerprint = "fp",
      .credentialGeneration = 1,
      .routingGeneration = 1,
      .resetGeneration = 1,
      .overageVersion = 1,
      .overageEnabled = false,
      .enabled = true,
      .models = {"fixture"},
      .proof = AccountProof{.kind = "quota_exhausted", .model = "fixture", .fingerprint = "fp", .observedAtMs = 1, .validUntilMs = 10'000},
    };
    auto store = std::make_shared<FixtureStore>();
    if (saved) {
      store->state = *saved;
    } else {
      AuthorityState initialState = createAuthorityState(scope, {account},
                                                       {PrincipalPolicy{
                                                         .principalId = "p",
                                                         .version = 1,
                                                         .deleted = false,
                                                         .windowResetAtMs = 10'000,
                                                         .requestLimit = 10,
                                                         .kernelLimit = 10,
                                                         .concurrencyLimit = 10,
                                                         .paidLimitMicrocredits = 100,
                                                       }});
      store->state = std::move(initialState);
    }
    return Fixture{.authority = std::make_shared<PortableAuthority>(store, scope), .store = store};
  }

  rt::Task<AdmissionHandle> admitted(std::shared_ptr<PortableAuthority> authority) {
    const AdmitInput request{
      .scope = scope,
      .requestId = "request",
      .principalId = "p",
      .policyVersion = 1,
      .digest = "digest",
      .route = "/v1/responses",
      .model = "fixture",
      .nowMs = 100,
    };
    rt::Task<AdmissionHandle> admitting = authority->admit(request);
    AdmissionHandle handle = co_await std::move(admitting);
    co_return handle;
  }

  const TerminalEvidence completed{.outcome = "completed", .providerRequestId = "openlux-id", .spendMicrocredits = std::nullopt, .fallback = std::nullopt};

  const PrincipalWindow& firstWindow(const AuthorityState& state) { return *state.windows.values().at(0); }
}

ASYNC_TEST("separate paid-hop billing survives abort and terminal/outbox replay after persisted fixture restart") {
  const Fixture f = fixture();
  rt::Task<AdmissionHandle> admitting = admitted(f.authority);
  const AdmissionHandle handle = co_await std::move(admitting);
  const AttemptInput first{.attemptId = "surplus-hop", .provider = "surplus", .maximumExposureMicrocredits = 40, .account = std::nullopt};
  rt::Task<void> reserving = f.authority->reserveAttempt(handle, first, 100);
  co_await std::move(reserving);
  rt::Task<void> reservingAgain = f.authority->reserveAttempt(handle, first, 100);
  co_await std::move(reservingAgain);
  rt::Task<bool> beginning = f.authority->beginDispatch(handle, first.attemptId, 100);
  const bool begun = co_await std::move(beginning);
  EXPECT_EQ(begun, true);
  rt::Task<bool> beginningAgain = f.authority->beginDispatch(handle, first.attemptId, 100);
  const bool begunAgain = co_await std::move(beginningAgain);
  EXPECT_EQ(begunAgain, false);
  rt::Task<void> confirming = f.authority->confirmDispatched(handle, first.attemptId);
  co_await std::move(confirming);
  const TerminalEvidence fallback{.outcome = "failed", .providerRequestId = "surplus-id", .spendMicrocredits = 3, .fallback = "quota_exhausted"};
  rt::Task<void> terminating = f.authority->observeTerminal(handle, first.attemptId, fallback, false);
  co_await std::move(terminating);
  const AttemptInput openlux{.attemptId = "openlux-hop", .provider = "openlux", .maximumExposureMicrocredits = 50, .account = std::nullopt};
  rt::Task<void> reservingOpenlux = f.authority->reserveAttempt(handle, openlux, 100);
  co_await std::move(reservingOpenlux);
  rt::Task<bool> beginningOpenlux = f.authority->beginDispatch(handle, "openlux-hop", 100);
  co_await std::move(beginningOpenlux);
  rt::Task<void> confirmingOpenlux = f.authority->confirmDispatched(handle, "openlux-hop");
  co_await std::move(confirmingOpenlux);
  rt::Task<void> completing = f.authority->observeTerminal(handle, "openlux-hop", completed);
  co_await std::move(completing);
  const PrincipalWindow beforeDelivery = firstWindow(f.state());
  EXPECT_EQ(beforeDelivery.kernelCompleted, 1);
  EXPECT_EQ(beforeDelivery.dispatched, 1);
  EXPECT_EQ(beforeDelivery.paidReservedMicrocredits, 50);
  rt::Task<std::string> settling = f.authority->settle(handle);
  const std::string pending = co_await std::move(settling);
  EXPECT_EQ(pending, "pending");
  const std::string directory = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "uos-authority-restart-"));
  rt::Reason failure;
  try {
    const std::string path = rt::path::join(directory, "state.json");
    rt::fs::writeFileSync(path, rt::jsonStringify(toJson(f.state())));
    const AuthorityState saved = fromJson<AuthorityState>(rt::jsonParse(rt::fs::readFileSync(path, "utf8")));
    const Fixture restart = fixture(saved);
    const auto outbox = std::make_shared<DeliveryOutbox>(restart.authority, handle);
    rt::Task<void> delivering = outbox->delivery("interrupted");
    co_await std::move(delivering);
    rt::Task<void> replaying = outbox->terminal("openlux-hop", completed);
    co_await std::move(replaying);
    const Usage usage{.providerRequestId = "openlux-id", .spendMicrocredits = 12};
    rt::Task<void> using1 = restart.authority->observeUsage(handle, "openlux-hop", usage);
    co_await std::move(using1);
    rt::Task<void> using2 = restart.authority->observeUsage(handle, "openlux-hop", usage);
    co_await std::move(using2);
    rt::Task<void> replayingAgain = outbox->terminal("openlux-hop", completed);
    co_await std::move(replayingAgain);
    rt::Task<std::string> settlingRestart = restart.authority->settle(handle);
    const std::string settled = co_await std::move(settlingRestart);
    EXPECT_EQ(settled, "settled");
    const PrincipalWindow& window = firstWindow(restart.state());
    EXPECT_EQ(window.dispatched, 1);
    EXPECT_EQ(window.reserved, 0);
    EXPECT_EQ(window.kernelCompleted, 1);
    EXPECT_EQ(window.kernelReserved, 0);
    EXPECT_EQ(window.active, 0);
    EXPECT_EQ(window.paidReservedMicrocredits, 0);
    EXPECT_EQ(window.paidSettledMicrocredits, 15);
    const Usage uncorrelated{.providerRequestId = "surplus-id", .spendMicrocredits = 12};
    rt::Task<void> using3 = restart.authority->observeUsage(handle, "openlux-hop", uncorrelated);
    ASSERT_REJECTS(std::move(using3), "usage_correlation_mismatch");
    const Usage conflicting{.providerRequestId = "openlux-id", .spendMicrocredits = 13};
    rt::Task<void> using4 = restart.authority->observeUsage(handle, "openlux-hop", conflicting);
    ASSERT_REJECTS(std::move(using4), "usage_evidence_conflict");
  } catch (...) {
    failure = std::current_exception();
  }
  rt::fs::rmSync(directory, {.recursive = true});
  if (failure) std::rethrow_exception(failure);
}

ASYNC_TEST("indeterminate dispatch stays pending and cannot refund or replay") {
  const Fixture f = fixture();
  rt::Task<AdmissionHandle> admitting = admitted(f.authority);
  const AdmissionHandle handle = co_await std::move(admitting);
  const AttemptInput hop{.attemptId = "hop", .provider = "surplus", .maximumExposureMicrocredits = 50, .account = std::nullopt};
  rt::Task<void> reserving = f.authority->reserveAttempt(handle, hop, 100);
  co_await std::move(reserving);
  rt::Task<bool> beginning = f.authority->beginDispatch(handle, "hop", 100);
  co_await std::move(beginning);
  rt::Task<void> releasing = f.authority->releaseBeforeDispatch(handle, "hop");
  ASSERT_REJECTS(std::move(releasing), "dispatch_indeterminate");
  rt::Task<void> cancelling = f.authority->cancelUndispatched(handle);
  ASSERT_REJECTS(std::move(cancelling), "dispatch_indeterminate");
  rt::Task<void> delivering = f.authority->observeDelivery(handle, "unobserved");
  co_await std::move(delivering);
  rt::Task<std::string> settling = f.authority->settle(handle);
  const std::string settled = co_await std::move(settling);
  EXPECT_EQ(settled, "pending");
  EXPECT_EQ(firstWindow(f.state()).paidReservedMicrocredits, 50);
}

ASYNC_TEST("pretransport cancellation releases every reservation once") {
  const Fixture f = fixture();
  rt::Task<AdmissionHandle> admitting = admitted(f.authority);
  const AdmissionHandle handle = co_await std::move(admitting);
  const AttemptInput hop{.attemptId = "hop", .provider = "surplus", .maximumExposureMicrocredits = 50, .account = std::nullopt};
  rt::Task<void> reserving = f.authority->reserveAttempt(handle, hop, 100);
  co_await std::move(reserving);
  rt::Task<void> cancelling = f.authority->cancelUndispatched(handle);
  co_await std::move(cancelling);
  rt::Task<void> cancellingAgain = f.authority->cancelUndispatched(handle);
  co_await std::move(cancellingAgain);
  const PrincipalWindow& window = firstWindow(f.state());
  EXPECT_EQ(window.reserved, 0);
  EXPECT_EQ(window.dispatched, 0);
  EXPECT_EQ(window.active, 0);
  EXPECT_EQ(window.paidReservedMicrocredits, 0);
  EXPECT_EQ(window.kernelReserved, 0);
}
