import {
  cacheCodexAuth,
  CODEX_KV_KEY,
  CodexError,
  getJwtExpMs,
  parseCodexAuthFromAuthJson,
  validateCodexAuthJson,
} from "./codex.ts";
import { json, openaiError } from "./http.ts";
import {
  API_KEY_ID_PREFIX,
  API_KEY_NO_EXPIRATION_MS,
  API_KEY_NO_USAGE_LIMIT,
  apiKeyHashKey,
  apiKeyIdKey,
  calculateNextResetMs,
  coerceApiKeyExpiresAtMs,
  DEFAULT_USAGE_LIMIT_REQUESTS,
  generateApiKeyToken,
  getDefaultExpiryMs,
} from "./api_keys.ts";
import { getApiKeyUsage } from "./analytics.ts";
import { reloadKernelPublicKeys } from "./auth.ts";
import {
  deleteKernelOrgUsageLimit,
  deleteKernelUsageLimit,
  getKernelOrgUsage,
  getKernelOrgUsageLimitSnapshot,
  getKernelUsage,
  getKernelUsageLimitSnapshot,
  listKernelOrgUsageLimits,
  listKernelUsageLimits,
  setKernelOrgUsageLimit,
  setKernelUsageLimit,
} from "./kernel_usage.ts";
import { kvPromise } from "./kv.ts";
import { readJsonBody } from "./request.ts";
import { getString, isRecord, sha256Base64Url } from "./utils.ts";
import type { ApiKeyHashRecord, ApiKeyRecord, CodexAuthState } from "./types.ts";

const UOS_KERNEL_PUBKEYS_KEY = ["uos_ai", "kernel_pubkeys"];

export const handleAdminCodexAuth = async (req: Request): Promise<Response> => {
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
    if (error instanceof CodexError) {
      return openaiError(error.status, error.message, error.code);
    }
    const detail = error instanceof Error ? error.message : String(error);
    const message = detail ? `Upstream validation request failed: ${detail}` : "Upstream validation request failed.";
    return openaiError(502, message, "codex_upstream_unreachable");
  }

  if (!validated.ok) {
    return openaiError(
      401,
      `Invalid Codex auth.json (upstream ${validated.status}): ${validated.body}`,
      "invalid_api_key",
    );
  }

  await kv.set(CODEX_KV_KEY, validated.auth);
  cacheCodexAuth(validated.auth);

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

const normalizeApiKeyExpiresAtMs = (value: unknown, nowMs: number): number | null => {
  if (value === undefined || value === null) return getDefaultExpiryMs(nowMs);
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  const expiresAtMs = Math.trunc(value);
  if (expiresAtMs === API_KEY_NO_EXPIRATION_MS) return API_KEY_NO_EXPIRATION_MS;
  if (expiresAtMs < 0) return null;
  if (expiresAtMs <= nowMs) return null;
  return expiresAtMs;
};

const shouldIncludeUsage = (value: string | null): boolean => {
  if (!value) return false;
  const normalized = value.trim().toLowerCase();
  return normalized === "1" || normalized === "true" || normalized === "yes";
};

const normalizeApiKeyUsageLimit = (value: unknown): number | null => {
  if (value === undefined || value === null) return DEFAULT_USAGE_LIMIT_REQUESTS;
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  const limit = Math.trunc(value);
  if (limit === API_KEY_NO_USAGE_LIMIT) return API_KEY_NO_USAGE_LIMIT;
  if (limit < 0) return null;
  return limit;
};

const normalizeKernelRepoPart = (value: unknown): string | null => {
  if (typeof value !== "string") return null;
  const trimmed = value.trim();
  if (!trimmed) return null;
  if (trimmed.length > 100) return null;
  if (/\s/.test(trimmed)) return null;
  if (trimmed.includes("/")) return null;
  return trimmed;
};

const normalizeKernelUsageLimitInput = (value: unknown): number | null => {
  if (value === undefined || value === null) return null;
  if (typeof value === "string") {
    const trimmed = value.trim().toLowerCase();
    if (!trimmed) return null;
    if (trimmed === "unlimited") return API_KEY_NO_USAGE_LIMIT;
    const parsed = Number(trimmed);
    if (!Number.isFinite(parsed)) return null;
    value = parsed;
  }
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  const limit = Math.trunc(value);
  if (limit === API_KEY_NO_USAGE_LIMIT) return API_KEY_NO_USAGE_LIMIT;
  if (limit < 0) return null;
  return limit;
};

const normalizeKernelWindowMsInput = (value: unknown): number | null => {
  if (value === undefined || value === null) return null;
  if (typeof value === "string") {
    const trimmed = value.trim();
    if (!trimmed) return null;
    const parsed = Number(trimmed);
    if (!Number.isFinite(parsed)) return null;
    value = parsed;
  }
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  const windowMs = Math.trunc(value);
  if (windowMs <= 0) return null;
  return windowMs;
};

const normalizeKernelScope = (value: unknown): "repo" | "org" => {
  if (typeof value !== "string") return "repo";
  const normalized = value.trim().toLowerCase();
  if (normalized === "org") return "org";
  return "repo";
};

const normalizeOptionalBoolean = (value: unknown): boolean => {
  if (value === true) return true;
  if (typeof value !== "string") return false;
  const trimmed = value.trim().toLowerCase();
  return trimmed === "true" || trimmed === "1" || trimmed === "yes";
};

export const handleAdminApiKeysCreate = async (req: Request): Promise<Response> => {
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

  const now = Date.now();
  const expiresAtMs = normalizeApiKeyExpiresAtMs(raw.expires_at_ms, now);
  if (expiresAtMs === null) {
    return openaiError(
      400,
      "expires_at_ms must be a Unix epoch ms timestamp in the future, or -1",
      "invalid_request_error",
    );
  }

  const usageLimitRequests = normalizeApiKeyUsageLimit(raw.usage_limit_requests);
  if (usageLimitRequests === null) {
    return openaiError(
      400,
      "usage_limit_requests must be a positive number or -1 for unlimited",
      "invalid_request_error",
    );
  }

  const hash = await sha256Base64Url(token);
  const hashKey = apiKeyHashKey(hash);
  const hashEntry = await kv.get<ApiKeyHashRecord>(hashKey);
  if (hashEntry.value) {
    return openaiError(409, "API key already exists", "invalid_request_error");
  }

  const id = crypto.randomUUID();
  const usageResetAtMs = calculateNextResetMs(now);
  const record: ApiKeyRecord = {
    id,
    name,
    prefix: token.slice(0, 12),
    hash,
    created_at_ms: now,
    expires_at_ms: expiresAtMs,
    revoked_at_ms: null,
    usage_limit_requests: usageLimitRequests,
    usage_requests: 0,
    usage_reset_at_ms: usageResetAtMs,
  };
  const hashRecord: ApiKeyHashRecord = {
    id,
    expires_at_ms: expiresAtMs,
    revoked_at_ms: null,
    usage_limit_requests: usageLimitRequests,
    usage_requests: 0,
    usage_reset_at_ms: usageResetAtMs,
  };

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
      expires_at_ms: record.expires_at_ms,
      usage_limit_requests: record.usage_limit_requests,
      usage_requests: record.usage_requests,
      usage_reset_at_ms: record.usage_reset_at_ms,
    },
    { "x-ubq-upstream": "chatgpt_codex" },
  );
};

export const handleAdminApiKeysList = async (req: Request): Promise<Response> => {
  const kv = await kvPromise;
  if (!kv) {
    return openaiError(500, "Deno KV is not available; cannot manage API keys", "server_error");
  }

  const records: ApiKeyRecord[] = [];
  for await (const entry of kv.list<ApiKeyRecord>({ prefix: API_KEY_ID_PREFIX })) {
    if (entry.value) records.push(entry.value);
  }
  records.sort((a, b) => b.created_at_ms - a.created_at_ms);

  const includeUsage = shouldIncludeUsage(new URL(req.url).searchParams.get("include_usage"));
  const usageById = new Map<string, Awaited<ReturnType<typeof getApiKeyUsage>>>();
  const dailyDays = 30;
  if (includeUsage) {
    for (const record of records) {
      usageById.set(record.id, await getApiKeyUsage(record.id, { includeDaily: true, dailyDays }));
    }
  }

  return json(
    200,
    {
      object: "list",
      data: records.map((r) => ({
        id: r.id,
        name: r.name,
        prefix: r.prefix,
        created_at_ms: r.created_at_ms,
        expires_at_ms: coerceApiKeyExpiresAtMs(r),
        revoked_at_ms: r.revoked_at_ms,
        usage_limit_requests: r.usage_limit_requests,
        usage_requests: r.usage_requests,
        usage_reset_at_ms: r.usage_reset_at_ms,
        ...(includeUsage ? { usage: usageById.get(r.id) ?? null } : {}),
      })),
    },
    { "x-ubq-upstream": "chatgpt_codex" },
  );
};

export const handleAdminApiKeysUpdate = async (req: Request): Promise<Response> => {
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
  const currentExpiresAtMs = coerceApiKeyExpiresAtMs(entry.value);
  let nextName = entry.value.name;
  let nextExpiresAtMs = currentExpiresAtMs;
  let nextUsageLimit = entry.value.usage_limit_requests;
  let nextUsageRequests = entry.value.usage_requests;
  let nextUsageResetAtMs = entry.value.usage_reset_at_ms;

  if (Object.prototype.hasOwnProperty.call(raw, "name")) {
    const name = normalizeApiKeyName(raw.name);
    if (!name) return openaiError(400, "name must be a non-empty string (<=80 chars)", "invalid_request_error");
    nextName = name;
  }

  if (Object.prototype.hasOwnProperty.call(raw, "expires_at_ms")) {
    const expiresAtMs = normalizeApiKeyExpiresAtMs(raw.expires_at_ms, now);
    if (expiresAtMs === null) {
      return openaiError(
        400,
        "expires_at_ms must be a Unix epoch ms timestamp in the future, or -1",
        "invalid_request_error",
      );
    }
    nextExpiresAtMs = expiresAtMs;
  }

  if (Object.prototype.hasOwnProperty.call(raw, "usage_limit_requests")) {
    const usageLimitRequests = normalizeApiKeyUsageLimit(raw.usage_limit_requests);
    if (usageLimitRequests === null) {
      return openaiError(
        400,
        "usage_limit_requests must be a non-negative number or -1 for unlimited",
        "invalid_request_error",
      );
    }
    nextUsageLimit = usageLimitRequests;
  }

  const resetUsage = normalizeOptionalBoolean(raw.reset_usage);
  if (resetUsage) {
    nextUsageRequests = 0;
    nextUsageResetAtMs = calculateNextResetMs(now);
  }

  const hasChanges = nextName !== entry.value.name ||
    nextExpiresAtMs !== currentExpiresAtMs ||
    nextUsageLimit !== entry.value.usage_limit_requests ||
    (resetUsage &&
      (nextUsageRequests !== entry.value.usage_requests || nextUsageResetAtMs !== entry.value.usage_reset_at_ms));

  if (!hasChanges) {
    return json(
      200,
      {
        ok: true,
        id: entry.value.id,
        name: entry.value.name,
        prefix: entry.value.prefix,
        created_at_ms: entry.value.created_at_ms,
        expires_at_ms: currentExpiresAtMs,
        revoked_at_ms: entry.value.revoked_at_ms,
        usage_limit_requests: entry.value.usage_limit_requests,
        usage_requests: entry.value.usage_requests,
        usage_reset_at_ms: entry.value.usage_reset_at_ms,
      },
      { "x-ubq-upstream": "chatgpt_codex" },
    );
  }

  const updated: ApiKeyRecord = {
    ...entry.value,
    name: nextName,
    expires_at_ms: nextExpiresAtMs,
    usage_limit_requests: nextUsageLimit,
    usage_requests: nextUsageRequests,
    usage_reset_at_ms: nextUsageResetAtMs,
  };
  const hashKey = apiKeyHashKey(entry.value.hash);
  const hashEntry = await kv.get<ApiKeyHashRecord>(hashKey);
  const updatedHash: ApiKeyHashRecord = {
    id: updated.id,
    expires_at_ms: updated.expires_at_ms,
    revoked_at_ms: updated.revoked_at_ms,
    usage_limit_requests: updated.usage_limit_requests,
    usage_requests: updated.usage_requests,
    usage_reset_at_ms: updated.usage_reset_at_ms,
  };

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
      name: updated.name,
      prefix: updated.prefix,
      created_at_ms: updated.created_at_ms,
      expires_at_ms: coerceApiKeyExpiresAtMs(updated),
      revoked_at_ms: updated.revoked_at_ms,
      usage_limit_requests: updated.usage_limit_requests,
      usage_requests: updated.usage_requests,
      usage_reset_at_ms: updated.usage_reset_at_ms,
    },
    { "x-ubq-upstream": "chatgpt_codex" },
  );
};

export const handleAdminApiKeysRevoke = async (req: Request): Promise<Response> => {
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
  const expiresAtMs = coerceApiKeyExpiresAtMs(entry.value);
  const updated: ApiKeyRecord = entry.value.revoked_at_ms
    ? { ...entry.value, expires_at_ms: expiresAtMs }
    : { ...entry.value, expires_at_ms: expiresAtMs, revoked_at_ms: now };
  const hashKey = apiKeyHashKey(entry.value.hash);
  const hashEntry = await kv.get<ApiKeyHashRecord>(hashKey);
  const updatedHash: ApiKeyHashRecord = {
    id,
    expires_at_ms: updated.expires_at_ms,
    revoked_at_ms: updated.revoked_at_ms,
  };

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

export const handleAdminApiKeysDelete = async (req: Request): Promise<Response> => {
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

  if (!entry.value.revoked_at_ms) {
    return openaiError(400, "Only revoked keys can be deleted", "invalid_request_error");
  }

  const atomic = kv.atomic()
    .check(entry)
    .delete(idKey)
    .delete(apiKeyHashKey(entry.value.hash))
    .delete(["ubq_ai", "api_keys", "usage", id]);

  const commit = await atomic.commit();
  if (!commit.ok) {
    return openaiError(409, "API key was modified concurrently; retry", "invalid_request_error");
  }

  return json(200, { ok: true, id }, { "x-ubq-upstream": "chatgpt_codex" });
};

const REASONING_EFFORTS: ReadonlySet<string> = new Set(["none", "minimal", "low", "medium", "high", "xhigh"]);

const DEFAULT_REASONING_EFFORT_KEY = ["default", "reasoning_effort"];

export const handleAdminReasoningLevel = async (req: Request): Promise<Response> => {
  const kv = await kvPromise;
  if (!kv) {
    return openaiError(500, "Deno KV is not available; cannot manage reasoning level", "server_error");
  }

  if (req.method === "GET") {
    const entry = await kv.get<string>(DEFAULT_REASONING_EFFORT_KEY);
    const effort = entry.value ?? "xhigh";
    return json(200, { effort }, { "x-ubq-upstream": "chatgpt_codex" });
  }

  if (req.method === "POST") {
    const raw = await readJsonBody(req);
    if (!raw || !isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

    const effort = getString(raw.effort);
    if (!effort || !REASONING_EFFORTS.has(effort)) {
      return openaiError(400, "effort must be one of: none, minimal, low, medium, high, xhigh", "invalid_request_error");
    }

    await kv.set(DEFAULT_REASONING_EFFORT_KEY, effort);
    return json(200, { ok: true, effort }, { "x-ubq-upstream": "chatgpt_codex" });
  }

  return openaiError(405, "Method not allowed", "method_not_allowed");
};

const normalizePem = (raw: unknown): string | null => {
  if (typeof raw !== "string") return null;
  const pem = raw.trim();
  if (!pem.startsWith("-----BEGIN PUBLIC KEY-----") || !pem.endsWith("-----END PUBLIC KEY-----")) return null;
  return pem;
};

export const handleAdminKernelPubKeysList = async (): Promise<Response> => {
  const kv = await kvPromise;
  if (!kv) return openaiError(500, "Deno KV is not available", "server_error");
  const kvEntry = await kv.get<Array<{ app_id: number; pem: string; owner: string; added_at_ms: number }>>(
    UOS_KERNEL_PUBKEYS_KEY,
  );
  return json(200, { data: kvEntry.value ?? [] });
};

export const handleAdminKernelPubKeysCreate = async (req: Request): Promise<Response> => {
  const kv = await kvPromise;
  if (!kv) return openaiError(500, "Deno KV is not available", "server_error");

  const raw = await readJsonBody(req);
  if (!raw || !isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const appId = typeof raw.app_id === "number" ? raw.app_id : null;
  if (appId === null) return openaiError(400, "app_id is required and must be a number", "invalid_request_error");

  const pem = normalizePem(raw.pem);
  if (!pem) return openaiError(400, "pem must be a valid RS256 public PEM", "invalid_request_error");

  const owner = getString(raw.owner) ?? "unknown";

  const entry = await kv.get<Array<{ app_id: number; pem: string; owner: string; added_at_ms: number }>>(
    UOS_KERNEL_PUBKEYS_KEY,
  );
  const existing = entry.value ?? [];
  if (existing.some((p) => p.app_id === appId)) {
    return openaiError(409, `Public key for App ID ${appId} already exists`, "invalid_request_error");
  }

  const record = { app_id: appId, pem, owner, added_at_ms: Date.now() };
  const updated = [...existing, record];

  const commit = await kv.atomic().check(entry).set(UOS_KERNEL_PUBKEYS_KEY, updated).commit();
  if (!commit.ok) return openaiError(409, "Concurrent modification; retry", "invalid_request_error");

  await reloadKernelPublicKeys();
  return json(200, { ok: true, data: record });
};

export const handleAdminKernelPubKeysDelete = async (req: Request): Promise<Response> => {
  const kv = await kvPromise;
  if (!kv) return openaiError(500, "Deno KV is not available", "server_error");

  const url = new URL(req.url);
  const appIdStr = url.searchParams.get("app_id");
  const appId = appIdStr ? parseInt(appIdStr, 10) : null;
  if (appId === null || isNaN(appId)) {
    return openaiError(400, "app_id query parameter is required and must be a number", "invalid_request_error");
  }

  const entry = await kv.get<Array<{ app_id: number; pem: string; owner: string; added_at_ms: number }>>(
    UOS_KERNEL_PUBKEYS_KEY,
  );
  const existing = entry.value ?? [];
  const updated = existing.filter((p) => p.app_id !== appId);

  if (updated.length === existing.length) return openaiError(404, "Not found", "not_found");

  const commit = await kv.atomic().check(entry).set(UOS_KERNEL_PUBKEYS_KEY, updated).commit();
  if (!commit.ok) return openaiError(409, "Concurrent modification; retry", "invalid_request_error");

  await reloadKernelPublicKeys();
  return json(200, { ok: true, deleted_app_id: appId });
};

export const handleAdminKernelUsageGet = async (req: Request): Promise<Response> => {
  const kv = await kvPromise;
  if (!kv) return openaiError(500, "Deno KV is not available", "server_error");

  const url = new URL(req.url);
  const scope = normalizeKernelScope(url.searchParams.get("scope"));
  const listRequested = shouldIncludeUsage(url.searchParams.get("list"));
  if (listRequested) {
    if (scope === "org") {
      const limits = await listKernelOrgUsageLimits();
      if (!limits) {
        return openaiError(500, "Failed to load kernel org usage limits", "server_error");
      }
      return json(200, { ok: true, scope, limits });
    }

    const limits = await listKernelUsageLimits();
    if (!limits) {
      return openaiError(500, "Failed to load kernel usage limits", "server_error");
    }
    return json(200, { ok: true, scope, limits });
  }

  const owner = normalizeKernelRepoPart(url.searchParams.get("owner"));
  if (!owner) {
    return openaiError(400, "owner query parameter is required", "invalid_request_error");
  }

  if (scope === "org") {
    const limitSnapshot = await getKernelOrgUsageLimitSnapshot(owner);
    if (!limitSnapshot) {
      return openaiError(500, "Failed to load kernel org usage limit", "server_error");
    }
    const usage = await getKernelOrgUsage(owner);
    return json(200, {
      ok: true,
      org: { owner },
      limit: { ...limitSnapshot.record, source: limitSnapshot.source },
      usage: usage ?? null,
    });
  }

  const repo = normalizeKernelRepoPart(url.searchParams.get("repo"));
  if (!repo) {
    return openaiError(400, "repo query parameter is required", "invalid_request_error");
  }

  const limitSnapshot = await getKernelUsageLimitSnapshot(owner, repo);
  if (!limitSnapshot) {
    return openaiError(500, "Failed to load kernel usage limit", "server_error");
  }
  const usage = await getKernelUsage(owner, repo);

  return json(200, {
    ok: true,
    repo: { owner, repo },
    limit: { ...limitSnapshot.record, source: limitSnapshot.source },
    usage: usage ?? null,
  });
};

export const handleAdminKernelUsageSet = async (req: Request): Promise<Response> => {
  const kv = await kvPromise;
  if (!kv) return openaiError(500, "Deno KV is not available", "server_error");

  const raw = await readJsonBody(req);
  if (!raw || !isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const owner = normalizeKernelRepoPart(raw.owner);
  const repo = normalizeKernelRepoPart(raw.repo);
  if (!owner) return openaiError(400, "owner is required", "invalid_request_error");
  const scope = normalizeKernelScope(raw.scope ?? (repo ? "repo" : "org"));
  if (scope === "repo" && !repo) {
    return openaiError(400, "repo is required for scope=repo", "invalid_request_error");
  }
  if (scope === "org" && repo) {
    return openaiError(400, "repo must be omitted for scope=org", "invalid_request_error");
  }

  const usageLimitRequests = normalizeKernelUsageLimitInput(raw.usage_limit_requests);
  if (usageLimitRequests === null) {
    return openaiError(
      400,
      "usage_limit_requests must be a non-negative number, -1, or 'unlimited'",
      "invalid_request_error",
    );
  }

  const windowMs = normalizeKernelWindowMsInput(raw.window_ms);
  if (raw.window_ms !== undefined && windowMs === null) {
    return openaiError(400, "window_ms must be a positive number", "invalid_request_error");
  }

  if (scope === "org") {
    const updated = await setKernelOrgUsageLimit(owner, usageLimitRequests, {
      windowMs: windowMs ?? undefined,
    });
    if (!updated) {
      return openaiError(409, "Concurrent modification; retry", "invalid_request_error");
    }
    return json(200, { ok: true, scope, org: { owner }, limit: { ...updated, source: "kv" } });
  }

  const updated = await setKernelUsageLimit(owner, repo!, usageLimitRequests, {
    windowMs: windowMs ?? undefined,
  });
  if (!updated) {
    return openaiError(409, "Concurrent modification; retry", "invalid_request_error");
  }

  return json(200, { ok: true, scope, repo: { owner, repo }, limit: { ...updated, source: "kv" } });
};

export const handleAdminKernelUsageDelete = async (req: Request): Promise<Response> => {
  const kv = await kvPromise;
  if (!kv) return openaiError(500, "Deno KV is not available", "server_error");

  const raw = await readJsonBody(req);
  if (!raw || !isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const owner = normalizeKernelRepoPart(raw.owner);
  const repo = normalizeKernelRepoPart(raw.repo);
  if (!owner) return openaiError(400, "owner is required", "invalid_request_error");
  const scope = normalizeKernelScope(raw.scope ?? (repo ? "repo" : "org"));
  if (scope === "repo" && !repo) {
    return openaiError(400, "repo is required for scope=repo", "invalid_request_error");
  }
  if (scope === "org" && repo) {
    return openaiError(400, "repo must be omitted for scope=org", "invalid_request_error");
  }

  if (scope === "org") {
    const deleted = await deleteKernelOrgUsageLimit(owner);
    if (deleted === null) {
      return openaiError(500, "Failed to delete kernel org usage limit", "server_error");
    }
    if (!deleted) {
      return openaiError(404, "Kernel org usage limit not found", "not_found");
    }
    return json(200, { ok: true, scope, org: { owner }, deleted: true });
  }

  const deleted = await deleteKernelUsageLimit(owner, repo!);
  if (deleted === null) {
    return openaiError(500, "Failed to delete kernel usage limit", "server_error");
  }
  if (!deleted) {
    return openaiError(404, "Kernel usage limit not found", "not_found");
  }

  return json(200, { ok: true, scope, repo: { owner, repo }, deleted: true });
};
