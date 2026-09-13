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
  handleAdminPromptCacheAnalytics,
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
import { handleHealth, handleHealthProviders, handleHealthUpstream } from "./health.ts";
import { corsHeaders, notFound, openaiError, withCors as withCorsHeaders, withoutBody } from "./http.ts";
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
import { recordPromptCacheAnalytics } from "./prompt_cache_analytics.ts";
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
  resolveSentinelClientFailureObservation,
  type SentinelClientBodyObservation,
  type SentinelFailureObservation,
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

const withProviderRequestId = (response: Response, providerRequestId: string | null): Response => {
  const requestId = providerRequestIdHeaderValue(providerRequestId);
  const headers = new Headers(response.headers);
  // Never reflect provider-native correlation headers. Expose one bounded UOS
  // header whose value has already passed the gateway sanitizer.
  headers.delete("x-request-id");
  headers.delete("x-api-request-id");
  headers.delete("x-oneapi-request-id");
  headers.delete("x-cerebras-request-id");
  headers.delete("x-uos-provider-request-id");
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
    affinity_outcome: telemetry?.affinityOutcome ?? "none",
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
  const cacheAnalyticsWrite = (input.recordCacheAnalytics ?? recordPromptCacheAnalytics)({
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
  });
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
    deliveryCompleted?: Promise<void>;
    deliverySignal?: AbortSignal;
    /** Test seam for proving aggregate cache analytics remains best effort. */
    recordCacheAnalytics?: typeof recordPromptCacheAnalytics;
    /** Test seam for proving terminal telemetry remains best effort. */
    recordTelemetry?: typeof recordPromptCacheTelemetry;
    /** Test seam for proving failed replay persistence and successful-request exclusion. */
    sentinelReplayInput?: AcceptedSentinelReplayInput | null;
    persistSentinelReplay?: typeof persistSentinelReplayFromEnvironment;
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
      if (!backgroundReplayInput) return;
      const telemetry = getResponseTelemetry(input.telemetryResponse ?? response);
      const observation: SentinelFailureObservation = {
        status: response.status,
        stream: streamReadFailure ? (telemetry?.stream ?? true) : (telemetry?.stream ?? null),
        completed: streamReadFailure ? false : (telemetry?.completed ?? false),
        terminal_type: streamReadFailure ? (telemetry?.streamTerminalType ?? "error") : (telemetry?.streamTerminalType ?? null),
        failure_kind: streamReadFailure ? (telemetry?.failureKind ?? "gateway_stream_read_error") : (telemetry?.failureKind ?? null),
        synthetic_terminal_type: telemetry?.syntheticTerminalType ?? null,
        provider_route: telemetry?.provider ?? response.headers.get("x-uos-upstream") ?? "gateway",
      };
      const startReplayPersistence = (clientObservation: ReturnType<typeof resolveSentinelClientFailureObservation>): Promise<void> => {
        if (!shouldPersistSentinelReplay(observation, clientObservation)) return Promise.resolve();
        return persistSentinelReplayBestEffort(
          input.persistSentinelReplay ?? persistSentinelReplayFromEnvironment,
          backgroundReplayInput,
          observation,
          clientObservation
        );
      };
      const fallbackClientObservation = resolveSentinelClientFailureObservation(observation);
      // An HTTP failure is already sufficient to decide that the capture is
      // persistable. Start the best-effort write before waiting for the body
      // clone so a stalled inspection or delivery cannot delay its handoff.
      if (input.deliveryCompleted !== undefined && !isSse && shouldPersistSentinelReplay(observation, fallbackClientObservation)) {
        const replayWrite = startReplayPersistence(fallbackClientObservation);
        zeroSentinelReplayInput(originalReplayInput);
        await replayWrite;
        return;
      }
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
  { methods: ["POST"], path: "/admin/codex/prompts/purge", run: () => handleAdminCodexPromptsPurge() },
  { methods: ["POST"], path: "/admin/kv-migration/import", superAdmin: true, run: (req) => handleAdminKvMigrationImport(req) },
  { methods: ["GET"], path: "/admin/kv-migration/validate", superAdmin: true, run: () => handleAdminKvMigrationValidate() },
  { methods: ["GET"], path: "/admin/sentinel/replay-captures", superAdmin: true, run: (req) => handleAdminSentinelReplayCaptures(req) },
  { methods: ["GET"], path: "/admin/sentinel/incidents", superAdmin: true, run: (req) => handleAdminSentinelIncidents(req) },
  { methods: ["GET"], path: "/admin/errors", run: (req) => handleAdminErrors(req) },
  { methods: ["GET", "POST"], path: "/admin/defaults", run: (req) => handleAdminDefaults(req) },
  { methods: ["GET", "POST", "DELETE"], path: "/admin/debug/routing", run: (req) => handleAdminDebugRouting(req) },
  { methods: ["GET"], path: "/admin/providers", run: () => handleHealthProviders({ includeQuota: true }) },
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
  input: Readonly<{ requestId: string; startedAtMonotonicMs: number; delivery?: RequestDeliveryInfo }>
): Promise<Response> => {
  if (!route) return Promise.resolve(response);
  return withTerminalRequestLog(response, {
    route,
    startedAtMonotonicMs: input.startedAtMonotonicMs,
    requestId: input.requestId,
    deliveryCompleted: input.delivery?.completed,
    deliverySignal: input.delivery?.downstreamSignal,
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
  delivery: RequestDeliveryInfo | undefined
): Promise<Readonly<{ reservation: ApiKeyUsageReservation } | { rejection: Response }>> => {
  const admission = await reserveApiKeyUsageV3(policy, requestId, route, { deferWhenFull: true });
  if (admission.ok) return { reservation: admission.reservation };
  const response = withCorsHeaders(withRequestId(admission.response, requestId), req);
  return { rejection: await withRejectionTerminalLog(response, route, { requestId, startedAtMonotonicMs, delivery }) };
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
  let usagePolicy = apiKeyPolicyFrom(authResult);
  let usageReservation: ApiKeyUsageReservation | null = null;
  if (usagePolicy && terminalRoute) {
    const admission = await reserveUsageAdmission(req, usagePolicy, requestId, terminalRoute, requestStartedAtMonotonicMs, delivery);
    if ("rejection" in admission) return admission.rejection;
    usageReservation = admission.reservation;
    // Admission re-reads the strict hash policy, so downstream quota headers
    // and paid fallback use the policy that actually reserved this request.
    usagePolicy = admission.reservation.policy;
  }
  const idempotencyPrincipal = await resolveIdempotencyPrincipal(authResult);
  const kernelRepo = await resolveKernelRepo(req, authResult);
  const kernelOrg = kernelRepo ? { owner: kernelRepo.owner } : null;
  let kernelReservation: KernelQuotaReservation | null = null;
  const kernelQuotaRoute = kernelQuotaRouteForRequest(req.method, path);
  if (kernelRepo && kernelQuotaRoute) {
    const admission = await reserveKernelAdmission({
      req,
      repo: kernelRepo,
      route: kernelQuotaRoute,
      requestId,
      startedAtMonotonicMs: requestStartedAtMonotonicMs,
      delivery,
      usageReservation,
    });
    if ("rejection" in admission) return admission.rejection;
    kernelReservation = admission.reservation;
  }
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
    downstreamSignal: downstreamSignalFor(req, delivery, kernelReservation),
    beforeProviderDispatch: usageReservation?.beforeProviderDispatch,
    ...(sentinelUpstreamRecorder ? { sentinelUpstreamRecorder } : {}),
  };
  if (terminalRoute) {
    console.info(
      "[ai.ubq.fi] request_accepted",
      JSON.stringify({
        request_id: requestId,
        route: terminalRoute,
        git_sha: runtimeGitSha(),
        deno_revision: runtimeDeploymentId(),
      })
    );
  }
  const sentinelReplayCandidate = terminalRoute ? captureAcceptedSentinelReplayInput(req, requestId) : null;
  const takeSentinelReplayInput = (): AcceptedSentinelReplayInput | null => {
    const materialized = materializeSentinelReplayInput(sentinelReplayCandidate);
    discardSentinelReplayCaptureCandidate(sentinelReplayCandidate);
    if (!materialized) {
      sentinelUpstreamRecorder?.dispose();
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
        deliveryCompleted: delivery?.completed,
        deliverySignal: delivery?.downstreamSignal,
        sentinelReplayInput,
      });
    } catch (error) {
      zeroSentinelReplayInput(sentinelReplayInput);
      disposeSentinelUpstreamRecorder(sentinelReplayInput);
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
      if (sentinelReplayInput) await persistInferenceExceptionReplay(sentinelReplayInput, runError);
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
  return await dispatchTerminalRoute();
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
