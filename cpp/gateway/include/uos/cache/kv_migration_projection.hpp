#pragma once
// Port of src/cache/kv-migration-projection.ts: legacy paid fallback projection and repair, split
// out of src/kv_migration.ts.
//
// `kv: Kv` is a non-null std::shared_ptr<rt::Kv>. Stored rows are rt::KvValue (conventions:
// uos/cache/kv_migration_paid_fallback.hpp).
#include "uos/cache/kv_migration_usage.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <string>
#include <vector>

namespace uos::cache::kv_migration_projection {
  // `Readonly<{ value: ApiKeyRequestLogRecord; ledgerKey: KvKey; existing: KvEntryMaybe<ApiKeyRequestLogRecord> }>`:
  // `value` is the stored legacy request-log row (kv-migration copies it as it is) and `existing`
  // the raw read of its dedicated-ledger destination.
  struct LegacyPaidFallbackLedgerCandidate {
    rt::KvValue value;
    rt::KvKey ledgerKey;
    rt::KvEntryMaybe<> existing;
    friend bool operator==(const LegacyPaidFallbackLedgerCandidate&, const LegacyPaidFallbackLedgerCandidate&) = default;
  };

  // `LegacyPaidFallbackProjectionResult` (module-private in the TS; projectLegacyPaidFallbackV3's result).
  struct LegacyPaidFallbackProjectionResult {
    double projected;
    double pending;
    friend bool operator==(const LegacyPaidFallbackProjectionResult&, const LegacyPaidFallbackProjectionResult&) = default;
  };

  // `pairs: readonly StrictApiKeyPair[]`
  rt::Task<LegacyPaidFallbackProjectionResult> projectLegacyPaidFallbackV3(std::shared_ptr<rt::Kv> kv, std::vector<kv_migration_usage::StrictApiKeyPair> pairs,
                                                                           double nowMs);

  // `Promise<{ candidates: LegacyPaidFallbackLedgerCandidate[]; errors: string[] }>`
  struct PendingPaidFallbackLedgerInventory {
    std::vector<LegacyPaidFallbackLedgerCandidate> candidates;
    std::vector<std::string> errors;
    friend bool operator==(const PendingPaidFallbackLedgerInventory&, const PendingPaidFallbackLedgerInventory&) = default;
  };

  rt::Task<PendingPaidFallbackLedgerInventory> inspectPendingPaidFallbackLedgers(std::shared_ptr<rt::Kv> kv, std::vector<kv_migration_usage::StrictApiKeyPair> pairs);
}
