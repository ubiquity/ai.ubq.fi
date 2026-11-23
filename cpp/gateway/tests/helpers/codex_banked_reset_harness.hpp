#pragma once
// Port of tests/helpers/codex-banked-reset-harness.ts (shared harness of the banked-reset suites).
//
// Held by std::shared_ptr: the KV fake and the provider fake are handed to module code that takes
// std::shared_ptr<rt::Kv> / std::shared_ptr<CodexUsageResetProvider>, and dependencies() captures the
// TestClock by shared_ptr (the TS closes over the object).
//
// Timing: like the TS, MemoryKv::get is an async function (its result settles when the body
// returns; `beforeGet` may hand back a promise it awaits first), set and atomic().commit() do their
// work when called and settle at once. The provider methods follow the TS fake: readInventory, redeem
// and verifyApplied are async, lookup settles at once, and a verifyFailure is a rejected promise
// returned from the async function (two turns later).
//
// Provider boundary: redemption results are typed, while verification retains raw JSON. The fake
// keeps the TS fixtures as the plain objects they are (`redeemResult`, `lookupResult`, `verifyResult`
// are json::value) and converts redemption results at the boundary. A malformed result (unknown kind, missing or
// non-string receipt, non-string reason) becomes a terminal result with an empty receipt, which the
// module rejects exactly where it rejects the original (`validRedeemResult` is the first check of every
// path that reads one). Verification retains the raw JSON fixture so strict true, false and
// non-boolean results reach the submission guard without losing their distinct outcomes.
#include "uos/codex/banked_reset.hpp"
#include "uos/codex/banked_reset_pool.hpp"
#include "uos/codex/banked_reset_provider.hpp"
#include "uos/codex/banked_reset_submission.hpp"
#include "uos/codex/reset_settings.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::tests::helpers::codex_banked_reset_harness {
  // The TS re-exports.
  using ::uos::codex::banked_reset::CODEX_BANKED_RESET_INVENTORY_MAX_AGE_MS;
  using ::uos::codex::banked_reset::CODEX_BANKED_RESET_INVENTORY_TIMEOUT_MS;
  using ::uos::codex::banked_reset::CODEX_BANKED_RESET_LEASE_MS;
  using ::uos::codex::banked_reset::CodexBankedResetConfig;
  using ::uos::codex::banked_reset::CodexBankedResetTelemetryFields;
  using ::uos::codex::banked_reset::codexResetAccountDailyKey;
  /** Retained legacy key: the submission budget never reads or enforces it after the per-account cutover. */
  using ::uos::codex::banked_reset::codexResetGlobalDailyKey;
  using ::uos::codex::banked_reset::codexResetRedemptionKey;
  using ::uos::codex::banked_reset::loadCodexBankedResetConfig;
  using ::uos::codex::banked_reset::parseCodexResetRedemptionRecord;
  using ::uos::codex::banked_reset::parseCodexResetShadowDecisionRecord;
  using ::uos::codex::banked_reset_pool::evaluateCodexBankedResetPool;
  using ::uos::codex::banked_reset_provider::CodexUsageResetProviderContract;
  using ::uos::codex::banked_reset_provider::providerSupportsLiveRedemption;
  using ::uos::codex::banked_reset_provider::providerSupportsResetType;
  using ::uos::codex::banked_reset_provider::RedeemResetResult;
  using ::uos::codex::banked_reset_provider::ResetInventory;
  using ::uos::codex::banked_reset_provider::unavailableCodexUsageResetProvider;
  using ::uos::codex::banked_reset_submission::attemptCodexBankedReset;
  using ::uos::codex::banked_reset_submission::reconcileCodexBankedReset;
  using ::uos::codex::reset_settings::codexResetUsageKey;
  using ::uos::types::CodexResetRedemptionRecord;

  // `clone(value)`: structuredClone.
  rt::KvValue clone(const rt::KvValue& value);
  // `encodeKey(key)`: JSON.stringify(key). A bigint part throws TypeError as JSON.stringify does.
  std::string encodeKey(const rt::KvKey& key);

  // `StoredEntry`
  struct StoredEntry {
    rt::KvKey key;
    rt::KvValue value;
    std::string versionstamp;
  };

  /** A small KV fake with versionstamps and deliberately injectable CAS failures. */
  class MemoryKv : public rt::Kv {
  public:
    rt::OrderedMap<std::string, StoredEntry> entries;
    rt::OrderedSet<double> failCommitNumbers;
    std::vector<std::vector<rt::KvKey>> atomicCheckBatches;
    double atomicCommitCount = 0;
    // `((commitNumber) => void) | null`: empty is null.
    std::function<void(double commitNumber)> beforeAtomicCommit;
    // `((key, getNumber) => Promise<void> | null | undefined) | null`: empty is null; the hook may throw.
    std::function<std::optional<rt::Promise<void>>(rt::KvKey key, double getNumber)> beforeGet;
    // `Error | null`: a null Reason is null.
    rt::Reason getFailure;
    double getCount = 0;

    // `versionstamp(key): string | null`
    std::optional<std::string> versionstamp(const rt::KvKey& key) const;

    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override;
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override;
    // `redemptionRecord(key)`: the stored value as JSON (an unchecked cast in the TS), null when absent.
    std::optional<json::value> redemptionRecord(const rt::KvKey& key) const;
    std::unique_ptr<rt::AtomicOperation> atomic() override;

    // Not part of the TS fake (calling them there throws "is not a function"); they throw TypeError.
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey> keys, rt::KvGetOptions options = {}) override;
    rt::Task<void> delete_(rt::KvKey key) override;
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override;
    void close() override {}

    // `#put` and `#nextVersionstamp` (for subclasses, as the TS private methods are for its own atomic()).
    std::string nextVersionstamp();
    void put(const rt::KvKey& key, const rt::KvValue& value, const std::string& versionstamp);

  private:
    double version_ = 0;
  };

  // `class Deferred<T>`: a promise and its resolver.
  template <class T = void> class Deferred {
  public:
    Deferred() {
      rt::Resolvers<T> resolvers = rt::withResolvers<T>();
      promise = resolvers.promise;
      resolve_ = std::make_shared<rt::Resolve<T>>(resolvers.resolve);
    }
    rt::Promise<T> promise;
    template <class... A> void resolve(A&&... value) const { (*resolve_)(std::forward<A>(value)...); }

  private:
    std::shared_ptr<rt::Resolve<T>> resolve_;
  };

  // `provenContract()`, as the plain object the TS fake carries.
  json::object provenContract();

  // `FakeProviderCall`
  struct FakeProviderCall {
    std::string method;  // "readInventory" | "redeem" | "lookup" | "verifyApplied"
    std::string accountId;
    std::string quotaGeneration;
    std::optional<std::string> idempotencyKey;
    double commitCount;
    std::optional<std::string> providerReceiptId;
    std::optional<std::string> timeoutStage;  // "before_provider_commit" | "after_provider_commit" | null
  };

  /**
   * Sanitized boundary fixtures, not inferred provider wire payloads. There is
   * no approved live adapter yet, so these cover every result shape accepted by
   * the injectable provider contract while keeping tests completely offline.
   */
  struct SanitizedProviderFixtures {
    ResetInventory inventory_available;
    ResetInventory inventory_empty;
    json::value redemption_completed;
    json::value redemption_accepted;
    json::value redemption_already_redeemed;
    json::value redemption_rejected;
    json::value authentication_error;
    json::value rate_limit;
    json::value server_error;
    json::value malformed_success;
    json::value schema_drift;
    json::value lookup_pending;
    json::value lookup_completed;
    json::value lookup_rejected;
    json::value lookup_not_found;
  };
  const SanitizedProviderFixtures& sanitizedProviderFixtures();

  // A provider result fixture (a plain object, possibly malformed) at the typed provider boundary.
  RedeemResetResult toRedeemResetResult(const json::value& result);

  class FakeCodexUsageResetProvider : public codex::banked_reset_provider::CodexUsageResetProvider {
  public:
    using ResetAccountContext = codex::banked_reset_provider::ResetAccountContext;
    using RedeemResetInput = codex::banked_reset_provider::RedeemResetInput;
    using LookupRedeemResetInput = codex::banked_reset_provider::LookupRedeemResetInput;

    // `contract = provenContract()`
    FakeCodexUsageResetProvider();
    explicit FakeCodexUsageResetProvider(json::value contract_);

    std::vector<FakeProviderCall> calls;
    std::vector<ResetAccountContext> inventoryInputs;
    std::vector<RedeemResetInput> redeemInputs;
    std::vector<LookupRedeemResetInput> lookupInputs;
    std::vector<ResetAccountContext> verificationInputs;
    ResetInventory inventory;
    json::value redeemResult;
    json::value lookupResult;
    json::value verifyResult = true;
    rt::Reason inventoryFailure;
    rt::Reason redeemFailure;
    /** Simulates a response loss after the provider has already committed. */
    rt::Reason redeemFailureAfterCommit;
    rt::Reason lookupFailure;
    rt::Reason verifyFailure;
    std::optional<rt::Promise<void>> inventoryGate;
    std::shared_ptr<Deferred<void>> inventoryEntered;
    std::optional<rt::Promise<void>> redeemGate;
    std::shared_ptr<Deferred<void>> redeemEntered;
    std::optional<rt::Promise<void>> verifyGate;
    std::shared_ptr<Deferred<void>> verifyEntered;
    std::optional<bool> commitOnRedeem;
    double commitCount = 0;

    // `provider.readInventory = ...` and the like: a non-empty replacement runs instead of the fake's
    // own method (synchronously, so a replacement that throws throws at the call).
    std::function<rt::Task<ResetInventory>(ResetAccountContext input, rt::AbortSignal signal)> readInventoryReplacement;
    std::function<rt::Task<RedeemResetResult>(RedeemResetInput input, rt::AbortSignal signal)> redeemReplacement;
    std::function<rt::Task<RedeemResetResult>(LookupRedeemResetInput input, rt::AbortSignal signal)> lookupReplacement;
    std::function<rt::Task<json::value>(ResetAccountContext input, rt::AbortSignal signal)> verifyAppliedReplacement;

    double callCount() const;

    rt::Task<ResetInventory> readInventory(ResetAccountContext input, rt::AbortSignal signal) override;
    rt::Task<RedeemResetResult> redeem(RedeemResetInput input, rt::AbortSignal signal) override;
    rt::Task<RedeemResetResult> lookup(LookupRedeemResetInput input, rt::AbortSignal signal) override;
    rt::Task<json::value> verifyApplied(ResetAccountContext input, rt::AbortSignal signal) override;

  private:
    void record(const std::string& method, const ResetAccountContext& input, std::optional<std::string> idempotencyKey,
                std::optional<std::string> providerReceiptId = std::nullopt, std::optional<std::string> timeoutStage = std::nullopt);
    rt::Task<ResetInventory> fakeReadInventory(ResetAccountContext input);
    rt::Task<RedeemResetResult> fakeRedeem(RedeemResetInput input);
    rt::Task<RedeemResetResult> fakeLookup(LookupRedeemResetInput input);
    rt::Task<json::value> fakeVerifyApplied(ResetAccountContext input);
  };

  class TestClock {
  public:
    double nowMs;
    explicit TestClock(double nowMs_ = 1'700'000'000'000) : nowMs(nowMs_) {}
    void advance(double ms) { nowMs += ms; }
  };

  // `testHash(value)`: FNV-1a over the UTF-16 code units, `test-hash-<hex>-<length>`; settles at once.
  std::string testHashText(const std::string& value);
  rt::Task<std::string> testHash(std::string value);

  // The harness's own `isRecord` (a non-null, non-array object).
  bool isRecord(const json::value& value);

  rt::KvKey routingFenceKey(const std::string& accountId);
  rt::KvKey credentialFenceKey(const std::string& accountId);

  std::vector<codex::banked_reset::CodexBankedResetFence> bankedResetFences(const std::string& accountId, double routingGeneration,
                                                                            const std::string& credentialVersion);

  // `Partial<CodexBankedResetCandidate>` as candidate() reads it.
  struct CandidateOverrides {
    std::optional<std::string> accountId{};
    std::optional<std::string> credentialVersion{};
    std::optional<double> quotaResetAtMs{};
    std::optional<double> routingGeneration{};
    std::optional<std::vector<codex::banked_reset::CodexBankedResetFence>> fences{};
    std::optional<std::string> requestId{};
    std::optional<rt::AbortSignal> signal{};
  };
  codex::banked_reset::CodexBankedResetCandidate candidate();
  codex::banked_reset::CodexBankedResetCandidate candidate(const CandidateOverrides& overrides);

  [[nodiscard]] rt::Task<void> seedFences(std::shared_ptr<MemoryKv> kv, codex::banked_reset::CodexBankedResetCandidate input);

  bool hasKey(const std::vector<rt::KvKey>& keys, const rt::KvKey& expected);

  // `Partial<CodexBankedResetConfig>`
  struct ConfigOverrides {
    std::optional<bool> enabled{};
    std::optional<std::string> mode{};
    std::optional<double> maxPerAccountPerDay{};
    std::optional<double> maxPerAccountPerWindow{};
  };
  CodexBankedResetConfig config();
  CodexBankedResetConfig config(const ConfigOverrides& overrides);

  // `dependencies(kv, provider, clock, configured = config(), telemetry = {}, reloadConfig?)`; an empty
  // reloadConfig is the absent one.
  codex::banked_reset::CodexBankedResetDependencies dependencies(std::shared_ptr<MemoryKv> kv,
                                                                 std::shared_ptr<codex::banked_reset_provider::CodexUsageResetProvider> provider,
                                                                 std::shared_ptr<TestClock> clock);
  codex::banked_reset::CodexBankedResetDependencies dependencies(std::shared_ptr<MemoryKv> kv,
                                                                 std::shared_ptr<codex::banked_reset_provider::CodexUsageResetProvider> provider,
                                                                 std::shared_ptr<TestClock> clock, CodexBankedResetConfig configured,
                                                                 codex::banked_reset::CodexBankedResetTelemetry telemetry = {},
                                                                 std::function<CodexBankedResetConfig()> reloadConfig = {});

  std::vector<codex::banked_reset::CodexBankedResetPoolCandidate> fullPool(
    const codex::banked_reset::CodexBankedResetCandidate& first, std::shared_ptr<codex::banked_reset_provider::CodexUsageResetProvider> firstProvider,
    const codex::banked_reset::CodexBankedResetCandidate& second, std::shared_ptr<codex::banked_reset_provider::CodexUsageResetProvider> secondProvider);

  // `inventory(id, expiresAtMs)`: expiresAtMs null is std::nullopt.
  ResetInventory inventory(const std::string& id, std::optional<double> expiresAtMs);

  /** The account hash of a reset result the test has already asserted is present. */
  std::string requiredHash(const std::optional<std::string>& value);

  // `shadowDecisionFrom(kv)`: the one parseable shadow decision among the stored values (asserted).
  ::uos::types::CodexResetShadowDecisionRecord shadowDecisionFrom(const MemoryKv& kv);

  // Helpers that lived between tests in the original file.

  enum class BankedResetGeneratedEvent {
    request,
    qualifying_429,
    non_qualifying_429,
    claim,
    submit,
    provider_commit,
    response_loss,
    lookup,
    verify,
    retry,
    crash,
    lease_expire,
    credential_rotate,
    kv_failure,
  };
  std::string_view toString(BankedResetGeneratedEvent event);

  extern const std::vector<BankedResetGeneratedEvent> BANKED_RESET_GENERATED_EVENTS;

  struct GeneratedResetScenario {
    codex::banked_reset::CodexBankedResetCandidate reset;
    codex::banked_reset::CodexBankedResetDependencies deps;
    std::shared_ptr<FakeCodexUsageResetProvider> provider;
    std::shared_ptr<MemoryKv> kv;
    std::shared_ptr<TestClock> clock;
    double sequenceSeed;
    bool qualifyingObservationIsCurrent;
    std::optional<codex::banked_reset::CodexBankedResetOutcome> lastOutcome;
    double postResetInferenceRetries;
    bool credentialWasRotated;
  };

  // The scenario is shared with the caller, which reads it after each event.
  [[nodiscard]] rt::Task<void> applyBankedResetEvent(std::shared_ptr<GeneratedResetScenario> scenario, BankedResetGeneratedEvent event, std::string label);

  // `mode: CodexBankedResetConfig["mode"]`
  void assertBankedResetInvariants(const GeneratedResetScenario& scenario, const std::string& label, double submissionsBefore, const std::string& mode);
}
