import { config } from "./config.ts";
import { apiKeyIdKey, coerceApiKeyExpiresAtMs } from "./api_keys.ts";
import { type ApiKeyPolicy, authenticateApiKeyToken, getApiKeyUsageV3, looksLikeUosApiKey } from "./api_key_policy.ts";
import { json, openaiError } from "./http.ts";
import { getBearerToken } from "./http.ts";
import { resolveKernelQuotaPolicyState } from "./kernel_usage.ts";
import { recordKernelPolicyQueue } from "./kernel_policy_queue.ts";
import { getKv } from "./kv.ts";
import { isAdminAuthDisabledForRequest } from "./local_admin_auth.ts";
import { getPasskeySessionForRequest, isPasskeyUserAdmin } from "./passkeys.ts";
import { getString, isRecord, sha256Base64Url, sha256Hex } from "./utils.ts";
import type { ApiKeyRecord } from "./types.ts";

const GITHUB_API_BASE_URL = "https://api.github.com";
const GITHUB_TOKEN_CACHE_TTL_MS = 5 * 60_000;
const githubTokenCache = new Map<string, number>();

const looksLikeGitHubToken = (token: string): boolean => {
  const trimmed = token.trim();
  return trimmed.startsWith("gh") || trimmed.startsWith("github_pat_");
};

const getEnv = (key: string): string | undefined => {
  try {
    return Deno.env.get(key);
  } catch {
    return undefined;
  }
};

const normalizePemValue = (raw: string): string => raw.trim().replace(/\\n/g, "\n");

const extractPemBlocks = (raw: string, begin: string, end: string): string[] => {
  const blocks: string[] = [];
  let cursor = 0;
  while (cursor < raw.length) {
    const start = raw.indexOf(begin, cursor);
    if (start === -1) break;
    const finish = raw.indexOf(end, start);
    if (finish === -1) break;
    blocks.push(raw.slice(start, finish + end.length).trim());
    cursor = finish + end.length;
  }
  return blocks;
};

const UOS_KERNEL_PUBKEYS_KEY = ["uos_ai", "kernel_pubkeys"];

const getKernelPublicKeyPems = async (): Promise<string[]> => {
  const envRaw = (getEnv("UOS_AI_KERNEL_PUBLIC_KEY") ?? "").trim();
  const tokens: string[] = [];

  if (envRaw) {
    const normalized = normalizePemValue(envRaw);
    const blocks = extractPemBlocks(normalized, "-----BEGIN PUBLIC KEY-----", "-----END PUBLIC KEY-----");
    if (blocks.length > 0) {
      tokens.push(...blocks);
    } else {
      tokens.push(normalized);
    }
  }

  const kv = await getKv();
  if (kv) {
    const kvEntry = await kv.get<Array<{ pem: string }>>(UOS_KERNEL_PUBKEYS_KEY);
    if (kvEntry.value) {
      tokens.push(...kvEntry.value.map((p) => p.pem));
    }
  }

  return tokens;
};

const importRsaPublicKey = async (publicKeyPem: string): Promise<CryptoKey> => {
  const pemContents = publicKeyPem
    .replace("-----BEGIN PUBLIC KEY-----", "")
    .replace("-----END PUBLIC KEY-----", "")
    .trim()
    .replace(/\s+/g, "");

  const binary = atob(pemContents);
  const binaryDer: Uint8Array<ArrayBuffer> = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) binaryDer[i] = binary.charCodeAt(i);
  return await crypto.subtle.importKey(
    "spki",
    binaryDer,
    { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" },
    true,
    ["verify"],
  );
};

const KERNEL_PUBLIC_KEYS_REFRESH_MS = 60_000;

let kernelPublicKeysPromise: Promise<ReadonlyArray<CryptoKey>> | null = null;
let kernelPublicKeysLoadedAtMs = 0;

const loadKernelPublicKeys = async (): Promise<ReadonlyArray<CryptoKey>> => {
  const pems = await getKernelPublicKeyPems();
  const keys: CryptoKey[] = [];
  for (const pem of pems) {
    try {
      keys.push(await importRsaPublicKey(pem));
    } catch (error) {
      console.error("[ai.ubq.fi] Failed to import kernel public key:", error);
    }
  }
  return keys;
};

const getKernelPublicKeys = (forceReload = false): Promise<ReadonlyArray<CryptoKey>> => {
  const now = Date.now();
  if (!forceReload && kernelPublicKeysPromise && now - kernelPublicKeysLoadedAtMs < KERNEL_PUBLIC_KEYS_REFRESH_MS) {
    return kernelPublicKeysPromise;
  }
  kernelPublicKeysPromise = (async () => {
    const keys = await loadKernelPublicKeys();
    kernelPublicKeysLoadedAtMs = Date.now();
    return keys;
  })();
  return kernelPublicKeysPromise;
};

export const reloadKernelPublicKeys = async (): Promise<void> => {
  await getKernelPublicKeys(true);
};

const decodeBase64UrlToBytes = (raw: string): Uint8Array<ArrayBuffer> => {
  const b64 = raw.replace(/-/g, "+").replace(/_/g, "/");
  const padded = b64 + "=".repeat((4 - (b64.length % 4)) % 4);
  const binary = atob(padded);
  const bytes: Uint8Array<ArrayBuffer> = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
};

const decodeBase64UrlToJson = (raw: string): unknown => {
  const bytes = decodeBase64UrlToBytes(raw);
  const text = new TextDecoder().decode(bytes);
  return JSON.parse(text);
};

type KernelAttestationPayload = Readonly<{
  iss: "ubiquity-os-kernel";
  aud: "ai.ubq.fi";
  iat: number;
  exp: number;
  jti: string;
  owner: string;
  repo: string;
  installation_id: number | null;
  auth_token_sha256: string;
  state_id: string;
}>;

const parseKernelAttestationPayload = (value: unknown): KernelAttestationPayload | null => {
  if (!isRecord(value)) return null;

  const iss = getString(value.iss);
  const aud = getString(value.aud);
  const jti = getString(value.jti);
  const owner = getString(value.owner);
  const repo = getString(value.repo);
  const authTokenSha = getString(value.auth_token_sha256);
  const stateId = getString(value.state_id);

  const iat = typeof value.iat === "number" && Number.isFinite(value.iat) ? Math.trunc(value.iat) : null;
  const exp = typeof value.exp === "number" && Number.isFinite(value.exp) ? Math.trunc(value.exp) : null;
  if (iat === null || exp === null) return null;

  const installationIdValue = value.installation_id;
  const installationId = installationIdValue === null
    ? null
    : typeof installationIdValue === "number" && Number.isFinite(installationIdValue)
    ? Math.trunc(installationIdValue)
    : null;
  if (installationIdValue !== null && installationId === null) return null;

  if (iss !== "ubiquity-os-kernel") return null;
  if (aud !== "ai.ubq.fi") return null;
  if (!jti || !owner || !repo || !authTokenSha || !stateId) return null;

  return {
    iss: "ubiquity-os-kernel",
    aud: "ai.ubq.fi",
    iat,
    exp,
    jti,
    owner,
    repo,
    installation_id: installationId,
    auth_token_sha256: authTokenSha,
    state_id: stateId,
  };
};

const KERNEL_ATTESTATION_CLOCK_SKEW_SECONDS = 60;
const KERNEL_ATTESTATION_MAX_TTL_SECONDS = 60 * 60;

const parseInstallationIdHeader = (req: Request): number | null => {
  const raw = (req.headers.get("X-GitHub-Installation-Id") ?? "").trim();
  if (!raw) return null;
  const num = Number(raw);
  if (!Number.isFinite(num)) return null;
  return Math.trunc(num);
};

const kernelTokenJtiCache = new Map<string, number>();

const pruneKernelTokenJtiCache = () => {
  const now = Date.now();
  for (const [jti, expiresAtMs] of kernelTokenJtiCache.entries()) {
    if (expiresAtMs <= now) kernelTokenJtiCache.delete(jti);
  }
};

const verifyKernelAttestation = async (
  req: Request,
  { token, owner, repo }: { token: string; owner?: string | null; repo?: string | null },
): Promise<{ ok: true; payload: KernelAttestationPayload } | { ok: false; response: Response }> => {
  const kernelToken = (req.headers.get("X-Ubiquity-Kernel-Token") ?? "").trim();
  if (!kernelToken) {
    return {
      ok: false,
      response: openaiError(
        401,
        "Unauthorized: missing 'X-Ubiquity-Kernel-Token' header for kernel attestation",
        "missing_kernel_token",
      ),
    };
  }

  const keys = await getKernelPublicKeys();
  if (keys.length === 0) {
    return {
      ok: false,
      response: openaiError(
        500,
        "Server misconfigured: No kernel public keys loaded. Set 'UOS_AI_KERNEL_PUBLIC_KEY' or use admin endpoints to add pubkeys.",
        "server_error",
      ),
    };
  }

  const parts = kernelToken.split(".");
  if (parts.length !== 3) {
    return {
      ok: false,
      response: openaiError(
        401,
        "Unauthorized: kernel attestation JWT MUST have 3 parts (header, payload, signature)",
        "invalid_kernel_token",
      ),
    };
  }

  let header: unknown;
  let payload: KernelAttestationPayload | null = null;

  try {
    header = decodeBase64UrlToJson(parts[0]);
    payload = parseKernelAttestationPayload(decodeBase64UrlToJson(parts[1]));
  } catch (err) {
    return {
      ok: false,
      response: openaiError(
        401,
        `Unauthorized: failed to parse kernel attestation JWT parts: ${
          err instanceof Error ? err.message : String(err)
        }`,
        "invalid_kernel_token",
      ),
    };
  }

  if (!isRecord(header) || getString(header.alg) !== "RS256") {
    return {
      ok: false,
      response: openaiError(
        401,
        `Unauthorized: kernel attestation JWT 'alg' MUST be 'RS256', got '${
          isRecord(header) ? header.alg : "undefined"
        }'`,
        "invalid_kernel_token",
      ),
    };
  }

  if (!payload) {
    return {
      ok: false,
      response: openaiError(
        401,
        "Unauthorized: kernel attestation JWT payload is invalid or fields are missing/type-mismatched",
        "invalid_kernel_token",
      ),
    };
  }

  const now = Math.floor(Date.now() / 1000);
  if (payload.exp < payload.iat) {
    return {
      ok: false,
      response: openaiError(
        401,
        `Unauthorized: kernel attestation 'exp' (${payload.exp}) is before 'iat' (${payload.iat})`,
        "invalid_kernel_token",
      ),
    };
  }
  if (payload.exp - payload.iat > KERNEL_ATTESTATION_MAX_TTL_SECONDS) {
    return {
      ok: false,
      response: openaiError(
        401,
        `Unauthorized: kernel attestation TTL is too long (${
          payload.exp - payload.iat
        }s > ${KERNEL_ATTESTATION_MAX_TTL_SECONDS}s)`,
        "invalid_kernel_token",
      ),
    };
  }
  if (payload.iat > now + KERNEL_ATTESTATION_CLOCK_SKEW_SECONDS) {
    return {
      ok: false,
      response: openaiError(
        401,
        `Unauthorized: kernel attestation 'iat' (${payload.iat}) is in the future (server now: ${now})`,
        "invalid_kernel_token",
      ),
    };
  }
  if (payload.exp < now - KERNEL_ATTESTATION_CLOCK_SKEW_SECONDS) {
    return {
      ok: false,
      response: openaiError(
        401,
        `Unauthorized: kernel attestation 'exp' (${payload.exp}) is in the past (server now: ${now})`,
        "invalid_kernel_token",
      ),
    };
  }

  const expectedOwner = typeof owner === "string" ? owner.trim() : "";
  const expectedRepo = typeof repo === "string" ? repo.trim() : "";
  if (expectedOwner && expectedRepo && (payload.owner !== expectedOwner || payload.repo !== expectedRepo)) {
    return {
      ok: false,
      response: openaiError(
        401,
        `Unauthorized: kernel attestation repo mismatch. Expected '${expectedOwner}/${expectedRepo}', got '${payload.owner}/${payload.repo}'`,
        "invalid_kernel_token",
      ),
    };
  }

  if (payload.installation_id !== null) {
    const installationId = parseInstallationIdHeader(req);
    if (installationId === null) {
      return {
        ok: false,
        response: openaiError(
          401,
          "Unauthorized: missing 'X-GitHub-Installation-Id' header, required for kernel attestation verification",
          "missing_installation_id",
        ),
      };
    }
    if (payload.installation_id !== installationId) {
      return {
        ok: false,
        response: openaiError(
          401,
          `Unauthorized: kernel attestation installation_id mismatch. Expected '${installationId}', got '${payload.installation_id}'`,
          "invalid_kernel_token",
        ),
      };
    }
  }

  const expectedTokenSha = await sha256Base64Url(token);
  if (payload.auth_token_sha256 !== expectedTokenSha) {
    return {
      ok: false,
      response: openaiError(
        401,
        "Unauthorized: kernel attestation 'auth_token_sha256' mismatch. Verify kernel is hashing the current 'authToken'.",
        "invalid_kernel_token",
      ),
    };
  }

  const signingInput = `${parts[0]}.${parts[1]}`;
  const dataArray = new TextEncoder().encode(signingInput) as Uint8Array<ArrayBuffer>;
  let signatureBytes: Uint8Array<ArrayBuffer>;
  try {
    signatureBytes = decodeBase64UrlToBytes(parts[2]);
  } catch (err) {
    return {
      ok: false,
      response: openaiError(
        401,
        `Unauthorized: failed to decode kernel attestation signature: ${
          err instanceof Error ? err.message : String(err)
        }`,
        "invalid_kernel_token",
      ),
    };
  }

  const verifySignature = async (candidateKeys: ReadonlyArray<CryptoKey>): Promise<boolean> => {
    for (const key of candidateKeys) {
      try {
        const ok = await crypto.subtle.verify("RSASSA-PKCS1-v1_5", key, signatureBytes, dataArray);
        if (ok) return true;
      } catch {
        // ignore and try next key
      }
    }
    return false;
  };

  let signatureValid = await verifySignature(keys);
  if (!signatureValid) {
    const refreshedKeys = await getKernelPublicKeys(true);
    if (refreshedKeys.length === 0) {
      return {
        ok: false,
        response: openaiError(
          500,
          "Server misconfigured: No kernel public keys loaded. Set 'UOS_AI_KERNEL_PUBLIC_KEY' or use admin endpoints to add pubkeys.",
          "server_error",
        ),
      };
    }
    signatureValid = await verifySignature(refreshedKeys);
  }
  if (!signatureValid) {
    return {
      ok: false,
      response: openaiError(401, "Unauthorized (invalid kernel attestation)", "invalid_kernel_token"),
    };
  }

  pruneKernelTokenJtiCache();
  // This token is expected to be re-used within its TTL (e.g. Codex tool-calling
  // results in multiple /v1/responses requests). Enforcing single-use "jti"
  // semantics breaks legitimate traffic and is unreliable in serverless anyway.
  kernelTokenJtiCache.set(payload.jti, payload.exp * 1000);

  return { ok: true, payload };
};

const getGitHubRepoHeaders = (req: Request): { owner: string; repo: string } | null => {
  const owner = (req.headers.get("X-GitHub-Owner") ?? "").trim();
  const repo = (req.headers.get("X-GitHub-Repo") ?? "").trim();
  if (!owner || !repo) return null;
  return { owner, repo };
};

export const getKernelAttestationContext = async (
  req: Request,
  token: string | null,
): Promise<{ owner: string; repo: string } | null> => {
  if (!token) return null;
  const kernelToken = (req.headers.get("X-Ubiquity-Kernel-Token") ?? "").trim();
  if (!kernelToken) return null;
  const repoHeaders = getGitHubRepoHeaders(req);
  const attestation = await verifyKernelAttestation(req, {
    token,
    owner: repoHeaders?.owner,
    repo: repoHeaders?.repo,
  });
  if (!attestation.ok) return null;
  return { owner: attestation.payload.owner, repo: attestation.payload.repo };
};

type GitHubTokenRepoAccessResult =
  | { ok: true }
  | { ok: false; reason: "rejected" | "upstream_unavailable" };

const verifyGitHubTokenRepoAccess = async (
  token: string,
  owner: string,
  repo: string,
): Promise<GitHubTokenRepoAccessResult> => {
  const res = await fetch(`${GITHUB_API_BASE_URL}/repos/${owner}/${repo}`, {
    method: "GET",
    headers: {
      "Authorization": `Bearer ${token}`,
      "Accept": "application/vnd.github+json",
      "X-GitHub-Api-Version": "2022-11-28",
      "User-Agent": "ai.ubq.fi",
    },
    redirect: "manual",
  });

  const status = res.status;

  try {
    await res.body?.cancel();
  } catch {
    // ignore
  }

  if (res.ok) return { ok: true };
  if (status === 401 || status === 404) {
    return { ok: false, reason: "rejected" };
  }
  return { ok: false, reason: "upstream_unavailable" };
};

type ClientAuthMethod =
  | { kind: "disabled" }
  | { kind: "github_token"; owner: string; repo: string; state_id: string; limit_scope: "org" | "repo" }
  | { kind: "auth_tokens_allowlist" }
  | { kind: "kv_api_key"; key_id: string; policy: ApiKeyPolicy }
  | { kind: "admin_allowlist" }
  | { kind: "deno_deploy_token" }
  | { kind: "passkey_session"; user_id: string; handle: string; is_admin: boolean; credential_count: number };

type AuthenticateClientResult =
  | { ok: true; token: string | null; method: ClientAuthMethod }
  | { ok: false; response: Response };

type CheckAdminTokenResult =
  | { ok: true; kind: "admin_allowlist" | "deno_deploy_token" }
  | { ok: false; response: Response | null };

type AdminAuthMethod =
  | { kind: "disabled" }
  | { kind: "admin_allowlist" }
  | { kind: "deno_deploy_token" }
  | { kind: "passkey_session"; user_id: string; handle: string; is_admin: boolean; credential_count: number };

export type AdminAuthResult =
  | { ok: true; token: string; method: AdminAuthMethod; is_super_admin: boolean }
  | { ok: false; response: Response };

const authenticateGitHubToken = async (
  req: Request,
  token: string,
): Promise<{ ok: true; method: ClientAuthMethod } | { ok: false; response: Response } | null> => {
  if (!looksLikeGitHubToken(token)) return null;
  // An explicitly configured admin token remains an allowlisted credential,
  // even if its value happens to use a GitHub token prefix. Let the normal
  // admin-token check handle it without attempting GitHub attestation.
  if (config.adminTokens.has(token)) return null;

  const repoHeaders = getGitHubRepoHeaders(req);
  const kernelToken = (req.headers.get("X-Ubiquity-Kernel-Token") ?? "").trim();
  if (!repoHeaders && !kernelToken) {
    return { ok: false, response: openaiError(401, "Unauthorized", "invalid_api_key") };
  }

  const attestation = await verifyKernelAttestation(req, {
    token,
    owner: repoHeaders?.owner,
    repo: repoHeaders?.repo,
  });
  if (!attestation.ok) return { ok: false, response: attestation.response };
  const { owner, repo } = attestation.payload;
  const stateId = attestation.payload.state_id;

  const cacheKey = await sha256Base64Url(`${token}:${owner}/${repo}`);
  const cachedUntil = githubTokenCache.get(cacheKey) ?? 0;
  if (cachedUntil > Date.now()) {
    const policyState = await resolveKernelQuotaPolicyState(owner, repo);
    if (!policyState.ok) return { ok: false, response: policyState.response };
    if (!policyState.has_policy) {
      await recordKernelPolicyQueue(owner, repo, getRequestPath(req));
    }
    return {
      ok: true,
      method: { kind: "github_token", owner, repo, state_id: stateId, limit_scope: policyState.limit_scope },
    };
  }

  try {
    const access = await verifyGitHubTokenRepoAccess(token, owner, repo);
    if (!access.ok && access.reason === "rejected") {
      return { ok: false, response: openaiError(401, "Invalid GitHub token for repo", "invalid_auth_for_repo") };
    }
    if (!access.ok) {
      return { ok: false, response: openaiError(502, "Failed to verify GitHub token", "bad_gateway") };
    }

    githubTokenCache.set(cacheKey, Date.now() + GITHUB_TOKEN_CACHE_TTL_MS);
    const policyState = await resolveKernelQuotaPolicyState(owner, repo);
    if (!policyState.ok) return { ok: false, response: policyState.response };
    if (!policyState.has_policy) {
      await recordKernelPolicyQueue(owner, repo, getRequestPath(req));
    }
    return {
      ok: true,
      method: { kind: "github_token", owner, repo, state_id: stateId, limit_scope: policyState.limit_scope },
    };
  } catch (error) {
    console.error("[ai.ubq.fi] GitHub token verification failed:", error);
    return { ok: false, response: openaiError(502, "Failed to verify GitHub token", "bad_gateway") };
  }
};

const looksLikeUbqAiClientToken = (token: string): boolean => looksLikeUosApiKey(token);

const classifyToken = (token: string): string => {
  const trimmed = token.trim();
  if (!trimmed) return "unset";
  if (trimmed.startsWith("ddw_")) return "deno_deploy_like(ddw_)";
  if (looksLikeGitHubToken(trimmed)) return "github_prefix";
  if (trimmed.startsWith("uos_ai_session_")) return "uos_ai_session_prefix";
  if (looksLikeUosApiKey(trimmed)) return "uos_api_key";
  if (/^[0-9a-fA-F]{64}$/.test(trimmed)) return "hex64";
  if (trimmed.includes("_")) return "has_underscore";
  return "other";
};

const getRequestPath = (req: Request): string => {
  try {
    return new URL(req.url).pathname;
  } catch {
    return "unknown";
  }
};

const isLocalClientAuthDisabledRequest = (req: Request): boolean => {
  if (config.isDeploy) return false;
  try {
    const hostname = new URL(req.url).hostname.toLowerCase();
    return hostname === "localhost" || hostname === "127.0.0.1" || hostname === "::1" || hostname === "[::1]";
  } catch {
    return false;
  }
};

type AuthLogEntry = Readonly<{
  scope: "client" | "admin";
  ok: boolean;
  method: string;
  status?: number;
  reason?: string;
  token_present: boolean;
  token_shape: string | null;
}>;

const getAuthHeaderSnapshot = (req: Request): Record<string, string | null> => {
  const owner = (req.headers.get("X-GitHub-Owner") ?? "").trim();
  const repo = (req.headers.get("X-GitHub-Repo") ?? "").trim();
  const installationId = (req.headers.get("X-GitHub-Installation-Id") ?? "").trim();
  const kernelTokenPresent = Boolean((req.headers.get("X-Ubiquity-Kernel-Token") ?? "").trim());
  return {
    "x-github-owner": owner || null,
    "x-github-repo": repo || null,
    "x-github-installation-id": installationId || null,
    "x-ubiquity-kernel-token": kernelTokenPresent ? "present" : "missing",
  };
};

const logAuthDecision = (req: Request, entry: AuthLogEntry): void => {
  if (entry.ok) return;
  const payload = {
    ...entry,
    path: getRequestPath(req),
    headers: getAuthHeaderSnapshot(req),
  };
  const line = JSON.stringify(payload);
  console.warn("[ai.ubq.fi] auth", line);
};

const getPasskeyCookieSessionForRequest = (req: Request) => {
  const passkeyHeaders = new Headers(req.headers);
  passkeyHeaders.delete("authorization");
  return getPasskeySessionForRequest(
    new Request(req.url, { headers: passkeyHeaders }),
  );
};

export const authenticateClient = async (req: Request): Promise<AuthenticateClientResult> => {
  const kv = await getKv();
  const localAuthDisabled = isLocalClientAuthDisabledRequest(req);
  const token = getBearerToken(req);
  const tokenPresent = Boolean(token);
  const tokenShape = token ? classifyToken(token) : null;
  const githubHeaders = token ? getGitHubRepoHeaders(req) : null;
  const githubCandidate = token ? looksLikeGitHubToken(token) : false;
  const logClientAuth = (entry: Omit<AuthLogEntry, "scope" | "token_present" | "token_shape">) =>
    logAuthDecision(req, {
      scope: "client",
      token_present: tokenPresent,
      token_shape: tokenShape,
      ...entry,
    });
  if (localAuthDisabled) {
    logClientAuth({ ok: true, method: "disabled" });
    return { ok: true, token, method: { kind: "disabled" } };
  }

  if (!token) {
    const passkeySession = await getPasskeySessionForRequest(req);
    if (passkeySession) {
      logClientAuth({ ok: true, method: "passkey_session" });
      return {
        ok: true,
        token: passkeySession.token,
        method: {
          kind: "passkey_session",
          user_id: passkeySession.user.id,
          handle: passkeySession.user.handle,
          is_admin: isPasskeyUserAdmin(passkeySession.user),
          credential_count: passkeySession.user.credential_ids.length,
        },
      };
    }
    logClientAuth({ ok: false, method: "missing", status: 401, reason: "missing_token" });
    return { ok: false, response: openaiError(401, "Unauthorized", "invalid_api_key") };
  }

  if (config.authTokens.has(token)) {
    logClientAuth({ ok: true, method: "auth_tokens_allowlist" });
    return { ok: true, token, method: { kind: "auth_tokens_allowlist" } };
  }

  if (token.startsWith("uos_ai_session_")) {
    const passkeySession = await getPasskeySessionForRequest(req);
    if (!passkeySession) {
      logClientAuth({ ok: false, method: "passkey_session", status: 401, reason: "invalid_api_key" });
      return { ok: false, response: openaiError(401, "Unauthorized", "invalid_api_key") };
    }
    logClientAuth({ ok: true, method: "passkey_session" });
    return {
      ok: true,
      token,
      method: {
        kind: "passkey_session",
        user_id: passkeySession.user.id,
        handle: passkeySession.user.handle,
        is_admin: isPasskeyUserAdmin(passkeySession.user),
        credential_count: passkeySession.user.credential_ids.length,
      },
    };
  }

  const githubResult = await authenticateGitHubToken(req, token);
  if (githubResult?.ok) {
    logClientAuth({ ok: true, method: "github_token" });
    return { ok: true, token, method: githubResult.method };
  }

  if (githubResult && !githubResult.ok && githubResult.response.status === 401 && req.headers.has("cookie")) {
    const passkeySession = await getPasskeyCookieSessionForRequest(req);
    if (passkeySession) {
      logClientAuth({ ok: true, method: "passkey_session" });
      return {
        ok: true,
        token: passkeySession.token,
        method: {
          kind: "passkey_session",
          user_id: passkeySession.user.id,
          handle: passkeySession.user.handle,
          is_admin: isPasskeyUserAdmin(passkeySession.user),
          credential_count: passkeySession.user.credential_ids.length,
        },
      };
    }
  }

  if (githubResult) {
    logClientAuth({
      ok: false,
      method: "github_token",
      status: githubResult.response.status,
      reason: "github_token_rejected",
    });
    return { ok: false, response: githubResult.response };
  }

  if (looksLikeUosApiKey(token)) {
    const result = await authenticateApiKeyToken(token, { kv });
    if (!result.ok) {
      logClientAuth({ ok: false, method: "kv_api_key", status: result.response.status, reason: "invalid_or_limited" });
      return result;
    }
    logClientAuth({ ok: true, method: "kv_api_key" });
    return {
      ok: true,
      token,
      method: {
        kind: "kv_api_key",
        key_id: result.policy.key_id,
        policy: result.policy,
      },
    };
  }

  const adminResult = await checkAdminToken(token);
  if (adminResult.ok) {
    logClientAuth({ ok: true, method: adminResult.kind });
    return { ok: true, token, method: { kind: adminResult.kind } };
  }
  if (adminResult.response) {
    logClientAuth({
      ok: false,
      method: "deno_deploy_token",
      status: adminResult.response.status,
      reason: "admin_token_verification_failed",
    });
    return { ok: false, response: adminResult.response };
  }

  if (kv) {
    logClientAuth({
      ok: false,
      method: githubCandidate && !githubHeaders ? "github_token" : "unknown",
      status: 401,
      reason: githubCandidate && !githubHeaders ? "missing_repo_headers" : "invalid_api_key",
    });
    return { ok: false, response: openaiError(401, "Unauthorized", "invalid_api_key") };
  }

  if (config.isDeploy && config.authTokens.size === 0) {
    logClientAuth({ ok: false, method: "server", status: 500, reason: "misconfigured" });
    return {
      ok: false,
      response: openaiError(500, "Server misconfigured: set UOS_AI_TOKEN or enable Deno KV", "server_error"),
    };
  }

  logClientAuth({ ok: false, method: "unknown", status: 401, reason: "invalid_api_key" });
  return { ok: false, response: openaiError(401, "Unauthorized", "invalid_api_key") };
};

export const requireClientAuth = async (req: Request): Promise<Response | null> => {
  const result = await authenticateClient(req);
  return result.ok ? null : result.response;
};

const DENO_API_V1_BASE_URL = "https://api.deno.com/v1";
const DENO_API_V2_BASE_URL = "https://api.deno.com/v2";
const DENO_CONSOLE_BASE_URL = "https://console.deno.com";
const DEPLOY_TOKEN_ADMIN_CACHE_TTL_MS = 10 * 60_000;
const deployTokenAdminCache = new Map<string, number>();

const looksLikeDenoDeployToken = (token: string): boolean => {
  const trimmed = token.trim();
  if (trimmed.length < 20) return false;
  if (trimmed.length > 500) return false;
  if (/\s/.test(trimmed)) return false;
  if (looksLikeUbqAiClientToken(trimmed)) return false;
  if (trimmed.startsWith("uos_ai_session_")) return false;
  if (!trimmed.includes("_")) return false;
  return true;
};

const fetchDenoApiOk = async (url: string, token: string): Promise<boolean> => {
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

const fetchDenoConsoleAppOk = async (orgSlug: string, appSlug: string, token: string): Promise<boolean> => {
  const url = `${DENO_CONSOLE_BASE_URL}/${encodeURIComponent(orgSlug)}/${encodeURIComponent(appSlug)}`;
  const res = await fetch(url, {
    method: "GET",
    headers: {
      "Accept": "text/html",
      "Cookie": `token=${token}; deno_auth_ghid=force`,
    },
    redirect: "manual",
  });

  if (!res.ok) {
    try {
      await res.body?.cancel();
    } catch {
      // ignore
    }
    return false;
  }

  const body = await res.text();
  const title = body.match(/<title[^>]*>([^<]*)<\/title>/i)?.[1] ?? "";
  return title.includes(`| ${appSlug} | Deploy`);
};

const verifyDenoDeployTokenForThisDeployment = async (token: string): Promise<boolean> => {
  const appSlug = (getEnv("DENO_DEPLOY_APP_SLUG") ?? "").trim();
  if (appSlug) {
    const appUrl = `${DENO_API_V2_BASE_URL}/apps/${encodeURIComponent(appSlug)}`;
    if (await fetchDenoApiOk(appUrl, token)) return true;
    const orgSlug = (getEnv("DENO_DEPLOY_ORG_SLUG") ?? "").trim();
    if (orgSlug && await fetchDenoConsoleAppOk(orgSlug, appSlug, token)) return true;
  }

  const deploymentId = (getEnv("DENO_DEPLOYMENT_ID") ?? "").trim();
  if (!deploymentId) return false;

  const url = `${DENO_API_V1_BASE_URL}/deployments/${deploymentId}`;
  return await fetchDenoApiOk(url, token);
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
  if (config.adminTokens.has(token)) {
    return { ok: true, kind: "admin_allowlist" };
  }

  // GitHub credentials must be handled by the attested GitHub path. Never
  // treat an unattested GitHub credential as a candidate Deno Deploy token.
  if (looksLikeGitHubToken(token)) return { ok: false, response: null };

  if (!looksLikeDenoDeployToken(token)) return { ok: false, response: null };

  const verified = await verifyDenoDeployTokenCached(token);
  if (verified.ok) return { ok: true, kind: "deno_deploy_token" };
  return verified;
};

export const authenticateAdmin = async (req: Request): Promise<AdminAuthResult> => {
  const token = getBearerToken(req);
  const tokenPresent = Boolean(token);
  const tokenShape = token ? classifyToken(token) : null;
  const logAdminAuth = (entry: Omit<AuthLogEntry, "scope" | "token_present" | "token_shape">) =>
    logAuthDecision(req, {
      scope: "admin",
      token_present: tokenPresent,
      token_shape: tokenShape,
      ...entry,
    });
  if (isAdminAuthDisabledForRequest(req)) {
    logAdminAuth({ ok: true, method: "disabled" });
    return {
      ok: true,
      token: token ?? "local-dev-admin",
      is_super_admin: true,
      method: { kind: "disabled" },
    };
  }
  let passkeySession = await getPasskeySessionForRequest(req);
  if (
    !passkeySession && token && looksLikeGitHubToken(token) && !config.adminTokens.has(token) &&
    !config.authTokens.has(token) &&
    req.headers.has("cookie")
  ) {
    const githubResult = await authenticateGitHubToken(req, token);
    if (githubResult?.ok) {
      logAdminAuth({ ok: false, method: "github_token", status: 401, reason: "github_token_not_admin" });
      return { ok: false, response: openaiError(401, "Unauthorized", "invalid_api_key") };
    }
    if (githubResult && githubResult.response.status !== 401) {
      logAdminAuth({
        ok: false,
        method: "github_token",
        status: githubResult.response.status,
        reason: "github_token_verification_failed",
      });
      return { ok: false, response: githubResult.response };
    }
    passkeySession = await getPasskeyCookieSessionForRequest(req);
  }
  if (passkeySession) {
    if (!isPasskeyUserAdmin(passkeySession.user)) {
      logAdminAuth({ ok: false, method: "passkey_session", status: 403, reason: "passkey_user_not_admin" });
      return { ok: false, response: openaiError(403, "Forbidden", "forbidden") };
    }
    logAdminAuth({ ok: true, method: "passkey_session" });
    return {
      ok: true,
      token: passkeySession.token,
      is_super_admin: false,
      method: {
        kind: "passkey_session",
        user_id: passkeySession.user.id,
        handle: passkeySession.user.handle,
        is_admin: true,
        credential_count: passkeySession.user.credential_ids.length,
      },
    };
  }

  if (!token) {
    logAdminAuth({ ok: false, method: "missing", status: 401, reason: "missing_token" });
    return { ok: false, response: openaiError(401, "Unauthorized", "invalid_api_key") };
  }

  if (looksLikeGitHubToken(token) && !config.adminTokens.has(token)) {
    logAdminAuth({ ok: false, method: "github_token", status: 401, reason: "missing_github_attestation" });
    return { ok: false, response: openaiError(401, "Unauthorized", "invalid_api_key") };
  }

  if (config.adminTokens.size === 0 && !looksLikeDenoDeployToken(token)) {
    logAdminAuth({ ok: false, method: "admin_allowlist", status: 401, reason: "admin_tokens_unconfigured" });
    return { ok: false, response: openaiError(401, "Unauthorized", "invalid_api_key") };
  }

  const result = await checkAdminToken(token);
  if (result.ok) {
    logAdminAuth({ ok: true, method: result.kind });
    return { ok: true, token, is_super_admin: true, method: { kind: result.kind } };
  }
  if (result.response) {
    logAdminAuth({
      ok: false,
      method: "deno_deploy_token",
      status: result.response.status,
      reason: "admin_token_verification_failed",
    });
    return { ok: false, response: result.response };
  }
  logAdminAuth({ ok: false, method: "admin_allowlist", status: 401, reason: "invalid_admin_token" });
  return { ok: false, response: openaiError(401, "Unauthorized", "invalid_api_key") };
};

export const requireAdminAuth = async (req: Request): Promise<Response | null> => {
  const result = await authenticateAdmin(req);
  return result.ok ? null : result.response;
};

export const requireSuperAdminAuth = async (req: Request): Promise<Response | null> => {
  const result = await authenticateAdmin(req);
  if (!result.ok) return result.response;
  if (result.is_super_admin) return null;
  return openaiError(403, "Super admin token required", "forbidden");
};

export const handleV1Auth = async (req: Request): Promise<Response> => {
  const authResult = await authenticateClient(req);
  if (!authResult.ok) return authResult.response;

  const kv = await getKv();
  const localClientAuthDisabled = isLocalClientAuthDisabledRequest(req);
  const mode = localClientAuthDisabled
    ? "disabled"
    : config.isDeploy && config.authTokens.size === 0 && !kv
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

  // Local client auth remains disabled for the development inference surface,
  // but admin access is resolved independently. This keeps localhost from
  // implicitly becoming an admin while still recognizing an explicit admin
  // credential when the CLI bypass is off.
  const localAdminAuth = localClientAuthDisabled ? await authenticateAdmin(req) : null;
  const reportingMethod = localAdminAuth?.ok ? localAdminAuth.method : authResult.method;
  const method: Record<string, unknown> = { kind: reportingMethod.kind };
  const isAdmin = localAdminAuth?.ok === true || reportingMethod.kind === "admin_allowlist" ||
    reportingMethod.kind === "deno_deploy_token" ||
    (reportingMethod.kind === "passkey_session" && reportingMethod.is_admin);
  const isSuperAdmin = localAdminAuth?.ok
    ? localAdminAuth.is_super_admin
    : reportingMethod.kind === "admin_allowlist" || reportingMethod.kind === "deno_deploy_token";

  if (reportingMethod.kind === "github_token") {
    method.repo = { owner: reportingMethod.owner, repo: reportingMethod.repo };
    method.state_id = reportingMethod.state_id;
    method.limit_scope = reportingMethod.limit_scope;
  }

  if (reportingMethod.kind === "kv_api_key") {
    const id = reportingMethod.key_id;
    let key: Record<string, unknown> = { id };
    if (kv) {
      const entry = await kv.get<ApiKeyRecord>(apiKeyIdKey(id));
      if (entry.value) {
        let usageRequests: number;
        try {
          usageRequests = await getApiKeyUsageV3(reportingMethod.policy, kv);
        } catch (error) {
          console.warn("[ai.ubq.fi] Failed to read API key quota usage for /uos/auth:", error);
          return openaiError(503, "API key quota ledger is unavailable", "server_error", { type: "server_error" });
        }
        key = {
          id: entry.value.id,
          name: entry.value.name,
          prefix: entry.value.prefix,
          created_at_ms: entry.value.created_at_ms,
          expires_at_ms: coerceApiKeyExpiresAtMs(entry.value),
          revoked_at_ms: entry.value.revoked_at_ms,
          usage_limit_requests: reportingMethod.policy.usage_limit_requests,
          usage_requests: usageRequests,
          usage_reset_at_ms: reportingMethod.policy.usage_reset_at_ms,
          window_ms: reportingMethod.policy.window_ms,
        };
      }
    }
    method.key = key;
  }

  if (reportingMethod.kind === "passkey_session") {
    method.user = {
      id: reportingMethod.user_id,
      handle: reportingMethod.handle,
      is_admin: reportingMethod.is_admin,
      credential_count: reportingMethod.credential_count,
    };
  }

  return json(
    200,
    {
      ok: true,
      service: "ai.ubq.fi",
      auth: {
        mode,
        is_admin: isAdmin,
        is_super_admin: isSuperAdmin,
        method,
        token: tokenInfo,
      },
    },
    { "Cache-Control": "no-store" },
  );
};
