#include "uos/authority/native_codex_mutations.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace uos::authority::native_codex_mutations {
  using namespace native_codex_contracts;
  using namespace native_codex_capacity;

  namespace {
    // `array[index]` with a JS number index: an element only for a canonical array index.

    // `lease?.expires_at_ms ?? 0`
    double leaseExpiresAtMs(const CodexRoutingSlot& slot) { return slot.probe_lease ? slot.probe_lease->expires_at_ms : 0; }
  }

  std::optional<CodexAccountRoutingState> markCodexUpstreamTimeout(const CodexAccountRoutingState& state, const CodexRoutingAccount& account, double now,
                                                                   double updatedAtMs) {
    const double blockedUntil = now + CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS;

    const CodexRoutingSlot current = slotFor(state, account);
    if (!slotMatchesCodexRoutingAccount(current, account)) return std::nullopt;
    if (!account.probeGeneration && current.probe_lease) return std::nullopt;
    if (account.probeGeneration && !probeLeaseMatchesCodexRoutingAccount(current, account)) return std::nullopt;
    const double priorTimeout = current.upstream_timeout_blocked_until_ms.valueOr(0.0);
    CodexRoutingSlot next = current;
    next.account_id_hash = account.accountIdHash;
    next.upstream_timeout_blocked_until_ms = rt::Math::max(priorTimeout, blockedUntil);
    next.generation = current.generation + 1;
    next.probe_lease = std::nullopt;
    return withSlot(state, account.slot, next, updatedAtMs);
  }

  std::optional<CodexAccountRoutingState> markCodexCredentialInvalid(const CodexAccountRoutingState& state, const CodexRoutingAccount& account, double now,
                                                                     double updatedAtMs) {
    const CodexRoutingSlot current = slotFor(state, account);
    if (!slotMatchesCodexRoutingAccount(current, account)) return std::nullopt;
    if (account.probeGeneration && !probeLeaseMatchesCodexRoutingAccount(current, account)) return std::nullopt;
    CodexRoutingSlot next = current;
    next.account_id_hash = account.accountIdHash;
    next.quota_blocked_until_ms = std::nullopt;
    next.quota_block_source = std::nullopt;
    next.quota_blocked_classes = std::vector<std::string>{};
    next.quota_blocks_by_class = CodexQuotaBlocksByClass{};
    next.upstream_timeout_blocked_until_ms = nullptr;
    next.invalid_credential_version = account.credentialVersion;
    next.quota_signal_observed_at_ms = now;
    next.probe_lease = std::nullopt;
    return withSlot(state, account.slot, next, updatedAtMs);
  }

  std::optional<CodexAccountRoutingState> releaseCodexRoutingProbe(const CodexAccountRoutingState& state, const CodexRoutingAccount& account, double updatedAtMs) {
    const CodexRoutingSlot current = slotFor(state, account);
    if (!slotMatchesCodexRoutingAccount(current, account) || !probeLeaseMatchesCodexRoutingAccount(current, account)) return std::nullopt;
    CodexRoutingSlot next = current;
    if (routingProbeCircuit(account) == "quota") {
      const CodexQuotaClass requested = quotaClass(account.requestedModel);
      if (next.probe_lease && !current.banked_reset_recovery_probe_pending && requested != "unknown" &&
          next.probe_lease->quota_class.toOptional() == requested &&
          quotaBlockKeyForClass(withLegacyQuotaClassMap(current), requested) == "unknown") {
        // Resolve a formerly slot-wide lease before releasing it. The effective
        // unknown block is eligible for this alias only when marked as migrated.
        next.probe_lease->quota_class = CodexQuotaClass("unknown");
      }
      next = releaseQuotaClassProbe(next, requested, requestedModelView(account.requestedModel));
    }
    next.account_id_hash = account.accountIdHash;
    next.upstream_timeout_blocked_until_ms =
      routingProbeCircuit(account) == "upstream_timeout" ? rt::Nullable<double>(nullptr) : current.upstream_timeout_blocked_until_ms;
    next.probe_lease = std::nullopt;
    return withSlot(state, account.slot, next, updatedAtMs);
  }

  std::optional<CodexAccountRoutingState> markCodexSuccess(const CodexAccountRoutingState& state, const CodexRoutingAccount& account, double updatedAtMs) {
    const CodexRoutingSlot current = slotFor(state, account);
    if (!slotMatchesCodexRoutingAccount(current, account) || !probeLeaseMatchesCodexRoutingAccount(current, account)) return std::nullopt;
    const CodexRoutingSlot classAwareCurrent = withLegacyQuotaClassMap(current);
    const CodexRoutingSlot released = [&account, &current, &classAwareCurrent] {
      if (routingProbeCircuit(account) != "quota") return current;
      const CodexQuotaClass requestedQuotaClass = quotaClass(account.requestedModel);
      const auto claimed = current.probe_lease->quota_class.toOptional();
      const bool namedClaim = claimed && isCodexCapacityModelScope(*claimed);
      const auto releasedKey = namedClaim ? *claimed : quotaBlockKeyForClass(classAwareCurrent, requestedQuotaClass).value_or(requestedQuotaClass);
      return withoutQuotaClass(classAwareCurrent, releasedKey);
    }();
    const bool hasOtherQuotaClasses = released.quota_blocks_by_class && released.quota_blocks_by_class->size() > 0;
    CodexRoutingSlot next = released;
    next.account_id_hash = account.accountIdHash;
    next.upstream_timeout_blocked_until_ms =
      routingProbeCircuit(account) == "upstream_timeout" ? rt::Nullable<double>(nullptr) : current.upstream_timeout_blocked_until_ms;
    next.observed_reset_at_ms = hasOtherQuotaClasses ? released.observed_reset_at_ms : std::nullopt;
    next.observed_reset_at_is_stable = hasOtherQuotaClasses ? released.observed_reset_at_is_stable : false;
    next.banked_reset_generation_ambiguous = hasOtherQuotaClasses ? released.banked_reset_generation_ambiguous : false;
    next.banked_reset_recovery_probe_pending = hasOtherQuotaClasses ? released.banked_reset_recovery_probe_pending : false;
    next.probe_lease = std::nullopt;
    return withSlot(state, account.slot, next, updatedAtMs);
  }

  std::optional<CodexAccountRoutingState> reconcileRoutingCredential(const CodexAccountRoutingState& state, const CodexRoutingAccount& account,
                                                                     const CodexRoutingAccountIdentity& identity, bool sameAccount, double updatedAtMs) {
    const std::string& accountIdHash = identity.accountIdHash;
    const std::string& credentialVersion = identity.credentialVersion;

    const CodexRoutingSlot current = slotFor(state, account);
    if (current.credential_version == credentialVersion) return std::nullopt;
    if (!slotMatchesCodexRoutingAccount(current, account)) return std::nullopt;
    return withSlot(state, account.slot,
                    sameAccount ? rotateCredentialForSameAccount(current, CodexRoutingAccountIdentity{accountIdHash, credentialVersion})
                                : neutralSlot(credentialVersion, accountIdHash),
                    updatedAtMs);
  }

  std::optional<CodexAccountRoutingState> reconcileRoutingCredential(const CodexAccountRoutingState& state, const CodexRoutingAccount& account,
                                                                     const CodexRoutingIdentity& identity, bool sameAccount, double updatedAtMs) {
    return reconcileRoutingCredential(state, account, CodexRoutingAccountIdentity{identity.accountIdHash, identity.credentialVersion}, sameAccount, updatedAtMs);
  }

  bool isCodexQuotaBlockFenceCurrent(const json::value* value, const CodexRoutingAccount& account, double quotaResetAtMs, double routingGeneration) {
    if (!isSafeMs(quotaResetAtMs) || !rt::Number::isSafeInteger(routingGeneration) || routingGeneration < 0) return false;
    const std::optional<CodexAccountRoutingState> state = parseCodexAccountRoutingState(value);
    const CodexRoutingSlot* current = state ? rt::element(state->slots, account.slot) : nullptr;
    const std::optional<CodexQuotaClassBlock> classBlock = current != nullptr ? quotaBlockForClass(*current, quotaClass(account.requestedModel), requestedModelView(account.requestedModel)) : std::nullopt;
    return !hasUnresolvedLegacyResetIdentity(state ? &*state : nullptr) && current != nullptr && current->credential_version == account.credentialVersion &&
           current->account_id_hash == account.accountIdHash && current->generation == routingGeneration && classBlock.has_value() &&
           classBlock->observed_reset_at_ms == quotaResetAtMs && classBlock->observed_reset_at_is_stable && !classBlock->banked_reset_generation_ambiguous &&
           classBlock->blocked_until_ms == quotaResetAtMs;
  }

  bool isCodexQuotaBlockFenceCurrent(const json::value& value, const CodexRoutingAccount& account, double quotaResetAtMs, double routingGeneration) {
    return isCodexQuotaBlockFenceCurrent(&value, account, quotaResetAtMs, routingGeneration);
  }

  CodexAccountRoutingState recoveryProbeTransition(const CodexAccountRoutingState& state, const CodexRoutingAccount& account, const CodexProbeLease& recoveryLease,
                                                   double updatedAtMs) {
    const CodexRoutingSlot* found = rt::element(state.slots, account.slot);
    // `state.slots[account.slot]` is undefined, and withLegacyQuotaClassMap reads a member of it.
    if (found == nullptr) throw rt::TypeError("undefined is not an object (evaluating 'slot.quota_blocks_by_class')");
    const CodexRoutingSlot& current = *found;
    const CodexQuotaClass requestedQuotaClass = quotaClass(account.requestedModel);
    const double nextGeneration = recoveryLease.generation;
    const CodexRoutingSlot classAwareCurrent = withLegacyQuotaClassMap(current);
    const auto claimed = recoveryLease.quota_class.toOptional();
    // A named lease pins its exact window. A class lease can cover a migrated slot-wide block.
    const bool namedClaim = claimed && isCodexCapacityModelScope(*claimed);
    const auto releasedKey = namedClaim ? *claimed : quotaBlockKeyForClass(classAwareCurrent, requestedQuotaClass).value_or(requestedQuotaClass);
    const auto* releasedClassBlock = namedClaim && classAwareCurrent.quota_blocks_by_class
      ? classAwareCurrent.quota_blocks_by_class->get(releasedKey) : nullptr;
    const auto classBlock = namedClaim ? std::nullopt : quotaBlockForClass(classAwareCurrent, requestedQuotaClass);
    if (classBlock) releasedClassBlock = &*classBlock;
    const CodexRoutingSlot released = withoutQuotaClass(classAwareCurrent, releasedKey);
    CodexRoutingSlot recovered = released;
    // The verified reset makes normal routing eligible, but an absolute
    // Retry-After cannot prove whether a delayed response names this old
    // window or a new one. Keep the observation lookup-only until a
    // recovery probe independently proves the circuit healthy.
    recovered.observed_reset_at_ms =
      releasedClassBlock && releasedClassBlock->observed_reset_at_ms ? releasedClassBlock->observed_reset_at_ms : current.observed_reset_at_ms;
    recovered.observed_reset_at_is_stable = releasedClassBlock ? releasedClassBlock->observed_reset_at_is_stable : current.observed_reset_at_is_stable;
    recovered.banked_reset_generation_ambiguous = true;
    recovered.banked_reset_recovery_probe_pending = true;
    recovered.generation = nextGeneration;
    recovered.probe_lease = recoveryLease;
    CodexAccountRoutingState next = withSlot(state, account.slot, recovered, updatedAtMs);
    return next;
  }

  std::optional<CodexExpiredProbeClaim> buildExpiredProbeClaim(const CodexAccountRoutingState& base, const CodexRoutingAccount& account, double now,
                                                               const std::string& token, double updatedAtMs) {
    const CodexRoutingSlot current = slotFor(base, account);
    const CodexProbeCircuit circuit = routingProbeCircuit(account);
    const CodexQuotaClass requestedQuotaClass = quotaClass(account.requestedModel);
    const std::optional<CodexQuotaClassBlock> classBlock = quotaBlockForClass(current, requestedQuotaClass, requestedModelView(account.requestedModel));
    std::optional<double> circuitDeadline;
    if (circuit == "upstream_timeout") {
      circuitDeadline = current.upstream_timeout_blocked_until_ms.toOptional();
    } else if (classBlock) {
      circuitDeadline = classBlock->blocked_until_ms;
    }
    // A capacity-exhausted subscription with no class circuit is the probe
    // candidate that has no deadline to expire: its only trusted recovery is a
    // successful bounded probe, so it is claimed like an expired circuit.
    const bool capacityProbe = circuit == "quota" && !classBlock && account.quotaHeadroom == 0.0;
    // A blocked cohort that proved no redeemable credit may hold exactly one
    // scoped overage attempt even while its class-block deadline is still live.
    // The live-lease check below still serializes the attempt.
    const bool overageProbe = circuit == "quota" && account.overageFallback == true;
    if (!slotMatchesCodexRoutingAccount(current, account) || current.invalid_credential_version == account.credentialVersion ||
        (!capacityProbe && !overageProbe && (!rt::truthy(circuitDeadline) || *circuitDeadline > now)) || leaseExpiresAtMs(current) > now)
      return std::nullopt;
    CodexProbeLease lease;
    lease.token = token;
    lease.expires_at_ms = now + CODEX_HALF_OPEN_LEASE_MS;
    lease.generation = current.generation;
    lease.circuit = circuit;
    const auto scope = quotaBlockKeyForClass(current, requestedQuotaClass, requestedModelView(account.requestedModel));
    lease.quota_class = circuit == "quota"
      ? rt::Nullable<CodexQuotaClass>(scope && isCodexCapacityModelScope(*scope) ? *scope : requestedQuotaClass)
      : rt::Nullable<CodexQuotaClass>(nullptr);
    CodexRoutingSlot claimed = current;
    claimed.account_id_hash = account.accountIdHash;
    claimed.probe_lease = lease;
    return CodexExpiredProbeClaim{withSlot(base, account.slot, claimed, updatedAtMs), lease};
  }

  bool staleVerifiedResetProbeEligible(const CodexAccountRoutingState& state, const CodexRoutingSlot& current, const CodexRoutingAccount& account,
                                       const CodexStaleVerifiedResetProbeInput& input, double nowMs) {
    return !state.banked_reset_legacy_identity_unresolved && slotMatchesCodexRoutingAccount(current, account) &&
           current.account_id_hash == account.accountIdHash && current.credential_version == account.credentialVersion &&
           current.generation == input.routingGeneration && current.invalid_credential_version != account.credentialVersion &&
           [&current, &account, &input] {
             const CodexQuotaClass requestedClass = quotaClass(account.requestedModel);
             const std::optional<CodexQuotaClassBlock> classBlock = quotaBlockForClass(current, requestedClass, requestedModelView(account.requestedModel));
             const double legacyDeadline = requestedClass == "unknown" ? current.quota_blocked_until_ms.value_or(0.0) : 0.0;
             return classBlock.has_value() && rt::Math::max(classBlock->blocked_until_ms, legacyDeadline) >= input.quotaResetAtMs &&
                    classBlock->observed_reset_at_ms == input.quotaResetAtMs && classBlock->observed_reset_at_is_stable;
           }() &&
           leaseExpiresAtMs(current) <= nowMs;
  }

  bool isCodexActiveAccountSnapshotCurrent(const json::value* value, const CodexActiveAccountSnapshot& snapshot) {
    const std::optional<CodexActiveAccountSelection> current = parseCodexActiveAccountSelection(value);
    const std::optional<CodexActiveAccountSelection>& observed = snapshot.selection;
    if (!current || !observed) return !current && !observed;
    return current->account_id_hash == observed->account_id_hash && current->credential_version == observed->credential_version &&
           current->pool_versionstamp == observed->pool_versionstamp && current->slot == observed->slot &&
           current->routing_generation == observed->routing_generation && current->generation == observed->generation &&
           current->transition_reason == observed->transition_reason && current->updated_at_ms == observed->updated_at_ms;
  }

  bool isCodexActiveAccountSnapshotCurrent(const json::value& value, const CodexActiveAccountSnapshot& snapshot) {
    return isCodexActiveAccountSnapshotCurrent(&value, snapshot);
  }

  bool staleResetRoutingIdentityCurrent(const json::value* value, const CodexRoutingAccount& account, double quotaResetAtMs) {
    const std::optional<CodexAccountRoutingState> state = parseCodexAccountRoutingState(value);
    const CodexRoutingSlot* current = state ? rt::at(state->slots, account.slot) : nullptr;
    if (current == nullptr) return false;
    const std::optional<CodexQuotaClassBlock> classBlock = quotaBlockForClass(*current, quotaClass(account.requestedModel), requestedModelView(account.requestedModel));
    return current->account_id_hash == account.accountIdHash && current->credential_version == account.credentialVersion &&
           current->generation == account.routingGeneration && current->invalid_credential_version != account.credentialVersion && classBlock.has_value() &&
           classBlock->observed_reset_at_ms == quotaResetAtMs && classBlock->observed_reset_at_is_stable;
  }

  bool staleResetRoutingIdentityCurrent(const json::value& value, const CodexRoutingAccount& account, double quotaResetAtMs) {
    return staleResetRoutingIdentityCurrent(&value, account, quotaResetAtMs);
  }

  double nextResetElectionGeneration(const std::optional<CodexActiveAccountSelection>& active, const CodexRoutingAccount& candidate) {
    if (!active) return 1;
    if (active->account_id_hash == candidate.accountIdHash && active->slot == candidate.slot) return active->generation;
    return active->generation + 1;
  }

  bool resetCandidateSlotMatches(const CodexRoutingSlot& currentSlot, const CodexRoutingAccount& account, bool routingIdentityCurrent) {
    return currentSlot.account_id_hash == account.accountIdHash && currentSlot.credential_version == account.credentialVersion &&
           currentSlot.generation == account.routingGeneration && routingIdentityCurrent;
  }

  CodexRoutingSlot recheckSlotTransition(const CodexRoutingSlot& current, double recheckAtMs) {
    const CodexRoutingSlot rechecked = recheckQuotaClasses(current, recheckAtMs);
    CodexRoutingSlot next = rechecked;
    next.quota_signal_observed_at_ms = recheckAtMs;
    next.banked_reset_generation_ambiguous = rechecked.banked_reset_generation_ambiguous || rechecked.quota_blocked_until_ms.has_value() ||
                                             current.banked_reset_generation_ambiguous ||
                                             (current.observed_reset_at_ms.has_value() && current.observed_reset_at_is_stable);
    next.generation = current.generation + 1;
    next.probe_lease = std::nullopt;
    return next;
  }

  std::optional<CodexLocatedRoutingSlot> locateRoutingSlot(const CodexAccountRoutingState& state, double slotNumber) {
    if (slotNumber > static_cast<double>(state.slots.size())) return std::nullopt;
    const double index = slotNumber - 1;
    const CodexRoutingSlot* slot = rt::at(state.slots, index);
    if (slot == nullptr) return std::nullopt;
    return CodexLocatedRoutingSlot{index, *slot};
  }

  std::optional<double> admissionRefreshRoutingGeneration(const CodexAccountRoutingState* normalized, const CodexRoutingAccount& account) {
    const CodexRoutingSlot* currentSlot = normalized != nullptr ? rt::at(normalized->slots, account.slot) : nullptr;
    if (normalized != nullptr && currentSlot == nullptr) return std::nullopt;
    if (currentSlot != nullptr &&
        (currentSlot->account_id_hash != account.accountIdHash || currentSlot->credential_version != account.credentialVersion ||
         currentSlot->invalid_credential_version == account.credentialVersion))
      return std::nullopt;
    return currentSlot != nullptr ? currentSlot->generation : 0.0;
  }

  CodexAdmissionRefreshPlan prepareCodexAdmissionRefresh(const CodexActiveAccountSelection& active, const CodexRoutingAccount& account,
                                                         const std::string& poolVersionstamp, double routingGeneration) {
    if (active.credential_version == account.credentialVersion && active.pool_versionstamp == poolVersionstamp && active.routing_generation == routingGeneration)
      return CodexAdmissionRefreshPlanCurrent{};
    CodexActiveAccountSelection next = active;
    next.credential_version = account.credentialVersion;
    next.pool_versionstamp = poolVersionstamp;
    next.routing_generation = routingGeneration;
    return CodexAdmissionRefreshPlanRefresh{std::move(next)};
  }
}
