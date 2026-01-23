import { config } from "./config.ts";
import { kvPromise } from "./kv.ts";
import { decodeBase64ToString, getString, isRecord } from "./utils.ts";
import type { CodexAuthState, ResponseMessageItem } from "./types.ts";

const CODEX_REFRESH_CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann";
const CODEX_REFRESH_TOKEN_URL = "https://auth.openai.com/oauth/token";
const CODEX_ORIGINATOR = "codex_cli_rs";
const CODEX_USER_AGENT = "codex_cli_rs/0.99.0 (ai.ubq.fi)";

export type CodexErrorCode =
  | "codex_auth_missing"
  | "codex_auth_invalid"
  | "codex_auth_refresh_failed"
  | "codex_auth_refresh_unreachable"
  | "codex_upstream_unreachable";

export class CodexError extends Error {
  readonly code: CodexErrorCode;
  readonly status: number;

  constructor(message: string, code: CodexErrorCode, status: number, cause?: unknown) {
    super(message);
    this.name = "CodexError";
    this.code = code;
    this.status = status;
    if (cause !== undefined) {
      (this as { cause?: unknown }).cause = cause;
    }
  }
}

export const CODEX_KV_KEY = ["ubq_ai", "codex_auth"] as const;
export const CODEX_MODELS_KV_KEY = ["ubq_ai", "codex_models"] as const;

export type CodexModelsSnapshot = Readonly<{
  models: Record<string, unknown>[];
  source: string;
  updated_at_ms: number;
  client_version?: string | null;
}>;

export const parseCodexAuthFromAuthJson = (value: unknown): Omit<CodexAuthState, "updated_at_ms"> | null => {
  if (!isRecord(value)) return null;
  const tokens = isRecord(value.tokens) ? value.tokens : null;
  if (!tokens) return null;
  const access_token = getString(tokens.access_token);
  const refresh_token = getString(tokens.refresh_token);
  const account_id = getString(tokens.account_id);
  if (!access_token || !refresh_token || !account_id) return null;
  return { access_token, refresh_token, account_id };
};

export const getJwtExpMs = (token: string): number | null => {
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

export const cacheCodexAuth = (auth: CodexAuthState): void => {
  cachedAuth = auth;
};

const loadAuthSeedFromEnv = (): CodexAuthState => {
  if (!config.codexAuthJsonB64) {
    throw new CodexError(
      "Codex auth missing: CODEX_AUTH_JSON_B64 unset and no KV entry.",
      "codex_auth_missing",
      503,
    );
  }
  let decoded: string;
  try {
    decoded = decodeBase64ToString(config.codexAuthJsonB64);
  } catch (error) {
    throw new CodexError(
      "Codex auth invalid: CODEX_AUTH_JSON_B64 is not valid base64.",
      "codex_auth_invalid",
      503,
      error,
    );
  }
  let parsed: unknown;
  try {
    parsed = JSON.parse(decoded) as unknown;
  } catch (error) {
    throw new CodexError(
      "Codex auth invalid: CODEX_AUTH_JSON_B64 is not valid JSON.",
      "codex_auth_invalid",
      503,
      error,
    );
  }
  const tokenData = parseCodexAuthFromAuthJson(parsed);
  if (!tokenData) {
    throw new CodexError(
      "Codex auth invalid: CODEX_AUTH_JSON_B64 does not look like a Codex auth.json.",
      "codex_auth_invalid",
      503,
    );
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

const formatFailureSnippet = (raw: string, maxLen = 240): string => {
  const trimmed = raw.trim();
  if (!trimmed) return "";
  if (trimmed.length <= maxLen) return trimmed;
  return `${trimmed.slice(0, maxLen)}...`;
};

const buildRefreshFailureMessage = async (response: Response): Promise<string> => {
  const text = await response.text().catch(() => "");
  const detail = formatFailureSnippet(text || response.statusText);
  return detail
    ? `Codex auth refresh failed (status ${response.status}): ${detail}`
    : `Codex auth refresh failed (status ${response.status}).`;
};

const refreshAuth = async (
  current: { kv: Deno.Kv | null; entry: Deno.KvEntryMaybe<CodexAuthState> | null; auth: CodexAuthState },
): Promise<CodexAuthState> => {
  let response: Response;
  try {
    response = await fetch(CODEX_REFRESH_TOKEN_URL, {
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
  } catch (error) {
    throw new CodexError(
      "Codex auth refresh failed: auth server unreachable.",
      "codex_auth_refresh_unreachable",
      502,
      error,
    );
  }

  if (!response.ok) {
    throw new CodexError(await buildRefreshFailureMessage(response), "codex_auth_refresh_failed", 503);
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
  let response: Response;
  try {
    response = await fetch(CODEX_REFRESH_TOKEN_URL, {
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
  } catch (error) {
    throw new CodexError(
      "Codex auth refresh failed: auth server unreachable.",
      "codex_auth_refresh_unreachable",
      502,
      error,
    );
  }

  if (!response.ok) {
    throw new CodexError(await buildRefreshFailureMessage(response), "codex_auth_refresh_failed", 503);
  }

  const parsed = (await response.json().catch(() => null)) as null | Record<string, unknown>;
  const access_token = parsed && getString(parsed.access_token);
  const refresh_token = parsed && getString(parsed.refresh_token);
  if (!access_token) {
    throw new CodexError(
      "Codex auth refresh failed: upstream response missing access_token.",
      "codex_auth_refresh_failed",
      503,
    );
  }

  return {
    access_token,
    refresh_token: refresh_token ?? auth.refresh_token,
    account_id: auth.account_id,
    updated_at_ms: Date.now(),
  };
};

type CodexAuthRefreshResult =
  | { ok: true; auth: CodexAuthState }
  | { ok: false; status: number; code: CodexErrorCode; error: string };

export const checkCodexAuthRefresh = async (): Promise<CodexAuthRefreshResult> => {
  const current = await getAuthEntry();
  try {
    const auth = await refreshAuth(current);
    return { ok: true, auth };
  } catch (error) {
    if (error instanceof CodexError) {
      return { ok: false, status: error.status, code: error.code, error: error.message };
    }
    const detail = error instanceof Error ? error.message : String(error);
    return { ok: false, status: 503, code: "codex_auth_refresh_failed", error: detail };
  }
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

const codexModelsBaseUrls = (): string[] => {
  const base = config.codexBaseUrl.replace(/\/+$/, "");
  const urls = new Set<string>();
  if (base.endsWith("/codex")) {
    urls.add(`${base.slice(0, -"/codex".length)}/models`);
  }
  urls.add(`${base}/models`);
  return Array.from(urls);
};

const fetchCodexModelsWithAuth = async (auth: CodexAuthState, url: string): Promise<Response> => {
  const headers = new Headers();
  headers.set("Authorization", `Bearer ${auth.access_token}`);
  headers.set("ChatGPT-Account-ID", auth.account_id);
  headers.set("originator", CODEX_ORIGINATOR);
  headers.set("user-agent", CODEX_USER_AGENT);
  headers.set("Accept", "application/json");

  try {
    return await fetch(url, {
      method: "GET",
      headers,
      redirect: "manual",
    });
  } catch (error) {
    throw new CodexError(
      "Codex upstream request failed: upstream unreachable.",
      "codex_upstream_unreachable",
      502,
      error,
    );
  }
};

export const fetchCodexResponses = async (body: unknown): Promise<Response> => {
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

  let res: Response;
  try {
    res = await fetch(url, {
      method: "POST",
      headers,
      body: JSON.stringify(body),
      redirect: "manual",
    });
  } catch (error) {
    throw new CodexError(
      "Codex upstream request failed: upstream unreachable.",
      "codex_upstream_unreachable",
      502,
      error,
    );
  }

  if (res.status !== 401) return res;

  await refreshAuth(await getAuthEntry());

  const auth2 = await getValidAuth();
  headers.set("Authorization", `Bearer ${auth2.access_token}`);
  headers.set("ChatGPT-Account-ID", auth2.account_id);
  try {
    return await fetch(url, {
      method: "POST",
      headers,
      body: JSON.stringify(body),
      redirect: "manual",
    });
  } catch (error) {
    throw new CodexError(
      "Codex upstream request failed: upstream unreachable.",
      "codex_upstream_unreachable",
      502,
      error,
    );
  }
};

export const fetchCodexModels = async (): Promise<Response> => {
  const auth = await getValidAuth();
  const urls = codexModelsBaseUrls();
  let lastResponse: Response | null = null;

  for (const url of urls) {
    let res = await fetchCodexModelsWithAuth(auth, url);
    if (res.status === 401) {
      await refreshAuth(await getAuthEntry());
      const auth2 = await getValidAuth();
      res = await fetchCodexModelsWithAuth(auth2, url);
    }
    if (res.status === 404 && urls.length > 1) {
      lastResponse = res;
      continue;
    }
    return res;
  }

  return lastResponse ?? new Response("Codex upstream models endpoint not found.", { status: 404 });
};

export const loadCodexModelsSnapshot = async (): Promise<CodexModelsSnapshot | null> => {
  const kv = await kvPromise;
  if (!kv) return null;
  const entry = await kv.get<CodexModelsSnapshot>(CODEX_MODELS_KV_KEY);
  return entry.value ?? null;
};

export const storeCodexModelsSnapshot = async (snapshot: CodexModelsSnapshot): Promise<boolean> => {
  const kv = await kvPromise;
  if (!kv) return false;
  await kv.set(CODEX_MODELS_KV_KEY, snapshot);
  return true;
};

export const buildCodexRequest = async (
  model: string,
  input: ResponseMessageItem[],
  options: Readonly<{ reasoning?: Record<string, unknown> | null; instructions?: string | null }> = {},
): Promise<Record<string, unknown>> => {
  const body: Record<string, unknown> = {
    model,
    input,
    store: false,
    stream: true,
  };

  if (options.reasoning !== undefined) body.reasoning = options.reasoning;
  if (options.instructions !== undefined) body.instructions = options.instructions;

  return body;
};

export const validateCodexAuthJson = async (
  auth: CodexAuthState,
): Promise<
  { ok: true; auth: CodexAuthState; refreshed: boolean; status: number; contentType: string | null } | {
    ok: false;
    status: number;
    body: string;
  }
> => {
  const urls = codexModelsBaseUrls();
  let refreshed = false;
  let lastResponse: Response | null = null;

  for (const url of urls) {
    let res = await fetchCodexModelsWithAuth(auth, url);
    if (res.status === 401) {
      try {
        const next = await refreshAuthStateless(auth);
        refreshed = true;
        res = await fetchCodexModelsWithAuth(next, url);
        auth = next;
      } catch {
        // ignore and return the original 401 response
      }
    }
    if (res.status === 404 && urls.length > 1) {
      lastResponse = res;
      continue;
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
  }

  const fallback = lastResponse ?? new Response("Codex upstream models endpoint not found.", { status: 404 });
  const text = await fallback.text().catch(() => "");
  const bodySnippet = (text || fallback.statusText).slice(0, 8_000);
  return { ok: false, status: fallback.status, body: bodySnippet };
};
