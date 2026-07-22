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

const decorateInferenceQuota = (response: Response, policy: ApiKeyPolicy | null): Response => {
  const usedPercent = apiKeyQuotaUsedPercent(policy);
  return withCodexQuotaHeaders(response, usedPercent === null ? null : { used_percent: usedPercent });
};

const logTerminalRequest = (
  input: Readonly<{
    route: string;
    status: number;
    response: Response;
    startedAtMs: number;
    keyId: string | null;
    model?: string | null;
    reasoning?: string | null;
  }>,
): void => {
  const provider = input.response.headers.get("x-ubq-upstream") || "gateway";
  console.info(
    "[ai.ubq.fi] request_terminal",
    JSON.stringify({
      route: input.route,
      status: input.status,
      provider,
      latency_ms: Math.max(0, Date.now() - input.startedAtMs),
      input_tokens: null,
      output_tokens: null,
      model: input.model ?? null,
      reasoning: input.reasoning ?? null,
      key_id: input.keyId,
      fallback_reason: provider === "yunwu" ? "primary_429" : null,
    }),
  );
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

  const authResult = await authenticateClient(req);
  if (!authResult.ok) return withCors(authResult.response);
  const requestId = crypto.randomUUID();
  const requestStartedAtMs = Date.now();
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

  if (req.method === "GET" && path === "/v1/models") {
    return withCors(await handleModels(req));
  }

  if (req.method === "POST" && path === "/uos/embeddings") {
    const response = await handleUosEmbeddings(req, usageContext);
    if (response.ok && response.headers.get("x-uos-idempotency-replayed") !== "true") {
      if (usagePolicy) await incrementApiKeyUsage(usagePolicy);
      await incrementKernelLimitUsage();
    }
    logTerminalRequest({
      route: "embeddings",
      status: response.status,
      response,
      startedAtMs: requestStartedAtMs,
      keyId: usageKeyId,
    });
    return withCors(response);
  }

  if (req.method === "POST" && path === "/uos/embedding-jobs") {
    const response = await handleEmbeddingsJobCreate(req, authResult.token, usageContext);
    if (response.ok) {
      if (usagePolicy) await incrementApiKeyUsage(usagePolicy);
      await incrementKernelLimitUsage();
    }
    logTerminalRequest({
      route: "embeddings.jobs.create",
      status: response.status,
      response,
      startedAtMs: requestStartedAtMs,
      keyId: usageKeyId,
    });
    return withCors(response);
  }

  if (req.method === "GET" && path.startsWith("/uos/embedding-jobs/")) {
    const jobId = path.slice("/uos/embedding-jobs/".length).trim();
    if (!jobId) return withCors(openaiError(404, "Not found", "not_found"));
    const response = await handleEmbeddingsJobGet(req, authResult.token, jobId, usageContext);
    logTerminalRequest({
      route: "embeddings.jobs.get",
      status: response.status,
      response,
      startedAtMs: requestStartedAtMs,
      keyId: usageKeyId,
    });
    return withCors(response);
  }

  if (req.method === "POST" && path === "/v1/embeddings") {
    const response = await handleEmbeddings(req, usageContext);
    if (response.ok) {
      if (usagePolicy) await incrementApiKeyUsage(usagePolicy);
      await incrementKernelLimitUsage();
    }
    logTerminalRequest({
      route: "embeddings",
      status: response.status,
      response,
      startedAtMs: requestStartedAtMs,
      keyId: usageKeyId,
    });
    return withCors(response);
  }

  if (req.method === "POST" && path === "/v1/chat/completions") {
    const response = await handleChatCompletions(req, usageContext);
    if (response.ok) {
      if (usagePolicy) await incrementApiKeyUsage(usagePolicy);
      await incrementKernelLimitUsage();
    }
    logTerminalRequest({
      route: "chat.completions",
      status: response.status,
      response,
      startedAtMs: requestStartedAtMs,
      keyId: usageKeyId,
    });
    return withCors(decorateInferenceQuota(response, usagePolicy));
  }

  if (req.method === "POST" && path === "/v1/responses") {
    const response = await handleResponses(req, usageContext);
    if (response.ok) {
      if (usagePolicy) await incrementApiKeyUsage(usagePolicy);
      await incrementKernelLimitUsage();
    }
    logTerminalRequest({
      route: "responses",
      status: response.status,
      response,
      startedAtMs: requestStartedAtMs,
      keyId: usageKeyId,
    });
    return withCors(decorateInferenceQuota(response, usagePolicy));
  }

  return withCors(openaiError(404, "Not found", "not_found"));
}
