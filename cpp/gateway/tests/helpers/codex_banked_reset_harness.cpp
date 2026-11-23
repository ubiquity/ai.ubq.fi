// Port of tests/helpers/codex-banked-reset-harness.ts.
#include "tests/helpers/codex_banked_reset_harness.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <set>
#include <utility>

namespace uos::tests::helpers::codex_banked_reset_harness {
  namespace {
    constexpr rt::StoredJsonViewOptions STORED_JSON_VIEW_OPTIONS{
      .undefinedMember = rt::StoredJsonViewOptions::UndefinedView::omit,
      .undefinedElement = rt::StoredJsonViewOptions::UndefinedView::null,
      .hole = rt::StoredJsonViewOptions::UndefinedView::null,
      .topLevelUndefined = rt::StoredJsonViewOptions::UndefinedView::null,
      .bigint = rt::StoredJsonViewOptions::BigIntView::emptyObject,
      .kvU64Members = false,
      .typedArrayMembers = false,
    };
    using codex::banked_reset::CodexBankedResetCandidate;
    using codex::banked_reset::CodexBankedResetDependencies;
    using codex::banked_reset::CodexBankedResetFence;
    using codex::banked_reset_provider::CodexUsageResetProvider;
    using codex::banked_reset_provider::LookupRedeemResetInput;
    using codex::banked_reset_provider::RedeemResetInput;
    using codex::banked_reset_provider::ResetAccountContext;
    using codex::banked_reset_provider::ResetInventoryCredit;

    // `assert.*` throws: a failed harness assertion fails the running test the same way (rt::test::run
    // reports the uncaught exception). The harness cannot include uos/rt/test.hpp, which defines main.
    [[noreturn]] void fail(const std::string& text) { throw rt::JsError(text, "AssertionError"); }

    void check(bool condition, const std::string& text) {
      if (!condition) fail(text);
    }

    // A rejected promise as a Task that fails when it starts (`Promise.reject(error)`).
    template <class T> rt::Task<T> rejectedTask(rt::Reason reason) {
      if (reason) std::rethrow_exception(reason);
      throw rt::TypeError("undefined is not an object");
      co_return T{};
    }

    // `{ kind, providerReceiptId }` and friends as plain objects.
    json::value resultObject(std::string_view kind, std::string_view member, json::value value) {
      json::object result;
      result["kind"] = json::string(kind);
      result[member] = std::move(value);
      return result;
    }

    json::value onlyKind(std::string_view kind) {
      json::object result;
      result["kind"] = json::string(kind);
      return result;
    }

    // The stored value as the TS reads it back (an unchecked cast of whatever was stored).

    // The atomic operation of the TS fake: it records checks and sets and commits them at once.
    class FakeAtomicOperation : public rt::AtomicOperation {
    public:
      explicit FakeAtomicOperation(MemoryKv* kv_) : kv(kv_) {}
      using rt::AtomicOperation::check;
      using rt::AtomicOperation::max;
      using rt::AtomicOperation::min;
      using rt::AtomicOperation::sum;

      rt::AtomicOperation& check(std::vector<rt::AtomicCheck> items) override {
        for (rt::AtomicCheck& item : items) checks.push_back(std::move(item));
        return *this;
      }
      rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions) override {
        writes.push_back(StoredEntry{.key = std::move(key), .value = clone(value), .versionstamp = ""});
        return *this;
      }
      rt::AtomicOperation& mutate(std::vector<rt::KvMutation>) override { throw rt::TypeError("operation.mutate is not a function"); }
      rt::AtomicOperation& sum(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("operation.sum is not a function"); }
      rt::AtomicOperation& min(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("operation.min is not a function"); }
      rt::AtomicOperation& max(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("operation.max is not a function"); }
      rt::AtomicOperation& delete_(rt::KvKey) override { throw rt::TypeError("operation.delete is not a function"); }

      // Not async in the TS: the commit happens when called and its result settles at once.
      rt::Task<rt::KvCommitResult> commit() override {
        kv->atomicCommitCount += 1;
        std::vector<rt::KvKey> keys;
        for (const rt::AtomicCheck& item : checks) keys.push_back(item.key);
        kv->atomicCheckBatches.push_back(std::move(keys));
        if (kv->beforeAtomicCommit) {
          const std::function<void(double)> hook = kv->beforeAtomicCommit;
          hook(kv->atomicCommitCount);
        }
        if (kv->failCommitNumbers.has(kv->atomicCommitCount)) return rt::fulfilledTask(rt::KvCommitResult{.ok = false, .versionstamp = ""});
        for (const rt::AtomicCheck& item : checks) {
          if (kv->versionstamp(item.key) != item.versionstamp) return rt::fulfilledTask(rt::KvCommitResult{.ok = false, .versionstamp = ""});
        }
        const std::string versionstamp = kv->nextVersionstamp();
        for (const StoredEntry& write : writes) kv->put(write.key, write.value, versionstamp);
        return rt::fulfilledTask(rt::KvCommitResult{.ok = true, .versionstamp = versionstamp});
      }

    private:
      MemoryKv* kv;
      std::vector<rt::AtomicCheck> checks;
      std::vector<StoredEntry> writes;
    };
  }

  rt::KvValue clone(const rt::KvValue& value) { return rt::kvStructuredClone(value); }

  std::string encodeKey(const rt::KvKey& key) {
    json::array parts;
    for (const rt::KvKeyPart& part : key) {
      if (part.isString()) parts.push_back(json::value(json::string(part.string())));
      else if (part.isNumber()) parts.push_back(rt::makeNumber(part.number()));
      else if (part.isBool()) parts.push_back(json::value(part.boolean()));
      else if (part.isBigInt()) throw rt::TypeError("JSON.stringify cannot serialize BigInt.");
      else {
        // A Uint8Array serializes as an object of its indexed bytes.
        json::object bytes;
        const rt::KvBytes& data = part.bytes();
        for (std::size_t index = 0; index < data.size(); ++index) bytes[std::to_string(index)] = rt::makeNumber(static_cast<std::int64_t>(data[index]));
        parts.push_back(json::value(std::move(bytes)));
      }
    }
    return rt::jsonStringify(json::value(std::move(parts)));
  }

  std::string MemoryKv::nextVersionstamp() {
    version_ += 1;
    return rt::padStart(rt::numberToString(version_), 20, "0");
  }

  void MemoryKv::put(const rt::KvKey& key, const rt::KvValue& value, const std::string& versionstamp_) {
    entries.set(encodeKey(key), StoredEntry{.key = key, .value = clone(value), .versionstamp = versionstamp_});
  }

  std::optional<std::string> MemoryKv::versionstamp(const rt::KvKey& key) const {
    const StoredEntry* entry = entries.get(encodeKey(key));
    if (entry == nullptr) return std::nullopt;
    return entry->versionstamp;
  }

  rt::Task<rt::KvEntryMaybe<>> MemoryKv::get(rt::KvKey key, rt::KvGetOptions) {
    if (getFailure) std::rethrow_exception(getFailure);
    std::optional<rt::Promise<void>> pause;
    if (beforeGet) {
      getCount += 1;
      const std::function<std::optional<rt::Promise<void>>(rt::KvKey, double)> hook = beforeGet;
      pause = hook(key, getCount);
    }
    if (pause) co_await pause->wait();
    rt::KvEntryMaybe<> result;
    result.key = key;
    const StoredEntry* entry = entries.get(encodeKey(key));
    if (entry != nullptr) {
      result.value = clone(entry->value);
      result.versionstamp = entry->versionstamp;
    } else {
      result.value = nullptr;
    }
    co_return result;
  }

  rt::Task<rt::KvCommitResult> MemoryKv::set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions) {
    const std::string stamp = nextVersionstamp();
    put(key, value, stamp);
    return rt::fulfilledTask(rt::KvCommitResult{.ok = true, .versionstamp = stamp});
  }

  std::optional<json::value> MemoryKv::redemptionRecord(const rt::KvKey& key) const {
    const StoredEntry* entry = entries.get(encodeKey(key));
    if (entry == nullptr || entry->value.isUndefined()) return std::nullopt;
    return rt::storedJsonView(clone(entry->value), STORED_JSON_VIEW_OPTIONS);
  }

  std::unique_ptr<rt::AtomicOperation> MemoryKv::atomic() { return std::make_unique<FakeAtomicOperation>(this); }

  rt::Task<std::vector<rt::KvEntryMaybe<>>> MemoryKv::getMany(std::vector<rt::KvKey>, rt::KvGetOptions) {
    throw rt::TypeError("kv.getMany is not a function");
  }

  rt::Task<void> MemoryKv::delete_(rt::KvKey) { throw rt::TypeError("kv.delete is not a function"); }

  std::unique_ptr<rt::KvListIterator> MemoryKv::list(rt::KvListSelector, rt::KvListOptions) { throw rt::TypeError("kv.list is not a function"); }

  json::object provenContract() {
    json::object idempotency;
    idempotency["callerSupplied"] = true;
    idempotency["retentionMs"] = rt::makeNumber(86'400'000.0);
    json::object lookup;
    lookup["byIdempotencyKey"] = true;
    lookup["byProviderReceiptId"] = true;
    json::object verification;
    verification["independentlyVerifiable"] = true;
    json::array supportedResetTypes;
    supportedResetTypes.push_back(json::string("codex_rate_limits"));
    json::object contract;
    contract["idempotency"] = std::move(idempotency);
    contract["lookup"] = std::move(lookup);
    contract["verification"] = std::move(verification);
    contract["receiptIdsSafeToPersistAndLog"] = true;
    contract["supportedResetTypes"] = std::move(supportedResetTypes);
    return contract;
  }

  const SanitizedProviderFixtures& sanitizedProviderFixtures() {
    static const SanitizedProviderFixtures fixtures{
      .inventory_available =
        ResetInventory{
          .availableCount = 1,
          .observedAtMs = 1'700'000'000'000,
          .credits = {ResetInventoryCredit{.id = "fixture-credit", .status = "available", .resetType = "codex_rate_limits", .expiresAtMs = std::nullopt}},
        },
      .inventory_empty = ResetInventory{.availableCount = 0, .observedAtMs = 1'700'000'000'000, .credits = {}},
      .redemption_completed = resultObject("completed", "providerReceiptId", json::string("fixture-completed")),
      .redemption_accepted = resultObject("accepted", "providerReceiptId", json::string("fixture-accepted")),
      .redemption_already_redeemed = resultObject("already_redeemed", "providerReceiptId", json::string("fixture-replay")),
      .redemption_rejected = resultObject("rejected", "reason", json::string("fixture-definitive-rejection")),
      .authentication_error = resultObject("rejected", "reason", json::string("fixture-authentication-error")),
      .rate_limit = resultObject("unknown", "providerReceiptId", nullptr),
      .server_error = resultObject("unknown", "providerReceiptId", nullptr),
      .malformed_success = onlyKind("completed"),
      .schema_drift = resultObject("future_completed", "providerReceiptId", json::string("fixture-unknown")),
      .lookup_pending = resultObject("unknown", "providerReceiptId", nullptr),
      .lookup_completed = resultObject("completed", "providerReceiptId", json::string("fixture-lookup-completed")),
      .lookup_rejected = resultObject("rejected", "reason", json::string("fixture-lookup-rejected")),
      .lookup_not_found = resultObject("rejected", "reason", json::string("fixture-lookup-not-found")),
    };
    return fixtures;
  }

  RedeemResetResult toRedeemResetResult(const json::value& result) {
    namespace provider = codex::banked_reset_provider;
    const std::optional<std::string_view> kind = rt::getString(result, "kind");
    const json::value* receipt = rt::get(result, "providerReceiptId");
    const bool receiptIsText = receipt != nullptr && receipt->is_string();
    if (kind == "completed" && receiptIsText) return provider::RedeemResetResultCompleted{.providerReceiptId = std::string(receipt->get_string())};
    if (kind == "accepted" && receiptIsText) return provider::RedeemResetResultAccepted{.providerReceiptId = std::string(receipt->get_string())};
    if (kind == "already_redeemed" && receiptIsText) return provider::RedeemResetResultAlreadyRedeemed{.providerReceiptId = std::string(receipt->get_string())};
    if (kind == "rejected") {
      if (const std::optional<std::string_view> reason = rt::getString(result, "reason")) return provider::RedeemResetResultRejected{.reason = std::string(*reason)};
    }
    if (kind == "unknown" && receipt != nullptr && receipt->is_null()) return provider::RedeemResetResultUnknown{.providerReceiptId = std::nullopt};
    if (kind == "unknown" && receiptIsText) return provider::RedeemResetResultUnknown{.providerReceiptId = std::string(receipt->get_string())};
    // Malformed: a terminal result without a usable receipt, which validRedeemResult refuses.
    return provider::RedeemResetResultCompleted{.providerReceiptId = ""};
  }

  FakeCodexUsageResetProvider::FakeCodexUsageResetProvider() : FakeCodexUsageResetProvider(json::value(provenContract())) {}

  FakeCodexUsageResetProvider::FakeCodexUsageResetProvider(json::value contract_) : CodexUsageResetProvider(std::move(contract_)) {
    inventory = ResetInventory{
      .availableCount = 1,
      .observedAtMs = 1'700'000'000'000,
      .credits = {ResetInventoryCredit{.id = "test-credit", .status = "available", .resetType = "codex_rate_limits", .expiresAtMs = std::nullopt}},
    };
    redeemResult = resultObject("completed", "providerReceiptId", json::string("receipt-completed"));
    lookupResult = resultObject("completed", "providerReceiptId", json::string("receipt-lookup"));
  }

  double FakeCodexUsageResetProvider::callCount() const {
    return static_cast<double>(inventoryInputs.size() + redeemInputs.size() + lookupInputs.size() + verificationInputs.size());
  }

  void FakeCodexUsageResetProvider::record(const std::string& method, const ResetAccountContext& input, std::optional<std::string> idempotencyKey,
                                           std::optional<std::string> providerReceiptId, std::optional<std::string> timeoutStage) {
    calls.push_back(FakeProviderCall{
      .method = method,
      .accountId = input.accountId,
      .quotaGeneration = input.quotaGeneration,
      .idempotencyKey = std::move(idempotencyKey),
      .commitCount = commitCount,
      .providerReceiptId = std::move(providerReceiptId),
      .timeoutStage = std::move(timeoutStage),
    });
  }

  rt::Task<ResetInventory> FakeCodexUsageResetProvider::readInventory(ResetAccountContext input, rt::AbortSignal signal) {
    if (readInventoryReplacement) {
      const auto replacement = readInventoryReplacement;
      return replacement(std::move(input), std::move(signal));
    }
    return fakeReadInventory(std::move(input));
  }

  rt::Task<ResetInventory> FakeCodexUsageResetProvider::fakeReadInventory(ResetAccountContext input) {
    inventoryInputs.push_back(input);
    record("readInventory", input, std::nullopt);
    if (inventoryEntered) inventoryEntered->resolve();
    const std::optional<rt::Promise<void>> gate = inventoryGate;
    inventoryGate.reset();
    if (gate) co_await gate->wait();
    if (inventoryFailure) std::rethrow_exception(inventoryFailure);
    co_return inventory;
  }

  rt::Task<RedeemResetResult> FakeCodexUsageResetProvider::redeem(RedeemResetInput input, rt::AbortSignal signal) {
    if (redeemReplacement) {
      const auto replacement = redeemReplacement;
      return replacement(std::move(input), std::move(signal));
    }
    return fakeRedeem(std::move(input));
  }

  rt::Task<RedeemResetResult> FakeCodexUsageResetProvider::fakeRedeem(RedeemResetInput input) {
    redeemInputs.push_back(input);
    if (redeemEntered) redeemEntered->resolve();
    if (redeemGate) {
      const rt::Promise<void> gate = *redeemGate;
      co_await gate.wait();
    }
    if (redeemFailure) {
      record("redeem", input, input.idempotencyKey, std::nullopt, std::string("before_provider_commit"));
      std::rethrow_exception(redeemFailure);
    }
    const json::value result = redeemResult;
    const std::optional<std::string_view> kind = rt::getString(result, "kind");
    if (commitOnRedeem.value_or(kind == "completed" || kind == "accepted")) commitCount += 1;
    // `"providerReceiptId" in result`: TypeError on a primitive, as the `in` operator.
    if (!result.is_object() && !result.is_array()) throw rt::TypeError("cannot use 'in' operator to search for 'providerReceiptId'");
    const json::value* receiptValue = rt::get(result, "providerReceiptId");
    const std::optional<std::string> receipt =
      receiptValue != nullptr && receiptValue->is_string() ? std::optional<std::string>(std::string(receiptValue->get_string())) : std::nullopt;
    if (redeemFailureAfterCommit) {
      record("redeem", input, input.idempotencyKey, receipt, std::string("after_provider_commit"));
      std::rethrow_exception(redeemFailureAfterCommit);
    }
    record("redeem", input, input.idempotencyKey, receipt);
    co_return toRedeemResetResult(result);
  }

  rt::Task<RedeemResetResult> FakeCodexUsageResetProvider::lookup(LookupRedeemResetInput input, rt::AbortSignal signal) {
    if (lookupReplacement) {
      const auto replacement = lookupReplacement;
      return replacement(std::move(input), std::move(signal));
    }
    return fakeLookup(std::move(input));
  }

  // Not async in the TS: it records at once and returns a settled promise.
  rt::Task<RedeemResetResult> FakeCodexUsageResetProvider::fakeLookup(LookupRedeemResetInput input) {
    lookupInputs.push_back(input);
    if (!lookupResult.is_object() && !lookupResult.is_array()) throw rt::TypeError("cannot use 'in' operator to search for 'providerReceiptId'");
    const json::value* receiptValue = rt::get(lookupResult, "providerReceiptId");
    const std::optional<std::string> receipt =
      receiptValue != nullptr && receiptValue->is_string() ? std::optional<std::string>(std::string(receiptValue->get_string())) : std::nullopt;
    record("lookup", input, input.idempotencyKey, receipt);
    if (lookupFailure) return rejectedTask<RedeemResetResult>(lookupFailure);
    return rt::fulfilledTask(toRedeemResetResult(lookupResult));
  }

  rt::Task<json::value> FakeCodexUsageResetProvider::verifyApplied(ResetAccountContext input, rt::AbortSignal signal) {
    if (verifyAppliedReplacement) {
      const auto replacement = verifyAppliedReplacement;
      return replacement(std::move(input), std::move(signal));
    }
    return fakeVerifyApplied(std::move(input));
  }

  rt::Task<json::value> FakeCodexUsageResetProvider::fakeVerifyApplied(ResetAccountContext input) {
    verificationInputs.push_back(input);
    if (verifyEntered) verifyEntered->resolve();
    const std::optional<rt::Promise<void>> gate = verifyGate;
    verifyGate.reset();
    if (gate) co_await gate->wait();
    record("verifyApplied", input, std::nullopt);
    if (verifyFailure) {
      // `return Promise.reject(error)` from an async function settles two turns later.
      const rt::Promise<json::value> rejected = rt::Promise<json::value>::reject(verifyFailure);
      co_await rt::microtask();
      const json::value never = co_await rejected.wait();
      co_return never;
    }
    co_return verifyResult;
  }

  std::string testHashText(const std::string& value) {
    std::int32_t hash = rt::toInt32(2'166'136'261.0);
    const std::u16string units = rt::toUtf16(value);
    for (const char16_t unit : units) {
      hash = rt::bitXor(static_cast<double>(hash), static_cast<double>(unit));
      hash = rt::Math::imul(static_cast<double>(hash), 16'777'619);
    }
    char hex[8];
    const auto end = std::to_chars(hex, hex + sizeof hex, static_cast<std::uint32_t>(hash), 16).ptr;
    return "test-hash-" + std::string(hex, end) + "-" + rt::numberToString(static_cast<double>(units.size()));
  }

  rt::Task<std::string> testHash(std::string value) { return rt::fulfilledTask(testHashText(value)); }

  bool isRecord(const json::value& value) { return value.is_object(); }

  rt::KvKey routingFenceKey(const std::string& accountId) { return rt::KvKey{"test", "codex-banked-reset", "routing", accountId}; }

  rt::KvKey credentialFenceKey(const std::string& accountId) { return rt::KvKey{"test", "codex-banked-reset", "credential", accountId}; }

  std::vector<CodexBankedResetFence> bankedResetFences(const std::string& accountId, double routingGeneration, const std::string& credentialVersion) {
    std::vector<CodexBankedResetFence> fences;
    fences.push_back(CodexBankedResetFence{
      .key = routingFenceKey(accountId),
      .isCurrent =
        [routingGeneration](const json::value& value) {
          const json::value* generation = rt::get(value, "routing_generation");
          return isRecord(value) && rt::getString(value, "kind") == "routing" && generation != nullptr && rt::isNumber(*generation) &&
                 rt::toNumber(*generation) == routingGeneration;
        },
    });
    fences.push_back(CodexBankedResetFence{
      .key = credentialFenceKey(accountId),
      .isCurrent =
        [credentialVersion](const json::value& value) {
          return isRecord(value) && rt::getString(value, "kind") == "credential" && rt::getString(value, "credential_version") == credentialVersion;
        },
    });
    return fences;
  }

  CodexBankedResetCandidate candidate() { return candidate(CandidateOverrides{}); }

  CodexBankedResetCandidate candidate(const CandidateOverrides& overrides) {
    const std::string accountId = overrides.accountId.value_or("test-account-a");
    const std::string credentialVersion = overrides.credentialVersion.value_or("test-credential-v1");
    const double quotaResetAtMs = overrides.quotaResetAtMs.value_or(1'700'000'060'000);
    const double routingGeneration = overrides.routingGeneration.value_or(7);
    CodexBankedResetCandidate result{
      .accountId = accountId,
      .authorization = std::nullopt,
      .credentialVersion = credentialVersion,
      .quotaResetAtMs = quotaResetAtMs,
      .routingGeneration = routingGeneration,
      .fences = overrides.fences ? *overrides.fences : bankedResetFences(accountId, routingGeneration, credentialVersion),
      .requestId = overrides.requestId.value_or("test-request"),
      .selectedCredit = std::nullopt,
      .signal = overrides.signal,
    };
    return result;
  }

  rt::Task<void> seedFences(std::shared_ptr<MemoryKv> kv, CodexBankedResetCandidate input) {
    json::object routing;
    routing["kind"] = "routing";
    routing["routing_generation"] = rt::makeNumber(input.routingGeneration);
    const rt::KvValue routingValue(routing);
    auto settingRouting = kv->set(routingFenceKey(input.accountId), routingValue);
    co_await std::move(settingRouting);
    json::object credential;
    credential["kind"] = "credential";
    credential["credential_version"] = input.credentialVersion;
    const rt::KvValue credentialValue(credential);
    auto settingCredential = kv->set(credentialFenceKey(input.accountId), credentialValue);
    co_await std::move(settingCredential);
  }

  bool hasKey(const std::vector<rt::KvKey>& keys, const rt::KvKey& expected) {
    const std::string encoded = encodeKey(expected);
    return std::any_of(keys.begin(), keys.end(), [&encoded](const rt::KvKey& key) { return encodeKey(key) == encoded; });
  }

  CodexBankedResetConfig config() { return config(ConfigOverrides{}); }

  CodexBankedResetConfig config(const ConfigOverrides& overrides) {
    return CodexBankedResetConfig{
      .enabled = overrides.enabled.value_or(true),
      .mode = overrides.mode.value_or("live"),
      .maxPerAccountPerDay = overrides.maxPerAccountPerDay.value_or(1),
      .maxPerAccountPerWindow = overrides.maxPerAccountPerWindow.value_or(1),
    };
  }

  CodexBankedResetDependencies dependencies(std::shared_ptr<MemoryKv> kv, std::shared_ptr<CodexUsageResetProvider> provider, std::shared_ptr<TestClock> clock) {
    return dependencies(std::move(kv), std::move(provider), std::move(clock), config());
  }

  CodexBankedResetDependencies dependencies(std::shared_ptr<MemoryKv> kv, std::shared_ptr<CodexUsageResetProvider> provider, std::shared_ptr<TestClock> clock,
                                            CodexBankedResetConfig configured, codex::banked_reset::CodexBankedResetTelemetry telemetry,
                                            std::function<CodexBankedResetConfig()> reloadConfig) {
    auto owner = std::make_shared<double>(0);
    CodexBankedResetDependencies result;
    result.config = std::move(configured);
    result.provider = std::move(provider);
    result.kv = std::shared_ptr<rt::Kv>(std::move(kv));
    result.now = [clock] { return clock->nowMs; };
    result.newOwnerToken = [owner] {
      *owner += 1;
      return "test-owner-" + rt::numberToString(*owner);
    };
    result.hash = &testHash;
    result.telemetry = std::move(telemetry);
    if (reloadConfig) result.reloadConfig = std::move(reloadConfig);
    return result;
  }

  std::vector<codex::banked_reset::CodexBankedResetPoolCandidate> fullPool(const CodexBankedResetCandidate& first,
                                                                           std::shared_ptr<CodexUsageResetProvider> firstProvider,
                                                                           const CodexBankedResetCandidate& second,
                                                                           std::shared_ptr<CodexUsageResetProvider> secondProvider) {
    std::vector<codex::banked_reset::CodexBankedResetPoolCandidate> pool;
    pool.push_back(codex::banked_reset::CodexBankedResetPoolCandidate{.slot = 0, .candidate = first, .provider = std::move(firstProvider)});
    pool.push_back(codex::banked_reset::CodexBankedResetPoolCandidate{.slot = 1, .candidate = second, .provider = std::move(secondProvider)});
    return pool;
  }

  ResetInventory inventory(const std::string& id, std::optional<double> expiresAtMs) {
    return ResetInventory{
      .availableCount = 1,
      .observedAtMs = 1'700'000'000'000,
      .credits = {ResetInventoryCredit{.id = id, .status = "available", .resetType = "codex_rate_limits", .expiresAtMs = expiresAtMs}},
    };
  }

  std::string requiredHash(const std::optional<std::string>& value) {
    if (!rt::truthy(value)) fail("expected the reset result to carry an account hash");
    return *value;
  }

  ::uos::types::CodexResetShadowDecisionRecord shadowDecisionFrom(const MemoryKv& kv) {
    std::vector<::uos::types::CodexResetShadowDecisionRecord> decisions;
    for (const auto& [encoded, entry] : kv.entries) {
      std::optional<::uos::types::CodexResetShadowDecisionRecord> decision = parseCodexResetShadowDecisionRecord(rt::storedJsonView(entry.value, STORED_JSON_VIEW_OPTIONS));
      if (decision) decisions.push_back(std::move(*decision));
    }
    check(decisions.size() == 1, "expected exactly one shadow decision, saw " + std::to_string(decisions.size()));
    return decisions.front();
  }

  std::string_view toString(BankedResetGeneratedEvent event) {
    switch (event) {
      case BankedResetGeneratedEvent::request:
        return "request";
      case BankedResetGeneratedEvent::qualifying_429:
        return "qualifying_429";
      case BankedResetGeneratedEvent::non_qualifying_429:
        return "non_qualifying_429";
      case BankedResetGeneratedEvent::claim:
        return "claim";
      case BankedResetGeneratedEvent::submit:
        return "submit";
      case BankedResetGeneratedEvent::provider_commit:
        return "provider_commit";
      case BankedResetGeneratedEvent::response_loss:
        return "response_loss";
      case BankedResetGeneratedEvent::lookup:
        return "lookup";
      case BankedResetGeneratedEvent::verify:
        return "verify";
      case BankedResetGeneratedEvent::retry:
        return "retry";
      case BankedResetGeneratedEvent::crash:
        return "crash";
      case BankedResetGeneratedEvent::lease_expire:
        return "lease_expire";
      case BankedResetGeneratedEvent::credential_rotate:
        return "credential_rotate";
      case BankedResetGeneratedEvent::kv_failure:
        return "kv_failure";
    }
    return "";
  }

  const std::vector<BankedResetGeneratedEvent> BANKED_RESET_GENERATED_EVENTS{
    BankedResetGeneratedEvent::request,
    BankedResetGeneratedEvent::qualifying_429,
    BankedResetGeneratedEvent::non_qualifying_429,
    BankedResetGeneratedEvent::claim,
    BankedResetGeneratedEvent::submit,
    BankedResetGeneratedEvent::provider_commit,
    BankedResetGeneratedEvent::response_loss,
    BankedResetGeneratedEvent::lookup,
    BankedResetGeneratedEvent::verify,
    BankedResetGeneratedEvent::retry,
    BankedResetGeneratedEvent::crash,
    BankedResetGeneratedEvent::lease_expire,
    BankedResetGeneratedEvent::credential_rotate,
    BankedResetGeneratedEvent::kv_failure,
  };

  namespace {
    rt::Task<void> attemptInto(std::shared_ptr<GeneratedResetScenario> scenario) {
      rt::Promise<codex::banked_reset::CodexBankedResetOutcome> attempting(attemptCodexBankedReset(scenario->reset, scenario->deps));
      scenario->lastOutcome = co_await attempting.wait();
    }

    rt::Task<void> reconcileInto(std::shared_ptr<GeneratedResetScenario> scenario) {
      rt::Promise<codex::banked_reset::CodexBankedResetOutcome> reconciling(reconcileCodexBankedReset(scenario->reset, scenario->deps));
      scenario->lastOutcome = co_await reconciling.wait();
    }
  }

  rt::Task<void> applyBankedResetEvent(std::shared_ptr<GeneratedResetScenario> scenario, BankedResetGeneratedEvent event, std::string label) {
    switch (event) {
      case BankedResetGeneratedEvent::request:
      case BankedResetGeneratedEvent::non_qualifying_429:
        // A generic, malformed, or otherwise non-qualifying 429 never
        // enters the state machine's submission path.
        scenario->qualifyingObservationIsCurrent = false;
        scenario->lastOutcome = std::nullopt;
        break;
      case BankedResetGeneratedEvent::qualifying_429: {
        scenario->qualifyingObservationIsCurrent = true;
        rt::Promise<void> attempting(attemptInto(scenario));
        co_await attempting.wait();
        break;
      }
      case BankedResetGeneratedEvent::claim:
      case BankedResetGeneratedEvent::submit:
        if (scenario->qualifyingObservationIsCurrent) {
          rt::Promise<void> attempting(attemptInto(scenario));
          co_await attempting.wait();
        }
        break;
      case BankedResetGeneratedEvent::provider_commit:
        scenario->provider->redeemResult = sanitizedProviderFixtures().rate_limit;
        scenario->provider->commitOnRedeem = true;
        if (scenario->qualifyingObservationIsCurrent) {
          rt::Promise<void> attempting(attemptInto(scenario));
          co_await attempting.wait();
        }
        break;
      case BankedResetGeneratedEvent::response_loss:
        scenario->provider->redeemFailureAfterCommit = rt::makeReason(rt::JsError("response loss " + rt::numberToString(scenario->sequenceSeed)));
        if (scenario->qualifyingObservationIsCurrent) {
          rt::Promise<void> attempting(attemptInto(scenario));
          co_await attempting.wait();
        }
        scenario->provider->redeemFailureAfterCommit = nullptr;
        break;
      case BankedResetGeneratedEvent::lookup: {
        scenario->clock->advance(CODEX_BANKED_RESET_LEASE_MS + 1);
        scenario->provider->lookupResult = sanitizedProviderFixtures().lookup_pending;
        rt::Promise<void> reconciling(reconcileInto(scenario));
        co_await reconciling.wait();
        break;
      }
      case BankedResetGeneratedEvent::verify: {
        scenario->clock->advance(CODEX_BANKED_RESET_LEASE_MS + 1);
        scenario->provider->lookupResult = sanitizedProviderFixtures().lookup_completed;
        scenario->provider->verifyResult = true;
        rt::Promise<void> reconciling(reconcileInto(scenario));
        co_await reconciling.wait();
        break;
      }
      case BankedResetGeneratedEvent::retry: {
        const double retriesBefore = scenario->postResetInferenceRetries;
        const std::optional<codex::banked_reset::CodexBankedResetOutcome>& outcome = scenario->lastOutcome;
        bool retryPermitted = false;
        // The model can execute its one retry only from a verified durable
        // outcome; it never treats an unknown or rejected state as a permit.
        if (outcome && outcome->kind == "verified" && retriesBefore == 0) {
          retryPermitted = true;
          check(outcome->record && outcome->record->state == ::uos::types::CodexResetRedemptionState::verified, label);
          // This is the generated model's post-reset transport. The public
          // Responses/Chat matrix separately drives the real gateway transport;
          // here it makes the retry permit an executable state transition rather
          // than a bookkeeping increment.
          scenario->postResetInferenceRetries += 1;
        }
        check((scenario->postResetInferenceRetries > retriesBefore) == retryPermitted, label + ": retry must be granted only after verification");
        check(scenario->postResetInferenceRetries <= 1, label);
        break;
      }
      case BankedResetGeneratedEvent::crash:
      case BankedResetGeneratedEvent::lease_expire:
        scenario->clock->advance(CODEX_BANKED_RESET_LEASE_MS + 1);
        break;
      case BankedResetGeneratedEvent::credential_rotate: {
        scenario->credentialWasRotated = true;
        json::object credential;
        credential["kind"] = "credential";
        credential["credential_version"] = "rotated-" + rt::numberToString(scenario->sequenceSeed);
        const rt::KvValue rotated(credential);
        auto setting = scenario->kv->set(credentialFenceKey(scenario->reset.accountId), rotated);
        co_await std::move(setting);
        break;
      }
      case BankedResetGeneratedEvent::kv_failure: {
        scenario->kv->getFailure = rt::makeReason(rt::JsError("generated KV outage " + rt::numberToString(scenario->sequenceSeed)));
        rt::Promise<codex::banked_reset::CodexBankedResetOutcome> attempting(attemptCodexBankedReset(scenario->reset, scenario->deps));
        const codex::banked_reset::CodexBankedResetOutcome failure = co_await attempting.wait();
        check(failure.kind != "verified", label);
        scenario->kv->getFailure = nullptr;
        break;
      }
    }
  }

  void assertBankedResetInvariants(const GeneratedResetScenario& scenario, const std::string& label, double submissionsBefore, const std::string& mode) {
    std::set<std::string> idempotencyKeys;
    for (const RedeemResetInput& input : scenario.provider->redeemInputs) idempotencyKeys.insert(input.idempotencyKey);
    const auto submissions = static_cast<double>(scenario.provider->redeemInputs.size());
    check(scenario.provider->commitCount <= 1, label + ": more than one provider commit");
    check(submissions <= 1, label + ": more than one submission");
    check(idempotencyKeys.size() <= 1, label + ": different idempotency keys");
    if (!scenario.qualifyingObservationIsCurrent) {
      check(submissions == submissionsBefore, label + ": a non-qualifying response reached submission");
    }
    if (mode == "disabled" || mode == "shadow") {
      check(scenario.provider->commitCount == 0, label + ": inactive mode committed");
    }
    if (scenario.credentialWasRotated) {
      // A rotated fence may still permit provider-level reconciliation of a
      // prior unknown record, but it cannot start a submission under the
      // stale candidate fence.
      check(submissions == submissionsBefore, label + ": stale fence submitted again");
    }
  }
}
