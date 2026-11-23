// Port of tests/native-codex-conformance.test.ts: the shared vectors serialVectors()
// (src/authority/native-codex.ts selectCodex), probeVectors() (src/authority/native-codex-mutations.ts)
// and capacityVectors(), and the single TS test case's assertions on them.
#include "uos/authority/native_codex.hpp"
#include "uos/authority/native_codex_capacity.hpp"
#include "uos/authority/native_codex_contracts.hpp"
#include "uos/authority/native_codex_mutations.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace contracts = uos::authority::native_codex_contracts;
namespace capacity = uos::authority::native_codex_capacity;
namespace codex = uos::authority::native_codex;
namespace mutations = uos::authority::native_codex_mutations;
using contracts::CodexAccountRoutingState;
using contracts::CodexCapacityRoutingObservation;
using contracts::CodexRoutingIdentity;
using contracts::CodexRoutingSlot;

namespace {
  // BEGIN SHARED_CODEX_VECTORS
  constexpr double fixtureNow = 1'700'000'000'000;
  const std::vector<CodexRoutingIdentity> identities = {
    {0, std::string(64, 'a'), std::string(64, 'b')},
    {1, std::string(64, 'c'), std::string(64, 'd')},
  };
  void same(const json::value& actual, const json::value& expected, const std::string& label) {
    if (rt::jsonStringify(actual) != rt::jsonStringify(expected)) throw std::runtime_error("Codex conformance: " + label);
  }
  template <class T> T present(const std::optional<T>& value, const std::string& label) {
    if (!value) throw std::runtime_error("Codex conformance missing " + label);
    return *value;
  }
  json::value numberOrNull(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }
  CodexAccountRoutingState stateOf(std::vector<CodexRoutingSlot> slots) {
    CodexAccountRoutingState state;
    state.v = 2;
    state.updated_at_ms = fixtureNow;
    state.banked_reset_legacy_identity_unresolved = false;
    state.slots = std::move(slots);
    return state;
  }
  CodexAccountRoutingState neutralState() {
    std::vector<CodexRoutingSlot> slots;
    for (const CodexRoutingIdentity& identity : identities) slots.push_back(capacity::neutralSlot(identity.credentialVersion, identity.accountIdHash));
    return stateOf(std::move(slots));
  }
  CodexRoutingSlot blockedSlot(std::size_t index, double deadline = fixtureNow + 60'000) {
    const CodexRoutingIdentity& identity = identities[index];
    const contracts::CodexQuotaClassBlock block{deadline, "body_resets_at", false, fixtureNow, deadline, true, false, false};
    CodexRoutingSlot slot = capacity::neutralSlot(identity.credentialVersion, identity.accountIdHash);
    slot.generation = 3;
    slot.quota_blocked_until_ms = deadline;
    slot.quota_block_source = block.source;
    slot.quota_blocked_classes = std::vector<std::string>{"standard"};
    contracts::CodexQuotaBlocksByClass blocks;
    blocks.set("standard", block);
    slot.quota_blocks_by_class = std::move(blocks);
    slot.quota_signal_observed_at_ms = fixtureNow;
    slot.observed_reset_at_ms = deadline;
    slot.observed_reset_at_is_stable = true;
    return slot;
  }
  json::value nullableNumber(const rt::Nullable<double>& value) { return value.toJson().value_or(json::value(nullptr)); }
  json::value leaseOrNull(const std::optional<contracts::CodexProbeLease>& lease) { return lease ? json::value(contracts::toJson(*lease)) : json::value(nullptr); }

  // `snapshotOf(routing = null, active = null)`; the rows are passed as the JSON the TS objects are.
  codex::CodexSerialSnapshot snapshotOf(const json::value& routing = nullptr, const json::value& active = nullptr) {
    codex::CodexSerialSnapshot snapshot;
    snapshot.active = active;
    snapshot.routing = routing;
    snapshot.capacity = nullptr;
    snapshot.poolIdentities = identities;
    for (const CodexRoutingIdentity& identity : identities) snapshot.selectedAccountHashes.push_back(identity.accountIdHash);
    for (const CodexRoutingIdentity& identity : identities)
      snapshot.accountPolicies.set(identity.accountIdHash, contracts::CodexRoutingAccountPolicy{.modelUnavailable = std::nullopt, .overageUsageAllowed = false});
    snapshot.revisions.active = active.is_null() ? std::nullopt : std::optional<std::string>("A/opaque:7");
    snapshot.revisions.pool = "P/opaque:11";
    snapshot.revisions.routing = routing.is_null() ? std::nullopt : std::optional<std::string>("R/opaque:13");
    snapshot.revisions.capacity = std::nullopt;
    return snapshot;
  }
  contracts::CodexRoutingAccount eligibleAccount(const contracts::CodexSerialAdmissionPlan& plan) {
    const auto* eligible = std::get_if<contracts::CodexRouteSelectionEligible>(&plan.selection);
    if (eligible == nullptr) throw std::runtime_error("Codex conformance expected eligible account");
    if (eligible->accounts.empty()) throw std::runtime_error("Codex conformance missing selected account");
    return eligible->accounts[0];
  }

  // JSON.stringify of the serial plans, as the TS `same` and the credential check read them.
  json::value textOrNull(const std::optional<std::string>& value) { return value ? json::value(*value) : json::value(nullptr); }
  json::value activeOrNull(const std::optional<contracts::CodexActiveAccountSelection>& active) {
    return active ? json::value(contracts::toJson(*active)) : json::value(nullptr);
  }
  json::object accountJson(const contracts::CodexRoutingAccount& account) {
    json::object out;
    out["slot"] = rt::makeNumber(account.slot);
    out["accountIdHash"] = account.accountIdHash;
    out["credentialVersion"] = account.credentialVersion;
    out["quotaHeadroom"] = numberOrNull(account.quotaHeadroom);
    out["probeRequired"] = account.probeRequired;
    out["probeGeneration"] = numberOrNull(account.probeGeneration);
    out["probeToken"] = textOrNull(account.probeToken);
    rt::objectDefine(out, "probeCircuit", account.probeCircuit);
    rt::objectDefine(out, "requestedModel", account.requestedModel);
    rt::objectDefine(out, "routingGeneration", account.routingGeneration);
    rt::objectDefine(out, "overageFallback", account.overageFallback);
    rt::objectDefine(out, "activeGeneration", account.activeGeneration);
    rt::objectDefine(out, "activePoolVersionstamp", account.activePoolVersionstamp);
    rt::objectDefine(out, "activeTransitionReason", account.activeTransitionReason);
    return out;
  }
  json::array numbersJson(const std::vector<double>& values) {
    json::array out;
    for (double value : values) out.push_back(rt::makeNumber(value));
    return out;
  }
  json::array blockedJson(const std::vector<contracts::CodexBlockedAccountMetadata>& accounts) {
    json::array out;
    for (const contracts::CodexBlockedAccountMetadata& account : accounts) {
      json::object row = accountJson(account);
      row["quotaResetAtMs"] = rt::makeNumber(account.quotaResetAtMs);
      out.push_back(std::move(row));
    }
    return out;
  }
  std::string kindOf(const contracts::CodexRouteSelection& selection) {
    return std::visit([](const auto& alternative) { return std::string(std::decay_t<decltype(alternative)>::kind); }, selection);
  }
  json::value selectionJson(const contracts::CodexRouteSelection& selection) {
    json::object out;
    out["kind"] = kindOf(selection);
    if (const auto* eligible = std::get_if<contracts::CodexRouteSelectionEligible>(&selection)) {
      json::array accounts;
      for (const contracts::CodexRoutingAccount& account : eligible->accounts) accounts.push_back(accountJson(account));
      out["accounts"] = std::move(accounts);
      out["skippedSlots"] = numbersJson(eligible->skippedSlots);
      out["blockedAccounts"] = blockedJson(eligible->blockedAccounts);
    } else if (const auto* blocked = std::get_if<contracts::CodexRouteSelectionQuotaBlocked>(&selection)) {
      out["skippedSlots"] = numbersJson(blocked->skippedSlots);
      out["retryAtMs"] = numberOrNull(blocked->retryAtMs);
      out["blockedAccounts"] = blockedJson(blocked->blockedAccounts);
      out["fullCohortExhausted"] = blocked->fullCohortExhausted;
      rt::objectDefine(out, "fullCohortUnavailable", blocked->fullCohortUnavailable);
      json::object snapshot;
      snapshot["selection"] = activeOrNull(blocked->activeSnapshot.selection);
      out["activeSnapshot"] = std::move(snapshot);
      out["poolSnapshotJson"] = textOrNull(blocked->poolSnapshotJson);
      out["capacitySnapshotJson"] = textOrNull(blocked->capacitySnapshotJson);
    } else if (const auto* upstream = std::get_if<contracts::CodexRouteSelectionUpstreamBlocked>(&selection)) {
      out["skippedSlots"] = numbersJson(upstream->skippedSlots);
      out["retryAtMs"] = numberOrNull(upstream->retryAtMs);
      out["blockedAccounts"] = blockedJson(upstream->blockedAccounts);
    } else if (const auto* model = std::get_if<contracts::CodexRouteSelectionModelUnavailable>(&selection)) {
      out["model"] = model->model;
      out["detail"] = textOrNull(model->detail);
      out["skippedSlots"] = numbersJson(model->skippedSlots);
    } else if (const auto* invalid = std::get_if<contracts::CodexRouteSelectionCredentialsInvalid>(&selection)) {
      out["skippedSlots"] = numbersJson(invalid->skippedSlots);
      rt::objectDefine(out, "fullCohortUnavailable", invalid->fullCohortUnavailable);
    }
    return out;
  }
  json::value checksJson(const contracts::CodexRoutingRowRevisions& checks) {
    json::object out;
    out["active"] = textOrNull(checks.active);
    out["pool"] = textOrNull(checks.pool);
    out["routing"] = textOrNull(checks.routing);
    out["capacity"] = textOrNull(checks.capacity);
    return out;
  }
  json::value writesJson(const contracts::CodexSerialAdmissionWrites& writes) {
    json::object out;
    out["routing"] = writes.routing ? json::value(contracts::toJson(*writes.routing)) : json::value(nullptr);
    out["active"] = activeOrNull(writes.active);
    return out;
  }
  json::value planJson(const contracts::CodexSerialAdmissionPlan& plan) {
    json::object out;
    out["kind"] = std::string(contracts::CodexSerialAdmissionPlan::kind);
    out["checks"] = checksJson(plan.checks);
    out["writes"] = writesJson(plan.writes);
    out["selection"] = selectionJson(plan.selection);
    return out;
  }

  struct SerialVectors {
    contracts::CodexSerialAdmissionPlan bootstrap;
    contracts::CodexSerialAdmissionPlan hot;
    contracts::CodexSerialAdmissionPlan changedCapacity;
    contracts::CodexSerialAdmissionPlan reduced;
    contracts::CodexSerialAdmissionPlan reordered;
    contracts::CodexSerialAdmissionPlan refreshed;
    contracts::CodexSerialAdmissionPlan advance;
    contracts::CodexSerialAdmissionPlan exhausted;
    contracts::CodexSerialAdmissionPlan mixed;
    contracts::CodexSerialAdmissionPlan leaseBlocked;
    contracts::CodexSerialAdmissionPlan unavailable;
  };

  SerialVectors serialVectors() {
    const contracts::CodexSerialAdmissionPlan bootstrap = present(codex::selectCodex(snapshotOf(), fixtureNow, "gpt-test"), "bootstrap");
    const contracts::CodexRoutingAccount account = eligibleAccount(bootstrap);
    same(json::array{rt::makeNumber(account.slot), numberOrNull(account.activeGeneration),
                     bootstrap.writes.active ? textOrNull(bootstrap.writes.active->transition_reason) : json::value(nullptr)},
         json::array{0, 1, nullptr}, "deterministic bootstrap");
    const CodexAccountRoutingState routing = present(bootstrap.writes.routing, "bootstrap routing");
    const contracts::CodexActiveAccountSelection active = present(bootstrap.writes.active, "bootstrap active");
    const json::value routingRow = contracts::toJson(routing);
    const json::value activeRow = contracts::toJson(active);
    const contracts::CodexSerialAdmissionPlan hot = present(codex::selectCodex(snapshotOf(routingRow, activeRow), fixtureNow + 10, "gpt-other"), "hot admission");
    same(writesJson(hot.writes), rt::jsonParse(R"({"routing":null,"active":null})"), "hot admission checks without writes");
    json::object manualActive = contracts::toJson(active);
    manualActive["generation"] = rt::makeNumber(active.generation + 1);
    manualActive["transition_reason"] = "manual_reset";
    const contracts::CodexActiveAccountSelection manualParsed =
      present(contracts::parseCodexActiveAccountSelection(json::value(manualActive)), "manual reset active codec");
    const contracts::CodexSerialAdmissionPlan manual =
      present(codex::selectCodex(snapshotOf(routingRow, contracts::toJson(manualParsed)), fixtureNow + 10, "gpt-other"), "manual priority hot admission");
    same(json::array{activeOrNull(manual.writes.active), eligibleAccount(manual).activeTransitionReason.toJson().value_or(json::value(nullptr))},
         json::array{nullptr, "manual_reset"}, "manual promotion survives native parser and ordinary admission");
    same(checksJson(hot.checks), rt::jsonParse(R"({"active":"A/opaque:7","pool":"P/opaque:11","routing":"R/opaque:13","capacity":null})"), "all four opaque revisions");
    codex::CodexSerialSnapshot conflicting = snapshotOf(routingRow, activeRow);
    conflicting.revisions = hot.checks;
    conflicting.revisions.capacity = "C/conflict:17";
    const contracts::CodexSerialAdmissionPlan changedCapacity = present(codex::selectCodex(conflicting, fixtureNow + 10, "gpt-test"), "conflicting capacity snapshot");
    same(textOrNull(changedCapacity.checks.capacity), json::value("C/conflict:17"), "conflict reload uses new C fence");
    same(writesJson(changedCapacity.writes), writesJson(hot.writes), "conflict replay stays check-only");
    codex::CodexSerialSnapshot reducedSnapshot = snapshotOf();
    reducedSnapshot.selectedAccountHashes = {identities[1].accountIdHash};
    const contracts::CodexSerialAdmissionPlan reduced = present(codex::selectCodex(reducedSnapshot, fixtureNow, "gpt-test"), "reduced cohort");
    same(json::array{rt::makeNumber(eligibleAccount(reduced).slot),
                     reduced.writes.routing ? rt::makeNumber(static_cast<double>(reduced.writes.routing->slots.size())) : json::value(nullptr)},
         json::array{1, 2}, "selected cohort keeps full-pool slots");
    CodexRoutingIdentity first = identities[1];
    first.slot = 0;
    CodexRoutingIdentity second = identities[0];
    second.slot = 1;
    codex::CodexSerialSnapshot reorderedSnapshot = snapshotOf(routingRow, activeRow);
    reorderedSnapshot.poolIdentities = {first, second};
    const contracts::CodexSerialAdmissionPlan reordered = present(codex::selectCodex(reorderedSnapshot, fixtureNow + 10, "gpt-test"), "reorder");
    same(json::array{rt::makeNumber(eligibleAccount(reordered).slot), numberOrNull(eligibleAccount(reordered).activeGeneration)}, json::array{1, 1},
         "reorder preserves sticky identity and generation");
    CodexRoutingIdentity rotatedIdentity = identities[0];
    rotatedIdentity.credentialVersion = std::string(64, 'e');
    codex::CodexSerialSnapshot refreshedSnapshot = snapshotOf(routingRow, activeRow);
    refreshedSnapshot.poolIdentities = {rotatedIdentity, identities[1]};
    const contracts::CodexSerialAdmissionPlan refreshed = present(codex::selectCodex(refreshedSnapshot, fixtureNow + 20, "gpt-test"), "same-account rotation");
    same(json::array{numberOrNull(eligibleAccount(refreshed).activeGeneration),
                     refreshed.writes.active ? rt::makeNumber(refreshed.writes.active->routing_generation) : json::value(nullptr)},
         json::array{1, 1}, "same-account refresh retains active generation");
    const json::value quotaState = contracts::toJson(stateOf({blockedSlot(0), routing.slots[1]}));
    const contracts::CodexSerialAdmissionPlan advance = present(codex::selectCodex(snapshotOf(quotaState, activeRow), fixtureNow, "gpt-test"), "quota advance");
    same(json::array{rt::makeNumber(eligibleAccount(advance).slot), advance.writes.active ? rt::makeNumber(advance.writes.active->generation) : json::value(nullptr),
                     advance.writes.active ? textOrNull(advance.writes.active->transition_reason) : json::value(nullptr)},
         json::array{1, 2, "quota_exhausted"}, "authoritative quota advances once");
    const contracts::CodexSerialAdmissionPlan exhausted =
      present(codex::selectCodex(snapshotOf(contracts::toJson(stateOf({blockedSlot(0), blockedSlot(1)})), activeRow), fixtureNow, "gpt-test"), "full cohort");
    const auto* exhaustedSelection = std::get_if<contracts::CodexRouteSelectionQuotaBlocked>(&exhausted.selection);
    if (exhaustedSelection == nullptr) throw std::runtime_error("Codex conformance expected exhausted quota cohort");
    same(json::array{exhaustedSelection->fullCohortExhausted, exhaustedSelection->fullCohortUnavailable ? json::value(*exhaustedSelection->fullCohortUnavailable) : json::value(nullptr)},
         json::array{true, true}, "quota and unavailability proofs");
    CodexRoutingSlot invalid = routing.slots[1];
    invalid.invalid_credential_version = identities[1].credentialVersion;
    const contracts::CodexSerialAdmissionPlan mixed =
      present(codex::selectCodex(snapshotOf(contracts::toJson(stateOf({blockedSlot(0), invalid})), activeRow), fixtureNow, "gpt-test"), "mixed cohort");
    const auto* mixedSelection = std::get_if<contracts::CodexRouteSelectionQuotaBlocked>(&mixed.selection);
    if (mixedSelection == nullptr) throw std::runtime_error("Codex conformance expected mixed quota cohort");
    same(json::array{mixedSelection->fullCohortExhausted, mixedSelection->fullCohortUnavailable ? json::value(*mixedSelection->fullCohortUnavailable) : json::value(nullptr)},
         json::array{false, true}, "invalid credential never authorizes quota reset");
    CodexRoutingSlot held = routing.slots[0];
    contracts::CodexProbeLease heldLease;
    heldLease.token = "held";
    heldLease.expires_at_ms = fixtureNow + 30'000;
    heldLease.generation = 0;
    heldLease.circuit = "quota";
    heldLease.quota_class = std::string("standard");
    held.probe_lease = heldLease;
    const contracts::CodexSerialAdmissionPlan leaseBlocked =
      present(codex::selectCodex(snapshotOf(contracts::toJson(stateOf({held, routing.slots[1]})), activeRow), fixtureNow, "gpt-test"), "held recovery lease");
    same(json::array{kindOf(leaseBlocked.selection), activeOrNull(leaseBlocked.writes.active)}, json::array{"routing_unavailable", nullptr},
         "lease cannot elect sibling or open paid fallback");
    codex::CodexSerialSnapshot unavailableSnapshot = snapshotOf(routingRow, activeRow);
    unavailableSnapshot.accountPolicies = {};
    for (const CodexRoutingIdentity& identity : identities)
      unavailableSnapshot.accountPolicies.set(identity.accountIdHash,
                                              contracts::CodexRoutingAccountPolicy{.modelUnavailable = contracts::CodexRoutingAccountModelUnavailable{.detail = "synthetic entitlement"},
                                                                                   .overageUsageAllowed = false});
    const contracts::CodexSerialAdmissionPlan unavailable = present(codex::selectCodex(unavailableSnapshot, fixtureNow, "gpt-test"), "model entitlement");
    same(json::value(kindOf(unavailable.selection)), json::value("model_unavailable"), "model refusal is separate from quota");
    codex::CodexSerialSnapshot malformedActive = snapshotOf();
    malformedActive.active = rt::jsonParse(R"({"v":1,"generation":"malformed"})");
    same(json::value(codex::selectCodex(malformedActive, fixtureNow, "gpt-test").has_value() ? json::value("plan") : json::value(nullptr)), json::value(nullptr),
         "malformed A refuses");
    codex::CodexSerialSnapshot malformedRouting = snapshotOf();
    malformedRouting.routing = rt::jsonParse(R"({"v":2,"slots":"malformed"})");
    same(json::value(codex::selectCodex(malformedRouting, fixtureNow, "gpt-test").has_value() ? json::value("plan") : json::value(nullptr)), json::value(nullptr),
         "malformed R refuses");
    codex::CodexSerialSnapshot missingPool = snapshotOf();
    missingPool.revisions = hot.checks;
    missingPool.revisions.pool = std::nullopt;
    same(json::value(codex::selectCodex(missingPool, fixtureNow, "gpt-test").has_value() ? json::value("plan") : json::value(nullptr)), json::value(nullptr),
         "missing opaque pool revision refuses");
    const std::string serialized = rt::jsonStringify(planJson(bootstrap));
    same(json::value(serialized.find("access_token") != std::string::npos || serialized.find("refresh_token") != std::string::npos ||
                     serialized.find("\"auth\"") != std::string::npos),
         json::value(false), "pure DTOs carry no credentials");
    return SerialVectors{bootstrap, hot, changedCapacity, reduced, reordered, refreshed, advance, exhausted, mixed, leaseBlocked, unavailable};
  }

  struct ProbeVectors {
    mutations::CodexExpiredProbeClaim claim;
    mutations::CodexExpiredProbeClaim reclaimed;
    CodexAccountRoutingState success;
    CodexAccountRoutingState rotated;
    CodexAccountRoutingState recovery;
    CodexRoutingSlot rechecked;
    CodexAccountRoutingState timeout;
    CodexAccountRoutingState normalized;
  };

  ProbeVectors probeVectors() {
    const contracts::CodexSerialAdmissionPlan bootstrap = present(codex::selectCodex(snapshotOf(), fixtureNow, "gpt-test"), "probe bootstrap");
    contracts::CodexRoutingAccount account = eligibleAccount(bootstrap);
    account.probeRequired = true;
    account.probeCircuit = std::string("quota");
    const CodexAccountRoutingState expired = stateOf({blockedSlot(0, fixtureNow - 1), neutralState().slots[1]});
    const mutations::CodexExpiredProbeClaim claim =
      present(mutations::buildExpiredProbeClaim(expired, account, fixtureNow, "probe-one", fixtureNow + 17), "expired claim");
    same(json::array{rt::makeNumber(claim.lease.expires_at_ms), rt::makeNumber(claim.lease.generation), rt::makeNumber(claim.next.updated_at_ms)},
         json::array{rt::makeNumber(fixtureNow + 30'000), 3, rt::makeNumber(fixtureNow + 17)}, "lease timing and separate mutation clock");
    same(json::value(mutations::buildExpiredProbeClaim(claim.next, account, fixtureNow + 29'999, "competing", fixtureNow).has_value()), json::value(false),
         "live lease refuses competing claim");
    const mutations::CodexExpiredProbeClaim reclaimed =
      present(mutations::buildExpiredProbeClaim(claim.next, account, fixtureNow + 30'000, "probe-two", fixtureNow + 30'001), "lease boundary reclaim");
    same(json::value(reclaimed.lease.token), json::value("probe-two"), "lease expires at exact deadline");
    contracts::CodexRoutingAccount claimedAccount = account;
    claimedAccount.probeRequired = false;
    claimedAccount.probeGeneration = claim.lease.generation;
    claimedAccount.probeToken = claim.lease.token;
    contracts::CodexRoutingAccount lateAccount = claimedAccount;
    lateAccount.probeToken = "late-other";
    same(json::value(mutations::markCodexSuccess(claim.next, lateAccount, fixtureNow + 20).has_value()), json::value(false), "stale probe cannot clear circuit");
    const CodexAccountRoutingState success = present(mutations::markCodexSuccess(claim.next, claimedAccount, fixtureNow + 21), "probe success");
    same(json::array{numberOrNull(success.slots[0].quota_blocked_until_ms), leaseOrNull(success.slots[0].probe_lease), success.slots[0].banked_reset_generation_ambiguous},
         json::array{nullptr, nullptr, false}, "successful matching probe clears recovery scope");
    CodexRoutingIdentity rotation = identities[0];
    rotation.credentialVersion = std::string(64, 'e');
    const CodexAccountRoutingState rotated =
      present(mutations::reconcileRoutingCredential(claim.next, claimedAccount, rotation, true, fixtureNow + 22), "credential rotation");
    const std::optional<contracts::CodexProbeLease>& rotatedLease = rotated.slots[0].probe_lease;
    same(json::array{rt::makeNumber(rotated.slots[0].generation), rotatedLease ? rt::makeNumber(rotatedLease->generation) : json::value(nullptr),
                     rotatedLease ? json::value(rotatedLease->token) : json::value(nullptr)},
         json::array{4, 4, "probe-one"}, "same-account refresh transfers exact lease");
    same(json::value(mutations::markCodexCredentialInvalid(rotated, claimedAccount, fixtureNow, fixtureNow).has_value()), json::value(false),
         "old credential completion cannot invalidate replacement");
    const CodexAccountRoutingState fenced = stateOf({blockedSlot(0), neutralState().slots[1]});
    contracts::CodexRoutingAccount fencedAccount = account;
    fencedAccount.routingGeneration = 3;
    const json::value fencedRow = contracts::toJson(fenced);
    same(json::value(mutations::isCodexQuotaBlockFenceCurrent(fencedRow, fencedAccount, fixtureNow + 60'000, 3)), json::value(true), "stable exact reset fence");
    same(json::value(mutations::isCodexQuotaBlockFenceCurrent(fencedRow, fencedAccount, fixtureNow + 59'999, 3)), json::value(false),
         "shorter later deadline cannot authorize reset");
    contracts::CodexProbeLease recoveryLease;
    recoveryLease.token = "recovery";
    recoveryLease.expires_at_ms = fixtureNow + 30'000;
    recoveryLease.generation = 4;
    recoveryLease.circuit = "quota";
    recoveryLease.quota_class = std::string("standard");
    const CodexAccountRoutingState recovery = mutations::recoveryProbeTransition(fenced, fencedAccount, recoveryLease, fixtureNow + 23);
    same(json::array{numberOrNull(recovery.slots[0].quota_blocked_until_ms), numberOrNull(recovery.slots[0].observed_reset_at_ms),
                     recovery.slots[0].banked_reset_generation_ambiguous, recovery.slots[0].banked_reset_recovery_probe_pending},
         json::array{nullptr, rt::makeNumber(fixtureNow + 60'000), true, true}, "verified reset stays lookup-only until successful recovery");
    const CodexRoutingSlot rechecked = mutations::recheckSlotTransition(blockedSlot(0), fixtureNow + 24);
    same(json::array{rt::makeNumber(rechecked.generation), rechecked.banked_reset_generation_ambiguous, numberOrNull(rechecked.observed_reset_at_ms)},
         json::array{4, true, rt::makeNumber(fixtureNow + 60'000)}, "admin recheck preserves ambiguous reset identity");
    const CodexAccountRoutingState timeout =
      present(mutations::markCodexUpstreamTimeout(neutralState(), account, fixtureNow, fixtureNow + 25), "legacy timeout transition");
    same(nullableNumber(timeout.slots[0].upstream_timeout_blocked_until_ms), rt::makeNumber(fixtureNow + 60'000), "legacy timeout duration");
    const CodexAccountRoutingState normalized = capacity::normalizeRoutingState(&timeout, identities, fixtureNow + 26, true);
    same(json::array{nullableNumber(normalized.slots[0].upstream_timeout_blocked_until_ms), rt::makeNumber(normalized.slots[0].generation)}, json::array{nullptr, 2},
         "normalization discards legacy timeout fences");
    return ProbeVectors{claim, reclaimed, success, rotated, recovery, rechecked, timeout, normalized};
  }

  struct CapacityVectors {
    CodexAccountRoutingState cleared;
    CodexAccountRoutingState retained;
    CodexAccountRoutingState pendingObserved;
    capacity::CodexCapacityObservationStorePlan store;
    std::optional<CodexAccountRoutingState> parser;
  };

  CapacityVectors capacityVectors() {
    CodexCapacityRoutingObservation observation;
    observation.slot = 0;
    observation.account_id_hash = identities[0].accountIdHash;
    observation.state = "available";
    observation.source_observed_at_ms = fixtureNow;
    observation.snapshot_at_ms = fixtureNow + 1;
    observation.windows = {contracts::CodexCapacityRoutingWindow{3600, 10, fixtureNow + 60'000}, std::nullopt};
    observation.additional_rate_limits = {contracts::CodexCapacityRoutingAdditionalRateLimit{
      "gpt-5.3-codex-spark", std::nullopt, {contracts::CodexCapacityRoutingWindow{3600, 100, fixtureNow + 60'000}, std::nullopt}}};
    const CodexAccountRoutingState base = stateOf({blockedSlot(0), neutralState().slots[1]});
    const CodexAccountRoutingState cleared = present(capacity::applyCapacityObservation(base, observation, fixtureNow + 2, "gpt-test", fixtureNow + 31), "positive capacity");
    same(json::array{numberOrNull(cleared.slots[0].quota_blocked_until_ms), rt::makeNumber(cleared.updated_at_ms)}, json::array{nullptr, rt::makeNumber(fixtureNow + 31)},
         "fresh positive class capacity uses injected write clock");
    CodexCapacityRoutingObservation older = observation;
    older.snapshot_at_ms = fixtureNow - 1;
    const CodexAccountRoutingState retained = present(capacity::applyCapacityObservation(base, older, fixtureNow + 2, "gpt-test", fixtureNow + 32), "older positive sample");
    same(numberOrNull(retained.slots[0].quota_blocked_until_ms), rt::makeNumber(fixtureNow + 60'000), "newer inference quota remains authoritative");
    same(numberOrNull(capacity::capacityHeadroomForObservation(observation, "gpt-5.3-codex-spark")), rt::makeNumber(0.0), "requested model class uses its own capacity");
    CodexCapacityRoutingObservation wrongAccount = observation;
    wrongAccount.account_id_hash = identities[1].accountIdHash;
    same(json::value(capacity::applyCapacityObservation(base, wrongAccount, fixtureNow + 2, "gpt-test", fixtureNow).has_value()), json::value(false),
         "wrong account observation refuses");
    CodexCapacityRoutingObservation future = observation;
    future.snapshot_at_ms = fixtureNow + 3;
    same(json::value(capacity::applyCapacityObservation(base, future, fixtureNow + 2, "gpt-test", fixtureNow).has_value()), json::value(false), "future observation refuses");
    CodexRoutingSlot pending = base.slots[0];
    pending.banked_reset_generation_ambiguous = true;
    pending.banked_reset_recovery_probe_pending = true;
    const CodexAccountRoutingState pendingObserved =
      present(capacity::applyCapacityObservation(stateOf({pending, base.slots[1]}), observation, fixtureNow + 2, "gpt-test", fixtureNow + 33), "pending reset capacity");
    same(json::array{pendingObserved.slots[0].banked_reset_generation_ambiguous, numberOrNull(pendingObserved.slots[0].observed_reset_at_ms)},
         json::array{true, rt::makeNumber(fixtureNow + 60'000)}, "capacity cannot clear pending reset safety");
    CodexCapacityRoutingObservation old = observation;
    old.account_id_hash = std::string(64, 'f');
    old.snapshot_at_ms = fixtureNow - contracts::CODEX_CAPACITY_ROUTING_MAX_AGE_MS - 1;
    CodexCapacityRoutingObservation atBoundary = observation;
    atBoundary.account_id_hash = identities[1].accountIdHash;
    atBoundary.snapshot_at_ms = fixtureNow - contracts::CODEX_CAPACITY_ROUTING_MAX_AGE_MS;
    CodexCapacityRoutingObservation current = observation;
    current.snapshot_at_ms = fixtureNow;
    const capacity::CodexCapacityObservationStorePlan store = capacity::planCapacityObservationStore("C/opaque:19", {old, atBoundary}, {current}, fixtureNow);
    const std::vector<CodexCapacityRoutingObservation>& rows = store.writes.capacity.observations;
    const bool keptOld = std::any_of(rows.begin(), rows.end(), [&old](const CodexCapacityRoutingObservation& row) { return row.account_id_hash == old.account_id_hash; });
    same(json::array{json::value(*store.checks.capacity), rt::makeNumber(static_cast<double>(rows.size())), keptOld}, json::array{"C/opaque:19", 2, false},
         "absolute observation retention boundary");
    const std::optional<CodexAccountRoutingState> parser = contracts::parseCodexAccountRoutingState(json::value(contracts::toJson(base)));
    same(parser ? rt::makeNumber(static_cast<double>(parser->slots.size())) : json::value(nullptr), rt::makeNumber(2.0), "routing codec preserves full pool");
    same(json::value(contracts::parseCodexActiveAccountSelection(rt::jsonParse(R"({"v":1,"account_id_hash":"not-an-opaque-hash"})")).has_value()), json::value(false),
         "active codec refuses malformed identity");
    return CapacityVectors{cleared, retained, pendingObserved, store, parser};
  }
  // END SHARED_CODEX_VECTORS
}

TEST("native Codex pure boundaries preserve serial fences, probe leases, reset safety and capacity provenance") {
  const SerialVectors serial = serialVectors();
  const ProbeVectors probes = probeVectors();
  const CapacityVectors vectors = capacityVectors();
  EXPECT(!serial.hot.writes.active.has_value());
  EXPECT(serial.reduced.writes.active.has_value() && serial.reduced.writes.active->slot == 1);
  EXPECT_EQ(probes.claim.lease.expires_at_ms, fixtureNow + 30'000);
  EXPECT_EQ(vectors.store.writes.capacity.observations.size(), std::size_t{2});
}
