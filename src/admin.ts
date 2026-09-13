import { config } from "./config.ts";
import { readCodexResetAvailableCount } from "./codex_banked_reset_provider.ts";
import { codexResetUsageKey, readCodexResetUsage } from "./codex_reset_settings.ts";
import {
  cacheCodexAuthPool,
  CODEX_AUTH_POOL_KV_KEY,
  CODEX_MODELS_KV_KEY,
  CodexError,
  type CodexModelsSnapshot,
  getCodexCapacityAccounts,
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
import { apiKeyRequestLogPrefix, apiKeyUsageDailyKey, apiKeyUsageKey, legacyApiKeyRequestLogPrefix } from "./analytics.ts";
import { reloadKernelPublicKeys } from "./auth.ts";
import { defaultPaidFallbackPolicy, hasStrictPaidFallbackKeyPolicy, initializePaidFallbackPolicy, paidFallbackHashFields } from "./paid_fallback.ts";
import {
  backfillPaidFallbackUsageRollups,
  backfillPaidFallbackWindowTtls,
  deletePaidFallbackStateV3,
  getPaidFallbackProviderUsageV3,
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
import { acquireKernelDefaultWindowCutover, type KernelDefaultWindowCutoverGuard, releaseKernelDefaultWindowCutover } from "./kernel_quota_v2.ts";
import { listKernelPolicyQueue } from "./kernel_policy_queue.ts";
import { defaultIncludeLegacyForProfile, importKvMigrationLines, type KvMigrationProfile, validateKvMigrationTarget } from "./kv_migration.ts";
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
  isValidPromptCacheAnalyticsGroupBy,
  type PromptCacheAnalyticsReadOptions,
  type PromptCacheAnalyticsView,
  readPromptCacheAnalytics,
} from "./prompt_cache_analytics.ts";
import {
  buildRuntimeConfig,
  cacheRuntimeConfig,
  loadRuntimeConfig,
  normalizeRuntimeConfig,
  RUNTIME_CONFIG_V2_KEY,
  RuntimeConfigError,
} from "./runtime_config.ts";
import { readJsonBody } from "./request.ts";
import { getString, isRecord, sha256Base64Url, sha256Hex } from "./utils.ts";
import type { ApiKeyHashRecord, ApiKeyRecord, ApiKeyUsageWindowV3, CodexAuthPoolState, CodexAuthState } from "./types.ts";
import { MeteredError } from "./metered.ts";
import {
  getConfiguredMeteredQuotaSnapshot,
  getMeteredQuotaDiagnostics,
  METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS,
  meterQuotaAccountFingerprint,
  normalizeMeteredQuotaBalanceWindowDays,
  readMeteredAccountCredentials,
  readMeteredQuotaBalanceHistory,
  resampleMeteredQuotaBalanceHistory,
} from "./metered_quota.ts";
import { listPaidFallbackUsageRollups, PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS } from "./paid_fallback_rollups.ts";
import { groupPaidFallbackUsageRollups, meteredQuotaRunwayView, projectPaidFallbackRunway, summarizePaidFallbackUsage } from "./quota_projection.ts";
import { DEBUG_ROUTING_MAX_DURATION_MS, type DebugRoutingScenario, loadDebugRoutingConfig, setDebugRoutingConfig } from "./debug_routing.ts";

const UOS_KERNEL_PUBKEYS_KEY = ["uos_ai", "kernel_pubkeys"];
const UOS_CODEX_PROMPTS_KEY = ["uos_ai", "codex_instructions"] as const;
const UOS_CODEX_PROMPTS_CHUNK_PREFIX = ["uos_ai", "codex_instructions_chunk"] as const;
const MAX_KV_MIGRATION_BODY_BYTES = 5 * 1024 * 1024;

const runtimeConfigErrorResponse = (error: unknown): Response | null => {
  if (!(error instanceof RuntimeConfigError)) return null;
  const status = error.message.includes("too large") || error.message.includes("4 KiB") ? 413 : 409;
  return openaiError(status, error.message, "runtime_config_invalid", { type: "invalid_request_error" });
};

export const handleAdminDebugRouting = async (req: Request): Promise<Response> => {
  if (req.method === "GET") {
    return json(200, { routing: await loadDebugRoutingConfig() }, { "Cache-Control": "no-store" });
  }
  if (req.method === "DELETE") {
    return json(200, { routing: await setDebugRoutingConfig("normal", 0) }, { "Cache-Control": "no-store" });
  }
  const raw = await readJsonBody(req);
  if (!raw || !isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");
  const scenario = getString(raw.scenario)?.trim() as DebugRoutingScenario | undefined;
  const durationMs = raw.duration_ms === undefined ? 15 * 60_000 : Number(raw.duration_ms);
  if (!scenario) return openaiError(400, "scenario is required", "invalid_request_error", { param: "scenario" });
  if (!Number.isSafeInteger(durationMs) || durationMs < 0 || durationMs > DEBUG_ROUTING_MAX_DURATION_MS) {
    return openaiError(400, "duration_ms must be between 0 and 3600000", "invalid_request_error", {
      param: "duration_ms",
    });
  }
  try {
    return json(200, { routing: await setDebugRoutingConfig(scenario, durationMs) }, { "Cache-Control": "no-store" });
  } catch (error) {
    return openaiError(400, error instanceof Error ? error.message : "Invalid debug routing scenario", "invalid_request_error");
  }
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

/** Returns only the redacted shadow-decision ledger, without redeeming. */
export const handleAdminCodexBankedResetShadowDecisions = async (): Promise<Response> => {
  const decisions = await listCodexResetShadowDecisions();
  if (decisions === null) {
    return openaiError(503, "Codex banked-reset shadow decisions are unavailable", "codex_banked_reset_shadow_unavailable", {
      type: "server_error",
      headers: { "Cache-Control": "no-store" },
    });
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
    return openaiError(503, "Prompt-cache scope experiment could not run.", "prompt_cache_scope_experiment_failed", { type: "server_error" });
  }
};

/**
 * This diagnostic has no caller-controlled target selector. It reads the
 * same server-selected cohort that a future POST would fence again, but does
 * not expose its model/hash or start any paid scope-probe work.
 */
export const handleAdminCodexCacheScopeExperimentTelemetryBaseline = async (
  readBaseline: () => ReturnType<typeof readPromptCacheScopeExperimentTelemetryBaseline> = readPromptCacheScopeExperimentTelemetryBaseline
): Promise<Response> => {
  try {
    const baseline = await readBaseline();
    const { status, reason, release, provider, aggregate, routes } = baseline;
    return json(200, { status, reason, release, provider, aggregate, routes }, { "Cache-Control": "no-store" });
  } catch {
    // Target-selection and KV failures can carry sensitive durable-key
    // material. This route is diagnostic-only, so return no thrown detail.
    console.error("[ai.ubq.fi] Prompt-cache Stage 0 telemetry baseline could not be read.");
    return openaiError(503, "Prompt-cache Stage 0 telemetry baseline could not be read.", "prompt_cache_scope_experiment_unavailable", {
      type: "server_error",
      headers: { "Cache-Control": "no-store" },
    });
  }
};

/**
 * Read-only, bounded cache analytics. The query deliberately accepts no raw
 * cache key, model, request, account, or general filter value.
 */
export const handleAdminPromptCacheAnalytics = async (
  req: Request,
  readAnalytics: (options: PromptCacheAnalyticsReadOptions) => Promise<PromptCacheAnalyticsView> = readPromptCacheAnalytics
): Promise<Response> => {
  const url = new URL(req.url);
  for (const key of url.searchParams.keys()) {
    if (key !== "group_by") {
      return openaiError(400, "Only group_by is supported for prompt-cache analytics", "invalid_request_error", {
        param: key,
      });
    }
  }
  const groupByParameters = url.searchParams.getAll("group_by");
  if (groupByParameters.length > 1) {
    return openaiError(400, "group_by may appear only once", "invalid_request_error", { param: "group_by" });
  }
  const groupBy = (groupByParameters[0] ?? "key_presence").split(",").map((value) => value.trim());
  if (!isValidPromptCacheAnalyticsGroupBy(groupBy)) {
    return openaiError(
      400,
      "group_by must contain up to two distinct values from provider, model, route, key_presence, mode, or fallback",
      "invalid_request_error",
      { param: "group_by" }
    );
  }
  try {
    return json(200, await readAnalytics({ groupBy }), { "Cache-Control": "no-store" });
  } catch {
    console.error("[ai.ubq.fi] Prompt-cache analytics could not be read.");
    return openaiError(503, "Prompt-cache analytics are unavailable", "prompt_cache_analytics_unavailable", {
      type: "server_error",
      headers: { "Cache-Control": "no-store" },
    });
  }
};

type CodexAuthValidation = Awaited<ReturnType<typeof validateCodexAuthJson>>;
type ValidatedCodexAuth = Extract<CodexAuthValidation, { ok: true }>;

/** Runs upstream validation and maps every failure mode to its original response. */
const validateUploadedCodexAuth = async (
  seed: CodexAuthState,
  clientVersion: string | null
): Promise<{ ok: true; validated: CodexAuthValidation } | { ok: false; response: Response }> => {
  try {
    return { ok: true, validated: await validateCodexAuthJson(seed, { clientVersion }) };
  } catch (error) {
    console.error("[ai.ubq.fi] Codex auth validation failed:", error);
    if (error instanceof CodexError) {
      return { ok: false, response: openaiError(error.status, error.message, error.code) };
    }
    const detail = error instanceof Error ? error.message : String(error);
    const message = detail ? `Upstream validation request failed: ${detail}` : "Upstream validation request failed.";
    return { ok: false, response: openaiError(502, message, "codex_upstream_unreachable") };
  }
};

/**
 * Normalizes the upstream catalog and mints the auth generation that fences the
 * stored snapshot. `crypto.randomUUID()` is called only for a valid snapshot,
 * after the size thresholds have passed.
 */
const prepareCodexAuthSnapshot = (
  validated: ValidatedCodexAuth
): { ok: true; snapshot: CodexModelsSnapshot; authGeneration: string } | { ok: false; response: Response } => {
  const snapshot = normalizeCodexModelsPayload(validated.models, {
    source: "chatgpt_codex",
    clientVersion: validated.clientVersion,
  });
  if (!snapshot) {
    return { ok: false, response: openaiError(502, "Codex upstream models response did not include a non-empty model catalog", "codex_upstream_unreachable") };
  }
  const snapshotSize = estimateJsonSize(snapshot);
  if (snapshotSize === null) {
    return { ok: false, response: openaiError(400, "models payload could not be serialized", "invalid_request_error") };
  }
  if (snapshotSize > SAFE_KV_BYTES) {
    return { ok: false, response: openaiError(413, `models snapshot too large (${snapshotSize} bytes; max ${MAX_KV_BYTES}).`, "invalid_request_error") };
  }
  return { ok: true, snapshot, authGeneration: crypto.randomUUID() };
};

/** Builds the runtime config that accompanies a new model snapshot. */
const buildCodexAuthRuntimeConfig = (
  snapshot: CodexModelsSnapshot,
  existingRuntime: unknown
): { ok: true; runtime: ReturnType<typeof buildRuntimeConfig> } | { ok: false; response: Response } => {
  const currentRuntime = normalizeRuntimeConfig(existingRuntime);
  try {
    return {
      ok: true,
      runtime: buildRuntimeConfig(snapshot, {
        defaultModel: preserveCodexDefaultModel(snapshot, currentRuntime?.default_model),
        defaultReasoningEffort: currentRuntime?.default_reasoning_effort,
      }),
    };
  } catch (error) {
    const response = runtimeConfigErrorResponse(error);
    if (response) return { ok: false, response };
    throw error;
  }
};

/** Commits the account pool, and — only for a full snapshot — the catalog keys. */
const commitCodexAuthState = async (
  kv: Deno.Kv,
  input: Readonly<{
    poolEntry: Deno.KvEntryMaybe<CodexAuthPoolState>;
    snapshotEntry: Deno.KvEntryMaybe<CodexModelsSnapshot>;
    runtimeEntry: Deno.KvEntryMaybe<unknown>;
    pool: CodexAuthPoolState;
    snapshot: CodexModelsSnapshot;
    runtime: ReturnType<typeof buildRuntimeConfig> | null;
    authGeneration: string | null;
    hasSnapshot: boolean;
  }>
): Promise<boolean> => {
  let atomic = kv.atomic().check(input.poolEntry).check(input.snapshotEntry).set(CODEX_AUTH_POOL_KV_KEY, input.pool);
  if (input.hasSnapshot && input.runtime && input.authGeneration) {
    atomic = atomic
      .check(input.runtimeEntry)
      .set(CODEX_CATALOG_AUTH_GENERATION_KEY, input.authGeneration)
      .set(CODEX_MODELS_KV_KEY, input.snapshot)
      .set(RUNTIME_CONFIG_V2_KEY, input.runtime);
  }
  return (await atomic.commit()).ok;
};

type CodexAuthPersistAttempt =
  | { kind: "stored"; pool: CodexAuthPoolState; snapshot: CodexModelsSnapshot; runtimeConfig: ReturnType<typeof buildRuntimeConfig> | null }
  | { kind: "retry" }
  | { kind: "response"; response: Response };

const persistCodexAuthStateAttempt = async (
  kv: Deno.Kv,
  input: Readonly<{ validatedAuth: CodexAuthState; snapshot: CodexModelsSnapshot | null; authGeneration: string | null }>
): Promise<CodexAuthPersistAttempt> => {
  const [existingPoolEntry, existingSnapshot, existingRuntimeEntry] = await Promise.all([
    kv.get<CodexAuthPoolState>(CODEX_AUTH_POOL_KV_KEY),
    kv.get<CodexModelsSnapshot>(CODEX_MODELS_KV_KEY),
    kv.get(RUNTIME_CONFIG_V2_KEY),
  ]);
  const existingPool = parseCodexAuthPool(existingPoolEntry.value);
  const nextPool = upsertCodexAuthAccount(existingPool, input.validatedAuth);
  if (!nextPool) {
    return {
      kind: "response",
      response: openaiError(
        409,
        "Codex auth pool already contains two accounts; upload an auth.json for an existing account to rotate it",
        "codex_auth_pool_full"
      ),
    };
  }
  const nextSnapshot = input.snapshot ? mergeCodexModelPromptCacheCapabilities(input.snapshot, existingSnapshot.value) : existingSnapshot.value;
  if (!nextSnapshot) {
    return { kind: "response", response: openaiError(409, "Cannot store rate-limited Codex auth without an existing model catalog", "codex_catalog_required") };
  }
  const built = input.snapshot && input.authGeneration ? buildCodexAuthRuntimeConfig(nextSnapshot, existingRuntimeEntry.value) : null;
  if (built && !built.ok) return { kind: "response", response: built.response };
  const stored = await commitCodexAuthState(kv, {
    poolEntry: existingPoolEntry,
    snapshotEntry: existingSnapshot,
    runtimeEntry: existingRuntimeEntry,
    pool: nextPool,
    snapshot: nextSnapshot,
    runtime: built?.ok ? built.runtime : null,
    authGeneration: input.authGeneration,
    hasSnapshot: input.snapshot !== null,
  });
  if (!stored) return { kind: "retry" };
  return { kind: "stored", pool: nextPool, snapshot: nextSnapshot, runtimeConfig: built?.ok ? built.runtime : null };
};

const persistCodexAuthState = async (
  kv: Deno.Kv,
  input: Readonly<{ validatedAuth: CodexAuthState; snapshot: CodexModelsSnapshot | null; authGeneration: string | null }>
): Promise<
  | { ok: true; pool: CodexAuthPoolState; snapshot: CodexModelsSnapshot; runtimeConfig: ReturnType<typeof buildRuntimeConfig> | null }
  | { ok: false; response: Response }
> => {
  for (let attempt = 0; attempt < 3; attempt += 1) {
    const attemptResult = await persistCodexAuthStateAttempt(kv, input);
    if (attemptResult.kind === "retry") continue;
    if (attemptResult.kind === "response") return { ok: false, response: attemptResult.response };
    return { ok: true, pool: attemptResult.pool, snapshot: attemptResult.snapshot, runtimeConfig: attemptResult.runtimeConfig };
  }
  return { ok: false, response: openaiError(500, "Deno KV could not persist Codex auth and models", "server_error") };
};

/**
 * Seeds the versioned upstream catalog. A seed failure is reported as `false`
 * and never fails the upload itself.
 */
const seedCodexAuthCatalog = async (kv: Deno.Kv, validated: CodexAuthValidation, authGeneration: string | null): Promise<boolean> => {
  if (!validated.ok || !authGeneration) return false;
  return await storeCodexCatalog(kv, {
    clientVersion: validated.clientVersion,
    authGeneration,
    body: validated.modelsBody,
    etag: validated.etag,
    contentType: validated.contentType,
    fetchedAtMs: Date.now(),
  }).catch((error: unknown) => {
    console.error("[ai.ubq.fi] Codex catalog seed failed:", error);
    return false;
  });
};

type CodexAuthUpload = Readonly<{ seed: CodexAuthState; clientVersion: string | null }>;

/** Accepts either the wrapped `{ auth, models }` upload or a bare auth.json. */
const parseCodexAuthUpload = (body: unknown): CodexAuthUpload | null => {
  const authPayload = isRecord(body) && "auth" in body ? (body.auth as unknown) : body;
  const modelsPayload = isRecord(body) && "models" in body ? (body.models as unknown) : undefined;
  const tokenData = parseCodexAuthFromAuthJson(authPayload);
  if (!tokenData) return null;
  const seed: CodexAuthState = { ...tokenData, updated_at_ms: Date.now() };
  const clientVersion = isRecord(modelsPayload) ? (getString(modelsPayload.client_version) ?? getString(modelsPayload.clientVersion)) : null;
  return { seed, clientVersion };
};

/** Maps an upstream validation result to its original 401, or `null` when acceptable. */
const codexAuthUpstreamErrorResponse = (validated: CodexAuthValidation): Response | null => {
  const authenticatedButLimited = !validated.ok && validated.status === 429;
  if (validated.ok || authenticatedButLimited) return null;
  return openaiError(401, `Invalid Codex auth.json (upstream ${validated.status}): ${validated.body}`, "invalid_api_key");
};

export const handleAdminCodexAuth = async (req: Request): Promise<Response> => {
  const kv = await getKv();
  if (!kv) {
    return openaiError(500, "Deno KV is not available; cannot persist Codex auth", "server_error");
  }

  const upload = parseCodexAuthUpload(await readJsonBody(req));
  if (!upload) {
    return openaiError(400, "Body does not look like a Codex auth.json", "invalid_request_error");
  }

  const validation = await validateUploadedCodexAuth(upload.seed, upload.clientVersion);
  if (!validation.ok) return validation.response;
  const validated = validation.validated;

  const upstreamError = codexAuthUpstreamErrorResponse(validated);
  if (upstreamError) return upstreamError;

  const validatedAuth = validated.ok ? validated.auth : upload.seed;
  let snapshot: CodexModelsSnapshot | null = null;
  let authGeneration: string | null = null;
  if (validated.ok) {
    const prepared = prepareCodexAuthSnapshot(validated);
    if (!prepared.ok) return prepared.response;
    snapshot = prepared.snapshot;
    authGeneration = prepared.authGeneration;
  }

  const persisted = await persistCodexAuthState(kv, { validatedAuth, snapshot, authGeneration });
  if (!persisted.ok) return persisted.response;
  cacheCodexAuthPool(persisted.pool);
  if (persisted.runtimeConfig) cacheRuntimeConfig(persisted.runtimeConfig);

  const catalogSeeded = await seedCodexAuthCatalog(kv, validated, authGeneration);

  const modelsStored = {
    count: persisted.snapshot.models.length,
    source: persisted.snapshot.source,
    updated_at_ms: persisted.snapshot.updated_at_ms,
    client_version: persisted.snapshot.client_version ?? null,
  };

  const expMs = getJwtExpMs(validatedAuth.access_token);
  return json(
    200,
    {
      stored: true,
      refreshed: validated.ok ? validated.refreshed : false,
      account_id: validatedAuth.account_id,
      account_count: persisted.pool.accounts.length,
      account_ids: persisted.pool.accounts.map((account) => account.account_id),
      access_token_expires_at_ms: expMs,
      updated_at_ms: validatedAuth.updated_at_ms,
      upstream_status: validated.status,
      upstream_content_type: validated.ok ? validated.contentType : null,
      models: modelsStored,
      catalog_seeded: catalogSeeded,
      normalized_snapshot_updated: validated.ok,
    },
    { "x-uos-upstream": "chatgpt_codex" }
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
    return openaiError(413, `models snapshot too large (${size} bytes; max ${MAX_KV_BYTES}).`, "invalid_request_error");
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
  // A missing, blank, or whitespace-only profile must still select "prod", so
  // this is an explicit comparison instead of a `||` default.
  const profile = url.searchParams.get("profile")?.trim();
  if (profile === "local") return "local";
  if (profile === undefined || profile === "" || profile === "prod") return "prod";
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

type AdminDefaultsDependencies = Readonly<{
  getMeteredQuotaDiagnostics?: typeof getMeteredQuotaDiagnostics;
}>;

type AdminDefaultsIntent = Readonly<{
  writesModel: boolean;
  writesReasoning: boolean;
  wantsModelUpdate: boolean;
  writesKernelLimit: boolean;
  writesKernelWindow: boolean;
  requestedKernelLimit: number | null | undefined;
  requestedKernelWindow: number | null | undefined;
}>;

const defaultsBody = (model: string, reasoningEffort: ReasoningEffort, kernelPolicyLimit: number, kernelPolicyWindow: number) => ({
  defaults: {
    model,
    reasoning_effort: reasoningEffort,
    kernel_policy_limit_requests: kernelPolicyLimit,
    kernel_policy_window_ms: kernelPolicyWindow,
  },
});

/** Single shared response builder so every defaults JSON response stays byte-identical. */
const defaultsJson = (model: string, reasoningEffort: ReasoningEffort, kernelPolicyLimit: number, kernelPolicyWindow: number): Response =>
  json(200, defaultsBody(model, reasoningEffort, kernelPolicyLimit, kernelPolicyWindow));

const adminDefaultsGetResponse = async (kv: Deno.Kv, dependencies: AdminDefaultsDependencies): Promise<Response> => {
  const [runtime, kernelLimitEntry, kernelWindowEntry, meteredQuota] = await Promise.all([
    loadRuntimeConfig(kv),
    kv.get<number>(DEFAULT_KERNEL_POLICY_LIMIT_KEY),
    kv.get<number>(DEFAULT_KERNEL_POLICY_WINDOW_KEY),
    (dependencies.getMeteredQuotaDiagnostics ?? getMeteredQuotaDiagnostics)(),
  ]);
  const model = runtime?.default_model ?? "";
  const reasoningEffort = runtime?.default_reasoning_effort ?? DEFAULT_REASONING_EFFORT;
  const kernelPolicyLimit = normalizeKernelUsageLimitInput(kernelLimitEntry.value) ?? DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS;
  const kernelPolicyWindow = normalizeKernelWindowMsInput(kernelWindowEntry.value) ?? DEFAULT_KERNEL_POLICY_WINDOW_MS;
  return json(200, { ...defaultsBody(model, reasoningEffort, kernelPolicyLimit, kernelPolicyWindow), metered_quota: meteredQuota });
};

/** Resolves the model/reasoning pair and its runtime config, or the original error response. */
const resolveDefaultsModelUpdate = async (
  raw: Record<string, unknown>,
  intent: AdminDefaultsIntent,
  runtime: ReturnType<typeof normalizeRuntimeConfig>,
  currentModel: string
): Promise<
  { ok: true; model: string; reasoningEffort: ReasoningEffort; nextRuntime: ReturnType<typeof buildRuntimeConfig> | null } | { ok: false; response: Response }
> => {
  if (!runtime) return { ok: false, response: openaiError(503, "Runtime configuration is unavailable", "server_error") };
  const nextModel = intent.writesModel ? normalizeDefaultModel(raw.model) : currentModel;
  if (!nextModel) return { ok: false, response: openaiError(400, "model must be a non-empty string", "invalid_request_error") };

  const snapshot = await loadCodexModelsSnapshot();
  if (!snapshot || !Array.isArray(snapshot.models) || snapshot.models.length === 0) {
    return { ok: false, response: openaiError(409, "No Codex model snapshot stored", "invalid_request_error") };
  }
  const modelRecord = snapshot.models.find((entry) => isRecord(entry) && getString(entry.slug) === nextModel) ?? null;
  if (!modelRecord) {
    return { ok: false, response: openaiError(400, "model is not in the stored Codex model list", "invalid_request_error") };
  }

  const modelDefault = modelRecord.default_reasoning_level === null ? "none" : normalizeReasoningEffort(modelRecord.default_reasoning_level);
  const levels = extractModelReasoningLevels(modelRecord);
  const nextReasoning = intent.writesReasoning ? normalizeReasoningEffort(raw.reasoning_effort) : (modelDefault ?? levels.at(0) ?? "none");
  if (!nextReasoning) {
    return { ok: false, response: openaiError(400, "reasoning_effort must be a non-empty string", "invalid_request_error") };
  }

  try {
    const nextRuntime = buildRuntimeConfig(snapshot, { defaultModel: nextModel, defaultReasoningEffort: nextReasoning });
    return { ok: true, model: nextModel, reasoningEffort: nextReasoning, nextRuntime };
  } catch (error) {
    const response = runtimeConfigErrorResponse(error);
    if (response) return { ok: false, response };
    throw error;
  }
};

type AdminDefaultsCutover =
  { kind: "none" } | { kind: "guard"; guard: KernelDefaultWindowCutoverGuard } | { kind: "retry" } | { kind: "response"; response: Response };

/** Acquires the default-window cutover guard when (and only when) it is required. */
const resolveDefaultsCutover = async (
  kv: Deno.Kv,
  kernelLimitEntry: Deno.KvEntryMaybe<number>,
  kernelWindowEntry: Deno.KvEntryMaybe<number>,
  intent: AdminDefaultsIntent,
  kernelPolicyWindow: number
): Promise<AdminDefaultsCutover> => {
  const currentKernelWindow = normalizeKernelWindowMsInput(kernelWindowEntry.value) ?? DEFAULT_KERNEL_POLICY_WINDOW_MS;
  if (!intent.writesKernelWindow || kernelPolicyWindow === currentKernelWindow) return { kind: "none" };
  const cutover = await acquireKernelDefaultWindowCutover(kv, kernelLimitEntry, kernelWindowEntry);
  if (cutover.ok) return { kind: "guard", guard: cutover.guard };
  if (cutover.reason === "active_reservations") {
    return {
      kind: "response",
      response: openaiError(409, "Active Kernel quota reservations must settle before changing the default window", "invalid_request_error"),
    };
  }
  if (cutover.reason === "concurrent_change") return { kind: "retry" };
  return { kind: "response", response: openaiError(503, "Kernel quota ledger is unavailable", "server_error") };
};

const commitDefaultsUpdate = async (
  kv: Deno.Kv,
  input: Readonly<{
    runtimeEntry: Deno.KvEntryMaybe<unknown>;
    kernelLimitEntry: Deno.KvEntryMaybe<number>;
    kernelWindowEntry: Deno.KvEntryMaybe<number>;
    cutoverGuard: KernelDefaultWindowCutoverGuard | null;
    nextRuntime: ReturnType<typeof buildRuntimeConfig> | null;
    writesKernelLimit: boolean;
    kernelPolicyLimit: number;
    writesKernelWindow: boolean;
    kernelPolicyWindow: number;
  }>
): Promise<{ ok: boolean }> => {
  let atomic = kv.atomic().check(input.runtimeEntry).check(input.kernelLimitEntry).check(input.kernelWindowEntry);
  if (input.cutoverGuard) {
    atomic = atomic.check(input.cutoverGuard.entry).delete(input.cutoverGuard.key);
  }
  if (input.nextRuntime) atomic = atomic.set(RUNTIME_CONFIG_V2_KEY, input.nextRuntime);
  if (input.writesKernelLimit) atomic = atomic.set(DEFAULT_KERNEL_POLICY_LIMIT_KEY, input.kernelPolicyLimit);
  if (input.writesKernelWindow) atomic = atomic.set(DEFAULT_KERNEL_POLICY_WINDOW_KEY, input.kernelPolicyWindow);
  try {
    if ((await atomic.commit()).ok) return { ok: true };
  } catch (error) {
    if (input.cutoverGuard) await releaseKernelDefaultWindowCutover(kv, input.cutoverGuard);
    throw error;
  }
  if (input.cutoverGuard) await releaseKernelDefaultWindowCutover(kv, input.cutoverGuard);
  return { ok: false };
};

/** One read-modify-write attempt; the caller owns the three-attempt retry bound. */
const applyDefaultsUpdateAttempt = async (
  kv: Deno.Kv,
  raw: Record<string, unknown>,
  intent: AdminDefaultsIntent
): Promise<{ kind: "retry" } | { kind: "response"; response: Response }> => {
  const [runtimeEntry, kernelLimitEntry, kernelWindowEntry] = await Promise.all([
    kv.get(RUNTIME_CONFIG_V2_KEY, { consistency: "strong" }),
    kv.get<number>(DEFAULT_KERNEL_POLICY_LIMIT_KEY, { consistency: "strong" }),
    kv.get<number>(DEFAULT_KERNEL_POLICY_WINDOW_KEY, { consistency: "strong" }),
  ]);
  const runtime = normalizeRuntimeConfig(runtimeEntry.value);
  let model = runtime?.default_model ?? "";
  let reasoningEffort = runtime?.default_reasoning_effort ?? DEFAULT_REASONING_EFFORT;
  const kernelPolicyLimit = intent.requestedKernelLimit ?? normalizeKernelUsageLimitInput(kernelLimitEntry.value) ?? DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS;
  const kernelPolicyWindow = intent.requestedKernelWindow ?? normalizeKernelWindowMsInput(kernelWindowEntry.value) ?? DEFAULT_KERNEL_POLICY_WINDOW_MS;
  let nextRuntime = null as ReturnType<typeof buildRuntimeConfig> | null;

  if (intent.wantsModelUpdate) {
    const resolved = await resolveDefaultsModelUpdate(raw, intent, runtime, model);
    if (!resolved.ok) return { kind: "response", response: resolved.response };
    model = resolved.model;
    reasoningEffort = resolved.reasoningEffort;
    nextRuntime = resolved.nextRuntime;
  }

  if (!nextRuntime && !intent.writesKernelLimit && !intent.writesKernelWindow) {
    return { kind: "response", response: defaultsJson(model, reasoningEffort, kernelPolicyLimit, kernelPolicyWindow) };
  }

  const cutover = await resolveDefaultsCutover(kv, kernelLimitEntry, kernelWindowEntry, intent, kernelPolicyWindow);
  if (cutover.kind === "retry") return { kind: "retry" };
  if (cutover.kind === "response") return { kind: "response", response: cutover.response };

  const committed = await commitDefaultsUpdate(kv, {
    runtimeEntry,
    kernelLimitEntry,
    kernelWindowEntry,
    cutoverGuard: cutover.kind === "guard" ? cutover.guard : null,
    nextRuntime,
    writesKernelLimit: intent.writesKernelLimit,
    kernelPolicyLimit,
    writesKernelWindow: intent.writesKernelWindow,
    kernelPolicyWindow,
  });
  if (!committed.ok) return { kind: "retry" };
  if (nextRuntime) cacheRuntimeConfig(nextRuntime);
  return { kind: "response", response: defaultsJson(model, reasoningEffort, kernelPolicyLimit, kernelPolicyWindow) };
};

/**
 * Everything is parsed and every candidate is built before the one atomic
 * commit. In particular, a late kernel field error or a runtime-size error
 * cannot leave a model/defaults half-update behind, so a lost race simply
 * re-reads and rebuilds.
 */
const applyDefaultsUpdate = async (kv: Deno.Kv, raw: Record<string, unknown>, intent: AdminDefaultsIntent): Promise<Response> => {
  for (let attempt = 0; attempt < 3; attempt += 1) {
    const attemptResult = await applyDefaultsUpdateAttempt(kv, raw, intent);
    if (attemptResult.kind === "retry") continue;
    return attemptResult.response;
  }
  return openaiError(409, "Defaults were modified concurrently; retry", "invalid_request_error");
};

const adminDefaultsPostResponse = async (kv: Deno.Kv, req: Request): Promise<Response> => {
  const raw = await readJsonBody(req);
  if (!raw || !isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");
  const allowedFields = new Set(["model", "reasoning_effort", "kernel_policy_limit_requests", "kernel_policy_window_ms"]);
  for (const field of Object.keys(raw)) {
    if (!allowedFields.has(field)) {
      return openaiError(400, `Unknown defaults field: ${field}`, "invalid_request_error", { param: field });
    }
  }
  const writesModel = Object.prototype.hasOwnProperty.call(raw, "model");
  const writesReasoning = Object.prototype.hasOwnProperty.call(raw, "reasoning_effort");
  const writesKernelLimit = Object.prototype.hasOwnProperty.call(raw, "kernel_policy_limit_requests");
  const writesKernelWindow = Object.prototype.hasOwnProperty.call(raw, "kernel_policy_window_ms");
  const requestedKernelLimit = writesKernelLimit ? normalizeKernelUsageLimitInput(raw.kernel_policy_limit_requests) : undefined;
  if (writesKernelLimit && requestedKernelLimit === null) {
    return openaiError(400, "kernel_policy_limit_requests must be a non-negative number or -1 for unlimited", "invalid_request_error");
  }
  const requestedKernelWindow = writesKernelWindow ? normalizeKernelWindowMsInput(raw.kernel_policy_window_ms) : undefined;
  if (writesKernelWindow && requestedKernelWindow === null) {
    return openaiError(400, "kernel_policy_window_ms must be a positive number", "invalid_request_error");
  }
  return await applyDefaultsUpdate(kv, raw, {
    writesModel,
    writesReasoning,
    wantsModelUpdate: writesModel || writesReasoning,
    writesKernelLimit,
    writesKernelWindow,
    requestedKernelLimit,
    requestedKernelWindow,
  });
};

export const handleAdminDefaults = async (
  req: Request,
  dependencies: Readonly<{
    getMeteredQuotaDiagnostics?: typeof getMeteredQuotaDiagnostics;
  }> = {}
): Promise<Response> => {
  const kv = await getKv();
  if (!kv) {
    return openaiError(500, "Deno KV is not available; cannot manage defaults", "server_error");
  }

  if (req.method === "GET") return await adminDefaultsGetResponse(kv, dependencies);
  if (req.method === "POST") return await adminDefaultsPostResponse(kv, req);

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

const normalizeWindowMsInput = (value: unknown): number | null => {
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

/** API-key quota windows and Kernel policy windows share this positive-number validation. */
const normalizeApiKeyWindowMsInput = normalizeWindowMsInput;
const normalizeKernelWindowMsInput = normalizeWindowMsInput;

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

const paidFallbackPublicFields = async (record: ApiKeyRecord, kv: Deno.Kv, windowResetAtMs = record.usage_reset_at_ms) => {
  const [projection, providerUsage] = await Promise.all([
    getPaidFallbackWindowProjectionV3(record.id, windowResetAtMs, record.paid_fallback_limit_microcredits, kv),
    getPaidFallbackProviderUsageV3(record.id, windowResetAtMs, kv),
  ]);
  return {
    paid_fallback_enabled: record.paid_fallback_enabled,
    paid_fallback_limit_credits: paidFallbackMicrocreditsToCredits(record.paid_fallback_limit_microcredits),
    paid_fallback_spent_credits: paidFallbackMicrocreditsToCredits(projection?.settled_microcredits ?? 0),
    paid_fallback_reserved_credits: paidFallbackMicrocreditsToCredits(projection?.reserved_microcredits ?? 0),
    paid_fallback_pending_count: projection?.pending_count ?? 0,
    paid_fallback_provider_usage: providerUsage,
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
    provider: request.provider ?? "metered",
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
    .map((entry): ReasoningEffort | null => {
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

type ApiKeyCreateFields = Readonly<{
  expiresAtMs: number;
  usageLimitRequests: number;
  windowMs: number;
  paidFallbackEnabled: boolean;
  paidFallbackLimitMicrocredits: number;
}>;

/** Resolves the caller-supplied token, or mints one when it is absent. */
const resolveApiKeyCreateToken = (raw: Record<string, unknown>): { ok: true; token: string } | { ok: false; response: Response } => {
  const providedToken = normalizeOptionalApiKeyToken(raw.token);
  if (raw.token !== undefined && raw.token !== null && providedToken === null) {
    return { ok: false, response: openaiError(400, "token must use the u_ prefix followed by 64 lowercase hexadecimal characters", "invalid_request_error") };
  }
  return { ok: true, token: providedToken ?? generateApiKeyToken() };
};

/** `paid_fallback_enabled` and `paid_fallback_limit_credits`, validated in that order. */
const resolveApiKeyCreatePaidFallback = (
  raw: Record<string, unknown>
): { ok: true; paidFallbackEnabled: boolean; paidFallbackLimitMicrocredits: number } | { ok: false; response: Response } => {
  if (Object.prototype.hasOwnProperty.call(raw, "paid_fallback_enabled") && typeof raw.paid_fallback_enabled !== "boolean") {
    return { ok: false, response: paidFallbackInputError("paid_fallback_enabled must be a boolean") };
  }
  const paidFallbackEnabled = raw.paid_fallback_enabled === true;
  const paidFallbackLimitMicrocredits = Object.prototype.hasOwnProperty.call(raw, "paid_fallback_limit_credits")
    ? paidFallbackCreditsToMicrocredits(raw.paid_fallback_limit_credits)
    : 0;
  if (paidFallbackLimitMicrocredits === null) {
    return { ok: false, response: paidFallbackInputError("paid_fallback_limit_credits must be a non-negative number or -1") };
  }
  if (paidFallbackEnabled && paidFallbackLimitMicrocredits === 0) {
    return { ok: false, response: paidFallbackInputError("paid_fallback_limit_credits must be positive or -1 when paid fallback is enabled") };
  }
  return { ok: true, paidFallbackEnabled, paidFallbackLimitMicrocredits };
};

/** Every create-time field except `name` and `token`, validated in the original order. */
const resolveApiKeyCreateFields = (
  raw: Record<string, unknown>,
  nowMs: number
): { ok: true; fields: ApiKeyCreateFields } | { ok: false; response: Response } => {
  const expiresAtMs = normalizeApiKeyExpiresAtMs(raw.expires_at_ms, nowMs);
  if (expiresAtMs === null) {
    return { ok: false, response: openaiError(400, "expires_at_ms must be a Unix epoch ms timestamp in the future, or -1", "invalid_request_error") };
  }

  const usageLimitRequests = normalizeApiKeyUsageLimit(raw.usage_limit_requests);
  if (usageLimitRequests === null) {
    return { ok: false, response: openaiError(400, "usage_limit_requests must be a positive number or -1 for unlimited", "invalid_request_error") };
  }

  const windowMs = normalizeApiKeyWindowMsInput(raw.window_ms);
  if (raw.window_ms !== undefined && windowMs === null) {
    return { ok: false, response: openaiError(400, "window_ms must be a positive number", "invalid_request_error") };
  }

  const paidFallback = resolveApiKeyCreatePaidFallback(raw);
  if (!paidFallback.ok) return paidFallback;

  return {
    ok: true,
    fields: {
      expiresAtMs,
      usageLimitRequests,
      windowMs: windowMs ?? USAGE_RESET_PERIOD_MS,
      paidFallbackEnabled: paidFallback.paidFallbackEnabled,
      paidFallbackLimitMicrocredits: paidFallback.paidFallbackLimitMicrocredits,
    },
  };
};

/**
 * Starts from the strict default policy. An enabled key additionally inherits
 * the Metered-owned pricing fields, and an initialization failure is reported
 * before anything is written.
 */
const resolveApiKeyCreatePolicy = async (
  signal: AbortSignal,
  fields: ApiKeyCreateFields
): Promise<{ ok: true; policy: ReturnType<typeof defaultPaidFallbackPolicy> } | { ok: false; response: Response }> => {
  const paidFallbackPolicy = defaultPaidFallbackPolicy();
  if (!fields.paidFallbackEnabled) {
    return { ok: true, policy: { ...paidFallbackPolicy, paid_fallback_limit_microcredits: fields.paidFallbackLimitMicrocredits } };
  }
  try {
    return {
      ok: true,
      policy: {
        ...paidFallbackPolicy,
        ...(await initializePaidFallbackPolicy(signal)),
        paid_fallback_enabled: true,
        paid_fallback_limit_microcredits: fields.paidFallbackLimitMicrocredits,
      },
    };
  } catch (error) {
    return { ok: false, response: paidFallbackInitializationError(error) };
  }
};

export const handleAdminApiKeysCreate = async (req: Request): Promise<Response> => {
  const kv = await getKv();
  if (!kv) {
    return openaiError(500, "Deno KV is not available; cannot manage API keys", "server_error");
  }

  const raw = await readJsonBody(req);
  if (!raw || !isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const name = normalizeApiKeyName(raw.name);
  if (!name) return openaiError(400, "name must be a non-empty string (<=80 chars)", "invalid_request_error");

  const tokenResult = resolveApiKeyCreateToken(raw);
  if (!tokenResult.ok) return tokenResult.response;
  const { token } = tokenResult;

  const now = Date.now();
  const createFields = resolveApiKeyCreateFields(raw, now);
  if (!createFields.ok) return createFields.response;
  const { expiresAtMs, usageLimitRequests, windowMs } = createFields.fields;

  const hash = await sha256Base64Url(token);
  const hashKey = apiKeyHashKey(hash);
  const hashEntry = await kv.get<ApiKeyHashRecord>(hashKey);
  if (hashEntry.value) {
    return openaiError(409, "API key already exists", "invalid_request_error");
  }

  const policy = await resolveApiKeyCreatePolicy(req.signal, createFields.fields);
  if (!policy.ok) return policy.response;

  const id = crypto.randomUUID();
  const usageResetAtMs = calculateNextResetMs(now, windowMs);
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
    window_ms: windowMs,
    usage_quota_version: 3,
    ...policy.policy,
  };
  const hashRecord: ApiKeyHashRecord = {
    id,
    expires_at_ms: expiresAtMs,
    revoked_at_ms: null,
    usage_limit_requests: usageLimitRequests,
    usage_requests: 0,
    usage_reset_at_ms: usageResetAtMs,
    window_ms: windowMs,
    usage_quota_version: 3,
    ...paidFallbackHashFields(record),
  };
  const quotaPolicy = apiKeyPolicyFromHashRecord(hash, hashRecord, now);
  if (!quotaPolicy) {
    return openaiError(500, "Failed to build API key quota policy", "server_error");
  }
  const quotaWindow = makeApiKeyUsageWindowV3(quotaPolicy, now);

  const commit = await kv
    .atomic()
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
      ...(await paidFallbackPublicFields(record, kv)),
    },
    { "x-uos-upstream": "chatgpt_codex" }
  );
};

export const handleAdminApiKeysList = async (req: Request): Promise<Response> => {
  const kv = await getKv();
  if (!kv) {
    return openaiError(500, "Deno KV is not available; cannot manage API keys", "server_error");
  }

  const records: ApiKeyRecord[] = [];
  // A listed record may be missing its value; only well-formed records are listed.
  for await (const entry of kv.list<ApiKeyRecord | null>({ prefix: API_KEY_ID_PREFIX })) {
    const record = entry.value;
    if (record) records.push(record);
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
      paidFallbackResetById.set(record.id, record.revoked_at_ms === null ? policy.usage_reset_at_ms : record.usage_reset_at_ms);
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
      records.map(
        async (record) => [record.id, await paidFallbackPublicFields(record, kv, paidFallbackResetById.get(record.id) ?? record.usage_reset_at_ms)] as const
      )
    )
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
        usage_reset_at_ms: includeUsage ? (usageById.get(r.id)?.reset_at_ms ?? r.usage_reset_at_ms) : r.usage_reset_at_ms,
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
    { "x-uos-upstream": "chatgpt_codex" }
  );
};

export const handleAdminApiKeysPaidFallbacks = async (req: Request, keyId: string, kvOverride?: Deno.Kv | null): Promise<Response> => {
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
    return json(200, { object: "list", data: records }, { "Cache-Control": "no-store" });
  } catch (error) {
    console.error("[ai.ubq.fi] Failed to load paid fallback ledger:", error);
    return openaiError(500, "Failed to load paid fallbacks", "server_error");
  }
};

type ApiKeyUpdateTarget = Readonly<{
  ok: true;
  id: string;
  idKey: Deno.KvKey;
  entry: Deno.KvEntryMaybe<ApiKeyRecord>;
  record: ApiKeyRecord;
}>;

/** Reads the target key and enforces the paid-fallback migration policy. */
const resolveApiKeyUpdateTarget = async (kv: Deno.Kv, raw: Record<string, unknown>): Promise<ApiKeyUpdateTarget | { ok: false; response: Response }> => {
  const id = getString(raw.id);
  if (!id) return { ok: false, response: openaiError(400, "id is required", "invalid_request_error") };

  const idKey = apiKeyIdKey(id);
  const entry = await kv.get<ApiKeyRecord>(idKey);
  const record = entry.value;
  if (!record) return { ok: false, response: openaiError(404, "Not found", "not_found") };
  if (!hasStrictPaidFallbackKeyPolicy(record)) {
    return {
      ok: false,
      response: openaiError(503, "API key paid fallback migration is incomplete", "server_error", {
        type: "server_error",
      }),
    };
  }
  return { ok: true, id, idKey, entry, record };
};

type ApiKeyUpdateFields = Readonly<{
  name: string;
  expiresAtMs: number;
  usageLimitRequests: number;
  windowMs: number;
  paidFallbackEnabled: boolean;
  paidFallbackLimitMicrocredits: number;
  paidFallbackModelIds: string[];
  paidFallbackQuotaPerCredit: number;
  paidFallbackMaxExposureMicrocredits: Record<string, number>;
  paidFallbackPricingCheckedAtMs: number | null;
  resetUsage: boolean;
}>;

type ApiKeyUpdateIdentity = Pick<ApiKeyUpdateFields, "name" | "expiresAtMs">;
type ApiKeyUpdateQuota = Pick<ApiKeyUpdateFields, "usageLimitRequests" | "windowMs">;
type ApiKeyUpdatePaidFallback = Pick<
  ApiKeyUpdateFields,
  | "paidFallbackEnabled"
  | "paidFallbackLimitMicrocredits"
  | "paidFallbackModelIds"
  | "paidFallbackQuotaPerCredit"
  | "paidFallbackMaxExposureMicrocredits"
  | "paidFallbackPricingCheckedAtMs"
>;

/** `name` and `expires_at_ms`, validated in that order. */
const resolveApiKeyUpdateIdentity = (
  raw: Record<string, unknown>,
  record: ApiKeyRecord,
  currentExpiresAtMs: number,
  nowMs: number
): { ok: true; identity: ApiKeyUpdateIdentity } | { ok: false; response: Response } => {
  let name = record.name;
  if (Object.prototype.hasOwnProperty.call(raw, "name")) {
    const normalized = normalizeApiKeyName(raw.name);
    if (!normalized) return { ok: false, response: openaiError(400, "name must be a non-empty string (<=80 chars)", "invalid_request_error") };
    name = normalized;
  }

  let expiresAtMs = currentExpiresAtMs;
  if (Object.prototype.hasOwnProperty.call(raw, "expires_at_ms")) {
    const normalized = normalizeApiKeyExpiresAtMs(raw.expires_at_ms, nowMs);
    if (normalized === null) {
      return {
        ok: false,
        response: openaiError(400, "expires_at_ms must be a Unix epoch ms timestamp in the future, or -1", "invalid_request_error"),
      };
    }
    expiresAtMs = normalized;
  }

  return { ok: true, identity: { name, expiresAtMs } };
};

/** `usage_limit_requests` and `window_ms`, validated in that order. */
const resolveApiKeyUpdateQuota = (
  raw: Record<string, unknown>,
  record: ApiKeyRecord
): { ok: true; quota: ApiKeyUpdateQuota } | { ok: false; response: Response } => {
  let usageLimitRequests = record.usage_limit_requests;
  if (Object.prototype.hasOwnProperty.call(raw, "usage_limit_requests")) {
    const normalized = normalizeApiKeyUsageLimit(raw.usage_limit_requests);
    if (normalized === null) {
      return { ok: false, response: openaiError(400, "usage_limit_requests must be a non-negative number or -1 for unlimited", "invalid_request_error") };
    }
    usageLimitRequests = normalized;
  }

  let windowMs = coerceApiKeyWindowMs(record);
  if (Object.prototype.hasOwnProperty.call(raw, "window_ms")) {
    const normalized = normalizeApiKeyWindowMsInput(raw.window_ms);
    if (normalized === null) {
      return { ok: false, response: openaiError(400, "window_ms must be a positive number", "invalid_request_error") };
    }
    windowMs = normalized;
  }

  return { ok: true, quota: { usageLimitRequests, windowMs } };
};

/**
 * The paid-fallback patch, including the one-shot Metered policy initialization
 * that only runs when the key is being enabled for the first time.
 */
const resolveApiKeyUpdatePaidFallback = async (
  raw: Record<string, unknown>,
  record: ApiKeyRecord,
  signal: AbortSignal
): Promise<{ ok: true; paidFallback: ApiKeyUpdatePaidFallback } | { ok: false; response: Response }> => {
  if (Object.prototype.hasOwnProperty.call(raw, "paid_fallback_enabled") && typeof raw.paid_fallback_enabled !== "boolean") {
    return { ok: false, response: paidFallbackInputError("paid_fallback_enabled must be a boolean") };
  }
  let paidFallbackEnabled = record.paid_fallback_enabled;
  if (Object.prototype.hasOwnProperty.call(raw, "paid_fallback_enabled")) {
    paidFallbackEnabled = raw.paid_fallback_enabled === true;
  }

  let paidFallbackLimitMicrocredits = record.paid_fallback_limit_microcredits;
  if (Object.prototype.hasOwnProperty.call(raw, "paid_fallback_limit_credits")) {
    const limitMicrocredits = paidFallbackCreditsToMicrocredits(raw.paid_fallback_limit_credits);
    if (limitMicrocredits === null) {
      return { ok: false, response: paidFallbackInputError("paid_fallback_limit_credits must be a non-negative number or -1") };
    }
    paidFallbackLimitMicrocredits = limitMicrocredits;
  }
  if (paidFallbackEnabled && paidFallbackLimitMicrocredits === 0) {
    return { ok: false, response: paidFallbackInputError("paid_fallback_limit_credits must be positive or -1 when paid fallback is enabled") };
  }

  const paidFallback: ApiKeyUpdatePaidFallback = {
    paidFallbackEnabled,
    paidFallbackLimitMicrocredits,
    paidFallbackModelIds: record.paid_fallback_model_ids,
    paidFallbackQuotaPerCredit: record.paid_fallback_quota_per_credit,
    paidFallbackMaxExposureMicrocredits: record.paid_fallback_max_exposure_microcredits ?? {},
    paidFallbackPricingCheckedAtMs: record.paid_fallback_pricing_checked_at_ms,
  };
  if (record.paid_fallback_enabled || !paidFallbackEnabled) return { ok: true, paidFallback };

  try {
    const initialized = await initializePaidFallbackPolicy(signal);
    return {
      ok: true,
      paidFallback: {
        ...paidFallback,
        paidFallbackModelIds: [...initialized.paid_fallback_model_ids],
        paidFallbackQuotaPerCredit: initialized.paid_fallback_quota_per_credit,
        paidFallbackMaxExposureMicrocredits: initialized.paid_fallback_max_exposure_microcredits ?? {},
        paidFallbackPricingCheckedAtMs: initialized.paid_fallback_pricing_checked_at_ms,
      },
    };
  } catch (error) {
    return { ok: false, response: paidFallbackInitializationError(error) };
  }
};

const resolveApiKeyUpdateFields = async (
  raw: Record<string, unknown>,
  record: ApiKeyRecord,
  currentExpiresAtMs: number,
  nowMs: number,
  signal: AbortSignal
): Promise<{ ok: true; fields: ApiKeyUpdateFields } | { ok: false; response: Response }> => {
  const identity = resolveApiKeyUpdateIdentity(raw, record, currentExpiresAtMs, nowMs);
  if (!identity.ok) return identity;

  const quota = resolveApiKeyUpdateQuota(raw, record);
  if (!quota.ok) return quota;

  const paidFallback = await resolveApiKeyUpdatePaidFallback(raw, record, signal);
  if (!paidFallback.ok) return paidFallback;

  if (Object.prototype.hasOwnProperty.call(raw, "reset_usage") && typeof raw.reset_usage !== "boolean") {
    return { ok: false, response: openaiError(400, "reset_usage must be a boolean", "invalid_request_error") };
  }

  return {
    ok: true,
    fields: {
      ...identity.identity,
      ...quota.quota,
      ...paidFallback.paidFallback,
      resetUsage: normalizeOptionalBoolean(raw.reset_usage),
    },
  };
};

/**
 * Merges the patch into the stored record. A reset (explicit, or implied by a
 * window change) must always select a distinct V3 aggregate identity: a create
 * followed by an immediate reset can otherwise share the same millisecond start
 * and overwrite the current window instead of opening a fresh one.
 */
const buildApiKeyUpdateRecord = (
  record: ApiKeyRecord,
  fields: ApiKeyUpdateFields,
  nowMs: number,
  currentWindowMs: number
): { updated: ApiKeyRecord; resetUsage: boolean; replaceQuotaWindow: boolean } => {
  const replaceQuotaWindow = fields.resetUsage || fields.windowMs !== currentWindowMs;
  let usageRequests = record.usage_requests;
  let usageResetAtMs = record.usage_reset_at_ms;
  let paidFallbackSpentMicrocredits = record.paid_fallback_spent_microcredits;
  if (replaceQuotaWindow) {
    usageRequests = 0;
    const currentWindowStartMs = record.usage_reset_at_ms - currentWindowMs;
    const freshWindowStartMs = Math.max(nowMs, currentWindowStartMs + 1);
    usageResetAtMs = freshWindowStartMs + fields.windowMs;
    paidFallbackSpentMicrocredits = 0;
  }
  return {
    updated: {
      ...record,
      name: fields.name,
      expires_at_ms: fields.expiresAtMs,
      usage_limit_requests: fields.usageLimitRequests,
      usage_requests: usageRequests,
      usage_reset_at_ms: usageResetAtMs,
      window_ms: fields.windowMs,
      paid_fallback_enabled: fields.paidFallbackEnabled,
      paid_fallback_limit_microcredits: fields.paidFallbackLimitMicrocredits,
      paid_fallback_spent_microcredits: paidFallbackSpentMicrocredits,
      paid_fallback_reserved_microcredits: record.paid_fallback_reserved_microcredits,
      paid_fallback_reservation_request_id: record.paid_fallback_reservation_request_id,
      paid_fallback_model_ids: fields.paidFallbackModelIds,
      paid_fallback_quota_per_credit: fields.paidFallbackQuotaPerCredit,
      paid_fallback_max_exposure_microcredits: fields.paidFallbackMaxExposureMicrocredits,
      paid_fallback_pricing_checked_at_ms: fields.paidFallbackPricingCheckedAtMs,
    },
    resetUsage: fields.resetUsage,
    replaceQuotaWindow,
  };
};

const apiKeyUpdateIdentityChanged = (record: ApiKeyRecord, updated: ApiKeyRecord, currentExpiresAtMs: number): boolean =>
  updated.name !== record.name || updated.expires_at_ms !== currentExpiresAtMs;

const apiKeyUpdateQuotaChanged = (record: ApiKeyRecord, updated: ApiKeyRecord, currentWindowMs: number, resetUsage: boolean): boolean =>
  updated.usage_limit_requests !== record.usage_limit_requests ||
  updated.window_ms !== currentWindowMs ||
  (resetUsage && (updated.usage_requests !== record.usage_requests || updated.usage_reset_at_ms !== record.usage_reset_at_ms));

const apiKeyUpdatePaidFallbackChanged = (record: ApiKeyRecord, updated: ApiKeyRecord): boolean =>
  updated.paid_fallback_enabled !== record.paid_fallback_enabled ||
  updated.paid_fallback_limit_microcredits !== record.paid_fallback_limit_microcredits ||
  updated.paid_fallback_spent_microcredits !== record.paid_fallback_spent_microcredits ||
  updated.paid_fallback_reserved_microcredits !== record.paid_fallback_reserved_microcredits ||
  updated.paid_fallback_reservation_request_id !== record.paid_fallback_reservation_request_id ||
  updated.paid_fallback_model_ids !== record.paid_fallback_model_ids ||
  updated.paid_fallback_quota_per_credit !== record.paid_fallback_quota_per_credit ||
  updated.paid_fallback_pricing_checked_at_ms !== record.paid_fallback_pricing_checked_at_ms;

/** Every comparison is pure, so this matches the original short-circuit chain. */
const apiKeyUpdateChanged = (record: ApiKeyRecord, updated: ApiKeyRecord, currentExpiresAtMs: number, currentWindowMs: number, resetUsage: boolean): boolean =>
  apiKeyUpdateIdentityChanged(record, updated, currentExpiresAtMs) ||
  apiKeyUpdateQuotaChanged(record, updated, currentWindowMs, resetUsage) ||
  apiKeyUpdatePaidFallbackChanged(record, updated);

/** The no-op response, which reports the live usage counter of the stored policy. */
const apiKeyUpdateUnchangedResponse = async (
  kv: Deno.Kv,
  record: ApiKeyRecord,
  currentExpiresAtMs: number,
  currentWindowMs: number,
  nowMs: number
): Promise<Response> => {
  const currentPolicy = apiKeyPolicyFromHashRecord(
    record.hash,
    {
      id: record.id,
      expires_at_ms: record.expires_at_ms,
      revoked_at_ms: record.revoked_at_ms,
      usage_limit_requests: record.usage_limit_requests,
      usage_requests: record.usage_requests,
      usage_reset_at_ms: record.usage_reset_at_ms,
      window_ms: record.window_ms,
      usage_quota_version: record.usage_quota_version,
      ...paidFallbackHashFields(record),
    },
    nowMs
  );
  return json(
    200,
    {
      id: record.id,
      name: record.name,
      prefix: record.prefix,
      created_at_ms: record.created_at_ms,
      expires_at_ms: currentExpiresAtMs,
      revoked_at_ms: record.revoked_at_ms,
      usage_limit_requests: record.usage_limit_requests,
      usage_requests: currentPolicy ? await getApiKeyUsageV3(currentPolicy, kv) : 0,
      usage_reset_at_ms: record.usage_reset_at_ms,
      window_ms: currentWindowMs,
      ...(await paidFallbackPublicFields(record, kv)),
    },
    { "x-uos-upstream": "chatgpt_codex" }
  );
};

/**
 * Reads the superseded V3 aggregate after reclaim and before the live scan. A
 * reservation before this read is included in the scan; one after it mutates
 * this checked entry and makes the reset conflict atomically.
 */
const inspectApiKeyQuotaResetReservations = async (
  kv: Deno.Kv,
  input: Readonly<{ keyId: string; currentQuotaPolicy: NonNullable<ReturnType<typeof apiKeyPolicyFromHashRecord>>; nowMs: number }>
): Promise<{ ok: true; windowEntry: Deno.KvEntryMaybe<ApiKeyUsageWindowV3> } | { ok: false; response: Response }> => {
  try {
    await reclaimApiKeyUsageReservationsForKeyV3(kv, input.keyId, input.nowMs);
    const windowEntry = await kv.get<ApiKeyUsageWindowV3>(apiKeyUsageV3WindowKey(input.currentQuotaPolicy), { consistency: "strong" });
    if (await hasLiveApiKeyUsageReservationsV3(kv, input.keyId, input.nowMs)) {
      return {
        ok: false,
        response: openaiError(
          409,
          "Cannot reset API key quota while requests are reserved; retry after their five-minute lease expires",
          "invalid_request_error"
        ),
      };
    }
    return { ok: true, windowEntry };
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to inspect API key quota reservations before reset:", error);
    return { ok: false, response: openaiError(503, "API key quota ledger is unavailable", "server_error", { type: "server_error" }) };
  }
};

/** Rechecks reservations after a lost commit race, then reports the conflict. */
const resolveApiKeyUpdateCommitConflict = async (
  kv: Deno.Kv,
  input: Readonly<{ keyId: string; nowMs: number; replaceQuotaWindow: boolean }>
): Promise<Response> => {
  if (!input.replaceQuotaWindow) return openaiError(409, "API key was modified concurrently; retry", "invalid_request_error");
  try {
    await reclaimApiKeyUsageReservationsForKeyV3(kv, input.keyId, input.nowMs);
    if (await hasLiveApiKeyUsageReservationsV3(kv, input.keyId, input.nowMs)) {
      return openaiError(409, "Cannot reset API key quota while requests are reserved; retry after their five-minute lease expires", "invalid_request_error");
    }
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to recheck API key quota reservations after reset conflict:", error);
    return openaiError(503, "API key quota ledger is unavailable", "server_error", { type: "server_error" });
  }
  return openaiError(409, "API key was modified concurrently; retry", "invalid_request_error");
};

/** Writes the record, hash record, and (when the window is replaced) quota windows atomically. */
const persistApiKeyUpdate = async (
  kv: Deno.Kv,
  input: Readonly<{
    idKey: Deno.KvKey;
    entry: Deno.KvEntryMaybe<ApiKeyRecord>;
    record: ApiKeyRecord;
    updated: ApiKeyRecord;
    replaceQuotaWindow: boolean;
    nowMs: number;
  }>
): Promise<
  { ok: true; updated: ApiKeyRecord; quotaPolicy: NonNullable<ReturnType<typeof apiKeyPolicyFromHashRecord>> } | { ok: false; response: Response }
> => {
  const hashKey = apiKeyHashKey(input.record.hash);
  const hashEntry = await kv.get<ApiKeyHashRecord>(hashKey);
  const updatedHash: ApiKeyHashRecord = {
    id: input.updated.id,
    expires_at_ms: input.updated.expires_at_ms,
    revoked_at_ms: input.updated.revoked_at_ms,
    usage_limit_requests: input.updated.usage_limit_requests,
    usage_requests: input.updated.usage_requests,
    usage_reset_at_ms: input.updated.usage_reset_at_ms,
    window_ms: input.updated.window_ms,
    usage_quota_version: input.updated.usage_quota_version,
    ...paidFallbackHashFields(input.updated),
  };

  const quotaPolicy = apiKeyPolicyFromHashRecord(input.updated.hash, updatedHash, input.nowMs);
  if (!quotaPolicy) {
    return { ok: false, response: openaiError(503, "API key quota migration is incomplete", "server_error", { type: "server_error" }) };
  }

  let currentQuotaWindowEntry: Deno.KvEntryMaybe<ApiKeyUsageWindowV3> | null = null;
  if (input.replaceQuotaWindow) {
    // The guard lives here so control-flow narrowing proves `currentQuotaPolicy`
    // is non-null for `apiKeyUsageV3WindowKey` below.
    const currentQuotaPolicy = apiKeyPolicyFromHashRecord(input.record.hash, input.record, input.nowMs);
    if (!currentQuotaPolicy) {
      return { ok: false, response: openaiError(503, "API key quota migration is incomplete", "server_error", { type: "server_error" }) };
    }
    const inspected = await inspectApiKeyQuotaResetReservations(kv, { keyId: input.updated.id, currentQuotaPolicy, nowMs: input.nowMs });
    if (!inspected.ok) return inspected;
    currentQuotaWindowEntry = inspected.windowEntry;
  }

  const quotaWindow = input.replaceQuotaWindow ? makeApiKeyUsageWindowV3(quotaPolicy, input.nowMs) : null;
  const quotaWindowEntry = quotaWindow ? await kv.get(apiKeyUsageV3WindowKey(quotaPolicy), { consistency: "strong" }) : null;

  const atomic = kv.atomic().check(input.entry).check(hashEntry).set(input.idKey, input.updated).set(hashKey, updatedHash);
  if (quotaWindow && quotaWindowEntry) {
    atomic.check(quotaWindowEntry).set(apiKeyUsageV3WindowKey(quotaPolicy), quotaWindow, {
      expireIn: apiKeyUsageV3RetentionMs(quotaWindow.window_reset_at_ms, input.nowMs),
    });
  }
  if (currentQuotaWindowEntry) atomic.check(currentQuotaWindowEntry);

  const commit = await atomic.commit();
  if (!commit.ok) {
    return {
      ok: false,
      response: await resolveApiKeyUpdateCommitConflict(kv, {
        keyId: input.updated.id,
        nowMs: input.nowMs,
        replaceQuotaWindow: input.replaceQuotaWindow,
      }),
    };
  }
  invalidateApiKeyPolicy(input.updated.id);
  return { ok: true, updated: input.updated, quotaPolicy };
};

export const handleAdminApiKeysUpdate = async (req: Request): Promise<Response> => {
  const kv = await getKv();
  if (!kv) {
    return openaiError(500, "Deno KV is not available; cannot manage API keys", "server_error");
  }

  const raw = await readJsonBody(req);
  if (!raw || !isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const target = await resolveApiKeyUpdateTarget(kv, raw);
  if (!target.ok) return target.response;
  const { idKey, entry, record } = target;

  const now = Date.now();
  const currentExpiresAtMs = coerceApiKeyExpiresAtMs(record);
  const currentWindowMs = coerceApiKeyWindowMs(record);
  const fields = await resolveApiKeyUpdateFields(raw, record, currentExpiresAtMs, now, req.signal);
  if (!fields.ok) return fields.response;

  const { updated, resetUsage, replaceQuotaWindow } = buildApiKeyUpdateRecord(record, fields.fields, now, currentWindowMs);
  if (!apiKeyUpdateChanged(record, updated, currentExpiresAtMs, currentWindowMs, resetUsage)) {
    return await apiKeyUpdateUnchangedResponse(kv, record, currentExpiresAtMs, currentWindowMs, now);
  }

  const persisted = await persistApiKeyUpdate(kv, { idKey, entry, record, updated, replaceQuotaWindow, nowMs: now });
  if (!persisted.ok) return persisted.response;

  return json(
    200,
    {
      id: persisted.updated.id,
      name: persisted.updated.name,
      prefix: persisted.updated.prefix,
      created_at_ms: persisted.updated.created_at_ms,
      expires_at_ms: coerceApiKeyExpiresAtMs(persisted.updated),
      revoked_at_ms: persisted.updated.revoked_at_ms,
      usage_limit_requests: persisted.updated.usage_limit_requests,
      usage_requests: await getApiKeyUsageV3(persisted.quotaPolicy, kv),
      usage_reset_at_ms: persisted.updated.usage_reset_at_ms,
      window_ms: persisted.updated.window_ms,
      ...(await paidFallbackPublicFields(persisted.updated, kv)),
    },
    { "x-uos-upstream": "chatgpt_codex" }
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

  const atomic = kv.atomic().check(entry).set(idKey, updated).set(hashKey, updatedHash);
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
    { "x-uos-upstream": "chatgpt_codex" }
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
    return openaiError(409, "API key deletion is in progress and cannot be reversed", "paid_fallback_deletion_in_progress");
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

  const atomic = kv.atomic().check(entry).check(deletionGuard).set(idKey, updated).set(hashKey, updatedHash);
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
    { "x-uos-upstream": "chatgpt_codex" }
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
    const guardCommit = await kv.atomic().check(entry).check(deletionGuard).set(deletionGuardKey, { created_at_ms: Date.now() }).commit();
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
      "paid_fallback_billing_outstanding"
    );
  }

  const atomic = kv.atomic().check(entry).delete(idKey).delete(apiKeyHashKey(entry.value.hash)).delete(apiKeyUsageKey(id)).delete(apiKeyUsageDailyKey(id));

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
  const kvEntry = await kv.get<{ app_id: number; pem: string; owner: string; added_at_ms: number }[]>(UOS_KERNEL_PUBKEYS_KEY);
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

  const entry = await kv.get<{ app_id: number; pem: string; owner: string; added_at_ms: number }[]>(UOS_KERNEL_PUBKEYS_KEY);
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

  const entry = await kv.get<{ app_id: number; pem: string; owner: string; added_at_ms: number }[]>(UOS_KERNEL_PUBKEYS_KEY);
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
    })
  );

  const repoPolicyPairs = new Set<string>();
  await Promise.all(
    records
      .filter((record) => !orgPolicyOwners.has(record.owner))
      .map(async (record) => {
        const entry = await kv.get(kernelLimitKey(record.owner, record.repo));
        if (entry.value) repoPolicyPairs.add(`${record.owner}/${record.repo}`);
      })
  );

  const pending = records.filter((record) => {
    if (orgPolicyOwners.has(record.owner)) return false;
    return !repoPolicyPairs.has(`${record.owner}/${record.repo}`);
  });

  return json(200, { data: pending });
};

const KERNEL_USAGE_DAILY_DAYS = 30;

const kernelUsageInventoryResponse = async (scope: "repo" | "org"): Promise<Response> => {
  if (scope === "org") {
    const orgRecords = await listKernelOrgUsageRecords({ includeDaily: true, dailyDays: KERNEL_USAGE_DAILY_DAYS });
    if (!orgRecords) {
      return openaiError(500, "Failed to load kernel org usage inventory", "server_error");
    }
    return json(200, { ok: true, scope, usage: orgRecords });
  }

  const records = await listKernelUsageRecords({ includeDaily: true, dailyDays: KERNEL_USAGE_DAILY_DAYS });
  if (!records) {
    return openaiError(500, "Failed to load kernel usage inventory", "server_error");
  }
  return json(200, { ok: true, scope, usage: records });
};

const kernelOrgUsageLimitsResponse = async (scope: "repo" | "org", includeUsage: boolean): Promise<Response> => {
  const limits = await listKernelOrgUsageLimits();
  if (!limits) {
    return openaiError(500, "Failed to load kernel org usage limits", "server_error");
  }
  const usageByOwner = new Map<string, Awaited<ReturnType<typeof getKernelOrgUsage>>>();
  if (includeUsage) {
    await Promise.all(
      limits.map(async (record) => {
        usageByOwner.set(record.owner, await getKernelOrgUsage(record.owner, { includeDaily: true, dailyDays: KERNEL_USAGE_DAILY_DAYS }));
      })
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
};

const kernelRepoUsageLimitsResponse = async (scope: "repo" | "org", includeUsage: boolean): Promise<Response> => {
  const limits = await listKernelUsageLimits();
  if (!limits) {
    return openaiError(500, "Failed to load kernel usage limits", "server_error");
  }
  const usageByRepo = new Map<string, Awaited<ReturnType<typeof getKernelUsage>>>();
  if (includeUsage) {
    await Promise.all(
      limits.map(async (record) => {
        const key = `${record.owner}/${record.repo}`;
        usageByRepo.set(key, await getKernelUsage(record.owner, record.repo, { includeDaily: true, dailyDays: KERNEL_USAGE_DAILY_DAYS }));
      })
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
};

const kernelUsageListResponse = async (scope: "repo" | "org", includeUsage: boolean): Promise<Response> =>
  scope === "org" ? await kernelOrgUsageLimitsResponse(scope, includeUsage) : await kernelRepoUsageLimitsResponse(scope, includeUsage);

const kernelOrgUsageSnapshotResponse = async (owner: string, includeUsage: boolean): Promise<Response> => {
  const limitSnapshot = await getKernelOrgUsageLimitSnapshot(owner);
  if (!limitSnapshot) {
    return openaiError(500, "Failed to load kernel org usage limit", "server_error");
  }
  const usage = await getKernelOrgUsage(owner, { includeDaily: includeUsage, dailyDays: KERNEL_USAGE_DAILY_DAYS });
  return json(200, {
    ok: true,
    org: { owner },
    limit: { ...limitSnapshot.record, source: limitSnapshot.source },
    usage: usage ?? null,
  });
};

const kernelRepoUsageSnapshotResponse = async (owner: string, repo: string, includeUsage: boolean): Promise<Response> => {
  const limitSnapshot = await getKernelUsageLimitSnapshot(owner, repo);
  if (!limitSnapshot) {
    return openaiError(500, "Failed to load kernel usage limit", "server_error");
  }
  const usage = await getKernelUsage(owner, repo, { includeDaily: includeUsage, dailyDays: KERNEL_USAGE_DAILY_DAYS });
  return json(200, {
    ok: true,
    repo: { owner, repo },
    limit: { ...limitSnapshot.record, source: limitSnapshot.source },
    usage: usage ?? null,
  });
};

export const handleAdminKernelUsageGet = async (req: Request): Promise<Response> => {
  const kv = await getKv();
  if (!kv) return openaiError(500, "Deno KV is not available", "server_error");

  const url = new URL(req.url);
  const scope = normalizeKernelScope(url.searchParams.get("scope"));
  const listRequested = shouldIncludeUsage(url.searchParams.get("list"));
  const inventoryRequested = shouldIncludeUsage(url.searchParams.get("inventory"));
  const includeUsage = shouldIncludeUsage(url.searchParams.get("include_usage"));
  if (inventoryRequested) return await kernelUsageInventoryResponse(scope);
  if (listRequested) return await kernelUsageListResponse(scope, includeUsage);

  const owner = normalizeKernelRepoPart(url.searchParams.get("owner"));
  if (!owner) {
    return openaiError(400, "owner query parameter is required", "invalid_request_error");
  }

  if (scope === "org") return await kernelOrgUsageSnapshotResponse(owner, includeUsage);

  const repo = normalizeKernelRepoPart(url.searchParams.get("repo"));
  if (!repo) {
    return openaiError(400, "repo query parameter is required", "invalid_request_error");
  }

  return await kernelRepoUsageSnapshotResponse(owner, repo, includeUsage);
};

type KernelUsageTarget = { ok: true; scope: "org"; owner: string; repo: null } | { ok: true; scope: "repo"; owner: string; repo: string };

/** The shared `owner`/`repo`/`scope` validation for the Kernel usage writers. */
const resolveKernelUsageTarget = (raw: Record<string, unknown>): KernelUsageTarget | { ok: false; response: Response } => {
  const owner = normalizeKernelRepoPart(raw.owner);
  const repo = normalizeKernelRepoPart(raw.repo);
  if (!owner) return { ok: false, response: openaiError(400, "owner is required", "invalid_request_error") };
  const scope = normalizeKernelScope(raw.scope ?? (repo ? "repo" : "org"));
  if (scope === "repo") {
    if (!repo) return { ok: false, response: openaiError(400, "repo is required for scope=repo", "invalid_request_error") };
    return { ok: true, scope: "repo", owner, repo };
  }
  if (repo) return { ok: false, response: openaiError(400, "repo must be omitted for scope=org", "invalid_request_error") };
  return { ok: true, scope: "org", owner, repo: null };
};

type KernelUsageLimits = Readonly<{ usageLimitRequests: number; windowMs: number | null; expiresAtMs: number | null; resetUsage: boolean }>;

const resolveKernelUsageLimits = (raw: Record<string, unknown>): { ok: true; limits: KernelUsageLimits } | { ok: false; response: Response } => {
  const usageLimitRequests = normalizeKernelUsageLimitInput(raw.usage_limit_requests);
  if (usageLimitRequests === null) {
    return { ok: false, response: openaiError(400, "usage_limit_requests must be a non-negative number, -1, or 'unlimited'", "invalid_request_error") };
  }

  const windowMs = normalizeKernelWindowMsInput(raw.window_ms);
  if (raw.window_ms !== undefined && windowMs === null) {
    return { ok: false, response: openaiError(400, "window_ms must be a positive number", "invalid_request_error") };
  }

  const nowMs = Date.now();
  const expiresAtMs = normalizeKernelExpiresAtMsInput(raw.expires_at_ms, nowMs);
  if (raw.expires_at_ms !== undefined && expiresAtMs === null) {
    return { ok: false, response: openaiError(400, "expires_at_ms must be a Unix epoch ms timestamp in the future, or -1", "invalid_request_error") };
  }

  if (Object.prototype.hasOwnProperty.call(raw, "reset_usage") && typeof raw.reset_usage !== "boolean") {
    return { ok: false, response: openaiError(400, "reset_usage must be a boolean", "invalid_request_error") };
  }

  return { ok: true, limits: { usageLimitRequests, windowMs, expiresAtMs, resetUsage: raw.reset_usage === true } };
};

const kernelOrgUsageLimitSetResponse = async (owner: string, limits: KernelUsageLimits): Promise<Response> => {
  const updated = await setKernelOrgUsageLimit(owner, limits.usageLimitRequests, {
    windowMs: limits.windowMs ?? undefined,
    expiresAtMs: limits.expiresAtMs ?? undefined,
    resetUsage: limits.resetUsage,
  });
  if (!updated) {
    return openaiError(409, "Concurrent modification; retry", "invalid_request_error");
  }
  return json(200, { ok: true, scope: "org", org: { owner }, limit: { ...updated, source: "kv" } });
};

export const handleAdminKernelUsageSet = async (req: Request): Promise<Response> => {
  const kv = await getKv();
  if (!kv) return openaiError(500, "Deno KV is not available", "server_error");

  const raw = await readJsonBody(req);
  if (!raw || !isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const target = resolveKernelUsageTarget(raw);
  if (!target.ok) return target.response;

  const resolved = resolveKernelUsageLimits(raw);
  if (!resolved.ok) return resolved.response;

  if (target.scope === "org") return await kernelOrgUsageLimitSetResponse(target.owner, resolved.limits);

  const updated = await setKernelUsageLimit(target.owner, target.repo, resolved.limits.usageLimitRequests, {
    windowMs: resolved.limits.windowMs ?? undefined,
    expiresAtMs: resolved.limits.expiresAtMs ?? undefined,
    resetUsage: resolved.limits.resetUsage,
  });
  if (!updated) {
    return openaiError(409, "Concurrent modification; retry", "invalid_request_error");
  }

  return json(200, { ok: true, scope: target.scope, repo: { owner: target.owner, repo: target.repo }, limit: { ...updated, source: "kv" } });
};

const kernelOrgUsageLimitDeleteResponse = async (owner: string): Promise<Response> => {
  const deleted = await deleteKernelOrgUsageLimit(owner);
  if (deleted === "conflict") {
    return openaiError(409, "Active Kernel quota reservations must settle before deletion", "invalid_request_error");
  }
  if (deleted === null) {
    return openaiError(500, "Failed to delete kernel org usage limit", "server_error");
  }
  if (!deleted) {
    return openaiError(404, "Kernel org usage limit not found", "not_found");
  }
  return json(200, { ok: true, scope: "org", org: { owner }, deleted: true });
};

const kernelRepoUsageLimitDeleteResponse = async (owner: string, repo: string): Promise<Response> => {
  const deleted = await deleteKernelUsageLimit(owner, repo);
  if (deleted === "conflict") {
    return openaiError(409, "Active Kernel quota reservations must settle before deletion", "invalid_request_error");
  }
  if (deleted === null) {
    return openaiError(500, "Failed to delete kernel usage limit", "server_error");
  }
  if (!deleted) {
    return openaiError(404, "Kernel usage limit not found", "not_found");
  }
  return json(200, { ok: true, scope: "repo", repo: { owner, repo }, deleted: true });
};

export const handleAdminKernelUsageDelete = async (req: Request): Promise<Response> => {
  const kv = await getKv();
  if (!kv) return openaiError(500, "Deno KV is not available", "server_error");

  const raw = await readJsonBody(req);
  if (!raw || !isRecord(raw)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const target = resolveKernelUsageTarget(raw);
  if (!target.ok) return target.response;

  if (target.scope === "org") return await kernelOrgUsageLimitDeleteResponse(target.owner);
  return await kernelRepoUsageLimitDeleteResponse(target.owner, target.repo);
};

const QUOTA_PROJECTION_ALLOWED_WINDOWS = new Set([7, 30, 90] as const);
const QUOTA_PROJECTION_DEFAULT_WINDOW_DAYS = 30 as const;
const QUOTA_PROJECTION_MAX_BALANCE_SAMPLES = 365;

/**
 * Admin quota-runway view: per-model consumption from retained rollups plus
 * exhaustion estimates against the current Metered balance. Reads only the
 * compact stores; never scans raw request rows. The `window_days` parameter
 * bounds the rollup scan (default 30): the UI poll is cheap and does not pull
 * the full 90-day rollup history on every refresh.
 */
export const handleAdminProvidersQuotaProjection = async (
  request: Request = new Request("https://ai.ubq.fi/admin/providers/quota-projection")
): Promise<Response> => {
  const kv = await getKv();
  const nowMs = Date.now();
  const rawWindowDays = Number.parseInt(new URL(request.url).searchParams.get("window_days") ?? "", 10);
  const windowDays =
    Number.isInteger(rawWindowDays) && QUOTA_PROJECTION_ALLOWED_WINDOWS.has(rawWindowDays as 7 | 30 | 90)
      ? (rawWindowDays as 7 | 30 | 90)
      : QUOTA_PROJECTION_DEFAULT_WINDOW_DAYS;
  const balanceWindowDays = normalizeMeteredQuotaBalanceWindowDays(new URL(request.url).searchParams.get("balance_window_days"));
  const windowMs = windowDays * 24 * 60 * 60 * 1_000;
  const balanceWindowMs = balanceWindowDays * 24 * 60 * 60 * 1_000;
  // Keep the complete requested range while bounding every response to 365
  // points. Seven days remains hourly; longer ranges use the smallest whole-
  // hour UTC bucket that can represent the range within the response cap.
  const balanceHistoryBucketMs = Math.max(
    METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS,
    // A closed interval can intersect one more aligned bucket than its
    // duration alone implies. Reserve one response slot for that partial
    // boundary bucket so neither end of a 365-day range is truncated.
    Math.ceil((balanceWindowDays * 24) / (QUOTA_PROJECTION_MAX_BALANCE_SAMPLES - 1)) * METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS
  );
  const accountFingerprint = await meterQuotaAccountFingerprint(readMeteredAccountCredentials()).catch(() => null);
  const [snapshot, rollups, sourceBalanceHistory] = await Promise.all([
    getConfiguredMeteredQuotaSnapshot({ kv }).catch(() => null),
    kv ? listPaidFallbackUsageRollups(kv, { sinceMs: nowMs - windowMs, nowMs }).catch(() => null) : Promise.resolve(null),
    kv
      ? readMeteredQuotaBalanceHistory(kv, {
          sinceMs: nowMs - balanceWindowMs,
          nowMs,
          accountFingerprint,
        }).catch(() => null)
      : Promise.resolve(null),
  ]);
  const quota = meteredQuotaRunwayView(snapshot);
  const balanceHistory =
    sourceBalanceHistory === null
      ? null
      : resampleMeteredQuotaBalanceHistory(sourceBalanceHistory, balanceHistoryBucketMs, QUOTA_PROJECTION_MAX_BALANCE_SAMPLES);
  const usage = summarizePaidFallbackUsage(groupPaidFallbackUsageRollups(rollups ?? []), nowMs);
  const models = usage.map((entry) => ({
    model: entry.model,
    provider: entry.provider,
    quota_source: entry.provider === "metered" ? "metered" : null,
    usage: entry.windows.filter((window) => window.window_days === windowDays),
    estimates: projectPaidFallbackRunway(entry, quota, nowMs).filter((estimate) => estimate.window_days === windowDays),
  }));
  return json(
    200,
    {
      snapshot_at_ms: nowMs,
      window_days: windowDays,
      balance_window_days: balanceWindowDays,
      retention: {
        rollup_bucket_ms: PAID_FALLBACK_USAGE_ROLLUP_BUCKET_MS,
        rollup_window_ms: windowMs,
        balance_history_source_bucket_ms: METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS,
        balance_history_bucket_ms: balanceHistoryBucketMs,
        balance_history_window_ms: balanceWindowMs,
      },
      quota,
      models,
      // A failed scan must not masquerade as zero history: operators need to
      // distinguish "nothing settled" from "history could not be read".
      rollup_scan: rollups === null ? "unavailable" : "ok",
      balance_history_scan: balanceHistory === null ? "unavailable" : "ok",
      balance_history: balanceHistory ?? [],
    },
    { "Cache-Control": "no-store" }
  );
};

/**
 * Admin-triggered one-time backfill of settled V3 rows into usage rollups,
 * including the anchored raw-row TTL for rows that predate it. Run with a
 * `limit` and repeat until `truncated` is false; the run is idempotent and
 * resumable.
 */
export const handleAdminProvidersQuotaProjectionBackfill = async (
  request: Request = new Request("https://ai.ubq.fi/admin/providers/quota-projection/backfill")
): Promise<Response> => {
  const kv = await getKv();
  if (!kv) return openaiError(503, "KV is unavailable", "server_error");
  const limitRaw = Number.parseInt(new URL(request.url).searchParams.get("limit") ?? "", 10);
  const limit = Number.isInteger(limitRaw) && limitRaw > 0 ? Math.min(limitRaw, 10_000) : 5_000;
  try {
    const requests = await backfillPaidFallbackUsageRollups(kv, { limit });
    const windows = await backfillPaidFallbackWindowTtls(kv, { limit });
    return json(200, {
      requests,
      windows,
      completed: !requests.truncated && !windows.truncated,
    });
  } catch (error) {
    return openaiError(500, error instanceof Error ? error.message : "Paid fallback rollup backfill failed", "server_error");
  }
};

export const handleAdminCodexResetSettings = async (request: Request): Promise<Response> => {
  const kv = await getKv();
  if (!kv) return openaiError(503, "Settings storage unavailable", "server_error");
  const accounts = await getCodexCapacityAccounts();
  const identities = await Promise.all(
    accounts.map(async (account) => ({
      slot: account.slot,
      account_id_hash: await sha256Hex(account.account_id),
      account_cohort_id: await sha256Hex(`uos-prompt-cache-account-cohort-v1\u0000${account.account_id}`),
    }))
  );
  if (request.method === "PATCH") {
    const raw: unknown = await request.json().catch(() => null);
    if (!isRecord(raw) || typeof raw.account_id_hash !== "string" || typeof raw.enabled !== "boolean") {
      return openaiError(400, "account_id_hash and boolean enabled are required", "invalid_request_error");
    }
    if (!identities.some((account) => account.account_id_hash === raw.account_id_hash)) {
      return openaiError(409, "Subscription changed. Reload Providers.", "invalid_request_error");
    }
    await kv.set(codexResetUsageKey(raw.account_id_hash), { enabled: raw.enabled });
    return json(200, { account_id_hash: raw.account_id_hash, enabled: raw.enabled }, { "Cache-Control": "no-store" });
  }
  const data = await Promise.all(
    identities.map(async (account, index) => {
      const enabled = (await readCodexResetUsage(kv, account.account_id_hash)).allowed;
      let availableCount: number | null = null;
      try {
        const credentials = accounts.at(index);
        if (!credentials) throw new Error("codex reset settings account disappeared");
        availableCount = await readCodexResetAvailableCount(
          {
            codexBaseUrl: config.codexBaseUrl,
            accountId: credentials.account_id,
            accessToken: credentials.access_token,
            userAgent: "codex_cli_rs/0.100.0 (ai.ubq.fi)",
          },
          AbortSignal.any([request.signal, AbortSignal.timeout(5000)])
        );
      } catch {
        /* An unavailable count must not appear as zero or block the switch. */
      }
      return { ...account, enabled, available_count: availableCount };
    })
  );
  return json(200, { data }, { "Cache-Control": "no-store" });
};
