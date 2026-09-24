// LithosAI Chat and Responses handlers, extracted from src/openai.ts.

import { LITHOS_RESPONSES_PROFILE } from "../deepseek/responses.ts";
import { toDeepSeekResponsesChatBody } from "../deepseek/chat-projection.ts";
import { type DeepSeekResponsesEcho, toDeepSeekResponsesPayload } from "../deepseek/responses-payload.ts";
import {
  fetchLithosChatCompletions,
  getLithosProviderRequestId,
  iterateLithosChatCompletionStream,
  LITHOS_DEFAULT_REASONING_EFFORT,
  LITHOS_REASONING_LEVELS,
  LithosError,
  lithosUpstreamModelFor,
  normalizeLithosChatCompletion,
} from "./lithos.ts";
import { ApiKeyQuotaDispatchError } from "../api-key-policy.ts";
import { readBoundedResponseBody } from "../bounded-response-body.ts";
import { json, openaiError } from "../http.ts";
import { BUFFERED_INFERENCE_DEADLINE_MS } from "../inference-deadline.ts";
import { recordLithosProviderHealth } from "./health.ts";
import { isRecord } from "../utils.ts";
import {
  ResponseStreamTerminalType,
  UsageContext,
  UsageTokens,
  extractChatUsageTokens,
  recordAttemptedProvider,
  recordCompletionUsage,
  recordErrorUsage,
  recordFirstProviderDispatch,
  recordFirstProviderHeaders,
  recordRequestUsage,
  recordStreamTerminal,
  recordStreamTerminalType,
  recordTerminalUsage,
} from "../openai-telemetry.ts";
import { EMPTY_UPSTREAM_COMPLETION_MESSAGE, markChatSemanticOutput } from "../chat/stream-translation.ts";
import {
  chatCompletionHasAnswerBearingOutput,
  deepSeekChatBodyDiagnostic,
  lithosResponseHeaders,
  toLithosErrorResponse,
  toLithosUpstreamErrorResponse,
} from "../upstream-wire.ts";
import { parseChatStreamOptions, parseReasoningEffortField, parseStreamField } from "../request-policy.ts";
import { ProviderStreamAdapter, ProviderStreamFrame, relayChatCompletionStream, relayResponsesStream } from "./stream-relay.ts";
import { downstreamSignalFor, inferenceSignal } from "../openai.ts";
import { deepSeekChatClientOutputAllowance, deepSeekTerminalTypeForPayload } from "../deepseek/handlers.ts";

const LITHOS_BUFFERED_BODY_MAX_BYTES = 8 * 1024 * 1024;

/** The seven tiers the provider accepted verbatim on 2026-09-23, as a membership set. */
export const LITHOS_REASONING_LEVEL_SET: ReadonlySet<string> = new Set(LITHOS_REASONING_LEVELS);

/** The canonical wire spelling for an accepted tier, or null when the provider refuses it. */
const lithosReasoningLevel = (value: string): string | null => {
  const level = value.trim().toLowerCase();
  return LITHOS_REASONING_LEVEL_SET.has(level) ? level : null;
};

/**
 * Validates the Chat Completions fields this route owns before any dispatch.
 *
 * `messages` is checked exactly as the DeepSeek branch checks it. The reasoning
 * tier is the one provider-specific rule: the vendor accepted exactly the seven
 * lowercase tiers verbatim and refused everything else with a 400, so a tier
 * outside that set (notably the Codex `ultra` preset, which this wire has no
 * mapping for) fails closed here instead of leaving the gateway only to be
 * refused upstream.
 */
const validateLithosChatRequestFields = (
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
  const reasoningEffort = parseReasoningEffortField(rawRecord.reasoning_effort, "reasoning_effort");
  if (!reasoningEffort.ok) {
    return { ok: false, response: openaiError(400, reasoningEffort.message, "invalid_request_error", { param: "reasoning_effort" }) };
  }
  // The provider's default for an omitted effort was not probed, so the
  // gateway's declared default (the middle verified tier) is sent explicitly
  // and the wire agrees with the capabilities endpoint.
  const reasoning = reasoningEffort.value === undefined ? LITHOS_DEFAULT_REASONING_EFFORT : lithosReasoningLevel(reasoningEffort.value);
  if (reasoning === null) {
    return {
      ok: false,
      response: openaiError(
        400,
        `reasoning_effort '${reasoningEffort.value}' is not supported by LithosAI. Use none, minimal, low, medium, high, xhigh, or max.`,
        "invalid_request_error",
        { param: "reasoning_effort" }
      ),
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
  return { ok: true, value: { reasoning, clientWantsStream: parsedStream.value } };
};

/**
 * The output cap the client supplied, if any. The cap fields are the official
 * Chat contract's (`max_completion_tokens`, then a literal `max_tokens`), not
 * provider-specific, so the shared reader serves this route too.
 */
const lithosChatClientOutputAllowance = (rawRecord: Record<string, unknown>): number | null => deepSeekChatClientOutputAllowance(rawRecord);

type LithosFailureKind =
  | "upstream_error"
  | "upstream_http_error"
  | "upstream_unreachable"
  | "incomplete_response"
  | "invalid_json"
  | "invalid_completion_schema"
  | "lithos_upstream_invalid_response"
  | "deadline"
  | "cancellation"
  | "api_key_quota_reservation_unavailable"
  | "lithos_api_key_missing"
  | "lithos_request_invalid"
  /** A stop reason the gateway cannot place in its terminal vocabulary. */
  | `lithos_finish_reason:${string}`;

const recordLithosFailureKind = (context: UsageContext | undefined, failureKind: LithosFailureKind): void => {
  if (context?.responseTelemetry) context.responseTelemetry.failureKind = failureKind;
};

/**
 * The telemetry classification for an upstream stop reason the gateway cannot
 * place. The value is bounded and carried so an operator can see exactly what
 * the provider said instead of reading a normal completion.
 */
const lithosFinishReasonFailureKind = (finishReason: string | null): LithosFailureKind =>
  `lithos_finish_reason:${finishReason !== null && /^[A-Za-z0-9_.:-]{1,64}$/.test(finishReason) ? finishReason : "unrecognized"}`;

const lithosTerminalTypeForError = (error: unknown, downstreamSignal: AbortSignal): ResponseStreamTerminalType => {
  if (downstreamSignal.aborted) return "cancelled";
  if (error instanceof LithosError && error.code === "gateway_timeout") return "deadline";
  if (error instanceof Error && error.name === "TimeoutError") return "deadline";
  if (error instanceof Error && error.name === "AbortError") return "cancelled";
  return "error";
};

const lithosTransportFailureKind = (error: unknown, terminalType: ResponseStreamTerminalType): LithosFailureKind => {
  if (terminalType === "cancelled") return "cancellation";
  if (terminalType === "deadline") return "deadline";
  if (error instanceof ApiKeyQuotaDispatchError) return "api_key_quota_reservation_unavailable";
  if (error instanceof LithosError) {
    switch (error.code) {
      case "lithos_api_key_missing":
        return "lithos_api_key_missing";
      case "lithos_request_invalid":
        return "lithos_request_invalid";
      case "lithos_upstream_unreachable":
        return "upstream_unreachable";
      case "lithos_upstream_invalid_response":
        return "lithos_upstream_invalid_response";
      default:
        break;
    }
  }
  return "upstream_error";
};

const respondLithosChatInvalidCompletion = async (
  failureKind: "invalid_json" | "invalid_completion_schema",
  upstreamStatus: number,
  providerRequestId: string | null,
  usageContext: UsageContext | undefined
): Promise<Response> => {
  recordStreamTerminalType(usageContext, "error");
  recordLithosFailureKind(usageContext, failureKind);
  void recordLithosProviderHealth("upstream_error", upstreamStatus, Date.now, providerRequestId);
  await recordErrorUsage(usageContext);
  return openaiError(502, "Upstream returned an invalid Chat Completions response.", "lithos_upstream_invalid_response", {
    type: "server_error",
    headers: lithosResponseHeaders(providerRequestId),
  });
};

const readLithosChatCompletion = async (
  bytes: Uint8Array,
  upstreamStatus: number,
  providerRequestId: string | null,
  usageContext: UsageContext | undefined,
  upstreamModel: string
): Promise<{ ok: true; value: Record<string, unknown> } | { ok: false; response: Response }> => {
  let payload: unknown;
  try {
    payload = JSON.parse(new TextDecoder().decode(bytes)) as unknown;
  } catch {
    return { ok: false, response: await respondLithosChatInvalidCompletion("invalid_json", upstreamStatus, providerRequestId, usageContext) };
  }
  const normalized = normalizeLithosChatCompletion(payload, upstreamModel);
  if (!normalized.ok) {
    return { ok: false, response: await respondLithosChatInvalidCompletion("invalid_completion_schema", upstreamStatus, providerRequestId, usageContext) };
  }
  return { ok: true, value: normalized.value };
};

const respondLithosChatDispatchFailure = async (error: unknown, downstreamSignal: AbortSignal, usageContext: UsageContext | undefined): Promise<Response> => {
  const terminalType = lithosTerminalTypeForError(error, downstreamSignal);
  recordLithosFailureKind(usageContext, lithosTransportFailureKind(error, terminalType));
  recordStreamTerminalType(usageContext, terminalType);
  if (terminalType !== "cancelled") {
    void recordLithosProviderHealth("upstream_error", null, Date.now, null);
  }
  await recordErrorUsage(usageContext);
  return toLithosErrorResponse(error);
};

const respondLithosChatUpstreamHttpFailure = async (
  upstream: Response,
  requestSignal: AbortSignal,
  providerRequestId: string | null,
  usageContext: UsageContext | undefined,
  body: Record<string, unknown>
): Promise<Response> => {
  recordLithosResponseHealth(upstream.status, providerRequestId);
  recordLithosFailureKind(usageContext, "upstream_http_error");
  recordStreamTerminalType(usageContext, "response.failed");
  await recordErrorUsage(usageContext);
  // The digest is shape-only (no prompt text, tool names, or ids), so the
  // existing Chat-body reader is provider-agnostic and serves this route too.
  return await toLithosUpstreamErrorResponse(upstream, requestSignal, deepSeekChatBodyDiagnostic(body));
};

const respondLithosChatIncompleteCapture = async (
  usageContext: UsageContext | undefined,
  downstreamSignal: AbortSignal,
  requestSignal: AbortSignal,
  providerRequestId: string | null
): Promise<Response> => {
  let terminalType: ResponseStreamTerminalType = "error";
  let failureKind: LithosFailureKind = "incomplete_response";
  if (downstreamSignal.aborted) {
    terminalType = "cancelled";
    failureKind = "cancellation";
  } else if (requestSignal.aborted) {
    terminalType = "deadline";
    failureKind = "deadline";
  }
  recordLithosFailureKind(usageContext, failureKind);
  recordStreamTerminalType(usageContext, terminalType);
  if (terminalType !== "cancelled") {
    void recordLithosProviderHealth("upstream_error", null, Date.now, providerRequestId);
  }
  await recordErrorUsage(usageContext);
  if (terminalType === "cancelled") {
    return openaiError(499, "Request was cancelled.", "request_cancelled", { type: "server_error", headers: lithosResponseHeaders(providerRequestId) });
  }
  return openaiError(
    terminalType === "deadline" ? 504 : 502,
    terminalType === "deadline" ? "Upstream request exceeded the gateway deadline." : "Upstream returned an incomplete response.",
    terminalType === "deadline" ? "gateway_timeout" : "lithos_upstream_invalid_response",
    { type: "server_error", headers: lithosResponseHeaders(providerRequestId) }
  );
};

/**
 * Maps the LithosAI transport's validated chunks onto the shared frame union.
 * This vendor's wire carries no SSE comment frames and its iterator ends at
 * `[DONE]`, which the shared loops read as an exhausted iterator; this provider
 * therefore claims no keep-alive relay it does not have.
 */
async function* lithosChatStreamFrames(
  upstream: Response,
  upstreamModel: string,
  options: Readonly<{ signal: AbortSignal }>
): AsyncGenerator<ProviderStreamFrame, void, unknown> {
  for await (const chunk of iterateLithosChatCompletionStream(upstream, upstreamModel, options)) {
    yield { kind: "chunk", value: chunk };
  }
}

export const recordLithosResponseHealth = (status: number, providerRequestId: string | null): void => {
  if (status === 401 || status === 403) {
    void recordLithosProviderHealth("auth_invalid", status, Date.now, providerRequestId);
    return;
  }
  if (status === 402) {
    void recordLithosProviderHealth("quota_exhausted", status, Date.now, providerRequestId);
    return;
  }
  if (status === 429) {
    void recordLithosProviderHealth("quota_exhausted", status, Date.now, providerRequestId);
    return;
  }
  if (status >= 500) {
    void recordLithosProviderHealth("upstream_error", status, Date.now, providerRequestId);
    return;
  }
  if (status >= 400) {
    void recordLithosProviderHealth("reachable", status, Date.now, providerRequestId);
    return;
  }
  void recordLithosProviderHealth("success", status, Date.now, providerRequestId);
};

/** The LithosAI seams for the shared stream writers. */
const lithosStreamAdapter: ProviderStreamAdapter = {
  responseHeaders: lithosResponseHeaders,
  frames: lithosChatStreamFrames,
  recordResponseHealth: recordLithosResponseHealth,
  recordProviderError: (status, providerRequestId) => void recordLithosProviderHealth("upstream_error", status, Date.now, providerRequestId),
  recordCancellation: (usageContext) => {
    recordLithosFailureKind(usageContext, "cancellation");
  },
  recordIncompleteResponse: (usageContext) => {
    recordLithosFailureKind(usageContext, "incomplete_response");
  },
  recordFinishFailureKind: (usageContext, finishReason) => {
    recordLithosFailureKind(usageContext, lithosFinishReasonFailureKind(finishReason));
  },
  recordTransportFailure: (usageContext, error, terminalType) => {
    recordLithosFailureKind(usageContext, lithosTransportFailureKind(error, terminalType));
  },
  terminalTypeForError: lithosTerminalTypeForError,
  streamErrorCode: "lithos_upstream_stream_error",
  responsesProfile: LITHOS_RESPONSES_PROFILE,
};

/**
 * Relays the LithosAI Chat Completions SSE stream through the shared writer.
 * The provider reports usage unconditionally - including on the trailing frame
 * whose `choices` is empty - so nothing here gates accounting on
 * `stream_options.include_usage`.
 */
const streamLithosChatCompletion = (
  upstream: Response,
  providerRequestId: string | null,
  usageContext: UsageContext | undefined,
  downstreamSignal: AbortSignal,
  requestSignal: AbortSignal,
  upstreamModel: string
): Response => relayChatCompletionStream(lithosStreamAdapter, upstream, providerRequestId, usageContext, downstreamSignal, requestSignal, upstreamModel);

type LithosDispatchResult =
  | Readonly<{ ok: true; upstream: Response; providerRequestId: string | null; requestSignal: AbortSignal; downstreamSignal: AbortSignal }>
  | Readonly<{ ok: false; response: Response }>;

/**
 * Shared LithosAI dispatch for both gateway routes. It owns the provider
 * request-id capture, the dispatch/headers telemetry, and the failure
 * responders, so the Chat and Responses adapters differ only in how they
 * translate the payload.
 */
const dispatchLithosUpstream = async (
  req: Request,
  body: Record<string, unknown>,
  modelRaw: string,
  usageContext: UsageContext | undefined
): Promise<LithosDispatchResult> => {
  const downstreamSignal = downstreamSignalFor(req, usageContext);
  const requestSignal = inferenceSignal(req, usageContext);
  let upstream: Response;
  try {
    upstream = await fetchLithosChatCompletions(body, modelRaw, {
      signal: requestSignal,
      beforeDispatch: () => usageContext?.beforeProviderDispatch?.("lithos") ?? Promise.resolve(undefined),
      onDispatch: () => {
        recordAttemptedProvider(usageContext, "lithos");
        recordFirstProviderDispatch(usageContext);
      },
      onHeaders: () => {
        recordFirstProviderHeaders(usageContext);
      },
      sentinelUpstreamRecorder: usageContext?.sentinelUpstreamRecorder,
    });
  } catch (error) {
    return { ok: false, response: await respondLithosChatDispatchFailure(error, downstreamSignal, usageContext) };
  }

  const providerRequestId = getLithosProviderRequestId(upstream);
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.providerRequestId = providerRequestId;
  if (!upstream.ok) {
    return { ok: false, response: await respondLithosChatUpstreamHttpFailure(upstream, requestSignal, providerRequestId, usageContext, body) };
  }
  return { ok: true, upstream, providerRequestId, requestSignal, downstreamSignal };
};

/**
 * The LithosAI Chat Completions route.
 *
 * The official nested Chat tools/`tool_choice` contract is preserved: this
 * branch dispatches before the Codex-specific flattening that follows it in
 * `handleChatCompletionsInternal`. Streaming is relayed rather than buffered,
 * because the vendor streams normally and reports usage on every frame
 * regardless of `stream_options`.
 */
export const handleLithosChatCompletions = async (
  req: Request,
  rawRecord: Record<string, unknown>,
  modelRaw: string,
  usageContext?: UsageContext
): Promise<Response> => {
  const parsedRequest = validateLithosChatRequestFields(rawRecord);
  if (!parsedRequest.ok) return parsedRequest.response;
  const { reasoning, clientWantsStream } = parsedRequest.value;
  // The canonical id the provider serves for this request; the buffered and
  // streamed readers echo it, so a differently-cased request never reports a
  // mismatched model.
  const upstreamModel = lithosUpstreamModelFor(modelRaw);
  if (!upstreamModel) {
    return openaiError(400, "The requested model is not configured.", "lithos_request_invalid", { param: "model" });
  }

  // Preserve the official nested Chat tools/tool_choice contract. In
  // particular, do not run the Codex-specific flattening that follows this
  // early branch in handleChatCompletionsInternal.
  const lithosBody: Record<string, unknown> = {
    ...rawRecord,
    reasoning_effort: reasoning,
    stream: clientWantsStream,
  };
  if (!clientWantsStream) {
    // OpenAI's own contract refuses stream_options without stream:true, and this
    // provider needs nothing from it: usage arrives unconditionally.
    delete lithosBody.stream_options;
  }
  if (usageContext?.responseTelemetry) {
    usageContext.responseTelemetry.provider = "lithos";
    usageContext.responseTelemetry.reasoning = reasoning;
    // The client's cap when it sent one, else unknown: this vendor publishes no
    // per-tier default allowance to stand in for it.
    usageContext.responseTelemetry.outputTokenAllowance = lithosChatClientOutputAllowance(rawRecord);
  }
  await recordRequestUsage(usageContext, {
    model: modelRaw,
    route: "chat.completions",
    stream: clientWantsStream,
    reasoning,
  });

  const dispatched = await dispatchLithosUpstream(req, lithosBody, modelRaw, usageContext);
  if (!dispatched.ok) return dispatched.response;
  const { upstream, requestSignal, downstreamSignal } = dispatched;
  const providerRequestId = dispatched.providerRequestId;

  if (clientWantsStream) {
    return streamLithosChatCompletion(upstream, providerRequestId, usageContext, downstreamSignal, requestSignal, upstreamModel);
  }

  const captured = await readBoundedResponseBody(upstream, {
    signal: requestSignal,
    maxBytes: LITHOS_BUFFERED_BODY_MAX_BYTES,
    // Successful buffered inference uses the request-level edge deadline, not
    // the one-second error-body default. `requestSignal` still caps the whole
    // request from dispatch through body completion.
    timeoutMs: BUFFERED_INFERENCE_DEADLINE_MS,
    cancellationReason: "LithosAI Chat Completions response was incomplete",
  });
  if (!captured.complete) {
    return await respondLithosChatIncompleteCapture(usageContext, downstreamSignal, requestSignal, providerRequestId);
  }

  const completion = await readLithosChatCompletion(captured.bytes, upstream.status, providerRequestId, usageContext, upstreamModel);
  if (!completion.ok) return completion.response;

  if (chatCompletionHasAnswerBearingOutput(completion.value)) markChatSemanticOutput(usageContext);
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.providerRequestId = providerRequestId;
  const usage = extractChatUsageTokens(completion.value.usage);
  await recordCompletionUsage(usageContext, usage);
  recordStreamTerminalType(usageContext, "response.completed");
  recordLithosResponseHealth(upstream.status, providerRequestId);
  return json(200, completion.value, lithosResponseHeaders(providerRequestId));
};

/**
 * Fails a buffered LithosAI Responses completion closed when the provider
 * reported success but the translated output carries nothing a client can act
 * on, matching the streamed path's guard.
 */
const respondLithosEmptyBufferedCompletion = (
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
  recordLithosResponseHealth(upstreamStatus, providerRequestId);
  return openaiError(502, EMPTY_UPSTREAM_COMPLETION_MESSAGE, "empty_upstream_completion", {
    type: "server_error",
    headers: lithosResponseHeaders(providerRequestId),
  });
};

/**
 * Records a buffered LithosAI Responses terminal. The payload carries the
 * provider's own terminal, so telemetry reports the terminal the client
 * receives instead of assuming success, and the non-completed classification
 * matches the streamed path.
 */
const recordBufferedLithosResponsesTerminal = (
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
    recordLithosFailureKind(usageContext, terminalType === "response.incomplete" ? "incomplete_response" : "upstream_error");
    void recordLithosProviderHealth("upstream_error", upstreamStatus, Date.now, providerRequestId);
  }
  recordLithosResponseHealth(upstreamStatus, providerRequestId);
};

/**
 * Finalizes the buffered LithosAI Responses branch: reads the captured body,
 * applies the provider terminal's classification, and returns this request's
 * single response. Admission, the request record, the response identity and the
 * echo all belong to the caller, so no second terminal is created here.
 */
const finalizeBufferedLithosResponses = async (
  options: Readonly<{
    upstream: Response;
    modelRaw: string;
    responseId: string;
    echo: DeepSeekResponsesEcho;
    toolNames: ReadonlyMap<string, string>;
    customToolNames: ReadonlySet<string>;
    upstreamModel: string;
    providerRequestId: string | null;
    requestSignal: AbortSignal;
    downstreamSignal: AbortSignal;
    usageContext?: UsageContext;
  }>
): Promise<Response> => {
  const captured = await readBoundedResponseBody(options.upstream, {
    signal: options.requestSignal,
    maxBytes: LITHOS_BUFFERED_BODY_MAX_BYTES,
    timeoutMs: BUFFERED_INFERENCE_DEADLINE_MS,
    cancellationReason: "LithosAI Responses adapter body was incomplete",
  });
  if (!captured.complete) {
    return await respondLithosChatIncompleteCapture(options.usageContext, options.downstreamSignal, options.requestSignal, options.providerRequestId);
  }

  const completion = await readLithosChatCompletion(
    captured.bytes,
    options.upstream.status,
    options.providerRequestId,
    options.usageContext,
    options.upstreamModel
  );
  if (!completion.ok) return completion.response;

  const providerRequestId = options.providerRequestId;
  if (options.usageContext?.responseTelemetry) options.usageContext.responseTelemetry.providerRequestId = providerRequestId;
  const firstCompletion = completion.value;
  const payload = toDeepSeekResponsesPayload(
    firstCompletion,
    options.modelRaw,
    options.responseId,
    options.echo,
    options.toolNames,
    options.customToolNames,
    LITHOS_RESPONSES_PROFILE
  );
  const usage = extractChatUsageTokens(firstCompletion.usage);
  // The provider's own reason decides the terminal first: an explicit
  // truncation is `response.incomplete` and is reported as such. Only a
  // would-be completion is then measured for answer-bearing output, which is
  // the same order the streamed path applies.
  if (deepSeekTerminalTypeForPayload(payload.status) === "response.completed" && !chatCompletionHasAnswerBearingOutput(firstCompletion)) {
    return respondLithosEmptyBufferedCompletion(options.usageContext, usage, options.upstream.status, providerRequestId);
  }
  recordBufferedLithosResponsesTerminal(options.usageContext, payload, usage, options.upstream.status, providerRequestId);
  return json(200, payload, lithosResponseHeaders(providerRequestId));
};

/**
 * Relays the LithosAI translated Responses event sequence through the shared
 * writer under this provider's own profile; the shared shape skips comment
 * frames and lets the translator decide the terminal.
 */
const streamLithosResponses = (
  upstream: Response,
  requestedModel: string,
  responseId: string,
  createdAtSeconds: number,
  echo: DeepSeekResponsesEcho,
  toolNames: ReadonlyMap<string, string>,
  customToolNames: ReadonlySet<string>,
  providerRequestId: string | null,
  usageContext: UsageContext | undefined,
  downstreamSignal: AbortSignal,
  requestSignal: AbortSignal,
  upstreamModel: string
): Response =>
  relayResponsesStream(lithosStreamAdapter, {
    upstream,
    requestedModel,
    responseId,
    createdAtSeconds,
    echo,
    toolNames,
    customToolNames,
    providerRequestId,
    usageContext,
    downstreamSignal,
    requestSignal,
    upstreamModel,
  });

/**
 * Responses adapter for the LithosAI route.
 *
 * The vendor has no `/v1/responses` endpoint (probed: 404), so the shared
 * translation in `src/deepseek_responses.ts` runs under
 * `LITHOS_RESPONSES_PROFILE`: the profile owns this provider's model table,
 * reasoning-tier acceptance, usage counters and streaming-usage policy, and the
 * translation itself is the same one the DeepSeek route uses. Everything
 * provider-level (dispatch admission, deadlines, health, telemetry, error
 * reflection) is shared with the Chat route above.
 */
export const handleLithosResponses = async (
  req: Request,
  rawRecord: Record<string, unknown>,
  modelRaw: string,
  usageContext?: UsageContext
): Promise<Response> => {
  const parsedStream = parseStreamField(rawRecord.stream);
  if (!parsedStream.ok) return openaiError(400, parsedStream.message, "invalid_request_error", { param: "stream" });
  const clientWantsStream = parsedStream.value;
  // The route only dispatches here for a LithosAI id, so this guard covers the
  // handler's own contract rather than a reachable client path.
  const upstreamModel = lithosUpstreamModelFor(modelRaw);
  if (!upstreamModel) return openaiError(400, `model '${modelRaw}' is not a LithosAI official model`, "invalid_request_error", { param: "model" });

  const translated = toDeepSeekResponsesChatBody(rawRecord, modelRaw, clientWantsStream, LITHOS_RESPONSES_PROFILE);
  if (!translated.ok) return openaiError(400, translated.message, "invalid_request_error", { param: translated.param });
  const { body: chatBody, toolNames, customToolNames } = translated.value;

  const echo: DeepSeekResponsesEcho = {
    tools: rawRecord.tools,
    tool_choice: rawRecord.tool_choice,
    parallel_tool_calls: rawRecord.parallel_tool_calls,
    instructions: typeof rawRecord.instructions === "string" && rawRecord.instructions.trim() ? rawRecord.instructions : null,
  };
  const reasoningLabel = typeof chatBody.reasoning_effort === "string" ? chatBody.reasoning_effort : LITHOS_DEFAULT_REASONING_EFFORT;
  // The client's cap when it sent one, else unknown: this vendor publishes no
  // per-tier default allowance to stand in for it.
  const outputAllowance = typeof chatBody.max_tokens === "number" ? chatBody.max_tokens : null;
  if (usageContext?.responseTelemetry) {
    usageContext.responseTelemetry.provider = "lithos";
    usageContext.responseTelemetry.reasoning = reasoningLabel;
    // `applyOutputLimit` put the client's `max_output_tokens` on the wire as
    // `max_tokens`; an absent cap stays unknown rather than being invented.
    usageContext.responseTelemetry.outputTokenAllowance = outputAllowance;
  }
  await recordRequestUsage(usageContext, {
    model: modelRaw,
    route: "responses",
    stream: clientWantsStream,
    reasoning: reasoningLabel,
  });

  const dispatched = await dispatchLithosUpstream(req, chatBody, modelRaw, usageContext);
  if (!dispatched.ok) return dispatched.response;
  const { upstream, requestSignal, downstreamSignal } = dispatched;
  const providerRequestId = dispatched.providerRequestId;
  const responseId = `resp_${(providerRequestId ?? crypto.randomUUID()).replace(/[^A-Za-z0-9]/g, "").slice(0, 40)}`;
  const createdAtSeconds = Math.floor(Date.now() / 1000);

  if (clientWantsStream) {
    return streamLithosResponses(
      upstream,
      modelRaw,
      responseId,
      createdAtSeconds,
      echo,
      toolNames,
      customToolNames,
      providerRequestId,
      usageContext,
      downstreamSignal,
      requestSignal,
      upstreamModel
    );
  }

  return finalizeBufferedLithosResponses({
    upstream,
    modelRaw,
    responseId,
    echo,
    toolNames,
    customToolNames,
    upstreamModel,
    providerRequestId,
    requestSignal,
    downstreamSignal,
    usageContext,
  });
};
