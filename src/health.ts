import { config, runtimeDeploymentId, runtimeGitSha } from "./config.ts";
import {
  CODEX_AUTH_POOL_KV_KEY,
  fetchCodexModels,
  getJwtExpMs,
  parseCodexAuthFromAuthJson,
  parseCodexAuthPool,
} from "./codex.ts";
import { json } from "./http.ts";
import { getKv } from "./kv.ts";
import {
  getCodexProviderHealth,
  getYunwuProviderHealth,
  PROVIDER_HEALTH_STALE_AFTER_MS,
  type ProviderHealthState,
} from "./provider_health.ts";
import { decodeBase64ToString } from "./utils.ts";
import type { CodexAuthPoolState } from "./types.ts";
import { readYunwuApiKey } from "./yunwu.ts";
import {
  getCachedConfiguredYunwuQuotaSnapshot,
  readYunwuAccountCredentials,
  type YunwuQuotaSnapshot,
} from "./yunwu_quota.ts";

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

type CodexAuthContext = Readonly<{
  meta: HealthAuthMetaBase;
  account_ids: string[];
}>;

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

const loadEnvCodexAuth = (): CodexAuthContext | null => {
  if (!config.codexAuthJsonB64) return null;
  try {
    const decoded = decodeBase64ToString(config.codexAuthJsonB64);
    const parsed = JSON.parse(decoded);
    const auth = parseCodexAuthFromAuthJson(parsed);
    if (!auth) return null;
    const accessTokenExpMs = getJwtExpMs(auth.access_token);
    return {
      meta: {
        source: "env",
        updated_at_ms: null,
        access_token_exp_ms: accessTokenExpMs,
        account_count: 1,
        accounts: [{
          slot: 1,
          updated_at_ms: null,
          access_token_exp_ms: accessTokenExpMs,
        }],
      },
      account_ids: [auth.account_id],
    };
  } catch {
    return null;
  }
};

const getCodexAuthContext = async (): Promise<CodexAuthContext> => {
  const kv = await getKv();
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
        meta: {
          source: "kv",
          updated_at_ms: pool.updated_at_ms,
          access_token_exp_ms: expirations.length > 0 ? Math.min(...expirations) : null,
          account_count: accounts.length,
          accounts,
        },
        account_ids: pool.accounts.map((account) => account.account_id),
      };
    }
  }

  const envMeta = loadEnvCodexAuth();
  if (envMeta) return envMeta;

  return {
    meta: {
      source: "none",
      updated_at_ms: null,
      access_token_exp_ms: null,
      account_count: 0,
      accounts: [],
    },
    account_ids: [],
  };
};

const getCodexAuthMeta = async (): Promise<HealthAuthMetaBase> => (await getCodexAuthContext()).meta;

const aggregateProviderStates = (states: readonly ProviderHealthState[]): ProviderHealthState => {
  if (states.length === 0 || states.every((state) => state === "unknown")) return "unknown";
  if (states.some((state) => state === "healthy")) return "healthy";
  if (states.every((state) => state === "invalid")) return "invalid";
  if (states.every((state) => state === "exhausted")) return "exhausted";
  return "degraded";
};

const quotaView = (snapshot: YunwuQuotaSnapshot | null) =>
  snapshot
    ? {
      available: true,
      cache_state: snapshot.cache_state,
      balance_credits: snapshot.balance_credits,
      baseline_credits: snapshot.baseline_credits,
      remaining_percent: snapshot.remaining_percent,
      used_percent: snapshot.used_percent,
      observed_at_ms: snapshot.state.observed_at_ms,
      confidence: snapshot.state.confidence,
      cycle_started_at_ms: snapshot.state.cycle_started_at_ms,
      last_credit_at_ms: snapshot.state.last_credit_at_ms,
    }
    : {
      available: false,
      cache_state: null,
      balance_credits: null,
      baseline_credits: null,
      remaining_percent: null,
      used_percent: null,
      observed_at_ms: null,
      confidence: null,
      cycle_started_at_ms: null,
      last_credit_at_ms: null,
    };

export const getPassiveProviderHealthSnapshot = async (
  options: Readonly<{ includeQuota?: boolean }> = {},
): Promise<Record<string, unknown>> => {
  const context = await getCodexAuthContext();
  const auth = enrichAuthMeta(context.meta);
  const [codexHealth, yunwuHealth, yunwuQuota] = await Promise.all([
    Promise.all(context.account_ids.map((accountId) => getCodexProviderHealth(accountId))),
    getYunwuProviderHealth(),
    getCachedConfiguredYunwuQuotaSnapshot(),
  ]);
  const codexAccounts = auth.accounts.map((account, index) => ({
    ...account,
    health: codexHealth[index],
  }));
  const quotaMonitoringConfigured = readYunwuAccountCredentials() !== null;
  return {
    mode: "passive",
    generated_at_ms: Date.now(),
    stale_after_ms: PROVIDER_HEALTH_STALE_AFTER_MS,
    codex: {
      configured: auth.source !== "none",
      source: auth.source,
      account_count: auth.account_count,
      state: aggregateProviderStates(codexHealth.map((health) => health.state)),
      accounts: codexAccounts,
    },
    yunwu: {
      configured: readYunwuApiKey() !== null,
      quota_monitoring_configured: quotaMonitoringConfigured,
      health: yunwuHealth,
      ...(options.includeQuota ? { quota: quotaView(yunwuQuota) } : {
        quota: {
          available: yunwuQuota !== null,
          cache_state: yunwuQuota?.cache_state ?? null,
          observed_at_ms: yunwuQuota?.state.observed_at_ms ?? null,
        },
      }),
    },
  };
};

export const handleHealthProviders = async (
  options: Readonly<{ includeQuota?: boolean }> = {},
): Promise<Response> =>
  json(200, await getPassiveProviderHealthSnapshot(options), {
    "Cache-Control": "no-store",
    "x-uos-git-sha": runtimeGitSha(),
    "x-uos-deployment-id": runtimeDeploymentId(),
  });

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
  const gitSha = runtimeGitSha();
  const deploymentId = runtimeDeploymentId();
  console.info(
    "[ai.ubq.fi] health_probe",
    JSON.stringify({
      git_sha: gitSha,
      deno_deployment_id: deploymentId,
      upstream_status: probe.status,
      healthy: probe.ok,
    }),
  );

  return json(probe.status, {
    ok: probe.ok,
    problems: probe.problems,
    upstream: probe.upstream,
    status: probe.status,
    content_type: probe.content_type,
    ...(probe.error !== undefined ? { error: probe.error } : {}),
    ...(probe.details !== undefined ? { details: probe.details } : {}),
    auth: probe.auth,
  }, {
    "x-uos-git-sha": gitSha,
    "x-uos-deployment-id": deploymentId,
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
    }, { "x-uos-git-sha": runtimeGitSha(), "x-uos-deployment-id": runtimeDeploymentId() });
  }

  return json(200, {
    ok: true,
    upstream: "chatgpt_codex",
    auth,
  }, { "x-uos-git-sha": runtimeGitSha(), "x-uos-deployment-id": runtimeDeploymentId() });
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
  }, { "x-uos-git-sha": runtimeGitSha(), "x-uos-deployment-id": runtimeDeploymentId() });
};
