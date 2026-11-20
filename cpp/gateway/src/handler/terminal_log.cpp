// Terminal request logging and delivery decoration, split out of src/handler.ts.
#include "uos/handler/terminal_log.hpp"

#include "uos/cache/prompt_analytics_queue.hpp"
#include "uos/config.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/responses_native_headers.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/runtime/http_tasks.hpp"
#include "uos/sentinel/incident_outbox.hpp"
#include "uos/sentinel/replay_store.hpp"
#include "uos/upstream_auth.hpp"

#include <boost/json/monotonic_resource.hpp>

#include <cstddef>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace uos::handler::terminal_log {
  namespace {
    namespace replay = sentinel::replay_observation;
    namespace model = sentinel::replay_model;
    using openai_telemetry::ResponseTelemetry;
    using BodyObservation = std::optional<model::SentinelClientBodyObservation>;
    using Controller = rt::ReadableStreamDefaultController<rt::ByteChunk>;

    bool skipsAdminError(const WithTerminalRequestLogInput& input, int status, std::string_view provider) {
      // The default admin writer deliberately omits gateway authentication failures.
      return !input.recordAdminError && status == 401 && provider == "gateway";
    }

    // rt::allSettled has a homogeneous result type; this batch discards heterogeneous values.
    template <class... T> rt::Promise<void> settleWrites(const rt::Promise<T>&... writes) {
      struct Batch {
        rt::Promise<void> result;
        std::size_t remaining;
      };
      auto batch = std::make_shared<Batch>(Batch{rt::Promise<void>(), sizeof...(T)});
      const auto settled = [batch] { if (--batch->remaining == 0) batch->result.state()->fulfill(); };
      // The logging frame already holds the input promises until the batch completes.
      (writes.state()->react(settled), ...);
      return batch->result;
    }

    template <class Writer, class... Args>
    auto invokeWriter(Writer writer, Args&&... args) {
      auto owner = std::make_shared<Writer>(std::move(writer));
      auto pending = (*owner)(std::forward<Args>(args)...);
      // A later writer can throw before the batch starts observing the writes.
      pending.state()->react([owner = std::move(owner)] { (void)owner; });
      return pending;
    }

    struct LogInput {
      WithTerminalRequestLogInput input;
      rt::Response response;
      std::optional<double> downstreamDrainedAtMonotonicMs;
      http::DeliveryOutcome deliveryOutcome;
      bool streamReadFailure;
      bool suppressSentinelReplay;
      std::function<rt::Task<BodyObservation>()> resolveClientBodyObservation;
    };

    rt::Task<void> logTerminalRequest(LogInput data) {
      auto& input = data.input;
      const auto& telemetryResponse = input.telemetryResponse ? *input.telemetryResponse : data.response;
      const auto telemetry = openai_telemetry::getResponseTelemetry(telemetryResponse);
      const auto accountCohortId = openai_telemetry::getResponseAccountCohortId(telemetryResponse);
      const auto nativeHttpFailure = responses_native_headers::originalNativeHttpFailure(input.request);
      const double latencyMs = rt::Math::max(0.0, rt::Math::round(rt::performanceNow() - input.startedAtMonotonicMs));
      std::optional<double> downstreamDrainMs;
      if (telemetry && telemetry->stream == true && telemetry->firstSemanticCommitmentMs && telemetry->streamTerminalMs &&
          data.downstreamDrainedAtMonotonicMs) {
        downstreamDrainMs = rt::Math::max(0.0, rt::Math::round(*data.downstreamDrainedAtMonotonicMs - input.startedAtMonotonicMs) - *telemetry->streamTerminalMs);
      }
      const std::string provider = telemetry ? telemetry->provider : data.response.headers().get("x-uos-upstream").value_or("gateway");
      const std::optional<bool> stream = data.streamReadFailure ? std::optional<bool>(telemetry ? telemetry->stream.value_or(true) : true)
                                                              : telemetry ? telemetry->stream : std::nullopt;
      const std::optional<std::string> terminalType = data.streamReadFailure
        ? std::optional<std::string>(telemetry ? telemetry->streamTerminalType.value_or("error") : "error")
        : telemetry ? telemetry->streamTerminalType : std::nullopt;
      const std::optional<std::string> failureKind = data.streamReadFailure
        ? std::optional<std::string>(telemetry ? telemetry->failureKind.value_or("gateway_stream_read_error") : "gateway_stream_read_error")
        : telemetry ? telemetry->failureKind : std::nullopt;
      auto recordText = [&] {
        // The keys and ordinary string values fit in the scratch buffer. Larger fields can allocate upstream.
        alignas(std::max_align_t) unsigned char buffer[4096];
        json::monotonic_resource storage(buffer);
        json::value record(json::object_kind, &storage);
        auto& terminal = record.get_object();
        terminal.reserve(56);
        const auto putOptional = [&terminal](std::string_view key, const auto& field) {
          if (!field) terminal.emplace(key, nullptr);
          else if constexpr (std::is_same_v<std::remove_cvref_t<decltype(*field)>, double>) terminal.emplace(key, rt::makeNumber(*field));
          else terminal.emplace(key, *field);
        };
        const auto putTelemetry = [&](std::string_view key, auto member) {
          if (telemetry) putOptional(key, (*telemetry).*member);
          else terminal.emplace(key, nullptr);
        };
        terminal.emplace("request_id", input.requestId);
        if (nativeHttpFailure) terminal.emplace("native_http_failure", json::value_from(*nativeHttpFailure, &storage));
        terminal.emplace("route", input.route);
        terminal.emplace("status", data.response.status());
        terminal.emplace("provider", provider);
        terminal.emplace("latency_ms", rt::makeNumber(latencyMs));
        putOptional("admission_wait_ms", input.admissionWaitMs);
        putOptional("admission_stage", input.admissionStage);
        putTelemetry("first_provider_dispatch_ms", &ResponseTelemetry::firstProviderDispatchMs);
        putTelemetry("first_provider_headers_ms", &ResponseTelemetry::firstProviderHeadersMs);
        putTelemetry("first_codex_dispatch_ms", &ResponseTelemetry::firstCodexDispatchMs);
        putTelemetry("first_codex_headers_ms", &ResponseTelemetry::firstCodexHeadersMs);
        putTelemetry("first_upstream_sse_event_ms", &ResponseTelemetry::firstUpstreamSseEventMs);
        putTelemetry("first_semantic_commitment_ms", &ResponseTelemetry::firstSemanticCommitmentMs);
        putTelemetry("stream_terminal_ms", &ResponseTelemetry::streamTerminalMs);
        putOptional("downstream_drain_ms", downstreamDrainMs);
        terminal.emplace("delivery_outcome", data.deliveryOutcome);
        putTelemetry("model", &ResponseTelemetry::model);
        putTelemetry("reasoning", &ResponseTelemetry::reasoning);
        putTelemetry("output_token_allowance", &ResponseTelemetry::outputTokenAllowance);
        putTelemetry("rate_limit_wait_ms", &ResponseTelemetry::rateLimitWaitMs);
        putTelemetry("rate_limit_failover_model", &ResponseTelemetry::rateLimitFailoverModel);
        putTelemetry("provider_request_id", &ResponseTelemetry::providerRequestId);
        putTelemetry("input_tokens", &ResponseTelemetry::inputTokens);
        putTelemetry("cached_input_tokens", &ResponseTelemetry::cachedInputTokens);
        putTelemetry("cache_write_input_tokens", &ResponseTelemetry::cacheWriteInputTokens);
        putTelemetry("output_tokens", &ResponseTelemetry::outputTokens);
        putTelemetry("total_tokens", &ResponseTelemetry::totalTokens);
        terminal.emplace("usage_observed", telemetry ? telemetry->usageObserved : false);
        terminal.emplace("usage_telemetry_status", telemetry ? std::string_view(telemetry->usageTelemetryStatus) : "missing");
        terminal.emplace("prompt_cache_key_present", telemetry ? telemetry->promptCacheKeyPresent : false);
        terminal.emplace("prompt_cache_mode", telemetry ? std::string_view(telemetry->promptCacheMode) : "unspecified");
        terminal.emplace("explicit_breakpoint_count", rt::makeNumber(telemetry ? telemetry->explicitBreakpointCount : 0));
        putTelemetry("account_slot", &ResponseTelemetry::accountSlot);
        putOptional("account_cohort_id", accountCohortId);
        putTelemetry("active_generation", &ResponseTelemetry::activeGeneration);
        putTelemetry("active_transition_reason", &ResponseTelemetry::activeTransitionReason);
        putTelemetry("fallback_reason", &ResponseTelemetry::fallbackReason);
        putTelemetry("semantic_output_observed", &ResponseTelemetry::semanticOutputObserved);
        terminal.emplace("upstream_event_kinds", telemetry ? json::value_from(telemetry->upstreamEventKinds, &storage) : json::value(json::array_kind, &storage));
        putOptional("stream", stream);
        putOptional("stream_terminal_type", terminalType);
        putOptional("failure_kind", failureKind);
        if (telemetry && telemetry->upstreamAuthFollowerStale) terminal.emplace("upstream_auth_reason", upstream_auth::FOLLOWER_STALE);
        terminal.emplace("response_created_observed", telemetry ? telemetry->responseCreatedObserved : false);
        putTelemetry("synthetic_terminal_type", &ResponseTelemetry::syntheticTerminalType);
        terminal.emplace("attempted_providers", telemetry ? json::value_from(telemetry->attemptedProviders, &storage) : json::value(json::array_kind, &storage));
        putTelemetry("removed_provider_trigger_class", &ResponseTelemetry::removedProviderTriggerClass);
        putTelemetry("removed_provider_circuit_transition", &ResponseTelemetry::removedProviderCircuitTransition);
        putTelemetry("removed_provider_selected_model", &ResponseTelemetry::removedProviderSelectedModel);
        putTelemetry("removed_provider_task_type", &ResponseTelemetry::removedProviderTaskType);
        putTelemetry("removed_provider_latency_ms", &ResponseTelemetry::removedProviderLatencyMs);
        putTelemetry("removed_provider_terminal_status", &ResponseTelemetry::removedProviderTerminalStatus);
        putTelemetry("removed_provider_semantic_commitment", &ResponseTelemetry::removedProviderSemanticCommitment);
        terminal.emplace("git_sha", config::runtimeGitSha());
        terminal.emplace("deno_revision", config::runtimeDeploymentId());
        putOptional("router_revision", data.response.headers().get("x-uos-router-revision"));
        return rt::jsonStringify(record);
      }();
      rt::console::info("[ai.ubq.fi] request_terminal", std::move(recordText));
      const bool completed = !data.streamReadFailure && telemetry && telemetry->completed;
      cache::telemetry_gate::PromptCacheTelemetryEvent telemetryEvent{
        .provider = provider, .model = telemetry ? telemetry->model : std::nullopt, .route = input.route, .status = static_cast<double>(data.response.status()),
        .completed = completed, .usageTelemetryStatus = telemetry ? telemetry->usageTelemetryStatus : "missing",
        .cacheWriteTokensPresent = telemetry && telemetry->cacheWriteInputTokens.has_value(),
      };
      auto telemetryWrite = input.recordTelemetry ? invokeWriter(std::move(input.recordTelemetry), telemetryEvent)
                                                 : rt::Promise(cache::telemetry_gate::recordPromptCacheTelemetry(telemetryEvent));
      // Only aggregate analytics is queued; durable gates and required evidence stay awaited.
      cache::prompt_analytics_core::PromptCacheAnalyticsEvent analyticsEvent{
        .provider = provider, .model = rt::orNull(telemetry ? telemetry->model : std::nullopt), .route = input.route,
        .status = static_cast<double>(data.response.status()), .completed = completed,
        .usageTelemetryStatus = telemetry ? telemetry->usageTelemetryStatus : "missing", .inputTokens = telemetry ? telemetry->inputTokens : std::nullopt,
        .cachedInputTokens = telemetry ? telemetry->cachedInputTokens : std::nullopt,
        .cacheWriteInputTokens = telemetry ? telemetry->cacheWriteInputTokens : std::nullopt,
        .promptCacheKeyPresent = telemetry ? telemetry->promptCacheKeyPresent : false,
        .promptCacheMode = telemetry ? telemetry->promptCacheMode : "unspecified", .fallbackReason = rt::orNull(telemetry ? telemetry->fallbackReason : std::nullopt),
      };
      auto cacheAnalyticsWrite = input.recordCacheAnalytics ? invokeWriter(std::move(input.recordCacheAnalytics), analyticsEvent)
        : rt::Promise(cache::prompt_analytics_queue::enqueuePromptCacheAnalytics(analyticsEvent));
      authority::native_paid_contracts::TerminalUsageRollupInput rollupInput{
        .model = telemetry ? telemetry->model : std::nullopt, .provider = provider, .request_id = input.requestId,
        .request_created_at_ms = input.requestStartedAtMs ? *input.requestStartedAtMs : rt::dateNow(), .input_tokens = telemetry ? telemetry->inputTokens : std::nullopt,
        .cached_input_tokens = telemetry ? telemetry->cachedInputTokens : std::nullopt, .output_tokens = telemetry ? telemetry->outputTokens : std::nullopt,
      };
      const auto usageRollupWrite = (input.recordUsageRollup ? invokeWriter(std::move(input.recordUsageRollup), rollupInput)
        : rt::Promise(paid_fallback::rollups::recordTerminalUsageRollup(rollupInput))).catch_([](rt::Reason) { return false; });
      if (!input.sentinelReplayInput && skipsAdminError(input, data.response.status(), provider)) {
        const auto writes = settleWrites(telemetryWrite, cacheAnalyticsWrite, usageRollupWrite);
        co_await writes.wait();
        co_return;
      }
      const model::SentinelFailureObservation replayObservation{
        .status = static_cast<double>(data.response.status()), .stream = stream, .completed = completed, .terminal_type = terminalType,
        .failure_kind = failureKind, .synthetic_terminal_type = telemetry ? telemetry->syntheticTerminalType : std::nullopt, .provider_route = provider,
      };
      const auto zero = rt::scopeExit([&input] { replay::zeroSentinelReplayInput(input.sentinelReplayInput); });
      auto resolvingBody = data.resolveClientBodyObservation();
      const auto clientBodyObservation = co_await std::move(resolvingBody);
      co_await rt::microtask();
      const auto clientObservation = replay::resolveSentinelClientFailureObservation(replayObservation, clientBodyObservation);
      std::optional<rt::Promise<replay::SentinelReplayPersistResult>> replayWrite;
      if (input.sentinelReplayInput && !data.suppressSentinelReplay && replay::shouldPersistSentinelReplay(replayObservation, clientObservation)) {
        replayWrite = input.persistSentinelReplay ? invokeWriter(std::move(input.persistSentinelReplay), *input.sentinelReplayInput, replayObservation, clientObservation)
          : rt::Promise(sentinel::replay_store::persistSentinelReplayFromEnvironment(*input.sentinelReplayInput, replayObservation, clientObservation));
      }
      const http::SentinelProviderDegradationInput degradationInput{
        .status = static_cast<double>(data.response.status()), .completed = telemetry && telemetry->completed,
        .removedProviderTriggerClass = telemetry ? telemetry->removedProviderTriggerClass : std::nullopt,
      };
      auto degradationWrite = http::shouldSignalSentinelProviderDegradation(degradationInput) && input.recordSentinelDegradation
        ? invokeWriter(std::move(input.recordSentinelDegradation), rt::dateNow()) : rt::Promise<bool>::resolve(false);
      admin::error_log::AdminErrorInput errorInput{
        .request_id = input.requestId, .route = input.route, .status = static_cast<double>(data.response.status()), .provider = provider,
        .model = telemetry ? telemetry->model : std::nullopt, .reasoning = telemetry ? telemetry->reasoning : std::nullopt, .stream = stream,
        .terminal_type = clientObservation.terminal_type, .failure_kind = clientObservation.failure_kind, .delivery_outcome = data.deliveryOutcome,
        .created_at_ms = rt::dateNow(), .latency_ms = latencyMs, .git_sha = config::runtimeGitSha(), .deno_revision = config::runtimeDeploymentId(),
      };
      const auto adminErrorWrite = input.recordAdminError ? invokeWriter(std::move(input.recordAdminError), errorInput) : rt::Promise(admin::error_log::recordAdminError(errorInput));
      const auto noReplay = rt::Promise<void>::resolve();
      const auto writes = replayWrite ? settleWrites(telemetryWrite, cacheAnalyticsWrite, *replayWrite, degradationWrite, adminErrorWrite, usageRollupWrite)
                                      : settleWrites(telemetryWrite, cacheAnalyticsWrite, noReplay, degradationWrite, adminErrorWrite, usageRollupWrite);
      co_await writes.wait();
    }

    rt::Task<void> persistSentinelReplayBestEffort(WithTerminalRequestLogInput input, model::AcceptedSentinelReplayInput snapshot,
                                                model::SentinelFailureObservation observation, model::SentinelClientFailureObservation clientObservation) {
      try {
        const auto task = input.persistSentinelReplay ? input.persistSentinelReplay(snapshot, observation, clientObservation)
          : rt::Promise(sentinel::replay_store::persistSentinelReplayFromEnvironment(snapshot, observation, clientObservation));
        co_await task.wait();
      } catch (...) {}
    }

    struct TerminalState : std::enable_shared_from_this<TerminalState> {
      rt::Response response;
      WithTerminalRequestLogInput input;
      BodyObservation clientBodyObservation;
      std::optional<rt::Promise<BodyObservation>> bufferedObservation;
      std::optional<rt::Promise<void>> replayFinalization;
      std::optional<rt::Promise<void>> terminalLog;
      std::optional<rt::Promise<void>> terminalFinalization;
      struct Intent { std::string outcome; std::optional<std::string> reason; };
      std::optional<Intent> terminalIntent;
      bool terminalSettled = false;
      std::optional<rt::Promise<std::string>> deliveryOutcome;
      std::optional<rt::ReadableStreamDefaultReader<rt::ByteChunk>> reader;
      std::optional<replay::SentinelSseInspector> sseInspector;
      bool sseInspectionFailed = false;
      std::optional<double> downstreamDrainedAtMonotonicMs;
      std::optional<rt::Resolvers<std::string>> bodyOutcome;
      bool bodyDidSettle = false;
      bool downstreamCancelled = false;

      TerminalState(rt::Response response_, WithTerminalRequestLogInput input_) : response(std::move(response_)), input(std::move(input_)) {}

      const rt::Response& telemetryResponse() const { return input.telemetryResponse ? *input.telemetryResponse : response; }

      model::SentinelFailureObservation replayObservationFor(bool streamReadFailure) {
        const auto telemetry = openai_telemetry::getResponseTelemetry(telemetryResponse());
        return {
          .status = static_cast<double>(response.status()),
          .stream = streamReadFailure ? std::optional<bool>(telemetry ? telemetry->stream.value_or(true) : true) : telemetry ? telemetry->stream : std::nullopt,
          .completed = !streamReadFailure && telemetry && telemetry->completed,
          .terminal_type = streamReadFailure ? std::optional<std::string>(telemetry ? telemetry->streamTerminalType.value_or("error") : "error")
                                            : telemetry ? telemetry->streamTerminalType : std::nullopt,
          .failure_kind = streamReadFailure ? std::optional<std::string>(telemetry ? telemetry->failureKind.value_or("gateway_stream_read_error") : "gateway_stream_read_error")
                                           : telemetry ? telemetry->failureKind : std::nullopt,
          .synthetic_terminal_type = telemetry ? telemetry->syntheticTerminalType : std::nullopt,
          .provider_route = telemetry ? telemetry->provider : response.headers().get("x-uos-upstream").value_or("gateway"),
        };
      }

      rt::Task<BodyObservation> resolveBody() {
        if (clientBodyObservation) co_return clientBodyObservation;
        if (bufferedObservation) { auto value = co_await bufferedObservation->wait(); co_return value; }
        co_await rt::microtask();
        co_return std::nullopt;
      }

      rt::Task<void> persistReplay(model::SentinelFailureObservation observation, std::optional<model::AcceptedSentinelReplayInput> backgroundReplayInput) {
        const auto keepAlive = shared_from_this();
        if (!backgroundReplayInput) {
          if (input.sentinelReplayOmission) {
            const auto body = co_await resolveBody();
            const auto client = replay::resolveSentinelClientFailureObservation(observation, body);
            if (replay::shouldPersistSentinelReplay(observation, client)) {
              const auto write = input.recordSentinelReplayOmission
                ? input.recordSentinelReplayOmission(input.requestId, *input.sentinelReplayOmission, rt::dateNow())
                : rt::Promise(sentinel::replay_store::recordSentinelReplayOmissionFromEnvironment(input.requestId, *input.sentinelReplayOmission, rt::dateNow()));
              co_await write.wait();
            }
          }
          co_return;
        }
        const auto body = co_await resolveBody();
        const auto client = replay::resolveSentinelClientFailureObservation(observation, body);
        if (!replay::shouldPersistSentinelReplay(observation, client)) { co_await rt::microtask(); co_return; }
        auto task = persistSentinelReplayBestEffort(input, *backgroundReplayInput, observation, client);
        const rt::Promise<void> writing(std::move(task));
        co_await writing.wait();
      }

      rt::Promise<void> persistReplayAtApplicationTerminal(bool streamReadFailure = false) {
        if (replayFinalization) return *replayFinalization;
        if (!input.sentinelReplayInput && !input.sentinelReplayOmission) {
          replayFinalization = rt::Promise<void>::resolve();
          return *replayFinalization;
        }
        const auto originalReplayInput = input.sentinelReplayInput;
        const auto backgroundReplayInput = originalReplayInput ? std::optional(replay::snapshotSentinelReplayInput(*originalReplayInput)) : std::nullopt;
        // Release request-owned bytes before inspection or persistence can suspend.
        replay::zeroSentinelReplayInput(originalReplayInput);
        const auto observation = replayObservationFor(streamReadFailure);
        replayFinalization = rt::Promise<void>(persistReplay(observation, backgroundReplayInput))
          .catch_(rt::ignoreRejection).finally_([backgroundReplayInput, originalReplayInput] {
            replay::zeroSentinelReplayInput(backgroundReplayInput);
            replay::zeroSentinelReplayInput(originalReplayInput);
          });
        return *replayFinalization;
      }

      rt::Promise<void> log(std::optional<double> drained = std::nullopt, http::DeliveryOutcome outcome = "unobserved",
                            bool streamReadFailure = false, bool suppressSentinelReplay = false) {
        if (terminalLog) return *terminalLog;
        try { if (input.onSettled) input.onSettled(); } catch (...) {}
        auto logInput = input;
        if (replayFinalization) logInput.sentinelReplayInput.reset();
        LogInput data{.input = std::move(logInput), .response = response, .downstreamDrainedAtMonotonicMs = drained,
          .deliveryOutcome = std::move(outcome), .streamReadFailure = streamReadFailure,
          .suppressSentinelReplay = suppressSentinelReplay || replayFinalization.has_value(),
          .resolveClientBodyObservation = [self = shared_from_this()] { return self->resolveBody(); }};
        terminalLog = rt::Promise<void>(logTerminalRequest(std::move(data))).catch_(rt::ignoreRejection);
        return *terminalLog;
      }

      rt::Task<void> runTerminal(Intent intended) {
        rt::Reason lastError;
        for (int attempt = 0; attempt < 3; ++attempt) {
          try {
            const auto settling = input.onTerminal(intended.outcome, intended.reason);
            co_await settling.wait();
            terminalSettled = true;
            co_return;
          } catch (...) { lastError = std::current_exception(); }
        }
        warnQuotaAccountingFailure({input.route, input.requestId}, lastError);
      }

      rt::Promise<void> finalizeTerminal(std::string outcome, std::optional<std::string> reason = std::nullopt) {
        if (!input.onTerminal) return rt::Promise<void>::resolve();
        if (outcome == "completed" && !terminalSettled) terminalIntent = Intent{outcome, reason};
        else if (!terminalIntent) terminalIntent = Intent{outcome, reason};
        if (terminalSettled) return rt::Promise<void>::resolve();
        const auto self = shared_from_this();
        if (terminalFinalization) {
          return terminalFinalization->then([self, outcome, reason] {
            return self->terminalSettled ? rt::Promise<void>::resolve() : self->finalizeTerminal(outcome, reason);
          });
        }
        const rt::Promise<void> running(runTerminal(*terminalIntent));
        auto current = std::make_shared<std::optional<rt::Promise<void>>>();
        const auto finalized = running.finally_([self, current] {
          if (self->terminalFinalization && current->has_value() && *self->terminalFinalization == **current) self->terminalFinalization.reset();
          current->reset();
        });
        *current = finalized;
        terminalFinalization = finalized;
        return finalized;
      }

      rt::Promise<void> finalizeObservedCompletion() {
        if (!response.ok()) return rt::Promise<void>::resolve();
        const auto telemetry = openai_telemetry::getResponseTelemetry(telemetryResponse());
        return telemetry && telemetry->completed ? finalizeTerminal("completed") : rt::Promise<void>::resolve();
      }

      rt::Promise<void> finalizeFromTelemetry(std::string reason) {
        if (!input.onTerminal) return rt::Promise<void>::resolve();
        const auto telemetry = openai_telemetry::getResponseTelemetry(telemetryResponse());
        return response.ok() && telemetry && telemetry->completed ? finalizeTerminal("completed") : finalizeTerminal("incomplete", std::move(reason));
      }

      static rt::Task<rt::Response> bufferedResponse(std::shared_ptr<TerminalState> state) {
        rt::Reason failure;
        try {
          const auto terminal = state->finalizeFromTelemetry(state->response.ok() ? "response_incomplete" : "response_error");
          co_await terminal.wait();
          const auto replayTask = state->persistReplayAtApplicationTerminal();
          if (!http::scheduleSentinelBackgroundTask(replayTask, state->input.waitUntil)) co_await replayTask.wait();
        } catch (...) { failure = std::current_exception(); }
        if (state->deliveryOutcome) {
          auto task = state->deliveryOutcome->then([state](const std::string& outcome) { return state->log(std::nullopt, outcome, false, true); });
          runtime::http_tasks::trackRequestTask(state->input.deliveryCompleted, task);
        } else {
          const auto task = state->log(std::nullopt, "unobserved", false, true);
          co_await task.wait();
        }
        if (failure) std::rethrow_exception(failure);
        co_return state->response;
      }

      void inspectSseChunk(const rt::ByteChunk& value) {
        if (sseInspectionFailed) return;
        try { sseInspector->push(value); } catch (...) { sseInspectionFailed = true; }
      }
      BodyObservation finishSseInspection(std::string termination = "eof") {
        if (sseInspectionFailed) return std::nullopt;
        try { return sseInspector->finish(termination); } catch (...) { return std::nullopt; }
      }
      void settleBody(std::string outcome) {
        if (!bodyOutcome || bodyDidSettle) return;
        bodyDidSettle = true;
        bodyOutcome->resolve(std::move(outcome));
      }
      void releaseReplayOwnership() {
        replay::zeroSentinelReplayInput(input.sentinelReplayInput);
        replay::disposeSentinelUpstreamRecorder(input.sentinelReplayInput);
      }
      rt::Task<void> settleInterruptedBody() {
        if (!deliveryOutcome) { const auto task = log(std::nullopt, "interrupted", false, true); co_await task.wait(); }
        settleBody("interrupted");
      }
      rt::Task<void> handleProviderReadFailure(rt::Reason error, Controller controller) {
        clientBodyObservation = finishSseInspection("read_error");
        const bool downstreamAborted = downstreamCancelled || (input.deliverySignal && input.deliverySignal->aborted());
        const auto terminal = finalizeFromTelemetry(downstreamAborted ? "downstream_cancelled" : "stream_read_error");
        co_await terminal.wait();
        if (downstreamAborted) releaseReplayOwnership();
        else { const auto task = persistReplayAtApplicationTerminal(true); co_await task.wait(); }
        if (!deliveryOutcome) { const auto task = log(std::nullopt, "interrupted", !downstreamAborted, true); co_await task.wait(); }
        try { controller.error(error); } catch (...) {}
        settleBody(downstreamAborted ? "interrupted" : "failed");
      }
      rt::Task<void> finishProviderStream(Controller controller) {
        clientBodyObservation = finishSseInspection();
        const bool downstreamAborted = downstreamCancelled || (input.deliverySignal && input.deliverySignal->aborted());
        if (downstreamAborted) {
          const auto terminal = finalizeFromTelemetry("downstream_cancelled");
          co_await terminal.wait();
          releaseReplayOwnership();
          try { controller.close(); } catch (...) {}
          const rt::Promise<void> interrupted(settleInterruptedBody());
          co_await interrupted.wait();
          co_return;
        }
        // Drain timing precedes accounting; delivery is the transport's separate receipt.
        downstreamDrainedAtMonotonicMs = rt::performanceNow();
        const auto terminal = finalizeFromTelemetry("stream_eof_without_completion");
        co_await terminal.wait();
        const auto replayTask = persistReplayAtApplicationTerminal();
        co_await replayTask.wait();
        bool closeFailed = false;
        try { controller.close(); } catch (...) { closeFailed = true; }
        if (closeFailed) { const rt::Promise<void> interrupted(settleInterruptedBody()); co_await interrupted.wait(); co_return; }
        if (!deliveryOutcome) { const auto task = log(downstreamDrainedAtMonotonicMs, "unobserved", false, true); co_await task.wait(); }
        settleBody("drained");
      }
      rt::Task<void> pull(Controller controller) {
        rt::IteratorResult<rt::ByteChunk> result;
        rt::Reason failure;
        try { result = co_await reader->read(); } catch (...) { failure = std::current_exception(); }
        if (failure) { const rt::Promise<void> task(handleProviderReadFailure(failure, controller)); co_await task.wait(); co_return; }
        if (result.done) { const rt::Promise<void> task(finishProviderStream(controller)); co_await task.wait(); co_return; }
        (void)finalizeObservedCompletion();
        inspectSseChunk(*result.value);
        bool enqueueFailed = false;
        try { controller.enqueue(*result.value); } catch (...) { enqueueFailed = true; }
        if (enqueueFailed) {
          downstreamCancelled = true;
          (void)reader->cancelPromise().catch_(rt::ignoreRejection);
          (void)finalizeFromTelemetry("downstream_enqueue_failed");
          releaseReplayOwnership();
          const rt::Promise<void> task(settleInterruptedBody());
          co_await task.wait();
        }
      }
      void cancel(rt::Reason reason) {
        // Do not await a concurrent provider pull; it performs layered cleanup.
        downstreamCancelled = true;
        (void)reader->cancelPromise(reason).catch_(rt::ignoreRejection);
        (void)finalizeFromTelemetry("downstream_cancelled");
        releaseReplayOwnership();
        if (!deliveryOutcome) (void)log(std::nullopt, "interrupted", false, true);
        settleBody("interrupted");
      }
    };
  }

  void warnQuotaAccountingFailure(const QuotaAccountingFailureInput& input, rt::Reason error) {
    std::vector<rt::Reason> errors{error};
    try { if (error) std::rethrow_exception(error); }
    catch (const rt::AggregateError& aggregate) { errors = aggregate.errors; }
    catch (...) {}
    try {
      json::array classes;
      for (const auto& item : errors) {
        std::string className;
        if (rt::isError(item)) className = rt::errorName(item).value_or("Error");
        else { const auto value = rt::thrownValue(item); className = value ? rt::typeOf(*value) : "undefined"; }
        classes.push_back(json::object{{"class", className}});
      }
      const json::object warning{{"request_id", input.requestId}, {"route", input.route}, {"errors", classes}};
      rt::console::warn("[ai.ubq.fi] quota_accounting_failed", rt::jsonStringify(warning));
    } catch (...) {}
  }

  rt::Promise<rt::Response> withTerminalRequestLog(rt::Response originalResponse, WithTerminalRequestLogInput input) {
    auto state = std::make_shared<TerminalState>(std::move(originalResponse), std::move(input));
    const auto& response = state->response;
    const std::string contentType = rt::toLowerCase(response.headers().get("Content-Type").value_or(""));
    const bool isSse = rt::includes(contentType, "text/event-stream");
    const auto initialTelemetry = openai_telemetry::getResponseTelemetry(state->telemetryResponse());
    // Pre-admission auth refusals without diagnostic consumers need no cloned body.
    const auto& options = state->input;
    const bool skipAuthBody = options.admissionStage == "pre_admission" && !options.onSettled && !initialTelemetry &&
      !response.headers().has("x-uos-upstream") && !options.sentinelReplayInput && !options.sentinelReplayOmission &&
      skipsAdminError(options, response.status(), "gateway");
    if (!isSse && response.body() && ((response.status() >= 400 && !skipAuthBody) || response.status() == 202 || rt::startsWith(state->input.route, "embeddings.jobs.") ||
        (initialTelemetry && !initialTelemetry->completed && (initialTelemetry->streamTerminalType || initialTelemetry->failureKind || initialTelemetry->syntheticTerminalType)))) {
      state->bufferedObservation = rt::Promise(replay::inspectSentinelBufferedResponseBody(response));
    }
    if (state->input.deliveryCompleted) {
      const auto signal = state->input.deliverySignal;
      state->deliveryOutcome = state->input.deliveryCompleted->then(
        [signal] { return signal && signal->aborted() ? std::string("interrupted") : std::string("delivered"); },
        [](rt::Reason) { return std::string("interrupted"); });
    }
    if (!response.body() || !isSse) {
      return rt::Promise<rt::Response>(TerminalState::bufferedResponse(std::move(state)));
    }
    state->reader = response.body()->getReader();
    state->sseInspector = replay::createSentinelSseInspector();
    if (state->deliveryOutcome) {
      state->bodyOutcome = rt::withResolvers<std::string>();
      const auto outcomes = rt::all(state->bodyOutcome->promise, *state->deliveryOutcome);
      const auto logging = outcomes.then([state](const auto& values) {
        const auto& body = std::get<0>(values);
        const auto& delivery = std::get<1>(values);
        return state->log(state->downstreamDrainedAtMonotonicMs, body == "drained" ? delivery : "interrupted", body == "failed", body == "interrupted");
      });
      runtime::http_tasks::trackRequestTask(state->input.deliveryCompleted, logging);
    }
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.pull = [state](Controller controller) { return state->pull(controller); };
    source.cancel = [state](rt::Reason reason) { state->cancel(reason); };
    const rt::ByteStream body(source);
    const rt::ResponseInit init{.status = static_cast<double>(response.status()), .statusText = response.statusText(), .headers = response.headers()};
    return rt::Promise<rt::Response>::resolve(rt::Response(rt::BodyInit(body), init));
  }

  rt::Promise<rt::Response> withRejectionTerminalLog(rt::Response response, std::optional<std::string> route, RejectionTerminalLogInput input) {
    if (!route || route->empty()) {
      if (input.onSettled) input.onSettled();
      return rt::Promise<rt::Response>::resolve(std::move(response));
    }
    WithTerminalRequestLogInput options{.route = std::move(*route), .startedAtMonotonicMs = input.startedAtMonotonicMs, .requestId = std::move(input.requestId)};
    options.admissionStage = std::move(input.admissionStage);
    if (input.delivery) { options.deliveryCompleted = std::move(input.delivery->completed); options.deliverySignal = std::move(input.delivery->downstreamSignal); }
    options.onSettled = std::move(input.onSettled);
    options.admissionWaitMs = input.admissionWaitMs;
    return withTerminalRequestLog(std::move(response), std::move(options));
  }
}
