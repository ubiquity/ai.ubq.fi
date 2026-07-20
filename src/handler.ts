import {
  handleAdminApiKeysCreate,
  handleAdminApiKeysDelete,
  handleAdminApiKeysList,
  handleAdminApiKeysRequests,
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
import { handleHealth, handleHealthAuth, handleHealthUpstream } from "./health.ts";
import { corsHeaders, notFound, openaiError, withCors } from "./http.ts";
import {
  getKernelUsageLimitSnapshot,
  incrementKernelOrgUsageLimit,
  incrementKernelUsageLimit,
} from "./kernel_usage.ts";
import {
  handleChatCompletions,
  handleEmbeddings,
  handleEmbeddingsJobCreate,
  handleEmbeddingsJobGet,
  handleModelCapabilities,
  handleModels,
  handleResponses,
  handleUosEmbeddings,
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
import { recordApiKeyRequestLog } from "./analytics.ts";
import { ensurePaidFallbackBackfill } from "./paid_fallback.ts";
import { handleRoot, handleStaticAsset } from "./static.ts";

const normalizePath = (path: string): string => {
  if (path === "/") return path;
  return path.replace(/\/+$/, "");
};

export default async function handler(req: Request): Promise<Response> {
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

  try {
    await ensurePaidFallbackBackfill();
  } catch (error) {
    console.error("[ai.ubq.fi] Paid fallback API key backfill failed:", error);
    return withCors(openaiError(503, "API key migration is unavailable", "server_error", {
      type: "server_error",
    }));
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

  const apiKeyRequestsPathMatch = path.match(/^\/admin\/api-keys\/([^/]+)\/requests$/);
  if (apiKeyRequestsPathMatch && req.method === "GET") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    const pathKeyId = apiKeyRequestsPathMatch[1] ?? "";
    let keyId: string;
    try {
      keyId = decodeURIComponent(pathKeyId);
    } catch {
      return withCors(openaiError(400, "Invalid API key id", "invalid_request_error"));
    }

    return withCors(await handleAdminApiKeysRequests(req, keyId));
  }

  if (apiKeyRequestsPathMatch) {
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

  const authResult = await authenticateClient(req);
  if (!authResult.ok) return withCors(authResult.response);
  const requestId = crypto.randomUUID();
  const requestStartedAtMs = Date.now();
  const usageKeyId = authResult.method.kind === "kv_api_key" ? authResult.method.key_id : null;
  const kernelLimitScope = authResult.method.kind === "github_token" ? authResult.method.limit_scope : null;
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

  const logApiKeyRequest = async (details: {
    route: string;
    path: string;
    method: string;
    status_code: number;
    stream: boolean;
    model?: string | null;
    reasoning?: string | null;
    provider?: "chatgpt_codex" | "voyage" | "yunwu";
  }): Promise<void> => {
    if (!usageKeyId) return;
    await recordApiKeyRequestLog(usageKeyId, {
      id: requestId,
      route: details.route,
      path: details.path,
      method: details.method,
      status_code: details.status_code,
      stream: details.stream,
      created_at_ms: requestStartedAtMs,
      provider: details.provider ?? "chatgpt_codex",
      ...(details.model !== undefined ? { model: details.model } : {}),
      ...(details.reasoning !== undefined ? { reasoning: details.reasoning } : {}),
      ...(!details.stream
        ? {
          completed_at_ms: Date.now(),
          latency_ms: Math.max(0, Date.now() - requestStartedAtMs),
        }
        : {}),
    });
  };

  if (req.method === "GET" && path === "/v1/models") {
    return withCors(await handleModels());
  }

  if (req.method === "POST" && path === "/uos/embeddings") {
    const response = await handleUosEmbeddings(req, usageContext);
    if (response.ok) {
      if (usageKeyId) await incrementApiKeyUsage(usageKeyId);
      await incrementKernelLimitUsage();
    }
    await logApiKeyRequest({
      route: "embeddings",
      path,
      method: req.method,
      status_code: response.status,
      stream: false,
      provider: "voyage",
    });
    return withCors(response);
  }

  if (req.method === "POST" && path === "/uos/embedding-jobs") {
    const response = await handleEmbeddingsJobCreate(req, authResult.token, usageContext);
    if (response.ok) {
      if (usageKeyId) await incrementApiKeyUsage(usageKeyId);
      await incrementKernelLimitUsage();
    }
    await logApiKeyRequest({
      route: "embeddings.jobs.create",
      path,
      method: req.method,
      status_code: response.status,
      stream: false,
      provider: "voyage",
    });
    return withCors(response);
  }

  if (req.method === "GET" && path.startsWith("/uos/embedding-jobs/")) {
    const jobId = path.slice("/uos/embedding-jobs/".length).trim();
    if (!jobId) return withCors(openaiError(404, "Not found", "not_found"));
    const response = await handleEmbeddingsJobGet(req, authResult.token, jobId, usageContext);
    await logApiKeyRequest({
      route: "embeddings.jobs.get",
      path,
      method: req.method,
      status_code: response.status,
      stream: false,
      provider: "voyage",
    });
    return withCors(response);
  }

  if (req.method === "POST" && path === "/v1/embeddings") {
    const response = await handleEmbeddings(req, usageContext);
    if (response.ok) {
      if (usageKeyId) await incrementApiKeyUsage(usageKeyId);
      await incrementKernelLimitUsage();
    }
    await logApiKeyRequest({
      route: "embeddings",
      path,
      method: req.method,
      status_code: response.status,
      stream: false,
      provider: "voyage",
    });
    return withCors(response);
  }

  if (req.method === "POST" && path === "/v1/chat/completions") {
    const response = await handleChatCompletions(req, usageContext);
    const isStream = (response.headers.get("content-type") ?? "").toLowerCase().includes("text/event-stream");
    if (response.ok) {
      if (usageKeyId) await incrementApiKeyUsage(usageKeyId);
      await incrementKernelLimitUsage();
    }
    await logApiKeyRequest({
      route: "chat.completions",
      path,
      method: req.method,
      status_code: response.status,
      stream: isStream,
      provider: response.headers.get("x-ubq-upstream") === "yunwu" ? "yunwu" : "chatgpt_codex",
    });
    return withCors(response);
  }

  if (req.method === "POST" && path === "/v1/responses") {
    const response = await handleResponses(req, usageContext);
    const isStream = (response.headers.get("content-type") ?? "").toLowerCase().includes("text/event-stream");
    if (response.ok) {
      if (usageKeyId) await incrementApiKeyUsage(usageKeyId);
      await incrementKernelLimitUsage();
    }
    await logApiKeyRequest({
      route: "responses",
      path,
      method: req.method,
      status_code: response.status,
      stream: isStream,
      provider: response.headers.get("x-ubq-upstream") === "yunwu" ? "yunwu" : "chatgpt_codex",
    });
    return withCors(response);
  }

  return withCors(openaiError(404, "Not found", "not_found"));
}
