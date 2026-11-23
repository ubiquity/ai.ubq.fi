#include "uos/authority/native_codex_contracts.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace uos::authority::native_codex_contracts {
  namespace {
    using utils::isRecord;

    // `value.key` on an unknown value (arrays and primitives have no such member).
    const json::value* member(const json::value* value, std::string_view key) { return value != nullptr ? rt::get(*value, key) : nullptr; }

    // `a === b` for two unknown members (nullptr is undefined).
    bool same(const json::value* a, const json::value* b) {
      if (a == nullptr || b == nullptr) return a == b;
      return rt::strictEquals(*a, *b);
    }
    bool isNull(const json::value* value) { return value != nullptr && value->is_null(); }
    bool isTrue(const json::value* value) { return value != nullptr && value->is_bool() && value->get_bool(); }
    bool isFalse(const json::value* value) { return value != nullptr && value->is_bool() && !value->get_bool(); }
    bool numberIs(const json::value* value, double number) {
      const std::optional<double> actual = rt::getNumber(value);
      return actual && *actual == number;
    }
    bool stringIs(const json::value* value, std::string_view text) { return value != nullptr && value->is_string() && value->get_string() == text; }
    std::optional<std::string> stringOf(const json::value* value) {
      if (value == nullptr || !value->is_string()) return std::nullopt;
      return std::string(value->get_string());
    }

    bool quotaClassBlockFieldsMatch(const json::value* left, const json::value* right) {
      for (const char* key : {"blocked_until_ms", "source", "quota_signal_observed_at_ms", "observed_reset_at_ms", "observed_reset_at_is_stable",
                              "banked_reset_generation_ambiguous", "banked_reset_recovery_probe_pending"}) {
        if (!same(member(left, key), member(right, key))) return false;
      }
      return true;
    }

    /**
     * The previous class-migration release copied an unclassified slot fence into
     * every class, including `unknown`, without a marker. Recognize that exact
     * all-class shape so a later class recovery does not retain a synthetic
     * fallback forever. Explicitly marked entries are always authoritative.
     */
    bool hasUnmarkedSyntheticLegacyUnknown(const json::value* rawClassBlocks) {
      const json::value* unknown = member(rawClassBlocks, "unknown");
      if (!isRecord(unknown) || member(unknown, "legacy_fallback") != nullptr) return false;
      // This detects the shape one historical release wrote, so it stays the four
      // entries of that release; `reserve` did not exist then.
      constexpr std::array<std::string_view, 3> knownClassKeys = {"spark", "gpt_oss_120b", "standard"};
      for (const std::string_view key : knownClassKeys) {
        const json::value* candidate = member(rawClassBlocks, key);
        if (!(isRecord(candidate) && member(candidate, "legacy_fallback") == nullptr)) return false;
      }
      // The preceding migration copied one unmarked block into all four entries.
      // Require every known entry to retain that exact shape; one matching class
      // is not enough because independent 429s can share a deadline by chance.
      for (const std::string_view key : knownClassKeys) {
        const json::value* candidate = member(rawClassBlocks, key);
        if (!(isRecord(candidate) && quotaClassBlockFieldsMatch(candidate, unknown))) return false;
      }
      return true;
    }

    /**
     * A missing or unrecognized probe circuit is the legacy representation of a
     * quota probe. An explicitly unrecognized value still fails the whole slot.
     */
    std::optional<CodexProbeCircuit> parseProbeLeaseCircuit(const json::value* lease) {
      if (isNull(lease) || !isRecord(lease) || member(lease, "circuit") == nullptr) return CodexProbeCircuit("quota");
      const json::value* circuit = member(lease, "circuit");
      if (stringIs(circuit, "quota") || stringIs(circuit, "upstream_timeout")) return stringOf(circuit);
      return std::nullopt;
    }

    std::optional<CodexQuotaClass> parseProbeQuotaClass(const json::value* value) {
      for (const char* known : {"spark", "gpt_oss_120b", "reserve", "standard", "unknown"})
        if (stringIs(value, known)) return CodexQuotaClass(known);
      return std::nullopt;
    }

    std::optional<CodexProbeLease> parseProbeLease(const json::value* lease, const std::optional<CodexProbeCircuit>& leaseCircuit) {
      if (isNull(lease) || !isRecord(lease)) return std::nullopt;
      const std::optional<double> generation = rt::getNumber(member(lease, "generation"));
      if (!stringOf(member(lease, "token")) || !isSafeMs(member(lease, "expires_at_ms")) || !generation || !rt::Number::isSafeInteger(*generation) || !leaseCircuit) {
        return std::nullopt;
      }
      return CodexProbeLease{
        *stringOf(member(lease, "token")),
        *rt::getNumber(member(lease, "expires_at_ms")),
        *generation,
        *leaseCircuit,
        rt::orNull(parseProbeQuotaClass(member(lease, "quota_class"))),
      };
    }

    /** `undefined` marks an unrecognized value, which fails the slot it was read from. */
    rt::Nullable<CodexQuotaBlockSource> parseQuotaBlockSource(const json::value* value) {
      if (isNull(value)) return nullptr;
      if (stringIs(value, "body_resets_at") || stringIs(value, "header_retry_after")) return *stringOf(value);
      return rt::undefined;
    }

    double parseStoredGeneration(const json::value* value) {
      const std::optional<double> number = rt::getNumber(value);
      return number && rt::Number::isSafeInteger(*number) && *number >= 0 ? *number : 0;
    }

    std::optional<double> parseStoredPercent(const json::value* value) {
      const std::optional<double> number = rt::getNumber(value);
      if (number && rt::Number::isFinite(*number)) return number;
      return std::nullopt;
    }

    std::optional<std::string> parseAccountIdHash(const json::value* value) {
      std::optional<std::string> text = stringOf(value);
      if (text && rt::length(*text) > 0) return text;
      return std::nullopt;
    }

    std::vector<std::string> parseQuotaBlockedClasses(const json::value* value) {
      if (value == nullptr || !value->is_array()) return {};
      const json::array& entries = value->get_array();
      if (!std::all_of(entries.begin(), entries.end(), [](const json::value& entry) { return entry.is_string(); })) return {};
      rt::OrderedSet<std::string> unique;
      for (const json::value& entry : entries) unique.add(std::string(entry.get_string()));
      return std::vector<std::string>(unique.begin(), unique.end());
    }

    /** The slot-wide reset fields a per-class block may inherit when it shares the slot deadline. */
    struct CodexSlotClassBlockFields {
      std::optional<double> observedResetAtMs;
      bool observedResetAtIsStable = false;
      bool bankedResetGenerationAmbiguous = false;
      bool bankedResetRecoveryProbePending = false;
    };

    std::optional<CodexQuotaClassBlock> parseSlotClassBlock(const json::value* block, std::string_view quotaClassKey, bool unmarkedSyntheticLegacyUnknown,
                                                            const CodexSlotClassBlockFields& slotFields) {
      const json::value* blockedUntil = member(block, "blocked_until_ms");
      if (!isSafeMs(blockedUntil)) return std::nullopt;
      const json::value* source = member(block, "source");
      if (!stringIs(source, "body_resets_at") && !stringIs(source, "header_retry_after")) return std::nullopt;
      const double blockedUntilMs = *rt::getNumber(blockedUntil);
      // A class block that shares the slot-wide deadline inherits that deadline's
      // reset identity; an independent block keeps only the identity it stored.
      const bool sharesSlotDeadline = slotFields.observedResetAtMs && blockedUntilMs == *slotFields.observedResetAtMs;
      std::optional<double> observedResetAtMs;
      if (isSafeMs(member(block, "observed_reset_at_ms"))) {
        observedResetAtMs = rt::getNumber(member(block, "observed_reset_at_ms"));
      } else if (sharesSlotDeadline && slotFields.observedResetAtIsStable) {
        observedResetAtMs = slotFields.observedResetAtMs;
      }
      const json::value* signal = member(block, "quota_signal_observed_at_ms");
      return CodexQuotaClassBlock{
        blockedUntilMs,
        *stringOf(source),
        isTrue(member(block, "legacy_fallback")) || (quotaClassKey == "unknown" && unmarkedSyntheticLegacyUnknown),
        isSafeMs(signal) ? rt::getNumber(signal) : std::nullopt,
        observedResetAtMs,
        isTrue(member(block, "observed_reset_at_is_stable")) || (sharesSlotDeadline && slotFields.observedResetAtIsStable),
        isTrue(member(block, "banked_reset_generation_ambiguous")) || (sharesSlotDeadline && slotFields.bankedResetGenerationAmbiguous),
        isTrue(member(block, "banked_reset_recovery_probe_pending")) || (sharesSlotDeadline && slotFields.bankedResetRecoveryProbePending),
      };
    }

    CodexQuotaBlocksByClass parseQuotaBlocksByClass(const json::value* rawClassBlocks, bool unmarkedSyntheticLegacyUnknown, const CodexSlotClassBlockFields& slotFields) {
      CodexQuotaBlocksByClass quotaBlocksByClass;
      for (const char* quotaClassKey : {"spark", "gpt_oss_120b", "reserve", "standard", "unknown"}) {
        const json::value* block = member(rawClassBlocks, quotaClassKey);
        if (!isRecord(block)) continue;
        std::optional<CodexQuotaClassBlock> parsedBlock = parseSlotClassBlock(block, quotaClassKey, unmarkedSyntheticLegacyUnknown, slotFields);
        if (parsedBlock) quotaBlocksByClass.set(quotaClassKey, std::move(*parsedBlock));
      }
      return quotaBlocksByClass;
    }

    /**
     * The exact pre-account-hash slot shape: every field is absent or neutral, so
     * the parser's synthetic ambiguity may be treated as inherited rather than
     * observed. Any other record keeps the fail-closed default.
     */
    bool isExactLegacyNeutralSlot(const json::value* value, const rt::Nullable<CodexQuotaBlockSource>& source, const json::value* lease) {
      return member(value, "account_id_hash") == nullptr && member(value, "observed_reset_at_is_stable") == nullptr &&
             member(value, "banked_reset_generation_ambiguous") == nullptr && numberIs(member(value, "generation"), 0) &&
             isNull(member(value, "quota_blocked_until_ms")) && source.isNull() && isNull(member(value, "invalid_credential_version")) &&
             isNull(member(value, "primary_used_percent")) && isNull(member(value, "secondary_used_percent")) && isNull(member(value, "observed_reset_at_ms")) &&
             isNull(lease);
    }

    struct LegacyNeutralFirstBodyFenceFields {
      const std::optional<std::string>& accountIdHash;
      const std::optional<double>& quotaBlockedUntilMs;
      const std::optional<double>& observedResetAtMs;
      bool observedResetAtIsStable;
      const rt::Nullable<CodexQuotaBlockSource>& source;
      const json::value* lease;
    };

    /**
     * The first body-derived fence written after an exact legacy-neutral slot
     * inherited the old parser's synthetic ambiguity. Generation one proves there
     * was no prior quota transition; every real revision, recheck, credential
     * rotation, or recovery transition increments it again.
     */
    bool isLegacyNeutralFirstBodyFence(const json::value* value, const LegacyNeutralFirstBodyFenceFields& fields) {
      return isTrue(member(value, "banked_reset_generation_ambiguous")) && numberIs(member(value, "generation"), 1) && fields.accountIdHash.has_value() &&
             fields.source == std::string("body_resets_at") && fields.quotaBlockedUntilMs.has_value() && fields.observedResetAtMs == fields.quotaBlockedUntilMs &&
             fields.observedResetAtIsStable && isNull(fields.lease) && isNull(member(value, "invalid_credential_version"));
    }

    bool repairedBankedResetGenerationAmbiguous(const json::value* value, bool exactLegacyNeutralSlot, bool legacyNeutralFirstBodyFence) {
      return exactLegacyNeutralSlot || legacyNeutralFirstBodyFence ? false : !isFalse(member(value, "banked_reset_generation_ambiguous"));
    }

    std::optional<CodexRoutingSlot> parseSlot(const json::value* value, bool allowLegacyNeutralRepair) {
      if (!isRecord(value) || !stringOf(member(value, "credential_version"))) return std::nullopt;
      const std::string credentialVersion = *stringOf(member(value, "credential_version"));
      const rt::Nullable<CodexQuotaBlockSource> source = parseQuotaBlockSource(member(value, "quota_block_source"));
      if (source.isUndefined()) return std::nullopt;
      const json::value* lease = member(value, "probe_lease");
      const std::optional<CodexProbeCircuit> leaseCircuit = parseProbeLeaseCircuit(lease);
      std::optional<CodexProbeLease> parsedLease = parseProbeLease(lease, leaseCircuit);
      if (!isNull(lease) && !parsedLease) return std::nullopt;
      const std::optional<std::string> accountIdHash = parseAccountIdHash(member(value, "account_id_hash"));
      const std::optional<double> quotaBlockedUntilMs = parseOptionalSafeMs(member(value, "quota_blocked_until_ms"));
      const std::optional<std::string> invalidCredentialVersion = stringOf(member(value, "invalid_credential_version"));
      const std::optional<double> observedResetAtMs = parseOptionalSafeMs(member(value, "observed_reset_at_ms"));
      const std::optional<double> quotaSignalObservedAtMs = parseOptionalSafeMs(member(value, "quota_signal_observed_at_ms"));
      const std::optional<double> capacityObservedAtMs = parseOptionalSafeMs(member(value, "capacity_observed_at_ms"));
      const std::optional<double> upstreamTimeoutBlockedUntilMs = parseOptionalSafeMs(member(value, "upstream_timeout_blocked_until_ms"));
      const bool observedResetAtIsStable = isTrue(member(value, "observed_reset_at_is_stable"));
      const double generation = parseStoredGeneration(member(value, "generation"));
      const bool isExactLegacyNeutralSlotRepair = allowLegacyNeutralRepair && isExactLegacyNeutralSlot(value, source, lease);
      const bool isLegacyNeutralFirstBodyFenceRepair =
        allowLegacyNeutralRepair &&
        isLegacyNeutralFirstBodyFence(value, LegacyNeutralFirstBodyFenceFields{accountIdHash, quotaBlockedUntilMs, observedResetAtMs, observedResetAtIsStable, source, lease});
      const bool bankedResetGenerationAmbiguous = repairedBankedResetGenerationAmbiguous(value, isExactLegacyNeutralSlotRepair, isLegacyNeutralFirstBodyFenceRepair);
      const bool bankedResetRecoveryProbePending = isTrue(member(value, "banked_reset_recovery_probe_pending"));
      std::vector<std::string> quotaBlockedClasses = parseQuotaBlockedClasses(member(value, "quota_blocked_classes"));
      static const json::value emptyObject = json::object{};
      const json::value* rawClassBlocks = isRecord(member(value, "quota_blocks_by_class")) ? member(value, "quota_blocks_by_class") : &emptyObject;
      const bool unmarkedSyntheticLegacyUnknown = hasUnmarkedSyntheticLegacyUnknown(rawClassBlocks);
      CodexQuotaBlocksByClass quotaBlocksByClass = parseQuotaBlocksByClass(
        rawClassBlocks, unmarkedSyntheticLegacyUnknown, CodexSlotClassBlockFields{observedResetAtMs, observedResetAtIsStable, bankedResetGenerationAmbiguous, bankedResetRecoveryProbePending});
      CodexRoutingSlot slot;
      slot.account_id_hash = accountIdHash;
      slot.credential_version = credentialVersion;
      slot.quota_blocked_until_ms = quotaBlockedUntilMs;
      slot.quota_block_source = source.toOptional();
      slot.quota_blocked_classes = std::move(quotaBlockedClasses);
      slot.quota_blocks_by_class = std::move(quotaBlocksByClass);
      slot.invalid_credential_version = invalidCredentialVersion;
      slot.primary_used_percent = parseStoredPercent(member(value, "primary_used_percent"));
      slot.secondary_used_percent = parseStoredPercent(member(value, "secondary_used_percent"));
      slot.quota_signal_observed_at_ms = quotaSignalObservedAtMs;
      slot.capacity_observed_at_ms = capacityObservedAtMs;
      slot.upstream_timeout_blocked_until_ms = rt::orNull(upstreamTimeoutBlockedUntilMs);
      slot.observed_reset_at_ms = observedResetAtMs;
      // Older routing records did not carry this flag. Treat their header
      // deadline as unsuitable for an expensive reset rather than guessing its
      // identity from a relative timeout.
      slot.observed_reset_at_is_stable = observedResetAtIsStable;
      // A record written before this fence cannot prove that its deadline was
      // never revised. The two exact legacy-contamination repairs above are the
      // only exceptions; every other old or malformed record remains fail closed.
      slot.banked_reset_generation_ambiguous = bankedResetGenerationAmbiguous;
      slot.banked_reset_recovery_probe_pending = bankedResetRecoveryProbePending;
      slot.generation = generation;
      slot.probe_lease = std::move(parsedLease);
      return slot;
    }

    bool isOpaqueHash(const json::value* value) {
      static const rt::Regex OPAQUE_HASH("^[a-f0-9]{64}$");
      return value != nullptr && value->is_string() && OPAQUE_HASH.test(std::string_view(value->get_string()));
    }

    rt::Nullable<CodexActiveAccountTransitionReason> parseActiveTransitionReason(const json::value* value) {
      if (isNull(value)) return nullptr;
      for (const char* reason : {"quota_exhausted", "credential_invalid", "account_removed_or_replaced", "model_unavailable", "manual_reset"})
        if (stringIs(value, reason)) return CodexActiveAccountTransitionReason(reason);
      return rt::undefined;
    }

    bool safeIntegerAtLeast(const json::value* value, double minimum) {
      const std::optional<double> number = rt::getNumber(value);
      return number && rt::Number::isSafeInteger(*number) && *number >= minimum;
    }

    json::value numberOrNull(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }
    json::value stringOrNull(const std::optional<std::string>& value) { return value ? json::value(*value) : json::value(nullptr); }
  }

  bool isSafeMs(const json::value* value) {
    const std::optional<double> number = rt::getNumber(value);
    return number && isSafeMs(*number);
  }

  bool isSafeMs(const json::value& value) { return isSafeMs(&value); }

  bool isSafeMs(double value) { return rt::Number::isSafeInteger(value) && value >= 0; }

  bool isSafeMs(std::optional<double> value) { return value && isSafeMs(*value); }

  bool isLegacyStableResetIdentity(const CodexRoutingSlot& slot) {
    return !slot.account_id_hash && slot.observed_reset_at_ms.has_value() && slot.observed_reset_at_is_stable;
  }

  bool hasUnresolvedLegacyResetIdentity(const CodexAccountRoutingState* state) {
    if (state == nullptr) return false;
    return state->banked_reset_legacy_identity_unresolved || std::any_of(state->slots.begin(), state->slots.end(), isLegacyStableResetIdentity);
  }

  std::optional<double> parseOptionalSafeMs(const json::value* value) { return isSafeMs(value) ? rt::getNumber(value) : std::nullopt; }

  std::optional<double> parseOptionalSafeMs(std::optional<double> value) { return isSafeMs(value) ? value : std::nullopt; }

  std::optional<CodexAccountRoutingState> parseCodexAccountRoutingState(const json::value* value) {
    const json::value* slots = member(value, "slots");
    if (!isRecord(value) || !numberIs(member(value, "v"), 2) || !isSafeMs(member(value, "updated_at_ms")) || slots == nullptr || !slots->is_array()) {
      return std::nullopt;
    }
    const bool legacyIdentityUnresolved = isTrue(member(value, "banked_reset_legacy_identity_unresolved"));
    std::vector<std::optional<CodexRoutingSlot>> parsed;
    for (const json::value& slot : slots->get_array()) parsed.push_back(parseSlot(&slot, !legacyIdentityUnresolved));
    if (std::any_of(parsed.begin(), parsed.end(), [](const std::optional<CodexRoutingSlot>& slot) { return !slot; })) return std::nullopt;
    CodexAccountRoutingState state;
    state.v = 2;
    state.updated_at_ms = *rt::getNumber(member(value, "updated_at_ms"));
    // The field is deliberately opt-in here: normalization can still attach a
    // legacy stable identity when its credential version proves the account.
    // Exact reset fences independently reject any un-hashed stable slot.
    state.banked_reset_legacy_identity_unresolved = legacyIdentityUnresolved;
    for (std::optional<CodexRoutingSlot>& slot : parsed) state.slots.push_back(std::move(*slot));
    return state;
  }

  std::optional<CodexAccountRoutingState> parseCodexAccountRoutingState(const json::value& value) { return parseCodexAccountRoutingState(&value); }

  bool isVersionstamp(const json::value* value) {
    if (value == nullptr || !value->is_string()) return false;
    const std::int64_t length = rt::length(std::string_view(value->get_string()));
    return length > 0 && length <= 256;
  }

  std::optional<CodexActiveAccountSelection> parseCodexActiveAccountSelection(const json::value* value) {
    if (!isRecord(value) || !numberIs(member(value, "v"), 1)) return std::nullopt;
    const rt::Nullable<CodexActiveAccountTransitionReason> transitionReason = parseActiveTransitionReason(member(value, "transition_reason"));
    const json::value* slot = member(value, "slot");
    const json::value* routingGeneration = member(value, "routing_generation");
    const json::value* generation = member(value, "generation");
    if (!isOpaqueHash(member(value, "account_id_hash")) || !isOpaqueHash(member(value, "credential_version")) || !isVersionstamp(member(value, "pool_versionstamp")) ||
        !safeIntegerAtLeast(slot, 0) || !safeIntegerAtLeast(routingGeneration, 0) || !safeIntegerAtLeast(generation, 1) || transitionReason.isUndefined() ||
        !isSafeMs(member(value, "updated_at_ms")))
      return std::nullopt;
    return CodexActiveAccountSelection{
      1,
      *stringOf(member(value, "account_id_hash")),
      *stringOf(member(value, "credential_version")),
      *stringOf(member(value, "pool_versionstamp")),
      *rt::getNumber(slot),
      *rt::getNumber(routingGeneration),
      *rt::getNumber(generation),
      transitionReason.toOptional(),
      *rt::getNumber(member(value, "updated_at_ms")),
    };
  }

  std::optional<CodexActiveAccountSelection> parseCodexActiveAccountSelection(const json::value& value) { return parseCodexActiveAccountSelection(&value); }

  bool codexActiveAccountSelectionCurrent(const json::value* value, const CodexRoutingAccount& account, double activeGeneration) {
    const std::optional<CodexActiveAccountSelection> active = parseCodexActiveAccountSelection(value);
    return active.has_value() && active->generation == activeGeneration && active->account_id_hash == account.accountIdHash &&
           active->credential_version == account.credentialVersion && account.activePoolVersionstamp && active->pool_versionstamp == *account.activePoolVersionstamp &&
           active->slot == account.slot && account.routingGeneration && active->routing_generation == *account.routingGeneration;
  }

  bool codexActiveAccountSelectionCurrent(const json::value& value, const CodexRoutingAccount& account, double activeGeneration) {
    return codexActiveAccountSelectionCurrent(&value, account, activeGeneration);
  }

  json::object toJson(const CodexQuotaClassBlock& block) {
    json::object object;
    rt::objectDefine(object, "blocked_until_ms", rt::makeNumber(block.blocked_until_ms));
    rt::objectDefine(object, "source", json::value(block.source));
    rt::objectDefine(object, "legacy_fallback", block.legacy_fallback);
    rt::objectDefine(object, "quota_signal_observed_at_ms", numberOrNull(block.quota_signal_observed_at_ms));
    rt::objectDefine(object, "observed_reset_at_ms", numberOrNull(block.observed_reset_at_ms));
    rt::objectDefine(object, "observed_reset_at_is_stable", block.observed_reset_at_is_stable);
    rt::objectDefine(object, "banked_reset_generation_ambiguous", block.banked_reset_generation_ambiguous);
    rt::objectDefine(object, "banked_reset_recovery_probe_pending", block.banked_reset_recovery_probe_pending);
    return object;
  }

  json::object toJson(const CodexQuotaBlocksByClass& blocks) {
    json::object object;
    for (const auto& [key, block] : blocks) rt::objectDefine(object, key, toJson(block));
    return object;
  }

  json::object toJson(const CodexProbeLease& lease) {
    json::object object;
    rt::objectDefine(object, "token", json::value(lease.token));
    rt::objectDefine(object, "expires_at_ms", rt::makeNumber(lease.expires_at_ms));
    rt::objectDefine(object, "generation", rt::makeNumber(lease.generation));
    rt::objectDefine(object, "circuit", json::value(lease.circuit));
    rt::objectDefine(object, "quota_class", lease.quota_class);
    return object;
  }

  json::object toJson(const CodexRoutingSlot& slot) {
    json::object object;
    rt::objectDefine(object, "account_id_hash", stringOrNull(slot.account_id_hash));
    rt::objectDefine(object, "credential_version", json::value(slot.credential_version));
    rt::objectDefine(object, "quota_blocked_until_ms", numberOrNull(slot.quota_blocked_until_ms));
    rt::objectDefine(object, "quota_block_source", stringOrNull(slot.quota_block_source));
    if (slot.quota_blocked_classes) rt::objectDefine(object, "quota_blocked_classes", json::array(slot.quota_blocked_classes->begin(), slot.quota_blocked_classes->end()));
    if (slot.quota_blocks_by_class) rt::objectDefine(object, "quota_blocks_by_class", toJson(*slot.quota_blocks_by_class));
    rt::objectDefine(object, "invalid_credential_version", stringOrNull(slot.invalid_credential_version));
    rt::objectDefine(object, "primary_used_percent", numberOrNull(slot.primary_used_percent));
    rt::objectDefine(object, "secondary_used_percent", numberOrNull(slot.secondary_used_percent));
    rt::objectDefine(object, "quota_signal_observed_at_ms", numberOrNull(slot.quota_signal_observed_at_ms));
    rt::objectDefine(object, "capacity_observed_at_ms", numberOrNull(slot.capacity_observed_at_ms));
    rt::objectDefine(object, "upstream_timeout_blocked_until_ms", slot.upstream_timeout_blocked_until_ms);
    rt::objectDefine(object, "observed_reset_at_ms", numberOrNull(slot.observed_reset_at_ms));
    rt::objectDefine(object, "observed_reset_at_is_stable", slot.observed_reset_at_is_stable);
    rt::objectDefine(object, "banked_reset_generation_ambiguous", slot.banked_reset_generation_ambiguous);
    rt::objectDefine(object, "banked_reset_recovery_probe_pending", slot.banked_reset_recovery_probe_pending);
    rt::objectDefine(object, "generation", rt::makeNumber(slot.generation));
    rt::objectDefine(object, "probe_lease", slot.probe_lease ? json::value(toJson(*slot.probe_lease)) : json::value(nullptr));
    return object;
  }

  json::object toJson(const CodexAccountRoutingState& state) {
    json::array slots;
    for (const CodexRoutingSlot& slot : state.slots) slots.push_back(toJson(slot));
    json::object object;
    rt::objectDefine(object, "v", rt::makeNumber(state.v));
    rt::objectDefine(object, "updated_at_ms", rt::makeNumber(state.updated_at_ms));
    rt::objectDefine(object, "banked_reset_legacy_identity_unresolved", state.banked_reset_legacy_identity_unresolved);
    rt::objectDefine(object, "slots", std::move(slots));
    return object;
  }

  json::object toJson(const CodexActiveAccountSelection& selection) {
    json::object object;
    rt::objectDefine(object, "v", rt::makeNumber(selection.v));
    rt::objectDefine(object, "account_id_hash", json::value(selection.account_id_hash));
    rt::objectDefine(object, "credential_version", json::value(selection.credential_version));
    rt::objectDefine(object, "pool_versionstamp", json::value(selection.pool_versionstamp));
    rt::objectDefine(object, "slot", rt::makeNumber(selection.slot));
    rt::objectDefine(object, "routing_generation", rt::makeNumber(selection.routing_generation));
    rt::objectDefine(object, "generation", rt::makeNumber(selection.generation));
    rt::objectDefine(object, "transition_reason", stringOrNull(selection.transition_reason));
    rt::objectDefine(object, "updated_at_ms", rt::makeNumber(selection.updated_at_ms));
    return object;
  }

  json::object toJson(const CodexCapacityRoutingWindow& window) {
    json::object object;
    rt::objectDefine(object, "limit_window_seconds", numberOrNull(window.limit_window_seconds));
    rt::objectDefine(object, "used_percent", numberOrNull(window.used_percent));
    rt::objectDefine(object, "reset_at_ms", numberOrNull(window.reset_at_ms));
    return object;
  }

  json::object toJson(const CodexCapacityRoutingWindows& windows) {
    json::object object;
    rt::objectDefine(object, "primary", windows.primary ? json::value(toJson(*windows.primary)) : json::value(nullptr));
    rt::objectDefine(object, "secondary", windows.secondary ? json::value(toJson(*windows.secondary)) : json::value(nullptr));
    return object;
  }

  json::object toJson(const CodexCapacityRoutingAdditionalRateLimit& limit) {
    json::object object;
    rt::objectDefine(object, "limit_name", json::value(limit.limit_name));
    rt::objectDefine(object, "metered_feature", stringOrNull(limit.metered_feature));
    rt::objectDefine(object, "windows", toJson(limit.windows));
    return object;
  }

  json::object toJson(const CodexCapacityRoutingObservation& observation) {
    json::array limits;
    for (const CodexCapacityRoutingAdditionalRateLimit& limit : observation.additional_rate_limits) limits.push_back(toJson(limit));
    json::object object;
    rt::objectDefine(object, "slot", rt::makeNumber(observation.slot));
    rt::objectDefine(object, "account_id_hash", json::value(observation.account_id_hash));
    rt::objectDefine(object, "state", json::value(observation.state));
    rt::objectDefine(object, "source_observed_at_ms", numberOrNull(observation.source_observed_at_ms));
    rt::objectDefine(object, "snapshot_at_ms", rt::makeNumber(observation.snapshot_at_ms));
    rt::objectDefine(object, "windows", toJson(observation.windows));
    rt::objectDefine(object, "additional_rate_limits", std::move(limits));
    return object;
  }
}
