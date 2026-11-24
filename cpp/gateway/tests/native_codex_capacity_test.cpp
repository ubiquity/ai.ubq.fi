// Differential cases for src/authority/native-codex-contracts.ts and native-codex-capacity.ts
// (neither has a direct bun test of its own beyond tests/native_codex_conformance_test.cpp): every
// exported function over the inputs in tests/native_codex_capacity_vectors.inc, compared by the
// SHA-256 of the JSON text Bun 1.4.0 printed for each result.
#include "uos/authority/native_codex_capacity.hpp"
#include "uos/authority/native_codex_contracts.hpp"

#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "native_codex_capacity_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::authority::native_codex_contracts;
using namespace uos::authority::native_codex_capacity;

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
    if (const std::optional<bool> overage = rt::getBool(object, "overageFallback")) account.overageFallback = overage;
    account.routingGeneration = optionalNumber(object, "routingGeneration");
    account.activeGeneration = optionalNumber(object, "activeGeneration");
    account.activePoolVersionstamp = optionalString(object, "activePoolVersionstamp");
    account.activeTransitionReason = nullableString(object, "activeTransitionReason");
    account.requestedModel = nullableString(object, "requestedModel");
    return account;
  }

  std::optional<CodexCapacityRoutingWindow> windowFrom(const json::value* value) {
    if (value == nullptr || value->is_null()) return std::nullopt;
    const json::object& object = value->get_object();
    return CodexCapacityRoutingWindow{optionalNumber(object, "limit_window_seconds"), optionalNumber(object, "used_percent"), optionalNumber(object, "reset_at_ms")};
  }

  CodexCapacityRoutingWindows windowsFrom(const json::value* value) {
    return CodexCapacityRoutingWindows{windowFrom(rt::get(*value, "primary")), windowFrom(rt::get(*value, "secondary"))};
  }

  CodexCapacityRoutingObservation observationFrom(const json::value& value) {
    const json::object& object = value.get_object();
    CodexCapacityRoutingObservation observation;
    observation.slot = *rt::getNumber(object, "slot");
    observation.account_id_hash = *optionalString(object, "account_id_hash");
    observation.state = *optionalString(object, "state");
    observation.source_observed_at_ms = optionalNumber(object, "source_observed_at_ms");
    observation.snapshot_at_ms = *rt::getNumber(object, "snapshot_at_ms");
    observation.windows = windowsFrom(rt::get(object, "windows"));
    for (const json::value& limit : *rt::getArray(object, "additional_rate_limits")) {
      const json::object& entry = limit.get_object();
      observation.additional_rate_limits.push_back(
        CodexCapacityRoutingAdditionalRateLimit{*optionalString(entry, "limit_name"), optionalString(entry, "metered_feature"), windowsFrom(rt::get(entry, "windows"))});
    }
    return observation;
  }

  std::vector<CodexCapacityRoutingObservation> observationsFrom(const json::value& value) {
    std::vector<CodexCapacityRoutingObservation> observations;
    for (const json::value& entry : value.get_array()) observations.push_back(observationFrom(entry));
    return observations;
  }

  std::vector<CodexRoutingIdentity> identitiesFrom(const json::value& value) {
    std::vector<CodexRoutingIdentity> identities;
    for (const json::value& entry : value.get_array()) {
      const json::object& object = entry.get_object();
      identities.push_back(CodexRoutingIdentity{*rt::getNumber(object, "slot"), *optionalString(object, "accountIdHash"), *optionalString(object, "credentialVersion")});
    }
    return identities;
  }

  json::value orNull(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }
  json::value orNull(const std::optional<std::string>& value) { return value ? json::value(*value) : json::value(nullptr); }
  template <class T> json::value jsonOrNull(const std::optional<T>& value) { return value ? json::value(toJson(*value)) : json::value(nullptr); }
  json::value arrayOf(const std::vector<CodexCapacityRoutingObservation>& observations) {
    json::array array;
    for (const CodexCapacityRoutingObservation& observation : observations) array.push_back(toJson(observation));
    return array;
  }
  json::value arrayOf(const std::vector<CodexCapacityRoutingAdditionalRateLimit>& limits) {
    json::array array;
    for (const CodexCapacityRoutingAdditionalRateLimit& limit : limits) array.push_back(toJson(limit));
    return array;
  }
  json::value planJson(const CodexCapacityObservationStorePlan& plan) {
    return json::object{{"kind", json::string(plan.kind)},
                        {"checks", json::object{{"capacity", orNull(plan.checks.capacity)}}},
                        {"writes", json::object{{"capacity", toJson(plan.writes.capacity)}}}};
  }

  std::optional<json::value> expectedHeldSparkRelease(const CodexRoutingSlot& input) {
    const auto* spark = input.quota_blocks_by_class ? input.quota_blocks_by_class->get("spark") : nullptr;
    const auto* standard = input.quota_blocks_by_class ? input.quota_blocks_by_class->get("standard") : nullptr;
    const auto* fallback = input.quota_blocks_by_class ? input.quota_blocks_by_class->get("unknown") : nullptr;
    const bool fixtureMatches = input.probe_lease && input.probe_lease->quota_class.toOptional() == "spark" &&
                                spark && standard && fallback && fallback->legacy_fallback;
    EXPECT(fixtureMatches);
    if (!fixtureMatches) return std::nullopt;

    // The archived TypeScript replay used the caller's class, even though this
    // slot already holds a Spark lease. The lease owns the Spark block and its
    // migrated fallback. The independent Standard 429 keeps its deadline.
    CodexRoutingSlot expected = input;
    expected.quota_blocks_by_class = CodexQuotaBlocksByClass{{"standard", *standard}};
    expected.quota_blocked_classes = std::vector<std::string>{"standard"};
    expected.quota_blocked_until_ms = standard->blocked_until_ms;
    expected.quota_block_source = standard->source;
    expected.quota_signal_observed_at_ms = standard->quota_signal_observed_at_ms;
    expected.observed_reset_at_ms = spark->observed_reset_at_ms;
    expected.observed_reset_at_is_stable = spark->observed_reset_at_is_stable;
    expected.banked_reset_generation_ambiguous = true;
    expected.banked_reset_recovery_probe_pending = standard->banked_reset_recovery_probe_pending;
    return json::value(toJson(expected));
  }

  struct Replay {
    std::size_t next = 0;
    int mismatches = 0;
    void push(const std::string& label, const json::value& value, const std::optional<json::value>& expectedNative = std::nullopt) {
      const std::string text = rt::jsonStringify(value);
      const std::string digest = rt::crypto::createHash("sha256").update(text).digest("hex").substr(0, 16);
      constexpr std::size_t total = sizeof(kCodexResults) / sizeof(kCodexResults[0]);
      if (next >= total) {
        EXPECT_EQ(label, "<no more recorded cases>");
        ++mismatches;
        return;
      }
      const std::string expectedLabel = kCodexResults[next][0];
      const std::string expectedDigest = kCodexResults[next][1];
      ++next;
      const bool resultMatches = expectedNative ? rt::deepEqual(value, *expectedNative) : digest == expectedDigest;
      if (label != expectedLabel || !resultMatches) {
        ++mismatches;
        if (mismatches <= 25) {
          EXPECT_EQ(label, expectedLabel);
          if (expectedNative) {
            EXPECT_EQ(text, rt::jsonStringify(*expectedNative));
          } else {
            EXPECT_EQ(digest + " " + text, expectedDigest + " <recorded>");
          }
        }
      }
    }
  };
}

TEST("differential: native codex contracts and capacity over recorded inputs") {
  const json::value inputs = rt::jsonParse(kCodexInputs);
  const double now = 1'700'000'000'000;
  const std::vector<std::string> classKeys = {"spark", "gpt_oss_120b", "reserve", "standard", "unknown"};
  std::vector<CodexRoutingAccount> accounts;
  for (const json::value& account : *rt::getArray(inputs, "accounts")) accounts.push_back(accountFrom(account));
  const std::vector<CodexCapacityRoutingObservation> observations = observationsFrom(*rt::get(inputs, "observations"));
  std::vector<std::vector<CodexRoutingIdentity>> identitySets;
  for (const json::value& set : *rt::getArray(inputs, "identitySets")) identitySets.push_back(identitiesFrom(set));
  std::vector<std::optional<std::string>> models;
  for (const json::value& model : *rt::getArray(inputs, "models")) models.push_back(model.is_null() ? std::nullopt : std::optional<std::string>(std::string(model.get_string())));
  const auto modelView = [](const std::optional<std::string>& model) { return model ? std::optional<std::string_view>(*model) : std::nullopt; };

  Replay replay;
  const json::array& rawStates = *rt::getArray(inputs, "rawStates");
  for (std::size_t i = 0; i < rawStates.size(); ++i) {
    const std::string state = "state" + std::to_string(i);
    const std::optional<CodexAccountRoutingState> parsed = parseCodexAccountRoutingState(rawStates[i]);
    replay.push(state + ".parse", jsonOrNull(parsed));
    if (!parsed) continue;
    replay.push(state + ".hasUnresolved", hasUnresolvedLegacyResetIdentity(&*parsed));
    replay.push(state + ".withoutTimeouts", toJson(withoutLegacyTimeoutCircuits(*parsed)));
    for (std::size_t j = 0; j < identitySets.size(); ++j) {
      replay.push(state + ".normalize" + std::to_string(j), toJson(normalizeRoutingState(&*parsed, identitySets[j], now + 5)));
      replay.push(state + ".normalize" + std::to_string(j) + ".discard", toJson(normalizeRoutingState(&*parsed, identitySets[j], now + 5, true)));
    }
    for (std::size_t s = 0; s < parsed->slots.size(); ++s) {
      const CodexRoutingSlot& slot = parsed->slots[s];
      const std::string p = state + ".slot" + std::to_string(s);
      replay.push(p + ".legacyStable", isLegacyStableResetIdentity(slot));
      replay.push(p + ".includingLegacy", toJson(quotaBlocksIncludingLegacy(slot)));
      replay.push(p + ".classMap", toJson(withLegacyQuotaClassMap(slot)));
      replay.push(p + ".recheck", toJson(recheckQuotaClasses(slot, now + 77)));
      replay.push(p + ".headroom", orNull(quotaHeadroomFor(slot)));
      replay.push(p + ".rotate", toJson(rotateCredentialForSameAccount(slot, CodexRoutingAccountIdentity{std::string(64, 'f'), std::string(64, 'e')})));
      const auto expectedRelease = i == 4 && s == 0 ? expectedHeldSparkRelease(slot) : std::nullopt;
      for (const std::string& key : classKeys) {
        replay.push(p + "." + key + ".block", jsonOrNull(quotaBlockForClass(slot, key)));
        replay.push(p + "." + key + ".key", orNull(quotaBlockKeyForClass(slot, key)));
        replay.push(p + "." + key + ".keyAware", orNull(quotaBlockKeyForClass(withLegacyQuotaClassMap(slot), key)));
        replay.push(p + "." + key + ".signal", orNull(quotaSignalObservedAtForClass(slot, key)));
        replay.push(p + "." + key + ".without", toJson(withoutQuotaClass(slot, key)));
        replay.push(p + "." + key + ".release", toJson(releaseQuotaClassProbe(slot, key)), expectedRelease);
      }
    }
    for (std::size_t a = 0; a < accounts.size(); ++a) {
      const std::string p = state + ".account" + std::to_string(a);
      const CodexRoutingSlot slot = slotFor(*parsed, accounts[a]);
      replay.push(p + ".slotFor", toJson(slot));
      replay.push(p + ".matches", slotMatchesCodexRoutingAccount(slot, accounts[a]));
      replay.push(p + ".lease", probeLeaseMatchesCodexRoutingAccount(slot, accounts[a]));
      for (std::size_t o = 0; o < observations.size(); ++o) {
        replay.push(p + ".obs" + std::to_string(o) + ".quotaBlock", jsonOrNull(applyCapacityQuotaBlock(*parsed, accounts[a], observations[o], now, now + 9)));
      }
    }
    for (std::size_t o = 0; o < observations.size(); ++o) {
      for (std::size_t m = 0; m < models.size(); ++m) {
        replay.push(state + ".obs" + std::to_string(o) + ".model" + std::to_string(m) + ".apply",
                    jsonOrNull(applyCapacityObservation(*parsed, observations[o], now, modelView(models[m]), now + 11)));
      }
    }
    const CodexRoutingSlot replacement = neutralSlot(std::string(64, 'e'), std::string(64, 'f'));
    const std::vector<double> indexes = {0, 3, -1, 1.5, std::numeric_limits<double>::quiet_NaN()};
    for (std::size_t x = 0; x < indexes.size(); ++x) replay.push(state + ".withSlot" + std::to_string(x), toJson(withSlot(*parsed, indexes[x], replacement, now + 1)));
  }
  for (std::size_t a = 0; a < accounts.size(); ++a) replay.push("account" + std::to_string(a) + ".circuit", json::value(routingProbeCircuit(accounts[a])));
  for (std::size_t m = 0; m < models.size(); ++m) replay.push("model" + std::to_string(m) + ".class", json::value(quotaClass(modelView(models[m]))));
  for (std::size_t o = 0; o < observations.size(); ++o) {
    replay.push("obs" + std::to_string(o) + ".fresh", capacityObservationIsFresh(observations[o], now));
    for (std::size_t m = 0; m < models.size(); ++m)
      replay.push("obs" + std::to_string(o) + ".model" + std::to_string(m) + ".headroom", orNull(capacityHeadroomForObservation(observations[o], modelView(models[m]))));
  }
  const json::array& rawWindows = *rt::getArray(inputs, "rawWindows");
  for (std::size_t w = 0; w < rawWindows.size(); ++w) replay.push("window" + std::to_string(w), jsonOrNull(normalizeCapacityWindow(&rawWindows[w])));
  replay.push("window.undefined", jsonOrNull(normalizeCapacityWindow(nullptr)));
  const json::array& rawLimits = *rt::getArray(inputs, "rawLimits");
  for (std::size_t l = 0; l < rawLimits.size(); ++l) replay.push("limits" + std::to_string(l), arrayOf(normalizeCapacityAdditionalRateLimits(&rawLimits[l])));
  const json::array& rawStates2 = *rt::getArray(inputs, "rawStates2");
  for (std::size_t s = 0; s < rawStates2.size(); ++s) replay.push("capacityState" + std::to_string(s), orNull(capacityState(&rawStates2[s])));
  const json::array& percentStrings = *rt::getArray(inputs, "percentStrings");
  for (std::size_t s = 0; s < percentStrings.size(); ++s) {
    const std::optional<std::string> text = percentStrings[s].is_null() ? std::nullopt : std::optional<std::string>(std::string(percentStrings[s].get_string()));
    replay.push("percent" + std::to_string(s), orNull(parseFinitePercent(text)));
  }
  const json::array& rawStores = *rt::getArray(inputs, "rawStores");
  for (std::size_t s = 0; s < rawStores.size(); ++s) replay.push("store" + std::to_string(s), arrayOf(parseStoredCapacityObservationStore(rawStores[s])));
  const json::object& mergeSets = *rt::getObject(inputs, "mergeSets");
  const std::vector<CodexCapacityRoutingObservation> durable = observationsFrom(*rt::get(mergeSets, "durable"));
  const std::vector<CodexCapacityRoutingObservation> fresh = observationsFrom(*rt::get(mergeSets, "fresh"));
  replay.push("merge", arrayOf(mergeCapacityObservations(durable, fresh)));
  replay.push("merge.single", arrayOf(mergeCapacityObservations(fresh)));
  replay.push("merge.none", arrayOf(mergeCapacityObservations(std::span<const std::vector<CodexCapacityRoutingObservation>>())));
  std::vector<CodexCapacityRoutingObservation> all = durable;
  all.insert(all.end(), fresh.begin(), fresh.end());
  replay.push("retain", arrayOf(retainRecentCapacityObservations(all, now)));
  replay.push("plan", planJson(planCapacityObservationStore("C/opaque:19", durable, fresh, now)));
  replay.push("plan.empty", planJson(planCapacityObservationStore(std::nullopt, {}, {}, 5)));
  replay.push("neutral.null", toJson(neutralSlot("cv", std::nullopt)));
  const json::array& rawActive = *rt::getArray(inputs, "rawActive");
  for (std::size_t r = 0; r < rawActive.size(); ++r) {
    replay.push("active" + std::to_string(r), jsonOrNull(parseCodexActiveAccountSelection(rawActive[r])));
    for (std::size_t a = 0; a < accounts.size(); ++a) {
      for (const double generation : {1.0, 2.0, 9.0}) {
        replay.push("active" + std::to_string(r) + ".account" + std::to_string(a) + ".gen" + rt::numberToString(generation),
                    codexActiveAccountSelectionCurrent(rawActive[r], accounts[a], generation));
      }
    }
  }
  const json::array& rawScalars = *rt::getArray(inputs, "rawScalars");
  for (std::size_t s = 0; s < rawScalars.size(); ++s) {
    replay.push("scalar" + std::to_string(s) + ".safe", isSafeMs(rawScalars[s]));
    replay.push("scalar" + std::to_string(s) + ".optional", orNull(parseOptionalSafeMs(&rawScalars[s])));
    replay.push("scalar" + std::to_string(s) + ".versionstamp", isVersionstamp(&rawScalars[s]));
  }
  const json::value* undefined = nullptr;
  replay.push("scalar.undefined", json::array{isSafeMs(undefined), orNull(parseOptionalSafeMs(undefined)), isVersionstamp(undefined)});
  replay.push("hasUnresolved.null", hasUnresolvedLegacyResetIdentity(nullptr));

  EXPECT_EQ(replay.next, sizeof(kCodexResults) / sizeof(kCodexResults[0]));
  EXPECT_EQ(replay.mismatches, 0);
}
