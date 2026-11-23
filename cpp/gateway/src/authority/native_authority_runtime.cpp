// Port of src/authority/native-authority-runtime.ts.
#include "uos/authority/native_authority_runtime.hpp"
#include "uos/authority/native_authority_rpc.hpp"
#include "uos/authority/native_api_key_policy.hpp"
#include "uos/api_keys.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/string.hpp"
#include <algorithm>
#include <unordered_set>

namespace uos::authority::native_authority_runtime {
  using namespace native_authority_contracts;
  using namespace native_runtime_contracts;
  namespace {
    std::string principal(const NativeRuntimeIdentity& identity) {
      return std::visit([](const auto& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, NativeRuntimeIdentityKvApiKey> || std::is_same_v<T, NativeRuntimeIdentityLocalDevelopmentKey>) return "api-key:" + value.keyId;
        else if constexpr (std::is_same_v<T, NativeRuntimeIdentityGithubToken>) return "github-repo:" + rt::toLowerCase(value.owner) + "/" + rt::toLowerCase(value.repo);
        else if constexpr (std::is_same_v<T, NativeRuntimeIdentityPasskeySession>) return "passkey-user:" + value.userId;
        else if constexpr (std::is_same_v<T, NativeRuntimeIdentityDisabledLocal>) return value.bearerDigest ? "bearer-sha256:" + *value.bearerDigest : "local-auth-disabled";
        else return "auth-method:" + std::string(T::kind);
      }, identity);
    }
    bool safeInteger(const json::value* value) { return value && rt::Number::isSafeInteger(*value); }
    bool validRuntimeGrant(const json::value& value) {
      const auto* roles = rt::getArray(value, "roles");
      if (!nativeObject(value) || rt::getNumber(value, "revision") != NATIVE_RUNTIME_REVISION || !rt::getString(value, "id") || !rt::getString(value, "epoch") || !rt::getString(value, "subjectId") || !rt::getString(value, "contextBinding") || !rt::getBool(value, "revoked") || !safeInteger(rt::get(value, "expiresAtMs")) || !roles || roles->empty()) return false;
      for (const auto& role : *roles) {
        if (!role.is_string()) return false;
        bool known = false;
        for (const auto& method : native_authority_rpc::NATIVE_METHOD_ROLES) if (rt::strictEquals(role, method.value())) known = true;
        if (!known) return false;
      }
      return true;
    }
    bool validRetainedGrant(const json::value& value) {
      if (!validRuntimeGrant(value)) return false;
      for (const auto& role : *rt::getArray(value, "roles")) if (role != "transport" && role != "billing" && role != "reconcile") return false;
      for (const auto field : {"namespace", "originalBindingId", "originalPrincipalId", "keyId"}) { const auto string = rt::getString(value, field); if (!string || string->empty()) return false; }
      const auto* handles = rt::getArray(value, "requestHandles"); if (!handles || handles->empty() || handles->size() > 100) return false;
      std::unordered_set<std::string> unique;
      for (const auto& handle : *handles) if (!handle.is_string() || handle.as_string().empty() || !unique.insert(std::string(handle.as_string())).second) return false;
      return true;
    }
    NativeTrustedRoutingDecisionWithoutIntent boundDecision(const NativeTrustedRoutingDecision& value) { return {value.subjectId, value.contextBinding, value.sourceRevision, value.cohort, value.codex, value.providers, value.allowUnrosteredModel, value.hops}; }
    bool configured(const NativeTrustedRoutingDecision& decision, const std::optional<NativeAuthorityConfig>& config, bool requireModels) {
      if (!config) return false;
      return std::all_of(decision.hops.begin(), decision.hops.end(), [&](const auto& hop) { return (!requireModels || (!hop.upstreamModel.empty() && !hop.billingModel.empty())) && std::any_of(config->providerHops.begin(), config->providerHops.end(), [&](const auto& candidate) { return candidate.id == hop.hopId && candidate.authorization == hop.authorization; }); });
    }
    template<class T> std::string bytes(const T& value) { return native_authority_rpc::nativeCommandBytes(json::value_from(value)); }
  }
  void NativeAuthorityRuntime::initialize() {
    if (_initial.retainedHistory) for (const auto& grant : _initial.retainedHistory->grants) {
      if (!validRetainedGrant(json::value_from(grant)) || grant.epoch != _sql.epoch || grant.namespace_ != _sql.namespace_) throw NativeAuthorityError("invalid_retained_history_grant", 500);
      _sql.write("retained_history_grant", {grant.id}, grant);
    }
    for (const auto& grant : _initial.grants) {
      if (!validRuntimeGrant(json::value_from(grant)) || grant.epoch != _sql.epoch) throw NativeAuthorityError("invalid_runtime_grant", 500);
      _sql.write("runtime_grant", {grant.id}, grant);
    }
    for (const auto& source : _initial.records) {
      if (bytes(native_api_key_policy::apiKeyHashRecordFor(source.record)) != bytes(source.hashRecord)) throw NativeAuthorityError("runtime_source_record_mismatch", 500);
      _sql.write("api_key_id", api_keys::apiKeyIdKey(source.record.id), source.record);
      _sql.write("api_key_hash", api_keys::apiKeyHashKey(source.record.hash), source.hashRecord);
      const auto projection = _sql.read("api_policy", api_keys::apiKeyHashKey(source.record.hash), 0).value.value_or(nullptr);
      const auto* api = rt::get(projection, "api"); if (!api || rt::getString(*api, "key_id") != source.record.id) throw NativeAuthorityError("runtime_source_projection_missing", 500);
      _sql.write("api_policy", api_keys::apiKeyHashKey(source.record.hash), rt::objectSpread(projection.as_object(), json::object{{"rawSource", true}}));
    }
  }
  rt::Task<NativeRuntimeAuthorization> NativeAuthorityRuntime::verify(NativeRuntimeBinding binding, std::string intent, double now) {
    auto task = _initial.verifier.verify(binding.privateContext, intent, now); auto verified = co_await std::move(task); co_await rt::microtask();
    if (verified.intentDigest != intent || verified.contextBinding != binding.contextBinding) throw NativeAuthorityError("runtime_context_or_intent_mismatch", 403);
    NativeRuntimeAuthorization result{binding, verified}; co_return result;
  }
  NativeCapability NativeAuthorityRuntime::authorize(const NativeRuntimeAuthorization& auth, double now) {
    const auto& binding = auth.binding; const auto& verified = auth.verified;
    const auto rawGrant = _sql.read("runtime_grant", {binding.grantId}, now).value.value_or(nullptr);
    if (!validRuntimeGrant(rawGrant)) throw NativeAuthorityError("runtime_grant_revoked_or_expired", 403);
    const auto grant = json::value_to<NativeRuntimeGrant>(rawGrant);
    if (grant.id != binding.grantId || grant.epoch != _sql.epoch || grant.revoked || grant.expiresAtMs <= now) throw NativeAuthorityError("runtime_grant_revoked_or_expired", 403);
    if (grant.contextBinding != binding.contextBinding || grant.subjectId != verified.subjectId) throw NativeAuthorityError("runtime_grant_context_mismatch", 403);
    const bool keyIdentity = std::holds_alternative<NativeRuntimeIdentityKvApiKey>(verified.identity) || std::holds_alternative<NativeRuntimeIdentityLocalDevelopmentKey>(verified.identity);
    if (keyIdentity != verified.api.has_value()) throw NativeAuthorityError("runtime_api_scope_mismatch", 403);
    if (keyIdentity) { const auto id = std::visit([](const auto& identity) -> std::string { if constexpr (requires { identity.keyId; }) return identity.keyId; else return ""; }, verified.identity); if (verified.api->principalRef.keyId != id) throw NativeAuthorityError("runtime_api_scope_mismatch", 403); }
    if (verified.bindingId.empty()) throw NativeAuthorityError("runtime_binding_identity_missing", 403);
    const auto existing = _sql.readAs<NativeCapability>("capability", {verified.bindingId}, now).value;
    if (verified.api) {
      const auto& ref = verified.api->principalRef; const auto resolution = _sql.principalReference(ref.id, ref.keyId, now);
      if ((!resolution && (!existing || existing->principalRefId != ref.id)) || (resolution && (resolution->epoch != _sql.epoch || resolution->keyId != ref.keyId || resolution->principalId != ref.principalId || resolution->tokenHash != verified.api->tokenHash))) throw NativeAuthorityError("runtime_principal_reference_mismatch", 403);
    }
    const std::optional<NativeKernelBinding> kernel = verified.kernel ? std::optional<NativeKernelBinding>({verified.kernel->owner, verified.kernel->repo}) : std::nullopt;
    NativeCapability next{verified.bindingId, grant.epoch, principal(verified.identity), verified.api ? std::optional<std::string>(verified.api->principalRef.keyId) : std::nullopt, verified.api ? std::optional<std::string>(verified.api->tokenHash) : std::nullopt, kernel, 3, grant.id, grant.contextBinding, grant.subjectId, verified.api ? std::optional<std::string>(verified.api->principalRef.id) : std::nullopt, std::nullopt, grant.roles, false};
    if (existing && (existing->revoked || existing->epoch != next.epoch)) throw NativeAuthorityError("capability_revoked_or_wrong_epoch", 403);
    if (existing && bytes(*existing) != bytes(next)) throw NativeAuthorityError("runtime_binding_scope_changed", 403);
    if (!existing) _sql.write("capability", {next.id}, next);
    return next;
  }
  void NativeAuthorityRuntime::assertCurrentApi(const NativeCapability& cap, double now) const {
    if (!cap.keyId) return;
    const auto source = _sql.currentKeyPolicy(cap.tokenHash, cap.keyId, now);
    if (!source || !source->value || source->value->revoked || (source->value->api.expires_at_ms != -1 && source->value->api.expires_at_ms <= now)) throw NativeAuthorityError("api_policy_revoked", 403);
  }
  NativeReply NativeAuthorityRuntime::open(const NativeCapability& cap, const NativeArguments::openAuthenticatedRequest& args, double now) {
    if (cap.keyId) { const auto source = _sql.currentKeyPolicy(cap.tokenHash, cap.keyId, now); if (!source || !source->value || source->value->revoked || (source->value->api.expires_at_ms != -1 && source->value->api.expires_at_ms <= now)) return {{"kind", "unauthorized"}}; }
    const NativeKey key{_sql.epoch, cap.principalId, args.requestId}; const auto prior = _sql.readAs<NativeRuntimeRequestBinding>("runtime_request", key, now).value;
    if (prior) { if (prior->runtimeBindingId != cap.id || prior->route != args.route || prior->path != args.path) throw NativeAuthorityError("request_intent_conflict"); return {{"kind", "reuse"}, {"handle", prior->handle}}; }
    const auto handle = rt::crypto::randomUUID();
    const NativeRequest request{handle, cap.id, cap.principalId, cap.keyId, args.requestId, args.route, args.path, std::nullopt, std::nullopt, std::nullopt, false, cap.keyId.has_value(), cap.kernel.has_value(), now, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, false, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    _api.save(request); _sql.write("runtime_request", key, NativeRuntimeRequestBinding{handle, cap.id, args.requestId, args.route, args.path}); return {{"kind", "opened"}, {"handle", handle}};
  }
  NativeReply NativeAuthorityRuntime::metadata(const NativeCapability& cap, const NativeArguments::bindValidatedRequestMetadata& args, double now) {
    assertCurrentApi(cap, now); const auto raw = _sql.read("request_lifetime", {args.handle}, now).value.value_or(nullptr);
    if (rt::getString(raw, "runtimeBindingId") != cap.id) throw NativeAuthorityError("request_ownership_mismatch", 403);
    auto row = json::value_to<NativeRequest>(raw);
    if ((row.apiApplicable && !row.api) || (row.kernelApplicable && !row.kernel)) throw NativeAuthorityError("request_admission_phase_incomplete");
    if (row.metadataBound) { if (row.model != args.model || row.stream != args.stream || row.reasoning != args.reasoning) throw NativeAuthorityError("validated_metadata_conflict"); return {{"kind", "unchanged"}, {"handle", row.handle}}; }
    row.model = args.model; row.stream = args.stream; row.reasoning = args.reasoning; row.metadataBound = true; _api.save(row); return {{"kind", "bound"}, {"handle", row.handle}};
  }
  rt::Task<NativeTrustedRoutingDecision> NativeAuthorityRuntime::routing(NativeRuntimeBinding binding, NativeValidatedRequest request, std::string intent, double now) {
    auto task = _initial.verifier.verifyRouting(binding.privateContext, request, intent, now); auto decision = co_await std::move(task); co_await rt::microtask();
    if ((decision.codex != "eligible" && decision.codex != "not_applicable") || decision.hops.size() > 32 || std::any_of(decision.hops.begin(), decision.hops.end(), [](const auto& hop) { return hop.authorization != "direct" && hop.authorization != "codex_unavailable"; })) throw NativeAuthorityError("routing_verification_malformed", 403);
    if (decision.intentDigest != intent || decision.contextBinding != binding.contextBinding || decision.sourceRevision.empty() || decision.cohort.empty()) throw NativeAuthorityError("routing_context_or_intent_mismatch", 403);
    if (!configured(decision, _sql.readAs<NativeAuthorityConfig>("authority_config", {"config"}, now).value, true)) throw NativeAuthorityError("routing_hop_not_configured", 403);
    co_return decision;
  }
  void NativeAuthorityRuntime::assertRouting(const NativeCapability& cap, const NativeValidatedRequest& request, const NativeTrustedRoutingDecision& decision, double now) const {
    if (decision.subjectId != cap.subjectId || !request.routingDecision || bytes(*request.routingDecision) != bytes(boundDecision(decision))) throw NativeAuthorityError("trusted_routing_stale", 403);
    if (!configured(decision, _sql.readAs<NativeAuthorityConfig>("authority_config", {"config"}, now).value, false)) throw NativeAuthorityError("routing_hop_not_configured", 403);
  }
  NativeReply NativeAuthorityRuntime::bindRouting(const NativeCapability& cap, const NativeValidatedRequest& request, const NativeTrustedRoutingDecision& decision, double now) {
    assertCurrentApi(cap, now);
    if (decision.subjectId != cap.subjectId) throw NativeAuthorityError("routing_subject_mismatch", 403);
    if (!configured(decision, _sql.readAs<NativeAuthorityConfig>("authority_config", {"config"}, now).value, false)) throw NativeAuthorityError("routing_hop_not_configured", 403);
    const auto bound = boundDecision(decision);
    if (request.routingDecision && bytes(*request.routingDecision) != bytes(bound)) throw NativeAuthorityError("trusted_routing_conflict");
    if (request.routingDecision) return {{"kind", "unchanged"}, {"handle", request.handle}};
    auto next = static_cast<const NativeRequest&>(request); next.routingDecision = bound; _api.save(next); return {{"kind", "bound"}, {"handle", request.handle}};
  }
  rt::Task<VerifiedNativeRetainedHistory> NativeAuthorityRuntime::verifyRetained(NativeRetainedHistoryBinding binding, std::string intent, double now) {
    if (!_initial.retainedHistory) throw NativeAuthorityError("retained_history_unavailable", 503);
    auto task = _initial.retainedHistory->verifier.verify(binding.privateContext, intent, now); auto verified = co_await std::move(task); co_await rt::microtask();
    if (verified.intentDigest != intent || verified.contextBinding != binding.contextBinding || verified.grantId != binding.grantId) throw NativeAuthorityError("retained_context_or_intent_mismatch", 403);
    co_return verified;
  }
  NativeCapability NativeAuthorityRuntime::authorizeRetained(const NativeRetainedHistoryBinding& binding, const VerifiedNativeRetainedHistory& verified, const NativeCommand& command, double now) const {
    _sql.assertEpoch();
    if (command.epoch != _sql.epoch) throw NativeAuthorityError("authority_epoch_mismatch", 403);
    if (std::find(NATIVE_RETAINED_METHODS.begin(), NATIVE_RETAINED_METHODS.end(), command.method) == NATIVE_RETAINED_METHODS.end()) throw NativeAuthorityError("retained_operation_denied", 403);
    const auto* patch = rt::get(command.args, "patch");
    if (command.method == "recordPaidAttemptLifecycle" && (rt::truthy(rt::get(command.args, "fetchStarted")) || (patch && rt::get(*patch, "dispatch_state")))) throw NativeAuthorityError("retained_dispatch_denied", 403);
    const auto raw = _sql.read("retained_history_grant", {binding.grantId}, now).value.value_or(nullptr);
    if (!validRetainedGrant(raw)) throw NativeAuthorityError("retained_grant_denied", 403);
    const auto grant = json::value_to<NativeRetainedHistoryGrant>(raw);
    if (grant.id != binding.grantId || grant.revoked || grant.expiresAtMs <= now || grant.epoch != _sql.epoch || grant.namespace_ != _sql.namespace_) throw NativeAuthorityError("retained_grant_denied", 403);
    const VerifiedNativeRetainedHistory expected{grant.namespace_, grant.originalBindingId, grant.originalPrincipalId, grant.keyId, grant.requestHandles, grant.id, grant.epoch, grant.subjectId, grant.contextBinding, verified.intentDigest};
    if (bytes(verified) != bytes(expected) || binding.contextBinding != grant.contextBinding) throw NativeAuthorityError("retained_scope_mismatch", 403);
    for (const auto& handle : grant.requestHandles) { const auto row = _sql.read("request_lifetime", {handle}, now).value.value_or(nullptr); if (rt::getString(row, "runtimeBindingId") != grant.originalBindingId || rt::getString(row, "principalId") != grant.originalPrincipalId || rt::getString(row, "keyId") != grant.keyId) throw NativeAuthorityError("retained_history_target_mismatch", 403); }
    if (const auto handle = rt::getString(command.args, "handle"); handle && std::find(grant.requestHandles.begin(), grant.requestHandles.end(), *handle) == grant.requestHandles.end()) throw NativeAuthorityError("retained_history_target_mismatch", 403);
    if (const auto handle = rt::getString(command.args, "attemptHandle")) { const auto attempt = _sql.read("attempt_lifetime", {std::string(*handle)}, now).value.value_or(nullptr); const auto requestHandle = rt::getString(attempt, "requestHandle"); if (rt::getString(attempt, "runtimeBindingId") != grant.originalBindingId || rt::getString(attempt, "principalId") != grant.originalPrincipalId || rt::getString(attempt, "keyId") != grant.keyId || !requestHandle || std::find(grant.requestHandles.begin(), grant.requestHandles.end(), *requestHandle) == grant.requestHandles.end()) throw NativeAuthorityError("retained_history_target_mismatch", 403); }
    return {grant.originalBindingId, grant.epoch, grant.originalPrincipalId, grant.keyId, std::nullopt, std::nullopt, 3, grant.id, grant.contextBinding, grant.subjectId, std::nullopt, grant.requestHandles, grant.roles, false};
  }
}
