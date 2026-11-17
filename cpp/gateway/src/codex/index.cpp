// The Codex subscription transport entry point and the public Codex surface it re-exports.
#include "uos/codex/index.hpp"

#include "uos/codex/responses_operations.hpp"
#include "uos/codex/responses_runners.hpp"
#include "uos/codex/responses_state.hpp"

#include <exception>
#include <utility>

namespace uos::codex::index {
  namespace {
    rt::Task<rt::Response> fetchPreparedCodexResponses(dispatch::PreparedCodexSubscriptionRequest prepared, dispatch::FetchCodexResponsesOptions options,
                                                       dispatch::CodexProviderDispatchCoordinator providerDispatch) {
      rt::Promise<responses_state::CreateCodexResponseStateResult> creating(responses_state::createCodexResponseState(prepared, options, providerDispatch));
      const responses_state::CreateCodexResponseStateResult created = co_await creating.wait();
      if (const auto* terminal = std::get_if<responses_state::CreateCodexResponseStateResultTerminal>(&created)) co_return terminal->response;
      const std::shared_ptr<responses_state::CodexResponseContext> ctx = std::get<responses_state::CreateCodexResponseStateResultState>(created).state;
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
      const rt::Response response = co_await fetching.wait();
      result = auth::withCodexWarnings(response, prepared.warnings);
    } catch (...) {
      failure = std::current_exception();
    }
    rt::Task<void> cancelling = providerDispatch.cancelBeforeTransport();
    co_await std::move(cancelling);
    if (failure) std::rethrow_exception(failure);
    co_return *result;
  }
}
