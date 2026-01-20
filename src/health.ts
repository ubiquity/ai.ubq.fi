import { config } from "./config.ts";
import {
  CODEX_KV_KEY,
  buildCodexRequest,
  checkCodexAuthRefresh,
  fetchCodexResponses,
  getCodexInstructions,
  getJwtExpMs,
  parseCodexAuthFromAuthJson,
} from "./codex.ts";
import { json } from "./http.ts";
import { kvPromise } from "./kv.ts";
import { decodeBase64ToString } from "./utils.ts";
import type { ResponseMessageItem } from "./types.ts";

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
  try {
    await getCodexInstructions();
  } catch {
    problems.push("CODEX instructions missing (env/KV or codex_instructions.md)");
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

  const result = await checkCodexAuthRefresh();
  if (result.ok) {
    return json(200, {
      ok: true,
      upstream: "chatgpt_codex",
      refreshed: true,
      auth: {
        source: authMeta.source,
        updated_at_ms: authMeta.updated_at_ms,
        access_token_exp_ms: getJwtExpMs(result.auth.access_token),
      },
    });
  }

  return json(result.status ?? 503, {
    ok: false,
    upstream: "chatgpt_codex",
    error: result.error,
    code: result.code,
    auth: authMeta,
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

  const model = "gpt-5.1-codex-mini";
  const input: ResponseMessageItem[] = [
    {
      type: "message",
      role: "user",
      content: [{ type: "input_text", text: "ping" }],
    },
  ];

  const body = await buildCodexRequest(model, input);

  let res: Response;
  try {
    res = await fetchCodexResponses(body);
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
