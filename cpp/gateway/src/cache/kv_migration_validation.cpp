#include "uos/cache/kv_migration_validation.hpp"

#include "uos/api_key_policy.hpp"
#include "uos/cache/kv_migration_paid_fallback.hpp"
#include "uos/cache/kv_migration_usage.hpp"
#include "uos/codex/banked_reset.hpp"
#include "uos/codex/manual_reset_state.hpp"
#include "uos/codex/reset_settings.hpp"
#include "uos/kernel/quota_v2.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/runtime_config.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uos::cache::kv_migration_validation {
  namespace {
    using kv_migration_base::KvMigrationValidationResult;
    using kv_migration_base::listKvMigrationCount;
    using kv_migration_paid_fallback::API_KEY_USAGE_V2_MIGRATION_BASELINE_PREFIX;
    using kv_migration_paid_fallback::inspectPaidFallbackV3;
    using kv_migration_paid_fallback::PAID_FALLBACK_LEDGER_PREFIX;
    using kv_migration_paid_fallback::paidFallbackLedgerReference;
    using kv_migration_paid_fallback::pendingPaidFallbackLedgerReferenceFromEntry;
    using kv_migration_usage::StrictApiKeyPair;
    namespace quota_v2 = kernel::quota_v2;

    // The helpers below that take references are awaited at once by their callers, whose locals
    // outlive them (the TS passes the same objects). The pairs come from inspectStrictApiKeyPairs,
    // so their `id`, `hash` and reservation id members are the strings its guards accepted.

    // `KvMigrationTargetCounts`
    struct KvMigrationTargetCounts {
      double apiIds;
      double apiHashes;
      double boundedCounters;
      double boundedCounterBaselines;
      double reconciledBoundedCounterBaselines;
      double paidFallbackLedger;
      double kernelLimits;
      double kernelOrgLimits;
      double apiKeyUsageV3Windows;
      double apiKeyUsageV3Requests;
      double kernelV2RepoPolicies;
      double kernelV2OrgPolicies;
      double kernelV2RepoWindows;
      double kernelV2OrgWindows;
      double passkeyUsers;
      double passkeyCredentials;
      double agentMessages;
      double embeddingCache;
      double legacyModelKeyConfigs;
      double legacyModelKeyHealth;
      double codexResetUsage;
    };

    bool isUndefinedValue(const rt::KvValue& value) noexcept { return value.isUndefined() || value.is<rt::KvHole>(); }
    bool isNumberValue(const rt::KvValue& value, double number) { return value.isNumber() && value.asNumber() == number; }

    // `left` SameValueZero `right` (Array.prototype.includes) on stored values.
    bool sameValueZero(const rt::KvValue& left, const rt::KvValue& right) {
      if (isUndefinedValue(left)) return isUndefinedValue(right);
      if (left.isNull()) return right.isNull();
      if (left.isBool()) return right.isBool() && left.asBool() == right.asBool();
      if (left.isNumber()) {
        if (!right.isNumber()) return false;
        return left.asNumber() == right.asNumber() || (rt::Number::isNaN(left.asNumber()) && rt::Number::isNaN(right.asNumber()));
      }
      if (left.isString()) return right.isString() && left.asString() == right.asString();
      if (left.isBigInt()) return right.isBigInt() && left.asBigInt() == right.asBigInt();
      return false;
    }

    // `key[index] === text`
    bool partIs(const rt::KvKey& key, std::size_t index, std::string_view text) { return index < key.size() && key[index].isString() && key[index].string() == text; }

    /** Counts every durable row group the migration validation reports on. */
    rt::Task<KvMigrationTargetCounts> countKvMigrationTargetRows(std::shared_ptr<rt::Kv> kv) {
      std::vector<rt::Promise<double>> counting;
      counting.push_back(listKvMigrationCount(kv, rt::KvKey{"ubq_ai", "api_keys", "id"}));
      counting.push_back(listKvMigrationCount(kv, rt::KvKey{"ubq_ai", "api_keys", "hash"}));
      counting.push_back(listKvMigrationCount(kv, rt::KvKey{"uos_ai", "api_key_usage", "v2"}));
      counting.push_back(listKvMigrationCount(kv, API_KEY_USAGE_V2_MIGRATION_BASELINE_PREFIX));
      counting.push_back(kv_migration_usage::countReconciledBoundedCounterBaselines(kv));
      counting.push_back(listKvMigrationCount(kv, rt::KvKey{"uos_ai", "paid_fallback", "ledger"}));
      counting.push_back(listKvMigrationCount(kv, rt::KvKey{"ubq_ai", "kernel_auth", "limits"}));
      counting.push_back(listKvMigrationCount(kv, rt::KvKey{"ubq_ai", "kernel_auth", "org_limits"}));
      counting.push_back(listKvMigrationCount(kv, api_key_policy::API_KEY_USAGE_V3_WINDOW_PREFIX));
      counting.push_back(listKvMigrationCount(kv, api_key_policy::API_KEY_USAGE_V3_REQUEST_PREFIX));
      counting.push_back(listKvMigrationCount(kv, quota_v2::KERNEL_REPO_POLICY_V2_PREFIX));
      counting.push_back(listKvMigrationCount(kv, quota_v2::KERNEL_ORG_POLICY_V2_PREFIX));
      counting.push_back(listKvMigrationCount(kv, quota_v2::KERNEL_REPO_WINDOW_V2_PREFIX));
      counting.push_back(listKvMigrationCount(kv, quota_v2::KERNEL_ORG_WINDOW_V2_PREFIX));
      counting.push_back(listKvMigrationCount(kv, rt::KvKey{"uos_ai", "auth", "users"}));
      counting.push_back(listKvMigrationCount(kv, rt::KvKey{"uos_ai", "auth", "credentials"}));
      counting.push_back(listKvMigrationCount(kv, rt::KvKey{"agent_messages"}));
      counting.push_back(listKvMigrationCount(kv, rt::KvKey{"embeddings", "v2"}, 10'000));
      counting.push_back(listKvMigrationCount(kv, rt::KvKey{"key", "config"}));
      counting.push_back(listKvMigrationCount(kv, rt::KvKey{"key", "health"}));
      counting.push_back(listKvMigrationCount(kv, codex::reset_settings::CODEX_RESET_USAGE_PREFIX));
      const std::vector<double> counts = co_await rt::all(std::move(counting)).wait();
      co_return KvMigrationTargetCounts{
        .apiIds = counts[0],
        .apiHashes = counts[1],
        .boundedCounters = counts[2],
        .boundedCounterBaselines = counts[3],
        .reconciledBoundedCounterBaselines = counts[4],
        .paidFallbackLedger = counts[5],
        .kernelLimits = counts[6],
        .kernelOrgLimits = counts[7],
        .apiKeyUsageV3Windows = counts[8],
        .apiKeyUsageV3Requests = counts[9],
        .kernelV2RepoPolicies = counts[10],
        .kernelV2OrgPolicies = counts[11],
        .kernelV2RepoWindows = counts[12],
        .kernelV2OrgWindows = counts[13],
        .passkeyUsers = counts[14],
        .passkeyCredentials = counts[15],
        .agentMessages = counts[16],
        .embeddingCache = counts[17],
        .legacyModelKeyConfigs = counts[18],
        .legacyModelKeyHealth = counts[19],
        .codexResetUsage = counts[20],
      };
    }

    /** True when the reservation has a matching row in the dedicated ledger. */
    rt::Task<bool> hasPendingPaidFallbackLedger(std::shared_ptr<rt::Kv> kv, std::string keyId, std::string requestId) {
      const std::string expected = paidFallbackLedgerReference(keyId, requestId);
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = rt::KvKey{PAID_FALLBACK_LEDGER_PREFIX, keyId}});
      while (std::optional<rt::KvEntry<>> ledger = co_await iterator->next()) {
        const std::optional<std::string> reference = pendingPaidFallbackLedgerReferenceFromEntry(*ledger, PAID_FALLBACK_LEDGER_PREFIX);
        if (reference == expected) {
          co_await iterator->return_();
          co_return true;
        }
      }
      co_return false;
    }

    /** Reports a bounded key whose V3 aggregate or migration baseline is invalid. */
    rt::Task<void> auditApiKeyBoundedAggregates(std::shared_ptr<rt::Kv> kv, const api_key_policy::ApiKeyPolicy& policy, const rt::KvValue& record,
                                                std::vector<std::string>& errors) {
      const rt::KvKey windowKey = api_key_policy::apiKeyUsageV3WindowKey(policy);
      const rt::KvEntryMaybe<> windowEntry = co_await kv->get(windowKey);
      const std::optional<::uos::types::ApiKeyUsageWindowV3> v3Window = api_key_policy::normalizeApiKeyUsageWindowV3(rt::storedJsonView(windowEntry.value));
      if (!v3Window || v3Window->key_id != policy.key_id || v3Window->policy_version != policy.policy_version || v3Window->window_start_ms != policy.window_start_ms ||
          v3Window->window_reset_at_ms != policy.usage_reset_at_ms) {
        errors.push_back("bounded V3 aggregate is missing or invalid: " + rt::kvToString(record["id"]));
      }
      const rt::KvKey baselineKey = kv_migration_usage::apiKeyUsageV2MigrationBaselineKey(policy);
      const rt::KvEntryMaybe<> baselineEntry = co_await kv->get(baselineKey);
      const std::optional<kv_migration_usage::ApiKeyUsageV2MigrationBaseline> baseline = kv_migration_usage::normalizeApiKeyUsageV2MigrationBaseline(baselineEntry.value);
      if (!baseline || baseline->key_id != policy.key_id || baseline->policy_version != policy.policy_version || baseline->window_start_ms != policy.window_start_ms) {
        errors.push_back("bounded counter migration baseline is missing, stale, or invalid: " + rt::kvToString(record["id"]));
      }
    }

    /** Reports why one api-key pair is not fully migrated. */
    rt::Task<void> auditApiKeyPolicy(std::shared_ptr<rt::Kv> kv, const StrictApiKeyPair& pair, double validationNowMs, std::vector<std::string>& errors) {
      const rt::KvValue& record = pair.record;
      const rt::KvValue& hashRecord = pair.hashRecord;
      const rt::KvValue& reservedRequestId = record["paid_fallback_reservation_request_id"];
      if (!reservedRequestId.isNull()) {
        rt::Task<bool> checking = hasPendingPaidFallbackLedger(kv, record["id"].asString(), reservedRequestId.asString());
        const bool found = co_await std::move(checking);
        if (!found) {
          errors.push_back("paid fallback reservation has no pending ledger record: " + rt::kvToString(record["id"]));
        }
      }
      if (!isNumberValue(record["usage_quota_version"], 3) || !isNumberValue(hashRecord["usage_quota_version"], 3)) {
        errors.push_back("api key quota ledger version is not V3: " + rt::kvToString(record["id"]));
        co_return;
      }
      const std::optional<api_key_policy::ApiKeyPolicy> policy = api_key_policy::apiKeyPolicyFromHashRecord(
        record["hash"].asString(), rt::storedJsonView(hashRecord), kv_migration_usage::migrationPolicyNow(record, validationNowMs));
      if (!policy) {
        errors.push_back("api key policy could not be normalized: " + rt::kvToString(record["id"]));
        co_return;
      }
      if (!isNumberValue(record["usage_limit_requests"], -1)) co_await auditApiKeyBoundedAggregates(kv, *policy, record, errors);
    }

    /** Reports a default model that the uploaded Codex snapshot cannot serve. */
    rt::Task<void> auditCodexModelSnapshot(std::shared_ptr<rt::Kv> kv, std::vector<std::string>& errors) {
      const rt::KvEntryMaybe<> codexModels = co_await kv->get(rt::KvKey{"ubq_ai", "codex_models"});
      const rt::KvEntryMaybe<> defaultModel = co_await kv->get(rt::KvKey{"default", "model"});
      // `Array.isArray(codexModels.value?.models) ? codexModels.value.models : []`
      const rt::KvValue& models = codexModels.value["models"];
      const rt::KvArray modelList = models.isArray() ? models.asArray() : rt::KvArray{};
      const rt::KvValue& defaultValue = defaultModel.value;
      if (defaultValue.truthy() && !codexModels.value.isNull() && modelList.empty()) {
        errors.push_back("codex model snapshot is empty or malformed; cannot validate default model: " + rt::kvToString(defaultValue));
      }
      if (defaultValue.truthy() && !modelList.empty()) {
        bool found = false;
        for (const rt::KvValue& model : modelList) {
          // `some` skips holes.
          if (model.is<rt::KvHole>()) continue;
          if (!model.truthy() || rt::kvTypeof(model) != "object") continue;
          const rt::KvValue& record = model;
          if (sameValueZero(record["slug"], defaultValue) || sameValueZero(record["id"], defaultValue) || sameValueZero(record["model"], defaultValue) ||
              sameValueZero(record["name"], defaultValue)) {
            found = true;
            break;
          }
        }
        if (!found) errors.push_back("default model is not present in codex model snapshot: " + rt::kvToString(defaultValue));
      }
    }

    bool validResetDailyEntry(const rt::KvKey& key, const rt::KvValue& value) {
      static const rt::Regex ACCOUNT_HASH("^[a-f0-9]{64}$");
      static const rt::Regex DAY("^\\d{4}-\\d{2}-\\d{2}$");
      const bool accountDay = partIs(key, 2, "account_day");
      if (!accountDay && !partIs(key, 2, "global_day")) return false;
      const std::size_t expectedLength = accountDay ? 6 : 5;
      if (!partIs(key, 3, "v1") || key.size() != expectedLength) return false;
      if (accountDay && (!key[4].isString() || !ACCOUNT_HASH.test(key[4].string()))) return false;
      const rt::KvKeyPart& dayKey = key[expectedLength - 1];
      return dayKey.isString() && DAY.test(dayKey.string()) && codex::banked_reset::parseAccountDayRecord(rt::storedJsonView(value), dayKey.string()).has_value();
    }

    /** Reports reset usage rows whose key or explicit boolean setting is malformed. */
    rt::Task<void> auditCodexResetUsage(std::shared_ptr<rt::Kv> kv, std::vector<std::string>& errors) {
      const rt::KvKey& prefix = codex::reset_settings::CODEX_RESET_USAGE_PREFIX;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = prefix});
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        const rt::KvKey& key = entry->key;
        const bool validKey = key.size() == 5 && rt::kvKeyPartStrictEquals(key[0], prefix[0]) && rt::kvKeyPartStrictEquals(key[1], prefix[1]) && partIs(key, 2, "account") &&
                              partIs(key, 3, "v1") && key[4].isString() && !key[4].string().empty();
        if (!validKey) {
          errors.push_back("codex reset usage key is malformed: " + *rt::kvJsonStringify(rt::kvValueFromKey(key)));
          continue;
        }
        if (!rt::kvIsObject(entry->value) || !entry->value["enabled"].isBool()) {
          errors.push_back("codex reset usage record is malformed: " + *rt::kvJsonStringify(rt::kvValueFromKey(key)));
        }
      }
    }

    /** Reads every settings key the validation reports presence for. */
    rt::Task<std::vector<rt::KvEntryMaybe<>>> readKvMigrationSettings(std::shared_ptr<rt::Kv> kv) {
      std::vector<rt::Promise<rt::KvEntryMaybe<>>> reads;
      reads.push_back(kv->get(rt::KvKey{"ubq_ai", "codex_auth"}));
      reads.push_back(kv->get(rt::KvKey{"ubq_ai", "codex_models"}));
      reads.push_back(kv->get(rt::KvKey{"default", "model"}));
      reads.push_back(kv->get(rt::KvKey{"default", "reasoning_effort"}));
      reads.push_back(kv->get(rt::KvKey{"default", "kernel_policy_limit_requests"}));
      reads.push_back(kv->get(rt::KvKey{"default", "kernel_policy_window_ms"}));
      reads.push_back(kv->get(rt::KvKey{"uos_ai", "voyage_api_key"}));
      reads.push_back(kv->get(rt::KvKey{"uos_ai", "kernel_pubkeys"}));
      reads.push_back(kv->get(runtime_config::RUNTIME_CONFIG_V2_KEY));
      std::vector<rt::KvEntryMaybe<>> settings = co_await rt::all(std::move(reads)).wait();
      co_return settings;
    }

    rt::Task<KvMigrationValidationResult> validateTarget(std::shared_ptr<rt::Kv> kv) {
      std::vector<std::string> errors;
      const KvMigrationTargetCounts counts = co_await countKvMigrationTargetRows(kv);

      if (counts.apiIds != counts.apiHashes) {
        errors.push_back("api key id/hash count mismatch: ids=" + rt::numberToString(counts.apiIds) + " hashes=" + rt::numberToString(counts.apiHashes));
      }

      const kv_migration_usage::StrictApiKeyPairsInventory apiKeyInventory = co_await kv_migration_usage::inspectStrictApiKeyPairs(kv);
      errors.insert(errors.end(), apiKeyInventory.errors.begin(), apiKeyInventory.errors.end());
      rt::OrderedSet<std::string> knownKeyIds;
      for (const StrictApiKeyPair& pair : apiKeyInventory.pairs) knownKeyIds.add(pair.record["id"].asString());
      rt::Task<kv_migration_usage::ApiKeyUsageV3Inventory> inspectingUsage = kv_migration_usage::inspectApiKeyUsageV3(kv, knownKeyIds);
      const kv_migration_usage::ApiKeyUsageV3Inventory apiKeyUsageV3 = co_await std::move(inspectingUsage);
      errors.insert(errors.end(), apiKeyUsageV3.errors.begin(), apiKeyUsageV3.errors.end());
      const kv_migration_usage::KernelQuotaV2Inventory kernelQuotaV2 = co_await kv_migration_usage::inspectKernelQuotaV2(kv);
      errors.insert(errors.end(), kernelQuotaV2.errors.begin(), kernelQuotaV2.errors.end());
      if (counts.kernelLimits > 0 || counts.kernelOrgLimits > 0) {
        errors.push_back("legacy combined kernel quota records remain: repo=" + rt::numberToString(counts.kernelLimits) +
                         " org=" + rt::numberToString(counts.kernelOrgLimits));
      }
      rt::OrderedSet<std::string> unlimitedKeyIds;
      for (const StrictApiKeyPair& pair : apiKeyInventory.pairs) {
        if (isNumberValue(pair.record["paid_fallback_limit_microcredits"], -1)) unlimitedKeyIds.add(pair.record["id"].asString());
      }
      rt::Task<kv_migration_paid_fallback::PaidFallbackV3Inventory> inspectingPaidFallback = inspectPaidFallbackV3(kv, knownKeyIds, unlimitedKeyIds);
      const kv_migration_paid_fallback::PaidFallbackV3Inventory paidFallbackV3 = co_await std::move(inspectingPaidFallback);
      errors.insert(errors.end(), paidFallbackV3.errors.begin(), paidFallbackV3.errors.end());
      const double validationNowMs = rt::dateNow();
      for (const StrictApiKeyPair& pair : apiKeyInventory.pairs) {
        co_await auditApiKeyPolicy(kv, pair, validationNowMs, errors);
      }
      co_await auditCodexModelSnapshot(kv, errors);
      co_await auditCodexResetUsage(kv, errors);
      co_await auditManualResetIntents(kv, errors);
      co_await auditCodexResetRedemptions(kv, errors);

      const std::vector<rt::KvEntryMaybe<>> knownSettings = co_await readKvMigrationSettings(kv);
      if (!runtime_config::normalizeRuntimeConfig(knownSettings[8].value)) {
        errors.push_back("runtime config v2 is missing or invalid");
      }

      co_return KvMigrationValidationResult{
        .counts =
          KvMigrationValidationResult::Counts{
            .api_key_ids = counts.apiIds,
            .api_key_hashes = counts.apiHashes,
            .api_key_bounded_counters_v2 = counts.boundedCounters,
            .api_key_bounded_counter_baselines_v2 = counts.boundedCounterBaselines,
            .api_key_bounded_counter_reconciled_baselines_v2 = counts.reconciledBoundedCounterBaselines,
            .api_key_usage_v3_windows = counts.apiKeyUsageV3Windows,
            .api_key_usage_v3_requests = counts.apiKeyUsageV3Requests,
            .paid_fallback_ledger = counts.paidFallbackLedger,
            .paid_fallback_v3_windows = paidFallbackV3.windows,
            .paid_fallback_v3_requests = paidFallbackV3.requests,
            .paid_fallback_v3_pending = paidFallbackV3.pending,
            .paid_fallback_v3_reconciliation_leases = paidFallbackV3.reconciliationLeases,
            .paid_fallback_v3_deletion_guards = paidFallbackV3.deletionGuards,
            .kernel_repo_limits = counts.kernelLimits,
            .kernel_org_limits = counts.kernelOrgLimits,
            .kernel_v2_repo_policies = counts.kernelV2RepoPolicies,
            .kernel_v2_org_policies = counts.kernelV2OrgPolicies,
            .kernel_v2_repo_windows = counts.kernelV2RepoWindows,
            .kernel_v2_org_windows = counts.kernelV2OrgWindows,
            .kernel_v2_repo_reservations = kernelQuotaV2.repoReservations,
            .kernel_v2_org_reservations = kernelQuotaV2.orgReservations,
            .passkey_users = counts.passkeyUsers,
            .passkey_credentials = counts.passkeyCredentials,
            .agent_messages = counts.agentMessages,
            .embeddings_v2_at_most_10000 = counts.embeddingCache,
            .legacy_model_key_configs = counts.legacyModelKeyConfigs,
            .legacy_model_key_health = counts.legacyModelKeyHealth,
            .codex_reset_usage = counts.codexResetUsage,
          },
        .settings_present =
          KvMigrationValidationResult::SettingsPresent{
            .codex_auth = !knownSettings[0].value.isNull(),
            .codex_models = !knownSettings[1].value.isNull(),
            .default_model = !knownSettings[2].value.isNull(),
            .default_reasoning_effort = !knownSettings[3].value.isNull(),
            .default_kernel_policy_limit_requests = !knownSettings[4].value.isNull(),
            .default_kernel_policy_window_ms = !knownSettings[5].value.isNull(),
            .voyage_api_key = !knownSettings[6].value.isNull(),
            .kernel_pubkeys = !knownSettings[7].value.isNull(),
            .runtime_config_v2 = runtime_config::normalizeRuntimeConfig(knownSettings[8].value).has_value(),
          },
        .errors = std::move(errors),
      };
    }
  }

  rt::Task<void> auditCodexResetRedemptions(std::shared_ptr<rt::Kv> kv, std::vector<std::string>& errors) {
    const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = rt::KvKey{"uos_ai", "codex_reset_redemption"}});
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      const rt::KvKey& key = entry->key;
      if (partIs(key, 2, "v1")) {
        const std::optional<::uos::types::CodexResetRedemptionRecord> record = codex::banked_reset::parseCodexResetRedemptionRecord(rt::storedJsonView(entry->value));
        if (key.size() != 5 || !key[3].isString() || !key[4].isString() || !record) {
          errors.push_back("codex reset redemption is malformed");
          continue;
        }
        if (key[3].string() != record->account_id_hash || key[4].string() != record->quota_generation) errors.push_back("codex reset redemption is malformed");
        continue;
      }
      if (!validResetDailyEntry(key, entry->value)) errors.push_back("codex reset daily budget is malformed");
    }
  }

  rt::Task<void> auditManualResetIntents(std::shared_ptr<rt::Kv> kv, std::vector<std::string>& errors) {
    // parseManualResetIntent counts `Object.keys(value)`: a member holding undefined is a key.
    rt::StoredJsonViewOptions keysView;
    keysView.undefinedMember = rt::StoredJsonViewOptions::UndefinedView::emptyObject;
    const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = codex::manual_reset_state::CODEX_MANUAL_RESET_PREFIX});
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      const std::optional<codex::manual_reset_state::ManualIntent> intent = codex::manual_reset_state::parseManualResetIntent(rt::storedJsonView(entry->value, keysView));
      if (entry->key.size() != 4 || !entry->key[3].isString() || !intent) {
        errors.push_back("codex manual reset intent is malformed");
        continue;
      }
      if (entry->key[3].string() != intent->account_id_hash) errors.push_back("codex manual reset intent is malformed");
    }
  }

  rt::Task<KvMigrationValidationResult> validateKvMigrationTarget(std::shared_ptr<rt::Kv> kv) { return validateTarget(std::move(kv)); }
}
