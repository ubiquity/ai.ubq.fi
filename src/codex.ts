import { config } from "./config.ts";
import { kvPromise } from "./kv.ts";
import { decodeBase64ToString, getString, isRecord } from "./utils.ts";
import type { CodexAuthState, ResponseMessageItem } from "./types.ts";

const CODEX_REFRESH_CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann";
const CODEX_REFRESH_TOKEN_URL = "https://auth.openai.com/oauth/token";
const CODEX_ORIGINATOR = "codex_cli_rs";
const CODEX_USER_AGENT = "codex_cli_rs/0.99.0 (ai.ubq.fi)";

export const CODEX_KV_KEY = ["ubq_ai", "codex_auth"] as const;

const CODEX_INSTRUCTIONS_URL = new URL("../codex_instructions.md", import.meta.url);

export const codexInstructionsPromise: Promise<string> = (async () => {
  if (config.codexInstructionsB64) return decodeBase64ToString(config.codexInstructionsB64);
  return await Deno.readTextFile(CODEX_INSTRUCTIONS_URL);
})();

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

export const buildCodexRequest = async (
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

export const validateCodexAuthJson = async (
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
