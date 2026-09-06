import {
  buildCodexRequest,
  CODEX_AUTH_REAUTH_MESSAGE,
  CODEX_AUTH_REAUTH_WARNING,
  CODEX_QUOTA_BLOCKED_ERROR_CODE,
  CODEX_UPSTREAM_DEGRADED_ERROR_CODE,
  CodexError,
  type CodexModelsSnapshot,
  fetchCodexResponses,
  getCodexModelsSnapshotDefaultModel,
  getCodexResponseAccountCohortId,
  getCodexResponseAffinityOutcome,
  getCodexResponseSlot,
  getCodexRoutingError,
  loadCodexModelsSnapshot,
  loadFullCodexModelsSnapshot,
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
import {
  BOUNDED_RESPONSE_BODY_MAX_BYTES,
  BOUNDED_RESPONSE_BODY_TIMEOUT_MS,
  readBoundedResponseBody,
} from "./bounded_response_body.ts";
import { json, openaiError, STANDARD_RATE_LIMIT_HEADERS } from "./http.ts";
import {
  BUFFERED_INFERENCE_DEADLINE_MS,
  createInferenceSignal,
  createStreamFirstEventDeadline,
  createStreamSemanticDeadline,
  STREAM_FAILOVER_RESERVE_MS,
  type StreamDeadline,
} from "./inference_deadline.ts";
import { getKv } from "./kv.ts";
import { loadRuntimeConfig } from "./runtime_config.ts";
import { recentModelContextFor } from "./recent_model_context.ts";
import { CHAT_COMPLETIONS_REQUEST_KEYS, RESPONSES_REQUEST_KEYS } from "./openai_schema.ts";
import { captureRawBodyOnce, discardRawBodyObserverOnce, readJsonBody } from "./request.ts";
import {
  type PreflightedResponsesStream,
  preflightResponsesStream,
  readResponsesStream,
  ResponsesStreamError,
  type ResponsesStreamEvent,
  type ResponsesStreamFailureKind,
  type ResponsesStreamIterator,
  withSseKeepalive,
} from "./responses_stream.ts";
import {
  deriveRemovedProviderSessionId,
  fetchRemovedProviderResponses,
  isEligibleRemovedProviderModel,
  readRemovedProviderApiKey,
  removedProviderModelFromEvent,
  removedProviderTaskTypeFromResponse,
  stripRemovedProviderMetadata,
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
import { recordRemovedProviderTelemetry } from "./removed_provider_telemetry.ts";
import {
  appendResponsesPrecommitEvent,
  createOwnedResponsesStream,
  isSyntheticResponsesFailureEvent,
  type OwnedResponsesStreamFailureDetails,
  type PreparedResponsesStream,
  prepareResponsesStreamForCommit,
  responseEventFromValue,
  responseIdFromEvents,
} from "./responses_failover_stream.ts";
import {
  type PaidFallbackReservation,
  recordMeteredAmbiguousFailure,
  recordMeteredPrefetchCancellation,
  recordMeteredTerminal,
  recordMeteredUndispatchedCancellation,
  recordMeteredUpstreamResponse,
  recordSurplusUsage,
  reservePaidFallback,
  type SurplusBillingPricing,
} from "./paid_fallback.ts";
import {
  recordCerebrasProviderHealth,
  recordMeteredProviderHealth,
  recordSurplusProviderHealth,
} from "./provider_health.ts";
import { getString, isRecord, sha256Hex } from "./utils.ts";
import type {
  ChatCompletionRequest,
  MessageContentItem,
  PromptCacheBreakpoint,
  ResponseInputItem,
  ResponseMessageItem,
  ResponsesRequest,
} from "./types.ts";
import {
  fetchMeteredModels,
  fetchMeteredResponses,
  METERED_MODELS_CACHE_TTL_MS,
  MeteredError,
  readMeteredApiKey,
} from "./metered.ts";
import {
  fetchSurplusModels,
  fetchSurplusResponses,
  readSurplusApiKey,
  SURPLUS_MODELS_CACHE_TTL_MS,
  SurplusError,
} from "./surplus.ts";
import { loadDebugRoutingConfig } from "./debug_routing.ts";

// Temporary hard cut while this exact gateway model has free Surplus inference.
// Remove the cut when the free-inference window ends; do not generalize it to
// other catalog models or paid-fallback routing.
const TEMPORARY_FREE_SURPLUS_MODEL = "glm-5.2";

const isTemporaryFreeSurplusModel = (model: string): boolean => model === TEMPORARY_FREE_SURPLUS_MODEL;

const temporaryFreeSurplusCapabilityError = (
  model: string,
  body: Record<string, unknown>,
): Response | null =>
  isTemporaryFreeSurplusModel(model) && Array.isArray(body.tools) && body.tools.length > 0
    ? openaiError(
      400,
      `The model '${model}' does not support tools through this gateway.`,
      "unsupported_model_capability",
      { param: "tools" },
    )
    : null;

const getDefaultModel = async (): Promise<string | null> => {
  const runtime = await loadRuntimeConfig();
  return runtime?.default_model ?? getCodexModelsSnapshotDefaultModel(runtime?.codex_models ?? null);
};

const downstreamSignalFor = (request: Request, context?: UsageContext): AbortSignal =>
  context?.downstreamSignal ?? request.signal;

const inferenceSignal = (request: Request, context?: UsageContext): AbortSignal =>
  createInferenceSignal(downstreamSignalFor(request, context));

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
  downstreamSignal?: AbortSignal;
  responseTelemetry?: ResponseTelemetryState;
  /** Commits an admitted API-key reservation exactly once before transport. */
  beforeProviderDispatch?: (
    provider: "cerebras" | "chatgpt_codex" | "removed_provider" | "metered" | "surplus" | "voyage",
  ) => Promise<ApiKeyProviderDispatch | void>;
  /** Test seam for proving one terminal usage observation per response. */
  onTerminalUsage?: (usage: UsageTokens | null, completed: boolean) => void;
}>;

type UpstreamProvider = "cerebras" | "chatgpt_codex" | "removed_provider" | "metered" | "surplus";
const supportsReasoningProgressRelease = (provider: UpstreamProvider): boolean =>
  provider === "chatgpt_codex" || provider === "surplus" || provider === "metered";
export type InferenceFallbackReason = "primary_quota_blocked" | "dynamic_paid_model";
export type UsageTelemetryStatus = "missing" | "partial" | "reported" | "invalid";
export type PromptCacheMode = "implicit" | "explicit" | "legacy_retention" | "unspecified";
export type AffinityOutcome =
  | "none"
  | "preferred"
  | "preferred_unavailable"
  | "remapped"
  | "failover"
  | "shadow_only";

export type ResponseTelemetry = Readonly<{
  provider: string;
  fallbackReason: InferenceFallbackReason | null;
  model: string | null;
  reasoning: string | null;
  outputTokenAllowance: number | null;
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
  semanticOutputObserved: boolean | null;
  upstreamEventKinds: readonly string[];
  streamTerminalType: ResponseStreamTerminalType | null;
  failureKind: string | null;
  responseCreatedObserved: boolean;
  syntheticTerminalType: "response.failed" | "error" | null;
  stream: boolean | null;
  providerRequestId: string | null;
  firstProviderDispatchMs: number | null;
  firstProviderHeadersMs: number | null;
  firstCodexDispatchMs: number | null;
  firstCodexHeadersMs: number | null;
  firstUpstreamSseEventMs: number | null;
  firstSemanticCommitmentMs: number | null;
  streamTerminalMs: number | null;
  attemptedProviders: readonly string[];
  removedProviderTriggerClass: string | null;
  removedProviderCircuitTransition: string | null;
  removedProviderSelectedModel: string | null;
  removedProviderTaskType: string | null;
  removedProviderSemanticCommitment: string | null;
  removedProviderLatencyMs: number | null;
  removedProviderTerminalStatus: string | null;
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
  outputTokenAllowance: number | null;
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
  accountCohortId: string | null;
  affinityOutcome: AffinityOutcome;
  quotaUsedPercent: number | null | undefined;
  completed: boolean;
  semanticOutputObserved: boolean | null;
  upstreamEventKinds: string[];
  streamTerminalType: ResponseStreamTerminalType | null;
  failureKind: string | null;
  responseCreatedObserved: boolean;
  syntheticTerminalType: "response.failed" | "error" | null;
  stream: boolean | null;
  providerRequestId: string | null;
  firstProviderDispatchMs: number | null;
  firstProviderHeadersMs: number | null;
  firstCodexDispatchMs: number | null;
  firstCodexHeadersMs: number | null;
  firstUpstreamSseEventMs: number | null;
  firstSemanticCommitmentMs: number | null;
  streamTerminalMs: number | null;
  attemptedProviders: string[];
  removedProviderTriggerClass: string | null;
  removedProviderCircuitTransition: string | null;
  removedProviderSelectedModel: string | null;
  removedProviderTaskType: string | null;
  removedProviderSemanticCommitment: string | null;
  removedProviderLatencyMs: number | null;
  removedProviderTerminalStatus: string | null;
};

const responseTelemetry = new WeakMap<Response, ResponseTelemetryState>();

const createResponseTelemetryState = (): ResponseTelemetryState => ({
  provider: null,
  fallbackReason: null,
  model: null,
  reasoning: null,
  outputTokenAllowance: null,
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
  accountCohortId: null,
  affinityOutcome: "none",
  quotaUsedPercent: undefined,
  completed: false,
  semanticOutputObserved: null,
  upstreamEventKinds: [],
  streamTerminalType: null,
  failureKind: null,
  responseCreatedObserved: false,
  syntheticTerminalType: null,
  stream: null,
  providerRequestId: null,
  firstProviderDispatchMs: null,
  firstProviderHeadersMs: null,
  firstCodexDispatchMs: null,
  firstCodexHeadersMs: null,
  firstUpstreamSseEventMs: null,
  firstSemanticCommitmentMs: null,
  streamTerminalMs: null,
  attemptedProviders: [],
  removedProviderTriggerClass: null,
  removedProviderCircuitTransition: null,
  removedProviderSelectedModel: null,
  removedProviderTaskType: null,
  removedProviderSemanticCommitment: null,
  removedProviderLatencyMs: null,
  removedProviderTerminalStatus: null,
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
  downstreamSignal: context?.downstreamSignal,
  beforeProviderDispatch: context?.beforeProviderDispatch,
  onTerminalUsage: context?.onTerminalUsage,
  responseTelemetry: state,
});

const attachResponseTelemetry = (response: Response, state: ResponseTelemetryState): Response => {
  state.provider ??= response.headers.get("x-uos-upstream") || "gateway";
  responseTelemetry.set(response, state);
  return response;
};

const sumTelemetryCounts = (
  states: readonly ResponseTelemetryState[],
  key: "inputTokens" | "cachedInputTokens" | "cacheWriteInputTokens" | "outputTokens" | "totalTokens",
  expectedCount: number,
): number | null => {
  if (states.length !== expectedCount || states.some((state) => state[key] === null)) return null;
  return states.reduce((total, state) => total + (state[key] as number), 0);
};

const commonTelemetryValue = <T>(values: readonly T[]): T | null => {
  if (values.length === 0) return null;
  const first = values[0] as T;
  return values.every((value) => Object.is(value, first)) ? first : null;
};

const aggregateResponseTelemetry = (
  sources: readonly Response[],
  target: Response,
): Response => {
  const states = sources.flatMap((source) => {
    const state = responseTelemetry.get(source);
    return state ? [state] : [];
  });
  if (states.length === 0) return target;
  if (states.length === 1 && sources.length === 1) {
    responseTelemetry.set(target, states[0]);
    return target;
  }

  const aggregate = createResponseTelemetryState();
  const providers = [...new Set(states.map((state) => state.provider).filter((value): value is string => !!value))];
  aggregate.provider = providers.length === 1 ? providers[0] : providers.length > 1 ? "mixed" : null;
  aggregate.fallbackReason = commonTelemetryValue(states.map((state) => state.fallbackReason));
  aggregate.model = commonTelemetryValue(states.map((state) => state.model));
  aggregate.reasoning = commonTelemetryValue(states.map((state) => state.reasoning));
  aggregate.outputTokenAllowance = commonTelemetryValue(states.map((state) => state.outputTokenAllowance));
  aggregate.inputTokens = sumTelemetryCounts(states, "inputTokens", sources.length);
  aggregate.cachedInputTokens = sumTelemetryCounts(states, "cachedInputTokens", sources.length);
  aggregate.cacheWriteInputTokens = sumTelemetryCounts(states, "cacheWriteInputTokens", sources.length);
  aggregate.outputTokens = sumTelemetryCounts(states, "outputTokens", sources.length);
  aggregate.totalTokens = sumTelemetryCounts(states, "totalTokens", sources.length);
  aggregate.usageObserved = states.some((state) => state.usageObserved);
  aggregate.usageTelemetryStatus = states.some((state) => state.usageTelemetryStatus === "invalid")
    ? "invalid"
    : states.length !== sources.length
    ? aggregate.usageObserved ? "partial" : "missing"
    : states.every((state) => state.usageTelemetryStatus === "reported")
    ? "reported"
    : aggregate.usageObserved
    ? "partial"
    : "missing";
  aggregate.promptCacheKeyPresent = states.some((state) => state.promptCacheKeyPresent);
  aggregate.promptCacheMode = commonTelemetryValue(states.map((state) => state.promptCacheMode)) ?? "unspecified";
  aggregate.explicitBreakpointCount = Math.max(0, ...states.map((state) => state.explicitBreakpointCount));
  aggregate.accountSlot = commonTelemetryValue(states.map((state) => state.accountSlot));
  aggregate.accountCohortId = commonTelemetryValue(states.map((state) => state.accountCohortId));
  aggregate.affinityOutcome = commonTelemetryValue(states.map((state) => state.affinityOutcome)) ?? "failover";
  const usedPercents = states.map((state) => state.quotaUsedPercent).filter((value): value is number =>
    typeof value === "number"
  );
  aggregate.quotaUsedPercent = usedPercents.length > 0
    ? Math.max(...usedPercents)
    : states.some((state) => state.quotaUsedPercent === null)
    ? null
    : undefined;
  aggregate.completed = states.length === sources.length && states.every((state) => state.completed);
  aggregate.semanticOutputObserved = states.some((state) => state.semanticOutputObserved === true)
    ? true
    : states.every((state) => state.semanticOutputObserved === false)
    ? false
    : null;
  aggregate.upstreamEventKinds = [...new Set(states.flatMap((state) => state.upstreamEventKinds))];
  aggregate.streamTerminalType = aggregate.completed
    ? "response.completed"
    : commonTelemetryValue(states.map((state) => state.streamTerminalType));
  aggregate.failureKind = commonTelemetryValue(states.map((state) => state.failureKind));
  aggregate.responseCreatedObserved = states.length === sources.length &&
    states.every((state) => state.responseCreatedObserved);
  aggregate.syntheticTerminalType = commonTelemetryValue(states.map((state) => state.syntheticTerminalType));
  aggregate.stream = false;
  aggregate.providerRequestId = states.length === 1 ? states[0].providerRequestId : null;
  const earliestTiming = (
    key:
      | "firstProviderDispatchMs"
      | "firstProviderHeadersMs"
      | "firstCodexDispatchMs"
      | "firstCodexHeadersMs"
      | "firstUpstreamSseEventMs"
      | "firstSemanticCommitmentMs",
  ): number | null => {
    const values = states.map((state) => state[key]).filter((value): value is number => value !== null);
    return values.length > 0 ? Math.min(...values) : null;
  };
  aggregate.firstProviderDispatchMs = earliestTiming("firstProviderDispatchMs");
  aggregate.firstProviderHeadersMs = earliestTiming("firstProviderHeadersMs");
  aggregate.firstCodexDispatchMs = earliestTiming("firstCodexDispatchMs");
  aggregate.firstCodexHeadersMs = earliestTiming("firstCodexHeadersMs");
  aggregate.firstUpstreamSseEventMs = earliestTiming("firstUpstreamSseEventMs");
  aggregate.firstSemanticCommitmentMs = earliestTiming("firstSemanticCommitmentMs");
  const terminalTimes = states.map((state) => state.streamTerminalMs).filter((value): value is number =>
    value !== null
  );
  aggregate.streamTerminalMs = terminalTimes.length > 0 ? Math.max(...terminalTimes) : null;
  aggregate.attemptedProviders = [...new Set(states.flatMap((state) => state.attemptedProviders))];
  aggregate.removedProviderTriggerClass = commonTelemetryValue(
    states.map((state) => state.removedProviderTriggerClass),
  );
  aggregate.removedProviderCircuitTransition = commonTelemetryValue(
    states.map((state) => state.removedProviderCircuitTransition),
  );
  aggregate.removedProviderSelectedModel = commonTelemetryValue(
    states.map((state) => state.removedProviderSelectedModel),
  );
  aggregate.removedProviderTaskType = commonTelemetryValue(states.map((state) => state.removedProviderTaskType));
  aggregate.removedProviderSemanticCommitment = commonTelemetryValue(
    states.map((state) => state.removedProviderSemanticCommitment),
  );
  const removedProviderLatencies = states.map((state) => state.removedProviderLatencyMs).filter((
    value,
  ): value is number => value !== null);
  aggregate.removedProviderLatencyMs = removedProviderLatencies.length > 0
    ? Math.max(...removedProviderLatencies)
    : null;
  aggregate.removedProviderTerminalStatus = commonTelemetryValue(
    states.map((state) => state.removedProviderTerminalStatus),
  );
  return attachResponseTelemetry(target, aggregate);
};

export const getResponseTelemetry = (response: Response): ResponseTelemetry | null => {
  const state = responseTelemetry.get(response);
  if (!state) return null;
  return {
    provider: state.provider ?? (response.headers.get("x-uos-upstream") || "gateway"),
    fallbackReason: state.fallbackReason,
    model: state.model,
    reasoning: state.reasoning,
    outputTokenAllowance: state.outputTokenAllowance,
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
    semanticOutputObserved: state.semanticOutputObserved,
    upstreamEventKinds: [...state.upstreamEventKinds],
    streamTerminalType: state.streamTerminalType,
    failureKind: state.failureKind,
    responseCreatedObserved: state.responseCreatedObserved,
    syntheticTerminalType: state.syntheticTerminalType,
    stream: state.stream,
    providerRequestId: state.providerRequestId,
    firstProviderDispatchMs: state.firstProviderDispatchMs,
    firstProviderHeadersMs: state.firstProviderHeadersMs,
    firstCodexDispatchMs: state.firstCodexDispatchMs,
    firstCodexHeadersMs: state.firstCodexHeadersMs,
    firstUpstreamSseEventMs: state.firstUpstreamSseEventMs,
    firstSemanticCommitmentMs: state.firstSemanticCommitmentMs,
    streamTerminalMs: state.streamTerminalMs,
    attemptedProviders: [...state.attemptedProviders],
    removedProviderTriggerClass: state.removedProviderTriggerClass,
    removedProviderCircuitTransition: state.removedProviderCircuitTransition,
    removedProviderSelectedModel: state.removedProviderSelectedModel,
    removedProviderTaskType: state.removedProviderTaskType,
    removedProviderSemanticCommitment: state.removedProviderSemanticCommitment,
    removedProviderLatencyMs: state.removedProviderLatencyMs,
    removedProviderTerminalStatus: state.removedProviderTerminalStatus,
  };
};

/** Stable pseudonymous account identity used only by aggregate cache telemetry. */
export const getResponseAccountCohortId = (response: Response): string | null =>
  responseTelemetry.get(response)?.accountCohortId ?? null;

const recordAttemptedProvider = (context: UsageContext | undefined, provider: string): void => {
  const attempted = context?.responseTelemetry?.attemptedProviders;
  if (attempted && !attempted.includes(provider)) attempted.push(provider);
};

const selectRemovedProviderTelemetry = (context: UsageContext | undefined): void => {
  const telemetry = context?.responseTelemetry;
  if (!telemetry) return;
  telemetry.provider = "removed_provider";
  telemetry.accountSlot = null;
  telemetry.accountCohortId = null;
  telemetry.providerRequestId = null;
};

type ResponseTelemetryTimingField =
  | "firstProviderDispatchMs"
  | "firstProviderHeadersMs"
  | "firstCodexDispatchMs"
  | "firstCodexHeadersMs"
  | "firstUpstreamSseEventMs"
  | "firstSemanticCommitmentMs"
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

const recordRemovedProviderFields = (
  context: UsageContext | undefined,
  fields: Readonly<Record<string, string | number | null | undefined>>,
): void => {
  const telemetry = context?.responseTelemetry;
  if (!telemetry) return;
  if (fields.triggerClass !== undefined) telemetry.removedProviderTriggerClass = fields.triggerClass as string | null;
  if (fields.circuitTransition !== undefined) {
    telemetry.removedProviderCircuitTransition = fields.circuitTransition as string | null;
  }
  if (fields.selectedModel !== undefined) {
    telemetry.removedProviderSelectedModel = fields.selectedModel as string | null;
  }
  if (fields.taskType !== undefined) telemetry.removedProviderTaskType = fields.taskType as string | null;
  if (fields.semanticCommitment !== undefined) {
    telemetry.removedProviderSemanticCommitment = fields.semanticCommitment as string | null;
  }
  if (fields.latencyMs !== undefined) telemetry.removedProviderLatencyMs = fields.latencyMs as number | null;
  if (fields.terminalStatus !== undefined) {
    telemetry.removedProviderTerminalStatus = fields.terminalStatus as string | null;
  }
};

const persistRemovedProviderFields = (_context: UsageContext | undefined): Promise<void> =>
  recordRemovedProviderTelemetry({}).catch(() => {});

const persistFailedRemovedProviderAttempt = (
  context: UsageContext | undefined,
  _startedAtMonotonicMs: number,
  triggerClass: string,
): Promise<void> => {
  recordRemovedProviderFields(context, { triggerClass, terminalStatus: "failed_before_commit" });
  return persistRemovedProviderFields(context);
};

const recordFirstUpstreamSseEvent = (context: UsageContext | undefined): void => {
  recordResponseTiming(context, "firstUpstreamSseEventMs");
};

const recordFirstSemanticCommitment = (context: UsageContext | undefined): void => {
  recordResponseTiming(context, "firstSemanticCommitmentMs");
};

const recordStreamTerminal = (context: UsageContext | undefined): void => {
  recordResponseTiming(context, "streamTerminalMs");
};

const CHAT_RESPONSE_EVENT_KINDS = new Set([
  "response.created",
  "response.in_progress",
  "response.queued",
  "response.output_text.delta",
  "response.output_text.done",
  "response.refusal.delta",
  "response.refusal.done",
  "response.content_part.added",
  "response.content_part.done",
  "response.reasoning_summary_text.delta",
  "response.reasoning_summary_text.done",
  "response.reasoning_text.delta",
  "response.reasoning_text.done",
  "response.reasoning_summary_part.added",
  "response.reasoning_summary_part.done",
  "response.custom_tool_call_input.delta",
  "response.custom_tool_call_input.done",
  "response.output_item.added",
  "response.output_item.done",
  "response.function_call_arguments.delta",
  "response.function_call_arguments.done",
  "response.output",
  "response.completed",
  "response.failed",
  "response.incomplete",
  "error",
]);

const boundedResponseEventKind = (type: string): string => CHAT_RESPONSE_EVENT_KINDS.has(type) ? type : "unrecognized";

const boundedResponsesFailureKind = (value: unknown): string | null =>
  typeof value === "string" && /^[A-Za-z0-9_.:-]{1,128}$/.test(value) ? value : null;

const officialResponsesErrorKind = (value: unknown): string | null => {
  if (!isRecord(value) || Array.isArray(value)) return null;
  return boundedResponsesFailureKind(value.code) ?? boundedResponsesFailureKind(value.type);
};

const failureKindFromResponsesTerminal = (event: ResponsesStreamEvent): string | null => {
  if (event.type === "response.failed") {
    const response = isRecord(event.value.response) ? event.value.response : null;
    return officialResponsesErrorKind(response?.error) ??
      "response_failed";
  }
  if (event.type === "error") {
    return officialResponsesErrorKind(event.value.error) ?? boundedResponsesFailureKind(event.value.code) ??
      "upstream_error";
  }
  return null;
};

const recordResponsesEventTelemetry = (
  context: UsageContext | undefined,
  event: ResponsesStreamEvent,
): void => {
  const telemetry = context?.responseTelemetry;
  if (!telemetry) return;
  const eventKind = boundedResponseEventKind(event.type);
  if (!telemetry.upstreamEventKinds.includes(eventKind)) telemetry.upstreamEventKinds.push(eventKind);
  if (event.type === "response.created") telemetry.responseCreatedObserved = true;
  if (event.type === "response.incomplete") {
    const response = isRecord(event.value.response) ? event.value.response : null;
    const details = response && isRecord(response.incomplete_details) ? response.incomplete_details : null;
    const reason = details && typeof details.reason === "string" ? details.reason : null;
    if (reason && /^(?:gateway|provider|upstream|server|network|timeout|deadline)[A-Za-z0-9_.:-]*$/i.test(reason)) {
      telemetry.failureKind = `response_incomplete:${reason}`;
    }
  }
  if (
    (event.type === "response.failed" || event.type === "error") &&
    !isSyntheticResponsesFailureEvent(event)
  ) {
    telemetry.failureKind = failureKindFromResponsesTerminal(event);
  }
  if (isSyntheticResponsesFailureEvent(event)) {
    telemetry.syntheticTerminalType = event.type === "response.failed" || event.type === "error" ? event.type : null;
  }
};

const failureKindFromError = (error: unknown): ResponsesStreamFailureKind =>
  error instanceof ResponsesStreamError ? error.kind : "read_error";

const recordResponsesFailureTelemetry = (
  context: UsageContext | undefined,
  error: unknown,
  details?: OwnedResponsesStreamFailureDetails,
): void => {
  const telemetry = context?.responseTelemetry;
  if (!telemetry) return;
  telemetry.failureKind = details?.failureKind ?? failureKindFromError(error);
  if (details) {
    telemetry.responseCreatedObserved = details.responseCreatedObserved;
    telemetry.semanticOutputObserved = details.semanticCommitmentObserved;
    telemetry.syntheticTerminalType = details.syntheticTerminalType;
  }
};

const runWithResponseTelemetry = async (
  context: UsageContext | undefined,
  run: (context: UsageContext) => Promise<Response>,
): Promise<Response> => {
  const state = createResponseTelemetryState();
  const telemetryContext = withResponseTelemetryContext(context, state);
  const response = await run(telemetryContext);
  // Attempt-level terminal types can be abandoned during failover. Record a
  // precommit terminal time only after the handler has selected the final
  // response, and only when an upstream SSE event was actually observed.
  if (
    state.firstUpstreamSseEventMs !== null && state.firstSemanticCommitmentMs === null &&
    state.streamTerminalType !== null && state.streamTerminalMs === null
  ) {
    recordStreamTerminal(telemetryContext);
  }
  return attachResponseTelemetry(response, state);
};

type RoutedResponsesUpstream = Readonly<{
  response: Response;
  provider: UpstreamProvider;
  paidFallback: PaidFallbackReservation | null;
  paidFallbackBilling?: SurplusBillingPricing | null;
  /** Trustworthy opaque identifier supplied by the selected upstream. */
  paidFallbackProviderRequestId?: string | null;
  gatewayResponse: boolean;
  fallbackReason: InferenceFallbackReason | null;
  /** Local admission decisions are terminal and must not enter legacy recovery. */
  allowRemovedProviderRecovery?: false;
  /** Record stream health even though this free route has no paid reservation. */
  providerHealthOnly?: boolean;
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
  if (telemetry.firstSemanticCommitmentMs !== null) recordStreamTerminal(context);
};

const classifyStreamFailure = (
  error: unknown,
  signal: AbortSignal,
  downstreamSignal: AbortSignal,
): ResponseStreamTerminalType => {
  if (isTimeoutFailure(error, signal.reason, downstreamSignal.reason)) return "deadline";
  if (downstreamSignal.aborted) return "cancelled";
  if (signal.aborted) return "deadline";
  if (error instanceof ResponsesStreamError && error.kind === "inactivity_timeout") return "deadline";
  if (error instanceof ResponsesStreamError && error.kind === "premature_eof") return "eof";
  return "error";
};

const isTimeoutFailure = (...values: readonly unknown[]): boolean =>
  values.some((value) =>
    (value instanceof CodexError && value.code === "gateway_timeout") ||
    (value instanceof Error && value.name === "TimeoutError")
  );

const classifyPreHeaderFailure = (
  error: unknown,
  signal: AbortSignal,
  downstreamSignal: AbortSignal,
): ResponseStreamTerminalType => {
  if (isTimeoutFailure(error, signal.reason, downstreamSignal.reason)) return "deadline";
  if (downstreamSignal.aborted) return "cancelled";
  if (signal.aborted) return "deadline";
  return "error";
};

const streamErrorResponse = (
  status: number,
  message: string,
  code: string,
  provider: UpstreamProvider,
  warnings: readonly string[],
  type?: string,
  param?: string | null,
): Response => {
  const mergedWarnings = Array.from(new Set(warnings));
  const hasAuthWarning = mergedWarnings.includes(CODEX_AUTH_REAUTH_WARNING);
  const headers: Record<string, string> = { "x-uos-upstream": provider };
  if (mergedWarnings.length) headers["x-uos-warning"] = mergedWarnings.join(", ");
  return openaiError(
    status,
    hasAuthWarning ? message + " " + CODEX_AUTH_REAUTH_MESSAGE : message,
    code,
    { ...(type ? { type } : {}), ...(param !== undefined ? { param } : {}), headers },
  );
};

const streamPreflightFailureResponse = (
  terminalType: ResponseStreamTerminalType,
  provider: UpstreamProvider,
  warnings: readonly string[] = [],
): Response => {
  if (terminalType === "cancelled") {
    return streamErrorResponse(
      499,
      "Request was cancelled.",
      "request_cancelled",
      provider,
      warnings,
      "server_error",
      null,
    );
  }
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
  | "event_too_large"
  | "premature_eof"
  | "semantic_timeout"
  | "terminal_failure"
  | "empty_upstream_completion"
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
  abort: (reason?: unknown) => void;
  clearDeadline: () => void;
}>;

type FailedResponsesAttempt = Readonly<{
  provider: UpstreamProvider;
  response: Response;
  trigger: ResponsesAttemptTrigger;
  terminal?: ResponsesStreamEvent | null;
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
    if (error.kind === "event_too_large") return "event_too_large";
    if (error.kind === "malformed_event") return "malformed_event";
    if (error.kind === "premature_eof") return "premature_eof";
    if (error.kind === "inactivity_timeout") return "semantic_timeout";
  }
  return "read_error";
};

const failureKindForResponsesAttemptTrigger = (
  trigger: ResponsesAttemptTrigger,
): ResponsesStreamFailureKind | null => {
  switch (trigger) {
    case "http_5xx":
      return "upstream_http_5xx";
    case "premature_eof":
      return "premature_eof";
    case "malformed_event":
      return "malformed_event";
    case "event_too_large":
      return "event_too_large";
    case "semantic_timeout":
      return "inactivity_timeout";
    case "empty_upstream_completion":
      return "empty_upstream_completion";
    case "read_error":
    case "missing_body":
      return "read_error";
    default:
      return null;
  }
};

const safeFailedAttemptResponse = (
  response: Response,
  provider: UpstreamProvider,
  trigger: ResponsesAttemptTrigger,
  warnings: readonly string[],
): Response => {
  if (!response.ok) return response;
  if (trigger === "empty_upstream_completion") {
    return streamErrorResponse(
      502,
      "The upstream completed without visible output.",
      "empty_upstream_completion",
      provider,
      warnings,
      "server_error",
      null,
    );
  }
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
      "server_error",
      provider,
      warnings,
    );
  }
  return streamErrorResponse(
    502,
    "Upstream Responses stream ended unexpectedly.",
    "server_error",
    provider,
    warnings,
  );
};

const prepareResponsesAttempt = async (
  response: Response,
  provider: UpstreamProvider,
  deadline: StreamDeadline,
  requestSignal: AbortSignal,
  warnings: readonly string[],
  options: Readonly<{
    usageContext?: UsageContext;
    requireEligibleModel?: boolean;
    rejectFailedTerminal?: boolean;
    rejectPresemanticFailureTerminal?: boolean;
    releaseOnProgress?: boolean;
  }> = {},
): Promise<ResponsesAttemptResult> => {
  const fail = (
    trigger: ResponsesAttemptTrigger,
    failedResponse = response,
    terminal: ResponsesStreamEvent | null = null,
  ): ResponsesAttemptResult => {
    deadline.clear();
    return {
      kind: "failed",
      attempt: {
        provider,
        response: safeFailedAttemptResponse(failedResponse, provider, trigger, warnings),
        trigger,
        terminal,
        signal: deadline.signal,
        clearDeadline: deadline.clear,
      },
    };
  };
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
    firstEventTimeoutMs: Math.ceil(deadline.remainingMs()),
  });
  let preparedStream: PreparedResponsesStream | null = null;
  try {
    const prepared = await prepareResponsesStreamForCommit(iterator, {
      onEvent: (event) => {
        recordFirstUpstreamSseEvent(options.usageContext);
        recordResponsesEventTelemetry(options.usageContext, event);
      },
      releaseOnProgress: options.releaseOnProgress,
    });
    preparedStream = prepared;
    if (prepared.terminal?.type === "response.completed" && prepared.semantic === null) {
      await iterator.return("empty upstream completion").catch(() => {});
      return fail("empty_upstream_completion", response, prepared.terminal);
    }
    if (
      prepared.terminal &&
      ((options.rejectFailedTerminal &&
        (prepared.terminal.type === "response.failed" || prepared.terminal.type === "error") &&
        prepared.semantic === null) ||
        (options.rejectPresemanticFailureTerminal && prepared.semantic === null &&
          (prepared.terminal.type === "response.failed" || prepared.terminal.type === "error")))
    ) {
      deadline.clear();
      return fail(
        options.rejectPresemanticFailureTerminal && prepared.semantic === null &&
          (prepared.terminal.type === "response.failed" || prepared.terminal.type === "error")
          ? "terminal_failure"
          : "read_error",
      );
    }
    let responseId = responseIdFromEvents(prepared.buffered);
    let bufferedChars = prepared.bufferedChars;
    let discoveredTerminal = prepared.terminal;
    let selectedModel: string | null = null;
    let taskType: string | null = null;
    for (const event of prepared.buffered) {
      const candidate = removedProviderModelFromEvent(event.value);
      if (candidate) {
        if (selectedModel && selectedModel !== candidate) {
          await iterator.return("inconsistent model identity").catch(() => {});
          return fail(prepared.semantic ? "terminal_failure" : "invalid_model");
        }
        selectedModel = candidate;
      }
      if (!taskType && isRecord(event.value.response)) {
        taskType = removedProviderTaskTypeFromResponse(event.value.response);
      }
    }
    while (options.requireEligibleModel && (!selectedModel || !responseId) && !discoveredTerminal) {
      const next = await iterator.next();
      if (next.done || !next.value) break;
      recordResponsesEventTelemetry(options.usageContext, next.value);
      bufferedChars = appendResponsesPrecommitEvent(prepared.buffered, next.value, bufferedChars);
      const candidateResponseId = responseIdFromEvents([next.value]);
      if (candidateResponseId && responseId && candidateResponseId !== responseId) {
        await iterator.return("inconsistent response identity").catch(() => {});
        return fail(prepared.semantic ? "terminal_failure" : "malformed_event");
      }
      responseId ??= candidateResponseId;
      const candidate = removedProviderModelFromEvent(next.value.value);
      if (candidate) {
        if (selectedModel && selectedModel !== candidate) {
          await iterator.return("inconsistent model identity").catch(() => {});
          return fail(prepared.semantic ? "terminal_failure" : "invalid_model");
        }
        selectedModel = candidate;
      }
      if (!taskType && isRecord(next.value.value.response)) {
        taskType = removedProviderTaskTypeFromResponse(next.value.value.response);
      }
      if (next.value.terminal) discoveredTerminal = next.value;
    }
    if (
      options.rejectFailedTerminal && discoveredTerminal &&
      (discoveredTerminal.type === "response.failed" || discoveredTerminal.type === "error") &&
      prepared.semantic === null
    ) {
      await iterator.return("failed terminal before release").catch(() => {});
      return fail("read_error");
    }
    if (options.requireEligibleModel && !prepared.buffered.some((event) => event.type === "response.created")) {
      await iterator.return("missing response.created").catch(() => {});
      return fail(prepared.semantic ? "terminal_failure" : "malformed_event");
    }
    if (options.requireEligibleModel && !responseId) {
      await iterator.return("missing response id").catch(() => {});
      return fail(prepared.semantic ? "terminal_failure" : "malformed_event");
    }
    deadline.clear();
    if (options.requireEligibleModel && (!selectedModel || !isEligibleRemovedProviderModel(selectedModel))) {
      await iterator.return("invalid selected model").catch(() => {});
      return fail(prepared.semantic ? "terminal_failure" : "invalid_model");
    }
    const sanitizedBuffered = options.requireEligibleModel
      ? prepared.buffered.map((event) => {
        const value = stripRemovedProviderMetadata(event.value);
        return value === event.value ? event : responseEventFromValue(value);
      })
      : prepared.buffered;
    const sanitizedTerminal = discoveredTerminal
      ? sanitizedBuffered[prepared.buffered.indexOf(discoveredTerminal)] ?? discoveredTerminal
      : null;
    const sanitizedIterator = options.requireEligibleModel
      ? (async function* (): ResponsesStreamIterator {
        for await (const event of iterator) {
          const value = stripRemovedProviderMetadata(event.value);
          yield value === event.value ? event : responseEventFromValue(value);
        }
        return undefined;
      })()
      : iterator;
    return {
      kind: "ready",
      attempt: {
        provider,
        response,
        prepared: {
          ...prepared,
          iterator: sanitizedIterator,
          buffered: sanitizedBuffered,
          bufferedChars,
          terminal: sanitizedTerminal,
        },
        responseId,
        selectedModel,
        taskType,
        signal: deadline.signal,
        abort: deadline.abort,
        clearDeadline: deadline.clear,
      },
    };
  } catch (error) {
    await preparedStream?.iterator.return(error).catch(() => {});
    deadline.clear();
    if (requestSignal.aborted) throw requestSignal.reason ?? error;
    return fail(preparedStream?.semantic ? "terminal_failure" : triggerForResponsesError(error, deadline.signal));
  }
};

type ResponsesRouteAttempt = Readonly<{
  routed: RoutedResponsesUpstream;
  prepared: PreparedResponsesAttempt;
  lifecycle: MeteredTransportLifecycle;
}>;

type ResponsesRouteFailure = Readonly<{
  routed: RoutedResponsesUpstream;
  failed: FailedResponsesAttempt;
  lifecycle: MeteredTransportLifecycle;
}>;

const responseFailureTerminalType = (
  trigger: ResponsesAttemptTrigger,
  signal: AbortSignal,
  downstreamSignal: AbortSignal,
): ResponseStreamTerminalType => {
  if (trigger === "semantic_timeout" || isTimeoutFailure(signal.reason, downstreamSignal.reason)) return "deadline";
  if (downstreamSignal.aborted) return "cancelled";
  if (signal.aborted) return "deadline";
  if (trigger === "premature_eof") return "eof";
  if (trigger === "terminal_failure" || trigger === "empty_upstream_completion") return "response.failed";
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
    downstreamSignal: AbortSignal;
    warnings: readonly string[];
    attemptDeadline: StreamDeadline;
    fallbackSignal?: AbortSignal;
    createFallbackDeadline?: () => StreamDeadline;
    rejectPresemanticFailureTerminal?: boolean;
    releaseOnProgress?: boolean;
  }>,
): Promise<{ kind: "ready"; value: ResponsesRouteAttempt } | { kind: "failed"; value: ResponsesRouteFailure }> => {
  const deadline = options.attemptDeadline;
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
      fallbackSignal: options.fallbackSignal,
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
          lifecycle: createMeteredTransportLifecycle(null),
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
  let preparationDeadline = deadline;
  if (routed.provider !== "chatgpt_codex" && options.createFallbackDeadline) {
    deadline.clear();
    preparationDeadline = options.createFallbackDeadline();
  }
  const lifecycle = createMeteredTransportLifecycle(
    routed.paidFallback,
    routed.provider,
    routed.paidFallbackProviderRequestId ?? null,
    routed.paidFallbackBilling ?? null,
    options.model,
    routed.providerHealthOnly === true,
  );
  if (routed.gatewayResponse) {
    preparationDeadline.clear();
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
          signal: preparationDeadline.signal,
          clearDeadline: preparationDeadline.clear,
        },
      },
    };
  }
  let prepared: ResponsesAttemptResult;
  try {
    prepared = await prepareResponsesAttempt(
      routed.response,
      routed.provider,
      preparationDeadline,
      options.requestSignal,
      [...options.warnings, ...responseWarnings(routed.response)],
      {
        usageContext: options.usageContext,
        rejectPresemanticFailureTerminal: options.rejectPresemanticFailureTerminal,
        releaseOnProgress: options.releaseOnProgress === true && supportsReasoningProgressRelease(routed.provider),
      },
    );
  } catch (error) {
    if (options.requestSignal.aborted) {
      await finalizeAbandonedPrimaryAttempt(routed, lifecycle, {
        cancelled: classifyPreHeaderFailure(
          error,
          options.requestSignal,
          options.downstreamSignal,
        ) === "cancelled",
      });
    }
    throw error;
  }
  if (prepared.kind === "ready") {
    return { kind: "ready", value: { routed, prepared: prepared.attempt, lifecycle } };
  }
  return { kind: "failed", value: { routed, failed: prepared.attempt, lifecycle } };
};

const fetchAndPrepareRemovedProviderResponses = async (
  body: Record<string, unknown>,
  options: Readonly<{
    usageContext?: UsageContext;
    requestSignal: AbortSignal;
    sessionId: string | null;
    apiKey: string;
    attemptDeadline: StreamDeadline;
  }>,
): Promise<ResponsesAttemptResult> => {
  const deadline = options.attemptDeadline;
  recordAttemptedProvider(options.usageContext, "removed_provider");
  selectRemovedProviderTelemetry(options.usageContext);
  let response: Response;
  try {
    const result = await fetchRemovedProviderResponses(body, {
      apiKey: options.apiKey,
      sessionId: options.sessionId,
      signal: deadline.signal,
      timing: {
        onDispatch: () => recordFirstProviderDispatch(options.usageContext),
        onHeaders: () => recordFirstProviderHeaders(options.usageContext),
      },
      beforeDispatch: () => options.usageContext?.beforeProviderDispatch?.("removed_provider") ?? Promise.resolve(),
    });
    response = result.response;
  } catch (error) {
    deadline.clear();
    if (error instanceof ApiKeyQuotaDispatchError) throw error;
    if (options.requestSignal.aborted) throw options.requestSignal.reason ?? error;
    return {
      kind: "failed",
      attempt: {
        provider: "removed_provider",
        response: streamErrorResponse(
          502,
          "RemovedProvider request failed before response headers were received.",
          "server_error",
          "removed_provider",
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
    "removed_provider",
    deadline,
    options.requestSignal,
    [],
    {
      usageContext: options.usageContext,
      requireEligibleModel: true,
      rejectFailedTerminal: true,
    },
  );
};

const finalizeAbandonedPrimaryAttempt = async (
  routed: RoutedResponsesUpstream,
  lifecycle: MeteredTransportLifecycle,
  options: Readonly<{
    cancelled?: boolean;
    failureTrigger?: ResponsesAttemptTrigger;
  }> = {},
): Promise<void> => {
  if (routed.provider === "chatgpt_codex") {
    const transition = routed.response.ok && !options.cancelled
      ? markCodexResponseUpstreamError(routed.response)
      : releaseCodexResponseProbe(routed.response);
    await transition.catch(() => {});
  } else if ((routed.provider === "metered" || routed.provider === "surplus") && !routed.gatewayResponse) {
    if (options.cancelled) lifecycle.cancelled();
    else if (
      options.failureTrigger === "http_5xx" || options.failureTrigger === "terminal_failure" ||
      options.failureTrigger === "empty_upstream_completion"
    ) {
      lifecycle.terminal("response.failed");
    } else lifecycle.ambiguous();
  }
};

const markPrimarySemanticRecovery = (
  routed: RoutedResponsesUpstream,
  circuitProbe: RemovedProviderCircuitProbe | null,
  usageContext?: UsageContext,
  terminalType?: string | null,
): void => {
  if (!circuitProbe) return;
  const transition = routed.provider === "chatgpt_codex"
    ? terminalType === "response.failed"
      ? recordRemovedProviderEligibleFailure(circuitProbe)
      : closeRemovedProviderCircuit(circuitProbe)
    : releaseGlobalRemovedProviderProbe(circuitProbe);
  void transition.then((value) => {
    if (value !== "none") recordRemovedProviderFields(usageContext, { circuitTransition: value });
  }).catch(() => {});
};

const collectBufferedResponses = async (
  attempt: PreparedResponsesAttempt,
  options: Readonly<{
    warningModel?: string | null;
    usageContext?: UsageContext;
    onTerminal?: (event: ResponsesStreamEvent) => void;
    onEvent?: (event: ResponsesStreamEvent) => void;
    validateEvent?: (event: ResponsesStreamEvent) => void;
    onFailure?: (error: unknown, details?: OwnedResponsesStreamFailureDetails) => Response | void;
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
        onEvent: (event) => {
          recordResponsesEventTelemetry(options.usageContext, event);
          options.onEvent?.(event);
        },
        onFailure: (error, details) => {
          void options.onFailure?.(error, details);
        },
      });
      return readResponsesStream(stream);
    })()
    : (async function* (): AsyncGenerator<ResponsesStreamEvent> {
      for (const event of attempt.prepared.buffered) {
        options.validateEvent?.(event);
        recordResponsesEventTelemetry(options.usageContext, event);
        options.onEvent?.(event);
        yield event;
      }
      for await (const event of attempt.prepared.iterator) {
        options.validateEvent?.(event);
        recordResponsesEventTelemetry(options.usageContext, event);
        options.onEvent?.(event);
        yield event;
      }
    })();
  let finalResponse: Record<string, unknown> | null = null;
  let responseId = attempt.responseId;
  const deltaTextParts = new Map<string, string>();
  const doneTextParts = new Map<string, string>();
  const textPartOrder: string[] = [];
  let refusalText = "";
  const outputItems: Record<string, unknown>[] = [];
  const textPartKey = (value: Record<string, unknown>): string => {
    const itemId = getString(value.item_id)?.trim();
    if (itemId) return `item:${itemId}:${String(value.content_index ?? 0)}`;
    return `output:${String(value.output_index ?? 0)}:${String(value.content_index ?? 0)}`;
  };
  const rememberTextPart = (value: Record<string, unknown>, text: string, done: boolean): void => {
    if (!text) return;
    const key = textPartKey(value);
    if (!textPartOrder.includes(key)) textPartOrder.push(key);
    if (done) {
      const deltaText = deltaTextParts.get(key) ?? "";
      // A done event normally repeats the complete text accumulated by its
      // deltas. Some upstreams instead send a conflicting fragment; retain
      // the delta text in that case, matching the owned stream reconciler.
      if (!deltaText || text.startsWith(deltaText)) doneTextParts.set(key, text);
      return;
    }
    deltaTextParts.set(key, `${deltaTextParts.get(key) ?? ""}${text}`);
  };
  try {
    for await (const event of initial) {
      const ev = event.value;
      const eventResponseId = responseIdFromEvents([event]);
      if (eventResponseId && responseId && eventResponseId !== responseId) {
        throw new ResponsesStreamError("Upstream Responses stream changed response identifiers.", {
          kind: "malformed_event",
        });
      }
      responseId ??= eventResponseId;
      if (
        event.type === "response.output_text.delta" &&
        !(options.warningModel && ev.output_index === 0)
      ) rememberTextPart(ev, getString(ev.delta) ?? "", false);
      if (
        event.type === "response.output_text.done" &&
        !(options.warningModel && ev.output_index === 0)
      ) rememberTextPart(ev, getString(ev.text) ?? "", true);
      if (event.type === "response.refusal.delta") refusalText += getString(ev.delta) ?? "";
      if (event.type === "response.refusal.done" && !refusalText) refusalText = getString(ev.refusal) ?? "";
      if (event.type === "response.output_item.done" && isRecord(ev.item)) outputItems.push(ev.item);
      if (event.type === "response.output") {
        const output = ev.output ?? (isRecord(ev.response) ? ev.response.output : undefined);
        if (Array.isArray(output)) outputItems.push(...output.filter(isRecord));
      }
      if (event.type === "error") {
        options.onTerminal?.(event);
        const code = getString(ev.code) ?? "server_error";
        const message = getString(ev.message) ?? "Upstream Responses stream ended unexpectedly.";
        return streamErrorResponse(
          502,
          message,
          code,
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
    const failureResponse = options.onFailure?.(error);
    if (failureResponse) return failureResponse;
    return streamErrorResponse(
      502,
      "Upstream Responses stream ended unexpectedly.",
      "server_error",
      attempt.provider,
      [],
    );
  }
  if (!finalResponse) {
    return streamErrorResponse(
      502,
      "Upstream Responses stream ended unexpectedly.",
      "server_error",
      attempt.provider,
      [],
    );
  }
  const outputText = textPartOrder.map((key) => doneTextParts.get(key) ?? deltaTextParts.get(key) ?? "").join("");
  finalResponse = withAccumulatedResponseItems(finalResponse, outputItems);
  finalResponse = withAccumulatedResponseText(finalResponse, outputText, options.warningModel ? 1 : 0);
  finalResponse = withAccumulatedResponseRefusal(finalResponse, refusalText, options.warningModel ? 1 : 0);
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
    | "MeteredError"
    | "SurplusError"
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
  "metered_api_key_missing",
  "metered_pricing_unavailable",
  "metered_pricing_invalid",
  "metered_status_unavailable",
  "metered_status_invalid",
  "metered_request_invalid",
  "metered_upstream_unreachable",
  "metered_logs_unavailable",
  "metered_logs_invalid",
  "surplus_api_key_missing",
  "surplus_request_invalid",
  "surplus_upstream_unreachable",
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
  } else if (error instanceof MeteredError) {
    errorClass = "MeteredError";
    status = redactedDiagnosticStatus(error.status);
    code = REDACTED_UPSTREAM_DIAGNOSTIC_CODES.has(error.code) ? error.code : null;
  } else if (error instanceof SurplusError) {
    errorClass = "SurplusError";
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

const MAX_PROVIDER_REQUEST_ID_CHARS = 256;

const normalizeProviderRequestId = (value: unknown): string | null => {
  if (typeof value !== "string") return null;
  const requestId = value.trim();
  if (!requestId || requestId.length > MAX_PROVIDER_REQUEST_ID_CHARS) return null;
  for (const character of requestId) {
    const code = character.codePointAt(0) ?? 0;
    if (code < 0x20 || code === 0x7f) return null;
  }
  return requestId;
};

const providerRequestIdFromResponse = (response: Response): string | null => {
  const requestId = response.headers.get("X-Request-Id") ??
    response.headers.get("X-Api-Request-Id") ??
    response.headers.get("X-Oneapi-Request-Id");
  return normalizeProviderRequestId(requestId);
};

const toCodexErrorResponse = (error: unknown, provider?: string | null): Response => {
  let response: Response;
  if (error instanceof ApiKeyQuotaDispatchError) {
    response = openaiError(error.status, error.message, error.code, {
      type: error.errorType,
      headers: error.headers,
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
    const paidProvider = provider === "metered" || provider === "surplus" ? provider : null;
    const providerLabel = paidProvider === "surplus" ? "Surplus" : paidProvider === "metered" ? "Metered" : "Codex";
    const message = detail
      ? `${providerLabel} upstream request failed: ${detail}`
      : `${providerLabel} upstream request failed.`;
    response = paidProvider
      ? openaiError(502, message, `${paidProvider}_upstream_unreachable`, {
        type: "server_error",
        param: null,
      })
      : openaiError(502, message, "codex_upstream_unreachable");
  }
  return withUpstreamProviderHeader(response, provider);
};

const toPreHeaderErrorResponse = (
  error: unknown,
  terminalType: ResponseStreamTerminalType,
  provider?: string | null,
): Response => {
  if (terminalType === "cancelled") {
    return withUpstreamProviderHeader(
      openaiError(499, "Request was cancelled.", "request_cancelled", {
        type: "server_error",
        param: null,
      }),
      provider,
    );
  }
  if (
    terminalType === "deadline" &&
    !(error instanceof CodexError && error.code === "gateway_timeout")
  ) {
    return withUpstreamProviderHeader(
      openaiError(504, "Upstream request exceeded the gateway deadline.", "gateway_timeout", {
        type: "server_error",
        param: null,
      }),
      provider,
    );
  }
  return toCodexErrorResponse(error, provider);
};

const toCerebrasErrorResponse = (error: unknown): Response => {
  let response: Response;
  if (error instanceof ApiKeyQuotaDispatchError) {
    response = openaiError(error.status, error.message, error.code, {
      type: error.errorType,
      headers: error.headers,
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

const cerebrasChatCompletionHasSemanticOutput = (completion: Record<string, unknown>): boolean =>
  Array.isArray(completion.choices) && completion.choices.some((choice) => {
    if (!isRecord(choice) || Array.isArray(choice) || !isRecord(choice.message) || Array.isArray(choice.message)) {
      return false;
    }
    const message = choice.message;
    return (typeof message.content === "string" && message.content.length > 0) ||
      (typeof message.refusal === "string" && message.refusal.length > 0) ||
      (Array.isArray(message.tool_calls) && message.tool_calls.length > 0);
  });

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
    // Native Cerebras stream deltas carry reasoning in the leading chunk;
    // mirror that 1:1 (compliance D1) instead of dropping it.
    if (typeof message.reasoning === "string" && message.reasoning) delta.reasoning = message.reasoning;
    if (typeof message.content === "string") delta.content = message.content;
    if (typeof message.refusal === "string" && message.refusal) delta.refusal = message.refusal;

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
// HTTP semantics clients need and forward ONLY the standard OpenAI error
// fields (message/code, bounded + whitelisted) so 1:1 behavior is
// debuggable (compliance D2) — never reflect the arbitrary upstream body.
const CEREBRAS_UPSTREAM_ERROR_MESSAGE_MAX = 1_000;
const CEREBRAS_UPSTREAM_ERROR_CODE_MAX = 200;

const parseCerebrasUpstreamErrorDetail = (
  body: unknown,
): { message?: string; code?: string } => {
  if (!isRecord(body) || Array.isArray(body)) return {};
  const error = isRecord(body.error) && !Array.isArray(body.error) ? body.error : null;
  const pickString = (value: unknown, max: number): string | undefined => {
    if (typeof value !== "string") return undefined;
    const trimmed = value.slice(0, max).trim();
    return trimmed ? trimmed : undefined;
  };
  return {
    message: pickString(
      error?.message ?? body.message,
      CEREBRAS_UPSTREAM_ERROR_MESSAGE_MAX,
    ),
    code: pickString(
      error?.code ?? body.code,
      CEREBRAS_UPSTREAM_ERROR_CODE_MAX,
    ),
  };
};

const toCerebrasUpstreamErrorResponse = async (
  upstream: Response,
  signal?: AbortSignal,
): Promise<Response> => {
  // Read the error body under the shared bounded ceiling (64 KiB / 1 s) so a
  // stalled upstream cannot extend the gateway request; only message/code are
  // ever forwarded.
  let detail: { message?: string; code?: string } = {};
  try {
    const captured = await readBoundedResponseBody(upstream, {
      signal,
      maxBytes: BOUNDED_RESPONSE_BODY_MAX_BYTES,
      timeoutMs: BOUNDED_RESPONSE_BODY_TIMEOUT_MS,
      cancellationReason: "Cerebras upstream error body",
    });
    if (captured.complete && captured.bytes.length > 0) {
      try {
        detail = parseCerebrasUpstreamErrorDetail(
          JSON.parse(new TextDecoder().decode(captured.bytes)) as unknown,
        );
      } catch {
        // Non-JSON error body: keep the generic message (never reflect it).
      }
    }
  } catch {
    // Bounded read failure must not change the error semantics.
  } finally {
    cancelResponseBody(upstream);
  }
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
    detail.message ?? "Cerebras upstream returned an error.",
    detail.code ?? "cerebras_upstream_error",
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

const logPaidProviderSelected = (
  requestId: string,
  reason: InferenceFallbackReason,
  provider: "metered" | "surplus",
): void => {
  try {
    if (provider === "metered") {
      // Keep the established Metered event shape for existing log consumers.
      console.info("[ai.ubq.fi] metered_selected", JSON.stringify({ request_id: requestId, reason }));
    } else {
      console.info(
        "[ai.ubq.fi] paid_provider_selected",
        JSON.stringify({ request_id: requestId, reason, provider }),
      );
    }
  } catch {
    // Routing telemetry must never alter provider selection.
  }
};

const logPaidProviderAdmissionRejected = (
  requestId: string,
  model: string,
  reason: string,
): void => {
  try {
    console.warn(
      "[ai.ubq.fi] paid_provider_admission_rejected",
      JSON.stringify({ request_id: requestId, model, reason }),
    );
  } catch {
    // Routing telemetry must never alter provider selection.
  }
};

const paidProviderAdmissionError = (reason: string): Response => {
  switch (reason) {
    case "disabled":
      return openaiError(
        403,
        "Paid-provider routing is disabled for this API key.",
        "paid_fallback_disabled",
      );
    case "limit_exceeded":
      return openaiError(
        429,
        "The API key's paid-provider limit is exhausted.",
        "paid_fallback_limit_exceeded",
      );
    case "provider_unconfigured":
      return openaiError(
        503,
        "No paid provider is configured for this model.",
        "paid_provider_unconfigured",
        { type: "server_error" },
      );
    case "model_not_priced":
      return openaiError(
        403,
        "This model is not admitted by the API key's paid-provider policy.",
        "paid_model_not_admitted",
      );
    case "reconciliation_pending":
      return openaiError(
        503,
        "Paid-provider billing reconciliation is pending.",
        "paid_fallback_reconciliation_pending",
        { type: "server_error" },
      );
    case "concurrent_update":
      return openaiError(
        503,
        "Paid-provider admission changed concurrently; retry the request.",
        "paid_fallback_concurrent_update",
        { type: "server_error" },
      );
    default:
      return openaiError(
        503,
        "Paid-provider admission is unavailable.",
        "paid_fallback_invalid_policy",
        { type: "server_error" },
      );
  }
};

const canAttemptPaidFallback = (context: UsageContext | undefined): boolean =>
  context?.paidFallbackEnabled === true &&
  Boolean(context?.keyId && context.requestId && context.startedAtMs !== undefined) &&
  (Boolean(readSurplusApiKey()) || Boolean(readMeteredApiKey()));

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

const paidProviderErrorStatus = (error: unknown): number | null =>
  error instanceof MeteredError || error instanceof SurplusError ? error.status : null;

const recordPaidProviderResponseHealth = async (
  provider: "metered" | "surplus",
  status: number | null,
  providerRequestId: string | null = null,
): Promise<void> => {
  try {
    const record = provider === "surplus" ? recordSurplusProviderHealth : recordMeteredProviderHealth;
    if (status === 401 || status === 403) await record("auth_invalid", status, Date.now, providerRequestId);
    else if (status === 402 || status === 429) {
      await record("quota_exhausted", status, Date.now, providerRequestId);
    } else if (status === null || status >= 500) {
      await record("upstream_error", status, Date.now, providerRequestId);
    } else if (status >= 100) await record("reachable", status, Date.now, providerRequestId);
  } catch {
    // Provider-health persistence must not change routing or response delivery.
  }
};

type MeteredTransportLifecycle = Readonly<{
  terminal: (eventType: string, usage?: UsageTokens | null) => void;
  ambiguous: () => void;
  cancelled: () => void;
}>;

const createMeteredTransportLifecycle = (
  reservation: PaidFallbackReservation | null,
  provider: UpstreamProvider = "metered",
  providerRequestId: string | null = null,
  surplusBilling: SurplusBillingPricing | null = null,
  model: string | null = null,
  providerHealthOnly = false,
): MeteredTransportLifecycle => {
  let recorded = false;
  const schedule = (
    operation: string,
    run: (reservation: PaidFallbackReservation) => Promise<void>,
  ): void => {
    if (!reservation || recorded) return;
    recorded = true;
    void bestEffortPaidFallbackBookkeeping(operation, () => run(reservation));
  };
  const scheduleProviderHealthOnly = (
    event: "success" | "upstream_error",
    status: number | null,
  ): void => {
    if (reservation || !providerHealthOnly || recorded) return;
    if (provider !== "metered" && provider !== "surplus") return;
    recorded = true;
    void bestEffortPaidFallbackBookkeeping(
      "unreserved provider health recording",
      () =>
        provider === "surplus"
          ? recordSurplusProviderHealth(event, status, Date.now, providerRequestId)
          : recordMeteredProviderHealth(event, status, Date.now, providerRequestId),
    );
  };
  return {
    terminal: (eventType, usage = null) => {
      const terminalState = eventType === "response.completed"
        ? "completed"
        : eventType === "response.failed" || eventType === "error"
        ? "failed"
        : eventType === "response.incomplete"
        ? "incomplete"
        : null;
      if (!terminalState) return;
      if (!reservation) {
        scheduleProviderHealthOnly(
          terminalState === "completed" ? "success" : "upstream_error",
          terminalState === "completed" ? 200 : null,
        );
        return;
      }
      schedule(
        "terminal reconciliation",
        async (activeReservation) => {
          const terminal = recordMeteredTerminal(
            activeReservation,
            terminalState,
            provider === "surplus" ? "surplus" : "metered",
          );
          const surplusSettlement = provider === "surplus" && surplusBilling && model && usage
            ? async (): Promise<void> => {
              await terminal;
              await recordSurplusUsage(
                activeReservation,
                providerRequestId ?? `surplus:${activeReservation.request_id}`,
                model,
                {
                  input_tokens: usage.inputTokens,
                  cached_input_tokens: usage.cachedInputTokens,
                  cache_write_input_tokens: usage.cacheWriteInputTokens,
                  output_tokens: usage.outputTokens,
                },
                surplusBilling,
              );
            }
            : () => terminal;
          await Promise.all([
            surplusSettlement(),
            provider === "surplus"
              ? recordSurplusProviderHealth(
                terminalState === "completed" ? "success" : "upstream_error",
                terminalState === "completed" ? 200 : null,
                Date.now,
                providerRequestId,
              )
              : recordMeteredProviderHealth(
                terminalState === "completed" ? "success" : "upstream_error",
                terminalState === "completed" ? 200 : null,
                Date.now,
                providerRequestId,
              ),
          ]);
        },
      );
    },
    ambiguous: () => {
      if (!reservation) {
        scheduleProviderHealthOnly("upstream_error", null);
        return;
      }
      schedule(
        "ambiguous failure recording",
        async (activeReservation) => {
          await Promise.all([
            recordMeteredAmbiguousFailure(
              activeReservation,
              provider === "surplus" ? "surplus" : "metered",
              providerRequestId,
            ),
            provider === "surplus"
              ? recordSurplusProviderHealth("upstream_error", null, Date.now, providerRequestId)
              : recordMeteredProviderHealth("upstream_error", null, Date.now, providerRequestId),
          ]);
        },
      );
    },
    cancelled: () => {
      if (!reservation && providerHealthOnly) {
        // A client cancellation is not provider degradation. Finalize this
        // lifecycle so a later stream race cannot replace the header result.
        recorded = true;
        return;
      }
      schedule(
        "dispatched cancellation recording",
        (activeReservation) =>
          recordMeteredTerminal(
            activeReservation,
            "cancelled",
            provider === "surplus" ? "surplus" : "metered",
          ),
      );
    },
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
    fallbackSignal?: AbortSignal;
  }>,
): Promise<RoutedResponsesUpstream> => {
  const fallbackSignal = options.fallbackSignal ?? options.signal;
  const telemetry = options.usageContext?.responseTelemetry;
  if (isTemporaryFreeSurplusModel(options.model)) {
    if (telemetry) {
      telemetry.provider = "surplus";
      telemetry.fallbackReason = null;
      telemetry.accountSlot = null;
      telemetry.accountCohortId = null;
      telemetry.providerRequestId = null;
      telemetry.quotaUsedPercent = null;
    }
    recordAttemptedProvider(options.usageContext, "surplus");
    let transportStarted = false;
    try {
      const result = await fetchSurplusResponses(body, {
        signal: options.signal,
        beforeDispatch: () => options.usageContext?.beforeProviderDispatch?.("surplus") ?? Promise.resolve(),
        onDispatch: () => {
          transportStarted = true;
          recordFirstProviderDispatch(options.usageContext);
        },
      });
      recordFirstProviderHeaders(options.usageContext);
      const providerRequestId = normalizeProviderRequestId(result.request_id);
      if (telemetry) telemetry.providerRequestId = providerRequestId;
      await recordPaidProviderResponseHealth("surplus", result.response.status, providerRequestId);
      return {
        response: result.response,
        provider: "surplus",
        paidFallback: null,
        paidFallbackProviderRequestId: providerRequestId,
        gatewayResponse: false,
        fallbackReason: null,
        providerHealthOnly: result.response.ok,
      };
    } catch (error) {
      if (error instanceof ApiKeyQuotaDispatchError) throw error;
      const timedOut = isTimeoutFailure(error, options.signal?.reason);
      if (options.signal?.aborted && !timedOut) throw error;
      const status = paidProviderErrorStatus(error);
      if (transportStarted) await recordPaidProviderResponseHealth("surplus", status);
      if (options.signal?.aborted) throw error;
      const response = timedOut
        ? openaiError(
          504,
          "Surplus upstream exceeded the gateway deadline before response headers were received.",
          "gateway_timeout",
          { type: "server_error", headers: { "x-uos-upstream": "surplus" } },
        )
        : error instanceof SurplusError
        ? openaiError(error.status, error.message, error.code, {
          type: "server_error",
          headers: { "x-uos-upstream": "surplus" },
        })
        : openaiError(
          502,
          "Surplus upstream request failed before response headers were received.",
          "upstream_error",
          { type: "server_error", headers: { "x-uos-upstream": "surplus" } },
        );
      return {
        response,
        provider: "surplus",
        paidFallback: null,
        gatewayResponse: true,
        fallbackReason: null,
      };
    }
  }
  const codexCatalog = await loadCodexModelsSnapshot();
  const codexModelKnown = codexCatalog?.models.some((model) => {
    const record = model as Record<string, unknown>;
    return (getString(record.slug) ?? getString(record.id) ?? getString(record.model) ?? getString(record.name)) ===
      options.model;
  }) === true;
  // Cached discovery must not delay the primary Codex transport. A cold
  // discovery is only needed before dispatch when the model is not in the
  // Codex roster; known Codex models can use the historical Metered path and
  // refresh paid catalogs after a fallback-triggering primary response.
  let [meteredCatalog, surplusCatalog] = await Promise.all([
    fetchMeteredModels({ cachedOnly: true }),
    fetchSurplusModels({ cachedOnly: true }),
  ]);
  const meteredCatalogNeedsRefresh = (): boolean =>
    meteredCatalog === null || Date.now() - meteredCatalog.updated_at_ms >= METERED_MODELS_CACHE_TTL_MS;
  const surplusCatalogNeedsRefresh = (): boolean =>
    surplusCatalog === null || Date.now() - surplusCatalog.updated_at_ms >= SURPLUS_MODELS_CACHE_TTL_MS;
  const endpointType = options.route === "responses" ? "openai-response" : "openai";
  const requestUsesTools = Array.isArray(body.tools) && body.tools.length > 0;
  const routingState = (): Readonly<{
    surplusBilling: SurplusBillingPricing | null;
    paidProviders: readonly ("metered" | "surplus")[];
    paidModelKnown: boolean;
    meteredOnly: boolean;
  }> => {
    const meteredModelSupportsRoute =
      meteredCatalog?.models.some((model) =>
        model.id === options.model && model.supported_endpoint_types.includes(endpointType)
      ) === true;
    const surplusModelSupportsRoute =
      surplusCatalog?.models.some((model) =>
        model.id === options.model && model.supported_endpoint_types.includes(endpointType)
      ) === true;
    const surplusModel = surplusCatalog?.models.find((model) => model.id === options.model) ?? null;
    const surplusInputPrice = surplusModel?.input_price_per_token;
    const surplusOutputPrice = surplusModel?.output_price_per_token;
    const surplusBilling: SurplusBillingPricing | null =
      surplusInputPrice !== undefined && Number.isFinite(surplusInputPrice) && surplusInputPrice >= 0 &&
        surplusOutputPrice !== undefined && Number.isFinite(surplusOutputPrice) && surplusOutputPrice >= 0
        ? {
          input_price_per_token: surplusInputPrice,
          output_price_per_token: surplusOutputPrice,
          ...(surplusModel?.cache_read_price_per_token === undefined
            ? {}
            : { cache_read_price_per_token: surplusModel.cache_read_price_per_token }),
          ...(surplusModel?.cache_write_price_per_token === undefined
            ? {}
            : { cache_write_price_per_token: surplusModel.cache_write_price_per_token }),
        }
        : null;
    // A known Codex model retains the historical OpenLux roster path even when
    // its discovery request is temporarily unavailable. Surplus is selected
    // only when its own catalog proves that the exact model is routable.
    const meteredCanServe = Boolean(readMeteredApiKey()) &&
      (meteredCatalog === null ? codexModelKnown : meteredModelSupportsRoute);
    // Tool-bearing work needs explicit capability evidence from the exact
    // Surplus model record. Missing or partial metadata remains fail-closed.
    const surplusCanServe = (!requestUsesTools || surplusModel?.supports_tools === true) &&
      Boolean(readSurplusApiKey()) && surplusModelSupportsRoute && surplusBilling !== null;
    // The paid tiers have a fixed cost order for every model. Provider
    // availability may remove a tier, but it must never reverse the order.
    const preferredPaidProviders: readonly ("metered" | "surplus")[] = ["surplus", "metered"];
    const paidProviders = preferredPaidProviders.filter((provider) =>
      provider === "surplus" ? surplusCanServe : meteredCanServe
    );
    return {
      surplusBilling,
      paidProviders,
      paidModelKnown: meteredModelSupportsRoute || surplusModelSupportsRoute,
      meteredOnly: paidProviders.length > 0 && !codexModelKnown,
    };
  };
  const refreshStalePaidCatalogsInBackground = (): void => {
    if (meteredCatalog !== null && meteredCatalogNeedsRefresh()) {
      void fetchMeteredModels().catch(() => {});
    }
    if (surplusCatalog !== null && surplusCatalogNeedsRefresh()) {
      void fetchSurplusModels().catch(() => {});
    }
  };
  if (!codexModelKnown && (meteredCatalogNeedsRefresh() || surplusCatalogNeedsRefresh())) {
    [meteredCatalog, surplusCatalog] = await Promise.all([
      meteredCatalogNeedsRefresh() ? fetchMeteredModels({ signal: options.signal }) : Promise.resolve(meteredCatalog),
      surplusCatalogNeedsRefresh() ? fetchSurplusModels({ signal: options.signal }) : Promise.resolve(surplusCatalog),
    ]);
  }
  let { surplusBilling, paidProviders, paidModelKnown, meteredOnly } = routingState();
  if (paidProviders.length) refreshStalePaidCatalogsInBackground();
  const rejectDirectPaidAdmission = (
    provider: "metered" | "surplus",
    errorReason: string,
    logReason = errorReason,
  ): RoutedResponsesUpstream => {
    if (telemetry) {
      telemetry.provider = "gateway";
      telemetry.fallbackReason = "dynamic_paid_model";
    }
    logPaidProviderAdmissionRejected(
      options.usageContext?.requestId ?? "unknown",
      options.model,
      logReason,
    );
    return {
      response: paidProviderAdmissionError(errorReason),
      provider,
      paidFallback: null,
      gatewayResponse: true,
      fallbackReason: "dynamic_paid_model",
      allowRemovedProviderRecovery: false,
    };
  };
  const surplusModelSupportsRoute =
    surplusCatalog?.models.some((model) =>
      model.id === options.model && model.supported_endpoint_types.includes(endpointType)
    ) === true;
  const catalogPaidProvider: "metered" | "surplus" = surplusModelSupportsRoute ? "surplus" : "metered";
  if (!codexModelKnown && paidModelKnown && paidProviders.length === 0) {
    if (options.usageContext?.paidFallbackEnabled === false) {
      return rejectDirectPaidAdmission(catalogPaidProvider, "disabled");
    }
    const surplusModelWithoutToolProof = requestUsesTools && surplusModelSupportsRoute &&
      Boolean(readSurplusApiKey()) && surplusBilling !== null &&
      surplusCatalog?.models.some((model) => model.id === options.model && model.supports_tools !== true) === true;
    if (surplusModelWithoutToolProof) {
      if (telemetry) {
        telemetry.provider = "gateway";
        telemetry.fallbackReason = "dynamic_paid_model";
      }
      logPaidProviderAdmissionRejected(
        options.usageContext?.requestId ?? "unknown",
        options.model,
        "tool_capability_unverified",
      );
      return {
        response: openaiError(
          400,
          `Model '${options.model}' does not support tool calling through the configured providers.`,
          "model_tool_calling_unsupported",
          { param: "tools" },
        ),
        provider: "surplus",
        paidFallback: null,
        gatewayResponse: true,
        fallbackReason: "dynamic_paid_model",
        allowRemovedProviderRecovery: false,
      };
    }
    return rejectDirectPaidAdmission(catalogPaidProvider, "provider_unconfigured");
  }
  const directPaidProvider = meteredOnly ? paidProviders[0] : null;
  const directPaidPrimary = directPaidProvider
    ? openaiError(
      503,
      "Paid-provider routing did not reach the selected upstream.",
      "paid_provider_not_dispatched",
      { type: "server_error" },
    )
    : null;
  if (telemetry) telemetry.provider = directPaidProvider ?? "chatgpt_codex";
  if (!directPaidProvider) recordAttemptedProvider(options.usageContext, "chatgpt_codex");
  let primary: Response;
  const debugScenario = (await loadDebugRoutingConfig()).scenario;
  const forcedStatus = debugScenario === "metered_first" || debugScenario === "codex_429"
    ? 429
    : debugScenario === "codex_403"
    ? 403
    : debugScenario === "codex_401"
    ? 401
    : null;
  if (directPaidPrimary) {
    primary = directPaidPrimary;
  } else if (forcedStatus !== null) {
    primary = openaiError(
      forcedStatus,
      `Debug routing scenario forced Codex ${forcedStatus}.`,
      forcedStatus === 429 ? "rate_limit_error" : "debug_forced_codex",
      { headers: { "x-uos-upstream": "chatgpt_codex", "x-uos-debug-scenario": debugScenario } },
    );
  } else {
    try {
      primary = await fetchCodexResponses(body, {
        clientVersion: options.clientVersion,
        cacheScope: options.usageContext?.idempotencyPrincipal || options.usageContext?.keyId,
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
  }
  const primaryStatus = primary.status;
  const authReauthenticationPrimary = primaryStatus === 401 &&
    responseWarnings(primary).includes(CODEX_AUTH_REAUTH_WARNING);
  if (telemetry) {
    telemetry.accountSlot = getCodexResponseSlot(primary);
    telemetry.accountCohortId = await getCodexResponseAccountCohortId(primary);
    telemetry.affinityOutcome = getCodexResponseAffinityOutcome(primary);
    telemetry.providerRequestId = providerRequestIdFromResponse(primary);
  }
  const routingError = getCodexRoutingError(primary);
  const gatewayResponse = directPaidProvider !== null || routingError === CODEX_QUOTA_BLOCKED_ERROR_CODE ||
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
  // Only a complete, authoritative Codex quota/capacity classification may
  // admit paid fallback. Generic 429 responses remain request-local.
  const fallbackReason: InferenceFallbackReason | null = directPaidProvider
    ? "dynamic_paid_model"
    : primaryStatus === 429 && routingError === CODEX_QUOTA_BLOCKED_ERROR_CODE
    ? "primary_quota_blocked"
    : null;
  if (telemetry) telemetry.fallbackReason = fallbackReason;
  if (directPaidProvider && options.usageContext?.paidFallbackEnabled === false) {
    return rejectDirectPaidAdmission(directPaidProvider, "disabled");
  }
  if (
    !fallbackReason ||
    options.usageContext?.paidFallbackEnabled === false ||
    !keyId ||
    !requestId ||
    createdAtMs === undefined
  ) {
    if (directPaidProvider) {
      return rejectDirectPaidAdmission(directPaidProvider, "invalid_policy", "invalid_context");
    }
    return {
      response: primary,
      provider: "chatgpt_codex",
      paidFallback: null,
      gatewayResponse,
      fallbackReason,
    };
  }
  if (fallbackSignal?.aborted) {
    if (primary) cancelResponseBody(primary);
    throw fallbackSignal.reason instanceof Error
      ? fallbackSignal.reason
      : new DOMException("The request was aborted.", "AbortError");
  }
  // A stale snapshot remains usable when it already selects a paid provider.
  // Missing catalogs must still be discovered before trusting that historical
  // provider path; otherwise Surplus-preferred models can bypass discovery.
  // When no provider is selectable, also re-discover expired catalogs so a
  // newly published model can still recover this request.
  if (
    meteredCatalog === null || surplusCatalog === null ||
    (!paidProviders.length && (meteredCatalogNeedsRefresh() || surplusCatalogNeedsRefresh()))
  ) {
    [meteredCatalog, surplusCatalog] = await Promise.all([
      meteredCatalogNeedsRefresh() ? fetchMeteredModels({ signal: fallbackSignal }) : Promise.resolve(meteredCatalog),
      surplusCatalogNeedsRefresh() ? fetchSurplusModels({ signal: fallbackSignal }) : Promise.resolve(surplusCatalog),
    ]);
    ({ surplusBilling, paidProviders, meteredOnly } = routingState());
  }
  if (paidProviders.length) refreshStalePaidCatalogsInBackground();
  if (!paidProviders.length) {
    return {
      response: primary,
      provider: "chatgpt_codex",
      paidFallback: null,
      gatewayResponse,
      fallbackReason,
    };
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
    allowUnrosteredModel: meteredOnly,
    reason: fallbackReason,
  } as const;
  let decision: Awaited<ReturnType<typeof reservePaidFallback>>;
  try {
    decision = await reservePaidFallback(reservationInput);
  } catch (error) {
    warnPaidFallbackBookkeepingFailure("admission", error);
    if (directPaidProvider) {
      return rejectDirectPaidAdmission(directPaidProvider, "invalid_policy", "admission_error");
    }
    return {
      response: primary,
      provider: "chatgpt_codex",
      paidFallback: null,
      gatewayResponse,
      fallbackReason: reservationInput.reason,
    };
  }
  if (decision.kind === "skip") {
    if (directPaidProvider) {
      return rejectDirectPaidAdmission(directPaidProvider, decision.reason);
    }
    return {
      response: primary,
      provider: "chatgpt_codex",
      paidFallback: null,
      gatewayResponse,
      fallbackReason: reservationInput.reason,
    };
  }
  if (decision.kind === "blocked") {
    if (directPaidProvider) {
      return rejectDirectPaidAdmission(directPaidProvider, decision.reason);
    }
    return {
      response: primary,
      provider: "chatgpt_codex",
      paidFallback: null,
      gatewayResponse,
      fallbackReason: reservationInput.reason,
    };
  }

  if (fallbackSignal?.aborted) {
    cancelResponseBody(primary);
    await bestEffortPaidFallbackBookkeeping(
      "prefetch cancellation recording",
      () => recordMeteredPrefetchCancellation(decision.reservation),
    );
    throw fallbackSignal.reason instanceof Error
      ? fallbackSignal.reason
      : new DOMException("The request was aborted.", "AbortError");
  }
  cancelResponseBody(primary);
  if (fallbackSignal?.aborted) {
    await bestEffortPaidFallbackBookkeeping(
      "prefetch cancellation recording",
      () => recordMeteredPrefetchCancellation(decision.reservation),
    );
    throw fallbackSignal.reason instanceof Error
      ? fallbackSignal.reason
      : new DOMException("The request was aborted.", "AbortError");
  }
  if (telemetry) {
    telemetry.provider = paidProviders[0];
    telemetry.accountSlot = null;
    telemetry.accountCohortId = null;
    telemetry.providerRequestId = null;
    telemetry.quotaUsedPercent = decision.reservation.quota_used_percent;
  }
  logPaidProviderSelected(requestId, fallbackReason, paidProviders[0]);
  const isAuthoritativeCapacityStatus = (status: number | null): boolean => status === 402 || status === 429;
  let result:
    | Awaited<ReturnType<typeof fetchMeteredResponses>>
    | Awaited<ReturnType<typeof fetchSurplusResponses>>
    | null = null;
  let selectedProvider: "metered" | "surplus" = paidProviders[0];
  let providerError: unknown = null;
  let previousRespondingProvider: "metered" | "surplus" | null = null;
  let previousProviderRequestId: string | null = null;
  const retainRespondingProviderTelemetry = (
    provider: "metered" | "surplus" | null,
    providerRequestId: string | null,
  ): void => {
    if (!telemetry || provider === null) return;
    telemetry.provider = provider;
    telemetry.providerRequestId = providerRequestId;
  };
  for (const [providerIndex, provider] of paidProviders.entries()) {
    if (fallbackSignal?.aborted) {
      retainRespondingProviderTelemetry(previousRespondingProvider, previousProviderRequestId);
      await bestEffortPaidFallbackBookkeeping(
        previousRespondingProvider === null
          ? "prefetch cancellation recording"
          : "inter-provider cancellation ambiguity recording",
        () =>
          previousRespondingProvider === null
            ? recordMeteredPrefetchCancellation(decision.reservation)
            : recordMeteredAmbiguousFailure(
              decision.reservation,
              previousRespondingProvider,
              previousProviderRequestId,
            ),
      );
      throw fallbackSignal.reason instanceof Error
        ? fallbackSignal.reason
        : new DOMException("The request was aborted.", "AbortError");
    }
    selectedProvider = provider;
    if (telemetry) {
      telemetry.provider = provider;
      telemetry.providerRequestId = null;
    }
    recordAttemptedProvider(options.usageContext, provider);
    let transportStarted = false;
    try {
      const candidate = provider === "surplus"
        ? await fetchSurplusResponses(body, {
          signal: fallbackSignal,
          supportsParallelToolCalls: surplusCatalog?.models.find((model) =>
            model.id === options.model
          )?.supports_parallel_tool_calls === true,
          beforeDispatch: () => options.usageContext?.beforeProviderDispatch?.("surplus") ?? Promise.resolve(),
          onDispatch: () => {
            transportStarted = true;
            recordFirstProviderDispatch(options.usageContext);
          },
        })
        : await fetchMeteredResponses(body, {
          signal: fallbackSignal,
          beforeDispatch: () => options.usageContext?.beforeProviderDispatch?.("metered") ?? Promise.resolve(),
          onDispatch: () => {
            transportStarted = true;
            recordFirstProviderDispatch(options.usageContext);
          },
        });
      recordFirstProviderHeaders(options.usageContext);
      await recordPaidProviderResponseHealth(provider, candidate.response.status, candidate.request_id);
      if (
        providerIndex < paidProviders.length - 1 &&
        isAuthoritativeCapacityStatus(candidate.response.status)
      ) {
        // Keep the reservation uncommitted until the provider that will be
        // delivered to the client is known. The paid-fallback ledger has one
        // terminal provider/request-id pair; recording this intermediate
        // attempt would pin reconciliation to the failed provider and leave a
        // later successful provider unbillable.
        previousRespondingProvider = provider;
        previousProviderRequestId = normalizeProviderRequestId(candidate.request_id);
        cancelResponseBody(candidate.response);
        continue;
      }
      result = candidate;
      break;
    } catch (error) {
      if (error instanceof ApiKeyQuotaDispatchError) {
        // Paid fallback writes a durable dispatch intent before provider
        // transport. A quota CAS rejection proves this provider was not
        // started, but an earlier provider in the same reservation may have
        // been contacted already.
        retainRespondingProviderTelemetry(previousRespondingProvider, previousProviderRequestId);
        await bestEffortPaidFallbackBookkeeping(
          previousRespondingProvider === null
            ? "pre-dispatch quota cancellation recording"
            : "prior-provider quota rejection ambiguity recording",
          () =>
            previousRespondingProvider === null
              ? recordMeteredUndispatchedCancellation(decision.reservation)
              : recordMeteredAmbiguousFailure(
                decision.reservation,
                previousRespondingProvider,
                previousProviderRequestId,
              ),
        );
        throw error;
      }
      if (fallbackSignal?.aborted) {
        // The explicit dispatch callback distinguishes cancellation before
        // this transport from an abort that may have reached the provider.
        // Keep any contacted provider for reconciliation and never retry.
        const ambiguousProvider = transportStarted ? provider : previousRespondingProvider;
        const ambiguousProviderRequestId = transportStarted ? null : previousProviderRequestId;
        retainRespondingProviderTelemetry(ambiguousProvider, ambiguousProviderRequestId);
        await bestEffortPaidFallbackBookkeeping(
          ambiguousProvider === null ? "pre-transport cancellation recording" : "aborted transport ambiguity recording",
          () =>
            ambiguousProvider === null
              ? recordMeteredUndispatchedCancellation(decision.reservation)
              : recordMeteredAmbiguousFailure(
                decision.reservation,
                ambiguousProvider,
                ambiguousProviderRequestId,
              ),
        );
        throw fallbackSignal.reason instanceof Error
          ? fallbackSignal.reason
          : new DOMException("The request was aborted.", "AbortError");
      }
      providerError = error;
      const status = paidProviderErrorStatus(error);
      await recordPaidProviderResponseHealth(provider, status);
      if (transportStarted) {
        await bestEffortPaidFallbackBookkeeping(
          "transport failure ambiguity recording",
          () => recordMeteredAmbiguousFailure(decision.reservation, provider),
        );
        throw error;
      }
      if (providerIndex < paidProviders.length - 1 && isAuthoritativeCapacityStatus(status)) continue;
      break;
    }
  }
  if (!result) {
    const error = providerError;
    const selectedProviderLabel = selectedProvider === "surplus" ? "Surplus" : "Metered";
    if (error instanceof ApiKeyQuotaDispatchError) {
      throw error;
    }
    await bestEffortPaidFallbackBookkeeping(
      previousRespondingProvider === null ? "undispatched failure recording" : "ambiguous failure recording",
      () =>
        previousRespondingProvider === null
          ? recordMeteredUndispatchedCancellation(decision.reservation)
          : recordMeteredAmbiguousFailure(
            decision.reservation,
            previousRespondingProvider,
            previousProviderRequestId,
          ),
    );
    const abortReason = fallbackSignal?.reason;
    if (
      (fallbackSignal?.aborted && abortReason instanceof Error && abortReason.name === "TimeoutError") ||
      (error instanceof Error && error.name === "TimeoutError")
    ) {
      return {
        response: openaiError(
          504,
          `${selectedProviderLabel} upstream exceeded the gateway deadline before response headers were received.`,
          "gateway_timeout",
          {
            type: "server_error",
            headers: { "x-uos-upstream": selectedProvider },
          },
        ),
        provider: selectedProvider,
        paidFallback: decision.reservation,
        gatewayResponse: true,
        fallbackReason: reservationInput.reason,
      };
    }
    if (error instanceof MeteredError || error instanceof SurplusError) {
      return {
        response: openaiError(error.status, error.message, error.code, {
          type: "server_error",
          headers: { "x-uos-upstream": selectedProvider },
        }),
        provider: selectedProvider,
        paidFallback: decision.reservation,
        gatewayResponse: true,
        fallbackReason: reservationInput.reason,
      };
    }
    return {
      response: openaiError(
        502,
        `${selectedProviderLabel} upstream request failed before response headers were received.`,
        "upstream_error",
        {
          type: "server_error",
          headers: { "x-uos-upstream": selectedProvider },
        },
      ),
      provider: selectedProvider,
      paidFallback: decision.reservation,
      gatewayResponse: true,
      fallbackReason: reservationInput.reason,
    };
  }
  const providerRequestId = normalizeProviderRequestId(result.request_id);
  if (telemetry) telemetry.providerRequestId = providerRequestId;
  await bestEffortPaidFallbackBookkeeping(
    "upstream response recording",
    () => recordMeteredUpstreamResponse(decision.reservation, result.response, providerRequestId, selectedProvider),
  );
  return {
    response: result.response,
    provider: selectedProvider,
    paidFallback: decision.reservation,
    paidFallbackBilling: selectedProvider === "surplus" ? surplusBilling : null,
    paidFallbackProviderRequestId: providerRequestId,
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
  supportedEndpoints: readonly string[] | null;
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

const getCodexModelMetadata = async (
  model: string,
  route: "chat.completions" | "responses",
): Promise<CodexModelMetadata> => {
  if (isTemporaryFreeSurplusModel(model)) {
    // This non-Codex routing record deliberately omits reasoning capability
    // claims. GLM preserves each caller-selected effort; no Surplus catalog
    // metadata currently authorizes the gateway to advertise a fixed list.
    const record = { slug: TEMPORARY_FREE_SURPLUS_MODEL };
    return {
      snapshot: null,
      record,
      reasoning: getCodexModelReasoning(record),
      supportedEndpoints: ["openai", "openai-response"],
    };
  }
  const snapshot = await loadCodexModelsSnapshot();
  const record = findSnapshotModelRecord(snapshot, model);
  if (record) return { snapshot, record, reasoning: getCodexModelReasoning(record), supportedEndpoints: null };
  const [metered, surplus] = await Promise.all([
    fetchMeteredModels(),
    fetchSurplusModels(),
  ]);
  const meteredRecord = metered?.models.find((candidate) => candidate.id === model);
  const surplusRecord = surplus?.models.find((candidate) => candidate.id === model);
  const endpointType = route === "responses" ? "openai-response" : "openai";
  const routeRecord = [meteredRecord, surplusRecord].find((candidate) =>
    candidate?.supported_endpoint_types.includes(endpointType)
  );
  const paidRecord = routeRecord ?? meteredRecord ?? surplusRecord;
  if (paidRecord) {
    const routeProvider = routeRecord === surplusRecord && surplusRecord ? "surplus" : "metered";
    const routeSnapshot = routeProvider === "surplus" ? surplus : metered;
    return {
      snapshot: snapshot ?? {
        models: [],
        source: routeProvider,
        updated_at_ms: routeSnapshot?.updated_at_ms ?? Date.now(),
      },
      record: {
        slug: paidRecord.id,
        supported_reasoning_levels: ["none"],
        default_reasoning_level: "none",
      },
      reasoning: getCodexModelReasoning({ supported_reasoning_levels: ["none"], default_reasoning_level: "none" }),
      supportedEndpoints: paidRecord.supported_endpoint_types,
    };
  }
  return { snapshot, record: null, reasoning: getCodexModelReasoning(null), supportedEndpoints: null };
};

const validateCodexModelAvailable = (
  model: string,
  route: "chat.completions" | "responses",
  metadata: CodexModelMetadata,
): Response | null => {
  if (
    metadata.supportedEndpoints &&
    !metadata.supportedEndpoints.includes(route === "responses" ? "openai-response" : "openai")
  ) {
    return openaiError(
      404,
      `The model '${model}' does not support ${route}. Use /v1/models for supported models.`,
      "model_not_found",
      { param: "model" },
    );
  }
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

const sleepUnlessAborted = (ms: number, signal: AbortSignal): Promise<boolean> => {
  if (signal.aborted) return Promise.resolve(false);
  return new Promise((resolve) => {
    const timeout = setTimeout(() => {
      signal.removeEventListener("abort", onAbort);
      resolve(true);
    }, ms);
    const onAbort = (): void => {
      clearTimeout(timeout);
      signal.removeEventListener("abort", onAbort);
      resolve(false);
    };
    signal.addEventListener("abort", onAbort, { once: true });
    if (signal.aborted) onAbort();
  });
};

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
  downstreamSignal?: AbortSignal;
  beforeProviderDispatch?: UsageContext["beforeProviderDispatch"];
}): Promise<{ vectors: number[][]; totalTokens: number | null }> => {
  const controller = new AbortController();
  const signal = params.downstreamSignal
    ? AbortSignal.any([controller.signal, params.downstreamSignal])
    : controller.signal;
  const now = Date.now();
  const timeoutMs = Math.max(1, Math.min(EMBEDDINGS_TIMEOUT_MS, params.deadlineMs - now));
  const timeout = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const dispatch = params.beforeProviderDispatch ? await params.beforeProviderDispatch("voyage") : undefined;
    if (signal.aborted) {
      await dispatch?.cancelBeforeTransport();
      throw signal.reason ?? new DOMException("The request was aborted.", "AbortError");
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
      signal,
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
    headers: error.headers,
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
      supported_reasoning_levels: ["low", "medium", "high"],
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
  const nativeContextWindow = normalizeTokenCount(value.context_window);
  const nativeMaxContextWindow = normalizeTokenCount(value.max_context_window);
  const nativeAutoCompactTokenLimit = normalizeTokenCount(value.auto_compact_token_limit);
  const resolvedContext = recentModelContextFor(id, {
    context_window_tokens: nativeContextWindow,
    max_context_window_tokens: nativeMaxContextWindow,
    auto_compact_token_limit_tokens: nativeAutoCompactTokenLimit,
    effective_context_window_percent: normalizeTokenCount(value.effective_context_window_percent),
  });
  const contextWindow = resolvedContext?.context_window_tokens ?? nativeContextWindow;
  const maxContextWindow = resolvedContext?.max_context_window_tokens ?? nativeMaxContextWindow ?? contextWindow;
  const autoCompactTokenLimit = resolvedContext?.auto_compact_token_limit_tokens ??
    (nativeAutoCompactTokenLimit !== null &&
        (contextWindow === null || nativeAutoCompactTokenLimit <= contextWindow)
      ? nativeAutoCompactTokenLimit
      : null);
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
    context_window_tokens: contextWindow,
    max_context_window_tokens: maxContextWindow,
    auto_compact_token_limit_tokens: autoCompactTokenLimit,
    ...(resolvedContext
      ? {
        model_class: resolvedContext.model_class,
        effective_context_window_percent: resolvedContext.effective_context_window_percent,
      }
      : {}),
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
    const refusal = value.refusal === undefined || value.refusal === null ? null : getString(value.refusal);
    if (refusal === null && value.refusal !== undefined && value.refusal !== null) {
      return invalidNormalizedField(`${param}.refusal`, `${param}.refusal must be a string or null`);
    }
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
    const messageContent = refusal === null
      ? content.value
      : [...content.value, { type: "output_text" as const, text: refusal }];
    if (messageContent.length) input.push({ type: "message", role, content: messageContent });
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
      return invalidNormalizedField(`${param}.content`, "assistant messages require content, refusal, or tool_calls");
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
  recordResponsesEventTelemetry(usageContext, event);
  recordStreamTerminalType(usageContext, event.type as ResponseStreamTerminalType);
  const usage = isRecord(event.value.response) ? extractUsageTokens(event.value.response.usage) : null;
  if (event.type === "response.completed") void recordCompletionUsage(usageContext, usage);
  else recordTerminalUsage(usageContext, usage, false);
};

const responseHasOutputText = (output: unknown, startIndex = 0): boolean => {
  if (!Array.isArray(output)) return false;
  for (const item of output.slice(startIndex)) {
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

const responseHasRefusal = (output: unknown, startIndex = 0): boolean => {
  if (!Array.isArray(output)) return false;
  return output.slice(startIndex).some((item) =>
    isRecord(item) && Array.isArray(item.content) &&
    item.content.some((part) => isRecord(part) && part.type === "refusal" && Boolean(getString(part.refusal)))
  );
};

const reconcileCompletedOutputText = (emittedText: string, completedText: string): string => {
  if (!completedText || completedText === emittedText || emittedText.startsWith(completedText)) return "";
  if (completedText.startsWith(emittedText)) return completedText.slice(emittedText.length);
  return malformedFunctionCallStream("Upstream response output text conflicts with prior text deltas.");
};

const reconcileCompletedRefusal = (emittedRefusal: string, completedRefusal: string): string => {
  if (!completedRefusal || completedRefusal === emittedRefusal || emittedRefusal.startsWith(completedRefusal)) {
    return "";
  }
  if (completedRefusal.startsWith(emittedRefusal)) return completedRefusal.slice(emittedRefusal.length);
  return malformedFunctionCallStream("Upstream response refusal conflicts with prior refusal deltas.");
};

const chatOutputTextPartKey = (event: Record<string, unknown>): string => {
  const itemId = getString(event.item_id)?.trim();
  if (itemId) return `item:${itemId}:${String(event.content_index ?? 0)}`;
  return `output:${String(event.output_index ?? 0)}:${String(event.content_index ?? 0)}`;
};

type ReconciledChatContent = Readonly<{ outputText: string; refusal: string }>;

const reconcileChatContentPart = (
  outputTextParts: Map<string, string>,
  refusalParts: Map<string, string>,
  event: Record<string, unknown>,
  part: unknown,
): ReconciledChatContent => {
  if (!isRecord(part) || Array.isArray(part)) {
    return malformedFunctionCallStream("Upstream completed content part is missing its part object.");
  }
  const type = getString(part.type);
  const key = chatOutputTextPartKey(event);
  if (type === "output_text" || type === "text") {
    const completedText = getString(part.text);
    if (completedText === null) {
      return malformedFunctionCallStream("Upstream completed content part is missing string output text.");
    }
    const emittedText = outputTextParts.get(key) ?? "";
    const suffix = reconcileCompletedOutputText(emittedText, completedText);
    outputTextParts.set(key, `${emittedText}${suffix}`);
    return { outputText: suffix, refusal: "" };
  }
  if (type === "refusal") {
    const completedRefusal = getString(part.refusal);
    if (completedRefusal === null) {
      return malformedFunctionCallStream("Upstream completed content part is missing string refusal text.");
    }
    const emittedRefusal = refusalParts.get(key) ?? "";
    const suffix = reconcileCompletedRefusal(emittedRefusal, completedRefusal);
    refusalParts.set(key, `${emittedRefusal}${suffix}`);
    return { outputText: "", refusal: suffix };
  }
  return { outputText: "", refusal: "" };
};

const reconcileChatOutputItemContent = (
  outputTextParts: Map<string, string>,
  refusalParts: Map<string, string>,
  event: Record<string, unknown>,
  item: unknown,
): ReconciledChatContent => {
  if (!isRecord(item) || Array.isArray(item) || !Array.isArray(item.content)) {
    return { outputText: "", refusal: "" };
  }
  let outputText = "";
  let refusal = "";
  for (const [contentIndex, part] of item.content.entries()) {
    const partEvent: Record<string, unknown> = {
      ...event,
      item_id: getString(item.id) ?? event.item_id,
      content_index: contentIndex,
    };
    const reconciled = reconcileChatContentPart(outputTextParts, refusalParts, partEvent, part);
    outputText += reconciled.outputText;
    refusal += reconciled.refusal;
  }
  return { outputText, refusal };
};

/**
 * Some compatible upstreams provide complete message content in response.output
 * before repeating it in the normal final-item events. Reconcile every part
 * through the same per-item maps so either ordering emits each value once.
 */
const reconcileChatResponseOutputContent = (
  outputTextParts: Map<string, string>,
  refusalParts: Map<string, string>,
  event: Record<string, unknown>,
  output: unknown,
): ReconciledChatContent => {
  if (!Array.isArray(output)) return { outputText: "", refusal: "" };
  let outputText = "";
  let refusal = "";
  for (const [outputIndex, item] of output.entries()) {
    const reconciled = reconcileChatOutputItemContent(
      outputTextParts,
      refusalParts,
      { ...event, output_index: outputIndex },
      item,
    );
    outputText += reconciled.outputText;
    refusal += reconciled.refusal;
  }
  return { outputText, refusal };
};

const withAccumulatedResponseText = (
  response: Record<string, unknown>,
  text: string,
  ignoredOutputPrefix = 0,
): Record<string, unknown> => {
  if (!text || responseHasOutputText(response.output, ignoredOutputPrefix)) return response;
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

const withAccumulatedResponseRefusal = (
  response: Record<string, unknown>,
  refusal: string,
  ignoredOutputPrefix = 0,
): Record<string, unknown> => {
  if (!refusal || responseHasRefusal(response.output, ignoredOutputPrefix)) return response;
  const output = Array.isArray(response.output) ? [...response.output] : [];
  output.push({
    id: `msg_${crypto.randomUUID().replace(/-/g, "")}`,
    type: "message",
    status: "completed",
    role: "assistant",
    content: [{ type: "refusal", refusal }],
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

const preparedChatCompletionIsEmpty = (prepared: PreparedResponsesStream): boolean => {
  let outputText = "";
  let refusal = "";
  const outputTextParts = new Map<string, string>();
  const refusalParts = new Map<string, string>();
  const functionCalls = new ChatFunctionCallAccumulator();
  let completed = false;

  for (const event of prepared.buffered) {
    const ev = event.value;
    switch (event.type) {
      case "response.output_text.delta": {
        const delta = getString(ev.delta);
        if (delta === null) return malformedFunctionCallStream("Upstream output-text delta is not a string.");
        const key = chatOutputTextPartKey(ev);
        outputTextParts.set(key, `${outputTextParts.get(key) ?? ""}${delta}`);
        outputText += delta;
        break;
      }
      case "response.output_text.done": {
        const completedText = getString(ev.text);
        if (completedText === null) {
          return malformedFunctionCallStream("Upstream completed output text is not a string.");
        }
        const key = chatOutputTextPartKey(ev);
        const partText = outputTextParts.get(key) ?? "";
        const suffix = reconcileCompletedOutputText(partText, completedText);
        outputTextParts.set(key, `${partText}${suffix}`);
        outputText += suffix;
        break;
      }
      case "response.refusal.delta": {
        const delta = getString(ev.delta);
        if (delta === null) return malformedFunctionCallStream("Upstream refusal delta is not a string.");
        const key = chatOutputTextPartKey(ev);
        refusalParts.set(key, `${refusalParts.get(key) ?? ""}${delta}`);
        refusal += delta;
        break;
      }
      case "response.refusal.done": {
        const completedRefusal = getString(ev.refusal);
        if (completedRefusal === null) {
          return malformedFunctionCallStream("Upstream completed refusal is not a string.");
        }
        const key = chatOutputTextPartKey(ev);
        const partRefusal = refusalParts.get(key) ?? "";
        const suffix = reconcileCompletedRefusal(partRefusal, completedRefusal);
        refusalParts.set(key, `${partRefusal}${suffix}`);
        refusal += suffix;
        break;
      }
      case "response.content_part.done": {
        const reconciled = reconcileChatContentPart(outputTextParts, refusalParts, ev, ev.part);
        outputText += reconciled.outputText;
        refusal += reconciled.refusal;
        break;
      }
      case "response.output_item.added":
        functionCalls.add(ev, ev.item);
        break;
      case "response.function_call_arguments.delta":
        functionCalls.delta(ev);
        break;
      case "response.function_call_arguments.done":
        functionCalls.done(ev);
        break;
      case "response.output_item.done": {
        const functionCall = functionCalls.reconcileItem(ev, ev.item);
        if (!functionCall) {
          const reconciled = reconcileChatOutputItemContent(outputTextParts, refusalParts, ev, ev.item);
          outputText += reconciled.outputText;
          refusal += reconciled.refusal;
        }
        break;
      }
      case "response.output": {
        const output = ev.output ?? (isRecord(ev.response) ? ev.response.output : undefined);
        const reconciled = reconcileChatResponseOutputContent(outputTextParts, refusalParts, ev, output);
        outputText += reconciled.outputText;
        refusal += reconciled.refusal;
        functionCalls.reconcileOutput(ev, output);
        break;
      }
      case "response.completed": {
        if (!isRecord(ev.response) || Array.isArray(ev.response)) {
          return malformedFunctionCallStream("Upstream response.completed event is missing its response object.");
        }
        const reconciled = reconcileChatResponseOutputContent(
          outputTextParts,
          refusalParts,
          ev,
          ev.response.output,
        );
        outputText += reconciled.outputText;
        refusal += reconciled.refusal;
        functionCalls.reconcileOutput(ev, ev.response.output);
        functionCalls.assertFinalized();
        completed = true;
        break;
      }
    }
  }

  if (!completed) {
    return malformedFunctionCallStream("Chat semantic preflight did not retain a completed terminal.");
  }
  return !outputText && !refusal && !functionCalls.hasCalls;
};

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

const chatSourceFromPrepared = (
  source: PreflightedResponsesStream,
  prepared: PreparedResponsesStream,
): PreflightedResponsesStream => {
  const first = prepared.buffered[0];
  if (!first) throw new ResponsesStreamError("Chat preflight did not retain its first event.", { kind: "read_error" });
  const iterator = (async function* (): ResponsesStreamIterator {
    try {
      for (const event of prepared.buffered.slice(1)) yield event;
      for await (const event of prepared.iterator) yield event;
      return undefined;
    } finally {
      await prepared.iterator.return("Chat prepared stream closed").catch(() => {});
    }
  })();
  return {
    first,
    iterator,
    cancel: async (reason?: unknown): Promise<void> => {
      await source.cancel(reason);
      await iterator.return(reason).catch(() => {});
    },
  };
};

const EMPTY_UPSTREAM_COMPLETION_MESSAGE = "Upstream response completed with no translated semantic output.";

const emptyUpstreamCompletionError = (): Record<string, unknown> => ({
  error: {
    message: EMPTY_UPSTREAM_COMPLETION_MESSAGE,
    type: "server_error",
    code: "empty_upstream_completion",
    param: null,
  },
});

const markChatSemanticOutput = (context: UsageContext | undefined): void => {
  if (context?.responseTelemetry) context.responseTelemetry.semanticOutputObserved = true;
};

const markFinalizedChatToolOutput = (
  context: UsageContext | undefined,
  functionCalls: ChatFunctionCallAccumulator,
): void => {
  if (functionCalls.calls.some((call) => call.argumentsDone)) markChatSemanticOutput(context);
};

const translatedChatOutputObserved = (
  outputText: string,
  refusal: string,
  functionCalls: ChatFunctionCallAccumulator,
): boolean => outputText.length > 0 || refusal.length > 0 || functionCalls.hasCalls;

const recordSuccessfulChatCompletion = async (
  context: UsageContext | undefined,
  lifecycle: MeteredTransportLifecycle,
  usage: UsageTokens | null,
  onResponseTerminal?: (terminalType: ResponseStreamTerminalType) => void,
): Promise<void> => {
  markChatSemanticOutput(context);
  onResponseTerminal?.("response.completed");
  lifecycle.terminal("response.completed", usage);
  recordStreamTerminalType(context, "response.completed");
  await recordCompletionUsage(context, usage);
};

const recordEmptyUpstreamCompletion = (
  context: UsageContext | undefined,
  lifecycle: MeteredTransportLifecycle,
  usage: UsageTokens | null,
  onResponseTerminal?: (terminalType: ResponseStreamTerminalType) => void,
): void => {
  if (context?.responseTelemetry) {
    context.responseTelemetry.failureKind = "empty_upstream_completion";
    context.responseTelemetry.semanticOutputObserved = false;
  }
  onResponseTerminal?.("error");
  lifecycle.terminal("response.failed", usage);
  recordStreamTerminalType(context, "error");
  recordTerminalUsage(context, usage, false);
};

const streamChatCompletions = (
  source: PreflightedResponsesStream,
  model: string,
  includeUsage: boolean,
  usageContext: UsageContext | undefined,
  provider: UpstreamProvider,
  lifecycle: MeteredTransportLifecycle,
  signal: AbortSignal,
  downstreamSignal: AbortSignal,
  onResponseTerminal?: (terminalType: ResponseStreamTerminalType) => void,
): Response => {
  const encoder = new TextEncoder();
  const iterator = source.iterator;
  let pending: ResponsesStreamEvent | undefined = source.first;
  let id = `chatcmpl_${crypto.randomUUID().replace(/-/g, "")}`;
  let created = Math.floor(Date.now() / 1000);
  let sentRole = false;
  let closed = false;
  let terminalSettled = false;
  let observedCompletedUsage: UsageTokens | null | undefined;
  let outputText = "";
  let refusal = "";
  const outputTextParts = new Map<string, string>();
  const refusalParts = new Map<string, string>();
  const functionCalls = new ChatFunctionCallAccumulator();
  const observedEvents = new WeakSet<object>();
  const queuedDeltas: Array<
    | Readonly<{ kind: "content"; content: string }>
    | Readonly<{ kind: "refusal"; refusal: string }>
    | Readonly<{
      kind: "tool";
      call: ChatFunctionCall;
      includeIdentity: boolean;
      argumentsDelta: string;
    }>
  > = [];
  const settleInitialTerminalOnCancel = async (): Promise<void> => {
    const event = source.first;
    if (terminalSettled || !event.terminal) return;
    recordResponsesEventTelemetry(usageContext, event);
    const ev = event.value;
    const usageTokens = isRecord(ev.response) ? extractUsageTokens(ev.response.usage) : null;
    if (event.type === "response.completed") {
      if (!isRecord(ev.response) || Array.isArray(ev.response)) {
        const error = new ResponsesStreamError(
          "Upstream response.completed event is missing its response object.",
          { kind: "malformed_event" },
        );
        recordResponsesFailureTelemetry(usageContext, error);
        onResponseTerminal?.("error");
        lifecycle.ambiguous();
        recordStreamTerminalType(usageContext, "error");
        terminalSettled = true;
        return;
      }
      try {
        const completed = reconcileChatResponseOutputContent(
          outputTextParts,
          refusalParts,
          ev,
          ev.response.output,
        );
        outputText += completed.outputText;
        refusal += completed.refusal;
        functionCalls.reconcileOutput(ev, ev.response.output);
        functionCalls.assertFinalized();
      } catch (error) {
        recordResponsesFailureTelemetry(usageContext, error);
        onResponseTerminal?.("error");
        lifecycle.terminal("response.failed", usageTokens);
        recordStreamTerminalType(usageContext, "error");
        recordTerminalUsage(usageContext, usageTokens, false);
        terminalSettled = true;
        return;
      }
      terminalSettled = true;
      if (translatedChatOutputObserved(outputText, refusal, functionCalls)) {
        await recordSuccessfulChatCompletion(usageContext, lifecycle, usageTokens, onResponseTerminal);
      } else {
        recordEmptyUpstreamCompletion(usageContext, lifecycle, usageTokens, onResponseTerminal);
      }
      return;
    }
    onResponseTerminal?.(event.type as ResponseStreamTerminalType);
    lifecycle.terminal(event.type, usageTokens);
    recordStreamTerminalType(usageContext, event.type as ResponseStreamTerminalType);
    recordTerminalUsage(usageContext, usageTokens, false);
    terminalSettled = true;
  };
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
          if (content.length > 0) markChatSemanticOutput(usageContext);
          controller.enqueue(encoder.encode(`data: ${JSON.stringify(chunk)}\n\n`));
        };
        const emitRefusal = (value: string): void => {
          const delta = sentRole ? { refusal: value } : { role: "assistant", refusal: value };
          const chunk: Record<string, unknown> = {
            id,
            object: "chat.completion.chunk",
            created,
            model,
            choices: [{ index: 0, delta, finish_reason: null }],
          };
          sentRole = true;
          if (value.length > 0) markChatSemanticOutput(usageContext);
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
          const completed = reconcileChatResponseOutputContent(outputTextParts, refusalParts, event, output);
          if (completed.outputText) {
            outputText += completed.outputText;
            queuedDeltas.push({ kind: "content", content: completed.outputText });
          }
          if (completed.refusal) {
            refusal += completed.refusal;
            queuedDeltas.push({ kind: "refusal", refusal: completed.refusal });
          }
          const beforeCount = functionCalls.calls.length;
          const reconciled = functionCalls.reconcileOutput(event, output);
          if (reconciled.length > 0) markFinalizedChatToolOutput(usageContext, functionCalls);
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
          else if (queued.kind === "refusal") emitRefusal(queued.refusal);
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
          if (!observedEvents.has(event)) {
            observedEvents.add(event);
            recordResponsesEventTelemetry(usageContext, event);
          }
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
            const key = chatOutputTextPartKey(ev);
            outputTextParts.set(key, `${outputTextParts.get(key) ?? ""}${delta}`);
            outputText += delta;
            emitContent(delta);
            return;
          }

          if (type === "response.output_text.done") {
            const completedText = getString(ev.text);
            if (completedText === null) {
              return malformedFunctionCallStream("Upstream completed output text is not a string.");
            }
            const key = chatOutputTextPartKey(ev);
            const partText = outputTextParts.get(key) ?? "";
            const suffix = reconcileCompletedOutputText(partText, completedText);
            outputTextParts.set(key, `${partText}${suffix}`);
            if (suffix) {
              outputText += suffix;
              emitContent(suffix);
              return;
            }
            continue;
          }

          if (type === "response.refusal.delta") {
            const delta = getString(ev.delta);
            if (delta === null) return malformedFunctionCallStream("Upstream refusal delta is not a string.");
            const key = chatOutputTextPartKey(ev);
            refusalParts.set(key, `${refusalParts.get(key) ?? ""}${delta}`);
            refusal += delta;
            emitRefusal(delta);
            return;
          }

          if (type === "response.refusal.done") {
            const completedRefusal = getString(ev.refusal);
            if (completedRefusal === null) {
              return malformedFunctionCallStream("Upstream completed refusal is not a string.");
            }
            const key = chatOutputTextPartKey(ev);
            const partRefusal = refusalParts.get(key) ?? "";
            const suffix = reconcileCompletedRefusal(partRefusal, completedRefusal);
            refusalParts.set(key, `${partRefusal}${suffix}`);
            if (suffix) {
              refusal += suffix;
              emitRefusal(suffix);
              return;
            }
            continue;
          }

          if (type === "response.content_part.done") {
            const reconciled = reconcileChatContentPart(outputTextParts, refusalParts, ev, ev.part);
            if (reconciled.outputText) {
              outputText += reconciled.outputText;
              emitContent(reconciled.outputText);
              return;
            }
            if (reconciled.refusal) {
              refusal += reconciled.refusal;
              emitRefusal(reconciled.refusal);
              return;
            }
            continue;
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
            markFinalizedChatToolOutput(usageContext, functionCalls);
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
              markFinalizedChatToolOutput(usageContext, functionCalls);
              if (!wasKnown || reconciled.suffix) {
                emitToolCall(reconciled.call, !wasKnown, reconciled.suffix);
                return;
              }
            } else {
              const completed = reconcileChatOutputItemContent(outputTextParts, refusalParts, ev, ev.item);
              outputText += completed.outputText;
              refusal += completed.refusal;
              if (completed.outputText) queuedDeltas.push({ kind: "content", content: completed.outputText });
              if (completed.refusal) queuedDeltas.push({ kind: "refusal", refusal: completed.refusal });
              const queued = queuedDeltas.shift();
              if (queued?.kind === "content") emitContent(queued.content);
              else if (queued?.kind === "refusal") emitRefusal(queued.refusal);
              else if (queued) emitToolCall(queued.call, queued.includeIdentity, queued.argumentsDelta);
              if (queued) return;
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
              else if (queued.kind === "refusal") emitRefusal(queued.refusal);
              else emitToolCall(queued.call, queued.includeIdentity, queued.argumentsDelta);
              return;
            }
            continue;
          }

          if (type === "response.completed") {
            if (!isRecord(ev.response) || Array.isArray(ev.response)) {
              return malformedFunctionCallStream("Upstream response.completed event is missing its response object.");
            }
            observedCompletedUsage = extractUsageTokens(ev.response.usage);
            const output = ev.response.output;
            queueFinalOutput(ev, output);
            functionCalls.assertFinalized();
            const usageTokens = observedCompletedUsage;
            if (!terminalSettled) {
              terminalSettled = true;
              if (!translatedChatOutputObserved(outputText, refusal, functionCalls)) {
                recordEmptyUpstreamCompletion(usageContext, lifecycle, usageTokens, onResponseTerminal);
                controller.enqueue(encoder.encode(`data: ${JSON.stringify(emptyUpstreamCompletionError())}\n\n`));
                closed = true;
                controller.close();
                void iterator.return("Empty Responses completion translated").catch(() => {});
                return;
              }
              await recordSuccessfulChatCompletion(usageContext, lifecycle, usageTokens, onResponseTerminal);
            }
            if (queuedDeltas.length) {
              pending = event;
              const queued = queuedDeltas.shift()!;
              if (queued.kind === "content") emitContent(queued.content);
              else if (queued.kind === "refusal") emitRefusal(queued.refusal);
              else emitToolCall(queued.call, queued.includeIdentity, queued.argumentsDelta);
              return;
            }
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
            const usageTokens = isRecord(ev.response) ? extractUsageTokens(ev.response.usage) : null;
            onResponseTerminal?.(type as ResponseStreamTerminalType);
            lifecycle.terminal(type, usageTokens);
            recordStreamTerminalType(usageContext, type as ResponseStreamTerminalType);
            recordTerminalUsage(usageContext, usageTokens, false);
            terminalSettled = true;
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
        await iterator.return(error).catch(() => {});
        if (!terminalSettled) {
          recordResponsesFailureTelemetry(usageContext, error);
          if (observedCompletedUsage !== undefined) {
            onResponseTerminal?.("error");
            lifecycle.terminal("response.failed", observedCompletedUsage);
            recordStreamTerminalType(usageContext, "error");
            recordTerminalUsage(usageContext, observedCompletedUsage, false);
            terminalSettled = true;
          } else {
            const terminalType = classifyStreamFailure(error, signal, downstreamSignal);
            onResponseTerminal?.(terminalType);
            recordStreamTerminalType(usageContext, terminalType);
            if (terminalType === "cancelled") lifecycle.cancelled();
            else lifecycle.ambiguous();
            void recordErrorUsage(usageContext);
          }
        }
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
      await settleInitialTerminalOnCancel();
      if (!terminalSettled) {
        onResponseTerminal?.("cancelled");
        recordStreamTerminalType(usageContext, "cancelled");
        lifecycle.cancelled();
        void recordErrorUsage(usageContext);
      }
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
  lifecycle: MeteredTransportLifecycle,
  signal: AbortSignal,
  downstreamSignal: AbortSignal,
  warnings: readonly string[] = [],
  onResponseTerminal?: (terminalType: ResponseStreamTerminalType) => void,
): Promise<Response> => {
  let id = `chatcmpl_${crypto.randomUUID().replace(/-/g, "")}`;
  let created = Math.floor(Date.now() / 1000);
  let content = "";
  let refusal = "";
  const outputTextParts = new Map<string, string>();
  const refusalParts = new Map<string, string>();
  let usage: Record<string, unknown> | null = null;
  const functionCalls = new ChatFunctionCallAccumulator();

  let completed = false;
  let terminalType: ResponseStreamTerminalType | null = null;
  let observedCompletedUsage: UsageTokens | null | undefined;
  try {
    let pending: ResponsesStreamEvent | undefined = source.first;
    for (;;) {
      const next = pending ? { done: false as const, value: pending } : await source.iterator.next();
      pending = undefined;
      if (next.done) break;
      const event = next.value;
      recordResponsesEventTelemetry(usageContext, event);
      const ev = event.value;
      const type = event.type;
      if (event.terminal && type !== "response.completed") {
        terminalType = type as ResponseStreamTerminalType;
        const terminalUsage = isRecord(ev.response) ? extractUsageTokens(ev.response.usage) : null;
        onResponseTerminal?.(type as ResponseStreamTerminalType);
        lifecycle.terminal(type, terminalUsage);
        recordStreamTerminalType(usageContext, type as ResponseStreamTerminalType);
        recordTerminalUsage(usageContext, terminalUsage, false);
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
        const key = chatOutputTextPartKey(ev);
        outputTextParts.set(key, `${outputTextParts.get(key) ?? ""}${delta}`);
        content += delta;
        if (delta.length > 0) markChatSemanticOutput(usageContext);
        continue;
      }
      if (type === "response.output_text.done") {
        const completedText = getString(ev.text);
        if (completedText === null) {
          return malformedFunctionCallStream("Upstream completed output text is not a string.");
        }
        const key = chatOutputTextPartKey(ev);
        const partText = outputTextParts.get(key) ?? "";
        const suffix = reconcileCompletedOutputText(partText, completedText);
        outputTextParts.set(key, `${partText}${suffix}`);
        content += suffix;
        if (suffix.length > 0) markChatSemanticOutput(usageContext);
        continue;
      }
      if (type === "response.refusal.delta") {
        const delta = getString(ev.delta);
        if (delta === null) return malformedFunctionCallStream("Upstream refusal delta is not a string.");
        const key = chatOutputTextPartKey(ev);
        refusalParts.set(key, `${refusalParts.get(key) ?? ""}${delta}`);
        refusal += delta;
        if (delta.length > 0) markChatSemanticOutput(usageContext);
        continue;
      }
      if (type === "response.refusal.done") {
        const completedRefusal = getString(ev.refusal);
        if (completedRefusal === null) {
          return malformedFunctionCallStream("Upstream completed refusal is not a string.");
        }
        const key = chatOutputTextPartKey(ev);
        const partRefusal = refusalParts.get(key) ?? "";
        const suffix = reconcileCompletedRefusal(partRefusal, completedRefusal);
        refusalParts.set(key, `${partRefusal}${suffix}`);
        refusal += suffix;
        if (suffix.length > 0) markChatSemanticOutput(usageContext);
        continue;
      }
      if (type === "response.content_part.done") {
        const reconciled = reconcileChatContentPart(outputTextParts, refusalParts, ev, ev.part);
        content += reconciled.outputText;
        refusal += reconciled.refusal;
        if (reconciled.outputText.length > 0 || reconciled.refusal.length > 0) {
          markChatSemanticOutput(usageContext);
        }
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
        markFinalizedChatToolOutput(usageContext, functionCalls);
        continue;
      }
      if (type === "response.output_item.done") {
        const functionCall = functionCalls.reconcileItem(ev, ev.item);
        if (functionCall) {
          markFinalizedChatToolOutput(usageContext, functionCalls);
        } else {
          const completed = reconcileChatOutputItemContent(outputTextParts, refusalParts, ev, ev.item);
          content += completed.outputText;
          refusal += completed.refusal;
          if (completed.outputText.length > 0 || completed.refusal.length > 0) {
            markChatSemanticOutput(usageContext);
          }
        }
        continue;
      }
      if (type === "response.output") {
        const output = ev.output ?? (isRecord(ev.response) ? ev.response.output : undefined);
        const completed = reconcileChatResponseOutputContent(outputTextParts, refusalParts, ev, output);
        content += completed.outputText;
        refusal += completed.refusal;
        const reconciled = functionCalls.reconcileOutput(ev, output);
        if (completed.outputText.length > 0 || completed.refusal.length > 0) markChatSemanticOutput(usageContext);
        if (reconciled.length > 0) markFinalizedChatToolOutput(usageContext, functionCalls);
        continue;
      }
      if (type === "response.completed" && isRecord(ev.response) && !Array.isArray(ev.response)) {
        observedCompletedUsage = extractUsageTokens(ev.response.usage);
        const completedOutput = reconcileChatResponseOutputContent(
          outputTextParts,
          refusalParts,
          ev,
          ev.response.output,
        );
        content += completedOutput.outputText;
        refusal += completedOutput.refusal;
        functionCalls.reconcileOutput(ev, ev.response.output);
        functionCalls.assertFinalized();
        const usageTokens = observedCompletedUsage;
        usage = toChatUsage(usageTokens);
        if (!translatedChatOutputObserved(content, refusal, functionCalls)) {
          recordEmptyUpstreamCompletion(usageContext, lifecycle, usageTokens, onResponseTerminal);
          return streamErrorResponse(
            502,
            EMPTY_UPSTREAM_COMPLETION_MESSAGE,
            "empty_upstream_completion",
            provider,
            warnings,
            "server_error",
            null,
          );
        }
        completed = true;
        await recordSuccessfulChatCompletion(usageContext, lifecycle, usageTokens, onResponseTerminal);
        break;
      }
      if (event.terminal) break;
    }
  } catch (error) {
    recordResponsesFailureTelemetry(usageContext, error);
    if (observedCompletedUsage !== undefined) {
      terminalType = "error";
      onResponseTerminal?.("error");
      lifecycle.terminal("response.failed", observedCompletedUsage);
      recordStreamTerminalType(usageContext, "error");
      recordTerminalUsage(usageContext, observedCompletedUsage, false);
    } else {
      terminalType = classifyStreamFailure(error, signal, downstreamSignal);
      onResponseTerminal?.(terminalType);
      recordStreamTerminalType(usageContext, terminalType);
      if (terminalType === "cancelled") lifecycle.cancelled();
      else lifecycle.ambiguous();
    }
    completed = false;
  } finally {
    // This path consumes the generator manually (rather than through
    // `for await`), so explicitly close it after a terminal event or error.
    // Otherwise the parser can remain suspended at its final `yield` while
    // retaining the upstream reader lock.
    await source.iterator.return("Chat Completions response consumed").catch(() => {});
  }
  if (!completed) {
    await recordErrorUsage(usageContext);
    if (terminalType === "cancelled") {
      return streamErrorResponse(
        499,
        "Request was cancelled.",
        "request_cancelled",
        provider,
        warnings,
        "server_error",
        null,
      );
    }
    if (terminalType === "deadline") {
      return streamErrorResponse(
        504,
        "Upstream request exceeded the gateway deadline.",
        "gateway_timeout",
        provider,
        warnings,
        "server_error",
        null,
      );
    }
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
    content: content || (!functionCalls.hasCalls && !refusal) ? content : null,
  };
  if (refusal) message.refusal = refusal;
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
  const [metered, surplus] = await Promise.all([
    fetchMeteredModels(),
    fetchSurplusModels(),
  ]);
  const merged = [...data];
  for (const model of [...(metered?.models ?? []), ...(surplus?.models ?? [])]) {
    if (!model.supported_endpoint_types.some((type) => type === "openai" || type === "openai-response")) continue;
    if (merged.some((candidate) => candidate.id === model.id)) continue;
    merged.push({
      id: model.id,
      object: "model",
      created: model.created,
      owned_by: model.owned_by,
    });
  }

  return json(
    200,
    { object: "list", data: merged },
    { "x-uos-upstream": snapshot?.source || "stored_codex_models" },
  );
};

type PublicModelProvider = Readonly<{
  id: "codex" | "openlux" | "surplus";
  owned_by: string;
  supported_endpoints: readonly string[];
}>;

type PublicModelCatalogEntry = {
  id: string;
  providers: PublicModelProvider[];
  model_class?: string;
  context_window_tokens?: number;
  max_context_window_tokens?: number;
  auto_compact_token_limit_tokens?: number;
  effective_context_window_percent?: number;
};

export const handlePublicModelCatalog = async (): Promise<Response> => {
  const snapshot = await loadCodexModelsSnapshot();
  const normalized = snapshot && Array.isArray(snapshot.models) && snapshot.models.length > 0
    ? normalizeModelList(snapshot)
    : null;
  const [metered, surplus] = await Promise.all([
    fetchMeteredModels(),
    fetchSurplusModels({ requireApiKey: false }),
  ]);
  const codexModels = normalized?.data ?? [];
  const surplusModels = surplus?.models ?? [];
  const otherProviderModelIds = new Set<string>();
  for (const model of codexModels) {
    const id = getString(model.id);
    if (id) otherProviderModelIds.add(id);
  }
  for (const model of surplusModels) otherProviderModelIds.add(model.id);
  const models = new Map<string, PublicModelCatalogEntry>();
  const includedOpenLuxModelIds = new Set<string>();
  const add = (id: string, provider: PublicModelProvider): void => {
    const existing = models.get(id);
    if (existing) {
      existing.providers.push(provider);
      return;
    }
    const context = recentModelContextFor(id);
    models.set(id, {
      id,
      providers: [provider],
      ...(context
        ? {
          model_class: context.model_class,
          context_window_tokens: context.context_window_tokens,
          max_context_window_tokens: context.max_context_window_tokens,
          auto_compact_token_limit_tokens: context.auto_compact_token_limit_tokens,
          effective_context_window_percent: context.effective_context_window_percent,
        }
        : {}),
    });
  };

  for (const model of codexModels) {
    const id = getString(model.id);
    if (!id) continue;
    add(id, {
      id: "codex",
      owned_by: getString(model.owned_by) ?? "openai",
      supported_endpoints: ["/v1/responses", "/v1/chat/completions"],
    });
  }
  for (const model of metered?.models ?? []) {
    // OpenLux is a broad discovery source; only advertise it when another
    // configured provider confirms the same model ID.
    if (!otherProviderModelIds.has(model.id)) continue;
    includedOpenLuxModelIds.add(model.id);
    add(model.id, {
      id: "openlux",
      owned_by: model.owned_by,
      supported_endpoints: [
        ...(model.supported_endpoint_types.includes("openai-response") ? ["/v1/responses"] : []),
        ...(model.supported_endpoint_types.includes("openai") ? ["/v1/chat/completions"] : []),
      ],
    });
  }
  for (const model of surplusModels) {
    add(model.id, {
      id: "surplus",
      owned_by: model.owned_by,
      supported_endpoints: [
        ...(model.supported_endpoint_types.includes("openai-response") ? ["/v1/responses"] : []),
        ...(model.supported_endpoint_types.includes("openai") ? ["/v1/chat/completions"] : []),
      ],
    });
  }

  return json(200, {
    object: "uos.model_catalog",
    data: [...models.values()].sort((left, right) => left.id.localeCompare(right.id)),
    sources: {
      codex: {
        status: normalized ? "available" : "unavailable",
        count: normalized?.data.length ?? 0,
        updated_at_ms: snapshot?.updated_at_ms ?? null,
      },
      openlux: {
        status: metered ? "available" : "unavailable",
        count: includedOpenLuxModelIds.size,
        updated_at_ms: metered?.updated_at_ms ?? null,
      },
      surplus: {
        status: surplus ? "available" : "unavailable",
        count: surplus?.models.length ?? 0,
        updated_at_ms: surplus?.updated_at_ms ?? null,
      },
    },
  });
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
  const [metered, surplus] = await Promise.all([
    fetchMeteredModels(),
    fetchSurplusModels(),
  ]);
  for (
    const [provider, models] of [
      ["metered", metered?.models ?? []],
      ["surplus", surplus?.models ?? []],
    ] as const
  ) {
    for (const model of models) {
      if (data.some((candidate) => candidate.id === model.id)) continue;
      const supportedEndpoints = [
        ...(model.supported_endpoint_types.includes("openai-response") ? ["/v1/responses"] : []),
        ...(model.supported_endpoint_types.includes("openai") ? ["/v1/chat/completions"] : []),
      ];
      const context = recentModelContextFor(model.id);
      data.push({
        id: model.id,
        object: "uos.model_capabilities",
        owned_by: model.owned_by,
        display_name: model.id,
        upstream_provider: provider,
        supported_endpoints: supportedEndpoints,
        supported_reasoning_levels: ["none"],
        default_reasoning_effort: "none",
        reasoning_effort_wire_map: {},
        context_window_tokens: context?.context_window_tokens ?? null,
        max_context_window_tokens: context?.max_context_window_tokens ?? null,
        auto_compact_token_limit_tokens: context?.auto_compact_token_limit_tokens ?? null,
        ...(context
          ? {
            model_class: context.model_class,
            effective_context_window_percent: context.effective_context_window_percent,
          }
          : {}),
      });
    }
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
  const downstreamSignal = downstreamSignalFor(req, usageContext);

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
  const cancelledResponse = async (): Promise<Response> => {
    recordStreamTerminalType(usageContext, "cancelled");
    await recordErrorUsage(usageContext);
    if (idempotencyLease && idempotencyDispatched) return await failIndeterminate();
    return await releaseBeforeDispatch(
      openaiError(499, "Request was cancelled.", "request_cancelled", { type: "server_error", param: null }),
    );
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
        if (downstreamSignal.aborted) return await cancelledResponse();
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
            downstreamSignal,
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
          if (downstreamSignal.aborted) return await cancelledResponse();
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

          if (!(await sleepUnlessAborted(waitMs, downstreamSignal))) return await cancelledResponse();
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
  let queueFailureKind: string | null = null;

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
    if (params.usageContext?.responseTelemetry) {
      params.usageContext.responseTelemetry.stream = false;
      params.usageContext.responseTelemetry.completed = false;
      params.usageContext.responseTelemetry.streamTerminalType = "error";
      params.usageContext.responseTelemetry.failureKind = code;
    }
    return json(200, buildEmbeddingsJobBody(failed, null), { "x-uos-upstream": failed.upstream });
  };

  const queueJob = async (waitMs: number, failureKind: string | null = null): Promise<Response> => {
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
    if (failureKind && params.usageContext?.responseTelemetry) {
      params.usageContext.responseTelemetry.stream = false;
      params.usageContext.responseTelemetry.completed = false;
      params.usageContext.responseTelemetry.streamTerminalType = "deadline";
      params.usageContext.responseTelemetry.failureKind = failureKind;
    }
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
      queueFailureKind = "embeddings_job_deadline";
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
        queueFailureKind = `embeddings_job_upstream_http_${status}`;
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
  return await queueJob(waitMs, queueFailureKind);
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

const recordCerebrasFailureKind = (
  context: UsageContext | undefined,
  failureKind: CerebrasFailureKind,
): void => {
  if (context?.responseTelemetry) context.responseTelemetry.failureKind = failureKind;
};

const cerebrasTransportFailureKind = (
  error: unknown,
  terminalType: ResponseStreamTerminalType,
): CerebrasFailureKind => {
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
  if (reasoningEffort.value === "none") {
    return openaiError(
      400,
      "reasoning_effort 'none' is not supported for gpt-oss-120b. Use low, medium, or high.",
      "invalid_request_error",
      { param: "reasoning_effort" },
    );
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

  const downstreamSignal = downstreamSignalFor(req, usageContext);
  const requestSignal = inferenceSignal(req, usageContext);
  let upstream: Response;
  try {
    upstream = await fetchCerebrasChatCompletions(cerebrasBody, {
      signal: requestSignal,
      beforeDispatch: () => usageContext?.beforeProviderDispatch?.("cerebras") ?? Promise.resolve(),
      onDispatch: () => {
        recordAttemptedProvider(usageContext, "cerebras");
        recordFirstProviderDispatch(usageContext);
      },
      onHeaders: () => recordFirstProviderHeaders(usageContext),
    });
  } catch (error) {
    const terminalType = cerebrasTerminalTypeForError(error, downstreamSignal);
    recordCerebrasFailureKind(usageContext, cerebrasTransportFailureKind(error, terminalType));
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
    recordCerebrasResponseHealth(upstream.status, providerRequestId);
    recordCerebrasFailureKind(usageContext, "upstream_http_error");
    recordStreamTerminalType(usageContext, "response.failed");
    await recordErrorUsage(usageContext);
    return await toCerebrasUpstreamErrorResponse(upstream, requestSignal);
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
    const terminalType = downstreamSignal.aborted ? "cancelled" : requestSignal.aborted ? "deadline" : "error";
    recordCerebrasFailureKind(
      usageContext,
      terminalType === "cancelled" ? "cancellation" : terminalType === "deadline" ? "deadline" : "incomplete_response",
    );
    recordStreamTerminalType(usageContext, terminalType);
    if (terminalType !== "cancelled") {
      void recordCerebrasProviderHealth("upstream_error", null, Date.now, providerRequestId);
    }
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
    recordCerebrasFailureKind(usageContext, "invalid_json");
    void recordCerebrasProviderHealth("upstream_error", upstream.status, Date.now, providerRequestId);
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
    recordCerebrasFailureKind(usageContext, "invalid_completion_schema");
    void recordCerebrasProviderHealth("upstream_error", upstream.status, Date.now, providerRequestId);
    await recordErrorUsage(usageContext);
    return openaiError(
      502,
      "Upstream returned an invalid Chat Completions response.",
      "cerebras_upstream_invalid_response",
      { type: "server_error", headers: cerebrasResponseHeaders(providerRequestId) },
    );
  }

  if (cerebrasChatCompletionHasSemanticOutput(normalized.value)) markChatSemanticOutput(usageContext);
  providerRequestId ??= normalizeCerebrasProviderRequestId(normalized.value.id);
  if (usageContext?.responseTelemetry) usageContext.responseTelemetry.providerRequestId = providerRequestId;
  const usage = extractChatUsageTokens(normalized.value.usage);
  await recordCompletionUsage(usageContext, usage);
  if (clientWantsStream) recordFirstSemanticCommitment(usageContext);
  recordStreamTerminalType(usageContext, "response.completed");
  recordCerebrasResponseHealth(upstream.status, providerRequestId);
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
  const jsonObjectTextFormat = isRecord(rawRecord.response_format) &&
      Object.keys(rawRecord.response_format).length === 1 && rawRecord.response_format.type === "json_object"
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
  const warnings = buildIgnoredWarnings(
    rawRecord,
    handledKeys,
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
  if (model.toLowerCase() === CEREBRAS_GPT_OSS_120B_MODEL) {
    return await handleCerebrasChatCompletions(req, rawRecord, modelRaw, usageContext);
  }
  const modelMetadata = await getCodexModelMetadata(model, "chat.completions");
  const modelAvailabilityError = validateCodexModelAvailable(modelRaw, "chat.completions", modelMetadata);
  if (modelAvailabilityError) return modelAvailabilityError;
  const modelCapabilityError = temporaryFreeSurplusCapabilityError(model, rawRecord);
  if (modelCapabilityError) return modelCapabilityError;
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
  if (jsonObjectTextFormat) codexBody.text = { format: jsonObjectTextFormat };
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
  if (usageContext?.responseTelemetry) {
    usageContext.responseTelemetry.reasoning = reasoningLabel;
    usageContext.responseTelemetry.outputTokenAllowance = maxCompletionTokens.value ?? null;
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
  // One timer covers both provider dispatch/headers and the first SSE event.
  // It is cleared immediately after preflight so active streams get their own
  // renewable inactivity deadline rather than an absolute buffered cutoff.
  const downstreamSignal = downstreamSignalFor(req, usageContext);
  const streamFirstEventDeadline = stream ? createStreamFirstEventDeadline(downstreamSignal) : null;
  const requestInferenceSignal = streamFirstEventDeadline?.signal ?? inferenceSignal(req, usageContext);
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
    const terminalType = classifyPreHeaderFailure(error, requestInferenceSignal, downstreamSignal);
    recordStreamTerminalType(usageContext, terminalType);
    if (terminalType !== "cancelled") {
      logRedactedUpstreamError("[ai.ubq.fi] Upstream fetch failed:", error);
    }
    await recordErrorUsage(usageContext);
    return toPreHeaderErrorResponse(error, terminalType, usageContext?.responseTelemetry?.provider);
  }
  const upstream = routed.response;
  const providerWarnings = responseWarnings(upstream);
  const lifecycle = createMeteredTransportLifecycle(
    routed.paidFallback,
    routed.provider,
    routed.paidFallbackProviderRequestId ?? null,
    routed.paidFallbackBilling ?? null,
    model,
    routed.providerHealthOnly === true,
  );
  let codexTerminalResolved = false;
  const resolveCodexProbe = (terminalType: ResponseStreamTerminalType): void => {
    if (routed.provider !== "chatgpt_codex" || codexTerminalResolved) return;
    codexTerminalResolved = true;
    const transition = terminalType === "response.completed"
      ? markCodexResponseCompleted(upstream)
      : terminalType === "response.failed" || terminalType === "error" || terminalType === "eof" ||
          terminalType === "deadline"
      ? markCodexResponseUpstreamError(upstream)
      : releaseCodexResponseProbe(upstream);
    void transition.catch(() => {});
  };

  if (routed.gatewayResponse) {
    clearStreamFirstEventDeadline();
    recordStreamTerminalType(usageContext, upstream.status === 504 ? "deadline" : "error");
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
    resolveCodexProbe("error");
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
    const firstEvent = await preflightResponsesStream(
      upstream.body,
      requestInferenceSignal,
      {},
    );
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
      releaseOnProgress: stream && supportsReasoningProgressRelease(routed.provider),
    });
    clearStreamFirstEventDeadline();
    const completedTerminalUsage = prepared.terminal?.type === "response.completed" &&
        isRecord(prepared.terminal.value.response)
      ? extractUsageTokens(prepared.terminal.value.response.usage)
      : null;
    let emptyCompletion = false;
    if (prepared.terminal?.type === "response.completed" && prepared.semantic === null) {
      try {
        emptyCompletion = preparedChatCompletionIsEmpty(prepared);
      } catch (error) {
        recordTerminalUsage(usageContext, completedTerminalUsage, false);
        throw error;
      }
    }
    if (emptyCompletion) {
      for (const event of prepared.buffered) recordResponsesEventTelemetry(usageContext, event);
      recordEmptyUpstreamCompletion(usageContext, lifecycle, completedTerminalUsage, resolveCodexProbe);
      await prepared.iterator.return("Empty Chat completion rejected").catch(() => {});
      return streamErrorResponse(
        502,
        EMPTY_UPSTREAM_COMPLETION_MESSAGE,
        "empty_upstream_completion",
        routed.provider,
        [...warnings, ...providerWarnings],
        "server_error",
        null,
      );
    }
    preflight = chatSourceFromPrepared(firstEvent, prepared);
  } catch (error) {
    clearStreamFirstEventDeadline();
    const terminalType = classifyStreamFailure(error, requestInferenceSignal, downstreamSignal);
    resolveCodexProbe(terminalType);
    recordStreamTerminalType(usageContext, terminalType);
    if (terminalType !== "cancelled") recordResponsesFailureTelemetry(usageContext, error);
    if (terminalType === "cancelled") lifecycle.cancelled();
    else lifecycle.ambiguous();
    await recordErrorUsage(usageContext);
    return streamPreflightFailureResponse(terminalType, routed.provider, [...warnings, ...providerWarnings]);
  }
  recordFirstSemanticCommitment(usageContext);
  const response = stream
    ? streamChatCompletions(
      preflight,
      model,
      streamOptions.includeUsage,
      usageContext,
      routed.provider,
      lifecycle,
      requestInferenceSignal,
      downstreamSignal,
      resolveCodexProbe,
    )
    : await completeChatCompletions(
      preflight,
      model,
      usageContext,
      routed.provider,
      lifecycle,
      requestInferenceSignal,
      downstreamSignal,
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

const handleResponsesInternal = async (
  req: Request,
  usageContext?: UsageContext,
  parsedBody?: unknown,
): Promise<Response> => {
  const rawBody = (parsedBody === undefined ? await readJsonBody(req) : parsedBody) as ResponsesRequest | null;
  if (!rawBody || !isRecord(rawBody)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const rawRecord = rawBody as Record<string, unknown>;
  const unknownKey = findUnknownKey(rawRecord, RESPONSES_ALLOWED_KEYS, CODEX_RESPONSES_EXTENSION_KEYS);
  if (unknownKey) {
    return openaiError(400, `Unrecognized request argument supplied: ${unknownKey}`, "invalid_request_error");
  }
  const maxOutputTokens = rawRecord.max_output_tokens;
  if (
    maxOutputTokens !== undefined && maxOutputTokens !== null &&
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
  if (
    maxToolCalls !== undefined && maxToolCalls !== null &&
    (typeof maxToolCalls !== "number" || !Number.isSafeInteger(maxToolCalls) || maxToolCalls <= 0)
  ) {
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
      "max_output_tokens",
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
  if (model.toLowerCase() === CEREBRAS_GPT_OSS_120B_MODEL) {
    return openaiError(
      400,
      "gpt-oss-120b is available only on /v1/chat/completions.",
      "unsupported_model",
      { param: "model" },
    );
  }
  const modelMetadata = await getCodexModelMetadata(model, "responses");
  const modelAvailabilityError = validateCodexModelAvailable(modelRaw, "responses", modelMetadata);
  if (modelAvailabilityError) return modelAvailabilityError;
  const modelCapabilityError = temporaryFreeSurplusCapabilityError(model, rawRecord);
  if (modelCapabilityError) return modelCapabilityError;

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
  for (
    const key of [
      "max_output_tokens",
      "max_tool_calls",
      "metadata",
      "safety_identifier",
      "service_tier",
      "temperature",
      "top_p",
      "truncation",
      "user",
    ]
  ) {
    if (Object.prototype.hasOwnProperty.call(rawRecord, key)) removedProviderBody[key] = rawRecord[key];
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
  const downstreamSignal = downstreamSignalFor(req, usageContext);
  const requestInferenceSignal = clientWantsStream ? downstreamSignal : inferenceSignal(req, usageContext);
  const preHeaderDeadline = createStreamFirstEventDeadline(requestInferenceSignal);
  const apiKey = isTemporaryFreeSurplusModel(model) ? null : readRemovedProviderApiKey();
  const paidFallbackAvailable = canAttemptPaidFallback(usageContext);
  const debugRoutingScenario = (await loadDebugRoutingConfig()).scenario;
  const circuit = apiKey ? await selectRemovedProviderCircuitRoute() : null;
  if (circuit && circuit.transition !== "none") {
    recordRemovedProviderFields(usageContext, { circuitTransition: circuit.transition });
  }
  const sessionId = apiKey
    ? await deriveRemovedProviderSessionId(usageContext?.idempotencyPrincipal, rawRecord.client_metadata)
    : null;
  let route: "codex" | "removed_provider" = debugRoutingScenario === "removed_provider_first" && apiKey
    ? "removed_provider"
    : circuit?.route ?? "codex";
  let globalProbe: RemovedProviderCircuitProbe | null = circuit?.probe ?? null;
  let primaryFailureResponse: Response | null = null;
  let primaryFailureCorrelation:
    | Readonly<{
      provider: string | null;
      accountSlot: number | null;
      accountCohortId: string | null;
      providerRequestId: string | null;
    }>
    | null = null;
  let primaryResult: ResponsesRouteAttempt | null = null;
  let removedProviderAttempt: PreparedResponsesAttempt | null = null;
  let selectedModel: string | null = null;
  let fallbackStartedAt = 0;

  try {
    if (route === "codex") {
      try {
        const remainingMs = preHeaderDeadline.remainingMs();
        const failoverReserveMs = Math.min(STREAM_FAILOVER_RESERVE_MS, remainingMs / 2);
        const primaryBudgetMs = apiKey || paidFallbackAvailable
          ? Math.max(0, remainingMs - failoverReserveMs)
          : remainingMs;
        const result = await fetchAndPreparePrimaryResponses(codexBody, {
          model,
          reasoning: reasoningLabel,
          clientWantsStream,
          usageContext,
          clientVersion: modelMetadata.snapshot?.client_version,
          requestSignal: requestInferenceSignal,
          downstreamSignal,
          warnings,
          attemptDeadline: createStreamSemanticDeadline(preHeaderDeadline.signal, Math.ceil(primaryBudgetMs)),
          fallbackSignal: paidFallbackAvailable ? preHeaderDeadline.signal : undefined,
          createFallbackDeadline: paidFallbackAvailable
            ? () =>
              createStreamSemanticDeadline(
                preHeaderDeadline.signal,
                Math.ceil(preHeaderDeadline.remainingMs()),
              )
            : undefined,
          rejectPresemanticFailureTerminal: apiKey !== null,
          releaseOnProgress: clientWantsStream,
        });
        if (result.kind === "ready") {
          primaryResult = result.value;
          if (
            result.value.prepared.prepared.terminal?.type === "response.completed" ||
            result.value.prepared.prepared.terminal?.type === "response.incomplete"
          ) {
            markPrimarySemanticRecovery(
              result.value.routed,
              globalProbe,
              usageContext,
              result.value.prepared.prepared.terminal?.type,
            );
          }
        } else {
          const { routed, failed, lifecycle } = result.value;
          primaryFailureResponse = failed.response;
          const telemetry = usageContext?.responseTelemetry;
          if (telemetry) {
            primaryFailureCorrelation = {
              provider: telemetry.provider,
              accountSlot: telemetry.accountSlot,
              accountCohortId: telemetry.accountCohortId,
              providerRequestId: telemetry.providerRequestId,
            };
          }
          const terminalType = responseFailureTerminalType(failed.trigger, failed.signal, downstreamSignal);
          recordStreamTerminalType(usageContext, terminalType);
          const failureKind = failureKindForResponsesAttemptTrigger(failed.trigger);
          if (failureKind && usageContext?.responseTelemetry) {
            usageContext.responseTelemetry.failureKind = failureKind;
            if (failureKind === "empty_upstream_completion") {
              usageContext.responseTelemetry.semanticOutputObserved = false;
            }
          }
          if (failed.trigger === "empty_upstream_completion") {
            const terminalUsage = failed.terminal && isRecord(failed.terminal.value.response)
              ? extractUsageTokens(failed.terminal.value.response.usage)
              : null;
            recordTerminalUsage(usageContext, terminalUsage, false);
            await finalizeAbandonedPrimaryAttempt(routed, lifecycle, { failureTrigger: failed.trigger });
            if (globalProbe) void releaseGlobalRemovedProviderProbe(globalProbe).catch(() => {});
            return failed.response;
          }
          if (routed.allowRemovedProviderRecovery === false) {
            if (globalProbe) void releaseGlobalRemovedProviderProbe(globalProbe).catch(() => {});
            return failed.response;
          }
          if (routed.gatewayResponse && !isEligibleResponsesAttemptStatus(failed.response)) {
            if (globalProbe) void releaseGlobalRemovedProviderProbe(globalProbe).catch(() => {});
            return failed.response;
          }
          if (!isEligibleResponsesAttemptStatus(failed.response) && failed.trigger === "read_error") {
            if (globalProbe) void releaseGlobalRemovedProviderProbe(globalProbe).catch(() => {});
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
            if (globalProbe) void releaseGlobalRemovedProviderProbe(globalProbe).catch(() => {});
            await recordErrorUsage(usageContext);
            return toPreHeaderErrorResponse(
              downstreamSignal.reason,
              terminalType,
              routed.provider,
            );
          }
          if (!apiKey) return failed.response;
          const transition = await recordRemovedProviderEligibleFailure(globalProbe);
          if (transition !== "none") recordRemovedProviderFields(usageContext, { circuitTransition: transition });
          recordRemovedProviderFields(usageContext, { triggerClass: failed.trigger });
          route = "removed_provider";
        }
      } catch (error) {
        if (globalProbe) void releaseGlobalRemovedProviderProbe(globalProbe).catch(() => {});
        const terminalType = classifyPreHeaderFailure(error, preHeaderDeadline.signal, downstreamSignal);
        recordStreamTerminalType(usageContext, terminalType);
        if (terminalType !== "cancelled") {
          logRedactedUpstreamError("[ai.ubq.fi] Upstream fetch failed:", error);
        }
        await recordErrorUsage(usageContext);
        return toPreHeaderErrorResponse(error, terminalType, usageContext?.responseTelemetry?.provider);
      }
    }

    if (route === "removed_provider" && apiKey) {
      fallbackStartedAt = performance.now();
      const removedProvider = await fetchAndPrepareRemovedProviderResponses(removedProviderBody, {
        usageContext,
        requestSignal: requestInferenceSignal,
        sessionId,
        apiKey,
        attemptDeadline: createStreamSemanticDeadline(
          preHeaderDeadline.signal,
          Math.ceil(preHeaderDeadline.remainingMs()),
        ),
      });
      if (removedProvider.kind === "ready") {
        removedProviderAttempt = removedProvider.attempt;
        selectedModel = removedProvider.attempt.selectedModel;
        recordRemovedProviderFields(usageContext, {
          selectedModel,
          taskType: removedProvider.attempt.taskType,
          semanticCommitment: removedProvider.attempt.prepared.semanticKind ??
            (removedProvider.attempt.prepared.terminal?.type === "response.completed" ? "terminal_completed" : null),
        });
      } else {
        void persistFailedRemovedProviderAttempt(usageContext, fallbackStartedAt, removedProvider.attempt.trigger);
        if (removedProvider.attempt.trigger === "empty_upstream_completion") {
          if (usageContext?.responseTelemetry) {
            usageContext.responseTelemetry.failureKind = "empty_upstream_completion";
            usageContext.responseTelemetry.semanticOutputObserved = false;
          }
          const terminalUsage = removedProvider.attempt.terminal &&
              isRecord(removedProvider.attempt.terminal.value.response)
            ? extractUsageTokens(removedProvider.attempt.terminal.value.response.usage)
            : null;
          recordTerminalUsage(usageContext, terminalUsage, false);
          recordStreamTerminalType(usageContext, "response.failed");
          return removedProvider.attempt.response;
        }
        if (primaryFailureResponse || removedProvider.attempt.trigger === "terminal_failure") {
          if (primaryFailureResponse && usageContext?.responseTelemetry) {
            const telemetry = usageContext.responseTelemetry;
            telemetry.provider = primaryFailureCorrelation?.provider ??
              primaryFailureResponse.headers.get("x-uos-upstream") ?? "chatgpt_codex";
            telemetry.accountSlot = primaryFailureCorrelation?.accountSlot ?? null;
            telemetry.accountCohortId = primaryFailureCorrelation?.accountCohortId ?? null;
            telemetry.providerRequestId = primaryFailureCorrelation?.providerRequestId ?? null;
          } else {
            selectRemovedProviderTelemetry(usageContext);
          }
          return primaryFailureResponse ?? removedProvider.attempt.response;
        }
        let recoveryProbe = await claimRemovedProviderEarlyRecoveryProbe();
        if (!recoveryProbe) {
          const recoveryRoute = await selectRemovedProviderCircuitRoute();
          if (recoveryRoute.route !== "codex") return removedProvider.attempt.response;
          recoveryProbe = recoveryRoute.probe;
        }
        globalProbe = recoveryProbe;
        let recovery: Awaited<ReturnType<typeof fetchAndPreparePrimaryResponses>>;
        try {
          recovery = await fetchAndPreparePrimaryResponses(codexBody, {
            model,
            reasoning: reasoningLabel,
            clientWantsStream,
            usageContext,
            clientVersion: modelMetadata.snapshot?.client_version,
            requestSignal: requestInferenceSignal,
            downstreamSignal,
            warnings,
            attemptDeadline: createStreamSemanticDeadline(
              preHeaderDeadline.signal,
              Math.ceil(preHeaderDeadline.remainingMs()),
            ),
            rejectPresemanticFailureTerminal: true,
            releaseOnProgress: clientWantsStream,
          });
        } catch (error) {
          const transition = recoveryProbe ? await releaseGlobalRemovedProviderProbe(recoveryProbe) : "none";
          if (transition !== "none") recordRemovedProviderFields(usageContext, { circuitTransition: transition });
          if (requestInferenceSignal.aborted) throw requestInferenceSignal.reason ?? error;
          selectRemovedProviderTelemetry(usageContext);
          return removedProvider.attempt.response;
        }
        if (recovery.kind === "failed") {
          const terminalType = responseFailureTerminalType(
            recovery.value.failed.trigger,
            recovery.value.failed.signal,
            downstreamSignal,
          );
          await finalizeAbandonedPrimaryAttempt(recovery.value.routed, recovery.value.lifecycle, {
            cancelled: terminalType === "cancelled",
            failureTrigger: recovery.value.failed.trigger,
          });
          if (recovery.value.failed.trigger === "empty_upstream_completion") {
            if (usageContext?.responseTelemetry) {
              usageContext.responseTelemetry.failureKind = "empty_upstream_completion";
              usageContext.responseTelemetry.semanticOutputObserved = false;
            }
            const terminalUsage = recovery.value.failed.terminal &&
                isRecord(recovery.value.failed.terminal.value.response)
              ? extractUsageTokens(recovery.value.failed.terminal.value.response.usage)
              : null;
            recordTerminalUsage(usageContext, terminalUsage, false);
            recordStreamTerminalType(usageContext, "response.failed");
            const transition = recoveryProbe ? await releaseGlobalRemovedProviderProbe(recoveryProbe) : "none";
            if (transition !== "none") {
              recordRemovedProviderFields(usageContext, { circuitTransition: transition });
            }
            return recovery.value.failed.response;
          }
          if (terminalType === "cancelled") {
            const transition = recoveryProbe ? await releaseGlobalRemovedProviderProbe(recoveryProbe) : "none";
            if (transition !== "none") recordRemovedProviderFields(usageContext, { circuitTransition: transition });
            recordStreamTerminalType(usageContext, terminalType);
            await recordErrorUsage(usageContext);
            return toPreHeaderErrorResponse(
              downstreamSignal.reason,
              terminalType,
              recovery.value.routed.provider,
            );
          }
          if (recovery.value.failed.trigger === "semantic_timeout") {
            const transition = recoveryProbe ? await releaseGlobalRemovedProviderProbe(recoveryProbe) : "none";
            if (transition !== "none") recordRemovedProviderFields(usageContext, { circuitTransition: transition });
            return recovery.value.failed.response;
          }
          const transition = isEligibleResponsesAttemptStatus(recovery.value.failed.response)
            ? await recordRemovedProviderEligibleFailure(recoveryProbe)
            : recoveryProbe
            ? await releaseGlobalRemovedProviderProbe(recoveryProbe)
            : "none";
          if (transition !== "none") recordRemovedProviderFields(usageContext, { circuitTransition: transition });
          selectRemovedProviderTelemetry(usageContext);
          return removedProvider.attempt.response;
        }
        primaryResult = recovery.value;
        if (
          recovery.value.prepared.prepared.terminal?.type === "response.completed" ||
          recovery.value.prepared.prepared.terminal?.type === "response.incomplete"
        ) {
          markPrimarySemanticRecovery(
            recovery.value.routed,
            recoveryProbe,
            usageContext,
            recovery.value.prepared.prepared.terminal?.type,
          );
        }
      }
    }
  } finally {
    preHeaderDeadline.clear();
  }

  const ready = removedProviderAttempt ?? primaryResult?.prepared;
  if (!ready) {
    return primaryFailureResponse ?? streamErrorResponse(
      502,
      "No upstream provider produced a response.",
      "upstream_error",
      "chatgpt_codex",
      warnings,
    );
  }
  // A failed pre-commit attempt is diagnostic evidence for routing, not the
  // terminal result of a later provider. Reset only final-failure telemetry
  // when failover produced a ready response; the selected attempt will record
  // its own terminal or stream failure below.
  if (usageContext?.responseTelemetry) {
    usageContext.responseTelemetry.failureKind = null;
    usageContext.responseTelemetry.syntheticTerminalType = null;
    usageContext.responseTelemetry.streamTerminalType = null;
  }
  const lifecycle = primaryResult?.lifecycle ?? createMeteredTransportLifecycle(null);
  const routed = primaryResult?.routed ?? null;
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
  const clientWarnings = [
    ...warnings,
    ...(primaryFailureResponse ? responseWarnings(primaryFailureResponse) : []),
    ...responseWarnings(ready.response),
  ].filter((warning) => {
    if (!removedProviderAttempt) return true;
    if (warning === "prompt_cache_breakpoint_ignored" && countExplicitPromptCacheBreakpoints(input) > 0) {
      return false;
    }
    return ![...forwardedRemovedProviderControls].some((key) =>
      Object.prototype.hasOwnProperty.call(rawRecord, key) &&
      warning === (WARNING_KEY_MAP.get(key) ?? `${key}_ignored`)
    );
  });
  const structuredTextOutput = isRecord(rawRecord.text) && isRecord(rawRecord.text.format) &&
    (rawRecord.text.format.type === "json_schema" || rawRecord.text.format.type === "json_object");
  const warningModel = removedProviderAttempt && !structuredTextOutput ? selectedModel : null;
  if (globalProbe && ready.prepared.semantic) {
    void renewRemovedProviderCircuitProbe(globalProbe).catch(() => {});
  }
  const probeRenewal = globalProbe && ready.prepared.semantic
    ? setInterval(() => void renewRemovedProviderCircuitProbe(globalProbe).catch(() => {}), 60_000)
    : null;
  const clearProbeRenewal = (): void => {
    if (probeRenewal !== null) clearInterval(probeRenewal);
  };
  let providerTerminalValidated = false;
  const reconcileCommittedFailure = (terminalType: ResponseStreamTerminalType): void => {
    clearProbeRenewal();
    // A terminal buffered during preflight already describes the provider.
    // A later client-body cancellation is delivery-only and cannot change that
    // provider outcome, health result, or paid settlement.
    if (providerTerminalValidated) return;
    if (usageContext?.responseTelemetry?.streamTerminalType === null) {
      recordStreamTerminalType(usageContext, terminalType);
    }
    if (routed) {
      void finalizeAbandonedPrimaryAttempt(routed, lifecycle, { cancelled: terminalType === "cancelled" });
    }
    if (globalProbe && (routed?.provider === "metered" || routed?.provider === "surplus")) {
      void releaseGlobalRemovedProviderProbe(globalProbe).then((value) => {
        if (value !== "none") recordRemovedProviderFields(usageContext, { circuitTransition: value });
      }).catch(() => {});
    } else if (routed?.provider === "chatgpt_codex") {
      const transition = terminalType === "cancelled"
        ? globalProbe ? releaseGlobalRemovedProviderProbe(globalProbe) : Promise.resolve("none" as const)
        : recordRemovedProviderEligibleFailure(globalProbe);
      void transition.then((value) => {
        if (value !== "none") recordRemovedProviderFields(usageContext, { circuitTransition: value });
      }).catch(() => {});
    }
    if (
      removedProviderAttempt && usageContext?.responseTelemetry?.removedProviderTerminalStatus !== "response.failed"
    ) {
      recordRemovedProviderFields(usageContext, {
        latencyMs: Math.max(0, Math.round(performance.now() - fallbackStartedAt)),
        terminalStatus: terminalType,
      });
      persistRemovedProviderFields(usageContext);
    }
    void recordErrorUsage(usageContext);
  };
  const validateRemovedProviderEvent = (event: ResponsesStreamEvent): void => {
    if (!removedProviderAttempt || !selectedModel) return;
    const candidate = removedProviderModelFromEvent(event.value);
    if (!candidate) return;
    if (candidate !== selectedModel || !isEligibleRemovedProviderModel(candidate)) {
      throw new ResponsesStreamError("RemovedProvider changed the selected model after stream release.", {
        kind: "malformed_event",
      });
    }
  };
  const onTerminal = (event: ResponsesStreamEvent): void => {
    if (!event.terminal) return;
    clearProbeRenewal();
    const syntheticFailure = isSyntheticResponsesFailureEvent(event);
    if (!syntheticFailure && providerTerminalValidated) {
      // Buffered collection replays a terminal already settled during
      // preflight, after it has recorded the first SSE event. Complete timing
      // telemetry without repeating provider settlement or usage recording.
      recordStreamTerminalType(usageContext, event.type as ResponseStreamTerminalType);
      return;
    }
    if (!syntheticFailure) providerTerminalValidated = true;
    if (routed && !syntheticFailure) {
      const terminalUsage = isRecord(event.value.response) ? extractUsageTokens(event.value.response.usage) : null;
      lifecycle.terminal(event.type, terminalUsage);
      if (routed.provider === "chatgpt_codex") {
        const transition = event.type === "response.completed"
          ? markCodexResponseCompleted(routed.response)
          : event.type === "response.failed" || event.type === "error"
          ? markCodexResponseUpstreamError(routed.response)
          : releaseCodexResponseProbe(routed.response);
        void transition.catch(() => {});
      }
      if (globalProbe) {
        const transition = routed.provider === "chatgpt_codex"
          ? event.type === "response.completed" || event.type === "response.incomplete"
            ? closeRemovedProviderCircuit(globalProbe)
            : event.type === "response.failed" || event.type === "error"
            ? recordRemovedProviderEligibleFailure(globalProbe)
            : releaseGlobalRemovedProviderProbe(globalProbe)
          : releaseGlobalRemovedProviderProbe(globalProbe);
        void transition.then((value) => {
          if (value !== "none") recordRemovedProviderFields(usageContext, { circuitTransition: value });
        }).catch(() => {});
      } else if (
        routed.provider === "chatgpt_codex" && (event.type === "response.failed" || event.type === "error")
      ) {
        void recordRemovedProviderEligibleFailure(null).then((value) => {
          if (value !== "none") recordRemovedProviderFields(usageContext, { circuitTransition: value });
        }).catch(() => {});
      }
    }
    // A synthetic failure is the client-visible terminal owner after a
    // committed stream breaks. For an abandoned Codex/Metered attempt, keep the
    // underlying EOF/read classification in telemetry so paid-fallback
    // reconciliation retains its diagnostic cause. RemovedProvider owns its
    // synthetic terminal because no later provider can take over after the
    // failover notice has been released.
    if (!syntheticFailure || removedProviderAttempt) {
      recordResponsesTerminal(event, usageContext);
    }
    if (removedProviderAttempt) {
      recordRemovedProviderFields(usageContext, {
        latencyMs: Math.max(0, Math.round(performance.now() - fallbackStartedAt)),
        terminalStatus: event.type,
      });
      persistRemovedProviderFields(usageContext);
    }
  };

  // Preflight has established either semantic ownership or a valid terminal,
  // so this is the content-free release boundary. Parser callbacks record the
  // earlier first upstream SSE event independently.
  recordFirstSemanticCommitment(usageContext);

  // Preflight can already contain a terminal. Record its provider outcome
  // before returning a body that a client may cancel without consuming.
  if (ready.prepared.terminal) onTerminal(ready.prepared.terminal);

  if (!clientWantsStream) {
    const response = await collectBufferedResponses(ready, {
      warningModel,
      usageContext,
      onTerminal,
      validateEvent: validateRemovedProviderEvent,
      onFailure: (error, details) => {
        const terminalType = classifyStreamFailure(error, ready.signal, downstreamSignal);
        if (terminalType !== "cancelled") recordResponsesFailureTelemetry(usageContext, error, details);
        else if (usageContext?.responseTelemetry) {
          usageContext.responseTelemetry.responseCreatedObserved = details?.responseCreatedObserved ??
            usageContext.responseTelemetry.responseCreatedObserved;
        }
        reconcileCommittedFailure(terminalType);
        if (terminalType === "cancelled" || terminalType === "deadline") {
          return toPreHeaderErrorResponse(error, terminalType, ready.provider);
        }
      },
    });
    return withUosWarning(response, clientWarnings);
  }

  const body = createOwnedResponsesStream({
    initial: ready.prepared.buffered,
    iterator: ready.prepared.iterator,
    responseId: ready.responseId,
    ...(warningModel ? { warning: { model: warningModel } } : {}),
    signal: ready.signal,
    downstreamSignal,
    abortUpstream: ready.abort,
    onEvent: (event) => {
      recordResponsesEventTelemetry(usageContext, event);
      onTerminal(event);
    },
    validateEvent: validateRemovedProviderEvent,
    onFailure: (error, details) => {
      const terminalType = classifyStreamFailure(error, ready.signal, downstreamSignal);
      if (terminalType !== "cancelled") recordResponsesFailureTelemetry(usageContext, error, details);
      else if (usageContext?.responseTelemetry) {
        usageContext.responseTelemetry.responseCreatedObserved = details?.responseCreatedObserved ??
          usageContext.responseTelemetry.responseCreatedObserved;
      }
      if (details?.failureKind === "empty_upstream_completion") {
        const terminalUsage = details.upstreamTerminal && isRecord(details.upstreamTerminal.value.response)
          ? extractUsageTokens(details.upstreamTerminal.value.response.usage)
          : null;
        recordTerminalUsage(usageContext, terminalUsage, false);
      }
      reconcileCommittedFailure(terminalType);
    },
    onCancel: () => {
      reconcileCommittedFailure("cancelled");
    },
  });
  const headers = new Headers(ready.response.headers);
  headers.delete("Content-Encoding");
  headers.delete("Content-Length");
  headers.set("Content-Type", "text/event-stream");
  headers.set("x-uos-upstream", ready.provider);
  return withUosWarning(new Response(withSseKeepalive(body), { status: 200, headers }), clientWarnings);
};

const runResponsesHandler = async (
  req: Request,
  usageContext?: UsageContext,
  parsedBody?: unknown,
): Promise<Response> =>
  await runWithResponseTelemetry(
    usageContext,
    async (context) => {
      try {
        return await handleResponsesInternal(req, context, parsedBody);
      } catch (error) {
        const downstreamSignal = downstreamSignalFor(req, context);
        const terminalType = isTimeoutFailure(error, downstreamSignal.reason)
          ? "deadline"
          : downstreamSignal.aborted
          ? "cancelled"
          : null;
        if (terminalType === null) throw error;
        recordStreamTerminalType(context, terminalType);
        await recordErrorUsage(context);
        return toPreHeaderErrorResponse(
          error,
          terminalType,
          context?.responseTelemetry?.provider ?? "chatgpt_codex",
        );
      }
    },
  );

export const handleResponses = async (req: Request, usageContext?: UsageContext): Promise<Response> =>
  await runResponsesHandler(req, usageContext);

/**
 * OpenAI-compatible image endpoints.
 *
 * ChatGPT/Codex has no native image endpoint. It exposes image generation as
 * an `image_generation` tool on the Responses API, so an images request is
 * rewritten into a tool-bearing Responses call. A live Codex subscription is
 * the only enabled transport until paid providers have image-specific model
 * authorization, pricing, and settlement.
 */

const IMAGE_BASE_MODEL_ENV = "IMAGE_BASE_MODEL";
const IMAGE_TOOL_TYPE = "image_generation";
const IMAGE_EDIT_DEFAULT_MODEL = "gpt-image-1.5";
const IMAGE_MAX_COUNT = 10;
const IMAGE_MAX_EDIT_INPUTS = 16;
const IMAGE_MAX_PROMPT_CHARS = 32_000;
const IMAGE_MAX_REFERENCE_URL_CHARS = 20_971_520;
const IMAGE_MAX_FILE_BYTES = 50 * 1_024 * 1_024;
const IMAGE_MAX_MASK_FILE_BYTES = IMAGE_MAX_FILE_BYTES;
const IMAGE_MAX_MULTIPART_INPUT_BYTES = 50 * 1_024 * 1_024;
const IMAGE_MAX_MULTIPART_BODY_BYTES = 64 * 1_024 * 1_024;
const IMAGE_MAX_FANOUT_INPUT_BYTES = 50 * 1_024 * 1_024;
const IMAGE_MAX_JSON_BODY_BYTES = Math.ceil(IMAGE_MAX_FANOUT_INPUT_BYTES / 3) * 4 + 1_024 * 1_024;
const IMAGE_MEDIA_TYPE_BY_EXTENSION = new Map([
  [".jpeg", "image/jpeg"],
  [".jpg", "image/jpeg"],
  [".png", "image/png"],
  [".webp", "image/webp"],
]);
const IMAGE_MEDIA_TYPE_ALIASES = new Map([
  ["image/jpeg", "image/jpeg"],
  ["image/jpg", "image/jpeg"],
  ["image/png", "image/png"],
  ["image/webp", "image/webp"],
  ["image/x-png", "image/png"],
]);
const IMAGE_NULLABLE_OPTION_KEYS = [
  "model",
  "n",
  "size",
  "quality",
  "background",
  "output_format",
  "output_compression",
  "input_fidelity",
  "moderation",
  "response_format",
  "stream",
  "partial_images",
  "style",
] as const;
const IMAGE_GENERATION_QUALITIES = new Set(["low", "medium", "high", "auto"]);
const IMAGE_EDIT_JSON_QUALITIES = new Set(["low", "medium", "high", "auto"]);
const IMAGE_EDIT_MULTIPART_QUALITIES = new Set(["low", "medium", "high", "auto"]);
const IMAGE_BACKGROUNDS = new Set(["transparent", "opaque", "auto"]);
const IMAGE_OUTPUT_FORMATS = new Set(["png", "jpeg", "webp"]);
const IMAGE_MODERATION_LEVELS = new Set(["low", "auto"]);
const IMAGE_INPUT_FIDELITIES = new Set(["low", "high"]);
const IMAGE_TEXT_ENCODER = new TextEncoder();

// Internal test seam for image translation tests. It avoids requiring the
// test runner to grant environment access for a deployment-only override.
let imageBaseModelForTest: string | null = null;

export const setImageBaseModelForTest = (model: string | null): void => {
  imageBaseModelForTest = model;
};

const IMAGE_SHARED_REQUEST_KEYS = [
  "model",
  "prompt",
  "n",
  "size",
  "quality",
  "background",
  "output_format",
  "output_compression",
  "stream",
  "partial_images",
  "user",
] as const;
const IMAGE_GENERATION_REQUEST_KEYS = new Set<string>([
  ...IMAGE_SHARED_REQUEST_KEYS,
  "moderation",
  "response_format",
  "style",
]);
const IMAGE_EDIT_JSON_REQUEST_KEYS = new Set<string>([
  ...IMAGE_SHARED_REQUEST_KEYS,
  "images",
  "mask",
  "input_fidelity",
  "moderation",
]);
const IMAGE_EDIT_MULTIPART_REQUEST_KEYS = new Set<string>([
  ...IMAGE_SHARED_REQUEST_KEYS,
  "image",
  "image[]",
  "mask",
  "input_fidelity",
  "response_format",
]);

export type ImageRouteKind = "generations" | "edits";

type ImageRequestFailure = Readonly<{ ok: false; response: Response }>;
type ParsedImageRequest =
  | Readonly<{ ok: true; body: Record<string, unknown>; count: number; inputBytes: number }>
  | ImageRequestFailure;

type ImageHandlerOptions = Readonly<{
  dispatch?: (request: Request) => Promise<Response>;
}>;

/** Resolve the text model that hosts the image tool. */
export const resolveImageBaseModel = async (): Promise<string | null> => {
  if (imageBaseModelForTest !== null) return imageBaseModelForTest;
  let configured: string | null = null;
  try {
    const raw = Deno.env.get(IMAGE_BASE_MODEL_ENV)?.trim();
    configured = raw && raw.length > 0 ? raw : null;
  } catch {
    configured = null;
  }
  return configured ?? await getDefaultModel();
};

/**
 * Rewrite an OpenAI Images request as a Responses request carrying the
 * image_generation tool. Its optional model field selects the requested image
 * model, while the outer Responses model remains the text model that hosts the
 * tool.
 */
export const buildImageResponsesRequest = (
  body: Record<string, unknown>,
  baseModel: string,
  kind: ImageRouteKind,
): Record<string, unknown> => {
  const prompt = typeof body.prompt === "string" ? body.prompt : "";
  const user = typeof body.user === "string" ? { user: body.user } : {};
  const tool: Record<string, unknown> = {
    type: IMAGE_TOOL_TYPE,
    action: kind === "edits" ? "edit" : "generate",
  };
  const requestedModel = typeof body.model === "string" ? body.model.trim() : "";
  if (requestedModel) tool.model = requestedModel;
  else if (kind === "edits") tool.model = IMAGE_EDIT_DEFAULT_MODEL;
  for (
    const key of [
      "size",
      "quality",
      "background",
      "output_format",
      "output_compression",
      "moderation",
    ]
  ) {
    if (body[key] !== undefined) tool[key] = body[key];
  }
  if (kind === "edits") {
    // Edits supply source images; the Responses input carries them alongside
    // the instruction so the tool can operate on the provided pixels.
    const images = Array.isArray(body.images) ? body.images : [];
    if (body.input_fidelity !== undefined) tool.input_fidelity = body.input_fidelity;
    if (isPlainRecord(body.mask)) tool.input_image_mask = body.mask;
    return {
      model: baseModel,
      ...user,
      input: [{
        role: "user",
        content: [
          { type: "input_text", text: prompt },
          ...images.flatMap((entry): Array<Record<string, unknown>> => {
            if (!isPlainRecord(entry)) return [];
            if (typeof entry.image_url === "string") {
              return [{ type: "input_image", image_url: entry.image_url }];
            }
            return [];
          }),
        ],
      }],
      tools: [tool],
      tool_choice: { type: IMAGE_TOOL_TYPE },
    };
  }
  return {
    model: baseModel,
    ...user,
    input: prompt,
    tools: [tool],
    tool_choice: { type: IMAGE_TOOL_TYPE },
  };
};

const isPlainRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const imageRequestError = (message: string, param: string | null = null): ImageRequestFailure => ({
  ok: false,
  response: openaiError(400, message, "invalid_request_error", { param }),
});

const normalizeImageReference = (value: unknown): Record<string, string> | null => {
  if (!isPlainRecord(value)) return null;
  if (Object.keys(value).some((key) => key !== "image_url")) return null;
  if (
    !Object.prototype.hasOwnProperty.call(value, "image_url") ||
    typeof value.image_url !== "string" ||
    value.image_url.length > IMAGE_MAX_REFERENCE_URL_CHARS
  ) {
    return null;
  }
  const imageUrl = typeof value.image_url === "string" ? value.image_url.trim() : "";
  if (!imageUrl) return null;
  if (!/^data:/iu.test(imageUrl)) {
    if (!/^https?:\/\//iu.test(imageUrl)) return null;
    try {
      const remoteUrl = new URL(imageUrl);
      return remoteUrl.protocol === "http:" || remoteUrl.protocol === "https:" ? { image_url: imageUrl } : null;
    } catch {
      return null;
    }
  }
  const inlineData = normalizeImageDataUrl(imageUrl);
  return inlineData ? { image_url: inlineData.url } : null;
};

const normalizeImageMaskReference = (value: unknown): Record<string, string> | null => {
  const reference = normalizeImageReference(value);
  if (!reference) return null;
  const inlineData = normalizeImageDataUrl(reference.image_url);
  return inlineData?.mediaType === "image/png" && inlineData.bytes < IMAGE_MAX_MASK_FILE_BYTES
    ? { image_url: inlineData.url }
    : null;
};

const normalizeImageMediaType = (value: string): string | null => {
  const mediaType = value.split(";", 1)[0]?.trim().toLowerCase() ?? "";
  return IMAGE_MEDIA_TYPE_ALIASES.get(mediaType) ?? null;
};

const normalizeImageDataUrl = (
  value: string,
): Readonly<{ url: string; bytes: number; mediaType: string }> | null => {
  const match = /^data:([^;,]+);base64,([a-z0-9+/]*={0,2})$/iu.exec(value);
  if (!match) return null;
  const mediaType = normalizeImageMediaType(match[1]);
  const encoded = match[2];
  if (!mediaType || encoded.length === 0 || encoded.length % 4 !== 0) return null;
  const padding = encoded.endsWith("==") ? 2 : encoded.endsWith("=") ? 1 : 0;
  const bytes = encoded.length / 4 * 3 - padding;
  if (bytes <= 0 || bytes >= IMAGE_MAX_FILE_BYTES) return null;
  return { url: `data:${mediaType};base64,${encoded}`, bytes, mediaType };
};

const imageReferenceInputBytes = (body: Record<string, unknown>): number => {
  const references = [
    ...(Array.isArray(body.images) ? body.images : []),
    ...(isPlainRecord(body.mask) ? [body.mask] : []),
  ];
  let total = 0;
  for (const reference of references) {
    if (!isPlainRecord(reference)) continue;
    if (typeof reference.image_url === "string") {
      const inlineData = normalizeImageDataUrl(reference.image_url);
      total += inlineData?.bytes ?? IMAGE_TEXT_ENCODER.encode(reference.image_url).byteLength;
    }
  }
  return total;
};

const imageFileMediaType = (file: File): string | null => {
  const declared = file.type.trim().toLowerCase();
  if (declared && declared !== "application/octet-stream") return normalizeImageMediaType(declared);
  const normalizedName = file.name.trim().toLowerCase();
  for (const [extension, mediaType] of IMAGE_MEDIA_TYPE_BY_EXTENSION) {
    if (normalizedName.endsWith(extension)) return mediaType;
  }
  return null;
};

const fileDataUrl = async (file: File, mediaType: string): Promise<string> => {
  const encoded = bytesToBase64(new Uint8Array(await file.arrayBuffer()));
  return `data:${mediaType};base64,${encoded}`;
};

const parseMultipartInteger = (value: FormDataEntryValue | null): unknown => {
  if (typeof value !== "string") return value;
  const normalized = value.trim();
  return /^(?:0|[1-9][0-9]*)$/u.test(normalized) ? Number(normalized) : value;
};

const parseMultipartNumber = (value: FormDataEntryValue | null): unknown => {
  if (typeof value !== "string") return value;
  const normalized = value.trim();
  if (!/^-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?$/u.test(normalized)) return value;
  const parsed = Number(normalized);
  return Number.isFinite(parsed) ? parsed : value;
};

const exceedsUnicodeCodePointLimit = (value: string, limit: number): boolean => {
  let count = 0;
  for (const _character of value) {
    count += 1;
    if (count > limit) return true;
  }
  return false;
};

const parseMultipartBoolean = (value: FormDataEntryValue | null): unknown => {
  if (typeof value !== "string") return value;
  const normalized = value.trim();
  return normalized === "true" ? true : normalized === "false" ? false : value;
};

type ImageScalarValidationResult =
  | Readonly<{ ok: true; count: number }>
  | ImageRequestFailure;

const validateImageScalarOptions = (
  raw: Record<string, unknown>,
  kind: ImageRouteKind,
  multipart = false,
): ImageScalarValidationResult => {
  for (const key of IMAGE_NULLABLE_OPTION_KEYS) {
    if (raw[key] === null) delete raw[key];
  }
  if (Object.prototype.hasOwnProperty.call(raw, "style")) {
    return imageRequestError(
      "style is not supported because this gateway uses the Responses image-generation tool.",
      "style",
    );
  }
  if (typeof raw.prompt !== "string" || raw.prompt.trim().length === 0) {
    return imageRequestError("Image requests must include a prompt.", "prompt");
  }
  if (exceedsUnicodeCodePointLimit(raw.prompt, IMAGE_MAX_PROMPT_CHARS)) {
    return imageRequestError(
      `prompt must contain no more than ${IMAGE_MAX_PROMPT_CHARS} characters.`,
      "prompt",
    );
  }
  if (
    Object.prototype.hasOwnProperty.call(raw, "model") &&
    (typeof raw.model !== "string" || raw.model.trim().length === 0)
  ) {
    return imageRequestError("model must be a non-empty string.", "model");
  }
  if (raw.user !== undefined && typeof raw.user !== "string") {
    return imageRequestError("user must be a string.", "user");
  }
  const count = raw.n === undefined ? 1 : raw.n;
  if (!Number.isInteger(count) || (count as number) < 1 || (count as number) > IMAGE_MAX_COUNT) {
    return imageRequestError(`n must be an integer from 1 to ${IMAGE_MAX_COUNT}.`, "n");
  }
  if (
    raw.output_compression !== undefined &&
    (!Number.isInteger(raw.output_compression) ||
      (raw.output_compression as number) < 0 ||
      (raw.output_compression as number) > 100)
  ) {
    return imageRequestError("output_compression must be an integer from 0 to 100.", "output_compression");
  }
  if (raw.size !== undefined && (typeof raw.size !== "string" || raw.size.trim().length === 0)) {
    return imageRequestError("size must be a non-empty string.", "size");
  }
  const qualities = kind === "generations"
    ? IMAGE_GENERATION_QUALITIES
    : multipart
    ? IMAGE_EDIT_MULTIPART_QUALITIES
    : IMAGE_EDIT_JSON_QUALITIES;
  if (raw.quality !== undefined && (typeof raw.quality !== "string" || !qualities.has(raw.quality))) {
    return imageRequestError(
      `quality must be low, medium, high, or auto${kind === "edits" ? " for image edits" : ""}.`,
      "quality",
    );
  }
  if (
    raw.background !== undefined &&
    (typeof raw.background !== "string" || !IMAGE_BACKGROUNDS.has(raw.background))
  ) {
    return imageRequestError("background must be transparent, opaque, or auto.", "background");
  }
  if (
    raw.output_format !== undefined &&
    (typeof raw.output_format !== "string" || !IMAGE_OUTPUT_FORMATS.has(raw.output_format))
  ) {
    return imageRequestError("output_format must be png, jpeg, or webp.", "output_format");
  }
  if (
    raw.moderation !== undefined &&
    (typeof raw.moderation !== "string" || !IMAGE_MODERATION_LEVELS.has(raw.moderation))
  ) {
    return imageRequestError("moderation must be low or auto.", "moderation");
  }
  if (
    raw.input_fidelity !== undefined &&
    (typeof raw.input_fidelity !== "string" || !IMAGE_INPUT_FIDELITIES.has(raw.input_fidelity))
  ) {
    return imageRequestError("input_fidelity must be low or high.", "input_fidelity");
  }
  if (raw.stream !== undefined && typeof raw.stream !== "boolean") {
    return imageRequestError("stream must be a boolean.", "stream");
  }
  if (raw.stream === true || raw.partial_images !== undefined) {
    return imageRequestError(
      "Streaming image responses are not supported by this gateway.",
      raw.stream === true ? "stream" : "partial_images",
    );
  }
  if (raw.response_format !== undefined && raw.response_format !== "b64_json") {
    return imageRequestError("response_format must be b64_json.", "response_format");
  }
  return { ok: true, count: count as number };
};

type MultipartImageEditResult =
  | Readonly<{ ok: true; body: Record<string, unknown>; inputBytes: number }>
  | ImageRequestFailure;

const parseMultipartImageEdit = async (req: Request): Promise<MultipartImageEditResult> => {
  const declaredLength = req.headers.get("content-length");
  if (declaredLength !== null) {
    const normalizedLength = declaredLength.trim();
    if (!/^(?:0|[1-9][0-9]*)$/u.test(normalizedLength)) {
      discardRawBodyObserverOnce(req);
      return imageRequestError("Multipart Content-Length is invalid.");
    }
    const parsedLength = Number(normalizedLength);
    if (!Number.isSafeInteger(parsedLength) || parsedLength > IMAGE_MAX_MULTIPART_BODY_BYTES) {
      discardRawBodyObserverOnce(req);
      await req.body?.cancel().catch(() => {});
      return imageRequestError("Multipart image edits must be no larger than 64 MiB.", "image");
    }
  }
  let bytes: Uint8Array<ArrayBuffer> | null = null;
  let captured = false;
  try {
    const bounded = await readBoundedResponseBody(new Response(req.body), {
      maxBytes: IMAGE_MAX_MULTIPART_BODY_BYTES + 1,
      timeoutMs: BUFFERED_INFERENCE_DEADLINE_MS,
      signal: req.signal,
      cancellationReason: "Multipart image request exceeded its read limit",
    });
    bytes = bounded.bytes;
    if (!bounded.complete || bytes.byteLength > IMAGE_MAX_MULTIPART_BODY_BYTES) {
      return imageRequestError("Multipart image edits must be no larger than 64 MiB.", "image");
    }

    let form: FormData;
    try {
      form = await new Request(req.url, {
        method: req.method,
        headers: req.headers,
        body: bytes,
      }).formData();
    } catch {
      return imageRequestError("Image edits must include valid multipart form data.");
    }
    captured = captureRawBodyOnce(req, bytes);

    const unsupportedField = [...form.keys()].find((key) => !IMAGE_EDIT_MULTIPART_REQUEST_KEYS.has(key));
    if (unsupportedField) {
      return imageRequestError(`Unsupported image edit field: ${unsupportedField}.`, unsupportedField);
    }
    const body: Record<string, unknown> = {};
    for (
      const key of [
        "model",
        "prompt",
        "size",
        "quality",
        "background",
        "output_format",
        "input_fidelity",
        "response_format",
        "user",
      ]
    ) {
      const value = form.get(key);
      if (value !== null && typeof value !== "string") {
        return imageRequestError(`${key} must be a string.`, key);
      }
      if (typeof value === "string") body[key] = value;
    }
    const stream = form.get("stream");
    if (stream !== null) body.stream = parseMultipartBoolean(stream);
    for (const key of ["n", "partial_images"]) {
      const value = form.get(key);
      if (value !== null) body[key] = parseMultipartInteger(value);
    }
    const outputCompression = form.get("output_compression");
    if (outputCompression !== null) body.output_compression = parseMultipartNumber(outputCompression);
    const scalarValidation = validateImageScalarOptions(body, "edits", true);
    if (!scalarValidation.ok) return scalarValidation;
    // GPT Image responses are always base64. The validated multipart
    // compatibility field has no Responses-tool equivalent.
    delete body.response_format;
    const imageEntries = [...form.entries()].flatMap(([name, entry]) =>
      name === "image" || name === "image[]" ? [entry] : []
    );
    const masks = form.getAll("mask");
    if (imageEntries.length === 0 || imageEntries.length > IMAGE_MAX_EDIT_INPUTS) {
      return imageRequestError(`image must contain from 1 to ${IMAGE_MAX_EDIT_INPUTS} files.`, "image");
    }
    if (imageEntries.some((entry) => !(entry instanceof File))) {
      return imageRequestError("Each multipart image entry must be a file.", "image");
    }
    if (masks.length > 1 || (masks.length === 1 && !(masks[0] instanceof File))) {
      return imageRequestError("mask must be one image file.", "mask");
    }
    const imageFiles = imageEntries as File[];
    const maskFile = masks[0] instanceof File ? masks[0] : null;
    if (imageFiles.some((file) => file.size === 0 || file.size >= IMAGE_MAX_FILE_BYTES)) {
      return imageRequestError("Each multipart image file must be non-empty and smaller than 50 MiB.", "image");
    }
    const imageMediaTypes = imageFiles.map(imageFileMediaType);
    if (imageMediaTypes.some((mediaType) => mediaType === null)) {
      return imageRequestError("Each multipart image must be a PNG, JPEG, or WebP file.", "image");
    }
    let maskMediaType: string | null = null;
    if (maskFile) {
      maskMediaType = imageFileMediaType(maskFile);
      if (
        maskFile.size === 0 ||
        maskFile.size >= IMAGE_MAX_MASK_FILE_BYTES ||
        maskMediaType !== "image/png"
      ) {
        return imageRequestError("The multipart mask must be a non-empty PNG file smaller than 50 MiB.", "mask");
      }
    }
    const files = maskFile ? [...imageFiles, maskFile] : imageFiles;
    const totalInputBytes = files.reduce((total, file) => total + file.size, 0);
    if (totalInputBytes > IMAGE_MAX_MULTIPART_INPUT_BYTES) {
      return imageRequestError("Multipart image files must total no more than 50 MiB.", "image");
    }
    if (totalInputBytes > Math.floor(IMAGE_MAX_FANOUT_INPUT_BYTES / scalarValidation.count)) {
      return imageRequestError("n and multipart image inputs exceed the 50 MiB request work limit.", "n");
    }
    const images: Array<Record<string, string>> = [];
    for (let index = 0; index < imageFiles.length; index += 1) {
      images.push({ image_url: await fileDataUrl(imageFiles[index], imageMediaTypes[index] as string) });
    }
    body.images = images;
    if (maskFile) {
      body.mask = {
        image_url: await fileDataUrl(maskFile, maskMediaType as string),
      };
    }
    return { ok: true, body, inputBytes: totalInputBytes };
  } finally {
    discardRawBodyObserverOnce(req);
    if (bytes && !captured) bytes.fill(0);
  }
};

const parseImageRequest = async (
  req: Request,
  kind: ImageRouteKind,
): Promise<ParsedImageRequest> => {
  const mediaType = req.headers.get("content-type")?.split(";", 1)[0]?.trim().toLowerCase() ?? "";
  let raw: unknown;
  let multipartInputBytes = 0;
  let isMultipart = false;
  if (kind === "edits" && mediaType === "multipart/form-data") {
    const multipart = await parseMultipartImageEdit(req);
    if (!multipart.ok) return multipart;
    raw = multipart.body;
    multipartInputBytes = multipart.inputBytes;
    isMultipart = true;
  } else {
    raw = await readJsonBody(req, kind === "edits" ? IMAGE_MAX_JSON_BODY_BYTES : undefined);
    if (raw === null) {
      return imageRequestError(
        kind === "edits"
          ? "Image edits must use a JSON object or multipart form-data body."
          : "Image generation requests must use a JSON object body.",
      );
    }
  }
  if (!isPlainRecord(raw)) return imageRequestError("Image requests must use an object body.");
  const allowedKeys = kind === "edits" ? IMAGE_EDIT_JSON_REQUEST_KEYS : IMAGE_GENERATION_REQUEST_KEYS;
  const unsupportedField = findUnknownKey(raw, allowedKeys);
  if (unsupportedField) {
    return imageRequestError(`Unsupported image ${kind} field: ${unsupportedField}.`, unsupportedField);
  }
  const scalarValidation = validateImageScalarOptions(raw, kind, isMultipart);
  if (!scalarValidation.ok) return scalarValidation;
  const { count } = scalarValidation;
  if (kind === "edits") {
    if (!Array.isArray(raw.images) || raw.images.length === 0 || raw.images.length > IMAGE_MAX_EDIT_INPUTS) {
      return imageRequestError(`images must contain from 1 to ${IMAGE_MAX_EDIT_INPUTS} image references.`, "images");
    }
    if (!isMultipart) {
      const images = raw.images.map(normalizeImageReference);
      if (images.some((image) => image === null)) {
        return imageRequestError(
          "Each image must contain one image_url with a fully qualified HTTP(S) URL or supported base64 data URL; file_id is unsupported by this gateway.",
          "images",
        );
      }
      raw.images = images;
      if (Object.prototype.hasOwnProperty.call(raw, "mask")) {
        const mask = normalizeImageMaskReference(raw.mask);
        if (!mask) {
          return imageRequestError(
            "mask.image_url must be a supported base64 PNG data URL; remote URLs and file_id are unsupported by this gateway.",
            "mask",
          );
        }
        raw.mask = mask;
      }
    }
  }
  const inputBytes = Math.max(multipartInputBytes, imageReferenceInputBytes(raw));
  if (inputBytes > Math.floor(IMAGE_MAX_FANOUT_INPUT_BYTES / (count as number))) {
    return imageRequestError("n and inline image inputs exceed the 50 MiB request work limit.", "n");
  }
  return { ok: true, body: raw, count: count as number, inputBytes };
};

const imageResponseHeaders = (responses: readonly Response[]): Record<string, string> => {
  const headers: Record<string, string> = {};
  const warnings = Array.from(new Set(responses.flatMap(responseWarnings)));
  if (warnings.length > 0) headers[UOS_WARNING_HEADER] = warnings.join(", ");
  for (const name of ["x-uos-upstream", "Retry-After", ...STANDARD_RATE_LIMIT_HEADERS]) {
    const values = responses.map((response) => response.headers.get(name));
    if (values.length > 0 && values.every((value) => value !== null && value === values[0])) {
      headers[name] = values[0] as string;
    }
  }
  return headers;
};

type ImageFanoutDispatchCoordinator = Readonly<{
  beforeProviderDispatchFor: (callIndex: number) => NonNullable<UsageContext["beforeProviderDispatch"]>;
  cancelBeforeTransport: () => Promise<void>;
  settled: (callIndex: number) => void;
}>;

/**
 * One Images request can launch several Responses calls, while API-key
 * admission intentionally reserves one logical request. Keep that reservation
 * committed when any sibling reaches transport; otherwise a cancelled leader
 * could refund it after a follower has already started its upstream fetch.
 */
export const createImageFanoutDispatchCoordinator = (
  callCount: number,
  beforeProviderDispatch: NonNullable<UsageContext["beforeProviderDispatch"]>,
): ImageFanoutDispatchCoordinator => {
  const settledCalls = new Set<number>();
  let transportStarted = false;
  let providerDispatch: ApiKeyProviderDispatch | null = null;
  let cancellation: Promise<void> | null = null;
  let resolveAllCallsSettled!: () => void;
  const allCallsSettled = new Promise<void>((resolve) => {
    resolveAllCallsSettled = resolve;
  });

  const settled = (callIndex: number): void => {
    if (settledCalls.has(callIndex)) return;
    settledCalls.add(callIndex);
    if (settledCalls.size === callCount) resolveAllCallsSettled();
  };
  const cancelBeforeTransport = async (): Promise<void> => {
    await allCallsSettled;
    if (transportStarted || !providerDispatch) return;
    cancellation ??= providerDispatch.cancelBeforeTransport();
    await cancellation;
  };

  return {
    beforeProviderDispatchFor: (callIndex) => async (provider) => {
      const dispatch = await beforeProviderDispatch(provider);
      if (dispatch && !providerDispatch) {
        providerDispatch = dispatch;
        if (transportStarted) providerDispatch.markTransportStarted();
      }
      return {
        markTransportStarted: () => {
          transportStarted = true;
          settled(callIndex);
          providerDispatch?.markTransportStarted();
        },
        cancelBeforeTransport: () => {
          settled(callIndex);
          // A child owns only its settlement signal. Waiting for every logical
          // child here deadlocks when a first-batch failure prevents a later
          // batch from starting. The outer Images handler owns the one shared
          // cancellation after it marks those unstarted calls settled.
          return Promise.resolve();
        },
      };
    },
    cancelBeforeTransport,
    settled,
  };
};

const imageCallUsageContext = (
  context: UsageContext | undefined,
  index: number,
  kind: ImageRouteKind,
): UsageContext | undefined => {
  if (!context) return undefined;
  const requestId = index === 0 || !context.requestId
    ? context.requestId
    : `${context.requestId}:image:${kind}:${index + 1}`;
  // The translated request targets a hidden text host model. Reusing its paid
  // roster would authorize and settle the requested image under the wrong
  // model, so image calls remain Codex-only until image billing is explicit.
  return {
    ...context,
    requestId,
    paidFallbackEnabled: false,
    onTerminalUsage: undefined,
  };
};

const usageTokensFromTelemetry = (state: ResponseTelemetryState): UsageTokens | null =>
  state.usageObserved
    ? {
      inputTokens: state.inputTokens,
      cachedInputTokens: state.cachedInputTokens,
      cacheWriteInputTokens: state.cacheWriteInputTokens,
      outputTokens: state.outputTokens,
      totalTokens: state.totalTokens,
      status: state.usageTelemetryStatus,
    }
    : null;

const finalizeImageResponse = (
  sources: readonly Response[],
  target: Response,
  context: UsageContext | undefined,
): Response => {
  const response = aggregateResponseTelemetry(sources, target);
  const aggregate = responseTelemetry.get(response);
  if (aggregate && context?.responseTelemetry && context.responseTelemetry !== aggregate) {
    Object.assign(context.responseTelemetry, aggregate);
    responseTelemetry.set(response, context.responseTelemetry);
  }
  if (aggregate && context?.onTerminalUsage) {
    try {
      context.onTerminalUsage(usageTokensFromTelemetry(aggregate), aggregate.completed);
    } catch {
      // Observability callbacks cannot alter the translated response.
    }
  }
  return response;
};

/**
 * Collect generated images from a Responses payload. The tool reports each
 * image as an `image_generation_call` output item whose `result` is base64.
 */
export const extractImagesFromResponses = (
  payload: unknown,
): Array<Record<string, unknown>> => {
  if (!isPlainRecord(payload) || !Array.isArray(payload.output)) return [];
  const images: Array<Record<string, unknown>> = [];
  for (const item of payload.output) {
    if (!isPlainRecord(item) || item.type !== "image_generation_call") continue;
    const result = typeof item.result === "string" ? item.result : "";
    if (!result) continue;
    const image: Record<string, unknown> = { b64_json: result };
    if (typeof item.revised_prompt === "string") image.revised_prompt = item.revised_prompt;
    images.push(image);
  }
  return images;
};

const extractImageOutputFormatFromResponses = (payload: unknown): string | null => {
  if (!isPlainRecord(payload) || !Array.isArray(payload.output)) return null;
  for (const item of payload.output) {
    if (
      !isPlainRecord(item) || item.type !== "image_generation_call" ||
      typeof item.result !== "string" || item.result.length === 0
    ) continue;
    return typeof item.output_format === "string" && IMAGE_OUTPUT_FORMATS.has(item.output_format)
      ? item.output_format
      : null;
  }
  return null;
};

export const handleImages = async (
  req: Request,
  kind: ImageRouteKind,
  usageContext?: UsageContext,
  options: ImageHandlerOptions = {},
): Promise<Response> => {
  const parsed = await parseImageRequest(req, kind);
  if (!parsed.ok) return parsed.response;
  const { body, count } = parsed;

  const baseModel = await resolveImageBaseModel();
  if (!baseModel) {
    return openaiError(
      503,
      "No base model is available to host image generation.",
      "model_unavailable",
    );
  }

  const responsesBody = buildImageResponsesRequest(body, baseModel, kind);
  const encodedResponsesBody = JSON.stringify(responsesBody);
  if (
    count > 1 &&
    IMAGE_TEXT_ENCODER.encode(encodedResponsesBody).byteLength > Math.floor(IMAGE_MAX_JSON_BODY_BYTES / count)
  ) {
    return imageRequestError("n and the translated image request exceed the fan-out work limit.", "n").response;
  }
  const serializedResponsesBody = options.dispatch ? encodedResponsesBody : null;
  const fanoutDispatch = count > 1 && !options.dispatch && usageContext?.beforeProviderDispatch
    ? createImageFanoutDispatchCoordinator(count, usageContext.beforeProviderDispatch)
    : null;
  const fanoutAbort = count > 1 ? new AbortController() : null;
  const childSignal = fanoutAbort
    ? AbortSignal.any([
      req.signal,
      ...(usageContext?.downstreamSignal ? [usageContext.downstreamSignal] : []),
      fanoutAbort.signal,
    ])
    : req.signal;
  type ImageFanoutFailure =
    | Readonly<{ kind: "response"; response: Response }>
    | Readonly<{ kind: "throw"; error: unknown }>;
  let firstFailure: ImageFanoutFailure | undefined;
  const recordFirstFailure = (failure: ImageFanoutFailure): void => {
    if (firstFailure !== undefined) return;
    // Abort listeners run synchronously, so preserve the temporal leader first.
    firstFailure = failure;
    fanoutAbort?.abort(new DOMException("A sibling image generation call failed.", "AbortError"));
  };
  const runChild = async (index: number): Promise<Response> => {
    // This is an internal JSON rewrite, not a proxy hop. In particular, an
    // outer Images Idempotency-Key cannot identify several independent child
    // generations, and client forwarding/authentication headers do not
    // describe the newly serialized request body.
    const headers = new Headers({ "content-type": "application/json" });
    const childContext = imageCallUsageContext(usageContext, index, kind);
    const coordinatedChildContext = childContext && (fanoutDispatch || fanoutAbort)
      ? {
        ...childContext,
        ...(fanoutAbort ? { downstreamSignal: childSignal } : {}),
        ...(fanoutDispatch ? { beforeProviderDispatch: fanoutDispatch.beforeProviderDispatchFor(index) } : {}),
      }
      : childContext;
    const request = new Request(new URL("/v1/responses", req.url), {
      method: "POST",
      headers,
      ...(serializedResponsesBody === null ? {} : { body: serializedResponsesBody }),
      signal: childSignal,
    });
    try {
      const upstream = options.dispatch
        ? await options.dispatch(request)
        : await runResponsesHandler(request, coordinatedChildContext, responsesBody);
      if (!upstream.ok) recordFirstFailure({ kind: "response", response: upstream });
      return upstream;
    } catch (error) {
      recordFirstFailure({ kind: "throw", error });
      throw error;
    } finally {
      fanoutDispatch?.settled(index);
    }
  };
  const settledUpstreams: PromiseSettledResult<Response>[] = [];
  const indexes = Array.from({ length: count }, (_, index) => index);
  settledUpstreams.push(...await Promise.allSettled(indexes.map(runChild)));
  const nextChildIndex = count;
  // The shared paid-dispatch coordinator waits for every logical child. Mark
  // calls that never started after a sibling failure as settled before cancel.
  for (let index = nextChildIndex; index < count; index += 1) fanoutDispatch?.settled(index);
  const failure = firstFailure;
  if (failure !== undefined) {
    await fanoutDispatch?.cancelBeforeTransport().catch(() => {});
    if (failure.kind === "throw") throw failure.error;
  }
  const upstreams = failure?.kind === "response"
    ? settledUpstreams.flatMap((result) => result.status === "fulfilled" ? [result.value] : [])
    : settledUpstreams.map((result) => {
      if (result.status === "rejected") throw result.reason;
      return result.value;
    });
  if (failure?.kind === "response") {
    const leaderIndex = upstreams.indexOf(failure.response);
    if (leaderIndex > 0) upstreams.unshift(upstreams.splice(leaderIndex, 1)[0] as Response);
  }
  const images: Array<Record<string, unknown>> = [];
  let created: number | null = null;
  let outputFormat: string | null = null;
  let outputFormatConsistent = true;
  for (const upstream of upstreams) {
    const text = await upstream.text();
    let payload: unknown = null;
    try {
      if (text.length > 0) payload = JSON.parse(text) as unknown;
    } catch {
      const response = openaiError(502, "Image upstream returned a non-JSON response.", "upstream_invalid", {
        headers: imageResponseHeaders([upstream]),
      });
      return finalizeImageResponse(upstreams, response, usageContext);
    }
    // A failed Responses call already carries an OpenAI-shaped error body;
    // pass it through unchanged so quota and roster errors stay actionable.
    if (!upstream.ok) {
      const response = json(upstream.status, payload, imageResponseHeaders([upstream]));
      return finalizeImageResponse(upstreams, response, usageContext);
    }
    const callImages = extractImagesFromResponses(payload);
    if (callImages.length === 0) {
      const response = openaiError(
        502,
        "The model did not return an image for this request.",
        "image_generation_failed",
        { headers: imageResponseHeaders(upstreams) },
      );
      return finalizeImageResponse(upstreams, response, usageContext);
    }
    images.push(callImages[0]);
    const callOutputFormat = extractImageOutputFormatFromResponses(payload);
    if (callOutputFormat === null) outputFormatConsistent = false;
    else if (outputFormat === null) outputFormat = callOutputFormat;
    else if (outputFormat !== callOutputFormat) outputFormatConsistent = false;
    if (created === null && isPlainRecord(payload) && typeof payload.created_at === "number") {
      created = payload.created_at;
    }
  }
  created ??= Math.floor(Date.now() / 1000);
  const response = json(200, {
    created,
    data: images,
    ...(outputFormatConsistent && outputFormat ? { output_format: outputFormat } : {}),
  }, imageResponseHeaders(upstreams));
  return finalizeImageResponse(upstreams, response, usageContext);
};
