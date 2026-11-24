// Port of tests/codex-capacity-routing-coverage.test.ts.
// Links: tests/codex_capacity_routing_coverage_test.cpp src/codex/capacity_routing.cpp src/codex/routing_state.cpp
//        src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp
//        src/kv.cpp src/storage/kv.cpp src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp src/storage/value_compatibility.cpp
// src/storage/key_compatibility.cpp src/utils.cpp
//
// The suite's ScriptedKv is ported below on rt::Kv. TS `assert.equal` identity checks on routing
// objects (`cached === first`, `noChange === reconciled`, `grown.slots[0] === state.slots[0]`,
// `reconciled.slots[1] === base.slots[1]`) are value comparisons here: the C++ results are copies.
#include "uos/codex/capacity_routing.hpp"

#include "uos/authority/native_codex_evaluation.hpp"
#include "uos/codex/banked_reset.hpp"

#include "uos/kv.hpp"
#include "uos/provider/capacity_contract.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/utils.hpp"

#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace capacity_routing = uos::codex::capacity_routing;
  namespace routing_state = uos::codex::routing_state;
  using routing_state::CodexAccountRoutingState;
  using routing_state::CodexCapacityRoutingObservation;
  using routing_state::CodexCapacityRoutingObservationInput;
  using routing_state::CodexCapacityRoutingWindow;
  using routing_state::CodexQuotaClassBlock;
  using routing_state::CodexRoutingSlot;
  using routing_state::RoutingAccount;
  using uos::types::CodexAuthPoolState;
  using uos::types::CodexAuthState;

  /* ------------------------------------------------------------------ fixtures */

  CodexAuthState account(const std::string& accountId) {
    return CodexAuthState{.access_token = "access-" + accountId, .refresh_token = "refresh-" + accountId, .account_id = accountId, .updated_at_ms = 1};
  }

  CodexAuthPoolState poolOf(std::vector<CodexAuthState> accounts) { return CodexAuthPoolState{.accounts = std::move(accounts), .updated_at_ms = 1}; }

  rt::Task<std::string> accountHashOf(CodexAuthState auth) {
    auto identifying = capacity_routing::routingAccountIdentity(auth);
    const capacity_routing::CodexRoutingAccountIdentity identity = co_await std::move(identifying);
    co_return identity.accountIdHash;
  }

  // `routingAccountOf(auth, slot, extra)`: `extra` is applied by the caller to the returned account.
  rt::Task<RoutingAccount> routingAccountOf(CodexAuthState auth, double slot) {
    co_await rt::microtask();
    auto hashing = accountHashOf(auth);
    const std::string accountIdHash = co_await std::move(hashing);
    RoutingAccount routed;
    routed.auth = auth;
    routed.slot = slot;
    routed.accountIdHash = accountIdHash;
    routed.credentialVersion = "credential-" + auth.account_id;
    routed.quotaHeadroom = std::nullopt;
    routed.probeRequired = true;
    routed.probeGeneration = std::nullopt;
    routed.probeToken = std::nullopt;
    co_return routed;
  }

  CodexAccountRoutingState stateWith(std::vector<CodexRoutingSlot> slots) {
    CodexAccountRoutingState state;
    state.v = 2;
    state.updated_at_ms = 1;
    state.banked_reset_legacy_identity_unresolved = false;
    state.slots = std::move(slots);
    return state;
  }

  CodexQuotaClassBlock quotaBlock(double blockedUntilMs) {
    CodexQuotaClassBlock block;
    block.blocked_until_ms = blockedUntilMs;
    block.source = "header_retry_after";
    block.legacy_fallback = false;
    block.quota_signal_observed_at_ms = blockedUntilMs - 1'000;
    block.observed_reset_at_ms = std::nullopt;
    block.observed_reset_at_is_stable = false;
    block.banked_reset_generation_ambiguous = false;
    block.banked_reset_recovery_probe_pending = false;
    return block;
  }

  CodexRoutingSlot slotWithClassBlock(const std::string& quotaClassKey, const CodexQuotaClassBlock& block) {
    CodexRoutingSlot slot = capacity_routing::neutralSlot("credential-one", std::string("account-one-hash"));
    routing_state::CodexQuotaBlocksByClass blocks;
    blocks.set(quotaClassKey, block);
    slot.quota_blocks_by_class = blocks;
    slot.quota_blocked_classes = std::vector<std::string>{quotaClassKey};
    slot.quota_blocked_until_ms = block.blocked_until_ms;
    slot.quota_block_source = block.source;
    slot.quota_signal_observed_at_ms = block.quota_signal_observed_at_ms;
    return slot;
  }

  CodexCapacityRoutingWindow window(std::optional<double> limitWindowSeconds, std::optional<double> usedPercent, std::optional<double> resetAtMs) {
    CodexCapacityRoutingWindow out;
    out.limit_window_seconds = limitWindowSeconds;
    out.used_percent = usedPercent;
    out.reset_at_ms = resetAtMs;
    return out;
  }

  // `observationOf({ slot, account_id_hash, ...overrides })`
  CodexCapacityRoutingObservation observationOf(double slot, const std::string& accountIdHash) {
    CodexCapacityRoutingObservation observation;
    observation.slot = slot;
    observation.account_id_hash = accountIdHash;
    observation.state = "available";
    observation.source_observed_at_ms = std::nullopt;
    observation.snapshot_at_ms = 1'790'000'000'000;
    observation.windows.primary = window(300, 10, std::nullopt);
    observation.windows.secondary = std::nullopt;
    return observation;
  }

  json::value slotJson(const CodexRoutingSlot& slot) { return json::value(routing_state::toJson(slot)); }
  json::value stateJson(const CodexAccountRoutingState& state) { return json::value(routing_state::toJson(state)); }
  json::value observationJson(const CodexCapacityRoutingObservation& observation) { return json::value(routing_state::toJson(observation)); }

  /* ------------------------------------------------------------------ fake KV */

  struct KvWrite {
    std::string type;  // "set" | "delete"
    rt::KvKey key;
  };

  // `JSON.stringify(key)` for the suite's string keys.
  std::string keyOf(const rt::KvKey& key) {
    std::string out = "[";
    for (std::size_t index = 0; index < key.size(); ++index) {
      if (index > 0) out += ",";
      const rt::KvKeyPart& part = key[index];
      out += part.isString() ? rt::jsonQuote(part.string()) : part.toString();
    }
    return out + "]";
  }

  /** In-memory KV with observable reads and injectable failures; `openKv` is undefined here. */
  class ScriptedKv final : public rt::Kv {
  public:
    rt::OrderedMap<std::string, rt::KvValue> values;
    rt::OrderedMap<std::string, double> versions;
    std::vector<std::string> gets;
    std::vector<KvWrite> writes;
    std::vector<KvWrite> commitAttempts;
    int commitFailures = 0;
    bool rejectGets = false;

    using rt::Kv::get;
    using rt::Kv::getMany;

    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override {
      EXPECT(options.consistency == rt::KvConsistencyLevel::strong);  // routing reads must be strong
      const std::string encoded = keyOf(key);
      gets.push_back(encoded);
      if (rejectGets) throw rt::JsError("kv unavailable");
      const rt::KvValue* value = values.get(encoded);
      rt::KvEntryMaybe<> entry;
      entry.key = key;
      entry.value = value != nullptr ? *value : rt::KvValue(nullptr);
      entry.versionstamp = versionstampOf(encoded);
      co_return entry;
    }

    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey>, rt::KvGetOptions = {}) override {
      throw rt::TypeError("kv.getMany is not a function");
    }

    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions = {}) override {
      writes.push_back(KvWrite{"set", key});
      apply("set", key, value);
      rt::KvCommitResult result;
      result.ok = true;
      result.versionstamp = rt::padStart(rt::numberToString(versionOf(keyOf(key)).value_or(0)), 20, "0");
      co_return result;
    }

    rt::Task<void> delete_(rt::KvKey) override { throw rt::TypeError("kv.delete is not a function"); }
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector, rt::KvListOptions = {}) override { throw rt::TypeError("kv.list is not a function"); }
    void close() override {}

    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<Chain>(*this); }

    std::optional<std::string> versionstampOf(const std::string& encoded) const {
      const std::optional<double> version = versionOf(encoded);
      if (!version) return std::nullopt;
      return rt::padStart(rt::numberToString(*version), 20, "0");
    }

    std::optional<double> versionOf(const std::string& encoded) const {
      const double* version = versions.get(encoded);
      return version != nullptr ? std::optional<double>(*version) : std::nullopt;
    }

    void apply(const std::string& type, const rt::KvKey& key, const rt::KvValue& value) {
      const std::string encoded = keyOf(key);
      if (type == "delete") values.delete_(encoded);
      else values.set(encoded, value);
      versions.set(encoded, versionOf(encoded).value_or(0) + 1);
    }

  private:
    class Chain final : public rt::AtomicOperation {
    public:
      explicit Chain(ScriptedKv& owner_) : owner(owner_) {}
      using rt::AtomicOperation::check;
      using rt::AtomicOperation::max;
      using rt::AtomicOperation::min;
      using rt::AtomicOperation::sum;
      rt::AtomicOperation& check(std::vector<rt::AtomicCheck> items) override {
        for (rt::AtomicCheck& item : items) checks.push_back(std::move(item));
        return *this;
      }
      rt::AtomicOperation& mutate(std::vector<rt::KvMutation>) override { throw rt::TypeError("chain.mutate is not a function"); }
      rt::AtomicOperation& sum(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("chain.sum is not a function"); }
      rt::AtomicOperation& min(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("chain.min is not a function"); }
      rt::AtomicOperation& max(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("chain.max is not a function"); }
      rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions = {}) override {
        pending.push_back(Write{"set", std::move(key), std::move(value)});
        return *this;
      }
      rt::AtomicOperation& delete_(rt::KvKey key) override {
        pending.push_back(Write{"delete", std::move(key), rt::KvValue()});
        return *this;
      }
      rt::Task<rt::KvCommitResult> commit() override {
        for (const Write& write : pending) owner.commitAttempts.push_back(KvWrite{write.type, write.key});
        rt::KvCommitResult result;
        if (owner.commitFailures > 0) {
          owner.commitFailures -= 1;
          co_return result;
        }
        for (const rt::AtomicCheck& check : checks) {
          if (owner.versionstampOf(keyOf(check.key)) != check.versionstamp) co_return result;
        }
        for (const Write& write : pending) owner.apply(write.type, write.key, write.value);
        result.ok = true;
        result.versionstamp = "00000000000000000007";
        co_return result;
      }

    private:
      struct Write {
        std::string type;
        rt::KvKey key;
        rt::KvValue value;
      };
      ScriptedKv& owner;
      std::vector<rt::AtomicCheck> checks;
      std::vector<Write> pending;
    };
  };

  /** Runs one test with this stub installed and the routing singletons reset. */
  rt::Task<void> runScripted(rt::Task<void> (*run)(std::shared_ptr<ScriptedKv> kv)) {
    auto kv = std::make_shared<ScriptedKv>();
    uos::kv::setKvForTest(kv);
    capacity_routing::resetCodexAccountRoutingForTest();
    rt::Reason failure;
    try {
      auto running = run(kv);
      co_await std::move(running);
    } catch (...) {
      failure = std::current_exception();
    }
    uos::kv::setKvForTest(nullptr);
    capacity_routing::resetCodexAccountRoutingForTest();
    if (failure) std::rethrow_exception(failure);
  }

  // Runs one test without a KV binding (`setKvForTest(null)`), resetting the routing singletons.
  rt::Task<void> runWithoutKv(rt::Task<void> (*run)()) {
    uos::kv::setKvForTest(nullptr);
    capacity_routing::resetCodexAccountRoutingForTest();
    rt::Reason failure;
    try {
      auto running = run();
      co_await std::move(running);
    } catch (...) {
      failure = std::current_exception();
    }
    uos::kv::setKvForTest(nullptr);
    capacity_routing::resetCodexAccountRoutingForTest();
    if (failure) std::rethrow_exception(failure);
  }

  rt::Task<std::optional<CodexAccountRoutingState>> update(capacity_routing::CodexRoutingTransform transform) {
    auto updating = capacity_routing::updateRoutingState(std::move(transform));
    std::optional<CodexAccountRoutingState> next = co_await std::move(updating);
    co_return next;
  }

  rt::Task<CodexAccountRoutingState> load(CodexAuthPoolState pool) {
    auto loading = capacity_routing::loadCodexAccountRouting(std::move(pool));
    CodexAccountRoutingState state = co_await std::move(loading);
    co_return state;
  }

  rt::Task<std::vector<CodexCapacityRoutingObservation>> loadObservations(CodexAuthPoolState pool, bool forceKv) {
    auto loading = capacity_routing::loadCodexCapacityRoutingObservations(std::move(pool), forceKv);
    std::vector<CodexCapacityRoutingObservation> observations = co_await std::move(loading);
    co_return observations;
  }

  rt::Task<void> record(std::vector<CodexCapacityRoutingObservationInput> inputs, double now) {
    auto recording = capacity_routing::recordCodexCapacityRoutingObservations(std::move(inputs), now);
    co_await std::move(recording);
  }

  rt::Task<void> setRow(std::shared_ptr<ScriptedKv> kv, rt::KvKey key, json::value value) {
    const rt::KvValue stored(value);
    auto setting = kv->set(std::move(key), stored);
    co_await std::move(setting);
  }

  /* ------------------------------------------------------------- pure helpers */

  ASYNC_TEST("slotFor falls back to a neutral slot for an account the state does not describe") {
    const CodexAuthState auth = account("one");
    auto routing = routingAccountOf(auth, 0);
    const RoutingAccount routingAccount = co_await std::move(routing);
    CodexRoutingSlot describedSlot = capacity_routing::neutralSlot(routingAccount.credentialVersion, routingAccount.accountIdHash);
    describedSlot.generation = 4;
    const CodexAccountRoutingState described = stateWith({describedSlot});
    EXPECT_EQ(capacity_routing::slotFor(described, routingAccount).generation, 4.0);

    const CodexAccountRoutingState missing = stateWith({});
    const CodexRoutingSlot neutral = capacity_routing::slotFor(missing, routingAccount);
    EXPECT_EQ(neutral.generation, 0.0);
    EXPECT_EQ(neutral.account_id_hash, std::optional<std::string>(routingAccount.accountIdHash));
    EXPECT_EQ(neutral.credential_version, routingAccount.credentialVersion);
    EXPECT(neutral.quota_blocks_by_class.has_value() && neutral.quota_blocks_by_class->size() == 0);
  }

  ASYNC_TEST("slotMatchesRoutingAccount accepts an exact identity and the legacy null-hash record only") {
    auto routing = routingAccountOf(account("one"), 0);
    const RoutingAccount routingAccount = co_await std::move(routing);
    const CodexRoutingSlot exact = capacity_routing::neutralSlot(routingAccount.credentialVersion, routingAccount.accountIdHash);
    EXPECT_EQ(capacity_routing::slotMatchesRoutingAccount(exact, routingAccount), true);
    const CodexRoutingSlot legacy = capacity_routing::neutralSlot(routingAccount.credentialVersion, std::nullopt);
    EXPECT_EQ(capacity_routing::slotMatchesRoutingAccount(legacy, routingAccount), true);  // a legacy slot without a hash matches on its credential version
    const CodexRoutingSlot rotated = capacity_routing::neutralSlot("some-other-credential", routingAccount.accountIdHash);
    EXPECT_EQ(capacity_routing::slotMatchesRoutingAccount(rotated, routingAccount), false);  // a different credential version never matches
    const CodexRoutingSlot foreign = capacity_routing::neutralSlot(routingAccount.credentialVersion, std::string("another-account-hash"));
    EXPECT_EQ(capacity_routing::slotMatchesRoutingAccount(foreign, routingAccount), false);
  }

  ASYNC_TEST("probeLeaseMatchesRoutingAccount requires a complete claim on the exact account") {
    auto routing = routingAccountOf(account("one"), 0);
    RoutingAccount routingAccount = co_await std::move(routing);
    routingAccount.probeGeneration = 3;
    routingAccount.probeToken = "token-3";
    routingAccount.probeCircuit = std::string("quota");
    routingAccount.requestedModel = std::string("gpt-6-astra");
    routing_state::CodexProbeLease lease;
    lease.generation = 3;
    lease.token = "token-3";
    lease.circuit = "quota";
    lease.quota_class = nullptr;
    lease.expires_at_ms = 1;
    CodexRoutingSlot leased = capacity_routing::neutralSlot(routingAccount.credentialVersion, routingAccount.accountIdHash);
    leased.generation = 3;
    leased.probe_lease = lease;
    EXPECT_EQ(capacity_routing::probeLeaseMatchesRoutingAccount(leased, routingAccount), true);  // a quota lease with no class claim covers the account

    auto plain = routingAccountOf(account("one"), 0);
    const RoutingAccount withoutGeneration = co_await std::move(plain);
    EXPECT_EQ(capacity_routing::probeLeaseMatchesRoutingAccount(leased, withoutGeneration), false);  // an account without a pending generation owns no lease
    CodexRoutingSlot unleased = leased;
    unleased.probe_lease = std::nullopt;
    EXPECT_EQ(capacity_routing::probeLeaseMatchesRoutingAccount(unleased, routingAccount), false);
    CodexRoutingSlot moved = leased;
    moved.generation = 4;
    EXPECT_EQ(capacity_routing::probeLeaseMatchesRoutingAccount(moved, routingAccount), false);  // a slot generation that moved on invalidates the lease
    CodexRoutingSlot stale = leased;
    stale.probe_lease->token = "stale";
    EXPECT_EQ(capacity_routing::probeLeaseMatchesRoutingAccount(stale, routingAccount), false);  // a different lease token belongs to another claim
    CodexRoutingSlot timeoutLease = leased;
    timeoutLease.probe_lease->circuit = "upstream_timeout";
    EXPECT_EQ(capacity_routing::probeLeaseMatchesRoutingAccount(timeoutLease, routingAccount), false);  // a lease for another circuit is not this account's claim
    auto timeoutRouting = routingAccountOf(account("one"), 0);
    RoutingAccount timeoutCircuit = co_await std::move(timeoutRouting);
    timeoutCircuit.probeGeneration = 3;
    timeoutCircuit.probeToken = "token-3";
    timeoutCircuit.probeCircuit = std::string("upstream_timeout");
    EXPECT_EQ(capacity_routing::probeLeaseMatchesRoutingAccount(timeoutLease, timeoutCircuit), true);  // an upstream-timeout circuit carries no model class
    auto defaultRouting = routingAccountOf(account("one"), 0);
    const RoutingAccount defaultCircuit = co_await std::move(defaultRouting);
    EXPECT_EQ(capacity_routing::routingProbeCircuit(defaultCircuit), std::string("quota"));  // a missing circuit means the quota circuit
    CodexRoutingSlot classified = leased;
    classified.probe_lease->quota_class = capacity_routing::quotaClass(std::optional<std::string_view>("gpt-6-astra"));
    EXPECT_EQ(capacity_routing::probeLeaseMatchesRoutingAccount(classified, routingAccount), true);
    auto otherRouting = routingAccountOf(account("one"), 0);
    RoutingAccount otherClass = co_await std::move(otherRouting);
    otherClass.probeGeneration = 3;
    otherClass.probeToken = "token-3";
    otherClass.probeCircuit = std::string("quota");
    otherClass.requestedModel = std::string("gpt-oss-120b");
    EXPECT_EQ(capacity_routing::probeLeaseMatchesRoutingAccount(classified, otherClass), false);  // a lease for another model class does not cover this request
  }

  TEST("withSlot grows the slot list to the requested index and stamps the update") {
    const CodexAccountRoutingState state = stateWith({capacity_routing::neutralSlot("credential-one", std::string("account-one-hash"))});
    const CodexAccountRoutingState grown = capacity_routing::withSlot(state, 2, capacity_routing::neutralSlot("credential-three", std::string("account-three-hash")));
    EXPECT_EQ(grown.slots.size(), std::size_t{3});
    EXPECT_EQ(grown.slots[2].account_id_hash, std::optional<std::string>("account-three-hash"));
    // a gap slot is a neutral filler until the durable state describes it
    EXPECT_EQ(grown.slots[1].account_id_hash, std::optional<std::string>("account-three-hash"));
    EXPECT_EQ(grown.slots[1].generation, 0.0);
    EXPECT(rt::deepEqual(slotJson(grown.slots[0]), slotJson(state.slots[0])));  // existing slot references are preserved
    EXPECT(grown.updated_at_ms > 0);
    const CodexAccountRoutingState replaced = capacity_routing::withSlot(state, 0, capacity_routing::neutralSlot("credential-one", std::string("account-one-hash")));
    EXPECT_EQ(replaced.slots.size(), std::size_t{1});
  }

  TEST("percent parsing and headroom projection ignore unusable values") {
    EXPECT_EQ(capacity_routing::parseFinitePercent(std::nullopt), std::optional<double>());
    EXPECT_EQ(capacity_routing::parseFinitePercent(std::string("42")), std::optional<double>(42));
    EXPECT_EQ(capacity_routing::parseFinitePercent(std::string("abc")), std::optional<double>());
    EXPECT_EQ(capacity_routing::parseFinitePercent(std::string("-1")), std::optional<double>());
    EXPECT_EQ(capacity_routing::parseFinitePercent(std::string("101")), std::optional<double>());
    const json::value available("available");
    const json::value staleState("stale");
    const json::value unavailable("unavailable");
    const json::value unknown("unknown");
    const json::value seven(7);
    EXPECT_EQ(capacity_routing::capacityState(&available), std::optional<std::string>("available"));
    EXPECT_EQ(capacity_routing::capacityState(&staleState), std::optional<std::string>("stale"));
    EXPECT_EQ(capacity_routing::capacityState(&unavailable), std::optional<std::string>("unavailable"));
    EXPECT_EQ(capacity_routing::capacityState(&unknown), std::optional<std::string>());
    EXPECT_EQ(capacity_routing::capacityState(&seven), std::optional<std::string>());

    CodexRoutingSlot both = capacity_routing::neutralSlot("credential-one", std::string("account-one-hash"));
    both.primary_used_percent = 70;
    both.secondary_used_percent = 90;
    EXPECT_EQ(capacity_routing::quotaHeadroomFor(both), std::optional<double>(10));  // the tightest window decides the headroom
    const CodexRoutingSlot empty = capacity_routing::neutralSlot("credential-one", std::string("account-one-hash"));
    EXPECT_EQ(capacity_routing::quotaHeadroomFor(empty), std::optional<double>());
    CodexRoutingSlot negative = empty;
    negative.primary_used_percent = 120;
    EXPECT_EQ(capacity_routing::quotaHeadroomFor(negative), std::optional<double>(0));  // an over-quota window reports no negative headroom
  }

  TEST("class-scoped quota blocks keep the legacy class list and its synthetic unknown fence") {
    const std::string astra = capacity_routing::quotaClass(std::optional<std::string_view>("gpt-6-astra"));
    const CodexRoutingSlot known = slotWithClassBlock(astra, quotaBlock(1'790'000'000));
    const std::optional<CodexQuotaClassBlock> knownBlock = capacity_routing::quotaBlockForClass(known, astra);
    EXPECT(knownBlock.has_value() && knownBlock->blocked_until_ms == 1'790'000'000);
    EXPECT_EQ(capacity_routing::quotaClass(std::optional<std::string_view>("gpt-6-astra")), std::string("standard"));
    EXPECT_EQ(capacity_routing::quotaClass(std::optional<std::string_view>("gpt-oss-120b")), std::string("gpt_oss_120b"));
    // another known class is not covered by this class's fence
    EXPECT(!capacity_routing::quotaBlockForClass(known, capacity_routing::quotaClass(std::optional<std::string_view>("gpt-oss-120b"))).has_value());

    CodexQuotaClassBlock syntheticFence = quotaBlock(1'790'000'100);
    syntheticFence.legacy_fallback = true;
    CodexRoutingSlot migrated = slotWithClassBlock(astra, quotaBlock(1'790'000'000));
    routing_state::CodexQuotaBlocksByClass migratedBlocks;
    migratedBlocks.set(astra, quotaBlock(1'790'000'000));
    migratedBlocks.set("unknown", syntheticFence);
    migrated.quota_blocks_by_class = migratedBlocks;
    migrated.quota_blocked_classes = std::vector<std::string>{astra, "unknown"};
    migrated.quota_blocked_until_ms = syntheticFence.blocked_until_ms;
    // a known-class recovery releases the shared synthetic fence instead of the class entry
    EXPECT_EQ(capacity_routing::quotaBlockKeyForClass(migrated, astra), std::optional<std::string>("unknown"));
    EXPECT_EQ(capacity_routing::quotaBlockKeyForClass(migrated, "unknown"), std::optional<std::string>("unknown"));
    EXPECT_EQ(capacity_routing::quotaBlockKeyForClass(capacity_routing::neutralSlot("credential-one", std::string("account-one-hash")), astra), std::optional<std::string>());

    const CodexRoutingSlot released = capacity_routing::withoutQuotaClass(migrated, astra);
    // the synthetic unknown fence is dropped with the class it mirrors
    EXPECT(released.quota_blocks_by_class.has_value() && released.quota_blocks_by_class->size() == 0);
    EXPECT(released.quota_blocked_classes.has_value() && released.quota_blocked_classes->empty());
    EXPECT_EQ(released.quota_blocked_until_ms, std::optional<double>());
    const routing_state::CodexQuotaBlocksByClass withLegacy = capacity_routing::quotaBlocksIncludingLegacy(migrated);
    const CodexQuotaClassBlock* unknownBlock = withLegacy.get("unknown");
    EXPECT(unknownBlock != nullptr && unknownBlock->blocked_until_ms == syntheticFence.blocked_until_ms);
    EXPECT_EQ(capacity_routing::quotaSignalObservedAtForClass(migrated, astra), syntheticFence.quota_signal_observed_at_ms);
    EXPECT_EQ(capacity_routing::quotaSignalObservedAtForClass(capacity_routing::neutralSlot("credential-one", std::string("account-one-hash")), astra),
              std::optional<double>());
  }

  TEST("recheckQuotaClasses preserves legacy evidence and re-fences every class block") {
    const double recheckAtMs = 1'790'000'500;
    CodexRoutingSlot legacyOnly = capacity_routing::neutralSlot("credential-one", std::string("account-one-hash"));
    legacyOnly.quota_blocked_until_ms = 1'790'000'100;
    legacyOnly.quota_blocked_classes = std::vector<std::string>{"codex"};
    const CodexRoutingSlot recheckedLegacy = capacity_routing::recheckQuotaClasses(legacyOnly, recheckAtMs);
    EXPECT_EQ(recheckedLegacy.quota_blocked_until_ms, std::optional<double>(recheckAtMs));  // a legacy fence is rechecked rather than erased
    EXPECT_EQ(recheckedLegacy.banked_reset_generation_ambiguous, true);                     // rechecking marks the generation as ambiguous
    const CodexRoutingSlot unblocked = capacity_routing::recheckQuotaClasses(capacity_routing::neutralSlot("credential-one", std::string("account-one-hash")), recheckAtMs);
    EXPECT_EQ(unblocked.quota_blocked_until_ms, std::optional<double>());  // an unblocked slot stays unblocked
    EXPECT_EQ(unblocked.banked_reset_generation_ambiguous, false);

    const std::string astra = capacity_routing::quotaClass(std::optional<std::string_view>("gpt-6-astra"));
    CodexQuotaClassBlock block = quotaBlock(1'790'000'100);
    block.observed_reset_at_ms = 1'790'000'200;
    block.observed_reset_at_is_stable = true;
    const CodexRoutingSlot blocked = slotWithClassBlock(astra, block);
    const CodexRoutingSlot rechecked = capacity_routing::recheckQuotaClasses(blocked, recheckAtMs);
    const CodexQuotaClassBlock* recheckedBlock = rechecked.quota_blocks_by_class ? rechecked.quota_blocks_by_class->get(astra) : nullptr;
    ASSERT(recheckedBlock != nullptr);
    EXPECT_EQ(recheckedBlock->blocked_until_ms, recheckAtMs);
    EXPECT_EQ(recheckedBlock->quota_signal_observed_at_ms, std::optional<double>(recheckAtMs));
    EXPECT_EQ(recheckedBlock->banked_reset_generation_ambiguous, true);
    EXPECT_EQ(rechecked.observed_reset_at_ms, std::optional<double>(1'790'000'200));  // the recorded reset evidence survives a recheck
    EXPECT_EQ(rechecked.observed_reset_at_is_stable, true);
  }

  TEST("releaseQuotaClassProbe clears the class fence and keeps pending recovery evidence") {
    const std::string astra = capacity_routing::quotaClass(std::optional<std::string_view>("gpt-6-astra"));
    const CodexRoutingSlot released = capacity_routing::releaseQuotaClassProbe(slotWithClassBlock(astra, quotaBlock(1'790'000'100)), astra);
    EXPECT(released.quota_blocks_by_class.has_value() && released.quota_blocks_by_class->size() == 0);
    EXPECT_EQ(released.banked_reset_generation_ambiguous, false);

    CodexQuotaClassBlock block = quotaBlock(1'790'000'100);
    block.observed_reset_at_ms = 1'790'000'200;
    block.observed_reset_at_is_stable = true;
    CodexRoutingSlot pending = slotWithClassBlock(astra, block);
    pending.banked_reset_recovery_probe_pending = true;
    const CodexRoutingSlot releasedPending = capacity_routing::releaseQuotaClassProbe(pending, astra);
    EXPECT_EQ(releasedPending.observed_reset_at_ms, std::optional<double>(1'790'000'200));  // the pending recovery keeps the observed reset instant
    EXPECT_EQ(releasedPending.observed_reset_at_is_stable, true);
    EXPECT_EQ(releasedPending.banked_reset_generation_ambiguous, true);
  }

  rt::Task<void> legacyTimeoutScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState auth = account("one");
    auto identifying = capacity_routing::routingAccountIdentity(auth);
    const capacity_routing::CodexRoutingAccountIdentity identity = co_await std::move(identifying);
    CodexRoutingSlot fenced = capacity_routing::neutralSlot(identity.credentialVersion, identity.accountIdHash);
    fenced.upstream_timeout_blocked_until_ms = 1'790'000'000;
    auto seeding = setRow(kv, routing_state::CODEX_ACCOUNT_ROUTING_KV_KEY, stateJson(stateWith({fenced})));
    co_await std::move(seeding);
    auto loading = load(poolOf({auth}));
    const CodexAccountRoutingState loaded = co_await std::move(loading);
    EXPECT(loaded.slots[0].upstream_timeout_blocked_until_ms.isNull());  // a durable legacy timeout fence never reaches a decision

    rt::Nullable<double> fenceSeenByTransform;
    capacity_routing::CodexRoutingTransform transform = [&fenceSeenByTransform](const CodexAccountRoutingState& state) -> std::optional<CodexAccountRoutingState> {
      fenceSeenByTransform = state.slots[0].upstream_timeout_blocked_until_ms;
      return std::nullopt;
    };
    auto updating = update(std::move(transform));
    co_await std::move(updating);
    EXPECT(fenceSeenByTransform.isNull());  // a transition always starts from a cleaned base state
  }

  ASYNC_TEST("legacy timeout fences are discarded before any routing decision") {
    auto running = runScripted(&legacyTimeoutScenario);
    co_await std::move(running);
  }

  /* --------------------------------------------------- loading and transitions */

  rt::Task<void> loadWithoutKvScenario() {
    const CodexAuthPoolState pool = poolOf({account("one"), account("two")});
    auto loading = load(pool);
    const CodexAccountRoutingState first = co_await std::move(loading);
    EXPECT_EQ(first.v, 2.0);
    EXPECT_EQ(first.slots.size(), std::size_t{2});  // one slot per pool account
    EXPECT_EQ(first.banked_reset_legacy_identity_unresolved, false);
    EXPECT(first.slots[0].quota_blocks_by_class.has_value() && first.slots[0].quota_blocks_by_class->size() == 0);

    auto reloading = load(pool);
    const CodexAccountRoutingState cached = co_await std::move(reloading);
    EXPECT(rt::deepEqual(stateJson(cached), stateJson(first)));  // the routing cache serves later reads without KV
  }

  ASYNC_TEST("loadCodexAccountRouting normalizes from the auth pool without a KV binding and caches the result") {
    auto running = runWithoutKv(&loadWithoutKvScenario);
    co_await std::move(running);
  }

  rt::Task<void> durableRowScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState auth = account("one");
    auto identifying = capacity_routing::routingAccountIdentity(auth);
    const capacity_routing::CodexRoutingAccountIdentity identity = co_await std::move(identifying);
    CodexRoutingSlot slot = capacity_routing::neutralSlot(identity.credentialVersion, identity.accountIdHash);
    slot.generation = 9;
    auto seeding = setRow(kv, routing_state::CODEX_ACCOUNT_ROUTING_KV_KEY, stateJson(stateWith({slot})));
    co_await std::move(seeding);
    auto loading = load(poolOf({auth}));
    const CodexAccountRoutingState loaded = co_await std::move(loading);
    EXPECT_EQ(loaded.slots[0].generation, 9.0);  // a durable slot is reused instead of rebuilt
    bool readRouting = false;
    for (const std::string& key : kv->gets) readRouting = readRouting || key == keyOf(routing_state::CODEX_ACCOUNT_ROUTING_KV_KEY);
    EXPECT(readRouting);
  }

  rt::Task<void> failedBindingScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthPoolState pool = poolOf({account("one")});
    kv->rejectGets = true;
    auto loading = load(pool);
    const CodexAccountRoutingState failedOpen = co_await std::move(loading);
    EXPECT_EQ(failedOpen.v, 2.0);
    EXPECT_EQ(failedOpen.slots.size(), std::size_t{1});
    // a KV failure still yields a usable neutral state
    EXPECT(failedOpen.slots[0].quota_blocks_by_class.has_value() && failedOpen.slots[0].quota_blocks_by_class->size() == 0);
  }

  ASYNC_TEST("loadCodexAccountRouting reads the durable row and falls open when the binding fails") {
    auto first = runScripted(&durableRowScenario);
    co_await std::move(first);
    auto second = runScripted(&failedBindingScenario);
    co_await std::move(second);
  }

  rt::Task<void> localTransitionScenario() {
    const CodexAuthPoolState pool = poolOf({account("one")});
    auto loading = load(pool);
    const CodexAccountRoutingState base = co_await std::move(loading);
    EXPECT_EQ(base.v, 2.0);  // the transition starts from a loaded routing state
    capacity_routing::CodexRoutingTransform to42 = [](const CodexAccountRoutingState& state) -> std::optional<CodexAccountRoutingState> {
      CodexAccountRoutingState next = state;
      next.updated_at_ms = 42;
      return next;
    };
    auto first = update(std::move(to42));
    const std::optional<CodexAccountRoutingState> next = co_await std::move(first);
    ASSERT(next.has_value());
    EXPECT_EQ(next->updated_at_ms, 42.0);

    capacity_routing::CodexRoutingTransform decline = [](const CodexAccountRoutingState&) -> std::optional<CodexAccountRoutingState> { return std::nullopt; };
    auto second = update(std::move(decline));
    const std::optional<CodexAccountRoutingState> unchanged = co_await std::move(second);
    ASSERT(unchanged.has_value());
    EXPECT_EQ(unchanged->updated_at_ms, 42.0);  // a transform that declines reports the current state

    capacity_routing::CodexRoutingTransform to99 = [](const CodexAccountRoutingState& state) -> std::optional<CodexAccountRoutingState> {
      CodexAccountRoutingState replaced = state;
      replaced.updated_at_ms = 99;
      return replaced;
    };
    auto third = update(std::move(to99));
    const std::optional<CodexAccountRoutingState> replacement = co_await std::move(third);
    ASSERT(replacement.has_value());
    EXPECT_EQ(replacement->updated_at_ms, 99.0);

    capacity_routing::resetCodexAccountRoutingForTest();
    capacity_routing::CodexRoutingTransform identity = [](const CodexAccountRoutingState& state) -> std::optional<CodexAccountRoutingState> { return state; };
    auto fourth = update(std::move(identity));
    const std::optional<CodexAccountRoutingState> refused = co_await std::move(fourth);
    EXPECT(!refused.has_value());  // a transition without any known state is refused
  }

  ASYNC_TEST("updateRoutingState applies a local transition when there is no KV binding") {
    auto running = runWithoutKv(&localTransitionScenario);
    co_await std::move(running);
  }

  rt::Task<void> cachedStampScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthPoolState pool = poolOf({account("one")});
    auto loading = load(pool);
    co_await std::move(loading);
    capacity_routing::CodexRoutingTransform to7 = [](const CodexAccountRoutingState& state) -> std::optional<CodexAccountRoutingState> {
      CodexAccountRoutingState next = state;
      next.updated_at_ms = 7;
      return next;
    };
    auto first = update(std::move(to7));
    const std::optional<CodexAccountRoutingState> committed = co_await std::move(first);
    ASSERT(committed.has_value());
    EXPECT_EQ(committed->updated_at_ms, 7.0);
    const rt::KvValue* durableRow = kv->values.get(keyOf(routing_state::CODEX_ACCOUNT_ROUTING_KV_KEY));
    ASSERT(durableRow != nullptr);
    json::value durable = durableRow->toJson();
    EXPECT_EQ(rt::getNumber(durable, "updated_at_ms"), std::optional<double>(7));  // the transition reached the durable row

    // A concurrent writer moves the row on: the cached stamp loses and the
    // strong re-read retry commits the transition on top of the new state.
    durable.as_object()["updated_at_ms"] = 11;
    auto moving = setRow(kv, routing_state::CODEX_ACCOUNT_ROUTING_KV_KEY, durable);
    co_await std::move(moving);
    capacity_routing::CodexRoutingTransform increment = [](const CodexAccountRoutingState& state) -> std::optional<CodexAccountRoutingState> {
      CodexAccountRoutingState next = state;
      next.updated_at_ms = state.updated_at_ms + 1;
      return next;
    };
    auto second = update(std::move(increment));
    const std::optional<CodexAccountRoutingState> retried = co_await std::move(second);
    ASSERT(retried.has_value());
    EXPECT_EQ(retried->updated_at_ms, 12.0);  // the retry applies the transition to the durable state
  }

  rt::Task<void> exhaustedCasScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthPoolState pool = poolOf({account("one")});
    auto loading = load(pool);
    co_await std::move(loading);
    kv->commitFailures = 20;
    capacity_routing::CodexRoutingTransform to5 = [](const CodexAccountRoutingState& state) -> std::optional<CodexAccountRoutingState> {
      CodexAccountRoutingState next = state;
      next.updated_at_ms = 5;
      return next;
    };
    auto updating = update(std::move(to5));
    const std::optional<CodexAccountRoutingState> result = co_await std::move(updating);
    EXPECT(!result.has_value());                    // an exhausted compare-and-set reports the conflict
    EXPECT(kv->commitAttempts.size() >= 3);         // every attempt is a checked compare-and-set
  }

  ASYNC_TEST("updateRoutingState commits with the cached versionstamp, survives a lost race and reports exhaustion") {
    auto first = runScripted(&cachedStampScenario);
    co_await std::move(first);
    auto second = runScripted(&exhaustedCasScenario);
    co_await std::move(second);
  }

  TEST("capacity observations are parsed defensively from the durable store") {
    const json::value valid = observationJson(observationOf(0, "account-one-hash"));
    const auto storeOf = [](json::value version, json::value observations) {
      json::object store;
      store["v"] = std::move(version);
      store["observations"] = std::move(observations);
      return json::value(std::move(store));
    };
    const auto parsedJson = [](const std::vector<CodexCapacityRoutingObservation>& parsed) {
      json::array out;
      for (const CodexCapacityRoutingObservation& observation : parsed) out.push_back(observationJson(observation));
      return json::value(std::move(out));
    };
    EXPECT(rt::deepEqual(parsedJson(capacity_routing::parseStoredCapacityObservationStore(storeOf(1, json::array{valid}))), json::array{valid}));
    // an unknown store version is ignored
    EXPECT(capacity_routing::parseStoredCapacityObservationStore(storeOf(2, json::array{valid})).empty());
    EXPECT(capacity_routing::parseStoredCapacityObservationStore(storeOf(1, "not-an-array")).empty());
    EXPECT(capacity_routing::parseStoredCapacityObservationStore(json::value(nullptr)).empty());
    const auto variant = [&valid](std::string_view key, json::value member) {
      json::value out = valid;
      out.as_object()[key] = std::move(member);
      return out;
    };
    json::array rejected{variant("slot", 3), variant("slot", 0.5), variant("account_id_hash", ""), variant("state", "bogus"), variant("snapshot_at_ms", -1),
                         variant("windows", nullptr), json::value("not-an-observation")};
    // an observation missing any required field is dropped rather than guessed at
    EXPECT(capacity_routing::parseStoredCapacityObservationStore(storeOf(1, rejected)).empty());

    const json::value lenient = rt::jsonParse(
      R"({"v":1,"observations":[{"slot":1,"account_id_hash":"account-two-hash","state":"available","source_observed_at_ms":null,"snapshot_at_ms":1790000000000,)"
      R"("windows":{"primary":5,"secondary":{"limit_window_seconds":600,"used_percent":200,"reset_at_ms":"later"}},)"
      R"("additional_rate_limits":[{"limit_name":"","windows":{}},{"limit_name":"gpt-6","windows":null},{"limit_name":"gpt-6","windows":{},"metered_feature":7}]}]})");
    const std::vector<CodexCapacityRoutingObservation> parsed = capacity_routing::parseStoredCapacityObservationStore(lenient);
    json::array projected;
    for (const CodexCapacityRoutingObservation& observation : parsed) {
      json::object entry;
      entry["windows"] = json::value(routing_state::toJson(observation.windows));
      json::array limits;
      for (const auto& limit : observation.additional_rate_limits) limits.push_back(json::value(routing_state::toJson(limit)));
      entry["limits"] = std::move(limits);
      projected.push_back(std::move(entry));
    }
    // a non-object window becomes null, an out-of-range percent is dropped and unusable extra limits are discarded
    EXPECT(rt::deepEqual(json::value(projected),
                         rt::jsonParse(R"([{"windows":{"primary":null,"secondary":{"limit_window_seconds":600,"used_percent":null,"reset_at_ms":null}},"limits":[]}])")));
  }

  TEST("capacity headroom follows the requested model's own rate-limit windows") {
    CodexCapacityRoutingObservation observation = observationOf(0, "account-one-hash");
    observation.windows.primary = window(300, 25, std::nullopt);
    observation.windows.secondary = std::nullopt;
    routing_state::CodexCapacityRoutingAdditionalRateLimit astra;
    astra.limit_name = "gpt-6-astra";
    astra.metered_feature = std::nullopt;
    astra.windows.primary = window(300, 80, std::nullopt);
    astra.windows.secondary = std::nullopt;
    routing_state::CodexCapacityRoutingAdditionalRateLimit luna;
    luna.limit_name = "gpt-6-luna";
    luna.metered_feature = std::nullopt;
    observation.additional_rate_limits = {astra, luna};
    EXPECT_EQ(capacity_routing::capacityHeadroomForObservation(observation, std::nullopt), std::optional<double>(75));  // without a model the account-wide windows decide
    EXPECT_EQ(capacity_routing::capacityHeadroomForObservation(observation, std::string_view("gpt-6-astra")), std::optional<double>(20));
    EXPECT_EQ(capacity_routing::capacityHeadroomForObservation(observation, std::string_view("gpt-6-luna")), std::optional<double>());
    EXPECT_EQ(capacity_routing::capacityHeadroomForObservation(observation, std::string_view("unlisted-model")), std::optional<double>(75));

    const double now = 1'790'000'100'000;
    CodexCapacityRoutingObservation fresh = observation;
    fresh.snapshot_at_ms = now - 1'000;
    EXPECT_EQ(capacity_routing::capacityObservationIsFresh(fresh, now), true);
    CodexCapacityRoutingObservation future = observation;
    future.snapshot_at_ms = now + 1;
    EXPECT_EQ(capacity_routing::capacityObservationIsFresh(future, now), false);  // an observation from the future is not fresh
    CodexCapacityRoutingObservation old = observation;
    old.snapshot_at_ms = now - routing_state::CODEX_CAPACITY_ROUTING_MAX_AGE_MS - 1;
    EXPECT_EQ(capacity_routing::capacityObservationIsFresh(old, now), false);
    CodexCapacityRoutingObservation unavailable = observation;
    unavailable.state = "unavailable";
    unavailable.snapshot_at_ms = now;
    EXPECT_EQ(capacity_routing::capacityObservationIsFresh(unavailable, now), false);
  }

  /* ------------------------------------- observation persistence and merging */

  const double NOW_MS = 1'790'000'000'000;

  rt::Task<void> protectOldPrimaryReset(std::shared_ptr<ScriptedKv> kv) {
    constexpr double day = 86'400'000;
    const auto auth = account("one");
    auto readingAccount = routingAccountOf(auth, 0);
    auto routed = co_await std::move(readingAccount);
    routed.requestedModel = std::string("gpt-6-astra");
    auto sample = observationOf(0, routed.accountIdHash);
    sample.windows.primary = window(18'000, 25, NOW_MS + 60'000);
    sample.windows.secondary = window(604'800, 100, NOW_MS + 5 * day);
    const auto accountHash = co_await uos::utils::sha256Hex(auth.account_id);
    namespace reset = uos::codex::banked_reset;

    for (const auto& [state, oldDeadline] : std::vector<std::pair<std::string, double>>{
           {"submitted", NOW_MS + 60'000}, {"unknown", NOW_MS - 60'000}}) {
      auto primary = quotaBlock(oldDeadline);
      primary.quota_signal_observed_at_ms = NOW_MS - day;
      primary.observed_reset_at_ms = oldDeadline;
      primary.observed_reset_at_is_stable = true;
      auto slot = slotWithClassBlock("standard", primary);
      slot.account_id_hash = routed.accountIdHash;
      slot.observed_reset_at_ms = oldDeadline;
      slot.observed_reset_at_is_stable = true;
      const json::value oldRouting = stateJson(stateWith({slot}));
      auto settingRouting = setRow(kv, routing_state::CODEX_ACCOUNT_ROUTING_KV_KEY, oldRouting);
      co_await std::move(settingRouting);
      const auto generationHash = co_await uos::utils::sha256Hex(reset::codexResetQuotaGenerationInput(accountHash, oldDeadline));
      const auto ledgerKey = reset::codexResetRedemptionKey(accountHash, "v1:" + generationHash);
      const json::object oldLedger{{"v", 1}, {"account_id_hash", accountHash}, {"quota_generation", "v1:" + generationHash},
                                   {"state", state}, {"submitted_at_ms", NOW_MS - day}};
      auto settingLedger = setRow(kv, ledgerKey, oldLedger);
      co_await std::move(settingLedger);
      const auto writesBefore = kv->commitAttempts.size();

      const bool materialized = co_await capacity_routing::reconcileCapacityQuotaBlock(routed, sample, NOW_MS + 1);
      EXPECT(!materialized);
      EXPECT_EQ(kv->commitAttempts.size(), writesBefore);
      EXPECT(rt::deepEqual(kv->values.get(keyOf(routing_state::CODEX_ACCOUNT_ROUTING_KV_KEY))->toJson(), oldRouting));
      EXPECT(rt::deepEqual(kv->values.get(keyOf(ledgerKey))->toJson(), json::value(oldLedger)));

      kv->apply("delete", ledgerKey, rt::KvValue(nullptr));
      const bool unspent = co_await capacity_routing::reconcileCapacityQuotaBlock(routed, sample, NOW_MS + 1);
      EXPECT(unspent);
    }
  }

  ASYNC_TEST("weekly capacity cannot replace a submitted or unknown primary reset across UTC days") {
    auto running = runScripted(&protectOldPrimaryReset);
    co_await std::move(running);
  }

  rt::Task<void> preserveNamedQuotaScope(std::shared_ptr<ScriptedKv> kv) {
    namespace decisions = uos::authority::native_codex_capacity;
    namespace evaluation = uos::authority::native_codex_evaluation;
    const auto auth = account("one");
    auto readingAccount = routingAccountOf(auth, 0);
    auto routed = co_await std::move(readingAccount);
    const std::string named = "gpt-6-astra";
    const std::string other = "gpt-6-luna";
    routed.requestedModel = named;
    const auto slot = capacity_routing::neutralSlot(routed.credentialVersion, routed.accountIdHash);
    auto settingRouting = setRow(kv, routing_state::CODEX_ACCOUNT_ROUTING_KV_KEY, stateJson(stateWith({slot})));
    co_await std::move(settingRouting);

    auto sample = observationOf(0, routed.accountIdHash);
    const double weekly = NOW_MS + 604'800'000;
    sample.windows.primary = window(18'000, 25, NOW_MS + 60'000);
    sample.additional_rate_limits.push_back({named, std::nullopt, {std::nullopt, window(604'800, 100, weekly)}});
    rt::OrderedMap<std::string, CodexCapacityRoutingObservation> observations;
    observations.set(routed.accountIdHash, sample);
    const routing_state::CodexRoutingIdentity identity{.slot = 0, .accountIdHash = routed.accountIdHash,
                                                        .credentialVersion = routed.credentialVersion};
    auto evaluate = [&](const CodexAccountRoutingState& state, const std::string& model) {
      return evaluation::evaluateCodexRoutingAccount(state, identity, model, observations, NOW_MS + 1, {});
    };
    const auto before = evaluate(stateWith({slot}), named);
    ASSERT(before.blockedAccount);
    EXPECT_EQ(before.blockedAccount->quotaResetAtMs, weekly);
    const bool materialized = co_await capacity_routing::reconcileCapacityQuotaBlock(routed, sample, NOW_MS + 1);
    ASSERT(materialized);
    const auto* stored = kv->values.get(keyOf(routing_state::CODEX_ACCOUNT_ROUTING_KV_KEY));
    ASSERT(stored);
    const auto durable = routing_state::parseCodexAccountRoutingState(stored->toJson());
    ASSERT(durable);
    EXPECT(!durable->slots[0].quota_blocked_until_ms);
    EXPECT(!decisions::quotaBlockForClass(durable->slots[0], "standard"));
    const auto healthy = evaluate(*durable, other);
    ASSERT(healthy.routedAccount);
    EXPECT(!healthy.routedAccount->probeRequired);
    EXPECT_EQ(healthy.routedAccount->quotaHeadroom, std::optional<double>(75));
    const auto repeated = evaluate(*durable, named);
    ASSERT(repeated.blockedAccount);
    EXPECT_EQ(repeated.blockedAccount->quotaResetAtMs, weekly);
    const auto namedBlock = decisions::quotaBlockForClass(durable->slots[0], "standard", named);
    ASSERT(namedBlock);
    EXPECT_EQ(namedBlock->observed_reset_at_ms, std::optional<double>(weekly));
    EXPECT(namedBlock->observed_reset_at_is_stable);
    EXPECT(!namedBlock->banked_reset_generation_ambiguous);
  }

  ASYNC_TEST("named exhaustion keeps an exact reset fence without blocking a healthy same-class model") {
    auto running = runScripted(&preserveNamedQuotaScope);
    co_await std::move(running);
  }

  CodexCapacityRoutingObservationInput observationInputOf(double slot, const std::string& accountId) {
    CodexCapacityRoutingObservationInput input;
    input.slot = slot;
    input.account_id = accountId;
    input.state = "available";
    input.source_observed_at_ms = std::nullopt;
    input.snapshot_at_ms = NOW_MS - 5'000;
    input.windows.primary = window(300, 12, std::nullopt);
    input.windows.secondary = std::nullopt;
    return input;
  }

  json::array storedObservationsOf(const ScriptedKv& kv) {
    const rt::KvValue* store = kv.values.get(keyOf(routing_state::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY));
    if (store == nullptr) return json::array{};
    const json::value value = store->toJson();
    const json::array* observations = rt::getArray(value, "observations");
    return observations != nullptr ? *observations : json::array{};
  }

  rt::Task<void> recordStoresHashesScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState one = account("one");
    const CodexAuthPoolState pool = poolOf({one, account("two")});
    std::vector<CodexCapacityRoutingObservationInput> firstInputs{observationInputOf(0, "one")};
    auto recording = record(std::move(firstInputs), NOW_MS);
    co_await std::move(recording);
    const json::array stored = storedObservationsOf(*kv);
    ASSERT_EQ(stored.size(), std::size_t{1});
    EXPECT_EQ(rt::getNumber(stored[0], "slot"), std::optional<double>(0));
    auto hashing = accountHashOf(one);
    const std::string oneHash = co_await std::move(hashing);
    EXPECT_EQ(rt::getString(stored[0], "account_id_hash"), std::optional<std::string_view>(oneHash));  // the durable row is bound to the opaque account hash
    const rt::KvValue* storeRow = kv->values.get(keyOf(routing_state::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY));
    ASSERT(storeRow != nullptr);
    EXPECT_EQ(rt::includes(rt::jsonStringify(storeRow->toJson()), "one"), false);  // the raw account id never reaches the durable row

    const std::size_t writesAfterFirst = kv->writes.size();
    std::vector<CodexCapacityRoutingObservationInput> rejected{observationInputOf(3, "one"), observationInputOf(0.5, "one"), observationInputOf(0, "   ")};
    CodexCapacityRoutingObservationInput negative = observationInputOf(0, "one");
    negative.snapshot_at_ms = -1;
    rejected.push_back(negative);
    CodexCapacityRoutingObservationInput bogus = observationInputOf(0, "one");
    bogus.state = "bogus";
    rejected.push_back(bogus);
    auto recordingRejected = record(std::move(rejected), NOW_MS);
    co_await std::move(recordingRejected);
    EXPECT_EQ(kv->writes.size(), writesAfterFirst);  // an unusable observation is dropped before it can be persisted
    EXPECT_EQ(storedObservationsOf(*kv).size(), std::size_t{1});

    auto loading = loadObservations(pool, true);
    const std::vector<CodexCapacityRoutingObservation> merged = co_await std::move(loading);
    ASSERT_EQ(merged.size(), std::size_t{1});  // a later read sees exactly the one accepted observation
    EXPECT_EQ(std::optional<std::string_view>(merged[0].account_id_hash), rt::getString(stored[0], "account_id_hash"));
  }

  ASYNC_TEST("recordCodexCapacityRoutingObservations stores only account-hash-bound rows and refuses unusable input") {
    auto running = runScripted(&recordStoresHashesScenario);
    co_await std::move(running);
  }

  rt::Task<void> lostCasScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthPoolState pool = poolOf({account("one")});
    kv->commitFailures = 20;
    std::vector<CodexCapacityRoutingObservationInput> inputs{observationInputOf(0, "one")};
    auto recording = record(std::move(inputs), NOW_MS);
    co_await std::move(recording);
    EXPECT_EQ(storedObservationsOf(*kv).size(), std::size_t{0});  // a lost compare-and-set persists nothing
    auto loading = loadObservations(pool, true);
    const std::vector<CodexCapacityRoutingObservation> loaded = co_await std::move(loading);
    EXPECT(loaded.empty());  // and this isolate does not pretend it was durable
  }

  rt::Task<void> inMemoryScenario() {
    const CodexAuthPoolState pool = poolOf({account("one")});
    std::vector<CodexCapacityRoutingObservationInput> inputs{observationInputOf(0, "one")};
    auto recording = record(std::move(inputs), NOW_MS);
    co_await std::move(recording);
    auto loading = loadObservations(pool, false);
    const std::vector<CodexCapacityRoutingObservation> cached = co_await std::move(loading);
    EXPECT_EQ(cached.size(), std::size_t{1});  // without a KV binding the sampler result is still visible in this isolate
    auto forcing = loadObservations(pool, true);
    const std::vector<CodexCapacityRoutingObservation> forced = co_await std::move(forcing);
    ASSERT_EQ(forced.size(), std::size_t{1});  // a forced read without a binding keeps the in-memory observations
    EXPECT_EQ(forced[0].slot, 0.0);
  }

  ASYNC_TEST("capacity observations are kept in memory without KV and are never adopted after a lost compare-and-set") {
    auto first = runScripted(&lostCasScenario);
    co_await std::move(first);
    auto second = runWithoutKv(&inMemoryScenario);
    co_await std::move(second);
  }

  rt::Task<void> legacyMergeScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState one = account("one");
    const CodexAuthPoolState pool = poolOf({one});
    const double durableSnapshot = NOW_MS - 20'000;
    auto hashing = accountHashOf(one);
    const std::string oneHash = co_await std::move(hashing);
    CodexCapacityRoutingObservation durableObservation = observationOf(0, oneHash);
    durableObservation.snapshot_at_ms = durableSnapshot;
    durableObservation.windows.primary = window(300, 90, std::nullopt);
    durableObservation.windows.secondary = std::nullopt;
    json::object store;
    store["v"] = 1;
    store["updated_at_ms"] = durableSnapshot;
    store["observations"] = json::array{observationJson(durableObservation)};
    auto seedingStore = setRow(kv, routing_state::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY, json::value(store));
    co_await std::move(seedingStore);
    const double legacySnapshot = NOW_MS - 1'000;
    json::value snapshot = rt::jsonParse(
      R"({"sources":[{"source":"codex","slot":1,"state":"available","windows":{"primary":{"limit_window_seconds":300,"used_percent":30,"reset_at_ms":null}}},)"
      R"({"source":"codex","slot":2,"state":"available","windows":{"primary":{"limit_window_seconds":300,"used_percent":5,"reset_at_ms":null}}},)"
      R"({"source":"codex","slot":1,"state":"bogus","windows":{"primary":null}},{"source":"metered","slot":1,"state":"available","windows":{"primary":null}}]})");
    json::object snapshotObject;
    snapshotObject["snapshot_at_ms"] = legacySnapshot;
    snapshotObject["sources"] = snapshot.as_object()["sources"];
    auto seedingSnapshot = setRow(kv, uos::provider::capacity_contract::PROVIDER_CAPACITY_SNAPSHOT_KEY, json::value(snapshotObject));
    co_await std::move(seedingSnapshot);

    auto loading = loadObservations(pool, true);
    const std::vector<CodexCapacityRoutingObservation> merged = co_await std::move(loading);
    ASSERT_EQ(merged.size(), std::size_t{1});  // both sources describe the same account, so the newest sample wins
    EXPECT_EQ(merged[0].slot, 0.0);
    EXPECT_EQ(merged[0].account_id_hash, oneHash);  // a legacy slot-only snapshot is re-bound to the current account
    EXPECT_EQ(merged[0].snapshot_at_ms, legacySnapshot);  // the newer legacy sample is adopted
    EXPECT(merged[0].windows.primary.has_value() && merged[0].windows.primary->used_percent == std::optional<double>(30));
  }

  rt::Task<void> olderSnapshotScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState one = account("one");
    const CodexAuthPoolState newerPool{.accounts = {one}, .updated_at_ms = NOW_MS};
    json::value snapshot = rt::jsonParse(
      R"({"snapshot_at_ms":0,"sources":[{"source":"codex","slot":1,"state":"available","windows":{"primary":{"limit_window_seconds":300,"used_percent":30,"reset_at_ms":null}}}]})");
    snapshot.as_object()["snapshot_at_ms"] = NOW_MS - 60'000;
    auto seeding = setRow(kv, uos::provider::capacity_contract::PROVIDER_CAPACITY_SNAPSHOT_KEY, snapshot);
    co_await std::move(seeding);
    auto loading = loadObservations(newerPool, true);
    const std::vector<CodexCapacityRoutingObservation> loaded = co_await std::move(loading);
    EXPECT(loaded.empty());  // a snapshot older than the auth pool can no longer be attributed to a slot
  }

  rt::Task<void> malformedSnapshotScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthPoolState pool = poolOf({account("one")});
    json::object snapshot;
    snapshot["snapshot_at_ms"] = NOW_MS;
    snapshot["sources"] = "not-an-array";
    auto seeding = setRow(kv, uos::provider::capacity_contract::PROVIDER_CAPACITY_SNAPSHOT_KEY, json::value(snapshot));
    co_await std::move(seeding);
    auto loading = loadObservations(pool, true);
    const std::vector<CodexCapacityRoutingObservation> loaded = co_await std::move(loading);
    EXPECT(loaded.empty());
    kv->rejectGets = true;
    auto failing = loadObservations(pool, true);
    const std::vector<CodexCapacityRoutingObservation> failed = co_await std::move(failing);
    EXPECT(failed.empty());  // a failing binding yields no observations instead of throwing
  }

  ASYNC_TEST("the durable observation store merges with the legacy provider snapshot by newest sample") {
    auto first = runScripted(&legacyMergeScenario);
    co_await std::move(first);
    auto second = runScripted(&olderSnapshotScenario);
    co_await std::move(second);
    auto third = runScripted(&malformedSnapshotScenario);
    co_await std::move(third);
  }

  rt::Task<void> reconcileScenario(std::shared_ptr<ScriptedKv>) {
    const CodexAuthState one = account("one");
    const CodexAuthState two = account("two");
    const CodexAuthPoolState pool = poolOf({one, two});
    auto loading = load(pool);
    const CodexAccountRoutingState base = co_await std::move(loading);
    const double now = NOW_MS;
    auto hashingOne = accountHashOf(one);
    const std::string oneHash = co_await std::move(hashingOne);
    auto hashingTwo = accountHashOf(two);
    const std::string twoHash = co_await std::move(hashingTwo);
    CodexCapacityRoutingObservation matching = observationOf(0, oneHash);
    matching.snapshot_at_ms = now - 1'000;
    matching.windows.primary = window(300, 40, std::nullopt);
    matching.windows.secondary = window(60, 10, std::nullopt);

    CodexCapacityRoutingObservation future = matching;
    future.snapshot_at_ms = now + 1;
    CodexCapacityRoutingObservation otherSlot = matching;
    otherSlot.slot = 5;
    CodexCapacityRoutingObservation otherAccount = matching;
    otherAccount.account_id_hash = twoHash;
    std::vector<CodexCapacityRoutingObservation> observations{future, otherSlot, otherAccount, matching};
    auto reconciling = capacity_routing::reconcileCapacityRoutingState(base, std::move(observations), now, std::nullopt);
    const CodexAccountRoutingState reconciled = co_await std::move(reconciling);
    EXPECT_EQ(reconciled.slots[0].primary_used_percent, std::optional<double>(40));  // the matching observation updates its own slot
    EXPECT_EQ(reconciled.slots[0].secondary_used_percent, std::optional<double>(10));
    EXPECT_EQ(reconciled.slots[0].capacity_observed_at_ms, std::optional<double>(now - 1'000));
    EXPECT_EQ(reconciled.slots[0].generation, base.slots[0].generation + 1);  // a positive observation advances the slot generation
    EXPECT(rt::deepEqual(slotJson(reconciled.slots[1]), slotJson(base.slots[1])));  // another account's slot is untouched

    CodexCapacityRoutingObservation older = matching;
    older.snapshot_at_ms = now - 2'000;
    older.windows.primary = window(300, 5, std::nullopt);
    older.windows.secondary = std::nullopt;
    std::vector<CodexCapacityRoutingObservation> olderOnly{older};
    auto ignoring = capacity_routing::reconcileCapacityRoutingState(reconciled, std::move(olderOnly), now, std::nullopt);
    const CodexAccountRoutingState noChange = co_await std::move(ignoring);
    EXPECT(rt::deepEqual(stateJson(noChange), stateJson(reconciled)));  // an observation older than the slot's last sample is ignored
    EXPECT_EQ(noChange.slots[0].primary_used_percent, std::optional<double>(40));
  }

  ASYNC_TEST("reconcileCapacityRoutingState applies only observations that still describe the durable slot") {
    auto running = runScripted(&reconcileScenario);
    co_await std::move(running);
  }

  rt::Task<void> recordReconcilesScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState one = account("one");
    const CodexAuthPoolState pool = poolOf({one});
    auto loading = load(pool);
    const CodexAccountRoutingState base = co_await std::move(loading);
    EXPECT_EQ(base.slots.size(), std::size_t{1});  // the cached slot is the one the recorded observation must reconcile
    auto hashing = accountHashOf(one);
    const std::string accountHash = co_await std::move(hashing);
    CodexCapacityRoutingObservationInput input = observationInputOf(0, "one");
    input.snapshot_at_ms = NOW_MS - 1'000;
    input.windows.primary = window(300, 55, std::nullopt);
    input.windows.secondary = std::nullopt;
    std::vector<CodexCapacityRoutingObservationInput> inputs{input};
    auto recording = record(std::move(inputs), NOW_MS);
    co_await std::move(recording);

    std::optional<double> cachedPercent;
    capacity_routing::CodexRoutingTransform peek = [&cachedPercent](const CodexAccountRoutingState& state) -> std::optional<CodexAccountRoutingState> {
      cachedPercent = state.slots[0].primary_used_percent;
      return std::nullopt;
    };
    auto updating = update(std::move(peek));
    co_await std::move(updating);
    EXPECT_EQ(cachedPercent, std::optional<double>(55));  // the dashboard refresh reconciles the cached slot immediately
    auto rehashing = accountHashOf(one);
    const std::string again = co_await std::move(rehashing);
    EXPECT_EQ(again, accountHash);
    const json::array stored = storedObservationsOf(*kv);
    ASSERT_EQ(stored.size(), std::size_t{1});
    EXPECT_EQ(rt::getNumber(stored[0], "snapshot_at_ms"), std::optional<double>(NOW_MS - 1'000));
  }

  ASYNC_TEST("a recorded observation reconciles the cached routing slot before the next inference") {
    auto running = runScripted(&recordReconcilesScenario);
    co_await std::move(running);
  }
}

