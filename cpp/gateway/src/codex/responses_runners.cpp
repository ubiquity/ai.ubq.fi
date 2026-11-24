// Codex Responses execution helpers, split out of src/codex.ts.
#include "uos/codex/responses_runners.hpp"

#include "uos/codex/account_routing.hpp"
#include "uos/codex/auth_refresh.hpp"
#include "uos/codex/capacity_routing.hpp"
#include "uos/codex/rate_limit_429.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"

#include <exception>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace uos::codex::responses_runners {
  namespace {
    using auth::CodexError;
    using dispatch::CodexBankedResetCandidate;
    using responses_state::CodexAccountAttemptOutcome;
    using responses_state::CodexAccountAttemptOutcomeNextAccount;
    using responses_state::CodexAccountAttemptOutcomeReselectActive;
    using responses_state::CodexAccountAttemptOutcomeResponse;
    using responses_state::CodexAccountAttemptOutcomeStop;
    using responses_state::CodexCredentialAttempt;
    using responses_state::CodexResponseContext;
    using responses_state::CodexRetryCandidate;
    using responses_state::EvaluatedBlockedReset;
    using routing_state::CodexActiveAccountSnapshot;
    using routing_state::RouteSelection;
    using routing_state::RoutingAccount;
    using ::uos::types::CodexAuthState;
    using Context = std::shared_ptr<CodexResponseContext>;
    using Operations = responses_state::CodexResponseOperations;
    using Runners = responses_state::CodexResponseRunners;

    // `reason instanceof E`
    template <class E> bool reasonIs(const rt::Reason& reason) noexcept {
      if (!reason) return false;
      try {
        std::rethrow_exception(reason);
      } catch (const E&) {
        return true;
      } catch (...) {
      }
      return false;
    }

    // The installed closures hold the context weakly (it owns them); a running runner holds it strongly.
    Context lockContext(const std::weak_ptr<CodexResponseContext>& weak) {
      Context ctx = weak.lock();
      if (!ctx) throw std::logic_error("Codex response runner called after its request context ended");
      return ctx;
    }

    json::value stringOrNull(const std::optional<std::string>& value) { return value.has_value() ? json::value(*value) : json::value(nullptr); }
    json::value numberOrNull(const std::optional<double>& value) { return value.has_value() ? json::value(*value) : json::value(nullptr); }
    json::value nullableOrNull(const rt::Nullable<std::string>& value) { return value.isNullish() ? json::value(nullptr) : json::value(*value); }

    Operations::CaptureBankedResetCandidateDisposition dispositionOf(const rate_limit_429::Codex429Classification& classification) {
      return Operations::CaptureBankedResetCandidateDisposition{.usageLimitReached = classification.usageLimitReached,
                                                                .retryAtMs = classification.retryAtMs,
                                                                .resetDeadlineIsStable = classification.resetDeadlineIsStable};
    }

    void noteReauthenticationWarning(CodexResponseContext& ctx) {
      if (!ctx.authWarning.has_value()) ctx.authWarning = std::string(auth::CODEX_AUTH_REAUTH_WARNING);
    }

    void cancelLastResponse(const CodexResponseContext& ctx) {
      if (ctx.lastResponse.has_value()) dispatch::cancelResponseBody(*ctx.lastResponse);
    }

    // Plain forwarders (not coroutines): the caller's rt::Promise wraps the async call itself.
    rt::Task<void> markCredentialInvalid(RoutingAccount routing) { return routing_mutations::markCodexCredentialInvalid(std::move(routing)); }

    rt::Task<void> releaseProbe(RoutingAccount routing) { return routing_mutations::releaseCodexRoutingProbe(std::move(routing)); }

    // ---- The runners installCodexResponseRunners assigns, in its order. ----

    bool responseIsCodexAuthFailure(const CodexAuthState& auth, const rt::Response& response) {
      return response.status() == 401 || (response.status() == 403 && auth::accessTokenExpired(auth));
    }

    // `normalResponse: Response | null`
    rt::Task<rt::Response> finalizePostResetRetry(Context ctx, CodexBankedResetCandidate retryCandidate, rt::Response retried,
                                                  std::optional<rt::Response> normalResponse) {
      if (normalResponse.has_value()) dispatch::cancelResponseBody(*normalResponse);
      if (ctx->responseIsCodexAuthFailure(retryCandidate.auth, retried)) {
        rt::Promise<void> marking(markCredentialInvalid(retryCandidate.routing));
        co_await marking.wait();
        noteReauthenticationWarning(*ctx);
      }
      rt::Response response = retried;
      if (retried.status() == 429) {
        // This is the one permitted post-reset inference attempt. Preserve a
        // replayable normal 429 and never feed it back into reset selection.
        rt::Promise<rate_limit_429::Codex429Classification> marking(routing_mutations::markCodexRecoveryProbeQuotaBlocked(retryCandidate.routing, retried));
        const rate_limit_429::Codex429Classification disposition = co_await marking.wait();
        response = disposition.response;
        auth::inheritCodexResponseActiveTelemetry(response, retried);
        if (disposition.usageLimitReached && disposition.retryAtMs.has_value()) {
          rt::Promise<std::string> capturing(
            ctx->captureBankedResetCandidate(retryCandidate.accountEntry, retryCandidate.routing, retryCandidate.auth, dispositionOf(disposition)));
          co_await capturing.wait();
        } else {
          ctx->bankedResetCandidates.clear();
        }
      } else if (!retried.ok()) {
        rt::Promise<void> releasing(releaseProbe(retryCandidate.routing));
        co_await releasing.wait();
      }
      if (response.ok() && retryCandidate.routing.probeGeneration.has_value()) auth::codexProbeByResponse.set(response, retryCandidate.routing);
      co_return response;
    }

    // The fields of the codex_reset_inference_retry* events; `status` is written when given (undefined members are dropped).
    json::object retryEventFields(const CodexResponseContext& ctx, const banked_reset::CodexBankedResetOutcome& reset, double routingGeneration,
                                  const std::optional<json::value>& status) {
      json::object fields;
      fields["request_id"] = stringOrNull(ctx.options.requestId);
      rt::objectSet(fields, "account_id_hash", reset.accountIdHash);
      rt::objectSet(fields, "quota_generation", reset.quotaGeneration);
      rt::objectSet(fields, "idempotency_key_hash", reset.idempotencyKeyHash);
      fields["routing_generation"] = routingGeneration;
      if (status.has_value()) fields["status"] = *status;
      return fields;
    }

    rt::Task<std::optional<rt::Response>> runPostResetRetry(Context ctx, EvaluatedBlockedReset evaluated, std::optional<rt::Response> normalResponse) {
      const CodexBankedResetCandidate& candidate = evaluated.candidate;
      const banked_reset::CodexBankedResetOutcome& reset = evaluated.reset;
      const CodexActiveAccountSnapshot& originalActive = evaluated.originalActive;
      const std::optional<::uos::types::CodexResetRedemptionRecord>& resetRecord = reset.record;
      if (!resetRecord.has_value() || (reset.kind != "verified" && !evaluated.staleVerified)) {
        co_return normalResponse;
      }
      rt::Promise<void> persisting(ctx->persistVerifiedCapacityReset(candidate, resetRecord));
      co_await persisting.wait();
      // Elect the verified account before any recovery-probe acquisition or
      // inference. The original active snapshot, the candidate's pool credential
      // and quota fence, and the full-cohort exhaustion proof must all still hold.
      rt::Promise<std::optional<RoutingAccount>> electing(
        account_routing::electCodexResetRecoveryAccount(candidate.routing, originalActive, candidate.quotaResetAtMs, evaluated.staleVerified));
      const std::optional<RoutingAccount> elected = co_await electing.wait();
      if (!elected.has_value()) co_return normalResponse;
      CodexBankedResetCandidate electedCandidate = candidate;
      electedCandidate.routing = *elected;
      electedCandidate.accountEntry.auth = elected->auth;
      electedCandidate.accountEntry.routing = *elected;
      std::optional<RoutingAccount> reconciled;
      if (evaluated.staleVerified) {
        std::vector<responses_state::BankedResetFence> fences;
        fences.push_back(ctx->authPoolFence(electedCandidate));
        fences.push_back(ctx->activeSelectionFence(electedCandidate));
        routing_mutations::CodexQuotaResetReconciliationInput input{
          .quotaResetAtMs = candidate.quotaResetAtMs,
          // The stale predicate proves the current circuit, not the ledger's
          // older generation.
          .routingGeneration = candidate.routingGeneration,
          .fences = std::move(fences),
        };
        rt::Promise<std::optional<RoutingAccount>> reconciling(routing_mutations::reconcileCodexQuotaAfterStaleVerifiedReset(*elected, std::move(input)));
        reconciled = co_await reconciling.wait();
      } else {
        routing_mutations::CodexQuotaResetReconciliationInput input{
          .quotaResetAtMs = candidate.quotaResetAtMs,
          .routingGeneration = resetRecord->routing_generation,
          .fences = ctx->electedResetFences(electedCandidate, resetRecord->routing_generation),
        };
        rt::Promise<std::optional<RoutingAccount>> reconciling(routing_mutations::reconcileCodexQuotaAfterVerifiedReset(*elected, std::move(input)));
        reconciled = co_await reconciling.wait();
      }
      if (!reconciled.has_value()) co_return normalResponse;
      rt::Promise<std::optional<CodexBankedResetCandidate>> refreshing(ctx->refreshBankedResetCandidate(electedCandidate));
      const std::optional<CodexBankedResetCandidate> refreshedCandidate = co_await refreshing.wait();
      if (!refreshedCandidate.has_value()) co_return normalResponse;
      const CodexBankedResetCandidate retryCandidate = ctx->withResetRecoveryProbe(*refreshedCandidate, *reconciled);

      ctx->logBankedResetEvent("codex_reset_inference_retry", retryEventFields(*ctx, reset, resetRecord->routing_generation, std::nullopt));
      std::optional<rt::Response> retriedResponse;
      rt::Reason failure;
      try {
        std::function<rt::Task<void>()> fence = [ctx, retryCandidate]() { return ctx->ensurePostResetRetryAuthCurrent(retryCandidate); };
        rt::Promise<rt::Response> attempting(
          ctx->fetchAttempt(retryCandidate.accountEntry, retryCandidate.auth, retryCandidate.routing, "post_banked_reset", std::move(fence), std::nullopt));
        retriedResponse = co_await attempting.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) {
        if (reasonIs<dispatch::CodexBankedResetRetryFenceError>(failure) || reasonIs<dispatch::CodexActiveAccountFenceError>(failure)) co_return normalResponse;
        const CodexError* codexError = auth::codexErrorOf(failure);
        const json::value status = codexError != nullptr ? json::value(codexError->status) : json::value(nullptr);
        ctx->logBankedResetEvent("codex_reset_inference_retry_result", retryEventFields(*ctx, reset, resetRecord->routing_generation, status));
        banked_reset::reportCodexBankedResetMetric(ctx->bankedResetDependencies.telemetry, "codex_reset_post_retry_total", 1,
                                                   retryEventFields(*ctx, reset, resetRecord->routing_generation, status));
        std::rethrow_exception(failure);
      }
      rt::Promise<rt::Response> finalizing(ctx->finalizePostResetRetry(retryCandidate, *retriedResponse, normalResponse));
      const rt::Response retried = co_await finalizing.wait();
      const json::value status(retried.status());
      ctx->logBankedResetEvent("codex_reset_inference_retry_result", retryEventFields(*ctx, reset, resetRecord->routing_generation, status));
      banked_reset::reportCodexBankedResetMetric(ctx->bankedResetDependencies.telemetry, "codex_reset_post_retry_total", 1,
                                                 retryEventFields(*ctx, reset, resetRecord->routing_generation, status));
      rt::Promise<rt::Response> authorizing(ctx->authorizePaidFallbackForCompleteCapacityEvidence(retried));
      const rt::Response authorized = co_await authorizing.wait();
      co_return ctx->decorateAuthWarning(authorized);
    }

    rt::Task<rt::Response> redeemAndRetryOnce(Context ctx, rt::Response normalResponse) {
      // A half-open account could still be healthy, but another isolate owns
      // its recovery probe. Keep the normal quota response instead of spending
      // a reset that was inferred only from a sibling's 429.
      if (ctx->probeUnavailable) co_return normalResponse;
      rt::Promise<std::optional<EvaluatedBlockedReset>> evaluating(ctx->evaluateBlockedCohortBankedReset());
      const std::optional<EvaluatedBlockedReset> evaluated = co_await evaluating.wait();
      if (!evaluated.has_value()) {
        // No banked reset could be redeemed for this episode: one bounded
        // overage probe may still serve the request for a blocked account.
        rt::Promise<std::optional<rt::Response>> overage(ctx->runOverageFallbackAttempt());
        const std::optional<rt::Response> served = co_await overage.wait();
        co_return served.has_value() ? *served : normalResponse;
      }
      rt::Promise<std::optional<rt::Response>> retrying(ctx->runPostResetRetry(*evaluated, normalResponse));
      const std::optional<rt::Response> retried = co_await retrying.wait();
      co_return retried.has_value() ? *retried : normalResponse;
    }

    rt::Task<std::optional<rt::Response>> recoverBlockedReset(Context ctx) {
      rt::Promise<std::optional<EvaluatedBlockedReset>> evaluating(ctx->evaluateBlockedCohortBankedReset());
      const std::optional<EvaluatedBlockedReset> evaluated = co_await evaluating.wait();
      if (evaluated.has_value()) {
        rt::Promise<std::optional<rt::Response>> retrying(ctx->runPostResetRetry(*evaluated, std::nullopt));
        std::optional<rt::Response> retried = co_await retrying.wait();
        co_return retried;
      }
      rt::Promise<std::optional<rt::Response>> overage(ctx->runOverageFallbackAttempt());
      std::optional<rt::Response> served = co_await overage.wait();
      co_return served;
    }

    // `retryAtMs: number | null`
    rt::Task<rt::Response> exhaustedQuotaBlockedCohort(Context ctx, std::optional<double> retryAtMs) {
      rt::Promise<std::optional<rt::Response>> recovering(ctx->recoverBlockedReset());
      const std::optional<rt::Response> recovered = co_await recovering.wait();
      if (recovered.has_value()) co_return *recovered;
      rt::Promise<rt::Response> responding(ctx->quotaBlockedOrRetryableResponse(retryAtMs));
      rt::Response response = co_await responding.wait();
      co_return response;
    }

    rt::Task<Runners::ClaimAttemptRoutingResult> claimAttemptRouting(Context ctx, RoutingAccount routing) {
      if (!routing.probeRequired) co_return Runners::ClaimAttemptRoutingResultClaimed{.routing = routing};
      rt::Promise<std::optional<RoutingAccount>> claiming(routing_mutations::claimCodexRoutingProbe(ctx->poolEntry.pool, routing));
      const std::optional<RoutingAccount> claimed = co_await claiming.wait();
      if (!claimed.has_value()) {
        ctx->probeUnavailable = true;
        if (!routing.probeCircuit.isNullish() && *routing.probeCircuit == "upstream_timeout") ctx->probeUnavailableCircuit = "upstream_timeout";
        co_return Runners::ClaimAttemptRoutingResultUnavailable{};
      }
      co_return Runners::ClaimAttemptRoutingResultClaimed{.routing = *claimed};
    }

    rt::Task<Runners::RecoverCodexAuthFailureResult> recoverCodexAuthFailure(Context ctx, auth::CodexDispatchAccountEntry accountEntry,
                                                                             std::shared_ptr<CodexCredentialAttempt> attempt, rt::Response response) {
      if (!ctx->responseIsCodexAuthFailure(attempt->auth, response)) co_return Runners::RecoverCodexAuthFailureResultRecovered{.response = response};
      if (ctx->refreshedSlots.has(attempt->routing.slot)) {
        rt::Promise<void> marking(markCredentialInvalid(attempt->routing));
        co_await marking.wait();
        ctx->requestActiveReselection(Operations::RequestActiveReselectionOptions{});
        noteReauthenticationWarning(*ctx);
        co_return Runners::RecoverCodexAuthFailureResultRecovered{.response = response};
      }
      dispatch::cancelResponseBody(response);
      rt::Reason failure;
      try {
        rt::Promise<CodexCredentialAttempt> refreshing(ctx->refreshAfter401(attempt->routing, attempt->auth, "401"));
        const CodexCredentialAttempt refreshed = co_await refreshing.wait();
        attempt->auth = refreshed.auth;
        attempt->routing = refreshed.routing;
        rt::Promise<rt::Response> attempting(ctx->fetchAttempt(accountEntry, attempt->auth, attempt->routing, "post_refresh", {}, std::nullopt));
        const rt::Response retried = co_await attempting.wait();
        if (ctx->responseIsCodexAuthFailure(attempt->auth, retried)) {
          rt::Promise<void> marking(markCredentialInvalid(attempt->routing));
          co_await marking.wait();
          ctx->requestActiveReselection(Operations::RequestActiveReselectionOptions{});
          noteReauthenticationWarning(*ctx);
        }
        co_return Runners::RecoverCodexAuthFailureResultRecovered{.response = retried};
      } catch (...) {
        failure = std::current_exception();
      }
      const CodexError* codexError = auth::codexErrorOf(failure);
      if (codexError != nullptr && codexError->status == 401) {
        rt::Promise<void> marking(markCredentialInvalid(attempt->routing));
        co_await marking.wait();
        ctx->requestActiveReselection(Operations::RequestActiveReselectionOptions{});
        ctx->noteCodexAuthFailure(failure);
        ctx->lastError = failure;
        co_return Runners::RecoverCodexAuthFailureResultReselectActive{};
      }
      std::rethrow_exception(failure);
    }

    rt::Task<CodexAccountAttemptOutcome> classifyAccountAttemptResponse(Context ctx, auth::CodexDispatchAccountEntry accountEntry, CodexCredentialAttempt attempt,
                                                                        rt::Response response) {
      rt::Response current = response;
      if (current.status() == 429) {
        rt::Promise<rt::Response> classifying(ctx->classify429(accountEntry, attempt.routing, attempt.auth, current));
        current = co_await classifying.wait();
        auth::setCodexResponseAccountTelemetry(current, attempt.routing.slot + 1, attempt.auth.account_id);
        auth::inheritCodexResponseActiveTelemetry(current, response);
      } else if (!current.ok()) {
        rt::Promise<void> releasing(releaseProbe(attempt.routing));
        co_await releasing.wait();
      }
      if (current.ok() && attempt.routing.probeGeneration.has_value()) auth::codexProbeByResponse.set(current, attempt.routing);
      if (current.status() == 401 || current.status() == 403 || current.status() == 429) {
        cancelLastResponse(*ctx);
        ctx->lastResponse = current;
        co_return CodexAccountAttemptOutcomeNextAccount{};
      }
      cancelLastResponse(*ctx);
      if (current.ok()) {
        rt::Promise<void> reporting(ctx->reportHealthyFallback());
        co_await reporting.wait();
      }
      co_return CodexAccountAttemptOutcomeResponse{.response = ctx->decorateAuthWarning(current)};
    }

    rt::Task<CodexAccountAttemptOutcome> absorbSiblingTransportFailure(Context ctx, CodexError error, RoutingAccount routing) {
      if (ctx->transportState.started) {
        if (error.code != "gateway_timeout") {
          rt::Promise<void> releasing(releaseProbe(routing));
          co_await releasing.wait();
        }
        cancelLastResponse(*ctx);
        throw error;
      }
      ctx->transportFailure = error;
      rt::Promise<void> releasing(releaseProbe(routing));
      co_await releasing.wait();
      if (ctx->lastResponse.has_value()) {
        dispatch::cancelResponseBody(*ctx->lastResponse);
        ctx->lastResponse = std::nullopt;
      }
      ctx->retryState.candidate = std::nullopt;
      ctx->bankedResetCandidates.clear();
      if (ctx->options.signal.has_value() && ctx->options.signal->aborted()) co_return CodexAccountAttemptOutcomeStop{};
      co_return CodexAccountAttemptOutcomeNextAccount{};
    }

    // `error: unknown`
    rt::Task<CodexAccountAttemptOutcome> handleAccountAttemptFailure(Context ctx, rt::Reason error, RoutingAccount routing) {
      ctx->lastError = error;
      const CodexError* codexError = auth::codexErrorOf(error);
      if (codexError != nullptr && codexError->status == 401) {
        rt::Promise<void> marking(markCredentialInvalid(routing));
        co_await marking.wait();
        ctx->requestActiveReselection(Operations::RequestActiveReselectionOptions{});
        ctx->noteCodexAuthFailure(error);
        co_return CodexAccountAttemptOutcomeReselectActive{};
      }
      if (reasonIs<dispatch::CodexActiveAccountFenceError>(error)) {
        // The admission never reached transport, so its half-open lease is
        // released before the bounded loop re-elects the current active account.
        rt::Promise<void> releasing(releaseProbe(routing));
        co_await releasing.wait();
        ctx->requestActiveReselection(Operations::RequestActiveReselectionOptions{});
        co_return CodexAccountAttemptOutcomeReselectActive{};
      }
      if (dispatch::isCodexSiblingTransportFailure(error)) {
        rt::Promise<CodexAccountAttemptOutcome> absorbing(ctx->absorbSiblingTransportFailure(*codexError, routing));
        CodexAccountAttemptOutcome outcome = co_await absorbing.wait();
        co_return outcome;
      }
      rt::Promise<void> releasing(releaseProbe(routing));
      co_await releasing.wait();
      // Other request-local failures never open a shared provider gate.
      cancelLastResponse(*ctx);
      std::rethrow_exception(error);
    }

    rt::Task<CodexAccountAttemptOutcome> attemptCodexAccount(Context ctx, auth::CodexDispatchAccountEntry accountEntry) {
      rt::Promise<Runners::ClaimAttemptRoutingResult> claiming(ctx->claimAttemptRouting(accountEntry.routing.value()));
      const Runners::ClaimAttemptRoutingResult claim = co_await claiming.wait();
      if (std::holds_alternative<Runners::ClaimAttemptRoutingResultUnavailable>(claim)) co_return CodexAccountAttemptOutcomeNextAccount{};
      const auto attempt = std::make_shared<CodexCredentialAttempt>(
        CodexCredentialAttempt{.auth = accountEntry.auth, .routing = std::get<Runners::ClaimAttemptRoutingResultClaimed>(claim).routing});
      if (attempt->routing.quotaUsageProbeRequired) {
        const auto config = ctx->reloadConfiguredBankedResetConfig();
        if (config.enabled && config.mode == "live") {
          using capacity_routing::QuotaUsageProbeResult;
          auto proof = QuotaUsageProbeResult::unavailable;
          if (!ctx->quotaUsageProbeAccounts.has(attempt->routing.accountIdHash)) {
            ctx->quotaUsageProbeAccounts.add(attempt->routing.accountIdHash);
            proof = co_await capacity_routing::refreshCodexQuotaUsage(attempt->routing, ctx->options.signal);
          }
          if (proof == QuotaUsageProbeResult::refreshed) {
            ctx->requestActiveReselection(Operations::RequestActiveReselectionOptions{});
            co_return CodexAccountAttemptOutcomeReselectActive{};
          }
          if (proof == QuotaUsageProbeResult::unavailable) {
            co_await capacity_routing::releaseCodexQuotaUsageProbe(attempt->routing);
            co_return CodexAccountAttemptOutcomeResponse{
              .response = dispatch::routingErrorResponse(429, "Codex capacity is temporarily unavailable; retry the request.", "codex_capacity_unavailable")};
          }
        }
        attempt->routing.quotaUsageProbeRequired = false;
      }
      rt::Reason failure;
      try {
        if (auth::needsRefresh(attempt->auth)) {
          rt::Promise<CodexCredentialAttempt> refreshing(ctx->refreshAfter401(attempt->routing, attempt->auth, "proactive"));
          const CodexCredentialAttempt refreshed = co_await refreshing.wait();
          attempt->auth = refreshed.auth;
          attempt->routing = refreshed.routing;
        }
        rt::Promise<rt::Response> attempting(ctx->fetchAttempt(accountEntry, attempt->auth, attempt->routing, "initial", {}, std::nullopt));
        const rt::Response response = co_await attempting.wait();
        rt::Promise<Runners::RecoverCodexAuthFailureResult> recovering(ctx->recoverCodexAuthFailure(accountEntry, attempt, response));
        const Runners::RecoverCodexAuthFailureResult recovered = co_await recovering.wait();
        if (std::holds_alternative<Runners::RecoverCodexAuthFailureResultReselectActive>(recovered)) co_return CodexAccountAttemptOutcomeReselectActive{};
        rt::Promise<CodexAccountAttemptOutcome> classifying(
          ctx->classifyAccountAttemptResponse(accountEntry, *attempt, std::get<Runners::RecoverCodexAuthFailureResultRecovered>(recovered).response));
        CodexAccountAttemptOutcome outcome = co_await classifying.wait();
        co_return outcome;
      } catch (...) {
        failure = std::current_exception();
      }
      rt::Promise<CodexAccountAttemptOutcome> handling(ctx->handleAccountAttemptFailure(failure, attempt->routing));
      CodexAccountAttemptOutcome outcome = co_await handling.wait();
      co_return outcome;
    }

    rt::Task<std::optional<rt::Response>> dispatchEligibleAccounts(Context ctx) {
      const std::vector<auth::CodexDispatchAccountEntry> accountEntries = ctx->accountEntries;
      for (const auth::CodexDispatchAccountEntry& accountEntry : accountEntries) {
        rt::Promise<CodexAccountAttemptOutcome> attempting(ctx->attemptCodexAccount(accountEntry));
        const CodexAccountAttemptOutcome outcome = co_await attempting.wait();
        if (std::holds_alternative<CodexAccountAttemptOutcomeNextAccount>(outcome)) continue;
        if (std::holds_alternative<CodexAccountAttemptOutcomeStop>(outcome)) break;
        // An authoritative exhaustion or final credential failure asks the
        // bounded admission loop to re-elect the current global active account
        // instead of walking a stale sibling list.
        if (std::holds_alternative<CodexAccountAttemptOutcomeReselectActive>(outcome)) co_return std::nullopt;
        co_return std::get<CodexAccountAttemptOutcomeResponse>(outcome).response;
      }
      co_return std::nullopt;
    }

    rt::Task<std::optional<rt::Response>> terminalTransportOutcome(Context ctx) {
      if (ctx->transportFailure.has_value()) {
        cancelLastResponse(*ctx);
        throw *ctx->transportFailure;
      }
      if (ctx->probeUnavailableCircuit != std::string("upstream_timeout")) co_return std::nullopt;
      cancelLastResponse(*ctx);
      std::optional<double> retryAtMs;
      try {
        rt::Promise<auth::CodexAuthPoolEntry> loading(auth::getAuthPoolEntry(true, true));
        const auth::CodexAuthPoolEntry currentPoolEntry = co_await loading.wait();
        rt::Promise<RouteSelection> selecting(
          account_routing::selectCodexRoutingAccountsStrong(currentPoolEntry.pool, currentPoolEntry.pool.accounts, rt::dateNow(), ctx->requestedModel));
        const RouteSelection currentSelection = co_await selecting.wait();
        if (const auto* blocked = std::get_if<routing_state::RouteSelectionUpstreamBlocked>(&currentSelection)) retryAtMs = blocked->retryAtMs;
      } catch (...) {
        // The failed claim already proves a competing timeout probe. Preserve its
        // 503 classification if a fresh routing read is unavailable.
      }
      co_return dispatch::upstreamTimeoutCircuitResponse(retryAtMs);
    }

    rt::Task<rt::Response> redeemOrReportQuotaBlocked(Context ctx) {
      if (ctx->lastResponse.has_value()) {
        rt::Promise<rt::Response> redeeming(ctx->redeemAndRetryOnce(*ctx->lastResponse));
        rt::Response response = co_await redeeming.wait();
        co_return response;
      }
      rt::Promise<rt::Response> responding(ctx->quotaBlockedOrRetryableResponse(std::nullopt));
      rt::Response response = co_await responding.wait();
      co_return response;
    }

    rt::Task<Runners::PrepareTwoSecondRetryResult> prepareTwoSecondRetry(Context ctx, CodexRetryCandidate candidate) {
      const double retryCheckAtMs = rt::dateNow();
      const double retryDelayMs = rt::Math::max(0.0, candidate.readyAtMs - retryCheckAtMs);
      if (retryCheckAtMs > candidate.expiresAtMs || retryCheckAtMs + retryDelayMs > candidate.expiresAtMs) {
        rt::Promise<rt::Response> responding(ctx->redeemOrReportQuotaBlocked());
        const rt::Response response = co_await responding.wait();
        co_return Runners::PrepareTwoSecondRetryResultRespond{.response = response};
      }
      std::function<rt::Task<void>(double milliseconds)> sleep =
        ctx->options.retrySleep ? ctx->options.retrySleep : std::function<rt::Task<void>(double milliseconds)>(&auth_refresh::delay);
      rt::Promise<void> waiting(dispatch::waitForCodexRetry(retryDelayMs, ctx->options.signal, std::move(sleep)));
      co_await waiting.wait();
      if (rt::dateNow() > candidate.expiresAtMs) {
        rt::Promise<rt::Response> responding(ctx->redeemOrReportQuotaBlocked());
        const rt::Response response = co_await responding.wait();
        co_return Runners::PrepareTwoSecondRetryResultRespond{.response = response};
      }
      CodexCredentialAttempt attempt{.auth = candidate.auth, .routing = candidate.routing};
      if (attempt.routing.probeRequired) {
        const double claimNow = rt::Math::max(rt::dateNow(), candidate.readyAtMs);
        rt::Promise<std::optional<RoutingAccount>> claiming(routing_mutations::claimCodexRoutingProbe(ctx->poolEntry.pool, attempt.routing, claimNow));
        const std::optional<RoutingAccount> claimed = co_await claiming.wait();
        if (!claimed.has_value()) {
          if (ctx->lastResponse.has_value()) co_return Runners::PrepareTwoSecondRetryResultRespond{.response = *ctx->lastResponse};
          rt::Promise<rt::Response> responding(ctx->quotaBlockedOrRetryableResponse(std::nullopt));
          const rt::Response response = co_await responding.wait();
          co_return Runners::PrepareTwoSecondRetryResultRespond{.response = response};
        }
        attempt.routing = *claimed;
      }
      if (rt::dateNow() > candidate.expiresAtMs) {
        rt::Promise<void> releasing(releaseProbe(attempt.routing));
        co_await releasing.wait();
        rt::Promise<rt::Response> responding(ctx->redeemOrReportQuotaBlocked());
        const rt::Response response = co_await responding.wait();
        co_return Runners::PrepareTwoSecondRetryResultRespond{.response = response};
      }
      co_return Runners::PrepareTwoSecondRetryResultReady{.attempt = attempt, .delayMs = retryDelayMs};
    }

    rt::Task<Runners::RecoverTwoSecondRetryAuthFailureResult> recoverTwoSecondRetryAuthFailure(Context ctx, CodexRetryCandidate candidate,
                                                                                               std::shared_ptr<CodexCredentialAttempt> attempt,
                                                                                               rt::Response response) {
      if (!ctx->responseIsCodexAuthFailure(attempt->auth, response)) co_return Runners::RecoverTwoSecondRetryAuthFailureResultRecovered{.response = response};
      if (ctx->refreshedSlots.has(attempt->routing.slot)) {
        rt::Promise<void> marking(markCredentialInvalid(attempt->routing));
        co_await marking.wait();
        noteReauthenticationWarning(*ctx);
        co_return Runners::RecoverTwoSecondRetryAuthFailureResultRecovered{.response = response};
      }
      dispatch::cancelResponseBody(response);
      std::optional<rt::Response> retried;
      rt::Reason failure;
      try {
        rt::Promise<CodexCredentialAttempt> refreshing(ctx->refreshAfter401(attempt->routing, attempt->auth, "401"));
        const CodexCredentialAttempt refreshed = co_await refreshing.wait();
        attempt->auth = refreshed.auth;
        attempt->routing = refreshed.routing;
        rt::Promise<rt::Response> attempting(ctx->fetchAttempt(candidate.accountEntry, attempt->auth, attempt->routing, "post_retry_refresh", {}, std::nullopt));
        retried = co_await attempting.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) {
        const CodexError* codexError = auth::codexErrorOf(failure);
        if (codexError != nullptr && codexError->status == 401) {
          rt::Promise<void> marking(markCredentialInvalid(attempt->routing));
          co_await marking.wait();
          ctx->noteCodexAuthFailure(failure);
          co_return Runners::RecoverTwoSecondRetryAuthFailureResultRespond{.response = ctx->authFailureResponse(*codexError)};
        }
        if (!(codexError != nullptr && codexError->code == "gateway_timeout")) {
          rt::Promise<void> releasing(releaseProbe(attempt->routing));
          co_await releasing.wait();
        }
        std::rethrow_exception(failure);
      }
      if (ctx->responseIsCodexAuthFailure(attempt->auth, *retried)) {
        rt::Promise<void> marking(markCredentialInvalid(attempt->routing));
        co_await marking.wait();
        noteReauthenticationWarning(*ctx);
      }
      co_return Runners::RecoverTwoSecondRetryAuthFailureResultRecovered{.response = *retried};
    }

    rt::Task<rt::Response> classifyTwoSecondRetryResponse(Context ctx, CodexRetryCandidate candidate, CodexCredentialAttempt attempt, rt::Response response) {
      rt::Response current = response;
      if (current.status() == 429) {
        rt::Promise<rate_limit_429::Codex429Classification> marking(rate_limit_429::markCodexQuotaBlocked(attempt.routing, current));
        const rate_limit_429::Codex429Classification disposition = co_await marking.wait();
        current = disposition.response;
        if (disposition.usageLimitReached && disposition.retryAtMs.has_value()) {
          rt::Promise<std::string> capturing(ctx->captureBankedResetCandidate(candidate.accountEntry, attempt.routing, attempt.auth, dispositionOf(disposition)));
          co_await capturing.wait();
        } else {
          // The ordinary bounded retry gave a generic answer. It is not evidence
          // that a banked reset is safe to spend.
          ctx->bankedResetCandidates.clear();
        }
        auth::setCodexResponseAccountTelemetry(current, attempt.routing.slot + 1, attempt.auth.account_id);
        auth::inheritCodexResponseActiveTelemetry(current, response);
      } else if (!current.ok()) {
        // A 401/403 says this retrying account cannot serve, but does not erase
        // a separately verified quota-exhaustion candidate from another slot.
        // Other non-successes remain conservative and discard that candidate.
        if (current.status() != 401 && current.status() != 403) ctx->bankedResetCandidates.clear();
        rt::Promise<void> releasing(releaseProbe(attempt.routing));
        co_await releasing.wait();
      }
      if (current.ok() && attempt.routing.probeGeneration.has_value()) auth::codexProbeByResponse.set(current, attempt.routing);
      // A successful ordinary bounded retry has already served the original
      // inference request. A captured exhaustion observation from before that
      // retry is no longer a reason to spend a reset or issue another request.
      if (current.ok()) co_return ctx->decorateAuthWarning(current);
      rt::Promise<rt::Response> redeeming(ctx->redeemAndRetryOnce(current));
      const rt::Response redeemed = co_await redeeming.wait();
      rt::Promise<rt::Response> authorizing(ctx->authorizePaidFallbackForCompleteCapacityEvidence(redeemed));
      const rt::Response authorized = co_await authorizing.wait();
      co_return ctx->decorateAuthWarning(authorized);
    }

    rt::Task<rt::Response> runTwoSecondRetryCandidate(Context ctx, CodexRetryCandidate candidate) {
      rt::Promise<Runners::PrepareTwoSecondRetryResult> preparing(ctx->prepareTwoSecondRetry(candidate));
      const Runners::PrepareTwoSecondRetryResult ready = co_await preparing.wait();
      if (const auto* respond = std::get_if<Runners::PrepareTwoSecondRetryResultRespond>(&ready)) co_return respond->response;
      const Runners::PrepareTwoSecondRetryResultReady& readyAttempt = std::get<Runners::PrepareTwoSecondRetryResultReady>(ready);
      const auto attempt = std::make_shared<CodexCredentialAttempt>(readyAttempt.attempt);
      json::object fields;
      fields["request_id"] = stringOrNull(ctx->options.requestId);
      fields["slot"] = candidate.routing.slot + 1;
      fields["delay_ms"] = readyAttempt.delayMs;
      fields["active_generation"] = numberOrNull(candidate.routing.activeGeneration);
      fields["active_transition_reason"] = nullableOrNull(candidate.routing.activeTransitionReason);
      dispatch::logCodexRouting("codex_two_second_retry", fields);
      cancelLastResponse(*ctx);
      std::optional<rt::Response> response;
      rt::Reason failure;
      try {
        rt::Promise<rt::Response> attempting(ctx->fetchAttempt(candidate.accountEntry, attempt->auth, attempt->routing, "two_second_retry", {}, std::nullopt));
        response = co_await attempting.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) {
        const CodexError* codexError = auth::codexErrorOf(failure);
        if (!(codexError != nullptr && codexError->code == "gateway_timeout")) {
          rt::Promise<void> releasing(releaseProbe(attempt->routing));
          co_await releasing.wait();
        }
        std::rethrow_exception(failure);
      }
      rt::Promise<Runners::RecoverTwoSecondRetryAuthFailureResult> recovering(ctx->recoverTwoSecondRetryAuthFailure(candidate, attempt, *response));
      const Runners::RecoverTwoSecondRetryAuthFailureResult recovered = co_await recovering.wait();
      if (const auto* respond = std::get_if<Runners::RecoverTwoSecondRetryAuthFailureResultRespond>(&recovered)) co_return respond->response;
      rt::Promise<rt::Response> classifying(
        ctx->classifyTwoSecondRetryResponse(candidate, *attempt, std::get<Runners::RecoverTwoSecondRetryAuthFailureResultRecovered>(recovered).response));
      rt::Response classified = co_await classifying.wait();
      co_return classified;
    }

    // `ctx.authorizePaidFallbackForCompleteCapacityEvidence(ctx.authFailureResponse(error))`: a plain forwarder,
    // so the caller's rt::Promise wraps the async call itself.
    rt::Task<rt::Response> authorizeFailureResponse(const Context& ctx, std::optional<CodexError> error) {
      return ctx->authorizePaidFallbackForCompleteCapacityEvidence(ctx->authFailureResponse(std::move(error)));
    }

    rt::Task<rt::Response> exhaustedCodexDispatchResponse(Context ctx) {
      if (ctx->lastResponse.has_value()) {
        rt::Promise<rt::Response> redeeming(ctx->redeemAndRetryOnce(*ctx->lastResponse));
        const rt::Response redeemed = co_await redeeming.wait();
        rt::Promise<rt::Response> authorizing(ctx->authorizePaidFallbackForCompleteCapacityEvidence(redeemed));
        const rt::Response authorized = co_await authorizing.wait();
        co_return ctx->decorateAuthWarning(authorized);
      }
      if (ctx->authFailureState.error.has_value()) {
        rt::Promise<rt::Response> responding(authorizeFailureResponse(ctx, ctx->authFailureState.error));
        rt::Response response = co_await responding.wait();
        co_return response;
      }
      const CodexError* lastError = auth::codexErrorOf(ctx->lastError);
      if (lastError != nullptr && lastError->status == 401) {
        rt::Promise<rt::Response> responding(authorizeFailureResponse(ctx, *lastError));
        rt::Response response = co_await responding.wait();
        co_return response;
      }
      if (std::holds_alternative<routing_state::RouteSelectionCredentialsInvalid>(ctx->selected)) {
        noteReauthenticationWarning(*ctx);
        rt::Promise<rt::Response> responding(authorizeFailureResponse(ctx, std::nullopt));
        rt::Response response = co_await responding.wait();
        co_return response;
      }
      if (ctx->probeUnavailable) {
        // A held recovery lease is retryable, not proof of quota exhaustion.
        rt::Promise<rt::Response> responding(ctx->quotaBlockedOrRetryableResponse(std::nullopt));
        rt::Response response = co_await responding.wait();
        co_return response;
      }
      std::rethrow_exception(dispatch::dispatchFailureAsError(
        ctx->lastError, [] { return rt::makeReason(CodexError("Codex auth pool is empty.", "codex_auth_missing", 503)); }));
    }

    rt::Task<std::optional<rt::Response>> runPendingShortRetry(Context ctx) {
      const std::optional<CodexRetryCandidate> candidate = ctx->retryState.candidate;
      if (!candidate.has_value()) co_return std::nullopt;
      ctx->retryState.candidate = std::nullopt;
      rt::Reason failure;
      try {
        rt::Promise<rt::Response> running(ctx->runTwoSecondRetryCandidate(*candidate));
        rt::Response response = co_await running.wait();
        co_return response;
      } catch (...) {
        failure = std::current_exception();
      }
      if (!reasonIs<dispatch::CodexActiveAccountFenceError>(failure)) std::rethrow_exception(failure);
      // A concurrent transition superseded this request's admitted account;
      // re-admit the current global active instead of surfacing the fence.
      ctx->requestActiveReselection(Operations::RequestActiveReselectionOptions{});
      co_return std::nullopt;
    }

    std::optional<Runners::ReselectedActiveIdentityResult> reselectedActiveIdentity(const CodexResponseContext& ctx) {
      if (const auto* eligible = std::get_if<routing_state::RouteSelectionEligible>(&ctx.selected)) {
        const RoutingAccount* active = rt::at(eligible->accounts, 0);
        if (active == nullptr) throw rt::TypeError("undefined is not an object (evaluating 'active.accountIdHash')");
        return Runners::ReselectedActiveIdentityResult{.accountIdHash = active->accountIdHash, .generation = active->activeGeneration};
      }
      if (const auto* blocked = std::get_if<routing_state::RouteSelectionQuotaBlocked>(&ctx.selected)) {
        const std::optional<routing_state::CodexActiveAccountSelection>& active = blocked->activeSnapshot.selection;
        return Runners::ReselectedActiveIdentityResult{
          .accountIdHash = active.has_value() ? std::optional<std::string>(active->account_id_hash) : std::nullopt,
          .generation = active.has_value() ? std::optional<double>(active->generation) : std::nullopt};
      }
      return std::nullopt;
    }

    bool restorePreservedShortRetry(CodexResponseContext& ctx) {
      const std::optional<responses_state::CodexResponseState::PreservedShortRetryState::Current> preserved = ctx.preservedShortRetryState.current;
      ctx.preservedShortRetryState.current = std::nullopt;
      if (!preserved.has_value()) return false;
      const std::optional<Runners::ReselectedActiveIdentityResult> currentActive = ctx.reselectedActiveIdentity();
      if (!currentActive.has_value()) return false;
      if (currentActive->accountIdHash != preserved->accountIdHash || currentActive->generation != preserved->activeGeneration) return false;
      // Same active opaque identity and generation: the unchanged account may
      // still spend its one bounded retry before any reset or redemption.
      ctx.retryState.candidate = preserved->candidate;
      return true;
    }

    rt::Task<std::optional<rt::Response>> advanceActiveReselection(Context ctx) {
      ctx->activeReselectionRequested = false;
      bool failed = false;
      try {
        rt::Promise<auth::CodexAuthPoolEntry> loading(auth::getAuthPoolEntry(true, true));
        ctx->poolEntry = co_await loading.wait();
        rt::Promise<RouteSelection> selecting(
          account_routing::selectCodexRoutingAccountsStrong(ctx->poolEntry.pool, ctx->poolEntry.pool.accounts, rt::dateNow(), ctx->requestedModel));
        ctx->selected = co_await selecting.wait();
      } catch (...) {
        failed = true;
      }
      if (failed) co_return dispatch::routingErrorResponse(503, "Codex routing state is temporarily unavailable; retry the request.", "codex_auth_missing");
      if (std::holds_alternative<routing_state::RouteSelectionCredentialsInvalid>(ctx->selected)) {
        // Preserve the request-local actionable credential error (for example a
        // reused refresh token) instead of replacing it with the generic
        // all-credentials-invalid response.
        rt::Promise<rt::Response> responding(authorizeFailureResponse(ctx, ctx->authFailureState.error));
        rt::Response response = co_await responding.wait();
        co_return response;
      }
      const std::optional<rt::Response> reselectedTerminal = dispatch::initialCodexSelectionResponse(ctx->selected);
      if (reselectedTerminal.has_value()) co_return reselectedTerminal;
      if (ctx->restorePreservedShortRetry()) co_return std::nullopt;
      if (const auto* blocked = std::get_if<routing_state::RouteSelectionQuotaBlocked>(&ctx->selected)) {
        // A request that already holds the upstream quota answer keeps that
        // actionable body; a cold all-blocked cohort reports the routing error.
        if (ctx->lastResponse.has_value()) {
          rt::Promise<rt::Response> exhausting(ctx->exhaustedCodexDispatchResponse());
          rt::Response response = co_await exhausting.wait();
          co_return response;
        }
        rt::Promise<rt::Response> exhausting(ctx->exhaustedQuotaBlockedCohort(blocked->retryAtMs));
        rt::Response response = co_await exhausting.wait();
        co_return response;
      }
      const auto* eligible = std::get_if<routing_state::RouteSelectionEligible>(&ctx->selected);
      if (eligible == nullptr) {
        rt::Promise<rt::Response> exhausting(ctx->exhaustedCodexDispatchResponse());
        rt::Response response = co_await exhausting.wait();
        co_return response;
      }
      std::vector<auth::CodexDispatchAccountEntry> accountEntries;
      for (const RoutingAccount& routing : eligible->accounts) {
        auth::CodexDispatchAccountEntry entry;
        static_cast<auth::CodexAuthPoolEntry&>(entry) = ctx->poolEntry;
        entry.auth = routing.auth;
        entry.routing = routing;
        accountEntries.push_back(std::move(entry));
      }
      ctx->accountEntries = std::move(accountEntries);
      co_return std::nullopt;
    }
  }

  void installCodexResponseRunners(std::shared_ptr<CodexResponseContext> ctx) {
    const std::weak_ptr<CodexResponseContext> weak = ctx;
    ctx->responseIsCodexAuthFailure = [](CodexAuthState auth, rt::Response response) { return responseIsCodexAuthFailure(auth, response); };
    ctx->finalizePostResetRetry = [weak](CodexBankedResetCandidate retryCandidate, rt::Response retried, std::optional<rt::Response> normalResponse) {
      return finalizePostResetRetry(lockContext(weak), std::move(retryCandidate), std::move(retried), std::move(normalResponse));
    };
    ctx->runPostResetRetry = [weak](EvaluatedBlockedReset evaluated, std::optional<rt::Response> normalResponse) {
      return runPostResetRetry(lockContext(weak), std::move(evaluated), std::move(normalResponse));
    };
    ctx->redeemAndRetryOnce = [weak](rt::Response normalResponse) { return redeemAndRetryOnce(lockContext(weak), std::move(normalResponse)); };
    ctx->recoverBlockedReset = [weak]() { return recoverBlockedReset(lockContext(weak)); };
    ctx->exhaustedQuotaBlockedCohort = [weak](std::optional<double> retryAtMs) { return exhaustedQuotaBlockedCohort(lockContext(weak), retryAtMs); };
    ctx->claimAttemptRouting = [weak](RoutingAccount routing) { return claimAttemptRouting(lockContext(weak), std::move(routing)); };
    ctx->recoverCodexAuthFailure = [weak](auth::CodexDispatchAccountEntry accountEntry, std::shared_ptr<CodexCredentialAttempt> attempt, rt::Response response) {
      return recoverCodexAuthFailure(lockContext(weak), std::move(accountEntry), std::move(attempt), std::move(response));
    };
    ctx->classifyAccountAttemptResponse = [weak](auth::CodexDispatchAccountEntry accountEntry, CodexCredentialAttempt attempt, rt::Response response) {
      return classifyAccountAttemptResponse(lockContext(weak), std::move(accountEntry), std::move(attempt), std::move(response));
    };
    ctx->absorbSiblingTransportFailure = [weak](CodexError error, RoutingAccount routing) {
      return absorbSiblingTransportFailure(lockContext(weak), std::move(error), std::move(routing));
    };
    ctx->handleAccountAttemptFailure = [weak](rt::Reason error, RoutingAccount routing) {
      return handleAccountAttemptFailure(lockContext(weak), std::move(error), std::move(routing));
    };
    ctx->attemptCodexAccount = [weak](auth::CodexDispatchAccountEntry accountEntry) { return attemptCodexAccount(lockContext(weak), std::move(accountEntry)); };
    ctx->dispatchEligibleAccounts = [weak]() { return dispatchEligibleAccounts(lockContext(weak)); };
    ctx->terminalTransportOutcome = [weak]() { return terminalTransportOutcome(lockContext(weak)); };
    ctx->redeemOrReportQuotaBlocked = [weak]() { return redeemOrReportQuotaBlocked(lockContext(weak)); };
    ctx->prepareTwoSecondRetry = [weak](CodexRetryCandidate candidate) { return prepareTwoSecondRetry(lockContext(weak), std::move(candidate)); };
    ctx->recoverTwoSecondRetryAuthFailure = [weak](CodexRetryCandidate candidate, std::shared_ptr<CodexCredentialAttempt> attempt, rt::Response response) {
      return recoverTwoSecondRetryAuthFailure(lockContext(weak), std::move(candidate), std::move(attempt), std::move(response));
    };
    ctx->classifyTwoSecondRetryResponse = [weak](CodexRetryCandidate candidate, CodexCredentialAttempt attempt, rt::Response response) {
      return classifyTwoSecondRetryResponse(lockContext(weak), std::move(candidate), std::move(attempt), std::move(response));
    };
    ctx->runTwoSecondRetryCandidate = [weak](CodexRetryCandidate candidate) { return runTwoSecondRetryCandidate(lockContext(weak), std::move(candidate)); };
    ctx->exhaustedCodexDispatchResponse = [weak]() { return exhaustedCodexDispatchResponse(lockContext(weak)); };
    ctx->runPendingShortRetry = [weak]() { return runPendingShortRetry(lockContext(weak)); };
    ctx->reselectedActiveIdentity = [weak]() { return reselectedActiveIdentity(*lockContext(weak)); };
    ctx->restorePreservedShortRetry = [weak]() { return restorePreservedShortRetry(*lockContext(weak)); };
    ctx->advanceActiveReselection = [weak]() { return advanceActiveReselection(lockContext(weak)); };
  }
}
