#pragma once
// Port of src/codex/reset-settings.ts.
// Per-account banked-reset usage consent and the one-way migration of the legacy global opt-out.
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"

#include <memory>
#include <string>
#include <vector>

namespace uos::codex::reset_settings {
  inline const rt::KvKey CODEX_RESET_USAGE_PREFIX{"uos_ai", "codex_reset_usage"};

  // The pre-migration global opt-out switch. It is read only until the migration
  // completion marker exists and is never deleted, so a rollback to a revision
  // that still reads it sees the operator's original decision.
  inline const rt::KvKey CODEX_BANKED_RESET_USAGE_LEGACY_KEY{"uos_ai", "codex_banked_reset_usage", "v1"};

  // The one-way completion marker. It lives under the existing migrations prefix
  // because every row under `codex_reset_usage` must be a per-account record: the
  // KV migration validator rejects any other key shape under that prefix.
  inline const rt::KvKey CODEX_RESET_USAGE_MIGRATION_KEY{"uos_ai", "migrations", "codex_reset_usage_legacy_global_v1"};

  // Stable account identity, independent of API keys and subscription ordering.
  rt::KvKey codexResetUsageKey(const std::string& accountIdHash);

  /**
   * Completes the one-way migration in one bounded atomic transaction whose
   * precondition is the exact strong auth-pool snapshot it enumerated: the pool
   * entry, the legacy switch and every per-subscription row are read with strong
   * consistency, then a single commit re-checks those versionstamps while seeding
   * the absent rows as `{ enabled: false }` and writing the completion marker.
   * `parseCodexAuthPool` caps the pool, so the commit carries a fixed, small
   * number of checks and mutations.
   *
   * An account configured after the enumeration read therefore invalidates the
   * commit instead of escaping the persisted global opt-out: nothing is written,
   * the marker stays absent, and the next guarded read retries this completion
   * from a fresh snapshot. An explicit per-subscription decision is never
   * overwritten, a stale legacy decision is never recorded, an absent, malformed
   * or empty pool stays `pending` so the opt-out can never retire early, and the
   * legacy key is left in place for rollback.
   */
  // `Promise<"done" | "pending">`
  [[nodiscard]] rt::Task<std::string> migrateLegacyCodexResetOptOut(std::shared_ptr<rt::Kv> kv);

  // The value readCodexResetUsage returns (inferred in the TS): `{ allowed: boolean; entries: KvEntryMaybe<unknown>[] }`.
  struct ReadCodexResetUsageResult {
    bool allowed;
    std::vector<rt::KvEntryMaybe<>> entries;
  };

  [[nodiscard]] rt::Task<ReadCodexResetUsageResult> readCodexResetUsage(std::shared_ptr<rt::Kv> kv, std::string accountIdHash);
}
