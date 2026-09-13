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
import { CODEX_CHATGPT_PROMPT_CACHE_PROVIDER, normalizePromptCacheCapabilities, type PromptCacheControls } from "./codex_models.ts";
import { type ApiKeyProviderDispatch, ApiKeyQuotaDispatchError, type ApiKeyUsageReservation } from "./api_key_policy.ts";
import { DEFAULT_REASONING_EFFORT, normalizeReasoningEffort, type ReasoningEffort } from "./defaults.ts";
import { BOUNDED_RESPONSE_BODY_MAX_BYTES, BOUNDED_RESPONSE_BODY_TIMEOUT_MS, readBoundedResponseBody } from "./bounded_response_body.ts";
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
import { recordCerebrasProviderHealth, recordMeteredProviderHealth, recordSurplusProviderHealth } from "./provider_health.ts";
import { getString, isRecord, sha256Hex } from "./utils.ts";
import type { ChatCompletionRequest, MessageContentItem, PromptCacheBreakpoint, ResponseInputItem, ResponseMessageItem, ResponsesRequest } from "./types.ts";
import { fetchMeteredModels, fetchMeteredResponses, METERED_MODELS_CACHE_TTL_MS, MeteredError, readMeteredApiKey } from "./metered.ts";
import { fetchSurplusModels, fetchSurplusResponses, readSurplusApiKey, SURPLUS_MODELS_CACHE_TTL_MS, SurplusError } from "./surplus.ts";
import { loadDebugRoutingConfig } from "./debug_routing.ts";
import type { SentinelUpstreamRecorder } from "./sentinel_upstream_capture.ts";

// Temporary hard cut while this exact gateway model has free Surplus inference.
// Remove the cut when the free-inference window ends; do not generalize it to
// other catalog models or paid-fallback routing.
const TEMPORARY_FREE_SURPLUS_MODEL = "glm-5.2";

const isTemporaryFreeSurplusModel = (model: string): boolean => model === TEMPORARY_FREE_SURPLUS_MODEL;

const temporaryFreeSurplusCapabilityError = (model: string, body: Record<string, unknown>): Response | null =>
  isTemporaryFreeSurplusModel(model) && Array.isArray(body.tools) && body.tools.length > 0
    ? openaiError(400, `The model '${model}' does not support tools through this gateway.`, "unsupported_model_capability", { param: "tools" })
    : null;

const getDefaultModel = async (): Promise<string | null> => {
  const runtime = await loadRuntimeConfig();
  return runtime?.default_model ?? getCodexModelsSnapshotDefaultModel(runtime?.codex_models ?? null);
};

const downstreamSignalFor = (request: Request, context?: UsageContext): AbortSignal => context?.downstreamSignal ?? request.signal;

const inferenceSignal = (request: Request, context?: UsageContext): AbortSignal => createInferenceSignal(downstreamSignalFor(request, context));

/**
 * Internal test seam for exercising the public OpenAI handlers through the
 * same guarded banked-reset flow. It has no request-schema or runtime-config
 * surface, and remains unset in production.
 */
type CodexBankedResetOptionsForTest = NonNullable<Parameters<typeof fetchCodexResponses>[1]>["bankedReset"];
let codexBankedResetOptionsForTest: CodexBankedResetOptionsForTest | null = null;

export const setCodexBankedResetOptionsForTest = (options: CodexBankedResetOptionsForTest | null): void => {
  codexBankedResetOptionsForTest = options;
};

const defaultModelUnavailableError = (): Response =>
  openaiError(503, "Default model is unavailable: no configured default model or Codex model snapshot.", "server_error");

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
  beforeProviderDispatch?: ApiKeyUsageReservation["beforeProviderDispatch"];
  /** Request-owned passive upstream recorder for accepted inference requests. */
  sentinelUpstreamRecorder?: SentinelUpstreamRecorder;
  /** Test seam for proving one terminal usage observation per response. */
  onTerminalUsage?: (usage: UsageTokens | null, completed: boolean) => void;
}>;

type UpstreamProvider = "cerebras" | "chatgpt_codex" | "removed_provider" | "metered" | "surplus";
const supportsReasoningProgressRelease = (provider: UpstreamProvider): boolean =>
  provider === "chatgpt_codex" || provider === "surplus" || provider === "metered";
export type InferenceFallbackReason = "primary_quota_blocked" | "dynamic_paid_model";
export type UsageTelemetryStatus = "missing" | "partial" | "reported" | "invalid";
export type PromptCacheMode = "implicit" | "explicit" | "legacy_retention" | "unspecified";
export type AffinityOutcome = "none" | "preferred" | "preferred_unavailable" | "remapped" | "failover" | "shadow_only";

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

export type ResponseStreamTerminalType = "response.completed" | "response.failed" | "response.incomplete" | "error" | "eof" | "cancelled" | "deadline";

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

const withResponseTelemetryContext = (context: UsageContext | undefined, state: ResponseTelemetryState): UsageContext => ({
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
  sentinelUpstreamRecorder: context?.sentinelUpstreamRecorder,
  onTerminalUsage: context?.onTerminalUsage,
  responseTelemetry: state,
});

// A header present with an empty value still falls back to the gateway label,
// so this keeps the original falsy fallback instead of a nullish one.
const upstreamProviderLabel = (response: Response): string => {
  const header = response.headers.get("x-uos-upstream");
  if (header === null || header === "") return "gateway";
  return header;
};

const attachResponseTelemetry = (response: Response, state: ResponseTelemetryState): Response => {
  state.provider ??= upstreamProviderLabel(response);
  responseTelemetry.set(response, state);
  return response;
};

const sumTelemetryCounts = (
  states: readonly ResponseTelemetryState[],
  key: "inputTokens" | "cachedInputTokens" | "cacheWriteInputTokens" | "outputTokens" | "totalTokens",
  expectedCount: number
): number | null => {
  if (states.length !== expectedCount || states.some((state) => state[key] === null)) return null;
  return states.reduce((total, state) => total + (state[key] ?? 0), 0);
};

const commonTelemetryValue = <T>(values: readonly T[]): T | null => {
  if (values.length === 0) return null;
  const first = values[0] as T;
  return values.every((value) => Object.is(value, first)) ? first : null;
};

// An invalid member state poisons the aggregate, an incomplete set of members
// can never be reported as complete, and a fully reported set counts as
// reported once usage was observed.
const aggregatedUsageTelemetryStatus = (states: readonly ResponseTelemetryState[], sourceCount: number, usageObserved: boolean): UsageTelemetryStatus => {
  if (states.some((state) => state.usageTelemetryStatus === "invalid")) return "invalid";
  if (states.length !== sourceCount) return usageObserved ? "partial" : "missing";
  if (states.every((state) => state.usageTelemetryStatus === "reported")) return "reported";
  return usageObserved ? "partial" : "missing";
};

const aggregatedQuotaUsedPercent = (usedPercents: readonly number[], states: readonly ResponseTelemetryState[]): number | null | undefined => {
  if (usedPercents.length > 0) return Math.max(...usedPercents);
  return states.some((state) => state.quotaUsedPercent === null) ? null : undefined;
};

const aggregatedSemanticOutputObserved = (states: readonly ResponseTelemetryState[]): boolean | null => {
  if (states.some((state) => state.semanticOutputObserved === true)) return true;
  return states.every((state) => state.semanticOutputObserved === false) ? false : null;
};

const aggregatedProviderLabel = (providers: readonly string[]): string | null => {
  if (providers.length === 1) return providers[0];
  if (providers.length > 1) return "mixed";
  return null;
};

const aggregateResponseTelemetry = (sources: readonly Response[], target: Response): Response => {
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
  aggregate.provider = aggregatedProviderLabel(providers);
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
  aggregate.usageTelemetryStatus = aggregatedUsageTelemetryStatus(states, sources.length, aggregate.usageObserved);
  aggregate.promptCacheKeyPresent = states.some((state) => state.promptCacheKeyPresent);
  aggregate.promptCacheMode = commonTelemetryValue(states.map((state) => state.promptCacheMode)) ?? "unspecified";
  aggregate.explicitBreakpointCount = Math.max(0, ...states.map((state) => state.explicitBreakpointCount));
  aggregate.accountSlot = commonTelemetryValue(states.map((state) => state.accountSlot));
  aggregate.accountCohortId = commonTelemetryValue(states.map((state) => state.accountCohortId));
  aggregate.affinityOutcome = commonTelemetryValue(states.map((state) => state.affinityOutcome)) ?? "failover";
  const usedPercents = states.map((state) => state.quotaUsedPercent).filter((value): value is number => typeof value === "number");
  aggregate.quotaUsedPercent = aggregatedQuotaUsedPercent(usedPercents, states);
  aggregate.completed = states.length === sources.length && states.every((state) => state.completed);
  aggregate.semanticOutputObserved = aggregatedSemanticOutputObserved(states);
  aggregate.upstreamEventKinds = [...new Set(states.flatMap((state) => state.upstreamEventKinds))];
  aggregate.streamTerminalType = aggregate.completed ? "response.completed" : commonTelemetryValue(states.map((state) => state.streamTerminalType));
  aggregate.failureKind = commonTelemetryValue(states.map((state) => state.failureKind));
  aggregate.responseCreatedObserved = states.length === sources.length && states.every((state) => state.responseCreatedObserved);
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
      | "firstSemanticCommitmentMs"
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
  const terminalTimes = states.map((state) => state.streamTerminalMs).filter((value): value is number => value !== null);
  aggregate.streamTerminalMs = terminalTimes.length > 0 ? Math.max(...terminalTimes) : null;
  aggregate.attemptedProviders = [...new Set(states.flatMap((state) => state.attemptedProviders))];
  aggregate.removedProviderTriggerClass = commonTelemetryValue(states.map((state) => state.removedProviderTriggerClass));
  aggregate.removedProviderCircuitTransition = commonTelemetryValue(states.map((state) => state.removedProviderCircuitTransition));
  aggregate.removedProviderSelectedModel = commonTelemetryValue(states.map((state) => state.removedProviderSelectedModel));
  aggregate.removedProviderTaskType = commonTelemetryValue(states.map((state) => state.removedProviderTaskType));
  aggregate.removedProviderSemanticCommitment = commonTelemetryValue(states.map((state) => state.removedProviderSemanticCommitment));
  const removedProviderLatencies = states.map((state) => state.removedProviderLatencyMs).filter((value): value is number => value !== null);
  aggregate.removedProviderLatencyMs = removedProviderLatencies.length > 0 ? Math.max(...removedProviderLatencies) : null;
  aggregate.removedProviderTerminalStatus = commonTelemetryValue(states.map((state) => state.removedProviderTerminalStatus));
  return attachResponseTelemetry(target, aggregate);
};

export const getResponseTelemetry = (response: Response): ResponseTelemetry | null => {
  const state = responseTelemetry.get(response);
  if (!state) return null;
  return {
    provider: state.provider ?? upstreamProviderLabel(response),
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
export const getResponseAccountCohortId = (response: Response): string | null => responseTelemetry.get(response)?.accountCohortId ?? null;

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
const recordResponseTiming = (context: UsageContext | undefined, field: ResponseTelemetryTimingField): void => {
  const state = context?.responseTelemetry;
  const startedAtMs = context?.startedAtMonotonicMs;
  if (!state) return;
  if (state[field] !== null || typeof startedAtMs !== "number" || !Number.isFinite(startedAtMs)) return;
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

const recordRemovedProviderFields = (context: UsageContext | undefined, fields: Readonly<Record<string, string | number | null | undefined>>): void => {
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

const persistRemovedProviderFields = (_context: UsageContext | undefined): Promise<void> => recordRemovedProviderTelemetry({}).catch(() => {});

const persistFailedRemovedProviderAttempt = (context: UsageContext | undefined, _startedAtMonotonicMs: number, triggerClass: string): Promise<void> => {
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

const boundedResponseEventKind = (type: string): string => (CHAT_RESPONSE_EVENT_KINDS.has(type) ? type : "unrecognized");

// An incomplete terminal is a recorded failure only when its reason names a
// gateway, provider or transport fault; unrecognized reasons are ignored.
const incompleteFailureKind = (event: ResponsesStreamEvent): string | null => {
  const response = isRecord(event.value.response) ? event.value.response : null;
  if (!response) return null;
  const details = isRecord(response.incomplete_details) ? response.incomplete_details : null;
  if (!details) return null;
  const reason = typeof details.reason === "string" ? details.reason : null;
  if (!reason) return null;
  // This character class is case-insensitive, so the upper-case range already
  // covers the lower-case letters.
  if (!/^(?:gateway|provider|upstream|server|network|timeout|deadline)[A-Z0-9_.:-]*$/i.test(reason)) return null;
  return `response_incomplete:${reason}`;
};

const recordResponsesEventTelemetry = (context: UsageContext | undefined, event: ResponsesStreamEvent): void => {
  const telemetry = context?.responseTelemetry;
  if (!telemetry) return;
  const eventKind = boundedResponseEventKind(event.type);
  if (!telemetry.upstreamEventKinds.includes(eventKind)) telemetry.upstreamEventKinds.push(eventKind);
  if (event.type === "response.created") telemetry.responseCreatedObserved = true;
  if (event.type === "response.incomplete") {
    const failureKind = incompleteFailureKind(event);
    if (failureKind !== null) telemetry.failureKind = failureKind;
  }
  if (isSyntheticResponsesFailureEvent(event)) {
    telemetry.syntheticTerminalType = event.type === "response.failed" || event.type === "error" ? event.type : null;
  }
};

const failureKindFromError = (error: unknown): ResponsesStreamFailureKind => (error instanceof ResponsesStreamError ? error.kind : "read_error");

const recordResponsesFailureTelemetry = (context: UsageContext | undefined, error: unknown, details?: OwnedResponsesStreamFailureDetails): void => {
  const telemetry = context?.responseTelemetry;
  if (!telemetry) return;
  telemetry.failureKind = details?.failureKind ?? failureKindFromError(error);
  if (details) {
    telemetry.responseCreatedObserved = details.responseCreatedObserved;
    telemetry.semanticOutputObserved = details.semanticCommitmentObserved;
    telemetry.syntheticTerminalType = details.syntheticTerminalType;
  }
};

const runWithResponseTelemetry = async (context: UsageContext | undefined, run: (context: UsageContext) => Promise<Response>): Promise<Response> => {
  const state = createResponseTelemetryState();
  const telemetryContext = withResponseTelemetryContext(context, state);
  const response = await run(telemetryContext);
  // Attempt-level terminal types can be abandoned during failover. Record a
  // precommit terminal time only after the handler has selected the final
  // response, and only when an upstream SSE event was actually observed.
  if (
    state.firstUpstreamSseEventMs !== null &&
    state.firstSemanticCommitmentMs === null &&
    state.streamTerminalType !== null &&
    state.streamTerminalMs === null
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
  const details = detailsPresent && isRecord(value.input_tokens_details) && !Array.isArray(value.input_tokens_details) ? value.input_tokens_details : null;
  const cachedInputTokens = parseUsageToken(details?.cached_tokens, details !== null && Object.prototype.hasOwnProperty.call(details, "cached_tokens"));
  const cacheWriteInputTokens = parseUsageToken(
    details?.cache_write_tokens,
    details !== null && Object.prototype.hasOwnProperty.call(details, "cache_write_tokens")
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
  const cachedTokensExceedInput = inputTokens.value !== null && cachedInputTokens.value !== null && cachedInputTokens.value > inputTokens.value;
  const inconsistentTotals = !coreMissing && inputTokens.value + outputTokens.value !== totalTokens.value;
  const invalid =
    inputTokens.invalid ||
    outputTokens.invalid ||
    totalTokens.invalid ||
    (detailsPresent && details === null) ||
    cachedInputTokens.invalid ||
    cacheWriteInputTokens.invalid ||
    cachedTokensExceedInput ||
    inconsistentTotals;

  let status: UsageTelemetryStatus;
  if (invalid) status = "invalid";
  else if (coreMissing || cacheReadMissing) status = "partial";
  else status = "reported";

  return {
    inputTokens: inputTokens.value,
    cachedInputTokens: cachedInputTokens.value,
    cacheWriteInputTokens: cacheWriteInputTokens.value,
    outputTokens: outputTokens.value,
    totalTokens: totalTokens.value,
    status,
  };
};

const toChatUsage = (usage: UsageTokens | null): Record<string, unknown> | null => {
  if (usage === null) return null;
  if (usage.inputTokens === null || usage.outputTokens === null || usage.totalTokens === null) {
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
  const options = isRecord(rawRecord.prompt_cache_options) && !Array.isArray(rawRecord.prompt_cache_options) ? rawRecord.prompt_cache_options : null;
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
  }
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

const recordCompletionUsage = (context: UsageContext | undefined, usage: UsageTokens | null): Promise<void> => {
  recordTerminalUsage(context, usage, true);
  return Promise.resolve();
};

const recordTerminalUsage = (context: UsageContext | undefined, usage: UsageTokens | null, completed: boolean): void => {
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
    context.onTerminalUsage?.(usage, completed);
  } catch {
    // A test/observability callback cannot alter response delivery.
  }
};

const recordErrorUsage = (_context: UsageContext | undefined): Promise<void> => Promise.resolve();

const recordStreamTerminalType = (context: UsageContext | undefined, terminalType: ResponseStreamTerminalType): void => {
  const telemetry = context?.responseTelemetry;
  if (!telemetry) return;
  telemetry.streamTerminalType = terminalType;
  if (telemetry.firstSemanticCommitmentMs !== null) recordStreamTerminal(context);
};

const classifyStreamFailure = (error: unknown, signal: AbortSignal, downstreamSignal: AbortSignal): ResponseStreamTerminalType => {
  if (isTimeoutFailure(error, signal.reason, downstreamSignal.reason)) return "deadline";
  if (downstreamSignal.aborted) return "cancelled";
  if (signal.aborted) return "deadline";
  if (error instanceof ResponsesStreamError && error.kind === "inactivity_timeout") return "deadline";
  if (error instanceof ResponsesStreamError && error.kind === "premature_eof") return "eof";
  return "error";
};

const isTimeoutFailure = (...values: readonly unknown[]): boolean =>
  values.some((value) => (value instanceof CodexError && value.code === "gateway_timeout") || (value instanceof Error && value.name === "TimeoutError"));

const classifyPreHeaderFailure = (error: unknown, signal: AbortSignal, downstreamSignal: AbortSignal): ResponseStreamTerminalType => {
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
  param?: string | null
): Response => {
  const mergedWarnings = Array.from(new Set(warnings));
  const hasAuthWarning = mergedWarnings.includes(CODEX_AUTH_REAUTH_WARNING);
  const headers: Record<string, string> = { "x-uos-upstream": provider };
  if (mergedWarnings.length) headers["x-uos-warning"] = mergedWarnings.join(", ");
  return openaiError(status, hasAuthWarning ? message + " " + CODEX_AUTH_REAUTH_MESSAGE : message, code, {
    ...(type ? { type } : {}),
    ...(param !== undefined ? { param } : {}),
    headers,
  });
};

const streamPreflightFailureResponse = (terminalType: ResponseStreamTerminalType, provider: UpstreamProvider, warnings: readonly string[] = []): Response => {
  if (terminalType === "cancelled") {
    return streamErrorResponse(499, "Request was cancelled.", "request_cancelled", provider, warnings, "server_error", null);
  }
  if (terminalType === "deadline") {
    return streamErrorResponse(
      504,
      "Upstream stream exceeded the gateway deadline before its first SSE event.",
      "gateway_timeout",
      provider,
      warnings,
      "server_error"
    );
  }
  return streamErrorResponse(502, "Codex upstream stream ended unexpectedly.", "codex_upstream_stream_error", provider, warnings);
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

type ResponsesAttemptResult = { kind: "ready"; attempt: PreparedResponsesAttempt } | { kind: "failed"; attempt: FailedResponsesAttempt };

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

const failureKindForResponsesAttemptTrigger = (trigger: ResponsesAttemptTrigger): ResponsesStreamFailureKind | null => {
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

const safeFailedAttemptResponse = (response: Response, provider: UpstreamProvider, trigger: ResponsesAttemptTrigger, warnings: readonly string[]): Response => {
  if (!response.ok) return response;
  if (trigger === "empty_upstream_completion") {
    return streamErrorResponse(502, "The upstream completed without visible output.", "empty_upstream_completion", provider, warnings, "server_error", null);
  }
  if (trigger === "semantic_timeout") {
    return streamErrorResponse(
      504,
      "Upstream stream exceeded the gateway deadline before semantic output.",
      "gateway_timeout",
      provider,
      warnings,
      "server_error"
    );
  }
  if (trigger === "missing_body") {
    return streamErrorResponse(502, "Upstream response missing body.", "server_error", provider, warnings);
  }
  return streamErrorResponse(502, "Upstream Responses stream ended unexpectedly.", "server_error", provider, warnings);
};

const responsesAttemptTriggerFor = (semantic: ResponsesStreamEvent | null, fallback: ResponsesAttemptTrigger): ResponsesAttemptTrigger =>
  semantic ? "terminal_failure" : fallback;

const responsesTerminalRejectionTrigger = (
  presemanticRejection: boolean | undefined,
  semantic: ResponsesStreamEvent | null,
  terminal: ResponsesStreamEvent
): ResponsesAttemptTrigger =>
  presemanticRejection && semantic === null && (terminal.type === "response.failed" || terminal.type === "error") ? "terminal_failure" : "read_error";

const applyResponsesEventIdentity = (
  event: ResponsesStreamEvent,
  selectedModel: string | null,
  taskType: string | null
): Readonly<{ selectedModel: string | null; taskType: string | null; modelConflict: boolean }> => {
  let resolvedModel = selectedModel;
  let resolvedTaskType = taskType;
  const candidate = removedProviderModelFromEvent(event.value);
  if (candidate) {
    if (resolvedModel && resolvedModel !== candidate) return { selectedModel: resolvedModel, taskType: resolvedTaskType, modelConflict: true };
    resolvedModel = candidate;
  }
  if (!resolvedTaskType && isRecord(event.value.response)) {
    resolvedTaskType = removedProviderTaskTypeFromResponse(event.value.response);
  }
  return { selectedModel: resolvedModel, taskType: resolvedTaskType, modelConflict: false };
};

const resolveBufferedResponsesIdentity = async (
  iterator: ResponsesStreamIterator,
  prepared: PreparedResponsesStream
): Promise<Readonly<{ selectedModel: string | null; taskType: string | null; trigger: ResponsesAttemptTrigger | null }>> => {
  let selectedModel: string | null = null;
  let taskType: string | null = null;
  for (const event of prepared.buffered) {
    const identity = applyResponsesEventIdentity(event, selectedModel, taskType);
    if (identity.modelConflict) {
      await iterator.return("inconsistent model identity").catch(() => {});
      return { selectedModel, taskType, trigger: responsesAttemptTriggerFor(prepared.semantic, "invalid_model") };
    }
    selectedModel = identity.selectedModel;
    taskType = identity.taskType;
  }
  return { selectedModel, taskType, trigger: null };
};

const extendResponsesIdentityFromStream = async (
  iterator: ResponsesStreamIterator,
  prepared: PreparedResponsesStream,
  options: Readonly<{ usageContext?: UsageContext; requireEligibleModel?: boolean }>,
  buffered: Readonly<{ selectedModel: string | null; taskType: string | null }>
): Promise<
  | Readonly<{ kind: "failed"; trigger: ResponsesAttemptTrigger }>
  | Readonly<{
      kind: "discovered";
      responseId: string | null;
      bufferedChars: number;
      terminal: ResponsesStreamEvent | null;
      selectedModel: string | null;
      taskType: string | null;
    }>
> => {
  let responseId = responseIdFromEvents(prepared.buffered);
  let bufferedChars = prepared.bufferedChars;
  let discoveredTerminal = prepared.terminal;
  let selectedModel = buffered.selectedModel;
  let taskType = buffered.taskType;
  while (options.requireEligibleModel && (!selectedModel || !responseId) && !discoveredTerminal) {
    const next = await iterator.next();
    if (next.done) break;
    recordResponsesEventTelemetry(options.usageContext, next.value);
    bufferedChars = appendResponsesPrecommitEvent(prepared.buffered, next.value, bufferedChars);
    const candidateResponseId = responseIdFromEvents([next.value]);
    if (candidateResponseId && responseId && candidateResponseId !== responseId) {
      await iterator.return("inconsistent response identity").catch(() => {});
      return { kind: "failed", trigger: responsesAttemptTriggerFor(prepared.semantic, "malformed_event") };
    }
    responseId ??= candidateResponseId;
    const identity = applyResponsesEventIdentity(next.value, selectedModel, taskType);
    if (identity.modelConflict) {
      await iterator.return("inconsistent model identity").catch(() => {});
      return { kind: "failed", trigger: responsesAttemptTriggerFor(prepared.semantic, "invalid_model") };
    }
    selectedModel = identity.selectedModel;
    taskType = identity.taskType;
    if (next.value.terminal) discoveredTerminal = next.value;
  }
  return { kind: "discovered", responseId, bufferedChars, terminal: discoveredTerminal, selectedModel, taskType };
};

const rejectFailedResponsesDiscovery = async (
  iterator: ResponsesStreamIterator,
  prepared: PreparedResponsesStream,
  deadline: StreamDeadline,
  options: Readonly<{ requireEligibleModel?: boolean; rejectFailedTerminal?: boolean }>,
  discovered: Readonly<{ responseId: string | null; selectedModel: string | null; terminal: ResponsesStreamEvent | null }>
): Promise<ResponsesAttemptTrigger | null> => {
  if (
    options.rejectFailedTerminal &&
    discovered.terminal &&
    (discovered.terminal.type === "response.failed" || discovered.terminal.type === "error") &&
    prepared.semantic === null
  ) {
    await iterator.return("failed terminal before release").catch(() => {});
    return "read_error";
  }
  if (options.requireEligibleModel && !prepared.buffered.some((event) => event.type === "response.created")) {
    await iterator.return("missing response.created").catch(() => {});
    return responsesAttemptTriggerFor(prepared.semantic, "malformed_event");
  }
  if (options.requireEligibleModel && !discovered.responseId) {
    await iterator.return("missing response id").catch(() => {});
    return responsesAttemptTriggerFor(prepared.semantic, "malformed_event");
  }
  deadline.clear();
  if (options.requireEligibleModel && (!discovered.selectedModel || !isEligibleRemovedProviderModel(discovered.selectedModel))) {
    await iterator.return("invalid selected model").catch(() => {});
    return responsesAttemptTriggerFor(prepared.semantic, "invalid_model");
  }
  return null;
};

const responsesStreamTerminalFailure = async (
  iterator: ResponsesStreamIterator,
  prepared: PreparedResponsesStream,
  deadline: StreamDeadline,
  options: Readonly<{ rejectFailedTerminal?: boolean; rejectPresemanticFailureTerminal?: boolean }>
): Promise<Readonly<{ trigger: ResponsesAttemptTrigger; terminal: ResponsesStreamEvent | null }> | null> => {
  if (prepared.terminal?.type === "response.completed" && prepared.semantic === null) {
    await iterator.return("empty upstream completion").catch(() => {});
    return { trigger: "empty_upstream_completion", terminal: prepared.terminal };
  }
  if (
    prepared.terminal &&
    ((options.rejectFailedTerminal && (prepared.terminal.type === "response.failed" || prepared.terminal.type === "error") && prepared.semantic === null) ||
      (options.rejectPresemanticFailureTerminal &&
        prepared.semantic === null &&
        (prepared.terminal.type === "response.failed" || prepared.terminal.type === "error")))
  ) {
    deadline.clear();
    return {
      trigger: responsesTerminalRejectionTrigger(options.rejectPresemanticFailureTerminal, prepared.semantic, prepared.terminal),
      terminal: null,
    };
  }
  return null;
};

const finalizePreparedResponsesAttempt = (
  prepared: PreparedResponsesStream,
  iterator: ResponsesStreamIterator,
  discovered: Readonly<{
    responseId: string | null;
    bufferedChars: number;
    terminal: ResponsesStreamEvent | null;
    selectedModel: string | null;
    taskType: string | null;
  }>,
  provider: UpstreamProvider,
  response: Response,
  deadline: StreamDeadline,
  options: Readonly<{ requireEligibleModel?: boolean }>
): ResponsesAttemptResult => {
  const sanitizedBuffered = options.requireEligibleModel
    ? prepared.buffered.map((event) => {
        const value = stripRemovedProviderMetadata(event.value);
        return value === event.value ? event : responseEventFromValue(value);
      })
    : prepared.buffered;
  const sanitizedTerminal = discovered.terminal ? (sanitizedBuffered[prepared.buffered.indexOf(discovered.terminal)] ?? discovered.terminal) : null;
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
        bufferedChars: discovered.bufferedChars,
        terminal: sanitizedTerminal,
      },
      responseId: discovered.responseId,
      selectedModel: discovered.selectedModel,
      taskType: discovered.taskType,
      signal: deadline.signal,
      abort: deadline.abort,
      clearDeadline: deadline.clear,
    },
  };
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
  }> = {}
): Promise<ResponsesAttemptResult> => {
  const fail = (trigger: ResponsesAttemptTrigger, failedResponse = response, terminal: ResponsesStreamEvent | null = null): ResponsesAttemptResult => {
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
    const terminalFailure = await responsesStreamTerminalFailure(iterator, prepared, deadline, options);
    if (terminalFailure) return fail(terminalFailure.trigger, response, terminalFailure.terminal);
    const buffered = await resolveBufferedResponsesIdentity(iterator, prepared);
    if (buffered.trigger) return fail(buffered.trigger);
    const discovered = await extendResponsesIdentityFromStream(iterator, prepared, options, buffered);
    if (discovered.kind === "failed") return fail(discovered.trigger);
    const rejectionTrigger = await rejectFailedResponsesDiscovery(iterator, prepared, deadline, options, discovered);
    if (rejectionTrigger) return fail(rejectionTrigger);
    return finalizePreparedResponsesAttempt(prepared, iterator, discovered, provider, response, deadline, options);
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

const responseFailureTerminalType = (trigger: ResponsesAttemptTrigger, signal: AbortSignal, downstreamSignal: AbortSignal): ResponseStreamTerminalType => {
  if (trigger === "semantic_timeout" || isTimeoutFailure(signal.reason, downstreamSignal.reason)) return "deadline";
  if (downstreamSignal.aborted) return "cancelled";
  if (signal.aborted) return "deadline";
  if (trigger === "premature_eof") return "eof";
  if (trigger === "terminal_failure" || trigger === "empty_upstream_completion") return "response.failed";
  return "error";
};

type PrimaryResponsesOptions = Readonly<{
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
}>;

const isRetryablePrimaryFetchFailure = (error: unknown): error is CodexError =>
  error instanceof CodexError && (error.code === "gateway_timeout" || error.code === "codex_upstream_unreachable");

const primaryResponsesAttemptTrigger = (status: number): ResponsesAttemptTrigger => {
  if (status === 504) return "semantic_timeout";
  if (status >= 500) return "http_5xx";
  return "read_error";
};

const failedPrimaryResponsesFetchOutcome = (error: CodexError, deadline: StreamDeadline): { kind: "failed"; value: ResponsesRouteFailure } => {
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
};

const failedPrimaryResponsesGatewayOutcome = (
  routed: RoutedResponsesUpstream,
  lifecycle: MeteredTransportLifecycle,
  preparationDeadline: StreamDeadline
): { kind: "failed"; value: ResponsesRouteFailure } => ({
  kind: "failed",
  value: {
    routed,
    lifecycle,
    failed: {
      provider: routed.provider,
      response: routed.response,
      trigger: primaryResponsesAttemptTrigger(routed.response.status),
      signal: preparationDeadline.signal,
      clearDeadline: preparationDeadline.clear,
    },
  },
});

const preparePrimaryResponsesAttempt = async (
  routed: RoutedResponsesUpstream,
  preparationDeadline: StreamDeadline,
  lifecycle: MeteredTransportLifecycle,
  options: PrimaryResponsesOptions
): Promise<ResponsesAttemptResult> => {
  try {
    return await prepareResponsesAttempt(
      routed.response,
      routed.provider,
      preparationDeadline,
      options.requestSignal,
      [...options.warnings, ...responseWarnings(routed.response)],
      {
        usageContext: options.usageContext,
        rejectPresemanticFailureTerminal: options.rejectPresemanticFailureTerminal,
        releaseOnProgress: options.releaseOnProgress === true && supportsReasoningProgressRelease(routed.provider),
      }
    );
  } catch (error) {
    if (options.requestSignal.aborted) {
      await finalizeAbandonedPrimaryAttempt(routed, lifecycle, {
        cancelled: classifyPreHeaderFailure(error, options.requestSignal, options.downstreamSignal) === "cancelled",
      });
    }
    throw error;
  }
};

const fetchAndPreparePrimaryResponses = async (
  body: Record<string, unknown>,
  options: PrimaryResponsesOptions
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
    if (!isRetryablePrimaryFetchFailure(error)) throw error;
    return failedPrimaryResponsesFetchOutcome(error, deadline);
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
    routed.providerHealthOnly === true
  );
  if (routed.gatewayResponse) {
    preparationDeadline.clear();
    return failedPrimaryResponsesGatewayOutcome(routed, lifecycle, preparationDeadline);
  }
  const prepared = await preparePrimaryResponsesAttempt(routed, preparationDeadline, lifecycle, options);
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
  }>
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
        onDispatch: () => {
          recordFirstProviderDispatch(options.usageContext);
        },
        onHeaders: () => {
          recordFirstProviderHeaders(options.usageContext);
        },
      },
      beforeDispatch: () => options.usageContext?.beforeProviderDispatch?.("removed_provider") ?? Promise.resolve(undefined),
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
        response: streamErrorResponse(502, "RemovedProvider request failed before response headers were received.", "server_error", "removed_provider", []),
        trigger: triggerForResponsesError(error, deadline.signal),
        signal: deadline.signal,
        clearDeadline: deadline.clear,
      },
    };
  }
  return await prepareResponsesAttempt(response, "removed_provider", deadline, options.requestSignal, [], {
    usageContext: options.usageContext,
    requireEligibleModel: true,
    rejectFailedTerminal: true,
  });
};

const finalizeAbandonedPrimaryAttempt = async (
  routed: RoutedResponsesUpstream,
  lifecycle: MeteredTransportLifecycle,
  options: Readonly<{
    cancelled?: boolean;
    failureTrigger?: ResponsesAttemptTrigger;
  }> = {}
): Promise<void> => {
  if (routed.provider === "chatgpt_codex") {
    const transition = routed.response.ok && !options.cancelled ? markCodexResponseUpstreamError(routed.response) : releaseCodexResponseProbe(routed.response);
    await transition.catch(() => {});
  } else if ((routed.provider === "metered" || routed.provider === "surplus") && !routed.gatewayResponse) {
    if (options.cancelled) lifecycle.cancelled();
    else if (options.failureTrigger === "http_5xx" || options.failureTrigger === "terminal_failure" || options.failureTrigger === "empty_upstream_completion") {
      lifecycle.terminal("response.failed");
    } else lifecycle.ambiguous();
  }
};

const selectPrimarySemanticRecoveryTransition = (
  routed: RoutedResponsesUpstream,
  circuitProbe: RemovedProviderCircuitProbe,
  terminalType?: string | null
): Promise<"none"> => {
  if (routed.provider !== "chatgpt_codex") return releaseGlobalRemovedProviderProbe(circuitProbe);
  if (terminalType === "response.failed") return recordRemovedProviderEligibleFailure(circuitProbe);
  return closeRemovedProviderCircuit(circuitProbe);
};

const markPrimarySemanticRecovery = (
  routed: RoutedResponsesUpstream,
  circuitProbe: RemovedProviderCircuitProbe | null,
  usageContext?: UsageContext,
  terminalType?: string | null
): void => {
  if (!circuitProbe) return;
  const transition = selectPrimarySemanticRecoveryTransition(routed, circuitProbe, terminalType);
  void transition
    .then((value) => {
      // The retired circuit module only ever yields "none" today, so the
      // transition is recorded through the same string-typed helper the
      // failover path uses. The guard still evaluates the real value.
      recordRemovedProviderCircuitTransition(usageContext, value);
    })
    .catch(() => {});
};

type BufferedResponsesOptions = Readonly<{
  warningModel?: string | null;
  usageContext?: UsageContext;
  onTerminal?: (event: ResponsesStreamEvent) => void;
  onEvent?: (event: ResponsesStreamEvent) => void;
  validateEvent?: (event: ResponsesStreamEvent) => void;
  onFailure?: (error: unknown, details?: OwnedResponsesStreamFailureDetails) => Response | undefined;
}>;

type BufferedResponsesAccumulator = {
  responseId: string | null;
  refusalText: string;
  readonly deltaTextParts: Map<string, string>;
  readonly doneTextParts: Map<string, string>;
  readonly textPartOrder: string[];
  readonly outputItems: Record<string, unknown>[];
};

type BufferedResponsesTerminalOutcome = Readonly<{ kind: "error"; response: Response }> | Readonly<{ kind: "terminal"; response: Record<string, unknown> }>;

// Parsed upstream JSON can put any shape on an index field. Stringify primitives
// exactly as before, serialize objects explicitly, and otherwise fall back to the
// same default the absent case uses instead of rendering "[object Object]".
const formatTextPartIndex = (value: unknown): string => {
  if (value === null || value === undefined) return "0";
  if (typeof value === "string" || typeof value === "number" || typeof value === "boolean" || typeof value === "bigint" || typeof value === "symbol") {
    return String(value);
  }
  if (typeof value === "object") return JSON.stringify(value);
  return "0";
};

const textPartKeyFromValue = (value: Record<string, unknown>): string => {
  const itemId = getString(value.item_id)?.trim();
  if (itemId) return `item:${itemId}:${formatTextPartIndex(value.content_index)}`;
  return `output:${formatTextPartIndex(value.output_index)}:${formatTextPartIndex(value.content_index)}`;
};

const createBufferedResponsesAccumulator = (responseId: string | null): BufferedResponsesAccumulator => ({
  responseId,
  refusalText: "",
  deltaTextParts: new Map<string, string>(),
  doneTextParts: new Map<string, string>(),
  textPartOrder: [],
  outputItems: [],
});

const rememberBufferedTextPart = (accumulator: BufferedResponsesAccumulator, value: Record<string, unknown>, text: string, done: boolean): void => {
  if (!text) return;
  const key = textPartKeyFromValue(value);
  if (!accumulator.textPartOrder.includes(key)) accumulator.textPartOrder.push(key);
  if (done) {
    const deltaText = accumulator.deltaTextParts.get(key) ?? "";
    // A done event normally repeats the complete text accumulated by its
    // deltas. Some upstreams instead send a conflicting fragment; retain
    // the delta text in that case, matching the owned stream reconciler.
    if (!deltaText || text.startsWith(deltaText)) accumulator.doneTextParts.set(key, text);
    return;
  }
  accumulator.deltaTextParts.set(key, `${accumulator.deltaTextParts.get(key) ?? ""}${text}`);
};

const trackBufferedResponseId = (accumulator: BufferedResponsesAccumulator, event: ResponsesStreamEvent): void => {
  const eventResponseId = responseIdFromEvents([event]);
  if (eventResponseId && accumulator.responseId && eventResponseId !== accumulator.responseId) {
    throw new ResponsesStreamError("Upstream Responses stream changed response identifiers.", {
      kind: "malformed_event",
    });
  }
  accumulator.responseId ??= eventResponseId;
};

const accumulateBufferedResponsesEvent = (
  accumulator: BufferedResponsesAccumulator,
  event: ResponsesStreamEvent,
  warningModel: string | null | undefined
): void => {
  const value = event.value;
  const suppressedWarningModelOutput = Boolean(warningModel) && value.output_index === 0;
  if (event.type === "response.output_text.delta" && !suppressedWarningModelOutput) {
    rememberBufferedTextPart(accumulator, value, getString(value.delta) ?? "", false);
  }
  if (event.type === "response.output_text.done" && !suppressedWarningModelOutput) {
    rememberBufferedTextPart(accumulator, value, getString(value.text) ?? "", true);
  }
  if (event.type === "response.refusal.delta") accumulator.refusalText += getString(value.delta) ?? "";
  if (event.type === "response.refusal.done" && !accumulator.refusalText) accumulator.refusalText = getString(value.refusal) ?? "";
  if (event.type === "response.output_item.done" && isRecord(value.item)) accumulator.outputItems.push(value.item);
  if (event.type === "response.output") {
    const output = value.output ?? (isRecord(value.response) ? value.response.output : undefined);
    if (Array.isArray(output)) accumulator.outputItems.push(...output.filter(isRecord));
  }
};

const resolveBufferedResponsesTerminal = (
  event: ResponsesStreamEvent,
  provider: UpstreamProvider,
  onTerminal?: (event: ResponsesStreamEvent) => void
): BufferedResponsesTerminalOutcome | null => {
  if (event.type === "error") {
    onTerminal?.(event);
    const code = getString(event.value.code) ?? "server_error";
    const message = getString(event.value.message) ?? "Upstream Responses stream ended unexpectedly.";
    return { kind: "error", response: streamErrorResponse(502, message, code, provider, []) };
  }
  if (
    (event.type === "response.completed" || event.type === "response.failed" || event.type === "response.incomplete") &&
    isRecord(event.value.response) &&
    !Array.isArray(event.value.response)
  ) {
    onTerminal?.(event);
    return { kind: "terminal", response: event.value.response };
  }
  return null;
};

const createBufferedResponsesInitial = (
  attempt: Pick<PreparedResponsesAttempt, "responseId" | "prepared">,
  options: BufferedResponsesOptions
): AsyncIterable<ResponsesStreamEvent> => {
  const warningModel = options.warningModel;
  if (warningModel) {
    const stream = createOwnedResponsesStream({
      initial: attempt.prepared.buffered,
      iterator: attempt.prepared.iterator,
      responseId: attempt.responseId,
      warning: { model: warningModel },
      validateEvent: options.validateEvent,
      onEvent: (event) => {
        recordResponsesEventTelemetry(options.usageContext, event);
        options.onEvent?.(event);
      },
      onFailure: (error, details) => {
        options.onFailure?.(error, details);
      },
    });
    return readResponsesStream(stream);
  }
  return (async function* (): AsyncGenerator<ResponsesStreamEvent> {
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
};

export const collectBufferedResponses = async (
  attempt: Pick<PreparedResponsesAttempt, "provider" | "responseId" | "prepared">,
  options: BufferedResponsesOptions = {}
): Promise<Response> => {
  const initial = createBufferedResponsesInitial(attempt, options);
  const accumulator = createBufferedResponsesAccumulator(attempt.responseId);
  let finalResponse: Record<string, unknown> | null = null;
  try {
    for await (const event of initial) {
      trackBufferedResponseId(accumulator, event);
      accumulateBufferedResponsesEvent(accumulator, event, options.warningModel);
      const terminal = resolveBufferedResponsesTerminal(event, attempt.provider, options.onTerminal);
      if (!terminal) continue;
      if (terminal.kind === "error") return terminal.response;
      finalResponse = terminal.response;
      break;
    }
  } catch (error) {
    const failureResponse = options.onFailure?.(error);
    if (failureResponse) return failureResponse;
    return streamErrorResponse(502, "Upstream Responses stream ended unexpectedly.", "server_error", attempt.provider, []);
  }
  if (!finalResponse) {
    return streamErrorResponse(502, "Upstream Responses stream ended unexpectedly.", "server_error", attempt.provider, []);
  }
  const outputText = accumulator.textPartOrder.map((key) => accumulator.doneTextParts.get(key) ?? accumulator.deltaTextParts.get(key) ?? "").join("");
  finalResponse = withAccumulatedResponseItems(finalResponse, accumulator.outputItems);
  finalResponse = withAccumulatedResponseText(finalResponse, outputText, options.warningModel ? 1 : 0);
  finalResponse = withAccumulatedResponseRefusal(finalResponse, accumulator.refusalText, options.warningModel ? 1 : 0);
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
  error_class: "ApiKeyQuotaDispatchError" | "CodexError" | "MeteredError" | "SurplusError" | "DOMException" | "TypeError" | "Error" | "unknown";
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
  const requestId = response.headers.get("X-Request-Id") ?? response.headers.get("X-Api-Request-Id") ?? response.headers.get("X-Oneapi-Request-Id");
  return normalizeProviderRequestId(requestId);
};

const paidUpstreamProviderLabel = (provider: string | null | undefined): string => {
  if (provider === "surplus") return "Surplus";
  if (provider === "metered") return "Metered";
  return "Codex";
};

const codexUpstreamErrorResponse = (error: CodexError): Response => {
  const authReauthenticationFailure =
    error.code === "codex_auth_invalid" || error.code === "codex_auth_refresh_failed" || error.code === "refresh_token_reused";
  const options = {
    ...(error.code === "gateway_timeout" || error.code === "codex_auth_refresh_failed" || error.code === "refresh_token_reused"
      ? { type: "server_error" }
      : {}),
    ...(authReauthenticationFailure ? { headers: { "x-uos-warning": CODEX_AUTH_REAUTH_WARNING } } : {}),
  };
  return openaiError(
    error.code === "codex_auth_refresh_failed" || error.code === "refresh_token_reused" ? 503 : error.status,
    error.message,
    error.code,
    options
  );
};

const unreachableUpstreamErrorResponse = (error: unknown, provider?: string | null): Response => {
  const detail = formatErrorSnippet(error);
  const paidProvider = provider === "metered" || provider === "surplus" ? provider : null;
  const providerLabel = paidUpstreamProviderLabel(paidProvider);
  const message = detail ? `${providerLabel} upstream request failed: ${detail}` : `${providerLabel} upstream request failed.`;
  if (paidProvider) {
    return openaiError(502, message, `${paidProvider}_upstream_unreachable`, {
      type: "server_error",
      param: null,
    });
  }
  return openaiError(502, message, "codex_upstream_unreachable");
};

const toCodexErrorResponse = (error: unknown, provider?: string | null): Response => {
  let response: Response;
  if (error instanceof ApiKeyQuotaDispatchError) {
    response = openaiError(error.status, error.message, error.code, {
      type: error.errorType,
      headers: error.headers,
    });
  } else if (error instanceof CodexError) {
    response = codexUpstreamErrorResponse(error);
  } else {
    response = unreachableUpstreamErrorResponse(error, provider);
  }
  return withUpstreamProviderHeader(response, provider);
};

const toPreHeaderErrorResponse = (error: unknown, terminalType: ResponseStreamTerminalType, provider?: string | null): Response => {
  if (terminalType === "cancelled") {
    return withUpstreamProviderHeader(
      openaiError(499, "Request was cancelled.", "request_cancelled", {
        type: "server_error",
        param: null,
      }),
      provider
    );
  }
  if (terminalType === "deadline" && !(error instanceof CodexError && error.code === "gateway_timeout")) {
    return withUpstreamProviderHeader(
      openaiError(504, "Upstream request exceeded the gateway deadline.", "gateway_timeout", {
        type: "server_error",
        param: null,
      }),
      provider
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

const cerebrasResponseHeaders = (providerRequestId: string | null, warning?: string): Record<string, string> => ({
  "x-uos-upstream": "cerebras",
  ...(providerRequestId ? { "x-uos-provider-request-id": providerRequestId } : {}),
  ...(warning ? { "x-uos-warning": warning } : {}),
});

const GPT_OSS_STREAM_DOWNGRADED_WARNING = "gpt_oss_stream_downgraded";

const cerebrasChatCompletionHasSemanticOutput = (completion: Record<string, unknown>): boolean =>
  Array.isArray(completion.choices) &&
  completion.choices.some((choice) => {
    if (!isRecord(choice) || Array.isArray(choice) || !isRecord(choice.message) || Array.isArray(choice.message)) {
      return false;
    }
    const message = choice.message;
    return (
      (typeof message.content === "string" && message.content.length > 0) ||
      (typeof message.refusal === "string" && message.refusal.length > 0) ||
      (Array.isArray(message.tool_calls) && message.tool_calls.length > 0)
    );
  });

const cerebrasChatToolCallDeltas = (toolCalls: readonly unknown[]): Record<string, unknown>[] =>
  toolCalls.flatMap((toolCall, toolCallIndex) => {
    if (!isRecord(toolCall) || Array.isArray(toolCall)) return [];
    const fn = isRecord(toolCall.function) && !Array.isArray(toolCall.function) ? toolCall.function : null;
    if (!fn) return [];
    return [
      {
        index: toolCallIndex,
        id: toolCall.id,
        type: "function",
        function: {
          name: fn.name,
          arguments: fn.arguments,
        },
      },
    ];
  });

const cerebrasChatDelta = (message: Record<string, unknown>): Record<string, unknown> => {
  const delta: Record<string, unknown> = { role: "assistant" };
  // Native Cerebras stream deltas carry reasoning in the leading chunk;
  // mirror that 1:1 (compliance D1) instead of dropping it.
  if (typeof message.reasoning === "string" && message.reasoning) delta.reasoning = message.reasoning;
  if (typeof message.content === "string") delta.content = message.content;
  if (typeof message.refusal === "string" && message.refusal) delta.refusal = message.refusal;

  if (Array.isArray(message.tool_calls)) {
    delta.tool_calls = cerebrasChatToolCallDeltas(message.tool_calls);
  }

  return delta;
};

const streamCerebrasChatCompletion = (completion: Record<string, unknown>, includeUsage: boolean, headers: HeadersInit): Response => {
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
    const delta = cerebrasChatDelta(message);

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

const readUpstreamErrorBody = async (upstream: Response, signal: AbortSignal): Promise<Readonly<{ text: string; complete: boolean }>> => {
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
  // Absent and blank values both mean "no string", so the falsy check stays.
  const trimmed = stringValue?.trim();
  if (trimmed === undefined || trimmed === "") return null;
  return trimmed;
};

const upstreamErrorDetailsFromParsedJson = (parsed: unknown): UpstreamErrorDetails | null => {
  if (!isRecord(parsed)) return null;
  const error = isRecord(parsed.error) ? parsed.error : null;
  const message = getJsonString(error, "message") ?? getJsonString(parsed, "detail") ?? getJsonString(parsed, "message");
  if (!message) return null;
  const details: UpstreamErrorDetails = {
    message,
    type: getJsonString(error, "type") ?? getJsonString(parsed, "type") ?? undefined,
    code: getJsonString(error, "code") ?? getJsonString(parsed, "code") ?? undefined,
  };
  return error && Object.prototype.hasOwnProperty.call(error, "param") ? { ...details, param: getString(error.param) ?? null } : details;
};

const parseUpstreamErrorDetails = (text: string, statusText: string): UpstreamErrorDetails => {
  const trimmed = text.trim();
  if (trimmed) {
    try {
      const parsed = JSON.parse(trimmed) as unknown;
      const details = upstreamErrorDetailsFromParsedJson(parsed);
      if (details) return details;
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

const toOpenAiUpstreamErrorResponse = async (upstream: Response, provider: UpstreamProvider, signal: AbortSignal): Promise<Response> => {
  const captured = await readUpstreamErrorBody(upstream, signal);
  const details = captured.complete
    ? parseUpstreamErrorDetails(captured.text, upstream.statusText)
    : { message: "Upstream returned an oversized or incomplete error response." };
  const headers: Record<string, string> = { "x-uos-upstream": provider };
  const warning = upstream.headers.get("x-uos-warning");
  const hasAuthWarning =
    warning
      ?.split(",")
      .map((value) => value.trim())
      .includes(CODEX_AUTH_REAUTH_WARNING) === true;
  if (warning) headers["x-uos-warning"] = warning;
  const retryAfter = upstream.headers.get("Retry-After");
  if (retryAfter) headers["Retry-After"] = retryAfter;
  const message = hasAuthWarning && !captured.text.includes(CODEX_AUTH_REAUTH_MESSAGE) ? `${details.message} ${CODEX_AUTH_REAUTH_MESSAGE}` : details.message;
  const options: { type?: string; param?: string | null; headers: HeadersInit } = {
    type: hasAuthWarning && upstream.status >= 500 ? "server_error" : upstreamStatusToErrorType(upstream.status, details.type),
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

const parseCerebrasUpstreamErrorDetail = (body: unknown): { message?: string; code?: string } => {
  if (!isRecord(body) || Array.isArray(body)) return {};
  const error = isRecord(body.error) && !Array.isArray(body.error) ? body.error : null;
  const pickString = (value: unknown, max: number): string | undefined => {
    if (typeof value !== "string") return undefined;
    const trimmed = value.slice(0, max).trim();
    return trimmed ? trimmed : undefined;
  };
  return {
    message: pickString(error?.message ?? body.message, CEREBRAS_UPSTREAM_ERROR_MESSAGE_MAX),
    code: pickString(error?.code ?? body.code, CEREBRAS_UPSTREAM_ERROR_CODE_MAX),
  };
};

const toCerebrasUpstreamErrorResponse = async (upstream: Response, signal?: AbortSignal): Promise<Response> => {
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
        detail = parseCerebrasUpstreamErrorDetail(JSON.parse(new TextDecoder().decode(captured.bytes)) as unknown);
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
  return openaiError(upstream.status, detail.message ?? "Cerebras upstream returned an error.", detail.code ?? "cerebras_upstream_error", {
    type: upstream.status === 408 ? "server_error" : upstreamStatusToErrorType(upstream.status),
    headers,
  });
};

const warnPaidFallbackBookkeepingFailure = (operation: string, error: unknown): void => {
  console.warn(`[ai.ubq.fi] Paid fallback ${operation} failed; leaving the reservation pending:`, error instanceof Error ? error.message : String(error));
};

const logPaidProviderSelected = (requestId: string, reason: InferenceFallbackReason, provider: "metered" | "surplus"): void => {
  try {
    if (provider === "metered") {
      // Keep the established Metered event shape for existing log consumers.
      console.info("[ai.ubq.fi] metered_selected", JSON.stringify({ request_id: requestId, reason }));
    } else {
      console.info("[ai.ubq.fi] paid_provider_selected", JSON.stringify({ request_id: requestId, reason, provider }));
    }
  } catch {
    // Routing telemetry must never alter provider selection.
  }
};

const logPaidProviderAdmissionRejected = (requestId: string, model: string, reason: string): void => {
  try {
    console.warn("[ai.ubq.fi] paid_provider_admission_rejected", JSON.stringify({ request_id: requestId, model, reason }));
  } catch {
    // Routing telemetry must never alter provider selection.
  }
};

const paidProviderAdmissionError = (reason: string): Response => {
  switch (reason) {
    case "disabled":
      return openaiError(403, "Paid-provider routing is disabled for this API key.", "paid_fallback_disabled");
    case "limit_exceeded":
      return openaiError(429, "The API key's paid-provider limit is exhausted.", "paid_fallback_limit_exceeded");
    case "provider_unconfigured":
      return openaiError(503, "No paid provider is configured for this model.", "paid_provider_unconfigured", { type: "server_error" });
    case "model_not_priced":
      return openaiError(403, "This model is not admitted by the API key's paid-provider policy.", "paid_model_not_admitted");
    case "reconciliation_pending":
      return openaiError(503, "Paid-provider billing reconciliation is pending.", "paid_fallback_reconciliation_pending", { type: "server_error" });
    case "concurrent_update":
      return openaiError(503, "Paid-provider admission changed concurrently; retry the request.", "paid_fallback_concurrent_update", { type: "server_error" });
    default:
      return openaiError(503, "Paid-provider admission is unavailable.", "paid_fallback_invalid_policy", { type: "server_error" });
  }
};

const canAttemptPaidFallback = (context: UsageContext | undefined): boolean =>
  context?.paidFallbackEnabled === true &&
  Boolean(context.keyId && context.requestId && context.startedAtMs !== undefined) &&
  (Boolean(readSurplusApiKey()) || Boolean(readMeteredApiKey()));

const bestEffortPaidFallbackBookkeeping = async (operation: string, run: () => Promise<unknown>): Promise<void> => {
  try {
    await run();
  } catch (error) {
    warnPaidFallbackBookkeepingFailure(operation, error);
  }
};

const paidProviderErrorStatus = (error: unknown): number | null => (error instanceof MeteredError || error instanceof SurplusError ? error.status : null);

const recordPaidProviderResponseHealth = async (
  provider: "metered" | "surplus",
  status: number | null,
  providerRequestId: string | null = null
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

const meteredTransportTerminalState = (eventType: string): "completed" | "failed" | "incomplete" | null => {
  if (eventType === "response.completed") return "completed";
  if (eventType === "response.failed" || eventType === "error") return "failed";
  if (eventType === "response.incomplete") return "incomplete";
  return null;
};

const paidLedgerProviderLabel = (provider: UpstreamProvider): "metered" | "surplus" => (provider === "surplus" ? "surplus" : "metered");

const recordProviderHealthForProvider = (
  provider: UpstreamProvider,
  event: "success" | "upstream_error",
  status: number | null,
  providerRequestId: string | null
): Promise<void> =>
  provider === "surplus"
    ? recordSurplusProviderHealth(event, status, Date.now, providerRequestId)
    : recordMeteredProviderHealth(event, status, Date.now, providerRequestId);

const reconcileMeteredTransportTerminal = async (
  reservation: PaidFallbackReservation,
  terminalState: "completed" | "failed" | "incomplete",
  provider: UpstreamProvider,
  providerRequestId: string | null,
  surplusBilling: SurplusBillingPricing | null,
  model: string | null,
  usage: UsageTokens | null
): Promise<void> => {
  const terminal = recordMeteredTerminal(reservation, terminalState, paidLedgerProviderLabel(provider));
  const surplusSettlement =
    provider === "surplus" && surplusBilling && model && usage
      ? async (): Promise<void> => {
          await terminal;
          await recordSurplusUsage(
            reservation,
            providerRequestId ?? `surplus:${reservation.request_id}`,
            model,
            {
              input_tokens: usage.inputTokens,
              cached_input_tokens: usage.cachedInputTokens,
              cache_write_input_tokens: usage.cacheWriteInputTokens,
              output_tokens: usage.outputTokens,
            },
            surplusBilling
          );
        }
      : () => terminal;
  const healthEvent = terminalState === "completed" ? "success" : "upstream_error";
  const healthStatus = terminalState === "completed" ? 200 : null;
  await Promise.all([surplusSettlement(), recordProviderHealthForProvider(provider, healthEvent, healthStatus, providerRequestId)]);
};

const recordMeteredTransportAmbiguity = async (
  reservation: PaidFallbackReservation,
  provider: UpstreamProvider,
  providerRequestId: string | null
): Promise<void> => {
  await Promise.all([
    recordMeteredAmbiguousFailure(reservation, paidLedgerProviderLabel(provider), providerRequestId),
    recordProviderHealthForProvider(provider, "upstream_error", null, providerRequestId),
  ]);
};

const createMeteredTransportLifecycle = (
  reservation: PaidFallbackReservation | null,
  provider: UpstreamProvider = "metered",
  providerRequestId: string | null = null,
  surplusBilling: SurplusBillingPricing | null = null,
  model: string | null = null,
  providerHealthOnly = false
): MeteredTransportLifecycle => {
  let recorded = false;
  const schedule = (operation: string, run: (reservation: PaidFallbackReservation) => Promise<void>): void => {
    if (!reservation || recorded) return;
    recorded = true;
    void bestEffortPaidFallbackBookkeeping(operation, () => run(reservation));
  };
  const scheduleProviderHealthOnly = (event: "success" | "upstream_error", status: number | null): void => {
    if (reservation || !providerHealthOnly || recorded) return;
    if (provider !== "metered" && provider !== "surplus") return;
    recorded = true;
    void bestEffortPaidFallbackBookkeeping("unreserved provider health recording", () =>
      recordProviderHealthForProvider(provider, event, status, providerRequestId)
    );
  };
  return {
    terminal: (eventType, usage = null) => {
      const terminalState = meteredTransportTerminalState(eventType);
      if (!terminalState) return;
      if (!reservation) {
        const completed = terminalState === "completed";
        scheduleProviderHealthOnly(completed ? "success" : "upstream_error", completed ? 200 : null);
        return;
      }
      schedule("terminal reconciliation", (activeReservation) =>
        reconcileMeteredTransportTerminal(activeReservation, terminalState, provider, providerRequestId, surplusBilling, model, usage)
      );
    },
    ambiguous: () => {
      if (!reservation) {
        scheduleProviderHealthOnly("upstream_error", null);
        return;
      }
      schedule("ambiguous failure recording", (activeReservation) => recordMeteredTransportAmbiguity(activeReservation, provider, providerRequestId));
    },
    cancelled: () => {
      if (!reservation && providerHealthOnly) {
        // A client cancellation is not provider degradation. Finalize this
        // lifecycle so a later stream race cannot replace the header result.
        recorded = true;
        return;
      }
      schedule("dispatched cancellation recording", (activeReservation) =>
        recordMeteredTerminal(activeReservation, "cancelled", paidLedgerProviderLabel(provider))
      );
    },
  };
};

const paidCatalogNeedsRefresh = (catalog: Readonly<{ updated_at_ms: number }> | null, ttlMs: number): boolean =>
  catalog === null || Date.now() - catalog.updated_at_ms >= ttlMs;

const refreshPaidCatalogs = async (
  meteredCatalog: Awaited<ReturnType<typeof fetchMeteredModels>>,
  surplusCatalog: Awaited<ReturnType<typeof fetchSurplusModels>>,
  signal?: AbortSignal
): Promise<readonly [Awaited<ReturnType<typeof fetchMeteredModels>>, Awaited<ReturnType<typeof fetchSurplusModels>>]> =>
  await Promise.all([
    paidCatalogNeedsRefresh(meteredCatalog, METERED_MODELS_CACHE_TTL_MS) ? fetchMeteredModels({ signal }) : Promise.resolve(meteredCatalog),
    paidCatalogNeedsRefresh(surplusCatalog, SURPLUS_MODELS_CACHE_TTL_MS) ? fetchSurplusModels({ signal }) : Promise.resolve(surplusCatalog),
  ]);

const refreshStalePaidCatalogsInBackground = (
  meteredCatalog: Awaited<ReturnType<typeof fetchMeteredModels>>,
  surplusCatalog: Awaited<ReturnType<typeof fetchSurplusModels>>
): void => {
  if (meteredCatalog !== null && paidCatalogNeedsRefresh(meteredCatalog, METERED_MODELS_CACHE_TTL_MS)) {
    void fetchMeteredModels().catch(() => {});
  }
  if (surplusCatalog !== null && paidCatalogNeedsRefresh(surplusCatalog, SURPLUS_MODELS_CACHE_TTL_MS)) {
    void fetchSurplusModels().catch(() => {});
  }
};

const resolvePaidRoutingState = (
  input: Readonly<{
    meteredCatalog: Awaited<ReturnType<typeof fetchMeteredModels>>;
    surplusCatalog: Awaited<ReturnType<typeof fetchSurplusModels>>;
    codexModelKnown: boolean;
    endpointType: string;
    requestUsesTools: boolean;
    model: string;
  }>
): Readonly<{
  surplusBilling: SurplusBillingPricing | null;
  paidProviders: readonly ("metered" | "surplus")[];
  paidModelKnown: boolean;
  meteredOnly: boolean;
}> => {
  const meteredModelSupportsRoute =
    input.meteredCatalog?.models.some((entry) => entry.id === input.model && entry.supported_endpoint_types.includes(input.endpointType)) === true;
  const surplusModelSupportsRoute =
    input.surplusCatalog?.models.some((entry) => entry.id === input.model && entry.supported_endpoint_types.includes(input.endpointType)) === true;
  const surplusModel = input.surplusCatalog?.models.find((entry) => entry.id === input.model) ?? null;
  const surplusInputPrice = surplusModel?.input_price_per_token;
  const surplusOutputPrice = surplusModel?.output_price_per_token;
  const surplusBilling: SurplusBillingPricing | null =
    surplusInputPrice !== undefined &&
    Number.isFinite(surplusInputPrice) &&
    surplusInputPrice >= 0 &&
    surplusOutputPrice !== undefined &&
    Number.isFinite(surplusOutputPrice) &&
    surplusOutputPrice >= 0
      ? {
          input_price_per_token: surplusInputPrice,
          output_price_per_token: surplusOutputPrice,
          ...(surplusModel?.cache_read_price_per_token === undefined ? {} : { cache_read_price_per_token: surplusModel.cache_read_price_per_token }),
          ...(surplusModel?.cache_write_price_per_token === undefined ? {} : { cache_write_price_per_token: surplusModel.cache_write_price_per_token }),
        }
      : null;
  // A known Codex model retains the historical OpenLux roster path even when
  // its discovery request is temporarily unavailable. Surplus is selected
  // only when its own catalog proves that the exact model is routable.
  const meteredCanServe = Boolean(readMeteredApiKey()) && (input.meteredCatalog === null ? input.codexModelKnown : meteredModelSupportsRoute);
  // Tool-bearing work needs explicit capability evidence from the exact
  // Surplus model record. Missing or partial metadata remains fail-closed.
  const surplusCanServe =
    (!input.requestUsesTools || surplusModel?.supports_tools === true) && Boolean(readSurplusApiKey()) && surplusModelSupportsRoute && surplusBilling !== null;
  // The paid tiers have a fixed cost order for every model. Provider
  // availability may remove a tier, but it must never reverse the order.
  const preferredPaidProviders: readonly ("metered" | "surplus")[] = ["surplus", "metered"];
  const paidProviders = preferredPaidProviders.filter((provider) => (provider === "surplus" ? surplusCanServe : meteredCanServe));
  return {
    surplusBilling,
    paidProviders,
    paidModelKnown: meteredModelSupportsRoute || surplusModelSupportsRoute,
    meteredOnly: paidProviders.length > 0 && !input.codexModelKnown,
  };
};

const isAuthoritativeCapacityStatus = (status: number | null): boolean => status === 402 || status === 429;

const paidFallbackAbortReason = (fallbackSignal: AbortSignal | undefined): Error =>
  fallbackSignal?.reason instanceof Error ? fallbackSignal.reason : new DOMException("The request was aborted.", "AbortError");

const surplusPreHeaderFailureResponse = (error: unknown, timedOut: boolean): Response => {
  if (timedOut) {
    return openaiError(504, "Surplus upstream exceeded the gateway deadline before response headers were received.", "gateway_timeout", {
      type: "server_error",
      headers: { "x-uos-upstream": "surplus" },
    });
  }
  if (error instanceof SurplusError) {
    return openaiError(error.status, error.message, error.code, {
      type: "server_error",
      headers: { "x-uos-upstream": "surplus" },
    });
  }
  return openaiError(502, "Surplus upstream request failed before response headers were received.", "upstream_error", {
    type: "server_error",
    headers: { "x-uos-upstream": "surplus" },
  });
};

const fetchTemporaryFreeSurplusRoutedResponses = async (
  body: Record<string, unknown>,
  options: Readonly<{ model: string; usageContext?: UsageContext; signal?: AbortSignal }>
): Promise<RoutedResponsesUpstream> => {
  const telemetry = options.usageContext?.responseTelemetry;
  if (telemetry) {
    telemetry.provider = "surplus";
    telemetry.fallbackReason = null;
    telemetry.accountSlot = null;
    telemetry.accountCohortId = null;
    telemetry.providerRequestId = null;
    telemetry.quotaUsedPercent = null;
  }
  recordAttemptedProvider(options.usageContext, "surplus");
  const dispatchState = { transportStarted: false };
  try {
    const result = await fetchSurplusResponses(body, {
      signal: options.signal,
      beforeDispatch: () => options.usageContext?.beforeProviderDispatch?.("surplus") ?? Promise.resolve(undefined),
      onDispatch: () => {
        dispatchState.transportStarted = true;
        recordFirstProviderDispatch(options.usageContext);
      },
      sentinelUpstreamRecorder: options.usageContext?.sentinelUpstreamRecorder,
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
    if (dispatchState.transportStarted) await recordPaidProviderResponseHealth("surplus", status);
    if (options.signal?.aborted) throw error;
    return {
      response: surplusPreHeaderFailureResponse(error, timedOut),
      provider: "surplus",
      paidFallback: null,
      gatewayResponse: true,
      fallbackReason: null,
    };
  }
};

const loadPaidResponsesCatalogs = async (
  body: Record<string, unknown>,
  options: Readonly<{ model: string; route: "chat.completions" | "responses"; signal?: AbortSignal }>
): Promise<
  Readonly<{
    codexModelKnown: boolean;
    endpointType: string;
    requestUsesTools: boolean;
    meteredCatalog: Awaited<ReturnType<typeof fetchMeteredModels>>;
    surplusCatalog: Awaited<ReturnType<typeof fetchSurplusModels>>;
    routing: Readonly<{
      surplusBilling: SurplusBillingPricing | null;
      paidProviders: readonly ("metered" | "surplus")[];
      paidModelKnown: boolean;
      meteredOnly: boolean;
    }>;
  }>
> => {
  const codexCatalog = await loadCodexModelsSnapshot();
  const codexModelKnown =
    codexCatalog?.models.some((model) => {
      const record = model as Record<string, unknown>;
      return (getString(record.slug) ?? getString(record.id) ?? getString(record.model) ?? getString(record.name)) === options.model;
    }) === true;
  const endpointType = options.route === "responses" ? "openai-response" : "openai";
  const requestUsesTools = Array.isArray(body.tools) && body.tools.length > 0;
  // Cached discovery must not delay the primary Codex transport. A cold
  // discovery is only needed before dispatch when the model is not in the
  // Codex roster; known Codex models can use the historical Metered path and
  // refresh paid catalogs after a fallback-triggering primary response.
  let [meteredCatalog, surplusCatalog] = await Promise.all([fetchMeteredModels({ cachedOnly: true }), fetchSurplusModels({ cachedOnly: true })]);
  if (
    !codexModelKnown &&
    (paidCatalogNeedsRefresh(meteredCatalog, METERED_MODELS_CACHE_TTL_MS) || paidCatalogNeedsRefresh(surplusCatalog, SURPLUS_MODELS_CACHE_TTL_MS))
  ) {
    [meteredCatalog, surplusCatalog] = await refreshPaidCatalogs(meteredCatalog, surplusCatalog, options.signal);
  }
  const routing = resolvePaidRoutingState({ meteredCatalog, surplusCatalog, codexModelKnown, endpointType, requestUsesTools, model: options.model });
  if (routing.paidProviders.length) refreshStalePaidCatalogsInBackground(meteredCatalog, surplusCatalog);
  return { codexModelKnown, endpointType, requestUsesTools, meteredCatalog, surplusCatalog, routing };
};

const rejectPaidAdmission = (
  provider: "metered" | "surplus",
  errorReason: string,
  telemetry: ResponseTelemetryState | undefined,
  usageContext: UsageContext | undefined,
  model: string,
  logReason = errorReason
): RoutedResponsesUpstream => {
  if (telemetry) {
    telemetry.provider = "gateway";
    telemetry.fallbackReason = "dynamic_paid_model";
  }
  logPaidProviderAdmissionRejected(usageContext?.requestId ?? "unknown", model, logReason);
  return {
    response: paidProviderAdmissionError(errorReason),
    provider,
    paidFallback: null,
    gatewayResponse: true,
    fallbackReason: "dynamic_paid_model",
    allowRemovedProviderRecovery: false,
  };
};

const surplusModelLacksToolProof = (surplusCatalog: Awaited<ReturnType<typeof fetchSurplusModels>>, model: string): boolean =>
  surplusCatalog?.models.some((entry) => entry.id === model && entry.supports_tools !== true) === true;

const rejectToolUnverifiedSurplusModel = (
  surplusCatalog: Awaited<ReturnType<typeof fetchSurplusModels>>,
  surplusBilling: SurplusBillingPricing | null,
  requestUsesTools: boolean,
  endpointType: string,
  model: string,
  usageContext: UsageContext | undefined,
  telemetry: ResponseTelemetryState | undefined
): RoutedResponsesUpstream | null => {
  const surplusModelSupportsRoute =
    surplusCatalog?.models.some((entry) => entry.id === model && entry.supported_endpoint_types.includes(endpointType)) === true;
  const surplusModelWithoutToolProof =
    requestUsesTools &&
    surplusModelSupportsRoute &&
    Boolean(readSurplusApiKey()) &&
    surplusBilling !== null &&
    surplusModelLacksToolProof(surplusCatalog, model);
  if (!surplusModelWithoutToolProof) return null;
  if (telemetry) {
    telemetry.provider = "gateway";
    telemetry.fallbackReason = "dynamic_paid_model";
  }
  logPaidProviderAdmissionRejected(usageContext?.requestId ?? "unknown", model, "tool_capability_unverified");
  return {
    response: openaiError(400, `Model '${model}' does not support tool calling through the configured providers.`, "model_tool_calling_unsupported", {
      param: "tools",
    }),
    provider: "surplus",
    paidFallback: null,
    gatewayResponse: true,
    fallbackReason: "dynamic_paid_model",
    allowRemovedProviderRecovery: false,
  };
};

const rejectUnroutablePaidModel = (
  codexModelKnown: boolean,
  routing: Readonly<{ paidModelKnown: boolean; paidProviders: readonly ("metered" | "surplus")[]; surplusBilling: SurplusBillingPricing | null }>,
  surplusCatalog: Awaited<ReturnType<typeof fetchSurplusModels>>,
  endpointType: string,
  requestUsesTools: boolean,
  usageContext: UsageContext | undefined,
  model: string,
  telemetry: ResponseTelemetryState | undefined
): RoutedResponsesUpstream | null => {
  if (codexModelKnown || !routing.paidModelKnown || routing.paidProviders.length > 0) return null;
  const surplusModelSupportsRoute =
    surplusCatalog?.models.some((entry) => entry.id === model && entry.supported_endpoint_types.includes(endpointType)) === true;
  const catalogPaidProvider: "metered" | "surplus" = surplusModelSupportsRoute ? "surplus" : "metered";
  if (usageContext?.paidFallbackEnabled === false) {
    return rejectPaidAdmission(catalogPaidProvider, "disabled", telemetry, usageContext, model);
  }
  const toolUnverified = rejectToolUnverifiedSurplusModel(
    surplusCatalog,
    routing.surplusBilling,
    requestUsesTools,
    endpointType,
    model,
    usageContext,
    telemetry
  );
  if (toolUnverified) return toolUnverified;
  return rejectPaidAdmission(catalogPaidProvider, "provider_unconfigured", telemetry, usageContext, model);
};

const forcedDebugCodexStatus = (debugScenario: string): number | null => {
  if (debugScenario === "metered_first" || debugScenario === "codex_429") return 429;
  if (debugScenario === "codex_403") return 403;
  if (debugScenario === "codex_401") return 401;
  return null;
};

const codexCacheScopeForUsageContext = (usageContext: UsageContext | undefined): string | undefined => {
  const principal = usageContext?.idempotencyPrincipal;
  if (principal) return principal;
  const keyId = usageContext?.keyId;
  if (keyId) return keyId;
  return undefined;
};

const dispatchCodexPrimaryResponse = async (
  body: Record<string, unknown>,
  options: Readonly<{ model: string; usageContext?: UsageContext; clientVersion?: string | null; signal?: AbortSignal }>,
  directPaidPrimary: Response | null
): Promise<Response> => {
  const debugScenario = (await loadDebugRoutingConfig()).scenario;
  const forcedStatus = forcedDebugCodexStatus(debugScenario);
  if (directPaidPrimary) return directPaidPrimary;
  if (forcedStatus !== null) {
    return openaiError(forcedStatus, `Debug routing scenario forced Codex ${forcedStatus}.`, forcedStatus === 429 ? "rate_limit_error" : "debug_forced_codex", {
      headers: { "x-uos-upstream": "chatgpt_codex", "x-uos-debug-scenario": debugScenario },
    });
  }
  try {
    return await fetchCodexResponses(body, {
      clientVersion: options.clientVersion,
      cacheScope: codexCacheScopeForUsageContext(options.usageContext),
      signal: options.signal,
      requestId: options.usageContext?.requestId,
      // Keep terminal telemetry bounded: only the first real Codex transport
      // attempt contributes dispatch/header timings, even when routing retries.
      timing: {
        onDispatch: () => {
          recordFirstCodexDispatch(options.usageContext);
        },
        onHeaders: () => {
          recordFirstCodexHeaders(options.usageContext);
        },
      },
      beforeDispatch: () => options.usageContext?.beforeProviderDispatch?.("chatgpt_codex") ?? Promise.resolve(undefined),
      bankedReset: codexBankedResetOptionsForTest ?? undefined,
      sentinelUpstreamRecorder: options.usageContext?.sentinelUpstreamRecorder,
    });
  } catch (error) {
    if (!(error instanceof CodexError) || error.status !== 401) throw error;
    return openaiError(error.status, error.message, error.code);
  }
};

const recordCodexPrimaryTelemetry = async (primary: Response, telemetry: ResponseTelemetryState | undefined): Promise<void> => {
  if (!telemetry) return;
  telemetry.accountSlot = getCodexResponseSlot(primary);
  telemetry.accountCohortId = await getCodexResponseAccountCohortId(primary);
  telemetry.affinityOutcome = getCodexResponseAffinityOutcome(primary);
  telemetry.providerRequestId = providerRequestIdFromResponse(primary);
};

const resolveDirectPaidPrimary = (
  routing: Readonly<{ meteredOnly: boolean; paidProviders: readonly ("metered" | "surplus")[] }>,
  telemetry: ResponseTelemetryState | undefined,
  usageContext: UsageContext | undefined
) => {
  const directPaidProvider = routing.meteredOnly ? routing.paidProviders[0] : null;
  const directPaidPrimary = directPaidProvider
    ? openaiError(503, "Paid-provider routing did not reach the selected upstream.", "paid_provider_not_dispatched", { type: "server_error" })
    : null;
  if (telemetry) telemetry.provider = directPaidProvider ?? "chatgpt_codex";
  if (!directPaidProvider) recordAttemptedProvider(usageContext, "chatgpt_codex");
  return { directPaidProvider, directPaidPrimary };
};

const paidFallbackReasonFor = (
  directPaidProvider: "metered" | "surplus" | null,
  primaryStatus: number,
  routingError: string | null
): InferenceFallbackReason | null => {
  if (directPaidProvider) return "dynamic_paid_model";
  if (primaryStatus === 429 && routingError === CODEX_QUOTA_BLOCKED_ERROR_CODE) return "primary_quota_blocked";
  return null;
};

const resolvePaidFallbackAdmission = (
  routing: Readonly<{ meteredOnly: boolean; paidProviders: readonly ("metered" | "surplus")[] }>,
  primary: Response,
  primaryStatus: number,
  routingError: string | null,
  usageContext: UsageContext | undefined,
  telemetry: ResponseTelemetryState | undefined,
  gatewayResponse: boolean,
  model: string
):
  | Readonly<{ kind: "rejected"; routed: RoutedResponsesUpstream }>
  | Readonly<{
      kind: "admitted";
      fallbackReason: InferenceFallbackReason;
      keyId: string;
      requestId: string;
      createdAtMs: number;
      rejectAdmission: (errorReason: string, logReason?: string) => RoutedResponsesUpstream;
    }> => {
  const directPaidProvider = routing.meteredOnly ? routing.paidProviders[0] : null;
  const keyId = usageContext?.keyId;
  const requestId = usageContext?.requestId;
  const createdAtMs = usageContext?.startedAtMs;
  // Only a complete, authoritative Codex quota/capacity classification may
  // admit paid fallback. Generic 429 responses remain request-local.
  const fallbackReason = paidFallbackReasonFor(directPaidProvider, primaryStatus, routingError);
  if (telemetry) telemetry.fallbackReason = fallbackReason;
  if (directPaidProvider && usageContext?.paidFallbackEnabled === false) {
    return { kind: "rejected", routed: rejectPaidAdmission(directPaidProvider, "disabled", telemetry, usageContext, model) };
  }
  if (!fallbackReason || usageContext?.paidFallbackEnabled === false || !keyId || !requestId || createdAtMs === undefined) {
    if (directPaidProvider) {
      return { kind: "rejected", routed: rejectPaidAdmission(directPaidProvider, "invalid_policy", telemetry, usageContext, model, "invalid_context") };
    }
    return {
      kind: "rejected",
      routed: { response: primary, provider: "chatgpt_codex", paidFallback: null, gatewayResponse, fallbackReason },
    };
  }
  const rejectAdmission = (errorReason: string, logReason = errorReason): RoutedResponsesUpstream =>
    directPaidProvider
      ? rejectPaidAdmission(directPaidProvider, errorReason, telemetry, usageContext, model, logReason)
      : { response: primary, provider: "chatgpt_codex", paidFallback: null, gatewayResponse, fallbackReason };
  return { kind: "admitted", fallbackReason, keyId, requestId, createdAtMs, rejectAdmission };
};

const replenishPaidFallbackCatalogs = async (
  meteredCatalog: Awaited<ReturnType<typeof fetchMeteredModels>>,
  surplusCatalog: Awaited<ReturnType<typeof fetchSurplusModels>>,
  routing: Readonly<{
    surplusBilling: SurplusBillingPricing | null;
    paidProviders: readonly ("metered" | "surplus")[];
    paidModelKnown: boolean;
    meteredOnly: boolean;
  }>,
  routingInput: Readonly<{ codexModelKnown: boolean; endpointType: string; requestUsesTools: boolean; model: string }>,
  fallbackSignal: AbortSignal | undefined
): Promise<
  Readonly<{
    meteredCatalog: Awaited<ReturnType<typeof fetchMeteredModels>>;
    surplusCatalog: Awaited<ReturnType<typeof fetchSurplusModels>>;
    routing: Readonly<{
      surplusBilling: SurplusBillingPricing | null;
      paidProviders: readonly ("metered" | "surplus")[];
      paidModelKnown: boolean;
      meteredOnly: boolean;
    }>;
  }>
> => {
  // A stale snapshot remains usable when it already selects a paid provider.
  // Missing catalogs must still be discovered before trusting that historical
  // provider path; otherwise Surplus-preferred models can bypass discovery.
  // When no provider is selectable, also re-discover expired catalogs so a
  // newly published model can still recover this request.
  if (
    meteredCatalog !== null &&
    surplusCatalog !== null &&
    (routing.paidProviders.length > 0 ||
      (!paidCatalogNeedsRefresh(meteredCatalog, METERED_MODELS_CACHE_TTL_MS) && !paidCatalogNeedsRefresh(surplusCatalog, SURPLUS_MODELS_CACHE_TTL_MS)))
  ) {
    if (routing.paidProviders.length) refreshStalePaidCatalogsInBackground(meteredCatalog, surplusCatalog);
    return { meteredCatalog, surplusCatalog, routing };
  }
  const [nextMeteredCatalog, nextSurplusCatalog] = await refreshPaidCatalogs(meteredCatalog, surplusCatalog, fallbackSignal);
  const nextRouting = resolvePaidRoutingState({
    meteredCatalog: nextMeteredCatalog,
    surplusCatalog: nextSurplusCatalog,
    codexModelKnown: routingInput.codexModelKnown,
    endpointType: routingInput.endpointType,
    requestUsesTools: routingInput.requestUsesTools,
    model: routingInput.model,
  });
  if (nextRouting.paidProviders.length) refreshStalePaidCatalogsInBackground(nextMeteredCatalog, nextSurplusCatalog);
  return { meteredCatalog: nextMeteredCatalog, surplusCatalog: nextSurplusCatalog, routing: nextRouting };
};

const cancelPaidFallbackPrefetchBeforeTransport = async (
  fallbackSignal: AbortSignal | undefined,
  primary: Response,
  reservation: PaidFallbackReservation
): Promise<void> => {
  if (fallbackSignal?.aborted) {
    cancelResponseBody(primary);
    await bestEffortPaidFallbackBookkeeping("prefetch cancellation recording", () => recordMeteredPrefetchCancellation(reservation));
    throw paidFallbackAbortReason(fallbackSignal);
  }
  cancelResponseBody(primary);
  if (fallbackSignal?.aborted) {
    await bestEffortPaidFallbackBookkeeping("prefetch cancellation recording", () => recordMeteredPrefetchCancellation(reservation));
    throw paidFallbackAbortReason(fallbackSignal);
  }
};

const reservePaidFallbackForRequest = async (
  reservationInput: Parameters<typeof reservePaidFallback>[0],
  rejectAdmission: (errorReason: string, logReason?: string) => RoutedResponsesUpstream
): Promise<Readonly<{ kind: "reserved"; reservation: PaidFallbackReservation }> | Readonly<{ kind: "final"; routed: RoutedResponsesUpstream }>> => {
  let decision: Awaited<ReturnType<typeof reservePaidFallback>>;
  try {
    decision = await reservePaidFallback(reservationInput);
  } catch (error) {
    warnPaidFallbackBookkeepingFailure("admission", error);
    return { kind: "final", routed: rejectAdmission("invalid_policy", "admission_error") };
  }
  if (decision.kind === "skip" || decision.kind === "blocked") {
    return { kind: "final", routed: rejectAdmission(decision.reason) };
  }
  return { kind: "reserved", reservation: decision.reservation };
};

const recordPaidFallbackProviderTransition = async (
  reservation: PaidFallbackReservation,
  responding: Readonly<{ provider: "metered" | "surplus"; requestId: string | null }> | null,
  recordUndispatched: (reservation: PaidFallbackReservation) => Promise<void>,
  labels: Readonly<{ undispatched: string; ambiguous: string }>
): Promise<void> => {
  if (responding === null) {
    await bestEffortPaidFallbackBookkeeping(labels.undispatched, () => recordUndispatched(reservation));
    return;
  }
  await bestEffortPaidFallbackBookkeeping(labels.ambiguous, () => recordMeteredAmbiguousFailure(reservation, responding.provider, responding.requestId));
};

const retainRespondingProviderTelemetry = (
  telemetry: ResponseTelemetryState | undefined,
  responding: Readonly<{ provider: "metered" | "surplus"; requestId: string | null }> | null
): void => {
  if (!telemetry || responding === null) return;
  telemetry.provider = responding.provider;
  telemetry.providerRequestId = responding.requestId;
};

const isIntermediatePaidProviderAttempt = (providerIndex: number, providerCount: number, status: number | null): boolean =>
  providerIndex < providerCount - 1 && isAuthoritativeCapacityStatus(status);

const abortInterProviderAttempts = async (
  fallbackSignal: AbortSignal,
  reservation: PaidFallbackReservation,
  responding: Readonly<{ provider: "metered" | "surplus"; requestId: string | null }> | null,
  telemetry: ResponseTelemetryState | undefined
): Promise<never> => {
  retainRespondingProviderTelemetry(telemetry, responding);
  await recordPaidFallbackProviderTransition(reservation, responding, recordMeteredPrefetchCancellation, {
    undispatched: "prefetch cancellation recording",
    ambiguous: "inter-provider cancellation ambiguity recording",
  });
  throw paidFallbackAbortReason(fallbackSignal);
};

const resolvePaidProviderAttemptFailure = async (
  error: unknown,
  provider: "metered" | "surplus",
  providerIndex: number,
  paidProviders: readonly ("metered" | "surplus")[],
  dispatchState: { transportStarted: boolean },
  responding: Readonly<{ provider: "metered" | "surplus"; requestId: string | null }> | null,
  reservation: PaidFallbackReservation,
  fallbackSignal: AbortSignal | undefined,
  telemetry: ResponseTelemetryState | undefined
): Promise<Readonly<{ retry: boolean; providerError: unknown }>> => {
  if (error instanceof ApiKeyQuotaDispatchError) {
    // Paid fallback writes a durable dispatch intent before provider
    // transport. A quota CAS rejection proves this provider was not
    // started, but an earlier provider in the same reservation may have
    // been contacted already.
    retainRespondingProviderTelemetry(telemetry, responding);
    await recordPaidFallbackProviderTransition(reservation, responding, recordMeteredUndispatchedCancellation, {
      undispatched: "pre-dispatch quota cancellation recording",
      ambiguous: "prior-provider quota rejection ambiguity recording",
    });
    throw error;
  }
  if (fallbackSignal?.aborted) {
    // The explicit dispatch callback distinguishes cancellation before
    // this transport from an abort that may have reached the provider.
    // Keep any contacted provider for reconciliation and never retry.
    const ambiguous = dispatchState.transportStarted ? { provider, requestId: null } : responding;
    retainRespondingProviderTelemetry(telemetry, ambiguous);
    await recordPaidFallbackProviderTransition(reservation, ambiguous, recordMeteredUndispatchedCancellation, {
      undispatched: "pre-transport cancellation recording",
      ambiguous: "aborted transport ambiguity recording",
    });
    throw paidFallbackAbortReason(fallbackSignal);
  }
  const status = paidProviderErrorStatus(error);
  await recordPaidProviderResponseHealth(provider, status);
  if (dispatchState.transportStarted) {
    await bestEffortPaidFallbackBookkeeping("transport failure ambiguity recording", () => recordMeteredAmbiguousFailure(reservation, provider));
    throw error;
  }
  return { retry: isIntermediatePaidProviderAttempt(providerIndex, paidProviders.length, status), providerError: error };
};

const failedPaidProviderAttempts = async (
  providerError: unknown,
  selectedProvider: "metered" | "surplus",
  reservation: PaidFallbackReservation,
  responding: Readonly<{ provider: "metered" | "surplus"; requestId: string | null }> | null
): Promise<Readonly<{ kind: "failed"; providerError: unknown; selectedProvider: "metered" | "surplus" }>> => {
  if (providerError instanceof ApiKeyQuotaDispatchError) {
    throw providerError;
  }
  await recordPaidFallbackProviderTransition(reservation, responding, recordMeteredUndispatchedCancellation, {
    undispatched: "undispatched failure recording",
    ambiguous: "ambiguous failure recording",
  });
  return { kind: "failed", providerError, selectedProvider };
};

const fetchPaidProviderResponses = async (
  body: Record<string, unknown>,
  options: Readonly<{ model: string; usageContext?: UsageContext }>,
  fallbackSignal: AbortSignal | undefined,
  provider: "metered" | "surplus",
  surplusCatalog: Awaited<ReturnType<typeof fetchSurplusModels>>,
  dispatchState: { transportStarted: boolean }
): Promise<Awaited<ReturnType<typeof fetchMeteredResponses>> | Awaited<ReturnType<typeof fetchSurplusResponses>>> => {
  if (provider === "surplus") {
    return await fetchSurplusResponses(body, {
      signal: fallbackSignal,
      supportsParallelToolCalls: surplusCatalog?.models.find((entry) => entry.id === options.model)?.supports_parallel_tool_calls === true,
      beforeDispatch: () => options.usageContext?.beforeProviderDispatch?.("surplus") ?? Promise.resolve(undefined),
      onDispatch: () => {
        dispatchState.transportStarted = true;
        recordFirstProviderDispatch(options.usageContext);
      },
      sentinelUpstreamRecorder: options.usageContext?.sentinelUpstreamRecorder,
    });
  }
  return await fetchMeteredResponses(body, {
    signal: fallbackSignal,
    beforeDispatch: () => options.usageContext?.beforeProviderDispatch?.("metered") ?? Promise.resolve(undefined),
    onDispatch: () => {
      dispatchState.transportStarted = true;
      recordFirstProviderDispatch(options.usageContext);
    },
    sentinelUpstreamRecorder: options.usageContext?.sentinelUpstreamRecorder,
  });
};

const runPaidProviderAttempts = async (
  body: Record<string, unknown>,
  options: Readonly<{ model: string; usageContext?: UsageContext }>,
  fallbackSignal: AbortSignal | undefined,
  telemetry: ResponseTelemetryState | undefined,
  reservation: PaidFallbackReservation,
  paidProviders: readonly ("metered" | "surplus")[],
  surplusCatalog: Awaited<ReturnType<typeof fetchSurplusModels>>
): Promise<
  | Readonly<{
      kind: "delivered";
      result: Awaited<ReturnType<typeof fetchMeteredResponses>> | Awaited<ReturnType<typeof fetchSurplusResponses>>;
      selectedProvider: "metered" | "surplus";
    }>
  | Readonly<{ kind: "failed"; providerError: unknown; selectedProvider: "metered" | "surplus" }>
> => {
  let result: Awaited<ReturnType<typeof fetchMeteredResponses>> | Awaited<ReturnType<typeof fetchSurplusResponses>> | null = null;
  let selectedProvider: "metered" | "surplus" = paidProviders[0];
  let providerError: unknown = null;
  let responding: Readonly<{ provider: "metered" | "surplus"; requestId: string | null }> | null = null;
  for (const [providerIndex, provider] of paidProviders.entries()) {
    if (fallbackSignal?.aborted) {
      await abortInterProviderAttempts(fallbackSignal, reservation, responding, telemetry);
    }
    selectedProvider = provider;
    if (telemetry) {
      telemetry.provider = provider;
      telemetry.providerRequestId = null;
    }
    recordAttemptedProvider(options.usageContext, provider);
    const dispatchState = { transportStarted: false };
    try {
      const candidate = await fetchPaidProviderResponses(body, options, fallbackSignal, provider, surplusCatalog, dispatchState);
      recordFirstProviderHeaders(options.usageContext);
      await recordPaidProviderResponseHealth(provider, candidate.response.status, candidate.request_id);
      if (isIntermediatePaidProviderAttempt(providerIndex, paidProviders.length, candidate.response.status)) {
        // Keep the reservation uncommitted until the provider that will be
        // delivered to the client is known. The paid-fallback ledger has one
        // terminal provider/request-id pair; recording this intermediate
        // attempt would pin reconciliation to the failed provider and leave a
        // later successful provider unbillable.
        responding = { provider, requestId: normalizeProviderRequestId(candidate.request_id) };
        cancelResponseBody(candidate.response);
        continue;
      }
      result = candidate;
      break;
    } catch (error) {
      const failure = await resolvePaidProviderAttemptFailure(
        error,
        provider,
        providerIndex,
        paidProviders,
        dispatchState,
        responding,
        reservation,
        fallbackSignal,
        telemetry
      );
      providerError = failure.providerError;
      if (!failure.retry) break;
    }
  }
  if (!result) return await failedPaidProviderAttempts(providerError, selectedProvider, reservation, responding);
  return { kind: "delivered", result, selectedProvider };
};

const paidProviderFailureResponse = (
  attempts: Readonly<{ providerError: unknown; selectedProvider: "metered" | "surplus" }>,
  reservation: PaidFallbackReservation,
  fallbackSignal: AbortSignal | undefined,
  fallbackReason: InferenceFallbackReason
): RoutedResponsesUpstream => {
  const error = attempts.providerError;
  const selectedProviderLabel = attempts.selectedProvider === "surplus" ? "Surplus" : "Metered";
  const abortReason = fallbackSignal?.reason;
  if (
    (fallbackSignal?.aborted && abortReason instanceof Error && abortReason.name === "TimeoutError") ||
    (error instanceof Error && error.name === "TimeoutError")
  ) {
    return {
      response: openaiError(504, `${selectedProviderLabel} upstream exceeded the gateway deadline before response headers were received.`, "gateway_timeout", {
        type: "server_error",
        headers: { "x-uos-upstream": attempts.selectedProvider },
      }),
      provider: attempts.selectedProvider,
      paidFallback: reservation,
      gatewayResponse: true,
      fallbackReason,
    };
  }
  if (error instanceof MeteredError || error instanceof SurplusError) {
    return {
      response: openaiError(error.status, error.message, error.code, {
        type: "server_error",
        headers: { "x-uos-upstream": attempts.selectedProvider },
      }),
      provider: attempts.selectedProvider,
      paidFallback: reservation,
      gatewayResponse: true,
      fallbackReason,
    };
  }
  return {
    response: openaiError(502, `${selectedProviderLabel} upstream request failed before response headers were received.`, "upstream_error", {
      type: "server_error",
      headers: { "x-uos-upstream": attempts.selectedProvider },
    }),
    provider: attempts.selectedProvider,
    paidFallback: reservation,
    gatewayResponse: true,
    fallbackReason,
  };
};

const deliverPaidProviderResponse = async (
  attempts: Readonly<{
    result: Awaited<ReturnType<typeof fetchMeteredResponses>> | Awaited<ReturnType<typeof fetchSurplusResponses>>;
    selectedProvider: "metered" | "surplus";
  }>,
  reservation: PaidFallbackReservation,
  telemetry: ResponseTelemetryState | undefined,
  surplusBilling: SurplusBillingPricing | null,
  fallbackReason: InferenceFallbackReason
): Promise<RoutedResponsesUpstream> => {
  const providerRequestId = normalizeProviderRequestId(attempts.result.request_id);
  if (telemetry) telemetry.providerRequestId = providerRequestId;
  await bestEffortPaidFallbackBookkeeping("upstream response recording", () =>
    recordMeteredUpstreamResponse(reservation, attempts.result.response, providerRequestId, attempts.selectedProvider)
  );
  return {
    response: attempts.result.response,
    provider: attempts.selectedProvider,
    paidFallback: reservation,
    paidFallbackBilling: attempts.selectedProvider === "surplus" ? surplusBilling : null,
    paidFallbackProviderRequestId: providerRequestId,
    gatewayResponse: false,
    fallbackReason,
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
  }>
): Promise<RoutedResponsesUpstream> => {
  const fallbackSignal = options.fallbackSignal ?? options.signal;
  const telemetry = options.usageContext?.responseTelemetry;
  if (isTemporaryFreeSurplusModel(options.model)) return fetchTemporaryFreeSurplusRoutedResponses(body, options);
  const catalogs = await loadPaidResponsesCatalogs(body, options);
  const { codexModelKnown, endpointType, requestUsesTools } = catalogs;
  const meteredCatalog = catalogs.meteredCatalog;
  let surplusCatalog = catalogs.surplusCatalog;
  let routing = catalogs.routing;
  const unroutableModel = rejectUnroutablePaidModel(
    codexModelKnown,
    routing,
    surplusCatalog,
    endpointType,
    requestUsesTools,
    options.usageContext,
    options.model,
    telemetry
  );
  if (unroutableModel) return unroutableModel;
  const { directPaidProvider, directPaidPrimary } = resolveDirectPaidPrimary(routing, telemetry, options.usageContext);
  let primary = await dispatchCodexPrimaryResponse(body, options, directPaidPrimary);
  const primaryStatus = primary.status;
  const authReauthenticationPrimary = primaryStatus === 401 && responseWarnings(primary).includes(CODEX_AUTH_REAUTH_WARNING);
  await recordCodexPrimaryTelemetry(primary, telemetry);
  const routingError = getCodexRoutingError(primary);
  const gatewayResponse = directPaidProvider !== null || routingError === CODEX_QUOTA_BLOCKED_ERROR_CODE || routingError === CODEX_UPSTREAM_DEGRADED_ERROR_CODE;
  if (authReauthenticationPrimary) {
    primary = new Response(primary.body, {
      status: 503,
      statusText: primary.statusText,
      headers: primary.headers,
    });
  }
  const admission = resolvePaidFallbackAdmission(
    routing,
    primary,
    primaryStatus,
    routingError,
    options.usageContext,
    telemetry,
    gatewayResponse,
    options.model
  );
  if (admission.kind === "rejected") return admission.routed;
  const { fallbackReason, keyId, requestId, createdAtMs } = admission;
  if (fallbackSignal?.aborted) {
    cancelResponseBody(primary);
    throw paidFallbackAbortReason(fallbackSignal);
  }
  const replenished = await replenishPaidFallbackCatalogs(
    meteredCatalog,
    surplusCatalog,
    routing,
    { codexModelKnown, endpointType, requestUsesTools, model: options.model },
    fallbackSignal
  );
  surplusCatalog = replenished.surplusCatalog;
  routing = replenished.routing;
  if (!routing.paidProviders.length) {
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
    allowUnrosteredModel: routing.meteredOnly,
    reason: fallbackReason,
  } as const;
  const reserved = await reservePaidFallbackForRequest(reservationInput, admission.rejectAdmission);
  if (reserved.kind === "final") return reserved.routed;
  const reservation = reserved.reservation;

  await cancelPaidFallbackPrefetchBeforeTransport(fallbackSignal, primary, reservation);
  if (telemetry) {
    telemetry.provider = routing.paidProviders[0];
    telemetry.accountSlot = null;
    telemetry.accountCohortId = null;
    telemetry.providerRequestId = null;
    telemetry.quotaUsedPercent = reservation.quota_used_percent;
  }
  logPaidProviderSelected(requestId, fallbackReason, routing.paidProviders[0]);
  const attempts = await runPaidProviderAttempts(body, options, fallbackSignal, telemetry, reservation, routing.paidProviders, surplusCatalog);
  if (attempts.kind === "failed") return paidProviderFailureResponse(attempts, reservation, fallbackSignal, fallbackReason);
  return deliverPaidProviderResponse(attempts, reservation, telemetry, routing.surplusBilling, fallbackReason);
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
  const trimmed = id?.trim();
  if (!trimmed) return null;
  return trimmed;
};

const findSnapshotModelRecord = (snapshot: CodexModelsSnapshot | null, model: string): Record<string, unknown> | null => {
  const target = model.trim();
  if (!target) return null;
  if (!snapshot || !Array.isArray(snapshot.models)) return null;
  return (
    snapshot.models.find((entry) => {
      if (!isRecord(entry)) return false;
      return modelIdFromSnapshotRecord(entry) === target;
    }) ?? null
  );
};

const normalizeSnapshotReasoningEffort = (value: unknown): ReasoningEffort | null => (value === null ? "none" : normalizeReasoningEffort(value));

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

const extractSnapshotReasoningEffortWireMap = (model: Record<string, unknown> | null): ReadonlyMap<ReasoningEffort, ReasoningEffort> => {
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

const getCodexModelMetadata = async (model: string, route: "chat.completions" | "responses"): Promise<CodexModelMetadata> => {
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
  const [metered, surplus] = await Promise.all([fetchMeteredModels(), fetchSurplusModels()]);
  const meteredRecord = metered?.models.find((candidate) => candidate.id === model);
  const surplusRecord = surplus?.models.find((candidate) => candidate.id === model);
  const endpointType = route === "responses" ? "openai-response" : "openai";
  const routeRecord = [meteredRecord, surplusRecord].find((candidate) => candidate?.supported_endpoint_types.includes(endpointType));
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

const validateCodexModelAvailable = (model: string, route: "chat.completions" | "responses", metadata: CodexModelMetadata): Response | null => {
  if (metadata.supportedEndpoints && !metadata.supportedEndpoints.includes(route === "responses" ? "openai-response" : "openai")) {
    return openaiError(404, `The model '${model}' does not support ${route}. Use /v1/models for supported models.`, "model_not_found", { param: "model" });
  }
  // The snapshot is read back from KV, so keep the runtime guard on `models`
  // even though the declared type always presents it.
  const snapshotModels = metadata.snapshot?.models;
  if (!snapshotModels?.length || metadata.record) return null;
  return openaiError(
    404,
    `The model '${model}' does not exist or is not available through this gateway. Use /v1/models for supported models.`,
    "model_not_found",
    { param: "model" }
  );
};

const promptCacheControlParam = (rawRecord: Record<string, unknown>): string | null => {
  for (const key of ["prompt_cache_key", "prompt_cache_options", "prompt_cache_retention"] as const) {
    if (Object.prototype.hasOwnProperty.call(rawRecord, key)) return key;
  }
  return null;
};

const hasExplicitPromptCacheBreakpoint = (value: unknown): boolean => isRecord(value) && !Array.isArray(value) && value.mode === "explicit";

type ExplicitPromptCacheBreakpoint = Readonly<{
  param: string;
  blockType: string | null;
}>;

const collectExplicitPromptCacheBreakpoint = (value: Record<string, unknown>, param: string, breakpoints: ExplicitPromptCacheBreakpoint[]): void => {
  if (!hasExplicitPromptCacheBreakpoint(value.prompt_cache_breakpoint)) return;
  breakpoints.push({ param, blockType: getString(value.type) });
};

const collectIndexedExplicitPromptCacheBreakpoints = (values: readonly unknown[], paramPrefix: string, breakpoints: ExplicitPromptCacheBreakpoint[]): void => {
  for (const [index, value] of values.entries()) {
    if (!isRecord(value) || Array.isArray(value)) continue;
    collectExplicitPromptCacheBreakpoint(value, `${paramPrefix}[${index}].prompt_cache_breakpoint`, breakpoints);
  }
};

const findExplicitPromptCacheBreakpoints = (rawInput: unknown, inputParam: "input" | "messages"): ExplicitPromptCacheBreakpoint[] => {
  if (!Array.isArray(rawInput)) return [];

  const breakpoints: ExplicitPromptCacheBreakpoint[] = [];

  for (const [index, item] of rawInput.entries()) {
    if (!isRecord(item) || Array.isArray(item)) continue;
    const itemParam = `${inputParam}[${index}]`;
    if (inputParam === "input") {
      collectExplicitPromptCacheBreakpoint(item, `${itemParam}.prompt_cache_breakpoint`, breakpoints);
    }
    if (Array.isArray(item.content)) {
      collectIndexedExplicitPromptCacheBreakpoints(item.content, `${itemParam}.content`, breakpoints);
    }
    if (inputParam !== "input" || item.type !== "function_call_output" || !Array.isArray(item.output)) continue;
    collectIndexedExplicitPromptCacheBreakpoints(item.output, `${itemParam}.output`, breakpoints);
  }
  return breakpoints;
};

const activePromptCacheControls = (metadata: CodexModelMetadata): PromptCacheControls | null => {
  const capabilities = normalizePromptCacheCapabilities(metadata.record?.prompt_cache);
  if (capabilities === null || capabilities === false) return null;
  return capabilities.providers.find((provider) => provider.id === CODEX_CHATGPT_PROMPT_CACHE_PROVIDER)?.controls ?? null;
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
  openaiError(400, `Prompt cache control '${param}' is not supported for model '${model}'.`, "invalid_request_error", { param });

const promptCacheControlModeIsKnownUnsupported = (controls: PromptCacheControls, value: "implicit" | "explicit"): boolean =>
  controls.modes !== undefined && !controls.modes.includes(value);

const validatePromptCacheControlKey = (model: string, controls: PromptCacheControls, rawRecord: Record<string, unknown>): Response | null => {
  if (Object.prototype.hasOwnProperty.call(rawRecord, "prompt_cache_key") && controls.key === false) {
    return knownUnsupportedPromptCacheUseError(model, "prompt_cache_key");
  }
  return null;
};

const validatePromptCacheControlOptions = (model: string, controls: PromptCacheControls, rawRecord: Record<string, unknown>): Response | null => {
  const ttl = requestedPromptCacheTtl(rawRecord);
  if (ttl !== null && controls.ttls !== undefined && !controls.ttls.includes(ttl)) {
    return knownUnsupportedPromptCacheUseError(model, "prompt_cache_options.ttl");
  }

  const retention = getString(rawRecord.prompt_cache_retention);
  if (retention !== null && controls.legacy_retentions !== undefined && !controls.legacy_retentions.includes(retention)) {
    return knownUnsupportedPromptCacheUseError(model, "prompt_cache_retention");
  }
  return null;
};

const validatePromptCacheControlBreakpoints = (
  model: string,
  controls: PromptCacheControls,
  inputParam: "input" | "messages",
  breakpoints: readonly ExplicitPromptCacheBreakpoint[]
): Response | null => {
  for (const breakpoint of breakpoints) {
    if (controls.explicit_breakpoints === false || promptCacheControlModeIsKnownUnsupported(controls, "explicit")) {
      return knownUnsupportedPromptCacheUseError(model, breakpoint.param);
    }
    const endpoint = inputParam === "input" ? "responses" : "chat_completions";
    const supportedBlockTypes = controls.breakpoint_block_types?.[endpoint];
    if (supportedBlockTypes !== undefined && (breakpoint.blockType === null || !supportedBlockTypes.includes(breakpoint.blockType))) {
      return knownUnsupportedPromptCacheUseError(model, breakpoint.param);
    }
  }
  return null;
};

const validateKnownUnsupportedPromptCacheUse = (
  model: string,
  metadata: CodexModelMetadata,
  rawRecord: Record<string, unknown>,
  input: readonly ResponseInputItem[],
  inputParam: "input" | "messages"
): Response | null => {
  const breakpoints = countExplicitPromptCacheBreakpoints(input) > 0 ? findExplicitPromptCacheBreakpoints(rawRecord[inputParam], inputParam) : [];

  if (metadata.record?.prompt_cache === false) {
    const param = promptCacheControlParam(rawRecord) ?? breakpoints[0]?.param;
    if (!param) return null;
    return openaiError(400, `Prompt caching is not supported for model '${model}'.`, "invalid_request_error", { param });
  }

  // A missing capability envelope, another provider's record, or an omitted
  // control field is unknown—not an unsupported upstream feature. Preserve
  // standard OpenAI controls in each of those cases for forward compatibility.
  const controls = activePromptCacheControls(metadata);
  if (!controls) return null;

  const keyError = validatePromptCacheControlKey(model, controls, rawRecord);
  if (keyError) return keyError;

  const mode = requestedPromptCacheMode(rawRecord);
  if (mode?.value === "implicit" && (controls.implicit === false || promptCacheControlModeIsKnownUnsupported(controls, "implicit"))) {
    return knownUnsupportedPromptCacheUseError(model, mode.param);
  }

  const optionsError = validatePromptCacheControlOptions(model, controls, rawRecord);
  if (optionsError) return optionsError;

  const breakpointsError = validatePromptCacheControlBreakpoints(model, controls, inputParam, breakpoints);
  if (breakpointsError) return breakpointsError;

  if (mode?.value === "explicit" && (controls.explicit_breakpoints === false || promptCacheControlModeIsKnownUnsupported(controls, "explicit"))) {
    return knownUnsupportedPromptCacheUseError(model, mode.param);
  }

  return null;
};

const resolveDefaultReasoningLabel = (_modelReasoning: CodexModelReasoning, defaultEffort: ReasoningEffort): ReasoningEffort => defaultEffort;

const resolveReasoningLabelFromEffort = (effort: ReasoningEffort | undefined, defaultLabel: ReasoningEffort): ReasoningEffort => {
  if (effort === undefined) return defaultLabel;
  return effort;
};

const resolveReasoningLabelFromParam = (reasoning: Record<string, unknown> | undefined, defaultLabel: ReasoningEffort): ReasoningEffort => {
  if (reasoning === undefined) return defaultLabel;
  if (!isRecord(reasoning)) return defaultLabel;
  if ("effort" in reasoning) {
    const effort = normalizeReasoningEffort(reasoning.effort);
    if (effort) return effort;
  }
  return defaultLabel;
};

const extractReasoningParamEffort = (reasoning: Record<string, unknown> | undefined): ReasoningEffort | undefined => {
  if (reasoning === undefined) return undefined;
  if (!Object.prototype.hasOwnProperty.call(reasoning, "effort")) return undefined;
  return normalizeReasoningEffort(reasoning.effort) ?? undefined;
};

const reasoningEffortForCodexRequest = (effort: ReasoningEffort, modelReasoning: CodexModelReasoning): ReasoningEffort => {
  if (effort === "none") return "none";
  // Codex CLI's advanced `ultra` preset is client-side orchestration and
  // always uses `max` on the upstream wire, even for an older catalog that
  // has not yet published its wire map.
  if (effort === "ultra") return "max";
  return modelReasoning.wireEfforts.get(effort) ?? effort;
};

const normalizeReasoningParamForCodex = (
  reasoning: Record<string, unknown> | undefined,
  modelReasoning: CodexModelReasoning
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
  keys: readonly PassthroughToolSchemaKey[]
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
const parseReasoningEffortField = (value: unknown, fieldName: string): { ok: true; value: ReasoningEffort | undefined } | { ok: false; message: string } => {
  if (value === undefined || value === null) return { ok: true, value: undefined };
  if (typeof value !== "string") {
    return { ok: false, message: `${fieldName} must be a string` };
  }
  const normalized = normalizeReasoningEffort(value);
  if (!normalized) return { ok: false, message: `${fieldName} must be a non-empty string` };
  return { ok: true, value: normalized };
};

const normalizeReasoningSummaryField = (record: Record<string, unknown>): { ok: false; message: string } | null => {
  if (!("summary" in record)) return null;
  const summary = record.summary;
  if (summary === undefined || summary === null) delete record.summary;
  else if (typeof summary !== "string") {
    return { ok: false, message: "reasoning.summary must be a string" };
  }
  return null;
};

const normalizeReasoningGenerateSummaryField = (record: Record<string, unknown>): { ok: false; message: string } | null => {
  if (!("generate_summary" in record)) return null;
  const generateSummary = record.generate_summary;
  if (generateSummary === undefined || generateSummary === null) delete record.generate_summary;
  else if (typeof generateSummary !== "string") {
    return { ok: false, message: "reasoning.generate_summary must be a string" };
  }
  return null;
};

const parseReasoningParam = (value: unknown): { ok: true; value: Record<string, unknown> | undefined } | { ok: false; message: string } => {
  if (value === undefined || value === null) return { ok: true, value: undefined };
  if (!isRecord(value) || Array.isArray(value)) return { ok: false, message: "reasoning must be an object" };
  const normalized = { ...value };
  if ("effort" in normalized) {
    const effort = parseReasoningEffortField(normalized.effort, "reasoning.effort");
    if (!effort.ok) return effort;
    if (effort.value === undefined) delete normalized.effort;
    else normalized.effort = effort.value;
  }
  const summaryError = normalizeReasoningSummaryField(normalized);
  if (summaryError) return summaryError;
  const generateSummaryError = normalizeReasoningGenerateSummaryField(normalized);
  if (generateSummaryError) return generateSummaryError;

  return { ok: true, value: Object.keys(normalized).length ? normalized : undefined };
};

const parseStreamField = (value: unknown): { ok: true; value: boolean } | { ok: false; message: string } => {
  if (value === undefined || value === false) return { ok: true, value: false };
  if (value === true) return { ok: true, value: true };
  return { ok: false, message: "stream must be a boolean" };
};

const parseChatStreamOptions = (value: unknown): { ok: true; includeUsage: boolean } | { ok: false; message: string } => {
  if (value === undefined) return { ok: true, includeUsage: false };
  if (!isRecord(value) || Array.isArray(value)) {
    return { ok: false, message: "stream_options must be an object" };
  }
  if (value.include_usage !== undefined && typeof value.include_usage !== "boolean") {
    return { ok: false, message: "stream_options.include_usage must be a boolean" };
  }
  return { ok: true, includeUsage: value.include_usage === true };
};

const parseMaxCompletionTokensField = (value: unknown): { ok: true; value: number | undefined } | { ok: false; message: string } => {
  if (value === undefined) return { ok: true, value: undefined };
  if (typeof value !== "number" || !Number.isSafeInteger(value) || value <= 0) {
    return { ok: false, message: "max_completion_tokens must be a positive integer" };
  }
  return { ok: true, value };
};

const CHAT_COMPLETIONS_ALLOWED_KEYS = new Set(CHAT_COMPLETIONS_REQUEST_KEYS);
const RESPONSES_ALLOWED_KEYS = new Set(RESPONSES_REQUEST_KEYS);
const CODEX_RESPONSES_EXTENSION_KEYS = new Set(["client_metadata"]);

const findUnknownKey = (record: Record<string, unknown>, allowed: ReadonlySet<string>, extensions?: ReadonlySet<string>): string | null => {
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

type EmbeddingsParseResult = Readonly<{ ok: true; value: ParsedEmbeddingsRequest }> | Readonly<{ ok: false; response: Response }>;

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
const UOS_SYNC_EMBEDDINGS_ALLOWED_KEYS = new Set(["dimensions", "encoding_format", "input", "input_type", "model", "truncation", "user"]);
// Jobs deliberately retain the original Voyage-only profile. In particular,
// they require an explicit retrieval input type and only persist float vectors.
const UOS_EMBEDDINGS_JOB_ALLOWED_KEYS = new Set(["dimensions", "encoding_format", "input", "input_type", "model", "truncation"]);
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

const embeddingsJobKey = (tokenHash: string, cacheProfileKey: string, id: string): Deno.KvKey => ["embeddings", "jobs", "v2", tokenHash, cacheProfileKey, id];
const embeddingsJobLookupKey = (tokenHash: string, id: string): Deno.KvKey => ["embeddings", "jobs", "v2", "lookup", tokenHash, id];
const embeddingsJobInputKey = (tokenHash: string, cacheProfileKey: string, jobId: string, hash: string): Deno.KvKey => [
  "embeddings",
  "jobs",
  "v2",
  "input",
  tokenHash,
  cacheProfileKey,
  jobId,
  hash,
];

const embeddingsCacheIndexKey = (cacheProfileKey: string, createdAtMs: number, hash: string): Deno.KvKey => [
  "embeddings",
  "v2",
  "cache_index",
  cacheProfileKey,
  createdAtMs,
  hash,
];
const embeddingsCacheGlobalIndexPrefix: Deno.KvKey = ["embeddings", "v2", "cache_index_global"];
const embeddingsCacheGlobalIndexKey = (createdAtMs: number, cacheProfileKey: string, hash: string): Deno.KvKey => [
  ...embeddingsCacheGlobalIndexPrefix,
  createdAtMs,
  cacheProfileKey,
  hash,
];
const embeddingsCacheIndexByHashKey = (cacheProfileKey: string, hash: string): Deno.KvKey => ["embeddings", "v2", "cache_index_by_hash", cacheProfileKey, hash];
const embeddingsCacheKey = (cacheProfileKey: string, hash: string): Deno.KvKey => ["embeddings", "v2", "cache", cacheProfileKey, hash];

const embeddingsIdempotencyKey = (principalHash: string, idempotencyKeyHash: string): Deno.KvKey => [
  "embeddings",
  "idempotency",
  "v1",
  principalHash,
  idempotencyKeyHash,
];

const embeddingsIdempotencyResponseKeyPrefix = (principalHash: string, idempotencyKeyHash: string): Deno.KvKey => [
  "embeddings",
  "idempotency",
  "v1",
  "response",
  principalHash,
  idempotencyKeyHash,
];

const isEmbeddingsIdempotencyState = (value: unknown): value is EmbeddingsIdempotencyState =>
  value === "reserved" || value === "dispatched" || value === "succeeded" || value === "indeterminate";

const isEmbeddingsIdempotencyTimestampMs = (value: unknown): value is number => typeof value === "number" && Number.isFinite(value);

const isEmbeddingsIdempotencyNullableTimestampMs = (value: unknown): value is number | null => value === null || isEmbeddingsIdempotencyTimestampMs(value);

const isEmbeddingsIdempotencyNullableString = (value: unknown): value is string | null => value === null || typeof value === "string";

const isEmbeddingsIdempotencyNullableInteger = (value: unknown): value is number | null =>
  value === null || (typeof value === "number" && Number.isInteger(value));

const isEmbeddingsIdempotencyChunkCount = (value: unknown): value is number | null =>
  value === null || (typeof value === "number" && Number.isInteger(value) && value >= 1 && value <= EMBEDDINGS_IDEMPOTENCY_MAX_RESPONSE_CHUNKS);

const normalizeEmbeddingsIdempotencyRecord = (value: unknown): EmbeddingsIdempotencyRecord | null => {
  if (
    !isRecord(value) ||
    value.v !== 1 ||
    typeof value.fingerprint !== "string" ||
    !isEmbeddingsIdempotencyState(value.state) ||
    !isEmbeddingsIdempotencyNullableString(value.owner_request_id) ||
    !isEmbeddingsIdempotencyTimestampMs(value.created_at_ms) ||
    !isEmbeddingsIdempotencyTimestampMs(value.updated_at_ms) ||
    !isEmbeddingsIdempotencyNullableTimestampMs(value.lease_until_ms) ||
    !isEmbeddingsIdempotencyNullableInteger(value.response_status) ||
    !isEmbeddingsIdempotencyNullableString(value.response_content_type) ||
    !isEmbeddingsIdempotencyNullableString(value.response_generation) ||
    !isEmbeddingsIdempotencyChunkCount(value.response_chunk_count) ||
    !isEmbeddingsIdempotencyNullableString(value.response_sha256)
  ) {
    return null;
  }

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
  code: "embedding_idempotency_conflict" | "embedding_idempotency_in_progress" | "embedding_idempotency_indeterminate" | "embedding_idempotency_unavailable",
  retryAfterSeconds?: number
): Response =>
  openaiError(status, message, code, {
    type: status === 503 ? "server_error" : "idempotency_error",
    param: null,
    ...(retryAfterSeconds === undefined ? {} : { headers: { "Retry-After": String(retryAfterSeconds) } }),
  });

const embeddingsIdempotencyConflictResponse = (): Response =>
  embeddingsIdempotencyError(409, "Idempotency-Key was already used with a different embeddings request.", "embedding_idempotency_conflict");

const embeddingsIdempotencyInProgressResponse = (): Response =>
  embeddingsIdempotencyError(409, "The embeddings request for this Idempotency-Key is still in progress.", "embedding_idempotency_in_progress", 1);

const embeddingsIdempotencyIndeterminateResponse = (): Response =>
  embeddingsIdempotencyError(409, "The embeddings request outcome is indeterminate and will not be dispatched again.", "embedding_idempotency_indeterminate");

const embeddingsIdempotencyUnavailableResponse = (): Response =>
  embeddingsIdempotencyError(503, "Idempotent embeddings requests require durable KV storage.", "embedding_idempotency_unavailable");

const hasAsciiControlCharacter = (value: string): boolean => {
  for (let index = 0; index < value.length; index += 1) {
    const code = value.charCodeAt(index);
    if (code <= 0x1f || code === 0x7f) return true;
  }
  return false;
};

const buildEmbeddingsIdempotencyFingerprint = async (profile: ResolvedEmbeddingsProfile, orderedInputHashes: string[]): Promise<string> =>
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
    ])
  );

const loadEmbeddingsIdempotencyResponse = async (
  lease: Omit<EmbeddingsIdempotencyLease, "ownerRequestId">,
  record: EmbeddingsIdempotencyRecord
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

  const responseGeneration = record.response_generation;
  const chunks = await Promise.all(
    Array.from({ length: record.response_chunk_count }, (_, index) => lease.kv.get<string>([...lease.responseKeyPrefix, responseGeneration, index]))
  );
  if (chunks.some((entry) => typeof entry.value !== "string")) return null;
  const body = chunks.map((entry) => entry.value ?? "").join("");
  if ((await sha256Hex(body)) !== record.response_sha256) return null;
  return new Response(body, {
    status: record.response_status,
    headers: {
      "Content-Type": record.response_content_type,
      "x-uos-idempotency-replayed": "true",
    },
  });
};

const markEmbeddingsIdempotencyIndeterminate = async (lease: EmbeddingsIdempotencyLease): Promise<void> => {
  try {
    for (let attempt = 0; attempt < 4; attempt += 1) {
      const entry = await lease.kv.get<EmbeddingsIdempotencyRecord>(lease.key);
      const record = normalizeEmbeddingsIdempotencyRecord(entry.value);
      if (record?.fingerprint !== lease.fingerprint) return;
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
      const commit = await lease.kv.atomic().check(entry).set(lease.key, next, { expireIn: EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS }).commit();
      if (commit.ok) return;
    }
  } catch (error) {
    console.error("[ai.ubq.fi] embeddings idempotency indeterminate-state write failed:", error);
  }
};

const buildEmbeddingsIdempotencyIndeterminateRecord = (record: EmbeddingsIdempotencyRecord, now: number): EmbeddingsIdempotencyRecord => ({
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
});

const commitEmbeddingsIdempotencyRecordReplacement = async (
  kv: Deno.Kv,
  entry: Deno.KvEntryMaybe<EmbeddingsIdempotencyRecord>,
  key: Deno.KvKey,
  record: EmbeddingsIdempotencyRecord
): Promise<boolean> => {
  const commit = await kv.atomic().check(entry).set(key, record, { expireIn: EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS }).commit();
  return commit.ok;
};

const resolveSucceededEmbeddingsIdempotencyLease = async (params: {
  kv: Deno.Kv;
  key: Deno.KvKey;
  entry: Deno.KvEntryMaybe<EmbeddingsIdempotencyRecord>;
  record: EmbeddingsIdempotencyRecord;
  lease: EmbeddingsIdempotencyLease;
  now: number;
}): Promise<EmbeddingsIdempotencyAcquireResult | null> => {
  const replay = await loadEmbeddingsIdempotencyResponse(params.lease, params.record);
  if (replay) return { kind: "replay", response: replay };
  const committed = await commitEmbeddingsIdempotencyRecordReplacement(
    params.kv,
    params.entry,
    params.key,
    buildEmbeddingsIdempotencyIndeterminateRecord(params.record, params.now)
  );
  if (committed) return { kind: "error", response: embeddingsIdempotencyIndeterminateResponse() };
  return null;
};

const retireDispatchedEmbeddingsIdempotencyLease = async (params: {
  kv: Deno.Kv;
  key: Deno.KvKey;
  entry: Deno.KvEntryMaybe<EmbeddingsIdempotencyRecord>;
  record: EmbeddingsIdempotencyRecord;
  now: number;
}): Promise<EmbeddingsIdempotencyAcquireResult | null> => {
  const committed = await commitEmbeddingsIdempotencyRecordReplacement(
    params.kv,
    params.entry,
    params.key,
    buildEmbeddingsIdempotencyIndeterminateRecord(params.record, params.now)
  );
  if (committed) return { kind: "error", response: embeddingsIdempotencyIndeterminateResponse() };
  return null;
};

const attemptEmbeddingsIdempotencyLeaseAcquire = async (params: {
  kv: Deno.Kv;
  key: Deno.KvKey;
  fingerprint: string;
  requestId: string;
  lease: EmbeddingsIdempotencyLease;
}): Promise<EmbeddingsIdempotencyAcquireResult | null> => {
  const entry = await params.kv.get<EmbeddingsIdempotencyRecord>(params.key);
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
    const committed = await commitEmbeddingsIdempotencyRecordReplacement(params.kv, entry, params.key, reserved);
    if (committed) return { kind: "acquired", lease: params.lease };
    return null;
  }

  if (!record) return { kind: "error", response: embeddingsIdempotencyIndeterminateResponse() };
  if (record.fingerprint !== params.fingerprint) {
    return { kind: "error", response: embeddingsIdempotencyConflictResponse() };
  }

  if (record.state === "succeeded") {
    return await resolveSucceededEmbeddingsIdempotencyLease({
      kv: params.kv,
      key: params.key,
      entry,
      record,
      lease: params.lease,
      now,
    });
  }

  if (record.state === "indeterminate") {
    return { kind: "error", response: embeddingsIdempotencyIndeterminateResponse() };
  }

  if (record.lease_until_ms !== null && record.lease_until_ms > now) {
    return { kind: "error", response: embeddingsIdempotencyInProgressResponse() };
  }

  if (record.state === "dispatched") {
    return await retireDispatchedEmbeddingsIdempotencyLease({
      kv: params.kv,
      key: params.key,
      entry,
      record,
      now,
    });
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
  const committed = await commitEmbeddingsIdempotencyRecordReplacement(params.kv, entry, params.key, reserved);
  if (committed) return { kind: "acquired", lease: params.lease };
  return null;
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
      const result = await attemptEmbeddingsIdempotencyLeaseAcquire({
        kv: params.kv,
        key,
        fingerprint: params.fingerprint,
        requestId: params.requestId,
        lease,
      });
      if (result) return result;
    }
  } catch (error) {
    console.error("[ai.ubq.fi] embeddings idempotency reservation failed:", error);
  }

  return { kind: "error", response: embeddingsIdempotencyUnavailableResponse() };
};

const markEmbeddingsIdempotencyDispatched = async (lease: EmbeddingsIdempotencyLease): Promise<boolean> => {
  try {
    for (let attempt = 0; attempt < 4; attempt += 1) {
      const entry = await lease.kv.get<EmbeddingsIdempotencyRecord>(lease.key);
      const record = normalizeEmbeddingsIdempotencyRecord(entry.value);
      if (!record) return false;
      if (record.fingerprint !== lease.fingerprint || record.state !== "reserved" || record.owner_request_id !== lease.ownerRequestId) {
        return false;
      }
      const now = Date.now();
      const dispatched: EmbeddingsIdempotencyRecord = {
        ...record,
        state: "dispatched",
        updated_at_ms: now,
        lease_until_ms: now + EMBEDDINGS_IDEMPOTENCY_LEASE_MS,
      };
      const commit = await lease.kv.atomic().check(entry).set(lease.key, dispatched, { expireIn: EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS }).commit();
      if (commit.ok) return true;
    }
  } catch (error) {
    console.error("[ai.ubq.fi] embeddings idempotency dispatch-state write failed:", error);
  }
  return false;
};

const releaseEmbeddingsIdempotencyReservation = async (lease: EmbeddingsIdempotencyLease, allowDispatched: boolean): Promise<boolean> => {
  try {
    for (let attempt = 0; attempt < 4; attempt += 1) {
      const entry = await lease.kv.get<EmbeddingsIdempotencyRecord>(lease.key);
      const record = normalizeEmbeddingsIdempotencyRecord(entry.value);
      if (record?.fingerprint !== lease.fingerprint) return false;
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

const storeEmbeddingsIdempotencySuccess = async (lease: EmbeddingsIdempotencyLease, response: Response): Promise<boolean> => {
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
      await lease.kv.set([...lease.responseKeyPrefix, responseGeneration, index], chunks[index], { expireIn: EMBEDDINGS_IDEMPOTENCY_RESPONSE_TTL_MS });
    }
    const bodyHash = await sha256Hex(body);

    for (let attempt = 0; attempt < 4; attempt += 1) {
      const entry = await lease.kv.get<EmbeddingsIdempotencyRecord>(lease.key);
      const record = normalizeEmbeddingsIdempotencyRecord(entry.value);
      if (!record) return false;
      if (
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
      const commit = await lease.kv.atomic().check(entry).set(lease.key, succeeded, { expireIn: EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS }).commit();
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

const readEmbeddingsCacheErrorName = (error: unknown): string => {
  if (error === null || error === undefined) return "";
  const name = (error as { name?: unknown }).name;
  return typeof name === "string" ? name : "";
};

const readEmbeddingsCacheErrorMessage = (error: unknown): string => {
  if (error instanceof Error) return error.message;
  if (typeof error === "string") return error;
  return "";
};

const isEmbeddingsCacheQuotaError = (error: unknown): boolean => {
  const combined = `${readEmbeddingsCacheErrorName(error)} ${readEmbeddingsCacheErrorMessage(error)}`.toLowerCase();
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
  createdAtMs: number
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
      const updated = await kv
        .atomic()
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
    const created = await kv
      .atomic()
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
  deadlineMs: number
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
          `[ai.ubq.fi] embeddings_cache quota eviction requesting_profile=${cacheProfileKey} scope=global evicted=${evicted.evicted_embeddings} stale_index_deleted=${evicted.deleted_stale_index_keys} batch=${evictBatch}`
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

const listEmbeddingsCacheEvictionCandidates = async (
  kv: Deno.Kv,
  count: number
): Promise<{ globalIndexKey: Deno.KvKey; cacheProfileKey: string; createdAtMs: number; hash: string }[]> => {
  const keys: {
    globalIndexKey: Deno.KvKey;
    cacheProfileKey: string;
    createdAtMs: number;
    hash: string;
  }[] = [];
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
  return keys;
};

const evictEmbeddingsCacheCandidate = async (
  kv: Deno.Kv,
  candidate: { globalIndexKey: Deno.KvKey; cacheProfileKey: string; createdAtMs: number; hash: string },
  pointerEntry: Deno.KvEntryMaybe<number>
): Promise<{ evicted: boolean; deletedStaleIndexKey: boolean }> => {
  const { globalIndexKey, cacheProfileKey, createdAtMs, hash } = candidate;
  const pointer = normalizeEmbeddingsCacheTimestampMs(pointerEntry.value);
  const cacheKey = embeddingsCacheKey(cacheProfileKey, hash);
  const profileIndexKey = embeddingsCacheIndexKey(cacheProfileKey, createdAtMs, hash);

  if (pointer !== null && pointer !== createdAtMs) {
    // Stale duplicate index keys for this hash; delete only the indexes.
    const deleted = await kv.atomic().check(pointerEntry).delete(globalIndexKey).delete(profileIndexKey).commit();
    return { evicted: false, deletedStaleIndexKey: deleted.ok };
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
      const deleted = await kv.atomic().check(pointerEntry).delete(globalIndexKey).delete(profileIndexKey).commit();
      return { evicted: false, deletedStaleIndexKey: deleted.ok };
    }

    const commit = await kv.atomic().check(pointerEntry).delete(globalIndexKey).delete(profileIndexKey).delete(cacheKey).commit();
    return { evicted: commit.ok, deletedStaleIndexKey: false };
  }

  // Canonical pointer match: evict embedding + index + pointer as an atomic unit.
  const commit = await kv
    .atomic()
    .check(pointerEntry)
    .delete(globalIndexKey)
    .delete(profileIndexKey)
    .delete(cacheKey)
    .delete(embeddingsCacheIndexByHashKey(cacheProfileKey, hash))
    .commit();
  return { evicted: commit.ok, deletedStaleIndexKey: false };
};

const evictOldestEmbeddingsCacheEntries = async (kv: Deno.Kv, count: number): Promise<EmbeddingsCacheEvictResult> => {
  const candidates = await listEmbeddingsCacheEvictionCandidates(kv, count);
  if (!candidates.length) return { evicted_embeddings: 0, deleted_stale_index_keys: 0 };

  const byHashEntries = await Promise.all(
    candidates.map((candidate) => kv.get<number>(embeddingsCacheIndexByHashKey(candidate.cacheProfileKey, candidate.hash)))
  );

  let evictedEmbeddings = 0;
  let deletedStaleIndexKeys = 0;
  for (let i = 0; i < candidates.length; i += 1) {
    const outcome = await evictEmbeddingsCacheCandidate(kv, candidates[i], byHashEntries[i]);
    if (outcome.evicted) evictedEmbeddings += 1;
    if (outcome.deletedStaleIndexKey) deletedStaleIndexKeys += 1;
  }
  return { evicted_embeddings: evictedEmbeddings, deleted_stale_index_keys: deletedStaleIndexKeys };
};

const resolveEmbeddingsJobTokenSeed = (jobId: string, authToken: string | null, usageContext?: UsageContext): string => {
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

const chunkByTokenBudget = (items: readonly { hash: string; text: string }[], maxItems: number, maxTokens: number): { hash: string; text: string }[][] => {
  const out: { hash: string; text: string }[][] = [];
  const itemLimit = Math.max(1, Math.trunc(maxItems));
  const tokenLimit = Math.max(1, Math.trunc(maxTokens));

  let current: { hash: string; text: string }[] = [];
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
  const windowStart = typeof value.window_start_ms === "number" && Number.isFinite(value.window_start_ms) ? Math.trunc(value.window_start_ms) : null;
  const requests = typeof value.requests === "number" && Number.isFinite(value.requests) ? Math.trunc(value.requests) : null;
  const tokens = typeof value.tokens === "number" && Number.isFinite(value.tokens) ? Math.trunc(value.tokens) : null;
  if (windowStart === null || requests === null || tokens === null) return null;
  if (windowStart < 0 || requests < 0 || tokens < 0) return null;
  return { window_start_ms: windowStart, requests, tokens };
};

const wouldExceedVoyageRateLimit = (limit: number, wouldBeUsed: number): boolean => limit > 0 && wouldBeUsed > limit;

const tryReserveVoyageBudget = async (kv: Deno.Kv, tokens: number): Promise<{ ok: true } | { ok: false; wait_ms: number }> => {
  const windowMs = 60_000;
  const now = Date.now();
  const entry = await kv.get<VoyageRateLimitState>(VOYAGE_RATE_LIMIT_KEY);
  const current = normalizeVoyageRateLimitState(entry.value);
  const state = !current || now - current.window_start_ms >= windowMs ? { window_start_ms: now, requests: 0, tokens: 0 } : current;

  const wouldExceedRequests = wouldExceedVoyageRateLimit(VOYAGE_RATE_LIMIT_RPM, state.requests + 1);
  const wouldExceedTokens = wouldExceedVoyageRateLimit(VOYAGE_RATE_LIMIT_TPM, state.tokens + tokens);
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

const tryReserveVoyageBudgetWithRetries = async (kv: Deno.Kv, tokens: number): Promise<{ ok: true } | { ok: false; wait_ms: number }> => {
  let reserved: { ok: true } | { ok: false; wait_ms: number } = { ok: false, wait_ms: 0 };
  for (let attempt = 0; attempt < 6; attempt += 1) {
    reserved = await tryReserveVoyageBudget(kv, tokens);
    if (reserved.ok) return reserved;
    if (reserved.wait_ms > 0) break;
    await sleep(5 + attempt * 5);
  }
  return reserved;
};

const applyVoyageRateLimit = async (kv: Deno.Kv, tokens: number, deadlineMs: number): Promise<{ ok: true } | { ok: false; wait_ms: number }> => {
  // Best-effort concurrency-safe rate limiting using KV. If we can't reserve
  // within the request deadline, we fail with 429 and let clients retry.
  for (;;) {
    const now = Date.now();
    if (now >= deadlineMs) return { ok: false, wait_ms: 0 };
    const reserved = await tryReserveVoyageBudgetWithRetries(kv, tokens);
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

const parseEmbeddingsEncodingFormat = (value: unknown): { ok: true; value: EmbeddingsEncodingFormat } | { ok: false; message: string } => {
  if (value === undefined) return { ok: true, value: "float" };
  if (typeof value !== "string") return { ok: false, message: "encoding_format must be a string" };
  if (value === "float" || value === "base64") return { ok: true, value };
  return { ok: false, message: 'encoding_format must be one of: "float", "base64"' };
};

const parseEmbeddingsDimensions = (value: unknown): { ok: true; value: VoyageEmbeddingsDimension } | { ok: false; message: string } => {
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
  truncation: boolean
): string => JSON.stringify(["voyage-profile-v2", VOYAGE_EMBEDDINGS_MODEL, inputType, dimensions, VOYAGE_OUTPUT_DTYPE, encodingFormat, truncation]);

const buildResolvedEmbeddingsProfile = (
  inputType: VoyageEmbeddingsInputType,
  dimensions: VoyageEmbeddingsDimension,
  encodingFormat: EmbeddingsEncodingFormat,
  truncation: boolean
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

const embeddingsUserFieldError = (rawBody: Record<string, unknown>, isJob: boolean): Response | null => {
  if (isJob || !Object.prototype.hasOwnProperty.call(rawBody, "user")) return null;
  const user = rawBody.user;
  if (user === undefined || user === null || typeof user === "string") return null;
  return openaiError(400, "user must be a string", "invalid_request_error", { param: "user" });
};

const resolveEmbeddingsInputTypeField = (
  rawInputType: unknown,
  isJob: boolean
): { ok: true; value: VoyageEmbeddingsInputType } | { ok: false; response: Response } => {
  if (rawInputType === undefined) {
    if (isJob) {
      return {
        ok: false,
        response: openaiError(400, 'input_type is required and must be one of: "query", "document"', "invalid_request_error", {
          param: "input_type",
        }),
      };
    }
    return { ok: true, value: "document" };
  }
  if (rawInputType === "query" || rawInputType === "document") {
    return { ok: true, value: rawInputType };
  }
  return {
    ok: false,
    response: openaiError(400, 'input_type must be one of: "query", "document"', "invalid_request_error", { param: "input_type" }),
  };
};

const parseEmbeddingsTruncationField = (rawTruncation: unknown): { ok: true; value: boolean } | { ok: false; response: Response } => {
  if (rawTruncation !== undefined && typeof rawTruncation !== "boolean") {
    return {
      ok: false,
      response: openaiError(400, "truncation must be a boolean", "invalid_request_error", {
        param: "truncation",
      }),
    };
  }
  return { ok: true, value: rawTruncation ?? true };
};

const parseEmbeddingsInputList = (inputRaw: unknown): { ok: true; value: string[] } | { ok: false; response: Response } => {
  if (typeof inputRaw === "string") {
    return { ok: true, value: [inputRaw] };
  }
  if (!Array.isArray(inputRaw)) {
    return {
      ok: false,
      response: openaiError(400, "input must be a string or an array of strings", "invalid_request_error", {
        param: "input",
      }),
    };
  }
  const inputs: string[] = [];
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
  return { ok: true, value: inputs };
};

const measureEmbeddingsInputs = (inputs: string[]): { ok: true; totalChars: number } | { ok: false; response: Response } => {
  if (inputs.length === 0) {
    return {
      ok: false,
      response: openaiError(400, "input must be a non-empty string or a non-empty array", "invalid_request_error", { param: "input" }),
    };
  }
  if (inputs.length > EMBEDDINGS_MAX_INPUTS_PER_REQUEST) {
    return {
      ok: false,
      response: openaiError(400, `Too many inputs: ${inputs.length} (max ${EMBEDDINGS_MAX_INPUTS_PER_REQUEST})`, "invalid_request_error", { param: "input" }),
    };
  }

  let totalChars = 0;
  for (const text of inputs) {
    const len = text.length;
    if (len > EMBEDDINGS_MAX_CHARS_PER_INPUT) {
      return {
        ok: false,
        response: openaiError(400, `Input too large: ${len} chars (max ${EMBEDDINGS_MAX_CHARS_PER_INPUT})`, "invalid_request_error", { param: "input" }),
      };
    }
    totalChars += len;
    if (totalChars > EMBEDDINGS_MAX_TOTAL_CHARS) {
      return {
        ok: false,
        response: openaiError(400, `Request too large: ${totalChars} chars total (max ${EMBEDDINGS_MAX_TOTAL_CHARS})`, "invalid_request_error", {
          param: "input",
        }),
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
          { param: "input" }
        ),
      };
    }
  }

  return { ok: true, totalChars };
};

const parseEmbeddingsRequest = (rawBody: Record<string, unknown>, contract: "uos_sync" | "uos_job"): EmbeddingsParseResult => {
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
  if (!modelRaw?.trim()) {
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
      response: openaiError(400, 'encoding_format must be "float" for embeddings jobs', "invalid_request_error", { param: "encoding_format" }),
    };
  }

  const inputType = resolveEmbeddingsInputTypeField(rawBody.input_type, isJob);
  if (!inputType.ok) return inputType;

  const truncation = parseEmbeddingsTruncationField(rawBody.truncation);
  if (!truncation.ok) return truncation;

  const userError = embeddingsUserFieldError(rawBody, isJob);
  if (userError) return { ok: false, response: userError };

  const parsedInputs = parseEmbeddingsInputList(rawBody.input);
  if (!parsedInputs.ok) return parsedInputs;

  const measured = measureEmbeddingsInputs(parsedInputs.value);
  if (!measured.ok) return measured;

  return {
    ok: true,
    value: {
      model,
      inputs: parsedInputs.value,
      total_chars: measured.totalChars,
      profile: buildResolvedEmbeddingsProfile(inputType.value, dimensions.value, encodingFormat.value, truncation.value),
    },
  };
};

const parseUosEmbeddingsRequest = (rawBody: Record<string, unknown>): EmbeddingsParseResult => parseEmbeddingsRequest(rawBody, "uos_sync");

const parseEmbeddingsJobRequest = (rawBody: Record<string, unknown>): EmbeddingsParseResult => parseEmbeddingsRequest(rawBody, "uos_job");

const isValidEmbeddingVector = (value: unknown, dimensions: VoyageEmbeddingsDimension): value is number[] =>
  Array.isArray(value) && value.length === dimensions && value.every((item) => typeof item === "number" && Number.isFinite(item));

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
  const createdAt = typeof value.created_at_ms === "number" && Number.isFinite(value.created_at_ms) ? Math.trunc(value.created_at_ms) : null;
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

const decryptEmbeddingsJobInput = async (tokenSeed: string, record: EmbeddingsJobInputRecord): Promise<string | null> => {
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

const parseVoyageEmbeddingVector = (embedding: unknown): number[] => {
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
  return vec;
};

const parseVoyageEmbeddingsPayload = (payload: unknown): { vectors: number[][]; totalTokens: number | null } => {
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

  const data = payload.data as Record<string, unknown>[];
  const vectors: number[][] = [];
  for (const item of data) {
    const embedding = isRecord(item) ? item.embedding : null;
    vectors.push(parseVoyageEmbeddingVector(embedding));
  }
  return { vectors, totalTokens };
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
  beforeProviderDispatch?: NonNullable<UsageContext["beforeProviderDispatch"]>;
}): Promise<{ vectors: number[][]; totalTokens: number | null }> => {
  const controller = new AbortController();
  const signal = params.downstreamSignal ? AbortSignal.any([controller.signal, params.downstreamSignal]) : controller.signal;
  const now = Date.now();
  const timeoutMs = Math.max(1, Math.min(EMBEDDINGS_TIMEOUT_MS, params.deadlineMs - now));
  const timeout = setTimeout(() => {
    controller.abort();
  }, timeoutMs);
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
        Authorization: `Bearer ${params.apiKey}`,
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
      (err as { retry_after_ms?: number }).retry_after_ms = extractRetryAfterMs(resp.headers.get("Retry-After")) ?? undefined;
      throw err;
    }

    const payload = (await resp.json().catch(() => null)) as unknown;
    return parseVoyageEmbeddingsPayload(payload);
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

type NormalizationResult<T> = Readonly<{ ok: true; value: T }> | Readonly<{ ok: false; message: string; param: string }>;

type InputImageDetail = "auto" | "low" | "high" | "original";
type InputFileDetail = "auto" | "low" | "high";

const invalidNormalizedField = <T>(param: string, message: string): NormalizationResult<T> => ({
  ok: false,
  message,
  param,
});

const parseImageDetail = (value: unknown, param: string): NormalizationResult<InputImageDetail | null | undefined> => {
  if (value === undefined) return { ok: true, value: undefined };
  if (value === null) return { ok: true, value: null };
  if (value === "auto" || value === "low" || value === "high" || value === "original") {
    return { ok: true, value };
  }
  return invalidNormalizedField(param, `${param} must be one of auto, low, high, or original`);
};

const parseInputFileDetail = (value: unknown, param: string): NormalizationResult<InputFileDetail | undefined> => {
  if (value === undefined) return { ok: true, value: undefined };
  if (value === "auto" || value === "low" || value === "high") return { ok: true, value };
  return invalidNormalizedField(param, `${param} must be one of auto, low, or high`);
};

const findUnknownContentField = (value: Record<string, unknown>, allowed: readonly string[]): string | null => {
  const allowedFields = new Set(allowed);
  return Object.keys(value).find((key) => !allowedFields.has(key)) ?? null;
};

const normalizePromptCacheBreakpoint = (value: unknown, param: string): NormalizationResult<PromptCacheBreakpoint | undefined> => {
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
  if (Object.prototype.hasOwnProperty.call(rawRecord, "prompt_cache_key") && typeof rawRecord.prompt_cache_key !== "string") {
    return invalidNormalizedField("prompt_cache_key", "prompt_cache_key must be a string");
  }

  if (Object.prototype.hasOwnProperty.call(rawRecord, "prompt_cache_options")) {
    const options = rawRecord.prompt_cache_options;
    if (!isRecord(options) || Array.isArray(options)) {
      return invalidNormalizedField("prompt_cache_options", "prompt_cache_options must be an object");
    }
    const unknown = findUnknownContentField(options, ["mode", "ttl"]);
    if (unknown) {
      return invalidNormalizedField(`prompt_cache_options.${unknown}`, `Unknown prompt cache option: ${unknown}`);
    }
    if (options.mode !== undefined && options.mode !== "implicit" && options.mode !== "explicit") {
      return invalidNormalizedField("prompt_cache_options.mode", "prompt_cache_options.mode must be implicit or explicit");
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
    return invalidNormalizedField("prompt_cache_retention", "prompt_cache_retention must be in_memory or 24h");
  }

  return { ok: true, value: undefined };
};

const withPromptCacheBreakpoint = <T extends object>(
  item: T,
  breakpoint: PromptCacheBreakpoint | undefined
): T & { prompt_cache_breakpoint?: PromptCacheBreakpoint } => (breakpoint === undefined ? item : { ...item, prompt_cache_breakpoint: breakpoint });

const normalizeChatTextContentPart = (
  part: Record<string, unknown>,
  partParam: string,
  isAssistant: boolean,
  textItemType: "input_text" | "output_text"
): NormalizationResult<MessageContentItem> => {
  const unknown = findUnknownContentField(part, ["type", "text", "prompt_cache_breakpoint"]);
  if (unknown) return invalidNormalizedField(`${partParam}.${unknown}`, `Unknown content field: ${unknown}`);
  if (typeof part.text !== "string") {
    return invalidNormalizedField(`${partParam}.text`, `${partParam}.text must be a string`);
  }
  if (isAssistant && part.prompt_cache_breakpoint !== undefined) {
    return invalidNormalizedField(
      `${partParam}.prompt_cache_breakpoint`,
      "prompt_cache_breakpoint is not supported for assistant output content in this gateway"
    );
  }
  const breakpoint = normalizePromptCacheBreakpoint(part.prompt_cache_breakpoint, `${partParam}.prompt_cache_breakpoint`);
  if (!breakpoint.ok) return breakpoint;
  if (textItemType === "input_text") {
    return { ok: true, value: withPromptCacheBreakpoint({ type: "input_text", text: part.text }, breakpoint.value) };
  }
  return { ok: true, value: { type: "output_text", text: part.text } };
};

const normalizeChatRefusalContentPart = (
  part: Record<string, unknown>,
  partParam: string,
  isAssistant: boolean,
  partCount: number
): NormalizationResult<MessageContentItem> => {
  const unknown = findUnknownContentField(part, ["type", "refusal", "prompt_cache_breakpoint"]);
  if (unknown) return invalidNormalizedField(`${partParam}.${unknown}`, `Unknown content field: ${unknown}`);
  if (!isAssistant) {
    return invalidNormalizedField(`${partParam}.type`, `${partParam}.type is only valid for assistant messages`);
  }
  if (partCount !== 1) {
    return invalidNormalizedField(`${partParam}.type`, "assistant refusal content must be the only part");
  }
  if (typeof part.refusal !== "string") {
    return invalidNormalizedField(`${partParam}.refusal`, `${partParam}.refusal must be a string`);
  }
  if (part.prompt_cache_breakpoint !== undefined) {
    return invalidNormalizedField(`${partParam}.prompt_cache_breakpoint`, "prompt_cache_breakpoint is not supported for refusal content in this gateway");
  }
  return { ok: true, value: { type: "output_text", text: part.refusal } };
};

const normalizeChatImageContentPart = (
  part: Record<string, unknown>,
  partParam: string,
  role: ResponseMessageItem["role"]
): NormalizationResult<MessageContentItem> => {
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
    return invalidNormalizedField(`${partParam}.image_url.${imageUnknown}`, `Unknown image_url field: ${imageUnknown}`);
  }
  if (typeof image.url !== "string" || !image.url.trim()) {
    return invalidNormalizedField(`${partParam}.image_url.url`, `${partParam}.image_url.url must contain a URL`);
  }
  const detail = parseImageDetail(image.detail, `${partParam}.image_url.detail`);
  if (!detail.ok) return detail;
  const breakpoint = normalizePromptCacheBreakpoint(part.prompt_cache_breakpoint, `${partParam}.prompt_cache_breakpoint`);
  if (!breakpoint.ok) return breakpoint;
  const item: Extract<MessageContentItem, { type: "input_image" }> =
    detail.value === undefined
      ? { type: "input_image", image_url: image.url.trim() }
      : { type: "input_image", image_url: image.url.trim(), detail: detail.value };
  return { ok: true, value: withPromptCacheBreakpoint(item, breakpoint.value) };
};

const chatFileContentItem = (
  fileId: string | undefined,
  fileData: string | undefined,
  filename: string | undefined
): Extract<MessageContentItem, { type: "input_file" }> => ({
  type: "input_file",
  ...(fileId === undefined ? {} : { file_id: fileId }),
  ...(fileData === undefined ? {} : { file_data: fileData }),
  ...(filename === undefined ? {} : { filename }),
});

const normalizeChatFileContentPart = (
  part: Record<string, unknown>,
  partParam: string,
  role: ResponseMessageItem["role"]
): NormalizationResult<MessageContentItem> => {
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
  for (const field of ["file_id", "file_data", "filename"] as const) {
    if (file[field] !== undefined && typeof file[field] !== "string") {
      return invalidNormalizedField(`${partParam}.file.${field}`, `${partParam}.file.${field} must be a string`);
    }
  }
  const fileId = getString(file.file_id) ?? undefined;
  const fileData = getString(file.file_data) ?? undefined;
  if (!fileId?.trim() && !fileData?.trim()) {
    return invalidNormalizedField(`${partParam}.file.file_id`, `${partParam}.file must include file_id or file_data`);
  }
  const breakpoint = normalizePromptCacheBreakpoint(part.prompt_cache_breakpoint, `${partParam}.prompt_cache_breakpoint`);
  if (!breakpoint.ok) return breakpoint;
  const filename = typeof file.filename === "string" ? file.filename : undefined;
  const item = chatFileContentItem(fileId, fileData, filename);
  return { ok: true, value: withPromptCacheBreakpoint(item, breakpoint.value) };
};

const normalizeChatContentPart = (
  part: Record<string, unknown>,
  partParam: string,
  role: ResponseMessageItem["role"],
  isAssistant: boolean,
  textItemType: "input_text" | "output_text",
  partCount: number
): NormalizationResult<MessageContentItem> => {
  const partType = getString(part.type);
  if (partType === "text") return normalizeChatTextContentPart(part, partParam, isAssistant, textItemType);
  if (partType === "refusal") return normalizeChatRefusalContentPart(part, partParam, isAssistant, partCount);
  if (partType === "image_url") return normalizeChatImageContentPart(part, partParam, role);
  if (partType === "file") return normalizeChatFileContentPart(part, partParam, role);
  if (partType === "input_audio" && Object.prototype.hasOwnProperty.call(part, "prompt_cache_breakpoint")) {
    return invalidNormalizedField(`${partParam}.prompt_cache_breakpoint`, "prompt_cache_breakpoint is not supported for input_audio content in this gateway");
  }
  return invalidNormalizedField(`${partParam}.type`, `${partParam}.type is not supported`);
};

const normalizeChatContentItems = (role: ResponseMessageItem["role"], content: unknown, param: string): NormalizationResult<MessageContentItem[]> => {
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
    const normalized = normalizeChatContentPart(part, partParam, role, isAssistant, textItemType, content.length);
    if (!normalized.ok) return normalized;
    items.push(normalized.value);
  }
  return { ok: true, value: items };
};

const messageContentToText = (items: MessageContentItem[]): string =>
  items
    .filter((item) => item.type === "input_text" || item.type === "output_text")
    .map((item) => item.text)
    .filter((text) => text.trim())
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
  const fallbackCreated =
    typeof payload.updated_at_ms === "number" && Number.isFinite(payload.updated_at_ms) ? Math.max(0, Math.trunc(payload.updated_at_ms / 1000)) : 0;
  const data = Array.isArray(payload.data) ? payload.data : null;
  if (data) {
    const normalized = data.map((entry) => normalizeModelEntry(entry, fallbackCreated)).filter(Boolean) as Record<string, unknown>[];
    return { object: "list", data: normalized };
  }
  const models = Array.isArray(payload.models) ? payload.models : null;
  if (models) {
    const normalized = models.map((entry) => normalizeModelEntry(entry, fallbackCreated)).filter(Boolean) as Record<string, unknown>[];
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

const withConfiguredCerebrasModel = (models: readonly Record<string, unknown>[]): Record<string, unknown>[] => {
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
  const autoCompactTokenLimit =
    resolvedContext?.auto_compact_token_limit_tokens ??
    (nativeAutoCompactTokenLimit !== null && (contextWindow === null || nativeAutoCompactTokenLimit <= contextWindow) ? nativeAutoCompactTokenLimit : null);
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

const normalizeResponseTextContentItem = (
  value: Record<string, unknown>,
  partType: "input_text" | "output_text",
  param: string,
  role: ResponseMessageItem["role"]
): NormalizationResult<MessageContentItem> => {
  if (partType === "output_text" && role !== "assistant") {
    return invalidNormalizedField(`${param}.type`, `${param}.type is only valid for assistant messages`);
  }
  const unknown = findUnknownContentField(value, partType === "output_text" ? ["type", "text", "annotations"] : ["type", "text", "prompt_cache_breakpoint"]);
  if (unknown) return invalidNormalizedField(`${param}.${unknown}`, `Unknown content field: ${unknown}`);
  if (typeof value.text !== "string") {
    return invalidNormalizedField(`${param}.text`, `${param}.text must be a string`);
  }
  if (partType === "output_text" && value.annotations !== undefined && !Array.isArray(value.annotations)) {
    return invalidNormalizedField(`${param}.annotations`, `${param}.annotations must be an array`);
  }
  if (partType === "output_text") return { ok: true, value: { type: partType, text: value.text } };
  const breakpoint = normalizePromptCacheBreakpoint(value.prompt_cache_breakpoint, `${param}.prompt_cache_breakpoint`);
  if (!breakpoint.ok) return breakpoint;
  return {
    ok: true,
    value: withPromptCacheBreakpoint({ type: "input_text", text: value.text }, breakpoint.value),
  };
};

const normalizeResponseImageContentItem = (value: Record<string, unknown>, param: string): NormalizationResult<MessageContentItem> => {
  const unknown = findUnknownContentField(value, ["type", "image_url", "file_id", "detail", "prompt_cache_breakpoint"]);
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
  const breakpoint = normalizePromptCacheBreakpoint(value.prompt_cache_breakpoint, `${param}.prompt_cache_breakpoint`);
  if (!breakpoint.ok) return breakpoint;
  const item: Extract<MessageContentItem, { type: "input_image" }> = imageUrl
    ? { type: "input_image", image_url: imageUrl }
    : { type: "input_image", file_id: fileId };
  if (detail.value === undefined) return { ok: true, value: withPromptCacheBreakpoint(item, breakpoint.value) };
  return { ok: true, value: withPromptCacheBreakpoint({ ...item, detail: detail.value }, breakpoint.value) };
};

const responseFileContentItem = (value: Record<string, unknown>, detail: InputFileDetail | undefined): Extract<MessageContentItem, { type: "input_file" }> => {
  const item: {
    type: "input_file";
    file_id?: string;
    file_data?: string;
    file_url?: string;
    filename?: string | null;
    detail?: InputFileDetail;
  } = { type: "input_file" };
  for (const field of ["file_id", "file_data", "file_url"] as const) {
    const fieldValue = getString(value[field]);
    if (fieldValue) item[field] = fieldValue;
  }
  if (Object.prototype.hasOwnProperty.call(value, "filename")) item.filename = value.filename as string | null;
  if (detail !== undefined) item.detail = detail;
  return item;
};

const normalizeResponseFileContentItem = (value: Record<string, unknown>, param: string): NormalizationResult<MessageContentItem> => {
  const unknown = findUnknownContentField(value, ["type", "file_id", "file_data", "file_url", "filename", "detail", "prompt_cache_breakpoint"]);
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
  const breakpoint = normalizePromptCacheBreakpoint(value.prompt_cache_breakpoint, `${param}.prompt_cache_breakpoint`);
  if (!breakpoint.ok) return breakpoint;
  return { ok: true, value: withPromptCacheBreakpoint(responseFileContentItem(value, detail.value), breakpoint.value) };
};

const normalizeResponseContentItem = (value: unknown, param: string, role: ResponseMessageItem["role"]): NormalizationResult<MessageContentItem> => {
  if (!isRecord(value) || Array.isArray(value)) {
    return invalidNormalizedField(param, `${param} must be an object`);
  }
  const partType = getString(value.type);
  if (!partType) return invalidNormalizedField(`${param}.type`, `${param}.type must be a string`);

  if (partType === "input_text" || partType === "output_text") return normalizeResponseTextContentItem(value, partType, param, role);
  if (partType === "input_image") return normalizeResponseImageContentItem(value, param);
  if (partType === "input_file") return normalizeResponseFileContentItem(value, param);
  return invalidNormalizedField(`${param}.type`, `${param}.type is not supported`);
};

const normalizeResponseMessageItem = (value: unknown, param: string): NormalizationResult<ResponseMessageItem> => {
  if (!isRecord(value) || Array.isArray(value)) return invalidNormalizedField(param, `${param} must be an object`);
  if (Object.prototype.hasOwnProperty.call(value, "prompt_cache_breakpoint")) {
    return invalidNormalizedField(`${param}.prompt_cache_breakpoint`, "prompt_cache_breakpoint is only valid on supported input content blocks");
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
const normalizeFunctionCallOutputItem = (value: Record<string, unknown>, param: string): NormalizationResult<ResponseInputItem> => {
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

const normalizeChatToolCall = (value: unknown, param: string): NormalizationResult<Readonly<Record<string, unknown> & { type: "function_call" }>> => {
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
    return invalidNormalizedField(`${param}.function.${unknownFunctionField}`, `Unknown tool call function field: ${unknownFunctionField}`);
  }
  const name = getString(value.function.name)?.trim();
  if (!name) {
    return invalidNormalizedField(`${param}.function.name`, `${param}.function.name must be a non-empty string`);
  }
  if (typeof value.function.arguments !== "string") {
    return invalidNormalizedField(`${param}.function.arguments`, `${param}.function.arguments must be a string`);
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

const normalizeChatToolOutput = (value: unknown, param: string): NormalizationResult<string | Extract<MessageContentItem, { type: "input_text" }>[]> => {
  if (typeof value === "string") return { ok: true, value };
  if (!Array.isArray(value)) return invalidNormalizedField(param, `${param} must be a string or an array`);
  const output: Extract<MessageContentItem, { type: "input_text" }>[] = [];
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
    const breakpoint = normalizePromptCacheBreakpoint(part.prompt_cache_breakpoint, `${partParam}.prompt_cache_breakpoint`);
    if (!breakpoint.ok) return breakpoint;
    output.push(withPromptCacheBreakpoint({ type: "input_text", text: part.text }, breakpoint.value));
  }
  return { ok: true, value: output };
};

const normalizeChatToolMessage = (
  value: Record<string, unknown>,
  param: string
): NormalizationResult<Readonly<{ instruction: string | null; instructionContent: MessageContentItem[] | null; input: ResponseInputItem[] }>> => {
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
};

const appendChatToolCallInputs = (input: ResponseInputItem[], toolCalls: unknown, param: string): NormalizationResult<void> => {
  if (!Array.isArray(toolCalls)) {
    return invalidNormalizedField(`${param}.tool_calls`, `${param}.tool_calls must be an array`);
  }
  for (const [callIndex, call] of toolCalls.entries()) {
    const normalized = normalizeChatToolCall(call, `${param}.tool_calls[${callIndex}]`);
    if (!normalized.ok) return normalized;
    input.push(normalized.value);
  }
  return { ok: true, value: undefined };
};

const normalizeChatAssistantMessage = (
  value: Record<string, unknown>,
  param: string,
  role: ResponseMessageItem["role"]
): NormalizationResult<Readonly<{ instruction: string | null; instructionContent: MessageContentItem[] | null; input: ResponseInputItem[] }>> => {
  const hasToolCalls = Object.prototype.hasOwnProperty.call(value, "tool_calls");
  const refusal = value.refusal === undefined || value.refusal === null ? null : getString(value.refusal);
  if (refusal === null && value.refusal !== undefined && value.refusal !== null) {
    return invalidNormalizedField(`${param}.refusal`, `${param}.refusal must be a string or null`);
  }
  // Chat permits an omitted assistant content field when the message is
  // solely a function-call turn. Normalize it as the same empty content as
  // the explicit null form, but keep missing content invalid otherwise.
  const content =
    value.content === undefined && hasToolCalls
      ? { ok: true as const, value: [] as MessageContentItem[] }
      : normalizeChatContentItems(role, value.content, `${param}.content`);
  if (!content.ok) return content;
  const input: ResponseInputItem[] = [];
  // A Chat assistant's natural-language output must precede its function
  // calls so a multi-turn tool conversation retains the original order.
  const messageContent = refusal === null ? content.value : [...content.value, { type: "output_text" as const, text: refusal }];
  if (messageContent.length) input.push({ type: "message", role, content: messageContent });
  if (hasToolCalls) {
    const toolCalls = appendChatToolCallInputs(input, value.tool_calls, param);
    if (!toolCalls.ok) return toolCalls;
  }
  if (!input.length) {
    return invalidNormalizedField(`${param}.content`, "assistant messages require content, refusal, or tool_calls");
  }
  return { ok: true, value: { instruction: null, instructionContent: null, input } };
};

const normalizeChatPlainMessage = (
  value: Record<string, unknown>,
  param: string,
  role: ResponseMessageItem["role"],
  roleRaw: string
): NormalizationResult<Readonly<{ instruction: string | null; instructionContent: MessageContentItem[] | null; input: ResponseInputItem[] }>> => {
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

const normalizeChatMessage = (
  value: unknown,
  index: number
): NormalizationResult<Readonly<{ instruction: string | null; instructionContent: MessageContentItem[] | null; input: ResponseInputItem[] }>> => {
  const param = `messages[${index}]`;
  if (!isRecord(value) || Array.isArray(value)) return invalidNormalizedField(param, `${param} must be an object`);
  if (Object.prototype.hasOwnProperty.call(value, "prompt_cache_breakpoint")) {
    return invalidNormalizedField(`${param}.prompt_cache_breakpoint`, "prompt_cache_breakpoint is only valid on supported input content blocks");
  }
  const roleRaw = getString(value.role);
  if (!roleRaw) return invalidNormalizedField(`${param}.role`, `${param}.role must be a string`);
  const role = chatRoleToCodexRole(roleRaw);
  if (!role) return invalidNormalizedField(`${param}.role`, `${param}.role is not supported`);

  if (roleRaw === "tool") return normalizeChatToolMessage(value, param);

  if (Object.prototype.hasOwnProperty.call(value, "tool_call_id")) {
    return invalidNormalizedField(`${param}.tool_call_id`, "tool_call_id is only valid for tool messages");
  }
  if (roleRaw === "assistant") return normalizeChatAssistantMessage(value, param, role);
  return normalizeChatPlainMessage(value, param, role, roleRaw);
};

const recordResponsesTerminal = (event: ResponsesStreamEvent, usageContext?: UsageContext): void => {
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
  return output
    .slice(startIndex)
    .some(
      (item) =>
        isRecord(item) &&
        Array.isArray(item.content) &&
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

const chatOutputTextPartIndexText = (value: unknown): string => (typeof value === "number" || typeof value === "string" ? String(value) : "0");

const chatOutputTextPartKey = (event: Record<string, unknown>): string => {
  const itemId = getString(event.item_id)?.trim();
  if (itemId) return `item:${itemId}:${chatOutputTextPartIndexText(event.content_index ?? 0)}`;
  return `output:${chatOutputTextPartIndexText(event.output_index ?? 0)}:${chatOutputTextPartIndexText(event.content_index ?? 0)}`;
};

type ReconciledChatContent = Readonly<{ outputText: string; refusal: string }>;

const reconcileChatContentPart = (
  outputTextParts: Map<string, string>,
  refusalParts: Map<string, string>,
  event: Record<string, unknown>,
  part: unknown
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
  item: unknown
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
  output: unknown
): ReconciledChatContent => {
  if (!Array.isArray(output)) return { outputText: "", refusal: "" };
  let outputText = "";
  let refusal = "";
  for (const [outputIndex, item] of output.entries()) {
    const reconciled = reconcileChatOutputItemContent(outputTextParts, refusalParts, { ...event, output_index: outputIndex }, item);
    outputText += reconciled.outputText;
    refusal += reconciled.refusal;
  }
  return { outputText, refusal };
};

const withAccumulatedResponseText = (response: Record<string, unknown>, text: string, ignoredOutputPrefix = 0): Record<string, unknown> => {
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

const withAccumulatedResponseRefusal = (response: Record<string, unknown>, refusal: string, ignoredOutputPrefix = 0): Record<string, unknown> => {
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

const withAccumulatedResponseItems = (response: Record<string, unknown>, accumulated: Record<string, unknown>[]): Record<string, unknown> => {
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
      return malformedFunctionCallStream("Upstream function-call stream ended before finalized arguments were received.");
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

  add(event: Record<string, unknown>, item: unknown): Readonly<{ call: ChatFunctionCall; includeIdentity: boolean; suffix: string }> | null {
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

  reconcileItem(event: Record<string, unknown>, item: unknown): Readonly<{ call: ChatFunctionCall; suffix: string }> | null {
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

  reconcileOutput(event: Record<string, unknown>, output: unknown): Readonly<{ call: ChatFunctionCall; suffix: string }>[] {
    if (!Array.isArray(output)) return [];
    const reconciled: Readonly<{ call: ChatFunctionCall; suffix: string }>[] = [];
    for (const item of output) {
      const result = this.reconcileItem(event, item);
      if (result) reconciled.push(result);
    }
    return reconciled;
  }
}

const applyPreparedChatEvent = (
  event: ResponsesStreamEvent,
  state: {
    outputText: string;
    refusal: string;
    outputTextParts: Map<string, string>;
    refusalParts: Map<string, string>;
    functionCalls: ChatFunctionCallAccumulator;
    completed: boolean;
  }
): string | null => {
  const ev = event.value;
  switch (event.type) {
    case "response.output_text.delta": {
      const delta = getString(ev.delta);
      if (delta === null) return "Upstream output-text delta is not a string.";
      const key = chatOutputTextPartKey(ev);
      state.outputTextParts.set(key, `${state.outputTextParts.get(key) ?? ""}${delta}`);
      state.outputText += delta;
      break;
    }
    case "response.output_text.done": {
      const completedText = getString(ev.text);
      if (completedText === null) {
        return "Upstream completed output text is not a string.";
      }
      const key = chatOutputTextPartKey(ev);
      const partText = state.outputTextParts.get(key) ?? "";
      const suffix = reconcileCompletedOutputText(partText, completedText);
      state.outputTextParts.set(key, `${partText}${suffix}`);
      state.outputText += suffix;
      break;
    }
    case "response.refusal.delta": {
      const delta = getString(ev.delta);
      if (delta === null) return "Upstream refusal delta is not a string.";
      const key = chatOutputTextPartKey(ev);
      state.refusalParts.set(key, `${state.refusalParts.get(key) ?? ""}${delta}`);
      state.refusal += delta;
      break;
    }
    case "response.refusal.done": {
      const completedRefusal = getString(ev.refusal);
      if (completedRefusal === null) {
        return "Upstream completed refusal is not a string.";
      }
      const key = chatOutputTextPartKey(ev);
      const partRefusal = state.refusalParts.get(key) ?? "";
      const suffix = reconcileCompletedRefusal(partRefusal, completedRefusal);
      state.refusalParts.set(key, `${partRefusal}${suffix}`);
      state.refusal += suffix;
      break;
    }
    case "response.content_part.done": {
      const reconciled = reconcileChatContentPart(state.outputTextParts, state.refusalParts, ev, ev.part);
      state.outputText += reconciled.outputText;
      state.refusal += reconciled.refusal;
      break;
    }
    case "response.output_item.added":
      state.functionCalls.add(ev, ev.item);
      break;
    case "response.function_call_arguments.delta":
      state.functionCalls.delta(ev);
      break;
    case "response.function_call_arguments.done":
      state.functionCalls.done(ev);
      break;
    case "response.output_item.done": {
      const functionCall = state.functionCalls.reconcileItem(ev, ev.item);
      if (!functionCall) {
        const reconciled = reconcileChatOutputItemContent(state.outputTextParts, state.refusalParts, ev, ev.item);
        state.outputText += reconciled.outputText;
        state.refusal += reconciled.refusal;
      }
      break;
    }
    case "response.output": {
      const output = ev.output ?? (isRecord(ev.response) ? ev.response.output : undefined);
      const reconciled = reconcileChatResponseOutputContent(state.outputTextParts, state.refusalParts, ev, output);
      state.outputText += reconciled.outputText;
      state.refusal += reconciled.refusal;
      state.functionCalls.reconcileOutput(ev, output);
      break;
    }
    case "response.completed": {
      if (!isRecord(ev.response) || Array.isArray(ev.response)) {
        return "Upstream response.completed event is missing its response object.";
      }
      const reconciled = reconcileChatResponseOutputContent(state.outputTextParts, state.refusalParts, ev, ev.response.output);
      state.outputText += reconciled.outputText;
      state.refusal += reconciled.refusal;
      state.functionCalls.reconcileOutput(ev, ev.response.output);
      state.functionCalls.assertFinalized();
      state.completed = true;
      break;
    }
    default:
      break;
  }
  return null;
};

const preparedChatCompletionIsEmpty = (prepared: PreparedResponsesStream): boolean => {
  const state = {
    outputText: "",
    refusal: "",
    outputTextParts: new Map<string, string>(),
    refusalParts: new Map<string, string>(),
    functionCalls: new ChatFunctionCallAccumulator(),
    completed: false,
  };

  for (const event of prepared.buffered) {
    const malformedMessage = applyPreparedChatEvent(event, state);
    if (malformedMessage !== null) return malformedFunctionCallStream(malformedMessage);
  }

  if (!state.completed) {
    return malformedFunctionCallStream("Chat semantic preflight did not retain a completed terminal.");
  }
  return !state.outputText && !state.refusal && !state.functionCalls.hasCalls;
};

const chatToolCallDelta = (
  call: ChatFunctionCall,
  options: Readonly<{ includeIdentity: boolean; argumentsDelta?: string }> = { includeIdentity: false }
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

const chatSourceFromPrepared = (source: PreflightedResponsesStream, prepared: PreparedResponsesStream): PreflightedResponsesStream => {
  const first = prepared.buffered.at(0);
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

const markFinalizedChatToolOutput = (context: UsageContext | undefined, functionCalls: ChatFunctionCallAccumulator): void => {
  if (functionCalls.calls.some((call) => call.argumentsDone)) markChatSemanticOutput(context);
};

const translatedChatOutputObserved = (outputText: string, refusal: string, functionCalls: ChatFunctionCallAccumulator): boolean =>
  outputText.length > 0 || refusal.length > 0 || functionCalls.hasCalls;

const recordSuccessfulChatCompletion = async (
  context: UsageContext | undefined,
  lifecycle: MeteredTransportLifecycle,
  usage: UsageTokens | null,
  onResponseTerminal?: (terminalType: ResponseStreamTerminalType) => void
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
  onResponseTerminal?: (terminalType: ResponseStreamTerminalType) => void
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
  onResponseTerminal?: (terminalType: ResponseStreamTerminalType) => void
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
  const queuedDeltas: (
    | Readonly<{ kind: "content"; content: string }>
    | Readonly<{ kind: "refusal"; refusal: string }>
    | Readonly<{
        kind: "tool";
        call: ChatFunctionCall;
        includeIdentity: boolean;
        argumentsDelta: string;
      }>
  )[] = [];
  const settleInitialTerminalOnCancel = async (): Promise<void> => {
    const event = source.first;
    if (terminalSettled || !event.terminal) return;
    recordResponsesEventTelemetry(usageContext, event);
    const ev = event.value;
    const usageTokens = isRecord(ev.response) ? extractUsageTokens(ev.response.usage) : null;
    if (event.type === "response.completed") {
      if (!isRecord(ev.response) || Array.isArray(ev.response)) {
        const error = new ResponsesStreamError("Upstream response.completed event is missing its response object.", { kind: "malformed_event" });
        recordResponsesFailureTelemetry(usageContext, error);
        onResponseTerminal?.("error");
        lifecycle.ambiguous();
        recordStreamTerminalType(usageContext, "error");
        terminalSettled = true;
        return;
      }
      try {
        const completed = reconcileChatResponseOutputContent(outputTextParts, refusalParts, ev, ev.response.output);
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
      const emitToolCall = (call: ChatFunctionCall, includeIdentity: boolean, argumentsDelta: string | undefined): void => {
        const toolCall = chatToolCallDelta(call, { includeIdentity, argumentsDelta });
        const delta: Record<string, unknown> = sentRole ? { tool_calls: [toolCall] } : { role: "assistant", tool_calls: [toolCall] };
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
      const emitNextQueuedDelta = (): boolean => {
        const queued = queuedDeltas.shift();
        if (!queued) return false;
        if (queued.kind === "content") emitContent(queued.content);
        else if (queued.kind === "refusal") emitRefusal(queued.refusal);
        else emitToolCall(queued.call, queued.includeIdentity, queued.argumentsDelta);
        return true;
      };
      const applyOutputTextDelta = (ev: Record<string, unknown>): void => {
        const delta = getString(ev.delta);
        if (delta === null) {
          return malformedFunctionCallStream("Upstream output-text delta is not a string.");
        }
        const key = chatOutputTextPartKey(ev);
        outputTextParts.set(key, `${outputTextParts.get(key) ?? ""}${delta}`);
        outputText += delta;
        emitContent(delta);
      };
      const applyOutputTextDone = (ev: Record<string, unknown>): "next" | "return" => {
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
          return "return";
        }
        return "next";
      };
      const applyRefusalDelta = (ev: Record<string, unknown>): void => {
        const delta = getString(ev.delta);
        if (delta === null) return malformedFunctionCallStream("Upstream refusal delta is not a string.");
        const key = chatOutputTextPartKey(ev);
        refusalParts.set(key, `${refusalParts.get(key) ?? ""}${delta}`);
        refusal += delta;
        emitRefusal(delta);
      };
      const applyRefusalDone = (ev: Record<string, unknown>): "next" | "return" => {
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
          return "return";
        }
        return "next";
      };
      const applyContentPartDone = (ev: Record<string, unknown>): "next" | "return" => {
        const reconciled = reconcileChatContentPart(outputTextParts, refusalParts, ev, ev.part);
        if (reconciled.outputText) {
          outputText += reconciled.outputText;
          emitContent(reconciled.outputText);
          return "return";
        }
        if (reconciled.refusal) {
          refusal += reconciled.refusal;
          emitRefusal(reconciled.refusal);
          return "return";
        }
        return "next";
      };
      const handleTextEvent = (event: ResponsesStreamEvent): "next" | "return" | "unhandled" => {
        const ev = event.value;
        const type = event.type;
        if (type === "response.output_text.delta") {
          applyOutputTextDelta(ev);
          return "return";
        }
        if (type === "response.output_text.done") return applyOutputTextDone(ev);
        if (type === "response.refusal.delta") {
          applyRefusalDelta(ev);
          return "return";
        }
        if (type === "response.refusal.done") return applyRefusalDone(ev);
        if (type === "response.content_part.done") return applyContentPartDone(ev);
        return "unhandled";
      };
      const handleOutputItemAdded = (ev: Record<string, unknown>): "next" | "return" => {
        const added = functionCalls.add(ev, ev.item);
        if (added && (added.includeIdentity || added.suffix)) {
          emitToolCall(added.call, added.includeIdentity, added.suffix);
          return "return";
        }
        return "next";
      };
      const handleOutputItemDone = (event: ResponsesStreamEvent): "next" | "return" => {
        const ev = event.value;
        const wasKnown = isRecord(ev.item) && !Array.isArray(ev.item) && functionCalls.has(ev, ev.item);
        const reconciled = functionCalls.reconcileItem(ev, ev.item);
        if (reconciled) {
          markFinalizedChatToolOutput(usageContext, functionCalls);
          if (!wasKnown || reconciled.suffix) {
            emitToolCall(reconciled.call, !wasKnown, reconciled.suffix);
            return "return";
          }
        } else {
          const completed = reconcileChatOutputItemContent(outputTextParts, refusalParts, ev, ev.item);
          outputText += completed.outputText;
          refusal += completed.refusal;
          if (completed.outputText) queuedDeltas.push({ kind: "content", content: completed.outputText });
          if (completed.refusal) queuedDeltas.push({ kind: "refusal", refusal: completed.refusal });
          if (emitNextQueuedDelta()) return "return";
        }
        return "next";
      };
      const handleToolEvent = (event: ResponsesStreamEvent): "next" | "return" | "unhandled" => {
        const ev = event.value;
        const type = event.type;
        if (type === "response.output_item.added") return handleOutputItemAdded(ev);
        if (type === "response.function_call_arguments.delta") {
          const { call, delta } = functionCalls.delta(ev);
          emitToolCall(call, false, delta);
          return "return";
        }
        if (type === "response.function_call_arguments.done") {
          const { call, suffix } = functionCalls.done(ev);
          markFinalizedChatToolOutput(usageContext, functionCalls);
          if (suffix) {
            emitToolCall(call, false, suffix);
            return "return";
          }
          return "next";
        }
        if (type === "response.output_item.done") return handleOutputItemDone(event);
        return "unhandled";
      };
      const handleFinalOutputEvent = (event: ResponsesStreamEvent): "next" | "return" => {
        const ev = event.value;
        const output = ev.output ?? (isRecord(ev.response) ? ev.response.output : undefined);
        queueFinalOutput(ev, output);
        if (queuedDeltas.length) {
          pending = event;
          emitNextQueuedDelta();
          return "return";
        }
        return "next";
      };
      const handleCompletedEvent = async (event: ResponsesStreamEvent): Promise<void> => {
        const ev = event.value;
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
          emitNextQueuedDelta();
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
          controller.enqueue(encoder.encode(`data: ${JSON.stringify({ id, object: "chat.completion.chunk", created, model, choices: [], usage })}\n\n`));
        }
        controller.enqueue(encoder.encode("data: [DONE]\n\n"));
        closed = true;
        controller.close();
        void iterator.return("Responses terminal event translated").catch(() => {});
      };
      const handleTerminalEvent = (event: ResponsesStreamEvent): void => {
        const ev = event.value;
        const type = event.type;
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
      };
      const handleStreamEvent = async (event: ResponsesStreamEvent): Promise<"next" | "return"> => {
        const ev = event.value;
        const type = event.type;
        if (type === "response.created" && isRecord(ev.response)) {
          const upstreamId = getString(ev.response.id);
          const createdAt = typeof ev.response.created_at === "number" ? ev.response.created_at : null;
          if (upstreamId) id = upstreamId;
          if (createdAt) created = createdAt;
          return "next";
        }
        const textOutcome = handleTextEvent(event);
        if (textOutcome !== "unhandled") return textOutcome;
        const toolOutcome = handleToolEvent(event);
        if (toolOutcome !== "unhandled") return toolOutcome;
        if (type === "response.output") return handleFinalOutputEvent(event);
        if (type === "response.completed") {
          await handleCompletedEvent(event);
          return "return";
        }
        if (event.terminal) {
          handleTerminalEvent(event);
          return "return";
        }
        return "next";
      };
      const pumpStream = async (): Promise<void> => {
        if (emitNextQueuedDelta()) return;
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
          const outcome = await handleStreamEvent(event);
          if (outcome === "return") return;
        }
      };
      const settleStreamFailure = async (error: unknown): Promise<void> => {
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
      };

      try {
        await pumpStream();
      } catch (error) {
        await settleStreamFailure(error);
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
  onResponseTerminal?: (terminalType: ResponseStreamTerminalType) => void
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
  let emptyCompletion = false;

  const applyTerminalEvent = (event: ResponsesStreamEvent): void => {
    const ev = event.value;
    const type = event.type;
    terminalType = type as ResponseStreamTerminalType;
    const terminalUsage = isRecord(ev.response) ? extractUsageTokens(ev.response.usage) : null;
    onResponseTerminal?.(type as ResponseStreamTerminalType);
    lifecycle.terminal(type, terminalUsage);
    recordStreamTerminalType(usageContext, type as ResponseStreamTerminalType);
    recordTerminalUsage(usageContext, terminalUsage, false);
  };

  const applyCreatedEvent = (event: ResponsesStreamEvent): boolean => {
    const ev = event.value;
    if (event.type !== "response.created" || !isRecord(ev.response)) return false;
    const upstreamId = getString(ev.response.id);
    const createdAt = typeof ev.response.created_at === "number" ? ev.response.created_at : null;
    if (upstreamId) id = upstreamId;
    if (createdAt) created = createdAt;
    return true;
  };

  const applyOutputTextDelta = (ev: Record<string, unknown>): void => {
    const delta = getString(ev.delta);
    if (delta === null) {
      return malformedFunctionCallStream("Upstream output-text delta is not a string.");
    }
    const key = chatOutputTextPartKey(ev);
    outputTextParts.set(key, `${outputTextParts.get(key) ?? ""}${delta}`);
    content += delta;
    if (delta.length > 0) markChatSemanticOutput(usageContext);
  };

  const applyOutputTextDone = (ev: Record<string, unknown>): void => {
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
  };

  const applyRefusalDelta = (ev: Record<string, unknown>): void => {
    const delta = getString(ev.delta);
    if (delta === null) return malformedFunctionCallStream("Upstream refusal delta is not a string.");
    const key = chatOutputTextPartKey(ev);
    refusalParts.set(key, `${refusalParts.get(key) ?? ""}${delta}`);
    refusal += delta;
    if (delta.length > 0) markChatSemanticOutput(usageContext);
  };

  const applyRefusalDone = (ev: Record<string, unknown>): void => {
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
  };

  const applyContentPartDone = (ev: Record<string, unknown>): void => {
    const reconciled = reconcileChatContentPart(outputTextParts, refusalParts, ev, ev.part);
    content += reconciled.outputText;
    refusal += reconciled.refusal;
    if (reconciled.outputText.length > 0 || reconciled.refusal.length > 0) {
      markChatSemanticOutput(usageContext);
    }
  };

  const applyTextEvent = (event: ResponsesStreamEvent): boolean => {
    const ev = event.value;
    const type = event.type;
    if (type === "response.output_text.delta") {
      applyOutputTextDelta(ev);
      return true;
    }
    if (type === "response.output_text.done") {
      applyOutputTextDone(ev);
      return true;
    }
    if (type === "response.refusal.delta") {
      applyRefusalDelta(ev);
      return true;
    }
    if (type === "response.refusal.done") {
      applyRefusalDone(ev);
      return true;
    }
    if (type === "response.content_part.done") {
      applyContentPartDone(ev);
      return true;
    }
    return false;
  };

  const applyOutputItemDone = (ev: Record<string, unknown>): void => {
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
  };

  const applyResponseOutput = (ev: Record<string, unknown>): void => {
    const output = ev.output ?? (isRecord(ev.response) ? ev.response.output : undefined);
    const completed = reconcileChatResponseOutputContent(outputTextParts, refusalParts, ev, output);
    content += completed.outputText;
    refusal += completed.refusal;
    const reconciled = functionCalls.reconcileOutput(ev, output);
    if (completed.outputText.length > 0 || completed.refusal.length > 0) markChatSemanticOutput(usageContext);
    if (reconciled.length > 0) markFinalizedChatToolOutput(usageContext, functionCalls);
  };

  const applyResponseCompleted = async (ev: Record<string, unknown>, response: Record<string, unknown>): Promise<"complete" | "empty"> => {
    observedCompletedUsage = extractUsageTokens(response.usage);
    const completedOutput = reconcileChatResponseOutputContent(outputTextParts, refusalParts, ev, response.output);
    content += completedOutput.outputText;
    refusal += completedOutput.refusal;
    functionCalls.reconcileOutput(ev, response.output);
    functionCalls.assertFinalized();
    const usageTokens = observedCompletedUsage;
    usage = toChatUsage(usageTokens);
    if (!translatedChatOutputObserved(content, refusal, functionCalls)) {
      recordEmptyUpstreamCompletion(usageContext, lifecycle, usageTokens, onResponseTerminal);
      return "empty";
    }
    completed = true;
    await recordSuccessfulChatCompletion(usageContext, lifecycle, usageTokens, onResponseTerminal);
    return "complete";
  };

  const applyOutputEvent = async (event: ResponsesStreamEvent): Promise<"unhandled" | "next" | "complete" | "empty"> => {
    const ev = event.value;
    const type = event.type;
    if (type === "response.output_item.added") {
      functionCalls.add(ev, ev.item);
      return "next";
    }
    if (type === "response.function_call_arguments.delta") {
      functionCalls.delta(ev);
      return "next";
    }
    if (type === "response.function_call_arguments.done") {
      functionCalls.done(ev);
      markFinalizedChatToolOutput(usageContext, functionCalls);
      return "next";
    }
    if (type === "response.output_item.done") {
      applyOutputItemDone(ev);
      return "next";
    }
    if (type === "response.output") {
      applyResponseOutput(ev);
      return "next";
    }
    if (type === "response.completed" && isRecord(ev.response) && !Array.isArray(ev.response)) {
      return await applyResponseCompleted(ev, ev.response);
    }
    return "unhandled";
  };

  const consumeEvent = async (event: ResponsesStreamEvent): Promise<"next" | "break" | "complete" | "empty"> => {
    recordResponsesEventTelemetry(usageContext, event);
    if (event.terminal && event.type !== "response.completed") applyTerminalEvent(event);
    if (applyCreatedEvent(event)) return "next";
    if (applyTextEvent(event)) return "next";
    const outcome = await applyOutputEvent(event);
    if (outcome !== "unhandled") return outcome;
    if (event.terminal) return "break";
    return "next";
  };

  const settleFailure = (error: unknown): void => {
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
  };

  const respondIncomplete = async (): Promise<Response> => {
    await recordErrorUsage(usageContext);
    if (terminalType === "cancelled") {
      return streamErrorResponse(499, "Request was cancelled.", "request_cancelled", provider, warnings, "server_error", null);
    }
    if (terminalType === "deadline") {
      return streamErrorResponse(504, "Upstream request exceeded the gateway deadline.", "gateway_timeout", provider, warnings, "server_error", null);
    }
    return streamErrorResponse(502, "Upstream stream ended without response.completed.", "upstream_stream_error", provider, warnings);
  };

  const buildCompletedBody = (): Record<string, unknown> => {
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
    return body;
  };

  try {
    let pending: ResponsesStreamEvent | undefined = source.first;
    for (;;) {
      const next = pending ? { done: false as const, value: pending } : await source.iterator.next();
      pending = undefined;
      if (next.done) break;
      const outcome = await consumeEvent(next.value);
      if (outcome === "break") break;
      if (outcome === "empty") {
        emptyCompletion = true;
        break;
      }
      if (outcome === "complete") {
        completed = true;
        break;
      }
    }
  } catch (error) {
    settleFailure(error);
  } finally {
    // This path consumes the generator manually (rather than through
    // `for await`), so explicitly close it after a terminal event or error.
    // Otherwise the parser can remain suspended at its final `yield` while
    // retaining the upstream reader lock.
    await source.iterator.return("Chat Completions response consumed").catch(() => {});
  }
  if (emptyCompletion) {
    return streamErrorResponse(502, EMPTY_UPSTREAM_COMPLETION_MESSAGE, "empty_upstream_completion", provider, warnings, "server_error", null);
  }
  if (!completed) return await respondIncomplete();

  return json(200, buildCompletedBody(), { "x-uos-upstream": provider });
};

const snapshotUpstreamSource = (snapshot: CodexModelsSnapshot | null): string => {
  const source = snapshot?.source;
  if (!source) return "stored_codex_models";
  return source;
};

export const handleModels = async (req?: Request): Promise<Response> => {
  if (req) {
    const clientVersion = getCatalogClientVersion(req);
    if (clientVersion !== null) return await handleCodexCatalogModels(req, clientVersion);
  }
  const snapshot = await loadCodexModelsSnapshot();
  const normalized = snapshot && Array.isArray(snapshot.models) && snapshot.models.length > 0 ? normalizeModelList(snapshot) : null;
  const data = withConfiguredCerebrasModel(normalized?.data ?? []);
  const [metered, surplus] = await Promise.all([fetchMeteredModels(), fetchSurplusModels()]);
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

  return json(200, { object: "list", data: merged }, { "x-uos-upstream": snapshotUpstreamSource(snapshot) });
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

const providerSupportedEndpointPaths = (supportedEndpointTypes: readonly string[]): string[] => [
  ...(supportedEndpointTypes.includes("openai-response") ? ["/v1/responses"] : []),
  ...(supportedEndpointTypes.includes("openai") ? ["/v1/chat/completions"] : []),
];

const catalogAvailabilityStatus = (available: unknown): "available" | "unavailable" => (available ? "available" : "unavailable");

const publicModelCatalogEntry = (id: string, provider: PublicModelProvider): PublicModelCatalogEntry => {
  const context = recentModelContextFor(id);
  return {
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
  };
};

const addPublicModelCatalogEntry = (models: Map<string, PublicModelCatalogEntry>, id: string, provider: PublicModelProvider): void => {
  const existing = models.get(id);
  if (existing) {
    existing.providers.push(provider);
    return;
  }
  models.set(id, publicModelCatalogEntry(id, provider));
};

const collectCodexCatalogModelIds = (codexModels: readonly Record<string, unknown>[]): Set<string> => {
  const ids = new Set<string>();
  for (const model of codexModels) {
    const id = getString(model.id);
    if (id) ids.add(id);
  }
  return ids;
};

export const handlePublicModelCatalog = async (): Promise<Response> => {
  const snapshot = await loadCodexModelsSnapshot();
  const normalized = snapshot && Array.isArray(snapshot.models) && snapshot.models.length > 0 ? normalizeModelList(snapshot) : null;
  const [metered, surplus] = await Promise.all([fetchMeteredModels(), fetchSurplusModels({ requireApiKey: false })]);
  const codexModels = normalized?.data ?? [];
  const surplusModels = surplus?.models ?? [];
  const otherProviderModelIds = collectCodexCatalogModelIds(codexModels);
  for (const model of surplusModels) otherProviderModelIds.add(model.id);
  const models = new Map<string, PublicModelCatalogEntry>();
  const includedOpenLuxModelIds = new Set<string>();

  for (const model of codexModels) {
    const id = getString(model.id);
    if (!id) continue;
    addPublicModelCatalogEntry(models, id, {
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
    addPublicModelCatalogEntry(models, model.id, {
      id: "openlux",
      owned_by: model.owned_by,
      supported_endpoints: providerSupportedEndpointPaths(model.supported_endpoint_types),
    });
  }
  for (const model of surplusModels) {
    addPublicModelCatalogEntry(models, model.id, {
      id: "surplus",
      owned_by: model.owned_by,
      supported_endpoints: providerSupportedEndpointPaths(model.supported_endpoint_types),
    });
  }

  return json(200, {
    object: "uos.model_catalog",
    data: [...models.values()].sort((left, right) => left.id.localeCompare(right.id)),
    sources: {
      codex: {
        status: catalogAvailabilityStatus(normalized),
        count: normalized?.data.length ?? 0,
        updated_at_ms: snapshot?.updated_at_ms ?? null,
      },
      openlux: {
        status: catalogAvailabilityStatus(metered),
        count: includedOpenLuxModelIds.size,
        updated_at_ms: metered?.updated_at_ms ?? null,
      },
      surplus: {
        status: catalogAvailabilityStatus(surplus),
        count: surplus?.models.length ?? 0,
        updated_at_ms: surplus?.updated_at_ms ?? null,
      },
    },
  });
};

const discoveredModelCapabilitiesEntry = (
  model: Readonly<{ id: string; owned_by: string; supported_endpoint_types: readonly string[] }>,
  provider: "metered" | "surplus"
): Record<string, unknown> => {
  const supportedEndpoints = providerSupportedEndpointPaths(model.supported_endpoint_types);
  const context = recentModelContextFor(model.id);
  return {
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
  };
};

export const handleModelCapabilities = async (): Promise<Response> => {
  const snapshot = await loadFullCodexModelsSnapshot();
  const data =
    snapshot && Array.isArray(snapshot.models) && snapshot.models.length > 0
      ? (snapshot.models.map(normalizeModelCapabilitiesEntry).filter(Boolean) as Record<string, unknown>[])
      : [];
  const cerebras = configuredCerebrasModelCapabilities();
  if (cerebras && !data.some((model) => model.id === CEREBRAS_GPT_OSS_120B_MODEL)) {
    data.push(cerebras);
  }
  const [metered, surplus] = await Promise.all([fetchMeteredModels(), fetchSurplusModels()]);
  for (const [provider, models] of [
    ["metered", metered?.models ?? []],
    ["surplus", surplus?.models ?? []],
  ] as const) {
    for (const model of models) {
      if (data.some((candidate) => candidate.id === model.id)) continue;
      data.push(discoveredModelCapabilitiesEntry(model, provider));
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
    { "x-uos-upstream": snapshotUpstreamSource(snapshot) }
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

const handleEmbeddingsRequest = async (req: Request, usageContext?: UsageContext, options: Readonly<{ kv?: Deno.Kv | null }> = {}): Promise<Response> => {
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

  const kv = Object.prototype.hasOwnProperty.call(options, "kv") ? (options.kv ?? null) : await getKv();
  const hashes = await Promise.all(inputs.map((text) => sha256Hex(text)));
  let idempotencyLease: EmbeddingsIdempotencyLease | null = null;
  let idempotencyDispatched = false;
  let idempotencyHasConfirmedSuccess = false;

  const acquireIdempotencyLease = async (): Promise<Response | null> => {
    const idempotencyKey = req.headers.get("Idempotency-Key");
    if (idempotencyKey === null) return null;
    if (!idempotencyKey || idempotencyKey.length > EMBEDDINGS_IDEMPOTENCY_MAX_KEY_CHARS || hasAsciiControlCharacter(idempotencyKey)) {
      return openaiError(400, `Idempotency-Key must contain 1-${EMBEDDINGS_IDEMPOTENCY_MAX_KEY_CHARS} non-control characters.`, "invalid_request_error", {
        param: null,
      });
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
    return null;
  };

  const leaseFailure = await acquireIdempotencyLease();
  if (leaseFailure) return leaseFailure;

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
    return await releaseBeforeDispatch(openaiError(499, "Request was cancelled.", "request_cancelled", { type: "server_error", param: null }));
  };

  await recordRequestUsage(usageContext, { model, route: "embeddings", stream: false, reasoning: null });

  const deadlineMs = startedAtMs + EMBEDDINGS_TIMEOUT_MS;
  const apiKey = await readVoyageApiKey(kv);
  if (!apiKey) {
    await recordErrorUsage(usageContext);
    return await releaseBeforeDispatch(
      openaiError(503, "Embeddings provider is not configured: set VOYAGEAI_API_KEY (or store it in Deno KV)", "server_error", {
        type: "server_error",
        param: null,
      })
    );
  }
  const shouldCache = Boolean(kv);

  // Dedupe within a request (hash collisions are astronomically unlikely).
  const buckets = new Map<string, { text: string; indices: number[] }>();
  for (let i = 0; i < inputs.length; i += 1) {
    const hash = hashes[i];
    const existing = buckets.get(hash);
    if (existing) {
      existing.indices.push(i);
    } else {
      buckets.set(hash, { text: inputs[i], indices: [i] });
    }
  }

  const cacheProfileKey = profile.cache_profile_key;
  const cacheKeyFor = (hash: string): Deno.KvKey => embeddingsCacheKey(cacheProfileKey, hash);
  const vectorsByIndex: (number[] | null)[] = Array.from({ length: inputs.length }, () => null);

  let voyageTotalTokens = 0;
  let sawVoyageTokenUsage = false;

  const missing: { hash: string; text: string; indices: number[] }[] = [];
  const loadCachedVectorsAndMissing = async (): Promise<void> => {
    if (!shouldCache || !kv) {
      for (const [hash, bucket] of buckets.entries()) {
        missing.push({ hash, text: bucket.text, indices: bucket.indices });
      }
      return;
    }
    const unique = Array.from(buckets.entries()).map(([hash, bucket]) => ({ hash, ...bucket }));
    const entries = await Promise.all(unique.map((item) => kv.get<{ embedding?: unknown }>(cacheKeyFor(item.hash))));
    for (let i = 0; i < unique.length; i += 1) {
      const item = unique[i];
      const entry = entries[i];
      const cached = entry.value?.embedding;
      if (isValidEmbeddingVector(cached, profile.dimensions)) {
        for (const idx of item.indices) vectorsByIndex[idx] = cached;
      } else {
        missing.push(item);
      }
    }
  };
  const releasedFailureResponse = async (response: Response): Promise<Response> => {
    await recordErrorUsage(usageContext);
    if (idempotencyLease && idempotencyDispatched) return await failIndeterminate();
    return await releaseBeforeDispatch(response);
  };
  const unreleasedFailureResponse = async (response: Response): Promise<Response> => {
    await recordErrorUsage(usageContext);
    if (idempotencyLease && idempotencyDispatched) return await failIndeterminate();
    return response;
  };
  const timeoutResponse = async (): Promise<Response> =>
    await releasedFailureResponse(openaiError(502, "Embeddings request timed out.", "timeout", { type: "server_error", param: null }));
  const rateLimitResponse = async (waitMs: number): Promise<Response> => {
    const retryAfterSeconds = Math.max(1, Math.ceil(waitMs / 1000));
    const body = {
      error: {
        message: `Rate limit exceeded; retry after ~${retryAfterSeconds}s`,
        type: "rate_limit_error",
        code: "rate_limit_exceeded",
        param: null,
      },
    };
    return await releasedFailureResponse(json(429, body, { "Retry-After": String(retryAfterSeconds) }));
  };
  const sizeMismatchResponse = async (): Promise<Response> =>
    await unreleasedFailureResponse(
      openaiError(502, "Embeddings upstream returned a size mismatch.", "upstream_error", {
        type: "server_error",
        param: null,
      })
    );
  const dimensionMismatchResponse = async (actualLength: number): Promise<Response> =>
    await unreleasedFailureResponse(
      openaiError(502, `Embeddings upstream returned vector length ${actualLength}; expected ${profile.dimensions}.`, "upstream_dimension_mismatch", {
        type: "server_error",
        param: null,
      })
    );
  const incompleteResponseFailure = async (): Promise<Response> =>
    await unreleasedFailureResponse(
      openaiError(502, "Embeddings gateway failed to construct a complete response.", "server_error", {
        type: "server_error",
        param: null,
      })
    );
  const ensureIdempotencyDispatched = async (): Promise<Response | null> => {
    if (!idempotencyLease || idempotencyDispatched) return null;
    const markedDispatched = await markEmbeddingsIdempotencyDispatched(idempotencyLease);
    if (!markedDispatched) {
      await recordErrorUsage(usageContext);
      return embeddingsIdempotencyUnavailableResponse();
    }
    idempotencyDispatched = true;
    return null;
  };
  const handleQuotaDispatchFailure = async (error: ApiKeyQuotaDispatchError): Promise<Response> => {
    await recordErrorUsage(usageContext);
    if (idempotencyLease) {
      const released = await releaseEmbeddingsIdempotencyReservation(idempotencyLease, idempotencyDispatched);
      if (!released) return embeddingsIdempotencyUnavailableResponse();
      idempotencyDispatched = false;
    }
    return apiKeyQuotaDispatchErrorResponse(error);
  };
  const upstreamFailureResponse = async (status: number, message: string, waitMs: number): Promise<Response> => {
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
      return await releaseAfterExplicitUpstreamFailure(json(429, body, { "Retry-After": String(retryAfterSeconds) }));
    }
    return await releaseAfterExplicitUpstreamFailure(openaiError(502, message, "upstream_error", { type: "server_error", param: null }));
  };
  const resolveUpstreamFailure = async (error: unknown, attempt: number, backoffMs: number): Promise<Response | null> => {
    if (downstreamSignal.aborted) return await cancelledResponse();
    if (error instanceof ApiKeyQuotaDispatchError) return await handleQuotaDispatchFailure(error);
    const status = (error as { status?: number }).status;
    const retryAfterMs = (error as { retry_after_ms?: number | null }).retry_after_ms ?? null;
    const snippet = formatErrorSnippet(error);
    const message = snippet ? `Embeddings upstream request failed: ${snippet}` : "Embeddings upstream request failed.";

    if (!status || !EMBEDDINGS_RETRYABLE_UPSTREAM_STATUSES.has(status)) {
      logRedactedUpstreamError(`[ai.ubq.fi] embeddings request_id=${requestId} upstream_error:`, error);
      await recordErrorUsage(usageContext);
      if (!status) return await failIndeterminate();
      return await releaseAfterExplicitUpstreamFailure(openaiError(502, message, "upstream_error", { type: "server_error", param: null }));
    }

    const waitMs = Math.max(0, retryAfterMs ?? backoffMs);
    if (attempt >= 2) {
      logRedactedUpstreamError(`[ai.ubq.fi] embeddings request_id=${requestId} upstream_error:`, error);
      await recordErrorUsage(usageContext);
      return await upstreamFailureResponse(status, message, waitMs);
    }

    const now = Date.now();
    if (now + waitMs >= deadlineMs) {
      await recordErrorUsage(usageContext);
      return await upstreamFailureResponse(status, message, waitMs);
    }

    if (!(await sleepUnlessAborted(waitMs, downstreamSignal))) return await cancelledResponse();
    return null;
  };
  const recordChunkUpstreamSuccess = (totalTokens: number | null): void => {
    if (idempotencyLease) idempotencyHasConfirmedSuccess = true;
    if (typeof totalTokens === "number") {
      sawVoyageTokenUsage = true;
      voyageTotalTokens += totalTokens;
    }
  };
  const fetchChunkVectors = async (
    texts: string[]
  ): Promise<{ kind: "ok"; vectors: number[][]; totalTokens: number | null } | { kind: "response"; response: Response }> => {
    let attempt = 0;
    let backoffMs = 250;
    for (;;) {
      if (downstreamSignal.aborted) return { kind: "response", response: await cancelledResponse() };
      const dispatchFailure = await ensureIdempotencyDispatched();
      if (dispatchFailure) return { kind: "response", response: dispatchFailure };
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
        recordChunkUpstreamSuccess(upstream.totalTokens);
        return { kind: "ok", vectors: upstream.vectors, totalTokens: upstream.totalTokens };
      } catch (error) {
        const failure = await resolveUpstreamFailure(error, attempt, backoffMs);
        if (failure) return { kind: "response", response: failure };
        backoffMs = Math.min(2000, backoffMs * 2);
        attempt += 1;
      }
    }
  };
  const writeChunkCacheEntries = async (chunkItems: { hash: string; text: string; indices: number[] }[], chunkVectors: number[][]): Promise<void> => {
    for (let i = 0; i < chunkItems.length; i += 1) {
      const item = chunkItems[i];
      const vec = chunkVectors[i];
      for (const idx of item.indices) vectorsByIndex[idx] = vec;
      if (shouldCache && kv) {
        await writeEmbeddingsCacheEntryBestEffort(kv, cacheProfileKey, item.hash, vec, Date.now(), deadlineMs);
      }
    }
  };
  const processChunk = async (chunkItems: { hash: string; text: string; indices: number[] }[]): Promise<Response | null> => {
    const texts = chunkItems.map((item) => item.text);
    const tokenEstimate = estimateTokenCount(texts);

    if (kv) {
      const reserved = await applyVoyageRateLimit(kv, tokenEstimate, deadlineMs);
      if (!reserved.ok) return await rateLimitResponse(reserved.wait_ms);
    }

    const fetched = await fetchChunkVectors(texts);
    if (fetched.kind === "response") return fetched.response;
    const vectors = fetched.vectors;

    if (vectors.length !== chunkItems.length) return await sizeMismatchResponse();

    const wrongLengthIndex = vectors.findIndex((vector) => vector.length !== profile.dimensions);
    if (wrongLengthIndex >= 0) return await dimensionMismatchResponse(vectors[wrongLengthIndex]?.length ?? 0);

    await writeChunkCacheEntries(chunkItems, vectors);
    return null;
  };
  const fillMissingVectors = async (): Promise<Response | null> => {
    const chunks = chunkByTokenBudget(
      missing.map((item) => ({ hash: item.hash, text: item.text })),
      EMBEDDINGS_MAX_INPUTS_PER_REQUEST,
      VOYAGE_RATE_LIMIT_TPM
    );

    let offset = 0;
    for (const chunk of chunks) {
      const now = Date.now();
      if (now >= deadlineMs) return await timeoutResponse();
      const chunkItems = missing.slice(offset, offset + chunk.length);
      offset += chunk.length;
      const failure = await processChunk(chunkItems);
      if (failure) return failure;
    }
    return null;
  };
  const storeSuccessOrFailure = async (response: Response): Promise<Response | null> => {
    if (!idempotencyLease) return null;
    const stored = await storeEmbeddingsIdempotencySuccess(idempotencyLease, response);
    if (stored) return null;
    if (idempotencyDispatched) return await failIndeterminate();
    return await releaseBeforeDispatch(embeddingsIdempotencyUnavailableResponse());
  };
  const completeEmbeddingsResponse = async (): Promise<Response> => {
    const data: { object: "embedding"; index: number; embedding: number[] | string }[] = [];
    for (let i = 0; i < vectorsByIndex.length; i += 1) {
      const vec = vectorsByIndex[i];
      if (!vec) return await incompleteResponseFailure();
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
    const storeFailure = await storeSuccessOrFailure(response);
    if (storeFailure) return storeFailure;
    await recordCompletionUsage(usageContext, usageTokens);
    return response;
  };

  await loadCachedVectorsAndMissing();

  if (missing.length > 0) {
    const failure = await fillMissingVectors();
    if (failure) return failure;
  }

  return await completeEmbeddingsResponse();
};

export const handleUosEmbeddings = async (req: Request, usageContext?: UsageContext, options: Readonly<{ kv?: Deno.Kv | null }> = {}): Promise<Response> =>
  await runWithResponseTelemetry(usageContext, async (context) => withVoyageUpstreamHeader(await handleEmbeddingsRequest(req, context, options)));

const buildEmbeddingsJobBody = (job: EmbeddingsJobRecord, result: Record<string, unknown> | null): Record<string, unknown> => ({
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
  dimensions: VoyageEmbeddingsDimension
): Promise<(number[] | null)[]> => {
  const uniqueHashes = Array.from(new Set(hashesByIndex));
  const cacheKeyFor = (hash: string): Deno.KvKey => embeddingsCacheKey(cacheProfileKey, hash);
  const entries = await Promise.all(uniqueHashes.map((hash) => kv.get<{ embedding?: unknown }>(cacheKeyFor(hash))));
  const vectorsByHash = new Map<string, number[]>();
  for (const [i, hash] of uniqueHashes.entries()) {
    const cached = entries[i]?.value?.embedding;
    if (isValidEmbeddingVector(cached, dimensions)) {
      vectorsByHash.set(hash, cached);
    }
  }
  return hashesByIndex.map((hash) => vectorsByHash.get(hash) ?? null);
};

const buildOpenAiEmbeddingsResult = (
  model: string,
  vectorsByIndex: (number[] | null)[],
  usageTotalTokens: number,
  encodingFormat: EmbeddingsEncodingFormat
): Record<string, unknown> => ({
  object: "list",
  data: vectorsByIndex.map((vec, index) => ({
    object: "embedding",
    index,
    embedding: vec && encodingFormat === "base64" ? floatEmbeddingToBase64(vec) : (vec ?? []),
  })),
  model,
  usage: { prompt_tokens: usageTotalTokens, total_tokens: usageTotalTokens },
});

const reserveVoyageBudgetForJob = async (kv: Deno.Kv, tokens: number): Promise<{ ok: true } | { ok: false; wait_ms: number }> => {
  for (let attempt = 0; attempt < 6; attempt += 1) {
    const reserved = await tryReserveVoyageBudget(kv, tokens);
    if (reserved.ok) return reserved;
    if (reserved.wait_ms > 0) return reserved;
    await sleep(5 + attempt * 5);
  }
  return { ok: false, wait_ms: 1000 };
};

const updateEmbeddingsJobRecord = async (kv: Deno.Kv, jobKey: Deno.KvKey, lookupKey: Deno.KvKey, job: EmbeddingsJobRecord): Promise<void> => {
  await kv
    .atomic()
    .set(jobKey, job, { expireIn: EMBEDDINGS_JOB_TTL_MS })
    .set(lookupKey, { cache_profile_key: job.cache_profile_key } satisfies EmbeddingsJobLookupRecord, { expireIn: EMBEDDINGS_JOB_TTL_MS })
    .commit();
};

const deleteEmbeddingsJobInputs = async (kv: Deno.Kv, tokenHash: string, cacheProfileKey: string, jobId: string, uniqueHashes: string[]): Promise<void> => {
  await Promise.all(uniqueHashes.map((hash) => kv.delete(embeddingsJobInputKey(tokenHash, cacheProfileKey, jobId, hash))));
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

  const lockCommit = await params.kv
    .atomic()
    .check(params.jobEntry)
    .set(params.jobKey, locked, { expireIn: EMBEDDINGS_JOB_TTL_MS })
    .set(params.jobLookupKey, { cache_profile_key: locked.cache_profile_key } satisfies EmbeddingsJobLookupRecord, { expireIn: EMBEDDINGS_JOB_TTL_MS })
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
    const entries = await Promise.all(uniqueHashes.map((hash) => params.kv.get<{ embedding?: unknown }>(cacheKeyFor(hash))));
    const missing: string[] = [];
    for (let i = 0; i < uniqueHashes.length; i += 1) {
      const hash = uniqueHashes[i];
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
    await deleteEmbeddingsJobInputs(params.kv, params.tokenHash, failed.cache_profile_key, failed.id, uniqueHashes);
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
    const vectorsByIndex = await loadEmbeddingsVectorsFromCache(params.kv, cacheProfileKey, hashesByIndex, currentJob.dimensions);
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
    await deleteEmbeddingsJobInputs(params.kv, params.tokenHash, succeeded.cache_profile_key, succeeded.id, uniqueHashes);
    const result = buildOpenAiEmbeddingsResult(succeeded.model, vectorsByIndex, succeeded.usage_total_tokens, succeeded.encoding_format);
    const usageTokens: UsageTokens | null =
      succeeded.usage_total_tokens > 0
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

  const loadInputItems = async (
    hashes: string[]
  ): Promise<{ kind: "ok"; items: { hash: string; text: string }[] } | { kind: "response"; response: Response }> => {
    const inputEntries = await Promise.all(
      hashes.map((hash) => params.kv.get<EmbeddingsJobInputRecord>(embeddingsJobInputKey(params.tokenHash, locked.cache_profile_key, locked.id, hash)))
    );
    const items: { hash: string; text: string }[] = [];
    for (let i = 0; i < hashes.length; i += 1) {
      const hash = hashes[i];
      const entry = inputEntries[i];
      const normalized = normalizeEmbeddingsJobInputRecord(entry.value);
      if (!normalized) {
        return { kind: "response", response: await failJob("Embeddings job input expired or was unavailable.", "embeddings_job_input_missing") };
      }
      const text = await decryptEmbeddingsJobInput(params.tokenSeed, normalized);
      if (text === null) {
        return { kind: "response", response: await failJob("Embeddings job input could not be decrypted.", "embeddings_job_input_decrypt_failed") };
      }
      items.push({ hash, text });
    }
    return { kind: "ok", items };
  };

  const fetchChunkVectors = async (
    texts: string[]
  ): Promise<
    | { kind: "ok"; vectors: number[][]; totalTokens: number | null }
    | { kind: "response"; response: Response }
    | { kind: "queued"; waitMs: number; failureKind: string }
  > => {
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
      return { kind: "ok", vectors: upstream.vectors, totalTokens: upstream.totalTokens };
    } catch (error) {
      if (error instanceof ApiKeyQuotaDispatchError) {
        return { kind: "response", response: await queueJob(1_000) };
      }
      const status = (error as { status?: number }).status;
      const retryAfterMs = (error as { retry_after_ms?: number | null }).retry_after_ms ?? null;
      if (status && EMBEDDINGS_RETRYABLE_UPSTREAM_STATUSES.has(status)) {
        return {
          kind: "queued",
          waitMs: retryAfterMs ?? (status === 429 ? 60_000 : 1_000),
          failureKind: `embeddings_job_upstream_http_${status}`,
        };
      }
      const snippet = formatErrorSnippet(error);
      const message = snippet ? `Embeddings upstream request failed: ${snippet}` : "Embeddings upstream request failed.";
      return { kind: "response", response: await failJob(message, "embeddings_job_upstream_error") };
    }
  };

  const processChunk = async (
    chunk: { hash: string; text: string }[]
  ): Promise<{ kind: "continue" } | { kind: "stop"; waitMs: number; failureKind: string | null } | { kind: "response"; response: Response }> => {
    if (Date.now() >= params.deadlineMs) {
      return { kind: "stop", waitMs: 1000, failureKind: "embeddings_job_deadline" };
    }

    const texts = chunk.map((item) => item.text);
    const tokenEstimate = estimateTokenCount(texts);

    const reserved = await reserveVoyageBudgetForJob(params.kv, tokenEstimate);
    if (!reserved.ok) {
      return { kind: "stop", waitMs: reserved.wait_ms > 0 ? reserved.wait_ms : 1000, failureKind: null };
    }

    const fetched = await fetchChunkVectors(texts);
    if (fetched.kind === "response") return { kind: "response", response: fetched.response };
    if (fetched.kind === "queued") return { kind: "stop", waitMs: fetched.waitMs, failureKind: fetched.failureKind };

    const vectors = fetched.vectors;
    if (vectors.length !== chunk.length) {
      return { kind: "response", response: await failJob("Embeddings upstream returned a size mismatch.", "embeddings_job_upstream_mismatch") };
    }

    const wrongLengthIndex = vectors.findIndex((vector) => vector.length !== currentJob.dimensions);
    if (wrongLengthIndex >= 0) {
      const actualLength = vectors[wrongLengthIndex]?.length ?? 0;
      return {
        kind: "response",
        response: await failJob(
          `Embeddings upstream returned vector length ${actualLength}; expected ${currentJob.dimensions}.`,
          "embeddings_job_upstream_dimension_mismatch"
        ),
      };
    }

    if (typeof fetched.totalTokens === "number") {
      currentJob = { ...currentJob, usage_total_tokens: currentJob.usage_total_tokens + fetched.totalTokens };
    }

    for (let i = 0; i < chunk.length; i += 1) {
      const item = chunk[i];
      const vec = vectors[i];
      await writeEmbeddingsCacheEntryBestEffort(params.kv, currentJob.cache_profile_key, item.hash, vec, Date.now(), params.deadlineMs);
    }
    return { kind: "continue" };
  };

  const missingBefore = await computeMissing();
  if (missingBefore.length === 0) return await succeedJob();

  const loadedInputs = await loadInputItems(missingBefore);
  if (loadedInputs.kind === "response") return loadedInputs.response;
  const items = loadedInputs.items;

  const chunks = chunkByTokenBudget(items, EMBEDDINGS_MAX_INPUTS_PER_REQUEST, VOYAGE_RATE_LIMIT_TPM);
  for (const chunk of chunks) {
    const outcome = await processChunk(chunk);
    if (outcome.kind === "response") return outcome.response;
    if (outcome.kind === "stop") {
      queueRetryAfterMs = outcome.waitMs;
      queueFailureKind = outcome.failureKind;
      break;
    }
  }

  const missingAfter = await computeMissing();
  if (missingAfter.length === 0) return await succeedJob();

  const waitMs = queueRetryAfterMs ?? 60_000;
  return await queueJob(waitMs, queueFailureKind);
};

const handleEmbeddingsJobCreateInternal = async (req: Request, authToken: string | null, usageContext?: UsageContext): Promise<Response> => {
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
    return openaiError(503, "Embeddings provider is not configured: set VOYAGEAI_API_KEY (or store it in Deno KV)", "server_error", {
      type: "server_error",
      param: null,
    });
  }

  const hashesByIndex = await Promise.all(inputs.map((text) => sha256Hex(text)));
  const uniqueTextsByHash = new Map<string, string>();
  for (const [i, text] of inputs.entries()) uniqueTextsByHash.set(hashesByIndex[i], text);
  const uniqueHashes = Array.from(uniqueTextsByHash.keys());

  const jobId = `embjob_${crypto.randomUUID().replace(/-/g, "")}`;
  const tokenSeed = resolveEmbeddingsJobTokenSeed(jobId, authToken, usageContext);
  const tokenHash = await sha256Hex(tokenSeed);
  const now = Date.now();

  // Store encrypted inputs (no raw text) so queued jobs can be processed later without the client resending inputs.
  const inputWrites = uniqueHashes.map(async (hash) => {
    const record = await encryptEmbeddingsJobInput(tokenSeed, uniqueTextsByHash.get(hash) ?? "");
    await kv.set(embeddingsJobInputKey(tokenHash, profile.cache_profile_key, jobId, hash), record, { expireIn: EMBEDDINGS_JOB_TTL_MS });
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
  const persisted = await kv
    .atomic()
    .set(jobKey, job, { expireIn: EMBEDDINGS_JOB_TTL_MS })
    .set(jobLookupKey, { cache_profile_key: profile.cache_profile_key } satisfies EmbeddingsJobLookupRecord, { expireIn: EMBEDDINGS_JOB_TTL_MS })
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

export const handleEmbeddingsJobCreate = async (req: Request, authToken: string | null, usageContext?: UsageContext): Promise<Response> =>
  await runWithResponseTelemetry(usageContext, async (context) => withVoyageUpstreamHeader(await handleEmbeddingsJobCreateInternal(req, authToken, context)));

const handleEmbeddingsJobGetInternal = async (_req: Request, authToken: string | null, jobId: string, usageContext?: UsageContext): Promise<Response> => {
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
  if (job?.cache_profile_key !== cacheProfileKey) {
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
    const vectorsByIndex = await loadEmbeddingsVectorsFromCache(kv, job.cache_profile_key, job.input_hashes, job.dimensions);
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
    return openaiError(503, "Embeddings provider is not configured: set VOYAGEAI_API_KEY (or store it in Deno KV)", "server_error", {
      type: "server_error",
      param: null,
    });
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

export const handleEmbeddingsJobGet = async (req: Request, authToken: string | null, jobId: string, usageContext?: UsageContext): Promise<Response> =>
  await runWithResponseTelemetry(usageContext, async (context) =>
    withVoyageUpstreamHeader(await handleEmbeddingsJobGetInternal(req, authToken, jobId, context))
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

  if (cerebrasChatCompletionHasSemanticOutput(completion.value)) markChatSemanticOutput(usageContext);
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
  const unknownKey = findUnknownKey(rawRecord, CHAT_COMPLETIONS_ALLOWED_KEYS);
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

const handleChatCompletionsInternal = async (req: Request, usageContext?: UsageContext): Promise<Response> => {
  const envelope = await parseChatCompletionsEnvelope(req);
  if (!envelope.ok) return envelope.response;
  const { body, rawRecord, warnings, jsonObjectTextFormat } = envelope.value;

  const modelChoice = await resolveChatCompletionsModel(rawRecord, usageContext);
  if (!modelChoice.ok) return modelChoice.response;
  const { modelRaw, model, maxCompletionTokens } = modelChoice.value;

  if (model.toLowerCase() === CEREBRAS_GPT_OSS_120B_MODEL) {
    return await handleCerebrasChatCompletions(req, rawRecord, modelRaw, usageContext);
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
  if (model.toLowerCase() === CEREBRAS_GPT_OSS_120B_MODEL) {
    return {
      ok: false,
      response: openaiError(400, "gpt-oss-120b is available only on /v1/chat/completions.", "unsupported_model", { param: "model" }),
    };
  }
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

const recordRemovedProviderCircuitTransition = (usageContext: UsageContext | undefined, transition: string): void => {
  if (transition !== "none") recordRemovedProviderFields(usageContext, { circuitTransition: transition });
};

const prepareResponsesRouting = async (req: Request, state: ResponsesRequestState, model: string): Promise<ResponsesRoutingState> => {
  const downstreamSignal = downstreamSignalFor(req, state.usageContext);
  const requestInferenceSignal = state.clientWantsStream ? downstreamSignal : inferenceSignal(req, state.usageContext);
  const preHeaderDeadline = createStreamFirstEventDeadline(requestInferenceSignal);
  const apiKey = isTemporaryFreeSurplusModel(model) ? null : readRemovedProviderApiKey();
  const paidFallbackAvailable = canAttemptPaidFallback(state.usageContext);
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
  if (!isEligibleResponsesAttemptStatus(failed.response) && failed.trigger === "read_error") {
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

const runResponsesHandler = async (req: Request, usageContext?: UsageContext, parsedBody?: unknown): Promise<Response> =>
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
const IMAGE_GENERATION_REQUEST_KEYS = new Set<string>([...IMAGE_SHARED_REQUEST_KEYS, "moderation", "response_format", "style"]);
const IMAGE_EDIT_JSON_REQUEST_KEYS = new Set<string>([...IMAGE_SHARED_REQUEST_KEYS, "images", "mask", "input_fidelity", "moderation"]);
const IMAGE_EDIT_MULTIPART_REQUEST_KEYS = new Set<string>([...IMAGE_SHARED_REQUEST_KEYS, "image", "image[]", "mask", "input_fidelity", "response_format"]);

export type ImageRouteKind = "generations" | "edits";

type ImageRequestFailure = Readonly<{ ok: false; response: Response }>;
type ParsedImageRequest = Readonly<{ ok: true; body: Record<string, unknown>; count: number; inputBytes: number }> | ImageRequestFailure;

type ImageHandlerOptions = Readonly<{
  dispatch?: (request: Request) => Promise<Response>;
}>;

/** Resolve the text model that hosts the image tool. */
export const resolveImageBaseModel = async (): Promise<string | null> => {
  if (imageBaseModelForTest !== null) return imageBaseModelForTest;
  let configured: string | null;
  try {
    const raw = Deno.env.get(IMAGE_BASE_MODEL_ENV)?.trim();
    configured = raw && raw.length > 0 ? raw : null;
  } catch {
    configured = null;
  }
  return configured ?? (await getDefaultModel());
};
/**
 * Rewrite an OpenAI Images request as a Responses request carrying the
 * image_generation tool. Its optional model field selects the requested image
 * model, while the outer Responses model remains the text model that hosts the
 * tool.
 */
export const buildImageResponsesRequest = (body: Record<string, unknown>, baseModel: string, kind: ImageRouteKind): Record<string, unknown> => {
  const prompt = typeof body.prompt === "string" ? body.prompt : "";
  const user = typeof body.user === "string" ? { user: body.user } : {};
  const tool: Record<string, unknown> = {
    type: IMAGE_TOOL_TYPE,
    action: kind === "edits" ? "edit" : "generate",
  };
  const requestedModel = typeof body.model === "string" ? body.model.trim() : "";
  if (requestedModel) tool.model = requestedModel;
  else if (kind === "edits") tool.model = IMAGE_EDIT_DEFAULT_MODEL;
  const toolOptionKeys = ["size", "quality", "background", "output_format", "output_compression", "moderation"];
  for (const key of toolOptionKeys.filter((key) => body[key] !== undefined)) tool[key] = body[key];
  if (kind === "edits") {
    // Edits supply source images; the Responses input carries them alongside
    // the instruction so the tool can operate on the provided pixels.
    const images = Array.isArray(body.images) ? body.images : [];
    if (body.input_fidelity !== undefined) tool.input_fidelity = body.input_fidelity;
    if (isPlainRecord(body.mask)) tool.input_image_mask = body.mask;
    return {
      model: baseModel,
      ...user,
      input: [
        {
          role: "user",
          content: [
            { type: "input_text", text: prompt },
            ...images.flatMap((entry): Record<string, unknown>[] => {
              if (!isPlainRecord(entry)) return [];
              if (typeof entry.image_url === "string") {
                return [{ type: "input_image", image_url: entry.image_url }];
              }
              return [];
            }),
          ],
        },
      ],
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

const isPlainRecord = (value: unknown): value is Record<string, unknown> => typeof value === "object" && value !== null && !Array.isArray(value);

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
  return inlineData?.mediaType === "image/png" && inlineData.bytes < IMAGE_MAX_MASK_FILE_BYTES ? { image_url: inlineData.url } : null;
};

const normalizeImageMediaType = (value: string): string | null => {
  const mediaType = value.split(";", 1)[0]?.trim().toLowerCase() ?? "";
  return IMAGE_MEDIA_TYPE_ALIASES.get(mediaType) ?? null;
};

const normalizeImageDataUrl = (value: string): Readonly<{ url: string; bytes: number; mediaType: string }> | null => {
  const match = /^data:([^;,]+);base64,([a-z0-9+/]*={0,2})$/iu.exec(value);
  if (!match) return null;
  const mediaType = normalizeImageMediaType(match[1]);
  const encoded = match[2];
  if (!mediaType || encoded.length === 0 || encoded.length % 4 !== 0) return null;
  let padding = 0;
  if (encoded.endsWith("==")) padding = 2;
  else if (encoded.endsWith("=")) padding = 1;
  const bytes = (encoded.length / 4) * 3 - padding;
  if (bytes <= 0 || bytes >= IMAGE_MAX_FILE_BYTES) return null;
  return { url: `data:${mediaType};base64,${encoded}`, bytes, mediaType };
};

const imageReferenceInputBytes = (body: Record<string, unknown>): number => {
  const references = [...(Array.isArray(body.images) ? body.images : []), ...(isPlainRecord(body.mask) ? [body.mask] : [])];
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
  return /^(?:0|[1-9]\d*)$/u.test(normalized) ? Number(normalized) : value;
};

const parseMultipartNumber = (value: FormDataEntryValue | null): unknown => {
  if (typeof value !== "string") return value;
  const normalized = value.trim();
  if (!/^-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?$/u.test(normalized)) return value;
  const parsed = Number(normalized);
  return Number.isFinite(parsed) ? parsed : value;
};

const exceedsUnicodeCodePointLimit = (value: string, limit: number): boolean => {
  const codePoints = value[Symbol.iterator]();
  let count = 0;
  while (!codePoints.next().done) {
    count += 1;
    if (count > limit) return true;
  }
  return false;
};

const parseMultipartBoolean = (value: FormDataEntryValue | null): unknown => {
  if (typeof value !== "string") return value;
  const normalized = value.trim();
  if (normalized === "true") return true;
  if (normalized === "false") return false;
  return value;
};

type ImageScalarValidationResult = Readonly<{ ok: true; count: number }> | ImageRequestFailure;

const imageEnumOptionError = (raw: Record<string, unknown>, key: string, allowed: ReadonlySet<string>, message: string): ImageRequestFailure | null => {
  const value = raw[key];
  if (value === undefined) return null;
  if (typeof value === "string" && allowed.has(value)) return null;
  return imageRequestError(message, key);
};

const imageScalarIdentityFailure = (raw: Record<string, unknown>): ImageRequestFailure | null => {
  if (Object.prototype.hasOwnProperty.call(raw, "style")) {
    return imageRequestError("style is not supported because this gateway uses the Responses image-generation tool.", "style");
  }
  if (typeof raw.prompt !== "string" || raw.prompt.trim().length === 0) {
    return imageRequestError("Image requests must include a prompt.", "prompt");
  }
  if (exceedsUnicodeCodePointLimit(raw.prompt, IMAGE_MAX_PROMPT_CHARS)) {
    return imageRequestError(`prompt must contain no more than ${IMAGE_MAX_PROMPT_CHARS} characters.`, "prompt");
  }
  if (Object.prototype.hasOwnProperty.call(raw, "model") && (typeof raw.model !== "string" || raw.model.trim().length === 0)) {
    return imageRequestError("model must be a non-empty string.", "model");
  }
  if (raw.user !== undefined && typeof raw.user !== "string") {
    return imageRequestError("user must be a string.", "user");
  }
  return null;
};

const imageScalarPrecisionFailure = (raw: Record<string, unknown>): ImageRequestFailure | null => {
  if (
    raw.output_compression !== undefined &&
    (!Number.isInteger(raw.output_compression) || (raw.output_compression as number) < 0 || (raw.output_compression as number) > 100)
  ) {
    return imageRequestError("output_compression must be an integer from 0 to 100.", "output_compression");
  }
  if (raw.size !== undefined && (typeof raw.size !== "string" || raw.size.trim().length === 0)) {
    return imageRequestError("size must be a non-empty string.", "size");
  }
  return null;
};

const imageEditQualities = (multipart: boolean): ReadonlySet<string> => (multipart ? IMAGE_EDIT_MULTIPART_QUALITIES : IMAGE_EDIT_JSON_QUALITIES);

const imageQualities = (kind: ImageRouteKind, multipart: boolean): ReadonlySet<string> =>
  kind === "generations" ? IMAGE_GENERATION_QUALITIES : imageEditQualities(multipart);

const imageScalarEnumerationFailure = (raw: Record<string, unknown>, kind: ImageRouteKind, multipart: boolean): ImageRequestFailure | null => {
  const qualities = imageQualities(kind, multipart);
  const qualityFailure = imageEnumOptionError(
    raw,
    "quality",
    qualities,
    `quality must be low, medium, high, or auto${kind === "edits" ? " for image edits" : ""}.`
  );
  if (qualityFailure) return qualityFailure;
  const backgroundFailure = imageEnumOptionError(raw, "background", IMAGE_BACKGROUNDS, "background must be transparent, opaque, or auto.");
  if (backgroundFailure) return backgroundFailure;
  const outputFormatFailure = imageEnumOptionError(raw, "output_format", IMAGE_OUTPUT_FORMATS, "output_format must be png, jpeg, or webp.");
  if (outputFormatFailure) return outputFormatFailure;
  const moderationFailure = imageEnumOptionError(raw, "moderation", IMAGE_MODERATION_LEVELS, "moderation must be low or auto.");
  if (moderationFailure) return moderationFailure;
  return imageEnumOptionError(raw, "input_fidelity", IMAGE_INPUT_FIDELITIES, "input_fidelity must be low or high.");
};

const imageScalarStreamingFailure = (raw: Record<string, unknown>): ImageRequestFailure | null => {
  if (raw.stream !== undefined && typeof raw.stream !== "boolean") {
    return imageRequestError("stream must be a boolean.", "stream");
  }
  if (raw.stream === true || raw.partial_images !== undefined) {
    return imageRequestError("Streaming image responses are not supported by this gateway.", raw.stream === true ? "stream" : "partial_images");
  }
  if (raw.response_format !== undefined && raw.response_format !== "b64_json") {
    return imageRequestError("response_format must be b64_json.", "response_format");
  }
  return null;
};

const validateImageScalarOptions = (raw: Record<string, unknown>, kind: ImageRouteKind, multipart = false): ImageScalarValidationResult => {
  const identityFailure = imageScalarIdentityFailure(raw);
  if (identityFailure) return identityFailure;
  const count = raw.n === undefined ? 1 : raw.n;
  if (!Number.isInteger(count) || (count as number) < 1 || (count as number) > IMAGE_MAX_COUNT) {
    return imageRequestError(`n must be an integer from 1 to ${IMAGE_MAX_COUNT}.`, "n");
  }
  const precisionFailure = imageScalarPrecisionFailure(raw);
  if (precisionFailure) return precisionFailure;
  const enumerationFailure = imageScalarEnumerationFailure(raw, kind, multipart);
  if (enumerationFailure) return enumerationFailure;
  const streamingFailure = imageScalarStreamingFailure(raw);
  if (streamingFailure) return streamingFailure;
  return { ok: true, count: count as number };
};

type MultipartImageEditResult = Readonly<{ ok: true; body: Record<string, unknown>; inputBytes: number }> | ImageRequestFailure;

const withoutNullImageOptions = (raw: Record<string, unknown>): Record<string, unknown> =>
  Object.fromEntries(Object.entries(raw).filter(([key, value]) => value !== null || !IMAGE_NULLABLE_OPTION_KEYS.some((option) => option === key)));

const rejectInvalidImageEditContentLength = async (req: Request, declaredLength: string): Promise<ImageRequestFailure | null> => {
  const normalizedLength = declaredLength.trim();
  if (!/^(?:0|[1-9]\d*)$/u.test(normalizedLength)) {
    discardRawBodyObserverOnce(req);
    return imageRequestError("Multipart Content-Length is invalid.");
  }
  const parsedLength = Number(normalizedLength);
  if (!Number.isSafeInteger(parsedLength) || parsedLength > IMAGE_MAX_MULTIPART_BODY_BYTES) {
    discardRawBodyObserverOnce(req);
    await req.body?.cancel().catch(() => {});
    return imageRequestError("Multipart image edits must be no larger than 64 MiB.", "image");
  }
  return null;
};

const readImageEditMultipartForm = async (req: Request, bytes: Uint8Array<ArrayBuffer>): Promise<FormData | null> => {
  try {
    return await new Request(req.url, {
      method: req.method,
      headers: req.headers,
      body: bytes,
    }).formData();
  } catch {
    return null;
  }
};

const collectImageEditFields = (form: FormData, body: Record<string, unknown>): ImageRequestFailure | null => {
  const unsupportedField = [...form.keys()].find((key) => !IMAGE_EDIT_MULTIPART_REQUEST_KEYS.has(key));
  if (unsupportedField) {
    return imageRequestError(`Unsupported image edit field: ${unsupportedField}.`, unsupportedField);
  }
  for (const key of ["model", "prompt", "size", "quality", "background", "output_format", "input_fidelity", "response_format", "user"]) {
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
  return null;
};

const selectImageEditFiles = (form: FormData): Readonly<{ ok: true; imageFiles: File[]; maskFile: File | null }> | ImageRequestFailure => {
  const imageEntries = [...form.entries()].flatMap(([name, entry]) => (name === "image" || name === "image[]" ? [entry] : []));
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
  return { ok: true, imageFiles: imageEntries as File[], maskFile: masks[0] instanceof File ? masks[0] : null };
};

const validateImageEditMask = (maskFile: File | null): Readonly<{ ok: true; mediaType: string | null }> | ImageRequestFailure => {
  if (maskFile === null) return { ok: true, mediaType: null };
  const mediaType = imageFileMediaType(maskFile);
  if (maskFile.size === 0 || maskFile.size >= IMAGE_MAX_MASK_FILE_BYTES || mediaType !== "image/png") {
    return imageRequestError("The multipart mask must be a non-empty PNG file smaller than 50 MiB.", "mask");
  }
  return { ok: true, mediaType };
};

const finalizeImageEditBody = async (form: FormData, body: Record<string, unknown>, count: number): Promise<MultipartImageEditResult> => {
  const selection = selectImageEditFiles(form);
  if (!selection.ok) return selection;
  const { imageFiles, maskFile } = selection;
  if (imageFiles.some((file) => file.size === 0 || file.size >= IMAGE_MAX_FILE_BYTES)) {
    return imageRequestError("Each multipart image file must be non-empty and smaller than 50 MiB.", "image");
  }
  const imageInputs = imageFiles.flatMap((file): { file: File; mediaType: string }[] => {
    const mediaType = imageFileMediaType(file);
    return mediaType === null ? [] : [{ file, mediaType }];
  });
  if (imageInputs.length !== imageFiles.length) {
    return imageRequestError("Each multipart image must be a PNG, JPEG, or WebP file.", "image");
  }
  const mask = validateImageEditMask(maskFile);
  if (!mask.ok) return mask;
  const maskMediaType = mask.mediaType;
  const files = maskFile ? [...imageFiles, maskFile] : imageFiles;
  const totalInputBytes = files.reduce((total, file) => total + file.size, 0);
  if (totalInputBytes > IMAGE_MAX_MULTIPART_INPUT_BYTES) {
    return imageRequestError("Multipart image files must total no more than 50 MiB.", "image");
  }
  if (totalInputBytes > Math.floor(IMAGE_MAX_FANOUT_INPUT_BYTES / count)) {
    return imageRequestError("n and multipart image inputs exceed the 50 MiB request work limit.", "n");
  }
  const images: Record<string, string>[] = [];
  for (const { file, mediaType } of imageInputs) {
    images.push({ image_url: await fileDataUrl(file, mediaType) });
  }
  body.images = images;
  if (maskFile && maskMediaType !== null) {
    body.mask = {
      image_url: await fileDataUrl(maskFile, maskMediaType),
    };
  }
  return { ok: true, body, inputBytes: totalInputBytes };
};

const parseMultipartImageEdit = async (req: Request): Promise<MultipartImageEditResult> => {
  const declaredLength = req.headers.get("content-length");
  if (declaredLength !== null) {
    const lengthFailure = await rejectInvalidImageEditContentLength(req, declaredLength);
    if (lengthFailure) return lengthFailure;
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
    const form = await readImageEditMultipartForm(req, bytes);
    if (!form) return imageRequestError("Image edits must include valid multipart form data.");
    captured = captureRawBodyOnce(req, bytes);
    const body: Record<string, unknown> = {};
    const fieldFailure = collectImageEditFields(form, body);
    if (fieldFailure) return fieldFailure;
    const sanitizedBody = withoutNullImageOptions(body);
    const scalarValidation = validateImageScalarOptions(sanitizedBody, "edits", true);
    if (!scalarValidation.ok) return scalarValidation;
    // GPT Image responses are always base64. The validated multipart
    // compatibility field has no Responses-tool equivalent.
    delete sanitizedBody.response_format;
    return await finalizeImageEditBody(form, sanitizedBody, scalarValidation.count);
  } finally {
    discardRawBodyObserverOnce(req);
    if (bytes && !captured) bytes.fill(0);
  }
};

const imageJsonAllowedKeys = (kind: ImageRouteKind): ReadonlySet<string> => (kind === "edits" ? IMAGE_EDIT_JSON_REQUEST_KEYS : IMAGE_GENERATION_REQUEST_KEYS);

const normalizeImageEditReferences = (body: Record<string, unknown>, isMultipart: boolean): ImageRequestFailure | null => {
  if (!Array.isArray(body.images) || body.images.length === 0 || body.images.length > IMAGE_MAX_EDIT_INPUTS) {
    return imageRequestError(`images must contain from 1 to ${IMAGE_MAX_EDIT_INPUTS} image references.`, "images");
  }
  if (isMultipart) return null;
  const images = body.images.map(normalizeImageReference);
  if (images.some((image) => image === null)) {
    return imageRequestError(
      "Each image must contain one image_url with a fully qualified HTTP(S) URL or supported base64 data URL; file_id is unsupported by this gateway.",
      "images"
    );
  }
  body.images = images;
  if (Object.prototype.hasOwnProperty.call(body, "mask")) {
    const mask = normalizeImageMaskReference(body.mask);
    if (!mask) {
      return imageRequestError("mask.image_url must be a supported base64 PNG data URL; remote URLs and file_id are unsupported by this gateway.", "mask");
    }
    body.mask = mask;
  }
  return null;
};

const finalizeImageRequest = (raw: unknown, kind: ImageRouteKind, multipartInputBytes: number, isMultipart: boolean): ParsedImageRequest => {
  if (!isPlainRecord(raw)) return imageRequestError("Image requests must use an object body.");
  const unsupportedField = findUnknownKey(raw, imageJsonAllowedKeys(kind));
  if (unsupportedField) {
    return imageRequestError(`Unsupported image ${kind} field: ${unsupportedField}.`, unsupportedField);
  }
  const body = withoutNullImageOptions(raw);
  const scalarValidation = validateImageScalarOptions(body, kind, isMultipart);
  if (!scalarValidation.ok) return scalarValidation;
  const { count } = scalarValidation;
  if (kind === "edits") {
    const referenceFailure = normalizeImageEditReferences(body, isMultipart);
    if (referenceFailure) return referenceFailure;
  }
  const inputBytes = Math.max(multipartInputBytes, imageReferenceInputBytes(body));
  if (inputBytes > Math.floor(IMAGE_MAX_FANOUT_INPUT_BYTES / count)) {
    return imageRequestError("n and inline image inputs exceed the 50 MiB request work limit.", "n");
  }
  return { ok: true, body, count, inputBytes };
};

const parseImageRequest = async (req: Request, kind: ImageRouteKind): Promise<ParsedImageRequest> => {
  const mediaType = req.headers.get("content-type")?.split(";", 1)[0]?.trim().toLowerCase() ?? "";
  if (kind === "edits" && mediaType === "multipart/form-data") {
    const multipart = await parseMultipartImageEdit(req);
    if (!multipart.ok) return multipart;
    return finalizeImageRequest(multipart.body, kind, multipart.inputBytes, true);
  }
  const raw = await readJsonBody(req, kind === "edits" ? IMAGE_MAX_JSON_BODY_BYTES : undefined);
  if (raw === null) {
    return imageRequestError(
      kind === "edits" ? "Image edits must use a JSON object or multipart form-data body." : "Image generation requests must use a JSON object body."
    );
  }
  return finalizeImageRequest(raw, kind, 0, false);
};

const imageResponseHeaders = (responses: readonly Response[]): Record<string, string> => {
  const headers: Record<string, string> = {};
  const warnings = Array.from(new Set(responses.flatMap(responseWarnings)));
  if (warnings.length > 0) headers[UOS_WARNING_HEADER] = warnings.join(", ");
  for (const name of ["x-uos-upstream", "Retry-After", ...STANDARD_RATE_LIMIT_HEADERS]) {
    const values = responses.map((response) => response.headers.get(name));
    const first = values[0];
    if (values.length > 0 && first !== null && values.every((value) => value !== null && value === first)) {
      headers[name] = first;
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
  beforeProviderDispatch: NonNullable<UsageContext["beforeProviderDispatch"]>
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

const imageCallUsageContext = (context: UsageContext | undefined, index: number, kind: ImageRouteKind): UsageContext | undefined => {
  if (!context) return undefined;
  const requestId = index === 0 || !context.requestId ? context.requestId : `${context.requestId}:image:${kind}:${index + 1}`;
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

const finalizeImageResponse = (sources: readonly Response[], target: Response, context: UsageContext | undefined): Response => {
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
export const extractImagesFromResponses = (payload: unknown): Record<string, unknown>[] => {
  if (!isPlainRecord(payload) || !Array.isArray(payload.output)) return [];
  const images: Record<string, unknown>[] = [];
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
    if (!isPlainRecord(item) || item.type !== "image_generation_call" || typeof item.result !== "string" || item.result.length === 0) continue;
    return typeof item.output_format === "string" && IMAGE_OUTPUT_FORMATS.has(item.output_format) ? item.output_format : null;
  }
  return null;
};

const createImageFanoutRuntime = (
  req: Request,
  kind: ImageRouteKind,
  usageContext: UsageContext | undefined,
  options: ImageHandlerOptions,
  responsesBody: Record<string, unknown>,
  serializedResponsesBody: string | null,
  count: number
) => {
  type ImageFanoutFailure = Readonly<{ kind: "response"; response: Response }> | Readonly<{ kind: "throw"; error: unknown }>;
  const fanoutDispatch =
    count > 1 && !options.dispatch && usageContext?.beforeProviderDispatch
      ? createImageFanoutDispatchCoordinator(count, usageContext.beforeProviderDispatch)
      : null;
  const fanoutAbort = count > 1 ? new AbortController() : null;
  const childSignal = fanoutAbort
    ? AbortSignal.any([req.signal, ...(usageContext?.downstreamSignal ? [usageContext.downstreamSignal] : []), fanoutAbort.signal])
    : req.signal;
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
    const coordinatedChildContext =
      childContext && (fanoutDispatch || fanoutAbort)
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
      const upstream = options.dispatch ? await options.dispatch(request) : await runResponsesHandler(request, coordinatedChildContext, responsesBody);
      if (!upstream.ok) recordFirstFailure({ kind: "response", response: upstream });
      return upstream;
    } catch (error) {
      recordFirstFailure({ kind: "throw", error });
      throw error;
    } finally {
      fanoutDispatch?.settled(index);
    }
  };
  return {
    runChild,
    settled: (index: number): void => {
      fanoutDispatch?.settled(index);
    },
    cancelBeforeTransport: async (): Promise<void> => {
      await fanoutDispatch?.cancelBeforeTransport().catch(() => {});
    },
    failure: (): ImageFanoutFailure | undefined => firstFailure,
  };
};

const runImageFanoutUpstreams = async (count: number, runtime: ReturnType<typeof createImageFanoutRuntime>): Promise<Response[]> => {
  const settledUpstreams: PromiseSettledResult<Response>[] = [];
  const indexes = Array.from({ length: count }, (_, index) => index);
  settledUpstreams.push(...(await Promise.allSettled(indexes.map(runtime.runChild))));
  const nextChildIndex = count;
  // The shared paid-dispatch coordinator waits for every logical child. Mark
  // calls that never started after a sibling failure as settled before cancel.
  for (let index = nextChildIndex; index < count; index += 1) runtime.settled(index);
  const failure = runtime.failure();
  if (failure !== undefined) {
    await runtime.cancelBeforeTransport();
    if (failure.kind === "throw") throw failure.error;
  }
  const upstreams =
    failure?.kind === "response"
      ? settledUpstreams.flatMap((result) => (result.status === "fulfilled" ? [result.value] : []))
      : settledUpstreams.map((result) => {
          if (result.status === "rejected") throw result.reason;
          return result.value;
        });
  if (failure?.kind === "response") {
    const leaderIndex = upstreams.indexOf(failure.response);
    if (leaderIndex > 0) upstreams.unshift(upstreams.splice(leaderIndex, 1)[0] as Response);
  }
  return upstreams;
};

const readImageUpstreamPayload = async (
  upstreams: readonly Response[],
  upstream: Response,
  usageContext: UsageContext | undefined
): Promise<Response | Readonly<{ payload: unknown }>> => {
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
  return { payload };
};

const mergeImageCallMetadata = (calls: { outputFormat: string | null; outputFormatConsistent: boolean; created: number | null }, payload: unknown): void => {
  const callOutputFormat = extractImageOutputFormatFromResponses(payload);
  if (callOutputFormat === null) calls.outputFormatConsistent = false;
  else if (calls.outputFormat === null) calls.outputFormat = callOutputFormat;
  else if (calls.outputFormat !== callOutputFormat) calls.outputFormatConsistent = false;
  if (calls.created === null && isPlainRecord(payload) && typeof payload.created_at === "number") {
    calls.created = payload.created_at;
  }
};

const buildImageAggregateResponse = async (upstreams: readonly Response[], usageContext: UsageContext | undefined): Promise<Response> => {
  const images: Record<string, unknown>[] = [];
  const calls: { outputFormat: string | null; outputFormatConsistent: boolean; created: number | null } = {
    outputFormat: null,
    outputFormatConsistent: true,
    created: null,
  };
  for (const upstream of upstreams) {
    const result = await readImageUpstreamPayload(upstreams, upstream, usageContext);
    if (result instanceof Response) return result;
    const callImages = extractImagesFromResponses(result.payload);
    if (callImages.length === 0) {
      const response = openaiError(502, "The model did not return an image for this request.", "image_generation_failed", {
        headers: imageResponseHeaders(upstreams),
      });
      return finalizeImageResponse(upstreams, response, usageContext);
    }
    images.push(callImages[0]);
    mergeImageCallMetadata(calls, result.payload);
  }
  const created = calls.created ?? Math.floor(Date.now() / 1000);
  const response = json(
    200,
    {
      created,
      data: images,
      ...(calls.outputFormatConsistent && calls.outputFormat ? { output_format: calls.outputFormat } : {}),
    },
    imageResponseHeaders(upstreams)
  );
  return finalizeImageResponse(upstreams, response, usageContext);
};

export const handleImages = async (req: Request, kind: ImageRouteKind, usageContext?: UsageContext, options: ImageHandlerOptions = {}): Promise<Response> => {
  const parsed = await parseImageRequest(req, kind);
  if (!parsed.ok) return parsed.response;
  const { body, count } = parsed;

  const baseModel = await resolveImageBaseModel();
  if (!baseModel) {
    return openaiError(503, "No base model is available to host image generation.", "model_unavailable");
  }

  const responsesBody = buildImageResponsesRequest(body, baseModel, kind);
  const encodedResponsesBody = JSON.stringify(responsesBody);
  if (count > 1 && IMAGE_TEXT_ENCODER.encode(encodedResponsesBody).byteLength > Math.floor(IMAGE_MAX_JSON_BODY_BYTES / count)) {
    return imageRequestError("n and the translated image request exceed the fan-out work limit.", "n").response;
  }
  const serializedResponsesBody = options.dispatch ? encodedResponsesBody : null;
  const runtime = createImageFanoutRuntime(req, kind, usageContext, options, responsesBody, serializedResponsesBody, count);
  const upstreams = await runImageFanoutUpstreams(count, runtime);
  return buildImageAggregateResponse(upstreams, usageContext);
};
