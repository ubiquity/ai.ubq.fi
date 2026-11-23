#include "uos/provider/claude_code.hpp"

#include "uos/inference_deadline.hpp"
#include "uos/provider/anthropic_responses.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <utility>

namespace uos::provider::claude_code {
  namespace {
    using claude_code_accounts::ClaudeCodeAccountBinding;
    using claude_code_credentials::ClaudeCodeResolvedCredential;
    using claude_code_credentials::ClaudeCodeResolvedWorkspace;
    using claude_code_quota::ClaudeCodeQuotaObservation;
    using claude_code_quota::sanitizeClaudeCodeErrorMessage;

    constexpr double CLAUDE_CODE_MAX_TRANSIENT_COOLDOWN_MS = 30 * 60'000;
    /** Bounded error text retained from upstream for classification and display. */
    constexpr double CLAUDE_CODE_ERROR_BODY_MAX_BYTES = 4'096;

    std::optional<std::string> nonEmptyString(const json::value* value) {
      if (value == nullptr || !value->is_string()) return std::nullopt;
      std::string trimmed = rt::trim(std::string_view(value->get_string()));
      if (rt::length(trimmed) == 0) return std::nullopt;
      return trimmed;
    }
    std::optional<std::string> nonEmptyString(const std::optional<std::string>& value) {
      if (!value) return std::nullopt;
      std::string trimmed = rt::trim(*value);
      if (rt::length(trimmed) == 0) return std::nullopt;
      return trimmed;
    }

    std::optional<std::string> defaultEnvLookup(const std::string& name) { return rt::env::get(name); }

    std::optional<double> parseRetryAfterMs(const std::optional<std::string>& value) {
      if (!value) return std::nullopt;
      const double seconds = rt::stringToNumber(*value);
      if (rt::Number::isFinite(seconds) && seconds >= 0) return rt::Math::min(rt::Math::trunc(seconds * 1'000), CLAUDE_CODE_MAX_TRANSIENT_COOLDOWN_MS);
      const double date = rt::Date::parse(*value);
      if (rt::Number::isFinite(date)) return rt::Math::min(rt::Math::max(0.0, date - rt::dateNow()), CLAUDE_CODE_MAX_TRANSIENT_COOLDOWN_MS);
      return std::nullopt;
    }

    /** Prefer the nested `error.message`; fall back to the sanitized raw text. */
    std::optional<std::string> extractClaudeCodeErrorMessage(const std::optional<std::string>& bodyText) {
      if (!bodyText || bodyText->empty()) return std::nullopt;
      try {
        const json::value parsed = rt::jsonParse(*bodyText);
        if (utils::isRecord(parsed)) {
          const json::value* error = rt::get(parsed, "error");
          if (utils::isRecord(error)) {
            const std::optional<std::string> message = nonEmptyString(rt::get(*error, "message"));
            if (message) return sanitizeClaudeCodeErrorMessage(*message);
          }
        }
      } catch (const rt::JsError&) {
        // Fall through: the sanitized raw body is still useful for diagnosis.
      }
      return sanitizeClaudeCodeErrorMessage(*bodyText);
    }

    bool contains(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

    // `error instanceof ApiKeyQuotaDispatchError` / `instanceof ClaudeCodeError`
    bool isDispatchOrClaudeCodeError(const rt::Reason& error) {
      if (!error) return false;
      try {
        std::rethrow_exception(error);
      } catch (const api_key_policy::ApiKeyQuotaDispatchError&) {
        return true;
      } catch (const ClaudeCodeError&) {
        return true;
      } catch (...) {
        return false;
      }
    }

    rt::Reason claudeCodeTransportFailure(const rt::Reason& error, const std::optional<rt::AbortSignal>& signal, const rt::AbortController& headersDeadline) {
      if (isDispatchOrClaudeCodeError(error)) return error;
      if (signal && signal->aborted()) return signal->reason() ? signal->reason() : error;
      if (headersDeadline.signal().aborted()) return headersDeadline.signal().reason() ? headersDeadline.signal().reason() : error;
      if (rt::isError(error) && rt::errorName(error) == std::optional<std::string>("AbortError")) return error;
      return rt::makeReason(ClaudeCodeError("Claude Code Messages request could not reach the upstream service.", "claude_code_upstream_unreachable", 502));
    }

    rt::Reason abortReasonError(const rt::AbortSignal& signal) {
      if (rt::isError(signal.reason())) return signal.reason();
      return rt::makeReason(rt::DOMException("The request was aborted.", "AbortError"));
    }

    rt::Task<rt::Response> awaitWithAbort(rt::Promise<rt::Response> promise, rt::AbortSignal signal) {
      if (signal.aborted()) std::rethrow_exception(abortReasonError(signal));
      rt::EventListener onAbort;
      const auto dispose = rt::scopeExit([&] { signal.removeEventListener("abort", onAbort); });
      rt::Promise<rt::Response> aborted = rt::newPromise<rt::Response>([&](const rt::Resolve<rt::Response>&, const rt::Reject& reject) {
        onAbort = rt::EventListener([reject, signal] { reject(abortReasonError(signal)); });
        signal.addEventListener("abort", onAbort, {.once = true});
      });
      std::vector<rt::Promise<rt::Response>> racers;
      racers.push_back(promise);
      racers.push_back(aborted);
      const rt::Promise<rt::Response> racing = rt::race(std::move(racers));
      auto response = co_await racing.wait();
      co_return response;
    }

    std::optional<std::string> responseRequestId(const rt::Response& response) {
      std::optional<std::string> id = response.headers().get("request-id");
      if (!id) id = response.headers().get("x-request-id");
      return nonEmptyString(id);
    }

    std::optional<std::string> responseReasoningSummary(const json::object& body) {
      const json::value* reasoning = rt::get(body, "reasoning");
      if (!utils::isRecord(reasoning)) return std::nullopt;
      return nonEmptyString(rt::get(*reasoning, "summary"));
    }

    /** Bounded body read for error classification; never throws. */
    rt::Task<std::optional<std::string>> readErrorBodyText(rt::Response response) {
      std::optional<std::string> out;
      try {
        const rt::Response copy = response.clone();
        const std::string text = co_await copy.text();
        out = static_cast<double>(rt::length(text)) > CLAUDE_CODE_ERROR_BODY_MAX_BYTES ? rt::slice(text, 0, CLAUDE_CODE_ERROR_BODY_MAX_BYTES) : text;
      } catch (...) {
        out = std::nullopt;
      }
      co_return out;
    }

    rt::Task<ClaudeCodeResolvedCredential> requireClaudeCodeCredential(std::optional<ClaudeCodeResolvedCredential> supplied, std::shared_ptr<rt::Kv> kv,
                                                                       ClaudeCodeEnvLookup env) {
      if (supplied) co_return std::move(*supplied);
      rt::Promise<std::optional<ClaudeCodeResolvedCredential>> resolving(claude_code_credentials::resolveClaudeCodeCredential(kv, env));
      std::optional<ClaudeCodeResolvedCredential> resolved = co_await resolving.wait();
      if (resolved) co_return std::move(*resolved);
      throw ClaudeCodeError("Claude subscription inference is unavailable because neither CLAUDE_API_KEY, " + std::string(CLAUDE_CODE_OAUTH_TOKEN_ENV) +
                              ", nor a stored credential is configured.",
                            "claude_code_credential_missing", 503);
    }

    struct ClaudeCodeWorkspaceListEntry {
      std::string id;
      std::optional<std::string> name;  // string | null
    };

    std::vector<ClaudeCodeWorkspaceListEntry> parseWorkspaceList(const json::value& value) {
      std::vector<ClaudeCodeWorkspaceListEntry> entries;
      if (!utils::isRecord(value)) return entries;
      const json::value* data = rt::get(value, "data");
      if (data == nullptr || !data->is_array()) return entries;
      for (const json::value& item : data->get_array()) {
        if (!utils::isRecord(item)) continue;
        std::optional<std::string> id = nonEmptyString(rt::get(item, "id"));
        if (!id || !rt::startsWith(*id, "wrkspc_")) continue;
        const json::value* archivedAt = rt::get(item, "archived_at");
        if (archivedAt != nullptr && !archivedAt->is_null()) continue;
        entries.push_back({.id = std::move(*id), .name = nonEmptyString(rt::get(item, "name"))});
      }
      return entries;
    }

    /**
     * The Console default workspace is the inference target for an
     * organization-scoped key, so a workspace literally named `Default` wins over
     * the listing order; otherwise the first non-archived workspace is used.
     */
    std::optional<ClaudeCodeWorkspaceListEntry> pickWorkspace(const std::vector<ClaudeCodeWorkspaceListEntry>& entries) {
      for (const ClaudeCodeWorkspaceListEntry& entry : entries)
        if (rt::toLowerCase(rt::trim(entry.name.value_or(""))) == "default") return entry;
      if (entries.empty()) return std::nullopt;
      return entries.front();
    }

    struct PerformClaudeCodeMessagesAttemptOptions {
      ClaudeCodeResolvedCredential credential;
      std::optional<ClaudeCodeResolvedWorkspace> workspace;  // ... | null
      std::string encodedBody;
      bool stream;
      std::optional<rt::AbortSignal> signal{};
      ClaudeCodeFetch fetcher{};
      std::function<rt::Task<std::optional<api_key_policy::ApiKeyProviderDispatch>>()> beforeDispatch{};
      std::function<void()> onDispatch{};
      std::optional<sentinel::upstream_capture::SentinelUpstreamRecorder> sentinelUpstreamRecorder{};
    };

    struct ClaudeCodeMessagesAttempt {
      rt::Response response;
      ClaudeCodeResponseClassification classification;
      std::optional<ClaudeCodeQuotaObservation> quota;  // ... | null
      std::optional<std::string> requestId;             // string | null
      double observedAtMs;
    };

    rt::Task<ClaudeCodeMessagesAttempt> performClaudeCodeMessagesAttempt(PerformClaudeCodeMessagesAttemptOptions options) {
      ClaudeCodeRequestHeadersOptions headerOptions{.stream = options.stream};
      if (options.workspace) headerOptions.workspaceId = options.workspace->id;
      const rt::Headers headers = claudeCodeRequestHeaders(ClaudeCodeAuthHeaders{.kind = options.credential.kind, .secret = options.credential.secret}, headerOptions);
      const rt::AbortController headersDeadline;
      const rt::Timeout headersTimer = rt::setTimeout(
        [headersDeadline] { headersDeadline.abort(rt::makeReason(rt::DOMException("Claude Code response headers timed out.", "TimeoutError"))); },
        inference_deadline::STREAM_FIRST_EVENT_DEADLINE_MS);
      std::vector<rt::AbortSignal> sources;
      if (options.signal) {
        sources.push_back(*options.signal);
        sources.push_back(headersDeadline.signal());
      }
      const rt::AbortSignal signal = options.signal ? rt::AbortSignal::any(sources) : headersDeadline.signal();
      std::optional<rt::Response> response;
      std::optional<sentinel::upstream_capture::SentinelUpstreamAttemptHandle> upstreamAttempt;
      rt::Reason failure;
      try {
        std::optional<api_key_policy::ApiKeyProviderDispatch> dispatch;
        if (options.beforeDispatch) {
          rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>> dispatching(options.beforeDispatch());
          dispatch = co_await dispatching.wait();
        }
        if (signal.aborted()) {
          if (dispatch) {
            rt::Promise<void> cancelling(dispatch->cancelBeforeTransport());
            co_await cancelling.wait();
          } else {
            co_await rt::microtask();
          }
          if (signal.reason()) std::rethrow_exception(signal.reason());
          throw rt::DOMException("The request was aborted.", "AbortError");
        }
        if (dispatch) dispatch->markTransportStarted();
        if (options.onDispatch) options.onDispatch();
        if (options.sentinelUpstreamRecorder && options.sentinelUpstreamRecorder->startAttempt)
          upstreamAttempt = options.sentinelUpstreamRecorder->startAttempt("claude_code");
        const ClaudeCodeFetch fetcher = options.fetcher ? options.fetcher : rt::globalFetch();
        rt::RequestInit init;
        init.method = "POST";
        init.headers = headers;
        init.body = rt::BodyInit(options.encodedBody);
        init.redirect = "manual";
        init.signal = signal;
        rt::RequestInfo input = std::string(CLAUDE_CODE_MESSAGES_URL);
        rt::Promise<rt::Response> fetching(fetcher(std::move(input), std::move(init)));
        rt::Promise<rt::Response> waiting(awaitWithAbort(fetching, signal));
        response = co_await waiting.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) {
        if (upstreamAttempt && upstreamAttempt->recordFetchError) upstreamAttempt->recordFetchError();
        const rt::Reason thrown = claudeCodeTransportFailure(failure, options.signal, headersDeadline);
        rt::clearTimeout(headersTimer);
        std::rethrow_exception(thrown);
      }
      rt::clearTimeout(headersTimer);

      const std::optional<std::string> requestId = responseRequestId(*response);
      const double observedAtMs = rt::dateNow();
      const ClaudeCodeResponseClassification base = classifyClaudeCodeResponse(response->status(), response->headers());
      ClaudeCodeResponseClassification classification = base;
      if (base.kind != "ok") {
        rt::Promise<std::optional<std::string>> reading(readErrorBodyText(*response));
        const std::optional<std::string> bodyText = co_await reading.wait();
        classification = classifyClaudeCodeErrorBody(base, bodyText);
      }
      std::optional<ClaudeCodeQuotaObservation> quota = claude_code_quota::observeClaudeCodeQuota(response->headers(), observedAtMs);
      const rt::Response recorded = upstreamAttempt && upstreamAttempt->wrap ? upstreamAttempt->wrap(*response) : *response;
      co_return ClaudeCodeMessagesAttempt{
        .response = recorded,
        .classification = std::move(classification),
        .quota = std::move(quota),
        .requestId = requestId,
        .observedAtMs = observedAtMs,
      };
    }
  }

  rt::OrderedSet<std::string> parseClaudeCodeAllowedKeyIds(const std::optional<std::string>& raw) {
    static const rt::Regex SEPARATORS("[\\s,]+", "");
    rt::OrderedSet<std::string> ids;
    if (!raw) return ids;
    for (const std::string& candidate : rt::split(*raw, SEPARATORS)) {
      std::string id = rt::trim(candidate);
      if (!id.empty()) ids.add(id);
    }
    return ids;
  }

  rt::OrderedSet<std::string> readClaudeCodeAllowedKeyIds(const ClaudeCodeEnvLookup& env) {
    const ClaudeCodeEnvLookup lookup = env ? env : ClaudeCodeEnvLookup(defaultEnvLookup);
    return parseClaudeCodeAllowedKeyIds(lookup(std::string(CLAUDE_CODE_ALLOWED_KEY_IDS_ENV)));
  }
  rt::OrderedSet<std::string> readClaudeCodeAllowedKeyIds() { return readClaudeCodeAllowedKeyIds(ClaudeCodeEnvLookup()); }

  ClaudeCodeEligibility resolveClaudeCodeEligibility(const ClaudeCodeEligibilityInput& input) {
    const double nowMs = input.nowMs ? *input.nowMs : rt::dateNow();
    if (!surplus_claude_request::isSurplusClaudeModel(input.model ? &*input.model : nullptr)) return ClaudeCodeEligibilityNotEligible{.reason = "model_unsupported"};
    if (!input.providerEnabled) return ClaudeCodeEligibilityNotEligible{.reason = "provider_disabled"};
    if (!input.keyId || input.keyId->empty() || !input.allowedKeyIds.has(*input.keyId)) return ClaudeCodeEligibilityNotEligible{.reason = "principal_not_allowed"};
    if (!input.credential) return ClaudeCodeEligibilityNotEligible{.reason = "credential_missing"};
    if (claude_code_quota::claudeCodeQuotaBlocked(input.quota, nowMs)) return ClaudeCodeEligibilityNotEligible{.reason = "quota_blocked"};
    if (claude_code_quota::claudeCodeQuotaCoolingDown(input.quota, nowMs)) return ClaudeCodeEligibilityNotEligible{.reason = "cooling_down"};
    return ClaudeCodeEligibilityEligible{.credential = *input.credential, .workspace = input.workspace};
  }

  bool isClaudeCodeConfigurationKind(std::string_view kind) {
    return kind == "billing_blocked" || kind == "workspace_not_found" || kind == "workspace_required" || kind == "forbidden";
  }

  ClaudeCodeResponseClassification classifyClaudeCodeResponse(double status, const rt::Headers& headers, double nowMs) {
    if (status >= 200 && status < 300) return {.kind = "ok", .exhaustion = std::nullopt, .retry_after_ms = std::nullopt, .detail = std::nullopt};
    if (status == 429) {
      const std::optional<ClaudeCodeQuotaObservation> observation = claude_code_quota::observeClaudeCodeQuota(headers, nowMs);
      std::optional<claude_code_quota::ClaudeCodeQuotaExhaustion> exhaustion = claude_code_quota::claudeCodeExhaustionFromResponse(status, observation);
      if (exhaustion) return {.kind = "durable_exhaustion", .exhaustion = std::move(exhaustion), .retry_after_ms = std::nullopt, .detail = std::nullopt};
      return {.kind = "transient_rate_limited", .exhaustion = std::nullopt, .retry_after_ms = parseRetryAfterMs(headers.get("retry-after")), .detail = std::nullopt};
    }
    if (status == 401) return {.kind = "auth_invalid", .exhaustion = std::nullopt, .retry_after_ms = std::nullopt, .detail = std::nullopt};
    if (status == 403) return {.kind = "forbidden", .exhaustion = std::nullopt, .retry_after_ms = std::nullopt, .detail = std::nullopt};
    if (status >= 500) return {.kind = "upstream_error", .exhaustion = std::nullopt, .retry_after_ms = std::nullopt, .detail = std::nullopt};
    return {.kind = "request_rejected", .exhaustion = std::nullopt, .retry_after_ms = std::nullopt, .detail = std::nullopt};
  }
  ClaudeCodeResponseClassification classifyClaudeCodeResponse(double status, const rt::Headers& headers) {
    return classifyClaudeCodeResponse(status, headers, rt::dateNow());
  }

  ClaudeCodeResponseClassification classifyClaudeCodeErrorBody(const ClaudeCodeResponseClassification& classification, const std::optional<std::string>& bodyText) {
    if (classification.kind == "ok" || classification.kind == "durable_exhaustion" || classification.kind == "upstream_error") return classification;
    const std::optional<std::string> detail = extractClaudeCodeErrorMessage(bodyText);
    if (!detail || detail->empty()) return classification;
    const std::string lower = rt::toLowerCase(*detail);
    ClaudeCodeResponseClassification withDetail = classification;
    withDetail.detail = detail;
    if (classification.kind == "request_rejected" && contains(lower, "not scoped to a workspace")) {
      withDetail.kind = "workspace_required";
      return withDetail;
    }
    if (contains(lower, "not found") && contains(lower, "workspace")) {
      withDetail.kind = "workspace_not_found";
      return withDetail;
    }
    if (contains(lower, "workspace") && contains(lower, "valid workspace")) {
      withDetail.kind = "workspace_not_found";
      return withDetail;
    }
    if (contains(lower, "credit balance") || contains(lower, "purchase credits")) {
      withDetail.kind = "billing_blocked";
      return withDetail;
    }
    if (contains(lower, "workspace") && contains(lower, "organization")) {
      withDetail.kind = "workspace_not_found";
      return withDetail;
    }
    return withDetail;
  }

  rt::Headers claudeCodeRequestHeaders(const ClaudeCodeAuthHeaders& credential, const ClaudeCodeRequestHeadersOptions& options) {
    rt::Headers headers;
    if (credential.kind == "oauth") {
      headers.set("Authorization", "Bearer " + credential.secret);
      headers.set("anthropic-beta", CLAUDE_CODE_OAUTH_BETA);
    } else {
      headers.set("x-api-key", credential.secret);
    }
    headers.set("Content-Type", "application/json");
    headers.set("Accept", options.stream ? "text/event-stream" : "application/json");
    headers.set("anthropic-version", CLAUDE_CODE_ANTHROPIC_VERSION);
    const std::optional<std::string> workspaceId = nonEmptyString(options.workspaceId);
    if (workspaceId) headers.set("anthropic-workspace-id", *workspaceId);
    return headers;
  }

  std::optional<surplus_claude_request::SurplusClaudeRequest> buildClaudeCodeRequest(const json::object& body, const BuildClaudeCodeRequestOptions& options) {
    if (!surplus_claude_request::isSurplusClaudeModel(rt::get(body, "model"))) return std::nullopt;
    std::optional<double> maxOutputTokens;
    if (rt::get(body, "max_output_tokens") == nullptr) maxOutputTokens = options.maxOutputTokens.value_or(CLAUDE_CODE_DEFAULT_MAX_OUTPUT_TOKENS);
    surplus_claude_request::SurplusClaudeRequestOptions builderOptions;
    builderOptions.provider = "claude_code";
    builderOptions.reasoningReplayScope = options.reasoningReplayScope;
    // The direct Messages route carries the documented adaptive-thinking wire
    // the shared builder already emits.
    builderOptions.supportsNativeReasoningEffort = (options.nativeReasoningEfforts ? static_cast<double>(options.nativeReasoningEfforts->size()) : 0.0) > 0;
    builderOptions.nativeReasoningEfforts = options.nativeReasoningEfforts;
    builderOptions.supportsNativeDisabledThinking = options.supportsNativeDisabledThinking;
    // Codex advertises hosted web_search by default; the direct Messages lane
    // has no equivalent server tool and must not reject the whole turn.
    builderOptions.omitHostedWebSearch = true;
    std::optional<surplus_claude_request::SurplusClaudeRequest> built = surplus_claude_request::buildSurplusClaudeRequest(body, maxOutputTokens, builderOptions);
    if (!built) return std::nullopt;
    // `prompt_cache_key` is a reseller-router affinity hint with no place in the
    // native Messages contract, so it never reaches the direct route.
    json::object nativeBody = built->body;
    rt::objectDelete(nativeBody, "prompt_cache_key");
    if (options.systemAttribution.value_or(false)) {
      json::array system;
      json::object attribution;
      attribution["type"] = "text";
      attribution["text"] = json::string(CLAUDE_CODE_SYSTEM_ATTRIBUTION);
      system.emplace_back(std::move(attribution));
      if (const json::value* existing = rt::get(nativeBody, "system"); existing != nullptr && existing->is_array())
        for (const json::value& block : existing->get_array()) system.push_back(block);
      rt::objectSet(nativeBody, "system", json::value(std::move(system)));
    }
    built->body = std::move(nativeBody);
    return built;
  }
  std::optional<surplus_claude_request::SurplusClaudeRequest> buildClaudeCodeRequest(const json::object& body) {
    return buildClaudeCodeRequest(body, BuildClaudeCodeRequestOptions());
  }

  rt::Task<ClaudeCodeWorkspaceDiscoveryResult> discoverClaudeCodeWorkspace(DiscoverClaudeCodeWorkspaceOptions options) {
    const std::shared_ptr<rt::Kv> kv = options.kv;
    const rt::Headers headers = claudeCodeRequestHeaders(options.credential, ClaudeCodeRequestHeadersOptions{.stream = false});
    std::optional<rt::Response> received;
    rt::Reason fetchError;
    try {
      const ClaudeCodeFetch fetcher = options.fetcher ? options.fetcher : rt::globalFetch();
      rt::RequestInit init;
      init.method = "GET";
      init.headers = headers;
      init.redirect = "manual";
      init.signal = options.signal;
      rt::RequestInfo input = std::string(CLAUDE_CODE_WORKSPACES_URL);
      rt::Promise<rt::Response> fetching(fetcher(std::move(input), std::move(init)));
      received = co_await fetching.wait();
    } catch (...) {
      fetchError = std::current_exception();
    }
    if (fetchError) {
      const std::string message = rt::isError(fetchError) ? rt::errorMessage(fetchError).value_or("") : rt::reasonToString(fetchError);
      co_return ClaudeCodeWorkspaceDiscoveryResultNotOk{.status = std::nullopt, .error = sanitizeClaudeCodeErrorMessage(message)};
    }
    const rt::Response response = *received;
    std::optional<std::string> bodyText;
    try {
      const std::string text = co_await response.text();
      bodyText = text;
    } catch (...) {
      bodyText = std::nullopt;
    }
    if (!response.ok()) co_return ClaudeCodeWorkspaceDiscoveryResultNotOk{.status = response.status(), .error = sanitizeClaudeCodeErrorMessage(bodyText)};
    json::value parsed;
    try {
      parsed = bodyText && !bodyText->empty() ? rt::jsonParse(*bodyText) : json::value(nullptr);
    } catch (const rt::JsError&) {
      parsed = nullptr;
    }
    const std::optional<ClaudeCodeWorkspaceListEntry> picked = pickWorkspace(parseWorkspaceList(parsed));
    if (!picked)
      co_return ClaudeCodeWorkspaceDiscoveryResultNotOk{.status = response.status(), .error = std::string("No usable workspace was listed for this credential.")};
    const ClaudeCodeResolvedWorkspace workspace{.id = picked->id, .name = picked->name, .source = "discovered"};
    std::optional<ClaudeCodeAccountBinding> accountBinding = options.accountBinding;
    if (kv) {
      if (options.accountBinding) {
        rt::Promise<bool> checking(claude_code_accounts::isClaudeCodeAccountCurrent(kv, *options.accountBinding));
        const bool current = co_await checking.wait();
        if (!current) co_return ClaudeCodeWorkspaceDiscoveryResultNotOk{.status = std::nullopt, .error = std::string("Claude account changed during workspace discovery.")};
      }
      std::optional<claude_code_credentials::ClaudeCodeWorkspaceState> previous;
      try {
        rt::Promise<std::optional<claude_code_credentials::ClaudeCodeWorkspaceState>> loading(claude_code_credentials::loadClaudeCodeWorkspaceState(kv));
        previous = co_await loading.wait();
      } catch (...) {
        previous = std::nullopt;
      }
      claude_code_credentials::MergeClaudeCodeWorkspaceStateInput merge{.id = workspace.id, .name = workspace.name, .source = "discovered"};
      const claude_code_credentials::ClaudeCodeWorkspaceState state = claude_code_credentials::mergeClaudeCodeWorkspaceState(previous, merge);
      if (options.accountBinding) {
        claude_code_accounts::AccountUpdate update;
        update.accountId = options.accountBinding->account_id;
        update.workspace = state;
        update.expected = options.accountBinding;
        rt::Promise<ClaudeCodeAccountBinding> updating(claude_code_accounts::updateClaudeCodeAccount(kv, std::move(update)));
        accountBinding = co_await updating.wait();
      } else {
        rt::Promise<bool> storing(claude_code_credentials::storeClaudeCodeWorkspaceState(kv, state));
        co_await storing.wait();
      }
    }
    co_return ClaudeCodeWorkspaceDiscoveryResultOk{.workspace = workspace, .account_binding = accountBinding};
  }

  rt::Task<ClaudeCodeResponsesResult> fetchClaudeCodeResponses(json::value body, ClaudeCodeAuthenticatedFetchOptions options) {
    if (!utils::isRecord(body)) {
      throw ClaudeCodeError("Claude Code Messages requests must use a canonical JSON object body.", "claude_code_request_invalid", 400);
    }
    const std::shared_ptr<rt::Kv> kv = options.kv;
    rt::Promise<ClaudeCodeResolvedCredential> requiring(requireClaudeCodeCredential(options.credential, kv, options.env));
    const ClaudeCodeResolvedCredential credential = co_await requiring.wait();
    // An array passes isRecord; its `model` is undefined, so it has no native representation.
    std::optional<surplus_claude_request::SurplusClaudeRequest> native;
    if (const json::object* object = body.if_object()) {
      BuildClaudeCodeRequestOptions buildOptions;
      buildOptions.reasoningReplayScope = options.reasoningReplayScope;
      buildOptions.supportsNativeDisabledThinking = options.supportsNativeDisabledThinking;
      buildOptions.nativeReasoningEfforts = options.nativeReasoningEfforts;
      buildOptions.maxOutputTokens = std::nullopt;
      // Only the subscription lane carries the native attribution; a Console API
      // key request must stay a plain API request.
      buildOptions.systemAttribution = credential.kind == "oauth";
      native = buildClaudeCodeRequest(*object, buildOptions);
    }
    if (!native) {
      throw ClaudeCodeError("The request has no lossless native Anthropic Messages representation.", "claude_code_request_invalid", 400);
    }

    std::string encodedBody;
    try {
      encodedBody = rt::jsonStringify(json::value(native->body));
    } catch (const rt::JsError&) {
      throw ClaudeCodeError("Claude Code Messages requests must use a JSON-serializable body.", "claude_code_request_invalid", 400);
    }

    std::optional<ClaudeCodeResolvedWorkspace> initialWorkspace;
    if (options.workspace == rt::undefined) {
      rt::Promise<std::optional<ClaudeCodeResolvedWorkspace>> resolving(claude_code_credentials::resolveClaudeCodeWorkspace(kv, options.env));
      initialWorkspace = co_await resolving.wait();
    } else if (options.workspace.hasValue()) {
      initialWorkspace = *options.workspace;
    }

    PerformClaudeCodeMessagesAttemptOptions attemptOptions{
      .credential = credential,
      .workspace = std::nullopt,
      .encodedBody = encodedBody,
      .stream = rt::get(native->body, "stream") != nullptr && rt::get(native->body, "stream")->is_bool() && rt::get(native->body, "stream")->get_bool(),
      .signal = options.signal,
      .fetcher = options.fetcher,
      .beforeDispatch = options.beforeDispatch,
      .onDispatch = options.onDispatch,
      .sentinelUpstreamRecorder = options.sentinelUpstreamRecorder,
    };

    std::optional<ClaudeCodeResolvedWorkspace> workspace = initialWorkspace;
    std::optional<ClaudeCodeAccountBinding> accountBinding = options.accountBinding;
    PerformClaudeCodeMessagesAttemptOptions firstAttempt = attemptOptions;
    firstAttempt.workspace = workspace;
    rt::Promise<ClaudeCodeMessagesAttempt> attempting(performClaudeCodeMessagesAttempt(std::move(firstAttempt)));
    ClaudeCodeMessagesAttempt attempt = co_await attempting.wait();
    // An organization-scoped key rejects the first request with a workspace
    // requirement; one discovery call (cached in KV) resolves it for the whole
    // deployment without a per-request lookup afterwards.
    // Disabled thinking was proven for the original binding, not a newly found
    // workspace. Preserve its first failure rather than transfer that permission.
    const json::value* thinking = rt::get(native->body, "thinking");
    const json::value* thinkingType = utils::isRecord(thinking) ? rt::get(*thinking, "type") : nullptr;
    const bool thinkingDisabled = thinkingType != nullptr && thinkingType->is_string() && thinkingType->get_string() == "disabled";
    if (attempt.classification.kind == "workspace_required" && !workspace && !thinkingDisabled) {
      DiscoverClaudeCodeWorkspaceOptions discoverOptions{
        .credential = ClaudeCodeAuthHeaders{.kind = credential.kind, .secret = credential.secret},
        .kv = kv,
        .fetcher = options.fetcher,
        .signal = options.signal,
        .accountBinding = options.accountBinding,
      };
      std::optional<ClaudeCodeWorkspaceDiscoveryResult> discovered;
      try {
        rt::Promise<ClaudeCodeWorkspaceDiscoveryResult> discovering(discoverClaudeCodeWorkspace(std::move(discoverOptions)));
        discovered = co_await discovering.wait();
      } catch (...) {
        discovered = std::nullopt;
      }
      if (discovered && std::holds_alternative<ClaudeCodeWorkspaceDiscoveryResultOk>(*discovered)) {
        const ClaudeCodeWorkspaceDiscoveryResultOk& found = std::get<ClaudeCodeWorkspaceDiscoveryResultOk>(*discovered);
        workspace = found.workspace;
        if (found.account_binding) accountBinding = found.account_binding;
        if (accountBinding) {
          rt::Promise<bool> checking(claude_code_accounts::isClaudeCodeAccountCurrent(kv, *accountBinding));
          const bool current = co_await checking.wait();
          if (!current) throw ClaudeCodeError("Claude account changed during workspace discovery.", "claude_code_request_invalid", 400);
        }
        // The retry is the same client dispatch: the single-use pre-transport
        // reservation and the first-provider attribution already happened for
        // this request and must not be claimed twice.
        PerformClaudeCodeMessagesAttemptOptions retryAttempt = attemptOptions;
        retryAttempt.workspace = workspace;
        retryAttempt.beforeDispatch = nullptr;
        retryAttempt.onDispatch = nullptr;
        rt::Promise<ClaudeCodeMessagesAttempt> retrying(performClaudeCodeMessagesAttempt(std::move(retryAttempt)));
        attempt = co_await retrying.wait();
      }
    }

    const json::object& object = body.as_object();
    anthropic_responses::AnthropicResponsesOptions normalizeOptions;
    normalizeOptions.model = rt::toString(rt::get(object, "model"));
    normalizeOptions.requestId = attempt.requestId;
    normalizeOptions.signal = options.signal;
    normalizeOptions.reasoningSummary = responseReasoningSummary(object);
    normalizeOptions.reasoningReplayScope = options.reasoningReplayScope;
    normalizeOptions.provider = "claude_code";
    normalizeOptions.toolNames = native->toolNames;
    rt::Promise<rt::Response> normalizing(anthropic_responses::normalizeAnthropicResponsesResponse(attempt.response, std::move(normalizeOptions)));
    const rt::Response delivered = co_await normalizing.wait();
    co_return ClaudeCodeResponsesResult{
      .account_binding = accountBinding,
      .observed_at_ms = attempt.observedAtMs,
      .response = delivered,
      .request_id = attempt.requestId,
      .credential = credential,
      .workspace = workspace,
      .prompt_cache_mode = native->promptCacheMode,
      .explicit_breakpoint_count = native->explicitBreakpointCount,
      .quota = attempt.quota,
      .classification = attempt.classification,
    };
  }
  rt::Task<ClaudeCodeResponsesResult> fetchClaudeCodeResponses(json::value body) {
    return fetchClaudeCodeResponses(std::move(body), ClaudeCodeAuthenticatedFetchOptions());
  }

  claude_code_credentials::ClaudeCodeCredentialKind claudeCodeCredentialKind(const std::string& secret) {
    return claude_code_credentials::classifyClaudeCodeCredentialKind(secret);
  }
}
