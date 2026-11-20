#pragma once
// Port of src/codex/dispatch.ts.
// Codex dispatch primitives and request preparation, split out of src/codex.ts.
//
// Declaration order follows the TS except where C++ needs a type first: CodexBankedResetOptions
// precedes FetchCodexResponsesOptions (which holds one) and CodexModelUnavailableAttempt precedes
// CodexSerialAdmissionDrivers. Coroutine parameters the TS defaults are overloads without them.
#include "uos/api_key_policy.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/banked_reset.hpp"
#include "uos/codex/banked_reset_provider.hpp"
#include "uos/codex/routing_state.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/sentinel/upstream_capture.hpp"
#include "uos/types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace uos::codex::dispatch {
  // `clientVersion: string | null`
  std::vector<std::string> codexModelsBaseUrls(const std::optional<std::string>& clientVersion);

  // `clientVersion: string | null`. `ifNoneMatch?: string | null` and `signal?: AbortSignal` are the
  // overload without them (std::nullopt is null or undefined).
  [[nodiscard]] rt::Task<rt::Response> fetchCodexModelsWithAuth(::uos::types::CodexAuthState auth, std::string url, std::optional<std::string> clientVersion);
  [[nodiscard]] rt::Task<rt::Response> fetchCodexModelsWithAuth(::uos::types::CodexAuthState auth, std::string url, std::optional<std::string> clientVersion,
                                                               std::optional<std::string> ifNoneMatch, std::optional<rt::AbortSignal> signal);

  std::vector<::uos::types::CodexAuthState> orderCodexAuthAccounts(const std::vector<::uos::types::CodexAuthState>& accounts, double startIndex);

  std::vector<auth::CodexAuthAccountEntry> randomizedAuthEntries(const auth::CodexAuthPoolEntry& poolEntry);

  void cancelResponseBody(const rt::Response& response);

  // `auth?: CodexAuthState` and `successfulResponseEvent: "success" | "reachable" | null = null` are the
  // overloads without them; std::nullopt is undefined / null.
  [[nodiscard]] rt::Task<void> recordCodexResponseHealth(std::string accountId, rt::Response response);
  [[nodiscard]] rt::Task<void> recordCodexResponseHealth(std::string accountId, rt::Response response, std::optional<::uos::types::CodexAuthState> auth);
  [[nodiscard]] rt::Task<void> recordCodexResponseHealth(std::string accountId, rt::Response response, std::optional<::uos::types::CodexAuthState> auth,
                                                        std::optional<std::string> successfulResponseEvent);

  // `error: unknown` (a caught value).
  [[nodiscard]] rt::Task<void> recordCodexThrownHealth(std::string accountId, rt::Reason error);

  /**
   * A post-reset retry is fenced more strictly than normal account routing. If
   * the auth-pool slot changes in the tiny interval before transport, preserve
   * the ordinary quota response instead of dispatching stale credentials.
   */
  class CodexBankedResetRetryFenceError : public rt::JsError {
  public:
    CodexBankedResetRetryFenceError() : rt::JsError("Codex banked-reset retry was fenced by an auth-pool change.", "CodexBankedResetRetryFenceError") {}
  };

  /** A dispatch admitted before a newer active selection must never reach transport. */
  class CodexActiveAccountFenceError : public rt::JsError {
  public:
    CodexActiveAccountFenceError() : rt::JsError("Codex active-account admission was superseded before transport.", "CodexActiveAccountFenceError") {}
  };

  // `signal?: AbortSignal`, `beforeTransport?: () => Promise<void>` and `onDispatch?: () => void` are the
  // overloads without them; std::nullopt and an empty std::function are undefined.
  [[nodiscard]] rt::Task<rt::Response> fetchCodexResponseWithAuth(::uos::types::CodexAuthState auth, std::string url, std::string serializedBody,
                                                                 rt::Headers baseHeaders);
  [[nodiscard]] rt::Task<rt::Response> fetchCodexResponseWithAuth(::uos::types::CodexAuthState auth, std::string url, std::string serializedBody,
                                                                 rt::Headers baseHeaders, std::optional<rt::AbortSignal> signal);
  [[nodiscard]] rt::Task<rt::Response> fetchCodexResponseWithAuth(::uos::types::CodexAuthState auth, std::string url, std::string serializedBody,
                                                                 rt::Headers baseHeaders, std::optional<rt::AbortSignal> signal,
                                                                 std::function<rt::Task<void>()> beforeTransport, std::function<void()> onDispatch);

  /** Keep a weekly upstream reset from becoming a multi-day client sleep. */
  inline constexpr double CODEX_CLIENT_RETRY_AFTER_MAX_SECONDS = 60;

  // `nowMs = Date.now()` is the overload without it.
  double codexRetryAfterSeconds(double retryAtMs);
  double codexRetryAfterSeconds(double retryAtMs, double nowMs);

  // `status: 401 | 404 | 429 | 503`; `retryAtMs: number | null = null`.
  rt::Response routingErrorResponse(double status, const std::string& message, const std::string& code, std::optional<double> retryAtMs = std::nullopt);

  // `retryAtMs: number | null`
  rt::Response upstreamTimeoutCircuitResponse(std::optional<double> retryAtMs);

  /**
   * No configured account can serve the named model. This is answered as an
   * unavailable model, never as a retryable quota or credential failure: waiting
   * changes nothing and this path wrote no fence.
   */
  // `detail: string | null`
  rt::Response codexModelUnavailableResponse(const std::string& model, const std::optional<std::string>& detail);

  // `type CodexResponseTimingHooks` (module-private in the TS); an empty std::function is undefined.
  struct CodexResponseTimingHooks {
    std::function<void()> onDispatch{};
    std::function<void()> onHeaders{};
  };

  // `type CodexBankedResetOptions` (module-private in the TS; FetchCodexResponsesOptions["bankedReset"]).
  // An absent member is std::nullopt, an empty std::function or a null provider.
  struct CodexBankedResetOptions {
    /** Test seam; normal traffic creates an account-bound upstream adapter only for a live reset candidate. */
    std::optional<banked_reset::CodexBankedResetConfig> config{};
    /** Test seam for proving a live configuration change stops a pending submission. */
    std::function<banked_reset::CodexBankedResetConfig()> reloadConfig{};
    std::shared_ptr<banked_reset_provider::CodexUsageResetProvider> provider{};
    // `kv?: Kv | null`: std::nullopt is undefined; a null pointer is null.
    std::optional<std::shared_ptr<rt::Kv>> kv{};
    std::function<double()> now{};
    std::function<std::string()> newOwnerToken{};
    std::function<rt::Task<std::string>(std::string value)> hash{};
    std::optional<banked_reset::CodexBankedResetTelemetry> telemetry{};
  };

  // `type FetchCodexResponsesOptions` (exported through the type list). std::nullopt is null and
  // undefined alike for the `string | null` members.
  struct FetchCodexResponsesOptions {
    /**
     * Quota hook. Callers either own a dispatch handle
     * (`Promise<ApiKeyProviderDispatch>`) or have nothing to hand back, which is
     * why the "no value" arm is a `void`-returning function type: it also accepts
     * the `Promise<void>` producers that `?? Promise.resolve()` call sites build.
     */
    // `(() => Promise<ApiKeyProviderDispatch>) | (() => void)` by arm; std::monostate is undefined. Every
    // promise-returning producer is the first arm (the TS awaits any object the hook hands back): one
    // that resolves to undefined (`Promise<void>`, an already-dispatched reservation) gives std::nullopt.
    using BeforeDispatch = std::variant<std::monostate, std::function<rt::Task<std::optional<api_key_policy::ApiKeyProviderDispatch>>()>,
                                        std::function<void()>>;

    std::optional<std::string> clientVersion{};
    std::optional<std::string> cacheScope{};
    std::optional<rt::AbortSignal> signal{};
    std::optional<std::string> requestId{};
    std::optional<CodexResponseTimingHooks> timing{};
    // `retrySleep?: (milliseconds: number) => Promise<void>`
    std::function<rt::Task<void>(double milliseconds)> retrySleep{};
    BeforeDispatch beforeDispatch{};
    std::optional<CodexBankedResetOptions> bankedReset{};
    /** Request-owned passive recorder; best effort, never required. */
    std::optional<::uos::sentinel::upstream_capture::SentinelUpstreamRecorder> sentinelUpstreamRecorder{};
    std::function<void()> onFollowerStale{};
  };

  // `type CodexProviderDispatchCoordinator` (exported through the type list).
  struct CodexProviderDispatchCoordinator {
    std::function<rt::Task<void>()> claim;
    std::function<void()> markTransportStarted;
    std::function<rt::Task<void>()> cancelBeforeTransport;
  };

  // `beforeDispatch: FetchCodexResponsesOptions["beforeDispatch"]`
  CodexProviderDispatchCoordinator createCodexProviderDispatchCoordinator(FetchCodexResponsesOptions::BeforeDispatch beforeDispatch);

  // `type PreparedCodexSubscriptionRequest` (exported through the type list).
  struct PreparedCodexSubscriptionRequest {
    // `unknown`: the request body without the ignored fields and prompt-cache breakpoints.
    json::value body;
    std::string serializedBody;
    std::string conversationIdentity;
    std::optional<std::string> nativeSessionIdentity;
    std::vector<std::string> warnings;
  };

  // `body: unknown`; `cacheScope: string | null`.
  [[nodiscard]] rt::Task<PreparedCodexSubscriptionRequest> prepareCodexSubscriptionRequest(json::value body, std::optional<std::string> cacheScope);

  // `type CodexAttemptPhase` (exported through the type list):
  // "initial" | "post_refresh" | "two_second_retry" | "post_retry_refresh" | "post_banked_reset" | "overage_fallback"
  using CodexAttemptPhase = std::string;

  // `type CodexBankedResetCandidate` (exported through the type list): the dispatch-side candidate, a
  // different type from banked_reset::CodexBankedResetCandidate.
  struct CodexBankedResetCandidate {
    auth::CodexAuthAccountEntry accountEntry;
    ::uos::types::CodexAuthState auth;
    routing_state::RoutingAccount routing;
    double quotaResetAtMs;
    double routingGeneration;
  };

  // `callback: (() => void) | undefined`: an empty std::function is undefined.
  void reportCodexResponseTiming(const std::function<void()>& callback);

  std::string codexStatusClass(double status);

  // `error: unknown` (a caught value).
  std::string codexErrorClass(const rt::Reason& error);

  // `(error: unknown): error is CodexError`; auth::codexErrorOf(error) is then that CodexError.
  bool isCodexSiblingTransportFailure(const rt::Reason& error);

  /**
   * A transport or provider callback may throw a non-Error value. Rethrowing it
   * verbatim would leak a non-Error rejection reason, so the value is preserved
   * as the cause instead; an absent value keeps the caller's own classification.
   */
  // `(error: unknown, fallback: () => Error): Error`: the result always holds an Error. A null Reason is
  // undefined (also the response state's initial `lastError: null`).
  rt::Reason dispatchFailureAsError(const rt::Reason& error, const std::function<rt::Reason()>& fallback);

  // `event`: "codex_attempt" | "codex_banked_reset_preflight" | "codex_quota_classification" |
  // "codex_token_refresh" | "codex_two_second_retry" | "codex_overage_served";
  // `fields: Readonly<Record<string, string | number | null>>`.
  void logCodexRouting(const std::string& event, const json::object& fields);

  // `signal: AbortSignal | undefined`; `sleep: (milliseconds: number) => Promise<void>`.
  [[nodiscard]] rt::Task<void> waitForCodexRetry(double milliseconds, std::optional<rt::AbortSignal> signal,
                                                std::function<rt::Task<void>(double milliseconds)> sleep);

  /** Settle outstanding probe transitions so routing reads observe their results. */
  [[nodiscard]] rt::Task<void> awaitPendingCodexProbeTransitions();

  /** The upstream model this body asks for, when it names one. */
  // `body: unknown`: nullptr is undefined.
  std::optional<std::string> requestedCodexModel(const json::value& body);
  std::optional<std::string> requestedCodexModel(const json::value* body);

  /** Codex CLI's native session identity is mirrored onto three internal headers. */
  // `nativeSessionIdentity: string | null`
  void applyNativeSessionHeaders(rt::Headers& headers, const std::optional<std::string>& nativeSessionIdentity);

  /** A selection no configured credential can serve is answered before any dispatch setup. */
  // `Response | null`
  std::optional<rt::Response> initialCodexSelectionResponse(const routing_state::RouteSelection& selection);

  struct CodexModelUnavailableAttempt {
    std::string accountId;
    std::string model;
    std::string detail;
    friend bool operator==(const CodexModelUnavailableAttempt&, const CodexModelUnavailableAttempt&) = default;
  };

  // `type CodexSerialAdmissionDrivers` (module-private in the TS). `Response | null` results are
  // std::nullopt for null.
  struct CodexSerialAdmissionDrivers {
    std::function<rt::Task<std::optional<rt::Response>>()> runPendingShortRetry;
    std::function<rt::Task<std::optional<rt::Response>>()> dispatchActive;
    std::function<rt::Task<std::optional<rt::Response>>()> terminalTransportResponse;
    std::function<bool()> hasQueuedRetry;
    std::function<bool()> reselectionRequested;
    std::function<rt::Task<std::optional<rt::Response>>()> advanceReselection;
    std::function<rt::Task<rt::Response>()> exhaustedResponse;
    /**
     * Learned account+model unavailability for one dispatched response. Real
     * traffic uses the account-attributed default; a test can supply its own
     * observation without a live upstream.
     */
    // An empty std::function is undefined.
    std::function<rt::Task<std::optional<CodexModelUnavailableAttempt>>(rt::Response response)> classifyModelUnavailable{};
  };

  // `Readonly<{ model: string; detail: string }>`: the result of parseCodexModelUnsupportedDetail and of
  // codexModelUnsupportedFromResponse.
  struct ParseCodexModelUnsupportedDetailResult {
    std::string model;
    std::string detail;
    friend bool operator==(const ParseCodexModelUnsupportedDetailResult&, const ParseCodexModelUnsupportedDetailResult&) = default;
  };

  /** The model a not-supported detail names, when the detail has exactly that shape. */
  // `payload: unknown`: nullptr is undefined. The result is null as std::nullopt.
  std::optional<ParseCodexModelUnsupportedDetailResult> parseCodexModelUnsupportedDetail(const json::value& payload);
  std::optional<ParseCodexModelUnsupportedDetailResult> parseCodexModelUnsupportedDetail(const json::value* payload);

  /**
   * Read a 400 body through a bounded clone, so a response that does not match the
   * learned shape still reaches the caller byte-for-byte with its own stream.
   */
  [[nodiscard]] rt::Task<std::optional<ParseCodexModelUnsupportedDetailResult>> codexModelUnsupportedFromResponse(rt::Response response);

  /**
   * Classify one upstream 400 as learned account+model unavailability and record
   * it for routing. Every other response, including every other 400, returns null
   * and leaves both routing state and the response untouched.
   */
  [[nodiscard]] rt::Task<std::optional<CodexModelUnavailableAttempt>> learnCodexModelUnavailable(rt::Response response);

  /**
   * The bounded one-account admission loop. Each pass runs a pending short retry,
   * dispatches only the currently admitted global active account, and asks for a
   * strong reselect after an authoritative exhaustion or final credential
   * failure instead of walking a stale sibling list. Order matters: a short retry,
   * then the active dispatch, then any terminal transport outcome, then a queued
   * retry, then one reselection advance, and finally the exhausted response.
   */
  [[nodiscard]] rt::Task<rt::Response> runCodexSerialAdmissionLoop(CodexSerialAdmissionDrivers drivers);
}
