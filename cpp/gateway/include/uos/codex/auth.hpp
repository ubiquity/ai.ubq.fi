#pragma once
// Port of src/codex/auth.ts.
// Codex auth pool parsing, caching and loading, split out of src/codex.ts.
//
// Part of the w04c1 cycle cluster (headers first). The live `let` bindings and the
// module-level WeakMap/Map/Set exports are `extern`, defined in src/codex/auth.cpp and
// touched only after main starts. `Kv | null` is a std::shared_ptr<rt::Kv> (null is null).
#include "uos/codex/routing_state.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/rt/weak.hpp"
#include "uos/types.hpp"

#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uos::codex::auth {
  inline constexpr std::string_view CODEX_REFRESH_CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann";
  inline constexpr std::string_view CODEX_REFRESH_TOKEN_URL = "https://auth.openai.com/oauth/token";
  inline constexpr std::string_view CODEX_ORIGINATOR = "codex_cli_rs";
  inline constexpr std::string_view CODEX_CLIENT_VERSION = "0.100.0";
  inline constexpr std::string_view CODEX_QUOTA_BLOCKED_ERROR_CODE = "codex_quota_blocked";
  inline constexpr std::string_view CODEX_SUBSCRIPTIONS_UNAVAILABLE_ERROR_CODE = "codex_subscriptions_unavailable";
  inline constexpr std::string_view CODEX_UPSTREAM_DEGRADED_ERROR_CODE = "codex_upstream_degraded";
  inline constexpr std::string_view CODEX_AUTH_REAUTH_WARNING = "codex_auth_reauthentication_required";
  inline constexpr std::string_view CODEX_AUTH_REAUTH_MESSAGE =
    "The gateway's Codex auth.json needs re-authentication. Upload a fresh auth.json and retry.";
  /** Bounded one-account re-admissions after an authoritative transition signal. */
  inline constexpr double CODEX_ACTIVE_ADMISSION_RESEELECTION_LIMIT = 3;

  // Routing errors need to remain distinguishable to the gateway's fallback
  // adapter without adding gateway-only headers to OpenAI-compatible responses.
  // `const codexRoutingErrors = new WeakMap<Response, string>()`.
  extern rt::WeakMap<rt::Response, std::string> codexRoutingErrors;

  // `string | null`
  std::optional<std::string> getCodexRoutingError(const rt::Response& response);

  // `clientVersion?: string | null`: std::nullopt is null and undefined alike.
  std::string codexUserAgent(const std::optional<std::string>& clientVersion = std::nullopt);

  // "codex_auth_missing" | "codex_auth_invalid" | "codex_auth_refresh_failed" | "refresh_token_reused" |
  // "codex_auth_refresh_unreachable" | "codex_auth_owner_unavailable" | "codex_auth_owner_conflict" |
  // "codex_upstream_unreachable" | "gateway_timeout"
  using CodexErrorCode = std::string;

  namespace detail {
    // rt::JsError::cause for a cause that is an Error (a copy of it), else null.
    inline std::shared_ptr<const rt::JsError> codexErrorCause(const rt::Reason& cause) {
      if (!cause) return nullptr;
      try {
        std::rethrow_exception(cause);
      } catch (const rt::JsError& error) {
        return std::make_shared<rt::JsError>(error);
      } catch (...) {
      }
      return nullptr;
    }
  }

  class CodexError : public rt::JsError {
  public:
    // `readonly code: CodexErrorCode`. It hides rt::JsError::code, which the constructor sets to
    // the same text so a reader of a plain rt::JsError sees it too.
    CodexErrorCode code;
    double status;
    // `cause?: unknown` exactly as given; a null Reason is undefined (no `cause` property).
    // rt::JsError::cause also holds it when it is an Error.
    rt::Reason causeValue;

    CodexError(std::string message_, CodexErrorCode code_, double status_, rt::Reason cause_ = nullptr)
      : rt::JsError(std::move(message_), "CodexError", detail::codexErrorCause(cause_)),
        code(std::move(code_)),
        status(status_),
        causeValue(std::move(cause_)) {
      rt::JsError::code = code;
    }
  };

  // Follower staleness uses the ordinary credential failure on the wire.
  class CodexAuthFollowerStaleError : public CodexError {
  public:
    CodexAuthFollowerStaleError() : CodexError(std::string(CODEX_AUTH_REAUTH_MESSAGE), "codex_auth_invalid", 401) {}
  };

  void requireCodexAuthRefreshAllowed();

  // `reason instanceof CodexError` (C++ addition): the CodexError a Reason holds, or nullptr.
  // The pointer lives as long as the Reason.
  inline const CodexError* codexErrorOf(const rt::Reason& reason) noexcept {
    if (!reason) return nullptr;
    try {
      std::rethrow_exception(reason);
    } catch (const CodexError& error) {
      return &error;
    } catch (...) {
    }
    return nullptr;
  }

  inline const rt::KvKey CODEX_AUTH_POOL_KV_KEY{"ubq_ai", "codex_auth"};
  inline const rt::KvKey CODEX_MODELS_KV_KEY{"ubq_ai", "codex_models"};
  inline const rt::KvKey CODEX_AUTH_REFRESH_LEASE_PREFIX{"uos_ai", "codex_auth_refresh", "v1"};
  inline constexpr double CODEX_AUTH_CACHE_TTL_MS = 5 * 60'000;
  inline constexpr double CODEX_AUTH_POOL_MAX_ACCOUNTS = 2;
  inline constexpr double CODEX_AUTH_REFRESH_LEASE_MS = 15'000;
  inline constexpr double CODEX_AUTH_REFRESH_WAIT_MS = 10'000;
  inline constexpr double CODEX_ADDITIONAL_429_RETRY_MAX_DELAY_MS = 2'000;

  // `Omit<CodexAuthState, "updated_at_ms">`. parseCodexAuthFromAuthJson never sets native_owner.
  struct ParseCodexAuthFromAuthJsonResult {
    std::string access_token;
    std::string refresh_token;
    std::string account_id;
    /** Local native Codex owns this account's rotating credential lineage. */
    std::optional<::uos::types::CodexAuthState::NativeOwner> native_owner{};
    friend bool operator==(const ParseCodexAuthFromAuthJsonResult&, const ParseCodexAuthFromAuthJsonResult&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const ParseCodexAuthFromAuthJsonResult& value);
  ParseCodexAuthFromAuthJsonResult tag_invoke(const json::value_to_tag<ParseCodexAuthFromAuthJsonResult>&, const json::value& value);

  // `value: unknown`: nullptr is undefined.
  std::optional<ParseCodexAuthFromAuthJsonResult> parseCodexAuthFromAuthJson(const json::value& value);
  std::optional<ParseCodexAuthFromAuthJsonResult> parseCodexAuthFromAuthJson(const json::value* value);

  // `value: unknown`: nullptr is undefined. Throws CodexError for a malformed native owner, as the TS does.
  std::optional<::uos::types::CodexAuthPoolState> parseCodexAuthPool(const json::value& value);
  std::optional<::uos::types::CodexAuthPoolState> parseCodexAuthPool(const json::value* value);

  // `pool: CodexAuthPoolState | null`; the result is null as std::nullopt.
  std::optional<::uos::types::CodexAuthPoolState> upsertCodexAuthAccount(const std::optional<::uos::types::CodexAuthPoolState>& pool,
                                                                         const ::uos::types::CodexAuthState& auth);

  // `token: unknown`: nullptr is undefined. The std::string_view overload is the typed call
  // (`getJwtExpMs(auth.access_token)`); the array overload takes a string literal, which would otherwise
  // be ambiguous between the json::value and std::string_view overloads.
  std::optional<double> getJwtExpMs(const json::value& token);
  std::optional<double> getJwtExpMs(const json::value* token);
  std::optional<double> getJwtExpMs(std::string_view token);
  template <std::size_t N>
  std::optional<double> getJwtExpMs(const char (&token)[N]) {
    return getJwtExpMs(std::string_view(token, N - 1));
  }

  /** Returns the provider email claim without retaining or exposing the token. */
  std::optional<std::string> getCodexAccountEmail(std::string_view accessToken);

  bool needsRefresh(const ::uos::types::CodexAuthState& auth);

  bool accessTokenExpired(const ::uos::types::CodexAuthState& auth);

  // `error: unknown` (a caught value).
  std::optional<std::string> codexAuthWarningForError(const rt::Reason& error);

  // `type CodexAuthPoolEntry = { kv: Kv | null; entry: KvEntryMaybe<CodexAuthPoolState> | null; pool }`.
  // `entry` holds the auth-pool row as stored, as JSON: the TS type is an unchecked cast, and every
  // reader either uses only key and versionstamp or re-parses the value with parseCodexAuthPool. An
  // entry the TS builds from a pool (`{ key, value: pool, versionstamp }`) holds json::value_from(pool).
  struct CodexAuthPoolEntry {
    std::shared_ptr<rt::Kv> kv;
    std::optional<rt::KvEntryMaybe<json::value>> entry;
    ::uos::types::CodexAuthPoolState pool;
  };

  // `CodexAuthPoolEntry & { auth: CodexAuthState; routing?: RoutingAccount }`
  struct CodexAuthAccountEntry : CodexAuthPoolEntry {
    ::uos::types::CodexAuthState auth;
    std::optional<routing_state::RoutingAccount> routing{};
  };

  /** A routable account entry: `routing` is always present once a slot is selected. */
  // `CodexAuthAccountEntry & { routing: RoutingAccount }`: the base member `routing` is always set on
  // these values (as routing_state::CodexBlockedRoutingAccount does for routingGeneration).
  struct CodexDispatchAccountEntry : CodexAuthAccountEntry {};

  /**
   * Safe response-local active-subscription telemetry. It is captured from the
   * routing account that was actually admitted and never inferred later from
   * mutable slot state. No raw account ids, keys or session metadata.
   */
  struct CodexResponseActiveTelemetry {
    std::optional<double> activeGeneration;
    std::optional<routing_state::CodexActiveAccountTransitionReason> activeTransitionReason;
    friend bool operator==(const CodexResponseActiveTelemetry&, const CodexResponseActiveTelemetry&) = default;
  };

  // The KV refresh-lease row.
  struct CodexRefreshLease {
    std::string owner;
    double lease_until_ms;
    friend bool operator==(const CodexRefreshLease&, const CodexRefreshLease&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexRefreshLease& value);
  CodexRefreshLease tag_invoke(const json::value_to_tag<CodexRefreshLease>&, const json::value& value);

  // `let cachedAuthPool: CodexAuthPoolState | null = null` (a live binding).
  extern std::optional<::uos::types::CodexAuthPoolState> cachedAuthPool;
  // `let authCacheGeneration = 0` (a live binding).
  extern double authCacheGeneration;
  // `const refreshesInFlight = new Map<string, Promise<CodexAuthState>>()`.
  extern rt::OrderedMap<std::string, rt::Promise<::uos::types::CodexAuthState>> refreshesInFlight;
  // `const codexProbeByResponse = new WeakMap<Response, RoutingAccount>()`.
  extern rt::WeakMap<rt::Response, routing_state::RoutingAccount> codexProbeByResponse;
  // `const codexProbeTransitionsInFlight = new Set<Promise<void>>()`: keyed by each promise's
  // identity (`promise.identity()`), values in insertion order (`[...set]`).
  extern rt::OrderedMap<const void*, rt::Promise<void>> codexProbeTransitionsInFlight;

  void setCodexResponseAccountTelemetry(const rt::Response& response, double slot, const std::string& accountId);

  void setCodexResponseActiveTelemetry(const rt::Response& response, const routing_state::RoutingAccount& routing);

  void inheritCodexResponseActiveTelemetry(const rt::Response& response, const rt::Response& source);

  /** Null fields mean the response was not admitted through a durable active decision. */
  CodexResponseActiveTelemetry getCodexResponseActiveTelemetry(const rt::Response& response);

  rt::Response withCodexWarnings(const rt::Response& response, const std::vector<std::string>& warnings);

  rt::Response withCodexAuthWarning(const rt::Response& response, const std::string& warning);

  /** The metadata stays isolate-local and never becomes a response header or durable credential record. */
  std::optional<routing_state::RoutingAccount> getCodexRoutingProbe(const rt::Response& response);

  /** The slot is isolate-local telemetry only; account IDs never leave the routing layer. */
  std::optional<double> getCodexResponseSlot(const rt::Response& response);

  /** Returns a stable digest while keeping the raw account ID inside this module. */
  [[nodiscard]] rt::Task<std::optional<std::string>> getCodexResponseAccountCohortId(rt::Response response);

  /**
   * Detach a response from its recovery probe without claiming success. Failed,
   * cancelled, or incomplete streams release ordinary routing, but retain the
   * durable ambiguity tombstone until a later recovery probe proves the account
   * healthy.
   */
  [[nodiscard]] rt::Task<void> releaseCodexResponseProbe(rt::Response response);

  /** Only a validated upstream `response.completed` event may clear the recovery probe. */
  [[nodiscard]] rt::Task<void> markCodexResponseCompleted(rt::Response response);

  /** A trustworthy failure after 2xx headers degrades health without treating cancellation or incompletion as failure. */
  [[nodiscard]] rt::Task<void> markCodexResponseUpstreamError(rt::Response response);

  void cacheCodexAuthPool(const ::uos::types::CodexAuthPoolState& pool);

  void resetCodexAuthCacheForTest();

  [[nodiscard]] rt::Task<CodexAuthPoolEntry> getAuthPoolEntry(bool forceKv = false, bool bypassInFlight = false);
}
