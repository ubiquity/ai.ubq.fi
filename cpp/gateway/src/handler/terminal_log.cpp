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

#include <memory>
#include <utility>
#include <vector>

namespace uos::handler::terminal_log {
  namespace {
    namespace replay = sentinel::replay_observation;
    namespace model = sentinel::replay_model;
    using BodyObservation = std::optional<model::SentinelClientBodyObservation>;
    using Controller = rt::ReadableStreamDefaultController<rt::ByteChunk>;

    // rt::allSettled has a homogeneous result type; this batch discards heterogeneous values.
    template <class... T> rt::Promise<void> settleWrites(const rt::Promise<T>&... writes) {
      auto result = rt::Promise<void>();
      auto output = result.state();
      auto remaining = std::make_shared<std::size_t>(sizeof...(T));
      const auto add = [output, remaining](const auto& write) {
        auto source = write.state();
        source->react([source, output, remaining] { if (--*remaining == 0) output->fulfill(); });
      };
      (add(writes), ...);
      return result;
    }

    struct LogInput {
      WithTerminalRequestLogInput input;
      rt::Response response;
      std::optional<double> downstreamDrainedAtMonotonicMs;
      http::DeliveryOutcome deliveryOutcome;
      bool streamReadFailure;
      bool suppressSentinelReplay;
      std::function<rt::Promise<BodyObservation>()> resolveClientBodyObservation;
    };

    rt::Task<void> logTerminalRequest(LogInput data) {
      auto& input = data.input;
      const auto telemetry = openai_telemetry::getResponseTelemetry(input.telemetryResponse.value_or(data.response));
      const auto accountCohortId = openai_telemetry::getResponseAccountCohortId(input.telemetryResponse.value_or(data.response));
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
      json::object terminal;
      terminal["request_id"] = input.requestId;
      if (nativeHttpFailure) terminal["native_http_failure"] = json::value_from(*nativeHttpFailure);
      terminal["route"] = input.route;
      terminal["status"] = data.response.status();
      terminal["provider"] = provider;
      terminal["latency_ms"] = rt::makeNumber(latencyMs);
      rt::objectSet(terminal, "admission_wait_ms", rt::orNull(input.admissionWaitMs));
      rt::objectSet(terminal, "admission_stage", rt::orNull(input.admissionStage));
#define TERMINAL_OPTIONAL(key, member) rt::objectSet(terminal, key, rt::orNull(telemetry ? telemetry->member : std::nullopt))
      TERMINAL_OPTIONAL("first_provider_dispatch_ms", firstProviderDispatchMs);
      TERMINAL_OPTIONAL("first_provider_headers_ms", firstProviderHeadersMs);
      TERMINAL_OPTIONAL("first_codex_dispatch_ms", firstCodexDispatchMs);
      TERMINAL_OPTIONAL("first_codex_headers_ms", firstCodexHeadersMs);
      TERMINAL_OPTIONAL("first_upstream_sse_event_ms", firstUpstreamSseEventMs);
      TERMINAL_OPTIONAL("first_semantic_commitment_ms", firstSemanticCommitmentMs);
      TERMINAL_OPTIONAL("stream_terminal_ms", streamTerminalMs);
      rt::objectSet(terminal, "downstream_drain_ms", rt::orNull(downstreamDrainMs));
      terminal["delivery_outcome"] = data.deliveryOutcome;
      TERMINAL_OPTIONAL("model", model);
      TERMINAL_OPTIONAL("reasoning", reasoning);
      TERMINAL_OPTIONAL("output_token_allowance", outputTokenAllowance);
      TERMINAL_OPTIONAL("rate_limit_wait_ms", rateLimitWaitMs);
      TERMINAL_OPTIONAL("rate_limit_failover_model", rateLimitFailoverModel);
      TERMINAL_OPTIONAL("provider_request_id", providerRequestId);
      TERMINAL_OPTIONAL("input_tokens", inputTokens);
      TERMINAL_OPTIONAL("cached_input_tokens", cachedInputTokens);
      TERMINAL_OPTIONAL("cache_write_input_tokens", cacheWriteInputTokens);
      TERMINAL_OPTIONAL("output_tokens", outputTokens);
      TERMINAL_OPTIONAL("total_tokens", totalTokens);
      terminal["usage_observed"] = telemetry ? telemetry->usageObserved : false;
      terminal["usage_telemetry_status"] = telemetry ? telemetry->usageTelemetryStatus : "missing";
      terminal["prompt_cache_key_present"] = telemetry ? telemetry->promptCacheKeyPresent : false;
      terminal["prompt_cache_mode"] = telemetry ? telemetry->promptCacheMode : "unspecified";
      terminal["explicit_breakpoint_count"] = rt::makeNumber(telemetry ? telemetry->explicitBreakpointCount : 0);
      TERMINAL_OPTIONAL("account_slot", accountSlot);
      rt::objectSet(terminal, "account_cohort_id", rt::orNull(accountCohortId));
      TERMINAL_OPTIONAL("active_generation", activeGeneration);
      TERMINAL_OPTIONAL("active_transition_reason", activeTransitionReason);
      TERMINAL_OPTIONAL("fallback_reason", fallbackReason);
      TERMINAL_OPTIONAL("semantic_output_observed", semanticOutputObserved);
      terminal["upstream_event_kinds"] = json::value_from(telemetry ? telemetry->upstreamEventKinds : std::vector<std::string>());
      rt::objectSet(terminal, "stream", rt::orNull(stream));
      rt::objectSet(terminal, "stream_terminal_type", rt::orNull(terminalType));
      rt::objectSet(terminal, "failure_kind", rt::orNull(failureKind));
      terminal["response_created_observed"] = telemetry ? telemetry->responseCreatedObserved : false;
      TERMINAL_OPTIONAL("synthetic_terminal_type", syntheticTerminalType);
      terminal["attempted_providers"] = json::value_from(telemetry ? telemetry->attemptedProviders : std::vector<std::string>());
      TERMINAL_OPTIONAL("removed_provider_trigger_class", removedProviderTriggerClass);
      TERMINAL_OPTIONAL("removed_provider_circuit_transition", removedProviderCircuitTransition);
      TERMINAL_OPTIONAL("removed_provider_selected_model", removedProviderSelectedModel);
      TERMINAL_OPTIONAL("removed_provider_task_type", removedProviderTaskType);
      TERMINAL_OPTIONAL("removed_provider_latency_ms", removedProviderLatencyMs);
      TERMINAL_OPTIONAL("removed_provider_terminal_status", removedProviderTerminalStatus);
      TERMINAL_OPTIONAL("removed_provider_semantic_commitment", removedProviderSemanticCommitment);
#undef TERMINAL_OPTIONAL
      terminal["git_sha"] = config::runtimeGitSha();
      terminal["deno_revision"] = config::runtimeDeploymentId();
      rt::objectSet(terminal, "router_revision", rt::orNull(data.response.headers().get("x-uos-router-revision")));
      rt::console::info("[ai.ubq.fi] request_terminal", rt::jsonStringify(terminal));
      const bool completed = !data.streamReadFailure && telemetry && telemetry->completed;
      cache::telemetry_gate::PromptCacheTelemetryEvent telemetryEvent{
        .provider = provider, .model = telemetry ? telemetry->model : std::nullopt, .route = input.route, .status = static_cast<double>(data.response.status()),
        .completed = completed, .usageTelemetryStatus = telemetry ? telemetry->usageTelemetryStatus : "missing",
        .cacheWriteTokensPresent = telemetry && telemetry->cacheWriteInputTokens.has_value(),
      };
      auto telemetryWrite = input.recordTelemetry ? input.recordTelemetry(telemetryEvent)
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
      auto cacheAnalyticsWrite = input.recordCacheAnalytics ? input.recordCacheAnalytics(analyticsEvent)
        : rt::Promise(cache::prompt_analytics_queue::enqueuePromptCacheAnalytics(analyticsEvent));
      authority::native_paid_contracts::TerminalUsageRollupInput rollupInput{
        .model = telemetry ? telemetry->model : std::nullopt, .provider = provider, .request_id = input.requestId,
        .request_created_at_ms = input.requestStartedAtMs ? *input.requestStartedAtMs : rt::dateNow(), .input_tokens = telemetry ? telemetry->inputTokens : std::nullopt,
        .cached_input_tokens = telemetry ? telemetry->cachedInputTokens : std::nullopt, .output_tokens = telemetry ? telemetry->outputTokens : std::nullopt,
      };
      const auto usageRollupWrite = (input.recordUsageRollup ? input.recordUsageRollup(rollupInput)
        : rt::Promise(paid_fallback::rollups::recordTerminalUsageRollup(rollupInput))).catch_([](rt::Reason) { return false; });
      const model::SentinelFailureObservation replayObservation{
        .status = static_cast<double>(data.response.status()), .stream = stream, .completed = completed, .terminal_type = terminalType,
        .failure_kind = failureKind, .synthetic_terminal_type = telemetry ? telemetry->syntheticTerminalType : std::nullopt, .provider_route = provider,
      };
      const auto zero = rt::scopeExit([&input] { replay::zeroSentinelReplayInput(input.sentinelReplayInput); });
      const auto clientBodyObservation = co_await data.resolveClientBodyObservation().wait();
      const auto clientObservation = replay::resolveSentinelClientFailureObservation(replayObservation, clientBodyObservation);
      std::optional<rt::Promise<replay::SentinelReplayPersistResult>> replayWrite;
      if (input.sentinelReplayInput && !data.suppressSentinelReplay && replay::shouldPersistSentinelReplay(replayObservation, clientObservation)) {
        replayWrite = input.persistSentinelReplay ? input.persistSentinelReplay(*input.sentinelReplayInput, replayObservation, clientObservation)
          : rt::Promise(sentinel::replay_store::persistSentinelReplayFromEnvironment(*input.sentinelReplayInput, replayObservation, clientObservation));
      }
      const http::SentinelProviderDegradationInput degradationInput{
        .status = static_cast<double>(data.response.status()), .completed = telemetry && telemetry->completed,
        .removedProviderTriggerClass = telemetry ? telemetry->removedProviderTriggerClass : std::nullopt,
      };
      auto degradationWrite = http::shouldSignalSentinelProviderDegradation(degradationInput) && input.recordSentinelDegradation
        ? input.recordSentinelDegradation(rt::dateNow()) : rt::Promise<bool>::resolve(false);
      admin::error_log::AdminErrorInput errorInput{
        .request_id = input.requestId, .route = input.route, .status = static_cast<double>(data.response.status()), .provider = provider,
        .model = telemetry ? telemetry->model : std::nullopt, .reasoning = telemetry ? telemetry->reasoning : std::nullopt, .stream = stream,
        .terminal_type = clientObservation.terminal_type, .failure_kind = clientObservation.failure_kind, .delivery_outcome = data.deliveryOutcome,
        .created_at_ms = rt::dateNow(), .latency_ms = latencyMs, .git_sha = config::runtimeGitSha(), .deno_revision = config::runtimeDeploymentId(),
      };
      const auto adminErrorWrite = input.recordAdminError ? input.recordAdminError(errorInput) : rt::Promise(admin::error_log::recordAdminError(errorInput));
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

      model::SentinelFailureObservation replayObservationFor(bool streamReadFailure) {
        const auto telemetry = openai_telemetry::getResponseTelemetry(input.telemetryResponse.value_or(response));
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
        auto self = shared_from_this();
        LogInput data{.input = std::move(logInput), .response = response, .downstreamDrainedAtMonotonicMs = drained,
          .deliveryOutcome = std::move(outcome), .streamReadFailure = streamReadFailure,
          .suppressSentinelReplay = suppressSentinelReplay || replayFinalization.has_value(),
          .resolveClientBodyObservation = [self] { return rt::Promise<BodyObservation>(self->resolveBody()); }};
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
        const auto telemetry = openai_telemetry::getResponseTelemetry(input.telemetryResponse.value_or(response));
        return telemetry && telemetry->completed ? finalizeTerminal("completed") : rt::Promise<void>::resolve();
      }

      rt::Promise<void> finalizeFromTelemetry(std::string reason) {
        const auto telemetry = openai_telemetry::getResponseTelemetry(input.telemetryResponse.value_or(response));
        return response.ok() && telemetry && telemetry->completed ? finalizeTerminal("completed") : finalizeTerminal("incomplete", std::move(reason));
      }

      rt::Task<rt::Response> bufferedResponse() {
        rt::Reason failure;
        try {
          const auto terminal = finalizeFromTelemetry(response.ok() ? "response_incomplete" : "response_error");
          co_await terminal.wait();
          const auto replayTask = persistReplayAtApplicationTerminal();
          if (!http::scheduleSentinelBackgroundTask(replayTask, input.waitUntil)) co_await replayTask.wait();
        } catch (...) { failure = std::current_exception(); }
        if (deliveryOutcome) {
          auto self = shared_from_this();
          auto task = deliveryOutcome->then([self](const std::string& outcome) { return self->log(std::nullopt, outcome, false, true); });
          runtime::http_tasks::trackRequestTask(input.deliveryCompleted, task);
        } else {
          const auto task = log(std::nullopt, "unobserved", false, true);
          co_await task.wait();
        }
        if (failure) std::rethrow_exception(failure);
        co_return response;
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

  rt::Promise<rt::Response> withTerminalRequestLog(rt::Response response, WithTerminalRequestLogInput input) {
    const std::string contentType = rt::toLowerCase(response.headers().get("Content-Type").value_or(""));
    const bool isSse = rt::includes(contentType, "text/event-stream");
    const auto initialTelemetry = openai_telemetry::getResponseTelemetry(input.telemetryResponse.value_or(response));
    auto state = std::make_shared<TerminalState>(response, std::move(input));
    if (!isSse && response.body() && (response.status() >= 400 || response.status() == 202 || rt::startsWith(state->input.route, "embeddings.jobs.") ||
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
      auto owner = [state]() -> rt::Task<rt::Response> { auto value = co_await state->bufferedResponse(); co_return value; };
      return rt::Promise<rt::Response>(rt::async(owner));
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
      return rt::Promise<rt::Response>::resolve(response);
    }
    WithTerminalRequestLogInput options{.route = *route, .startedAtMonotonicMs = input.startedAtMonotonicMs, .requestId = input.requestId};
    options.admissionStage = input.admissionStage;
    if (input.delivery) { options.deliveryCompleted = input.delivery->completed; options.deliverySignal = input.delivery->downstreamSignal; }
    options.onSettled = input.onSettled;
    options.admissionWaitMs = input.admissionWaitMs;
    return withTerminalRequestLog(response, std::move(options));
  }
}
