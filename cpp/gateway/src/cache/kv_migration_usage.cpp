#include "uos/cache/kv_migration_usage.hpp"

#include "uos/api_keys.hpp"
#include "uos/cache/kv_migration_paid_fallback.hpp"
#include "uos/defaults.hpp"
#include "uos/kernel/quota_v2.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uos::cache::kv_migration_usage {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
    using api_key_policy::ApiKeyPolicy;
    using ::uos::types::ApiKeyUsageRequestV3;
    using ::uos::types::ApiKeyUsageWindowV3;
    using kv_migration_paid_fallback::isApiKeyHash;
    using kv_migration_paid_fallback::isApiKeyId;
    using kv_migration_paid_fallback::isPositiveSafeInteger;
    using kv_migration_paid_fallback::isSafeUsageCount;
    namespace quota_v2 = kernel::quota_v2;
    namespace contracts = authority::native_kernel_contracts;

    // The helpers below that take references are awaited at once by their callers, whose locals
    // outlive them (the TS passes the same objects).

    const rt::KvGetOptions STRONG{.consistency = rt::KvConsistencyLevel::strong};

    // `key[index]`: nullptr past the end (undefined).
    const rt::KvKeyPart* partAt(const rt::KvKey& key, std::size_t index) { return index < key.size() ? &key[index] : nullptr; }

    // `key[index]` as the stored value JS holds for it (undefined past the end).
    rt::KvValue partValue(const rt::KvKey& key, std::size_t index) { return index < key.size() ? rt::kvValueFromKeyPart(key[index]) : rt::KvValue(); }

    bool isNonEmptyStringPart(const rt::KvKeyPart* part) { return part != nullptr && part->isString() && !part->string().empty(); }

    // `JSON.stringify(key)` (a bigint part throws TypeError, as in JS).
    std::string keyJson(const rt::KvKey& key) { return *rt::kvJsonStringify(rt::kvValueFromKey(key)); }

    bool isUndefinedValue(const rt::KvValue& value) noexcept { return value.isUndefined() || value.is<rt::KvHole>(); }

    // `left === right` on stored values (objects are distinct JS objects).
    bool strictEquals(const rt::KvValue& left, const rt::KvValue& right) {
      if (isUndefinedValue(left)) return isUndefinedValue(right);
      if (left.isNull()) return right.isNull();
      if (left.isBool()) return right.isBool() && left.asBool() == right.asBool();
      if (left.isNumber()) return right.isNumber() && left.asNumber() == right.asNumber();
      if (left.isString()) return right.isString() && left.asString() == right.asString();
      if (left.isBigInt()) return right.isBigInt() && left.asBigInt() == right.asBigInt();
      return false;
    }

    // ToNumber(ToPrimitive(value, number)) for a stored value that is not a bigint.
    double toNumber(const rt::KvValue& value) {
      if (isUndefinedValue(value)) return rt::Number::NaN;
      if (value.isNull()) return 0;
      if (value.isBool()) return value.asBool() ? 1 : 0;
      if (value.isNumber()) return value.asNumber();
      if (value.isString()) return rt::stringToNumber(value.asString());
      if (const auto* date = std::get_if<rt::KvDate>(&value.variant())) return date->time;
      return rt::stringToNumber(rt::kvToString(value));
    }

    // `left < right` with a number on the left (a bigint compares by its value).
    bool numberLessThan(double left, const rt::KvValue& right) { return left < (right.isBigInt() ? right.asBigInt().toNumber() : toNumber(right)); }

    // `value - subtrahend`
    double subtract(const rt::KvValue& value, double subtrahend) {
      if (value.isBigInt()) throw rt::TypeError("Invalid mix of BigInt and other type in subtraction.");
      return toNumber(value) - subtrahend;
    }

    // The first view of a stored row a JSON normalizer reads.
    json::value view(const rt::KvValue& value) { return rt::storedJsonView(value); }

    // The first window reset a migrated V3 aggregate keeps for the post-migration validation pass.
    // The first V3 aggregate for a revoked historical window must remain available
    // long enough for the post-migration validation pass.
    double migrationV3RetentionMs(double windowResetAtMs, double nowMs) {
      return api_key_policy::apiKeyUsageV3RetentionMs(windowResetAtMs, rt::Math::min(nowMs, windowResetAtMs));
    }

    struct BoundedCounterKeys {
      rt::KvKey baselineKey;
      rt::KvKey counterKey;
      rt::KvKey windowKey;
    };

    struct BoundedCounterEntries {
      rt::KvEntryMaybe<> baselineEntry;
      rt::KvEntryMaybe<> counterEntry;
      rt::KvEntryMaybe<> windowEntry;
    };

    /** Seeds the first V2 migration baseline and V3 window for a bounded counter. */
    // `existingWindow` is the normalized stored window; its stored row (entries.windowEntry.value)
    // is what the spread copies.
    rt::Task<std::optional<BoundedCounterHandoffResult>> seedBoundedCounterBaseline(std::shared_ptr<rt::Kv> kv, const ApiKeyPolicy& policy, const BoundedCounterKeys& keys,
                                                                                    const BoundedCounterEntries& entries, double observedV2Usage,
                                                                                    const std::optional<ApiKeyUsageWindowV3>& existingWindow, double nowMs,
                                                                                    bool handoffAlreadyInitialized) {
      const ApiKeyUsageV2MigrationBaseline baseline{.version = 1,
                                                    .key_id = policy.key_id,
                                                    .policy_version = policy.policy_version,
                                                    .window_start_ms = policy.window_start_ms,
                                                    .last_legacy_usage_requests = observedV2Usage,
                                                    .seeded_at_ms = nowMs,
                                                    .reconciled_at_ms = handoffAlreadyInitialized ? std::optional<double>(nowMs) : std::nullopt,
                                                    .reconciliation_runs = handoffAlreadyInitialized ? 1.0 : 0.0};
      // `{ ...(existingWindow ?? makeApiKeyUsageWindowV3(policy, nowMs)), committed_requests, updated_at_ms }`
      const double committedRequests = rt::Math::max(existingWindow ? existingWindow->committed_requests : 0, observedV2Usage);
      rt::KvValue seededWindow;
      double seededWindowResetAtMs = 0;
      if (existingWindow) {
        rt::KvObject window = entries.windowEntry.value.asObject();
        window.set("committed_requests", committedRequests);
        window.set("updated_at_ms", nowMs);
        seededWindowResetAtMs = existingWindow->window_reset_at_ms;
        seededWindow = rt::KvValue(std::move(window));
      } else {
        ApiKeyUsageWindowV3 window = api_key_policy::makeApiKeyUsageWindowV3(policy, nowMs);
        window.committed_requests = committedRequests;
        window.updated_at_ms = nowMs;
        seededWindowResetAtMs = window.window_reset_at_ms;
        seededWindow = rt::KvValue(json::value_from(window));
      }
      const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      atomic->check(entries.baselineEntry)
        .check(entries.counterEntry)
        .check(entries.windowEntry)
        .set(keys.baselineKey, rt::KvValue(json::value_from(baseline)))
        .set(keys.windowKey, seededWindow, rt::KvSetOptions{.expireIn = migrationV3RetentionMs(seededWindowResetAtMs, nowMs)});
      const rt::KvCommitResult committed = co_await atomic->commit();
      if (!committed.ok) co_return std::nullopt;
      co_return BoundedCounterHandoffResult{.baseline_created = true, .baseline_reconciled = handoffAlreadyInitialized, .legacy_usage_delta_applied = observedV2Usage};
    }

    /** Applies one legacy-usage delta to an existing baseline and V3 window. */
    rt::Task<std::optional<BoundedCounterHandoffResult>> updateBoundedCounterBaseline(std::shared_ptr<rt::Kv> kv, const ApiKeyPolicy& policy, const BoundedCounterKeys& keys,
                                                                                      const BoundedCounterEntries& entries, double observedV2Usage,
                                                                                      const std::optional<ApiKeyUsageWindowV3>& existingWindow, double nowMs,
                                                                                      bool handoffAlreadyInitialized) {
      const std::optional<ApiKeyUsageV2MigrationBaseline> baseline = normalizeApiKeyUsageV2MigrationBaseline(entries.baselineEntry.value);
      if (!baseline || baseline->key_id != policy.key_id || baseline->policy_version != policy.policy_version || baseline->window_start_ms != policy.window_start_ms) {
        throw rt::JsError("Bounded counter handoff baseline is invalid: " + policy.key_id);
      }
      if (!existingWindow) throw rt::JsError("V3 API-key window is missing: " + policy.key_id);
      const double nextLegacyUsage = rt::Math::max(baseline->last_legacy_usage_requests, observedV2Usage);
      const double legacyDelta = nextLegacyUsage - baseline->last_legacy_usage_requests;
      // `{ ...baseline, ... }` and `{ ...existingWindow, ... }`: the validated stored rows.
      rt::KvObject updatedBaseline = entries.baselineEntry.value.asObject();
      updatedBaseline.set("last_legacy_usage_requests", nextLegacyUsage);
      updatedBaseline.set("reconciled_at_ms", handoffAlreadyInitialized ? rt::KvValue(nowMs) : rt::KvValue(nullptr));
      updatedBaseline.set("reconciliation_runs", handoffAlreadyInitialized ? baseline->reconciliation_runs + 1 : 0);
      rt::KvObject updatedWindow = entries.windowEntry.value.asObject();
      updatedWindow.set("committed_requests", existingWindow->committed_requests + legacyDelta);
      updatedWindow.set("updated_at_ms", nowMs);
      const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      atomic->check(entries.baselineEntry)
        .check(entries.counterEntry)
        .check(entries.windowEntry)
        .set(keys.baselineKey, rt::KvValue(std::move(updatedBaseline)))
        .set(keys.windowKey, rt::KvValue(std::move(updatedWindow)), rt::KvSetOptions{.expireIn = migrationV3RetentionMs(existingWindow->window_reset_at_ms, nowMs)});
      const rt::KvCommitResult committed = co_await atomic->commit();
      if (!committed.ok) co_return std::nullopt;
      co_return BoundedCounterHandoffResult{.baseline_created = false, .baseline_reconciled = handoffAlreadyInitialized, .legacy_usage_delta_applied = legacyDelta};
    }

    // `entries.counterEntry.value.value` on a present stored value.
    rt::KvValue counterMember(const rt::KvValue& counter) {
      if (isUndefinedValue(counter)) throw rt::TypeError("undefined is not an object (evaluating 'entries.counterEntry.value.value')");
      if (counter.isU64()) return rt::KvValue(counter.asU64().toBigInt());
      return counter["value"];
    }

    rt::Task<BoundedCounterHandoffResult> migrateHandoff(std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy, double legacyUsageRequests, double nowMs,
                                                         bool handoffAlreadyInitialized) {
      const BoundedCounterKeys keys{
        .baselineKey = apiKeyUsageV2MigrationBaselineKey(policy),
        .counterKey = api_key_policy::apiKeyUsageV2Key(policy),
        .windowKey = api_key_policy::apiKeyUsageV3WindowKey(policy),
      };

      for (int attempt = 0; attempt < 5; attempt += 1) {
        std::vector<rt::Promise<rt::KvEntryMaybe<>>> reads;
        reads.push_back(kv->get(keys.baselineKey, STRONG));
        reads.push_back(kv->get(keys.counterKey, STRONG));
        reads.push_back(kv->get(keys.windowKey, STRONG));
        std::vector<rt::KvEntryMaybe<>> read = co_await rt::all(std::move(reads)).wait();
        const BoundedCounterEntries entries{.baselineEntry = std::move(read[0]), .counterEntry = std::move(read[1]), .windowEntry = std::move(read[2])};
        const rt::KvValue& counter = entries.counterEntry.value;
        std::optional<rt::KvValue> counterValue;
        if (!counter.isNull()) {
          counterValue = counterMember(counter);
          if (!counterValue->isBigInt()) throw rt::JsError("Legacy V2 API-key counter is invalid: " + policy.key_id);
        }
        const double observedV2Usage = counterValue ? counterValue->asBigInt().toNumber() : legacyUsageRequests;
        if (!isSafeUsageCount(rt::KvValue(observedV2Usage))) {
          throw rt::JsError("Legacy V2 API-key counter is out of range: " + policy.key_id);
        }
        const std::optional<ApiKeyUsageWindowV3> existingWindow = api_key_policy::normalizeApiKeyUsageWindowV3(view(entries.windowEntry.value));
        if (existingWindow && (existingWindow->key_id != policy.key_id || existingWindow->policy_version != policy.policy_version ||
                               existingWindow->window_start_ms != policy.window_start_ms || existingWindow->window_reset_at_ms != policy.usage_reset_at_ms)) {
          throw rt::JsError("V3 API-key window identity is invalid: " + policy.key_id);
        }
        std::optional<BoundedCounterHandoffResult> handoff;
        if (entries.baselineEntry.value.isNull()) {
          rt::Task<std::optional<BoundedCounterHandoffResult>> seeding =
            seedBoundedCounterBaseline(kv, policy, keys, entries, observedV2Usage, existingWindow, nowMs, handoffAlreadyInitialized);
          handoff = co_await std::move(seeding);
        } else {
          rt::Task<std::optional<BoundedCounterHandoffResult>> updating =
            updateBoundedCounterBaseline(kv, policy, keys, entries, observedV2Usage, existingWindow, nowMs, handoffAlreadyInitialized);
          handoff = co_await std::move(updating);
        }
        if (handoff) co_return *handoff;
      }
      throw rt::JsError("Bounded counter handoff changed concurrently: " + policy.key_id);
    }

    // `LegacyKernelLimitRecord`
    struct LegacyKernelLimitRecord {
      double usage_limit_requests;
      double usage_requests;
      double usage_reset_at_ms;
      double window_ms;
      double expires_at_ms;
      double created_at_ms;
      double updated_at_ms;
    };

    // `{ limit: number; windowMs: number }`
    struct MigrationKernelDefaults {
      double limit;
      double windowMs;
    };

    double legacyKernelNumber(const rt::KvValue& value, double fallback) {
      double number = 0;
      if (value.isString()) number = rt::stringToNumber(value.asString());
      else if (value.isNumber()) number = value.asNumber();
      else return fallback;
      if (!rt::Number::isFinite(number)) return fallback;
      return rt::Math::trunc(number);
    }

    // std::nullopt is null.
    std::optional<LegacyKernelLimitRecord> normalizeLegacyKernelLimit(const rt::KvValue& value, const MigrationKernelDefaults& defaults, double nowMs) {
      if (!rt::kvIsObject(value)) return std::nullopt;
      const double limit = legacyKernelNumber(value["usage_limit_requests"], defaults.limit);
      const double usage = rt::Math::max(0, legacyKernelNumber(value["usage_requests"], 0));
      const double windowMs = legacyKernelNumber(value["window_ms"], defaults.windowMs);
      const double resetAtMs = legacyKernelNumber(value["usage_reset_at_ms"], nowMs + rt::Math::max(1, windowMs));
      const double expiresAtMs = legacyKernelNumber(value["expires_at_ms"], -1);
      if (!(limit == -1 || limit >= 0) || windowMs <= 0 || resetAtMs <= 0 || !(expiresAtMs == -1 || expiresAtMs >= 0)) return std::nullopt;
      return LegacyKernelLimitRecord{
        .usage_limit_requests = limit,
        .usage_requests = usage,
        .usage_reset_at_ms = resetAtMs,
        .window_ms = windowMs,
        .expires_at_ms = expiresAtMs,
        .created_at_ms = rt::Math::max(0, legacyKernelNumber(value["created_at_ms"], nowMs)),
        .updated_at_ms = rt::Math::max(0, legacyKernelNumber(value["updated_at_ms"], nowMs)),
      };
    }

    rt::Task<MigrationKernelDefaults> migrationKernelDefaults(std::shared_ptr<rt::Kv> kv) {
      std::vector<rt::Promise<rt::KvEntryMaybe<>>> reads;
      reads.push_back(kv->get(defaults::DEFAULT_KERNEL_POLICY_LIMIT_KEY, STRONG));
      reads.push_back(kv->get(defaults::DEFAULT_KERNEL_POLICY_WINDOW_KEY, STRONG));
      const std::vector<rt::KvEntryMaybe<>> read = co_await rt::all(std::move(reads)).wait();
      const double limit = legacyKernelNumber(read[0].value, defaults::DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS);
      const double windowMs = legacyKernelNumber(read[1].value, defaults::DEFAULT_KERNEL_POLICY_WINDOW_MS);
      co_return MigrationKernelDefaults{
        .limit = limit == -1 || limit >= 0 ? limit : defaults::DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS,
        .windowMs = windowMs > 0 ? windowMs : defaults::DEFAULT_KERNEL_POLICY_WINDOW_MS,
      };
    }

    // `${owner}/${repo ?? ""}`
    std::string scopeLabel(const std::string& owner, const std::optional<std::string>& repo) { return owner + "/" + repo.value_or(""); }

    /** Legacy entry already mirrored by a V2 policy row: drop the legacy row. */
    rt::Task<void> deleteMigratedLegacyKernelEntry(std::shared_ptr<rt::Kv> kv, const rt::KvEntry<>& legacyEntry, const rt::KvEntryMaybe<>& policyEntry,
                                                   const rt::KvEntryMaybe<>& windowEntry, const std::string& owner, const std::optional<std::string>& repo) {
      const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      atomic->check(legacyEntry).check(policyEntry).check(windowEntry).delete_(legacyEntry.key);
      const rt::KvCommitResult committed = co_await atomic->commit();
      if (!committed.ok) throw rt::JsError("legacy kernel quota changed concurrently: " + scopeLabel(owner, repo));
    }

    // `Readonly<{ scope; owner; repo; effectiveWindowMs; nowMs }>`
    struct MigratedKernelWindowContext {
      contracts::KernelQuotaScope scope;
      std::string owner;
      std::optional<std::string> repo;
      double effectiveWindowMs;
      double nowMs;
    };

    /** Builds the V2 window row for a legacy kernel limit entry. */
    contracts::KernelQuotaWindowV2 migratedKernelWindow(const std::optional<contracts::KernelQuotaWindowV2>& currentWindow, const LegacyKernelLimitRecord& legacy,
                                                       const MigratedKernelWindowContext& context) {
      const bool sameWindow =
        currentWindow && currentWindow->applied_window_ms == context.effectiveWindowMs && currentWindow->usage_reset_at_ms == legacy.usage_reset_at_ms;
      if (sameWindow) {
        contracts::KernelQuotaWindowV2 window = *currentWindow;
        window.usage_requests = rt::Math::max(currentWindow->usage_requests, legacy.usage_requests);
        window.updated_at_ms = context.nowMs;
        return window;
      }
      return contracts::KernelQuotaWindowV2{
        .v = 2,
        .scope = context.scope,
        .owner = context.owner,
        .repo = context.scope == "repo" ? context.repo : std::nullopt,
        .usage_requests = legacy.usage_reset_at_ms <= context.nowMs ? 0 : legacy.usage_requests,
        .reserved_requests = 0,
        .usage_reset_at_ms = legacy.usage_reset_at_ms <= context.nowMs ? context.nowMs + context.effectiveWindowMs : legacy.usage_reset_at_ms,
        .applied_window_ms = context.effectiveWindowMs,
        .created_at_ms = legacy.created_at_ms,
        .updated_at_ms = context.nowMs,
      };
    }

    // `Readonly<{ owner: string; repo: string | undefined }>`
    struct LegacyKernelIdentity {
      std::string owner;
      std::optional<std::string> repo;
    };

    /** Owner/repo identity carried by one legacy kernel quota key. */
    LegacyKernelIdentity legacyKernelIdentity(const rt::KvKey& key, const rt::KvKey& prefix, const contracts::KernelQuotaScope& scope) {
      const rt::KvKeyPart* ownerPart = partAt(key, prefix.size());
      const rt::KvKeyPart* repoPart = partAt(key, prefix.size() + 1);
      if (!isNonEmptyStringPart(ownerPart) || (scope == "repo" && !isNonEmptyStringPart(repoPart))) {
        throw rt::JsError("legacy kernel quota key is malformed: " + keyJson(key));
      }
      return LegacyKernelIdentity{.owner = ownerPart->string(), .repo = scope == "repo" ? std::optional<std::string>(repoPart->string()) : std::nullopt};
    }

    // `Readonly<{ policyKey: KvKey; windowKey: KvKey }>`
    struct KernelV2Keys {
      rt::KvKey policyKey;
      rt::KvKey windowKey;
    };

    /** V2 policy and window keys for one kernel quota scope. */
    KernelV2Keys kernelV2Keys(const contracts::KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo) {
      if (scope == "repo" && repo) return KernelV2Keys{.policyKey = quota_v2::kernelRepoPolicyKey(owner, *repo), .windowKey = quota_v2::kernelRepoWindowKey(owner, *repo)};
      return KernelV2Keys{.policyKey = quota_v2::kernelOrgPolicyKey(owner), .windowKey = quota_v2::kernelOrgWindowKey(owner)};
    }

    // The `context` of writeMigratedKernelPolicy.
    struct WriteMigratedKernelPolicyContext {
      rt::KvEntry<> legacyEntry;
      LegacyKernelLimitRecord legacy;
      KernelV2Keys keys;
      rt::KvEntryMaybe<> policyEntry;
      rt::KvEntryMaybe<> windowEntry;
      LegacyKernelIdentity identity;
      contracts::KernelQuotaScope scope;
      bool defaultBacked;
      MigrationKernelDefaults defaults;
      double nowMs;
    };

    /** Writes the V2 policy and window rows for one legacy kernel limit entry. */
    rt::Task<void> writeMigratedKernelPolicy(std::shared_ptr<rt::Kv> kv, const WriteMigratedKernelPolicyContext& context) {
      const LegacyKernelLimitRecord& legacy = context.legacy;
      const LegacyKernelIdentity& identity = context.identity;
      const double effectiveWindowMs = context.defaultBacked ? context.defaults.windowMs : legacy.window_ms;
      rt::Task<std::optional<contracts::KernelQuotaWindowV2>> reconciling =
        quota_v2::reconcileKernelQuotaWindowReservations(kv, context.windowEntry, context.scope, identity.owner, identity.repo);
      const std::optional<contracts::KernelQuotaWindowV2> currentWindow = co_await std::move(reconciling);
      const MigratedKernelWindowContext windowContext{
        .scope = context.scope, .owner = identity.owner, .repo = identity.repo, .effectiveWindowMs = effectiveWindowMs, .nowMs = context.nowMs};
      const contracts::KernelQuotaWindowV2 window = migratedKernelWindow(currentWindow, legacy, windowContext);
      const contracts::KernelQuotaPolicyV2 policy{
        .v = 2,
        .scope = context.scope,
        .owner = identity.owner,
        .repo = context.scope == "repo" ? identity.repo : std::nullopt,
        .usage_limit_requests = legacy.usage_limit_requests,
        .window_ms = legacy.window_ms,
        .expires_at_ms = legacy.expires_at_ms,
        .created_at_ms = legacy.created_at_ms,
        .updated_at_ms = context.nowMs,
      };
      const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      atomic->check(context.legacyEntry)
        .check(context.policyEntry)
        .check(context.windowEntry)
        .set(context.keys.windowKey, rt::KvValue(json::value_from(window)))
        .delete_(context.legacyEntry.key);
      if (context.defaultBacked) atomic->delete_(context.keys.policyKey);
      else atomic->set(context.keys.policyKey, rt::KvValue(json::value_from(policy)));
      const rt::KvCommitResult committed = co_await atomic->commit();
      if (!committed.ok) throw rt::JsError("legacy kernel quota changed concurrently: " + scopeLabel(identity.owner, identity.repo));
    }

    /** Migrates one legacy kernel limit entry into the V2 policy and window rows. */
    rt::Task<void> migrateLegacyKernelEntry(std::shared_ptr<rt::Kv> kv, const rt::KvEntry<>& legacyEntry, const rt::KvKey& legacyPrefix,
                                            const contracts::KernelQuotaScope& scope, const MigrationKernelDefaults& defaults, double nowMs) {
      const LegacyKernelIdentity identity = legacyKernelIdentity(legacyEntry.key, legacyPrefix, scope);
      const std::optional<LegacyKernelLimitRecord> legacy = normalizeLegacyKernelLimit(legacyEntry.value, defaults, nowMs);
      if (!legacy) throw rt::JsError("legacy kernel quota value is malformed: " + keyJson(legacyEntry.key));
      const bool defaultBacked = legacy->expires_at_ms == -1 && legacy->usage_limit_requests == defaults.limit && legacy->window_ms == defaults.windowMs;
      const KernelV2Keys keys = kernelV2Keys(scope, identity.owner, identity.repo);
      std::vector<rt::Promise<rt::KvEntryMaybe<>>> reads;
      reads.push_back(kv->get(keys.policyKey, STRONG));
      reads.push_back(kv->get(keys.windowKey, STRONG));
      std::vector<rt::KvEntryMaybe<>> read = co_await rt::all(std::move(reads)).wait();
      const rt::KvEntryMaybe<> policyEntry = std::move(read[0]);
      const rt::KvEntryMaybe<> windowEntry = std::move(read[1]);
      if (quota_v2::normalizeKernelQuotaPolicyV2(quota_v2::kernelQuotaValue(policyEntry.value), scope, identity.owner, identity.repo)) {
        co_await deleteMigratedLegacyKernelEntry(kv, legacyEntry, policyEntry, windowEntry, identity.owner, identity.repo);
        co_return;
      }
      const WriteMigratedKernelPolicyContext context{
        .legacyEntry = legacyEntry,
        .legacy = *legacy,
        .keys = keys,
        .policyEntry = policyEntry,
        .windowEntry = windowEntry,
        .identity = identity,
        .scope = scope,
        .defaultBacked = defaultBacked,
        .defaults = defaults,
        .nowMs = nowMs,
      };
      co_await writeMigratedKernelPolicy(kv, context);
    }

    rt::Task<double> migrateLegacyKernelScope(std::shared_ptr<rt::Kv> kv, contracts::KernelQuotaScope scope, MigrationKernelDefaults defaults, double nowMs) {
      const rt::KvKey legacyPrefix = scope == "repo" ? rt::KvKey{"ubq_ai", "kernel_auth", "limits"} : rt::KvKey{"ubq_ai", "kernel_auth", "org_limits"};
      double migrated = 0;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = legacyPrefix});
      while (std::optional<rt::KvEntry<>> legacyEntry = co_await iterator->next()) {
        co_await migrateLegacyKernelEntry(kv, *legacyEntry, legacyPrefix, scope, defaults, nowMs);
        migrated += 1;
      }
      co_return migrated;
    }

    std::string apiKeyUsageV3WindowReference(const std::string& keyId, const std::string& policyVersion, double windowStartMs) {
      json::array reference;
      reference.emplace_back(json::string(keyId));
      reference.emplace_back(json::string(policyVersion));
      reference.push_back(rt::makeNumber(windowStartMs));
      return rt::jsonStringify(reference);
    }

    using ApiKeyUsageV3WindowMap = rt::OrderedMap<std::string, ApiKeyUsageWindowV3>;

    struct ApiKeyUsageV3WindowInventory {
      double count;
      ApiKeyUsageV3WindowMap windows;
    };

    /** Validates every V3 usage window row and indexes it by its reference. */
    rt::Task<ApiKeyUsageV3WindowInventory> collectApiKeyUsageV3Windows(std::shared_ptr<rt::Kv> kv, const rt::OrderedSet<std::string>& knownKeyIds,
                                                                       std::vector<std::string>& errors) {
      ApiKeyUsageV3WindowInventory inventory{.count = 0, .windows = {}};
      const rt::KvKey& prefix = api_key_policy::API_KEY_USAGE_V3_WINDOW_PREFIX;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = prefix});
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        inventory.count += 1;
        const rt::KvValue keyId = partValue(entry->key, prefix.size());
        const rt::KvKeyPart* policyVersion = partAt(entry->key, prefix.size() + 1);
        const rt::KvValue windowStartMs = partValue(entry->key, prefix.size() + 2);
        const std::optional<ApiKeyUsageWindowV3> window = api_key_policy::normalizeApiKeyUsageWindowV3(view(entry->value));
        if (entry->key.size() != prefix.size() + 3 || !isApiKeyId(keyId) || !isNonEmptyStringPart(policyVersion) || !isSafeUsageCount(windowStartMs)) {
          errors.push_back("API key usage V3 window is malformed: " + keyJson(entry->key));
          continue;
        }
        if (!window) {
          errors.push_back("API key usage V3 window is malformed: " + keyJson(entry->key));
          continue;
        }
        if (window->key_id != keyId.asString() || window->policy_version != policyVersion->string() || window->window_start_ms != windowStartMs.asNumber()) {
          errors.push_back("API key usage V3 window is malformed: " + keyJson(entry->key));
          continue;
        }
        if (!knownKeyIds.has(keyId.asString())) errors.push_back("API key usage V3 window is orphaned: " + keyId.asString());
        inventory.windows.set(apiKeyUsageV3WindowReference(keyId.asString(), policyVersion->string(), windowStartMs.asNumber()), *window);
      }
      co_return inventory;
    }

    /** Reports a V3 usage request row whose state fields are incomplete. */
    void pushApiKeyUsageRequestStateErrors(const ApiKeyUsageRequestV3& request, std::vector<std::string>& errors) {
      if (request.state == "dispatched" && (!request.provider || !request.dispatched_at_ms)) {
        errors.push_back("API key usage V3 dispatched request is incomplete: " + request.key_id + "/" + request.request_id);
      }
      if (request.state == "released" && !request.released_at_ms) {
        errors.push_back("API key usage V3 released request is incomplete: " + request.key_id + "/" + request.request_id);
      }
    }

    struct ApiKeyUsageV3RequestInventory {
      double count;
      rt::OrderedMap<std::string, double> reservedByWindow;
      rt::OrderedSet<std::string> requestWindows;
    };

    /** Validates every V3 usage request row and tallies reservations per window. */
    rt::Task<ApiKeyUsageV3RequestInventory> collectApiKeyUsageV3Requests(std::shared_ptr<rt::Kv> kv, const rt::OrderedSet<std::string>& knownKeyIds,
                                                                         std::vector<std::string>& errors) {
      ApiKeyUsageV3RequestInventory inventory{.count = 0, .reservedByWindow = {}, .requestWindows = {}};
      const rt::KvKey& prefix = api_key_policy::API_KEY_USAGE_V3_REQUEST_PREFIX;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = prefix});
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        inventory.count += 1;
        const rt::KvValue keyId = partValue(entry->key, prefix.size());
        const rt::KvKeyPart* policyVersion = partAt(entry->key, prefix.size() + 1);
        const rt::KvValue windowStartMs = partValue(entry->key, prefix.size() + 2);
        const rt::KvValue requestId = partValue(entry->key, prefix.size() + 3);
        const std::optional<ApiKeyUsageRequestV3> request = api_key_policy::normalizeApiKeyUsageRequestV3(view(entry->value));
        if (entry->key.size() != prefix.size() + 4 || !isApiKeyId(keyId) || !isNonEmptyStringPart(policyVersion) || !isSafeUsageCount(windowStartMs) ||
            !isApiKeyId(requestId)) {
          errors.push_back("API key usage V3 request is malformed: " + keyJson(entry->key));
          continue;
        }
        if (!request) {
          errors.push_back("API key usage V3 request is malformed: " + keyJson(entry->key));
          continue;
        }
        if (request->key_id != keyId.asString() || request->request_id != requestId.asString()) {
          errors.push_back("API key usage V3 request is malformed: " + keyJson(entry->key));
          continue;
        }
        if (!knownKeyIds.has(keyId.asString())) errors.push_back("API key usage V3 request is orphaned: " + keyId.asString() + "/" + requestId.asString());
        const std::string reference = apiKeyUsageV3WindowReference(keyId.asString(), policyVersion->string(), windowStartMs.asNumber());
        inventory.requestWindows.add(reference);
        if (request->state == "reserved") {
          const double* reserved = inventory.reservedByWindow.get(reference);
          inventory.reservedByWindow.set(reference, (reserved != nullptr ? *reserved : 0) + 1);
        }
        pushApiKeyUsageRequestStateErrors(*request, errors);
      }
      co_return inventory;
    }

    // `KernelQuotaWindowEntry = Readonly<{ window: KernelQuotaWindowV2; hasReservationAggregate: boolean }>`
    struct KernelQuotaWindowEntry {
      contracts::KernelQuotaWindowV2 window;
      bool hasReservationAggregate;
    };

    std::string kernelWindowReference(const contracts::KernelQuotaScope& scope, const std::string& owner, const std::optional<std::string>& repo, double createdAtMs) {
      json::array reference;
      reference.emplace_back(json::string(scope));
      reference.emplace_back(json::string(owner));
      // `JSON.stringify` writes an undefined array element as null.
      if (scope == "repo") reference.push_back(repo ? json::value(json::string(*repo)) : json::value(nullptr));
      reference.push_back(rt::makeNumber(createdAtMs));
      return rt::jsonStringify(reference);
    }

    // What the six concurrent scans of inspectKernelQuotaV2 share (JS closes over the four
    // collections; a scan still running after another one failed keeps them alive).
    struct KernelQuotaScanState {
      std::vector<std::string> errors;
      rt::OrderedMap<std::string, KernelQuotaWindowEntry> windows;
      rt::OrderedMap<std::string, double> reservedByWindow;
      rt::OrderedMap<std::string, double> resetAtByWindow;
    };

    // `validKey` of the policy and window scans.
    bool kernelScopeKeyIsValid(const rt::KvKey& key, const rt::KvKey& prefix, const contracts::KernelQuotaScope& scope) {
      const rt::KvKeyPart* owner = partAt(key, prefix.size());
      const rt::KvKeyPart* repo = partAt(key, prefix.size() + 1);
      return isNonEmptyStringPart(owner) && (scope == "org" ? key.size() == prefix.size() + 1 : isNonEmptyStringPart(repo) && key.size() == prefix.size() + 2);
    }

    /** Validates every V2 kernel policy row under one prefix and counts them. */
    rt::Task<double> inspectKernelQuotaPolicies(std::shared_ptr<rt::Kv> kv, rt::KvKey prefix, contracts::KernelQuotaScope scope, std::shared_ptr<KernelQuotaScanState> state) {
      double count = 0;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = prefix});
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        count += 1;
        const bool validKey = kernelScopeKeyIsValid(entry->key, prefix, scope);
        const bool validValue = validKey && quota_v2::normalizeKernelQuotaPolicyV2(quota_v2::kernelQuotaValue(entry->value), scope, entry->key[prefix.size()].string(),
                                                                                   scope == "repo" ? std::optional<std::string>(entry->key[prefix.size() + 1].string())
                                                                                                   : std::nullopt);
        if (!validValue) state->errors.push_back("kernel quota V2 policy is malformed: " + keyJson(entry->key));
      }
      co_return count;
    }

    /** Validates every V2 kernel window row under one prefix and indexes it. */
    rt::Task<double> inspectKernelQuotaWindows(std::shared_ptr<rt::Kv> kv, rt::KvKey prefix, contracts::KernelQuotaScope scope, std::shared_ptr<KernelQuotaScanState> state) {
      double count = 0;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = prefix});
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        count += 1;
        const bool validKey = kernelScopeKeyIsValid(entry->key, prefix, scope);
        const std::optional<std::string> scopeRepo = scope == "repo" && validKey ? std::optional<std::string>(entry->key[prefix.size() + 1].string()) : std::nullopt;
        const std::optional<contracts::KernelQuotaWindowV2> window =
          validKey ? quota_v2::normalizeKernelQuotaWindowV2(quota_v2::kernelQuotaValue(entry->value), scope, entry->key[prefix.size()].string(), scopeRepo) : std::nullopt;
        if (!window) {
          state->errors.push_back("kernel quota V2 window is malformed: " + keyJson(entry->key));
          continue;
        }
        state->windows.set(kernelWindowReference(scope, window->owner, window->repo, window->created_at_ms),
                           KernelQuotaWindowEntry{.window = *window,
                                                  .hasReservationAggregate = rt::kvIsObject(entry->value) && entry->value.isObject() &&
                                                                             entry->value.asObject().contains("reserved_requests")});
      }
      co_return count;
    }

    // `Readonly<{ owner; repo; windowCreatedAtMs; requestId }>`
    struct KernelReservationKeyIdentity {
      std::string owner;
      std::optional<std::string> repo;
      double windowCreatedAtMs;
      std::string requestId;
    };

    /** Owner, timestamps and request id carried by one V2 reservation key. */
    // std::nullopt is null.
    std::optional<KernelReservationKeyIdentity> kernelReservationKeyIdentity(const rt::KvKey& key, const rt::KvKey& prefix, const contracts::KernelQuotaScope& scope) {
      const bool repoScope = scope == "repo";
      const rt::KvKeyPart* owner = partAt(key, prefix.size());
      const rt::KvKeyPart* repo = repoScope ? partAt(key, prefix.size() + 1) : nullptr;
      const rt::KvKeyPart* windowCreatedAtMs = partAt(key, prefix.size() + (repoScope ? 2 : 1));
      const rt::KvKeyPart* requestId = partAt(key, prefix.size() + (repoScope ? 3 : 2));
      const bool validKey = isNonEmptyStringPart(owner) && (scope == "org" || isNonEmptyStringPart(repo)) && windowCreatedAtMs != nullptr &&
                            windowCreatedAtMs->isNumber() && rt::Number::isSafeInteger(windowCreatedAtMs->number()) && windowCreatedAtMs->number() >= 0 &&
                            isNonEmptyStringPart(requestId) && key.size() == prefix.size() + (repoScope ? 4 : 3);
      if (!validKey) return std::nullopt;
      return KernelReservationKeyIdentity{.owner = owner->string(),
                                          .repo = repo != nullptr && repo->isString() ? std::optional<std::string>(repo->string()) : std::nullopt,
                                          .windowCreatedAtMs = windowCreatedAtMs->number(),
                                          .requestId = requestId->string()};
    }

    /** Validates every V2 kernel reservation row under one prefix and tallies it. */
    rt::Task<double> inspectKernelQuotaReservations(std::shared_ptr<rt::Kv> kv, rt::KvKey prefix, contracts::KernelQuotaScope scope,
                                                    std::shared_ptr<KernelQuotaScanState> state) {
      double count = 0;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = prefix});
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        count += 1;
        const std::optional<KernelReservationKeyIdentity> identity = kernelReservationKeyIdentity(entry->key, prefix, scope);
        if (!identity) {
          state->errors.push_back("kernel quota V2 reservation is malformed: " + keyJson(entry->key));
          continue;
        }
        const std::optional<contracts::KernelQuotaReservationRowV2> reservation =
          quota_v2::normalizeKernelQuotaReservationRowV2(quota_v2::kernelQuotaValue(entry->value), scope, identity->owner, identity->repo);
        if (!reservation) {
          state->errors.push_back("kernel quota V2 reservation is malformed: " + keyJson(entry->key));
          continue;
        }
        if (reservation->request_id != identity->requestId || reservation->window_created_at_ms != identity->windowCreatedAtMs) {
          state->errors.push_back("kernel quota V2 reservation is malformed: " + keyJson(entry->key));
          continue;
        }
        const std::string reference = kernelWindowReference(scope, reservation->owner, reservation->repo, reservation->window_created_at_ms);
        const double* knownResetAtMs = state->resetAtByWindow.get(reference);
        if (knownResetAtMs != nullptr && *knownResetAtMs != reservation->window_reset_at_ms) {
          state->errors.push_back("kernel quota V2 reservation window identity is inconsistent: " + reference);
        } else {
          state->resetAtByWindow.set(reference, reservation->window_reset_at_ms);
        }
        if (reservation->state == "reserved") {
          const double* reserved = state->reservedByWindow.get(reference);
          state->reservedByWindow.set(reference, (reserved != nullptr ? *reserved : 0) + 1);
        }
      }
      co_return count;
    }

    /** Validates every api-key hash policy row and indexes it by its hash. */
    rt::Task<rt::OrderedMap<std::string, rt::KvValue>> collectStrictApiKeyHashEntries(std::shared_ptr<rt::Kv> kv, std::vector<std::string>& errors) {
      rt::OrderedMap<std::string, rt::KvValue> hashEntries;
      const rt::KvKey& prefix = api_keys::API_KEY_HASH_PREFIX;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = prefix});
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        const rt::KvValue hash = entry->key.size() == prefix.size() + 1 ? rt::kvValueFromKeyPart(entry->key.back()) : rt::KvValue(nullptr);
        if (!isApiKeyHash(hash)) {
          errors.push_back("api key hash entry has an invalid key: " + keyJson(entry->key));
          continue;
        }
        if (!kv_migration_paid_fallback::hasStrictApiKeyHashCorePolicy(entry->value)) {
          errors.push_back("api key hash policy has invalid core fields: " + hash.asString());
          continue;
        }
        hashEntries.set(hash.asString(), entry->value);
      }
      co_return hashEntries;
    }

    struct StrictApiKeyPairCollection {
      std::vector<StrictApiKeyPair> pairs;
      rt::OrderedMap<std::string, double> referencedHashes;
    };

    /** Pairs every api-key id row with its consistent hash policy row. */
    rt::Task<StrictApiKeyPairCollection> collectStrictApiKeyPairs(std::shared_ptr<rt::Kv> kv, const rt::OrderedMap<std::string, rt::KvValue>& hashEntries,
                                                                  std::vector<std::string>& errors) {
      StrictApiKeyPairCollection collection{.pairs = {}, .referencedHashes = {}};
      const rt::KvKey& prefix = api_keys::API_KEY_ID_PREFIX;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = prefix});
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        const rt::KvValue keyId = entry->key.size() == prefix.size() + 1 ? rt::kvValueFromKeyPart(entry->key.back()) : rt::KvValue(nullptr);
        if (!isApiKeyId(keyId)) {
          errors.push_back("api key id entry has an invalid key: " + keyJson(entry->key));
          continue;
        }
        if (!kv_migration_paid_fallback::hasStrictApiKeyCorePolicy(entry->value)) {
          errors.push_back("api key has invalid v2 policy: " + keyId.asString());
          continue;
        }
        const rt::KvValue& record = entry->value;
        if (!strictEquals(record["id"], keyId)) {
          errors.push_back("api key id key suffix does not match record id: key=" + keyId.asString() + " record=" + record["id"].asString());
          continue;
        }
        const std::string& hash = record["hash"].asString();
        const rt::KvValue* hashRecord = hashEntries.get(hash);
        if (hashRecord == nullptr || !kv_migration_paid_fallback::apiKeyHashPolicyMatches(record, *hashRecord)) {
          errors.push_back("api key hash policy is missing or inconsistent: " + record["id"].asString());
          continue;
        }
        const double* references = collection.referencedHashes.get(hash);
        collection.referencedHashes.set(hash, (references != nullptr ? *references : 0) + 1);
        collection.pairs.push_back(StrictApiKeyPair{.record = record, .hashRecord = *hashRecord});
      }
      co_return collection;
    }

    /** Reports hash policy rows with no (or several) referencing id records. */
    void auditReferencedApiKeyHashes(const rt::OrderedMap<std::string, rt::KvValue>& hashEntries, const rt::OrderedMap<std::string, double>& referencedHashes,
                                     std::vector<std::string>& errors) {
      for (const std::string& hash : hashEntries.keys()) {
        const double* counted = referencedHashes.get(hash);
        const double references = counted != nullptr ? *counted : 0;
        if (references == 0) errors.push_back("api key hash policy is orphaned: " + hash);
        if (references > 1) errors.push_back("api key hash policy has multiple id records: " + hash);
      }
    }
  }

  rt::KvKey apiKeyUsageV2MigrationBaselineKey(const ApiKeyPolicy& policy) {
    return rt::KvKey{kv_migration_paid_fallback::API_KEY_USAGE_V2_MIGRATION_BASELINE_PREFIX, policy.key_id, policy.policy_version, policy.window_start_ms};
  }

  std::optional<ApiKeyUsageV2MigrationBaseline> normalizeApiKeyUsageV2MigrationBaseline(const rt::KvValue& value) {
    if (!value.truthy() || rt::kvTypeof(value) != "object") return std::nullopt;
    const rt::KvValue& record = value;
    const rt::KvValue& version = record["version"];
    const rt::KvValue& policyVersion = record["policy_version"];
    const rt::KvValue& reconciledAtMs = record["reconciled_at_ms"];
    if (!(version.isNumber() && version.asNumber() == 1) || !isApiKeyId(record["key_id"]) || !policyVersion.isString() || policyVersion.asString().empty() ||
        !isSafeUsageCount(record["window_start_ms"]) || !isSafeUsageCount(record["last_legacy_usage_requests"]) || !isPositiveSafeInteger(record["seeded_at_ms"]) ||
        !(reconciledAtMs.isNull() || isPositiveSafeInteger(reconciledAtMs)) || !isSafeUsageCount(record["reconciliation_runs"])) {
      return std::nullopt;
    }
    return ApiKeyUsageV2MigrationBaseline{
      .version = 1,
      .key_id = record["key_id"].asString(),
      .policy_version = policyVersion.asString(),
      .window_start_ms = record["window_start_ms"].asNumber(),
      .last_legacy_usage_requests = record["last_legacy_usage_requests"].asNumber(),
      .seeded_at_ms = record["seeded_at_ms"].asNumber(),
      .reconciled_at_ms = reconciledAtMs.isNull() ? std::nullopt : std::optional<double>(reconciledAtMs.asNumber()),
      .reconciliation_runs = record["reconciliation_runs"].asNumber(),
    };
  }

  double currentLegacyUsage(const rt::KvValue& record, double nowMs) {
    if (!numberLessThan(nowMs, record["usage_reset_at_ms"])) return 0;
    const rt::KvValue& usage = record["usage_requests"];
    return usage.isNumber() ? usage.asNumber() : rt::Number::NaN;
  }

  double migrationPolicyNow(const rt::KvValue& record, double nowMs) {
    if (record["revoked_at_ms"].isNull()) return nowMs;
    return rt::Math::max(0, subtract(record["usage_reset_at_ms"], 1));
  }

  rt::Task<BoundedCounterHandoffResult> migrateBoundedCounterHandoff(std::shared_ptr<rt::Kv> kv, const ApiKeyPolicy& policy, double legacyUsageRequests, double nowMs,
                                                                     bool handoffAlreadyInitialized) {
    return migrateHandoff(std::move(kv), policy, legacyUsageRequests, nowMs, handoffAlreadyInitialized);
  }

  rt::Task<double> countReconciledBoundedCounterBaselines(std::shared_ptr<rt::Kv> kv) {
    double count = 0;
    const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = kv_migration_paid_fallback::API_KEY_USAGE_V2_MIGRATION_BASELINE_PREFIX});
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      const std::optional<ApiKeyUsageV2MigrationBaseline> baseline = normalizeApiKeyUsageV2MigrationBaseline(entry->value);
      if (baseline && baseline->reconciled_at_ms) count += 1;
    }
    co_return count;
  }

  rt::Task<KernelQuotaV2MigrationResult> migrateKernelQuotaV2(std::shared_ptr<rt::Kv> kv, double nowMs) {
    const MigrationKernelDefaults defaults = co_await migrationKernelDefaults(kv);
    rt::Task<double> migratingRepo = migrateLegacyKernelScope(kv, "repo", defaults, nowMs);
    const double repo = co_await std::move(migratingRepo);
    rt::Task<double> migratingOrg = migrateLegacyKernelScope(kv, "org", defaults, nowMs);
    const double org = co_await std::move(migratingOrg);
    co_return KernelQuotaV2MigrationResult{.repo = repo, .org = org};
  }

  rt::Task<ApiKeyUsageV3Inventory> inspectApiKeyUsageV3(std::shared_ptr<rt::Kv> kv, rt::OrderedSet<std::string> knownKeyIds) {
    std::vector<std::string> errors;
    rt::Task<ApiKeyUsageV3WindowInventory> collectingWindows = collectApiKeyUsageV3Windows(kv, knownKeyIds, errors);
    const ApiKeyUsageV3WindowInventory windowInventory = co_await std::move(collectingWindows);
    rt::Task<ApiKeyUsageV3RequestInventory> collectingRequests = collectApiKeyUsageV3Requests(kv, knownKeyIds, errors);
    const ApiKeyUsageV3RequestInventory requestInventory = co_await std::move(collectingRequests);

    for (const auto& [reference, window] : windowInventory.windows) {
      const double* counted = requestInventory.reservedByWindow.get(reference);
      const double reserved = counted != nullptr ? *counted : 0;
      if (window.reserved_requests != reserved) {
        errors.push_back("API key usage V3 reserved aggregate is inconsistent: " + reference);
      }
    }
    for (const std::string& reference : requestInventory.requestWindows) {
      if (!windowInventory.windows.has(reference)) errors.push_back("API key usage V3 request has no window: " + reference);
    }
    co_return ApiKeyUsageV3Inventory{.windows = windowInventory.count, .requests = requestInventory.count, .errors = std::move(errors)};
  }

  rt::Task<KernelQuotaV2Inventory> inspectKernelQuotaV2(std::shared_ptr<rt::Kv> kv) {
    const auto state = std::make_shared<KernelQuotaScanState>();
    std::vector<rt::Promise<double>> scans;
    scans.push_back(inspectKernelQuotaPolicies(kv, quota_v2::KERNEL_REPO_POLICY_V2_PREFIX, "repo", state));
    scans.push_back(inspectKernelQuotaPolicies(kv, quota_v2::KERNEL_ORG_POLICY_V2_PREFIX, "org", state));
    scans.push_back(inspectKernelQuotaWindows(kv, quota_v2::KERNEL_REPO_WINDOW_V2_PREFIX, "repo", state));
    scans.push_back(inspectKernelQuotaWindows(kv, quota_v2::KERNEL_ORG_WINDOW_V2_PREFIX, "org", state));
    scans.push_back(inspectKernelQuotaReservations(kv, quota_v2::KERNEL_REPO_RESERVATION_V2_PREFIX, "repo", state));
    scans.push_back(inspectKernelQuotaReservations(kv, quota_v2::KERNEL_ORG_RESERVATION_V2_PREFIX, "org", state));
    const std::vector<double> counts = co_await rt::all(std::move(scans)).wait();
    for (const auto& [reference, scanned] : state->windows) {
      const double* reservationResetAtMs = state->resetAtByWindow.get(reference);
      if (reservationResetAtMs != nullptr && *reservationResetAtMs != scanned.window.usage_reset_at_ms) {
        state->errors.push_back("kernel quota V2 reservation window reset is inconsistent: " + reference);
      }
      const double* reserved = state->reservedByWindow.get(reference);
      if (scanned.hasReservationAggregate && scanned.window.reserved_requests != (reserved != nullptr ? *reserved : 0)) {
        state->errors.push_back("kernel quota V2 reserved aggregate is inconsistent: " + reference);
      }
    }
    for (const std::string& reference : state->reservedByWindow.keys()) {
      if (!state->windows.has(reference)) {
        state->errors.push_back("kernel quota V2 active reservation has no window: " + reference);
      }
    }
    co_return KernelQuotaV2Inventory{.repoPolicies = counts[0],
                                     .orgPolicies = counts[1],
                                     .repoWindows = counts[2],
                                     .orgWindows = counts[3],
                                     .repoReservations = counts[4],
                                     .orgReservations = counts[5],
                                     .errors = state->errors};
  }

  rt::Task<StrictApiKeyPairsInventory> inspectStrictApiKeyPairs(std::shared_ptr<rt::Kv> kv) {
    std::vector<std::string> errors;
    rt::Task<rt::OrderedMap<std::string, rt::KvValue>> collectingHashes = collectStrictApiKeyHashEntries(kv, errors);
    const rt::OrderedMap<std::string, rt::KvValue> hashEntries = co_await std::move(collectingHashes);
    rt::Task<StrictApiKeyPairCollection> collectingPairs = collectStrictApiKeyPairs(kv, hashEntries, errors);
    StrictApiKeyPairCollection collection = co_await std::move(collectingPairs);
    auditReferencedApiKeyHashes(hashEntries, collection.referencedHashes, errors);
    co_return StrictApiKeyPairsInventory{.pairs = std::move(collection.pairs), .errors = std::move(errors)};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyUsageV2MigrationBaseline& value) {
    out = Writer()
            .required("version", value.version)
            .required("key_id", value.key_id)
            .required("policy_version", value.policy_version)
            .required("window_start_ms", value.window_start_ms)
            .required("last_legacy_usage_requests", value.last_legacy_usage_requests)
            .required("seeded_at_ms", value.seeded_at_ms)
            .orNull("reconciled_at_ms", value.reconciled_at_ms)
            .required("reconciliation_runs", value.reconciliation_runs)
            .done();
  }

  ApiKeyUsageV2MigrationBaseline tag_invoke(const json::value_to_tag<ApiKeyUsageV2MigrationBaseline>&, const json::value& value) {
    const Reader in(value, "ApiKeyUsageV2MigrationBaseline");
    return ApiKeyUsageV2MigrationBaseline{
      .version = in.required<double>("version"),
      .key_id = in.required<std::string>("key_id"),
      .policy_version = in.required<std::string>("policy_version"),
      .window_start_ms = in.required<double>("window_start_ms"),
      .last_legacy_usage_requests = in.required<double>("last_legacy_usage_requests"),
      .seeded_at_ms = in.required<double>("seeded_at_ms"),
      .reconciled_at_ms = in.orNull<double>("reconciled_at_ms"),
      .reconciliation_runs = in.required<double>("reconciliation_runs"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const BoundedCounterHandoffResult& value) {
    out = Writer()
            .required("baseline_created", value.baseline_created)
            .required("baseline_reconciled", value.baseline_reconciled)
            .required("legacy_usage_delta_applied", value.legacy_usage_delta_applied)
            .done();
  }

  BoundedCounterHandoffResult tag_invoke(const json::value_to_tag<BoundedCounterHandoffResult>&, const json::value& value) {
    const Reader in(value, "BoundedCounterHandoffResult");
    return BoundedCounterHandoffResult{.baseline_created = in.required<bool>("baseline_created"),
                                       .baseline_reconciled = in.required<bool>("baseline_reconciled"),
                                       .legacy_usage_delta_applied = in.required<double>("legacy_usage_delta_applied")};
  }
}
