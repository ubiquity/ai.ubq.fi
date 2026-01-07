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
  apiKeyHashKey,
  apiKeyIdKey,
  coerceApiKeyExpiresAtMs,
  generateApiKeyToken,
} from "./api_keys.ts";
import { getApiKeyUsage } from "./analytics.ts";
import { kvPromise } from "./kv.ts";
import { readJsonBody } from "./request.ts";
import { getString, isRecord, sha256Base64Url } from "./utils.ts";
import type { ApiKeyHashRecord, ApiKeyRecord, CodexAuthState } from "./types.ts";

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
  if (value === undefined || value === null) return API_KEY_NO_EXPIRATION_MS;
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

  const hash = await sha256Base64Url(token);
  const hashKey = apiKeyHashKey(hash);
  const hashEntry = await kv.get<ApiKeyHashRecord>(hashKey);
  if (hashEntry.value) {
    return openaiError(409, "API key already exists", "invalid_request_error");
  }

  const id = crypto.randomUUID();
  const record: ApiKeyRecord = {
    id,
    name,
    prefix: token.slice(0, 12),
    hash,
    created_at_ms: now,
    expires_at_ms: expiresAtMs,
    revoked_at_ms: null,
  };
  const hashRecord: ApiKeyHashRecord = { id, expires_at_ms: expiresAtMs, revoked_at_ms: null };

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
  if (includeUsage) {
    for (const record of records) {
      usageById.set(record.id, await getApiKeyUsage(record.id));
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
        ...(includeUsage ? { usage: usageById.get(r.id) ?? null } : {}),
      })),
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
