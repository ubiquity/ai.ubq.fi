#include "uos/authority/native_codex_capacity.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace uos::authority::native_codex_capacity {
  using namespace native_codex_contracts;

  namespace {
    using utils::isRecord;

    const json::value* member(const json::value* value, std::string_view key) { return value != nullptr ? rt::get(*value, key) : nullptr; }

    // `array[index]` with a JS number index: an element only for a canonical array index.

    // `(latest, block) => (!latest || block.blocked_until_ms > latest.blocked_until_ms ? block : latest)`
    void keepLatest(std::optional<CodexQuotaClassBlock>& latest, const CodexQuotaClassBlock& block) {
      if (!latest || block.blocked_until_ms > latest->blocked_until_ms) latest = block;
    }

    bool hasEntries(const std::optional<CodexQuotaBlocksByClass>& blocks) { return blocks && blocks->size() > 0; }

    std::vector<std::string> keysOf(const CodexQuotaBlocksByClass& blocks) {
      std::vector<std::string> keys;
      for (const auto& [key, block] : blocks) {
        if (!isCodexCapacityModelScope(key)) keys.push_back(key);
      }
      return keys;
    }

    CodexQuotaClassBlock legacyBlockOf(const CodexRoutingSlot& slot, double blockedUntilMs) {
      return CodexQuotaClassBlock{
        blockedUntilMs,
        *slot.quota_block_source,
        true,
        slot.quota_signal_observed_at_ms,
        slot.observed_reset_at_ms,
        slot.observed_reset_at_is_stable,
        slot.banked_reset_generation_ambiguous,
        slot.banked_reset_recovery_probe_pending,
      };
    }

    /**
     * Timeout fences were a shared provider gate. They are retained in the parser
     * only for a safe hard cut from existing durable rows, then discarded before
     * any live routing decision.
     */
    // nullopt: the TS returns the slot itself.
    std::optional<CodexRoutingSlot> withoutLegacyTimeoutCircuitChange(const CodexRoutingSlot& slot) {
      const bool hasTimeoutLease = slot.probe_lease && slot.probe_lease->circuit == "upstream_timeout";
      const bool timeoutFence = slot.upstream_timeout_blocked_until_ms.hasValue() && rt::truthy(*slot.upstream_timeout_blocked_until_ms);
      if (!timeoutFence && !hasTimeoutLease) return std::nullopt;
      CodexRoutingSlot next = slot;
      next.upstream_timeout_blocked_until_ms = nullptr;
      if (hasTimeoutLease) next.probe_lease.reset();
      next.generation = slot.generation + 1;
      return next;
    }

    CodexRoutingSlot withoutLegacyTimeoutCircuit(const CodexRoutingSlot& slot) {
      std::optional<CodexRoutingSlot> changed = withoutLegacyTimeoutCircuitChange(slot);
      return changed ? std::move(*changed) : slot;
    }

    bool preservesStableResetIdentity(const CodexRoutingSlot& slot) {
      return slot.banked_reset_generation_ambiguous || (slot.observed_reset_at_ms.has_value() && slot.observed_reset_at_is_stable);
    }

    CodexRoutingSlot attachLegacyAccountIdentity(const CodexRoutingSlot& slot, const CodexRoutingAccountIdentity& identity) {
      CodexRoutingSlot next = slot;
      next.account_id_hash = identity.accountIdHash;
      // A pre-account-identity record cannot establish that its stable deadline
      // belongs to this account after a pool transition. Keep it lookup-only.
      next.banked_reset_generation_ambiguous = preservesStableResetIdentity(slot);
      return next;
    }

    CodexRoutingAccountIdentity identityOf(const CodexRoutingIdentity& identity) { return CodexRoutingAccountIdentity{identity.accountIdHash, identity.credentialVersion}; }

    // A missing field and a non-canonical value both mean "no usable observation",
    // so each reader needs only the canonical-value test.
    std::optional<double> capacityPercent(const json::value* value) {
      const std::optional<double> number = rt::getNumber(value);
      if (number && rt::Number::isFinite(*number) && *number >= 0 && *number <= 100) return number;
      return std::nullopt;
    }

    std::optional<double> capacityWindowSeconds(const json::value* value) {
      const std::optional<double> number = rt::getNumber(value);
      if (number && rt::Number::isSafeInteger(*number) && *number >= 0) return number;
      return std::nullopt;
    }

    std::optional<double> capacityResetAtMs(const json::value* value) { return parseOptionalSafeMs(value); }

    std::optional<CodexCapacityRoutingAdditionalRateLimit> normalizeCapacityAdditionalRateLimit(const json::value* value) {
      if (!isRecord(value)) return std::nullopt;
      const json::value* name = member(value, "limit_name");
      const std::string limitName = name != nullptr && name->is_string() ? rt::trim(std::string_view(name->get_string())) : std::string();
      if (limitName.empty()) return std::nullopt;
      const json::value* windows = isRecord(member(value, "windows")) ? member(value, "windows") : nullptr;
      if (windows == nullptr) return std::nullopt;
      const json::value* meteredFeature = member(value, "metered_feature");
      if (meteredFeature != nullptr && !meteredFeature->is_null() && !meteredFeature->is_string()) return std::nullopt;
      std::optional<std::string> feature;
      if (meteredFeature != nullptr && meteredFeature->is_string()) {
        std::string trimmed = rt::trim(std::string_view(meteredFeature->get_string()));
        if (!trimmed.empty()) feature = std::move(trimmed);
      }
      return CodexCapacityRoutingAdditionalRateLimit{
        limitName,
        std::move(feature),
        CodexCapacityRoutingWindows{normalizeCapacityWindow(member(windows, "primary")), normalizeCapacityWindow(member(windows, "secondary"))},
      };
    }

    std::optional<double> capacityHeadroomForWindows(const CodexCapacityRoutingWindows& windows) {
      std::vector<double> headrooms;
      for (const std::optional<CodexCapacityRoutingWindow>* window : {&windows.primary, &windows.secondary}) {
        if (*window && (*window)->used_percent) headrooms.push_back(100 - *(*window)->used_percent);
      }
      if (headrooms.empty()) return std::nullopt;
      return rt::Math::max(0, rt::Math::min(std::span<const double>(headrooms)));
    }

    std::string normalizeQuotaLabel(std::optional<std::string_view> value) {
      static const rt::Regex NON_ALPHANUMERIC("[^a-z0-9]", "g");
      return value ? rt::replace(rt::toLowerCase(*value), NON_ALPHANUMERIC, "") : std::string();
    }

    CodexQuotaClass capacityModelScope(std::optional<std::string_view> model) {
      const std::string key = normalizeQuotaLabel(model);
      return key.empty() ? CodexQuotaClass{} : std::string(CODEX_CAPACITY_MODEL_SCOPE_PREFIX) + key;
    }

    std::vector<const CodexCapacityRoutingWindows*> capacityWindowsForModel(const CodexCapacityRoutingObservation& observation,
                                                                             std::optional<std::string_view> model) {
      const std::string modelKey = normalizeQuotaLabel(model);
      std::vector<const CodexCapacityRoutingWindows*> applicable;
      if (!modelKey.empty()) {
        for (const CodexCapacityRoutingAdditionalRateLimit& limit : observation.additional_rate_limits) {
          if (normalizeQuotaLabel(std::string_view(limit.limit_name)) == modelKey) applicable.push_back(&limit.windows);
        }
      }
      if (applicable.empty()) applicable.push_back(&observation.windows);
      return applicable;
    }

    std::optional<double> exhaustedWindowResetAtMs(const CodexCapacityRoutingWindows& windows, double now) {
      std::optional<double> latest;
      for (const auto* window : {&windows.primary, &windows.secondary}) {
        if (!*window || !(*window)->used_percent || *(*window)->used_percent < 100) continue;
        const auto resetAtMs = (*window)->reset_at_ms;
        if (!isSafeMs(resetAtMs)) return std::nullopt;
        if (*resetAtMs > now && (!latest || *resetAtMs > *latest)) latest = resetAtMs;
      }
      return latest;
    }

    constexpr std::array<std::string_view, 5> quotaClassKeys = {"spark", "gpt_oss_120b", "reserve", "standard", "unknown"};

    bool isQuotaClassKey(std::string_view value) { return std::find(quotaClassKeys.begin(), quotaClassKeys.end(), value) != quotaClassKeys.end(); }

    // quotaBlockForClass's candidates are, in order, the class's own entry, the shared unknown
    // entry and the legacy slot-wide deadline; quotaBlockKeyForClass asks which one won.
    enum class BlockOrigin { exact, model, unknown, legacy };
    struct ClassBlockChoice {
      CodexQuotaClassBlock block;
      BlockOrigin origin;
    };

    std::optional<ClassBlockChoice> chooseQuotaBlockForClass(const CodexRoutingSlot& slot, std::string_view quotaClassKey,
                                                           std::optional<std::string_view> model) {
      const CodexQuotaClassBlock* exact = slot.quota_blocks_by_class ? slot.quota_blocks_by_class->get(quotaClassKey) : nullptr;
      const CodexQuotaClassBlock* unknown = quotaClassKey == "unknown" || !slot.quota_blocks_by_class ? nullptr : slot.quota_blocks_by_class->get("unknown");
      const std::vector<std::string> legacyClasses = slot.quota_blocked_classes.value_or(std::vector<std::string>{});
      const bool hasClassMap = hasEntries(slot.quota_blocks_by_class);
      const auto listed = [&legacyClasses](std::string_view key) { return std::find(legacyClasses.begin(), legacyClasses.end(), key) != legacyClasses.end(); };
      // Records written before class-aware routing have only a slot-wide deadline
      // plus the classes observed so far. Preserve those named blocks during the
      // one-time migration instead of dispatching directly into a known circuit.
      std::optional<CodexQuotaClassBlock> legacy;
      if (!hasClassMap && slot.quota_blocked_until_ms && slot.quota_block_source &&
          (legacyClasses.empty() || quotaClassKey == "unknown" || listed("unknown") || listed(quotaClassKey))) {
        legacy = legacyBlockOf(slot, *slot.quota_blocked_until_ms);
      }
      std::optional<ClassBlockChoice> latest;
      const auto consider = [&latest](const CodexQuotaClassBlock& block, BlockOrigin origin) {
        if (!latest || block.blocked_until_ms > latest->block.blocked_until_ms) latest = ClassBlockChoice{block, origin};
      };
      if (exact != nullptr) consider(*exact, BlockOrigin::exact);
      if (unknown != nullptr) consider(*unknown, BlockOrigin::unknown);
      if (legacy) consider(*legacy, BlockOrigin::legacy);
      if (model && slot.quota_blocks_by_class) {
        const auto* scoped = slot.quota_blocks_by_class->get(capacityModelScope(model));
        // A class-wide 429 cannot shorten a separate named quota window.
        if (scoped) consider(*scoped, BlockOrigin::model);
      }
      return latest;
    }

    // The TS also drops the unknown entry when it is the very same object as the class entry;
    // the only reader takes the maximum signal, which that cannot change.
    std::vector<CodexQuotaClassBlock> quotaBlocksForClass(const CodexRoutingSlot& slot, std::string_view quotaClassKey,
                                                         std::optional<std::string_view> model) {
      const CodexRoutingSlot classAwareSlot = withLegacyQuotaClassMap(slot);
      std::vector<CodexQuotaClassBlock> candidates;
      if (classAwareSlot.quota_blocks_by_class) {
        if (model) {
          if (const auto* scoped = classAwareSlot.quota_blocks_by_class->get(capacityModelScope(model))) candidates.push_back(*scoped);
        }
        if (const CodexQuotaClassBlock* exact = classAwareSlot.quota_blocks_by_class->get(quotaClassKey)) candidates.push_back(*exact);
        if (quotaClassKey != "unknown") {
          if (const CodexQuotaClassBlock* unknown = classAwareSlot.quota_blocks_by_class->get("unknown")) candidates.push_back(*unknown);
        }
      }
      return candidates;
    }

    bool capacityHasAnyPositiveHeadroom(const CodexCapacityRoutingObservation& observation, std::optional<std::string_view> model) {
      const std::optional<double> headroom = capacityHeadroomForObservation(observation, model);
      return observation.state == "available" && headroom && *headroom > 0;
    }

    std::optional<CodexCapacityRoutingObservation> parseStoredCapacityObservation(const json::value* value) {
      const std::optional<double> rawSlot = rt::getNumber(member(value, "slot"));
      const std::optional<double> slot =
        isRecord(value) && rawSlot && rt::Number::isInteger(*rawSlot) && *rawSlot >= 0 && *rawSlot <= 1 ? rawSlot : std::nullopt;
      if (!isRecord(value) || !slot) return std::nullopt;
      const json::value* accountIdHash = member(value, "account_id_hash");
      if (accountIdHash == nullptr || !accountIdHash->is_string() || accountIdHash->get_string().empty()) return std::nullopt;
      const std::optional<std::string> state = capacityState(member(value, "state"));
      const std::optional<double> snapshotAtMs = isSafeMs(member(value, "snapshot_at_ms")) ? rt::getNumber(member(value, "snapshot_at_ms")) : std::nullopt;
      const json::value* source = member(value, "source_observed_at_ms");
      const std::optional<double> sourceObservedAtMs = isSafeMs(source) ? rt::getNumber(source) : std::nullopt;
      const json::value* windows = isRecord(member(value, "windows")) ? member(value, "windows") : nullptr;
      if (!rt::truthy(state) || !snapshotAtMs || windows == nullptr) return std::nullopt;
      return CodexCapacityRoutingObservation{
        *slot,
        std::string(accountIdHash->get_string()),
        *state,
        sourceObservedAtMs,
        *snapshotAtMs,
        CodexCapacityRoutingWindows{normalizeCapacityWindow(member(windows, "primary")), normalizeCapacityWindow(member(windows, "secondary"))},
        normalizeCapacityAdditionalRateLimits(member(value, "additional_rate_limits")),
      };
    }

    CodexRoutingSlot capacityObservedSlot(const CodexRoutingSlot& current, const CodexRoutingSlot& capacityClearedSlot, const CodexCapacityRoutingObservation& observation,
                                          bool clearCircuit) {
      CodexRoutingSlot observed = capacityClearedSlot;
      observed.primary_used_percent = observation.windows.primary ? observation.windows.primary->used_percent : std::nullopt;
      observed.secondary_used_percent = observation.windows.secondary ? observation.windows.secondary->used_percent : std::nullopt;
      observed.capacity_observed_at_ms = observation.snapshot_at_ms;
      if (!clearCircuit) return observed;
      CodexRoutingSlot cleared = observed;
      cleared.account_id_hash = observation.account_id_hash;
      const bool preserveResetSafety = current.banked_reset_generation_ambiguous || current.banked_reset_recovery_probe_pending || current.probe_lease.has_value();
      if (preserveResetSafety) {
        cleared.observed_reset_at_ms = current.observed_reset_at_ms;
        cleared.observed_reset_at_is_stable = current.observed_reset_at_is_stable;
        cleared.banked_reset_generation_ambiguous = current.banked_reset_generation_ambiguous;
        cleared.banked_reset_recovery_probe_pending = current.banked_reset_recovery_probe_pending;
        cleared.probe_lease = current.probe_lease;
        return cleared;
      }
      if (hasEntries(capacityClearedSlot.quota_blocks_by_class)) return cleared;
      cleared.observed_reset_at_ms = std::nullopt;
      cleared.observed_reset_at_is_stable = false;
      cleared.banked_reset_generation_ambiguous = false;
      cleared.banked_reset_recovery_probe_pending = false;
      cleared.generation = current.generation + 1;
      cleared.probe_lease = std::nullopt;
      return cleared;
    }
  }

  CodexRoutingSlot neutralSlot(std::string credentialVersion, std::optional<std::string> accountIdHash) {
    CodexRoutingSlot slot;
    slot.account_id_hash = std::move(accountIdHash);
    slot.credential_version = std::move(credentialVersion);
    slot.quota_blocked_until_ms = std::nullopt;
    slot.quota_block_source = std::nullopt;
    slot.quota_blocked_classes = std::vector<std::string>{};
    slot.quota_blocks_by_class = CodexQuotaBlocksByClass{};
    slot.invalid_credential_version = std::nullopt;
    slot.primary_used_percent = std::nullopt;
    slot.secondary_used_percent = std::nullopt;
    slot.quota_signal_observed_at_ms = std::nullopt;
    slot.capacity_observed_at_ms = std::nullopt;
    slot.upstream_timeout_blocked_until_ms = nullptr;
    slot.observed_reset_at_ms = std::nullopt;
    slot.observed_reset_at_is_stable = false;
    slot.banked_reset_generation_ambiguous = false;
    slot.banked_reset_recovery_probe_pending = false;
    slot.generation = 0;
    slot.probe_lease = std::nullopt;
    return slot;
  }

  CodexAccountRoutingState withoutLegacyTimeoutCircuits(const CodexAccountRoutingState& state) {
    bool changed = false;
    std::vector<CodexRoutingSlot> slots;
    for (const CodexRoutingSlot& slot : state.slots) {
      std::optional<CodexRoutingSlot> next = withoutLegacyTimeoutCircuitChange(slot);
      changed = changed || next.has_value();
      slots.push_back(next ? std::move(*next) : slot);
    }
    if (!changed) return state;
    CodexAccountRoutingState next = state;
    next.slots = std::move(slots);
    return next;
  }

  CodexRoutingSlot rotateCredentialForSameAccount(const CodexRoutingSlot& slot, const CodexRoutingAccountIdentity& identity) {
    const double generation = slot.generation + 1;
    std::optional<CodexQuotaClassBlock> preservedBlock;
    if (slot.observed_reset_at_ms && slot.quota_block_source) {
      preservedBlock = legacyBlockOf(slot, slot.quota_blocked_until_ms.value_or(*slot.observed_reset_at_ms));
    }
    if (slot.quota_blocks_by_class) {
      for (const auto& [key, block] : *slot.quota_blocks_by_class) keepLatest(preservedBlock, block);
    }
    CodexRoutingSlot next = slot;
    next.account_id_hash = identity.accountIdHash;
    next.credential_version = identity.credentialVersion;
    next.quota_blocked_until_ms = std::nullopt;
    next.quota_block_source = std::nullopt;
    next.quota_blocked_classes = std::vector<std::string>{};
    next.quota_blocks_by_class = CodexQuotaBlocksByClass{};
    next.invalid_credential_version = std::nullopt;
    next.primary_used_percent = std::nullopt;
    next.secondary_used_percent = std::nullopt;
    next.quota_signal_observed_at_ms = std::nullopt;
    next.capacity_observed_at_ms = std::nullopt;
    next.observed_reset_at_ms = preservedBlock ? preservedBlock->observed_reset_at_ms : std::nullopt;
    next.observed_reset_at_is_stable = preservedBlock ? preservedBlock->observed_reset_at_is_stable : false;
    next.banked_reset_generation_ambiguous = preservesStableResetIdentity(slot) || (preservedBlock && preservedBlock->banked_reset_generation_ambiguous);
    next.banked_reset_recovery_probe_pending = slot.banked_reset_recovery_probe_pending || (preservedBlock && preservedBlock->banked_reset_recovery_probe_pending);
    next.generation = generation;
    // A refresh does not end an already-dispatched half-open probe. Transfer
    // its lease to the new credential and fence completions to this generation.
    if (slot.probe_lease) next.probe_lease->generation = generation;
    return next;
  }

  CodexRoutingSlot rotateCredentialForSameAccount(const CodexRoutingSlot& slot, const CodexRoutingIdentity& identity) {
    return rotateCredentialForSameAccount(slot, identityOf(identity));
  }

  CodexAccountRoutingState normalizeRoutingState(const CodexAccountRoutingState* raw, const std::vector<CodexRoutingIdentity>& identities, double now,
                                                 bool discardLegacyTimeoutCircuit) {
    static const std::vector<CodexRoutingSlot> noSlots;
    const std::vector<CodexRoutingSlot>& priorSlots = raw != nullptr ? raw->slots : noSlots;
    std::unordered_set<std::size_t> usedPriorSlots;
    const auto takePrior = [&priorSlots, &usedPriorSlots](const auto& matches) -> std::optional<CodexRoutingSlot> {
      for (std::size_t index = 0; index < priorSlots.size(); ++index) {
        if (usedPriorSlots.contains(index) || !matches(priorSlots[index])) continue;
        usedPriorSlots.insert(index);
        return priorSlots[index];
      }
      return std::nullopt;
    };
    std::vector<CodexRoutingSlot> slots;
    for (std::size_t index = 0; index < identities.size(); ++index) {
      const CodexRoutingIdentity& identity = identities[index];
      const CodexRoutingSlot* direct = index < priorSlots.size() ? &priorSlots[index] : nullptr;
      std::optional<CodexRoutingSlot> prior;
      if (direct != nullptr && !usedPriorSlots.contains(index) &&
          (direct->account_id_hash == identity.accountIdHash || (!direct->account_id_hash && direct->credential_version == identity.credentialVersion))) {
        usedPriorSlots.insert(index);
        prior = *direct;
      }
      if (!prior) prior = takePrior([&identity](const CodexRoutingSlot& slot) { return slot.account_id_hash == identity.accountIdHash; });
      // Version equality is the only safe way to attach an account identity to a
      // legacy record that predates the opaque account hash.
      if (!prior) prior = takePrior([&identity](const CodexRoutingSlot& slot) { return !slot.account_id_hash && slot.credential_version == identity.credentialVersion; });
      if (!prior) {
        slots.push_back(neutralSlot(identity.credentialVersion, identity.accountIdHash));
        continue;
      }
      if (discardLegacyTimeoutCircuit) prior = withoutLegacyTimeoutCircuit(*prior);
      if (prior->credential_version == identity.credentialVersion) {
        slots.push_back(withoutLegacyTimeoutCircuit(prior->account_id_hash == identity.accountIdHash ? *prior : attachLegacyAccountIdentity(*prior, identityOf(identity))));
        continue;
      }
      slots.push_back(withoutLegacyTimeoutCircuit(rotateCredentialForSameAccount(*prior, identityOf(identity))));
    }
    // Do not attach an unidentifiable legacy identity to whichever account now
    // occupies its old slot. A global claim fence keeps ordinary routing usable
    // while preventing a same-account token rotation from minting a new key.
    bool legacyIdentityUnresolved = raw != nullptr && raw->banked_reset_legacy_identity_unresolved;
    for (std::size_t index = 0; index < priorSlots.size() && !legacyIdentityUnresolved; ++index) {
      legacyIdentityUnresolved = !usedPriorSlots.contains(index) && isLegacyStableResetIdentity(priorSlots[index]);
    }
    CodexAccountRoutingState state;
    state.v = 2;
    state.updated_at_ms = now;
    state.banked_reset_legacy_identity_unresolved = legacyIdentityUnresolved;
    state.slots = std::move(slots);
    return state;
  }

  CodexRoutingSlot slotFor(const CodexAccountRoutingState& state, const CodexRoutingAccount& account) {
    const CodexRoutingSlot* slot = rt::element(state.slots, account.slot);
    return slot != nullptr ? *slot : neutralSlot(account.credentialVersion, account.accountIdHash);
  }

  bool slotMatchesCodexRoutingAccount(const CodexRoutingSlot& slot, const CodexRoutingAccount& account) {
    return slot.credential_version == account.credentialVersion &&
           // Exact credential-version equality is the legacy proof of account scope;
           // every successful transition writes the opaque account hash immediately.
           (slot.account_id_hash == account.accountIdHash || !slot.account_id_hash);
  }

  CodexProbeCircuit routingProbeCircuit(const CodexRoutingAccount& account) { return account.probeCircuit.valueOr(CodexProbeCircuit("quota")); }

  bool probeLeaseMatchesCodexRoutingAccount(const CodexRoutingSlot& slot, const CodexRoutingAccount& account) {
    const std::optional<CodexProbeLease>& lease = slot.probe_lease;
    if (!account.probeGeneration || !lease) return false;
    if (slot.generation != *account.probeGeneration) return false;
    if (lease->generation != *account.probeGeneration || !account.probeToken || lease->token != *account.probeToken) return false;
    if (lease->circuit != routingProbeCircuit(account)) return false;
    if (routingProbeCircuit(account) != "quota") return true;
    return lease->quota_class.isNullish() || codexQuotaScopeApplies(*lease->quota_class, requestedModelView(account.requestedModel));
  }

  CodexAccountRoutingState withSlot(const CodexAccountRoutingState& state, double index, const CodexRoutingSlot& slot, double updatedAtMs) {
    std::vector<CodexRoutingSlot> slots = state.slots;
    while (static_cast<double>(slots.size()) <= index) slots.push_back(neutralSlot(slot.credential_version, slot.account_id_hash));
    // `slots[index] = slot` with an index that is not an array index sets a property no reader sees.
    if (index >= 0 && std::trunc(index) == index && index < static_cast<double>(slots.size())) slots[static_cast<std::size_t>(index)] = slot;
    CodexAccountRoutingState next = state;
    next.updated_at_ms = updatedAtMs;
    next.slots = std::move(slots);
    return next;
  }

  std::optional<double> parseFinitePercent(const std::optional<std::string>& value) {
    if (!value) return std::nullopt;
    const double parsed = rt::stringToNumber(*value);
    if (rt::Number::isFinite(parsed) && parsed >= 0 && parsed <= 100) return parsed;
    return std::nullopt;
  }

  std::optional<double> quotaHeadroomFor(const CodexRoutingSlot& slot) {
    std::vector<double> headrooms;
    for (const std::optional<double>* used : {&slot.primary_used_percent, &slot.secondary_used_percent}) {
      if (*used) headrooms.push_back(100 - **used);
    }
    if (headrooms.empty()) return std::nullopt;
    return rt::Math::max(0, rt::Math::min(std::span<const double>(headrooms)));
  }

  std::optional<std::string> capacityState(const json::value* value) {
    if (value == nullptr || !value->is_string()) return std::nullopt;
    const json::string& state = value->get_string();
    if (state == "available" || state == "stale" || state == "unavailable") return std::string(state);
    return std::nullopt;
  }

  std::optional<CodexCapacityRoutingWindow> normalizeCapacityWindow(const json::value* value) {
    if (value == nullptr || value->is_null()) return std::nullopt;
    if (!isRecord(value)) return std::nullopt;
    return CodexCapacityRoutingWindow{
      capacityWindowSeconds(member(value, "limit_window_seconds")),
      capacityPercent(member(value, "used_percent")),
      capacityResetAtMs(member(value, "reset_at_ms")),
    };
  }

  std::vector<CodexCapacityRoutingAdditionalRateLimit> normalizeCapacityAdditionalRateLimits(const json::value* value) {
    std::vector<CodexCapacityRoutingAdditionalRateLimit> limits;
    if (value == nullptr || !value->is_array()) return limits;
    for (const json::value& candidate : value->get_array()) {
      std::optional<CodexCapacityRoutingAdditionalRateLimit> parsed = normalizeCapacityAdditionalRateLimit(&candidate);
      if (parsed) limits.push_back(std::move(*parsed));
    }
    return limits;
  }

  CodexQuotaClass quotaClass(std::optional<std::string_view> model) {
    const std::string normalized = model ? rt::toLowerCase(rt::trim(*model)) : std::string();
    if (normalized == "gpt-5.3-codex-spark") return "spark";
    if (normalized == "gpt-oss-120b") return "gpt_oss_120b";
    // `gpt-reserve` is the owner-authorized second id for luna. It keeps its own
    // bucket so exhausting reserve can never fence the standard class, and the
    // compact form matches however the id is spelled at the call site.
    if (normalizeQuotaLabel(normalized) == "gptreserve") return "reserve";
    return !normalized.empty() ? "standard" : "unknown";
  }

  CodexQuotaClass quotaClass(const rt::Nullable<std::string>& model) {
    return model.hasValue() ? quotaClass(std::optional<std::string_view>(*model)) : quotaClass(std::optional<std::string_view>());
  }

  std::optional<std::string_view> requestedModelView(const rt::Nullable<std::string>& model) {
    return model.hasValue() ? std::optional<std::string_view>(*model) : std::nullopt;
  }

  bool codexQuotaScopeApplies(std::string_view scope, std::optional<std::string_view> model) {
    return isCodexCapacityModelScope(scope) ? scope == capacityModelScope(model) : scope == quotaClass(model);
  }

  std::optional<CodexQuotaClassBlock> quotaBlockForClass(const CodexRoutingSlot& slot, std::string_view quotaClassKey, std::optional<std::string_view> model) {
    std::optional<ClassBlockChoice> choice = chooseQuotaBlockForClass(slot, quotaClassKey, model);
    if (!choice) return std::nullopt;
    return std::move(choice->block);
  }

  CodexQuotaBlocksByClass quotaBlocksIncludingLegacy(const CodexRoutingSlot& slot) {
    CodexQuotaBlocksByClass blocks = slot.quota_blocks_by_class.value_or(CodexQuotaBlocksByClass{});
    if (!slot.quota_blocked_until_ms || !slot.quota_block_source) return blocks;
    std::vector<std::string> legacyClasses;
    for (const std::string& key : slot.quota_blocked_classes.value_or(std::vector<std::string>{}))
      if (isQuotaClassKey(key)) legacyClasses.push_back(key);
    // An unclassified legacy deadline is conservatively copied to every quota
    // class so clearing one class cannot release the other separately metered
    // pools. The synthetic unknown entry remains the slot-wide fallback for
    // callers that do not provide a model.
    std::vector<std::string> classes = legacyClasses;
    if (classes.empty()) classes.assign(quotaClassKeys.begin(), quotaClassKeys.end());
    const CodexQuotaClassBlock legacyBlock = legacyBlockOf(slot, *slot.quota_blocked_until_ms);
    for (const std::string& quotaClassKey : classes) {
      if (!blocks.has(quotaClassKey)) blocks.set(quotaClassKey, legacyBlock);
    }
    return blocks;
  }

  CodexRoutingSlot withLegacyQuotaClassMap(const CodexRoutingSlot& slot) {
    CodexQuotaBlocksByClass quotaBlocksByClass = quotaBlocksIncludingLegacy(slot);
    if (quotaBlocksByClass.size() == 0) return slot;
    CodexRoutingSlot next = slot;
    next.quota_blocked_classes = keysOf(quotaBlocksByClass);
    next.quota_blocks_by_class = std::move(quotaBlocksByClass);
    return next;
  }

  std::optional<double> quotaSignalObservedAtForClass(const CodexRoutingSlot& slot, std::string_view quotaClassKey, std::optional<std::string_view> model) {
    std::vector<double> signals;
    for (const CodexQuotaClassBlock& block : quotaBlocksForClass(slot, quotaClassKey, model)) {
      const std::optional<double> signal = block.quota_signal_observed_at_ms ? block.quota_signal_observed_at_ms : slot.quota_signal_observed_at_ms;
      if (signal) signals.push_back(*signal);
    }
    if (signals.empty()) return slot.quota_signal_observed_at_ms;
    return rt::Math::max(std::span<const double>(signals));
  }

  std::optional<CodexQuotaClass> quotaBlockKeyForClass(const CodexRoutingSlot& slot, std::string_view quotaClassKey, std::optional<std::string_view> model) {
    const std::optional<ClassBlockChoice> choice = chooseQuotaBlockForClass(slot, quotaClassKey, model);
    if (!choice) return std::nullopt;
    if (choice->origin == BlockOrigin::model) return capacityModelScope(model);
    if (choice->origin == BlockOrigin::exact) return CodexQuotaClass(quotaClassKey);
    // An independent unknown fence protects every model class. A known-class
    // recovery can release the shared fallback only when migration marked it as
    // a synthetic copy of the legacy slot-wide fence.
    if (quotaClassKey != "unknown" && choice->origin == BlockOrigin::unknown && choice->block.legacy_fallback) return CodexQuotaClass("unknown");
    return std::nullopt;
  }

  CodexRoutingSlot withoutQuotaClass(const CodexRoutingSlot& slot, std::string_view quotaClassKey) {
    // Rebuilt rather than copied-and-deleted: the map keeps a plain data shape,
    // and an unknown-class legacy fallback is dropped with the requested class.
    const CodexQuotaBlocksByClass currentBlocks = slot.quota_blocks_by_class.value_or(CodexQuotaBlocksByClass{});
    const CodexQuotaClassBlock* currentUnknown = currentBlocks.get("unknown");
    const bool dropUnknownLegacyFallback = !isCodexCapacityModelScope(quotaClassKey) && quotaClassKey != "unknown" &&
                                           currentUnknown != nullptr && currentUnknown->legacy_fallback;
    CodexQuotaBlocksByClass quotaBlocksByClass;
    for (const auto& [key, block] : currentBlocks) {
      if (key != quotaClassKey && !(dropUnknownLegacyFallback && key == "unknown")) quotaBlocksByClass.set(key, block);
    }
    std::optional<CodexQuotaClassBlock> latest;
    std::vector<double> observedQuotaSignals;
    bool ambiguous = false;
    bool pending = false;
    for (const auto& [key, block] : quotaBlocksByClass) {
      if (isCodexCapacityModelScope(key)) continue;
      keepLatest(latest, block);
      if (block.quota_signal_observed_at_ms) observedQuotaSignals.push_back(*block.quota_signal_observed_at_ms);
      ambiguous = ambiguous || block.banked_reset_generation_ambiguous;
      pending = pending || block.banked_reset_recovery_probe_pending;
    }
    CodexRoutingSlot next = slot;
    next.quota_blocked_classes = keysOf(quotaBlocksByClass);
    next.quota_blocks_by_class = std::move(quotaBlocksByClass);
    next.quota_blocked_until_ms = latest ? std::optional<double>(latest->blocked_until_ms) : std::nullopt;
    next.quota_block_source = latest ? std::optional<CodexQuotaBlockSource>(latest->source) : std::nullopt;
    next.quota_signal_observed_at_ms =
      observedQuotaSignals.empty() ? slot.quota_signal_observed_at_ms : std::optional<double>(rt::Math::max(std::span<const double>(observedQuotaSignals)));
    next.observed_reset_at_ms = latest ? latest->observed_reset_at_ms : std::nullopt;
    next.observed_reset_at_is_stable = latest ? latest->observed_reset_at_is_stable : false;
    next.banked_reset_generation_ambiguous = ambiguous;
    next.banked_reset_recovery_probe_pending = pending;
    return next;
  }

  CodexRoutingSlot recheckQuotaClasses(const CodexRoutingSlot& slot, double recheckAtMs) {
    if (!hasEntries(slot.quota_blocks_by_class)) {
      CodexRoutingSlot next = slot;
      // Preserve the legacy class list until a model-specific transition can
      // migrate it. Rechecking must not erase the only evidence that names
      // which old quota pool was blocked.
      next.quota_blocked_until_ms = slot.quota_blocked_until_ms ? std::optional<double>(recheckAtMs) : std::nullopt;
      next.banked_reset_generation_ambiguous = slot.quota_blocked_until_ms.has_value() || slot.banked_reset_generation_ambiguous;
      return next;
    }
    CodexQuotaBlocksByClass quotaBlocksByClass;
    for (const auto& [key, block] : *slot.quota_blocks_by_class) {
      CodexQuotaClassBlock rechecked = block;
      rechecked.blocked_until_ms = recheckAtMs;
      rechecked.quota_signal_observed_at_ms = recheckAtMs;
      rechecked.banked_reset_generation_ambiguous = true;
      quotaBlocksByClass.set(key, rechecked);
    }
    std::optional<CodexQuotaClassBlock> latest;
    for (const auto& [key, block] : quotaBlocksByClass) {
      if (!isCodexCapacityModelScope(key)) keepLatest(latest, block);
    }
    CodexRoutingSlot next = slot;
    next.quota_blocked_until_ms = latest ? std::optional<double>(latest->blocked_until_ms)
                                         : (rt::truthy(slot.quota_blocked_until_ms) ? std::optional<double>(recheckAtMs) : std::nullopt);
    next.quota_block_source = latest ? std::optional<CodexQuotaBlockSource>(latest->source) : slot.quota_block_source;
    next.quota_blocked_classes = keysOf(quotaBlocksByClass);
    next.observed_reset_at_ms = latest && latest->observed_reset_at_ms ? latest->observed_reset_at_ms : slot.observed_reset_at_ms;
    next.observed_reset_at_is_stable = latest ? latest->observed_reset_at_is_stable : slot.observed_reset_at_is_stable;
    next.quota_blocks_by_class = std::move(quotaBlocksByClass);
    return next;
  }

  CodexRoutingSlot releaseQuotaClassProbe(const CodexRoutingSlot& slot, std::string_view quotaClassKey, std::optional<std::string_view>) {
    const CodexRoutingSlot classAwareSlot = withLegacyQuotaClassMap(slot);
    const auto claimed = slot.probe_lease ? slot.probe_lease->quota_class.toOptional() : std::nullopt;
    const bool namedClaim = claimed && isCodexCapacityModelScope(*claimed);
    const auto key = namedClaim ? *claimed : quotaBlockKeyForClass(classAwareSlot, quotaClassKey).value_or(std::string(quotaClassKey));
    const auto* block = namedClaim && classAwareSlot.quota_blocks_by_class ? classAwareSlot.quota_blocks_by_class->get(key) : nullptr;
    const auto classBlock = namedClaim ? std::nullopt : quotaBlockForClass(classAwareSlot, quotaClassKey);
    if (classBlock) block = &*classBlock;
    CodexRoutingSlot released = withoutQuotaClass(classAwareSlot, key);
    if (!classAwareSlot.banked_reset_recovery_probe_pending) return released;
    const bool pending = released.banked_reset_recovery_probe_pending;
    released.observed_reset_at_ms = block && block->observed_reset_at_ms ? block->observed_reset_at_ms : slot.observed_reset_at_ms;
    released.observed_reset_at_is_stable = block ? block->observed_reset_at_is_stable : slot.observed_reset_at_is_stable;
    released.banked_reset_generation_ambiguous = true;
    released.banked_reset_recovery_probe_pending = pending;
    return released;
  }

  std::optional<double> capacityHeadroomForObservation(const CodexCapacityRoutingObservation& observation, std::optional<std::string_view> model) {
    std::optional<double> best;
    for (const auto* windows : capacityWindowsForModel(observation, model)) {
      const auto headroom = capacityHeadroomForWindows(*windows);
      if (headroom && (!best || *headroom > *best)) best = headroom;
    }
    return best;
  }

  bool capacityObservationIsFresh(const CodexCapacityRoutingObservation& observation, double now) {
    return observation.state == "available" && isSafeMs(observation.snapshot_at_ms) && observation.snapshot_at_ms <= now &&
           now - observation.snapshot_at_ms <= CODEX_CAPACITY_ROUTING_MAX_AGE_MS;
  }

  bool hasModelScopedQuotaBlock(const CodexRoutingSlot& slot, std::optional<std::string_view> model) {
    return model && slot.quota_blocks_by_class && slot.quota_blocks_by_class->has(capacityModelScope(model));
  }

  std::optional<CodexRoutingSlot> recoverQuotaScopesFromCapacity(const CodexRoutingSlot& slot, const CodexCapacityRoutingObservation& observation,
                                                                 std::optional<std::string_view> model) {
    const auto quotaClassKey = quotaClass(model);
    const auto modelScope = capacityModelScope(model);
    const auto* namedBlock = slot.quota_blocks_by_class ? slot.quota_blocks_by_class->get(modelScope) : nullptr;
    const auto classSignal = quotaSignalObservedAtForClass(slot, quotaClassKey);
    const auto isNewer = [&](const std::optional<double>& signal) { return !signal || *signal < observation.snapshot_at_ms; };
    const auto classEvidence = namedBlock ? std::nullopt : model;
    std::optional<CodexRoutingSlot> recovered;
    if (capacityHasAnyPositiveHeadroom(observation, classEvidence) && isNewer(classSignal)) {
      const auto key = quotaBlockKeyForClass(slot, quotaClassKey);
      if (!namedBlock || key) recovered = withoutQuotaClass(slot, key.value_or(quotaClassKey));
    }
    if (namedBlock && isNewer(namedBlock->quota_signal_observed_at_ms) && capacityHasAnyPositiveHeadroom(observation, model) &&
        capacityWindowsForModel(observation, model).front() != &observation.windows) {
      recovered = withoutQuotaClass(recovered ? *recovered : slot, modelScope);
    }
    return recovered;
  }

  bool capacityHasCurrentExhaustedWindow(const CodexCapacityRoutingObservation& observation, std::optional<std::string_view> model, double now) {
    for (const auto* windows : capacityWindowsForModel(observation, model)) {
      for (const auto* window : {&windows->primary, &windows->secondary}) {
        if (*window && (*window)->used_percent == 100 && (!isSafeMs((*window)->reset_at_ms) || *(*window)->reset_at_ms > now)) return true;
      }
    }
    return false;
  }

  bool capacityLimitMatchesModel(std::string_view limitName, std::string_view model) {
    const std::string modelKey = normalizeQuotaLabel(model);
    return !modelKey.empty() && normalizeQuotaLabel(limitName) == modelKey;
  }

  std::optional<double> capacityExhaustedResetAtMs(const CodexCapacityRoutingObservation& observation, std::optional<std::string_view> model, double now) {
    if (!capacityObservationIsFresh(observation, now)) return std::nullopt;
    std::optional<double> deadline;
    for (const auto* windows : capacityWindowsForModel(observation, model)) {
      // Both windows in one limit apply. Multiple matching limits describe alternatives
      // and must agree before they can establish a single reset ledger identity.
      const auto resetAtMs = exhaustedWindowResetAtMs(*windows, now);
      if (!resetAtMs || (deadline && *resetAtMs != *deadline)) return std::nullopt;
      deadline = resetAtMs;
    }
    return deadline;
  }

  std::vector<CodexCapacityRoutingObservation> parseStoredCapacityObservationStore(const json::value* value) {
    std::vector<CodexCapacityRoutingObservation> observations;
    const json::value* rows = member(value, "observations");
    const std::optional<double> version = rt::getNumber(member(value, "v"));
    if (!isRecord(value) || !version || *version != 1 || rows == nullptr || !rows->is_array()) return observations;
    for (const json::value& candidate : rows->get_array()) {
      std::optional<CodexCapacityRoutingObservation> parsed = parseStoredCapacityObservation(&candidate);
      if (parsed) observations.push_back(std::move(*parsed));
    }
    return observations;
  }

  std::vector<CodexCapacityRoutingObservation> parseStoredCapacityObservationStore(const json::value& value) { return parseStoredCapacityObservationStore(&value); }

  std::vector<CodexCapacityRoutingObservation> mergeCapacityObservations(std::span<const std::vector<CodexCapacityRoutingObservation>> sets) {
    rt::OrderedMap<std::string, CodexCapacityRoutingObservation> byAccount;
    for (const std::vector<CodexCapacityRoutingObservation>& set : sets) {
      for (const CodexCapacityRoutingObservation& observation : set) {
        const CodexCapacityRoutingObservation* prior = byAccount.get(observation.account_id_hash);
        if (prior == nullptr || observation.snapshot_at_ms >= prior->snapshot_at_ms) byAccount.set(observation.account_id_hash, observation);
      }
    }
    std::vector<CodexCapacityRoutingObservation> merged;
    for (const auto& [key, observation] : byAccount) merged.push_back(observation);
    return rt::sort(merged, [](const CodexCapacityRoutingObservation& left, const CodexCapacityRoutingObservation& right) { return left.slot - right.slot; });
  }

  std::vector<CodexCapacityRoutingObservation> retainRecentCapacityObservations(const std::vector<CodexCapacityRoutingObservation>& observations, double now) {
    const double oldestRetainedAtMs = rt::Math::max(0, now - CODEX_CAPACITY_ROUTING_MAX_AGE_MS);
    std::vector<CodexCapacityRoutingObservation> retained;
    for (const CodexCapacityRoutingObservation& observation : observations) {
      if (observation.snapshot_at_ms >= oldestRetainedAtMs && observation.snapshot_at_ms <= now) retained.push_back(observation);
    }
    return retained;
  }

  std::optional<CodexAccountRoutingState> applyCapacityQuotaBlock(const CodexAccountRoutingState& state, const CodexRoutingAccount& account,
                                                                  const CodexCapacityRoutingObservation& observation, double now, double updatedAtMs) {
    if (observation.slot != account.slot || observation.account_id_hash != account.accountIdHash) return std::nullopt;
    const std::optional<std::string_view> requestedModel = account.requestedModel.hasValue() ? std::optional<std::string_view>(*account.requestedModel) : std::nullopt;
    const auto resetAtMs = capacityExhaustedResetAtMs(observation, requestedModel, now);
    if (!resetAtMs) return std::nullopt;
    const CodexRoutingSlot current = slotFor(state, account);
    if (!slotMatchesCodexRoutingAccount(current, account) || current.probe_lease) return std::nullopt;
    const CodexQuotaClass requestedQuotaClass = quotaClass(account.requestedModel);
    const CodexRoutingSlot classAwareCurrent = withLegacyQuotaClassMap(current);
    const bool named = requestedModel && capacityWindowsForModel(observation, requestedModel).front() != &observation.windows;
    const CodexQuotaClass scope = named ? capacityModelScope(requestedModel) : requestedQuotaClass;
    const std::optional<double> priorSignalObservedAtMs = quotaSignalObservedAtForClass(classAwareCurrent, requestedQuotaClass, requestedModel);
    // A request that reached the provider after the sample may have established
    // a newer authoritative circuit. The older sample cannot replace it.
    if (priorSignalObservedAtMs && *priorSignalObservedAtMs > observation.snapshot_at_ms) return std::nullopt;
    CodexQuotaBlocksByClass quotaBlocksByClass = quotaBlocksIncludingLegacy(current);
    const auto* priorBlock = quotaBlocksByClass.get(scope);
    const bool recoveryProbePending = current.banked_reset_recovery_probe_pending || (priorBlock && priorBlock->banked_reset_recovery_probe_pending);
    const CodexQuotaClassBlock nextBlock{*resetAtMs, "header_retry_after", false, observation.snapshot_at_ms, *resetAtMs, true, false, recoveryProbePending};
    if (priorBlock && priorBlock->blocked_until_ms == nextBlock.blocked_until_ms && priorBlock->source == nextBlock.source &&
        priorBlock->observed_reset_at_ms == nextBlock.observed_reset_at_ms && priorBlock->observed_reset_at_is_stable && !priorBlock->banked_reset_generation_ambiguous &&
        priorBlock->banked_reset_recovery_probe_pending == recoveryProbePending)
      return std::nullopt;
    quotaBlocksByClass.set(scope, nextBlock);
    // Named observations are exact to a model. Do not project them into the slot-wide legacy fields.
    if (named) {
      auto blocked = current;
      blocked.quota_blocks_by_class = std::move(quotaBlocksByClass);
      blocked.generation = current.generation + 1;
      return withSlot(state, account.slot, blocked, updatedAtMs);
    }
    CodexQuotaClassBlock latest = nextBlock;
    std::vector<double> observedSignals;
    bool ambiguous = false;
    bool pending = false;
    for (const auto& [key, block] : quotaBlocksByClass) {
      if (isCodexCapacityModelScope(key)) continue;
      if (block.blocked_until_ms > latest.blocked_until_ms) latest = block;
      if (block.quota_signal_observed_at_ms) observedSignals.push_back(*block.quota_signal_observed_at_ms);
      ambiguous = ambiguous || block.banked_reset_generation_ambiguous;
      pending = pending || block.banked_reset_recovery_probe_pending;
    }
    const double latestSignalObservedAtMs = observedSignals.empty() ? observation.snapshot_at_ms : rt::Math::max(std::span<const double>(observedSignals));
    CodexAccountRoutingState next = state;
    next.updated_at_ms = updatedAtMs;
    for (std::size_t index = 0; index < next.slots.size(); ++index) {
      if (static_cast<double>(index) != account.slot) continue;
      CodexRoutingSlot blocked = current;
      blocked.account_id_hash = account.accountIdHash;
      blocked.quota_blocked_until_ms = latest.blocked_until_ms;
      blocked.quota_block_source = latest.source;
      blocked.quota_blocked_classes = keysOf(quotaBlocksByClass);
      blocked.quota_blocks_by_class = quotaBlocksByClass;
      blocked.quota_signal_observed_at_ms = latestSignalObservedAtMs;
      blocked.observed_reset_at_ms = latest.observed_reset_at_ms;
      blocked.observed_reset_at_is_stable = latest.observed_reset_at_is_stable;
      blocked.banked_reset_generation_ambiguous = ambiguous;
      blocked.banked_reset_recovery_probe_pending = pending;
      blocked.generation = current.generation + 1;
      next.slots[index] = std::move(blocked);
    }
    return next;
  }

  std::optional<CodexAccountRoutingState> applyCapacityObservation(const CodexAccountRoutingState& state, const CodexCapacityRoutingObservation& observation,
                                                                   double now, std::optional<std::string_view> model, double updatedAtMs) {
    if (observation.snapshot_at_ms > now) return std::nullopt;
    const CodexRoutingSlot* found = rt::at(state.slots, observation.slot);
    if (found == nullptr || found->account_id_hash != observation.account_id_hash) return std::nullopt;
    const CodexRoutingSlot& current = *found;
    if (current.capacity_observed_at_ms && observation.snapshot_at_ms < *current.capacity_observed_at_ms) return std::nullopt;
    const auto capacityClearedSlot = capacityObservationIsFresh(observation, now)
      ? recoverQuotaScopesFromCapacity(withLegacyQuotaClassMap(current), observation, model)
      : std::nullopt;
    const bool clearCircuit = capacityClearedSlot.has_value();
    const CodexRoutingSlot nextSlot = capacityObservedSlot(current, capacityClearedSlot ? *capacityClearedSlot : current, observation, clearCircuit);
    // The TS compares every member with `!==`. A cleared circuit always carries the class map
    // and class list withoutQuotaClass rebuilt (new objects); otherwise only the three observed
    // numbers can differ from the current slot's members.
    const bool changed = clearCircuit || nextSlot.primary_used_percent != current.primary_used_percent ||
                         nextSlot.secondary_used_percent != current.secondary_used_percent || nextSlot.capacity_observed_at_ms != current.capacity_observed_at_ms;
    if (!changed) return std::nullopt;
    return withSlot(state, observation.slot, nextSlot, updatedAtMs);
  }

  CodexCapacityObservationStorePlan planCapacityObservationStore(std::optional<std::string> capacityRevision, const std::vector<CodexCapacityRoutingObservation>& durable,
                                                                 const std::vector<CodexCapacityRoutingObservation>& observations, double now) {
    std::vector<CodexCapacityRoutingObservation> merged = retainRecentCapacityObservations(mergeCapacityObservations(durable, observations), now);
    CodexCapacityObservationStorePlan plan;
    plan.checks.capacity = std::move(capacityRevision);
    plan.writes.capacity = CodexCapacityObservationStore{1, now, std::move(merged)};
    return plan;
  }

  json::object toJson(const CodexCapacityObservationStore& store) {
    json::array observations;
    for (const CodexCapacityRoutingObservation& observation : store.observations) observations.push_back(native_codex_contracts::toJson(observation));
    json::object object;
    rt::objectDefine(object, "v", rt::makeNumber(store.v));
    rt::objectDefine(object, "updated_at_ms", rt::makeNumber(store.updated_at_ms));
    rt::objectDefine(object, "observations", std::move(observations));
    return object;
  }
}
