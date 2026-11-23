// Codex auth refresh coordination, split out of src/codex.ts.
#include "uos/codex/auth_refresh.hpp"

#include "uos/bounded_response_body.hpp"
#include "uos/codex/native_auth.hpp"
#include "uos/provider/health.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/utils.hpp"

#include <array>
#include <exception>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace uos::codex::auth_refresh {
  namespace {
    using auth::CodexError;
    using ::uos::types::CodexAuthPoolState;
    using ::uos::types::CodexAuthState;

    const rt::KvGetOptions STRONG_READ{.consistency = rt::KvConsistencyLevel::strong};

    double refreshFailureStatus(double status) { return status == 400 || status == 401 || status == 403 ? 401 : 503; }

    // `Readonly<{ message: string; code: "codex_auth_refresh_failed" | "refresh_token_reused"; status: number }>`
    struct CodexRefreshFailure {
      std::string message;
      auth::CodexErrorCode code;
      double status;
    };

    /**
     * OAuth error bodies are advisory only; the status stays authoritative. The
     * recognized fields are collected in a stable order so the classifier below
     * can match an exact token or a described failure.
     */
    std::vector<std::string> readCodexRefreshFailureValues(std::span<const std::uint8_t> bytes) {
      json::value parsed;
      try {
        parsed = rt::jsonParse(rt::decodeUtf8(bytes));
      } catch (...) {
        // OAuth error bodies are advisory only; the status remains authoritative.
        return {};
      }
      if (!utils::isRecord(parsed)) return {};
      std::vector<std::string> values;
      const json::value* error = rt::get(parsed, "error");
      if (error != nullptr && error->is_string()) values.push_back(std::string(error->get_string()));
      if (utils::isRecord(error)) {
        for (const std::string_view key : std::array<std::string_view, 3>{"code", "message", "type"}) {
          const std::optional<std::string> value = utils::getString(rt::get(error, key));
          if (rt::truthy(value)) values.push_back(*value);
        }
      }
      for (const std::string_view key : std::array<std::string_view, 5>{"error_description", "detail", "message", "code", "type"}) {
        const std::optional<std::string> value = utils::getString(rt::get(parsed, key));
        if (rt::truthy(value)) values.push_back(*value);
      }
      return values;
    }

    bool someValueIs(const std::vector<std::string>& values, std::string_view token) {
      for (const std::string& value : values)
        if (rt::toLowerCase(rt::trim(value)) == token) return true;
      return false;
    }

    // A rotated or shared refresh token is reported either as an exact code or as
    // prose in the provider's error detail; both shapes mean re-authentication.
    bool codexRefreshTokenReused(const std::vector<std::string>& values, const std::string& detail) {
      static const rt::Regex REUSED("refresh token.{0,80}(already|previously) used|token.{0,40}reused", "");
      return someValueIs(values, "refresh_token_reused") || REUSED.test(detail);
    }

    bool codexInvalidGrant(const std::vector<std::string>& values, const std::string& detail) {
      static const rt::Regex INVALID("refresh token.{0,80}(expired|invalid|revoked)|authorization grant.{0,80}(expired|invalid|revoked)", "");
      return someValueIs(values, "invalid_grant") || INVALID.test(detail);
    }

    rt::Task<CodexRefreshFailure> classifyCodexRefreshFailure(rt::Response response) {
      bounded_response_body::ReadBoundedResponseBodyOptions options;
      options.maxBytes = 16 * 1024;
      options.timeoutMs = 1'000;
      options.cancellationReason = json::value("Codex auth refresh error body discarded");
      rt::Promise<bounded_response_body::BoundedResponseBody> reading(bounded_response_body::readBoundedResponseBody(response, options));
      const bounded_response_body::BoundedResponseBody bounded = co_await reading.wait();
      const std::vector<std::string> values =
        bounded.complete && !bounded.bytes.empty() ? readCodexRefreshFailureValues(bounded.bytes) : std::vector<std::string>{};
      const std::string detail = rt::toLowerCase(rt::join(values, " "));
      const double status = static_cast<double>(response.status());
      CodexRefreshFailure failure;
      if (codexRefreshTokenReused(values, detail)) {
        failure.message = "The gateway's Codex refresh token was already used. Sign in again or upload a fresh auth.json and retry.";
        failure.code = "refresh_token_reused";
        failure.status = refreshFailureStatus(status);
        co_return failure;
      }
      const bool invalidGrant = codexInvalidGrant(values, detail);
      if (invalidGrant || status == 400 || status == 401 || status == 403) {
        failure.message = std::string(auth::CODEX_AUTH_REAUTH_MESSAGE) + " The provider rejected the configured refresh token.";
        failure.code = "codex_auth_refresh_failed";
        failure.status = refreshFailureStatus(status);
        co_return failure;
      }
      failure.message = "Codex auth refresh failed (status " + rt::numberToString(status) + ").";
      failure.code = "codex_auth_refresh_failed";
      failure.status = 503;
      co_return failure;
    }

    /**
     * A concurrent refresh inside this isolate may already have persisted a newer
     * token for the account. Adopting it avoids overwriting a rotation this request
     * never observed.
     */
    std::optional<CodexAuthState> adoptConcurrentRefreshedAuth(const CodexAuthState& authState, double generationAtStart) {
      if (auth::authCacheGeneration == generationAtStart || !auth::cachedAuthPool) return std::nullopt;
      for (const CodexAuthState& cached : auth::cachedAuthPool->accounts) {
        if (cached.account_id != authState.account_id) continue;
        if (cached.access_token == authState.access_token && cached.refresh_token == authState.refresh_token) return std::nullopt;
        return cached;
      }
      return std::nullopt;
    }

    rt::Task<rt::KvEntryMaybe<json::value>> readPoolEntry(std::shared_ptr<rt::Kv> kv) {
      auto reading = kv->get(auth::CODEX_AUTH_POOL_KV_KEY, STRONG_READ);
      const rt::KvEntryMaybe<> stored = co_await std::move(reading);
      co_return stored.as<json::value>();
    }

    std::optional<std::size_t> accountIndex(const CodexAuthPoolState& pool, const std::string& accountId) {
      for (std::size_t index = 0; index < pool.accounts.size(); ++index)
        if (pool.accounts[index].account_id == accountId) return index;
      return std::nullopt;
    }

    /**
     * Persist a refreshed token with optimistic concurrency. Every attempt
     * re-reads and re-validates the pool, adopts a token another writer already
     * rotated, and retries the compare-and-set while the pool keeps changing.
     */
    rt::Task<CodexAuthState> persistRefreshedAuthAccount(auth::CodexAuthAccountEntry current, CodexAuthState next, std::shared_ptr<rt::Kv> kv) {
      std::optional<rt::KvEntryMaybe<json::value>> poolEntry = current.entry;
      for (int attempt = 0; attempt < 3; attempt += 1) {
        if (!poolEntry) {
          auto reading = readPoolEntry(kv);
          poolEntry = co_await std::move(reading);
        }
        const std::optional<CodexAuthPoolState> latestPool = auth::parseCodexAuthPool(poolEntry->value);
        const std::optional<std::size_t> latestIndex = latestPool ? accountIndex(*latestPool, current.auth.account_id) : std::nullopt;
        if (!latestPool || !latestIndex) throw CodexError("Codex auth account disappeared during refresh.", "codex_auth_missing", 503);
        const CodexAuthState latestAuth = latestPool->accounts[*latestIndex];
        if (latestAuth.access_token != current.auth.access_token || latestAuth.refresh_token != current.auth.refresh_token) {
          auth::cacheCodexAuthPool(*latestPool);
          co_return latestAuth;
        }

        std::vector<CodexAuthState> accounts = latestPool->accounts;
        accounts[*latestIndex] = next;
        const CodexAuthPoolState nextPool{.accounts = std::move(accounts), .updated_at_ms = rt::dateNow()};
        const rt::KvValue row(json::value_from(nextPool));
        std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        operation->check(*poolEntry).set(auth::CODEX_AUTH_POOL_KV_KEY, row);
        auto committing = operation->commit();
        const rt::KvCommitResult commit = co_await std::move(committing);
        if (commit.ok) {
          auth::cacheCodexAuthPool(nextPool);
          co_return next;
        }
        auto rereading = readPoolEntry(kv);
        poolEntry = co_await std::move(rereading);
      }
      throw CodexError("Codex auth refresh could not persist after concurrent updates.", "codex_auth_refresh_failed", 503);
    }

    // The `fetch(CODEX_REFRESH_TOKEN_URL, { method: "POST", headers, body, signal: AbortSignal.timeout(10_000) })` init.
    rt::RequestInit refreshTokenRequest(const std::string& refreshToken) {
      json::object body;
      body["client_id"] = auth::CODEX_REFRESH_CLIENT_ID;
      body["grant_type"] = "refresh_token";
      body["refresh_token"] = refreshToken;
      body["scope"] = "openid profile email";
      rt::RequestInit init;
      init.method = "POST";
      init.headers = rt::Headers{{"Content-Type", "application/json"}, {"Accept", "application/json"}};
      init.body = rt::BodyInit(rt::jsonStringify(json::value(std::move(body))));
      init.signal = rt::AbortSignal::timeout(10'000);
      return init;
    }

    // `(await response.json().catch(() => null)) as null | Record<string, unknown>`, then
    // `parsed && getString(parsed.<key>)`: a falsy body is returned as no token.
    std::optional<std::string> parsedToken(const json::value& parsed, std::string_view key) {
      return rt::truthy(parsed) ? utils::getString(rt::get(parsed, key)) : std::nullopt;
    }

    rt::Task<CodexAuthState> refreshAuth(auth::CodexAuthAccountEntry current) {
      auth::requireCodexAuthRefreshAllowed();
      const double generationAtStart = auth::authCacheGeneration;
      rt::Response response;
      rt::Reason unreachable;
      try {
        rt::RequestInit init = refreshTokenRequest(current.auth.refresh_token);
        const rt::RequestInfo input{std::string(auth::CODEX_REFRESH_TOKEN_URL)};
        auto fetching = rt::fetch(input, std::move(init));
        response = co_await std::move(fetching);
      } catch (...) {
        unreachable = std::current_exception();
      }
      if (unreachable) throw CodexError("Codex auth refresh failed: auth server unreachable.", "codex_auth_refresh_unreachable", 502, unreachable);

      if (!response.ok()) {
        rt::Promise<CodexRefreshFailure> classifying(classifyCodexRefreshFailure(response));
        const CodexRefreshFailure failure = co_await classifying.wait();
        throw CodexError(failure.message, failure.code, failure.status);
      }

      json::value parsed(nullptr);
      try {
        auto reading = response.json();
        parsed = co_await std::move(reading);
      } catch (...) {
        parsed = nullptr;
      }
      const std::optional<std::string> accessToken = parsedToken(parsed, "access_token");
      const std::optional<std::string> refreshToken = parsedToken(parsed, "refresh_token");
      if (!rt::truthy(accessToken)) {
        throw CodexError("Codex auth refresh failed: upstream response missing access_token.", "codex_auth_refresh_failed", 503);
      }

      const CodexAuthState next{.access_token = *accessToken,
                                .refresh_token = refreshToken.value_or(current.auth.refresh_token),
                                .account_id = current.auth.account_id,
                                .updated_at_ms = rt::dateNow()};

      const std::optional<CodexAuthState> adopted = adoptConcurrentRefreshedAuth(current.auth, generationAtStart);
      if (adopted) co_return *adopted;

      const std::shared_ptr<rt::Kv> kv = current.kv;
      if (kv) {
        rt::Promise<CodexAuthState> persisting(persistRefreshedAuthAccount(current, next, kv));
        const CodexAuthState persisted = co_await persisting.wait();
        co_return persisted;
      }

      const CodexAuthPoolState basePool = auth::cachedAuthPool ? *auth::cachedAuthPool : current.pool;
      std::vector<CodexAuthState> accounts;
      for (const CodexAuthState& candidate : basePool.accounts) accounts.push_back(candidate.account_id == current.auth.account_id ? next : candidate);
      auth::cacheCodexAuthPool(CodexAuthPoolState{.accounts = std::move(accounts), .updated_at_ms = rt::dateNow()});
      co_return next;
    }

    rt::Task<CodexAuthState> refreshAuthWithHealth(auth::CodexAuthAccountEntry current) {
      std::optional<CodexAuthState> refreshed;
      rt::Reason failure;
      try {
        rt::Promise<CodexAuthState> refreshing(refreshAuth(current));
        refreshed = co_await refreshing.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      if (!failure) {
        (void)provider::health::recordCodexProviderHealth(current.auth.account_id, "refresh_success", 200);
        co_return *refreshed;
      }
      const CodexError* codexError = auth::codexErrorOf(failure);
      const std::optional<double> status = codexError != nullptr ? std::optional<double>(codexError->status) : std::nullopt;
      (void)provider::health::recordCodexProviderHealth(current.auth.account_id, "refresh_failed", status);
      std::rethrow_exception(failure);
    }

    rt::Task<rt::KvKey> refreshLeaseKey(std::string accountId) {
      rt::Promise<std::string> hashing(utils::sha256Hex(accountId));
      const std::string hash = co_await hashing.wait();
      co_return rt::KvKey{auth::CODEX_AUTH_REFRESH_LEASE_PREFIX, hash};
    }

    rt::Task<void> releaseRefreshLease(std::shared_ptr<rt::Kv> kv, rt::KvKey key, std::string owner) {
      try {
        auto reading = kv->get(key, STRONG_READ);
        const rt::KvEntryMaybe<> entry = co_await std::move(reading);
        const json::value value = entry.value.toJson();
        const json::value* leaseOwner = rt::get(value, "owner");
        if (!(leaseOwner != nullptr && leaseOwner->is_string() && leaseOwner->get_string() == owner)) co_return;
        std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        operation->check(entry).delete_(key);
        auto committing = operation->commit();
        co_await std::move(committing);
      } catch (...) {
        // Leases expire on their own; a release failure must not mask refresh work.
      }
    }

    struct LeaseAdopted {
      CodexAuthState auth;
    };
    struct LeaseOwned {
      std::string owner;
      auth::CodexAuthAccountEntry entry;
    };
    struct LeaseUnclaimed {
      auth::CodexAuthAccountEntry entry;
    };
    struct LeaseReleased {
      auth::CodexAuthAccountEntry entry;
    };
    // `{ kind: "adopted"; auth } | { kind: "owned"; owner; entry } | { kind: "unclaimed"; entry }`
    using CodexRefreshLeaseAcquisition = std::variant<LeaseAdopted, LeaseOwned, LeaseUnclaimed>;
    // `{ kind: "adopted"; auth } | { kind: "released"; entry }`
    using CodexRefreshLeaseWait = std::variant<LeaseAdopted, LeaseReleased>;

    /**
     * A lease owner may have persisted rotating credentials immediately before
     * releasing its lease. Re-read once at that handoff so this waiter never
     * refreshes an obsolete token.
     */
    rt::Task<CodexRefreshLeaseWait> adoptCodexCredentialsAtLeaseHandoff(auth::CodexAuthAccountEntry current) {
      try {
        rt::Promise<auth::CodexAuthAccountEntry> reading(getCurrentAccountEntry(current.auth.account_id, true));
        const auth::CodexAuthAccountEntry newest = co_await reading.wait();
        if (!sameCodexCredentials(newest.auth, current.auth)) {
          LeaseAdopted adopted{newest.auth};
          co_return adopted;
        }
        LeaseReleased released{newest};
        co_return released;
      } catch (...) {
        // The normal fail-open refresh remains available if KV is transiently
        // unavailable at the handoff.
      }
      LeaseReleased released{current};
      co_return released;
    }

    /**
     * Observe one live lease. The wait is bounded by the remaining lease window and
     * is deliberately not bound to an individual request signal: callers race the
     * resulting shared refresh independently, so one aborted client cannot cancel a
     * refresh another client still needs.
     */
    // `lease`: the stored lease row as read (`CodexRefreshLease` is an unchecked cast in the TS).
    rt::Task<CodexRefreshLeaseWait> waitForCodexRefreshLeaseHandoff(std::shared_ptr<rt::Kv> kv, rt::KvKey key, json::value lease, double nowMs,
                                                                   auth::CodexAuthAccountEntry initial) {
      auth::CodexAuthAccountEntry current = initial;
      double waitedMs = 0;
      double stepMs = 50;
      const double maxWaitMs = rt::Math::min(auth::CODEX_AUTH_REFRESH_WAIT_MS, rt::Math::max(0.0, rt::toNumber(rt::get(lease, "lease_until_ms")) - nowMs));
      while (waitedMs < maxWaitMs) {
        co_await delay(stepMs);
        waitedMs += stepMs;
        stepMs = rt::Math::min(500.0, stepMs * 2);
        try {
          rt::Promise<auth::CodexAuthAccountEntry> reading(getCurrentAccountEntry(current.auth.account_id, true));
          const auth::CodexAuthAccountEntry newest = co_await reading.wait();
          if (!sameCodexCredentials(newest.auth, current.auth)) {
            LeaseAdopted adopted{newest.auth};
            co_return adopted;
          }
          current = newest;
        } catch (...) {
          // The current owner may still persist a refreshed token. Keep waiting
          // until the bounded lease observation window elapses.
        }
        auto leasing = kv->get(key, STRONG_READ);
        const rt::KvEntryMaybe<> currentLease = co_await std::move(leasing);
        const json::value currentLeaseValue = currentLease.value.toJson();
        if (!rt::truthy(currentLeaseValue) || rt::toNumber(rt::get(currentLeaseValue, "lease_until_ms")) <= rt::dateNow()) {
          rt::Promise<CodexRefreshLeaseWait> handoff(adoptCodexCredentialsAtLeaseHandoff(current));
          const CodexRefreshLeaseWait waited = co_await handoff.wait();
          co_return waited;
        }
      }
      LeaseReleased released{current};
      co_return released;
    }

    /**
     * Claim the cross-isolate refresh lease for this account, or observe the owner
     * that already holds it. Lease reads and CAS operations are an optional
     * coordination optimization: a failure here is reported as `unclaimed` so the
     * caller falls open to a bounded direct refresh. OAuth itself never runs inside
     * this helper, so its deterministic errors are never retried here.
     */
    rt::Task<CodexRefreshLeaseAcquisition> acquireCodexRefreshLease(std::shared_ptr<rt::Kv> kv, rt::KvKey key, auth::CodexAuthAccountEntry initial) {
      auth::CodexAuthAccountEntry current = initial;
      try {
        for (int attempt = 0; attempt < 3; attempt += 1) {
          auto reading = kv->get(key, STRONG_READ);
          const rt::KvEntryMaybe<> leaseEntry = co_await std::move(reading);
          const json::value lease = leaseEntry.value.toJson();
          const double now = rt::dateNow();
          if (!rt::truthy(lease) || rt::toNumber(rt::get(lease, "lease_until_ms")) <= now) {
            const std::string owner = rt::crypto::randomUUID();
            const auth::CodexRefreshLease claim{.owner = owner, .lease_until_ms = now + auth::CODEX_AUTH_REFRESH_LEASE_MS};
            const rt::KvValue row(json::value_from(claim));
            const rt::KvSetOptions expiry{.expireIn = auth::CODEX_AUTH_REFRESH_LEASE_MS * 2};
            std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
            operation->check(leaseEntry).set(key, row, expiry);
            auto committing = operation->commit();
            const rt::KvCommitResult claimed = co_await std::move(committing);
            if (!claimed.ok) continue;
            LeaseOwned owned{owner, current};
            co_return owned;
          }

          rt::Promise<CodexRefreshLeaseWait> waiting(waitForCodexRefreshLeaseHandoff(kv, key, lease, now, current));
          const CodexRefreshLeaseWait wait = co_await waiting.wait();
          if (const LeaseAdopted* adopted = std::get_if<LeaseAdopted>(&wait)) {
            LeaseAdopted result = *adopted;
            co_return result;
          }
          current = std::get<LeaseReleased>(wait).entry;
        }
      } catch (...) {
        LeaseUnclaimed unclaimed{current};
        co_return unclaimed;
      }
      LeaseUnclaimed unclaimed{current};
      co_return unclaimed;
    }

    bool isNullishReason(const rt::Reason& reason) {
      if (!reason) return true;
      const std::optional<json::value> thrown = rt::thrownValue(reason);
      return thrown.has_value() && thrown->is_null();
    }
  }

  rt::Task<auth::CodexAuthAccountEntry> getCurrentAccountEntry(std::string accountId, bool forceKv) {
    rt::Promise<auth::CodexAuthPoolEntry> loading(auth::getAuthPoolEntry(forceKv));
    const auth::CodexAuthPoolEntry poolEntry = co_await loading.wait();
    const std::optional<std::size_t> index = accountIndex(poolEntry.pool, accountId);
    if (!index) throw CodexError("Codex auth account is no longer configured.", "codex_auth_missing", 503);
    auth::CodexAuthAccountEntry entry;
    static_cast<auth::CodexAuthPoolEntry&>(entry) = poolEntry;
    entry.auth = poolEntry.pool.accounts[*index];
    co_return entry;
  }

  bool sameCodexCredentials(const CodexAuthState& left, const CodexAuthState& right) {
    return left.access_token == right.access_token && left.refresh_token == right.refresh_token;
  }

  rt::Task<void> delay(double milliseconds) { co_await rt::sleep(milliseconds); }

  /**
   * Coalesce refreshes across isolates. A waiter repeatedly force-reads the
   * pool and adopts a newer token instead of issuing OAuth itself. The lease
   * contains only a hash-derived slot key and an opaque owner token.
   */
  rt::Task<CodexAuthState> refreshAuthCoordinated(auth::CodexAuthAccountEntry input) {
    if (auth::hasLeaderAuthReader()) {
      auto reading = auth::useLeaderCredential(std::move(input.auth), true);
      const auto adopted = co_await std::move(reading);
      co_return adopted;
    }
    auth::requireCodexAuthRefreshAllowed();
    rt::Promise<std::optional<CodexAuthState>> nativeRefresh(native_auth::refreshNativeCodexAuthIfOwned(input.auth));
    const std::optional<CodexAuthState> native = co_await nativeRefresh.wait();
    if (native) co_return *native;
    auth::CodexAuthAccountEntry current = input;
    try {
      rt::Promise<auth::CodexAuthAccountEntry> reading(getCurrentAccountEntry(input.auth.account_id, true));
      const auth::CodexAuthAccountEntry newest = co_await reading.wait();
      if (!sameCodexCredentials(newest.auth, input.auth)) co_return newest.auth;
      current = newest;
    } catch (...) {
      // Continue with the supplied in-memory account when the optional
      // cross-isolate read is unavailable; refreshAuth still persists safely.
    }

    const std::shared_ptr<rt::Kv> kv = current.kv;
    if (!kv) {
      rt::Promise<CodexAuthState> refreshing(refreshAuthWithHealth(current));
      const CodexAuthState refreshed = co_await refreshing.wait();
      co_return refreshed;
    }
    rt::KvKey key;
    bool keyed = false;
    try {
      rt::Promise<rt::KvKey> keying(refreshLeaseKey(current.auth.account_id));
      key = co_await keying.wait();
      keyed = true;
    } catch (...) {
    }
    if (!keyed) {
      rt::Promise<CodexAuthState> refreshing(refreshAuthWithHealth(current));
      const CodexAuthState refreshed = co_await refreshing.wait();
      co_return refreshed;
    }

    rt::Promise<CodexRefreshLeaseAcquisition> acquiring(acquireCodexRefreshLease(kv, key, current));
    const CodexRefreshLeaseAcquisition lease = co_await acquiring.wait();
    if (const LeaseAdopted* adopted = std::get_if<LeaseAdopted>(&lease)) co_return adopted->auth;
    if (const LeaseOwned* owned = std::get_if<LeaseOwned>(&lease)) {
      std::optional<CodexAuthState> refreshed;
      rt::Reason failure;
      try {
        rt::Promise<CodexAuthState> refreshing(refreshAuthWithHealth(owned->entry));
        refreshed = co_await refreshing.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      rt::Promise<void> releasing(releaseRefreshLease(kv, key, owned->owner));
      co_await releasing.wait();
      if (failure) std::rethrow_exception(failure);
      co_return *refreshed;
    }

    // No usable cross-isolate lease was observed or claimed. The refresh itself
    // remains generation-aware and bounded.
    rt::Promise<CodexAuthState> refreshing(refreshAuthWithHealth(std::get<LeaseUnclaimed>(lease).entry));
    const CodexAuthState refreshed = co_await refreshing.wait();
    co_return refreshed;
  }

  rt::Reason abortReasonAsError(const rt::Reason& reason) {
    if (rt::isError(reason)) return reason;
    if (!reason) return rt::makeReason(rt::DOMException("The request was aborted.", "AbortError"));
    return rt::makeReason(rt::withCause(rt::JsError("The request was aborted."), reason));
  }

  template <class T> rt::Task<T> awaitWithoutCancellingSharedWork(rt::Promise<T> promise, std::optional<rt::AbortSignal> signal) {
    if (!signal) {
      T value = co_await promise.wait();
      co_return value;
    }
    if (signal->aborted()) {
      if (isNullishReason(signal->reason())) throw rt::DOMException("The request was aborted.", "AbortError");
      rt::rethrow(signal->reason());
    }
    const rt::AbortSignal abortSignal = *signal;
    const auto listenerSlot = std::make_shared<rt::EventListener>();
    const auto cleanup = rt::scopeExit([&] { abortSignal.removeEventListener("abort", *listenerSlot); });
    const std::weak_ptr<rt::EventListener> listenerHandle = listenerSlot;
    const std::weak_ptr<rt::detail::AbortSignalState> weakSignal = abortSignal.state();
    rt::Promise<T> abortion = rt::newPromise<T>([&](auto, rt::Reject reject) {
      *listenerSlot = rt::EventListener([weakSignal, listenerHandle, reject]() {
        const auto state = weakSignal.lock();
        if (!state) return;
        const rt::AbortSignal active(state);
        if (const auto listener = listenerHandle.lock()) active.removeEventListener("abort", *listener);
        reject(abortReasonAsError(active.reason()));
      });
      abortSignal.addEventListener("abort", *listenerSlot, rt::AddEventListenerOptions{.once = true});
      (void)promise.finally_([abortSignal, listenerSlot] { abortSignal.removeEventListener("abort", *listenerSlot); }).catch_(rt::ignoreRejection);
    });
    std::vector<rt::Promise<T>> racers{promise, abortion};
    rt::Promise<T> raced = rt::race(std::move(racers));
    T value = co_await raced.wait();
    co_return value;
  }

  template rt::Task<CodexAuthState> awaitWithoutCancellingSharedWork<CodexAuthState>(rt::Promise<CodexAuthState> promise, std::optional<rt::AbortSignal> signal);

  rt::Task<CodexAuthState> refreshAuthStateless(CodexAuthState authState) {
    auth::requireCodexAuthRefreshAllowed();
    rt::Promise<std::optional<CodexAuthState>> nativeRefresh(native_auth::refreshNativeCodexAuthIfOwned(authState, true));
    const std::optional<CodexAuthState> native = co_await nativeRefresh.wait();
    if (native) co_return *native;
    rt::Response response;
    rt::Reason unreachable;
    try {
      rt::RequestInit init = refreshTokenRequest(authState.refresh_token);
      const rt::RequestInfo input{std::string(auth::CODEX_REFRESH_TOKEN_URL)};
      auto fetching = rt::fetch(input, std::move(init));
      response = co_await std::move(fetching);
    } catch (...) {
      unreachable = std::current_exception();
    }
    if (unreachable) throw CodexError("Codex auth refresh failed: auth server unreachable.", "codex_auth_refresh_unreachable", 502, unreachable);

    if (!response.ok()) {
      rt::Promise<CodexRefreshFailure> classifying(classifyCodexRefreshFailure(response));
      const CodexRefreshFailure failure = co_await classifying.wait();
      throw CodexError(failure.message, failure.code, failure.status);
    }

    json::value parsed(nullptr);
    try {
      auto reading = response.json();
      parsed = co_await std::move(reading);
    } catch (...) {
      parsed = nullptr;
    }
    const std::optional<std::string> accessToken = parsedToken(parsed, "access_token");
    const std::optional<std::string> refreshToken = parsedToken(parsed, "refresh_token");
    if (!rt::truthy(accessToken)) {
      throw CodexError("Codex auth refresh failed: upstream response missing access_token.", "codex_auth_refresh_failed", 503);
    }

    const CodexAuthState refreshed{.access_token = *accessToken,
                                   .refresh_token = refreshToken.value_or(authState.refresh_token),
                                   .account_id = authState.account_id,
                                   .updated_at_ms = rt::dateNow()};
    co_return refreshed;
  }

  rt::Task<CodexAuthState> getValidAuth(auth::CodexAuthAccountEntry current) {
    if (!auth::needsRefresh(current.auth)) co_return current.auth;
    if (auth::hasLeaderAuthReader()) {
      auto reading = auth::useLeaderCredential(std::move(current.auth), false);
      const auto adopted = co_await std::move(reading);
      co_return adopted;
    }
    auth::requireCodexAuthRefreshAllowed();

    if (const rt::Promise<CodexAuthState>* existing = auth::refreshesInFlight.get(current.auth.account_id)) {
      const rt::Promise<CodexAuthState> inFlight = *existing;
      const CodexAuthState shared = co_await inFlight.wait();
      co_return shared;
    }
    const std::string accountId = current.auth.account_id;
    const rt::Promise<CodexAuthState> refresh =
      rt::Promise<CodexAuthState>(refreshAuthCoordinated(current)).finally_([accountId] { auth::refreshesInFlight.delete_(accountId); });
    auth::refreshesInFlight.set(accountId, refresh);
    const CodexAuthState refreshed = co_await refresh.wait();
    co_return refreshed;
  }
}
