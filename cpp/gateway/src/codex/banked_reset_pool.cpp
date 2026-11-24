// Port of src/codex/banked-reset-pool.ts.
// Codex banked reset pool evaluation and shadow decisions, split out of src/codex_banked_reset.ts.
#include "uos/codex/banked_reset_pool.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/codex/banked_reset_claim.hpp"
#include "uos/codex/banked_reset_provider.hpp"
#include "uos/codex/banked_reset_submission.hpp"
#include "uos/codex/reset_settings.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <initializer_list>
#include <span>
#include <utility>
#include <variant>

namespace uos::codex::banked_reset_pool {
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
    using ::uos::types::CodexResetShadowDecisionRecord;
    using banked_reset::CodexBankedResetConfig;
    using banked_reset::CodexBankedResetDependencies;
    using banked_reset::CodexBankedResetOutcome;
    using banked_reset::CodexBankedResetPoolCandidate;
    using banked_reset::CodexBankedResetPoolOutcome;
    using banked_reset::CodexBankedResetTelemetry;
    using banked_reset::CodexBankedResetTelemetryFields;
    using banked_reset::emit;
    using banked_reset::MAX_CAS_ATTEMPTS;
    using banked_reset::metric;
    using banked_reset::ResetContext;
    using banked_reset::telemetryFields;
    using banked_reset_claim::readClock;
    using banked_reset_provider::ResetInventory;
    using banked_reset_provider::ResetInventoryCredit;
    using ShadowDecisionFences = std::vector<CodexResetShadowDecisionRecord::Fence>;
    using HashFunction = std::function<rt::Task<std::string>(std::string value)>;

    rt::KvGetOptions strongRead() { return rt::KvGetOptions{.consistency = rt::KvConsistencyLevel::strong}; }

    // A stored row as the TS reads it (`unknown`): JSON-shaped parts as they are, an undefined member
    // absent and an undefined element or hole null; any other value (a Date, Map, Uint8Array, KvU64,
    // bigint, ...) a member-less object. parseCodexResetShadowDecisionRecord rejects that object exactly
    // where it rejects the original value (it accepts only strings, numbers, null, arrays and records).

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

    struct ResolvedPoolCandidate {
      CodexBankedResetPoolCandidate pool;
      ResetContext context;
    };

    struct SelectedPoolCredit {
      ResolvedPoolCandidate resolved;
      ResetInventoryCredit credit;
      std::string creditIdHash;
    };

    CodexBankedResetPoolOutcome poolOutcome(const std::string& kind, const std::string& reason,
                                            const std::optional<CodexBankedResetPoolCandidate>& selected = std::nullopt,
                                            const std::optional<CodexBankedResetOutcome>& reset = std::nullopt) {
      return CodexBankedResetPoolOutcome{.kind = kind, .reason = reason, .selected = selected, .reset = reset};
    }

    bool sameShadowFences(const ShadowDecisionFences& left, const ShadowDecisionFences& right) {
      if (left.size() != right.size()) return false;
      for (std::size_t index = 0; index < left.size(); index += 1) {
        // The lengths are equal, so this index is always inside `right`.
        const CodexResetShadowDecisionRecord::Fence& fence = left[index];
        const CodexResetShadowDecisionRecord::Fence& other = right[index];
        if (!(fence.slot == other.slot && fence.account_id_hash == other.account_id_hash && fence.quota_generation == other.quota_generation &&
              fence.routing_generation == other.routing_generation && fence.quota_reset_at_ms == other.quota_reset_at_ms))
          return false;
      }
      return true;
    }

    banked_reset_claim::LoadLiveSubmissionConfigResult loadCurrentPoolConfig(const CodexBankedResetDependencies& dependencies) {
      try {
        const CodexBankedResetConfig config = dependencies.reloadConfig ? dependencies.reloadConfig() : dependencies.config;
        if (!config.enabled) return {.config = std::nullopt, .reason = "feature_disabled"};
        if (config.mode == "disabled") return {.config = std::nullopt, .reason = "mode_disabled"};
        if (config.maxPerAccountPerDay <= 0) return {.config = std::nullopt, .reason = "per_account_day_limit_invalid"};
        if (config.maxPerAccountPerWindow != 1) return {.config = std::nullopt, .reason = "per_account_window_limit_invalid"};
        return {.config = config, .reason = std::nullopt};
      } catch (...) {
        return {.config = std::nullopt, .reason = "configuration_unavailable"};
      }
    }

    // `Readonly<{ kind: "written" | "duplicate"; record: CodexResetShadowDecisionRecord }>`
    struct ShadowDecisionWrite {
      std::string kind;
      CodexResetShadowDecisionRecord record;
    };

    /**
     * An existing decision for this exact episode is a duplicate; anything else --
     * an unparseable record, a different episode, or an expired decision -- is
     * unusable and fails closed.
     */
    std::optional<ShadowDecisionWrite> duplicateShadowDecision(const json::value& value, const CodexResetShadowDecisionRecord& record, double nowMs) {
      const std::optional<CodexResetShadowDecisionRecord> existing = banked_reset::parseCodexResetShadowDecisionRecord(value);
      if (!existing) return std::nullopt;
      if (existing->episode_hash != record.episode_hash || existing->expires_at_ms <= nowMs) return std::nullopt;
      return ShadowDecisionWrite{.kind = "duplicate", .record = *existing};
    }

    rt::Task<std::optional<ShadowDecisionWrite>> shadowDecisionRecord(std::shared_ptr<rt::Kv> kv, CodexResetShadowDecisionRecord record, double nowMs,
                                                                      std::vector<rt::KvEntryMaybe<>> settingsEntries) {
      const rt::KvKey key = banked_reset::codexResetShadowDecisionKey(record.episode_hash);
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
        if (!entry.value.isNull()) co_return duplicateShadowDecision(rt::storedJsonView(entry.value, STORED_JSON_VIEW_OPTIONS), record, nowMs);
        bool failed = false;
        bool disallowed = false;
        bool committedOk = false;
        try {
          reset_settings::ReadCodexResetUsageResult usage{.allowed = true, .entries = {}};
          if (rt::truthy(record.selected_account_id_hash)) {
            rt::Promise<reset_settings::ReadCodexResetUsageResult> reading(reset_settings::readCodexResetUsage(kv, *record.selected_account_id_hash));
            usage = co_await reading.wait();
          }
          if (!usage.allowed) {
            disallowed = true;
          } else {
            std::vector<rt::KvEntryMaybe<>> checkedEntries = usage.entries;
            for (const rt::KvEntryMaybe<>& settingsEntry : settingsEntries) checkedEntries.push_back(settingsEntry);
            std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
            operation->check(entry);
            banked_reset_claim::withFenceChecks(*operation, checkedEntries).set(key, rt::KvValue(json::value_from(record)));
            rt::Promise<rt::KvCommitResult> committing(operation->commit());
            const rt::KvCommitResult committed = co_await committing.wait();
            committedOk = committed.ok;
          }
        } catch (...) {
          failed = true;
        }
        if (failed || disallowed) co_return std::nullopt;
        if (committedOk) co_return ShadowDecisionWrite{.kind = "written", .record = record};
      }
      co_return std::nullopt;
    }

    rt::Task<std::optional<CodexResetShadowDecisionRecord>> readShadowDecision(std::shared_ptr<rt::Kv> kv, std::string episodeHash) {
      std::optional<CodexResetShadowDecisionRecord> decision;
      try {
        rt::Promise<rt::KvEntryMaybe<>> reading(kv->get(banked_reset::codexResetShadowDecisionKey(episodeHash), strongRead()));
        const rt::KvEntryMaybe<> entry = co_await reading.wait();
        if (!entry.value.isNull()) decision = banked_reset::parseCodexResetShadowDecisionRecord(rt::storedJsonView(entry.value, STORED_JSON_VIEW_OPTIONS));
      } catch (...) {
        decision = std::nullopt;
      }
      co_return decision;
    }
  }

  rt::Task<std::optional<std::vector<CodexResetShadowDecisionRecord>>> listCodexResetShadowDecisions() { return listCodexResetShadowDecisions(std::nullopt); }

  rt::Task<std::optional<std::vector<CodexResetShadowDecisionRecord>>> listCodexResetShadowDecisions(std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    std::shared_ptr<rt::Kv> kv;
    bool unavailable = false;
    try {
      if (!kvOverride) {
        rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
        kv = co_await opening.wait();
      } else {
        kv = *kvOverride;
      }
    } catch (...) {
      unavailable = true;
    }
    if (unavailable) co_return std::nullopt;
    if (!kv) co_return std::nullopt;
    std::vector<CodexResetShadowDecisionRecord> decisions;
    bool unreadable = false;
    try {
      rt::KvListSelector selector;
      selector.prefix = banked_reset::CODEX_RESET_SHADOW_DECISION_KV_PREFIX;
      rt::KvListOptions options;
      options.limit = 100;
      std::unique_ptr<rt::KvListIterator> iterator = kv->list(std::move(selector), std::move(options));
      for (;;) {
        rt::Promise<std::optional<rt::KvEntry<>>> stepping(iterator->next());
        const std::optional<rt::KvEntry<>> entry = co_await stepping.wait();
        if (!entry) break;
        std::optional<CodexResetShadowDecisionRecord> parsed = banked_reset::parseCodexResetShadowDecisionRecord(rt::storedJsonView(entry->value, STORED_JSON_VIEW_OPTIONS));
        if (parsed) decisions.push_back(std::move(*parsed));
      }
    } catch (...) {
      unreadable = true;
    }
    if (unreadable) co_return std::nullopt;
    rt::sort(decisions, [](const CodexResetShadowDecisionRecord& left, const CodexResetShadowDecisionRecord& right) {
      const double newerFirst = right.created_at_ms - left.created_at_ms;
      if (rt::truthy(newerFirst)) return newerFirst;
      return static_cast<double>(rt::localeCompare(left.episode_hash, right.episode_hash));
    });
    co_return decisions;
  }

  namespace {
    // `Readonly<{ kind: "ordered"; ordered } > | Readonly<{ kind: "failure"; reason: string }>` and the other
    // `{ kind: "failure"; reason }` / `{ kind: "outcome"; outcome }` unions of the evaluator.
    struct PoolFailure {
      [[maybe_unused]] static constexpr std::string_view kind = "failure";
      std::string reason;
    };
    struct PoolStepOutcome {
      [[maybe_unused]] static constexpr std::string_view kind = "outcome";
      CodexBankedResetPoolOutcome outcome;
    };
    struct OrderedPool {
      [[maybe_unused]] static constexpr std::string_view kind = "ordered";
      std::vector<CodexBankedResetPoolCandidate> ordered;
    };
    struct PoolConfig {
      [[maybe_unused]] static constexpr std::string_view kind = "config";
      CodexBankedResetConfig config;
    };

    bool poolSlotsAreValid(const std::vector<CodexBankedResetPoolCandidate>& ordered);

    /** Pool candidates in ascending slot order, or the reason the pool is malformed. */
    std::variant<OrderedPool, PoolFailure> orderPoolCandidates(const std::vector<CodexBankedResetPoolCandidate>& candidates) {
      if (candidates.empty()) return PoolFailure{.reason = "full_pool_missing"};
      std::vector<CodexBankedResetPoolCandidate> ordered = rt::toSorted(
        candidates, [](const CodexBankedResetPoolCandidate& left, const CodexBankedResetPoolCandidate& right) { return left.slot - right.slot; });
      if (!poolSlotsAreValid(ordered)) return PoolFailure{.reason = "full_pool_invalid"};
      return OrderedPool{.ordered = std::move(ordered)};
    }

    /** Slots must be safe non-negative integers and unique after the sort. */
    bool poolSlotsAreValid(const std::vector<CodexBankedResetPoolCandidate>& ordered) {
      std::optional<double> previousSlot;
      for (const CodexBankedResetPoolCandidate& candidate : ordered) {
        if (!banked_reset::isSafeNonnegativeInteger(candidate.slot)) return false;
        if (previousSlot && *previousSlot == candidate.slot) return false;
        previousSlot = candidate.slot;
      }
      return true;
    }

    bool anyPoolProviderTreatsRedeemOutcomeAsFinal(const std::vector<CodexBankedResetPoolCandidate>& ordered) {
      return std::any_of(ordered.begin(), ordered.end(), [](const CodexBankedResetPoolCandidate& candidate) {
        return banked_reset_provider::providerTreatsRedeemOutcomeAsFinal(candidate.provider.get());
      });
    }

    /** Pool policy gate: the usable configuration, or the reason the pool stops. */
    std::variant<PoolConfig, PoolFailure> loadPoolSubmissionConfig(const std::vector<CodexBankedResetPoolCandidate>& ordered,
                                                                   const CodexBankedResetDependencies& dependencies) {
      const banked_reset_claim::LoadLiveSubmissionConfigResult loadedConfig = loadCurrentPoolConfig(dependencies);
      if (!loadedConfig.config) return PoolFailure{.reason = *loadedConfig.reason};
      const CodexBankedResetConfig& config = *loadedConfig.config;
      if (config.mode == "live" && config.maxPerAccountPerDay != 1) {
        return PoolFailure{
          .reason = anyPoolProviderTreatsRedeemOutcomeAsFinal(ordered) ? "terminal_outcome_account_day_limit_must_be_one" : "per_account_day_limit_invalid",
        };
      }
      return PoolConfig{.config = config};
    }

    // One `async (pool) => ...` of resolvePoolCandidates.
    rt::Task<std::optional<ResolvedPoolCandidate>> resolvePoolCandidate(CodexBankedResetPoolCandidate pool, HashFunction hash) {
      rt::Promise<std::optional<ResetContext>> contextualizing(banked_reset_claim::makeResetContext(pool.candidate, hash));
      const std::optional<ResetContext> context = co_await contextualizing.wait();
      if (!context) co_return std::nullopt;
      co_return ResolvedPoolCandidate{.pool = std::move(pool), .context = *context};
    }

    /** Every candidate must produce a valid reset context, or the pool is unusable. */
    rt::Task<std::optional<std::vector<ResolvedPoolCandidate>>> resolvePoolCandidates(std::vector<CodexBankedResetPoolCandidate> ordered, HashFunction hash) {
      std::vector<rt::Promise<std::optional<ResolvedPoolCandidate>>> resolving;
      for (const CodexBankedResetPoolCandidate& pool : ordered) resolving.push_back(rt::Promise<std::optional<ResolvedPoolCandidate>>(resolvePoolCandidate(pool, hash)));
      auto all = rt::all(std::move(resolving));
      std::vector<std::optional<ResolvedPoolCandidate>> resolved = co_await all.wait();
      if (std::any_of(resolved.begin(), resolved.end(), [](const auto& candidate) { return !candidate; })) co_return std::nullopt;
      std::vector<ResolvedPoolCandidate> complete;
      for (std::optional<ResolvedPoolCandidate>& candidate : resolved) complete.push_back(std::move(*candidate));
      co_return complete;
    }

    /** The redacted fence evidence that identifies one episode. */
    ShadowDecisionFences poolFences(const std::vector<ResolvedPoolCandidate>& complete) {
      ShadowDecisionFences fences;
      for (const ResolvedPoolCandidate& resolved : complete) {
        fences.push_back(CodexResetShadowDecisionRecord::Fence{
          .slot = resolved.pool.slot,
          .account_id_hash = resolved.context.account.accountIdHash,
          .quota_generation = resolved.context.account.quotaGeneration,
          .routing_generation = resolved.pool.candidate.routingGeneration,
          .quota_reset_at_ms = resolved.pool.candidate.quotaResetAtMs,
        });
      }
      return fences;
    }

    /** Episode hash, or null when the hash cannot be used as an audit key. */
    rt::Task<std::optional<std::string>> poolEpisodeHash(std::string settingsVersion, ShadowDecisionFences fences, HashFunction hash) {
      std::vector<std::string> fenceTexts;
      for (const CodexResetShadowDecisionRecord::Fence& fence : fences) {
        fenceTexts.push_back(joinWithNul({rt::numberToString(fence.slot), fence.account_id_hash, fence.quota_generation,
                                          rt::numberToString(fence.routing_generation), rt::numberToString(fence.quota_reset_at_ms)}));
      }
      const std::string episodeInput = joinWithNul({"uos_ai", "codex_reset_shadow_episode", settingsVersion, rt::join(fenceTexts, std::string(1, '\x01'))});
      rt::Promise<std::string> hashing(hash(episodeInput));
      const std::string episodeHash = co_await hashing.wait();
      if (banked_reset::isNonEmptyText(std::string_view(episodeHash))) co_return episodeHash;
      co_return std::nullopt;
    }

    /**
     * Re-check every fence. A selection based on any stale account state is not
     * audit evidence and can never become a live spend.
     */
    rt::Task<std::optional<std::string>> stalePoolFenceReason(std::shared_ptr<rt::Kv> kv, std::vector<ResolvedPoolCandidate> complete) {
      for (const ResolvedPoolCandidate& resolved : complete) {
        rt::Promise<banked_reset_claim::FenceRead> reading(banked_reset_claim::readCurrentFences(kv, resolved.pool.candidate));
        const banked_reset_claim::FenceRead current = co_await reading.wait();
        if (!std::holds_alternative<banked_reset_claim::FenceReadValid>(current)) {
          if (std::holds_alternative<banked_reset_claim::FenceReadStale>(current)) co_return std::string("routing_fence_stale");
          co_return std::get<banked_reset_claim::FenceReadFailure>(current).code;
        }
      }
      co_return std::nullopt;
    }

    struct PoolEpisode {
      [[maybe_unused]] static constexpr std::string_view kind = "episode";
      std::vector<ResolvedPoolCandidate> complete;
      std::vector<ResolvedPoolCandidate> eligible;
      std::vector<rt::KvEntryMaybe<>> settingsEntries;
      ShadowDecisionFences fences;
      std::string episodeHash;
    };

    struct LivePoolWindow {
      double slot;
      std::optional<std::string> unavailableReason;
      banked_reset::CodexBankedResetFence fence;
    };

    using LivePoolWindows = std::variant<std::vector<LivePoolWindow>, PoolStepOutcome>;

    // Keep every blocked account's window in the submission CAS, including
    // accounts that cannot be selected. Concurrent callers can see different
    // inventories, so ranking alone cannot serialize claims on different slots.
    rt::Task<LivePoolWindows> readLivePoolWindows(std::shared_ptr<rt::Kv> kv, std::vector<ResolvedPoolCandidate> complete,
                                                  CodexBankedResetDependencies dependencies, double nowMs) {
      std::vector<LivePoolWindow> windows;
      try {
        for (const auto& candidate : complete) {
          const auto& account = candidate.context.account;
          auto key = banked_reset::codexResetRedemptionKey(account.accountIdHash, account.quotaGeneration);
          const auto entry = co_await kv->get(key, strongRead());
          const auto snapshot = entry.value.toJson();
          std::optional<std::string> unavailableReason;
          if (!entry.value.isNull()) {
            const auto record = banked_reset::parseCodexResetRedemptionRecord(snapshot);
            if (!record) co_return PoolStepOutcome{.outcome = poolOutcome("skipped", "redemption_record_invalid")};
            if (!banked_reset_claim::matchesContext(*record, candidate.context)) {
              co_return PoolStepOutcome{.outcome = poolOutcome("skipped", "redemption_record_context_mismatch")};
            }
            if (record->state == ::uos::types::CodexResetRedemptionState::claimed && record->lease_expires_at_ms <= nowMs) {
              unavailableReason = "unsubmitted_transaction";
            } else if (banked_reset_claim::isUnsubmittedInventoryRejection(*record)) {
              unavailableReason = record->last_error_code;
            }
            if (!unavailableReason) {
              if (record->state != ::uos::types::CodexResetRedemptionState::verified) {
                co_return PoolStepOutcome{.outcome = poolOutcome("pending", "transaction_in_progress")};
              }
              dependencies.provider = candidate.pool.provider;
              rt::Promise<CodexBankedResetOutcome> reconciling(
                banked_reset_submission::reconcileCodexBankedReset(candidate.pool.candidate, std::move(dependencies)));
              const auto reset = co_await reconciling.wait();
              if (reset.kind != "verified") {
                co_return PoolStepOutcome{.outcome = poolOutcome("skipped", "routing_fence_stale")};
              }
              co_return PoolStepOutcome{.outcome = poolOutcome(reset.kind, reset.reason, candidate.pool, reset)};
            }
          }
          windows.push_back({candidate.pool.slot, std::move(unavailableReason),
                             {std::move(key), [snapshot](const json::value& current) { return rt::deepEqual(current, snapshot); }}});
        }
      } catch (...) {
        co_return PoolStepOutcome{.outcome = poolOutcome("skipped", "kv_unavailable")};
      }
      co_return windows;
    }

    rt::Task<std::optional<PoolStepOutcome>> filterLivePoolEligibility(std::shared_ptr<rt::Kv> kv, PoolEpisode& episode,
                                                                      const std::vector<LivePoolWindow>& windows, double nowMs) {
      const auto day = banked_reset::utcDay(nowMs);
      if (!day) co_return PoolStepOutcome{.outcome = poolOutcome("skipped", "invalid_clock")};
      std::vector<ResolvedPoolCandidate> eligible;
      std::optional<CodexBankedResetPoolCandidate> firstCapped;
      std::optional<std::string> firstRefusal;
      try {
        for (const auto& candidate : episode.eligible) {
          const auto window = std::find_if(windows.begin(), windows.end(), [&](const auto& item) { return item.slot == candidate.pool.slot; });
          if (window == windows.end()) co_return PoolStepOutcome{.outcome = poolOutcome("skipped", "full_pool_missing")};
          if (window->unavailableReason) {
            if (!firstRefusal) firstRefusal = window->unavailableReason;
            continue;
          }
          const auto key = banked_reset::codexResetAccountDailyKey(candidate.context.account.accountIdHash, *day);
          const auto entry = co_await kv->get(key, strongRead());
          if (!entry.value.isNull()) {
            const auto budget = banked_reset::parseAccountDayRecord(rt::storedJsonView(entry.value, STORED_JSON_VIEW_OPTIONS), *day);
            if (!budget) co_return PoolStepOutcome{.outcome = poolOutcome("skipped", "account_day_record_invalid")};
            if (budget->submission_count >= 1) {
              if (!firstCapped) firstCapped = candidate.pool;
              continue;
            }
          }
          eligible.push_back(candidate);
        }
      } catch (...) {
        co_return PoolStepOutcome{.outcome = poolOutcome("skipped", "kv_unavailable")};
      }
      if (eligible.empty()) {
        if (firstCapped) co_return PoolStepOutcome{.outcome = poolOutcome("skipped", "account_day_limit_reached", firstCapped)};
        const std::string reason = firstRefusal.value_or("unsubmitted_transaction");
        co_return PoolStepOutcome{.outcome = poolOutcome(reason == "unsubmitted_transaction" ? "pending" : "skipped", reason)};
      }
      episode.eligible = std::move(eligible);
      co_return std::nullopt;
    }

    rt::Task<bool> livePoolWindowsCurrent(std::shared_ptr<rt::Kv> kv, const std::vector<LivePoolWindow>& windows) {
      try {
        for (const auto& window : windows) {
          const auto entry = co_await kv->get(window.fence.key, strongRead());
          if (!window.fence.isCurrent(entry.value.toJson())) co_return false;
        }
      } catch (...) {
        co_return false;
      }
      co_return true;
    }

    // One `{ entry, usage }` of preparePoolEpisode's settings reads.
    struct PoolSettings {
      ResolvedPoolCandidate entry;
      reset_settings::ReadCodexResetUsageResult usage;
    };

    // One `async (entry) => ({ entry, usage: await readBankedResetUsage(...) })`.
    rt::Task<PoolSettings> readPoolSettings(std::shared_ptr<rt::Kv> kv, ResolvedPoolCandidate entry) {
      rt::Promise<reset_settings::ReadCodexResetUsageResult> reading(reset_settings::readCodexResetUsage(kv, entry.context.account.accountIdHash));
      reset_settings::ReadCodexResetUsageResult usage = co_await reading.wait();
      co_return PoolSettings{.entry = std::move(entry), .usage = std::move(usage)};
    }

    /** The episode evidence: proven contexts, fences, and episode hash. */
    rt::Task<std::variant<PoolEpisode, PoolStepOutcome>> preparePoolEpisode(std::shared_ptr<rt::Kv> kv, std::vector<CodexBankedResetPoolCandidate> ordered,
                                                                            HashFunction hash, double nowMs) {
      rt::Promise<std::optional<std::vector<ResolvedPoolCandidate>>> resolving(resolvePoolCandidates(ordered, hash));
      std::optional<std::vector<ResolvedPoolCandidate>> resolved = co_await resolving.wait();
      if (!resolved) co_return PoolStepOutcome{.outcome = poolOutcome("skipped", "invalid_quota_generation")};
      std::vector<ResolvedPoolCandidate> complete = std::move(*resolved);
      if (std::any_of(complete.begin(), complete.end(),
                      [nowMs](const ResolvedPoolCandidate& entry) { return !banked_reset_claim::quotaWindowIsOpen(entry.pool.candidate, nowMs); })) {
        co_return PoolStepOutcome{.outcome = poolOutcome("skipped", "quota_window_expired")};
      }
      // Reset settings are owned by the subscription: a disabled account is removed
      // from this episode's candidate list, while an unreadable record fails the
      // whole episode closed instead of silently spending.
      std::vector<PoolSettings> settings;
      bool unavailable = false;
      try {
        std::vector<rt::Promise<PoolSettings>> reading;
        for (const ResolvedPoolCandidate& entry : complete) reading.push_back(rt::Promise<PoolSettings>(readPoolSettings(kv, entry)));
        auto all = rt::all(std::move(reading));
        settings = co_await all.wait();
      } catch (...) {
        unavailable = true;
      }
      if (unavailable) co_return PoolStepOutcome{.outcome = poolOutcome("skipped", "configuration_unavailable")};
      std::vector<ResolvedPoolCandidate> eligible;
      for (const PoolSettings& setting : settings) {
        if (setting.usage.allowed) eligible.push_back(setting.entry);
      }
      if (eligible.empty()) {
        auto outcome = poolOutcome("skipped", "usage_disabled");
        const bool explicitlyDisabled = std::all_of(settings.begin(), settings.end(), [](const PoolSettings& setting) {
          if (setting.usage.entries.size() != 1 || !setting.usage.entries.front().value.isObject()) return false;
          const auto& enabled = setting.usage.entries.front().value["enabled"];
          return enabled.isBool() && !enabled.asBool();
        });
        if (explicitlyDisabled) {
          rt::Promise<std::optional<std::string>> checking(stalePoolFenceReason(kv, complete));
          outcome.allCandidatesExplicitlyDisabled = !(co_await checking.wait());
        }
        co_return PoolStepOutcome{.outcome = std::move(outcome)};
      }
      std::vector<rt::KvEntryMaybe<>> settingsEntries;
      for (const PoolSettings& setting : settings) {
        for (const rt::KvEntryMaybe<>& entry : setting.usage.entries) settingsEntries.push_back(entry);
      }
      std::vector<std::string> versions;
      for (const rt::KvEntryMaybe<>& entry : settingsEntries) versions.push_back(entry.versionstamp.value_or("unset"));
      const std::string settingsVersion = rt::join(versions, ",");
      ShadowDecisionFences fences = poolFences(complete);
      rt::Promise<std::optional<std::string>> hashing(poolEpisodeHash(settingsVersion, fences, hash));
      const std::optional<std::string> episodeHash = co_await hashing.wait();
      if (!rt::truthy(episodeHash)) co_return PoolStepOutcome{.outcome = poolOutcome("skipped", "episode_hash_unavailable")};
      co_return PoolEpisode{
        .complete = std::move(complete),
        .eligible = std::move(eligible),
        .settingsEntries = std::move(settingsEntries),
        .fences = std::move(fences),
        .episodeHash = *episodeHash,
      };
    }

    /** The pool was already rejected as `full_pool_missing`, so it is never empty. */
    const ResolvedPoolCandidate& firstResolvedPoolCandidate(const std::vector<ResolvedPoolCandidate>& complete) {
      if (complete.empty()) throw rt::JsError("codex banked reset pool is empty");
      return complete.front();
    }

    /** The already-persisted shadow decision for this episode, when it still holds. */
    rt::Task<std::optional<CodexBankedResetPoolOutcome>> existingShadowDecisionOutcome(std::shared_ptr<rt::Kv> kv, CodexBankedResetConfig config,
                                                                                       CodexBankedResetDependencies dependencies,
                                                                                       std::vector<ResolvedPoolCandidate> complete, std::string episodeHash,
                                                                                       ShadowDecisionFences fences, double nowMs) {
      if (config.mode != "shadow") co_return std::nullopt;
      rt::Promise<std::optional<CodexResetShadowDecisionRecord>> reading(readShadowDecision(kv, episodeHash));
      const std::optional<CodexResetShadowDecisionRecord> existing = co_await reading.wait();
      if (!existing) co_return std::nullopt;
      if (existing->expires_at_ms <= nowMs) co_return std::nullopt;
      if (!sameShadowFences(existing->fences, fences)) co_return std::nullopt;
      const auto found = std::find_if(complete.begin(), complete.end(), [&existing](const ResolvedPoolCandidate& entry) {
        return existing->selected_account_id_hash && entry.context.account.accountIdHash == *existing->selected_account_id_hash;
      });
      const std::optional<ResolvedPoolCandidate> selected = found == complete.end() ? std::nullopt : std::optional<ResolvedPoolCandidate>(*found);
      // A subscription disabled after its decision was persisted must not report a
      // duplicate would-spend, let alone arm a live spend. An unreadable record
      // fails this duplicate path closed through the same structured outcome the
      // rest of the evaluator returns, instead of rejecting out of it.
      if (selected) {
        rt::Promise<banked_reset::ReadUsageGateResult> gating(banked_reset::readUsageGate(kv, selected->context.account.accountIdHash));
        const banked_reset::ReadUsageGateResult gate = co_await gating.wait();
        if (const auto* failure = std::get_if<banked_reset::ReadUsageGateResultFailure>(&gate)) co_return poolOutcome("skipped", failure->code);
      }
      const CodexBankedResetTelemetry telemetry = dependencies.telemetry ? *dependencies.telemetry : banked_reset::defaultTelemetry;
      const ResolvedPoolCandidate& telemetryCandidate = selected ? *selected : firstResolvedPoolCandidate(complete);
      const CodexBankedResetTelemetryFields fields = telemetryFields(
        telemetryCandidate.context, telemetryCandidate.pool.candidate,
        json::object{
          {"episode_hash", episodeHash},
          {"credit_id_hash", existing->selected_credit_id_hash ? json::value(*existing->selected_credit_id_hash) : json::value(nullptr)},
          {"selected", existing->selected_account_id_hash.has_value()},
          {"reason", existing->decision_reason},
        });
      json::object duplicateFields = fields;
      rt::objectDefine(duplicateFields, "reason", json::value("shadow_decision_exists"));
      emit(telemetry, "codex_reset_duplicate_prevented", duplicateFields);
      metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
      const bool wouldSpend = existing->decision_reason == "selected" && existing->selected_account_id_hash.has_value() &&
                              existing->selected_credit_id_hash.has_value() && selected.has_value();
      co_return poolOutcome("shadow", wouldSpend ? "already_would_spend_once" : existing->decision_reason,
                            wouldSpend ? std::optional<CodexBankedResetPoolCandidate>(selected->pool) : std::nullopt);
    }

    // `Readonly<{ resolvedCandidate: ResolvedPoolCandidate; inventory: ResetInventory | null }>`
    struct PoolInventoryObservation {
      ResolvedPoolCandidate resolvedCandidate;
      std::optional<ResetInventory> inventory;
    };

    // One `async (resolvedCandidate) => ...` of readPoolInventories.
    rt::Task<PoolInventoryObservation> readPoolInventory(ResolvedPoolCandidate resolvedCandidate) {
      std::optional<ResetInventory> inventory;
      try {
        if (!resolvedCandidate.pool.provider) throw rt::TypeError("undefined is not an object (evaluating 'resolvedCandidate.pool.provider.readInventory')");
        rt::Promise<ResetInventory> reading(resolvedCandidate.pool.provider->readInventory(
          resolvedCandidate.context.account, banked_reset::boundedInventorySignal(resolvedCandidate.pool.candidate.signal)));
        inventory = co_await reading.wait();
      } catch (...) {
        inventory = std::nullopt;
      }
      co_return PoolInventoryObservation{.resolvedCandidate = std::move(resolvedCandidate), .inventory = std::move(inventory)};
    }

    /** Read every candidate's inventory concurrently; a failed read is recorded as null. */
    rt::Task<std::vector<PoolInventoryObservation>> readPoolInventories(std::vector<ResolvedPoolCandidate> complete) {
      std::vector<rt::Promise<PoolInventoryObservation>> reading;
      for (const ResolvedPoolCandidate& resolvedCandidate : complete) {
        reading.push_back(rt::Promise<PoolInventoryObservation>(readPoolInventory(resolvedCandidate)));
      }
      auto all = rt::all(std::move(reading));
      std::vector<PoolInventoryObservation> observations = co_await all.wait();
      co_return observations;
    }

    /** Narrows an observation to one that carries a real inventory. */
    bool hasPoolInventory(const PoolInventoryObservation& observation) { return observation.inventory.has_value(); }

    struct PoolObserved {
      [[maybe_unused]] static constexpr std::string_view kind = "observed";
      std::vector<PoolInventoryObservation> inventoryResults;
      double nowMs;
    };

    /**
     * Read every inventory, then re-prove the fences and the observed windows.
     * A selection based on any stale account state is not audit evidence and can
     * never become a live spend.
     */
    rt::Task<std::variant<PoolObserved, PoolStepOutcome>> observePoolInventories(std::shared_ptr<rt::Kv> kv, std::vector<ResolvedPoolCandidate> complete,
                                                                                std::function<double()> clock) {
      rt::Promise<std::vector<PoolInventoryObservation>> reading(readPoolInventories(complete));
      std::vector<PoolInventoryObservation> inventoryResults = co_await reading.wait();
      const std::optional<double> nowMs = readClock(clock);
      if (!nowMs) co_return PoolStepOutcome{.outcome = poolOutcome("skipped", "invalid_clock")};
      if (std::any_of(inventoryResults.begin(), inventoryResults.end(), [](const PoolInventoryObservation& observation) { return !observation.inventory; })) {
        co_return PoolStepOutcome{.outcome = poolOutcome("skipped", "inventory_unavailable")};
      }
      rt::Promise<std::optional<std::string>> checking(stalePoolFenceReason(kv, complete));
      const std::optional<std::string> staleFence = co_await checking.wait();
      if (rt::truthy(staleFence)) co_return PoolStepOutcome{.outcome = poolOutcome("skipped", *staleFence)};
      if (std::any_of(complete.begin(), complete.end(),
                      [&nowMs](const ResolvedPoolCandidate& entry) { return !banked_reset_claim::quotaWindowIsOpen(entry.pool.candidate, *nowMs); })) {
        co_return PoolStepOutcome{.outcome = poolOutcome("skipped", "quota_window_expired")};
      }
      co_return PoolObserved{.inventoryResults = std::move(inventoryResults), .nowMs = *nowMs};
    }

    /**
     * A pool inventory is only selectable while it validates and no credit that is
     * still marked available has already expired.
     */
    bool poolInventoryIsUsable(const ResetInventory& inventory, double nowMs) {
      return banked_reset_claim::validInventory(inventory, nowMs) &&
             !std::any_of(inventory.credits.begin(), inventory.credits.end(), [nowMs](const ResetInventoryCredit& credit) {
               return credit.status == "available" && credit.expiresAtMs && *credit.expiresAtMs <= nowMs;
             });
    }

    struct PoolSelection {
      [[maybe_unused]] static constexpr std::string_view kind = "selection";
      std::optional<SelectedPoolCredit> selected;
      std::string decisionReason;
    };

    /** Pick one usable credit per candidate and rank them deterministically. */
    rt::Task<std::variant<PoolSelection, PoolStepOutcome>> selectPoolCredits(std::vector<PoolInventoryObservation> inventoryResults, double nowMs,
                                                                            HashFunction hash) {
      std::vector<SelectedPoolCredit> selectedCredits;
      std::string decisionReason = "inventory_empty";
      for (const PoolInventoryObservation& observation : inventoryResults) {
        if (!hasPoolInventory(observation)) continue;
        const ResetInventory& inventory = *observation.inventory;
        if (!poolInventoryIsUsable(inventory, nowMs)) {
          decisionReason = "inventory_response_invalid_or_expired";
          selectedCredits.clear();
          break;
        }
        const banked_reset_claim::InventoryCreditSelection selection =
          banked_reset_claim::selectInventoryCredit(inventory, observation.resolvedCandidate.pool.provider.get(), nowMs);
        if (!std::holds_alternative<banked_reset_claim::InventoryCreditSelectionSelected>(selection)) {
          if (std::holds_alternative<banked_reset_claim::InventoryCreditSelectionNoEligibleCredit>(selection)) decisionReason = "inventory_no_eligible_codex_credit";
          continue;
        }
        const ResetInventoryCredit& credit = std::get<banked_reset_claim::InventoryCreditSelectionSelected>(selection).credit;
        rt::Promise<std::string> hashing(hash(joinWithNul({"uos_ai", "codex_reset_credit", credit.id})));
        const std::string creditIdHash = co_await hashing.wait();
        if (!banked_reset::isNonEmptyText(std::string_view(creditIdHash))) co_return PoolStepOutcome{.outcome = poolOutcome("skipped", "credit_hash_unavailable")};
        selectedCredits.push_back(SelectedPoolCredit{.resolved = observation.resolvedCandidate, .credit = credit, .creditIdHash = creditIdHash});
      }
      rt::sort(selectedCredits, [](const SelectedPoolCredit& left, const SelectedPoolCredit& right) {
        const double leftExpiry = left.credit.expiresAtMs.value_or(rt::Number::POSITIVE_INFINITY);
        const double rightExpiry = right.credit.expiresAtMs.value_or(rt::Number::POSITIVE_INFINITY);
        const double expiryOrder = leftExpiry - rightExpiry;
        if (rt::truthy(expiryOrder)) return expiryOrder;
        const double slotOrder = left.resolved.pool.slot - right.resolved.pool.slot;
        if (rt::truthy(slotOrder)) return slotOrder;
        return static_cast<double>(rt::localeCompare(left.credit.id, right.credit.id));
      });
      if (!selectedCredits.empty()) co_return PoolSelection{.selected = selectedCredits.front(), .decisionReason = "selected"};
      co_return PoolSelection{.selected = std::nullopt, .decisionReason = decisionReason == "inventory_empty" ? "no_eligible_credit" : decisionReason};
    }

    struct ShadowDecisionBuilt {
      [[maybe_unused]] static constexpr std::string_view kind = "decision";
      CodexResetShadowDecisionRecord decision;
    };

    /** The redacted decision record, or the reason it must not be written. */
    std::variant<ShadowDecisionBuilt, PoolStepOutcome> buildShadowDecision(const std::string& episodeHash, double nowMs, const std::string& decisionReason,
                                                                           const std::optional<SelectedPoolCredit>& selected, const ShadowDecisionFences& fences) {
      std::vector<double> expiries;
      for (const CodexResetShadowDecisionRecord::Fence& fence : fences) expiries.push_back(fence.quota_reset_at_ms);
      expiries.push_back(selected && selected->credit.expiresAtMs ? *selected->credit.expiresAtMs : rt::Number::POSITIVE_INFINITY);
      const double episodeExpiresAtMs = rt::Math::min(std::span<const double>(expiries));
      if (!banked_reset::isSafeMs(episodeExpiresAtMs) || episodeExpiresAtMs <= nowMs) {
        return PoolStepOutcome{.outcome = poolOutcome("skipped", "shadow_decision_expired")};
      }
      return ShadowDecisionBuilt{
        .decision =
          CodexResetShadowDecisionRecord{
            .v = 1,
            .episode_hash = episodeHash,
            .created_at_ms = nowMs,
            .expires_at_ms = episodeExpiresAtMs,
            .decision_reason = decisionReason,
            .selected_account_id_hash = selected ? std::optional<std::string>(selected->resolved.context.account.accountIdHash) : std::nullopt,
            .selected_credit_id_hash = selected ? std::optional<std::string>(selected->creditIdHash) : std::nullopt,
            .selected_credit_expires_at_ms = selected ? selected->credit.expiresAtMs : std::nullopt,
            .fences = fences,
          },
      };
    }

    /** Persist the shadow decision and emit the telemetry that describes it. */
    rt::Task<CodexBankedResetPoolOutcome> persistShadowDecisionOutcome(std::shared_ptr<rt::Kv> kv, CodexResetShadowDecisionRecord decision, std::string episodeHash,
                                                                       CodexBankedResetDependencies dependencies, std::vector<ResolvedPoolCandidate> complete,
                                                                       std::vector<rt::KvEntryMaybe<>> settingsEntries, PoolSelection selection, double nowMs) {
      const std::optional<SelectedPoolCredit>& selected = selection.selected;
      rt::Promise<std::optional<ShadowDecisionWrite>> persisting(shadowDecisionRecord(kv, decision, nowMs, settingsEntries));
      const std::optional<ShadowDecisionWrite> persisted = co_await persisting.wait();
      if (!persisted) co_return poolOutcome("skipped", "shadow_decision_unavailable");
      const CodexBankedResetTelemetry telemetry = dependencies.telemetry ? *dependencies.telemetry : banked_reset::defaultTelemetry;
      const ResolvedPoolCandidate& telemetryCandidate = selected ? selected->resolved : firstResolvedPoolCandidate(complete);
      const CodexBankedResetTelemetryFields fields = telemetryFields(telemetryCandidate.context, telemetryCandidate.pool.candidate,
                                                                     json::object{
                                                                       {"episode_hash", episodeHash},
                                                                       {"credit_id_hash", selected ? json::value(selected->creditIdHash) : json::value(nullptr)},
                                                                       {"selected", selected.has_value()},
                                                                       {"reason", persisted->record.decision_reason},
                                                                     });
      const std::optional<CodexBankedResetPoolCandidate> selectedPool =
        selected ? std::optional<CodexBankedResetPoolCandidate>(selected->resolved.pool) : std::nullopt;
      if (persisted->kind == "duplicate") {
        json::object duplicateFields = fields;
        rt::objectDefine(duplicateFields, "reason", json::value("shadow_decision_exists"));
        emit(telemetry, "codex_reset_duplicate_prevented", duplicateFields);
        metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
        co_return poolOutcome("shadow", "already_would_spend_once", selectedPool);
      }
      emit(telemetry, "codex_reset_eligible", fields);
      emit(telemetry, "codex_reset_shadow_candidate", fields);
      metric(telemetry, "codex_reset_eligible_total", 1, fields);
      metric(telemetry, "codex_reset_shadow_candidates_total", 1, fields);
      co_return poolOutcome("shadow", selected ? "shadow_selected" : selection.decisionReason, selectedPool);
    }

    /** The live tail: one durable submission attempt for the selected credit. */
    rt::Task<CodexBankedResetPoolOutcome> runLivePoolSubmission(CodexBankedResetDependencies dependencies, std::vector<ResolvedPoolCandidate> complete,
                                                                std::vector<LivePoolWindow> windows, PoolSelection selection) {
      if (!selection.selected) co_return poolOutcome("skipped", selection.decisionReason);
      const SelectedPoolCredit& selected = *selection.selected;
      // The durable claim/submission path must fence every blocked account that
      // established this episode, not just the selected owner. Recovery or
      // rotation after the fresh inventory read makes the episode stale and must
      // block the external consume.
      std::vector<banked_reset::CodexBankedResetFence> fullPoolFences;
      for (const ResolvedPoolCandidate& resolved : complete) {
        for (const banked_reset::CodexBankedResetFence& fence : resolved.pool.candidate.fences) fullPoolFences.push_back(fence);
      }
      for (const auto& window : windows) {
        if (window.slot != selected.resolved.pool.slot) fullPoolFences.push_back(window.fence);
      }
      banked_reset::CodexBankedResetCandidate candidate = selected.resolved.pool.candidate;
      candidate.fences = std::move(fullPoolFences);
      candidate.selectedCredit = selected.credit;
      CodexBankedResetDependencies selectedDependencies = dependencies;
      selectedDependencies.provider = selected.resolved.pool.provider;
      rt::Promise<CodexBankedResetOutcome> attempting(banked_reset_submission::attemptCodexBankedReset(std::move(candidate), std::move(selectedDependencies)));
      const CodexBankedResetOutcome reset = co_await attempting.wait();
      // A racing submission can consume the previously available day budget
      // after selection. Keep the new local claim fenced until the next request.
      if (reset.reason == "account_day_limit_reached") co_return poolOutcome("pending", "account_day_budget_changed", selected.resolved.pool);
      co_return poolOutcome(reset.kind, reset.reason, selected.resolved.pool, reset);
    }
  }

  rt::Task<CodexBankedResetPoolOutcome> evaluateCodexBankedResetPool(std::vector<CodexBankedResetPoolCandidate> candidates,
                                                                     CodexBankedResetDependencies dependencies) {
    const std::function<double()> clock = dependencies.now ? dependencies.now : rt::globalDateNow();
    const std::optional<double> nowMs = readClock(clock);
    if (!nowMs) co_return poolOutcome("skipped", "invalid_clock");
    std::variant<OrderedPool, PoolFailure> orderedResult = orderPoolCandidates(candidates);
    if (const PoolFailure* failure = std::get_if<PoolFailure>(&orderedResult)) co_return poolOutcome("skipped", failure->reason);
    const std::vector<CodexBankedResetPoolCandidate> ordered = std::move(std::get<OrderedPool>(orderedResult).ordered);
    const std::variant<PoolConfig, PoolFailure> configResult = loadPoolSubmissionConfig(ordered, dependencies);
    if (const PoolFailure* failure = std::get_if<PoolFailure>(&configResult)) co_return poolOutcome("skipped", failure->reason);
    const CodexBankedResetConfig config = std::get<PoolConfig>(configResult).config;
    rt::Promise<std::shared_ptr<rt::Kv>> opening(banked_reset_submission::openResetKv(dependencies));
    const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
    if (!kv) co_return poolOutcome("skipped", "kv_unavailable");
    const HashFunction hash = dependencies.hash ? dependencies.hash : HashFunction(&utils::sha256Hex);
    rt::Promise<std::variant<PoolEpisode, PoolStepOutcome>> preparing(preparePoolEpisode(kv, ordered, hash, *nowMs));
    std::variant<PoolEpisode, PoolStepOutcome> prepared = co_await preparing.wait();
    if (PoolStepOutcome* settled = std::get_if<PoolStepOutcome>(&prepared)) co_return std::move(settled->outcome);
    PoolEpisode& episode = std::get<PoolEpisode>(prepared);
    rt::Promise<std::optional<std::string>> checking(stalePoolFenceReason(kv, episode.complete));
    const std::optional<std::string> staleFence = co_await checking.wait();
    if (rt::truthy(staleFence)) co_return poolOutcome("skipped", *staleFence);
    rt::Promise<std::optional<CodexBankedResetPoolOutcome>> deduplicating(
      existingShadowDecisionOutcome(kv, config, dependencies, episode.complete, episode.episodeHash, episode.fences, *nowMs));
    std::optional<CodexBankedResetPoolOutcome> duplicateShadow = co_await deduplicating.wait();
    if (duplicateShadow) co_return std::move(*duplicateShadow);

    std::vector<LivePoolWindow> liveWindows;
    if (config.mode == "live") {
      rt::Promise<LivePoolWindows> reading(readLivePoolWindows(kv, episode.complete, dependencies, *nowMs));
      auto live = co_await reading.wait();
      if (auto* settled = std::get_if<PoolStepOutcome>(&live)) co_return std::move(settled->outcome);
      liveWindows = std::move(std::get<std::vector<LivePoolWindow>>(live));
      rt::Promise<std::optional<PoolStepOutcome>> filtering(filterLivePoolEligibility(kv, episode, liveWindows, *nowMs));
      if (auto settled = co_await filtering.wait()) {
        if (!(co_await livePoolWindowsCurrent(kv, liveWindows))) co_return poolOutcome("pending", "redemption_fence_stale");
        const auto current = readClock(clock);
        if (!current || banked_reset::utcDay(*current) != banked_reset::utcDay(*nowMs)) co_return poolOutcome("pending", "account_day_budget_changed");
        co_return std::move(settled->outcome);
      }
    }

    rt::Promise<std::variant<PoolObserved, PoolStepOutcome>> observing(observePoolInventories(kv, episode.eligible, clock));
    std::variant<PoolObserved, PoolStepOutcome> observed = co_await observing.wait();
    if (!liveWindows.empty()) {
      if (!(co_await livePoolWindowsCurrent(kv, liveWindows))) co_return poolOutcome("pending", "redemption_fence_stale");
      const auto current = readClock(clock);
      if (!current || banked_reset::utcDay(*current) != banked_reset::utcDay(*nowMs)) co_return poolOutcome("pending", "account_day_budget_changed");
    }
    if (PoolStepOutcome* settled = std::get_if<PoolStepOutcome>(&observed)) co_return std::move(settled->outcome);
    const PoolObserved& observation = std::get<PoolObserved>(observed);
    rt::Promise<std::variant<PoolSelection, PoolStepOutcome>> selecting(selectPoolCredits(observation.inventoryResults, observation.nowMs, hash));
    std::variant<PoolSelection, PoolStepOutcome> selected = co_await selecting.wait();
    if (PoolStepOutcome* settled = std::get_if<PoolStepOutcome>(&selected)) co_return std::move(settled->outcome);
    const PoolSelection& selection = std::get<PoolSelection>(selected);
    std::variant<ShadowDecisionBuilt, PoolStepOutcome> decisionResult =
      buildShadowDecision(episode.episodeHash, observation.nowMs, selection.decisionReason, selection.selected, episode.fences);
    if (PoolStepOutcome* settled = std::get_if<PoolStepOutcome>(&decisionResult)) co_return std::move(settled->outcome);
    const CodexResetShadowDecisionRecord& decision = std::get<ShadowDecisionBuilt>(decisionResult).decision;
    if (config.mode == "shadow") {
      rt::Promise<CodexBankedResetPoolOutcome> persisting(
        persistShadowDecisionOutcome(kv, decision, episode.episodeHash, dependencies, episode.complete, episode.settingsEntries, selection, observation.nowMs));
      CodexBankedResetPoolOutcome persisted = co_await persisting.wait();
      co_return persisted;
    }
    rt::Promise<CodexBankedResetPoolOutcome> submitting(runLivePoolSubmission(dependencies, episode.complete, std::move(liveWindows), selection));
    CodexBankedResetPoolOutcome submitted = co_await submitting.wait();
    co_return submitted;
  }
}
