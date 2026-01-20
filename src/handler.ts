import {
  handleAdminApiKeysCreate,
  handleAdminApiKeysDelete,
  handleAdminApiKeysList,
  handleAdminApiKeysUnrevoke,
  handleAdminApiKeysUpdate,
  handleAdminApiKeysRevoke,
  handleAdminCodexAuth,
  handleAdminCodexModelsGet,
  handleAdminCodexModelsSet,
  handleAdminDefaults,
  handleAdminKernelUsageDelete,
  handleAdminKernelUsageGet,
  handleAdminKernelUsageSet,
  handleAdminKernelPubKeysCreate,
  handleAdminKernelPubKeysDelete,
  handleAdminKernelPubKeysList,
} from "./admin.ts";
import { handleAgentMessagesList, handleAgentMessagesPost } from "./agent_messages.ts";
import { authenticateClient, getKernelAttestationContext, handleV1Auth, incrementApiKeyUsage, requireAdminAuth } from "./auth.ts";
import { handleHealth, handleHealthAuth, handleHealthUpstream } from "./health.ts";
import { corsHeaders, openaiError, withCors } from "./http.ts";
import { getKernelUsageLimitSnapshot, incrementKernelOrgUsageLimit, incrementKernelUsageLimit } from "./kernel_usage.ts";
import { handleChatCompletions, handleModels, handleResponses } from "./openai.ts";
import {
  handleAdminCss,
  handleAdminJs,
  handleAdminPage,
  handleAppJs,
  handleChatCss,
  handleChatJs,
  handleChatPage,
  handleHomeCss,
  handleCompanyLogo,
  handleFavicon,
  handleFavicon32,
  handleNetworkJs,
  handleRoot,
  handleStyleCss,
} from "./static.ts";

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

  if (req.method === "GET" && (path === "/chat" || path === "/chat.html")) {
    return withCors(await handleChatPage());
  }

  if (req.method === "GET" && (path === "/admin" || path === "/admin.html")) {
    return withCors(await handleAdminPage());
  }

  if (req.method === "GET" && path === "/chat.js") {
    return withCors(await handleChatJs());
  }

  if (req.method === "GET" && path === "/admin.js") {
    return withCors(await handleAdminJs());
  }

  if (req.method === "GET" && path === "/network.js") {
    return withCors(await handleNetworkJs());
  }

  if (req.method === "GET" && path === "/style.css") {
    return withCors(await handleStyleCss());
  }

  if (req.method === "GET" && path === "/chat.css") {
    return withCors(await handleChatCss());
  }

  if (req.method === "GET" && path === "/home.css") {
    return withCors(await handleHomeCss());
  }

  if (req.method === "GET" && path === "/admin.css") {
    return withCors(await handleAdminCss());
  }

  if (req.method === "GET" && path === "/app.js") {
    return withCors(await handleAppJs());
  }

  if (req.method === "GET" && path === "/company-logo.svg") {
    return withCors(await handleCompanyLogo());
  }

  if (req.method === "GET" && path === "/favicon-32.png") {
    return withCors(await handleFavicon32());
  }

  if (req.method === "GET" && path === "/favicon.png") {
    return withCors(await handleFavicon());
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

  if (!path.startsWith("/v1/")) {
    return withCors(openaiError(404, "Not found", "not_found"));
  }

  if (req.method === "GET" && path === "/v1/auth") {
    return withCors(await handleV1Auth(req));
  }

  if (path === "/v1/agent-bus") {
    if (req.method === "GET") return withCors(await handleAgentMessagesList(req));
    if (req.method === "POST") return withCors(await handleAgentMessagesPost(req));
    return withCors(openaiError(405, "Method not allowed", "method_not_allowed"));
  }

  const authResult = await authenticateClient(req);
  if (!authResult.ok) return withCors(authResult.response);
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
  const usageContext = { keyId: usageKeyId, kernelRepo, kernelOrg };
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
    return withCors(await handleModels());
  }

  if (req.method === "POST" && path === "/v1/chat/completions") {
    const response = await handleChatCompletions(req, usageContext);
    if (response.ok) {
      if (usageKeyId) await incrementApiKeyUsage(usageKeyId);
      await incrementKernelLimitUsage();
    }
    return withCors(response);
  }

  if (req.method === "POST" && path === "/v1/responses") {
    const response = await handleResponses(req, usageContext);
    if (response.ok) {
      if (usageKeyId) await incrementApiKeyUsage(usageKeyId);
      await incrementKernelLimitUsage();
    }
    return withCors(response);
  }

  return withCors(openaiError(404, "Not found", "not_found"));
}
