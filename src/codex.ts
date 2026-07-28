import { config } from "./config.ts";
import {
  claimCodexRoutingProbe,
  CODEX_ACCOUNT_ROUTING_KV_KEY,
  getCodexQuotaBlockFence,
  isCodexQuotaBlockFenceCurrent,
  markCodexCredentialInvalid,
  markCodexQuotaBlocked,
  markCodexSuccess,
  reconcileCodexQuotaAfterVerifiedReset,
  reconcileCodexRoutingAccount,
  releaseCodexRoutingProbe,
  resetCodexAccountRoutingForTest,
  type RoutingAccount,
  selectCodexRoutingAccounts,
} from "./codex_account_routing.ts";
import {
  attemptCodexBankedReset,
  type CodexBankedResetConfig,
  type CodexBankedResetDependencies,
  type CodexBankedResetEvent,
  type CodexBankedResetTelemetry,
  type CodexBankedResetTelemetryFields,
  loadCodexBankedResetConfig,
  reconcileCodexBankedReset,
  reportCodexBankedResetEvent,
  reportCodexBankedResetMetric,
} from "./codex_banked_reset.ts";
import {
  type CodexUsageResetProvider,
  createStatusOnlyCodexUsageResetProvider,
  unavailableCodexUsageResetProvider,
} from "./codex_banked_reset_provider.ts";
import { type CodexModelsSnapshot, parseCodexClientVersion } from "./codex_models.ts";
import { getKv } from "./kv.ts";
import { type ApiKeyProviderDispatch, ApiKeyQuotaDispatchError } from "./api_key_policy.ts";
import { recordCodexProviderHealth } from "./provider_health.ts";
import { buildRuntimeConfig, cacheRuntimeConfig, loadRuntimeConfig, RUNTIME_CONFIG_V2_KEY } from "./runtime_config.ts";
import { decodeBase64ToString, getString, isRecord, sha256Hex } from "./utils.ts";
import type { CodexAuthPoolState, CodexAuthState, ResponseInputItem } from "./types.ts";

const CODEX_REFRESH_CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann";
const CODEX_REFRESH_TOKEN_URL = "https://auth.openai.com/oauth/token";
const CODEX_ORIGINATOR = "codex_cli_rs";
const CODEX_CLIENT_VERSION = "0.100.0";

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

const needsRefresh = (auth: CodexAuthState): boolean => {
  const expMs = getJwtExpMs(auth.access_token);
  const now = Date.now();
  if (expMs) return expMs - now < 2 * 60_000;
  return now - auth.updated_at_ms > 7 * 60_000;
};

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
const codexProbeTransitionsInFlight = new Set<Promise<void>>();

/** The metadata stays isolate-local and never becomes a response header or durable credential record. */
export const getCodexRoutingProbe = (response: Response): RoutingAccount | null =>
  codexProbeByResponse.get(response) ?? null;

export const releaseCodexResponseProbe = async (response: Response): Promise<void> => {
  const probe = codexProbeByResponse.get(response);
  if (!probe) return;
  codexProbeByResponse.delete(response);
  const transition = markCodexSuccess(probe);
  codexProbeTransitionsInFlight.add(transition);
  try {
    await transition;
  } finally {
    codexProbeTransitionsInFlight.delete(transition);
  }
};

export const markCodexResponseCompleted = releaseCodexResponseProbe;

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

const buildRefreshFailureMessage = (response: Response): string => {
  try {
    const cancellation = response.body?.cancel();
    if (cancellation) void cancellation.catch(() => {});
  } catch {
    // A refresh error body is deliberately discarded and never logged.
  }
  return `Codex auth refresh failed (status ${response.status}).`;
};

const refreshFailureStatus = (status: number): number => status === 400 || status === 401 || status === 403 ? 401 : 503;

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
    throw new CodexError(
      await buildRefreshFailureMessage(response),
      "codex_auth_refresh_failed",
      refreshFailureStatus(response.status),
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
    throw new CodexError(
      await buildRefreshFailureMessage(response),
      "codex_auth_refresh_failed",
      refreshFailureStatus(response.status),
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

const recordCodexResponseHealth = async (accountId: string, response: Response): Promise<void> => {
  if (response.status === 401) {
    await recordCodexProviderHealth(accountId, "auth_invalid", response.status);
  } else if (response.status === 429) {
    await recordCodexProviderHealth(accountId, "quota_exhausted", response.status);
  } else if (response.status >= 500) {
    await recordCodexProviderHealth(accountId, "upstream_error", response.status);
  } else {
    await recordCodexProviderHealth(accountId, response.ok ? "success" : "reachable", response.status);
  }
};

const recordCodexThrownHealth = async (accountId: string, error: unknown): Promise<void> => {
  if (
    error instanceof CodexError &&
    (error.code === "codex_auth_refresh_failed" || error.code === "codex_auth_refresh_unreachable")
  ) {
    return;
  }
  await recordCodexProviderHealth(accountId, "upstream_error", null);
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
    85_000,
  );
  const transportSignal = signal ? AbortSignal.any([signal, deadline.signal]) : deadline.signal;
  try {
    // This is intentionally immediately adjacent to the actual provider
    // transport: request-quota reservations commit on dispatch, not on
    // validation, routing, or a later retry outcome.
    const dispatch = beforeDispatch ? await beforeDispatch() : undefined;
    if (dispatch && transportSignal.aborted) {
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
  status: 401 | 429,
  message: string,
  code: string,
  retryAtMs: number | null = null,
): Response => {
  const headers = new Headers({ "Content-Type": "application/json" });
  if (status === 429 && retryAtMs !== null) {
    headers.set("Retry-After", String(Math.max(1, Math.ceil((retryAtMs - Date.now()) / 1000))));
  }
  return new Response(
    JSON.stringify({
      error: {
        message,
        type: status === 429 ? "rate_limit_error" : "invalid_request_error",
        code,
        param: null,
      },
    }),
    { status, headers },
  );
};

type CodexResponseTimingHooks = Readonly<{
  onDispatch?: () => void;
  onHeaders?: () => void;
}>;

type CodexAttemptPhase = "initial" | "post_refresh" | "two_second_retry" | "post_retry_refresh" | "post_banked_reset";

type CodexBankedResetOptions = Readonly<{
  /** Test seam; normal traffic builds an account-bound adapter only for a live reset candidate. */
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
  event: "codex_attempt" | "codex_token_refresh" | "codex_two_second_retry",
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

export const fetchCodexResponses = async (
  body: unknown,
  options: Readonly<{
    clientVersion?: string | null;
    signal?: AbortSignal;
    requestId?: string | null;
    timing?: CodexResponseTimingHooks;
    retrySleep?: (milliseconds: number) => Promise<void>;
    beforeDispatch?: () => Promise<ApiKeyProviderDispatch | void>;
    bankedReset?: CodexBankedResetOptions;
  }> = {},
): Promise<Response> => {
  if (codexProbeTransitionsInFlight.size) {
    await Promise.allSettled([...codexProbeTransitionsInFlight]);
  }
  const poolEntry = await getAuthPoolEntry();
  const selected = await selectCodexRoutingAccounts(
    poolEntry.pool,
    poolEntry.pool.accounts,
  );
  if (selected.kind === "credentials_invalid") {
    return routingErrorResponse(
      401,
      "All configured Codex credentials are invalid.",
      "codex_auth_invalid",
    );
  }
  const accountEntries = selected.kind === "eligible"
    ? selected.accounts.map((routing) => ({ ...poolEntry, auth: routing.auth, routing }))
    : [];
  const url = `${config.codexBaseUrl}/responses`;
  const serializedBody = JSON.stringify(body);
  const baseHeaders = new Headers({
    "originator": CODEX_ORIGINATOR,
    "user-agent": codexUserAgent(options.clientVersion),
    "Content-Type": "application/json",
    "Accept": "text/event-stream",
    "conversation_id": crypto.randomUUID(),
  });
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
  };
  let lastResponse: Response | null = null;
  let lastError: unknown = null;
  let probeUnavailable = false;
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
  ) => [
    {
      key: CODEX_ACCOUNT_ROUTING_KV_KEY,
      isCurrent: (value: unknown): boolean =>
        isCodexQuotaBlockFenceCurrent(
          value,
          candidate.routing,
          candidate.quotaResetAtMs,
          routingGeneration,
        ),
    },
    authPoolFence(candidate),
  ];

  const resetInput = (
    candidate: CodexBankedResetCandidate,
    routingGeneration: number,
  ) => ({
    accountId: candidate.auth.account_id,
    credentialVersion: candidate.routing.credentialVersion,
    quotaResetAtMs: candidate.quotaResetAtMs,
    routingGeneration,
    fences: resetFences(candidate, routingGeneration),
    requestId: options.requestId ?? null,
    signal: options.signal,
  });

  /**
   * Bind the reset transport to the same freshly fenced credentials that
   * identified this candidate. Test providers remain an explicit injection;
   * the normal path never carries credentials into the durable reset context.
   * A malformed base URL (or malformed auth material) fails closed before any
   * reset-credit request is attempted.
   */
  const resetDependenciesForCandidate = (
    candidate: CodexBankedResetCandidate,
  ): CodexBankedResetDependencies => {
    if (configuredBankedReset?.provider) return bankedResetDependencies;
    try {
      return {
        ...bankedResetDependencies,
        provider: createStatusOnlyCodexUsageResetProvider({
          codexBaseUrl: config.codexBaseUrl,
          accountId: candidate.auth.account_id,
          accessToken: candidate.auth.access_token,
          userAgent: codexUserAgent(options.clientVersion),
          originator: CODEX_ORIGINATOR,
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
  ): Promise<void> => {
    // Relative Retry-After delays are valid for ordinary routing, but their
    // Date.now-derived deadline cannot name a durable reset window. A
    // canonical absolute HTTP-date is only a provisional identity; the
    // durable no-revision fence is what permits it to receive a key.
    if (!disposition.usageLimitReached || disposition.retryAtMs === null || !disposition.resetDeadlineIsStable) return;
    const routingGeneration = await getCodexQuotaBlockFence(routing, disposition.retryAtMs);
    if (routingGeneration === null) {
      // A revised quota window invalidates any older observation for this
      // slot. It must not remain selectable just because capture could not
      // establish a fresh fence for the new deadline.
      bankedResetCandidates.delete(routing.slot);
      return;
    }
    bankedResetCandidates.set(routing.slot, {
      accountEntry,
      auth,
      routing,
      quotaResetAtMs: disposition.retryAtMs,
      routingGeneration,
    });
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

  const fetchAttempt = async (
    accountEntry: CodexAuthAccountEntry,
    auth: CodexAuthState,
    routing: RoutingAccount,
    phase: CodexAttemptPhase,
    beforeTransport?: () => Promise<void>,
  ): Promise<Response> => {
    attemptNumber += 1;
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
        () => reportCodexResponseTiming(options.timing?.onDispatch),
      );
      reportCodexResponseTiming(options.timing?.onHeaders);
      void recordCodexResponseHealth(auth.account_id, response);
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
      if (!(error instanceof CodexBankedResetRetryFenceError)) {
        void recordCodexThrownHealth(accountEntry.auth.account_id, error);
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
    if (disposition.usageLimitReached && disposition.retryAtMs !== null && disposition.resetDeadlineIsStable) {
      await captureBankedResetCandidate(accountEntry, routing, auth, disposition);
    } else {
      // A later ordinary/ambiguous 429 means this request did not establish
      // that every failed account is genuinely quota-exhausted. Do not spend
      // a reset based on an older candidate in the same failover pass.
      bankedResetCandidates.clear();
    }
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

  const redeemAndRetryOnce = async (normalResponse: Response): Promise<Response> => {
    // Keep every independently qualifying account until a healthy fallback
    // proves no reset is needed. The last 429 may be from an unallowlisted
    // sibling, so a single overwritten candidate could otherwise suppress an
    // earlier eligible account. A skipped non-allowlisted candidate is safe
    // to bypass; every other outcome stops selection before another account
    // can make a new external submission.
    for (const capturedCandidate of bankedResetCandidates.values()) {
      const candidate = await refreshBankedResetCandidate(capturedCandidate);
      if (!candidate) continue;

      // Re-read the routing fence immediately before starting the expensive
      // transaction. A stale 429 may never clear or redeem against a newer
      // quota observation.
      const currentFence = await getCodexQuotaBlockFence(candidate.routing, candidate.quotaResetAtMs);
      if (currentFence === null) continue;
      const reset = await attemptCodexBankedReset(
        resetInput(candidate, currentFence),
        resetDependenciesForCandidate(candidate),
      );
      if (reset.kind !== "verified" || !reset.record) {
        if (reset.kind === "skipped" && reset.reason === "account_not_allowlisted") continue;
        return normalResponse;
      }

      // The transaction can be independently verified yet still be fenced off
      // by a credential rotation or later 429. In that case preserve the normal
      // upstream response; a stale worker must not dispatch an inference retry.
      const reconciled = await reconcileCodexQuotaAfterVerifiedReset(candidate.routing, {
        quotaResetAtMs: candidate.quotaResetAtMs,
        routingGeneration: reset.record.routing_generation,
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
        routing_generation: reset.record.routing_generation,
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
          routing_generation: reset.record.routing_generation,
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
            routing_generation: reset.record.routing_generation,
            status: error instanceof CodexError ? error.status : null,
          },
        );
        throw error;
      }
      cancelResponseBody(normalResponse);
      if (retried.status === 401) await markCodexCredentialInvalid(retryCandidate.routing);
      if (retried.status === 429) {
        // This is the one permitted post-reset inference attempt. Preserve a
        // replayable normal 429 and never feed it back into reset selection.
        retried = (await markCodexQuotaBlocked(retryCandidate.routing, retried)).response;
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
        routing_generation: reset.record.routing_generation,
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
          routing_generation: reset.record.routing_generation,
          status: retried.status,
        },
      );
      return retried;
    }
    return normalResponse;
  };

  const recoverBlockedReset = async (): Promise<Response | null> => {
    if (selected.kind !== "quota_blocked") return null;
    for (const blocked of selected.blockedAccounts) {
      const capturedCandidate: CodexBankedResetCandidate = {
        accountEntry: { ...poolEntry, auth: blocked.auth, routing: blocked },
        auth: blocked.auth,
        routing: blocked,
        quotaResetAtMs: blocked.quotaResetAtMs,
        routingGeneration: blocked.routingGeneration,
      };
      const candidate = await refreshBankedResetCandidate(capturedCandidate);
      if (!candidate) continue;
      const reset = await reconcileCodexBankedReset(
        resetInput(candidate, candidate.routingGeneration),
        resetDependenciesForCandidate(candidate),
      );
      if (reset.kind !== "verified" || !reset.record) continue;
      const reconciled = await reconcileCodexQuotaAfterVerifiedReset(candidate.routing, {
        quotaResetAtMs: candidate.quotaResetAtMs,
        routingGeneration: reset.record.routing_generation,
        fences: [authPoolFence(candidate)],
      });
      if (!reconciled) continue;
      const refreshedCandidate = await refreshBankedResetCandidate(candidate);
      if (!refreshedCandidate) continue;
      const retryCandidate = withResetRecoveryProbe(refreshedCandidate, reconciled);
      logBankedResetEvent("codex_reset_inference_retry", {
        request_id: options.requestId ?? null,
        account_id_hash: reset.accountIdHash,
        quota_generation: reset.quotaGeneration,
        idempotency_key_hash: reset.idempotencyKeyHash,
        routing_generation: reset.record.routing_generation,
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
        if (error instanceof CodexBankedResetRetryFenceError) return null;
        logBankedResetEvent("codex_reset_inference_retry_result", {
          request_id: options.requestId ?? null,
          account_id_hash: reset.accountIdHash,
          quota_generation: reset.quotaGeneration,
          idempotency_key_hash: reset.idempotencyKeyHash,
          routing_generation: reset.record.routing_generation,
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
            routing_generation: reset.record.routing_generation,
            status: error instanceof CodexError ? error.status : null,
          },
        );
        throw error;
      }
      if (retried.status === 401) await markCodexCredentialInvalid(retryCandidate.routing);
      if (retried.status === 429) {
        retried = (await markCodexQuotaBlocked(retryCandidate.routing, retried)).response;
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
        routing_generation: reset.record.routing_generation,
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
          routing_generation: reset.record.routing_generation,
          status: retried.status,
        },
      );
      return retried;
    }
    return null;
  };

  if (selected.kind === "quota_blocked") {
    const recovered = await recoverBlockedReset();
    if (recovered) return recovered;
    return routingErrorResponse(
      429,
      "All configured Codex accounts are quota-blocked; retry after their next reset.",
      "codex_quota_blocked",
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
      if (response.status === 401) {
        if (refreshedSlots.has(routing.slot)) {
          await markCodexCredentialInvalid(routing);
        } else {
          cancelResponseBody(response);
          try {
            ({ auth, routing } = await refreshAfter401(routing, auth, "401"));
            response = await fetchAttempt(accountEntry, auth, routing, "post_refresh");
            if (response.status === 401) await markCodexCredentialInvalid(routing);
          } catch (error) {
            if (error instanceof CodexError && error.status === 401) {
              await markCodexCredentialInvalid(routing);
              lastError = error;
              continue;
            }
            throw error;
          }
        }
      }
      if (response.status === 429) {
        response = await classify429(accountEntry, routing, auth, response);
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
      return response;
    } catch (error) {
      lastError = error;
      // A deterministic OAuth rejection is attributable to this credential
      // even when it happens before the first inference fetch. Quarantine it
      // and let an eligible sibling serve the request; transient refresh
      // outages deliberately stay on this path and never trigger a switch.
      if (error instanceof CodexError && error.status === 401) {
        await markCodexCredentialInvalid(routing);
        continue;
      }
      await releaseCodexRoutingProbe(routing);
      // Fetch failures, aborts, and deadlines are account-independent.
      if (lastResponse) cancelResponseBody(lastResponse);
      throw error;
    }
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
        "codex_quota_blocked",
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
        "codex_quota_blocked",
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
          "codex_quota_blocked",
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
        "codex_quota_blocked",
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
      await releaseCodexRoutingProbe(retryRouting);
      throw error;
    }
    if (response.status === 401) {
      if (refreshedSlots.has(retryRouting.slot)) {
        await markCodexCredentialInvalid(retryRouting);
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
            return routingErrorResponse(401, "All configured Codex credentials are invalid.", "codex_auth_invalid");
          }
          await releaseCodexRoutingProbe(retryRouting);
          throw error;
        }
        if (response.status === 401) await markCodexCredentialInvalid(retryRouting);
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
    if (response.ok) return response;
    return await redeemAndRetryOnce(response);
  }
  if (lastResponse) return await redeemAndRetryOnce(lastResponse);
  if (lastError instanceof CodexError && lastError.status === 401) {
    return routingErrorResponse(401, "All configured Codex credentials are invalid.", "codex_auth_invalid");
  }
  if (probeUnavailable) {
    return routingErrorResponse(
      429,
      "All configured Codex accounts are temporarily quota blocked.",
      "codex_quota_blocked",
    );
  }
  throw lastError ?? new CodexError("Codex auth pool is empty.", "codex_auth_missing", 503);
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
        await recordCodexResponseHealth(auth.account_id, res);
        if (res.status === 401) {
          cancelResponseBody(res);
          auth = await awaitWithoutCancellingSharedWork(
            refreshAuthCoordinated(await getCurrentAccountEntry(auth.account_id, true)),
            options.signal,
          );
          res = await fetchCodexModelsWithAuth(auth, url, clientVersion, options.ifNoneMatch, options.signal);
          await recordCodexResponseHealth(auth.account_id, res);
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
  const current = await loadRuntimeConfig(kv);
  const runtimeConfig = buildRuntimeConfig(snapshot, {
    defaultModel: preserveCodexDefaultModel(snapshot, current?.default_model),
    defaultReasoningEffort: current?.default_reasoning_effort,
  });
  const commit = await kv.atomic()
    .set(CODEX_MODELS_KV_KEY, snapshot)
    .set(RUNTIME_CONFIG_V2_KEY, runtimeConfig)
    .commit();
  if (!commit.ok) return false;
  cacheRuntimeConfig(runtimeConfig);
  return true;
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
