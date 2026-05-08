import {
  cacheCodexAuth,
  CODEX_KV_KEY,
  CODEX_MODELS_KV_KEY,
  CodexError,
  type CodexModelsSnapshot,
  getCodexModelsSnapshotDefaultModel,
  getJwtExpMs,
  loadCodexModelsSnapshot,
  parseCodexAuthFromAuthJson,
  storeCodexModelsSnapshot,
  validateCodexAuthJson,
} from "./codex.ts";
import {
  DEFAULT_KERNEL_POLICY_LIMIT_KEY,
  DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS,
  DEFAULT_KERNEL_POLICY_WINDOW_KEY,
  DEFAULT_KERNEL_POLICY_WINDOW_MS,
  DEFAULT_MODEL_KEY,
  DEFAULT_REASONING_EFFORT,
  DEFAULT_REASONING_EFFORT_KEY,
  normalizeReasoningEffort,
  type ReasoningEffort,
} from "./defaults.ts";
import { json, openaiError } from "./http.ts";
import {
  API_KEY_ID_PREFIX,
  API_KEY_NO_EXPIRATION_MS,
  API_KEY_NO_USAGE_LIMIT,
  apiKeyHashKey,
  apiKeyIdKey,
  calculateNextResetMs,
  coerceApiKeyExpiresAtMs,
  coerceApiKeyWindowMs,
  DEFAULT_USAGE_LIMIT_REQUESTS,
  generateApiKeyToken,
  getDefaultExpiryMs,
  USAGE_RESET_PERIOD_MS,
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
  kernelLimitKey,
  kernelOrgLimitKey,
  listKernelOrgUsageLimits,
  listKernelOrgUsageRecords,
  listKernelUsageLimits,
  listKernelUsageRecords,
  setKernelOrgUsageLimit,
  setKernelUsageLimit,
} from "./kernel_usage.ts";
import { listKernelPolicyQueue } from "./kernel_policy_queue.ts";
import {
  defaultIncludeLegacyForProfile,
  importKvMigrationLines,
  type KvMigrationProfile,
  validateKvMigrationTarget,
} from "./kv_migration.ts";
import { kvPromise } from "./kv.ts";
import { readJsonBody } from "./request.ts";
import { getString, isRecord, sha256Base64Url } from "./utils.ts";
import type { ApiKeyHashRecord, ApiKeyRecord, CodexAuthState } from "./types.ts";

const UOS_KERNEL_PUBKEYS_KEY = ["uos_ai", "kernel_pubkeys"];
const UOS_CODEX_PROMPTS_KEY = ["uos_ai", "codex_instructions"] as const;
const UOS_CODEX_PROMPTS_CHUNK_PREFIX = ["uos_ai", "codex_instructions_chunk"] as const;
const MAX_KV_MIGRATION_BODY_BYTES = 5 * 1024 * 1024;

const isHiddenCodexModel = (value: Record<string, unknown>): boolean =>
  getString(value.visibility)?.trim().toLowerCase() === "hide";

const resolveDefaultModel = async (entryValue: unknown): Promise<string> => {
  const configured = typeof entryValue === "string" ? entryValue.trim() : "";
  if (configured) return configured;
  return getCodexModelsSnapshotDefaultModel(await loadCodexModelsSnapshot()) ?? "";
};

export const handleAdminCodexAuth = async (req: Request): Promise<Response> => {
  const kv = await kvPromise;
  if (!kv) {
    return openaiError(500, "Deno KV is not available; cannot persist Codex auth", "server_error");
  }

  const body = await readJsonBody(req);
  const authPayload = isRecord(body) && "auth" in body ? (body.auth as unknown) : body;
  const modelsPayload = isRecord(body) && "models" in body ? (body.models as unknown) : undefined;
  const tokenData = parseCodexAuthFromAuthJson(authPayload);
  if (!tokenData) {
    return openaiError(400, "Body does not look like a Codex auth.json", "invalid_request_error");
  }

  const seed: CodexAuthState = { ...tokenData, updated_at_ms: Date.now() };
  const clientVersion = isRecord(modelsPayload)
    ? getString(modelsPayload.client_version) ?? getString(modelsPayload.clientVersion)
    : null;
  const snapshot = normalizeCodexModelsPayload(modelsPayload);
  if (!snapshot) {
    return openaiError(
      400,
      "models must include a non-empty Codex CLI models array",
      "invalid_request_error",
    );
  }
  const snapshotSize = estimateJsonSize(snapshot);
  if (snapshotSize === null) {
    return openaiError(400, "models payload could not be serialized", "invalid_request_error");
  }
  if (snapshotSize > SAFE_KV_BYTES) {
    return openaiError(
      413,
      `models snapshot too large (${snapshotSize} bytes; max ${MAX_KV_BYTES}).`,
      "invalid_request_error",
    );
  }

  let validated: Awaited<ReturnType<typeof validateCodexAuthJson>>;
  try {
    validated = await validateCodexAuthJson(seed, { clientVersion });
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

  const stored = await kv.atomic()
    .set(CODEX_KV_KEY, validated.auth)
    .set(CODEX_MODELS_KV_KEY, snapshot)
    .commit();
  if (!stored.ok) {
    return openaiError(500, "Deno KV could not persist Codex auth and models", "server_error");
  }
  cacheCodexAuth(validated.auth);

  const modelsStored = {
    count: snapshot.models.length,
    source: snapshot.source,
    updated_at_ms: snapshot.updated_at_ms,
    client_version: snapshot.client_version ?? null,
  };

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
      models: modelsStored,
    },
    { "x-ubq-upstream": "chatgpt_codex" },
  );
};

export const handleAdminCodexModelsGet = async (): Promise<Response> => {
  const snapshot = await loadCodexModelsSnapshot();
  if (!snapshot) return json(200, { ok: true, data: null });
  return json(200, { ok: true, data: snapshot });
};

export const handleAdminCodexModelsSet = async (req: Request): Promise<Response> => {
  const raw = await readJsonBody(req);
  if (!raw) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const snapshot = normalizeCodexModelsPayload(raw);
  if (!snapshot) {
    return openaiError(400, "models must include a non-empty models array", "invalid_request_error");
  }
  const size = estimateJsonSize(snapshot);
  if (size === null) {
    return openaiError(400, "models payload could not be serialized", "invalid_request_error");
  }
  if (size > SAFE_KV_BYTES) {
    return openaiError(
      413,
      `models snapshot too large (${size} bytes; max ${MAX_KV_BYTES}).`,
      "invalid_request_error",
    );
  }

  const stored = await storeCodexModelsSnapshot(snapshot);
  if (!stored) {
    return openaiError(500, "Deno KV is not available; cannot persist Codex models", "server_error");
  }

  return json(200, {
    ok: true,
    stored: true,
    count: snapshot.models.length,
    source: snapshot.source,
    updated_at_ms: snapshot.updated_at_ms,
    client_version: snapshot.client_version ?? null,
  });
};

export const handleAdminCodexPromptsPurge = async (): Promise<Response> => {
  const kv = await kvPromise;
  if (!kv) {
    return openaiError(500, "Deno KV is not available; cannot purge Codex prompts", "server_error");
  }

  let deleted = 0;
  const entry = await kv.get(UOS_CODEX_PROMPTS_KEY);
  if (entry.value !== null) {
    await kv.delete(UOS_CODEX_PROMPTS_KEY);
    deleted++;
  }

  for await (const item of kv.list({ prefix: UOS_CODEX_PROMPTS_CHUNK_PREFIX })) {
    await kv.delete(item.key);
    deleted++;
  }

  return json(200, { deleted });
};

const parseBooleanParam = (url: URL, name: string): boolean | null => {
  const value = url.searchParams.get(name);
  if (value === null) return null;
  const normalized = value.trim().toLowerCase();
  if (["1", "true", "yes", "on"].includes(normalized)) return true;
  if (["0", "false", "no", "off"].includes(normalized)) return false;
  return null;
};

const parseMigrationProfile = (url: URL): KvMigrationProfile | null => {
  const profile = url.searchParams.get("profile")?.trim() || "prod";
  if (profile === "local" || profile === "prod") return profile;
  return null;
};

function* splitNdjsonLines(text: string): Iterable<string> {
  for (const line of text.split(/\r?\n/)) {
    if (line.trim()) yield line;
  }
}

export const handleAdminKvMigrationImport = async (req: Request): Promise<Response> => {
  const kv = await kvPromise;
  if (!kv) return openaiError(500, "Deno KV is not available; cannot import migration", "server_error");

  const url = new URL(req.url);
  const profile = parseMigrationProfile(url);
  if (!profile) return openaiError(400, "profile must be local or prod", "invalid_request_error");

  const contentLength = Number(req.headers.get("content-length") ?? 0);
  if (Number.isFinite(contentLength) && contentLength > MAX_KV_MIGRATION_BODY_BYTES) {
    return openaiError(413, "Migration body is too large", "invalid_request_error");
  }

  const body = await req.text();
  if (new TextEncoder().encode(body).length > MAX_KV_MIGRATION_BODY_BYTES) {
    return openaiError(413, "Migration body is too large", "invalid_request_error");
  }

  const includeCache = parseBooleanParam(url, "include_cache") === true;
  const includeLegacy = parseBooleanParam(url, "include_legacy") ?? defaultIncludeLegacyForProfile(profile);
  const overwrite = parseBooleanParam(url, "overwrite") === true;
  const write = parseBooleanParam(url, "write") === true;
  const dryRunParam = parseBooleanParam(url, "dry_run");
  if (write && dryRunParam === true) {
    return openaiError(400, "dry_run and write are mutually exclusive", "invalid_request_error");
  }
  const dryRun = !write;

  const result = await importKvMigrationLines(kv, splitNdjsonLines(body), {
    profile,
    includeCache,
    includeLegacy,
    overwrite,
    dryRun,
  });

  return json(200, {
    profile,
    include_cache: includeCache,
    include_legacy: includeLegacy,
    overwrite,
    dry_run: dryRun,
    ...result,
  });
};

export const handleAdminKvMigrationValidate = async (): Promise<Response> => {
  const kv = await kvPromise;
  if (!kv) return openaiError(500, "Deno KV is not available; cannot validate migration", "server_error");
  return json(200, await validateKvMigrationTarget(kv));
};

export const handleAdminDefaults = async (req: Request): Promise<Response> => {
  const kv = await kvPromise;
  if (!kv) {
    return openaiError(500, "Deno KV is not available; cannot manage defaults", "server_error");
  }

  if (req.method === "GET") {
    const modelEntry = await kv.get<string>(DEFAULT_MODEL_KEY);
    const reasoningEntry = await kv.get<string>(DEFAULT_REASONING_EFFORT_KEY);
    const kernelLimitEntry = await kv.get<number>(DEFAULT_KERNEL_POLICY_LIMIT_KEY);
    const kernelWindowEntry = await kv.get<number>(DEFAULT_KERNEL_POLICY_WINDOW_KEY);
    const model = await resolveDefaultModel(modelEntry.value);
    const reasoningEffort = normalizeReasoningEffort(reasoningEntry.value) ?? DEFAULT_REASONING_EFFORT;
    const kernelPolicyLimit = normalizeKernelUsageLimitInput(kernelLimitEntry.value) ??
      DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS;
    const kernelPolicyWindow = normalizeKernelWindowMsInput(kernelWindowEntry.value) ?? DEFAULT_KERNEL_POLICY_WINDOW_MS;
    return json(200, {
      ok: true,
      defaults: {
        model,
        reasoning_effort: reasoningEffort,
        kernel_policy_limit_requests: kernelPolicyLimit,
        kernel_policy_window_ms: kernelPolicyWindow,
      },
    });
  }

  if (req.method === "POST") {
    const raw = await readJsonBody(req);
    if (!raw || !isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

    const modelEntry = await kv.get<string>(DEFAULT_MODEL_KEY);
    const reasoningEntry = await kv.get<string>(DEFAULT_REASONING_EFFORT_KEY);
    const kernelLimitEntry = await kv.get<number>(DEFAULT_KERNEL_POLICY_LIMIT_KEY);
    const kernelWindowEntry = await kv.get<number>(DEFAULT_KERNEL_POLICY_WINDOW_KEY);

    let model = await resolveDefaultModel(modelEntry.value);
    let reasoningEffort = normalizeReasoningEffort(reasoningEntry.value) ?? DEFAULT_REASONING_EFFORT;
    let kernelPolicyLimit = normalizeKernelUsageLimitInput(kernelLimitEntry.value) ??
      DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS;
    let kernelPolicyWindow = normalizeKernelWindowMsInput(kernelWindowEntry.value) ?? DEFAULT_KERNEL_POLICY_WINDOW_MS;

    const wantsModelUpdate = Object.prototype.hasOwnProperty.call(raw, "model") ||
      Object.prototype.hasOwnProperty.call(raw, "reasoning_effort");
    if (wantsModelUpdate) {
      const nextModel = normalizeDefaultModel(raw.model ?? model);
      if (!nextModel) return openaiError(400, "model must be a non-empty string", "invalid_request_error");

      const snapshot = await loadCodexModelsSnapshot();
      if (!snapshot || !Array.isArray(snapshot.models) || snapshot.models.length === 0) {
        return openaiError(409, "No Codex model snapshot stored", "invalid_request_error");
      }

      const modelRecord = snapshot.models.find((entry) => isRecord(entry) && getString(entry.slug) === nextModel) ??
        null;
      if (!modelRecord) {
        return openaiError(400, "model is not in the stored Codex model list", "invalid_request_error");
      }

      let nextReasoning = normalizeReasoningEffort(raw.reasoning_effort ?? reasoningEffort);
      const modelDefault = normalizeReasoningEffort(modelRecord.default_reasoning_level);
      if (!nextReasoning) {
        nextReasoning = modelDefault ?? DEFAULT_REASONING_EFFORT;
      }

      const levels = extractModelReasoningLevels(modelRecord);
      if (levels.length > 0 && !levels.includes(nextReasoning)) {
        return openaiError(
          400,
          `reasoning_effort must be one of: ${levels.join(", ")}`,
          "invalid_request_error",
        );
      }
      if (levels.length === 0) {
        nextReasoning = "none";
      }

      model = nextModel;
      reasoningEffort = nextReasoning;
      await kv.set(DEFAULT_MODEL_KEY, model);
      await kv.set(DEFAULT_REASONING_EFFORT_KEY, reasoningEffort);
    }

    if (Object.prototype.hasOwnProperty.call(raw, "kernel_policy_limit_requests")) {
      const parsed = normalizeKernelUsageLimitInput(raw.kernel_policy_limit_requests);
      if (parsed === null) {
        return openaiError(
          400,
          "kernel_policy_limit_requests must be a non-negative number or -1 for unlimited",
          "invalid_request_error",
        );
      }
      kernelPolicyLimit = parsed;
      await kv.set(DEFAULT_KERNEL_POLICY_LIMIT_KEY, kernelPolicyLimit);
    }

    if (Object.prototype.hasOwnProperty.call(raw, "kernel_policy_window_ms")) {
      const parsed = normalizeKernelWindowMsInput(raw.kernel_policy_window_ms);
      if (parsed === null) {
        return openaiError(400, "kernel_policy_window_ms must be a positive number", "invalid_request_error");
      }
      kernelPolicyWindow = parsed;
      await kv.set(DEFAULT_KERNEL_POLICY_WINDOW_KEY, kernelPolicyWindow);
    }

    return json(200, {
      ok: true,
      defaults: {
        model,
        reasoning_effort: reasoningEffort,
        kernel_policy_limit_requests: kernelPolicyLimit,
        kernel_policy_window_ms: kernelPolicyWindow,
      },
    });
  }

  return openaiError(405, "Method not allowed", "method_not_allowed");
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

const normalizeApiKeyWindowMsInput = (value: unknown): number | null => {
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

const normalizeKernelExpiresAtMsInput = (value: unknown, nowMs: number): number | null => {
  if (value === undefined || value === null) return null;
  if (typeof value === "string") {
    const trimmed = value.trim();
    if (!trimmed) return null;
    const parsed = Number(trimmed);
    if (!Number.isFinite(parsed)) return null;
    value = parsed;
  }
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  const expiresAtMs = Math.trunc(value);
  if (expiresAtMs === API_KEY_NO_EXPIRATION_MS) return API_KEY_NO_EXPIRATION_MS;
  if (expiresAtMs <= nowMs) return null;
  return expiresAtMs;
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

const normalizeDefaultModel = (value: unknown): string | null => {
  if (typeof value !== "string") return null;
  const model = value.trim();
  if (!model) return null;
  if (/\s/.test(model)) return null;
  return model;
};

const extractModelReasoningLevels = (model: Record<string, unknown> | null): ReasoningEffort[] => {
  if (!model) return [];
  const raw = Array.isArray(model.supported_reasoning_levels) ? model.supported_reasoning_levels : [];
  const levels = raw
    .map((entry) => {
      if (typeof entry === "string") return normalizeReasoningEffort(entry);
      if (isRecord(entry)) return normalizeReasoningEffort(entry.effort);
      return null;
    })
    .filter((entry): entry is ReasoningEffort => Boolean(entry));
  return Array.from(new Set(levels));
};

const normalizeCodexModelsPayload = (value: unknown): CodexModelsSnapshot | null => {
  let modelsRaw: unknown = null;
  let source = "codex_cli";
  let clientVersion: string | null = null;
  let updatedAtMs: number | null = null;

  if (Array.isArray(value)) {
    modelsRaw = value;
  } else if (isRecord(value)) {
    if (Array.isArray(value.models)) modelsRaw = value.models;
    else if (Array.isArray(value.data)) modelsRaw = value.data;
    const sourceValue = getString(value.source);
    if (sourceValue) source = sourceValue;
    clientVersion = getString(value.client_version) ?? getString(value.clientVersion);
    if (typeof value.updated_at_ms === "number" && Number.isFinite(value.updated_at_ms)) {
      updatedAtMs = Math.trunc(value.updated_at_ms);
    }
  }

  if (!modelsRaw || !Array.isArray(modelsRaw)) return null;

  const normalizeModel = (item: Record<string, unknown>): Record<string, unknown> | null => {
    if (isHiddenCodexModel(item)) return null;
    const slug = getString(item.slug) ?? getString(item.id) ?? getString(item.model) ?? getString(item.name);
    if (!slug) return null;
    const normalized: Record<string, unknown> = { slug };
    const displayName = getString(item.display_name) ?? getString(item.displayName) ?? getString(item.name);
    if (displayName) normalized.display_name = displayName;
    const description = getString(item.description);
    if (description) normalized.description = description;
    const defaultReasoning = getString(item.default_reasoning_level);
    if (defaultReasoning) normalized.default_reasoning_level = defaultReasoning;
    if (Array.isArray(item.supported_reasoning_levels)) {
      const levels = item.supported_reasoning_levels
        .map((entry) => {
          if (typeof entry === "string") return entry;
          if (isRecord(entry)) return getString(entry.effort);
          return null;
        })
        .filter((entry): entry is string => typeof entry === "string" && entry.length > 0);
      if (levels.length) normalized.supported_reasoning_levels = levels;
    }
    return normalized;
  };

  const models: Record<string, unknown>[] = [];
  const seen = new Set<string>();
  for (const item of modelsRaw) {
    if (!isRecord(item)) continue;
    const normalized = normalizeModel(item);
    if (!normalized) continue;
    const slug = getString(normalized.slug);
    if (!slug || seen.has(slug)) continue;
    models.push(normalized);
    seen.add(slug);
  }

  if (!models.length) return null;

  return {
    models,
    source,
    updated_at_ms: updatedAtMs ?? Date.now(),
    client_version: clientVersion ?? undefined,
  };
};

const estimateJsonSize = (value: unknown): number | null => {
  try {
    const text = JSON.stringify(value);
    return new TextEncoder().encode(text).length;
  } catch {
    return null;
  }
};

const MAX_KV_BYTES = 65_536;
const SAFE_KV_BYTES = 60_000;

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
  const windowMs = normalizeApiKeyWindowMsInput(raw.window_ms);
  if (raw.window_ms !== undefined && windowMs === null) {
    return openaiError(400, "window_ms must be a positive number", "invalid_request_error");
  }
  const resolvedWindowMs = windowMs ?? USAGE_RESET_PERIOD_MS;

  const hash = await sha256Base64Url(token);
  const hashKey = apiKeyHashKey(hash);
  const hashEntry = await kv.get<ApiKeyHashRecord>(hashKey);
  if (hashEntry.value) {
    return openaiError(409, "API key already exists", "invalid_request_error");
  }

  const id = crypto.randomUUID();
  const usageResetAtMs = calculateNextResetMs(now, resolvedWindowMs);
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
    window_ms: resolvedWindowMs,
  };
  const hashRecord: ApiKeyHashRecord = {
    id,
    expires_at_ms: expiresAtMs,
    revoked_at_ms: null,
    usage_limit_requests: usageLimitRequests,
    usage_requests: 0,
    usage_reset_at_ms: usageResetAtMs,
    window_ms: resolvedWindowMs,
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
      window_ms: record.window_ms,
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
        window_ms: coerceApiKeyWindowMs(r),
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
  const currentWindowMs = coerceApiKeyWindowMs(entry.value);
  let nextName = entry.value.name;
  let nextExpiresAtMs = currentExpiresAtMs;
  let nextUsageLimit = entry.value.usage_limit_requests;
  let nextUsageRequests = entry.value.usage_requests;
  let nextUsageResetAtMs = entry.value.usage_reset_at_ms;
  let nextWindowMs = currentWindowMs;

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

  if (Object.prototype.hasOwnProperty.call(raw, "window_ms")) {
    const windowMs = normalizeApiKeyWindowMsInput(raw.window_ms);
    if (windowMs === null) {
      return openaiError(400, "window_ms must be a positive number", "invalid_request_error");
    }
    nextWindowMs = windowMs;
  }

  const resetUsage = normalizeOptionalBoolean(raw.reset_usage);
  if (resetUsage || nextWindowMs !== currentWindowMs) {
    nextUsageRequests = 0;
    nextUsageResetAtMs = calculateNextResetMs(now, nextWindowMs);
  }

  const hasChanges = nextName !== entry.value.name ||
    nextExpiresAtMs !== currentExpiresAtMs ||
    nextUsageLimit !== entry.value.usage_limit_requests ||
    nextWindowMs !== currentWindowMs ||
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
        window_ms: currentWindowMs,
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
    window_ms: nextWindowMs,
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
    window_ms: updated.window_ms,
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
      window_ms: updated.window_ms,
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
    usage_limit_requests: updated.usage_limit_requests,
    usage_requests: updated.usage_requests,
    usage_reset_at_ms: updated.usage_reset_at_ms,
    window_ms: updated.window_ms,
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

export const handleAdminApiKeysUnrevoke = async (req: Request): Promise<Response> => {
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
    return json(200, { ok: true, id, revoked_at_ms: null }, { "x-ubq-upstream": "chatgpt_codex" });
  }

  const expiresAtMs = coerceApiKeyExpiresAtMs(entry.value);
  const updated: ApiKeyRecord = { ...entry.value, expires_at_ms: expiresAtMs, revoked_at_ms: null };
  const hashKey = apiKeyHashKey(entry.value.hash);
  const hashEntry = await kv.get<ApiKeyHashRecord>(hashKey);
  const updatedHash: ApiKeyHashRecord = {
    id,
    expires_at_ms: updated.expires_at_ms,
    revoked_at_ms: updated.revoked_at_ms,
    usage_limit_requests: updated.usage_limit_requests,
    usage_requests: updated.usage_requests,
    usage_reset_at_ms: updated.usage_reset_at_ms,
    window_ms: updated.window_ms,
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

export const handleAdminKernelPolicyQueueList = async (): Promise<Response> => {
  const records = await listKernelPolicyQueue();
  if (!records) return openaiError(500, "Deno KV is not available", "server_error");
  if (records.length === 0) return json(200, { data: records });

  const kv = await kvPromise;
  if (!kv) return openaiError(500, "Deno KV is not available", "server_error");

  // This queue is meant to surface *current* gaps. Once an org/repo rate limit policy exists,
  // the corresponding queue entries should disappear automatically.
  const orgPolicyOwners = new Set<string>();
  const owners = [...new Set(records.map((record) => record.owner))];
  await Promise.all(
    owners.map(async (owner) => {
      const entry = await kv.get(kernelOrgLimitKey(owner));
      if (entry.value) orgPolicyOwners.add(owner);
    }),
  );

  const repoPolicyPairs = new Set<string>();
  await Promise.all(
    records
      .filter((record) => !orgPolicyOwners.has(record.owner))
      .map(async (record) => {
        const entry = await kv.get(kernelLimitKey(record.owner, record.repo));
        if (entry.value) repoPolicyPairs.add(`${record.owner}/${record.repo}`);
      }),
  );

  const pending = records.filter((record) => {
    if (orgPolicyOwners.has(record.owner)) return false;
    return !repoPolicyPairs.has(`${record.owner}/${record.repo}`);
  });

  return json(200, { data: pending });
};

export const handleAdminKernelUsageGet = async (req: Request): Promise<Response> => {
  const kv = await kvPromise;
  if (!kv) return openaiError(500, "Deno KV is not available", "server_error");

  const url = new URL(req.url);
  const scope = normalizeKernelScope(url.searchParams.get("scope"));
  const listRequested = shouldIncludeUsage(url.searchParams.get("list"));
  const inventoryRequested = shouldIncludeUsage(url.searchParams.get("inventory"));
  const includeUsage = shouldIncludeUsage(url.searchParams.get("include_usage"));
  const dailyDays = 30;
  if (inventoryRequested) {
    if (scope === "org") {
      const records = await listKernelOrgUsageRecords({ includeDaily: true, dailyDays });
      if (!records) {
        return openaiError(500, "Failed to load kernel org usage inventory", "server_error");
      }
      return json(200, { ok: true, scope, usage: records });
    }

    const records = await listKernelUsageRecords({ includeDaily: true, dailyDays });
    if (!records) {
      return openaiError(500, "Failed to load kernel usage inventory", "server_error");
    }
    return json(200, { ok: true, scope, usage: records });
  }
  if (listRequested) {
    if (scope === "org") {
      const limits = await listKernelOrgUsageLimits();
      if (!limits) {
        return openaiError(500, "Failed to load kernel org usage limits", "server_error");
      }
      const usageByOwner = new Map<string, Awaited<ReturnType<typeof getKernelOrgUsage>>>();
      if (includeUsage) {
        await Promise.all(
          limits.map(async (record) => {
            usageByOwner.set(
              record.owner,
              await getKernelOrgUsage(record.owner, { includeDaily: true, dailyDays }),
            );
          }),
        );
      }
      return json(200, {
        ok: true,
        scope,
        limits: limits.map((record) => ({
          ...record,
          ...(includeUsage ? { usage: usageByOwner.get(record.owner) ?? null } : {}),
        })),
      });
    }

    const limits = await listKernelUsageLimits();
    if (!limits) {
      return openaiError(500, "Failed to load kernel usage limits", "server_error");
    }
    const usageByRepo = new Map<string, Awaited<ReturnType<typeof getKernelUsage>>>();
    if (includeUsage) {
      await Promise.all(
        limits.map(async (record) => {
          const key = `${record.owner}/${record.repo}`;
          usageByRepo.set(
            key,
            await getKernelUsage(record.owner, record.repo, { includeDaily: true, dailyDays }),
          );
        }),
      );
    }
    return json(200, {
      ok: true,
      scope,
      limits: limits.map((record) => ({
        ...record,
        ...(includeUsage ? { usage: usageByRepo.get(`${record.owner}/${record.repo}`) ?? null } : {}),
      })),
    });
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
    const usage = await getKernelOrgUsage(owner, { includeDaily: includeUsage, dailyDays });
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
  const usage = await getKernelUsage(owner, repo, { includeDaily: includeUsage, dailyDays });

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
  const nowMs = Date.now();
  const expiresAtMs = normalizeKernelExpiresAtMsInput(raw.expires_at_ms, nowMs);
  if (raw.expires_at_ms !== undefined && expiresAtMs === null) {
    return openaiError(
      400,
      "expires_at_ms must be a Unix epoch ms timestamp in the future, or -1",
      "invalid_request_error",
    );
  }

  if (scope === "org") {
    const updated = await setKernelOrgUsageLimit(owner, usageLimitRequests, {
      windowMs: windowMs ?? undefined,
      expiresAtMs: expiresAtMs ?? undefined,
    });
    if (!updated) {
      return openaiError(409, "Concurrent modification; retry", "invalid_request_error");
    }
    return json(200, { ok: true, scope, org: { owner }, limit: { ...updated, source: "kv" } });
  }

  const updated = await setKernelUsageLimit(owner, repo!, usageLimitRequests, {
    windowMs: windowMs ?? undefined,
    expiresAtMs: expiresAtMs ?? undefined,
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
