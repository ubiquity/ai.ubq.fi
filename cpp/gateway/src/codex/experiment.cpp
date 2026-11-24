// Codex capacity and cache scope experiment, split out of src/codex.ts.
#include "uos/codex/experiment.hpp"

#include "uos/codex/account_routing.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/auth_refresh.hpp"
#include "uos/codex/dispatch.hpp"
#include "uos/codex/rate_limit_429.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <exception>
#include <memory>
#include <utility>

namespace uos::codex::experiment {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
    using routing_state::RouteSelection;
    using routing_state::RoutingAccount;
    using ::uos::types::CodexAuthPoolState;
    using ::uos::types::CodexAuthState;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexCapacityAccount& value) {
    out = Writer().required("slot", value.slot).required("account_id", value.account_id).required("access_token", value.access_token).orNull("email", value.email).done();
  }

  CodexCapacityAccount tag_invoke(const json::value_to_tag<CodexCapacityAccount>&, const json::value& value) {
    const Reader in(value, "CodexCapacityAccount");
    return CodexCapacityAccount{.slot = in.required<double>("slot"),
                                .account_id = in.required<std::string>("account_id"),
                                .access_token = in.required<std::string>("access_token"),
                                .email = in.orNull<std::string>("email")};
  }

  rt::Task<std::vector<CodexCapacityAccount>> getCodexCapacityAccounts() {
    rt::Promise<auth::CodexAuthPoolEntry> loading(auth::getAuthPoolEntry(true));
    const auth::CodexAuthPoolEntry current = co_await loading.wait();
    std::vector<CodexCapacityAccount> accounts;
    for (std::size_t index = 0; index < current.pool.accounts.size(); ++index) {
      const CodexAuthState& account = current.pool.accounts[index];
      accounts.push_back(CodexCapacityAccount{.slot = static_cast<double>(index) + 1,
                                              .account_id = account.account_id,
                                              .access_token = account.access_token,
                                              .email = auth::getCodexAccountEmail(account.access_token)});
    }
    co_return accounts;
  }

  namespace {
    // `CodexAuthPoolEntry & { kv: Kv; entry: KvEntry<CodexAuthPoolState> }`: the strongly read row with its
    // (non-null) versionstamp.
    struct StrongAuthPoolEntry {
      std::shared_ptr<rt::Kv> kv;
      rt::KvEntryMaybe<> entry;
      std::string versionstamp;
      CodexAuthPoolState pool;
    };

    rt::Task<StrongAuthPoolEntry> getStrongAuthPoolEntryForCacheScopeExperiment() {
      rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (!kv) {
        throw CodexCacheScopeExperimentError("Prompt-cache scope experiments require Deno KV-backed Codex auth.");
      }
      rt::KvGetOptions strong;
      strong.consistency = rt::KvConsistencyLevel::strong;
      rt::Task<rt::KvEntryMaybe<>> reading = kv->get(auth::CODEX_AUTH_POOL_KV_KEY, strong);
      const rt::KvEntryMaybe<> entry = co_await std::move(reading);
      const std::optional<CodexAuthPoolState> pool = auth::parseCodexAuthPool(entry.value.toJson());
      if (!pool.has_value() || !rt::truthy(entry.versionstamp)) {
        throw CodexCacheScopeExperimentError("Codex auth pool is unavailable for the prompt-cache scope experiment.");
      }
      auth::cacheCodexAuthPool(*pool);
      co_return StrongAuthPoolEntry{.kv = kv, .entry = entry, .versionstamp = *entry.versionstamp, .pool = *pool};
    }

    CodexAuthState cacheScopeExperimentAccount(const CodexCacheScopeExperimentSession& session, const StrongAuthPoolEntry& poolEntry, double slot) {
      if (!rt::Number::isInteger(slot) || slot < 1 || slot > auth::CODEX_AUTH_POOL_MAX_ACCOUNTS) {
        throw CodexCacheScopeExperimentError("Prompt-cache scope experiment slot is invalid.");
      }
      if (poolEntry.versionstamp != session.authPoolVersionstamp) {
        throw CodexCacheScopeExperimentError("Codex auth pool changed during the prompt-cache scope experiment.");
      }
      // `Array.prototype.at` keeps the unchecked slot visible as possibly absent,
      // which the pool can be: it may hold only the first of the two slots.
      const CodexAuthState* auth = rt::at(poolEntry.pool.accounts, slot - 1);
      const std::string& expectedAccountId = session.expectedAccountIds[static_cast<std::size_t>(slot - 1)];
      if (auth == nullptr) {
        throw CodexCacheScopeExperimentError("Codex auth pool changed during the prompt-cache scope experiment.");
      }
      if (auth->account_id != expectedAccountId) {
        throw CodexCacheScopeExperimentError("Codex auth pool changed during the prompt-cache scope experiment.");
      }
      return *auth;
    }
  }

  rt::Task<CodexCacheScopeExperimentSession> beginCodexCacheScopeExperiment() {
    rt::Promise<StrongAuthPoolEntry> loading(getStrongAuthPoolEntryForCacheScopeExperiment());
    const StrongAuthPoolEntry poolEntry = co_await loading.wait();
    const CodexAuthState* first = rt::at(poolEntry.pool.accounts, 0);
    const CodexAuthState* second = rt::at(poolEntry.pool.accounts, 1);
    if (poolEntry.pool.accounts.size() != 2 || first == nullptr || second == nullptr) {
      throw CodexCacheScopeExperimentError("Prompt-cache scope experiments require exactly two configured Codex slots.");
    }
    co_return CodexCacheScopeExperimentSession{.expectedAccountIds = {first->account_id, second->account_id}, .authPoolVersionstamp = poolEntry.versionstamp};
  }

  rt::Task<CodexCacheScopeExperimentRefreshResult> refreshCodexCacheScopeExperimentSlot(CodexCacheScopeExperimentSession session, double slot) {
    return refreshCodexCacheScopeExperimentSlot(std::move(session), slot, std::nullopt);
  }

  rt::Task<CodexCacheScopeExperimentRefreshResult> refreshCodexCacheScopeExperimentSlot(CodexCacheScopeExperimentSession session, double slot,
                                                                                         std::optional<rt::AbortSignal> signal) {
    rt::Promise<StrongAuthPoolEntry> loading(getStrongAuthPoolEntryForCacheScopeExperiment());
    const StrongAuthPoolEntry current = co_await loading.wait();
    const CodexAuthState auth = cacheScopeExperimentAccount(session, current, slot);
    std::vector<CodexAuthState> orderedAccounts{auth};
    rt::Promise<RouteSelection> selecting(account_routing::selectCodexRoutingAccounts(current.pool, std::move(orderedAccounts)));
    const RouteSelection selected = co_await selecting.wait();
    const auto* eligible = std::get_if<routing_state::RouteSelectionEligible>(&selected);
    const RoutingAccount* selectedAccount = eligible != nullptr ? rt::at(eligible->accounts, 0) : nullptr;
    if (eligible == nullptr || eligible->accounts.size() != 1 || selectedAccount == nullptr || selectedAccount->slot != slot - 1) {
      throw CodexCacheScopeExperimentError("The requested Codex slot is not eligible for OAuth refresh during the prompt-cache scope experiment.");
    }
    std::optional<CodexAuthState> refreshed;
    rt::Reason failure;
    try {
      // Unlike normal inference refreshes, this is a controlled experiment
      // transition. It must not adopt a same-account rotation from another
      // request, because that would falsely make the experiment's refresh row
      // appear to prove a cache scope. Persistence below is fenced to the exact
      // pool version we just validated.
      rt::Promise<CodexAuthState> refreshing(auth_refresh::refreshAuthStateless(auth));
      rt::Promise<CodexAuthState> awaiting(auth_refresh::awaitWithoutCancellingSharedWork(refreshing, signal));
      refreshed = co_await awaiting.wait();
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) {
      rt::Promise<StrongAuthPoolEntry> reloading(getStrongAuthPoolEntryForCacheScopeExperiment());
      const rt::Promise<std::optional<StrongAuthPoolEntry>> caught = reloading.then(
        [](const StrongAuthPoolEntry& entry) { return std::optional<StrongAuthPoolEntry>(entry); },
        [](const rt::Reason&) { return std::optional<StrongAuthPoolEntry>(); });
      const std::optional<StrongAuthPoolEntry> afterFailure = co_await caught.wait();
      if (afterFailure.has_value() && afterFailure->versionstamp != current.versionstamp) co_return CodexCacheScopeExperimentRefreshResultAuthPoolDrift{};
      std::rethrow_exception(failure);
    }
    if (refreshed->account_id != auth.account_id) {
      throw CodexCacheScopeExperimentError("Codex OAuth refresh changed account identity during the prompt-cache scope experiment.");
    }

    std::vector<CodexAuthState> accounts = current.pool.accounts;
    accounts[static_cast<std::size_t>(slot - 1)] = *refreshed;
    const CodexAuthPoolState nextPool{.accounts = std::move(accounts), .updated_at_ms = rt::dateNow()};
    const rt::KvValue nextPoolValue(json::value_from(nextPool));
    auto operation = current.kv->atomic();
    operation->check(current.entry).set(auth::CODEX_AUTH_POOL_KV_KEY, nextPoolValue);
    rt::Task<rt::KvCommitResult> committing = operation->commit();
    const rt::KvCommitResult persisted = co_await std::move(committing);
    if (!persisted.ok) co_return CodexCacheScopeExperimentRefreshResultAuthPoolDrift{};

    rt::Promise<RoutingAccount> reconciling(routing_mutations::reconcileCodexRoutingAccount(*selectedAccount, *refreshed));
    co_await reconciling.wait();
    rt::Promise<StrongAuthPoolEntry> reloading(getStrongAuthPoolEntryForCacheScopeExperiment());
    const StrongAuthPoolEntry after = co_await reloading.wait();
    const CodexAuthState* persistedAuth = rt::at(after.pool.accounts, slot - 1);
    if (persistedAuth == nullptr) co_return CodexCacheScopeExperimentRefreshResultAuthPoolDrift{};
    const CodexAuthState* firstAfter = rt::element(after.pool.accounts, 0);
    const CodexAuthState* secondAfter = rt::element(after.pool.accounts, 1);
    if (after.versionstamp != persisted.versionstamp || persistedAuth->account_id != refreshed->account_id ||
        !auth_refresh::sameCodexCredentials(*persistedAuth, *refreshed) || firstAfter == nullptr || firstAfter->account_id != session.expectedAccountIds[0] ||
        secondAfter == nullptr || secondAfter->account_id != session.expectedAccountIds[1]) {
      co_return CodexCacheScopeExperimentRefreshResultAuthPoolDrift{};
    }
    co_return CodexCacheScopeExperimentRefreshResultRefreshed{
      .tokenChanged = persistedAuth->access_token != auth.access_token || persistedAuth->refresh_token != auth.refresh_token,
      .session = CodexCacheScopeExperimentSession{.expectedAccountIds = session.expectedAccountIds, .authPoolVersionstamp = after.versionstamp}};
  }

  namespace {
    /**
     * Slot-pinned dispatch must observe the exact credentials it selected. Both
     * fences force-read the pool, once before the upstream call and once after it,
     * so a concurrent rotation can never be attributed to the experiment.
     */
    // `phase: "before dispatch" | "after dispatch"`
    rt::Task<void> assertCacheScopeExperimentCredentialsCurrent(CodexCacheScopeExperimentSession session, double slot, CodexAuthState auth, std::string phase) {
      rt::Promise<StrongAuthPoolEntry> loading(getStrongAuthPoolEntryForCacheScopeExperiment());
      const StrongAuthPoolEntry poolEntry = co_await loading.wait();
      const CodexAuthState persisted = cacheScopeExperimentAccount(session, poolEntry, slot);
      if (!auth_refresh::sameCodexCredentials(persisted, auth)) {
        throw CodexCacheScopeExperimentError("Codex credentials changed " + phase + " during the prompt-cache scope experiment.");
      }
    }

    /** Claim the pinned slot's probe lease when routing requires one. */
    rt::Task<RoutingAccount> claimCacheScopeExperimentRouting(CodexAuthPoolState pool, RoutingAccount routing) {
      if (!routing.probeRequired) co_return routing;
      rt::Promise<std::optional<RoutingAccount>> claiming(routing_mutations::claimCodexRoutingProbe(pool, routing));
      const std::optional<RoutingAccount> claimed = co_await claiming.wait();
      if (!claimed.has_value()) {
        throw CodexCacheScopeExperimentError("The requested Codex slot could not be claimed for the prompt-cache scope experiment.");
      }
      co_return *claimed;
    }

    /**
     * The experiment never retries inference, so a failed dispatch is recorded as
     * thrown provider health and rerouted through the normal routing telemetry
     * before it propagates to the caller.
     */
    // `signal: AbortSignal | undefined`
    rt::Task<rt::Response> dispatchCacheScopeExperimentAttempt(RoutingAccount routing, json::value body, rt::Headers headers, std::optional<rt::AbortSignal> signal) {
      rt::Reason failure;
      try {
        rt::Promise<rt::Response> fetching(dispatch::fetchCodexResponseWithAuth(routing.auth, ::uos::config::config.codexBaseUrl + "/responses",
                                                                                rt::jsonStringify(body), headers, signal));
        rt::Response response = co_await fetching.wait();
        co_return response;
      } catch (...) {
        failure = std::current_exception();
      }
      rt::spawn(dispatch::recordCodexThrownHealth(routing.auth.account_id, failure));
      const auth::CodexError* codexError = auth::codexErrorOf(failure);
      json::object fields;
      fields["request_id"] = nullptr;
      fields["attempt"] = 1;
      fields["slot"] = routing.slot + 1;
      fields["phase"] = "initial";
      fields["status"] = codexError != nullptr ? json::value(codexError->status) : json::value(nullptr);
      fields["status_class"] = dispatch::codexErrorClass(failure);
      dispatch::logCodexRouting("codex_attempt", fields);
      std::rethrow_exception(failure);
    }
  }

  rt::Task<rt::Response> fetchCodexResponsesForCacheScopeExperiment(json::value body, FetchCodexResponsesForCacheScopeExperimentOptions options) {
    const std::string conversationId = rt::trim(options.conversationId);
    if (conversationId.empty()) {
      throw CodexCacheScopeExperimentError("Prompt-cache scope experiment conversation id is invalid.");
    }
    if (auth::codexProbeTransitionsInFlight.size()) {
      std::vector<rt::Promise<void>> transitions;
      for (const rt::Promise<void>& transition : auth::codexProbeTransitionsInFlight.values()) transitions.push_back(transition);
      auto settled = rt::allSettled(std::move(transitions));
      co_await settled.wait();
    }

    rt::Promise<StrongAuthPoolEntry> loading(getStrongAuthPoolEntryForCacheScopeExperiment());
    const StrongAuthPoolEntry current = co_await loading.wait();
    const CodexAuthState auth = cacheScopeExperimentAccount(options.session, current, options.slot);
    const std::optional<std::string> requestedModel = ::uos::utils::isRecord(body) ? ::uos::utils::getString(rt::get(body, "model")) : std::nullopt;
    std::vector<CodexAuthState> orderedAccounts{auth};
    rt::Promise<RouteSelection> selecting(account_routing::selectCodexRoutingAccounts(current.pool, std::move(orderedAccounts), rt::dateNow(), requestedModel));
    const RouteSelection selected = co_await selecting.wait();
    const auto* eligible = std::get_if<routing_state::RouteSelectionEligible>(&selected);
    if (eligible == nullptr || eligible->accounts.size() != 1) {
      throw CodexCacheScopeExperimentError("The requested Codex slot is not eligible for the prompt-cache scope experiment.");
    }
    RoutingAccount routing = eligible->accounts[0];
    if (routing.slot != options.slot - 1 || routing.auth.account_id != auth.account_id) {
      throw CodexCacheScopeExperimentError("Prompt-cache scope experiment routing did not preserve the requested slot.");
    }
    if (routing.quotaUsageProbeRequired) {
      throw CodexCacheScopeExperimentError("Current Codex quota could not be verified for the selected experiment slot.");
    }
    rt::Promise<RoutingAccount> claiming(claimCacheScopeExperimentRouting(current.pool, routing));
    routing = co_await claiming.wait();

    std::optional<rt::Response> response;
    const rt::Headers headers{
      {"originator", std::string(auth::CODEX_ORIGINATOR)},
      {"user-agent", auth::codexUserAgent(options.clientVersion)},
      {"Content-Type", "application/json"},
      {"Accept", "text/event-stream"},
      {"conversation_id", conversationId},
    };
    rt::Reason failure;
    try {
      rt::Promise<void> before(assertCacheScopeExperimentCredentialsCurrent(options.session, options.slot, routing.auth, "before dispatch"));
      co_await before.wait();
      rt::Promise<rt::Response> dispatching(dispatchCacheScopeExperimentAttempt(routing, body, headers, options.signal));
      response = co_await dispatching.wait();
      auth::setCodexResponseAccountTelemetry(*response, options.slot, routing.auth.account_id);
      rt::spawn(dispatch::recordCodexResponseHealth(routing.auth.account_id, *response, routing.auth));
      json::object fields;
      fields["request_id"] = nullptr;
      fields["attempt"] = 1;
      fields["slot"] = routing.slot + 1;
      fields["phase"] = "initial";
      fields["status"] = response->status();
      fields["status_class"] = dispatch::codexStatusClass(response->status());
      dispatch::logCodexRouting("codex_attempt", fields);

      if (response->status() == 429) {
        rt::Promise<rate_limit_429::Codex429Classification> marking(rate_limit_429::markCodexQuotaBlocked(routing, *response));
        const rate_limit_429::Codex429Classification classification = co_await marking.wait();
        response = classification.response;
        auth::setCodexResponseAccountTelemetry(*response, options.slot, routing.auth.account_id);
      }
      if (!response->ok()) {
        rt::Promise<void> releasing(routing_mutations::releaseCodexRoutingProbe(routing));
        co_await releasing.wait();
      } else if (routing.probeGeneration.has_value()) {
        auth::codexProbeByResponse.set(*response, routing);
      }

      rt::Promise<void> after(assertCacheScopeExperimentCredentialsCurrent(options.session, options.slot, routing.auth, "after dispatch"));
      co_await after.wait();
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) {
      if (response.has_value()) dispatch::cancelResponseBody(*response);
      rt::Promise<void> releasing(routing_mutations::releaseCodexRoutingProbe(routing));
      co_await releasing.wait();
      std::rethrow_exception(failure);
    }
    co_return *response;
  }
}
