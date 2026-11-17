#pragma once
// Port of src/cache/kv-migration-paid-fallback.ts: paid fallback v3 inspection, split out of
// src/kv_migration.ts.
//
// `kv: Kv` is a non-null std::shared_ptr<rt::Kv>. The `value: unknown` guards and the record
// comparisons read stored rows as rt::KvValue (what a KV read returns; a json::value converts
// implicitly), with JavaScript's semantics for every stored kind (rt/kv_js.hpp). Key parts are
// passed as stored values with rt::kvValueFromKeyPart (an absent part is undefined, rt::KvValue()).
// `ApiKeyRecord` / `ApiKeyHashRecord` / `ApiKeyRequestLogRecord` parameters are the stored rows
// themselves (the TS casts unchecked storage after the guards), so they are rt::KvValue too.
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::cache::kv_migration_paid_fallback {
  inline const rt::KvKey KV_READ_INCIDENT_V2_MIGRATION_KEY{"uos_ai", "migrations", "kv_read_incident_v2"};
  // `[...KV_READ_INCIDENT_V2_MIGRATION_KEY, "api_key_usage_baseline"]`
  inline const rt::KvKey API_KEY_USAGE_V2_MIGRATION_BASELINE_PREFIX{"uos_ai", "migrations", "kv_read_incident_v2", "api_key_usage_baseline"};
  inline const rt::KvKey LEGACY_REQUEST_LOG_PREFIX{"ubq_ai", "api_keys", "request_log"};
  inline const rt::KvKey PAID_FALLBACK_LEDGER_PREFIX{"uos_ai", "paid_fallback", "ledger"};
  inline const rt::KvKey PAID_FALLBACK_V3_PREFIX{"uos_ai", "paid_fallback", "v3"};
  // `[...PAID_FALLBACK_V3_PREFIX, "window"]` and the other V3 sub-prefixes.
  inline const rt::KvKey PAID_FALLBACK_WINDOW_V3_PREFIX{"uos_ai", "paid_fallback", "v3", "window"};
  inline const rt::KvKey PAID_FALLBACK_REQUEST_V3_PREFIX{"uos_ai", "paid_fallback", "v3", "request"};
  inline const rt::KvKey PAID_FALLBACK_PENDING_V3_PREFIX{"uos_ai", "paid_fallback", "v3", "pending"};

  // `(value: unknown): value is number` and friends.
  bool isSafeUsageCount(const rt::KvValue& value);
  bool isApiKeyId(const rt::KvValue& value);
  bool isApiKeyHash(const rt::KvValue& value);
  bool isPositiveSafeInteger(const rt::KvValue& value);
  bool isFiniteNonNegativeNumber(const rt::KvValue& value);

  // `(value: unknown): value is ApiKeyRequestLogRecord`
  bool isPendingPaidFallbackLedgerRecord(const rt::KvValue& value);

  // `JSON.stringify([keyId, requestId])`
  std::string paidFallbackLedgerReference(const std::string& keyId, const std::string& requestId);

  // `entry: Pick<KvEntry<unknown>, "key" | "value">` (a list entry converts implicitly);
  // std::nullopt is null. Throws JSON.stringify's TypeError for a bigint key part, as the TS does.
  std::optional<std::string> pendingPaidFallbackLedgerReferenceFromEntry(const rt::KvEntryMaybe<>& entry, const rt::KvKey& prefix);

  // `expected: Pick<ApiKeyRequestLogRecord, "key_id" | "id" | "created_at_ms">`: the stored row.
  bool paidFallbackLedgerEntryMatchesIdentity(const rt::KvEntryMaybe<>& entry, const rt::KvValue& expected);

  // `(value: unknown): value is ApiKeyHashRecord` / `value is ApiKeyRecord`
  bool hasStrictApiKeyHashCorePolicy(const rt::KvValue& value);
  bool hasStrictApiKeyCorePolicy(const rt::KvValue& value);

  // `record: ApiKeyRecord, hashRecord: ApiKeyHashRecord`: the stored rows, compared with `===`.
  bool apiKeyHashPolicyMatches(const rt::KvValue& record, const rt::KvValue& hashRecord);

  // `(value: unknown): value is PaidFallbackRequestV3`
  bool isPaidFallbackRequestV3(const rt::KvValue& value);

  // `JSON.stringify([keyId, windowResetAtMs])`
  std::string paidFallbackV3WindowReference(const std::string& keyId, double windowResetAtMs);

  // `PaidFallbackV3Inventory` (module-private in the TS; the result type of inspectPaidFallbackV3).
  struct PaidFallbackV3Inventory {
    double windows;
    double requests;
    double pending;
    double reconciliationLeases;
    double deletionGuards;
    std::vector<std::string> errors;
    friend bool operator==(const PaidFallbackV3Inventory&, const PaidFallbackV3Inventory&) = default;
  };

  // `knownKeyIds` / `unlimitedKeyIds: ReadonlySet<string>`; `nowMs = Date.now()` (read when called).
  rt::Task<PaidFallbackV3Inventory> inspectPaidFallbackV3(std::shared_ptr<rt::Kv> kv, rt::OrderedSet<std::string> knownKeyIds, rt::OrderedSet<std::string> unlimitedKeyIds);
  rt::Task<PaidFallbackV3Inventory> inspectPaidFallbackV3(std::shared_ptr<rt::Kv> kv, rt::OrderedSet<std::string> knownKeyIds, rt::OrderedSet<std::string> unlimitedKeyIds,
                                                          double nowMs);
}
