import {
  handleAdminApiKeysCreate,
  handleAdminApiKeysDelete,
  handleAdminApiKeysList,
  handleAdminApiKeysPaidFallbacks,
  handleAdminApiKeysRevoke,
  handleAdminApiKeysUnrevoke,
  handleAdminApiKeysUpdate,
  handleAdminCodexAuth,
  handleAdminCodexModelsGet,
  handleAdminCodexModelsSet,
  handleAdminCodexPromptsPurge,
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
  incrementApiKeyUsage,
  requireAdminAuth,
  requireSuperAdminAuth,
} from "./auth.ts";
import { type ApiKeyPolicy, apiKeyQuotaUsedPercent } from "./api_key_policy.ts";
import { handleHealth, handleHealthAuth, handleHealthUpstream } from "./health.ts";
import { corsHeaders, notFound, openaiError, withCors } from "./http.ts";
import {
  getKernelUsageLimitSnapshot,
  incrementKernelOrgUsageLimit,
  incrementKernelUsageLimit,
} from "./kernel_usage.ts";
import {
  getResponseTelemetry,
  handleChatCompletions,
  handleEmbeddings,
  handleEmbeddingsJobCreate,
  handleEmbeddingsJobGet,
  handleModelCapabilities,
  handleModels,
  handleResponses,
  handleUosEmbeddings,
  type ResponseTelemetry,
} from "./openai.ts";
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

const logTerminalRequest = (
  input: Readonly<{
    route: string;
    response: Response;
    telemetryResponse?: Response;
    startedAtMs: number;
    keyId: string | null;
  }>,
): void => {
  const telemetry = getResponseTelemetry(input.telemetryResponse ?? input.response);
  console.info(
    "[ai.ubq.fi] request_terminal",
    JSON.stringify({
      route: input.route,
      status: input.response.status,
      provider: telemetry?.provider ?? input.response.headers.get("x-ubq-upstream") ?? "gateway",
      latency_ms: Math.max(0, Date.now() - input.startedAtMs),
      input_tokens: telemetry?.inputTokens ?? null,
      output_tokens: telemetry?.outputTokens ?? null,
      model: telemetry?.model ?? null,
      reasoning: telemetry?.reasoning ?? null,
      key_id: input.keyId,
      fallback_reason: telemetry?.fallbackReason ?? null,
    }),
  );
};

const withTerminalRequestLog = (
  response: Response,
  input: Readonly<{
    route: string;
    telemetryResponse?: Response;
    startedAtMs: number;
    keyId: string | null;
    onCompleted?: () => Promise<void>;
  }>,
): Promise<Response> => {
  let logged = false;
  let completionFinalization: Promise<void> | null = null;
  const log = (): void => {
    if (logged) return;
    logged = true;
    logTerminalRequest({ ...input, response });
  };
  const finalizeCompletion = (): Promise<void> => {
    if (!input.onCompleted || !response.ok) return Promise.resolve();
    const telemetry = getResponseTelemetry(input.telemetryResponse ?? response);
    if (!telemetry?.completed) return Promise.resolve();
    completionFinalization ??= input.onCompleted();
    return completionFinalization;
  };
  if (!response.body || !response.headers.get("Content-Type")?.toLowerCase().includes("text/event-stream")) {
    return (async () => {
      try {
        await finalizeCompletion();
        return response;
      } finally {
        log();
      }
    })();
  }

  const reader = response.body.getReader();
  const body = new ReadableStream<Uint8Array>({
    async pull(controller) {
      try {
        const { done, value } = await reader.read();
        if (done) {
          await finalizeCompletion();
          log();
          controller.close();
          return;
        }
        // The OpenAI stream observer marks response.completed before yielding
        // the chunk that contains it, so quota accounting cannot be skipped by
        // cancelling immediately after that chunk becomes visible to a client.
        await finalizeCompletion();
        controller.enqueue(value);
      } catch (error) {
        log();
        controller.error(error);
      }
    },
    async cancel(reason) {
      try {
        await reader.cancel(reason);
      } finally {
        try {
          await finalizeCompletion();
        } finally {
          log();
        }
      }
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
  if (method === "POST" && (path === "/uos/embeddings" || path === "/v1/embeddings")) return "embeddings";
  if (method === "POST" && path === "/uos/embedding-jobs") return "embeddings.jobs.create";
  if (method === "GET" && path.startsWith("/uos/embedding-jobs/")) return "embeddings.jobs.get";
  if (method === "POST" && path === "/v1/chat/completions") return "chat.completions";
  if (method === "POST" && path === "/v1/responses") return "responses";
  return null;
};

export default async function handler(req: Request): Promise<Response> {
  const requestStartedAtMs = Date.now();
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

  if (req.method === "GET" && path === "/health") {
    return withCors(await handleHealth());
  }

  if (req.method === "GET" && path === "/health/auth") {
    return withCors(await handleHealthAuth());
  }

  if (req.method === "GET" && path === "/health/upstream") {
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
    const response = withCors(authResult.response);
    return terminalRoute
      ? await withTerminalRequestLog(response, {
        route: terminalRoute,
        startedAtMs: requestStartedAtMs,
        keyId: null,
      })
      : response;
  }
  const requestId = crypto.randomUUID();
  const usageKeyId = authResult.method.kind === "kv_api_key" ? authResult.method.key_id : null;
  const usagePolicy = authResult.method.kind === "kv_api_key" ? authResult.method.policy : null;
  const kernelLimitScope = authResult.method.kind === "github_token" ? authResult.method.limit_scope : null;
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
  };
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
    return await withTerminalRequestLog(withCors(decorated), {
      route,
      telemetryResponse: response,
      startedAtMs: requestStartedAtMs,
      keyId: usageKeyId,
      onCompleted,
    });
  };
  const incrementInferenceUsage = async (): Promise<void> => {
    if (usagePolicy) await incrementApiKeyUsage(usagePolicy);
    await incrementKernelLimitUsage();
  };

  if (req.method === "GET" && path === "/v1/models") {
    return withCors(await handleModels(req));
  }

  if (req.method === "POST" && path === "/uos/embeddings") {
    const response = await handleUosEmbeddings(req, usageContext);
    if (response.ok && response.headers.get("x-uos-idempotency-replayed") !== "true") {
      if (usagePolicy) await incrementApiKeyUsage(usagePolicy);
      await incrementKernelLimitUsage();
    }
    return await finishTerminalResponse(response, "embeddings");
  }

  if (req.method === "POST" && path === "/uos/embedding-jobs") {
    const response = await handleEmbeddingsJobCreate(req, authResult.token, usageContext);
    if (response.ok) {
      if (usagePolicy) await incrementApiKeyUsage(usagePolicy);
      await incrementKernelLimitUsage();
    }
    return await finishTerminalResponse(response, "embeddings.jobs.create");
  }

  if (req.method === "GET" && path.startsWith("/uos/embedding-jobs/")) {
    const jobId = path.slice("/uos/embedding-jobs/".length).trim();
    if (!jobId) {
      return await finishTerminalResponse(openaiError(404, "Not found", "not_found"), "embeddings.jobs.get");
    }
    const response = await handleEmbeddingsJobGet(req, authResult.token, jobId, usageContext);
    return await finishTerminalResponse(response, "embeddings.jobs.get");
  }

  if (req.method === "POST" && path === "/v1/embeddings") {
    const response = await handleEmbeddings(req, usageContext);
    if (response.ok) {
      if (usagePolicy) await incrementApiKeyUsage(usagePolicy);
      await incrementKernelLimitUsage();
    }
    return await finishTerminalResponse(response, "embeddings");
  }

  if (req.method === "POST" && path === "/v1/chat/completions") {
    const response = await handleChatCompletions(req, usageContext);
    return await finishTerminalResponse(response, "chat.completions", true, incrementInferenceUsage);
  }

  if (req.method === "POST" && path === "/v1/responses") {
    const response = await handleResponses(req, usageContext);
    return await finishTerminalResponse(response, "responses", true, incrementInferenceUsage);
  }

  return withCors(openaiError(404, "Not found", "not_found"));
}
