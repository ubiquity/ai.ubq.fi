// Codex Responses execution helpers, split out of src/codex.ts.
#include "uos/codex/responses_operations.hpp"

#include "uos/api_key_policy.hpp"
#include "uos/codex/account_routing.hpp"
#include "uos/codex/auth_refresh.hpp"
#include "uos/codex/banked_reset_claim.hpp"
#include "uos/codex/banked_reset_pool.hpp"
#include "uos/codex/banked_reset_provider.hpp"
#include "uos/codex/banked_reset_submission.hpp"
#include "uos/codex/capacity_routing.hpp"
#include "uos/codex/overage_settings.hpp"
#include "uos/codex/rate_limit_429.hpp"
#include "uos/codex/responses_runners.hpp"
#include "uos/codex/routing_serial.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/sentinel/upstream_capture.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace uos::codex::responses_operations {
  namespace {
    using auth::CodexError;
    using dispatch::CodexBankedResetCandidate;
    using responses_state::BankedResetFence;
    using responses_state::CodexCredentialAttempt;
    using responses_state::CodexResetCohortSnapshot;
    using responses_state::CodexResponseContext;
    using responses_state::CodexRetryCandidate;
    using responses_state::EvaluatedBlockedReset;
    using responses_state::OverageCohortMember;
    using responses_state::PendingOverageFallback;
    using responses_state::PreexistingBankedReset;
    using routing_state::CodexActiveAccountSnapshot;
    using routing_state::CodexBlockedRoutingAccount;
    using routing_state::RouteSelection;
    using routing_state::RouteSelectionQuotaBlocked;
    using routing_state::RoutingAccount;
    using ::uos::types::CodexAuthState;
    using Context = std::shared_ptr<CodexResponseContext>;
    using Operations = responses_state::CodexResponseOperations;

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

    // `reason instanceof Error && reason.name === name`
    bool errorNamed(const rt::Reason& reason, std::string_view name) {
      if (!rt::isError(reason)) return false;
      const std::optional<std::string> errorName = rt::errorName(reason);
      return errorName.has_value() && *errorName == name;
    }

    // The installed closures hold the context weakly (it owns them); a running operation holds it strongly.
    Context lockContext(const std::weak_ptr<CodexResponseContext>& weak) {
      Context ctx = weak.lock();
      if (!ctx) throw std::logic_error("Codex response operation called after its request context ended");
      return ctx;
    }

    json::value stringOrNull(const std::optional<std::string>& value) { return value.has_value() ? json::value(*value) : json::value(nullptr); }
    json::value numberOrNull(const std::optional<double>& value) { return value.has_value() ? json::value(*value) : json::value(nullptr); }
    json::value nullableOrNull(const rt::Nullable<std::string>& value) { return value.isNullish() ? json::value(nullptr) : json::value(*value); }

    // `{ ...poolEntry, auth, routing }`
    auth::CodexAuthAccountEntry accountEntryOf(const auth::CodexAuthPoolEntry& poolEntry, const CodexAuthState& auth,
                                               const std::optional<RoutingAccount>& routing) {
      return auth::CodexAuthAccountEntry{poolEntry, auth, routing};
    }

    Operations::CaptureBankedResetCandidateDisposition dispositionOf(const rate_limit_429::Codex429Classification& classification) {
      return Operations::CaptureBankedResetCandidateDisposition{.usageLimitReached = classification.usageLimitReached,
                                                                .retryAtMs = classification.retryAtMs,
                                                                .resetDeadlineIsStable = classification.resetDeadlineIsStable};
    }

    bool hasExhaustedBlockedCohort(const RouteSelection& pool) {
      const auto* blocked = std::get_if<routing_state::RouteSelectionQuotaBlocked>(&pool);
      return blocked != nullptr && blocked->fullCohortExhausted && !blocked->blockedAccounts.empty();
    }

    /** Outcomes whose reason proves no banked reset could be redeemed this episode. */
    constexpr std::string_view MIXED_UNAVAILABLE = "quota_and_invalid_credentials";
    class OverageCohortChanged : public dispatch::CodexActiveAccountFenceError {};
    constexpr std::array<std::string_view, 11> OVERAGE_FALLBACK_REASONS{
      "no_eligible_credit",
      "inventory_empty",
      "inventory_no_eligible_codex_credit",
      "inventory_response_invalid_or_expired",
      "inventory_unavailable",
      "account_day_limit_reached",
      "usage_explicitly_disabled",
      MIXED_UNAVAILABLE,
      // Shadow mode observes a would-spend decision but never consumes its credit.
      // It therefore proves the reset is unavailable for this episode and may use
      // the same single bounded overage probe as any other no-redemption outcome.
      "shadow_selected",
      "already_would_spend_once",
      // A stale local claim was never submitted to the provider. Reconciliation
      // cannot advance it, so it proves no external reset was consumed.
      "unsubmitted_transaction",
    };

    bool permitsOverageFallback(std::string_view reason) {
      return std::find(OVERAGE_FALLBACK_REASONS.begin(), OVERAGE_FALLBACK_REASONS.end(), reason) != OVERAGE_FALLBACK_REASONS.end();
    }

    // The skipped slots are the actual selected cohort when no account routed.
    // Capture current credential versions for invalid members as well as quota windows.
    rt::Task<std::optional<std::vector<OverageCohortMember>>> overageCohort(const RouteSelectionQuotaBlocked& quota) {
      if (!quota.poolSnapshotJson) co_return std::nullopt;
      const auto pool = auth::parseCodexAuthPool(rt::jsonParse(*quota.poolSnapshotJson));
      if (!pool) co_return std::nullopt;
      std::vector<OverageCohortMember> cohort;
      for (const double skipped : quota.skippedSlots) {
        const double slot = skipped - 1;
        const auto* current = rt::at(pool->accounts, slot);
        if (!current) co_return std::nullopt;
        const auto blocked = std::find_if(quota.blockedAccounts.begin(), quota.blockedAccounts.end(),
                                          [slot](const CodexBlockedRoutingAccount& account) { return account.slot == slot; });
        if (blocked == quota.blockedAccounts.end()) {
          auto version = co_await capacity_routing::codexCredentialVersion(*current);
          cohort.push_back({slot, std::move(version), std::nullopt});
        } else {
          cohort.push_back({slot, blocked->credentialVersion, blocked->quotaResetAtMs});
        }
      }
      co_return cohort;
    }

    // Prefer the account named by the evaluator, then the active account, then pool order.
    rt::Task<std::optional<PendingOverageFallback>> planOverageFallback(std::string reason, std::optional<double> selectedSlot,
                                                                       const RouteSelectionQuotaBlocked& quota,
                                                                       const std::optional<std::string>& requestedModel) {
      if (!permitsOverageFallback(reason)) co_return std::nullopt;
      const std::optional<routing_state::CodexActiveAccountSelection>& active = quota.activeSnapshot.selection;
      const CodexBlockedRoutingAccount* blocked = nullptr;
      for (const CodexBlockedRoutingAccount& account : quota.blockedAccounts) {
        if (!overage_settings::overageUsageAllowedSync(account.accountIdHash)) continue;
        if (selectedSlot == account.slot) {
          blocked = &account;
          break;
        }
        if (blocked == nullptr || (active && account.accountIdHash == active->account_id_hash && account.slot == active->slot)) blocked = &account;
      }
      if (blocked == nullptr) co_return std::nullopt;
      auto cohort = co_await overageCohort(quota);
      if (!cohort) co_return std::nullopt;
      // Overage is an explicitly enabled serving path, not a single-flight reset
      // recovery probe. Concurrent requests may use the account directly; each one
      // still performs exactly one bounded transport attempt.
      CodexBlockedRoutingAccount routing = *blocked;
      routing.probeRequired = false;
      routing.probeCircuit = nullptr;
      routing.overageFallback = true;
      routing.requestedModel = rt::orNull(requestedModel);
      co_return PendingOverageFallback{std::move(reason), std::move(routing), quota.activeSnapshot, std::move(*cohort)};
    }

    rt::Task<void> ensureOverageAllowed(std::string accountId) {
      bool fenced = false;
      try {
        rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
        const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
        if (!kv) throw dispatch::CodexActiveAccountFenceError();
        const auto hash = co_await utils::sha256Hex(accountId);
        fenced = !(co_await overage_settings::readOverageUsage(kv, hash));
      } catch (...) {
        fenced = true;
      }
      if (fenced) throw dispatch::CodexActiveAccountFenceError();
    }

    // ---- The operations installCodexResponseOperations assigns, in its order. ----

    banked_reset::CodexBankedResetConfig reloadConfiguredBankedResetConfig(const CodexResponseContext& ctx) {
      return ctx.configuredBankedResetConfig.has_value() ? *ctx.configuredBankedResetConfig : banked_reset::loadCodexBankedResetConfig();
    }

    void requestActiveReselection(CodexResponseContext& ctx, const Operations::RequestActiveReselectionOptions& optionsArg) {
      ctx.activeReselectionRequested = true;
      const std::optional<CodexRetryCandidate> candidate = ctx.retryState.candidate;
      ctx.retryState.candidate = std::nullopt;
      const std::optional<RoutingAccount>& routing = optionsArg.routing;
      if (optionsArg.preserveRetry == true && candidate.has_value() && routing.has_value()) {
        const std::optional<double> activeGeneration = routing->activeGeneration;
        if (activeGeneration.has_value() && rt::Number::isSafeInteger(*activeGeneration)) {
          ctx.preservedShortRetryState.current = responses_state::CodexResponseState::PreservedShortRetryState::Current{
            .candidate = *candidate, .accountIdHash = routing->accountIdHash, .activeGeneration = *activeGeneration};
          return;
        }
      }
      ctx.preservedShortRetryState.current = std::nullopt;
    }

    void noteCodexAuthFailure(CodexResponseContext& ctx, const rt::Reason& error) {
      if (!ctx.authWarning.has_value()) {
        const std::optional<std::string> warning = auth::codexAuthWarningForError(error);
        ctx.authWarning = warning.has_value() ? *warning : std::string(auth::CODEX_AUTH_REAUTH_WARNING);
      }
      const CodexError* codexError = auth::codexErrorOf(error);
      if (codexError != nullptr && codexError->status == 401 && !ctx.authFailureState.error.has_value()) ctx.authFailureState.error = *codexError;
    }

    rt::Response decorateAuthWarning(const CodexResponseContext& ctx, const rt::Response& response) {
      return rt::truthy(ctx.authWarning) ? auth::withCodexAuthWarning(response, *ctx.authWarning) : response;
    }

    rt::Task<RouteSelection> selectCurrentPool(Context ctx) {
      rt::Promise<auth::CodexAuthPoolEntry> loading(auth::getAuthPoolEntry(true, true));
      const auth::CodexAuthPoolEntry currentPoolEntry = co_await loading.wait();
      rt::Promise<RouteSelection> selecting(
        account_routing::selectCodexRoutingAccountsStrong(currentPoolEntry.pool, currentPoolEntry.pool.accounts, rt::dateNow(), ctx->requestedModel));
      RouteSelection currentSelection = co_await selecting.wait();
      co_return currentSelection;
    }

    rt::Task<bool> freshFullCohortExhaustedProof(Context ctx) {
      try {
        rt::Task<RouteSelection> selecting = selectCurrentPool(ctx);
        const RouteSelection currentSelection = co_await std::move(selecting);
        const auto* blocked = std::get_if<routing_state::RouteSelectionQuotaBlocked>(&currentSelection);
        co_return blocked != nullptr && blocked->fullCohortExhausted;
      } catch (...) {
      }
      co_return false;
    }

    rt::Task<bool> freshFullCohortUnavailableProof(Context ctx) {
      try {
        rt::Task<RouteSelection> selecting = selectCurrentPool(ctx);
        const RouteSelection currentSelection = co_await std::move(selecting);
        if (const auto* blocked = std::get_if<routing_state::RouteSelectionQuotaBlocked>(&currentSelection)) co_return blocked->fullCohortUnavailable == true;
        if (const auto* invalid = std::get_if<routing_state::RouteSelectionCredentialsInvalid>(&currentSelection)) co_return invalid->fullCohortUnavailable == true;
        co_return false;
      } catch (...) {
      }
      co_return false;
    }

    rt::Response subscriptionsUnavailableResponse() {
      return dispatch::routingErrorResponse(503, "No Codex subscription can serve the requested model: quota is exhausted or current credentials are invalid.",
                                            std::string(auth::CODEX_SUBSCRIPTIONS_UNAVAILABLE_ERROR_CODE));
    }

    // `retryAtMs: number | null = null`
    rt::Task<rt::Response> quotaBlockedOrRetryableResponse(Context ctx, std::optional<double> retryAtMs) {
      rt::Promise<bool> exhausted(ctx->freshFullCohortExhaustedProof());
      const bool fullCohortExhausted = co_await exhausted.wait();
      if (fullCohortExhausted) {
        co_return dispatch::routingErrorResponse(429, "All configured Codex accounts are quota-blocked; retry after their next reset.",
                                                 std::string(auth::CODEX_QUOTA_BLOCKED_ERROR_CODE), retryAtMs);
      }
      rt::Promise<bool> unavailable(ctx->freshFullCohortUnavailableProof());
      const bool fullCohortUnavailable = co_await unavailable.wait();
      if (fullCohortUnavailable) co_return subscriptionsUnavailableResponse();
      co_return dispatch::routingErrorResponse(429, "Codex capacity is temporarily unavailable; retry the request.", "codex_capacity_unavailable", retryAtMs);
    }

    rt::Response authFailureResponse(CodexResponseContext& ctx, const std::optional<CodexError>& error) {
      const rt::Response response =
        dispatch::routingErrorResponse(401, error.has_value() ? error->message : std::string(auth::CODEX_AUTH_REAUTH_MESSAGE),
                                       error.has_value() && error->code == "refresh_token_reused" ? error->code : std::string("codex_auth_invalid"));
      return ctx.decorateAuthWarning(response);
    }

    rt::Task<void> ensureActiveRoutingAdmissionCurrent(RoutingAccount routing) {
      if (!routing.activeGeneration.has_value() || !rt::Number::isSafeInteger(*routing.activeGeneration) || *routing.activeGeneration < 1 ||
          !routing.activePoolVersionstamp.has_value() || routing.activePoolVersionstamp->empty()) {
        throw dispatch::CodexActiveAccountFenceError();
      }
      bool fenced = false;
      try {
        rt::Promise<bool> refreshing(account_routing::refreshCodexActiveAccountAdmission(routing));
        const bool admitted = co_await refreshing.wait();
        if (!admitted) throw dispatch::CodexActiveAccountFenceError();
      } catch (...) {
        fenced = true;
      }
      if (fenced) throw dispatch::CodexActiveAccountFenceError();
    }

    rt::Task<std::optional<CodexBankedResetCandidate>> refreshBankedResetCandidate(CodexBankedResetCandidate candidate) {
      try {
        // A banked reset is stricter than normal routing: force-read the auth
        // pool immediately before the durable side-effect path. A rotated or
        // reordered account is not eligible for an old candidate.
        rt::Promise<auth::CodexAuthPoolEntry> loading(auth::getAuthPoolEntry(true, true));
        const auth::CodexAuthPoolEntry currentPoolEntry = co_await loading.wait();
        const CodexAuthState* currentAuth = rt::at(currentPoolEntry.pool.accounts, candidate.routing.slot);
        if (currentAuth == nullptr) co_return std::nullopt;
        if (currentAuth->account_id != candidate.auth.account_id || !auth_refresh::sameCodexCredentials(*currentAuth, candidate.auth)) co_return std::nullopt;
        CodexBankedResetCandidate refreshed = candidate;
        refreshed.auth = *currentAuth;
        refreshed.accountEntry = accountEntryOf(currentPoolEntry, *currentAuth, candidate.routing);
        co_return refreshed;
      } catch (...) {
      }
      co_return std::nullopt;
    }

    CodexBankedResetCandidate withResetRecoveryProbe(const CodexBankedResetCandidate& candidate, const RoutingAccount& probe) {
      RoutingAccount routing = probe;
      routing.auth = candidate.auth;
      CodexBankedResetCandidate next = candidate;
      next.routing = routing;
      next.accountEntry.auth = candidate.auth;
      next.accountEntry.routing = routing;
      return next;
    }

    BankedResetFence authPoolFence(const CodexBankedResetCandidate& candidate) {
      const double slot = candidate.routing.slot;
      const CodexAuthState expected = candidate.auth;
      return BankedResetFence{auth::CODEX_AUTH_POOL_KV_KEY, [slot, expected](const json::value& value) {
                                const std::optional<::uos::types::CodexAuthPoolState> pool = auth::parseCodexAuthPool(value);
                                const CodexAuthState* current = pool.has_value() ? rt::element(pool->accounts, slot) : nullptr;
                                return current != nullptr && current->account_id == expected.account_id && auth_refresh::sameCodexCredentials(*current, expected);
                              }};
    }

    BankedResetFence activeSelectionFence(const CodexBankedResetCandidate& candidate) {
      const RoutingAccount routing = candidate.routing;
      return BankedResetFence{routing_state::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, [routing](const json::value& value) {
                                return routing.activeGeneration.has_value() &&
                                       routing_state::isCodexActiveAccountSelectionCurrent(value, routing, *routing.activeGeneration);
                              }};
    }

    BankedResetFence activeSnapshotFence(const CodexActiveAccountSnapshot& snapshot) {
      return BankedResetFence{routing_state::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY,
                              [snapshot](const json::value& value) { return account_routing::isCodexActiveAccountSnapshotCurrent(value, snapshot); }};
    }

    rt::Task<void> ensurePostResetRetryAuthCurrent(CodexBankedResetCandidate candidate) {
      rt::Promise<auth::CodexAuthPoolEntry> loading(auth::getAuthPoolEntry(true, true));
      const auth::CodexAuthPoolEntry currentPoolEntry = co_await loading.wait();
      const CodexAuthState* currentAuth = rt::at(currentPoolEntry.pool.accounts, candidate.routing.slot);
      if (currentAuth == nullptr) throw dispatch::CodexBankedResetRetryFenceError();
      if (currentAuth->account_id != candidate.auth.account_id || !auth_refresh::sameCodexCredentials(*currentAuth, candidate.auth)) {
        throw dispatch::CodexBankedResetRetryFenceError();
      }
    }

    BankedResetFence exactValueFence(const rt::KvKey& key, const std::string& expectedJson) {
      return BankedResetFence{key, [expectedJson](const json::value& value) { return rt::jsonStringify(value) == expectedJson; }};
    }

    std::vector<BankedResetFence> resetFences(const Context& ctx, const CodexBankedResetCandidate& candidate, double routingGeneration,
                                              const CodexActiveAccountSnapshot& originalActive, const CodexResetCohortSnapshot& cohort) {
      const RoutingAccount routing = candidate.routing;
      const double quotaResetAtMs = candidate.quotaResetAtMs;
      std::vector<BankedResetFence> fences;
      fences.push_back(BankedResetFence{routing_state::CODEX_ACCOUNT_ROUTING_KV_KEY, [routing, quotaResetAtMs, routingGeneration](const json::value& value) {
                                          return routing_mutations::isCodexQuotaBlockFenceCurrent(value, routing, quotaResetAtMs, routingGeneration);
                                        }});
      fences.push_back(BankedResetFence{auth::CODEX_AUTH_POOL_KV_KEY,
                                        [ctx, candidate](const json::value& value) { return ctx->authPoolFence(candidate).isCurrent(value); }});
      fences.push_back(ctx->activeSnapshotFence(originalActive));
      fences.push_back(ctx->exactValueFence(auth::CODEX_AUTH_POOL_KV_KEY, cohort.poolJson));
      fences.push_back(ctx->exactValueFence(routing_state::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY, cohort.capacityJson));
      return fences;
    }

    std::vector<BankedResetFence> electedResetFences(const Context& ctx, const CodexBankedResetCandidate& candidate, double routingGeneration) {
      const RoutingAccount routing = candidate.routing;
      const double quotaResetAtMs = candidate.quotaResetAtMs;
      std::vector<BankedResetFence> fences;
      fences.push_back(BankedResetFence{routing_state::CODEX_ACCOUNT_ROUTING_KV_KEY, [routing, quotaResetAtMs, routingGeneration](const json::value& value) {
                                          return routing_mutations::isCodexQuotaBlockFenceCurrent(value, routing, quotaResetAtMs, routingGeneration);
                                        }});
      fences.push_back(BankedResetFence{auth::CODEX_AUTH_POOL_KV_KEY,
                                        [ctx, candidate](const json::value& value) { return ctx->authPoolFence(candidate).isCurrent(value); }});
      fences.push_back(ctx->activeSelectionFence(candidate));
      return fences;
    }

    responses_state::BankedResetInput resetInput(const Context& ctx, const CodexBankedResetCandidate& candidate, double routingGeneration,
                                                 const CodexActiveAccountSnapshot& originalActive, const CodexResetCohortSnapshot& cohort) {
      responses_state::BankedResetInput input;
      input.accountId = candidate.auth.account_id;
      input.credentialVersion = candidate.routing.credentialVersion;
      input.quotaResetAtMs = candidate.quotaResetAtMs;
      input.routingGeneration = routingGeneration;
      input.fences = ctx->resetFences(candidate, routingGeneration, originalActive, cohort);
      input.requestId = ctx->options.requestId;
      input.signal = ctx->options.signal;
      return input;
    }

    banked_reset::CodexBankedResetDependencies resetDependenciesForCandidate(const CodexResponseContext& ctx, const CodexBankedResetCandidate& candidate) {
      if (ctx.configuredBankedReset.has_value() && ctx.configuredBankedReset->provider) return ctx.bankedResetDependencies;
      try {
        banked_reset::CodexBankedResetDependencies dependencies = ctx.bankedResetDependencies;
        banked_reset_provider::UpstreamCodexUsageResetProviderOptions providerOptions;
        providerOptions.codexBaseUrl = ::uos::config::config.codexBaseUrl;
        providerOptions.accountId = candidate.auth.account_id;
        providerOptions.accessToken = candidate.auth.access_token;
        providerOptions.userAgent = auth::codexUserAgent(ctx.options.clientVersion);
        providerOptions.now = ctx.bankedResetDependencies.now;
        dependencies.provider = banked_reset_provider::createUpstreamCodexUsageResetProvider(providerOptions);
        return dependencies;
      } catch (...) {
        return ctx.bankedResetDependencies;
      }
    }

    // `Promise<"captured" | "ineligible" | "routing_fence_unavailable">`
    rt::Task<std::string> captureBankedResetCandidate(Context ctx, auth::CodexAuthAccountEntry accountEntry, RoutingAccount routing, CodexAuthState auth,
                                                      Operations::CaptureBankedResetCandidateDisposition disposition) {
      // Relative Retry-After delays are valid for ordinary routing, but their
      // Date.now-derived deadline cannot name a durable reset window. A
      // canonical absolute HTTP-date is only a provisional identity; the
      // durable no-revision fence is what permits it to receive a key.
      if (!disposition.usageLimitReached || !disposition.retryAtMs.has_value() || !disposition.resetDeadlineIsStable) co_return "ineligible";
      rt::Promise<std::optional<double>> fencing(routing_mutations::getCodexQuotaBlockFence(routing, *disposition.retryAtMs));
      const std::optional<double> routingGeneration = co_await fencing.wait();
      if (!routingGeneration.has_value()) {
        // A revised quota window invalidates any older observation for this
        // slot. It must not remain selectable just because capture could not
        // establish a fresh fence for the new deadline.
        ctx->bankedResetCandidates.delete_(routing.slot);
        co_return "routing_fence_unavailable";
      }
      CodexBankedResetCandidate candidate{.accountEntry = std::move(accountEntry),
                                          .auth = std::move(auth),
                                          .routing = routing,
                                          .quotaResetAtMs = *disposition.retryAtMs,
                                          .routingGeneration = *routingGeneration};
      ctx->bankedResetCandidates.set(routing.slot, std::move(candidate));
      co_return "captured";
    }

    void logBankedResetEvent(const CodexResponseContext& ctx, const banked_reset::CodexBankedResetEvent& event,
                             const banked_reset::CodexBankedResetTelemetryFields& fields) {
      banked_reset::reportCodexBankedResetEvent(ctx.bankedResetDependencies.telemetry, event, fields);
    }

    rt::Task<void> reportHealthyFallback(Context ctx) {
      using Hash = std::function<rt::Task<std::string>(std::string value)>;
      for (const CodexBankedResetCandidate& candidate : ctx->bankedResetCandidates.values()) {
        try {
          const Hash hash = ctx->bankedResetDependencies.hash ? ctx->bankedResetDependencies.hash : Hash(&::uos::utils::sha256Hex);
          rt::Promise<std::string> hashingAccount(hash(candidate.auth.account_id));
          const std::string accountIdHash = co_await hashingAccount.wait();
          std::string material = "uos_ai";
          material += '\0';
          material += "codex_reset_generation";
          material += '\0';
          material = rt::concat(material, accountIdHash);
          material += '\0';
          material += rt::numberToString(candidate.quotaResetAtMs);
          rt::Promise<std::string> hashingGeneration(hash(material));
          const std::string quotaGeneration = co_await hashingGeneration.wait();
          json::object fields;
          fields["request_id"] = stringOrNull(ctx->options.requestId);
          fields["account_id_hash"] = accountIdHash;
          fields["quota_generation"] = rt::concat("v1:", quotaGeneration);
          fields["routing_generation"] = candidate.routingGeneration;
          ctx->logBankedResetEvent("codex_reset_skipped_healthy_fallback", fields);
        } catch (...) {
          // A telemetry hash failure must not delay a healthy inference response.
        }
      }
    }

    rt::Task<PreexistingBankedReset> preexistingBankedReset(Context ctx, std::vector<CodexBankedResetCandidate> localCandidates,
                                                            CodexActiveAccountSnapshot originalActive, CodexResetCohortSnapshot cohort) {
      std::optional<std::string> definitiveRefusal;
      bool hasUnusedWindow = false;
      for (const CodexBankedResetCandidate& candidate : localCandidates) {
        banked_reset::CodexBankedResetCandidate input = ctx->resetInput(candidate, candidate.routingGeneration, originalActive, cohort);
        banked_reset::CodexBankedResetDependencies dependencies = ctx->resetDependenciesForCandidate(candidate);
        rt::Promise<banked_reset::CodexBankedResetOutcome> reconciling(
          banked_reset_submission::reconcileCodexBankedReset(std::move(input), std::move(dependencies)));
        const banked_reset::CodexBankedResetOutcome reset = co_await reconciling.wait();
        if (reset.kind == "verified") {
          co_return responses_state::PreexistingBankedResetEvaluated{
            .evaluated = EvaluatedBlockedReset{.candidate = candidate, .reset = reset, .staleVerified = false, .originalActive = originalActive}};
        }
        if (reset.reason == "verified_routing_generation_stale") {
          co_return responses_state::PreexistingBankedResetEvaluated{
            .evaluated = EvaluatedBlockedReset{.candidate = candidate, .reset = reset, .staleVerified = true, .originalActive = originalActive}};
        }
        if (reset.reason == "no_existing_transaction") {
          hasUnusedWindow = true;
        } else if (permitsOverageFallback(reset.reason)) {
          definitiveRefusal = reset.reason;
        } else {
          co_return responses_state::PreexistingBankedResetBlocked{.reason = reset.reason};
        }
      }
      if (!hasUnusedWindow && definitiveRefusal) co_return responses_state::PreexistingBankedResetBlocked{.reason = *definitiveRefusal};
      co_return responses_state::PreexistingBankedResetNone{};
    }

    std::optional<EvaluatedBlockedReset> selectedBankedResetCandidate(const banked_reset::CodexBankedResetPoolOutcome& evaluated,
                                                                      const std::vector<CodexBankedResetCandidate>& localCandidates,
                                                                      const CodexActiveAccountSnapshot& originalActive) {
      if (!evaluated.selected.has_value() || !evaluated.reset.has_value()) return std::nullopt;
      const double selectedSlot = evaluated.selected->slot;
      for (const CodexBankedResetCandidate& item : localCandidates) {
        if (item.routing.slot == selectedSlot) {
          return EvaluatedBlockedReset{.candidate = item, .reset = *evaluated.reset, .staleVerified = false, .originalActive = originalActive};
        }
      }
      return std::nullopt;
    }

    /**
     * A fully used capacity observation is authoritative evidence that the
     * subscription cannot serve the requested class until its reset deadline:
     * materialize the durable class block so a local request can drive the
     * same banked-reset redemption a live 429 would. Without it the pool
     * dead-ends locally and silently burns the account's overage credits.
     */
    // evaluateBlockedCohortBankedReset's `materializeCapacityQuotaBlocks` closure.
    rt::Task<bool> materializeCapacityQuotaBlocks(Context ctx, auth::CodexAuthPoolEntry currentPoolEntry) {
      const double nowMs = rt::dateNow();
      const std::optional<std::string_view> requestedModel = ctx->requestedModel ? std::optional<std::string_view>(*ctx->requestedModel) : std::nullopt;
      rt::Promise<std::vector<routing_state::CodexCapacityRoutingObservation>> loading(
        capacity_routing::loadCodexCapacityRoutingObservations(currentPoolEntry.pool, true));
      const std::vector<routing_state::CodexCapacityRoutingObservation> observations = co_await loading.wait();
      bool wrote = false;
      for (std::size_t index = 0; index < currentPoolEntry.pool.accounts.size(); ++index) {
        const CodexAuthState& auth = currentPoolEntry.pool.accounts[index];
        try {
          rt::Promise<capacity_routing::CodexRoutingAccountIdentity> identifying(capacity_routing::routingAccountIdentity(auth));
          const capacity_routing::CodexRoutingAccountIdentity identity = co_await identifying.wait();
          const routing_state::CodexCapacityRoutingObservation* observation = nullptr;
          for (const routing_state::CodexCapacityRoutingObservation& candidate : observations) {
            if (candidate.account_id_hash == identity.accountIdHash) {
              observation = &candidate;
              break;
            }
          }
          if (observation == nullptr || !authority::native_codex_capacity::capacityExhaustedResetAtMs(*observation, requestedModel, nowMs)) continue;
          RoutingAccount account;
          account.slot = static_cast<double>(index);
          account.accountIdHash = identity.accountIdHash;
          account.credentialVersion = identity.credentialVersion;
          account.quotaHeadroom = 0;
          account.probeRequired = false;
          account.probeGeneration = std::nullopt;
          account.probeToken = std::nullopt;
          account.probeCircuit = nullptr;
          account.requestedModel = rt::orNull(ctx->requestedModel);
          account.auth = auth;
          rt::Promise<bool> reconciling(capacity_routing::reconcileCapacityQuotaBlock(account, *observation, nowMs));
          const bool reconciled = co_await reconciling.wait();
          if (reconciled) wrote = true;
        } catch (...) {
          // One account's failed materialization must not stop the others.
        }
      }
      co_return wrote;
    }

    rt::Task<RouteSelection> selectPoolStrong(Context ctx, auth::CodexAuthPoolEntry currentPoolEntry) {
      rt::Promise<RouteSelection> selecting(
        account_routing::selectCodexRoutingAccountsStrong(currentPoolEntry.pool, currentPoolEntry.pool.accounts, rt::dateNow(), ctx->requestedModel));
      RouteSelection selection = co_await selecting.wait();
      co_return selection;
    }

    CodexBankedResetCandidate blockedResetCandidate(const auth::CodexAuthPoolEntry& pool, const CodexBlockedRoutingAccount& blocked) {
      return CodexBankedResetCandidate{
        .accountEntry = accountEntryOf(pool, blocked.auth, blocked),
        .auth = blocked.auth,
        .routing = blocked,
        .quotaResetAtMs = blocked.quotaResetAtMs,
        .routingGeneration = blocked.routingGeneration.value_or(std::numeric_limits<double>::quiet_NaN()),
      };
    }

    rt::Task<std::optional<EvaluatedBlockedReset>> evaluateBlockedCohortBankedReset(Context ctx) {
      // Each evaluation replaces the previous plan; a request can attempt at most
      // one overage fallback.
      ctx->overageFallbackPending = std::nullopt;
      if (ctx->probeUnavailable) co_return std::nullopt;
      std::optional<auth::CodexAuthPoolEntry> loaded;
      try {
        rt::Promise<auth::CodexAuthPoolEntry> loading(auth::getAuthPoolEntry(true, true));
        loaded = co_await loading.wait();
      } catch (...) {
      }
      if (!loaded.has_value()) co_return std::nullopt;
      const auth::CodexAuthPoolEntry currentPoolEntry = *loaded;
      rt::Task<RouteSelection> selecting = selectPoolStrong(ctx, currentPoolEntry);
      RouteSelection routedPool = co_await std::move(selecting);
      if (hasExhaustedBlockedCohort(routedPool)) {
        rt::Promise<bool> materializing(materializeCapacityQuotaBlocks(ctx, currentPoolEntry));
        const bool materialized = co_await materializing.wait();
        if (materialized) {
          rt::Task<RouteSelection> reselecting = selectPoolStrong(ctx, currentPoolEntry);
          routedPool = co_await std::move(reselecting);
        }
      }
      if (std::holds_alternative<routing_state::RouteSelectionRoutingUnavailable>(routedPool)) {
        json::object fields;
        fields["request_id"] = stringOrNull(ctx->options.requestId);
        fields["require_full_pool"] = "true";
        fields["outcome"] = "routing_unavailable";
        fields["reason"] = "strong_routing_read_failed";
        dispatch::logCodexRouting("codex_banked_reset_preflight", fields);
        co_return std::nullopt;
      }
      // Every current account must be authoritatively exhausted. A held recovery
      // lease, an invalid credential, or unclassified state is not quota
      // exhaustion and must not spend a credit.
      const auto* quotaBlocked = std::get_if<routing_state::RouteSelectionQuotaBlocked>(&routedPool);
      if (quotaBlocked == nullptr) co_return std::nullopt;
      if (!quotaBlocked->fullCohortExhausted) {
        if (quotaBlocked->fullCohortUnavailable == true)
          ctx->overageFallbackPending = co_await planOverageFallback(std::string(MIXED_UNAVAILABLE), std::nullopt, *quotaBlocked, ctx->requestedModel);
        co_return std::nullopt;
      }
      std::vector<CodexBlockedRoutingAccount> blockedAccounts = quotaBlocked->blockedAccounts;
      if (blockedAccounts.empty()) {
        rt::Promise<bool> materializing(materializeCapacityQuotaBlocks(ctx, currentPoolEntry));
        co_await materializing.wait();
        rt::Task<RouteSelection> reselecting = selectPoolStrong(ctx, currentPoolEntry);
        const RouteSelection refreshed = co_await std::move(reselecting);
        if (hasExhaustedBlockedCohort(refreshed)) {
          routedPool = refreshed;
          blockedAccounts = std::get<routing_state::RouteSelectionQuotaBlocked>(refreshed).blockedAccounts;
        } else {
          json::object fields;
          fields["request_id"] = stringOrNull(ctx->options.requestId);
          fields["require_full_pool"] = "true";
          fields["outcome"] = "skipped";
          fields["reason"] = "no_blocked_identity";
          fields["candidate_count"] = 0;
          fields["selected_slot"] = nullptr;
          dispatch::logCodexRouting("codex_banked_reset_preflight", fields);
          co_return std::nullopt;
        }
      }
      const routing_state::RouteSelectionQuotaBlocked& blockedPool = std::get<routing_state::RouteSelectionQuotaBlocked>(routedPool);
      if (blockedAccounts.size() != blockedPool.skippedSlots.size()) co_return std::nullopt;
      if (!blockedPool.poolSnapshotJson.has_value() || !blockedPool.capacitySnapshotJson.has_value()) co_return std::nullopt;
      const CodexResetCohortSnapshot cohort{.poolJson = *blockedPool.poolSnapshotJson, .capacityJson = *blockedPool.capacitySnapshotJson};
      const CodexActiveAccountSnapshot originalActive = blockedPool.activeSnapshot;
      // Consider every blocked account's credit inventory. The original active
      // snapshot fences inventory and consume; the selected account is
      // elected only after its reset is verified.
      std::vector<CodexBankedResetCandidate> localCandidates;
      for (const CodexBlockedRoutingAccount& blocked : blockedAccounts) {
        localCandidates.push_back(blockedResetCandidate(currentPoolEntry, blocked));
      }

      rt::Promise<PreexistingBankedReset> checking(ctx->preexistingBankedReset(localCandidates, originalActive, cohort));
      const PreexistingBankedReset preexisting = co_await checking.wait();
      if (const auto* refused = std::get_if<responses_state::PreexistingBankedResetBlocked>(&preexisting)) {
        ctx->overageFallbackPending =
          co_await planOverageFallback(refused->reason, std::nullopt, blockedPool, ctx->requestedModel);
        co_return std::nullopt;
      }
      if (const auto* evaluated = std::get_if<responses_state::PreexistingBankedResetEvaluated>(&preexisting)) co_return evaluated->evaluated;

      std::vector<banked_reset::CodexBankedResetPoolCandidate> poolCandidates;
      for (const CodexBankedResetCandidate& candidate : localCandidates) {
        banked_reset::CodexBankedResetPoolCandidate poolCandidate;
        poolCandidate.slot = candidate.routing.slot;
        poolCandidate.candidate = ctx->resetInput(candidate, candidate.routingGeneration, originalActive, cohort);
        poolCandidate.provider = ctx->resetDependenciesForCandidate(candidate).provider;
        poolCandidates.push_back(std::move(poolCandidate));
      }
      rt::Promise<banked_reset::CodexBankedResetPoolOutcome> evaluating(
        banked_reset_pool::evaluateCodexBankedResetPool(poolCandidates, ctx->bankedResetDependencies));
      const banked_reset::CodexBankedResetPoolOutcome evaluated = co_await evaluating.wait();
      json::object fields;
      fields["request_id"] = stringOrNull(ctx->options.requestId);
      fields["require_full_pool"] = "true";
      fields["outcome"] = evaluated.kind;
      fields["reason"] = evaluated.reason;
      fields["candidate_count"] = static_cast<double>(poolCandidates.size());
      fields["selected_slot"] = evaluated.selected.has_value() ? json::value(evaluated.selected->slot + 1) : json::value(nullptr);
      dispatch::logCodexRouting("codex_banked_reset_preflight", fields);
      ctx->overageFallbackPending =
        co_await planOverageFallback(evaluated.allCandidatesExplicitlyDisabled ? "usage_explicitly_disabled" : evaluated.reason,
                                     evaluated.selected.has_value() ? std::optional<double>(evaluated.selected->slot) : std::nullopt, blockedPool,
                                     ctx->requestedModel);
      if (ctx->overageFallbackPending) co_return std::nullopt;
      co_return ctx->selectedBankedResetCandidate(evaluated, localCandidates, originalActive);
    }

    rt::Task<void> ensureOverageAdmissionCurrent(Context ctx, PendingOverageFallback pending, RoutingAccount routing) {
      try {
        const auto currentPool = co_await auth::getAuthPoolEntry(true, true);
        const RouteSelection selection = co_await selectPoolStrong(ctx, currentPool);
        const auto* quota = std::get_if<routing_state::RouteSelectionQuotaBlocked>(&selection);
        const bool mixed = pending.reason == MIXED_UNAVAILABLE;
        if (!quota || quota->fullCohortUnavailable != true || quota->fullCohortExhausted == mixed ||
            !quota->poolSnapshotJson || !quota->capacitySnapshotJson) throw OverageCohortChanged();
        const auto currentCohort = co_await overageCohort(*quota);
        if (!currentCohort || *currentCohort != pending.cohort) throw OverageCohortChanged();
        const auto& priorActive = pending.activeSnapshot.selection;
        const auto& currentActive = quota->activeSnapshot.selection;
        if (priorActive.has_value() != currentActive.has_value() ||
            (priorActive && (priorActive->account_id_hash != currentActive->account_id_hash || priorActive->generation != currentActive->generation))) {
          throw OverageCohortChanged();
        }
        const auto candidate = std::find_if(quota->blockedAccounts.begin(), quota->blockedAccounts.end(), [&](const CodexBlockedRoutingAccount& blocked) {
          return blocked.slot == routing.slot && blocked.accountIdHash == routing.accountIdHash &&
                 blocked.credentialVersion == routing.credentialVersion && blocked.routingGeneration == routing.routingGeneration &&
                 blocked.quotaResetAtMs == pending.routing.quotaResetAtMs && blocked.auth.account_id == routing.auth.account_id &&
                 auth_refresh::sameCodexCredentials(blocked.auth, routing.auth);
        });
        if (candidate == quota->blockedAccounts.end()) throw OverageCohortChanged();

        // A sibling can acquire a reset while inventory was being evaluated.
        // Recheck every current window just before admitting overage.
        const auto& dependencies = ctx->bankedResetDependencies;
        const auto kv = co_await banked_reset_submission::openResetKv(dependencies);
        if (!kv) throw dispatch::CodexActiveAccountFenceError();
        auto checks = kv->atomic();
        auto hash = dependencies.hash;
        if (!hash) hash = [](std::string value) { return utils::sha256Hex(std::move(value)); };
        const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
        const double now = dependencies.now ? dependencies.now() : rt::dateNow();
        for (const auto& blocked : quota->blockedAccounts) {
          const auto resetCandidate = blockedResetCandidate(currentPool, blocked);
          const auto input = ctx->resetInput(resetCandidate, resetCandidate.routingGeneration, quota->activeSnapshot,
                                            CodexResetCohortSnapshot{*quota->poolSnapshotJson, *quota->capacitySnapshotJson});
          const auto reset = co_await banked_reset_claim::makeResetContext(input, hash);
          if (!reset) throw dispatch::CodexActiveAccountFenceError();
          const auto row = co_await kv->get(banked_reset::codexResetRedemptionKey(reset->account.accountIdHash, reset->account.quotaGeneration), strong);
          checks->check(row);
          if (!row.versionstamp) continue;
          const auto record = banked_reset::parseCodexResetRedemptionRecord(row.value.toJson());
          using State = ::uos::types::CodexResetRedemptionState;
          if (!record || !banked_reset_claim::matchesContext(*record, *reset)) throw dispatch::CodexActiveAccountFenceError();
          const bool expiredClaim = record->state == State::claimed && record->lease_expires_at_ms <= now;
          const bool definitiveRefusal = record->state == State::rejected && record->last_error_code && permitsOverageFallback(*record->last_error_code);
          if (!expiredClaim && !definitiveRefusal) throw dispatch::CodexActiveAccountFenceError();
        }
        co_await ensureOverageAllowed(routing.auth.account_id);
        const auto policyRow = co_await kv->get(provider::selection::PROVIDER_SELECTION_KV_KEY, strong);
        const auto policy = provider::selection::normalizeProviderSelection(policyRow.value);
        if (policyRow.versionstamp && !policy) throw dispatch::CodexActiveAccountFenceError();
        std::vector<double> selectedSlots;
        for (std::size_t i = 0; i < currentPool.pool.accounts.size(); ++i) {
          const auto accountHash = co_await provider::selection::codexSubscriptionHash(currentPool.pool.accounts[i].account_id);
          if (provider::selection::isCodexSubscriptionEnabled(accountHash, policy)) selectedSlots.push_back(static_cast<double>(i) + 1);
        }
        if (selectedSlots != quota->skippedSlots) throw OverageCohortChanged();
        const auto rows = co_await routing_serial::readStrongCodexRoutingRows(kv);
        if (!currentPool.entry || currentPool.entry->versionstamp != rows.authEntry.versionstamp ||
            rt::jsonStringify(rows.authEntry.value) != *quota->poolSnapshotJson ||
            rt::jsonStringify(rows.capacityEntry.value) != *quota->capacitySnapshotJson ||
            !account_routing::isCodexActiveAccountSnapshotCurrent(rows.activeEntry.value, quota->activeSnapshot)) throw OverageCohortChanged();
        checks->check(policyRow, rows.activeEntry, rows.authEntry, rows.capacityEntry);
        if (!(co_await checks->commit()).ok) throw OverageCohortChanged();
        co_return;
      } catch (const OverageCohortChanged&) {
        throw;
      } catch (...) {
        throw dispatch::CodexActiveAccountFenceError();
      }
    }

    rt::Task<std::optional<rt::Response>> attemptPlannedOverage(Context ctx, PendingOverageFallback pending) {
      RoutingAccount routing = pending.routing;
      std::optional<rt::Response> upstream;
      rt::Reason failure;
      try {
        co_await ensureOverageAllowed(routing.auth.account_id);
        const auto currentPool = co_await auth::getAuthPoolEntry(true, true);
        const auto* current = rt::at(currentPool.pool.accounts, routing.slot);
        if (!current || current->account_id != routing.auth.account_id || !auth_refresh::sameCodexCredentials(*current, routing.auth)) {
          throw OverageCohortChanged();
        }
        const auto entry = accountEntryOf(currentPool, *current, routing);
        if (auth::needsRefresh(*current)) {
          const auto validated = co_await auth_refresh::getValidAuth(entry);
          if (!auth_refresh::sameCodexCredentials(*current, validated)) {
            routing = co_await routing_mutations::reconcileCodexRoutingAccount(routing, validated);
            const auto refreshedPool = co_await auth::getAuthPoolEntry(true, true);
            const auto refreshed = co_await selectPoolStrong(ctx, refreshedPool);
            const auto* blocked = std::get_if<routing_state::RouteSelectionQuotaBlocked>(&refreshed);
            if (!blocked) throw OverageCohortChanged();
            const auto renewed = std::find_if(blocked->blockedAccounts.begin(), blocked->blockedAccounts.end(), [&](const CodexBlockedRoutingAccount& candidate) {
              return candidate.slot == routing.slot && candidate.accountIdHash == routing.accountIdHash &&
                     candidate.credentialVersion == routing.credentialVersion && candidate.quotaResetAtMs == pending.routing.quotaResetAtMs;
            });
            if (renewed == blocked->blockedAccounts.end()) throw OverageCohortChanged();
            routing.routingGeneration = renewed->routingGeneration;
          }
          routing.auth = validated;
        }
        const auto check = [ctx, pending, routing]() { return ensureOverageAdmissionCurrent(ctx, pending, routing); };
        upstream = co_await ctx->fetchAttempt(entry, routing.auth, routing, "overage_fallback", check, true);
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) {
        if (reasonIs<OverageCohortChanged>(failure)) std::rethrow_exception(failure);
        if (reasonIs<dispatch::CodexActiveAccountFenceError>(failure)) co_return std::nullopt;
        const auto* error = auth::codexErrorOf(failure);
        if (dynamic_cast<const auth::CodexAuthFollowerStaleError*>(error)) dispatch::reportCodexResponseTiming(ctx->options.onFollowerStale);
        if (error && error->status == 401) co_await routing_mutations::markCodexCredentialInvalid(routing);
        std::rethrow_exception(failure);
      }
      const rt::Response response = *upstream;
      if (ctx->lastResponse) dispatch::cancelResponseBody(*ctx->lastResponse);
      if (response.ok()) {
        dispatch::logCodexRouting("codex_overage_served", json::object{
          {"request_id", stringOrNull(ctx->options.requestId)}, {"account_id_hash", routing.accountIdHash},
          {"slot", routing.slot + 1}, {"reason", pending.reason},
        });
      } else if (ctx->responseIsCodexAuthFailure(routing.auth, response)) {
        co_await routing_mutations::markCodexCredentialInvalid(routing);
        ctx->authWarning = std::string(auth::CODEX_AUTH_REAUTH_WARNING);
      } else if (response.status() == 429) {
        const auto disposition = co_await rate_limit_429::markCodexQuotaBlocked(routing, response);
        if (disposition.usageLimitReached && disposition.retryAtMs) {
          const auto authorized = co_await ctx->authorizePaidFallbackForCompleteCapacityEvidence(disposition.response);
          co_return ctx->decorateAuthWarning(authorized);
        }
        co_return ctx->decorateAuthWarning(disposition.response);
      }
      co_return ctx->decorateAuthWarning(response);
    }

    rt::Task<std::optional<rt::Response>> runOverageFallbackAttempt(Context ctx) {
      // Recompute reset eligibility once if the proof changed before transport.
      // A stale plan never spends overage or qualifies for external paid fallback.
      for (bool reselected = false;; reselected = true) {
        auto pending = std::exchange(ctx->overageFallbackPending, std::nullopt);
        if (!pending) {
          if (!reselected) co_return std::nullopt;
          break;
        }
        try {
          auto response = co_await attemptPlannedOverage(ctx, std::move(*pending));
          if (response || !reselected) co_return response;
          break;
        } catch (const OverageCohortChanged&) {
        }
        if (reselected) break;
        const auto reset = co_await ctx->evaluateBlockedCohortBankedReset();
        if (reset) {
          auto response = co_await ctx->runPostResetRetry(*reset, ctx->lastResponse);
          if (response) co_return response;
          break;
        }
      }
      if (ctx->lastResponse) dispatch::cancelResponseBody(*ctx->lastResponse);
      co_return dispatch::routingErrorResponse(429, "Codex capacity changed before dispatch; retry the request.", "codex_capacity_unavailable");
    }

    // fetchAttempt's `async () => { await beforeTransport?.(); if (!activeAdmissionAlreadyFenced) ... }`.
    rt::Task<void> fetchAttemptBeforeTransport(Context ctx, std::function<rt::Task<void>()> beforeTransport, bool activeAdmissionAlreadyFenced,
                                               RoutingAccount routing) {
      if (beforeTransport) {
        rt::Promise<void> preparing(beforeTransport());
        co_await preparing.wait();
      } else {
        co_await rt::microtask();
      }
      if (!activeAdmissionAlreadyFenced) {
        rt::Promise<void> admitting(ctx->ensureActiveRoutingAdmissionCurrent(routing));
        co_await admitting.wait();
      }
    }

    json::object attemptFields(const CodexResponseContext& ctx, const RoutingAccount& routing, const std::string& phase) {
      json::object fields;
      fields["request_id"] = stringOrNull(ctx.options.requestId);
      fields["attempt"] = ctx.attemptNumber;
      fields["slot"] = routing.slot + 1;
      fields["phase"] = phase;
      return fields;
    }

    // `activeAdmissionAlreadyFenced = false`: std::nullopt is the default.
    rt::Task<rt::Response> fetchAttempt(Context ctx, auth::CodexAuthAccountEntry accountEntry, CodexAuthState auth, RoutingAccount routing,
                                        dispatch::CodexAttemptPhase phase, std::function<rt::Task<void>()> beforeTransport,
                                        std::optional<bool> activeAdmissionAlreadyFencedArg) {
      const bool activeAdmissionAlreadyFenced = activeAdmissionAlreadyFencedArg.value_or(false);
      ctx->attemptNumber += 1;
      if (ctx->streamRetryPending && phase == "initial") phase = "stream_retry";
      ctx->streamAttempt.reset();
      ctx->transportState.started = false;
      // Assigned inside the actual onDispatch callback; a holder avoids control
      // flow narrowing `null` to `never` across the opaque transport call.
      const auto upstreamAttempt = std::make_shared<std::optional<sentinel::upstream_capture::SentinelUpstreamAttemptHandle>>();
      rt::Reason failure;
      try {
        rt::Task<void> claiming = ctx->providerDispatch.claim();
        co_await std::move(claiming);
        std::function<rt::Task<void>()> preTransport = [ctx, beforeTransport, activeAdmissionAlreadyFenced, routing]() {
          return fetchAttemptBeforeTransport(ctx, beforeTransport, activeAdmissionAlreadyFenced, routing);
        };
        std::function<void()> onDispatch = [ctx, upstreamAttempt]() {
          ctx->providerDispatch.markTransportStarted();
          ctx->transportState.started = true;
          if (std::exchange(ctx->streamRetryPending, false)) dispatch::reportCodexResponseTiming(ctx->options.streamRetry->onRetry);
          dispatch::reportCodexResponseTiming(ctx->options.timing.has_value() ? ctx->options.timing->onDispatch : std::function<void()>());
          if (ctx->options.sentinelUpstreamRecorder.has_value()) *upstreamAttempt = ctx->options.sentinelUpstreamRecorder->startAttempt("chatgpt_codex");
          else *upstreamAttempt = std::nullopt;
        };
        rt::Promise<rt::Response> fetching(dispatch::fetchCodexResponseWithAuth(auth, ctx->url, ctx->prepared->serializedBody, ctx->baseHeaders, ctx->options.signal,
                                                                                std::move(preTransport), std::move(onDispatch)));
        const rt::Response response = co_await fetching.wait();
        // Wrap before the response WeakMap account/health registration
        // so the wrapper is the canonical identity every consumer observes.
        const rt::Response recordedResponse = upstreamAttempt->has_value() ? (*upstreamAttempt)->wrap(response) : response;
        auth::setCodexResponseAccountTelemetry(recordedResponse, routing.slot + 1, auth.account_id);
        auth::setCodexResponseActiveTelemetry(recordedResponse, routing);
        dispatch::reportCodexResponseTiming(ctx->options.timing.has_value() ? ctx->options.timing->onHeaders : std::function<void()>());
        rt::spawn(dispatch::recordCodexResponseHealth(auth.account_id, recordedResponse, auth));
        json::object fields = attemptFields(*ctx, routing, phase);
        fields["status"] = recordedResponse.status();
        fields["status_class"] = dispatch::codexStatusClass(recordedResponse.status());
        fields["active_generation"] = numberOrNull(routing.activeGeneration);
        fields["active_transition_reason"] = nullableOrNull(routing.activeTransitionReason);
        if (ctx->options.streamRetry && recordedResponse.ok()) {
          ctx->streamAttempt = responses_state::CodexResponseState::StreamAttempt{
            std::move(fields), auth.account_id, phase != "post_banked_reset" && phase != "overage_fallback"};
        } else {
          dispatch::logCodexRouting("codex_attempt", fields);
        }
        co_return recordedResponse;
      } catch (...) {
        failure = std::current_exception();
      }
      if (upstreamAttempt->has_value()) (*upstreamAttempt)->recordFetchError();
      const std::optional<rt::AbortSignal>& signal = ctx->options.signal;
      const rt::Reason signalReason = signal.has_value() ? signal->reason() : rt::Reason();
      const bool clientCancelled = signal.has_value() && signal->aborted() && !errorNamed(signalReason, "TimeoutError");
      const bool siblingTransportFailure = dispatch::isCodexSiblingTransportFailure(failure);
      if (!reasonIs<dispatch::CodexBankedResetRetryFenceError>(failure) && !reasonIs<dispatch::CodexActiveAccountFenceError>(failure) &&
          !reasonIs<::uos::api_key_policy::ApiKeyQuotaDispatchError>(failure) && !siblingTransportFailure && !clientCancelled) {
        rt::spawn(dispatch::recordCodexThrownHealth(accountEntry.auth.account_id, failure));
      }
      const CodexError* codexError = auth::codexErrorOf(failure);
      json::object fields = attemptFields(*ctx, routing, phase);
      fields["status"] = codexError != nullptr ? json::value(codexError->status) : json::value(nullptr);
      fields["status_class"] = reasonIs<dispatch::CodexBankedResetRetryFenceError>(failure) ? std::string("banked_reset_fenced") : dispatch::codexErrorClass(failure);
      fields["active_generation"] = numberOrNull(routing.activeGeneration);
      fields["active_transition_reason"] = nullableOrNull(routing.activeTransitionReason);
      dispatch::logCodexRouting("codex_attempt", fields);
      std::rethrow_exception(failure);
    }

    json::object tokenRefreshFields(const CodexResponseContext& ctx, const RoutingAccount& routing, const std::string& trigger, const std::string& outcome,
                                    const std::string& statusClass) {
      json::object fields;
      fields["request_id"] = stringOrNull(ctx.options.requestId);
      fields["slot"] = routing.slot + 1;
      fields["trigger"] = trigger;
      fields["outcome"] = outcome;
      fields["status_class"] = statusClass;
      fields["active_generation"] = numberOrNull(routing.activeGeneration);
      fields["active_transition_reason"] = nullableOrNull(routing.activeTransitionReason);
      return fields;
    }

    // `trigger: "401" | "proactive"`
    rt::Task<CodexCredentialAttempt> refreshAfter401(Context ctx, RoutingAccount routing, CodexAuthState auth, std::string trigger) {
      const bool liveFollower = auth::hasLeaderAuthReader();
      if (!liveFollower || trigger == "401") ctx->refreshedSlots.add(routing.slot);
      rt::Reason failure;
      try {
        const auth::CodexAuthAccountEntry current = accountEntryOf(ctx->poolEntry, auth, routing);
        const rt::Promise<CodexAuthState> refreshing = trigger == "proactive" ? rt::Promise<CodexAuthState>(auth_refresh::getValidAuth(current))
                                                                               : rt::Promise<CodexAuthState>(auth_refresh::refreshAuthCoordinated(current));
        rt::Promise<CodexAuthState> awaiting = liveFollower ? refreshing
          : rt::Promise<CodexAuthState>(auth_refresh::awaitWithoutCancellingSharedWork(refreshing, ctx->options.signal));
        const CodexAuthState refreshed = co_await awaiting.wait();
        if (liveFollower && auth_refresh::sameCodexCredentials(auth, refreshed))
          co_return CodexCredentialAttempt{.auth = refreshed, .routing = routing};
        if (liveFollower) ctx->refreshedSlots.add(routing.slot);
        rt::Promise<RoutingAccount> reconciling(routing_mutations::reconcileCodexRoutingAccount(routing, refreshed));
        const RoutingAccount reconciled = co_await reconciling.wait();
        dispatch::logCodexRouting("codex_token_refresh", tokenRefreshFields(*ctx, routing, trigger, "succeeded", "2xx"));
        co_return CodexCredentialAttempt{.auth = refreshed, .routing = reconciled};
      } catch (...) {
        failure = std::current_exception();
      }
      if (dynamic_cast<const auth::CodexAuthFollowerStaleError*>(auth::codexErrorOf(failure)))
        dispatch::reportCodexResponseTiming(ctx->options.onFollowerStale);
      dispatch::logCodexRouting("codex_token_refresh", tokenRefreshFields(*ctx, routing, trigger, "failed", dispatch::codexErrorClass(failure)));
      std::rethrow_exception(failure);
    }

    rt::Task<rt::Response> classify429(Context ctx, auth::CodexAuthAccountEntry accountEntry, RoutingAccount routing, CodexAuthState auth, rt::Response response) {
      rt::Promise<rate_limit_429::Codex429Classification> marking(rate_limit_429::markCodexQuotaBlocked(routing, response));
      const rate_limit_429::Codex429Classification disposition = co_await marking.wait();
      // Ordinary exhaustion is a valid usage-limit classification with a known
      // deadline, relative or absolute. A stable absolute deadline remains
      // required only to mint a banked-reset candidate.
      const bool ordinaryQuotaExhaustion = disposition.usageLimitReached && disposition.retryAtMs.has_value();
      std::string candidateOutcome = "ineligible";
      if (ordinaryQuotaExhaustion) {
        rt::Promise<std::string> capturing(ctx->captureBankedResetCandidate(accountEntry, routing, auth, dispositionOf(disposition)));
        candidateOutcome = co_await capturing.wait();
      } else {
        // A generic 429 is not proof that the account is exhausted. Do not spend
        // a reset based on an older candidate in the same failover pass.
        ctx->bankedResetCandidates.clear();
      }
      json::object fields;
      fields["request_id"] = stringOrNull(ctx->options.requestId);
      fields["slot"] = routing.slot + 1;
      fields["usage_limit_reached"] = disposition.usageLimitReached ? "true" : "false";
      fields["retry_at_ms"] = numberOrNull(disposition.retryAtMs);
      fields["quota_block_source"] = stringOrNull(disposition.quotaBlockSource);
      fields["reset_deadline_is_stable"] = disposition.resetDeadlineIsStable ? "true" : "false";
      fields["reset_deadline_conflict"] = disposition.resetDeadlineConflict ? "true" : "false";
      fields["candidate_outcome"] = candidateOutcome;
      fields["active_generation"] = numberOrNull(routing.activeGeneration);
      fields["active_transition_reason"] = nullableOrNull(routing.activeTransitionReason);
      dispatch::logCodexRouting("codex_quota_classification", fields);
      const double classifiedAtMs = rt::dateNow();
      const double retryAfterDelay = !disposition.retryAtMs.has_value() ? 0 : rt::Math::max(0.0, *disposition.retryAtMs - classifiedAtMs);
      const bool mayRetryWithinBound = !disposition.usageLimitReached || retryAfterDelay <= auth::CODEX_ADDITIONAL_429_RETRY_MAX_DELAY_MS;
      if (mayRetryWithinBound) {
        const bool quotaProbe = disposition.usageLimitReached && disposition.retryAtMs.has_value();
        CodexRetryCandidate candidate{.accountEntry = accountEntry,
                                      .auth = auth,
                                      .routing = routing,
                                      .delayMs = rt::Math::min(retryAfterDelay, auth::CODEX_ADDITIONAL_429_RETRY_MAX_DELAY_MS),
                                      .readyAtMs = disposition.retryAtMs.value_or(classifiedAtMs),
                                      .expiresAtMs = classifiedAtMs + auth::CODEX_ADDITIONAL_429_RETRY_MAX_DELAY_MS};
        candidate.routing.probeRequired = quotaProbe;
        candidate.routing.probeGeneration = std::nullopt;
        candidate.routing.probeToken = std::nullopt;
        candidate.routing.probeCircuit = quotaProbe ? rt::Nullable<routing_state::CodexProbeCircuit>(std::string("quota")) : rt::Nullable<routing_state::CodexProbeCircuit>(nullptr);
        if (!ctx->retryState.candidate.has_value() || candidate.delayMs < ctx->retryState.candidate->delayMs) ctx->retryState.candidate = candidate;
      }
      if (ordinaryQuotaExhaustion) {
        // Authoritative exhaustion is a global transition reason. Its former
        // short retry is preserved across the reselect only when the same active
        // identity and generation are re-admitted.
        Operations::RequestActiveReselectionOptions options;
        options.preserveRetry = true;
        options.routing = routing;
        ctx->requestActiveReselection(options);
      }
      co_return disposition.response;
    }

    rt::Task<rt::Response> authorizePaidFallbackForCompleteCapacityEvidence(Context ctx, rt::Response response) {
      bool fullCohortExhausted = false;
      if (response.status() == 429) {
        rt::Promise<bool> proving(ctx->freshFullCohortExhaustedProof());
        fullCohortExhausted = co_await proving.wait();
      }
      if (fullCohortExhausted) {
        auth::codexRoutingErrors.set(response, std::string(auth::CODEX_QUOTA_BLOCKED_ERROR_CODE));
      } else if (response.status() == 401 || response.status() == 429) {
        rt::Promise<bool> proving(ctx->freshFullCohortUnavailableProof());
        const bool fullCohortUnavailable = co_await proving.wait();
        if (fullCohortUnavailable) {
          if (response.status() == 401) {
            auth::codexRoutingErrors.set(response, std::string(auth::CODEX_SUBSCRIPTIONS_UNAVAILABLE_ERROR_CODE));
            co_return response;
          }
          dispatch::cancelResponseBody(response);
          co_return ctx->decorateAuthWarning(subscriptionsUnavailableResponse());
        }
      }
      co_return response;
    }

    // `record: CodexResetRedemptionRecord | null`
    rt::Task<void> persistVerifiedCapacityReset(Context ctx, CodexBankedResetCandidate candidate, std::optional<::uos::types::CodexResetRedemptionRecord> record) {
      if (!record.has_value() || record->state != ::uos::types::CodexResetRedemptionState::verified || !record->verified_at_ms.has_value()) co_return;
      const double slot = candidate.routing.slot + 1;
      if (slot != 1 && slot != 2) co_return;
      try {
        std::shared_ptr<rt::Kv> kv;
        if (ctx->bankedResetDependencies.kv.has_value() && *ctx->bankedResetDependencies.kv) {
          kv = *ctx->bankedResetDependencies.kv;
        } else {
          rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
          kv = co_await opening.wait();
        }
        if (!kv) co_return;
        ::uos::provider::capacity_events::ProviderCapacityResetEvent event{
          .v = 1, .event_id = record->idempotency_key_hash, .slot = slot, .observed_at_ms = *record->verified_at_ms};
        rt::Promise<bool> recording(::uos::provider::capacity_events::recordProviderCapacityResetEvent(event, std::optional<std::shared_ptr<rt::Kv>>(kv)));
        co_await recording.wait();
        // A verified reset changes the capacity picture; refresh the sample.
        ::uos::provider::capacity_events::triggerProviderCapacitySample();
      } catch (...) {
        // Capacity telemetry is best effort and must never change inference.
      }
    }
  }

  void installCodexResponseOperations(std::shared_ptr<CodexResponseContext> ctx) {
    const std::weak_ptr<CodexResponseContext> weak = ctx;
    ctx->reloadConfiguredBankedResetConfig = [weak]() { return reloadConfiguredBankedResetConfig(*lockContext(weak)); };
    ctx->requestActiveReselection = [weak](Operations::RequestActiveReselectionOptions options) { requestActiveReselection(*lockContext(weak), options); };
    ctx->noteCodexAuthFailure = [weak](rt::Reason error) { noteCodexAuthFailure(*lockContext(weak), error); };
    ctx->decorateAuthWarning = [weak](rt::Response response) { return decorateAuthWarning(*lockContext(weak), response); };
    ctx->freshFullCohortExhaustedProof = [weak]() { return freshFullCohortExhaustedProof(lockContext(weak)); };
    ctx->freshFullCohortUnavailableProof = [weak]() { return freshFullCohortUnavailableProof(lockContext(weak)); };
    ctx->quotaBlockedOrRetryableResponse = [weak](std::optional<double> retryAtMs) { return quotaBlockedOrRetryableResponse(lockContext(weak), retryAtMs); };
    ctx->authFailureResponse = [weak](std::optional<CodexError> error) { return authFailureResponse(*lockContext(weak), error); };
    ctx->ensureActiveRoutingAdmissionCurrent = [](RoutingAccount routing) { return ensureActiveRoutingAdmissionCurrent(std::move(routing)); };
    ctx->refreshBankedResetCandidate = [](CodexBankedResetCandidate candidate) { return refreshBankedResetCandidate(std::move(candidate)); };
    ctx->withResetRecoveryProbe = [](CodexBankedResetCandidate candidate, RoutingAccount probe) { return withResetRecoveryProbe(candidate, probe); };
    ctx->authPoolFence = [](CodexBankedResetCandidate candidate) { return authPoolFence(candidate); };
    ctx->activeSelectionFence = [](CodexBankedResetCandidate candidate) { return activeSelectionFence(candidate); };
    ctx->activeSnapshotFence = [](CodexActiveAccountSnapshot snapshot) { return activeSnapshotFence(snapshot); };
    ctx->ensurePostResetRetryAuthCurrent = [](CodexBankedResetCandidate candidate) { return ensurePostResetRetryAuthCurrent(std::move(candidate)); };
    ctx->exactValueFence = [](rt::KvKey key, std::string expectedJson) { return exactValueFence(key, expectedJson); };
    ctx->resetFences = [weak](CodexBankedResetCandidate candidate, double routingGeneration, CodexActiveAccountSnapshot originalActive,
                              CodexResetCohortSnapshot cohort) { return resetFences(lockContext(weak), candidate, routingGeneration, originalActive, cohort); };
    ctx->electedResetFences = [weak](CodexBankedResetCandidate candidate, double routingGeneration) {
      return electedResetFences(lockContext(weak), candidate, routingGeneration);
    };
    ctx->resetInput = [weak](CodexBankedResetCandidate candidate, double routingGeneration, CodexActiveAccountSnapshot originalActive,
                             CodexResetCohortSnapshot cohort) { return resetInput(lockContext(weak), candidate, routingGeneration, originalActive, cohort); };
    ctx->resetDependenciesForCandidate = [weak](CodexBankedResetCandidate candidate) { return resetDependenciesForCandidate(*lockContext(weak), candidate); };
    ctx->captureBankedResetCandidate = [weak](auth::CodexAuthAccountEntry accountEntry, RoutingAccount routing, CodexAuthState auth,
                                              Operations::CaptureBankedResetCandidateDisposition disposition) {
      return captureBankedResetCandidate(lockContext(weak), std::move(accountEntry), std::move(routing), std::move(auth), disposition);
    };
    ctx->logBankedResetEvent = [weak](banked_reset::CodexBankedResetEvent event, banked_reset::CodexBankedResetTelemetryFields fields) {
      logBankedResetEvent(*lockContext(weak), event, fields);
    };
    ctx->reportHealthyFallback = [weak]() { return reportHealthyFallback(lockContext(weak)); };
    ctx->preexistingBankedReset = [weak](std::vector<CodexBankedResetCandidate> localCandidates, CodexActiveAccountSnapshot originalActive,
                                         CodexResetCohortSnapshot cohort) {
      return preexistingBankedReset(lockContext(weak), std::move(localCandidates), std::move(originalActive), std::move(cohort));
    };
    ctx->selectedBankedResetCandidate = [](banked_reset::CodexBankedResetPoolOutcome evaluated, std::vector<CodexBankedResetCandidate> localCandidates,
                                           CodexActiveAccountSnapshot originalActive) {
      return selectedBankedResetCandidate(evaluated, localCandidates, originalActive);
    };
    ctx->evaluateBlockedCohortBankedReset = [weak]() { return evaluateBlockedCohortBankedReset(lockContext(weak)); };
    ctx->runOverageFallbackAttempt = [weak]() { return runOverageFallbackAttempt(lockContext(weak)); };
    ctx->fetchAttempt = [weak](auth::CodexAuthAccountEntry accountEntry, CodexAuthState auth, RoutingAccount routing, dispatch::CodexAttemptPhase phase,
                               std::function<rt::Task<void>()> beforeTransport, std::optional<bool> activeAdmissionAlreadyFenced) {
      return fetchAttempt(lockContext(weak), std::move(accountEntry), std::move(auth), std::move(routing), std::move(phase), std::move(beforeTransport),
                          activeAdmissionAlreadyFenced);
    };
    ctx->refreshAfter401 = [weak](RoutingAccount routing, CodexAuthState auth, std::string trigger) {
      return refreshAfter401(lockContext(weak), std::move(routing), std::move(auth), std::move(trigger));
    };
    ctx->classify429 = [weak](auth::CodexAuthAccountEntry accountEntry, RoutingAccount routing, CodexAuthState auth, rt::Response response) {
      return classify429(lockContext(weak), std::move(accountEntry), std::move(routing), std::move(auth), std::move(response));
    };
    ctx->authorizePaidFallbackForCompleteCapacityEvidence = [weak](rt::Response response) {
      return authorizePaidFallbackForCompleteCapacityEvidence(lockContext(weak), std::move(response));
    };
    ctx->persistVerifiedCapacityReset = [weak](CodexBankedResetCandidate candidate, std::optional<::uos::types::CodexResetRedemptionRecord> record) {
      return persistVerifiedCapacityReset(lockContext(weak), std::move(candidate), std::move(record));
    };
  }

  rt::Task<std::optional<BlockedSubscriptionAttempt>> recoverBlockedSubscription(BlockedSubscriptionRequest request) {
    const auto ctx = std::make_shared<CodexResponseContext>();
    const std::weak_ptr<CodexResponseContext> weak = ctx;
    ctx->requestedModel = request.model;
    ctx->options.signal = request.signal;
    ctx->bankedResetDependencies.config = banked_reset::loadCodexBankedResetConfig();
    ctx->bankedResetDependencies.reloadConfig = &banked_reset::loadCodexBankedResetConfig;
    ctx->bankedResetDependencies.provider = banked_reset_provider::unavailableCodexUsageResetProvider;
    installCodexResponseOperations(ctx);
    responses_runners::installCodexResponseRunners(ctx);
    ctx->decorateAuthWarning = [](rt::Response response) { return response; };
    ctx->authorizePaidFallbackForCompleteCapacityEvidence = [](rt::Response response) { return rt::fulfilledTask(std::move(response)); };
    // Realtime's caller processes the actual 401/403 with its existing leader handoff.
    ctx->responseIsCodexAuthFailure = [](const CodexAuthState&, const rt::Response&) { return false; };
    const auto dispatched = std::make_shared<std::optional<RoutingAccount>>();
    ctx->fetchAttempt = [weak, request = std::move(request), dispatched](auth::CodexAuthAccountEntry, CodexAuthState auth, RoutingAccount routing,
                          const dispatch::CodexAttemptPhase&, std::function<rt::Task<void>()> beforeTransport, std::optional<bool> alreadyFenced) {
      routing.auth = auth;
      if (request.attemptedAccountIds.has(auth.account_id)) throw dispatch::CodexActiveAccountFenceError();
      const auto check = [weak, beforeTransport = std::move(beforeTransport), alreadyFenced, routing]() {
        return fetchAttemptBeforeTransport(lockContext(weak), beforeTransport, alreadyFenced.value_or(false), routing);
      };
      const auto onDispatch = [dispatched, routing]() { *dispatched = routing; };
      return dispatch::fetchCodexResponseWithAuth(std::move(auth), request.url, request.body, request.headers, request.signal, check, onDispatch);
    };
    std::optional<rt::Response> response;
    rt::Reason failure;
    try {
      response = co_await ctx->recoverBlockedReset();
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) {
      if (*dispatched) {
        co_await routing_mutations::releaseCodexRoutingProbe(**dispatched);
        co_await dispatch::recordCodexThrownHealth((*dispatched)->auth.account_id, failure);
      }
      std::rethrow_exception(failure);
    }
    if (!response || !*dispatched) co_return std::nullopt;
    co_return BlockedSubscriptionAttempt{std::move(**dispatched), std::move(*response)};
  }
}
