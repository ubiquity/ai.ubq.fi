// Port of src/authority/native-authority-codex.ts.
#include "uos/authority/native_authority_codex.hpp"
#include "uos/authority/native_codex_mutations.hpp"
#include "uos/rt/crypto.hpp"
#include <algorithm>

namespace uos::authority::native_authority_codex {
  using namespace native_authority_contracts;
  using namespace native_codex_contracts;
  namespace mutation = native_codex_mutations;
  namespace capacity = native_codex_capacity;
  namespace {
    json::value member(const NativeRequest& row, const std::string& key) { const auto encoded = json::value_from(row); return *rt::get(encoded, key); }
    const CodexRoutingIdentity* identityFor(const NativePool& pool, double slot) { const auto found = std::find_if(pool.identities.begin(), pool.identities.end(), [&](const auto& value) { return value.slot == slot; }); return found == pool.identities.end() ? nullptr : &*found; }
    bool selected(const NativePool& pool, const std::string& hash) { return std::find(pool.selectedAccountHashes.begin(), pool.selectedAccountHashes.end(), hash) != pool.selectedAccountHashes.end(); }
  }
  rt::OrderedMap<std::string, CodexRoutingAccountPolicy> NativeAuthorityCodex::_policies(const NativePool& pool, const std::string& model) const {
    const auto* policies = pool.modelPolicies ? rt::getObject(*pool.modelPolicies, model) : nullptr; if (!policies) policies = &pool.policies;
    rt::OrderedMap<std::string, CodexRoutingAccountPolicy> result;
    for (const auto& key : rt::objectKeys(*policies)) {
      const auto& value = policies->at(key); CodexRoutingAccountPolicy policy;
      const auto* unavailable = rt::get(value, "modelUnavailable");
      if (unavailable && !unavailable->is_null()) policy.modelUnavailable = CodexRoutingAccountModelUnavailable{rt::getString(*unavailable, "detail") ? std::optional<std::string>(std::string(*rt::getString(*unavailable, "detail"))) : std::nullopt};
      policy.overageUsageAllowed = rt::getBool(value, "overageUsageAllowed").value_or(false); result.set(key, policy);
    }
    return result;
  }
  NativeAuthorityCodex::Rows NativeAuthorityCodex::_rows(double now) const {
    const auto pool = _sql.readAs<NativePool>("codex_pool", CODEX_AUTH_POOL_KV_KEY, now); const auto active = _sql.read("codex_active", CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, now), routing = _sql.read("codex_routing", CODEX_ACCOUNT_ROUTING_KV_KEY, now), observations = _sql.read("codex_capacity", CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY, now);
    if (!pool.value || !pool.revision) throw NativeAuthorityError("codex_pool_unavailable", 503);
    const auto parsed = routing.value ? parseCodexAccountRoutingState(*routing.value) : std::nullopt;
    if (routing.value && !parsed) throw NativeAuthorityError("codex_routing_malformed", 503);
    return {*pool.value, *pool.revision, active, routing, observations, parsed, capacity::normalizeRoutingState(parsed ? &*parsed : nullptr, pool.value->identities, now, true), {active.revision, pool.revision, routing.revision, observations.revision}};
  }
  namespace {
    struct CheckedOwner { NativeOwner value; json::object raw; };
    CheckedOwner checkedOwner(native_authority_sql::NativeAuthoritySql& sql, const CodexRoutingAccount& account, double now) {
      const auto raw = sql.read("codex_owner", {account.accountIdHash}, now).value;
      const auto owner = raw && !raw->is_null() ? std::optional<NativeOwner>(json::value_to<NativeOwner>(*raw)) : std::nullopt;
      if (!owner || owner->revoked || owner->routingAccountIdHash != account.accountIdHash || owner->credentialVersion != account.credentialVersion || owner->expiresAtMs <= now) throw NativeAuthorityError("codex_owner_unavailable", 503);
      return {*owner, raw->as_object()};
    }
  }
  NativeOwner NativeAuthorityCodex::_owner(const CodexRoutingAccount& account, double now) const { return checkedOwner(_sql, account, now).value; }
  NativeCodexAuthorization NativeAuthorityCodex::authorization(const NativeCapability& cap, const std::string& handle, double now) const {
    const auto request = _api.request(cap, handle, now);
    if (!request.codex || !request.ownerFence) throw NativeAuthorityError("codex_admission_missing");
    return {*request.codex, *request.ownerFence};
  }
  void NativeAuthorityCodex::fence(const NativeCapability& cap, const std::string& handle, double now, const std::optional<NativeCodexAuthorization>& authorizationValue) const {
    const auto request = _api.request(cap, handle, now); const auto account = authorizationValue ? std::optional<CodexRoutingAccount>(authorizationValue->account) : request.codex;
    if (!account || !account->activeGeneration) throw NativeAuthorityError("codex_admission_missing");
    const auto rows = _rows(now); const auto* identity = identityFor(rows.pool, account->slot);
    if (!identity || identity->accountIdHash != account->accountIdHash || identity->credentialVersion != account->credentialVersion || !selected(rows.pool, account->accountIdHash) || !codexActiveAccountSelectionCurrent(rows.active.value.value_or(nullptr), *account, *account->activeGeneration) || account->activePoolVersionstamp != rows.poolRevision) throw NativeAuthorityError("codex_dispatch_fence_stale");
    const auto slot = capacity::slotFor(rows.normalized, *account);
    if (!mutation::admissionRefreshRoutingGeneration(&rows.normalized, *account)) throw NativeAuthorityError("codex_credential_invalid");
    if (!capacity::slotMatchesCodexRoutingAccount(slot, *account) || account->routingGeneration != slot.generation || (account->probeGeneration && !capacity::probeLeaseMatchesCodexRoutingAccount(slot, *account))) throw NativeAuthorityError("codex_routing_fence_stale");
    const auto owner = _owner(*account, now); const auto prior = authorizationValue ? std::optional<NativeOwner>(authorizationValue->owner) : request.ownerFence;
    if (!prior || prior->ownerId != owner.ownerId || prior->ownerEpoch != owner.ownerEpoch || prior->publicationRevision != owner.publicationRevision || prior->nativeGenerationHash != owner.nativeGenerationHash) throw NativeAuthorityError("codex_owner_fence_stale");
  }
  NativeReply NativeAuthorityCodex::select(const NativeCapability& cap, const std::string& handle, double now) {
    const auto validated = _api.validated(cap, handle, now); auto request = static_cast<const NativeRequest&>(validated); const auto rows = _rows(now);
    const auto plan = native_codex::selectCodex({rows.active.value.value_or(nullptr), rows.routing.value.value_or(nullptr), rows.capacity.value.value_or(nullptr), rows.pool.identities, rows.pool.selectedAccountHashes, _policies(rows.pool, validated.model), rows.revisions}, now, validated.model);
    if (!plan) return {{"kind", "routing_unavailable"}};
    if (plan->writes.routing) _sql.write("codex_routing", CODEX_ACCOUNT_ROUTING_KV_KEY, toJson(*plan->writes.routing));
    if (plan->writes.active) _sql.write("codex_active", CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, toJson(*plan->writes.active));
    request.selection = plan->selection; const auto* eligible = std::get_if<CodexRouteSelectionEligible>(&plan->selection);
    if (!eligible) {
      request.codexProof = _rows(now).revisions; request.codex.reset(); request.ownerFence.reset(); _api.save(request);
      return {{"kind", std::visit([](const auto& value) { return std::string(value.kind); }, plan->selection)}, {"detail", member(request, "selection")}};
    }
    if (eligible->accounts.empty()) throw NativeAuthorityError("codex_selection_empty", 503);
    auto account = eligible->accounts.front();
    if (account.probeRequired) {
      const auto current = _rows(now); const auto claim = mutation::buildExpiredProbeClaim(current.normalized, account, now, rt::crypto::randomUUID(), now);
      if (!claim) return {{"kind", "routing_unavailable"}};
      _sql.write("codex_routing", CODEX_ACCOUNT_ROUTING_KV_KEY, toJson(claim->next)); account.probeRequired = false; account.probeGeneration = claim->lease.generation; account.probeToken = claim->lease.token; account.probeCircuit = claim->lease.circuit;
    }
    const auto owner = checkedOwner(_sql, account, now); request.codexProof.reset(); request.codex = account; request.ownerFence = owner.value; _api.save(request, json::object{{"codex", member(request, "codex")}, {"ownerFence", owner.raw}}); return {{"kind", "eligible"}, {"detail", member(request, "codex")}};
  }
  NativeReply NativeAuthorityCodex::signal(const NativeCapability& cap, const NativeArguments::recordCodexAttemptSignal& args, const native_codex_429::Codex429Classification* parsed, double now) {
    auto request = _api.request(cap, args.handle, now); const auto account = request.codex;
    if (!account) throw NativeAuthorityError("codex_admission_missing");
    request.providerObserved = true;
    if (args.signal == "success" && !request.semantic) request.responding = NativeRequestResponding{"chatgpt_codex", std::nullopt};
    _api.save(request); const auto rows = _rows(now); const auto* identity = identityFor(rows.pool, account->slot);
    if (!identity || identity->accountIdHash != account->accountIdHash || identity->credentialVersion != account->credentialVersion) return {{"kind", "stale"}};
    std::optional<CodexAccountRoutingState> next;
    if (args.signal == "success") next = mutation::markCodexSuccess(rows.normalized, *account, now);
    else if (args.signal == "credential_invalid") next = mutation::markCodexCredentialInvalid(rows.normalized, *account, now, now);
    else if (args.signal == "upstream_timeout") next = mutation::markCodexUpstreamTimeout(rows.normalized, *account, now, now);
    else if (args.signal == "quota") { if (!parsed) throw NativeAuthorityError("codex_quota_response_missing", 400); next = parsed->usageLimitReached ? native_codex_429::quotaBlockTransition(rows.normalized, *account, *parsed, now, args.recoveryProbe == true, now) : native_codex_429::releaseNonBlockingQuotaProbe(rows.normalized, *account, now); }
    else throw NativeAuthorityError("invalid_codex_signal", 400);
    if (next) _sql.write("codex_routing", CODEX_ACCOUNT_ROUTING_KV_KEY, toJson(*next));
    NativeReply reply{{"kind", next ? "recorded" : "unchanged"}};
    if (parsed) reply["detail"] = json::object{{"usageLimitReached", parsed->usageLimitReached}, {"retryAtMs", parsed->retryAtMs ? json::value(rt::makeNumber(*parsed->retryAtMs)) : json::value(nullptr)}, {"quotaBlockSource", parsed->quotaBlockSource ? json::value(*parsed->quotaBlockSource) : json::value(nullptr)}, {"resetDeadlineIsStable", parsed->resetDeadlineIsStable}, {"resetDeadlineConflict", parsed->resetDeadlineConflict}};
    return reply;
  }
  NativeReply NativeAuthorityCodex::release(const NativeCapability& cap, const std::string& handle, double now) {
    const auto request = _api.request(cap, handle, now);
    if (!request.codex) return {{"kind", "missing"}};
    const auto next = mutation::releaseCodexRoutingProbe(_rows(now).normalized, *request.codex, now);
    if (next) _sql.write("codex_routing", CODEX_ACCOUNT_ROUTING_KV_KEY, toJson(*next));
    return {{"kind", next ? "released" : "stale"}};
  }
  NativeReply NativeAuthorityCodex::refresh(const NativeCapability& cap, const std::string& handle, double now) {
    auto request = _api.request(cap, handle, now); const auto account = request.codex;
    if (!account) return {{"kind", "stale"}};
    const auto rows = _rows(now); const auto active = parseCodexActiveAccountSelection(rows.active.value.value_or(nullptr)); const auto* identity = identityFor(rows.pool, account->slot);
    if (!active || account->activeGeneration != active->generation || active->account_id_hash != account->accountIdHash || active->slot != account->slot || !identity || identity->accountIdHash != account->accountIdHash) return {{"kind", "stale"}};
    const bool changed = account->credentialVersion != identity->credentialVersion; const auto* slot = rt::at(rows.normalized.slots, account->slot); const auto lease = slot ? slot->probe_lease : std::nullopt;
    const bool retained = changed && account->probeGeneration && account->probeToken && lease && lease->token == *account->probeToken && lease->circuit == account->probeCircuit.valueOr("quota") && slot && lease->generation == slot->generation;
    auto current = *account;
    if (changed) { current.probeRequired = false; current.probeGeneration.reset(); current.probeToken.reset(); current.probeCircuit = nullptr; }
    if (retained) { current.probeGeneration = slot->generation; current.probeToken = lease->token; current.probeCircuit = lease->circuit; }
    current.credentialVersion = identity->credentialVersion;
    const auto generation = mutation::admissionRefreshRoutingGeneration(rows.routing.value ? &rows.normalized : nullptr, current);
    if (!generation) return {{"kind", "stale"}};
    if (native_codex_evaluation::routingStateChangedByNormalization(rows.parsed ? &*rows.parsed : nullptr, rows.normalized)) _sql.write("codex_routing", CODEX_ACCOUNT_ROUTING_KV_KEY, toJson(rows.normalized));
    const auto prepared = mutation::prepareCodexAdmissionRefresh(*active, current, rows.poolRevision, *generation);
    if (const auto* refresh = std::get_if<mutation::CodexAdmissionRefreshPlanRefresh>(&prepared)) { auto next = refresh->next; next.updated_at_ms = now; _sql.write("codex_active", CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, toJson(next)); }
    const auto owner = checkedOwner(_sql, current, now);
    if (owner.value.expiresAtMs - now < 120000) throw NativeAuthorityError("codex_native_owner_renewal_required", 503);
    if (request.ownerFence && (request.ownerFence->ownerId != owner.value.ownerId || request.ownerFence->ownerEpoch != owner.value.ownerEpoch)) throw NativeAuthorityError("codex_owner_fence_stale");
    current.routingGeneration = *generation; current.activePoolVersionstamp = rows.poolRevision; request.codex = current; request.ownerFence = owner.value; _api.save(request, json::object{{"ownerFence", owner.raw}}); const auto saved = _sql.read("request_lifetime", {handle}, now).value.value(); return {{"kind", "current"}, {"detail", *rt::get(saved, "codex")}};
  }
  NativeReply NativeAuthorityCodex::elect(const NativeCapability& cap, const NativeArguments::electCodexResetProbe& args, double now) {
    const auto validated = _api.validated(cap, args.handle, now); auto request = static_cast<const NativeRequest&>(validated); const auto rawProof = _sql.read("codex_reset", {args.proofId}, now).value;
    const auto proof = rawProof && !rawProof->is_null() ? std::optional<NativeResetProof>(json::value_to<NativeResetProof>(*rawProof)) : std::nullopt;
    if (!proof || !proof->verified) return {{"kind", "refused"}, {"detail", "reset_proof_unavailable"}};
    const rt::Nullable<std::string> requestedModel(validated.model);
    if (capacity::quotaClass(proof->account.requestedModel) != capacity::quotaClass(requestedModel)) return {{"kind", "refused"}, {"detail", "reset_model_class_mismatch"}};
    const auto rows = _rows(now); auto account = proof->account; account.requestedModel = validated.model; const auto* slot = rt::at(rows.normalized.slots, account.slot); const auto* identity = identityFor(rows.pool, account.slot);
    if (rows.poolRevision != proof->poolRevision || rows.capacity.revision != proof->capacityRevision || !mutation::isCodexActiveAccountSnapshotCurrent(rows.active.value.value_or(nullptr), {proof->activeSnapshot}) || !identity || identity->accountIdHash != account.accountIdHash || identity->credentialVersion != account.credentialVersion || !selected(rows.pool, account.accountIdHash) || !slot || !mutation::resetCandidateSlotMatches(*slot, account, mutation::isCodexQuotaBlockFenceCurrent(rows.routing.value.value_or(nullptr), account, proof->quotaResetAtMs, proof->routingGeneration))) return {{"kind", "refused"}, {"detail", "reset_fence_stale"}};
    const auto cohort = native_codex::selectCodex({rows.active.value.value_or(nullptr), rows.routing.value.value_or(nullptr), rows.capacity.value.value_or(nullptr), rows.pool.identities, rows.pool.selectedAccountHashes, _policies(rows.pool, validated.model), rows.revisions}, now, validated.model);
    const auto* blocked = cohort ? std::get_if<CodexRouteSelectionQuotaBlocked>(&cohort->selection) : nullptr;
    if (!blocked || !blocked->fullCohortExhausted) return {{"kind", "refused"}, {"detail", "cohort_not_quota_exhausted"}};
    const auto quotaClass = capacity::quotaClass(account.requestedModel);
    const auto scope = capacity::quotaBlockKeyForClass(*slot, quotaClass, capacity::requestedModelView(account.requestedModel));
    if (scope && isCodexCapacityModelScope(*scope) && !capacity::codexQuotaScopeApplies(*scope, capacity::requestedModelView(proof->account.requestedModel)))
      return {{"kind", "refused"}, {"detail", "reset_model_class_mismatch"}};
    const auto active = parseCodexActiveAccountSelection(rows.active.value.value_or(nullptr)); const auto next = native_codex::activeSelectionForAccount(account, rows.poolRevision, mutation::nextResetElectionGeneration(active, account), now, std::string("quota_exhausted"));
    if (!next) return {{"kind", "refused"}};
    auto election = native_codex::planCodexResetElection(rows.revisions, rows.parsed ? &*rows.parsed : nullptr, rows.normalized, *next);
    // The source compares raw stored slots with parseSlot copies by object identity here.
    if (rows.routing.value && !rows.normalized.slots.empty()) election.writes.routing = rows.normalized;
    if (election.writes.routing) _sql.write("codex_routing", CODEX_ACCOUNT_ROUTING_KV_KEY, toJson(*election.writes.routing));
    _sql.write("codex_active", CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, toJson(election.writes.active));
    const CodexProbeLease lease{rt::crypto::randomUUID(), now + CODEX_HALF_OPEN_LEASE_MS, slot->generation + 1, "quota",
                                scope && isCodexCapacityModelScope(*scope) ? *scope : quotaClass};
    const auto recovered = mutation::recoveryProbeTransition(rows.normalized, account, lease, now); _sql.write("codex_routing", CODEX_ACCOUNT_ROUTING_KV_KEY, toJson(recovered));
    account.routingGeneration = lease.generation; account.activeGeneration = next->generation; account.activePoolVersionstamp = next->pool_versionstamp; account.activeTransitionReason = rt::orNull(next->transition_reason); account.probeRequired = false; account.probeGeneration = lease.generation; account.probeToken = lease.token; account.probeCircuit = lease.circuit;
    const auto owner = checkedOwner(_sql, account, now); request.codex = account; request.ownerFence = owner.value;
    const auto admitted = rt::objectSpread(*rt::getObject(*rawProof, "account"), member(request, "codex").as_object());
    _api.save(request, json::object{{"codex", admitted}, {"ownerFence", owner.raw}}); auto consumed = rawProof->as_object(); consumed["verified"] = false; _sql.write("codex_reset", {proof->id}, consumed); return {{"kind", "probe_elected"}, {"detail", admitted}};
  }
}
