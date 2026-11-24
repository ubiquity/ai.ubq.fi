#include "uos/authority/native_codex_evaluation.hpp"

#include "uos/authority/native_codex_capacity.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"

#include <string_view>
#include <utility>

namespace uos::authority::native_codex_evaluation {
  using namespace native_codex_contracts;
  using namespace native_codex_capacity;

  namespace {
    std::optional<std::string_view> modelView(const std::optional<std::string>& model) {
      return model ? std::optional<std::string_view>(*model) : std::nullopt;
    }

    // `{ ...account, quotaResetAtMs }`
    CodexBlockedAccountMetadata blockedAccountFor(const CodexRoutedAccount& account, double quotaResetAtMs) {
      CodexBlockedAccountMetadata blocked;
      static_cast<CodexRoutingAccount&>(blocked) = account;
      blocked.quotaResetAtMs = quotaResetAtMs;
      return blocked;
    }

    CodexRoutingAccountEvaluation skippedRoutingAccount(const CodexRoutedAccount& account, double slotNumber, std::optional<CodexProbeCircuit> blockedCircuit,
                                                        std::optional<double> retryAtMs = std::nullopt,
                                                        std::optional<CodexBlockedAccountMetadata> blockedAccount = std::nullopt,
                                                        std::optional<CodexActiveAccountTransitionReason> activeTransitionReason = std::nullopt) {
      return {
        .account = account,
        .routedAccount = std::nullopt,
        .skippedSlot = slotNumber,
        .blockedCircuit = std::move(blockedCircuit),
        .retryAtMs = retryAtMs,
        .blockedAccount = std::move(blockedAccount),
        .activeTransitionReason = std::move(activeTransitionReason),
        .capacityExhausted = false,
        .modelUnavailable = std::nullopt,
      };
    }

    CodexRoutingAccountEvaluation routedRoutingAccount(const CodexRoutedAccount& routedAccount) {
      return {
        .account = routedAccount,
        .routedAccount = routedAccount,
        .skippedSlot = std::nullopt,
        .blockedCircuit = std::nullopt,
        .retryAtMs = std::nullopt,
        .blockedAccount = std::nullopt,
        .activeTransitionReason = std::nullopt,
        .capacityExhausted = false,
        .modelUnavailable = std::nullopt,
      };
    }

    /** A stale capacity record is not an observation; only a fresh one may override a circuit. */
    // `observation: CodexCapacityRoutingObservation | undefined`: nullptr is undefined.
    const CodexCapacityRoutingObservation* freshCapacityObservation(const CodexCapacityRoutingObservation* observation, double now) {
      return observation != nullptr && capacityObservationIsFresh(*observation, now) ? observation : nullptr;
    }

    struct CapacityDecision {
      std::optional<double> quotaHeadroom;
      bool capacityOverride;
      bool capacityExhausted;
    };

    /**
     * The last in-memory guard after the durable reconciliation CAS. It prevents a
     * stale local circuit from suppressing a fresh, positive account observation
     * that lost a concurrent write race.
     */
    CapacityDecision resolveCodexCapacityDecision(const CodexRoutingSlot& slot, const CodexRoutingSlot& classAwareSlot, const CodexQuotaClass& requestedQuotaClass,
                                                  const CodexCapacityRoutingObservation* observation, const std::optional<std::string>& model, double now) {
      const CodexCapacityRoutingObservation* freshObservation = freshCapacityObservation(observation, now);
      const std::optional<double> observedCapacityHeadroom =
        freshObservation == nullptr ? std::nullopt : capacityHeadroomForObservation(*freshObservation, modelView(model));
      const std::optional<double> classQuotaSignalObservedAtMs = quotaSignalObservedAtForClass(classAwareSlot, requestedQuotaClass, modelView(model));
      const std::optional<double> snapshotAtMs = freshObservation != nullptr ? std::optional<double>(freshObservation->snapshot_at_ms) : std::nullopt;
      const bool quotaSignalNewer = snapshotAtMs.has_value() && classQuotaSignalObservedAtMs.has_value() && *classQuotaSignalObservedAtMs >= *snapshotAtMs;
      const bool capacityOverride = freshObservation != nullptr && observedCapacityHeadroom.has_value() && *observedCapacityHeadroom > 0 && !quotaSignalNewer;
      const bool capacityExhausted = freshObservation != nullptr && observedCapacityHeadroom.has_value() && *observedCapacityHeadroom == 0 && !quotaSignalNewer;
      return {
        .quotaHeadroom = freshObservation == nullptr ? quotaHeadroomFor(slot) : observedCapacityHeadroom,
        .capacityOverride = capacityOverride,
        .capacityExhausted = capacityExhausted,
      };
    }

    struct QuotaSkip {
      double retryAtMs;
      std::optional<CodexBlockedAccountMetadata> blockedAccount;
    };

    /** A running class circuit skips the account and keeps its reset identity available to banked redemption. */
    std::optional<QuotaSkip> quotaBlockedSkipFor(const std::optional<CodexQuotaClassBlock>& requestedClassBlock, const CodexRoutedAccount& routedAccount, double now,
                                                 bool capacityOverride) {
      if (!requestedClassBlock || requestedClassBlock->blocked_until_ms <= now || capacityOverride) return std::nullopt;
      std::optional<CodexBlockedAccountMetadata> blockedAccount;
      if (requestedClassBlock->observed_reset_at_ms.has_value() && requestedClassBlock->observed_reset_at_is_stable)
        blockedAccount = blockedAccountFor(routedAccount, *requestedClassBlock->observed_reset_at_ms);
      return QuotaSkip{.retryAtMs = requestedClassBlock->blocked_until_ms, .blockedAccount = std::move(blockedAccount)};
    }

    struct LeaseSkip {
      CodexProbeCircuit circuit;
      double retryAtMs;
    };

    /**
     * A verified banked reset releases the quota deadline but retains its
     * recovery-probe lease. Ordinary routing stays unavailable until that exact
     * fenced probe succeeds, fails, or expires.
     */
    std::optional<LeaseSkip> probeLeaseSkipFor(const CodexRoutingSlot& slot, std::optional<std::string_view> model, double now) {
      const std::optional<CodexProbeLease>& lease = slot.probe_lease;
      if (!lease || lease->expires_at_ms <= now) return std::nullopt;
      const bool leaseBlocksRequest =
        lease->circuit != "upstream_timeout" && (lease->quota_class.isNullish() || codexQuotaScopeApplies(*lease->quota_class, model));
      if (!leaseBlocksRequest) return std::nullopt;
      return LeaseSkip{.circuit = lease->circuit, .retryAtMs = lease->expires_at_ms};
    }

    // The TS compares the slot objects by identity; see the header.
    bool sameSlot(const CodexRoutingSlot& left, const CodexRoutingSlot& right) {
      return rt::deepEqual(json::value(toJson(left)), json::value(toJson(right)));
    }
  }

  CodexRoutingAccountEvaluation evaluateCodexRoutingAccount(const CodexAccountRoutingState& state, const CodexRoutingSlotIdentity& mapped, const std::optional<std::string>& model,
                                                            const rt::OrderedMap<std::string, CodexCapacityRoutingObservation>& observationsByAccount, double now,
                                                            const CodexRoutingAccountPolicy& policy) {
    CodexRoutingAccount account;
    account.slot = mapped.slot;
    account.accountIdHash = mapped.accountIdHash;
    account.credentialVersion = mapped.credentialVersion;
    account.quotaHeadroom = std::nullopt;
    account.probeRequired = false;
    account.probeGeneration = std::nullopt;
    account.probeToken = std::nullopt;
    account.probeCircuit = nullptr;
    account.requestedModel = model ? rt::Nullable<std::string>(*model) : rt::Nullable<std::string>(nullptr);
    const CodexRoutingSlot storedSlot = slotFor(state, account);
    const CodexRoutingSlot slot = slotMatchesCodexRoutingAccount(storedSlot, account) ? storedSlot : neutralSlot(account.credentialVersion, account.accountIdHash);
    const CodexQuotaClass requestedQuotaClass = quotaClass(modelView(model));
    const CodexRoutingSlot classAwareSlot = withLegacyQuotaClassMap(slot);
    const std::optional<CodexQuotaClassBlock> requestedClassBlock = quotaBlockForClass(classAwareSlot, requestedQuotaClass, modelView(model));
    const CodexCapacityRoutingObservation* observation = observationsByAccount.get(account.accountIdHash);
    const CapacityDecision capacity = resolveCodexCapacityDecision(slot, classAwareSlot, requestedQuotaClass, observation, model, now);
    CodexRoutedAccount routedAccount = account;
    routedAccount.quotaHeadroom = capacity.quotaHeadroom;
    routedAccount.routingGeneration = slot.generation;
    routedAccount.hasFreshIncludedCapacity = capacity.capacityOverride;
    if (slot.invalid_credential_version == account.credentialVersion) {
      return skippedRoutingAccount(routedAccount, mapped.slot + 1, std::nullopt, std::nullopt, std::nullopt, "credential_invalid");
    }
    // A model this account provably cannot serve is not a quota or credential
    // observation: skip it without touching any fence and let an entitled sibling
    // take the request. Unknown availability never skips.
    if (model.has_value() && policy.modelUnavailable.has_value()) {
      CodexRoutingAccountEvaluation skipped = skippedRoutingAccount(routedAccount, mapped.slot + 1, std::nullopt, std::nullopt, std::nullopt, "model_unavailable");
      skipped.modelUnavailable = CodexRoutingAccountEvaluation::ModelUnavailable{.model = *model, .detail = policy.modelUnavailable->detail};
      return skipped;
    }
    const std::optional<QuotaSkip> quotaSkip = quotaBlockedSkipFor(requestedClassBlock, routedAccount, now, capacity.capacityOverride);
    if (quotaSkip) {
      return skippedRoutingAccount(routedAccount, mapped.slot + 1, "quota", quotaSkip->retryAtMs, quotaSkip->blockedAccount, "quota_exhausted");
    }
    const std::optional<LeaseSkip> leaseSkip = probeLeaseSkipFor(slot, modelView(model), now);
    if (leaseSkip) return skippedRoutingAccount(routedAccount, mapped.slot + 1, leaseSkip->circuit, leaseSkip->retryAtMs, std::nullopt);
    if (capacity.capacityExhausted) {
      // A fully used capacity observation with a future reset deadline is the
      // authoritative quota block for that window: it must feed the blocked
      // cohort so the request can consider a reset before attempting overage.
      // Without a deadline, a bounded probe can re-observe the account.
      const auto observationResetAtMs = capacityExhaustedResetAtMs(*observation, modelView(model), now);
      if (observationResetAtMs) {
        CodexRoutingAccountEvaluation skipped = skippedRoutingAccount(routedAccount, mapped.slot + 1, "quota", *observationResetAtMs,
                                                                      blockedAccountFor(routedAccount, *observationResetAtMs), "quota_exhausted");
        skipped.capacityExhausted = true;
        return skipped;
      }
      CodexRoutedAccount probing = routedAccount;
      probing.probeRequired = true;
      probing.probeCircuit = CodexProbeCircuit("quota");
      probing.quotaUsageProbeRequired = capacityHasCurrentExhaustedWindow(*observation, modelView(model), now);
      CodexRoutingAccountEvaluation routed = routedRoutingAccount(probing);
      routed.capacityExhausted = true;
      return routed;
    }
    // Claim the half-open lease only if request execution actually reaches this
    // slot. This preserves first/second order without abandoning a secondary
    // lease when the healthy first account returns directly.
    if (requestedClassBlock) {
      CodexRoutedAccount halfOpen = routedAccount;
      halfOpen.probeRequired = !capacity.capacityOverride;
      halfOpen.probeCircuit = capacity.capacityOverride ? rt::Nullable<CodexProbeCircuit>(nullptr) : rt::Nullable<CodexProbeCircuit>(CodexProbeCircuit("quota"));
      return routedRoutingAccount(halfOpen);
    }
    return routedRoutingAccount(routedAccount);
  }

  void foldCodexRoutingEvaluation(CodexRoutingAccumulator& accumulator, const CodexRoutingAccountEvaluation& evaluated) {
    if (evaluated.routedAccount) {
      accumulator.available.push_back(*evaluated.routedAccount);
      return;
    }
    if (evaluated.skippedSlot) accumulator.skipped.push_back(*evaluated.skippedSlot);
    if (evaluated.blockedCircuit == "upstream_timeout") accumulator.hasUpstreamTimeoutBlock = true;
    if (evaluated.blockedCircuit == "quota") accumulator.hasQuotaBlock = true;
    if (evaluated.modelUnavailable && !accumulator.modelUnavailable) accumulator.modelUnavailable = evaluated.modelUnavailable;
    if (evaluated.retryAtMs) {
      accumulator.retryAt = !accumulator.retryAt ? *evaluated.retryAtMs : rt::Math::min(*accumulator.retryAt, *evaluated.retryAtMs);
    }
    if (evaluated.blockedAccount) accumulator.blockedAccounts.push_back(*evaluated.blockedAccount);
  }

  CodexRouteSelection classifyCodexRouteSelection(const CodexRoutingAccumulation& accumulated, bool fullCohortExhausted, const CodexActiveAccountSnapshot& activeSnapshot,
                                                  const std::optional<std::string>& poolSnapshotJson, const std::optional<std::string>& capacitySnapshotJson,
                                                  bool fullCohortUnavailable) {
    if (!accumulated.available.empty()) {
      CodexRouteSelectionEligible eligible;
      eligible.accounts = accumulated.available;
      eligible.skippedSlots = accumulated.skipped;
      eligible.blockedAccounts = accumulated.blockedAccounts;
      return eligible;
    }
    if (accumulated.hasUpstreamTimeoutBlock) {
      CodexRouteSelectionUpstreamBlocked blocked;
      blocked.skippedSlots = accumulated.skipped;
      blocked.retryAtMs = accumulated.retryAt;
      return blocked;
    }
    if (accumulated.hasQuotaBlock) {
      CodexRouteSelectionQuotaBlocked blocked;
      blocked.skippedSlots = accumulated.skipped;
      blocked.retryAtMs = accumulated.retryAt;
      blocked.blockedAccounts = accumulated.blockedAccounts;
      blocked.fullCohortExhausted = fullCohortExhausted;
      if (fullCohortUnavailable) blocked.fullCohortUnavailable = fullCohortUnavailable;
      blocked.activeSnapshot = activeSnapshot;
      blocked.poolSnapshotJson = poolSnapshotJson;
      blocked.capacitySnapshotJson = capacitySnapshotJson;
      return blocked;
    }
    // No account is even entitled to serve the named model: answer the model
    // itself rather than a retryable quota or credential failure.
    if (accumulated.modelUnavailable) {
      CodexRouteSelectionModelUnavailable unavailable;
      unavailable.model = accumulated.modelUnavailable->model;
      unavailable.detail = accumulated.modelUnavailable->detail;
      unavailable.skippedSlots = accumulated.skipped;
      return unavailable;
    }
    CodexRouteSelectionCredentialsInvalid invalid;
    invalid.skippedSlots = accumulated.skipped;
    if (fullCohortUnavailable) invalid.fullCohortUnavailable = fullCohortUnavailable;
    return invalid;
  }

  bool routingStateChangedByNormalization(const CodexAccountRoutingState* durable, const CodexAccountRoutingState& normalized) {
    if (durable == nullptr) return true;
    if (durable->banked_reset_legacy_identity_unresolved != normalized.banked_reset_legacy_identity_unresolved || durable->slots.size() != normalized.slots.size()) return true;
    for (std::size_t index = 0; index < durable->slots.size(); ++index)
      if (!sameSlot(durable->slots[index], normalized.slots[index])) return true;
    return false;
  }
}
