// The Codex subscription transport entry point and the public Codex surface it re-exports.
#include "uos/codex/index.hpp"

#include "uos/codex/responses_operations.hpp"
#include "uos/codex/responses_runners.hpp"
#include "uos/codex/responses_state.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <utility>

namespace uos::codex::index {
  namespace {
    constexpr unsigned CODEX_STREAM_RETRY_LIMIT = 3;

    rt::Task<rt::Response> dispatchSelectedCodexResponses(std::shared_ptr<responses_state::CodexResponseContext> ctx) {
      responses_operations::installCodexResponseOperations(ctx);
      responses_runners::installCodexResponseRunners(ctx);
      if (const auto* blocked = std::get_if<routing_state::RouteSelectionQuotaBlocked>(&ctx->selected)) {
        rt::Promise<rt::Response> exhausting(ctx->exhaustedQuotaBlockedCohort(blocked->retryAtMs));
        rt::Response response = co_await exhausting.wait();
        co_return response;
      }
      // The loop runs inside this frame, which keeps `ctx` alive for it.
      responses_state::CodexResponseContext* const context = ctx.get();
      dispatch::CodexSerialAdmissionDrivers drivers;
      drivers.runPendingShortRetry = ctx->runPendingShortRetry;
      drivers.dispatchActive = ctx->dispatchEligibleAccounts;
      drivers.terminalTransportResponse = ctx->terminalTransportOutcome;
      drivers.hasQueuedRetry = [context]() { return context->retryState.candidate.has_value(); };
      drivers.reselectionRequested = [context]() { return context->activeReselectionRequested; };
      drivers.advanceReselection = ctx->advanceActiveReselection;
      drivers.exhaustedResponse = ctx->exhaustedCodexDispatchResponse;
      rt::Promise<rt::Response> running(dispatch::runCodexSerialAdmissionLoop(std::move(drivers)));
      rt::Response response = co_await running.wait();
      co_return response;
    }

    double streamRetryDelay(unsigned retry, double retryAfterMs) {
      std::array<std::uint8_t, 1> entropy{};
      rt::crypto::getRandomValues(std::span<std::uint8_t>(entropy));
      const double jitter = 1.0 + static_cast<double>(entropy[0]) / 1020.0;
      const double backoff = 250.0 * static_cast<double>(1U << retry) * jitter;
      // setTimeout treats delays greater than INT32_MAX as 1 ms. The request
      // deadline will fire well before this timer when an upstream hint is huge.
      return std::min(2'147'483'647.0, std::ceil(std::max(backoff, retryAfterMs)));
    }

    rt::Task<rt::Response> fetchPreparedCodexResponses(dispatch::PreparedCodexSubscriptionRequest prepared, dispatch::FetchCodexResponsesOptions options,
                                                       dispatch::CodexProviderDispatchCoordinator providerDispatch) {
      double attemptNumber = 0;
      for (unsigned retries = 0;; ++retries) {
        rt::Promise<responses_state::CreateCodexResponseStateResult> creating(responses_state::createCodexResponseState(prepared, options, providerDispatch));
        const auto created = co_await creating.wait();
        if (const auto* terminal = std::get_if<responses_state::CreateCodexResponseStateResultTerminal>(&created)) {
          co_return auth::withCodexWarnings(terminal->response, prepared.warnings);
        }
        const auto ctx = std::get<responses_state::CreateCodexResponseStateResultState>(created).state;
        ctx->attemptNumber = attemptNumber;
        ctx->streamRetryPending = retries > 0;
        auto dispatched = co_await dispatchSelectedCodexResponses(ctx);
        auto response = auth::withCodexWarnings(dispatched, prepared.warnings);
        if (!ctx->streamAttempt) co_return response;

        auto& attempt = *ctx->streamAttempt;
        const auto& hooks = *options.streamRetry;
        auto inspecting = hooks.preflight(response);
        std::optional<dispatch::CodexStreamRetry> retry;
        try { retry = co_await std::move(inspecting); }
        catch (...) { dispatch::logCodexRouting("codex_attempt", attempt.log); throw; }
        if (retry) {
          attempt.log["http_status"] = response.status();
          attempt.log["status"] = retry->status;
          attempt.log["status_class"] = dispatch::codexStatusClass(retry->status);
        }
        dispatch::logCodexRouting("codex_attempt", attempt.log);
        if (!retry) co_return response;

        // Release the probe and consume this response's terminal health outcome.
        // The retry carries the same request reservation and never settles usage.
        auto releasing = auth::releaseCodexResponseProbe(response);
        co_await std::move(releasing);
        const rt::Response classified(nullptr, {.status = retry->status, .headers = response.headers()});
        auto recording = dispatch::recordCodexResponseHealth(attempt.accountId, classified);
        try { co_await std::move(recording); } catch (...) {}
        if (!attempt.retryAllowed || retries == CODEX_STREAM_RETRY_LIMIT) co_return response;

        auto discarding = hooks.discard();
        co_await std::move(discarding);
        const double delay = streamRetryDelay(retries, retry->retryAfterMs);
        auto sleep = options.retrySleep;
        if (!sleep) sleep = [signal = options.signal](double ms) { return signal ? rt::sleep(ms, *signal) : rt::sleep(ms); };
        auto waiting = dispatch::waitForCodexRetry(delay, options.signal, sleep);
        co_await std::move(waiting);
        if (options.signal && options.signal->aborted()) rt::rethrow(options.signal->reason());
        attemptNumber = ctx->attemptNumber;
      }
    }
  }

  rt::Task<rt::Response> fetchCodexResponses(json::value body) { return fetchCodexResponses(std::move(body), dispatch::FetchCodexResponsesOptions{}); }

  rt::Task<rt::Response> fetchCodexResponses(json::value body, dispatch::FetchCodexResponsesOptions options) {
    rt::Promise<dispatch::PreparedCodexSubscriptionRequest> preparing(dispatch::prepareCodexSubscriptionRequest(body, options.cacheScope));
    const dispatch::PreparedCodexSubscriptionRequest prepared = co_await preparing.wait();
    const dispatch::CodexProviderDispatchCoordinator providerDispatch = dispatch::createCodexProviderDispatchCoordinator(options.beforeDispatch);
    std::optional<rt::Response> result;
    rt::Reason failure;
    try {
      rt::Promise<rt::Response> fetching(fetchPreparedCodexResponses(prepared, options, providerDispatch));
      result = co_await fetching.wait();
    } catch (...) {
      failure = std::current_exception();
    }
    rt::Task<void> cancelling = providerDispatch.cancelBeforeTransport();
    co_await std::move(cancelling);
    if (failure) std::rethrow_exception(failure);
    co_return *result;
  }
}
