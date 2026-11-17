// Differential cases for src/authority/native-codex-evaluation.ts (no direct bun test), recorded from
// the TypeScript under Bun 1.4.0: evaluateCodexRoutingAccount over every branch, folding and
// classification of whole cohorts, routingStateChangedByNormalization (the TS compares slot objects
// by identity; the port compares values) and, through selectCodex, the serial decisions of
// src/authority/native-codex.ts that consume them.
#include "uos/authority/native_codex.hpp"
#include "uos/authority/native_codex_capacity.hpp"
#include "uos/authority/native_codex_contracts.hpp"
#include "uos/authority/native_codex_evaluation.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "native_codex_evaluation_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
namespace contracts = uos::authority::native_codex_contracts;
namespace capacity = uos::authority::native_codex_capacity;
namespace evaluation = uos::authority::native_codex_evaluation;
namespace codex = uos::authority::native_codex;

namespace {
  const json::value& vectors() {
    static const json::value parsed = rt::jsonParse(kEvaluationVectors);
    return parsed;
  }
  const json::array& section(std::string_view name) { return *rt::getArray(vectors(), name); }
  const json::value& at(const json::value& value, std::string_view key) {
    static const json::value null;
    const json::value* member = rt::get(value, key);
    return member != nullptr ? *member : null;
  }
  std::string text(const json::value& value, std::string_view key) { return std::string(*rt::getString(value, key)); }
  double number(const json::value& value, std::string_view key) { return *rt::getNumber(value, key); }
  std::optional<std::string> textOrNull(const json::value& value) {
    if (value.is_null()) return std::nullopt;
    return std::string(value.as_string());
  }

  // ---- inputs ------------------------------------------------------------------------------
  contracts::CodexRoutingIdentity identityOf(const json::value& value) {
    return {.slot = number(value, "slot"), .accountIdHash = text(value, "accountIdHash"), .credentialVersion = text(value, "credentialVersion")};
  }
  std::vector<contracts::CodexRoutingIdentity> identitiesOf(const json::value& value) {
    std::vector<contracts::CodexRoutingIdentity> identities;
    for (const json::value& item : value.as_array()) identities.push_back(identityOf(item));
    return identities;
  }
  contracts::CodexRoutingAccountPolicy policyOf(const json::value& value) {
    contracts::CodexRoutingAccountPolicy policy;
    const json::value& unavailable = at(value, "modelUnavailable");
    if (!unavailable.is_null()) policy.modelUnavailable = contracts::CodexRoutingAccountModelUnavailable{.detail = textOrNull(at(unavailable, "detail"))};
    policy.overageUsageAllowed = at(value, "overageUsageAllowed").as_bool();
    return policy;
  }
  contracts::CodexAccountRoutingState stateOf(const json::value& value) {
    std::optional<contracts::CodexAccountRoutingState> parsed = contracts::parseCodexAccountRoutingState(value);
    if (!parsed) throw std::runtime_error("vector state does not parse");
    return *parsed;
  }
  rt::OrderedMap<std::string, contracts::CodexCapacityRoutingObservation> observationsOf(const json::value& rows) {
    json::object store;
    store["v"] = 1;
    store["updated_at_ms"] = rt::makeNumber(number(vectors(), "now"));
    store["observations"] = rows;
    rt::OrderedMap<std::string, contracts::CodexCapacityRoutingObservation> observations;
    for (const contracts::CodexCapacityRoutingObservation& observation : capacity::parseStoredCapacityObservationStore(json::value(store)))
      observations.set(observation.account_id_hash, observation);
    return observations;
  }

  // ---- results as JSON.stringify writes them -------------------------------------------------
  json::value numberOrNull(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }
  json::value textOrNullJson(const std::optional<std::string>& value) { return value ? json::value(*value) : json::value(nullptr); }

  json::object accountJson(const contracts::CodexRoutingAccount& account) {
    json::object out;
    out["slot"] = rt::makeNumber(account.slot);
    out["accountIdHash"] = account.accountIdHash;
    out["credentialVersion"] = account.credentialVersion;
    out["quotaHeadroom"] = numberOrNull(account.quotaHeadroom);
    out["probeRequired"] = account.probeRequired;
    out["probeGeneration"] = numberOrNull(account.probeGeneration);
    out["probeToken"] = textOrNullJson(account.probeToken);
    rt::objectDefine(out, "probeCircuit", account.probeCircuit);
    rt::objectDefine(out, "overageFallback", account.overageFallback);
    rt::objectDefine(out, "routingGeneration", account.routingGeneration);
    rt::objectDefine(out, "activeGeneration", account.activeGeneration);
    rt::objectDefine(out, "activePoolVersionstamp", account.activePoolVersionstamp);
    rt::objectDefine(out, "activeTransitionReason", account.activeTransitionReason);
    rt::objectDefine(out, "requestedModel", account.requestedModel);
    return out;
  }
  json::object blockedJson(const contracts::CodexBlockedAccountMetadata& account) {
    json::object out = accountJson(account);
    out["quotaResetAtMs"] = rt::makeNumber(account.quotaResetAtMs);
    return out;
  }
  json::value unavailableJson(const std::optional<evaluation::CodexRoutingAccountEvaluation::ModelUnavailable>& value) {
    if (!value) return nullptr;
    json::object out;
    out["model"] = value->model;
    out["detail"] = textOrNullJson(value->detail);
    return out;
  }
  json::value evaluationJson(const evaluation::CodexRoutingAccountEvaluation& value) {
    json::object out;
    out["account"] = accountJson(value.account);
    out["routedAccount"] = value.routedAccount ? json::value(accountJson(*value.routedAccount)) : json::value(nullptr);
    out["skippedSlot"] = numberOrNull(value.skippedSlot);
    out["blockedCircuit"] = textOrNullJson(value.blockedCircuit);
    out["retryAtMs"] = numberOrNull(value.retryAtMs);
    out["blockedAccount"] = value.blockedAccount ? json::value(blockedJson(*value.blockedAccount)) : json::value(nullptr);
    out["activeTransitionReason"] = textOrNullJson(value.activeTransitionReason);
    out["capacityExhausted"] = value.capacityExhausted;
    out["modelUnavailable"] = unavailableJson(value.modelUnavailable);
    return out;
  }
  json::array numbersJson(const std::vector<double>& values) {
    json::array out;
    for (double value : values) out.push_back(rt::makeNumber(value));
    return out;
  }
  json::array blockedListJson(const std::vector<contracts::CodexBlockedAccountMetadata>& values) {
    json::array out;
    for (const auto& value : values) out.push_back(blockedJson(value));
    return out;
  }
  json::value accumulatorJson(const evaluation::CodexRoutingAccumulation& value) {
    json::array available;
    for (const auto& account : value.available) available.push_back(accountJson(account));
    json::object out;
    out["available"] = std::move(available);
    out["blockedAccounts"] = blockedListJson(value.blockedAccounts);
    out["skipped"] = numbersJson(value.skipped);
    out["retryAt"] = numberOrNull(value.retryAt);
    out["hasQuotaBlock"] = value.hasQuotaBlock;
    out["hasUpstreamTimeoutBlock"] = value.hasUpstreamTimeoutBlock;
    out["modelUnavailable"] = unavailableJson(value.modelUnavailable);
    return out;
  }
  json::value activeJson(const std::optional<contracts::CodexActiveAccountSelection>& value) {
    return value ? json::value(contracts::toJson(*value)) : json::value(nullptr);
  }
  json::value selectionJson(const contracts::CodexRouteSelection& selection) {
    json::object out;
    if (const auto* eligible = std::get_if<contracts::CodexRouteSelectionEligible>(&selection)) {
      json::array accounts;
      for (const auto& account : eligible->accounts) accounts.push_back(accountJson(account));
      out["kind"] = "eligible";
      out["accounts"] = std::move(accounts);
      out["skippedSlots"] = numbersJson(eligible->skippedSlots);
      out["blockedAccounts"] = blockedListJson(eligible->blockedAccounts);
    } else if (const auto* blocked = std::get_if<contracts::CodexRouteSelectionQuotaBlocked>(&selection)) {
      out["kind"] = "quota_blocked";
      out["skippedSlots"] = numbersJson(blocked->skippedSlots);
      out["retryAtMs"] = numberOrNull(blocked->retryAtMs);
      out["blockedAccounts"] = blockedListJson(blocked->blockedAccounts);
      out["fullCohortExhausted"] = blocked->fullCohortExhausted;
      rt::objectDefine(out, "fullCohortUnavailable", blocked->fullCohortUnavailable);
      json::object snapshot;
      snapshot["selection"] = activeJson(blocked->activeSnapshot.selection);
      out["activeSnapshot"] = std::move(snapshot);
      out["poolSnapshotJson"] = textOrNullJson(blocked->poolSnapshotJson);
      out["capacitySnapshotJson"] = textOrNullJson(blocked->capacitySnapshotJson);
    } else if (const auto* upstream = std::get_if<contracts::CodexRouteSelectionUpstreamBlocked>(&selection)) {
      out["kind"] = "upstream_blocked";
      out["skippedSlots"] = numbersJson(upstream->skippedSlots);
      out["retryAtMs"] = numberOrNull(upstream->retryAtMs);
      out["blockedAccounts"] = blockedListJson(upstream->blockedAccounts);
    } else if (const auto* model = std::get_if<contracts::CodexRouteSelectionModelUnavailable>(&selection)) {
      out["kind"] = "model_unavailable";
      out["model"] = model->model;
      out["detail"] = textOrNullJson(model->detail);
      out["skippedSlots"] = numbersJson(model->skippedSlots);
    } else if (const auto* invalid = std::get_if<contracts::CodexRouteSelectionCredentialsInvalid>(&selection)) {
      out["kind"] = "credentials_invalid";
      out["skippedSlots"] = numbersJson(invalid->skippedSlots);
      rt::objectDefine(out, "fullCohortUnavailable", invalid->fullCohortUnavailable);
    } else {
      out["kind"] = "routing_unavailable";
    }
    return out;
  }
  json::object revisionsJson(const contracts::CodexRoutingRowRevisions& revisions) {
    json::object out;
    out["active"] = textOrNullJson(revisions.active);
    out["pool"] = textOrNullJson(revisions.pool);
    out["routing"] = textOrNullJson(revisions.routing);
    out["capacity"] = textOrNullJson(revisions.capacity);
    return out;
  }
  json::value planJson(const std::optional<contracts::CodexSerialAdmissionPlan>& plan) {
    if (!plan) return nullptr;
    json::object writes;
    writes["routing"] = plan->writes.routing ? json::value(contracts::toJson(*plan->writes.routing)) : json::value(nullptr);
    writes["active"] = activeJson(plan->writes.active);
    json::object out;
    out["kind"] = "serial_admission";
    out["checks"] = revisionsJson(plan->checks);
    out["writes"] = std::move(writes);
    out["selection"] = selectionJson(plan->selection);
    return out;
  }
  contracts::CodexRoutingRowRevisions revisionsOf(const json::value& value) {
    return {.active = textOrNull(at(value, "active")), .pool = textOrNull(at(value, "pool")), .routing = textOrNull(at(value, "routing")),
            .capacity = textOrNull(at(value, "capacity"))};
  }

  bool same(const json::value& actual, const json::value& expected, const std::string& label) {
    if (rt::deepEqual(actual, expected)) return true;
    std::fprintf(stderr, "    %s\n      actual:   %s\n      expected: %s\n", label.c_str(), rt::jsonStringify(actual).c_str(), rt::jsonStringify(expected).c_str());
    return false;
  }
}

TEST("evaluateCodexRoutingAccount matches the oracle on every branch") {
  int cases = 0;
  for (const json::value& vector : section("evalVectors")) {
    const contracts::CodexAccountRoutingState state = stateOf(at(vector, "state"));
    const evaluation::CodexRoutingAccountEvaluation result =
      evaluation::evaluateCodexRoutingAccount(state, identityOf(at(vector, "mapped")), textOrNull(at(vector, "model")), observationsOf(at(vector, "observations")),
                                              number(vector, "now"), policyOf(at(vector, "policy")));
    EXPECT(same(evaluationJson(result), at(vector, "result"), text(vector, "name")));
    ++cases;
  }
  EXPECT_EQ(cases, 35);
}

TEST("foldCodexRoutingEvaluation and classifyCodexRouteSelection match the oracle over whole cohorts") {
  int cases = 0;
  for (const json::value& vector : section("cohortVectors")) {
    const contracts::CodexAccountRoutingState state = stateOf(at(vector, "state"));
    const auto observations = observationsOf(at(vector, "observations"));
    const std::vector<contracts::CodexRoutingIdentity> identities = identitiesOf(at(vector, "identities"));
    const json::array& policies = at(vector, "policies").as_array();
    evaluation::CodexRoutingAccumulator accumulator{.available = {}, .blockedAccounts = {}, .skipped = {}, .retryAt = std::nullopt, .hasQuotaBlock = false,
                                                    .hasUpstreamTimeoutBlock = false, .modelUnavailable = std::nullopt};
    for (std::size_t index = 0; index < identities.size(); ++index)
      evaluation::foldCodexRoutingEvaluation(accumulator, evaluation::evaluateCodexRoutingAccount(state, identities[index], textOrNull(at(vector, "model")), observations,
                                                                                                 number(vectors(), "now"), policyOf(policies[index])));
    const json::array& classify = at(vector, "classify").as_array();
    contracts::CodexRouteSelection classified;
    if (classify.empty()) {
      classified = evaluation::classifyCodexRouteSelection(accumulator);
    } else {
      contracts::CodexActiveAccountSnapshot snapshot;
      const json::value& selection = at(classify[1], "selection");
      if (!selection.is_null()) snapshot.selection = contracts::parseCodexActiveAccountSelection(selection);
      classified = evaluation::classifyCodexRouteSelection(accumulator, classify[0].as_bool(), snapshot, textOrNull(classify[2]), textOrNull(classify[3]), classify[4].as_bool());
    }
    const std::string name = text(vector, "name");
    EXPECT(same(accumulatorJson(accumulator), at(vector, "accumulator"), name + " (accumulator)"));
    EXPECT(same(selectionJson(classified), at(vector, "classified"), name + " (classified)"));
    ++cases;
  }
  EXPECT_EQ(cases, 12);
}

TEST("routingStateChangedByNormalization compares slots by value exactly where the TS compares identities") {
  int cases = 0;
  for (const json::value& vector : section("normalizeVectors")) {
    std::optional<contracts::CodexAccountRoutingState> durable;
    if (!at(vector, "durable").is_null()) durable = stateOf(at(vector, "durable"));
    const contracts::CodexAccountRoutingState* durableRow = durable ? &*durable : nullptr;
    const contracts::CodexAccountRoutingState normalized =
      capacity::normalizeRoutingState(durableRow, identitiesOf(at(vector, "identities")), number(vectors(), "now") + 7, at(vector, "discard").as_bool());
    const bool changed = evaluation::routingStateChangedByNormalization(durableRow, normalized);
    if (changed != at(vector, "changed").as_bool()) std::fprintf(stderr, "    %s\n", text(vector, "name").c_str());
    EXPECT_EQ(changed, at(vector, "changed").as_bool());
    ++cases;
  }
  EXPECT_EQ(cases, 15);
}

TEST("selectCodex plans match the oracle") {
  int cases = 0;
  for (const json::value& vector : section("selectVectors")) {
    const json::value& raw = at(vector, "snapshot");
    codex::CodexSerialSnapshot snapshot{
      .active = at(raw, "active"),
      .routing = at(raw, "routing"),
      .capacity = at(raw, "capacity"),
      .poolIdentities = identitiesOf(at(raw, "poolIdentities")),
      .selectedAccountHashes = {},
      .accountPolicies = {},
      .revisions = revisionsOf(at(raw, "revisions")),
    };
    for (const json::value& hash : at(raw, "selectedAccountHashes").as_array()) snapshot.selectedAccountHashes.emplace_back(hash.as_string());
    for (const auto& [hash, policy] : at(raw, "accountPolicies").as_object()) snapshot.accountPolicies.set(std::string(hash), policyOf(policy));
    const std::optional<contracts::CodexSerialAdmissionPlan> plan = codex::selectCodex(snapshot, number(vector, "now"), textOrNull(at(vector, "model")));
    EXPECT(same(planJson(plan), at(vector, "plan"), text(vector, "name")));
    ++cases;
  }
  EXPECT_EQ(cases, 17);
}

TEST("activeSelectionForAccount and planCodexResetElection match the oracle") {
  const std::string C(64, 'c'), D(64, 'd');
  const double now = number(vectors(), "now");
  contracts::CodexRoutingAccount account;
  account.slot = 1;
  account.accountIdHash = C;
  account.credentialVersion = D;
  account.routingGeneration = 4;
  const json::array& expected = section("activeSelection");
  EXPECT(same(activeJson(codex::activeSelectionForAccount(account, "P/9", 3, now, std::string("manual_reset"))), expected[0], "routed"));
  contracts::CodexRoutingAccount unrouted = account;
  unrouted.routingGeneration.reset();
  EXPECT(same(activeJson(codex::activeSelectionForAccount(unrouted, "P/9", 3, now, std::nullopt)), expected[1], "no routing generation"));
  EXPECT(same(activeJson(codex::activeSelectionForAccount(account, "", 3, now, std::nullopt)), expected[2], "empty versionstamp"));
  EXPECT(same(activeJson(codex::activeSelectionForAccount(account, "P/9", 0, now, std::nullopt)), expected[3], "generation 0"));
  EXPECT(same(activeJson(codex::activeSelectionForAccount(account, "P/9", 1.5, now, std::nullopt)), expected[4], "fractional generation"));
  EXPECT(same(activeJson(codex::activeSelectionForAccount(account, std::string(257, 'x'), 2, now, std::nullopt)), expected[5], "long versionstamp"));

  const json::value& election = at(vectors(), "resetElection");
  const contracts::CodexAccountRoutingState durable = stateOf(at(at(section("selectVectors")[1], "snapshot"), "routing"));
  const std::vector<contracts::CodexRoutingIdentity> pool = identitiesOf(at(at(section("selectVectors")[0], "snapshot"), "poolIdentities"));
  const contracts::CodexAccountRoutingState normalized = capacity::normalizeRoutingState(&durable, pool, now, true);
  const contracts::CodexActiveAccountSelection next = *contracts::parseCodexActiveAccountSelection(at(at(election, "unchanged"), "writes").as_object().at("active"));
  const auto electionJson = [](const codex::CodexResetElectionPlan& plan) {
    json::object writes;
    writes["routing"] = plan.writes.routing ? json::value(contracts::toJson(*plan.writes.routing)) : json::value(nullptr);
    writes["active"] = contracts::toJson(plan.writes.active);
    json::object out;
    out["kind"] = std::string(codex::CodexResetElectionPlan::kind);
    out["checks"] = revisionsJson(plan.checks);
    out["writes"] = std::move(writes);
    return json::value(std::move(out));
  };
  const contracts::CodexRoutingRowRevisions checks{.active = "A/1", .pool = "P/2", .routing = "R/3", .capacity = "C/4"};
  EXPECT(same(electionJson(codex::planCodexResetElection(checks, &durable, normalized, next)), at(election, "unchanged"), "election without routing write"));
  const contracts::CodexRoutingRowRevisions bare{.active = std::nullopt, .pool = "P/2", .routing = std::nullopt, .capacity = std::nullopt};
  EXPECT(same(electionJson(codex::planCodexResetElection(bare, nullptr, normalized, next)), at(election, "fromNull"), "election from an absent routing row"));
}
