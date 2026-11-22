#include "uos/api_key_policy.hpp"
#include "uos/rt/memory.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/api_keys.hpp"
#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <cstddef>
#include <exception>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace uos::api_key_policy {
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
    namespace native_api_key = authority::native_api_key;
    namespace contracts = authority::native_contracts;
    using ::uos::api_keys::API_KEY_NO_EXPIRATION_MS;
    using ::uos::api_keys::API_KEY_NO_USAGE_LIMIT;
    using ::uos::types::ApiKeyUsageWindowV3;

    constexpr double MAX_KV_RETRIES = 5;

    // `{ consistency: "strong" }`
    rt::KvGetOptions strongRead() { return rt::KvGetOptions{.consistency = rt::KvConsistencyLevel::strong}; }

    rt::KvListSelector prefixSelector(rt::KvKey prefix) {
      rt::KvListSelector selector;
      selector.prefix = std::move(prefix);
      return selector;
    }

    // A stored KV value as the TS reads it through property access: what JSON can hold is kept, an
    // undefined member is absent, an undefined or hole element is null, and every other part (bigint,
    // KvU64, Date, Map, buffers, ...) becomes `{}`, an object no number/string/boolean/null check accepts.

    // `entry.value` of a read: null for an absent row (Deno KV reads `value: null`).

    contracts::ApiKeyUsageSnapshot snapshotOf(const rt::KvEntryMaybe<>& requestEntry, const rt::KvEntryMaybe<>& windowEntry) {
      return contracts::ApiKeyUsageSnapshot{.request = rt::storedJsonView(requestEntry.value, STORED_JSON_VIEW_OPTIONS), .window = rt::storedJsonView(windowEntry.value, STORED_JSON_VIEW_OPTIONS)};
    }

    // `error instanceof ApiKeyQuotaDispatchError`
    bool isQuotaDispatchError(const rt::Reason& reason) {
      if (!reason) return false;
      try {
        std::rethrow_exception(reason);
      } catch (const ApiKeyQuotaDispatchError&) {
        return true;
      } catch (...) {
        return false;
      }
    }

    // `type CachedPolicy = Readonly<{ policy: ApiKeyPolicy; expires_at_ms: number }>`
    struct CachedPolicy {
      ApiKeyPolicy policy;
      double expires_at_ms;
    };

    // `const policyCache = new Map<string, CachedPolicy>()`
    rt::OrderedMap<std::string, CachedPolicy>& policyCache() {
      static rt::OrderedMap<std::string, CachedPolicy> cache;
      return cache;
    }

    std::size_t policyCacheBytes = 0;

    std::size_t policyBytes(const std::string& hash, const CachedPolicy& entry) {
      return sizeof(CachedPolicy) + 2 * rt::memory::bytes(hash) + rt::memory::bytes(entry.policy.token_hash) +
             rt::memory::bytes(entry.policy.key_id) + rt::memory::bytes(entry.policy.policy_version) +
             (entry.policy.paid_fallback_reservation_request_id ? rt::memory::bytes(*entry.policy.paid_fallback_reservation_request_id) : 0);
    }
    void erasePolicy(const std::string& hash) {
      auto& cache = policyCache();
      if (const auto* entry = cache.get(hash)) {
        policyCacheBytes -= policyBytes(hash, *entry);
        cache.delete_(hash);
        rt::memory::setCache(rt::memory::Cache::api_key_policy, cache.size(), policyCacheBytes);
      }
    }
    void cachePolicy(const std::string& hash, CachedPolicy entry) {
      auto& cache = policyCache();
      const std::size_t bytes = policyBytes(hash, entry);
      erasePolicy(hash);
      if (bytes > rt::limits::kApiKeyPolicyCacheBytes) return;
      // TTL stays an upper bound on freshness. When the memo is full, an
      // evicted key goes through the same durable lookup as any cold key.
      while (cache.size() >= rt::limits::kApiKeyPolicyCacheEntries ||
             policyCacheBytes > rt::limits::kApiKeyPolicyCacheBytes - bytes) {
        const auto oldest = cache.begin();
        erasePolicy(oldest->first);
      }
      cache.set(hash, std::move(entry));
      policyCacheBytes += bytes;
      rt::memory::setCache(rt::memory::Cache::api_key_policy, cache.size(), policyCacheBytes);
    }

    rt::Response quotaUnavailable(std::string message) {
      http::OpenaiErrorOptions options;
      options.type = "server_error";
      return http::openaiError(503, std::move(message), std::string("server_error"), std::move(options));
    }
    rt::Response quotaUnavailable() { return quotaUnavailable("API key quota ledger is unavailable"); }

    ApiKeyUsageReservationDecision quotaUnavailableDecision(std::string message) { return ApiKeyUsageReservationDecisionNotOk{.response = quotaUnavailable(std::move(message))}; }
    ApiKeyUsageReservationDecision quotaUnavailableDecision() { return ApiKeyUsageReservationDecisionNotOk{.response = quotaUnavailable()}; }

    ApiKeyUsageReservationDecision expiredPolicyResponse() {
      return ApiKeyUsageReservationDecisionNotOk{.response = http::openaiError(401, "Unauthorized", std::string("invalid_api_key"))};
    }

    rt::Headers rateLimitExceededHeaders(const ApiKeyUsageWindowV3& window, const ApiKeyPolicy& policy) {
      const double retryAfterSeconds = rt::Math::max(1.0, rt::Math::ceil(rt::Math::max(0.0, window.window_reset_at_ms - rt::dateNow()) / 1000));
      const double remaining = rt::Math::max(0.0, policy.usage_limit_requests - window.committed_requests - window.reserved_requests);
      const double windowSeconds = rt::Math::max(1.0, rt::Math::ceil((window.window_reset_at_ms - window.window_start_ms) / 1000));
      rt::Headers headers;
      headers.append("Retry-After", rt::numberToString(retryAfterSeconds));
      headers.append("RateLimit", "\"api-key\";r=" + rt::numberToString(remaining) + ";t=" + rt::numberToString(retryAfterSeconds));
      headers.append("RateLimit-Policy", "\"api-key\";q=" + rt::numberToString(policy.usage_limit_requests) + ";w=" + rt::numberToString(windowSeconds));
      // Older API clients still look for this de facto field family. Keep it in
      // addition to the active HTTPAPI RateLimit Internet-Draft fields.
      headers.append("RateLimit-Limit", rt::numberToString(policy.usage_limit_requests));
      headers.append("RateLimit-Remaining", rt::numberToString(remaining));
      headers.append("RateLimit-Reset", rt::numberToString(retryAfterSeconds));
      return headers;
    }

    ApiKeyUsageReservationDecision quotaExceededResponse(const ApiKeyUsageWindowV3& window, const ApiKeyPolicy& policy) {
      std::string message = "Usage limit exceeded (" + rt::numberToString(window.committed_requests) + "/" + rt::numberToString(policy.usage_limit_requests) +
                            "). Resets at " + rt::Date(window.window_reset_at_ms).toISOString();
      http::OpenaiErrorOptions options;
      options.type = "rate_limit_error";
      options.headers = rateLimitExceededHeaders(window, policy);
      return ApiKeyUsageReservationDecisionNotOk{.response = http::openaiError(429, std::move(message), std::string("rate_limit_exceeded"), std::move(options))};
    }

    // `entry.value?.revoked_at_ms !== null`
    bool revokedOrMissing(const json::value& record) {
      const json::value* revokedAtMs = rt::get(record, "revoked_at_ms");
      return revokedAtMs == nullptr || !revokedAtMs->is_null();
    }

    std::optional<ApiKeyPolicy> livePolicyFromEntry(const std::string& tokenHash, const json::value& record, double nowMs) {
      if (revokedOrMissing(record)) return std::nullopt;
      std::optional<ApiKeyPolicy> policy = apiKeyPolicyFromHashRecord(tokenHash, record, nowMs);
      if (!policy) return std::nullopt;
      if (policy->expires_at_ms != API_KEY_NO_EXPIRATION_MS && policy->expires_at_ms <= nowMs) return std::nullopt;
      return policy;
    }

    /**
     * A same-isolate contention optimization for the aggregate mutations below.
     *
     * This never replaces the Deno KV checks in those mutations: another isolate
     * (or a deployment replacement) does not share this map, so KV CAS remains
     * the correctness boundary. Keeping the queue scoped to a window lets a burst
     * of local requests make forward progress instead of repeatedly colliding on
     * the same aggregate until MAX_KV_RETRIES is exhausted.
     */
    rt::OrderedMap<std::string, rt::Promise<void>>& apiKeyUsageWindowLocks() {
      static rt::OrderedMap<std::string, rt::Promise<void>> locks;
      return locks;
    }

    // `JSON.stringify(apiKeyUsageV3WindowKey(policy))`
    std::string apiKeyUsageWindowLockKey(const ApiKeyPolicy& policy) {
      json::array key;
      for (const rt::KvKeyPart& part : apiKeyUsageV3WindowKey(policy)) {
        if (part.isString()) key.push_back(json::value(json::string(part.string())));
        else key.push_back(rt::makeNumber(part.number()));
      }
      return rt::jsonStringify(json::value(std::move(key)));
    }

    template <class T> using LockedOperation = std::function<rt::Task<T>()>;

    template <class T> rt::Task<T> withApiKeyUsageWindowLock(ApiKeyPolicy policy, LockedOperation<T> operation) {
      const std::string lockKey = apiKeyUsageWindowLockKey(policy);
      std::optional<rt::Promise<void>> previous;
      if (const rt::Promise<void>* found = apiKeyUsageWindowLocks().get(lockKey)) previous = *found;
      rt::Resolvers<void> resolvers = rt::withResolvers<void>();
      const rt::Promise<void> current = resolvers.promise;
      apiKeyUsageWindowLocks().set(lockKey, current);
      if (previous) co_await previous->wait();

      std::optional<T> result;
      rt::Reason failure;
      try {
        const rt::Promise<T> running(operation());
        result = co_await running.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      resolvers.resolve();
      if (const rt::Promise<void>* tail = apiKeyUsageWindowLocks().get(lockKey); tail != nullptr && *tail == current) apiKeyUsageWindowLocks().delete_(lockKey);
      if (failure) std::rethrow_exception(failure);
      co_return std::move(*result);
    }

    // `"released" | "settled" | "missing" | "invalid" | "conflict"`
    rt::Task<std::string> commitUsagePlan(std::shared_ptr<rt::Kv> kv, rt::KvKey requestKey, rt::KvKey windowKey, rt::KvEntryMaybe<> requestEntry,
                                          rt::KvEntryMaybe<> windowEntry, contracts::ApiKeyUsageReleasePlan plan) {
      if (const auto* other = std::get_if<contracts::ApiKeyUsageReleasePlanMissingInvalidSettled>(&plan)) co_return other->kind;
      const contracts::ApiKeyUsageWritePlan& write = std::get<contracts::ApiKeyUsageWritePlan>(plan);
      const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(requestEntry)
        .check(windowEntry)
        .set(requestKey, rt::KvValue(json::value_from(write.request)), rt::KvSetOptions{.expireIn = write.expireIn})
        .set(windowKey, rt::KvValue(json::value_from(write.window)), rt::KvSetOptions{.expireIn = write.expireIn});
      const rt::KvCommitResult committed = co_await operation->commit();
      co_return std::string(committed.ok ? "released" : "conflict");
    }

    rt::Task<std::string> releaseReservedRequest(std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy, std::string requestId, std::string reason, double nowMs,
                                                 std::optional<std::string> expectedRoute) {
      rt::KvKey requestKey = apiKeyUsageV3RequestKey(policy, requestId);
      rt::KvKey windowKey = apiKeyUsageV3WindowKey(policy);
      const rt::KvGetOptions strong = strongRead();
      const rt::Promise<rt::KvEntryMaybe<>> requestRead(kv->get(requestKey, strong));
      const rt::Promise<rt::KvEntryMaybe<>> windowRead(kv->get(windowKey, strong));
      auto reads = rt::all(requestRead, windowRead);
      auto readEntries = co_await reads.wait();
      auto& [requestEntry, windowEntry] = readEntries;
      contracts::ApiKeyUsageReleasePlan plan =
        native_api_key::planApiKeyUsageReservedRelease(policy, requestId, reason, snapshotOf(requestEntry, windowEntry), nowMs, expectedRoute);
      rt::Task<std::string> committing = commitUsagePlan(kv, std::move(requestKey), std::move(windowKey), requestEntry, windowEntry, std::move(plan));
      std::string outcome = co_await std::move(committing);
      co_return outcome;
    }

    rt::Task<std::string> releaseDispatchedRequest(std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy, std::string requestId, std::string route,
                                                   ApiKeyUsageProvider provider, double nowMs) {
      rt::KvKey requestKey = apiKeyUsageV3RequestKey(policy, requestId);
      rt::KvKey windowKey = apiKeyUsageV3WindowKey(policy);
      const rt::KvGetOptions strong = strongRead();
      const rt::Promise<rt::KvEntryMaybe<>> requestRead(kv->get(requestKey, strong));
      const rt::Promise<rt::KvEntryMaybe<>> windowRead(kv->get(windowKey, strong));
      auto reads = rt::all(requestRead, windowRead);
      auto readEntries = co_await reads.wait();
      auto& [requestEntry, windowEntry] = readEntries;
      contracts::ApiKeyUsageReleasePlan plan =
        native_api_key::planApiKeyUsageDispatchCancellation(policy, requestId, route, provider, snapshotOf(requestEntry, windowEntry), nowMs);
      rt::Task<std::string> committing = commitUsagePlan(kv, std::move(requestKey), std::move(windowKey), requestEntry, windowEntry, std::move(plan));
      std::string outcome = co_await std::move(committing);
      co_return outcome;
    }

    // The body `withApiKeyUsageWindowLock` runs for `cancelBeforeTransport`.
    rt::Task<std::monostate> releaseDispatchedUnderLock(std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy, std::string requestId, std::string route,
                                                        ApiKeyUsageProvider provider) {
      for (double attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
        const rt::Promise<std::string> releasing(releaseDispatchedRequest(kv, policy, requestId, route, provider, rt::dateNow()));
        const std::string outcome = co_await releasing.wait();
        if (outcome == "released" || outcome == "settled" || outcome == "missing") co_return std::monostate{};
        if (outcome == "invalid") {
          throw ApiKeyQuotaDispatchError("API key quota reservation is malformed");
        }
      }
      throw ApiKeyQuotaDispatchError("API key quota reservation changed concurrently");
    }

    // The `cancellation ??= (async () => { ... })()` body of providerDispatchContext.
    rt::Task<void> compensateProviderDispatch(std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy, std::string requestId, std::string route,
                                              ApiKeyUsageProvider provider) {
      rt::Reason failure;
      try {
        LockedOperation<std::monostate> operation = [kv, policy, requestId, route, provider]() {
          return releaseDispatchedUnderLock(kv, policy, requestId, route, provider);
        };
        const rt::Promise<std::monostate> locked(withApiKeyUsageWindowLock<std::monostate>(policy, std::move(operation)));
        co_await locked.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      if (!failure) co_return;
      if (isQuotaDispatchError(failure)) std::rethrow_exception(failure);
      rt::console::warn("[ai.ubq.fi] Failed to compensate API key quota dispatch:", failure);
      throw ApiKeyQuotaDispatchError("API key quota reservation is unavailable");
    }

    // The closure state of providerDispatchContext.
    struct ProviderDispatchState {
      bool transportStarted = false;
      std::optional<rt::Promise<void>> cancellation;
    };

    rt::Task<void> cancelBeforeTransport(std::shared_ptr<ProviderDispatchState> state, std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy, std::string requestId,
                                         std::string route, ApiKeyUsageProvider provider) {
      if (state->transportStarted) co_return;
      if (!state->cancellation) state->cancellation = rt::Promise<void>(compensateProviderDispatch(kv, policy, requestId, route, provider));
      const rt::Promise<void> cancellation = *state->cancellation;
      co_await cancellation.wait();
    }

    ApiKeyProviderDispatch providerDispatchContext(std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy, std::string requestId, std::string route,
                                                   ApiKeyUsageProvider provider) {
      auto state = std::make_shared<ProviderDispatchState>();
      ApiKeyProviderDispatch dispatch;
      dispatch.markTransportStarted = [state] { state->transportStarted = true; };
      dispatch.cancelBeforeTransport = [state, kv = std::move(kv), policy = std::move(policy), requestId = std::move(requestId), route = std::move(route),
                                        provider = std::move(provider)]() { return cancelBeforeTransport(state, kv, policy, requestId, route, provider); };
      return dispatch;
    }

    /** Request ids in this window whose reservation lease has already expired. */
    rt::Task<std::vector<std::string>> expiredApiKeyUsageRequestIds(std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy, double nowMs) {
      const rt::KvKey prefix{API_KEY_USAGE_V3_REQUEST_PREFIX, policy.key_id, policy.policy_version, policy.window_start_ms};
      std::vector<std::string> expired;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(prefixSelector(prefix));
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        const std::optional<::uos::types::ApiKeyUsageRequestV3> request = native_api_key::normalizeApiKeyUsageRequestV3(rt::storedJsonView(entry->value, STORED_JSON_VIEW_OPTIONS));
        const rt::KvKeyPart* requestId = entry->key.empty() ? nullptr : &entry->key.back();
        if (!request || requestId == nullptr || !requestId->isString() || request->key_id != policy.key_id || request->request_id != requestId->string()) {
          co_await iterator->return_();
          throw rt::JsError("API key quota request is malformed");
        }
        if (native_api_key::isApiKeyUsageReservationExpired(*request, nowMs)) expired.push_back(request->request_id);
      }
      co_return expired;
    }

    /** Releases one expired reservation, retrying KV conflicts up to MAX_KV_RETRIES times. */
    rt::Task<void> releaseExpiredApiKeyUsageReservation(std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy, std::string requestId, double nowMs) {
      for (double attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
        const rt::Promise<std::string> releasing(releaseReservedRequest(kv, policy, requestId, "lease_expired", nowMs, std::nullopt));
        const std::string outcome = co_await releasing.wait();
        if (outcome == "conflict") continue;
        if (outcome == "invalid") throw rt::JsError("API key quota request is malformed");
        co_return;
      }
      throw rt::JsError("API key quota lease changed concurrently");
    }

    /**
     * Expired leases are released before admission. This is intentionally a
     * separate, retryable pass: Deno KV cannot list a dynamic set of request keys
     * inside the aggregate's atomic check.
     */
    rt::Task<std::monostate> reclaimExpiredApiKeyUsageReservationsV3Unlocked(std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy, double nowMs) {
      const rt::Promise<std::vector<std::string>> listing(expiredApiKeyUsageRequestIds(kv, policy, nowMs));
      const std::vector<std::string> requestIds = co_await listing.wait();
      for (const std::string& requestId : requestIds) {
        const rt::Promise<void> releasing(releaseExpiredApiKeyUsageReservation(kv, policy, requestId, nowMs));
        co_await releasing.wait();
      }
      co_return std::monostate{};
    }

    // The closure state of reservationContext: `let dispatchedByThisReservation = false`.
    struct ReservationState {
      bool dispatchedByThisReservation = false;
    };

    // The body `withApiKeyUsageWindowLock` runs for `beforeProviderDispatch`.
    rt::Task<bool> commitProviderDispatchUnderLock(std::shared_ptr<ReservationState> state, std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy, std::string requestId,
                                                   std::string route, ApiKeyUsageProvider provider) {
      const rt::KvKey requestKey = apiKeyUsageV3RequestKey(policy, requestId);
      const rt::KvKey windowKey = apiKeyUsageV3WindowKey(policy);
      const rt::KvGetOptions strong = strongRead();
      for (double attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
        const rt::Promise<rt::KvEntryMaybe<>> requestRead(kv->get(requestKey, strong));
        const rt::Promise<rt::KvEntryMaybe<>> windowRead(kv->get(windowKey, strong));
        auto reads = rt::all(requestRead, windowRead);
        auto readEntries = co_await reads.wait();
        auto& [requestEntry, windowEntry] = readEntries;
        const contracts::ApiKeyUsageDispatchPlan plan =
          native_api_key::planApiKeyUsageDispatch(policy, requestId, route, provider, snapshotOf(requestEntry, windowEntry), rt::dateNow());
        if (std::holds_alternative<contracts::ApiKeyUsageDispatchPlanInvalid>(plan)) throw ApiKeyQuotaDispatchError();
        if (std::holds_alternative<contracts::ApiKeyUsageDispatchPlanAlreadyDispatched>(plan)) co_return false;
        const contracts::ApiKeyUsageWritePlan& write = std::get<contracts::ApiKeyUsageWritePlan>(plan);
        const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        operation->check(requestEntry)
          .check(windowEntry)
          .set(requestKey, rt::KvValue(json::value_from(write.request)), rt::KvSetOptions{.expireIn = write.expireIn})
          .set(windowKey, rt::KvValue(json::value_from(write.window)), rt::KvSetOptions{.expireIn = write.expireIn});
        const rt::KvCommitResult committed = co_await operation->commit();
        if (committed.ok) {
          state->dispatchedByThisReservation = true;
          co_return true;
        }
      }
      throw ApiKeyQuotaDispatchError("API key quota reservation changed concurrently");
    }

    rt::Task<std::optional<ApiKeyProviderDispatch>> beforeProviderDispatch(std::shared_ptr<ReservationState> state, std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy,
                                                                           std::string requestId, std::string route, ApiKeyUsageProvider provider) {
      // Every path that reaches the read below assigns this: the catch block always rethrows.
      bool dispatchedHere = false;
      rt::Reason failure;
      try {
        // The commit result must flow out of the closure: control-flow analysis cannot see
        // assignments made inside it, so a captured `let` flag would read as always false.
        LockedOperation<bool> operation = [state, kv, policy, requestId, route, provider]() {
          return commitProviderDispatchUnderLock(state, kv, policy, requestId, route, provider);
        };
        const rt::Promise<bool> locked(withApiKeyUsageWindowLock<bool>(policy, std::move(operation)));
        dispatchedHere = co_await locked.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) {
        if (isQuotaDispatchError(failure)) std::rethrow_exception(failure);
        rt::console::warn("[ai.ubq.fi] Failed to commit API key quota dispatch:", failure);
        throw ApiKeyQuotaDispatchError("API key quota reservation is unavailable");
      }
      if (!dispatchedHere) co_return std::nullopt;
      co_return providerDispatchContext(kv, policy, requestId, route, provider);
    }

    // The body `withApiKeyUsageWindowLock` runs for `release`.
    rt::Task<std::monostate> releaseReservationUnderLock(std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy, std::string requestId, std::string route, std::string reason) {
      for (double attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
        const rt::Promise<std::string> releasing(releaseReservedRequest(kv, policy, requestId, reason, rt::dateNow(), route));
        const std::string outcome = co_await releasing.wait();
        if (outcome == "released" || outcome == "settled" || outcome == "missing") co_return std::monostate{};
        if (outcome == "invalid") throw ApiKeyQuotaDispatchError("API key quota reservation is malformed");
      }
      throw ApiKeyQuotaDispatchError("API key quota reservation changed concurrently");
    }

    rt::Task<void> releaseReservation(std::shared_ptr<ReservationState> state, std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy, std::string requestId, std::string route,
                                      std::string reason) {
      if (state->dispatchedByThisReservation) co_return;
      rt::Reason failure;
      try {
        LockedOperation<std::monostate> operation = [kv, policy, requestId, route, reason]() {
          return releaseReservationUnderLock(kv, policy, requestId, route, reason);
        };
        const rt::Promise<std::monostate> locked(withApiKeyUsageWindowLock<std::monostate>(policy, std::move(operation)));
        co_await locked.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      if (!failure) co_return;
      if (isQuotaDispatchError(failure)) std::rethrow_exception(failure);
      rt::console::warn("[ai.ubq.fi] Failed to release API key quota reservation:", failure);
      throw ApiKeyQuotaDispatchError("API key quota reservation is unavailable");
    }

    ApiKeyUsageReservation reservationContext(std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy, std::string requestId, std::string route) {
      // `release()` runs after every route. Once this exact reservation has
      // durably moved from reserved to dispatched, it cannot release a
      // reservation: the request row is no longer reserved. Remembering that
      // local transition avoids rereading the request and aggregate merely to
      // rediscover the already-settled state. This is deliberately not shared
      // across requests or isolates; KV remains the authority before dispatch.
      auto state = std::make_shared<ReservationState>();
      ApiKeyUsageReservation reservation{
        .policy = policy,
        .request_id = requestId,
        .route = route,
        .beforeProviderDispatch = {},
        .release = {},
      };
      reservation.beforeProviderDispatch = [state, kv, policy, requestId, route](ApiKeyUsageProvider provider) {
        return beforeProviderDispatch(state, kv, policy, requestId, route, std::move(provider));
      };
      reservation.release = [state, kv, policy, requestId, route](std::optional<std::string> reason) {
        return releaseReservation(state, kv, policy, requestId, route, reason.value_or("route_completed_without_provider_dispatch"));
      };
      return reservation;
    }

    rt::Task<ApiKeyQuotaDispatchError> quotaDispatchErrorFromResponse(rt::Response response) {
      // `await response.clone().json().catch(() => null)`: the clone throws outside the catch.
      const rt::Response copy = response.clone();
      json::value payload = nullptr;
      try {
        const rt::Promise<json::value> reading(copy.json());
        payload = co_await reading.wait();
      } catch (...) {
        payload = nullptr;
      }
      // The turn of the derived `.catch` promise.
      co_await rt::microtask();
      const json::value* error = utils::isRecord(payload) && utils::isRecord(rt::get(payload, "error")) ? rt::get(payload, "error") : nullptr;
      const std::optional<std::string_view> errorMessage = error != nullptr ? rt::getString(*error, "message") : std::nullopt;
      std::string message = errorMessage ? std::string(*errorMessage) : std::string("API key quota reservation is no longer available");
      rt::OrderedMap<std::string, std::string> headers;
      std::vector<std::string_view> names{"Retry-After"};
      names.insert(names.end(), http::STANDARD_RATE_LIMIT_HEADERS.begin(), http::STANDARD_RATE_LIMIT_HEADERS.end());
      for (const std::string_view name : names) {
        const std::optional<std::string> value = response.headers().get(name);
        if (rt::truthy(value)) headers.set(std::string(name), *value);
      }
      const bool rateLimited = response.status() == 429;
      const std::optional<std::string_view> explicitCode = error != nullptr ? rt::getString(*error, "code") : std::nullopt;
      const std::optional<std::string_view> explicitErrorType = error != nullptr ? rt::getString(*error, "type") : std::nullopt;
      ApiKeyQuotaDispatchErrorOptions options;
      options.status = static_cast<double>(response.status());
      options.code = explicitCode ? std::string(*explicitCode) : std::string(rateLimited ? "rate_limit_exceeded" : "api_key_quota_reservation_unavailable");
      options.errorType = explicitErrorType ? std::string(*explicitErrorType) : std::string(rateLimited ? "rate_limit_error" : "server_error");
      const std::optional<std::string> retryAfter = response.headers().get("Retry-After");
      if (retryAfter) options.retryAfter = *retryAfter;
      else options.retryAfter = nullptr;
      options.headers = std::move(headers);
      co_return ApiKeyQuotaDispatchError(std::move(message), std::move(options));
    }

    // The closure state of deferredReservationContext.
    struct DeferredReservationState {
      std::optional<ApiKeyUsageReservation> active;
      std::optional<rt::Promise<ApiKeyUsageReservation>> admission;
    };

    // The `admission ??= (async () => { ... })()` body.
    rt::Task<ApiKeyUsageReservation> admitDeferredReservation(std::shared_ptr<DeferredReservationState> state, std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy,
                                                              std::string requestId, std::string route) {
      ReserveApiKeyUsageV3Options options;
      options.kv = kv;
      const rt::Promise<ApiKeyUsageReservationDecision> reserving(reserveApiKeyUsageV3(policy, requestId, route, std::move(options)));
      const ApiKeyUsageReservationDecision decision = co_await reserving.wait();
      if (const auto* refused = std::get_if<ApiKeyUsageReservationDecisionNotOk>(&decision)) {
        const rt::Promise<ApiKeyQuotaDispatchError> converting(quotaDispatchErrorFromResponse(refused->response));
        const ApiKeyQuotaDispatchError error = co_await converting.wait();
        throw error;
      }
      state->active = std::get<ApiKeyUsageReservationDecisionOk>(decision).reservation;
      co_return *state->active;
    }

    rt::Task<ApiKeyUsageReservation> requireAdmission(std::shared_ptr<DeferredReservationState> state, std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy,
                                                      std::string requestId, std::string route) {
      if (state->active) co_return *state->active;
      if (!state->admission) state->admission = rt::Promise<ApiKeyUsageReservation>(admitDeferredReservation(state, kv, policy, requestId, route));
      const rt::Promise<ApiKeyUsageReservation> admission = *state->admission;
      ApiKeyUsageReservation reservation = co_await admission.wait();
      co_return reservation;
    }

    // `async (provider) => (await requireAdmission()).beforeProviderDispatch(provider)`: the async
    // arrow returns the dispatch promise, which it adopts two turns after it settles.
    rt::Task<std::optional<ApiKeyProviderDispatch>> deferredBeforeProviderDispatch(std::shared_ptr<DeferredReservationState> state, std::shared_ptr<rt::Kv> kv,
                                                                                   ApiKeyPolicy policy, std::string requestId, std::string route,
                                                                                   ApiKeyUsageProvider provider) {
      const rt::Promise<ApiKeyUsageReservation> admitting(requireAdmission(state, kv, policy, requestId, route));
      const ApiKeyUsageReservation active = co_await admitting.wait();
      const rt::Promise<std::optional<ApiKeyProviderDispatch>> dispatching(active.beforeProviderDispatch(std::move(provider)));
      co_await rt::microtask();
      std::optional<ApiKeyProviderDispatch> dispatch = co_await dispatching.wait();
      co_return dispatch;
    }

    rt::Task<void> deferredRelease(std::shared_ptr<DeferredReservationState> state, std::optional<std::string> reason) {
      if (!state->active) co_return;
      const ApiKeyUsageReservation active = *state->active;
      const rt::Promise<void> releasing(active.release(std::move(reason)));
      co_await releasing.wait();
    }

    ApiKeyUsageReservation deferredReservationContext(std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy, std::string requestId, std::string route) {
      auto state = std::make_shared<DeferredReservationState>();
      ApiKeyUsageReservation reservation{
        .policy = policy,
        .request_id = requestId,
        .route = route,
        .beforeProviderDispatch = {},
        .release = {},
      };
      reservation.beforeProviderDispatch = [state, kv, policy, requestId, route](ApiKeyUsageProvider provider) {
        return deferredBeforeProviderDispatch(state, kv, policy, requestId, route, std::move(provider));
      };
      reservation.release = [state](std::optional<std::string> reason) { return deferredRelease(state, std::move(reason)); };
      return reservation;
    }

    /**
     * One guarded reservation attempt inside the window lock: a terminal decision, a
     * retry after expired leases were reclaimed, or a plain conflict retry.
     */
    struct ApiKeyUsageReservationAttemptDecision {
      ApiKeyUsageReservationDecision decision;
    };
    struct ApiKeyUsageReservationAttemptReclaimed {};
    struct ApiKeyUsageReservationAttemptRetry {};
    using ApiKeyUsageReservationAttempt =
      std::variant<ApiKeyUsageReservationAttemptDecision, ApiKeyUsageReservationAttemptReclaimed, ApiKeyUsageReservationAttemptRetry>;

    /**
     * The 401-versus-503 split for a hash record that cannot produce a live policy.
     * The record is KV data, so its expiry field is still re-checked at runtime.
     */
    ApiKeyUsageReservationDecision unavailablePolicyResponse(const json::value& record, double nowMs) {
      const json::value* revokedAtMs = rt::get(record, "revoked_at_ms");
      const std::optional<double> expiresAtMs = rt::getNumber(rt::get(record, "expires_at_ms"));
      if (!utils::isRecord(record) || revokedAtMs == nullptr || !revokedAtMs->is_null() || (expiresAtMs && *expiresAtMs <= nowMs)) {
        return expiredPolicyResponse();
      }
      return quotaUnavailableDecision("API key quota policy is incomplete");
    }

    /** Handles an apparently full window; null means expired leases were reclaimed and the caller must retry. */
    rt::Task<std::optional<ApiKeyUsageReservationDecision>> fullWindowReservation(std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy, std::string requestId,
                                                                                  std::string route, ApiKeyUsageWindowV3 window, double nowMs,
                                                                                  bool reclaimedExpiredReservations, bool deferWhenFull) {
      if (reclaimedExpiredReservations) {
        if (deferWhenFull) co_return ApiKeyUsageReservationDecision(ApiKeyUsageReservationDecisionOk{.reservation = deferredReservationContext(kv, policy, requestId, route)});
        co_return quotaExceededResponse(window, policy);
      }
      rt::Reason failure;
      try {
        // Unlimited policies never need this list. Bounded policies scan
        // only after the aggregate appears full, so an expired lease is
        // always reclaimed before we return the capacity 429.
        const rt::Promise<std::monostate> reclaiming(reclaimExpiredApiKeyUsageReservationsV3Unlocked(kv, policy, nowMs));
        co_await reclaiming.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) {
        rt::console::warn("[ai.ubq.fi] Failed to reclaim API key quota leases:", failure);
        co_return quotaUnavailableDecision();
      }
      co_return std::nullopt;
    }

    // `messages[plan.reason]`; an unknown reason reads undefined, the default message.
    ApiKeyUsageReservationDecision unavailableReservation(const std::string& reason) {
      if (reason == "request_malformed") return quotaUnavailableDecision("API key quota request is malformed");
      if (reason == "identity_conflict") return quotaUnavailableDecision("API key quota request identity conflicts");
      if (reason == "identity_consumed") return quotaUnavailableDecision("API key quota request identity is already consumed");
      if (reason == "window_malformed") return quotaUnavailableDecision("API key quota aggregate is malformed");
      return quotaUnavailableDecision();
    }

    /**
     * Runs one reservation attempt against the policy entry, the request row and the
     * window aggregate, keeping the original check order: policy, request, window,
     * capacity, then the compare-and-set commit.
     */
    rt::Task<ApiKeyUsageReservationAttempt> attemptApiKeyUsageReservation(std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy, std::string requestId, std::string route,
                                                                          double nowMs, bool reclaimedExpiredReservations, bool deferWhenFull) {
      const rt::KvGetOptions strong = strongRead();
      const rt::KvKey hashKey = api_keys::apiKeyHashKey(policy.token_hash);
      const rt::KvEntryMaybe<> policyEntry = co_await kv->get(hashKey, strong);
      const json::value policyRecord = rt::storedJsonView(policyEntry.value, STORED_JSON_VIEW_OPTIONS);
      const std::optional<ApiKeyPolicy> livePolicy = livePolicyFromEntry(policy.token_hash, policyRecord, nowMs);
      if (!livePolicy) co_return ApiKeyUsageReservationAttemptDecision{.decision = unavailablePolicyResponse(policyRecord, nowMs)};

      const rt::KvKey windowKey = apiKeyUsageV3WindowKey(*livePolicy);
      const rt::KvKey requestKey = apiKeyUsageV3RequestKey(*livePolicy, requestId);
      const rt::Promise<rt::KvEntryMaybe<>> windowRead(kv->get(windowKey, strong));
      const rt::Promise<rt::KvEntryMaybe<>> requestRead(kv->get(requestKey, strong));
      auto reads = rt::all(windowRead, requestRead);
      auto readEntries = co_await reads.wait();
      auto& [windowEntry, requestEntry] = readEntries;
      const contracts::ApiKeyUsageReservationPlan plan =
        native_api_key::planApiKeyUsageReservation(*livePolicy, requestId, route, snapshotOf(requestEntry, windowEntry), nowMs);
      if (const auto* unavailable = std::get_if<contracts::ApiKeyUsageReservationPlanUnavailable>(&plan)) {
        co_return ApiKeyUsageReservationAttemptDecision{.decision = unavailableReservation(unavailable->reason)};
      }
      if (std::holds_alternative<contracts::ApiKeyUsageReservationPlanReuse>(plan)) {
        co_return ApiKeyUsageReservationAttemptDecision{
          .decision = ApiKeyUsageReservationDecisionOk{.reservation = reservationContext(kv, *livePolicy, requestId, route)},
        };
      }
      if (const auto* full = std::get_if<contracts::ApiKeyUsageReservationPlanFull>(&plan)) {
        rt::Task<std::optional<ApiKeyUsageReservationDecision>> handling =
          fullWindowReservation(kv, *livePolicy, requestId, route, full->window, nowMs, reclaimedExpiredReservations, deferWhenFull);
        const rt::Promise<std::optional<ApiKeyUsageReservationDecision>> fullHandling(std::move(handling));
        std::optional<ApiKeyUsageReservationDecision> fullOutcome = co_await fullHandling.wait();
        if (fullOutcome) co_return ApiKeyUsageReservationAttemptDecision{.decision = std::move(*fullOutcome)};
        co_return ApiKeyUsageReservationAttemptReclaimed{};
      }
      const contracts::ApiKeyUsageWritePlan& write = std::get<contracts::ApiKeyUsageWritePlan>(plan);
      const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(policyEntry)
        .check(windowEntry)
        .check(requestEntry)
        .set(windowKey, rt::KvValue(json::value_from(write.window)), rt::KvSetOptions{.expireIn = write.expireIn})
        .set(requestKey, rt::KvValue(json::value_from(write.request)), rt::KvSetOptions{.expireIn = write.expireIn});
      const rt::KvCommitResult committed = co_await operation->commit();
      if (!committed.ok) co_return ApiKeyUsageReservationAttemptRetry{};
      co_return ApiKeyUsageReservationAttemptDecision{
        .decision = ApiKeyUsageReservationDecisionOk{.reservation = reservationContext(kv, *livePolicy, requestId, route)},
      };
    }

    // The body `withApiKeyUsageWindowLock` runs for reserveApiKeyUsageV3.
    rt::Task<ApiKeyUsageReservationDecision> reserveUnderLock(std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy, std::string requestId, std::string route,
                                                              std::optional<double> fixedNowMs, bool deferWhenFull) {
      bool reclaimedExpiredReservations = false;
      for (double attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
        const double nowMs = fixedNowMs ? *fixedNowMs : rt::dateNow();
        const rt::Promise<ApiKeyUsageReservationAttempt> attempting(
          attemptApiKeyUsageReservation(kv, policy, requestId, route, nowMs, reclaimedExpiredReservations, deferWhenFull));
        ApiKeyUsageReservationAttempt attemptOutcome = co_await attempting.wait();
        if (auto* decided = std::get_if<ApiKeyUsageReservationAttemptDecision>(&attemptOutcome)) {
          ApiKeyUsageReservationDecision decision = std::move(decided->decision);
          co_return decision;
        }
        // "reclaimed" retries with expired leases already reclaimed; "retry" retries unchanged.
        if (std::holds_alternative<ApiKeyUsageReservationAttemptReclaimed>(attemptOutcome)) reclaimedExpiredReservations = true;
      }
      co_return quotaUnavailableDecision("API key quota ledger changed concurrently");
    }

    rt::Task<std::shared_ptr<rt::Kv>> resolveKvOption(std::optional<std::shared_ptr<rt::Kv>> kv) {
      if (kv) co_return *kv;
      const rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
      std::shared_ptr<rt::Kv> opened = co_await opening.wait();
      co_return opened;
    }

    ApiKeyPolicyDecision invalidApiKey() { return ApiKeyPolicyDecisionNotOk{.response = http::openaiError(401, "Unauthorized", std::string("invalid_api_key"))}; }
  }

  double apiKeyUsageV3RetentionMs(double windowResetAtMs) { return apiKeyUsageV3RetentionMs(windowResetAtMs, rt::dateNow()); }
  double apiKeyUsageV3RetentionMs(double windowResetAtMs, double nowMs) { return native_api_key::apiKeyUsageV3RetentionMs(windowResetAtMs, nowMs); }

  ::uos::types::ApiKeyUsageWindowV3 makeApiKeyUsageWindowV3(const ApiKeyPolicy& policy) { return makeApiKeyUsageWindowV3(policy, rt::dateNow()); }
  ::uos::types::ApiKeyUsageWindowV3 makeApiKeyUsageWindowV3(const ApiKeyPolicy& policy, double nowMs) { return native_api_key::makeApiKeyUsageWindowV3(policy, nowMs); }

  rt::KvKey apiKeyUsageV2Key(const ApiKeyPolicy& policy) {
    const std::string version = rt::startsWith(policy.policy_version, "v3:") ? rt::slice(policy.policy_version, 3) : policy.policy_version;
    return rt::KvKey{API_KEY_USAGE_V2_PREFIX, policy.key_id, version, policy.window_start_ms};
  }

  rt::KvKey apiKeyUsageV3WindowKey(const ApiKeyPolicy& policy) {
    return rt::KvKey{API_KEY_USAGE_V3_WINDOW_PREFIX, policy.key_id, policy.policy_version, policy.window_start_ms};
  }

  rt::KvKey apiKeyUsageV3RequestKey(const ApiKeyPolicy& policy, const std::string& requestId) {
    return rt::KvKey{API_KEY_USAGE_V3_REQUEST_PREFIX, policy.key_id, policy.policy_version, policy.window_start_ms, requestId};
  }

  rt::OrderedMap<std::string, std::string> apiKeyRateLimitPolicyHeaders(const ApiKeyPolicy* policy) {
    rt::OrderedMap<std::string, std::string> headers;
    if (policy == nullptr || policy->usage_limit_requests == API_KEY_NO_USAGE_LIMIT) return headers;
    headers.set("RateLimit-Policy", "\"api-key\";q=" + rt::numberToString(policy->usage_limit_requests) +
                                      ";w=" + rt::numberToString(rt::Math::max(1.0, rt::Math::ceil(policy->window_ms / 1000))));
    return headers;
  }

  rt::Task<void> reclaimExpiredApiKeyUsageReservationsV3(std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy) {
    rt::Task<void> reclaiming = reclaimExpiredApiKeyUsageReservationsV3(std::move(kv), std::move(policy), rt::dateNow());
    co_await std::move(reclaiming);
  }

  rt::Task<void> reclaimExpiredApiKeyUsageReservationsV3(std::shared_ptr<rt::Kv> kv, ApiKeyPolicy policy, double nowMs) {
    // `() => reclaimExpiredApiKeyUsageReservationsV3Unlocked(kv, policy, nowMs)`
    LockedOperation<std::monostate> operation = [kv, policy, nowMs]() { return reclaimExpiredApiKeyUsageReservationsV3Unlocked(kv, policy, nowMs); };
    const rt::Promise<std::monostate> locked(withApiKeyUsageWindowLock<std::monostate>(policy, std::move(operation)));
    co_await locked.wait();
  }

  rt::Task<ApiKeyUsageReservationDecision> reserveApiKeyUsageV3(ApiKeyPolicy policy, std::string requestId, std::string route) {
    rt::Task<ApiKeyUsageReservationDecision> reserving = reserveApiKeyUsageV3(std::move(policy), std::move(requestId), std::move(route), ReserveApiKeyUsageV3Options{});
    ApiKeyUsageReservationDecision decision = co_await std::move(reserving);
    co_return decision;
  }

  rt::Task<ApiKeyUsageReservationDecision> reserveApiKeyUsageV3(ApiKeyPolicy policy, std::string requestId, std::string route, ReserveApiKeyUsageV3Options options) {
    rt::Reason failure;
    try {
      const rt::Promise<std::shared_ptr<rt::Kv>> resolving(resolveKvOption(options.kv));
      const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
      if (!kv) co_return quotaUnavailableDecision();
      if (requestId.empty() || route.empty()) co_return quotaUnavailableDecision("API key quota reservation requires a request id and route");

      const std::optional<double> fixedNowMs = options.nowMs;
      const bool deferWhenFull = options.deferWhenFull == true;
      LockedOperation<ApiKeyUsageReservationDecision> operation = [kv, policy, requestId, route, fixedNowMs, deferWhenFull]() {
        return reserveUnderLock(kv, policy, requestId, route, fixedNowMs, deferWhenFull);
      };
      const rt::Promise<ApiKeyUsageReservationDecision> locked(withApiKeyUsageWindowLock<ApiKeyUsageReservationDecision>(policy, std::move(operation)));
      ApiKeyUsageReservationDecision decision = co_await locked.wait();
      co_return decision;
    } catch (...) {
      failure = std::current_exception();
    }
    rt::console::warn("[ai.ubq.fi] Failed to reserve API key quota:", failure);
    co_return quotaUnavailableDecision();
  }

  rt::Task<double> getApiKeyUsageV3(ApiKeyPolicy policy) {
    rt::Task<double> reading = getApiKeyUsageV3(std::move(policy), std::nullopt);
    const double usage = co_await std::move(reading);
    co_return usage;
  }

  rt::Task<double> getApiKeyUsageV3(ApiKeyPolicy policy, std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    const rt::Promise<std::shared_ptr<rt::Kv>> resolving(resolveKvOption(std::move(kvOverride)));
    const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
    if (!kv) co_return 0;
    const rt::KvGetOptions strong = strongRead();
    const rt::KvKey windowKey = apiKeyUsageV3WindowKey(policy);
    const rt::KvEntryMaybe<> entry = co_await kv->get(windowKey, strong);
    const std::optional<ApiKeyUsageWindowV3> window = native_api_key::matchingApiKeyUsageWindowV3(rt::storedJsonView(entry.value, STORED_JSON_VIEW_OPTIONS), policy);
    co_return window ? window->committed_requests : 0;
  }

  rt::Task<bool> hasLiveApiKeyUsageReservationsV3(std::shared_ptr<rt::Kv> kv, std::string keyId) {
    rt::Task<bool> checking = hasLiveApiKeyUsageReservationsV3(std::move(kv), std::move(keyId), rt::dateNow());
    const bool live = co_await std::move(checking);
    co_return live;
  }

  rt::Task<bool> hasLiveApiKeyUsageReservationsV3(std::shared_ptr<rt::Kv> kv, std::string keyId, double nowMs) {
    const rt::KvKey prefix{API_KEY_USAGE_V3_REQUEST_PREFIX, keyId};
    const std::unique_ptr<rt::KvListIterator> iterator = kv->list(prefixSelector(prefix));
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      const std::optional<::uos::types::ApiKeyUsageRequestV3> request = native_api_key::normalizeApiKeyUsageRequestV3(rt::storedJsonView(entry->value, STORED_JSON_VIEW_OPTIONS));
      if (request && request->state == "reserved" && request->lease_expires_at_ms > nowMs) {
        co_await iterator->return_();
        co_return true;
      }
    }
    co_return false;
  }

  rt::Task<void> reclaimApiKeyUsageReservationsForKeyV3(std::shared_ptr<rt::Kv> kv, std::string keyId) {
    rt::Task<void> reclaiming = reclaimApiKeyUsageReservationsForKeyV3(std::move(kv), std::move(keyId), rt::dateNow());
    co_await std::move(reclaiming);
  }

  rt::Task<void> reclaimApiKeyUsageReservationsForKeyV3(std::shared_ptr<rt::Kv> kv, std::string keyId, double nowMs) {
    rt::OrderedMap<std::string, ApiKeyPolicy> policies;
    const rt::KvGetOptions strong = strongRead();
    const std::size_t prefixLength = API_KEY_USAGE_V3_REQUEST_PREFIX.size();
    const rt::KvKey prefix{API_KEY_USAGE_V3_REQUEST_PREFIX, keyId};
    const std::unique_ptr<rt::KvListIterator> iterator = kv->list(prefixSelector(prefix));
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      const std::optional<::uos::types::ApiKeyUsageRequestV3> request = native_api_key::normalizeApiKeyUsageRequestV3(rt::storedJsonView(entry->value, STORED_JSON_VIEW_OPTIONS));
      if (!request || request->state != "reserved" || request->lease_expires_at_ms > nowMs) continue;
      const rt::KvKey& key = entry->key;
      const rt::KvKeyPart* policyVersionValue = key.size() > prefixLength + 1 ? &key[prefixLength + 1] : nullptr;
      const rt::KvKeyPart* windowStartValue = key.size() > prefixLength + 2 ? &key[prefixLength + 2] : nullptr;
      if (policyVersionValue == nullptr || !policyVersionValue->isString() || windowStartValue == nullptr || !windowStartValue->isNumber() ||
          !native_api_key::isSafeNonNegativeInteger(windowStartValue->number())) {
        continue;
      }
      // Only fields used by releaseReservedRequest are required here.
      ApiKeyPolicy policy{
        .token_hash = "",
        .key_id = keyId,
        .expires_at_ms = API_KEY_NO_EXPIRATION_MS,
        .usage_limit_requests = API_KEY_NO_USAGE_LIMIT,
        .window_ms = 1,
        .window_start_ms = windowStartValue->number(),
        .usage_reset_at_ms = 0,
        .policy_version = policyVersionValue->string(),
        .paid_fallback_enabled = false,
        .paid_fallback_limit_microcredits = 0,
        .paid_fallback_spent_microcredits = 0,
        .paid_fallback_reserved_microcredits = 0,
        .paid_fallback_reservation_request_id = std::nullopt,
      };
      const rt::KvKey windowKey = apiKeyUsageV3WindowKey(policy);
      const rt::KvEntryMaybe<> windowEntry = co_await kv->get(windowKey, strong);
      const std::optional<ApiKeyUsageWindowV3> window = native_api_key::normalizeApiKeyUsageWindowV3(rt::storedJsonView(windowEntry.value, STORED_JSON_VIEW_OPTIONS));
      if (!window) continue;
      ApiKeyPolicy windowPolicy = policy;
      windowPolicy.usage_reset_at_ms = window->window_reset_at_ms;
      policies.set(policy.policy_version + ":" + rt::numberToString(policy.window_start_ms), std::move(windowPolicy));
    }
    for (const ApiKeyPolicy& policy : policies.values()) {
      const rt::Promise<void> reclaiming(reclaimExpiredApiKeyUsageReservationsV3(kv, policy, nowMs));
      co_await reclaiming.wait();
    }
  }

  rt::Task<void> deleteApiKeyUsageV3(std::shared_ptr<rt::Kv> kv, std::string keyId) {
    std::vector<rt::KvKey> keys;
    const rt::KvKey windowPrefix{API_KEY_USAGE_V3_WINDOW_PREFIX, keyId};
    const std::unique_ptr<rt::KvListIterator> windows = kv->list(prefixSelector(windowPrefix));
    while (std::optional<rt::KvEntry<>> entry = co_await windows->next()) keys.push_back(entry->key);
    const rt::KvKey requestPrefix{API_KEY_USAGE_V3_REQUEST_PREFIX, keyId};
    const std::unique_ptr<rt::KvListIterator> requests = kv->list(prefixSelector(requestPrefix));
    while (std::optional<rt::KvEntry<>> entry = co_await requests->next()) keys.push_back(entry->key);
    for (const rt::KvKey& key : keys) co_await kv->delete_(key);
  }

  rt::Task<ApiKeyPolicyDecision> authenticateApiKeyToken(std::string token) {
    rt::Task<ApiKeyPolicyDecision> authenticating = authenticateApiKeyToken(std::move(token), AuthenticateApiKeyTokenOptions{});
    ApiKeyPolicyDecision decision = co_await std::move(authenticating);
    co_return decision;
  }

  rt::Task<ApiKeyPolicyDecision> authenticateApiKeyToken(std::string token, AuthenticateApiKeyTokenOptions options) {
    if (!looksLikeUosApiKey(token)) co_return invalidApiKey();
    const double nowMs = options.nowMs ? *options.nowMs : rt::dateNow();
    const rt::Promise<std::string> hashing(utils::sha256Base64Url(token));
    const std::string tokenHash = co_await hashing.wait();
    const rt::Promise<std::shared_ptr<rt::Kv>> resolving(resolveKvOption(options.kv));
    const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
    if (!kv) co_return invalidApiKey();

    const CachedPolicy* cachedPolicy = policyCache().get(tokenHash);
    std::optional<ApiKeyPolicy> policy = cachedPolicy != nullptr && cachedPolicy->expires_at_ms > nowMs ? std::optional<ApiKeyPolicy>(cachedPolicy->policy) : std::nullopt;
    if (!policy) {
      erasePolicy(tokenHash);
      const rt::KvGetOptions strong = strongRead();
      const rt::KvKey hashKey = api_keys::apiKeyHashKey(tokenHash);
      const rt::KvEntryMaybe<> entry = co_await kv->get(hashKey, strong);
      const json::value record = rt::storedJsonView(entry.value, STORED_JSON_VIEW_OPTIONS);
      if (revokedOrMissing(record)) co_return invalidApiKey();
      policy = apiKeyPolicyFromHashRecord(tokenHash, record, nowMs);
      if (!policy) {
        http::OpenaiErrorOptions errorOptions;
        errorOptions.type = "server_error";
        co_return ApiKeyPolicyDecisionNotOk{
          .response = http::openaiError(503, "API key quota migration is incomplete", std::string("server_error"), std::move(errorOptions)),
        };
      }
      cachePolicy(tokenHash, CachedPolicy{.policy = *policy, .expires_at_ms = nowMs + API_KEY_POLICY_CACHE_TTL_MS});
    } else if (nowMs >= policy->usage_reset_at_ms) {
      const native_api_key::ApiKeyUsageWindowBounds window = native_api_key::currentApiKeyUsageWindow(policy->usage_reset_at_ms, policy->window_ms, nowMs);
      policy->window_start_ms = window.start;
      policy->usage_reset_at_ms = window.reset;
      cachePolicy(tokenHash, CachedPolicy{.policy = *policy, .expires_at_ms = nowMs + API_KEY_POLICY_CACHE_TTL_MS});
    }

    if (policy->expires_at_ms != API_KEY_NO_EXPIRATION_MS && policy->expires_at_ms <= nowMs) {
      erasePolicy(tokenHash);
      co_return invalidApiKey();
    }
    // Authentication remains quota-free so non-inference endpoints (including
    // /v1/models) never depend on an aggregate read or an inference reservation.
    // /uos/auth performs its own committed-usage projection after auth succeeds.
    co_return ApiKeyPolicyDecisionOk{.policy = std::move(*policy)};
  }

  void invalidateApiKeyPolicy() {
    policyCache().clear();
    policyCacheBytes = 0;
    rt::memory::setCache(rt::memory::Cache::api_key_policy, 0, 0);
  }

  void invalidateApiKeyPolicy(const std::string& keyId) {
    if (keyId.empty()) {
      invalidateApiKeyPolicy();
      return;
    }
    for (auto& [hash, cached] : policyCache()) {
      if (cached.policy.key_id == keyId) erasePolicy(hash);
    }
  }

  void resetApiKeyPolicyCacheForTest() { invalidateApiKeyPolicy(); }

  std::optional<double> apiKeyQuotaUsedPercent(const ApiKeyPolicy* policy) {
    if (policy == nullptr || !policy->paid_fallback_enabled || policy->paid_fallback_limit_microcredits <= 0) return std::nullopt;
    const double used = policy->paid_fallback_spent_microcredits + policy->paid_fallback_reserved_microcredits;
    return rt::Math::min(100.0, rt::Math::max(0.0, (used * 100) / policy->paid_fallback_limit_microcredits));
  }
}
