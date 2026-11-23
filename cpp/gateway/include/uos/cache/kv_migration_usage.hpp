#pragma once
// Port of src/cache/kv-migration-usage.ts: bounded counter handoff and kernel quota v2 migration,
// split out of src/kv_migration.ts.
//
// `kv: Kv` is a non-null std::shared_ptr<rt::Kv>. Stored rows are rt::KvValue (the TS `unknown`,
// and the `ApiKeyRecord` / `ApiKeyHashRecord` it casts unchecked storage to after its guards);
// they reach the native-kernel normalizers as quota_v2::kernelQuotaValue(value) and the other
// JSON guards as rt::storedJsonView(value). Where the TS spreads a stored row into a write
// (`{ ...existingWindow, ... }`, `{ ...baseline, ... }`) the stored object is merged member by
// member, so undeclared members survive and keep their order. Coroutines that take an aggregate
// are reached through non-coroutine overloads taking it by const reference.
#include "uos/api_key_policy.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::cache::kv_migration_usage {
  // `StrictApiKeyPair = Readonly<{ record: ApiKeyRecord; hashRecord: ApiKeyHashRecord }>`: the two
  // stored rows, validated by hasStrictApiKeyCorePolicy / hasStrictApiKeyHashCorePolicy and paired
  // by apiKeyHashPolicyMatches (src/cache/kv-migration-paid-fallback.ts).
  struct StrictApiKeyPair {
    rt::KvValue record;
    rt::KvValue hashRecord;
    friend bool operator==(const StrictApiKeyPair&, const StrictApiKeyPair&) = default;
  };

  struct ApiKeyUsageV2MigrationBaseline {
    double version = 1;
    std::string key_id;
    std::string policy_version;
    double window_start_ms;
    double last_legacy_usage_requests;
    double seeded_at_ms;
    std::optional<double> reconciled_at_ms;
    double reconciliation_runs;
    friend bool operator==(const ApiKeyUsageV2MigrationBaseline&, const ApiKeyUsageV2MigrationBaseline&) = default;
  };

  // `policy: Pick<ApiKeyPolicy, "key_id" | "policy_version" | "window_start_ms">`
  rt::KvKey apiKeyUsageV2MigrationBaselineKey(const api_key_policy::ApiKeyPolicy& policy);

  // `value: unknown`; std::nullopt is null. The TS returns the validated row itself; its members
  // are exactly these (an undeclared member of the stored row is not copied here).
  std::optional<ApiKeyUsageV2MigrationBaseline> normalizeApiKeyUsageV2MigrationBaseline(const rt::KvValue& value);

  // `BoundedCounterHandoffResult` (module-private in the TS; migrateBoundedCounterHandoff's result).
  struct BoundedCounterHandoffResult {
    bool baseline_created;
    bool baseline_reconciled;
    double legacy_usage_delta_applied;
    friend bool operator==(const BoundedCounterHandoffResult&, const BoundedCounterHandoffResult&) = default;
  };

  // `record: ApiKeyRecord` (the stored row). Member arithmetic and comparisons follow JavaScript's
  // number coercion; currentLegacyUsage returns `record.usage_requests` when it is a number and
  // NaN for any other value (every caller then fails isSafeUsageCount, as on the raw value).
  double currentLegacyUsage(const rt::KvValue& record, double nowMs);

  // Revoked keys no longer advance their legacy window. Keep migration and
  // validation pinned to the last stored window instead of rolling them forward.
  double migrationPolicyNow(const rt::KvValue& record, double nowMs);

  rt::Task<BoundedCounterHandoffResult> migrateBoundedCounterHandoff(std::shared_ptr<rt::Kv> kv, const api_key_policy::ApiKeyPolicy& policy, double legacyUsageRequests,
                                                                     double nowMs, bool handoffAlreadyInitialized);

  rt::Task<double> countReconciledBoundedCounterBaselines(std::shared_ptr<rt::Kv> kv);

  // `Promise<{ repo: number; org: number }>`
  struct KernelQuotaV2MigrationResult {
    double repo;
    double org;
    friend bool operator==(const KernelQuotaV2MigrationResult&, const KernelQuotaV2MigrationResult&) = default;
  };

  rt::Task<KernelQuotaV2MigrationResult> migrateKernelQuotaV2(std::shared_ptr<rt::Kv> kv, double nowMs);

  // `ApiKeyUsageV3Inventory` (module-private in the TS; inspectApiKeyUsageV3's result).
  struct ApiKeyUsageV3Inventory {
    double windows;
    double requests;
    std::vector<std::string> errors;
    friend bool operator==(const ApiKeyUsageV3Inventory&, const ApiKeyUsageV3Inventory&) = default;
  };

  // `knownKeyIds: ReadonlySet<string>`
  rt::Task<ApiKeyUsageV3Inventory> inspectApiKeyUsageV3(std::shared_ptr<rt::Kv> kv, rt::OrderedSet<std::string> knownKeyIds);

  // `KernelQuotaV2Inventory` (module-private in the TS; inspectKernelQuotaV2's result).
  struct KernelQuotaV2Inventory {
    double repoPolicies;
    double orgPolicies;
    double repoWindows;
    double orgWindows;
    double repoReservations;
    double orgReservations;
    std::vector<std::string> errors;
    friend bool operator==(const KernelQuotaV2Inventory&, const KernelQuotaV2Inventory&) = default;
  };

  rt::Task<KernelQuotaV2Inventory> inspectKernelQuotaV2(std::shared_ptr<rt::Kv> kv);

  // `Promise<{ pairs: StrictApiKeyPair[]; errors: string[] }>`
  struct StrictApiKeyPairsInventory {
    std::vector<StrictApiKeyPair> pairs;
    std::vector<std::string> errors;
    friend bool operator==(const StrictApiKeyPairsInventory&, const StrictApiKeyPairsInventory&) = default;
  };

  rt::Task<StrictApiKeyPairsInventory> inspectStrictApiKeyPairs(std::shared_ptr<rt::Kv> kv);

  // Boost.JSON conversions of the snake_case records: value_from writes the members in declaration
  // order; value_to is strict (rt::TypeError) and drops undeclared members.
  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyUsageV2MigrationBaseline& value);
  ApiKeyUsageV2MigrationBaseline tag_invoke(const json::value_to_tag<ApiKeyUsageV2MigrationBaseline>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const BoundedCounterHandoffResult& value);
  BoundedCounterHandoffResult tag_invoke(const json::value_to_tag<BoundedCounterHandoffResult>&, const json::value& value);
}
