let pending: any; let reads=0;
const refreshClaudeCodeUsage=async()=>pending;
const claudeSubscriptionBlock=async()=>{reads++;return {accepted:"source",label:"\ud800"};};
export const json = (status: number, body: unknown, extraHeaders: HeadersInit = {}): Response => {
  const headers = new Headers(extraHeaders);
  // extraHeaders wins over the JSON default, matching the previous object spread.
  if (!headers.has("Content-Type")) headers.set("Content-Type", "application/json");
  return new Response(JSON.stringify(body), {
    status,
    headers,
  });
};
export const openaiError = (
  status: number,
  message: string,
  code?: string,
  options: { type?: string; param?: string | null; headers?: HeadersInit } = {}
): Response => {
  const type = (options.type ?? "invalid_request_error").trim() || "invalid_request_error";
  const error: Record<string, unknown> = {
    message,
    type,
    code,
  };
  if (Object.prototype.hasOwnProperty.call(options, "param")) {
    error.param = options.param ?? null;
  }
  return json(
    status,
    {
      error,
    },
    options.headers
  );
};
const runClaudeUsageRefresh = async (kv: Kv, account: ClaudeCodeAccountBinding, options: ClaudeUsageRefreshOptions): Promise<Response> => {
  const refreshed = await refreshClaudeCodeUsage(kv, account, options);
  if (refreshed.kind === "changed") return openaiError(409, "Claude account changed during usage refresh", "claude_code_account_changed");
  if (refreshed.kind === "busy")
    return json(200, {
      data: await claudeSubscriptionBlock(),
      refreshed: false,
      reason: "refresh_in_progress",
      message: "A usage refresh is already in progress.",
    });
  if (refreshed.kind === "fresh")
    return json(200, {
      data: await claudeSubscriptionBlock(),
      refreshed: false,
      reason: "fresh",
      observed_at_ms: refreshed.usage.attempted_at_ms,
      usage: refreshed.usage,
    });
  const { result, usage } = refreshed;
  const data = await claudeSubscriptionBlock();
  if (result.ok) {
    return json(200, {
      data,
      refreshed: true,
      source: "oauth_usage",
      observed_at_ms: result.observed_at_ms,
      usage,
    });
  }
  if (result.kind === "scope_missing" || result.kind === "unsupported_credential") {
    return json(200, {
      data,
      refreshed: false,
      reason: result.kind,
      message: result.message ?? "This credential cannot read Claude usage.",
      usage,
    });
  }
  return openaiError(502, result.message ?? "Claude usage could not be read from the upstream service", "claude_usage_unavailable", {
    headers: result.retry_after_ms === null ? undefined : { "Retry-After": String(Math.max(1, Math.ceil(result.retry_after_ms / 1_000))) },
  });
};
export {runClaudeUsageRefresh};
export const setResult=(value:any)=>{pending=value;reads=0;};
export const blockReads=()=>reads;

const runtimeGitSha=()=>"public-source-fixture";
const runtimeDeploymentId=()=>"public-source-fixture";
const corsHeaders=()=>({});
const isAdminPath = (req?: Request): boolean => {
  if (!req) return false;
  try {
    const path = new URL(req.url).pathname;
    return path === "/admin" || path === "/admin.html" || path.startsWith("/admin/");
  } catch {
    return false;
  }
};
export const withCors = (response: Response, req?: Request): Response => {
  const headers = new Headers(response.headers);
  if (isAdminPath(req)) headers.set("Cache-Control", "no-store");
  headers.set("x-uos-git-sha", runtimeGitSha());
  headers.set("x-uos-deployment-id", runtimeDeploymentId());
  if (!headers.has("x-uos-request-id")) headers.set("x-uos-request-id", crypto.randomUUID());
  for (const [key, value] of Object.entries(corsHeaders(req))) {
    if (key.toLowerCase() !== "vary") {
      headers.set(key, value);
      continue;
    }
    const varyTokens = new Map<string, string>();
    for (const token of `${headers.get("Vary") ?? ""},${value}`.split(",")) {
      const normalized = token.trim();
      if (normalized) varyTokens.set(normalized.toLowerCase(), normalized);
    }
    headers.set("Vary", varyTokens.has("*") ? "*" : [...varyTokens.values()].join(", "));
  }
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
};
