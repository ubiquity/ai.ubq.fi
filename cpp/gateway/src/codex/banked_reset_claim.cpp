// Port of src/codex/banked-reset-claim.ts.
// Codex banked reset claim and submission preparation, split out of src/codex_banked_reset.ts.
#include "uos/codex/banked_reset_claim.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"

#include <algorithm>
#include <initializer_list>
#include <unordered_set>
#include <utility>

namespace uos::codex::banked_reset_claim {
  namespace {
    constexpr rt::StoredJsonViewOptions STORED_JSON_VIEW_OPTIONS{
      .undefinedMember = rt::StoredJsonViewOptions::UndefinedView::omit,
      .undefinedElement = rt::StoredJsonViewOptions::UndefinedView::null,
      .hole = rt::StoredJsonViewOptions::UndefinedView::null,
      .topLevelUndefined = rt::StoredJsonViewOptions::UndefinedView::null,
      .bigint = rt::StoredJsonViewOptions::BigIntView::emptyObject,
      .kvU64Members = false,
      .typedArrayMembers = false,
    };
    using ::uos::types::CodexResetGlobalDailyRecord;
    using ::uos::types::CodexResetRedemptionRecord;
    using ::uos::types::CodexResetRedemptionState;
    using banked_reset::CODEX_BANKED_RESET_INVENTORY_MAX_AGE_MS;
    using banked_reset::CODEX_BANKED_RESET_LEASE_MS;
    using banked_reset::CodexBankedResetCandidate;
    using banked_reset::CodexBankedResetConfig;
    using banked_reset::CodexBankedResetDependencies;
    using banked_reset::CodexBankedResetFence;
    using banked_reset::CodexBankedResetOutcome;
    using banked_reset::CodexBankedResetTelemetry;
    using banked_reset::isNonEmptyText;
    using banked_reset::isSafeMs;
    using banked_reset::isSafeNonnegativeInteger;
    using banked_reset::MAX_CAS_ATTEMPTS;
    using banked_reset::ResetContext;
    using banked_reset_provider::CodexUsageResetProvider;
    using banked_reset_provider::RedeemResetResult;
    using banked_reset_provider::ResetInventory;
    using banked_reset_provider::ResetInventoryCredit;

    rt::KvGetOptions strongRead() { return rt::KvGetOptions{.consistency = rt::KvConsistencyLevel::strong}; }

    // A stored row as the TS reads it (`unknown`): JSON-shaped parts as they are, an undefined member
    // absent and an undefined element or hole null; any other value (a Date, Map, Uint8Array, KvU64,
    // bigint, ...) a member-less object. The record parsers reject that object exactly where they
    // reject the original value (they accept only strings, numbers, null and plain records).

    rt::KvValue recordValue(const CodexResetRedemptionRecord& record) { return rt::KvValue(json::value_from(record)); }

    // `${a}\u0000${b}\u0000...`
    std::string joinWithNul(std::initializer_list<std::string_view> parts) {
      std::string joined;
      bool first = true;
      for (std::string_view part : parts) {
        if (!first) joined.push_back('\0');
        joined.append(part);
        first = false;
      }
      return joined;
    }
  }

  rt::Task<std::optional<ResetContext>> makeResetContext(CodexBankedResetCandidate candidate, std::function<rt::Task<std::string>(std::string value)> hash) {
    if (!isNonEmptyText(std::string_view(candidate.accountId), 1024) || !isNonEmptyText(std::string_view(candidate.credentialVersion), 512) ||
        !isSafeMs(candidate.quotaResetAtMs) || !rt::Number::isSafeInteger(candidate.routingGeneration) || candidate.routingGeneration < 0) {
      co_return std::nullopt;
    }
    std::optional<ResetContext> context;
    try {
      rt::Promise<std::string> hashingAccountId(hash(candidate.accountId));
      const std::string accountIdHash = co_await hashingAccountId.wait();
      // The routing layer permits this deadline-derived identity only while its
      // durable fence proves the absolute observation has not been revised.
      // A future provider-proven quota generation should replace this deadline
      // identity; an observed deadline change fails closed before this path.
      // Credential version remains a separate routing fence, so refresh cannot
      // manufacture a second logical redemption for one observed window.
      const std::string credentialVersionInput = joinWithNul({"uos_ai", "codex_reset_credential_version", candidate.credentialVersion});
      rt::Promise<std::string> hashingCredentialVersion(hash(credentialVersionInput));
      const std::string credentialVersionHash = co_await hashingCredentialVersion.wait();
      const std::string credentialVersion = "v1:" + credentialVersionHash;
      const std::string quotaGenerationInput = banked_reset::codexResetQuotaGenerationInput(accountIdHash, candidate.quotaResetAtMs);
      rt::Promise<std::string> hashingQuotaGeneration(hash(quotaGenerationInput));
      const std::string quotaGenerationHash = co_await hashingQuotaGeneration.wait();
      const std::string quotaGeneration = "v1:" + quotaGenerationHash;
      const std::string idempotencyKeyInput = joinWithNul({"uos_ai", "codex_reset_idempotency", accountIdHash, quotaGeneration});
      rt::Promise<std::string> hashingIdempotencyKey(hash(idempotencyKeyInput));
      const std::string idempotencyKeySuffix = co_await hashingIdempotencyKey.wait();
      const std::string idempotencyKey = "uos_ai_codex_reset_v1_" + idempotencyKeySuffix;
      rt::Promise<std::string> hashingIdempotencyKeyHash(hash(idempotencyKey));
      const std::string idempotencyKeyHash = co_await hashingIdempotencyKeyHash.wait();
      if (isNonEmptyText(std::string_view(accountIdHash)) && isNonEmptyText(std::string_view(credentialVersion)) &&
          isNonEmptyText(std::string_view(quotaGeneration)) && isNonEmptyText(std::string_view(idempotencyKeyHash))) {
        context = ResetContext{
          .account =
            banked_reset_provider::ResetAccountContext{
              .accountId = candidate.accountId,
              .accountIdHash = accountIdHash,
              .credentialVersion = credentialVersion,
              .quotaGeneration = quotaGeneration,
            },
          .idempotencyKey = idempotencyKey,
          .idempotencyKeyHash = idempotencyKeyHash,
        };
      }
    } catch (...) {
      context = std::nullopt;
    }
    co_return context;
  }

  bool matchesContext(const CodexResetRedemptionRecord& record, const ResetContext& context) {
    return record.account_id_hash == context.account.accountIdHash && record.credential_version == context.account.credentialVersion &&
           record.quota_generation == context.account.quotaGeneration && record.idempotency_key_hash == context.idempotencyKeyHash;
  }

  namespace {
    /**
     * Ownership fence for a record this worker still holds: same reset context, the
     * same owner token and fence it wrote, and the expected durable state.
     */
    bool isOwnedRecordInState(const std::optional<CodexResetRedemptionRecord>& current, const ResetContext& context, const CodexResetRedemptionRecord& expected,
                              CodexResetRedemptionState state) {
      return current && matchesContext(*current, context) && current->owner_token == expected.owner_token && current->fence == expected.fence &&
             current->state == state;
    }

    std::optional<double> leaseUntil(double nowMs) {
      const double next = nowMs + CODEX_BANKED_RESET_LEASE_MS;
      if (isSafeMs(next)) return next;
      return std::nullopt;
    }

    std::optional<double> nextFence(double fence) {
      const double next = fence + 1;
      if (rt::Number::isSafeInteger(next)) return next;
      return std::nullopt;
    }
  }

  bool quotaWindowIsOpen(const CodexBankedResetCandidate& candidate, double nowMs) { return nowMs < candidate.quotaResetAtMs; }

  std::optional<double> readClock(const std::function<double()>& clock) {
    try {
      const double value = clock();
      if (isSafeMs(value)) return value;
      return std::nullopt;
    } catch (...) {
      return std::nullopt;
    }
  }

  namespace {
    // `Array.isArray(fences) && fences.length > 0 && fences.every((fence) => isRecord(fence) &&
    // Array.isArray(fence.key) && typeof fence.isCurrent === "function")`: an empty predicate is not a function.
    bool hasUsableFences(const CodexBankedResetCandidate& candidate) {
      return !candidate.fences.empty() &&
             std::all_of(candidate.fences.begin(), candidate.fences.end(), [](const CodexBankedResetFence& fence) { return static_cast<bool>(fence.isCurrent); });
    }
  }

  rt::Task<FenceRead> readCurrentFences(std::shared_ptr<rt::Kv> kv, CodexBankedResetCandidate candidate) {
    if (!hasUsableFences(candidate)) co_return FenceReadFailure{.code = "routing_fence_missing"};
    std::vector<rt::KvEntryMaybe<>> entries;
    for (const CodexBankedResetFence& fence : candidate.fences) {
      rt::KvEntryMaybe<> entry;
      bool unavailable = false;
      try {
        rt::Promise<rt::KvEntryMaybe<>> reading(kv->get(fence.key, strongRead()));
        entry = co_await reading.wait();
      } catch (...) {
        unavailable = true;
      }
      if (unavailable) co_return FenceReadFailure{.code = "kv_unavailable"};
      bool current = false;
      try {
        current = fence.isCurrent(entry.value.toJson());
      } catch (...) {
        current = false;
      }
      if (!current) co_return FenceReadStale{};
      entries.push_back(std::move(entry));
    }
    co_return FenceReadValid{.entries = std::move(entries)};
  }

  rt::AtomicOperation& withFenceChecks(rt::AtomicOperation& operation, const std::vector<rt::KvEntryMaybe<>>& entries) {
    for (const rt::KvEntryMaybe<>& entry : entries) operation.check(entry);
    return operation;
  }

  namespace {
    /**
     * A fence read that a caller may act on. An unavailable read and a stale fence
     * are both stops, but only the stale fence reports a routing fence problem.
     */
    using RequiredFences = std::variant<FenceReadValid, FenceReadFailure>;

    rt::Task<RequiredFences> readRequiredFences(std::shared_ptr<rt::Kv> kv, CodexBankedResetCandidate candidate) {
      rt::Promise<FenceRead> reading(readCurrentFences(kv, candidate));
      FenceRead fences = co_await reading.wait();
      if (const FenceReadFailure* failure = std::get_if<FenceReadFailure>(&fences)) co_return FenceReadFailure{.code = failure->code};
      if (std::holds_alternative<FenceReadStale>(fences)) co_return FenceReadFailure{.code = "routing_fence_stale"};
      co_return FenceReadValid{.entries = std::move(std::get<FenceReadValid>(fences).entries)};
    }
  }

  rt::Task<ReadExistingRecordResult> readExistingRecord(std::shared_ptr<rt::Kv> kv, ResetContext context) {
    const rt::KvKey key = banked_reset::codexResetRedemptionKey(context.account.accountIdHash, context.account.quotaGeneration);
    ReadExistingRecordResult result;
    try {
      rt::Promise<rt::KvEntryMaybe<>> reading(kv->get(key, strongRead()));
      const rt::KvEntryMaybe<> entry = co_await reading.wait();
      if (!entry.value.isNull()) {
        const std::optional<CodexResetRedemptionRecord> record = banked_reset::parseCodexResetRedemptionRecord(rt::storedJsonView(entry.value, STORED_JSON_VIEW_OPTIONS));
        if (record) result.record = record;
        else result.code = "redemption_record_invalid";
      }
    } catch (...) {
      result.record = std::nullopt;
      result.code = "kv_unavailable";
    }
    co_return result;
  }

  namespace {
    /**
     * Create the first durable claim for a quota window. `null` means the atomic
     * write lost a race and the caller must re-read the record and try again.
     */
    rt::Task<std::optional<ClaimResult>> createClaimRecord(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexBankedResetCandidate candidate, double nowMs,
                                                           std::function<double()> clock, std::string ownerToken, bool allowNewSubmission, double expiresAtMs,
                                                           rt::KvEntryMaybe<> entry, rt::KvKey key) {
      if (!allowNewSubmission) co_return ClaimResultNoTransaction{};
      // Do not claim a quota window that has already recovered. In particular, this
      // must precede the daily-cap write so an expired candidate cannot consume
      // capacity without reaching the provider.
      if (!quotaWindowIsOpen(candidate, nowMs)) co_return ClaimResultFailure{.code = "quota_window_expired"};
      rt::Promise<RequiredFences> reading(readRequiredFences(kv, candidate));
      const RequiredFences fences = co_await reading.wait();
      if (const FenceReadFailure* failure = std::get_if<FenceReadFailure>(&fences)) co_return ClaimResultFailure{.code = failure->code};
      const CodexResetRedemptionRecord created{
        .v = 1,
        .account_id_hash = context.account.accountIdHash,
        .credential_version = context.account.credentialVersion,
        .quota_generation = context.account.quotaGeneration,
        .routing_generation = candidate.routingGeneration,
        .idempotency_key_hash = context.idempotencyKeyHash,
        .state = CodexResetRedemptionState::claimed,
        .owner_token = ownerToken,
        .fence = 1,
        .lease_expires_at_ms = expiresAtMs,
        .provider_receipt_id = std::nullopt,
        .created_at_ms = nowMs,
        .updated_at_ms = nowMs,
        .submitted_at_ms = std::nullopt,
        .verified_at_ms = std::nullopt,
        .last_error_code = std::nullopt,
      };
      // Fence reads are asynchronous. Re-check immediately before the atomic write
      // so a window that expired during those reads cannot create a fresh
      // submission path.
      const std::optional<double> nowBeforeClaim = readClock(clock);
      if (!nowBeforeClaim) co_return ClaimResultFailure{.code = "invalid_clock"};
      if (!quotaWindowIsOpen(candidate, *nowBeforeClaim)) {
        co_return ClaimResultFailure{.code = "quota_window_expired"};
      }
      bool unavailable = false;
      bool committedOk = false;
      try {
        std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        operation->check(entry);
        withFenceChecks(*operation, std::get<FenceReadValid>(fences).entries).set(key, recordValue(created));
        rt::Promise<rt::KvCommitResult> committing(operation->commit());
        const rt::KvCommitResult committed = co_await committing.wait();
        committedOk = committed.ok;
      } catch (...) {
        unavailable = true;
      }
      if (unavailable) co_return ClaimResultFailure{.code = "kv_unavailable"};
      if (committedOk) co_return ClaimResultSubmit{.record = created, .tookOver = false};
      co_return std::nullopt;
    }

    /** Terminal or still-owned records short-circuit a takeover before any write. */
    std::optional<ClaimResult> existingRecordDisposition(const CodexResetRedemptionRecord& record, const ResetContext& context,
                                                         const CodexBankedResetCandidate& candidate, double nowMs, bool allowNewSubmission) {
      if (!matchesContext(record, context)) return ClaimResultFailure{.code = "redemption_record_context_mismatch"};
      if (record.state == CodexResetRedemptionState::verified) return ClaimResultVerified{.record = record};
      if (record.state == CodexResetRedemptionState::rejected) return ClaimResultRejected{.record = record};
      if (record.lease_expires_at_ms > nowMs) return ClaimResultInProgress{.record = record};
      if (record.state == CodexResetRedemptionState::claimed && !allowNewSubmission) return ClaimResultInProgress{.record = record};
      // A takeover of an expired `claimed` record would otherwise create a fresh
      // submission path after the observed quota window has reopened.
      // Submitted/unknown records deliberately bypass this guard and reconcile
      // lookup-only; they are never re-redeemed here.
      if (record.state == CodexResetRedemptionState::claimed && !quotaWindowIsOpen(candidate, nowMs)) {
        return ClaimResultFailure{.code = "quota_window_expired"};
      }
      return std::nullopt;
    }

    /** Only a `claimed` record is fenced on takeover; reconciliation reads none. */
    rt::Task<RequiredFences> takeOverFences(std::shared_ptr<rt::Kv> kv, CodexBankedResetCandidate candidate, CodexResetRedemptionRecord record) {
      if (record.state != CodexResetRedemptionState::claimed) co_return FenceReadValid{};
      if (record.routing_generation != candidate.routingGeneration) {
        co_return FenceReadFailure{.code = "routing_fence_stale"};
      }
      rt::Promise<RequiredFences> reading(readRequiredFences(kv, candidate));
      RequiredFences fences = co_await reading.wait();
      co_return fences;
    }

    /** Commit the takeover. `null` means the CAS lost and the caller must retry. */
    rt::Task<std::optional<ClaimResult>> commitTakeOver(std::shared_ptr<rt::Kv> kv, CodexBankedResetCandidate candidate, CodexResetRedemptionRecord record,
                                                        double nowMs, std::function<double()> clock, std::string ownerToken, double expiresAtMs,
                                                        std::vector<rt::KvEntryMaybe<>> fenceEntries, rt::KvEntryMaybe<> entry, rt::KvKey key) {
      // A stale claimant may have spent time reading its routing/auth fences.
      // Recheck here before it can become the new owner of an expired quota
      // window. Submitted/unknown reconciliation remains outside this path.
      const std::optional<double> nowBeforeTakeover = readClock(clock);
      if (!nowBeforeTakeover) co_return ClaimResultFailure{.code = "invalid_clock"};
      if (record.state == CodexResetRedemptionState::claimed && !quotaWindowIsOpen(candidate, *nowBeforeTakeover)) {
        co_return ClaimResultFailure{.code = "quota_window_expired"};
      }
      const std::optional<double> renewedFence = nextFence(record.fence);
      if (!renewedFence) co_return ClaimResultFailure{.code = "owner_fence_exhausted"};
      CodexResetRedemptionRecord takenOver = record;
      takenOver.owner_token = ownerToken;
      takenOver.fence = *renewedFence;
      takenOver.lease_expires_at_ms = expiresAtMs;
      takenOver.updated_at_ms = nowMs;
      bool unavailable = false;
      bool committedOk = false;
      try {
        std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        operation->check(entry);
        withFenceChecks(*operation, fenceEntries).set(key, recordValue(takenOver));
        rt::Promise<rt::KvCommitResult> committing(operation->commit());
        const rt::KvCommitResult committed = co_await committing.wait();
        committedOk = committed.ok;
      } catch (...) {
        unavailable = true;
      }
      if (unavailable) co_return ClaimResultFailure{.code = "kv_unavailable"};
      if (!committedOk) co_return std::nullopt;
      if (record.state == CodexResetRedemptionState::claimed) co_return ClaimResultSubmit{.record = takenOver, .tookOver = true};
      co_return ClaimResultReconcile{.record = takenOver, .tookOver = true};
    }

    /**
     * One compare-and-set attempt. `null` means a concurrent writer changed the
     * record between the read and the write, so the caller must try again.
     */
    rt::Task<std::optional<ClaimResult>> claimAttempt(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexBankedResetCandidate candidate, double nowMs,
                                                      std::function<double()> clock, std::string ownerToken, bool allowNewSubmission, double expiresAtMs,
                                                      rt::KvKey key) {
      rt::KvEntryMaybe<> entry;
      bool unavailable = false;
      try {
        rt::Promise<rt::KvEntryMaybe<>> reading(kv->get(key, strongRead()));
        entry = co_await reading.wait();
      } catch (...) {
        unavailable = true;
      }
      if (unavailable) co_return ClaimResultFailure{.code = "kv_unavailable"};
      const std::optional<CodexResetRedemptionRecord> record =
        entry.value.isNull() ? std::nullopt : banked_reset::parseCodexResetRedemptionRecord(rt::storedJsonView(entry.value, STORED_JSON_VIEW_OPTIONS));
      if (!entry.value.isNull() && !record) co_return ClaimResultFailure{.code = "redemption_record_invalid"};
      if (!record) {
        rt::Promise<std::optional<ClaimResult>> creating(
          createClaimRecord(kv, context, candidate, nowMs, clock, ownerToken, allowNewSubmission, expiresAtMs, entry, key));
        std::optional<ClaimResult> created = co_await creating.wait();
        co_return created;
      }

      std::optional<ClaimResult> disposition = existingRecordDisposition(*record, context, candidate, nowMs, allowNewSubmission);
      if (disposition) co_return disposition;
      rt::Promise<RequiredFences> reading(takeOverFences(kv, candidate, *record));
      RequiredFences fences = co_await reading.wait();
      if (const FenceReadFailure* failure = std::get_if<FenceReadFailure>(&fences)) co_return ClaimResultFailure{.code = failure->code};
      rt::Promise<std::optional<ClaimResult>> committing(
        commitTakeOver(kv, candidate, *record, nowMs, clock, ownerToken, expiresAtMs, std::move(std::get<FenceReadValid>(fences).entries), entry, key));
      std::optional<ClaimResult> committed = co_await committing.wait();
      co_return committed;
    }
  }

  rt::Task<ClaimResult> claimTransaction(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexBankedResetCandidate candidate, double nowMs,
                                         std::function<double()> clock, std::string ownerToken, bool allowNewSubmission) {
    const std::optional<double> expiresAtMs = leaseUntil(nowMs);
    if (!expiresAtMs) co_return ClaimResultFailure{.code = "invalid_clock"};
    const rt::KvKey key = banked_reset::codexResetRedemptionKey(context.account.accountIdHash, context.account.quotaGeneration);

    for (double attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
      rt::Promise<std::optional<ClaimResult>> attempting(claimAttempt(kv, context, candidate, nowMs, clock, ownerToken, allowNewSubmission, *expiresAtMs, key));
      std::optional<ClaimResult> claimed = co_await attempting.wait();
      if (claimed) co_return std::move(*claimed);
    }
    co_return ClaimResultFailure{.code = "kv_cas_exhausted"};
  }

  rt::Task<std::optional<CodexResetRedemptionRecord>> updateOwnedRecord(
    std::shared_ptr<rt::Kv> kv, ResetContext context, CodexResetRedemptionRecord expected,
    std::function<CodexResetRedemptionRecord(const CodexResetRedemptionRecord& record)> mutate) {
    const rt::KvKey key = banked_reset::codexResetRedemptionKey(context.account.accountIdHash, context.account.quotaGeneration);
    for (double attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
      rt::KvEntryMaybe<> entry;
      bool unavailable = false;
      try {
        rt::Promise<rt::KvEntryMaybe<>> reading(kv->get(key, strongRead()));
        entry = co_await reading.wait();
      } catch (...) {
        unavailable = true;
      }
      if (unavailable) co_return std::nullopt;
      const std::optional<CodexResetRedemptionRecord> current = banked_reset::parseCodexResetRedemptionRecord(rt::storedJsonView(entry.value, STORED_JSON_VIEW_OPTIONS));
      if (!current || !matchesContext(*current, context) || current->owner_token != expected.owner_token || current->fence != expected.fence) {
        co_return std::nullopt;
      }
      const CodexResetRedemptionRecord next = mutate(*current);
      bool failed = false;
      bool committedOk = false;
      try {
        std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        operation->check(entry).set(key, recordValue(next));
        rt::Promise<rt::KvCommitResult> committing(operation->commit());
        const rt::KvCommitResult committed = co_await committing.wait();
        committedOk = committed.ok;
      } catch (...) {
        failed = true;
      }
      if (failed) co_return std::nullopt;
      if (committedOk) co_return next;
    }
    co_return std::nullopt;
  }

  namespace {
    struct DailySubmissionBudgetOk {
      [[maybe_unused]] static constexpr std::string_view kind = "ok";
      rt::KvEntryMaybe<> entry;
      double submissionCount;
    };
    struct DailySubmissionBudgetFailure {
      [[maybe_unused]] static constexpr std::string_view kind = "failure";
      std::string code;
    };
    using DailySubmissionBudget = std::variant<DailySubmissionBudgetOk, DailySubmissionBudgetFailure>;

    /**
     * Strong read of the account's UTC-day submission budget. A corrupt daily
     * record fails closed rather than resetting the cap to zero.
     */
    rt::Task<DailySubmissionBudget> readDailySubmissionBudget(std::shared_ptr<rt::Kv> kv, rt::KvKey dailyKey, std::string day, double maxPerAccountPerDay) {
      rt::KvEntryMaybe<> dailyEntry;
      bool unavailable = false;
      try {
        rt::Promise<rt::KvEntryMaybe<>> reading(kv->get(dailyKey, strongRead()));
        dailyEntry = co_await reading.wait();
      } catch (...) {
        unavailable = true;
      }
      if (unavailable) co_return DailySubmissionBudgetFailure{.code = "kv_unavailable"};
      const std::optional<CodexResetGlobalDailyRecord> daily =
        dailyEntry.value.isNull() ? std::nullopt : banked_reset::parseAccountDayRecord(rt::storedJsonView(dailyEntry.value, STORED_JSON_VIEW_OPTIONS), day);
      if (!dailyEntry.value.isNull() && !daily) co_return DailySubmissionBudgetFailure{.code = "account_day_record_invalid"};
      const double submissionCount = daily ? daily->submission_count : 0;
      if (submissionCount >= maxPerAccountPerDay) co_return DailySubmissionBudgetFailure{.code = "account_day_limit_reached"};
      co_return DailySubmissionBudgetOk{.entry = std::move(dailyEntry), .submissionCount = submissionCount};
    }

    /**
     * One compare-and-set attempt of the durable `claimed -> submitted` boundary.
     * `null` means the CAS lost and the caller must try again.
     */
    rt::Task<std::optional<SubmissionPreparation>> prepareSubmissionAttempt(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexBankedResetCandidate candidate,
                                                                            CodexResetRedemptionRecord expected, double nowMs, std::function<double()> clock,
                                                                            double maxPerAccountPerDay, double expiresAtMs, std::string day, rt::KvKey key,
                                                                            rt::KvKey dailyKey) {
      rt::KvEntryMaybe<> entry;
      bool unavailable = false;
      try {
        rt::Promise<rt::KvEntryMaybe<>> reading(kv->get(key, strongRead()));
        entry = co_await reading.wait();
      } catch (...) {
        unavailable = true;
      }
      if (unavailable) co_return SubmissionPreparationFailure{.code = "kv_unavailable"};
      const std::optional<CodexResetRedemptionRecord> current = banked_reset::parseCodexResetRedemptionRecord(rt::storedJsonView(entry.value, STORED_JSON_VIEW_OPTIONS));
      if (!isOwnedRecordInState(current, context, expected, CodexResetRedemptionState::claimed)) co_return SubmissionPreparationFailure{.code = "stale_owner"};
      if (!banked_reset::claimedDuringCurrentUtcDay(*current, nowMs)) co_return SubmissionPreparationFailure{.code = "claim_day_elapsed"};
      if (current->routing_generation != candidate.routingGeneration || current->lease_expires_at_ms <= nowMs) {
        co_return SubmissionPreparationFailure{.code = "stale_owner"};
      }
      rt::Promise<RequiredFences> readingFences(readRequiredFences(kv, candidate));
      RequiredFences fences = co_await readingFences.wait();
      if (const FenceReadFailure* failure = std::get_if<FenceReadFailure>(&fences)) co_return SubmissionPreparationFailure{.code = failure->code};

      rt::Promise<banked_reset::ReadUsageGateResult> gating(banked_reset::readUsageGate(kv, context.account.accountIdHash, candidate.authorization));
      banked_reset::ReadUsageGateResult usage = co_await gating.wait();
      if (const banked_reset::ReadUsageGateResultFailure* failure = std::get_if<banked_reset::ReadUsageGateResultFailure>(&usage)) {
        co_return SubmissionPreparationFailure{.code = failure->code};
      }
      rt::Promise<DailySubmissionBudget> budgeting(readDailySubmissionBudget(kv, dailyKey, day, maxPerAccountPerDay));
      DailySubmissionBudget budget = co_await budgeting.wait();
      if (const DailySubmissionBudgetFailure* failure = std::get_if<DailySubmissionBudgetFailure>(&budget)) {
        co_return SubmissionPreparationFailure{.code = failure->code};
      }
      const DailySubmissionBudgetOk& budgetOk = std::get<DailySubmissionBudgetOk>(budget);
      const double submissionCount = budgetOk.submissionCount;
      // Inventory, fences, and the daily budget are all strong reads. Check
      // again after them so a naturally recovered quota window cannot cross the
      // durable submission boundary or consume the daily budget.
      const std::optional<double> nowBeforeCommit = readClock(clock);
      if (!nowBeforeCommit) co_return SubmissionPreparationFailure{.code = "invalid_clock"};
      if (!quotaWindowIsOpen(candidate, *nowBeforeCommit)) co_return SubmissionPreparationFailure{.code = "quota_window_expired"};
      if (!banked_reset::claimedDuringCurrentUtcDay(*current, *nowBeforeCommit)) co_return SubmissionPreparationFailure{.code = "claim_day_elapsed"};
      StateWithPatch submittedPatch;
      submittedPatch.submitted_at_ms = nowMs;
      submittedPatch.last_error_code = nullptr;
      CodexResetRedemptionRecord submitted = stateWith(*current, CodexResetRedemptionState::submitted, nowMs, submittedPatch);
      submitted.lease_expires_at_ms = expiresAtMs;
      const CodexResetGlobalDailyRecord nextDaily{
        .v = 1,
        .day = day,
        .submission_count = submissionCount + 1,
        .updated_at_ms = nowMs,
      };
      std::vector<rt::KvEntryMaybe<>> checkedEntries = std::get<FenceReadValid>(fences).entries;
      for (const rt::KvEntryMaybe<>& usageEntry : std::get<banked_reset::ReadUsageGateResultAllowed>(usage).entries) checkedEntries.push_back(usageEntry);
      bool failed = false;
      bool committedOk = false;
      try {
        std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        operation->check(entry).check(budgetOk.entry);
        withFenceChecks(*operation, checkedEntries).set(key, recordValue(submitted)).set(dailyKey, rt::KvValue(json::value_from(nextDaily)));
        rt::Promise<rt::KvCommitResult> committing(operation->commit());
        const rt::KvCommitResult committed = co_await committing.wait();
        committedOk = committed.ok;
      } catch (...) {
        failed = true;
      }
      if (failed) co_return SubmissionPreparationFailure{.code = "kv_unavailable"};
      if (committedOk) co_return SubmissionPreparationSubmitted{.record = submitted};
      co_return std::nullopt;
    }
  }

  rt::Task<SubmissionPreparation> prepareSubmission(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexBankedResetCandidate candidate,
                                                    CodexResetRedemptionRecord expected, double nowMs, std::function<double()> clock, double maxPerAccountPerDay) {
    const std::optional<double> expiresAtMs = leaseUntil(nowMs);
    const std::optional<std::string> day = banked_reset::utcDay(nowMs);
    if (!expiresAtMs || !rt::truthy(day)) co_return SubmissionPreparationFailure{.code = "invalid_clock"};
    const rt::KvKey key = banked_reset::codexResetRedemptionKey(context.account.accountIdHash, context.account.quotaGeneration);
    const rt::KvKey dailyKey = banked_reset::codexResetAccountDailyKey(context.account.accountIdHash, *day);
    for (double attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
      rt::Promise<std::optional<SubmissionPreparation>> attempting(
        prepareSubmissionAttempt(kv, context, candidate, expected, nowMs, clock, maxPerAccountPerDay, *expiresAtMs, *day, key, dailyKey));
      std::optional<SubmissionPreparation> prepared = co_await attempting.wait();
      if (prepared) co_return std::move(*prepared);
    }
    co_return SubmissionPreparationFailure{.code = "kv_cas_exhausted"};
  }

  namespace {
    struct RenewalCommitGateReady {
      [[maybe_unused]] static constexpr std::string_view kind = "ready";
      double nowMs;
    };
    struct RenewalCommitGateFailure {
      [[maybe_unused]] static constexpr std::string_view kind = "failure";
      std::string code;
    };
    using RenewalCommitGate = std::variant<RenewalCommitGateReady, RenewalCommitGateFailure>;

    /**
     * Last-moment checks before the renewal CAS: the lease may only be extended
     * while the observed window is still open and inside the claim's UTC day.
     */
    RenewalCommitGate renewalCommitGate(const CodexResetRedemptionRecord& current, const CodexBankedResetCandidate& candidate, const std::function<double()>& clock) {
      // A slow strong read must not commit a new lease after the observed quota
      // window has naturally reopened.
      const std::optional<double> nowMs = readClock(clock);
      if (!nowMs) return RenewalCommitGateFailure{.code = "invalid_clock"};
      if (!quotaWindowIsOpen(candidate, *nowMs)) return RenewalCommitGateFailure{.code = "quota_window_expired"};
      if (!banked_reset::claimedDuringCurrentUtcDay(current, *nowMs)) return RenewalCommitGateFailure{.code = "claim_day_elapsed"};
      if (current.lease_expires_at_ms <= *nowMs) return RenewalCommitGateFailure{.code = "stale_owner"};
      return RenewalCommitGateReady{.nowMs = *nowMs};
    }

    /** The renewed record, or the failure code that stops the lease extension. */
    SubmissionRenewal renewedLeaseRecord(const CodexResetRedemptionRecord& current, double nowMs) {
      const std::optional<double> expiresAtMs = leaseUntil(nowMs);
      const std::optional<double> renewedFence = nextFence(current.fence);
      if (!expiresAtMs) return SubmissionRenewalFailure{.code = "invalid_clock"};
      if (!renewedFence) return SubmissionRenewalFailure{.code = "owner_fence_exhausted"};
      CodexResetRedemptionRecord record = current;
      record.fence = *renewedFence;
      record.lease_expires_at_ms = *expiresAtMs;
      record.updated_at_ms = nowMs;
      return SubmissionRenewalRenewed{.record = std::move(record)};
    }

    /**
     * One compare-and-set attempt of the pre-redeem lease renewal. `null` means the
     * CAS lost and the caller must try again.
     */
    rt::Task<std::optional<SubmissionRenewal>> renewSubmittedAttempt(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexBankedResetCandidate candidate,
                                                                     CodexResetRedemptionRecord expected, std::function<double()> clock, rt::KvKey key) {
      const std::optional<double> nowBeforeRead = readClock(clock);
      if (!nowBeforeRead) co_return SubmissionRenewalFailure{.code = "invalid_clock"};
      if (!quotaWindowIsOpen(candidate, *nowBeforeRead)) co_return SubmissionRenewalFailure{.code = "quota_window_expired"};

      rt::KvEntryMaybe<> entry;
      bool unavailable = false;
      try {
        rt::Promise<rt::KvEntryMaybe<>> reading(kv->get(key, strongRead()));
        entry = co_await reading.wait();
      } catch (...) {
        unavailable = true;
      }
      if (unavailable) co_return SubmissionRenewalFailure{.code = "kv_unavailable"};
      const std::optional<CodexResetRedemptionRecord> current = banked_reset::parseCodexResetRedemptionRecord(rt::storedJsonView(entry.value, STORED_JSON_VIEW_OPTIONS));
      if (!isOwnedRecordInState(current, context, expected, CodexResetRedemptionState::submitted)) co_return SubmissionRenewalFailure{.code = "stale_owner"};
      if (current->routing_generation != candidate.routingGeneration || current->lease_expires_at_ms <= *nowBeforeRead) {
        co_return SubmissionRenewalFailure{.code = "stale_owner"};
      }
      if (!banked_reset::claimedDuringCurrentUtcDay(*current, *nowBeforeRead)) co_return SubmissionRenewalFailure{.code = "claim_day_elapsed"};

      rt::Promise<banked_reset::ReadUsageGateResult> gating(banked_reset::readUsageGate(kv, context.account.accountIdHash, candidate.authorization));
      banked_reset::ReadUsageGateResult usage = co_await gating.wait();
      if (const banked_reset::ReadUsageGateResultFailure* failure = std::get_if<banked_reset::ReadUsageGateResultFailure>(&usage)) {
        co_return SubmissionRenewalFailure{.code = failure->code};
      }

      rt::Promise<RequiredFences> readingFences(readRequiredFences(kv, candidate));
      RequiredFences fences = co_await readingFences.wait();
      if (const FenceReadFailure* failure = std::get_if<FenceReadFailure>(&fences)) co_return SubmissionRenewalFailure{.code = failure->code};

      const RenewalCommitGate gate = renewalCommitGate(*current, candidate, clock);
      if (const RenewalCommitGateFailure* failure = std::get_if<RenewalCommitGateFailure>(&gate)) co_return SubmissionRenewalFailure{.code = failure->code};
      const SubmissionRenewal renewed = renewedLeaseRecord(*current, std::get<RenewalCommitGateReady>(gate).nowMs);
      if (const SubmissionRenewalFailure* failure = std::get_if<SubmissionRenewalFailure>(&renewed)) co_return SubmissionRenewalFailure{.code = failure->code};
      const CodexResetRedemptionRecord& renewedRecord = std::get<SubmissionRenewalRenewed>(renewed).record;
      std::vector<rt::KvEntryMaybe<>> checkedEntries = std::get<FenceReadValid>(fences).entries;
      for (const rt::KvEntryMaybe<>& usageEntry : std::get<banked_reset::ReadUsageGateResultAllowed>(usage).entries) checkedEntries.push_back(usageEntry);
      bool failed = false;
      bool committedOk = false;
      try {
        std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        operation->check(entry);
        withFenceChecks(*operation, checkedEntries).set(key, recordValue(renewedRecord));
        rt::Promise<rt::KvCommitResult> committing(operation->commit());
        const rt::KvCommitResult committed = co_await committing.wait();
        committedOk = committed.ok;
      } catch (...) {
        failed = true;
      }
      if (failed) co_return SubmissionRenewalFailure{.code = "kv_unavailable"};
      if (committedOk) co_return SubmissionRenewalRenewed{.record = renewedRecord};
      co_return std::nullopt;
    }
  }

  rt::Task<SubmissionRenewal> renewSubmittedForRedeem(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexBankedResetCandidate candidate,
                                                      CodexResetRedemptionRecord expected, std::function<double()> clock) {
    const rt::KvKey key = banked_reset::codexResetRedemptionKey(context.account.accountIdHash, context.account.quotaGeneration);
    for (double attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
      rt::Promise<std::optional<SubmissionRenewal>> attempting(renewSubmittedAttempt(kv, context, candidate, expected, clock, key));
      std::optional<SubmissionRenewal> renewed = co_await attempting.wait();
      if (renewed) co_return std::move(*renewed);
    }
    co_return SubmissionRenewalFailure{.code = "kv_cas_exhausted"};
  }

  std::optional<std::string> receiptId(const json::value& value) { return receiptId(&value); }

  std::optional<std::string> receiptId(const json::value* value) {
    if (isNonEmptyText(value, 512)) return std::string(value->get_string());
    return std::nullopt;
  }

  std::optional<std::string> receiptId(const std::optional<std::string>& value) {
    if (value && isNonEmptyText(std::string_view(*value), 512)) return *value;
    return std::nullopt;
  }

  std::optional<std::string> durableReceiptId(const CodexUsageResetProvider* provider, const json::value& value) {
    return banked_reset_provider::providerReceiptIdsSafeToPersistAndLog(provider) ? receiptId(value) : std::nullopt;
  }

  std::optional<std::string> durableReceiptId(const CodexUsageResetProvider* provider, const json::value* value) {
    return banked_reset_provider::providerReceiptIdsSafeToPersistAndLog(provider) ? receiptId(value) : std::nullopt;
  }

  std::optional<std::string> durableReceiptId(const CodexUsageResetProvider* provider, const std::optional<std::string>& value) {
    return banked_reset_provider::providerReceiptIdsSafeToPersistAndLog(provider) ? receiptId(value) : std::nullopt;
  }

  bool validInventory(const ResetInventory& inventory, double nowMs) {
    if (!isSafeNonnegativeInteger(inventory.availableCount) || !isSafeMs(inventory.observedAtMs) || !(inventory.observedAtMs <= nowMs) ||
        !(nowMs - inventory.observedAtMs <= CODEX_BANKED_RESET_INVENTORY_MAX_AGE_MS))
      return false;
    const bool creditsValid = std::all_of(inventory.credits.begin(), inventory.credits.end(), [](const ResetInventoryCredit& credit) {
      return isNonEmptyText(std::string_view(credit.id), 512) && isNonEmptyText(std::string_view(credit.status), 128) &&
             isNonEmptyText(std::string_view(credit.resetType), 128) && (!credit.expiresAtMs || isSafeMs(*credit.expiresAtMs));
    });
    if (!creditsValid) return false;
    // The production adapter rejects duplicate opaque IDs. Retain that same
    // invariant at the evaluator boundary so an injected or future provider
    // cannot make an ambiguous inventory look selectable.
    std::unordered_set<std::string> ids;
    for (const ResetInventoryCredit& credit : inventory.credits) ids.insert(credit.id);
    if (ids.size() != inventory.credits.size()) return false;
    const auto available =
      std::count_if(inventory.credits.begin(), inventory.credits.end(), [](const ResetInventoryCredit& credit) { return credit.status == "available"; });
    return static_cast<double>(available) == inventory.availableCount;
  }

  InventoryCreditSelection selectInventoryCredit(const ResetInventory& inventory, const CodexUsageResetProvider* provider, double nowMs) {
    if (inventory.availableCount == 0) return InventoryCreditSelectionEmpty{};
    std::vector<ResetInventoryCredit> candidates;
    for (const ResetInventoryCredit& credit : inventory.credits) {
      if (credit.status == "available" && credit.resetType == "codex_rate_limits" &&
          banked_reset_provider::providerSupportsResetType(provider, json::value(credit.resetType)) && (!credit.expiresAtMs || *credit.expiresAtMs > nowMs)) {
        candidates.push_back(credit);
      }
    }
    if (candidates.empty()) return InventoryCreditSelectionNoEligibleCredit{};
    rt::sort(candidates, [](const ResetInventoryCredit& left, const ResetInventoryCredit& right) {
      const double leftExpiry = left.expiresAtMs.value_or(rt::Number::POSITIVE_INFINITY);
      const double rightExpiry = right.expiresAtMs.value_or(rt::Number::POSITIVE_INFINITY);
      const double difference = leftExpiry - rightExpiry;
      if (rt::truthy(difference)) return difference;
      return static_cast<double>(rt::localeCompare(left.id, right.id));
    });
    const ResetInventoryCredit* best = candidates.empty() ? nullptr : &candidates.front();
    if (!best) return InventoryCreditSelectionNoEligibleCredit{};
    return InventoryCreditSelectionSelected{.credit = *best};
  }

  bool validRedeemResult(const RedeemResetResult& value) {
    if (const auto* completed = std::get_if<banked_reset_provider::RedeemResetResultCompleted>(&value)) {
      return receiptId(std::optional<std::string>(completed->providerReceiptId)).has_value();
    }
    if (const auto* accepted = std::get_if<banked_reset_provider::RedeemResetResultAccepted>(&value)) {
      return receiptId(std::optional<std::string>(accepted->providerReceiptId)).has_value();
    }
    if (const auto* alreadyRedeemed = std::get_if<banked_reset_provider::RedeemResetResultAlreadyRedeemed>(&value)) {
      return receiptId(std::optional<std::string>(alreadyRedeemed->providerReceiptId)).has_value();
    }
    // `typeof value.reason === "string"`: the typed result always carries one.
    if (std::holds_alternative<banked_reset_provider::RedeemResetResultRejected>(value)) return true;
    if (const auto* unknown = std::get_if<banked_reset_provider::RedeemResetResultUnknown>(&value)) {
      return !unknown->providerReceiptId || receiptId(unknown->providerReceiptId).has_value();
    }
    return false;
  }

  CodexResetRedemptionRecord stateWith(const CodexResetRedemptionRecord& record, CodexResetRedemptionState state, double nowMs) {
    return stateWith(record, state, nowMs, StateWithPatch{});
  }

  CodexResetRedemptionRecord stateWith(const CodexResetRedemptionRecord& record, CodexResetRedemptionState state, double nowMs, const StateWithPatch& patch) {
    CodexResetRedemptionRecord next = record;
    if (!patch.provider_receipt_id.isUndefined()) next.provider_receipt_id = patch.provider_receipt_id.toOptional();
    if (!patch.submitted_at_ms.isUndefined()) next.submitted_at_ms = patch.submitted_at_ms.toOptional();
    if (!patch.verified_at_ms.isUndefined()) next.verified_at_ms = patch.verified_at_ms.toOptional();
    if (!patch.last_error_code.isUndefined()) next.last_error_code = patch.last_error_code.toOptional();
    next.state = state;
    next.updated_at_ms = nowMs;
    return next;
  }

  rt::Task<std::optional<CodexResetRedemptionRecord>> rejectOwned(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexResetRedemptionRecord record,
                                                                  double nowMs, std::string code) {
    std::function<CodexResetRedemptionRecord(const CodexResetRedemptionRecord&)> mutate = [nowMs, code](const CodexResetRedemptionRecord& current) {
      StateWithPatch patch;
      patch.last_error_code = code;
      return stateWith(current, CodexResetRedemptionState::rejected, nowMs, patch);
    };
    rt::Promise<std::optional<CodexResetRedemptionRecord>> updating(updateOwnedRecord(kv, context, record, mutate));
    std::optional<CodexResetRedemptionRecord> updated = co_await updating.wait();
    co_return updated;
  }

  rt::Task<std::optional<CodexResetRedemptionRecord>> unknownOwned(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexResetRedemptionRecord record,
                                                                   double nowMs, std::string code, std::optional<std::string> providerReceiptId) {
    std::function<CodexResetRedemptionRecord(const CodexResetRedemptionRecord&)> mutate = [nowMs, code,
                                                                                            providerReceiptId](const CodexResetRedemptionRecord& current) {
      StateWithPatch patch;
      patch.provider_receipt_id = rt::orNull(providerReceiptId ? providerReceiptId : current.provider_receipt_id);
      patch.last_error_code = code;
      return stateWith(current, CodexResetRedemptionState::unknown, nowMs, patch);
    };
    rt::Promise<std::optional<CodexResetRedemptionRecord>> updating(updateOwnedRecord(kv, context, record, mutate));
    std::optional<CodexResetRedemptionRecord> updated = co_await updating.wait();
    co_return updated;
  }

  rt::Task<std::optional<CodexResetRedemptionRecord>> preserveReceipt(std::shared_ptr<rt::Kv> kv, ResetContext context, CodexResetRedemptionRecord record,
                                                                      double nowMs, std::optional<std::string> providerReceiptId) {
    std::function<CodexResetRedemptionRecord(const CodexResetRedemptionRecord&)> mutate = [nowMs, providerReceiptId](const CodexResetRedemptionRecord& current) {
      StateWithPatch patch;
      patch.provider_receipt_id = rt::orNull(providerReceiptId);
      patch.submitted_at_ms = current.submitted_at_ms ? *current.submitted_at_ms : nowMs;
      patch.last_error_code = nullptr;
      return stateWith(current, CodexResetRedemptionState::submitted, nowMs, patch);
    };
    rt::Promise<std::optional<CodexResetRedemptionRecord>> updating(updateOwnedRecord(kv, context, record, mutate));
    std::optional<CodexResetRedemptionRecord> updated = co_await updating.wait();
    co_return updated;
  }

  CodexBankedResetOutcome unknownOutcome(const CodexBankedResetTelemetry& telemetry, const ResetContext& context, const CodexBankedResetCandidate& candidate,
                                         const std::string& reason, const CodexResetRedemptionRecord& record) {
    json::object extras;
    extras["state"] = json::string(::uos::types::toString(record.state));
    extras["reason"] = reason;
    banked_reset::emit(telemetry, "codex_reset_unknown", banked_reset::telemetryFields(context, candidate, extras));
    banked_reset::metric(telemetry, "codex_reset_unknown_total", 1, banked_reset::telemetryFields(context, candidate));
    return banked_reset::outcome("pending", reason, context, record);
  }

  std::optional<std::string> liveSubmissionPolicyReason(const CodexBankedResetDependencies& dependencies, bool manual) {
    return loadLiveSubmissionConfig(dependencies, manual).reason;
  }

  LoadLiveSubmissionConfigResult loadLiveSubmissionConfig(const CodexBankedResetDependencies& dependencies, bool manual) {
    std::optional<CodexBankedResetConfig> loaded;
    try {
      loaded = dependencies.reloadConfig ? dependencies.reloadConfig() : dependencies.config;
    } catch (...) {
      return LoadLiveSubmissionConfigResult{.config = std::nullopt, .reason = "configuration_unavailable"};
    }
    const CodexBankedResetConfig& config = *loaded;
    std::optional<std::string> reason = banked_reset::policyReason(config, manual);
    if (!reason) reason = banked_reset::providerPolicyReason(config, dependencies.provider.get(), manual);
    if (rt::truthy(reason) || (!manual && config.mode != "live")) {
      return LoadLiveSubmissionConfigResult{.config = std::nullopt, .reason = reason ? *reason : std::string("mode_not_live")};
    }
    return LoadLiveSubmissionConfigResult{.config = config, .reason = std::nullopt};
  }
}
