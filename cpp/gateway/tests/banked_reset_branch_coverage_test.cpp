// Port of tests/banked-reset-branch-coverage.test.ts.
// Links: tests/banked_reset_branch_coverage_test.cpp tests/helpers/codex_banked_reset_harness.cpp src/codex/reset_settings.cpp
//        src/codex/banked_reset.cpp src/codex/banked_reset_claim.cpp src/codex/banked_reset_submission.cpp src/codex/banked_reset_pool.cpp
//        src/codex/banked_reset_provider.cpp src/codex/auth.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp
//        src/codex/external_auth.cpp src/codex/capacity_routing.cpp src/codex/rate_limit_429.cpp src/codex/routing_mutations.cpp
//        src/codex/routing_state.cpp src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp
//        src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp
//        src/bounded_response_body.cpp src/config.cpp src/kv.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp
//        src/provider/health.cpp src/release.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp
//        src/runtime/http_upgrade.cpp src/runtime/http.cpp src/runtime/websocket.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
// src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp
//        src/utils.cpp src/types.cpp
//
// Every case clears HOME, CODEX_HOME and CODEX_AUTH_JSON_B64 (and config.codexAuthJsonB64) for its
// scope: the reset modules link codex auth, whose native path would otherwise read the host's real
// credential home.
//
// The provider boundary is typed in C++ (RedeemResetResult, bool, ResetInventory), so the TS inputs
// that only an untyped injection can produce reach the module through the harness's boundary mapping
// (toRedeemResetResult: a malformed result becomes a terminal result with an empty receipt, which
// validRedeemResult refuses as the TS refuses the original). Two TS assertions have no C++ counterpart
// and are not ported: "verification_response_invalid" for a non-boolean verifyApplied result (the
// C++ boundary only carries a boolean), and "kv_unavailable" from a `kv` getter that throws (a C++
// dependency member cannot throw when read). A fence whose isCurrent is not a function is an empty
// std::function.
#include "tests/helpers/codex_banked_reset_harness.hpp"

#include "uos/codex/banked_reset.hpp"
#include "uos/codex/banked_reset_claim.hpp"
#include "uos/codex/banked_reset_pool.hpp"
#include "uos/codex/banked_reset_submission.hpp"
#include "uos/config.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace claim = uos::codex::banked_reset_claim;
namespace harness = uos::tests::helpers::codex_banked_reset_harness;
using uos::codex::banked_reset::CodexBankedResetCandidate;
using uos::codex::banked_reset::CodexBankedResetDependencies;
using uos::codex::banked_reset::CodexBankedResetFence;
using uos::codex::banked_reset::CodexBankedResetOutcome;
using uos::codex::banked_reset::CodexBankedResetPoolCandidate;
using uos::codex::banked_reset::CodexBankedResetPoolOutcome;
using uos::codex::banked_reset::MAX_CAS_ATTEMPTS;
using uos::codex::banked_reset::ResetContext;
using uos::codex::banked_reset_provider::ResetInventory;
using uos::codex::banked_reset_provider::ResetInventoryCredit;
using uos::types::CodexResetRedemptionRecord;
using uos::types::CodexResetRedemptionState;
using harness::FakeCodexUsageResetProvider;
using harness::MemoryKv;
using harness::TestClock;

// GCC 12.2 coroutine rule (PORTING.md): every awaited call is bound to a named Task first and its
// result to a named local before an assertion reads it.
#define AWAIT(name, expression)   \
  auto name##Task = (expression); \
  auto name = co_await std::move(name##Task)
// The same for a call whose result the case does not read.
#define AWAIT_DONE(name, expression) \
  auto name##Task = (expression);    \
  co_await std::move(name##Task)

namespace {
  /* ------------------------------------------------------------------ isolation */

  // No credential home and no seeded auth.json for the scope of a case.
  struct IsolatedEnvironment {
    rt::env::ScopedOverride environment{{"HOME", std::nullopt}, {"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt}};
    std::string savedAuthJson = uos::config::config.codexAuthJsonB64;
    IsolatedEnvironment() { uos::config::config.codexAuthJsonB64.clear(); }
    ~IsolatedEnvironment() { uos::config::config.codexAuthJsonB64 = savedAuthJson; }
    IsolatedEnvironment(const IsolatedEnvironment&) = delete;
    IsolatedEnvironment& operator=(const IsolatedEnvironment&) = delete;
  };

  /** A clock whose readings are scripted, so a mid-transaction reading can change. */
  std::function<double()> scriptedClock(std::vector<std::optional<double>> readings) {
    auto index = std::make_shared<std::size_t>(0);
    return [readings = std::move(readings), index]() {
      const std::optional<double> value = readings[std::min(*index, readings.size() - 1)];
      *index += 1;
      return value.value_or(0);
    };
  }

  // A test KvListIterator over fixed entries (`next` resolves at once).
  class FixedListIterator : public rt::KvListIterator {
  public:
    explicit FixedListIterator(std::vector<rt::KvEntry<>> entries_) : entries(std::move(entries_)) {}
    rt::Task<std::optional<rt::KvEntry<>>> next() override {
      if (index < entries.size()) return rt::fulfilledTask(std::optional<rt::KvEntry<>>(entries[index++]));
      return rt::fulfilledTask(std::optional<rt::KvEntry<>>());
    }
    std::string cursor() const override { return ""; }
    rt::Task<void> return_() override { return rt::fulfilledTask(); }

  private:
    std::vector<rt::KvEntry<>> entries;
    std::size_t index = 0;
  };

  template <class T> rt::Task<T> rejectWith(std::string message) {
    throw rt::JsError(message);
    co_return T{};
  }

  // The failing operation of FailKeyKv: every builder returns itself and commit rejects.
  class FailingAtomicOperation : public rt::AtomicOperation {
  public:
    using rt::AtomicOperation::check;
    using rt::AtomicOperation::max;
    using rt::AtomicOperation::min;
    using rt::AtomicOperation::sum;
    rt::AtomicOperation& check(std::vector<rt::AtomicCheck>) override { return *this; }
    rt::AtomicOperation& mutate(std::vector<rt::KvMutation>) override { throw rt::TypeError("failing.mutate is not a function"); }
    rt::AtomicOperation& sum(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("failing.sum is not a function"); }
    rt::AtomicOperation& min(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("failing.min is not a function"); }
    rt::AtomicOperation& max(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("failing.max is not a function"); }
    rt::AtomicOperation& set(rt::KvKey, rt::KvValue, rt::KvSetOptions) override { return *this; }
    rt::AtomicOperation& delete_(rt::KvKey) override { return *this; }
    rt::Task<rt::KvCommitResult> commit() override { return rejectWith<rt::KvCommitResult>("kv commit unavailable"); }
  };

  /** MemoryKv with targeted read failures and a commit that can reject outright. */
  class FailKeyKv : public MemoryKv {
  public:
    std::vector<std::string> failGetKeys;
    bool commitThrows = false;
    std::vector<rt::KvEntry<>> listEntries;
    bool listThrows = false;

    using MemoryKv::get;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override {
      const std::string encoded = harness::encodeKey(key);
      if (std::find(failGetKeys.begin(), failGetKeys.end(), encoded) != failGetKeys.end()) return rejectWith<rt::KvEntryMaybe<>>("kv read unavailable");
      return MemoryKv::get(std::move(key), options);
    }

    std::unique_ptr<rt::AtomicOperation> atomic() override {
      if (!commitThrows) return MemoryKv::atomic();
      return std::make_unique<FailingAtomicOperation>();
    }

    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector, rt::KvListOptions) override {
      if (listThrows) throw rt::JsError("kv list unavailable");
      return std::make_unique<FixedListIterator>(listEntries);
    }
  };

  /** Fails the next `count` atomic commits, whatever their number. */
  void failNextCommits(MemoryKv& kv, double count) {
    const double base = kv.atomicCommitCount;
    for (double index = 1; index <= count; index += 1) kv.failCommitNumbers.add(base + index);
  }

  std::shared_ptr<MemoryKv> asMemory(const std::shared_ptr<FailKeyKv>& kv) { return kv; }

  rt::Task<ResetContext> contextOf(CodexBankedResetCandidate input) {
    rt::Task<std::optional<ResetContext>> making = claim::makeResetContext(input, &harness::testHash);
    std::optional<ResetContext> context = co_await std::move(making);
    if (!context) throw rt::JsError("the fixture candidate must produce a reset context", "AssertionError");
    co_return *context;
  }

  CodexResetRedemptionRecord createRecord(const ResetContext& context, const CodexBankedResetCandidate& input,
                                          CodexResetRedemptionState state = CodexResetRedemptionState::claimed, double nowMs = 1'700'000'000'000) {
    return CodexResetRedemptionRecord{
      .v = 1,
      .account_id_hash = context.account.accountIdHash,
      .credential_version = context.account.credentialVersion,
      .quota_generation = context.account.quotaGeneration,
      .routing_generation = input.routingGeneration,
      .idempotency_key_hash = context.idempotencyKeyHash,
      .state = state,
      .owner_token = "owner-1",
      .fence = 1,
      .lease_expires_at_ms = nowMs - 1,
      .provider_receipt_id = std::nullopt,
      .created_at_ms = nowMs - 1'000,
      .updated_at_ms = nowMs,
      .submitted_at_ms = state == CodexResetRedemptionState::claimed ? std::nullopt : std::optional<double>(nowMs),
      .verified_at_ms = std::nullopt,
      .last_error_code = std::nullopt,
    };
  }

  rt::KvValue recordValue(const CodexResetRedemptionRecord& record) { return rt::KvValue(json::value_from(record)); }

  rt::KvKey recordKeyOf(const ResetContext& context) {
    return harness::codexResetRedemptionKey(context.account.accountIdHash, context.account.quotaGeneration);
  }

  void resetRecordTo(const ResetContext& context, MemoryKv& kv, const CodexBankedResetCandidate& input, double nowMs) {
    const rt::KvKey key = recordKeyOf(context);
    kv.entries.set(harness::encodeKey(key), harness::StoredEntry{
                                              .key = key,
                                              .value = harness::clone(recordValue(createRecord(context, input, CodexResetRedemptionState::submitted, nowMs))),
                                              .versionstamp = "00000000000000000099",
                                            });
  }

  // `assert.deepEqual(result, { kind: "failure", code })` on a claim-module variant.
  template <class Failure, class Variant> bool isFailure(const Variant& value, const std::string& code) {
    const Failure* failure = std::get_if<Failure>(&value);
    return failure != nullptr && failure->code == code;
  }

  std::optional<std::string> stateOf(const std::optional<json::value>& record) {
    if (!record) return std::nullopt;
    const std::optional<std::string_view> state = rt::getString(*record, "state");
    return state ? std::optional<std::string>(std::string(*state)) : std::nullopt;
  }

  json::value fixture(std::string_view kind, std::string_view member, json::value value) {
    json::object result;
    result["kind"] = json::string(kind);
    result[member] = std::move(value);
    return result;
  }

  json::value kindOnly(std::string_view kind) {
    json::object result;
    result["kind"] = json::string(kind);
    return result;
  }

  json::value contractWith(std::string_view key, json::value value) {
    json::object contract = harness::provenContract();
    contract[key] = std::move(value);
    return contract;
  }

  std::shared_ptr<FakeCodexUsageResetProvider> fakeProvider() { return std::make_shared<FakeCodexUsageResetProvider>(); }

  std::shared_ptr<FakeCodexUsageResetProvider> fakeProvider(json::value contract) { return std::make_shared<FakeCodexUsageResetProvider>(std::move(contract)); }
}

/* ---------------------------------------------------------------- claim core */

ASYNC_TEST("makeResetContext refuses an unusable candidate and an unusable hash") {
  IsolatedEnvironment isolation;
  const CodexBankedResetCandidate usable = harness::candidate();
  CodexBankedResetCandidate noAccount = usable;
  noAccount.accountId = "";
  AWAIT(noAccountContext, claim::makeResetContext(noAccount, &harness::testHash));
  EXPECT(!noAccountContext);
  CodexBankedResetCandidate noCredential = usable;
  noCredential.credentialVersion = "";
  AWAIT(noCredentialContext, claim::makeResetContext(noCredential, &harness::testHash));
  EXPECT(!noCredentialContext);
  CodexBankedResetCandidate negativeReset = usable;
  negativeReset.quotaResetAtMs = -1;
  AWAIT(negativeResetContext, claim::makeResetContext(negativeReset, &harness::testHash));
  EXPECT(!negativeResetContext);
  CodexBankedResetCandidate nanReset = usable;
  nanReset.quotaResetAtMs = rt::Number::NaN;
  AWAIT(nanResetContext, claim::makeResetContext(nanReset, &harness::testHash));
  EXPECT(!nanResetContext);
  CodexBankedResetCandidate fractionalGeneration = usable;
  fractionalGeneration.routingGeneration = 1.5;
  AWAIT(fractionalGenerationContext, claim::makeResetContext(fractionalGeneration, &harness::testHash));
  EXPECT(!fractionalGenerationContext);
  CodexBankedResetCandidate negativeGeneration = usable;
  negativeGeneration.routingGeneration = -1;
  AWAIT(negativeGenerationContext, claim::makeResetContext(negativeGeneration, &harness::testHash));
  EXPECT(!negativeGenerationContext);
  const std::function<rt::Task<std::string>(std::string)> emptyHash = [](std::string) { return rt::fulfilledTask(std::string()); };
  AWAIT(emptyHashContext, claim::makeResetContext(usable, emptyHash));
  EXPECT(!emptyHashContext);  // a hash that cannot produce a key is refused
  CodexBankedResetCandidate nanGeneration = usable;
  nanGeneration.routingGeneration = rt::Number::NaN;
  AWAIT(nanGenerationContext, claim::makeResetContext(nanGeneration, &harness::testHash));
  EXPECT(!nanGenerationContext);
  const std::function<rt::Task<std::string>(std::string)> unavailableHash = [](std::string) { return rejectWith<std::string>("hash unavailable"); };
  AWAIT(unavailableHashContext, claim::makeResetContext(usable, unavailableHash));
  EXPECT(!unavailableHashContext);  // an unavailable hash is reported as an unusable context
  AWAIT(emptyHashAgain, claim::makeResetContext(usable, emptyHash));
  EXPECT(!emptyHashAgain);

  AWAIT(context, contextOf(usable));
  EXPECT(rt::Regex("^v1:").test(context.account.credentialVersion));
  EXPECT(rt::Regex("^v1:").test(context.account.quotaGeneration));
  EXPECT(rt::Regex("^uos_ai_codex_reset_v1_").test(context.idempotencyKey));
  EXPECT_EQ(context.account.accountId, usable.accountId);
  const CodexResetRedemptionRecord record = createRecord(context, usable);
  EXPECT_EQ(claim::matchesContext(record, context), true);
  CodexResetRedemptionRecord otherKey = record;
  otherKey.idempotency_key_hash = "other";
  EXPECT_EQ(claim::matchesContext(otherKey, context), false);
  CodexResetRedemptionRecord otherCredential = record;
  otherCredential.credential_version = "v1:other";
  EXPECT_EQ(claim::matchesContext(otherCredential, context), false);
}

TEST("readClock rejects unsafe readings and a clock that throws") {
  IsolatedEnvironment isolation;
  EXPECT_EQ(claim::readClock([] { return 1'700'000'000'000.0; }), std::optional<double>(1'700'000'000'000));
  EXPECT_EQ(claim::readClock([] { return rt::Number::NaN; }), std::optional<double>());
  EXPECT_EQ(claim::readClock([] { return -1.0; }), std::optional<double>());
  const std::function<double()> unavailable = []() -> double { throw rt::JsError("clock unavailable"); };
  // an unavailable clock is reported as an unreadable reading
  EXPECT_EQ(claim::readClock(unavailable), std::optional<double>());
}

ASYNC_TEST("readCurrentFences refuses a candidate without usable fences and treats an exploding predicate as stale") {
  IsolatedEnvironment isolation;
  auto kv = std::make_shared<MemoryKv>();
  const CodexBankedResetCandidate input = harness::candidate();
  AWAIT_DONE(seeded, harness::seedFences(kv, input));

  CodexBankedResetCandidate noFences = input;
  noFences.fences = {};
  AWAIT(missing, claim::readCurrentFences(kv, noFences));
  EXPECT((isFailure<claim::FenceReadFailure>(missing, "routing_fence_missing")));
  CodexBankedResetCandidate malformedFences = input;
  malformedFences.fences = {CodexBankedResetFence{.key = harness::routingFenceKey(input.accountId), .isCurrent = {}}};
  AWAIT(malformed, claim::readCurrentFences(kv, malformedFences));
  EXPECT((isFailure<claim::FenceReadFailure>(malformed, "routing_fence_missing")));

  CodexBankedResetCandidate explodingFences = input;
  explodingFences.fences = {CodexBankedResetFence{
    .key = harness::routingFenceKey(input.accountId),
    .isCurrent = [](const json::value&) -> bool { throw rt::JsError("fence predicate unavailable"); },
  }};
  AWAIT(exploding, claim::readCurrentFences(kv, explodingFences));
  EXPECT(std::holds_alternative<claim::FenceReadStale>(exploding));  // an unreadable predicate proves nothing about the fence

  AWAIT(valid, claim::readCurrentFences(kv, input));
  ASSERT(std::holds_alternative<claim::FenceReadValid>(valid));  // the fixture must be a valid decision
  EXPECT_EQ(std::get<claim::FenceReadValid>(valid).entries.size(), std::size_t{2});

  auto failing = std::make_shared<FailKeyKv>();
  failing->failGetKeys.push_back(harness::encodeKey(harness::routingFenceKey(input.accountId)));
  AWAIT(unreadable, claim::readCurrentFences(failing, input));
  EXPECT((isFailure<claim::FenceReadFailure>(unreadable, "kv_unavailable")));
}

ASYNC_TEST("claimTransaction refuses a quota window that expired after the fence reads") {
  IsolatedEnvironment isolation;
  auto kv = std::make_shared<MemoryKv>();
  const TestClock clock;
  const CodexBankedResetCandidate input = harness::candidate();
  AWAIT_DONE(seeded, harness::seedFences(kv, input));
  AWAIT(context, contextOf(input));

  const std::function<double()> closedClock = scriptedClock({input.quotaResetAtMs});
  AWAIT(claimed, claim::claimTransaction(kv, context, input, input.quotaResetAtMs - 1'000, closedClock, "owner-1", true));
  // the window is rechecked after the asynchronous fence reads
  EXPECT((isFailure<claim::ClaimResultFailure>(claimed, "quota_window_expired")));
  EXPECT_EQ(kv->entries.size(), std::size_t{2});  // an expired window writes nothing but its fences
  EXPECT_EQ(clock.nowMs, 1'700'000'000'000.0);
}

ASYNC_TEST("claimTransaction reports an unusable KV and an exhausted compare-and-set") {
  IsolatedEnvironment isolation;
  const CodexBankedResetCandidate input = harness::candidate();
  AWAIT(context, contextOf(input));
  auto clock = std::make_shared<TestClock>();
  const std::function<double()> now = [clock] { return clock->nowMs; };

  auto unreadable = std::make_shared<FailKeyKv>();
  AWAIT_DONE(seededUnreadable, harness::seedFences(asMemory(unreadable), input));
  unreadable->failGetKeys.push_back(harness::encodeKey(harness::routingFenceKey(input.accountId)));
  AWAIT(unavailable, claim::claimTransaction(unreadable, context, input, clock->nowMs, now, "owner-1", true));
  // an unreadable fence read is reported, never retried blindly
  EXPECT((isFailure<claim::ClaimResultFailure>(unavailable, "kv_unavailable")));

  auto contending = std::make_shared<MemoryKv>();
  AWAIT_DONE(seededContending, harness::seedFences(contending, input));
  failNextCommits(*contending, MAX_CAS_ATTEMPTS);
  AWAIT(exhausted, claim::claimTransaction(contending, context, input, clock->nowMs, now, "owner-1", true));
  EXPECT((isFailure<claim::ClaimResultFailure>(exhausted, "kv_cas_exhausted")));

  auto claimThrows = std::make_shared<FailKeyKv>();
  AWAIT_DONE(seededThrows, harness::seedFences(asMemory(claimThrows), input));
  claimThrows->commitThrows = true;
  AWAIT(claimUnavailable, claim::claimTransaction(claimThrows, context, input, clock->nowMs, now, "owner-1", true));
  EXPECT((isFailure<claim::ClaimResultFailure>(claimUnavailable, "kv_unavailable")));
}

ASYNC_TEST("a takeover of an expired claim is fenced on the routing generation and survives a lost race") {
  IsolatedEnvironment isolation;
  auto kv = std::make_shared<MemoryKv>();
  auto clock = std::make_shared<TestClock>();
  const std::function<double()> now = [clock] { return clock->nowMs; };
  const CodexBankedResetCandidate input = harness::candidate();
  AWAIT_DONE(seeded, harness::seedFences(kv, input));
  AWAIT(context, contextOf(input));
  AWAIT(claimed, claim::claimTransaction(kv, context, input, clock->nowMs, now, "owner-1", true));
  EXPECT(std::holds_alternative<claim::ClaimResultSubmit>(claimed));
  clock->advance(harness::CODEX_BANKED_RESET_LEASE_MS + 1);

  CodexBankedResetCandidate nextGeneration = input;
  nextGeneration.routingGeneration = input.routingGeneration + 1;
  AWAIT(staleGeneration, claim::claimTransaction(kv, context, nextGeneration, clock->nowMs, now, "owner-2", true));
  // a claimed record cannot be taken over across a routing generation
  EXPECT((isFailure<claim::ClaimResultFailure>(staleGeneration, "routing_fence_stale")));

  auto contended = std::make_shared<MemoryKv>();
  AWAIT_DONE(seededContended, harness::seedFences(contended, input));
  const double earlier = clock->nowMs - harness::CODEX_BANKED_RESET_LEASE_MS - 1;
  const std::function<double()> earlierClock = [earlier] { return earlier; };
  AWAIT(claimedThere, claim::claimTransaction(contended, context, input, earlier, earlierClock, "owner-1", true));
  EXPECT(std::holds_alternative<claim::ClaimResultSubmit>(claimedThere));
  failNextCommits(*contended, MAX_CAS_ATTEMPTS);
  AWAIT(lostRace, claim::claimTransaction(contended, context, input, clock->nowMs, now, "owner-2", true));
  EXPECT((isFailure<claim::ClaimResultFailure>(lostRace, "kv_cas_exhausted")));

  AWAIT(exhaustedPermit, claim::claimTransaction(kv, context, input, clock->nowMs, now, "owner-2", false));
  // an expired claim without a new-submission permit is never taken over
  EXPECT(std::holds_alternative<claim::ClaimResultInProgress>(exhaustedPermit));

  auto throwing = std::make_shared<FailKeyKv>();
  AWAIT_DONE(seededThrowing, harness::seedFences(asMemory(throwing), input));
  auto firstClock = std::make_shared<TestClock>();
  const std::function<double()> firstNow = [firstClock] { return firstClock->nowMs; };
  AWAIT(claimedThrow, claim::claimTransaction(throwing, context, input, firstClock->nowMs, firstNow, "owner-1", true));
  EXPECT(std::holds_alternative<claim::ClaimResultSubmit>(claimedThrow));
  firstClock->advance(harness::CODEX_BANKED_RESET_LEASE_MS + 1);
  throwing->commitThrows = true;
  AWAIT(takeoverUnavailable, claim::claimTransaction(throwing, context, input, firstClock->nowMs, firstNow, "owner-2", true));
  EXPECT((isFailure<claim::ClaimResultFailure>(takeoverUnavailable, "kv_unavailable")));
}

ASYNC_TEST("claimTransaction reconciles an expired submitted record without a new submission permit") {
  IsolatedEnvironment isolation;
  auto kv = std::make_shared<MemoryKv>();
  auto clock = std::make_shared<TestClock>();
  const std::function<double()> now = [clock] { return clock->nowMs; };
  const CodexBankedResetCandidate input = harness::candidate();
  AWAIT_DONE(seeded, harness::seedFences(kv, input));
  AWAIT(context, contextOf(input));
  AWAIT(claimed, claim::claimTransaction(kv, context, input, clock->nowMs, now, "owner-1", true));
  ASSERT(std::holds_alternative<claim::ClaimResultSubmit>(claimed));  // the fixture must be a submit decision
  claim::StateWithPatch submittedPatch;
  submittedPatch.submitted_at_ms = clock->nowMs;
  const rt::KvValue submitted = recordValue(claim::stateWith(std::get<claim::ClaimResultSubmit>(claimed).record, CodexResetRedemptionState::submitted,
                                                             clock->nowMs, submittedPatch));
  AWAIT_DONE(written, kv->set(recordKeyOf(context), submitted));
  clock->advance(harness::CODEX_BANKED_RESET_LEASE_MS + 1);

  AWAIT(reconciled, claim::claimTransaction(kv, context, input, clock->nowMs, now, "owner-2", false));
  ASSERT(std::holds_alternative<claim::ClaimResultReconcile>(reconciled));  // an expired submitted record is taken over for reconciliation
  const claim::ClaimResultReconcile& reconcile = std::get<claim::ClaimResultReconcile>(reconciled);
  EXPECT_EQ(reconcile.tookOver, true);
  EXPECT_EQ(reconcile.record.owner_token, std::string("owner-2"));
  EXPECT_EQ(reconcile.record.fence, 2.0);
}

ASYNC_TEST("updateOwnedRecord writes only for the exact owner and fence") {
  IsolatedEnvironment isolation;
  auto kv = std::make_shared<MemoryKv>();
  auto clock = std::make_shared<TestClock>();
  const std::function<double()> now = [clock] { return clock->nowMs; };
  const CodexBankedResetCandidate input = harness::candidate();
  AWAIT_DONE(seeded, harness::seedFences(kv, input));
  AWAIT(context, contextOf(input));
  AWAIT(claimed, claim::claimTransaction(kv, context, input, clock->nowMs, now, "owner-1", true));
  ASSERT(std::holds_alternative<claim::ClaimResultSubmit>(claimed));  // the fixture must be a submit decision
  const CodexResetRedemptionRecord claimedRecord = std::get<claim::ClaimResultSubmit>(claimed).record;

  CodexResetRedemptionRecord foreignRecord = claimedRecord;
  foreignRecord.owner_token = "someone-else";
  AWAIT(foreign, claim::rejectOwned(kv, context, foreignRecord, clock->nowMs, "not_ours"));
  EXPECT(!foreign);  // another owner's record is never mutated
  CodexResetRedemptionRecord replacedRecord = claimedRecord;
  replacedRecord.fence = claimedRecord.fence + 1;
  AWAIT(replaced, claim::rejectOwned(kv, context, replacedRecord, clock->nowMs, "fence_mismatch"));
  EXPECT(!replaced);  // a newer fence means this worker no longer owns the record
  AWAIT(rejected, claim::rejectOwned(kv, context, claimedRecord, clock->nowMs, "provider_rejected"));
  ASSERT(rejected.has_value());
  EXPECT(rejected->state == CodexResetRedemptionState::rejected);
  EXPECT_EQ(rejected->last_error_code, std::optional<std::string>("provider_rejected"));

  auto throwing = std::make_shared<FailKeyKv>();
  AWAIT_DONE(seededThrowing, harness::seedFences(asMemory(throwing), input));
  AWAIT(claimedThere, claim::claimTransaction(throwing, context, input, clock->nowMs, now, "owner-1", true));
  ASSERT(std::holds_alternative<claim::ClaimResultSubmit>(claimedThere));  // the fixture must be a submit decision
  const CodexResetRedemptionRecord claimedThereRecord = std::get<claim::ClaimResultSubmit>(claimedThere).record;
  throwing->commitThrows = true;
  AWAIT(unavailable, claim::rejectOwned(throwing, context, claimedThereRecord, clock->nowMs, "unavailable"));
  EXPECT(!unavailable);
  auto failingReads = std::make_shared<FailKeyKv>();
  AWAIT_DONE(seededFailing, harness::seedFences(asMemory(failingReads), input));
  failingReads->failGetKeys.push_back(harness::encodeKey(recordKeyOf(context)));
  AWAIT(unreadable, claim::rejectOwned(failingReads, context, claimedThereRecord, clock->nowMs, "unreadable"));
  EXPECT(!unreadable);
}

ASYNC_TEST("prepareSubmission refuses a claim that lost its lease, an unreadable budget and an exhausted compare-and-set") {
  IsolatedEnvironment isolation;
  const CodexBankedResetCandidate input = harness::candidate();
  AWAIT(context, contextOf(input));

  auto clock = std::make_shared<TestClock>();
  const std::function<double()> now = [clock] { return clock->nowMs; };
  auto kv = std::make_shared<MemoryKv>();
  AWAIT_DONE(seeded, harness::seedFences(kv, input));
  AWAIT(claimed, claim::claimTransaction(kv, context, input, clock->nowMs, now, "owner-1", true));
  ASSERT(std::holds_alternative<claim::ClaimResultSubmit>(claimed));  // the fixture must be a submit decision
  clock->advance(harness::CODEX_BANKED_RESET_LEASE_MS + 1);
  AWAIT(stale, claim::prepareSubmission(kv, context, input, std::get<claim::ClaimResultSubmit>(claimed).record, clock->nowMs, now, 5));
  // an expired lease never crosses the submission boundary
  EXPECT((isFailure<claim::SubmissionPreparationFailure>(stale, "stale_owner")));

  auto fresh = std::make_shared<TestClock>();
  const std::function<double()> freshNow = [fresh] { return fresh->nowMs; };
  auto budgetKv = std::make_shared<FailKeyKv>();
  AWAIT_DONE(seededBudget, harness::seedFences(asMemory(budgetKv), input));
  AWAIT(budgetClaim, claim::claimTransaction(budgetKv, context, input, fresh->nowMs, freshNow, "owner-1", true));
  ASSERT(std::holds_alternative<claim::ClaimResultSubmit>(budgetClaim));  // the fixture must be a submit decision
  budgetKv->failGetKeys.push_back(harness::encodeKey(harness::codexResetAccountDailyKey(context.account.accountIdHash, "2023-11-14")));
  AWAIT(unreadableBudget,
        claim::prepareSubmission(budgetKv, context, input, std::get<claim::ClaimResultSubmit>(budgetClaim).record, fresh->nowMs, freshNow, 5));
  EXPECT((isFailure<claim::SubmissionPreparationFailure>(unreadableBudget, "kv_unavailable")));

  auto commitClock = std::make_shared<TestClock>();
  const std::function<double()> commitNow = [commitClock] { return commitClock->nowMs; };
  auto commitKv = std::make_shared<FailKeyKv>();
  AWAIT_DONE(seededCommit, harness::seedFences(asMemory(commitKv), input));
  AWAIT(commitClaim, claim::claimTransaction(commitKv, context, input, commitClock->nowMs, commitNow, "owner-1", true));
  ASSERT(std::holds_alternative<claim::ClaimResultSubmit>(commitClaim));  // the fixture must be a submit decision
  commitKv->commitThrows = true;
  AWAIT(unavailableCommit,
        claim::prepareSubmission(commitKv, context, input, std::get<claim::ClaimResultSubmit>(commitClaim).record, commitClock->nowMs, commitNow, 5));
  EXPECT((isFailure<claim::SubmissionPreparationFailure>(unavailableCommit, "kv_unavailable")));

  auto contendedClock = std::make_shared<TestClock>();
  const std::function<double()> contendedNow = [contendedClock] { return contendedClock->nowMs; };
  auto contended = std::make_shared<MemoryKv>();
  AWAIT_DONE(seededContended, harness::seedFences(contended, input));
  AWAIT(contendedClaim, claim::claimTransaction(contended, context, input, contendedClock->nowMs, contendedNow, "owner-1", true));
  ASSERT(std::holds_alternative<claim::ClaimResultSubmit>(contendedClaim));  // the fixture must be a submit decision
  failNextCommits(*contended, MAX_CAS_ATTEMPTS);
  AWAIT(exhausted, claim::prepareSubmission(contended, context, input, std::get<claim::ClaimResultSubmit>(contendedClaim).record, contendedClock->nowMs,
                                            contendedNow, 5));
  EXPECT((isFailure<claim::SubmissionPreparationFailure>(exhausted, "kv_cas_exhausted")));

  auto happy = std::make_shared<MemoryKv>();
  AWAIT_DONE(seededHappy, harness::seedFences(happy, input));
  auto happyClock = std::make_shared<TestClock>();
  const std::function<double()> happyNow = [happyClock] { return happyClock->nowMs; };
  AWAIT(happyClaim, claim::claimTransaction(happy, context, input, happyClock->nowMs, happyNow, "owner-1", true));
  ASSERT(std::holds_alternative<claim::ClaimResultSubmit>(happyClaim));  // the fixture must be a submit decision
  AWAIT(prepared, claim::prepareSubmission(happy, context, input, std::get<claim::ClaimResultSubmit>(happyClaim).record, happyClock->nowMs, happyNow, 5));
  ASSERT(std::holds_alternative<claim::SubmissionPreparationSubmitted>(prepared));  // the submission boundary must be crossed
  EXPECT(std::get<claim::SubmissionPreparationSubmitted>(prepared).record.state == CodexResetRedemptionState::submitted);
  const harness::StoredEntry* daily = happy->entries.get(harness::encodeKey(harness::codexResetAccountDailyKey(context.account.accountIdHash, "2023-11-14")));
  ASSERT(daily != nullptr);
  // the daily budget is consumed exactly once at the boundary
  EXPECT_EQ(daily->value["submission_count"].asNumber(), 1.0);
}

ASYNC_TEST("renewSubmittedForRedeem requires an owned submitted lease and reports a broken binding") {
  IsolatedEnvironment isolation;
  const CodexBankedResetCandidate input = harness::candidate();
  AWAIT(context, contextOf(input));
  auto clock = std::make_shared<TestClock>();
  const std::function<double()> now = [clock] { return clock->nowMs; };
  auto kv = std::make_shared<MemoryKv>();
  AWAIT_DONE(seeded, harness::seedFences(kv, input));
  AWAIT(claimed, claim::claimTransaction(kv, context, input, clock->nowMs, now, "owner-1", true));
  ASSERT(std::holds_alternative<claim::ClaimResultSubmit>(claimed));  // the fixture must be a submit decision
  AWAIT(prepared, claim::prepareSubmission(kv, context, input, std::get<claim::ClaimResultSubmit>(claimed).record, clock->nowMs, now, 5));
  ASSERT(std::holds_alternative<claim::SubmissionPreparationSubmitted>(prepared));  // expected a submitted record
  const CodexResetRedemptionRecord preparedRecord = std::get<claim::SubmissionPreparationSubmitted>(prepared).record;

  AWAIT(renewed, claim::renewSubmittedForRedeem(kv, context, input, preparedRecord, now));
  ASSERT(std::holds_alternative<claim::SubmissionRenewalRenewed>(renewed));  // expected the lease to be renewed
  const CodexResetRedemptionRecord renewedRecord = std::get<claim::SubmissionRenewalRenewed>(renewed).record;
  EXPECT_EQ(renewedRecord.fence, preparedRecord.fence + 1);
  EXPECT_EQ(renewedRecord.owner_token, preparedRecord.owner_token);

  clock->advance(harness::CODEX_BANKED_RESET_LEASE_MS + 1);
  AWAIT(stale, claim::renewSubmittedForRedeem(kv, context, input, renewedRecord, now));
  EXPECT((isFailure<claim::SubmissionRenewalFailure>(stale, "stale_owner")));

  auto throwing = std::make_shared<FailKeyKv>();
  auto throwingClock = std::make_shared<TestClock>();
  const std::function<double()> throwingNow = [throwingClock] { return throwingClock->nowMs; };
  AWAIT_DONE(seededThrowing, harness::seedFences(asMemory(throwing), input));
  AWAIT(throwingClaim, claim::claimTransaction(throwing, context, input, throwingClock->nowMs, throwingNow, "owner-1", true));
  ASSERT(std::holds_alternative<claim::ClaimResultSubmit>(throwingClaim));  // the fixture must be a submit decision
  AWAIT(throwingPrepared, claim::prepareSubmission(throwing, context, input, std::get<claim::ClaimResultSubmit>(throwingClaim).record,
                                                   throwingClock->nowMs, throwingNow, 5));
  ASSERT(std::holds_alternative<claim::SubmissionPreparationSubmitted>(throwingPrepared));  // expected a submitted record
  throwing->commitThrows = true;
  AWAIT(throwingRenewal,
        claim::renewSubmittedForRedeem(throwing, context, input, std::get<claim::SubmissionPreparationSubmitted>(throwingPrepared).record, throwingNow));
  EXPECT((isFailure<claim::SubmissionRenewalFailure>(throwingRenewal, "kv_unavailable")));

  auto contended = std::make_shared<MemoryKv>();
  auto contendedClock = std::make_shared<TestClock>();
  const std::function<double()> contendedNow = [contendedClock] { return contendedClock->nowMs; };
  AWAIT_DONE(seededContended, harness::seedFences(contended, input));
  AWAIT(contendedClaim, claim::claimTransaction(contended, context, input, contendedClock->nowMs, contendedNow, "owner-1", true));
  ASSERT(std::holds_alternative<claim::ClaimResultSubmit>(contendedClaim));  // the fixture must be a submit decision
  AWAIT(contendedPrepared, claim::prepareSubmission(contended, context, input, std::get<claim::ClaimResultSubmit>(contendedClaim).record,
                                                    contendedClock->nowMs, contendedNow, 5));
  ASSERT(std::holds_alternative<claim::SubmissionPreparationSubmitted>(contendedPrepared));  // expected a submitted record
  failNextCommits(*contended, MAX_CAS_ATTEMPTS);
  AWAIT(contendedRenewal, claim::renewSubmittedForRedeem(contended, context, input,
                                                         std::get<claim::SubmissionPreparationSubmitted>(contendedPrepared).record, contendedNow));
  EXPECT((isFailure<claim::SubmissionRenewalFailure>(contendedRenewal, "kv_cas_exhausted")));
}

TEST("credit selection ranks finite expiry first and refuses unusable inventories") {
  IsolatedEnvironment isolation;
  const FakeCodexUsageResetProvider provider;
  const double nowMs = 1'700'000'000'000;
  const ResetInventory expired = harness::inventory("expired", nowMs - 1'000);
  ResetInventory emptyInventory = expired;
  emptyInventory.availableCount = 0;
  emptyInventory.credits = {};
  EXPECT(std::holds_alternative<claim::InventoryCreditSelectionEmpty>(claim::selectInventoryCredit(emptyInventory, &provider, nowMs)));
  const ResetInventory wrongType{
    .availableCount = 1,
    .observedAtMs = nowMs,
    .credits = {ResetInventoryCredit{.id = "wrong-type", .status = "available", .resetType = "other", .expiresAtMs = std::nullopt}},
  };
  EXPECT(std::holds_alternative<claim::InventoryCreditSelectionNoEligibleCredit>(claim::selectInventoryCredit(wrongType, &provider, nowMs)));
  // an available credit that already expired is not selectable
  EXPECT(std::holds_alternative<claim::InventoryCreditSelectionNoEligibleCredit>(claim::selectInventoryCredit(expired, &provider, nowMs)));

  const ResetInventory later = harness::inventory("later", nowMs + 60'000);
  const ResetInventory earlier = harness::inventory("earlier", nowMs + 30'000);
  ResetInventory twoCredits{.availableCount = 2, .observedAtMs = nowMs, .credits = {}};
  for (ResetInventoryCredit credit : later.credits) {
    credit.status = "available";
    twoCredits.credits.push_back(credit);
  }
  for (ResetInventoryCredit credit : earlier.credits) {
    credit.status = "available";
    twoCredits.credits.push_back(credit);
  }
  const claim::InventoryCreditSelection finite = claim::selectInventoryCredit(twoCredits, &provider, nowMs);
  ASSERT(std::holds_alternative<claim::InventoryCreditSelectionSelected>(finite));  // the fixture must be a selected decision
  EXPECT_EQ(std::get<claim::InventoryCreditSelectionSelected>(finite).credit.id, std::string("earlier"));  // the soonest finite expiry wins

  ResetInventoryCredit sameExpiryA = earlier.credits[0];
  sameExpiryA.id = "zzz";
  ResetInventoryCredit sameExpiryB = earlier.credits[0];
  sameExpiryB.id = "aaa";
  ResetInventory tieInventory{.availableCount = 2, .observedAtMs = nowMs, .credits = {sameExpiryA, sameExpiryB}};
  for (ResetInventoryCredit& credit : tieInventory.credits) credit.status = "available";
  const claim::InventoryCreditSelection tie = claim::selectInventoryCredit(tieInventory, &provider, nowMs);
  ASSERT(std::holds_alternative<claim::InventoryCreditSelectionSelected>(tie));  // the fixture must be a selected decision
  EXPECT_EQ(std::get<claim::InventoryCreditSelectionSelected>(tie).credit.id, std::string("aaa"));  // an expiry tie is broken by the opaque credit id

  ResetInventory foreverInventory{
    .availableCount = 2,
    .observedAtMs = nowMs,
    .credits = {ResetInventoryCredit{.id = "forever", .status = "available", .resetType = "codex_rate_limits", .expiresAtMs = std::nullopt}},
  };
  for (ResetInventoryCredit credit : earlier.credits) {
    credit.status = "available";
    foreverInventory.credits.push_back(credit);
  }
  const claim::InventoryCreditSelection neverExpires = claim::selectInventoryCredit(foreverInventory, &provider, nowMs);
  ASSERT(std::holds_alternative<claim::InventoryCreditSelectionSelected>(neverExpires));  // the fixture must be a selected decision
  // a finite expiry beats a credit that never expires
  EXPECT_EQ(std::get<claim::InventoryCreditSelectionSelected>(neverExpires).credit.id, std::string("earlier"));
}

TEST("redeem-result and receipt validation reject unusable provider output") {
  IsolatedEnvironment isolation;
  // Untyped results reach validRedeemResult through the provider boundary mapping.
  EXPECT_EQ(claim::validRedeemResult(harness::toRedeemResetResult(json::value("completed"))), false);  // a non-object result is never trusted
  // a terminal result without a receipt is unusable
  EXPECT_EQ(claim::validRedeemResult(harness::toRedeemResetResult(kindOnly("completed"))), false);
  EXPECT_EQ(claim::validRedeemResult(harness::toRedeemResetResult(fixture("accepted", "providerReceiptId", rt::makeNumber(7.0)))), false);
  EXPECT_EQ(claim::validRedeemResult(harness::toRedeemResetResult(kindOnly("rejected"))), false);  // a rejection without a reason is unusable
  EXPECT_EQ(claim::validRedeemResult(harness::toRedeemResetResult(fixture("rejected", "reason", json::string("definitive")))), true);
  EXPECT_EQ(claim::validRedeemResult(harness::toRedeemResetResult(fixture("unknown", "providerReceiptId", nullptr))), true);
  EXPECT_EQ(claim::validRedeemResult(harness::toRedeemResetResult(fixture("unknown", "providerReceiptId", json::string("receipt")))), true);
  // an unknown result kind is schema drift
  EXPECT_EQ(claim::validRedeemResult(harness::toRedeemResetResult(fixture("future_kind", "providerReceiptId", json::string("receipt")))), false);
  EXPECT_EQ(claim::receiptId("receipt-1"), std::optional<std::string>("receipt-1"));
  EXPECT_EQ(claim::receiptId(json::value(rt::repeat("x", 513))), std::optional<std::string>());  // an over-long receipt id is refused
  EXPECT_EQ(claim::receiptId("   "), std::optional<std::string>("   "));  // any bounded opaque text is accepted verbatim
  EXPECT_EQ(claim::receiptId(rt::makeNumber(7.0)), std::optional<std::string>());
  const FakeCodexUsageResetProvider unsafeProvider(contractWith("receiptIdsSafeToPersistAndLog", false));
  // an unapproved receipt id stays out of the durable record
  EXPECT_EQ(claim::durableReceiptId(&unsafeProvider, json::value("receipt-1")), std::optional<std::string>());
  const FakeCodexUsageResetProvider approvedProvider;
  EXPECT_EQ(claim::durableReceiptId(&approvedProvider, json::value("receipt-1")), std::optional<std::string>("receipt-1"));
}

TEST("loadLiveSubmissionConfig names every reason a live submission is closed") {
  IsolatedEnvironment isolation;
  auto kv = std::make_shared<MemoryKv>();
  auto clock = std::make_shared<TestClock>();
  auto provider = fakeProvider();
  const CodexBankedResetDependencies live = harness::dependencies(kv, provider, clock);
  EXPECT_EQ(claim::loadLiveSubmissionConfig(live).reason, std::optional<std::string>());

  const auto reasonWith = [&live](const harness::ConfigOverrides& overrides) {
    CodexBankedResetDependencies changed = live;
    changed.config = harness::config(overrides);
    return claim::loadLiveSubmissionConfig(changed).reason;
  };
  EXPECT_EQ(reasonWith({.enabled = false}), std::optional<std::string>("feature_disabled"));
  EXPECT_EQ(reasonWith({.mode = "disabled"}), std::optional<std::string>("mode_disabled"));
  EXPECT_EQ(reasonWith({.maxPerAccountPerDay = 0}), std::optional<std::string>("per_account_day_limit_invalid"));
  EXPECT_EQ(reasonWith({.maxPerAccountPerWindow = 2}), std::optional<std::string>("per_account_window_limit_invalid"));
  EXPECT_EQ(reasonWith({.mode = "shadow"}), std::optional<std::string>("mode_not_live"));
  const CodexBankedResetDependencies exploding =
    harness::dependencies(kv, provider, clock, harness::config(), {}, []() -> harness::CodexBankedResetConfig { throw rt::JsError("configuration unavailable"); });
  EXPECT_EQ(claim::loadLiveSubmissionConfig(exploding).reason, std::optional<std::string>("configuration_unavailable"));
}

ASYNC_TEST("readExistingRecord distinguishes an absent, a corrupt and an unreadable record") {
  IsolatedEnvironment isolation;
  auto kv = std::make_shared<FailKeyKv>();
  const CodexBankedResetCandidate input = harness::candidate();
  AWAIT(context, contextOf(input));
  const rt::KvKey key = recordKeyOf(context);

  AWAIT(absent, claim::readExistingRecord(kv, context));
  EXPECT(!absent.record && !absent.code);
  json::object corrupt;
  corrupt["v"] = 1;
  corrupt["state"] = "claimed";
  AWAIT_DONE(wroteCorrupt, kv->set(key, rt::KvValue(corrupt)));
  AWAIT(invalid, claim::readExistingRecord(kv, context));
  EXPECT(!invalid.record);
  EXPECT_EQ(invalid.code, std::optional<std::string>("redemption_record_invalid"));
  AWAIT_DONE(wroteValid, kv->set(key, recordValue(createRecord(context, input))));
  AWAIT(read, claim::readExistingRecord(kv, context));
  EXPECT(read.record.has_value());
  EXPECT_EQ(read.code, std::optional<std::string>());
  kv->failGetKeys.push_back(harness::encodeKey(key));
  AWAIT(unreadable, claim::readExistingRecord(kv, context));
  EXPECT(!unreadable.record);
  EXPECT_EQ(unreadable.code, std::optional<std::string>("kv_unavailable"));
  EXPECT_EQ(claim::quotaWindowIsOpen(input, input.quotaResetAtMs - 1), true);
  EXPECT_EQ(claim::quotaWindowIsOpen(input, input.quotaResetAtMs), false);
}

ASYNC_TEST("listCodexResetShadowDecisions reports an unavailable binding and sorts usable records") {
  IsolatedEnvironment isolation;
  const std::optional<std::shared_ptr<rt::Kv>> nullKv = std::shared_ptr<rt::Kv>();
  AWAIT(unavailable, uos::codex::banked_reset_pool::listCodexResetShadowDecisions(nullKv));
  EXPECT(!unavailable);
  auto kv = std::make_shared<FailKeyKv>();
  const std::optional<std::shared_ptr<rt::Kv>> emptyKv = std::shared_ptr<rt::Kv>(kv);
  AWAIT(empty, uos::codex::banked_reset_pool::listCodexResetShadowDecisions(emptyKv));
  EXPECT(empty.has_value() && empty->empty());
  kv->listThrows = true;
  AWAIT(unreadable, uos::codex::banked_reset_pool::listCodexResetShadowDecisions(emptyKv));
  EXPECT(!unreadable);  // an unreadable store is reported instead of an empty list

  auto decisions = std::make_shared<FailKeyKv>();
  const auto decisionFence = [](double slot, const std::string& suffix) {
    json::object fence;
    fence["slot"] = rt::makeNumber(slot);
    fence["account_id_hash"] = "hash-" + suffix;
    fence["quota_generation"] = "gen-" + suffix;
    fence["routing_generation"] = 7;
    fence["quota_reset_at_ms"] = rt::makeNumber(1'700'000'060'000.0);
    return fence;
  };
  const auto decision = [&decisionFence](const std::string& episode, const std::string& reason, const std::optional<std::string>& selectedAccount,
                                         const std::optional<std::string>& selectedCredit, double slot) {
    json::object value;
    value["v"] = 1;
    value["episode_hash"] = episode;
    value["created_at_ms"] = 200;
    value["expires_at_ms"] = rt::makeNumber(1'700'000'100'000.0);
    value["decision_reason"] = reason;
    value["selected_account_id_hash"] = selectedAccount ? json::value(*selectedAccount) : json::value(nullptr);
    value["selected_credit_id_hash"] = selectedCredit ? json::value(*selectedCredit) : json::value(nullptr);
    value["selected_credit_expires_at_ms"] = nullptr;
    json::array fences;
    fences.push_back(decisionFence(slot, episode));
    value["fences"] = std::move(fences);
    return value;
  };
  json::object corruptValue;
  corruptValue["v"] = 1;
  decisions->listEntries = {
    rt::KvEntry<>{.key = rt::KvKey{"uos_ai", "codex_reset_shadow_decision", "b"},
                  .value = rt::KvValue(decision("b", "selected", std::string("hash-b"), std::string("credit-b"), 1)),
                  .versionstamp = "1"},
    rt::KvEntry<>{.key = rt::KvKey{"uos_ai", "codex_reset_shadow_decision", "corrupt"}, .value = rt::KvValue(corruptValue), .versionstamp = "1"},
    rt::KvEntry<>{.key = rt::KvKey{"uos_ai", "codex_reset_shadow_decision", "a"},
                  .value = rt::KvValue(decision("a", "inventory_empty", std::nullopt, std::nullopt, 0)),
                  .versionstamp = "1"},
  };
  const std::optional<std::shared_ptr<rt::Kv>> decisionsKv = std::shared_ptr<rt::Kv>(decisions);
  AWAIT(listed, uos::codex::banked_reset_pool::listCodexResetShadowDecisions(decisionsKv));
  ASSERT(listed.has_value());
  std::vector<std::string> episodes;
  for (const uos::types::CodexResetShadowDecisionRecord& record : *listed) episodes.push_back(record.episode_hash);
  // usable records are sorted by creation time then episode hash, and unparsable rows are skipped
  EXPECT_EQ(episodes, (std::vector<std::string>{"a", "b"}));
}

/* ------------------------------------------------------- submission routing */

ASYNC_TEST("attemptCodexBankedReset reports every prerequisite it cannot satisfy") {
  IsolatedEnvironment isolation;
  const CodexBankedResetCandidate input = harness::candidate();
  auto clock = std::make_shared<TestClock>();
  auto provider = fakeProvider();
  auto kv = std::make_shared<MemoryKv>();
  AWAIT_DONE(seeded, harness::seedFences(kv, input));
  const CodexBankedResetDependencies deps = harness::dependencies(kv, provider, clock);

  CodexBankedResetCandidate noCredential = input;
  noCredential.credentialVersion = "";
  AWAIT(invalidGeneration, harness::attemptCodexBankedReset(noCredential, deps));
  EXPECT_EQ(invalidGeneration.reason, std::string("invalid_quota_generation"));
  CodexBankedResetDependencies nullKv = deps;
  nullKv.kv = std::shared_ptr<rt::Kv>();
  AWAIT(kvUnavailable, harness::attemptCodexBankedReset(input, nullKv));
  EXPECT_EQ(kvUnavailable.reason, std::string("kv_unavailable"));
  CodexBankedResetDependencies nanClock = deps;
  nanClock.now = [] { return rt::Number::NaN; };
  AWAIT(invalidClock, harness::attemptCodexBankedReset(input, nanClock));
  EXPECT_EQ(invalidClock.reason, std::string("invalid_clock"));
  CodexBankedResetDependencies emptyOwner = deps;
  emptyOwner.newOwnerToken = [] { return std::string(); };
  AWAIT(ownerUnavailable, harness::attemptCodexBankedReset(input, emptyOwner));
  EXPECT_EQ(ownerUnavailable.reason, std::string("owner_token_unavailable"));
  CodexBankedResetDependencies explodingConfig = deps;
  explodingConfig.reloadConfig = []() -> harness::CodexBankedResetConfig { throw rt::JsError("configuration unavailable"); };
  AWAIT(configurationUnavailable, harness::attemptCodexBankedReset(input, explodingConfig));
  EXPECT_EQ(configurationUnavailable.reason, std::string("configuration_unavailable"));

  AWAIT(context, contextOf(input));
  auto corrupt = std::make_shared<MemoryKv>();
  AWAIT_DONE(seededCorrupt, harness::seedFences(corrupt, input));
  json::object corruptValue;
  corruptValue["v"] = 1;
  corruptValue["state"] = "claimed";
  AWAIT_DONE(wroteCorrupt, corrupt->set(recordKeyOf(context), rt::KvValue(corruptValue)));
  AWAIT(corruptOutcome, harness::attemptCodexBankedReset(input, harness::dependencies(corrupt, provider, clock)));
  EXPECT_EQ(corruptOutcome.reason, std::string("redemption_record_invalid"));

  auto foreign = std::make_shared<MemoryKv>();
  AWAIT_DONE(seededForeign, harness::seedFences(foreign, input));
  CodexResetRedemptionRecord foreignRecord = createRecord(context, input);
  foreignRecord.idempotency_key_hash = "another-key-hash";
  AWAIT_DONE(wroteForeign, foreign->set(recordKeyOf(context), recordValue(foreignRecord)));
  AWAIT(mismatch, harness::attemptCodexBankedReset(input, harness::dependencies(foreign, provider, clock)));
  EXPECT_EQ(mismatch.reason, std::string("redemption_record_context_mismatch"));

  json::object noLookup;
  noLookup["byIdempotencyKey"] = false;
  noLookup["byProviderReceiptId"] = false;
  json::object noVerification;
  noVerification["independentlyVerifiable"] = false;
  json::object unprovenContract = harness::provenContract();
  unprovenContract["lookup"] = noLookup;
  unprovenContract["verification"] = noVerification;
  auto withoutProvider = fakeProvider(unprovenContract);
  CodexBankedResetCandidate unprovenAccount = input;
  unprovenAccount.accountId = "unproven-account";
  CodexBankedResetDependencies unprovenDeps = deps;
  unprovenDeps.provider = withoutProvider;
  AWAIT(unproven, harness::attemptCodexBankedReset(unprovenAccount, unprovenDeps));
  EXPECT_EQ(unproven.reason, std::string("provider_contract_unproven"));
  // an unproven provider contract is refused before any provider call
  EXPECT_EQ(withoutProvider->inventoryInputs.size(), std::size_t{0});
}

ASYNC_TEST("reconcileCodexBankedReset reports both client aborts and never submits") {
  IsolatedEnvironment isolation;
  const CodexBankedResetCandidate input = harness::candidate();
  auto clock = std::make_shared<TestClock>();
  auto provider = fakeProvider();
  auto kv = std::make_shared<MemoryKv>();
  AWAIT_DONE(seeded, harness::seedFences(kv, input));
  AWAIT(context, contextOf(input));
  AWAIT_DONE(wrote, kv->set(recordKeyOf(context), recordValue(createRecord(context, input, CodexResetRedemptionState::submitted, clock->nowMs))));

  rt::AbortController controller;
  controller.abort();
  CodexBankedResetCandidate abortedInput = input;
  abortedInput.signal = controller.signal();
  AWAIT(aborted, harness::reconcileCodexBankedReset(abortedInput, harness::dependencies(kv, provider, clock)));
  // an aborted client never starts a recovery lookup
  EXPECT_EQ(aborted.reason, std::string("client_aborted_before_reconciliation"));
  EXPECT_EQ(provider->lookupInputs.size(), std::size_t{0});

  AWAIT(noTransaction, harness::reconcileCodexBankedReset(harness::candidate({.accountId = "empty-account"}),
                                                          harness::dependencies(std::make_shared<MemoryKv>(), provider, clock)));
  EXPECT_EQ(noTransaction.reason, std::string("no_existing_transaction"));  // a reconcile-only attempt never creates a claim
}

namespace {
  rt::Task<std::shared_ptr<MemoryKv>> submittedKv(CodexBankedResetCandidate input, std::shared_ptr<TestClock> clock) {
    auto kv = std::make_shared<MemoryKv>();
    AWAIT_DONE(seeded, harness::seedFences(kv, input));
    AWAIT(context, contextOf(input));
    AWAIT_DONE(wrote, kv->set(recordKeyOf(context), recordValue(createRecord(context, input, CodexResetRedemptionState::submitted, clock->nowMs))));
    co_return kv;
  }
}

ASYNC_TEST("recovery lookups classify unavailable, malformed and receipt-less provider answers") {
  IsolatedEnvironment isolation;
  auto clock = std::make_shared<TestClock>();
  const CodexBankedResetCandidate contextInput = harness::candidate();

  auto unavailableProvider = fakeProvider();
  unavailableProvider->lookupFailure = rt::makeReason(rt::JsError("lookup unavailable"));
  AWAIT(unavailableKv, submittedKv(contextInput, clock));
  AWAIT(unavailable, harness::reconcileCodexBankedReset(contextInput, harness::dependencies(unavailableKv, unavailableProvider, clock)));
  EXPECT_EQ(unavailable.reason, std::string("lookup_unavailable"));

  auto malformedProvider = fakeProvider();
  malformedProvider->lookupResult = fixture("future_completed", "providerReceiptId", json::string("receipt"));
  AWAIT(malformedKv, submittedKv(contextInput, clock));
  AWAIT(malformed, harness::reconcileCodexBankedReset(contextInput, harness::dependencies(malformedKv, malformedProvider, clock)));
  EXPECT_EQ(malformed.reason, std::string("lookup_response_invalid"));

  auto receiptlessProvider = fakeProvider();
  receiptlessProvider->lookupResult = kindOnly("completed");
  AWAIT(receiptlessKv, submittedKv(contextInput, clock));
  AWAIT(receiptless, harness::reconcileCodexBankedReset(contextInput, harness::dependencies(receiptlessKv, receiptlessProvider, clock)));
  EXPECT_EQ(receiptless.reason, std::string("lookup_response_invalid"));  // a completed lookup without a receipt is unusable

  auto negativeProvider = fakeProvider();
  negativeProvider->lookupResult = fixture("rejected", "reason", json::string("not found"));
  negativeProvider->verifyResult = true;
  AWAIT(negativeKv, submittedKv(contextInput, clock));
  AWAIT(negative, harness::reconcileCodexBankedReset(contextInput, harness::dependencies(negativeKv, negativeProvider, clock)));
  EXPECT_EQ(negative.kind, std::string("verified"));  // a negative lookup alone is resolved only by independent verification

  // (TS: a non-boolean verifyApplied result reports "verification_response_invalid"; not representable.)

  auto unappliedProvider = fakeProvider();
  unappliedProvider->lookupResult = fixture("completed", "providerReceiptId", json::string("lookup-receipt"));
  unappliedProvider->verifyResult = false;
  AWAIT(unappliedKv, submittedKv(contextInput, clock));
  AWAIT(unapplied, harness::reconcileCodexBankedReset(contextInput, harness::dependencies(unappliedKv, unappliedProvider, clock)));
  EXPECT_EQ(unapplied.reason, std::string("verification_not_applied"));

  auto failedVerifyProvider = fakeProvider();
  failedVerifyProvider->lookupResult = fixture("completed", "providerReceiptId", json::string("lookup-receipt"));
  failedVerifyProvider->verifyFailure = rt::makeReason(rt::JsError("verification unavailable"));
  AWAIT(failedVerifyKv, submittedKv(contextInput, clock));
  AWAIT(failedVerify, harness::reconcileCodexBankedReset(contextInput, harness::dependencies(failedVerifyKv, failedVerifyProvider, clock)));
  EXPECT_EQ(failedVerify.reason, std::string("verification_unavailable"));
}

ASYNC_TEST("a recovery that cannot read its clock reports invalid_clock instead of guessing a state") {
  IsolatedEnvironment isolation;
  auto clock = std::make_shared<TestClock>();
  const CodexBankedResetCandidate input = harness::candidate();
  auto provider = fakeProvider();
  provider->lookupResult = fixture("completed", "providerReceiptId", json::string("lookup-receipt"));
  provider->verifyResult = true;
  auto kv = std::make_shared<MemoryKv>();
  AWAIT_DONE(seeded, harness::seedFences(kv, input));
  AWAIT(context, contextOf(input));
  AWAIT_DONE(wrote, kv->set(recordKeyOf(context), recordValue(createRecord(context, input, CodexResetRedemptionState::submitted, clock->nowMs))));
  const CodexBankedResetDependencies base = harness::dependencies(kv, provider, clock);

  struct Observed {
    double validReads;
    std::string kind;
    std::string reason;
  };
  std::vector<Observed> outcomes;
  const rt::KvKey recordKey = recordKeyOf(context);
  for (double validReads = 0; validReads <= 6; validReads += 1) {
    resetRecordTo(context, *kv, input, clock->nowMs);
    std::vector<std::optional<double>> readings(static_cast<std::size_t>(validReads), clock->nowMs);
    readings.push_back(rt::Number::NaN);
    CodexBankedResetDependencies scripted = base;
    scripted.now = scriptedClock(std::move(readings));
    AWAIT(outcome, harness::reconcileCodexBankedReset(input, scripted));
    if (outcome.reason == "invalid_clock") {
      // an unreadable clock never settles the record
      EXPECT(outcome.kind == "pending" || outcome.kind == "skipped");
      // an unreadable clock never verifies the record
      EXPECT(stateOf(kv->redemptionRecord(recordKey)) != std::optional<std::string>("verified"));
    }
    outcomes.push_back(Observed{.validReads = validReads, .kind = outcome.kind, .reason = outcome.reason});
  }
  EXPECT(std::any_of(outcomes.begin(), outcomes.end(), [](const Observed& observed) { return observed.reason == "invalid_clock"; }));
  // the same fixture settles when every reading is usable
  EXPECT_EQ(outcomes.back().reason, std::string("verified"));
}

ASYNC_TEST("a submission whose provider throws synchronously or rejects leaves a durable unknown record") {
  IsolatedEnvironment isolation;
  auto clock = std::make_shared<TestClock>();
  const CodexBankedResetCandidate input = harness::candidate();
  auto kv = std::make_shared<MemoryKv>();
  AWAIT_DONE(seeded, harness::seedFences(kv, input));
  auto synchronousProvider = fakeProvider();
  synchronousProvider->redeemReplacement = [](harness::FakeCodexUsageResetProvider::RedeemResetInput, rt::AbortSignal) -> rt::Task<harness::RedeemResetResult> {
    throw rt::JsError("provider threw synchronously");
  };

  AWAIT(outcome, harness::attemptCodexBankedReset(input, harness::dependencies(kv, synchronousProvider, clock)));
  EXPECT_EQ(outcome.kind, std::string("pending"));
  EXPECT_EQ(outcome.reason, std::string("submit_transport_unknown"));
  AWAIT(context, contextOf(input));
  const std::optional<json::value> record = kv->redemptionRecord(recordKeyOf(context));
  // a synchronous transport failure is durable, never a silent success
  EXPECT_EQ(stateOf(record), std::optional<std::string>("unknown"));
  ASSERT(record.has_value());
  EXPECT(rt::getString(*record, "last_error_code") == std::optional<std::string_view>("submit_transport_unknown"));

  auto rejectingKv = std::make_shared<MemoryKv>();
  AWAIT_DONE(seededRejecting, harness::seedFences(rejectingKv, input));
  auto rejectingProvider = fakeProvider();
  rejectingProvider->redeemFailure = rt::makeReason(rt::JsError("provider rejected"));
  AWAIT(rejected, harness::attemptCodexBankedReset(input, harness::dependencies(rejectingKv, rejectingProvider, clock)));
  EXPECT_EQ(rejected.kind, std::string("pending"));
  EXPECT_EQ(rejected.reason, std::string("submit_transport_unknown"));

  auto receiptlessKv = std::make_shared<MemoryKv>();
  AWAIT_DONE(seededReceiptless, harness::seedFences(receiptlessKv, input));
  auto receiptlessProvider = fakeProvider();
  receiptlessProvider->redeemResult = fixture("unknown", "providerReceiptId", nullptr);
  AWAIT(receiptless, harness::attemptCodexBankedReset(input, harness::dependencies(receiptlessKv, receiptlessProvider, clock)));
  EXPECT_EQ(receiptless.kind, std::string("pending"));
  // an unknown outcome is never treated as a verified reset
  EXPECT_EQ(receiptless.reason, std::string("provider_commit_unknown"));
}

ASYNC_TEST("a client aborted mid-submission is rejected before and after the final renewal") {
  IsolatedEnvironment isolation;
  const CodexBankedResetCandidate input = harness::candidate();
  auto controller = std::make_shared<rt::AbortController>();
  CodexBankedResetCandidate abortedInput = input;
  abortedInput.signal = controller->signal();
  auto clock = std::make_shared<TestClock>();
  auto provider = fakeProvider();
  auto kv = std::make_shared<MemoryKv>();
  AWAIT_DONE(seeded, harness::seedFences(kv, abortedInput));
  auto entered = std::make_shared<harness::Deferred<void>>();
  provider->inventoryEntered = entered;
  provider->inventoryGate = entered->promise.then([controller] { controller->abort(); });

  AWAIT(beforeRenewal, harness::attemptCodexBankedReset(abortedInput, harness::dependencies(kv, provider, clock)));
  // an abort observed before the durable submission rejects the record
  EXPECT_EQ(beforeRenewal.reason, std::string("client_aborted_before_submission"));
  EXPECT_EQ(provider->redeemInputs.size(), std::size_t{0});  // no provider redemption is attempted for an aborted client

  auto afterSubmission = std::make_shared<rt::AbortController>();
  auto afterSubmissionKv = std::make_shared<MemoryKv>();
  const CodexBankedResetCandidate afterSubmissionInput = harness::candidate({.accountId = "test-account-c"});
  AWAIT_DONE(seededAfterSubmission, harness::seedFences(afterSubmissionKv, afterSubmissionInput));
  afterSubmissionKv->beforeAtomicCommit = [afterSubmission](double commitNumber) {
    if (commitNumber == 2) afterSubmission->abort();
  };
  CodexBankedResetCandidate abortedAfterSubmitInput = afterSubmissionInput;
  abortedAfterSubmitInput.signal = afterSubmission->signal();
  AWAIT(abortedAfterSubmit, harness::attemptCodexBankedReset(abortedAfterSubmitInput, harness::dependencies(afterSubmissionKv, fakeProvider(), clock)));
  // an abort seen after the durable submission rejects the record instead of spending
  EXPECT_EQ(abortedAfterSubmit.reason, std::string("client_aborted_after_submission"));
  // the durable submission boundary was crossed before the abort was observed
  EXPECT(afterSubmissionKv->atomicCommitCount >= 2);

  auto afterRenewal = std::make_shared<rt::AbortController>();
  auto afterRenewalKv = std::make_shared<MemoryKv>();
  const CodexBankedResetCandidate afterRenewalInput = harness::candidate({.accountId = "test-account-d"});
  AWAIT_DONE(seededAfterRenewal, harness::seedFences(afterRenewalKv, afterRenewalInput));
  afterRenewalKv->beforeAtomicCommit = [afterRenewal](double commitNumber) {
    if (commitNumber == 3) afterRenewal->abort();
  };
  auto spendingProvider = fakeProvider();
  CodexBankedResetCandidate abortedAfterRenewalInput = afterRenewalInput;
  abortedAfterRenewalInput.signal = afterRenewal->signal();
  AWAIT(abortedAfterRenewal, harness::attemptCodexBankedReset(abortedAfterRenewalInput, harness::dependencies(afterRenewalKv, spendingProvider, clock)));
  // an abort seen after the final renewal stops before the provider call
  EXPECT_EQ(abortedAfterRenewal.reason, std::string("client_aborted_after_submission"));
  EXPECT_EQ(spendingProvider->redeemInputs.size(), std::size_t{0});
}

ASYNC_TEST("a settled verified or rejected record is reported without a new claim") {
  IsolatedEnvironment isolation;
  auto clock = std::make_shared<TestClock>();
  const CodexBankedResetCandidate input = harness::candidate();
  auto provider = fakeProvider();
  AWAIT(context, contextOf(input));

  auto verifiedKv = std::make_shared<MemoryKv>();
  AWAIT_DONE(seededVerified, harness::seedFences(verifiedKv, input));
  CodexResetRedemptionRecord verifiedRecord = createRecord(context, input, CodexResetRedemptionState::verified, clock->nowMs);
  verifiedRecord.verified_at_ms = clock->nowMs;
  AWAIT_DONE(wroteVerified, verifiedKv->set(recordKeyOf(context), recordValue(verifiedRecord)));
  AWAIT(verified, harness::attemptCodexBankedReset(input, harness::dependencies(verifiedKv, provider, clock)));
  EXPECT_EQ(verified.kind, std::string("verified"));
  EXPECT_EQ(verified.reason, std::string("previously_verified"));
  EXPECT_EQ(provider->callCount(), 0.0);  // a settled outcome never re-enters the provider

  auto rejectedKv = std::make_shared<MemoryKv>();
  AWAIT_DONE(seededRejected, harness::seedFences(rejectedKv, input));
  CodexResetRedemptionRecord rejectedRecord = createRecord(context, input, CodexResetRedemptionState::rejected, clock->nowMs);
  rejectedRecord.last_error_code = "provider_rejected";
  AWAIT_DONE(wroteRejected, rejectedKv->set(recordKeyOf(context), recordValue(rejectedRecord)));
  AWAIT(rejected, harness::attemptCodexBankedReset(input, harness::dependencies(rejectedKv, provider, clock)));
  EXPECT_EQ(rejected.kind, std::string("rejected"));
  EXPECT_EQ(rejected.reason, std::string("provider_rejected"));

  auto staleKv = std::make_shared<MemoryKv>();
  AWAIT_DONE(seededStale, harness::seedFences(staleKv, input));
  AWAIT_DONE(wroteStale, staleKv->set(recordKeyOf(context), recordValue(verifiedRecord)));
  CodexBankedResetCandidate nextGeneration = input;
  nextGeneration.routingGeneration = input.routingGeneration + 1;
  AWAIT(stale, harness::attemptCodexBankedReset(nextGeneration, harness::dependencies(staleKv, provider, clock)));
  // an older verified reset is never a recovery candidate for a newer circuit
  EXPECT_EQ(stale.reason, std::string("verified_routing_generation_stale"));
}

/* ------------------------------------------------------------- pool routing */

ASYNC_TEST("evaluateCodexBankedResetPool reports malformed pools, an unreadable clock and an unavailable KV") {
  IsolatedEnvironment isolation;
  auto clock = std::make_shared<TestClock>();
  auto provider = fakeProvider();
  const CodexBankedResetCandidate input = harness::candidate();
  auto kv = std::make_shared<MemoryKv>();
  AWAIT_DONE(seeded, harness::seedFences(kv, input));
  const CodexBankedResetDependencies deps = harness::dependencies(kv, provider, clock, harness::config({.mode = "shadow"}));

  AWAIT(missing, harness::evaluateCodexBankedResetPool({}, deps));
  EXPECT_EQ(missing.reason, std::string("full_pool_missing"));
  const CodexBankedResetCandidate second = harness::candidate({.accountId = "test-account-b"});
  AWAIT_DONE(seededSecond, harness::seedFences(kv, second));
  const auto poolWithSlots = [&](double firstSlot, double secondSlot) {
    return std::vector<CodexBankedResetPoolCandidate>{
      CodexBankedResetPoolCandidate{.slot = firstSlot, .candidate = input, .provider = provider},
      CodexBankedResetPoolCandidate{.slot = secondSlot, .candidate = second, .provider = provider},
    };
  };
  AWAIT(sharedSlot, harness::evaluateCodexBankedResetPool(poolWithSlots(0, 0), deps));
  EXPECT_EQ(sharedSlot.reason, std::string("full_pool_invalid"));  // two candidates may not share a slot
  AWAIT(fractionalSlot, harness::evaluateCodexBankedResetPool(poolWithSlots(0.5, 1), deps));
  EXPECT_EQ(fractionalSlot.reason, std::string("full_pool_invalid"));  // a fractional slot is not a pool slot
  CodexBankedResetDependencies nanClock = deps;
  nanClock.now = [] { return rt::Number::NaN; };
  AWAIT(invalidClock, harness::evaluateCodexBankedResetPool(poolWithSlots(0, 1), nanClock));
  EXPECT_EQ(invalidClock.reason, std::string("invalid_clock"));
  CodexBankedResetDependencies nullKv = deps;
  nullKv.kv = std::shared_ptr<rt::Kv>();
  AWAIT(kvUnavailable, harness::evaluateCodexBankedResetPool(harness::fullPool(input, provider, harness::candidate({.accountId = "test-account-b"}), provider),
                                                             nullKv));
  EXPECT_EQ(kvUnavailable.reason, std::string("kv_unavailable"));
  CodexBankedResetCandidate noCredential = input;
  noCredential.credentialVersion = "";
  AWAIT(invalidGeneration,
        harness::evaluateCodexBankedResetPool(harness::fullPool(noCredential, provider, harness::candidate({.accountId = "test-account-b"}), provider), deps));
  EXPECT_EQ(invalidGeneration.reason, std::string("invalid_quota_generation"));
  CodexBankedResetCandidate recovered = input;
  recovered.quotaResetAtMs = clock->nowMs - 1;
  AWAIT(windowExpired,
        harness::evaluateCodexBankedResetPool(harness::fullPool(recovered, provider, harness::candidate({.accountId = "test-account-b"}), provider), deps));
  EXPECT_EQ(windowExpired.reason, std::string("quota_window_expired"));
  CodexBankedResetDependencies explodingConfig = deps;
  explodingConfig.reloadConfig = []() -> harness::CodexBankedResetConfig { throw rt::JsError("config unavailable"); };
  AWAIT(configurationUnavailable,
        harness::evaluateCodexBankedResetPool(harness::fullPool(input, provider, harness::candidate({.accountId = "test-account-b"}), provider), explodingConfig));
  EXPECT_EQ(configurationUnavailable.reason, std::string("configuration_unavailable"));
}

ASYNC_TEST("a live pool refuses a per-account day limit above one when an injected provider treats redeem outcomes as final") {
  IsolatedEnvironment isolation;
  auto clock = std::make_shared<TestClock>();
  const CodexBankedResetCandidate input = harness::candidate();
  auto kv = std::make_shared<MemoryKv>();
  AWAIT_DONE(seeded, harness::seedFences(kv, input));
  auto terminalProvider = fakeProvider(contractWith("redeemOutcomeIsFinal", true));
  const CodexBankedResetCandidate second = harness::candidate({.accountId = "test-account-b"});
  AWAIT_DONE(seededSecond, harness::seedFences(kv, second));

  AWAIT(outcome, harness::evaluateCodexBankedResetPool(harness::fullPool(input, terminalProvider, second, terminalProvider),
                                                       harness::dependencies(kv, terminalProvider, clock,
                                                                             harness::config({.mode = "live", .maxPerAccountPerDay = 2}))));
  EXPECT_EQ(outcome.reason, std::string("terminal_outcome_account_day_limit_must_be_one"));

  AWAIT(atMostOnce, harness::evaluateCodexBankedResetPool(harness::fullPool(input, terminalProvider, second, terminalProvider),
                                                          harness::dependencies(kv, terminalProvider, clock,
                                                                                harness::config({.mode = "live", .maxPerAccountPerDay = 1}))));
  EXPECT(atMostOnce.reason != "terminal_outcome_account_day_limit_must_be_one");
}

ASYNC_TEST("a shadow pool refuses an unreadable usage record and reports its read-only decision") {
  IsolatedEnvironment isolation;
  auto clock = std::make_shared<TestClock>();
  const CodexBankedResetCandidate input = harness::candidate();
  const CodexBankedResetCandidate second = harness::candidate({.accountId = "test-account-b"});
  auto provider = fakeProvider();
  auto kv = std::make_shared<FailKeyKv>();
  AWAIT_DONE(seededFirst, harness::seedFences(asMemory(kv), input));
  AWAIT_DONE(seededSecond, harness::seedFences(asMemory(kv), second));
  const CodexBankedResetDependencies deps = harness::dependencies(kv, provider, clock, harness::config({.mode = "shadow"}));

  provider->inventoryFailure = rt::makeReason(rt::JsError("inventory unavailable"));
  AWAIT(inventoryUnavailable, harness::evaluateCodexBankedResetPool(harness::fullPool(input, provider, second, provider), deps));
  EXPECT_EQ(inventoryUnavailable.reason, std::string("inventory_unavailable"));  // a failed inventory read stops the episode
  provider->inventoryFailure = nullptr;

  AWAIT(context, contextOf(input));
  auto unreadableUsage = std::make_shared<FailKeyKv>();
  AWAIT_DONE(seededUnreadableFirst, harness::seedFences(asMemory(unreadableUsage), input));
  AWAIT_DONE(seededUnreadableSecond, harness::seedFences(asMemory(unreadableUsage), second));
  unreadableUsage->failGetKeys.push_back(harness::encodeKey(harness::codexResetUsageKey(context.account.accountIdHash)));
  AWAIT(configurationUnavailable, harness::evaluateCodexBankedResetPool(harness::fullPool(input, provider, second, provider),
                                                                        harness::dependencies(unreadableUsage, provider, clock,
                                                                                              harness::config({.mode = "shadow"}))));
  // an unreadable reset-settings record fails the whole episode closed
  EXPECT_EQ(configurationUnavailable.reason, std::string("configuration_unavailable"));

  provider->inventory = harness::inventory("credit-1", clock->nowMs + 60'000);
  const CodexBankedResetDependencies configuredGloballyDisallowed =
    harness::dependencies(kv, provider, clock, harness::config({.enabled = false, .mode = "shadow"}));
  AWAIT(featureDisabled, harness::evaluateCodexBankedResetPool(harness::fullPool(input, provider, second, provider), configuredGloballyDisallowed));
  EXPECT_EQ(featureDisabled.reason, std::string("feature_disabled"));

  AWAIT(shadow, harness::evaluateCodexBankedResetPool(harness::fullPool(input, provider, second, provider), deps));
  EXPECT_EQ(shadow.kind, std::string("shadow"));
  EXPECT_EQ(shadow.reason, std::string("shadow_selected"));
  EXPECT(shadow.selected.has_value());  // a selected shadow decision names the candidate it would spend
}
