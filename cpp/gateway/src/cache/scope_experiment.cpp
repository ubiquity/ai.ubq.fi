#include "uos/cache/scope_experiment.hpp"
#include "uos/catalog/promotion.hpp"
#include "uos/codex/experiment.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include <algorithm>
#include <memory>

namespace uos::cache::scope_experiment {
namespace {
  using namespace scope_experiment_model;
  using namespace scope_experiment_targets;
  namespace transport = codex::experiment;
  using Session = transport::CodexCacheScopeExperimentSession;
  using Entry = rt::KvEntryMaybe<>;
  rt::Task<Entry> readEntry(std::shared_ptr<rt::Kv> store, rt::KvKey key) {
    const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
    auto task = store->get(key, strong); auto entry = co_await std::move(task); co_return entry;
  }
  std::vector<CycleEvidence> existingCycles(const ExperimentState& state, const rt::KvValue& evidenceValue) {
    const auto evidence = parseEvidence(evidenceValue);
    if (!activeStateEvidenceIsConsistent(state, evidence)) throw PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment evidence is inconsistent.");
    return evidence ? evidence->cycles : std::vector<CycleEvidence>{};
  }
  /** Durable state must still be the exact campaign row this cycle claimed. */
  ExperimentState requirePersistedState(const rt::KvValue& value, const ExperimentState& state) {
    const auto persisted = parseState(value);
    if (!persisted || persisted->started_at_ms != state.started_at_ms || persisted->campaign_owner != state.campaign_owner ||
        !sameTargetDefinition(persisted->target, state.target))
      throw PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment state changed concurrently.");
    return *persisted;
  }
  struct PersistEntries { Entry state; Entry evidence; Entry cycle; Entry campaign; };
  rt::Task<PersistEntries> loadPersistEntries(std::shared_ptr<rt::Kv> kv, PromptCacheScopeTargetBinding target) {
    rt::Promise<Entry> state(readEntry(kv, stateKey(target))), evidence(readEntry(kv, evidenceKey(target))),
      cycle(readEntry(kv, cycleLeaseKey(target))), campaign(readEntry(kv, campaignLeaseKey(target)));
    auto readEntries = co_await rt::all(state, evidence, cycle, campaign).wait();
    auto& [stateEntry, evidenceEntry, cycleEntry, campaignEntry] = readEntries;
    co_return PersistEntries{std::move(stateEntry), std::move(evidenceEntry), std::move(cycleEntry), std::move(campaignEntry)};
  }
  rt::Task<void> persistIntermediate(std::shared_ptr<rt::Kv> kv, ExperimentState state, std::string cycleOwner, CycleEvidence cycle,
                                    ExperimentState nextState) {
    for (int attempt = 0; attempt < 3; ++attempt) {
      rt::Promise<PersistEntries> reading(loadPersistEntries(kv, state.target)); const auto entries = co_await reading.wait();
      if (!ownsLease(entries.cycle.value, cycleOwner) || !ownsCampaignLease(entries.campaign.value, state))
        throw PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment lost its lease.");
      const auto persistedState = requirePersistedState(entries.state.value, state);
      if (persistedState.next_cycle != state.next_cycle) throw PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment state changed concurrently.");
      auto cycles = existingCycles(state, entries.evidence.value); cycles.push_back(cycle);
      const StoredEvidence evidence{3, nextState.target, nextState.next_cycle == PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES + 1 ? "ready_to_promote" : "in_progress",
        state.started_at_ms, rt::dateNow(), std::move(cycles)};
      auto atomic = kv->atomic(); atomic->check(entries.state).check(entries.evidence).check(entries.cycle).check(entries.campaign)
        .set(stateKey(nextState.target), rt::KvValue(json::value_from(nextState)), {.expireIn = rt::Math::max(1, nextState.expires_at_ms - rt::dateNow())})
        .set(evidenceKey(nextState.target), rt::KvValue(json::value_from(evidence)), {.expireIn = rt::Math::max(1, nextState.expires_at_ms - rt::dateNow())});
      if (nextState.next_cycle <= PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES) atomic->delete_(cycleLeaseKey(state.target));
      rt::Promise<rt::KvCommitResult> committing(atomic->commit()); const auto commit = co_await committing.wait();
      if (commit.ok) co_return;
    }
    throw PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment evidence could not be persisted.");
  }
  struct FinalizeOptions {
    std::optional<ConcreteScopeObservation> scope{};
    std::optional<InconclusiveReason> reason{};
    std::optional<CycleEvidence> cycle{};
    std::optional<double> completedCycles{};
  };
  PromptCacheScopeExperimentResult finalizeResult(const ExperimentState& state, const std::string& outcome, const StoredEvidence& evidence,
                                                  const FinalizeOptions& options) {
    return {std::string(PROMPT_CACHE_SCOPE_EXPERIMENT_PROVIDER), std::string(CODEX_CHATGPT_PROMPT_CACHE_TELEMETRY_PROVIDER), state.target.id,
      state.target.model, outcome == "completed" ? "completed" : "inconclusive",
      options.completedCycles.value_or(static_cast<double>(state.classifications.size()) + (options.cycle && options.cycle->classification ? 1 : 0)),
      evidence.verified_at_ms, options.scope, options.reason};
  }
  rt::Task<PromptCacheScopeExperimentResult> finalize(std::shared_ptr<rt::Kv> kv, ExperimentState state, std::string cycleOwner,
                                                      std::string outcome, FinalizeOptions options) {
    for (int attempt = 0; attempt < 3; ++attempt) {
      rt::Promise<PersistEntries> reading(loadPersistEntries(kv, state.target)); const auto entries = co_await reading.wait();
      if (!ownsLease(entries.cycle.value, cycleOwner) || !ownsCampaignLease(entries.campaign.value, state))
        throw PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment lost its lease.");
      const auto persistedState = requirePersistedState(entries.state.value, state);
      const auto prior = parseEvidence(entries.evidence.value);
      if (!activeStateEvidenceIsConsistent(persistedState, prior))
        throw PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment evidence changed concurrently.");
      auto cycles = prior ? prior->cycles : std::vector<CycleEvidence>{}; if (options.cycle) cycles.push_back(*options.cycle);
      const StoredEvidence evidence{3, state.target, outcome, state.started_at_ms, rt::dateNow(), std::move(cycles), options.reason};
      auto atomic = kv->atomic(); atomic->check(entries.state).check(entries.evidence).check(entries.cycle).check(entries.campaign)
        // Terminal evidence outlives the active session, allowing later invocations to advance to sibling targets.
        .set(evidenceKey(state.target), rt::KvValue(json::value_from(evidence)))
        .delete_(stateKey(state.target)).delete_(cycleLeaseKey(state.target)).delete_(campaignLeaseKey(state.target));
      rt::Promise<rt::KvCommitResult> committing(atomic->commit()); const auto commit = co_await committing.wait();
      if (commit.ok) co_return finalizeResult(state, outcome, evidence, options);
    }
    throw PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment final result could not be persisted.");
  }
  void assertRequestBodyStable(const json::object& body, const std::string& expectedBody) {
    if (rt::jsonStringify(body) != expectedBody) throw PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment request body drifted.");
  }
  struct MidCycleSession { std::optional<InconclusiveReason> reason; Session session; PromptCacheScopeTargetBinding binding; };
  /** Row five refreshes the pinned slot exactly once. */
  rt::Task<MidCycleSession> refreshMidCycleSession(std::shared_ptr<rt::Kv> kv, ExperimentState state, std::string cycleOwner,
    PromptCacheScopeTargetBinding binding, Session session, rt::AbortSignal cycleSignal) {
    rt::Promise<ScopeTargetResolution> resolving(resolveBoundTarget(kv, binding)); const auto beforeRefresh = co_await resolving.wait();
    if (const auto* stop = std::get_if<ScopeTargetResolutionInconclusive>(&beforeRefresh)) co_return MidCycleSession{stop->reason, session, binding};
    const auto refreshedBinding = std::get<ScopeTargetResolutionResolved>(beforeRefresh).value.binding;
    rt::Promise<void> renewing(renewLease(kv, state, cycleOwner)); co_await renewing.wait();
    const std::optional<rt::AbortSignal> signal = cycleSignal;
    auto task = transport::refreshCodexCacheScopeExperimentSlot(session, 1, signal);
    rt::Promise<transport::CodexCacheScopeExperimentRefreshResult> refreshing(std::move(task)); const auto refresh = co_await refreshing.wait();
    const auto* refreshed = std::get_if<transport::CodexCacheScopeExperimentRefreshResultRefreshed>(&refresh);
    if (!refreshed) co_return MidCycleSession{"auth_pool_drift", session, refreshedBinding};
    if (!refreshed->tokenChanged) co_return MidCycleSession{"refresh_unchanged", refreshed->session, refreshedBinding};
    auto nextBinding = refreshedBinding; nextBinding.auth_pool_versionstamp = refreshed->session.authPoolVersionstamp;
    co_return MidCycleSession{std::nullopt, refreshed->session, nextBinding};
  }
  std::optional<InconclusiveReason> validateSampleSignal(const ReadSampleResultSample& parsed, std::size_t index, std::optional<double> reusableTokens) {
    const auto& expectedSignal = CACHE_SCOPE_EXPECTED_SIGNALS[index];
    if (expectedSignal && parsed.signal != *expectedSignal) return "invalid_cache_signal";
    if (index > 0 && (!reusableTokens || !matchesCycleReusableCounter(parsed.sample.usage, *reusableTokens))) return "invalid_cache_signal";
    if (CACHE_SCOPE_DISCRIMINATOR_INDEXES.has(static_cast<double>(index)) && hasMixedPrefixScaleCacheSignals(parsed.sample.usage)) return "invalid_cache_signal";
    return std::nullopt;
  }
  struct CycleRun { CycleEvidence evidence; Session session; PromptCacheScopeTargetBinding binding; };
  rt::Task<CycleRun> runCycle(std::shared_ptr<rt::Kv> kv, ExperimentState state, std::string cycleOwner, double cycleNumber, Session initialSession) {
    const auto cycleSignal = rt::AbortSignal::timeout(PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLE_DEADLINE_MS);
    const auto body = buildExperimentRequest(state.target.model, rt::crypto::randomUUID(), "uos-cache-scope-v5-" + rt::crypto::randomUUID());
    const auto expectedBody = rt::jsonStringify(body);
    const auto conversationA = rt::crypto::randomUUID(), conversationB = rt::crypto::randomUUID();
    const std::array<std::string, 10> conversations{conversationA, conversationA, conversationA, conversationA, conversationA, conversationA,
      conversationA, conversationB, conversationB, conversationA};
    std::vector<PromptCacheScopeSample> samples; std::vector<CacheSignal> signals;
    auto session = initialSession; auto binding = state.target;
    const auto inconclusive = [&](const InconclusiveReason& reason) { return CycleRun{{cycleNumber, samples, std::nullopt, reason}, session, binding}; };
    for (std::size_t index = 0; static_cast<double>(index) < PROMPT_CACHE_SCOPE_EXPERIMENT_SAMPLES_PER_CYCLE; ++index) {
      throwIfAborted(cycleSignal);
      if (index == 5) {
        auto task = refreshMidCycleSession(kv, state, cycleOwner, binding, session, cycleSignal);
        rt::Promise<MidCycleSession> refreshing(std::move(task)); const auto refresh = co_await refreshing.wait();
        session = refresh.session; binding = refresh.binding;
        if (refresh.reason) co_return inconclusive(*refresh.reason);
      }
      rt::Promise<void> renewing(renewLease(kv, state, cycleOwner)); co_await renewing.wait();
      rt::Promise<ScopeTargetResolution> resolving(resolveBoundTarget(kv, binding)); const auto currentTarget = co_await resolving.wait();
      if (const auto* stop = std::get_if<ScopeTargetResolutionInconclusive>(&currentTarget)) co_return inconclusive(stop->reason);
      binding = std::get<ScopeTargetResolutionResolved>(currentTarget).value.binding;
      assertRequestBodyStable(body, expectedBody);
      const auto startedAtMs = rt::performanceNow();
      const auto sampleSignal = rt::AbortSignal::any({cycleSignal, rt::AbortSignal::timeout(PROMPT_CACHE_SCOPE_EXPERIMENT_SAMPLE_DEADLINE_MS)});
      transport::FetchCodexResponsesForCacheScopeExperimentOptions options{session, CACHE_SCOPE_EXPECTED_SLOTS[index], conversations[index],
        binding.catalog_client_version, sampleSignal};
      auto fetchTask = transport::fetchCodexResponsesForCacheScopeExperiment(body, options);
      rt::Promise<rt::Response> fetching(std::move(fetchTask)); const auto response = co_await fetching.wait();
      auto parseTask = readPromptCacheScopeExperimentCompletedUsage(response, CACHE_SCOPE_EXPECTED_SLOTS[index], binding.model, startedAtMs, sampleSignal);
      rt::Promise<ReadSampleResult> parsing(std::move(parseTask)); const auto parsed = co_await parsing.wait();
      if (const auto* stop = std::get_if<ReadSampleResultInconclusive>(&parsed)) co_return inconclusive(stop->reason);
      const auto& sample = std::get<ReadSampleResultSample>(parsed);
      const auto reusableTokens = samples.empty() ? std::nullopt : std::optional(samples.front().usage.cache_write_tokens);
      const auto invalidReason = validateSampleSignal(sample, index, reusableTokens);
      if (invalidReason) co_return inconclusive(*invalidReason);
      samples.push_back({std::string(CACHE_SCOPE_STEP_NAMES[index]), sample.sample.slot, sample.sample.raw_usage, sample.sample.usage, sample.sample.elapsed_ms});
      signals.push_back(sample.signal);
    }
    const auto classification = classifyCycle(binding.model, samples, signals);
    if (classification) co_return CycleRun{{cycleNumber, samples, classification}, session, binding};
    co_return inconclusive("slot_drift");
  }
  PromptCacheScopeExperimentResult resultForIntermediate(const ExperimentState& state) {
    return {std::string(PROMPT_CACHE_SCOPE_EXPERIMENT_PROVIDER), std::string(CODEX_CHATGPT_PROMPT_CACHE_TELEMETRY_PROVIDER), state.target.id,
      state.target.model, "in_progress", static_cast<double>(state.classifications.size()), rt::dateNow()};
  }
  InconclusiveReason promotionInconclusiveReason(const std::string& reason) {
    if (reason == "model_drift" || reason == "catalog_drift" || reason == "capability_changed") return "target_catalog_drift";
    if (reason == "auth_pool_drift" || reason == "runtime_drift") return reason;
    return "promotion_conflict";
  }
  struct ScopePromotion { std::string status; std::optional<InconclusiveReason> reason{}; };
  rt::Task<ScopePromotion> promoteScope(std::shared_ptr<rt::Kv> kv, ExperimentState state, std::string cycleOwner, ConcreteScopeObservation scope) {
    models::codex_models::PromptCacheScope observation{scope.probe_profile, scope.account_slots, scope.token_refresh, scope.conversation_id,
      scope.effective_model, PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES, "live_probe", rt::dateNow()};
    catalog::promotion::PromptCacheScopePromotionInput input{state.target.model, observation, {cycleLeaseKey(state.target), cycleOwner},
      state.auth_pool_versionstamp, state.target.catalog_versionstamp, state.target.runtime_versionstamp};
    auto task = catalog::promotion::promoteCodexPromptCacheScope(kv, input);
    rt::Promise<catalog::promotion::PromptCacheScopePromotionResult> promoting(std::move(task)); const auto promotion = co_await promoting.wait();
    if (promotion.status == "promoted") co_return ScopePromotion{"promoted"};
    co_return ScopePromotion{"inconclusive", promotionInconclusiveReason(promotion.reason.value_or(""))};
  }
  /** A successful promotion must never be downgraded to failed by a terminal-write failure. */
  struct PromotionReceipt { bool promoted = false; };
  rt::Task<PromptCacheScopeExperimentResult> promoteResolvedScope(std::shared_ptr<rt::Kv> kv, ExperimentState state, std::string cycleOwner,
    ConcreteScopeObservation scope, std::shared_ptr<PromotionReceipt> receipt) {
    rt::Promise<ScopePromotion> promoting(promoteScope(kv, state, cycleOwner, scope)); const auto promotion = co_await promoting.wait();
    FinalizeOptions options;
    std::string outcome = "completed";
    if (promotion.status == "inconclusive") { options.reason = promotion.reason; outcome = "inconclusive"; }
    else { receipt->promoted = true; options.scope = scope; }
    auto task = finalize(kv, state, cycleOwner, outcome, options);
    rt::Promise<PromptCacheScopeExperimentResult> finishing(std::move(task)); const auto result = co_await finishing.wait(); co_return result;
  }
  rt::Task<PromptCacheScopeExperimentResult> promotePendingScope(std::shared_ptr<rt::Kv> kv, ExperimentState state, std::string cycleOwner,
                                                               std::shared_ptr<PromotionReceipt> receipt) {
    if (!state.pending_scope) {
      FinalizeOptions options{.reason = "cycle_disagreement"};
      auto task = finalize(kv, state, cycleOwner, "inconclusive", options);
      rt::Promise<PromptCacheScopeExperimentResult> finishing(std::move(task)); const auto result = co_await finishing.wait(); co_return result;
    }
    rt::Promise<PromptCacheScopeExperimentResult> promoting(promoteResolvedScope(kv, state, cycleOwner, *state.pending_scope, receipt));
    const auto result = co_await promoting.wait(); co_return result;
  }
  struct CycleCompletionFinished { PromptCacheScopeExperimentResult result; };
  struct CycleCompletionReady { ExperimentState state; ConcreteScopeObservation scope; };
  using CycleCompletion = std::variant<CycleCompletionFinished, CycleCompletionReady>;
  /** Runs one fixed cycle, persists it, and reports whether only promotion is left. */
  rt::Task<CycleCompletion> completeCycle(std::shared_ptr<rt::Kv> kv, ExperimentState state, std::string cycleOwner, Session initialSession) {
    rt::Promise<CycleRun> running(runCycle(kv, state, cycleOwner, state.next_cycle, initialSession)); const auto cycle = co_await running.wait();
    const auto& classification = cycle.evidence.classification;
    if (!classification) {
      FinalizeOptions options{.reason = cycle.evidence.inconclusive_reason.value_or("incomplete_telemetry"), .cycle = cycle.evidence};
      auto task = finalize(kv, state, cycleOwner, "inconclusive", options);
      rt::Promise<PromptCacheScopeExperimentResult> finishing(std::move(task)); const auto result = co_await finishing.wait(); co_return CycleCompletionFinished{result};
    }
    auto classifications = state.classifications; classifications.push_back(*classification); const auto nextCycle = state.next_cycle + 1;
    const bool agreed = static_cast<double>(classifications.size()) == PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES && sharedObservation(classifications).has_value();
    if (static_cast<double>(classifications.size()) == PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES && !agreed) {
      FinalizeOptions options{.reason = "cycle_disagreement", .cycle = cycle.evidence, .completedCycles = static_cast<double>(classifications.size())};
      auto task = finalize(kv, state, cycleOwner, "inconclusive", options);
      rt::Promise<PromptCacheScopeExperimentResult> finishing(std::move(task)); const auto result = co_await finishing.wait(); co_return CycleCompletionFinished{result};
    }
    auto nextState = state; nextState.target = cycle.binding; nextState.auth_pool_versionstamp = cycle.session.authPoolVersionstamp;
    nextState.next_cycle = nextCycle; nextState.classifications = std::move(classifications);
    if (nextCycle == PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES + 1) nextState.pending_scope = *classification;
    auto task = persistIntermediate(kv, state, cycleOwner, cycle.evidence, nextState); rt::Promise<void> persisting(std::move(task)); co_await persisting.wait();
    if (nextCycle <= PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES) co_return CycleCompletionFinished{resultForIntermediate(nextState)};
    co_return CycleCompletionReady{nextState, *classification};
  }
  rt::Reason experimentFailure(rt::Reason error) {
    try { if (error) std::rethrow_exception(error); }
    catch (const PromptCacheScopeExperimentFailedError&) { return error; }
    catch (const transport::CodexCacheScopeExperimentError& failure) { return rt::makeReason(PromptCacheScopeExperimentFailedError(failure.message)); }
    catch (...) {}
    return rt::makeReason(PromptCacheScopeExperimentFailedError(rt::isError(error) ? rt::errorMessage(error).value_or("") : "Prompt-cache scope experiment cycle failed."));
  }
  rt::Task<rt::Reason> handleCycleFailure(std::shared_ptr<rt::Kv> kv, ExperimentState state, std::string cycleOwner, rt::Reason error, bool promotionSucceeded) {
    bool passthrough = false;
    try { if (error) std::rethrow_exception(error); }
    catch (const PromptCacheScopeExperimentBusyError&) { passthrough = true; }
    catch (const PromptCacheScopeExperimentUnavailableError&) { passthrough = true; }
    catch (...) {}
    if (passthrough) co_return error;
    if (!promotionSucceeded) {
      try {
        FinalizeOptions options; auto task = finalize(kv, state, cycleOwner, "failed", options);
        rt::Promise<PromptCacheScopeExperimentResult> finishing(std::move(task)); static_cast<void>(co_await finishing.wait());
      } catch (...) { /* A lost lease fences the run; its short expiry is the safe fallback. */ }
    }
    co_return experimentFailure(error);
  }
  struct PreparedCampaignRun { std::shared_ptr<rt::Kv> kv; Session session; PromptCacheScopeTargetBinding binding; };
  rt::Task<PreparedCampaignRun> prepareCampaignRun(PromptCacheScopeExperimentTelemetryBaseline telemetryBaseline) {
    auto pending = ::uos::kv::getKv(); const auto kv = co_await pending.wait();
    if (!kv) throw PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiments require Deno KV.");
    const auto baselineTarget = parseTargetBinding(json::value_from(telemetryBaseline.target));
    if (!baselineTarget) throw PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiment requires a current-release Stage 0 telemetry baseline.");
    rt::Promise<ScopeTargetResolution> resolving(resolveBoundTarget(kv, *baselineTarget)); const auto preflight = co_await resolving.wait();
    const auto* resolved = std::get_if<ScopeTargetResolutionResolved>(&preflight);
    if (!resolved) throw PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiment target changed after its Stage 0 telemetry baseline.");
    rt::Promise<Session> beginning(transport::beginCodexCacheScopeExperiment()); const auto session = co_await beginning.wait();
    co_return PreparedCampaignRun{kv, session, resolved->value.binding};
  }
  std::optional<InconclusiveReason> campaignGuardReason(const ExperimentState& state, const PromptCacheScopeTargetBinding& binding, const Session& session) {
    if (state.expires_at_ms <= rt::dateNow()) return "session_expired";
    if (state.auth_pool_versionstamp != session.authPoolVersionstamp) return "auth_pool_drift";
    if (!sameTargetCore(state.target, binding)) return "target_catalog_drift";
    return std::nullopt;
  }
}
rt::Task<scope_experiment_model::PromptCacheScopeExperimentResult> runPromptCacheScopeExperiment(PromptCacheScopeExperimentTelemetryBaseline telemetryBaseline) {
  rt::Promise<PreparedCampaignRun> preparing(prepareCampaignRun(telemetryBaseline)); const auto prepared = co_await preparing.wait();
  rt::Promise<AcquiredCycle> acquiring(acquireCycle(prepared.kv, prepared.binding, prepared.session.authPoolVersionstamp)); const auto acquired = co_await acquiring.wait();
  const auto state = acquired.state; const auto cycleOwner = acquired.cycle_owner; const auto kv = prepared.kv;
  auto receipt = std::make_shared<PromotionReceipt>(); rt::Reason failure;
  try {
    const auto guardReason = campaignGuardReason(state, prepared.binding, prepared.session);
    if (guardReason) {
      FinalizeOptions options{.reason = guardReason}; auto task = finalize(kv, state, cycleOwner, "inconclusive", options);
      rt::Promise<PromptCacheScopeExperimentResult> finishing(std::move(task)); const auto result = co_await finishing.wait(); co_return result;
    }
    rt::Promise<ScopeTargetResolution> resolving(resolveBoundTarget(kv, state.target)); const auto rebound = co_await resolving.wait();
    if (const auto* stop = std::get_if<ScopeTargetResolutionInconclusive>(&rebound)) {
      FinalizeOptions options{.reason = stop->reason}; auto task = finalize(kv, state, cycleOwner, "inconclusive", options);
      rt::Promise<PromptCacheScopeExperimentResult> finishing(std::move(task)); const auto result = co_await finishing.wait(); co_return result;
    }
    auto boundState = state; boundState.target = std::get<ScopeTargetResolutionResolved>(rebound).value.binding;
    if (state.next_cycle == PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES + 1) {
      rt::Promise<PromptCacheScopeExperimentResult> promoting(promotePendingScope(kv, boundState, cycleOwner, receipt));
      const auto result = co_await promoting.wait(); co_return result;
    }
    rt::Promise<CycleCompletion> completing(completeCycle(kv, boundState, cycleOwner, prepared.session)); const auto completion = co_await completing.wait();
    if (const auto* finished = std::get_if<CycleCompletionFinished>(&completion)) co_return finished->result;
    const auto& ready = std::get<CycleCompletionReady>(completion);
    rt::Promise<PromptCacheScopeExperimentResult> promoting(promoteResolvedScope(kv, ready.state, cycleOwner, ready.scope, receipt));
    const auto result = co_await promoting.wait(); co_return result;
  } catch (...) { failure = std::current_exception(); }
  rt::Promise<rt::Reason> handling(handleCycleFailure(kv, state, cycleOwner, failure, receipt->promoted)); const auto error = co_await handling.wait();
  std::rethrow_exception(error);
}
}
