import {
  cacheCodexAuthPool,
  CODEX_AUTH_POOL_KV_KEY,
  CODEX_MODELS_KV_KEY,
  CodexError,
  type CodexModelsSnapshot,
  getJwtExpMs,
  loadCodexModelsSnapshot,
  loadFullCodexModelsSnapshot,
  parseCodexAuthFromAuthJson,
  parseCodexAuthPool,
  preserveCodexDefaultModel,
  storeCodexModelsSnapshot,
  upsertCodexAuthAccount,
  validateCodexAuthJson,
} from "./codex.ts";
import { recheckCodexRoutingSlot } from "./codex_account_routing.ts";
import { mergeCodexModelPromptCacheCapabilities, normalizeCodexModelsPayload } from "./codex_models.ts";
import { CODEX_CATALOG_AUTH_GENERATION_KEY, storeCodexCatalog } from "./codex_catalog.ts";
import {
  DEFAULT_KERNEL_POLICY_LIMIT_KEY,
  DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS,
  DEFAULT_KERNEL_POLICY_WINDOW_KEY,
  DEFAULT_KERNEL_POLICY_WINDOW_MS,
  DEFAULT_REASONING_EFFORT,
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
  paidFallbackCreditsToMicrocredits,
  paidFallbackMicrocreditsToCredits,
  USAGE_RESET_PERIOD_MS,
} from "./api_keys.ts";
import {
  API_KEY_USAGE_V2_PREFIX,
  apiKeyPolicyFromHashRecord,
  apiKeyUsageV3RetentionMs,
  apiKeyUsageV3WindowKey,
  deleteApiKeyUsageV3,
  getApiKeyUsageV3,
  hasLiveApiKeyUsageReservationsV3,
  invalidateApiKeyPolicy,
  looksLikeUosApiKey,
  makeApiKeyUsageWindowV3,
  reclaimApiKeyUsageReservationsForKeyV3,
} from "./api_key_policy.ts";
import {
  apiKeyRequestLogPrefix,
  apiKeyUsageDailyKey,
  apiKeyUsageKey,
  legacyApiKeyRequestLogPrefix,
} from "./analytics.ts";
import { reloadKernelPublicKeys } from "./auth.ts";
import {
  defaultPaidFallbackPolicy,
  hasStrictPaidFallbackKeyPolicy,
  initializePaidFallbackPolicy,
  paidFallbackHashFields,
} from "./paid_fallback.ts";
import {
  deletePaidFallbackStateV3,
  getPaidFallbackWindowProjectionV3,
  listPaidFallbackRequestsV3,
  paidFallbackDeletionGuardV3Key,
} from "./paid_fallback_ledger.ts";
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
import { getKv } from "./kv.ts";
import { listCodexResetShadowDecisions } from "./codex_banked_reset.ts";
import {
  assertPromptCacheScopeExperimentTelemetryBaseline,
  PromptCacheScopeExperimentBusyError,
  PromptCacheScopeExperimentFailedError,
  PromptCacheScopeExperimentUnavailableError,
  readPromptCacheScopeExperimentTelemetryBaseline,
  runPromptCacheScopeExperiment,
} from "./prompt_cache_scope_experiment.ts";
import {
  buildRuntimeConfig,
  cacheRuntimeConfig,
  loadRuntimeConfig,
  normalizeRuntimeConfig,
  RUNTIME_CONFIG_V2_KEY,
  RuntimeConfigError,
} from "./runtime_config.ts";
import { readJsonBody } from "./request.ts";
import { getString, isRecord, sha256Base64Url } from "./utils.ts";
import type {
  ApiKeyHashRecord,
  ApiKeyRecord,
  ApiKeyUsageWindowV3,
  CodexAuthPoolState,
  CodexAuthState,
} from "./types.ts";
import { MeteredError } from "./metered.ts";
import { getMeteredQuotaDiagnostics } from "./metered_quota.ts";

const UOS_KERNEL_PUBKEYS_KEY = ["uos_ai", "kernel_pubkeys"];
const UOS_CODEX_PROMPTS_KEY = ["uos_ai", "codex_instructions"] as const;
const UOS_CODEX_PROMPTS_CHUNK_PREFIX = ["uos_ai", "codex_instructions_chunk"] as const;
const MAX_KV_MIGRATION_BODY_BYTES = 5 * 1024 * 1024;

const runtimeConfigErrorResponse = (error: unknown): Response | null => {
  if (!(error instanceof RuntimeConfigError)) return null;
  const status = error.message.includes("too large") || error.message.includes("4 KiB") ? 413 : 409;
  return openaiError(status, error.message, "runtime_config_invalid", { type: "invalid_request_error" });
};

/**
 * Does not issue inference. It merely makes an operator-redeemed quota reset
 * eligible for the next single, coordinated half-open request.
 */
export const handleAdminCodexRecheck = async (slot: number): Promise<Response> => {
  if (!Number.isInteger(slot) || slot < 1 || slot > 2) {
    return openaiError(404, "Codex account slot not found", "not_found");
  }
  const accepted = await recheckCodexRoutingSlot(slot);
  if (!accepted) return openaiError(404, "Codex account slot is not configured", "not_found");
  return new Response(null, { status: 204 });
};

/**
 * Returns only the redacted shadow-decision ledger. It has no mutation or
 * redemption action, so operators can audit a canary without a manual
 * approval endpoint or access to raw account/credit identifiers.
 */
export const handleAdminCodexBankedResetShadowDecisions = async (): Promise<Response> => {
  const decisions = await listCodexResetShadowDecisions();
  if (decisions === null) {
    return openaiError(
      503,
      "Codex banked-reset shadow decisions are unavailable",
      "codex_banked_reset_shadow_unavailable",
      { type: "server_error", headers: { "Cache-Control": "no-store" } },
    );
  }
  return json(200, { decisions }, { "Cache-Control": "no-store" });
};

/**
 * The three-cycle scope probe owns every input. In particular, callers cannot
 * select a model, account, prompt-cache key, or conversation partition.
 */
export const handleAdminCodexCacheScopeExperiment = async (req: Request): Promise<Response> => {
  if ((await req.text()).trim()) {
    return openaiError(400, "Prompt-cache scope experiment does not accept request fields", "invalid_request_error");
  }
  try {
    const telemetryBaseline = await assertPromptCacheScopeExperimentTelemetryBaseline();
    const result = await runPromptCacheScopeExperiment(telemetryBaseline);
    return json(result.status === "in_progress" ? 202 : 200, result);
  } catch (error) {
    if (error instanceof PromptCacheScopeExperimentBusyError) {
      return openaiError(409, error.message, "prompt_cache_scope_experiment_busy");
    }
    if (error instanceof PromptCacheScopeExperimentUnavailableError) {
      return openaiError(503, error.message, "prompt_cache_scope_experiment_unavailable", { type: "server_error" });
    }
    if (error instanceof PromptCacheScopeExperimentFailedError) {
      return openaiError(503, error.message, "prompt_cache_scope_experiment_failed", { type: "server_error" });
    }
    // The experiment reads provider streams and OAuth responses; an unknown
    // thrown value might contain upstream/request material, so never log it.
    console.error("[ai.ubq.fi] Prompt-cache scope experiment could not run.");
    return openaiError(
      503,
      "Prompt-cache scope experiment could not run.",
      "prompt_cache_scope_experiment_failed",
      { type: "server_error" },
    );
  }
};

/**
 * This diagnostic has no caller-controlled target selector. It reads the
 * same server-selected cohort that a future POST would fence again, but does
 * not expose its model/hash or start any paid scope-probe work.
 */
export const handleAdminCodexCacheScopeExperimentTelemetryBaseline = async (
  readBaseline: () => ReturnType<typeof readPromptCacheScopeExperimentTelemetryBaseline> =
    readPromptCacheScopeExperimentTelemetryBaseline,
): Promise<Response> => {
  try {
    const baseline = await readBaseline();
    const { status, reason, release, provider, aggregate, routes } = baseline;
    return json(200, { status, reason, release, provider, aggregate, routes }, { "Cache-Control": "no-store" });
  } catch {
    // Target-selection and KV failures can carry sensitive durable-key
    // material. This route is diagnostic-only, so return no thrown detail.
    console.error("[ai.ubq.fi] Prompt-cache Stage 0 telemetry baseline could not be read.");
    return openaiError(
      503,
      "Prompt-cache Stage 0 telemetry baseline could not be read.",
      "prompt_cache_scope_experiment_unavailable",
      { type: "server_error", headers: { "Cache-Control": "no-store" } },
    );
  }
};

export const handleAdminCodexAuth = async (req: Request): Promise<Response> => {
  const kv = await getKv();
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

  const authenticatedButLimited = !validated.ok && validated.status === 429;
  if (!validated.ok && !authenticatedButLimited) {
    return openaiError(
      401,
      `Invalid Codex auth.json (upstream ${validated.status}): ${validated.body}`,
      "invalid_api_key",
    );
  }

  const validatedAuth = validated.ok ? validated.auth : seed;
  const validatedClientVersion = validated.ok ? validated.clientVersion : clientVersion;
  let snapshot: CodexModelsSnapshot | null = null;
  let runtimeConfig: ReturnType<typeof buildRuntimeConfig> | null = null;
  let authGeneration: string | null = null;
  if (validated.ok) {
    snapshot = normalizeCodexModelsPayload(validated.models, {
      source: "chatgpt_codex",
      clientVersion: validatedClientVersion,
    });
    if (!snapshot) {
      return openaiError(
        502,
        "Codex upstream models response did not include a non-empty model catalog",
        "codex_upstream_unreachable",
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

    authGeneration = crypto.randomUUID();
  }

  let stored = false;
  let storedPool: CodexAuthPoolState | null = null;
  let storedSnapshot: CodexModelsSnapshot | null = null;
  for (let attempt = 0; attempt < 3; attempt += 1) {
    const [existingPoolEntry, existingSnapshot, existingRuntimeEntry] = await Promise.all([
      kv.get<CodexAuthPoolState>(CODEX_AUTH_POOL_KV_KEY),
      kv.get<CodexModelsSnapshot>(CODEX_MODELS_KV_KEY),
      kv.get(RUNTIME_CONFIG_V2_KEY),
    ]);
    const existingPool = parseCodexAuthPool(existingPoolEntry.value);
    const nextPool = upsertCodexAuthAccount(existingPool, validatedAuth);
    if (!nextPool) {
      return openaiError(
        409,
        "Codex auth pool already contains two accounts; upload an auth.json for an existing account to rotate it",
        "codex_auth_pool_full",
      );
    }
    const nextSnapshot = snapshot
      ? mergeCodexModelPromptCacheCapabilities(snapshot, existingSnapshot.value)
      : existingSnapshot.value;
    if (!nextSnapshot) {
      return openaiError(
        409,
        "Cannot store rate-limited Codex auth without an existing model catalog",
        "codex_catalog_required",
      );
    }
    let nextRuntime: ReturnType<typeof buildRuntimeConfig> | null = null;
    if (snapshot && authGeneration) {
      const currentRuntime = normalizeRuntimeConfig(existingRuntimeEntry.value);
      try {
        nextRuntime = buildRuntimeConfig(nextSnapshot, {
          defaultModel: preserveCodexDefaultModel(nextSnapshot, currentRuntime?.default_model),
          defaultReasoningEffort: currentRuntime?.default_reasoning_effort,
        });
      } catch (error) {
        const response = runtimeConfigErrorResponse(error);
        if (response) return response;
        throw error;
      }
    }
    let atomic = kv.atomic()
      .check(existingPoolEntry)
      .check(existingSnapshot)
      .set(CODEX_AUTH_POOL_KV_KEY, nextPool);
    if (snapshot && nextRuntime && authGeneration) {
      atomic = atomic
        .check(existingRuntimeEntry)
        .set(CODEX_CATALOG_AUTH_GENERATION_KEY, authGeneration)
        .set(CODEX_MODELS_KV_KEY, nextSnapshot)
        .set(RUNTIME_CONFIG_V2_KEY, nextRuntime);
    }
    if ((await atomic.commit()).ok) {
      stored = true;
      storedPool = nextPool;
      storedSnapshot = nextSnapshot;
      runtimeConfig = nextRuntime;
      break;
    }
  }
  if (!stored || !storedPool || !storedSnapshot) {
    return openaiError(500, "Deno KV could not persist Codex auth and models", "server_error");
  }
  cacheCodexAuthPool(storedPool);
  if (runtimeConfig) cacheRuntimeConfig(runtimeConfig);

  const catalogSeeded = validated.ok && authGeneration
    ? await storeCodexCatalog(kv, {
      clientVersion: validated.clientVersion,
      authGeneration,
      body: validated.modelsBody,
      etag: validated.etag,
      contentType: validated.contentType,
      fetchedAtMs: Date.now(),
    }).catch((error) => {
      console.error("[ai.ubq.fi] Codex catalog seed failed:", error);
      return false;
    })
    : false;

  const modelsStored = {
    count: storedSnapshot.models.length,
    source: storedSnapshot.source,
    updated_at_ms: storedSnapshot.updated_at_ms,
    client_version: storedSnapshot.client_version ?? null,
  };

  const expMs = getJwtExpMs(validatedAuth.access_token);
  return json(
    200,
    {
      stored: true,
      refreshed: validated.ok ? validated.refreshed : false,
      account_id: validatedAuth.account_id,
      account_count: storedPool.accounts.length,
      account_ids: storedPool.accounts.map((account) => account.account_id),
      access_token_expires_at_ms: expMs,
      updated_at_ms: validatedAuth.updated_at_ms,
      upstream_status: validated.status,
      upstream_content_type: validated.ok ? validated.contentType : null,
      models: modelsStored,
      catalog_seeded: catalogSeeded,
      normalized_snapshot_updated: validated.ok,
    },
    { "x-uos-upstream": "chatgpt_codex" },
  );
};

export const handleAdminCodexModelsGet = async (): Promise<Response> => {
  const snapshot = await loadFullCodexModelsSnapshot();
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

  let stored: boolean;
  try {
    stored = await storeCodexModelsSnapshot(snapshot);
  } catch (error) {
    const response = runtimeConfigErrorResponse(error);
    if (response) return response;
    throw error;
  }
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
  const kv = await getKv();
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
  const kv = await getKv();
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

  const summary = {
    profile,
    include_cache: includeCache,
    include_legacy: includeLegacy,
    overwrite,
    dry_run: dryRun,
    ...result,
  };
  // Import is intentionally allowed to apply valid rows before reporting
  // malformed ones, but callers must receive a non-success status whenever
  // the summary contains any errors.
  return json(result.errors > 0 ? 422 : 200, summary);
};

export const handleAdminKvMigrationValidate = async (): Promise<Response> => {
  const kv = await getKv();
  if (!kv) return openaiError(500, "Deno KV is not available; cannot validate migration", "server_error");
  return json(200, await validateKvMigrationTarget(kv));
};

export const handleAdminDefaults = async (
  req: Request,
  dependencies: Readonly<{
    getMeteredQuotaDiagnostics?: typeof getMeteredQuotaDiagnostics;
  }> = {},
): Promise<Response> => {
  const kv = await getKv();
  if (!kv) {
    return openaiError(500, "Deno KV is not available; cannot manage defaults", "server_error");
  }

  if (req.method === "GET") {
    const [runtime, kernelLimitEntry, kernelWindowEntry, meteredQuota] = await Promise.all([
      loadRuntimeConfig(kv),
      kv.get<number>(DEFAULT_KERNEL_POLICY_LIMIT_KEY),
      kv.get<number>(DEFAULT_KERNEL_POLICY_WINDOW_KEY),
      (dependencies.getMeteredQuotaDiagnostics ?? getMeteredQuotaDiagnostics)(),
    ]);
    const model = runtime?.default_model ?? "";
    const reasoningEffort = runtime?.default_reasoning_effort ?? DEFAULT_REASONING_EFFORT;
    const kernelPolicyLimit = normalizeKernelUsageLimitInput(kernelLimitEntry.value) ??
      DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS;
    const kernelPolicyWindow = normalizeKernelWindowMsInput(kernelWindowEntry.value) ?? DEFAULT_KERNEL_POLICY_WINDOW_MS;
    return json(200, {
      defaults: {
        model,
        reasoning_effort: reasoningEffort,
        kernel_policy_limit_requests: kernelPolicyLimit,
        kernel_policy_window_ms: kernelPolicyWindow,
      },
      metered_quota: meteredQuota,
    });
  }

  if (req.method === "POST") {
    const raw = await readJsonBody(req);
    if (!raw || !isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");
    const allowedFields = new Set([
      "model",
      "reasoning_effort",
      "kernel_policy_limit_requests",
      "kernel_policy_window_ms",
    ]);
    for (const field of Object.keys(raw)) {
      if (!allowedFields.has(field)) {
        return openaiError(400, `Unknown defaults field: ${field}`, "invalid_request_error", { param: field });
      }
    }
    const writesModel = Object.prototype.hasOwnProperty.call(raw, "model");
    const writesReasoning = Object.prototype.hasOwnProperty.call(raw, "reasoning_effort");
    const wantsModelUpdate = writesModel || writesReasoning;
    const writesKernelLimit = Object.prototype.hasOwnProperty.call(raw, "kernel_policy_limit_requests");
    const writesKernelWindow = Object.prototype.hasOwnProperty.call(raw, "kernel_policy_window_ms");
    const requestedKernelLimit = writesKernelLimit
      ? normalizeKernelUsageLimitInput(raw.kernel_policy_limit_requests)
      : undefined;
    if (writesKernelLimit && requestedKernelLimit === null) {
      return openaiError(
        400,
        "kernel_policy_limit_requests must be a non-negative number or -1 for unlimited",
        "invalid_request_error",
      );
    }
    const requestedKernelWindow = writesKernelWindow
      ? normalizeKernelWindowMsInput(raw.kernel_policy_window_ms)
      : undefined;
    if (writesKernelWindow && requestedKernelWindow === null) {
      return openaiError(400, "kernel_policy_window_ms must be a positive number", "invalid_request_error");
    }

    // Everything is parsed and every candidate is built before the one atomic
    // commit. In particular, a late kernel field error or a runtime-size error
    // cannot leave a model/defaults half-update behind.
    for (let attempt = 0; attempt < 3; attempt += 1) {
      const [runtimeEntry, kernelLimitEntry, kernelWindowEntry] = await Promise.all([
        kv.get(RUNTIME_CONFIG_V2_KEY, { consistency: "strong" }),
        kv.get<number>(DEFAULT_KERNEL_POLICY_LIMIT_KEY, { consistency: "strong" }),
        kv.get<number>(DEFAULT_KERNEL_POLICY_WINDOW_KEY, { consistency: "strong" }),
      ]);
      const runtime = normalizeRuntimeConfig(runtimeEntry.value);
      let model = runtime?.default_model ?? "";
      let reasoningEffort = runtime?.default_reasoning_effort ?? DEFAULT_REASONING_EFFORT;
      const kernelPolicyLimit = requestedKernelLimit ??
        normalizeKernelUsageLimitInput(kernelLimitEntry.value) ?? DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS;
      const kernelPolicyWindow = requestedKernelWindow ??
        normalizeKernelWindowMsInput(kernelWindowEntry.value) ?? DEFAULT_KERNEL_POLICY_WINDOW_MS;
      let nextRuntime = null as ReturnType<typeof buildRuntimeConfig> | null;

      if (wantsModelUpdate) {
        if (!runtime) return openaiError(503, "Runtime configuration is unavailable", "server_error");
        const nextModel = writesModel ? normalizeDefaultModel(raw.model) : model;
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

        const wantsReasoningUpdate = writesReasoning;
        const modelDefault = modelRecord.default_reasoning_level === null
          ? "none"
          : normalizeReasoningEffort(modelRecord.default_reasoning_level);
        const levels = extractModelReasoningLevels(modelRecord);
        const nextReasoning = wantsReasoningUpdate
          ? normalizeReasoningEffort(raw.reasoning_effort)
          : modelDefault ?? levels[0] ?? "none";
        if (!nextReasoning) {
          return openaiError(400, "reasoning_effort must be a non-empty string", "invalid_request_error");
        }
        model = nextModel;
        reasoningEffort = nextReasoning;
        try {
          nextRuntime = buildRuntimeConfig(snapshot, {
            defaultModel: model,
            defaultReasoningEffort: reasoningEffort,
          });
        } catch (error) {
          const response = runtimeConfigErrorResponse(error);
          if (response) return response;
          throw error;
        }
      }

      if (!nextRuntime && !writesKernelLimit && !writesKernelWindow) {
        return json(200, {
          defaults: {
            model,
            reasoning_effort: reasoningEffort,
            kernel_policy_limit_requests: kernelPolicyLimit,
            kernel_policy_window_ms: kernelPolicyWindow,
          },
        });
      }

      let atomic = kv.atomic()
        .check(runtimeEntry)
        .check(kernelLimitEntry)
        .check(kernelWindowEntry);
      if (nextRuntime) atomic = atomic.set(RUNTIME_CONFIG_V2_KEY, nextRuntime);
      if (writesKernelLimit) atomic = atomic.set(DEFAULT_KERNEL_POLICY_LIMIT_KEY, kernelPolicyLimit);
      if (writesKernelWindow) atomic = atomic.set(DEFAULT_KERNEL_POLICY_WINDOW_KEY, kernelPolicyWindow);
      const committed = await atomic.commit();
      if (!committed.ok) continue;
      if (nextRuntime) cacheRuntimeConfig(nextRuntime);
      return json(200, {
        defaults: {
          model,
          reasoning_effort: reasoningEffort,
          kernel_policy_limit_requests: kernelPolicyLimit,
          kernel_policy_window_ms: kernelPolicyWindow,
        },
      });
    }
    return openaiError(409, "Defaults were modified concurrently; retry", "invalid_request_error");
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
  return looksLikeUosApiKey(token) ? token : null;
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

const paidFallbackInputError = (message: string): Response => openaiError(400, message, "invalid_request_error");

const paidFallbackInitializationError = (error: unknown): Response => {
  if (error instanceof MeteredError) {
    return openaiError(error.status, error.message, error.code, { type: "server_error" });
  }
  console.error("[ai.ubq.fi] Failed to initialize Metered paid fallback:", error);
  return openaiError(502, "Failed to initialize Metered paid fallback", "metered_pricing_unavailable", {
    type: "server_error",
  });
};

const paidFallbackPublicFields = async (
  record: ApiKeyRecord,
  kv: Deno.Kv,
  windowResetAtMs = record.usage_reset_at_ms,
) => {
  const projection = await getPaidFallbackWindowProjectionV3(
    record.id,
    windowResetAtMs,
    record.paid_fallback_limit_microcredits,
    kv,
  );
  return {
    paid_fallback_enabled: record.paid_fallback_enabled,
    paid_fallback_limit_credits: paidFallbackMicrocreditsToCredits(record.paid_fallback_limit_microcredits),
    paid_fallback_spent_credits: paidFallbackMicrocreditsToCredits(projection?.settled_microcredits ?? 0),
    paid_fallback_reserved_credits: paidFallbackMicrocreditsToCredits(projection?.reserved_microcredits ?? 0),
    paid_fallback_pending_count: projection?.pending_count ?? 0,
    paid_fallback_model_ids: record.paid_fallback_model_ids,
    paid_fallback_pricing_checked_at_ms: record.paid_fallback_pricing_checked_at_ms,
  };
};

const paidFallbackHistoryRecord = (request: Awaited<ReturnType<typeof listPaidFallbackRequestsV3>>[number]) => {
  const startedAtMs = request.dispatched_at_ms ?? request.created_at_ms;
  const completedAtMs = request.terminal_at_ms;
  return {
    ...request,
    id: request.request_id,
    method: "POST",
    status_code: request.terminal_state === "completed" ? 200 : null,
    provider: "metered",
    fallback_reason: "codex_429",
    started_at_ms: startedAtMs,
    completed_at_ms: completedAtMs,
    latency_ms: completedAtMs === null ? null : Math.max(0, completedAtMs - startedAtMs),
    paid_fallback_window_reset_at_ms: request.window_reset_at_ms,
    billing_status: request.billing_state === "settled" ? "reconciled" : request.billing_state,
  };
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
  return value === true;
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
      if (entry === null) return "none";
      if (typeof entry === "string") return normalizeReasoningEffort(entry);
      if (isRecord(entry)) return entry.effort === null ? "none" : normalizeReasoningEffort(entry.effort);
      return null;
    })
    .filter((entry): entry is ReasoningEffort => Boolean(entry));
  return Array.from(new Set(levels));
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
  const kv = await getKv();
  if (!kv) {
    return openaiError(500, "Deno KV is not available; cannot manage API keys", "server_error");
  }

  const raw = await readJsonBody(req);
  if (!raw || !isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const name = normalizeApiKeyName(raw.name);
  if (!name) return openaiError(400, "name must be a non-empty string (<=80 chars)", "invalid_request_error");

  const providedToken = normalizeOptionalApiKeyToken(raw.token);
  if (raw.token !== undefined && raw.token !== null && providedToken === null) {
    return openaiError(
      400,
      "token must use the u_ prefix followed by 64 lowercase hexadecimal characters",
      "invalid_request_error",
    );
  }
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

  if (
    Object.prototype.hasOwnProperty.call(raw, "paid_fallback_enabled") &&
    typeof raw.paid_fallback_enabled !== "boolean"
  ) {
    return paidFallbackInputError("paid_fallback_enabled must be a boolean");
  }
  const paidFallbackEnabled = raw.paid_fallback_enabled === true;
  const paidFallbackLimitMicrocredits = Object.prototype.hasOwnProperty.call(raw, "paid_fallback_limit_credits")
    ? paidFallbackCreditsToMicrocredits(raw.paid_fallback_limit_credits)
    : 0;
  if (paidFallbackLimitMicrocredits === null) {
    return paidFallbackInputError("paid_fallback_limit_credits must be a non-negative number or -1");
  }
  if (paidFallbackEnabled && paidFallbackLimitMicrocredits === 0) {
    return paidFallbackInputError("paid_fallback_limit_credits must be positive or -1 when paid fallback is enabled");
  }

  const hash = await sha256Base64Url(token);
  const hashKey = apiKeyHashKey(hash);
  const hashEntry = await kv.get<ApiKeyHashRecord>(hashKey);
  if (hashEntry.value) {
    return openaiError(409, "API key already exists", "invalid_request_error");
  }

  let paidFallbackPolicy = defaultPaidFallbackPolicy();
  if (paidFallbackEnabled) {
    try {
      paidFallbackPolicy = {
        ...paidFallbackPolicy,
        ...await initializePaidFallbackPolicy(req.signal),
        paid_fallback_enabled: true,
        paid_fallback_limit_microcredits: paidFallbackLimitMicrocredits,
      };
    } catch (error) {
      return paidFallbackInitializationError(error);
    }
  } else {
    paidFallbackPolicy = {
      ...paidFallbackPolicy,
      paid_fallback_limit_microcredits: paidFallbackLimitMicrocredits,
    };
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
    usage_quota_version: 3,
    ...paidFallbackPolicy,
  };
  const hashRecord: ApiKeyHashRecord = {
    id,
    expires_at_ms: expiresAtMs,
    revoked_at_ms: null,
    usage_limit_requests: usageLimitRequests,
    usage_requests: 0,
    usage_reset_at_ms: usageResetAtMs,
    window_ms: resolvedWindowMs,
    usage_quota_version: 3,
    ...paidFallbackHashFields(record),
  };
  const quotaPolicy = apiKeyPolicyFromHashRecord(hash, hashRecord, now);
  if (!quotaPolicy) {
    return openaiError(500, "Failed to build API key quota policy", "server_error");
  }
  const quotaWindow = makeApiKeyUsageWindowV3(quotaPolicy, now);

  const commit = await kv.atomic()
    .check(hashEntry)
    .set(apiKeyIdKey(id), record)
    .set(hashKey, hashRecord)
    .set(apiKeyUsageV3WindowKey(quotaPolicy), quotaWindow, {
      expireIn: apiKeyUsageV3RetentionMs(quotaWindow.window_reset_at_ms, now),
    })
    .commit();
  if (!commit.ok) {
    return openaiError(500, "Failed to persist API key", "server_error");
  }

  return json(
    200,
    {
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
      ...await paidFallbackPublicFields(record, kv),
    },
    { "x-uos-upstream": "chatgpt_codex" },
  );
};

export const handleAdminApiKeysList = async (req: Request): Promise<Response> => {
  const kv = await getKv();
  if (!kv) {
    return openaiError(500, "Deno KV is not available; cannot manage API keys", "server_error");
  }

  const records: ApiKeyRecord[] = [];
  for await (const entry of kv.list<ApiKeyRecord>({ prefix: API_KEY_ID_PREFIX })) {
    if (entry.value) records.push(entry.value);
  }
  records.sort((a, b) => b.created_at_ms - a.created_at_ms);

  const includeUsage = shouldIncludeUsage(new URL(req.url).searchParams.get("include_usage"));
  const usageById = new Map<string, Record<string, number>>();
  const paidFallbackResetById = new Map<string, number>();
  for (const record of records) {
    const hashRecord: ApiKeyHashRecord = {
      id: record.id,
      expires_at_ms: record.expires_at_ms,
      revoked_at_ms: record.revoked_at_ms,
      usage_limit_requests: record.usage_limit_requests,
      usage_requests: record.usage_requests,
      usage_reset_at_ms: record.usage_reset_at_ms,
      window_ms: record.window_ms,
      usage_quota_version: record.usage_quota_version,
      ...paidFallbackHashFields(record),
    };
    const policy = apiKeyPolicyFromHashRecord(record.hash, hashRecord, Date.now());
    if (policy) {
      paidFallbackResetById.set(
        record.id,
        record.revoked_at_ms === null ? policy.usage_reset_at_ms : record.usage_reset_at_ms,
      );
      if (includeUsage) {
        usageById.set(record.id, {
          request_count: await getApiKeyUsageV3(policy, kv),
          limit: policy.usage_limit_requests,
          reset_at_ms: policy.usage_reset_at_ms,
        });
      }
    }
  }
  const paidFallbackById = new Map(
    await Promise.all(
      records.map(async (record) =>
        [
          record.id,
          await paidFallbackPublicFields(
            record,
            kv,
            paidFallbackResetById.get(record.id) ?? record.usage_reset_at_ms,
          ),
        ] as const
      ),
    ),
  );

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
        usage_reset_at_ms: includeUsage ? usageById.get(r.id)?.reset_at_ms ?? r.usage_reset_at_ms : r.usage_reset_at_ms,
        window_ms: coerceApiKeyWindowMs(r),
        ...paidFallbackById.get(r.id),
        ...(includeUsage
          ? {
            usage_requests: usageById.get(r.id)?.request_count ?? 0,
            usage: usageById.get(r.id) ?? null,
          }
          : {}),
      })),
    },
    { "x-uos-upstream": "chatgpt_codex" },
  );
};

export const handleAdminApiKeysPaidFallbacks = async (
  req: Request,
  keyId: string,
  kvOverride?: Deno.Kv | null,
): Promise<Response> => {
  const kv = kvOverride === undefined ? await getKv() : kvOverride;
  if (!kv) {
    return openaiError(500, "Deno KV is not available; cannot load paid fallbacks", "server_error");
  }

  const normalizedKeyId = keyId.trim();
  if (!normalizedKeyId || normalizedKeyId.length > 200) {
    return openaiError(400, "Invalid API key id", "invalid_request_error");
  }

  const keyEntry = await kv.get<ApiKeyRecord>(apiKeyIdKey(normalizedKeyId));
  if (!keyEntry.value) return openaiError(404, "Not found", "not_found");

  const rawLimit = new URL(req.url).searchParams.get("limit");
  if (rawLimit !== null && !/^\d+$/.test(rawLimit.trim())) {
    return openaiError(400, "limit must be a positive integer", "invalid_request_error");
  }
  const requestedLimit = rawLimit === null ? 20 : Number(rawLimit);
  if (!Number.isSafeInteger(requestedLimit) || requestedLimit < 1) {
    return openaiError(400, "limit must be a positive integer", "invalid_request_error");
  }
  const limit = Math.min(requestedLimit, 100);

  try {
    const records = (await listPaidFallbackRequestsV3(normalizedKeyId, limit, kv)).map(paidFallbackHistoryRecord);
    return json(
      200,
      { object: "list", data: records },
      { "Cache-Control": "no-store" },
    );
  } catch (error) {
    console.error("[ai.ubq.fi] Failed to load paid fallback ledger:", error);
    return openaiError(500, "Failed to load paid fallbacks", "server_error");
  }
};

export const handleAdminApiKeysUpdate = async (req: Request): Promise<Response> => {
  const kv = await getKv();
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
  if (!hasStrictPaidFallbackKeyPolicy(entry.value)) {
    return openaiError(503, "API key paid fallback migration is incomplete", "server_error", {
      type: "server_error",
    });
  }

  const now = Date.now();
  const currentExpiresAtMs = coerceApiKeyExpiresAtMs(entry.value);
  const currentWindowMs = coerceApiKeyWindowMs(entry.value);
  let nextName = entry.value.name;
  let nextExpiresAtMs = currentExpiresAtMs;
  let nextUsageLimit = entry.value.usage_limit_requests;
  let nextUsageRequests = entry.value.usage_requests;
  let nextUsageResetAtMs = entry.value.usage_reset_at_ms;
  let nextWindowMs = currentWindowMs;
  let nextPaidFallbackEnabled = entry.value.paid_fallback_enabled;
  let nextPaidFallbackLimitMicrocredits = entry.value.paid_fallback_limit_microcredits;
  let nextPaidFallbackSpentMicrocredits = entry.value.paid_fallback_spent_microcredits;
  const nextPaidFallbackReservedMicrocredits = entry.value.paid_fallback_reserved_microcredits;
  const nextPaidFallbackReservationRequestId = entry.value.paid_fallback_reservation_request_id;
  let nextPaidFallbackModelIds = entry.value.paid_fallback_model_ids;
  let nextPaidFallbackQuotaPerCredit = entry.value.paid_fallback_quota_per_credit;
  let nextPaidFallbackMaxExposureMicrocredits = entry.value.paid_fallback_max_exposure_microcredits ?? {};
  let nextPaidFallbackPricingCheckedAtMs = entry.value.paid_fallback_pricing_checked_at_ms;

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

  if (
    Object.prototype.hasOwnProperty.call(raw, "paid_fallback_enabled") &&
    typeof raw.paid_fallback_enabled !== "boolean"
  ) {
    return paidFallbackInputError("paid_fallback_enabled must be a boolean");
  }
  if (Object.prototype.hasOwnProperty.call(raw, "paid_fallback_enabled")) {
    nextPaidFallbackEnabled = raw.paid_fallback_enabled === true;
  }
  if (Object.prototype.hasOwnProperty.call(raw, "paid_fallback_limit_credits")) {
    const limitMicrocredits = paidFallbackCreditsToMicrocredits(raw.paid_fallback_limit_credits);
    if (limitMicrocredits === null) {
      return paidFallbackInputError("paid_fallback_limit_credits must be a non-negative number or -1");
    }
    nextPaidFallbackLimitMicrocredits = limitMicrocredits;
  }
  if (nextPaidFallbackEnabled && nextPaidFallbackLimitMicrocredits === 0) {
    return paidFallbackInputError("paid_fallback_limit_credits must be positive or -1 when paid fallback is enabled");
  }

  const initializePaidFallback = !entry.value.paid_fallback_enabled && nextPaidFallbackEnabled;
  if (initializePaidFallback) {
    try {
      const initialized = await initializePaidFallbackPolicy(req.signal);
      nextPaidFallbackModelIds = [...initialized.paid_fallback_model_ids];
      nextPaidFallbackQuotaPerCredit = initialized.paid_fallback_quota_per_credit;
      nextPaidFallbackMaxExposureMicrocredits = initialized.paid_fallback_max_exposure_microcredits ?? {};
      nextPaidFallbackPricingCheckedAtMs = initialized.paid_fallback_pricing_checked_at_ms;
    } catch (error) {
      return paidFallbackInitializationError(error);
    }
  }

  if (Object.prototype.hasOwnProperty.call(raw, "reset_usage") && typeof raw.reset_usage !== "boolean") {
    return openaiError(400, "reset_usage must be a boolean", "invalid_request_error");
  }
  const resetUsage = normalizeOptionalBoolean(raw.reset_usage);
  if (resetUsage || nextWindowMs !== currentWindowMs) {
    nextUsageRequests = 0;
    // A reset must always select a distinct V3 aggregate identity. A create
    // followed by an immediate reset can otherwise share the same millisecond
    // start and overwrite the current window instead of opening a fresh one.
    const currentWindowStartMs = entry.value.usage_reset_at_ms - currentWindowMs;
    const freshWindowStartMs = Math.max(now, currentWindowStartMs + 1);
    nextUsageResetAtMs = freshWindowStartMs + nextWindowMs;
    nextPaidFallbackSpentMicrocredits = 0;
  }

  const hasChanges = nextName !== entry.value.name ||
    nextExpiresAtMs !== currentExpiresAtMs ||
    nextUsageLimit !== entry.value.usage_limit_requests ||
    nextWindowMs !== currentWindowMs ||
    nextPaidFallbackEnabled !== entry.value.paid_fallback_enabled ||
    nextPaidFallbackLimitMicrocredits !== entry.value.paid_fallback_limit_microcredits ||
    nextPaidFallbackSpentMicrocredits !== entry.value.paid_fallback_spent_microcredits ||
    nextPaidFallbackReservedMicrocredits !== entry.value.paid_fallback_reserved_microcredits ||
    nextPaidFallbackReservationRequestId !== entry.value.paid_fallback_reservation_request_id ||
    nextPaidFallbackModelIds !== entry.value.paid_fallback_model_ids ||
    nextPaidFallbackQuotaPerCredit !== entry.value.paid_fallback_quota_per_credit ||
    nextPaidFallbackPricingCheckedAtMs !== entry.value.paid_fallback_pricing_checked_at_ms ||
    (resetUsage &&
      (nextUsageRequests !== entry.value.usage_requests || nextUsageResetAtMs !== entry.value.usage_reset_at_ms));

  if (!hasChanges) {
    const currentPolicy = apiKeyPolicyFromHashRecord(entry.value.hash, {
      id: entry.value.id,
      expires_at_ms: entry.value.expires_at_ms,
      revoked_at_ms: entry.value.revoked_at_ms,
      usage_limit_requests: entry.value.usage_limit_requests,
      usage_requests: entry.value.usage_requests,
      usage_reset_at_ms: entry.value.usage_reset_at_ms,
      window_ms: entry.value.window_ms,
      usage_quota_version: entry.value.usage_quota_version,
      ...paidFallbackHashFields(entry.value),
    }, now);
    return json(
      200,
      {
        id: entry.value.id,
        name: entry.value.name,
        prefix: entry.value.prefix,
        created_at_ms: entry.value.created_at_ms,
        expires_at_ms: currentExpiresAtMs,
        revoked_at_ms: entry.value.revoked_at_ms,
        usage_limit_requests: entry.value.usage_limit_requests,
        usage_requests: currentPolicy ? await getApiKeyUsageV3(currentPolicy, kv) : 0,
        usage_reset_at_ms: entry.value.usage_reset_at_ms,
        window_ms: currentWindowMs,
        ...await paidFallbackPublicFields(entry.value, kv),
      },
      { "x-uos-upstream": "chatgpt_codex" },
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
    paid_fallback_enabled: nextPaidFallbackEnabled,
    paid_fallback_limit_microcredits: nextPaidFallbackLimitMicrocredits,
    paid_fallback_spent_microcredits: nextPaidFallbackSpentMicrocredits,
    paid_fallback_reserved_microcredits: nextPaidFallbackReservedMicrocredits,
    paid_fallback_reservation_request_id: nextPaidFallbackReservationRequestId,
    paid_fallback_model_ids: nextPaidFallbackModelIds,
    paid_fallback_quota_per_credit: nextPaidFallbackQuotaPerCredit,
    paid_fallback_max_exposure_microcredits: nextPaidFallbackMaxExposureMicrocredits,
    paid_fallback_pricing_checked_at_ms: nextPaidFallbackPricingCheckedAtMs,
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
    usage_quota_version: updated.usage_quota_version,
    ...paidFallbackHashFields(updated),
  };

  const quotaPolicy = apiKeyPolicyFromHashRecord(updated.hash, updatedHash, now);
  if (!quotaPolicy) {
    return openaiError(503, "API key quota migration is incomplete", "server_error", { type: "server_error" });
  }
  const replaceQuotaWindow = resetUsage || nextWindowMs !== currentWindowMs;
  const currentQuotaPolicy = replaceQuotaWindow ? apiKeyPolicyFromHashRecord(entry.value.hash, entry.value, now) : null;
  if (replaceQuotaWindow && !currentQuotaPolicy) {
    return openaiError(503, "API key quota migration is incomplete", "server_error", { type: "server_error" });
  }
  let currentQuotaWindowEntry: Deno.KvEntryMaybe<ApiKeyUsageWindowV3> | null = null;
  if (replaceQuotaWindow) {
    try {
      await reclaimApiKeyUsageReservationsForKeyV3(kv, updated.id, now);
      // Read the old aggregate after reclaim and before the live scan. A
      // reservation before this read is included in the scan; one after it
      // mutates this checked entry and makes the reset conflict atomically.
      currentQuotaWindowEntry = await kv.get<ApiKeyUsageWindowV3>(
        apiKeyUsageV3WindowKey(currentQuotaPolicy!),
        { consistency: "strong" },
      );
      if (await hasLiveApiKeyUsageReservationsV3(kv, updated.id, now)) {
        return openaiError(
          409,
          "Cannot reset API key quota while requests are reserved; retry after their five-minute lease expires",
          "invalid_request_error",
        );
      }
    } catch (error) {
      console.warn("[ai.ubq.fi] Failed to inspect API key quota reservations before reset:", error);
      return openaiError(503, "API key quota ledger is unavailable", "server_error", { type: "server_error" });
    }
  }

  const quotaWindow = replaceQuotaWindow ? makeApiKeyUsageWindowV3(quotaPolicy, now) : null;
  const quotaWindowEntry = quotaWindow
    ? await kv.get(apiKeyUsageV3WindowKey(quotaPolicy), { consistency: "strong" })
    : null;

  const atomic = kv.atomic()
    .check(entry)
    .check(hashEntry)
    .set(idKey, updated)
    .set(hashKey, updatedHash);
  if (quotaWindow && quotaWindowEntry) {
    atomic.check(quotaWindowEntry).set(apiKeyUsageV3WindowKey(quotaPolicy), quotaWindow, {
      expireIn: apiKeyUsageV3RetentionMs(quotaWindow.window_reset_at_ms, now),
    });
  }
  if (currentQuotaWindowEntry) atomic.check(currentQuotaWindowEntry);

  const commit = await atomic.commit();
  if (!commit.ok) {
    if (replaceQuotaWindow) {
      try {
        await reclaimApiKeyUsageReservationsForKeyV3(kv, updated.id, now);
        if (await hasLiveApiKeyUsageReservationsV3(kv, updated.id, now)) {
          return openaiError(
            409,
            "Cannot reset API key quota while requests are reserved; retry after their five-minute lease expires",
            "invalid_request_error",
          );
        }
      } catch (error) {
        console.warn("[ai.ubq.fi] Failed to recheck API key quota reservations after reset conflict:", error);
        return openaiError(503, "API key quota ledger is unavailable", "server_error", { type: "server_error" });
      }
    }
    return openaiError(409, "API key was modified concurrently; retry", "invalid_request_error");
  }
  invalidateApiKeyPolicy(updated.id);

  return json(
    200,
    {
      id: updated.id,
      name: updated.name,
      prefix: updated.prefix,
      created_at_ms: updated.created_at_ms,
      expires_at_ms: coerceApiKeyExpiresAtMs(updated),
      revoked_at_ms: updated.revoked_at_ms,
      usage_limit_requests: updated.usage_limit_requests,
      usage_requests: await getApiKeyUsageV3(quotaPolicy, kv),
      usage_reset_at_ms: updated.usage_reset_at_ms,
      window_ms: updated.window_ms,
      ...await paidFallbackPublicFields(updated, kv),
    },
    { "x-uos-upstream": "chatgpt_codex" },
  );
};

export const handleAdminApiKeysRevoke = async (req: Request): Promise<Response> => {
  const kv = await getKv();
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
  if (!hasStrictPaidFallbackKeyPolicy(entry.value)) {
    return openaiError(503, "API key paid fallback migration is incomplete", "server_error", {
      type: "server_error",
    });
  }

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
    usage_quota_version: updated.usage_quota_version,
    ...paidFallbackHashFields(updated),
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
  invalidateApiKeyPolicy(updated.id);

  return json(
    200,
    {
      id: updated.id,
      revoked_at_ms: updated.revoked_at_ms,
    },
    { "x-uos-upstream": "chatgpt_codex" },
  );
};

export const handleAdminApiKeysUnrevoke = async (req: Request): Promise<Response> => {
  const kv = await getKv();
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
  if (!hasStrictPaidFallbackKeyPolicy(entry.value)) {
    return openaiError(503, "API key paid fallback migration is incomplete", "server_error", {
      type: "server_error",
    });
  }

  const deletionGuard = await kv.get(paidFallbackDeletionGuardV3Key(id), { consistency: "strong" });
  if (deletionGuard.value) {
    return openaiError(
      409,
      "API key deletion is in progress and cannot be reversed",
      "paid_fallback_deletion_in_progress",
    );
  }
  if (!entry.value.revoked_at_ms) {
    return json(200, { id, revoked_at_ms: null }, { "x-uos-upstream": "chatgpt_codex" });
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
    usage_quota_version: updated.usage_quota_version,
    ...paidFallbackHashFields(updated),
  };

  const atomic = kv.atomic()
    .check(entry)
    .check(deletionGuard)
    .set(idKey, updated)
    .set(hashKey, updatedHash);
  if (hashEntry.versionstamp) atomic.check(hashEntry);

  const commit = await atomic.commit();
  if (!commit.ok) {
    return openaiError(409, "API key was modified concurrently; retry", "invalid_request_error");
  }
  invalidateApiKeyPolicy(updated.id);

  return json(
    200,
    {
      id: updated.id,
      revoked_at_ms: updated.revoked_at_ms,
    },
    { "x-uos-upstream": "chatgpt_codex" },
  );
};

export const handleAdminApiKeysDelete = async (req: Request): Promise<Response> => {
  const kv = await getKv();
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

  const deletionGuardKey = paidFallbackDeletionGuardV3Key(id);
  const deletionGuard = await kv.get(deletionGuardKey, { consistency: "strong" });
  if (!deletionGuard.value) {
    const guardCommit = await kv.atomic()
      .check(entry)
      .check(deletionGuard)
      .set(deletionGuardKey, { created_at_ms: Date.now() })
      .commit();
    if (!guardCommit.ok) {
      return openaiError(409, "API key was modified concurrently; retry", "invalid_request_error");
    }
  }

  let paidFallbackDeletion: Awaited<ReturnType<typeof deletePaidFallbackStateV3>>;
  try {
    paidFallbackDeletion = await deletePaidFallbackStateV3(id, kv);
  } catch (error) {
    console.error("[ai.ubq.fi] Failed to clean V3 paid fallback state before API key deletion:", {
      error,
    });
    return openaiError(500, "Failed to prepare paid fallback state for API key deletion", "server_error");
  }
  if (paidFallbackDeletion.kind === "unavailable") {
    return openaiError(500, "Deno KV is not available; cannot inspect paid fallback billing", "server_error");
  }
  if (paidFallbackDeletion.kind === "blocked") {
    const outstandingPaidFallback = paidFallbackDeletion.outstanding;
    return openaiError(
      409,
      `Cannot delete API key while metered billing is pending or unresolved ` +
        `(pending=${outstandingPaidFallback.pending_requests}, ` +
        `unresolved=${outstandingPaidFallback.unresolved_requests}, ` +
        `markers=${outstandingPaidFallback.pending_markers})`,
      "paid_fallback_billing_outstanding",
    );
  }

  const atomic = kv.atomic()
    .check(entry)
    .delete(idKey)
    .delete(apiKeyHashKey(entry.value.hash))
    .delete(apiKeyUsageKey(id))
    .delete(apiKeyUsageDailyKey(id));

  const commit = await atomic.commit();
  if (!commit.ok) {
    return openaiError(409, "API key was modified concurrently; retry", "invalid_request_error");
  }
  invalidateApiKeyPolicy(id);

  for await (const requestEntry of kv.list({ prefix: apiKeyRequestLogPrefix(id) })) {
    await kv.delete(requestEntry.key);
  }
  for await (const legacyRequestEntry of kv.list({ prefix: legacyApiKeyRequestLogPrefix(id) })) {
    await kv.delete(legacyRequestEntry.key);
  }
  for await (const counterEntry of kv.list({ prefix: [...API_KEY_USAGE_V2_PREFIX, id] })) {
    await kv.delete(counterEntry.key);
  }
  await deleteApiKeyUsageV3(kv, id);

  return json(200, { id }, { "x-uos-upstream": "chatgpt_codex" });
};

const normalizePem = (raw: unknown): string | null => {
  if (typeof raw !== "string") return null;
  const pem = raw.trim();
  if (!pem.startsWith("-----BEGIN PUBLIC KEY-----") || !pem.endsWith("-----END PUBLIC KEY-----")) return null;
  return pem;
};

export const handleAdminKernelPubKeysList = async (): Promise<Response> => {
  const kv = await getKv();
  if (!kv) return openaiError(500, "Deno KV is not available", "server_error");
  const kvEntry = await kv.get<Array<{ app_id: number; pem: string; owner: string; added_at_ms: number }>>(
    UOS_KERNEL_PUBKEYS_KEY,
  );
  return json(200, { data: kvEntry.value ?? [] });
};

export const handleAdminKernelPubKeysCreate = async (req: Request): Promise<Response> => {
  const kv = await getKv();
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
  const kv = await getKv();
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

  const kv = await getKv();
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
  const kv = await getKv();
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
  const kv = await getKv();
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
  if (Object.prototype.hasOwnProperty.call(raw, "reset_usage") && typeof raw.reset_usage !== "boolean") {
    return openaiError(400, "reset_usage must be a boolean", "invalid_request_error");
  }
  const resetUsage = raw.reset_usage === true;

  if (scope === "org") {
    const updated = await setKernelOrgUsageLimit(owner, usageLimitRequests, {
      windowMs: windowMs ?? undefined,
      expiresAtMs: expiresAtMs ?? undefined,
      resetUsage,
    });
    if (!updated) {
      return openaiError(409, "Concurrent modification; retry", "invalid_request_error");
    }
    return json(200, { ok: true, scope, org: { owner }, limit: { ...updated, source: "kv" } });
  }

  const updated = await setKernelUsageLimit(owner, repo!, usageLimitRequests, {
    windowMs: windowMs ?? undefined,
    expiresAtMs: expiresAtMs ?? undefined,
    resetUsage,
  });
  if (!updated) {
    return openaiError(409, "Concurrent modification; retry", "invalid_request_error");
  }

  return json(200, { ok: true, scope, repo: { owner, repo }, limit: { ...updated, source: "kv" } });
};

export const handleAdminKernelUsageDelete = async (req: Request): Promise<Response> => {
  const kv = await getKv();
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
