#pragma once
// Port of src/provider/claude-code.ts.
//
// Claude subscription tier. This is the owner-gated adapter described in
// docs/handoff/claude-code-subscription-tier-2026-10-11.md, extended with the
// Console API-key direction: a `CLAUDE_API_KEY` (or admin-stored equivalent)
// travels as `x-api-key`, the legacy `CLAUDE_CODE_OAUTH_TOKEN` keeps the
// documented Bearer plus OAuth beta wire, and an organization-scoped key that
// demands `anthropic-workspace-id` discovers a workspace once instead of
// failing every request. The provider's OAuth usage endpoint and unified
// rate-limit headers are authoritative quota evidence; inference requests
// never synthesize a client fingerprint.
//
// ClaudeCodeFetch, ClaudeCodeEnvLookup and ClaudeCodeAuthHeaders are defined in
// uos/provider/claude_code_fwd.hpp (included here). `options = {}` parameters and
// `nowMs = Date.now()` defaults are overloads. `fetcher?` options are rt::Fetch values whose
// empty value is undefined; `signal?: AbortSignal` is std::optional<rt::AbortSignal>.
#include "uos/api_key_policy.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/provider/claude_code_fwd.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/provider/surplus_claude_request.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/sentinel/upstream_capture.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uos::provider::claude_code {
  inline constexpr std::string_view CLAUDE_CODE_BASE_URL = "https://api.anthropic.com";
  // `${CLAUDE_CODE_BASE_URL}/v1/messages`
  inline constexpr std::string_view CLAUDE_CODE_MESSAGES_URL = "https://api.anthropic.com/v1/messages";
  // `${CLAUDE_CODE_BASE_URL}/v1/models`
  inline constexpr std::string_view CLAUDE_CODE_MODELS_URL = "https://api.anthropic.com/v1/models";
  // `${CLAUDE_CODE_BASE_URL}/v1/organizations/workspaces`
  inline constexpr std::string_view CLAUDE_CODE_WORKSPACES_URL = "https://api.anthropic.com/v1/organizations/workspaces";
  // `export { CLAUDE_CODE_OAUTH_TOKEN_ENV }` (from ./claude-code-credentials.ts)
  using claude_code_credentials::CLAUDE_CODE_OAUTH_TOKEN_ENV;
  inline constexpr std::string_view CLAUDE_CODE_ALLOWED_KEY_IDS_ENV = "CLAUDE_CODE_ALLOWED_KEY_IDS";
  inline constexpr std::string_view CLAUDE_CODE_ANTHROPIC_VERSION = "2023-06-01";
  inline constexpr std::string_view CLAUDE_CODE_OAUTH_BETA = "oauth-2025-04-20";
  /**
   * The native client attribution the subscription lane requires first in the
   * system blocks. The endpoint strips this block before generation, but a
   * request without it is answered with a bare rate-limit rejection, so the
   * subscription credential always sends it exactly as the official client does.
   */
  inline constexpr std::string_view CLAUDE_CODE_SYSTEM_ATTRIBUTION = "You are Claude Code, Anthropic's official CLI for Claude.";
  /**
   * Anthropic Messages requires `max_tokens`. A client that omits one gets this
   * conservative floor rather than a fabricated per-model ceiling; an explicit
   * client value always wins, and catalog-driven resolution is a follow-up.
   */
  inline constexpr double CLAUDE_CODE_DEFAULT_MAX_OUTPUT_TOKENS = 32'000;

  // `export type ClaudeCodeFetch`: claude_code_fwd.hpp.

  // `export type ClaudeCodeEnvLookup` (environment lookup seam): claude_code_fwd.hpp.

  /**
   * The explicit principal allowlist. Empty or absent means nobody, so enabling
   * the tier always takes a second deliberate operator action beyond storing a
   * credential.
   */
  // `raw: string | null | undefined` (std::nullopt is either); `ReadonlySet<string>` in insertion order.
  rt::OrderedSet<std::string> parseClaudeCodeAllowedKeyIds(const std::optional<std::string>& raw);

  // `env: ClaudeCodeEnvLookup = defaultEnvLookup` (an empty function is the default as well).
  rt::OrderedSet<std::string> readClaudeCodeAllowedKeyIds();
  rt::OrderedSet<std::string> readClaudeCodeAllowedKeyIds(const ClaudeCodeEnvLookup& env);

  // `Readonly<{ eligible: true; credential; workspace: ClaudeCodeResolvedWorkspace | null }> |
  // Readonly<{ eligible: false; reason: ... }>`
  struct ClaudeCodeEligibilityEligible {
    static constexpr bool eligible = true;
    claude_code_credentials::ClaudeCodeResolvedCredential credential;
    std::optional<claude_code_credentials::ClaudeCodeResolvedWorkspace> workspace;  // ... | null
    friend bool operator==(const ClaudeCodeEligibilityEligible&, const ClaudeCodeEligibilityEligible&) = default;
  };
  struct ClaudeCodeEligibilityNotEligible {
    static constexpr bool eligible = false;
    // "model_unsupported" | "provider_disabled" | "principal_not_allowed" | "credential_missing" | "quota_blocked" | "cooling_down"
    std::string reason;
    friend bool operator==(const ClaudeCodeEligibilityNotEligible&, const ClaudeCodeEligibilityNotEligible&) = default;
  };
  using ClaudeCodeEligibility = std::variant<ClaudeCodeEligibilityEligible, ClaudeCodeEligibilityNotEligible>;

  struct ClaudeCodeEligibilityInput {
    /** Gateway-visible model id for this request. */
    // `model: unknown`: std::nullopt is undefined.
    std::optional<json::value> model;
    /** Authenticated principal for this request; never a shared or anonymous scope. */
    std::optional<std::string> keyId;  // string | null
    bool providerEnabled;
    rt::OrderedSet<std::string> allowedKeyIds;  // ReadonlySet<string>
    std::optional<claude_code_credentials::ClaudeCodeResolvedCredential> credential;  // ... | null
    // `workspace?: ClaudeCodeResolvedWorkspace | null` (null and undefined alike: `?? null`).
    std::optional<claude_code_credentials::ClaudeCodeResolvedWorkspace> workspace{};
    std::optional<claude_code_quota::ClaudeCodeQuotaState> quota;  // ... | null
    std::optional<double> nowMs{};  // undefined: Date.now()
  };

  /**
   * All conditions are conjunctive and none is inferred from another: the model
   * must be a Claude model, the operator selection must keep the provider on,
   * the request principal must be on the explicit allowlist, a credential must
   * exist, and the last authoritative window observation must not still block.
   */
  ClaudeCodeEligibility resolveClaudeCodeEligibility(const ClaudeCodeEligibilityInput& input);

  // "claude_code_credential_missing" | "claude_code_request_invalid" | "claude_code_upstream_unreachable"
  using ClaudeCodeErrorCode = std::string;

  // `code` is also stored in rt::JsError::code, so code that reads `error.code` off any caught
  // error sees it as JS does.
  class ClaudeCodeError : public rt::JsError {
  public:
    const ClaudeCodeErrorCode code;
    const double status;
    const std::optional<double> upstream_status;  // number | null

    // `upstreamStatus: number | null = null`
    ClaudeCodeError(std::string message_, ClaudeCodeErrorCode code_, double status_, std::optional<double> upstreamStatus = std::nullopt)
      : rt::JsError(std::move(message_), "ClaudeCodeError"), code(std::move(code_)), status(status_), upstream_status(upstreamStatus) {
      rt::JsError::code = code;
    }
  };

  // "ok" | "durable_exhaustion" | "transient_rate_limited" | "auth_invalid" | "forbidden" |
  // "billing_blocked" | "workspace_required" | "workspace_not_found" | "upstream_error" | "request_rejected"
  using ClaudeCodeResponseKind = std::string;

  struct ClaudeCodeResponseClassification {
    ClaudeCodeResponseKind kind;
    // `Readonly<{ window: ClaudeCodeWindowId; reset_at_ms: number | null }> | null`
    std::optional<claude_code_quota::ClaudeCodeQuotaExhaustion> exhaustion;
    std::optional<double> retry_after_ms;  // number | null
    /** Sanitized upstream message for operator display; never a header echo. */
    std::optional<std::string> detail;  // string | null
    friend bool operator==(const ClaudeCodeResponseClassification&, const ClaudeCodeResponseClassification&) = default;
  };

  /**
   * True for the rejections that mean the credential or its workspace selection
   * is wrong for this account, not that the provider is degraded. They are
   * retried only after the bounded configuration cooldown.
   */
  bool isClaudeCodeConfigurationKind(std::string_view kind);

  /**
   * Classify one upstream response without reading its body: durable exhaustion
   * needs the documented unified window evidence on a rejection, a generic rate
   * limit stays request-local, and authentication failures are their own class.
   * `classifyClaudeCodeErrorBody` then narrows 4xx configuration rejections.
   */
  ClaudeCodeResponseClassification classifyClaudeCodeResponse(double status, const rt::Headers& headers);
  ClaudeCodeResponseClassification classifyClaudeCodeResponse(double status, const rt::Headers& headers, double nowMs);

  /**
   * Narrow a non-2xx classification using the upstream error message. The
   * workspace and billing rejections are configuration states the operator must
   * see in the admin console, not generic request failures or provider outages.
   */
  // `bodyText: string | null`
  ClaudeCodeResponseClassification classifyClaudeCodeErrorBody(const ClaudeCodeResponseClassification& classification,
                                                               const std::optional<std::string>& bodyText);

  struct ClaudeCodeAuthenticatedFetchOptions {
    std::optional<claude_code_accounts::ClaudeCodeAccountBinding> accountBinding{};
    /** Resolved credential; resolved from KV and the environment when absent. */
    // `credential?: ClaudeCodeResolvedCredential | null` (null and undefined alike).
    std::optional<claude_code_credentials::ClaudeCodeResolvedCredential> credential{};
    /** Resolved workspace; resolved from KV and the environment when absent. */
    // `workspace?: ClaudeCodeResolvedWorkspace | null`: undefined resolves it, null is no workspace.
    rt::Nullable<claude_code_credentials::ClaudeCodeResolvedWorkspace> workspace{};
    /** KV handle used for credential, workspace, and quota state. */
    // `kv?: Kv | null`: undefined and null alike (`options.kv ?? null`, never getKv()).
    std::shared_ptr<rt::Kv> kv{};
    ClaudeCodeEnvLookup env{};    // empty: undefined
    ClaudeCodeFetch fetcher{};    // empty: the global fetch
    std::optional<rt::AbortSignal> signal{};
    /** Private authenticated identity for signed native reasoning replay; never sent upstream. */
    std::optional<std::string> reasoningReplayScope{};
    /** Positive disabled-thinking proof for this exact credential/workspace model. */
    std::optional<bool> supportsNativeDisabledThinking{};
    std::optional<std::vector<std::string>> nativeReasoningEfforts{};
    // `() => Promise<ApiKeyProviderDispatch | undefined>`: an empty function is undefined; a
    // std::nullopt result is undefined.
    std::function<rt::Task<std::optional<api_key_policy::ApiKeyProviderDispatch>>()> beforeDispatch{};
    std::function<void()> onDispatch{};
    std::optional<sentinel::upstream_capture::SentinelUpstreamRecorder> sentinelUpstreamRecorder{};
  };

  struct ClaudeCodeResponsesResult {
    std::optional<claude_code_accounts::ClaudeCodeAccountBinding> account_binding{};
    double observed_at_ms;
    rt::Response response;
    std::optional<std::string> request_id;  // string | null
    claude_code_credentials::ClaudeCodeResolvedCredential credential;
    /** Workspace used for this attempt, including one discovered during it. */
    std::optional<claude_code_credentials::ClaudeCodeResolvedWorkspace> workspace;  // ... | null
    std::optional<openai_telemetry::PromptCacheMode> prompt_cache_mode{};
    std::optional<double> explicit_breakpoint_count{};
    std::optional<claude_code_quota::ClaudeCodeQuotaObservation> quota;  // ... | null
    ClaudeCodeResponseClassification classification;
  };

  // `export type ClaudeCodeAuthHeaders = Readonly<{ kind: ClaudeCodeCredentialKind; secret: string }>`:
  // claude_code_fwd.hpp.

  // `options: Readonly<{ stream: boolean; workspaceId?: string | null }>` of claudeCodeRequestHeaders.
  struct ClaudeCodeRequestHeadersOptions {
    bool stream;
    std::optional<std::string> workspaceId{};  // null and undefined alike
  };

  /**
   * The documented credential wire per credential kind. Console API keys travel
   * as `x-api-key`; OAuth and setup tokens travel as `Authorization: Bearer`
   * with the OAuth beta that must survive gateway forwarding. An
   * organization-scoped key additionally needs the workspace selection, and no
   * client fingerprint beyond these documented headers is ever synthesized.
   */
  rt::Headers claudeCodeRequestHeaders(const ClaudeCodeAuthHeaders& credential, const ClaudeCodeRequestHeadersOptions& options);

  // `options: Readonly<{ reasoningReplayScope?; maxOutputTokens?; systemAttribution?;
  // supportsNativeDisabledThinking?; nativeReasoningEfforts? }>` of buildClaudeCodeRequest.
  struct BuildClaudeCodeRequestOptions {
    std::optional<std::string> reasoningReplayScope{};
    std::optional<double> maxOutputTokens{};
    /** Subscription credentials lead their system blocks with the native attribution. */
    std::optional<bool> systemAttribution{};
    std::optional<bool> supportsNativeDisabledThinking{};
    std::optional<std::vector<std::string>> nativeReasoningEfforts{};
  };

  /**
   * Build the native Messages request for a Claude model. Returns null when the
   * caller's body has no lossless native representation; the caller then leaves
   * the request to the ordinary paid route instead of trimming its meaning.
   */
  // `body: Record<string, unknown>`; returns `ReturnType<typeof buildSurplusClaudeRequest>`
  // (std::nullopt is null).
  std::optional<surplus_claude_request::SurplusClaudeRequest> buildClaudeCodeRequest(const json::object& body);
  std::optional<surplus_claude_request::SurplusClaudeRequest> buildClaudeCodeRequest(const json::object& body, const BuildClaudeCodeRequestOptions& options);

  // `Readonly<{ ok: true; workspace; account_binding?: ClaudeCodeAccountBinding }> |
  // Readonly<{ ok: false; status: number | null; error: string | null }>`
  struct ClaudeCodeWorkspaceDiscoveryResultOk {
    static constexpr bool ok = true;
    claude_code_credentials::ClaudeCodeResolvedWorkspace workspace;
    std::optional<claude_code_accounts::ClaudeCodeAccountBinding> account_binding{};
    friend bool operator==(const ClaudeCodeWorkspaceDiscoveryResultOk&, const ClaudeCodeWorkspaceDiscoveryResultOk&) = default;
  };
  struct ClaudeCodeWorkspaceDiscoveryResultNotOk {
    static constexpr bool ok = false;
    std::optional<double> status;       // number | null
    std::optional<std::string> error;  // string | null
    friend bool operator==(const ClaudeCodeWorkspaceDiscoveryResultNotOk&, const ClaudeCodeWorkspaceDiscoveryResultNotOk&) = default;
  };
  using ClaudeCodeWorkspaceDiscoveryResult = std::variant<ClaudeCodeWorkspaceDiscoveryResultOk, ClaudeCodeWorkspaceDiscoveryResultNotOk>;

  // `options: Readonly<{ credential: ClaudeCodeAuthHeaders; kv?: Kv | null; fetcher?; signal?;
  // accountBinding? }>` of discoverClaudeCodeWorkspace.
  struct DiscoverClaudeCodeWorkspaceOptions {
    ClaudeCodeAuthHeaders credential;
    // `kv?: Kv | null`: undefined and null alike (`options.kv ?? null`, never getKv()).
    std::shared_ptr<rt::Kv> kv{};
    ClaudeCodeFetch fetcher{};  // empty: the global fetch
    std::optional<rt::AbortSignal> signal{};
    std::optional<claude_code_accounts::ClaudeCodeAccountBinding> accountBinding{};
  };

  /**
   * Discover the workspace an organization-scoped credential must name. The
   * result is cached in KV as advisory state so later requests skip the lookup;
   * an operator pin always wins because resolution prefers KV and environment
   * configuration over the discovered entry.
   */
  rt::Task<ClaudeCodeWorkspaceDiscoveryResult> discoverClaudeCodeWorkspace(DiscoverClaudeCodeWorkspaceOptions options);

  // `body: unknown`; throws ClaudeCodeError("claude_code_request_invalid") for a body that is not an
  // object, and the TS's other errors.
  rt::Task<ClaudeCodeResponsesResult> fetchClaudeCodeResponses(json::value body);
  rt::Task<ClaudeCodeResponsesResult> fetchClaudeCodeResponses(json::value body, ClaudeCodeAuthenticatedFetchOptions options);

  /** Kind of a credential the admin surface is about to store, for validation. */
  claude_code_credentials::ClaudeCodeCredentialKind claudeCodeCredentialKind(const std::string& secret);
}
