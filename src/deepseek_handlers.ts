// DeepSeek Chat and Responses handlers, extracted from src/openai.ts.

import {
  DEEPSEEK_DEFAULT_REASONING_EFFORT,
  DEEPSEEK_FLASH_MODEL,
  deepSeekDefaultOutputAllowance,
  deepSeekThinkingToolChoiceConflict,
  deepSeekToolChoiceThinkingConflictMessage,
  deepSeekUpstreamModelFor,
  normalizeDeepSeekProviderRequestId,
} from "./deepseek.ts";
import { type DeepSeekResponsesEcho, toDeepSeekResponsesChatBody, toDeepSeekResponsesPayload } from "./deepseek_responses.ts";
import { readBoundedResponseBody } from "./bounded_response_body.ts";
import { json, openaiError } from "./http.ts";
import { BUFFERED_INFERENCE_DEADLINE_MS } from "./inference_deadline.ts";
import { isRecord } from "./utils.ts";
import { UsageContext, extractChatUsageTokens, recordCompletionUsage, recordRequestUsage, recordStreamTerminalType } from "./openai_telemetry.ts";
import { markChatSemanticOutput } from "./chat_stream_translation.ts";
import { chatCompletionHasAnswerBearingOutput, deepseekResponseHeaders } from "./upstream_wire.ts";
import { parseStreamField } from "./request_policy.ts";
import { relayResponsesStream } from "./provider_stream_relay.ts";
import {
  DEEPSEEK_BUFFERED_BODY_MAX_BYTES,
  deepSeekChatClientOutputAllowance,
  deepSeekTerminalTypeForPayload,
  deepseekStreamAdapter,
  dispatchDeepSeekUpstream,
  readDeepSeekChatCompletion,
  recordBufferedDeepSeekResponsesTerminal,
  recordDeepSeekResponseHealth,
  respondDeepSeekChatIncompleteCapture,
  respondDeepSeekEmptyBufferedCompletion,
  streamDeepSeekChatCompletion,
  validateDeepSeekChatRequestFields,
} from "./openai.ts";

export const handleDeepSeekChatCompletions = async (
  req: Request,
  rawRecord: Record<string, unknown>,
  modelRaw: string,
  usageContext?: UsageContext
): Promise<Response> => {
  const parsedRequest = validateDeepSeekChatRequestFields(rawRecord);
  if (!parsedRequest.ok) return parsedRequest.response;
  const { reasoning, clientWantsStream } = parsedRequest.value;
  // DeepSeek refuses `tool_choice` `required` and the named-function form while
  // thinking mode is active (upstream 400 "Thinking mode does not support this
  // tool_choice"). Reject the combination at the boundary with a gateway-shaped
  // error naming both fields rather than relaying the provider's message about
  // a parameter this route otherwise advertises.
  const toolChoiceConflict = deepSeekThinkingToolChoiceConflict(reasoning, rawRecord.thinking, rawRecord.tool_choice);
  if (toolChoiceConflict) {
    return openaiError(400, deepSeekToolChoiceThinkingConflictMessage(toolChoiceConflict, "reasoning_effort"), "invalid_request_error", {
      param: "tool_choice",
    });
  }
  // The canonical id the provider serves for this request; the buffered and
  // streamed readers echo it, so an alias never reports a mismatched model.
  const upstreamModel = deepSeekUpstreamModelFor(modelRaw) ?? DEEPSEEK_FLASH_MODEL;

  // Preserve the official nested Chat tools/tool_choice contract. In
  // particular, do not run the Codex-specific flattening that follows this
  // early branch in handleChatCompletionsInternal.
  const deepseekBody: Record<string, unknown> = {
    ...rawRecord,
    reasoning_effort: reasoning,
    stream: clientWantsStream,
  };
  // `thinking` is an input to the effort resolution above, not a wire field for
  // this route: the gateway sends one representation (`reasoning_effort`) so
  // the two can never disagree on the wire.
  delete deepseekBody.thinking;
  if (clientWantsStream) {
    // DeepSeek requires stream_options to be requested alongside a stream, and
    // reports usage on the final content chunk rather than a separate frame.
    if (!isRecord(deepseekBody.stream_options)) deepseekBody.stream_options = { include_usage: true };
  } else {
    // DeepSeek answers 400 when stream_options is present without stream:true.
    delete deepseekBody.stream_options;
  }
  if (usageContext?.responseTelemetry) {
    usageContext.responseTelemetry.provider = "deepseek";
    usageContext.responseTelemetry.reasoning = reasoning;
    // The client's cap when it sent one, else the provider's own default for
    // the requested tier, else unknown. The gateway never supplies a cap here,
    // so an omitted field stays omitted on the wire.
    usageContext.responseTelemetry.outputTokenAllowance = deepSeekChatClientOutputAllowance(rawRecord) ?? deepSeekDefaultOutputAllowance(reasoning);
  }
  await recordRequestUsage(usageContext, {
    model: modelRaw,
    route: "chat.completions",
    stream: clientWantsStream,
    reasoning,
  });

  const dispatched = await dispatchDeepSeekUpstream(req, deepseekBody, modelRaw, usageContext);
  if (!dispatched.ok) return dispatched.response;
  const { upstream, requestSignal, downstreamSignal } = dispatched;
  let providerRequestId = dispatched.providerRequestId;

  if (clientWantsStream) {
    return streamDeepSeekChatCompletion(upstream, providerRequestId, usageContext, downstreamSignal, requestSignal, upstreamModel);
  }

  const captured = await readBoundedResponseBody(upstream, {
    signal: requestSignal,
    maxBytes: DEEPSEEK_BUFFERED_BODY_MAX_BYTES,
    // Successful buffered inference uses the request-level edge deadline, not
    // the one-second error-body default. `requestSignal` still caps the whole
    // request from dispatch through body completion.
    timeoutMs: BUFFERED_INFERENCE_DEADLINE_MS,
    cancellationReason: "DeepSeek Chat Completions response was incomplete",
  });
  if (!captured.complete) {
    return await respondDeepSeekChatIncompleteCapture(usageContext, downstreamSignal, requestSignal, providerRequestId);
  }

  const completion = await readDeepSeekChatCompletion(captured.bytes, upstream.status, providerRequestId, usageContext, upstreamModel);
  if (!completion.ok) return completion.response;

  if (chatCompletionHasAnswerBearingOutput(completion.value)) markChatSemanticOutput(usageContext);
  providerRequestId ??= normalizeDeepSeekProviderRequestId(completion.value.id);
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.providerRequestId = providerRequestId;
  const usage = extractChatUsageTokens(completion.value.usage);
  await recordCompletionUsage(usageContext, usage);
  recordStreamTerminalType(usageContext, "response.completed");
  recordDeepSeekResponseHealth(upstream.status, providerRequestId);
  return json(200, completion.value, deepseekResponseHeaders(providerRequestId));
};

/**
 * Finalizes the buffered DeepSeek Responses branch: reads the captured body,
 * applies the provider terminal's classification, and returns this request's
 * single response.
 *
 * Admission, the request record, the response identity and the echo all belong
 * to the caller; this helper only reuses the already-dispatched upstream, so no
 * second admission, request record or terminal is created.
 */
const finalizeBufferedDeepSeekResponses = async (
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
    maxBytes: DEEPSEEK_BUFFERED_BODY_MAX_BYTES,
    timeoutMs: BUFFERED_INFERENCE_DEADLINE_MS,
    cancellationReason: "DeepSeek Responses adapter body was incomplete",
  });
  if (!captured.complete) {
    return await respondDeepSeekChatIncompleteCapture(options.usageContext, options.downstreamSignal, options.requestSignal, options.providerRequestId);
  }

  const completion = await readDeepSeekChatCompletion(
    captured.bytes,
    options.upstream.status,
    options.providerRequestId,
    options.usageContext,
    options.upstreamModel
  );
  if (!completion.ok) return completion.response;

  let providerRequestId = options.providerRequestId;
  providerRequestId ??= normalizeDeepSeekProviderRequestId(completion.value.id);
  if (options.usageContext?.responseTelemetry) options.usageContext.responseTelemetry.providerRequestId = providerRequestId;
  const firstCompletion = completion.value;
  const payload = toDeepSeekResponsesPayload(firstCompletion, options.modelRaw, options.responseId, options.echo, options.toolNames, options.customToolNames);
  const usage = extractChatUsageTokens(firstCompletion.usage);
  // The provider's own reason decides the terminal first: an explicit
  // truncation is `response.incomplete` and is reported as such. Only a
  // would-be completion is then measured for answer-bearing output, which is
  // the same order the streamed path applies.
  if (deepSeekTerminalTypeForPayload(payload.status) === "response.completed" && !chatCompletionHasAnswerBearingOutput(firstCompletion)) {
    return respondDeepSeekEmptyBufferedCompletion(options.usageContext, usage, options.upstream.status, providerRequestId);
  }
  recordBufferedDeepSeekResponsesTerminal(options.usageContext, payload, usage, options.upstream.status, providerRequestId);
  return json(200, payload, deepseekResponseHeaders(providerRequestId));
};

/**
 * Responses adapter for the DeepSeek official route.
 *
 * The Codex client speaks only the Responses API, so this route translates the
 * request, the buffered payload and the stream through
 * `src/deepseek_responses.ts`. Every provider-level concern (dispatch
 * admission, deadlines, health, telemetry, error reflection) is shared with the
 * Chat route.
 *
 * The translation is no longer forced by a provider gap: DeepSeek now serves a
 * native Responses endpoint (`POST /responses` and `/v1/responses`, probed
 * 2026-09-21). Two reasons the translator is still the right seam, not a
 * legacy shim: the provider's native endpoint is documented as stateless with
 * several control parameters ignored, and this adapter's filler for
 * `reasoning_content` on the tool-bearing tail is a measured provider
 * requirement a native response would have to reproduce. Migrating would be a
 * separate, evidence-driven evaluation against representative histories, not
 * an assumed cure.
 */
export const handleDeepSeekResponses = async (
  req: Request,
  rawRecord: Record<string, unknown>,
  modelRaw: string,
  usageContext?: UsageContext
): Promise<Response> => {
  const parsedStream = parseStreamField(rawRecord.stream);
  if (!parsedStream.ok) return openaiError(400, parsedStream.message, "invalid_request_error", { param: "stream" });
  const clientWantsStream = parsedStream.value;

  const translated = toDeepSeekResponsesChatBody(rawRecord, modelRaw, clientWantsStream);
  const upstreamModel = deepSeekUpstreamModelFor(modelRaw) ?? DEEPSEEK_FLASH_MODEL;
  if (!translated.ok) return openaiError(400, translated.message, "invalid_request_error", { param: translated.param });
  const { body: chatBody, toolNames, customToolNames } = translated.value;

  const echo: DeepSeekResponsesEcho = {
    tools: rawRecord.tools,
    tool_choice: rawRecord.tool_choice,
    parallel_tool_calls: rawRecord.parallel_tool_calls,
    instructions: typeof rawRecord.instructions === "string" && rawRecord.instructions.trim() ? rawRecord.instructions : null,
  };
  const reasoningLabel = typeof chatBody.reasoning_effort === "string" ? chatBody.reasoning_effort : DEEPSEEK_DEFAULT_REASONING_EFFORT;
  // The client's cap when it sent one, else the provider's own measured default
  // for the requested tier, else null. When it is unknown, telemetry stays null
  // rather than inventing a default for the tier.
  const outputAllowance = (typeof chatBody.max_tokens === "number" ? chatBody.max_tokens : null) ?? deepSeekDefaultOutputAllowance(reasoningLabel);
  if (usageContext?.responseTelemetry) {
    usageContext.responseTelemetry.provider = "deepseek";
    usageContext.responseTelemetry.reasoning = reasoningLabel;
    // `applyOutputLimit` put the client's `max_output_tokens` on the wire as
    // `max_tokens`; when it was absent the provider's own tier default applies.
    usageContext.responseTelemetry.outputTokenAllowance = outputAllowance;
  }
  await recordRequestUsage(usageContext, {
    model: modelRaw,
    route: "responses",
    stream: clientWantsStream,
    reasoning: reasoningLabel,
  });

  const dispatched = await dispatchDeepSeekUpstream(req, chatBody, modelRaw, usageContext);
  if (!dispatched.ok) return dispatched.response;
  const { upstream, requestSignal, downstreamSignal } = dispatched;
  const providerRequestId = dispatched.providerRequestId;
  const responseId = `resp_${(providerRequestId ?? crypto.randomUUID()).replace(/[^A-Za-z0-9]/g, "").slice(0, 40)}`;
  const createdAtSeconds = Math.floor(Date.now() / 1000);

  if (clientWantsStream) {
    return streamDeepSeekResponses(
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

  return finalizeBufferedDeepSeekResponses({
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

/**
 * Relays the DeepSeek translated Responses event sequence through the shared
 * writer; this shape skips comment frames and lets the translator decide the
 * terminal under DeepSeek's own profile.
 */
const streamDeepSeekResponses = (
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
  relayResponsesStream(deepseekStreamAdapter, {
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
 * LithosAI route support.
 *
 * The vendor serves OpenAI Chat Completions only — `POST /v1/responses` answers
 * 404 (probed 2026-09-23) — while the gateway's shared Responses adapter
 * (`src/deepseek_responses.ts`) translates a Responses request into a Chat
 * Completions body under a provider profile. Both gateway routes are therefore
 * served natively, and everything provider-level (dispatch admission,
 * deadlines, health, telemetry, error reflection) is shared between them here;
 * the two adapters differ only in how they translate the payload.
 */
