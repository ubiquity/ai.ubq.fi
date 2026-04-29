import { config } from "./config.ts";
import { CODEX_KV_KEY, fetchCodexModels, getJwtExpMs, parseCodexAuthFromAuthJson } from "./codex.ts";
import { json } from "./http.ts";
import { kvPromise } from "./kv.ts";
import { decodeBase64ToString } from "./utils.ts";

const AUTH_REFRESH_WINDOW_MS = 2 * 60_000;

export const handleHealth = async (): Promise<Response> => {
  const problems: string[] = [];
  const kv = await kvPromise;
  let hasCodexAuth = Boolean(config.codexAuthJsonB64);
  if (!hasCodexAuth && kv) {
    const entry = await kv.get(CODEX_KV_KEY);
    hasCodexAuth = Boolean(entry.value);
  }
  if (!hasCodexAuth) problems.push("CODEX_AUTH_JSON_B64 missing");
  if (config.isDeploy && config.authTokens.size === 0 && !kv) {
    problems.push("No UOS_AI_TOKEN and Deno KV unavailable");
  }
  return json(problems.length === 0 ? 200 : 500, {
    ok: problems.length === 0,
    problems,
  });
};

const loadEnvCodexAuth = (): { source: "env"; access_token_exp_ms: number | null } | null => {
  if (!config.codexAuthJsonB64) return null;
  try {
    const decoded = decodeBase64ToString(config.codexAuthJsonB64);
    const parsed = JSON.parse(decoded);
    const auth = parseCodexAuthFromAuthJson(parsed);
    if (!auth) return null;
    return { source: "env", access_token_exp_ms: getJwtExpMs(auth.access_token) };
  } catch {
    return null;
  }
};

const getCodexAuthMeta = async (): Promise<
  | { source: "kv"; updated_at_ms: number; access_token_exp_ms: number | null }
  | { source: "env"; updated_at_ms: null; access_token_exp_ms: number | null }
  | { source: "none"; updated_at_ms: null; access_token_exp_ms: null }
> => {
  const kv = await kvPromise;
  if (kv) {
    const entry = await kv.get<{ access_token: string; updated_at_ms: number }>(CODEX_KV_KEY);
    if (entry.value) {
      return {
        source: "kv",
        updated_at_ms: entry.value.updated_at_ms,
        access_token_exp_ms: getJwtExpMs(entry.value.access_token),
      };
    }
  }
  const envMeta = loadEnvCodexAuth();
  if (envMeta) return { ...envMeta, updated_at_ms: null };
  return { source: "none", updated_at_ms: null, access_token_exp_ms: null };
};

export const handleHealthAuth = async (): Promise<Response> => {
  const authMeta = await getCodexAuthMeta();
  if (authMeta.source === "none") {
    return json(503, {
      ok: false,
      upstream: "chatgpt_codex",
      error: "No Codex auth configured (CODEX_AUTH_JSON_B64 or KV entry missing).",
      auth: authMeta,
    });
  }

  const expMs = authMeta.access_token_exp_ms;
  const now = Date.now();
  const accessTokenExpired = typeof expMs === "number" ? expMs <= now : null;
  const refreshRecommended = typeof expMs === "number" ? expMs - now < AUTH_REFRESH_WINDOW_MS : null;

  return json(200, {
    ok: true,
    upstream: "chatgpt_codex",
    auth: {
      ...authMeta,
      access_token_expired: accessTokenExpired,
      refresh_recommended: refreshRecommended,
    },
  });
};

export const handleHealthUpstream = async (): Promise<Response> => {
  const authMeta = await getCodexAuthMeta();
  if (authMeta.source === "none") {
    return json(503, {
      ok: false,
      upstream: "chatgpt_codex",
      error: "No Codex auth configured (CODEX_AUTH_JSON_B64 or KV entry missing).",
      auth: authMeta,
    });
  }

  let res: Response;
  try {
    res = await fetchCodexModels();
  } catch (error) {
    const detail = error instanceof Error ? error.message : String(error);
    return json(503, {
      ok: false,
      upstream: "chatgpt_codex",
      error: detail || "Upstream fetch failed",
      auth: authMeta,
    });
  }

  const contentType = res.headers.get("Content-Type");

  if (res.ok) {
    try {
      await res.body?.cancel();
    } catch {
      // ignore
    }
    return json(200, {
      ok: true,
      upstream: "chatgpt_codex",
      status: res.status,
      content_type: contentType,
      auth: authMeta,
    });
  }

  const text = await res.text().catch(() => "");
  const snippet = text.trim().slice(0, 800) || res.statusText;

  return json(res.status === 401 ? 401 : 503, {
    ok: false,
    upstream: "chatgpt_codex",
    status: res.status,
    content_type: contentType,
    error: snippet,
    auth: authMeta,
  });
};
