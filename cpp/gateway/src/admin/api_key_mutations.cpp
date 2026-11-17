// Port of src/admin/api-key-mutations.ts.
#include "uos/admin/api_key_mutations.hpp"
#include "uos/admin/api_keys.hpp"
#include "uos/analytics.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/api_keys.hpp"
#include "uos/auth/local_development_key.hpp"
#include "uos/authority/native_api_key_policy.hpp"
#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "uos/paid_fallback/index.hpp"
#include "uos/paid_fallback/ledger_admission.hpp"
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/request.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/utils.hpp"

#include <limits>
#include <utility>
#include <variant>

namespace uos::admin::api_key_mutations {
  namespace {
    namespace policy = authority::native_api_key_policy;
    namespace keys = ::uos::api_keys;
    namespace quota = api_key_policy;
    namespace paid = paid_fallback::index;
    namespace ledger = paid_fallback::ledger_state;
    namespace admission = paid_fallback::ledger_admission;
    using Record = ::uos::types::ApiKeyRecord;
    using Context = std::optional<api_key_audit::ApiKeyAuditContext>;
    using Entry = rt::KvEntryMaybe<>;
    struct Failed { rt::Response response; };

    // Storage is unchecked in the source; the typed policy view must not discard its backing row.
    Record recordView(const rt::KvValue& stored) {
      const auto value = rt::storedJsonView(stored);
      const auto number = [&](std::string_view name) { return rt::getNumber(value, name).value_or(std::numeric_limits<double>::quiet_NaN()); };
      const auto string = [&](std::string_view name) { return std::string(rt::getString(value, name).value_or("")); };
      Record record{};
      record.id = string("id"); record.name = string("name"); record.prefix = string("prefix"); record.hash = string("hash");
      record.created_at_ms = number("created_at_ms"); record.expires_at_ms = keys::coerceApiKeyExpiresAtMs(value);
      record.revoked_at_ms = rt::getNumber(value, "revoked_at_ms"); record.usage_limit_requests = number("usage_limit_requests");
      record.usage_requests = number("usage_requests"); record.usage_reset_at_ms = number("usage_reset_at_ms");
      record.window_ms = keys::coerceApiKeyWindowMs(value); record.usage_quota_version = number("usage_quota_version");
      record.paid_fallback_enabled = rt::getBool(value, "paid_fallback_enabled").value_or(false);
      record.paid_fallback_limit_microcredits = number("paid_fallback_limit_microcredits");
      record.paid_fallback_spent_microcredits = number("paid_fallback_spent_microcredits");
      record.paid_fallback_reserved_microcredits = number("paid_fallback_reserved_microcredits");
      if (const auto id = rt::getString(value, "paid_fallback_reservation_request_id")) record.paid_fallback_reservation_request_id = std::string(*id);
      if (const auto* ids = rt::getArray(value, "paid_fallback_model_ids")) {
        for (const auto& id : *ids) if (id.is_string()) record.paid_fallback_model_ids.emplace_back(id.as_string());
      }
      record.paid_fallback_quota_per_credit = number("paid_fallback_quota_per_credit");
      if (const auto* exposure = rt::getObject(value, "paid_fallback_max_exposure_microcredits")) record.paid_fallback_max_exposure_microcredits = *exposure;
      record.paid_fallback_pricing_checked_at_ms = rt::getNumber(value, "paid_fallback_pricing_checked_at_ms");
      return record;
    }
    rt::KvObject spread(const rt::KvValue& stored) {
      if (stored.is<rt::KvObject>()) return stored.as<rt::KvObject>();
      const auto value = rt::storedJsonView(stored);
      rt::KvObject output;
      if (value.is_object()) for (const auto& member : value.as_object()) output.set(std::string(member.key()), rt::KvValue(member.value()));
      else if (value.is_array()) for (std::size_t i = 0; i < value.as_array().size(); ++i) output.set(std::to_string(i), rt::KvValue(value.as_array()[i]));
      return output;
    }
    rt::KvValue apiKeyHashRecordFor(const rt::KvValue& record, const std::string& id) {
      rt::KvObject output;
      output.set("id", rt::KvValue(id));
      for (const auto field : {"expires_at_ms", "revoked_at_ms", "usage_limit_requests", "usage_requests", "usage_reset_at_ms", "window_ms", "usage_quota_version",
                              "paid_fallback_enabled", "paid_fallback_limit_microcredits", "paid_fallback_spent_microcredits", "paid_fallback_reserved_microcredits",
                              "paid_fallback_reservation_request_id"}) output.set(field, record[field]);
      return rt::KvValue(std::move(output));
    }
    void appendChange(rt::AtomicOperation& atomic, const std::string& action, const std::string& id, const rt::KvValue& record,
                      json::object changed, const Context& context, double nowMs) {
      api_key_audit::ApiKeyChangeEventInput input{.action = action, .targetKeyId = id, .changed = std::move(changed), .context = context, .nowMs = nowMs};
      if (record["name"].isString()) input.targetKeyName = record["name"].asString();
      api_key_audit::appendApiKeyChangeEvent(atomic, api_key_audit::buildApiKeyChangeEvent(input));
    }
    rt::Response response(const rt::KvObject& body) {
      const auto serialized = rt::kvJsonStringify(rt::KvValue(body));
      return http::json(200, rt::jsonParse(*serialized), rt::Headers{{"x-uos-upstream", "chatgpt_codex"}});
    }
    rt::Response migrationError() {
      return http::openaiError(503, "API key paid fallback migration is incomplete", "server_error", http::OpenaiErrorOptions{.type = "server_error"});
    }
    rt::Response quotaMigrationError() {
      return http::openaiError(503, "API key quota migration is incomplete", "server_error", http::OpenaiErrorOptions{.type = "server_error"});
    }
    rt::Response conflict() { return http::openaiError(409, "API key was modified concurrently; retry", "invalid_request_error"); }
    rt::Response reservationsConflict() {
      return http::openaiError(409, "Cannot reset API key quota while requests are reserved; retry after their five-minute lease expires", "invalid_request_error");
    }
    struct ApiKeyUpdateTarget { std::string id; rt::KvKey idKey; Entry entry; Record record; };
    rt::Task<std::variant<ApiKeyUpdateTarget, Failed>> resolveApiKeyUpdateTarget(std::shared_ptr<rt::Kv> kv, json::object raw) {
      const auto id = utils::getString(rt::get(raw, "id"));
      if (!id || id->empty()) co_return Failed{http::openaiError(400, "id is required", "invalid_request_error")};
      const auto idKey = keys::apiKeyIdKey(*id);
      rt::Promise<Entry> pending(kv->get(idKey)); const auto entry = co_await pending.wait();
      if (!entry.value.truthy()) co_return Failed{http::openaiError(404, "Not found", "not_found")};
      if (!paid::hasStrictPaidFallbackKeyPolicy(rt::storedJsonView(entry.value))) co_return Failed{migrationError()};
      co_return ApiKeyUpdateTarget{*id, idKey, entry, recordView(entry.value)};
    }
    /** The paid-fallback initialization only runs when the key is enabled for the first time. */
    rt::Task<std::variant<policy::ApiKeyUpdatePaidFallback, Failed>> resolveApiKeyUpdatePaidFallback(json::object raw, Record record, rt::AbortSignal signal) {
      const auto result = policy::resolveApiKeyUpdatePaidFallback(raw, record);
      if (const auto* failure = std::get_if<policy::ApiKeyFieldsNotOk>(&result)) co_return Failed{api_keys::paidFallbackInputError(failure->error)};
      const auto& resolved = std::get<policy::ResolveApiKeyUpdatePaidFallbackOk>(result);
      auto paidFallback = resolved.paidFallback;
      if (!resolved.needsInitialization) co_return paidFallback;
      try {
        rt::Promise<paid::InitializePaidFallbackPolicyResult> pending(paid::initializePaidFallbackPolicy(signal));
        const auto initialized = co_await pending.wait();
        paidFallback.paidFallbackModelIds = initialized.paid_fallback_model_ids;
        paidFallback.paidFallbackQuotaPerCredit = initialized.paid_fallback_quota_per_credit;
        paidFallback.paidFallbackMaxExposureMicrocredits = initialized.paid_fallback_max_exposure_microcredits.value_or(json::object());
        paidFallback.paidFallbackPricingCheckedAtMs = initialized.paid_fallback_pricing_checked_at_ms;
        co_return paidFallback;
      } catch (...) { co_return Failed{api_keys::paidFallbackInitializationError(std::current_exception())}; }
    }
    rt::Task<std::variant<policy::ApiKeyUpdateFields, Failed>> resolveApiKeyUpdateFields(json::object raw, Record record, double currentExpiresAtMs,
                                                                                       double nowMs, rt::AbortSignal signal) {
      const auto identity = policy::resolveApiKeyUpdateIdentity(raw, record, currentExpiresAtMs, nowMs, keys::getDefaultExpiryMs(nowMs));
      if (const auto* failure = std::get_if<policy::ApiKeyFieldsNotOk>(&identity)) co_return Failed{http::openaiError(400, failure->error, "invalid_request_error")};
      const auto quotaFields = policy::resolveApiKeyUpdateQuota(raw, record, keys::DEFAULT_USAGE_LIMIT_REQUESTS);
      if (const auto* failure = std::get_if<policy::ApiKeyFieldsNotOk>(&quotaFields)) co_return Failed{http::openaiError(400, failure->error, "invalid_request_error")};
      rt::Promise<std::variant<policy::ApiKeyUpdatePaidFallback, Failed>> pending(resolveApiKeyUpdatePaidFallback(raw, record, signal));
      const auto paidFallback = co_await pending.wait();
      if (const auto* failure = std::get_if<Failed>(&paidFallback)) co_return *failure;
      const auto reset = policy::resolveApiKeyResetUsage(raw);
      if (const auto* failure = std::get_if<policy::ApiKeyFieldsNotOk>(&reset)) co_return Failed{http::openaiError(400, failure->error, "invalid_request_error")};
      const auto& identityFields = std::get<policy::ResolveApiKeyUpdateIdentityOk>(identity).identity;
      const auto& quotaFieldsValue = std::get<policy::ResolveApiKeyUpdateQuotaOk>(quotaFields).quota;
      const auto& paidFields = std::get<policy::ApiKeyUpdatePaidFallback>(paidFallback);
      co_return policy::ApiKeyUpdateFields{identityFields.name, identityFields.expiresAtMs, quotaFieldsValue.usageLimitRequests, quotaFieldsValue.windowMs,
        paidFields.paidFallbackEnabled, paidFields.paidFallbackLimitMicrocredits, paidFields.paidFallbackModelIds, paidFields.paidFallbackQuotaPerCredit,
        paidFields.paidFallbackMaxExposureMicrocredits, paidFields.paidFallbackPricingCheckedAtMs, std::get<policy::ResolveApiKeyResetUsageOk>(reset).resetUsage};
    }
    rt::Task<rt::Response> apiKeyUpdateUnchangedResponse(std::shared_ptr<rt::Kv> kv, rt::KvValue record, double currentExpiresAtMs,
                                                       double currentWindowMs, double nowMs, std::optional<quota::ApiKeyPolicy> quotaPolicy = std::nullopt,
                                                       bool unchanged = true) {
      const auto typed = recordView(record);
      if (unchanged) {
        const auto hashRecord = apiKeyHashRecordFor(record, typed.id);
        quotaPolicy = quota::apiKeyPolicyFromHashRecord(typed.hash, rt::storedJsonView(hashRecord), nowMs);
      }
      double used = 0;
      if (quotaPolicy) { rt::Promise<double> pending(quota::getApiKeyUsageV3(*quotaPolicy, kv)); used = co_await pending.wait(); }
      rt::KvObject body;
      for (const auto field : {"id", "name", "prefix", "created_at_ms"}) body.set(field, record[field]);
      body.set("expires_at_ms", currentExpiresAtMs); body.set("revoked_at_ms", record["revoked_at_ms"]);
      body.set("usage_limit_requests", record["usage_limit_requests"]); body.set("usage_requests", used);
      body.set("usage_reset_at_ms", record["usage_reset_at_ms"]); body.set("window_ms", currentWindowMs);
      rt::Promise<json::object> publicPending(api_keys::paidFallbackPublicFields(typed, kv)); const auto fields = co_await publicPending.wait();
      for (const auto& member : fields) body.set(std::string(member.key()), rt::KvValue(member.value()));
      co_return response(body);
    }
    /** Read the superseded aggregate after reclaim and before the live scan, checking reservation races atomically. */
    rt::Task<std::variant<Entry, Failed>> inspectApiKeyQuotaResetReservations(std::shared_ptr<rt::Kv> kv, std::string keyId,
                                                                           quota::ApiKeyPolicy currentQuotaPolicy, double nowMs) {
      try {
        rt::Promise<void> reclaim(quota::reclaimApiKeyUsageReservationsForKeyV3(kv, keyId, nowMs)); co_await reclaim.wait();
        const auto key = quota::apiKeyUsageV3WindowKey(currentQuotaPolicy);
        rt::Promise<Entry> pending(kv->get(key, rt::KvGetOptions{.consistency = rt::KvConsistencyLevel::strong})); const auto windowEntry = co_await pending.wait();
        rt::Promise<bool> live(quota::hasLiveApiKeyUsageReservationsV3(kv, keyId, nowMs)); const bool reserved = co_await live.wait();
        if (reserved) co_return Failed{reservationsConflict()};
        co_return windowEntry;
      } catch (...) {
        rt::console::warn("[ai.ubq.fi] Failed to inspect API key quota reservations before reset:", rt::console::Value(std::current_exception()));
        co_return Failed{http::openaiError(503, "API key quota ledger is unavailable", "server_error", http::OpenaiErrorOptions{.type = "server_error"})};
      }
    }
    /** Recheck reservations after a lost commit race, then report the conflict. */
    rt::Task<rt::Response> resolveApiKeyUpdateCommitConflict(std::shared_ptr<rt::Kv> kv, std::string keyId, double nowMs, bool replaceQuotaWindow) {
      if (!replaceQuotaWindow) co_return conflict();
      try {
        rt::Promise<void> reclaim(quota::reclaimApiKeyUsageReservationsForKeyV3(kv, keyId, nowMs)); co_await reclaim.wait();
        rt::Promise<bool> live(quota::hasLiveApiKeyUsageReservationsV3(kv, keyId, nowMs)); const bool reserved = co_await live.wait();
        if (reserved) co_return reservationsConflict();
      } catch (...) {
        rt::console::warn("[ai.ubq.fi] Failed to recheck API key quota reservations after reset conflict:", rt::console::Value(std::current_exception()));
        co_return http::openaiError(503, "API key quota ledger is unavailable", "server_error", http::OpenaiErrorOptions{.type = "server_error"});
      }
      co_return conflict();
    }
    struct Persisted { Record updated; quota::ApiKeyPolicy quotaPolicy; };
    /** Write the record, hash record and replaced quota windows with their audit event atomically. */
    rt::Task<std::variant<Persisted, Failed>> persistApiKeyUpdate(std::shared_ptr<rt::Kv> kv, ApiKeyUpdateTarget target, Record updated,
      rt::KvValue updatedStored, bool replaceQuotaWindow, double nowMs, double currentExpiresAtMs, double currentWindowMs, bool resetUsage, Context context) {
      const auto hashKey = keys::apiKeyHashKey(target.record.hash);
      rt::Promise<Entry> hashPending(kv->get(hashKey)); const auto hashEntry = co_await hashPending.wait();
      const auto updatedHash = apiKeyHashRecordFor(updatedStored, updated.id);
      const auto quotaPolicy = quota::apiKeyPolicyFromHashRecord(updated.hash, rt::storedJsonView(updatedHash), nowMs);
      if (!quotaPolicy) co_return Failed{quotaMigrationError()};
      std::optional<Entry> currentQuotaWindowEntry;
      if (replaceQuotaWindow) {
        const auto currentQuotaPolicy = quota::apiKeyPolicyFromHashRecord(target.record.hash, rt::storedJsonView(target.entry.value), nowMs);
        if (!currentQuotaPolicy) co_return Failed{quotaMigrationError()};
        rt::Promise<std::variant<Entry, Failed>> inspection(inspectApiKeyQuotaResetReservations(kv, updated.id, *currentQuotaPolicy, nowMs));
        const auto inspected = co_await inspection.wait();
        if (const auto* failure = std::get_if<Failed>(&inspected)) co_return *failure;
        currentQuotaWindowEntry = std::get<Entry>(inspected);
      }
      std::optional<::uos::types::ApiKeyUsageWindowV3> quotaWindow;
      std::optional<Entry> quotaWindowEntry;
      if (replaceQuotaWindow) {
        quotaWindow = quota::makeApiKeyUsageWindowV3(*quotaPolicy, nowMs);
        const auto key = quota::apiKeyUsageV3WindowKey(*quotaPolicy);
        rt::Promise<Entry> pending(kv->get(key, rt::KvGetOptions{.consistency = rt::KvConsistencyLevel::strong})); quotaWindowEntry = co_await pending.wait();
      }
      auto atomic = kv->atomic();
      atomic->check(target.entry).check(hashEntry).set(target.idKey, updatedStored).set(hashKey, updatedHash);
      if (quotaWindow && quotaWindowEntry) {
        const rt::KvSetOptions options{.expireIn = quota::apiKeyUsageV3RetentionMs(quotaWindow->window_reset_at_ms, nowMs)};
        atomic->check(*quotaWindowEntry).set(quota::apiKeyUsageV3WindowKey(*quotaPolicy), rt::KvValue(json::value_from(*quotaWindow)), options);
      }
      if (currentQuotaWindowEntry) atomic->check(*currentQuotaWindowEntry);
      auto changed = policy::apiKeyUpdateAuditChanges(target.record, updated, currentExpiresAtMs, currentWindowMs, resetUsage);
      appendChange(*atomic, "update", updated.id, updatedStored, std::move(changed), context, nowMs);
      rt::Promise<rt::KvCommitResult> commitPending(atomic->commit()); const auto commit = co_await commitPending.wait();
      if (!commit.ok) {
        rt::Promise<rt::Response> resolution(resolveApiKeyUpdateCommitConflict(kv, updated.id, nowMs, replaceQuotaWindow));
        const auto error = co_await resolution.wait(); co_return Failed{error};
      }
      quota::invalidateApiKeyPolicy(updated.id);
      co_return Persisted{updated, *quotaPolicy};
    }
    rt::Task<rt::Response> update(rt::Request req, Context context) {
      auto kvPending = kv::getKv(); const auto kv = co_await kvPending.wait();
      if (!kv) co_return http::openaiError(500, "Deno KV is not available; cannot manage API keys", "server_error");
      rt::Promise<json::value> parsed(request::readJsonBody(req)); const auto raw = co_await parsed.wait();
      if (!rt::truthy(raw) || !utils::isRecord(raw)) co_return http::openaiError(400, "Invalid JSON body", "invalid_request_error");
      const auto object = raw.is_object() ? raw.as_object() : json::object();
      if (const auto retired = api_keys::rejectRetiredApiKeyResetSetting(object)) co_return *retired;
      rt::Promise<std::variant<ApiKeyUpdateTarget, Failed>> targetPending(resolveApiKeyUpdateTarget(kv, object)); const auto targetResult = co_await targetPending.wait();
      if (const auto* failure = std::get_if<Failed>(&targetResult)) co_return failure->response;
      const auto target = std::get<ApiKeyUpdateTarget>(targetResult);
      const double now = rt::dateNow();
      const double currentExpiresAtMs = keys::coerceApiKeyExpiresAtMs(rt::storedJsonView(target.entry.value));
      const double currentWindowMs = keys::coerceApiKeyWindowMs(rt::storedJsonView(target.entry.value));
      rt::Promise<std::variant<policy::ApiKeyUpdateFields, Failed>> fieldsPending(resolveApiKeyUpdateFields(object, target.record, currentExpiresAtMs, now, req.signal()));
      const auto fieldsResult = co_await fieldsPending.wait();
      if (const auto* failure = std::get_if<Failed>(&fieldsResult)) co_return failure->response;
      const auto built = policy::buildApiKeyUpdateRecord(target.record, std::get<policy::ApiKeyUpdateFields>(fieldsResult), now, currentWindowMs);
      if (!policy::apiKeyUpdateChanged(target.record, built.updated, currentExpiresAtMs, currentWindowMs, built.resetUsage)) {
        rt::Promise<rt::Response> unchanged(apiKeyUpdateUnchangedResponse(kv, target.entry.value, currentExpiresAtMs, currentWindowMs, now));
        const auto output = co_await unchanged.wait(); co_return output;
      }
      auto updatedObject = spread(target.entry.value);
      const auto updatedJson = json::value_from(built.updated);
      for (const auto field : {"name", "expires_at_ms", "usage_limit_requests", "usage_requests", "usage_reset_at_ms", "window_ms", "paid_fallback_enabled",
                              "paid_fallback_limit_microcredits", "paid_fallback_spent_microcredits", "paid_fallback_reserved_microcredits",
                              "paid_fallback_reservation_request_id", "paid_fallback_model_ids", "paid_fallback_quota_per_credit",
                              "paid_fallback_max_exposure_microcredits", "paid_fallback_pricing_checked_at_ms"}) {
        if ((std::string_view(field) == "name" && !rt::hasOwn(object, "name")) ||
            (std::string_view(field) == "usage_limit_requests" && !rt::hasOwn(object, "usage_limit_requests")) ||
            (!built.replaceQuotaWindow && (std::string_view(field) == "usage_requests" || std::string_view(field) == "usage_reset_at_ms"))) {
          updatedObject.set(field, target.entry.value[field]);
        } else { const auto* value = rt::get(updatedJson, field); updatedObject.set(field, value ? rt::KvValue(*value) : rt::KvValue()); }
      }
      const rt::KvValue updatedStored(std::move(updatedObject));
      rt::Promise<std::variant<Persisted, Failed>> persistedPending(persistApiKeyUpdate(kv, target, built.updated, updatedStored, built.replaceQuotaWindow,
        now, currentExpiresAtMs, currentWindowMs, built.resetUsage, context));
      const auto persistedResult = co_await persistedPending.wait();
      if (const auto* failure = std::get_if<Failed>(&persistedResult)) co_return failure->response;
      const auto persisted = std::get<Persisted>(persistedResult);
      rt::Promise<rt::Response> outputPending(apiKeyUpdateUnchangedResponse(kv, updatedStored, persisted.updated.expires_at_ms, persisted.updated.window_ms,
        now, persisted.quotaPolicy, false));
      const auto output = co_await outputPending.wait(); co_return output;
    }
    rt::Task<rt::Response> revocation(rt::Request req, Context context, bool unrevoke) {
      auto kvPending = kv::getKv(); const auto kv = co_await kvPending.wait();
      if (!kv) co_return http::openaiError(500, "Deno KV is not available; cannot manage API keys", "server_error");
      rt::Promise<json::value> parsed(request::readJsonBody(req)); const auto raw = co_await parsed.wait();
      if (!rt::truthy(raw) || !utils::isRecord(raw)) co_return http::openaiError(400, "Invalid JSON body", "invalid_request_error");
      const auto id = utils::getString(rt::get(raw, "id"));
      if (!id || id->empty()) co_return http::openaiError(400, "id is required", "invalid_request_error");
      const auto idKey = keys::apiKeyIdKey(*id);
      rt::Promise<Entry> entryPending(kv->get(idKey)); const auto entry = co_await entryPending.wait();
      if (!entry.value.truthy()) co_return http::openaiError(404, "Not found", "not_found");
      if (!paid::hasStrictPaidFallbackKeyPolicy(rt::storedJsonView(entry.value))) co_return migrationError();
      std::optional<Entry> deletionGuard;
      if (unrevoke) {
        const auto guardKey = ledger::paidFallbackDeletionGuardV3Key(*id);
        rt::Promise<Entry> pending(kv->get(guardKey, rt::KvGetOptions{.consistency = rt::KvConsistencyLevel::strong})); deletionGuard = co_await pending.wait();
        if (deletionGuard->value.truthy()) co_return http::openaiError(409, "API key deletion is in progress and cannot be reversed", "paid_fallback_deletion_in_progress");
        if (!entry.value["revoked_at_ms"].truthy()) {
          const rt::KvObject body{{"id", *id}, {"revoked_at_ms", nullptr}}; co_return response(body);
        }
      }
      const auto now = rt::dateNow();
      const auto expiresAtMs = keys::coerceApiKeyExpiresAtMs(rt::storedJsonView(entry.value));
      auto updatedObject = spread(entry.value);
      updatedObject.set("expires_at_ms", expiresAtMs);
      updatedObject.set("revoked_at_ms", unrevoke ? rt::KvValue(nullptr) : entry.value["revoked_at_ms"].truthy() ? entry.value["revoked_at_ms"] : rt::KvValue(now));
      const rt::KvValue updated(std::move(updatedObject));
      const auto hashKey = keys::apiKeyHashKey(recordView(entry.value).hash);
      rt::Promise<Entry> hashPending(kv->get(hashKey)); const auto hashEntry = co_await hashPending.wait();
      auto atomic = kv->atomic(); atomic->check(entry);
      if (deletionGuard) atomic->check(*deletionGuard);
      atomic->set(idKey, updated).set(hashKey, apiKeyHashRecordFor(updated, *id));
      if (hashEntry.versionstamp) atomic->check(hashEntry);
      json::object changed;
      if (unrevoke) changed["revoked_at_ms"] = nullptr;
      else {
        if (updated["revoked_at_ms"] != entry.value["revoked_at_ms"]) changed["revoked_at_ms"] = rt::storedJsonView(updated["revoked_at_ms"]);
        if (updated["expires_at_ms"] != entry.value["expires_at_ms"]) changed["expires_at_ms"] = rt::makeNumber(expiresAtMs);
      }
      appendChange(*atomic, unrevoke ? "unrevoke" : "revoke", *id, updated, changed, context, now);
      rt::Promise<rt::KvCommitResult> commitPending(atomic->commit()); const auto commit = co_await commitPending.wait();
      if (!commit.ok) co_return conflict();
      quota::invalidateApiKeyPolicy(recordView(updated).id);
      const rt::KvObject body{{"id", updated["id"]}, {"revoked_at_ms", updated["revoked_at_ms"]}}; co_return response(body);
    }
    using LocalGuard = auth::local_development_key::LocalDevelopmentDeletionGuard;
    struct LocalApiKeyDeletionClaim { LocalGuard guard; rt::AtomicCheck check; };
    rt::Task<std::variant<LocalApiKeyDeletionClaim, Failed>> claimLocalDeletion(std::shared_ptr<rt::Kv> kv, Entry entry, Entry deletionGuard) {
      const auto retryable = policy::retryableLocalDeletionGuard(deletionGuard.value);
      if (deletionGuard.value.truthy() && !retryable) co_return Failed{http::openaiError(409, "Local API key deletion already has an owner", "paid_fallback_deletion_in_progress")};
      const LocalGuard guard{.created_at_ms = retryable ? retryable->created_at_ms : rt::dateNow(),
                            .local_deletion = {.owner = rt::crypto::randomUUID(), .completed_at_ms = std::nullopt}};
      auto atomic = kv->atomic(); atomic->check(entry).check(deletionGuard).set(deletionGuard.key, rt::KvValue(json::value_from(guard)));
      rt::Promise<rt::KvCommitResult> pending(atomic->commit()); const auto claim = co_await pending.wait();
      if (!claim.ok) co_return Failed{http::openaiError(409, "Local API key deletion was claimed concurrently", "paid_fallback_deletion_in_progress")};
      co_return LocalApiKeyDeletionClaim{guard, rt::AtomicCheck{deletionGuard.key, claim.versionstamp}};
    }
    rt::Task<std::variant<std::optional<LocalApiKeyDeletionClaim>, Failed>> prepareDeletionGuard(std::shared_ptr<rt::Kv> kv, std::string id, Entry entry, Entry deletionGuard) {
      if (id == auth::local_development_key::LOCAL_DEVELOPMENT_KEY_ID) {
        rt::Promise<std::variant<LocalApiKeyDeletionClaim, Failed>> pending(claimLocalDeletion(kv, entry, deletionGuard)); const auto claim = co_await pending.wait();
        if (const auto* failure = std::get_if<Failed>(&claim)) co_return *failure;
        co_return std::optional<LocalApiKeyDeletionClaim>(std::get<LocalApiKeyDeletionClaim>(claim));
      }
      if (deletionGuard.value.truthy()) co_return std::optional<LocalApiKeyDeletionClaim>();
      const rt::KvValue guard(json::object{{"created_at_ms", rt::makeNumber(rt::dateNow())}});
      auto atomic = kv->atomic(); atomic->check(entry).check(deletionGuard).set(deletionGuard.key, guard);
      rt::Promise<rt::KvCommitResult> pending(atomic->commit()); const auto commit = co_await pending.wait();
      if (!commit.ok) co_return Failed{conflict()};
      co_return std::optional<LocalApiKeyDeletionClaim>();
    }
    /** Only a known refusal before ID deletion releases ownership for explicit retry. */
    rt::Task<void> releaseLocalDeletionClaim(std::shared_ptr<rt::Kv> kv, Entry entry, std::optional<LocalApiKeyDeletionClaim> claim) {
      if (!claim) co_return;
      auto guard = claim->guard; guard.local_deletion = {.owner = std::nullopt, .completed_at_ms = std::nullopt};
      auto atomic = kv->atomic(); atomic->check(entry).check(claim->check).set(claim->check.key, rt::KvValue(json::value_from(guard)));
      rt::Promise<rt::KvCommitResult> pending(atomic->commit()); co_await pending.wait();
    }
    /** A failed ID deletion can release only after a fresh read still proves the key is revoked. */
    rt::Task<void> releaseLocalDeletionClaimAfterConflict(std::shared_ptr<rt::Kv> kv, rt::KvKey idKey, std::optional<LocalApiKeyDeletionClaim> claim) {
      try {
        if (!claim) co_return;
        rt::Promise<Entry> pending(kv->get(idKey, rt::KvGetOptions{.consistency = rt::KvConsistencyLevel::strong})); const auto entry = co_await pending.wait();
        if (!entry.value["revoked_at_ms"].truthy()) co_return;
        rt::Promise<void> release(releaseLocalDeletionClaim(kv, entry, claim)); co_await release.wait();
      } catch (...) {
        rt::console::error("[ai.ubq.fi] Failed to release local API key deletion claim after a concurrent update:", rt::console::Value::object({{"error", rt::console::Value(std::current_exception())}}));
      }
    }
    rt::Task<std::optional<rt::Response>> completeLocalDeletion(std::shared_ptr<rt::Kv> kv, rt::KvKey idKey, std::optional<LocalApiKeyDeletionClaim> claim) {
      if (!claim) co_return std::nullopt;
      rt::Promise<Entry> pending(kv->get(idKey, rt::KvGetOptions{.consistency = rt::KvConsistencyLevel::strong})); const auto absentId = co_await pending.wait();
      if (absentId.value.truthy()) co_return http::openaiError(409, "Local API key deletion ownership changed", "paid_fallback_deletion_in_progress");
      auto guard = claim->guard; guard.local_deletion.completed_at_ms = rt::dateNow();
      auto atomic = kv->atomic(); atomic->check(absentId).check(claim->check).set(claim->check.key, rt::KvValue(json::value_from(guard)));
      rt::Promise<rt::KvCommitResult> completionPending(atomic->commit()); const auto completion = co_await completionPending.wait();
      if (!completion.ok) co_return http::openaiError(409, "Local API key deletion ownership changed", "paid_fallback_deletion_in_progress");
      co_return std::nullopt;
    }
    rt::Task<rt::Response> delete_(rt::Request req, Context context) {
      auto kvPending = kv::getKv(); const auto kv = co_await kvPending.wait();
      if (!kv) co_return http::openaiError(500, "Deno KV is not available; cannot manage API keys", "server_error");
      rt::Promise<json::value> parsed(request::readJsonBody(req)); const auto raw = co_await parsed.wait();
      if (!rt::truthy(raw) || !utils::isRecord(raw)) co_return http::openaiError(400, "Invalid JSON body", "invalid_request_error");
      const auto id = utils::getString(rt::get(raw, "id"));
      if (!id || id->empty()) co_return http::openaiError(400, "id is required", "invalid_request_error");
      const auto idKey = keys::apiKeyIdKey(*id);
      rt::Promise<Entry> entryPending(kv->get(idKey)); const auto entry = co_await entryPending.wait();
      if (!entry.value.truthy()) co_return http::openaiError(404, "Not found", "not_found");
      if (!entry.value["revoked_at_ms"].truthy()) co_return http::openaiError(400, "Only revoked keys can be deleted", "invalid_request_error");
      const auto guardKey = ledger::paidFallbackDeletionGuardV3Key(*id);
      rt::Promise<Entry> guardPending(kv->get(guardKey, rt::KvGetOptions{.consistency = rt::KvConsistencyLevel::strong})); const auto deletionGuard = co_await guardPending.wait();
      rt::Promise<std::variant<std::optional<LocalApiKeyDeletionClaim>, Failed>> preparation(prepareDeletionGuard(kv, *id, entry, deletionGuard));
      const auto prepared = co_await preparation.wait();
      if (const auto* failure = std::get_if<Failed>(&prepared)) co_return failure->response;
      const auto claim = std::get<std::optional<LocalApiKeyDeletionClaim>>(prepared);
      std::optional<admission::PaidFallbackDeletionV3> paidFallbackDeletion;
      rt::Reason failure;
      try { rt::Promise<admission::PaidFallbackDeletionV3> pending(admission::deletePaidFallbackStateV3(*id, kv)); paidFallbackDeletion = co_await pending.wait(); }
      catch (...) { failure = std::current_exception(); }
      if (failure) {
        rt::console::error("[ai.ubq.fi] Failed to clean V3 paid fallback state before API key deletion:", rt::console::Value::object({{"error", rt::console::Value(failure)}}));
        rt::Promise<void> release(releaseLocalDeletionClaim(kv, entry, claim)); co_await release.wait();
        co_return http::openaiError(500, "Failed to prepare paid fallback state for API key deletion", "server_error");
      }
      if (std::holds_alternative<admission::PaidFallbackDeletionV3Unavailable>(*paidFallbackDeletion)) {
        rt::Promise<void> release(releaseLocalDeletionClaim(kv, entry, claim)); co_await release.wait();
        co_return http::openaiError(500, "Deno KV is not available; cannot inspect paid fallback billing", "server_error");
      }
      if (const auto* blocked = std::get_if<admission::PaidFallbackDeletionV3Blocked>(&*paidFallbackDeletion)) {
        rt::Promise<void> release(releaseLocalDeletionClaim(kv, entry, claim)); co_await release.wait();
        const auto& outstanding = blocked->outstanding;
        co_return http::openaiError(409, "Cannot delete API key while metered billing is pending or unresolved (pending=" + rt::numberToString(outstanding.pending_requests) +
          ", unresolved=" + rt::numberToString(outstanding.unresolved_requests) + ", markers=" + rt::numberToString(outstanding.pending_markers) + ")", "paid_fallback_billing_outstanding");
      }
      auto atomic = kv->atomic();
      atomic->check(entry).delete_(idKey).delete_(keys::apiKeyHashKey(recordView(entry.value).hash)).delete_(analytics::apiKeyUsageKey(*id)).delete_(analytics::apiKeyUsageDailyKey(*id));
      if (claim) atomic->check(claim->check);
      appendChange(*atomic, "delete", *id, entry.value, json::object(), context, rt::dateNow());
      rt::Promise<rt::KvCommitResult> commitPending(atomic->commit()); const auto commit = co_await commitPending.wait();
      if (!commit.ok) {
        rt::Promise<void> release(releaseLocalDeletionClaimAfterConflict(kv, idKey, claim)); co_await release.wait();
        co_return conflict();
      }
      quota::invalidateApiKeyPolicy(*id);
      const std::vector<rt::KvKey> prefixes{analytics::apiKeyRequestLogPrefix(*id), analytics::legacyApiKeyRequestLogPrefix(*id), rt::KvKey{quota::API_KEY_USAGE_V2_PREFIX, *id}};
      for (const auto& prefix : prefixes) {
        const rt::KvListSelector selector{.prefix = prefix}; auto iterator = kv->list(selector);
        while (true) {
          rt::Promise<std::optional<rt::KvEntry<>>> next(iterator->next()); const auto item = co_await next.wait();
          if (!item) break;
          rt::Promise<void> removal(kv->delete_(item->key)); co_await removal.wait();
        }
      }
      rt::Promise<void> quotaRemoval(quota::deleteApiKeyUsageV3(kv, *id)); co_await quotaRemoval.wait();
      rt::Promise<std::optional<rt::Response>> completion(completeLocalDeletion(kv, idKey, claim)); const auto completionError = co_await completion.wait();
      if (completionError) co_return *completionError;
      const rt::KvObject body{{"id", *id}}; co_return response(body);
    }
  }
  rt::Task<rt::Response> handleAdminApiKeysUpdate(rt::Request req) { return update(std::move(req), std::nullopt); }
  rt::Task<rt::Response> handleAdminApiKeysUpdate(rt::Request req, const api_key_audit::ApiKeyAuditContext& context) { return update(std::move(req), context); }
  rt::Task<rt::Response> handleAdminApiKeysRevoke(rt::Request req) { return revocation(std::move(req), std::nullopt, false); }
  rt::Task<rt::Response> handleAdminApiKeysRevoke(rt::Request req, const api_key_audit::ApiKeyAuditContext& context) { return revocation(std::move(req), context, false); }
  rt::Task<rt::Response> handleAdminApiKeysUnrevoke(rt::Request req) { return revocation(std::move(req), std::nullopt, true); }
  rt::Task<rt::Response> handleAdminApiKeysUnrevoke(rt::Request req, const api_key_audit::ApiKeyAuditContext& context) { return revocation(std::move(req), context, true); }
  rt::Task<rt::Response> handleAdminApiKeysDelete(rt::Request req) { return delete_(std::move(req), std::nullopt); }
  rt::Task<rt::Response> handleAdminApiKeysDelete(rt::Request req, const api_key_audit::ApiKeyAuditContext& context) { return delete_(std::move(req), context); }
}
