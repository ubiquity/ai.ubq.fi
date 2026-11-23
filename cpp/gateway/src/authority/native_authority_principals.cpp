// Port of src/authority/native-authority-principals.ts.
#include "uos/authority/native_authority_principals.hpp"
#include "uos/authority/native_authority_rpc.hpp"
#include "uos/authority/native_api_key_policy.hpp"
#include "uos/authority/native_paid_policy.hpp"
#include "uos/api_keys.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include <algorithm>
#include <unordered_map>

namespace uos::authority::native_authority_principals {
  using namespace native_authority_contracts;
  using namespace native_principal_contracts;
  namespace policy = native_api_key_policy;
  namespace {
    using Outcome = NativeControlOutcome;
    using Cleaning = std::unordered_map<std::string, rt::Promise<void>>;
    using Persist = std::function<NativeKeyCreatePreparation(NativeKeyCreatePreparation)>;
    struct Target { ::uos::types::ApiKeyRecord record; NativeRead<::uos::types::ApiKeyRecord> id; std::optional<std::string> hashRevision; NativeRead<NativeKeyPolicy> projection; };
    struct Applied { Outcome reply; std::optional<NativeControlCleanup> cleanup{}; };
    struct Commit { std::function<Applied()> apply; std::function<void()> onFailure{}; std::function<void()> prepare{}; };
    struct CreateFlight {
      std::string intent;
      std::optional<rt::Promise<Commit>> work{};
      std::optional<std::string> token{};
      std::optional<double> now{};
      std::optional<NativePrincipalControlInitial::Sources::Defaults> defaults{};
      std::optional<std::string> hash{};
      std::optional<paid_fallback::index::PaidFallbackPolicyFields> pricing{};
      bool pricingReady = false;
      std::optional<std::string> id{};
      std::optional<NativeKeyCreatePreparation> prepared{};
    };
    Outcome refusal(double status, const std::string& message, const std::string& code = "invalid_request_error") { return NativeControlOutcomeRefused{status, code, message}; }
    Outcome conflict() { return refusal(409, "API key was modified concurrently; retry"); }
    Commit fixed(Outcome reply) { return {[reply] { return Applied{reply}; }}; }
    NativeControlReply replyOf(const Outcome& value) { return std::visit([](const auto& item) -> NativeControlReply { return item; }, value); }
    template<class T> std::string bytes(const T& value) { return native_authority_rpc::nativeCommandBytes(json::value_from(value)); }
    std::string keyBytes(const NativeKey& key) { json::array values; for (const auto& part : key) values.push_back(json::value(part.string())); return native_authority_rpc::nativeCommandBytes(values); }
    bool includes(const std::vector<std::string>& values, const std::string& value) { return std::find(values.begin(), values.end(), value) != values.end(); }
    bool activeGrant(const json::value& value) {
      const auto* roles = rt::getArray(value, "roles"); const auto* targets = rt::get(value, "targetKeyIds"); const auto* expires = rt::get(value, "expiresAtMs");
      if (!nativeObject(value) || rt::getBool(value, "revoked") != false || !rt::getString(value, "id") || !rt::getString(value, "epoch") || !rt::getString(value, "subjectId") || !rt::getString(value, "contextBinding") || !expires || !rt::Number::isSafeInteger(*expires) || !roles || !targets) return false;
      for (const auto& role : *roles) if (role != "key_admin" && role != "principal_resolver") return false;
      if (targets->is_null()) return true;
      if (!targets->is_array()) return false;
      for (const auto& target : targets->as_array()) if (!target.is_string() || target.as_string().empty()) return false;
      return true;
    }
    rt::Task<std::string> digest(std::string value) { const auto hash = rt::crypto::subtle::digest("SHA-256", rt::encodeUtf8(value)); co_await rt::microtask(); co_return rt::toHex(hash); }
    std::shared_ptr<Cleaning> cleanupWorkFor(const NativePrincipalControlInitial::Sources& source) {
      static std::unordered_map<const void*, std::weak_ptr<Cleaning>> bySource;
      auto& weak = bySource[&source]; auto work = weak.lock(); if (!work) { work = std::make_shared<Cleaning>(); weak = work; } return work;
    }
    const json::object& inputOf(const NativeControlCommand& command) { return command.args.at("input").as_object(); }
    void mergePricing(paid_fallback::index::PaidFallbackPolicyFields& fields, const paid_fallback::index::InitializePaidFallbackPolicyResult& pricing) {
      fields.paid_fallback_model_ids = pricing.paid_fallback_model_ids; fields.paid_fallback_quota_per_credit = pricing.paid_fallback_quota_per_credit; fields.paid_fallback_max_exposure_microcredits = pricing.paid_fallback_max_exposure_microcredits; fields.paid_fallback_pricing_checked_at_ms = pricing.paid_fallback_pricing_checked_at_ms;
    }
  }
  struct NativeAuthorityPrincipals::State {
    native_authority_sql::NativeAuthoritySql& _sql;
    native_authority_api::NativeAuthorityApi& _api;
    native_authority_paid::NativeAuthorityPaid& _paid;
    const NativePrincipalControlInitial& _initial;
    std::function<double()> _clock;
    std::unordered_map<std::string, std::shared_ptr<CreateFlight>> _creating;
    std::shared_ptr<Cleaning> _cleaning;
    State(native_authority_sql::NativeAuthoritySql& sql, native_authority_api::NativeAuthorityApi& api, native_authority_paid::NativeAuthorityPaid& paid, const NativePrincipalControlInitial& initial, std::function<double()> clock) : _sql(sql), _api(api), _paid(paid), _initial(initial), _clock(std::move(clock)), _cleaning(cleanupWorkFor(initial.sources)) {}
    void initialize() {
      for (const auto& grant : _initial.grants) { if (grant.epoch != _sql.epoch) throw NativeAuthorityError("control_initial_grant_epoch_mismatch", 500); _sql.write("key_control_grant", {grant.id}, grant); }
      for (const auto& source : _initial.records) {
        if (bytes(source.hashRecord) != bytes(policy::apiKeyHashRecordFor(source.record))) throw NativeAuthorityError("control_initial_source_mismatch", 500);
        const auto old = _sql.readAs<NativeKeyPolicy>("api_policy", api_keys::apiKeyHashKey(source.record.hash), _clock()).value;
        if (!old || old->api.key_id != source.record.id) throw NativeAuthorityError("control_initial_binding_missing", 500);
        _sql.write("api_key_id", api_keys::apiKeyIdKey(source.record.id), source.record); _sql.write("api_key_hash", api_keys::apiKeyHashKey(source.record.hash), source.hashRecord); _sql.write("api_policy", api_keys::apiKeyHashKey(source.record.hash), _projection(source.record, nullptr, old, _clock()));
      }
    }
    void _authorize(const NativeControlBinding& binding, const NativeControlCommand& command, const VerifiedControlIdentity& verified, const std::string& intentDigest) const {
      const auto now = _clock(); _sql.assertEpoch(); const auto raw = _sql.read("key_control_grant", {binding.grantId}, now).value.value_or(nullptr); const std::string role = command.method == "resolvePrincipal" ? "principal_resolver" : "key_admin";
      if (!activeGrant(raw)) throw NativeAuthorityError("control_grant_denied", 403);
      const auto grant = json::value_to<NativeKeyControlGrant>(raw);
      const bool valid = std::visit([&](const auto& identity) { return grant.subjectId == identity.subjectId && identity.contextBinding == binding.contextBinding && identity.intentDigest == intentDigest && identity.kind == role; }, verified);
      if (command.epoch != _sql.epoch || grant.id != binding.grantId || grant.epoch != _sql.epoch || grant.expiresAtMs <= now || grant.contextBinding != binding.contextBinding || !valid || !includes(grant.roles, role)) throw NativeAuthorityError("control_grant_denied", 403);
      if (const auto* admin = std::get_if<VerifiedKeyAdmin>(&verified)) {
        if (command.method != "createKey" && (!admin->targetKeyId || admin->targetKeyId->empty() || (grant.targetKeyIds && !includes(*grant.targetKeyIds, *admin->targetKeyId)))) throw NativeAuthorityError("control_target_denied", 403);
        if (command.method == "createKey" && (admin->targetKeyId || grant.targetKeyIds)) throw NativeAuthorityError("control_target_denied", 403);
      }
    }
    Outcome _resolve(const VerifiedKeyAuth& auth, const NativeControlBinding& binding, double now) {
      const auto source = _sql.read("api_key_hash", api_keys::apiKeyHashKey(auth.tokenHash), now); const auto record = source.value.value_or(nullptr); const auto* revoked = rt::get(record, "revoked_at_ms");
      if (!revoked || !revoked->is_null()) return refusal(401, "Unauthorized", "invalid_api_key");
      const auto api = native_paid_policy::hasStrictPaidFallbackPolicy(record) ? policy::apiKeyPolicyFromHashRecord(auth.tokenHash, record, now) : std::nullopt;
      if (!api) return refusal(503, "API key quota migration is incomplete", "server_error");
      if (api->expires_at_ms != -1 && api->expires_at_ms <= now) return refusal(401, "Unauthorized", "invalid_api_key");
      if (auth.principalId.empty() || !source.revision || source.revision->empty()) throw NativeAuthorityError("verified_principal_missing", 503);
      const NativeKey key{_sql.epoch, binding.grantId, auth.principalId, api->key_id}; const auto prior = _sql.readAs<NativePrincipalResolution>("principal_resolution", key, now).value;
      const NativePrincipalReference ref{prior ? prior->id : _initial.sources.randomUUID(), auth.principalId, api->key_id, _sql.epoch};
      _sql.write("principal_resolution", key, NativePrincipalResolution{ref.id, ref.principalId, ref.keyId, ref.epoch, auth.tokenHash, *source.revision, binding.grantId}, std::nullopt, {.scope = api->key_id}); return NativeControlOutcomePrincipal{ref};
    }
    std::optional<Target> _target(const VerifiedKeyAdmin& admin, double now) const {
      if (!admin.targetKeyId || admin.targetKeyId->empty()) throw NativeAuthorityError("control_target_denied", 403);
      const auto id = _sql.readAs<::uos::types::ApiKeyRecord>("api_key_id", api_keys::apiKeyIdKey(*admin.targetKeyId), now);
      if (!id.value) return std::nullopt;
      const auto& record = *id.value;
      if (record.id != *admin.targetKeyId) throw NativeAuthorityError("control_source_identity_mismatch", 503);
      return Target{record, id, _sql.read("api_key_hash", api_keys::apiKeyHashKey(record.hash), now).revision, _sql.readAs<NativeKeyPolicy>("api_policy", api_keys::apiKeyHashKey(record.hash), now)};
    }
    bool _current(const Target& target, double now) const { return _sql.read("api_key_id", api_keys::apiKeyIdKey(target.record.id), now).revision == target.id.revision && _sql.read("api_key_hash", api_keys::apiKeyHashKey(target.record.hash), now).revision == target.hashRevision && _sql.read("api_policy", api_keys::apiKeyHashKey(target.record.hash), now).revision == target.projection.revision; }
    NativeKeyPolicy _projection(const ::uos::types::ApiKeyRecord& record, const VerifiedKeyAdmin* admin, const std::optional<NativeKeyPolicy>& old, double now) const {
      const auto api = policy::apiKeyPolicyFromHashRecord(record.hash, policy::apiKeyHashRecordFor(record), now);
      if (!api || !native_paid_policy::hasStrictPaidFallbackKeyPolicy(json::value_from(record))) throw NativeAuthorityError("API key quota migration is incomplete", 503);
      const std::optional<bool> allow = old ? std::optional<bool>(old->paid.allowUnrosteredUnlimited) : admin && _initial.sources.allowUnrosteredUnlimited ? _initial.sources.allowUnrosteredUnlimited(*admin, record) : std::nullopt;
      if (!allow) throw NativeAuthorityError("trusted_unrostered_binding_unavailable", 503);
      return {*api, {rt::numberToString(record.window_ms) + ":" + rt::numberToString(record.paid_fallback_pricing_checked_at_ms.value_or(0)), record.paid_fallback_quota_per_credit, record.paid_fallback_max_exposure_microcredits.value_or(json::object()), record.paid_fallback_model_ids, *allow}, record.revoked_at_ms.has_value(), true};
    }
    void _audit(const native_api_key_audit::ApiKeyChangeAction& action, const ::uos::types::ApiKeyRecord& record, const json::object& changed, const VerifiedKeyAdmin& admin, double now) {
      const auto event = native_api_key_audit::buildApiKeyChangeEvent({action, record.id, record.name, changed, admin.auditContext, now}, _initial.sources.audit); _sql.write("api_key_audit", native_api_key_audit::apiKeyChangeEventKey(event), event, _clock() + native_api_key_audit::API_KEY_CHANGE_RETENTION_MS, {.scope = record.id});
    }
    void _writeRecord(const ::uos::types::ApiKeyRecord& record, const std::optional<NativeKeyPolicy>& projection) {
      const auto old = _sql.read("api_key_id", api_keys::apiKeyIdKey(record.id), 0).value.value_or(json::object()); const auto value = json::value_from(record);
      _sql.write("api_key_id", api_keys::apiKeyIdKey(record.id), rt::objectSpread(old.as_object(), value.as_object())); _sql.write("api_key_hash", api_keys::apiKeyHashKey(record.hash), policy::apiKeyHashRecordFor(record));
      if (projection) _sql.write("api_policy", api_keys::apiKeyHashKey(record.hash), *projection); else _sql.delete_("api_policy", api_keys::apiKeyHashKey(record.hash));
    }
    json::object _public(const ::uos::types::ApiKeyRecord& record, double now) const {
      const auto api = policy::apiKeyPolicyFromHashRecord(record.hash, policy::apiKeyHashRecordFor(record), now); const auto raw = json::value_from(record);
      return {{"id", record.id}, {"name", record.name}, {"prefix", record.prefix}, {"created_at_ms", rt::makeNumber(record.created_at_ms)}, {"expires_at_ms", rt::makeNumber(api_keys::coerceApiKeyExpiresAtMs(raw))}, {"revoked_at_ms", record.revoked_at_ms ? json::value(rt::makeNumber(*record.revoked_at_ms)) : json::value(nullptr)}, {"usage_limit_requests", rt::makeNumber(record.usage_limit_requests)}, {"usage_requests", rt::makeNumber(api ? _api.usage(*api, now) : 0)}, {"usage_reset_at_ms", rt::makeNumber(record.usage_reset_at_ms)}, {"window_ms", rt::makeNumber(api_keys::coerceApiKeyWindowMs(raw))}};
    }
    Commit _createCommit(const NativeKeyCreatePreparation& prepared, const VerifiedKeyAdmin& admin) {
      return {[this, prepared, admin] {
        const auto& record = prepared.record;
        if (_sql.read("api_key_hash", api_keys::apiKeyHashKey(record.hash), _clock()).revision != prepared.hashRevision || _sql.read("api_key_id", api_keys::apiKeyIdKey(record.id), _clock()).value) return Applied{refusal(500, "Failed to persist API key", "server_error")};
        _writeRecord(record, prepared.projection); _api.createWindow(prepared.projection.api, prepared.atMs);
        const auto raw = json::value_from(record); json::object changed{{"name", record.name}, {"expires_at_ms", rt::makeNumber(record.expires_at_ms)}, {"usage_limit_requests", rt::makeNumber(record.usage_limit_requests)}};
        changed["window_ms"] = rt::makeNumber(record.window_ms);
        changed["paid_fallback_enabled"] = record.paid_fallback_enabled; changed["paid_fallback_limit_credits"] = rt::makeNumber(api_keys::paidFallbackMicrocreditsToCredits(rt::get(raw, "paid_fallback_limit_microcredits")));
        _audit("create", record, changed, admin, prepared.atMs); return Applied{NativeControlOutcomeApplied{record.id, _public(record, prepared.atMs)}};
      }};
    }
    rt::Task<Commit> _create(NativeControlCommand command, VerifiedKeyAdmin admin, Persist persist, std::shared_ptr<CreateFlight> flight) {
      const auto& raw = inputOf(command); const auto name = policy::normalizeApiKeyName(rt::get(raw, "name"));
      if (!name) co_return fixed(refusal(400, "name must be a non-empty string (<=80 chars)"));
      const auto providedJson = admin.createToken.toJson(); const auto provided = policy::normalizeOptionalApiKeyToken(providedJson ? &*providedJson : nullptr);
      if (!admin.createToken.isNullish() && !provided) co_return fixed(refusal(400, "token must use the u_ prefix followed by 64 lowercase hexadecimal characters"));
      if (!flight->token) flight->token = provided ? *provided : _initial.sources.generateToken();
      if (!flight->now) flight->now = _initial.sources.audit.now();
      if (!flight->defaults) flight->defaults = _initial.sources.defaults(*flight->now);
      const auto fields = policy::resolveApiKeyCreateFields(raw, *flight->now, {flight->defaults->expiryMs, flight->defaults->usageLimitRequests});
      if (const auto* failed = std::get_if<policy::ApiKeyFieldsNotOk>(&fields)) co_return fixed(refusal(400, failed->error));
      const auto& resolved = std::get<policy::ResolveApiKeyCreateFieldsOk>(fields).fields;
      if (!flight->hash) { rt::Promise<std::string> work(_initial.sources.hashToken(*flight->token)); auto value = co_await work.wait(); flight->hash = value; }
      const auto original = _sql.transaction([&] { return _sql.read("api_key_hash", api_keys::apiKeyHashKey(*flight->hash), _clock()); });
      if (original.value) co_return fixed(refusal(409, "API key already exists"));
      if (!flight->pricing) flight->pricing = _initial.sources.defaultPaidPolicy();
      auto pricing = *flight->pricing;
      if (resolved.paidFallbackEnabled && !flight->pricingReady) {
        rt::Reason error;
        try { rt::Promise<paid_fallback::index::InitializePaidFallbackPolicyResult> work(_initial.sources.initializePricing()); const auto value = co_await work.wait(); mergePricing(pricing, value); flight->pricing = pricing; flight->pricingReady = true; } catch (...) { error = std::current_exception(); }
        if (error) co_return fixed(_initial.sources.pricingFailure(error));
      }
      if (!flight->id) flight->id = _initial.sources.randomUUID();
      pricing.paid_fallback_enabled = resolved.paidFallbackEnabled; pricing.paid_fallback_limit_microcredits = resolved.paidFallbackLimitMicrocredits;
      const auto built = policy::buildApiKeyCreateRecords({*flight->id, *name, rt::slice(*flight->token, 0, 12), *flight->hash}, resolved, pricing, *flight->now);
      const auto projection = _projection(built.record, &admin, std::nullopt, *flight->now);
      if (!flight->prepared) flight->prepared = NativeKeyCreatePreparation{built.record, projection, *flight->now, original.revision};
      auto accepted = std::make_shared<NativeKeyCreatePreparation>(*flight->prepared); const auto prepared = *flight->prepared;
      Commit commit{[this, accepted, admin] { return _createCommit(*accepted, admin).apply(); }}; commit.prepare = [accepted, prepared, persist] { *accepted = persist(prepared); }; co_return commit;
    }
    rt::Task<Commit> _update(NativeControlCommand command, VerifiedKeyAdmin admin, Target target) {
      const auto record = target.record; const auto& raw = inputOf(command); const auto now = _initial.sources.audit.now(); const auto encoded = json::value_from(record); const auto expiry = api_keys::coerceApiKeyExpiresAtMs(encoded), windowMs = api_keys::coerceApiKeyWindowMs(encoded); const auto defaults = _initial.sources.defaults(now);
      const auto identity = policy::resolveApiKeyUpdateIdentity(raw, record, expiry, now, defaults.expiryMs);
      if (const auto* failed = std::get_if<policy::ApiKeyFieldsNotOk>(&identity)) co_return fixed(refusal(400, failed->error));
      const auto quota = policy::resolveApiKeyUpdateQuota(raw, record, defaults.usageLimitRequests);
      if (const auto* failed = std::get_if<policy::ApiKeyFieldsNotOk>(&quota)) co_return fixed(refusal(400, failed->error));
      const auto paid = policy::resolveApiKeyUpdatePaidFallback(raw, record);
      if (const auto* failed = std::get_if<policy::ApiKeyFieldsNotOk>(&paid)) co_return fixed(refusal(400, failed->error));
      auto paidFields = std::get<policy::ResolveApiKeyUpdatePaidFallbackOk>(paid).paidFallback;
      if (std::get<policy::ResolveApiKeyUpdatePaidFallbackOk>(paid).needsInitialization) {
        rt::Reason error;
        try { rt::Promise<paid_fallback::index::InitializePaidFallbackPolicyResult> work(_initial.sources.initializePricing()); const auto pricing = co_await work.wait(); paidFields.paidFallbackModelIds = pricing.paid_fallback_model_ids; paidFields.paidFallbackQuotaPerCredit = pricing.paid_fallback_quota_per_credit; paidFields.paidFallbackMaxExposureMicrocredits = pricing.paid_fallback_max_exposure_microcredits.value_or(json::object()); paidFields.paidFallbackPricingCheckedAtMs = pricing.paid_fallback_pricing_checked_at_ms; } catch (...) { error = std::current_exception(); }
        if (error) co_return fixed(_initial.sources.pricingFailure(error));
      }
      const auto reset = policy::resolveApiKeyResetUsage(raw);
      if (const auto* failed = std::get_if<policy::ApiKeyFieldsNotOk>(&reset)) co_return fixed(refusal(400, failed->error));
      const auto& id = std::get<policy::ResolveApiKeyUpdateIdentityOk>(identity).identity; const auto& q = std::get<policy::ResolveApiKeyUpdateQuotaOk>(quota).quota;
      const auto built = policy::buildApiKeyUpdateRecord(record, {id.name, id.expiresAtMs, q.usageLimitRequests, q.windowMs, paidFields.paidFallbackEnabled, paidFields.paidFallbackLimitMicrocredits, paidFields.paidFallbackModelIds, paidFields.paidFallbackQuotaPerCredit, paidFields.paidFallbackMaxExposureMicrocredits, paidFields.paidFallbackPricingCheckedAtMs, std::get<policy::ResolveApiKeyResetUsageOk>(reset).resetUsage}, now, windowMs);
      const bool changed = policy::apiKeyUpdateChanged(record, built.updated, expiry, windowMs, built.resetUsage); const auto projection = changed ? std::optional<NativeKeyPolicy>(_projection(built.updated, &admin, target.projection.value, now)) : std::nullopt;
      Commit commit{[this, target, record, built, changed, projection, admin, now, expiry, windowMs] {
        const bool current = _current(target, _clock());
        if (built.replaceQuotaWindow && _api.inspectKeyReset(record.id, _clock())) return Applied{refusal(409, "Cannot reset API key quota while requests are reserved; retry after their five-minute lease expires")};
        if (!current) return Applied{conflict()};
        if (!changed) return Applied{NativeControlOutcomeUnchanged{record.id, _public(record, _clock())}};
        if (!projection) throw NativeAuthorityError("control_projection_missing", 503);
        if (built.replaceQuotaWindow) _api.createWindow(projection->api, now);
        _writeRecord(built.updated, projection); _audit("update", built.updated, policy::apiKeyUpdateAuditChanges(record, built.updated, expiry, windowMs, built.resetUsage), admin, now); return Applied{NativeControlOutcomeApplied{record.id, _public(built.updated, _clock())}};
      }}; co_return commit;
    }
    std::optional<NativeKeyPolicy> _revocationProjection(const ::uos::types::ApiKeyRecord& record, const VerifiedKeyAdmin& admin, const std::optional<NativeKeyPolicy>& old, double now) const {
      if (policy::apiKeyPolicyFromHashRecord(record.hash, policy::apiKeyHashRecordFor(record), now)) return _projection(record, &admin, old, now);
      if (!old) return std::nullopt;
      auto next = *old; next.rawSource = true; next.revoked = record.revoked_at_ms.has_value(); return next;
    }
    Commit _revocation(const NativeControlCommand& command, const VerifiedKeyAdmin& admin, const Target& target) {
      const auto record = target.record; const auto guardKey = native_paid::paidFallbackDeletionGuardV3Key(record.id);
      if (command.method == "unrevokeKey") {
        const auto decision = policy::apiKeyUnrevokeDecision(record, _sql.read("paid_deletion_guard", guardKey, _clock()).value.value_or(nullptr));
        if (decision == "blocked") return fixed(refusal(409, "API key deletion is in progress and cannot be reversed", "paid_fallback_deletion_in_progress"));
        if (decision == "unchanged") return {[this, target, record, guardKey] { if (_sql.read("paid_deletion_guard", guardKey, _clock()).value) return Applied{refusal(409, "API key deletion is in progress and cannot be reversed", "paid_fallback_deletion_in_progress")}; return Applied{_current(target, _clock()) ? Outcome(NativeControlOutcomeUnchanged{record.id, json::object{{"id", record.id}, {"revoked_at_ms", nullptr}}}) : conflict()}; }};
      }
      const auto now = _initial.sources.audit.now(), expiry = api_keys::coerceApiKeyExpiresAtMs(json::value_from(record)); const auto updated = command.method == "revokeKey" ? policy::buildApiKeyRevocationRecord(record, expiry, now) : policy::buildApiKeyUnrevocationRecord(record, expiry); const auto projection = _revocationProjection(updated, admin, target.projection.value, now);
      return {[this, command, admin, target, record, guardKey, updated, projection, now] {
        if (!_current(target, _clock())) return Applied{conflict()};
        if (command.method == "unrevokeKey" && _sql.read("paid_deletion_guard", guardKey, _clock()).value) return Applied{refusal(409, "API key deletion is in progress and cannot be reversed", "paid_fallback_deletion_in_progress")};
        _writeRecord(updated, projection); _audit(command.method == "revokeKey" ? "revoke" : "unrevoke", updated, command.method == "revokeKey" ? policy::apiKeyRevokeAuditChanges(record, updated) : json::object{{"revoked_at_ms", nullptr}}, admin, now); return Applied{NativeControlOutcomeApplied{record.id, json::object{{"id", record.id}, {"revoked_at_ms", updated.revoked_at_ms ? json::value(rt::makeNumber(*updated.revoked_at_ms)) : json::value(nullptr)}}}};
      }};
    }
    void _releaseLocal(const Target& target, const std::optional<std::string>& owner) {
      if (!owner) return;
      const auto key = native_paid::paidFallbackDeletionGuardV3Key(target.record.id); const auto raw = _sql.read("paid_deletion_guard", key, _clock()).value.value_or(nullptr); const auto* local = rt::get(raw, "local_deletion"); const auto record = _sql.read("api_key_id", api_keys::apiKeyIdKey(target.record.id), _clock()).value.value_or(nullptr);
      if (local && rt::getString(*local, "owner") == *owner && rt::truthy(rt::get(record, "revoked_at_ms"))) _sql.write("paid_deletion_guard", key, rt::objectSpread(raw.as_object(), json::object{{"local_deletion", json::object{{"owner", nullptr}, {"completed_at_ms", nullptr}}}}), std::nullopt, {.scope = target.record.id});
    }
    Commit _delete(const VerifiedKeyAdmin& admin, const Target& target, const std::function<void()>& authorize) {
      const auto record = target.record;
      if (policy::apiKeyDeleteDecision(record) == "requires_revocation") return fixed(refusal(400, "Only revoked keys can be deleted"));
      std::optional<std::string> owner;
      const auto guardResult = _sql.transaction([&]() -> std::optional<Outcome> {
        authorize();
        if (!_current(target, _clock())) return conflict();
        const auto key = native_paid::paidFallbackDeletionGuardV3Key(record.id); const auto guard = _sql.read("paid_deletion_guard", key, _clock()).value;
        if (record.id == _initial.sources.localDevelopmentKeyId) {
          const auto retryable = policy::retryableLocalDeletionGuard(guard.value_or(nullptr));
          if (guard && !retryable) return refusal(409, "Local API key deletion already has an owner", "paid_fallback_deletion_in_progress");
          const auto created = retryable ? retryable->created_at_ms : _initial.sources.audit.now(); owner = _initial.sources.randomUUID(); _sql.write("paid_deletion_guard", key, json::object{{"created_at_ms", rt::makeNumber(created)}, {"local_deletion", json::object{{"owner", *owner}, {"completed_at_ms", nullptr}}}}, std::nullopt, {.scope = record.id});
        } else if (!guard) _sql.write("paid_deletion_guard", key, native_paid_contracts::PaidFallbackDeletionGuardV3{_initial.sources.audit.now()}, std::nullopt, {.scope = record.id});
        return std::nullopt;
      });
      if (guardResult) return fixed(*guardResult);
      native_authority_paid::NativePaidDeletionResult deletion;
      try { deletion = _sql.transaction([&] { authorize(); return _paid.deleteForKey(record.id, _clock()); }); }
      catch (...) { _sql.transaction([&] { authorize(); _releaseLocal(target, owner); }); return fixed(refusal(500, "Failed to prepare paid fallback state for API key deletion", "server_error")); }
      if (deletion.kind != "deleted") {
        _sql.transaction([&] { authorize(); _releaseLocal(target, owner); }); const auto& outstanding = *deletion.outstanding;
        return fixed(refusal(409, "Cannot delete API key while metered billing is pending or unresolved (pending=" + rt::numberToString(outstanding.pending_requests) + ", unresolved=" + rt::numberToString(outstanding.unresolved_requests) + ", markers=" + rt::numberToString(outstanding.pending_markers) + ")", "paid_fallback_billing_outstanding"));
      }
      Commit commit{[this, record, target, owner, admin] {
        if (!_current(target, _clock())) { _releaseLocal(target, owner); return Applied{conflict()}; }
        const auto guard = _sql.read("paid_deletion_guard", native_paid::paidFallbackDeletionGuardV3Key(record.id), _clock()).value.value_or(nullptr); const auto* local = rt::get(guard, "local_deletion");
        if (guard.is_null() || (owner && (!local || rt::getString(*local, "owner") != *owner))) throw NativeAuthorityError("local_deletion_ownership_changed");
        _sql.delete_("api_key_id", api_keys::apiKeyIdKey(record.id)); _sql.delete_("api_key_hash", api_keys::apiKeyHashKey(record.hash)); _sql.delete_("api_policy", api_keys::apiKeyHashKey(record.hash)); _audit("delete", record, {}, admin, _initial.sources.audit.now()); return Applied{NativeControlOutcomeApplied{record.id, json::object{{"id", record.id}}}, NativeControlCleanup{record.id, owner}};
      }};
      commit.onFailure = [this, authorize, target, owner] { _sql.transaction([&] { authorize(); _releaseLocal(target, owner); }); }; return commit;
    }
    rt::Task<Commit> _prepare(NativeControlCommand command, VerifiedKeyAdmin admin, Persist persist, std::function<void()> authorize, std::shared_ptr<CreateFlight> flight) {
      if (command.method == "createKey" || command.method == "updateKey") { const auto retired = policy::rejectRetiredApiKeyResetSetting(inputOf(command)); if (retired) co_return fixed(refusal(400, *retired)); }
      if (command.method == "createKey") { if (!flight) throw NativeAuthorityError("control_create_state_missing", 503); auto task = _create(command, admin, persist, flight); rt::Promise<Commit> work(std::move(task)); auto result = co_await work.wait(); co_return result; }
      const auto target = _sql.transaction([&] { return _target(admin, _clock()); });
      if (!target) co_return fixed(refusal(404, "Not found", "not_found"));
      if (command.method == "deleteKey") co_return _delete(admin, *target, authorize);
      if (!native_paid_policy::hasStrictPaidFallbackKeyPolicy(json::value_from(target->record))) co_return fixed(refusal(503, "API key paid fallback migration is incomplete", "server_error"));
      if (command.method == "updateKey") { auto task = _update(command, admin, *target); rt::Promise<Commit> work(std::move(task)); auto result = co_await work.wait(); co_return result; }
      co_return _revocation(command, admin, *target);
    }
    rt::Task<void> _cleanupOwned(NativeControlBinding binding, NativeControlCommand command, VerifiedControlIdentity verified, std::string intentDigest, NativeKey key, NativeControlReceipt receipt) {
      if (!receipt.cleanup) co_return;
      rt::Promise<void> work(_initial.sources.cleanupApiLogs(receipt.cleanup->keyId)); co_await work.wait();
      _sql.transaction([&] {
        _authorize(binding, command, verified, intentDigest); const auto current = _sql.readAs<NativeControlReceipt>("key_control_receipt", key, _clock()).value;
        if (!current || current->intent != receipt.intent) throw NativeAuthorityError("control_receipt_conflict");
        if (!current->cleanup) return;
        const auto cleanup = *current->cleanup;
        if (_sql.read("api_key_id", api_keys::apiKeyIdKey(cleanup.keyId), _clock()).value) throw NativeAuthorityError("local_deletion_ownership_changed");
        _api.deleteForKey(cleanup.keyId, _clock());
        if (cleanup.localOwner) {
          const auto guardKey = native_paid::paidFallbackDeletionGuardV3Key(cleanup.keyId); const auto guard = _sql.read("paid_deletion_guard", guardKey, _clock()).value.value_or(nullptr); const auto* local = rt::get(guard, "local_deletion");
          if (!local || rt::getString(*local, "owner") != *cleanup.localOwner) throw NativeAuthorityError("local_deletion_ownership_changed");
          _sql.write("paid_deletion_guard", guardKey, rt::objectSpread(guard.as_object(), json::object{{"local_deletion", rt::objectSpread(local->as_object(), json::object{{"completed_at_ms", rt::makeNumber(_initial.sources.audit.now())}})}}), std::nullopt, {.scope = cleanup.keyId});
        }
        auto next = *current; next.cleanup.reset(); _sql.write("key_control_receipt", key, next);
      });
    }
    rt::Task<void> _cleanup(NativeControlBinding binding, NativeControlCommand command, VerifiedControlIdentity verified, std::string intentDigest, NativeKey key, NativeControlReceipt receipt) {
      if (!receipt.cleanup) co_return;
      const auto ownerKey = native_authority_rpc::nativeCommandBytes(json::object{{"keyId", receipt.cleanup->keyId}, {"localOwner", receipt.cleanup->localOwner ? json::value(*receipt.cleanup->localOwner) : json::value(nullptr)}});
      auto found = _cleaning->find(ownerKey);
      if (found == _cleaning->end()) { rt::Promise<void> work(_cleanupOwned(binding, command, verified, intentDigest, key, receipt)); found = _cleaning->emplace(ownerKey, work).first; }
      const auto work = found->second; rt::Reason failure;
      try { co_await work.wait(); } catch (...) { failure = std::current_exception(); }
      const auto current = _cleaning->find(ownerKey); if (current != _cleaning->end() && current->second == work) _cleaning->erase(current);
      if (failure) std::rethrow_exception(failure);
      _sql.transaction([&] { _authorize(binding, command, verified, intentDigest); });
    }
    rt::Task<Commit> _preparedCommit(NativeControlCommand command, VerifiedKeyAdmin admin, std::string operationKey, std::string intent, std::optional<NativeKeyCreatePreparation> prior, Persist persist, std::function<void()> authorize) {
      auto found = _creating.find(operationKey); auto active = found == _creating.end() ? std::shared_ptr<CreateFlight>() : found->second;
      if (active && active->intent != intent) throw NativeAuthorityError("control_operation_replay_conflict");
      if (command.method == "createKey" && !active) { active = std::make_shared<CreateFlight>(); active->intent = intent; _creating[operationKey] = active; }
      auto work = active ? active->work : std::nullopt;
      if (!work) { if (prior) work = rt::Promise<Commit>::resolve(_createCommit(*prior, admin)); else work = rt::Promise<Commit>(_prepare(command, admin, persist, authorize, active)); if (active) active->work = work; }
      rt::Reason failure; std::optional<Commit> result;
      try { result = co_await work->wait(); } catch (...) { failure = std::current_exception(); }
      if (failure) { if (active && active->work && *active->work == *work) active->work.reset(); std::rethrow_exception(failure); }
      co_return *result;
    }
    rt::Task<NativeControlReply> control(NativeControlBinding binding, NativeControlCommand input) {
      const auto command = native_authority_rpc::parseNativeControlCommand(json::value_from(input)); const auto commandBytes = bytes(command); auto digestTask = digest(commandBytes); auto intentDigest = co_await std::move(digestTask);
      auto verifyTask = _initial.verifier.verify(binding.privateContext, intentDigest, _clock()); rt::Promise<VerifiedControlIdentity> verification(std::move(verifyTask)); const auto verified = co_await verification.wait(); const auto* admin = std::get_if<VerifiedKeyAdmin>(&verified); const auto targetKeyId = admin ? admin->targetKeyId : std::nullopt;
      const auto intentJson = json::object{{"command", json::value_from(command)}, {"targetKeyId", targetKeyId ? json::value(*targetKeyId) : json::value(nullptr)}, {"contextBinding", binding.contextBinding}, {"createToken", admin ? admin->createToken.toJson().value_or(nullptr) : json::value(nullptr)}}; auto intentTask = digest(native_authority_rpc::nativeCommandBytes(intentJson)); const auto intent = co_await std::move(intentTask);
      const auto subjectId = std::visit([](const auto& identity) { return identity.subjectId; }, verified); const NativeKey key{_sql.epoch, subjectId, binding.grantId, command.operationId};
      struct Prior { std::optional<Outcome> resolution; std::optional<NativeControlReceipt> receipt; };
      const auto prior = _sql.transaction([&]() -> Prior {
        _authorize(binding, command, verified, intentDigest);
        if (const auto* auth = std::get_if<VerifiedKeyAuth>(&verified)) return {_resolve(*auth, binding, _clock()), std::nullopt};
        const auto receipt = _sql.readAs<NativeControlReceipt>("key_control_receipt", key, _clock()).value;
        if (receipt && (receipt->intent != intent || receipt->targetKeyId != targetKeyId)) throw NativeAuthorityError("control_operation_replay_conflict");
        return {std::nullopt, receipt};
      });
      if (prior.resolution) co_return replyOf(*prior.resolution);
      if (prior.receipt && prior.receipt->reply) { auto task = _cleanup(binding, command, verified, intentDigest, key, *prior.receipt); rt::Promise<void> work(std::move(task)); co_await work.wait(); NativeControlReply result = NativeControlReplyHistoricalAck{*prior.receipt->reply}; co_return result; }
      if (!admin) throw NativeAuthorityError("control_grant_denied", 403);
      const Persist persist = [this, binding, command, verified, intentDigest, key, intent, targetKeyId](NativeKeyCreatePreparation prepared) {
        return _sql.transaction([&] {
          _authorize(binding, command, verified, intentDigest); const auto existing = _sql.readAs<NativeControlReceipt>("key_control_receipt", key, _clock()).value;
          if (existing && existing->intent != intent) throw NativeAuthorityError("control_operation_replay_conflict");
          if (existing && existing->prepared) return *existing->prepared;
          if (existing && existing->reply) return prepared;
          _sql.write("key_control_receipt", key, NativeControlReceipt{intent, targetKeyId, std::nullopt, prepared, std::nullopt}); return prepared;
        });
      };
      const auto operationKey = keyBytes(key); const std::function<void()> authorize = [this, binding, command, verified, intentDigest] { _authorize(binding, command, verified, intentDigest); };
      auto preparedTask = _preparedCommit(command, *admin, operationKey, intent, prior.receipt ? prior.receipt->prepared : std::nullopt, persist, authorize); rt::Promise<Commit> preparation(std::move(preparedTask)); const auto commit = co_await preparation.wait();
      try { if (commit.prepare) commit.prepare(); } catch (...) { const auto active = _creating.find(operationKey); if (active != _creating.end()) active->second->work.reset(); throw; }
      struct Result { NativeControlReceipt receipt; bool replay; };
      std::optional<Result> result;
      try { result = _sql.transaction([&]() -> Result {
        _authorize(binding, command, verified, intentDigest); const auto existing = _sql.readAs<NativeControlReceipt>("key_control_receipt", key, _clock()).value;
        if (existing && (existing->intent != intent || existing->targetKeyId != targetKeyId)) throw NativeAuthorityError("control_operation_replay_conflict");
        if (existing && existing->reply) return {*existing, true};
        const auto applied = commit.apply(); const NativeControlReceipt receipt{intent, targetKeyId, applied.reply, std::nullopt, applied.cleanup}; _sql.write("key_control_receipt", key, receipt); return {receipt, false};
      }); } catch (...) { if (commit.onFailure) commit.onFailure(); throw; }
      auto cleanupTask = _cleanup(binding, command, verified, intentDigest, key, result->receipt); rt::Promise<void> cleanup(std::move(cleanupTask)); co_await cleanup.wait(); _creating.erase(operationKey);
      if (!result->receipt.reply) throw NativeAuthorityError("control_preparation_incomplete", 503);
      NativeControlReply reply = result->replay ? NativeControlReply(NativeControlReplyHistoricalAck{*result->receipt.reply}) : replyOf(*result->receipt.reply); co_return reply;
    }
  };
  NativeAuthorityPrincipals::NativeAuthorityPrincipals(native_authority_sql::NativeAuthoritySql& sql, native_authority_api::NativeAuthorityApi& api, native_authority_paid::NativeAuthorityPaid& paid, const NativePrincipalControlInitial& initial, std::function<double()> clock) : _state(std::make_shared<State>(sql, api, paid, initial, std::move(clock))) {}
  void NativeAuthorityPrincipals::initialize() { _state->initialize(); }
  rt::Task<NativeControlReply> NativeAuthorityPrincipals::control(NativeControlBinding binding, NativeControlCommand input) { auto state = _state; auto task = state->control(std::move(binding), std::move(input)); auto reply = co_await std::move(task); co_return reply; }
}
