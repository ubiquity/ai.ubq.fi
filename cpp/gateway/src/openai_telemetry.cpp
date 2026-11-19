// Port of src/openai-telemetry.ts.
// Response usage, provider and stream telemetry, extracted from src/openai.ts.
#include "uos/openai_telemetry.hpp"

#include "uos/codex/auth.hpp"
#include "uos/http.hpp"
#include "uos/provider/removed_provider_telemetry.hpp"
#include "uos/responses_failover_stream.hpp"
#include "uos/responses_stream.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <mutex>
#include <span>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace uos::openai_telemetry {
  namespace {
    constexpr std::size_t MAX_UPSTREAM_FAILURE_EVENTS = 16;
  }

  bool supportsReasoningProgressRelease(const UpstreamProvider& provider) {
    return provider == "chatgpt_codex" || provider == "surplus" || provider == "metered";
  }

  rt::WeakMap<rt::Response, std::shared_ptr<ResponseTelemetryState>> responseTelemetry;

  std::shared_ptr<ResponseTelemetryState> createResponseTelemetryState() {
    return std::make_shared<ResponseTelemetryState>(ResponseTelemetryState{
      .provider = std::nullopt,
      .fallbackReason = std::nullopt,
      .model = std::nullopt,
      .reasoning = std::nullopt,
      .outputTokenAllowance = std::nullopt,
      .rateLimitWaitMs = std::nullopt,
      .rateLimitFailoverModel = std::nullopt,
      .inputTokens = std::nullopt,
      .cachedInputTokens = std::nullopt,
      .cacheWriteInputTokens = std::nullopt,
      .outputTokens = std::nullopt,
      .totalTokens = std::nullopt,
      .usageObserved = false,
      .usageTelemetryStatus = "missing",
      .promptCacheKeyPresent = false,
      .promptCacheMode = "unspecified",
      .explicitBreakpointCount = 0,
      .accountSlot = std::nullopt,
      .accountCohortId = std::nullopt,
      .activeGeneration = std::nullopt,
      .activeTransitionReason = std::nullopt,
      .quotaUsedPercent = rt::undefined,
      .completed = false,
      .semanticOutputObserved = std::nullopt,
      .upstreamEventKinds = {},
      .streamTerminalType = std::nullopt,
      .failureKind = std::nullopt,
      .responseCreatedObserved = false,
      .syntheticTerminalType = std::nullopt,
      .stream = std::nullopt,
      .providerRequestId = std::nullopt,
      .firstProviderDispatchMs = std::nullopt,
      .firstProviderHeadersMs = std::nullopt,
      .firstCodexDispatchMs = std::nullopt,
      .firstCodexHeadersMs = std::nullopt,
      .firstUpstreamSseEventMs = std::nullopt,
      .firstSemanticCommitmentMs = std::nullopt,
      .streamTerminalMs = std::nullopt,
      .attemptedProviders = {},
      .removedProviderTriggerClass = std::nullopt,
      .removedProviderCircuitTransition = std::nullopt,
      .removedProviderSelectedModel = std::nullopt,
      .removedProviderTaskType = std::nullopt,
      .removedProviderSemanticCommitment = std::nullopt,
      .removedProviderLatencyMs = std::nullopt,
      .removedProviderTerminalStatus = std::nullopt,
    });
  }

  namespace {
    UsageContext withResponseTelemetryContext(const std::optional<UsageContext>& context, std::shared_ptr<ResponseTelemetryState> state) {
      return UsageContext{
        .keyId = context ? context->keyId : std::nullopt,
        .kernelRepo = context ? context->kernelRepo : std::nullopt,
        .kernelOrg = context ? context->kernelOrg : std::nullopt,
        .paidFallbackEnabled = context ? context->paidFallbackEnabled : std::nullopt,
        .idempotencyPrincipal = context ? context->idempotencyPrincipal : rt::Nullable<std::string>(),
        .requestId = context ? context->requestId : std::nullopt,
        .paidFallbackRequestId = context ? context->paidFallbackRequestId : std::nullopt,
        .startedAtMs = context ? context->startedAtMs : std::nullopt,
        .startedAtMonotonicMs = context ? context->startedAtMonotonicMs : std::nullopt,
        .downstreamSignal = context ? context->downstreamSignal : std::nullopt,
        .responseTelemetry = std::move(state),
        .beforeProviderDispatch = context ? context->beforeProviderDispatch : api_key_policy::ApiKeyUsageReservation::BeforeProviderDispatch(),
        .sentinelUpstreamRecorder = context ? context->sentinelUpstreamRecorder : std::nullopt,
        .onTerminalUsage = context ? context->onTerminalUsage : std::function<void(std::optional<UsageTokens>, bool)>(),
      };
    }

    // A header present with an empty value still falls back to the gateway label,
    // so this keeps the original falsy fallback instead of a nullish one.
    std::string upstreamProviderLabel(const rt::Response& response) {
      const std::optional<std::string> header = response.headers().get("x-uos-upstream");
      if (!header || header->empty()) return "gateway";
      return *header;
    }
  }

  rt::Response attachResponseTelemetry(rt::Response response, std::shared_ptr<ResponseTelemetryState> state) {
    if (!state->provider) state->provider = upstreamProviderLabel(response);
    responseTelemetry.set(response, state);
    return response;
  }

  rt::Response setResponseCompletionTelemetry(rt::Response response, bool stream) {
    std::shared_ptr<ResponseTelemetryState> state = createResponseTelemetryState();
    state->completed = true;
    state->semanticOutputObserved = true;
    state->stream = stream;
    state->streamTerminalType = "response.completed";
    return attachResponseTelemetry(std::move(response), std::move(state));
  }

  namespace {
    using TelemetryStates = std::vector<std::shared_ptr<ResponseTelemetryState>>;
    using TelemetryCount = std::optional<double> ResponseTelemetryState::*;
    using TelemetryText = std::optional<std::string> ResponseTelemetryState::*;

    std::optional<double> sumTelemetryCounts(const TelemetryStates& states, TelemetryCount key, std::size_t expectedCount) {
      if (states.size() != expectedCount ||
          std::any_of(states.begin(), states.end(), [&](const std::shared_ptr<ResponseTelemetryState>& state) { return !((*state).*key).has_value(); })) {
        return std::nullopt;
      }
      double total = 0;
      for (const std::shared_ptr<ResponseTelemetryState>& state : states) total = total + ((*state).*key).value_or(0);
      return total;
    }

    // Object.is over the `T | null` values the TS compares.
    bool sameTelemetryValue(const std::optional<double>& a, const std::optional<double>& b) {
      if (!a || !b) return !a && !b;
      return rt::sameValue(*a, *b);
    }
    bool sameTelemetryValue(const std::optional<std::string>& a, const std::optional<std::string>& b) { return a == b; }

    template <class T>
    std::optional<T> commonTelemetryValue(const std::vector<std::optional<T>>& values) {
      if (values.empty()) return std::nullopt;
      const std::optional<T>& first = values[0];
      for (const std::optional<T>& value : values) {
        if (!sameTelemetryValue(value, first)) return std::nullopt;
      }
      return first;
    }

    std::vector<std::optional<double>> telemetryValues(const TelemetryStates& states, TelemetryCount key) {
      std::vector<std::optional<double>> values;
      for (const std::shared_ptr<ResponseTelemetryState>& state : states) values.push_back((*state).*key);
      return values;
    }
    std::vector<std::optional<std::string>> telemetryValues(const TelemetryStates& states, TelemetryText key) {
      std::vector<std::optional<std::string>> values;
      for (const std::shared_ptr<ResponseTelemetryState>& state : states) values.push_back((*state).*key);
      return values;
    }

    // An invalid member state poisons the aggregate, an incomplete set of members
    // can never be reported as complete, and a fully reported set counts as
    // reported once usage was observed.
    UsageTelemetryStatus aggregatedUsageTelemetryStatus(const TelemetryStates& states, std::size_t sourceCount, bool usageObserved) {
      if (std::any_of(states.begin(), states.end(), [](const std::shared_ptr<ResponseTelemetryState>& state) { return state->usageTelemetryStatus == "invalid"; })) {
        return "invalid";
      }
      if (states.size() != sourceCount) return usageObserved ? "partial" : "missing";
      if (std::all_of(states.begin(), states.end(), [](const std::shared_ptr<ResponseTelemetryState>& state) { return state->usageTelemetryStatus == "reported"; })) {
        return "reported";
      }
      return usageObserved ? "partial" : "missing";
    }

    rt::Nullable<double> aggregatedQuotaUsedPercent(const std::vector<double>& usedPercents, const TelemetryStates& states) {
      if (!usedPercents.empty()) return rt::Math::max(std::span<const double>(usedPercents));
      return std::any_of(states.begin(), states.end(), [](const std::shared_ptr<ResponseTelemetryState>& state) { return state->quotaUsedPercent == nullptr; })
               ? rt::Nullable<double>(nullptr)
               : rt::Nullable<double>(rt::undefined);
    }

    std::optional<bool> aggregatedSemanticOutputObserved(const TelemetryStates& states) {
      if (std::any_of(states.begin(), states.end(), [](const std::shared_ptr<ResponseTelemetryState>& state) { return state->semanticOutputObserved == true; })) {
        return true;
      }
      return std::all_of(states.begin(), states.end(), [](const std::shared_ptr<ResponseTelemetryState>& state) { return state->semanticOutputObserved == false; })
               ? std::optional<bool>(false)
               : std::nullopt;
    }

    std::optional<std::string> aggregatedProviderLabel(const std::vector<std::string>& providers) {
      if (providers.size() == 1) return providers[0];
      if (providers.size() > 1) return "mixed";
      return std::nullopt;
    }

    // `[...new Set(values)]`: first occurrences in order.
    void appendUnique(std::vector<std::string>& target, const std::vector<std::string>& values) {
      for (const std::string& value : values) {
        if (std::find(target.begin(), target.end(), value) == target.end()) target.push_back(value);
      }
    }

    std::vector<double> presentValues(const TelemetryStates& states, TelemetryCount key) {
      std::vector<double> values;
      for (const std::shared_ptr<ResponseTelemetryState>& state : states) {
        if (const std::optional<double>& value = (*state).*key) values.push_back(*value);
      }
      return values;
    }
  }

  rt::Response aggregateResponseTelemetry(const std::vector<rt::Response>& sources, rt::Response target) {
    TelemetryStates states;
    for (const rt::Response& source : sources) {
      if (const std::shared_ptr<ResponseTelemetryState>* state = responseTelemetry.find(source); state && *state) states.push_back(*state);
    }
    if (states.empty()) return target;
    if (states.size() == 1 && sources.size() == 1) {
      responseTelemetry.set(target, states[0]);
      return target;
    }

    std::shared_ptr<ResponseTelemetryState> aggregate = createResponseTelemetryState();
    std::vector<std::string> providers;
    for (const std::shared_ptr<ResponseTelemetryState>& state : states) {
      if (state->provider && !state->provider->empty() && std::find(providers.begin(), providers.end(), *state->provider) == providers.end()) {
        providers.push_back(*state->provider);
      }
    }
    aggregate->provider = aggregatedProviderLabel(providers);
    aggregate->fallbackReason = commonTelemetryValue(telemetryValues(states, &ResponseTelemetryState::fallbackReason));
    aggregate->model = commonTelemetryValue(telemetryValues(states, &ResponseTelemetryState::model));
    aggregate->reasoning = commonTelemetryValue(telemetryValues(states, &ResponseTelemetryState::reasoning));
    aggregate->outputTokenAllowance = commonTelemetryValue(telemetryValues(states, &ResponseTelemetryState::outputTokenAllowance));
    // Waits from several attempts of one request add up into the absorbed total.
    const std::vector<double> waits = presentValues(states, &ResponseTelemetryState::rateLimitWaitMs);
    if (!waits.empty()) {
      double total = 0;
      for (const double value : waits) total = total + value;
      aggregate->rateLimitWaitMs = total;
    } else {
      aggregate->rateLimitWaitMs = std::nullopt;
    }
    aggregate->rateLimitFailoverModel = commonTelemetryValue(telemetryValues(states, &ResponseTelemetryState::rateLimitFailoverModel));
    aggregate->inputTokens = sumTelemetryCounts(states, &ResponseTelemetryState::inputTokens, sources.size());
    aggregate->cachedInputTokens = sumTelemetryCounts(states, &ResponseTelemetryState::cachedInputTokens, sources.size());
    aggregate->cacheWriteInputTokens = sumTelemetryCounts(states, &ResponseTelemetryState::cacheWriteInputTokens, sources.size());
    aggregate->outputTokens = sumTelemetryCounts(states, &ResponseTelemetryState::outputTokens, sources.size());
    aggregate->totalTokens = sumTelemetryCounts(states, &ResponseTelemetryState::totalTokens, sources.size());
    aggregate->usageObserved =
      std::any_of(states.begin(), states.end(), [](const std::shared_ptr<ResponseTelemetryState>& state) { return state->usageObserved; });
    aggregate->usageTelemetryStatus = aggregatedUsageTelemetryStatus(states, sources.size(), aggregate->usageObserved);
    aggregate->promptCacheKeyPresent =
      std::any_of(states.begin(), states.end(), [](const std::shared_ptr<ResponseTelemetryState>& state) { return state->promptCacheKeyPresent; });
    std::vector<std::optional<std::string>> promptCacheModes;
    for (const std::shared_ptr<ResponseTelemetryState>& state : states) promptCacheModes.emplace_back(state->promptCacheMode);
    aggregate->promptCacheMode = commonTelemetryValue(promptCacheModes).value_or("unspecified");
    std::vector<double> breakpointCounts{0};
    for (const std::shared_ptr<ResponseTelemetryState>& state : states) breakpointCounts.push_back(state->explicitBreakpointCount);
    aggregate->explicitBreakpointCount = rt::Math::max(std::span<const double>(breakpointCounts));
    aggregate->accountSlot = commonTelemetryValue(telemetryValues(states, &ResponseTelemetryState::accountSlot));
    aggregate->accountCohortId = commonTelemetryValue(telemetryValues(states, &ResponseTelemetryState::accountCohortId));
    // Generation and reason are reported together: a differing generation, or
    // any source without active telemetry, yields null for both fields.
    const bool allSourcesHaveActiveGeneration =
      states.size() == sources.size() &&
      std::all_of(states.begin(), states.end(), [](const std::shared_ptr<ResponseTelemetryState>& state) { return state->activeGeneration.has_value(); });
    const std::optional<double> commonActiveGeneration =
      allSourcesHaveActiveGeneration ? commonTelemetryValue(telemetryValues(states, &ResponseTelemetryState::activeGeneration)) : std::nullopt;
    aggregate->activeGeneration = commonActiveGeneration;
    aggregate->activeTransitionReason =
      !commonActiveGeneration ? std::nullopt : commonTelemetryValue(telemetryValues(states, &ResponseTelemetryState::activeTransitionReason));
    std::vector<double> usedPercents;
    for (const std::shared_ptr<ResponseTelemetryState>& state : states) {
      if (state->quotaUsedPercent.hasValue()) usedPercents.push_back(*state->quotaUsedPercent);
    }
    aggregate->quotaUsedPercent = aggregatedQuotaUsedPercent(usedPercents, states);
    aggregate->completed = states.size() == sources.size() &&
                           std::all_of(states.begin(), states.end(), [](const std::shared_ptr<ResponseTelemetryState>& state) { return state->completed; });
    aggregate->semanticOutputObserved = aggregatedSemanticOutputObserved(states);
    std::vector<std::string> upstreamEventKinds;
    for (const std::shared_ptr<ResponseTelemetryState>& state : states) appendUnique(upstreamEventKinds, state->upstreamEventKinds);
    aggregate->upstreamEventKinds = std::move(upstreamEventKinds);
    for (const auto& state : states) {
      aggregate->inStreamRetries += state->inStreamRetries;
      for (const auto& failure : state->upstreamFailureEvents) {
        if (aggregate->upstreamFailureEvents.size() == MAX_UPSTREAM_FAILURE_EVENTS) break;
        aggregate->upstreamFailureEvents.push_back(failure);
      }
    }
    aggregate->streamTerminalType = aggregate->completed ? std::optional<std::string>("response.completed")
                                                         : commonTelemetryValue(telemetryValues(states, &ResponseTelemetryState::streamTerminalType));
    aggregate->failureKind = commonTelemetryValue(telemetryValues(states, &ResponseTelemetryState::failureKind));
    aggregate->responseCreatedObserved =
      states.size() == sources.size() &&
      std::all_of(states.begin(), states.end(), [](const std::shared_ptr<ResponseTelemetryState>& state) { return state->responseCreatedObserved; });
    aggregate->syntheticTerminalType = commonTelemetryValue(telemetryValues(states, &ResponseTelemetryState::syntheticTerminalType));
    aggregate->stream = false;
    aggregate->providerRequestId = states.size() == 1 ? states[0]->providerRequestId : std::nullopt;
    const auto earliestTiming = [&](TelemetryCount key) -> std::optional<double> {
      const std::vector<double> values = presentValues(states, key);
      return !values.empty() ? std::optional<double>(rt::Math::min(std::span<const double>(values))) : std::nullopt;
    };
    aggregate->firstProviderDispatchMs = earliestTiming(&ResponseTelemetryState::firstProviderDispatchMs);
    aggregate->firstProviderHeadersMs = earliestTiming(&ResponseTelemetryState::firstProviderHeadersMs);
    aggregate->firstCodexDispatchMs = earliestTiming(&ResponseTelemetryState::firstCodexDispatchMs);
    aggregate->firstCodexHeadersMs = earliestTiming(&ResponseTelemetryState::firstCodexHeadersMs);
    aggregate->firstUpstreamSseEventMs = earliestTiming(&ResponseTelemetryState::firstUpstreamSseEventMs);
    aggregate->firstSemanticCommitmentMs = earliestTiming(&ResponseTelemetryState::firstSemanticCommitmentMs);
    const std::vector<double> terminalTimes = presentValues(states, &ResponseTelemetryState::streamTerminalMs);
    aggregate->streamTerminalMs = !terminalTimes.empty() ? std::optional<double>(rt::Math::max(std::span<const double>(terminalTimes))) : std::nullopt;
    std::vector<std::string> attemptedProviders;
    for (const std::shared_ptr<ResponseTelemetryState>& state : states) appendUnique(attemptedProviders, state->attemptedProviders);
    aggregate->attemptedProviders = std::move(attemptedProviders);
    aggregate->removedProviderTriggerClass = commonTelemetryValue(telemetryValues(states, &ResponseTelemetryState::removedProviderTriggerClass));
    aggregate->removedProviderCircuitTransition = commonTelemetryValue(telemetryValues(states, &ResponseTelemetryState::removedProviderCircuitTransition));
    aggregate->removedProviderSelectedModel = commonTelemetryValue(telemetryValues(states, &ResponseTelemetryState::removedProviderSelectedModel));
    aggregate->removedProviderTaskType = commonTelemetryValue(telemetryValues(states, &ResponseTelemetryState::removedProviderTaskType));
    aggregate->removedProviderSemanticCommitment = commonTelemetryValue(telemetryValues(states, &ResponseTelemetryState::removedProviderSemanticCommitment));
    const std::vector<double> removedProviderLatencies = presentValues(states, &ResponseTelemetryState::removedProviderLatencyMs);
    aggregate->removedProviderLatencyMs =
      !removedProviderLatencies.empty() ? std::optional<double>(rt::Math::max(std::span<const double>(removedProviderLatencies))) : std::nullopt;
    aggregate->removedProviderTerminalStatus = commonTelemetryValue(telemetryValues(states, &ResponseTelemetryState::removedProviderTerminalStatus));
    return attachResponseTelemetry(std::move(target), std::move(aggregate));
  }

  std::optional<ResponseTelemetry> getResponseTelemetry(const rt::Response& response) {
    const std::shared_ptr<ResponseTelemetryState>* found = responseTelemetry.find(response);
    if (!found || !*found) return std::nullopt;
    const ResponseTelemetryState& state = **found;
    return ResponseTelemetry{
      .provider = state.provider ? *state.provider : upstreamProviderLabel(response),
      .fallbackReason = state.fallbackReason,
      .model = state.model,
      .reasoning = state.reasoning,
      .outputTokenAllowance = state.outputTokenAllowance,
      .rateLimitWaitMs = state.rateLimitWaitMs,
      .rateLimitFailoverModel = state.rateLimitFailoverModel,
      .inputTokens = state.inputTokens,
      .cachedInputTokens = state.cachedInputTokens,
      .cacheWriteInputTokens = state.cacheWriteInputTokens,
      .outputTokens = state.outputTokens,
      .totalTokens = state.totalTokens,
      .usageObserved = state.usageObserved,
      .usageTelemetryStatus = state.usageTelemetryStatus,
      .promptCacheKeyPresent = state.promptCacheKeyPresent,
      .promptCacheMode = state.promptCacheMode,
      .explicitBreakpointCount = state.explicitBreakpointCount,
      .accountSlot = state.accountSlot,
      .activeGeneration = state.activeGeneration,
      .activeTransitionReason = state.activeTransitionReason,
      .quotaUsedPercent = state.quotaUsedPercent,
      .completed = state.completed,
      .semanticOutputObserved = state.semanticOutputObserved,
      .upstreamEventKinds = state.upstreamEventKinds,
      .streamTerminalType = state.streamTerminalType,
      .failureKind = state.failureKind,
      .responseCreatedObserved = state.responseCreatedObserved,
      .syntheticTerminalType = state.syntheticTerminalType,
      .stream = state.stream,
      .providerRequestId = state.providerRequestId,
      .firstProviderDispatchMs = state.firstProviderDispatchMs,
      .firstProviderHeadersMs = state.firstProviderHeadersMs,
      .firstCodexDispatchMs = state.firstCodexDispatchMs,
      .firstCodexHeadersMs = state.firstCodexHeadersMs,
      .firstUpstreamSseEventMs = state.firstUpstreamSseEventMs,
      .firstSemanticCommitmentMs = state.firstSemanticCommitmentMs,
      .streamTerminalMs = state.streamTerminalMs,
      .attemptedProviders = state.attemptedProviders,
      .removedProviderTriggerClass = state.removedProviderTriggerClass,
      .removedProviderCircuitTransition = state.removedProviderCircuitTransition,
      .removedProviderSelectedModel = state.removedProviderSelectedModel,
      .removedProviderTaskType = state.removedProviderTaskType,
      .removedProviderSemanticCommitment = state.removedProviderSemanticCommitment,
      .removedProviderLatencyMs = state.removedProviderLatencyMs,
      .removedProviderTerminalStatus = state.removedProviderTerminalStatus,
      .inStreamRetries = state.inStreamRetries,
      .upstreamFailureEvents = state.upstreamFailureEvents,
    };
  }

  std::optional<std::string> getResponseAccountCohortId(const rt::Response& response) {
    const std::shared_ptr<ResponseTelemetryState>* found = responseTelemetry.find(response);
    if (!found || !*found) return std::nullopt;
    return (*found)->accountCohortId;
  }

  void recordAttemptedProvider(const UsageContext* context, const std::string& provider) {
    if (!context || !context->responseTelemetry) return;
    std::vector<std::string>& attempted = context->responseTelemetry->attemptedProviders;
    if (std::find(attempted.begin(), attempted.end(), provider) == attempted.end()) attempted.push_back(provider);
  }

  void selectRemovedProviderTelemetry(const UsageContext* context) {
    if (!context || !context->responseTelemetry) return;
    ResponseTelemetryState& telemetry = *context->responseTelemetry;
    telemetry.provider = "removed_provider";
    telemetry.accountSlot = std::nullopt;
    telemetry.accountCohortId = std::nullopt;
    telemetry.activeGeneration = std::nullopt;
    telemetry.activeTransitionReason = std::nullopt;
    telemetry.providerRequestId = std::nullopt;
  }

  namespace {
    bool safeEventIdentifier(std::string_view value) {
      return !value.empty() && value.size() <= 80 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
      });
    }

    void logUnknownResponseEventKind(const std::string& type) {
      if (!safeEventIdentifier(type)) return;
      static std::mutex mutex;
      static std::unordered_set<std::string> loggedKinds;
      {
        const std::lock_guard lock(mutex);
        // Keep the first names for the process lifetime so they cannot be
        // evicted and logged repeatedly by a noisy upstream.
        if (loggedKinds.size() >= 64 || !loggedKinds.insert(type).second) return;
      }
      rt::console::info("[ai.ubq.fi] unrecognized_responses_event", rt::jsonStringify(json::object{{"type", type}}));
    }

    // "firstProviderDispatchMs" | "firstProviderHeadersMs" | "firstCodexDispatchMs" | "firstCodexHeadersMs" |
    // "firstUpstreamSseEventMs" | "firstSemanticCommitmentMs" | "streamTerminalMs"
    using ResponseTelemetryTimingField = std::optional<double> ResponseTelemetryState::*;

    // Timings are elapsed from handler ingress using a monotonic clock. They are
    // telemetry only: missing context leaves the corresponding field unavailable.
    void recordResponseTiming(const UsageContext* context, ResponseTelemetryTimingField field) {
      ResponseTelemetryState* state = context ? context->responseTelemetry.get() : nullptr;
      const std::optional<double> startedAtMs = context ? context->startedAtMonotonicMs : std::nullopt;
      if (!state) return;
      if ((*state).*field || !startedAtMs || !rt::Number::isFinite(*startedAtMs)) return;
      (*state).*field = rt::Math::max(0.0, rt::Math::round(rt::performanceNow() - *startedAtMs));
    }
  }

  void recordFirstCodexDispatch(const UsageContext* context) {
    recordResponseTiming(context, &ResponseTelemetryState::firstProviderDispatchMs);
    recordResponseTiming(context, &ResponseTelemetryState::firstCodexDispatchMs);
  }

  void recordFirstCodexHeaders(const UsageContext* context) {
    recordResponseTiming(context, &ResponseTelemetryState::firstProviderHeadersMs);
    recordResponseTiming(context, &ResponseTelemetryState::firstCodexHeadersMs);
  }

  void recordFirstProviderDispatch(const UsageContext* context) { recordResponseTiming(context, &ResponseTelemetryState::firstProviderDispatchMs); }

  void recordFirstProviderHeaders(const UsageContext* context) { recordResponseTiming(context, &ResponseTelemetryState::firstProviderHeadersMs); }

  void recordRemovedProviderFields(const UsageContext* context, const RecordRemovedProviderFieldsFields& fields) {
    if (!context || !context->responseTelemetry) return;
    ResponseTelemetryState& telemetry = *context->responseTelemetry;
    if (fields.triggerClass != rt::undefined) telemetry.removedProviderTriggerClass = fields.triggerClass.toOptional();
    if (fields.circuitTransition != rt::undefined) {
      telemetry.removedProviderCircuitTransition = fields.circuitTransition.toOptional();
    }
    if (fields.selectedModel != rt::undefined) {
      telemetry.removedProviderSelectedModel = fields.selectedModel.toOptional();
    }
    if (fields.taskType != rt::undefined) telemetry.removedProviderTaskType = fields.taskType.toOptional();
    if (fields.semanticCommitment != rt::undefined) {
      telemetry.removedProviderSemanticCommitment = fields.semanticCommitment.toOptional();
    }
    if (fields.latencyMs != rt::undefined) telemetry.removedProviderLatencyMs = fields.latencyMs.toOptional();
    if (fields.terminalStatus != rt::undefined) {
      telemetry.removedProviderTerminalStatus = fields.terminalStatus.toOptional();
    }
  }

  rt::Promise<void> persistRemovedProviderFields([[maybe_unused]] const UsageContext* _context) {
    const json::object input;
    return provider::removed_provider_telemetry::recordRemovedProviderTelemetry(input).catch_([](const rt::Reason&) {});
  }

  rt::Promise<void> persistFailedRemovedProviderAttempt(const UsageContext* context, [[maybe_unused]] double _startedAtMonotonicMs, const std::string& triggerClass) {
    recordRemovedProviderFields(context, {.triggerClass = triggerClass, .terminalStatus = std::string("failed_before_commit")});
    return persistRemovedProviderFields(context);
  }

  void recordFirstUpstreamSseEvent(const UsageContext* context) { recordResponseTiming(context, &ResponseTelemetryState::firstUpstreamSseEventMs); }

  void recordFirstSemanticCommitment(const UsageContext* context) { recordResponseTiming(context, &ResponseTelemetryState::firstSemanticCommitmentMs); }

  void recordStreamTerminal(const UsageContext* context) { recordResponseTiming(context, &ResponseTelemetryState::streamTerminalMs); }

  namespace {
    constexpr std::string_view CHAT_RESPONSE_EVENT_KINDS[]{
      "response.created",
      "response.in_progress",
      "response.queued",
      "response.output_text.delta",
      "response.output_text.done",
      "response.refusal.delta",
      "response.refusal.done",
      "response.content_part.added",
      "response.content_part.done",
      "response.reasoning_summary_text.delta",
      "response.reasoning_summary_text.done",
      "response.reasoning_text.delta",
      "response.reasoning_text.done",
      "response.reasoning_summary_part.added",
      "response.reasoning_summary_part.done",
      "response.custom_tool_call_input.delta",
      "response.custom_tool_call_input.done",
      "response.output_item.added",
      "response.output_item.done",
      "response.function_call_arguments.delta",
      "response.function_call_arguments.done",
      "response.output",
      "response.completed",
      "response.failed",
      "response.incomplete",
      "error",
    };

    std::string boundedResponseEventKind(const std::string& type) {
      const bool known = std::find(std::begin(CHAT_RESPONSE_EVENT_KINDS), std::end(CHAT_RESPONSE_EVENT_KINDS), std::string_view(type)) != std::end(CHAT_RESPONSE_EVENT_KINDS);
      if (known) return type;
      logUnknownResponseEventKind(type);
      return "unrecognized";
    }
  }

  void recordUpstreamResponsesFailure(ResponseTelemetryState& telemetry, const responses_stream::ResponsesStreamEvent& event) {
    if (event.type != "error" && event.type != "response.failed") return;
    if (telemetry.upstreamFailureEvents.size() == MAX_UPSTREAM_FAILURE_EVENTS) return;
    const auto* error = responses_stream::responsesFailurePayload(event);
    const auto code = error ? rt::getString(*error, "code") : std::nullopt;
    const auto type = error && error != &event.value ? rt::getString(*error, "type") : std::nullopt;
    telemetry.upstreamFailureEvents.push_back(json::object{
      {"type", event.type},
      {"code", code && safeEventIdentifier(*code) ? json::value(*code) : json::value(nullptr)},
      {"error_type", type && safeEventIdentifier(*type) ? json::value(*type) : json::value(nullptr)},
    });
  }

  // An incomplete terminal is a recorded failure only when its reason names a
  // gateway, provider or transport fault; unrecognized reasons are ignored.
  std::optional<std::string> responseIncompleteFailureKind(const responses_stream::ResponsesStreamEvent& event) {
    const json::value* response = rt::get(event.value, "response");
    if (!utils::isRecord(response)) return std::nullopt;
    const json::value* details = rt::get(*response, "incomplete_details");
    if (!utils::isRecord(details)) return std::nullopt;
    const std::optional<std::string_view> reason = rt::getString(*details, "reason");
    if (!reason || reason->empty()) return std::nullopt;
    // This character class is case-insensitive, so the upper-case range already
    // covers the lower-case letters.
    static const rt::Regex FAILURE_REASON("^(?:gateway|provider|upstream|server|network|timeout|deadline)[A-Z0-9_.:-]*$", "i");
    if (!FAILURE_REASON.test(std::string(*reason))) return std::nullopt;
    return "response_incomplete:" + std::string(*reason);
  }

  void recordResponsesEventTelemetry(const UsageContext* context, const responses_stream::ResponsesStreamEvent& event) {
    if (!context || !context->responseTelemetry) return;
    ResponseTelemetryState& telemetry = *context->responseTelemetry;
    const std::string eventKind = boundedResponseEventKind(event.type);
    if (std::find(telemetry.upstreamEventKinds.begin(), telemetry.upstreamEventKinds.end(), eventKind) == telemetry.upstreamEventKinds.end()) {
      telemetry.upstreamEventKinds.push_back(eventKind);
    }
    if (event.type == "response.created") telemetry.responseCreatedObserved = true;
    if (event.type == "response.incomplete") {
      std::optional<std::string> failureKind = responseIncompleteFailureKind(event);
      if (failureKind) telemetry.failureKind = std::move(failureKind);
    }
    if (responses_failover_stream::isSyntheticResponsesFailureEvent(event)) {
      telemetry.syntheticTerminalType = event.type == "response.failed" || event.type == "error" ? std::optional<std::string>(event.type) : std::nullopt;
    }
  }

  namespace {
    responses_stream::ResponsesStreamFailureKind failureKindFromError(const rt::Reason& error) {
      const responses_stream::ResponsesStreamError* streamError = responses_stream::responsesStreamErrorOf(error);
      return streamError ? streamError->kind : "read_error";
    }
  }

  void recordResponsesFailureTelemetry(const UsageContext* context, const rt::Reason& error,
                                       const responses_failover_stream::OwnedResponsesStreamFailureDetails* details) {
    if (!context || !context->responseTelemetry) return;
    ResponseTelemetryState& telemetry = *context->responseTelemetry;
    telemetry.failureKind = details ? details->failureKind : failureKindFromError(error);
    if (details) {
      telemetry.responseCreatedObserved = details->responseCreatedObserved;
      telemetry.semanticOutputObserved = details->semanticCommitmentObserved;
      telemetry.syntheticTerminalType = details->syntheticTerminalType;
    }
  }

  rt::Task<rt::Response> runWithResponseTelemetry(std::optional<UsageContext> context, std::function<rt::Task<rt::Response>(UsageContext)> run) {
    std::shared_ptr<ResponseTelemetryState> state = createResponseTelemetryState();
    const UsageContext telemetryContext = withResponseTelemetryContext(context, state);
    rt::Promise<rt::Response> running(run(telemetryContext));
    rt::Response response = co_await running.wait();
    // Attempt-level terminal types can be abandoned during failover. Record a
    // precommit terminal time only after the handler has selected the final
    // response, and only when an upstream SSE event was actually observed.
    if (state->firstUpstreamSseEventMs && !state->firstSemanticCommitmentMs && state->streamTerminalType && !state->streamTerminalMs) {
      recordStreamTerminal(&telemetryContext);
    }
    rt::Response attached = attachResponseTelemetry(std::move(response), state);
    co_return attached;
  }

  namespace {
    // Readonly<{ value: number | null; invalid: boolean }>
    struct ParsedUsageToken {
      std::optional<double> value;
      bool invalid;
    };

    ParsedUsageToken parseUsageToken(const json::value* value, bool present) {
      if (!present) return {.value = std::nullopt, .invalid = false};
      const std::optional<double> number = rt::getNumber(value);
      if (!number || !rt::Number::isSafeInteger(*number) || *number < 0) {
        return {.value = std::nullopt, .invalid = true};
      }
      return {.value = number, .invalid = false};
    }
  }

  std::optional<UsageTokens> extractUsageTokens(const json::value* value) {
    if (!value) return std::nullopt;
    return extractUsageTokens(*value);
  }

  std::optional<UsageTokens> extractUsageTokens(const json::value& value) {
    if (!utils::isRecord(value) || value.is_array()) {
      return UsageTokens{
        .inputTokens = std::nullopt,
        .cachedInputTokens = std::nullopt,
        .cacheWriteInputTokens = std::nullopt,
        .outputTokens = std::nullopt,
        .totalTokens = std::nullopt,
        .status = "invalid",
      };
    }
    const json::object& record = value.get_object();
    const auto has = [&](std::string_view key) { return rt::hasOwn(record, key); };
    const ParsedUsageToken inputTokens = parseUsageToken(rt::get(record, "input_tokens"), has("input_tokens"));
    const ParsedUsageToken outputTokens = parseUsageToken(rt::get(record, "output_tokens"), has("output_tokens"));
    const ParsedUsageToken totalTokens = parseUsageToken(rt::get(record, "total_tokens"), has("total_tokens"));
    const bool detailsPresent = has("input_tokens_details");
    const json::value* detailsValue = rt::get(record, "input_tokens_details");
    const json::object* details = detailsPresent && utils::isRecord(detailsValue) && !detailsValue->is_array() ? &detailsValue->get_object() : nullptr;
    const ParsedUsageToken cachedInputTokens =
      parseUsageToken(details ? rt::get(*details, "cached_tokens") : nullptr, details != nullptr && rt::hasOwn(*details, "cached_tokens"));
    const ParsedUsageToken cacheWriteInputTokens =
      parseUsageToken(details ? rt::get(*details, "cache_write_tokens") : nullptr, details != nullptr && rt::hasOwn(*details, "cache_write_tokens"));

    const bool coreMissing = !inputTokens.value || !outputTokens.value || !totalTokens.value;
    // OpenAI documents cached_tokens for every response, including an explicit
    // zero below the cacheability threshold. cache_write_tokens remains
    // model-dependent, so its absence does not downgrade otherwise complete
    // cache-read telemetry.
    const bool cacheReadMissing = !cachedInputTokens.value;
    // cache_write_tokens is an independent dimension: an input can be both
    // newly cached and partly served from cache. cached_tokens, however, is a
    // subset of the request input and may never exceed input_tokens.
    const bool cachedTokensExceedInput = inputTokens.value && cachedInputTokens.value && *cachedInputTokens.value > *inputTokens.value;
    const bool inconsistentTotals = !coreMissing && *inputTokens.value + *outputTokens.value != *totalTokens.value;
    const bool invalid = inputTokens.invalid || outputTokens.invalid || totalTokens.invalid || (detailsPresent && details == nullptr) || cachedInputTokens.invalid ||
                         cacheWriteInputTokens.invalid || cachedTokensExceedInput || inconsistentTotals;

    UsageTelemetryStatus status;
    if (invalid) status = "invalid";
    else if (coreMissing || cacheReadMissing) status = "partial";
    else status = "reported";

    UsageTokens usage{
      .inputTokens = inputTokens.value,
      .cachedInputTokens = cachedInputTokens.value,
      .cacheWriteInputTokens = cacheWriteInputTokens.value,
      .outputTokens = outputTokens.value,
      .totalTokens = totalTokens.value,
      .status = std::move(status),
    };
    if (has("buyer_cost_micro")) usage.buyerCostMicro = rt::orNull(parseUsageToken(rt::get(record, "buyer_cost_micro"), true).value);
    return usage;
  }

  std::optional<json::object> toChatUsage(const std::optional<UsageTokens>& usage) {
    if (!usage) return std::nullopt;
    if (!usage->inputTokens || !usage->outputTokens || !usage->totalTokens) {
      return std::nullopt;
    }
    json::object promptTokenDetails;
    // Prompt Caching documents both fields on Chat Completions usage details.
    if (usage->cachedInputTokens) rt::objectSet(promptTokenDetails, "cached_tokens", rt::makeNumber(*usage->cachedInputTokens));
    if (usage->cacheWriteInputTokens) rt::objectSet(promptTokenDetails, "cache_write_tokens", rt::makeNumber(*usage->cacheWriteInputTokens));
    json::object chatUsage;
    rt::objectDefine(chatUsage, "prompt_tokens", rt::makeNumber(*usage->inputTokens));
    rt::objectDefine(chatUsage, "completion_tokens", rt::makeNumber(*usage->outputTokens));
    rt::objectDefine(chatUsage, "total_tokens", rt::makeNumber(*usage->totalTokens));
    if (!promptTokenDetails.empty()) rt::objectDefine(chatUsage, "prompt_tokens_details", std::move(promptTokenDetails));
    return chatUsage;
  }

  PromptCacheMode promptCacheModeFor(const json::object& rawRecord) {
    const json::value* promptCacheOptions = rt::get(rawRecord, "prompt_cache_options");
    const json::object* options =
      utils::isRecord(promptCacheOptions) && !promptCacheOptions->is_array() ? &promptCacheOptions->get_object() : nullptr;
    if (options && rt::getString(*options, "mode") == std::optional<std::string_view>("explicit")) return "explicit";
    // OpenAI's cache policy defaults to implicit whenever options are supplied
    // without an explicit mode (for example, a ttl-only configuration).
    if (options) return "implicit";
    if (rt::hasOwn(rawRecord, "prompt_cache_retention")) return "legacy_retention";
    return "unspecified";
  }

  namespace {
    // `contentItem.prompt_cache_breakpoint.mode === "explicit"` over a typed message content item.
    bool explicitBreakpoint(const std::optional<::uos::types::PromptCacheBreakpoint>& breakpoint) { return breakpoint && breakpoint->mode == "explicit"; }
  }

  double countExplicitPromptCacheBreakpoints(const std::vector<::uos::types::ResponseInputItem>& input) {
    double count = 0;
    const auto countContent = [&](const json::value* content) {
      if (!content || !content->is_array()) return;
      for (const json::value& contentItem : content->get_array()) {
        if (!utils::isRecord(contentItem) || !utils::isRecord(rt::get(contentItem, "prompt_cache_breakpoint"))) continue;
        if (rt::getString(*rt::get(contentItem, "prompt_cache_breakpoint"), "mode") == std::optional<std::string_view>("explicit")) count += 1;
      }
    };
    for (const ::uos::types::ResponseInputItem& item : input) {
      if (const auto* message = std::get_if<::uos::types::ResponseMessageItem>(&item)) {
        // A message's content items are records; only the input kinds carry a breakpoint.
        for (const ::uos::types::MessageContentItem& contentItem : message->content) {
          if (const auto* text = std::get_if<::uos::types::MessageContentItemInputText>(&contentItem)) {
            if (explicitBreakpoint(text->prompt_cache_breakpoint)) count += 1;
          } else if (const auto* image = std::get_if<::uos::types::MessageContentItemInputImage>(&contentItem)) {
            if (explicitBreakpoint(image->prompt_cache_breakpoint)) count += 1;
          } else if (const auto* file = std::get_if<::uos::types::MessageContentItemInputFile>(&contentItem)) {
            if (explicitBreakpoint(file->prompt_cache_breakpoint)) count += 1;
          }
        }
        continue;
      }
      const json::object& record = std::get<json::object>(item);
      countContent(rt::get(record, "content"));
      if (rt::getString(record, "type") == std::optional<std::string_view>("function_call_output")) countContent(rt::get(record, "output"));
    }
    return count;
  }

  bool promptCacheKeyPresent(const json::object& rawRecord) {
    const std::optional<std::string_view> key = rt::getString(rawRecord, "prompt_cache_key");
    return key && !rt::trim(*key).empty();
  }

  std::optional<UsageTokens> extractChatUsageTokens(const json::value* value) {
    if (!value) return std::nullopt;
    return extractChatUsageTokens(*value);
  }

  std::optional<UsageTokens> extractChatUsageTokens(const json::value& value) {
    if (!utils::isRecord(value) || value.is_array()) return std::nullopt;
    const json::object& record = value.get_object();
    json::object responsesUsage;
    constexpr std::array<std::pair<std::string_view, std::string_view>, 5> KEYS{{
      {"prompt_tokens", "input_tokens"},
      {"completion_tokens", "output_tokens"},
      {"total_tokens", "total_tokens"},
      {"prompt_tokens_details", "input_tokens_details"},
      {"buyer_cost_micro", "buyer_cost_micro"},
    }};
    for (const auto& [chatKey, responsesKey] : KEYS) {
      if (rt::hasOwn(record, chatKey)) rt::objectSet(responsesUsage, responsesKey, *rt::get(record, chatKey));
    }
    // Preserve field presence and exact values for the shared strict parser:
    // truncating a fractional counter can fabricate a measured cache miss.
    return extractUsageTokens(json::value(std::move(responsesUsage)));
  }

  rt::Promise<void> recordRequestUsage(const UsageContext* context, const RecordRequestUsageDetails& details) {
    if (context && context->responseTelemetry) {
      ResponseTelemetryState& telemetry = *context->responseTelemetry;
      telemetry.model = details.model;
      telemetry.reasoning = details.reasoning;
      telemetry.stream = details.stream;
      telemetry.promptCacheKeyPresent = details.promptCacheKeyPresent.value_or(false);
      telemetry.promptCacheMode = details.promptCacheMode.value_or("unspecified");
      telemetry.explicitBreakpointCount = details.explicitBreakpointCount.value_or(0);
    }
    return rt::Promise<void>::resolve();
  }

  rt::Promise<void> recordCompletionUsage(const UsageContext* context, const std::optional<UsageTokens>& usage, bool completed) {
    recordTerminalUsage(context, usage, completed);
    return rt::Promise<void>::resolve();
  }

  void recordTerminalUsage(const UsageContext* context, const std::optional<UsageTokens>& usage, bool completed) {
    if (!context || !context->responseTelemetry) return;
    ResponseTelemetryState& telemetry = *context->responseTelemetry;
    telemetry.inputTokens = usage ? usage->inputTokens : std::nullopt;
    telemetry.cachedInputTokens = usage ? usage->cachedInputTokens : std::nullopt;
    telemetry.cacheWriteInputTokens = usage ? usage->cacheWriteInputTokens : std::nullopt;
    telemetry.outputTokens = usage ? usage->outputTokens : std::nullopt;
    telemetry.totalTokens = usage ? usage->totalTokens : std::nullopt;
    telemetry.usageObserved = usage.has_value();
    telemetry.usageTelemetryStatus = usage ? usage->status : "missing";
    telemetry.completed = completed;
    try {
      if (context->onTerminalUsage) context->onTerminalUsage(usage, completed);
    } catch (...) {
      // A test/observability callback cannot alter response delivery.
    }
  }

  rt::Promise<void> recordErrorUsage([[maybe_unused]] const UsageContext* _context) { return rt::Promise<void>::resolve(); }

  void recordStreamTerminalType(const UsageContext* context, const ResponseStreamTerminalType& terminalType) {
    if (!context || !context->responseTelemetry) return;
    ResponseTelemetryState& telemetry = *context->responseTelemetry;
    telemetry.streamTerminalType = terminalType;
    if (telemetry.firstSemanticCommitmentMs) recordStreamTerminal(context);
  }

  ResponseStreamTerminalType classifyStreamFailure(const rt::Reason& error, const rt::AbortSignal& signal, const rt::AbortSignal& downstreamSignal) {
    if (isTimeoutFailure(error, signal.reason(), downstreamSignal.reason())) return "deadline";
    if (downstreamSignal.aborted()) return "cancelled";
    if (signal.aborted()) return "deadline";
    const responses_stream::ResponsesStreamError* streamError = responses_stream::responsesStreamErrorOf(error);
    if (streamError && streamError->kind == "inactivity_timeout") return "deadline";
    if (streamError && streamError->kind == "premature_eof") return "eof";
    return "error";
  }

  bool isTimeoutFailure(const std::vector<rt::Reason>& values) {
    return std::any_of(values.begin(), values.end(), [](const rt::Reason& value) {
      const codex::auth::CodexError* codexError = codex::auth::codexErrorOf(value);
      return (codexError && codexError->code == "gateway_timeout") || (rt::isError(value) && rt::errorName(value) == std::optional<std::string>("TimeoutError"));
    });
  }

  ResponseStreamTerminalType classifyPreHeaderFailure(const rt::Reason& error, const rt::AbortSignal& signal, const rt::AbortSignal& downstreamSignal) {
    if (isTimeoutFailure(error, signal.reason(), downstreamSignal.reason())) return "deadline";
    if (downstreamSignal.aborted()) return "cancelled";
    if (signal.aborted()) return "deadline";
    return "error";
  }

  rt::Response streamErrorResponse(double status, const std::string& message, const std::string& code, const UpstreamProvider& provider,
                                   const std::vector<std::string>& warnings) {
    return streamErrorResponse(status, message, code, provider, warnings, std::nullopt, rt::undefined);
  }

  rt::Response streamErrorResponse(double status, const std::string& message, const std::string& code, const UpstreamProvider& provider,
                                   const std::vector<std::string>& warnings, std::optional<std::string> type) {
    return streamErrorResponse(status, message, code, provider, warnings, std::move(type), rt::undefined);
  }

  rt::Response streamErrorResponse(double status, const std::string& message, const std::string& code, const UpstreamProvider& provider,
                                   const std::vector<std::string>& warnings, std::optional<std::string> type, rt::Nullable<std::string> param) {
    std::vector<std::string> mergedWarnings;
    appendUnique(mergedWarnings, warnings);
    const bool hasAuthWarning = std::find(mergedWarnings.begin(), mergedWarnings.end(), codex::auth::CODEX_AUTH_REAUTH_WARNING) != mergedWarnings.end();
    rt::Headers headers{{"x-uos-upstream", provider}};
    if (!mergedWarnings.empty()) headers.append("x-uos-warning", rt::join(mergedWarnings, ", "));
    http::OpenaiErrorOptions options;
    if (type && !type->empty()) options.type = std::move(type);
    if (param != rt::undefined) options.param = param.toOptional();
    options.headers = std::move(headers);
    return http::openaiError(status, hasAuthWarning ? message + " " + std::string(codex::auth::CODEX_AUTH_REAUTH_MESSAGE) : message, code, std::move(options));
  }

  rt::Response streamPreflightFailureResponse(const ResponseStreamTerminalType& terminalType, const UpstreamProvider& provider) {
    const std::vector<std::string> warnings;
    return streamPreflightFailureResponse(terminalType, provider, warnings);
  }

  rt::Response streamPreflightFailureResponse(const ResponseStreamTerminalType& terminalType, const UpstreamProvider& provider,
                                              const std::vector<std::string>& warnings) {
    if (terminalType == "cancelled") {
      return streamErrorResponse(499, "Request was cancelled.", "request_cancelled", provider, warnings, std::string("server_error"), nullptr);
    }
    if (terminalType == "deadline") {
      return streamErrorResponse(504, "Upstream stream exceeded the gateway deadline before its first SSE event.", "gateway_timeout", provider, warnings,
                                 std::string("server_error"));
    }
    return streamErrorResponse(502, "Codex upstream stream ended unexpectedly.", "codex_upstream_stream_error", provider, warnings);
  }
}
