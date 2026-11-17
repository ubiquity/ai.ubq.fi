#pragma once
// Port of src/cache/kv-migration-validation.ts: KV migration target validation, split out of
// src/kv_migration.ts.
//
// `kv: Kv` is a non-null std::shared_ptr<rt::Kv>. `errors: string[]` is the caller's array, which
// the audits append to: the caller keeps it alive until the returned task completes (the TS
// callers await at once).
#include "uos/cache/kv_migration_base.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <string>
#include <vector>

namespace uos::cache::kv_migration_validation {
  rt::Task<void> auditCodexResetRedemptions(std::shared_ptr<rt::Kv> kv, std::vector<std::string>& errors);

  rt::Task<void> auditManualResetIntents(std::shared_ptr<rt::Kv> kv, std::vector<std::string>& errors);

  rt::Task<kv_migration_base::KvMigrationValidationResult> validateKvMigrationTarget(std::shared_ptr<rt::Kv> kv);
}
