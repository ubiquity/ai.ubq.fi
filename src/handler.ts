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
} from "./admin.ts";
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
  type ApiKeyUsageReservation,
  reserveApiKeyUsageV3,
} from "./api_key_policy.ts";
import { runtimeDeploymentId, runtimeGitSha } from "./config.ts";
import { handleHealth, handleHealthProviders, handleHealthUpstream } from "./health.ts";
import { corsHeaders, notFound, openaiError, withCors } from "./http.ts";
import {
  getKernelUsageLimitSnapshot,
  incrementKernelOrgUsageLimit,
  incrementKernelUsageLimit,
} from "./kernel_usage.ts";
import {
  getResponseTelemetry,
  handleChatCompletions,
  handleEmbeddingsJobCreate,
  handleEmbeddingsJobGet,
  handleModelCapabilities,
  handleModels,
  handleResponses,
  handleUosEmbeddings,
  type ResponseTelemetry,
} from "./openai.ts";
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
import {
  handleMarketplaceCreateAuth,
  handleMarketplaceDisableAuth,
  handleMarketplaceListAuth,
  handleMarketplacePublicCatalog,
  handleMarketplaceUpdateAuth,
} from "./marketplace.ts";
import { handleRoot, handleStaticAsset } from "./static.ts";
import { sha256Hex } from "./utils.ts";
import { handleProviderCapacity } from "./provider_capacity.ts";

type AuthenticatedClientResult = Extract<
  Awaited<ReturnType<typeof authenticateClient>>,
  { ok: true }
>;

export const resolveIdempotencyPrincipal = async (
  authResult: Readonly<{
    token: string | null;
    method:
      | Readonly<{ kind: "kv_api_key"; key_id: string }>
      | Exclude<AuthenticatedClientResult["method"], { kind: "kv_api_key" }>;
  }>,
): Promise<string> => {
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

const decodeMarketplaceAuthId = (value: string): string | null => {
  try {
    const decoded = decodeURIComponent(value);
    return /^auth_[A-Za-z0-9-]+$/.test(decoded) ? decoded : null;
  } catch {
    return null;
  }
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

type InferenceTimingState = {
  authStartedAtMonotonicMs: number;
  authCompletedAtMonotonicMs: number;
  quotaCompletedAtMonotonicMs: number;
  contextReadyAtMonotonicMs: number;
};

const serverTimingDuration = (value: number): string => String(Math.max(0, Math.round(value)));

const withInferenceServerTiming = (
  response: Response,
  telemetry: ResponseTelemetry | null,
  requestStartedAtMonotonicMs: number,
  timing: InferenceTimingState,
): Response => {
  const completedAtMonotonicMs = performance.now();
  const metrics = [
    `auth;dur=${serverTimingDuration(timing.authCompletedAtMonotonicMs - timing.authStartedAtMonotonicMs)}`,
    `quota;dur=${serverTimingDuration(timing.quotaCompletedAtMonotonicMs - timing.authCompletedAtMonotonicMs)}`,
    `context;dur=${serverTimingDuration(timing.contextReadyAtMonotonicMs - timing.quotaCompletedAtMonotonicMs)}`,
  ];
  if (telemetry?.firstProviderDispatchMs !== null && telemetry?.firstProviderDispatchMs !== undefined) {
    const dispatchAtMonotonicMs = requestStartedAtMonotonicMs + telemetry.firstProviderDispatchMs;
    metrics.push(
      `pre_provider;dur=${serverTimingDuration(dispatchAtMonotonicMs - timing.contextReadyAtMonotonicMs)}`,
    );
    if (telemetry.firstProviderHeadersMs !== null) {
      const headersAtMonotonicMs = requestStartedAtMonotonicMs + telemetry.firstProviderHeadersMs;
      metrics.push(
        `provider;dur=${serverTimingDuration(headersAtMonotonicMs - dispatchAtMonotonicMs)}`,
        `post_provider;dur=${serverTimingDuration(completedAtMonotonicMs - headersAtMonotonicMs)}`,
      );
    }
  }
  metrics.push(`total;dur=${serverTimingDuration(completedAtMonotonicMs - requestStartedAtMonotonicMs)}`);
  const headers = new Headers(response.headers);
  headers.set("Server-Timing", metrics.join(", "));
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
  return withCodexQuotaHeaders(response, usedPercent === null ? null : { used_percent: usedPercent });
};

const logTerminalRequest = async (
  input: Readonly<{
    route: string;
    response: Response;
    telemetryResponse?: Response;
    startedAtMonotonicMs: number;
    downstreamDrainedAtMonotonicMs?: number;
    requestId: string;
    recordTelemetry?: typeof recordPromptCacheTelemetry;
  }>,
): Promise<void> => {
  const telemetry = getResponseTelemetry(input.telemetryResponse ?? input.response);
  const latencyMs = Math.max(0, Math.round(performance.now() - input.startedAtMonotonicMs));
  const downstreamDrainMs = telemetry?.stream === true && telemetry.firstSseEventMs !== null &&
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
    first_sse_event_ms: telemetry?.firstSseEventMs ?? null,
    stream_terminal_ms: telemetry?.streamTerminalMs ?? null,
    downstream_drain_ms: downstreamDrainMs,
    model: telemetry?.model ?? null,
    reasoning: telemetry?.reasoning ?? null,
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
    affinity_outcome: telemetry?.affinityOutcome ?? "none",
    fallback_reason: telemetry?.fallbackReason ?? null,
    stream: telemetry?.stream ?? null,
    stream_terminal_type: telemetry?.streamTerminalType ?? null,
    attempted_providers: telemetry?.attemptedProviders ?? [],
    openrouter_trigger_class: telemetry?.openRouterTriggerClass ?? null,
    openrouter_circuit_transition: telemetry?.openRouterCircuitTransition ?? null,
    openrouter_selected_model: telemetry?.openRouterSelectedModel ?? null,
    openrouter_task_type: telemetry?.openRouterTaskType ?? null,
    openrouter_latency_ms: telemetry?.openRouterLatencyMs ?? null,
    openrouter_terminal_status: telemetry?.openRouterTerminalStatus ?? null,
    openrouter_semantic_commitment: telemetry?.openRouterSemanticCommitment ?? null,
    git_sha: runtimeGitSha(),
    deno_revision: runtimeDeploymentId(),
    router_revision: input.response.headers.get("x-uos-router-revision"),
  };
  console.info("[ai.ubq.fi] request_terminal", JSON.stringify(terminal));
  await (input.recordTelemetry ?? recordPromptCacheTelemetry)({
    provider: terminal.provider,
    model: terminal.model,
    route: terminal.route,
    status: terminal.status,
    completed: telemetry?.completed ?? false,
    usageTelemetryStatus: terminal.usage_telemetry_status,
    cacheWriteTokensPresent: terminal.cache_write_input_tokens !== null,
  });
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
    onCompleted?: () => Promise<void>;
    /** Test seam for proving terminal telemetry remains best effort. */
    recordTelemetry?: typeof recordPromptCacheTelemetry;
  }>,
): Promise<Response> => {
  let terminalLog: Promise<void> | null = null;
  let completionFinalization: Promise<void> | null = null;
  const log = (downstreamDrainedAtMonotonicMs?: number): Promise<void> => {
    if (terminalLog) return terminalLog;
    terminalLog = logTerminalRequest({ ...input, response, downstreamDrainedAtMonotonicMs }).catch(() => {
      // Terminal logging and its durable baseline counters are best effort;
      // neither may replace a response that is already ready for the client.
    });
    return terminalLog;
  };
  const finalizeCompletion = (): Promise<void> => {
    const onCompleted = input.onCompleted;
    if (!onCompleted || !response.ok) return Promise.resolve();
    const telemetry = getResponseTelemetry(input.telemetryResponse ?? response);
    if (!telemetry?.completed) return Promise.resolve();
    completionFinalization ??= (async () => {
      try {
        await onCompleted();
      } catch (error) {
        warnQuotaAccountingFailure(input, error);
      }
    })();
    return completionFinalization;
  };
  if (!response.body || !response.headers.get("Content-Type")?.toLowerCase().includes("text/event-stream")) {
    return (async () => {
      try {
        await finalizeCompletion();
        return response;
      } finally {
        await log();
      }
    })();
  }

  const reader = response.body.getReader();
  const body = new ReadableStream<Uint8Array>({
    async pull(controller) {
      try {
        const { done, value } = await reader.read();
        if (done) {
          // Snapshot the downstream drain before finalization. Accounting can
          // wait on KV and belongs in total latency, not drain telemetry.
          const downstreamDrainedAtMonotonicMs = performance.now();
          // The terminal bytes have already been delivered. Finish accounting
          // before closing the downstream body so callers observe durable
          // counters without holding back the terminal frame itself.
          await finalizeCompletion();
          await log(downstreamDrainedAtMonotonicMs);
          controller.close();
          return;
        }
        // The OpenAI stream observer marks response.completed before yielding
        // the chunk that contains it. Schedule accounting, but never hold back
        // the provider bytes that are already ready for the client.
        void finalizeCompletion();
        controller.enqueue(value);
      } catch (error) {
        await log();
        controller.error(error);
      }
    },
    cancel(reason) {
      // Cancellation must not await a concurrently pending provider pull;
      // that pull observes the cancellation and performs layered cleanup.
      void reader.cancel(reason).catch(() => {});
      void finalizeCompletion();
      void log();
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
  return null;
};

export default async function handler(req: Request): Promise<Response> {
  const requestStartedAtMs = Date.now();
  const requestStartedAtMonotonicMs = performance.now();
  const requestId = crypto.randomUUID();
  const applyCors = (response: Response): Response => withCors(response, req);
  if (req.method === "OPTIONS") {
    return applyCors(new Response(null, { status: 204, headers: corsHeaders(req) }));
  }

  const url = new URL(req.url);
  const path = normalizePath(url.pathname);

  if (req.method === "GET" && (path === "/" || path === "/index.html")) {
    return applyCors(await handleRoot(req));
  }

  if (req.method === "GET") {
    const staticResponse = await handleStaticAsset(path);
    if (staticResponse) return applyCors(staticResponse);
  }

  if ((req.method === "GET" || req.method === "HEAD") && path === "/health") {
    const health = await handleHealth();
    // Keep HEAD semantically equivalent to public GET liveness while correctly
    // omitting the body.
    return applyCors(
      req.method === "HEAD"
        ? new Response(null, { status: health.status, statusText: health.statusText, headers: health.headers })
        : health,
    );
  }

  if (req.method === "GET" && path === "/health/providers") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleHealthProviders());
  }

  if (req.method === "GET" && path === "/health/upstream") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleHealthUpstream());
  }

  if (req.method === "POST" && path === "/api/auth/register/start") {
    const auth = await authenticateAdmin(req);
    if (!auth.ok) return applyCors(auth.response);
    return applyCors(await handlePasskeyRegisterStart(req, { defaultIsAdmin: auth.is_super_admin }));
  }

  if (req.method === "POST" && path === "/api/auth/register/finish") {
    return applyCors(await handlePasskeyRegisterFinish(req));
  }

  if (req.method === "POST" && path === "/api/auth/login/start") {
    return applyCors(await handlePasskeyLoginStart(req));
  }

  if (req.method === "POST" && path === "/api/auth/login/finish") {
    return applyCors(await handlePasskeyLoginFinish(req));
  }

  if (req.method === "GET" && path === "/api/auth/session") {
    return applyCors(await handlePasskeySession(req));
  }

  if (req.method === "POST" && path === "/api/auth/logout") {
    return applyCors(await handlePasskeyLogout(req));
  }

  if (req.method === "GET" && path === "/admin/passkey-users") {
    const authError = await requireSuperAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handlePasskeyUsersList());
  }

  if (req.method === "PATCH" && path === "/admin/passkey-users") {
    const authError = await requireSuperAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handlePasskeyUsersUpdate(req));
  }

  if (req.method === "POST" && path === "/admin/codex/auth") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminCodexAuth(req));
  }

  if (req.method === "GET" && path === "/admin/providers/codex/banked-resets/shadow-decisions") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminCodexBankedResetShadowDecisions());
  }

  if (req.method === "GET" && path === "/admin/providers/codex/cache-scope-experiment") {
    const authError = await requireSuperAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminCodexCacheScopeExperimentTelemetryBaseline());
  }

  if (req.method === "POST" && path === "/admin/providers/codex/cache-scope-experiment") {
    const authError = await requireSuperAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminCodexCacheScopeExperiment(req));
  }

  const codexRecheckMatch = path.match(/^\/admin\/providers\/codex\/(\d+)\/recheck$/);
  if (req.method === "POST" && codexRecheckMatch) {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminCodexRecheck(Number(codexRecheckMatch[1])));
  }

  if (req.method === "GET" && path === "/admin/codex/models") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminCodexModelsGet());
  }

  if (req.method === "POST" && path === "/admin/codex/models") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminCodexModelsSet(req));
  }

  if (req.method === "POST" && path === "/admin/codex/prompts/purge") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminCodexPromptsPurge());
  }

  if (req.method === "POST" && path === "/admin/kv-migration/import") {
    const authError = await requireSuperAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminKvMigrationImport(req));
  }

  if (req.method === "GET" && path === "/admin/kv-migration/validate") {
    const authError = await requireSuperAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminKvMigrationValidate());
  }

  if ((req.method === "GET" || req.method === "POST") && path === "/admin/defaults") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminDefaults(req));
  }

  if (req.method === "GET" && path === "/admin/providers") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleHealthProviders({ includeQuota: true }));
  }

  if (req.method === "GET" && path === "/admin/providers/capacity") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleProviderCapacity(req));
  }

  if (req.method === "POST" && path === "/admin/api-keys") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminApiKeysCreate(req));
  }

  if (req.method === "GET" && path === "/admin/api-keys") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminApiKeysList(req));
  }

  const apiKeyPaidFallbacksPathMatch = path.match(/^\/admin\/api-keys\/([^/]+)\/paid-fallbacks$/);
  if (apiKeyPaidFallbacksPathMatch && req.method === "GET") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    const pathKeyId = apiKeyPaidFallbacksPathMatch[1] ?? "";
    let keyId: string;
    try {
      keyId = decodeURIComponent(pathKeyId);
    } catch {
      return applyCors(openaiError(400, "Invalid API key id", "invalid_request_error"));
    }

    return applyCors(await handleAdminApiKeysPaidFallbacks(req, keyId));
  }

  if (apiKeyPaidFallbacksPathMatch) {
    return applyCors(openaiError(405, "Method not allowed", "method_not_allowed"));
  }

  if (req.method === "PATCH" && path === "/admin/api-keys") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminApiKeysUpdate(req));
  }

  if (req.method === "POST" && path === "/admin/api-keys/revoke") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminApiKeysRevoke(req));
  }

  if (req.method === "POST" && path === "/admin/api-keys/unrevoke") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminApiKeysUnrevoke(req));
  }

  if (req.method === "DELETE" && path === "/admin/api-keys") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminApiKeysDelete(req));
  }

  if (req.method === "GET" && path === "/admin/kernel-usage") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminKernelUsageGet(req));
  }

  if (req.method === "GET" && path === "/admin/kernel-policy-queue") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminKernelPolicyQueueList());
  }

  if (req.method === "POST" && path === "/admin/kernel-usage") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminKernelUsageSet(req));
  }

  if (req.method === "DELETE" && path === "/admin/kernel-usage") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminKernelUsageDelete(req));
  }

  if (req.method === "GET" && path === "/admin/kernel-pubkeys") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminKernelPubKeysList());
  }

  if (req.method === "POST" && path === "/admin/kernel-pubkeys") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminKernelPubKeysCreate(req));
  }

  if (req.method === "DELETE" && path === "/admin/kernel-pubkeys") {
    const authError = await requireAdminAuth(req);
    if (authError) return applyCors(authError);
    return applyCors(await handleAdminKernelPubKeysDelete(req));
  }

  if (req.method === "GET" && path === "/uos/auth") {
    return applyCors(await handleV1Auth(req));
  }

  if (req.method === "GET" && path === "/uos/models/capabilities") {
    const authResult = await authenticateClient(req);
    if (!authResult.ok) return applyCors(authResult.response);
    return applyCors(await handleModelCapabilities());
  }

  if (path === "/uos/agent-messages") {
    if (req.method === "GET") return applyCors(await handleAgentMessagesList(req));
    if (req.method === "POST") return applyCors(await handleAgentMessagesPost(req));
    return applyCors(openaiError(405, "Method not allowed", "method_not_allowed"));
  }

  if (path === "/marketplace/auths") {
    if (req.method === "POST") return applyCors(await handleMarketplaceCreateAuth(req));
    if (req.method === "GET") return applyCors(await handleMarketplacePublicCatalog(req));
    return applyCors(openaiError(405, "Method not allowed", "method_not_allowed"));
  }

  if (path === "/marketplace/auths/me") {
    if (req.method === "GET") return applyCors(await handleMarketplaceListAuth(req));
    return applyCors(openaiError(405, "Method not allowed", "method_not_allowed"));
  }

  const marketplaceDisableMatch = path.match(/^\/marketplace\/auths\/([^/]+)\/disable$/);
  if (marketplaceDisableMatch) {
    if (req.method !== "POST") return applyCors(openaiError(405, "Method not allowed", "method_not_allowed"));
    const id = decodeMarketplaceAuthId(marketplaceDisableMatch[1]);
    if (!id) return applyCors(openaiError(400, "Invalid marketplace auth id", "invalid_request_error"));
    return applyCors(await handleMarketplaceDisableAuth(req, id));
  }

  const marketplaceAuthMatch = path.match(/^\/marketplace\/auths\/([^/]+)$/);
  if (marketplaceAuthMatch) {
    if (req.method !== "PATCH") return applyCors(openaiError(405, "Method not allowed", "method_not_allowed"));
    const id = decodeMarketplaceAuthId(marketplaceAuthMatch[1]);
    if (!id) return applyCors(openaiError(400, "Invalid marketplace auth id", "invalid_request_error"));
    return applyCors(await handleMarketplaceUpdateAuth(req, id));
  }

  const isUosEmbeddingPath = path === "/uos/embeddings" || path === "/uos/embedding-jobs" ||
    path.startsWith("/uos/embedding-jobs/");
  if (!path.startsWith("/v1/") && !isUosEmbeddingPath) {
    return applyCors(notFound());
  }

  const terminalRoute = terminalRouteForRequest(req.method, path);
  const authStartedAtMonotonicMs = performance.now();
  const authResult = await authenticateClient(req);
  const authCompletedAtMonotonicMs = performance.now();
  if (!authResult.ok) {
    const response = applyCors(withRequestId(authResult.response, requestId));
    return terminalRoute
      ? await withTerminalRequestLog(response, {
        route: terminalRoute,
        startedAtMonotonicMs: requestStartedAtMonotonicMs,
        requestId,
      })
      : response;
  }
  const usageKeyId = authResult.method.kind === "kv_api_key" ? authResult.method.key_id : null;
  let usagePolicy = authResult.method.kind === "kv_api_key" ? authResult.method.policy : null;
  const kernelLimitScope = authResult.method.kind === "github_token" ? authResult.method.limit_scope : null;
  let usageReservation: ApiKeyUsageReservation | null = null;
  if (usagePolicy && terminalRoute) {
    const admission = await reserveApiKeyUsageV3(usagePolicy, requestId, terminalRoute, {
      deferWhenFull: true,
      ...(terminalRoute === "chat.completions" || terminalRoute === "responses"
        ? { unmeteredProviderWhenUnlimited: "cerebras" as const }
        : {}),
    });
    if (!admission.ok) {
      const response = applyCors(withRequestId(admission.response, requestId));
      return await withTerminalRequestLog(response, {
        route: terminalRoute,
        startedAtMonotonicMs: requestStartedAtMonotonicMs,
        requestId,
      });
    }
    usageReservation = admission.reservation;
    // Full admission exposes its strict live policy immediately. The unlimited
    // Cerebras path refreshes this getter at provider dispatch, and response
    // decoration samples it again after the handler completes.
    usagePolicy = admission.reservation.policy;
  }
  const quotaCompletedAtMonotonicMs = performance.now();
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
  const usageContext = {
    keyId: usageKeyId,
    kernelRepo,
    kernelOrg,
    get paidFallbackEnabled() {
      return (usageReservation?.policy ?? usagePolicy)?.paid_fallback_enabled === true;
    },
    idempotencyPrincipal,
    requestId,
    startedAtMs: requestStartedAtMs,
    startedAtMonotonicMs: requestStartedAtMonotonicMs,
    beforeProviderDispatch: usageReservation?.beforeProviderDispatch,
  };
  const inferenceTiming: InferenceTimingState = {
    authStartedAtMonotonicMs,
    authCompletedAtMonotonicMs,
    quotaCompletedAtMonotonicMs,
    contextReadyAtMonotonicMs: performance.now(),
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
  const resolveKernelLimitScope = async (): Promise<"org" | "repo" | null> => {
    if (!kernelRepo) return null;
    if (kernelLimitScope) return kernelLimitScope;
    const snapshot = await getKernelUsageLimitSnapshot(kernelRepo.owner, kernelRepo.repo);
    if (snapshot?.source === "kv") return "repo";
    return "org";
  };
  const incrementKernelLimitUsage = async (): Promise<void> => {
    const limitScope = await resolveKernelLimitScope();
    if (!kernelRepo || !limitScope) return;
    if (limitScope === "repo") {
      await incrementKernelUsageLimit(kernelRepo.owner, kernelRepo.repo);
      return;
    }
    if (kernelOrg) await incrementKernelOrgUsageLimit(kernelOrg.owner);
  };
  const finishTerminalResponse = async (
    response: Response,
    route: string,
    includeQuota = false,
    onCompleted?: () => Promise<void>,
  ): Promise<Response> => {
    usagePolicy = usageReservation?.policy ?? usagePolicy;
    const telemetry = getResponseTelemetry(response);
    const decorated = includeQuota ? decorateInferenceQuota(response, usagePolicy, telemetry) : response;
    const terminalResponse = await withTerminalRequestLog(applyCors(withRequestId(decorated, requestId)), {
      route,
      telemetryResponse: response,
      startedAtMonotonicMs: requestStartedAtMonotonicMs,
      requestId,
      onCompleted,
    });
    return withInferenceServerTiming(
      terminalResponse,
      telemetry,
      requestStartedAtMonotonicMs,
      inferenceTiming,
    );
  };
  const bestEffortKernelInferenceUsage = async (): Promise<void> => {
    try {
      await incrementKernelLimitUsage();
    } catch (error) {
      warnQuotaAccountingFailure(
        { route: terminalRoute ?? "inference", requestId },
        error,
      );
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
        ...(quotaError.retryAfter ? { headers: { "Retry-After": quotaError.retryAfter } } : {}),
      });
    }
    if (runError instanceof ApiKeyQuotaDispatchError) {
      return openaiError(runError.status, runError.message, runError.code, {
        type: runError.errorType,
        ...(runError.retryAfter ? { headers: { "Retry-After": runError.retryAfter } } : {}),
      });
    }
    if (runError) throw runError;
    if (!response) throw new Error("Inference handler completed without a response");
    return response;
  };

  if (req.method === "GET" && path === "/v1/models") {
    return applyCors(await handleModels(req));
  }

  if (req.method === "POST" && path === "/uos/embeddings") {
    const response = await executeInference(() => handleUosEmbeddings(req, usageContext));
    if (response.ok && response.headers.get("x-uos-idempotency-replayed") !== "true") {
      await bestEffortKernelInferenceUsage();
    }
    return await finishTerminalResponse(response, "embeddings");
  }

  if (req.method === "POST" && path === "/uos/embedding-jobs") {
    const response = await executeInference(() => handleEmbeddingsJobCreate(req, authResult.token, usageContext));
    if (response.ok) {
      await bestEffortKernelInferenceUsage();
    }
    return await finishTerminalResponse(response, "embeddings.jobs.create");
  }

  if (req.method === "GET" && path.startsWith("/uos/embedding-jobs/")) {
    const jobId = path.slice("/uos/embedding-jobs/".length).trim();
    if (!jobId) {
      return await finishTerminalResponse(openaiError(404, "Not found", "not_found"), "embeddings.jobs.get");
    }
    const response = await executeInference(() => handleEmbeddingsJobGet(req, authResult.token, jobId, usageContext));
    return await finishTerminalResponse(response, "embeddings.jobs.get");
  }

  if (req.method === "POST" && path === "/v1/chat/completions") {
    const response = await executeInference(() => handleChatCompletions(req, usageContext));
    return await finishTerminalResponse(response, "chat.completions", true, incrementKernelLimitUsage);
  }

  if (req.method === "POST" && path === "/v1/responses") {
    const response = await executeInference(() => handleResponses(req, usageContext));
    return await finishTerminalResponse(response, "responses", true, incrementKernelLimitUsage);
  }

  return applyCors(openaiError(404, "Not found", "not_found"));
}
