#include "uos/handler/terminal_route.hpp"
#include "uos/handler/admission.hpp"
#include "uos/handler/terminal_log.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/auth/index.hpp"
#include "uos/chat/envelope.hpp"
#include "uos/config.hpp"
#include "uos/decisions/handlers.hpp"
#include "uos/embeddings/handlers.hpp"
#include "uos/embeddings/jobs.hpp"
#include "uos/http.hpp"
#include "uos/images.hpp"
#include "uos/kernel/attestation.hpp"
#include "uos/kernel/quota_reserve.hpp"
#include "uos/models/catalog.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/responses_handler.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/clock.hpp"
#include "uos/sentinel/replay_store.hpp"
#include "uos/systemone/handlers.hpp"

#include <memory>
#include <utility>
#include <variant>

namespace uos::handler::terminal_route {
  namespace {
    using AuthenticatedClientResult = http::AuthenticatedClientResult;
    using ApiKeyPolicy = authority::native_contracts::ApiKeyPolicy;
    using ApiKeyUsageReservation = api_key_policy::ApiKeyUsageReservation;
    using KernelQuotaReservation = kernel::quota_v2::KernelQuotaReservation;
    using KernelRepo = openai_telemetry::UsageContext::KernelRepo;
    using AcceptedSentinelReplayInput = sentinel::replay_model::AcceptedSentinelReplayInput;
    using AdmissionResult = std::variant<ApiKeyUsageReservation, rt::Response>;
    using KernelAdmissionResult = std::variant<KernelQuotaReservation, rt::Response>;

    std::optional<std::string> apiKeyIdFrom(const AuthenticatedClientResult& authResult) {
      const auto* method = std::get_if<auth::index::ClientAuthMethodKvApiKey>(&authResult.method);
      return method ? std::optional(method->key_id) : std::nullopt;
    }

    std::optional<ApiKeyPolicy> apiKeyPolicyFrom(const AuthenticatedClientResult& authResult) {
      const auto* method = std::get_if<auth::index::ClientAuthMethodKvApiKey>(&authResult.method);
      return method ? std::optional(method->policy) : std::nullopt;
    }

    rt::Task<std::optional<KernelRepo>> resolveKernelRepo(rt::Request req, AuthenticatedClientResult authResult) {
      if (const auto* method = std::get_if<auth::index::ClientAuthMethodGithubToken>(&authResult.method))
        co_return KernelRepo{method->owner, method->repo};
      rt::Promise<std::optional<kernel::attestation::GitHubRepo>> pending(kernel::attestation::getKernelAttestationContext(req, authResult.token));
      auto attestation = co_await pending.wait();
      if (!attestation) co_return std::nullopt;
      co_return KernelRepo{attestation->owner, attestation->repo};
    }

    std::optional<rt::AbortSignal> downstreamSignalFor(const rt::Request& req, const std::optional<http::RequestDeliveryInfo>& delivery,
                                                      const std::optional<KernelQuotaReservation>& reservation) {
      if (reservation) return rt::AbortSignal::any({delivery ? delivery->downstreamSignal : req.signal(), reservation->signal});
      return delivery ? std::optional(delivery->downstreamSignal) : std::nullopt;
    }

    rt::Task<AdmissionResult> reserveUsageAdmission(rt::Request req, ApiKeyPolicy policy, std::string requestId, std::string route,
                                                   double startedAtMonotonicMs, std::optional<http::RequestDeliveryInfo> delivery,
                                                   std::function<void()> onSettled, std::optional<double> admissionWaitMs) {
      api_key_policy::ReserveApiKeyUsageV3Options options;
      options.deferWhenFull = true;
      rt::Promise<api_key_policy::ApiKeyUsageReservationDecision> pending(api_key_policy::reserveApiKeyUsageV3(policy, requestId, route, options));
      auto admission = co_await pending.wait();
      if (auto* accepted = std::get_if<api_key_policy::ApiKeyUsageReservationDecisionOk>(&admission)) co_return accepted->reservation;
      const auto response = ::uos::http::withCors(http::withRequestId(std::get<api_key_policy::ApiKeyUsageReservationDecisionNotOk>(admission).response, requestId), req);
      terminal_log::RejectionTerminalLogInput log{.requestId = requestId, .admissionStage = "pre_admission", .startedAtMonotonicMs = startedAtMonotonicMs,
                                                .delivery = delivery, .onSettled = onSettled, .admissionWaitMs = admissionWaitMs};
      auto rejection = terminal_log::withRejectionTerminalLog(response, route, log);
      auto result = co_await rejection.wait();
      co_return result;
    }

    struct ReserveKernelAdmissionInput {
      rt::Request req;
      KernelRepo repo;
      std::string route;
      std::string requestId;
      double startedAtMonotonicMs;
      std::optional<http::RequestDeliveryInfo> delivery;
      std::optional<ApiKeyUsageReservation> usageReservation;
      std::function<void()> onSettled;
      std::optional<double> admissionWaitMs;
    };

    rt::Task<KernelAdmissionResult> reserveKernelAdmission(ReserveKernelAdmissionInput input) {
      rt::Promise<kernel::quota_v2::KernelQuotaReservationDecision> pending(
        kernel::quota_reserve::reserveEffectiveKernelUsageLimit(input.repo.owner, input.repo.repo, input.requestId, input.route));
      auto admission = co_await pending.wait();
      if (auto* accepted = std::get_if<kernel::quota_v2::KernelQuotaReservationDecisionOk>(&admission)) co_return accepted->reservation;
      try {
        if (input.usageReservation) {
          std::optional<std::string> reason = "kernel_quota_rejected";
          rt::Promise<void> released(input.usageReservation->release(reason));
          co_await released.wait();
        } else co_await rt::microtask();
      } catch (...) {
        terminal_log::QuotaAccountingFailureInput warning{input.route, input.requestId};
        terminal_log::warnQuotaAccountingFailure(warning, std::current_exception());
      }
      auto response = ::uos::http::withCors(http::withRequestId(std::get<kernel::quota_v2::KernelQuotaReservationDecisionNotOk>(admission).response, input.requestId), input.req);
      terminal_log::RejectionTerminalLogInput log{.requestId = input.requestId, .admissionStage = "pre_admission", .startedAtMonotonicMs = input.startedAtMonotonicMs,
                                                .delivery = input.delivery, .onSettled = input.onSettled, .admissionWaitMs = input.admissionWaitMs};
      auto rejection = terminal_log::withRejectionTerminalLog(response, input.route, log);
      auto result = co_await rejection.wait();
      co_return result;
    }

    bool truthyReason(const rt::Reason& reason) {
      if (!reason) return false;
      if (rt::isError(reason)) return true;
      const auto value = rt::thrownValue(reason);
      return value && rt::truthy(*value);
    }

    rt::Response quotaErrorResponse(const api_key_policy::ApiKeyQuotaDispatchError& error) {
      rt::Headers headers;
      for (const auto& entry : error.headers.entries()) headers.set(entry.first, entry.second);
      ::uos::http::OpenaiErrorOptions options;
      options.type = error.errorType;
      options.headers = headers;
      return ::uos::http::openaiError(error.status, error.message, error.code, options);
    }

    struct TerminalState : std::enable_shared_from_this<TerminalState> {
      rt::Request req;
      std::string path;
      std::optional<http::RequestDeliveryInfo> delivery;
      std::string requestId;
      double requestStartedAtMs;
      double requestStartedAtMonotonicMs;
      std::optional<std::string> terminalRoute;
      AuthenticatedClientResult authResult;
      std::string idempotencyPrincipal;
      rt::AbortSignal callerSignal;
      std::optional<inference_admission::InferenceAdmissionGranted> processPermit;
      std::optional<double> admissionWaitMs;
      std::optional<std::string> admissionRoute;
      std::optional<ApiKeyPolicy> usagePolicy;
      std::optional<ApiKeyUsageReservation> usageReservation;
      std::shared_ptr<sentinel::replay_model::SentinelReplayCaptureCandidate> sentinelReplayCandidate;
      std::optional<std::string> sentinelReplayOmission;
      std::optional<KernelRepo> kernelRepo;
      std::optional<KernelQuotaReservation> kernelReservation;
      std::optional<std::string> kernelQuotaRoute;
      std::optional<sentinel::upstream_capture::SentinelUpstreamRecorder> sentinelUpstreamRecorder;
      openai_telemetry::UsageContext usageContext;

      TerminalState(rt::Request request, std::string requestPath, std::optional<http::RequestDeliveryInfo> info, std::string id,
                    double started, double monotonic, std::optional<std::string> route, AuthenticatedClientResult auth, std::string principal)
        : req(std::move(request)), path(std::move(requestPath)), delivery(std::move(info)), requestId(std::move(id)), requestStartedAtMs(started),
          requestStartedAtMonotonicMs(monotonic), terminalRoute(std::move(route)), authResult(std::move(auth)), idempotencyPrincipal(std::move(principal)),
          callerSignal(delivery ? delivery->downstreamSignal : req.signal()) {}

      rt::Response withCors(rt::Response response) const { return ::uos::http::withCors(response, req); }
      void releaseProcessPermit() {
        auto permit = processPermit;
        processPermit.reset();
        if (permit) permit->release();
      }
      template<class T> rt::Task<T> releaseOnThrow(std::function<rt::Task<T>()> step) {
        try {
          rt::Promise<T> pending(step());
          auto result = co_await pending.wait();
          co_return result;
        } catch (...) {
          releaseProcessPermit();
          throw;
        }
      }
      std::function<void()> onSettled() {
        return [self = shared_from_this()] { self->releaseProcessPermit(); };
      }
      rt::Task<std::optional<rt::Response>> acquireProcessPermit() {
        if (!admissionRoute) co_return std::nullopt;
        inference_admission::InferenceAdmissionRequest input{.signal = callerSignal, .principal = idempotencyPrincipal};
        auto admittedPromise = admission::admissionController().acquire(input);
        auto admitted = co_await admittedPromise.wait();
        if (const auto* granted = std::get_if<inference_admission::InferenceAdmissionGranted>(&admitted)) {
          processPermit = *granted;
          admissionWaitMs = granted->waitedMs;
          co_return std::nullopt;
        }
        ::uos::http::OpenaiErrorOptions options;
        options.type = "server_error";
        options.param = std::optional<std::string>();
        auto refusal = std::holds_alternative<inference_admission::InferenceAdmissionCallerAborted>(admitted)
          ? ::uos::http::openaiError(499, "Request was cancelled.", "request_cancelled", options)
          : admission::localOverloadResponse(std::get<inference_admission::InferenceAdmissionLocalOverload>(admitted).cause);
        auto response = withCors(http::withRequestId(refusal, requestId));
        terminal_log::RejectionTerminalLogInput log{.requestId = requestId, .admissionStage = "pre_admission",
                                                  .startedAtMonotonicMs = requestStartedAtMonotonicMs, .delivery = delivery};
        auto rejection = terminal_log::withRejectionTerminalLog(response, admissionRoute, log);
        auto result = co_await rejection.wait();
        co_return result;
      }
      rt::Task<void> recordPreCaptureRejection() {
        sentinel::replay_observation::discardSentinelReplayCaptureCandidate(sentinelReplayCandidate);
        if (!sentinelReplayOmission) sentinelReplayOmission = "rejected_before_capture";
        rt::Promise<void> omissionTask(sentinel::replay_store::recordSentinelReplayOmissionFromEnvironment(requestId, *sentinelReplayOmission, rt::dateNow()));
        if (!http::scheduleSentinelBackgroundTask(omissionTask, {})) co_await omissionTask.wait();
      }
      rt::Task<std::optional<rt::Response>> reserveUsageForRequest() {
        if (!usagePolicy || !terminalRoute) co_return std::nullopt;
        auto self = shared_from_this();
        auto policy = *usagePolicy;
        auto route = *terminalRoute;
        std::function<rt::Task<AdmissionResult>()> step = [self, policy, route] {
          return reserveUsageAdmission(self->req, policy, self->requestId, route, self->requestStartedAtMonotonicMs,
                                       self->delivery, self->onSettled(), self->admissionWaitMs);
        };
        rt::Promise<AdmissionResult> pending(releaseOnThrow<AdmissionResult>(step));
        auto admission = co_await pending.wait();
        if (auto* rejection = std::get_if<rt::Response>(&admission)) {
          rt::Promise<void> recorded(recordPreCaptureRejection());
          co_await recorded.wait();
          co_return *rejection;
        }
        usageReservation = std::get<ApiKeyUsageReservation>(admission);
        usagePolicy = usageReservation->policy;
        co_return std::nullopt;
      }
      rt::Task<std::optional<rt::Response>> reserveKernelForRequest() {
        if (!kernelRepo || !kernelQuotaRoute) co_return std::nullopt;
        auto self = shared_from_this();
        ReserveKernelAdmissionInput input{req, *kernelRepo, *kernelQuotaRoute, requestId, requestStartedAtMonotonicMs, delivery,
                                         usageReservation, onSettled(), admissionWaitMs};
        std::function<rt::Task<KernelAdmissionResult>()> step = [input] { return reserveKernelAdmission(input); };
        rt::Promise<KernelAdmissionResult> pending(releaseOnThrow<KernelAdmissionResult>(step));
        auto admission = co_await pending.wait();
        if (auto* rejection = std::get_if<rt::Response>(&admission)) {
          rt::Promise<void> recorded(recordPreCaptureRejection());
          co_await recorded.wait();
          co_return *rejection;
        }
        kernelReservation = std::get<KernelQuotaReservation>(admission);
        co_return std::nullopt;
      }
      std::optional<AcceptedSentinelReplayInput> takeSentinelReplayInput() {
        auto materialized = sentinel::replay_observation::materializeSentinelReplayInput(sentinelReplayCandidate);
        auto omission = sentinelReplayCandidate ? sentinelReplayCandidate->body_omitted_reason : (terminalRoute ? std::optional<std::string>("non_post") : std::nullopt);
        sentinel::replay_observation::discardSentinelReplayCaptureCandidate(sentinelReplayCandidate);
        if (!materialized) {
          if (sentinelUpstreamRecorder) sentinelUpstreamRecorder->dispose();
          if (omission && !sentinelReplayOmission) sentinelReplayOmission = omission;
          return std::nullopt;
        }
        if (sentinelUpstreamRecorder) materialized->upstreamRecorder = sentinelUpstreamRecorder;
        return materialized;
      }
      rt::Task<void> settleKernelQuota(std::string outcome, std::optional<std::string> reason) {
        if (!kernelReservation) co_return;
        if (outcome == "completed") {
          auto committed = kernelReservation->commit();
          co_await committed.wait();
        } else {
          if (!reason) reason = "request_incomplete";
          auto released = kernelReservation->release(reason);
          co_await released.wait();
        }
      }
      rt::Task<void> bestEffortSettleKernelQuota(std::string outcome, std::string reason = "request_incomplete") {
        try {
          rt::Promise<void> pending(settleKernelQuota(outcome, reason));
          co_await pending.wait();
        } catch (...) {
          terminal_log::QuotaAccountingFailureInput warning{terminalRoute.value_or("inference"), requestId};
          terminal_log::warnQuotaAccountingFailure(warning, std::current_exception());
        }
      }
      rt::Task<std::optional<rt::Response>> releaseUsageReservation(rt::Reason runError) {
        rt::Reason failure;
        try {
          if (usageReservation) {
            rt::Promise<void> released(usageReservation->release(std::nullopt));
            co_await released.wait();
          } else co_await rt::microtask();
          co_return std::nullopt;
        } catch (...) { failure = std::current_exception(); }
        rt::Promise<void> settled(bestEffortSettleKernelQuota("incomplete", "api_key_quota_accounting_error"));
        co_await settled.wait();
        if (truthyReason(runError)) {
          terminal_log::QuotaAccountingFailureInput warning{terminalRoute.value_or("inference"), requestId};
          terminal_log::warnQuotaAccountingFailure(warning, runError);
        }
        try { std::rethrow_exception(failure); }
        catch (const api_key_policy::ApiKeyQuotaDispatchError& error) { co_return quotaErrorResponse(error); }
        catch (...) {}
        const api_key_policy::ApiKeyQuotaDispatchError quotaError("API key quota reservation is unavailable");
        co_return quotaErrorResponse(quotaError);
      }
      rt::Task<rt::Response> finishTerminalResponse(rt::Response response, std::string route, bool includeQuota = false, bool trackKernelTerminal = false) {
        auto telemetry = openai_telemetry::getResponseTelemetry(response);
        auto correlated = http::withProviderRequestId(response, telemetry ? telemetry->providerRequestId : std::nullopt);
        auto decorated = includeQuota ? http::decorateInferenceQuota(correlated, usagePolicy ? &*usagePolicy : nullptr, telemetry ? &*telemetry : nullptr) : correlated;
        auto sentinelReplayInput = takeSentinelReplayInput();
        rt::Reason failure;
        try {
          terminal_log::WithTerminalRequestLogInput input{.route = route, .telemetryResponse = response, .startedAtMonotonicMs = requestStartedAtMonotonicMs,
            .requestId = requestId, .request = req, .admissionStage = "accepted"};
          if (trackKernelTerminal) input.onTerminal = [self = shared_from_this()](std::string outcome, std::optional<std::string> reason) {
            return rt::Promise<void>(self->settleKernelQuota(outcome, reason));
          };
          input.onSettled = onSettled();
          if (delivery) { input.deliveryCompleted = delivery->completed; input.deliverySignal = delivery->downstreamSignal; }
          input.requestStartedAtMs = requestStartedAtMs;
          input.sentinelReplayInput = sentinelReplayInput;
          input.admissionWaitMs = admissionWaitMs;
          input.sentinelReplayOmission = sentinelReplayOmission;
          auto pending = terminal_log::withTerminalRequestLog(withCors(http::withRequestId(decorated, requestId)), input);
          auto result = co_await pending.wait();
          co_return result;
        } catch (...) { failure = std::current_exception(); }
        sentinel::replay_observation::zeroSentinelReplayInput(sentinelReplayInput);
        sentinel::replay_observation::disposeSentinelUpstreamRecorder(sentinelReplayInput);
        releaseProcessPermit();
        rt::Promise<void> settled(bestEffortSettleKernelQuota("incomplete", "terminal_wrapper_error"));
        co_await settled.wait();
        std::rethrow_exception(failure);
      }
      rt::Task<rt::Response> executeInference(std::function<rt::Task<rt::Response>()> run) {
        std::optional<rt::Response> response;
        rt::Reason runError;
        try {
          rt::Promise<rt::Response> pending(run());
          response = co_await pending.wait();
        } catch (...) { runError = std::current_exception(); }
        rt::Promise<std::optional<rt::Response>> released(releaseUsageReservation(runError));
        auto quotaAccountingFailure = co_await released.wait();
        if (quotaAccountingFailure) co_return *quotaAccountingFailure;
        std::optional<rt::Response> dispatchError;
        if (runError) {
          try { std::rethrow_exception(runError); }
          catch (const api_key_policy::ApiKeyQuotaDispatchError& error) { dispatchError = quotaErrorResponse(error); }
          catch (...) {}
        }
        if (dispatchError) {
          rt::Promise<void> settled(bestEffortSettleKernelQuota("incomplete", "api_key_quota_dispatch_error"));
          co_await settled.wait();
          co_return *dispatchError;
        }
        if (truthyReason(runError)) {
          rt::Promise<void> settled(bestEffortSettleKernelQuota("incomplete", "inference_exception"));
          co_await settled.wait();
          auto sentinelReplayInput = takeSentinelReplayInput();
          if (sentinelReplayInput) {
            rt::Promise<void> persisted(persistInferenceExceptionReplay(*sentinelReplayInput, runError));
            co_await persisted.wait();
          } else if (sentinelReplayOmission) {
            rt::Promise<void> omissionTask(sentinel::replay_store::recordSentinelReplayOmissionFromEnvironment(requestId, *sentinelReplayOmission, rt::dateNow()));
            if (!http::scheduleSentinelBackgroundTask(omissionTask, {})) co_await omissionTask.wait();
          }
          if (rt::isError(runError)) std::rethrow_exception(runError);
          throw rt::withCause(rt::JsError("Inference handler threw a non-Error value"), runError);
        }
        if (!response) {
          rt::Promise<void> settled(bestEffortSettleKernelQuota("incomplete", "missing_inference_response"));
          co_await settled.wait();
          throw rt::JsError("Inference handler completed without a response");
        }
        co_return *response;
      }
      rt::Task<rt::Response> runModelsRoute() {
        rt::Promise<rt::Response> pending(models::catalog::handleModels(req));
        auto response = co_await pending.wait();
        co_return withCors(response);
      }
      rt::Task<rt::Response> runEmbeddingsRoute() {
        std::function<rt::Task<rt::Response>()> run = [self = shared_from_this()] { return embeddings::handlers::handleUosEmbeddings(self->req, &self->usageContext); };
        rt::Promise<rt::Response> pending(executeInference(run));
        auto response = co_await pending.wait();
        const bool replayed = response.headers().get("x-uos-idempotency-replayed") == "true";
        rt::Promise<void> settled(bestEffortSettleKernelQuota(response.ok() && !replayed ? "completed" : "incomplete", replayed ? "idempotency_replay" : "embedding_failed"));
        co_await settled.wait();
        rt::Promise<rt::Response> finished(finishTerminalResponse(response, "embeddings"));
        auto result = co_await finished.wait();
        co_return result;
      }
      rt::Task<rt::Response> runEmbeddingJobCreateRoute() {
        std::function<rt::Task<rt::Response>()> run = [self = shared_from_this()] { return embeddings::jobs::handleEmbeddingsJobCreate(self->req, self->authResult.token, &self->usageContext); };
        rt::Promise<rt::Response> pending(executeInference(run));
        auto response = co_await pending.wait();
        rt::Promise<void> settled(bestEffortSettleKernelQuota(response.ok() ? "completed" : "incomplete", "embedding_job_create_failed"));
        co_await settled.wait();
        rt::Promise<rt::Response> finished(finishTerminalResponse(response, "embeddings.jobs.create"));
        auto result = co_await finished.wait();
        co_return result;
      }
      rt::Task<rt::Response> runEmbeddingJobGetRoute() {
        auto jobId = rt::trim(rt::slice(path, 20));
        if (jobId.empty()) {
          rt::Promise<void> settled(bestEffortSettleKernelQuota("incomplete", "missing_embedding_job_id"));
          co_await settled.wait();
          auto response = ::uos::http::openaiError(404, "Not found", "not_found");
          rt::Promise<rt::Response> finished(finishTerminalResponse(response, "embeddings.jobs.get"));
          auto result = co_await finished.wait();
          co_return result;
        }
        std::function<rt::Task<rt::Response>()> run = [self = shared_from_this(), jobId] { return embeddings::jobs::handleEmbeddingsJobGet(self->req, self->authResult.token, jobId, &self->usageContext); };
        rt::Promise<rt::Response> pending(executeInference(run));
        auto response = co_await pending.wait();
        rt::Promise<void> settled(bestEffortSettleKernelQuota("incomplete", "embedding_job_read_not_counted"));
        co_await settled.wait();
        rt::Promise<rt::Response> finished(finishTerminalResponse(response, "embeddings.jobs.get"));
        auto result = co_await finished.wait();
        co_return result;
      }
      rt::Task<rt::Response> runImagesRoute() {
        auto kind = path == "/v1/images/edits" ? std::string("edits") : std::string("generations");
        std::function<rt::Task<rt::Response>()> run = [self = shared_from_this(), kind] { return images::handleImages(self->req, kind, &self->usageContext); };
        rt::Promise<rt::Response> pending(executeInference(run));
        auto response = co_await pending.wait();
        rt::Promise<rt::Response> finished(finishTerminalResponse(response, "images." + kind, true, true));
        auto result = co_await finished.wait();
        co_return result;
      }
      rt::Task<rt::Response> runChatCompletionsRoute() {
        std::function<rt::Task<rt::Response>()> run = [self = shared_from_this()] { return chat::envelope::handleChatCompletions(self->req, &self->usageContext); };
        rt::Promise<rt::Response> pending(executeInference(run));
        auto response = co_await pending.wait();
        rt::Promise<rt::Response> finished(finishTerminalResponse(response, "chat.completions", true, true));
        auto result = co_await finished.wait();
        co_return result;
      }
      rt::Task<rt::Response> runResponsesRoute() {
        std::function<rt::Task<rt::Response>()> run = [self = shared_from_this()] { return responses_handler::handleResponses(self->req, &self->usageContext); };
        rt::Promise<rt::Response> pending(executeInference(run));
        auto response = co_await pending.wait();
        rt::Promise<rt::Response> finished(finishTerminalResponse(response, "responses", true, true));
        auto result = co_await finished.wait();
        co_return result;
      }
      rt::Task<rt::Response> runDecisionsRoute() {
        std::function<rt::Task<rt::Response>()> run = [self = shared_from_this()] { return decisions::handlers::handleDecisions(self->req, &self->usageContext); };
        rt::Promise<rt::Response> pending(executeInference(run));
        auto response = co_await pending.wait();
        rt::Promise<void> settled(bestEffortSettleKernelQuota(response.ok() ? "completed" : "incomplete", "decisions_failed"));
        co_await settled.wait();
        rt::Promise<rt::Response> finished(finishTerminalResponse(response, "decisions", true, true));
        auto result = co_await finished.wait();
        co_return result;
      }
      rt::Task<rt::Response> runSystemOneRoute() {
        std::function<rt::Task<rt::Response>()> run = [self = shared_from_this()] { return systemone::handlers::handleSystemOne(self->req, self->usageContext); };
        rt::Promise<rt::Response> pending(executeInference(run));
        auto response = co_await pending.wait();
        rt::Promise<void> settled(bestEffortSettleKernelQuota(response.ok() ? "completed" : "incomplete", "systemone_failed"));
        co_await settled.wait();
        rt::Promise<rt::Response> finished(finishTerminalResponse(response, "systemone", true, true));
        auto result = co_await finished.wait();
        co_return result;
      }
      rt::Task<rt::Response> dispatchTerminalRoute() {
        std::function<rt::Task<rt::Response>()> run;
        auto self = shared_from_this();
        if (req.method() == "GET" && path == "/v1/models") run = [self] { return self->runModelsRoute(); };
        if (!run) for (const auto& route : UOS_TERMINAL_ROUTES) {
          if (req.method() != route.method || !(route.prefix ? path.starts_with(route.path) : path == route.path)) continue;
          if (route.handler == "embeddings") run = [self] { return self->runEmbeddingsRoute(); };
          else if (route.handler == "embeddingJobCreate") run = [self] { return self->runEmbeddingJobCreateRoute(); };
          else run = [self] { return self->runEmbeddingJobGetRoute(); };
          break;
        }
        if (!run && req.method() == "POST") {
          if (path == "/v1/images/generations" || path == "/v1/images/edits") run = [self] { return self->runImagesRoute(); };
          else if (path == "/v1/chat/completions") run = [self] { return self->runChatCompletionsRoute(); };
          else if (path == "/v1/responses") run = [self] { return self->runResponsesRoute(); };
          else if (path == "/v1/decisions") run = [self] { return self->runDecisionsRoute(); };
          else if (path == "/v1/systemone") run = [self] { return self->runSystemOneRoute(); };
        }
        if (run) {
          rt::Promise<rt::Response> pending(run());
          auto result = co_await pending.wait();
          co_return result;
        }
        auto response = ::uos::http::openaiError(404, "Not found", "not_found");
        co_return withCors(req.method() == "HEAD" ? ::uos::http::withoutBody(response) : response);
      }
      rt::Task<std::optional<rt::Response>> refuseAbortedBeforeDispatch() {
        if (!processPermit || !callerSignal.aborted()) co_return std::nullopt;
        std::optional<std::string> abortReason = "caller_aborted_before_dispatch";
        try {
          if (usageReservation) {
            rt::Promise<void> released(usageReservation->release(abortReason));
            co_await released.wait();
          } else co_await rt::microtask();
        } catch (...) {
          terminal_log::QuotaAccountingFailureInput warning{terminalRoute.value_or("inference"), requestId};
          terminal_log::warnQuotaAccountingFailure(warning, std::current_exception());
        }
        rt::Promise<void> settled(bestEffortSettleKernelQuota("incomplete", *abortReason));
        co_await settled.wait();
        ::uos::http::OpenaiErrorOptions options;
        options.type = "server_error";
        options.param = std::optional<std::string>();
        auto refusal = ::uos::http::openaiError(499, "Request was cancelled.", "request_cancelled", options);
        auto response = withCors(http::withRequestId(refusal, requestId));
        terminal_log::RejectionTerminalLogInput log{.requestId = requestId, .admissionStage = "accepted",
          .startedAtMonotonicMs = requestStartedAtMonotonicMs, .delivery = delivery, .onSettled = onSettled(), .admissionWaitMs = admissionWaitMs};
        auto rejection = terminal_log::withRejectionTerminalLog(response, terminalRoute, log);
        auto result = co_await rejection.wait();
        co_return result;
      }
    };
  }

  rt::Task<void> persistInferenceExceptionReplay(AcceptedSentinelReplayInput sentinelReplayInput, rt::Reason runError) {
    PersistInferenceExceptionReplayOptions options;
    return persistInferenceExceptionReplay(std::move(sentinelReplayInput), runError, options);
  }

  rt::Task<void> persistInferenceExceptionReplay(AcceptedSentinelReplayInput sentinelReplayInput, rt::Reason runError,
                                               PersistInferenceExceptionReplayOptions options) {
    sentinel::replay_model::SentinelFailureObservation observation{500, std::nullopt, false, "error",
      rt::isError(runError) ? rt::errorName(runError) : "unknown_exception", std::nullopt, "gateway"};
    auto replaySnapshot = sentinel::replay_observation::snapshotSentinelReplayInput(sentinelReplayInput);
    sentinel::replay_observation::zeroSentinelReplayInput(&sentinelReplayInput);
    auto persist = options.persistSentinelReplay
      ? std::make_shared<decltype(options.persistSentinelReplay)>(std::move(options.persistSentinelReplay)) : nullptr;
    rt::Promise<sentinel::replay_observation::SentinelReplayPersistResult> persisted = persist
      ? (*persist)(replaySnapshot, observation)
      : rt::Promise<sentinel::replay_observation::SentinelReplayPersistResult>(sentinel::replay_store::persistSentinelReplayFromEnvironment(replaySnapshot, observation));
    auto persistence = persisted.then([](const sentinel::replay_observation::SentinelReplayPersistResult&) {}, [](rt::Reason) {}).then([replaySnapshot, persist] {
      (void)persist;
      sentinel::replay_observation::zeroSentinelReplayInput(&replaySnapshot);
    });
    if (!http::scheduleSentinelBackgroundTask(persistence, options.waitUntil)) co_await persistence.wait();
  }

  rt::Task<rt::Response> handleTerminalRoute(rt::Request req, std::string path, std::optional<http::RequestDeliveryInfo> delivery,
                                            std::string requestId, double requestStartedAtMs, double requestStartedAtMonotonicMs) {
    const auto terminalRoute = http::terminalRouteForRequest(req.method(), path);
    rt::Promise<auth::index::AuthenticateClientResult> authPending(auth::index::authenticateClient(req));
    auto auth = co_await authPending.wait();
    if (const auto* rejection = std::get_if<auth::index::AuthenticateClientResultNotOk>(&auth)) {
      const auto response = ::uos::http::withCors(http::withRequestId(rejection->response, requestId), req);
      terminal_log::RejectionTerminalLogInput input{.requestId = requestId, .admissionStage = "pre_admission",
        .startedAtMonotonicMs = requestStartedAtMonotonicMs, .delivery = delivery};
      auto pending = terminal_log::withRejectionTerminalLog(response, terminalRoute, input);
      auto result = co_await pending.wait();
      co_return result;
    }
    auto authResult = std::get<AuthenticatedClientResult>(auth);
    rt::Promise<std::string> principalPending(http::resolveIdempotencyPrincipal(authResult));
    auto principal = co_await principalPending.wait();
    auto state = std::make_shared<TerminalState>(req, path, delivery, requestId, requestStartedAtMs, requestStartedAtMonotonicMs, terminalRoute, authResult, principal);
    if (terminalRoute && admission::ADMISSION_ROUTES.has(*terminalRoute)) state->admissionRoute = terminalRoute;
    rt::Promise<std::optional<rt::Response>> admissionPending(state->acquireProcessPermit());
    auto admissionRefusal = co_await admissionPending.wait();
    if (admissionRefusal) co_return *admissionRefusal;
    state->usagePolicy = apiKeyPolicyFrom(authResult);
    std::function<rt::Task<std::shared_ptr<sentinel::replay_model::SentinelReplayCaptureCandidate>>()> capture = [state] {
      return rt::fulfilledTask(state->terminalRoute ? sentinel::replay_observation::captureAcceptedSentinelReplayInput(state->req, state->requestId) : nullptr);
    };
    rt::Promise<std::shared_ptr<sentinel::replay_model::SentinelReplayCaptureCandidate>> captured(state->releaseOnThrow(capture));
    state->sentinelReplayCandidate = co_await captured.wait();
    rt::Promise<std::optional<rt::Response>> usagePending(state->reserveUsageForRequest());
    auto usageRefusal = co_await usagePending.wait();
    if (usageRefusal) co_return *usageRefusal;
    std::function<rt::Task<std::optional<KernelRepo>>()> resolve = [state] { return resolveKernelRepo(state->req, state->authResult); };
    rt::Promise<std::optional<KernelRepo>> repoPending(state->releaseOnThrow(resolve));
    state->kernelRepo = co_await repoPending.wait();
    state->kernelQuotaRoute = http::kernelQuotaRouteForRequest(req.method(), path);
    rt::Promise<std::optional<rt::Response>> kernelPending(state->reserveKernelForRequest());
    auto kernelRefusal = co_await kernelPending.wait();
    if (kernelRefusal) co_return *kernelRefusal;
    if (terminalRoute) state->sentinelUpstreamRecorder = sentinel::upstream_capture::createSentinelUpstreamRecorder();
    state->usageContext.keyId = apiKeyIdFrom(authResult);
    state->usageContext.kernelRepo = state->kernelRepo;
    if (state->kernelRepo) state->usageContext.kernelOrg = openai_telemetry::UsageContext::KernelOrg{state->kernelRepo->owner};
    state->usageContext.paidFallbackEnabled = state->usagePolicy && state->usagePolicy->paid_fallback_enabled;
    state->usageContext.idempotencyPrincipal = principal;
    state->usageContext.requestId = requestId;
    state->usageContext.startedAtMs = requestStartedAtMs;
    state->usageContext.startedAtMonotonicMs = requestStartedAtMonotonicMs;
    state->usageContext.downstreamSignal = downstreamSignalFor(req, delivery, state->kernelReservation);
    if (state->usageReservation) state->usageContext.beforeProviderDispatch = state->usageReservation->beforeProviderDispatch;
    state->usageContext.sentinelUpstreamRecorder = state->sentinelUpstreamRecorder;
    if (terminalRoute) {
      json::object event{{"request_id", requestId}, {"route", *terminalRoute}, {"queue_wait_ms", state->admissionWaitMs.value_or(0)},
        {"git_sha", config::runtimeGitSha()}, {"deno_revision", config::runtimeDeploymentId()}};
      rt::console::info("[ai.ubq.fi] request_accepted", rt::jsonStringify(event));
    }
    rt::Promise<std::optional<rt::Response>> abortedPending(state->refuseAbortedBeforeDispatch());
    auto abortedRefusal = co_await abortedPending.wait();
    if (abortedRefusal) co_return *abortedRefusal;
    std::function<rt::Task<rt::Response>()> dispatch = [state] { return state->dispatchTerminalRoute(); };
    rt::Promise<rt::Response> pending(state->releaseOnThrow(dispatch));
    auto result = co_await pending.wait();
    co_return result;
  }
}
