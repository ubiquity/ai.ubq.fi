#pragma once
// Port of src/openai-telemetry.ts.
// Response usage, provider and stream telemetry, extracted from src/openai.ts.
//
// ResponseTelemetryState is one mutable JS object shared by the request's UsageContext and the
// `responseTelemetry` WeakMap entry of its Response: it is held by std::shared_ptr and mutated
// through it. `context: UsageContext | undefined` is `const UsageContext*` (nullptr is undefined);
// the recorders only mutate the shared state behind `context->responseTelemetry`. The functions
// that return `Promise.resolve()` (not async) do their work when called and return an
// already-settled rt::Promise, so `await` keeps its JS turn.
#include "uos/api_key_policy.hpp"
#include "uos/paid_fallback/index.hpp"
#include "uos/responses_failover_stream.hpp"
#include "uos/responses_stream.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/rt/weak.hpp"
#include "uos/sentinel/upstream_capture.hpp"
#include "uos/types.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::openai_telemetry {
  struct UsageTokens;
  struct ResponseTelemetryState;

  // `Readonly<{ terminal: (eventType: string, usage?: UsageTokens | null) => void; ambiguous; cancelled }>`.
  // `usage` std::nullopt is null or omitted (the TS reads both alike).
  struct MeteredTransportLifecycle {
    std::function<void(std::string eventType, std::optional<UsageTokens> usage)> terminal;
    std::function<void()> ambiguous;
    std::function<void()> cancelled;
  };

  using PaidProviderHealthEvent = std::string;  // "auth_invalid" | "quota_exhausted" | "upstream_error" | "reachable"

  struct PaidProviderHealthClassification {
    PaidProviderHealthEvent event;
    std::optional<double> status;
    friend bool operator==(const PaidProviderHealthClassification&, const PaidProviderHealthClassification&) = default;
  };

  struct UsageContext {
    // `{ owner: string; repo: string }`
    struct KernelRepo {
      std::string owner;
      std::string repo;
      friend bool operator==(const KernelRepo&, const KernelRepo&) = default;
    };
    // `{ owner: string }`
    struct KernelOrg {
      std::string owner;
      friend bool operator==(const KernelOrg&, const KernelOrg&) = default;
    };

    std::optional<std::string> keyId;
    std::optional<KernelRepo> kernelRepo;
    std::optional<KernelOrg> kernelOrg;
    std::optional<bool> paidFallbackEnabled{};
    rt::Nullable<std::string> idempotencyPrincipal{};
    std::optional<std::string> requestId{};
    /** Internal paid-attempt ledger identity; client correlation and request quota retain requestId. */
    std::optional<std::string> paidFallbackRequestId{};
    std::optional<double> startedAtMs{};
    std::optional<double> startedAtMonotonicMs{};
    std::optional<rt::AbortSignal> downstreamSignal{};
    // nullptr is undefined.
    std::shared_ptr<ResponseTelemetryState> responseTelemetry{};
    /** Commits an admitted API-key reservation exactly once before transport. */
    // An empty function is undefined.
    api_key_policy::ApiKeyUsageReservation::BeforeProviderDispatch beforeProviderDispatch{};
    /** Request-owned passive upstream recorder for accepted inference requests. */
    std::optional<sentinel::upstream_capture::SentinelUpstreamRecorder> sentinelUpstreamRecorder{};
    /** Test seam for proving one terminal usage observation per response. */
    // `(usage: UsageTokens | null, completed: boolean) => void`; an empty function is undefined.
    std::function<void(std::optional<UsageTokens> usage, bool completed)> onTerminalUsage{};
  };

  // "cerebras" | "chatgpt_codex" | "claude_code" | "deepseek" | "gemini" | "lithos" | "openrouter" | "removed_provider" | "metered" | "surplus"
  using UpstreamProvider = std::string;
  bool supportsReasoningProgressRelease(const UpstreamProvider& provider);
  using InferenceFallbackReason = std::string;  // "primary_quota_blocked" | "primary_subscriptions_unavailable" | "dynamic_paid_model"
  /**
   * Exact fallback reason for one failed hop of the `ubiquity/deepseek-flash`
   * waterfall, so production traffic keeps the same per-hop evidence the
   * benchmark recorded (provider plus failure class or status). The paid
   * admission reasons stay their own union; only the telemetry record carries
   * either kind.
   */
  // `deepseek_waterfall:${"lithos" | "deepseek" | "openrouter" | "surplus" | "openlux"}:${number | "transport_failure" | "stream_failure"}`
  using DeepSeekWaterfallFallbackReason = std::string;
  using RecordedFallbackReason = std::string;  // InferenceFallbackReason | DeepSeekWaterfallFallbackReason
  using UsageTelemetryStatus = std::string;    // "missing" | "partial" | "reported" | "invalid"
  using PromptCacheMode = std::string;         // "implicit" | "explicit" | "legacy_retention" | "unspecified"
  // "quota_exhausted" | "credential_invalid" | "account_removed_or_replaced" | "model_unavailable" | "manual_reset" | null
  using ActiveTransitionReason = std::optional<std::string>;

  // "response.completed" | "response.failed" | "response.incomplete" | "error" | "eof" | "cancelled" | "deadline"
  using ResponseStreamTerminalType = std::string;

  struct ResponseTelemetry {
    std::string provider;
    std::optional<RecordedFallbackReason> fallbackReason;
    std::optional<std::string> model;
    std::optional<std::string> reasoning;
    std::optional<double> outputTokenAllowance;
    /** Total time this request spent waiting for a provider's own retry window. */
    std::optional<double> rateLimitWaitMs;
    /** The sibling tier that served this request after its own tier refused it. */
    std::optional<std::string> rateLimitFailoverModel;
    std::optional<double> inputTokens;
    std::optional<double> cachedInputTokens;
    std::optional<double> cacheWriteInputTokens;
    std::optional<double> outputTokens;
    std::optional<double> totalTokens;
    bool usageObserved;
    UsageTelemetryStatus usageTelemetryStatus;
    bool promptCacheKeyPresent;
    PromptCacheMode promptCacheMode;
    double explicitBreakpointCount;
    std::optional<double> accountSlot;
    std::optional<double> activeGeneration;
    ActiveTransitionReason activeTransitionReason;
    rt::Nullable<double> quotaUsedPercent;  // number | null | undefined
    bool completed;
    std::optional<bool> semanticOutputObserved;
    std::vector<std::string> upstreamEventKinds;
    std::optional<ResponseStreamTerminalType> streamTerminalType;
    std::optional<std::string> failureKind;
    bool responseCreatedObserved;
    std::optional<std::string> syntheticTerminalType;  // "response.failed" | "error" | null
    std::optional<bool> stream;
    std::optional<std::string> providerRequestId;
    std::optional<double> firstProviderDispatchMs;
    std::optional<double> firstProviderHeadersMs;
    std::optional<double> firstCodexDispatchMs;
    std::optional<double> firstCodexHeadersMs;
    std::optional<double> firstUpstreamSseEventMs;
    std::optional<double> firstSemanticCommitmentMs;
    std::optional<double> streamTerminalMs;
    std::vector<std::string> attemptedProviders;
    std::optional<std::string> removedProviderTriggerClass;
    std::optional<std::string> removedProviderCircuitTransition;
    std::optional<std::string> removedProviderSelectedModel;
    std::optional<std::string> removedProviderTaskType;
    std::optional<std::string> removedProviderSemanticCommitment;
    std::optional<double> removedProviderLatencyMs;
    std::optional<std::string> removedProviderTerminalStatus;
    std::size_t inStreamRetries = 0;
    json::array upstreamFailureEvents{};
    bool upstreamAuthFollowerStale = false;
    friend bool operator==(const ResponseTelemetry&, const ResponseTelemetry&) = default;
  };

  struct ResponseTelemetryState {
    std::optional<std::string> provider;
    std::optional<RecordedFallbackReason> fallbackReason;
    std::optional<std::string> model;
    std::optional<std::string> reasoning;
    std::optional<double> outputTokenAllowance;
    /** Total time this request spent waiting for a provider's own retry window. */
    std::optional<double> rateLimitWaitMs;
    /** The sibling tier that served this request after its own tier refused it. */
    std::optional<std::string> rateLimitFailoverModel;
    std::optional<double> inputTokens;
    std::optional<double> cachedInputTokens;
    std::optional<double> cacheWriteInputTokens;
    std::optional<double> outputTokens;
    std::optional<double> totalTokens;
    bool usageObserved;
    UsageTelemetryStatus usageTelemetryStatus;
    bool promptCacheKeyPresent;
    PromptCacheMode promptCacheMode;
    double explicitBreakpointCount;
    std::optional<double> accountSlot;
    std::optional<std::string> accountCohortId;
    std::optional<double> activeGeneration;
    ActiveTransitionReason activeTransitionReason;
    rt::Nullable<double> quotaUsedPercent;  // number | null | undefined
    bool completed;
    std::optional<bool> semanticOutputObserved;
    std::vector<std::string> upstreamEventKinds;
    std::optional<ResponseStreamTerminalType> streamTerminalType;
    std::optional<std::string> failureKind;
    bool responseCreatedObserved;
    std::optional<std::string> syntheticTerminalType;  // "response.failed" | "error" | null
    std::optional<bool> stream;
    std::optional<std::string> providerRequestId;
    std::optional<double> firstProviderDispatchMs;
    std::optional<double> firstProviderHeadersMs;
    std::optional<double> firstCodexDispatchMs;
    std::optional<double> firstCodexHeadersMs;
    std::optional<double> firstUpstreamSseEventMs;
    std::optional<double> firstSemanticCommitmentMs;
    std::optional<double> streamTerminalMs;
    std::vector<std::string> attemptedProviders;
    std::optional<std::string> removedProviderTriggerClass;
    std::optional<std::string> removedProviderCircuitTransition;
    std::optional<std::string> removedProviderSelectedModel;
    std::optional<std::string> removedProviderTaskType;
    std::optional<std::string> removedProviderSemanticCommitment;
    std::optional<double> removedProviderLatencyMs;
    std::optional<std::string> removedProviderTerminalStatus;
    std::size_t inStreamRetries = 0;
    json::array upstreamFailureEvents{};
    bool upstreamAuthFollowerStale = false;
    friend bool operator==(const ResponseTelemetryState&, const ResponseTelemetryState&) = default;
  };

  // `export const responseTelemetry = new WeakMap<Response, ResponseTelemetryState>()`: an exported
  // mutable container, defined in the .cpp. Values are the shared state objects.
  extern rt::WeakMap<rt::Response, std::shared_ptr<ResponseTelemetryState>> responseTelemetry;

  // A fresh state object (the TS object literal), shared from here on.
  std::shared_ptr<ResponseTelemetryState> createResponseTelemetryState();

  rt::Response attachResponseTelemetry(rt::Response response, std::shared_ptr<ResponseTelemetryState> state);

  /**
   * Attaches completed, answer-bearing telemetry to a gateway-produced Responses
   * result that did not come from a provider dispatch (the local Jev compaction
   * answer). The terminal wrapper's normal completion decision reads this state,
   * so the request settles as completed through the existing handoff instead of
   * being classified as a stream that ended without a completion. No token,
   * allowance, model-cost or provider-usage counter is invented: those fields stay
   * null/missing because no main-model call measured them, and the provider label
   * remains the honest gateway default. Timing fields stay unknown too: there is
   * no provider-relative stream or semantic-commitment instant here, and a
   * wall-clock stamp would be misread as request-relative latency.
   */
  rt::Response setResponseCompletionTelemetry(rt::Response response, bool stream);

  rt::Response aggregateResponseTelemetry(const std::vector<rt::Response>& sources, rt::Response target);

  // std::nullopt is null.
  std::optional<ResponseTelemetry> getResponseTelemetry(const rt::Response& response);

  /** Stable pseudonymous account identity used only by aggregate cache telemetry. */
  std::optional<std::string> getResponseAccountCohortId(const rt::Response& response);

  void recordAttemptedProvider(const UsageContext* context, const std::string& provider);

  void selectRemovedProviderTelemetry(const UsageContext* context);

  void recordFirstCodexDispatch(const UsageContext* context);

  void recordFirstCodexHeaders(const UsageContext* context);

  void recordFirstProviderDispatch(const UsageContext* context);

  void recordFirstProviderHeaders(const UsageContext* context);

  // The `fields` of recordRemovedProviderFields, a `Readonly<Record<string, string | number | null |
  // undefined>>` of which the function reads these members (cast to `string | null` / `number | null`):
  // undefined leaves the telemetry field as it is, null clears it.
  struct RecordRemovedProviderFieldsFields {
    rt::Nullable<std::string> triggerClass{};
    rt::Nullable<std::string> circuitTransition{};
    rt::Nullable<std::string> selectedModel{};
    rt::Nullable<std::string> taskType{};
    rt::Nullable<std::string> semanticCommitment{};
    rt::Nullable<double> latencyMs{};
    rt::Nullable<std::string> terminalStatus{};
    friend bool operator==(const RecordRemovedProviderFieldsFields&, const RecordRemovedProviderFieldsFields&) = default;
  };

  void recordRemovedProviderFields(const UsageContext* context, const RecordRemovedProviderFieldsFields& fields);

  // `recordRemovedProviderTelemetry({}).catch(() => {})`: never rejects.
  rt::Promise<void> persistRemovedProviderFields(const UsageContext* _context);

  rt::Promise<void> persistFailedRemovedProviderAttempt(const UsageContext* context, double _startedAtMonotonicMs, const std::string& triggerClass);

  void recordFirstUpstreamSseEvent(const UsageContext* context);

  void recordFirstSemanticCommitment(const UsageContext* context);

  void recordStreamTerminal(const UsageContext* context);

  // `string | null`: std::nullopt is null.
  std::optional<std::string> responseIncompleteFailureKind(const responses_stream::ResponsesStreamEvent& event);

  void recordResponsesEventTelemetry(const UsageContext* context, const responses_stream::ResponsesStreamEvent& event);
  void recordUpstreamResponsesFailure(ResponseTelemetryState& telemetry, const responses_stream::ResponsesStreamEvent& event);

  // `error: unknown` is the caught reason; `details?`: nullptr is undefined.
  void recordResponsesFailureTelemetry(const UsageContext* context, const rt::Reason& error,
                                       const responses_failover_stream::OwnedResponsesStreamFailureDetails* details = nullptr);

  // `run: (context: UsageContext) => Promise<Response>`.
  rt::Task<rt::Response> runWithResponseTelemetry(std::optional<UsageContext> context, std::function<rt::Task<rt::Response>(UsageContext context)> run);

  struct RoutedResponsesUpstream {
    rt::Response response;
    UpstreamProvider provider;
    std::optional<paid_fallback::index::PaidFallbackReservation> paidFallback;
    rt::Nullable<paid_fallback::index::SurplusBillingPricing> paidFallbackBilling{};
    /** Trustworthy opaque identifier supplied by the selected upstream. */
    rt::Nullable<std::string> paidFallbackProviderRequestId{};
    /**
     * Health classification of a delivered non-2xx paid response, so the terminal
     * transport record preserves the provider-specific signal.
     */
    std::optional<PaidProviderHealthClassification> paidFallbackErrorHealth{};
    bool gatewayResponse;
    std::optional<RecordedFallbackReason> fallbackReason;
    /** Local admission decisions are terminal and must not enter legacy recovery. */
    std::optional<bool> allowRemovedProviderRecovery{};  // `?: false`
    /** Record stream health even though this free route has no paid reservation. */
    std::optional<bool> providerHealthOnly{};
  };

  struct UsageTokens {
    std::optional<double> inputTokens;
    std::optional<double> cachedInputTokens;
    std::optional<double> cacheWriteInputTokens;
    std::optional<double> outputTokens;
    std::optional<double> totalTokens;
    UsageTelemetryStatus status;
    /** Exact Surplus charge in microcredits; null means a present malformed amount. */
    rt::Nullable<double> buyerCostMicro{};
    friend bool operator==(const UsageTokens&, const UsageTokens&) = default;
  };

  /**
   * Normalizes terminal Responses usage for gateway telemetry. The fixed
   * cache-scope experiment reuses this parser so it cannot invent a divergent
   * interpretation of cache-read or cache-write fields.
   */
  // `value: unknown`: nullptr is undefined. std::nullopt is null.
  std::optional<UsageTokens> extractUsageTokens(const json::value* value);
  std::optional<UsageTokens> extractUsageTokens(const json::value& value);

  // `usage: UsageTokens | null`; `Record<string, unknown> | null` (std::nullopt is null).
  std::optional<json::object> toChatUsage(const std::optional<UsageTokens>& usage);

  PromptCacheMode promptCacheModeFor(const json::object& rawRecord);

  double countExplicitPromptCacheBreakpoints(const std::vector<::uos::types::ResponseInputItem>& input);

  bool promptCacheKeyPresent(const json::object& rawRecord);

  /**
   * Cache-read telemetry for the special upstreams that answer with Chat
   * Completions usage. Their transports publish the provider's cache counter
   * under the official `prompt_tokens_details.cached_tokens` field, and the
   * Responses parser owns the interpretation of that field so the two routes
   * cannot report cache reads through divergent rules.
   *
   * An upstream that reports no counter leaves `input_tokens_details` absent,
   * which the parser reports as `partial`: unmeasured is never published as a
   * measured zero.
   */
  std::optional<UsageTokens> extractChatUsageTokens(const json::value* value);
  std::optional<UsageTokens> extractChatUsageTokens(const json::value& value);

  // The `details` of recordRequestUsage.
  struct RecordRequestUsageDetails {
    std::string model;
    std::string route;
    bool stream;
    std::optional<std::string> reasoning;
    std::optional<bool> promptCacheKeyPresent{};
    std::optional<PromptCacheMode> promptCacheMode{};
    std::optional<double> explicitBreakpointCount{};
    friend bool operator==(const RecordRequestUsageDetails&, const RecordRequestUsageDetails&) = default;
  };

  rt::Promise<void> recordRequestUsage(const UsageContext* context, const RecordRequestUsageDetails& details);

  // `usage: UsageTokens | null`, `completed = true`.
  rt::Promise<void> recordCompletionUsage(const UsageContext* context, const std::optional<UsageTokens>& usage, bool completed = true);

  void recordTerminalUsage(const UsageContext* context, const std::optional<UsageTokens>& usage, bool completed);

  rt::Promise<void> recordErrorUsage(const UsageContext* _context);

  void recordStreamTerminalType(const UsageContext* context, const ResponseStreamTerminalType& terminalType);

  // `error: unknown` is the caught reason.
  ResponseStreamTerminalType classifyStreamFailure(const rt::Reason& error, const rt::AbortSignal& signal, const rt::AbortSignal& downstreamSignal);

  // `(...values: readonly unknown[])`: each value is a caught error or an AbortSignal reason (a null
  // Reason is undefined).
  bool isTimeoutFailure(const std::vector<rt::Reason>& values);
  template <class... Reasons>
  bool isTimeoutFailure(const Reasons&... values) {
    const std::vector<rt::Reason> collected{rt::Reason(values)...};
    return isTimeoutFailure(collected);
  }

  ResponseStreamTerminalType classifyPreHeaderFailure(const rt::Reason& error, const rt::AbortSignal& signal, const rt::AbortSignal& downstreamSignal);

  // `type?: string` (std::nullopt is undefined), `param?: string | null` (undefined leaves `param` out
  // of the error options; null is written as `param: null`).
  rt::Response streamErrorResponse(double status, const std::string& message, const std::string& code, const UpstreamProvider& provider,
                                   const std::vector<std::string>& warnings);
  rt::Response streamErrorResponse(double status, const std::string& message, const std::string& code, const UpstreamProvider& provider,
                                   const std::vector<std::string>& warnings, std::optional<std::string> type);
  rt::Response streamErrorResponse(double status, const std::string& message, const std::string& code, const UpstreamProvider& provider,
                                   const std::vector<std::string>& warnings, std::optional<std::string> type, rt::Nullable<std::string> param);

  // `warnings = []`: the two-argument overload has none.
  rt::Response streamPreflightFailureResponse(const ResponseStreamTerminalType& terminalType, const UpstreamProvider& provider);
  rt::Response streamPreflightFailureResponse(const ResponseStreamTerminalType& terminalType, const UpstreamProvider& provider,
                                              const std::vector<std::string>& warnings);
}
