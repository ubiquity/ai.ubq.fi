// Differential cases for src/authority/native-codex-mutations.ts beyond the probe vectors its bun test
// runs (tests/native_codex_conformance_test.cpp): every exported function over the inputs in
// tests/native_codex_mutations_vectors.inc, compared by the SHA-256 of the JSON text Bun 1.4.0 printed
// for each result.
#include "uos/authority/native_codex_capacity.hpp"
#include "uos/authority/native_codex_contracts.hpp"
#include "uos/authority/native_codex_mutations.hpp"

#include "uos/rt/crypto.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test.hpp"

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "native_codex_mutations_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::authority::native_codex_contracts;
using namespace uos::authority::native_codex_mutations;
using uos::authority::native_codex_capacity::CodexRoutingAccountIdentity;

namespace {
  std::optional<std::string> optionalString(const json::object& object, std::string_view key) {
    const json::value* value = rt::get(object, key);
    if (value == nullptr || value->is_null()) return std::nullopt;
    return std::string(value->get_string());
  }
  std::optional<double> optionalNumber(const json::object& object, std::string_view key) { return rt::getNumber(rt::get(object, key)); }
  rt::Nullable<std::string> nullableString(const json::object& object, std::string_view key) {
    const json::value* value = rt::get(object, key);
    if (value == nullptr) return rt::undefined;
    if (value->is_null()) return nullptr;
    return std::string(value->get_string());
  }

  CodexRoutingAccount accountFrom(const json::value& value) {
    const json::object& object = value.get_object();
    CodexRoutingAccount account;
    account.slot = *rt::getNumber(object, "slot");
    account.accountIdHash = *optionalString(object, "accountIdHash");
    account.credentialVersion = *optionalString(object, "credentialVersion");
    account.quotaHeadroom = optionalNumber(object, "quotaHeadroom");
    account.probeRequired = *rt::getBool(object, "probeRequired");
    account.probeGeneration = optionalNumber(object, "probeGeneration");
    account.probeToken = optionalString(object, "probeToken");
    account.probeCircuit = nullableString(object, "probeCircuit");
    account.overageFallback = rt::getBool(object, "overageFallback");
    account.routingGeneration = optionalNumber(object, "routingGeneration");
    account.activeGeneration = optionalNumber(object, "activeGeneration");
    account.activePoolVersionstamp = optionalString(object, "activePoolVersionstamp");
    account.activeTransitionReason = nullableString(object, "activeTransitionReason");
    account.requestedModel = nullableString(object, "requestedModel");
    return account;
  }

  CodexProbeLease leaseFrom(const json::value& value) {
    const json::object& object = value.get_object();
    CodexProbeLease lease;
    lease.token = *optionalString(object, "token");
    lease.expires_at_ms = *rt::getNumber(object, "expires_at_ms");
    lease.generation = *rt::getNumber(object, "generation");
    lease.circuit = *optionalString(object, "circuit");
    lease.quota_class = nullableString(object, "quota_class");
    return lease;
  }

  json::value numberOrNull(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }
  json::value stateOrNull(const std::optional<CodexAccountRoutingState>& state) { return state ? json::value(toJson(*state)) : json::value(nullptr); }
  json::value activeOrNull(const std::optional<CodexActiveAccountSelection>& active) { return active ? json::value(toJson(*active)) : json::value(nullptr); }
  json::value claimJson(const std::optional<CodexExpiredProbeClaim>& claim) {
    if (!claim) return nullptr;
    json::object object;
    rt::objectDefine(object, "next", toJson(claim->next));
    rt::objectDefine(object, "lease", toJson(claim->lease));
    return object;
  }
  json::value locatedJson(const std::optional<CodexLocatedRoutingSlot>& located) {
    if (!located) return nullptr;
    json::object object;
    rt::objectDefine(object, "index", rt::makeNumber(located->index));
    rt::objectDefine(object, "slot", toJson(located->slot));
    return object;
  }
  json::value planJson(const CodexAdmissionRefreshPlan& plan) {
    json::object object;
    if (const auto* refresh = std::get_if<CodexAdmissionRefreshPlanRefresh>(&plan)) {
      rt::objectDefine(object, "kind", json::value(json::string(refresh->kind)));
      rt::objectDefine(object, "next", toJson(refresh->next));
    } else {
      rt::objectDefine(object, "kind", json::value(json::string(CodexAdmissionRefreshPlanCurrent::kind)));
    }
    return object;
  }

  struct Replay {
    std::size_t next = 0;
    int mismatches = 0;
    void push(const std::string& label, const json::value& value) {
      const std::string text = rt::jsonStringify(value);
      const std::string digest = rt::crypto::createHash("sha256").update(text).digest("hex").substr(0, 16);
      constexpr std::size_t total = sizeof(kMutationResults) / sizeof(kMutationResults[0]);
      if (next >= total) {
        EXPECT_EQ(label, "<no more recorded cases>");
        ++mismatches;
        return;
      }
      const std::string expectedLabel = kMutationResults[next][0];
      const std::string expectedDigest = kMutationResults[next][1];
      ++next;
      if (label != expectedLabel || digest != expectedDigest) {
        ++mismatches;
        if (mismatches <= 25) {
          EXPECT_EQ(label, expectedLabel);
          EXPECT_EQ(digest + " " + text, expectedDigest + " <recorded>");
        }
      }
    }
  };
}

TEST("differential: native codex mutations over recorded inputs") {
  const json::value inputs = rt::jsonParse(kMutationInputs);
  const double now = 1'700'000'000'000;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  const json::array& rawStates = *rt::getArray(inputs, "rawStates");
  std::vector<CodexRoutingAccount> accounts;
  for (const json::value& account : *rt::getArray(inputs, "accounts")) accounts.push_back(accountFrom(account));
  std::vector<CodexRoutingAccountIdentity> identities;
  for (const json::value& identity : *rt::getArray(inputs, "identities")) {
    identities.push_back(CodexRoutingAccountIdentity{*optionalString(identity.get_object(), "accountIdHash"), *optionalString(identity.get_object(), "credentialVersion")});
  }
  std::vector<CodexProbeLease> recoveryLeases;
  for (const json::value& lease : *rt::getArray(inputs, "recoveryLeases")) recoveryLeases.push_back(leaseFrom(lease));
  const json::array& rawActives = *rt::getArray(inputs, "rawActives");
  const json::array& extraRaw = *rt::getArray(inputs, "extraRaw");
  const std::vector<std::pair<double, double>> fences = {
    {now + 60'000, 3}, {now + 60'000, 4}, {now + 59'999, 3}, {now - 1, 3}, {now + 60'000, -1},
    {now + 60'000, 1.5}, {nan, 3}, {now + 70'000, 3}, {now + 90'000, 3}, {now + 40'000, 2},
  };
  const std::vector<double> claimNows = {now, now + 30'000, now + 60'000, now - 10, now + 90'000};
  const std::vector<CodexStaleVerifiedResetProbeInput> staleInputs = {
    {now + 60'000, 3}, {now + 60'000, 4}, {now - 1, 3}, {now + 40'000, 2}, {now + 90'000, 3},
  };
  const std::vector<double> staleNows = {now, now + 30'000};
  const std::vector<double> resetAts = {now + 60'000, now - 1, now + 59'999, now + 70'000, now + 90'000};
  const std::vector<double> slotNumbers = {0, 1, 2, 3, -1, 1.5, nan, infinity, -0.0, -2};
  const json::value* undefined = nullptr;

  Replay replay;
  for (std::size_t s = 0; s < rawStates.size(); ++s) {
    const std::string st = "state" + std::to_string(s);
    const std::optional<CodexAccountRoutingState> parsed = parseCodexAccountRoutingState(rawStates[s]);
    replay.push(st + ".parse", stateOrNull(parsed));
    if (!parsed) continue;
    for (std::size_t a = 0; a < accounts.size(); ++a) {
      const std::string p = st + ".account" + std::to_string(a);
      const CodexRoutingAccount& account = accounts[a];
      replay.push(p + ".timeout", stateOrNull(markCodexUpstreamTimeout(*parsed, account, now, now + 25)));
      replay.push(p + ".invalid", stateOrNull(markCodexCredentialInvalid(*parsed, account, now + 3, now + 26)));
      replay.push(p + ".release", stateOrNull(releaseCodexRoutingProbe(*parsed, account, now + 27)));
      replay.push(p + ".success", stateOrNull(markCodexSuccess(*parsed, account, now + 28)));
      json::array reconciled;
      for (const CodexRoutingAccountIdentity& identity : identities) {
        for (const bool sameAccount : {true, false}) reconciled.push_back(stateOrNull(reconcileRoutingCredential(*parsed, account, identity, sameAccount, now + 29)));
      }
      replay.push(p + ".reconcile", reconciled);
      json::array fenced;
      for (const auto& [resetAt, generation] : fences) fenced.push_back(isCodexQuotaBlockFenceCurrent(rawStates[s], account, resetAt, generation));
      replay.push(p + ".fence", fenced);
      json::array recovered;
      for (const CodexProbeLease& lease : recoveryLeases) {
        try {
          recovered.push_back(toJson(recoveryProbeTransition(*parsed, account, lease, now + 23)));
        } catch (const rt::JsError& error) {
          json::object thrown;
          rt::objectDefine(thrown, "error", json::value(json::string(error.toString())));
          recovered.push_back(std::move(thrown));
        }
      }
      replay.push(p + ".recovery", recovered);
      json::array claims;
      for (const double claimNow : claimNows) claims.push_back(claimJson(buildExpiredProbeClaim(*parsed, account, claimNow, "tok-" + rt::numberToString(claimNow), claimNow + 17)));
      replay.push(p + ".claim", claims);
      json::array eligible;
      for (const CodexRoutingSlot& slot : parsed->slots) {
        for (const CodexStaleVerifiedResetProbeInput& input : staleInputs) {
          for (const double nowMs : staleNows) eligible.push_back(staleVerifiedResetProbeEligible(*parsed, slot, account, input, nowMs));
        }
      }
      replay.push(p + ".staleEligible", eligible);
      json::array staleIdentity;
      for (const double resetAt : resetAts) staleIdentity.push_back(staleResetRoutingIdentityCurrent(rawStates[s], account, resetAt));
      replay.push(p + ".staleIdentity", staleIdentity);
      replay.push(p + ".admission", numberOrNull(admissionRefreshRoutingGeneration(&*parsed, account)));
      json::array candidates;
      for (const CodexRoutingSlot& slot : parsed->slots) {
        for (const bool current : {true, false}) candidates.push_back(resetCandidateSlotMatches(slot, account, current));
      }
      replay.push(p + ".candidate", candidates);
    }
    for (std::size_t index = 0; index < parsed->slots.size(); ++index) {
      replay.push(st + ".slot" + std::to_string(index) + ".recheck", toJson(recheckSlotTransition(parsed->slots[index], now + 24)));
    }
    json::array located;
    for (const double slotNumber : slotNumbers) located.push_back(locatedJson(locateRoutingSlot(*parsed, slotNumber)));
    replay.push(st + ".locate", located);
  }
  for (std::size_t a = 0; a < accounts.size(); ++a) {
    const std::string p = "account" + std::to_string(a);
    const CodexRoutingAccount& account = accounts[a];
    replay.push(p + ".admission.null", numberOrNull(admissionRefreshRoutingGeneration(nullptr, account)));
    for (std::size_t r = 0; r < extraRaw.size(); ++r) {
      json::array fenced;
      for (const auto& [resetAt, generation] : fences) fenced.push_back(isCodexQuotaBlockFenceCurrent(extraRaw[r], account, resetAt, generation));
      replay.push(p + ".extra" + std::to_string(r) + ".fence", fenced);
      json::array staleIdentity;
      for (const double resetAt : resetAts) staleIdentity.push_back(staleResetRoutingIdentityCurrent(extraRaw[r], account, resetAt));
      replay.push(p + ".extra" + std::to_string(r) + ".staleIdentity", staleIdentity);
    }
    replay.push(p + ".fence.undefined", isCodexQuotaBlockFenceCurrent(undefined, account, now + 60'000, 3));
    replay.push(p + ".staleIdentity.undefined", staleResetRoutingIdentityCurrent(undefined, account, now + 60'000));
  }
  std::vector<std::optional<CodexActiveAccountSelection>> parsedActives;
  for (const json::value& raw : rawActives) parsedActives.push_back(parseCodexActiveAccountSelection(raw));
  for (std::size_t r = 0; r < rawActives.size(); ++r) {
    const std::string p = "active" + std::to_string(r);
    replay.push(p + ".parse", activeOrNull(parsedActives[r]));
    json::array snapshots;
    for (const std::optional<CodexActiveAccountSelection>& selection : parsedActives) {
      snapshots.push_back(isCodexActiveAccountSnapshotCurrent(rawActives[r], CodexActiveAccountSnapshot{selection}));
    }
    replay.push(p + ".snapshot", snapshots);
    const std::optional<CodexActiveAccountSelection>& active = parsedActives[r];
    for (std::size_t a = 0; a < accounts.size(); ++a) {
      replay.push(p + ".account" + std::to_string(a) + ".election", rt::makeNumber(nextResetElectionGeneration(active, accounts[a])));
      if (!active) continue;
      json::array prepared;
      for (const std::string poolVersionstamp : {"P/opaque:11", "P/other"}) {
        for (const double generation : {0.0, 3.0}) prepared.push_back(planJson(prepareCodexAdmissionRefresh(*active, accounts[a], poolVersionstamp, generation)));
      }
      replay.push(p + ".account" + std::to_string(a) + ".prepare", prepared);
    }
  }
  json::array undefinedSnapshots;
  for (const std::optional<CodexActiveAccountSelection>& selection : parsedActives) {
    undefinedSnapshots.push_back(isCodexActiveAccountSnapshotCurrent(undefined, CodexActiveAccountSnapshot{selection}));
  }
  replay.push("snapshot.undefined", undefinedSnapshots);

  EXPECT_EQ(replay.next, sizeof(kMutationResults) / sizeof(kMutationResults[0]));
  EXPECT_EQ(replay.mismatches, 0);
}

TEST("a class recovery lease preserves the shorter class and independent model blocks") {
  const json::value inputs = rt::jsonParse(kMutationInputs);
  auto state = parseCodexAccountRoutingState((*rt::getArray(inputs, "rawStates"))[7]);
  ASSERT(state);
  CodexRoutingAccount account = accountFrom((*rt::getArray(inputs, "accounts"))[0]);
  const CodexProbeLease lease = leaseFrom((*rt::getArray(inputs, "recoveryLeases"))[0]);
  CodexRoutingSlot& slot = state->slots.front();
  ASSERT(slot.quota_blocks_by_class);
  const auto* standard = slot.quota_blocks_by_class->get("standard");
  ASSERT(standard);
  auto named = *standard;
  named.blocked_until_ms = 1'700'000'090'000;
  slot.quota_blocks_by_class->set("model:gpttest", named);

  const auto expectOtherBlocks = [](const CodexRoutingSlot& updated) {
    ASSERT(updated.quota_blocks_by_class);
    EXPECT(!updated.quota_blocks_by_class->has("unknown"));
    EXPECT(updated.quota_blocks_by_class->has("standard"));
    EXPECT(updated.quota_blocks_by_class->has("model:gpttest"));
  };

  const double now = 1'700'000'000'023;
  const auto recovered = recoveryProbeTransition(*state, account, lease, now);
  expectOtherBlocks(recovered.slots.front());
  ASSERT(recovered.slots.front().observed_reset_at_ms);
  EXPECT_EQ(*recovered.slots.front().observed_reset_at_ms, 1'700'000'070'000.0);

  slot.generation = lease.generation;
  slot.probe_lease = lease;
  account.probeGeneration = lease.generation;
  account.probeToken = lease.token;
  account.probeCircuit = lease.circuit;
  const auto succeeded = markCodexSuccess(*state, account, now);
  const auto released = releaseCodexRoutingProbe(*state, account, now);
  ASSERT(succeeded);
  ASSERT(released);
  expectOtherBlocks(succeeded->slots.front());
  expectOtherBlocks(released->slots.front());
}

TEST("recoveryProbeTransition throws the TS TypeError for a slot index the state does not hold") {
  CodexAccountRoutingState state;
  state.v = 2;
  state.updated_at_ms = 1;
  state.slots.push_back(uos::authority::native_codex_capacity::neutralSlot(std::string(64, 'b'), std::string(64, 'a')));
  CodexRoutingAccount account;
  account.slot = 5;
  account.accountIdHash = std::string(64, 'a');
  account.credentialVersion = std::string(64, 'b');
  CodexProbeLease lease;
  lease.token = "t";
  lease.expires_at_ms = 1;
  lease.generation = 1;
  lease.circuit = "quota";
  lease.quota_class = nullptr;
  EXPECT_THROWS_MESSAGE(recoveryProbeTransition(state, account, lease, 2), rt::TypeError, "undefined is not an object (evaluating 'slot.quota_blocks_by_class')");
  account.slot = -1;
  EXPECT_THROWS_MESSAGE(recoveryProbeTransition(state, account, lease, 2), rt::TypeError, "undefined is not an object (evaluating 'slot.quota_blocks_by_class')");
  account.slot = 0.5;
  EXPECT_THROWS_MESSAGE(recoveryProbeTransition(state, account, lease, 2), rt::TypeError, "undefined is not an object (evaluating 'slot.quota_blocks_by_class')");
  account.slot = -0.0;
  EXPECT_EQ(recoveryProbeTransition(state, account, lease, 2).slots.size(), std::size_t{1});
}
