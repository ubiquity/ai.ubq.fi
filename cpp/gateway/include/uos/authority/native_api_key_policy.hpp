#pragma once
// Port of src/authority/native-api-key-policy.ts.
// Source-owned native API-key parsing and admin decisions. Native wrappers retain I/O, clocks, random IDs and response formatting.
//
// `value: unknown` parameters have a `const json::value&` and a `const json::value*` overload
// (nullptr is undefined); `raw: Record<string, unknown>` is the request body object. Every
// `{ ok: true; ... } | { ok: false; error: string }` result is a std::variant whose failure
// alternative is ApiKeyFieldsNotOk.
#include "uos/auth/local_development_key.hpp"
#include "uos/authority/native_contracts.hpp"
#include "uos/paid_fallback/index.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace uos::authority::native_api_key_policy {
  // The typed overload is for records the caller built; KV readers pass the stored row as JSON
  // (the TS `ApiKeyHashRecord` is unchecked storage there), so a malformed row yields null as in
  // the TS instead of throwing in a strict conversion.
  std::optional<native_contracts::ApiKeyPolicy> apiKeyPolicyFromHashRecord(const std::string& tokenHash, const ::uos::types::ApiKeyHashRecord& record, double nowMs);
  std::optional<native_contracts::ApiKeyPolicy> apiKeyPolicyFromHashRecord(const std::string& tokenHash, const json::value& record, double nowMs);

  bool looksLikeUosApiKey(const std::string& token);

  std::optional<std::string> normalizeApiKeyName(const json::value* value);
  std::optional<std::string> normalizeApiKeyName(const json::value& value);

  std::optional<std::string> normalizeOptionalApiKeyToken(const json::value* value);
  std::optional<std::string> normalizeOptionalApiKeyToken(const json::value& value);

  std::optional<double> normalizeApiKeyExpiresAtMs(const json::value* value, double nowMs, double defaultExpiryMs);
  std::optional<double> normalizeApiKeyExpiresAtMs(const json::value& value, double nowMs, double defaultExpiryMs);

  std::optional<double> normalizeApiKeyUsageLimit(const json::value* value, double defaultUsageLimit);
  std::optional<double> normalizeApiKeyUsageLimit(const json::value& value, double defaultUsageLimit);

  std::optional<double> normalizeApiKeyWindowMsInput(const json::value* value);
  std::optional<double> normalizeApiKeyWindowMsInput(const json::value& value);

  struct ApiKeyCreateFields {
    double expiresAtMs;
    double usageLimitRequests;
    double windowMs;
    bool paidFallbackEnabled;
    double paidFallbackLimitMicrocredits;
    friend bool operator==(const ApiKeyCreateFields&, const ApiKeyCreateFields&) = default;
  };

  // `{ ok: false; error: string }`, the failure of every resolver in this module.
  struct ApiKeyFieldsNotOk {
    static constexpr bool ok = false;
    std::string error;
    friend bool operator==(const ApiKeyFieldsNotOk&, const ApiKeyFieldsNotOk&) = default;
  };

  // The `defaults` of resolveApiKeyCreateFields: `Readonly<{ expiryMs: number; usageLimitRequests: number }>`.
  struct ResolveApiKeyCreateFieldsDefaults {
    double expiryMs;
    double usageLimitRequests;
    friend bool operator==(const ResolveApiKeyCreateFieldsDefaults&, const ResolveApiKeyCreateFieldsDefaults&) = default;
  };

  struct ResolveApiKeyCreateFieldsOk {
    static constexpr bool ok = true;
    ApiKeyCreateFields fields;
    friend bool operator==(const ResolveApiKeyCreateFieldsOk&, const ResolveApiKeyCreateFieldsOk&) = default;
  };
  using ResolveApiKeyCreateFieldsResult = std::variant<ResolveApiKeyCreateFieldsOk, ApiKeyFieldsNotOk>;

  /** Every create-time field except `name` and `token`, validated in the original order. */
  ResolveApiKeyCreateFieldsResult resolveApiKeyCreateFields(const json::object& raw, double nowMs, const ResolveApiKeyCreateFieldsDefaults& defaults);

  struct ApiKeyUpdateFields {
    std::string name;
    double expiresAtMs;
    double usageLimitRequests;
    double windowMs;
    bool paidFallbackEnabled;
    double paidFallbackLimitMicrocredits;
    std::vector<std::string> paidFallbackModelIds;
    double paidFallbackQuotaPerCredit;
    json::object paidFallbackMaxExposureMicrocredits;  // Record<string, number>
    std::optional<double> paidFallbackPricingCheckedAtMs;
    bool resetUsage;
    friend bool operator==(const ApiKeyUpdateFields&, const ApiKeyUpdateFields&) = default;
  };

  // Pick<ApiKeyUpdateFields, "name" | "expiresAtMs">
  struct ApiKeyUpdateIdentity {
    std::string name;
    double expiresAtMs;
    friend bool operator==(const ApiKeyUpdateIdentity&, const ApiKeyUpdateIdentity&) = default;
  };
  // Pick<ApiKeyUpdateFields, "usageLimitRequests" | "windowMs">
  struct ApiKeyUpdateQuota {
    double usageLimitRequests;
    double windowMs;
    friend bool operator==(const ApiKeyUpdateQuota&, const ApiKeyUpdateQuota&) = default;
  };
  // Pick<ApiKeyUpdateFields, "paidFallbackEnabled" | "paidFallbackLimitMicrocredits" | "paidFallbackModelIds" |
  //   "paidFallbackQuotaPerCredit" | "paidFallbackMaxExposureMicrocredits" | "paidFallbackPricingCheckedAtMs">
  struct ApiKeyUpdatePaidFallback {
    bool paidFallbackEnabled;
    double paidFallbackLimitMicrocredits;
    std::vector<std::string> paidFallbackModelIds;
    double paidFallbackQuotaPerCredit;
    json::object paidFallbackMaxExposureMicrocredits;  // Record<string, number>
    std::optional<double> paidFallbackPricingCheckedAtMs;
    friend bool operator==(const ApiKeyUpdatePaidFallback&, const ApiKeyUpdatePaidFallback&) = default;
  };

  struct ResolveApiKeyUpdateIdentityOk {
    static constexpr bool ok = true;
    ApiKeyUpdateIdentity identity;
    friend bool operator==(const ResolveApiKeyUpdateIdentityOk&, const ResolveApiKeyUpdateIdentityOk&) = default;
  };
  using ResolveApiKeyUpdateIdentityResult = std::variant<ResolveApiKeyUpdateIdentityOk, ApiKeyFieldsNotOk>;

  /** `name` and `expires_at_ms`, validated in that order. */
  ResolveApiKeyUpdateIdentityResult resolveApiKeyUpdateIdentity(const json::object& raw, const ::uos::types::ApiKeyRecord& record, double currentExpiresAtMs,
                                                                double nowMs, double defaultExpiryMs);

  struct ResolveApiKeyUpdateQuotaOk {
    static constexpr bool ok = true;
    ApiKeyUpdateQuota quota;
    friend bool operator==(const ResolveApiKeyUpdateQuotaOk&, const ResolveApiKeyUpdateQuotaOk&) = default;
  };
  using ResolveApiKeyUpdateQuotaResult = std::variant<ResolveApiKeyUpdateQuotaOk, ApiKeyFieldsNotOk>;

  /** `usage_limit_requests` and `window_ms`, validated in that order. */
  ResolveApiKeyUpdateQuotaResult resolveApiKeyUpdateQuota(const json::object& raw, const ::uos::types::ApiKeyRecord& record, double defaultUsageLimit);

  struct ResolveApiKeyUpdatePaidFallbackOk {
    static constexpr bool ok = true;
    ApiKeyUpdatePaidFallback paidFallback;
    bool needsInitialization;
    friend bool operator==(const ResolveApiKeyUpdatePaidFallbackOk&, const ResolveApiKeyUpdatePaidFallbackOk&) = default;
  };
  using ResolveApiKeyUpdatePaidFallbackResult = std::variant<ResolveApiKeyUpdatePaidFallbackOk, ApiKeyFieldsNotOk>;

  ResolveApiKeyUpdatePaidFallbackResult resolveApiKeyUpdatePaidFallback(const json::object& raw, const ::uos::types::ApiKeyRecord& record);

  // `{ updated: ApiKeyRecord; resetUsage: boolean; replaceQuotaWindow: boolean }`
  struct BuildApiKeyUpdateRecordResult {
    ::uos::types::ApiKeyRecord updated;
    bool resetUsage;
    bool replaceQuotaWindow;
    friend bool operator==(const BuildApiKeyUpdateRecordResult&, const BuildApiKeyUpdateRecordResult&) = default;
  };

  /**
   * Merges the patch into the stored record. A reset (explicit, or implied by a
   * window change) must always select a distinct V3 aggregate identity: a create
   * followed by an immediate reset can otherwise share the same millisecond start
   * and overwrite the current window instead of opening a fresh one.
   */
  BuildApiKeyUpdateRecordResult buildApiKeyUpdateRecord(const ::uos::types::ApiKeyRecord& record, const ApiKeyUpdateFields& fields, double nowMs, double currentWindowMs);

  /** Every comparison is pure, so this matches the original short-circuit chain. */
  // The TS compares `paid_fallback_model_ids` by array identity (`!==`): see the module port.
  bool apiKeyUpdateChanged(const ::uos::types::ApiKeyRecord& record, const ::uos::types::ApiKeyRecord& updated, double currentExpiresAtMs, double currentWindowMs,
                           bool resetUsage);

  /** The safe policy fields that actually changed, ready for the audit allowlist. */
  // `Record<string, unknown>`: members in the order the TS assigns them.
  json::object apiKeyUpdateAuditChanges(const ::uos::types::ApiKeyRecord& record, const ::uos::types::ApiKeyRecord& updated, double currentExpiresAtMs,
                                        double currentWindowMs, bool resetUsage);

  // `value: unknown`; the rt::KvValue overload takes a stored guard as read (`entry.value`).
  // std::nullopt is null. A json::object argument is ambiguous between the json::value and
  // rt::KvValue overloads (here and in apiKeyUnrevokeDecision): pass it as json::value.
  std::optional<auth::local_development_key::LocalDevelopmentDeletionGuard> retryableLocalDeletionGuard(const json::value* value);
  std::optional<auth::local_development_key::LocalDevelopmentDeletionGuard> retryableLocalDeletionGuard(const json::value& value);
  std::optional<auth::local_development_key::LocalDevelopmentDeletionGuard> retryableLocalDeletionGuard(const rt::KvValue& value);

  // `string | null`: std::nullopt is null.
  std::optional<std::string> rejectRetiredApiKeyResetSetting(const json::object& raw);

  struct ResolveApiKeyResetUsageOk {
    static constexpr bool ok = true;
    bool resetUsage;
    friend bool operator==(const ResolveApiKeyResetUsageOk&, const ResolveApiKeyResetUsageOk&) = default;
  };
  using ResolveApiKeyResetUsageResult = std::variant<ResolveApiKeyResetUsageOk, ApiKeyFieldsNotOk>;

  ResolveApiKeyResetUsageResult resolveApiKeyResetUsage(const json::object& raw);

  // `id = record.id`: the one-argument overload uses record.id.
  ::uos::types::ApiKeyHashRecord apiKeyHashRecordFor(const ::uos::types::ApiKeyRecord& record);
  ::uos::types::ApiKeyHashRecord apiKeyHashRecordFor(const ::uos::types::ApiKeyRecord& record, const std::string& id);

  // The `identity` of buildApiKeyCreateRecords: `Readonly<{ id: string; name: string; prefix: string; hash: string }>`.
  struct BuildApiKeyCreateRecordsIdentity {
    std::string id;
    std::string name;
    std::string prefix;
    std::string hash;
    friend bool operator==(const BuildApiKeyCreateRecordsIdentity&, const BuildApiKeyCreateRecordsIdentity&) = default;
  };

  // `{ record: ApiKeyRecord; hashRecord: ApiKeyHashRecord }`
  struct BuildApiKeyCreateRecordsResult {
    ::uos::types::ApiKeyRecord record;
    ::uos::types::ApiKeyHashRecord hashRecord;
    friend bool operator==(const BuildApiKeyCreateRecordsResult&, const BuildApiKeyCreateRecordsResult&) = default;
  };

  // `policy: Pick<ApiKeyRecord, ...the nine paid_fallback_* members>`: the same Pick as
  // PaidFallbackPolicyFields.
  BuildApiKeyCreateRecordsResult buildApiKeyCreateRecords(const BuildApiKeyCreateRecordsIdentity& identity, const ApiKeyCreateFields& fields,
                                                          const paid_fallback::index::PaidFallbackPolicyFields& policy, double now);

  ::uos::types::ApiKeyRecord buildApiKeyRevocationRecord(const ::uos::types::ApiKeyRecord& record, double expiresAtMs, double now);
  ::uos::types::ApiKeyRecord buildApiKeyUnrevocationRecord(const ::uos::types::ApiKeyRecord& record, double expiresAtMs);
  // `Record<string, unknown>`: `revoked_at_ms` then `expires_at_ms`, each only when it changed.
  json::object apiKeyRevokeAuditChanges(const ::uos::types::ApiKeyRecord& record, const ::uos::types::ApiKeyRecord& updated);
  // `deletionGuard: unknown` (only its truthiness is read); the rt::KvValue overload takes a stored
  // value as read. Returns "blocked" | "unchanged" | "write".
  std::string apiKeyUnrevokeDecision(const ::uos::types::ApiKeyRecord& record, const json::value* deletionGuard);
  std::string apiKeyUnrevokeDecision(const ::uos::types::ApiKeyRecord& record, const json::value& deletionGuard);
  std::string apiKeyUnrevokeDecision(const ::uos::types::ApiKeyRecord& record, const rt::KvValue& deletionGuard);
  // "requires_revocation" | "delete"
  std::string apiKeyDeleteDecision(const ::uos::types::ApiKeyRecord& record);
}
