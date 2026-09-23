import {
  buildCodexRequest,
  fetchCodexResponses,
  getCodexModelsSnapshotDefaultModel,
  markCodexResponseCompleted,
  markCodexResponseUpstreamError,
  releaseCodexResponseProbe,
} from "./codex.ts";
import {
  CEREBRAS_GPT_OSS_120B_MODEL,
  CerebrasError,
  fetchCerebrasChatCompletions,
  getCerebrasProviderRequestId,
  normalizeCerebrasChatCompletion,
  normalizeCerebrasProviderRequestId,
} from "./cerebras.ts";
import {
  DEEPSEEK_DEFAULT_REASONING_EFFORT,
  DeepSeekError,
  DeepSeekStreamError,
  deepSeekThinkingModeActive,
  deepSeekUpstreamModelFor,
  fetchDeepSeekChatCompletions,
  getDeepSeekProviderRequestId,
  iterateDeepSeekChatCompletionStream,
  normalizeDeepSeekChatCompletion,
} from "./deepseek.ts";
import { DEEPSEEK_RESPONSES_PROFILE } from "./deepseek_responses.ts";
import { lithosUpstreamModelFor } from "./lithos.ts";

import { isProviderEnabled, loadProviderSelectionCached } from "./provider_selection.ts";
import { ApiKeyQuotaDispatchError } from "./api_key_policy.ts";
import { DEFAULT_REASONING_EFFORT, type ReasoningEffort } from "./defaults.ts";
import { readBoundedResponseBody } from "./bounded_response_body.ts";
import { json, openaiError } from "./http.ts";
import {
  BUFFERED_INFERENCE_DEADLINE_MS,
  createInferenceSignal,
  createStreamFirstEventDeadline,
  createStreamSemanticDeadline,
  STREAM_FAILOVER_RESERVE_MS,
  type StreamDeadline,
} from "./inference_deadline.ts";
import { loadRuntimeConfig } from "./runtime_config.ts";
import {} from "./model_metadata.ts";
import { CHAT_COMPLETIONS_REQUEST_KEYS, RESPONSES_REQUEST_KEYS } from "./openai_schema.ts";
import { readJsonBody } from "./request.ts";
import {
  type PreflightedResponsesStream,
  preflightResponsesStream,
  ResponsesStreamError,
  type ResponsesStreamEvent,
  type ResponsesStreamIterator,
  withSseKeepalive,
} from "./responses_stream.ts";
import {
  deriveRemovedProviderSessionId,
  isEligibleRemovedProviderModel,
  readRemovedProviderApiKey,
  removedProviderModelFromEvent,
} from "./removed_provider.ts";
import {
  claimRemovedProviderEarlyRecoveryProbe,
  closeRemovedProviderCircuit,
  recordRemovedProviderEligibleFailure,
  releaseRemovedProviderCircuitProbe as releaseGlobalRemovedProviderProbe,
  type RemovedProviderCircuitProbe,
  renewRemovedProviderCircuitProbe,
  selectRemovedProviderCircuitRoute,
} from "./removed_provider_circuit.ts";
import {} from "./removed_provider_telemetry.ts";
import {
  createOwnedResponsesStream,
  isGatewayFailoverWarningItem,
  isSyntheticResponsesFailureEvent,
  type PreparedResponsesStream,
  prepareResponsesStreamForCommit,
} from "./responses_failover_stream.ts";
import {} from "./paid_fallback.ts";
import { recordCerebrasProviderHealth, recordDeepSeekProviderHealth } from "./provider_health.ts";
import { getString, isRecord } from "./utils.ts";
import type { ChatCompletionRequest, MessageContentItem, ResponseInputItem, ResponsesRequest } from "./types.ts";
import { loadDebugRoutingConfig } from "./debug_routing.ts";
import type {} from "./sentinel_upstream_capture.ts";

import {
  ActiveTransitionReason,
  ResponseStreamTerminalType,
  RoutedResponsesUpstream,
  UpstreamProvider,
  UsageContext,
  UsageTokens,
  attachResponseTelemetry,
  classifyPreHeaderFailure,
  classifyStreamFailure,
  countExplicitPromptCacheBreakpoints,
  createResponseTelemetryState,
  extractChatUsageTokens,
  extractUsageTokens,
  isTimeoutFailure,
  persistFailedRemovedProviderAttempt,
  persistRemovedProviderFields,
  promptCacheKeyPresent,
  promptCacheModeFor,
  recordAttemptedProvider,
  recordCompletionUsage,
  recordErrorUsage,
  recordFirstProviderDispatch,
  recordFirstProviderHeaders,
  recordFirstSemanticCommitment,
  recordFirstUpstreamSseEvent,
  recordRemovedProviderFields,
  recordRequestUsage,
  recordResponsesEventTelemetry,
  recordResponsesFailureTelemetry,
  recordStreamTerminal,
  recordStreamTerminalType,
  recordTerminalUsage,
  runWithResponseTelemetry,
  selectRemovedProviderTelemetry,
  streamErrorResponse,
  streamPreflightFailureResponse,
  supportsReasoningProgressRelease,
  MeteredTransportLifecycle,
} from "./openai_telemetry.ts";
import {
  normalizeChatMessage,
  normalizeFunctionCallOutputItem,
  normalizeModelForCodex,
  normalizeResponseContentItem,
  normalizeResponseMessageItem,
  validatePromptCacheControls,
} from "./input_normalization.ts";
import {
  EMPTY_UPSTREAM_COMPLETION_MESSAGE,
  chatSourceFromPrepared,
  markChatSemanticOutput,
  preparedChatCompletionIsEmpty,
  recordEmptyUpstreamCompletion,
  recordResponsesTerminal,
} from "./chat_stream_translation.ts";
import {
  GPT_OSS_STREAM_DOWNGRADED_WARNING,
  cerebrasResponseHeaders,
  chatCompletionHasAnswerBearingOutput,
  deepSeekChatBodyDiagnostic,
  deepseekResponseHeaders,
  logRedactedUpstreamError,
  streamCerebrasChatCompletion,
  toCerebrasErrorResponse,
  toCerebrasUpstreamErrorResponse,
  toDeepSeekErrorResponse,
  toDeepSeekUpstreamErrorResponse,
  toOpenAiUpstreamErrorResponse,
  toPreHeaderErrorResponse,
} from "./upstream_wire.ts";
import {
  isTemporaryFreeSurplusModel,
  CodexModelMetadata,
  CodexModelReasoning,
  PassthroughToolSchemaKey,
  WARNING_KEY_MAP,
  applyPassthroughToCodexRequest,
  buildIgnoredWarnings,
  getCodexModelMetadata,
  normalizeReasoningParamForCodex,
  parseChatStreamOptions,
  parseMaxCompletionTokensField,
  parseReasoningEffortField,
  parseReasoningParam,
  parseStreamField,
  reasoningEffortForCodexRequest,
  resolveDefaultReasoningLabel,
  resolveReasoningLabelFromEffort,
  resolveReasoningLabelFromParam,
  responseWarnings,
  validateCodexModelAvailable,
  validateKnownUnsupportedPromptCacheUse,
  withUosWarning,
} from "./request_policy.ts";
const temporaryFreeSurplusCapabilityError = (model: string, body: Record<string, unknown>): Response | null =>
  isTemporaryFreeSurplusModel(model) && Array.isArray(body.tools) && body.tools.length > 0
    ? openaiError(400, `The model '${model}' does not support tools through this gateway.`, "unsupported_model_capability", { param: "tools" })
    : null;

export const getDefaultModel = async (): Promise<string | null> => {
  const runtime = await loadRuntimeConfig();
  return runtime?.default_model ?? getCodexModelsSnapshotDefaultModel(runtime?.codex_models ?? null);
};

export const downstreamSignalFor = (request: Request, context?: UsageContext): AbortSignal => context?.downstreamSignal ?? request.signal;

export const inferenceSignal = (request: Request, context?: UsageContext): AbortSignal => createInferenceSignal(downstreamSignalFor(request, context));

/**
 * Internal test seam for exercising the public OpenAI handlers through the
 * same guarded banked-reset flow. It has no request-schema or runtime-config
 * surface, and remains unset in production.
 */
type CodexBankedResetOptionsForTest = NonNullable<Parameters<typeof fetchCodexResponses>[1]>["bankedReset"];
export let codexBankedResetOptionsForTest: CodexBankedResetOptionsForTest | null = null;

export const setCodexBankedResetOptionsForTest = (options: CodexBankedResetOptionsForTest | null): void => {
  codexBankedResetOptionsForTest = options;
};

const defaultModelUnavailableError = (): Response =>
  openaiError(503, "Default model is unavailable: no configured default model or Codex model snapshot.", "server_error");

const getDefaultReasoningEffort = async (): Promise<ReasoningEffort> => {
  return (await loadRuntimeConfig())?.default_reasoning_effort ?? DEFAULT_REASONING_EFFORT;
};
import {
  FailedResponsesAttempt,
  PreparedResponsesAttempt,
  ResponsesRouteAttempt,
  ResponsesRouteFailure,
  failureKindForResponsesAttemptTrigger,
  fetchAndPreparePrimaryResponses,
  fetchAndPrepareRemovedProviderResponses,
  finalizeAbandonedPrimaryAttempt,
  isEligibleResponsesAttemptStatus,
  markPrimarySemanticRecovery,
  responseFailureTerminalType,
} from "./responses_attempts.ts";
import { collectBufferedResponses } from "./responses_buffered.ts";
import { canAttemptPaidFallback, createMeteredTransportLifecycle } from "./paid_fallback_health.ts";
import { fetchResponsesWithPaidFallback } from "./paid_fallback_routing.ts";
const CHAT_COMPLETIONS_ALLOWED_KEYS = new Set(CHAT_COMPLETIONS_REQUEST_KEYS);
const RESPONSES_ALLOWED_KEYS = new Set(RESPONSES_REQUEST_KEYS);
const CODEX_RESPONSES_EXTENSION_KEYS = new Set(["client_metadata"]);
/**
 * Fields a first-party DeepSeek client sends on the provider's own Chat
 * contract that the official OpenAI Chat schema does not define. Kept separate
 * from the OpenAI allowlist so the compatibility surface stays explicit and
 * cannot silently widen the OpenAI-compatible routes; see AGENTS.md's Codex CLI
 * compatibility rule for the same pattern.
 */
const DEEPSEEK_CHAT_EXTENSION_KEYS = new Set(["thinking"]);

export const findUnknownKey = (record: Record<string, unknown>, allowed: ReadonlySet<string>, extensions?: ReadonlySet<string>): string | null => {
  for (const key of Object.keys(record)) {
    if (!allowed.has(key) && !extensions?.has(key)) return key;
  }
  return null;
};
import { completeChatCompletions, streamChatCompletions } from "./chat_completions_primary.ts";
export const recordDeepSeekResponseHealth = (status: number, providerRequestId: string | null): void => {
  if (status === 401 || status === 403) {
    void recordDeepSeekProviderHealth("auth_invalid", status, Date.now, providerRequestId);
    return;
  }
  if (status === 429) {
    void recordDeepSeekProviderHealth("quota_exhausted", status, Date.now, providerRequestId);
    return;
  }
  if (status >= 500) {
    void recordDeepSeekProviderHealth("upstream_error", status, Date.now, providerRequestId);
    return;
  }
  if (status >= 400) {
    void recordDeepSeekProviderHealth("reachable", status, Date.now, providerRequestId);
    return;
  }
  void recordDeepSeekProviderHealth("success", status, Date.now, providerRequestId);
};

/**
 * Provider health for the LithosAI route.
 *
 * `402 insufficient_quota` gets its own explicit branch instead of falling into
 * the generic failure bucket. It is still a quota event (`quota_exhausted`), but
 * the recorded status keeps "out of credit" distinguishable from "rate
 * limited", and unlike a 429 it is terminal for the account rather than a
 * transient per-minute budget. The comparison statuses are the vendor's own:
 * `401`/`403` are auth, `>=500` is an upstream fault, any other `4xx` is a
 * reachable provider answering a client-shaped error.
 */

/**
 * Fails a buffered DeepSeek Responses completion closed when the provider
 * reported success but the translated output carries nothing a client can act
 * on. This is the buffered counterpart of the streamed G3 guard: one request
 * shape must not report success on one transport and failure on the other.
 *
 * Response headers are still unsent on this branch, so the client gets the
 * ordinary `empty_upstream_completion` 502 the non-DeepSeek routes already
 * return rather than a synthetic terminal event.
 */
export const respondDeepSeekEmptyBufferedCompletion = (
  usageContext: UsageContext | undefined,
  usage: UsageTokens | null,
  upstreamStatus: number,
  providerRequestId: string | null
): Response => {
  if (usageContext?.responseTelemetry) {
    usageContext.responseTelemetry.failureKind = "empty_upstream_completion";
    usageContext.responseTelemetry.semanticOutputObserved = false;
  }
  recordTerminalUsage(usageContext, usage, false);
  recordStreamTerminalType(usageContext, "response.failed");
  recordDeepSeekResponseHealth(upstreamStatus, providerRequestId);
  return openaiError(502, EMPTY_UPSTREAM_COMPLETION_MESSAGE, "empty_upstream_completion", {
    type: "server_error",
    headers: deepseekResponseHeaders(providerRequestId),
  });
};

/**
 * Records a buffered DeepSeek Responses terminal. The payload carries the
 * provider's own terminal, so telemetry reports the terminal the client
 * receives instead of assuming success, and the non-completed classification
 * matches the streamed path.
 *
 * `completed` is derived from that same terminal: recording the usage counters
 * as a completion before reading the payload's status would persist a
 * truncated reply as a completed one.
 */
export const recordBufferedDeepSeekResponsesTerminal = (
  usageContext: UsageContext | undefined,
  payload: Record<string, unknown>,
  usage: UsageTokens | null,
  upstreamStatus: number,
  providerRequestId: string | null
): void => {
  const terminalType = deepSeekTerminalTypeForPayload(payload.status);
  recordTerminalUsage(usageContext, usage, terminalType === "response.completed");
  recordStreamTerminalType(usageContext, terminalType);
  if (terminalType === "response.completed") {
    recordStreamTerminal(usageContext);
  } else {
    recordDeepSeekFailureKind(usageContext, terminalType === "response.incomplete" ? "incomplete_response" : "upstream_error");
    void recordDeepSeekProviderHealth("upstream_error", upstreamStatus, Date.now, providerRequestId);
  }
  recordDeepSeekResponseHealth(upstreamStatus, providerRequestId);
};

/** The gateway terminal a buffered DeepSeek Responses payload's status implies. */
export const deepSeekTerminalTypeForPayload = (status: unknown): ResponseStreamTerminalType => {
  if (status === "incomplete") return "response.incomplete";
  if (status === "failed") return "response.failed";
  return "response.completed";
};

const deepSeekTerminalTypeForError = (error: unknown, downstreamSignal: AbortSignal): ResponseStreamTerminalType => {
  if (downstreamSignal.aborted) return "cancelled";
  if (error instanceof DeepSeekStreamError) {
    if (error.kind === "inactivity_timeout") return "deadline";
    if (error.kind === "premature_eof") return "eof";
    return "error";
  }
  if (error instanceof DeepSeekError && error.status === 504) return "deadline";
  if (error instanceof Error && error.name === "TimeoutError") return "deadline";
  if (error instanceof Error && error.name === "AbortError") return "cancelled";
  return "error";
};

type DeepSeekFailureKind =
  | "upstream_error"
  | "upstream_http_error"
  | "upstream_unreachable"
  | "incomplete_response"
  | "invalid_json"
  | "invalid_completion_schema"
  | "invalid_stream_chunk"
  | "deadline"
  | "cancellation"
  | "api_key_quota_reservation_unavailable"
  | "deepseek_api_key_missing"
  | "deepseek_request_invalid"
  /** A stop reason the gateway cannot place in its terminal vocabulary. */
  | `deepseek_finish_reason:${string}`;

const recordDeepSeekFailureKind = (context: UsageContext | undefined, failureKind: DeepSeekFailureKind): void => {
  if (context?.responseTelemetry) context.responseTelemetry.failureKind = failureKind;
};

/**
 * The telemetry classification for an upstream stop reason the gateway cannot
 * place. The value is bounded and carried so an operator can see exactly what
 * the provider said instead of reading a normal completion.
 */
const deepSeekFinishReasonFailureKind = (finishReason: string | null): DeepSeekFailureKind =>
  `deepseek_finish_reason:${finishReason !== null && /^[A-Za-z0-9_.:-]{1,64}$/.test(finishReason) ? finishReason : "unrecognized"}`;

const deepSeekTransportFailureKind = (error: unknown, terminalType: ResponseStreamTerminalType): DeepSeekFailureKind => {
  if (terminalType === "cancelled") return "cancellation";
  if (terminalType === "deadline") return "deadline";
  if (terminalType === "eof") return "incomplete_response";
  if (error instanceof ApiKeyQuotaDispatchError) return "api_key_quota_reservation_unavailable";
  if (error instanceof DeepSeekStreamError) {
    switch (error.kind) {
      case "malformed_event":
        return "invalid_json";
      case "invalid_chunk":
        return "invalid_stream_chunk";
      case "premature_eof":
        return "incomplete_response";
      case "inactivity_timeout":
        return "deadline";
      default:
        break;
    }
  }
  if (error instanceof DeepSeekError) {
    switch (error.code) {
      case "deepseek_api_key_missing":
        return "deepseek_api_key_missing";
      case "deepseek_request_invalid":
        return "deepseek_request_invalid";
      case "deepseek_upstream_unreachable":
        return "upstream_unreachable";
      case "gateway_timeout":
        return "deadline";
      default:
        break;
    }
  }
  return "upstream_unreachable";
};

const recordCerebrasResponseHealth = (status: number, providerRequestId: string | null): void => {
  if (status === 401 || status === 403) {
    void recordCerebrasProviderHealth("auth_invalid", status, Date.now, providerRequestId);
    return;
  }
  if (status === 429) {
    void recordCerebrasProviderHealth("quota_exhausted", status, Date.now, providerRequestId);
    return;
  }
  if (status >= 500) {
    void recordCerebrasProviderHealth("upstream_error", status, Date.now, providerRequestId);
    return;
  }
  if (status >= 400) {
    void recordCerebrasProviderHealth("reachable", status, Date.now, providerRequestId);
    return;
  }
  void recordCerebrasProviderHealth("success", status, Date.now, providerRequestId);
};

const cerebrasTerminalTypeForError = (error: unknown, downstreamSignal: AbortSignal): ResponseStreamTerminalType => {
  if (downstreamSignal.aborted) return "cancelled";
  if (error instanceof CerebrasError && error.status === 504) return "deadline";
  if (error instanceof Error && error.name === "TimeoutError") return "deadline";
  if (error instanceof Error && error.name === "AbortError") return "cancelled";
  return "error";
};

type CerebrasFailureKind =
  | "upstream_http_error"
  | "upstream_unreachable"
  | "incomplete_response"
  | "invalid_json"
  | "invalid_completion_schema"
  | "deadline"
  | "cancellation"
  | "api_key_quota_reservation_unavailable"
  | "cerebras_api_key_missing"
  | "cerebras_request_invalid";

const recordCerebrasFailureKind = (context: UsageContext | undefined, failureKind: CerebrasFailureKind): void => {
  if (context?.responseTelemetry) context.responseTelemetry.failureKind = failureKind;
};

const cerebrasTransportFailureKind = (error: unknown, terminalType: ResponseStreamTerminalType): CerebrasFailureKind => {
  if (terminalType === "cancelled") return "cancellation";
  if (terminalType === "deadline") return "deadline";
  if (error instanceof ApiKeyQuotaDispatchError) return "api_key_quota_reservation_unavailable";
  if (error instanceof CerebrasError) {
    switch (error.code) {
      case "cerebras_api_key_missing":
        return "cerebras_api_key_missing";
      case "cerebras_request_invalid":
        return "cerebras_request_invalid";
      case "cerebras_upstream_unreachable":
        return "upstream_unreachable";
      case "gateway_timeout":
        return "deadline";
      default:
        break;
    }
  }
  return "upstream_unreachable";
};
/**
 * The GPT-OSS route is deliberately separate from the Codex Responses bridge.
 * It forwards the official Chat Completions body unchanged (apart from the
 * canonical model and explicit non-streaming flag) and never races or falls
 * back to another provider.
 */
const validateCerebrasChatRequestFields = (
  rawRecord: Record<string, unknown>
): { ok: true; value: { reasoning: string; clientWantsStream: boolean; includeUsage: boolean } } | { ok: false; response: Response } => {
  const messages = rawRecord.messages;
  if (!Array.isArray(messages)) return { ok: false, response: openaiError(400, "messages must be an array", "invalid_request_error") };
  if (messages.length === 0) {
    return { ok: false, response: openaiError(400, "messages must be a non-empty array", "invalid_request_error") };
  }
  if (messages.some((message) => !isRecord(message) || Array.isArray(message))) {
    return { ok: false, response: openaiError(400, "messages must contain objects", "invalid_request_error", { param: "messages" }) };
  }

  const reasoningEffort = parseReasoningEffortField(rawRecord.reasoning_effort, "reasoning_effort");
  if (!reasoningEffort.ok) {
    return { ok: false, response: openaiError(400, reasoningEffort.message, "invalid_request_error", { param: "reasoning_effort" }) };
  }
  if (reasoningEffort.value === "none") {
    return {
      ok: false,
      response: openaiError(400, "reasoning_effort 'none' is not supported for gpt-oss-120b. Use low, medium, or high.", "invalid_request_error", {
        param: "reasoning_effort",
      }),
    };
  }
  const parsedStream = parseStreamField(rawRecord.stream);
  if (!parsedStream.ok) {
    return { ok: false, response: openaiError(400, parsedStream.message, "invalid_request_error", { param: "stream" }) };
  }
  const streamOptions = parseChatStreamOptions(rawRecord.stream_options);
  if (!streamOptions.ok) {
    return { ok: false, response: openaiError(400, streamOptions.message, "invalid_request_error", { param: "stream_options" }) };
  }
  return {
    ok: true,
    value: {
      reasoning: reasoningEffort.value ?? DEFAULT_REASONING_EFFORT,
      clientWantsStream: parsedStream.value,
      includeUsage: streamOptions.includeUsage,
    },
  };
};

const respondCerebrasChatInvalidCompletion = async (
  failureKind: "invalid_json" | "invalid_completion_schema",
  upstreamStatus: number,
  providerRequestId: string | null,
  usageContext: UsageContext | undefined
): Promise<Response> => {
  recordStreamTerminalType(usageContext, "error");
  recordCerebrasFailureKind(usageContext, failureKind);
  void recordCerebrasProviderHealth("upstream_error", upstreamStatus, Date.now, providerRequestId);
  await recordErrorUsage(usageContext);
  return openaiError(502, "Upstream returned an invalid Chat Completions response.", "cerebras_upstream_invalid_response", {
    type: "server_error",
    headers: cerebrasResponseHeaders(providerRequestId),
  });
};

const readCerebrasChatCompletion = async (
  bytes: Uint8Array,
  upstreamStatus: number,
  providerRequestId: string | null,
  usageContext: UsageContext | undefined
): Promise<{ ok: true; value: Record<string, unknown> } | { ok: false; response: Response }> => {
  let payload: unknown;
  try {
    payload = JSON.parse(new TextDecoder().decode(bytes)) as unknown;
  } catch {
    return { ok: false, response: await respondCerebrasChatInvalidCompletion("invalid_json", upstreamStatus, providerRequestId, usageContext) };
  }
  const normalized = normalizeCerebrasChatCompletion(payload, CEREBRAS_GPT_OSS_120B_MODEL);
  if (!normalized.ok) {
    return {
      ok: false,
      response: await respondCerebrasChatInvalidCompletion("invalid_completion_schema", upstreamStatus, providerRequestId, usageContext),
    };
  }
  return { ok: true, value: normalized.value };
};

const respondCerebrasChatDispatchFailure = async (error: unknown, downstreamSignal: AbortSignal, usageContext: UsageContext | undefined): Promise<Response> => {
  const terminalType = cerebrasTerminalTypeForError(error, downstreamSignal);
  recordCerebrasFailureKind(usageContext, cerebrasTransportFailureKind(error, terminalType));
  recordStreamTerminalType(usageContext, terminalType);
  if (terminalType !== "cancelled") void recordCerebrasProviderHealth("upstream_error", null);
  await recordErrorUsage(usageContext);
  // Do not log the caught value: an upstream implementation can attach raw
  // response text or request configuration to an Error instance.
  return toCerebrasErrorResponse(error);
};

const respondCerebrasChatUpstreamHttpFailure = async (
  upstream: Response,
  requestSignal: AbortSignal,
  providerRequestId: string | null,
  usageContext: UsageContext | undefined
): Promise<Response> => {
  recordCerebrasResponseHealth(upstream.status, providerRequestId);
  recordCerebrasFailureKind(usageContext, "upstream_http_error");
  recordStreamTerminalType(usageContext, "response.failed");
  await recordErrorUsage(usageContext);
  return await toCerebrasUpstreamErrorResponse(upstream, requestSignal);
};

const respondCerebrasChatIncompleteCapture = async (
  usageContext: UsageContext | undefined,
  downstreamSignal: AbortSignal,
  requestSignal: AbortSignal,
  providerRequestId: string | null
): Promise<Response> => {
  let terminalType: ResponseStreamTerminalType = "error";
  let failureKind: CerebrasFailureKind = "incomplete_response";
  if (downstreamSignal.aborted) {
    terminalType = "cancelled";
    failureKind = "cancellation";
  } else if (requestSignal.aborted) {
    terminalType = "deadline";
    failureKind = "deadline";
  }
  recordCerebrasFailureKind(usageContext, failureKind);
  recordStreamTerminalType(usageContext, terminalType);
  if (terminalType !== "cancelled") {
    void recordCerebrasProviderHealth("upstream_error", null, Date.now, providerRequestId);
  }
  await recordErrorUsage(usageContext);
  if (terminalType === "cancelled") {
    return openaiError(499, "Request was cancelled.", "request_cancelled", { type: "server_error", headers: cerebrasResponseHeaders(providerRequestId) });
  }
  return openaiError(
    terminalType === "deadline" ? 504 : 502,
    terminalType === "deadline" ? "Upstream request exceeded the gateway deadline." : "Upstream returned an incomplete response.",
    terminalType === "deadline" ? "gateway_timeout" : "cerebras_upstream_invalid_response",
    { type: "server_error", headers: cerebrasResponseHeaders(providerRequestId) }
  );
};

const handleCerebrasChatCompletions = async (
  req: Request,
  rawRecord: Record<string, unknown>,
  modelRaw: string,
  usageContext?: UsageContext
): Promise<Response> => {
  const parsedRequest = validateCerebrasChatRequestFields(rawRecord);
  if (!parsedRequest.ok) return parsedRequest.response;
  const { reasoning, clientWantsStream, includeUsage } = parsedRequest.value;

  // Preserve the official nested Chat tools/tool_choice contract. In
  // particular, do not run the Codex-specific flattening that follows this
  // early branch in handleChatCompletionsInternal.
  const cerebrasBody: Record<string, unknown> = {
    ...rawRecord,
    model: CEREBRAS_GPT_OSS_120B_MODEL,
    reasoning_effort: reasoning,
    stream: false,
  };
  delete cerebrasBody.stream_options;
  if (usageContext?.responseTelemetry) {
    usageContext.responseTelemetry.provider = "cerebras";
    usageContext.responseTelemetry.reasoning = reasoning;
  }
  await recordRequestUsage(usageContext, {
    model: modelRaw,
    route: "chat.completions",
    stream: clientWantsStream,
    reasoning,
  });

  const downstreamSignal = downstreamSignalFor(req, usageContext);
  const requestSignal = inferenceSignal(req, usageContext);
  let upstream: Response;
  try {
    upstream = await fetchCerebrasChatCompletions(cerebrasBody, {
      signal: requestSignal,
      beforeDispatch: () => usageContext?.beforeProviderDispatch?.("cerebras") ?? Promise.resolve(undefined),
      onDispatch: () => {
        recordAttemptedProvider(usageContext, "cerebras");
        recordFirstProviderDispatch(usageContext);
      },
      onHeaders: () => {
        recordFirstProviderHeaders(usageContext);
      },
      sentinelUpstreamRecorder: usageContext?.sentinelUpstreamRecorder,
    });
  } catch (error) {
    return await respondCerebrasChatDispatchFailure(error, downstreamSignal, usageContext);
  }

  let providerRequestId = getCerebrasProviderRequestId(upstream);
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.providerRequestId = providerRequestId;

  if (!upstream.ok) {
    return await respondCerebrasChatUpstreamHttpFailure(upstream, requestSignal, providerRequestId, usageContext);
  }

  const captured = await readBoundedResponseBody(upstream, {
    signal: requestSignal,
    maxBytes: 128 * 1024,
    // Successful buffered inference uses the request-level edge deadline, not
    // the one-second error-body default. `requestSignal` still caps the whole
    // request from dispatch through body completion.
    timeoutMs: BUFFERED_INFERENCE_DEADLINE_MS,
    cancellationReason: "Cerebras Chat Completions response was incomplete",
  });
  if (!captured.complete) {
    return await respondCerebrasChatIncompleteCapture(usageContext, downstreamSignal, requestSignal, providerRequestId);
  }

  const completion = await readCerebrasChatCompletion(captured.bytes, upstream.status, providerRequestId, usageContext);
  if (!completion.ok) return completion.response;

  if (chatCompletionHasAnswerBearingOutput(completion.value)) markChatSemanticOutput(usageContext);
  providerRequestId ??= normalizeCerebrasProviderRequestId(completion.value.id);
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.providerRequestId = providerRequestId;
  const usage = extractChatUsageTokens(completion.value.usage);
  await recordCompletionUsage(usageContext, usage);
  if (clientWantsStream) recordFirstSemanticCommitment(usageContext);
  recordStreamTerminalType(usageContext, "response.completed");
  recordCerebrasResponseHealth(upstream.status, providerRequestId);
  const responseHeaders = cerebrasResponseHeaders(providerRequestId, clientWantsStream ? GPT_OSS_STREAM_DOWNGRADED_WARNING : undefined);
  if (clientWantsStream) {
    recordStreamTerminal(usageContext);
    return streamCerebrasChatCompletion(completion.value, includeUsage, responseHeaders);
  }
  return json(200, completion.value, responseHeaders);
};

/**
 * The DeepSeek route is deliberately separate from the Codex Responses bridge.
 * It forwards the official Chat Completions body to the official DeepSeek API,
 * relays native SSE chunks as they arrive instead of buffering a streamed
 * reply, and never races or falls back to another provider.
 */
export const DEEPSEEK_BUFFERED_BODY_MAX_BYTES = 8 * 1024 * 1024;

export const validateDeepSeekChatRequestFields = (
  rawRecord: Record<string, unknown>
): { ok: true; value: { reasoning: string; clientWantsStream: boolean } } | { ok: false; response: Response } => {
  const messages = rawRecord.messages;
  if (!Array.isArray(messages)) return { ok: false, response: openaiError(400, "messages must be an array", "invalid_request_error") };
  if (messages.length === 0) {
    return { ok: false, response: openaiError(400, "messages must be a non-empty array", "invalid_request_error") };
  }
  if (messages.some((message) => !isRecord(message) || Array.isArray(message))) {
    return { ok: false, response: openaiError(400, "messages must contain objects", "invalid_request_error", { param: "messages" }) };
  }
  // Unlike Cerebras, DeepSeek documents every tier the gateway can carry,
  // including `none` (which disables thinking mode), so no tier is rejected
  // locally here.
  const reasoningEffort = parseReasoningEffortField(rawRecord.reasoning_effort, "reasoning_effort");
  if (!reasoningEffort.ok) {
    return { ok: false, response: openaiError(400, reasoningEffort.message, "invalid_request_error", { param: "reasoning_effort" }) };
  }
  const parsedStream = parseStreamField(rawRecord.stream);
  if (!parsedStream.ok) {
    return { ok: false, response: openaiError(400, parsedStream.message, "invalid_request_error", { param: "stream" }) };
  }
  const streamOptions = parseChatStreamOptions(rawRecord.stream_options);
  if (!streamOptions.ok) {
    return { ok: false, response: openaiError(400, streamOptions.message, "invalid_request_error", { param: "stream_options" }) };
  }
  return {
    ok: true,
    value: {
      // DeepSeek's documented default is thinking mode enabled at effort
      // `high`. Sending it explicitly keeps the wire and the gateway's
      // reasoning telemetry in agreement without changing provider behavior.
      //
      // A first-party client may instead send the provider's own
      // `thinking: { type }` switch. That field is authoritative for whether
      // thinking is on, so it resolves the effort here rather than being
      // forwarded beside a contradictory default: `disabled` becomes `none`
      // (the documented equivalent this gateway already sends, see the Delta 5
      // entry in docs/DECISIONS.md) and an explicit `reasoning_effort` still
      // wins when thinking is enabled.
      reasoning:
        reasoningEffort.value === undefined && !deepSeekThinkingModeActive(undefined, rawRecord.thinking)
          ? "none"
          : (reasoningEffort.value ?? DEEPSEEK_DEFAULT_REASONING_EFFORT),
      clientWantsStream: parsedStream.value,
    },
  };
};

/** The output cap the client supplied on the DeepSeek Chat contract, if any. */
export const deepSeekChatClientOutputAllowance = (rawRecord: Record<string, unknown>): number | null => {
  // `max_completion_tokens` is the documented OpenAI field and the one
  // `projectDeepSeekRequest` maps onto the provider's `max_tokens`; a literal
  // `max_tokens` in the record is forwarded unchanged, so both are real caps.
  const completionTokens = parseMaxCompletionTokensField(rawRecord.max_completion_tokens);
  if (!completionTokens.ok) return null;
  if (completionTokens.value !== undefined) return completionTokens.value;
  const maxTokens = parseMaxCompletionTokensField(rawRecord.max_tokens);
  return maxTokens.ok ? (maxTokens.value ?? null) : null;
};

const respondDeepSeekChatInvalidCompletion = async (
  failureKind: "invalid_json" | "invalid_completion_schema",
  upstreamStatus: number,
  providerRequestId: string | null,
  usageContext: UsageContext | undefined
): Promise<Response> => {
  recordStreamTerminalType(usageContext, "error");
  recordDeepSeekFailureKind(usageContext, failureKind);
  void recordDeepSeekProviderHealth("upstream_error", upstreamStatus, Date.now, providerRequestId);
  await recordErrorUsage(usageContext);
  return openaiError(502, "Upstream returned an invalid Chat Completions response.", "deepseek_upstream_invalid_response", {
    type: "server_error",
    headers: deepseekResponseHeaders(providerRequestId),
  });
};

export const readDeepSeekChatCompletion = async (
  bytes: Uint8Array,
  upstreamStatus: number,
  providerRequestId: string | null,
  usageContext: UsageContext | undefined,
  upstreamModel: string
): Promise<{ ok: true; value: Record<string, unknown> } | { ok: false; response: Response }> => {
  let payload: unknown;
  try {
    payload = JSON.parse(new TextDecoder().decode(bytes));
  } catch {
    return { ok: false, response: await respondDeepSeekChatInvalidCompletion("invalid_json", upstreamStatus, providerRequestId, usageContext) };
  }
  const normalized = normalizeDeepSeekChatCompletion(payload, upstreamModel);
  if (!normalized.ok) {
    return { ok: false, response: await respondDeepSeekChatInvalidCompletion("invalid_completion_schema", upstreamStatus, providerRequestId, usageContext) };
  }
  return { ok: true, value: normalized.value };
};

const respondDeepSeekChatDispatchFailure = async (error: unknown, downstreamSignal: AbortSignal, usageContext: UsageContext | undefined): Promise<Response> => {
  const terminalType = deepSeekTerminalTypeForError(error, downstreamSignal);
  recordDeepSeekFailureKind(usageContext, deepSeekTransportFailureKind(error, terminalType));
  recordStreamTerminalType(usageContext, terminalType);
  if (terminalType !== "cancelled") {
    void recordDeepSeekProviderHealth("upstream_error", null, Date.now, null);
  }
  await recordErrorUsage(usageContext);
  return toDeepSeekErrorResponse(error);
};

const respondDeepSeekChatUpstreamHttpFailure = async (
  upstream: Response,
  requestSignal: AbortSignal,
  providerRequestId: string | null,
  usageContext: UsageContext | undefined,
  body: Record<string, unknown>
): Promise<Response> => {
  recordDeepSeekResponseHealth(upstream.status, providerRequestId);
  recordDeepSeekFailureKind(usageContext, "upstream_http_error");
  recordStreamTerminalType(usageContext, "response.failed");
  await recordErrorUsage(usageContext);
  return await toDeepSeekUpstreamErrorResponse(upstream, requestSignal, deepSeekChatBodyDiagnostic(body));
};

export const respondDeepSeekChatIncompleteCapture = async (
  usageContext: UsageContext | undefined,
  downstreamSignal: AbortSignal,
  requestSignal: AbortSignal,
  providerRequestId: string | null
): Promise<Response> => {
  let terminalType: ResponseStreamTerminalType = "error";
  let failureKind: DeepSeekFailureKind = "incomplete_response";
  if (downstreamSignal.aborted) {
    terminalType = "cancelled";
    failureKind = "cancellation";
  } else if (requestSignal.aborted) {
    terminalType = "deadline";
    failureKind = "deadline";
  }
  recordDeepSeekFailureKind(usageContext, failureKind);
  recordStreamTerminalType(usageContext, terminalType);
  if (terminalType !== "cancelled") {
    void recordDeepSeekProviderHealth("upstream_error", null, Date.now, providerRequestId);
  }
  await recordErrorUsage(usageContext);
  if (terminalType === "cancelled") {
    return openaiError(499, "Request was cancelled.", "request_cancelled", { type: "server_error", headers: deepseekResponseHeaders(providerRequestId) });
  }
  return openaiError(
    terminalType === "deadline" ? 504 : 502,
    terminalType === "deadline" ? "Upstream request exceeded the gateway deadline." : "Upstream returned an incomplete response.",
    terminalType === "deadline" ? "gateway_timeout" : "deepseek_upstream_invalid_response",
    { type: "server_error", headers: deepseekResponseHeaders(providerRequestId) }
  );
};

/**
 * The frame union both provider transports are normalized to. DeepSeek's
 * iterator yields these frames directly; the LithosAI normalizer maps the
 * chunks its transport validates onto the same shape.
 */
import { ProviderStreamAdapter, relayChatCompletionStream } from "./provider_stream_relay.ts";
import { handleDeepSeekChatCompletions, handleDeepSeekResponses } from "./deepseek_handlers.ts";

export const deepseekStreamAdapter: ProviderStreamAdapter = {
  responseHeaders: deepseekResponseHeaders,
  frames: iterateDeepSeekChatCompletionStream,
  recordResponseHealth: recordDeepSeekResponseHealth,
  recordProviderError: (status, providerRequestId) => void recordDeepSeekProviderHealth("upstream_error", status, Date.now, providerRequestId),
  recordCancellation: (usageContext) => {
    recordDeepSeekFailureKind(usageContext, "cancellation");
  },
  recordIncompleteResponse: (usageContext) => {
    recordDeepSeekFailureKind(usageContext, "incomplete_response");
  },
  recordFinishFailureKind: (usageContext, finishReason) => {
    recordDeepSeekFailureKind(usageContext, deepSeekFinishReasonFailureKind(finishReason));
  },
  recordTransportFailure: (usageContext, error, terminalType) => {
    recordDeepSeekFailureKind(usageContext, deepSeekTransportFailureKind(error, terminalType));
  },
  terminalTypeForError: deepSeekTerminalTypeForError,
  streamErrorCode: "deepseek_upstream_stream_error",
  responsesProfile: DEEPSEEK_RESPONSES_PROFILE,
};

/**
 * Relays the DeepSeek Chat Completions SSE stream through the shared writer,
 * keeping DeepSeek's documented `: keep-alive` comment frames relayed verbatim.
 */
export const streamDeepSeekChatCompletion = (
  upstream: Response,
  providerRequestId: string | null,
  usageContext: UsageContext | undefined,
  downstreamSignal: AbortSignal,
  requestSignal: AbortSignal,
  upstreamModel: string
): Response => relayChatCompletionStream(deepseekStreamAdapter, upstream, providerRequestId, usageContext, downstreamSignal, requestSignal, upstreamModel);

type DeepSeekDispatchResult =
  | Readonly<{ ok: true; upstream: Response; providerRequestId: string | null; requestSignal: AbortSignal; downstreamSignal: AbortSignal }>
  | Readonly<{ ok: false; response: Response }>;

/**
 * Shared DeepSeek dispatch for both gateway routes. It owns the provider
 * request-id capture, the dispatch/headers telemetry, and the failure
 * responders, so the Chat and Responses adapters differ only in how they
 * translate the payload.
 */
export const dispatchDeepSeekUpstream = async (
  req: Request,
  body: Record<string, unknown>,
  modelRaw: string,
  usageContext: UsageContext | undefined
): Promise<DeepSeekDispatchResult> => {
  const downstreamSignal = downstreamSignalFor(req, usageContext);
  const requestSignal = inferenceSignal(req, usageContext);
  let upstream: Response;
  try {
    upstream = await fetchDeepSeekChatCompletions(body, modelRaw, {
      signal: requestSignal,
      beforeDispatch: () => usageContext?.beforeProviderDispatch?.("deepseek") ?? Promise.resolve(undefined),
      onDispatch: () => {
        recordAttemptedProvider(usageContext, "deepseek");
        recordFirstProviderDispatch(usageContext);
      },
      onHeaders: () => {
        recordFirstProviderHeaders(usageContext);
      },
      sentinelUpstreamRecorder: usageContext?.sentinelUpstreamRecorder,
    });
  } catch (error) {
    return { ok: false, response: await respondDeepSeekChatDispatchFailure(error, downstreamSignal, usageContext) };
  }

  const providerRequestId = getDeepSeekProviderRequestId(upstream);
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.providerRequestId = providerRequestId;
  if (!upstream.ok) {
    return { ok: false, response: await respondDeepSeekChatUpstreamHttpFailure(upstream, requestSignal, providerRequestId, usageContext, body) };
  }
  return { ok: true, upstream, providerRequestId, requestSignal, downstreamSignal };
};

import { handleLithosChatCompletions, handleLithosResponses } from "./lithos_handlers.ts";
const parseChatCompletionsEnvelope = async (
  req: Request
): Promise<
  | {
      ok: true;
      value: {
        body: ChatCompletionRequest;
        rawRecord: Record<string, unknown>;
        warnings: string[];
        jsonObjectTextFormat: { type: "json_object" } | null;
      };
    }
  | { ok: false; response: Response }
> => {
  const body = (await readJsonBody(req)) as ChatCompletionRequest | null;
  if (!body || !isRecord(body)) return { ok: false, response: openaiError(400, "Invalid JSON body", "invalid_request_error") };

  const rawRecord = body as Record<string, unknown>;
  // The DeepSeek route dispatches before the Codex/paid path and speaks the
  // provider's own documented Chat contract, which includes `thinking`. Accept
  // that one field there so a first-party DeepSeek client can reach this route
  // at all; every other route keeps the strict OpenAI allowlist. The field is
  // read for its documented semantics (it selects thinking mode) and is not
  // forwarded verbatim: `projectDeepSeekRequest` owns the wire translation.
  const isDeepSeekRoute = deepSeekUpstreamModelFor(getString(rawRecord.model)?.trim() ?? "") !== null;
  const unknownKey = findUnknownKey(rawRecord, CHAT_COMPLETIONS_ALLOWED_KEYS, isDeepSeekRoute ? DEEPSEEK_CHAT_EXTENSION_KEYS : undefined);
  if (unknownKey) {
    return { ok: false, response: openaiError(400, `Unrecognized request argument supplied: ${unknownKey}`, "invalid_request_error") };
  }
  const promptCacheControls = validatePromptCacheControls(rawRecord);
  if (!promptCacheControls.ok) {
    return { ok: false, response: openaiError(400, promptCacheControls.message, "invalid_request_error", { param: promptCacheControls.param }) };
  }
  const jsonObjectTextFormat =
    isRecord(rawRecord.response_format) && Object.keys(rawRecord.response_format).length === 1 && rawRecord.response_format.type === "json_object"
      ? { type: "json_object" as const }
      : null;
  const handledKeys = new Set([
    "messages",
    "model",
    "stream",
    "reasoning_effort",
    "max_completion_tokens",
    "tools",
    "tool_choice",
    "parallel_tool_calls",
    "prompt_cache_key",
    "prompt_cache_options",
    "prompt_cache_retention",
    "stream_options",
  ]);
  if (jsonObjectTextFormat) handledKeys.add("response_format");
  const warnings = buildIgnoredWarnings(rawRecord, handledKeys);
  return { ok: true, value: { body, rawRecord, warnings, jsonObjectTextFormat } };
};

const resolveChatCompletionsModel = async (
  rawRecord: Record<string, unknown>,
  usageContext: UsageContext | undefined
): Promise<{ ok: true; value: { modelRaw: string; model: string; maxCompletionTokens: number | undefined } } | { ok: false; response: Response }> => {
  const hasModel = Object.prototype.hasOwnProperty.call(rawRecord, "model");
  const rawModelValue = rawRecord.model;
  const modelRawValue = getString(rawModelValue);
  if (hasModel && modelRawValue === null && rawModelValue !== null && rawModelValue !== undefined) {
    return { ok: false, response: openaiError(400, "model must be a string", "invalid_request_error") };
  }
  let modelRaw = (modelRawValue ?? "").trim();
  if (!modelRaw) {
    const defaultModel = await getDefaultModel();
    if (!defaultModel) return { ok: false, response: defaultModelUnavailableError() };
    modelRaw = defaultModel;
  }
  const model = normalizeModelForCodex(modelRaw);
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.model = modelRaw;
  const maxCompletionTokens = parseMaxCompletionTokensField(rawRecord.max_completion_tokens);
  if (!maxCompletionTokens.ok) {
    return { ok: false, response: openaiError(400, maxCompletionTokens.message, "invalid_request_error", { param: "max_completion_tokens" }) };
  }
  return { ok: true, value: { modelRaw, model, maxCompletionTokens: maxCompletionTokens.value } };
};

const validateChatCompletionsOptions = async (
  model: string,
  modelRaw: string,
  rawRecord: Record<string, unknown>,
  body: ChatCompletionRequest
): Promise<
  | {
      ok: true;
      value: {
        modelMetadata: CodexModelMetadata;
        messagesRaw: readonly unknown[];
        reasoningEffort: ReasoningEffort | undefined;
        stream: boolean;
        includeUsage: boolean;
      };
    }
  | { ok: false; response: Response }
> => {
  const modelMetadata = await getCodexModelMetadata(model, "chat.completions");
  const modelAvailabilityError = validateCodexModelAvailable(modelRaw, "chat.completions", modelMetadata);
  if (modelAvailabilityError) return { ok: false, response: modelAvailabilityError };
  const modelCapabilityError = temporaryFreeSurplusCapabilityError(model, rawRecord);
  if (modelCapabilityError) return { ok: false, response: modelCapabilityError };
  const messagesRaw = body.messages;
  if (!Array.isArray(messagesRaw)) return { ok: false, response: openaiError(400, "messages must be an array", "invalid_request_error") };
  if (messagesRaw.length === 0) return { ok: false, response: openaiError(400, "messages must be a non-empty array", "invalid_request_error") };

  const reasoningEffort = parseReasoningEffortField(body.reasoning_effort, "reasoning_effort");
  if (!reasoningEffort.ok) {
    return { ok: false, response: openaiError(400, reasoningEffort.message, "invalid_request_error", { param: "reasoning_effort" }) };
  }

  const parsedStream = parseStreamField(body.stream);
  if (!parsedStream.ok) {
    return { ok: false, response: openaiError(400, parsedStream.message, "invalid_request_error", { param: "stream" }) };
  }
  const streamOptions = parseChatStreamOptions(rawRecord.stream_options);
  if (!streamOptions.ok) {
    return { ok: false, response: openaiError(400, streamOptions.message, "invalid_request_error", { param: "stream_options" }) };
  }
  return {
    ok: true,
    value: {
      modelMetadata,
      messagesRaw,
      reasoningEffort: reasoningEffort.value,
      stream: parsedStream.value,
      includeUsage: streamOptions.includeUsage,
    },
  };
};

const normalizeChatCompletionsInput = (
  messagesRaw: readonly unknown[],
  modelRaw: string,
  modelMetadata: CodexModelMetadata,
  rawRecord: Record<string, unknown>
): { ok: true; value: { input: ResponseInputItem[]; instructions: string | undefined } } | { ok: false; response: Response } => {
  const normalizedMessages: Readonly<{
    instruction: string | null;
    instructionContent: MessageContentItem[] | null;
    input: ResponseInputItem[];
  }>[] = [];
  for (const [index, msg] of messagesRaw.entries()) {
    const converted = normalizeChatMessage(msg, index);
    if (!converted.ok) {
      return { ok: false, response: openaiError(400, converted.message, "invalid_request_error", { param: converted.param }) };
    }
    normalizedMessages.push(converted.value);
  }
  const preserveDeveloperMessages = normalizedMessages.some(
    (message) => message.instructionContent?.some((item) => item.type !== "output_text" && item.prompt_cache_breakpoint?.mode === "explicit") === true
  );
  const input: ResponseInputItem[] = [];
  const instructionParts: string[] = [];
  for (const message of normalizedMessages) {
    if (preserveDeveloperMessages && message.instructionContent !== null) {
      input.push({ type: "message", role: "developer", content: message.instructionContent });
    } else if (message.instruction?.trim()) {
      instructionParts.push(message.instruction.trim());
    }
    input.push(...message.input);
  }

  if (input.length === 0) {
    // Ensure upstream receives a non-empty input for system-only chats.
    input.push({
      type: "message",
      role: "user",
      content: [{ type: "input_text", text: "" }],
    });
  }

  const instructions = preserveDeveloperMessages ? undefined : instructionParts.join("\n\n").trim();
  const promptCacheAvailabilityError = validateKnownUnsupportedPromptCacheUse(modelRaw, modelMetadata, rawRecord, input, "messages");
  if (promptCacheAvailabilityError) return { ok: false, response: promptCacheAvailabilityError };
  return { ok: true, value: { input, instructions } };
};

const buildChatCompletionsCodexBody = async (
  model: string,
  input: ResponseInputItem[],
  options: Readonly<{
    instructions: string | undefined;
    reasoningEffort: ReasoningEffort | undefined;
    modelReasoning: CodexModelReasoning;
    jsonObjectTextFormat: { type: "json_object" } | null;
    maxCompletionTokens: number | undefined;
    rawRecord: Record<string, unknown>;
  }>
): Promise<{ codexBody: Record<string, unknown>; defaultReasoningLabel: ReasoningEffort }> => {
  const defaultEffort = await getDefaultReasoningEffort();
  const modelReasoning = options.modelReasoning;
  const defaultReasoningLabel = resolveDefaultReasoningLabel(modelReasoning, defaultEffort);
  let reasoningValue: Record<string, unknown> | undefined;
  if (options.reasoningEffort === undefined) {
    reasoningValue = { effort: reasoningEffortForCodexRequest(defaultReasoningLabel, modelReasoning) };
  } else {
    reasoningValue = { effort: reasoningEffortForCodexRequest(options.reasoningEffort, modelReasoning) };
  }
  const codexBody = buildCodexRequest(model, input, {
    reasoning: reasoningValue,
    instructions: options.instructions,
  });
  if (options.jsonObjectTextFormat) codexBody.text = { format: options.jsonObjectTextFormat };
  if (options.maxCompletionTokens !== undefined) codexBody.max_output_tokens = options.maxCompletionTokens;
  const passthroughKeys: PassthroughToolSchemaKey[] = [
    "tools",
    "tool_choice",
    "parallel_tool_calls",
    "prompt_cache_key",
    "prompt_cache_options",
    "prompt_cache_retention",
  ];
  applyPassthroughToCodexRequest(codexBody, options.rawRecord, passthroughKeys);
  codexBody.store = false;
  return { codexBody, defaultReasoningLabel };
};

const completedTerminalUsageOf = (prepared: PreparedResponsesStream): UsageTokens | null =>
  prepared.terminal?.type === "response.completed" && isRecord(prepared.terminal.value.response)
    ? extractUsageTokens(prepared.terminal.value.response.usage)
    : null;

const chatCompletionPreflightIsEmpty = (
  prepared: PreparedResponsesStream,
  completedTerminalUsage: UsageTokens | null,
  usageContext: UsageContext | undefined
): boolean => {
  if (prepared.terminal?.type !== "response.completed" || prepared.semantic !== null) return false;
  try {
    return preparedChatCompletionIsEmpty(prepared);
  } catch (error) {
    recordTerminalUsage(usageContext, completedTerminalUsage, false);
    throw error;
  }
};

const rejectEmptyChatCompletion = async (
  prepared: PreparedResponsesStream,
  completedTerminalUsage: UsageTokens | null,
  usageContext: UsageContext | undefined,
  lifecycle: MeteredTransportLifecycle,
  resolveCodexProbe: (terminalType: ResponseStreamTerminalType) => void,
  provider: UpstreamProvider,
  warnings: string[]
): Promise<Response> => {
  for (const event of prepared.buffered) recordResponsesEventTelemetry(usageContext, event);
  recordEmptyUpstreamCompletion(usageContext, lifecycle, completedTerminalUsage, resolveCodexProbe);
  await prepared.iterator.return("Empty Chat completion rejected").catch(() => {});
  return streamErrorResponse(502, EMPTY_UPSTREAM_COMPLETION_MESSAGE, "empty_upstream_completion", provider, warnings, "server_error", null);
};

const respondChatCompletionsDispatchFailure = async (
  error: unknown,
  requestInferenceSignal: AbortSignal,
  downstreamSignal: AbortSignal,
  usageContext: UsageContext | undefined
): Promise<Response> => {
  const terminalType = classifyPreHeaderFailure(error, requestInferenceSignal, downstreamSignal);
  recordStreamTerminalType(usageContext, terminalType);
  if (terminalType !== "cancelled") {
    logRedactedUpstreamError("[ai.ubq.fi] Upstream fetch failed:", error);
  }
  await recordErrorUsage(usageContext);
  return toPreHeaderErrorResponse(error, terminalType, usageContext?.responseTelemetry?.provider);
};

const respondChatCompletionsUpstreamHttpFailure = async (
  upstream: Response,
  options: Readonly<{
    provider: UpstreamProvider;
    requestInferenceSignal: AbortSignal;
    usageContext: UsageContext | undefined;
    lifecycle: MeteredTransportLifecycle;
    clearStreamFirstEventDeadline: () => void;
  }>
): Promise<Response> => {
  options.lifecycle.terminal("response.failed");
  recordStreamTerminalType(options.usageContext, "response.failed");
  await recordErrorUsage(options.usageContext);
  try {
    const normalized = await toOpenAiUpstreamErrorResponse(upstream, options.provider, options.requestInferenceSignal);
    return attachResponseTelemetry(normalized, options.usageContext?.responseTelemetry ?? createResponseTelemetryState());
  } finally {
    options.clearStreamFirstEventDeadline();
  }
};

const respondChatCompletionsPreflightFailure = async (
  error: unknown,
  options: Readonly<{
    requestInferenceSignal: AbortSignal;
    downstreamSignal: AbortSignal;
    usageContext: UsageContext | undefined;
    provider: UpstreamProvider;
    lifecycle: MeteredTransportLifecycle;
    warnings: string[];
    resolveCodexProbe: (terminalType: ResponseStreamTerminalType) => void;
    clearStreamFirstEventDeadline: () => void;
  }>
): Promise<Response> => {
  options.clearStreamFirstEventDeadline();
  const terminalType = classifyStreamFailure(error, options.requestInferenceSignal, options.downstreamSignal);
  options.resolveCodexProbe(terminalType);
  recordStreamTerminalType(options.usageContext, terminalType);
  if (terminalType !== "cancelled") recordResponsesFailureTelemetry(options.usageContext, error);
  if (terminalType === "cancelled") options.lifecycle.cancelled();
  else options.lifecycle.ambiguous();
  await recordErrorUsage(options.usageContext);
  return streamPreflightFailureResponse(terminalType, options.provider, options.warnings);
};

const dispatchAndPreflightChatCompletions = async (
  codexBody: Record<string, unknown>,
  options: Readonly<{
    req: Request;
    model: string;
    clientVersion: string | null | undefined;
    stream: boolean;
    reasoningLabel: ReasoningEffort;
    warnings: string[];
    usageContext: UsageContext | undefined;
    downstreamSignal: AbortSignal;
  }>
): Promise<
  | {
      ok: true;
      value: {
        preflight: PreflightedResponsesStream;
        provider: UpstreamProvider;
        lifecycle: MeteredTransportLifecycle;
        signal: AbortSignal;
        resolveCodexProbe: (terminalType: ResponseStreamTerminalType) => void;
        combinedWarnings: string[];
      };
    }
  | { ok: false; response: Response }
> => {
  const downstreamSignal = options.downstreamSignal;
  const usageContext = options.usageContext;
  // One timer covers both provider dispatch/headers and the first SSE event.
  // It is cleared immediately after preflight so active streams get their own
  // renewable inactivity deadline rather than an absolute buffered cutoff.
  const streamFirstEventDeadline = options.stream ? createStreamFirstEventDeadline(downstreamSignal) : null;
  const requestInferenceSignal = streamFirstEventDeadline?.signal ?? inferenceSignal(options.req, usageContext);
  const clearStreamFirstEventDeadline = (): void => streamFirstEventDeadline?.clear();

  let routed: RoutedResponsesUpstream;
  try {
    routed = await fetchResponsesWithPaidFallback(codexBody, {
      model: options.model,
      route: "chat.completions",
      stream: options.stream,
      reasoning: options.reasoningLabel,
      usageContext,
      clientVersion: options.clientVersion,
      signal: requestInferenceSignal,
    });
  } catch (error) {
    clearStreamFirstEventDeadline();
    return { ok: false, response: await respondChatCompletionsDispatchFailure(error, requestInferenceSignal, downstreamSignal, usageContext) };
  }
  const upstream = routed.response;
  const provider = routed.provider;
  const providerWarnings = responseWarnings(upstream);
  const combinedWarnings = [...options.warnings, ...providerWarnings];
  const lifecycle = createMeteredTransportLifecycle(
    routed.paidFallback,
    provider,
    routed.paidFallbackProviderRequestId ?? null,
    routed.paidFallbackBilling ?? null,
    options.model,
    routed.providerHealthOnly === true
  );
  let codexTerminalResolved = false;
  const resolveCodexProbe = (terminalType: ResponseStreamTerminalType): void => {
    if (routed.provider !== "chatgpt_codex" || codexTerminalResolved) return;
    codexTerminalResolved = true;
    let transition: Promise<void>;
    if (terminalType === "response.completed") {
      transition = markCodexResponseCompleted(upstream);
    } else if (terminalType === "response.failed" || terminalType === "error" || terminalType === "eof" || terminalType === "deadline") {
      transition = markCodexResponseUpstreamError(upstream);
    } else {
      transition = releaseCodexResponseProbe(upstream);
    }
    void transition.catch(() => {});
  };

  if (routed.gatewayResponse) {
    clearStreamFirstEventDeadline();
    recordStreamTerminalType(usageContext, upstream.status === 504 ? "deadline" : "error");
    await recordErrorUsage(usageContext);
    return { ok: false, response: upstream };
  }
  if (!upstream.ok) {
    return {
      ok: false,
      response: await respondChatCompletionsUpstreamHttpFailure(upstream, {
        provider,
        requestInferenceSignal,
        usageContext,
        lifecycle,
        clearStreamFirstEventDeadline,
      }),
    };
  }

  if (!upstream.body) {
    clearStreamFirstEventDeadline();
    resolveCodexProbe("error");
    lifecycle.ambiguous();
    recordStreamTerminalType(usageContext, "error");
    await recordErrorUsage(usageContext);
    return {
      ok: false,
      response: streamErrorResponse(502, "Codex upstream response missing body.", "codex_upstream_missing_body", provider, combinedWarnings),
    };
  }

  let preflight: PreflightedResponsesStream;
  try {
    const firstEvent = await preflightResponsesStream(upstream.body, requestInferenceSignal, {});
    recordFirstUpstreamSseEvent(usageContext);
    const replay = (async function* (): ResponsesStreamIterator {
      try {
        yield firstEvent.first;
        for await (const event of firstEvent.iterator) yield event;
        return undefined;
      } finally {
        await firstEvent.iterator.return("Chat semantic preflight closed").catch(() => {});
      }
    })();
    const prepared = await prepareResponsesStreamForCommit(replay, {
      releaseOnProgress: options.stream && supportsReasoningProgressRelease(provider),
    });
    clearStreamFirstEventDeadline();
    const completedTerminalUsage = completedTerminalUsageOf(prepared);
    if (chatCompletionPreflightIsEmpty(prepared, completedTerminalUsage, usageContext)) {
      return {
        ok: false,
        response: await rejectEmptyChatCompletion(prepared, completedTerminalUsage, usageContext, lifecycle, resolveCodexProbe, provider, combinedWarnings),
      };
    }
    preflight = chatSourceFromPrepared(firstEvent, prepared);
  } catch (error) {
    return {
      ok: false,
      response: await respondChatCompletionsPreflightFailure(error, {
        requestInferenceSignal,
        downstreamSignal,
        usageContext,
        provider,
        lifecycle,
        warnings: combinedWarnings,
        resolveCodexProbe,
        clearStreamFirstEventDeadline,
      }),
    };
  }
  return { ok: true, value: { preflight, provider, lifecycle, signal: requestInferenceSignal, resolveCodexProbe, combinedWarnings } };
};

export const handleChatCompletionsInternal = async (req: Request, usageContext?: UsageContext): Promise<Response> => {
  const envelope = await parseChatCompletionsEnvelope(req);
  if (!envelope.ok) return envelope.response;
  const { body, rawRecord, warnings, jsonObjectTextFormat } = envelope.value;

  const modelChoice = await resolveChatCompletionsModel(rawRecord, usageContext);
  if (!modelChoice.ok) return modelChoice.response;
  const { modelRaw, model, maxCompletionTokens } = modelChoice.value;

  // A switched-off direct provider is not dispatched to; those ids then follow
  // the ordinary Codex/paid waterfall like any other catalog model.
  const selection = await loadProviderSelectionCached();
  if (isProviderEnabled("cerebras", selection) && model.toLowerCase() === CEREBRAS_GPT_OSS_120B_MODEL) {
    return await handleCerebrasChatCompletions(req, rawRecord, modelRaw, usageContext);
  }
  if (isProviderEnabled("deepseek", selection) && deepSeekUpstreamModelFor(model)) {
    return await handleDeepSeekChatCompletions(req, rawRecord, modelRaw, usageContext);
  }
  if (isProviderEnabled("lithos", selection) && lithosUpstreamModelFor(model)) {
    return await handleLithosChatCompletions(req, rawRecord, modelRaw, usageContext);
  }

  const options = await validateChatCompletionsOptions(model, modelRaw, rawRecord, body);
  if (!options.ok) return options.response;
  const { modelMetadata, messagesRaw, reasoningEffort, stream, includeUsage } = options.value;

  const preparedInput = normalizeChatCompletionsInput(messagesRaw, modelRaw, modelMetadata, rawRecord);
  if (!preparedInput.ok) return preparedInput.response;
  const { input, instructions } = preparedInput.value;

  const built = await buildChatCompletionsCodexBody(model, input, {
    instructions,
    reasoningEffort,
    modelReasoning: modelMetadata.reasoning,
    jsonObjectTextFormat,
    maxCompletionTokens,
    rawRecord,
  });
  const codexBody = built.codexBody;

  const reasoningLabel = resolveReasoningLabelFromEffort(reasoningEffort, built.defaultReasoningLabel);
  if (usageContext?.responseTelemetry) {
    usageContext.responseTelemetry.reasoning = reasoningLabel;
    usageContext.responseTelemetry.outputTokenAllowance = maxCompletionTokens ?? null;
    usageContext.responseTelemetry.semanticOutputObserved = false;
  }
  await recordRequestUsage(usageContext, {
    model: modelRaw,
    route: "chat.completions",
    stream,
    reasoning: reasoningLabel,
    promptCacheKeyPresent: promptCacheKeyPresent(rawRecord),
    promptCacheMode: promptCacheModeFor(rawRecord),
    explicitBreakpointCount: countExplicitPromptCacheBreakpoints(input),
  });
  const downstreamSignal = downstreamSignalFor(req, usageContext);
  const dispatched = await dispatchAndPreflightChatCompletions(codexBody, {
    req,
    model,
    clientVersion: modelMetadata.snapshot?.client_version,
    stream,
    reasoningLabel,
    warnings,
    usageContext,
    downstreamSignal,
  });
  if (!dispatched.ok) return dispatched.response;
  const { preflight, provider, lifecycle, signal, resolveCodexProbe, combinedWarnings } = dispatched.value;

  recordFirstSemanticCommitment(usageContext);
  const response = stream
    ? streamChatCompletions(preflight, model, includeUsage, usageContext, provider, lifecycle, signal, downstreamSignal, resolveCodexProbe)
    : await completeChatCompletions(preflight, model, usageContext, provider, lifecycle, signal, downstreamSignal, combinedWarnings, resolveCodexProbe);
  return withUosWarning(response, combinedWarnings);
};

export const handleChatCompletions = async (req: Request, usageContext?: UsageContext): Promise<Response> =>
  await runWithResponseTelemetry(usageContext, (context) => handleChatCompletionsInternal(req, context));

type ResponsesStep<T> = { ok: true; value: T } | { ok: false; response: Response };

type ResponsesRequestState = {
  usageContext: UsageContext | undefined;
  rawRecord: Record<string, unknown>;
  warnings: string[];
  clientWantsStream: boolean;
  modelRaw: string;
  model: string;
  modelMetadata: CodexModelMetadata;
  input: ResponseInputItem[];
  reasoningLabel: ReasoningEffort;
  codexBody: Record<string, unknown>;
  removedProviderBody: Record<string, unknown>;
};

type ResponsesCircuitRoute = Readonly<{
  route: "codex" | "removed_provider";
  probe: RemovedProviderCircuitProbe | null;
}>;

type ResponsesFailureCorrelation = Readonly<{
  provider: string | null;
  accountSlot: number | null;
  accountCohortId: string | null;
  activeGeneration: number | null;
  activeTransitionReason: ActiveTransitionReason;
  providerRequestId: string | null;
}>;

type ResponsesRoutingState = {
  downstreamSignal: AbortSignal;
  requestInferenceSignal: AbortSignal;
  preHeaderDeadline: StreamDeadline;
  apiKey: string | null;
  paidFallbackAvailable: boolean;
  sessionId: string | null;
  route: "codex" | "removed_provider";
  probe: RemovedProviderCircuitProbe | null;
  primaryFailureResponse: Response | null;
  primaryFailureCorrelation: ResponsesFailureCorrelation | null;
  primaryResult: ResponsesRouteAttempt | null;
  removedProviderAttempt: PreparedResponsesAttempt | null;
  selectedModel: string | null;
  fallbackStartedAt: number;
};

type ResponsesHandlerState = ResponsesRequestState & ResponsesRoutingState;

type ResponsesInputAccumulator = {
  converted: ResponseInputItem[];
  contentBuffer: MessageContentItem[];
  sawNonContentItem: boolean;
};

type ResponsesDeliveryState = {
  usageContext: UsageContext | undefined;
  ready: PreparedResponsesAttempt;
  lifecycle: MeteredTransportLifecycle;
  routed: RoutedResponsesUpstream | null;
  clientWantsStream: boolean;
  downstreamSignal: AbortSignal;
  clientWarnings: string[];
  warningModel: string | null;
  selectedModel: string | null;
  probe: RemovedProviderCircuitProbe | null;
  removedProviderAttempt: PreparedResponsesAttempt | null;
  fallbackStartedAt: number;
  providerTerminalValidated: boolean;
  clearProbeRenewal: () => void;
};

const readResponsesRequest = async (
  req: Request,
  parsedBody?: unknown
): Promise<ResponsesStep<{ rawBody: ResponsesRequest; rawRecord: Record<string, unknown> }>> => {
  const rawBody = (parsedBody === undefined ? await readJsonBody(req) : parsedBody) as ResponsesRequest | null;
  if (!rawBody || !isRecord(rawBody)) return { ok: false, response: openaiError(400, "Invalid JSON body", "invalid_request_error") };
  return { ok: true, value: { rawBody, rawRecord: rawBody as Record<string, unknown> } };
};

const validateResponsesRequestFields = (rawRecord: Record<string, unknown>, rawBody: ResponsesRequest): Response | null => {
  const unknownKey = findUnknownKey(rawRecord, RESPONSES_ALLOWED_KEYS, CODEX_RESPONSES_EXTENSION_KEYS);
  if (unknownKey) {
    return openaiError(400, `Unrecognized request argument supplied: ${unknownKey}`, "invalid_request_error");
  }
  const maxOutputTokens = rawRecord.max_output_tokens;
  if (
    maxOutputTokens !== undefined &&
    maxOutputTokens !== null &&
    (typeof maxOutputTokens !== "number" || !Number.isSafeInteger(maxOutputTokens) || maxOutputTokens <= 0)
  ) {
    return openaiError(400, "max_output_tokens must be a positive integer", "invalid_request_error", {
      param: "max_output_tokens",
    });
  }
  const parallelToolCalls = rawRecord.parallel_tool_calls;
  if (parallelToolCalls !== undefined && typeof parallelToolCalls !== "boolean") {
    return openaiError(400, "parallel_tool_calls must be a boolean", "invalid_request_error", {
      param: "parallel_tool_calls",
    });
  }
  const maxToolCalls = rawRecord.max_tool_calls;
  if (maxToolCalls !== undefined && maxToolCalls !== null && (typeof maxToolCalls !== "number" || !Number.isSafeInteger(maxToolCalls) || maxToolCalls <= 0)) {
    return openaiError(400, "max_tool_calls must be a positive integer", "invalid_request_error", {
      param: "max_tool_calls",
    });
  }
  const promptCacheControls = validatePromptCacheControls(rawRecord);
  if (!promptCacheControls.ok) {
    return openaiError(400, promptCacheControls.message, "invalid_request_error", { param: promptCacheControls.param });
  }
  if (Object.prototype.hasOwnProperty.call(rawRecord, "client_metadata")) {
    const clientMetadata = rawBody.client_metadata;
    if (!isRecord(clientMetadata) || Array.isArray(clientMetadata) || Object.values(clientMetadata).some((value) => typeof value !== "string")) {
      return openaiError(400, "client_metadata must be an object with string values", "invalid_request_error", { param: "client_metadata" });
    }
  }
  return null;
};

const resolveResponsesStreamSettings = (
  rawRecord: Record<string, unknown>,
  rawBody: ResponsesRequest
): ResponsesStep<{ warnings: string[]; clientWantsStream: boolean }> => {
  const warnings = buildIgnoredWarnings(
    rawRecord,
    new Set([
      "model",
      "input",
      "stream",
      "reasoning",
      "instructions",
      "tools",
      "tool_choice",
      "parallel_tool_calls",
      "max_output_tokens",
      "prompt_cache_key",
      "prompt_cache_options",
      "prompt_cache_retention",
      "text",
      "include",
      "context_management",
      "client_metadata",
    ])
  );

  const parsedStream = parseStreamField(rawBody.stream);
  if (!parsedStream.ok) {
    return { ok: false, response: openaiError(400, parsedStream.message, "invalid_request_error", { param: "stream" }) };
  }
  return { ok: true, value: { warnings, clientWantsStream: parsedStream.value } };
};

const resolveResponsesModel = async (
  rawRecord: Record<string, unknown>,
  usageContext: UsageContext | undefined
): Promise<ResponsesStep<{ modelRaw: string; model: string; modelMetadata: CodexModelMetadata }>> => {
  const hasModel = Object.prototype.hasOwnProperty.call(rawRecord, "model");
  const rawModelValue = rawRecord.model;
  const modelRawValue = getString(rawModelValue);
  if (hasModel && modelRawValue === null && rawModelValue !== null && rawModelValue !== undefined) {
    return { ok: false, response: openaiError(400, "model must be a string", "invalid_request_error") };
  }
  let modelRaw = (modelRawValue ?? "").trim();
  if (!modelRaw) {
    const defaultModel = await getDefaultModel();
    if (!defaultModel) return { ok: false, response: defaultModelUnavailableError() };
    modelRaw = defaultModel;
  }
  const model = normalizeModelForCodex(modelRaw);
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.model = modelRaw;
  // A switched-off Cerebras provider no longer owns this id, so the ordinary
  // availability check below decides whether anything else can serve it.
  if (model.toLowerCase() === CEREBRAS_GPT_OSS_120B_MODEL && isProviderEnabled("cerebras", await loadProviderSelectionCached())) {
    return {
      ok: false,
      response: openaiError(400, "gpt-oss-120b is available only on /v1/chat/completions.", "unsupported_model", { param: "model" }),
    };
  }
  // The DeepSeek official route is deliberately scoped to /v1/chat/completions.
  // Unlike the Cerebras model, the interchangeable DeepSeek ids are already
  // catalog models that /v1/responses serves through the provider waterfall,
  // and this gateway has no Responses adapter for the official API. Leave that
  // route untouched so existing Responses clients keep working; only the Chat
  // Completions route is redirected to the official provider.
  const modelMetadata = await getCodexModelMetadata(model, "responses");
  const modelAvailabilityError = validateCodexModelAvailable(modelRaw, "responses", modelMetadata);
  if (modelAvailabilityError) return { ok: false, response: modelAvailabilityError };
  const modelCapabilityError = temporaryFreeSurplusCapabilityError(model, rawRecord);
  if (modelCapabilityError) return { ok: false, response: modelCapabilityError };
  return { ok: true, value: { modelRaw, model, modelMetadata } };
};

const flushResponsesContentBuffer = (accumulator: ResponsesInputAccumulator): void => {
  if (!accumulator.contentBuffer.length) return;
  accumulator.converted.push({ type: "message", role: "user", content: accumulator.contentBuffer });
  accumulator.contentBuffer = [];
};

const appendResponsesMessageEntry = (msg: unknown, param: string, accumulator: ResponsesInputAccumulator): Response | null => {
  const mapped = normalizeResponseMessageItem(msg, param);
  if (!mapped.ok) {
    return openaiError(400, mapped.message, "invalid_request_error", { param: mapped.param });
  }
  flushResponsesContentBuffer(accumulator);
  accumulator.converted.push(mapped.value);
  accumulator.sawNonContentItem = true;
  return null;
};

const appendResponsesTextEntry = (msg: string, accumulator: ResponsesInputAccumulator): void => {
  const contentItem: MessageContentItem = { type: "input_text", text: msg };
  if (accumulator.sawNonContentItem) {
    accumulator.converted.push({ type: "message", role: "user", content: [contentItem] });
  } else {
    accumulator.contentBuffer.push(contentItem);
  }
};

const appendResponsesContentEntry = (msg: unknown, param: string, accumulator: ResponsesInputAccumulator): Response | null => {
  const contentItem = normalizeResponseContentItem(msg, param, "user");
  if (!contentItem.ok) {
    return openaiError(400, contentItem.message, "invalid_request_error", { param: contentItem.param });
  }
  if (accumulator.sawNonContentItem) {
    accumulator.converted.push({ type: "message", role: "user", content: [contentItem.value] });
  } else {
    accumulator.contentBuffer.push(contentItem.value);
  }
  return null;
};

const appendResponsesPassthroughEntry = (
  msg: Record<string, unknown>,
  messageType: string,
  param: string,
  accumulator: ResponsesInputAccumulator
): Response | null => {
  // Content items belong inside a message (or are normalized above).
  // Do not mistake an unsupported input_* content type for an arbitrary
  // Responses item and silently relay it upstream.
  if (messageType.startsWith("input_") || messageType === "text" || messageType === "image_url" || messageType === "output_text") {
    return openaiError(400, `${param}.type is not supported`, "invalid_request_error", {
      param: `${param}.type`,
    });
  }
  flushResponsesContentBuffer(accumulator);
  const normalizedFunctionOutput = normalizeFunctionCallOutputItem(msg, param);
  if (!normalizedFunctionOutput.ok) {
    return openaiError(400, normalizedFunctionOutput.message, "invalid_request_error", {
      param: normalizedFunctionOutput.param,
    });
  }
  accumulator.converted.push(normalizedFunctionOutput.value);
  accumulator.sawNonContentItem = true;
  return null;
};

const appendResponsesInputEntry = (msg: unknown, param: string, accumulator: ResponsesInputAccumulator): Response | null => {
  const messageType = isRecord(msg) && !Array.isArray(msg) ? getString(msg.type) : null;
  if (messageType === "message" || (messageType === null && isRecord(msg) && "role" in msg)) {
    // A turn replayed from a failover response carries this gateway's own
    // notice. Sending it upstream would feed the provider text the model never
    // wrote and the user never typed.
    if (isGatewayFailoverWarningItem(msg)) return null;
    return appendResponsesMessageEntry(msg, param, accumulator);
  }

  if (typeof msg === "string") {
    appendResponsesTextEntry(msg, accumulator);
    return null;
  }

  if (messageType === "input_text" || messageType === "input_image" || messageType === "input_file") {
    return appendResponsesContentEntry(msg, param, accumulator);
  }

  if (isRecord(msg) && !Array.isArray(msg) && Object.prototype.hasOwnProperty.call(msg, "prompt_cache_breakpoint")) {
    return openaiError(400, "prompt_cache_breakpoint is only valid on supported input content blocks", "invalid_request_error", {
      param: `${param}.prompt_cache_breakpoint`,
    });
  }

  // Codex CLI uses the Responses API and can send additional input item types
  // (e.g. reasoning + function_call + function_call_output). Pass them through
  // so tool-calling conversations work end-to-end.
  if (isRecord(msg) && typeof msg.type === "string" && msg.type !== "message") {
    return appendResponsesPassthroughEntry(msg, msg.type, param, accumulator);
  }

  return openaiError(400, "Invalid message in input[]", "invalid_request_error", { param });
};

const normalizeResponsesInput = (inputRaw: unknown): ResponsesStep<ResponseInputItem[]> => {
  if (inputRaw === undefined) {
    return { ok: true, value: [] };
  }
  if (typeof inputRaw === "string") {
    return { ok: true, value: [{ type: "message", role: "user", content: [{ type: "input_text", text: inputRaw }] }] };
  }
  if (!Array.isArray(inputRaw)) {
    return { ok: false, response: openaiError(400, "input must be a string or an array", "invalid_request_error") };
  }

  const accumulator: ResponsesInputAccumulator = { converted: [], contentBuffer: [], sawNonContentItem: false };
  for (const [index, msg] of inputRaw.entries()) {
    const entryError = appendResponsesInputEntry(msg, `input[${index}]`, accumulator);
    if (entryError) return { ok: false, response: entryError };
  }
  if (!accumulator.sawNonContentItem || accumulator.contentBuffer.length) {
    flushResponsesContentBuffer(accumulator);
  }
  return { ok: true, value: accumulator.converted };
};

const resolveResponsesReasoning = async (
  rawRecord: Record<string, unknown>,
  rawBody: ResponsesRequest,
  modelMetadata: CodexModelMetadata,
  usageContext: UsageContext | undefined
): Promise<ResponsesStep<{ reasoningLabel: ReasoningEffort; instructions: string | undefined; reasoning: Record<string, unknown> | undefined }>> => {
  const reasoning = parseReasoningParam(rawBody.reasoning);
  if (!reasoning.ok) return { ok: false, response: openaiError(400, reasoning.message, "invalid_request_error", { param: "reasoning" }) };

  let instructions: string | undefined;
  if (Object.prototype.hasOwnProperty.call(rawRecord, "instructions")) {
    if (rawBody.instructions === null) {
      instructions = undefined;
    } else if (typeof rawBody.instructions === "string") {
      instructions = rawBody.instructions;
    } else {
      return { ok: false, response: openaiError(400, "instructions must be a string", "invalid_request_error") };
    }
  }
  const defaultEffort = await getDefaultReasoningEffort();
  const modelReasoning = modelMetadata.reasoning;
  const defaultReasoningLabel = resolveDefaultReasoningLabel(modelReasoning, defaultEffort);
  const reasoningLabel = resolveReasoningLabelFromParam(reasoning.value, defaultReasoningLabel);
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.reasoning = reasoningLabel;

  let reasoningValue = normalizeReasoningParamForCodex(reasoning.value, modelReasoning);
  if (reasoningValue === undefined && reasoning.value === undefined) {
    reasoningValue = { effort: reasoningEffortForCodexRequest(defaultReasoningLabel, modelReasoning) };
  }
  return { ok: true, value: { reasoningLabel, instructions, reasoning: reasoningValue } };
};

const buildResponsesUpstreamBodies = (
  model: string,
  input: ResponseInputItem[],
  instructions: string | undefined,
  reasoning: Record<string, unknown> | undefined,
  rawRecord: Record<string, unknown>
): { codexBody: Record<string, unknown>; removedProviderBody: Record<string, unknown> } => {
  const codexBody = buildCodexRequest(model, input, { reasoning, instructions });
  if (Object.prototype.hasOwnProperty.call(rawRecord, "max_output_tokens")) {
    codexBody.max_output_tokens = rawRecord.max_output_tokens;
  }
  const passthroughKeys: PassthroughToolSchemaKey[] = [
    "tools",
    "tool_choice",
    "parallel_tool_calls",
    "prompt_cache_key",
    "prompt_cache_options",
    "prompt_cache_retention",
    "text",
    "include",
    "context_management",
  ];
  applyPassthroughToCodexRequest(codexBody, rawRecord, passthroughKeys);
  codexBody.model = model;
  codexBody.input = input;
  codexBody.stream = true;
  codexBody.store = false;
  const removedProviderBody = { ...codexBody };
  // Preserve official controls supported by RemovedProvider even when Codex does
  // not currently accept them on its compatibility transport.
  for (const key of ["max_output_tokens", "max_tool_calls", "metadata", "safety_identifier", "service_tier", "temperature", "top_p", "truncation", "user"]) {
    if (Object.prototype.hasOwnProperty.call(rawRecord, key)) removedProviderBody[key] = rawRecord[key];
  }
  return { codexBody, removedProviderBody };
};

export const recordRemovedProviderCircuitTransition = (usageContext: UsageContext | undefined, transition: string): void => {
  if (transition !== "none") recordRemovedProviderFields(usageContext, { circuitTransition: transition });
};

const prepareResponsesRouting = async (req: Request, state: ResponsesRequestState, model: string): Promise<ResponsesRoutingState> => {
  const downstreamSignal = downstreamSignalFor(req, state.usageContext);
  const requestInferenceSignal = state.clientWantsStream ? downstreamSignal : inferenceSignal(req, state.usageContext);
  const preHeaderDeadline = createStreamFirstEventDeadline(requestInferenceSignal);
  const apiKey = isTemporaryFreeSurplusModel(model) ? null : readRemovedProviderApiKey();
  const paidFallbackAvailable = canAttemptPaidFallback(state.usageContext, await loadProviderSelectionCached());
  const debugRoutingScenario = (await loadDebugRoutingConfig()).scenario;
  const circuit = apiKey ? await selectRemovedProviderCircuitRoute() : null;
  if (circuit) {
    recordRemovedProviderCircuitTransition(state.usageContext, circuit.transition);
  }
  const sessionId = apiKey ? await deriveRemovedProviderSessionId(state.usageContext?.idempotencyPrincipal, state.rawRecord.client_metadata) : null;
  const route: "codex" | "removed_provider" = debugRoutingScenario === "removed_provider_first" && apiKey ? "removed_provider" : (circuit?.route ?? "codex");
  return {
    downstreamSignal,
    requestInferenceSignal,
    preHeaderDeadline,
    apiKey,
    paidFallbackAvailable,
    sessionId,
    route,
    probe: circuit ? circuit.probe : null,
    primaryFailureResponse: null,
    primaryFailureCorrelation: null,
    primaryResult: null,
    removedProviderAttempt: null,
    selectedModel: null,
    fallbackStartedAt: 0,
  };
};

const prepareResponsesRequest = async (
  req: Request,
  rawRecord: Record<string, unknown>,
  rawBody: ResponsesRequest,
  usageContext: UsageContext | undefined
): Promise<ResponsesStep<ResponsesHandlerState>> => {
  const settings = resolveResponsesStreamSettings(rawRecord, rawBody);
  if (!settings.ok) return settings;
  const modelResolution = await resolveResponsesModel(rawRecord, usageContext);
  if (!modelResolution.ok) return modelResolution;
  const input = normalizeResponsesInput(rawBody.input);
  if (!input.ok) return input;
  const promptCacheAvailabilityError = validateKnownUnsupportedPromptCacheUse(
    modelResolution.value.modelRaw,
    modelResolution.value.modelMetadata,
    rawRecord,
    input.value,
    "input"
  );
  if (promptCacheAvailabilityError) return { ok: false, response: promptCacheAvailabilityError };
  const reasoning = await resolveResponsesReasoning(rawRecord, rawBody, modelResolution.value.modelMetadata, usageContext);
  if (!reasoning.ok) return reasoning;
  const upstreamBodies = buildResponsesUpstreamBodies(
    modelResolution.value.model,
    input.value,
    reasoning.value.instructions,
    reasoning.value.reasoning,
    rawRecord
  );
  await recordRequestUsage(usageContext, {
    model: modelResolution.value.modelRaw,
    route: "responses",
    stream: settings.value.clientWantsStream,
    reasoning: reasoning.value.reasoningLabel,
    promptCacheKeyPresent: promptCacheKeyPresent(rawRecord),
    promptCacheMode: promptCacheModeFor(rawRecord),
    explicitBreakpointCount: countExplicitPromptCacheBreakpoints(input.value),
  });
  const requestState: ResponsesRequestState = {
    usageContext,
    rawRecord,
    warnings: settings.value.warnings,
    clientWantsStream: settings.value.clientWantsStream,
    modelRaw: modelResolution.value.modelRaw,
    model: modelResolution.value.model,
    modelMetadata: modelResolution.value.modelMetadata,
    input: input.value,
    reasoningLabel: reasoning.value.reasoningLabel,
    codexBody: upstreamBodies.codexBody,
    removedProviderBody: upstreamBodies.removedProviderBody,
  };
  const routingState = await prepareResponsesRouting(req, requestState, modelResolution.value.model);
  return { ok: true, value: { ...requestState, ...routingState } };
};

const releaseResponsesProbeIfSet = (probe: RemovedProviderCircuitProbe | null): void => {
  if (probe) void releaseGlobalRemovedProviderProbe(probe).catch(() => {});
};

const releaseResponsesProbeAndReturn = (probe: RemovedProviderCircuitProbe | null, response: Response): Response => {
  releaseResponsesProbeIfSet(probe);
  return response;
};

const completeCodexPrimaryAttempt = (state: ResponsesHandlerState, attempt: ResponsesRouteAttempt): void => {
  state.primaryResult = attempt;
  const terminalType = attempt.prepared.prepared.terminal?.type;
  if (terminalType === "response.completed" || terminalType === "response.incomplete") {
    markPrimarySemanticRecovery(attempt.routed, state.probe, state.usageContext, terminalType);
  }
};

const recordCodexPrimaryFailureTelemetry = (state: ResponsesHandlerState, failed: FailedResponsesAttempt, terminalType: ResponseStreamTerminalType): void => {
  const telemetry = state.usageContext?.responseTelemetry;
  if (telemetry) {
    state.primaryFailureCorrelation = {
      provider: telemetry.provider,
      accountSlot: telemetry.accountSlot,
      accountCohortId: telemetry.accountCohortId,
      activeGeneration: telemetry.activeGeneration,
      activeTransitionReason: telemetry.activeTransitionReason,
      providerRequestId: telemetry.providerRequestId,
    };
  }
  recordStreamTerminalType(state.usageContext, terminalType);
  const failureKind = failureKindForResponsesAttemptTrigger(failed.trigger);
  if (failureKind && state.usageContext?.responseTelemetry) {
    state.usageContext.responseTelemetry.failureKind = failureKind;
    if (failureKind === "empty_upstream_completion") {
      state.usageContext.responseTelemetry.semanticOutputObserved = false;
    }
  }
};

const settleCodexPrimaryFailure = async (state: ResponsesHandlerState, failure: ResponsesRouteFailure): Promise<Response | null> => {
  const { routed, failed, lifecycle } = failure;
  state.primaryFailureResponse = failed.response;
  const terminalType = responseFailureTerminalType(failed.trigger, failed.signal, state.downstreamSignal);
  recordCodexPrimaryFailureTelemetry(state, failed, terminalType);
  if (failed.trigger === "empty_upstream_completion") {
    const terminalUsage = failed.terminal && isRecord(failed.terminal.value.response) ? extractUsageTokens(failed.terminal.value.response.usage) : null;
    recordTerminalUsage(state.usageContext, terminalUsage, false);
    await finalizeAbandonedPrimaryAttempt(routed, lifecycle, { failureTrigger: failed.trigger });
    return releaseResponsesProbeAndReturn(state.probe, failed.response);
  }
  if (routed.allowRemovedProviderRecovery === false) {
    return releaseResponsesProbeAndReturn(state.probe, failed.response);
  }
  if (routed.gatewayResponse && !isEligibleResponsesAttemptStatus(failed.response)) {
    return releaseResponsesProbeAndReturn(state.probe, failed.response);
  }
  if (
    !isEligibleResponsesAttemptStatus(failed.response) &&
    (failed.trigger === "http_4xx" || failed.trigger === "http_error" || failed.trigger === "read_error")
  ) {
    releaseResponsesProbeIfSet(state.probe);
    lifecycle.terminal("response.failed");
    return failed.response;
  }
  if (!routed.gatewayResponse) {
    await finalizeAbandonedPrimaryAttempt(routed, lifecycle, {
      cancelled: terminalType === "cancelled",
      failureTrigger: failed.trigger,
    });
  }
  if (terminalType === "cancelled") {
    releaseResponsesProbeIfSet(state.probe);
    await recordErrorUsage(state.usageContext);
    return toPreHeaderErrorResponse(state.downstreamSignal.reason, terminalType, routed.provider);
  }
  if (!state.apiKey) return failed.response;
  recordRemovedProviderCircuitTransition(state.usageContext, await recordRemovedProviderEligibleFailure(state.probe));
  recordRemovedProviderFields(state.usageContext, { triggerClass: failed.trigger });
  state.route = "removed_provider";
  return null;
};

const handleCodexPrimaryThrow = async (state: ResponsesHandlerState, error: unknown): Promise<Response> => {
  releaseResponsesProbeIfSet(state.probe);
  const terminalType = classifyPreHeaderFailure(error, state.preHeaderDeadline.signal, state.downstreamSignal);
  recordStreamTerminalType(state.usageContext, terminalType);
  if (terminalType !== "cancelled") {
    logRedactedUpstreamError("[ai.ubq.fi] Upstream fetch failed:", error);
  }
  await recordErrorUsage(state.usageContext);
  return toPreHeaderErrorResponse(error, terminalType, state.usageContext?.responseTelemetry?.provider);
};

const runCodexPrimaryAttempt = async (state: ResponsesHandlerState): Promise<Response | null> => {
  try {
    const remainingMs = state.preHeaderDeadline.remainingMs();
    const failoverReserveMs = Math.min(STREAM_FAILOVER_RESERVE_MS, remainingMs / 2);
    const primaryBudgetMs = state.apiKey || state.paidFallbackAvailable ? Math.max(0, remainingMs - failoverReserveMs) : remainingMs;
    const result = await fetchAndPreparePrimaryResponses(state.codexBody, {
      model: state.model,
      reasoning: state.reasoningLabel,
      clientWantsStream: state.clientWantsStream,
      usageContext: state.usageContext,
      clientVersion: state.modelMetadata.snapshot?.client_version,
      requestSignal: state.requestInferenceSignal,
      downstreamSignal: state.downstreamSignal,
      warnings: state.warnings,
      attemptDeadline: createStreamSemanticDeadline(state.preHeaderDeadline.signal, Math.ceil(primaryBudgetMs)),
      fallbackSignal: state.paidFallbackAvailable ? state.preHeaderDeadline.signal : undefined,
      createFallbackDeadline: state.paidFallbackAvailable
        ? () => createStreamSemanticDeadline(state.preHeaderDeadline.signal, Math.ceil(state.preHeaderDeadline.remainingMs()))
        : undefined,
      rejectPresemanticFailureTerminal: state.apiKey !== null,
      releaseOnProgress: state.clientWantsStream,
    });
    if (result.kind === "ready") {
      completeCodexPrimaryAttempt(state, result.value);
      return null;
    }
    return await settleCodexPrimaryFailure(state, result.value);
  } catch (error) {
    return await handleCodexPrimaryThrow(state, error);
  }
};

const resolveResponsesRecoveryRoute = (recoveryRoute: ResponsesCircuitRoute, fallbackResponse: Response): ResponsesStep<RemovedProviderCircuitProbe | null> => {
  if (recoveryRoute.route !== "codex") return { ok: false, response: fallbackResponse };
  return { ok: true, value: recoveryRoute.probe };
};

const resolveResponsesRecoveryProbe = async (
  claimedProbe: RemovedProviderCircuitProbe | null,
  fallbackResponse: Response
): Promise<ResponsesStep<RemovedProviderCircuitProbe | null>> => {
  if (claimedProbe) return { ok: true, value: claimedProbe };
  const recoveryRoute = await selectRemovedProviderCircuitRoute();
  return resolveResponsesRecoveryRoute(recoveryRoute, fallbackResponse);
};

const acquireResponsesRecoveryProbe = async (fallbackResponse: Response): Promise<ResponsesStep<RemovedProviderCircuitProbe | null>> => {
  const claimedProbe = await claimRemovedProviderEarlyRecoveryProbe();
  return await resolveResponsesRecoveryProbe(claimedProbe, fallbackResponse);
};

const releaseResponsesRecoveryProbe = async (state: ResponsesHandlerState, recoveryProbe: RemovedProviderCircuitProbe | null): Promise<void> => {
  const transition = recoveryProbe ? await releaseGlobalRemovedProviderProbe(recoveryProbe) : "none";
  recordRemovedProviderCircuitTransition(state.usageContext, transition);
};

const settleResponsesRecoveryFailure = async (
  state: ResponsesHandlerState,
  failure: ResponsesRouteFailure,
  recoveryProbe: RemovedProviderCircuitProbe | null,
  fallbackResponse: Response
): Promise<Response> => {
  const { routed, failed, lifecycle } = failure;
  const terminalType = responseFailureTerminalType(failed.trigger, failed.signal, state.downstreamSignal);
  await finalizeAbandonedPrimaryAttempt(routed, lifecycle, {
    cancelled: terminalType === "cancelled",
    failureTrigger: failed.trigger,
  });
  if (failed.trigger === "empty_upstream_completion") {
    if (state.usageContext?.responseTelemetry) {
      state.usageContext.responseTelemetry.failureKind = "empty_upstream_completion";
      state.usageContext.responseTelemetry.semanticOutputObserved = false;
    }
    const terminalUsage = failed.terminal && isRecord(failed.terminal.value.response) ? extractUsageTokens(failed.terminal.value.response.usage) : null;
    recordTerminalUsage(state.usageContext, terminalUsage, false);
    recordStreamTerminalType(state.usageContext, "response.failed");
    await releaseResponsesRecoveryProbe(state, recoveryProbe);
    return failed.response;
  }
  if (terminalType === "cancelled") {
    await releaseResponsesRecoveryProbe(state, recoveryProbe);
    recordStreamTerminalType(state.usageContext, terminalType);
    await recordErrorUsage(state.usageContext);
    return toPreHeaderErrorResponse(state.downstreamSignal.reason, terminalType, routed.provider);
  }
  if (failed.trigger === "semantic_timeout") {
    await releaseResponsesRecoveryProbe(state, recoveryProbe);
    return failed.response;
  }
  if (isEligibleResponsesAttemptStatus(failed.response)) {
    recordRemovedProviderCircuitTransition(state.usageContext, await recordRemovedProviderEligibleFailure(recoveryProbe));
  } else {
    await releaseResponsesRecoveryProbe(state, recoveryProbe);
  }
  selectRemovedProviderTelemetry(state.usageContext);
  return fallbackResponse;
};

const runResponsesRecoveryAttempt = async (
  state: ResponsesHandlerState,
  fallbackAttempt: FailedResponsesAttempt,
  recoveryProbe: RemovedProviderCircuitProbe | null
): Promise<Response | null> => {
  let recovery: Awaited<ReturnType<typeof fetchAndPreparePrimaryResponses>>;
  try {
    recovery = await fetchAndPreparePrimaryResponses(state.codexBody, {
      model: state.model,
      reasoning: state.reasoningLabel,
      clientWantsStream: state.clientWantsStream,
      usageContext: state.usageContext,
      clientVersion: state.modelMetadata.snapshot?.client_version,
      requestSignal: state.requestInferenceSignal,
      downstreamSignal: state.downstreamSignal,
      warnings: state.warnings,
      attemptDeadline: createStreamSemanticDeadline(state.preHeaderDeadline.signal, Math.ceil(state.preHeaderDeadline.remainingMs())),
      rejectPresemanticFailureTerminal: true,
      releaseOnProgress: state.clientWantsStream,
    });
  } catch (error) {
    await releaseResponsesRecoveryProbe(state, recoveryProbe);
    if (state.requestInferenceSignal.aborted) throw state.requestInferenceSignal.reason ?? error;
    selectRemovedProviderTelemetry(state.usageContext);
    return fallbackAttempt.response;
  }
  if (recovery.kind === "failed") {
    return await settleResponsesRecoveryFailure(state, recovery.value, recoveryProbe, fallbackAttempt.response);
  }
  completeCodexPrimaryAttempt(state, recovery.value);
  return null;
};

const settleRemovedProviderFailure = async (state: ResponsesHandlerState, attempt: FailedResponsesAttempt): Promise<Response | null> => {
  void persistFailedRemovedProviderAttempt(state.usageContext, state.fallbackStartedAt, attempt.trigger);
  if (attempt.trigger === "empty_upstream_completion") {
    if (state.usageContext?.responseTelemetry) {
      state.usageContext.responseTelemetry.failureKind = "empty_upstream_completion";
      state.usageContext.responseTelemetry.semanticOutputObserved = false;
    }
    const terminalUsage = attempt.terminal && isRecord(attempt.terminal.value.response) ? extractUsageTokens(attempt.terminal.value.response.usage) : null;
    recordTerminalUsage(state.usageContext, terminalUsage, false);
    recordStreamTerminalType(state.usageContext, "response.failed");
    return attempt.response;
  }
  const primaryFailureResponse = state.primaryFailureResponse;
  if (primaryFailureResponse || attempt.trigger === "terminal_failure") {
    if (primaryFailureResponse && state.usageContext?.responseTelemetry) {
      const telemetry = state.usageContext.responseTelemetry;
      telemetry.provider = state.primaryFailureCorrelation?.provider ?? primaryFailureResponse.headers.get("x-uos-upstream") ?? "chatgpt_codex";
      telemetry.accountSlot = state.primaryFailureCorrelation?.accountSlot ?? null;
      telemetry.accountCohortId = state.primaryFailureCorrelation?.accountCohortId ?? null;
      telemetry.activeGeneration = state.primaryFailureCorrelation?.activeGeneration ?? null;
      telemetry.activeTransitionReason = state.primaryFailureCorrelation?.activeTransitionReason ?? null;
      telemetry.providerRequestId = state.primaryFailureCorrelation?.providerRequestId ?? null;
    } else {
      selectRemovedProviderTelemetry(state.usageContext);
    }
    return primaryFailureResponse ?? attempt.response;
  }
  const recovery = await acquireResponsesRecoveryProbe(attempt.response);
  if (!recovery.ok) return recovery.response;
  state.probe = recovery.value;
  return await runResponsesRecoveryAttempt(state, attempt, recovery.value);
};

const runRemovedProviderAttempt = async (state: ResponsesHandlerState, apiKey: string): Promise<Response | null> => {
  state.fallbackStartedAt = performance.now();
  const removedProvider = await fetchAndPrepareRemovedProviderResponses(state.removedProviderBody, {
    usageContext: state.usageContext,
    requestSignal: state.requestInferenceSignal,
    sessionId: state.sessionId,
    apiKey,
    attemptDeadline: createStreamSemanticDeadline(state.preHeaderDeadline.signal, Math.ceil(state.preHeaderDeadline.remainingMs())),
  });
  if (removedProvider.kind !== "ready") {
    return await settleRemovedProviderFailure(state, removedProvider.attempt);
  }
  state.removedProviderAttempt = removedProvider.attempt;
  state.selectedModel = removedProvider.attempt.selectedModel;
  recordRemovedProviderFields(state.usageContext, {
    selectedModel: state.selectedModel,
    taskType: removedProvider.attempt.taskType,
    semanticCommitment:
      removedProvider.attempt.prepared.semanticKind ?? (removedProvider.attempt.prepared.terminal?.type === "response.completed" ? "terminal_completed" : null),
  });
  return null;
};

const runResponsesFailover = async (state: ResponsesHandlerState): Promise<Response | null> => {
  try {
    if (state.route === "codex") {
      const codexResponse = await runCodexPrimaryAttempt(state);
      if (codexResponse) return codexResponse;
    }

    if (state.route === "removed_provider" && state.apiKey) {
      const removedProviderResponse = await runRemovedProviderAttempt(state, state.apiKey);
      if (removedProviderResponse) return removedProviderResponse;
    }
    return null;
  } finally {
    state.preHeaderDeadline.clear();
  }
};

const removedProviderProbeTransitionForTerminal = (probe: RemovedProviderCircuitProbe | null, terminalType: ResponseStreamTerminalType): Promise<"none"> => {
  if (terminalType !== "cancelled") return recordRemovedProviderEligibleFailure(probe);
  if (probe) return releaseGlobalRemovedProviderProbe(probe);
  return Promise.resolve("none" as const);
};

const reconcileCommittedFailure = (delivery: ResponsesDeliveryState, terminalType: ResponseStreamTerminalType): void => {
  delivery.clearProbeRenewal();
  // A terminal buffered during preflight already describes the provider.
  // A later client-body cancellation is delivery-only and cannot change that
  // provider outcome, health result, or paid settlement.
  if (delivery.providerTerminalValidated) return;
  if (delivery.usageContext?.responseTelemetry?.streamTerminalType === null) {
    recordStreamTerminalType(delivery.usageContext, terminalType);
  }
  const routed = delivery.routed;
  if (routed) {
    void finalizeAbandonedPrimaryAttempt(routed, delivery.lifecycle, { cancelled: terminalType === "cancelled" });
  }
  if (delivery.probe && (routed?.provider === "metered" || routed?.provider === "surplus")) {
    void releaseGlobalRemovedProviderProbe(delivery.probe)
      .then((value) => {
        recordRemovedProviderCircuitTransition(delivery.usageContext, value);
      })
      .catch(() => {});
  } else if (routed?.provider === "chatgpt_codex") {
    void removedProviderProbeTransitionForTerminal(delivery.probe, terminalType)
      .then((value) => {
        recordRemovedProviderCircuitTransition(delivery.usageContext, value);
      })
      .catch(() => {});
  }
  if (delivery.removedProviderAttempt && delivery.usageContext?.responseTelemetry?.removedProviderTerminalStatus !== "response.failed") {
    recordRemovedProviderFields(delivery.usageContext, {
      latencyMs: Math.max(0, Math.round(performance.now() - delivery.fallbackStartedAt)),
      terminalStatus: terminalType,
    });
    void persistRemovedProviderFields(delivery.usageContext);
  }
  void recordErrorUsage(delivery.usageContext);
};

const validateRemovedProviderStreamEvent = (delivery: ResponsesDeliveryState, event: ResponsesStreamEvent): void => {
  if (!delivery.removedProviderAttempt || !delivery.selectedModel) return;
  const candidate = removedProviderModelFromEvent(event.value);
  if (!candidate) return;
  if (candidate !== delivery.selectedModel || !isEligibleRemovedProviderModel(candidate)) {
    throw new ResponsesStreamError("RemovedProvider changed the selected model after stream release.", {
      kind: "malformed_event",
    });
  }
};

const codexResponseTerminalTransition = (response: Response, eventType: string): Promise<void> => {
  if (eventType === "response.completed") return markCodexResponseCompleted(response);
  if (eventType === "response.failed" || eventType === "error") return markCodexResponseUpstreamError(response);
  return releaseCodexResponseProbe(response);
};

const codexTerminalCircuitTransition = (provider: UpstreamProvider, eventType: string, probe: RemovedProviderCircuitProbe | null): Promise<"none"> => {
  if (provider !== "chatgpt_codex") return releaseGlobalRemovedProviderProbe(probe);
  if (eventType === "response.completed" || eventType === "response.incomplete") return closeRemovedProviderCircuit(probe);
  if (eventType === "response.failed" || eventType === "error") return recordRemovedProviderEligibleFailure(probe);
  return releaseGlobalRemovedProviderProbe(probe);
};

const applyRemovedProviderTerminalTransition = (delivery: ResponsesDeliveryState, event: ResponsesStreamEvent, routed: RoutedResponsesUpstream): void => {
  if (delivery.probe) {
    void codexTerminalCircuitTransition(routed.provider, event.type, delivery.probe)
      .then((value) => {
        recordRemovedProviderCircuitTransition(delivery.usageContext, value);
      })
      .catch(() => {});
    return;
  }
  if (routed.provider === "chatgpt_codex" && (event.type === "response.failed" || event.type === "error")) {
    void recordRemovedProviderEligibleFailure(null)
      .then((value) => {
        recordRemovedProviderCircuitTransition(delivery.usageContext, value);
      })
      .catch(() => {});
  }
};

const applyRoutedStreamTerminal = (delivery: ResponsesDeliveryState, routed: RoutedResponsesUpstream, event: ResponsesStreamEvent): void => {
  const terminalUsage = isRecord(event.value.response) ? extractUsageTokens(event.value.response.usage) : null;
  delivery.lifecycle.terminal(event.type, terminalUsage);
  if (routed.provider === "chatgpt_codex") {
    void codexResponseTerminalTransition(routed.response, event.type).catch(() => {});
  }
  applyRemovedProviderTerminalTransition(delivery, event, routed);
};

const onResponsesTerminal = (delivery: ResponsesDeliveryState, event: ResponsesStreamEvent): void => {
  if (!event.terminal) return;
  delivery.clearProbeRenewal();
  const syntheticFailure = isSyntheticResponsesFailureEvent(event);
  if (!syntheticFailure && delivery.providerTerminalValidated) {
    // Buffered collection replays a terminal already settled during
    // preflight, after it has recorded the first SSE event. Complete timing
    // telemetry without repeating provider settlement or usage recording.
    recordStreamTerminalType(delivery.usageContext, event.type as ResponseStreamTerminalType);
    return;
  }
  if (!syntheticFailure) delivery.providerTerminalValidated = true;
  const routed = delivery.routed;
  if (routed && !syntheticFailure) {
    applyRoutedStreamTerminal(delivery, routed, event);
  }
  // A synthetic failure is the client-visible terminal owner after a
  // committed stream breaks. For an abandoned Codex/Metered attempt, keep the
  // underlying EOF/read classification in telemetry so paid-fallback
  // reconciliation retains its diagnostic cause. RemovedProvider owns its
  // synthetic terminal because no later provider can take over after the
  // failover notice has been released.
  if (!syntheticFailure || delivery.removedProviderAttempt) {
    recordResponsesTerminal(event, delivery.usageContext);
  }
  if (delivery.removedProviderAttempt) {
    recordRemovedProviderFields(delivery.usageContext, {
      latencyMs: Math.max(0, Math.round(performance.now() - delivery.fallbackStartedAt)),
      terminalStatus: event.type,
    });
    void persistRemovedProviderFields(delivery.usageContext);
  }
};

const buildResponsesClientWarnings = (state: ResponsesHandlerState, ready: PreparedResponsesAttempt): string[] => {
  const forwardedRemovedProviderControls = new Set([
    "max_output_tokens",
    "max_tool_calls",
    "metadata",
    "prompt_cache_options",
    "prompt_cache_retention",
    "safety_identifier",
    "service_tier",
    "temperature",
    "top_p",
    "truncation",
    "user",
  ]);
  const primaryFailureResponse = state.primaryFailureResponse;
  return [...state.warnings, ...(primaryFailureResponse ? responseWarnings(primaryFailureResponse) : []), ...responseWarnings(ready.response)].filter(
    (warning) => {
      if (!state.removedProviderAttempt) return true;
      if (warning === "prompt_cache_breakpoint_ignored" && countExplicitPromptCacheBreakpoints(state.input) > 0) {
        return false;
      }
      return ![...forwardedRemovedProviderControls].some(
        (key) => Object.prototype.hasOwnProperty.call(state.rawRecord, key) && warning === (WARNING_KEY_MAP.get(key) ?? `${key}_ignored`)
      );
    }
  );
};

const buildResponsesDelivery = (state: ResponsesHandlerState): ResponsesStep<ResponsesDeliveryState> => {
  const ready = state.removedProviderAttempt ?? state.primaryResult?.prepared;
  if (!ready) {
    return {
      ok: false,
      response:
        state.primaryFailureResponse ??
        streamErrorResponse(502, "No upstream provider produced a response.", "upstream_error", "chatgpt_codex", state.warnings),
    };
  }
  // A failed pre-commit attempt is diagnostic evidence for routing, not the
  // terminal result of a later provider. Reset only final-failure telemetry
  // when failover produced a ready response; the selected attempt will record
  // its own terminal or stream failure below.
  if (state.usageContext?.responseTelemetry) {
    state.usageContext.responseTelemetry.failureKind = null;
    state.usageContext.responseTelemetry.syntheticTerminalType = null;
    state.usageContext.responseTelemetry.streamTerminalType = null;
  }
  const lifecycle = state.primaryResult?.lifecycle ?? createMeteredTransportLifecycle(null);
  const routed = state.primaryResult?.routed ?? null;
  const clientWarnings = buildResponsesClientWarnings(state, ready);
  const structuredTextOutput =
    isRecord(state.rawRecord.text) &&
    isRecord(state.rawRecord.text.format) &&
    (state.rawRecord.text.format.type === "json_schema" || state.rawRecord.text.format.type === "json_object");
  const warningModel = state.removedProviderAttempt && !structuredTextOutput ? state.selectedModel : null;
  const probe = state.probe;
  if (probe && ready.prepared.semantic) {
    void renewRemovedProviderCircuitProbe(probe).catch(() => {});
  }
  const probeRenewal = probe && ready.prepared.semantic ? setInterval(() => void renewRemovedProviderCircuitProbe(probe).catch(() => {}), 60_000) : null;
  const clearProbeRenewal = (): void => {
    if (probeRenewal !== null) clearInterval(probeRenewal);
  };
  return {
    ok: true,
    value: {
      usageContext: state.usageContext,
      ready,
      lifecycle,
      routed,
      clientWantsStream: state.clientWantsStream,
      downstreamSignal: state.downstreamSignal,
      clientWarnings,
      warningModel,
      selectedModel: state.selectedModel,
      probe,
      removedProviderAttempt: state.removedProviderAttempt,
      fallbackStartedAt: state.fallbackStartedAt,
      providerTerminalValidated: false,
      clearProbeRenewal,
    },
  };
};

const deliverBufferedResponses = async (delivery: ResponsesDeliveryState): Promise<Response> => {
  const response = await collectBufferedResponses(delivery.ready, {
    warningModel: delivery.warningModel,
    usageContext: delivery.usageContext,
    onTerminal: (event) => {
      onResponsesTerminal(delivery, event);
    },
    validateEvent: (event) => {
      validateRemovedProviderStreamEvent(delivery, event);
    },
    onFailure: (error, details) => {
      const terminalType = classifyStreamFailure(error, delivery.ready.signal, delivery.downstreamSignal);
      if (terminalType !== "cancelled") recordResponsesFailureTelemetry(delivery.usageContext, error, details);
      else if (delivery.usageContext?.responseTelemetry) {
        delivery.usageContext.responseTelemetry.responseCreatedObserved =
          details?.responseCreatedObserved ?? delivery.usageContext.responseTelemetry.responseCreatedObserved;
      }
      reconcileCommittedFailure(delivery, terminalType);
      if (terminalType === "cancelled" || terminalType === "deadline") {
        return toPreHeaderErrorResponse(error, terminalType, delivery.ready.provider);
      }
    },
  });
  return withUosWarning(response, delivery.clientWarnings);
};

const deliverStreamingResponses = (delivery: ResponsesDeliveryState): Response => {
  const body = createOwnedResponsesStream({
    initial: delivery.ready.prepared.buffered,
    iterator: delivery.ready.prepared.iterator,
    responseId: delivery.ready.responseId,
    ...(delivery.warningModel ? { warning: { model: delivery.warningModel } } : {}),
    signal: delivery.ready.signal,
    downstreamSignal: delivery.downstreamSignal,
    abortUpstream: delivery.ready.abort,
    onEvent: (event) => {
      recordResponsesEventTelemetry(delivery.usageContext, event);
      onResponsesTerminal(delivery, event);
    },
    validateEvent: (event) => {
      validateRemovedProviderStreamEvent(delivery, event);
    },
    onFailure: (error, details) => {
      const terminalType = classifyStreamFailure(error, delivery.ready.signal, delivery.downstreamSignal);
      if (terminalType !== "cancelled") recordResponsesFailureTelemetry(delivery.usageContext, error, details);
      else if (delivery.usageContext?.responseTelemetry) {
        delivery.usageContext.responseTelemetry.responseCreatedObserved = details.responseCreatedObserved;
      }
      if (details.failureKind === "empty_upstream_completion") {
        const terminalUsage =
          details.upstreamTerminal && isRecord(details.upstreamTerminal.value.response)
            ? extractUsageTokens(details.upstreamTerminal.value.response.usage)
            : null;
        recordTerminalUsage(delivery.usageContext, terminalUsage, false);
      }
      reconcileCommittedFailure(delivery, terminalType);
    },
    onCancel: () => {
      reconcileCommittedFailure(delivery, "cancelled");
    },
  });
  const headers = new Headers(delivery.ready.response.headers);
  headers.delete("Content-Encoding");
  headers.delete("Content-Length");
  headers.set("Content-Type", "text/event-stream");
  headers.set("x-uos-upstream", delivery.ready.provider);
  return withUosWarning(new Response(withSseKeepalive(body), { status: 200, headers }), delivery.clientWarnings);
};

const deliverPreparedResponses = async (delivery: ResponsesDeliveryState): Promise<Response> => {
  // Preflight has established either semantic ownership or a valid terminal,
  // so this is the content-free release boundary. Parser callbacks record the
  // earlier first upstream SSE event independently.
  recordFirstSemanticCommitment(delivery.usageContext);

  // Preflight can already contain a terminal. Record its provider outcome
  // before returning a body that a client may cancel without consuming.
  if (delivery.ready.prepared.terminal) onResponsesTerminal(delivery, delivery.ready.prepared.terminal);

  if (!delivery.clientWantsStream) return await deliverBufferedResponses(delivery);
  return deliverStreamingResponses(delivery);
};

const handleResponsesInternal = async (req: Request, usageContext?: UsageContext, parsedBody?: unknown): Promise<Response> => {
  const request = await readResponsesRequest(req, parsedBody);
  if (!request.ok) return request.response;
  const { rawRecord, rawBody } = request.value;
  const invalidField = validateResponsesRequestFields(rawRecord, rawBody);
  if (invalidField) return invalidField;
  // DeepSeek official models are dispatched from the Responses adapter before
  // the Codex catalog lookup, exactly as the Chat Completions route is
  // dispatched before Codex model validation. Only an explicit DeepSeek id
  // takes this branch; every other request is unchanged.
  const requestedModel = getString(rawRecord.model)?.trim();
  if (requestedModel && deepSeekUpstreamModelFor(requestedModel)) {
    return await handleDeepSeekResponses(req, rawRecord, requestedModel, usageContext);
  }
  // LithosAI has no Responses endpoint of its own, so this route is served by
  // the shared translation under the LithosAI profile rather than by a
  // provider-side endpoint. Only an explicit LithosAI id takes this branch, so
  // the Cerebras `unsupported_model` refusal below stays untouched.
  if (requestedModel && lithosUpstreamModelFor(requestedModel)) {
    return await handleLithosResponses(req, rawRecord, requestedModel, usageContext);
  }
  const prepared = await prepareResponsesRequest(req, rawRecord, rawBody, usageContext);
  if (!prepared.ok) return prepared.response;
  const failoverResponse = await runResponsesFailover(prepared.value);
  if (failoverResponse) return failoverResponse;
  const delivery = buildResponsesDelivery(prepared.value);
  if (!delivery.ok) return delivery.response;
  return await deliverPreparedResponses(delivery.value);
};

const responsesHandlerTerminalType = (error: unknown, downstreamSignal: AbortSignal): "deadline" | "cancelled" | null => {
  if (isTimeoutFailure(error, downstreamSignal.reason)) return "deadline";
  if (downstreamSignal.aborted) return "cancelled";
  return null;
};

export const runResponsesHandler = async (req: Request, usageContext?: UsageContext, parsedBody?: unknown): Promise<Response> =>
  await runWithResponseTelemetry(usageContext, async (context) => {
    try {
      return await handleResponsesInternal(req, context, parsedBody);
    } catch (error) {
      const downstreamSignal = downstreamSignalFor(req, context);
      const terminalType = responsesHandlerTerminalType(error, downstreamSignal);
      if (terminalType === null) throw error;
      recordStreamTerminalType(context, terminalType);
      await recordErrorUsage(context);
      return toPreHeaderErrorResponse(error, terminalType, context.responseTelemetry?.provider ?? "chatgpt_codex");
    }
  });

export const handleResponses = async (req: Request, usageContext?: UsageContext): Promise<Response> => await runResponsesHandler(req, usageContext);
