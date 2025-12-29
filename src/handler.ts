import {
  handleAdminApiKeysCreate,
  handleAdminApiKeysList,
  handleAdminApiKeysRevoke,
  handleAdminCodexAuth,
} from "./admin.ts";
import { handleAgentMessagesList, handleAgentMessagesPost } from "./agent_messages.ts";
import { handleV1Auth, requireAdminAuth, requireClientAuth } from "./auth.ts";
import { handleHealth } from "./health.ts";
import { corsHeaders, openaiError, withCors } from "./http.ts";
import { handleChatCompletions, handleModels, handleResponses } from "./openai.ts";
import {
  handleAppJs,
  handleChatJs,
  handleChatPage,
  handleFavicon,
  handleFavicon32,
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

  if (req.method === "GET" && path === "/chat.js") {
    return withCors(await handleChatJs());
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
    return withCors(await handleAdminApiKeysList());
  }

  if (req.method === "POST" && path === "/admin/api-keys/revoke") {
    const authError = await requireAdminAuth(req);
    if (authError) return withCors(authError);
    return withCors(await handleAdminApiKeysRevoke(req));
  }

  if (!path.startsWith("/v1/")) {
    return withCors(openaiError(404, "Not found", "not_found"));
  }

  if (req.method === "GET" && path === "/v1/auth") {
    return withCors(await handleV1Auth(req));
  }

  if (path === "/v1/agent-messages") {
    if (req.method === "GET") return withCors(await handleAgentMessagesList(req));
    if (req.method === "POST") return withCors(await handleAgentMessagesPost(req));
    return withCors(openaiError(405, "Method not allowed", "method_not_allowed"));
  }

  const authError = await requireClientAuth(req);
  if (authError) return withCors(authError);

  if (req.method === "GET" && path === "/v1/models") {
    return withCors(handleModels());
  }

  if (req.method === "POST" && path === "/v1/chat/completions") {
    return withCors(await handleChatCompletions(req));
  }

  if (req.method === "POST" && path === "/v1/responses") {
    return withCors(await handleResponses(req));
  }

  return withCors(openaiError(404, "Not found", "not_found"));
}
