#include "uos/codex/manual_reset.hpp"
#include "uos/codex/manual_reset_state.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/capacity_routing.hpp"
#include "uos/codex/banked_reset_submission.hpp"
#include "uos/codex/routing_state.hpp"
#include "uos/authority/native_codex.hpp"
#include "uos/authority/native_codex_capacity.hpp"
#include "uos/provider/capacity_parse.hpp"
#include "uos/provider/selection.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/utils.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include <algorithm>
#include <vector>
namespace uos::codex::manual_reset {
  namespace {
    namespace capacity = ::uos::authority::native_codex_capacity;
    namespace contracts = ::uos::authority::native_codex_contracts;
    namespace selection = ::uos::provider::selection;
    namespace parsing = ::uos::provider::capacity_parse;
    using manual_reset_state::ManualIntent;
    using manual_reset_state::intentKey;
    [[noreturn]] void fail(double status, std::string code_, std::string message, std::optional<std::string> resetStatus = std::nullopt) { throw ManualResetError(status, std::move(code_), std::move(message), std::move(resetStatus)); }
    bool same(const json::value& left, const json::value& right) { return rt::jsonStringify(left) == rt::jsonStringify(right); }
    std::string day(double now) { return rt::slice(rt::Date(now).toISOString(), 0, 10); }
    banked_reset::CodexBankedResetConfig hardPolicy(const std::function<banked_reset::CodexBankedResetConfig()>& read) {
      const auto current = read(); const auto reason = banked_reset::policyReason(current, true);
      if (reason) fail(409, *reason, *reason == "feature_disabled" || *reason == "mode_disabled" ? "Banked resets are globally disabled." : "Banked reset limits must permit exactly one redemption per account per day.");
      return current;
    }
    bool enabledTarget(const rt::KvValue& value, const std::string& hash) {
      const auto normalized = selection::normalizeProviderSelection(value);
      if (!value.isNull() && !normalized) return false;
      const auto eligibility = selection::codexAccountEligibility(normalized);
      if (std::holds_alternative<selection::CodexAccountEligibilityAll>(eligibility)) return true;
      const auto* only = std::get_if<selection::CodexAccountEligibilityOnly>(&eligibility);
      return only && std::find(only->hashes.begin(), only->hashes.end(), hash) != only->hashes.end();
    }
    struct Target { ::uos::types::CodexAuthPoolState pool; std::size_t slot; ::uos::types::CodexAuthState auth; capacity_routing::CodexRoutingAccountIdentity identity; };
    rt::Task<Target> currentTarget(rt::KvValue value, std::string hash) {
      const auto pool = auth::parseCodexAuthPool(rt::storedJsonView(value));
      if (!pool) fail(409, "codex_account_changed", "Subscription changed. Reload Providers.");
      for (std::size_t slot = 0; slot < pool->accounts.size(); ++slot) {
        const auto account = pool->accounts[slot];
        rt::Promise hashing(utils::sha256Hex(account.account_id)); const auto accountHash = co_await hashing.wait();
        if (accountHash != hash) continue;
        rt::Promise identifying(capacity_routing::routingAccountIdentity(account)); const auto identity = co_await identifying.wait();
        co_return Target{*pool, slot, account, identity};
      }
      fail(409, "codex_account_changed", "Subscription changed. Reload Providers.");
    }
    std::optional<contracts::CodexCapacityRoutingWindow> routingWindow(const std::optional<provider::capacity_contract::ProviderCapacityWindow>& window) {
      if (!window) return std::nullopt;
      return contracts::CodexCapacityRoutingWindow{window->limit_window_seconds, window->used_percent, window->reset_at_ms};
    }
    rt::Task<contracts::CodexCapacityRoutingObservation> usageObservation(Target target, ManualResetDependencies dependencies, rt::AbortSignal signal) {
      const auto reload = dependencies.reloadConfig ? dependencies.reloadConfig : std::function<banked_reset::CodexBankedResetConfig()>(banked_reset::loadCodexBankedResetConfig);
      hardPolicy(reload);
      const auto fetcher = dependencies.fetcher ? dependencies.fetcher : rt::globalFetch();
      const auto timeout = rt::AbortSignal::timeout(8'000); const auto combined = rt::AbortSignal::any({signal, timeout});
      rt::RequestInit init; init.headers = rt::Headers{{"Authorization", "Bearer " + target.auth.access_token}, {"ChatGPT-Account-ID", target.auth.account_id}, {"Accept", "application/json"}}; init.redirect = "manual"; init.signal = combined;
      rt::Promise fetching(fetcher(parsing::codexUsageUrl(), init)); auto response = co_await fetching.wait();
      if (!response.ok()) { if (auto body = response.body()) rt::spawn(body->cancel(), rt::ignoreRejection); fail(409, "codex_quota_unavailable", "Current subscription quota could not be verified."); }
      rt::Promise reading(response.json()); const auto payload = co_await reading.wait();
      const auto usage = parsing::parseCodexUsage(payload);
      if (!usage || (!usage->primary && !usage->secondary)) fail(409, "codex_quota_unavailable", "Current subscription quota is unavailable.");
      const double at = dependencies.now ? dependencies.now() : rt::dateNow();
      contracts::CodexCapacityRoutingObservation result; result.slot = static_cast<double>(target.slot); result.account_id_hash = target.identity.accountIdHash; result.state = "available"; result.source_observed_at_ms = at; result.snapshot_at_ms = at;
      result.windows = {routingWindow(usage->primary), routingWindow(usage->secondary)};
      for (const auto& limit : parsing::additionalRateLimitsForRouting(usage->additional_rate_limits, at)) result.additional_rate_limits.push_back({limit.limit_name, limit.metered_feature, {routingWindow(limit.windows.primary), routingWindow(limit.windows.secondary)}});
      co_return result;
    }
    double resetDeadline(const contracts::CodexCapacityRoutingObservation& observation, double now) {
      std::vector<contracts::CodexCapacityRoutingWindow> windows;
      for (const auto& window : {observation.windows.primary, observation.windows.secondary}) if (window && window->reset_at_ms && *window->reset_at_ms > now) windows.push_back(*window);
      std::vector<double> exhausted, deadlines;
      for (const auto& window : windows) { deadlines.push_back(*window.reset_at_ms); if (window.used_percent == 100) exhausted.push_back(*window.reset_at_ms); }
      if (deadlines.empty()) fail(409, "codex_quota_identity_unavailable", "A current quota window is required before using a reset.");
      return rt::Math::max(exhausted.empty() ? std::span<const double>(deadlines) : std::span<const double>(exhausted));
    }
    rt::Task<rt::KvEntryMaybe<>> readEntry(std::shared_ptr<rt::Kv> kv, rt::KvKey key) {
      auto read = kv->get(std::move(key), {.consistency = rt::KvConsistencyLevel::strong});
      return rt::detail::keepAlive(std::move(kv), std::move(read));
    }
    rt::Task<rt::KvEntryMaybe<>> readManualDailyBudget(std::shared_ptr<rt::Kv> kv, std::string hash, double now) {
      rt::Promise reading(readEntry(kv, banked_reset::codexResetAccountDailyKey(hash, day(now)))); const auto budget = co_await reading.wait();
      const auto value = rt::storedJsonView(budget.value);
      if (!budget.value.isNull() && (!utils::isRecord(value) || rt::getNumber(value, "submission_count") != 0)) fail(429, "account_day_limit_reached", "This account has already used its one banked reset for today.");
      co_return budget;
    }
    struct ResolvedIntent { rt::KvEntryMaybe<> entry; ManualIntent intent; json::value value; };
    rt::Task<ResolvedIntent> resolveIntent(std::shared_ptr<rt::Kv> kv, std::string hash, Target target, ManualResetDependencies dependencies, rt::AbortSignal signal, rt::KvEntryMaybe<> routingEntry, rt::KvEntryMaybe<> poolEntry, rt::KvEntryMaybe<> selectionEntry) {
      const auto clock = dependencies.now ? dependencies.now : std::function<double()>(rt::dateNow); const auto key = intentKey(hash);
      rt::Promise reading(readEntry(kv, key)); const auto entry = co_await reading.wait();
      const auto stored = rt::storedJsonView(entry.value); const auto existing = manual_reset_state::parseManualResetIntent(stored);
      if (!entry.value.isNull() && !existing) fail(409, "codex_reset_intent_invalid", "Saved reset recovery requires operator inspection.");
      if (existing && !existing->promoted_at_ms) {
        if (existing->credential_version != target.identity.credentialVersion) fail(409, "codex_account_changed", "Subscription credentials changed while reset recovery was pending.");
        co_return ResolvedIntent{entry, *existing, stored};
      }
      if (existing && day(existing->created_at_ms) == day(clock())) fail(409, "codex_reset_already_completed", "This account's banked reset was already completed today.", "verified");
      rt::Promise budgetRead(readManualDailyBudget(kv, hash, clock())); const auto budget = co_await budgetRead.wait();
      rt::Promise observing(usageObservation(target, dependencies, signal)); const auto observation = co_await observing.wait();
      const auto raw = routingEntry.value.isNull() ? std::nullopt : contracts::parseCodexAccountRoutingState(rt::storedJsonView(routingEntry.value));
      if (!routingEntry.value.isNull() && !raw) fail(409, "codex_routing_unavailable", "Subscription routing state is unavailable.");
      rt::Promise normalizing(capacity_routing::normalizeRoutingState(raw, target.pool, clock(), true)); const auto routing = co_await normalizing.wait();
      const auto& slot = routing.slots[target.slot];
      if (slot.invalid_credential_version == target.identity.credentialVersion) fail(409, "codex_auth_invalid", "This subscription requires credential recovery before using a reset.");
      const double deadline = resetDeadline(observation, clock());
      if (existing && existing->quota_reset_at_ms == deadline) fail(409, "codex_reset_already_completed", "This quota window's reset action was already completed.", "verified");
      const ManualIntent intent{.v = 1, .account_id_hash = hash, .credential_version = target.identity.credentialVersion, .quota_reset_at_ms = deadline, .routing_generation = slot.generation, .created_at_ms = clock(), .promoted_at_ms = std::nullopt, .quota_signal_at_ms = slot.quota_signal_observed_at_ms};
      json::object value{{"v", 1}, {"account_id_hash", hash}, {"credential_version", intent.credential_version}, {"quota_reset_at_ms", rt::makeNumber(deadline)}, {"routing_generation", rt::makeNumber(slot.generation)}};
      value["quota_signal_at_ms"] = slot.quota_signal_observed_at_ms ? rt::makeNumber(*slot.quota_signal_observed_at_ms) : json::value(nullptr); value["created_at_ms"] = rt::makeNumber(intent.created_at_ms); value["promoted_at_ms"] = nullptr;
      const rt::KvValue storedIntent(value); auto operation = kv->atomic(); operation->check(entry).check(poolEntry).check(routingEntry).check(selectionEntry).check(budget).set(key, storedIntent);
      const auto committed = co_await operation->commit();
      if (!committed.ok) fail(409, "codex_reset_concurrent_update", "Subscription changed. Reload and retry the action.");
      co_return ResolvedIntent{{.key = key, .value = storedIntent, .versionstamp = committed.versionstamp}, intent, value};
    }
    std::vector<std::string> promotionModels(const contracts::CodexCapacityRoutingObservation& observation) {
      std::vector<std::string> models;
      if (capacity::capacityHeadroomForObservation(observation, std::nullopt).value_or(0) > 0) models.push_back("standard");
      for (const auto& limit : observation.additional_rate_limits) { const auto kind = capacity::quotaClass(std::optional<std::string_view>(limit.limit_name)); if (kind != "standard" && kind != "unknown" && capacity::capacityHeadroomForObservation(observation, limit.limit_name).value_or(0) > 0 && std::find(models.begin(), models.end(), limit.limit_name) == models.end()) models.push_back(limit.limit_name); }
      return models;
    }
    rt::Task<Target> validatePromotionTarget(rt::KvEntryMaybe<> pool, rt::KvEntryMaybe<> selected, rt::KvEntryMaybe<> pending, rt::KvEntryMaybe<> redemption, std::string hash, Target target, ResolvedIntent resolved) {
      rt::Promise resolving(currentTarget(pool.value, hash)); const auto current = co_await resolving.wait();
      if (current.identity.credentialVersion != resolved.intent.credential_version || current.auth.account_id != target.auth.account_id) fail(409, "codex_account_changed", "Reset verified, but subscription credentials changed before activation.", "verified");
      if (!enabledTarget(selected.value, hash)) fail(409, "codex_provider_disabled", "Reset verified, but this subscription is disabled in Active providers.", "verified");
      const auto pendingValue = rt::storedJsonView(pending.value);
      if (!manual_reset_state::parseManualResetIntent(pendingValue) || !same(pendingValue, resolved.value)) fail(409, "codex_reset_already_completed", "This reset action was already completed or superseded.", "verified");
      const auto record = banked_reset::parseCodexResetRedemptionRecord(rt::storedJsonView(redemption.value));
      if (!record || ::uos::types::toString(record->state) != "verified") fail(409, "codex_reset_unverified", "Reset verification is pending.", "pending");
      co_return current;
    }
    struct PromotionState { std::optional<contracts::CodexActiveAccountSelection> active; std::optional<contracts::CodexAccountRoutingState> raw; };
    PromotionState parsePromotionState(const rt::KvValue& activeValue, const rt::KvValue& routingValue) {
      const auto active = activeValue.isNull() ? std::nullopt : contracts::parseCodexActiveAccountSelection(rt::storedJsonView(activeValue));
      const auto raw = routingValue.isNull() ? std::nullopt : contracts::parseCodexAccountRoutingState(rt::storedJsonView(routingValue));
      if ((!activeValue.isNull() && !active) || (!routingValue.isNull() && !raw)) fail(409, "codex_routing_unavailable", "Reset verified, but routing could not be activated.", "verified");
      return {active, raw};
    }
    rt::Task<contracts::CodexAccountRoutingState> recoverPromotionRouting(std::optional<contracts::CodexAccountRoutingState> raw, Target current, contracts::CodexCapacityRoutingObservation observation, std::vector<std::string> models, std::function<double()> clock) {
      rt::Promise normalizing(capacity_routing::normalizeRoutingState(raw, current.pool, clock(), true)); auto routing = co_await normalizing.wait();
      const auto slot = routing.slots[current.slot];
      if (slot.invalid_credential_version == current.identity.credentialVersion || (slot.probe_lease ? slot.probe_lease->expires_at_ms : 0) > clock() || slot.quota_signal_observed_at_ms.value_or(-1) >= observation.snapshot_at_ms) fail(409, "codex_reset_concurrent_update", "Reset verified; a newer account transition must settle before activation.", "verified");
      observation.slot = static_cast<double>(current.slot);
      for (const auto& model : models) { const auto next = capacity::applyCapacityObservation(routing, observation, clock(), model, clock()); if (next) routing = *next; }
      for (const auto& model : models) { const auto block = capacity::quotaBlockForClass(routing.slots[current.slot], capacity::quotaClass(std::optional<std::string_view>(model))); if (block && block->blocked_until_ms > clock()) fail(409, "codex_reset_concurrent_update", "Reset verified; a newer quota fence remains authoritative.", "verified"); }
      co_return routing;
    }
    rt::Task<ManualResetResult> promoteVerified(std::shared_ptr<rt::Kv> kv, std::string hash, Target target, ResolvedIntent resolved, rt::KvKey redemptionKey, contracts::CodexCapacityRoutingObservation observation, std::function<double()> clock, std::function<banked_reset::CodexBankedResetConfig()> reloadConfig) {
      const auto models = promotionModels(observation);
      if (models.empty()) fail(409, "codex_reset_propagation_pending", "Reset verified; restored quota is not visible yet. Check again without consuming another reset.", "verified");
      for (int attempt = 0; attempt < 3; ++attempt) {
        hardPolicy(reloadConfig);
        std::vector<rt::Promise<rt::KvEntryMaybe<>>> reads;
        for (const auto& key : std::vector<rt::KvKey>{contracts::CODEX_AUTH_POOL_KV_KEY, contracts::CODEX_ACCOUNT_ROUTING_KV_KEY, contracts::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, contracts::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY, selection::PROVIDER_SELECTION_KV_KEY, intentKey(hash), redemptionKey}) reads.emplace_back(readEntry(kv, key));
        const auto entries = co_await rt::all(std::move(reads)).wait();
        rt::Promise validating(validatePromotionTarget(entries[0], entries[4], entries[5], entries[6], hash, target, resolved)); const auto current = co_await validating.wait();
        const auto state = parsePromotionState(entries[2].value, entries[1].value);
        rt::Promise recovering(recoverPromotionRouting(state.raw, current, observation, models, clock)); const auto routing = co_await recovering.wait();
        const auto durable = capacity::parseStoredCapacityObservationStore(rt::storedJsonView(entries[3].value));
        for (const auto& item : durable) if (item.account_id_hash == observation.account_id_hash && item.snapshot_at_ms > observation.snapshot_at_ms) fail(409, "codex_reset_concurrent_update", "Reset verified; newer quota evidence must be checked before activation.", "verified");
        auto currentObservation = observation; currentObservation.slot = static_cast<double>(current.slot);
        contracts::CodexRoutingAccount account; account.slot = static_cast<double>(current.slot); account.accountIdHash = current.identity.accountIdHash; account.credentialVersion = current.identity.credentialVersion; account.routingGeneration = routing.slots[current.slot].generation;
        const auto active = authority::native_codex::activeSelectionForAccount(account, entries[0].versionstamp.value_or(""), (state.active ? state.active->generation : 0) + 1, clock(), "manual_reset");
        if (!active) fail(409, "codex_routing_unavailable", "Reset verified, but account activation could not be prepared.", "verified");
        const std::vector<contracts::CodexCapacityRoutingObservation> incoming{currentObservation};
        const auto observations = capacity::planCapacityObservationStore(entries[3].versionstamp, durable, incoming, clock()).writes.capacity;
        hardPolicy(reloadConfig);
        const rt::KvValue routingValue(contracts::toJson(routing)); const rt::KvValue capacityValue(capacity::toJson(observations)); const rt::KvValue activeValue(contracts::toJson(*active));
        auto intentValue = resolved.value; intentValue.as_object()["promoted_at_ms"] = rt::makeNumber(clock()); const rt::KvValue promoted(intentValue);
        auto operation = kv->atomic(); for (const auto& entry : entries) operation->check(entry);
        operation->set(contracts::CODEX_ACCOUNT_ROUTING_KV_KEY, routingValue).set(contracts::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY, capacityValue).set(contracts::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, activeValue).set(intentKey(hash), promoted);
        const auto committed = co_await operation->commit();
        if (!committed.ok) continue;
        routing_state::setRoutingStateCache(routing, committed.versionstamp);
        co_return ManualResetResult{"verified", hash};
      }
      fail(409, "codex_reset_concurrent_update", "Reset verified; account activation conflicted. Check again without consuming another reset.", "verified");
    }
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const ManualResetResult& value) { out = rt::record::Writer().required("reset_status", value.reset_status).required("active_account_id_hash", value.active_account_id_hash).done(); }
  ManualResetResult tag_invoke(const json::value_to_tag<ManualResetResult>&, const json::value& value) { const rt::record::Reader in(value, "ManualResetResult"); return {in.required<std::string>("reset_status"), in.required<std::string>("active_account_id_hash")}; }
  // Only the authenticated admin route constructs this explicit manual authorization. No inference is issued.
  rt::Task<ManualResetResult> consumeManualCodexReset(std::string hash) { return consumeManualCodexReset(std::move(hash), {}); }
  rt::Task<ManualResetResult> consumeManualCodexReset(std::string hash, ManualResetDependencies dependencies) { return consumeManualCodexReset(std::move(hash), std::move(dependencies), rt::AbortController().signal()); }
  rt::Task<ManualResetResult> consumeManualCodexReset(std::string hash, ManualResetDependencies dependencies, rt::AbortSignal signal) {
    const auto reload = dependencies.reloadConfig ? dependencies.reloadConfig : std::function<banked_reset::CodexBankedResetConfig()>(banked_reset::loadCodexBankedResetConfig);
    const auto configured = hardPolicy(reload); // Must precede every provider read, including usage and inventory.
    const auto clock = dependencies.now ? dependencies.now : std::function<double()>(rt::dateNow);
    std::shared_ptr<rt::Kv> kv;
    if (dependencies.kv) kv = *dependencies.kv; else { auto getting = ::uos::kv::getKv(); kv = co_await getting.wait(); }
    if (!kv) fail(503, "codex_reset_storage_unavailable", "Banked reset storage is unavailable.");
    std::vector<rt::Promise<rt::KvEntryMaybe<>>> reads;
    reads.emplace_back(readEntry(kv, contracts::CODEX_AUTH_POOL_KV_KEY)); reads.emplace_back(readEntry(kv, selection::PROVIDER_SELECTION_KV_KEY)); reads.emplace_back(readEntry(kv, contracts::CODEX_ACCOUNT_ROUTING_KV_KEY));
    const auto entries = co_await rt::all(std::move(reads)).wait();
    rt::Promise targeting(currentTarget(entries[0].value, hash)); const auto target = co_await targeting.wait();
    if (!enabledTarget(entries[1].value, hash)) fail(409, "codex_provider_disabled", "Enable this subscription in Active providers before using a reset.");
    const auto expiry = auth::getJwtExpMs(std::string_view(target.auth.access_token));
    if (expiry && *expiry <= clock()) fail(409, "codex_auth_invalid", "This subscription's credential expired. Restore its account authorization first.");
    rt::Promise resolving(resolveIntent(kv, hash, target, dependencies, signal, entries[2], entries[0], entries[1])); const auto resolved = co_await resolving.wait();
    auto provider = dependencies.provider ? dependencies.provider(target.auth) : nullptr;
    if (!provider) { const banked_reset_provider::UpstreamCodexUsageResetProviderOptions options{.codexBaseUrl = config::config.codexBaseUrl, .accountId = target.auth.account_id, .accessToken = target.auth.access_token, .userAgent = "codex_cli_rs/0.100.0 (ai.ubq.fi)", .fetch = dependencies.fetcher}; provider = banked_reset_provider::createUpstreamCodexUsageResetProvider(options); }
    banked_reset::CodexBankedResetCandidate candidate{}; candidate.accountId = target.auth.account_id; candidate.credentialVersion = resolved.intent.credential_version; candidate.quotaResetAtMs = resolved.intent.quota_reset_at_ms; candidate.routingGeneration = resolved.intent.routing_generation; candidate.authorization = banked_reset::CodexBankedResetCandidate::Authorization{hash}; candidate.signal = signal;
    candidate.fences.push_back({contracts::CODEX_AUTH_POOL_KV_KEY, [target](const json::value& value) { const auto pool = auth::parseCodexAuthPool(value); if (!pool) return false; for (const auto& account : pool->accounts) if (account.account_id == target.auth.account_id && account.access_token == target.auth.access_token && account.refresh_token == target.auth.refresh_token) return true; return false; }});
    const auto selectionValue = rt::storedJsonView(entries[1].value);
    candidate.fences.push_back({selection::PROVIDER_SELECTION_KV_KEY, [selectionValue, hash](const json::value& value) { return same(value, selectionValue) && enabledTarget(rt::KvValue(value), hash); }});
    candidate.fences.push_back({intentKey(hash), [resolved, clock](const json::value& value) { return same(value, resolved.value) && clock() - resolved.intent.created_at_ms < 30'000; }});
    candidate.fences.push_back({contracts::CODEX_ACCOUNT_ROUTING_KV_KEY, [target, resolved](const json::value& value) {
      const auto state = value.is_null() ? std::nullopt : contracts::parseCodexAccountRoutingState(value);
      if (value.is_null()) return resolved.intent.routing_generation == 0;
      if (!state || target.slot >= state->slots.size()) return false;
      const auto& slot = state->slots[target.slot]; return slot.account_id_hash == target.identity.accountIdHash && slot.credential_version == resolved.intent.credential_version && slot.generation == resolved.intent.routing_generation && slot.quota_signal_observed_at_ms == resolved.intent.quota_signal_at_ms;
    }});
    const banked_reset::CodexBankedResetDependencies resetDependencies{.config = configured, .reloadConfig = reload, .provider = provider, .kv = kv, .now = clock};
    rt::Promise attempting(banked_reset_submission::attemptCodexBankedReset(candidate, resetDependencies)); const auto outcome = co_await attempting.wait();
    if (outcome.kind != "verified" || !outcome.record || !outcome.quotaGeneration || outcome.quotaGeneration->empty()) fail(outcome.kind == "pending" ? 409 : 422, outcome.reason, "Banked reset was not completed: " + rt::replaceAll(outcome.reason, "_", " ") + ".", outcome.kind == "pending" ? std::optional<std::string>("pending") : std::nullopt);
    contracts::CodexCapacityRoutingObservation observation;
    try { hardPolicy(reload); rt::Promise observing(usageObservation(target, dependencies, signal)); observation = co_await observing.wait(); }
    catch (...) { fail(409, "codex_reset_usage_pending", "Reset verified; current quota verification is pending. Check again without consuming another reset.", "verified"); }
    try {
      rt::Promise promoting(promoteVerified(kv, hash, target, resolved, banked_reset::codexResetRedemptionKey(hash, *outcome.quotaGeneration), observation, clock, reload)); const auto result = co_await promoting.wait(); co_return result;
    } catch (const ManualResetError& error) { throw ManualResetError(error.status, error.code, error.message, "verified"); }
    catch (...) { fail(503, "codex_reset_activation_pending", "Reset verified; account activation is pending. Check again without consuming another reset.", "verified"); }
  }
}
