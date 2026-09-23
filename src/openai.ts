import { handleLithosResponses } from "./lithos_handlers.ts";
import { handleDeepSeekResponses } from "./deepseek_handlers.ts";
import {
  buildCodexRequest,
  fetchCodexResponses,
  getCodexModelsSnapshotDefaultModel,
  markCodexResponseCompleted,
  markCodexResponseUpstreamError,
  releaseCodexResponseProbe,
} from "./codex.ts";
import { CEREBRAS_GPT_OSS_120B_MODEL } from "./cerebras.ts";
import { deepSeekUpstreamModelFor } from "./deepseek.ts";
import { lithosUpstreamModelFor } from "./lithos.ts";

import { isProviderEnabled, loadProviderSelectionCached } from "./provider_selection.ts";
import { DEFAULT_REASONING_EFFORT, type ReasoningEffort } from "./defaults.ts";
import { openaiError } from "./http.ts";
import {
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
import { ResponsesStreamError, type ResponsesStreamEvent, withSseKeepalive } from "./responses_stream.ts";
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
import { createOwnedResponsesStream, isGatewayFailoverWarningItem, isSyntheticResponsesFailureEvent } from "./responses_failover_stream.ts";
import {} from "./paid_fallback.ts";
import { getString, isRecord } from "./utils.ts";
import type { MessageContentItem, ResponseInputItem, ResponsesRequest } from "./types.ts";
import { loadDebugRoutingConfig } from "./debug_routing.ts";
import type {} from "./sentinel_upstream_capture.ts";

import {
  ActiveTransitionReason,
  ResponseStreamTerminalType,
  RoutedResponsesUpstream,
  UpstreamProvider,
  UsageContext,
  classifyPreHeaderFailure,
  classifyStreamFailure,
  countExplicitPromptCacheBreakpoints,
  extractUsageTokens,
  isTimeoutFailure,
  persistFailedRemovedProviderAttempt,
  persistRemovedProviderFields,
  promptCacheKeyPresent,
  promptCacheModeFor,
  recordErrorUsage,
  recordFirstSemanticCommitment,
  recordRemovedProviderFields,
  recordRequestUsage,
  recordResponsesEventTelemetry,
  recordResponsesFailureTelemetry,
  recordStreamTerminalType,
  recordTerminalUsage,
  runWithResponseTelemetry,
  selectRemovedProviderTelemetry,
  streamErrorResponse,
  MeteredTransportLifecycle,
} from "./openai_telemetry.ts";
import {
  normalizeFunctionCallOutputItem,
  normalizeModelForCodex,
  normalizeResponseContentItem,
  normalizeResponseMessageItem,
  validatePromptCacheControls,
} from "./input_normalization.ts";
import { recordResponsesTerminal } from "./chat_stream_translation.ts";
import { logRedactedUpstreamError, toPreHeaderErrorResponse } from "./upstream_wire.ts";
import {
  isTemporaryFreeSurplusModel,
  CodexModelMetadata,
  PassthroughToolSchemaKey,
  WARNING_KEY_MAP,
  applyPassthroughToCodexRequest,
  buildIgnoredWarnings,
  getCodexModelMetadata,
  normalizeReasoningParamForCodex,
  parseReasoningParam,
  parseStreamField,
  reasoningEffortForCodexRequest,
  resolveDefaultReasoningLabel,
  resolveReasoningLabelFromParam,
  responseWarnings,
  validateCodexModelAvailable,
  validateKnownUnsupportedPromptCacheUse,
  withUosWarning,
} from "./request_policy.ts";
export const temporaryFreeSurplusCapabilityError = (model: string, body: Record<string, unknown>): Response | null =>
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

export const defaultModelUnavailableError = (): Response =>
  openaiError(503, "Default model is unavailable: no configured default model or Codex model snapshot.", "server_error");

export const getDefaultReasoningEffort = async (): Promise<ReasoningEffort> => {
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
export const CHAT_COMPLETIONS_ALLOWED_KEYS = new Set(CHAT_COMPLETIONS_REQUEST_KEYS);
const RESPONSES_ALLOWED_KEYS = new Set(RESPONSES_REQUEST_KEYS);
const CODEX_RESPONSES_EXTENSION_KEYS = new Set(["client_metadata"]);
/**
 * Fields a first-party DeepSeek client sends on the provider's own Chat
 * contract that the official OpenAI Chat schema does not define. Kept separate
 * from the OpenAI allowlist so the compatibility surface stays explicit and
 * cannot silently widen the OpenAI-compatible routes; see AGENTS.md's Codex CLI
 * compatibility rule for the same pattern.
 */
export const DEEPSEEK_CHAT_EXTENSION_KEYS = new Set(["thinking"]);

export const findUnknownKey = (record: Record<string, unknown>, allowed: ReadonlySet<string>, extensions?: ReadonlySet<string>): string | null => {
  for (const key of Object.keys(record)) {
    if (!allowed.has(key) && !extensions?.has(key)) return key;
  }
  return null;
};
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
