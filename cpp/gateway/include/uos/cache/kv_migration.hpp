#pragma once
// Port of src/cache/kv-migration.ts. Stored API-key pairs remain raw KvValue rows so object
// spreads preserve opaque fields, typed values and property order.
#include "uos/cache/kv_migration_base.hpp"
#include "uos/cache/kv_migration_paid_fallback.hpp"
#include "uos/cache/kv_migration_validation.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <string>

namespace uos::cache::kv_migration {
  using kv_migration_base::classifyKvMigrationKey;
  using kv_migration_base::defaultIncludeLegacyForProfile;
  using kv_migration_base::importKvMigrationLines;
  using kv_migration_base::parseKvMigrationEntryLine;
  using kv_migration_base::safeKvMigrationValueType;
  using kv_migration_base::KvMigrationDecisionAction;
  using kv_migration_base::KvMigrationProfile;
  using kv_migration_paid_fallback::KV_READ_INCIDENT_V2_MIGRATION_KEY;
  using kv_migration_validation::validateKvMigrationTarget;

  struct KvReadIncidentV2MigrationResult {
    double api_keys;
    double bounded_counters;
    std::string handoff_phase;
    double bounded_baselines_created;
    double bounded_baselines_reconciled;
    double legacy_usage_delta_applied;
    double kernel_repo_records;
    double kernel_org_records;
    double paid_fallback_records;
    bool runtime_config_written;
    friend bool operator==(const KvReadIncidentV2MigrationResult&, const KvReadIncidentV2MigrationResult&) = default;
  };

  rt::Task<KvReadIncidentV2MigrationResult> migrateKvReadIncidentV2(std::shared_ptr<rt::Kv> kv);

  void tag_invoke(const json::value_from_tag&, json::value& out, const KvReadIncidentV2MigrationResult& value);
  KvReadIncidentV2MigrationResult tag_invoke(const json::value_to_tag<KvReadIncidentV2MigrationResult>&, const json::value& value);
}
