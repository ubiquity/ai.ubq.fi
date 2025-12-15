/// <reference lib="deno.ns" />

type Config = Readonly<{
  isDeploy: boolean;
  allowOrigin: string;
  authTokens: ReadonlySet<string>;
  adminTokens: ReadonlySet<string>;
  codexBaseUrl: string;
  codexAuthJsonB64: string;
  codexInstructionsB64: string | null;
}>;

type CodexAuthState = Readonly<{
  access_token: string;
  refresh_token: string;
  account_id: string;
  updated_at_ms: number;
}>;

type ApiKeyRecord = Readonly<{
  id: string;
  name: string;
  prefix: string;
  hash: string;
  created_at_ms: number;
  revoked_at_ms: number | null;
}>;

type ApiKeyHashRecord = Readonly<{
  id: string;
  revoked_at_ms: number | null;
}>;

type ChatCompletionRequest = Readonly<{
  model?: unknown;
  messages?: unknown;
  reasoning_effort?: unknown;
  stream?: unknown;
}>;

type ResponsesRequest = Readonly<{
  model?: unknown;
  input?: unknown;
  reasoning?: unknown;
  stream?: unknown;
}>;

type MessageContentItem = Readonly<
  | { type: "input_text"; text: string }
  | { type: "output_text"; text: string }
  | { type: "input_image"; image_url: string }
>;

type ResponseMessageItem = Readonly<{
  type: "message";
  role: "user" | "assistant" | "developer";
  content: MessageContentItem[];
}>;

type DenoWithKv = typeof Deno & {
  openKv?: () => Promise<Deno.Kv>;
};

const CODEX_REFRESH_CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann";
const CODEX_REFRESH_TOKEN_URL = "https://auth.openai.com/oauth/token";
const CODEX_ORIGINATOR = "codex_cli_rs";
const CODEX_USER_AGENT = "codex_cli_rs/0.99.0 (ai.ubq.fi)";
const CODEX_KV_KEY = ["ubq_ai", "codex_auth"] as const;
const API_KEY_ID_PREFIX = ["ubq_ai", "api_keys", "id"] as const;
const API_KEY_HASH_PREFIX = ["ubq_ai", "api_keys", "hash"] as const;
const CODEX_INSTRUCTIONS_URL = new URL("./codex_instructions.md", import.meta.url);
const INDEX_HTML_URL = new URL("./index.html", import.meta.url);
const STYLE_CSS_URL = new URL("./style.css", import.meta.url);
const APP_JS_URL = new URL("./app.js", import.meta.url);

const parseTokens = (raw: string | undefined | null): Set<string> => {
  if (!raw) return new Set();
  return new Set(
    raw
      .split(/[\n,]/g)
      .map((token) => token.trim())
      .filter(Boolean),
  );
};

const loadConfig = (): Config => {
  const isDeploy = Boolean(Deno.env.get("DENO_DEPLOYMENT_ID") ?? Deno.env.get("DENO_REGION"));
  const authTokens = parseTokens(Deno.env.get("UBIQUITY_AI_USER_TOKEN"));
  const adminTokens = parseTokens(Deno.env.get("UBIQUITY_AI_ADMIN_TOKEN"));
  const allowOrigin = (Deno.env.get("CORS_ALLOW_ORIGIN") ?? "*").trim() || "*";

  const codexBaseUrl = (Deno.env.get("CODEX_BASE_URL") ?? "https://chatgpt.com/backend-api/codex")
    .trim()
    .replace(/\/$/, "");
  const codexAuthJsonB64 = (Deno.env.get("CODEX_AUTH_JSON_B64") ?? "").trim();
  const codexInstructionsB64 = (Deno.env.get("CODEX_INSTRUCTIONS_B64") ?? "").trim() || null;

  return {
    isDeploy,
    codexBaseUrl,
    codexAuthJsonB64,
    codexInstructionsB64,
    allowOrigin,
    authTokens,
    adminTokens,
  };
};

const config = loadConfig();

const kvPromise: Promise<Deno.Kv | null> = (async () => {
  const openKv = (Deno as DenoWithKv).openKv;
  if (typeof openKv !== "function") return null;
  try {
    return await openKv();
  } catch (error) {
    console.error("[ai.ubq.fi] Failed to open Deno KV (token refresh will be in-memory only):", error);
    return null;
  }
})();

const decodeBase64ToString = (raw: string): string => {
  const cleaned = raw.trim().replace(/\s+/g, "");
  const bytes = Uint8Array.from(atob(cleaned), (ch) => ch.charCodeAt(0));
  return new TextDecoder().decode(bytes);
};

const encodeBase64Url = (bytes: Uint8Array): string => {
  let binary = "";
  for (const b of bytes) binary += String.fromCharCode(b);
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
};

const sha256Base64Url = async (value: string): Promise<string> => {
  const bytes = new TextEncoder().encode(value);
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return encodeBase64Url(new Uint8Array(digest));
};

const encodeHex = (bytes: Uint8Array): string => {
  let out = "";
  for (const b of bytes) out += b.toString(16).padStart(2, "0");
  return out;
};

const sha256Hex = async (value: string): Promise<string> => {
  const bytes = new TextEncoder().encode(value);
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return encodeHex(new Uint8Array(digest));
};

const apiKeyIdKey = (id: string) => [...API_KEY_ID_PREFIX, id] as const;
const apiKeyHashKey = (hash: string) => [...API_KEY_HASH_PREFIX, hash] as const;

const generateApiKeyToken = (): string => {
  const bytes = new Uint8Array(32);
  crypto.getRandomValues(bytes);
  return encodeHex(bytes);
};

const codexInstructionsPromise: Promise<string> = (async () => {
  if (config.codexInstructionsB64) return decodeBase64ToString(config.codexInstructionsB64);
  return await Deno.readTextFile(CODEX_INSTRUCTIONS_URL);
})();

const indexHtmlPromise: Promise<string | null> = (async () => {
  try {
    return await Deno.readTextFile(INDEX_HTML_URL);
  } catch (error) {
    console.error("[ai.ubq.fi] Failed to load index.html:", error);
    return null;
  }
})();

const styleCssPromise: Promise<string | null> = (async () => {
  try {
    return await Deno.readTextFile(STYLE_CSS_URL);
  } catch (error) {
    console.error("[ai.ubq.fi] Failed to load style.css:", error);
    return null;
  }
})();

const appJsPromise: Promise<string | null> = (async () => {
  try {
    return await Deno.readTextFile(APP_JS_URL);
  } catch (error) {
    console.error("[ai.ubq.fi] Failed to load app.js:", error);
    return null;
  }
})();

const isRecord = (value: unknown): value is Record<string, unknown> => typeof value === "object" && value !== null;

const getString = (value: unknown): string | null => (typeof value === "string" ? value : null);

type ReasoningEffort = "none" | "minimal" | "low" | "medium" | "high" | "xhigh";

const REASONING_EFFORTS: ReadonlySet<ReasoningEffort> = new Set(["none", "minimal", "low", "medium", "high", "xhigh"]);

const DEFAULT_MODEL = "gpt-5-chat-latest";
const DEFAULT_REASONING_EFFORT: ReasoningEffort = "xhigh";

const looksLikeReasoningModel = (model: string): boolean => {
  const trimmed = model.trim().toLowerCase();
  return trimmed.startsWith("gpt-5") || trimmed.startsWith("o");
};

const parseReasoningEffortField = (
  value: unknown,
  fieldName: string,
): { ok: true; value: ReasoningEffort | null | undefined } | { ok: false; message: string } => {
  if (value === undefined) return { ok: true, value: undefined };
  if (value === null) return { ok: true, value: null };
  if (typeof value !== "string") {
    return { ok: false, message: `${fieldName} must be a string or null` };
  }
  const normalized = value.trim().toLowerCase();
  if (!normalized) return { ok: false, message: `${fieldName} must be a non-empty string or null` };
  if (!REASONING_EFFORTS.has(normalized as ReasoningEffort)) {
    return { ok: false, message: `${fieldName} must be one of: none, minimal, low, medium, high, xhigh` };
  }
  return { ok: true, value: normalized as ReasoningEffort };
};

const parseReasoningParam = (
  value: unknown,
): { ok: true; value: Record<string, unknown> | null | undefined } | { ok: false; message: string } => {
  if (value === undefined) return { ok: true, value: undefined };
  if (value === null) return { ok: true, value: null };
  if (!isRecord(value)) return { ok: false, message: "reasoning must be an object or null" };
  if ("effort" in value) {
    const effort = parseReasoningEffortField(value.effort, "reasoning.effort");
    if (!effort.ok) return effort;
  }
  if ("summary" in value) {
    const summary = value.summary;
    if (summary !== undefined && summary !== null && typeof summary !== "string") {
      return { ok: false, message: "reasoning.summary must be a string or null" };
    }
  }
  if ("generate_summary" in value) {
    const generateSummary = value.generate_summary;
    if (generateSummary !== undefined && generateSummary !== null && typeof generateSummary !== "string") {
      return { ok: false, message: "reasoning.generate_summary must be a string or null" };
    }
  }

  return { ok: true, value };
};

const corsHeaders = (): HeadersInit => ({
  "Access-Control-Allow-Origin": config.allowOrigin,
  "Access-Control-Allow-Methods": "GET,POST,PUT,PATCH,DELETE,OPTIONS",
  "Access-Control-Allow-Headers": "Authorization,Content-Type,OpenAI-Beta,OpenAI-Organization,OpenAI-Project",
  "Access-Control-Max-Age": "86400",
});

const withCors = (response: Response): Response => {
  const headers = new Headers(response.headers);
  for (const [key, value] of Object.entries(corsHeaders())) {
    headers.set(key, value);
  }
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
};

const json = (status: number, body: unknown, extraHeaders: HeadersInit = {}): Response =>
  new Response(JSON.stringify(body), {
    status,
    headers: {
      "Content-Type": "application/json",
      ...extraHeaders,
    },
  });

const openaiError = (status: number, message: string, code?: string): Response =>
  json(status, {
    error: {
      message,
      type: "invalid_request_error",
      code,
    },
  });

const getBearerToken = (req: Request): string | null => {
  const value = req.headers.get("Authorization");
  if (!value) return null;
  const match = value.match(/^Bearer\s+(.+)$/i);
  return match?.[1]?.trim() || null;
};

const looksLikeUbqAiClientToken = (token: string): boolean => token.trim().startsWith("ubq_ai_");

const classifyToken = (token: string): string => {
  const trimmed = token.trim();
  if (!trimmed) return "unset";
  if (trimmed.startsWith("ddw_")) return "deno_deploy_like(ddw_)";
  if (trimmed.startsWith("ubq_ai_")) return "ubq_ai_prefix";
  if (/^[0-9a-fA-F]{64}$/.test(trimmed)) return "hex64";
  if (trimmed.includes("_")) return "has_underscore";
  return "other";
};

type ClientAuthMethod =
  | { kind: "disabled" }
  | { kind: "auth_tokens_allowlist" }
  | { kind: "kv_api_key"; key_id: string }
  | { kind: "admin_allowlist" }
  | { kind: "deno_deploy_token" };

type AuthenticateClientResult =
  | { ok: true; token: string | null; method: ClientAuthMethod }
  | { ok: false; response: Response };

type CheckAdminTokenResult =
  | { ok: true; kind: "admin_allowlist" | "deno_deploy_token" }
  | { ok: false; response: Response | null };

const authenticateClient = async (req: Request): Promise<AuthenticateClientResult> => {
  const kv = await kvPromise;
  const localAuthDisabled = !config.isDeploy && config.authTokens.size === 0 && !kv;
  const token = getBearerToken(req);
  if (localAuthDisabled) return { ok: true, token, method: { kind: "disabled" } };

  if (!token) return { ok: false, response: openaiError(401, "Unauthorized", "invalid_api_key") };

  if (config.authTokens.has(token)) return { ok: true, token, method: { kind: "auth_tokens_allowlist" } };

  if (kv) {
    const hash = await sha256Base64Url(token);
    const entry = await kv.get<ApiKeyHashRecord>(apiKeyHashKey(hash));
    if (entry.value && entry.value.revoked_at_ms == null) {
      return { ok: true, token, method: { kind: "kv_api_key", key_id: entry.value.id } };
    }
  }

  const adminResult = await checkAdminToken(token);
  if (adminResult.ok) return { ok: true, token, method: { kind: adminResult.kind } };
  if (adminResult.response) return { ok: false, response: adminResult.response };

  if (kv) return { ok: false, response: openaiError(401, "Unauthorized", "invalid_api_key") };

  if (config.isDeploy && config.authTokens.size === 0) {
    return {
      ok: false,
      response: openaiError(500, "Server misconfigured: set UBIQUITY_AI_USER_TOKEN or enable Deno KV", "server_error"),
    };
  }

  return { ok: false, response: openaiError(401, "Unauthorized", "invalid_api_key") };
};

const requireClientAuth = async (req: Request): Promise<Response | null> => {
  const result = await authenticateClient(req);
  return result.ok ? null : result.response;
};

const DENO_API_BASE_URL = "https://api.deno.com/v1";
const DEPLOY_TOKEN_ADMIN_CACHE_TTL_MS = 10 * 60_000;
const deployTokenAdminCache = new Map<string, number>();

const looksLikeDenoDeployToken = (token: string): boolean => {
  const trimmed = token.trim();
  if (trimmed.length < 20) return false;
  if (trimmed.length > 500) return false;
  if (/\s/.test(trimmed)) return false;
  if (looksLikeUbqAiClientToken(trimmed)) return false;
  if (!trimmed.includes("_")) return false;
  return true;
};

const verifyDenoDeployTokenCached = async (token: string): Promise<
  { ok: true } | { ok: false; response: Response | null }
> => {
  let keyHash: string | null = null;
  try {
    keyHash = await sha256Base64Url(token);
    const cachedUntil = deployTokenAdminCache.get(keyHash) ?? 0;
    if (cachedUntil > Date.now()) return { ok: true };
  } catch {
    // ignore and try network verification
  }

  try {
    const ok = await verifyDenoDeployTokenForThisDeployment(token);
    if (!ok) return { ok: false, response: null };
    if (keyHash) deployTokenAdminCache.set(keyHash, Date.now() + DEPLOY_TOKEN_ADMIN_CACHE_TTL_MS);
    return { ok: true };
  } catch (error) {
    console.error("[ai.ubq.fi] Failed to verify Deno Deploy token:", error);
    return { ok: false, response: openaiError(502, "Failed to verify admin token", "bad_gateway") };
  }
};

const checkAdminToken = async (token: string): Promise<CheckAdminTokenResult> => {
  if (config.adminTokens.size > 0) {
    return config.adminTokens.has(token) ? { ok: true, kind: "admin_allowlist" } : { ok: false, response: null };
  }

  if (!looksLikeDenoDeployToken(token)) return { ok: false, response: null };

  const verified = await verifyDenoDeployTokenCached(token);
  if (verified.ok) return { ok: true, kind: "deno_deploy_token" };
  return verified;
};

const isAdminToken = async (token: string): Promise<{ ok: true } | { ok: false; response: Response | null }> => {
  const result = await checkAdminToken(token);
  return result.ok ? { ok: true } : result;
};

const verifyDenoDeployTokenForThisDeployment = async (token: string): Promise<boolean> => {
  if (!config.isDeploy) return false;
  const deploymentId = (Deno.env.get("DENO_DEPLOYMENT_ID") ?? "").trim();
  if (!deploymentId) return false;

  const url = `${DENO_API_BASE_URL}/deployments/${deploymentId}`;
  const res = await fetch(url, {
    method: "GET",
    headers: {
      "Authorization": `Bearer ${token}`,
      "Accept": "application/json",
    },
    redirect: "manual",
  });

  try {
    await res.body?.cancel();
  } catch {
    // ignore
  }

  return res.ok;
};

const requireAdminAuth = async (req: Request): Promise<Response | null> => {
  const token = getBearerToken(req);
  if (!token) return openaiError(401, "Unauthorized", "invalid_api_key");

  if (config.adminTokens.size === 0 && !looksLikeDenoDeployToken(token)) {
    return openaiError(404, "Not found", "not_found");
  }

  const result = await isAdminToken(token);
  if (result.ok) return null;
  return result.response ?? openaiError(401, "Unauthorized", "invalid_api_key");
};

const parseCodexAuthFromAuthJson = (value: unknown): Omit<CodexAuthState, "updated_at_ms"> | null => {
  if (!isRecord(value)) return null;
  const tokens = isRecord(value.tokens) ? value.tokens : null;
  if (!tokens) return null;
  const access_token = getString(tokens.access_token);
  const refresh_token = getString(tokens.refresh_token);
  const account_id = getString(tokens.account_id);
  if (!access_token || !refresh_token || !account_id) return null;
  return { access_token, refresh_token, account_id };
};

const getJwtExpMs = (token: string): number | null => {
  const parts = token.split(".");
  if (parts.length < 2) return null;
  const payload = parts[1].replace(/-/g, "+").replace(/_/g, "/");
  const padded = payload + "=".repeat((4 - (payload.length % 4)) % 4);
  try {
    const decoded = decodeBase64ToString(padded);
    const parsed = JSON.parse(decoded);
    const exp = typeof parsed?.exp === "number" ? parsed.exp : null;
    return exp ? exp * 1000 : null;
  } catch {
    return null;
  }
};

const needsRefresh = (auth: CodexAuthState): boolean => {
  const expMs = getJwtExpMs(auth.access_token);
  const now = Date.now();
  if (expMs) return expMs - now < 2 * 60_000;
  return now - auth.updated_at_ms > 7 * 60_000;
};

let cachedAuth: CodexAuthState | null = null;
let refreshInFlight: Promise<CodexAuthState> | null = null;

const loadAuthSeedFromEnv = (): CodexAuthState => {
  if (!config.codexAuthJsonB64) {
    throw new Error("CODEX_AUTH_JSON_B64 is missing");
  }
  const decoded = decodeBase64ToString(config.codexAuthJsonB64);
  const parsed = JSON.parse(decoded) as unknown;
  const tokenData = parseCodexAuthFromAuthJson(parsed);
  if (!tokenData) {
    throw new Error("CODEX_AUTH_JSON_B64 does not look like a Codex auth.json");
  }
  return { ...tokenData, updated_at_ms: Date.now() };
};

const getAuthEntry = async (): Promise<{
  kv: Deno.Kv | null;
  entry: Deno.KvEntryMaybe<CodexAuthState> | null;
  auth: CodexAuthState;
}> => {
  const kv = await kvPromise;
  if (!kv) {
    const auth = cachedAuth ?? loadAuthSeedFromEnv();
    cachedAuth = auth;
    return { kv: null, entry: null, auth };
  }

  const entry = await kv.get<CodexAuthState>(CODEX_KV_KEY);
  if (entry.value) {
    cachedAuth = entry.value;
    return { kv, entry, auth: entry.value };
  }

  const seed = loadAuthSeedFromEnv();
  await kv.set(CODEX_KV_KEY, seed);
  cachedAuth = seed;
  return { kv, entry: null, auth: seed };
};

const refreshAuth = async (
  current: { kv: Deno.Kv | null; entry: Deno.KvEntryMaybe<CodexAuthState> | null; auth: CodexAuthState },
): Promise<CodexAuthState> => {
  const response = await fetch(CODEX_REFRESH_TOKEN_URL, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "Accept": "application/json",
    },
    body: JSON.stringify({
      client_id: CODEX_REFRESH_CLIENT_ID,
      grant_type: "refresh_token",
      refresh_token: current.auth.refresh_token,
      scope: "openid profile email",
    }),
  });

  if (!response.ok) {
    const text = await response.text().catch(() => "");
    throw new Error(`Token refresh failed (${response.status}): ${text || response.statusText}`);
  }

  const parsed = (await response.json().catch(() => null)) as null | Record<string, unknown>;
  const access_token = parsed && getString(parsed.access_token);
  const refresh_token = parsed && getString(parsed.refresh_token);

  const next: CodexAuthState = {
    access_token: access_token ?? current.auth.access_token,
    refresh_token: refresh_token ?? current.auth.refresh_token,
    account_id: current.auth.account_id,
    updated_at_ms: Date.now(),
  };

  cachedAuth = next;

  if (current.kv) {
    if (current.entry) {
      const commit = await current.kv.atomic().check(current.entry).set(CODEX_KV_KEY, next).commit();
      if (!commit.ok) {
        const latest = await current.kv.get<CodexAuthState>(CODEX_KV_KEY);
        if (latest.value) {
          cachedAuth = latest.value;
          return latest.value;
        }
      }
    } else {
      await current.kv.set(CODEX_KV_KEY, next);
    }
  }

  return next;
};

const refreshAuthStateless = async (auth: CodexAuthState): Promise<CodexAuthState> => {
  const response = await fetch(CODEX_REFRESH_TOKEN_URL, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "Accept": "application/json",
    },
    body: JSON.stringify({
      client_id: CODEX_REFRESH_CLIENT_ID,
      grant_type: "refresh_token",
      refresh_token: auth.refresh_token,
      scope: "openid profile email",
    }),
  });

  if (!response.ok) {
    const text = await response.text().catch(() => "");
    throw new Error(`Token refresh failed (${response.status}): ${text || response.statusText}`);
  }

  const parsed = (await response.json().catch(() => null)) as null | Record<string, unknown>;
  const access_token = parsed && getString(parsed.access_token);
  const refresh_token = parsed && getString(parsed.refresh_token);
  if (!access_token) {
    throw new Error("Token refresh response missing access_token");
  }

  return {
    access_token,
    refresh_token: refresh_token ?? auth.refresh_token,
    account_id: auth.account_id,
    updated_at_ms: Date.now(),
  };
};

const getValidAuth = async (): Promise<CodexAuthState> => {
  const current = await getAuthEntry();
  if (!needsRefresh(current.auth)) return current.auth;

  if (refreshInFlight) return await refreshInFlight;
  refreshInFlight = (async () => {
    const refreshed = await refreshAuth(current);
    return refreshed;
  })().finally(() => {
    refreshInFlight = null;
  });
  return await refreshInFlight;
};

const fetchCodexResponsesWithAuth = async (auth: CodexAuthState, body: unknown): Promise<Response> => {
  const url = `${config.codexBaseUrl}/responses`;

  const headers = new Headers();
  headers.set("Authorization", `Bearer ${auth.access_token}`);
  headers.set("ChatGPT-Account-ID", auth.account_id);
  headers.set("originator", CODEX_ORIGINATOR);
  headers.set("user-agent", CODEX_USER_AGENT);
  headers.set("Content-Type", "application/json");
  headers.set("Accept", "text/event-stream");
  headers.set("conversation_id", crypto.randomUUID());

  return await fetch(url, {
    method: "POST",
    headers,
    body: JSON.stringify(body),
    redirect: "manual",
  });
};

const fetchCodexResponses = async (body: unknown): Promise<Response> => {
  const auth = await getValidAuth();
  const url = `${config.codexBaseUrl}/responses`;

  const headers = new Headers();
  headers.set("Authorization", `Bearer ${auth.access_token}`);
  headers.set("ChatGPT-Account-ID", auth.account_id);
  headers.set("originator", CODEX_ORIGINATOR);
  headers.set("user-agent", CODEX_USER_AGENT);
  headers.set("Content-Type", "application/json");
  headers.set("Accept", "text/event-stream");
  headers.set("conversation_id", crypto.randomUUID());

  const res = await fetch(url, {
    method: "POST",
    headers,
    body: JSON.stringify(body),
    redirect: "manual",
  });

  if (res.status !== 401) return res;

  try {
    await refreshAuth(await getAuthEntry());
  } catch {
    return res;
  }

  const auth2 = await getValidAuth();
  headers.set("Authorization", `Bearer ${auth2.access_token}`);
  headers.set("ChatGPT-Account-ID", auth2.account_id);
  return await fetch(url, {
    method: "POST",
    headers,
    body: JSON.stringify(body),
    redirect: "manual",
  });
};

const validateCodexAuthJson = async (
  auth: CodexAuthState,
): Promise<
  { ok: true; auth: CodexAuthState; refreshed: boolean; status: number; contentType: string | null } | {
    ok: false;
    status: number;
    body: string;
  }
> => {
  const model = "gpt-5.1-codex-mini";
  const input: ResponseMessageItem[] = [
    {
      type: "message",
      role: "user",
      content: [{ type: "input_text", text: "ping" }],
    },
  ];
  const body = await buildCodexRequest(model, input);

  let refreshed = false;
  let res = await fetchCodexResponsesWithAuth(auth, body);
  if (res.status === 401) {
    try {
      const next = await refreshAuthStateless(auth);
      refreshed = true;
      res = await fetchCodexResponsesWithAuth(next, body);
      auth = next;
    } catch {
      // ignore and return the original 401 response
    }
  }

  const contentType = res.headers.get("Content-Type");
  if (res.ok) {
    try {
      await res.body?.cancel();
    } catch {
      // ignore
    }
    return { ok: true, auth, refreshed, status: res.status, contentType };
  }

  const text = await res.text().catch(() => "");
  const bodySnippet = (text || res.statusText).slice(0, 8_000);
  return { ok: false, status: res.status, body: bodySnippet };
};

const readJsonBody = async (req: Request): Promise<unknown> => {
  try {
    return await req.json();
  } catch {
    return null;
  }
};

const extractTextParts = (content: unknown): string => {
  if (typeof content === "string") return content;
  if (!Array.isArray(content)) return "";
  const parts: string[] = [];
  for (const item of content) {
    if (!isRecord(item)) continue;
    const type = getString(item.type);
    if (type === "text") {
      const text = getString(item.text);
      if (text) parts.push(text);
    } else if (type === "input_text") {
      const text = getString(item.text);
      if (text) parts.push(text);
    } else if (type === "output_text") {
      const text = getString(item.text);
      if (text) parts.push(text);
    }
  }
  return parts.join("");
};

const chatRoleToCodexRole = (role: string): ResponseMessageItem["role"] | null => {
  if (role === "system") return "developer";
  if (role === "user") return "user";
  if (role === "assistant") return "assistant";
  if (role === "developer") return "developer";
  if (role === "tool") return "developer";
  return null;
};

const normalizeModelForCodex = (model: string): string => {
  const trimmed = model.trim();
  if (!trimmed) return "gpt-5.1-codex-mini";
  if (trimmed === "gpt-5.2-chat-latest") return "gpt-5.2";
  if (trimmed === "gpt-5.1-chat-latest") return "gpt-5.1";
  if (trimmed === "gpt-5-chat-latest") return "gpt-5.2";
  return trimmed;
};

const toResponseMessageItem = (message: unknown): ResponseMessageItem | null => {
  if (!isRecord(message)) return null;
  const roleRaw = getString(message.role);
  if (!roleRaw) return null;
  const role = chatRoleToCodexRole(roleRaw);
  if (!role) return null;
  const text = extractTextParts(message.content);
  const content: MessageContentItem[] = role === "assistant"
    ? [{ type: "output_text", text }]
    : [{ type: "input_text", text }];
  return { type: "message", role, content };
};

const buildCodexRequest = async (
  model: string,
  input: ResponseMessageItem[],
  options: Readonly<{ reasoning?: Record<string, unknown> | null }> = {},
): Promise<Record<string, unknown>> => {
  const body: Record<string, unknown> = {
    model,
    instructions: await codexInstructionsPromise,
    input,
    tools: [],
    tool_choice: "auto",
    parallel_tool_calls: false,
    store: false,
    stream: true,
    include: [],
  };

  if (options.reasoning !== undefined) body.reasoning = options.reasoning;

  return body;
};

const parseSseEvents = async function* (stream: ReadableStream<Uint8Array>): AsyncGenerator<unknown> {
  const reader = stream.getReader();
  const decoder = new TextDecoder();
  let buffer = "";
  try {
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      buffer += decoder.decode(value, { stream: true });
      const parts = buffer.split("\n\n");
      buffer = parts.pop() ?? "";
      for (const part of parts) {
        if (!part.trim()) continue;
        const lines = part.split("\n");
        const dataLines = lines.filter((line) => line.startsWith("data:"));
        const data = dataLines.map((line) => line.slice(5).trim()).join("\n");
        if (!data) continue;
        try {
          yield JSON.parse(data);
        } catch {
          continue;
        }
      }
    }
  } finally {
    try {
      await reader.cancel();
    } catch {
      // ignore
    }
  }
};

const streamChatCompletions = (upstream: Response, model: string): Response => {
  if (!upstream.body) {
    return openaiError(502, "Upstream response missing body", "bad_gateway");
  }

  const encoder = new TextEncoder();
  const stream = new ReadableStream<Uint8Array>({
    async start(controller) {
      let id = `chatcmpl_${crypto.randomUUID().replace(/-/g, "")}`;
      let created = Math.floor(Date.now() / 1000);
      let sentRole = false;

      try {
        for await (const ev of parseSseEvents(upstream.body!)) {
          if (!isRecord(ev)) continue;
          const type = getString(ev.type);
          if (type === "response.created" && isRecord(ev.response)) {
            const upstreamId = getString(ev.response.id);
            const createdAt = typeof ev.response.created_at === "number" ? ev.response.created_at : null;
            if (upstreamId) id = upstreamId;
            if (createdAt) created = createdAt;
            continue;
          }

          if (type === "response.output_text.delta") {
            const delta = getString(ev.delta) ?? "";
            const chunk: Record<string, unknown> = {
              id,
              object: "chat.completion.chunk",
              created,
              model,
              choices: [
                {
                  index: 0,
                  delta: sentRole ? { content: delta } : { role: "assistant", content: delta },
                  finish_reason: null,
                },
              ],
            };
            sentRole = true;
            controller.enqueue(encoder.encode(`data: ${JSON.stringify(chunk)}\n\n`));
            continue;
          }

          if (type === "response.completed") {
            const chunk: Record<string, unknown> = {
              id,
              object: "chat.completion.chunk",
              created,
              model,
              choices: [
                {
                  index: 0,
                  delta: sentRole ? {} : { role: "assistant" },
                  finish_reason: "stop",
                },
              ],
            };
            controller.enqueue(encoder.encode(`data: ${JSON.stringify(chunk)}\n\n`));
            controller.enqueue(encoder.encode("data: [DONE]\n\n"));
            controller.close();
            return;
          }
        }

        controller.enqueue(encoder.encode("data: [DONE]\n\n"));
        controller.close();
      } catch (error) {
        controller.error(error);
      }
    },
  });

  return new Response(stream, {
    status: 200,
    headers: {
      "Content-Type": "text/event-stream",
      "Cache-Control": "no-cache",
      "x-ubq-upstream": "chatgpt_codex",
    },
  });
};

const completeChatCompletions = async (upstream: Response, model: string): Promise<Response> => {
  if (!upstream.body) return openaiError(502, "Upstream response missing body", "bad_gateway");

  let id = `chatcmpl_${crypto.randomUUID().replace(/-/g, "")}`;
  let created = Math.floor(Date.now() / 1000);
  let content = "";
  let usage: Record<string, unknown> | null = null;

  for await (const ev of parseSseEvents(upstream.body)) {
    if (!isRecord(ev)) continue;
    const type = getString(ev.type);
    if (type === "response.created" && isRecord(ev.response)) {
      const upstreamId = getString(ev.response.id);
      const createdAt = typeof ev.response.created_at === "number" ? ev.response.created_at : null;
      if (upstreamId) id = upstreamId;
      if (createdAt) created = createdAt;
      continue;
    }
    if (type === "response.output_text.delta") {
      content += getString(ev.delta) ?? "";
      continue;
    }
    if (type === "response.completed" && isRecord(ev.response)) {
      const u = isRecord(ev.response.usage) ? ev.response.usage : null;
      if (u) {
        const promptTokens = typeof u.input_tokens === "number" ? u.input_tokens : null;
        const completionTokens = typeof u.output_tokens === "number" ? u.output_tokens : null;
        const totalTokens = typeof u.total_tokens === "number" ? u.total_tokens : null;
        if (promptTokens !== null && completionTokens !== null && totalTokens !== null) {
          usage = {
            prompt_tokens: promptTokens,
            completion_tokens: completionTokens,
            total_tokens: totalTokens,
          };
        }
      }
      break;
    }
  }

  const body: Record<string, unknown> = {
    id,
    object: "chat.completion",
    created,
    model,
    choices: [
      {
        index: 0,
        message: { role: "assistant", content },
        finish_reason: "stop",
      },
    ],
  };
  if (usage) body.usage = usage;
  return json(200, body, { "x-ubq-upstream": "chatgpt_codex" });
};

const handleChatCompletions = async (req: Request): Promise<Response> => {
  const body = (await readJsonBody(req)) as ChatCompletionRequest | null;
  if (!body || !isRecord(body)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const modelRaw = (getString(body.model) ?? "").trim() || DEFAULT_MODEL;
  const model = normalizeModelForCodex(modelRaw);
  const messagesRaw = body.messages;
  if (!Array.isArray(messagesRaw)) return openaiError(400, "messages must be an array", "invalid_request_error");

  const reasoningEffort = parseReasoningEffortField(body.reasoning_effort, "reasoning_effort");
  if (!reasoningEffort.ok) return openaiError(400, reasoningEffort.message, "invalid_request_error");

  const input: ResponseMessageItem[] = [];
  for (const msg of messagesRaw) {
    const converted = toResponseMessageItem(msg);
    if (!converted) return openaiError(400, "Invalid message in messages[]", "invalid_request_error");
    input.push(converted);
  }

  const defaultReasoning = looksLikeReasoningModel(model) ? { effort: DEFAULT_REASONING_EFFORT } : undefined;
  const codexBody = await buildCodexRequest(model, input, {
    reasoning: reasoningEffort.value === undefined ? defaultReasoning : { effort: reasoningEffort.value },
  });

  let upstream: Response;
  try {
    upstream = await fetchCodexResponses(codexBody);
  } catch (error) {
    console.error("[ai.ubq.fi] Upstream fetch failed:", error);
    return openaiError(502, "Upstream request failed", "bad_gateway");
  }

  if (!upstream.ok) {
    const text = await upstream.text().catch(() => "");
    return new Response(text || upstream.statusText, {
      status: upstream.status,
      headers: {
        "Content-Type": upstream.headers.get("Content-Type") ?? "text/plain",
        "x-ubq-upstream": "chatgpt_codex",
      },
    });
  }

  const stream = Boolean(body.stream);
  return stream ? streamChatCompletions(upstream, model) : await completeChatCompletions(upstream, model);
};

const handleResponses = async (req: Request): Promise<Response> => {
  const body = (await readJsonBody(req)) as ResponsesRequest | null;
  if (!body || !isRecord(body)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const modelRaw = (getString(body.model) ?? "").trim() || DEFAULT_MODEL;
  const model = normalizeModelForCodex(modelRaw);
  const inputRaw = body.input;

  let input: ResponseMessageItem[];
  if (typeof inputRaw === "string") {
    input = [{ type: "message", role: "user", content: [{ type: "input_text", text: inputRaw }] }];
  } else if (Array.isArray(inputRaw)) {
    input = [];
    for (const item of inputRaw) {
      const converted = toResponseMessageItem(item);
      if (!converted) return openaiError(400, "Invalid item in input[]", "invalid_request_error");
      input.push(converted);
    }
  } else {
    return openaiError(400, "input must be a string or an array", "invalid_request_error");
  }

  const reasoning = parseReasoningParam(body.reasoning);
  if (!reasoning.ok) return openaiError(400, reasoning.message, "invalid_request_error");

  const defaultReasoning = looksLikeReasoningModel(model) ? { effort: DEFAULT_REASONING_EFFORT } : undefined;
  const codexBody = await buildCodexRequest(model, input, {
    reasoning: reasoning.value === undefined ? defaultReasoning : reasoning.value,
  });

  let upstream: Response;
  try {
    upstream = await fetchCodexResponses(codexBody);
  } catch (error) {
    console.error("[ai.ubq.fi] Upstream fetch failed:", error);
    return openaiError(502, "Upstream request failed", "bad_gateway");
  }

  if (!upstream.ok) {
    const text = await upstream.text().catch(() => "");
    return new Response(text || upstream.statusText, {
      status: upstream.status,
      headers: {
        "Content-Type": upstream.headers.get("Content-Type") ?? "text/plain",
        "x-ubq-upstream": "chatgpt_codex",
      },
    });
  }

  const clientWantsStream = Boolean(body.stream);
  if (clientWantsStream) {
    const headers = new Headers(upstream.headers);
    headers.set("x-ubq-upstream", "chatgpt_codex");
    return new Response(upstream.body, {
      status: upstream.status,
      statusText: upstream.statusText,
      headers,
    });
  }

  if (!upstream.body) return openaiError(502, "Upstream response missing body", "bad_gateway");

  let finalResponse: unknown | null = null;
  for await (const ev of parseSseEvents(upstream.body)) {
    if (!isRecord(ev)) continue;
    if (getString(ev.type) === "response.completed" && isRecord(ev.response)) {
      finalResponse = ev.response;
      break;
    }
  }
  if (!finalResponse) return openaiError(502, "Upstream stream ended unexpectedly", "bad_gateway");
  return json(200, finalResponse, { "x-ubq-upstream": "chatgpt_codex" });
};

const handleAdminCodexAuth = async (req: Request): Promise<Response> => {
  const kv = await kvPromise;
  if (!kv) {
    return openaiError(500, "Deno KV is not available; cannot persist Codex auth", "server_error");
  }

  const body = await readJsonBody(req);
  const tokenData = parseCodexAuthFromAuthJson(body);
  if (!tokenData) {
    return openaiError(400, "Body does not look like a Codex auth.json", "invalid_request_error");
  }

  const seed: CodexAuthState = { ...tokenData, updated_at_ms: Date.now() };

  let validated: Awaited<ReturnType<typeof validateCodexAuthJson>>;
  try {
    validated = await validateCodexAuthJson(seed);
  } catch (error) {
    console.error("[ai.ubq.fi] Codex auth validation failed:", error);
    return openaiError(502, "Upstream validation request failed", "bad_gateway");
  }

  if (!validated.ok) {
    return openaiError(
      401,
      `Invalid Codex auth.json (upstream ${validated.status}): ${validated.body}`,
      "invalid_api_key",
    );
  }

  await kv.set(CODEX_KV_KEY, validated.auth);
  cachedAuth = validated.auth;

  const expMs = getJwtExpMs(validated.auth.access_token);
  return json(
    200,
    {
      ok: true,
      stored: true,
      refreshed: validated.refreshed,
      account_id: validated.auth.account_id,
      access_token_expires_at_ms: expMs,
      updated_at_ms: validated.auth.updated_at_ms,
      upstream_status: validated.status,
      upstream_content_type: validated.contentType,
    },
    { "x-ubq-upstream": "chatgpt_codex" },
  );
};

const normalizeApiKeyName = (value: unknown): string | null => {
  if (typeof value !== "string") return null;
  const name = value.trim();
  if (!name) return null;
  if (name.length > 80) return null;
  if (/[\r\n]/.test(name)) return null;
  return name;
};

const normalizeOptionalApiKeyToken = (value: unknown): string | null => {
  if (value === undefined || value === null) return null;
  if (typeof value !== "string") return null;
  const token = value.trim();
  if (!token) return null;
  if (/\s/.test(token)) return null;
  if (token.length < 24) return null;
  if (token.length > 300) return null;
  return token;
};

const handleAdminApiKeysCreate = async (req: Request): Promise<Response> => {
  const kv = await kvPromise;
  if (!kv) {
    return openaiError(500, "Deno KV is not available; cannot manage API keys", "server_error");
  }

  const raw = await readJsonBody(req);
  if (!raw || !isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const name = normalizeApiKeyName(raw.name);
  if (!name) return openaiError(400, "name must be a non-empty string (<=80 chars)", "invalid_request_error");

  const providedToken = normalizeOptionalApiKeyToken(raw.token);
  const token = providedToken ?? generateApiKeyToken();

  const hash = await sha256Base64Url(token);
  const hashKey = apiKeyHashKey(hash);
  const hashEntry = await kv.get<ApiKeyHashRecord>(hashKey);
  if (hashEntry.value) {
    return openaiError(409, "API key already exists", "invalid_request_error");
  }

  const id = crypto.randomUUID();
  const now = Date.now();
  const record: ApiKeyRecord = {
    id,
    name,
    prefix: token.slice(0, 12),
    hash,
    created_at_ms: now,
    revoked_at_ms: null,
  };
  const hashRecord: ApiKeyHashRecord = { id, revoked_at_ms: null };

  const commit = await kv.atomic()
    .check(hashEntry)
    .set(apiKeyIdKey(id), record)
    .set(hashKey, hashRecord)
    .commit();
  if (!commit.ok) {
    return openaiError(500, "Failed to persist API key", "server_error");
  }

  return json(
    200,
    {
      ok: true,
      id,
      name,
      token,
      prefix: record.prefix,
      created_at_ms: record.created_at_ms,
    },
    { "x-ubq-upstream": "chatgpt_codex" },
  );
};

const handleAdminApiKeysList = async (): Promise<Response> => {
  const kv = await kvPromise;
  if (!kv) {
    return openaiError(500, "Deno KV is not available; cannot manage API keys", "server_error");
  }

  const records: ApiKeyRecord[] = [];
  for await (const entry of kv.list<ApiKeyRecord>({ prefix: API_KEY_ID_PREFIX })) {
    if (entry.value) records.push(entry.value);
  }
  records.sort((a, b) => b.created_at_ms - a.created_at_ms);

  return json(
    200,
    {
      object: "list",
      data: records.map((r) => ({
        id: r.id,
        name: r.name,
        prefix: r.prefix,
        created_at_ms: r.created_at_ms,
        revoked_at_ms: r.revoked_at_ms,
      })),
    },
    { "x-ubq-upstream": "chatgpt_codex" },
  );
};

const handleAdminApiKeysRevoke = async (req: Request): Promise<Response> => {
  const kv = await kvPromise;
  if (!kv) {
    return openaiError(500, "Deno KV is not available; cannot manage API keys", "server_error");
  }

  const raw = await readJsonBody(req);
  if (!raw || !isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");
  const id = getString(raw.id);
  if (!id) return openaiError(400, "id is required", "invalid_request_error");

  const idKey = apiKeyIdKey(id);
  const entry = await kv.get<ApiKeyRecord>(idKey);
  if (!entry.value) return openaiError(404, "Not found", "not_found");

  const now = Date.now();
  const updated: ApiKeyRecord = entry.value.revoked_at_ms ? entry.value : { ...entry.value, revoked_at_ms: now };
  const hashKey = apiKeyHashKey(entry.value.hash);
  const hashEntry = await kv.get<ApiKeyHashRecord>(hashKey);
  const updatedHash: ApiKeyHashRecord = { id, revoked_at_ms: updated.revoked_at_ms };

  const atomic = kv.atomic()
    .check(entry)
    .set(idKey, updated)
    .set(hashKey, updatedHash);
  if (hashEntry.versionstamp) atomic.check(hashEntry);

  const commit = await atomic.commit();
  if (!commit.ok) {
    return openaiError(409, "API key was modified concurrently; retry", "invalid_request_error");
  }

  return json(
    200,
    {
      ok: true,
      id: updated.id,
      revoked_at_ms: updated.revoked_at_ms,
    },
    { "x-ubq-upstream": "chatgpt_codex" },
  );
};

async function handler(req: Request): Promise<Response> {
  if (req.method === "OPTIONS") {
    return withCors(new Response(null, { status: 204, headers: corsHeaders() }));
  }

  const url = new URL(req.url);
  const path = url.pathname;

  if (req.method === "GET" && (path === "/" || path === "/index.html")) {
    const accept = req.headers.get("Accept") ?? "";
    const wantsHtml = path === "/index.html" || accept.includes("text/html") ||
      accept.includes("application/xhtml+xml");
    if (wantsHtml) {
      const html = await indexHtmlPromise;
      if (html) {
        return withCors(
          new Response(html, {
            status: 200,
            headers: {
              "Content-Type": "text/html; charset=utf-8",
              "Vary": "Accept",
              "Cache-Control": "public, max-age=300",
              "Content-Security-Policy":
                "default-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'; img-src 'self' data:; object-src 'none'; style-src 'self'; script-src 'self'; connect-src 'self'",
              "Referrer-Policy": "no-referrer",
              "X-Content-Type-Options": "nosniff",
            },
          }),
        );
      }
    }

    const kv = await kvPromise;
    const auth = config.isDeploy && config.authTokens.size === 0 && !kv
      ? "misconfigured"
      : config.isDeploy || config.authTokens.size > 0 || Boolean(kv)
      ? "required"
      : "disabled (local only)";

    return withCors(
      json(
        200,
        {
          ok: true,
          service: "ai.ubq.fi",
          upstream: "chatgpt_codex",
          auth,
          endpoints: {
            openai_compat: "/v1/*",
            health: "/health",
          },
        },
        { "Vary": "Accept" },
      ),
    );
  }

  if (req.method === "GET" && path === "/style.css") {
    const css = await styleCssPromise;
    if (!css) {
      return withCors(
        new Response("Not found", {
          status: 404,
          headers: { "Content-Type": "text/plain; charset=utf-8" },
        }),
      );
    }
    return withCors(
      new Response(css, {
        status: 200,
        headers: {
          "Content-Type": "text/css; charset=utf-8",
          "Cache-Control": "public, max-age=300",
          "X-Content-Type-Options": "nosniff",
        },
      }),
    );
  }

  if (req.method === "GET" && path === "/app.js") {
    const js = await appJsPromise;
    if (!js) {
      return withCors(
        new Response("Not found", {
          status: 404,
          headers: { "Content-Type": "text/plain; charset=utf-8" },
        }),
      );
    }
    return withCors(
      new Response(js, {
        status: 200,
        headers: {
          "Content-Type": "text/javascript; charset=utf-8",
          "Cache-Control": "public, max-age=300",
          "X-Content-Type-Options": "nosniff",
        },
      }),
    );
  }

  if (req.method === "GET" && path === "/health") {
    const problems: string[] = [];
    if (!config.codexAuthJsonB64) problems.push("CODEX_AUTH_JSON_B64 missing");
    const kv = await kvPromise;
    if (config.isDeploy && config.authTokens.size === 0 && !kv) {
      problems.push("No UBIQUITY_AI_USER_TOKEN and Deno KV unavailable");
    }
    try {
      await codexInstructionsPromise;
    } catch {
      problems.push("CODEX instructions missing (codex_instructions.md)");
    }
    return withCors(
      json(problems.length === 0 ? 200 : 500, {
        ok: problems.length === 0,
        problems,
      }),
    );
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
    const authResult = await authenticateClient(req);
    if (!authResult.ok) return withCors(authResult.response);

    const kv = await kvPromise;
    const mode = config.isDeploy && config.authTokens.size === 0 && !kv
      ? "misconfigured"
      : config.isDeploy || config.authTokens.size > 0 || Boolean(kv)
      ? "required"
      : "disabled";

    const token = authResult.token;
    const tokenInfo = token
      ? {
        present: true,
        length: token.length,
        shape: classifyToken(token),
        sha256_12: (await sha256Hex(token)).slice(0, 12),
      }
      : {
        present: false,
        length: null,
        shape: null,
        sha256_12: null,
      };

    const method: Record<string, unknown> = { kind: authResult.method.kind };
    const isAdmin = authResult.method.kind === "admin_allowlist" || authResult.method.kind === "deno_deploy_token";

    if (authResult.method.kind === "kv_api_key") {
      const id = authResult.method.key_id;
      let key: Record<string, unknown> = { id };
      if (kv) {
        const entry = await kv.get<ApiKeyRecord>(apiKeyIdKey(id));
        if (entry.value) {
          key = {
            id: entry.value.id,
            name: entry.value.name,
            prefix: entry.value.prefix,
            created_at_ms: entry.value.created_at_ms,
            revoked_at_ms: entry.value.revoked_at_ms,
          };
        }
      }
      method.key = key;
    }

    return withCors(
      json(
        200,
        {
          ok: true,
          service: "ai.ubq.fi",
          auth: {
            mode,
            is_admin: isAdmin,
            method,
            token: tokenInfo,
          },
        },
        { "Cache-Control": "no-store" },
      ),
    );
  }

  const authError = await requireClientAuth(req);
  if (authError) return withCors(authError);

  if (req.method === "GET" && path === "/v1/models") {
    return withCors(
      json(
        200,
        {
          object: "list",
          data: [
            { id: "gpt-5-chat-latest", object: "model", owned_by: "openai" },
            { id: "gpt-5.2-chat-latest", object: "model", owned_by: "openai" },
            { id: "gpt-5.1-chat-latest", object: "model", owned_by: "openai" },
            { id: "gpt-5.1-codex-max", object: "model", owned_by: "openai" },
            { id: "gpt-5.1-codex", object: "model", owned_by: "openai" },
            { id: "gpt-5.1-codex-mini", object: "model", owned_by: "openai" },
            { id: "gpt-5.2", object: "model", owned_by: "openai" },
            { id: "gpt-5.1", object: "model", owned_by: "openai" },
          ],
        },
        { "x-ubq-upstream": "chatgpt_codex" },
      ),
    );
  }

  if (req.method === "POST" && path === "/v1/chat/completions") {
    return withCors(await handleChatCompletions(req));
  }

  if (req.method === "POST" && path === "/v1/responses") {
    return withCors(await handleResponses(req));
  }

  return withCors(openaiError(404, "Not found", "not_found"));
}

export default { fetch: handler };
