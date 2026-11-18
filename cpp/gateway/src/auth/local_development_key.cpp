#include "uos/auth/local_development_key.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/analytics.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/api_keys.hpp"
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

namespace uos::auth::local_development_key {
  namespace {
    constexpr rt::StoredJsonViewOptions STORED_JSON_VIEW_OPTIONS{
      .undefinedMember = rt::StoredJsonViewOptions::UndefinedView::omit,
      .undefinedElement = rt::StoredJsonViewOptions::UndefinedView::null,
      .hole = rt::StoredJsonViewOptions::UndefinedView::null,
      .topLevelUndefined = rt::StoredJsonViewOptions::UndefinedView::null,
      .bigint = rt::StoredJsonViewOptions::BigIntView::emptyObject,
      .kvU64Members = false,
      .typedArrayMembers = false,
    };
    using rt::record::Reader;
    using rt::record::Writer;
    namespace contracts = authority::native_paid_contracts;
    using authority::native_contracts::ApiKeyPolicy;
    using ::uos::api_keys::API_KEY_NO_EXPIRATION_MS;
    using ::uos::api_keys::API_KEY_NO_USAGE_LIMIT;
    using ::uos::api_keys::PAID_FALLBACK_NO_LIMIT;
    using ::uos::api_keys::USAGE_RESET_PERIOD_MS;
    using ::uos::types::ApiKeyHashRecord;
    using ::uos::types::ApiKeyRecord;

    // `{ consistency: "strong" }`
    rt::KvGetOptions strongRead() { return rt::KvGetOptions{.consistency = rt::KvConsistencyLevel::strong}; }

    // A stored KV value as the TS reads it through property access: what JSON can hold is kept, an
    // undefined member is absent, an undefined or hole element is null, and every other part (bigint,
    // KvU64, Date, Map, buffers, ...) becomes `{}`, an object no number/string/boolean/null check accepts.

    // `entry.value` of a read: null for an absent row.

    // `value?.revoked_at_ms !== null`
    bool revokedOrMissing(const json::value& value) {
      const json::value* revokedAtMs = rt::get(value, "revoked_at_ms");
      return revokedAtMs == nullptr || !revokedAtMs->is_null();
    }

    bool isCompletedLocalDevelopmentDeletion(const json::value& value) {
      const std::optional<double> createdAtMs = rt::getNumber(rt::get(value, "created_at_ms"));
      if (!utils::isRecord(value) || !createdAtMs || !rt::Number::isSafeInteger(*createdAtMs) || *createdAtMs <= 0 || !utils::isRecord(rt::get(value, "local_deletion"))) {
        return false;
      }
      const json::value* localDeletion = rt::get(value, "local_deletion");
      const std::optional<std::string_view> owner = rt::getString(rt::get(localDeletion, "owner"));
      const std::optional<double> completedAtMs = rt::getNumber(rt::get(localDeletion, "completed_at_ms"));
      return owner && rt::length(*owner) > 0 && completedAtMs && rt::Number::isSafeInteger(*completedAtMs) && *completedAtMs > 0;
    }

    // `let localDevelopmentPricingDeadlineMs = LOCAL_DEVELOPMENT_PRICING_DEADLINE_MS`
    double localDevelopmentPricingDeadlineMs = LOCAL_DEVELOPMENT_PRICING_DEADLINE_MS;

    struct LocalDevelopmentKeyMaterial {
      ApiKeyRecord record;
      rt::KvKey hashKey;
      ApiKeyHashRecord hashRecord;
      ApiKeyPolicy policy;
    };

    /** The reason carried by a cancelled or expired signal, or a generic AbortError. */
    rt::Reason abortReason(const rt::AbortSignal& signal) {
      if (rt::isError(signal.reason())) return signal.reason();
      return rt::makeReason(rt::DOMException("The request was aborted.", "AbortError"));
    }

    /**
     * Settles as soon as `operation` does or `signal` aborts, whichever happens
     * first. The operation always stays observed by the race, so a late rejection
     * cannot surface as an unhandled rejection and a late result can never be
     * published by a caller that already moved on.
     */
    template <class T> rt::Task<T> awaitWithAbort(rt::Promise<T> operation, rt::AbortSignal signal) {
      rt::Resolvers<T> aborted = rt::withResolvers<T>();
      const rt::Reject reject = aborted.reject;
      const rt::EventListener onAbort = [reject, signal] { reject(abortReason(signal)); };
      signal.addEventListener("abort", onAbort, rt::AddEventListenerOptions{.once = true});
      std::optional<T> result;
      rt::Reason failure;
      bool observed = false;
      try {
        if (signal.aborted()) onAbort();
        std::vector<rt::Promise<T>> racers;
        racers.push_back(operation);
        racers.push_back(aborted.promise);
        const rt::Promise<T> racing = rt::race(std::move(racers));
        observed = true;
        result = co_await racing.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      signal.removeEventListener("abort", onAbort);
      if (observed) reject();
      if (failure) std::rethrow_exception(failure);
      co_return std::move(*result);
    }

    rt::Task<LocalDevelopmentKeyMaterial> buildLocalDevelopmentKey(double nowMs, LocalDevelopmentPricingInitializer initializePolicy, std::optional<rt::AbortSignal> signal) {
      // Reuse the operator-facing pricing initializer so the local key's paid
      // policy is byte-for-byte the policy an unlimited admin key would receive.
      paid_fallback::index::PaidFallbackPolicyFields policyFields = paid_fallback::index::defaultPaidFallbackPolicy();
      const rt::Promise<paid_fallback::index::InitializePaidFallbackPolicyResult> initializing(initializePolicy(std::move(signal)));
      const paid_fallback::index::InitializePaidFallbackPolicyResult pricing = co_await initializing.wait();
      policyFields.paid_fallback_model_ids = pricing.paid_fallback_model_ids;
      policyFields.paid_fallback_quota_per_credit = pricing.paid_fallback_quota_per_credit;
      if (pricing.paid_fallback_max_exposure_microcredits) policyFields.paid_fallback_max_exposure_microcredits = pricing.paid_fallback_max_exposure_microcredits;
      policyFields.paid_fallback_pricing_checked_at_ms = pricing.paid_fallback_pricing_checked_at_ms;
      policyFields.paid_fallback_enabled = true;
      policyFields.paid_fallback_limit_microcredits = PAID_FALLBACK_NO_LIMIT;
      // Minted per provisioning and never exposed: the record's only purpose is to
      // be the principal local requests are admitted as.
      const std::string token = api_keys::generateApiKeyToken();
      const rt::Promise<std::string> hashing(utils::sha256Base64Url(token));
      const std::string hash = co_await hashing.wait();
      const double usageResetAtMs = api_keys::calculateNextResetMs(nowMs, USAGE_RESET_PERIOD_MS);
      ApiKeyRecord record{
        .id = std::string(LOCAL_DEVELOPMENT_KEY_ID),
        .name = std::string(LOCAL_DEVELOPMENT_KEY_NAME),
        .prefix = rt::slice(token, 0, 12),
        .hash = hash,
        .created_at_ms = nowMs,
        .expires_at_ms = API_KEY_NO_EXPIRATION_MS,
        .revoked_at_ms = std::nullopt,
        .usage_limit_requests = API_KEY_NO_USAGE_LIMIT,
        .usage_requests = 0,
        .usage_reset_at_ms = usageResetAtMs,
        .window_ms = USAGE_RESET_PERIOD_MS,
        .usage_quota_version = 3,
        .paid_fallback_enabled = policyFields.paid_fallback_enabled,
        .paid_fallback_limit_microcredits = policyFields.paid_fallback_limit_microcredits,
        .paid_fallback_spent_microcredits = policyFields.paid_fallback_spent_microcredits,
        .paid_fallback_reserved_microcredits = policyFields.paid_fallback_reserved_microcredits,
        .paid_fallback_reservation_request_id = policyFields.paid_fallback_reservation_request_id,
        .paid_fallback_model_ids = policyFields.paid_fallback_model_ids,
        .paid_fallback_quota_per_credit = policyFields.paid_fallback_quota_per_credit,
        .paid_fallback_max_exposure_microcredits = policyFields.paid_fallback_max_exposure_microcredits,
        .paid_fallback_pricing_checked_at_ms = policyFields.paid_fallback_pricing_checked_at_ms,
      };
      const authority::native_paid_policy::PaidFallbackHashFields paid = paid_fallback::index::paidFallbackHashFields(record);
      ApiKeyHashRecord hashRecord{
        .id = std::string(LOCAL_DEVELOPMENT_KEY_ID),
        .expires_at_ms = API_KEY_NO_EXPIRATION_MS,
        .revoked_at_ms = std::nullopt,
        .usage_limit_requests = API_KEY_NO_USAGE_LIMIT,
        .usage_requests = 0,
        .usage_reset_at_ms = usageResetAtMs,
        .window_ms = USAGE_RESET_PERIOD_MS,
        .usage_quota_version = 3,
        .paid_fallback_enabled = paid.paid_fallback_enabled,
        .paid_fallback_limit_microcredits = paid.paid_fallback_limit_microcredits,
        .paid_fallback_spent_microcredits = paid.paid_fallback_spent_microcredits,
        .paid_fallback_reserved_microcredits = paid.paid_fallback_reserved_microcredits,
        .paid_fallback_reservation_request_id = paid.paid_fallback_reservation_request_id,
      };
      std::optional<ApiKeyPolicy> policy = api_key_policy::apiKeyPolicyFromHashRecord(hash, hashRecord, nowMs);
      if (!policy) throw rt::JsError("[ai.ubq.fi] Local development key policy is not strict");
      co_return LocalDevelopmentKeyMaterial{
        .record = std::move(record),
        .hashKey = api_keys::apiKeyHashKey(hash),
        .hashRecord = std::move(hashRecord),
        .policy = std::move(*policy),
      };
    }

    /** Completion is owner-published only after every old cleanup iterator settles. */
    rt::Task<bool> localDevelopmentDeletionStateEmpty(std::shared_ptr<rt::Kv> kv) {
      const std::string keyId(LOCAL_DEVELOPMENT_KEY_ID);
      const rt::Promise<std::optional<contracts::PaidFallbackOutstandingV3>> reading(paid_fallback::ledger_state::getPaidFallbackOutstandingV3(keyId, kv));
      const std::optional<contracts::PaidFallbackOutstandingV3> outstanding = co_await reading.wait();
      if (!outstanding || outstanding->has_outstanding) co_return false;
      const rt::KvGetOptions strong = strongRead();
      const rt::KvKey leaseKey = paid_fallback::ledger_state::paidFallbackReconciliationLeaseV3Key(keyId);
      const rt::KvEntryMaybe<> lease = co_await kv->get(leaseKey, strong);
      if (lease.value.truthy()) co_return false;
      std::vector<rt::KvKey> prefixes;
      prefixes.push_back(paid_fallback::ledger_state::paidFallbackRequestV3Prefix(keyId));
      prefixes.push_back(paid_fallback::ledger_state::paidFallbackWindowV3Prefix(keyId));
      prefixes.push_back(analytics::apiKeyRequestLogPrefix(keyId));
      prefixes.push_back(analytics::legacyApiKeyRequestLogPrefix(keyId));
      prefixes.push_back(rt::KvKey{api_key_policy::API_KEY_USAGE_V2_PREFIX, keyId});
      prefixes.push_back(rt::KvKey{api_key_policy::API_KEY_USAGE_V3_WINDOW_PREFIX, keyId});
      prefixes.push_back(rt::KvKey{api_key_policy::API_KEY_USAGE_V3_REQUEST_PREFIX, keyId});
      for (const rt::KvKey& prefix : prefixes) {
        rt::KvListSelector selector;
        selector.prefix = prefix;
        rt::KvListOptions options;
        options.consistency = rt::KvConsistencyLevel::strong;
        options.limit = 1;
        const std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector, options);
        const std::optional<rt::KvEntry<>> first = co_await iterator->next();
        if (first) co_return false;
      }
      co_return true;
    }

    // `!readMeteredApiKey() && !readSurplusApiKey()`: both read a key or null.
    bool noPaidProviderKey() {
      const std::optional<std::string> metered = provider::metered::readMeteredApiKey();
      const std::optional<std::string> surplus = provider::surplus::readSurplusApiKey();
      return !(metered && !metered->empty()) && !(surplus && !surplus->empty());
    }
  }

  void setLocalDevelopmentPricingDeadlineMsForTest(std::optional<double> deadlineMs) {
    localDevelopmentPricingDeadlineMs = deadlineMs.value_or(LOCAL_DEVELOPMENT_PRICING_DEADLINE_MS);
  }

  rt::Task<LocalDevelopmentKeyStatus> ensureLocalDevelopmentApiKey(std::shared_ptr<rt::Kv> kv) {
    rt::Task<LocalDevelopmentKeyStatus> ensuring = ensureLocalDevelopmentApiKey(std::move(kv), EnsureLocalDevelopmentApiKeyOptions{});
    LocalDevelopmentKeyStatus status = co_await std::move(ensuring);
    co_return status;
  }

  rt::Task<LocalDevelopmentKeyStatus> ensureLocalDevelopmentApiKey(std::shared_ptr<rt::Kv> kv, EnsureLocalDevelopmentApiKeyOptions options) {
    const std::string keyId(LOCAL_DEVELOPMENT_KEY_ID);
    const rt::KvGetOptions strong = strongRead();
    const rt::KvKey idKey = api_keys::apiKeyIdKey(keyId);
    const rt::KvEntryMaybe<> existing = co_await kv->get(idKey, strong);
    if (existing.value.truthy()) co_return LocalDevelopmentKeyStatus(revokedOrMissing(rt::storedJsonView(existing.value, STORED_JSON_VIEW_OPTIONS)) ? "revoked" : "present");
    const rt::KvKey deletionGuardKey = paid_fallback::ledger_state::paidFallbackDeletionGuardV3Key(keyId);
    const rt::KvEntryMaybe<> deletionGuard = co_await kv->get(deletionGuardKey, strong);
    if (deletionGuard.value.truthy()) {
      if (!isCompletedLocalDevelopmentDeletion(rt::storedJsonView(deletionGuard.value, STORED_JSON_VIEW_OPTIONS))) co_return LocalDevelopmentKeyStatus("conflict");
      const rt::Promise<bool> checking(localDevelopmentDeletionStateEmpty(kv));
      const bool empty = co_await checking.wait();
      if (!empty) co_return LocalDevelopmentKeyStatus("conflict");
    }
    if (noPaidProviderKey()) {
      rt::console::warn("[ai.ubq.fi] Local development key was not provisioned: no paid provider API key is configured.");
      co_return LocalDevelopmentKeyStatus("unconfigured");
    }

    const double nowMs = rt::dateNow();
    // The initializer receives a cooperative abort at the deadline, while the
    // race bounds an initializer that never observes it. Both paths end the same
    // way: startup resumes and nothing is published from the aborted attempt.
    const rt::AbortController pricingDeadline;
    const rt::Timeout deadlineTimer = rt::setTimeout(
      [pricingDeadline] {
        pricingDeadline.abort(rt::makeReason(rt::DOMException("Local development pricing initialization exceeded its internal deadline.", "TimeoutError")));
      },
      localDevelopmentPricingDeadlineMs);
    std::vector<rt::AbortSignal> sources;
    if (options.signal) {
      sources.push_back(*options.signal);
      sources.push_back(pricingDeadline.signal());
    }
    const rt::AbortSignal pricingSignal = options.signal ? rt::AbortSignal::any(sources) : pricingDeadline.signal();
    LocalDevelopmentPricingInitializer initializePolicy = options.initializePolicy;
    if (!initializePolicy) {
      initializePolicy = [](std::optional<rt::AbortSignal> signal) { return paid_fallback::index::initializePaidFallbackPolicy(std::move(signal)); };
    }
    std::optional<LocalDevelopmentKeyMaterial> material;
    rt::Reason failure;
    try {
      const rt::Promise<LocalDevelopmentKeyMaterial> building(buildLocalDevelopmentKey(nowMs, std::move(initializePolicy), pricingSignal));
      const rt::Promise<LocalDevelopmentKeyMaterial> racing(awaitWithAbort(building, pricingSignal));
      material = co_await racing.wait();
    } catch (...) {
      failure = std::current_exception();
    }
    rt::clearTimeout(deadlineTimer);
    if (failure) {
      rt::console::warn("[ai.ubq.fi] Local development key was not provisioned; paid providers stay unavailable locally:",
                        rt::isError(failure) ? rt::errorMessage(failure).value_or(std::string()) : rt::reasonToString(failure));
      co_return LocalDevelopmentKeyStatus("unavailable");
    }
    // A deadline or caller cancellation that landed during the attempt must never
    // publish, even when the initializer resolved before it was observed.
    if (pricingSignal.aborted()) co_return LocalDevelopmentKeyStatus("unavailable");

    const rt::KvEntryMaybe<> hashEntry = co_await kv->get(material->hashKey);
    const ::uos::types::ApiKeyUsageWindowV3 window = api_key_policy::makeApiKeyUsageWindowV3(material->policy, nowMs);
    const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
    atomic->check(existing)
      .check(hashEntry)
      .check(deletionGuard)
      .set(idKey, rt::KvValue(json::value_from(material->record)))
      .set(material->hashKey, rt::KvValue(json::value_from(material->hashRecord)))
      .set(api_key_policy::apiKeyUsageV3WindowKey(material->policy), rt::KvValue(json::value_from(window)),
           rt::KvSetOptions{
             .expireIn = api_key_policy::apiKeyUsageV3RetentionMs(window.window_reset_at_ms, nowMs),
           });
    if (deletionGuard.value.truthy()) atomic->delete_(deletionGuard.key);
    const rt::KvCommitResult committed = co_await atomic->commit();
    if (!committed.ok) co_return LocalDevelopmentKeyStatus("conflict");
    co_return LocalDevelopmentKeyStatus("created");
  }

  rt::Task<std::optional<ApiKeyPolicy>> resolveLocalDevelopmentApiKeyPolicy(std::shared_ptr<rt::Kv> kv) {
    rt::Task<std::optional<ApiKeyPolicy>> resolving = resolveLocalDevelopmentApiKeyPolicy(std::move(kv), rt::dateNow());
    std::optional<ApiKeyPolicy> policy = co_await std::move(resolving);
    co_return policy;
  }

  rt::Task<std::optional<ApiKeyPolicy>> resolveLocalDevelopmentApiKeyPolicy(std::shared_ptr<rt::Kv> kv, double nowMs) {
    if (!kv) co_return std::nullopt;
    const rt::KvGetOptions strong = strongRead();
    const rt::KvKey idKey = api_keys::apiKeyIdKey(LOCAL_DEVELOPMENT_KEY_ID);
    const rt::KvEntryMaybe<> recordEntry = co_await kv->get(idKey, strong);
    const json::value record = rt::storedJsonView(recordEntry.value, STORED_JSON_VIEW_OPTIONS);
    if (revokedOrMissing(record)) co_return std::nullopt;
    const std::optional<std::string_view> hash = rt::getString(rt::get(record, "hash"));
    if (!hash || hash->empty()) co_return std::nullopt;
    const std::string tokenHash(*hash);
    const rt::KvKey hashKey = api_keys::apiKeyHashKey(tokenHash);
    const rt::KvEntryMaybe<> hashEntry = co_await kv->get(hashKey, strong);
    const json::value hashRecord = rt::storedJsonView(hashEntry.value, STORED_JSON_VIEW_OPTIONS);
    if (revokedOrMissing(hashRecord)) co_return std::nullopt;
    std::optional<ApiKeyPolicy> policy = api_key_policy::apiKeyPolicyFromHashRecord(tokenHash, hashRecord, nowMs);
    if (!policy) co_return std::nullopt;
    if (policy->expires_at_ms != API_KEY_NO_EXPIRATION_MS && policy->expires_at_ms <= nowMs) co_return std::nullopt;
    co_return policy;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const LocalDevelopmentDeletionGuard::LocalDeletion& value) {
    out = Writer().orNull("owner", value.owner).orNull("completed_at_ms", value.completed_at_ms).done();
  }

  LocalDevelopmentDeletionGuard::LocalDeletion tag_invoke(const json::value_to_tag<LocalDevelopmentDeletionGuard::LocalDeletion>&, const json::value& value) {
    const Reader in(value, "LocalDevelopmentDeletionGuard.local_deletion");
    return {
      .owner = in.orNull<std::string>("owner"),
      .completed_at_ms = in.orNull<double>("completed_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const LocalDevelopmentDeletionGuard& value) {
    out = Writer().required("created_at_ms", value.created_at_ms).required("local_deletion", value.local_deletion).done();
  }

  LocalDevelopmentDeletionGuard tag_invoke(const json::value_to_tag<LocalDevelopmentDeletionGuard>&, const json::value& value) {
    const Reader in(value, "LocalDevelopmentDeletionGuard");
    return {
      .created_at_ms = in.required<double>("created_at_ms"),
      .local_deletion = in.required<LocalDevelopmentDeletionGuard::LocalDeletion>("local_deletion"),
    };
  }
}
