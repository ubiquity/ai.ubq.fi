#include "uos/authority/native_api_key_policy.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/api_keys.hpp"
#include "uos/authority/native_api_key.hpp"
#include "uos/authority/native_paid_policy.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>

namespace uos::authority::native_api_key_policy {
  namespace {
    constexpr rt::StoredJsonViewOptions STORED_JSON_VIEW_OPTIONS{
      .undefinedMember = rt::StoredJsonViewOptions::UndefinedView::omit,
      .undefinedElement = rt::StoredJsonViewOptions::UndefinedView::null,
      .hole = rt::StoredJsonViewOptions::UndefinedView::null,
      .topLevelUndefined = rt::StoredJsonViewOptions::UndefinedView::emptyObject,
      .bigint = rt::StoredJsonViewOptions::BigIntView::emptyObject,
      .kvU64Members = false,
      .typedArrayMembers = false,
    };
    using ::uos::api_keys::API_KEY_NO_EXPIRATION_MS;
    using ::uos::api_keys::API_KEY_NO_USAGE_LIMIT;
    using ::uos::api_keys::USAGE_RESET_PERIOD_MS;
    using auth::local_development_key::LocalDevelopmentDeletionGuard;

    // A stored KV value as the TS reads it through property access: what JSON can hold is kept, an
    // undefined member is absent, an undefined or hole element is null, and every other part (bigint,
    // KvU64, Date, Map, buffers, ...) becomes `{}`, an object no number/string/boolean/null check accepts.

    bool isNullMember(const json::value* value) { return value != nullptr && value->is_null(); }

    bool hasUsageQuotaV3(const json::value& record) { return utils::isRecord(record) && rt::getNumber(rt::get(record, "usage_quota_version")) == 3.0; }

    // `apiKeyUsageV3PolicyVersion(normalizeApiKeyWindowMs(record.window_ms))`
    std::string policyVersion(const json::value& record) {
      return native_api_key::apiKeyUsageV3PolicyVersion(api_keys::normalizeApiKeyWindowMs(rt::get(record, "window_ms")));
    }

    // `/^u_[0-9a-f]{64}$/`
    bool matchesUosApiKey(std::string_view token) {
      if (token.size() != 66 || token.substr(0, 2) != "u_") return false;
      for (const char ch : token.substr(2)) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
      }
      return true;
    }

    // `typeof value === "string"`
    std::optional<std::string> stringOf(const json::value* value) {
      if (value == nullptr || !value->is_string()) return std::nullopt;
      return std::string(value->get_string());
    }

    // `{ ok: true; paidFallbackEnabled: boolean; paidFallbackLimitMicrocredits: number } | { ok: false; error: string }`
    struct ApiKeyCreatePaidFallbackOk {
      bool paidFallbackEnabled;
      double paidFallbackLimitMicrocredits;
    };
    using ApiKeyCreatePaidFallbackResult = std::variant<ApiKeyCreatePaidFallbackOk, ApiKeyFieldsNotOk>;

    /** `paid_fallback_enabled` and `paid_fallback_limit_credits`, validated in that order. */
    ApiKeyCreatePaidFallbackResult resolveApiKeyCreatePaidFallback(const json::object& raw) {
      if (rt::hasOwn(raw, "paid_fallback_enabled") && !rt::get(raw, "paid_fallback_enabled")->is_bool()) {
        return ApiKeyFieldsNotOk{.error = "paid_fallback_enabled must be a boolean"};
      }
      const bool paidFallbackEnabled = rt::getBool(raw, "paid_fallback_enabled") == true;
      const std::optional<double> paidFallbackLimitMicrocredits =
        rt::hasOwn(raw, "paid_fallback_limit_credits") ? api_keys::paidFallbackCreditsToMicrocredits(rt::get(raw, "paid_fallback_limit_credits")) : std::optional<double>(0);
      if (!paidFallbackLimitMicrocredits) {
        return ApiKeyFieldsNotOk{.error = "paid_fallback_limit_credits must be a non-negative number or -1"};
      }
      if (paidFallbackEnabled && *paidFallbackLimitMicrocredits == 0) {
        return ApiKeyFieldsNotOk{.error = "paid_fallback_limit_credits must be positive or -1 when paid fallback is enabled"};
      }
      return ApiKeyCreatePaidFallbackOk{.paidFallbackEnabled = paidFallbackEnabled, .paidFallbackLimitMicrocredits = *paidFallbackLimitMicrocredits};
    }

    bool apiKeyUpdateIdentityChanged(const ::uos::types::ApiKeyRecord& record, const ::uos::types::ApiKeyRecord& updated, double currentExpiresAtMs) {
      return updated.name != record.name || updated.expires_at_ms != currentExpiresAtMs;
    }

    bool apiKeyUpdateQuotaChanged(const ::uos::types::ApiKeyRecord& record, const ::uos::types::ApiKeyRecord& updated, double currentWindowMs, bool resetUsage) {
      return updated.usage_limit_requests != record.usage_limit_requests || updated.window_ms != currentWindowMs ||
             (resetUsage && (updated.usage_requests != record.usage_requests || updated.usage_reset_at_ms != record.usage_reset_at_ms));
    }

    // `updated.paid_fallback_model_ids !== record.paid_fallback_model_ids` compares the arrays by
    // identity. Every caller passes either the record's own array (which is equal by value) or a
    // freshly initialized one, and it initializes only when paid fallback switches from disabled to
    // enabled, which the first comparison already reports; comparing the contents gives the same
    // result for those calls (pinned in tests/native_api_key_policy_differential_test.cpp).
    bool apiKeyUpdatePaidFallbackChanged(const ::uos::types::ApiKeyRecord& record, const ::uos::types::ApiKeyRecord& updated) {
      return updated.paid_fallback_enabled != record.paid_fallback_enabled ||
             updated.paid_fallback_limit_microcredits != record.paid_fallback_limit_microcredits ||
             updated.paid_fallback_spent_microcredits != record.paid_fallback_spent_microcredits ||
             updated.paid_fallback_reserved_microcredits != record.paid_fallback_reserved_microcredits ||
             updated.paid_fallback_reservation_request_id != record.paid_fallback_reservation_request_id ||
             updated.paid_fallback_model_ids != record.paid_fallback_model_ids ||
             updated.paid_fallback_quota_per_credit != record.paid_fallback_quota_per_credit ||
             updated.paid_fallback_pricing_checked_at_ms != record.paid_fallback_pricing_checked_at_ms;
    }

    json::value nullableNumber(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }
  }

  std::optional<native_contracts::ApiKeyPolicy> apiKeyPolicyFromHashRecord(const std::string& tokenHash, const ::uos::types::ApiKeyHashRecord& record, double nowMs) {
    return apiKeyPolicyFromHashRecord(tokenHash, json::value_from(record), nowMs);
  }

  std::optional<native_contracts::ApiKeyPolicy> apiKeyPolicyFromHashRecord(const std::string& tokenHash, const json::value& record, double nowMs) {
    if (!native_paid_policy::hasStrictPaidFallbackPolicy(record) || !hasUsageQuotaV3(record)) return std::nullopt;
    // The TS copies `id`, `usage_limit_requests` and `usage_reset_at_ms` unchecked; a row whose
    // members do not have their declared types has no typed policy here and counts as null.
    const std::optional<std::string_view> id = rt::getString(record, "id");
    const std::optional<double> usageLimitRequests = rt::getNumber(record, "usage_limit_requests");
    const std::optional<double> usageResetAtMs = rt::getNumber(record, "usage_reset_at_ms");
    if (!id || !usageLimitRequests || !usageResetAtMs) return std::nullopt;
    const double windowMs = api_keys::normalizeApiKeyWindowMs(rt::get(record, "window_ms"));
    const native_api_key::ApiKeyUsageWindowBounds window = native_api_key::currentApiKeyUsageWindow(*usageResetAtMs, windowMs, nowMs);
    const std::optional<double> expiresAtMs = rt::getNumber(record, "expires_at_ms");
    const std::optional<std::string_view> reservationRequestId = rt::getString(record, "paid_fallback_reservation_request_id");
    return native_contracts::ApiKeyPolicy{
      .token_hash = tokenHash,
      .key_id = std::string(*id),
      .expires_at_ms = expiresAtMs && rt::Number::isFinite(*expiresAtMs) ? rt::Math::trunc(*expiresAtMs) : API_KEY_NO_EXPIRATION_MS,
      .usage_limit_requests = *usageLimitRequests,
      .window_ms = windowMs,
      .window_start_ms = window.start,
      .usage_reset_at_ms = window.reset,
      .policy_version = policyVersion(record),
      .paid_fallback_enabled = *rt::getBool(record, "paid_fallback_enabled"),
      .paid_fallback_limit_microcredits = *rt::getNumber(record, "paid_fallback_limit_microcredits"),
      .paid_fallback_spent_microcredits = *rt::getNumber(record, "paid_fallback_spent_microcredits"),
      .paid_fallback_reserved_microcredits = *rt::getNumber(record, "paid_fallback_reserved_microcredits"),
      .paid_fallback_reservation_request_id = reservationRequestId ? std::optional<std::string>(std::string(*reservationRequestId)) : std::nullopt,
    };
  }

  bool looksLikeUosApiKey(const std::string& token) { return matchesUosApiKey(rt::trim(token)); }

  std::optional<std::string> normalizeApiKeyName(const json::value* value) {
    const std::optional<std::string> text = stringOf(value);
    if (!text) return std::nullopt;
    const std::string name = rt::trim(*text);
    if (name.empty()) return std::nullopt;
    if (rt::length(name) > 80) return std::nullopt;
    if (name.find_first_of("\r\n") != std::string::npos) return std::nullopt;
    return name;
  }
  std::optional<std::string> normalizeApiKeyName(const json::value& value) { return normalizeApiKeyName(&value); }

  std::optional<std::string> normalizeOptionalApiKeyToken(const json::value* value) {
    if (rt::isNullish(value)) return std::nullopt;
    const std::optional<std::string> text = stringOf(value);
    if (!text) return std::nullopt;
    std::string token = rt::trim(*text);
    if (!looksLikeUosApiKey(token)) return std::nullopt;
    return token;
  }
  std::optional<std::string> normalizeOptionalApiKeyToken(const json::value& value) { return normalizeOptionalApiKeyToken(&value); }

  std::optional<double> normalizeApiKeyExpiresAtMs(const json::value* value, double nowMs, double defaultExpiryMs) {
    if (rt::isNullish(value)) return defaultExpiryMs;
    const std::optional<double> number = rt::getNumber(value);
    if (!number || !rt::Number::isFinite(*number)) return std::nullopt;
    const double expiresAtMs = rt::Math::trunc(*number);
    if (expiresAtMs == API_KEY_NO_EXPIRATION_MS) return API_KEY_NO_EXPIRATION_MS;
    if (expiresAtMs < 0) return std::nullopt;
    if (expiresAtMs <= nowMs) return std::nullopt;
    return expiresAtMs;
  }
  std::optional<double> normalizeApiKeyExpiresAtMs(const json::value& value, double nowMs, double defaultExpiryMs) {
    return normalizeApiKeyExpiresAtMs(&value, nowMs, defaultExpiryMs);
  }

  std::optional<double> normalizeApiKeyUsageLimit(const json::value* value, double defaultUsageLimit) {
    if (rt::isNullish(value)) return defaultUsageLimit;
    const std::optional<double> number = rt::getNumber(value);
    if (!number || !rt::Number::isFinite(*number)) return std::nullopt;
    const double limit = rt::Math::trunc(*number);
    if (limit == API_KEY_NO_USAGE_LIMIT) return API_KEY_NO_USAGE_LIMIT;
    if (limit < 0) return std::nullopt;
    return limit;
  }
  std::optional<double> normalizeApiKeyUsageLimit(const json::value& value, double defaultUsageLimit) { return normalizeApiKeyUsageLimit(&value, defaultUsageLimit); }

  std::optional<double> normalizeApiKeyWindowMsInput(const json::value* value) {
    if (rt::isNullish(value)) return std::nullopt;
    std::optional<double> number = rt::getNumber(value);
    if (value->is_string()) {
      const std::string trimmed = rt::trim(std::string_view(value->get_string()));
      if (trimmed.empty()) return std::nullopt;
      const double parsed = rt::stringToNumber(trimmed);
      if (!rt::Number::isFinite(parsed)) return std::nullopt;
      number = parsed;
    }
    if (!number || !rt::Number::isFinite(*number)) return std::nullopt;
    const double windowMs = rt::Math::trunc(*number);
    if (windowMs <= 0) return std::nullopt;
    return windowMs;
  }
  std::optional<double> normalizeApiKeyWindowMsInput(const json::value& value) { return normalizeApiKeyWindowMsInput(&value); }

  ResolveApiKeyCreateFieldsResult resolveApiKeyCreateFields(const json::object& raw, double nowMs, const ResolveApiKeyCreateFieldsDefaults& defaults) {
    const std::optional<double> expiresAtMs = normalizeApiKeyExpiresAtMs(rt::get(raw, "expires_at_ms"), nowMs, defaults.expiryMs);
    if (!expiresAtMs) {
      return ApiKeyFieldsNotOk{.error = "expires_at_ms must be a Unix epoch ms timestamp in the future, or -1"};
    }

    const std::optional<double> usageLimitRequests = normalizeApiKeyUsageLimit(rt::get(raw, "usage_limit_requests"), defaults.usageLimitRequests);
    if (!usageLimitRequests) {
      return ApiKeyFieldsNotOk{.error = "usage_limit_requests must be a positive number or -1 for unlimited"};
    }

    const std::optional<double> windowMs = normalizeApiKeyWindowMsInput(rt::get(raw, "window_ms"));
    if (rt::get(raw, "window_ms") != nullptr && !windowMs) {
      return ApiKeyFieldsNotOk{.error = "window_ms must be a positive number"};
    }

    const ApiKeyCreatePaidFallbackResult paidFallback = resolveApiKeyCreatePaidFallback(raw);
    if (const auto* failure = std::get_if<ApiKeyFieldsNotOk>(&paidFallback)) return *failure;
    const ApiKeyCreatePaidFallbackOk& resolved = std::get<ApiKeyCreatePaidFallbackOk>(paidFallback);

    return ResolveApiKeyCreateFieldsOk{
      .fields =
        ApiKeyCreateFields{
          .expiresAtMs = *expiresAtMs,
          .usageLimitRequests = *usageLimitRequests,
          .windowMs = windowMs.value_or(USAGE_RESET_PERIOD_MS),
          .paidFallbackEnabled = resolved.paidFallbackEnabled,
          .paidFallbackLimitMicrocredits = resolved.paidFallbackLimitMicrocredits,
        },
    };
  }

  ResolveApiKeyUpdateIdentityResult resolveApiKeyUpdateIdentity(const json::object& raw, const ::uos::types::ApiKeyRecord& record, double currentExpiresAtMs,
                                                                double nowMs, double defaultExpiryMs) {
    std::string name = record.name;
    if (rt::hasOwn(raw, "name")) {
      const std::optional<std::string> normalized = normalizeApiKeyName(rt::get(raw, "name"));
      if (!normalized) return ApiKeyFieldsNotOk{.error = "name must be a non-empty string (<=80 chars)"};
      name = *normalized;
    }

    double expiresAtMs = currentExpiresAtMs;
    if (rt::hasOwn(raw, "expires_at_ms")) {
      const std::optional<double> normalized = normalizeApiKeyExpiresAtMs(rt::get(raw, "expires_at_ms"), nowMs, defaultExpiryMs);
      if (!normalized) {
        return ApiKeyFieldsNotOk{
          .error = "expires_at_ms must be a Unix epoch ms timestamp in the future, or -1",
        };
      }
      expiresAtMs = *normalized;
    }

    return ResolveApiKeyUpdateIdentityOk{.identity = ApiKeyUpdateIdentity{.name = std::move(name), .expiresAtMs = expiresAtMs}};
  }

  ResolveApiKeyUpdateQuotaResult resolveApiKeyUpdateQuota(const json::object& raw, const ::uos::types::ApiKeyRecord& record, double defaultUsageLimit) {
    double usageLimitRequests = record.usage_limit_requests;
    if (rt::hasOwn(raw, "usage_limit_requests")) {
      const std::optional<double> normalized = normalizeApiKeyUsageLimit(rt::get(raw, "usage_limit_requests"), defaultUsageLimit);
      if (!normalized) {
        return ApiKeyFieldsNotOk{.error = "usage_limit_requests must be a non-negative number or -1 for unlimited"};
      }
      usageLimitRequests = *normalized;
    }

    // `coerceApiKeyWindowMs(record)`: a record is always a record, so this is its window_ms normalized.
    double windowMs = api_keys::normalizeApiKeyWindowMs(record.window_ms);
    if (rt::hasOwn(raw, "window_ms")) {
      const std::optional<double> normalized = normalizeApiKeyWindowMsInput(rt::get(raw, "window_ms"));
      if (!normalized) {
        return ApiKeyFieldsNotOk{.error = "window_ms must be a positive number"};
      }
      windowMs = *normalized;
    }

    return ResolveApiKeyUpdateQuotaOk{.quota = ApiKeyUpdateQuota{.usageLimitRequests = usageLimitRequests, .windowMs = windowMs}};
  }

  ResolveApiKeyUpdatePaidFallbackResult resolveApiKeyUpdatePaidFallback(const json::object& raw, const ::uos::types::ApiKeyRecord& record) {
    if (rt::hasOwn(raw, "paid_fallback_enabled") && !rt::get(raw, "paid_fallback_enabled")->is_bool()) {
      return ApiKeyFieldsNotOk{.error = "paid_fallback_enabled must be a boolean"};
    }
    bool paidFallbackEnabled = record.paid_fallback_enabled;
    if (rt::hasOwn(raw, "paid_fallback_enabled")) {
      paidFallbackEnabled = rt::getBool(raw, "paid_fallback_enabled") == true;
    }

    double paidFallbackLimitMicrocredits = record.paid_fallback_limit_microcredits;
    if (rt::hasOwn(raw, "paid_fallback_limit_credits")) {
      const std::optional<double> limitMicrocredits = api_keys::paidFallbackCreditsToMicrocredits(rt::get(raw, "paid_fallback_limit_credits"));
      if (!limitMicrocredits) {
        return ApiKeyFieldsNotOk{.error = "paid_fallback_limit_credits must be a non-negative number or -1"};
      }
      paidFallbackLimitMicrocredits = *limitMicrocredits;
    }
    if (paidFallbackEnabled && paidFallbackLimitMicrocredits == 0) {
      return ApiKeyFieldsNotOk{.error = "paid_fallback_limit_credits must be positive or -1 when paid fallback is enabled"};
    }

    ApiKeyUpdatePaidFallback paidFallback{
      .paidFallbackEnabled = paidFallbackEnabled,
      .paidFallbackLimitMicrocredits = paidFallbackLimitMicrocredits,
      .paidFallbackModelIds = record.paid_fallback_model_ids,
      .paidFallbackQuotaPerCredit = record.paid_fallback_quota_per_credit,
      .paidFallbackMaxExposureMicrocredits = record.paid_fallback_max_exposure_microcredits.value_or(json::object()),
      .paidFallbackPricingCheckedAtMs = record.paid_fallback_pricing_checked_at_ms,
    };
    return ResolveApiKeyUpdatePaidFallbackOk{
      .paidFallback = std::move(paidFallback),
      .needsInitialization = !record.paid_fallback_enabled && paidFallbackEnabled,
    };
  }

  BuildApiKeyUpdateRecordResult buildApiKeyUpdateRecord(const ::uos::types::ApiKeyRecord& record, const ApiKeyUpdateFields& fields, double nowMs, double currentWindowMs) {
    const bool replaceQuotaWindow = fields.resetUsage || fields.windowMs != currentWindowMs;
    double usageRequests = record.usage_requests;
    double usageResetAtMs = record.usage_reset_at_ms;
    double paidFallbackSpentMicrocredits = record.paid_fallback_spent_microcredits;
    if (replaceQuotaWindow) {
      usageRequests = 0;
      const double currentWindowStartMs = record.usage_reset_at_ms - currentWindowMs;
      const double freshWindowStartMs = rt::Math::max(nowMs, currentWindowStartMs + 1);
      usageResetAtMs = freshWindowStartMs + fields.windowMs;
      paidFallbackSpentMicrocredits = 0;
    }
    ::uos::types::ApiKeyRecord updated = record;
    updated.name = fields.name;
    updated.expires_at_ms = fields.expiresAtMs;
    updated.usage_limit_requests = fields.usageLimitRequests;
    updated.usage_requests = usageRequests;
    updated.usage_reset_at_ms = usageResetAtMs;
    updated.window_ms = fields.windowMs;
    updated.paid_fallback_enabled = fields.paidFallbackEnabled;
    updated.paid_fallback_limit_microcredits = fields.paidFallbackLimitMicrocredits;
    updated.paid_fallback_spent_microcredits = paidFallbackSpentMicrocredits;
    updated.paid_fallback_reserved_microcredits = record.paid_fallback_reserved_microcredits;
    updated.paid_fallback_reservation_request_id = record.paid_fallback_reservation_request_id;
    updated.paid_fallback_model_ids = fields.paidFallbackModelIds;
    updated.paid_fallback_quota_per_credit = fields.paidFallbackQuotaPerCredit;
    updated.paid_fallback_max_exposure_microcredits = fields.paidFallbackMaxExposureMicrocredits;
    updated.paid_fallback_pricing_checked_at_ms = fields.paidFallbackPricingCheckedAtMs;
    return BuildApiKeyUpdateRecordResult{
      .updated = std::move(updated),
      .resetUsage = fields.resetUsage,
      .replaceQuotaWindow = replaceQuotaWindow,
    };
  }

  bool apiKeyUpdateChanged(const ::uos::types::ApiKeyRecord& record, const ::uos::types::ApiKeyRecord& updated, double currentExpiresAtMs, double currentWindowMs,
                           bool resetUsage) {
    return apiKeyUpdateIdentityChanged(record, updated, currentExpiresAtMs) || apiKeyUpdateQuotaChanged(record, updated, currentWindowMs, resetUsage) ||
           apiKeyUpdatePaidFallbackChanged(record, updated);
  }

  json::object apiKeyUpdateAuditChanges(const ::uos::types::ApiKeyRecord& record, const ::uos::types::ApiKeyRecord& updated, double currentExpiresAtMs,
                                        double currentWindowMs, bool resetUsage) {
    json::object changed;
    if (updated.name != record.name) rt::objectSet(changed, "name", json::value(json::string(updated.name)));
    if (updated.expires_at_ms != currentExpiresAtMs) rt::objectSet(changed, "expires_at_ms", rt::makeNumber(updated.expires_at_ms));
    if (updated.usage_limit_requests != record.usage_limit_requests) rt::objectSet(changed, "usage_limit_requests", rt::makeNumber(updated.usage_limit_requests));
    if (updated.window_ms != currentWindowMs) rt::objectSet(changed, "window_ms", rt::makeNumber(updated.window_ms));
    if (updated.paid_fallback_enabled != record.paid_fallback_enabled) rt::objectSet(changed, "paid_fallback_enabled", json::value(updated.paid_fallback_enabled));
    if (updated.paid_fallback_limit_microcredits != record.paid_fallback_limit_microcredits) {
      rt::objectSet(changed, "paid_fallback_limit_credits", rt::makeNumber(api_keys::paidFallbackMicrocreditsToCredits(updated.paid_fallback_limit_microcredits)));
    }
    if (resetUsage) rt::objectSet(changed, "reset_usage", json::value(true));
    return changed;
  }

  std::optional<LocalDevelopmentDeletionGuard> retryableLocalDeletionGuard(const json::value* value) {
    if (!utils::isRecord(value) || !utils::isRecord(rt::get(value, "local_deletion"))) return std::nullopt;
    const std::optional<double> createdAtMs = rt::getNumber(rt::get(value, "created_at_ms"));
    if (!createdAtMs || !rt::Number::isSafeInteger(*createdAtMs) || *createdAtMs <= 0) return std::nullopt;
    const json::value* localDeletion = rt::get(value, "local_deletion");
    if (!isNullMember(rt::get(localDeletion, "owner")) || !isNullMember(rt::get(localDeletion, "completed_at_ms"))) return std::nullopt;
    return LocalDevelopmentDeletionGuard{
      .created_at_ms = *createdAtMs,
      .local_deletion = LocalDevelopmentDeletionGuard::LocalDeletion{.owner = std::nullopt, .completed_at_ms = std::nullopt},
    };
  }
  std::optional<LocalDevelopmentDeletionGuard> retryableLocalDeletionGuard(const json::value& value) { return retryableLocalDeletionGuard(&value); }
  std::optional<LocalDevelopmentDeletionGuard> retryableLocalDeletionGuard(const rt::KvValue& value) {
    if (value.isUndefined()) return retryableLocalDeletionGuard(static_cast<const json::value*>(nullptr));
    const json::value view = rt::storedJsonView(value, STORED_JSON_VIEW_OPTIONS);
    return retryableLocalDeletionGuard(&view);
  }

  std::optional<std::string> rejectRetiredApiKeyResetSetting(const json::object& raw) {
    if (rt::hasOwn(raw, "banked_resets_enabled")) return std::string("banked_resets_enabled is retired; configure banked resets per Codex subscription");
    return std::nullopt;
  }

  ResolveApiKeyResetUsageResult resolveApiKeyResetUsage(const json::object& raw) {
    if (rt::hasOwn(raw, "reset_usage") && !rt::get(raw, "reset_usage")->is_bool()) return ApiKeyFieldsNotOk{.error = "reset_usage must be a boolean"};
    return ResolveApiKeyResetUsageOk{.resetUsage = rt::getBool(raw, "reset_usage") == true};
  }

  ::uos::types::ApiKeyHashRecord apiKeyHashRecordFor(const ::uos::types::ApiKeyRecord& record) { return apiKeyHashRecordFor(record, record.id); }

  ::uos::types::ApiKeyHashRecord apiKeyHashRecordFor(const ::uos::types::ApiKeyRecord& record, const std::string& id) {
    const native_paid_policy::PaidFallbackHashFields paid = native_paid_policy::paidFallbackHashFields(record);
    return ::uos::types::ApiKeyHashRecord{
      .id = id,
      .expires_at_ms = record.expires_at_ms,
      .revoked_at_ms = record.revoked_at_ms,
      .usage_limit_requests = record.usage_limit_requests,
      .usage_requests = record.usage_requests,
      .usage_reset_at_ms = record.usage_reset_at_ms,
      .window_ms = record.window_ms,
      .usage_quota_version = record.usage_quota_version,
      .paid_fallback_enabled = paid.paid_fallback_enabled,
      .paid_fallback_limit_microcredits = paid.paid_fallback_limit_microcredits,
      .paid_fallback_spent_microcredits = paid.paid_fallback_spent_microcredits,
      .paid_fallback_reserved_microcredits = paid.paid_fallback_reserved_microcredits,
      .paid_fallback_reservation_request_id = paid.paid_fallback_reservation_request_id,
    };
  }

  BuildApiKeyCreateRecordsResult buildApiKeyCreateRecords(const BuildApiKeyCreateRecordsIdentity& identity, const ApiKeyCreateFields& fields,
                                                          const paid_fallback::index::PaidFallbackPolicyFields& policy, double now) {
    const double usageResetAtMs = api_keys::calculateNextResetMs(now, fields.windowMs);
    ::uos::types::ApiKeyRecord record{
      .id = identity.id,
      .name = identity.name,
      .prefix = identity.prefix,
      .hash = identity.hash,
      .created_at_ms = now,
      .expires_at_ms = fields.expiresAtMs,
      .revoked_at_ms = std::nullopt,
      .usage_limit_requests = fields.usageLimitRequests,
      .usage_requests = 0,
      .usage_reset_at_ms = usageResetAtMs,
      .window_ms = fields.windowMs,
      .usage_quota_version = 3,
      .paid_fallback_enabled = policy.paid_fallback_enabled,
      .paid_fallback_limit_microcredits = policy.paid_fallback_limit_microcredits,
      .paid_fallback_spent_microcredits = policy.paid_fallback_spent_microcredits,
      .paid_fallback_reserved_microcredits = policy.paid_fallback_reserved_microcredits,
      .paid_fallback_reservation_request_id = policy.paid_fallback_reservation_request_id,
      .paid_fallback_model_ids = policy.paid_fallback_model_ids,
      .paid_fallback_quota_per_credit = policy.paid_fallback_quota_per_credit,
      .paid_fallback_max_exposure_microcredits = policy.paid_fallback_max_exposure_microcredits,
      .paid_fallback_pricing_checked_at_ms = policy.paid_fallback_pricing_checked_at_ms,
    };
    const native_paid_policy::PaidFallbackHashFields paid = native_paid_policy::paidFallbackHashFields(record);
    ::uos::types::ApiKeyHashRecord hashRecord{
      .id = identity.id,
      .expires_at_ms = fields.expiresAtMs,
      .revoked_at_ms = std::nullopt,
      .usage_limit_requests = fields.usageLimitRequests,
      .usage_requests = 0,
      .usage_reset_at_ms = usageResetAtMs,
      .window_ms = fields.windowMs,
      .usage_quota_version = 3,
      .paid_fallback_enabled = paid.paid_fallback_enabled,
      .paid_fallback_limit_microcredits = paid.paid_fallback_limit_microcredits,
      .paid_fallback_spent_microcredits = paid.paid_fallback_spent_microcredits,
      .paid_fallback_reserved_microcredits = paid.paid_fallback_reserved_microcredits,
      .paid_fallback_reservation_request_id = paid.paid_fallback_reservation_request_id,
    };
    return BuildApiKeyCreateRecordsResult{.record = std::move(record), .hashRecord = std::move(hashRecord)};
  }

  ::uos::types::ApiKeyRecord buildApiKeyRevocationRecord(const ::uos::types::ApiKeyRecord& record, double expiresAtMs, double now) {
    ::uos::types::ApiKeyRecord updated = record;
    updated.expires_at_ms = expiresAtMs;
    if (!rt::truthy(record.revoked_at_ms)) updated.revoked_at_ms = now;
    return updated;
  }

  ::uos::types::ApiKeyRecord buildApiKeyUnrevocationRecord(const ::uos::types::ApiKeyRecord& record, double expiresAtMs) {
    ::uos::types::ApiKeyRecord updated = record;
    updated.expires_at_ms = expiresAtMs;
    updated.revoked_at_ms = std::nullopt;
    return updated;
  }

  json::object apiKeyRevokeAuditChanges(const ::uos::types::ApiKeyRecord& record, const ::uos::types::ApiKeyRecord& updated) {
    json::object changes;
    if (updated.revoked_at_ms != record.revoked_at_ms) rt::objectDefine(changes, "revoked_at_ms", nullableNumber(updated.revoked_at_ms));
    if (updated.expires_at_ms != record.expires_at_ms) rt::objectDefine(changes, "expires_at_ms", rt::makeNumber(updated.expires_at_ms));
    return changes;
  }

  std::string apiKeyUnrevokeDecision(const ::uos::types::ApiKeyRecord& record, const json::value* deletionGuard) {
    if (rt::truthy(deletionGuard)) return "blocked";
    return !rt::truthy(record.revoked_at_ms) ? "unchanged" : "write";
  }
  std::string apiKeyUnrevokeDecision(const ::uos::types::ApiKeyRecord& record, const json::value& deletionGuard) { return apiKeyUnrevokeDecision(record, &deletionGuard); }
  std::string apiKeyUnrevokeDecision(const ::uos::types::ApiKeyRecord& record, const rt::KvValue& deletionGuard) {
    if (deletionGuard.truthy()) return "blocked";
    return !rt::truthy(record.revoked_at_ms) ? "unchanged" : "write";
  }

  std::string apiKeyDeleteDecision(const ::uos::types::ApiKeyRecord& record) { return !rt::truthy(record.revoked_at_ms) ? "requires_revocation" : "delete"; }
}
