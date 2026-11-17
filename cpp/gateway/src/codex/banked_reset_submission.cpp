// Port of src/codex/banked-reset-submission.ts.
// Codex banked reset submission, redemption and reconciliation, split out of src/codex_banked_reset.ts.
#include "uos/codex/banked_reset_submission.hpp"

#include "uos/codex/banked_reset_claim.hpp"
#include "uos/codex/banked_reset_provider.hpp"
#include "uos/codex/reset_settings.hpp"
#include "uos/kv.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/utils.hpp"

#include <utility>
#include <variant>

namespace uos::codex::banked_reset_submission {
  namespace {
    using ::uos::types::CodexResetRedemptionRecord;
    using ::uos::types::CodexResetRedemptionState;
    using banked_reset::CodexBankedResetCandidate;
    using banked_reset::CodexBankedResetConfig;
    using banked_reset::CodexBankedResetDependencies;
    using banked_reset::CodexBankedResetOutcome;
    using banked_reset::CodexBankedResetTelemetry;
    using banked_reset::CodexBankedResetTelemetryFields;
    using banked_reset::emit;
    using banked_reset::metric;
    using banked_reset::outcome;
    using banked_reset::ResetContext;
    using banked_reset::telemetryFields;
    using banked_reset_claim::readClock;
    using banked_reset_provider::CodexUsageResetProvider;
    using banked_reset_provider::RedeemResetResult;
    using banked_reset_provider::ResetInventory;
    using banked_reset_provider::ResetInventoryCredit;
    using RecordUpdate = std::optional<CodexResetRedemptionRecord>;

    // `candidate.signal ?? new AbortController().signal`
    rt::AbortSignal signalOrFresh(const CodexBankedResetCandidate& candidate) {
      if (candidate.signal) return *candidate.signal;
      const rt::AbortController controller;
      return controller.signal();
    }

    // `candidate.signal?.aborted`
    bool callerAborted(const CodexBankedResetCandidate& candidate) { return candidate.signal && candidate.signal->aborted(); }

    // `candidate.authorization?.kind === "manual_admin"`
    bool isManual(const CodexBankedResetCandidate& candidate) { return candidate.authorization.has_value(); }

    // `${"new_submission_"}${reason}`
    std::string newSubmissionReason(const std::optional<std::string>& reason) { return "new_submission_" + reason.value_or("null"); }

    // `Readonly<{ kind: "ready"; nowMs: number }> | Readonly<{ kind: "outcome"; outcome: CodexBankedResetOutcome }>` and the
    // other `{ kind: "outcome" }` unions of this module.
    struct StepReady {
      [[maybe_unused]] static constexpr std::string_view kind = "ready";
      double nowMs;
    };
    struct StepOutcome {
      [[maybe_unused]] static constexpr std::string_view kind = "outcome";
      CodexBankedResetOutcome outcome;
    };
    struct StepSelected {
      [[maybe_unused]] static constexpr std::string_view kind = "selected";
      ResetInventoryCredit credit;
      double nowMs;
    };
    struct StepPrepared {
      [[maybe_unused]] static constexpr std::string_view kind = "prepared";
      CodexResetRedemptionRecord record;
    };
    struct StepAllow {
      [[maybe_unused]] static constexpr std::string_view kind = "allow";
      bool allowNewSubmission;
    };
    using ReadyStep = std::variant<StepReady, StepOutcome>;
    using SelectedStep = std::variant<StepSelected, StepOutcome>;
    using PreparedStep = std::variant<StepPrepared, StepOutcome>;
    using AllowanceStep = std::variant<StepAllow, StepOutcome>;

    rt::Task<CodexBankedResetOutcome> verifyOwned(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexResetRedemptionRecord record,
                                                  CodexBankedResetCandidate candidate, std::shared_ptr<CodexUsageResetProvider> provider,
                                                  std::function<double()> clock, CodexBankedResetTelemetry telemetry) {
      if (callerAborted(candidate)) co_return outcome("pending", "client_aborted_before_verification", context, record);
      const double startedAt = rt::performanceNow();
      json::value result = false;
      bool unavailable = false;
      try {
        rt::Promise<json::value> verifying(provider->verifyApplied(context.account, signalOrFresh(candidate)));
        result = co_await verifying.wait();
      } catch (...) {
        unavailable = true;
      }
      if (unavailable) {
        const std::optional<double> nowMs = readClock(clock);
        if (!nowMs) co_return outcome("pending", "invalid_clock", context, record);
        rt::Promise<RecordUpdate> marking(banked_reset_claim::unknownOwned(kv, context, record, *nowMs, "verification_unavailable", record.provider_receipt_id));
        const RecordUpdate unknown = co_await marking.wait();
        co_return banked_reset_claim::unknownOutcome(telemetry, context, candidate, "verification_unavailable", unknown ? *unknown : record);
      }
      const std::optional<double> nowMs = readClock(clock);
      if (!nowMs) co_return outcome("pending", "invalid_clock", context, record);
      if (!result.is_bool() || !result.get_bool()) {
        const std::string reason = result.is_bool() ? "verification_not_applied" : "verification_response_invalid";
        rt::Promise<RecordUpdate> marking(banked_reset_claim::unknownOwned(kv, context, record, *nowMs, reason, record.provider_receipt_id));
        const RecordUpdate unknown = co_await marking.wait();
        co_return banked_reset_claim::unknownOutcome(telemetry, context, candidate, reason, unknown ? *unknown : record);
      }
      const double verifiedAtMs = *nowMs;
      std::function<CodexResetRedemptionRecord(const CodexResetRedemptionRecord&)> finalize = [verifiedAtMs](const CodexResetRedemptionRecord& current) {
        banked_reset_claim::StateWithPatch patch;
        patch.verified_at_ms = verifiedAtMs;
        patch.last_error_code = nullptr;
        return banked_reset_claim::stateWith(current, CodexResetRedemptionState::verified, verifiedAtMs, patch);
      };
      rt::Promise<RecordUpdate> finalizing(banked_reset_claim::updateOwnedRecord(kv, context, record, finalize));
      const RecordUpdate finalized = co_await finalizing.wait();
      if (!finalized) co_return outcome("pending", "verification_cas_failed", context, record);
      emit(telemetry, "codex_reset_verified", telemetryFields(context, candidate, json::object{{"state", "verified"}}));
      metric(telemetry, "codex_reset_verified_total", 1, telemetryFields(context, candidate));
      metric(telemetry, "codex_reset_verification_latency_ms", rt::Math::max(0.0, rt::Math::round(rt::performanceNow() - startedAt)),
             telemetryFields(context, candidate));
      metric(telemetry, "codex_reset_estimated_spend_total", 1, telemetryFields(context, candidate));
      co_return outcome("verified", "verified", context, finalized);
    }

    /**
     * The upstream adapter has already parsed a documented terminal redemption
     * result (`reset` or `already_redeemed`). Lost, malformed, non-2xx, and unknown
     * responses never reach this path and remain durable `unknown`.
     */
    // `redeemOutcome: "reset" | "already_redeemed"`
    rt::Task<CodexBankedResetOutcome> finalizeDocumentedRedeemOutcome(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexResetRedemptionRecord record,
                                                                      CodexBankedResetCandidate candidate, double nowMs, CodexBankedResetTelemetry telemetry,
                                                                      std::string redeemOutcome) {
      std::function<CodexResetRedemptionRecord(const CodexResetRedemptionRecord&)> finalize = [nowMs](const CodexResetRedemptionRecord& current) {
        banked_reset_claim::StateWithPatch patch;
        patch.verified_at_ms = nowMs;
        patch.last_error_code = nullptr;
        return banked_reset_claim::stateWith(current, CodexResetRedemptionState::verified, nowMs, patch);
      };
      rt::Promise<RecordUpdate> finalizing(banked_reset_claim::updateOwnedRecord(kv, context, record, finalize));
      const RecordUpdate finalized = co_await finalizing.wait();
      if (!finalized) co_return outcome("pending", "redeem_outcome_finalization_cas_failed", context, record);
      emit(telemetry, "codex_reset_verified",
           telemetryFields(context, candidate,
                           json::object{
                             {"state", "verified"},
                             {"verification_source", "redeem_outcome"},
                             {"redeem_outcome", redeemOutcome},
                           }));
      metric(telemetry, "codex_reset_verified_total", 1, telemetryFields(context, candidate));
      metric(telemetry, "codex_reset_verification_latency_ms", 0, telemetryFields(context, candidate));
      metric(telemetry, "codex_reset_estimated_spend_total", 1, telemetryFields(context, candidate));
      co_return outcome("verified", "redeem_outcome_" + redeemOutcome, context, finalized);
    }

    // `!value?.[key]` on a member of an injected contract: absent unless it is an own member of an object.
    bool contractFlagIsFalsy(const json::value* container, std::string_view key) {
      if (container == nullptr || !container->is_object()) return true;
      return !rt::truthy(rt::get(*container, key));
    }

    /**
     * A terminal redeem outcome can only be reconciled when the contract proves a
     * lookup or an independent verification. The provider check runs first so a
     * provider that does not claim a final outcome is never inspected further.
     */
    bool terminalOutcomeCannotReconcile(const CodexUsageResetProvider& provider) {
      if (!banked_reset_provider::providerTreatsRedeemOutcomeAsFinal(&provider)) return false;
      // `UnverifiedResetContract`: the capability fields an injected contract may be missing.
      const json::value& contract = provider.contract;
      const json::value* lookup = rt::get(contract, "lookup");
      const json::value* verification = rt::get(contract, "verification");
      return contractFlagIsFalsy(lookup, "byIdempotencyKey") && contractFlagIsFalsy(lookup, "byProviderReceiptId") &&
             contractFlagIsFalsy(verification, "independentlyVerifiable");
    }

    // The receipt of a terminal or accepted result (`lookedUp.providerReceiptId`).
    std::optional<std::string> providerReceiptIdOf(const RedeemResetResult& result) {
      if (const auto* completed = std::get_if<banked_reset_provider::RedeemResetResultCompleted>(&result)) return completed->providerReceiptId;
      if (const auto* accepted = std::get_if<banked_reset_provider::RedeemResetResultAccepted>(&result)) return accepted->providerReceiptId;
      if (const auto* alreadyRedeemed = std::get_if<banked_reset_provider::RedeemResetResultAlreadyRedeemed>(&result)) return alreadyRedeemed->providerReceiptId;
      if (const auto* unknown = std::get_if<banked_reset_provider::RedeemResetResultUnknown>(&result)) return unknown->providerReceiptId;
      return std::nullopt;
    }

    rt::Task<CodexBankedResetOutcome> reconcileOwned(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexResetRedemptionRecord record,
                                                     CodexBankedResetCandidate candidate, std::shared_ptr<CodexUsageResetProvider> provider,
                                                     std::function<double()> clock, CodexBankedResetTelemetry telemetry) {
      if (callerAborted(candidate)) co_return outcome("pending", "client_aborted_before_reconciliation", context, record);
      bool cannotReconcile = false;
      bool contractUnreadable = false;
      try {
        cannotReconcile = terminalOutcomeCannotReconcile(*provider);
      } catch (...) {
        contractUnreadable = true;
      }
      if (contractUnreadable) co_return outcome("pending", "provider_contract_unproven", context, record);
      if (cannotReconcile) {
        co_return outcome("pending", "terminal_outcome_ambiguous", context, record);
      }
      std::optional<RedeemResetResult> lookedUp;
      bool unavailable = false;
      try {
        banked_reset_provider::LookupRedeemResetInput input;
        static_cast<banked_reset_provider::ResetAccountContext&>(input) = context.account;
        input.idempotencyKey = context.idempotencyKey;
        input.providerReceiptId = record.provider_receipt_id;
        rt::Promise<RedeemResetResult> looking(provider->lookup(std::move(input), signalOrFresh(candidate)));
        lookedUp = co_await looking.wait();
      } catch (...) {
        unavailable = true;
      }
      if (unavailable) {
        const std::optional<double> nowMs = readClock(clock);
        if (!nowMs) co_return outcome("pending", "invalid_clock", context, record);
        rt::Promise<RecordUpdate> marking(banked_reset_claim::unknownOwned(kv, context, record, *nowMs, "lookup_unavailable", record.provider_receipt_id));
        const RecordUpdate unknown = co_await marking.wait();
        co_return banked_reset_claim::unknownOutcome(telemetry, context, candidate, "lookup_unavailable", unknown ? *unknown : record);
      }
      const std::optional<double> nowMs = readClock(clock);
      if (!nowMs) co_return outcome("pending", "invalid_clock", context, record);
      if (!banked_reset_claim::validRedeemResult(*lookedUp)) {
        rt::Promise<RecordUpdate> marking(banked_reset_claim::unknownOwned(kv, context, record, *nowMs, "lookup_response_invalid", record.provider_receipt_id));
        const RecordUpdate unknown = co_await marking.wait();
        co_return banked_reset_claim::unknownOutcome(telemetry, context, candidate, "lookup_response_invalid", unknown ? *unknown : record);
      }
      if (std::holds_alternative<banked_reset_provider::RedeemResetResultRejected>(*lookedUp) ||
          std::holds_alternative<banked_reset_provider::RedeemResetResultUnknown>(*lookedUp)) {
        // A recovery lookup can race a slow original provider invocation after its
        // lease expires. A negative lookup alone is therefore not proof that a
        // reset was never spent; only independent verification may resolve it.
        rt::Promise<CodexBankedResetOutcome> verifying(verifyOwned(kv, context, record, candidate, provider, clock, telemetry));
        CodexBankedResetOutcome verified = co_await verifying.wait();
        co_return verified;
      }
      const std::optional<std::string> receipt = banked_reset_claim::receiptId(providerReceiptIdOf(*lookedUp));
      if (!receipt) {
        rt::Promise<RecordUpdate> marking(banked_reset_claim::unknownOwned(kv, context, record, *nowMs, "lookup_response_invalid", record.provider_receipt_id));
        const RecordUpdate unknown = co_await marking.wait();
        co_return banked_reset_claim::unknownOutcome(telemetry, context, candidate, "lookup_response_invalid", unknown ? *unknown : record);
      }
      rt::Promise<RecordUpdate> preserving(
        banked_reset_claim::preserveReceipt(kv, context, record, *nowMs, banked_reset_claim::durableReceiptId(provider.get(), receipt)));
      const RecordUpdate submitted = co_await preserving.wait();
      if (!submitted) co_return outcome("pending", "lookup_cas_failed", context, record);
      rt::Promise<CodexBankedResetOutcome> verifying(verifyOwned(kv, context, *submitted, candidate, provider, clock, telemetry));
      CodexBankedResetOutcome verified = co_await verifying.wait();
      co_return verified;
    }

    /**
     * A claimed record may only be submitted while its UTC day and quota window are
     * still open. A violation is durably rejected, exactly as before.
     */
    rt::Task<std::optional<CodexBankedResetOutcome>> rejectIfClaimWindowClosed(std::shared_ptr<rt::Kv> kv, ResetContext context,
                                                                               CodexResetRedemptionRecord record, CodexBankedResetCandidate candidate,
                                                                               double nowMs) {
      if (!banked_reset::claimedDuringCurrentUtcDay(record, nowMs)) {
        rt::Promise<RecordUpdate> rejecting(banked_reset_claim::rejectOwned(kv, context, record, nowMs, "claim_day_elapsed"));
        const RecordUpdate rejected = co_await rejecting.wait();
        co_return outcome("rejected", "claim_day_elapsed", context, rejected ? *rejected : record);
      }
      if (!banked_reset_claim::quotaWindowIsOpen(candidate, nowMs)) {
        rt::Promise<RecordUpdate> rejecting(banked_reset_claim::rejectOwned(kv, context, record, nowMs, "quota_window_expired"));
        const RecordUpdate rejected = co_await rejecting.wait();
        co_return outcome("rejected", "quota_window_expired", context, rejected ? *rejected : record);
      }
      co_return std::nullopt;
    }

    /** Pre-inventory policy and clock checks; `ready` carries the observed time. */
    rt::Task<ReadyStep> submissionPreflight(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexResetRedemptionRecord record,
                                            CodexBankedResetCandidate candidate, CodexBankedResetDependencies dependencies, std::function<double()> clock) {
      const std::optional<std::string> initialPolicy = banked_reset_claim::liveSubmissionPolicyReason(dependencies, isManual(candidate));
      if (rt::truthy(initialPolicy)) co_return StepOutcome{.outcome = outcome("pending", newSubmissionReason(initialPolicy), context, record)};
      const std::optional<double> nowBeforeInventory = readClock(clock);
      if (!nowBeforeInventory) co_return StepOutcome{.outcome = outcome("pending", "invalid_clock", context, record)};
      rt::Promise<std::optional<CodexBankedResetOutcome>> checking(rejectIfClaimWindowClosed(kv, context, record, candidate, *nowBeforeInventory));
      std::optional<CodexBankedResetOutcome> closed = co_await checking.wait();
      if (closed) co_return StepOutcome{.outcome = std::move(*closed)};
      if (callerAborted(candidate)) {
        rt::Promise<RecordUpdate> rejecting(banked_reset_claim::rejectOwned(kv, context, record, *nowBeforeInventory, "client_aborted_before_submission"));
        const RecordUpdate rejected = co_await rejecting.wait();
        co_return StepOutcome{.outcome = outcome("rejected", "client_aborted_before_submission", context, rejected ? *rejected : record)};
      }
      co_return StepReady{.nowMs = *nowBeforeInventory};
    }

    /** Inventory fallback: read once, re-check the claim, and select one credit. */
    rt::Task<SelectedStep> resolveInventoryCredit(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexResetRedemptionRecord record,
                                                  CodexBankedResetCandidate candidate, CodexBankedResetDependencies dependencies,
                                                  std::function<double()> clock, CodexBankedResetTelemetry telemetry) {
      std::optional<ResetInventory> inventory;
      bool unavailable = false;
      try {
        rt::Promise<ResetInventory> reading(dependencies.provider->readInventory(context.account, banked_reset::boundedInventorySignal(candidate.signal)));
        inventory = co_await reading.wait();
      } catch (...) {
        unavailable = true;
      }
      if (unavailable) {
        const std::optional<double> nowMs = readClock(clock);
        if (!nowMs) co_return StepOutcome{.outcome = outcome("pending", "invalid_clock", context, record)};
        rt::Promise<RecordUpdate> rejecting(banked_reset_claim::rejectOwned(kv, context, record, *nowMs, "inventory_unavailable"));
        const RecordUpdate rejected = co_await rejecting.wait();
        emit(telemetry, "codex_reset_rejected", telemetryFields(context, candidate, json::object{{"state", "rejected"}, {"reason", "inventory_unavailable"}}));
        co_return StepOutcome{.outcome = outcome("rejected", "inventory_unavailable", context, rejected ? *rejected : record)};
      }
      const std::optional<double> nowMs = readClock(clock);
      if (!nowMs) co_return StepOutcome{.outcome = outcome("pending", "invalid_clock", context, record)};
      rt::Promise<std::optional<CodexBankedResetOutcome>> checking(rejectIfClaimWindowClosed(kv, context, record, candidate, *nowMs));
      std::optional<CodexBankedResetOutcome> closed = co_await checking.wait();
      if (closed) co_return StepOutcome{.outcome = std::move(*closed)};
      if (!banked_reset_claim::validInventory(*inventory, *nowMs)) {
        rt::Promise<RecordUpdate> rejecting(banked_reset_claim::rejectOwned(kv, context, record, *nowMs, "inventory_response_invalid_or_unsupported"));
        const RecordUpdate rejected = co_await rejecting.wait();
        emit(telemetry, "codex_reset_rejected",
             telemetryFields(context, candidate, json::object{{"state", "rejected"}, {"reason", "inventory_response_invalid_or_unsupported"}}));
        co_return StepOutcome{.outcome = outcome("rejected", "inventory_response_invalid_or_unsupported", context, rejected ? *rejected : record)};
      }
      const banked_reset_claim::InventoryCreditSelection selection = banked_reset_claim::selectInventoryCredit(*inventory, dependencies.provider.get(), *nowMs);
      if (!std::holds_alternative<banked_reset_claim::InventoryCreditSelectionSelected>(selection)) {
        const std::string reason =
          std::holds_alternative<banked_reset_claim::InventoryCreditSelectionEmpty>(selection) ? "inventory_empty" : "inventory_no_eligible_codex_credit";
        rt::Promise<RecordUpdate> rejecting(banked_reset_claim::rejectOwned(kv, context, record, *nowMs, reason));
        const RecordUpdate rejected = co_await rejecting.wait();
        emit(telemetry, "codex_reset_rejected", telemetryFields(context, candidate, json::object{{"state", "rejected"}, {"reason", reason}}));
        co_return StepOutcome{.outcome = outcome("rejected", reason, context, rejected ? *rejected : record)};
      }
      co_return StepSelected{.credit = std::get<banked_reset_claim::InventoryCreditSelectionSelected>(selection).credit, .nowMs = *nowMs};
    }

    /** The credit to spend must still be a live, supported Codex rate-limit credit. */
    bool isUsableSelectedCredit(const std::optional<ResetInventoryCredit>& credit, const CodexUsageResetProvider* provider, double nowMs) {
      return credit && banked_reset::isNonEmptyText(std::string_view(credit->id), 512) && credit->status == "available" &&
             credit->resetType == "codex_rate_limits" && banked_reset_provider::providerSupportsResetType(provider, json::value(credit->resetType)) &&
             (!credit->expiresAtMs || (banked_reset::isSafeMs(*credit->expiresAtMs) && *credit->expiresAtMs > nowMs));
    }

    /**
     * Resolve the credit for this submission: the evaluator's in-memory pick when
     * present, otherwise one read of the provider inventory.
     */
    rt::Task<SelectedStep> resolveSelectedCredit(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexResetRedemptionRecord record,
                                                 CodexBankedResetCandidate candidate, CodexBankedResetDependencies dependencies, std::function<double()> clock,
                                                 CodexBankedResetTelemetry telemetry, double nowBeforeInventory) {
      std::optional<ResetInventoryCredit> selectedCredit = candidate.selectedCredit;
      double nowAfterInventory = nowBeforeInventory;
      if (!selectedCredit) {
        rt::Promise<SelectedStep> resolving(resolveInventoryCredit(kv, context, record, candidate, dependencies, clock, telemetry));
        SelectedStep resolved = co_await resolving.wait();
        if (StepOutcome* settled = std::get_if<StepOutcome>(&resolved)) co_return StepOutcome{.outcome = std::move(settled->outcome)};
        selectedCredit = std::get<StepSelected>(resolved).credit;
        nowAfterInventory = std::get<StepSelected>(resolved).nowMs;
      }
      if (!isUsableSelectedCredit(selectedCredit, dependencies.provider.get(), nowAfterInventory)) {
        rt::Promise<RecordUpdate> rejecting(banked_reset_claim::rejectOwned(kv, context, record, nowAfterInventory, "selected_credit_invalid_or_expired"));
        const RecordUpdate rejected = co_await rejecting.wait();
        emit(telemetry, "codex_reset_rejected",
             telemetryFields(context, candidate, json::object{{"state", "rejected"}, {"reason", "selected_credit_invalid_or_expired"}}));
        co_return StepOutcome{.outcome = outcome("rejected", "selected_credit_invalid_or_expired", context, rejected ? *rejected : record)};
      }
      rt::Promise<std::optional<CodexBankedResetOutcome>> checking(rejectIfClaimWindowClosed(kv, context, record, candidate, nowAfterInventory));
      std::optional<CodexBankedResetOutcome> closed = co_await checking.wait();
      if (closed) co_return StepOutcome{.outcome = std::move(*closed)};
      co_return StepSelected{.credit = *selectedCredit, .nowMs = nowAfterInventory};
    }

    /**
     * Cross the durable `claimed -> submitted` side-effect boundary after the last
     * policy re-read, keeping the observed inventory time as the commit time.
     */
    rt::Task<PreparedStep> prepareLiveSubmission(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexResetRedemptionRecord record,
                                                 CodexBankedResetCandidate candidate, CodexBankedResetDependencies dependencies, std::function<double()> clock,
                                                 double nowAfterInventory) {
      // Re-read the kill switch after inventory and immediately before the fenced
      // side-effect boundary. A disable leaves `claimed` intact and makes no call.
      const banked_reset_claim::LoadLiveSubmissionConfigResult finalConfig = banked_reset_claim::loadLiveSubmissionConfig(dependencies, isManual(candidate));
      if (!finalConfig.config) co_return StepOutcome{.outcome = outcome("pending", newSubmissionReason(finalConfig.reason), context, record)};
      if (callerAborted(candidate)) {
        rt::Promise<RecordUpdate> rejecting(banked_reset_claim::rejectOwned(kv, context, record, nowAfterInventory, "client_aborted_before_submission"));
        const RecordUpdate rejected = co_await rejecting.wait();
        co_return StepOutcome{.outcome = outcome("rejected", "client_aborted_before_submission", context, rejected ? *rejected : record)};
      }
      // Inventory validation and policy checks may take long enough for the
      // current quota window to end. Do not cross the durable side-effect
      // boundary after that deadline.
      const std::optional<double> nowBeforePreparation = readClock(clock);
      if (!nowBeforePreparation) co_return StepOutcome{.outcome = outcome("pending", "invalid_clock", context, record)};
      rt::Promise<std::optional<CodexBankedResetOutcome>> checking(rejectIfClaimWindowClosed(kv, context, record, candidate, *nowBeforePreparation));
      std::optional<CodexBankedResetOutcome> closed = co_await checking.wait();
      if (closed) co_return StepOutcome{.outcome = std::move(*closed)};
      rt::Promise<banked_reset_claim::SubmissionPreparation> preparing(
        banked_reset_claim::prepareSubmission(kv, context, candidate, record, *nowBeforePreparation, clock, finalConfig.config->maxPerAccountPerDay));
      banked_reset_claim::SubmissionPreparation prepared = co_await preparing.wait();
      if (const auto* failure = std::get_if<banked_reset_claim::SubmissionPreparationFailure>(&prepared)) {
        co_return StepOutcome{.outcome = outcome(failure->code == "account_day_limit_reached" ? "skipped" : "pending", failure->code, context, record)};
      }
      co_return StepPrepared{.record = std::move(std::get<banked_reset_claim::SubmissionPreparationSubmitted>(prepared).record)};
    }

    rt::Task<CodexBankedResetOutcome> renewAndRedeem(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexBankedResetCandidate candidate,
                                                     CodexBankedResetDependencies dependencies, std::function<double()> clock,
                                                     CodexBankedResetTelemetry telemetry, ResetInventoryCredit credit, CodexResetRedemptionRecord preparedRecord);

    rt::Task<CodexBankedResetOutcome> submitClaimed(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexResetRedemptionRecord record,
                                                    CodexBankedResetCandidate candidate, CodexBankedResetDependencies dependencies, std::function<double()> clock,
                                                    CodexBankedResetTelemetry telemetry) {
      rt::Promise<ReadyStep> preflighting(submissionPreflight(kv, context, record, candidate, dependencies, clock));
      ReadyStep preflight = co_await preflighting.wait();
      if (StepOutcome* settled = std::get_if<StepOutcome>(&preflight)) co_return std::move(settled->outcome);
      rt::Promise<SelectedStep> resolving(
        resolveSelectedCredit(kv, context, record, candidate, dependencies, clock, telemetry, std::get<StepReady>(preflight).nowMs));
      SelectedStep resolution = co_await resolving.wait();
      if (StepOutcome* settled = std::get_if<StepOutcome>(&resolution)) co_return std::move(settled->outcome);
      const StepSelected& selected = std::get<StepSelected>(resolution);
      rt::Promise<PreparedStep> preparing(prepareLiveSubmission(kv, context, record, candidate, dependencies, clock, selected.nowMs));
      PreparedStep preparation = co_await preparing.wait();
      if (StepOutcome* settled = std::get_if<StepOutcome>(&preparation)) co_return std::move(settled->outcome);
      rt::Promise<CodexBankedResetOutcome> redeeming(
        renewAndRedeem(kv, context, candidate, dependencies, clock, telemetry, selected.credit, std::get<StepPrepared>(preparation).record));
      CodexBankedResetOutcome redeemed = co_await redeeming.wait();
      co_return redeemed;
    }

    /**
     * The last checks before the provider call. This function deliberately does not
     * await: nothing may run between the final kill-switch read and `redeem`
     * except these in-memory checks.
     */
    ReadyStep redeemGate(const CodexBankedResetCandidate& candidate, const CodexResetRedemptionRecord& record, const ResetContext& context,
                         const std::function<double()>& clock) {
      const std::optional<double> nowMs = readClock(clock);
      if (!nowMs) return StepOutcome{.outcome = outcome("pending", "invalid_clock", context, record)};
      if (!banked_reset::claimedDuringCurrentUtcDay(record, *nowMs)) return StepOutcome{.outcome = outcome("pending", "claim_day_elapsed", context, record)};
      if (!banked_reset_claim::quotaWindowIsOpen(candidate, *nowMs)) return StepOutcome{.outcome = outcome("pending", "quota_window_expired", context, record)};
      if (record.lease_expires_at_ms <= *nowMs) return StepOutcome{.outcome = outcome("pending", "stale_owner", context, record)};
      return StepReady{.nowMs = *nowMs};
    }

    /**
     * A parsed terminal redeem outcome is finalized directly when the contract says
     * that outcome is authoritative; `null` leaves the result to the ordinary path.
     */
    rt::Task<std::optional<CodexBankedResetOutcome>> finalizeTerminalRedeemResult(std::shared_ptr<rt::Kv> kv, ResetContext context,
                                                                                  CodexResetRedemptionRecord record, CodexBankedResetCandidate candidate,
                                                                                  std::shared_ptr<CodexUsageResetProvider> provider, double nowMs,
                                                                                  RedeemResetResult submittedResult, CodexBankedResetTelemetry telemetry) {
      const bool completed = std::holds_alternative<banked_reset_provider::RedeemResetResultCompleted>(submittedResult);
      if (!completed && !std::holds_alternative<banked_reset_provider::RedeemResetResultAlreadyRedeemed>(submittedResult)) co_return std::nullopt;
      if (!banked_reset_provider::providerTreatsRedeemOutcomeAsFinal(provider.get())) co_return std::nullopt;
      emit(telemetry, "codex_reset_submitted", telemetryFields(context, candidate, json::object{{"state", "submitted"}, {"provider_receipt_id", nullptr}}));
      rt::Promise<CodexBankedResetOutcome> finalizing(
        finalizeDocumentedRedeemOutcome(kv, context, record, candidate, nowMs, telemetry, completed ? "reset" : "already_redeemed"));
      CodexBankedResetOutcome finalized = co_await finalizing.wait();
      co_return finalized;
    }

    /** Record the receipt durably and verify the reset it belongs to. */
    rt::Task<CodexBankedResetOutcome> persistReceiptAndVerify(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexResetRedemptionRecord record,
                                                              CodexBankedResetCandidate candidate, std::shared_ptr<CodexUsageResetProvider> provider,
                                                              double nowMs, std::string receipt, std::function<double()> clock,
                                                              CodexBankedResetTelemetry telemetry) {
      const std::optional<std::string> receiptValue(receipt);
      rt::Promise<RecordUpdate> preserving(
        banked_reset_claim::preserveReceipt(kv, context, record, nowMs, banked_reset_claim::durableReceiptId(provider.get(), receiptValue)));
      const RecordUpdate persistedReceipt = co_await preserving.wait();
      if (!persistedReceipt) co_return outcome("pending", "receipt_cas_failed", context, record);
      const std::optional<std::string> durableReceipt = banked_reset_claim::durableReceiptId(provider.get(), receiptValue);
      emit(telemetry, "codex_reset_submitted",
           telemetryFields(context, candidate,
                           json::object{
                             {"state", "submitted"},
                             {"provider_receipt_id", durableReceipt ? json::value(*durableReceipt) : json::value(nullptr)},
                           }));
      rt::Promise<CodexBankedResetOutcome> verifying(verifyOwned(kv, context, *persistedReceipt, candidate, provider, clock, telemetry));
      CodexBankedResetOutcome verified = co_await verifying.wait();
      co_return verified;
    }

    /** Invoke `redeem` once and finalize the durable record for its outcome. */
    rt::Task<CodexBankedResetOutcome> redeemSubmittedOwned(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexResetRedemptionRecord record,
                                                           CodexBankedResetCandidate candidate, std::shared_ptr<CodexUsageResetProvider> provider,
                                                           ResetInventoryCredit credit, std::function<double()> clock, CodexBankedResetTelemetry telemetry) {
      std::optional<rt::Promise<RedeemResetResult>> submittedPromise;
      bool threw = false;
      try {
        // Do not insert telemetry or another await between the final synchronous
        // kill-switch check in the caller and starting the provider invocation.
        banked_reset_provider::RedeemResetInput input;
        static_cast<banked_reset_provider::ResetAccountContext&>(input) = context.account;
        input.idempotencyKey = context.idempotencyKey;
        input.creditId = credit.id;
        submittedPromise.emplace(provider->redeem(std::move(input), signalOrFresh(candidate)));
      } catch (...) {
        threw = true;
      }
      if (threw) {
        const std::optional<double> nowMs = readClock(clock);
        if (!nowMs) co_return outcome("pending", "invalid_clock", context, record);
        rt::Promise<RecordUpdate> marking(banked_reset_claim::unknownOwned(kv, context, record, *nowMs, "submit_transport_unknown", std::nullopt));
        const RecordUpdate unknown = co_await marking.wait();
        co_return banked_reset_claim::unknownOutcome(telemetry, context, candidate, "submit_transport_unknown", unknown ? *unknown : record);
      }
      emit(telemetry, "codex_reset_submit_started", telemetryFields(context, candidate, json::object{{"state", "submitted"}}));
      metric(telemetry, "codex_reset_submission_attempts_total", 1, telemetryFields(context, candidate));

      std::optional<RedeemResetResult> submittedResult;
      bool rejectedTransport = false;
      try {
        submittedResult = co_await submittedPromise->wait();
      } catch (...) {
        rejectedTransport = true;
      }
      if (rejectedTransport) {
        const std::optional<double> nowMs = readClock(clock);
        if (!nowMs) co_return outcome("pending", "invalid_clock", context, record);
        rt::Promise<RecordUpdate> marking(banked_reset_claim::unknownOwned(kv, context, record, *nowMs, "submit_transport_unknown", std::nullopt));
        const RecordUpdate unknown = co_await marking.wait();
        co_return banked_reset_claim::unknownOutcome(telemetry, context, candidate, "submit_transport_unknown", unknown ? *unknown : record);
      }
      const std::optional<double> nowAfterRedeem = readClock(clock);
      if (!nowAfterRedeem) co_return outcome("pending", "invalid_clock", context, record);
      if (!banked_reset_claim::validRedeemResult(*submittedResult)) {
        rt::Promise<RecordUpdate> marking(banked_reset_claim::unknownOwned(kv, context, record, *nowAfterRedeem, "submit_response_invalid", std::nullopt));
        const RecordUpdate unknown = co_await marking.wait();
        co_return banked_reset_claim::unknownOutcome(telemetry, context, candidate, "submit_response_invalid", unknown ? *unknown : record);
      }
      if (std::holds_alternative<banked_reset_provider::RedeemResetResultRejected>(*submittedResult)) {
        rt::Promise<RecordUpdate> rejecting(banked_reset_claim::rejectOwned(kv, context, record, *nowAfterRedeem, "provider_rejected"));
        const RecordUpdate rejected = co_await rejecting.wait();
        emit(telemetry, "codex_reset_rejected", telemetryFields(context, candidate, json::object{{"state", "rejected"}, {"reason", "provider_rejected"}}));
        co_return outcome("rejected", "provider_rejected", context, rejected ? *rejected : record);
      }
      if (const auto* unknownResult = std::get_if<banked_reset_provider::RedeemResetResultUnknown>(&*submittedResult)) {
        rt::Promise<RecordUpdate> marking(banked_reset_claim::unknownOwned(kv, context, record, *nowAfterRedeem, "provider_commit_unknown",
                                                                          banked_reset_claim::durableReceiptId(provider.get(), unknownResult->providerReceiptId)));
        const RecordUpdate unknown = co_await marking.wait();
        co_return banked_reset_claim::unknownOutcome(telemetry, context, candidate, "provider_commit_unknown", unknown ? *unknown : record);
      }
      rt::Promise<std::optional<CodexBankedResetOutcome>> finalizing(
        finalizeTerminalRedeemResult(kv, context, record, candidate, provider, *nowAfterRedeem, *submittedResult, telemetry));
      std::optional<CodexBankedResetOutcome> terminal = co_await finalizing.wait();
      if (terminal) co_return std::move(*terminal);
      const std::optional<std::string> receipt = banked_reset_claim::receiptId(providerReceiptIdOf(*submittedResult));
      if (!receipt) {
        rt::Promise<RecordUpdate> marking(banked_reset_claim::unknownOwned(kv, context, record, *nowAfterRedeem, "submit_response_invalid", std::nullopt));
        const RecordUpdate unknown = co_await marking.wait();
        co_return banked_reset_claim::unknownOutcome(telemetry, context, candidate, "submit_response_invalid", unknown ? *unknown : record);
      }
      rt::Promise<CodexBankedResetOutcome> persisting(
        persistReceiptAndVerify(kv, context, record, candidate, provider, *nowAfterRedeem, *receipt, clock, telemetry));
      CodexBankedResetOutcome persisted = co_await persisting.wait();
      co_return persisted;
    }

    /**
     * Renew the lease at the last possible moment and then invoke the provider.
     * Every policy read here is synchronous, so a disable observed after the final
     * renewal still prevents the redemption.
     */
    rt::Task<CodexBankedResetOutcome> renewAndRedeem(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexBankedResetCandidate candidate,
                                                     CodexBankedResetDependencies dependencies, std::function<double()> clock,
                                                     CodexBankedResetTelemetry telemetry, ResetInventoryCredit credit, CodexResetRedemptionRecord preparedRecord) {
      // `prepareSubmission` itself awaits strong reads and a CAS. Re-read the
      // kill switch after that durable transition. A disable visible at this
      // final pre-renewal check leaves the conservative `submitted` record
      // available for non-submitting recovery and makes no provider call.
      const std::optional<std::string> beforeRedeemPolicy = banked_reset_claim::liveSubmissionPolicyReason(dependencies, isManual(candidate));
      if (rt::truthy(beforeRedeemPolicy)) co_return outcome("pending", newSubmissionReason(beforeRedeemPolicy), context, preparedRecord);
      if (callerAborted(candidate)) {
        const std::optional<double> nowMs = readClock(clock);
        if (!nowMs) co_return outcome("pending", "invalid_clock", context, preparedRecord);
        rt::Promise<RecordUpdate> marking(
          banked_reset_claim::unknownOwned(kv, context, preparedRecord, *nowMs, "client_aborted_after_submission", std::nullopt));
        const RecordUpdate unknown = co_await marking.wait();
        co_return banked_reset_claim::unknownOutcome(telemetry, context, candidate, "client_aborted_after_submission", unknown ? *unknown : preparedRecord);
      }
      rt::Promise<banked_reset_claim::SubmissionRenewal> renewing(banked_reset_claim::renewSubmittedForRedeem(kv, context, candidate, preparedRecord, clock));
      const banked_reset_claim::SubmissionRenewal renewal = co_await renewing.wait();
      if (const auto* failure = std::get_if<banked_reset_claim::SubmissionRenewalFailure>(&renewal)) {
        co_return outcome("pending", failure->code, context, preparedRecord);
      }
      const CodexResetRedemptionRecord& renewed = std::get<banked_reset_claim::SubmissionRenewalRenewed>(renewal).record;
      // The last lease/fence renewal itself awaits KV. Re-read the kill switch
      // synchronously after it returns so a disable that landed during that final
      // renewal cannot proceed to the provider call. `reloadConfig` is
      // deliberately synchronous; do not introduce an await after this point.
      const std::optional<std::string> afterRenewalPolicy = banked_reset_claim::liveSubmissionPolicyReason(dependencies, isManual(candidate));
      if (rt::truthy(afterRenewalPolicy)) {
        co_return outcome("pending", newSubmissionReason(afterRenewalPolicy), context, renewed);
      }
      if (callerAborted(candidate)) {
        const std::optional<double> nowMs = readClock(clock);
        if (!nowMs) co_return outcome("pending", "invalid_clock", context, renewed);
        rt::Promise<RecordUpdate> marking(banked_reset_claim::unknownOwned(kv, context, renewed, *nowMs, "client_aborted_after_submission", std::nullopt));
        const RecordUpdate unknown = co_await marking.wait();
        co_return banked_reset_claim::unknownOutcome(telemetry, context, candidate, "client_aborted_after_submission", unknown ? *unknown : renewed);
      }
      const ReadyStep gate = redeemGate(candidate, renewed, context, clock);
      if (const StepOutcome* settled = std::get_if<StepOutcome>(&gate)) co_return settled->outcome;
      rt::Promise<CodexBankedResetOutcome> redeeming(redeemSubmittedOwned(kv, context, renewed, candidate, dependencies.provider, credit, clock, telemetry));
      CodexBankedResetOutcome redeemed = co_await redeeming.wait();
      co_return redeemed;
    }
  }

  rt::Task<std::shared_ptr<rt::Kv>> openResetKv(CodexBankedResetDependencies dependencies) {
    std::shared_ptr<rt::Kv> kv;
    try {
      if (!dependencies.kv) {
        rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
        kv = co_await opening.wait();
      } else {
        kv = *dependencies.kv;
      }
    } catch (...) {
      kv = nullptr;
    }
    co_return kv;
  }

  namespace {
    /** A settled record is terminal: it is reported and never reopened. */
    std::optional<CodexBankedResetOutcome> settledRecordDisposition(const CodexBankedResetTelemetry& telemetry, const ResetContext& context,
                                                                    const CodexBankedResetCandidate& candidate,
                                                                    const std::optional<CodexResetRedemptionRecord>& existing,
                                                                    const CodexBankedResetTelemetryFields& fields) {
      if (existing && existing->state == CodexResetRedemptionState::verified) {
        if (existing->routing_generation != candidate.routingGeneration) {
          // The provider reset was verified for an older routing observation. The
          // old credit is already spent, and the post-reset probe may already have
          // re-blocked the account. Never present that old verification as a
          // recovery candidate for a newer quota circuit.
          emit(telemetry, "codex_reset_duplicate_prevented",
               telemetryFields(context, candidate,
                               json::object{
                                 {"state", "verified"},
                                 {"fence", rt::makeNumber(existing->fence)},
                                 {"reason", "routing_generation_stale"},
                               }));
          metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
          return outcome("skipped", "verified_routing_generation_stale", context, existing);
        }
        emit(telemetry, "codex_reset_duplicate_prevented",
             telemetryFields(context, candidate, json::object{{"state", "verified"}, {"fence", rt::makeNumber(existing->fence)}}));
        metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
        return outcome("verified", "previously_verified", context, existing);
      }
      if (existing && existing->state == CodexResetRedemptionState::rejected) {
        emit(telemetry, "codex_reset_duplicate_prevented",
             telemetryFields(context, candidate, json::object{{"state", "rejected"}, {"fence", rt::makeNumber(existing->fence)}}));
        metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
        return outcome("rejected", existing->last_error_code.value_or("previously_rejected"), context, existing);
      }
      return std::nullopt;
    }

    /** Policy gate for the very first claim of an observed quota window. */
    rt::Task<AllowanceStep> resolveNewSubmissionAllowance(CodexBankedResetCandidate candidate, std::shared_ptr<rt::Kv> kv, ResetContext context,
                                                          CodexBankedResetDependencies dependencies, bool reconcileOnly, CodexBankedResetTelemetry telemetry,
                                                          CodexBankedResetTelemetryFields fields) {
      if (reconcileOnly) co_return StepOutcome{.outcome = outcome("skipped", "no_existing_transaction", context)};
      std::optional<CodexBankedResetConfig> configForClaim;
      bool usageDisabled = false;
      bool unavailable = false;
      try {
        if (!isManual(candidate)) {
          rt::Promise<reset_settings::ReadCodexResetUsageResult> reading(reset_settings::readCodexResetUsage(kv, context.account.accountIdHash));
          const reset_settings::ReadCodexResetUsageResult usage = co_await reading.wait();
          usageDisabled = !usage.allowed;
        }
        if (!usageDisabled) configForClaim = dependencies.reloadConfig ? dependencies.reloadConfig() : dependencies.config;
      } catch (...) {
        unavailable = true;
      }
      if (unavailable) co_return StepOutcome{.outcome = outcome("skipped", "configuration_unavailable", context)};
      if (usageDisabled) co_return StepOutcome{.outcome = outcome("skipped", "usage_disabled", context)};
      const std::optional<std::string> reason = banked_reset::policyReason(*configForClaim, isManual(candidate));
      if (rt::truthy(reason)) co_return StepOutcome{.outcome = outcome("skipped", *reason, context)};
      const std::optional<std::string> providerReason = banked_reset::providerPolicyReason(*configForClaim, dependencies.provider.get(), isManual(candidate));
      if (rt::truthy(providerReason)) co_return StepOutcome{.outcome = outcome("skipped", *providerReason, context)};
      emit(telemetry, "codex_reset_eligible", fields);
      metric(telemetry, "codex_reset_eligible_total", 1, fields);
      if (configForClaim->mode == "shadow" && !isManual(candidate)) {
        emit(telemetry, "codex_reset_shadow_candidate", fields);
        metric(telemetry, "codex_reset_shadow_candidates_total", 1, fields);
        // Shadow mode deliberately makes no provider call, including inventory.
        co_return StepOutcome{.outcome = outcome("skipped", "shadow", context)};
      }
      co_return StepAllow{.allowNewSubmission = true};
    }

    /**
     * Whether this call may create a new submission. An existing `submitted` or
     * `unknown` record is only ever reconciled, never re-submitted.
     */
    rt::Task<AllowanceStep> resolveSubmissionAllowance(CodexBankedResetCandidate candidate, std::shared_ptr<rt::Kv> kv, ResetContext context,
                                                       std::optional<CodexResetRedemptionRecord> existing, CodexBankedResetDependencies dependencies,
                                                       bool reconcileOnly, CodexBankedResetTelemetry telemetry, CodexBankedResetTelemetryFields fields) {
      if (!existing) {
        rt::Promise<AllowanceStep> resolving(resolveNewSubmissionAllowance(candidate, kv, context, dependencies, reconcileOnly, telemetry, fields));
        AllowanceStep resolved = co_await resolving.wait();
        co_return resolved;
      }
      if (existing->state != CodexResetRedemptionState::claimed) co_return StepAllow{.allowNewSubmission = false};
      if (reconcileOnly) co_return StepOutcome{.outcome = outcome("pending", "unsubmitted_transaction", context, existing)};
      const std::optional<std::string> reason = banked_reset_claim::liveSubmissionPolicyReason(dependencies, isManual(candidate));
      if (rt::truthy(reason)) co_return StepOutcome{.outcome = outcome("pending", newSubmissionReason(reason), context, existing)};
      co_return StepAllow{.allowNewSubmission = true};
    }

    /** Translate one claim result into the caller-visible outcome. */
    rt::Task<CodexBankedResetOutcome> handleClaimResult(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexBankedResetCandidate candidate,
                                                        CodexBankedResetDependencies dependencies, std::function<double()> clock,
                                                        CodexBankedResetTelemetry telemetry, CodexBankedResetTelemetryFields fields,
                                                        banked_reset_claim::ClaimResult claimed) {
      static_assert(std::variant_size_v<banked_reset_claim::ClaimResult> == 8, "every claim result is handled below");
      if (const auto* failure = std::get_if<banked_reset_claim::ClaimResultFailure>(&claimed)) co_return outcome("skipped", failure->code, context);
      if (std::holds_alternative<banked_reset_claim::ClaimResultNoTransaction>(claimed)) co_return outcome("skipped", "no_existing_transaction", context);
      if (std::holds_alternative<banked_reset_claim::ClaimResultAccountDayLimit>(claimed)) co_return outcome("skipped", "account_day_limit_reached", context);
      if (const auto* inProgress = std::get_if<banked_reset_claim::ClaimResultInProgress>(&claimed)) {
        emit(telemetry, "codex_reset_duplicate_prevented",
             telemetryFields(context, candidate,
                             json::object{
                               {"state", ::uos::types::toString(inProgress->record.state)},
                               {"fence", rt::makeNumber(inProgress->record.fence)},
                             }));
        metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
        co_return outcome("pending", "transaction_in_progress", context, inProgress->record);
      }
      if (const auto* rejected = std::get_if<banked_reset_claim::ClaimResultRejected>(&claimed)) {
        emit(telemetry, "codex_reset_duplicate_prevented",
             telemetryFields(context, candidate, json::object{{"state", "rejected"}, {"fence", rt::makeNumber(rejected->record.fence)}}));
        metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
        co_return outcome("rejected", rejected->record.last_error_code.value_or("previously_rejected"), context, rejected->record);
      }
      if (const auto* verified = std::get_if<banked_reset_claim::ClaimResultVerified>(&claimed)) {
        emit(telemetry, "codex_reset_duplicate_prevented",
             telemetryFields(context, candidate, json::object{{"state", "verified"}, {"fence", rt::makeNumber(verified->record.fence)}}));
        metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
        co_return outcome("verified", "previously_verified", context, verified->record);
      }
      if (const auto* submit = std::get_if<banked_reset_claim::ClaimResultSubmit>(&claimed)) {
        emit(telemetry, "codex_reset_claimed",
             telemetryFields(context, candidate,
                             json::object{
                               {"state", "claimed"},
                               {"fence", rt::makeNumber(submit->record.fence)},
                               {"takeover", submit->tookOver},
                             }));
        rt::Promise<CodexBankedResetOutcome> submitting(submitClaimed(kv, context, submit->record, candidate, dependencies, clock, telemetry));
        CodexBankedResetOutcome submitted = co_await submitting.wait();
        co_return submitted;
      }
      if (const auto* reconcile = std::get_if<banked_reset_claim::ClaimResultReconcile>(&claimed)) {
        emit(telemetry, "codex_reset_duplicate_prevented",
             telemetryFields(context, candidate,
                             json::object{
                               {"state", ::uos::types::toString(reconcile->record.state)},
                               {"fence", rt::makeNumber(reconcile->record.fence)},
                               {"takeover", reconcile->tookOver},
                             }));
        metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
        rt::Promise<CodexBankedResetOutcome> reconciling(reconcileOwned(kv, context, reconcile->record, candidate, dependencies.provider, clock, telemetry));
        CodexBankedResetOutcome reconciled = co_await reconciling.wait();
        co_return reconciled;
      }
      // Every variant above is handled, so this branch is unreachable. Assigning
      // to a `never` makes a future variant a compile error instead of a silent
      // `undefined` return.
      throw rt::JsError("unhandled codex banked reset claim result");
    }

    rt::Task<CodexBankedResetOutcome> attemptInternal(CodexBankedResetCandidate candidate, CodexBankedResetDependencies dependencies, bool reconcileOnly) {
      const std::function<rt::Task<std::string>(std::string)> hash =
        dependencies.hash ? dependencies.hash : std::function<rt::Task<std::string>(std::string)>(&utils::sha256Hex);
      rt::Promise<std::optional<ResetContext>> contextualizing(banked_reset_claim::makeResetContext(candidate, hash));
      const std::optional<ResetContext> context = co_await contextualizing.wait();
      if (!context) co_return outcome("skipped", "invalid_quota_generation");
      const CodexBankedResetTelemetry telemetry = dependencies.telemetry ? *dependencies.telemetry : banked_reset::defaultTelemetry;
      const CodexBankedResetTelemetryFields fields = telemetryFields(*context, candidate);
      rt::Promise<std::shared_ptr<rt::Kv>> opening(openResetKv(dependencies));
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (!kv) co_return outcome("skipped", "kv_unavailable", context);

      rt::Promise<banked_reset_claim::ReadExistingRecordResult> reading(banked_reset_claim::readExistingRecord(kv, *context));
      const banked_reset_claim::ReadExistingRecordResult existing = co_await reading.wait();
      if (rt::truthy(existing.code)) co_return outcome("skipped", *existing.code, context);
      if (existing.record && !banked_reset_claim::matchesContext(*existing.record, *context)) {
        co_return outcome("skipped", "redemption_record_context_mismatch", context, existing.record);
      }
      std::optional<CodexBankedResetOutcome> settled = settledRecordDisposition(telemetry, *context, candidate, existing.record, fields);
      if (settled) co_return std::move(*settled);
      rt::Promise<AllowanceStep> allowing(
        resolveSubmissionAllowance(candidate, kv, *context, existing.record, dependencies, reconcileOnly, telemetry, fields));
      AllowanceStep allowance = co_await allowing.wait();
      if (StepOutcome* decided = std::get_if<StepOutcome>(&allowance)) co_return std::move(decided->outcome);

      if (!banked_reset_provider::providerSupportsLiveRedemption(dependencies.provider.get())) {
        if (!existing.record) {
          emit(telemetry, "codex_reset_rejected", telemetryFields(*context, candidate, json::object{{"state", "rejected"}, {"reason", "provider_contract_unproven"}}));
        }
        co_return outcome("skipped", "provider_contract_unproven", context, existing.record);
      }
      const std::function<double()> clock = dependencies.now ? dependencies.now : rt::globalDateNow();
      const std::optional<double> nowMs = readClock(clock);
      if (!nowMs) co_return outcome("skipped", "invalid_clock", context, existing.record);
      const std::function<std::string()> tokenSource =
        dependencies.newOwnerToken ? dependencies.newOwnerToken : std::function<std::string()>([] { return rt::crypto::randomUUID(); });
      const std::optional<std::string> ownerToken = banked_reset::safeOwnerToken(tokenSource);
      if (!ownerToken) co_return outcome("skipped", "owner_token_unavailable", context, existing.record);
      rt::Promise<banked_reset_claim::ClaimResult> claiming(
        banked_reset_claim::claimTransaction(kv, *context, candidate, *nowMs, clock, *ownerToken, std::get<StepAllow>(allowance).allowNewSubmission));
      banked_reset_claim::ClaimResult claimed = co_await claiming.wait();
      rt::Promise<CodexBankedResetOutcome> handling(handleClaimResult(kv, *context, candidate, dependencies, clock, telemetry, fields, std::move(claimed)));
      CodexBankedResetOutcome handled = co_await handling.wait();
      co_return handled;
    }
  }

  rt::Task<banked_reset::CodexBankedResetOutcome> attemptCodexBankedReset(CodexBankedResetCandidate candidate, CodexBankedResetDependencies dependencies) {
    rt::Promise<CodexBankedResetOutcome> attempting(attemptInternal(std::move(candidate), std::move(dependencies), false));
    CodexBankedResetOutcome attempted = co_await attempting.wait();
    co_return attempted;
  }

  rt::Task<banked_reset::CodexBankedResetOutcome> reconcileCodexBankedReset(CodexBankedResetCandidate candidate, CodexBankedResetDependencies dependencies) {
    rt::Promise<CodexBankedResetOutcome> attempting(attemptInternal(std::move(candidate), std::move(dependencies), true));
    CodexBankedResetOutcome attempted = co_await attempting.wait();
    co_return attempted;
  }
}
