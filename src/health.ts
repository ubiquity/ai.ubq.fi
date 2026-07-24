import { config } from "./config.ts";
import {
  CODEX_AUTH_POOL_KV_KEY,
  fetchCodexModels,
  getJwtExpMs,
  parseCodexAuthFromAuthJson,
  parseCodexAuthPool,
} from "./codex.ts";
import { json } from "./http.ts";
import { kvPromise } from "./kv.ts";
import { decodeBase64ToString } from "./utils.ts";
import type { CodexAuthPoolState } from "./types.ts";

const AUTH_REFRESH_WINDOW_MS = 2 * 60_000;
const AUTH_NOT_CONFIGURED = "No Codex auth configured (CODEX_AUTH_JSON_B64 or KV entry missing).";

type HealthAuthMetaBase = {
  source: "kv" | "env" | "none";
  updated_at_ms: number | null;
  access_token_exp_ms: number | null;
  account_count: number;
  accounts: Array<{
    slot: number;
    updated_at_ms: number | null;
    access_token_exp_ms: number | null;
  }>;
};

type HealthAuthMeta = HealthAuthMetaBase & {
  access_token_expired: boolean | null;
  refresh_recommended: boolean | null;
  accounts: Array<{
    slot: number;
    updated_at_ms: number | null;
    access_token_exp_ms: number | null;
    access_token_expired: boolean | null;
    refresh_recommended: boolean | null;
  }>;
};

type HealthUpstreamProbe = {
  ok: boolean;
  status: number;
  upstream: "chatgpt_codex";
  content_type: string | null;
  auth: HealthAuthMeta;
  error?: string;
  details?: string;
  problems: string[];
};

const nowMs = (): number => Date.now();

const enrichAuthMeta = (meta: HealthAuthMetaBase): HealthAuthMeta => {
  const now = nowMs();
  const expMs = meta.access_token_exp_ms;
  return {
    ...meta,
    access_token_expired: typeof expMs === "number" ? expMs <= now : null,
    refresh_recommended: typeof expMs === "number" ? expMs - now < AUTH_REFRESH_WINDOW_MS : null,
    accounts: meta.accounts.map((account) => ({
      ...account,
      access_token_expired: typeof account.access_token_exp_ms === "number" ? account.access_token_exp_ms <= now : null,
      refresh_recommended: typeof account.access_token_exp_ms === "number"
        ? account.access_token_exp_ms - now < AUTH_REFRESH_WINDOW_MS
        : null,
    })),
  };
};

const loadEnvCodexAuth = (): HealthAuthMetaBase | null => {
  if (!config.codexAuthJsonB64) return null;
  try {
    const decoded = decodeBase64ToString(config.codexAuthJsonB64);
    const parsed = JSON.parse(decoded);
    const auth = parseCodexAuthFromAuthJson(parsed);
    if (!auth) return null;
    const accessTokenExpMs = getJwtExpMs(auth.access_token);
    return {
      source: "env",
      updated_at_ms: null,
      access_token_exp_ms: accessTokenExpMs,
      account_count: 1,
      accounts: [{
        slot: 1,
        updated_at_ms: null,
        access_token_exp_ms: accessTokenExpMs,
      }],
    };
  } catch {
    return null;
  }
};

const getCodexAuthMeta = async (): Promise<HealthAuthMetaBase> => {
  const kv = await kvPromise;
  if (kv) {
    const entry = await kv.get<CodexAuthPoolState>(CODEX_AUTH_POOL_KV_KEY);
    const pool = parseCodexAuthPool(entry.value);
    if (pool) {
      const accounts = pool.accounts.map((account, index) => ({
        slot: index + 1,
        updated_at_ms: account.updated_at_ms,
        access_token_exp_ms: getJwtExpMs(account.access_token),
      }));
      const expirations = accounts
        .map((account) => account.access_token_exp_ms)
        .filter((value): value is number => typeof value === "number");
      return {
        source: "kv",
        updated_at_ms: pool.updated_at_ms,
        access_token_exp_ms: expirations.length > 0 ? Math.min(...expirations) : null,
        account_count: accounts.length,
        accounts,
      };
    }
  }

  const envMeta = loadEnvCodexAuth();
  if (envMeta) return { ...envMeta, updated_at_ms: null };

  return {
    source: "none",
    updated_at_ms: null,
    access_token_exp_ms: null,
    account_count: 0,
    accounts: [],
  };
};

const probeUpstream = async (): Promise<HealthUpstreamProbe> => {
  const auth = enrichAuthMeta(await getCodexAuthMeta());

  if (auth.source === "none") {
    return {
      ok: false,
      status: 503,
      upstream: "chatgpt_codex",
      content_type: null,
      auth,
      error: AUTH_NOT_CONFIGURED,
      details: AUTH_NOT_CONFIGURED,
      problems: [AUTH_NOT_CONFIGURED],
    };
  }

  try {
    const res = await fetchCodexModels();
    const contentType = res.headers.get("Content-Type");
    if (res.ok) {
      try {
        await res.body?.cancel();
      } catch {
        // ignore
      }
      return {
        ok: true,
        status: 200,
        upstream: "chatgpt_codex",
        content_type: contentType,
        auth,
        problems: [],
      };
    }

    const text = await res.text().catch(() => "");
    const snippet = text.trim().slice(0, 800) || res.statusText;
    const status = res.status === 401 ? 401 : 503;
    return {
      ok: false,
      status,
      upstream: "chatgpt_codex",
      content_type: contentType,
      auth,
      error: snippet,
      details: `Codex upstream models endpoint returned ${res.status}.`,
      problems: status === 401 ? ["Upstream auth is invalid."] : ["Upstream unavailable."],
    };
  } catch (error) {
    const detail = error instanceof Error ? error.message : String(error);
    return {
      ok: false,
      status: 503,
      upstream: "chatgpt_codex",
      content_type: null,
      auth,
      error: "Upstream fetch failed",
      details: detail,
      problems: [`Upstream fetch failed: ${detail || "network error"}`],
    };
  }
};

export const handleHealth = async (): Promise<Response> => {
  const probe = await probeUpstream();

  return json(probe.status, {
    ok: probe.ok,
    problems: probe.problems,
    upstream: probe.upstream,
    status: probe.status,
    content_type: probe.content_type,
    ...(probe.error !== undefined ? { error: probe.error } : {}),
    ...(probe.details !== undefined ? { details: probe.details } : {}),
    auth: probe.auth,
  });
};

export const handleHealthAuth = async (): Promise<Response> => {
  const auth = enrichAuthMeta(await getCodexAuthMeta());
  if (auth.source === "none") {
    return json(503, {
      ok: false,
      upstream: "chatgpt_codex",
      error: AUTH_NOT_CONFIGURED,
      auth,
    });
  }

  return json(200, {
    ok: true,
    upstream: "chatgpt_codex",
    auth,
  });
};

export const handleHealthUpstream = async (): Promise<Response> => {
  const probe = await probeUpstream();
  return json(probe.status, {
    ok: probe.ok,
    upstream: probe.upstream,
    status: probe.status,
    content_type: probe.content_type,
    ...(probe.error !== undefined ? { error: probe.error } : {}),
    ...(probe.details !== undefined ? { details: probe.details } : {}),
    auth: probe.auth,
  });
};
