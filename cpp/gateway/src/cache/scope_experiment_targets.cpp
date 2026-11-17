#include "uos/cache/scope_experiment_targets.hpp"
#include "uos/cache/scope_targets.hpp"
#include "uos/kv.hpp"
#include "uos/runtime_config.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include <algorithm>

namespace uos::cache::scope_experiment_targets {
namespace {
  using namespace scope_experiment_model;
  using Entry = rt::KvEntryMaybe<>;
  constexpr rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
  rt::Task<Entry> readEntry(std::shared_ptr<rt::Kv> store, rt::KvKey key) {
    auto task = store->get(key, strong); auto entry = co_await std::move(task); co_return entry;
  }
  rt::Task<std::optional<RuntimeBinding>> loadRuntimeBinding(std::shared_ptr<rt::Kv> kv) {
    rt::Promise<Entry> pending(readEntry(kv, runtime_config::RUNTIME_CONFIG_V2_KEY));
    const auto entry = co_await pending.wait();
    const auto runtime = runtime_config::normalizeRuntimeConfig(entry.value);
    if (!runtime || !entry.versionstamp || entry.versionstamp->empty()) co_return std::nullopt;
    co_return RuntimeBinding{*entry.versionstamp, runtime->default_model};
  }
  std::optional<std::string> trimmed(const std::optional<std::string>& value) {
    if (!value) return std::nullopt;
    auto result = rt::trim(*value); return result.empty() ? std::nullopt : std::optional(result);
  }
  std::optional<PromptCacheScopeTargetBinding> targetBindingFromInventory(const scope_targets::PromptCacheScopeTargetInventory& inventory,
    const scope_targets::PromptCacheScopeTarget& target, const RuntimeBinding& runtime) {
    const auto inventoryFingerprint = trimmed(inventory.inventory_fingerprint);
    const auto catalogVersionstamp = trimmed(target.catalog_versionstamp);
    const auto authPoolVersionstamp = trimmed(target.codex_auth_pool_versionstamp);
    const auto authPoolIdentityFingerprint = trimmed(target.codex_auth_pool_identity_fingerprint);
    const auto model = rt::trim(target.model), targetId = rt::trim(target.id), capabilityFingerprint = rt::trim(target.capability_fingerprint);
    const auto clientVersion = trimmed(target.catalog_client_version);
    if (inventory.status != "ready" || !inventoryFingerprint || !catalogVersionstamp || !authPoolVersionstamp || !authPoolIdentityFingerprint ||
        model.empty() || targetId.empty() || capabilityFingerprint.empty() || target.provider != PROMPT_CACHE_SCOPE_EXPERIMENT_PROVIDER ||
        target.telemetry_provider != CODEX_CHATGPT_PROMPT_CACHE_TELEMETRY_PROVIDER ||
        !std::holds_alternative<scope_targets::PromptCacheScopeTargetTopologyCodexAccountPool>(target.topology) ||
        !std::holds_alternative<scope_targets::PromptCacheScopeTargetProbeabilityProbeable>(target.probeability)) return std::nullopt;
    return PromptCacheScopeTargetBinding{targetId, std::string(PROMPT_CACHE_SCOPE_EXPERIMENT_PROVIDER),
      std::string(CODEX_CHATGPT_PROMPT_CACHE_TELEMETRY_PROVIDER), "codex_account_pool", model,
      std::string(models::codex_models::PROMPT_CACHE_SCOPE_PROBE_PROFILE), capabilityFingerprint, *inventoryFingerprint, *catalogVersionstamp,
      runtime.versionstamp, *authPoolVersionstamp, *authPoolIdentityFingerprint, clientVersion};
  }
  struct InventoryAndRuntime { scope_targets::PromptCacheScopeTargetInventory inventory; RuntimeBinding runtime; };
  rt::Task<std::optional<InventoryAndRuntime>> loadInventoryAndRuntime(std::shared_ptr<rt::Kv> kv) {
    scope_targets::LoadPromptCacheScopeTargetInventoryOptions options{.kv = kv};
    rt::Promise<scope_targets::PromptCacheScopeTargetInventory> inventory(scope_targets::loadPromptCacheScopeTargetInventory(options));
    rt::Promise<std::optional<RuntimeBinding>> runtime(loadRuntimeBinding(kv));
    auto readEntries = co_await rt::all(inventory, runtime).wait();
    auto& [inventoryResult, runtimeResult] = readEntries;
    if (inventoryResult.status != "ready" || !runtimeResult) co_return std::nullopt;
    co_return InventoryAndRuntime{std::move(inventoryResult), *runtimeResult};
  }
  struct SelectedCampaignTarget { PromptCacheScopeTargetBinding binding; std::optional<ExperimentState> active_state{}; };
  /** One target's parsed durable campaign pair, as read for campaign selection. */
  struct TargetCampaignRecord { PromptCacheScopeTargetBinding binding; std::optional<ExperimentState> state; std::optional<StoredEvidence> evidence; };
  rt::Task<TargetCampaignRecord> readTargetCampaign(std::shared_ptr<rt::Kv> kv, PromptCacheScopeTargetBinding binding) {
    rt::Promise<Entry> stateEntry(readEntry(kv, stateKey(binding))), evidenceEntry(readEntry(kv, evidenceKey(binding)));
    auto readEntries = co_await rt::all(stateEntry, evidenceEntry).wait();
    auto& [stateRow, evidenceRow] = readEntries;
    const auto state = stateRow.value.isNull() ? std::nullopt : parseState(stateRow.value);
    const auto evidence = evidenceRow.value.isNull() ? std::nullopt : parseEvidence(evidenceRow.value);
    if ((!stateRow.value.isNull() && !state) || (!evidenceRow.value.isNull() && !evidence))
      throw PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiment has malformed target-scoped durable state.");
    if (state && !activeStateEvidenceIsConsistent(*state, evidence))
      throw PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiment has inconsistent target-scoped durable evidence.");
    co_return TargetCampaignRecord{std::move(binding), state, evidence};
  }
  /** The public route resumes the sole active campaign or selects the first nonterminal probeable target. */
  rt::Task<SelectedCampaignTarget> selectCampaignTarget(std::shared_ptr<rt::Kv> kv, scope_targets::PromptCacheScopeTargetInventory inventory,
                                                       RuntimeBinding runtime) {
    std::vector<PromptCacheScopeTargetBinding> bindings;
    for (const auto& target : inventory.targets) if (auto binding = targetBindingFromInventory(inventory, target, runtime)) bindings.push_back(*binding);
    if (bindings.empty()) throw PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiment has no probeable Codex target in the current inventory.");
    std::vector<rt::Promise<TargetCampaignRecord>> pending;
    for (const auto& binding : bindings) pending.emplace_back(readTargetCampaign(kv, binding));
    auto records = co_await rt::all(std::move(pending)).wait();
    std::vector<const TargetCampaignRecord*> active;
    for (const auto& record : records) if (record.state && record.state->expires_at_ms > rt::dateNow()) active.push_back(&record);
    if (active.size() > 1) throw PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiment found more than one active provider campaign.");
    if (active.size() == 1) {
      const auto& record = *active.front(); const auto& state = *record.state;
      if (!sameTargetCore(state.target, record.binding))
        throw PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiment target changed while its campaign was active.");
      if (state.target.inventory_fingerprint != record.binding.inventory_fingerprint)
        throw PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiment inventory changed while its campaign was active.");
      co_return SelectedCampaignTarget{record.binding, state};
    }
    for (const auto& record : records) {
      const bool terminal = record.evidence && record.evidence->started_at_ms > 0 && sameTargetDefinition(record.evidence->target, record.binding) &&
                            record.evidence->outcome == "completed";
      if (!terminal) co_return SelectedCampaignTarget{record.binding};
    }
    throw PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiment has no nonterminal probeable Codex target in the current inventory.");
  }
  struct ResolvedPromptCacheScopeExperimentTelemetryBaseline { PromptCacheScopeTargetBinding target; telemetry_gate::PromptCacheTelemetryBaselineResult baseline; };
  rt::Task<ResolvedPromptCacheScopeExperimentTelemetryBaseline> resolvePromptCacheScopeExperimentTelemetryBaseline(
    PromptCacheScopeExperimentTelemetryBaselineTestOptions options) {
    std::shared_ptr<rt::Kv> kv;
    if (options.kv) kv = *options.kv;
    else { auto pending = ::uos::kv::getKv(); kv = co_await pending.wait(); }
    if (!kv) throw PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiments require Deno KV.");
    rt::Promise<std::optional<InventoryAndRuntime>> loading(loadInventoryAndRuntime(kv)); const auto loaded = co_await loading.wait();
    if (!loaded) throw PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiment requires a current full target inventory and runtime configuration.");
    auto selection = selectCampaignTarget(kv, loaded->inventory, loaded->runtime);
    rt::Promise<SelectedCampaignTarget> selecting(std::move(selection)); const auto selected = co_await selecting.wait();
    rt::Promise<ScopeTargetResolution> resolving(resolveBoundTarget(kv, selected.binding)); const auto resolved = co_await resolving.wait();
    const auto* current = std::get_if<ScopeTargetResolutionResolved>(&resolved);
    if (!current) throw PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiment target changed before its Stage 0 baseline could be checked.");
    telemetry_gate::PromptCacheTelemetryBaselineTarget baselineTarget{std::string(CODEX_CHATGPT_PROMPT_CACHE_TELEMETRY_PROVIDER), current->value.binding.model};
    telemetry_gate::PromptCacheTelemetryGateOptions baselineOptions{.kv = kv, .release = options.release};
    auto task = telemetry_gate::readPromptCacheTelemetryBaseline(baselineTarget, baselineOptions);
    rt::Promise<telemetry_gate::PromptCacheTelemetryBaselineResult> reading(std::move(task)); const auto baseline = co_await reading.wait();
    co_return ResolvedPromptCacheScopeExperimentTelemetryBaseline{current->value.binding, baseline};
  }
  bool hasActiveLease(const rt::KvValue& value) {
    if (!rt::kvIsObject(value)) return false;
    const auto lease = value["lease_until_ms"];
    return lease.isNumber() && isSafeNonNegativeInteger(lease.asNumber()) && lease.asNumber() > rt::dateNow();
  }
  struct CycleEntries { Entry state; Entry evidence; Entry campaign; Entry cycle; };
  rt::Task<CycleEntries> loadCycleEntries(std::shared_ptr<rt::Kv> kv, PromptCacheScopeTargetBinding target) {
    rt::Promise<Entry> state(readEntry(kv, stateKey(target))), evidence(readEntry(kv, evidenceKey(target))),
                       campaign(readEntry(kv, campaignLeaseKey(target))), cycle(readEntry(kv, cycleLeaseKey(target)));
    auto readEntries = co_await rt::all(state, evidence, campaign, cycle).wait();
    auto& [stateRow, evidenceRow, campaignRow, cycleRow] = readEntries;
    co_return CycleEntries{std::move(stateRow), std::move(evidenceRow), std::move(campaignRow), std::move(cycleRow)};
  }
  struct CycleRecords { std::optional<ExperimentState> state; std::optional<StoredEvidence> evidence; };
  CycleRecords readCycleRecords(const CycleEntries& entries) {
    const auto state = parseState(entries.state.value);
    const auto evidence = entries.evidence.value.isNull() ? std::nullopt : parseEvidence(entries.evidence.value);
    if (!entries.state.value.isNull() && !state) throw PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiment has malformed target-scoped state.");
    if (!entries.evidence.value.isNull() && !evidence) throw PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiment has malformed target-scoped evidence.");
    if (state && !activeStateEvidenceIsConsistent(*state, evidence))
      throw PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiment has inconsistent target-scoped durable evidence.");
    return {state, evidence};
  }
  struct CampaignState { std::string campaignOwner; ExperimentState state; };
  CampaignState resolveCampaignState(const rt::KvValue& campaignLeaseValue, const std::optional<ExperimentState>& activeState,
    const PromptCacheScopeTargetBinding& target, const std::string& authPoolVersionstamp, double now) {
    if (!activeState) {
      if (hasActiveLease(campaignLeaseValue)) throw PromptCacheScopeExperimentBusyError();
      const auto campaignOwner = rt::crypto::randomUUID();
      return {campaignOwner, {3, target, campaignOwner, now, now + PROMPT_CACHE_SCOPE_EXPERIMENT_SESSION_MS, authPoolVersionstamp, 1, {}}};
    }
    if (!sameTargetCore(activeState->target, target) || activeState->target.inventory_fingerprint != target.inventory_fingerprint)
      throw PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiment target changed while its campaign was active.");
    if (!ownsCampaignLease(campaignLeaseValue, *activeState)) {
      if (hasActiveLease(campaignLeaseValue)) throw PromptCacheScopeExperimentBusyError();
      throw PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiment lost its provider campaign lease.");
    }
    auto state = *activeState; state.target = target; return {state.campaign_owner, std::move(state)};
  }
}
bool sameTargetCore(const scope_experiment_model::PromptCacheScopeTargetBinding& left, const scope_experiment_model::PromptCacheScopeTargetBinding& right) {
  return left.id == right.id && sameLiteralField(left.provider, right.provider) && sameLiteralField(left.telemetry_provider, right.telemetry_provider) &&
    sameLiteralField(left.topology_kind, right.topology_kind) && left.model == right.model && sameLiteralField(left.probe_profile, right.probe_profile) &&
    left.capability_fingerprint == right.capability_fingerprint && left.auth_pool_identity_fingerprint == right.auth_pool_identity_fingerprint &&
    left.catalog_client_version == right.catalog_client_version;
}
rt::Task<ScopeTargetResolution> resolveBoundTarget(std::shared_ptr<rt::Kv> kv, PromptCacheScopeTargetBinding expected) {
  rt::Promise<std::optional<InventoryAndRuntime>> loading(loadInventoryAndRuntime(kv)); const auto loaded = co_await loading.wait();
  if (!loaded) co_return ScopeTargetResolutionInconclusive{"target_catalog_drift"};
  const auto current = std::find_if(loaded->inventory.targets.begin(), loaded->inventory.targets.end(), [&](const auto& target) { return target.id == expected.id; });
  const auto binding = current == loaded->inventory.targets.end() ? std::nullopt : targetBindingFromInventory(loaded->inventory, *current, loaded->runtime);
  if (!binding) co_return ScopeTargetResolutionInconclusive{"target_catalog_drift"};
  if (binding->auth_pool_identity_fingerprint != expected.auth_pool_identity_fingerprint) co_return ScopeTargetResolutionInconclusive{"auth_pool_drift"};
  if (!sameTargetCore(*binding, expected)) co_return ScopeTargetResolutionInconclusive{"target_catalog_drift"};
  if (binding->inventory_fingerprint != expected.inventory_fingerprint) co_return ScopeTargetResolutionInconclusive{"inventory_drift"};
  if (binding->auth_pool_versionstamp != expected.auth_pool_versionstamp) co_return ScopeTargetResolutionInconclusive{"auth_pool_drift"};
  co_return ScopeTargetResolutionResolved{{*binding}};
}
rt::Task<telemetry_gate::PromptCacheTelemetryBaselineResult> readPromptCacheScopeExperimentTelemetryBaseline() {
  return readPromptCacheScopeExperimentTelemetryBaseline(PromptCacheScopeExperimentTelemetryBaselineTestOptions{});
}
rt::Task<telemetry_gate::PromptCacheTelemetryBaselineResult> readPromptCacheScopeExperimentTelemetryBaseline(PromptCacheScopeExperimentTelemetryBaselineTestOptions options) {
  auto task = resolvePromptCacheScopeExperimentTelemetryBaseline(options);
  rt::Promise<ResolvedPromptCacheScopeExperimentTelemetryBaseline> pending(std::move(task)); const auto result = co_await pending.wait(); co_return result.baseline;
}
rt::Task<PromptCacheScopeExperimentTelemetryBaseline> assertPromptCacheScopeExperimentTelemetryBaseline() {
  return assertPromptCacheScopeExperimentTelemetryBaseline(PromptCacheScopeExperimentTelemetryBaselineTestOptions{});
}
rt::Task<PromptCacheScopeExperimentTelemetryBaseline> assertPromptCacheScopeExperimentTelemetryBaseline(PromptCacheScopeExperimentTelemetryBaselineTestOptions options) {
  auto task = resolvePromptCacheScopeExperimentTelemetryBaseline(options);
  rt::Promise<ResolvedPromptCacheScopeExperimentTelemetryBaseline> pending(std::move(task)); const auto result = co_await pending.wait();
  const auto route = std::find_if(result.baseline.routes.begin(), result.baseline.routes.end(), [](const auto& row) { return row.route == "responses"; });
  if (result.baseline.status != "eligible" || route == result.baseline.routes.end() || !route->completed_minimum_passed ||
      !route->reported_coverage_passed || !route->cache_write_reported_coverage_passed)
    throw PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiment requires a passing current-release Stage 0 telemetry baseline.");
  co_return PromptCacheScopeExperimentTelemetryBaseline{result.target};
}
rt::Task<AcquiredCycle> acquireCycle(std::shared_ptr<rt::Kv> kv, PromptCacheScopeTargetBinding target, std::string authPoolVersionstamp) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    rt::Promise<CycleEntries> loading(loadCycleEntries(kv, target)); const auto entries = co_await loading.wait();
    const auto existing = readCycleRecords(entries).state; const auto now = rt::dateNow();
    const auto activeState = existing && existing->expires_at_ms > now ? existing : std::nullopt;
    const bool resettingPriorSession = !activeState && (!entries.state.value.isNull() || !entries.evidence.value.isNull());
    if (hasActiveLease(entries.cycle.value)) throw PromptCacheScopeExperimentBusyError();
    auto campaign = resolveCampaignState(entries.campaign.value, activeState, target, authPoolVersionstamp, now);
    const auto cycleOwner = rt::crypto::randomUUID();
    const CampaignLease campaignLease{campaign.campaignOwner, campaign.state.target.id, campaign.state.target.inventory_fingerprint, campaign.state.expires_at_ms};
    const ExperimentLease cycleLease{cycleOwner, now + PROMPT_CACHE_SCOPE_EXPERIMENT_LEASE_MS};
    auto atomic = kv->atomic();
    atomic->check(entries.state).check(entries.evidence).check(entries.campaign).check(entries.cycle)
      .set(cycleLeaseKey(target), rt::KvValue(json::value_from(cycleLease)), {.expireIn = PROMPT_CACHE_SCOPE_EXPERIMENT_LEASE_MS * 2})
      .set(campaignLeaseKey(target), rt::KvValue(json::value_from(campaignLease)), {.expireIn = rt::Math::max(1, campaign.state.expires_at_ms - now)})
      .set(stateKey(target), rt::KvValue(json::value_from(campaign.state)), {.expireIn = rt::Math::max(1, campaign.state.expires_at_ms - now)});
    if (resettingPriorSession) atomic->delete_(evidenceKey(target));
    rt::Promise<rt::KvCommitResult> committing(atomic->commit()); const auto committed = co_await committing.wait();
    if (committed.ok) co_return AcquiredCycle{std::move(campaign.state), cycleOwner};
  }
  throw PromptCacheScopeExperimentBusyError();
}
rt::Task<void> renewLease(std::shared_ptr<rt::Kv> kv, ExperimentState state, std::string cycleOwner) {
  const auto cycleKey = cycleLeaseKey(state.target), campaignKey = campaignLeaseKey(state.target);
  for (int attempt = 0; attempt < 3; ++attempt) {
    rt::Promise<Entry> cycle(readEntry(kv, cycleKey)), campaign(readEntry(kv, campaignKey));
    const auto readEntries = co_await rt::all(cycle, campaign).wait();
    const auto& [cycleEntry, campaignEntry] = readEntries;
    if (!ownsLease(cycleEntry.value, cycleOwner) || !ownsCampaignLease(campaignEntry.value, state))
      throw PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment lost its lease.");
    const auto now = rt::dateNow();
    const ExperimentLease cycleLease{cycleOwner, now + PROMPT_CACHE_SCOPE_EXPERIMENT_LEASE_MS};
    const CampaignLease campaignLease{state.campaign_owner, state.target.id, state.target.inventory_fingerprint, state.expires_at_ms};
    auto atomic = kv->atomic(); atomic->check(cycleEntry).check(campaignEntry)
      .set(cycleKey, rt::KvValue(json::value_from(cycleLease)), {.expireIn = PROMPT_CACHE_SCOPE_EXPERIMENT_LEASE_MS * 2})
      .set(campaignKey, rt::KvValue(json::value_from(campaignLease)), {.expireIn = rt::Math::max(1, state.expires_at_ms - now)});
    rt::Promise<rt::KvCommitResult> committing(atomic->commit()); const auto commit = co_await committing.wait();
    if (commit.ok) co_return;
  }
  throw PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment could not renew its lease.");
}
}
