import {
  handleAdminApiKeysCreate,
  handleAdminApiKeysDelete,
  handleAdminApiKeysList,
  handleAdminApiKeysPaidFallbacks,
  handleAdminApiKeysRevoke,
  handleAdminApiKeysUnrevoke,
  handleAdminApiKeysUpdate,
  handleAdminCodexAuth,
  handleAdminCodexCacheScopeExperiment,
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
import { handleRoot, handleStaticAsset } from "./static.ts";
import { sha256Hex } from "./utils.ts";

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
    first_codex_dispatch_ms: telemetry?.firstCodexDispatchMs ?? null,
    first_codex_headers_ms: telemetry?.firstCodexHeadersMs ?? null,
    first_sse_event_ms: telemetry?.firstSseEventMs ?? null,
    stream_terminal_ms: telemetry?.streamTerminalMs ?? null,
    downstream_drain_ms: downstreamDrainMs,
    model: telemetry?.model ?? null,
    reasoning: telemetry?.reasoning ?? null,
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
    git_sha: runtimeGitSha(),
    deno_revision: runtimeDeploymentId(),
    router_revision: input.response.headers.get("x-uos-router-revision"),
  };
  console.info("[ai.ubq.fi] request_terminal", JSON.stringify(terminal));
  await recordPromptCacheTelemetry({
    provider: terminal.provider,
    model: terminal.model,
    route: terminal.route,
    status: terminal.status,
    completed: telemetry?.completed ?? false,
    usageTelemetryStatus: terminal.usage_telemetry_status,
  });
};

const warnQuotaAccountingFailure = (
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
        errors: errors.map((item) => item instanceof Error ? item.message : String(item)),
      }),
    );
  } catch {
    // Accounting and its warning are both best-effort after completion. Neither
    // may replace an upstream response that is already ready for the client.
  }
};

const withTerminalRequestLog = (
  response: Response,
  input: Readonly<{
    route: string;
    telemetryResponse?: Response;
    startedAtMonotonicMs: number;
    requestId: string;
    onCompleted?: () => Promise<void>;
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
    async cancel(reason) {
      void reader.cancel(reason).catch(() => {});
      void finalizeCompletion();
      await log();
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
  if (req.method === "OPTIONS") {
    return withCors(new Response(null, { status: 204, headers: corsHeaders() }));
  }

  const url = new URL(req.url);
  const path = normalizePath(url.pathname);

  if (req.method === "GET" && (path === "/" || path === "/index.html")) {
    return withCors(await handleRoot(req));
  }

  if (req.method === "GET") {
    const staticResponse = await handleStaticAsset(path);
    if (staticResponse) return withCors(staticResponse);
  }

  if ((req.method === "GET" || req.method === "HEAD") && path === "/health") {
    const health = await handleHealth();
    // Keep HEAD semantically equivalent to public GET liveness while correctly
    // omitting the body.
    return withCors(
      req.method === "HEAD"
        ? new Response(null, { status: health.status, statusText: health.statusText, headers: health.headers })
        : health,
    );
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

  if ((req.method === "GET" || req.method === "POST") && path === "/admin/defaults") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminDefaults(req));
  }

  if (req.method === "GET" && path === "/admin/providers") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleHealthProviders({ includeQuota: true }));
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
    return withCors(notFound());
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
      })
      : response;
  }
  const usageKeyId = authResult.method.kind === "kv_api_key" ? authResult.method.key_id : null;
  let usagePolicy = authResult.method.kind === "kv_api_key" ? authResult.method.policy : null;
  const kernelLimitScope = authResult.method.kind === "github_token" ? authResult.method.limit_scope : null;
  let usageReservation: ApiKeyUsageReservation | null = null;
  if (usagePolicy && terminalRoute) {
    const admission = await reserveApiKeyUsageV3(usagePolicy, requestId, terminalRoute, { deferWhenFull: true });
    if (!admission.ok) {
      const response = withCors(withRequestId(admission.response, requestId));
      return await withTerminalRequestLog(response, {
        route: terminalRoute,
        startedAtMonotonicMs: requestStartedAtMonotonicMs,
        requestId,
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
  const usageContext = {
    keyId: usageKeyId,
    kernelRepo,
    kernelOrg,
    paidFallbackEnabled: usagePolicy?.paid_fallback_enabled === true,
    idempotencyPrincipal,
    requestId,
    startedAtMs: requestStartedAtMs,
    startedAtMonotonicMs: requestStartedAtMonotonicMs,
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
    const telemetry = getResponseTelemetry(response);
    const decorated = includeQuota ? decorateInferenceQuota(response, usagePolicy, telemetry) : response;
    return await withTerminalRequestLog(withCors(withRequestId(decorated, requestId)), {
      route,
      telemetryResponse: response,
      startedAtMonotonicMs: requestStartedAtMonotonicMs,
      requestId,
      onCompleted,
    });
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
    return withCors(await handleModels(req));
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

  return withCors(openaiError(404, "Not found", "not_found"));
}
