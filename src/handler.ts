import {
  handleAdminApiKeysCreate,
  handleAdminApiKeysDelete,
  handleAdminApiKeysList,
  handleAdminApiKeysUnrevoke,
  handleAdminApiKeysUpdate,
  handleAdminApiKeysRevoke,
  handleAdminCodexAuth,
  handleAdminKernelUsageDelete,
  handleAdminKernelUsageGet,
  handleAdminKernelUsageSet,
  handleAdminKernelPubKeysCreate,
  handleAdminKernelPubKeysDelete,
  handleAdminKernelPubKeysList,
  handleAdminReasoningLevel,
} from "./admin.ts";
import { handleAgentMessagesList, handleAgentMessagesPost } from "./agent_messages.ts";
import { authenticateClient, handleV1Auth, incrementApiKeyUsage, requireAdminAuth } from "./auth.ts";
import { handleHealth, handleHealthAuth, handleHealthUpstream } from "./health.ts";
import { corsHeaders, openaiError, withCors } from "./http.ts";
import { incrementKernelOrgUsageLimit, incrementKernelUsageLimit } from "./kernel_usage.ts";
import { handleChatCompletions, handleModels, handleResponses } from "./openai.ts";
import {
  handleAdminJs,
  handleAdminPage,
  handleAppJs,
  handleChatJs,
  handleChatPage,
  handleFavicon,
  handleFavicon32,
  handleNetworkJs,
  handleRoot,
  handleStyleCss,
} from "./static.ts";

export default async function handler(req: Request): Promise<Response> {
  if (req.method === "OPTIONS") {
    return withCors(new Response(null, { status: 204, headers: corsHeaders() }));
  }

  const url = new URL(req.url);
  const path = url.pathname;

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

  if (req.method === "GET" && path === "/app.js") {
    return withCors(await handleAppJs());
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

  if ((req.method === "GET" || req.method === "POST") && path === "/admin/reasoning-level") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminReasoningLevel(req));
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
  const kernelRepo = authResult.method.kind === "github_token"
    ? { owner: authResult.method.owner, repo: authResult.method.repo }
    : null;
  const kernelOrg = kernelRepo ? { owner: kernelRepo.owner } : null;
  const usageContext = { keyId: usageKeyId, kernelRepo, kernelOrg };

  if (req.method === "GET" && path === "/v1/models") {
    return withCors(await handleModels());
  }

  if (req.method === "POST" && path === "/v1/chat/completions") {
    const response = await handleChatCompletions(req, usageContext);
    if (response.ok) {
      if (usageKeyId) await incrementApiKeyUsage(usageKeyId);
      if (kernelRepo) await incrementKernelUsageLimit(kernelRepo.owner, kernelRepo.repo);
      if (kernelOrg) await incrementKernelOrgUsageLimit(kernelOrg.owner);
    }
    return withCors(response);
  }

  if (req.method === "POST" && path === "/v1/responses") {
    const response = await handleResponses(req, usageContext);
    if (response.ok) {
      if (usageKeyId) await incrementApiKeyUsage(usageKeyId);
      if (kernelRepo) await incrementKernelUsageLimit(kernelRepo.owner, kernelRepo.repo);
      if (kernelOrg) await incrementKernelOrgUsageLimit(kernelOrg.owner);
    }
    return withCors(response);
  }

  return withCors(openaiError(404, "Not found", "not_found"));
}
