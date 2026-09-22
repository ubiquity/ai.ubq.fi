import {
  handleAdminApiKeysCreate,
  handleAdminApiKeysDelete,
  handleAdminApiKeysList,
  handleAdminApiKeysPaidFallbacks,
  handleAdminApiKeysRevoke,
  handleAdminApiKeysUnrevoke,
  handleAdminApiKeysUpdate,
  handleAdminCodexAuth,
  handleAdminCodexBankedResetShadowDecisions,
  handleAdminCodexCacheScopeExperiment,
  handleAdminCodexCacheScopeExperimentTelemetryBaseline,
  handleAdminCodexModelsGet,
  handleAdminCodexModelsSet,
  handleAdminCodexModelsWhitelistGet,
  handleAdminCodexModelsWhitelistSet,
  handleAdminCodexPromptsPurge,
  handleAdminCodexRecheck,
  handleAdminCodexResetSettings,
  handleAdminDebugRouting,
  handleAdminDefaults,
  handleAdminKernelPolicyQueueList,
  handleAdminKernelPubKeysCreate,
  handleAdminKernelPubKeysDelete,
  handleAdminKernelPubKeysList,
  handleAdminKernelUsageDelete,
  handleAdminKernelUsageGet,
  handleAdminKernelUsageSet,
  handleAdminKvMigrationImport,
  handleAdminKvMigrationValidate,
  handleAdminModelsCatalogGet,
  handleAdminModelsRefresh,
  handleAdminPromptCacheAnalytics,
  handleAdminProviderSelectionGet,
  handleAdminProviderSelectionSet,
  handleAdminProvidersQuotaProjection,
  handleAdminProvidersQuotaProjectionBackfill,
} from "./admin.ts";
import { handleAdminErrors, recordAdminError } from "./admin_error_log.ts";
import { handleAgentMessagesList, handleAgentMessagesPost } from "./agent_messages.ts";
import { authenticateAdmin, authenticateClient, getKernelAttestationContext, handleV1Auth, requireAdminAuth, requireSuperAdminAuth } from "./auth.ts";
import {
  type ApiKeyPolicy,
  ApiKeyQuotaDispatchError,
  apiKeyQuotaUsedPercent,
  apiKeyRateLimitPolicyHeaders,
  type ApiKeyUsageReservation,
  reserveApiKeyUsageV3,
} from "./api_key_policy.ts";
import { runtimeDeploymentId, runtimeGitSha } from "./config.ts";
import { handleAdminCodexSupervisorOutput, handleAdminCodexSupervisorSessions } from "./codex_supervisor.ts";
import { handleAdminCodexSupervisorBrief } from "./codex_supervisor_brief.ts";
import { handleHealth, handleHealthProviders, handleHealthUpstream } from "./health.ts";
import { corsHeaders, notFound, openaiError, withCors as withCorsHeaders, withoutBody } from "./http.ts";
import {
  acquireInferenceAdmission,
  DEFAULT_INFERENCE_ADMISSION_LIMITS,
  type InferenceAdmissionController,
  type InferenceAdmissionLocalOverloadCause,
  type InferenceAdmissionResult,
  inferenceAdmissionSnapshot,
} from "./inference_admission.ts";
import { handleJevResponsesCompaction, isJevCompactionRequest } from "./jev_compaction/compaction.ts";
import { type KernelQuotaReservation, reserveEffectiveKernelUsageLimit } from "./kernel_usage.ts";
import {
  getResponseAccountCohortId,
  getResponseTelemetry,
  handleChatCompletions,
  handleEmbeddingsJobCreate,
  handleEmbeddingsJobGet,
  handleImages,
  handleModelCapabilities,
  handleModels,
  handlePublicModelCatalog,
  handleResponses,
  handleUosEmbeddings,
  type ResponseTelemetry,
} from "./openai.ts";
import { enqueuePromptCacheAnalytics, recordPromptCacheAnalytics } from "./prompt_cache_analytics.ts";
import { recordPromptCacheTelemetry } from "./prompt_cache_telemetry_gate.ts";
import {
  handlePasskeyLoginFinish,
  handlePasskeyLoginStart,
  handlePasskeyLogout,
  handlePasskeyRegisterFinish,
  handlePasskeyRegisterStart,
  handlePasskeySession,
  handlePasskeyUsersList,
  handlePasskeyUsersUpdate,
} from "./passkeys.ts";
import { withCodexQuotaHeaders } from "./codex_quota.ts";
import { handleRoot, handleStaticAsset } from "./static.ts";
import { sha256Hex } from "./utils.ts";
import { handleProviderCapacity } from "./provider_capacity.ts";
import {
  type AcceptedSentinelReplayInput,
  captureAcceptedSentinelReplayInput,
  createSentinelSseInspector,
  discardSentinelReplayCaptureCandidate,
  disposeSentinelUpstreamRecorder,
  inspectSentinelBufferedResponseBody,
  materializeSentinelReplayInput,
  persistSentinelReplayFromEnvironment,
  recordSentinelReplayOmissionFromEnvironment,
  resolveSentinelClientFailureObservation,
  type SentinelClientBodyObservation,
  type SentinelFailureObservation,
  type SentinelReplayCaptureOmissionReason,
  shouldPersistSentinelReplay,
  snapshotSentinelReplayInput,
  zeroSentinelReplayInput,
} from "./sentinel_replay_capture.ts";
import { createSentinelUpstreamRecorder } from "./sentinel_upstream_capture.ts";
import { handleAdminSentinelReplayCaptures } from "./sentinel_replay_admin.ts";
import { handleAdminSentinelIncidents } from "./sentinel_incident_admin.ts";
import type { recordSentinelProviderDegradationFromEnvironment } from "./sentinel_incident_outbox.ts";

type ClientAuthResult = Awaited<ReturnType<typeof authenticateClient>>;
type AuthenticatedClientResult = Extract<ClientAuthResult, { ok: true }>;

type RequestDeliveryInfo = Readonly<{
  completed: Promise<void>;
  downstreamSignal: AbortSignal;
}>;

type DeliveryOutcome = "delivered" | "interrupted" | "unobserved";
type BodyOutcome = "drained" | "interrupted" | "failed";

type SentinelBackgroundTaskRegistrar = (task: Promise<unknown>) => void;
type SentinelBackgroundRuntime = Readonly<{
  waitUntil?: SentinelBackgroundTaskRegistrar;
}>;

const sentinelBackgroundTaskRegistrar = (): SentinelBackgroundTaskRegistrar | null => {
  const globals = globalThis as unknown as Readonly<{
    EdgeRuntime?: SentinelBackgroundRuntime;
  }>;
  if (typeof globals.EdgeRuntime?.waitUntil === "function") {
    return globals.EdgeRuntime.waitUntil.bind(globals.EdgeRuntime);
  }
  return null;
};

const scheduleSentinelBackgroundTask = (task: Promise<void>, registrar: SentinelBackgroundTaskRegistrar | undefined): boolean => {
  const waitUntil = registrar ?? sentinelBackgroundTaskRegistrar();
  if (!waitUntil) return false;
  try {
    waitUntil(task);
    return true;
  } catch {
    return false;
  }
};

export const shouldSignalSentinelProviderDegradation = (
  input: Readonly<{ status: number; completed: boolean; removedProviderTriggerClass: string | null }>
): boolean => input.status >= 200 && input.status < 400 && input.completed && input.removedProviderTriggerClass !== null;

type PrincipalAuthResult = Readonly<{
  token: string | null;
  method: Readonly<{ kind: "kv_api_key"; key_id: string }> | Exclude<AuthenticatedClientResult["method"], { kind: "kv_api_key" }>;
}>;

/** Exhaustiveness guard for authentication method kinds; unreachable at runtime. */
const assertNeverAuthMethod = (method: never): never => {
  throw new Error(`Unhandled authentication method: ${JSON.stringify(method)}`);
};

export const resolveIdempotencyPrincipal = async (authResult: PrincipalAuthResult): Promise<string> => {
  switch (authResult.method.kind) {
    case "kv_api_key":
      return `api-key:${authResult.method.key_id}`;
    case "github_token":
      return `github-repo:${authResult.method.owner.toLowerCase()}/${authResult.method.repo.toLowerCase()}`;
    case "passkey_session":
      return `passkey-user:${authResult.method.user_id}`;
    case "auth_tokens_allowlist":
    case "admin_allowlist":
    case "deno_deploy_token":
      return `auth-method:${authResult.method.kind}`;
    case "disabled":
      return authResult.token ? `bearer-sha256:${await sha256Hex(authResult.token)}` : "local-auth-disabled";
    default:
      // Every known method kind is handled above; this keeps the switch total.
      return assertNeverAuthMethod(authResult.method);
  }
};

const normalizePath = (path: string): string => {
  if (path === "/") return path;
  // Equivalent to `path.replace(/\/+$/, "")`, without the quadratic
  // backtracking that pattern needs when a request path ends in many slashes.
  let end = path.length;
  while (end > 0 && path[end - 1] === "/") end -= 1;
  return path.slice(0, end);
};

const withRequestId = (response: Response, requestId: string): Response => {
  const headers = new Headers(response.headers);
  headers.set("x-uos-request-id", requestId);
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
};

const decorateInferenceQuota = (response: Response, policy: ApiKeyPolicy | null, telemetry: ResponseTelemetry | null): Response => {
  const usedPercent = telemetry?.quotaUsedPercent !== undefined ? telemetry.quotaUsedPercent : apiKeyQuotaUsedPercent(policy);
  const codexDecorated = withCodexQuotaHeaders(response, usedPercent === null ? null : { used_percent: usedPercent });
  const headers = new Headers(codexDecorated.headers);
  for (const [name, value] of Object.entries(apiKeyRateLimitPolicyHeaders(policy))) headers.set(name, value);
  return new Response(codexDecorated.body, {
    status: codexDecorated.status,
    statusText: codexDecorated.statusText,
    headers,
  });
};

const providerRequestIdHeaderValue = (value: string | null): string | null => {
  const requestId = value?.trim();
  if (!requestId || requestId.length > 256) return null;
  for (const character of requestId) {
    const code = character.codePointAt(0) ?? 0;
    if (code < 0x20 || code === 0x7f) return null;
  }
  return requestId;
};

/**
 * Provider-native correlation headers. The gateway never reflects them: it
 * exposes one bounded `x-uos-provider-request-id` whose value has already
 * passed the gateway sanitizer.
 *
 * Every header a provider reader accepts must be listed here, or that
 * provider's own spelling can survive to the client.
 * `getCerebrasProviderRequestId` reads the first four spellings and
 * `getDeepSeekProviderRequestId` reads `x-request-id`, `x-ds-request-id` and
 * `x-deepseek-request-id`.
 */
export const PROVIDER_NATIVE_CORRELATION_HEADERS = [
  "x-request-id",
  "x-api-request-id",
  "x-oneapi-request-id",
  "x-cerebras-request-id",
  "x-ds-request-id",
  "x-deepseek-request-id",
  "x-uos-provider-request-id",
] as const;

export const scrubProviderNativeCorrelationHeaders = (headers: Headers): void => {
  for (const header of PROVIDER_NATIVE_CORRELATION_HEADERS) headers.delete(header);
};

const withProviderRequestId = (response: Response, providerRequestId: string | null): Response => {
  const requestId = providerRequestIdHeaderValue(providerRequestId);
  const headers = new Headers(response.headers);
  scrubProviderNativeCorrelationHeaders(headers);
  if (requestId) headers.set("x-uos-provider-request-id", requestId);
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
};

const logTerminalRequest = async (
  input: Readonly<{
    route: string;
    response: Response;
    telemetryResponse?: Response;
    startedAtMonotonicMs: number;
    downstreamDrainedAtMonotonicMs?: number;
    deliveryOutcome: DeliveryOutcome;
    requestId: string;
    recordCacheAnalytics?: typeof recordPromptCacheAnalytics;
    recordTelemetry?: typeof recordPromptCacheTelemetry;
    sentinelReplayInput?: AcceptedSentinelReplayInput | null;
    persistSentinelReplay?: typeof persistSentinelReplayFromEnvironment;
    recordSentinelDegradation?: typeof recordSentinelProviderDegradationFromEnvironment;
    recordAdminError?: typeof recordAdminError;
    streamReadFailure?: boolean;
    suppressSentinelReplay?: boolean;
    /** Time this request spent waiting for a process-resource permit, if it queued. */
    admissionWaitMs?: number | null;
    resolveClientBodyObservation?: () => SentinelClientBodyObservation | null | Promise<SentinelClientBodyObservation | null>;
  }>
): Promise<void> => {
  const telemetry = getResponseTelemetry(input.telemetryResponse ?? input.response);
  const accountCohortId = getResponseAccountCohortId(input.telemetryResponse ?? input.response);
  const latencyMs = Math.max(0, Math.round(performance.now() - input.startedAtMonotonicMs));
  const downstreamDrainMs =
    telemetry?.stream === true &&
    telemetry.firstSemanticCommitmentMs !== null &&
    telemetry.streamTerminalMs !== null &&
    input.downstreamDrainedAtMonotonicMs !== undefined
      ? Math.max(0, Math.round(input.downstreamDrainedAtMonotonicMs - input.startedAtMonotonicMs) - telemetry.streamTerminalMs)
      : null;
  const terminal = {
    request_id: input.requestId,
    route: input.route,
    status: input.response.status,
    provider: telemetry?.provider ?? input.response.headers.get("x-uos-upstream") ?? "gateway",
    latency_ms: latencyMs,
    admission_wait_ms: input.admissionWaitMs ?? null,
    first_provider_dispatch_ms: telemetry?.firstProviderDispatchMs ?? null,
    first_provider_headers_ms: telemetry?.firstProviderHeadersMs ?? null,
    first_codex_dispatch_ms: telemetry?.firstCodexDispatchMs ?? null,
    first_codex_headers_ms: telemetry?.firstCodexHeadersMs ?? null,
    first_upstream_sse_event_ms: telemetry?.firstUpstreamSseEventMs ?? null,
    first_semantic_commitment_ms: telemetry?.firstSemanticCommitmentMs ?? null,
    stream_terminal_ms: telemetry?.streamTerminalMs ?? null,
    downstream_drain_ms: downstreamDrainMs,
    delivery_outcome: input.deliveryOutcome,
    model: telemetry?.model ?? null,
    reasoning: telemetry?.reasoning ?? null,
    output_token_allowance: telemetry?.outputTokenAllowance ?? null,
    provider_request_id: telemetry?.providerRequestId ?? null,
    input_tokens: telemetry?.inputTokens ?? null,
    cached_input_tokens: telemetry?.cachedInputTokens ?? null,
    cache_write_input_tokens: telemetry?.cacheWriteInputTokens ?? null,
    output_tokens: telemetry?.outputTokens ?? null,
    total_tokens: telemetry?.totalTokens ?? null,
    usage_observed: telemetry?.usageObserved ?? false,
    usage_telemetry_status: telemetry?.usageTelemetryStatus ?? "missing",
    prompt_cache_key_present: telemetry?.promptCacheKeyPresent ?? false,
    prompt_cache_mode: telemetry?.promptCacheMode ?? "unspecified",
    explicit_breakpoint_count: telemetry?.explicitBreakpointCount ?? 0,
    account_slot: telemetry?.accountSlot ?? null,
    account_cohort_id: accountCohortId,
    active_generation: telemetry?.activeGeneration ?? null,
    active_transition_reason: telemetry?.activeTransitionReason ?? null,
    fallback_reason: telemetry?.fallbackReason ?? null,
    semantic_output_observed: telemetry?.semanticOutputObserved ?? null,
    upstream_event_kinds: telemetry?.upstreamEventKinds ?? [],
    stream: input.streamReadFailure ? (telemetry?.stream ?? true) : (telemetry?.stream ?? null),
    stream_terminal_type: input.streamReadFailure ? (telemetry?.streamTerminalType ?? "error") : (telemetry?.streamTerminalType ?? null),
    failure_kind: input.streamReadFailure ? (telemetry?.failureKind ?? "gateway_stream_read_error") : (telemetry?.failureKind ?? null),
    response_created_observed: telemetry?.responseCreatedObserved ?? false,
    synthetic_terminal_type: telemetry?.syntheticTerminalType ?? null,
    attempted_providers: telemetry?.attemptedProviders ?? [],
    removed_provider_trigger_class: telemetry?.removedProviderTriggerClass ?? null,
    removed_provider_circuit_transition: telemetry?.removedProviderCircuitTransition ?? null,
    removed_provider_selected_model: telemetry?.removedProviderSelectedModel ?? null,
    removed_provider_task_type: telemetry?.removedProviderTaskType ?? null,
    removed_provider_latency_ms: telemetry?.removedProviderLatencyMs ?? null,
    removed_provider_terminal_status: telemetry?.removedProviderTerminalStatus ?? null,
    removed_provider_semantic_commitment: telemetry?.removedProviderSemanticCommitment ?? null,
    git_sha: runtimeGitSha(),
    deno_revision: runtimeDeploymentId(),
    router_revision: input.response.headers.get("x-uos-router-revision"),
  };
  console.info("[ai.ubq.fi] request_terminal", JSON.stringify(terminal));
  const telemetryWrite = (input.recordTelemetry ?? recordPromptCacheTelemetry)({
    provider: terminal.provider,
    model: terminal.model,
    route: terminal.route,
    status: terminal.status,
    completed: input.streamReadFailure ? false : (telemetry?.completed ?? false),
    usageTelemetryStatus: terminal.usage_telemetry_status,
    cacheWriteTokensPresent: terminal.cache_write_input_tokens !== null,
  });
  // Optional aggregate analytics is enqueued on the bounded best-effort queue
  // by default, so a slow analytics sink cannot extend the terminal handoff.
  // The explicit test seam still injects the direct writer. Every other write
  // in this batch stays awaited: the durable gate counters, admin error
  // evidence, Sentinel replay/degradation and quota accounting are reliable.
  const cacheAnalyticsEvent = {
    provider: terminal.provider,
    model: terminal.model,
    route: terminal.route,
    status: terminal.status,
    completed: input.streamReadFailure ? false : (telemetry?.completed ?? false),
    usageTelemetryStatus: terminal.usage_telemetry_status,
    inputTokens: terminal.input_tokens,
    cachedInputTokens: terminal.cached_input_tokens,
    cacheWriteInputTokens: terminal.cache_write_input_tokens,
    promptCacheKeyPresent: terminal.prompt_cache_key_present,
    promptCacheMode: terminal.prompt_cache_mode,
    fallbackReason: terminal.fallback_reason,
  };
  const cacheAnalyticsWrite = input.recordCacheAnalytics ? input.recordCacheAnalytics(cacheAnalyticsEvent) : enqueuePromptCacheAnalytics(cacheAnalyticsEvent);
  const replayObservation: SentinelFailureObservation = {
    status: terminal.status,
    stream: terminal.stream,
    completed: input.streamReadFailure ? false : (telemetry?.completed ?? false),
    terminal_type: terminal.stream_terminal_type,
    failure_kind: terminal.failure_kind,
    synthetic_terminal_type: terminal.synthetic_terminal_type,
    provider_route: terminal.provider,
  };
  try {
    const clientBodyObservation = (await input.resolveClientBodyObservation?.()) ?? null;
    const clientObservation = resolveSentinelClientFailureObservation(replayObservation, clientBodyObservation);
    const replayWrite =
      input.sentinelReplayInput && !input.suppressSentinelReplay && shouldPersistSentinelReplay(replayObservation, clientObservation)
        ? (input.persistSentinelReplay ?? persistSentinelReplayFromEnvironment)(input.sentinelReplayInput, replayObservation, clientObservation)
        : Promise.resolve();
    const degradationWrite = shouldSignalSentinelProviderDegradation({
      status: terminal.status,
      completed: telemetry?.completed ?? false,
      removedProviderTriggerClass: terminal.removed_provider_trigger_class,
    })
      ? input.recordSentinelDegradation?.(Date.now())
      : Promise.resolve();
    const adminErrorWrite = (input.recordAdminError ?? recordAdminError)({
      request_id: terminal.request_id,
      route: terminal.route,
      status: terminal.status,
      provider: terminal.provider,
      model: terminal.model,
      reasoning: terminal.reasoning,
      stream: terminal.stream,
      terminal_type: clientObservation.terminal_type,
      failure_kind: clientObservation.failure_kind,
      delivery_outcome: terminal.delivery_outcome,
      created_at_ms: Date.now(),
      latency_ms: terminal.latency_ms,
      git_sha: terminal.git_sha,
      deno_revision: terminal.deno_revision,
    });
    await Promise.all([telemetryWrite, cacheAnalyticsWrite, replayWrite, degradationWrite, adminErrorWrite]);
  } finally {
    zeroSentinelReplayInput(input.sentinelReplayInput);
  }
};

export const warnQuotaAccountingFailure = (input: Readonly<{ route: string; requestId: string }>, error: unknown): void => {
  const errors = error instanceof AggregateError ? error.errors : [error];
  try {
    console.warn(
      "[ai.ubq.fi] quota_accounting_failed",
      JSON.stringify({
        request_id: input.requestId,
        route: input.route,
        errors: errors.map((item) => ({
          class: item instanceof Error ? item.name : typeof item,
        })),
      })
    );
  } catch {
    // Accounting and its warning are both best-effort after completion. Neither
    // may replace an upstream response that is already ready for the client.
  }
};

/**
 * Runs one best-effort Sentinel replay write.  A synchronous throw from the
 * persistence call and a rejected write both settle as success, so capture
 * persistence can never replace a response that is already ready for the
 * client.
 */
const persistSentinelReplayBestEffort = async (
  persist: typeof persistSentinelReplayFromEnvironment,
  input: AcceptedSentinelReplayInput,
  observation: SentinelFailureObservation,
  clientObservation: ReturnType<typeof resolveSentinelClientFailureObservation>
): Promise<void> => {
  try {
    await persist(input, observation, clientObservation);
  } catch {
    // Capture persistence is best effort and must not replace the response.
  }
};

export const withTerminalRequestLog = (
  response: Response,
  input: Readonly<{
    route: string;
    telemetryResponse?: Response;
    startedAtMonotonicMs: number;
    requestId: string;
    onTerminal?: (outcome: "completed" | "incomplete", reason?: string) => Promise<void>;
    /**
     * Runs exactly once, when the wrapped response's body and delivery have
     * settled (or the application terminal has been observed for a buffered
     * response). It owns the process-resource permit release, so it must never
     * await accounting or replace the response.
     */
    onSettled?: () => void;
    deliveryCompleted?: Promise<void>;
    deliverySignal?: AbortSignal;
    /** Time this request spent waiting for a process-resource permit, if it queued. */
    admissionWaitMs?: number | null;
    /** Test seam for proving aggregate cache analytics remains best effort. */
    recordCacheAnalytics?: typeof recordPromptCacheAnalytics;
    /** Test seam for proving terminal telemetry remains best effort. */
    recordTelemetry?: typeof recordPromptCacheTelemetry;
    /** Test seam for proving failed replay persistence and successful-request exclusion. */
    sentinelReplayInput?: AcceptedSentinelReplayInput | null;
    persistSentinelReplay?: typeof persistSentinelReplayFromEnvironment;
    /** Why this request carries no captured body; published for persistable failures only. */
    sentinelReplayOmission?: SentinelReplayCaptureOmissionReason | null;
    /** Test seam for proving a body-less failure still publishes its capture status. */
    recordSentinelReplayOmission?: typeof recordSentinelReplayOmissionFromEnvironment;
    recordSentinelDegradation?: typeof recordSentinelProviderDegradationFromEnvironment;
    recordAdminError?: typeof recordAdminError;
    waitUntil?: SentinelBackgroundTaskRegistrar;
  }>
): Promise<Response> => {
  const contentType = response.headers.get("Content-Type")?.toLowerCase() ?? "";
  const isSse = contentType.includes("text/event-stream");
  const initialTelemetry = getResponseTelemetry(input.telemetryResponse ?? response);
  let clientBodyObservation: SentinelClientBodyObservation | null = null;
  let bufferedObservation: Promise<SentinelClientBodyObservation | null> | null = null;
  if (
    !isSse &&
    response.body &&
    (response.status >= 400 ||
      response.status === 202 ||
      input.route.startsWith("embeddings.jobs.") ||
      (initialTelemetry?.completed === false &&
        (initialTelemetry.streamTerminalType !== null || initialTelemetry.failureKind !== null || initialTelemetry.syntheticTerminalType !== null)))
  ) {
    bufferedObservation = inspectSentinelBufferedResponseBody(response);
  }
  let replayFinalization: Promise<void> | null = null;
  /** The same internal failure observation feeds capture persistence and omission status. */
  const replayObservationFor = (streamReadFailure: boolean): SentinelFailureObservation => {
    const telemetry = getResponseTelemetry(input.telemetryResponse ?? response);
    return {
      status: response.status,
      stream: streamReadFailure ? (telemetry?.stream ?? true) : (telemetry?.stream ?? null),
      completed: streamReadFailure ? false : (telemetry?.completed ?? false),
      terminal_type: streamReadFailure ? (telemetry?.streamTerminalType ?? "error") : (telemetry?.streamTerminalType ?? null),
      failure_kind: streamReadFailure ? (telemetry?.failureKind ?? "gateway_stream_read_error") : (telemetry?.failureKind ?? null),
      synthetic_terminal_type: telemetry?.syntheticTerminalType ?? null,
      provider_route: telemetry?.provider ?? response.headers.get("x-uos-upstream") ?? "gateway",
    };
  };
  const persistReplayAtApplicationTerminal = (streamReadFailure = false): Promise<void> => {
    if (replayFinalization) return replayFinalization;
    const originalReplayInput = input.sentinelReplayInput;
    // One snapshot copies the body and seals the request-owned upstream
    // recorder together, before the original is zeroed or any await. The
    // recorder is disposed inside the snapshot: zeroing the original can
    // never erase captured upstream evidence, and HMAC/encryption use the
    // same immutable trace.
    const backgroundReplayInput = originalReplayInput ? snapshotSentinelReplayInput(originalReplayInput) : null;
    // The background task owns the independent snapshot. Release the
    // request-owned bytes before any inspection or persistence await so a
    // stalled clone, crypto operation, or KV write cannot retain both copies.
    zeroSentinelReplayInput(originalReplayInput);
    replayFinalization = (async () => {
      const observation = replayObservationFor(streamReadFailure);
      if (!backgroundReplayInput) {
        // Nothing was carried into a capture. A persistable failure still
        // publishes its explicit omission status so a missing key, an omitted
        // body or a rejected request is never an empty replay history.
        if (input.sentinelReplayOmission) {
          const bodyObservation = clientBodyObservation ?? (await bufferedObservation);
          const clientObservation = resolveSentinelClientFailureObservation(observation, bodyObservation);
          if (shouldPersistSentinelReplay(observation, clientObservation)) {
            await (input.recordSentinelReplayOmission ?? recordSentinelReplayOmissionFromEnvironment)(
              input.requestId,
              input.sentinelReplayOmission,
              Date.now()
            );
          }
        }
        return;
      }
      const startReplayPersistence = (clientObservation: ReturnType<typeof resolveSentinelClientFailureObservation>): Promise<void> => {
        if (!shouldPersistSentinelReplay(observation, clientObservation)) return Promise.resolve();
        return persistSentinelReplayBestEffort(
          input.persistSentinelReplay ?? persistSentinelReplayFromEnvironment,
          backgroundReplayInput,
          observation,
          clientObservation
        );
      };
      // Never persist from a status-only fallback when an observed downstream
      // body is available: the client-visible error code/param and the bounded
      // terminal body are required replay evidence. The bounded clone was
      // started as soon as the response was known, so waiting for it keeps the
      // real HTTP failure capture as rich as the embedded-call one.
      const bodyObservation = clientBodyObservation ?? (await bufferedObservation);
      const clientObservation = resolveSentinelClientFailureObservation(observation, bodyObservation);
      await startReplayPersistence(clientObservation);
    })()
      .catch(() => {
        // Capture persistence is best effort and must not replace the response.
      })
      .finally(() => {
        zeroSentinelReplayInput(backgroundReplayInput);
        zeroSentinelReplayInput(originalReplayInput);
      });
    return replayFinalization;
  };
  let terminalLog: Promise<void> | null = null;
  let terminalFinalization: Promise<void> | null = null;
  let terminalIntent: Readonly<{ outcome: "completed" | "incomplete"; reason?: string }> | null = null;
  let terminalSettled = false;
  const log = (
    downstreamDrainedAtMonotonicMs?: number,
    deliveryOutcome: DeliveryOutcome = "unobserved",
    streamReadFailure = false,
    suppressSentinelReplay = false
  ): Promise<void> => {
    if (terminalLog) return terminalLog;
    // The response body and its delivery are settled here, so the process
    // resources behind this request are released before the durable terminal
    // writes start. Accounting has already been scheduled by the wrapper and is
    // never cancelled, refunded or duplicated by this release.
    try {
      input.onSettled?.();
    } catch {
      // A release fault cannot replace a response that is already delivered.
    }
    terminalLog = logTerminalRequest({
      ...input,
      // Replay persistence owns cleanup after it has taken its snapshot. Do
      // not let terminal logging clear the original body first.
      sentinelReplayInput: replayFinalization === null ? input.sentinelReplayInput : null,
      response,
      downstreamDrainedAtMonotonicMs,
      deliveryOutcome,
      streamReadFailure,
      suppressSentinelReplay: suppressSentinelReplay || replayFinalization !== null,
      resolveClientBodyObservation: async () => clientBodyObservation ?? (await bufferedObservation),
    }).catch(() => {
      // Terminal logging and its durable baseline counters are best effort;
      // neither may replace a response that is already ready for the client.
    });
    return terminalLog;
  };
  const deliveryOutcome = input.deliveryCompleted
    ? input.deliveryCompleted.then(
        () => (input.deliverySignal?.aborted ? ("interrupted" as const) : ("delivered" as const)),
        () => "interrupted" as const
      )
    : null;
  const finalizeTerminal = (outcome: "completed" | "incomplete", reason?: string): Promise<void> => {
    const onTerminal = input.onTerminal;
    if (!onTerminal) return Promise.resolve();
    if (outcome === "completed" && !terminalSettled) terminalIntent = { outcome, reason };
    else terminalIntent ??= { outcome, reason };
    if (terminalSettled) return Promise.resolve();
    if (terminalFinalization) {
      const pending = terminalFinalization;
      return pending.then(() => (terminalSettled ? undefined : finalizeTerminal(outcome, reason)));
    }
    const intended = terminalIntent;
    const current = (async () => {
      let lastError: unknown = null;
      for (let attempt = 0; attempt < 3; attempt += 1) {
        try {
          await onTerminal(intended.outcome, intended.reason);
          terminalSettled = true;
          return;
        } catch (error) {
          lastError = error;
        }
      }
      warnQuotaAccountingFailure(input, lastError);
    })().finally(() => {
      if (terminalFinalization === current) terminalFinalization = null;
    });
    terminalFinalization = current;
    return current;
  };
  const finalizeObservedCompletion = (): Promise<void> => {
    if (!response.ok) return Promise.resolve();
    const telemetry = getResponseTelemetry(input.telemetryResponse ?? response);
    if (!telemetry?.completed) return Promise.resolve();
    return finalizeTerminal("completed");
  };
  const finalizeFromTelemetry = (reason: string): Promise<void> => {
    const telemetry = getResponseTelemetry(input.telemetryResponse ?? response);
    return response.ok && telemetry?.completed ? finalizeTerminal("completed") : finalizeTerminal("incomplete", reason);
  };
  if (!response.body || !isSse) {
    return (async () => {
      try {
        await finalizeFromTelemetry(response.ok ? "response_incomplete" : "response_error");
        // Deno can return the already-computed response while this best-effort
        // capture continues in the background. In particular, buffered replay
        // inspection, compression, encryption, and KV writes must not extend
        // client-visible gateway error latency.
        const replayTask = persistReplayAtApplicationTerminal();
        if (!scheduleSentinelBackgroundTask(replayTask, input.waitUntil)) await replayTask;
        return response;
      } finally {
        if (deliveryOutcome) {
          void deliveryOutcome.then((outcome) => log(undefined, outcome, false, true));
        } else {
          await log(undefined, "unobserved", false, true);
        }
      }
    })();
  }

  const reader = response.body.getReader();
  const sseInspector = createSentinelSseInspector();
  let sseInspectionFailed = false;
  const inspectSseChunk = (value: Uint8Array): void => {
    if (sseInspectionFailed) return;
    try {
      sseInspector.push(value);
    } catch {
      sseInspectionFailed = true;
    }
  };
  const finishSseInspection = (termination: "eof" | "read_error" = "eof"): SentinelClientBodyObservation | null => {
    if (sseInspectionFailed) return null;
    try {
      return sseInspector.finish(termination);
    } catch {
      return null;
    }
  };
  let downstreamDrainedAtMonotonicMs: number | undefined;
  let settleBody: ((outcome: BodyOutcome) => void) | null = null;
  let bodyDidSettle = false;
  let downstreamCancelled = false;
  const bodyOutcome = deliveryOutcome
    ? new Promise<BodyOutcome>((resolve) => {
        settleBody = (outcome) => {
          if (bodyDidSettle) return;
          bodyDidSettle = true;
          resolve(outcome);
        };
      })
    : null;
  if (bodyOutcome && deliveryOutcome) {
    void Promise.all([bodyOutcome, deliveryOutcome]).then(([bodyResult, deliveryResult]) =>
      log(downstreamDrainedAtMonotonicMs, bodyResult === "drained" ? deliveryResult : "interrupted", bodyResult === "failed", bodyResult === "interrupted")
    );
  }
  /** Releases the request-owned replay capture before terminal cleanup runs. */
  const releaseReplayOwnership = (): void => {
    zeroSentinelReplayInput(input.sentinelReplayInput);
    disposeSentinelUpstreamRecorder(input.sentinelReplayInput);
  };
  /** Records an interrupted wrapper body and settles the body-outcome observer. */
  const settleInterruptedBody = async (): Promise<void> => {
    if (!deliveryOutcome) await log(undefined, "interrupted", false, true);
    settleBody?.("interrupted");
  };
  /** Cleans up after a failed provider read, then surfaces that error downstream. */
  const handleProviderReadFailure = async (error: unknown, controller: ReadableStreamDefaultController<Uint8Array>): Promise<void> => {
    clientBodyObservation = finishSseInspection("read_error");
    const downstreamAborted = downstreamCancelled || input.deliverySignal?.aborted === true;
    await finalizeFromTelemetry(downstreamAborted ? "downstream_cancelled" : "stream_read_error");
    if (downstreamAborted) {
      releaseReplayOwnership();
    } else {
      await persistReplayAtApplicationTerminal(true);
    }
    if (!deliveryOutcome) await log(undefined, "interrupted", !downstreamAborted, true);
    try {
      controller.error(error);
    } catch {
      // The downstream may have cancelled while the provider read failed.
    }
    settleBody?.(downstreamAborted ? "interrupted" : "failed");
  };
  /** Finishes the wrapper body once the provider stream reports EOF. */
  const finishProviderStream = async (controller: ReadableStreamDefaultController<Uint8Array>): Promise<void> => {
    clientBodyObservation = finishSseInspection();
    const downstreamAborted = downstreamCancelled || input.deliverySignal?.aborted === true;
    if (downstreamAborted) {
      await finalizeFromTelemetry("downstream_cancelled");
      releaseReplayOwnership();
      try {
        controller.close();
      } catch {
        // The downstream cancellation may already have closed the wrapper.
      }
      await settleInterruptedBody();
      return;
    }
    // Snapshot the downstream drain before finalization. Accounting can
    // wait on KV and belongs in total latency, not drain telemetry.
    downstreamDrainedAtMonotonicMs = performance.now();
    // The application stream has drained, but Deno still owns delivery.
    // Finish one-shot usage accounting before closing this wrapper, then
    // let `completed` classify the separate delivery outcome.
    await finalizeFromTelemetry("stream_eof_without_completion");
    await persistReplayAtApplicationTerminal();
    try {
      controller.close();
    } catch {
      await settleInterruptedBody();
      return;
    }
    if (!deliveryOutcome) await log(downstreamDrainedAtMonotonicMs, "unobserved", false, true);
    settleBody?.("drained");
  };
  const body = new ReadableStream<Uint8Array>({
    async pull(controller) {
      let result: ReadableStreamReadResult<Uint8Array>;
      try {
        result = await reader.read();
      } catch (error) {
        await handleProviderReadFailure(error, controller);
        return;
      }
      if (result.done) {
        await finishProviderStream(controller);
        return;
      }
      // The OpenAI stream observer marks response.completed before yielding
      // the chunk that contains it. Schedule accounting, but never hold back
      // the provider bytes that are already ready for the client.
      void finalizeObservedCompletion();
      inspectSseChunk(result.value);
      try {
        controller.enqueue(result.value);
      } catch {
        downstreamCancelled = true;
        void reader.cancel().catch(() => {});
        void finalizeFromTelemetry("downstream_enqueue_failed");
        releaseReplayOwnership();
        await settleInterruptedBody();
      }
    },
    cancel(reason) {
      // Cancellation must not await a concurrently pending provider pull;
      // that pull observes the cancellation and performs layered cleanup.
      downstreamCancelled = true;
      void reader.cancel(reason).catch(() => {});
      void finalizeFromTelemetry("downstream_cancelled");
      releaseReplayOwnership();
      if (!deliveryOutcome) void log(undefined, "interrupted", false, true);
      settleBody?.("interrupted");
    },
  });
  return Promise.resolve(
    new Response(body, {
      status: response.status,
      statusText: response.statusText,
      headers: response.headers,
    })
  );
};

const terminalRouteForRequest = (method: string, path: string): string | null => {
  if (method === "POST" && path === "/uos/embeddings") return "embeddings";
  if (method === "POST" && path === "/uos/embedding-jobs") return "embeddings.jobs.create";
  if (method === "GET" && path.startsWith("/uos/embedding-jobs/")) return "embeddings.jobs.get";
  if (method === "POST" && path === "/v1/chat/completions") return "chat.completions";
  if (method === "POST" && path === "/v1/responses") return "responses";
  if (method === "POST" && path === "/v1/images/generations") return "images.generations";
  if (method === "POST" && path === "/v1/images/edits") return "images.edits";
  return null;
};

const kernelQuotaRouteForRequest = (method: string, path: string): string | null => {
  if (method === "POST" && path === "/uos/embeddings") return "embeddings";
  if (method === "POST" && path === "/uos/embedding-jobs") return "embeddings.jobs.create";
  if (method === "POST" && path === "/v1/chat/completions") return "chat.completions";
  if (method === "POST" && path === "/v1/responses") return "responses";
  if (method === "POST" && path === "/v1/images/generations") return "images.generations";
  if (method === "POST" && path === "/v1/images/edits") return "images.edits";
  return null;
};

/**
 * Routes that hold process resources - an upstream transport, its retained
 * response buffer and the downstream body - for as long as the request lives.
 * Catalog reads (`/v1/models`) are deliberately absent: they never dispatch
 * provider inference and retain no provider response, so they must not consume
 * a permit. `embeddings.jobs.get` is present even though it also serves
 * completed-job reads, because a queued job poll calls
 * `runEmbeddingsJobAttempt`, which can dispatch `fetchVoyageEmbeddings`;
 * separating that work from an ordinary read would require job-state inspection
 * shared into the embeddings module, so every job poll is admitted instead of
 * letting a work-producing poll bypass the bound. The finite guard is the
 * merged internal controller, not caller-lane admission: it holds no per-caller
 * lease and caps no principal.
 */
const ADMISSION_ROUTES: ReadonlySet<string> = new Set([
  "embeddings",
  "embeddings.jobs.create",
  "embeddings.jobs.get",
  "chat.completions",
  "responses",
  "images.generations",
  "images.edits",
]);

/** The explicit local-overload error code; never a provider quota or fallback signal. */
export const LOCAL_INFERENCE_OVERLOAD_CODE = "local_inference_overload";

const sharedAdmissionController: InferenceAdmissionController = {
  acquire: acquireInferenceAdmission,
  snapshot: inferenceAdmissionSnapshot,
};

let admissionControllerForTest: InferenceAdmissionController | null = null;

/**
 * Internal test seam: install a small deterministic guard so an HTTP fixture
 * can reach the active and waiting bounds without 192 live requests. It is not
 * a runtime configuration surface, and null restores the shared controller.
 */
export const setInferenceAdmissionControllerForTest = (controller: InferenceAdmissionController | null): void => {
  admissionControllerForTest = controller;
};

const admissionController = (): InferenceAdmissionController => admissionControllerForTest ?? sharedAdmissionController;

/**
 * The local finite-overload refusal. It is a gateway decision, so it carries a
 * dedicated error code and a bounded Retry-After instead of borrowing provider
 * quota, capacity or paid-fallback vocabulary. The queue wait stays internal
 * telemetry rather than a response header.
 */
const localOverloadResponse = (cause: InferenceAdmissionLocalOverloadCause): Response => {
  const retryAfterSeconds = Math.max(1, Math.ceil(DEFAULT_INFERENCE_ADMISSION_LIMITS.maxQueueWaitMs / 1_000));
  const message =
    cause === "queue_limit"
      ? "The gateway is at its concurrent inference limit and its waiting queue is full."
      : `The gateway could not admit this request within ${DEFAULT_INFERENCE_ADMISSION_LIMITS.maxQueueWaitMs}ms.`;
  return openaiError(503, message, LOCAL_INFERENCE_OVERLOAD_CODE, {
    type: "server_error",
    param: null,
    headers: { "Retry-After": String(retryAfterSeconds) },
  });
};

// ---------------------------------------------------------------------------
// Request routing groups.  Each group returns the response it owns without CORS
// decoration, or null when the request belongs to a later group; the default
// export keeps the original order between the groups.
// ---------------------------------------------------------------------------

/** A route whose HTTP method and path are matched exactly. */
type ExactRouteEntry = Readonly<{
  methods: readonly string[];
  path: string;
  run: (req: Request) => Response | Promise<Response>;
}>;

/** An admin route, optionally restricted to super admins. */
type AdminRouteEntry = ExactRouteEntry & Readonly<{ superAdmin?: true }>;

/** The first exact-path route matching this request, if any. */
const matchExactRoute = <TRoute extends ExactRouteEntry>(routes: readonly TRoute[], req: Request, path: string): TRoute | undefined =>
  routes.find((entry) => entry.methods.includes(req.method) && entry.path === path);

/** Reads the optional client authentication used by the session and logout routes. */
const optionalClientAuth = async (req: Request): Promise<ClientAuthResult | null> =>
  req.headers.has("authorization") && req.headers.has("cookie") ? await authenticateClient(req) : null;

/** The passkey token to relay for an opportunistic client authentication. */
const passkeyTokenFrom = (auth: ClientAuthResult | null): string | undefined => {
  if (!auth?.ok) return undefined;
  return auth.method.kind === "passkey_session" ? (auth.token ?? undefined) : undefined;
};

/** Exact-path passkey routes whose own handlers perform authentication. */
const AUTH_ROUTES: readonly ExactRouteEntry[] = [
  { methods: ["POST"], path: "/api/auth/register/finish", run: (req) => handlePasskeyRegisterFinish(req) },
  { methods: ["POST"], path: "/api/auth/login/start", run: (req) => handlePasskeyLoginStart(req) },
  { methods: ["POST"], path: "/api/auth/login/finish", run: (req) => handlePasskeyLoginFinish(req) },
];

/** Admin API routes in wire order; every one authenticates before dispatch. */
const ADMIN_ROUTES: readonly AdminRouteEntry[] = [
  { methods: ["GET"], path: "/admin/passkey-users", superAdmin: true, run: () => handlePasskeyUsersList() },
  { methods: ["PATCH"], path: "/admin/passkey-users", superAdmin: true, run: (req) => handlePasskeyUsersUpdate(req) },
  { methods: ["POST"], path: "/admin/codex/auth", run: (req) => handleAdminCodexAuth(req) },
  { methods: ["GET", "PATCH"], path: "/admin/providers/codex/banked-resets", run: (req) => handleAdminCodexResetSettings(req) },
  { methods: ["GET"], path: "/admin/providers/codex/banked-resets/shadow-decisions", run: () => handleAdminCodexBankedResetShadowDecisions() },
  {
    methods: ["GET"],
    path: "/admin/providers/codex/cache-scope-experiment",
    superAdmin: true,
    run: () => handleAdminCodexCacheScopeExperimentTelemetryBaseline(),
  },
  { methods: ["POST"], path: "/admin/providers/codex/cache-scope-experiment", superAdmin: true, run: (req) => handleAdminCodexCacheScopeExperiment(req) },
  { methods: ["GET"], path: "/admin/codex/models", run: () => handleAdminCodexModelsGet() },
  { methods: ["POST"], path: "/admin/codex/models", run: (req) => handleAdminCodexModelsSet(req) },
  { methods: ["GET"], path: "/admin/models/whitelist", run: () => handleAdminCodexModelsWhitelistGet() },
  { methods: ["GET"], path: "/admin/models/catalog", run: () => handleAdminModelsCatalogGet() },
  { methods: ["POST"], path: "/admin/models/refresh", run: () => handleAdminModelsRefresh() },
  { methods: ["POST"], path: "/admin/models/whitelist", run: (req) => handleAdminCodexModelsWhitelistSet(req) },
  { methods: ["POST"], path: "/admin/codex/prompts/purge", run: () => handleAdminCodexPromptsPurge() },
  { methods: ["POST"], path: "/admin/kv-migration/import", superAdmin: true, run: (req) => handleAdminKvMigrationImport(req) },
  { methods: ["GET"], path: "/admin/kv-migration/validate", superAdmin: true, run: () => handleAdminKvMigrationValidate() },
  { methods: ["GET"], path: "/admin/sentinel/replay-captures", superAdmin: true, run: (req) => handleAdminSentinelReplayCaptures(req) },
  { methods: ["GET"], path: "/admin/sentinel/incidents", superAdmin: true, run: (req) => handleAdminSentinelIncidents(req) },
  { methods: ["GET"], path: "/admin/codex/supervisor/sessions", superAdmin: true, run: () => handleAdminCodexSupervisorSessions() },
  { methods: ["GET"], path: "/admin/codex/supervisor/output", superAdmin: true, run: (req) => handleAdminCodexSupervisorOutput(req) },
  { methods: ["POST"], path: "/admin/codex/supervisor/brief", superAdmin: true, run: (req) => handleAdminCodexSupervisorBrief(req) },
  { methods: ["GET"], path: "/admin/errors", run: (req) => handleAdminErrors(req) },
  { methods: ["GET", "POST"], path: "/admin/defaults", run: (req) => handleAdminDefaults(req) },
  { methods: ["GET", "POST", "DELETE"], path: "/admin/debug/routing", run: (req) => handleAdminDebugRouting(req) },
  { methods: ["GET"], path: "/admin/providers", run: () => handleHealthProviders({ includeQuota: true }) },
  { methods: ["GET"], path: "/admin/providers/selection", run: () => handleAdminProviderSelectionGet() },
  { methods: ["POST"], path: "/admin/providers/selection", run: (req) => handleAdminProviderSelectionSet(req) },
  { methods: ["GET"], path: "/admin/providers/capacity", run: (req) => handleProviderCapacity(req) },
  { methods: ["GET"], path: "/admin/providers/quota-projection", run: (req) => handleAdminProvidersQuotaProjection(req) },
  { methods: ["POST"], path: "/admin/providers/quota-projection/backfill", run: (req) => handleAdminProvidersQuotaProjectionBackfill(req) },
  { methods: ["GET"], path: "/admin/prompt-cache-analytics", run: (req) => handleAdminPromptCacheAnalytics(req) },
  { methods: ["POST"], path: "/admin/api-keys", run: (req) => handleAdminApiKeysCreate(req) },
  { methods: ["GET"], path: "/admin/api-keys", run: (req) => handleAdminApiKeysList(req) },
  { methods: ["PATCH"], path: "/admin/api-keys", run: (req) => handleAdminApiKeysUpdate(req) },
  { methods: ["POST"], path: "/admin/api-keys/revoke", run: (req) => handleAdminApiKeysRevoke(req) },
  { methods: ["POST"], path: "/admin/api-keys/unrevoke", run: (req) => handleAdminApiKeysUnrevoke(req) },
  { methods: ["DELETE"], path: "/admin/api-keys", run: (req) => handleAdminApiKeysDelete(req) },
  { methods: ["GET"], path: "/admin/kernel-usage", run: (req) => handleAdminKernelUsageGet(req) },
  { methods: ["GET"], path: "/admin/kernel-policy-queue", run: () => handleAdminKernelPolicyQueueList() },
  { methods: ["POST"], path: "/admin/kernel-usage", run: (req) => handleAdminKernelUsageSet(req) },
  { methods: ["DELETE"], path: "/admin/kernel-usage", run: (req) => handleAdminKernelUsageDelete(req) },
  { methods: ["GET"], path: "/admin/kernel-pubkeys", run: () => handleAdminKernelPubKeysList() },
  { methods: ["POST"], path: "/admin/kernel-pubkeys", run: (req) => handleAdminKernelPubKeysCreate(req) },
  { methods: ["DELETE"], path: "/admin/kernel-pubkeys", run: (req) => handleAdminKernelPubKeysDelete(req) },
];

/** Serves the root document and static assets; null when nothing matches. */
const handleStaticRoute = async (req: Request, path: string): Promise<Response | null> => {
  if ((req.method === "GET" || req.method === "HEAD") && (path === "/" || path === "/index.html")) {
    const rootResponse = await handleRoot(req);
    return req.method === "HEAD" ? withoutBody(rootResponse) : rootResponse;
  }
  if (req.method === "GET" || req.method === "HEAD") {
    const staticResponse = await handleStaticAsset(path);
    if (staticResponse) return req.method === "HEAD" ? withoutBody(staticResponse) : staticResponse;
  }
  return null;
};

/** Serves the liveness endpoints; null when no health path matches. */
const handleHealthRoute = async (req: Request, path: string): Promise<Response | null> => {
  if ((req.method === "GET" || req.method === "HEAD") && path === "/health") {
    const health = handleHealth();
    // Keep HEAD semantically equivalent to public GET liveness while correctly
    // omitting the body.
    return req.method === "HEAD" ? withoutBody(health) : health;
  }
  if (req.method !== "GET") return null;
  if (path === "/health/providers") {
    const authError = await requireAdminAuth(req);
    if (authError) return authError;
    return await handleHealthProviders();
  }
  if (path === "/health/upstream") {
    const authError = await requireAdminAuth(req);
    if (authError) return authError;
    return await handleHealthUpstream();
  }
  return null;
};

/** Serves the passkey and session routes; null when no auth route matches. */
const handleAuthRoute = async (req: Request, path: string): Promise<Response | null> => {
  if (req.method === "POST" && path === "/api/auth/register/start") {
    const auth = await authenticateAdmin(req);
    if (!auth.ok) return auth.response;
    return await handlePasskeyRegisterStart(req, {
      defaultIsAdmin: auth.is_super_admin,
      authenticatedPasskeyToken: auth.method.kind === "passkey_session" ? auth.token : undefined,
    });
  }
  const route = matchExactRoute(AUTH_ROUTES, req, path);
  if (route) return await route.run(req);
  if (req.method === "GET" && path === "/api/auth/session") {
    return await handlePasskeySession(req, { authenticatedPasskeyToken: passkeyTokenFrom(await optionalClientAuth(req)) });
  }
  if (req.method === "POST" && path === "/api/auth/logout") {
    return await handlePasskeyLogout(req, { authenticatedPasskeyToken: passkeyTokenFrom(await optionalClientAuth(req)) });
  }
  return null;
};

/** Serves the API-key paid-fallback routes, including their method-not-allowed reply. */
const handleApiKeyPaidFallbacksRoute = async (req: Request, path: string): Promise<Response | null> => {
  const match = /^\/admin\/api-keys\/([^/]+)\/paid-fallbacks$/.exec(path);
  if (!match) return null;
  if (req.method !== "GET") return openaiError(405, "Method not allowed", "method_not_allowed");
  const authError = await requireAdminAuth(req);
  if (authError) return authError;
  let keyId: string;
  try {
    keyId = decodeURIComponent(match[1]);
  } catch {
    return openaiError(400, "Invalid API key id", "invalid_request_error");
  }
  return await handleAdminApiKeysPaidFallbacks(req, keyId);
};

/** Serves the admin API surface; null when no admin route matches. */
const handleAdminRoute = async (req: Request, path: string): Promise<Response | null> => {
  const route = matchExactRoute(ADMIN_ROUTES, req, path);
  if (route) {
    const authError = route.superAdmin ? await requireSuperAdminAuth(req) : await requireAdminAuth(req);
    if (authError) return authError;
    return await route.run(req);
  }
  const recheckMatch = /^\/admin\/providers\/codex\/(\d+)\/recheck$/.exec(path);
  if (req.method === "POST" && recheckMatch) {
    const authError = await requireAdminAuth(req);
    if (authError) return authError;
    return await handleAdminCodexRecheck(Number(recheckMatch[1]));
  }
  return await handleApiKeyPaidFallbacksRoute(req, path);
};

/** Serves the UOS catalog and agent-message routes; null when none matches. */
const handleUosRoute = async (req: Request, path: string): Promise<Response | null> => {
  if (req.method === "GET" && path === "/uos/auth") return await handleV1Auth(req);
  if (req.method === "GET" && path === "/uos/models/catalog") return await handlePublicModelCatalog();
  if (req.method === "GET" && path === "/uos/models/capabilities") {
    const authResult = await authenticateClient(req);
    if (!authResult.ok) return authResult.response;
    return await handleModelCapabilities();
  }
  if (path === "/uos/agent-messages" && req.method === "GET") return await handleAgentMessagesList(req);
  if (path === "/uos/agent-messages" && req.method === "POST") return await handleAgentMessagesPost(req);
  if (path === "/uos/agent-messages") return openaiError(405, "Method not allowed", "method_not_allowed");
  return null;
};

/** True when the path belongs to the authenticated terminal inference surface. */
const isTerminalInferencePath = (path: string): boolean =>
  path.startsWith("/v1/") || path === "/uos/embeddings" || path === "/uos/embedding-jobs" || path.startsWith("/uos/embedding-jobs/");

/** Terminal-logs a response that never reached a provider; a null route skips logging. */
const withRejectionTerminalLog = (
  response: Response,
  route: string | null,
  input: Readonly<{
    requestId: string;
    startedAtMonotonicMs: number;
    delivery?: RequestDeliveryInfo;
    onSettled?: () => void;
    admissionWaitMs?: number | null;
  }>
): Promise<Response> => {
  if (!route) {
    input.onSettled?.();
    return Promise.resolve(response);
  }
  return withTerminalRequestLog(response, {
    route,
    startedAtMonotonicMs: input.startedAtMonotonicMs,
    requestId: input.requestId,
    deliveryCompleted: input.delivery?.completed,
    deliverySignal: input.delivery?.downstreamSignal,
    onSettled: input.onSettled,
    admissionWaitMs: input.admissionWaitMs,
  });
};

/** The API-key id that usage telemetry attributes this request to, if any. */
const apiKeyIdFrom = (authResult: AuthenticatedClientResult): string | null => (authResult.method.kind === "kv_api_key" ? authResult.method.key_id : null);

/** The API-key policy that applies to this request, if it authenticated with one. */
const apiKeyPolicyFrom = (authResult: AuthenticatedClientResult): ApiKeyPolicy | null =>
  authResult.method.kind === "kv_api_key" ? authResult.method.policy : null;

/** Resolves the GitHub repository that owns kernel quota for this request. */
const resolveKernelRepo = async (req: Request, authResult: AuthenticatedClientResult): Promise<Readonly<{ owner: string; repo: string }> | null> => {
  if (authResult.method.kind === "github_token") return { owner: authResult.method.owner, repo: authResult.method.repo };
  const attestation = await getKernelAttestationContext(req, authResult.token);
  if (!attestation) return null;
  return { owner: attestation.owner, repo: attestation.repo };
};

/** Signal handed to an inference handler: an active kernel reservation also aborts it. */
const downstreamSignalFor = (req: Request, delivery: RequestDeliveryInfo | undefined, reservation: KernelQuotaReservation | null): AbortSignal | undefined =>
  reservation ? AbortSignal.any([delivery?.downstreamSignal ?? req.signal, reservation.signal]) : delivery?.downstreamSignal;

/** Reserves API-key usage for a terminal route; a refusal is returned as a response. */
const reserveUsageAdmission = async (
  req: Request,
  policy: ApiKeyPolicy,
  requestId: string,
  route: string,
  startedAtMonotonicMs: number,
  delivery: RequestDeliveryInfo | undefined,
  onSettled?: () => void,
  admissionWaitMs?: number | null
): Promise<Readonly<{ reservation: ApiKeyUsageReservation } | { rejection: Response }>> => {
  const admission = await reserveApiKeyUsageV3(policy, requestId, route, { deferWhenFull: true });
  if (admission.ok) return { reservation: admission.reservation };
  const response = withCorsHeaders(withRequestId(admission.response, requestId), req);
  return { rejection: await withRejectionTerminalLog(response, route, { requestId, startedAtMonotonicMs, delivery, onSettled, admissionWaitMs }) };
};

/** Reserves kernel usage for a repository-scoped route; a refusal is returned as a response. */
const reserveKernelAdmission = async (
  input: Readonly<{
    req: Request;
    repo: Readonly<{ owner: string; repo: string }>;
    route: string;
    requestId: string;
    startedAtMonotonicMs: number;
    delivery?: RequestDeliveryInfo;
    usageReservation: ApiKeyUsageReservation | null;
    onSettled?: () => void;
    admissionWaitMs?: number | null;
  }>
): Promise<Readonly<{ reservation: KernelQuotaReservation } | { rejection: Response }>> => {
  const admission = await reserveEffectiveKernelUsageLimit(input.repo.owner, input.repo.repo, input.requestId, input.route);
  if (admission.ok) return { reservation: admission.reservation };
  try {
    await input.usageReservation?.release("kernel_quota_rejected");
  } catch (error) {
    warnQuotaAccountingFailure({ route: input.route, requestId: input.requestId }, error);
  }
  const response = withCorsHeaders(withRequestId(admission.response, input.requestId), input.req);
  const rejection = await withRejectionTerminalLog(response, input.route, {
    requestId: input.requestId,
    startedAtMonotonicMs: input.startedAtMonotonicMs,
    delivery: input.delivery,
    onSettled: input.onSettled,
    admissionWaitMs: input.admissionWaitMs,
  });
  return { rejection };
};

/**
 * Persists the Sentinel replay capture for a thrown inference exception.
 * Snapshotting happens before the try block, exactly as the inline capture did:
 * a snapshot failure propagates instead of being swallowed as a capture failure.
 */
const persistInferenceExceptionReplay = async (sentinelReplayInput: AcceptedSentinelReplayInput, runError: unknown): Promise<void> => {
  const observation: SentinelFailureObservation = {
    status: 500,
    stream: null,
    completed: false,
    terminal_type: "error",
    failure_kind: runError instanceof Error ? runError.name : "unknown_exception",
    synthetic_terminal_type: null,
    provider_route: "gateway",
  };
  // Snapshot body and upstream recorder together before persistence:
  // the recorder is sealed and disposed here, and the same immutable
  // trace feeds HMAC and encryption.
  const replaySnapshot = snapshotSentinelReplayInput(sentinelReplayInput);
  try {
    await persistSentinelReplayFromEnvironment(replaySnapshot, observation);
  } catch {
    // Replay persistence is best effort and cannot replace the original
    // gateway exception or expose its request body in logs.
  } finally {
    zeroSentinelReplayInput(sentinelReplayInput);
    zeroSentinelReplayInput(replaySnapshot);
  }
};

/**
 * Authenticates, admits, and dispatches one terminal inference request.  Every
 * response that leaves here already carries CORS headers, the request id, quota
 * decoration, and the terminal accounting handoff.
 */
const handleTerminalRoute = async (
  req: Request,
  path: string,
  delivery: RequestDeliveryInfo | undefined,
  requestId: string,
  requestStartedAtMs: number,
  requestStartedAtMonotonicMs: number
): Promise<Response> => {
  const withCors = (response: Response): Response => withCorsHeaders(response, req);
  const terminalRoute = terminalRouteForRequest(req.method, path);
  const authResult = await authenticateClient(req);
  if (!authResult.ok) {
    const response = withCors(withRequestId(authResult.response, requestId));
    return await withRejectionTerminalLog(response, terminalRoute, { requestId, startedAtMonotonicMs: requestStartedAtMonotonicMs, delivery });
  }
  // Authenticated state, not a client header or session id: a caller cannot
  // rename itself to escape the waiting turn its principal was served in.
  const idempotencyPrincipal = await resolveIdempotencyPrincipal(authResult);
  const callerSignal = delivery?.downstreamSignal ?? req.signal;
  let processPermit: Extract<InferenceAdmissionResult, { ok: true }> | null = null;
  let admissionWaitMs: number | null = null;
  /** Idempotent: the guard hands out one permit, and this clears it once. */
  const releaseProcessPermit = (): void => {
    const permit = processPermit;
    processPermit = null;
    permit?.release();
  };
  /** Releases the permit when a step after acquisition throws before dispatch. */
  const releaseOnThrow = async <T>(step: () => T | Promise<T>): Promise<T> => {
    try {
      return await step();
    } catch (error) {
      releaseProcessPermit();
      throw error;
    }
  };
  const admissionRoute = terminalRoute !== null && ADMISSION_ROUTES.has(terminalRoute) ? terminalRoute : null;
  /**
   * Acquires the process-resource permit for an admitted route, or returns the
   * refusal response. A granted permit stays owned until the response settles.
   */
  const acquireProcessPermit = async (): Promise<Response | null> => {
    if (admissionRoute === null) return null;
    // Acquired after authentication and before any quota reservation, so a
    // queued request never holds an expiring API-key or kernel lease. The
    // caller's own signal and the guard's five-second bound conclude the wait.
    const admitted = await admissionController().acquire({ signal: callerSignal, principal: idempotencyPrincipal });
    if (admitted.ok) {
      processPermit = admitted;
      admissionWaitMs = admitted.waitedMs;
      return null;
    }
    // A queued caller that aborted never dispatches; a full queue or an
    // expired wait is this gateway's own bounded overload decision.
    const refusal =
      admitted.kind === "caller_aborted"
        ? openaiError(499, "Request was cancelled.", "request_cancelled", { type: "server_error", param: null })
        : localOverloadResponse(admitted.cause);
    const response = withCors(withRequestId(refusal, requestId));
    return await withRejectionTerminalLog(response, admissionRoute, { requestId, startedAtMonotonicMs: requestStartedAtMonotonicMs, delivery });
  };
  const admissionRefusal = await acquireProcessPermit();
  if (admissionRefusal) return admissionRefusal;
  let usagePolicy = apiKeyPolicyFrom(authResult);
  let usageReservation: ApiKeyUsageReservation | null = null;
  // The request candidate is created as soon as the caller is authenticated,
  // before quota admission, so every authenticated return path can publish an
  // explicit capture status. An unauthenticated rejection creates nothing, so
  // its zero-KV behavior is preserved. Candidate creation is passive: it only
  // registers the one accepted-body observer, it never touches KV. A capture
  // that throws still returns this request's process permit before dispatch.
  const sentinelReplayCandidate = await releaseOnThrow(() => (terminalRoute ? captureAcceptedSentinelReplayInput(req, requestId) : null));
  let sentinelReplayOmission: SentinelReplayCaptureOmissionReason | null = null;
  /**
   * Publishes the explicit status for an authenticated request rejected before
   * capture setup. Best effort: the rejection response is already final.
   */
  const recordPreCaptureRejection = async (): Promise<void> => {
    discardSentinelReplayCaptureCandidate(sentinelReplayCandidate);
    sentinelReplayOmission ??= "rejected_before_capture";
    const omissionTask = recordSentinelReplayOmissionFromEnvironment(requestId, sentinelReplayOmission, Date.now());
    // Deferred where the runtime supports it so a diagnostic status write can
    // never extend the client-visible rejection latency.
    if (!scheduleSentinelBackgroundTask(omissionTask, undefined)) await omissionTask;
  };
  /** Reserves API-key usage for a policied terminal route, or returns the refusal. */
  const reserveUsageForRequest = async (): Promise<Response | null> => {
    if (usagePolicy === null || terminalRoute === null) return null;
    // Copied out of the mutable bindings so the narrowed policy and route
    // survive into the release-on-throw closure.
    const policy = usagePolicy;
    const route = terminalRoute;
    const admission = await releaseOnThrow(() =>
      reserveUsageAdmission(req, policy, requestId, route, requestStartedAtMonotonicMs, delivery, releaseProcessPermit, admissionWaitMs)
    );
    if ("rejection" in admission) {
      await recordPreCaptureRejection();
      return admission.rejection;
    }
    usageReservation = admission.reservation;
    // Admission re-reads the strict hash policy, so downstream quota headers
    // and paid fallback use the policy that actually reserved this request.
    usagePolicy = admission.reservation.policy;
    return null;
  };
  const usageRefusal = await reserveUsageForRequest();
  if (usageRefusal) return usageRefusal;
  /**
   * Reads the reservation through a call. `reserveUsageForRequest` assigns it,
   * and reading the binding directly would let control-flow analysis keep
   * treating it as its initial `null` here.
   */
  const currentUsageReservation = (): ApiKeyUsageReservation | null => usageReservation;
  /** Same closure-reader reason as `currentUsageReservation`, for the queue wait. */
  const currentAdmissionWaitMs = (): number | null => admissionWaitMs;
  const kernelRepo = await releaseOnThrow(() => resolveKernelRepo(req, authResult));
  const kernelOrg = kernelRepo ? { owner: kernelRepo.owner } : null;
  let kernelReservation: KernelQuotaReservation | null = null;
  const kernelQuotaRoute = kernelQuotaRouteForRequest(req.method, path);
  /** Reserves repository kernel quota for a scoped route, or returns the refusal. */
  const reserveKernelForRequest = async (): Promise<Response | null> => {
    if (kernelRepo === null || kernelQuotaRoute === null) return null;
    const admission = await releaseOnThrow(() =>
      reserveKernelAdmission({
        req,
        repo: kernelRepo,
        route: kernelQuotaRoute,
        requestId,
        startedAtMonotonicMs: requestStartedAtMonotonicMs,
        delivery,
        usageReservation,
        onSettled: releaseProcessPermit,
        admissionWaitMs,
      })
    );
    if ("rejection" in admission) {
      await recordPreCaptureRejection();
      return admission.rejection;
    }
    kernelReservation = admission.reservation;
    return null;
  };
  const kernelRefusal = await reserveKernelForRequest();
  if (kernelRefusal) return kernelRefusal;
  /** Same closure-reader reason as `currentUsageReservation`. */
  const currentKernelReservation = (): KernelQuotaReservation | null => kernelReservation;
  // One request-owned passive upstream recorder for accepted terminal
  // inference requests. It is created only after quota admission, is never
  // global, and is sealed/disposed at the application-terminal handoff.
  const sentinelUpstreamRecorder = terminalRoute ? createSentinelUpstreamRecorder() : null;
  const usageContext = {
    keyId: apiKeyIdFrom(authResult),
    kernelRepo,
    kernelOrg,
    paidFallbackEnabled: usagePolicy?.paid_fallback_enabled === true,
    idempotencyPrincipal,
    requestId,
    startedAtMs: requestStartedAtMs,
    startedAtMonotonicMs: requestStartedAtMonotonicMs,
    downstreamSignal: downstreamSignalFor(req, delivery, currentKernelReservation()),
    beforeProviderDispatch: currentUsageReservation()?.beforeProviderDispatch,
    ...(sentinelUpstreamRecorder ? { sentinelUpstreamRecorder } : {}),
  };
  if (terminalRoute) {
    console.info(
      "[ai.ubq.fi] request_accepted",
      JSON.stringify({
        request_id: requestId,
        route: terminalRoute,
        queue_wait_ms: currentAdmissionWaitMs() ?? 0,
        git_sha: runtimeGitSha(),
        deno_revision: runtimeDeploymentId(),
      })
    );
  }
  const takeSentinelReplayInput = (): AcceptedSentinelReplayInput | null => {
    const materialized = materializeSentinelReplayInput(sentinelReplayCandidate);
    // Read the omission after materializing: a body-less candidate with no
    // recorded reason is classified as `body_unavailable`, never silently
    // dropped. A non-POST terminal route has no body to carry at all.
    const omission = sentinelReplayCandidate?.body_omitted_reason ?? (sentinelReplayCandidate === null && terminalRoute !== null ? "non_post" : null);
    discardSentinelReplayCaptureCandidate(sentinelReplayCandidate);
    if (!materialized) {
      sentinelUpstreamRecorder?.dispose();
      if (omission !== null) sentinelReplayOmission ??= omission;
      return null;
    }
    return sentinelUpstreamRecorder ? { ...materialized, upstreamRecorder: sentinelUpstreamRecorder } : materialized;
  };
  const settleKernelQuota = async (outcome: "completed" | "incomplete", reason = "request_incomplete"): Promise<void> => {
    if (!kernelReservation) return;
    if (outcome === "completed") await kernelReservation.commit();
    else await kernelReservation.release(reason);
  };
  const bestEffortSettleKernelQuota = async (outcome: "completed" | "incomplete", reason = "request_incomplete"): Promise<void> => {
    try {
      await settleKernelQuota(outcome, reason);
    } catch (error) {
      warnQuotaAccountingFailure({ route: terminalRoute ?? "inference", requestId }, error);
    }
  };
  const finishTerminalResponse = async (response: Response, route: string, includeQuota = false, trackKernelTerminal = false): Promise<Response> => {
    const telemetry = getResponseTelemetry(response);
    const correlated = withProviderRequestId(response, telemetry?.providerRequestId ?? null);
    const decorated = includeQuota ? decorateInferenceQuota(correlated, usagePolicy, telemetry) : correlated;
    const sentinelReplayInput = takeSentinelReplayInput();
    try {
      return await withTerminalRequestLog(withCors(withRequestId(decorated, requestId)), {
        route,
        telemetryResponse: response,
        startedAtMonotonicMs: requestStartedAtMonotonicMs,
        requestId,
        onTerminal: trackKernelTerminal ? settleKernelQuota : undefined,
        onSettled: releaseProcessPermit,
        deliveryCompleted: delivery?.completed,
        deliverySignal: delivery?.downstreamSignal,
        sentinelReplayInput,
        admissionWaitMs,
        sentinelReplayOmission,
      });
    } catch (error) {
      zeroSentinelReplayInput(sentinelReplayInput);
      disposeSentinelUpstreamRecorder(sentinelReplayInput);
      releaseProcessPermit();
      await bestEffortSettleKernelQuota("incomplete", "terminal_wrapper_error");
      throw error;
    }
  };
  const executeInference = async (run: () => Promise<Response>): Promise<Response> => {
    let response: Response | null = null;
    let runError: unknown = null;
    try {
      response = await run();
    } catch (error) {
      runError = error;
    }
    try {
      // A provider dispatch settles this as committed; every validation,
      // cache, idempotency, queue, and synthetic-routing path is released.
      await usageReservation?.release();
    } catch (error) {
      await bestEffortSettleKernelQuota("incomplete", "api_key_quota_accounting_error");
      if (runError) {
        warnQuotaAccountingFailure({ route: terminalRoute ?? "inference", requestId }, runError);
      }
      const quotaError = error instanceof ApiKeyQuotaDispatchError ? error : new ApiKeyQuotaDispatchError("API key quota reservation is unavailable");
      return openaiError(quotaError.status, quotaError.message, quotaError.code, {
        type: quotaError.errorType,
        headers: quotaError.headers,
      });
    }
    if (runError instanceof ApiKeyQuotaDispatchError) {
      await bestEffortSettleKernelQuota("incomplete", "api_key_quota_dispatch_error");
      return openaiError(runError.status, runError.message, runError.code, {
        type: runError.errorType,
        headers: runError.headers,
      });
    }
    if (runError) {
      await bestEffortSettleKernelQuota("incomplete", "inference_exception");
      const sentinelReplayInput = takeSentinelReplayInput();
      if (sentinelReplayInput) {
        await persistInferenceExceptionReplay(sentinelReplayInput, runError);
      } else if (sentinelReplayOmission !== null) {
        // A thrown inference failure with no captured body still publishes its
        // explicit omission status instead of an empty replay history.
        await recordSentinelReplayOmissionFromEnvironment(requestId, sentinelReplayOmission, Date.now());
      }
      // `only-throw-error` requires an Error: an Error run failure is rethrown
      // unchanged, and any other value is preserved as the cause.
      throw runError instanceof Error ? runError : new Error("Inference handler threw a non-Error value", { cause: runError });
    }
    if (!response) {
      await bestEffortSettleKernelQuota("incomplete", "missing_inference_response");
      throw new Error("Inference handler completed without a response");
    }
    return response;
  };
  const runModelsRoute = async (): Promise<Response> => withCors(await handleModels(req));
  const runEmbeddingsRoute = async (): Promise<Response> => {
    const response = await executeInference(() => handleUosEmbeddings(req, usageContext));
    if (response.ok && response.headers.get("x-uos-idempotency-replayed") !== "true") {
      await bestEffortSettleKernelQuota("completed");
    } else {
      await bestEffortSettleKernelQuota(
        "incomplete",
        response.headers.get("x-uos-idempotency-replayed") === "true" ? "idempotency_replay" : "embedding_failed"
      );
    }
    return await finishTerminalResponse(response, "embeddings");
  };
  const runEmbeddingJobCreateRoute = async (): Promise<Response> => {
    const response = await executeInference(() => handleEmbeddingsJobCreate(req, authResult.token, usageContext));
    if (response.ok) {
      await bestEffortSettleKernelQuota("completed");
    } else {
      await bestEffortSettleKernelQuota("incomplete", "embedding_job_create_failed");
    }
    return await finishTerminalResponse(response, "embeddings.jobs.create");
  };
  const runEmbeddingJobGetRoute = async (): Promise<Response> => {
    const jobId = path.slice("/uos/embedding-jobs/".length).trim();
    if (!jobId) {
      await bestEffortSettleKernelQuota("incomplete", "missing_embedding_job_id");
      return await finishTerminalResponse(openaiError(404, "Not found", "not_found"), "embeddings.jobs.get");
    }
    const response = await executeInference(() => handleEmbeddingsJobGet(req, authResult.token, jobId, usageContext));
    await bestEffortSettleKernelQuota("incomplete", "embedding_job_read_not_counted");
    return await finishTerminalResponse(response, "embeddings.jobs.get");
  };
  const runImagesRoute = async (): Promise<Response> => {
    const kind = path === "/v1/images/edits" ? "edits" : "generations";
    const response = await executeInference(() => handleImages(req, kind, usageContext));
    return await finishTerminalResponse(response, `images.${kind}`, true, true);
  };
  const runChatCompletionsRoute = async (): Promise<Response> => {
    const response = await executeInference(() => handleChatCompletions(req, usageContext));
    return await finishTerminalResponse(response, "chat.completions", true, true);
  };
  const runResponsesRoute = async (): Promise<Response> => {
    // Header-marked Codex compaction is answered locally by Jev; the body is
    // read only for a recognized request. A failure is a non-success response,
    // never a fallback to the main model, so Codex keeps its existing history.
    // The normal terminal wrapper keeps cancellation, settlement and terminal
    // logging ownership exactly as the ordinary route does.
    if (isJevCompactionRequest(req)) {
      const response = await executeInference(() => handleJevResponsesCompaction(req, { signal: callerSignal }));
      return await finishTerminalResponse(response, "responses", true, true);
    }
    const response = await executeInference(() => handleResponses(req, usageContext));
    return await finishTerminalResponse(response, "responses", true, true);
  };
  // Terminal routes in wire order. The conditions are pure, so the first match
  // owns the response exactly as the original if/else chain did.
  const dispatchTerminalRoute = async (): Promise<Response> => {
    const terminalRoutes: readonly (readonly [boolean, () => Promise<Response>])[] = [
      [req.method === "GET" && path === "/v1/models", runModelsRoute],
      [req.method === "POST" && path === "/uos/embeddings", runEmbeddingsRoute],
      [req.method === "POST" && path === "/uos/embedding-jobs", runEmbeddingJobCreateRoute],
      [req.method === "GET" && path.startsWith("/uos/embedding-jobs/"), runEmbeddingJobGetRoute],
      [req.method === "POST" && (path === "/v1/images/generations" || path === "/v1/images/edits"), runImagesRoute],
      [req.method === "POST" && path === "/v1/chat/completions", runChatCompletionsRoute],
      [req.method === "POST" && path === "/v1/responses", runResponsesRoute],
    ];
    for (const [matches, run] of terminalRoutes) {
      if (matches) return await run();
    }
    const response = openaiError(404, "Not found", "not_found");
    return withCors(req.method === "HEAD" ? withoutBody(response) : response);
  };
  /**
   * Refuses a request whose caller left while admission waited or quota/setup
   * ran. The permit returns through the terminal wrapper, an acquired API-key
   * reservation is released, and an acquired kernel reservation is settled as
   * released so it stops renewing; a request that never dispatched is not
   * charged.
   */
  const refuseAbortedBeforeDispatch = async (): Promise<Response | null> => {
    if (processPermit === null || !callerSignal.aborted) return null;
    // No provider dispatch happened, so settle exactly the pre-dispatch
    // resources `executeInference` settles: releasing the API-key reservation
    // is idempotent (a deferred reservation releases nothing), and releasing an
    // acquired kernel reservation is the existing no-dispatch settlement that
    // stops its lease renewal. A settlement fault cannot replace the 499.
    const abortReason = "caller_aborted_before_dispatch";
    try {
      await usageReservation?.release(abortReason);
    } catch (error) {
      warnQuotaAccountingFailure({ route: terminalRoute ?? "inference", requestId }, error);
    }
    await bestEffortSettleKernelQuota("incomplete", abortReason);
    const refusal = openaiError(499, "Request was cancelled.", "request_cancelled", { type: "server_error", param: null });
    const response = withCors(withRequestId(refusal, requestId));
    return await withRejectionTerminalLog(response, terminalRoute, {
      requestId,
      startedAtMonotonicMs: requestStartedAtMonotonicMs,
      delivery,
      onSettled: releaseProcessPermit,
      admissionWaitMs,
    });
  };
  const abortedRefusal = await refuseAbortedBeforeDispatch();
  if (abortedRefusal) return abortedRefusal;
  return await releaseOnThrow(dispatchTerminalRoute);
};

export default async function handler(req: Request, delivery?: RequestDeliveryInfo): Promise<Response> {
  const requestStartedAtMs = Date.now();
  const requestStartedAtMonotonicMs = performance.now();
  const requestId = crypto.randomUUID();
  const withCors = (response: Response): Response => withCorsHeaders(response, req);
  if (req.method === "OPTIONS") {
    return withCors(new Response(null, { status: 204, headers: corsHeaders(req) }));
  }

  const url = new URL(req.url);
  const path = normalizePath(url.pathname);

  const staticResponse = await handleStaticRoute(req, path);
  if (staticResponse) return withCors(staticResponse);
  const healthResponse = await handleHealthRoute(req, path);
  if (healthResponse) return withCors(healthResponse);
  const authRouteResponse = await handleAuthRoute(req, path);
  if (authRouteResponse) return withCors(authRouteResponse);
  const adminResponse = await handleAdminRoute(req, path);
  if (adminResponse) return withCors(adminResponse);
  const uosResponse = await handleUosRoute(req, path);
  if (uosResponse) return withCors(uosResponse);

  if (!isTerminalInferencePath(path)) {
    const response = notFound();
    return withCors(req.method === "HEAD" ? withoutBody(response) : response);
  }

  return await handleTerminalRoute(req, path, delivery, requestId, requestStartedAtMs, requestStartedAtMonotonicMs);
}
