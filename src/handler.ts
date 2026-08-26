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
import {
  authenticateAdmin,
  authenticateClient,
  getKernelAttestationContext,
  handleV1Auth,
  requireAdminAuth,
  requireSuperAdminAuth,
} from "./auth.ts";
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
  inspectSentinelBufferedResponseBody,
  materializeSentinelReplayInput,
  persistSentinelReplayFromEnvironment,
  resolveSentinelClientFailureObservation,
  type SentinelClientBodyObservation,
  type SentinelFailureObservation,
  shouldPersistSentinelReplay,
  zeroSentinelReplayInput,
} from "./sentinel_replay_capture.ts";
import { handleAdminSentinelReplayCaptures } from "./sentinel_replay_admin.ts";
import { handleAdminSentinelIncidentAck, handleAdminSentinelIncidentClaim } from "./sentinel_incident_admin.ts";
import { recordSentinelProviderDegradationFromEnvironment } from "./sentinel_incident_outbox.ts";

type AuthenticatedClientResult = Extract<
  Awaited<ReturnType<typeof authenticateClient>>,
  { ok: true }
>;

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

const scheduleSentinelBackgroundTask = (
  task: Promise<void>,
  registrar: SentinelBackgroundTaskRegistrar | undefined,
): boolean => {
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
  input: Readonly<{ status: number; completed: boolean; removedProviderTriggerClass: string | null }>,
): boolean =>
  input.status >= 200 && input.status < 400 && input.completed && input.removedProviderTriggerClass !== null;

type PrincipalAuthResult = Readonly<{
  token: string | null;
  method:
    | Readonly<{ kind: "kv_api_key"; key_id: string }>
    | Exclude<AuthenticatedClientResult["method"], { kind: "kv_api_key" }>;
}>;

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
  }
};

const normalizePath = (path: string): string => {
  if (path === "/") return path;
  return path.replace(/\/+$/, "");
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

const decorateInferenceQuota = (
  response: Response,
  policy: ApiKeyPolicy | null,
  telemetry: ResponseTelemetry | null,
): Response => {
  const usedPercent = telemetry?.quotaUsedPercent !== undefined
    ? telemetry.quotaUsedPercent
    : apiKeyQuotaUsedPercent(policy);
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
    resolveClientBodyObservation?: () =>
      | SentinelClientBodyObservation
      | null
      | Promise<SentinelClientBodyObservation | null>;
  }>,
): Promise<void> => {
  const telemetry = getResponseTelemetry(input.telemetryResponse ?? input.response);
  const accountCohortId = getResponseAccountCohortId(input.telemetryResponse ?? input.response);
  const latencyMs = Math.max(0, Math.round(performance.now() - input.startedAtMonotonicMs));
  const downstreamDrainMs = telemetry?.stream === true && telemetry.firstSemanticCommitmentMs !== null &&
      telemetry.streamTerminalMs !== null && input.downstreamDrainedAtMonotonicMs !== undefined
    ? Math.max(
      0,
      Math.round(input.downstreamDrainedAtMonotonicMs - input.startedAtMonotonicMs) - telemetry.streamTerminalMs,
    )
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
    stream: input.streamReadFailure ? telemetry?.stream ?? true : telemetry?.stream ?? null,
    stream_terminal_type: input.streamReadFailure
      ? telemetry?.streamTerminalType ?? "error"
      : telemetry?.streamTerminalType ?? null,
    failure_kind: input.streamReadFailure
      ? telemetry?.failureKind ?? "gateway_stream_read_error"
      : telemetry?.failureKind ?? null,
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
    completed: input.streamReadFailure ? false : telemetry?.completed ?? false,
    usageTelemetryStatus: terminal.usage_telemetry_status,
    cacheWriteTokensPresent: terminal.cache_write_input_tokens !== null,
  });
  const cacheAnalyticsWrite = (input.recordCacheAnalytics ?? recordPromptCacheAnalytics)({
    provider: terminal.provider,
    model: terminal.model,
    route: terminal.route,
    status: terminal.status,
    completed: input.streamReadFailure ? false : telemetry?.completed ?? false,
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
    completed: input.streamReadFailure ? false : telemetry?.completed ?? false,
    terminal_type: terminal.stream_terminal_type,
    failure_kind: terminal.failure_kind,
    synthetic_terminal_type: terminal.synthetic_terminal_type,
    provider_route: terminal.provider,
  };
  try {
    const clientBodyObservation = await input.resolveClientBodyObservation?.() ?? null;
    const clientObservation = resolveSentinelClientFailureObservation(replayObservation, clientBodyObservation);
    const replayWrite = input.sentinelReplayInput && !input.suppressSentinelReplay &&
        shouldPersistSentinelReplay(replayObservation, clientObservation)
      ? (input.persistSentinelReplay ?? persistSentinelReplayFromEnvironment)(
        input.sentinelReplayInput,
        replayObservation,
        clientObservation,
      )
      : Promise.resolve();
    const degradationWrite = shouldSignalSentinelProviderDegradation({
        status: terminal.status,
        completed: telemetry?.completed ?? false,
        removedProviderTriggerClass: terminal.removed_provider_trigger_class,
      })
      ? (input.recordSentinelDegradation ?? recordSentinelProviderDegradationFromEnvironment)(Date.now())
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

export const warnQuotaAccountingFailure = (
  input: Readonly<{ route: string; requestId: string }>,
  error: unknown,
): void => {
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
      }),
    );
  } catch {
    // Accounting and its warning are both best-effort after completion. Neither
    // may replace an upstream response that is already ready for the client.
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
  }>,
): Promise<Response> => {
  const contentType = response.headers.get("Content-Type")?.toLowerCase() ?? "";
  const isSse = contentType.includes("text/event-stream");
  const initialTelemetry = getResponseTelemetry(input.telemetryResponse ?? response);
  let clientBodyObservation: SentinelClientBodyObservation | null = null;
  let bufferedObservation: Promise<SentinelClientBodyObservation | null> | null = null;
  if (
    !isSse && response.body &&
    (response.status >= 400 || response.status === 202 || input.route.startsWith("embeddings.jobs.") ||
      (initialTelemetry?.completed === false &&
        (initialTelemetry.streamTerminalType !== null || initialTelemetry.failureKind !== null ||
          initialTelemetry.syntheticTerminalType !== null)))
  ) {
    bufferedObservation = inspectSentinelBufferedResponseBody(response);
  }
  let replayFinalization: Promise<void> | null = null;
  const persistReplayAtApplicationTerminal = (streamReadFailure = false): Promise<void> => {
    if (replayFinalization) return replayFinalization;
    const originalReplayInput = input.sentinelReplayInput;
    const backgroundReplayInput = originalReplayInput
      ? { ...originalReplayInput, body: new Uint8Array(originalReplayInput.body) }
      : null;
    // The background task owns the independent snapshot. Release the
    // request-owned bytes before any inspection or persistence await so a
    // stalled clone, crypto operation, or KV write cannot retain both copies.
    zeroSentinelReplayInput(originalReplayInput);
    replayFinalization = (async () => {
      if (!backgroundReplayInput) return;
      const telemetry = getResponseTelemetry(input.telemetryResponse ?? response);
      const observation: SentinelFailureObservation = {
        status: response.status,
        stream: streamReadFailure ? telemetry?.stream ?? true : telemetry?.stream ?? null,
        completed: streamReadFailure ? false : telemetry?.completed ?? false,
        terminal_type: streamReadFailure
          ? telemetry?.streamTerminalType ?? "error"
          : telemetry?.streamTerminalType ?? null,
        failure_kind: streamReadFailure
          ? telemetry?.failureKind ?? "gateway_stream_read_error"
          : telemetry?.failureKind ?? null,
        synthetic_terminal_type: telemetry?.syntheticTerminalType ?? null,
        provider_route: telemetry?.provider ?? response.headers.get("x-uos-upstream") ?? "gateway",
      };
      const startReplayPersistence = (
        clientObservation: ReturnType<typeof resolveSentinelClientFailureObservation>,
      ): Promise<void> => {
        if (!shouldPersistSentinelReplay(observation, clientObservation)) return Promise.resolve();
        try {
          return Promise.resolve(
            (input.persistSentinelReplay ?? persistSentinelReplayFromEnvironment)(
              backgroundReplayInput,
              observation,
              clientObservation,
            ),
          ).then(() => undefined);
        } catch {
          return Promise.resolve();
        }
      };
      const fallbackClientObservation = resolveSentinelClientFailureObservation(observation);
      // An HTTP failure is already sufficient to decide that the capture is
      // persistable. Start the best-effort write before waiting for the body
      // clone so a stalled inspection or delivery cannot delay its handoff.
      if (
        input.deliveryCompleted !== undefined && !isSse &&
        shouldPersistSentinelReplay(observation, fallbackClientObservation)
      ) {
        const replayWrite = startReplayPersistence(fallbackClientObservation);
        zeroSentinelReplayInput(originalReplayInput);
        await replayWrite;
        return;
      }
      const bodyObservation = clientBodyObservation ?? await bufferedObservation;
      const clientObservation = resolveSentinelClientFailureObservation(observation, bodyObservation);
      await startReplayPersistence(clientObservation);
    })().catch(() => {
      // Capture persistence is best effort and must not replace the response.
    }).finally(() => {
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
    suppressSentinelReplay = false,
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
      resolveClientBodyObservation: async () => clientBodyObservation ?? await bufferedObservation,
    }).catch(() => {
      // Terminal logging and its durable baseline counters are best effort;
      // neither may replace a response that is already ready for the client.
    });
    return terminalLog;
  };
  const deliveryOutcome = input.deliveryCompleted
    ? input.deliveryCompleted.then(
      () => input.deliverySignal?.aborted ? "interrupted" as const : "delivered" as const,
      () => "interrupted" as const,
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
      return pending.then(() => terminalSettled ? undefined : finalizeTerminal(outcome, reason));
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
      log(
        downstreamDrainedAtMonotonicMs,
        bodyResult === "drained" ? deliveryResult : "interrupted",
        bodyResult === "failed",
        bodyResult === "interrupted",
      )
    );
  }
  const body = new ReadableStream<Uint8Array>({
    async pull(controller) {
      let result: ReadableStreamReadResult<Uint8Array>;
      try {
        result = await reader.read();
      } catch (error) {
        clientBodyObservation = finishSseInspection("read_error");
        const downstreamAborted = downstreamCancelled || input.deliverySignal?.aborted === true;
        await finalizeFromTelemetry(downstreamAborted ? "downstream_cancelled" : "stream_read_error");
        if (downstreamAborted) zeroSentinelReplayInput(input.sentinelReplayInput);
        else await persistReplayAtApplicationTerminal(true);
        if (!deliveryOutcome) await log(undefined, "interrupted", !downstreamAborted, true);
        try {
          controller.error(error);
        } catch {
          // The downstream may have cancelled while the provider read failed.
        }
        settleBody?.(downstreamAborted ? "interrupted" : "failed");
        return;
      }
      if (result.done) {
        clientBodyObservation = finishSseInspection();
        const downstreamAborted = downstreamCancelled || input.deliverySignal?.aborted === true;
        if (downstreamAborted) {
          await finalizeFromTelemetry("downstream_cancelled");
          zeroSentinelReplayInput(input.sentinelReplayInput);
          try {
            controller.close();
          } catch {
            // The downstream cancellation may already have closed the wrapper.
          }
          if (!deliveryOutcome) await log(undefined, "interrupted", false, true);
          settleBody?.("interrupted");
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
          if (!deliveryOutcome) await log(undefined, "interrupted", false, true);
          settleBody?.("interrupted");
          return;
        }
        if (!deliveryOutcome) await log(downstreamDrainedAtMonotonicMs, "unobserved", false, true);
        settleBody?.("drained");
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
        zeroSentinelReplayInput(input.sentinelReplayInput);
        if (!deliveryOutcome) await log(undefined, "interrupted", false, true);
        settleBody?.("interrupted");
      }
    },
    cancel(reason) {
      // Cancellation must not await a concurrently pending provider pull;
      // that pull observes the cancellation and performs layered cleanup.
      downstreamCancelled = true;
      void reader.cancel(reason).catch(() => {});
      void finalizeFromTelemetry("downstream_cancelled");
      zeroSentinelReplayInput(input.sentinelReplayInput);
      if (!deliveryOutcome) void log(undefined, "interrupted", false, true);
      settleBody?.("interrupted");
    },
  });
  return Promise.resolve(
    new Response(body, {
      status: response.status,
      statusText: response.statusText,
      headers: response.headers,
    }),
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

  if ((req.method === "GET" || req.method === "HEAD") && (path === "/" || path === "/index.html")) {
    const rootResponse = await handleRoot(req);
    return withCors(req.method === "HEAD" ? withoutBody(rootResponse) : rootResponse);
  }

  if (req.method === "GET" || req.method === "HEAD") {
    const staticResponse = await handleStaticAsset(path);
    if (staticResponse) return withCors(req.method === "HEAD" ? withoutBody(staticResponse) : staticResponse);
  }

  if ((req.method === "GET" || req.method === "HEAD") && path === "/health") {
    const health = await handleHealth();
    // Keep HEAD semantically equivalent to public GET liveness while correctly
    // omitting the body.
    return withCors(req.method === "HEAD" ? withoutBody(health) : health);
  }

  if (req.method === "GET" && path === "/health/providers") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleHealthProviders());
  }

  if (req.method === "GET" && path === "/health/upstream") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleHealthUpstream());
  }

  if (req.method === "POST" && path === "/api/auth/register/start") {
    const auth = await authenticateAdmin(req);
    if (!auth.ok) return withCors(auth.response);
    return withCors(await handlePasskeyRegisterStart(req, { defaultIsAdmin: auth.is_super_admin }));
  }

  if (req.method === "POST" && path === "/api/auth/register/finish") {
    return withCors(await handlePasskeyRegisterFinish(req));
  }

  if (req.method === "POST" && path === "/api/auth/login/start") {
    return withCors(await handlePasskeyLoginStart(req));
  }

  if (req.method === "POST" && path === "/api/auth/login/finish") {
    return withCors(await handlePasskeyLoginFinish(req));
  }

  if (req.method === "GET" && path === "/api/auth/session") {
    return withCors(await handlePasskeySession(req));
  }

  if (req.method === "POST" && path === "/api/auth/logout") {
    return withCors(await handlePasskeyLogout(req));
  }

  if (req.method === "GET" && path === "/admin/passkey-users") {
    const authError = await requireSuperAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handlePasskeyUsersList());
  }

  if (req.method === "PATCH" && path === "/admin/passkey-users") {
    const authError = await requireSuperAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handlePasskeyUsersUpdate(req));
  }

  if (req.method === "POST" && path === "/admin/codex/auth") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminCodexAuth(req));
  }

  if (req.method === "GET" && path === "/admin/providers/codex/banked-resets/shadow-decisions") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminCodexBankedResetShadowDecisions());
  }

  if (req.method === "GET" && path === "/admin/providers/codex/cache-scope-experiment") {
    const authError = await requireSuperAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminCodexCacheScopeExperimentTelemetryBaseline());
  }

  if (req.method === "POST" && path === "/admin/providers/codex/cache-scope-experiment") {
    const authError = await requireSuperAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminCodexCacheScopeExperiment(req));
  }

  const codexRecheckMatch = path.match(/^\/admin\/providers\/codex\/(\d+)\/recheck$/);
  if (req.method === "POST" && codexRecheckMatch) {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminCodexRecheck(Number(codexRecheckMatch[1])));
  }

  if (req.method === "GET" && path === "/admin/codex/models") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminCodexModelsGet());
  }

  if (req.method === "POST" && path === "/admin/codex/models") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminCodexModelsSet(req));
  }

  if (req.method === "POST" && path === "/admin/codex/prompts/purge") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminCodexPromptsPurge());
  }

  if (req.method === "POST" && path === "/admin/kv-migration/import") {
    const authError = await requireSuperAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminKvMigrationImport(req));
  }

  if (req.method === "GET" && path === "/admin/kv-migration/validate") {
    const authError = await requireSuperAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminKvMigrationValidate());
  }

  if (req.method === "GET" && path === "/admin/sentinel/replay-captures") {
    const authError = await requireSuperAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminSentinelReplayCaptures(req));
  }

  if (req.method === "GET" && path === "/admin/errors") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminErrors(req));
  }

  if (req.method === "POST" && path === "/admin/sentinel/incidents/ack") {
    const authError = await requireSuperAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminSentinelIncidentAck(req));
  }

  if (req.method === "POST" && path === "/admin/sentinel/incidents/claim") {
    const authError = await requireSuperAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminSentinelIncidentClaim(req));
  }

  if ((req.method === "GET" || req.method === "POST") && path === "/admin/defaults") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminDefaults(req));
  }

  if ((req.method === "GET" || req.method === "POST" || req.method === "DELETE") && path === "/admin/debug/routing") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminDebugRouting(req));
  }

  if (req.method === "GET" && path === "/admin/providers") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleHealthProviders({ includeQuota: true }));
  }

  if (req.method === "GET" && path === "/admin/providers/capacity") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleProviderCapacity(req));
  }

  if (req.method === "GET" && path === "/admin/providers/quota-projection") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminProvidersQuotaProjection(req));
  }

  if (req.method === "POST" && path === "/admin/providers/quota-projection/backfill") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminProvidersQuotaProjectionBackfill(req));
  }

  if (req.method === "GET" && path === "/admin/prompt-cache-analytics") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminPromptCacheAnalytics(req));
  }

  if (req.method === "POST" && path === "/admin/api-keys") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminApiKeysCreate(req));
  }

  if (req.method === "GET" && path === "/admin/api-keys") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminApiKeysList(req));
  }

  const apiKeyPaidFallbacksPathMatch = path.match(/^\/admin\/api-keys\/([^/]+)\/paid-fallbacks$/);
  if (apiKeyPaidFallbacksPathMatch && req.method === "GET") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    const pathKeyId = apiKeyPaidFallbacksPathMatch[1] ?? "";
    let keyId: string;
    try {
      keyId = decodeURIComponent(pathKeyId);
    } catch {
      return withCors(openaiError(400, "Invalid API key id", "invalid_request_error"));
    }

    return withCors(await handleAdminApiKeysPaidFallbacks(req, keyId));
  }

  if (apiKeyPaidFallbacksPathMatch) {
    return withCors(openaiError(405, "Method not allowed", "method_not_allowed"));
  }

  if (req.method === "PATCH" && path === "/admin/api-keys") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminApiKeysUpdate(req));
  }

  if (req.method === "POST" && path === "/admin/api-keys/revoke") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminApiKeysRevoke(req));
  }

  if (req.method === "POST" && path === "/admin/api-keys/unrevoke") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminApiKeysUnrevoke(req));
  }

  if (req.method === "DELETE" && path === "/admin/api-keys") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminApiKeysDelete(req));
  }

  if (req.method === "GET" && path === "/admin/kernel-usage") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminKernelUsageGet(req));
  }

  if (req.method === "GET" && path === "/admin/kernel-policy-queue") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminKernelPolicyQueueList());
  }

  if (req.method === "POST" && path === "/admin/kernel-usage") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminKernelUsageSet(req));
  }

  if (req.method === "DELETE" && path === "/admin/kernel-usage") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminKernelUsageDelete(req));
  }

  if (req.method === "GET" && path === "/admin/kernel-pubkeys") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminKernelPubKeysList());
  }

  if (req.method === "POST" && path === "/admin/kernel-pubkeys") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminKernelPubKeysCreate(req));
  }

  if (req.method === "DELETE" && path === "/admin/kernel-pubkeys") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminKernelPubKeysDelete(req));
  }

  if (req.method === "GET" && path === "/uos/auth") {
    return withCors(await handleV1Auth(req));
  }

  if (req.method === "GET" && path === "/uos/models/catalog") {
    return withCors(await handlePublicModelCatalog());
  }

  if (req.method === "GET" && path === "/uos/models/capabilities") {
    const authResult = await authenticateClient(req);
    if (!authResult.ok) return withCors(authResult.response);
    return withCors(await handleModelCapabilities());
  }

  if (path === "/uos/agent-messages") {
    if (req.method === "GET") return withCors(await handleAgentMessagesList(req));
    if (req.method === "POST") return withCors(await handleAgentMessagesPost(req));
    return withCors(openaiError(405, "Method not allowed", "method_not_allowed"));
  }

  const isUosEmbeddingPath = path === "/uos/embeddings" || path === "/uos/embedding-jobs" ||
    path.startsWith("/uos/embedding-jobs/");
  if (!path.startsWith("/v1/") && !isUosEmbeddingPath) {
    const response = notFound();
    return withCors(req.method === "HEAD" ? withoutBody(response) : response);
  }

  const terminalRoute = terminalRouteForRequest(req.method, path);
  const authResult = await authenticateClient(req);
  if (!authResult.ok) {
    const response = withCors(withRequestId(authResult.response, requestId));
    return terminalRoute
      ? await withTerminalRequestLog(response, {
        route: terminalRoute,
        startedAtMonotonicMs: requestStartedAtMonotonicMs,
        requestId,
        deliveryCompleted: delivery?.completed,
        deliverySignal: delivery?.downstreamSignal,
      })
      : response;
  }
  const usageKeyId = authResult.method.kind === "kv_api_key" ? authResult.method.key_id : null;
  let usagePolicy = authResult.method.kind === "kv_api_key" ? authResult.method.policy : null;
  let usageReservation: ApiKeyUsageReservation | null = null;
  if (usagePolicy && terminalRoute) {
    const admission = await reserveApiKeyUsageV3(usagePolicy, requestId, terminalRoute, { deferWhenFull: true });
    if (!admission.ok) {
      const response = withCors(withRequestId(admission.response, requestId));
      return await withTerminalRequestLog(response, {
        route: terminalRoute,
        startedAtMonotonicMs: requestStartedAtMonotonicMs,
        requestId,
        deliveryCompleted: delivery?.completed,
        deliverySignal: delivery?.downstreamSignal,
      });
    }
    usageReservation = admission.reservation;
    // Admission re-reads the strict hash policy, so downstream quota headers
    // and paid fallback use the policy that actually reserved this request.
    usagePolicy = admission.reservation.policy;
  }
  const idempotencyPrincipal = await resolveIdempotencyPrincipal(authResult);
  let kernelRepo = authResult.method.kind === "github_token"
    ? { owner: authResult.method.owner, repo: authResult.method.repo }
    : null;
  if (!kernelRepo) {
    const attestation = await getKernelAttestationContext(req, authResult.token);
    if (attestation) {
      kernelRepo = { owner: attestation.owner, repo: attestation.repo };
    }
  }
  const kernelOrg = kernelRepo ? { owner: kernelRepo.owner } : null;
  let kernelReservation: KernelQuotaReservation | null = null;
  const kernelQuotaRoute = kernelQuotaRouteForRequest(req.method, path);
  if (kernelRepo && kernelQuotaRoute) {
    const admission = await reserveEffectiveKernelUsageLimit(
      kernelRepo.owner,
      kernelRepo.repo,
      requestId,
      kernelQuotaRoute,
    );
    if (!admission.ok) {
      try {
        await usageReservation?.release("kernel_quota_rejected");
      } catch (error) {
        warnQuotaAccountingFailure({ route: kernelQuotaRoute, requestId }, error);
      }
      const response = withCors(withRequestId(admission.response, requestId));
      return await withTerminalRequestLog(response, {
        route: kernelQuotaRoute,
        startedAtMonotonicMs: requestStartedAtMonotonicMs,
        requestId,
        deliveryCompleted: delivery?.completed,
        deliverySignal: delivery?.downstreamSignal,
      });
    }
    kernelReservation = admission.reservation;
  }
  const usageContext = {
    keyId: usageKeyId,
    kernelRepo,
    kernelOrg,
    paidFallbackEnabled: usagePolicy?.paid_fallback_enabled === true,
    idempotencyPrincipal,
    requestId,
    startedAtMs: requestStartedAtMs,
    startedAtMonotonicMs: requestStartedAtMonotonicMs,
    downstreamSignal: kernelReservation
      ? AbortSignal.any([delivery?.downstreamSignal ?? req.signal, kernelReservation.signal])
      : delivery?.downstreamSignal,
    beforeProviderDispatch: usageReservation?.beforeProviderDispatch,
  };
  if (terminalRoute) {
    console.info(
      "[ai.ubq.fi] request_accepted",
      JSON.stringify({
        request_id: requestId,
        route: terminalRoute,
        git_sha: runtimeGitSha(),
        deno_revision: runtimeDeploymentId(),
      }),
    );
  }
  const sentinelReplayCandidate = terminalRoute ? captureAcceptedSentinelReplayInput(req, requestId) : null;
  const takeSentinelReplayInput = (): AcceptedSentinelReplayInput | null => {
    const materialized = materializeSentinelReplayInput(sentinelReplayCandidate);
    discardSentinelReplayCaptureCandidate(sentinelReplayCandidate);
    return materialized;
  };
  const settleKernelQuota = async (
    outcome: "completed" | "incomplete",
    reason = "request_incomplete",
  ): Promise<void> => {
    if (!kernelReservation) return;
    if (outcome === "completed") await kernelReservation.commit();
    else await kernelReservation.release(reason);
  };
  const bestEffortSettleKernelQuota = async (
    outcome: "completed" | "incomplete",
    reason = "request_incomplete",
  ): Promise<void> => {
    try {
      await settleKernelQuota(outcome, reason);
    } catch (error) {
      warnQuotaAccountingFailure({ route: terminalRoute ?? "inference", requestId }, error);
    }
  };
  const finishTerminalResponse = async (
    response: Response,
    route: string,
    includeQuota = false,
    trackKernelTerminal = false,
  ): Promise<Response> => {
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
        warnQuotaAccountingFailure(
          { route: terminalRoute ?? "inference", requestId },
          runError,
        );
      }
      const quotaError = error instanceof ApiKeyQuotaDispatchError
        ? error
        : new ApiKeyQuotaDispatchError("API key quota reservation is unavailable");
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
        const observation: SentinelFailureObservation = {
          status: 500,
          stream: null,
          completed: false,
          terminal_type: "error",
          failure_kind: runError instanceof Error ? runError.name : "unknown_exception",
          synthetic_terminal_type: null,
          provider_route: "gateway",
        };
        try {
          await persistSentinelReplayFromEnvironment(sentinelReplayInput, observation);
        } catch {
          // Replay persistence is best effort and cannot replace the original
          // gateway exception or expose its request body in logs.
        } finally {
          zeroSentinelReplayInput(sentinelReplayInput);
        }
      }
      throw runError;
    }
    if (!response) {
      await bestEffortSettleKernelQuota("incomplete", "missing_inference_response");
      throw new Error("Inference handler completed without a response");
    }
    return response;
  };

  if (req.method === "GET" && path === "/v1/models") {
    return withCors(await handleModels(req));
  }

  if (req.method === "POST" && path === "/uos/embeddings") {
    const response = await executeInference(() => handleUosEmbeddings(req, usageContext));
    if (response.ok && response.headers.get("x-uos-idempotency-replayed") !== "true") {
      await bestEffortSettleKernelQuota("completed");
    } else {
      await bestEffortSettleKernelQuota(
        "incomplete",
        response.headers.get("x-uos-idempotency-replayed") === "true" ? "idempotency_replay" : "embedding_failed",
      );
    }
    return await finishTerminalResponse(response, "embeddings");
  }

  if (req.method === "POST" && path === "/uos/embedding-jobs") {
    const response = await executeInference(() => handleEmbeddingsJobCreate(req, authResult.token, usageContext));
    if (response.ok) {
      await bestEffortSettleKernelQuota("completed");
    } else {
      await bestEffortSettleKernelQuota("incomplete", "embedding_job_create_failed");
    }
    return await finishTerminalResponse(response, "embeddings.jobs.create");
  }

  if (req.method === "GET" && path.startsWith("/uos/embedding-jobs/")) {
    const jobId = path.slice("/uos/embedding-jobs/".length).trim();
    if (!jobId) {
      await bestEffortSettleKernelQuota("incomplete", "missing_embedding_job_id");
      return await finishTerminalResponse(openaiError(404, "Not found", "not_found"), "embeddings.jobs.get");
    }
    const response = await executeInference(() => handleEmbeddingsJobGet(req, authResult.token, jobId, usageContext));
    await bestEffortSettleKernelQuota("incomplete", "embedding_job_read_not_counted");
    return await finishTerminalResponse(response, "embeddings.jobs.get");
  }

  if (req.method === "POST" && (path === "/v1/images/generations" || path === "/v1/images/edits")) {
    const kind = path === "/v1/images/edits" ? "edits" : "generations";
    const response = await executeInference(() => handleImages(req, kind, usageContext));
    return await finishTerminalResponse(response, `images.${kind}`, true, true);
  }

  if (req.method === "POST" && path === "/v1/chat/completions") {
    const response = await executeInference(() => handleChatCompletions(req, usageContext));
    return await finishTerminalResponse(response, "chat.completions", true, true);
  }

  if (req.method === "POST" && path === "/v1/responses") {
    const response = await executeInference(() => handleResponses(req, usageContext));
    return await finishTerminalResponse(response, "responses", true, true);
  }

  const response = openaiError(404, "Not found", "not_found");
  return withCors(req.method === "HEAD" ? withoutBody(response) : response);
}
