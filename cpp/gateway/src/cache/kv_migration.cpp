#include "uos/cache/kv_migration.hpp"

#include "uos/api_key_policy.hpp"
#include "uos/api_keys.hpp"
#include "uos/cache/kv_migration_projection.hpp"
#include "uos/cache/kv_migration_usage.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/runtime_config.hpp"

#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace uos::cache::kv_migration {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
    using MigratedApiKeyPair = kv_migration_usage::StrictApiKeyPair;
    const rt::KvGetOptions STRONG{.consistency = rt::KvConsistencyLevel::strong};

    struct ApiKeyMigrationProgress {
      double apiKeys = 0;
      double boundedCounters = 0;
      double boundedBaselinesCreated = 0;
      double boundedBaselinesReconciled = 0;
      double legacyUsageDeltaApplied = 0;
    };

    /** Upgrades one api-key pair to the V3 quota ledger and migrates its counter. */
    // progress belongs to the outer migration, which awaits this helper before continuing.
    rt::Task<MigratedApiKeyPair> migrateApiKeyQuotaV3(std::shared_ptr<rt::Kv> kv, rt::KvValue record, double migrationNowMs,
                                                     bool handoffAlreadyInitialized, ApiKeyMigrationProgress& progress) {
      const std::string id = record["id"].asString();
      const std::string hash = record["hash"].asString();
      const rt::KvKey idKey = api_keys::apiKeyIdKey(id);
      const rt::KvKey hashKey = api_keys::apiKeyHashKey(hash);
      rt::Promise<rt::KvEntryMaybe<>> idRead(kv->get(idKey, STRONG));
      rt::Promise<rt::KvEntryMaybe<>> hashRead(kv->get(hashKey, STRONG));
      auto reads = rt::all(idRead, hashRead);
      auto readEntries = co_await reads.wait();
      auto& [idEntry, hashEntry] = readEntries;
      if (!idEntry.value.truthy() || !hashEntry.value.truthy()) throw rt::JsError("API key changed during quota V3 migration: " + id);
      if (idEntry.value["hash"] != record["hash"] || !kv_migration_paid_fallback::apiKeyHashPolicyMatches(idEntry.value, hashEntry.value)) {
        throw rt::JsError("API key changed during quota V3 migration: " + id);
      }
      rt::KvObject upgradedIdObject = idEntry.value.asObject();
      upgradedIdObject.set("usage_quota_version", 3);
      rt::KvValue upgradedRecord(std::move(upgradedIdObject));
      rt::KvObject upgradedHashObject = hashEntry.value.asObject();
      upgradedHashObject.set("usage_quota_version", 3);
      rt::KvValue upgradedHash(std::move(upgradedHashObject));
      const auto& idVersion = idEntry.value["usage_quota_version"];
      const auto& hashVersion = hashEntry.value["usage_quota_version"];
      if (!idVersion.isNumber() || idVersion.asNumber() != 3 || !hashVersion.isNumber() || hashVersion.asNumber() != 3) {
        auto upgrade = kv->atomic();
        upgrade->check(idEntry).check(hashEntry).set(idKey, upgradedRecord).set(hashKey, upgradedHash);
        const auto upgraded = co_await upgrade->commit();
        if (!upgraded.ok) throw rt::JsError("API key changed during quota V3 migration: " + id);
      }
      const auto hashView = rt::storedJsonView(upgradedHash);
      const auto policy = api_key_policy::apiKeyPolicyFromHashRecord(upgradedRecord["hash"].asString(), hashView,
        kv_migration_usage::migrationPolicyNow(upgradedRecord, migrationNowMs));
      if (!policy) throw rt::JsError("API key " + id + " policy could not be normalized");
      progress.apiKeys += 1;
      auto handoffTask = kv_migration_usage::migrateBoundedCounterHandoff(kv, *policy,
        kv_migration_usage::currentLegacyUsage(upgradedRecord, migrationNowMs), migrationNowMs, handoffAlreadyInitialized);
      rt::Promise<kv_migration_usage::BoundedCounterHandoffResult> handoffPromise(std::move(handoffTask));
      const auto handoff = co_await handoffPromise.wait();
      if (policy->usage_limit_requests != -1) {
        if (handoff.baseline_created) progress.boundedBaselinesCreated += 1;
        if (handoff.baseline_reconciled) progress.boundedBaselinesReconciled += 1;
        progress.boundedCounters += 1;
      }
      progress.legacyUsageDeltaApplied += handoff.legacy_usage_delta_applied;
      co_return MigratedApiKeyPair{.record = std::move(upgradedRecord), .hashRecord = std::move(upgradedHash)};
    }

    /** Phase label reported for the bounded-counter handoff of this run. */
    std::string boundedCounterHandoffPhase(double boundedCounters, double reconciled) {
      if (boundedCounters == 0) return "no_bounded_keys";
      if (reconciled == boundedCounters) return "postdeploy_reconcile";
      if (reconciled == 0) return "predeploy_seed";
      return "mixed";
    }

    /** Copies every pending legacy ledger row into the dedicated ledger. */
    rt::Task<double> migratePendingPaidFallbackLedgers(std::shared_ptr<rt::Kv> kv,
      std::vector<kv_migration_projection::LegacyPaidFallbackLedgerCandidate> candidates) {
      double paidFallbackRecords = 0;
      for (const auto& candidate : candidates) {
        if (candidate.existing.value.isNull()) {
          auto operation = kv->atomic();
          operation->check(candidate.existing).set(candidate.ledgerKey, candidate.value);
          const auto commit = co_await operation->commit();
          if (!commit.ok) {
            const auto concurrent = co_await kv->get(candidate.ledgerKey);
            if (concurrent.value.isNull()) {
              throw rt::JsError("Paid fallback ledger changed during migration: " + rt::kvToString(candidate.value["id"]));
            }
          }
        }
        paidFallbackRecords += 1;
      }
      co_return paidFallbackRecords;
    }
  }

  rt::Task<KvReadIncidentV2MigrationResult> migrateKvReadIncidentV2(std::shared_ptr<rt::Kv> kv) {
    const double migrationNowMs = rt::dateNow();
    const auto previousMigration = co_await kv->get(KV_READ_INCIDENT_V2_MIGRATION_KEY, STRONG);
    const auto& handoffVersion = previousMigration.value["api_key_quota_v3_handoff_version"];
    const bool handoffAlreadyInitialized = handoffVersion.isNumber() && handoffVersion.asNumber() == 1;
    const auto codexModels = co_await kv->get({"ubq_ai", "codex_models"});
    if (!codexModels.value.truthy()) throw rt::JsError("Codex model snapshot is missing");
    const auto defaultModel = co_await kv->get({"default", "model"});
    const auto defaultReasoning = co_await kv->get({"default", "reasoning_effort"});
    using UndefinedView = rt::StoredJsonViewOptions::UndefinedView;
    const rt::StoredJsonViewOptions catalogViewOptions{.undefinedMember = UndefinedView::emptyObject,
      .undefinedElement = UndefinedView::emptyObject, .hole = UndefinedView::emptyObject,
      .topLevelUndefined = UndefinedView::emptyObject, .kvU64Members = false, .typedArrayMembers = false};
    auto catalogView = rt::storedJsonView(codexModels.value, catalogViewOptions);
    // Error.name is inherited and is the source compactor's final slug candidate.
    if (catalogView.is_object() && codexModels.value["models"].isArray()) {
      auto* models = catalogView.as_object().if_contains("models");
      const auto& storedModels = codexModels.value["models"].asArray();
      for (std::size_t index = 0; index < storedModels.size(); ++index) {
        if (const auto* error = std::get_if<rt::KvError>(&storedModels[index].variant())) {
          models->as_array()[index] = json::object{{"name", error->name}};
        }
      }
    }
    if (!catalogView.is_object()) {
      runtime_config::compactRuntimeCodexModels(&catalogView);
    }
    if (!defaultModel.value.isNull() && !defaultModel.value.isUndefined() && !defaultModel.value.isString()) {
      // buildRuntimeConfig compacts the catalog before calling the unchecked default's trim.
      runtime_config::compactRuntimeCodexModels(&catalogView);
      throw rt::TypeError("options.defaultModel?.trim is not a function. (In 'options.defaultModel?.trim()', 'options.defaultModel?.trim' is undefined)");
    }
    runtime_config::BuildRuntimeConfigOptions runtimeOptions;
    if (defaultModel.value.isString()) runtimeOptions.defaultModel = defaultModel.value.asString();
    if (defaultReasoning.value.isString()) runtimeOptions.defaultReasoningEffort = defaultReasoning.value.asString();
    const auto runtimeConfig = runtime_config::buildRuntimeConfig(catalogView.as_object(), runtimeOptions);

    rt::Promise<kv_migration_usage::StrictApiKeyPairsInventory> apiKeyRead(kv_migration_usage::inspectStrictApiKeyPairs(kv));
    const auto apiKeyInventory = co_await apiKeyRead.wait();
    if (!apiKeyInventory.errors.empty()) throw rt::JsError("API key policy validation failed: " + rt::join(apiKeyInventory.errors, "; "));
    auto paidReadTask = kv_migration_projection::inspectPendingPaidFallbackLedgers(kv, apiKeyInventory.pairs);
    rt::Promise<kv_migration_projection::PendingPaidFallbackLedgerInventory> paidRead(std::move(paidReadTask));
    const auto paidFallbackInventory = co_await paidRead.wait();
    if (!paidFallbackInventory.errors.empty()) throw rt::JsError("Paid fallback ledger validation failed: " + rt::join(paidFallbackInventory.errors, "; "));

    ApiKeyMigrationProgress progress;
    std::vector<MigratedApiKeyPair> upgradedApiKeyPairs;
    for (const auto& pair : apiKeyInventory.pairs) {
      auto upgradeTask = migrateApiKeyQuotaV3(kv, pair.record, migrationNowMs, handoffAlreadyInitialized, progress);
      rt::Promise<MigratedApiKeyPair> upgrade(std::move(upgradeTask));
      auto upgraded = co_await upgrade.wait();
      upgradedApiKeyPairs.push_back(std::move(upgraded));
    }
    const std::string handoffPhase = boundedCounterHandoffPhase(progress.boundedCounters, progress.boundedBaselinesReconciled);
    auto migratePaidTask = migratePendingPaidFallbackLedgers(kv, paidFallbackInventory.candidates);
    rt::Promise<double> migratePaid(std::move(migratePaidTask));
    const double paidFallbackRecords = co_await migratePaid.wait();
    rt::Promise<kv_migration_usage::KernelQuotaV2MigrationResult> kernelRead(kv_migration_usage::migrateKernelQuotaV2(kv, migrationNowMs));
    const auto kernelMigration = co_await kernelRead.wait();
    auto projectTask = kv_migration_projection::projectLegacyPaidFallbackV3(kv, upgradedApiKeyPairs, migrationNowMs);
    rt::Promise<kv_migration_projection::LegacyPaidFallbackProjectionResult> projection(std::move(projectTask));
    co_await projection.wait();

    const KvReadIncidentV2MigrationResult result{.api_keys = progress.apiKeys, .bounded_counters = progress.boundedCounters,
      .handoff_phase = handoffPhase, .bounded_baselines_created = progress.boundedBaselinesCreated,
      .bounded_baselines_reconciled = progress.boundedBaselinesReconciled, .legacy_usage_delta_applied = progress.legacyUsageDeltaApplied,
      .kernel_repo_records = kernelMigration.repo, .kernel_org_records = kernelMigration.org,
      .paid_fallback_records = paidFallbackRecords, .runtime_config_written = true};
    const rt::KvValue marker(json::object{{"version", 2}, {"counter_handoff_version", 1}, {"api_key_quota_v3_handoff_version", 1},
      {"completed_at_ms", rt::dateNow()}, {"api_keys", progress.apiKeys}, {"bounded_counters", progress.boundedCounters},
      {"handoff_phase", handoffPhase}, {"bounded_baselines_created", progress.boundedBaselinesCreated},
      {"bounded_baselines_reconciled", progress.boundedBaselinesReconciled}, {"legacy_usage_delta_applied", progress.legacyUsageDeltaApplied},
      {"kernel_repo_records", kernelMigration.repo}, {"kernel_org_records", kernelMigration.org}, {"paid_fallback_records", paidFallbackRecords}});
    const rt::KvValue storedRuntimeConfig(runtime_config::toJson(runtimeConfig));
    auto finish = kv->atomic();
    finish->set(runtime_config::RUNTIME_CONFIG_V2_KEY, storedRuntimeConfig).set(KV_READ_INCIDENT_V2_MIGRATION_KEY, marker);
    co_await finish->commit();
    co_return result;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KvReadIncidentV2MigrationResult& value) {
    out = Writer().required("api_keys", value.api_keys).required("bounded_counters", value.bounded_counters).required("handoff_phase", value.handoff_phase)
      .required("bounded_baselines_created", value.bounded_baselines_created).required("bounded_baselines_reconciled", value.bounded_baselines_reconciled)
      .required("legacy_usage_delta_applied", value.legacy_usage_delta_applied).required("kernel_repo_records", value.kernel_repo_records)
      .required("kernel_org_records", value.kernel_org_records).required("paid_fallback_records", value.paid_fallback_records)
      .required("runtime_config_written", value.runtime_config_written).done();
  }

  KvReadIncidentV2MigrationResult tag_invoke(const json::value_to_tag<KvReadIncidentV2MigrationResult>&, const json::value& value) {
    const Reader in(value, "KvReadIncidentV2MigrationResult");
    return {.api_keys = in.required<double>("api_keys"), .bounded_counters = in.required<double>("bounded_counters"),
      .handoff_phase = in.required<std::string>("handoff_phase"), .bounded_baselines_created = in.required<double>("bounded_baselines_created"),
      .bounded_baselines_reconciled = in.required<double>("bounded_baselines_reconciled"), .legacy_usage_delta_applied = in.required<double>("legacy_usage_delta_applied"),
      .kernel_repo_records = in.required<double>("kernel_repo_records"), .kernel_org_records = in.required<double>("kernel_org_records"),
      .paid_fallback_records = in.required<double>("paid_fallback_records"), .runtime_config_written = in.required<bool>("runtime_config_written")};
  }
}
