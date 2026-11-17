#include "uos/cache/scope_experiment_model.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/experiment.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/responses_stream.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace uos::cache::scope_experiment_model {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
    using codex::experiment::CodexCacheScopeExperimentError;
    constexpr double CACHE_SCOPE_MIN_REUSABLE_TOKENS = 2'000;
    constexpr std::array<std::string_view, 16> INCONCLUSIVE_REASONS{
      "auth_pool_drift", "capability_changed", "cycle_disagreement", "effective_model_drift", "incomplete_telemetry", "invalid_cache_signal",
      "invalid_input_size", "inventory_drift", "lease_lost", "model_drift", "promotion_conflict", "refresh_unchanged", "runtime_drift",
      "session_expired", "slot_drift", "target_catalog_drift"};

    bool hasOwn(const json::value* value, std::string_view key) { return value && value->is_object() && rt::hasOwn(value->as_object(), key); }
    bool hasOnlyKeys(const json::value* value, std::initializer_list<std::string_view> allowed) {
      if (!value || !value->is_object()) return false;
      for (const auto& key : rt::objectKeys(value->as_object()))
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) return false;
      return true;
    }
    bool hasExactKeys(const json::value* value, std::initializer_list<std::string_view> expected) {
      if (!value || !value->is_object() || value->as_object().size() != expected.size()) return false;
      return std::all_of(expected.begin(), expected.end(), [&](auto key) { return hasOwn(value, key); });
    }
    std::optional<std::string> trimmedString(const json::value* value) {
      const auto text = rt::getString(value);
      if (!text) return std::nullopt;
      return rt::trim(*text);
    }
    bool equalString(const json::value* value, std::string_view expected) {
      const auto text = rt::getString(value);
      return text && *text == expected;
    }
    bool equalNumber(const json::value* value, double expected) { return value && rt::isNumber(*value) && rt::toNumber(value) == expected; }
    bool isConcreteObservation(const json::value* value) {
      if (!hasExactKeys(value, {"probe_profile", "account_slots", "token_refresh", "conversation_id", "effective_model"})) return false;
      const auto model = trimmedString(rt::get(value, "effective_model"));
      return equalString(rt::get(value, "probe_profile"), models::codex_models::PROMPT_CACHE_SCOPE_PROBE_PROFILE) &&
        (equalString(rt::get(value, "account_slots"), "shared") || equalString(rt::get(value, "account_slots"), "account_scoped")) &&
        (equalString(rt::get(value, "token_refresh"), "preserved") || equalString(rt::get(value, "token_refresh"), "changed")) &&
        (equalString(rt::get(value, "conversation_id"), "independent") || equalString(rt::get(value, "conversation_id"), "scoped")) && model && !model->empty();
    }
    bool sameObservation(const ConcreteScopeObservation& left, const ConcreteScopeObservation& right) {
      return sameLiteralField(left.probe_profile, right.probe_profile) && left.account_slots == right.account_slots &&
        left.token_refresh == right.token_refresh && left.conversation_id == right.conversation_id && left.effective_model == right.effective_model;
    }
    bool classificationEvidenceAgrees(const std::vector<CycleEvidence>& cycles) {
      if (cycles.empty() || !cycles.front().classification) return false;
      const auto& first = *cycles.front().classification;
      return std::all_of(cycles.begin(), cycles.end(), [&](const auto& cycle) { return cycle.classification && sameObservation(*cycle.classification, first); });
    }
    rt::KvKey targetKeyParts(const PromptCacheScopeTargetBinding& target) {
      return rt::KvKey{target.provider, target.telemetry_provider, target.topology_kind, target.probe_profile, target.model};
    }
    void cancelResponse(const rt::Response& response) {
      try {
        if (const auto body = response.body()) {
          auto cancellation = body->cancelPromise();
          (void)cancellation.catch_([](rt::Reason) {});
        }
      } catch (...) {
        // Cancellation is diagnostic cleanup only.
      }
    }
    std::optional<NormalizedUsage> rawUsageSample(const json::value* value) {
      if (!value || !value->is_object()) return std::nullopt;
      const auto* details = rt::get(value, "input_tokens_details");
      if (!details || !details->is_object()) return std::nullopt;
      const auto* inputTokens = rt::get(value, "input_tokens");
      const auto* cachedTokens = rt::get(details, "cached_tokens");
      const auto* cacheWriteTokens = rt::get(details, "cache_write_tokens");
      const auto* outputTokens = rt::get(value, "output_tokens");
      const auto* totalTokens = rt::get(value, "total_tokens");
      if (!isSafeNonNegativeInteger(inputTokens) || !isSafeNonNegativeInteger(cachedTokens) || !isSafeNonNegativeInteger(cacheWriteTokens) ||
          !isSafeNonNegativeInteger(outputTokens) || !isSafeNonNegativeInteger(totalTokens)) return std::nullopt;
      return NormalizedUsage{rt::toNumber(inputTokens), rt::toNumber(cachedTokens), rt::toNumber(cacheWriteTokens), rt::toNumber(outputTokens), rt::toNumber(totalTokens)};
    }
    bool sameUsage(const NormalizedUsage& left, const NormalizedUsage& right) {
      return left.input_tokens == right.input_tokens && left.cached_tokens == right.cached_tokens && left.cache_write_tokens == right.cache_write_tokens &&
        left.output_tokens == right.output_tokens && left.total_tokens == right.total_tokens;
    }
    bool isPrefixScaleCounter(double tokens, double inputTokens) { return tokens == 0 || (tokens >= CACHE_SCOPE_MIN_REUSABLE_TOKENS && tokens <= inputTokens); }
    std::optional<CacheSignal> cacheSignal(const NormalizedUsage& usage) {
      if (!isPrefixScaleCounter(usage.cached_tokens, usage.input_tokens) || !isPrefixScaleCounter(usage.cache_write_tokens, usage.input_tokens)) return std::nullopt;
      // Positive cache reads remain evidence even alongside a write at a later breakpoint.
      if (usage.cached_tokens > 0) return "read";
      if (usage.cache_write_tokens > 0) return "write";
      return std::nullopt;
    }
    std::optional<std::string> triStateFromSignal(const CacheSignal& signal, std::string_view whenRead, std::string_view whenWrite) {
      if (signal == "read") return std::string(whenRead);
      if (signal == "write") return std::string(whenWrite);
      return std::nullopt;
    }
    struct ReportedUsage { NormalizedUsage raw; NormalizedUsage usage; };
    std::optional<ReportedUsage> reportedUsage(const json::value* value) {
      const auto raw = rawUsageSample(value);
      const auto normalized = openai_telemetry::extractUsageTokens(value);
      if (!raw || !normalized || normalized->status != "reported" || !normalized->inputTokens || !normalized->cachedInputTokens ||
          !normalized->cacheWriteInputTokens || !normalized->outputTokens || !normalized->totalTokens) return std::nullopt;
      const NormalizedUsage usage{*normalized->inputTokens, *normalized->cachedInputTokens, *normalized->cacheWriteInputTokens,
                                  *normalized->outputTokens, *normalized->totalTokens};
      return sameUsage(*raw, usage) ? std::optional<ReportedUsage>(ReportedUsage{*raw, usage}) : std::nullopt;
    }
    ReadSampleResult sampleFromCompletedResponse(const json::object& response, double expectedSlot, const std::string& expectedModel, double startedAtMs) {
      if (trimmedString(rt::get(response, "model")) != expectedModel) return ReadSampleResultInconclusive{"effective_model_drift"};
      const auto reported = reportedUsage(rt::get(response, "usage"));
      if (!reported) return ReadSampleResultInconclusive{"incomplete_telemetry"};
      const auto& usage = reported->usage;
      if (usage.input_tokens < CACHE_SCOPE_MIN_REUSABLE_TOKENS || usage.input_tokens > 4'000) return ReadSampleResultInconclusive{"invalid_input_size"};
      const auto signalValue = cacheSignal(usage);
      if (!signalValue) return ReadSampleResultInconclusive{"invalid_cache_signal"};
      return ReadSampleResultSample{{expectedSlot, reported->raw, usage, rt::Math::max(0, rt::Math::round(rt::performanceNow() - startedAtMs))}, *signalValue};
    }
    rt::Task<void> settleResponseProbe(rt::Response response, bool responseCompleted) {
      try {
        if (responseCompleted) {
          rt::Promise<void> transition(codex::auth::markCodexResponseCompleted(response));
          co_await transition.wait();
        } else {
          rt::Promise<void> transition(codex::auth::releaseCodexResponseProbe(response));
          co_await transition.wait();
        }
      } catch (...) {
        // Provider-health and probe transitions are best effort after terminal handling.
      }
    }
    bool isNormalizedUsage(const json::value* value) {
      if (!hasExactKeys(value, {"input_tokens", "cached_tokens", "cache_write_tokens", "output_tokens", "total_tokens"})) return false;
      for (auto key : {"input_tokens", "cached_tokens", "cache_write_tokens", "output_tokens", "total_tokens"})
        if (!isSafeNonNegativeInteger(rt::get(value, key))) return false;
      return true;
    }
    bool isPromptCacheScopeSample(const json::value* value) {
      if (!hasExactKeys(value, {"step", "slot", "raw_usage", "usage", "elapsed_ms"})) return false;
      const auto step = rt::getString(rt::get(value, "step"));
      if (!step) return false;
      const auto position = std::find(CACHE_SCOPE_STEP_NAMES.begin(), CACHE_SCOPE_STEP_NAMES.end(), *step);
      if (position == CACHE_SCOPE_STEP_NAMES.end()) return false;
      const auto index = static_cast<std::size_t>(position - CACHE_SCOPE_STEP_NAMES.begin());
      const auto* raw = rt::get(value, "raw_usage");
      const auto* usage = rt::get(value, "usage");
      return equalNumber(rt::get(value, "slot"), CACHE_SCOPE_EXPECTED_SLOTS[index]) && isNormalizedUsage(raw) && isNormalizedUsage(usage) &&
        sameUsage(json::value_to<NormalizedUsage>(*raw), json::value_to<NormalizedUsage>(*usage)) && isSafeNonNegativeInteger(rt::get(value, "elapsed_ms"));
    }
    bool isReason(const json::value* value) {
      const auto text = rt::getString(value);
      return text && std::find(INCONCLUSIVE_REASONS.begin(), INCONCLUSIVE_REASONS.end(), *text) != INCONCLUSIVE_REASONS.end();
    }
    bool isCycleEvidence(const json::value* value, const std::string& model) {
      if (!hasOnlyKeys(value, {"cycle", "samples", "classification", "inconclusive_reason"}) || !hasOwn(value, "cycle") || !hasOwn(value, "samples") ||
          !isSafeNonNegativeInteger(rt::get(value, "cycle"))) return false;
      const double cycle = rt::toNumber(rt::get(value, "cycle"));
      const auto* samples = rt::getArray(rt::get(value, "samples"));
      if (cycle < 1 || cycle > PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES || !samples || static_cast<double>(samples->size()) > PROMPT_CACHE_SCOPE_EXPERIMENT_SAMPLES_PER_CYCLE) return false;
      for (std::size_t i = 0; i < samples->size(); ++i)
        if (!isPromptCacheScopeSample(&(*samples)[i]) || !equalString(rt::get((*samples)[i], "step"), CACHE_SCOPE_STEP_NAMES[i])) return false;
      const bool hasClassification = hasOwn(value, "classification");
      const bool hasReason = hasOwn(value, "inconclusive_reason");
      if (hasClassification == hasReason) return false;
      if (hasReason) return isReason(rt::get(value, "inconclusive_reason"));
      const auto* classification = rt::get(value, "classification");
      if (!isConcreteObservation(classification) || !equalString(rt::get(classification, "effective_model"), model) ||
          static_cast<double>(samples->size()) != PROMPT_CACHE_SCOPE_EXPERIMENT_SAMPLES_PER_CYCLE) return false;
      std::vector<PromptCacheScopeSample> typedSamples;
      std::vector<CacheSignal> signals;
      for (const auto& sample : *samples) {
        auto typed = json::value_to<PromptCacheScopeSample>(sample);
        const auto signal = cacheSignal(typed.usage);
        if (!signal) return false;
        typedSamples.push_back(std::move(typed));
        signals.push_back(*signal);
      }
      const auto derived = classifyCycle(model, typedSamples, signals);
      return derived && sameObservation(json::value_to<ConcreteScopeObservation>(*classification), *derived);
    }
    json::value storedView(const rt::KvValue& value) {
      rt::StoredJsonViewOptions options;
      options.undefinedMember = rt::StoredJsonViewOptions::UndefinedView::emptyObject;
      return rt::storedJsonView(value, options);
    }
  }

  bool isSafeNonNegativeInteger(const json::value* value) { return value && rt::isNumber(*value) && isSafeNonNegativeInteger(rt::toNumber(value)); }
  bool isSafeNonNegativeInteger(const json::value& value) { return isSafeNonNegativeInteger(&value); }
  bool isSafeNonNegativeInteger(double value) { return rt::Number::isSafeInteger(value) && value >= 0; }
  rt::KvKey stateKey(const PromptCacheScopeTargetBinding& target) { return rt::KvKey{PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX, "state", targetKeyParts(target)}; }
  rt::KvKey evidenceKey(const PromptCacheScopeTargetBinding& target) { return rt::KvKey{PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX, "evidence", targetKeyParts(target)}; }
  rt::KvKey cycleLeaseKey(const PromptCacheScopeTargetBinding& target) { return rt::KvKey{PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX, "cycle_lease", targetKeyParts(target)}; }
  rt::KvKey campaignLeaseKey(const PromptCacheScopeTargetBinding& target) {
    // OAuth refresh mutates a shared Codex pool, so this fence is provider-wide.
    return rt::KvKey{PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX, "campaign", target.provider, target.telemetry_provider, target.probe_profile};
  }
  std::optional<PromptCacheScopeTargetBinding> parseTargetBinding(const json::value* value) {
    if (!hasExactKeys(value, {"id", "provider", "telemetry_provider", "topology_kind", "model", "probe_profile", "capability_fingerprint", "inventory_fingerprint",
                             "catalog_versionstamp", "runtime_versionstamp", "auth_pool_versionstamp", "auth_pool_identity_fingerprint", "catalog_client_version"}) ||
        !equalString(rt::get(value, "provider"), PROMPT_CACHE_SCOPE_EXPERIMENT_PROVIDER) ||
        !equalString(rt::get(value, "telemetry_provider"), CODEX_CHATGPT_PROMPT_CACHE_TELEMETRY_PROVIDER) ||
        !equalString(rt::get(value, "topology_kind"), "codex_account_pool") ||
        !equalString(rt::get(value, "probe_profile"), models::codex_models::PROMPT_CACHE_SCOPE_PROBE_PROFILE)) return std::nullopt;
    json::object out = value->as_object();
    for (auto key : {"id", "model", "capability_fingerprint", "inventory_fingerprint", "catalog_versionstamp", "runtime_versionstamp", "auth_pool_versionstamp", "auth_pool_identity_fingerprint"}) {
      const auto text = trimmedString(rt::get(value, key));
      if (!text || text->empty()) return std::nullopt;
      rt::objectSet(out, key, json::value(*text));
    }
    const auto* client = rt::get(value, "catalog_client_version");
    if (!client->is_null()) {
      const auto text = trimmedString(client);
      if (!text || text->empty()) return std::nullopt;
      rt::objectSet(out, "catalog_client_version", json::value(*text));
    }
    return json::value_to<PromptCacheScopeTargetBinding>(json::value(out));
  }
  std::optional<PromptCacheScopeTargetBinding> parseTargetBinding(const json::value& value) { return parseTargetBinding(&value); }
  std::optional<PromptCacheScopeTargetBinding> parseTargetBinding(const rt::KvValue& value) { const auto view = storedView(value); return parseTargetBinding(view); }
  bool sameLiteralField(std::string_view left, std::string_view right) { return left == right; }
  bool sameTargetDefinition(const PromptCacheScopeTargetBinding& left, const PromptCacheScopeTargetBinding& right) {
    return left.id == right.id && sameLiteralField(left.provider, right.provider) && sameLiteralField(left.telemetry_provider, right.telemetry_provider) &&
      sameLiteralField(left.topology_kind, right.topology_kind) && left.model == right.model && sameLiteralField(left.probe_profile, right.probe_profile) &&
      left.capability_fingerprint == right.capability_fingerprint && left.inventory_fingerprint == right.inventory_fingerprint &&
      left.auth_pool_identity_fingerprint == right.auth_pool_identity_fingerprint;
  }
  std::optional<ConcreteScopeObservation> sharedObservation(const std::vector<ConcreteScopeObservation>& observations) {
    if (observations.empty()) return std::nullopt;
    const auto& first = observations.front();
    return std::all_of(observations.begin(), observations.end(), [&](const auto& item) { return sameObservation(item, first); })
      ? std::optional<ConcreteScopeObservation>(first) : std::nullopt;
  }
  std::optional<ExperimentState> parseState(const json::value* value) {
    if (!hasOnlyKeys(value, {"v", "target", "campaign_owner", "started_at_ms", "expires_at_ms", "auth_pool_versionstamp", "next_cycle", "classifications", "pending_scope"}) ||
        !equalNumber(rt::get(value, "v"), 3)) return std::nullopt;
    for (auto key : {"target", "campaign_owner", "started_at_ms", "expires_at_ms", "auth_pool_versionstamp", "next_cycle", "classifications"})
      if (!hasOwn(value, key)) return std::nullopt;
    const auto target = parseTargetBinding(rt::get(value, "target"));
    const auto campaignOwner = trimmedString(rt::get(value, "campaign_owner"));
    const auto authPoolVersionstamp = trimmedString(rt::get(value, "auth_pool_versionstamp"));
    const auto* nextCycleValue = rt::get(value, "next_cycle");
    const auto* classifications = rt::getArray(rt::get(value, "classifications"));
    const double now = rt::dateNow();
    if ((!nextCycleValue || !rt::isNumber(*nextCycleValue)) || !rt::Number::isSafeInteger(rt::toNumber(nextCycleValue))) return std::nullopt;
    const double nextCycle = rt::toNumber(nextCycleValue);
    const auto* started = rt::get(value, "started_at_ms");
    const auto* expires = rt::get(value, "expires_at_ms");
    if (!target || !campaignOwner || campaignOwner->empty() || !authPoolVersionstamp || authPoolVersionstamp->empty() ||
        *authPoolVersionstamp != target->auth_pool_versionstamp || !isSafeNonNegativeInteger(started) || !isSafeNonNegativeInteger(expires) ||
        rt::toNumber(started) <= 0 || rt::toNumber(started) > now || rt::toNumber(expires) <= rt::toNumber(started) || nextCycle < 1 ||
        nextCycle > PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES + 1 || !classifications) return std::nullopt;
    for (const auto& classification : *classifications)
      if (!isConcreteObservation(&classification) || !equalString(rt::get(classification, "effective_model"), target->model)) return std::nullopt;
    if (static_cast<double>(classifications->size()) != rt::Math::min(nextCycle - 1, PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES)) return std::nullopt;
    const auto* pending = rt::get(value, "pending_scope");
    if (pending && !isConcreteObservation(pending)) return std::nullopt;
    const auto typed = json::value_to<std::vector<ConcreteScopeObservation>>(json::value(*classifications));
    if (nextCycle == PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES + 1) {
      const auto agreed = sharedObservation(typed);
      if (!isConcreteObservation(pending) || !equalString(rt::get(pending, "effective_model"), target->model) || !agreed ||
          !sameObservation(json::value_to<ConcreteScopeObservation>(*pending), *agreed)) return std::nullopt;
    }
    if (nextCycle <= PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES && pending) return std::nullopt;
    return ExperimentState{3, *target, *campaignOwner, rt::toNumber(started), rt::toNumber(expires), *authPoolVersionstamp, nextCycle, typed,
                           pending ? std::optional<ConcreteScopeObservation>(json::value_to<ConcreteScopeObservation>(*pending)) : std::nullopt};
  }
  std::optional<ExperimentState> parseState(const json::value& value) { return parseState(&value); }
  std::optional<ExperimentState> parseState(const rt::KvValue& value) {
    auto view = storedView(value);
    if (view.is_object() && value.isObject() && value.asObject().contains("pending_scope") && value["pending_scope"].isUndefined())
      rt::objectDelete(view.as_object(), "pending_scope");
    return parseState(view);
  }
  bool ownsLease(const json::value* value, std::string_view owner) {
    return value && utils::isRecord(*value) && equalString(rt::get(value, "owner"), owner) &&
      isSafeNonNegativeInteger(rt::get(value, "lease_until_ms")) && rt::toNumber(rt::get(value, "lease_until_ms")) > rt::dateNow();
  }
  bool ownsLease(const json::value& value, std::string_view owner) { return ownsLease(&value, owner); }
  bool ownsLease(const rt::KvValue& value, std::string_view owner) { const auto view = storedView(value); return ownsLease(view, owner); }
  bool ownsCampaignLease(const json::value* value, const ExperimentState& state) {
    return value && utils::isRecord(*value) && equalString(rt::get(value, "owner"), state.campaign_owner) &&
      equalString(rt::get(value, "target_id"), state.target.id) && equalString(rt::get(value, "inventory_fingerprint"), state.target.inventory_fingerprint) &&
      isSafeNonNegativeInteger(rt::get(value, "lease_until_ms")) && rt::toNumber(rt::get(value, "lease_until_ms")) > rt::dateNow();
  }
  bool ownsCampaignLease(const json::value& value, const ExperimentState& state) { return ownsCampaignLease(&value, state); }
  bool ownsCampaignLease(const rt::KvValue& value, const ExperimentState& state) { const auto view = storedView(value); return ownsCampaignLease(view, state); }
  json::object buildExperimentRequest(const std::string& model, const std::string& cycleId, const std::string& cacheKey) {
    static const std::string staticPrefix = [] {
      std::string out;
      for (int i = 0; i < 2'560; ++i) { if (i) out += ' '; out += "cache"; }
      return out;
    }();
    // The nonce precedes reusable material so prior cycles cannot satisfy the initial prefix.
    const std::string prefix = rt::concat(rt::concat("cache-scope-cycle:", cycleId), "\n\n" + staticPrefix);
    const json::array input{
      json::object{{"type", "message"}, {"role", "developer"}, {"content", json::array{json::object{{"type", "input_text"}, {"text", prefix}}}}},
      json::object{{"type", "message"}, {"role", "user"}, {"content", json::array{json::object{{"type", "input_text"}, {"text", "Reply with exactly: cache scope experiment."}}}}}};
    return json::object{{"model", model}, {"input", input}, {"store", false}, {"stream", true}, {"reasoning", json::object{{"effort", "none"}}}, {"prompt_cache_key", cacheKey}};
  }
  bool hasMixedPrefixScaleCacheSignals(const NormalizedUsage& usage) {
    return isPrefixScaleCounter(usage.cached_tokens, usage.input_tokens) && isPrefixScaleCounter(usage.cache_write_tokens, usage.input_tokens) &&
      usage.cached_tokens > 0 && usage.cache_write_tokens > 0;
  }
  bool matchesCycleReusableCounter(const NormalizedUsage& usage, double reusableTokens) {
    return (usage.cached_tokens == 0 || usage.cached_tokens == reusableTokens) && (usage.cache_write_tokens == 0 || usage.cache_write_tokens == reusableTokens);
  }
  rt::Task<ReadSampleResult> readPromptCacheScopeExperimentCompletedUsage(rt::Response response, double expectedSlot, std::string expectedModel,
                                                                       double startedAtMs, rt::AbortSignal signal) {
    bool responseCompleted = false;
    rt::Reason failure;
    std::optional<ReadSampleResult> result;
    try {
      if (!response.ok() || !response.body()) {
        cancelResponse(response);
        throw CodexCacheScopeExperimentError("Prompt-cache scope experiment did not receive a readable upstream response.");
      }
      if (codex::auth::getCodexResponseSlot(response) != expectedSlot) {
        cancelResponse(response);
        result = ReadSampleResultInconclusive{"slot_drift"};
      } else {
        std::optional<json::object> terminalResponse;
        auto events = responses_stream::readResponsesStream(*response.body(), signal);
        for (;;) {
          const auto next = co_await events.next();
          if (next.done) break;
          const auto& event = *next.value;
          const auto* payload = rt::get(event.value, "response");
          if (event.type == "response.completed" && payload && payload->is_object()) {
            terminalResponse = payload->as_object();
            responseCompleted = true;
            auto returning = events.return_();
            co_await std::move(returning);
            break;
          }
          if (event.terminal) {
            auto returning = events.return_();
            co_await std::move(returning);
            break;
          }
        }
        if (!terminalResponse) throw CodexCacheScopeExperimentError("Prompt-cache scope experiment did not receive a completed terminal response.");
        result = sampleFromCompletedResponse(*terminalResponse, expectedSlot, expectedModel, startedAtMs);
      }
    } catch (const CodexCacheScopeExperimentError&) {
      failure = std::current_exception();
    } catch (...) {
      const auto error = std::current_exception();
      const auto message = rt::isError(error) ? "Prompt-cache scope experiment stream failed: " + rt::errorMessage(error).value_or("") : "Prompt-cache scope experiment stream failed.";
      failure = rt::makeReason(CodexCacheScopeExperimentError(message));
    }
    rt::Promise<void> settlement(settleResponseProbe(response, responseCompleted));
    co_await settlement.wait();
    if (failure) std::rethrow_exception(failure);
    co_return *result;
  }
  std::optional<ConcreteScopeObservation> classifyCycle(const std::string& model, const std::vector<PromptCacheScopeSample>& samples,
                                                       const std::vector<CacheSignal>& signals) {
    if (static_cast<double>(samples.size()) != PROMPT_CACHE_SCOPE_EXPERIMENT_SAMPLES_PER_CYCLE || signals.size() != samples.size()) return std::nullopt;
    for (std::size_t i = 0; i < samples.size(); ++i)
      if (samples[i].slot != CACHE_SCOPE_EXPECTED_SLOTS[i] || samples[i].usage.input_tokens != samples.front().usage.input_tokens) return std::nullopt;
    const double reusableTokens = samples.front().usage.cache_write_tokens;
    if (reusableTokens == 0) return std::nullopt;
    for (std::size_t i = 0; i < samples.size(); ++i) {
      if (!matchesCycleReusableCounter(samples[i].usage, reusableTokens)) return std::nullopt;
      if (CACHE_SCOPE_DISCRIMINATOR_INDEXES.has(static_cast<double>(i)) && hasMixedPrefixScaleCacheSignals(samples[i].usage)) return std::nullopt;
      if (CACHE_SCOPE_EXPECTED_SIGNALS[i] && signals[i] != *CACHE_SCOPE_EXPECTED_SIGNALS[i]) return std::nullopt;
    }
    const auto accountSlots = triStateFromSignal(signals[2], "shared", "account_scoped");
    const auto tokenRefresh = triStateFromSignal(signals[5], "preserved", "changed");
    const auto conversationId = triStateFromSignal(signals[7], "independent", "scoped");
    if (!accountSlots || !tokenRefresh || !conversationId) return std::nullopt;
    return ConcreteScopeObservation{std::string(models::codex_models::PROMPT_CACHE_SCOPE_PROBE_PROFILE), *accountSlots, *tokenRefresh, *conversationId, model};
  }
  std::optional<StoredEvidence> parseEvidence(const json::value* value) {
    if (!hasOnlyKeys(value, {"v", "target", "outcome", "started_at_ms", "verified_at_ms", "cycles", "inconclusive_reason"}) ||
        !equalNumber(rt::get(value, "v"), 3)) return std::nullopt;
    const auto target = parseTargetBinding(rt::get(value, "target"));
    const auto outcome = rt::toString(rt::get(value, "outcome"));
    const bool hasInconclusiveReason = hasOwn(value, "inconclusive_reason");
    const double now = rt::dateNow();
    const auto* started = rt::get(value, "started_at_ms");
    const auto* verified = rt::get(value, "verified_at_ms");
    const auto* cycles = rt::getArray(rt::get(value, "cycles"));
    constexpr std::array<std::string_view, 5> outcomes{"in_progress", "ready_to_promote", "completed", "inconclusive", "failed"};
    if (!target || !isSafeNonNegativeInteger(started) || !isSafeNonNegativeInteger(verified) || rt::toNumber(started) <= 0 ||
        rt::toNumber(verified) < rt::toNumber(started) || rt::toNumber(verified) > now || !cycles ||
        static_cast<double>(cycles->size()) > PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES || std::find(outcomes.begin(), outcomes.end(), outcome) == outcomes.end() ||
        (hasInconclusiveReason && !isReason(rt::get(value, "inconclusive_reason")))) return std::nullopt;
    for (std::size_t i = 0; i < cycles->size(); ++i)
      if (!isCycleEvidence(&(*cycles)[i], target->model) || !equalNumber(rt::get((*cycles)[i], "cycle"), static_cast<double>(i + 1))) return std::nullopt;
    const auto typed = json::value_to<std::vector<CycleEvidence>>(json::value(*cycles));
    const bool allClassified = std::all_of(typed.begin(), typed.end(), [](const auto& cycle) { return cycle.classification.has_value(); });
    const bool classificationsAgree = allClassified && classificationEvidenceAgrees(typed);
    bool inconclusiveCyclesCoherent = typed.empty();
    if (!typed.empty()) {
      const auto& last = typed.back();
      const auto reason = rt::getString(rt::get(value, "inconclusive_reason"));
      inconclusiveCyclesCoherent = std::all_of(typed.begin(), typed.end() - 1, [](const auto& cycle) { return cycle.classification.has_value(); }) &&
        (last.classification || (last.inconclusive_reason && reason && *last.inconclusive_reason == *reason));
    }
    const double count = static_cast<double>(typed.size());
    if ((outcome == "in_progress" && (hasInconclusiveReason || !allClassified || count >= PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES)) ||
        ((outcome == "ready_to_promote" || outcome == "completed") && (hasInconclusiveReason || count != PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES || !classificationsAgree)) ||
        (outcome == "inconclusive" && (!hasInconclusiveReason || !inconclusiveCyclesCoherent)) ||
        (outcome == "failed" && (hasInconclusiveReason || !allClassified))) return std::nullopt;
    std::optional<InconclusiveReason> reason;
    if (hasInconclusiveReason) reason = std::string(*rt::getString(rt::get(value, "inconclusive_reason")));
    return StoredEvidence{3, *target, outcome, rt::toNumber(started), rt::toNumber(verified), typed, reason};
  }
  std::optional<StoredEvidence> parseEvidence(const json::value& value) { return parseEvidence(&value); }
  std::optional<StoredEvidence> parseEvidence(const rt::KvValue& value) { const auto view = storedView(value); return parseEvidence(view); }
  bool activeStateEvidenceIsConsistent(const ExperimentState& state, const std::optional<StoredEvidence>& evidence) {
    if (state.classifications.empty()) return !evidence;
    if (!evidence || !sameTargetDefinition(evidence->target, state.target) || evidence->started_at_ms != state.started_at_ms ||
        evidence->cycles.size() != state.classifications.size()) return false;
    for (std::size_t i = 0; i < evidence->cycles.size(); ++i)
      if (!evidence->cycles[i].classification || !sameObservation(*evidence->cycles[i].classification, state.classifications[i])) return false;
    return evidence->outcome == (state.next_cycle == PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES + 1 ? "ready_to_promote" : "in_progress");
  }
  void throwIfAborted(const rt::AbortSignal& signal) {
    if (!signal.aborted()) return;
    const auto& reason = signal.reason();
    const auto primitive = rt::thrownValue(reason);
    if (reason && (!primitive || !primitive->is_null())) std::rethrow_exception(reason);
    throw rt::DOMException("Prompt-cache scope experiment was aborted.", "AbortError");
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ReadSampleResult& value) {
    if (const auto* sample = std::get_if<ReadSampleResultSample>(&value)) {
      const auto& row = sample->sample;
      const auto serialized = Writer().required("slot", row.slot).required("raw_usage", row.raw_usage).required("usage", row.usage).required("elapsed_ms", row.elapsed_ms).done();
      out = Writer().required("status", std::string(sample->status)).required("signal", sample->signal).required("sample", serialized).done();
    } else {
      const auto& inconclusive = std::get<ReadSampleResultInconclusive>(value);
      out = Writer().required("status", std::string(inconclusive.status)).required("reason", inconclusive.reason).done();
    }
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const ConcreteScopeObservation& value) {
    out = Writer()
      .required("probe_profile", value.probe_profile)
      .required("account_slots", value.account_slots)
      .required("token_refresh", value.token_refresh)
      .required("conversation_id", value.conversation_id)
      .required("effective_model", value.effective_model).done();
  }
  ConcreteScopeObservation tag_invoke(const json::value_to_tag<ConcreteScopeObservation>&, const json::value& value) {
    const Reader in(value, "ConcreteScopeObservation");
    return {.probe_profile = in.required<std::string>("probe_profile"),
      .account_slots = in.required<std::string>("account_slots"),
      .token_refresh = in.required<std::string>("token_refresh"),
      .conversation_id = in.required<std::string>("conversation_id"),
      .effective_model = in.required<std::string>("effective_model")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const NormalizedUsage& value) {
    out = Writer()
      .required("input_tokens", value.input_tokens)
      .required("cached_tokens", value.cached_tokens)
      .required("cache_write_tokens", value.cache_write_tokens)
      .required("output_tokens", value.output_tokens)
      .required("total_tokens", value.total_tokens).done();
  }
  NormalizedUsage tag_invoke(const json::value_to_tag<NormalizedUsage>&, const json::value& value) {
    const Reader in(value, "NormalizedUsage");
    return {.input_tokens = in.required<double>("input_tokens"),
      .cached_tokens = in.required<double>("cached_tokens"),
      .cache_write_tokens = in.required<double>("cache_write_tokens"),
      .output_tokens = in.required<double>("output_tokens"),
      .total_tokens = in.required<double>("total_tokens")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheScopeSample& value) {
    out = Writer()
      .required("step", value.step)
      .required("slot", value.slot)
      .required("raw_usage", value.raw_usage)
      .required("usage", value.usage)
      .required("elapsed_ms", value.elapsed_ms).done();
  }
  PromptCacheScopeSample tag_invoke(const json::value_to_tag<PromptCacheScopeSample>&, const json::value& value) {
    const Reader in(value, "PromptCacheScopeSample");
    return {.step = in.required<CacheScopeStepName>("step"),
      .slot = in.required<double>("slot"),
      .raw_usage = in.required<NormalizedUsage>("raw_usage"),
      .usage = in.required<NormalizedUsage>("usage"),
      .elapsed_ms = in.required<double>("elapsed_ms")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const CycleEvidence& value) {
    out = Writer()
      .required("cycle", value.cycle)
      .required("samples", value.samples)
      .orUndefined("classification", value.classification)
      .orUndefined("inconclusive_reason", value.inconclusive_reason).done();
  }
  CycleEvidence tag_invoke(const json::value_to_tag<CycleEvidence>&, const json::value& value) {
    const Reader in(value, "CycleEvidence");
    return {.cycle = in.required<double>("cycle"),
      .samples = in.required<std::vector<PromptCacheScopeSample>>("samples"),
      .classification = in.orUndefined<ConcreteScopeObservation>("classification"),
      .inconclusive_reason = in.orUndefined<InconclusiveReason>("inconclusive_reason")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheScopeTargetBinding& value) {
    out = Writer()
      .required("id", value.id)
      .required("provider", value.provider)
      .required("telemetry_provider", value.telemetry_provider)
      .required("topology_kind", value.topology_kind)
      .required("model", value.model)
      .required("probe_profile", value.probe_profile)
      .required("capability_fingerprint", value.capability_fingerprint)
      .required("inventory_fingerprint", value.inventory_fingerprint)
      .required("catalog_versionstamp", value.catalog_versionstamp)
      .required("runtime_versionstamp", value.runtime_versionstamp)
      .required("auth_pool_versionstamp", value.auth_pool_versionstamp)
      .required("auth_pool_identity_fingerprint", value.auth_pool_identity_fingerprint)
      .orNull("catalog_client_version", value.catalog_client_version).done();
  }
  PromptCacheScopeTargetBinding tag_invoke(const json::value_to_tag<PromptCacheScopeTargetBinding>&, const json::value& value) {
    const Reader in(value, "PromptCacheScopeTargetBinding");
    return {.id = in.required<std::string>("id"),
      .provider = in.required<std::string>("provider"),
      .telemetry_provider = in.required<std::string>("telemetry_provider"),
      .topology_kind = in.required<std::string>("topology_kind"),
      .model = in.required<std::string>("model"),
      .probe_profile = in.required<std::string>("probe_profile"),
      .capability_fingerprint = in.required<std::string>("capability_fingerprint"),
      .inventory_fingerprint = in.required<std::string>("inventory_fingerprint"),
      .catalog_versionstamp = in.required<std::string>("catalog_versionstamp"),
      .runtime_versionstamp = in.required<std::string>("runtime_versionstamp"),
      .auth_pool_versionstamp = in.required<std::string>("auth_pool_versionstamp"),
      .auth_pool_identity_fingerprint = in.required<std::string>("auth_pool_identity_fingerprint"),
      .catalog_client_version = in.orNull<std::string>("catalog_client_version")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const StoredEvidence& value) {
    out = Writer()
      .required("v", value.v)
      .required("target", value.target)
      .required("outcome", value.outcome)
      .required("started_at_ms", value.started_at_ms)
      .required("verified_at_ms", value.verified_at_ms)
      .required("cycles", value.cycles)
      .orUndefined("inconclusive_reason", value.inconclusive_reason).done();
  }
  StoredEvidence tag_invoke(const json::value_to_tag<StoredEvidence>&, const json::value& value) {
    const Reader in(value, "StoredEvidence");
    return {.v = in.required<double>("v"),
      .target = in.required<PromptCacheScopeTargetBinding>("target"),
      .outcome = in.required<std::string>("outcome"),
      .started_at_ms = in.required<double>("started_at_ms"),
      .verified_at_ms = in.required<double>("verified_at_ms"),
      .cycles = in.required<std::vector<CycleEvidence>>("cycles"),
      .inconclusive_reason = in.orUndefined<InconclusiveReason>("inconclusive_reason")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const ExperimentState& value) {
    out = Writer()
      .required("v", value.v)
      .required("target", value.target)
      .required("campaign_owner", value.campaign_owner)
      .required("started_at_ms", value.started_at_ms)
      .required("expires_at_ms", value.expires_at_ms)
      .required("auth_pool_versionstamp", value.auth_pool_versionstamp)
      .required("next_cycle", value.next_cycle)
      .required("classifications", value.classifications)
      .orUndefined("pending_scope", value.pending_scope).done();
  }
  ExperimentState tag_invoke(const json::value_to_tag<ExperimentState>&, const json::value& value) {
    const Reader in(value, "ExperimentState");
    return {.v = in.required<double>("v"),
      .target = in.required<PromptCacheScopeTargetBinding>("target"),
      .campaign_owner = in.required<std::string>("campaign_owner"),
      .started_at_ms = in.required<double>("started_at_ms"),
      .expires_at_ms = in.required<double>("expires_at_ms"),
      .auth_pool_versionstamp = in.required<std::string>("auth_pool_versionstamp"),
      .next_cycle = in.required<double>("next_cycle"),
      .classifications = in.required<std::vector<ConcreteScopeObservation>>("classifications"),
      .pending_scope = in.orUndefined<ConcreteScopeObservation>("pending_scope")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const ExperimentLease& value) {
    out = Writer()
      .required("owner", value.owner)
      .required("lease_until_ms", value.lease_until_ms).done();
  }
  ExperimentLease tag_invoke(const json::value_to_tag<ExperimentLease>&, const json::value& value) {
    const Reader in(value, "ExperimentLease");
    return {.owner = in.required<std::string>("owner"),
      .lease_until_ms = in.required<double>("lease_until_ms")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const CampaignLease& value) {
    out = Writer()
      .required("owner", value.owner)
      .required("target_id", value.target_id)
      .required("inventory_fingerprint", value.inventory_fingerprint)
      .required("lease_until_ms", value.lease_until_ms).done();
  }
  CampaignLease tag_invoke(const json::value_to_tag<CampaignLease>&, const json::value& value) {
    const Reader in(value, "CampaignLease");
    return {.owner = in.required<std::string>("owner"),
      .target_id = in.required<std::string>("target_id"),
      .inventory_fingerprint = in.required<std::string>("inventory_fingerprint"),
      .lease_until_ms = in.required<double>("lease_until_ms")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const PromptCacheScopeExperimentResult& value) {
    out = Writer()
      .required("provider", value.provider)
      .required("telemetry_provider", value.telemetry_provider)
      .required("target_id", value.target_id)
      .required("model", value.model)
      .required("status", value.status)
      .required("completed_cycles", value.completed_cycles)
      .required("verified_at_ms", value.verified_at_ms)
      .orUndefined("scope", value.scope)
      .orUndefined("inconclusive_reason", value.inconclusive_reason).done();
  }
  PromptCacheScopeExperimentResult tag_invoke(const json::value_to_tag<PromptCacheScopeExperimentResult>&, const json::value& value) {
    const Reader in(value, "PromptCacheScopeExperimentResult");
    return {.provider = in.required<std::string>("provider"),
      .telemetry_provider = in.required<std::string>("telemetry_provider"),
      .target_id = in.required<std::string>("target_id"),
      .model = in.required<std::string>("model"),
      .status = in.required<std::string>("status"),
      .completed_cycles = in.required<double>("completed_cycles"),
      .verified_at_ms = in.required<double>("verified_at_ms"),
      .scope = in.orUndefined<ConcreteScopeObservation>("scope"),
      .inconclusive_reason = in.orUndefined<InconclusiveReason>("inconclusive_reason")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const RuntimeBinding& value) {
    out = Writer()
      .required("versionstamp", value.versionstamp)
      .required("default_model", value.default_model).done();
  }
  RuntimeBinding tag_invoke(const json::value_to_tag<RuntimeBinding>&, const json::value& value) {
    const Reader in(value, "RuntimeBinding");
    return {.versionstamp = in.required<std::string>("versionstamp"),
      .default_model = in.required<std::string>("default_model")};
  }
}
