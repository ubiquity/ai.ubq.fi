import { config } from "./config.ts";
import {
  claimCodexRoutingProbe,
  CODEX_ACCOUNT_ROUTING_KV_KEY,
  type CodexProbeCircuit,
  codexQuotaBlockForModel,
  codexQuotaClassForModel,
  getCodexQuotaBlockFence,
  isCodexQuotaBlockFenceCurrent,
  markCodexCredentialInvalid,
  markCodexQuotaBlocked,
  markCodexRecoveryProbeQuotaBlocked,
  markCodexSuccess,
  markCodexUpstreamTimeout,
  parseCodexAccountRoutingState,
  reconcileCodexQuotaAfterStaleVerifiedReset,
  reconcileCodexQuotaAfterVerifiedReset,
  reconcileCodexRoutingAccount,
  releaseCodexRoutingProbe,
  resetCodexAccountRoutingForTest,
  type RoutingAccount,
  selectCodexRoutingAccounts,
  selectCodexRoutingAccountsStrong,
} from "./codex_account_routing.ts";
import {
  type CodexBankedResetConfig,
  type CodexBankedResetDependencies,
  type CodexBankedResetEvent,
  type CodexBankedResetTelemetry,
  type CodexBankedResetTelemetryFields,
  evaluateCodexBankedResetPool,
  loadCodexBankedResetConfig,
  reconcileCodexBankedReset,
  reportCodexBankedResetEvent,
  reportCodexBankedResetMetric,
} from "./codex_banked_reset.ts";
import {
  type CodexUsageResetProvider,
  createUpstreamCodexUsageResetProvider,
  unavailableCodexUsageResetProvider,
} from "./codex_banked_reset_provider.ts";
import {
  type CodexModelsSnapshot,
  mergeCodexModelPromptCacheCapabilities,
  parseCodexClientVersion,
} from "./codex_models.ts";
import { getKv } from "./kv.ts";
import { type ApiKeyProviderDispatch, ApiKeyQuotaDispatchError } from "./api_key_policy.ts";
import { readBoundedResponseBody } from "./bounded_response_body.ts";
import { BUFFERED_INFERENCE_DEADLINE_MS } from "./inference_deadline.ts";
import { recordCodexProviderHealth } from "./provider_health.ts";
import {
  buildRuntimeConfig,
  cacheRuntimeConfig,
  loadRuntimeConfig,
  normalizeRuntimeConfig,
  RUNTIME_CONFIG_V2_KEY,
} from "./runtime_config.ts";
import { recordProviderCapacityDowntimeEvent, recordProviderCapacityResetEvent } from "./provider_capacity_events.ts";
import { base64UrlDecode, decodeBase64ToString, getString, isRecord, sha256Hex } from "./utils.ts";
import type { CodexAuthPoolState, CodexAuthState, ResponseInputItem } from "./types.ts";

const CODEX_REFRESH_CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann";
const CODEX_REFRESH_TOKEN_URL = "https://auth.openai.com/oauth/token";
const CODEX_ORIGINATOR = "codex_cli_rs";
const CODEX_CLIENT_VERSION = "0.100.0";
export const CODEX_QUOTA_BLOCKED_ERROR_CODE = "codex_quota_blocked";
export const CODEX_UPSTREAM_DEGRADED_ERROR_CODE = "codex_upstream_degraded";
export const CODEX_AUTH_REAUTH_WARNING = "codex_auth_reauthentication_required";
export const CODEX_AUTH_REAUTH_MESSAGE =
  "The gateway's Codex auth.json needs re-authentication. Upload a fresh auth.json and retry.";

// Routing errors need to remain distinguishable to the gateway's fallback
// adapter without adding gateway-only headers to OpenAI-compatible responses.
const codexRoutingErrors = new WeakMap<Response, string>();
const codexAuthWarnings = new WeakMap<Response, string>();

export const getCodexRoutingError = (response: Response): string | null => codexRoutingErrors.get(response) ?? null;
export const getCodexAuthWarning = (response: Response): string | null => codexAuthWarnings.get(response) ?? null;

const parseSemverTriplet = (value: string): [number, number, number] | null => {
  const parts = value.trim().split(".");
  if (parts.length < 2 || parts.length > 3) return null;
  const nums = parts.map((part) => Number(part));
  if (nums.some((num) => !Number.isFinite(num) || num < 0)) return null;
  const [major, minor, patch = 0] = nums;
  return [Math.trunc(major), Math.trunc(minor), Math.trunc(patch)];
};

const pickHigherSemver = (a: string | null | undefined, b: string | null | undefined): string | null => {
  const aNorm = typeof a === "string" ? a.trim() : "";
  const bNorm = typeof b === "string" ? b.trim() : "";
  if (!aNorm && !bNorm) return null;
  if (!aNorm) return bNorm || null;
  if (!bNorm) return aNorm || null;

  const aParsed = parseSemverTriplet(aNorm);
  const bParsed = parseSemverTriplet(bNorm);
  if (!aParsed || !bParsed) return aNorm;
  for (let i = 0; i < 3; i++) {
    if (aParsed[i] > bParsed[i]) return aNorm;
    if (aParsed[i] < bParsed[i]) return bNorm;
  }
  return aNorm;
};

const codexUserAgent = (clientVersion?: string | null): string => {
  const version = pickHigherSemver(clientVersion, CODEX_CLIENT_VERSION) ?? CODEX_CLIENT_VERSION;
  return `codex_cli_rs/${version} (ai.ubq.fi)`;
};

export type CodexErrorCode =
  | "codex_auth_missing"
  | "codex_auth_invalid"
  | "codex_auth_refresh_failed"
  | "refresh_token_reused"
  | "codex_auth_refresh_unreachable"
  | "codex_upstream_unreachable"
  | "gateway_timeout";

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

export const CODEX_AUTH_POOL_KV_KEY = ["ubq_ai", "codex_auth"] as const;
export const CODEX_MODELS_KV_KEY = ["ubq_ai", "codex_models"] as const;
export const CODEX_AUTH_REFRESH_LEASE_PREFIX = ["uos_ai", "codex_auth_refresh", "v1"] as const;
export const CODEX_AUTH_CACHE_TTL_MS = 5 * 60_000;
export const CODEX_AUTH_POOL_MAX_ACCOUNTS = 2;
const CODEX_AUTH_REFRESH_LEASE_MS = 15_000;
const CODEX_AUTH_REFRESH_WAIT_MS = 10_000;
export const CODEX_ADDITIONAL_429_RETRY_MAX_DELAY_MS = 2_000;

export type { CodexModelsSnapshot } from "./codex_models.ts";
export { getCodexModelsSnapshotDefaultModel } from "./codex_models.ts";

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

export const parseCodexAuthPool = (value: unknown): CodexAuthPoolState | null => {
  if (!isRecord(value) || !Array.isArray(value.accounts)) return null;
  if (value.accounts.length < 1 || value.accounts.length > CODEX_AUTH_POOL_MAX_ACCOUNTS) return null;
  const updatedAtMs = typeof value.updated_at_ms === "number" && Number.isFinite(value.updated_at_ms)
    ? value.updated_at_ms
    : null;
  if (updatedAtMs === null) return null;

  const accountIds = new Set<string>();
  const accounts: CodexAuthState[] = [];
  for (const candidate of value.accounts) {
    if (!isRecord(candidate)) return null;
    const accessToken = getString(candidate.access_token);
    const refreshToken = getString(candidate.refresh_token);
    const accountId = getString(candidate.account_id);
    const accountUpdatedAtMs = typeof candidate.updated_at_ms === "number" &&
        Number.isFinite(candidate.updated_at_ms)
      ? candidate.updated_at_ms
      : null;
    if (!accessToken || !refreshToken || !accountId || accountUpdatedAtMs === null || accountIds.has(accountId)) {
      return null;
    }
    accountIds.add(accountId);
    accounts.push({
      access_token: accessToken,
      refresh_token: refreshToken,
      account_id: accountId,
      updated_at_ms: accountUpdatedAtMs,
    });
  }

  return { accounts, updated_at_ms: updatedAtMs };
};

export const upsertCodexAuthAccount = (
  pool: CodexAuthPoolState | null,
  auth: CodexAuthState,
): CodexAuthPoolState | null => {
  const accounts = pool ? [...pool.accounts] : [];
  const matchingIndex = accounts.findIndex((candidate) => candidate.account_id === auth.account_id);
  if (matchingIndex >= 0) {
    accounts[matchingIndex] = auth;
  } else if (accounts.length < CODEX_AUTH_POOL_MAX_ACCOUNTS) {
    accounts.push(auth);
  } else {
    return null;
  }
  return { accounts, updated_at_ms: Date.now() };
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

const normalizeCodexAccountEmail = (value: unknown): string | null => {
  if (typeof value !== "string") return null;
  const email = value.trim();
  if (
    email.length === 0 || email.length > 320 ||
    [...email].some((character) => character.charCodeAt(0) < 32 || character.charCodeAt(0) === 127)
  ) return null;
  return email.includes("@") ? email : null;
};

const jwtPayload = (token: string): Record<string, unknown> | null => {
  const payload = token.split(".")[1];
  if (!payload) return null;
  try {
    const parsed: unknown = JSON.parse(new TextDecoder().decode(base64UrlDecode(payload)));
    return isRecord(parsed) ? parsed : null;
  } catch {
    return null;
  }
};

/** Returns the provider email claim without retaining or exposing the token. */
export const getCodexAccountEmail = (accessToken: string): string | null => {
  const payload = jwtPayload(accessToken);
  if (!payload) return null;
  const profile = isRecord(payload["https://api.openai.com/profile"])
    ? payload["https://api.openai.com/profile"]
    : null;
  return normalizeCodexAccountEmail(profile?.email) ?? normalizeCodexAccountEmail(payload.email);
};

const needsRefresh = (auth: CodexAuthState): boolean => {
  const expMs = getJwtExpMs(auth.access_token);
  const now = Date.now();
  if (expMs) return expMs - now < 2 * 60_000;
  return now - auth.updated_at_ms > 7 * 60_000;
};

const accessTokenExpired = (auth: CodexAuthState): boolean => {
  const expMs = getJwtExpMs(auth.access_token);
  return expMs !== null && expMs <= Date.now();
};

const codexAuthWarningForError = (error: unknown): string | null =>
  error instanceof CodexError &&
    (error.code === "codex_auth_invalid" || error.code === "codex_auth_refresh_failed" ||
      error.code === "refresh_token_reused")
    ? CODEX_AUTH_REAUTH_WARNING
    : null;

type CodexAuthPoolEntry = {
  kv: Deno.Kv | null;
  entry: Deno.KvEntryMaybe<CodexAuthPoolState> | null;
  pool: CodexAuthPoolState;
};

type CodexAuthAccountEntry = CodexAuthPoolEntry & {
  auth: CodexAuthState;
  routing?: RoutingAccount;
};

type CodexRefreshLease = Readonly<{
  owner: string;
  lease_until_ms: number;
}>;

let cachedAuthPool: CodexAuthPoolState | null = null;
let cachedAuthPoolExpiresAtMs = 0;
let authCacheGeneration = 0;
let authPoolEntryInFlight: Promise<CodexAuthPoolEntry> | null = null;
const refreshesInFlight = new Map<string, Promise<CodexAuthState>>();
const codexProbeByResponse = new WeakMap<Response, RoutingAccount>();
const codexSlotByResponse = new WeakMap<Response, number>();
const codexAccountIdByResponse = new WeakMap<Response, string>();
const codexTerminalOutcomeByResponse = new WeakSet<Response>();
const codexProbeTransitionsInFlight = new Set<Promise<void>>();

const setCodexResponseAccountTelemetry = (
  response: Response,
  slot: number,
  accountId: string,
): void => {
  codexSlotByResponse.set(response, slot);
  codexAccountIdByResponse.set(response, accountId);
};

const withCodexWarnings = (response: Response, warnings: readonly string[]): Response => {
  const headers = new Headers(response.headers);
  const existing = headers.get("x-uos-warning")?.split(",").map((value) => value.trim()).filter(Boolean) ?? [];
  for (const warning of warnings) {
    if (!existing.includes(warning)) existing.push(warning);
  }
  if (!existing.length) return response;
  headers.set("x-uos-warning", existing.join(", "));
  const decorated = new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
  const routingError = codexRoutingErrors.get(response);
  if (routingError) codexRoutingErrors.set(decorated, routingError);
  const probe = codexProbeByResponse.get(response);
  if (probe) codexProbeByResponse.set(decorated, probe);
  const slot = codexSlotByResponse.get(response);
  if (slot !== undefined) codexSlotByResponse.set(decorated, slot);
  const accountId = codexAccountIdByResponse.get(response);
  if (accountId !== undefined) codexAccountIdByResponse.set(decorated, accountId);
  const authWarning = codexAuthWarnings.get(response);
  if (authWarning !== undefined) codexAuthWarnings.set(decorated, authWarning);
  if (codexTerminalOutcomeByResponse.has(response)) codexTerminalOutcomeByResponse.add(decorated);
  return decorated;
};

const withCodexAuthWarning = (response: Response, warning: string): Response => {
  const decorated = withCodexWarnings(response, [warning]);
  codexAuthWarnings.set(decorated, warning);
  return decorated;
};

/** The metadata stays isolate-local and never becomes a response header or durable credential record. */
export const getCodexRoutingProbe = (response: Response): RoutingAccount | null =>
  codexProbeByResponse.get(response) ?? null;

/** The slot is isolate-local telemetry only; account IDs never leave the routing layer. */
export const getCodexResponseSlot = (response: Response): number | null => codexSlotByResponse.get(response) ?? null;

/** Returns a stable digest while keeping the raw account ID inside this module. */
export const getCodexResponseAccountCohortId = async (response: Response): Promise<string | null> => {
  const accountId = codexAccountIdByResponse.get(response);
  return accountId === undefined ? null : await sha256Hex(`uos-prompt-cache-account-cohort-v1\u0000${accountId}`);
};

const takeCodexResponseProbe = (response: Response): RoutingAccount | null => {
  const probe = codexProbeByResponse.get(response);
  if (probe) codexProbeByResponse.delete(response);
  return probe ?? null;
};

const beginCodexResponseTerminalOutcome = (
  response: Response,
): Readonly<{ accountId: string | null; probe: RoutingAccount | null }> | null => {
  if (codexTerminalOutcomeByResponse.has(response)) return null;
  codexTerminalOutcomeByResponse.add(response);
  return {
    accountId: codexAccountIdByResponse.get(response) ?? null,
    probe: takeCodexResponseProbe(response),
  };
};

const completeCodexProbeTransition = async (transition: Promise<void>): Promise<void> => {
  codexProbeTransitionsInFlight.add(transition);
  try {
    await transition;
  } finally {
    codexProbeTransitionsInFlight.delete(transition);
  }
};

const codexProviderRequestId = (response: Response): string | null =>
  response.headers.get("X-Request-Id") ??
    response.headers.get("X-Api-Request-Id") ??
    response.headers.get("X-Oneapi-Request-Id");

/**
 * Detach a response from its recovery probe without claiming success. Failed,
 * cancelled, or incomplete streams release ordinary routing, but retain the
 * durable ambiguity tombstone until a later recovery probe proves the account
 * healthy.
 */
export const releaseCodexResponseProbe = async (response: Response): Promise<void> => {
  const terminal = beginCodexResponseTerminalOutcome(response);
  if (!terminal?.probe) return;
  await completeCodexProbeTransition(releaseCodexRoutingProbe(terminal.probe));
};

/** Only a validated upstream `response.completed` event may clear the recovery probe. */
export const markCodexResponseCompleted = async (response: Response): Promise<void> => {
  const terminal = beginCodexResponseTerminalOutcome(response);
  if (!terminal) return;
  if (terminal.accountId !== null) {
    void recordCodexProviderHealth(
      terminal.accountId,
      "success",
      response.status,
      Date.now,
      codexProviderRequestId(response),
    ).catch(() => {});
  }
  if (!terminal.probe) return;
  await completeCodexProbeTransition(markCodexSuccess(terminal.probe));
};

/** A trustworthy failure after 2xx headers degrades health without treating cancellation or incompletion as failure. */
export const markCodexResponseUpstreamError = async (response: Response): Promise<void> => {
  const terminal = beginCodexResponseTerminalOutcome(response);
  if (!terminal) return;
  if (response.ok && terminal.accountId !== null) {
    void recordCodexProviderHealth(
      terminal.accountId,
      "upstream_error",
      response.status,
      Date.now,
      codexProviderRequestId(response),
    ).catch(() => {});
  }
  if (!terminal.probe) return;
  await completeCodexProbeTransition(releaseCodexRoutingProbe(terminal.probe));
};

export const cacheCodexAuthPool = (pool: CodexAuthPoolState): void => {
  authCacheGeneration += 1;
  cachedAuthPool = pool;
  cachedAuthPoolExpiresAtMs = Date.now() + CODEX_AUTH_CACHE_TTL_MS;
};

export const resetCodexAuthCacheForTest = (): void => {
  authCacheGeneration += 1;
  cachedAuthPool = null;
  cachedAuthPoolExpiresAtMs = 0;
  authPoolEntryInFlight = null;
  refreshesInFlight.clear();
  codexProbeTransitionsInFlight.clear();
  resetCodexAccountRoutingForTest();
};

const loadAuthSeedFromEnv = (): CodexAuthState => {
  if (config.isDeploy) {
    if (!config.codexAuthJsonB64) {
      throw new CodexError(
        "Codex auth missing: CODEX_AUTH_JSON_B64 unset and no KV entry.",
        "codex_auth_missing",
        503,
      );
    }
  }

  if (config.codexAuthJsonB64) {
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
  }

  if (config.isDeploy) {
    throw new CodexError(
      "Codex auth missing: CODEX_AUTH_JSON_B64 unset and no KV entry.",
      "codex_auth_missing",
      503,
    );
  }

  return loadAuthSeedFromDisk();
};

const loadAuthSeedFromDisk = (): CodexAuthState => {
  if (!config.isDeploy) {
    const home = (Deno as unknown as { homeDir?: () => string | null }).homeDir?.() ?? Deno.env.get("HOME");
    if (!home) {
      throw new CodexError("Could not resolve home directory for ~/.codex/auth.json.", "codex_auth_invalid", 503);
    }
    try {
      const raw = Deno.readTextFileSync(`${home}/.codex/auth.json`);
      const parsed = JSON.parse(raw) as unknown;
      const tokenData = parseCodexAuthFromAuthJson(parsed);
      if (!tokenData) {
        throw new CodexError(
          "Codex auth invalid: ~/.codex/auth.json does not look like a Codex auth.json.",
          "codex_auth_invalid",
          503,
        );
      }
      return { ...tokenData, updated_at_ms: Date.now() };
    } catch (error) {
      if (error instanceof CodexError) throw error;
      throw new CodexError(
        "Codex auth invalid: ~/.codex/auth.json is missing or unreadable.",
        "codex_auth_invalid",
        503,
        error,
      );
    }
  }
  throw new CodexError("Codex auth missing: CODEX_AUTH_JSON_B64 unset and no KV entry.", "codex_auth_missing", 503);
};

const getConfiguredCodexAuthSeed = (): CodexAuthState | null => {
  if (!config.codexAuthJsonB64) {
    return loadAuthSeedFromEnv(); // throws when unavailable in deploy or returns fallback in local
  }
  try {
    return loadAuthSeedFromEnv();
  } catch {
    if (config.isDeploy) return null;
    try {
      return loadAuthSeedFromDisk();
    } catch {
      return null;
    }
  }
};

const poolFromSeed = (auth: CodexAuthState): CodexAuthPoolState => ({
  accounts: [auth],
  updated_at_ms: auth.updated_at_ms,
});

const loadedAuthPoolEntry = (
  pool: CodexAuthPoolState,
  generationAtStart: number,
  kv: Deno.Kv | null,
  entry: Deno.KvEntryMaybe<CodexAuthPoolState> | null,
): CodexAuthPoolEntry => {
  if (authCacheGeneration !== generationAtStart && cachedAuthPool) {
    return { kv: null, entry: null, pool: cachedAuthPool };
  }
  cacheCodexAuthPool(pool);
  return { kv, entry, pool };
};

const loadAuthPoolEntry = async (generationAtStart: number): Promise<CodexAuthPoolEntry> => {
  const kv = await getKv();
  if (!kv) {
    const pool = cachedAuthPool ?? (() => {
      const seed = getConfiguredCodexAuthSeed();
      return seed ? poolFromSeed(seed) : null;
    })();
    if (!pool) {
      throw new CodexError(
        "Codex auth missing: CODEX_AUTH_JSON_B64 unset and no KV entry.",
        "codex_auth_missing",
        503,
      );
    }
    return loadedAuthPoolEntry(pool, generationAtStart, null, null);
  }

  const entry = await kv.get<CodexAuthPoolState>(CODEX_AUTH_POOL_KV_KEY, { consistency: "strong" });
  const storedPool = parseCodexAuthPool(entry.value);
  if (storedPool) {
    // A valid persisted pool is the authority. Local/disk seeds may bootstrap
    // an absent row or run without KV, but must never overwrite or append to
    // credentials an admin has already uploaded.
    return loadedAuthPoolEntry(storedPool, generationAtStart, kv, entry);
  }

  const seed = getConfiguredCodexAuthSeed();
  if (!seed) {
    throw new CodexError(
      "Codex auth missing: CODEX_AUTH_JSON_B64 unset and no KV entry.",
      "codex_auth_missing",
      503,
    );
  }
  if (authCacheGeneration !== generationAtStart && cachedAuthPool) {
    return { kv: null, entry: null, pool: cachedAuthPool };
  }
  const pool = poolFromSeed(seed);
  await kv.set(CODEX_AUTH_POOL_KV_KEY, pool);
  return loadedAuthPoolEntry(pool, generationAtStart, kv, null);
};

const getAuthPoolEntry = async (forceKv = false, bypassInFlight = false): Promise<CodexAuthPoolEntry> => {
  if (!forceKv && cachedAuthPool && Date.now() < cachedAuthPoolExpiresAtMs) {
    return { kv: null, entry: null, pool: cachedAuthPool };
  }

  // A banked-reset retry uses this after a verified, expensive side effect.
  // It must not inherit an in-flight read that began before an auth-pool
  // rotation. A direct strong read is deliberately narrower than the normal
  // warm-cache single-flight path.
  if (bypassInFlight) return await loadAuthPoolEntry(authCacheGeneration);

  // A bounded single read makes credential replacement converge across warm
  // isolates without restoring a KV lookup to every inference request.
  if (authPoolEntryInFlight) return await authPoolEntryInFlight;
  authPoolEntryInFlight = loadAuthPoolEntry(authCacheGeneration).finally(() => {
    authPoolEntryInFlight = null;
  });
  return await authPoolEntryInFlight;
};

/**
 * Control-plane credentials for the redacted admin capacity snapshot. The
 * caller uses these only to make account-bound upstream reads; account IDs
 * and access tokens must never cross the HTTP response boundary.
 */
export type CodexCapacityAccount = Readonly<{
  slot: number;
  account_id: string;
  access_token: string;
  email: string | null;
}>;

export const getCodexCapacityAccounts = async (): Promise<readonly CodexCapacityAccount[]> => {
  const current = await getAuthPoolEntry(true);
  return current.pool.accounts.map((account, index) => ({
    slot: index + 1,
    account_id: account.account_id,
    access_token: account.access_token,
    email: getCodexAccountEmail(account.access_token),
  }));
};

/**
 * Private control-plane state for the fixed prompt-cache experiment. Account
 * identities stay inside this transport module and are never persisted by the
 * experiment coordinator or returned from its admin endpoint.
 */
export type CodexCacheScopeExperimentSession = Readonly<{
  expectedAccountIds: readonly [string, string];
  authPoolVersionstamp: string;
}>;

export class CodexCacheScopeExperimentError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "CodexCacheScopeExperimentError";
  }
}

type CodexCacheScopeExperimentRefreshResult =
  | Readonly<{
    status: "refreshed";
    tokenChanged: boolean;
    session: CodexCacheScopeExperimentSession;
  }>
  | Readonly<{ status: "auth_pool_drift" }>;

const getStrongAuthPoolEntryForCacheScopeExperiment = async (): Promise<
  CodexAuthPoolEntry & { kv: Deno.Kv; entry: Deno.KvEntry<CodexAuthPoolState> }
> => {
  const kv = await getKv();
  if (!kv) {
    throw new CodexCacheScopeExperimentError("Prompt-cache scope experiments require Deno KV-backed Codex auth.");
  }
  const entry = await kv.get<CodexAuthPoolState>(CODEX_AUTH_POOL_KV_KEY, { consistency: "strong" });
  const pool = parseCodexAuthPool(entry.value);
  if (!pool || !entry.versionstamp) {
    throw new CodexCacheScopeExperimentError("Codex auth pool is unavailable for the prompt-cache scope experiment.");
  }
  cacheCodexAuthPool(pool);
  return { kv, entry: entry as Deno.KvEntry<CodexAuthPoolState>, pool };
};

const cacheScopeExperimentAccount = (
  session: CodexCacheScopeExperimentSession,
  poolEntry: CodexAuthPoolEntry & { entry: Deno.KvEntry<CodexAuthPoolState> },
  slot: number,
): CodexAuthState => {
  if (!Number.isInteger(slot) || slot < 1 || slot > CODEX_AUTH_POOL_MAX_ACCOUNTS) {
    throw new CodexCacheScopeExperimentError("Prompt-cache scope experiment slot is invalid.");
  }
  if (poolEntry.entry.versionstamp !== session.authPoolVersionstamp) {
    throw new CodexCacheScopeExperimentError("Codex auth pool changed during the prompt-cache scope experiment.");
  }
  const auth = poolEntry.pool.accounts[slot - 1];
  const expectedAccountId = session.expectedAccountIds[slot - 1];
  if (!auth || auth.account_id !== expectedAccountId) {
    throw new CodexCacheScopeExperimentError("Codex auth pool changed during the prompt-cache scope experiment.");
  }
  return auth;
};

export const beginCodexCacheScopeExperiment = async (): Promise<CodexCacheScopeExperimentSession> => {
  const poolEntry = await getStrongAuthPoolEntryForCacheScopeExperiment();
  const first = poolEntry.pool.accounts[0];
  const second = poolEntry.pool.accounts[1];
  if (poolEntry.pool.accounts.length !== 2 || !first || !second) {
    throw new CodexCacheScopeExperimentError(
      "Prompt-cache scope experiments require exactly two configured Codex slots.",
    );
  }
  return {
    expectedAccountIds: [first.account_id, second.account_id],
    authPoolVersionstamp: poolEntry.entry.versionstamp,
  };
};

/**
 * Refresh one pinned slot exactly once. The returned session carries the new
 * pool versionstamp; callers must use it for every following dispatch.
 */
export const refreshCodexCacheScopeExperimentSlot = async (
  session: CodexCacheScopeExperimentSession,
  slot: number,
  signal?: AbortSignal,
): Promise<CodexCacheScopeExperimentRefreshResult> => {
  const current = await getStrongAuthPoolEntryForCacheScopeExperiment();
  const auth = cacheScopeExperimentAccount(session, current, slot);
  const selected = await selectCodexRoutingAccounts(current.pool, [auth]);
  if (selected.kind !== "eligible" || selected.accounts.length !== 1 || selected.accounts[0]?.slot !== slot - 1) {
    throw new CodexCacheScopeExperimentError(
      "The requested Codex slot is not eligible for OAuth refresh during the prompt-cache scope experiment.",
    );
  }
  let refreshed: CodexAuthState;
  try {
    // Unlike normal inference refreshes, this is a controlled experiment
    // transition. It must not adopt a same-account rotation from another
    // request, because that would falsely make the experiment's refresh row
    // appear to prove a cache scope. Persistence below is fenced to the exact
    // pool version we just validated.
    refreshed = await awaitWithoutCancellingSharedWork(refreshAuthStateless(auth), signal);
  } catch (error) {
    const afterFailure = await getStrongAuthPoolEntryForCacheScopeExperiment().catch(() => null);
    if (afterFailure && afterFailure.entry.versionstamp !== current.entry.versionstamp) {
      return { status: "auth_pool_drift" };
    }
    throw error;
  }
  if (refreshed.account_id !== auth.account_id) {
    throw new CodexCacheScopeExperimentError(
      "Codex OAuth refresh changed account identity during the prompt-cache scope experiment.",
    );
  }

  const accounts = [...current.pool.accounts];
  accounts[slot - 1] = refreshed;
  const nextPool: CodexAuthPoolState = { accounts, updated_at_ms: Date.now() };
  const persisted = await current.kv.atomic()
    .check(current.entry)
    .set(CODEX_AUTH_POOL_KV_KEY, nextPool)
    .commit();
  if (!persisted.ok) return { status: "auth_pool_drift" };

  await reconcileCodexRoutingAccount(selected.accounts[0]!, refreshed);
  const after = await getStrongAuthPoolEntryForCacheScopeExperiment();
  const persistedAuth = after.pool.accounts[slot - 1];
  if (
    after.entry.versionstamp !== persisted.versionstamp ||
    !persistedAuth || persistedAuth.account_id !== refreshed.account_id ||
    !sameCodexCredentials(persistedAuth, refreshed) ||
    after.pool.accounts[0]?.account_id !== session.expectedAccountIds[0] ||
    after.pool.accounts[1]?.account_id !== session.expectedAccountIds[1]
  ) {
    return { status: "auth_pool_drift" };
  }
  return {
    status: "refreshed",
    tokenChanged: persistedAuth.access_token !== auth.access_token ||
      persistedAuth.refresh_token !== auth.refresh_token,
    session: { ...session, authPoolVersionstamp: after.entry.versionstamp },
  };
};

/**
 * Slot-pinned internal transport for the cache-scope experiment. It never
 * tries a sibling account or retries inference. Conversation IDs are internal
 * upstream headers, never public request fields or durable evidence.
 */
export const fetchCodexResponsesForCacheScopeExperiment = async (
  body: unknown,
  options: Readonly<{
    session: CodexCacheScopeExperimentSession;
    slot: number;
    conversationId: string;
    clientVersion?: string | null;
    signal?: AbortSignal;
  }>,
): Promise<Response> => {
  const conversationId = options.conversationId.trim();
  if (!conversationId) {
    throw new CodexCacheScopeExperimentError("Prompt-cache scope experiment conversation id is invalid.");
  }
  if (codexProbeTransitionsInFlight.size) await Promise.allSettled([...codexProbeTransitionsInFlight]);

  const current = await getStrongAuthPoolEntryForCacheScopeExperiment();
  const auth = cacheScopeExperimentAccount(options.session, current, options.slot);
  const requestedModel = isRecord(body) ? getString(body.model) : null;
  const selected = await selectCodexRoutingAccounts(current.pool, [auth], Date.now(), requestedModel);
  if (selected.kind !== "eligible" || selected.accounts.length !== 1) {
    throw new CodexCacheScopeExperimentError(
      "The requested Codex slot is not eligible for the prompt-cache scope experiment.",
    );
  }
  let routing = selected.accounts[0]!;
  if (routing.slot !== options.slot - 1 || routing.auth.account_id !== auth.account_id) {
    throw new CodexCacheScopeExperimentError(
      "Prompt-cache scope experiment routing did not preserve the requested slot.",
    );
  }
  if (routing.probeRequired) {
    const claimed = await claimCodexRoutingProbe(current.pool, routing);
    if (!claimed) {
      throw new CodexCacheScopeExperimentError(
        "The requested Codex slot could not be claimed for the prompt-cache scope experiment.",
      );
    }
    routing = claimed;
  }

  let response: Response | null = null;
  let transportStarted = false;
  const headers = new Headers({
    originator: CODEX_ORIGINATOR,
    "user-agent": codexUserAgent(options.clientVersion),
    "Content-Type": "application/json",
    Accept: "text/event-stream",
    conversation_id: conversationId,
  });
  try {
    const beforeDispatch = await getStrongAuthPoolEntryForCacheScopeExperiment();
    const persistedBeforeDispatch = cacheScopeExperimentAccount(options.session, beforeDispatch, options.slot);
    if (!sameCodexCredentials(persistedBeforeDispatch, routing.auth)) {
      throw new CodexCacheScopeExperimentError(
        "Codex credentials changed before dispatch during the prompt-cache scope experiment.",
      );
    }

    try {
      response = await fetchCodexResponseWithAuth(
        routing.auth,
        `${config.codexBaseUrl}/responses`,
        JSON.stringify(body),
        headers,
        options.signal,
        undefined,
        () => {
          transportStarted = true;
        },
      );
    } catch (error) {
      void recordCodexThrownHealth(routing.auth.account_id, error);
      logCodexRouting("codex_attempt", {
        request_id: null,
        attempt: 1,
        slot: routing.slot + 1,
        phase: "initial",
        status: error instanceof CodexError ? error.status : null,
        status_class: codexErrorClass(error),
      });
      throw error;
    }
    setCodexResponseAccountTelemetry(response, options.slot, routing.auth.account_id);
    void recordCodexResponseHealth(routing.auth.account_id, response, routing.auth);
    logCodexRouting("codex_attempt", {
      request_id: null,
      attempt: 1,
      slot: routing.slot + 1,
      phase: "initial",
      status: response.status,
      status_class: codexStatusClass(response.status),
    });

    if (response.status === 429) {
      response = (await markCodexQuotaBlocked(routing, response)).response;
      setCodexResponseAccountTelemetry(response, options.slot, routing.auth.account_id);
    }
    if (!response.ok) {
      await releaseCodexRoutingProbe(routing);
    } else if (routing.probeGeneration !== null) {
      codexProbeByResponse.set(response, routing);
    }

    const afterDispatch = await getStrongAuthPoolEntryForCacheScopeExperiment();
    const persistedAfterDispatch = cacheScopeExperimentAccount(options.session, afterDispatch, options.slot);
    if (!sameCodexCredentials(persistedAfterDispatch, routing.auth)) {
      throw new CodexCacheScopeExperimentError(
        "Codex credentials changed after dispatch during the prompt-cache scope experiment.",
      );
    }
  } catch (error) {
    if (response) cancelResponseBody(response);
    if (transportStarted && error instanceof CodexError && error.code === "gateway_timeout") {
      await markCodexUpstreamTimeout(routing);
    } else {
      await releaseCodexRoutingProbe(routing);
    }
    throw error;
  }
  return response!;
};

const getCurrentAccountEntry = async (
  accountId: string,
  forceKv: boolean,
): Promise<CodexAuthAccountEntry> => {
  const poolEntry = await getAuthPoolEntry(forceKv);
  const auth = poolEntry.pool.accounts.find((candidate) => candidate.account_id === accountId);
  if (!auth) {
    throw new CodexError("Codex auth account is no longer configured.", "codex_auth_missing", 503);
  }
  return { ...poolEntry, auth };
};

const refreshFailureStatus = (status: number): number => status === 400 || status === 401 || status === 403 ? 401 : 503;

type CodexRefreshFailure = Readonly<{
  message: string;
  code: Extract<CodexErrorCode, "codex_auth_refresh_failed" | "refresh_token_reused">;
  status: number;
}>;

const classifyCodexRefreshFailure = async (response: Response): Promise<CodexRefreshFailure> => {
  const bounded = await readBoundedResponseBody(response, {
    maxBytes: 16 * 1024,
    timeoutMs: 1_000,
    cancellationReason: "Codex auth refresh error body discarded",
  });
  const values: string[] = [];
  if (bounded.complete && bounded.bytes.byteLength) {
    try {
      const parsed = JSON.parse(new TextDecoder().decode(bounded.bytes)) as unknown;
      if (isRecord(parsed)) {
        const error = parsed.error;
        if (typeof error === "string") values.push(error);
        if (isRecord(error)) {
          for (const key of ["code", "message", "type"] as const) {
            const value = getString(error[key]);
            if (value) values.push(value);
          }
        }
        for (const key of ["error_description", "detail", "message", "code", "type"] as const) {
          const value = getString(parsed[key]);
          if (value) values.push(value);
        }
      }
    } catch {
      // OAuth error bodies are advisory only; the status remains authoritative.
    }
  }
  const detail = values.join(" ").toLowerCase();
  const refreshTokenReused = values.some((value) => value.trim().toLowerCase() === "refresh_token_reused") ||
    /refresh token.{0,80}(already|previously) used|token.{0,40}reused/.test(detail);
  const invalidGrant = values.some((value) => value.trim().toLowerCase() === "invalid_grant") ||
    /refresh token.{0,80}(expired|invalid|revoked)|authorization grant.{0,80}(expired|invalid|revoked)/.test(detail);
  if (refreshTokenReused) {
    return {
      message:
        "The gateway's Codex refresh token was already used. Sign in again or upload a fresh auth.json and retry.",
      code: "refresh_token_reused",
      status: refreshFailureStatus(response.status),
    };
  }
  if (invalidGrant || response.status === 400 || response.status === 401 || response.status === 403) {
    return {
      message: `${CODEX_AUTH_REAUTH_MESSAGE} The provider rejected the configured refresh token.`,
      code: "codex_auth_refresh_failed",
      status: refreshFailureStatus(response.status),
    };
  }
  return {
    message: `Codex auth refresh failed (status ${response.status}).`,
    code: "codex_auth_refresh_failed",
    status: 503,
  };
};

const refreshAuth = async (
  current: CodexAuthAccountEntry,
): Promise<CodexAuthState> => {
  const generationAtStart = authCacheGeneration;
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
      signal: AbortSignal.timeout(10_000),
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
    const failure = await classifyCodexRefreshFailure(response);
    throw new CodexError(
      failure.message,
      failure.code,
      failure.status,
    );
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

  const next: CodexAuthState = {
    access_token,
    refresh_token: refresh_token ?? current.auth.refresh_token,
    account_id: current.auth.account_id,
    updated_at_ms: Date.now(),
  };

  if (authCacheGeneration !== generationAtStart && cachedAuthPool) {
    const cached = cachedAuthPool.accounts.find((candidate) => candidate.account_id === current.auth.account_id);
    if (
      cached &&
      (cached.access_token !== current.auth.access_token || cached.refresh_token !== current.auth.refresh_token)
    ) {
      return cached;
    }
  }

  if (current.kv) {
    let poolEntry = current.entry;
    for (let attempt = 0; attempt < 3; attempt += 1) {
      poolEntry ??= await current.kv.get<CodexAuthPoolState>(CODEX_AUTH_POOL_KV_KEY, { consistency: "strong" });
      const latestPool = parseCodexAuthPool(poolEntry.value);
      const latestIndex = latestPool?.accounts.findIndex((candidate) =>
        candidate.account_id === current.auth.account_id
      ) ?? -1;
      if (!latestPool || latestIndex < 0) {
        throw new CodexError("Codex auth account disappeared during refresh.", "codex_auth_missing", 503);
      }
      const latestAuth = latestPool.accounts[latestIndex];
      if (
        latestAuth.access_token !== current.auth.access_token ||
        latestAuth.refresh_token !== current.auth.refresh_token
      ) {
        cacheCodexAuthPool(latestPool);
        return latestAuth;
      }

      const accounts = [...latestPool.accounts];
      accounts[latestIndex] = next;
      const nextPool: CodexAuthPoolState = { accounts, updated_at_ms: Date.now() };
      const commit = await current.kv.atomic()
        .check(poolEntry)
        .set(CODEX_AUTH_POOL_KV_KEY, nextPool)
        .commit();
      if (commit.ok) {
        cacheCodexAuthPool(nextPool);
        return next;
      }
      poolEntry = await current.kv.get<CodexAuthPoolState>(CODEX_AUTH_POOL_KV_KEY, { consistency: "strong" });
    }
    throw new CodexError(
      "Codex auth refresh could not persist after concurrent updates.",
      "codex_auth_refresh_failed",
      503,
    );
  }

  const basePool = cachedAuthPool ?? current.pool;
  const accounts = basePool.accounts.map((candidate) =>
    candidate.account_id === current.auth.account_id ? next : candidate
  );
  cacheCodexAuthPool({ accounts, updated_at_ms: Date.now() });
  return next;
};

const refreshAuthWithHealth = async (
  current: CodexAuthAccountEntry,
): Promise<CodexAuthState> => {
  try {
    const refreshed = await refreshAuth(current);
    void recordCodexProviderHealth(current.auth.account_id, "refresh_success", 200);
    return refreshed;
  } catch (error) {
    const status = error instanceof CodexError ? error.status : null;
    void recordCodexProviderHealth(current.auth.account_id, "refresh_failed", status);
    throw error;
  }
};

const sameCodexCredentials = (left: CodexAuthState, right: CodexAuthState): boolean =>
  left.access_token === right.access_token && left.refresh_token === right.refresh_token;

const refreshLeaseKey = async (accountId: string): Promise<Deno.KvKey> => [
  ...CODEX_AUTH_REFRESH_LEASE_PREFIX,
  await sha256Hex(accountId),
];

const delay = (milliseconds: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, milliseconds));

const releaseRefreshLease = async (kv: Deno.Kv, key: Deno.KvKey, owner: string): Promise<void> => {
  try {
    const entry = await kv.get<CodexRefreshLease>(key, { consistency: "strong" });
    if (entry.value?.owner !== owner) return;
    await kv.atomic().check(entry).delete(key).commit();
  } catch {
    // Leases expire on their own; a release failure must not mask refresh work.
  }
};

/**
 * Coalesce refreshes across isolates. A waiter repeatedly force-reads the
 * pool and adopts a newer token instead of issuing OAuth itself. The lease
 * contains only a hash-derived slot key and an opaque owner token.
 */
const refreshAuthCoordinated = async (input: CodexAuthAccountEntry): Promise<CodexAuthState> => {
  let current = input;
  try {
    const newest = await getCurrentAccountEntry(input.auth.account_id, true);
    if (!sameCodexCredentials(newest.auth, input.auth)) return newest.auth;
    current = newest;
  } catch {
    // Continue with the supplied in-memory account when the optional
    // cross-isolate read is unavailable; refreshAuth still persists safely.
  }

  const kv = current.kv;
  if (!kv) return await refreshAuthWithHealth(current);
  let key: Deno.KvKey;
  try {
    key = await refreshLeaseKey(current.auth.account_id);
  } catch {
    return await refreshAuthWithHealth(current);
  }

  // Lease reads/CAS operations are an optional coordination optimization. A
  // failure here must fall open to a bounded direct refresh, but OAuth must
  // execute outside this catch so its deterministic errors are never retried.
  let ownedLease: { owner: string } | null = null;
  try {
    for (let attempt = 0; attempt < 3; attempt += 1) {
      const leaseEntry = await kv.get<CodexRefreshLease>(key, { consistency: "strong" });
      const lease = leaseEntry.value;
      const now = Date.now();
      if (!lease || lease.lease_until_ms <= now) {
        const owner = crypto.randomUUID();
        const claimed = await kv.atomic()
          .check(leaseEntry)
          .set(
            key,
            { owner, lease_until_ms: now + CODEX_AUTH_REFRESH_LEASE_MS } satisfies CodexRefreshLease,
            { expireIn: CODEX_AUTH_REFRESH_LEASE_MS * 2 },
          )
          .commit();
        if (!claimed.ok) continue;
        ownedLease = { owner };
        break;
      }

      // Do not bind the shared lease wait to an individual request signal.
      // Callers race the resulting shared promise independently, so one aborted
      // client cannot cancel a refresh another client still needs.
      let waitedMs = 0;
      let stepMs = 50;
      const maxWaitMs = Math.min(CODEX_AUTH_REFRESH_WAIT_MS, Math.max(0, lease.lease_until_ms - now));
      while (waitedMs < maxWaitMs) {
        await delay(stepMs);
        waitedMs += stepMs;
        stepMs = Math.min(500, stepMs * 2);
        try {
          const newest = await getCurrentAccountEntry(current.auth.account_id, true);
          if (!sameCodexCredentials(newest.auth, current.auth)) return newest.auth;
          current = newest;
        } catch {
          // The current owner may still persist a refreshed token. Keep waiting
          // until the bounded lease observation window elapses.
        }
        const currentLease = await kv.get<CodexRefreshLease>(key, { consistency: "strong" });
        if (!currentLease.value || currentLease.value.lease_until_ms <= Date.now()) {
          // A lease owner may have persisted rotating credentials immediately
          // before releasing its lease. Re-read once at that handoff so this
          // waiter never refreshes an obsolete token.
          try {
            const newest = await getCurrentAccountEntry(current.auth.account_id, true);
            if (!sameCodexCredentials(newest.auth, current.auth)) return newest.auth;
            current = newest;
          } catch {
            // The normal fail-open refresh below remains available if KV is
            // transiently unavailable at the handoff.
          }
          break;
        }
      }
      if (ownedLease) break;
    }
  } catch {
    return await refreshAuthWithHealth(current);
  }

  if (ownedLease) {
    try {
      return await refreshAuthWithHealth(current);
    } finally {
      await releaseRefreshLease(kv, key, ownedLease.owner);
    }
  }

  // No usable cross-isolate lease was observed or claimed. The refresh itself
  // remains generation-aware and bounded.
  return await refreshAuthWithHealth(current);
};

const awaitWithoutCancellingSharedWork = async <T>(promise: Promise<T>, signal?: AbortSignal): Promise<T> => {
  if (!signal) return await promise;
  if (signal.aborted) throw signal.reason ?? new DOMException("The request was aborted.", "AbortError");
  return await Promise.race([
    promise,
    new Promise<never>((_, reject) => {
      const onAbort = () => {
        signal.removeEventListener("abort", onAbort);
        reject(signal.reason ?? new DOMException("The request was aborted.", "AbortError"));
      };
      signal.addEventListener("abort", onAbort, { once: true });
      promise.finally(() => signal.removeEventListener("abort", onAbort)).catch(() => {});
    }),
  ]);
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
      signal: AbortSignal.timeout(10_000),
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
    const failure = await classifyCodexRefreshFailure(response);
    throw new CodexError(
      failure.message,
      failure.code,
      failure.status,
    );
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
  | { ok: true; accounts: CodexAuthState[] }
  | { ok: false; status: number; code: CodexErrorCode; error: string };

export const checkCodexAuthRefresh = async (): Promise<CodexAuthRefreshResult> => {
  try {
    const current = await getAuthPoolEntry();
    const accounts: CodexAuthState[] = [];
    for (const auth of current.pool.accounts) {
      accounts.push(await refreshAuthCoordinated({ ...current, auth }));
    }
    return { ok: true, accounts };
  } catch (error) {
    if (error instanceof CodexError) {
      return { ok: false, status: error.status, code: error.code, error: error.message };
    }
    const detail = error instanceof Error ? error.message : String(error);
    return { ok: false, status: 503, code: "codex_auth_refresh_failed", error: detail };
  }
};

const getValidAuth = async (current: CodexAuthAccountEntry): Promise<CodexAuthState> => {
  if (!needsRefresh(current.auth)) return current.auth;

  const existing = refreshesInFlight.get(current.auth.account_id);
  if (existing) return await existing;
  const refresh = refreshAuthCoordinated(current).finally(() => {
    refreshesInFlight.delete(current.auth.account_id);
  });
  refreshesInFlight.set(current.auth.account_id, refresh);
  return await refresh;
};

const codexModelsBaseUrls = (clientVersion: string | null): string[] => {
  const base = config.codexBaseUrl.replace(/\/+$/, "");
  const urls = new Set<string>();

  // Prefer the Codex-specific models endpoint when available. It requires `client_version`.
  if (base.endsWith("/codex")) {
    const codexUrl = new URL(`${base}/models`);
    if (clientVersion) codexUrl.searchParams.set("client_version", clientVersion);
    urls.add(codexUrl.toString());
  } else {
    urls.add(`${base}/models`);
  }

  return Array.from(urls);
};

const fetchCodexModelsWithAuth = async (
  auth: CodexAuthState,
  url: string,
  clientVersion: string | null,
  ifNoneMatch?: string | null,
  signal?: AbortSignal,
): Promise<Response> => {
  const headers = new Headers();
  headers.set("Authorization", `Bearer ${auth.access_token}`);
  headers.set("ChatGPT-Account-ID", auth.account_id);
  headers.set("originator", CODEX_ORIGINATOR);
  headers.set("user-agent", `codex_cli_rs/${clientVersion ?? CODEX_CLIENT_VERSION} (ai.ubq.fi)`);
  headers.set("Accept", "application/json");
  if (ifNoneMatch) headers.set("If-None-Match", ifNoneMatch);

  try {
    return await fetch(url, {
      method: "GET",
      headers,
      redirect: "manual",
      signal,
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

export const orderCodexAuthAccounts = (
  accounts: readonly CodexAuthState[],
  startIndex: number,
): CodexAuthState[] => {
  if (accounts.length === 0) return [];
  const normalizedStart = ((Math.trunc(startIndex) % accounts.length) + accounts.length) % accounts.length;
  return accounts.map((_, offset) => accounts[(normalizedStart + offset) % accounts.length]);
};

const randomizedAuthEntries = (poolEntry: CodexAuthPoolEntry): CodexAuthAccountEntry[] => {
  const entropy = crypto.getRandomValues(new Uint8Array(1))[0];
  return orderCodexAuthAccounts(poolEntry.pool.accounts, entropy).map((auth) => ({ ...poolEntry, auth }));
};

const cancelResponseBody = (response: Response): void => {
  try {
    const cancellation = response.body?.cancel();
    if (cancellation) void cancellation.catch(() => {});
  } catch {
    // Best effort before retrying another account.
  }
};

const recordCodexResponseHealth = async (
  accountId: string,
  response: Response,
  auth?: CodexAuthState,
  successfulResponseEvent: "success" | "reachable" | null = null,
): Promise<void> => {
  const providerRequestId = response.headers.get("X-Request-Id") ??
    response.headers.get("X-Api-Request-Id") ??
    response.headers.get("X-Oneapi-Request-Id");
  if (response.status === 401 || (response.status === 403 && auth !== undefined && accessTokenExpired(auth))) {
    await recordCodexProviderHealth(accountId, "auth_invalid", response.status, Date.now, providerRequestId);
  } else if (response.status === 429) {
    await recordCodexProviderHealth(accountId, "quota_exhausted", response.status, Date.now, providerRequestId);
  } else if (response.status >= 500) {
    void recordProviderCapacityDowntimeEvent({
      failure_kind: "upstream_error",
      status: response.status,
      observed_at_ms: Date.now(),
    });
    await recordCodexProviderHealth(accountId, "upstream_error", response.status, Date.now, providerRequestId);
  } else if (response.ok && successfulResponseEvent !== null) {
    await recordCodexProviderHealth(
      accountId,
      successfulResponseEvent,
      response.status,
      Date.now,
      providerRequestId,
    );
  } else if (!response.ok) {
    await recordCodexProviderHealth(accountId, "reachable", response.status, Date.now, providerRequestId);
  }
};

const recordCodexThrownHealth = async (accountId: string, error: unknown): Promise<void> => {
  if (
    error instanceof CodexError &&
    (error.code === "codex_auth_refresh_failed" || error.code === "refresh_token_reused" ||
      error.code === "codex_auth_refresh_unreachable")
  ) {
    return;
  }
  const isProviderTransportFailure = error instanceof CodexError &&
    (error.code === "gateway_timeout" || error.code === "codex_upstream_unreachable");
  await recordCodexProviderHealth(accountId, "upstream_error", null);
  if (isProviderTransportFailure) {
    void recordProviderCapacityDowntimeEvent({
      failure_kind: "unreachable",
      status: error instanceof CodexError && error.status >= 500 && error.status <= 599 ? error.status : null,
      observed_at_ms: Date.now(),
    });
  }
};

/**
 * A post-reset retry is fenced more strictly than normal account routing. If
 * the auth-pool slot changes in the tiny interval before transport, preserve
 * the ordinary quota response instead of dispatching stale credentials.
 */
class CodexBankedResetRetryFenceError extends Error {
  constructor() {
    super("Codex banked-reset retry was fenced by an auth-pool change.");
    this.name = "CodexBankedResetRetryFenceError";
  }
}

const fetchCodexResponseWithAuth = async (
  auth: CodexAuthState,
  url: string,
  serializedBody: string,
  baseHeaders: Headers,
  signal?: AbortSignal,
  beforeDispatch?: () => Promise<ApiKeyProviderDispatch | void>,
  onDispatch?: () => void,
): Promise<Response> => {
  const headers = new Headers(baseHeaders);
  headers.set("Authorization", `Bearer ${auth.access_token}`);
  headers.set("ChatGPT-Account-ID", auth.account_id);
  const deadline = new AbortController();
  const deadlineTimer = setTimeout(
    () => deadline.abort(new DOMException("Codex response headers timed out.", "TimeoutError")),
    BUFFERED_INFERENCE_DEADLINE_MS,
  );
  const transportSignal = signal ? AbortSignal.any([signal, deadline.signal]) : deadline.signal;
  try {
    // This is intentionally immediately adjacent to the actual provider
    // transport: request-quota reservations commit on dispatch, not on
    // validation, routing, or a later retry outcome.
    const dispatch = beforeDispatch ? await beforeDispatch() : undefined;
    if (transportSignal.aborted) {
      await dispatch?.cancelBeforeTransport();
      throw transportSignal.reason ?? new DOMException("The request was aborted.", "AbortError");
    }
    dispatch?.markTransportStarted();
    onDispatch?.();
    return await fetch(url, {
      method: "POST",
      headers,
      body: serializedBody,
      redirect: "manual",
      signal: transportSignal,
    });
  } catch (error) {
    if (error instanceof ApiKeyQuotaDispatchError) throw error;
    if (error instanceof CodexBankedResetRetryFenceError) throw error;
    const timedOut = deadline.signal.aborted ||
      (signal?.aborted && signal.reason instanceof Error && signal.reason.name === "TimeoutError");
    if (timedOut) {
      throw new CodexError(
        "Codex upstream exceeded the gateway deadline before response headers were received.",
        "gateway_timeout",
        504,
        error,
      );
    }
    if (signal?.aborted) throw signal.reason ?? error;
    throw new CodexError(
      "Codex upstream request failed: upstream unreachable.",
      "codex_upstream_unreachable",
      502,
      error,
    );
  } finally {
    clearTimeout(deadlineTimer);
  }
};

const routingErrorResponse = (
  status: 401 | 429 | 503,
  message: string,
  code: string,
  retryAtMs: number | null = null,
): Response => {
  const headers = new Headers({ "Content-Type": "application/json" });
  if ((status === 429 || status === 503) && retryAtMs !== null) {
    headers.set("Retry-After", String(Math.max(1, Math.ceil((retryAtMs - Date.now()) / 1000))));
  }
  const response = new Response(
    JSON.stringify({
      error: {
        message,
        type: status === 429 ? "rate_limit_error" : status >= 500 ? "server_error" : "invalid_request_error",
        code,
        param: null,
      },
    }),
    { status, headers },
  );
  if (code === CODEX_QUOTA_BLOCKED_ERROR_CODE || code === CODEX_UPSTREAM_DEGRADED_ERROR_CODE) {
    codexRoutingErrors.set(response, code);
  }
  return response;
};

const upstreamTimeoutCircuitResponse = (retryAtMs: number | null): Response =>
  routingErrorResponse(
    503,
    "Codex upstream is temporarily unavailable after response-header timeouts; retry later.",
    CODEX_UPSTREAM_DEGRADED_ERROR_CODE,
    retryAtMs,
  );

type CodexResponseTimingHooks = Readonly<{
  onDispatch?: () => void;
  onHeaders?: () => void;
}>;

type FetchCodexResponsesOptions = Readonly<{
  clientVersion?: string | null;
  cacheScope?: string | null;
  signal?: AbortSignal;
  requestId?: string | null;
  timing?: CodexResponseTimingHooks;
  retrySleep?: (milliseconds: number) => Promise<void>;
  beforeDispatch?: () => Promise<ApiKeyProviderDispatch | void>;
  bankedReset?: CodexBankedResetOptions;
}>;

type PreparedCodexSubscriptionRequest = Readonly<{
  body: unknown;
  serializedBody: string;
  conversationIdentity: string;
  nativeSessionIdentity: string | null;
  warnings: readonly string[];
}>;

const CODEX_PROMPT_CACHE_OPTIONS_IGNORED_WARNING = "prompt_cache_options_ignored";
const CODEX_PROMPT_CACHE_RETENTION_IGNORED_WARNING = "prompt_cache_retention_ignored";
const CODEX_PROMPT_CACHE_BREAKPOINT_IGNORED_WARNING = "prompt_cache_breakpoint_ignored";
const CODEX_MAX_OUTPUT_TOKENS_IGNORED_WARNING = "max_output_tokens_ignored";

const deterministicCodexSessionIdentity = async (
  cacheScope: string,
  promptCacheKey: string,
): Promise<string> => {
  const digest = await sha256Hex(`uos-codex-prompt-cache-session-v2\u0000${cacheScope}\u0000${promptCacheKey}`);
  const variant = ((Number.parseInt(digest[16]!, 16) & 0b0011) | 0b1000).toString(16);
  return `${digest.slice(0, 8)}-${digest.slice(8, 12)}-8${digest.slice(13, 16)}-${variant}${digest.slice(17, 20)}-${
    digest.slice(20, 32)
  }`;
};

const stripCodexPromptCacheBreakpoints = (value: unknown): Readonly<{ value: unknown; removed: boolean }> => {
  if (Array.isArray(value)) {
    let removed = false;
    const next = value.map((item) => {
      const stripped = stripCodexPromptCacheBreakpoints(item);
      removed ||= stripped.removed;
      return stripped.value;
    });
    return removed ? { value: next, removed: true } : { value, removed: false };
  }
  if (!isRecord(value)) return { value, removed: false };

  let removed = false;
  let next: Record<string, unknown> | null = null;
  if (Object.prototype.hasOwnProperty.call(value, "prompt_cache_breakpoint")) {
    next = { ...value };
    delete next.prompt_cache_breakpoint;
    removed = true;
  }
  for (const key of ["content", "output"] as const) {
    if (!Object.prototype.hasOwnProperty.call(value, key)) continue;
    const stripped = stripCodexPromptCacheBreakpoints(value[key]);
    if (!stripped.removed) continue;
    next ??= { ...value };
    next[key] = stripped.value;
    removed = true;
  }
  return removed ? { value: next!, removed: true } : { value, removed: false };
};

const prepareCodexSubscriptionRequest = async (
  body: unknown,
  cacheScope: string | null,
): Promise<PreparedCodexSubscriptionRequest> => {
  const warnings: string[] = [];
  let preparedBody = body;
  let promptCacheKey: string | null = null;

  if (isRecord(body)) {
    const prepared: Record<string, unknown> = { ...body };
    promptCacheKey = typeof body.prompt_cache_key === "string" && body.prompt_cache_key.trim().length > 0
      ? body.prompt_cache_key
      : null;
    for (
      const [field, warning] of [
        ["prompt_cache_options", CODEX_PROMPT_CACHE_OPTIONS_IGNORED_WARNING],
        ["prompt_cache_retention", CODEX_PROMPT_CACHE_RETENTION_IGNORED_WARNING],
        ["max_output_tokens", CODEX_MAX_OUTPUT_TOKENS_IGNORED_WARNING],
        ["max_completion_tokens", CODEX_MAX_OUTPUT_TOKENS_IGNORED_WARNING],
      ] as const
    ) {
      if (!Object.prototype.hasOwnProperty.call(prepared, field)) continue;
      delete prepared[field];
      if (!warnings.includes(warning)) warnings.push(warning);
    }
    if (Object.prototype.hasOwnProperty.call(prepared, "input")) {
      const stripped = stripCodexPromptCacheBreakpoints(prepared.input);
      prepared.input = stripped.value;
      if (stripped.removed) warnings.push(CODEX_PROMPT_CACHE_BREAKPOINT_IGNORED_WARNING);
    }
    preparedBody = prepared;
  }

  const nativeSessionIdentity = promptCacheKey === null || cacheScope === null || cacheScope.length === 0
    ? null
    : await deterministicCodexSessionIdentity(cacheScope, promptCacheKey);
  return {
    body: preparedBody,
    serializedBody: JSON.stringify(preparedBody),
    conversationIdentity: nativeSessionIdentity ?? crypto.randomUUID(),
    nativeSessionIdentity,
    warnings,
  };
};

type CodexAttemptPhase = "initial" | "post_refresh" | "two_second_retry" | "post_retry_refresh" | "post_banked_reset";

type CodexBankedResetOptions = Readonly<{
  /** Test seam; normal traffic creates an account-bound upstream adapter only for a live reset candidate. */
  config?: CodexBankedResetConfig;
  /** Test seam for proving a live configuration change stops a pending submission. */
  reloadConfig?: () => CodexBankedResetConfig;
  provider?: CodexUsageResetProvider;
  kv?: Deno.Kv | null;
  now?: () => number;
  newOwnerToken?: () => string;
  hash?: (value: string) => Promise<string>;
  telemetry?: CodexBankedResetTelemetry;
}>;

type CodexBankedResetCandidate = Readonly<{
  accountEntry: CodexAuthAccountEntry;
  auth: CodexAuthState;
  routing: RoutingAccount;
  quotaResetAtMs: number;
  routingGeneration: number;
}>;

const reportCodexResponseTiming = (callback: (() => void) | undefined): void => {
  try {
    callback?.();
  } catch {
    // Observability must not affect inference routing or delivery.
  }
};

const codexStatusClass = (status: number): string => {
  if (status >= 200 && status < 300) return "2xx";
  if (status === 401) return "401";
  if (status === 403) return "403";
  if (status === 429) return "429";
  if (status >= 400 && status < 500) return "invalid_request_4xx";
  if (status >= 500 && status < 600) return "5xx";
  return "other_http";
};

const codexErrorClass = (error: unknown): string => {
  if (error instanceof CodexError) {
    if (error.code === "gateway_timeout") return "timeout";
    if (error.code === "codex_upstream_unreachable" || error.code === "codex_auth_refresh_unreachable") {
      return "network_failure";
    }
    return codexStatusClass(error.status);
  }
  return error instanceof Error && error.name === "TimeoutError" ? "timeout" : "network_failure";
};

const logCodexRouting = (
  event:
    | "codex_attempt"
    | "codex_banked_reset_preflight"
    | "codex_quota_classification"
    | "codex_token_refresh"
    | "codex_two_second_retry",
  fields: Readonly<Record<string, string | number | null>>,
): void => {
  try {
    console.info("[ai.ubq.fi] codex_routing", JSON.stringify({ event, ...fields }));
  } catch {
    // Routing telemetry must never alter provider selection.
  }
};

const waitForCodexRetry = async (
  milliseconds: number,
  signal: AbortSignal | undefined,
  sleep: (milliseconds: number) => Promise<void>,
): Promise<void> => {
  const abortReason = (): unknown => {
    const reason = signal?.reason ?? new DOMException("The request was aborted.", "AbortError");
    if (reason instanceof Error && reason.name === "TimeoutError") {
      return new CodexError(
        "Codex upstream exceeded the gateway deadline while waiting to retry.",
        "gateway_timeout",
        504,
        reason,
      );
    }
    return reason;
  };

  if (signal?.aborted) throw abortReason();
  if (milliseconds <= 0) return;
  if (!signal) {
    await sleep(milliseconds);
    return;
  }
  let onAbort = (): void => {};
  try {
    const aborted = new Promise<never>((_, reject) => {
      onAbort = () => reject(abortReason());
      signal.addEventListener("abort", onAbort, { once: true });
      if (signal.aborted) onAbort();
    });
    await Promise.race([
      sleep(milliseconds),
      aborted,
    ]);
  } finally {
    signal.removeEventListener("abort", onAbort);
  }
};

const fetchPreparedCodexResponses = async (
  prepared: PreparedCodexSubscriptionRequest,
  options: FetchCodexResponsesOptions,
): Promise<Response> => {
  if (codexProbeTransitionsInFlight.size) {
    await Promise.allSettled([...codexProbeTransitionsInFlight]);
  }
  const body = prepared.body;
  const requestedModel = isRecord(body) ? getString(body.model) : null;
  let poolEntry = await getAuthPoolEntry();
  let selected = await selectCodexRoutingAccounts(
    poolEntry.pool,
    poolEntry.pool.accounts,
    Date.now(),
    requestedModel,
  );
  if (selected.kind === "credentials_invalid") {
    return withCodexAuthWarning(
      routingErrorResponse(
        401,
        `${CODEX_AUTH_REAUTH_MESSAGE} All configured Codex credentials are invalid.`,
        "codex_auth_invalid",
      ),
      CODEX_AUTH_REAUTH_WARNING,
    );
  }
  if (selected.kind === "upstream_blocked") return upstreamTimeoutCircuitResponse(selected.retryAtMs);
  let accountEntries = selected.kind === "eligible"
    ? selected.accounts.map((routing) => ({ ...poolEntry, auth: routing.auth, routing }))
    : [];
  const url = `${config.codexBaseUrl}/responses`;
  const serializedBody = prepared.serializedBody;
  const baseHeaders = new Headers({
    "originator": CODEX_ORIGINATOR,
    "user-agent": codexUserAgent(options.clientVersion),
    "Content-Type": "application/json",
    "Accept": "text/event-stream",
    "conversation_id": prepared.conversationIdentity,
  });
  if (prepared.nativeSessionIdentity !== null) {
    baseHeaders.set("session-id", prepared.nativeSessionIdentity);
    baseHeaders.set("thread-id", prepared.nativeSessionIdentity);
    baseHeaders.set("x-client-request-id", prepared.nativeSessionIdentity);
  }
  const configuredBankedReset = options.bankedReset;
  const bankedResetDependencies: CodexBankedResetDependencies = {
    config: configuredBankedReset?.config ?? loadCodexBankedResetConfig(),
    reloadConfig: configuredBankedReset?.reloadConfig ??
      (configuredBankedReset?.config ? () => configuredBankedReset.config! : loadCodexBankedResetConfig),
    provider: configuredBankedReset?.provider ?? unavailableCodexUsageResetProvider,
    kv: configuredBankedReset?.kv,
    now: configuredBankedReset?.now,
    newOwnerToken: configuredBankedReset?.newOwnerToken,
    hash: configuredBankedReset?.hash,
    telemetry: configuredBankedReset?.telemetry,
    // A supplied provider exists only as the hermetic `fetchCodexResponses`
    // test seam. Real traffic creates its account-bound adapter below and
    // therefore always enforces a durable decision before live submission.
    allowLiveWithoutShadowForTest: configuredBankedReset?.provider !== undefined,
  };
  let lastResponse: Response | null = null;
  let lastError: unknown = null;
  let authWarning: string | null = null;
  let authFailure: CodexError | null = null;
  let probeUnavailable = false;
  let probeUnavailableCircuit: CodexProbeCircuit | null = null;
  let attemptNumber = 0;
  const refreshedSlots = new Set<number>();
  const retryState: {
    candidate:
      | Readonly<{
        accountEntry: CodexAuthAccountEntry;
        auth: CodexAuthState;
        routing: RoutingAccount;
        delayMs: number;
        readyAtMs: number;
        expiresAtMs: number;
      }>
      | null;
  } = { candidate: null };
  const bankedResetCandidates = new Map<number, CodexBankedResetCandidate>();

  const noteCodexAuthFailure = (error: unknown): void => {
    authWarning ??= codexAuthWarningForError(error) ?? CODEX_AUTH_REAUTH_WARNING;
    if (error instanceof CodexError && error.status === 401) authFailure ??= error;
  };

  const decorateAuthWarning = (response: Response): Response =>
    authWarning ? withCodexAuthWarning(response, authWarning) : response;

  const authFailureResponse = (error: CodexError | null): Response => {
    const response = routingErrorResponse(
      401,
      error?.message ?? CODEX_AUTH_REAUTH_MESSAGE,
      error?.code === "refresh_token_reused" ? error.code : "codex_auth_invalid",
    );
    return decorateAuthWarning(response);
  };

  const refreshBankedResetCandidate = async (
    candidate: CodexBankedResetCandidate,
  ): Promise<CodexBankedResetCandidate | null> => {
    try {
      // A banked reset is stricter than normal routing: force-read the auth
      // pool immediately before the durable side-effect path. A rotated or
      // reordered account is not eligible for an old candidate.
      const currentPoolEntry = await getAuthPoolEntry(true, true);
      const currentAuth = currentPoolEntry.pool.accounts[candidate.routing.slot];
      if (
        !currentAuth || currentAuth.account_id !== candidate.auth.account_id ||
        !sameCodexCredentials(currentAuth, candidate.auth)
      ) return null;
      return {
        ...candidate,
        auth: currentAuth,
        accountEntry: { ...currentPoolEntry, auth: currentAuth, routing: candidate.routing },
      };
    } catch {
      return null;
    }
  };

  /**
   * Reconciliation returns a fenced recovery probe. Keep the newly read auth
   * material but dispatch the one permitted post-reset retry through that
   * probe so a late pre-reset 429 cannot clear or rewrite its tombstone.
   */
  const withResetRecoveryProbe = (
    candidate: CodexBankedResetCandidate,
    probe: RoutingAccount,
  ): CodexBankedResetCandidate => {
    const routing = { ...probe, auth: candidate.auth };
    return {
      ...candidate,
      routing,
      accountEntry: { ...candidate.accountEntry, auth: candidate.auth, routing },
    };
  };

  const authPoolFence = (candidate: CodexBankedResetCandidate) => ({
    key: CODEX_AUTH_POOL_KV_KEY,
    isCurrent: (value: unknown): boolean => {
      const current = parseCodexAuthPool(value)?.accounts[candidate.routing.slot];
      return current?.account_id === candidate.auth.account_id &&
        sameCodexCredentials(current, candidate.auth);
    },
  });

  type PartialBankedResetCohortFence = Readonly<{
    authPool: CodexAuthPoolState;
    healthyAccounts: readonly RoutingAccount[];
    observedAtMs: number;
  }>;

  const partialCohortAuthIsCurrent = (
    value: unknown,
    cohort: PartialBankedResetCohortFence,
  ): boolean => {
    const current = parseCodexAuthPool(value);
    return current?.accounts.length === cohort.authPool.accounts.length &&
      cohort.authPool.accounts.every((expected, slot) => {
        const actual = current.accounts[slot];
        return actual?.account_id === expected.account_id && sameCodexCredentials(actual, expected);
      });
  };

  const partialCohortRoutingIsCurrent = (
    value: unknown,
    cohort: PartialBankedResetCohortFence,
  ): boolean => {
    const current = parseCodexAccountRoutingState(value);
    return current !== null && cohort.healthyAccounts.every((account) => {
      const slot = current.slots[account.slot];
      return slot?.account_id_hash === account.accountIdHash &&
        slot.credential_version === account.credentialVersion &&
        typeof account.routingGeneration === "number" &&
        slot.generation === account.routingGeneration &&
        slot.invalid_credential_version !== account.credentialVersion &&
        (codexQuotaBlockForModel(slot, account.requestedModel)?.blocked_until_ms ?? 0) <= cohort.observedAtMs &&
        (slot.upstream_timeout_blocked_until_ms ?? 0) <= cohort.observedAtMs &&
        ((slot.probe_lease?.circuit === "quota" && slot.probe_lease.quota_class !== undefined &&
            slot.probe_lease.quota_class !== null &&
            slot.probe_lease.quota_class !== codexQuotaClassForModel(account.requestedModel))
          ? true
          : (slot.probe_lease?.expires_at_ms ?? 0) <= cohort.observedAtMs);
    });
  };

  /**
   * This runs inside the transport's final dispatch hook, after any API-key
   * reservation hook and immediately before `fetch`. It closes the last
   * awaitable auth-pool rotation window for a post-reset retry.
   */
  const ensurePostResetRetryAuthCurrent = async (candidate: CodexBankedResetCandidate): Promise<void> => {
    const currentPoolEntry = await getAuthPoolEntry(true, true);
    const currentAuth = currentPoolEntry.pool.accounts[candidate.routing.slot];
    if (
      !currentAuth || currentAuth.account_id !== candidate.auth.account_id ||
      !sameCodexCredentials(currentAuth, candidate.auth)
    ) {
      throw new CodexBankedResetRetryFenceError();
    }
  };

  const resetFences = (
    candidate: CodexBankedResetCandidate,
    routingGeneration: number,
    partialCohort: PartialBankedResetCohortFence | null = null,
  ) => [
    {
      key: CODEX_ACCOUNT_ROUTING_KV_KEY,
      isCurrent: (value: unknown): boolean =>
        isCodexQuotaBlockFenceCurrent(
          value,
          candidate.routing,
          candidate.quotaResetAtMs,
          routingGeneration,
        ) && (partialCohort === null || partialCohortRoutingIsCurrent(value, partialCohort)),
    },
    {
      key: CODEX_AUTH_POOL_KV_KEY,
      isCurrent: (value: unknown): boolean =>
        authPoolFence(candidate).isCurrent(value) &&
        (partialCohort === null || partialCohortAuthIsCurrent(value, partialCohort)),
    },
  ];

  const resetInput = (
    candidate: CodexBankedResetCandidate,
    routingGeneration: number,
    partialCohort: PartialBankedResetCohortFence | null = null,
  ) => ({
    accountId: candidate.auth.account_id,
    credentialVersion: candidate.routing.credentialVersion,
    quotaResetAtMs: candidate.quotaResetAtMs,
    routingGeneration,
    fences: resetFences(candidate, routingGeneration, partialCohort),
    requestId: options.requestId ?? null,
    signal: options.signal,
  });

  /**
   * Bind the reset transport to the freshly fenced credentials that produced
   * this candidate. Credentials stay in this closure rather than entering the
   * durable transaction context. Tests inject a provider explicitly and never
   * reach this transport.
   */
  const resetDependenciesForCandidate = (
    candidate: CodexBankedResetCandidate,
  ): CodexBankedResetDependencies => {
    if (configuredBankedReset?.provider) return bankedResetDependencies;
    try {
      return {
        ...bankedResetDependencies,
        provider: createUpstreamCodexUsageResetProvider({
          codexBaseUrl: config.codexBaseUrl,
          accountId: candidate.auth.account_id,
          accessToken: candidate.auth.access_token,
          userAgent: codexUserAgent(options.clientVersion),
          now: bankedResetDependencies.now,
        }),
      };
    } catch {
      return bankedResetDependencies;
    }
  };

  const captureBankedResetCandidate = async (
    accountEntry: CodexAuthAccountEntry,
    routing: RoutingAccount,
    auth: CodexAuthState,
    disposition: Readonly<{
      usageLimitReached: boolean;
      retryAtMs: number | null;
      resetDeadlineIsStable: boolean;
    }>,
  ): Promise<"captured" | "ineligible" | "routing_fence_unavailable"> => {
    // Relative Retry-After delays are valid for ordinary routing, but their
    // Date.now-derived deadline cannot name a durable reset window. A
    // canonical absolute HTTP-date is only a provisional identity; the
    // durable no-revision fence is what permits it to receive a key.
    if (!disposition.usageLimitReached || disposition.retryAtMs === null || !disposition.resetDeadlineIsStable) {
      return "ineligible";
    }
    const routingGeneration = await getCodexQuotaBlockFence(routing, disposition.retryAtMs);
    if (routingGeneration === null) {
      // A revised quota window invalidates any older observation for this
      // slot. It must not remain selectable just because capture could not
      // establish a fresh fence for the new deadline.
      bankedResetCandidates.delete(routing.slot);
      return "routing_fence_unavailable";
    }
    bankedResetCandidates.set(routing.slot, {
      accountEntry,
      auth,
      routing,
      quotaResetAtMs: disposition.retryAtMs,
      routingGeneration,
    });
    return "captured";
  };

  const logBankedResetEvent = (
    event: CodexBankedResetEvent,
    fields: CodexBankedResetTelemetryFields,
  ): void => reportCodexBankedResetEvent(bankedResetDependencies.telemetry, event, fields);

  const reportHealthyFallback = async (): Promise<void> => {
    for (const candidate of bankedResetCandidates.values()) {
      try {
        const hash = bankedResetDependencies.hash ?? sha256Hex;
        const accountIdHash = await hash(candidate.auth.account_id);
        const quotaGeneration = await hash(
          `uos_ai\u0000codex_reset_generation\u0000${accountIdHash}\u0000${candidate.quotaResetAtMs}`,
        );
        logBankedResetEvent("codex_reset_skipped_healthy_fallback", {
          request_id: options.requestId ?? null,
          account_id_hash: accountIdHash,
          quota_generation: `v1:${quotaGeneration}`,
          routing_generation: candidate.routingGeneration,
        });
      } catch {
        // A telemetry hash failure must not delay a healthy inference response.
      }
    }
  };

  /**
   * Re-read the current pool before every banked-reset decision. The ordinary
   * all-blocked path requires the full pool. The expiring-credit canary may
   * instead evaluate a complete cohort containing stable blocked accounts plus
   * healthy, non-probing siblings; only the blocked cohort can reach inventory
   * or redemption.
   */
  const evaluateBlockedCohortBankedReset = async (
    requireFullPool: boolean,
  ): Promise<
    | Readonly<{
      candidate: CodexBankedResetCandidate;
      reset: Awaited<ReturnType<typeof reconcileCodexBankedReset>>;
      recoveryProbe: RoutingAccount | null;
    }>
    | null
  > => {
    if (probeUnavailable) return null;
    let currentPoolEntry: Awaited<ReturnType<typeof getAuthPoolEntry>>;
    try {
      currentPoolEntry = await getAuthPoolEntry(true, true);
    } catch {
      return null;
    }
    const routedPool = await selectCodexRoutingAccountsStrong(
      currentPoolEntry.pool,
      currentPoolEntry.pool.accounts,
      Date.now(),
      requestedModel,
    );
    if (routedPool.kind === "routing_unavailable") {
      logCodexRouting("codex_banked_reset_preflight", {
        request_id: options.requestId ?? null,
        require_full_pool: requireFullPool ? "true" : "false",
        outcome: "routing_unavailable",
        reason: "strong_routing_read_failed",
      });
      return null;
    }
    const blockedAccounts = routedPool.kind === "eligible" || routedPool.kind === "quota_blocked"
      ? routedPool.blockedAccounts
      : [];
    if (!blockedAccounts.length) return null;
    let partialCohort: PartialBankedResetCohortFence | null = null;
    if (requireFullPool) {
      if (routedPool.kind !== "quota_blocked" || blockedAccounts.length !== currentPoolEntry.pool.accounts.length) {
        return null;
      }
    } else {
      const eligibleAccounts = routedPool.kind === "eligible" ? routedPool.accounts : [];
      const coveredSlots = [
        ...eligibleAccounts.map((account) => account.slot),
        ...blockedAccounts.map((account) => account.slot),
      ];
      if (
        routedPool.kind !== "eligible" || eligibleAccounts.some((account) => account.probeRequired) ||
        coveredSlots.length !== currentPoolEntry.pool.accounts.length ||
        new Set(coveredSlots).size !== currentPoolEntry.pool.accounts.length
      ) {
        return null;
      }
      partialCohort = {
        authPool: currentPoolEntry.pool,
        healthyAccounts: eligibleAccounts,
        observedAtMs: Date.now(),
      };
    }
    const localCandidates = blockedAccounts.map((routing) => ({
      accountEntry: { ...currentPoolEntry, auth: routing.auth, routing },
      auth: routing.auth,
      routing,
      quotaResetAtMs: routing.quotaResetAtMs,
      routingGeneration: routing.routingGeneration,
    } satisfies CodexBankedResetCandidate));
    if (
      new Set(localCandidates.map((candidate) => candidate.routing.slot)).size !== blockedAccounts.length
    ) {
      return null;
    }

    // Preserve a pre-existing durable transaction before considering a new
    // shadow/live decision. A pending or rejected legacy transaction blocks a
    // replacement spend; a verified one is returned for its single retry.
    for (const candidate of localCandidates) {
      const reset = await reconcileCodexBankedReset(
        resetInput(candidate, candidate.routingGeneration, partialCohort),
        resetDependenciesForCandidate(candidate),
      );
      if (reset.kind === "verified") return { candidate, reset, recoveryProbe: null };
      if (reset.reason === "verified_routing_generation_stale") {
        const recoveryProbe = await reconcileCodexQuotaAfterStaleVerifiedReset(candidate.routing, {
          quotaResetAtMs: candidate.quotaResetAtMs,
          routingGeneration: candidate.routingGeneration,
          fences: [authPoolFence(candidate)],
        });
        if (!recoveryProbe) return null;
        return { candidate, reset, recoveryProbe };
      }
      if (reset.reason !== "no_existing_transaction") return null;
    }

    const poolCandidates = localCandidates.map((candidate) => ({
      slot: candidate.routing.slot,
      candidate: resetInput(candidate, candidate.routingGeneration, partialCohort),
      provider: resetDependenciesForCandidate(candidate).provider,
    }));
    const evaluated = await evaluateCodexBankedResetPool(poolCandidates, bankedResetDependencies);
    logCodexRouting("codex_banked_reset_preflight", {
      request_id: options.requestId ?? null,
      require_full_pool: requireFullPool ? "true" : "false",
      outcome: evaluated.kind,
      reason: evaluated.reason,
      candidate_count: poolCandidates.length,
      selected_slot: evaluated.selected === null ? null : evaluated.selected.slot + 1,
    });
    if (!evaluated.selected || !evaluated.reset) return null;
    const candidate = localCandidates.find((item) => item.routing.slot === evaluated.selected!.slot);
    return candidate ? { candidate, reset: evaluated.reset, recoveryProbe: null } : null;
  };

  const fetchAttempt = async (
    accountEntry: CodexAuthAccountEntry,
    auth: CodexAuthState,
    routing: RoutingAccount,
    phase: CodexAttemptPhase,
    beforeTransport?: () => Promise<void>,
  ): Promise<Response> => {
    attemptNumber += 1;
    let transportStarted = false;
    try {
      const response = await fetchCodexResponseWithAuth(
        auth,
        url,
        serializedBody,
        baseHeaders,
        options.signal,
        beforeTransport
          ? async () => {
            const dispatch = await options.beforeDispatch?.();
            try {
              await beforeTransport();
            } catch (error) {
              await dispatch?.cancelBeforeTransport();
              throw error;
            }
            return dispatch;
          }
          : options.beforeDispatch,
        () => {
          transportStarted = true;
          reportCodexResponseTiming(options.timing?.onDispatch);
        },
      );
      setCodexResponseAccountTelemetry(response, routing.slot + 1, auth.account_id);
      reportCodexResponseTiming(options.timing?.onHeaders);
      void recordCodexResponseHealth(auth.account_id, response, auth);
      logCodexRouting("codex_attempt", {
        request_id: options.requestId ?? null,
        attempt: attemptNumber,
        slot: routing.slot + 1,
        phase,
        status: response.status,
        status_class: codexStatusClass(response.status),
      });
      return response;
    } catch (error) {
      const signalReason = options.signal?.reason;
      const clientCancelled = options.signal?.aborted === true &&
        !(signalReason instanceof Error && signalReason.name === "TimeoutError");
      if (
        !(error instanceof CodexBankedResetRetryFenceError) &&
        !(error instanceof ApiKeyQuotaDispatchError) &&
        !clientCancelled
      ) {
        void recordCodexThrownHealth(accountEntry.auth.account_id, error);
      }
      if (transportStarted && error instanceof CodexError && error.code === "gateway_timeout") {
        // The transport may already have reached Codex. Fence only future
        // requests; every inference attempt, including bounded and reset
        // retries, must preserve the no-replay circuit.
        await markCodexUpstreamTimeout(routing);
      }
      logCodexRouting("codex_attempt", {
        request_id: options.requestId ?? null,
        attempt: attemptNumber,
        slot: routing.slot + 1,
        phase,
        status: error instanceof CodexError ? error.status : null,
        status_class: error instanceof CodexBankedResetRetryFenceError ? "banked_reset_fenced" : codexErrorClass(error),
      });
      throw error;
    }
  };

  const refreshAfter401 = async (
    routing: RoutingAccount,
    auth: CodexAuthState,
    trigger: "401" | "proactive",
  ): Promise<Readonly<{ auth: CodexAuthState; routing: RoutingAccount }>> => {
    refreshedSlots.add(routing.slot);
    try {
      const refreshed = trigger === "proactive"
        ? await awaitWithoutCancellingSharedWork(
          getValidAuth({ ...poolEntry, auth, routing }),
          options.signal,
        )
        : await awaitWithoutCancellingSharedWork(
          refreshAuthCoordinated({ ...poolEntry, auth, routing }),
          options.signal,
        );
      const reconciled = await reconcileCodexRoutingAccount(routing, refreshed);
      logCodexRouting("codex_token_refresh", {
        request_id: options.requestId ?? null,
        slot: routing.slot + 1,
        trigger,
        outcome: "succeeded",
        status_class: "2xx",
      });
      return { auth: refreshed, routing: reconciled };
    } catch (error) {
      logCodexRouting("codex_token_refresh", {
        request_id: options.requestId ?? null,
        slot: routing.slot + 1,
        trigger,
        outcome: "failed",
        status_class: codexErrorClass(error),
      });
      throw error;
    }
  };

  const classify429 = async (
    accountEntry: CodexAuthAccountEntry,
    routing: RoutingAccount,
    auth: CodexAuthState,
    response: Response,
  ): Promise<Response> => {
    const disposition = await markCodexQuotaBlocked(routing, response);
    let candidateOutcome: "captured" | "ineligible" | "routing_fence_unavailable" = "ineligible";
    if (disposition.usageLimitReached && disposition.retryAtMs !== null && disposition.resetDeadlineIsStable) {
      candidateOutcome = await captureBankedResetCandidate(accountEntry, routing, auth, disposition);
    } else {
      // A later ordinary/ambiguous 429 means this request did not establish
      // that every failed account is genuinely quota-exhausted. Do not spend
      // a reset based on an older candidate in the same failover pass.
      bankedResetCandidates.clear();
    }
    logCodexRouting("codex_quota_classification", {
      request_id: options.requestId ?? null,
      slot: routing.slot + 1,
      usage_limit_reached: disposition.usageLimitReached ? "true" : "false",
      retry_at_ms: disposition.retryAtMs,
      quota_block_source: disposition.quotaBlockSource,
      reset_deadline_is_stable: disposition.resetDeadlineIsStable ? "true" : "false",
      reset_deadline_conflict: disposition.resetDeadlineConflict ? "true" : "false",
      candidate_outcome: candidateOutcome,
    });
    const classifiedAtMs = Date.now();
    const retryAfterDelay = disposition.retryAtMs === null ? 0 : Math.max(0, disposition.retryAtMs - classifiedAtMs);
    const mayRetryWithinBound = !disposition.usageLimitReached ||
      retryAfterDelay <= CODEX_ADDITIONAL_429_RETRY_MAX_DELAY_MS;
    if (mayRetryWithinBound) {
      const candidate = {
        accountEntry,
        auth,
        routing: {
          ...routing,
          probeRequired: disposition.usageLimitReached && disposition.retryAtMs !== null,
          probeGeneration: null,
          probeToken: null,
          probeCircuit: disposition.usageLimitReached && disposition.retryAtMs !== null ? "quota" as const : null,
        },
        delayMs: Math.min(retryAfterDelay, CODEX_ADDITIONAL_429_RETRY_MAX_DELAY_MS),
        readyAtMs: disposition.retryAtMs ?? classifiedAtMs,
        expiresAtMs: classifiedAtMs + CODEX_ADDITIONAL_429_RETRY_MAX_DELAY_MS,
      };
      if (!retryState.candidate || candidate.delayMs < retryState.candidate.delayMs) {
        retryState.candidate = candidate;
      }
    }
    return disposition.response;
  };

  type EvaluatedBlockedReset = NonNullable<Awaited<ReturnType<typeof evaluateBlockedCohortBankedReset>>>;

  const persistVerifiedCapacityReset = async (
    candidate: CodexBankedResetCandidate,
    record: NonNullable<Awaited<ReturnType<typeof reconcileCodexBankedReset>>>["record"],
  ): Promise<void> => {
    if (record?.state !== "verified" || record.verified_at_ms === null) return;
    const slot = candidate.routing.slot + 1;
    if (slot !== 1 && slot !== 2) return;
    try {
      const kv = bankedResetDependencies.kv ?? await getKv();
      if (!kv) return;
      await recordProviderCapacityResetEvent({
        v: 1,
        event_id: record.idempotency_key_hash,
        slot,
        observed_at_ms: record.verified_at_ms,
      }, kv);
    } catch {
      // Capacity telemetry is best effort and must never change inference.
    }
  };

  // Codex can report an expired bearer as a quota-shaped 403. Only classify
  // that status as auth when the locally decoded JWT is already expired.
  const responseIsCodexAuthFailure = (auth: CodexAuthState, response: Response): boolean =>
    response.status === 401 || (response.status === 403 && accessTokenExpired(auth));

  const runPostResetRetry = async (
    evaluated: EvaluatedBlockedReset,
    normalResponse: Response | null,
  ): Promise<Response | null> => {
    if (!evaluated.reset.record || (evaluated.reset.kind !== "verified" && !evaluated.recoveryProbe)) {
      return normalResponse;
    }
    const { candidate, reset } = evaluated;
    const resetRecord = reset.record!;
    await persistVerifiedCapacityReset(candidate, resetRecord);
    const reconciled = evaluated.recoveryProbe ?? await reconcileCodexQuotaAfterVerifiedReset(candidate.routing, {
      quotaResetAtMs: candidate.quotaResetAtMs,
      routingGeneration: resetRecord.routing_generation,
      fences: [authPoolFence(candidate)],
    });
    if (!reconciled) return normalResponse;
    const refreshedCandidate = await refreshBankedResetCandidate(candidate);
    if (!refreshedCandidate) return normalResponse;
    const retryCandidate = withResetRecoveryProbe(refreshedCandidate, reconciled);

    logBankedResetEvent("codex_reset_inference_retry", {
      request_id: options.requestId ?? null,
      account_id_hash: reset.accountIdHash,
      quota_generation: reset.quotaGeneration,
      idempotency_key_hash: reset.idempotencyKeyHash,
      routing_generation: resetRecord.routing_generation,
    });
    let retried: Response;
    try {
      retried = await fetchAttempt(
        retryCandidate.accountEntry,
        retryCandidate.auth,
        retryCandidate.routing,
        "post_banked_reset",
        () => ensurePostResetRetryAuthCurrent(retryCandidate),
      );
    } catch (error) {
      if (error instanceof CodexBankedResetRetryFenceError) return normalResponse;
      logBankedResetEvent("codex_reset_inference_retry_result", {
        request_id: options.requestId ?? null,
        account_id_hash: reset.accountIdHash,
        quota_generation: reset.quotaGeneration,
        idempotency_key_hash: reset.idempotencyKeyHash,
        routing_generation: resetRecord.routing_generation,
        status: error instanceof CodexError ? error.status : null,
      });
      reportCodexBankedResetMetric(
        bankedResetDependencies.telemetry,
        "codex_reset_post_retry_total",
        1,
        {
          request_id: options.requestId ?? null,
          account_id_hash: reset.accountIdHash,
          quota_generation: reset.quotaGeneration,
          idempotency_key_hash: reset.idempotencyKeyHash,
          routing_generation: resetRecord.routing_generation,
          status: error instanceof CodexError ? error.status : null,
        },
      );
      throw error;
    }
    if (normalResponse) cancelResponseBody(normalResponse);
    if (responseIsCodexAuthFailure(retryCandidate.auth, retried)) {
      await markCodexCredentialInvalid(retryCandidate.routing);
      authWarning ??= CODEX_AUTH_REAUTH_WARNING;
    }
    if (retried.status === 429) {
      // This is the one permitted post-reset inference attempt. Preserve a
      // replayable normal 429 and never feed it back into reset selection.
      retried = (await markCodexRecoveryProbeQuotaBlocked(retryCandidate.routing, retried)).response;
    } else if (!retried.ok) {
      await releaseCodexRoutingProbe(retryCandidate.routing);
    }
    if (retried.ok && retryCandidate.routing.probeGeneration !== null) {
      codexProbeByResponse.set(retried, retryCandidate.routing);
    }
    logBankedResetEvent("codex_reset_inference_retry_result", {
      request_id: options.requestId ?? null,
      account_id_hash: reset.accountIdHash,
      quota_generation: reset.quotaGeneration,
      idempotency_key_hash: reset.idempotencyKeyHash,
      routing_generation: resetRecord.routing_generation,
      status: retried.status,
    });
    reportCodexBankedResetMetric(
      bankedResetDependencies.telemetry,
      "codex_reset_post_retry_total",
      1,
      {
        request_id: options.requestId ?? null,
        account_id_hash: reset.accountIdHash,
        quota_generation: reset.quotaGeneration,
        idempotency_key_hash: reset.idempotencyKeyHash,
        routing_generation: resetRecord.routing_generation,
        status: retried.status,
      },
    );
    return decorateAuthWarning(retried);
  };

  const redeemAndRetryOnce = async (normalResponse: Response): Promise<Response> => {
    // A half-open account could still be healthy, but another isolate owns
    // its recovery probe. Keep the normal quota response instead of spending
    // a reset that was inferred only from a sibling's 429.
    if (probeUnavailable) return normalResponse;
    const evaluated = await evaluateBlockedCohortBankedReset(true);
    if (!evaluated) return normalResponse;
    return await runPostResetRetry(evaluated, normalResponse) ?? normalResponse;
  };

  const recoverBlockedReset = async (requireFullPool: boolean): Promise<Response | null> => {
    const evaluated = await evaluateBlockedCohortBankedReset(requireFullPool);
    return evaluated ? await runPostResetRetry(evaluated, null) : null;
  };

  if (selected.kind === "eligible" && selected.blockedAccounts.length) {
    const fallbackAccountIds = new Set(selected.accounts.map((account) => account.auth.account_id));
    let definitiveCanaryFailure: Response | null = null;
    try {
      const canary = await recoverBlockedReset(false);
      if (canary?.ok) return canary;
      if (canary && (canary.status === 401 || canary.status === 403 || canary.status === 429)) {
        definitiveCanaryFailure = canary;
      } else if (canary) {
        return canary;
      }
    } catch (error) {
      // A post-reset transport ambiguity may have dispatched upstream work.
      // Preserve ordinary no-replay semantics instead of trying a sibling.
      throw error;
    }

    // Partial-cohort preflight may await inventory, redemption, and a recovery
    // probe. Do not dispatch ordinary fallback with the auth/routing snapshot
    // captured before those operations: a sibling may now be blocked, rotated,
    // invalid, reordered, or probing.
    poolEntry = await getAuthPoolEntry(true, true);
    const refreshedSelection = await selectCodexRoutingAccountsStrong(
      poolEntry.pool,
      poolEntry.pool.accounts,
      Date.now(),
      requestedModel,
    );
    if (refreshedSelection.kind === "routing_unavailable") {
      if (definitiveCanaryFailure) return definitiveCanaryFailure;
      throw new CodexError(
        "Codex routing state is unavailable after banked-reset preflight.",
        "codex_auth_missing",
        503,
      );
    }
    selected = refreshedSelection;
    if (selected.kind === "upstream_blocked") {
      if (definitiveCanaryFailure) return definitiveCanaryFailure;
      return upstreamTimeoutCircuitResponse(selected.retryAtMs);
    }
    if (selected.kind === "credentials_invalid") {
      if (definitiveCanaryFailure) cancelResponseBody(definitiveCanaryFailure);
      return withCodexAuthWarning(
        routingErrorResponse(
          401,
          `${CODEX_AUTH_REAUTH_MESSAGE} All configured Codex credentials are invalid.`,
          "codex_auth_invalid",
        ),
        CODEX_AUTH_REAUTH_WARNING,
      );
    }
    if (selected.kind === "quota_blocked") {
      if (definitiveCanaryFailure) cancelResponseBody(definitiveCanaryFailure);
      // The cohort changed while the partial preflight was in flight. Do not
      // start a second, now-all-blocked reset evaluation in the same request.
      return routingErrorResponse(
        429,
        "All configured Codex accounts are quota-blocked; retry after their next reset.",
        CODEX_QUOTA_BLOCKED_ERROR_CODE,
        selected.retryAtMs,
      );
    }
    const refreshedFallbacks = selected.accounts.filter((account) => fallbackAccountIds.has(account.auth.account_id));
    if (!refreshedFallbacks.length) {
      if (definitiveCanaryFailure) return definitiveCanaryFailure;
      throw new CodexError(
        "No originally healthy Codex fallback remains eligible after banked-reset preflight.",
        "codex_auth_missing",
        503,
      );
    }
    if (definitiveCanaryFailure) cancelResponseBody(definitiveCanaryFailure);
    accountEntries = refreshedFallbacks.map((routing) => ({ ...poolEntry, auth: routing.auth, routing }));
  }

  if (selected.kind === "quota_blocked") {
    const recovered = await recoverBlockedReset(true);
    if (recovered) return recovered;
    return routingErrorResponse(
      429,
      "All configured Codex accounts are quota-blocked; retry after their next reset.",
      CODEX_QUOTA_BLOCKED_ERROR_CODE,
      selected.retryAtMs,
    );
  }
  for (let index = 0; index < accountEntries.length; index += 1) {
    const accountEntry = accountEntries[index];
    let routing = accountEntry.routing!;
    if (routing.probeRequired) {
      const claimed = await claimCodexRoutingProbe(poolEntry.pool, routing);
      if (!claimed) {
        probeUnavailable = true;
        if (routing.probeCircuit === "upstream_timeout") probeUnavailableCircuit = "upstream_timeout";
        continue;
      }
      routing = claimed;
    }
    try {
      let auth: CodexAuthState;
      if (needsRefresh(accountEntry.auth)) {
        ({ auth, routing } = await refreshAfter401(routing, accountEntry.auth, "proactive"));
      } else {
        auth = accountEntry.auth;
      }
      let response = await fetchAttempt(accountEntry, auth, routing, "initial");
      if (responseIsCodexAuthFailure(auth, response)) {
        if (refreshedSlots.has(routing.slot)) {
          await markCodexCredentialInvalid(routing);
          authWarning ??= CODEX_AUTH_REAUTH_WARNING;
        } else {
          cancelResponseBody(response);
          try {
            ({ auth, routing } = await refreshAfter401(routing, auth, "401"));
            response = await fetchAttempt(accountEntry, auth, routing, "post_refresh");
            if (responseIsCodexAuthFailure(auth, response)) {
              await markCodexCredentialInvalid(routing);
              authWarning ??= CODEX_AUTH_REAUTH_WARNING;
            }
          } catch (error) {
            if (error instanceof CodexError && error.status === 401) {
              await markCodexCredentialInvalid(routing);
              noteCodexAuthFailure(error);
              lastError = error;
              continue;
            }
            throw error;
          }
        }
      }
      if (response.status === 429) {
        response = await classify429(accountEntry, routing, auth, response);
        setCodexResponseAccountTelemetry(response, routing.slot + 1, auth.account_id);
      } else if (!response.ok) {
        await releaseCodexRoutingProbe(routing);
      }
      if (response.ok && routing.probeGeneration !== null) codexProbeByResponse.set(response, routing);
      if (response.status === 401 || response.status === 403 || response.status === 429) {
        if (lastResponse) cancelResponseBody(lastResponse);
        lastResponse = response;
        continue;
      }
      if (lastResponse) cancelResponseBody(lastResponse);
      if (response.ok) await reportHealthyFallback();
      return decorateAuthWarning(response);
    } catch (error) {
      lastError = error;
      // A deterministic OAuth rejection is attributable to this credential
      // even when it happens before the first inference fetch. Quarantine it
      // and let an eligible sibling serve the request; transient refresh
      // outages deliberately stay on this path and never trigger a switch.
      if (error instanceof CodexError && error.status === 401) {
        await markCodexCredentialInvalid(routing);
        noteCodexAuthFailure(error);
        continue;
      }
      if (
        error instanceof CodexError &&
        (error.code === "gateway_timeout" || error.code === "codex_upstream_unreachable")
      ) {
        await releaseCodexRoutingProbe(routing);
        continue;
      }
      if (!(error instanceof CodexError && error.code === "gateway_timeout")) {
        await releaseCodexRoutingProbe(routing);
      }
      // Other transport failures, aborts, and deadlines remain account-independent;
      // only the explicit upstream response-header timeout opens this circuit.
      if (lastResponse) cancelResponseBody(lastResponse);
      throw error;
    }
  }

  if (probeUnavailableCircuit === "upstream_timeout") {
    if (lastResponse) cancelResponseBody(lastResponse);
    let retryAtMs: number | null = null;
    try {
      const currentPoolEntry = await getAuthPoolEntry(true, true);
      const currentSelection = await selectCodexRoutingAccountsStrong(
        currentPoolEntry.pool,
        currentPoolEntry.pool.accounts,
        Date.now(),
        requestedModel,
      );
      if (currentSelection.kind === "upstream_blocked") retryAtMs = currentSelection.retryAtMs;
    } catch {
      // The failed claim already proves a competing timeout probe. Preserve its
      // 503 classification if a fresh routing read is unavailable.
    }
    return upstreamTimeoutCircuitResponse(retryAtMs);
  }

  const retryCandidate = retryState.candidate;
  if (retryCandidate) {
    const retryCheckAtMs = Date.now();
    const retryDelayMs = Math.max(0, retryCandidate.readyAtMs - retryCheckAtMs);
    if (
      retryCheckAtMs > retryCandidate.expiresAtMs ||
      retryCheckAtMs + retryDelayMs > retryCandidate.expiresAtMs
    ) {
      if (lastResponse) return await redeemAndRetryOnce(lastResponse);
      return routingErrorResponse(
        429,
        "All configured Codex accounts are temporarily quota blocked.",
        CODEX_QUOTA_BLOCKED_ERROR_CODE,
      );
    }
    await waitForCodexRetry(
      retryDelayMs,
      options.signal,
      options.retrySleep ?? delay,
    );
    if (Date.now() > retryCandidate.expiresAtMs) {
      if (lastResponse) return await redeemAndRetryOnce(lastResponse);
      return routingErrorResponse(
        429,
        "All configured Codex accounts are temporarily quota blocked.",
        CODEX_QUOTA_BLOCKED_ERROR_CODE,
      );
    }
    let retryAuth = retryCandidate.auth;
    let retryRouting = retryCandidate.routing;
    if (retryRouting.probeRequired) {
      const claimed = await claimCodexRoutingProbe(
        poolEntry.pool,
        retryRouting,
        Math.max(Date.now(), retryCandidate.readyAtMs),
      );
      if (!claimed) {
        if (lastResponse) return lastResponse;
        return routingErrorResponse(
          429,
          "All configured Codex accounts are temporarily quota blocked.",
          CODEX_QUOTA_BLOCKED_ERROR_CODE,
        );
      }
      retryRouting = claimed;
    }
    if (Date.now() > retryCandidate.expiresAtMs) {
      await releaseCodexRoutingProbe(retryRouting);
      if (lastResponse) return await redeemAndRetryOnce(lastResponse);
      return routingErrorResponse(
        429,
        "All configured Codex accounts are temporarily quota blocked.",
        CODEX_QUOTA_BLOCKED_ERROR_CODE,
      );
    }
    logCodexRouting("codex_two_second_retry", {
      request_id: options.requestId ?? null,
      slot: retryCandidate.routing.slot + 1,
      delay_ms: retryDelayMs,
    });
    if (lastResponse) cancelResponseBody(lastResponse);
    let response: Response;
    try {
      response = await fetchAttempt(
        retryCandidate.accountEntry,
        retryAuth,
        retryRouting,
        "two_second_retry",
      );
    } catch (error) {
      if (!(error instanceof CodexError && error.code === "gateway_timeout")) {
        await releaseCodexRoutingProbe(retryRouting);
      }
      throw error;
    }
    if (responseIsCodexAuthFailure(retryAuth, response)) {
      if (refreshedSlots.has(retryRouting.slot)) {
        await markCodexCredentialInvalid(retryRouting);
        authWarning ??= CODEX_AUTH_REAUTH_WARNING;
      } else {
        cancelResponseBody(response);
        try {
          ({ auth: retryAuth, routing: retryRouting } = await refreshAfter401(retryRouting, retryAuth, "401"));
          response = await fetchAttempt(
            retryCandidate.accountEntry,
            retryAuth,
            retryRouting,
            "post_retry_refresh",
          );
        } catch (error) {
          if (error instanceof CodexError && error.status === 401) {
            await markCodexCredentialInvalid(retryRouting);
            noteCodexAuthFailure(error);
            return authFailureResponse(error);
          }
          if (!(error instanceof CodexError && error.code === "gateway_timeout")) {
            await releaseCodexRoutingProbe(retryRouting);
          }
          throw error;
        }
        if (responseIsCodexAuthFailure(retryAuth, response)) {
          await markCodexCredentialInvalid(retryRouting);
          authWarning ??= CODEX_AUTH_REAUTH_WARNING;
        }
      }
    }
    if (response.status === 429) {
      const disposition = await markCodexQuotaBlocked(retryRouting, response);
      response = disposition.response;
      if (disposition.usageLimitReached && disposition.retryAtMs !== null && disposition.resetDeadlineIsStable) {
        await captureBankedResetCandidate(retryCandidate.accountEntry, retryRouting, retryAuth, disposition);
      } else {
        // The ordinary bounded retry gave a non-qualifying answer. It is not
        // evidence that a banked reset is safe to spend.
        bankedResetCandidates.clear();
      }
      setCodexResponseAccountTelemetry(response, retryRouting.slot + 1, retryAuth.account_id);
    } else if (!response.ok) {
      // A 401/403 says this retrying account cannot serve, but does not erase
      // a separately verified quota-exhaustion candidate from another slot.
      // Other non-successes remain conservative and discard that candidate.
      if (response.status !== 401 && response.status !== 403) bankedResetCandidates.clear();
      await releaseCodexRoutingProbe(retryRouting);
    }
    if (response.ok && retryRouting.probeGeneration !== null) {
      codexProbeByResponse.set(response, retryRouting);
    }
    // A successful ordinary bounded retry has already served the original
    // inference request. A captured exhaustion observation from before that
    // retry is no longer a reason to spend a reset or issue another request.
    if (response.ok) return decorateAuthWarning(response);
    return decorateAuthWarning(await redeemAndRetryOnce(response));
  }
  if (lastResponse) return decorateAuthWarning(await redeemAndRetryOnce(lastResponse));
  if (authFailure) return authFailureResponse(authFailure);
  if (lastError instanceof CodexError && lastError.status === 401) return authFailureResponse(lastError);
  if (probeUnavailable) {
    return routingErrorResponse(
      429,
      "All configured Codex accounts are temporarily quota blocked.",
      CODEX_QUOTA_BLOCKED_ERROR_CODE,
    );
  }
  throw lastError ?? new CodexError("Codex auth pool is empty.", "codex_auth_missing", 503);
};

export const fetchCodexResponses = async (
  body: unknown,
  options: FetchCodexResponsesOptions = {},
): Promise<Response> => {
  const prepared = await prepareCodexSubscriptionRequest(body, options.cacheScope ?? null);
  const response = await fetchPreparedCodexResponses(prepared, options);
  return withCodexWarnings(response, prepared.warnings);
};

export const fetchCodexModels = async (
  options: Readonly<{ clientVersion?: string | null; ifNoneMatch?: string | null; signal?: AbortSignal }> = {},
): Promise<Response> => {
  const poolEntry = await getAuthPoolEntry();
  const accountEntries = randomizedAuthEntries(poolEntry);
  const requestedVersion = options.clientVersion?.trim() || null;
  const clientVersion = requestedVersion && parseCodexClientVersion(requestedVersion)
    ? requestedVersion
    : CODEX_CLIENT_VERSION;
  const urls = codexModelsBaseUrls(clientVersion);
  let lastResponse: Response | null = null;

  for (const url of urls) {
    for (let index = 0; index < accountEntries.length; index += 1) {
      const accountEntry = accountEntries[index];
      const hasFallbackAccount = index < accountEntries.length - 1;
      let auth: CodexAuthState;
      let res: Response;
      try {
        auth = await awaitWithoutCancellingSharedWork(getValidAuth(accountEntry), options.signal);
        res = await fetchCodexModelsWithAuth(auth, url, clientVersion, options.ifNoneMatch, options.signal);
        await recordCodexResponseHealth(auth.account_id, res, auth, "reachable");
        if (res.status === 401) {
          cancelResponseBody(res);
          auth = await awaitWithoutCancellingSharedWork(
            refreshAuthCoordinated(await getCurrentAccountEntry(auth.account_id, true)),
            options.signal,
          );
          res = await fetchCodexModelsWithAuth(auth, url, clientVersion, options.ifNoneMatch, options.signal);
          await recordCodexResponseHealth(auth.account_id, res, auth, "reachable");
        }
      } catch (error) {
        await recordCodexThrownHealth(accountEntry.auth.account_id, error);
        if (hasFallbackAccount) continue;
        throw error;
      }
      if (hasFallbackAccount && (res.status === 401 || res.status === 429)) {
        cancelResponseBody(res);
        lastResponse = res;
        continue;
      }
      if ((res.status === 404 || res.status === 400) && urls.length > 1) {
        lastResponse = res;
        break;
      }
      return res;
    }
  }

  return lastResponse ?? new Response("Codex upstream models endpoint not found.", { status: 404 });
};

export const loadCodexModelsSnapshot = async (): Promise<CodexModelsSnapshot | null> => {
  return (await loadRuntimeConfig())?.codex_models ?? null;
};

export const preserveCodexDefaultModel = (
  snapshot: CodexModelsSnapshot,
  candidate: string | null | undefined,
): string | undefined => {
  const target = candidate?.trim();
  if (!target) return undefined;
  const found = snapshot.models.some((model) => {
    if (!isRecord(model)) return false;
    const id = getString(model.slug) ?? getString(model.id) ?? getString(model.model) ?? getString(model.name);
    return id?.trim() === target;
  });
  return found ? target : undefined;
};

export const loadFullCodexModelsSnapshot = async (
  kvOverride?: Deno.Kv | null,
): Promise<CodexModelsSnapshot | null> => {
  const kv = kvOverride === undefined ? await getKv() : kvOverride;
  if (!kv) return null;
  const entry = await kv.get<CodexModelsSnapshot>(CODEX_MODELS_KV_KEY, { consistency: "strong" });
  const snapshot = entry.value;
  if (!snapshot || !Array.isArray(snapshot.models) || snapshot.models.length === 0) return null;
  if (snapshot.models.some((model) => !isRecord(model))) return null;
  if (!getString(snapshot.source)?.trim()) return null;
  if (
    typeof snapshot.updated_at_ms !== "number" || !Number.isSafeInteger(snapshot.updated_at_ms) ||
    snapshot.updated_at_ms <= 0
  ) return null;
  return snapshot;
};

export const storeCodexModelsSnapshot = async (snapshot: CodexModelsSnapshot): Promise<boolean> => {
  const kv = await getKv();
  if (!kv) return false;
  for (let attempt = 0; attempt < 3; attempt += 1) {
    const [currentEntry, runtimeEntry] = await Promise.all([
      kv.get<CodexModelsSnapshot>(CODEX_MODELS_KV_KEY, { consistency: "strong" }),
      kv.get(RUNTIME_CONFIG_V2_KEY, { consistency: "strong" }),
    ]);
    const currentRuntime = normalizeRuntimeConfig(runtimeEntry.value);
    const nextSnapshot = mergeCodexModelPromptCacheCapabilities(snapshot, currentEntry.value);
    const runtimeConfig = buildRuntimeConfig(nextSnapshot, {
      defaultModel: preserveCodexDefaultModel(nextSnapshot, currentRuntime?.default_model),
      defaultReasoningEffort: currentRuntime?.default_reasoning_effort,
    });
    const commit = await kv.atomic()
      .check(currentEntry)
      .check(runtimeEntry)
      .set(CODEX_MODELS_KV_KEY, nextSnapshot)
      .set(RUNTIME_CONFIG_V2_KEY, runtimeConfig)
      .commit();
    if (!commit.ok) continue;
    cacheRuntimeConfig(runtimeConfig);
    return true;
  }
  return false;
};

export const buildCodexRequest = (
  model: string,
  input: ResponseInputItem[],
  options: Readonly<{ reasoning?: Record<string, unknown> | null; instructions?: string | null }> = {},
): Record<string, unknown> => {
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
  options: Readonly<{ clientVersion?: string | null }> = {},
): Promise<
  {
    ok: true;
    auth: CodexAuthState;
    refreshed: boolean;
    status: number;
    contentType: string | null;
    models: unknown;
    modelsBody: string;
    etag: string | null;
    clientVersion: string;
  } | {
    ok: false;
    status: number;
    body: string;
  }
> => {
  const requestedVersion = options.clientVersion?.trim() || null;
  const clientVersion = requestedVersion && parseCodexClientVersion(requestedVersion)
    ? requestedVersion
    : CODEX_CLIENT_VERSION;
  const urls = codexModelsBaseUrls(clientVersion);
  let refreshed = false;
  let lastResponse: Response | null = null;

  for (const url of urls) {
    let res = await fetchCodexModelsWithAuth(auth, url, clientVersion);
    if (res.status === 401) {
      try {
        const next = await refreshAuthStateless(auth);
        refreshed = true;
        res = await fetchCodexModelsWithAuth(next, url, clientVersion);
        auth = next;
      } catch {
        // ignore and return the original 401 response
      }
    }
    if ((res.status === 404 || res.status === 400) && urls.length > 1) {
      lastResponse = res;
      continue;
    }

    const contentType = res.headers.get("Content-Type");
    if (res.ok) {
      const text = await res.text().catch(() => "");
      let models: unknown = null;
      if (contentType?.includes("application/json") && text) {
        try {
          models = JSON.parse(text) as unknown;
        } catch {
          models = null;
        }
      }
      try {
        await res.body?.cancel();
      } catch {
        // ignore
      }
      return {
        ok: true,
        auth,
        refreshed,
        status: res.status,
        contentType,
        models,
        modelsBody: text,
        etag: res.headers.get("ETag"),
        clientVersion,
      };
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
