#pragma once
// Port of src/cache/kv-migration-base.ts: KV migration types, prefix tables and validators, split
// out of src/kv_migration.ts.
//
// `kv: Kv` is a non-null std::shared_ptr<rt::Kv>; `kv: Kv | null` is a std::shared_ptr<rt::Kv>
// whose null pointer is null. Values parsed from profile NDJSON and read back from KV are
// rt::KvValue (the TS `unknown`). Coroutines that take an aggregate are reached through
// non-coroutine overloads taking it by const reference (GCC 12.2: an aggregate passed by value
// to a coroutine inside a co_await expression is destroyed twice).
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <string>
#include <vector>

namespace uos::cache::kv_migration_base {
  using KvMigrationProfile = std::string;         // "local" | "prod"
  using KvMigrationDecisionAction = std::string;  // "import" | "skip" | "optional"

  struct KvMigrationDecision {
    KvMigrationDecisionAction action;
    std::string group;
    std::string reason;
    friend bool operator==(const KvMigrationDecision&, const KvMigrationDecision&) = default;
  };

  struct KvMigrationCounters {
    double total;
    double imported;
    double skipped;
    double optional;
    double errors;
    friend bool operator==(const KvMigrationCounters&, const KvMigrationCounters&) = default;
  };

  struct KvMigrationClassifyOptions {
    KvMigrationProfile profile;
    bool includeCache;
    bool includeLegacy;
  };

  // `KvMigrationClassifyOptions & Readonly<{ overwrite; dryRun }>` (an intersection: build it
  // positionally, `{{profile, includeCache, includeLegacy}, overwrite, dryRun}`, or by member).
  struct KvMigrationImportOptions : KvMigrationClassifyOptions {
    bool overwrite;
    bool dryRun;
  };

  // `KvMigrationCounters & { groups: Record<string, number> }`. The groups are the prefix-table
  // group names (never integer-like), so the JS property order is the insertion order kept here.
  struct KvMigrationImportResult : KvMigrationCounters {
    rt::OrderedMap<std::string, double> groups;
  };

  struct KvMigrationValidationResult {
    struct Counts {
      double api_key_ids;
      double api_key_hashes;
      double api_key_bounded_counters_v2;
      double api_key_bounded_counter_baselines_v2;
      double api_key_bounded_counter_reconciled_baselines_v2;
      double api_key_usage_v3_windows;
      double api_key_usage_v3_requests;
      double paid_fallback_ledger;
      double paid_fallback_v3_windows;
      double paid_fallback_v3_requests;
      double paid_fallback_v3_pending;
      double paid_fallback_v3_reconciliation_leases;
      double paid_fallback_v3_deletion_guards;
      double kernel_repo_limits;
      double kernel_org_limits;
      double kernel_v2_repo_policies;
      double kernel_v2_org_policies;
      double kernel_v2_repo_windows;
      double kernel_v2_org_windows;
      double kernel_v2_repo_reservations;
      double kernel_v2_org_reservations;
      double passkey_users;
      double passkey_credentials;
      double agent_messages;
      double embeddings_v2_at_most_10000;
      double legacy_model_key_configs;
      double legacy_model_key_health;
      double codex_reset_usage;
      friend bool operator==(const Counts&, const Counts&) = default;
    };
    struct SettingsPresent {
      bool codex_auth;
      bool codex_models;
      bool default_model;
      bool default_reasoning_effort;
      bool default_kernel_policy_limit_requests;
      bool default_kernel_policy_window_ms;
      bool voyage_api_key;
      bool kernel_pubkeys;
      bool runtime_config_v2;
      friend bool operator==(const SettingsPresent&, const SettingsPresent&) = default;
    };
    Counts counts;
    SettingsPresent settings_present;
    std::vector<std::string> errors;
    friend bool operator==(const KvMigrationValidationResult&, const KvMigrationValidationResult&) = default;
  };

  bool defaultIncludeLegacyForProfile(const KvMigrationProfile& profile);

  KvMigrationDecision classifyKvMigrationKey(const rt::KvKey& key, const KvMigrationClassifyOptions& options);

  // `valueJson: unknown` (a profile line's raw `value` member): nullptr is undefined.
  std::string safeKvMigrationValueType(const json::value& valueJson);
  std::string safeKvMigrationValueType(const json::value* valueJson);

  // `{ key: KvKey; value: unknown; raw: KvEntryJson }`. `raw` is `JSON.parse(line) as KvEntryJson`,
  // an unchecked cast, so it stays the parsed JSON (the kv-migrate CLI reads `raw.value` from it).
  struct KvMigrationEntryLine {
    rt::KvKey key;
    rt::KvValue value;
    json::value raw;
  };

  // Throws what the TS throws: JSON.parse's SyntaxError, JavaScriptCore's TypeError for a line
  // whose `key` is not an array (or a null line), and toKey / toValue's errors
  // (src/kv-conversion/profile-json.ts).
  KvMigrationEntryLine parseKvMigrationEntryLine(const std::string& line);

  // `lines: AsyncIterable<string> | Iterable<string>`: an array of lines, or an async generator of
  // them. `for await` over the array takes a microtask turn per line, as it does over a JS array.
  rt::Task<KvMigrationImportResult> importKvMigrationLines(std::shared_ptr<rt::Kv> kv, std::vector<std::string> lines, const KvMigrationImportOptions& options);
  rt::Task<KvMigrationImportResult> importKvMigrationLines(std::shared_ptr<rt::Kv> kv, rt::AsyncGenerator<std::string> lines, const KvMigrationImportOptions& options);

  // `limit = Number.POSITIVE_INFINITY`
  rt::Task<double> listKvMigrationCount(std::shared_ptr<rt::Kv> kv, rt::KvKey prefix);
  rt::Task<double> listKvMigrationCount(std::shared_ptr<rt::Kv> kv, rt::KvKey prefix, double limit);

  // Boost.JSON conversions of the snake_case result records: value_from writes the members in
  // declaration order; value_to is strict (rt::TypeError) and drops undeclared members.
  void tag_invoke(const json::value_from_tag&, json::value& out, const KvMigrationValidationResult::Counts& value);
  KvMigrationValidationResult::Counts tag_invoke(const json::value_to_tag<KvMigrationValidationResult::Counts>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const KvMigrationValidationResult::SettingsPresent& value);
  KvMigrationValidationResult::SettingsPresent tag_invoke(const json::value_to_tag<KvMigrationValidationResult::SettingsPresent>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const KvMigrationValidationResult& value);
  KvMigrationValidationResult tag_invoke(const json::value_to_tag<KvMigrationValidationResult>&, const json::value& value);
  // `{ ...counters, groups }` as the CLI prints it (no value_to: the groups are a dictionary).
  void tag_invoke(const json::value_from_tag&, json::value& out, const KvMigrationImportResult& value);
}
