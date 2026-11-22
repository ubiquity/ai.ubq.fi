// Codex Responses execution state and operations interface, split out of src/codex.ts.
#include "uos/codex/responses_state.hpp"

#include "uos/codex/account_routing.hpp"
#include "uos/codex/banked_reset_provider.hpp"
#include "uos/codex/overage_settings.hpp"
#include "uos/config.hpp"
#include "uos/rt/clock.hpp"

#include <utility>

namespace uos::codex::responses_state {
  rt::Task<CreateCodexResponseStateResult> createCodexResponseState(std::shared_ptr<const dispatch::PreparedCodexSubscriptionRequest> prepared,
                                                                    dispatch::FetchCodexResponsesOptions options,
                                                                    dispatch::CodexProviderDispatchCoordinator providerDispatch) {
    rt::Promise<void> settling(dispatch::awaitPendingCodexProbeTransitions());
    co_await settling.wait();
    const auto requestedModel = dispatch::requestedCodexModel(prepared->body);
    rt::Promise<auth::CodexAuthPoolEntry> loading(auth::getAuthPoolEntry(true, true));
    const auth::CodexAuthPoolEntry poolEntry = co_await loading.wait();
    // Refresh the per-account overage switch once per request so the synchronous
    // routing evaluation reads a bounded, current value.
    rt::Promise<void> overage(overage_settings::loadOverageUsageSettings(poolEntry.pool));
    co_await overage.wait();
    rt::Promise<routing_state::RouteSelection> selecting(
      account_routing::selectCodexRoutingAccountsStrong(poolEntry.pool, poolEntry.pool.accounts, rt::dateNow(), requestedModel));
    const routing_state::RouteSelection selected = co_await selecting.wait();
    // Current-invalid pools use the same fresh complete proof as mixed pools
    // before they may authorize paid fallback.
    const std::optional<rt::Response> terminalSelection =
      std::holds_alternative<routing_state::RouteSelectionCredentialsInvalid>(selected) ? std::nullopt : dispatch::initialCodexSelectionResponse(selected);
    if (terminalSelection.has_value()) co_return CreateCodexResponseStateResultTerminal{.response = *terminalSelection};
    std::vector<auth::CodexDispatchAccountEntry> accountEntries;
    if (const auto* eligible = std::get_if<routing_state::RouteSelectionEligible>(&selected)) {
      for (const routing_state::RoutingAccount& routing : eligible->accounts) {
        auth::CodexDispatchAccountEntry entry;
        static_cast<auth::CodexAuthPoolEntry&>(entry) = poolEntry;
        entry.auth = routing.auth;
        entry.routing = routing;
        accountEntries.push_back(std::move(entry));
      }
    }
    const std::string url = ::uos::config::config.codexBaseUrl + "/responses";
    rt::Headers baseHeaders{
      {"originator", std::string(auth::CODEX_ORIGINATOR)},
      {"user-agent", auth::codexUserAgent(options.clientVersion)},
      {"Content-Type", "application/json"},
      {"Accept", "text/event-stream"},
      {"conversation_id", prepared->conversationIdentity},
    };
    dispatch::applyNativeSessionHeaders(baseHeaders, prepared->nativeSessionIdentity);
    const std::optional<dispatch::CodexBankedResetOptions> configuredBankedReset = options.bankedReset;
    const std::optional<banked_reset::CodexBankedResetConfig> configuredBankedResetConfig =
      configuredBankedReset.has_value() ? configuredBankedReset->config : std::nullopt;
    banked_reset::CodexBankedResetDependencies bankedResetDependencies;
    bankedResetDependencies.config = configuredBankedResetConfig.has_value() ? *configuredBankedResetConfig : banked_reset::loadCodexBankedResetConfig();
    if (configuredBankedReset.has_value() && configuredBankedReset->reloadConfig) {
      bankedResetDependencies.reloadConfig = configuredBankedReset->reloadConfig;
    } else if (configuredBankedResetConfig.has_value()) {
      const banked_reset::CodexBankedResetConfig fixedConfig = *configuredBankedResetConfig;
      bankedResetDependencies.reloadConfig = [fixedConfig]() { return fixedConfig; };
    } else {
      bankedResetDependencies.reloadConfig = &banked_reset::loadCodexBankedResetConfig;
    }
    bankedResetDependencies.provider = configuredBankedReset.has_value() && configuredBankedReset->provider
                                         ? configuredBankedReset->provider
                                         : banked_reset_provider::unavailableCodexUsageResetProvider;
    if (configuredBankedReset.has_value()) {
      bankedResetDependencies.kv = configuredBankedReset->kv;
      bankedResetDependencies.now = configuredBankedReset->now;
      bankedResetDependencies.newOwnerToken = configuredBankedReset->newOwnerToken;
      bankedResetDependencies.hash = configuredBankedReset->hash;
      bankedResetDependencies.telemetry = configuredBankedReset->telemetry;
    }

    const auto state = std::make_shared<CodexResponseContext>();
    state->prepared = std::move(prepared);
    state->options = options;
    state->providerDispatch = providerDispatch;
    state->requestedModel = requestedModel;
    state->poolEntry = poolEntry;
    state->selected = selected;
    state->accountEntries = std::move(accountEntries);
    state->url = url;
    state->baseHeaders = baseHeaders;
    state->configuredBankedReset = configuredBankedReset;
    state->configuredBankedResetConfig = configuredBankedResetConfig;
    state->bankedResetDependencies = std::move(bankedResetDependencies);
    state->lastResponse = std::nullopt;
    state->lastError = nullptr;
    state->transportFailure = std::nullopt;
    state->authWarning = std::nullopt;
    state->authFailureState = CodexResponseState::AuthFailureState{.error = std::nullopt};
    state->probeUnavailable = false;
    state->probeUnavailableCircuit = std::nullopt;
    state->attemptNumber = 0;
    state->transportState = CodexResponseState::TransportState{.started = false};
    state->refreshedSlots.clear();
    state->retryState = CodexResponseState::RetryState{.candidate = std::nullopt};
    state->bankedResetCandidates.clear();
    state->activeReselectionRequested = false;
    state->preservedShortRetryState = CodexResponseState::PreservedShortRetryState{.current = std::nullopt};
    state->overageFallbackPending = std::nullopt;
    co_return CreateCodexResponseStateResultState{.state = state};
  }
}
