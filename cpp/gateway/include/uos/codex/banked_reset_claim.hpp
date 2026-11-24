#pragma once
// Port of src/codex/banked-reset-claim.ts.
// Codex banked reset claim and submission preparation, split out of src/codex_banked_reset.ts.
//
// Every export is module-internal to the banked-reset modules (an export list in the TS).
// `kv: Kv` parameters are non-null std::shared_ptr<rt::Kv>. The provider inputs validInventory and
// validRedeemResult check are typed in C++ (the provider boundary returns ResetInventory /
// RedeemResetResult), so they take those types instead of `unknown`.
#include "uos/codex/banked_reset.hpp"
#include "uos/codex/banked_reset_provider.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::codex::banked_reset_claim {
  // `hash: (value: string) => Promise<string>`; the result is null as std::nullopt.
  [[nodiscard]] rt::Task<std::optional<banked_reset::ResetContext>> makeResetContext(banked_reset::CodexBankedResetCandidate candidate,
                                                                                     std::function<rt::Task<std::string>(std::string value)> hash);

  // `type ClaimResult` (exported through the type list), by `kind`.
  struct ClaimResultSubmit {
    static constexpr std::string_view kind = "submit";
    ::uos::types::CodexResetRedemptionRecord record;
    bool tookOver;
  };
  struct ClaimResultReconcile {
    static constexpr std::string_view kind = "reconcile";
    ::uos::types::CodexResetRedemptionRecord record;
    bool tookOver;
  };
  struct ClaimResultVerified {
    static constexpr std::string_view kind = "verified";
    ::uos::types::CodexResetRedemptionRecord record;
  };
  struct ClaimResultRejected {
    static constexpr std::string_view kind = "rejected";
    ::uos::types::CodexResetRedemptionRecord record;
  };
  struct ClaimResultInProgress {
    static constexpr std::string_view kind = "in_progress";
    ::uos::types::CodexResetRedemptionRecord record;
  };
  struct ClaimResultNoTransaction {
    static constexpr std::string_view kind = "no_transaction";
  };
  struct ClaimResultAccountDayLimit {
    static constexpr std::string_view kind = "account_day_limit";
  };
  struct ClaimResultFailure {
    static constexpr std::string_view kind = "failure";
    std::string code;
  };
  using ClaimResult = std::variant<ClaimResultSubmit, ClaimResultReconcile, ClaimResultVerified, ClaimResultRejected, ClaimResultInProgress,
                                   ClaimResultNoTransaction, ClaimResultAccountDayLimit, ClaimResultFailure>;

  // `type FenceRead` (module-private in the TS), by `kind`.
  struct FenceReadValid {
    static constexpr std::string_view kind = "valid";
    std::vector<rt::KvEntryMaybe<>> entries;
  };
  struct FenceReadStale {
    static constexpr std::string_view kind = "stale";
  };
  struct FenceReadFailure {
    static constexpr std::string_view kind = "failure";
    std::string code;
  };
  using FenceRead = std::variant<FenceReadValid, FenceReadStale, FenceReadFailure>;

  // `type SubmissionPreparation` (module-private in the TS), by `kind`.
  struct SubmissionPreparationSubmitted {
    static constexpr std::string_view kind = "submitted";
    ::uos::types::CodexResetRedemptionRecord record;
  };
  struct SubmissionPreparationFailure {
    static constexpr std::string_view kind = "failure";
    std::string code;
  };
  using SubmissionPreparation = std::variant<SubmissionPreparationSubmitted, SubmissionPreparationFailure>;

  // `type SubmissionRenewal` (module-private in the TS), by `kind`.
  struct SubmissionRenewalRenewed {
    static constexpr std::string_view kind = "renewed";
    ::uos::types::CodexResetRedemptionRecord record;
  };
  struct SubmissionRenewalFailure {
    static constexpr std::string_view kind = "failure";
    std::string code;
  };
  using SubmissionRenewal = std::variant<SubmissionRenewalRenewed, SubmissionRenewalFailure>;

  bool matchesContext(const ::uos::types::CodexResetRedemptionRecord& record, const banked_reset::ResetContext& context);

  bool quotaWindowIsOpen(const banked_reset::CodexBankedResetCandidate& candidate, double nowMs);

  // `clock: () => number`; the result is null as std::nullopt.
  std::optional<double> readClock(const std::function<double()>& clock);

  [[nodiscard]] rt::Task<FenceRead> readCurrentFences(std::shared_ptr<rt::Kv> kv, banked_reset::CodexBankedResetCandidate candidate);

  // Adds one check per entry to `operation` and returns it (the builder's own reference).
  rt::AtomicOperation& withFenceChecks(rt::AtomicOperation& operation, const std::vector<rt::KvEntryMaybe<>>& entries);

  // `Readonly<{ record: CodexResetRedemptionRecord | null; code: string | null }>`
  struct ReadExistingRecordResult {
    std::optional<::uos::types::CodexResetRedemptionRecord> record;
    std::optional<std::string> code;
  };

  [[nodiscard]] rt::Task<ReadExistingRecordResult> readExistingRecord(std::shared_ptr<rt::Kv> kv, banked_reset::ResetContext context);

  // An inventory refusal written before the durable submission boundary.
  inline bool isUnsubmittedInventoryRejection(const ::uos::types::CodexResetRedemptionRecord& record) {
    const auto& reason = record.last_error_code;
    return record.state == ::uos::types::CodexResetRedemptionState::rejected && !record.submitted_at_ms && !record.provider_receipt_id &&
           (reason == "inventory_empty" || reason == "inventory_no_eligible_codex_credit" || reason == "inventory_unavailable");
  }

  // `clock: () => number`
  [[nodiscard]] rt::Task<ClaimResult> claimTransaction(std::shared_ptr<rt::Kv> kv, banked_reset::ResetContext context,
                                                       banked_reset::CodexBankedResetCandidate candidate, double nowMs, std::function<double()> clock,
                                                       std::string ownerToken, bool allowNewSubmission);

  // `mutate: (record) => CodexResetRedemptionRecord`; the result is null as std::nullopt.
  [[nodiscard]] rt::Task<std::optional<::uos::types::CodexResetRedemptionRecord>> updateOwnedRecord(
    std::shared_ptr<rt::Kv> kv, banked_reset::ResetContext context, ::uos::types::CodexResetRedemptionRecord expected,
    std::function<::uos::types::CodexResetRedemptionRecord(const ::uos::types::CodexResetRedemptionRecord& record)> mutate);

  [[nodiscard]] rt::Task<SubmissionPreparation> prepareSubmission(std::shared_ptr<rt::Kv> kv, banked_reset::ResetContext context,
                                                                  banked_reset::CodexBankedResetCandidate candidate,
                                                                  ::uos::types::CodexResetRedemptionRecord expected, double nowMs,
                                                                  std::function<double()> clock, double maxPerAccountPerDay);

  /**
   * The `submitted` state is intentionally durable before a provider invocation
   * because a process can die after issuing it. Renew ownership again at the
   * last possible moment so a worker paused after `prepareSubmission()` cannot
   * spend a reset after losing its lease, routing fence, or auth-pool fence.
   *
   * There must be no await between a successful return and invoking `redeem`.
   */
  [[nodiscard]] rt::Task<SubmissionRenewal> renewSubmittedForRedeem(std::shared_ptr<rt::Kv> kv, banked_reset::ResetContext context,
                                                                    banked_reset::CodexBankedResetCandidate candidate,
                                                                    ::uos::types::CodexResetRedemptionRecord expected, std::function<double()> clock);

  // `value: unknown`: nullptr is undefined. The std::optional<std::string> overload is the typed call
  // (a provider result's receipt id; std::nullopt is null); the array overload takes a string literal,
  // which would otherwise be ambiguous between the json::value and std::optional<std::string> overloads.
  std::optional<std::string> receiptId(const json::value& value);
  std::optional<std::string> receiptId(const json::value* value);
  std::optional<std::string> receiptId(const std::optional<std::string>& value);
  template <std::size_t N>
  std::optional<std::string> receiptId(const char (&value)[N]) {
    return receiptId(std::optional<std::string>(std::string(value, N - 1)));
  }

  /**
   * Receipt identifiers are optional optimization hints: reconciliation is
   * required to work by deterministic idempotency key. Keep an unapproved
   * receipt in process memory only, never in the durable record or telemetry.
   */
  // `provider: Pick<CodexUsageResetProvider, "contract">` as the provider* checks take it (nullptr is an
  // absent receiver); `value: unknown` as receiptId's.
  std::optional<std::string> durableReceiptId(const banked_reset_provider::CodexUsageResetProvider* provider, const json::value& value);
  std::optional<std::string> durableReceiptId(const banked_reset_provider::CodexUsageResetProvider* provider, const json::value* value);
  std::optional<std::string> durableReceiptId(const banked_reset_provider::CodexUsageResetProvider* provider, const std::optional<std::string>& value);

  // `(inventory: unknown, nowMs): inventory is ResetInventory`, over the typed provider result.
  bool validInventory(const banked_reset_provider::ResetInventory& inventory, double nowMs);

  // `type InventoryCreditSelection` (module-private in the TS), by `kind`.
  struct InventoryCreditSelectionSelected {
    static constexpr std::string_view kind = "selected";
    banked_reset_provider::ResetInventoryCredit credit;
  };
  struct InventoryCreditSelectionEmpty {
    static constexpr std::string_view kind = "empty";
  };
  struct InventoryCreditSelectionNoEligibleCredit {
    static constexpr std::string_view kind = "no_eligible_credit";
  };
  using InventoryCreditSelection = std::variant<InventoryCreditSelectionSelected, InventoryCreditSelectionEmpty, InventoryCreditSelectionNoEligibleCredit>;

  /**
   * The only selectable credits are explicit, currently valid Codex
   * rate-limit credits. Finite expiry wins over non-expiring credits; callers
   * add the account slot as the next global tie-breaker.
   */
  // `provider` as the provider* checks take it (nullptr is an absent receiver).
  InventoryCreditSelection selectInventoryCredit(const banked_reset_provider::ResetInventory& inventory,
                                                 const banked_reset_provider::CodexUsageResetProvider* provider, double nowMs);

  // `(value: unknown): value is RedeemResetResult`, over the typed provider result.
  bool validRedeemResult(const banked_reset_provider::RedeemResetResult& value);

  // `Partial<Pick<CodexResetRedemptionRecord, "provider_receipt_id" | "submitted_at_ms" | "verified_at_ms" |
  // "last_error_code">>`: an undefined member keeps the record's value, null writes null.
  struct StateWithPatch {
    rt::Nullable<std::string> provider_receipt_id{};
    rt::Nullable<double> submitted_at_ms{};
    rt::Nullable<double> verified_at_ms{};
    rt::Nullable<std::string> last_error_code{};
  };

  // `patch = {}` is the overload without it.
  ::uos::types::CodexResetRedemptionRecord stateWith(const ::uos::types::CodexResetRedemptionRecord& record, ::uos::types::CodexResetRedemptionState state,
                                                     double nowMs);
  ::uos::types::CodexResetRedemptionRecord stateWith(const ::uos::types::CodexResetRedemptionRecord& record, ::uos::types::CodexResetRedemptionState state,
                                                     double nowMs, const StateWithPatch& patch);

  [[nodiscard]] rt::Task<std::optional<::uos::types::CodexResetRedemptionRecord>> rejectOwned(std::shared_ptr<rt::Kv> kv, banked_reset::ResetContext context,
                                                                                            ::uos::types::CodexResetRedemptionRecord record, double nowMs,
                                                                                            std::string code);

  // `providerReceiptId: string | null`
  [[nodiscard]] rt::Task<std::optional<::uos::types::CodexResetRedemptionRecord>> unknownOwned(std::shared_ptr<rt::Kv> kv, banked_reset::ResetContext context,
                                                                                             ::uos::types::CodexResetRedemptionRecord record, double nowMs,
                                                                                             std::string code, std::optional<std::string> providerReceiptId);

  // `providerReceiptId: string | null`
  [[nodiscard]] rt::Task<std::optional<::uos::types::CodexResetRedemptionRecord>> preserveReceipt(std::shared_ptr<rt::Kv> kv, banked_reset::ResetContext context,
                                                                                                ::uos::types::CodexResetRedemptionRecord record, double nowMs,
                                                                                                std::optional<std::string> providerReceiptId);

  banked_reset::CodexBankedResetOutcome unknownOutcome(const banked_reset::CodexBankedResetTelemetry& telemetry, const banked_reset::ResetContext& context,
                                                       const banked_reset::CodexBankedResetCandidate& candidate, const std::string& reason,
                                                       const ::uos::types::CodexResetRedemptionRecord& record);

  // `string | null`
  std::optional<std::string> liveSubmissionPolicyReason(const banked_reset::CodexBankedResetDependencies& dependencies, bool manual = false);

  // `Readonly<{ config: CodexBankedResetConfig; reason: null }> | Readonly<{ config: null; reason: string }>`:
  // exactly one member is set.
  struct LoadLiveSubmissionConfigResult {
    std::optional<banked_reset::CodexBankedResetConfig> config;
    std::optional<std::string> reason;
  };

  LoadLiveSubmissionConfigResult loadLiveSubmissionConfig(const banked_reset::CodexBankedResetDependencies& dependencies, bool manual = false);
}
