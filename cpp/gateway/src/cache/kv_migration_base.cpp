#include "uos/cache/kv_migration_base.hpp"

#include "uos/analytics.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/codex/reset_settings.hpp"
#include "uos/embeddings/ledger.hpp"
#include "uos/kernel/quota_v2.hpp"
#include "uos/kv_conversion/profile_json.hpp"
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uos::cache::kv_migration_base {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
    namespace quota_v2 = kernel::quota_v2;
    namespace contracts = authority::native_kernel_contracts;

    // `{ group: string; prefix: KvKey }`
    struct PrefixGroup {
      std::string group;
      rt::KvKey prefix;
    };

    const std::vector<PrefixGroup>& DURABLE_PREFIXES() {
      static const std::vector<PrefixGroup> table{
        {"api_keys_id", rt::KvKey{"ubq_ai", "api_keys", "id"}},
        {"api_keys_hash", rt::KvKey{"ubq_ai", "api_keys", "hash"}},
        {"api_keys_usage", rt::KvKey{"ubq_ai", "api_keys", "usage"}},
        {"api_keys_usage_daily", rt::KvKey{"ubq_ai", "api_keys", "usage_daily"}},
        {"api_keys_request_log", rt::KvKey{"ubq_ai", "api_keys", "request_log"}},
        {"api_key_usage_v2", rt::KvKey{"uos_ai", "api_key_usage", "v2"}},
        {"api_key_usage_v3_windows", rt::KvKey{"uos_ai", "api_key_usage", "v3", "window"}},
        {"api_key_usage_v3_requests", rt::KvKey{"uos_ai", "api_key_usage", "v3", "request"}},
        {"paid_fallback_ledger", rt::KvKey{"uos_ai", "paid_fallback", "ledger"}},
        {"paid_fallback_v3_windows", rt::KvKey{"uos_ai", "paid_fallback", "v3", "window"}},
        {"paid_fallback_v3_requests", rt::KvKey{"uos_ai", "paid_fallback", "v3", "request"}},
        {"paid_fallback_v3_usage_rollups", rt::KvKey{"uos_ai", "paid_fallback", "v3", "usage_rollup"}},
        {"paid_fallback_v3_ledger_stats", rt::KvKey{"uos_ai", "paid_fallback", "v3", "ledger_stats"}},
        {"metered_quota_balance_history", rt::KvKey{"uos_ai", "metered_quota", "v1", "balance_history"}},
        {"provider_capacity_rollups", rt::KvKey{"uos_ai", "provider_capacity", "v1", "rollup"}},
        {"paid_fallback_v3_pending", rt::KvKey{"uos_ai", "paid_fallback", "v3", "pending"}},
        {"paid_fallback_v3_reconciliation_leases", rt::KvKey{"uos_ai", "paid_fallback", "v3", "reconciliation_lease"}},
        {"paid_fallback_v3_deletion_guards", rt::KvKey{"uos_ai", "paid_fallback", "v3", "deletion_guard"}},
        {"runtime_config_v2", rt::KvKey{"uos_ai", "runtime_config", "v2"}},
        {"kernel_usage", rt::KvKey{"ubq_ai", "kernel_auth", "usage"}},
        {"kernel_usage_daily", rt::KvKey{"ubq_ai", "kernel_auth", "usage_daily"}},
        {"kernel_quota_v2_repo_policy", rt::KvKey{"uos_ai", "kernel_quota", "v2", "repo_policy"}},
        {"kernel_quota_v2_org_policy", rt::KvKey{"uos_ai", "kernel_quota", "v2", "org_policy"}},
        {"kernel_quota_v2_repo_window", rt::KvKey{"uos_ai", "kernel_quota", "v2", "repo_window"}},
        {"kernel_quota_v2_org_window", rt::KvKey{"uos_ai", "kernel_quota", "v2", "org_window"}},
        {"kernel_quota_v2_repo_reservation", rt::KvKey{"uos_ai", "kernel_quota", "v2", "repo_reservation"}},
        {"kernel_quota_v2_org_reservation", rt::KvKey{"uos_ai", "kernel_quota", "v2", "org_reservation"}},
        // Kept importable until the two-phase incident migration has replayed old
        // isolate increments into the split V2 window records.
        {"kernel_limits_legacy", rt::KvKey{"ubq_ai", "kernel_auth", "limits"}},
        {"kernel_org_usage", rt::KvKey{"ubq_ai", "kernel_auth", "org_usage"}},
        {"kernel_org_usage_daily", rt::KvKey{"ubq_ai", "kernel_auth", "org_usage_daily"}},
        {"kernel_org_limits_legacy", rt::KvKey{"ubq_ai", "kernel_auth", "org_limits"}},
        {"defaults", rt::KvKey{"default"}},
        {"kernel_pubkeys", rt::KvKey{"uos_ai", "kernel_pubkeys"}},
        {"voyage_api_key", rt::KvKey{"uos_ai", "voyage_api_key"}},
        {"codex_prompts", rt::KvKey{"uos_ai", "codex_instructions"}},
        {"codex_prompts_chunks", rt::KvKey{"uos_ai", "codex_instructions_chunk"}},
        {"codex_reset_usage", codex::reset_settings::CODEX_RESET_USAGE_PREFIX},
        {"codex_manual_reset", rt::KvKey{"uos_ai", "codex_manual_reset", "v1"}},
        {"codex_reset_redemption", rt::KvKey{"uos_ai", "codex_reset_redemption"}},
        {"kernel_policy_queue", rt::KvKey{"uos_ai", "kernel_policy_queue"}},
        {"migrations", rt::KvKey{"uos_ai", "migrations"}},
        {"embeddings_idempotency_responses", rt::KvKey{"embeddings", "idempotency", "v1", "response"}},
        {"embeddings_idempotency", rt::KvKey{"embeddings", "idempotency", "v1"}},
        {"passkey_users", rt::KvKey{"uos_ai", "auth", "users"}},
        {"passkey_handles", rt::KvKey{"uos_ai", "auth", "handles"}},
        {"passkey_credentials", rt::KvKey{"uos_ai", "auth", "credentials"}},
        {"agent_messages", rt::KvKey{"agent_messages"}},
      };
      return table;
    }

    const std::vector<PrefixGroup>& CODEX_BOOTSTRAP_PREFIXES() {
      static const std::vector<PrefixGroup> table{
        {"codex_auth", rt::KvKey{"ubq_ai", "codex_auth"}},
        {"codex_models", rt::KvKey{"ubq_ai", "codex_models"}},
      };
      return table;
    }

    const std::vector<PrefixGroup>& LEGACY_DURABLE_PREFIXES() {
      static const std::vector<PrefixGroup> table{
        {"legacy_model_key_config", rt::KvKey{"key", "config"}},
        {"legacy_model_key_health", rt::KvKey{"key", "health"}},
      };
      return table;
    }

    const std::vector<PrefixGroup>& TRANSIENT_PREFIXES() {
      static const std::vector<PrefixGroup> table{
        {"passkey_challenges", rt::KvKey{"uos_ai", "auth", "challenges"}},
        {"passkey_sessions", rt::KvKey{"uos_ai", "auth", "sessions"}},
        {"embeddings_rate", rt::KvKey{"embeddings", "v1", "rate"}},
        {"embeddings_jobs", rt::KvKey{"embeddings", "jobs"}},
        {"paid_fallback_v3_backfill_cursor", rt::KvKey{"uos_ai", "paid_fallback", "v3", "rollup_backfill_cursor"}},
        {"paid_fallback_v3_backfill_window_cursor", rt::KvKey{"uos_ai", "paid_fallback", "v3", "rollup_backfill_window_cursor"}},
      };
      return table;
    }

    const std::vector<PrefixGroup>& EMBEDDINGS_CACHE_PREFIXES() {
      static const std::vector<PrefixGroup> table{
        {"embeddings_cache_index", rt::KvKey{"embeddings", "v2", "cache_index"}},
        {"embeddings_cache_index_by_hash", rt::KvKey{"embeddings", "v2", "cache_index_by_hash"}},
        {"embeddings_cache_values", rt::KvKey{"embeddings", "v2"}},
      };
      return table;
    }

    bool keyStartsWith(const rt::KvKey& key, const rt::KvKey& prefix) {
      if (key.size() < prefix.size()) return false;
      for (std::size_t index = 0; index < prefix.size(); ++index)
        if (!rt::kvKeyPartStrictEquals(key[index], prefix[index])) return false;
      return true;
    }

    // `prefixes.find(...) ?? null`
    const PrefixGroup* findPrefix(const rt::KvKey& key, const std::vector<PrefixGroup>& prefixes) {
      for (const PrefixGroup& entry : prefixes)
        if (keyStartsWith(key, entry.prefix)) return &entry;
      return nullptr;
    }

    // `JSON.stringify(key)`
    std::string keyJson(const rt::KvKey& key) { return *rt::kvJsonStringify(rt::kvValueFromKey(key)); }

    // JavaScriptCore's description of the value in "x is not a function. (In '...', 'x' is <value>)".
    std::string describeValue(const json::value* value) {
      if (value == nullptr) return "undefined";
      if (value->is_null()) return "null";
      if (value->is_bool()) return value->get_bool() ? "true" : "false";
      if (value->is_string()) return "\"" + std::string(value->get_string()) + "\"";
      if (value->is_array()) return "an instance of Array";
      if (value->is_object()) return "an instance of Object";
      return rt::numberToString(*rt::getNumber(*value));
    }

    // `toKey(raw.key)`: toKey runs `json.map(...)` on whatever the line holds.
    rt::KvKey lineKey(const json::value* key) {
      if (key == nullptr) throw rt::TypeError("undefined is not an object (evaluating 'json.map')");
      if (key->is_null()) throw rt::TypeError("null is not an object (evaluating 'json.map')");
      if (const json::array* parts = key->if_array()) return kv_conversion::profile_json::toKey(*parts);
      const json::value* map = key->is_object() ? rt::get(key->get_object(), "map") : nullptr;
      throw rt::TypeError(
        "json.map is not a function. (In 'json.map((part) => {\n  if (![\"bigint\", \"boolean\", \"number\", \"string\", \"Uint8Array\"].includes(part.type))\n"
        "    throw TypeError(\"Unsupported profile key type\");\n  return toValue(part);\n})', 'json.map' is " +
        describeValue(map) + ")");
    }

    // `number | null | "skip"`
    struct MigrationExpireIn {
      enum class Kind { ttl, durable, skip };
      Kind kind;
      double ms = 0;
    };
    constexpr MigrationExpireIn SKIP{MigrationExpireIn::Kind::skip};
    constexpr MigrationExpireIn DURABLE{MigrationExpireIn::Kind::durable};

    /** @returns Remaining retention in milliseconds, or "skip" for invalid or expired anchors. */
    MigrationExpireIn boundedMigrationExpireIn(const rt::KvValue& anchorMs, double retentionMs, double nowMs) {
      if (!anchorMs.isNumber() || !rt::Number::isSafeInteger(anchorMs.asNumber()) || anchorMs.asNumber() < 0) return SKIP;
      const double expiresAtMs = anchorMs.asNumber() + retentionMs;
      if (!rt::Number::isSafeInteger(expiresAtMs) || expiresAtMs <= nowMs) return SKIP;
      return MigrationExpireIn{MigrationExpireIn::Kind::ttl, expiresAtMs - nowMs};
    }

    // `number | null`: std::nullopt is null.
    std::optional<double> apiKeyUsageV3RequestWindowResetAtMs(const rt::KvKey& key) {
      const std::size_t offset = api_key_policy::API_KEY_USAGE_V3_REQUEST_PREFIX.size();
      if (key.size() != offset + 4) return std::nullopt;
      const rt::KvKeyPart& policyVersionPart = key[offset + 1];
      const rt::KvKeyPart& windowStartPart = key[offset + 2];
      if (!policyVersionPart.isString() || !windowStartPart.isNumber() || !rt::Number::isSafeInteger(windowStartPart.number()) || windowStartPart.number() < 0) {
        return std::nullopt;
      }
      const std::string& policyVersion = policyVersionPart.string();
      const double windowStartMs = windowStartPart.number();
      const double windowMs = rt::stringToNumber(rt::slice(policyVersion, 3));
      if (!rt::startsWith(policyVersion, "v3:") || !rt::Number::isSafeInteger(windowMs) || windowMs <= 0 || policyVersion != "v3:" + rt::numberToString(windowMs)) {
        return std::nullopt;
      }
      const double windowResetAtMs = windowStartMs + windowMs;
      return rt::Number::isSafeInteger(windowResetAtMs) ? std::optional<double>(windowResetAtMs) : std::nullopt;
    }

    rt::KvValue optionalNumber(const std::optional<double>& value) { return value ? rt::KvValue(*value) : rt::KvValue(nullptr); }

    // `record?.member` with `record = isRecord(value) ? value : null`.
    const rt::KvValue& recordMember(const rt::KvValue& value, std::string_view member) {
      static const rt::KvValue undefined;
      return rt::kvIsObject(value) ? value[member] : undefined;
    }

    /**
     * Profile NDJSON does not carry the source KV expiry metadata.  These rows are
     * nevertheless bounded state, so restore the same anchored retention instead
     * of turning an old source row into a permanent destination row.
     * @returns A bounded TTL, null for durable rows, or "skip" for stale or invalid rows.
     */
    MigrationExpireIn migrationExpireIn(const rt::KvKey& key, const rt::KvValue& value, double nowMs) {
      if (keyStartsWith(key, api_key_policy::API_KEY_USAGE_V3_WINDOW_PREFIX)) {
        return boundedMigrationExpireIn(recordMember(value, "window_reset_at_ms"), api_key_policy::API_KEY_USAGE_V3_RETENTION_MS, nowMs);
      }
      if (keyStartsWith(key, api_key_policy::API_KEY_USAGE_V3_REQUEST_PREFIX)) {
        // V3 request rows predate the reset timestamp field. Reconstruct the
        // matching aggregate window from the request key, as the source writer
        // anchors both rows to that window rather than to admission time.
        return boundedMigrationExpireIn(optionalNumber(apiKeyUsageV3RequestWindowResetAtMs(key)), api_key_policy::API_KEY_USAGE_V3_RETENTION_MS, nowMs);
      }
      if (keyStartsWith(key, analytics::LEGACY_API_KEY_REQUEST_LOG_PREFIX) || keyStartsWith(key, rt::KvKey{"uos_ai", "paid_fallback", "ledger"})) {
        return boundedMigrationExpireIn(recordMember(value, "created_at_ms"), analytics::API_KEY_REQUEST_LOG_RETENTION_MS, nowMs);
      }
      if (keyStartsWith(key, rt::KvKey{"uos_ai", "paid_fallback", "v3", "window"})) {
        return boundedMigrationExpireIn(recordMember(value, "window_reset_at_ms"), paid_fallback::ledger_state::PAID_FALLBACK_REQUEST_LOG_RETENTION_MS, nowMs);
      }
      if (keyStartsWith(key, rt::KvKey{"uos_ai", "paid_fallback", "v3", "request"})) {
        return boundedMigrationExpireIn(recordMember(value, "created_at_ms"), paid_fallback::ledger_state::PAID_FALLBACK_REQUEST_LOG_RETENTION_MS, nowMs);
      }
      // Pending markers are written without a TTL and remain the durable source
      // of unresolved work until reconciliation removes them.
      if (keyStartsWith(key, rt::KvKey{"uos_ai", "paid_fallback", "v3", "pending"})) return DURABLE;
      if (keyStartsWith(key, rt::KvKey{"uos_ai", "paid_fallback", "v3", "reconciliation_lease"})) {
        return boundedMigrationExpireIn(recordMember(value, "expires_at_ms"), 0, nowMs);
      }
      // Response chunks carry no timestamp in their value, so retain only one
      // bounded response lifetime instead of turning an unanchored row permanent.
      if (keyStartsWith(key, rt::KvKey{"embeddings", "idempotency", "v1", "response"})) {
        return MigrationExpireIn{MigrationExpireIn::Kind::ttl, embeddings::ledger::EMBEDDINGS_IDEMPOTENCY_RESPONSE_TTL_MS};
      }
      if (keyStartsWith(key, rt::KvKey{"embeddings", "idempotency", "v1"})) {
        // `record?.updated_at_ms ?? record?.created_at_ms ?? nowMs`
        const rt::KvValue& updatedAtMs = recordMember(value, "updated_at_ms");
        const rt::KvValue& createdAtMs = recordMember(value, "created_at_ms");
        const rt::KvValue anchor = !updatedAtMs.isNullish() ? updatedAtMs : !createdAtMs.isNullish() ? createdAtMs : rt::KvValue(nowMs);
        return boundedMigrationExpireIn(anchor, embeddings::ledger::EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS, nowMs);
      }
      if (keyStartsWith(key, quota_v2::KERNEL_REPO_WINDOW_V2_PREFIX) || keyStartsWith(key, quota_v2::KERNEL_ORG_WINDOW_V2_PREFIX)) {
        return boundedMigrationExpireIn(recordMember(value, "usage_reset_at_ms"), quota_v2::KERNEL_QUOTA_RESERVATION_RETENTION_MS, nowMs);
      }
      if (keyStartsWith(key, quota_v2::KERNEL_REPO_RESERVATION_V2_PREFIX) || keyStartsWith(key, quota_v2::KERNEL_ORG_RESERVATION_V2_PREFIX)) {
        return boundedMigrationExpireIn(recordMember(value, "window_reset_at_ms"), quota_v2::KERNEL_QUOTA_RESERVATION_RETENTION_MS, nowMs);
      }
      return DURABLE;
    }

    // `Readonly<{ scope: "repo"; owner; repo }> | Readonly<{ scope: "org"; owner; repo: undefined }>`
    struct KernelWindowIdentity {
      contracts::KernelQuotaScope scope;
      std::string owner;
      std::optional<std::string> repo;
    };

    // std::nullopt is null.
    std::optional<KernelWindowIdentity> kernelWindowIdentity(const rt::KvKey& key) {
      const std::size_t repoOffset = quota_v2::KERNEL_REPO_WINDOW_V2_PREFIX.size();
      if (keyStartsWith(key, quota_v2::KERNEL_REPO_WINDOW_V2_PREFIX) && key.size() == repoOffset + 2) {
        const rt::KvKeyPart& owner = key[repoOffset];
        const rt::KvKeyPart& repo = key[repoOffset + 1];
        if (owner.isString() && !owner.string().empty() && repo.isString() && !repo.string().empty()) {
          return KernelWindowIdentity{.scope = "repo", .owner = owner.string(), .repo = repo.string()};
        }
      }
      const std::size_t orgOffset = quota_v2::KERNEL_ORG_WINDOW_V2_PREFIX.size();
      if (keyStartsWith(key, quota_v2::KERNEL_ORG_WINDOW_V2_PREFIX) && key.size() == orgOffset + 1) {
        const rt::KvKeyPart& owner = key[orgOffset];
        if (owner.isString() && !owner.string().empty()) return KernelWindowIdentity{.scope = "org", .owner = owner.string(), .repo = std::nullopt};
      }
      return std::nullopt;
    }

    // `requestId: unknown` (the last key part; nullptr is undefined), `reservation: ... | null`.
    bool isMatchingReservedKernelRequest(const rt::KvKeyPart* requestId, const std::optional<contracts::KernelQuotaReservationRowV2>& reservation, double windowCreatedAtMs) {
      return requestId != nullptr && requestId->isString() && reservation && reservation->request_id == requestId->string() &&
             reservation->window_created_at_ms == windowCreatedAtMs && reservation->state == "reserved";
    }

    /** Adds the reservation aggregate to an imported legacy window when it was absent. */
    rt::Task<void> reconcileImportedKernelWindows(std::shared_ptr<rt::Kv> kv, rt::OrderedMap<std::string, rt::KvKey> windowKeys, double nowMs) {
      const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
      for (const auto& [encoded, key] : windowKeys) {
        const std::optional<KernelWindowIdentity> identity = kernelWindowIdentity(key);
        if (!identity) continue;
        const rt::KvEntryMaybe<> entry = co_await kv->get(key, strong);
        const std::optional<contracts::KernelQuotaWindowV2> window =
          quota_v2::normalizeKernelQuotaWindowV2(quota_v2::kernelQuotaValue(entry.value), identity->scope, identity->owner, identity->repo);
        if (!window || (rt::kvIsObject(entry.value) && entry.value.isObject() && entry.value.asObject().contains("reserved_requests"))) continue;

        const rt::KvKey reservationPrefix = identity->scope == "repo"
                                              ? rt::KvKey{quota_v2::KERNEL_REPO_RESERVATION_V2_PREFIX, identity->owner, *identity->repo, window->created_at_ms}
                                              : rt::KvKey{quota_v2::KERNEL_ORG_RESERVATION_V2_PREFIX, identity->owner, window->created_at_ms};
        double reservedRequests = 0;
        const rt::KvListOptions listOptions{.consistency = rt::KvConsistencyLevel::strong};
        const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = reservationPrefix}, listOptions);
        while (std::optional<rt::KvEntry<>> reservationEntry = co_await iterator->next()) {
          const rt::KvKeyPart* requestId = reservationEntry->key.empty() ? nullptr : &reservationEntry->key.back();
          const std::optional<contracts::KernelQuotaReservationRowV2> reservation =
            quota_v2::normalizeKernelQuotaReservationRowV2(quota_v2::kernelQuotaValue(reservationEntry->value), identity->scope, identity->owner, identity->repo);
          if (isMatchingReservedKernelRequest(requestId, reservation, window->created_at_ms)) {
            reservedRequests += 1;
          }
        }
        contracts::KernelQuotaWindowV2 reconciledWindow = *window;
        reconciledWindow.reserved_requests = reservedRequests;
        const double expireIn = rt::Math::max(1, window->usage_reset_at_ms + quota_v2::KERNEL_QUOTA_RESERVATION_RETENTION_MS - nowMs);
        const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
        atomic->check(entry).set(key, rt::KvValue(json::value_from(reconciledWindow)), rt::KvSetOptions{.expireIn = expireIn});
        const rt::KvCommitResult committed = co_await atomic->commit();
        if (!committed.ok) throw rt::JsError("kernel quota window changed during migration: " + keyJson(key));
      }
    }

    // What one import loop shares across its lines.
    struct ImportState {
      KvMigrationImportOptions options;
      KvMigrationCounters counters{.total = 0, .imported = 0, .skipped = 0, .optional = 0, .errors = 0};
      rt::OrderedMap<std::string, double> byGroup{};
      double migrationNowMs = 0;
      rt::OrderedMap<std::string, rt::KvKey> importedKernelWindows{};
    };

    /** Imports one parsed migration entry, or reports that it must be skipped. */
    // `Promise<"imported" | "skipped">`: true is "imported".
    rt::Task<bool> importKvMigrationEntry(std::shared_ptr<rt::Kv> kv, std::shared_ptr<const KvMigrationEntryLine> entry, std::shared_ptr<ImportState> state, double nowMs) {
      const MigrationExpireIn migrationTtl = migrationExpireIn(entry->key, entry->value, nowMs);
      if (migrationTtl.kind == MigrationExpireIn::Kind::skip) co_return false;
      const rt::KvSetOptions setOptions =
        migrationTtl.kind == MigrationExpireIn::Kind::durable ? rt::KvSetOptions{} : rt::KvSetOptions{.expireIn = migrationTtl.ms};
      if (kv && !state->options.overwrite) {
        const rt::KvEntryMaybe<> existing = co_await kv->get(entry->key);
        if (!existing.value.isNull()) co_return false;
        // Re-check the missing destination in the write transaction. A
        // read-then-set pair can otherwise import the same row twice when two
        // migration workers race with overwrite disabled.
        if (!state->options.dryRun) {
          const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
          atomic->check(existing).set(entry->key, entry->value, setOptions);
          const rt::KvCommitResult committed = co_await atomic->commit();
          if (!committed.ok) co_return false;
          co_return true;
        }
      }
      if (!state->options.dryRun && kv) {
        co_await kv->set(entry->key, entry->value, setOptions);
      }
      co_return true;
    }

    // The body of `for await (const line of lines) { ... }`.
    rt::Task<void> importLine(std::shared_ptr<rt::Kv> kv, std::string line, std::shared_ptr<ImportState> state) {
      if (rt::trim(line).empty()) co_return;
      state->counters.total += 1;
      try {
        const auto entry = std::make_shared<const KvMigrationEntryLine>(parseKvMigrationEntryLine(line));
        const KvMigrationDecision decision = classifyKvMigrationKey(entry->key, state->options);
        const double* groupCount = state->byGroup.get(decision.group);
        state->byGroup.set(decision.group, (groupCount != nullptr ? *groupCount : 0) + 1);
        if (decision.action == "skip") {
          state->counters.skipped += 1;
          co_return;
        }
        if (decision.action == "optional") {
          state->counters.optional += 1;
          co_return;
        }
        const std::optional<KernelWindowIdentity> kernelWindow = kernelWindowIdentity(entry->key);
        const bool imported = co_await importKvMigrationEntry(kv, entry, state, state->migrationNowMs);
        if (imported) state->counters.imported += 1;
        else state->counters.skipped += 1;
        if (imported && kernelWindow) {
          state->importedKernelWindows.set(keyJson(entry->key), entry->key);
        }
      } catch (...) {
        state->counters.errors += 1;
      }
    }

    rt::Task<KvMigrationImportResult> finishImport(std::shared_ptr<rt::Kv> kv, std::shared_ptr<ImportState> state) {
      if (kv && !state->options.dryRun && state->importedKernelWindows.size() > 0) {
        bool failed = false;
        try {
          co_await reconcileImportedKernelWindows(kv, state->importedKernelWindows, state->migrationNowMs);
        } catch (...) {
          failed = true;
        }
        if (failed) state->counters.errors += 1;
      }

      std::vector<std::pair<std::string, double>> groups = rt::arrayFrom(state->byGroup);
      rt::sort(groups, [](const std::pair<std::string, double>& a, const std::pair<std::string, double>& b) { return b.second - a.second; });
      KvMigrationImportResult result;
      static_cast<KvMigrationCounters&>(result) = state->counters;
      for (auto& [group, count] : groups) result.groups.set(group, count);
      co_return result;
    }

    rt::Task<KvMigrationImportResult> importVectorLines(std::shared_ptr<rt::Kv> kv, std::vector<std::string> lines, std::shared_ptr<ImportState> state) {
      for (std::string& line : lines) {
        // `for await` over an array awaits every element.
        co_await rt::microtask();
        co_await importLine(kv, std::move(line), state);
      }
      KvMigrationImportResult result = co_await finishImport(kv, state);
      co_return result;
    }

    rt::Task<KvMigrationImportResult> importGeneratorLines(std::shared_ptr<rt::Kv> kv, rt::AsyncGenerator<std::string> lines, std::shared_ptr<ImportState> state) {
      for (;;) {
        rt::IteratorResult<std::string> step = co_await lines.next();
        if (step.done) break;
        co_await importLine(kv, std::move(*step.value), state);
      }
      KvMigrationImportResult result = co_await finishImport(kv, state);
      co_return result;
    }

    std::shared_ptr<ImportState> importState(const KvMigrationImportOptions& options) {
      auto state = std::make_shared<ImportState>(ImportState{.options = options});
      state->migrationNowMs = rt::dateNow();
      return state;
    }
  }

  bool defaultIncludeLegacyForProfile(const KvMigrationProfile& profile) { return profile == "local"; }

  KvMigrationDecision classifyKvMigrationKey(const rt::KvKey& key, const KvMigrationClassifyOptions& options) {
    if (const PrefixGroup* transient = findPrefix(key, TRANSIENT_PREFIXES())) return {.action = "skip", .group = transient->group, .reason = "transient_runtime_state"};

    if (const PrefixGroup* codex = findPrefix(key, CODEX_BOOTSTRAP_PREFIXES())) {
      if (options.profile == "local") {
        return {.action = "import", .group = codex->group, .reason = "local_replay"};
      }
      return {.action = "skip", .group = codex->group, .reason = "refreshed_by_deploy_bootstrap"};
    }

    if (const PrefixGroup* durable = findPrefix(key, DURABLE_PREFIXES())) return {.action = "import", .group = durable->group, .reason = "durable"};

    if (const PrefixGroup* legacyDurable = findPrefix(key, LEGACY_DURABLE_PREFIXES())) {
      if (options.includeLegacy) {
        return {.action = "import", .group = legacyDurable->group, .reason = "legacy_durable"};
      }
      return {.action = "skip", .group = legacyDurable->group, .reason = "legacy_skipped"};
    }

    if (const PrefixGroup* cache = findPrefix(key, EMBEDDINGS_CACHE_PREFIXES())) {
      if (options.includeCache) return {.action = "import", .group = cache->group, .reason = "cache_requested"};
      return {.action = "optional", .group = cache->group, .reason = "cache_skipped_by_default"};
    }

    return {.action = "skip", .group = "unknown", .reason = "unknown_prefix"};
  }

  std::string safeKvMigrationValueType(const json::value& valueJson) { return safeKvMigrationValueType(&valueJson); }

  std::string safeKvMigrationValueType(const json::value* valueJson) {
    if (!rt::truthy(valueJson) || rt::typeOf(valueJson) != "object") return std::string(rt::typeOf(valueJson));
    const json::value* type = rt::get(*valueJson, "type");
    return type != nullptr && type->is_string() ? std::string(type->get_string()) : std::string("object");
  }

  KvMigrationEntryLine parseKvMigrationEntryLine(const std::string& line) {
    json::value raw = rt::jsonParse(line);
    if (raw.is_null()) throw rt::TypeError("null is not an object (evaluating 'raw.key')");
    rt::KvKey key = lineKey(rt::get(raw, "key"));
    rt::KvValue value = kv_conversion::profile_json::toValue(rt::get(raw, "value"));
    return KvMigrationEntryLine{.key = std::move(key), .value = std::move(value), .raw = std::move(raw)};
  }

  rt::Task<KvMigrationImportResult> importKvMigrationLines(std::shared_ptr<rt::Kv> kv, std::vector<std::string> lines, const KvMigrationImportOptions& options) {
    return importVectorLines(std::move(kv), std::move(lines), importState(options));
  }

  rt::Task<KvMigrationImportResult> importKvMigrationLines(std::shared_ptr<rt::Kv> kv, rt::AsyncGenerator<std::string> lines, const KvMigrationImportOptions& options) {
    return importGeneratorLines(std::move(kv), std::move(lines), importState(options));
  }

  rt::Task<double> listKvMigrationCount(std::shared_ptr<rt::Kv> kv, rt::KvKey prefix) { return listKvMigrationCount(std::move(kv), std::move(prefix), rt::Number::POSITIVE_INFINITY); }

  rt::Task<double> listKvMigrationCount(std::shared_ptr<rt::Kv> kv, rt::KvKey prefix, double limit) {
    double count = 0;
    const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = std::move(prefix)});
    for (;;) {
      const std::optional<rt::KvEntry<>> entry = co_await iterator->next();
      if (!entry) break;
      count += 1;
      if (count >= limit) break;
    }
    co_return count;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KvMigrationValidationResult::Counts& value) {
    out = Writer()
            .required("api_key_ids", value.api_key_ids)
            .required("api_key_hashes", value.api_key_hashes)
            .required("api_key_bounded_counters_v2", value.api_key_bounded_counters_v2)
            .required("api_key_bounded_counter_baselines_v2", value.api_key_bounded_counter_baselines_v2)
            .required("api_key_bounded_counter_reconciled_baselines_v2", value.api_key_bounded_counter_reconciled_baselines_v2)
            .required("api_key_usage_v3_windows", value.api_key_usage_v3_windows)
            .required("api_key_usage_v3_requests", value.api_key_usage_v3_requests)
            .required("paid_fallback_ledger", value.paid_fallback_ledger)
            .required("paid_fallback_v3_windows", value.paid_fallback_v3_windows)
            .required("paid_fallback_v3_requests", value.paid_fallback_v3_requests)
            .required("paid_fallback_v3_pending", value.paid_fallback_v3_pending)
            .required("paid_fallback_v3_reconciliation_leases", value.paid_fallback_v3_reconciliation_leases)
            .required("paid_fallback_v3_deletion_guards", value.paid_fallback_v3_deletion_guards)
            .required("kernel_repo_limits", value.kernel_repo_limits)
            .required("kernel_org_limits", value.kernel_org_limits)
            .required("kernel_v2_repo_policies", value.kernel_v2_repo_policies)
            .required("kernel_v2_org_policies", value.kernel_v2_org_policies)
            .required("kernel_v2_repo_windows", value.kernel_v2_repo_windows)
            .required("kernel_v2_org_windows", value.kernel_v2_org_windows)
            .required("kernel_v2_repo_reservations", value.kernel_v2_repo_reservations)
            .required("kernel_v2_org_reservations", value.kernel_v2_org_reservations)
            .required("passkey_users", value.passkey_users)
            .required("passkey_credentials", value.passkey_credentials)
            .required("agent_messages", value.agent_messages)
            .required("embeddings_v2_at_most_10000", value.embeddings_v2_at_most_10000)
            .required("legacy_model_key_configs", value.legacy_model_key_configs)
            .required("legacy_model_key_health", value.legacy_model_key_health)
            .required("codex_reset_usage", value.codex_reset_usage)
            .done();
  }

  KvMigrationValidationResult::Counts tag_invoke(const json::value_to_tag<KvMigrationValidationResult::Counts>&, const json::value& value) {
    const Reader in(value, "KvMigrationValidationResult.counts");
    KvMigrationValidationResult::Counts counts{.api_key_ids = in.required<double>("api_key_ids"),
                                               .api_key_hashes = in.required<double>("api_key_hashes"),
                                               .api_key_bounded_counters_v2 = in.required<double>("api_key_bounded_counters_v2"),
                                               .api_key_bounded_counter_baselines_v2 = in.required<double>("api_key_bounded_counter_baselines_v2"),
                                               .api_key_bounded_counter_reconciled_baselines_v2 = in.required<double>("api_key_bounded_counter_reconciled_baselines_v2"),
                                               .api_key_usage_v3_windows = in.required<double>("api_key_usage_v3_windows"),
                                               .api_key_usage_v3_requests = in.required<double>("api_key_usage_v3_requests"),
                                               .paid_fallback_ledger = in.required<double>("paid_fallback_ledger"),
                                               .paid_fallback_v3_windows = in.required<double>("paid_fallback_v3_windows"),
                                               .paid_fallback_v3_requests = in.required<double>("paid_fallback_v3_requests"),
                                               .paid_fallback_v3_pending = in.required<double>("paid_fallback_v3_pending"),
                                               .paid_fallback_v3_reconciliation_leases = in.required<double>("paid_fallback_v3_reconciliation_leases"),
                                               .paid_fallback_v3_deletion_guards = in.required<double>("paid_fallback_v3_deletion_guards"),
                                               .kernel_repo_limits = in.required<double>("kernel_repo_limits"),
                                               .kernel_org_limits = in.required<double>("kernel_org_limits"),
                                               .kernel_v2_repo_policies = in.required<double>("kernel_v2_repo_policies"),
                                               .kernel_v2_org_policies = in.required<double>("kernel_v2_org_policies"),
                                               .kernel_v2_repo_windows = in.required<double>("kernel_v2_repo_windows"),
                                               .kernel_v2_org_windows = in.required<double>("kernel_v2_org_windows"),
                                               .kernel_v2_repo_reservations = in.required<double>("kernel_v2_repo_reservations"),
                                               .kernel_v2_org_reservations = in.required<double>("kernel_v2_org_reservations"),
                                               .passkey_users = in.required<double>("passkey_users"),
                                               .passkey_credentials = in.required<double>("passkey_credentials"),
                                               .agent_messages = in.required<double>("agent_messages"),
                                               .embeddings_v2_at_most_10000 = in.required<double>("embeddings_v2_at_most_10000"),
                                               .legacy_model_key_configs = in.required<double>("legacy_model_key_configs"),
                                               .legacy_model_key_health = in.required<double>("legacy_model_key_health"),
                                               .codex_reset_usage = in.required<double>("codex_reset_usage")};
    return counts;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KvMigrationValidationResult::SettingsPresent& value) {
    out = Writer()
            .required("codex_auth", value.codex_auth)
            .required("codex_models", value.codex_models)
            .required("default_model", value.default_model)
            .required("default_reasoning_effort", value.default_reasoning_effort)
            .required("default_kernel_policy_limit_requests", value.default_kernel_policy_limit_requests)
            .required("default_kernel_policy_window_ms", value.default_kernel_policy_window_ms)
            .required("voyage_api_key", value.voyage_api_key)
            .required("kernel_pubkeys", value.kernel_pubkeys)
            .required("runtime_config_v2", value.runtime_config_v2)
            .done();
  }

  KvMigrationValidationResult::SettingsPresent tag_invoke(const json::value_to_tag<KvMigrationValidationResult::SettingsPresent>&, const json::value& value) {
    const Reader in(value, "KvMigrationValidationResult.settings_present");
    KvMigrationValidationResult::SettingsPresent settings{.codex_auth = in.required<bool>("codex_auth"),
                                                          .codex_models = in.required<bool>("codex_models"),
                                                          .default_model = in.required<bool>("default_model"),
                                                          .default_reasoning_effort = in.required<bool>("default_reasoning_effort"),
                                                          .default_kernel_policy_limit_requests = in.required<bool>("default_kernel_policy_limit_requests"),
                                                          .default_kernel_policy_window_ms = in.required<bool>("default_kernel_policy_window_ms"),
                                                          .voyage_api_key = in.required<bool>("voyage_api_key"),
                                                          .kernel_pubkeys = in.required<bool>("kernel_pubkeys"),
                                                          .runtime_config_v2 = in.required<bool>("runtime_config_v2")};
    return settings;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KvMigrationValidationResult& value) {
    out = Writer().required("counts", value.counts).required("settings_present", value.settings_present).required("errors", value.errors).done();
  }

  KvMigrationValidationResult tag_invoke(const json::value_to_tag<KvMigrationValidationResult>&, const json::value& value) {
    const Reader in(value, "KvMigrationValidationResult");
    KvMigrationValidationResult result{.counts = in.required<KvMigrationValidationResult::Counts>("counts"),
                                       .settings_present = in.required<KvMigrationValidationResult::SettingsPresent>("settings_present"),
                                       .errors = in.required<std::vector<std::string>>("errors")};
    return result;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KvMigrationImportResult& value) {
    json::object groups;
    for (const auto& [group, count] : value.groups) rt::objectDefine(groups, group, rt::makeNumber(count));
    out = Writer()
            .required("total", value.total)
            .required("imported", value.imported)
            .required("skipped", value.skipped)
            .required("optional", value.optional)
            .required("errors", value.errors)
            .required("groups", groups)
            .done();
  }
}
