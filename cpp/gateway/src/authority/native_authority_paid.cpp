// Port of src/authority/native-authority-paid.ts.
#include "uos/authority/native_authority_paid.hpp"
#include "uos/authority/native_codex.hpp"
#include "uos/rt/crypto.hpp"
#include <algorithm>

namespace uos::authority::native_authority_paid {
  using namespace native_authority_contracts;
  using namespace native_paid_contracts;
  namespace paid = native_paid;
  namespace {
    template<class T> NativePaidRow<T> read(const native_authority_sql::NativeAuthoritySql& sql, const std::string& table, const NativeKey& key, double now) {
      const auto row = sql.readAs<T>(table, key, now); return {row.value, row.versionstamp, rt::orNull(row.expiresAtMs)};
    }
    NativePaidRow<PaidFallbackReconciliationGateV3> typedGate(const NativePaidRow<json::value>& row) {
      const auto value = row.value && paid::isPaidFallbackReconciliationGate(*row.value) ? std::optional<PaidFallbackReconciliationGateV3>(json::value_to<PaidFallbackReconciliationGateV3>(*row.value)) : std::nullopt;
      return {value, row.versionstamp, row.expiresAtMs};
    }
    bool fullUnavailable(const std::optional<native_codex_contracts::CodexRouteSelection>& selection) {
      if (!selection) return false;
      return std::visit([](const auto& value) { using T = std::decay_t<decltype(value)>; if constexpr (std::is_same_v<T, native_codex_contracts::CodexRouteSelectionQuotaBlocked> || std::is_same_v<T, native_codex_contracts::CodexRouteSelectionCredentialsInvalid>) return value.fullCohortUnavailable == true; else return false; }, *selection);
    }
    bool includes(const std::vector<std::string>& values, const std::string& value) { return std::find(values.begin(), values.end(), value) != values.end(); }
    void writeWindow(native_authority_sql::NativeAuthoritySql& sql, const NativeKey& key, const PaidFallbackWindowV3& window, double expiry, const std::string& keyId) {
      const auto old = sql.read("paid_window", key, 0).value.value_or(json::object()); const auto value = json::value_from(window);
      sql.write("paid_window", key, rt::objectSpread(old.as_object(), value.as_object()), expiry, {.scope = keyId});
    }
  }
  NativeProviderHop NativeAuthorityPaid::_hop(const std::string& id) const {
    const auto config = _sql.readAs<NativeAuthorityConfig>("authority_config", {"config"}, 0).value;
    if (config) for (const auto& hop : config->providerHops) if (hop.id == id) return hop;
    throw NativeAuthorityError("provider_hop_not_configured", 403);
  }
  NativeAttempt NativeAuthorityPaid::attempt(const NativeCapability& cap, const std::string& handle, double now) const {
    const auto row = _sql.read("attempt_lifetime", {handle}, now).value.value_or(nullptr);
    if (rt::getString(row, "principalId") != cap.principalId || rt::getString(row, "keyId") != nativeApiKeyId(cap) || rt::getString(row, "runtimeBindingId") != cap.id) throw NativeAuthorityError("attempt_ownership_mismatch", 403);
    const auto result = json::value_to<NativeAttempt>(row); _api.request(cap, result.requestHandle, now); return result;
  }
  void NativeAuthorityPaid::_saveAttempt(const NativeAttempt& row) {
    const auto old = _sql.read("attempt_lifetime", {row.handle}, 0).value.value_or(json::object()); const auto encoded = json::value_from(row);
    _sql.write("attempt_lifetime", {row.handle}, rt::objectSpread(old.as_object(), encoded.as_object()), std::nullopt, {.scope = row.keyId, .parent = row.requestHandle});
  }
  NativePaidRow<PaidFallbackRequestV3> NativeAuthorityPaid::_request(const NativeAttempt& attemptValue, double now) const { return read<PaidFallbackRequestV3>(_sql, "paid_request", paid::paidFallbackRequestV3Key(attemptValue.keyId, attemptValue.billingRequestId), now); }
  NativePaidRow<json::value> NativeAuthorityPaid::_gate(double now) const { const auto row = _sql.read("paid_gate", paid::paidFallbackReconciliationGateV3Key(), now); return {row.value, row.versionstamp, rt::orNull(row.expiresAtMs)}; }
  void NativeAuthorityPaid::_writeRequest(const NativeKey& key, const PaidFallbackRequestV3& row, double expiry) {
    const auto old = _sql.read("paid_request", key, 0).value.value_or(json::object()); const auto encoded = json::value_from(row);
    _sql.write("paid_request", key, rt::objectSpread(old.as_object(), encoded.as_object()), expiry, {.scope = row.key_id});
  }
  void NativeAuthorityPaid::_arm(double now) { _sql.write("paid_gate", paid::paidFallbackReconciliationGateV3Key(), paid::paidFallbackReconciliationGateDueNow(_gate(now), now)); }
  std::optional<NativeRead<NativeKeyPolicy>> NativeAuthorityPaid::_policy(const NativeCapability& cap, double now) const {
    if (!cap.keyId || !cap.tokenHash) return std::nullopt;
    const auto source = _sql.currentKeyPolicy(cap.tokenHash, nativeApiKeyId(cap), now);
    if (!source || !source->value || source->value->revoked || source->value->api.key_id != nativeApiKeyId(cap) || !source->value->api.paid_fallback_enabled || (source->value->api.expires_at_ms != -1 && source->value->api.expires_at_ms <= now)) return std::nullopt;
    return source;
  }
  bool NativeAuthorityPaid::_fallbackCurrent(const NativeRequest& request, double now) const {
    using namespace native_codex_contracts;
    if (!request.codexProof || !request.model) return false;
    const auto pool = _sql.readAs<NativePool>("codex_pool", CODEX_AUTH_POOL_KV_KEY, now); const auto active = _sql.read("codex_active", CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, now), routing = _sql.read("codex_routing", CODEX_ACCOUNT_ROUTING_KV_KEY, now), capacity = _sql.read("codex_capacity", CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY, now);
    const auto& proof = *request.codexProof;
    if (proof.pool != pool.revision || proof.active != active.revision || proof.routing != routing.revision || proof.capacity != capacity.revision || !pool.value || !pool.revision) return false;
    const auto* policies = pool.value->modelPolicies ? rt::getObject(*pool.value->modelPolicies, *request.model) : nullptr; if (!policies) policies = &pool.value->policies;
    rt::OrderedMap<std::string, CodexRoutingAccountPolicy> mapped;
    for (const auto& key : rt::objectKeys(*policies)) {
      const auto& value = policies->at(key); CodexRoutingAccountPolicy policy; const auto* unavailable = rt::get(value, "modelUnavailable");
      if (unavailable && !unavailable->is_null()) policy.modelUnavailable = CodexRoutingAccountModelUnavailable{rt::getString(*unavailable, "detail") ? std::optional<std::string>(std::string(*rt::getString(*unavailable, "detail"))) : std::nullopt};
      policy.overageUsageAllowed = rt::getBool(value, "overageUsageAllowed").value_or(false); mapped.set(key, policy);
    }
    const auto decision = native_codex::selectCodex({active.value.value_or(nullptr), routing.value.value_or(nullptr), capacity.value.value_or(nullptr), pool.value->identities, pool.value->selectedAccountHashes, mapped, {active.revision, pool.revision, routing.revision, capacity.revision}}, now, request.model);
    return decision && fullUnavailable(decision->selection);
  }
  bool NativeAuthorityPaid::_fallbackAllowed(const NativeRequest& request, double now) const { return fullUnavailable(request.selection) && _fallbackCurrent(request, now); }
  NativeAuthorityPaid::Prior NativeAuthorityPaid::_prior(const NativeRequest& request, const std::string& hopId, const std::string& billingId, double now) const {
    const auto prior = _sql.paidAttempt(nativeApiKeyId(NativeApiKeyIdentity{request.keyId}), billingId, now); std::optional<NativeReply> blocked;
    if (prior && (prior->requestHandle != request.handle || prior->hopId != hopId)) blocked = NativeReply{{"kind", "blocked"}, {"detail", "attempt_identity_conflict"}};
    else if (prior && !_request(*prior, now).value) blocked = NativeReply{{"kind", "blocked"}, {"detail", "attempt_retention_expired"}};
    return {prior, blocked};
  }
  NativeAttempt NativeAuthorityPaid::_requireAttempt(const std::optional<NativeAttempt>& prior) const { if (!prior) throw NativeAuthorityError("paid_attempt_lifetime_missing", 503); return *prior; }
  NativeReply NativeAuthorityPaid::reserve(const NativeCapability& cap, const NativeArguments::reservePaidAttempt& args, double now) {
    const auto request = _api.validated(cap, args.handle, now); const auto hop = _hop(args.hopId); const native_runtime_contracts::NativeRoutingHop* routingHop = nullptr;
    if (request.routingDecision) for (const auto& candidate : request.routingDecision->hops) if (candidate.hopId == hop.id && candidate.authorization == hop.authorization) { routingHop = &candidate; break; }
    if (!routingHop) return {{"kind", "blocked"}, {"detail", "request_hop_not_authorized"}};
    if (hop.authorization == "codex_unavailable" && !_fallbackAllowed(request, now)) return {{"kind", "blocked"}, {"detail", "subscriptions_not_authoritatively_unavailable"}};
    const auto source = _policy(cap, now);
    if (!source || !source->value) return {{"kind", "blocked"}, {"detail", "invalid_policy"}};
    const auto& policy = *source->value; const bool unlimited = policy.api.paid_fallback_limit_microcredits == -1;
    if (!includes(policy.paid.eligibleModels, routingHop->billingModel) && !(unlimited && policy.paid.allowUnrosteredUnlimited && request.routingDecision->allowUnrosteredModel)) return {{"kind", "skip"}, {"detail", "model_not_priced"}};
    const auto billingId = request.requestId + ":" + hop.billingSuffix; const auto prior = _prior(request, hop.id, billingId, now);
    if (prior.blocked) return *prior.blocked;
    const auto reset = native_api_key::currentApiKeyUsageWindow(policy.api.usage_reset_at_ms, policy.api.window_ms, request.createdAtMs).reset;
    const NativePaidAdmissionInput input{nativeApiKeyId(cap), billingId, request.createdAtMs, policy.paid.policyVersion, policy.api.paid_fallback_limit_microcredits, rt::getNumber(policy.paid.modelExposure, routingHop->billingModel), 0, policy.paid.quotaPerCredit, reset, routingHop->billingModel, request.route, request.path, request.stream, request.reasoning, true};
    if (paid::isInvalidAdmissionPolicy(input, unlimited, input.maximumExposureMicrocredits)) return {{"kind", "blocked"}, {"detail", "invalid_policy"}};
    const auto requestKey = paid::paidFallbackRequestV3Key(nativeApiKeyId(cap), billingId), windowKey = paid::paidFallbackWindowV3Key(nativeApiKeyId(cap), reset);
    const auto plan = paid::planPaidFallbackAdmissionV3(input, {read<PaidFallbackRequestV3>(_sql, "paid_request", requestKey, now), read<PaidFallbackWindowV3>(_sql, "paid_window", windowKey, now), read<PaidFallbackDeletionGuardV3>(_sql, "paid_deletion_guard", paid::paidFallbackDeletionGuardV3Key(nativeApiKeyId(cap)), now), NativePaidAdmissionSnapshot::PolicyFence{source->revision}}, {unlimited, input.maximumExposureMicrocredits}, now);
    if (const auto* exhausted = std::get_if<NativePaidAdmissionPlanExhaustedPolicy>(&plan)) { writeWindow(_sql, windowKey, exhausted->window, now + exhausted->windowExpireIn, nativeApiKeyId(cap)); return {{"kind", "blocked"}, {"detail", "limit_exceeded"}}; }
    if (const auto* blocked = std::get_if<NativePaidAdmissionPlanBlocked>(&plan)) return {{"kind", "blocked"}, {"detail", blocked->reason}};
    if (const auto* reserved = std::get_if<NativePaidAdmissionPlanReserved>(&plan)) return {{"kind", "reserved"}, {"attemptHandle", _requireAttempt(prior.prior).handle}, {"detail", json::value_from(reserved->reservation)}};
    const auto& write = std::get<NativePaidAdmissionPlanWrite>(plan); _writeRequest(requestKey, write.request, now + write.requestExpireIn);
    _sql.write("paid_pending", paid::paidFallbackPendingV3Key(nativeApiKeyId(cap), billingId), write.pending, std::nullopt, {.scope = nativeApiKeyId(cap), .due = write.pending.next_reconciliation_at_ms});
    if (write.checkGate) _arm(now);
    if (write.window && write.windowExpireIn) writeWindow(_sql, windowKey, *write.window, now + *write.windowExpireIn, nativeApiKeyId(cap));
    const auto handle = rt::crypto::randomUUID(); _saveAttempt({handle, cap.id, request.handle, cap.principalId, nativeApiKeyId(cap), hop.id, billingId, reset, false});
    return {{"kind", "reserved"}, {"attemptHandle", handle}, {"detail", json::value_from(write.reservation)}};
  }
  void NativeAuthorityPaid::_expedite(const NativeAttempt& attemptValue, double now) {
    const auto key = paid::paidFallbackPendingV3Key(attemptValue.keyId, attemptValue.billingRequestId); const auto pending = _sql.readAs<PaidFallbackPendingV3>("paid_pending", key, now).value;
    if (!pending) return;
    const bool expedite = pending->next_reconciliation_at_ms > now;
    if (!expedite && !paid::paidFallbackReconciliationGateNeedsArm(_gate(now), now)) return;
    if (expedite) { const auto raw = _sql.read("paid_pending", key, now).value.value(); _sql.write("paid_pending", key, rt::objectSpread(raw.as_object(), json::object{{"next_reconciliation_at_ms", rt::makeNumber(now)}}), std::nullopt, {.scope = attemptValue.keyId, .due = now}); }
    _arm(now);
  }
  NativePaidResponse NativeAuthorityPaid::response(const NativeCapability& cap, const std::string& attemptHandle, double now) const { const auto value = attempt(cap, attemptHandle, now); return {value, _hop(value.hopId).ledgerProvider}; }
  std::optional<NativeAttempt> NativeAuthorityPaid::assertStart(const NativeCapability& cap, const NativeArguments::recordTransportStarted& args, double now) const {
    if (!args.attemptHandle || args.attemptHandle->empty()) { if (args.provider == "metered" || args.provider == "surplus") throw NativeAuthorityError("paid_transport_attempt_required", 403); return std::nullopt; }
    const auto value = response(cap, *args.attemptHandle, now);
    if (value.attempt.requestHandle != args.handle || value.provider != args.provider) throw NativeAuthorityError("transport_attempt_mismatch", 403);
    const auto row = _request(value.attempt, now).value;
    if (!row || row->billing_state != "pending" || row->terminal_state != "pending") throw NativeAuthorityError("paid_attempt_not_dispatchable");
    return value.attempt;
  }
  void NativeAuthorityPaid::start(const NativeCapability& cap, const NativeArguments::recordTransportStarted& args, double now) { const auto value = assertStart(cap, args, now); if (value) lifecycle(cap, {value->handle, {}, true}, now); }
  void NativeAuthorityPaid::_assertFetchStart(const NativeCapability& cap, const NativeAttempt& attemptValue, std::optional<bool> fetchStarted, double now) const { if (fetchStarted == true && !attemptValue.fetchStarted && !_policy(cap, now)) throw NativeAuthorityError("api_policy_revoked", 403); }
  NativeReply NativeAuthorityPaid::lifecycle(const NativeCapability& cap, const NativeArguments::recordPaidAttemptLifecycle& args, double now) {
    auto value = attempt(cap, args.attemptHandle, now); const auto hop = _hop(value.hopId); const auto entry = _request(value, now); _assertFetchStart(cap, value, args.fetchStarted, now);
    if (!entry.value) return {{"kind", "missing"}};
    PaidFallbackRequestLifecyclePatchV3 patch; patch.provider = hop.ledgerProvider; patch.provider_request_id = args.patch.provider_request_id; patch.dispatch_state = args.patch.dispatch_state; patch.terminal_state = args.patch.terminal_state; if (args.patch.terminal_state) patch.increment_reconciliation_attempts = true;
    const auto plan = paid::planPaidFallbackLifecycleV3(*entry.value, patch, now);
    if (plan) { _writeRequest(paid::paidFallbackRequestV3Key(nativeApiKeyId(cap), value.billingRequestId), plan->request, now + plan->requestExpireIn); if (plan->readGate && (plan->dispatchBoundary || paid::paidFallbackReconciliationGateNeedsArm(_gate(now), now))) _arm(now); }
    if (args.fetchStarted == true) { value.fetchStarted = true; _saveAttempt(value); auto original = _api.request(cap, value.requestHandle, now); if (original.dispatch) { original.dispatch->fetchStarted = true; _api.save(original); } }
    const auto observed = plan ? plan->request : *entry.value;
    if (observed.terminal_state != "pending") _expedite(value, now);
    const bool terminal = args.patch.terminal_state && *args.patch.terminal_state != "pending";
    if (observed.provider_request_id || terminal || args.fetchStarted == true) { auto original = _api.request(cap, value.requestHandle, now); if (!original.semantic) { original.providerObserved = true; original.responding = NativeRequestResponding{hop.ledgerProvider, value.handle}; _api.save(original); } }
    return {{"kind", plan ? "updated" : "unchanged"}, {"detail", json::value_from(observed)}};
  }
  void NativeAuthorityPaid::_applyRelease(const NativeAttempt& value, const NativePaidReleasePlan& plan, double now) {
    _writeRequest(paid::paidFallbackRequestV3Key(value.keyId, value.billingRequestId), plan.request, now + plan.requestExpireIn); _sql.delete_("paid_pending", paid::paidFallbackPendingV3Key(value.keyId, value.billingRequestId)); _sql.write("paid_gate", paid::paidFallbackReconciliationGateV3Key(), plan.gate);
    if (plan.window && plan.windowExpireIn) writeWindow(_sql, paid::paidFallbackWindowV3Key(value.keyId, value.windowResetAtMs), *plan.window, now + *plan.windowExpireIn, value.keyId);
  }
  NativeReply NativeAuthorityPaid::release(const NativeCapability& cap, const std::string& handle, double now) {
    const auto value = attempt(cap, handle, now); const auto original = _api.request(cap, value.requestHandle, now); const auto current = _request(value, now).value;
    const bool billable = current && (current->billing_state == "pending" || current->billing_state == "unresolved");
    if (billable && (value.fetchStarted || current->terminal_state != "pending" || original.semantic || (original.delivery && *original.delivery != "unobserved"))) throw NativeAuthorityError("paid_fetch_may_have_started");
    const auto plan = paid::planPaidFallbackReleaseV3({_request(value, now), read<PaidFallbackWindowV3>(_sql, "paid_window", paid::paidFallbackWindowV3Key(nativeApiKeyId(cap), value.windowResetAtMs), now), typedGate(_gate(now))}, true, now);
    if (!plan) return {{"kind", "unchanged"}};
    _applyRelease(value, *plan, now); return {{"kind", "released"}, {"detail", json::value_from(plan->request)}};
  }
  void NativeAuthorityPaid::_cleanup(const NativeAttempt& value, double now) { _sql.delete_("paid_pending", paid::paidFallbackPendingV3Key(value.keyId, value.billingRequestId)); _arm(now); }
  NativeReply NativeAuthorityPaid::defer(const NativeCapability& cap, const std::string& handle, double now) {
    const auto value = attempt(cap, handle, now); const auto key = paid::paidFallbackPendingV3Key(nativeApiKeyId(cap), value.billingRequestId);
    const auto plan = paid::planPaidFallbackDeferralV3(_request(value, now).value, _sql.readAs<PaidFallbackPendingV3>("paid_pending", key, now).value, now);
    const auto detail = std::visit([&](const auto& item) {
      json::object result{{"kind", item.kind}};
      if constexpr (std::is_same_v<std::decay_t<decltype(item)>, NativePaidDeferralPlanCleanup>) _cleanup(value, now);
      if constexpr (std::is_same_v<std::decay_t<decltype(item)>, NativePaidDeferralPlanWrite>) { _writeRequest(paid::paidFallbackRequestV3Key(nativeApiKeyId(cap), value.billingRequestId), item.request, now + item.requestExpireIn); _sql.write("paid_pending", key, item.pending, std::nullopt, {.scope = nativeApiKeyId(cap), .due = item.pending.next_reconciliation_at_ms}); result["request"] = json::value_from(item.request); result["pending"] = json::value_from(item.pending); result["delay"] = rt::makeNumber(item.delay); result["requestExpireIn"] = rt::makeNumber(item.requestExpireIn); }
      result["checks"] = json::value_from(item.checks); return result;
    }, plan);
    return {{"kind", detail.at("kind")}, {"detail", detail}};
  }
  NativeReply NativeAuthorityPaid::billing(const NativeCapability& cap, const NativeArguments::recordVerifiedPaidBilling& args, double now) {
    const auto value = attempt(cap, args.attemptHandle, now); const auto hop = _hop(value.hopId); const auto request = _request(value, now); const auto pending = read<PaidFallbackPendingV3>(_sql, "paid_pending", paid::paidFallbackPendingV3Key(nativeApiKeyId(cap), value.billingRequestId), now);
    const auto evaluation = paid::evaluatePaidFallbackSettlementV3(request.value, pending.value, args.log, {hop.billingCorrelation, hop.microcreditsPerCredit, args.surplusBuyerCostMicro});
    if (std::holds_alternative<NativePaidSettlementEvaluationCleanup>(evaluation)) { _cleanup(value, now); return {{"kind", "cleanup"}}; }
    if (std::holds_alternative<NativePaidSettlementEvaluationDefer>(evaluation)) return defer(cap, args.attemptHandle, now);
    const auto* settle = std::get_if<NativePaidSettlementEvaluationSettle>(&evaluation);
    if (!settle) return {{"kind", std::visit([](const auto& item) { return std::string(item.kind); }, evaluation)}};
    if (!request.value) throw NativeAuthorityError("paid_request_missing", 503);
    const auto identity = paid::paidFallbackRequestRollupIdentity(*request.value, hop.billingCorrelation); const auto key = paid::paidFallbackUsageRollupKey(identity.bucketStartAtMs, identity.model, identity.provider, identity.shard);
    const auto raw = _sql.read("paid_rollup", key, now); const auto rollup = raw.value && paid::isPaidFallbackUsageRollup(*raw.value) ? std::optional<PaidFallbackUsageRollup>(json::value_to<PaidFallbackUsageRollup>(*raw.value)) : std::nullopt;
    const auto folded = paid::paidFallbackSettlementRollupV3(*request.value, args.log, settle->spend, hop.billingCorrelation, rollup, now);
    const auto plan = paid::planPaidFallbackSettlementRowsV3({request, pending, typedGate(_gate(now)), read<PaidFallbackWindowV3>(_sql, "paid_window", paid::paidFallbackWindowV3Key(nativeApiKeyId(cap), value.windowResetAtMs), now), {rollup, raw.versionstamp, rt::orNull(raw.expiresAtMs)}}, args.log, settle->spend, folded.foldIntoRollup, now);
    _applyRelease(value, {plan.request, plan.window, plan.gate, plan.requestExpireIn, plan.windowExpireIn, plan.checks}, now);
    if (folded.foldIntoRollup) _sql.write("paid_rollup", key, folded.rollup);
    return {{"kind", "settled"}, {"detail", json::value_from(plan.request)}};
  }
  bool NativeAuthorityPaid::_ownsCursor(const NativeCapability& cap, const std::optional<NativeCursor>& row) const {
    if (!row || row->capabilityId != cap.id || row->parent != nativeApiKeyId(cap)) return false;
    if (!cap.retainedRequestHandles) return !row->retainedGrantId && !row->retainedHandles;
    return row->retainedGrantId == cap.runtimeGrantId && row->retainedHandles == cap.retainedRequestHandles;
  }
  std::string NativeAuthorityPaid::_resume(const NativeCapability& cap, const std::optional<std::string>& cursor, const std::string& family, double now) const {
    if (!cursor || cursor->empty()) return "";
    const auto row = _sql.readAs<NativeCursor>("cursor", {*cursor}, now).value;
    if (!_ownsCursor(cap, row) || row->family != family) throw NativeAuthorityError("cursor_ownership_mismatch", 403);
    return row->after;
  }
  void NativeAuthorityPaid::_cursorScope(const NativeCapability& cap, NativeCursor& row) const { if (cap.retainedRequestHandles) { row.retainedGrantId = cap.runtimeGrantId; row.retainedHandles = cap.retainedRequestHandles; } }
  std::string NativeAuthorityPaid::_cursor(const NativeCapability& cap, const std::string& family, const std::string& after, const std::optional<std::string>& lease) {
    const auto id = rt::crypto::randomUUID(); NativeCursor row{id, cap.id, family, after, nativeApiKeyId(cap), lease}; _cursorScope(cap, row); _sql.write("cursor", {id}, row); return id;
  }
  std::optional<json::object> NativeAuthorityPaid::_dueAttempt(const NativeCapability& cap, const std::string& billingId, double now) {
    const auto value = _sql.paidAttempt(nativeApiKeyId(cap), billingId, now);
    if (cap.retainedRequestHandles && (!value || value->principalId != cap.principalId || value->runtimeBindingId != cap.id || !includes(*cap.retainedRequestHandles, value->requestHandle))) return std::nullopt;
    const auto request = _sql.readAs<PaidFallbackRequestV3>("paid_request", paid::paidFallbackRequestV3Key(nativeApiKeyId(cap), billingId), now).value;
    if (!request || request->billing_state == "settled" || request->billing_state == "not_billed") { _sql.delete_("paid_pending", paid::paidFallbackPendingV3Key(nativeApiKeyId(cap), billingId)); _arm(now); return std::nullopt; }
    if (!value) throw NativeAuthorityError("paid_attempt_lifetime_missing", 503);
    if (value->principalId != cap.principalId || value->runtimeBindingId != cap.id) return std::nullopt;
    if (cap.retainedRequestHandles && !includes(*cap.retainedRequestHandles, value->requestHandle)) return std::nullopt;
    attempt(cap, value->handle, now); json::object result{{"attemptHandle", value->handle}, {"requestHandle", value->requestHandle}, {"requestId", billingId}};
    rt::objectSet(result, "provider", request->provider); result["providerRequestId"] = request->provider_request_id ? json::value(*request->provider_request_id) : json::value(nullptr); return result;
  }
  NativeReply NativeAuthorityPaid::claim(const NativeCapability& cap, const NativeArguments::claimPaidReconciliation& args, double now) {
    const auto key = paid::paidFallbackReconciliationLeaseV3Key(nativeApiKeyId(cap)); const auto prior = _sql.readAs<PaidFallbackReconciliationLeaseV3>("paid_lease", key, now).value;
    if (paid::paidFallbackLeaseActiveV3(prior, now)) return {{"kind", "leased"}};
    const auto token = rt::crypto::randomUUID(); const auto lease = paid::paidFallbackReconciliationLeaseV3(token, now); _sql.write("paid_lease", key, lease, lease.expires_at_ms, {.scope = nativeApiKeyId(cap)});
    NativeCursor owner{token, cap.id, "paid_lease", "", nativeApiKeyId(cap), token}; _cursorScope(cap, owner); _sql.write("cursor", {token}, owner);
    const auto page = _sql.pageAs<PaidFallbackPendingV3>("paid_pending", now, _resume(cap, args.cursor, "paid_pending", now), args.limit, {.scope = nativeApiKeyId(cap)}); json::array due;
    for (const auto& item : page) if (item.row.value && item.row.value->next_reconciliation_at_ms <= now) {
      const auto* billingId = item.key.empty() ? nullptr : std::get_if<std::string>(&item.key.back().value);
      if (!billingId) throw NativeAuthorityError("paid_pending_key_malformed", 503);
      const auto entry = _dueAttempt(cap, *billingId, now); if (entry) due.push_back(*entry);
    }
    return {{"kind", "claimed"}, {"lease", token}, {"cursor", static_cast<double>(page.size()) == args.limit ? json::value(_cursor(cap, "paid_pending", page.back().encoded, token)) : json::value(nullptr)}, {"detail", due}};
  }
  NativeReply NativeAuthorityPaid::finish(const NativeCapability& cap, const std::string& token, double now) {
    const auto owner = _sql.readAs<NativeCursor>("cursor", {token}, now).value; const auto key = paid::paidFallbackReconciliationLeaseV3Key(nativeApiKeyId(cap)); const auto lease = _sql.readAs<PaidFallbackReconciliationLeaseV3>("paid_lease", key, now).value;
    if (!_ownsCursor(cap, owner) || owner->family != "paid_lease" || !paid::paidFallbackLeaseOwnedV3(lease, token) || !paid::paidFallbackLeaseActiveV3(lease, now)) throw NativeAuthorityError("reconciliation_lease_stale");
    const auto plan = paid::planPaidFallbackGateAfterScanV3(_gate(now), _sql.earliestPaidDue(now)); _sql.write("paid_gate", paid::paidFallbackReconciliationGateV3Key(), plan.gate); _sql.delete_("paid_lease", key); return {{"kind", "finished"}};
  }
  double NativeAuthorityPaid::_backfillRow(const NativeKey& key, const PaidFallbackRequestV3& request, double now) {
    if (paid::paidFallbackRequestBackfillStateV3(request) == "retention") { _writeRequest(key, request, now + paid::requestRowExpireIn(request, now)); return 0; }
    const auto id = paid::paidFallbackRequestRollupIdentity(request); const auto rollupKey = paid::paidFallbackUsageRollupKey(id.bucketStartAtMs, id.model, id.provider, id.shard); const auto old = _sql.read("paid_rollup", rollupKey, now).value;
    const auto plan = paid::planPaidFallbackBackfillFoldV3(request, old && paid::isPaidFallbackUsageRollup(*old) ? std::optional<PaidFallbackUsageRollup>(json::value_to<PaidFallbackUsageRollup>(*old)) : std::nullopt, now);
    _writeRequest(key, plan.request, now + plan.requestExpireIn);
    if (!plan.rollup) return 0;
    _sql.write("paid_rollup", rollupKey, *plan.rollup); return 1;
  }
  NativeReply NativeAuthorityPaid::backfill(const NativeCapability& cap, const NativeArguments::foldPaidBackfillPage& args, double now) {
    const auto key = paid::paidFallbackBackfillLeaseV3Key();
    if (paid::paidFallbackLeaseActiveV3(_sql.readAs<PaidFallbackBackfillLeaseV3>("paid_backfill_lease", key, now).value, now)) return {{"kind", "leased"}};
    const auto lease = paid::paidFallbackBackfillLeaseV3(rt::crypto::randomUUID(), now); _sql.write("paid_backfill_lease", key, lease, lease.expires_at_ms);
    const auto page = _sql.pageAs<PaidFallbackRequestV3>("paid_request", now, _resume(cap, args.cursor, "paid_backfill", now), args.limit, {.scope = nativeApiKeyId(cap)}); double folds = 0;
    for (const auto& item : page) if (item.row.value) folds += _backfillRow(item.key, *item.row.value, now);
    _sql.delete_("paid_backfill_lease", key); return {{"kind", "backfilled"}, {"cursor", static_cast<double>(page.size()) == args.limit ? json::value(_cursor(cap, "paid_backfill", page.back().encoded, std::nullopt)) : json::value(nullptr)}, {"detail", json::object{{"scanned", page.size()}, {"folds", rt::makeNumber(folds)}}}};
  }
  PaidFallbackOutstandingV3 NativeAuthorityPaid::_outstanding(const std::string& keyId, double now) const {
    double pending = 0, unresolved = 0, markers = 0; std::string after;
    for (;;) { const auto page = _sql.page("paid_request", now, after, 100, {.scope = keyId}); for (const auto& item : page) { const auto value = item.row.value.value_or(nullptr); if (rt::getString(value, "billing_state") == "pending") ++pending; if (rt::getString(value, "billing_state") == "unresolved") ++unresolved; } if (page.size() < 100) break; after = page.back().encoded; }
    after.clear();
    for (;;) { const auto page = _sql.page("paid_pending", now, after, 100, {.scope = keyId}); markers += static_cast<double>(page.size()); if (page.size() < 100) break; after = page.back().encoded; }
    return {pending, unresolved, markers, pending + unresolved + markers > 0};
  }
  NativePaidDeletionResult NativeAuthorityPaid::deleteForKey(const std::string& keyId, double now) {
    const auto key = paid::paidFallbackDeletionGuardV3Key(keyId); const auto raw = _sql.read("paid_deletion_guard", key, now).value;
    const auto guard = paid::planPaidFallbackDeletionGuardV3(raw ? std::optional<PaidFallbackDeletionGuardV3>(json::value_to<PaidFallbackDeletionGuardV3>(*raw)) : std::nullopt, now);
    if (guard) _sql.write("paid_deletion_guard", key, *guard, std::nullopt, {.scope = keyId});
    const auto outstanding = _outstanding(keyId, now); const auto decision = paid::planPaidFallbackDeletionV3(outstanding);
    if (!std::holds_alternative<paid::PaidFallbackDeletionPlanV3Delete>(decision)) return {"blocked", outstanding, {}};
    json::object deleted;
    for (const std::string table : {"paid_request", "paid_window", "paid_pending"}) { double count = 0; for (;;) { const auto page = _sql.page(table, now, "", 100, {.scope = keyId}); for (const auto& item : page) { _sql.delete_(table, item.key); ++count; } if (page.size() < 100) break; } deleted[table] = rt::makeNumber(count); }
    _sql.delete_("paid_lease", paid::paidFallbackReconciliationLeaseV3Key(keyId)); return {"deleted", std::nullopt, deleted};
  }
  NativeReply NativeAuthorityPaid::deletion(const NativeCapability& cap, double now) { const auto result = deleteForKey(nativeApiKeyId(cap), now); return {{"kind", result.kind}, {"detail", result.kind == "blocked" ? json::value_from(*result.outstanding) : json::value(result.deleted)}}; }
}
