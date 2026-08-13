import {
  buildCodexRequest,
  CODEX_AUTH_REAUTH_MESSAGE,
  CODEX_AUTH_REAUTH_WARNING,
  CODEX_QUOTA_BLOCKED_ERROR_CODE,
  CODEX_UPSTREAM_DEGRADED_ERROR_CODE,
  CodexError,
  type CodexModelsSnapshot,
  fetchCodexResponses,
  getCodexAuthWarning,
  getCodexModelsSnapshotDefaultModel,
  getCodexResponseSlot,
  getCodexRoutingError,
  loadCodexModelsSnapshot,
  loadFullCodexModelsSnapshot,
  markCodexResponseCompleted,
  releaseCodexResponseProbe,
} from "./codex.ts";
import {
  CEREBRAS_GPT_OSS_120B_MODEL,
  CerebrasError,
  fetchCerebrasChatCompletions,
  getCerebrasProviderRequestId,
  normalizeCerebrasChatCompletion,
  normalizeCerebrasProviderRequestId,
  readCerebrasApiKey,
} from "./cerebras.ts";
import { CEREBRAS_RATE_LIMIT_HEADERS } from "./cerebras_rate_limits.ts";
import { getCatalogClientVersion, handleCodexCatalogModels } from "./codex_catalog.ts";
import {
  CODEX_CHATGPT_PROMPT_CACHE_PROVIDER,
  normalizePromptCacheCapabilities,
  type PromptCacheControls,
} from "./codex_models.ts";
import { type ApiKeyProviderDispatch, ApiKeyQuotaDispatchError } from "./api_key_policy.ts";
import { DEFAULT_REASONING_EFFORT, normalizeReasoningEffort, type ReasoningEffort } from "./defaults.ts";
import { readBoundedResponseBody } from "./bounded_response_body.ts";
import { json, openaiError } from "./http.ts";
import {
  BUFFERED_INFERENCE_DEADLINE_MS,
  createInferenceSignal,
  createStreamFirstEventDeadline,
  createStreamSemanticDeadline,
} from "./inference_deadline.ts";
import { getKv } from "./kv.ts";
import { loadRuntimeConfig } from "./runtime_config.ts";
import { CHAT_COMPLETIONS_REQUEST_KEYS, RESPONSES_REQUEST_KEYS } from "./openai_schema.ts";
import { readJsonBody } from "./request.ts";
import {
  type PreflightedResponsesStream,
  preflightResponsesStream,
  readResponsesStream,
  ResponsesStreamError,
  type ResponsesStreamEvent,
  withSseKeepalive,
} from "./responses_stream.ts";
import {
  deriveOpenRouterSessionId,
  fetchOpenRouterResponses,
  isEligibleOpenRouterModel,
  openRouterModelFromEvent,
  openRouterTaskTypeFromResponse,
  readOpenRouterApiKey,
} from "./openrouter.ts";
import {
  claimOpenRouterEarlyRecoveryProbe,
  closeOpenRouterCircuit,
  type OpenRouterCircuitProbe,
  recordOpenRouterEligibleFailure,
  releaseOpenRouterCircuitProbe as releaseGlobalOpenRouterProbe,
  selectOpenRouterCircuitRoute,
} from "./openrouter_circuit.ts";
import { recordOpenRouterTelemetry } from "./openrouter_telemetry.ts";
import {
  createOwnedResponsesStream,
  isSyntheticResponsesFailureEvent,
  type PreparedResponsesStream,
  prepareResponsesStreamForCommit,
  responseIdFromEvents,
} from "./responses_failover_stream.ts";
import {
  type PaidFallbackReservation,
  recordYunwuAmbiguousFailure,
  recordYunwuPrefetchCancellation,
  recordYunwuTerminal,
  recordYunwuUndispatchedCancellation,
  recordYunwuUpstreamResponse,
  reservePaidFallback,
} from "./paid_fallback.ts";
import { recordCerebrasProviderHealth, recordYunwuProviderHealth } from "./provider_health.ts";
import { getString, isRecord, sha256Hex } from "./utils.ts";
import type {
  ChatCompletionRequest,
  MessageContentItem,
  PromptCacheBreakpoint,
  ResponseInputItem,
  ResponseMessageItem,
  ResponsesRequest,
} from "./types.ts";
import { fetchYunwuResponses, YunwuError } from "./yunwu.ts";

const getDefaultModel = async (): Promise<string | null> => {
  const runtime = await loadRuntimeConfig();
  return runtime?.default_model ?? getCodexModelsSnapshotDefaultModel(runtime?.codex_models ?? null);
};

const inferenceSignal = (request: Request): AbortSignal => createInferenceSignal(request.signal);

/**
 * Internal test seam for exercising the public OpenAI handlers through the
 * same guarded banked-reset flow. It has no request-schema or runtime-config
 * surface, and remains unset in production.
 */
type CodexBankedResetOptionsForTest = NonNullable<Parameters<typeof fetchCodexResponses>[1]>["bankedReset"];
let codexBankedResetOptionsForTest: CodexBankedResetOptionsForTest | null = null;

export const setCodexBankedResetOptionsForTest = (
  options: CodexBankedResetOptionsForTest | null,
): void => {
  codexBankedResetOptionsForTest = options;
};

const defaultModelUnavailableError = (): Response =>
  openaiError(
    503,
    "Default model is unavailable: no configured default model or Codex model snapshot.",
    "server_error",
  );

const getDefaultReasoningEffort = async (): Promise<ReasoningEffort> => {
  return (await loadRuntimeConfig())?.default_reasoning_effort ?? DEFAULT_REASONING_EFFORT;
};

type UsageContext = Readonly<{
  keyId: string | null;
  kernelRepo: { owner: string; repo: string } | null;
  kernelOrg: { owner: string } | null;
  paidFallbackEnabled?: boolean;
  idempotencyPrincipal?: string | null;
  requestId?: string;
  startedAtMs?: number;
  startedAtMonotonicMs?: number;
  responseTelemetry?: ResponseTelemetryState;
  /** Commits an admitted API-key reservation exactly once before transport. */
  beforeProviderDispatch?: (
    provider: "cerebras" | "chatgpt_codex" | "openrouter" | "yunwu" | "voyage",
  ) => Promise<ApiKeyProviderDispatch | void>;
  /** Test seam for proving one terminal usage observation per response. */
  onTerminalUsage?: (usage: UsageTokens | null, completed: boolean) => void;
}>;

type UpstreamProvider = "cerebras" | "chatgpt_codex" | "openrouter" | "yunwu";
export type InferenceFallbackReason = "primary_401" | "primary_403" | "primary_429";
export type UsageTelemetryStatus = "missing" | "partial" | "reported" | "invalid";
export type PromptCacheMode = "implicit" | "explicit" | "legacy_retention" | "unspecified";
export type AffinityOutcome = "none" | "preferred" | "failover" | "shadow_only";

export type ResponseTelemetry = Readonly<{
  provider: string;
  fallbackReason: InferenceFallbackReason | null;
  model: string | null;
  reasoning: string | null;
  inputTokens: number | null;
  cachedInputTokens: number | null;
  cacheWriteInputTokens: number | null;
  outputTokens: number | null;
  totalTokens: number | null;
  usageObserved: boolean;
  usageTelemetryStatus: UsageTelemetryStatus;
  promptCacheKeyPresent: boolean;
  promptCacheMode: PromptCacheMode;
  explicitBreakpointCount: number;
  accountSlot: number | null;
  affinityOutcome: AffinityOutcome;
  quotaUsedPercent: number | null | undefined;
  completed: boolean;
  streamTerminalType: ResponseStreamTerminalType | null;
  stream: boolean | null;
  providerRequestId: string | null;
  firstProviderDispatchMs: number | null;
  firstProviderHeadersMs: number | null;
  firstCodexDispatchMs: number | null;
  firstCodexHeadersMs: number | null;
  firstSseEventMs: number | null;
  streamTerminalMs: number | null;
  attemptedProviders: readonly string[];
  openRouterTriggerClass: string | null;
  openRouterCircuitTransition: string | null;
  openRouterSelectedModel: string | null;
  openRouterTaskType: string | null;
  openRouterSemanticCommitment: string | null;
  openRouterLatencyMs: number | null;
  openRouterTerminalStatus: string | null;
}>;

export type ResponseStreamTerminalType =
  | "response.completed"
  | "response.failed"
  | "response.incomplete"
  | "error"
  | "eof"
  | "cancelled"
  | "deadline";

type ResponseTelemetryState = {
  provider: string | null;
  fallbackReason: InferenceFallbackReason | null;
  model: string | null;
  reasoning: string | null;
  inputTokens: number | null;
  cachedInputTokens: number | null;
  cacheWriteInputTokens: number | null;
  outputTokens: number | null;
  totalTokens: number | null;
  usageObserved: boolean;
  usageTelemetryStatus: UsageTelemetryStatus;
  promptCacheKeyPresent: boolean;
  promptCacheMode: PromptCacheMode;
  explicitBreakpointCount: number;
  accountSlot: number | null;
  affinityOutcome: AffinityOutcome;
  quotaUsedPercent: number | null | undefined;
  completed: boolean;
  streamTerminalType: ResponseStreamTerminalType | null;
  stream: boolean | null;
  providerRequestId: string | null;
  firstProviderDispatchMs: number | null;
  firstProviderHeadersMs: number | null;
  firstCodexDispatchMs: number | null;
  firstCodexHeadersMs: number | null;
  firstSseEventMs: number | null;
  streamTerminalMs: number | null;
  attemptedProviders: string[];
  openRouterTriggerClass: string | null;
  openRouterCircuitTransition: string | null;
  openRouterSelectedModel: string | null;
  openRouterTaskType: string | null;
  openRouterSemanticCommitment: string | null;
  openRouterLatencyMs: number | null;
  openRouterTerminalStatus: string | null;
};

const responseTelemetry = new WeakMap<Response, ResponseTelemetryState>();

const createResponseTelemetryState = (): ResponseTelemetryState => ({
  provider: null,
  fallbackReason: null,
  model: null,
  reasoning: null,
  inputTokens: null,
  cachedInputTokens: null,
  cacheWriteInputTokens: null,
  outputTokens: null,
  totalTokens: null,
  usageObserved: false,
  usageTelemetryStatus: "missing",
  promptCacheKeyPresent: false,
  promptCacheMode: "unspecified",
  explicitBreakpointCount: 0,
  accountSlot: null,
  affinityOutcome: "none",
  quotaUsedPercent: undefined,
  completed: false,
  streamTerminalType: null,
  stream: null,
  providerRequestId: null,
  firstProviderDispatchMs: null,
  firstProviderHeadersMs: null,
  firstCodexDispatchMs: null,
  firstCodexHeadersMs: null,
  firstSseEventMs: null,
  streamTerminalMs: null,
  attemptedProviders: [],
  openRouterTriggerClass: null,
  openRouterCircuitTransition: null,
  openRouterSelectedModel: null,
  openRouterTaskType: null,
  openRouterSemanticCommitment: null,
  openRouterLatencyMs: null,
  openRouterTerminalStatus: null,
});

const withResponseTelemetryContext = (
  context: UsageContext | undefined,
  state: ResponseTelemetryState,
): UsageContext => ({
  keyId: context?.keyId ?? null,
  kernelRepo: context?.kernelRepo ?? null,
  kernelOrg: context?.kernelOrg ?? null,
  paidFallbackEnabled: context?.paidFallbackEnabled,
  idempotencyPrincipal: context?.idempotencyPrincipal,
  requestId: context?.requestId,
  startedAtMs: context?.startedAtMs,
  startedAtMonotonicMs: context?.startedAtMonotonicMs,
  beforeProviderDispatch: context?.beforeProviderDispatch,
  onTerminalUsage: context?.onTerminalUsage,
  responseTelemetry: state,
});

const attachResponseTelemetry = (response: Response, state: ResponseTelemetryState): Response => {
  state.provider ??= response.headers.get("x-uos-upstream") || "gateway";
  responseTelemetry.set(response, state);
  return response;
};

export const getResponseTelemetry = (response: Response): ResponseTelemetry | null => {
  const state = responseTelemetry.get(response);
  if (!state) return null;
  return {
    provider: state.provider ?? (response.headers.get("x-uos-upstream") || "gateway"),
    fallbackReason: state.fallbackReason,
    model: state.model,
    reasoning: state.reasoning,
    inputTokens: state.inputTokens,
    cachedInputTokens: state.cachedInputTokens,
    cacheWriteInputTokens: state.cacheWriteInputTokens,
    outputTokens: state.outputTokens,
    totalTokens: state.totalTokens,
    usageObserved: state.usageObserved,
    usageTelemetryStatus: state.usageTelemetryStatus,
    promptCacheKeyPresent: state.promptCacheKeyPresent,
    promptCacheMode: state.promptCacheMode,
    explicitBreakpointCount: state.explicitBreakpointCount,
    accountSlot: state.accountSlot,
    affinityOutcome: state.affinityOutcome,
    quotaUsedPercent: state.quotaUsedPercent,
    completed: state.completed,
    streamTerminalType: state.streamTerminalType,
    stream: state.stream,
    providerRequestId: state.providerRequestId,
    firstProviderDispatchMs: state.firstProviderDispatchMs,
    firstProviderHeadersMs: state.firstProviderHeadersMs,
    firstCodexDispatchMs: state.firstCodexDispatchMs,
    firstCodexHeadersMs: state.firstCodexHeadersMs,
    firstSseEventMs: state.firstSseEventMs,
    streamTerminalMs: state.streamTerminalMs,
    attemptedProviders: [...state.attemptedProviders],
    openRouterTriggerClass: state.openRouterTriggerClass,
    openRouterCircuitTransition: state.openRouterCircuitTransition,
    openRouterSelectedModel: state.openRouterSelectedModel,
    openRouterTaskType: state.openRouterTaskType,
    openRouterSemanticCommitment: state.openRouterSemanticCommitment,
    openRouterLatencyMs: state.openRouterLatencyMs,
    openRouterTerminalStatus: state.openRouterTerminalStatus,
  };
};

const recordAttemptedProvider = (context: UsageContext | undefined, provider: string): void => {
  const attempted = context?.responseTelemetry?.attemptedProviders;
  if (attempted && !attempted.includes(provider)) attempted.push(provider);
};

type ResponseTelemetryTimingField =
  | "firstProviderDispatchMs"
  | "firstProviderHeadersMs"
  | "firstCodexDispatchMs"
  | "firstCodexHeadersMs"
  | "firstSseEventMs"
  | "streamTerminalMs";

// Timings are elapsed from handler ingress using a monotonic clock. They are
// telemetry only: missing context leaves the corresponding field unavailable.
const recordResponseTiming = (
  context: UsageContext | undefined,
  field: ResponseTelemetryTimingField,
): void => {
  const state = context?.responseTelemetry;
  const startedAtMs = context?.startedAtMonotonicMs;
  if (!state || state[field] !== null || typeof startedAtMs !== "number" || !Number.isFinite(startedAtMs)) return;
  state[field] = Math.max(0, Math.round(performance.now() - startedAtMs));
};

const recordFirstCodexDispatch = (context: UsageContext | undefined): void => {
  recordResponseTiming(context, "firstProviderDispatchMs");
  recordResponseTiming(context, "firstCodexDispatchMs");
};

const recordFirstCodexHeaders = (context: UsageContext | undefined): void => {
  recordResponseTiming(context, "firstProviderHeadersMs");
  recordResponseTiming(context, "firstCodexHeadersMs");
};

const recordFirstProviderDispatch = (context: UsageContext | undefined): void => {
  recordResponseTiming(context, "firstProviderDispatchMs");
};

const recordFirstProviderHeaders = (context: UsageContext | undefined): void => {
  recordResponseTiming(context, "firstProviderHeadersMs");
};

const recordOpenRouterFields = (
  context: UsageContext | undefined,
  fields: Readonly<{
    triggerClass?: string | null;
    circuitTransition?: string | null;
    selectedModel?: string | null;
    taskType?: string | null;
    semanticCommitment?: string | null;
    latencyMs?: number | null;
    terminalStatus?: string | null;
  }>,
): void => {
  const telemetry = context?.responseTelemetry;
  if (!telemetry) return;
  if (fields.triggerClass !== undefined) telemetry.openRouterTriggerClass = fields.triggerClass;
  if (fields.circuitTransition !== undefined) telemetry.openRouterCircuitTransition = fields.circuitTransition;
  if (fields.selectedModel !== undefined) telemetry.openRouterSelectedModel = fields.selectedModel;
  if (fields.taskType !== undefined) telemetry.openRouterTaskType = fields.taskType;
  if (fields.semanticCommitment !== undefined) telemetry.openRouterSemanticCommitment = fields.semanticCommitment;
  if (fields.latencyMs !== undefined) telemetry.openRouterLatencyMs = fields.latencyMs;
  if (fields.terminalStatus !== undefined) telemetry.openRouterTerminalStatus = fields.terminalStatus;
};

const persistOpenRouterFields = (context: UsageContext | undefined): Promise<void> => {
  const telemetry = context?.responseTelemetry;
  if (!telemetry) return Promise.resolve();
  return recordOpenRouterTelemetry({
    attempted_provider: telemetry.attemptedProviders.join(",") || null,
    trigger_class: telemetry.openRouterTriggerClass,
    circuit_transition: telemetry.openRouterCircuitTransition,
    selected_model: telemetry.openRouterSelectedModel,
    task_type: telemetry.openRouterTaskType,
    latency_ms: telemetry.openRouterLatencyMs,
    terminal_status: telemetry.openRouterTerminalStatus,
    semantic_commitment: telemetry.openRouterSemanticCommitment,
  }).catch(() => {});
};

const persistFailedOpenRouterAttempt = (
  context: UsageContext | undefined,
  startedAtMonotonicMs: number,
  triggerClass: string,
): Promise<void> => {
  recordOpenRouterFields(context, {
    triggerClass,
    latencyMs: Math.max(0, Math.round(performance.now() - startedAtMonotonicMs)),
    terminalStatus: "failed_before_commit",
  });
  return persistOpenRouterFields(context);
};

const recordFirstSseEvent = (context: UsageContext | undefined): void => {
  recordResponseTiming(context, "firstSseEventMs");
};

const recordStreamTerminal = (context: UsageContext | undefined): void => {
  recordResponseTiming(context, "streamTerminalMs");
};

const runWithResponseTelemetry = async (
  context: UsageContext | undefined,
  run: (context: UsageContext) => Promise<Response>,
): Promise<Response> => {
  const state = createResponseTelemetryState();
  const response = await run(withResponseTelemetryContext(context, state));
  return attachResponseTelemetry(response, state);
};

type RoutedResponsesUpstream = Readonly<{
  response: Response;
  provider: UpstreamProvider;
  paidFallback: PaidFallbackReservation | null;
  gatewayResponse: boolean;
  fallbackReason: InferenceFallbackReason | null;
}>;

export type UsageTokens = Readonly<{
  inputTokens: number | null;
  cachedInputTokens: number | null;
  cacheWriteInputTokens: number | null;
  outputTokens: number | null;
  totalTokens: number | null;
  status: UsageTelemetryStatus;
}>;

const normalizeTokenCount = (value: unknown): number | null => {
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  const count = Math.trunc(value);
  if (count < 0) return null;
  return count;
};

type ParsedUsageToken = Readonly<{ value: number | null; invalid: boolean }>;

const parseUsageToken = (value: unknown, present: boolean): ParsedUsageToken => {
  if (!present) return { value: null, invalid: false };
  if (typeof value !== "number" || !Number.isSafeInteger(value) || value < 0) {
    return { value: null, invalid: true };
  }
  return { value, invalid: false };
};

/**
 * Normalizes terminal Responses usage for gateway telemetry. The fixed
 * cache-scope experiment reuses this parser so it cannot invent a divergent
 * interpretation of cache-read or cache-write fields.
 */
export const extractUsageTokens = (value: unknown): UsageTokens | null => {
  if (value === undefined) return null;
  if (!isRecord(value) || Array.isArray(value)) {
    return {
      inputTokens: null,
      cachedInputTokens: null,
      cacheWriteInputTokens: null,
      outputTokens: null,
      totalTokens: null,
      status: "invalid",
    };
  }
  const has = (key: string): boolean => Object.prototype.hasOwnProperty.call(value, key);
  const inputTokens = parseUsageToken(value.input_tokens, has("input_tokens"));
  const outputTokens = parseUsageToken(value.output_tokens, has("output_tokens"));
  const totalTokens = parseUsageToken(value.total_tokens, has("total_tokens"));
  const detailsPresent = has("input_tokens_details");
  const details = detailsPresent && isRecord(value.input_tokens_details) && !Array.isArray(value.input_tokens_details)
    ? value.input_tokens_details
    : null;
  const cachedInputTokens = parseUsageToken(
    details?.cached_tokens,
    details !== null && Object.prototype.hasOwnProperty.call(details, "cached_tokens"),
  );
  const cacheWriteInputTokens = parseUsageToken(
    details?.cache_write_tokens,
    details !== null && Object.prototype.hasOwnProperty.call(details, "cache_write_tokens"),
  );

  const coreMissing = inputTokens.value === null || outputTokens.value === null || totalTokens.value === null;
  // OpenAI documents cached_tokens for every response, including an explicit
  // zero below the cacheability threshold. cache_write_tokens remains
  // model-dependent, so its absence does not downgrade otherwise complete
  // cache-read telemetry.
  const cacheReadMissing = cachedInputTokens.value === null;
  // cache_write_tokens is an independent dimension: an input can be both
  // newly cached and partly served from cache. cached_tokens, however, is a
  // subset of the request input and may never exceed input_tokens.
  const cachedTokensExceedInput = inputTokens.value !== null && cachedInputTokens.value !== null &&
    cachedInputTokens.value > inputTokens.value;
  const inconsistentTotals = !coreMissing && inputTokens.value + outputTokens.value !== totalTokens.value;
  const invalid = inputTokens.invalid || outputTokens.invalid || totalTokens.invalid ||
    (detailsPresent && details === null) || cachedInputTokens.invalid || cacheWriteInputTokens.invalid ||
    cachedTokensExceedInput || inconsistentTotals;

  return {
    inputTokens: inputTokens.value,
    cachedInputTokens: cachedInputTokens.value,
    cacheWriteInputTokens: cacheWriteInputTokens.value,
    outputTokens: outputTokens.value,
    totalTokens: totalTokens.value,
    status: invalid ? "invalid" : coreMissing || cacheReadMissing ? "partial" : "reported",
  };
};

const toChatUsage = (usage: UsageTokens | null): Record<string, unknown> | null => {
  if (
    usage === null || usage.inputTokens === null || usage.outputTokens === null || usage.totalTokens === null
  ) {
    return null;
  }
  const promptTokenDetails: Record<string, number> = {};
  // Prompt Caching documents both fields on Chat Completions usage details.
  if (usage.cachedInputTokens !== null) promptTokenDetails.cached_tokens = usage.cachedInputTokens;
  if (usage.cacheWriteInputTokens !== null) promptTokenDetails.cache_write_tokens = usage.cacheWriteInputTokens;
  return {
    prompt_tokens: usage.inputTokens,
    completion_tokens: usage.outputTokens,
    total_tokens: usage.totalTokens,
    ...(Object.keys(promptTokenDetails).length ? { prompt_tokens_details: promptTokenDetails } : {}),
  };
};

const promptCacheModeFor = (rawRecord: Record<string, unknown>): PromptCacheMode => {
  const options = isRecord(rawRecord.prompt_cache_options) && !Array.isArray(rawRecord.prompt_cache_options)
    ? rawRecord.prompt_cache_options
    : null;
  if (options?.mode === "explicit") return "explicit";
  // OpenAI's cache policy defaults to implicit whenever options are supplied
  // without an explicit mode (for example, a ttl-only configuration).
  if (options !== null) return "implicit";
  if (Object.prototype.hasOwnProperty.call(rawRecord, "prompt_cache_retention")) return "legacy_retention";
  return "unspecified";
};

const countExplicitPromptCacheBreakpoints = (input: readonly ResponseInputItem[]): number => {
  let count = 0;
  const countContent = (content: unknown): void => {
    if (!Array.isArray(content)) return;
    for (const contentItem of content) {
      if (!isRecord(contentItem) || !isRecord(contentItem.prompt_cache_breakpoint)) continue;
      if (contentItem.prompt_cache_breakpoint.mode === "explicit") count += 1;
    }
  };
  for (const item of input) {
    if (!isRecord(item)) continue;
    countContent(item.content);
    if (item.type === "function_call_output") countContent(item.output);
  }
  return count;
};

const promptCacheKeyPresent = (rawRecord: Record<string, unknown>): boolean =>
  typeof rawRecord.prompt_cache_key === "string" && rawRecord.prompt_cache_key.trim().length > 0;

const extractChatUsageTokens = (value: unknown): UsageTokens | null => {
  if (!isRecord(value) || Array.isArray(value)) return null;
  const inputTokens = normalizeTokenCount(value.prompt_tokens);
  const outputTokens = normalizeTokenCount(value.completion_tokens);
  const totalTokens = normalizeTokenCount(value.total_tokens);
  if (inputTokens === null || outputTokens === null || totalTokens === null) return null;
  return {
    inputTokens,
    cachedInputTokens: 0,
    cacheWriteInputTokens: null,
    outputTokens,
    totalTokens,
    status: "reported",
  };
};

const recordRequestUsage = (
  context: UsageContext | undefined,
  details: {
    model: string;
    route: string;
    stream: boolean;
    reasoning: string | null;
    promptCacheKeyPresent?: boolean;
    promptCacheMode?: PromptCacheMode;
    explicitBreakpointCount?: number;
  },
): Promise<void> => {
  if (context?.responseTelemetry) {
    context.responseTelemetry.model = details.model;
    context.responseTelemetry.reasoning = details.reasoning;
    context.responseTelemetry.stream = details.stream;
    context.responseTelemetry.promptCacheKeyPresent = details.promptCacheKeyPresent ?? false;
    context.responseTelemetry.promptCacheMode = details.promptCacheMode ?? "unspecified";
    context.responseTelemetry.explicitBreakpointCount = details.explicitBreakpointCount ?? 0;
  }
  return Promise.resolve();
};

const recordCompletionUsage = (
  context: UsageContext | undefined,
  usage: UsageTokens | null,
): Promise<void> => {
  recordTerminalUsage(context, usage, true);
  return Promise.resolve();
};

const recordTerminalUsage = (
  context: UsageContext | undefined,
  usage: UsageTokens | null,
  completed: boolean,
): void => {
  const telemetry = context?.responseTelemetry;
  if (!telemetry) return;
  telemetry.inputTokens = usage?.inputTokens ?? null;
  telemetry.cachedInputTokens = usage?.cachedInputTokens ?? null;
  telemetry.cacheWriteInputTokens = usage?.cacheWriteInputTokens ?? null;
  telemetry.outputTokens = usage?.outputTokens ?? null;
  telemetry.totalTokens = usage?.totalTokens ?? null;
  telemetry.usageObserved = usage !== null;
  telemetry.usageTelemetryStatus = usage?.status ?? "missing";
  telemetry.completed = completed;
  try {
    context?.onTerminalUsage?.(usage, completed);
  } catch {
    // A test/observability callback cannot alter response delivery.
  }
};

const recordErrorUsage = (_context: UsageContext | undefined): Promise<void> => Promise.resolve();

const recordStreamTerminalType = (
  context: UsageContext | undefined,
  terminalType: ResponseStreamTerminalType,
): void => {
  const telemetry = context?.responseTelemetry;
  if (!telemetry) return;
  telemetry.streamTerminalType = terminalType;
  if (telemetry.firstSseEventMs !== null) recordStreamTerminal(context);
};

const classifyStreamFailure = (
  error: unknown,
  signal: AbortSignal,
  downstreamSignal: AbortSignal,
): ResponseStreamTerminalType => {
  if (downstreamSignal.aborted) return "cancelled";
  if (signal.aborted) return "deadline";
  if (error instanceof ResponsesStreamError && error.kind === "inactivity_timeout") return "deadline";
  if (error instanceof ResponsesStreamError && error.kind === "premature_eof") return "eof";
  return "error";
};

const streamErrorResponse = (
  status: number,
  message: string,
  code: string,
  provider: UpstreamProvider,
  warnings: readonly string[],
  type?: string,
): Response => {
  const mergedWarnings = Array.from(new Set(warnings));
  const hasAuthWarning = mergedWarnings.includes(CODEX_AUTH_REAUTH_WARNING);
  const headers: Record<string, string> = { "x-uos-upstream": provider };
  if (mergedWarnings.length) headers["x-uos-warning"] = mergedWarnings.join(", ");
  return openaiError(
    status,
    hasAuthWarning ? message + " " + CODEX_AUTH_REAUTH_MESSAGE : message,
    code,
    { ...(type ? { type } : {}), headers },
  );
};

const streamPreflightFailureResponse = (
  terminalType: ResponseStreamTerminalType,
  provider: UpstreamProvider,
  warnings: readonly string[] = [],
): Response => {
  if (terminalType === "deadline") {
    return streamErrorResponse(
      504,
      "Upstream stream exceeded the gateway deadline before its first SSE event.",
      "gateway_timeout",
      provider,
      warnings,
      "server_error",
    );
  }
  return streamErrorResponse(
    502,
    "Codex upstream stream ended unexpectedly.",
    "codex_upstream_stream_error",
    provider,
    warnings,
  );
};

type ResponsesAttemptTrigger =
  | "http_5xx"
  | "missing_body"
  | "malformed_event"
  | "premature_eof"
  | "semantic_timeout"
  | "read_error"
  | "invalid_model";

type PreparedResponsesAttempt = Readonly<{
  provider: UpstreamProvider;
  response: Response;
  prepared: PreparedResponsesStream;
  responseId: string | null;
  selectedModel: string | null;
  taskType: string | null;
  signal: AbortSignal;
  clearDeadline: () => void;
}>;

type FailedResponsesAttempt = Readonly<{
  provider: UpstreamProvider;
  response: Response;
  trigger: ResponsesAttemptTrigger;
  signal: AbortSignal;
  clearDeadline: () => void;
}>;

type ResponsesAttemptResult =
  | { kind: "ready"; attempt: PreparedResponsesAttempt }
  | { kind: "failed"; attempt: FailedResponsesAttempt };

const isEligibleResponsesAttemptStatus = (response: Response): boolean => response.status >= 500;

const triggerForResponsesError = (error: unknown, signal: AbortSignal): ResponsesAttemptTrigger => {
  if (signal.aborted && signal.reason instanceof Error && signal.reason.name === "TimeoutError") {
    return "semantic_timeout";
  }
  if (error instanceof ResponsesStreamError) {
    if (error.kind === "malformed_event" || error.kind === "event_too_large") return "malformed_event";
    if (error.kind === "premature_eof") return "premature_eof";
    if (error.kind === "inactivity_timeout") return "semantic_timeout";
  }
  return "read_error";
};

const safeFailedAttemptResponse = (
  response: Response,
  provider: UpstreamProvider,
  trigger: ResponsesAttemptTrigger,
  warnings: readonly string[],
): Response => {
  if (!response.ok) return response;
  if (trigger === "semantic_timeout") {
    return streamErrorResponse(
      504,
      "Upstream stream exceeded the gateway deadline before semantic output.",
      "gateway_timeout",
      provider,
      warnings,
      "server_error",
    );
  }
  if (trigger === "missing_body") {
    return streamErrorResponse(
      502,
      "Upstream response missing body.",
      "upstream_missing_body",
      provider,
      warnings,
    );
  }
  return streamErrorResponse(
    502,
    "Upstream Responses stream ended unexpectedly.",
    "upstream_stream_error",
    provider,
    warnings,
  );
};

const prepareResponsesAttempt = async (
  response: Response,
  provider: UpstreamProvider,
  deadline: Readonly<{ signal: AbortSignal; clear: () => void }>,
  requestSignal: AbortSignal,
  warnings: readonly string[],
  options: Readonly<{ requireEligibleModel?: boolean; rejectFailedTerminal?: boolean }> = {},
): Promise<ResponsesAttemptResult> => {
  const fail = (trigger: ResponsesAttemptTrigger, failedResponse = response): ResponsesAttemptResult => ({
    kind: "failed",
    attempt: {
      provider,
      response: safeFailedAttemptResponse(failedResponse, provider, trigger, warnings),
      trigger,
      signal: deadline.signal,
      clearDeadline: deadline.clear,
    },
  });
  if (!response.ok) {
    const trigger = isEligibleResponsesAttemptStatus(response) ? "http_5xx" : "read_error";
    const normalized = await toOpenAiUpstreamErrorResponse(response, provider, deadline.signal);
    deadline.clear();
    return fail(trigger, normalized);
  }
  if (!response.body) {
    deadline.clear();
    return fail("missing_body");
  }
  const iterator = readResponsesStream(response.body, deadline.signal, {
    firstEventTimeoutMs: BUFFERED_INFERENCE_DEADLINE_MS,
  });
  let preparedStream: PreparedResponsesStream | null = null;
  try {
    const prepared = await prepareResponsesStreamForCommit(iterator);
    preparedStream = prepared;
    if (
      options.rejectFailedTerminal && prepared.terminal &&
      prepared.terminal.type !== "response.completed"
    ) {
      deadline.clear();
      return fail("read_error");
    }
    const responseId = responseIdFromEvents(prepared.buffered);
    if (options.requireEligibleModel && !responseId) {
      await iterator.return("missing response id").catch(() => {});
      return fail("malformed_event");
    }
    let selectedModel: string | null = null;
    let taskType: string | null = null;
    for (const event of prepared.buffered) {
      const candidate = openRouterModelFromEvent(event.value);
      if (candidate) {
        if (selectedModel && selectedModel !== candidate) {
          await iterator.return("inconsistent model identity").catch(() => {});
          return fail("invalid_model");
        }
        selectedModel = candidate;
      }
      if (!taskType && isRecord(event.value.response)) {
        taskType = openRouterTaskTypeFromResponse(event.value.response);
      }
    }
    while (options.requireEligibleModel && !selectedModel && !prepared.terminal) {
      const next = await iterator.next();
      if (next.done || !next.value) break;
      prepared.buffered.push(next.value);
      responseIdFromEvents(prepared.buffered);
      const candidate = openRouterModelFromEvent(next.value.value);
      if (candidate) {
        if (selectedModel && selectedModel !== candidate) {
          await iterator.return("inconsistent model identity").catch(() => {});
          return fail("invalid_model");
        }
        selectedModel = candidate;
      }
      if (!taskType && isRecord(next.value.value.response)) {
        taskType = openRouterTaskTypeFromResponse(next.value.value.response);
      }
      if (next.value.terminal) break;
    }
    if (options.requireEligibleModel && !prepared.buffered.some((event) => event.type === "response.created")) {
      await iterator.return("missing response.created").catch(() => {});
      return fail("malformed_event");
    }
    deadline.clear();
    if (options.requireEligibleModel && (!selectedModel || !isEligibleOpenRouterModel(selectedModel))) {
      await iterator.return("invalid selected model").catch(() => {});
      return fail("invalid_model");
    }
    return {
      kind: "ready",
      attempt: {
        provider,
        response,
        prepared,
        responseId,
        selectedModel,
        taskType,
        signal: deadline.signal,
        clearDeadline: deadline.clear,
      },
    };
  } catch (error) {
    await preparedStream?.iterator.return(error).catch(() => {});
    deadline.clear();
    if (requestSignal.aborted) throw requestSignal.reason ?? error;
    return fail(triggerForResponsesError(error, deadline.signal));
  }
};

type ResponsesRouteAttempt = Readonly<{
  routed: RoutedResponsesUpstream;
  prepared: PreparedResponsesAttempt;
  lifecycle: YunwuTransportLifecycle;
}>;

type ResponsesRouteFailure = Readonly<{
  routed: RoutedResponsesUpstream;
  failed: FailedResponsesAttempt;
  lifecycle: YunwuTransportLifecycle;
}>;

const responseFailureTerminalType = (
  trigger: ResponsesAttemptTrigger,
  signal: AbortSignal,
  downstreamSignal: AbortSignal,
): ResponseStreamTerminalType => {
  if (downstreamSignal.aborted) return "cancelled";
  if (trigger === "semantic_timeout" || signal.aborted) return "deadline";
  if (trigger === "premature_eof") return "eof";
  return "error";
};

const fetchAndPreparePrimaryResponses = async (
  body: Record<string, unknown>,
  options: Readonly<{
    model: string;
    reasoning: string | null;
    clientWantsStream: boolean;
    usageContext?: UsageContext;
    clientVersion?: string | null;
    requestSignal: AbortSignal;
    warnings: readonly string[];
  }>,
): Promise<{ kind: "ready"; value: ResponsesRouteAttempt } | { kind: "failed"; value: ResponsesRouteFailure }> => {
  const deadline = createStreamSemanticDeadline(options.requestSignal);
  let routed: RoutedResponsesUpstream;
  try {
    routed = await fetchResponsesWithPaidFallback(body, {
      model: options.model,
      route: "responses",
      stream: options.clientWantsStream,
      reasoning: options.reasoning,
      usageContext: options.usageContext,
      clientVersion: options.clientVersion,
      signal: deadline.signal,
    });
  } catch (error) {
    deadline.clear();
    if (options.requestSignal.aborted || error instanceof ApiKeyQuotaDispatchError) throw error;
    if (
      error instanceof CodexError &&
      (error.code === "gateway_timeout" || error.code === "codex_upstream_unreachable")
    ) {
      logRedactedUpstreamError("[ai.ubq.fi] Upstream fetch failed:", error);
      const response = toCodexErrorResponse(error, "chatgpt_codex");
      const trigger: ResponsesAttemptTrigger = error.code === "gateway_timeout" ? "semantic_timeout" : "read_error";
      return {
        kind: "failed",
        value: {
          routed: {
            response,
            provider: "chatgpt_codex",
            paidFallback: null,
            gatewayResponse: false,
            fallbackReason: null,
          },
          lifecycle: createYunwuTransportLifecycle(null),
          failed: {
            provider: "chatgpt_codex",
            response,
            trigger,
            signal: deadline.signal,
            clearDeadline: deadline.clear,
          },
        },
      };
    }
    throw error;
  }
  const lifecycle = createYunwuTransportLifecycle(routed.paidFallback);
  if (routed.gatewayResponse) {
    deadline.clear();
    const trigger: ResponsesAttemptTrigger = routed.response.status === 504
      ? "semantic_timeout"
      : routed.response.status >= 500
      ? "http_5xx"
      : "read_error";
    return {
      kind: "failed",
      value: {
        routed,
        lifecycle,
        failed: {
          provider: routed.provider,
          response: routed.response,
          trigger,
          signal: deadline.signal,
          clearDeadline: deadline.clear,
        },
      },
    };
  }
  const prepared = await prepareResponsesAttempt(
    routed.response,
    routed.provider,
    deadline,
    options.requestSignal,
    [...options.warnings, ...responseWarnings(routed.response)],
  );
  return prepared.kind === "ready"
    ? { kind: "ready", value: { routed, prepared: prepared.attempt, lifecycle } }
    : { kind: "failed", value: { routed, failed: prepared.attempt, lifecycle } };
};

const fetchAndPrepareOpenRouterResponses = async (
  body: Record<string, unknown>,
  options: Readonly<{
    usageContext?: UsageContext;
    requestSignal: AbortSignal;
    sessionId: string | null;
    apiKey: string;
  }>,
): Promise<ResponsesAttemptResult> => {
  const deadline = createStreamSemanticDeadline(options.requestSignal);
  recordAttemptedProvider(options.usageContext, "openrouter");
  if (options.usageContext?.responseTelemetry) options.usageContext.responseTelemetry.provider = "openrouter";
  let response: Response;
  try {
    const result = await fetchOpenRouterResponses(body, {
      apiKey: options.apiKey,
      sessionId: options.sessionId,
      signal: deadline.signal,
      timing: {
        onDispatch: () => recordFirstProviderDispatch(options.usageContext),
        onHeaders: () => recordFirstProviderHeaders(options.usageContext),
      },
      beforeDispatch: () => options.usageContext?.beforeProviderDispatch?.("openrouter") ?? Promise.resolve(),
    });
    response = result.response;
  } catch (error) {
    deadline.clear();
    if (options.requestSignal.aborted) throw options.requestSignal.reason ?? error;
    return {
      kind: "failed",
      attempt: {
        provider: "openrouter",
        response: streamErrorResponse(
          502,
          "OpenRouter request failed before response headers were received.",
          "openrouter_upstream_unreachable",
          "openrouter",
          [],
        ),
        trigger: triggerForResponsesError(error, deadline.signal),
        signal: deadline.signal,
        clearDeadline: deadline.clear,
      },
    };
  }
  return await prepareResponsesAttempt(
    response,
    "openrouter",
    deadline,
    options.requestSignal,
    [],
    { requireEligibleModel: true, rejectFailedTerminal: true },
  );
};

const finalizeAbandonedPrimaryAttempt = (
  routed: RoutedResponsesUpstream,
  lifecycle: YunwuTransportLifecycle,
  cancelled = false,
): void => {
  if (routed.provider === "chatgpt_codex") {
    void releaseCodexResponseProbe(routed.response).catch(() => {});
  } else if (routed.provider === "yunwu" && !routed.gatewayResponse) {
    if (cancelled) lifecycle.cancelled();
    else lifecycle.ambiguous();
  }
};

const markPrimarySemanticRecovery = (
  _routed: RoutedResponsesUpstream,
  circuitProbe: OpenRouterCircuitProbe | null,
  usageContext?: UsageContext,
): void => {
  if (circuitProbe) {
    void closeOpenRouterCircuit(circuitProbe).then((transition) => {
      if (transition !== "none") recordOpenRouterFields(usageContext, { circuitTransition: transition });
    }).catch(() => {});
  }
};

const collectBufferedResponses = async (
  attempt: PreparedResponsesAttempt,
  options: Readonly<{
    warningModel?: string | null;
    usageContext?: UsageContext;
    onTerminal?: (event: ResponsesStreamEvent) => void;
    validateEvent?: (event: ResponsesStreamEvent) => void;
    onFailure?: (error: unknown) => void;
  }> = {},
): Promise<Response> => {
  const initial = options.warningModel
    ? (() => {
      const stream = createOwnedResponsesStream({
        initial: attempt.prepared.buffered,
        iterator: attempt.prepared.iterator,
        responseId: attempt.responseId,
        warning: { model: options.warningModel! },
        validateEvent: options.validateEvent,
        onFailure: options.onFailure,
      });
      return readResponsesStream(stream);
    })()
    : (async function* (): AsyncGenerator<ResponsesStreamEvent> {
      for (const event of attempt.prepared.buffered) {
        options.validateEvent?.(event);
        yield event;
      }
      for await (const event of attempt.prepared.iterator) {
        options.validateEvent?.(event);
        yield event;
      }
    })();
  let finalResponse: Record<string, unknown> | null = null;
  let outputText = "";
  const outputItems: Record<string, unknown>[] = [];
  try {
    for await (const event of initial) {
      recordFirstSseEvent(options.usageContext);
      const ev = event.value;
      if (event.type === "response.output_text.delta") outputText += getString(ev.delta) ?? "";
      if (event.type === "response.output_item.done" && isRecord(ev.item)) outputItems.push(ev.item);
      if (event.type === "error") {
        options.onTerminal?.(event);
        return streamErrorResponse(
          502,
          "Upstream Responses stream ended unexpectedly.",
          "upstream_stream_error",
          attempt.provider,
          [],
        );
      }
      if (
        (event.type === "response.completed" || event.type === "response.failed" ||
          event.type === "response.incomplete") &&
        isRecord(ev.response) && !Array.isArray(ev.response)
      ) {
        options.onTerminal?.(event);
        finalResponse = ev.response;
        break;
      }
    }
  } catch (error) {
    options.onFailure?.(error);
    return streamErrorResponse(
      502,
      "Upstream Responses stream ended unexpectedly.",
      "upstream_stream_error",
      attempt.provider,
      [],
    );
  }
  if (!finalResponse) {
    return streamErrorResponse(
      502,
      "Upstream Responses stream ended unexpectedly.",
      "upstream_stream_error",
      attempt.provider,
      [],
    );
  }
  finalResponse = withAccumulatedResponseItems(finalResponse, outputItems);
  finalResponse = withAccumulatedResponseText(finalResponse, outputText);
  // The terminal callback owns usage and terminal telemetry for buffered and
  // streamed Responses alike. Do not record it a second time here.
  return json(200, finalResponse, { "x-uos-upstream": attempt.provider });
};

const formatErrorSnippet = (error: unknown, maxLen = 280): string => {
  const raw = error instanceof Error ? error.message : String(error);
  const trimmed = raw.trim();
  if (!trimmed) return "";
  if (trimmed.length <= maxLen) return trimmed;
  return `${trimmed.slice(0, maxLen)}...`;
};

type RedactedUpstreamErrorDiagnostic = Readonly<{
  error_class:
    | "ApiKeyQuotaDispatchError"
    | "CodexError"
    | "YunwuError"
    | "DOMException"
    | "TypeError"
    | "Error"
    | "unknown";
  status: number | null;
  code: string | null;
}>;

// Only codes owned by this gateway's typed errors may reach server logs. In
// particular, never log arbitrary error messages, causes, stacks, or provider
// response bodies: an upstream error may echo request content or credentials.
const REDACTED_UPSTREAM_DIAGNOSTIC_CODES = new Set<string>([
  "api_key_quota_reservation_unavailable",
  "codex_auth_missing",
  "codex_auth_invalid",
  "codex_auth_refresh_failed",
  "refresh_token_reused",
  "codex_auth_refresh_unreachable",
  "codex_upstream_unreachable",
  "gateway_timeout",
  "invalid_api_key",
  "rate_limit_exceeded",
  "server_error",
  "yunwu_api_key_missing",
  "yunwu_pricing_unavailable",
  "yunwu_pricing_invalid",
  "yunwu_status_unavailable",
  "yunwu_status_invalid",
  "yunwu_request_invalid",
  "yunwu_upstream_unreachable",
  "yunwu_logs_unavailable",
  "yunwu_logs_invalid",
]);

const redactedDiagnosticStatus = (value: unknown): number | null =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 100 && value <= 599 ? value : null;

const redactedUpstreamErrorDiagnostic = (error: unknown): RedactedUpstreamErrorDiagnostic => {
  let errorClass: RedactedUpstreamErrorDiagnostic["error_class"] = "unknown";
  let status: number | null = null;
  let code: string | null = null;

  if (error instanceof ApiKeyQuotaDispatchError) {
    errorClass = "ApiKeyQuotaDispatchError";
    status = redactedDiagnosticStatus(error.status);
    code = REDACTED_UPSTREAM_DIAGNOSTIC_CODES.has(error.code) ? error.code : null;
  } else if (error instanceof CodexError) {
    errorClass = "CodexError";
    status = redactedDiagnosticStatus(error.status);
    code = REDACTED_UPSTREAM_DIAGNOSTIC_CODES.has(error.code) ? error.code : null;
  } else if (error instanceof YunwuError) {
    errorClass = "YunwuError";
    status = redactedDiagnosticStatus(error.status);
    code = REDACTED_UPSTREAM_DIAGNOSTIC_CODES.has(error.code) ? error.code : null;
  } else if (error instanceof DOMException) {
    errorClass = "DOMException";
  } else if (error instanceof TypeError) {
    errorClass = "TypeError";
  } else if (error instanceof Error) {
    errorClass = "Error";
    // Voyage attaches a numeric HTTP status to its locally-created Error.
    status = redactedDiagnosticStatus((error as { status?: unknown }).status);
  }

  return { error_class: errorClass, status, code };
};

const logRedactedUpstreamError = (label: string, error: unknown): void => {
  console.error(label, redactedUpstreamErrorDiagnostic(error));
};

const withUpstreamProviderHeader = (response: Response, provider: string | null | undefined): Response => {
  if (!provider) return response;
  const headers = new Headers(response.headers);
  headers.set("x-uos-upstream", provider);
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
};

const toCodexErrorResponse = (error: unknown, provider?: string | null): Response => {
  let response: Response;
  if (error instanceof ApiKeyQuotaDispatchError) {
    response = openaiError(error.status, error.message, error.code, {
      type: error.errorType,
      ...(error.retryAfter ? { headers: { "Retry-After": error.retryAfter } } : {}),
    });
  } else if (error instanceof CodexError) {
    const authReauthenticationFailure = error.code === "codex_auth_invalid" ||
      error.code === "codex_auth_refresh_failed" || error.code === "refresh_token_reused";
    const options = {
      ...(error.code === "gateway_timeout" || error.code === "codex_auth_refresh_failed" ||
          error.code === "refresh_token_reused"
        ? { type: "server_error" }
        : {}),
      ...(authReauthenticationFailure ? { headers: { "x-uos-warning": CODEX_AUTH_REAUTH_WARNING } } : {}),
    };
    response = openaiError(
      error.code === "codex_auth_refresh_failed" || error.code === "refresh_token_reused" ? 503 : error.status,
      error.message,
      error.code,
      options,
    );
  } else {
    const detail = formatErrorSnippet(error);
    const message = detail ? `Codex upstream request failed: ${detail}` : "Codex upstream request failed.";
    response = openaiError(502, message, "codex_upstream_unreachable");
  }
  return withUpstreamProviderHeader(response, provider);
};

const toCerebrasErrorResponse = (error: unknown): Response => {
  let response: Response;
  if (error instanceof ApiKeyQuotaDispatchError) {
    response = openaiError(error.status, error.message, error.code, {
      type: error.errorType,
      ...(error.retryAfter ? { headers: { "Retry-After": error.retryAfter } } : {}),
    });
  } else if (error instanceof CerebrasError) {
    response = openaiError(error.status, error.message, error.code, {
      type: error.status >= 500 ? "server_error" : "invalid_request_error",
    });
  } else if (error instanceof Error && error.name === "TimeoutError") {
    response = openaiError(504, "Upstream request exceeded the gateway deadline.", "gateway_timeout", {
      type: "server_error",
    });
  } else if (error instanceof Error && error.name === "AbortError") {
    response = openaiError(499, "Request was cancelled.", "request_cancelled", { type: "server_error" });
  } else {
    // The adapter deliberately converts provider transport errors to a safe
    // CerebrasError. Keep this fallback content-free as a final guard.
    response = openaiError(502, "Upstream request could not be completed.", "cerebras_upstream_unreachable", {
      type: "server_error",
    });
  }
  return withUpstreamProviderHeader(response, "cerebras");
};

const cerebrasResponseHeaders = (
  providerRequestId: string | null,
  warning?: string,
): Record<string, string> => ({
  "x-uos-upstream": "cerebras",
  ...(providerRequestId ? { "x-uos-provider-request-id": providerRequestId } : {}),
  ...(warning ? { "x-uos-warning": warning } : {}),
});

const GPT_OSS_STREAM_DOWNGRADED_WARNING = "gpt_oss_stream_downgraded";

const streamCerebrasChatCompletion = (
  completion: Record<string, unknown>,
  includeUsage: boolean,
  headers: HeadersInit,
): Response => {
  const id = getString(completion.id) ?? `chatcmpl_${crypto.randomUUID().replace(/-/g, "")}`;
  const created = typeof completion.created === "number" ? completion.created : Math.floor(Date.now() / 1000);
  const model = getString(completion.model) ?? CEREBRAS_GPT_OSS_120B_MODEL;
  const choices = Array.isArray(completion.choices) ? completion.choices : [];
  const events: string[] = [];
  const appendEvent = (value: Record<string, unknown>): void => {
    events.push(`data: ${JSON.stringify(value)}\n\n`);
  };

  for (const [choiceIndex, value] of choices.entries()) {
    if (!isRecord(value) || Array.isArray(value)) continue;
    const index = typeof value.index === "number" ? value.index : choiceIndex;
    const message = isRecord(value.message) && !Array.isArray(value.message) ? value.message : {};
    const delta: Record<string, unknown> = { role: "assistant" };
    if (typeof message.content === "string") delta.content = message.content;

    if (Array.isArray(message.tool_calls)) {
      delta.tool_calls = message.tool_calls.flatMap((toolCall, toolCallIndex) => {
        if (!isRecord(toolCall) || Array.isArray(toolCall)) return [];
        const fn = isRecord(toolCall.function) && !Array.isArray(toolCall.function) ? toolCall.function : null;
        if (!fn) return [];
        return [{
          index: toolCallIndex,
          id: toolCall.id,
          type: "function",
          function: {
            name: fn.name,
            arguments: fn.arguments,
          },
        }];
      });
    }

    appendEvent({
      id,
      object: "chat.completion.chunk",
      created,
      model,
      choices: [{ index, delta, finish_reason: null }],
    });
    appendEvent({
      id,
      object: "chat.completion.chunk",
      created,
      model,
      choices: [{ index, delta: {}, finish_reason: getString(value.finish_reason) ?? "stop" }],
    });
  }

  if (includeUsage && completion.usage !== undefined) {
    appendEvent({ id, object: "chat.completion.chunk", created, model, choices: [], usage: completion.usage });
  }
  events.push("data: [DONE]\n\n");

  const responseHeaders = new Headers(headers);
  responseHeaders.set("Content-Type", "text/event-stream");
  responseHeaders.set("Cache-Control", "no-cache");
  return new Response(events.join(""), { status: 200, headers: responseHeaders });
};

type UpstreamErrorDetails = Readonly<{
  message: string;
  type?: string;
  code?: string;
  param?: string | null;
}>;

const readUpstreamErrorBody = async (
  upstream: Response,
  signal: AbortSignal,
): Promise<Readonly<{ text: string; complete: boolean }>> => {
  const { bytes, complete } = await readBoundedResponseBody(upstream, {
    signal,
    cancellationReason: "Upstream error body captured",
  });
  // Error normalization must never surface a partial provider payload.
  return complete ? { text: new TextDecoder().decode(bytes), complete: true } : { text: "", complete: false };
};

const getJsonString = (value: unknown, key: string): string | null => {
  if (!isRecord(value)) return null;
  const stringValue = getString(value[key]);
  return stringValue?.trim() || null;
};

const parseUpstreamErrorDetails = (text: string, statusText: string): UpstreamErrorDetails => {
  const trimmed = text.trim();
  if (trimmed) {
    try {
      const parsed = JSON.parse(trimmed) as unknown;
      if (isRecord(parsed)) {
        const error = isRecord(parsed.error) ? parsed.error : null;
        const message = getJsonString(error, "message") ?? getJsonString(parsed, "detail") ??
          getJsonString(parsed, "message");
        if (message) {
          const details: UpstreamErrorDetails = {
            message,
            type: getJsonString(error, "type") ?? getJsonString(parsed, "type") ?? undefined,
            code: getJsonString(error, "code") ?? getJsonString(parsed, "code") ?? undefined,
          };
          return error && Object.prototype.hasOwnProperty.call(error, "param")
            ? { ...details, param: getString(error.param) ?? null }
            : details;
        }
      }
    } catch {
      // Non-JSON bodies are normalized as plain text below.
    }
  }

  const snippet = trimmed ? formatErrorSnippet(trimmed) : "";
  return { message: snippet || statusText || "Upstream request failed." };
};

const upstreamStatusToErrorType = (status: number, upstreamType?: string): string => {
  if (upstreamType) return upstreamType;
  if (status >= 500) return "server_error";
  return status === 429 ? "rate_limit_error" : "invalid_request_error";
};

const toOpenAiUpstreamErrorResponse = async (
  upstream: Response,
  provider: UpstreamProvider,
  signal: AbortSignal,
): Promise<Response> => {
  const captured = await readUpstreamErrorBody(upstream, signal);
  const details = captured.complete
    ? parseUpstreamErrorDetails(captured.text, upstream.statusText)
    : { message: "Upstream returned an oversized or incomplete error response." };
  const headers: Record<string, string> = { "x-uos-upstream": provider };
  const warning = upstream.headers.get("x-uos-warning");
  const hasAuthWarning = warning?.split(",").map((value) => value.trim()).includes(CODEX_AUTH_REAUTH_WARNING) === true;
  if (warning) headers["x-uos-warning"] = warning;
  const retryAfter = upstream.headers.get("Retry-After");
  if (retryAfter) headers["Retry-After"] = retryAfter;
  const message = hasAuthWarning && !captured.text.includes(CODEX_AUTH_REAUTH_MESSAGE)
    ? `${details.message} ${CODEX_AUTH_REAUTH_MESSAGE}`
    : details.message;
  const options: { type?: string; param?: string | null; headers: HeadersInit } = {
    type: hasAuthWarning && upstream.status >= 500
      ? "server_error"
      : upstreamStatusToErrorType(upstream.status, details.type),
    headers,
  };
  if (Object.prototype.hasOwnProperty.call(details, "param")) options.param = details.param ?? null;
  return openaiError(upstream.status, message, details.code ?? "upstream_error", options);
};

const cancelResponseBody = (response: Response): void => {
  try {
    const cancellation = response.body?.cancel();
    if (cancellation) void cancellation.catch(() => {});
  } catch {
    // The response may already be closed.
  }
};

// Cerebras responses can contain provider-specific diagnostics. Preserve the
// HTTP semantics clients need, but never reflect that body through the gateway
// (and do not leave its stream open while returning the normalized error).
const toCerebrasUpstreamErrorResponse = (upstream: Response): Response => {
  cancelResponseBody(upstream);
  const headers = cerebrasResponseHeaders(getCerebrasProviderRequestId(upstream));
  const retryAfter = upstream.headers.get("Retry-After");
  if (retryAfter) headers["Retry-After"] = retryAfter;
  if (upstream.status === 429) {
    for (const header of CEREBRAS_RATE_LIMIT_HEADERS) {
      const value = upstream.headers.get(header);
      if (value !== null) headers[header] = value;
    }
  }
  return openaiError(
    upstream.status,
    "Cerebras upstream returned an error.",
    "cerebras_upstream_error",
    {
      type: upstream.status === 408 ? "server_error" : upstreamStatusToErrorType(upstream.status),
      headers,
    },
  );
};

const warnPaidFallbackBookkeepingFailure = (operation: string, error: unknown): void => {
  console.warn(
    `[ai.ubq.fi] Paid fallback ${operation} failed; leaving the reservation pending:`,
    error instanceof Error ? error.message : String(error),
  );
};

const logYunwuSelected = (requestId: string, reason: InferenceFallbackReason): void => {
  try {
    console.info("[ai.ubq.fi] yunwu_selected", JSON.stringify({ request_id: requestId, reason }));
  } catch {
    // Routing telemetry must never alter provider selection.
  }
};

const bestEffortPaidFallbackBookkeeping = async (
  operation: string,
  run: () => Promise<unknown>,
): Promise<void> => {
  try {
    await run();
  } catch (error) {
    warnPaidFallbackBookkeepingFailure(operation, error);
  }
};

type YunwuTransportLifecycle = Readonly<{
  terminal: (eventType: string) => void;
  ambiguous: () => void;
  cancelled: () => void;
}>;

const createYunwuTransportLifecycle = (
  reservation: PaidFallbackReservation | null,
): YunwuTransportLifecycle => {
  let recorded = false;
  const schedule = (
    operation: string,
    run: (reservation: PaidFallbackReservation) => Promise<void>,
  ): void => {
    if (!reservation || recorded) return;
    recorded = true;
    void bestEffortPaidFallbackBookkeeping(operation, () => run(reservation));
  };
  return {
    terminal: (eventType) => {
      const terminalState = eventType === "response.completed"
        ? "completed"
        : eventType === "response.failed" || eventType === "error"
        ? "failed"
        : eventType === "response.incomplete"
        ? "incomplete"
        : null;
      if (!terminalState) return;
      schedule(
        "terminal reconciliation",
        async (activeReservation) => {
          await Promise.all([
            recordYunwuTerminal(activeReservation, terminalState),
            recordYunwuProviderHealth(
              terminalState === "completed" ? "success" : "upstream_error",
              terminalState === "completed" ? 200 : null,
            ),
          ]);
        },
      );
    },
    ambiguous: () => {
      schedule(
        "ambiguous failure recording",
        async (activeReservation) => {
          await Promise.all([
            recordYunwuAmbiguousFailure(activeReservation),
            recordYunwuProviderHealth("upstream_error", null),
          ]);
        },
      );
    },
    cancelled: () =>
      schedule(
        "dispatched cancellation recording",
        (activeReservation) => recordYunwuTerminal(activeReservation, "cancelled"),
      ),
  };
};

const fetchResponsesWithPaidFallback = async (
  body: Record<string, unknown>,
  options: Readonly<{
    model: string;
    route: "chat.completions" | "responses";
    stream: boolean;
    reasoning: string | null;
    usageContext?: UsageContext;
    clientVersion?: string | null;
    signal?: AbortSignal;
  }>,
): Promise<RoutedResponsesUpstream> => {
  const telemetry = options.usageContext?.responseTelemetry;
  if (telemetry) telemetry.provider = "chatgpt_codex";
  recordAttemptedProvider(options.usageContext, "chatgpt_codex");
  let primary: Response;
  try {
    primary = await fetchCodexResponses(body, {
      clientVersion: options.clientVersion,
      signal: options.signal,
      requestId: options.usageContext?.requestId,
      // Keep terminal telemetry bounded: only the first real Codex transport
      // attempt contributes dispatch/header timings, even when routing retries.
      timing: {
        onDispatch: () => recordFirstCodexDispatch(options.usageContext),
        onHeaders: () => recordFirstCodexHeaders(options.usageContext),
      },
      beforeDispatch: () => options.usageContext?.beforeProviderDispatch?.("chatgpt_codex") ?? Promise.resolve(),
      bankedReset: codexBankedResetOptionsForTest ?? undefined,
    });
  } catch (error) {
    if (!(error instanceof CodexError) || error.status !== 401) throw error;
    primary = openaiError(error.status, error.message, error.code);
  }
  const primaryStatus = primary.status;
  const authReauthenticationPrimary = primaryStatus === 401 &&
    responseWarnings(primary).includes(CODEX_AUTH_REAUTH_WARNING);
  const primaryWarnings = Array.from(
    new Set([
      ...responseWarnings(primary),
      ...(getCodexAuthWarning(primary) ? [getCodexAuthWarning(primary)!] : []),
    ]),
  );
  const preservePrimaryWarnings = (response: Response): Response => withUosWarning(response, primaryWarnings);
  if (telemetry) telemetry.accountSlot = getCodexResponseSlot(primary);
  const routingError = getCodexRoutingError(primary);
  const gatewayResponse = routingError === CODEX_QUOTA_BLOCKED_ERROR_CODE ||
    routingError === CODEX_UPSTREAM_DEGRADED_ERROR_CODE;
  if (authReauthenticationPrimary) {
    primary = new Response(primary.body, {
      status: 503,
      statusText: primary.statusText,
      headers: primary.headers,
    });
  }
  const keyId = options.usageContext?.keyId;
  const requestId = options.usageContext?.requestId;
  const createdAtMs = options.usageContext?.startedAtMs;
  const fallbackReason: InferenceFallbackReason | null = primaryStatus === 401
    ? "primary_401"
    : primaryStatus === 403
    ? "primary_403"
    : primaryStatus === 429
    ? "primary_429"
    : null;
  if (telemetry) telemetry.fallbackReason = fallbackReason;
  if (
    !fallbackReason ||
    options.usageContext?.paidFallbackEnabled === false ||
    !keyId ||
    !requestId ||
    createdAtMs === undefined
  ) {
    return {
      response: primary,
      provider: "chatgpt_codex",
      paidFallback: null,
      gatewayResponse,
      fallbackReason,
    };
  }
  if (options.signal?.aborted) {
    if (primary) cancelResponseBody(primary);
    throw options.signal.reason instanceof Error
      ? options.signal.reason
      : new DOMException("The request was aborted.", "AbortError");
  }

  const reservationInput = {
    keyId,
    requestId,
    createdAtMs,
    model: options.model,
    route: options.route,
    path: options.route === "responses" ? "/v1/responses" : "/v1/chat/completions",
    stream: options.stream,
    reasoning: options.reasoning,
    reason: fallbackReason,
  } as const;
  let decision: Awaited<ReturnType<typeof reservePaidFallback>>;
  try {
    decision = await reservePaidFallback(reservationInput);
  } catch (error) {
    warnPaidFallbackBookkeepingFailure("admission", error);
    return {
      response: primary,
      provider: "chatgpt_codex",
      paidFallback: null,
      gatewayResponse,
      fallbackReason: reservationInput.reason,
    };
  }
  if (decision.kind === "skip") {
    return {
      response: primary,
      provider: "chatgpt_codex",
      paidFallback: null,
      gatewayResponse,
      fallbackReason: reservationInput.reason,
    };
  }
  if (decision.kind === "blocked") {
    return {
      response: primary,
      provider: "chatgpt_codex",
      paidFallback: null,
      gatewayResponse,
      fallbackReason: reservationInput.reason,
    };
  }

  if (options.signal?.aborted) {
    cancelResponseBody(primary);
    await bestEffortPaidFallbackBookkeeping(
      "prefetch cancellation recording",
      () => recordYunwuPrefetchCancellation(decision.reservation),
    );
    throw options.signal.reason instanceof Error
      ? options.signal.reason
      : new DOMException("The request was aborted.", "AbortError");
  }
  cancelResponseBody(primary);
  if (options.signal?.aborted) {
    await bestEffortPaidFallbackBookkeeping(
      "prefetch cancellation recording",
      () => recordYunwuPrefetchCancellation(decision.reservation),
    );
    throw options.signal.reason instanceof Error
      ? options.signal.reason
      : new DOMException("The request was aborted.", "AbortError");
  }
  if (telemetry) {
    telemetry.provider = "yunwu";
    telemetry.accountSlot = null;
    telemetry.quotaUsedPercent = decision.reservation.quota_used_percent;
  }
  recordAttemptedProvider(options.usageContext, "yunwu");
  logYunwuSelected(requestId, fallbackReason);
  let result: Awaited<ReturnType<typeof fetchYunwuResponses>>;
  try {
    result = await fetchYunwuResponses(body, {
      signal: options.signal,
      beforeDispatch: () => options.usageContext?.beforeProviderDispatch?.("yunwu") ?? Promise.resolve(),
    });
    if (result.response.status === 401 || result.response.status === 403) {
      await recordYunwuProviderHealth("auth_invalid", result.response.status);
    } else if (result.response.status === 429) {
      await recordYunwuProviderHealth("quota_exhausted", result.response.status);
    } else if (result.response.status >= 500) {
      await recordYunwuProviderHealth("upstream_error", result.response.status);
    } else if (!result.response.ok) {
      await recordYunwuProviderHealth("reachable", result.response.status);
    }
  } catch (error) {
    if (error instanceof ApiKeyQuotaDispatchError) {
      // Paid fallback writes a durable dispatch intent before Yunwu transport.
      // If the API-key quota CAS rejects that transport, it is still known not
      // to have started and must be released rather than reconciled as billed.
      await bestEffortPaidFallbackBookkeeping(
        "pre-dispatch quota cancellation recording",
        () => recordYunwuUndispatchedCancellation(decision.reservation),
      );
      throw error;
    }
    const yunwuStatus = error instanceof YunwuError ? error.status : null;
    await recordYunwuProviderHealth(
      yunwuStatus === 401 || yunwuStatus === 403
        ? "auth_invalid"
        : yunwuStatus === 429
        ? "quota_exhausted"
        : "upstream_error",
      yunwuStatus,
    );
    await bestEffortPaidFallbackBookkeeping(
      "ambiguous failure recording",
      () => recordYunwuAmbiguousFailure(decision.reservation),
    );
    const abortReason = options.signal?.reason;
    if (
      (options.signal?.aborted && abortReason instanceof Error && abortReason.name === "TimeoutError") ||
      (error instanceof Error && error.name === "TimeoutError")
    ) {
      return {
        response: preservePrimaryWarnings(openaiError(
          504,
          "YunWu upstream exceeded the gateway deadline before response headers were received.",
          "gateway_timeout",
          {
            type: "server_error",
            headers: { "x-uos-upstream": "yunwu" },
          },
        )),
        provider: "yunwu",
        paidFallback: decision.reservation,
        gatewayResponse: true,
        fallbackReason: reservationInput.reason,
      };
    }
    if (error instanceof YunwuError) {
      return {
        response: preservePrimaryWarnings(openaiError(error.status, error.message, error.code, {
          type: "server_error",
          headers: { "x-uos-upstream": "yunwu" },
        })),
        provider: "yunwu",
        paidFallback: decision.reservation,
        gatewayResponse: true,
        fallbackReason: reservationInput.reason,
      };
    }
    return {
      response: preservePrimaryWarnings(openaiError(
        502,
        "YunWu upstream request failed before response headers were received.",
        "upstream_error",
        {
          type: "server_error",
          headers: { "x-uos-upstream": "yunwu" },
        },
      )),
      provider: "yunwu",
      paidFallback: decision.reservation,
      gatewayResponse: true,
      fallbackReason: reservationInput.reason,
    };
  }
  await bestEffortPaidFallbackBookkeeping(
    "upstream response recording",
    () => recordYunwuUpstreamResponse(decision.reservation, result.response, result.request_id),
  );
  return {
    response: preservePrimaryWarnings(result.response),
    provider: "yunwu",
    paidFallback: decision.reservation,
    gatewayResponse: false,
    fallbackReason: reservationInput.reason,
  };
};

type CodexModelReasoning = Readonly<{
  levels: ReasoningEffort[];
  defaultLevel: ReasoningEffort | null;
  wireEfforts: ReadonlyMap<ReasoningEffort, ReasoningEffort>;
}>;

type CodexModelMetadata = Readonly<{
  snapshot: CodexModelsSnapshot | null;
  record: Record<string, unknown> | null;
  reasoning: CodexModelReasoning;
}>;

const modelIdFromSnapshotRecord = (model: Record<string, unknown>): string | null => {
  const id = getString(model.slug) ?? getString(model.id) ?? getString(model.model) ?? getString(model.name);
  return id?.trim() || null;
};

const findSnapshotModelRecord = (
  snapshot: CodexModelsSnapshot | null,
  model: string,
): Record<string, unknown> | null => {
  const target = model.trim();
  if (!target) return null;
  if (!snapshot || !Array.isArray(snapshot.models)) return null;
  return snapshot.models.find((entry) => {
    if (!isRecord(entry)) return false;
    return modelIdFromSnapshotRecord(entry) === target;
  }) ?? null;
};

const normalizeSnapshotReasoningEffort = (value: unknown): ReasoningEffort | null =>
  value === null ? "none" : normalizeReasoningEffort(value);

const extractSnapshotReasoningLevels = (model: Record<string, unknown> | null): ReasoningEffort[] => {
  const raw = Array.isArray(model?.supported_reasoning_levels) ? model.supported_reasoning_levels : [];
  const levels = raw
    .map((entry) => {
      if (entry === null || typeof entry === "string") return normalizeSnapshotReasoningEffort(entry);
      if (isRecord(entry)) return normalizeSnapshotReasoningEffort(entry.effort);
      return null;
    })
    .filter((entry): entry is ReasoningEffort => Boolean(entry));
  return Array.from(new Set(levels));
};

const extractSnapshotReasoningEffortWireMap = (
  model: Record<string, unknown> | null,
): ReadonlyMap<ReasoningEffort, ReasoningEffort> => {
  const raw = model?.reasoning_effort_wire_map;
  if (!isRecord(raw)) return new Map();
  const entries = Object.entries(raw)
    .map(([effort, wireEffort]) => [normalizeReasoningEffort(effort), normalizeReasoningEffort(wireEffort)] as const)
    .filter((entry): entry is readonly [ReasoningEffort, ReasoningEffort] => entry[0] !== null && entry[1] !== null);
  return new Map(entries);
};

const getCodexModelReasoning = (record: Record<string, unknown> | null): CodexModelReasoning => {
  const defaultLevel = normalizeSnapshotReasoningEffort(record?.default_reasoning_level);
  const catalogLevels = extractSnapshotReasoningLevels(record);
  const levels = catalogLevels.includes("none") ? catalogLevels : ["none", ...catalogLevels];
  return {
    levels: defaultLevel && !levels.includes(defaultLevel) ? [...levels, defaultLevel] : levels,
    defaultLevel,
    wireEfforts: extractSnapshotReasoningEffortWireMap(record),
  };
};

const getCodexModelMetadata = async (model: string): Promise<CodexModelMetadata> => {
  const snapshot = await loadCodexModelsSnapshot();
  const record = findSnapshotModelRecord(snapshot, model);
  return { snapshot, record, reasoning: getCodexModelReasoning(record) };
};

const validateCodexModelAvailable = (model: string, metadata: CodexModelMetadata): Response | null => {
  if (!metadata.snapshot?.models?.length || metadata.record) return null;
  return openaiError(
    404,
    `The model '${model}' does not exist or is not available through this gateway. Use /v1/models for supported models.`,
    "model_not_found",
    { param: "model" },
  );
};

const promptCacheControlParam = (rawRecord: Record<string, unknown>): string | null => {
  for (const key of ["prompt_cache_key", "prompt_cache_options", "prompt_cache_retention"] as const) {
    if (Object.prototype.hasOwnProperty.call(rawRecord, key)) return key;
  }
  return null;
};

const hasExplicitPromptCacheBreakpoint = (value: unknown): boolean =>
  isRecord(value) && !Array.isArray(value) && value.mode === "explicit";

type ExplicitPromptCacheBreakpoint = Readonly<{
  param: string;
  blockType: string | null;
}>;

const findExplicitPromptCacheBreakpoints = (
  rawInput: unknown,
  inputParam: "input" | "messages",
): ExplicitPromptCacheBreakpoint[] => {
  if (!Array.isArray(rawInput)) return [];

  const breakpoints: ExplicitPromptCacheBreakpoint[] = [];

  const collect = (value: Record<string, unknown>, param: string): void => {
    if (hasExplicitPromptCacheBreakpoint(value.prompt_cache_breakpoint)) {
      breakpoints.push({ param, blockType: getString(value.type) });
    }
  };

  for (const [index, item] of rawInput.entries()) {
    if (!isRecord(item) || Array.isArray(item)) continue;
    const itemParam = `${inputParam}[${index}]`;
    if (inputParam === "input") {
      collect(item, `${itemParam}.prompt_cache_breakpoint`);
    }
    if (Array.isArray(item.content)) {
      for (const [contentIndex, contentItem] of item.content.entries()) {
        if (!isRecord(contentItem) || Array.isArray(contentItem)) continue;
        collect(contentItem, `${itemParam}.content[${contentIndex}].prompt_cache_breakpoint`);
      }
    }
    if (inputParam !== "input" || item.type !== "function_call_output" || !Array.isArray(item.output)) continue;
    for (const [outputIndex, outputItem] of item.output.entries()) {
      if (!isRecord(outputItem) || Array.isArray(outputItem)) continue;
      collect(outputItem, `${itemParam}.output[${outputIndex}].prompt_cache_breakpoint`);
    }
  }
  return breakpoints;
};

const activePromptCacheControls = (metadata: CodexModelMetadata): PromptCacheControls | null => {
  const capabilities = normalizePromptCacheCapabilities(metadata.record?.prompt_cache);
  if (capabilities === null || capabilities === false) return null;
  return capabilities.providers.find((provider) => provider.id === CODEX_CHATGPT_PROMPT_CACHE_PROVIDER)?.controls ??
    null;
};

type RequestedPromptCacheMode = Readonly<{
  value: "implicit" | "explicit";
  param: "prompt_cache_options" | "prompt_cache_options.mode";
}>;

const requestedPromptCacheMode = (rawRecord: Record<string, unknown>): RequestedPromptCacheMode | null => {
  if (!Object.prototype.hasOwnProperty.call(rawRecord, "prompt_cache_options")) return null;
  const options = rawRecord.prompt_cache_options;
  if (!isRecord(options) || Array.isArray(options)) return null;
  if (options.mode === "explicit") return { value: "explicit", param: "prompt_cache_options.mode" };
  return {
    value: "implicit",
    param: options.mode === "implicit" ? "prompt_cache_options.mode" : "prompt_cache_options",
  };
};

const requestedPromptCacheTtl = (rawRecord: Record<string, unknown>): string | null => {
  const options = rawRecord.prompt_cache_options;
  if (!isRecord(options) || Array.isArray(options)) return null;
  return getString(options.ttl);
};

const knownUnsupportedPromptCacheUseError = (model: string, param: string): Response =>
  openaiError(
    400,
    `Prompt cache control '${param}' is not supported for model '${model}'.`,
    "invalid_request_error",
    { param },
  );

const validateKnownUnsupportedPromptCacheUse = (
  model: string,
  metadata: CodexModelMetadata,
  rawRecord: Record<string, unknown>,
  input: readonly ResponseInputItem[],
  inputParam: "input" | "messages",
): Response | null => {
  const breakpoints = countExplicitPromptCacheBreakpoints(input) > 0
    ? findExplicitPromptCacheBreakpoints(rawRecord[inputParam], inputParam)
    : [];

  if (metadata.record?.prompt_cache === false) {
    const param = promptCacheControlParam(rawRecord) ?? breakpoints[0]?.param;
    if (!param) return null;
    return openaiError(
      400,
      `Prompt caching is not supported for model '${model}'.`,
      "invalid_request_error",
      { param },
    );
  }

  // A missing capability envelope, another provider's record, or an omitted
  // control field is unknown—not an unsupported upstream feature. Preserve
  // standard OpenAI controls in each of those cases for forward compatibility.
  const controls = activePromptCacheControls(metadata);
  if (!controls) return null;

  if (Object.prototype.hasOwnProperty.call(rawRecord, "prompt_cache_key") && controls.key === false) {
    return knownUnsupportedPromptCacheUseError(model, "prompt_cache_key");
  }

  const mode = requestedPromptCacheMode(rawRecord);
  const modeIsKnownUnsupported = (value: "implicit" | "explicit"): boolean =>
    controls.modes !== undefined && !controls.modes.includes(value);
  if (mode?.value === "implicit" && (controls.implicit === false || modeIsKnownUnsupported("implicit"))) {
    return knownUnsupportedPromptCacheUseError(model, mode.param);
  }

  const ttl = requestedPromptCacheTtl(rawRecord);
  if (ttl !== null && controls.ttls !== undefined && !controls.ttls.includes(ttl)) {
    return knownUnsupportedPromptCacheUseError(model, "prompt_cache_options.ttl");
  }

  const retention = getString(rawRecord.prompt_cache_retention);
  if (
    retention !== null && controls.legacy_retentions !== undefined &&
    !controls.legacy_retentions.includes(retention)
  ) {
    return knownUnsupportedPromptCacheUseError(model, "prompt_cache_retention");
  }

  for (const breakpoint of breakpoints) {
    if (controls.explicit_breakpoints === false || modeIsKnownUnsupported("explicit")) {
      return knownUnsupportedPromptCacheUseError(model, breakpoint.param);
    }
    const endpoint = inputParam === "input" ? "responses" : "chat_completions";
    const supportedBlockTypes = controls.breakpoint_block_types?.[endpoint];
    if (
      supportedBlockTypes !== undefined &&
      (breakpoint.blockType === null || !supportedBlockTypes.includes(breakpoint.blockType))
    ) {
      return knownUnsupportedPromptCacheUseError(model, breakpoint.param);
    }
  }

  if (mode?.value === "explicit" && (controls.explicit_breakpoints === false || modeIsKnownUnsupported("explicit"))) {
    return knownUnsupportedPromptCacheUseError(model, mode.param);
  }

  return null;
};

const resolveDefaultReasoningLabel = (
  _modelReasoning: CodexModelReasoning,
  defaultEffort: ReasoningEffort,
): ReasoningEffort => defaultEffort;

const resolveReasoningLabelFromEffort = (
  effort: ReasoningEffort | undefined,
  defaultLabel: ReasoningEffort,
): ReasoningEffort => {
  if (effort === undefined) return defaultLabel;
  return effort;
};

const resolveReasoningLabelFromParam = (
  reasoning: Record<string, unknown> | undefined,
  defaultLabel: ReasoningEffort,
): ReasoningEffort => {
  if (reasoning === undefined) return defaultLabel;
  if (!isRecord(reasoning)) return defaultLabel;
  if ("effort" in reasoning) {
    const effort = normalizeReasoningEffort(reasoning.effort);
    if (effort) return effort;
  }
  return defaultLabel;
};

const extractReasoningParamEffort = (
  reasoning: Record<string, unknown> | undefined,
): ReasoningEffort | undefined => {
  if (reasoning === undefined) return undefined;
  if (!Object.prototype.hasOwnProperty.call(reasoning, "effort")) return undefined;
  return normalizeReasoningEffort(reasoning.effort) ?? undefined;
};

const reasoningEffortForCodexRequest = (
  effort: ReasoningEffort,
  modelReasoning: CodexModelReasoning,
): ReasoningEffort => {
  if (effort === "none") return "none";
  // Codex CLI's advanced `ultra` preset is client-side orchestration and
  // always uses `max` on the upstream wire, even for an older catalog that
  // has not yet published its wire map.
  if (effort === "ultra") return "max";
  return modelReasoning.wireEfforts.get(effort) ?? effort;
};

const normalizeReasoningParamForCodex = (
  reasoning: Record<string, unknown> | undefined,
  modelReasoning: CodexModelReasoning,
): Record<string, unknown> | undefined => {
  if (reasoning === undefined) return undefined;
  const effort = extractReasoningParamEffort(reasoning);
  if (effort === undefined) return reasoning;
  return { ...reasoning, effort: reasoningEffortForCodexRequest(effort, modelReasoning) };
};

const UOS_WARNING_HEADER = "x-uos-warning";
const TEMPERATURE_IGNORED_WARNING = "temperature_ignored";
const MAX_OUTPUT_TOKENS_IGNORED_WARNING = "max_output_tokens_ignored";

const WARNING_KEY_MAP = new Map<string, string>([
  ["temperature", TEMPERATURE_IGNORED_WARNING],
  ["max_tokens", MAX_OUTPUT_TOKENS_IGNORED_WARNING],
  ["max_completion_tokens", MAX_OUTPUT_TOKENS_IGNORED_WARNING],
  ["max_output_tokens", MAX_OUTPUT_TOKENS_IGNORED_WARNING],
]);

const buildIgnoredWarnings = (record: Record<string, unknown>, usedKeys: ReadonlySet<string>): string[] => {
  const warnings = new Set<string>();
  for (const key of Object.keys(record)) {
    if (usedKeys.has(key)) continue;
    const mapped = WARNING_KEY_MAP.get(key) ?? `${key}_ignored`;
    warnings.add(mapped);
  }
  return Array.from(warnings);
};

const responseWarnings = (response: Response): string[] =>
  (response.headers.get(UOS_WARNING_HEADER) ?? "")
    .split(",")
    .map((value) => value.trim())
    .filter(Boolean);

type PassthroughToolSchemaKey =
  | "tools"
  | "tool_choice"
  | "parallel_tool_calls"
  | "prompt_cache_key"
  | "prompt_cache_options"
  | "prompt_cache_retention"
  | "text"
  | "include"
  | "context_management";

const normalizeCodexToolChoice = (value: unknown): unknown => {
  if (!isRecord(value)) return value;
  if (getString(value.type) !== "function") return value;

  const normalized: Record<string, unknown> = { ...value };
  const topLevelName = getString(normalized.name);
  const fn = isRecord(value.function) ? value.function : null;
  if (!fn && !topLevelName) return value;

  if (!topLevelName) {
    const functionName = getString(fn?.name);
    if (!functionName) return value;
    normalized.name = functionName;
  }

  delete normalized.function;
  return normalized;
};

const normalizeCodexTools = (value: unknown): unknown => {
  if (!Array.isArray(value)) return value;
  return value.map((tool) => {
    if (!isRecord(tool)) return tool;
    if (getString(tool.type) !== "function") return tool;
    const nestedFunction = isRecord(tool.function) ? tool.function : null;
    if (!nestedFunction) return tool;

    const normalized: Record<string, unknown> = { ...tool };
    const topLevelName = getString(normalized.name);
    const nestedName = getString(nestedFunction.name);
    if (!topLevelName && !nestedName) return tool;

    if (!topLevelName) {
      normalized.name = nestedName;
    }
    for (const [key, nestedValue] of Object.entries(nestedFunction)) {
      if (key in normalized) continue;
      normalized[key] = nestedValue;
    }
    delete normalized.function;
    return normalized;
  });
};

const normalizePassthroughForCodex = (key: PassthroughToolSchemaKey, value: unknown): unknown => {
  if (key === "tools") return normalizeCodexTools(value);
  if (key === "tool_choice") return normalizeCodexToolChoice(value);
  return value;
};

const applyPassthroughToCodexRequest = (
  codexBody: Record<string, unknown>,
  rawRecord: Record<string, unknown>,
  keys: readonly PassthroughToolSchemaKey[],
): void => {
  for (const key of keys) {
    if (Object.prototype.hasOwnProperty.call(rawRecord, key)) {
      codexBody[key] = normalizePassthroughForCodex(key, rawRecord[key]);
    }
  }
};

const withUosWarning = (response: Response, warnings: string[]): Response => {
  const merged = Array.from(new Set([...responseWarnings(response), ...warnings]));
  if (!merged.length) return response;
  const headers = new Headers(response.headers);
  headers.set(UOS_WARNING_HEADER, merged.join(", "));
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
};
const parseReasoningEffortField = (
  value: unknown,
  fieldName: string,
): { ok: true; value: ReasoningEffort | undefined } | { ok: false; message: string } => {
  if (value === undefined || value === null) return { ok: true, value: undefined };
  if (typeof value !== "string") {
    return { ok: false, message: `${fieldName} must be a string` };
  }
  const normalized = normalizeReasoningEffort(value);
  if (!normalized) return { ok: false, message: `${fieldName} must be a non-empty string` };
  return { ok: true, value: normalized };
};

const parseReasoningParam = (
  value: unknown,
): { ok: true; value: Record<string, unknown> | undefined } | { ok: false; message: string } => {
  if (value === undefined || value === null) return { ok: true, value: undefined };
  if (!isRecord(value) || Array.isArray(value)) return { ok: false, message: "reasoning must be an object" };
  const normalized = { ...value };
  if ("effort" in normalized) {
    const effort = parseReasoningEffortField(normalized.effort, "reasoning.effort");
    if (!effort.ok) return effort;
    if (effort.value === undefined) delete normalized.effort;
    else normalized.effort = effort.value;
  }
  if ("summary" in normalized) {
    const summary = normalized.summary;
    if (summary === undefined || summary === null) delete normalized.summary;
    else if (typeof summary !== "string") {
      return { ok: false, message: "reasoning.summary must be a string" };
    }
  }
  if ("generate_summary" in normalized) {
    const generateSummary = normalized.generate_summary;
    if (generateSummary === undefined || generateSummary === null) delete normalized.generate_summary;
    else if (typeof generateSummary !== "string") {
      return { ok: false, message: "reasoning.generate_summary must be a string" };
    }
  }

  return { ok: true, value: Object.keys(normalized).length ? normalized : undefined };
};

const parseStreamField = (
  value: unknown,
): { ok: true; value: boolean } | { ok: false; message: string } => {
  if (value === undefined || value === false) return { ok: true, value: false };
  if (value === true) return { ok: true, value: true };
  return { ok: false, message: "stream must be a boolean" };
};

const parseChatStreamOptions = (
  value: unknown,
): { ok: true; includeUsage: boolean } | { ok: false; message: string } => {
  if (value === undefined) return { ok: true, includeUsage: false };
  if (!isRecord(value) || Array.isArray(value)) {
    return { ok: false, message: "stream_options must be an object" };
  }
  if (value.include_usage !== undefined && typeof value.include_usage !== "boolean") {
    return { ok: false, message: "stream_options.include_usage must be a boolean" };
  }
  return { ok: true, includeUsage: value.include_usage === true };
};

const parseMaxCompletionTokensField = (
  value: unknown,
): { ok: true; value: number | undefined } | { ok: false; message: string } => {
  if (value === undefined) return { ok: true, value: undefined };
  if (typeof value !== "number" || !Number.isSafeInteger(value) || value <= 0) {
    return { ok: false, message: "max_completion_tokens must be a positive integer" };
  }
  return { ok: true, value };
};

const CHAT_COMPLETIONS_ALLOWED_KEYS = new Set(CHAT_COMPLETIONS_REQUEST_KEYS);
const RESPONSES_ALLOWED_KEYS = new Set(RESPONSES_REQUEST_KEYS);
const CODEX_RESPONSES_EXTENSION_KEYS = new Set(["client_metadata"]);

const findUnknownKey = (
  record: Record<string, unknown>,
  allowed: ReadonlySet<string>,
  extensions?: ReadonlySet<string>,
): string | null => {
  for (const key of Object.keys(record)) {
    if (!allowed.has(key) && !extensions?.has(key)) return key;
  }
  return null;
};

type EmbeddingsEncodingFormat = "float" | "base64";
type VoyageEmbeddingsInputType = "query" | "document";
type VoyageEmbeddingsDimension = 256 | 512 | 1024 | 2048;
type VoyageEmbeddingsOutputDtype = "float";

type ResolvedEmbeddingsProfile = Readonly<{
  upstream: "voyage";
  upstream_model: "voyage-4-large";
  input_type: VoyageEmbeddingsInputType;
  dimensions: VoyageEmbeddingsDimension;
  output_dtype: VoyageEmbeddingsOutputDtype;
  encoding_format: EmbeddingsEncodingFormat;
  truncation: boolean;
  cache_profile_key: string;
}>;

type ParsedEmbeddingsRequest = Readonly<{
  model: string;
  inputs: string[];
  total_chars: number;
  profile: ResolvedEmbeddingsProfile;
}>;

type EmbeddingsParseResult =
  | Readonly<{ ok: true; value: ParsedEmbeddingsRequest }>
  | Readonly<{ ok: false; response: Response }>;

type VoyageRateLimitState = Readonly<{
  window_start_ms: number;
  requests: number;
  tokens: number;
}>;

const EMBEDDINGS_MAX_INPUTS_PER_REQUEST = 128;
const EMBEDDINGS_MAX_CHARS_PER_INPUT = 20_000;
const EMBEDDINGS_MAX_TOTAL_CHARS = 100_000;
const EMBEDDINGS_TIMEOUT_MS = 20_000;
// KV cache is best-effort and quota-driven: we cache embeddings until KV rejects
// writes (storage/quota), then evict the oldest entries (FIFO index) and retry.
// We do not track "last read" to keep writes minimal.
const EMBEDDINGS_CACHE_EVICT_BATCH = 512;
const EMBEDDINGS_CACHE_EVICT_MAX_BATCH = 8192;
const EMBEDDINGS_CACHE_QUOTA_MAX_RETRIES = 4;
const EMBEDDINGS_JOB_TTL_MS = 24 * 60 * 60_000;
const EMBEDDINGS_JOB_LOCK_MS = 30_000;
const EMBEDDINGS_RETRYABLE_UPSTREAM_STATUSES = new Set([429, 500, 502, 503, 504]);
const EMBEDDINGS_IDEMPOTENCY_LEASE_MS = 60_000;
const EMBEDDINGS_IDEMPOTENCY_RESPONSE_CHUNK_CHARS = 48_000;
// 128 inputs x 2,048 finite JSON numbers fit comfortably below this cap.
const EMBEDDINGS_IDEMPOTENCY_MAX_RESPONSE_CHUNKS = 256;
const EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS = 7 * 24 * 60 * 60_000;
// Response chunks are published before their ledger record. Keep them for one
// extra day so every published ledger expires before the chunks it references;
// unpublished/orphaned generations are reclaimed by the same TTL.
const EMBEDDINGS_IDEMPOTENCY_RESPONSE_TTL_MS = EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS + 24 * 60 * 60_000;
const EMBEDDINGS_IDEMPOTENCY_MAX_KEY_CHARS = 255;

const VOYAGE_EMBEDDINGS_URL = "https://api.voyageai.com/v1/embeddings";
const VOYAGE_EMBEDDINGS_MODEL = "voyage-4-large";
const VOYAGE_DEFAULT_DIMENSIONS: VoyageEmbeddingsDimension = 1024;
const VOYAGE_OUTPUT_DTYPE: VoyageEmbeddingsOutputDtype = "float";
const VOYAGE_SUPPORTED_DIMENSIONS = new Set<number>([256, 512, 1024, 2048]);
const UOS_SYNC_EMBEDDINGS_ALLOWED_KEYS = new Set([
  "dimensions",
  "encoding_format",
  "input",
  "input_type",
  "model",
  "truncation",
  "user",
]);
// Jobs deliberately retain the original Voyage-only profile. In particular,
// they require an explicit retrieval input type and only persist float vectors.
const UOS_EMBEDDINGS_JOB_ALLOWED_KEYS = new Set([
  "dimensions",
  "encoding_format",
  "input",
  "input_type",
  "model",
  "truncation",
]);
// Voyage free-tier throttles are tiny; we enforce conservative defaults to avoid 429s.
const VOYAGE_RATE_LIMIT_RPM = 3;
const VOYAGE_RATE_LIMIT_TPM = 10_000;
const VOYAGE_RATE_LIMIT_KEY: Deno.KvKey = ["embeddings", "v1", "rate", "voyage"];
const VOYAGE_API_KEY_KV_KEY: Deno.KvKey = ["uos_ai", "voyage_api_key"];

type EmbeddingsIdempotencyState = "reserved" | "dispatched" | "succeeded" | "indeterminate";

type EmbeddingsIdempotencyRecord = Readonly<{
  v: 1;
  fingerprint: string;
  state: EmbeddingsIdempotencyState;
  owner_request_id: string | null;
  created_at_ms: number;
  updated_at_ms: number;
  lease_until_ms: number | null;
  response_status: number | null;
  response_content_type: string | null;
  response_generation: string | null;
  response_chunk_count: number | null;
  response_sha256: string | null;
}>;

type EmbeddingsIdempotencyLease = Readonly<{
  kv: Deno.Kv;
  key: Deno.KvKey;
  responseKeyPrefix: Deno.KvKey;
  fingerprint: string;
  ownerRequestId: string;
}>;

type EmbeddingsIdempotencyAcquireResult =
  | Readonly<{ kind: "acquired"; lease: EmbeddingsIdempotencyLease }>
  | Readonly<{ kind: "replay"; response: Response }>
  | Readonly<{ kind: "error"; response: Response }>;

type EmbeddingsJobStatus = "queued" | "running" | "succeeded" | "failed";

type EmbeddingsJobRecord = Readonly<{
  id: string;
  status: EmbeddingsJobStatus;
  created_at_ms: number;
  updated_at_ms: number;
  model: string;
  cache_profile_key: string;
  upstream: "voyage";
  upstream_model: "voyage-4-large";
  input_type: VoyageEmbeddingsInputType;
  dimensions: VoyageEmbeddingsDimension;
  output_dtype: VoyageEmbeddingsOutputDtype;
  encoding_format: EmbeddingsEncodingFormat;
  truncation: boolean;
  input_hashes: string[];
  input_count: number;
  total_chars: number;
  usage_total_tokens: number;
  retry_after_seconds: number | null;
  locked_until_ms: number | null;
  error: { message: string; type: string; code?: string } | null;
}>;

type EmbeddingsJobInputRecord = Readonly<{
  v: 1;
  iv_b64: string;
  data_b64: string;
  created_at_ms: number;
}>;

type EmbeddingsJobLookupRecord = Readonly<{
  cache_profile_key: string;
}>;

const embeddingsJobKey = (tokenHash: string, cacheProfileKey: string, id: string): Deno.KvKey => [
  "embeddings",
  "jobs",
  "v2",
  tokenHash,
  cacheProfileKey,
  id,
];
const embeddingsJobLookupKey = (tokenHash: string, id: string): Deno.KvKey => [
  "embeddings",
  "jobs",
  "v2",
  "lookup",
  tokenHash,
  id,
];
const embeddingsJobInputKey = (
  tokenHash: string,
  cacheProfileKey: string,
  jobId: string,
  hash: string,
): Deno.KvKey => [
  "embeddings",
  "jobs",
  "v2",
  "input",
  tokenHash,
  cacheProfileKey,
  jobId,
  hash,
];

const embeddingsCacheIndexKey = (
  cacheProfileKey: string,
  createdAtMs: number,
  hash: string,
): Deno.KvKey => ["embeddings", "v2", "cache_index", cacheProfileKey, createdAtMs, hash];
const embeddingsCacheGlobalIndexPrefix: Deno.KvKey = ["embeddings", "v2", "cache_index_global"];
const embeddingsCacheGlobalIndexKey = (
  createdAtMs: number,
  cacheProfileKey: string,
  hash: string,
): Deno.KvKey => [...embeddingsCacheGlobalIndexPrefix, createdAtMs, cacheProfileKey, hash];
const embeddingsCacheIndexByHashKey = (cacheProfileKey: string, hash: string): Deno.KvKey => [
  "embeddings",
  "v2",
  "cache_index_by_hash",
  cacheProfileKey,
  hash,
];
const embeddingsCacheKey = (cacheProfileKey: string, hash: string): Deno.KvKey => [
  "embeddings",
  "v2",
  "cache",
  cacheProfileKey,
  hash,
];

const embeddingsIdempotencyKey = (principalHash: string, idempotencyKeyHash: string): Deno.KvKey => [
  "embeddings",
  "idempotency",
  "v1",
  principalHash,
  idempotencyKeyHash,
];

const embeddingsIdempotencyResponseKeyPrefix = (
  principalHash: string,
  idempotencyKeyHash: string,
): Deno.KvKey => [
  "embeddings",
  "idempotency",
  "v1",
  "response",
  principalHash,
  idempotencyKeyHash,
];

const isEmbeddingsIdempotencyState = (value: unknown): value is EmbeddingsIdempotencyState =>
  value === "reserved" || value === "dispatched" || value === "succeeded" || value === "indeterminate";

const normalizeEmbeddingsIdempotencyRecord = (value: unknown): EmbeddingsIdempotencyRecord | null => {
  if (!isRecord(value) || value.v !== 1 || typeof value.fingerprint !== "string") return null;
  if (!isEmbeddingsIdempotencyState(value.state)) return null;
  if (value.owner_request_id !== null && typeof value.owner_request_id !== "string") return null;
  if (
    typeof value.created_at_ms !== "number" || !Number.isFinite(value.created_at_ms) ||
    typeof value.updated_at_ms !== "number" || !Number.isFinite(value.updated_at_ms)
  ) {
    return null;
  }
  if (
    value.lease_until_ms !== null &&
    (typeof value.lease_until_ms !== "number" || !Number.isFinite(value.lease_until_ms))
  ) {
    return null;
  }
  if (
    value.response_status !== null &&
    (typeof value.response_status !== "number" || !Number.isInteger(value.response_status))
  ) {
    return null;
  }
  if (value.response_content_type !== null && typeof value.response_content_type !== "string") return null;
  if (value.response_generation !== null && typeof value.response_generation !== "string") return null;
  if (
    value.response_chunk_count !== null &&
    (
      typeof value.response_chunk_count !== "number" ||
      !Number.isInteger(value.response_chunk_count) ||
      value.response_chunk_count < 1 ||
      value.response_chunk_count > EMBEDDINGS_IDEMPOTENCY_MAX_RESPONSE_CHUNKS
    )
  ) {
    return null;
  }
  if (value.response_sha256 !== null && typeof value.response_sha256 !== "string") return null;

  return {
    v: 1,
    fingerprint: value.fingerprint,
    state: value.state,
    owner_request_id: value.owner_request_id,
    created_at_ms: Math.trunc(value.created_at_ms),
    updated_at_ms: Math.trunc(value.updated_at_ms),
    lease_until_ms: value.lease_until_ms === null ? null : Math.trunc(value.lease_until_ms),
    response_status: value.response_status === null ? null : Math.trunc(value.response_status),
    response_content_type: value.response_content_type,
    response_generation: value.response_generation,
    response_chunk_count: value.response_chunk_count === null ? null : Math.trunc(value.response_chunk_count),
    response_sha256: value.response_sha256,
  };
};

const embeddingsIdempotencyError = (
  status: 409 | 503,
  message: string,
  code:
    | "embedding_idempotency_conflict"
    | "embedding_idempotency_in_progress"
    | "embedding_idempotency_indeterminate"
    | "embedding_idempotency_unavailable",
  retryAfterSeconds?: number,
): Response =>
  openaiError(status, message, code, {
    type: status === 503 ? "server_error" : "idempotency_error",
    param: null,
    ...(retryAfterSeconds === undefined ? {} : { headers: { "Retry-After": String(retryAfterSeconds) } }),
  });

const embeddingsIdempotencyConflictResponse = (): Response =>
  embeddingsIdempotencyError(
    409,
    "Idempotency-Key was already used with a different embeddings request.",
    "embedding_idempotency_conflict",
  );

const embeddingsIdempotencyInProgressResponse = (): Response =>
  embeddingsIdempotencyError(
    409,
    "The embeddings request for this Idempotency-Key is still in progress.",
    "embedding_idempotency_in_progress",
    1,
  );

const embeddingsIdempotencyIndeterminateResponse = (): Response =>
  embeddingsIdempotencyError(
    409,
    "The embeddings request outcome is indeterminate and will not be dispatched again.",
    "embedding_idempotency_indeterminate",
  );

const embeddingsIdempotencyUnavailableResponse = (): Response =>
  embeddingsIdempotencyError(
    503,
    "Idempotent embeddings requests require durable KV storage.",
    "embedding_idempotency_unavailable",
  );

const hasAsciiControlCharacter = (value: string): boolean => {
  for (let index = 0; index < value.length; index += 1) {
    const code = value.charCodeAt(index);
    if (code <= 0x1f || code === 0x7f) return true;
  }
  return false;
};

const buildEmbeddingsIdempotencyFingerprint = async (
  profile: ResolvedEmbeddingsProfile,
  orderedInputHashes: string[],
): Promise<string> =>
  await sha256Hex(
    JSON.stringify([
      "uos-embeddings-idempotency-v1",
      profile.upstream,
      profile.upstream_model,
      profile.input_type,
      profile.dimensions,
      profile.output_dtype,
      profile.encoding_format,
      profile.truncation,
      orderedInputHashes,
    ]),
  );

const loadEmbeddingsIdempotencyResponse = async (
  lease: Omit<EmbeddingsIdempotencyLease, "ownerRequestId">,
  record: EmbeddingsIdempotencyRecord,
): Promise<Response | null> => {
  if (
    record.state !== "succeeded" ||
    record.response_status !== 200 ||
    !record.response_content_type ||
    !record.response_generation ||
    record.response_chunk_count === null ||
    !record.response_sha256
  ) {
    return null;
  }

  const chunks = await Promise.all(
    Array.from(
      { length: record.response_chunk_count },
      (_, index) =>
        lease.kv.get<string>([
          ...lease.responseKeyPrefix,
          record.response_generation!,
          index,
        ]),
    ),
  );
  if (chunks.some((entry) => typeof entry.value !== "string")) return null;
  const body = chunks.map((entry) => entry.value as string).join("");
  if (await sha256Hex(body) !== record.response_sha256) return null;
  return new Response(body, {
    status: record.response_status,
    headers: {
      "Content-Type": record.response_content_type,
      "x-uos-idempotency-replayed": "true",
    },
  });
};

const markEmbeddingsIdempotencyIndeterminate = async (
  lease: EmbeddingsIdempotencyLease,
): Promise<void> => {
  try {
    for (let attempt = 0; attempt < 4; attempt += 1) {
      const entry = await lease.kv.get<EmbeddingsIdempotencyRecord>(lease.key);
      const record = normalizeEmbeddingsIdempotencyRecord(entry.value);
      if (!record || record.fingerprint !== lease.fingerprint) return;
      if (record.state === "indeterminate" || record.state === "succeeded") return;
      const now = Date.now();
      const next: EmbeddingsIdempotencyRecord = {
        ...record,
        state: "indeterminate",
        owner_request_id: null,
        updated_at_ms: now,
        lease_until_ms: null,
        response_status: null,
        response_content_type: null,
        response_generation: null,
        response_chunk_count: null,
        response_sha256: null,
      };
      const commit = await lease.kv.atomic()
        .check(entry)
        .set(lease.key, next, { expireIn: EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS })
        .commit();
      if (commit.ok) return;
    }
  } catch (error) {
    console.error("[ai.ubq.fi] embeddings idempotency indeterminate-state write failed:", error);
  }
};

const acquireEmbeddingsIdempotencyLease = async (params: {
  kv: Deno.Kv;
  principal: string;
  idempotencyKey: string;
  fingerprint: string;
  requestId: string;
}): Promise<EmbeddingsIdempotencyAcquireResult> => {
  const [principalHash, idempotencyKeyHash] = await Promise.all([
    sha256Hex(`uos-embeddings-principal-v1:${params.principal}`),
    sha256Hex(`uos-embeddings-key-v1:${params.idempotencyKey}`),
  ]);
  const key = embeddingsIdempotencyKey(principalHash, idempotencyKeyHash);
  const responseKeyPrefix = embeddingsIdempotencyResponseKeyPrefix(principalHash, idempotencyKeyHash);
  const lease: EmbeddingsIdempotencyLease = {
    kv: params.kv,
    key,
    responseKeyPrefix,
    fingerprint: params.fingerprint,
    ownerRequestId: params.requestId,
  };

  try {
    for (let attempt = 0; attempt < 8; attempt += 1) {
      const entry = await params.kv.get<EmbeddingsIdempotencyRecord>(key);
      const record = normalizeEmbeddingsIdempotencyRecord(entry.value);
      const now = Date.now();

      if (entry.versionstamp === null) {
        const reserved: EmbeddingsIdempotencyRecord = {
          v: 1,
          fingerprint: params.fingerprint,
          state: "reserved",
          owner_request_id: params.requestId,
          created_at_ms: now,
          updated_at_ms: now,
          lease_until_ms: now + EMBEDDINGS_IDEMPOTENCY_LEASE_MS,
          response_status: null,
          response_content_type: null,
          response_generation: null,
          response_chunk_count: null,
          response_sha256: null,
        };
        const commit = await params.kv.atomic()
          .check(entry)
          .set(key, reserved, { expireIn: EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS })
          .commit();
        if (commit.ok) return { kind: "acquired", lease };
        continue;
      }

      if (!record) return { kind: "error", response: embeddingsIdempotencyIndeterminateResponse() };
      if (record.fingerprint !== params.fingerprint) {
        return { kind: "error", response: embeddingsIdempotencyConflictResponse() };
      }

      if (record.state === "succeeded") {
        const replay = await loadEmbeddingsIdempotencyResponse(lease, record);
        if (replay) return { kind: "replay", response: replay };
        const indeterminate: EmbeddingsIdempotencyRecord = {
          ...record,
          state: "indeterminate",
          owner_request_id: null,
          updated_at_ms: now,
          lease_until_ms: null,
          response_status: null,
          response_content_type: null,
          response_generation: null,
          response_chunk_count: null,
          response_sha256: null,
        };
        const commit = await params.kv.atomic()
          .check(entry)
          .set(key, indeterminate, { expireIn: EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS })
          .commit();
        if (commit.ok) return { kind: "error", response: embeddingsIdempotencyIndeterminateResponse() };
        continue;
      }

      if (record.state === "indeterminate") {
        return { kind: "error", response: embeddingsIdempotencyIndeterminateResponse() };
      }

      if (record.lease_until_ms !== null && record.lease_until_ms > now) {
        return { kind: "error", response: embeddingsIdempotencyInProgressResponse() };
      }

      if (record.state === "dispatched") {
        const indeterminate: EmbeddingsIdempotencyRecord = {
          ...record,
          state: "indeterminate",
          owner_request_id: null,
          updated_at_ms: now,
          lease_until_ms: null,
          response_status: null,
          response_content_type: null,
          response_generation: null,
          response_chunk_count: null,
          response_sha256: null,
        };
        const commit = await params.kv.atomic()
          .check(entry)
          .set(key, indeterminate, { expireIn: EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS })
          .commit();
        if (commit.ok) return { kind: "error", response: embeddingsIdempotencyIndeterminateResponse() };
        continue;
      }

      const reserved: EmbeddingsIdempotencyRecord = {
        ...record,
        state: "reserved",
        owner_request_id: params.requestId,
        updated_at_ms: now,
        lease_until_ms: now + EMBEDDINGS_IDEMPOTENCY_LEASE_MS,
        response_status: null,
        response_content_type: null,
        response_generation: null,
        response_chunk_count: null,
        response_sha256: null,
      };
      const commit = await params.kv.atomic()
        .check(entry)
        .set(key, reserved, { expireIn: EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS })
        .commit();
      if (commit.ok) return { kind: "acquired", lease };
    }
  } catch (error) {
    console.error("[ai.ubq.fi] embeddings idempotency reservation failed:", error);
  }

  return { kind: "error", response: embeddingsIdempotencyUnavailableResponse() };
};

const markEmbeddingsIdempotencyDispatched = async (
  lease: EmbeddingsIdempotencyLease,
): Promise<boolean> => {
  try {
    for (let attempt = 0; attempt < 4; attempt += 1) {
      const entry = await lease.kv.get<EmbeddingsIdempotencyRecord>(lease.key);
      const record = normalizeEmbeddingsIdempotencyRecord(entry.value);
      if (
        !record ||
        record.fingerprint !== lease.fingerprint ||
        record.state !== "reserved" ||
        record.owner_request_id !== lease.ownerRequestId
      ) {
        return false;
      }
      const now = Date.now();
      const dispatched: EmbeddingsIdempotencyRecord = {
        ...record,
        state: "dispatched",
        updated_at_ms: now,
        lease_until_ms: now + EMBEDDINGS_IDEMPOTENCY_LEASE_MS,
      };
      const commit = await lease.kv.atomic()
        .check(entry)
        .set(lease.key, dispatched, { expireIn: EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS })
        .commit();
      if (commit.ok) return true;
    }
  } catch (error) {
    console.error("[ai.ubq.fi] embeddings idempotency dispatch-state write failed:", error);
  }
  return false;
};

const releaseEmbeddingsIdempotencyReservation = async (
  lease: EmbeddingsIdempotencyLease,
  allowDispatched: boolean,
): Promise<boolean> => {
  try {
    for (let attempt = 0; attempt < 4; attempt += 1) {
      const entry = await lease.kv.get<EmbeddingsIdempotencyRecord>(lease.key);
      const record = normalizeEmbeddingsIdempotencyRecord(entry.value);
      if (!record || record.fingerprint !== lease.fingerprint) return false;
      if (record.owner_request_id !== lease.ownerRequestId) return false;
      if (record.state !== "reserved" && !(allowDispatched && record.state === "dispatched")) return false;
      const commit = await lease.kv.atomic().check(entry).delete(lease.key).commit();
      if (commit.ok) return true;
    }
  } catch (error) {
    console.error("[ai.ubq.fi] embeddings idempotency reservation release failed:", error);
  }
  return false;
};

const storeEmbeddingsIdempotencySuccess = async (
  lease: EmbeddingsIdempotencyLease,
  response: Response,
): Promise<boolean> => {
  try {
    const body = await response.clone().text();
    const chunks: string[] = [];
    for (let offset = 0; offset < body.length; offset += EMBEDDINGS_IDEMPOTENCY_RESPONSE_CHUNK_CHARS) {
      chunks.push(body.slice(offset, offset + EMBEDDINGS_IDEMPOTENCY_RESPONSE_CHUNK_CHARS));
    }
    if (!chunks.length) chunks.push("");
    if (chunks.length > EMBEDDINGS_IDEMPOTENCY_MAX_RESPONSE_CHUNKS) return false;
    const responseGeneration = lease.ownerRequestId;
    for (let index = 0; index < chunks.length; index += 1) {
      await lease.kv.set(
        [...lease.responseKeyPrefix, responseGeneration, index],
        chunks[index]!,
        { expireIn: EMBEDDINGS_IDEMPOTENCY_RESPONSE_TTL_MS },
      );
    }
    const bodyHash = await sha256Hex(body);

    for (let attempt = 0; attempt < 4; attempt += 1) {
      const entry = await lease.kv.get<EmbeddingsIdempotencyRecord>(lease.key);
      const record = normalizeEmbeddingsIdempotencyRecord(entry.value);
      if (
        !record ||
        record.fingerprint !== lease.fingerprint ||
        (record.state !== "reserved" && record.state !== "dispatched") ||
        record.owner_request_id !== lease.ownerRequestId
      ) {
        return false;
      }
      const now = Date.now();
      const succeeded: EmbeddingsIdempotencyRecord = {
        ...record,
        state: "succeeded",
        owner_request_id: null,
        updated_at_ms: now,
        lease_until_ms: null,
        response_status: response.status,
        response_content_type: response.headers.get("Content-Type") ?? "application/json",
        response_generation: responseGeneration,
        response_chunk_count: chunks.length,
        response_sha256: bodyHash,
      };
      const commit = await lease.kv.atomic()
        .check(entry)
        .set(lease.key, succeeded, { expireIn: EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS })
        .commit();
      if (commit.ok) return true;
    }
  } catch (error) {
    console.error("[ai.ubq.fi] embeddings idempotency response write failed:", error);
  }
  return false;
};

const normalizeEmbeddingsCacheTimestampMs = (value: unknown): number | null => {
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  const ts = Math.trunc(value);
  if (ts < 0) return null;
  return ts;
};

type EmbeddingsCacheEvictResult = Readonly<{
  evicted_embeddings: number;
  deleted_stale_index_keys: number;
}>;

const isEmbeddingsCacheQuotaError = (error: unknown): boolean => {
  const name = typeof (error as { name?: unknown })?.name === "string" ? String((error as { name: string }).name) : "";
  const message = error instanceof Error ? error.message : typeof error === "string" ? error : "";
  const combined = `${name} ${message}`.toLowerCase();
  if (!combined) return false;
  return (
    combined.includes("quota") ||
    (combined.includes("insufficient") && combined.includes("storage")) ||
    (combined.includes("insufficient") && combined.includes("space")) ||
    combined.includes("no space") ||
    combined.includes("storage limit") ||
    (combined.includes("storage") && combined.includes("exceeded"))
  );
};

const writeEmbeddingsCacheEntry = async (
  kv: Deno.Kv,
  cacheProfileKey: string,
  hash: string,
  embedding: number[],
  createdAtMs: number,
): Promise<{ isNew: boolean }> => {
  const byHashKey = embeddingsCacheIndexByHashKey(cacheProfileKey, hash);
  const cacheKey = embeddingsCacheKey(cacheProfileKey, hash);

  // Concurrency-safe: if multiple requests try to cache the same hash, only one
  // will win the "create index" CAS; the others will reuse the winner's index.
  for (let attempt = 0; attempt < 5; attempt += 1) {
    const entry = await kv.get<number>(byHashKey);
    const existingCreatedAtMs = normalizeEmbeddingsCacheTimestampMs(entry.value);
    if (existingCreatedAtMs !== null) {
      const indexKey = embeddingsCacheIndexKey(cacheProfileKey, existingCreatedAtMs, hash);
      const updated = await kv.atomic()
        .check(entry)
        .set(cacheKey, { embedding, created_at: new Date(existingCreatedAtMs).toISOString() })
        .set(indexKey, 1)
        .set(embeddingsCacheGlobalIndexKey(existingCreatedAtMs, cacheProfileKey, hash), 1)
        .commit();
      if (updated.ok) return { isNew: false };
      continue;
    }

    const createdAtIso = new Date(createdAtMs).toISOString();
    const indexKey = embeddingsCacheIndexKey(cacheProfileKey, createdAtMs, hash);
    const created = await kv.atomic()
      .check(entry)
      .set(cacheKey, { embedding, created_at: createdAtIso })
      .set(indexKey, 1)
      .set(embeddingsCacheGlobalIndexKey(createdAtMs, cacheProfileKey, hash), 1)
      .set(byHashKey, createdAtMs)
      .commit();
    if (created.ok) return { isNew: true };
    // CAS failed: `byHashKey` was updated/created in between, or was evicted and
    // recreated concurrently. Retry to reuse the now-canonical pointer.
  }
  return { isNew: false };
};

const writeEmbeddingsCacheEntryBestEffort = async (
  kv: Deno.Kv,
  cacheProfileKey: string,
  hash: string,
  embedding: number[],
  createdAtMs: number,
  deadlineMs: number,
): Promise<{ isNew: boolean }> => {
  let evictBatch = EMBEDDINGS_CACHE_EVICT_BATCH;
  // Attempts = 1 (initial write) + max retries.
  for (let attempt = 0; attempt <= EMBEDDINGS_CACHE_QUOTA_MAX_RETRIES; attempt += 1) {
    if (Date.now() >= deadlineMs) return { isNew: false };
    try {
      return await writeEmbeddingsCacheEntry(kv, cacheProfileKey, hash, embedding, createdAtMs);
    } catch (error) {
      if (!isEmbeddingsCacheQuotaError(error)) {
        console.warn("[ai.ubq.fi] embeddings_cache write failed:", error);
        return { isNew: false };
      }

      // KV rejected the write (likely storage quota). Evict the oldest entries
      // across every embedding profile so a newly introduced profile cannot be
      // starved by cache entries owned by another profile.
      try {
        const evicted = await evictOldestEmbeddingsCacheEntries(kv, evictBatch);
        console.warn(
          `[ai.ubq.fi] embeddings_cache quota eviction requesting_profile=${cacheProfileKey} scope=global evicted=${evicted.evicted_embeddings} stale_index_deleted=${evicted.deleted_stale_index_keys} batch=${evictBatch}`,
        );
        if (evicted.evicted_embeddings <= 0 && evicted.deleted_stale_index_keys <= 0) return { isNew: false };
      } catch (evictError) {
        console.warn("[ai.ubq.fi] embeddings_cache quota eviction failed:", evictError);
        return { isNew: false };
      }

      evictBatch = Math.min(EMBEDDINGS_CACHE_EVICT_MAX_BATCH, evictBatch * 2);
    }
  }
  return { isNew: false };
};

const evictOldestEmbeddingsCacheEntries = async (
  kv: Deno.Kv,
  count: number,
): Promise<EmbeddingsCacheEvictResult> => {
  const keys: Array<{
    globalIndexKey: Deno.KvKey;
    cacheProfileKey: string;
    createdAtMs: number;
    hash: string;
  }> = [];
  for await (const entry of kv.list({ prefix: embeddingsCacheGlobalIndexPrefix }, { limit: count })) {
    const key = entry.key;
    const hash = key.at(-1);
    const cacheProfileKey = key.at(-2);
    const createdAtMs = key.at(-3);
    if (typeof hash !== "string" || !hash) continue;
    if (typeof cacheProfileKey !== "string" || !cacheProfileKey) continue;
    if (typeof createdAtMs !== "number" || !Number.isFinite(createdAtMs)) continue;
    keys.push({
      globalIndexKey: key,
      cacheProfileKey,
      createdAtMs: Math.trunc(createdAtMs),
      hash,
    });
  }
  if (!keys.length) return { evicted_embeddings: 0, deleted_stale_index_keys: 0 };

  const byHashKeys = keys.map((item) => embeddingsCacheIndexByHashKey(item.cacheProfileKey, item.hash));
  const byHashEntries = await Promise.all(byHashKeys.map((key) => kv.get<number>(key)));

  let evictedEmbeddings = 0;
  let deletedStaleIndexKeys = 0;
  for (let i = 0; i < keys.length; i += 1) {
    const { globalIndexKey, cacheProfileKey, createdAtMs, hash } = keys[i]!;
    const pointerEntry = byHashEntries[i]!;
    const pointer = normalizeEmbeddingsCacheTimestampMs(pointerEntry.value);
    const cacheKey = embeddingsCacheKey(cacheProfileKey, hash);
    const profileIndexKey = embeddingsCacheIndexKey(cacheProfileKey, createdAtMs, hash);

    if (pointer !== null && pointer !== createdAtMs) {
      // Stale duplicate index keys for this hash; delete only the indexes.
      const deleted = await kv.atomic()
        .check(pointerEntry)
        .delete(globalIndexKey)
        .delete(profileIndexKey)
        .commit();
      if (deleted.ok) deletedStaleIndexKeys += 1;
      continue;
    }

    if (pointer === null) {
      // Missing pointer (legacy / partial state): only delete the embedding value
      // if it still matches the index timestamp to avoid deleting a newer cache
      // entry that happens to share the same hash.
      const valueEntry = await kv.get<{ created_at?: unknown }>(cacheKey);
      const value = valueEntry.value;
      const createdAtIso = isRecord(value) && typeof value.created_at === "string" ? value.created_at : null;
      const expectedIso = new Date(createdAtMs).toISOString();
      if (createdAtIso !== expectedIso) {
        const deleted = await kv.atomic()
          .check(pointerEntry)
          .delete(globalIndexKey)
          .delete(profileIndexKey)
          .commit();
        if (deleted.ok) deletedStaleIndexKeys += 1;
        continue;
      }

      const commit = await kv.atomic()
        .check(pointerEntry)
        .delete(globalIndexKey)
        .delete(profileIndexKey)
        .delete(cacheKey)
        .commit();
      if (commit.ok) evictedEmbeddings += 1;
      continue;
    }

    // Canonical pointer match: evict embedding + index + pointer as an atomic unit.
    const commit = await kv.atomic()
      .check(pointerEntry)
      .delete(globalIndexKey)
      .delete(profileIndexKey)
      .delete(cacheKey)
      .delete(embeddingsCacheIndexByHashKey(cacheProfileKey, hash))
      .commit();
    if (commit.ok) evictedEmbeddings += 1;
  }
  return { evicted_embeddings: evictedEmbeddings, deleted_stale_index_keys: deletedStaleIndexKeys };
};

const resolveEmbeddingsJobTokenSeed = (
  jobId: string,
  authToken: string | null,
  usageContext?: UsageContext,
): string => {
  // Prefer stable identities so queued jobs remain resolvable even if bearer tokens refresh/rotate.
  if (usageContext?.keyId) return `uos_api_key_id:${usageContext.keyId}`;
  if (usageContext?.kernelRepo) {
    return `uos_kernel_repo:${usageContext.kernelRepo.owner}/${usageContext.kernelRepo.repo}`;
  }
  if (authToken) return authToken;
  return jobId;
};

const TOKEN_ESTIMATOR = new TextEncoder();

const getEnv = (key: string): string | undefined => {
  try {
    return Deno.env.get(key);
  } catch {
    return undefined;
  }
};

const sleep = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms));

const estimateTokens = (text: string): number => {
  if (!text) return 0;
  const bytes = TOKEN_ESTIMATOR.encode(text).byteLength;
  return Math.ceil(bytes / 4);
};

const estimateTokenCount = (texts: string[]): number => texts.reduce((sum, text) => sum + estimateTokens(text), 0);

const chunkByTokenBudget = (
  items: ReadonlyArray<{ hash: string; text: string }>,
  maxItems: number,
  maxTokens: number,
): Array<Array<{ hash: string; text: string }>> => {
  const out: Array<Array<{ hash: string; text: string }>> = [];
  const itemLimit = Math.max(1, Math.trunc(maxItems));
  const tokenLimit = Math.max(1, Math.trunc(maxTokens));

  let current: Array<{ hash: string; text: string }> = [];
  let currentTokens = 0;

  for (const item of items) {
    const tokens = estimateTokens(item.text);
    const nextTokens = currentTokens + tokens;
    const hitsItemLimit = current.length >= itemLimit;
    const hitsTokenLimit = nextTokens > tokenLimit && current.length > 0;
    if (hitsItemLimit || hitsTokenLimit) {
      out.push(current);
      current = [];
      currentTokens = 0;
    }
    current.push(item);
    currentTokens += tokens;
  }
  if (current.length) out.push(current);
  return out;
};

const normalizeVoyageRateLimitState = (value: unknown): VoyageRateLimitState | null => {
  if (!isRecord(value)) return null;
  const windowStart = typeof value.window_start_ms === "number" && Number.isFinite(value.window_start_ms)
    ? Math.trunc(value.window_start_ms)
    : null;
  const requests = typeof value.requests === "number" && Number.isFinite(value.requests)
    ? Math.trunc(value.requests)
    : null;
  const tokens = typeof value.tokens === "number" && Number.isFinite(value.tokens) ? Math.trunc(value.tokens) : null;
  if (windowStart === null || requests === null || tokens === null) return null;
  if (windowStart < 0 || requests < 0 || tokens < 0) return null;
  return { window_start_ms: windowStart, requests, tokens };
};

const tryReserveVoyageBudget = async (
  kv: Deno.Kv,
  tokens: number,
): Promise<{ ok: true } | { ok: false; wait_ms: number }> => {
  const windowMs = 60_000;
  const now = Date.now();
  const entry = await kv.get<VoyageRateLimitState>(VOYAGE_RATE_LIMIT_KEY);
  const current = normalizeVoyageRateLimitState(entry.value);
  const state = !current || now - current.window_start_ms >= windowMs
    ? { window_start_ms: now, requests: 0, tokens: 0 }
    : current;

  const wouldExceedRequests = VOYAGE_RATE_LIMIT_RPM > 0 && state.requests + 1 > VOYAGE_RATE_LIMIT_RPM;
  const wouldExceedTokens = VOYAGE_RATE_LIMIT_TPM > 0 && state.tokens + tokens > VOYAGE_RATE_LIMIT_TPM;
  if (wouldExceedRequests || wouldExceedTokens) {
    const waitMs = Math.max(0, windowMs - (now - state.window_start_ms));
    return { ok: false, wait_ms: waitMs };
  }

  const next: VoyageRateLimitState = {
    window_start_ms: state.window_start_ms,
    requests: state.requests + 1,
    tokens: state.tokens + tokens,
  };
  const commit = await kv.atomic().check(entry).set(VOYAGE_RATE_LIMIT_KEY, next).commit();
  if (commit.ok) return { ok: true };
  return { ok: false, wait_ms: 0 };
};

const applyVoyageRateLimit = async (
  kv: Deno.Kv,
  tokens: number,
  deadlineMs: number,
): Promise<{ ok: true } | { ok: false; wait_ms: number }> => {
  // Best-effort concurrency-safe rate limiting using KV. If we can't reserve
  // within the request deadline, we fail with 429 and let clients retry.
  for (;;) {
    const now = Date.now();
    if (now >= deadlineMs) return { ok: false, wait_ms: 0 };
    let reserved: { ok: true } | { ok: false; wait_ms: number } = { ok: false, wait_ms: 0 };
    for (let attempt = 0; attempt < 6; attempt += 1) {
      reserved = await tryReserveVoyageBudget(kv, tokens);
      if (reserved.ok) return reserved;
      if (reserved.wait_ms > 0) break;
      await sleep(5 + attempt * 5);
    }
    if (reserved.ok) return reserved;
    const waitMs = reserved.wait_ms;
    if (waitMs <= 0) {
      // CAS contention without a concrete rate-limit wait; avoid tight spinning.
      const now2 = Date.now();
      const sleepMs = Math.min(25, Math.max(0, deadlineMs - now2));
      if (sleepMs > 0) await sleep(sleepMs);
      continue;
    }
    if (now + waitMs > deadlineMs) return { ok: false, wait_ms: waitMs };
    await sleep(waitMs);
  }
};

const parseEmbeddingsEncodingFormat = (
  value: unknown,
): { ok: true; value: EmbeddingsEncodingFormat } | { ok: false; message: string } => {
  if (value === undefined) return { ok: true, value: "float" };
  if (typeof value !== "string") return { ok: false, message: "encoding_format must be a string" };
  if (value === "float" || value === "base64") return { ok: true, value };
  return { ok: false, message: 'encoding_format must be one of: "float", "base64"' };
};

const parseEmbeddingsDimensions = (
  value: unknown,
): { ok: true; value: VoyageEmbeddingsDimension } | { ok: false; message: string } => {
  if (value === undefined) return { ok: true, value: VOYAGE_DEFAULT_DIMENSIONS };
  if (typeof value !== "number" || !Number.isFinite(value) || !Number.isInteger(value)) {
    return { ok: false, message: "dimensions must be an integer" };
  }
  if (!VOYAGE_SUPPORTED_DIMENSIONS.has(value)) {
    return { ok: false, message: "dimensions must be one of: 256, 512, 1024, 2048" };
  }
  return { ok: true, value: value as VoyageEmbeddingsDimension };
};

const buildEmbeddingsCacheProfileKey = (
  inputType: VoyageEmbeddingsInputType,
  dimensions: VoyageEmbeddingsDimension,
  encodingFormat: EmbeddingsEncodingFormat,
  truncation: boolean,
): string =>
  JSON.stringify([
    "voyage-profile-v2",
    VOYAGE_EMBEDDINGS_MODEL,
    inputType,
    dimensions,
    VOYAGE_OUTPUT_DTYPE,
    encodingFormat,
    truncation,
  ]);

const buildResolvedEmbeddingsProfile = (
  inputType: VoyageEmbeddingsInputType,
  dimensions: VoyageEmbeddingsDimension,
  encodingFormat: EmbeddingsEncodingFormat,
  truncation: boolean,
): ResolvedEmbeddingsProfile => ({
  upstream: "voyage",
  upstream_model: VOYAGE_EMBEDDINGS_MODEL,
  input_type: inputType,
  dimensions,
  output_dtype: VOYAGE_OUTPUT_DTYPE,
  encoding_format: encodingFormat,
  truncation,
  cache_profile_key: buildEmbeddingsCacheProfileKey(inputType, dimensions, encodingFormat, truncation),
});

const parseEmbeddingsRequest = (
  rawBody: Record<string, unknown>,
  contract: "uos_sync" | "uos_job",
): EmbeddingsParseResult => {
  const isJob = contract === "uos_job";
  const allowedKeys = isJob ? UOS_EMBEDDINGS_JOB_ALLOWED_KEYS : UOS_SYNC_EMBEDDINGS_ALLOWED_KEYS;
  const unknownKey = findUnknownKey(rawBody, allowedKeys);
  if (unknownKey) {
    return {
      ok: false,
      response: openaiError(400, `Unrecognized request argument supplied: ${unknownKey}`, "invalid_request_error"),
    };
  }

  const modelRaw = getString(rawBody.model);
  if (!modelRaw || !modelRaw.trim()) {
    return {
      ok: false,
      response: openaiError(400, "model is required and must be a non-empty string", "invalid_request_error", {
        param: "model",
      }),
    };
  }
  const model = modelRaw;
  if (model !== VOYAGE_EMBEDDINGS_MODEL) {
    return {
      ok: false,
      response: openaiError(400, `Unsupported embedding model: ${model}`, "model_not_found", { param: "model" }),
    };
  }

  const dimensions = parseEmbeddingsDimensions(rawBody.dimensions);
  if (!dimensions.ok) {
    return {
      ok: false,
      response: openaiError(400, dimensions.message, "invalid_request_error", { param: "dimensions" }),
    };
  }

  const encodingFormat = parseEmbeddingsEncodingFormat(rawBody.encoding_format);
  if (!encodingFormat.ok) {
    return {
      ok: false,
      response: openaiError(400, encodingFormat.message, "invalid_request_error", { param: "encoding_format" }),
    };
  }
  if (isJob && encodingFormat.value !== "float") {
    return {
      ok: false,
      response: openaiError(
        400,
        'encoding_format must be "float" for embeddings jobs',
        "invalid_request_error",
        { param: "encoding_format" },
      ),
    };
  }

  let inputType: VoyageEmbeddingsInputType = "document";
  let truncation = true;
  if (rawBody.input_type === undefined) {
    if (isJob) {
      return {
        ok: false,
        response: openaiError(
          400,
          'input_type is required and must be one of: "query", "document"',
          "invalid_request_error",
          { param: "input_type" },
        ),
      };
    }
  } else if (rawBody.input_type === "query" || rawBody.input_type === "document") {
    inputType = rawBody.input_type;
  } else {
    return {
      ok: false,
      response: openaiError(
        400,
        'input_type must be one of: "query", "document"',
        "invalid_request_error",
        { param: "input_type" },
      ),
    };
  }
  if (rawBody.truncation !== undefined && typeof rawBody.truncation !== "boolean") {
    return {
      ok: false,
      response: openaiError(400, "truncation must be a boolean", "invalid_request_error", {
        param: "truncation",
      }),
    };
  }
  truncation = rawBody.truncation === undefined ? true : rawBody.truncation;

  if (!isJob && Object.prototype.hasOwnProperty.call(rawBody, "user")) {
    const user = rawBody.user;
    if (user !== undefined && user !== null && typeof user !== "string") {
      return {
        ok: false,
        response: openaiError(400, "user must be a string", "invalid_request_error", { param: "user" }),
      };
    }
  }

  const inputRaw = rawBody.input;
  let inputs: string[] = [];
  if (typeof inputRaw === "string") {
    inputs = [inputRaw];
  } else if (Array.isArray(inputRaw)) {
    for (const item of inputRaw) {
      if (typeof item !== "string") {
        return {
          ok: false,
          response: openaiError(400, "input must be a string or an array of strings", "invalid_request_error", {
            param: "input",
          }),
        };
      }
      inputs.push(item);
    }
  } else {
    return {
      ok: false,
      response: openaiError(400, "input must be a string or an array of strings", "invalid_request_error", {
        param: "input",
      }),
    };
  }

  if (inputs.length === 0) {
    return {
      ok: false,
      response: openaiError(
        400,
        "input must be a non-empty string or a non-empty array",
        "invalid_request_error",
        { param: "input" },
      ),
    };
  }
  if (inputs.length > EMBEDDINGS_MAX_INPUTS_PER_REQUEST) {
    return {
      ok: false,
      response: openaiError(
        400,
        `Too many inputs: ${inputs.length} (max ${EMBEDDINGS_MAX_INPUTS_PER_REQUEST})`,
        "invalid_request_error",
        { param: "input" },
      ),
    };
  }

  let totalChars = 0;
  for (const text of inputs) {
    const len = text.length;
    if (len > EMBEDDINGS_MAX_CHARS_PER_INPUT) {
      return {
        ok: false,
        response: openaiError(
          400,
          `Input too large: ${len} chars (max ${EMBEDDINGS_MAX_CHARS_PER_INPUT})`,
          "invalid_request_error",
          { param: "input" },
        ),
      };
    }
    totalChars += len;
    if (totalChars > EMBEDDINGS_MAX_TOTAL_CHARS) {
      return {
        ok: false,
        response: openaiError(
          400,
          `Request too large: ${totalChars} chars total (max ${EMBEDDINGS_MAX_TOTAL_CHARS})`,
          "invalid_request_error",
          { param: "input" },
        ),
      };
    }
    const tokenEstimate = estimateTokens(text);
    if (tokenEstimate > VOYAGE_RATE_LIMIT_TPM) {
      return {
        ok: false,
        response: openaiError(
          400,
          `Input too large for embeddings provider: ~${tokenEstimate} tokens (max ${VOYAGE_RATE_LIMIT_TPM}).`,
          "invalid_request_error",
          { param: "input" },
        ),
      };
    }
  }

  return {
    ok: true,
    value: {
      model,
      inputs,
      total_chars: totalChars,
      profile: buildResolvedEmbeddingsProfile(inputType, dimensions.value, encodingFormat.value, truncation),
    },
  };
};

const parseUosEmbeddingsRequest = (rawBody: Record<string, unknown>): EmbeddingsParseResult =>
  parseEmbeddingsRequest(rawBody, "uos_sync");

const parseEmbeddingsJobRequest = (rawBody: Record<string, unknown>): EmbeddingsParseResult =>
  parseEmbeddingsRequest(rawBody, "uos_job");

const isValidEmbeddingVector = (value: unknown, dimensions: VoyageEmbeddingsDimension): value is number[] =>
  Array.isArray(value) &&
  value.length === dimensions &&
  value.every((item) => typeof item === "number" && Number.isFinite(item));

const floatEmbeddingToBase64 = (embedding: number[]): string => {
  const buffer = new ArrayBuffer(embedding.length * 4);
  const view = new DataView(buffer);
  for (let i = 0; i < embedding.length; i += 1) {
    view.setFloat32(i * 4, embedding[i], true);
  }
  const bytes = new Uint8Array(buffer);
  const chunkSize = 0x8000; // Avoid large variadic calls and quadratic string concatenation.
  const chunks: string[] = [];
  for (let i = 0; i < bytes.length; i += chunkSize) {
    const chunk = bytes.subarray(i, i + chunkSize);
    chunks.push(String.fromCharCode(...chunk));
  }
  return btoa(chunks.join(""));
};

const bytesToBase64 = (bytes: Uint8Array): string => {
  const chunkSize = 0x8000;
  const chunks: string[] = [];
  for (let i = 0; i < bytes.length; i += chunkSize) {
    const chunk = bytes.subarray(i, i + chunkSize);
    chunks.push(String.fromCharCode(...chunk));
  }
  return btoa(chunks.join(""));
};

const base64ToBytes = (value: string): Uint8Array<ArrayBuffer> | null => {
  try {
    const raw = atob(value);
    const bytes = new Uint8Array(new ArrayBuffer(raw.length));
    for (let i = 0; i < raw.length; i += 1) bytes[i] = raw.charCodeAt(i);
    return bytes;
  } catch {
    return null;
  }
};

const normalizeEmbeddingsJobInputRecord = (value: unknown): EmbeddingsJobInputRecord | null => {
  if (!isRecord(value)) return null;
  const v = value.v;
  if (v !== 1) return null;
  const iv = getString(value.iv_b64);
  const data = getString(value.data_b64);
  if (!iv || !data) return null;
  const createdAt = typeof value.created_at_ms === "number" && Number.isFinite(value.created_at_ms)
    ? Math.trunc(value.created_at_ms)
    : null;
  if (createdAt === null || createdAt < 0) return null;
  return { v: 1, iv_b64: iv, data_b64: data, created_at_ms: createdAt };
};

const importEmbeddingsJobKey = async (tokenSeed: string): Promise<CryptoKey> => {
  const material = new TextEncoder().encode(`uos_embeddings_job_v2:${tokenSeed}`);
  const digest = await crypto.subtle.digest("SHA-256", material);
  return await crypto.subtle.importKey("raw", digest, { name: "AES-GCM" }, false, ["encrypt", "decrypt"]);
};

const encryptEmbeddingsJobInput = async (tokenSeed: string, text: string): Promise<EmbeddingsJobInputRecord> => {
  const key = await importEmbeddingsJobKey(tokenSeed);
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const bytes = new TextEncoder().encode(text);
  const encrypted = await crypto.subtle.encrypt({ name: "AES-GCM", iv }, key, bytes);
  return {
    v: 1,
    iv_b64: bytesToBase64(iv),
    data_b64: bytesToBase64(new Uint8Array(encrypted)),
    created_at_ms: Date.now(),
  };
};

const decryptEmbeddingsJobInput = async (
  tokenSeed: string,
  record: EmbeddingsJobInputRecord,
): Promise<string | null> => {
  const iv = base64ToBytes(record.iv_b64);
  const data = base64ToBytes(record.data_b64);
  if (!iv || !data) return null;
  try {
    const key = await importEmbeddingsJobKey(tokenSeed);
    const decrypted = await crypto.subtle.decrypt({ name: "AES-GCM", iv }, key, data);
    return new TextDecoder().decode(new Uint8Array(decrypted));
  } catch {
    return null;
  }
};

const extractRetryAfterMs = (value: string | null): number | null => {
  if (!value) return null;
  const trimmed = value.trim();
  if (!trimmed) return null;
  const seconds = Number(trimmed);
  if (Number.isFinite(seconds) && seconds > 0) {
    return Math.min(60_000, Math.trunc(seconds * 1000));
  }
  const asDate = Date.parse(trimmed);
  if (!Number.isNaN(asDate)) {
    const delta = asDate - Date.now();
    if (delta > 0) return Math.min(60_000, Math.trunc(delta));
  }
  return null;
};

const readVoyageApiKey = async (kv: Deno.Kv | null): Promise<string | null> => {
  const envKey = (getEnv("VOYAGEAI_API_KEY") ?? "").trim();
  if (envKey) return envKey;
  if (!kv) return null;
  const entry = await kv.get<string>(VOYAGE_API_KEY_KV_KEY);
  const kvKey = typeof entry.value === "string" ? entry.value.trim() : "";
  return kvKey || null;
};

const fetchVoyageEmbeddings = async (params: {
  apiKey: string;
  model: "voyage-4-large";
  inputs: string[];
  inputType: VoyageEmbeddingsInputType;
  dimensions: VoyageEmbeddingsDimension;
  outputDtype: VoyageEmbeddingsOutputDtype;
  truncation: boolean;
  deadlineMs: number;
  beforeProviderDispatch?: UsageContext["beforeProviderDispatch"];
}): Promise<{ vectors: number[][]; totalTokens: number | null }> => {
  const controller = new AbortController();
  const now = Date.now();
  const timeoutMs = Math.max(1, Math.min(EMBEDDINGS_TIMEOUT_MS, params.deadlineMs - now));
  const timeout = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const dispatch = params.beforeProviderDispatch ? await params.beforeProviderDispatch("voyage") : undefined;
    if (controller.signal.aborted) {
      await dispatch?.cancelBeforeTransport();
      throw controller.signal.reason ?? new DOMException("The request was aborted.", "AbortError");
    }
    dispatch?.markTransportStarted();
    const resp = await fetch(VOYAGE_EMBEDDINGS_URL, {
      method: "POST",
      headers: {
        "Authorization": `Bearer ${params.apiKey}`,
        "Content-Type": "application/json",
      },
      body: JSON.stringify({
        model: params.model,
        input: params.inputs.length === 1 ? params.inputs[0] : params.inputs,
        input_type: params.inputType,
        output_dimension: params.dimensions,
        output_dtype: params.outputDtype,
        truncation: params.truncation,
      }),
      signal: controller.signal,
    });

    if (!resp.ok) {
      // Avoid echoing upstream bodies; they can contain provider details and may be surfaced to clients/logs.
      const err = new Error(`Voyage embeddings failed (${resp.status}).`);
      (err as { status?: number; retry_after_ms?: number }).status = resp.status;
      (err as { retry_after_ms?: number }).retry_after_ms = extractRetryAfterMs(resp.headers.get("Retry-After")) ??
        undefined;
      throw err;
    }

    const payload = await resp.json().catch(() => null) as unknown;
    if (!isRecord(payload) || !Array.isArray(payload.data)) {
      throw new Error("Voyage embeddings returned invalid JSON.");
    }

    let totalTokens: number | null = null;
    if (isRecord(payload.usage)) {
      const rawTotalTokens = payload.usage.total_tokens;
      if (typeof rawTotalTokens === "number" && Number.isFinite(rawTotalTokens)) {
        totalTokens = Math.max(0, Math.trunc(rawTotalTokens));
      }
    }

    const data = payload.data as Array<Record<string, unknown>>;
    const vectors: number[][] = [];
    for (const item of data) {
      const embedding = isRecord(item) ? item.embedding : null;
      if (!Array.isArray(embedding)) {
        throw new Error("Voyage embeddings response missing embedding vector.");
      }
      const vec: number[] = [];
      for (const v of embedding) {
        if (typeof v !== "number" || !Number.isFinite(v)) {
          throw new Error("Voyage embeddings response contained non-numeric values.");
        }
        vec.push(v);
      }
      vectors.push(vec);
    }
    return { vectors, totalTokens };
  } finally {
    clearTimeout(timeout);
  }
};

const apiKeyQuotaDispatchErrorResponse = (error: ApiKeyQuotaDispatchError): Response =>
  openaiError(error.status, error.message, error.code, {
    type: error.errorType,
    param: null,
    ...(error.retryAfter ? { headers: { "Retry-After": error.retryAfter } } : {}),
  });

type NormalizationResult<T> =
  | Readonly<{ ok: true; value: T }>
  | Readonly<{ ok: false; message: string; param: string }>;

type InputImageDetail = "auto" | "low" | "high" | "original";
type InputFileDetail = "auto" | "low" | "high";

const invalidNormalizedField = <T>(param: string, message: string): NormalizationResult<T> => ({
  ok: false,
  message,
  param,
});

const parseImageDetail = (
  value: unknown,
  param: string,
): NormalizationResult<InputImageDetail | null | undefined> => {
  if (value === undefined) return { ok: true, value: undefined };
  if (value === null) return { ok: true, value: null };
  if (value === "auto" || value === "low" || value === "high" || value === "original") {
    return { ok: true, value };
  }
  return invalidNormalizedField(param, `${param} must be one of auto, low, high, or original`);
};

const parseInputFileDetail = (
  value: unknown,
  param: string,
): NormalizationResult<InputFileDetail | undefined> => {
  if (value === undefined) return { ok: true, value: undefined };
  if (value === "auto" || value === "low" || value === "high") return { ok: true, value };
  return invalidNormalizedField(param, `${param} must be one of auto, low, or high`);
};

const findUnknownContentField = (value: Record<string, unknown>, allowed: readonly string[]): string | null => {
  const allowedFields = new Set(allowed);
  return Object.keys(value).find((key) => !allowedFields.has(key)) ?? null;
};

const normalizePromptCacheBreakpoint = (
  value: unknown,
  param: string,
): NormalizationResult<PromptCacheBreakpoint | undefined> => {
  if (value === undefined) return { ok: true, value: undefined };
  if (!isRecord(value) || Array.isArray(value)) {
    return invalidNormalizedField(param, `${param} must be an object`);
  }
  const unknown = findUnknownContentField(value, ["mode"]);
  if (unknown) return invalidNormalizedField(`${param}.${unknown}`, `Unknown cache breakpoint field: ${unknown}`);
  if (value.mode !== "explicit") {
    return invalidNormalizedField(`${param}.mode`, `${param}.mode must be explicit`);
  }
  return { ok: true, value: { mode: "explicit" } };
};

const validatePromptCacheControls = (rawRecord: Record<string, unknown>): NormalizationResult<void> => {
  if (
    Object.prototype.hasOwnProperty.call(rawRecord, "prompt_cache_key") &&
    typeof rawRecord.prompt_cache_key !== "string"
  ) {
    return invalidNormalizedField("prompt_cache_key", "prompt_cache_key must be a string");
  }

  if (Object.prototype.hasOwnProperty.call(rawRecord, "prompt_cache_options")) {
    const options = rawRecord.prompt_cache_options;
    if (!isRecord(options) || Array.isArray(options)) {
      return invalidNormalizedField("prompt_cache_options", "prompt_cache_options must be an object");
    }
    const unknown = findUnknownContentField(options, ["mode", "ttl"]);
    if (unknown) {
      return invalidNormalizedField(
        `prompt_cache_options.${unknown}`,
        `Unknown prompt cache option: ${unknown}`,
      );
    }
    if (options.mode !== undefined && options.mode !== "implicit" && options.mode !== "explicit") {
      return invalidNormalizedField(
        "prompt_cache_options.mode",
        "prompt_cache_options.mode must be implicit or explicit",
      );
    }
    if (options.ttl !== undefined && options.ttl !== "30m") {
      return invalidNormalizedField("prompt_cache_options.ttl", "prompt_cache_options.ttl must be 30m");
    }
  }

  if (
    Object.prototype.hasOwnProperty.call(rawRecord, "prompt_cache_retention") &&
    rawRecord.prompt_cache_retention !== "in_memory" &&
    rawRecord.prompt_cache_retention !== "24h"
  ) {
    return invalidNormalizedField(
      "prompt_cache_retention",
      "prompt_cache_retention must be in_memory or 24h",
    );
  }

  return { ok: true, value: undefined };
};

const withPromptCacheBreakpoint = <T extends object>(
  item: T,
  breakpoint: PromptCacheBreakpoint | undefined,
): T & { prompt_cache_breakpoint?: PromptCacheBreakpoint } =>
  breakpoint === undefined ? item : { ...item, prompt_cache_breakpoint: breakpoint };

const normalizeChatContentItems = (
  role: ResponseMessageItem["role"],
  content: unknown,
  param: string,
): NormalizationResult<MessageContentItem[]> => {
  const isAssistant = role === "assistant";
  const textItemType: "input_text" | "output_text" = isAssistant ? "output_text" : "input_text";
  if (typeof content === "string") return { ok: true, value: [{ type: textItemType, text: content }] };
  if (content === null && isAssistant) return { ok: true, value: [] };
  if (!Array.isArray(content)) return invalidNormalizedField(param, `${param} must be a string or an array`);

  const items: MessageContentItem[] = [];
  for (const [index, part] of content.entries()) {
    const partParam = `${param}[${index}]`;
    if (!isRecord(part) || Array.isArray(part)) {
      return invalidNormalizedField(partParam, `${partParam} must be an object`);
    }
    const partType = getString(part.type);
    if (partType === "text") {
      const unknown = findUnknownContentField(part, ["type", "text", "prompt_cache_breakpoint"]);
      if (unknown) return invalidNormalizedField(`${partParam}.${unknown}`, `Unknown content field: ${unknown}`);
      if (typeof part.text !== "string") {
        return invalidNormalizedField(`${partParam}.text`, `${partParam}.text must be a string`);
      }
      if (isAssistant && part.prompt_cache_breakpoint !== undefined) {
        return invalidNormalizedField(
          `${partParam}.prompt_cache_breakpoint`,
          "prompt_cache_breakpoint is not supported for assistant output content in this gateway",
        );
      }
      const breakpoint = normalizePromptCacheBreakpoint(
        part.prompt_cache_breakpoint,
        `${partParam}.prompt_cache_breakpoint`,
      );
      if (!breakpoint.ok) return breakpoint;
      if (textItemType === "input_text") {
        items.push(withPromptCacheBreakpoint({ type: "input_text", text: part.text }, breakpoint.value));
      } else {
        items.push({ type: "output_text", text: part.text });
      }
      continue;
    }
    if (partType === "refusal") {
      const unknown = findUnknownContentField(part, ["type", "refusal", "prompt_cache_breakpoint"]);
      if (unknown) return invalidNormalizedField(`${partParam}.${unknown}`, `Unknown content field: ${unknown}`);
      if (!isAssistant) {
        return invalidNormalizedField(`${partParam}.type`, `${partParam}.type is only valid for assistant messages`);
      }
      if (content.length !== 1) {
        return invalidNormalizedField(`${partParam}.type`, "assistant refusal content must be the only part");
      }
      if (typeof part.refusal !== "string") {
        return invalidNormalizedField(`${partParam}.refusal`, `${partParam}.refusal must be a string`);
      }
      if (part.prompt_cache_breakpoint !== undefined) {
        return invalidNormalizedField(
          `${partParam}.prompt_cache_breakpoint`,
          "prompt_cache_breakpoint is not supported for refusal content in this gateway",
        );
      }
      items.push({ type: "output_text", text: part.refusal });
      continue;
    }
    if (partType === "image_url") {
      const unknown = findUnknownContentField(part, ["type", "image_url", "prompt_cache_breakpoint"]);
      if (unknown) return invalidNormalizedField(`${partParam}.${unknown}`, `Unknown content field: ${unknown}`);
      if (role !== "user") {
        return invalidNormalizedField(`${partParam}.type`, `${partParam}.type is only valid for user messages`);
      }
      const image = isRecord(part.image_url) && !Array.isArray(part.image_url) ? part.image_url : null;
      if (!image) {
        return invalidNormalizedField(`${partParam}.image_url`, `${partParam}.image_url must be an object`);
      }
      const imageUnknown = findUnknownContentField(image, ["url", "detail"]);
      if (imageUnknown) {
        return invalidNormalizedField(
          `${partParam}.image_url.${imageUnknown}`,
          `Unknown image_url field: ${imageUnknown}`,
        );
      }
      if (typeof image.url !== "string" || !image.url.trim()) {
        return invalidNormalizedField(`${partParam}.image_url.url`, `${partParam}.image_url.url must contain a URL`);
      }
      const detail = parseImageDetail(image.detail, `${partParam}.image_url.detail`);
      if (!detail.ok) return detail;
      const breakpoint = normalizePromptCacheBreakpoint(
        part.prompt_cache_breakpoint,
        `${partParam}.prompt_cache_breakpoint`,
      );
      if (!breakpoint.ok) return breakpoint;
      const item: Extract<MessageContentItem, { type: "input_image" }> = detail.value === undefined
        ? { type: "input_image", image_url: image.url.trim() }
        : { type: "input_image", image_url: image.url.trim(), detail: detail.value };
      items.push(withPromptCacheBreakpoint(item, breakpoint.value));
      continue;
    }
    if (partType === "file") {
      const unknown = findUnknownContentField(part, ["type", "file", "prompt_cache_breakpoint"]);
      if (unknown) return invalidNormalizedField(`${partParam}.${unknown}`, `Unknown content field: ${unknown}`);
      if (role !== "user") {
        return invalidNormalizedField(`${partParam}.type`, `${partParam}.type is only valid for user messages`);
      }
      const file = isRecord(part.file) && !Array.isArray(part.file) ? part.file : null;
      if (!file) return invalidNormalizedField(`${partParam}.file`, `${partParam}.file must be an object`);
      const fileUnknown = findUnknownContentField(file, ["file_id", "file_data", "filename"]);
      if (fileUnknown) {
        return invalidNormalizedField(`${partParam}.file.${fileUnknown}`, `Unknown file field: ${fileUnknown}`);
      }
      if (file.file_id !== undefined && typeof file.file_id !== "string") {
        return invalidNormalizedField(`${partParam}.file.file_id`, `${partParam}.file.file_id must be a string`);
      }
      if (file.file_data !== undefined && typeof file.file_data !== "string") {
        return invalidNormalizedField(`${partParam}.file.file_data`, `${partParam}.file.file_data must be a string`);
      }
      if (file.filename !== undefined && typeof file.filename !== "string") {
        return invalidNormalizedField(`${partParam}.file.filename`, `${partParam}.file.filename must be a string`);
      }
      const fileId = typeof file.file_id === "string" ? file.file_id : undefined;
      const fileData = typeof file.file_data === "string" ? file.file_data : undefined;
      if (!fileId?.trim() && !fileData?.trim()) {
        return invalidNormalizedField(
          `${partParam}.file.file_id`,
          `${partParam}.file must include file_id or file_data`,
        );
      }
      const breakpoint = normalizePromptCacheBreakpoint(
        part.prompt_cache_breakpoint,
        `${partParam}.prompt_cache_breakpoint`,
      );
      if (!breakpoint.ok) return breakpoint;
      const item: Extract<MessageContentItem, { type: "input_file" }> = {
        type: "input_file",
        ...(fileId === undefined ? {} : { file_id: fileId }),
        ...(fileData === undefined ? {} : { file_data: fileData }),
        ...(file.filename === undefined ? {} : { filename: file.filename }),
      };
      items.push(withPromptCacheBreakpoint(item, breakpoint.value));
      continue;
    }
    if (partType === "input_audio" && Object.prototype.hasOwnProperty.call(part, "prompt_cache_breakpoint")) {
      return invalidNormalizedField(
        `${partParam}.prompt_cache_breakpoint`,
        "prompt_cache_breakpoint is not supported for input_audio content in this gateway",
      );
    }
    return invalidNormalizedField(`${partParam}.type`, `${partParam}.type is not supported`);
  }
  return { ok: true, value: items };
};

const messageContentToText = (items: MessageContentItem[]): string =>
  items
    .filter((item) => item.type === "input_text" || item.type === "output_text")
    .map((item) => item.text)
    .filter((text) => text && text.trim())
    .join("\n");

const chatRoleToCodexRole = (role: string): ResponseMessageItem["role"] | null => {
  if (role === "system") return "developer";
  if (role === "user") return "user";
  if (role === "assistant") return "assistant";
  if (role === "developer") return "developer";
  if (role === "tool") return "developer";
  return null;
};

const normalizeModelForCodex = (model: string): string => {
  return model.trim();
};

const normalizeUnixSeconds = (value: unknown): number | null => {
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  const seconds = Math.trunc(value);
  return seconds >= 0 ? seconds : null;
};

const normalizeModelEntry = (value: unknown, fallbackCreated: number): Record<string, unknown> | null => {
  if (!isRecord(value)) return null;
  const id = getString(value.id) ?? getString(value.slug) ?? getString(value.model) ?? getString(value.name);
  if (!id) return null;
  return {
    id,
    object: "model",
    created: normalizeUnixSeconds(value.created) ?? fallbackCreated,
    owned_by: getString(value.owned_by) ?? "openai",
  };
};

const normalizeModelList = (payload: unknown): { object: "list"; data: Record<string, unknown>[] } | null => {
  if (!isRecord(payload)) return null;
  const fallbackCreated = typeof payload.updated_at_ms === "number" && Number.isFinite(payload.updated_at_ms)
    ? Math.max(0, Math.trunc(payload.updated_at_ms / 1000))
    : 0;
  const data = Array.isArray(payload.data) ? payload.data : null;
  if (data) {
    const normalized = data.map((entry) => normalizeModelEntry(entry, fallbackCreated)).filter(Boolean) as Record<
      string,
      unknown
    >[];
    return { object: "list", data: normalized };
  }
  const models = Array.isArray(payload.models) ? payload.models : null;
  if (models) {
    const normalized = models.map((entry) => normalizeModelEntry(entry, fallbackCreated)).filter(Boolean) as Record<
      string,
      unknown
    >[];
    return { object: "list", data: normalized };
  }
  return null;
};

const configuredCerebrasModel = (): Record<string, unknown> | null =>
  readCerebrasApiKey()
    ? {
      id: CEREBRAS_GPT_OSS_120B_MODEL,
      object: "model",
      created: 0,
      owned_by: "cerebras",
    }
    : null;

const configuredCerebrasModelCapabilities = (): Record<string, unknown> | null =>
  readCerebrasApiKey()
    ? {
      id: CEREBRAS_GPT_OSS_120B_MODEL,
      object: "uos.model_capabilities",
      owned_by: "cerebras",
      display_name: "GPT-OSS 120B",
      upstream_provider: "cerebras",
      supported_endpoints: ["/v1/chat/completions"],
      supported_reasoning_levels: ["medium"],
      default_reasoning_effort: "medium",
      reasoning_effort_wire_map: {},
      context_window_tokens: null,
      max_context_window_tokens: null,
      auto_compact_token_limit_tokens: null,
    }
    : null;

const withConfiguredCerebrasModel = (
  models: readonly Record<string, unknown>[],
): Record<string, unknown>[] => {
  const cerebras = configuredCerebrasModel();
  if (!cerebras || models.some((model) => model.id === CEREBRAS_GPT_OSS_120B_MODEL)) {
    return [...models];
  }
  return [...models, cerebras];
};

const normalizeModelCapabilitiesEntry = (value: unknown): Record<string, unknown> | null => {
  if (!isRecord(value)) return null;
  const id = modelIdFromSnapshotRecord(value);
  if (!id) return null;
  const reasoning = getCodexModelReasoning(value);
  const promptCache = normalizePromptCacheCapabilities(value.prompt_cache);
  return {
    id,
    object: "uos.model_capabilities",
    owned_by: getString(value.owned_by) ?? "openai",
    display_name: getString(value.display_name),
    upstream_provider: "codex_chatgpt",
    supported_endpoints: ["/v1/chat/completions", "/v1/responses"],
    supported_reasoning_levels: reasoning.levels,
    default_reasoning_effort: reasoning.defaultLevel,
    reasoning_effort_wire_map: Object.fromEntries(reasoning.wireEfforts),
    context_window_tokens: normalizeTokenCount(value.context_window),
    max_context_window_tokens: normalizeTokenCount(value.max_context_window),
    auto_compact_token_limit_tokens: normalizeTokenCount(value.auto_compact_token_limit),
    ...(promptCache !== null ? { prompt_cache: promptCache } : {}),
  };
};

const normalizeResponseContentItem = (
  value: unknown,
  param: string,
  role: ResponseMessageItem["role"],
): NormalizationResult<MessageContentItem> => {
  if (!isRecord(value) || Array.isArray(value)) {
    return invalidNormalizedField(param, `${param} must be an object`);
  }
  const partType = getString(value.type);
  if (!partType) return invalidNormalizedField(`${param}.type`, `${param}.type must be a string`);

  if (partType === "input_text" || partType === "output_text") {
    if (partType === "output_text" && role !== "assistant") {
      return invalidNormalizedField(`${param}.type`, `${param}.type is only valid for assistant messages`);
    }
    const unknown = findUnknownContentField(
      value,
      partType === "output_text" ? ["type", "text", "annotations"] : ["type", "text", "prompt_cache_breakpoint"],
    );
    if (unknown) return invalidNormalizedField(`${param}.${unknown}`, `Unknown content field: ${unknown}`);
    if (typeof value.text !== "string") {
      return invalidNormalizedField(`${param}.text`, `${param}.text must be a string`);
    }
    if (partType === "output_text" && value.annotations !== undefined && !Array.isArray(value.annotations)) {
      return invalidNormalizedField(`${param}.annotations`, `${param}.annotations must be an array`);
    }
    if (partType === "output_text") return { ok: true, value: { type: partType, text: value.text } };
    const breakpoint = normalizePromptCacheBreakpoint(
      value.prompt_cache_breakpoint,
      `${param}.prompt_cache_breakpoint`,
    );
    if (!breakpoint.ok) return breakpoint;
    return {
      ok: true,
      value: withPromptCacheBreakpoint({ type: "input_text", text: value.text }, breakpoint.value),
    };
  }

  if (partType === "input_image") {
    const unknown = findUnknownContentField(value, [
      "type",
      "image_url",
      "file_id",
      "detail",
      "prompt_cache_breakpoint",
    ]);
    if (unknown) return invalidNormalizedField(`${param}.${unknown}`, `Unknown content field: ${unknown}`);
    const imageUrl = getString(value.image_url)?.trim() ?? "";
    const fileId = getString(value.file_id)?.trim() ?? "";
    if ((imageUrl && fileId) || (!imageUrl && !fileId)) {
      return invalidNormalizedField(`${param}.image_url`, `${param} must include exactly one of image_url or file_id`);
    }
    if (value.image_url !== undefined && typeof value.image_url !== "string") {
      return invalidNormalizedField(`${param}.image_url`, `${param}.image_url must be a string`);
    }
    if (value.file_id !== undefined && typeof value.file_id !== "string") {
      return invalidNormalizedField(`${param}.file_id`, `${param}.file_id must be a string`);
    }
    const detail = parseImageDetail(value.detail, `${param}.detail`);
    if (!detail.ok) return detail;
    const breakpoint = normalizePromptCacheBreakpoint(
      value.prompt_cache_breakpoint,
      `${param}.prompt_cache_breakpoint`,
    );
    if (!breakpoint.ok) return breakpoint;
    const item: Extract<MessageContentItem, { type: "input_image" }> = detail.value === undefined
      ? imageUrl
        ? { type: "input_image" as const, image_url: imageUrl }
        : { type: "input_image" as const, file_id: fileId }
      : imageUrl
      ? { type: "input_image" as const, image_url: imageUrl, detail: detail.value }
      : { type: "input_image" as const, file_id: fileId, detail: detail.value };
    return { ok: true, value: withPromptCacheBreakpoint(item, breakpoint.value) };
  }

  if (partType === "input_file") {
    const unknown = findUnknownContentField(value, [
      "type",
      "file_id",
      "file_data",
      "file_url",
      "filename",
      "detail",
      "prompt_cache_breakpoint",
    ]);
    if (unknown) return invalidNormalizedField(`${param}.${unknown}`, `Unknown content field: ${unknown}`);
    const fields = ["file_id", "file_data", "file_url"] as const;
    const present = fields.filter((field) => typeof value[field] === "string" && value[field].trim());
    if (!present.length) {
      return invalidNormalizedField(`${param}.file_id`, `${param} must include file_id, file_data, or file_url`);
    }
    for (const field of fields) {
      if (value[field] !== undefined && typeof value[field] !== "string") {
        return invalidNormalizedField(`${param}.${field}`, `${param}.${field} must be a string`);
      }
    }
    if (value.filename !== undefined && value.filename !== null && typeof value.filename !== "string") {
      return invalidNormalizedField(`${param}.filename`, `${param}.filename must be a string or null`);
    }
    const detail = parseInputFileDetail(value.detail, `${param}.detail`);
    if (!detail.ok) return detail;
    const breakpoint = normalizePromptCacheBreakpoint(
      value.prompt_cache_breakpoint,
      `${param}.prompt_cache_breakpoint`,
    );
    if (!breakpoint.ok) return breakpoint;
    const item: {
      type: "input_file";
      file_id?: string;
      file_data?: string;
      file_url?: string;
      filename?: string | null;
      detail?: InputFileDetail;
    } = { type: "input_file" };
    for (const field of fields) {
      const fieldValue = getString(value[field]);
      if (fieldValue) item[field] = fieldValue;
    }
    if (Object.prototype.hasOwnProperty.call(value, "filename")) item.filename = value.filename as string | null;
    if (detail.value !== undefined) item.detail = detail.value;
    return { ok: true, value: withPromptCacheBreakpoint(item, breakpoint.value) };
  }

  return invalidNormalizedField(`${param}.type`, `${param}.type is not supported`);
};

const normalizeResponseMessageItem = (
  value: unknown,
  param: string,
): NormalizationResult<ResponseMessageItem> => {
  if (!isRecord(value) || Array.isArray(value)) return invalidNormalizedField(param, `${param} must be an object`);
  if (Object.prototype.hasOwnProperty.call(value, "prompt_cache_breakpoint")) {
    return invalidNormalizedField(
      `${param}.prompt_cache_breakpoint`,
      "prompt_cache_breakpoint is only valid on supported input content blocks",
    );
  }
  if (Object.prototype.hasOwnProperty.call(value, "type") && value.type !== "message") {
    return invalidNormalizedField(`${param}.type`, `${param}.type must be message`);
  }
  const roleRaw = getString(value.role);
  // Native Responses tool output is a top-level function_call_output item;
  // do not silently reinterpret a message role:"tool" as developer text.
  const role = roleRaw && roleRaw !== "tool" ? chatRoleToCodexRole(roleRaw) : null;
  if (!role) return invalidNormalizedField(`${param}.role`, `${param}.role is invalid`);
  const content = value.content;
  if (typeof content === "string") {
    return {
      ok: true,
      value: {
        type: "message",
        role,
        content: [{ type: role === "assistant" ? "output_text" : "input_text", text: content }],
      },
    };
  }
  if (!Array.isArray(content)) {
    return invalidNormalizedField(`${param}.content`, `${param}.content must be a string or an array`);
  }
  const items: MessageContentItem[] = [];
  for (const [index, part] of content.entries()) {
    const normalized = normalizeResponseContentItem(part, `${param}.content[${index}]`, role);
    if (!normalized.ok) return normalized;
    items.push(normalized.value);
  }
  return { ok: true, value: { type: "message", role, content: items } };
};

/**
 * Responses permits a function-call result to carry the same input content
 * blocks as a message. Normalize that known standard shape so cache
 * breakpoints are neither passed through unchecked nor omitted from telemetry.
 * Other Codex Responses extension items remain opaque passthrough values.
 */
const normalizeFunctionCallOutputItem = (
  value: Record<string, unknown>,
  param: string,
): NormalizationResult<ResponseInputItem> => {
  if (value.type !== "function_call_output" || !Array.isArray(value.output)) {
    return { ok: true, value: value as ResponseInputItem };
  }
  const output: MessageContentItem[] = [];
  for (const [index, content] of value.output.entries()) {
    const normalized = normalizeResponseContentItem(content, `${param}.output[${index}]`, "user");
    if (!normalized.ok) return normalized;
    output.push(normalized.value);
  }
  return { ok: true, value: { ...value, type: "function_call_output", output } };
};

const normalizeChatToolCall = (
  value: unknown,
  param: string,
): NormalizationResult<Readonly<Record<string, unknown> & { type: "function_call" }>> => {
  if (!isRecord(value) || Array.isArray(value)) return invalidNormalizedField(param, `${param} must be an object`);
  const unknownField = findUnknownContentField(value, ["id", "type", "function"]);
  if (unknownField) {
    return invalidNormalizedField(`${param}.${unknownField}`, `Unknown tool call field: ${unknownField}`);
  }
  if (value.type !== "function") {
    return invalidNormalizedField(`${param}.type`, `${param}.type must be function`);
  }
  const callId = getString(value.id)?.trim();
  if (!callId) return invalidNormalizedField(`${param}.id`, `${param}.id must be a non-empty string`);
  if (!isRecord(value.function) || Array.isArray(value.function)) {
    return invalidNormalizedField(`${param}.function`, `${param}.function must be an object`);
  }
  const unknownFunctionField = findUnknownContentField(value.function, ["name", "arguments"]);
  if (unknownFunctionField) {
    return invalidNormalizedField(
      `${param}.function.${unknownFunctionField}`,
      `Unknown tool call function field: ${unknownFunctionField}`,
    );
  }
  const name = getString(value.function.name)?.trim();
  if (!name) {
    return invalidNormalizedField(`${param}.function.name`, `${param}.function.name must be a non-empty string`);
  }
  if (typeof value.function.arguments !== "string") {
    return invalidNormalizedField(
      `${param}.function.arguments`,
      `${param}.function.arguments must be a string`,
    );
  }
  // Arguments are an opaque JSON string in the Chat contract. Do not parse,
  // validate, or reserialize them: callers rely on byte-for-byte fidelity.
  return {
    ok: true,
    value: {
      type: "function_call",
      call_id: callId,
      name,
      arguments: value.function.arguments,
    },
  };
};

const normalizeChatToolOutput = (
  value: unknown,
  param: string,
): NormalizationResult<string | Array<Extract<MessageContentItem, { type: "input_text" }>>> => {
  if (typeof value === "string") return { ok: true, value };
  if (!Array.isArray(value)) return invalidNormalizedField(param, `${param} must be a string or an array`);
  const output: Array<Extract<MessageContentItem, { type: "input_text" }>> = [];
  for (const [index, part] of value.entries()) {
    const partParam = `${param}[${index}]`;
    if (!isRecord(part) || Array.isArray(part)) {
      return invalidNormalizedField(partParam, `${partParam} must be an object`);
    }
    const type = getString(part.type);
    if (type !== "text") {
      return invalidNormalizedField(`${partParam}.type`, `${partParam}.type must be a text content part`);
    }
    const unknown = findUnknownContentField(part, ["type", "text", "prompt_cache_breakpoint"]);
    if (unknown) return invalidNormalizedField(`${partParam}.${unknown}`, `Unknown content field: ${unknown}`);
    if (typeof part.text !== "string") {
      return invalidNormalizedField(`${partParam}.text`, `${partParam}.text must be a string`);
    }
    const breakpoint = normalizePromptCacheBreakpoint(
      part.prompt_cache_breakpoint,
      `${partParam}.prompt_cache_breakpoint`,
    );
    if (!breakpoint.ok) return breakpoint;
    output.push(withPromptCacheBreakpoint({ type: "input_text", text: part.text }, breakpoint.value));
  }
  return { ok: true, value: output };
};

const normalizeChatMessage = (
  value: unknown,
  index: number,
): NormalizationResult<
  Readonly<{ instruction: string | null; instructionContent: MessageContentItem[] | null; input: ResponseInputItem[] }>
> => {
  const param = `messages[${index}]`;
  if (!isRecord(value) || Array.isArray(value)) return invalidNormalizedField(param, `${param} must be an object`);
  if (Object.prototype.hasOwnProperty.call(value, "prompt_cache_breakpoint")) {
    return invalidNormalizedField(
      `${param}.prompt_cache_breakpoint`,
      "prompt_cache_breakpoint is only valid on supported input content blocks",
    );
  }
  const roleRaw = getString(value.role);
  if (!roleRaw) return invalidNormalizedField(`${param}.role`, `${param}.role must be a string`);
  const role = chatRoleToCodexRole(roleRaw);
  if (!role) return invalidNormalizedField(`${param}.role`, `${param}.role is not supported`);

  if (roleRaw === "tool") {
    if (Object.prototype.hasOwnProperty.call(value, "tool_calls")) {
      return invalidNormalizedField(`${param}.tool_calls`, "tool_calls are only valid for assistant messages");
    }
    const callId = getString(value.tool_call_id)?.trim();
    if (!callId) {
      return invalidNormalizedField(`${param}.tool_call_id`, `${param}.tool_call_id must be a non-empty string`);
    }
    const output = normalizeChatToolOutput(value.content, `${param}.content`);
    if (!output.ok) return output;
    return {
      ok: true,
      value: {
        instruction: null,
        instructionContent: null,
        input: [{ type: "function_call_output", call_id: callId, output: output.value }],
      },
    };
  }

  if (Object.prototype.hasOwnProperty.call(value, "tool_call_id")) {
    return invalidNormalizedField(`${param}.tool_call_id`, "tool_call_id is only valid for tool messages");
  }
  if (roleRaw === "assistant") {
    const hasToolCalls = Object.prototype.hasOwnProperty.call(value, "tool_calls");
    // Chat permits an omitted assistant content field when the message is
    // solely a function-call turn. Normalize it as the same empty content as
    // the explicit null form, but keep missing content invalid otherwise.
    const content = value.content === undefined && hasToolCalls
      ? { ok: true as const, value: [] as MessageContentItem[] }
      : normalizeChatContentItems(role, value.content, `${param}.content`);
    if (!content.ok) return content;
    const input: ResponseInputItem[] = [];
    // A Chat assistant's natural-language output must precede its function
    // calls so a multi-turn tool conversation retains the original order.
    if (content.value.length) input.push({ type: "message", role, content: content.value });
    if (hasToolCalls) {
      if (!Array.isArray(value.tool_calls)) {
        return invalidNormalizedField(`${param}.tool_calls`, `${param}.tool_calls must be an array`);
      }
      for (const [callIndex, call] of value.tool_calls.entries()) {
        const normalized = normalizeChatToolCall(call, `${param}.tool_calls[${callIndex}]`);
        if (!normalized.ok) return normalized;
        input.push(normalized.value);
      }
    }
    if (!input.length) {
      return invalidNormalizedField(`${param}.content`, "assistant messages require content or tool_calls");
    }
    return { ok: true, value: { instruction: null, instructionContent: null, input } };
  }

  const content = normalizeChatContentItems(role, value.content, `${param}.content`);
  if (!content.ok) return content;

  if (roleRaw === "system" || roleRaw === "developer") {
    if (Object.prototype.hasOwnProperty.call(value, "tool_calls")) {
      return invalidNormalizedField(`${param}.tool_calls`, "tool_calls are only valid for assistant messages");
    }
    return {
      ok: true,
      value: { instruction: messageContentToText(content.value), instructionContent: content.value, input: [] },
    };
  }

  if (Object.prototype.hasOwnProperty.call(value, "tool_calls")) {
    return invalidNormalizedField(`${param}.tool_calls`, "tool_calls are only valid for assistant messages");
  }
  return {
    ok: true,
    value: { instruction: null, instructionContent: null, input: [{ type: "message", role, content: content.value }] },
  };
};

const recordResponsesTerminal = (
  event: ResponsesStreamEvent,
  usageContext?: UsageContext,
): void => {
  if (!event.terminal) return;
  recordStreamTerminalType(usageContext, event.type as ResponseStreamTerminalType);
  const usage = isRecord(event.value.response) ? extractUsageTokens(event.value.response.usage) : null;
  if (event.type === "response.completed") void recordCompletionUsage(usageContext, usage);
  else recordTerminalUsage(usageContext, usage, false);
};

const responseHasOutputText = (output: unknown): boolean => {
  if (!Array.isArray(output)) return false;
  for (const item of output) {
    if (!isRecord(item) || !Array.isArray(item.content)) continue;
    for (const contentItem of item.content) {
      if (!isRecord(contentItem)) continue;
      if (getString(contentItem.type) === "output_text" && (getString(contentItem.text) ?? "").length > 0) {
        return true;
      }
    }
  }
  return false;
};

/**
 * Buffered and streamed Responses transports normally emit output_text.delta
 * events.  Some compatible upstreams only provide the completed output item,
 * however, so recover that text without turning a mixed text/tool result into
 * a tool-call-only Chat completion.
 */
const responseOutputText = (output: unknown): string => {
  if (!Array.isArray(output)) return "";
  let text = "";
  for (const item of output) {
    if (!isRecord(item) || !Array.isArray(item.content)) continue;
    for (const contentItem of item.content) {
      if (!isRecord(contentItem)) continue;
      const type = getString(contentItem.type);
      if (type !== "output_text" && type !== "text") continue;
      const value = getString(contentItem.text);
      if (value !== null) text += value;
    }
  }
  return text;
};

const reconcileResponseOutputText = (emittedText: string, output: unknown): string => {
  const finalText = responseOutputText(output);
  if (!finalText || finalText === emittedText || emittedText.startsWith(finalText)) return "";
  if (finalText.startsWith(emittedText)) return finalText.slice(emittedText.length);
  return malformedFunctionCallStream("Upstream response output text conflicts with prior text deltas.");
};

const withAccumulatedResponseText = (response: Record<string, unknown>, text: string): Record<string, unknown> => {
  if (!text || responseHasOutputText(response.output)) return response;
  const output = Array.isArray(response.output) ? [...response.output] : [];
  output.push({
    id: `msg_${crypto.randomUUID().replace(/-/g, "")}`,
    type: "message",
    status: "completed",
    role: "assistant",
    content: [{ type: "output_text", text, annotations: [] }],
  });
  return { ...response, output };
};

const withAccumulatedResponseItems = (
  response: Record<string, unknown>,
  accumulated: Record<string, unknown>[],
): Record<string, unknown> => {
  if (!accumulated.length) return response;
  const output = Array.isArray(response.output) ? response.output.filter(isRecord).map((item) => ({ ...item })) : [];
  const existingIds = new Set(output.map((item) => getString(item.id)).filter(Boolean));
  for (const item of accumulated) {
    const id = getString(item.id);
    if (id && existingIds.has(id)) continue;
    output.push(item);
    if (id) existingIds.add(id);
  }
  return { ...response, output };
};

type ChatFunctionCall = {
  key: string;
  index: number;
  callId: string;
  name: string;
  arguments: string;
  argumentsDone: boolean;
};

const malformedFunctionCallStream = (message: string): never => {
  throw new ResponsesStreamError(message, { kind: "malformed_event" });
};

/**
 * Reconciles Responses function-call events into the Chat Completions shape.
 * Both buffered and SSE translations use this one accumulator so a final
 * output item cannot duplicate arguments already emitted as deltas.
 */
class ChatFunctionCallAccumulator {
  #calls: ChatFunctionCall[] = [];
  #byKey = new Map<string, ChatFunctionCall>();

  get hasCalls(): boolean {
    return this.#calls.length > 0;
  }

  get calls(): readonly ChatFunctionCall[] {
    return this.#calls;
  }

  assertFinalized(): void {
    const unfinished = this.#calls.find((call) => !call.argumentsDone);
    if (unfinished) {
      return malformedFunctionCallStream(
        "Upstream function-call stream ended before finalized arguments were received.",
      );
    }
  }

  has(event: Record<string, unknown>, item?: Record<string, unknown>): boolean {
    const key = this.#key(event, item);
    return Boolean(key && this.#byKey.has(key));
  }

  #key(event: Record<string, unknown>, item?: Record<string, unknown>): string | null {
    const itemId = getString(event.item_id) ?? getString(item?.id);
    if (itemId?.trim()) return `item:${itemId}`;
    const outputIndex = event.output_index;
    if (typeof outputIndex === "number" && Number.isInteger(outputIndex) && outputIndex >= 0) {
      return `output:${outputIndex}`;
    }
    return null;
  }

  #create(event: Record<string, unknown>, item: Record<string, unknown>): ChatFunctionCall {
    if (getString(item.type) !== "function_call") {
      return malformedFunctionCallStream("Upstream function-call event did not contain a function_call item.");
    }
    const key = this.#key(event, item);
    if (!key) return malformedFunctionCallStream("Upstream function-call event omitted item_id and output_index.");
    const callId = getString(item.call_id)?.trim();
    const name = getString(item.name)?.trim();
    // Added items may omit arguments because the argument stream follows.
    const argumentsText = item.arguments === undefined ? "" : getString(item.arguments);
    if (!callId || !name || argumentsText === null) {
      return malformedFunctionCallStream("Upstream function-call item is missing call_id, name, or string arguments.");
    }
    const existing = this.#byKey.get(key);
    if (existing) {
      if (existing.callId !== callId || existing.name !== name) {
        return malformedFunctionCallStream("Upstream function-call item changed its call_id or name.");
      }
      this.#reconcileArguments(existing, argumentsText);
      return existing;
    }
    const call: ChatFunctionCall = {
      key,
      index: this.#calls.length,
      callId,
      name,
      arguments: argumentsText,
      argumentsDone: false,
    };
    this.#calls.push(call);
    this.#byKey.set(key, call);
    return call;
  }

  #reconcileArguments(call: ChatFunctionCall, finalArguments: string): string {
    if (finalArguments === call.arguments) return "";
    if (!finalArguments.startsWith(call.arguments)) {
      return malformedFunctionCallStream("Upstream function-call arguments conflict with prior argument deltas.");
    }
    const suffix = finalArguments.slice(call.arguments.length);
    call.arguments = finalArguments;
    return suffix;
  }

  add(
    event: Record<string, unknown>,
    item: unknown,
  ): Readonly<{ call: ChatFunctionCall; includeIdentity: boolean; suffix: string }> | null {
    if (!isRecord(item) || Array.isArray(item) || getString(item.type) !== "function_call") return null;
    const existing = this.#byKey.get(this.#key(event, item) ?? "");
    const priorArguments = existing?.arguments;
    const call = this.#create(event, item);
    return {
      call,
      includeIdentity: !existing,
      suffix: priorArguments === undefined ? call.arguments : call.arguments.slice(priorArguments.length),
    };
  }

  delta(event: Record<string, unknown>): Readonly<{ call: ChatFunctionCall; delta: string }> {
    const key = this.#key(event);
    const call = key ? this.#byKey.get(key) : undefined;
    if (!call) return malformedFunctionCallStream("Upstream function-call argument delta has no matching item.");
    if (call.argumentsDone) {
      return malformedFunctionCallStream("Upstream function-call emitted arguments after its completion event.");
    }
    const delta = getString(event.delta);
    if (delta === null) return malformedFunctionCallStream("Upstream function-call argument delta is not a string.");
    call.arguments += delta;
    return { call, delta };
  }

  done(event: Record<string, unknown>): Readonly<{ call: ChatFunctionCall; suffix: string }> {
    const key = this.#key(event);
    const call = key ? this.#byKey.get(key) : undefined;
    if (!call) return malformedFunctionCallStream("Upstream function-call completion has no matching item.");
    const finalArguments = getString(event.arguments);
    if (finalArguments === null) {
      return malformedFunctionCallStream("Upstream function-call completion is missing string arguments.");
    }
    if (call.argumentsDone) {
      if (finalArguments !== call.arguments) {
        return malformedFunctionCallStream("Upstream function-call completion changed finalized arguments.");
      }
      return { call, suffix: "" };
    }
    const suffix = this.#reconcileArguments(call, finalArguments);
    call.argumentsDone = true;
    return { call, suffix };
  }

  reconcileItem(
    event: Record<string, unknown>,
    item: unknown,
  ): Readonly<{ call: ChatFunctionCall; suffix: string }> | null {
    if (!isRecord(item) || Array.isArray(item) || getString(item.type) !== "function_call") return null;
    const key = this.#key(event, item);
    const existing = key ? this.#byKey.get(key) : undefined;
    // An item that is done or appears in final output, on the other hand,
    // must carry a concrete arguments string. Accepting a missing value would
    // emit a successful terminal for a malformed upstream function call.
    const argumentsText = getString(item.arguments);
    if (argumentsText === null) {
      return malformedFunctionCallStream("Upstream function-call item is missing string arguments.");
    }
    if (!existing) {
      const created = this.#create(event, item);
      created.argumentsDone = true;
      return { call: created, suffix: created.arguments };
    }
    if (existing.callId !== getString(item.call_id)?.trim() || existing.name !== getString(item.name)?.trim()) {
      return malformedFunctionCallStream("Upstream function-call item changed its call_id or name.");
    }
    if (existing.argumentsDone) {
      if (argumentsText !== existing.arguments) {
        return malformedFunctionCallStream("Upstream function-call item changed finalized arguments.");
      }
      return { call: existing, suffix: "" };
    }
    const suffix = this.#reconcileArguments(existing, argumentsText);
    existing.argumentsDone = true;
    return { call: existing, suffix };
  }

  reconcileOutput(
    event: Record<string, unknown>,
    output: unknown,
  ): Array<Readonly<{ call: ChatFunctionCall; suffix: string }>> {
    if (!Array.isArray(output)) return [];
    const reconciled: Array<Readonly<{ call: ChatFunctionCall; suffix: string }>> = [];
    for (const item of output) {
      const result = this.reconcileItem(event, item);
      if (result) reconciled.push(result);
    }
    return reconciled;
  }
}

const chatToolCallDelta = (
  call: ChatFunctionCall,
  options: Readonly<{ includeIdentity: boolean; argumentsDelta?: string }> = { includeIdentity: false },
): Record<string, unknown> => {
  const fn: Record<string, unknown> = {};
  if (options.includeIdentity) fn.name = call.name;
  if (options.argumentsDelta !== undefined) fn.arguments = options.argumentsDelta;
  const value: Record<string, unknown> = { index: call.index, function: fn };
  if (options.includeIdentity) {
    value.id = call.callId;
    value.type = "function";
  }
  return value;
};

const streamChatCompletions = (
  source: PreflightedResponsesStream,
  model: string,
  includeUsage: boolean,
  usageContext: UsageContext | undefined,
  provider: UpstreamProvider,
  lifecycle: YunwuTransportLifecycle,
  signal: AbortSignal,
  downstreamSignal: AbortSignal,
  onResponseTerminal?: (completed: boolean) => void,
): Response => {
  const encoder = new TextEncoder();
  const iterator = source.iterator;
  let pending: ResponsesStreamEvent | undefined = source.first;
  let id = `chatcmpl_${crypto.randomUUID().replace(/-/g, "")}`;
  let created = Math.floor(Date.now() / 1000);
  let sentRole = false;
  let closed = false;
  let outputText = "";
  const functionCalls = new ChatFunctionCallAccumulator();
  const queuedDeltas: Array<
    | Readonly<{ kind: "content"; content: string }>
    | Readonly<{
      kind: "tool";
      call: ChatFunctionCall;
      includeIdentity: boolean;
      argumentsDelta: string;
    }>
  > = [];
  const stream = new ReadableStream<Uint8Array>({
    async pull(controller) {
      if (closed) return;
      try {
        const emitContent = (content: string): void => {
          const chunk: Record<string, unknown> = {
            id,
            object: "chat.completion.chunk",
            created,
            model,
            choices: [
              {
                index: 0,
                delta: sentRole ? { content } : { role: "assistant", content },
                finish_reason: null,
              },
            ],
          };
          sentRole = true;
          controller.enqueue(encoder.encode(`data: ${JSON.stringify(chunk)}\n\n`));
        };
        const emitToolCall = (
          call: ChatFunctionCall,
          includeIdentity: boolean,
          argumentsDelta: string | undefined,
        ): void => {
          const toolCall = chatToolCallDelta(call, { includeIdentity, argumentsDelta });
          const delta: Record<string, unknown> = sentRole
            ? { tool_calls: [toolCall] }
            : { role: "assistant", tool_calls: [toolCall] };
          const chunk: Record<string, unknown> = {
            id,
            object: "chat.completion.chunk",
            created,
            model,
            choices: [{ index: 0, delta, finish_reason: null }],
          };
          sentRole = true;
          controller.enqueue(encoder.encode(`data: ${JSON.stringify(chunk)}\n\n`));
        };
        const queueFinalOutput = (event: Record<string, unknown>, output: unknown): void => {
          const textSuffix = reconcileResponseOutputText(outputText, output);
          if (textSuffix) {
            outputText += textSuffix;
            queuedDeltas.push({ kind: "content", content: textSuffix });
          }
          const beforeCount = functionCalls.calls.length;
          const reconciled = functionCalls.reconcileOutput(event, output);
          for (const result of reconciled) {
            const includeIdentity = result.call.index >= beforeCount;
            if (includeIdentity || result.suffix) {
              queuedDeltas.push({
                kind: "tool",
                call: result.call,
                includeIdentity,
                argumentsDelta: result.suffix,
              });
            }
          }
        };
        const queued = queuedDeltas.shift();
        if (queued) {
          if (queued.kind === "content") emitContent(queued.content);
          else emitToolCall(queued.call, queued.includeIdentity, queued.argumentsDelta);
          return;
        }
        while (!closed) {
          const next = pending ? { done: false as const, value: pending } : await iterator.next();
          pending = undefined;
          if (next.done) {
            throw new ResponsesStreamError("Upstream Responses stream ended before a terminal event.", {
              kind: "premature_eof",
            });
          }
          const event = next.value;
          const ev = event.value;
          const type = event.type;
          if (type === "response.created" && isRecord(ev.response)) {
            const upstreamId = getString(ev.response.id);
            const createdAt = typeof ev.response.created_at === "number" ? ev.response.created_at : null;
            if (upstreamId) id = upstreamId;
            if (createdAt) created = createdAt;
            continue;
          }

          if (type === "response.output_text.delta") {
            const delta = getString(ev.delta);
            if (delta === null) {
              return malformedFunctionCallStream("Upstream output-text delta is not a string.");
            }
            outputText += delta;
            emitContent(delta);
            return;
          }

          if (type === "response.output_item.added") {
            const added = functionCalls.add(ev, ev.item);
            if (added && (added.includeIdentity || added.suffix)) {
              emitToolCall(added.call, added.includeIdentity, added.suffix);
              return;
            }
            continue;
          }

          if (type === "response.function_call_arguments.delta") {
            const { call, delta } = functionCalls.delta(ev);
            emitToolCall(call, false, delta);
            return;
          }

          if (type === "response.function_call_arguments.done") {
            const { call, suffix } = functionCalls.done(ev);
            if (suffix) {
              emitToolCall(call, false, suffix);
              return;
            }
            continue;
          }

          if (type === "response.output_item.done") {
            const wasKnown = isRecord(ev.item) && !Array.isArray(ev.item) && functionCalls.has(ev, ev.item);
            const reconciled = functionCalls.reconcileItem(ev, ev.item);
            if (reconciled) {
              if (!wasKnown || reconciled.suffix) {
                emitToolCall(reconciled.call, !wasKnown, reconciled.suffix);
                return;
              }
            } else {
              const textSuffix = reconcileResponseOutputText(outputText, [ev.item]);
              if (textSuffix) {
                outputText += textSuffix;
                emitContent(textSuffix);
                return;
              }
            }
            continue;
          }

          if (type === "response.output") {
            const output = ev.output ?? (isRecord(ev.response) ? ev.response.output : undefined);
            queueFinalOutput(ev, output);
            if (queuedDeltas.length) {
              pending = event;
              const queued = queuedDeltas.shift()!;
              if (queued.kind === "content") emitContent(queued.content);
              else emitToolCall(queued.call, queued.includeIdentity, queued.argumentsDelta);
              return;
            }
            continue;
          }

          if (type === "response.completed") {
            if (!isRecord(ev.response) || Array.isArray(ev.response)) {
              return malformedFunctionCallStream("Upstream response.completed event is missing its response object.");
            }
            const output = ev.response.output;
            queueFinalOutput(ev, output);
            functionCalls.assertFinalized();
            if (queuedDeltas.length) {
              pending = event;
              const queued = queuedDeltas.shift()!;
              if (queued.kind === "content") emitContent(queued.content);
              else emitToolCall(queued.call, queued.includeIdentity, queued.argumentsDelta);
              return;
            }
            onResponseTerminal?.(true);
            lifecycle.terminal(type);
            recordStreamTerminalType(usageContext, "response.completed");
            const usageTokens = isRecord(ev.response) ? extractUsageTokens(ev.response.usage) : null;
            void recordCompletionUsage(usageContext, usageTokens);
            const chunk: Record<string, unknown> = {
              id,
              object: "chat.completion.chunk",
              created,
              model,
              choices: [
                {
                  index: 0,
                  delta: sentRole ? {} : { role: "assistant" },
                  finish_reason: functionCalls.hasCalls ? "tool_calls" : "stop",
                },
              ],
            };
            controller.enqueue(encoder.encode(`data: ${JSON.stringify(chunk)}\n\n`));
            const usage = toChatUsage(usageTokens);
            if (includeUsage && usage !== null) {
              controller.enqueue(
                encoder.encode(
                  `data: ${
                    JSON.stringify({ id, object: "chat.completion.chunk", created, model, choices: [], usage })
                  }\n\n`,
                ),
              );
            }
            controller.enqueue(encoder.encode("data: [DONE]\n\n"));
            closed = true;
            controller.close();
            void iterator.return("Responses terminal event translated").catch(() => {});
            return;
          }
          if (event.terminal) {
            onResponseTerminal?.(false);
            lifecycle.terminal(type);
            recordStreamTerminalType(usageContext, type as ResponseStreamTerminalType);
            const usageTokens = isRecord(ev.response) ? extractUsageTokens(ev.response.usage) : null;
            recordTerminalUsage(usageContext, usageTokens, false);
            const errorValue = {
              error: {
                message: `Upstream terminated with ${type}.`,
                type: "server_error",
                code: "upstream_stream_error",
              },
            };
            controller.enqueue(encoder.encode(`data: ${JSON.stringify(errorValue)}\n\n`));
            closed = true;
            controller.close();
            void iterator.return("Responses terminal error translated").catch(() => {});
            return;
          }
        }
      } catch (error) {
        if (closed) return;
        onResponseTerminal?.(false);
        const terminalType = classifyStreamFailure(error, signal, downstreamSignal);
        recordStreamTerminalType(usageContext, terminalType);
        if (terminalType === "cancelled") lifecycle.cancelled();
        else lifecycle.ambiguous();
        void recordErrorUsage(usageContext);
        const errorValue = {
          error: {
            message: "The upstream stream ended unexpectedly.",
            type: "server_error",
            code: "upstream_stream_error",
          },
        };
        if (!downstreamSignal.aborted) {
          controller.enqueue(encoder.encode(`data: ${JSON.stringify(errorValue)}\n\n`));
        }
        closed = true;
        controller.close();
      }
    },
    async cancel(reason) {
      if (closed) return;
      closed = true;
      onResponseTerminal?.(false);
      recordStreamTerminalType(usageContext, "cancelled");
      lifecycle.cancelled();
      void recordErrorUsage(usageContext);
      await source.cancel(reason);
    },
  });

  return new Response(withSseKeepalive(stream), {
    status: 200,
    headers: {
      "Content-Type": "text/event-stream",
      "Cache-Control": "no-cache",
      "x-uos-upstream": provider,
    },
  });
};

const completeChatCompletions = async (
  source: PreflightedResponsesStream,
  model: string,
  usageContext: UsageContext | undefined,
  provider: UpstreamProvider,
  lifecycle: YunwuTransportLifecycle,
  signal: AbortSignal,
  downstreamSignal: AbortSignal,
  warnings: readonly string[] = [],
  onResponseTerminal?: (completed: boolean) => void,
): Promise<Response> => {
  let id = `chatcmpl_${crypto.randomUUID().replace(/-/g, "")}`;
  let created = Math.floor(Date.now() / 1000);
  let content = "";
  let usage: Record<string, unknown> | null = null;
  const functionCalls = new ChatFunctionCallAccumulator();

  let completed = false;
  try {
    let pending: ResponsesStreamEvent | undefined = source.first;
    for (;;) {
      const next = pending ? { done: false as const, value: pending } : await source.iterator.next();
      pending = undefined;
      if (next.done) break;
      const event = next.value;
      const ev = event.value;
      const type = event.type;
      if (event.terminal) {
        if (type !== "response.completed") onResponseTerminal?.(false);
        lifecycle.terminal(type);
        recordStreamTerminalType(usageContext, type as ResponseStreamTerminalType);
        if (type !== "response.completed") {
          const terminalUsage = isRecord(ev.response) ? extractUsageTokens(ev.response.usage) : null;
          recordTerminalUsage(usageContext, terminalUsage, false);
        }
      }
      if (type === "response.created" && isRecord(ev.response)) {
        const upstreamId = getString(ev.response.id);
        const createdAt = typeof ev.response.created_at === "number" ? ev.response.created_at : null;
        if (upstreamId) id = upstreamId;
        if (createdAt) created = createdAt;
        continue;
      }
      if (type === "response.output_text.delta") {
        const delta = getString(ev.delta);
        if (delta === null) {
          return malformedFunctionCallStream("Upstream output-text delta is not a string.");
        }
        content += delta;
        continue;
      }
      if (type === "response.output_item.added") {
        functionCalls.add(ev, ev.item);
        continue;
      }
      if (type === "response.function_call_arguments.delta") {
        functionCalls.delta(ev);
        continue;
      }
      if (type === "response.function_call_arguments.done") {
        functionCalls.done(ev);
        continue;
      }
      if (type === "response.output_item.done") {
        functionCalls.reconcileItem(ev, ev.item);
        continue;
      }
      if (type === "response.output") {
        const output = ev.output ?? (isRecord(ev.response) ? ev.response.output : undefined);
        content += reconcileResponseOutputText(content, output);
        functionCalls.reconcileOutput(ev, output);
        continue;
      }
      if (type === "response.completed" && isRecord(ev.response) && !Array.isArray(ev.response)) {
        content += reconcileResponseOutputText(content, ev.response.output);
        functionCalls.reconcileOutput(ev, ev.response.output);
        functionCalls.assertFinalized();
        const usageTokens = extractUsageTokens(ev.response.usage);
        usage = toChatUsage(usageTokens);
        completed = true;
        onResponseTerminal?.(true);
        await recordCompletionUsage(usageContext, usageTokens);
        break;
      }
      if (event.terminal) break;
    }
  } catch (error) {
    const terminalType = classifyStreamFailure(error, signal, downstreamSignal);
    recordStreamTerminalType(usageContext, terminalType);
    if (terminalType === "cancelled") lifecycle.cancelled();
    else lifecycle.ambiguous();
    completed = false;
  } finally {
    if (!completed) onResponseTerminal?.(false);
    // This path consumes the generator manually (rather than through
    // `for await`), so explicitly close it after a terminal event or error.
    // Otherwise the parser can remain suspended at its final `yield` while
    // retaining the upstream reader lock.
    await source.iterator.return("Chat Completions response consumed").catch(() => {});
  }
  if (!completed) {
    await recordErrorUsage(usageContext);
    return streamErrorResponse(
      502,
      "Upstream stream ended without response.completed.",
      "upstream_stream_error",
      provider,
      warnings,
    );
  }

  const message: Record<string, unknown> = {
    role: "assistant",
    content: content || !functionCalls.hasCalls ? content : null,
  };
  if (functionCalls.hasCalls) {
    message.tool_calls = functionCalls.calls.map((call) => ({
      id: call.callId,
      type: "function",
      function: { name: call.name, arguments: call.arguments },
    }));
  }
  const body: Record<string, unknown> = {
    id,
    object: "chat.completion",
    created,
    model,
    choices: [
      {
        index: 0,
        message,
        finish_reason: functionCalls.hasCalls ? "tool_calls" : "stop",
      },
    ],
  };
  if (usage) body.usage = usage;
  return json(200, body, { "x-uos-upstream": provider });
};

export const handleModels = async (req?: Request): Promise<Response> => {
  if (req) {
    const clientVersion = getCatalogClientVersion(req);
    if (clientVersion !== null) return await handleCodexCatalogModels(req, clientVersion);
  }
  const snapshot = await loadCodexModelsSnapshot();
  const normalized = snapshot && Array.isArray(snapshot.models) && snapshot.models.length > 0
    ? normalizeModelList(snapshot)
    : null;
  const data = withConfiguredCerebrasModel(normalized?.data ?? []);

  return json(
    200,
    { object: "list", data },
    { "x-uos-upstream": snapshot?.source || "stored_codex_models" },
  );
};

export const handleModelCapabilities = async (): Promise<Response> => {
  const snapshot = await loadFullCodexModelsSnapshot();
  const data = snapshot && Array.isArray(snapshot.models) && snapshot.models.length > 0
    ? snapshot.models.map(normalizeModelCapabilitiesEntry).filter(Boolean) as Record<string, unknown>[]
    : [];
  const cerebras = configuredCerebrasModelCapabilities();
  if (cerebras && !data.some((model) => model.id === CEREBRAS_GPT_OSS_120B_MODEL)) {
    data.push(cerebras);
  }

  return json(
    200,
    {
      object: "list",
      data,
      upstream_provider: "codex_chatgpt",
      source: snapshot?.source ?? "stored_codex_models",
      client_version: snapshot?.client_version ?? null,
      updated_at_ms: snapshot?.updated_at_ms ?? null,
    },
    { "x-uos-upstream": snapshot?.source || "stored_codex_models" },
  );
};

const withVoyageUpstreamHeader = (response: Response): Response => {
  const headers = new Headers(response.headers);
  headers.set("x-uos-upstream", "voyage");
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
};

const handleEmbeddingsRequest = async (
  req: Request,
  usageContext?: UsageContext,
  options: Readonly<{ kv?: Deno.Kv | null }> = {},
): Promise<Response> => {
  const requestId = crypto.randomUUID();
  const startedAtMs = Date.now();

  const rawBody = (await readJsonBody(req)) as Record<string, unknown> | null;
  if (!rawBody || !isRecord(rawBody)) {
    return openaiError(400, "Invalid JSON body", "invalid_request_error");
  }

  const parsed = parseUosEmbeddingsRequest(rawBody);
  if (!parsed.ok) return parsed.response;
  const { model, inputs, profile } = parsed.value;

  const kv = Object.prototype.hasOwnProperty.call(options, "kv") ? options.kv ?? null : await getKv();
  const hashes = await Promise.all(inputs.map((text) => sha256Hex(text)));
  let idempotencyLease: EmbeddingsIdempotencyLease | null = null;
  let idempotencyDispatched = false;
  let idempotencyHasConfirmedSuccess = false;
  const idempotencyKey = req.headers.get("Idempotency-Key");
  if (idempotencyKey !== null) {
    if (
      !idempotencyKey ||
      idempotencyKey.length > EMBEDDINGS_IDEMPOTENCY_MAX_KEY_CHARS ||
      hasAsciiControlCharacter(idempotencyKey)
    ) {
      return openaiError(
        400,
        `Idempotency-Key must contain 1-${EMBEDDINGS_IDEMPOTENCY_MAX_KEY_CHARS} non-control characters.`,
        "invalid_request_error",
        { param: null },
      );
    }
    const principal = usageContext?.idempotencyPrincipal?.trim() ?? "";
    if (!kv || !principal) return embeddingsIdempotencyUnavailableResponse();
    const fingerprint = await buildEmbeddingsIdempotencyFingerprint(profile, hashes);
    const acquired = await acquireEmbeddingsIdempotencyLease({
      kv,
      principal,
      idempotencyKey,
      fingerprint,
      requestId,
    });
    if (acquired.kind === "replay") return acquired.response;
    if (acquired.kind === "error") return acquired.response;
    idempotencyLease = acquired.lease;
  }

  const releaseBeforeDispatch = async (response: Response): Promise<Response> => {
    if (!idempotencyLease) return response;
    const released = await releaseEmbeddingsIdempotencyReservation(idempotencyLease, false);
    return released ? response : embeddingsIdempotencyUnavailableResponse();
  };
  const releaseAfterExplicitUpstreamFailure = async (response: Response): Promise<Response> => {
    if (!idempotencyLease) return response;
    if (idempotencyHasConfirmedSuccess) return await failIndeterminate();
    const released = await releaseEmbeddingsIdempotencyReservation(idempotencyLease, true);
    if (released) return response;
    await markEmbeddingsIdempotencyIndeterminate(idempotencyLease);
    return embeddingsIdempotencyIndeterminateResponse();
  };
  const failIndeterminate = async (): Promise<Response> => {
    if (idempotencyLease) await markEmbeddingsIdempotencyIndeterminate(idempotencyLease);
    return embeddingsIdempotencyIndeterminateResponse();
  };

  await recordRequestUsage(usageContext, { model, route: "embeddings", stream: false, reasoning: null });

  const deadlineMs = startedAtMs + EMBEDDINGS_TIMEOUT_MS;
  const apiKey = await readVoyageApiKey(kv);
  if (!apiKey) {
    await recordErrorUsage(usageContext);
    return await releaseBeforeDispatch(
      openaiError(
        503,
        "Embeddings provider is not configured: set VOYAGEAI_API_KEY (or store it in Deno KV)",
        "server_error",
        { type: "server_error", param: null },
      ),
    );
  }
  const shouldCache = Boolean(kv);

  // Dedupe within a request (hash collisions are astronomically unlikely).
  const buckets = new Map<string, { text: string; indices: number[] }>();
  for (let i = 0; i < inputs.length; i += 1) {
    const hash = hashes[i]!;
    const existing = buckets.get(hash);
    if (existing) {
      existing.indices.push(i);
    } else {
      buckets.set(hash, { text: inputs[i]!, indices: [i] });
    }
  }

  const cacheProfileKey = profile.cache_profile_key;
  const cacheKeyFor = (hash: string): Deno.KvKey => embeddingsCacheKey(cacheProfileKey, hash);
  const vectorsByIndex: Array<number[] | null> = Array.from({ length: inputs.length }, () => null);

  let voyageTotalTokens = 0;
  let sawVoyageTokenUsage = false;

  const missing: Array<{ hash: string; text: string; indices: number[] }> = [];
  if (shouldCache && kv) {
    const unique = Array.from(buckets.entries()).map(([hash, bucket]) => ({ hash, ...bucket }));
    const entries = await Promise.all(unique.map((item) => kv.get<{ embedding?: unknown }>(cacheKeyFor(item.hash))));
    for (let i = 0; i < unique.length; i += 1) {
      const item = unique[i]!;
      const entry = entries[i]!;
      const cached = entry.value?.embedding;
      if (isValidEmbeddingVector(cached, profile.dimensions)) {
        for (const idx of item.indices) vectorsByIndex[idx] = cached;
      } else {
        missing.push(item);
      }
    }
  } else {
    for (const [hash, bucket] of buckets.entries()) {
      missing.push({ hash, text: bucket.text, indices: bucket.indices });
    }
  }

  if (missing.length > 0) {
    const chunks = chunkByTokenBudget(
      missing.map((item) => ({ hash: item.hash, text: item.text })),
      EMBEDDINGS_MAX_INPUTS_PER_REQUEST,
      VOYAGE_RATE_LIMIT_TPM,
    );

    let offset = 0;
    for (const chunk of chunks) {
      const now = Date.now();
      if (now >= deadlineMs) {
        await recordErrorUsage(usageContext);
        if (idempotencyLease && idempotencyDispatched) return await failIndeterminate();
        return await releaseBeforeDispatch(
          openaiError(502, "Embeddings request timed out.", "timeout", { type: "server_error", param: null }),
        );
      }

      const chunkItems = missing.slice(offset, offset + chunk.length);
      offset += chunk.length;
      const texts = chunkItems.map((item) => item.text);
      const tokenEstimate = estimateTokenCount(texts);

      if (kv) {
        const reserved = await applyVoyageRateLimit(kv, tokenEstimate, deadlineMs);
        if (!reserved.ok) {
          await recordErrorUsage(usageContext);
          if (idempotencyLease && idempotencyDispatched) return await failIndeterminate();
          const retryAfterSeconds = Math.max(1, Math.ceil(reserved.wait_ms / 1000));
          const body = {
            error: {
              message: `Rate limit exceeded; retry after ~${retryAfterSeconds}s`,
              type: "rate_limit_error",
              code: "rate_limit_exceeded",
              param: null,
            },
          };
          return await releaseBeforeDispatch(json(429, body, { "Retry-After": String(retryAfterSeconds) }));
        }
      }

      let attempt = 0;
      let backoffMs = 250;
      let vectors: number[][] | null = null;

      for (;;) {
        if (idempotencyLease && !idempotencyDispatched) {
          const markedDispatched = await markEmbeddingsIdempotencyDispatched(idempotencyLease);
          if (!markedDispatched) {
            await recordErrorUsage(usageContext);
            return embeddingsIdempotencyUnavailableResponse();
          }
          idempotencyDispatched = true;
        }
        try {
          const upstream = await fetchVoyageEmbeddings({
            apiKey,
            model: profile.upstream_model,
            inputs: texts,
            inputType: profile.input_type,
            dimensions: profile.dimensions,
            outputDtype: profile.output_dtype,
            truncation: profile.truncation,
            deadlineMs,
            beforeProviderDispatch: usageContext?.beforeProviderDispatch,
          });
          vectors = upstream.vectors;
          if (idempotencyLease) idempotencyHasConfirmedSuccess = true;
          if (typeof upstream.totalTokens === "number") {
            sawVoyageTokenUsage = true;
            voyageTotalTokens += upstream.totalTokens;
          }
          break;
        } catch (error) {
          if (error instanceof ApiKeyQuotaDispatchError) {
            await recordErrorUsage(usageContext);
            if (idempotencyLease) {
              const released = await releaseEmbeddingsIdempotencyReservation(
                idempotencyLease,
                idempotencyDispatched,
              );
              if (!released) return embeddingsIdempotencyUnavailableResponse();
              idempotencyDispatched = false;
            }
            return apiKeyQuotaDispatchErrorResponse(error);
          }
          const status = (error as { status?: number }).status;
          const retryAfterMs = (error as { retry_after_ms?: number | null }).retry_after_ms ?? null;
          const snippet = formatErrorSnippet(error);
          const message = snippet
            ? `Embeddings upstream request failed: ${snippet}`
            : "Embeddings upstream request failed.";

          if (!status || !EMBEDDINGS_RETRYABLE_UPSTREAM_STATUSES.has(status)) {
            logRedactedUpstreamError(`[ai.ubq.fi] embeddings request_id=${requestId} upstream_error:`, error);
            await recordErrorUsage(usageContext);
            if (!status) return await failIndeterminate();
            return await releaseAfterExplicitUpstreamFailure(
              openaiError(502, message, "upstream_error", { type: "server_error", param: null }),
            );
          }

          const waitMs = Math.max(0, retryAfterMs ?? backoffMs);
          if (attempt >= 2) {
            logRedactedUpstreamError(`[ai.ubq.fi] embeddings request_id=${requestId} upstream_error:`, error);
            await recordErrorUsage(usageContext);
            if (status === 429) {
              const retryAfterSeconds = Math.max(1, Math.ceil(waitMs / 1000));
              const body = {
                error: {
                  message,
                  type: "rate_limit_error",
                  code: "rate_limit_exceeded",
                  param: null,
                },
              };
              return await releaseAfterExplicitUpstreamFailure(
                json(429, body, { "Retry-After": String(retryAfterSeconds) }),
              );
            }
            return await releaseAfterExplicitUpstreamFailure(
              openaiError(502, message, "upstream_error", { type: "server_error", param: null }),
            );
          }

          const now = Date.now();
          if (now + waitMs >= deadlineMs) {
            if (status === 429) {
              await recordErrorUsage(usageContext);
              const retryAfterSeconds = Math.max(1, Math.ceil(waitMs / 1000));
              const body = {
                error: {
                  message,
                  type: "rate_limit_error",
                  code: "rate_limit_exceeded",
                  param: null,
                },
              };
              return await releaseAfterExplicitUpstreamFailure(
                json(429, body, { "Retry-After": String(retryAfterSeconds) }),
              );
            }
            await recordErrorUsage(usageContext);
            return await releaseAfterExplicitUpstreamFailure(
              openaiError(502, message, "upstream_error", { type: "server_error", param: null }),
            );
          }

          await sleep(waitMs);
          backoffMs = Math.min(2000, backoffMs * 2);
          attempt += 1;
        }
      }

      if (!vectors || vectors.length !== chunkItems.length) {
        await recordErrorUsage(usageContext);
        if (idempotencyLease && idempotencyDispatched) return await failIndeterminate();
        return openaiError(502, "Embeddings upstream returned a size mismatch.", "upstream_error", {
          type: "server_error",
          param: null,
        });
      }

      const wrongLengthIndex = vectors.findIndex((vector) => vector.length !== profile.dimensions);
      if (wrongLengthIndex >= 0) {
        await recordErrorUsage(usageContext);
        const actualLength = vectors[wrongLengthIndex]?.length ?? 0;
        if (idempotencyLease && idempotencyDispatched) return await failIndeterminate();
        return openaiError(
          502,
          `Embeddings upstream returned vector length ${actualLength}; expected ${profile.dimensions}.`,
          "upstream_dimension_mismatch",
          { type: "server_error", param: null },
        );
      }

      for (let i = 0; i < chunkItems.length; i += 1) {
        const item = chunkItems[i]!;
        const vec = vectors[i]!;
        for (const idx of item.indices) vectorsByIndex[idx] = vec;
        if (shouldCache && kv) {
          await writeEmbeddingsCacheEntryBestEffort(
            kv,
            cacheProfileKey,
            item.hash,
            vec,
            Date.now(),
            deadlineMs,
          );
        }
      }
    }
  }

  const data: Array<{ object: "embedding"; index: number; embedding: number[] | string }> = [];
  for (let i = 0; i < vectorsByIndex.length; i += 1) {
    const vec = vectorsByIndex[i];
    if (!vec) {
      await recordErrorUsage(usageContext);
      if (idempotencyLease && idempotencyDispatched) return await failIndeterminate();
      return openaiError(502, "Embeddings gateway failed to construct a complete response.", "server_error", {
        type: "server_error",
        param: null,
      });
    }
    data.push({
      object: "embedding",
      index: i,
      embedding: profile.encoding_format === "base64" ? floatEmbeddingToBase64(vec) : vec,
    });
  }

  const usageTokens: UsageTokens | null = sawVoyageTokenUsage
    ? {
      inputTokens: voyageTotalTokens,
      cachedInputTokens: null,
      cacheWriteInputTokens: null,
      outputTokens: 0,
      totalTokens: voyageTotalTokens,
      status: "reported",
    }
    : null;
  const response = json(200, {
    object: "list",
    data,
    model,
    usage: {
      prompt_tokens: usageTokens?.inputTokens ?? 0,
      total_tokens: usageTokens?.totalTokens ?? 0,
    },
  });
  if (idempotencyLease) {
    const stored = await storeEmbeddingsIdempotencySuccess(idempotencyLease, response);
    if (!stored) {
      if (idempotencyDispatched) return await failIndeterminate();
      return await releaseBeforeDispatch(embeddingsIdempotencyUnavailableResponse());
    }
  }
  await recordCompletionUsage(usageContext, usageTokens);
  return response;
};

export const handleUosEmbeddings = async (
  req: Request,
  usageContext?: UsageContext,
  options: Readonly<{ kv?: Deno.Kv | null }> = {},
): Promise<Response> =>
  await runWithResponseTelemetry(
    usageContext,
    async (context) => withVoyageUpstreamHeader(await handleEmbeddingsRequest(req, context, options)),
  );

const buildEmbeddingsJobBody = (
  job: EmbeddingsJobRecord,
  result: Record<string, unknown> | null,
): Record<string, unknown> => ({
  id: job.id,
  object: "embeddings.job",
  status: job.status,
  created_at_ms: job.created_at_ms,
  updated_at_ms: job.updated_at_ms,
  model: job.model,
  upstream: job.upstream,
  upstream_model: job.upstream_model,
  input_type: job.input_type,
  dimensions: job.dimensions,
  output_dtype: job.output_dtype,
  encoding_format: job.encoding_format,
  truncation: job.truncation,
  input_count: job.input_count,
  total_chars: job.total_chars,
  retry_after_seconds: job.retry_after_seconds,
  error: job.error,
  result,
});

const loadEmbeddingsVectorsFromCache = async (
  kv: Deno.Kv,
  cacheProfileKey: string,
  hashesByIndex: string[],
  dimensions: VoyageEmbeddingsDimension,
): Promise<Array<number[] | null>> => {
  const uniqueHashes = Array.from(new Set(hashesByIndex));
  const cacheKeyFor = (hash: string): Deno.KvKey => embeddingsCacheKey(cacheProfileKey, hash);
  const entries = await Promise.all(uniqueHashes.map((hash) => kv.get<{ embedding?: unknown }>(cacheKeyFor(hash))));
  const vectorsByHash = new Map<string, number[]>();
  for (let i = 0; i < uniqueHashes.length; i += 1) {
    const hash = uniqueHashes[i]!;
    const cached = entries[i]?.value?.embedding;
    if (isValidEmbeddingVector(cached, dimensions)) {
      vectorsByHash.set(hash, cached);
    }
  }
  return hashesByIndex.map((hash) => vectorsByHash.get(hash) ?? null);
};

const buildOpenAiEmbeddingsResult = (
  model: string,
  vectorsByIndex: Array<number[] | null>,
  usageTotalTokens: number,
  encodingFormat: EmbeddingsEncodingFormat,
): Record<string, unknown> => ({
  object: "list",
  data: vectorsByIndex.map((vec, index) => ({
    object: "embedding",
    index,
    embedding: vec && encodingFormat === "base64" ? floatEmbeddingToBase64(vec) : vec ?? [],
  })),
  model,
  usage: { prompt_tokens: usageTotalTokens, total_tokens: usageTotalTokens },
});

const reserveVoyageBudgetForJob = async (
  kv: Deno.Kv,
  tokens: number,
): Promise<{ ok: true } | { ok: false; wait_ms: number }> => {
  for (let attempt = 0; attempt < 6; attempt += 1) {
    const reserved = await tryReserveVoyageBudget(kv, tokens);
    if (reserved.ok) return reserved;
    if (reserved.wait_ms > 0) return reserved;
    await sleep(5 + attempt * 5);
  }
  return { ok: false, wait_ms: 1000 };
};

const updateEmbeddingsJobRecord = async (
  kv: Deno.Kv,
  jobKey: Deno.KvKey,
  lookupKey: Deno.KvKey,
  job: EmbeddingsJobRecord,
): Promise<void> => {
  await kv.atomic()
    .set(jobKey, job, { expireIn: EMBEDDINGS_JOB_TTL_MS })
    .set(
      lookupKey,
      { cache_profile_key: job.cache_profile_key } satisfies EmbeddingsJobLookupRecord,
      { expireIn: EMBEDDINGS_JOB_TTL_MS },
    )
    .commit();
};

const deleteEmbeddingsJobInputs = async (
  kv: Deno.Kv,
  tokenHash: string,
  cacheProfileKey: string,
  jobId: string,
  uniqueHashes: string[],
): Promise<void> => {
  await Promise.all(
    uniqueHashes.map((hash) => kv.delete(embeddingsJobInputKey(tokenHash, cacheProfileKey, jobId, hash))),
  );
};

const runEmbeddingsJobAttempt = async (params: {
  reqId: string;
  kv: Deno.Kv;
  apiKey: string;
  tokenSeed: string;
  tokenHash: string;
  jobKey: Deno.KvKey;
  jobLookupKey: Deno.KvKey;
  jobEntry: Deno.KvEntryMaybe<EmbeddingsJobRecord>;
  job: EmbeddingsJobRecord;
  deadlineMs: number;
  usageContext?: UsageContext;
}): Promise<Response> => {
  const now = Date.now();
  if (params.job.locked_until_ms && params.job.locked_until_ms > now) {
    const retryAfterSeconds = Math.max(1, Math.ceil((params.job.locked_until_ms - now) / 1000));
    const body = buildEmbeddingsJobBody(params.job, null);
    return json(202, body, {
      "Retry-After": String(retryAfterSeconds),
      "x-uos-upstream": params.job.upstream,
    });
  }

  const lockedUntilMs = now + EMBEDDINGS_JOB_LOCK_MS;
  const locked: EmbeddingsJobRecord = {
    ...params.job,
    status: "running",
    locked_until_ms: lockedUntilMs,
    updated_at_ms: now,
    retry_after_seconds: null,
  };

  const lockCommit = await params.kv.atomic()
    .check(params.jobEntry)
    .set(params.jobKey, locked, { expireIn: EMBEDDINGS_JOB_TTL_MS })
    .set(
      params.jobLookupKey,
      { cache_profile_key: locked.cache_profile_key } satisfies EmbeddingsJobLookupRecord,
      { expireIn: EMBEDDINGS_JOB_TTL_MS },
    )
    .commit();
  if (!lockCommit.ok) {
    const body = buildEmbeddingsJobBody(params.job, null);
    return json(202, body, { "Retry-After": "1", "x-uos-upstream": params.job.upstream });
  }

  const cacheProfileKey = locked.cache_profile_key;
  const hashesByIndex = locked.input_hashes;
  const uniqueHashes = Array.from(new Set(hashesByIndex));
  const cacheKeyFor = (hash: string): Deno.KvKey => embeddingsCacheKey(cacheProfileKey, hash);

  let currentJob: EmbeddingsJobRecord = locked;
  let queueRetryAfterMs: number | null = null;

  const computeMissing = async (): Promise<string[]> => {
    const entries = await Promise.all(
      uniqueHashes.map((hash) => params.kv.get<{ embedding?: unknown }>(cacheKeyFor(hash))),
    );
    const missing: string[] = [];
    for (let i = 0; i < uniqueHashes.length; i += 1) {
      const hash = uniqueHashes[i]!;
      const cached = entries[i]?.value?.embedding;
      if (isValidEmbeddingVector(cached, locked.dimensions)) continue;
      missing.push(hash);
    }
    return missing;
  };

  const failJob = async (message: string, code: string): Promise<Response> => {
    const failed: EmbeddingsJobRecord = {
      ...currentJob,
      status: "failed",
      updated_at_ms: Date.now(),
      locked_until_ms: null,
      retry_after_seconds: null,
      error: { message, type: "server_error", code },
    };
    currentJob = failed;
    await updateEmbeddingsJobRecord(params.kv, params.jobKey, params.jobLookupKey, failed);
    await deleteEmbeddingsJobInputs(
      params.kv,
      params.tokenHash,
      failed.cache_profile_key,
      failed.id,
      uniqueHashes,
    );
    await recordErrorUsage(params.usageContext);
    return json(200, buildEmbeddingsJobBody(failed, null), { "x-uos-upstream": failed.upstream });
  };

  const queueJob = async (waitMs: number): Promise<Response> => {
    const retryAfterSeconds = Math.max(1, Math.ceil(waitMs / 1000));
    const queued: EmbeddingsJobRecord = {
      ...currentJob,
      status: "queued",
      updated_at_ms: Date.now(),
      locked_until_ms: null,
      retry_after_seconds: retryAfterSeconds,
      error: null,
    };
    currentJob = queued;
    await updateEmbeddingsJobRecord(params.kv, params.jobKey, params.jobLookupKey, queued);
    const body = buildEmbeddingsJobBody(queued, null);
    return json(202, body, { "Retry-After": String(retryAfterSeconds), "x-uos-upstream": queued.upstream });
  };

  const succeedJob = async (): Promise<Response> => {
    const vectorsByIndex = await loadEmbeddingsVectorsFromCache(
      params.kv,
      cacheProfileKey,
      hashesByIndex,
      currentJob.dimensions,
    );
    if (vectorsByIndex.some((vec) => !vec)) {
      return await failJob("Embeddings job completed but cache entries were missing.", "embeddings_job_cache_miss");
    }
    const succeeded: EmbeddingsJobRecord = {
      ...currentJob,
      status: "succeeded",
      updated_at_ms: Date.now(),
      locked_until_ms: null,
      retry_after_seconds: null,
      error: null,
    };
    currentJob = succeeded;
    await updateEmbeddingsJobRecord(params.kv, params.jobKey, params.jobLookupKey, succeeded);
    await deleteEmbeddingsJobInputs(
      params.kv,
      params.tokenHash,
      succeeded.cache_profile_key,
      succeeded.id,
      uniqueHashes,
    );
    const result = buildOpenAiEmbeddingsResult(
      succeeded.model,
      vectorsByIndex,
      succeeded.usage_total_tokens,
      succeeded.encoding_format,
    );
    const usageTokens: UsageTokens | null = succeeded.usage_total_tokens > 0
      ? {
        inputTokens: succeeded.usage_total_tokens,
        cachedInputTokens: null,
        cacheWriteInputTokens: null,
        outputTokens: 0,
        totalTokens: succeeded.usage_total_tokens,
        status: "reported",
      }
      : null;
    await recordCompletionUsage(params.usageContext, usageTokens);
    return json(200, buildEmbeddingsJobBody(succeeded, result), { "x-uos-upstream": succeeded.upstream });
  };

  const missingBefore = await computeMissing();
  if (missingBefore.length === 0) return await succeedJob();

  const inputEntries = await Promise.all(
    missingBefore.map((hash) =>
      params.kv.get<EmbeddingsJobInputRecord>(
        embeddingsJobInputKey(params.tokenHash, locked.cache_profile_key, locked.id, hash),
      )
    ),
  );
  const items: Array<{ hash: string; text: string }> = [];
  for (let i = 0; i < missingBefore.length; i += 1) {
    const hash = missingBefore[i]!;
    const entry = inputEntries[i]!;
    const normalized = normalizeEmbeddingsJobInputRecord(entry.value);
    if (!normalized) {
      return await failJob("Embeddings job input expired or was unavailable.", "embeddings_job_input_missing");
    }
    const text = await decryptEmbeddingsJobInput(params.tokenSeed, normalized);
    if (text === null) {
      return await failJob("Embeddings job input could not be decrypted.", "embeddings_job_input_decrypt_failed");
    }
    items.push({ hash, text });
  }

  const chunks = chunkByTokenBudget(items, EMBEDDINGS_MAX_INPUTS_PER_REQUEST, VOYAGE_RATE_LIMIT_TPM);
  for (const chunk of chunks) {
    if (Date.now() >= params.deadlineMs) {
      queueRetryAfterMs = 1000;
      break;
    }

    const texts = chunk.map((item) => item.text);
    const tokenEstimate = estimateTokenCount(texts);

    const reserved = await reserveVoyageBudgetForJob(params.kv, tokenEstimate);
    if (!reserved.ok) {
      queueRetryAfterMs = reserved.wait_ms > 0 ? reserved.wait_ms : 1000;
      break;
    }

    let vectors: number[][] | null = null;
    let totalTokens: number | null = null;
    try {
      const upstream = await fetchVoyageEmbeddings({
        apiKey: params.apiKey,
        model: currentJob.upstream_model,
        inputs: texts,
        inputType: currentJob.input_type,
        dimensions: currentJob.dimensions,
        outputDtype: currentJob.output_dtype,
        truncation: currentJob.truncation,
        deadlineMs: params.deadlineMs,
        beforeProviderDispatch: params.usageContext?.beforeProviderDispatch,
      });
      vectors = upstream.vectors;
      totalTokens = upstream.totalTokens;
    } catch (error) {
      if (error instanceof ApiKeyQuotaDispatchError) {
        return await queueJob(1_000);
      }
      const status = (error as { status?: number }).status;
      const retryAfterMs = (error as { retry_after_ms?: number | null }).retry_after_ms ?? null;
      if (status && EMBEDDINGS_RETRYABLE_UPSTREAM_STATUSES.has(status)) {
        queueRetryAfterMs = retryAfterMs ?? (status === 429 ? 60_000 : 1_000);
        break;
      }
      const snippet = formatErrorSnippet(error);
      const message = snippet
        ? `Embeddings upstream request failed: ${snippet}`
        : "Embeddings upstream request failed.";
      return await failJob(message, "embeddings_job_upstream_error");
    }

    if (!vectors || vectors.length !== chunk.length) {
      return await failJob("Embeddings upstream returned a size mismatch.", "embeddings_job_upstream_mismatch");
    }

    const wrongLengthIndex = vectors.findIndex((vector) => vector.length !== currentJob.dimensions);
    if (wrongLengthIndex >= 0) {
      const actualLength = vectors[wrongLengthIndex]?.length ?? 0;
      return await failJob(
        `Embeddings upstream returned vector length ${actualLength}; expected ${currentJob.dimensions}.`,
        "embeddings_job_upstream_dimension_mismatch",
      );
    }

    if (typeof totalTokens === "number") {
      currentJob = { ...currentJob, usage_total_tokens: currentJob.usage_total_tokens + totalTokens };
    }

    for (let i = 0; i < chunk.length; i += 1) {
      const item = chunk[i]!;
      const vec = vectors[i]!;
      await writeEmbeddingsCacheEntryBestEffort(
        params.kv,
        currentJob.cache_profile_key,
        item.hash,
        vec,
        Date.now(),
        params.deadlineMs,
      );
    }
  }

  const missingAfter = await computeMissing();
  if (missingAfter.length === 0) return await succeedJob();

  const waitMs = queueRetryAfterMs ?? 60_000;
  return await queueJob(waitMs);
};

const handleEmbeddingsJobCreateInternal = async (
  req: Request,
  authToken: string | null,
  usageContext?: UsageContext,
): Promise<Response> => {
  const requestId = crypto.randomUUID();
  const startedAtMs = Date.now();

  const rawBody = (await readJsonBody(req)) as Record<string, unknown> | null;
  if (!rawBody || !isRecord(rawBody)) {
    await recordErrorUsage(usageContext);
    return openaiError(400, "Invalid JSON body", "invalid_request_error");
  }

  const parsed = parseEmbeddingsJobRequest(rawBody);
  if (!parsed.ok) {
    await recordErrorUsage(usageContext);
    return parsed.response;
  }
  const { model, inputs, total_chars: totalChars, profile } = parsed.value;

  await recordRequestUsage(usageContext, { model, route: "embeddings.jobs.create", stream: false, reasoning: null });

  const kv = await getKv();
  if (!kv) {
    await recordErrorUsage(usageContext);
    return openaiError(503, "Embeddings jobs require Deno KV", "server_error", { type: "server_error", param: null });
  }

  const apiKey = await readVoyageApiKey(kv);
  if (!apiKey) {
    await recordErrorUsage(usageContext);
    return openaiError(
      503,
      "Embeddings provider is not configured: set VOYAGEAI_API_KEY (or store it in Deno KV)",
      "server_error",
      { type: "server_error", param: null },
    );
  }

  const hashesByIndex = await Promise.all(inputs.map((text) => sha256Hex(text)));
  const uniqueTextsByHash = new Map<string, string>();
  for (let i = 0; i < inputs.length; i += 1) uniqueTextsByHash.set(hashesByIndex[i]!, inputs[i]!);
  const uniqueHashes = Array.from(uniqueTextsByHash.keys());

  const jobId = `embjob_${crypto.randomUUID().replace(/-/g, "")}`;
  const tokenSeed = resolveEmbeddingsJobTokenSeed(jobId, authToken, usageContext);
  const tokenHash = await sha256Hex(tokenSeed);
  const now = Date.now();

  // Store encrypted inputs (no raw text) so queued jobs can be processed later without the client resending inputs.
  const inputWrites = uniqueHashes.map(async (hash) => {
    const record = await encryptEmbeddingsJobInput(tokenSeed, uniqueTextsByHash.get(hash)!);
    await kv.set(
      embeddingsJobInputKey(tokenHash, profile.cache_profile_key, jobId, hash),
      record,
      { expireIn: EMBEDDINGS_JOB_TTL_MS },
    );
  });
  await Promise.all(inputWrites);

  const job: EmbeddingsJobRecord = {
    id: jobId,
    status: "queued",
    created_at_ms: now,
    updated_at_ms: now,
    model,
    cache_profile_key: profile.cache_profile_key,
    upstream: profile.upstream,
    upstream_model: profile.upstream_model,
    input_type: profile.input_type,
    dimensions: profile.dimensions,
    output_dtype: profile.output_dtype,
    encoding_format: profile.encoding_format,
    truncation: profile.truncation,
    input_hashes: hashesByIndex,
    input_count: inputs.length,
    total_chars: totalChars,
    usage_total_tokens: 0,
    retry_after_seconds: null,
    locked_until_ms: null,
    error: null,
  };
  const jobKey = embeddingsJobKey(tokenHash, profile.cache_profile_key, jobId);
  const jobLookupKey = embeddingsJobLookupKey(tokenHash, jobId);
  const persisted = await kv.atomic()
    .set(jobKey, job, { expireIn: EMBEDDINGS_JOB_TTL_MS })
    .set(
      jobLookupKey,
      { cache_profile_key: profile.cache_profile_key } satisfies EmbeddingsJobLookupRecord,
      { expireIn: EMBEDDINGS_JOB_TTL_MS },
    )
    .commit();
  if (!persisted.ok) {
    await recordErrorUsage(usageContext);
    return openaiError(502, "Embeddings job could not be persisted.", "server_error", {
      type: "server_error",
      param: null,
    });
  }

  const deadlineMs = startedAtMs + EMBEDDINGS_TIMEOUT_MS;
  const entry = await kv.get<EmbeddingsJobRecord>(jobKey);
  const value = entry.value;
  if (!value) {
    await recordErrorUsage(usageContext);
    return openaiError(502, "Embeddings job could not be persisted.", "server_error", {
      type: "server_error",
      param: null,
    });
  }

  return await runEmbeddingsJobAttempt({
    reqId: requestId,
    kv,
    apiKey,
    tokenSeed,
    tokenHash,
    jobKey,
    jobLookupKey,
    jobEntry: entry,
    job: value,
    deadlineMs,
    usageContext,
  });
};

export const handleEmbeddingsJobCreate = async (
  req: Request,
  authToken: string | null,
  usageContext?: UsageContext,
): Promise<Response> =>
  await runWithResponseTelemetry(
    usageContext,
    async (context) => withVoyageUpstreamHeader(await handleEmbeddingsJobCreateInternal(req, authToken, context)),
  );

const handleEmbeddingsJobGetInternal = async (
  _req: Request,
  authToken: string | null,
  jobId: string,
  usageContext?: UsageContext,
): Promise<Response> => {
  const requestId = crypto.randomUUID();
  const startedAtMs = Date.now();

  const kv = await getKv();
  if (!kv) {
    await recordErrorUsage(usageContext);
    return openaiError(503, "Embeddings jobs require Deno KV", "server_error", { type: "server_error", param: null });
  }

  const preferredSeed = resolveEmbeddingsJobTokenSeed(jobId, authToken, usageContext);
  const preferredHash = await sha256Hex(preferredSeed);
  const tokenSeed = preferredSeed;
  const tokenHash = preferredHash;
  const jobLookupKey = embeddingsJobLookupKey(preferredHash, jobId);
  const lookupEntry = await kv.get<EmbeddingsJobLookupRecord>(jobLookupKey);
  const cacheProfileKey = isRecord(lookupEntry.value) ? getString(lookupEntry.value.cache_profile_key) : null;
  if (!cacheProfileKey) {
    await recordErrorUsage(usageContext);
    return openaiError(404, "Embeddings job not found", "not_found", {
      type: "invalid_request_error",
      param: null,
    });
  }
  const jobKey = embeddingsJobKey(preferredHash, cacheProfileKey, jobId);
  const entry = await kv.get<EmbeddingsJobRecord>(jobKey);

  const job = entry.value;
  if (!job || job.cache_profile_key !== cacheProfileKey) {
    await recordErrorUsage(usageContext);
    return openaiError(404, "Embeddings job not found", "not_found", { type: "invalid_request_error", param: null });
  }

  await recordRequestUsage(usageContext, {
    model: job.model,
    route: "embeddings.jobs.get",
    stream: false,
    reasoning: null,
  });

  if (job.status === "succeeded") {
    const vectorsByIndex = await loadEmbeddingsVectorsFromCache(
      kv,
      job.cache_profile_key,
      job.input_hashes,
      job.dimensions,
    );
    const result = vectorsByIndex.some((vec) => !vec)
      ? null
      : buildOpenAiEmbeddingsResult(job.model, vectorsByIndex, job.usage_total_tokens, job.encoding_format);
    if (!result) {
      // Cache misses are unexpected (cache TTL is longer than job TTL), but if it happens
      // there's nothing the client can do besides resubmitting the job.
      const failed: EmbeddingsJobRecord = {
        ...job,
        status: "failed",
        updated_at_ms: Date.now(),
        locked_until_ms: null,
        retry_after_seconds: null,
        error: {
          message: "Embeddings job result was unavailable; please resubmit.",
          type: "server_error",
          code: "embeddings_job_result_missing",
        },
      };
      await updateEmbeddingsJobRecord(kv, jobKey, jobLookupKey, failed);
      return json(200, buildEmbeddingsJobBody(failed, null), { "x-uos-upstream": failed.upstream });
    }
    return json(200, buildEmbeddingsJobBody(job, result), { "x-uos-upstream": job.upstream });
  }

  if (job.status === "failed") {
    return json(200, buildEmbeddingsJobBody(job, null), { "x-uos-upstream": job.upstream });
  }

  const apiKey = await readVoyageApiKey(kv);
  if (!apiKey) {
    await recordErrorUsage(usageContext);
    return openaiError(
      503,
      "Embeddings provider is not configured: set VOYAGEAI_API_KEY (or store it in Deno KV)",
      "server_error",
      { type: "server_error", param: null },
    );
  }

  const deadlineMs = startedAtMs + EMBEDDINGS_TIMEOUT_MS;
  const response = await runEmbeddingsJobAttempt({
    reqId: requestId,
    kv,
    apiKey,
    tokenSeed,
    tokenHash,
    jobKey,
    jobLookupKey,
    jobEntry: entry,
    job,
    deadlineMs,
    usageContext,
  });

  return response;
};

export const handleEmbeddingsJobGet = async (
  req: Request,
  authToken: string | null,
  jobId: string,
  usageContext?: UsageContext,
): Promise<Response> =>
  await runWithResponseTelemetry(
    usageContext,
    async (context) => withVoyageUpstreamHeader(await handleEmbeddingsJobGetInternal(req, authToken, jobId, context)),
  );

const recordCerebrasResponseHealth = (status: number): void => {
  if (status === 401 || status === 403) {
    void recordCerebrasProviderHealth("auth_invalid", status);
    return;
  }
  if (status === 429) {
    void recordCerebrasProviderHealth("quota_exhausted", status);
    return;
  }
  if (status >= 500) {
    void recordCerebrasProviderHealth("upstream_error", status);
    return;
  }
  if (status >= 400) {
    void recordCerebrasProviderHealth("reachable", status);
    return;
  }
  void recordCerebrasProviderHealth("success", status);
};

const cerebrasTerminalTypeForError = (error: unknown, downstreamSignal: AbortSignal): ResponseStreamTerminalType => {
  if (downstreamSignal.aborted) return "cancelled";
  if (error instanceof CerebrasError && error.status === 504) return "deadline";
  if (error instanceof Error && error.name === "TimeoutError") return "deadline";
  if (error instanceof Error && error.name === "AbortError") return "cancelled";
  return "error";
};

/**
 * The GPT-OSS route is deliberately separate from the Codex Responses bridge.
 * It forwards the official Chat Completions body unchanged (apart from the
 * canonical model and explicit non-streaming flag) and never races or falls
 * back to another provider.
 */
const handleCerebrasChatCompletions = async (
  req: Request,
  rawRecord: Record<string, unknown>,
  modelRaw: string,
  usageContext?: UsageContext,
): Promise<Response> => {
  const messages = rawRecord.messages;
  if (!Array.isArray(messages)) return openaiError(400, "messages must be an array", "invalid_request_error");
  if (messages.length === 0) return openaiError(400, "messages must be a non-empty array", "invalid_request_error");
  if (messages.some((message) => !isRecord(message) || Array.isArray(message))) {
    return openaiError(400, "messages must contain objects", "invalid_request_error", { param: "messages" });
  }

  const reasoningEffort = parseReasoningEffortField(rawRecord.reasoning_effort, "reasoning_effort");
  if (!reasoningEffort.ok) {
    return openaiError(400, reasoningEffort.message, "invalid_request_error", { param: "reasoning_effort" });
  }
  const parsedStream = parseStreamField(rawRecord.stream);
  if (!parsedStream.ok) {
    return openaiError(400, parsedStream.message, "invalid_request_error", { param: "stream" });
  }
  const streamOptions = parseChatStreamOptions(rawRecord.stream_options);
  if (!streamOptions.ok) {
    return openaiError(400, streamOptions.message, "invalid_request_error", { param: "stream_options" });
  }
  const clientWantsStream = parsedStream.value;

  // Preserve the official nested Chat tools/tool_choice contract. In
  // particular, do not run the Codex-specific flattening that follows this
  // early branch in handleChatCompletionsInternal.
  const reasoning = reasoningEffort.value ?? DEFAULT_REASONING_EFFORT;
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

  const requestSignal = inferenceSignal(req);
  let upstream: Response;
  try {
    upstream = await fetchCerebrasChatCompletions(cerebrasBody, {
      signal: requestSignal,
      beforeDispatch: () => usageContext?.beforeProviderDispatch?.("cerebras") ?? Promise.resolve(),
      onDispatch: () => recordFirstProviderDispatch(usageContext),
      onHeaders: () => recordFirstProviderHeaders(usageContext),
    });
  } catch (error) {
    const terminalType = cerebrasTerminalTypeForError(error, req.signal);
    recordStreamTerminalType(usageContext, terminalType);
    if (terminalType !== "cancelled") void recordCerebrasProviderHealth("upstream_error", null);
    await recordErrorUsage(usageContext);
    // Do not log the caught value: an upstream implementation can attach raw
    // response text or request configuration to an Error instance.
    return toCerebrasErrorResponse(error);
  }

  let providerRequestId = getCerebrasProviderRequestId(upstream);
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.providerRequestId = providerRequestId;

  if (!upstream.ok) {
    recordCerebrasResponseHealth(upstream.status);
    recordStreamTerminalType(usageContext, "response.failed");
    await recordErrorUsage(usageContext);
    return toCerebrasUpstreamErrorResponse(upstream);
  }

  const captured = await readBoundedResponseBody(upstream, {
    signal: requestSignal,
    maxBytes: 128 * 1024,
    // Successful buffered inference uses the request-level 85s deadline, not
    // the one-second error-body default. `requestSignal` still caps the whole
    // request from dispatch through body completion.
    timeoutMs: BUFFERED_INFERENCE_DEADLINE_MS,
    cancellationReason: "Cerebras Chat Completions response was incomplete",
  });
  if (!captured.complete) {
    const terminalType = req.signal.aborted ? "cancelled" : requestSignal.aborted ? "deadline" : "error";
    recordStreamTerminalType(usageContext, terminalType);
    if (terminalType !== "cancelled") void recordCerebrasProviderHealth("upstream_error", null);
    await recordErrorUsage(usageContext);
    if (terminalType === "cancelled") {
      return openaiError(
        499,
        "Request was cancelled.",
        "request_cancelled",
        { type: "server_error", headers: cerebrasResponseHeaders(providerRequestId) },
      );
    }
    return openaiError(
      terminalType === "deadline" ? 504 : 502,
      terminalType === "deadline"
        ? "Upstream request exceeded the gateway deadline."
        : "Upstream returned an incomplete response.",
      terminalType === "deadline" ? "gateway_timeout" : "cerebras_upstream_invalid_response",
      { type: "server_error", headers: cerebrasResponseHeaders(providerRequestId) },
    );
  }

  let payload: unknown;
  try {
    payload = JSON.parse(new TextDecoder().decode(captured.bytes)) as unknown;
  } catch {
    recordStreamTerminalType(usageContext, "error");
    void recordCerebrasProviderHealth("upstream_error", upstream.status);
    await recordErrorUsage(usageContext);
    return openaiError(
      502,
      "Upstream returned an invalid Chat Completions response.",
      "cerebras_upstream_invalid_response",
      { type: "server_error", headers: cerebrasResponseHeaders(providerRequestId) },
    );
  }
  const normalized = normalizeCerebrasChatCompletion(payload, CEREBRAS_GPT_OSS_120B_MODEL);
  if (!normalized.ok) {
    recordStreamTerminalType(usageContext, "error");
    void recordCerebrasProviderHealth("upstream_error", upstream.status);
    await recordErrorUsage(usageContext);
    return openaiError(
      502,
      "Upstream returned an invalid Chat Completions response.",
      "cerebras_upstream_invalid_response",
      { type: "server_error", headers: cerebrasResponseHeaders(providerRequestId) },
    );
  }

  providerRequestId ??= normalizeCerebrasProviderRequestId(normalized.value.id);
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.providerRequestId = providerRequestId;
  const usage = extractChatUsageTokens(normalized.value.usage);
  await recordCompletionUsage(usageContext, usage);
  if (clientWantsStream) recordFirstSseEvent(usageContext);
  recordStreamTerminalType(usageContext, "response.completed");
  recordCerebrasResponseHealth(upstream.status);
  const responseHeaders = cerebrasResponseHeaders(
    providerRequestId,
    clientWantsStream ? GPT_OSS_STREAM_DOWNGRADED_WARNING : undefined,
  );
  if (clientWantsStream) {
    recordStreamTerminal(usageContext);
    return streamCerebrasChatCompletion(normalized.value, streamOptions.includeUsage, responseHeaders);
  }
  return json(200, normalized.value, responseHeaders);
};

const handleChatCompletionsInternal = async (req: Request, usageContext?: UsageContext): Promise<Response> => {
  const body = (await readJsonBody(req)) as ChatCompletionRequest | null;
  if (!body || !isRecord(body)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const rawRecord = body as Record<string, unknown>;
  const unknownKey = findUnknownKey(rawRecord, CHAT_COMPLETIONS_ALLOWED_KEYS);
  if (unknownKey) {
    return openaiError(400, `Unrecognized request argument supplied: ${unknownKey}`, "invalid_request_error");
  }
  const promptCacheControls = validatePromptCacheControls(rawRecord);
  if (!promptCacheControls.ok) {
    return openaiError(400, promptCacheControls.message, "invalid_request_error", { param: promptCacheControls.param });
  }
  const warnings = buildIgnoredWarnings(
    rawRecord,
    new Set([
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
    ]),
  );

  const hasModel = Object.prototype.hasOwnProperty.call(rawRecord, "model");
  const rawModelValue = rawRecord.model;
  const modelRawValue = getString(rawModelValue);
  if (hasModel && modelRawValue === null && rawModelValue !== null && rawModelValue !== undefined) {
    return openaiError(400, "model must be a string", "invalid_request_error");
  }
  let modelRaw = (modelRawValue ?? "").trim();
  if (!modelRaw) {
    const defaultModel = await getDefaultModel();
    if (!defaultModel) return defaultModelUnavailableError();
    modelRaw = defaultModel;
  }
  const model = normalizeModelForCodex(modelRaw);
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.model = modelRaw;
  const maxCompletionTokens = parseMaxCompletionTokensField(rawRecord.max_completion_tokens);
  if (!maxCompletionTokens.ok) {
    return openaiError(400, maxCompletionTokens.message, "invalid_request_error", { param: "max_completion_tokens" });
  }
  if (model === CEREBRAS_GPT_OSS_120B_MODEL) {
    return await handleCerebrasChatCompletions(req, rawRecord, modelRaw, usageContext);
  }
  const modelMetadata = await getCodexModelMetadata(model);
  const modelAvailabilityError = validateCodexModelAvailable(modelRaw, modelMetadata);
  if (modelAvailabilityError) return modelAvailabilityError;
  const messagesRaw = body.messages;
  if (!Array.isArray(messagesRaw)) return openaiError(400, "messages must be an array", "invalid_request_error");
  if (messagesRaw.length === 0) return openaiError(400, "messages must be a non-empty array", "invalid_request_error");

  const reasoningEffort = parseReasoningEffortField(body.reasoning_effort, "reasoning_effort");
  if (!reasoningEffort.ok) {
    return openaiError(400, reasoningEffort.message, "invalid_request_error", { param: "reasoning_effort" });
  }

  const parsedStream = parseStreamField(body.stream);
  if (!parsedStream.ok) {
    return openaiError(400, parsedStream.message, "invalid_request_error", { param: "stream" });
  }
  const streamOptions = parseChatStreamOptions(rawRecord.stream_options);
  if (!streamOptions.ok) {
    return openaiError(400, streamOptions.message, "invalid_request_error", { param: "stream_options" });
  }

  const normalizedMessages: Array<
    Readonly<{
      instruction: string | null;
      instructionContent: MessageContentItem[] | null;
      input: ResponseInputItem[];
    }>
  > = [];
  for (const [index, msg] of messagesRaw.entries()) {
    const converted = normalizeChatMessage(msg, index);
    if (!converted.ok) {
      return openaiError(400, converted.message, "invalid_request_error", { param: converted.param });
    }
    normalizedMessages.push(converted.value);
  }
  const preserveDeveloperMessages = normalizedMessages.some((message) =>
    message.instructionContent?.some((item) =>
      item.type !== "output_text" && item.prompt_cache_breakpoint?.mode === "explicit"
    ) === true
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
  const promptCacheAvailabilityError = validateKnownUnsupportedPromptCacheUse(
    modelRaw,
    modelMetadata,
    rawRecord,
    input,
    "messages",
  );
  if (promptCacheAvailabilityError) return promptCacheAvailabilityError;
  const defaultEffort = await getDefaultReasoningEffort();
  const modelReasoning = modelMetadata.reasoning;
  const defaultReasoningLabel = resolveDefaultReasoningLabel(modelReasoning, defaultEffort);
  let reasoningValue: Record<string, unknown> | undefined;
  if (reasoningEffort.value === undefined) {
    reasoningValue = { effort: reasoningEffortForCodexRequest(defaultReasoningLabel, modelReasoning) };
  } else {
    reasoningValue = { effort: reasoningEffortForCodexRequest(reasoningEffort.value, modelReasoning) };
  }
  const codexBody = await buildCodexRequest(model, input, {
    reasoning: reasoningValue,
    instructions,
  });
  if (maxCompletionTokens.value !== undefined) codexBody.max_output_tokens = maxCompletionTokens.value;
  const passthroughKeys: PassthroughToolSchemaKey[] = [
    "tools",
    "tool_choice",
    "parallel_tool_calls",
    "prompt_cache_key",
    "prompt_cache_options",
    "prompt_cache_retention",
  ];
  applyPassthroughToCodexRequest(codexBody, rawRecord, passthroughKeys);
  codexBody.store = false;

  const stream = parsedStream.value;
  const reasoningLabel = resolveReasoningLabelFromEffort(reasoningEffort.value, defaultReasoningLabel);
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.reasoning = reasoningLabel;
  await recordRequestUsage(usageContext, {
    model: modelRaw,
    route: "chat.completions",
    stream,
    reasoning: reasoningLabel,
    promptCacheKeyPresent: promptCacheKeyPresent(rawRecord),
    promptCacheMode: promptCacheModeFor(rawRecord),
    explicitBreakpointCount: countExplicitPromptCacheBreakpoints(input),
  });
  // One timer covers both provider dispatch/headers and the first SSE event.
  // It is cleared immediately after preflight so active streams get their own
  // renewable inactivity deadline rather than an 85-second absolute cutoff.
  const streamFirstEventDeadline = stream ? createStreamFirstEventDeadline(req.signal) : null;
  const requestInferenceSignal = streamFirstEventDeadline?.signal ?? inferenceSignal(req);
  const clearStreamFirstEventDeadline = (): void => streamFirstEventDeadline?.clear();

  let routed: RoutedResponsesUpstream;
  try {
    routed = await fetchResponsesWithPaidFallback(codexBody, {
      model,
      route: "chat.completions",
      stream,
      reasoning: reasoningLabel,
      usageContext,
      clientVersion: modelMetadata.snapshot?.client_version,
      signal: requestInferenceSignal,
    });
  } catch (error) {
    clearStreamFirstEventDeadline();
    logRedactedUpstreamError("[ai.ubq.fi] Upstream fetch failed:", error);
    await recordErrorUsage(usageContext);
    return toCodexErrorResponse(error, usageContext?.responseTelemetry?.provider);
  }
  const upstream = routed.response;
  const providerWarnings = responseWarnings(upstream);
  const lifecycle = createYunwuTransportLifecycle(routed.paidFallback);
  const resolveCodexProbe = (completed = false): void => {
    if (routed.provider !== "chatgpt_codex") return;
    const transition = completed ? markCodexResponseCompleted(upstream) : releaseCodexResponseProbe(upstream);
    void transition.catch(() => {});
  };

  if (routed.gatewayResponse) {
    clearStreamFirstEventDeadline();
    recordStreamTerminalType(usageContext, "error");
    await recordErrorUsage(usageContext);
    return upstream;
  }
  if (!upstream.ok) {
    lifecycle.terminal("response.failed");
    recordStreamTerminalType(usageContext, "response.failed");
    await recordErrorUsage(usageContext);
    try {
      const normalized = await toOpenAiUpstreamErrorResponse(upstream, routed.provider, requestInferenceSignal);
      return attachResponseTelemetry(normalized, usageContext?.responseTelemetry ?? createResponseTelemetryState());
    } finally {
      clearStreamFirstEventDeadline();
    }
  }

  if (!upstream.body) {
    clearStreamFirstEventDeadline();
    resolveCodexProbe();
    lifecycle.ambiguous();
    recordStreamTerminalType(usageContext, "error");
    await recordErrorUsage(usageContext);
    return streamErrorResponse(
      502,
      "Codex upstream response missing body.",
      "codex_upstream_missing_body",
      routed.provider,
      [...warnings, ...providerWarnings],
    );
  }

  let preflight: PreflightedResponsesStream;
  try {
    preflight = await preflightResponsesStream(upstream.body, requestInferenceSignal);
    clearStreamFirstEventDeadline();
  } catch (error) {
    clearStreamFirstEventDeadline();
    resolveCodexProbe();
    const terminalType = classifyStreamFailure(error, requestInferenceSignal, req.signal);
    recordStreamTerminalType(usageContext, terminalType);
    if (terminalType === "cancelled") lifecycle.cancelled();
    else lifecycle.ambiguous();
    await recordErrorUsage(usageContext);
    return streamPreflightFailureResponse(terminalType, routed.provider, [...warnings, ...providerWarnings]);
  }
  recordFirstSseEvent(usageContext);
  if (preflight.first.terminal) recordStreamTerminal(usageContext);
  const response = stream
    ? streamChatCompletions(
      preflight,
      model,
      streamOptions.includeUsage,
      usageContext,
      routed.provider,
      lifecycle,
      requestInferenceSignal,
      req.signal,
      resolveCodexProbe,
    )
    : await completeChatCompletions(
      preflight,
      model,
      usageContext,
      routed.provider,
      lifecycle,
      requestInferenceSignal,
      req.signal,
      [...warnings, ...providerWarnings],
      resolveCodexProbe,
    );
  return withUosWarning(response, [...warnings, ...providerWarnings]);
};

export const handleChatCompletions = async (req: Request, usageContext?: UsageContext): Promise<Response> =>
  await runWithResponseTelemetry(
    usageContext,
    (context) => handleChatCompletionsInternal(req, context),
  );

const handleResponsesInternal = async (req: Request, usageContext?: UsageContext): Promise<Response> => {
  const rawBody = (await readJsonBody(req)) as ResponsesRequest | null;
  if (!rawBody || !isRecord(rawBody)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const rawRecord = rawBody as Record<string, unknown>;
  const unknownKey = findUnknownKey(rawRecord, RESPONSES_ALLOWED_KEYS, CODEX_RESPONSES_EXTENSION_KEYS);
  if (unknownKey) {
    return openaiError(400, `Unrecognized request argument supplied: ${unknownKey}`, "invalid_request_error");
  }
  const promptCacheControls = validatePromptCacheControls(rawRecord);
  if (!promptCacheControls.ok) {
    return openaiError(400, promptCacheControls.message, "invalid_request_error", { param: promptCacheControls.param });
  }
  if (Object.prototype.hasOwnProperty.call(rawRecord, "client_metadata")) {
    const clientMetadata = rawBody.client_metadata;
    if (
      !isRecord(clientMetadata) ||
      Array.isArray(clientMetadata) ||
      Object.values(clientMetadata).some((value) => typeof value !== "string")
    ) {
      return openaiError(
        400,
        "client_metadata must be an object with string values",
        "invalid_request_error",
        { param: "client_metadata" },
      );
    }
  }
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
      "prompt_cache_key",
      "prompt_cache_options",
      "prompt_cache_retention",
      "text",
      "include",
      "context_management",
      "client_metadata",
    ]),
  );

  const parsedStream = parseStreamField(rawBody.stream);
  if (!parsedStream.ok) {
    return openaiError(400, parsedStream.message, "invalid_request_error", { param: "stream" });
  }
  const clientWantsStream = parsedStream.value;

  const hasModel = Object.prototype.hasOwnProperty.call(rawRecord, "model");
  const rawModelValue = rawRecord.model;
  const modelRawValue = getString(rawModelValue);
  if (hasModel && modelRawValue === null && rawModelValue !== null && rawModelValue !== undefined) {
    return openaiError(400, "model must be a string", "invalid_request_error");
  }
  let modelRaw = (modelRawValue ?? "").trim();
  if (!modelRaw) {
    const defaultModel = await getDefaultModel();
    if (!defaultModel) return defaultModelUnavailableError();
    modelRaw = defaultModel;
  }
  const model = normalizeModelForCodex(modelRaw);
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.model = modelRaw;
  if (model === CEREBRAS_GPT_OSS_120B_MODEL) {
    return openaiError(
      400,
      "gpt-oss-120b is available only on /v1/chat/completions.",
      "unsupported_model",
      { param: "model" },
    );
  }
  const modelMetadata = await getCodexModelMetadata(model);
  const modelAvailabilityError = validateCodexModelAvailable(modelRaw, modelMetadata);
  if (modelAvailabilityError) return modelAvailabilityError;

  const inputRaw = rawBody.input;
  let input: ResponseInputItem[];
  if (inputRaw === undefined) {
    input = [];
  } else if (typeof inputRaw === "string") {
    input = [{ type: "message", role: "user", content: [{ type: "input_text", text: inputRaw }] }];
  } else if (Array.isArray(inputRaw)) {
    const converted: ResponseInputItem[] = [];
    let contentBuffer: MessageContentItem[] = [];

    const flushContentBuffer = () => {
      if (!contentBuffer.length) return;
      converted.push({ type: "message", role: "user", content: contentBuffer });
      contentBuffer = [];
    };

    let sawNonContentItem = false;
    for (const [index, msg] of inputRaw.entries()) {
      const param = `input[${index}]`;
      const messageType = isRecord(msg) && !Array.isArray(msg) ? getString(msg.type) : null;
      if (messageType === "message" || (messageType === null && isRecord(msg) && "role" in msg)) {
        const mapped = normalizeResponseMessageItem(msg, param);
        if (!mapped.ok) {
          return openaiError(400, mapped.message, "invalid_request_error", { param: mapped.param });
        }
        flushContentBuffer();
        converted.push(mapped.value);
        sawNonContentItem = true;
        continue;
      }

      if (typeof msg === "string") {
        const contentItem: MessageContentItem = { type: "input_text", text: msg };
        if (sawNonContentItem) {
          converted.push({ type: "message", role: "user", content: [contentItem] });
        } else {
          contentBuffer.push(contentItem);
        }
        continue;
      }

      const contentType = isRecord(msg) && !Array.isArray(msg) ? getString(msg.type) : null;
      if (
        contentType === "input_text" || contentType === "input_image" || contentType === "input_file"
      ) {
        const contentItem = normalizeResponseContentItem(msg, param, "user");
        if (!contentItem.ok) {
          return openaiError(400, contentItem.message, "invalid_request_error", { param: contentItem.param });
        }
        if (sawNonContentItem) {
          converted.push({ type: "message", role: "user", content: [contentItem.value] });
        } else {
          contentBuffer.push(contentItem.value);
        }
        continue;
      }

      if (
        isRecord(msg) && !Array.isArray(msg) &&
        Object.prototype.hasOwnProperty.call(msg, "prompt_cache_breakpoint")
      ) {
        return openaiError(
          400,
          "prompt_cache_breakpoint is only valid on supported input content blocks",
          "invalid_request_error",
          { param: `${param}.prompt_cache_breakpoint` },
        );
      }

      // Codex CLI uses the Responses API and can send additional input item types
      // (e.g. reasoning + function_call + function_call_output). Pass them through
      // so tool-calling conversations work end-to-end.
      if (isRecord(msg) && typeof msg.type === "string" && msg.type !== "message") {
        // Content items belong inside a message (or are normalized above).
        // Do not mistake an unsupported input_* content type for an arbitrary
        // Responses item and silently relay it upstream.
        if (
          msg.type.startsWith("input_") || msg.type === "text" || msg.type === "image_url" ||
          msg.type === "output_text"
        ) {
          return openaiError(400, `${param}.type is not supported`, "invalid_request_error", {
            param: `${param}.type`,
          });
        }
        flushContentBuffer();
        const normalizedFunctionOutput = normalizeFunctionCallOutputItem(msg, param);
        if (!normalizedFunctionOutput.ok) {
          return openaiError(400, normalizedFunctionOutput.message, "invalid_request_error", {
            param: normalizedFunctionOutput.param,
          });
        }
        converted.push(normalizedFunctionOutput.value);
        sawNonContentItem = true;
        continue;
      }

      return openaiError(400, "Invalid message in input[]", "invalid_request_error", { param });
    }
    if (!sawNonContentItem || contentBuffer.length) {
      flushContentBuffer();
    }
    input = converted;
  } else {
    return openaiError(400, "input must be a string or an array", "invalid_request_error");
  }

  const promptCacheAvailabilityError = validateKnownUnsupportedPromptCacheUse(
    modelRaw,
    modelMetadata,
    rawRecord,
    input,
    "input",
  );
  if (promptCacheAvailabilityError) return promptCacheAvailabilityError;

  const reasoning = parseReasoningParam(rawBody.reasoning);
  if (!reasoning.ok) return openaiError(400, reasoning.message, "invalid_request_error", { param: "reasoning" });

  let instructions: string | undefined;
  if (Object.prototype.hasOwnProperty.call(rawRecord, "instructions")) {
    if (rawBody.instructions === null) {
      instructions = undefined;
    } else if (typeof rawBody.instructions === "string") {
      instructions = rawBody.instructions;
    } else {
      return openaiError(400, "instructions must be a string", "invalid_request_error");
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

  const codexBody = await buildCodexRequest(model, input, { reasoning: reasoningValue, instructions });
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
  const openRouterBody = { ...codexBody };
  // Codex ignores this official control today, but the emergency OpenRouter
  // route preserves it because that upstream supports the official field.
  if (Object.prototype.hasOwnProperty.call(rawRecord, "max_output_tokens")) {
    openRouterBody.max_output_tokens = rawRecord.max_output_tokens;
  }

  await recordRequestUsage(usageContext, {
    model: modelRaw,
    route: "responses",
    stream: clientWantsStream,
    reasoning: reasoningLabel,
    promptCacheKeyPresent: promptCacheKeyPresent(rawRecord),
    promptCacheMode: promptCacheModeFor(rawRecord),
    explicitBreakpointCount: countExplicitPromptCacheBreakpoints(input),
  });
  const requestInferenceSignal = inferenceSignal(req);
  const apiKey = readOpenRouterApiKey();
  const circuit = apiKey ? await selectOpenRouterCircuitRoute() : null;
  if (circuit && circuit.transition !== "none") {
    recordOpenRouterFields(usageContext, { circuitTransition: circuit.transition });
  }
  const sessionId = apiKey
    ? await deriveOpenRouterSessionId(usageContext?.idempotencyPrincipal, rawRecord.client_metadata)
    : null;
  let route: "codex" | "openrouter" = circuit?.route ?? "codex";
  let globalProbe: OpenRouterCircuitProbe | null = circuit?.probe ?? null;
  let primaryFailureResponse: Response | null = null;
  let primaryResult: ResponsesRouteAttempt | null = null;
  let openRouterAttempt: PreparedResponsesAttempt | null = null;
  let selectedModel: string | null = null;
  let fallbackStartedAt = 0;

  if (route === "codex") {
    try {
      const result = await fetchAndPreparePrimaryResponses(codexBody, {
        model,
        reasoning: reasoningLabel,
        clientWantsStream,
        usageContext,
        clientVersion: modelMetadata.snapshot?.client_version,
        requestSignal: requestInferenceSignal,
        warnings,
      });
      if (result.kind === "ready") {
        primaryResult = result.value;
        if (result.value.prepared.prepared.semantic) {
          markPrimarySemanticRecovery(result.value.routed, globalProbe, usageContext);
        }
      } else {
        const { routed, failed, lifecycle } = result.value;
        primaryFailureResponse = failed.response;
        const terminalType = responseFailureTerminalType(failed.trigger, failed.signal, req.signal);
        recordStreamTerminalType(usageContext, terminalType);
        if (routed.gatewayResponse && !isEligibleResponsesAttemptStatus(failed.response)) {
          if (globalProbe) void releaseGlobalOpenRouterProbe(globalProbe).catch(() => {});
          return failed.response;
        }
        if (!isEligibleResponsesAttemptStatus(failed.response) && failed.trigger === "read_error") {
          if (globalProbe) void releaseGlobalOpenRouterProbe(globalProbe).catch(() => {});
          lifecycle.terminal("response.failed");
          return failed.response;
        }
        if (!routed.gatewayResponse) finalizeAbandonedPrimaryAttempt(routed, lifecycle);
        if (!apiKey) return failed.response;
        const transition = await recordOpenRouterEligibleFailure(globalProbe);
        if (transition !== "none") recordOpenRouterFields(usageContext, { circuitTransition: transition });
        recordOpenRouterFields(usageContext, { triggerClass: failed.trigger });
        route = "openrouter";
      }
    } catch (error) {
      if (globalProbe) void releaseGlobalOpenRouterProbe(globalProbe).catch(() => {});
      logRedactedUpstreamError("[ai.ubq.fi] Upstream fetch failed:", error);
      await recordErrorUsage(usageContext);
      return toCodexErrorResponse(error, usageContext?.responseTelemetry?.provider);
    }
  }

  if (route === "openrouter" && apiKey) {
    fallbackStartedAt = performance.now();
    const openRouter = await fetchAndPrepareOpenRouterResponses(openRouterBody, {
      usageContext,
      requestSignal: requestInferenceSignal,
      sessionId,
      apiKey,
    });
    if (openRouter.kind === "ready") {
      openRouterAttempt = openRouter.attempt;
      selectedModel = openRouter.attempt.selectedModel;
      recordOpenRouterFields(usageContext, {
        selectedModel,
        taskType: openRouter.attempt.taskType,
        semanticCommitment: openRouter.attempt.prepared.semanticKind ??
          (openRouter.attempt.prepared.terminal?.type === "response.completed" ? "terminal_completed" : null),
      });
    } else {
      await persistFailedOpenRouterAttempt(usageContext, fallbackStartedAt, openRouter.attempt.trigger);
      if (primaryFailureResponse) {
        if (usageContext?.responseTelemetry) {
          usageContext.responseTelemetry.provider = primaryFailureResponse.headers.get("x-uos-upstream") ||
            "chatgpt_codex";
        }
        return primaryFailureResponse;
      }
      const recoveryProbe = await claimOpenRouterEarlyRecoveryProbe();
      if (!recoveryProbe) return openRouter.attempt.response;
      globalProbe = recoveryProbe;
      const recovery = await fetchAndPreparePrimaryResponses(codexBody, {
        model,
        reasoning: reasoningLabel,
        clientWantsStream,
        usageContext,
        clientVersion: modelMetadata.snapshot?.client_version,
        requestSignal: requestInferenceSignal,
        warnings,
      });
      if (recovery.kind === "failed") {
        finalizeAbandonedPrimaryAttempt(recovery.value.routed, recovery.value.lifecycle);
        const transition = await recordOpenRouterEligibleFailure(recoveryProbe);
        if (transition !== "none") recordOpenRouterFields(usageContext, { circuitTransition: transition });
        if (usageContext?.responseTelemetry) usageContext.responseTelemetry.provider = "openrouter";
        return openRouter.attempt.response;
      }
      primaryResult = recovery.value;
      if (recovery.value.prepared.prepared.semantic) {
        markPrimarySemanticRecovery(recovery.value.routed, recoveryProbe, usageContext);
      }
    }
  }

  const ready = openRouterAttempt ?? primaryResult?.prepared;
  if (!ready) {
    return primaryFailureResponse ?? streamErrorResponse(
      502,
      "No upstream provider produced a response.",
      "upstream_error",
      "chatgpt_codex",
      warnings,
    );
  }
  const lifecycle = primaryResult?.lifecycle ?? createYunwuTransportLifecycle(null);
  const routed = primaryResult?.routed ?? null;
  const validateOpenRouterEvent = (event: ResponsesStreamEvent): void => {
    if (!openRouterAttempt || !selectedModel) return;
    const candidate = openRouterModelFromEvent(event.value);
    if (!candidate) return;
    if (candidate !== selectedModel || !isEligibleOpenRouterModel(candidate)) {
      throw new ResponsesStreamError("OpenRouter changed the selected model after stream release.", {
        kind: "malformed_event",
      });
    }
  };
  const onTerminal = (event: ResponsesStreamEvent): void => {
    const syntheticFailure = isSyntheticResponsesFailureEvent(event);
    if (routed && !syntheticFailure) {
      lifecycle.terminal(event.type);
      if (routed.provider === "chatgpt_codex") {
        const transition = event.type === "response.completed"
          ? markCodexResponseCompleted(routed.response)
          : releaseCodexResponseProbe(routed.response);
        void transition.catch(() => {});
      }
      if (globalProbe && !ready.prepared.semantic) {
        void releaseGlobalOpenRouterProbe(globalProbe).catch(() => {});
      }
    }
    // A synthetic failure is the client-visible terminal owner after a
    // committed stream breaks. For an abandoned Codex/Yunwu attempt, keep the
    // underlying EOF/read classification in telemetry so paid-fallback
    // reconciliation retains its diagnostic cause. OpenRouter owns its
    // synthetic terminal because no later provider can take over after the
    // failover notice has been released.
    if (!syntheticFailure || openRouterAttempt) {
      recordResponsesTerminal(event, usageContext);
    }
    if (openRouterAttempt) {
      recordOpenRouterFields(usageContext, {
        latencyMs: Math.max(0, Math.round(performance.now() - fallbackStartedAt)),
        terminalStatus: event.type,
      });
      persistOpenRouterFields(usageContext);
    }
  };

  if (!clientWantsStream) {
    const response = await collectBufferedResponses(ready, {
      warningModel: openRouterAttempt ? selectedModel : null,
      usageContext,
      onTerminal,
      validateEvent: validateOpenRouterEvent,
      onFailure: (error) => {
        const terminalType = classifyStreamFailure(error, ready.signal, req.signal);
        if (usageContext?.responseTelemetry?.streamTerminalType === null) {
          recordStreamTerminalType(usageContext, terminalType);
        }
        if (routed) finalizeAbandonedPrimaryAttempt(routed, lifecycle, terminalType === "cancelled");
      },
    });
    return withUosWarning(response, warnings);
  }

  recordFirstSseEvent(usageContext);
  const body = createOwnedResponsesStream({
    initial: ready.prepared.buffered,
    iterator: ready.prepared.iterator,
    responseId: ready.responseId,
    ...(openRouterAttempt && selectedModel ? { warning: { model: selectedModel } } : {}),
    signal: ready.signal,
    downstreamSignal: req.signal,
    onEvent: onTerminal,
    validateEvent: validateOpenRouterEvent,
    onFailure: (error) => {
      const terminalType = classifyStreamFailure(error, ready.signal, req.signal);
      if (usageContext?.responseTelemetry?.streamTerminalType === null) {
        recordStreamTerminalType(usageContext, terminalType);
      }
      if (routed) finalizeAbandonedPrimaryAttempt(routed, lifecycle, terminalType === "cancelled");
      if (openRouterAttempt) {
        if (usageContext?.responseTelemetry?.openRouterTerminalStatus !== "response.failed") {
          recordOpenRouterFields(usageContext, {
            latencyMs: Math.max(0, Math.round(performance.now() - fallbackStartedAt)),
            terminalStatus: terminalType,
          });
          persistOpenRouterFields(usageContext);
        }
      }
      void recordErrorUsage(usageContext);
    },
    onCancel: () => {
      recordStreamTerminalType(usageContext, "cancelled");
      if (routed) finalizeAbandonedPrimaryAttempt(routed, lifecycle, true);
      if (openRouterAttempt) {
        recordOpenRouterFields(usageContext, {
          latencyMs: Math.max(0, Math.round(performance.now() - fallbackStartedAt)),
          terminalStatus: "cancelled",
        });
        persistOpenRouterFields(usageContext);
      }
      void recordErrorUsage(usageContext);
    },
  });
  const headers = new Headers(ready.response.headers);
  headers.set("Content-Type", "text/event-stream");
  headers.set("x-uos-upstream", ready.provider);
  return withUosWarning(new Response(withSseKeepalive(body), { status: 200, headers }), warnings);
};

export const handleResponses = async (req: Request, usageContext?: UsageContext): Promise<Response> =>
  await runWithResponseTelemetry(
    usageContext,
    (context) => handleResponsesInternal(req, context),
  );
