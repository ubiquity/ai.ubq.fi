// Native auth/model/overage boundaries around source-owned pure evaluation.
#include "uos/codex/routing_evaluation.hpp"

#include "uos/codex/auth.hpp"
#include "uos/codex/overage_settings.hpp"
#include "uos/models/codex_models_availability.hpp"
#include "uos/rt/json.hpp"
#include "uos/utils.hpp"

#include <unordered_set>
#include <utility>
#include <variant>

namespace uos::codex::routing_evaluation {
  namespace {
    namespace decisions = authority::native_codex_evaluation;
    using routing_state::CodexAccountRoutingState;
    using routing_state::CodexBlockedAccountMetadata;
    using routing_state::CodexBlockedRoutingAccount;
    using routing_state::CodexCapacityRoutingObservation;
    using routing_state::CodexRouteSelection;
    using routing_state::CodexRoutingAccount;
    using routing_state::isSafeMs;
    using routing_state::nativeCodexAccountMetadata;
    using routing_state::RouteSelection;
    using routing_state::RoutingAccount;
    using ::uos::types::CodexAuthPoolState;
    using ::uos::types::CodexAuthState;

    // `hydrateAccount(account, auth)`: `{ auth, ...account }`.
    RoutingAccount hydrateAccount(const CodexRoutingAccount& account, const CodexAuthState& auth) {
      RoutingAccount hydrated;
      static_cast<CodexRoutingAccount&>(hydrated) = account;
      hydrated.auth = auth;
      return hydrated;
    }

    CodexBlockedRoutingAccount hydrateBlockedAccount(const CodexBlockedAccountMetadata& account, const CodexAuthState& auth) {
      CodexBlockedRoutingAccount hydrated;
      static_cast<CodexRoutingAccount&>(hydrated) = account;
      hydrated.auth = auth;
      hydrated.quotaResetAtMs = account.quotaResetAtMs;
      return hydrated;
    }

    // `{ ...nativeCodexAccountMetadata(account), quotaResetAtMs: account.quotaResetAtMs, routingGeneration: account.routingGeneration }`
    CodexBlockedAccountMetadata blockedAccountMetadata(const CodexBlockedRoutingAccount& account) {
      CodexBlockedAccountMetadata metadata;
      static_cast<CodexRoutingAccount&>(metadata) = nativeCodexAccountMetadata(account);
      metadata.quotaResetAtMs = account.quotaResetAtMs;
      metadata.routingGeneration = account.routingGeneration;
      return metadata;
    }

    decisions::CodexRoutingAccumulation accumulationMetadata(const CodexRoutingAccumulation& accumulated) {
      decisions::CodexRoutingAccumulation metadata;
      for (const RoutingAccount& account : accumulated.available) metadata.available.push_back(nativeCodexAccountMetadata(account));
      for (const CodexBlockedRoutingAccount& account : accumulated.blockedAccounts) metadata.blockedAccounts.push_back(blockedAccountMetadata(account));
      metadata.skipped = accumulated.skipped;
      metadata.retryAt = accumulated.retryAt;
      metadata.hasQuotaBlock = accumulated.hasQuotaBlock;
      metadata.hasUpstreamTimeoutBlock = accumulated.hasUpstreamTimeoutBlock;
      metadata.modelUnavailable = accumulated.modelUnavailable;
      return metadata;
    }

    // `accounts.find((candidate) => candidate.accountIdHash === account.accountIdHash && candidate.slot === account.slot)?.auth`
    template <class Candidates>
    const CodexAuthState& trustedAuthFor(const Candidates& candidates, const CodexRoutingAccount& account, const char* failure) {
      for (const auto& candidate : candidates)
        if (candidate.accountIdHash == account.accountIdHash && candidate.slot == account.slot) return candidate.auth;
      throw rt::JsError(failure);
    }

    // The `{ accountIdHash, slot, auth }` view of a trusted account the fold may rebind.
    struct TrustedAccount {
      std::string accountIdHash;
      double slot;
      CodexAuthState auth;
    };
  }

  decisions::CodexRoutingAccountEvaluation codexEvaluationMetadata(const CodexRoutingAccountEvaluation& evaluated) {
    decisions::CodexRoutingAccountEvaluation metadata;
    metadata.account = nativeCodexAccountMetadata(evaluated.account);
    metadata.account.routingGeneration = evaluated.account.routingGeneration;
    metadata.routedAccount = evaluated.routedAccount ? std::optional<CodexRoutingAccount>(nativeCodexAccountMetadata(*evaluated.routedAccount)) : std::nullopt;
    metadata.skippedSlot = evaluated.skippedSlot;
    metadata.blockedCircuit = evaluated.blockedCircuit;
    metadata.retryAtMs = evaluated.retryAtMs;
    metadata.blockedAccount = evaluated.blockedAccount ? std::optional<CodexBlockedAccountMetadata>(blockedAccountMetadata(*evaluated.blockedAccount)) : std::nullopt;
    metadata.activeTransitionReason = evaluated.activeTransitionReason;
    metadata.capacityExhausted = evaluated.capacityExhausted;
    metadata.modelUnavailable = evaluated.modelUnavailable;
    return metadata;
  }

  RouteSelection hydrateCodexRouteSelection(const CodexRouteSelection& selection, const std::vector<RoutingAccount>& accounts,
                                            const std::optional<std::string>& poolSnapshotJson, const std::optional<std::string>& capacitySnapshotJson) {
    const char* const failure = "Native Codex account metadata lost its trusted pool binding";
    if (const auto* eligible = std::get_if<routing_state::CodexRouteSelectionEligible>(&selection)) {
      routing_state::RouteSelectionEligible hydrated;
      for (const CodexRoutingAccount& account : eligible->accounts) hydrated.accounts.push_back(hydrateAccount(account, trustedAuthFor(accounts, account, failure)));
      hydrated.skippedSlots = eligible->skippedSlots;
      for (const CodexBlockedAccountMetadata& account : eligible->blockedAccounts)
        hydrated.blockedAccounts.push_back(hydrateBlockedAccount(account, trustedAuthFor(accounts, account, failure)));
      return hydrated;
    }
    if (const auto* blocked = std::get_if<routing_state::CodexRouteSelectionQuotaBlocked>(&selection)) {
      routing_state::RouteSelectionQuotaBlocked hydrated;
      hydrated.skippedSlots = blocked->skippedSlots;
      hydrated.retryAtMs = blocked->retryAtMs;
      for (const CodexBlockedAccountMetadata& account : blocked->blockedAccounts)
        hydrated.blockedAccounts.push_back(hydrateBlockedAccount(account, trustedAuthFor(accounts, account, failure)));
      hydrated.fullCohortExhausted = blocked->fullCohortExhausted;
      hydrated.fullCohortUnavailable = blocked->fullCohortUnavailable;
      hydrated.activeSnapshot = blocked->activeSnapshot;
      hydrated.poolSnapshotJson = poolSnapshotJson;
      hydrated.capacitySnapshotJson = capacitySnapshotJson;
      return hydrated;
    }
    if (const auto* upstream = std::get_if<routing_state::CodexRouteSelectionUpstreamBlocked>(&selection)) return *upstream;
    if (const auto* model = std::get_if<routing_state::CodexRouteSelectionModelUnavailable>(&selection)) return *model;
    if (const auto* credentials = std::get_if<routing_state::CodexRouteSelectionCredentialsInvalid>(&selection)) return *credentials;
    return std::get<routing_state::CodexRouteSelectionRoutingUnavailable>(selection);
  }

  CodexRoutingAccountEvaluation evaluateCodexRoutingAccount(const CodexAccountRoutingState& state, const CodexAuthState& auth, const CodexRoutingSlotIdentity& mapped,
                                                            const std::optional<std::string>& model,
                                                            const rt::OrderedMap<std::string, CodexCapacityRoutingObservation>& observationsByAccount, double now,
                                                            const rt::OrderedMap<std::string, std::optional<std::string>>& modelUnavailable) {
    routing_state::CodexRoutingAccountPolicy policy;
    if (modelUnavailable.has(auth.account_id)) {
      const std::optional<std::string>* detail = modelUnavailable.get(auth.account_id);
      policy.modelUnavailable = routing_state::CodexRoutingAccountModelUnavailable{detail != nullptr ? *detail : std::nullopt};
    }
    policy.overageUsageAllowed = overage_settings::overageUsageAllowedSync(mapped.accountIdHash);
    const decisions::CodexRoutingAccountEvaluation evaluated = decisions::evaluateCodexRoutingAccount(state, mapped, model, observationsByAccount, now, policy);
    CodexRoutingAccountEvaluation hydrated;
    hydrated.account = hydrateAccount(evaluated.account, auth);
    hydrated.routedAccount = evaluated.routedAccount ? std::optional<RoutingAccount>(hydrateAccount(*evaluated.routedAccount, auth)) : std::nullopt;
    hydrated.skippedSlot = evaluated.skippedSlot;
    hydrated.blockedCircuit = evaluated.blockedCircuit;
    hydrated.retryAtMs = evaluated.retryAtMs;
    hydrated.blockedAccount =
      evaluated.blockedAccount ? std::optional<CodexBlockedRoutingAccount>(hydrateBlockedAccount(*evaluated.blockedAccount, auth)) : std::nullopt;
    hydrated.activeTransitionReason = evaluated.activeTransitionReason;
    hydrated.capacityExhausted = evaluated.capacityExhausted;
    hydrated.modelUnavailable = evaluated.modelUnavailable;
    return hydrated;
  }

  void foldCodexRoutingEvaluation(CodexRoutingAccumulator& accumulator, const CodexRoutingAccountEvaluation& evaluated) {
    std::vector<TrustedAccount> trustedAccounts;
    for (const RoutingAccount& account : accumulator.available) trustedAccounts.push_back(TrustedAccount{account.accountIdHash, account.slot, account.auth});
    for (const CodexBlockedRoutingAccount& account : accumulator.blockedAccounts)
      trustedAccounts.push_back(TrustedAccount{account.accountIdHash, account.slot, account.auth});
    trustedAccounts.push_back(TrustedAccount{evaluated.account.accountIdHash, evaluated.account.slot, evaluated.account.auth});
    decisions::CodexRoutingAccumulation metadata = accumulationMetadata(accumulator);
    decisions::foldCodexRoutingEvaluation(metadata, codexEvaluationMetadata(evaluated));
    // The decision pushes onto the accumulator's own `skipped` array, which the metadata shares.
    accumulator.skipped = metadata.skipped;
    const char* const failure = "Native Codex accumulation lost its trusted pool binding";
    std::vector<RoutingAccount> available;
    for (const CodexRoutingAccount& account : metadata.available) available.push_back(hydrateAccount(account, trustedAuthFor(trustedAccounts, account, failure)));
    std::vector<CodexBlockedRoutingAccount> blockedAccounts;
    for (const CodexBlockedAccountMetadata& account : metadata.blockedAccounts)
      blockedAccounts.push_back(hydrateBlockedAccount(account, trustedAuthFor(trustedAccounts, account, failure)));
    accumulator.available = std::move(available);
    accumulator.blockedAccounts = std::move(blockedAccounts);
    accumulator.retryAt = metadata.retryAt;
    accumulator.hasQuotaBlock = metadata.hasQuotaBlock;
    accumulator.hasUpstreamTimeoutBlock = metadata.hasUpstreamTimeoutBlock;
    accumulator.modelUnavailable = metadata.modelUnavailable;
  }

  RouteSelection classifyCodexRouteSelection(const CodexRoutingAccumulation& accumulated, bool fullCohortExhausted,
                                             const routing_state::CodexActiveAccountSnapshot& activeSnapshot, const std::optional<std::string>& poolSnapshotJson,
                                             const std::optional<std::string>& capacitySnapshotJson, bool fullCohortUnavailable) {
    std::vector<RoutingAccount> accounts(accumulated.available.begin(), accumulated.available.end());
    for (const CodexBlockedRoutingAccount& account : accumulated.blockedAccounts) accounts.push_back(static_cast<const RoutingAccount&>(account));
    const CodexRouteSelection selection =
      decisions::classifyCodexRouteSelection(accumulationMetadata(accumulated), fullCohortExhausted, activeSnapshot, std::nullopt, std::nullopt, fullCohortUnavailable);
    return hydrateCodexRouteSelection(selection, accounts, poolSnapshotJson, capacitySnapshotJson);
  }

  /** Accumulate every ordered account's routing outcome, preserving selection order. */
  CodexRoutingAccumulation accumulateCodexRoutingAccounts(const CodexAccountRoutingState& state, const rt::OrderedMap<std::string, CodexRoutingSlotIdentity>& byId,
                                                          const std::vector<CodexAuthState>& orderedAccounts, const std::optional<std::string>& model,
                                                          const rt::OrderedMap<std::string, CodexCapacityRoutingObservation>& observationsByAccount, double now) {
    CodexRoutingAccumulator accumulator;
    accumulator.retryAt = std::nullopt;
    accumulator.hasQuotaBlock = false;
    accumulator.hasUpstreamTimeoutBlock = false;
    accumulator.modelUnavailable = std::nullopt;
    std::vector<std::string> accountIds;
    for (const CodexAuthState& auth : orderedAccounts) accountIds.push_back(auth.account_id);
    const rt::OrderedMap<std::string, std::optional<std::string>> unavailableAccounts = models::codex_models_availability::codexModelUnavailableAccounts(model, accountIds);
    for (const CodexAuthState& auth : orderedAccounts) {
      const CodexRoutingSlotIdentity* mapped = byId.get(auth.account_id);
      if (mapped == nullptr) continue;
      foldCodexRoutingEvaluation(accumulator, evaluateCodexRoutingAccount(state, auth, *mapped, model, observationsByAccount, now, unavailableAccounts));
    }
    return accumulator;
  }

  std::optional<CodexAuthPoolState> parseCodexAuthPoolSnapshot(const json::value& value) { return parseCodexAuthPoolSnapshot(&value); }

  std::optional<CodexAuthPoolState> parseCodexAuthPoolSnapshot(const json::value* value) {
    const json::value* accounts = rt::get(value, "accounts");
    if (!utils::isRecord(value) || accounts == nullptr || !accounts->is_array() || !isSafeMs(rt::get(value, "updated_at_ms")) || accounts->get_array().size() < 1)
      return std::nullopt;
    std::unordered_set<std::string> accountIds;
    for (const json::value& candidate : accounts->get_array()) {
      if (!utils::isRecord(candidate)) return std::nullopt;
      const std::optional<std::string> accountId = utils::getString(rt::get(candidate, "account_id"));
      const std::optional<std::string> accessToken = utils::getString(rt::get(candidate, "access_token"));
      if (!rt::truthy(accountId) || !rt::truthy(accessToken) || !isSafeMs(rt::get(candidate, "updated_at_ms")) || accountIds.contains(*accountId)) return std::nullopt;
      accountIds.insert(*accountId);
    }
    // Preserve validated ownership and the canonical maximum-pool bound.
    return auth::parseCodexAuthPool(value);
  }
}
