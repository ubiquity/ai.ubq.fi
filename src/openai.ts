import {
  buildCodexRequest,
  CodexError,
  fetchCodexModels,
  fetchCodexResponses,
  loadCodexModelsSnapshot,
} from "./codex.ts";
import {
  DEFAULT_MODEL,
  DEFAULT_MODEL_KEY,
  DEFAULT_REASONING_EFFORT,
  DEFAULT_REASONING_EFFORT_KEY,
  normalizeReasoningEffort,
  REASONING_EFFORTS,
  type ReasoningEffort,
} from "./defaults.ts";
import { recordApiKeyUsage } from "./analytics.ts";
import { recordKernelOrgUsage, recordKernelUsage } from "./kernel_usage.ts";
import { json, openaiError } from "./http.ts";
import { kvPromise } from "./kv.ts";
import { readJsonBody } from "./request.ts";
import { getString, isRecord, sha256Hex } from "./utils.ts";
import type {
  ChatCompletionRequest,
  MessageContentItem,
  ResponseInputItem,
  ResponseMessageItem,
  ResponsesRequest,
} from "./types.ts";

const getDefaultModel = async (): Promise<string> => {
  const kv = await kvPromise;
  if (!kv) return DEFAULT_MODEL;
  const entry = await kv.get<string>(DEFAULT_MODEL_KEY);
  const model = typeof entry.value === "string" ? entry.value.trim() : "";
  return model || DEFAULT_MODEL;
};

const getDefaultReasoningEffort = async (): Promise<ReasoningEffort> => {
  const kv = await kvPromise;
  if (!kv) return DEFAULT_REASONING_EFFORT;
  const entry = await kv.get<string>(DEFAULT_REASONING_EFFORT_KEY);
  const effort = entry.value;
  if (effort && REASONING_EFFORTS.has(effort as ReasoningEffort)) return effort as ReasoningEffort;
  return DEFAULT_REASONING_EFFORT;
};

type UsageContext = Readonly<{
  keyId: string | null;
  kernelRepo: { owner: string; repo: string } | null;
  kernelOrg: { owner: string } | null;
}>;

type UsageTokens = Readonly<{
  inputTokens: number;
  outputTokens: number;
  totalTokens: number;
}>;

type UsageDelta = Readonly<{
  request_count?: number;
  stream_request_count?: number;
  non_stream_request_count?: number;
  completed_request_count?: number;
  error_request_count?: number;
  input_tokens?: number;
  output_tokens?: number;
  total_tokens?: number;
  model?: string | null;
  reasoning?: string | null;
  route?: string | null;
  seen_at_ms?: number;
}>;

const recordUsageDelta = async (context: UsageContext | undefined, delta: UsageDelta): Promise<void> => {
  if (!context) return;
  const tasks: Promise<void>[] = [];
  if (context.keyId) tasks.push(recordApiKeyUsage(context.keyId, delta));
  if (context.kernelRepo) tasks.push(recordKernelUsage(context.kernelRepo.owner, context.kernelRepo.repo, delta));
  if (context.kernelOrg) tasks.push(recordKernelOrgUsage(context.kernelOrg.owner, delta));
  if (!tasks.length) return;
  if (tasks.length === 1) {
    await tasks[0];
    return;
  }
  await Promise.all(tasks);
};

const normalizeTokenCount = (value: unknown): number | null => {
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  const count = Math.trunc(value);
  if (count < 0) return null;
  return count;
};

const extractUsageTokens = (value: unknown): UsageTokens | null => {
  if (!isRecord(value)) return null;
  const inputTokens = normalizeTokenCount(value.input_tokens);
  const outputTokens = normalizeTokenCount(value.output_tokens);
  const totalTokens = normalizeTokenCount(value.total_tokens);
  if (inputTokens === null || outputTokens === null || totalTokens === null) return null;
  return { inputTokens, outputTokens, totalTokens };
};

const recordRequestUsage = async (
  context: UsageContext | undefined,
  details: { model: string; route: string; stream: boolean; reasoning: string | null },
): Promise<void> => {
  await recordUsageDelta(context, {
    request_count: 1,
    stream_request_count: details.stream ? 1 : 0,
    non_stream_request_count: details.stream ? 0 : 1,
    model: details.model,
    reasoning: details.reasoning,
    route: details.route,
    seen_at_ms: Date.now(),
  });
};

const recordCompletionUsage = async (
  context: UsageContext | undefined,
  usage: UsageTokens | null,
): Promise<void> => {
  await recordUsageDelta(context, {
    completed_request_count: 1,
    input_tokens: usage?.inputTokens,
    output_tokens: usage?.outputTokens,
    total_tokens: usage?.totalTokens,
  });
};

const recordErrorUsage = async (context: UsageContext | undefined): Promise<void> => {
  await recordUsageDelta(context, { error_request_count: 1 });
};

const formatErrorSnippet = (error: unknown, maxLen = 280): string => {
  const raw = error instanceof Error ? error.message : String(error);
  const trimmed = raw.trim();
  if (!trimmed) return "";
  if (trimmed.length <= maxLen) return trimmed;
  return `${trimmed.slice(0, maxLen)}...`;
};

const toCodexErrorResponse = (error: unknown): Response => {
  if (error instanceof CodexError) {
    return openaiError(error.status, error.message, error.code);
  }
  const detail = formatErrorSnippet(error);
  const message = detail ? `Codex upstream request failed: ${detail}` : "Codex upstream request failed.";
  return openaiError(502, message, "codex_upstream_unreachable");
};

const looksLikeReasoningModel = (model: string): boolean => {
  const trimmed = model.trim().toLowerCase();
  return trimmed.startsWith("gpt-5") || trimmed.startsWith("o");
};

const resolveDefaultReasoningLabel = (model: string, defaultEffort: ReasoningEffort): ReasoningEffort => {
  return looksLikeReasoningModel(model) ? defaultEffort : "none";
};

const resolveReasoningLabelFromEffort = (
  effort: ReasoningEffort | null | undefined,
  defaultLabel: ReasoningEffort,
): ReasoningEffort => {
  if (effort === undefined) return defaultLabel;
  if (effort === null) return "none";
  return effort;
};

const resolveReasoningLabelFromParam = (
  reasoning: Record<string, unknown> | null | undefined,
  defaultLabel: ReasoningEffort,
): ReasoningEffort => {
  if (reasoning === undefined) return defaultLabel;
  if (reasoning === null) return "none";
  if (!isRecord(reasoning)) return defaultLabel;
  if ("effort" in reasoning) {
    if (reasoning.effort === null) return "none";
    const effort = normalizeReasoningEffort(reasoning.effort);
    if (effort) return effort;
  }
  return defaultLabel;
};

const UOS_WARNING_HEADER = "x-uos-warning";
const TEMPERATURE_IGNORED_WARNING = "temperature_ignored";
const MAX_OUTPUT_TOKENS_IGNORED_WARNING = "max_output_tokens_ignored";

const WARNING_KEY_MAP = new Map<string, string>([
  ["temperature", TEMPERATURE_IGNORED_WARNING],
  ["max_tokens", MAX_OUTPUT_TOKENS_IGNORED_WARNING],
  ["max_completion_tokens", MAX_OUTPUT_TOKENS_IGNORED_WARNING],
  ["max_output_tokens", MAX_OUTPUT_TOKENS_IGNORED_WARNING],
]);

const buildIgnoredWarnings = (record: Record<string, unknown>, usedKeys: ReadonlySet<string>): string[] => {
  const warnings = new Set<string>();
  for (const key of Object.keys(record)) {
    if (usedKeys.has(key)) continue;
    const mapped = WARNING_KEY_MAP.get(key) ?? `${key}_ignored`;
    warnings.add(mapped);
  }
  return Array.from(warnings);
};

type PassthroughToolSchemaKey =
  | "tools"
  | "tool_choice"
  | "parallel_tool_calls"
  | "prompt_cache_key"
  | "text"
  | "include";

const normalizeCodexToolChoice = (value: unknown): unknown => {
  if (!isRecord(value)) return value;
  if (getString(value.type) !== "function") return value;

  const topLevelName = getString(value.name);
  const fn = isRecord(value.function) ? value.function : null;

  // If no function field and no valid top-level name, nothing to normalize
  if (!fn && !topLevelName) return value;

  // Determine the name to use: prefer non-empty top-level name, fallback to function.name
  const functionName = fn ? getString(fn.name) : null;
  const name = (topLevelName?.trim() || "") ? topLevelName : functionName;
  if (!name?.trim()) return value;

  const normalized: Record<string, unknown> = { ...value, name };
  delete normalized.function;
  return normalized;
};

const normalizeCodexTools = (value: unknown): unknown => {
  if (!Array.isArray(value)) return value;
  return value.map((tool) => {
    if (!isRecord(tool)) return tool;
    if (getString(tool.type) !== "function") return tool;
    const nestedFunction = isRecord(tool.function) ? tool.function : null;
    if (!nestedFunction) return tool;

    const nestedName = getString(nestedFunction.name);
    if (!nestedName?.trim()) return tool;

    const normalized: Record<string, unknown> = { ...tool };

    // Prefer existing non-empty top-level name; otherwise use nested function name.
    const existingName = getString(normalized.name);
    if (!existingName?.trim()) normalized.name = nestedName;

    if (!("description" in normalized) && nestedFunction.description !== undefined) {
      normalized.description = nestedFunction.description;
    }
    if (!("parameters" in normalized)) {
      const nestedParameters = nestedFunction.parameters;
      if (nestedParameters !== undefined) normalized.parameters = nestedParameters;
    }
    delete normalized.function;
    return normalized;
  });
};

const normalizePassthroughForCodex = (key: PassthroughToolSchemaKey, value: unknown): unknown => {
  if (key === "tools") return normalizeCodexTools(value);
  if (key === "tool_choice") return normalizeCodexToolChoice(value);
  return value;
};

const applyPassthroughToCodexRequest = (
  codexBody: Record<string, unknown>,
  rawRecord: Record<string, unknown>,
  keys: readonly PassthroughToolSchemaKey[],
): void => {
  for (const key of keys) {
    if (Object.prototype.hasOwnProperty.call(rawRecord, key)) {
      codexBody[key] = normalizePassthroughForCodex(key, rawRecord[key]);
    }
  }
};

const withUosWarning = (response: Response, warnings: string[]): Response => {
  if (!warnings.length) return response;
  const headers = new Headers(response.headers);
  headers.set(UOS_WARNING_HEADER, warnings.join(", "));
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
};
const parseReasoningEffortField = (
  value: unknown,
  fieldName: string,
): { ok: true; value: ReasoningEffort | null | undefined } | { ok: false; message: string } => {
  if (value === undefined) return { ok: true, value: undefined };
  if (value === null) return { ok: true, value: null };
  if (typeof value !== "string") {
    return { ok: false, message: `${fieldName} must be a string or null` };
  }
  const normalized = value.trim().toLowerCase();
  if (!normalized) return { ok: false, message: `${fieldName} must be a non-empty string or null` };
  if (!REASONING_EFFORTS.has(normalized as ReasoningEffort)) {
    return { ok: false, message: `${fieldName} must be one of: none, minimal, low, medium, high, xhigh` };
  }
  return { ok: true, value: normalized as ReasoningEffort };
};

const parseReasoningParam = (
  value: unknown,
): { ok: true; value: Record<string, unknown> | null | undefined } | { ok: false; message: string } => {
  if (value === undefined) return { ok: true, value: undefined };
  if (value === null) return { ok: true, value: null };
  if (!isRecord(value)) return { ok: false, message: "reasoning must be an object or null" };
  if ("effort" in value) {
    const effort = parseReasoningEffortField(value.effort, "reasoning.effort");
    if (!effort.ok) return effort;
  }
  if ("summary" in value) {
    const summary = value.summary;
    if (summary !== undefined && summary !== null && typeof summary !== "string") {
      return { ok: false, message: "reasoning.summary must be a string or null" };
    }
  }
  if ("generate_summary" in value) {
    const generateSummary = value.generate_summary;
    if (generateSummary !== undefined && generateSummary !== null && typeof generateSummary !== "string") {
      return { ok: false, message: "reasoning.generate_summary must be a string or null" };
    }
  }

  return { ok: true, value };
};

const CHAT_COMPLETIONS_ALLOWED_KEYS = new Set([
  "messages",
  "model",
  "audio",
  "frequency_penalty",
  "function_call",
  "functions",
  "logit_bias",
  "logprobs",
  "max_completion_tokens",
  "max_tokens",
  "metadata",
  "modalities",
  "n",
  "parallel_tool_calls",
  "prediction",
  "presence_penalty",
  "prompt_cache_key",
  "prompt_cache_retention",
  "reasoning_effort",
  "response_format",
  "safety_identifier",
  "seed",
  "service_tier",
  "stop",
  "store",
  "stream",
  "stream_options",
  "temperature",
  "tool_choice",
  "tools",
  "top_logprobs",
  "top_p",
  "user",
  "verbosity",
  "web_search_options",
]);

const RESPONSES_ALLOWED_KEYS = new Set([
  "background",
  "conversation",
  "include",
  "input",
  "instructions",
  "max_output_tokens",
  "max_tool_calls",
  "metadata",
  "model",
  "parallel_tool_calls",
  "previous_response_id",
  "prompt",
  "prompt_cache_key",
  "prompt_cache_retention",
  "reasoning",
  "safety_identifier",
  "service_tier",
  "store",
  "stream",
  "stream_options",
  "temperature",
  "text",
  "tool_choice",
  "tools",
  "top_logprobs",
  "top_p",
  "truncation",
  "user",
]);

const EMBEDDINGS_ALLOWED_KEYS = new Set([
  "model",
  "input",
  "encoding_format",
  "dimensions",
  "user",
]);

const findUnknownKey = (record: Record<string, unknown>, allowed: ReadonlySet<string>): string | null => {
  for (const key of Object.keys(record)) {
    if (!allowed.has(key)) return key;
  }
  return null;
};

type EmbeddingsEncodingFormat = "float" | "base64";

type VoyageRateLimitState = Readonly<{
  window_start_ms: number;
  requests: number;
  tokens: number;
}>;

const EMBEDDINGS_MAX_INPUTS_PER_REQUEST = 128;
const EMBEDDINGS_MAX_CHARS_PER_INPUT = 20_000;
const EMBEDDINGS_MAX_TOTAL_CHARS = 100_000;
const EMBEDDINGS_TIMEOUT_MS = 20_000;
// KV cache is best-effort and quota-driven: we cache embeddings until KV rejects
// writes (storage/quota), then evict the oldest entries (FIFO index) and retry.
// We do not track "last read" to keep writes minimal.
const EMBEDDINGS_CACHE_EVICT_BATCH = 512;
const EMBEDDINGS_CACHE_EVICT_MAX_BATCH = 8192;
const EMBEDDINGS_CACHE_QUOTA_MAX_RETRIES = 4;
const EMBEDDINGS_JOB_TTL_MS = 24 * 60 * 60_000;
const EMBEDDINGS_JOB_LOCK_MS = 30_000;

const VOYAGE_EMBEDDINGS_URL = "https://api.voyageai.com/v1/embeddings";
const VOYAGE_INPUT_TYPE = "document";
// Voyage free-tier throttles are tiny; we enforce conservative defaults to avoid 429s.
const VOYAGE_RATE_LIMIT_RPM = 3;
const VOYAGE_RATE_LIMIT_TPM = 10_000;
const VOYAGE_RATE_LIMIT_KEY: Deno.KvKey = ["embeddings", "v1", "rate", "voyage"];
const VOYAGE_API_KEY_KV_KEY: Deno.KvKey = ["uos_ai", "voyage_api_key"];

type EmbeddingsJobStatus = "queued" | "running" | "succeeded" | "failed";

type EmbeddingsJobRecord = Readonly<{
  id: string;
  status: EmbeddingsJobStatus;
  created_at_ms: number;
  updated_at_ms: number;
  model: string;
  cache_model_key: string;
  upstream: "voyage";
  upstream_model: string;
  input_hashes: string[];
  input_count: number;
  total_chars: number;
  usage_total_tokens: number;
  retry_after_seconds: number | null;
  locked_until_ms: number | null;
  error: { message: string; type: string; code?: string } | null;
}>;

type EmbeddingsJobInputRecord = Readonly<{
  v: 1;
  iv_b64: string;
  data_b64: string;
  created_at_ms: number;
}>;

const embeddingsJobKey = (tokenHash: string, id: string): Deno.KvKey => ["embeddings", "jobs", "v1", tokenHash, id];
const embeddingsJobInputKey = (tokenHash: string, jobId: string, hash: string): Deno.KvKey => [
  "embeddings",
  "jobs",
  "v1",
  "input",
  tokenHash,
  jobId,
  hash,
];

const embeddingsCacheIndexPrefix = (
  cacheModelKey: string,
): Deno.KvKey => ["embeddings", "v1", "cache_index", cacheModelKey];
const embeddingsCacheIndexKey = (
  cacheModelKey: string,
  createdAtMs: number,
  hash: string,
): Deno.KvKey => ["embeddings", "v1", "cache_index", cacheModelKey, createdAtMs, hash];
const embeddingsCacheIndexByHashKey = (cacheModelKey: string, hash: string): Deno.KvKey => [
  "embeddings",
  "v1",
  "cache_index_by_hash",
  cacheModelKey,
  hash,
];

const normalizeEmbeddingsCacheTimestampMs = (value: unknown): number | null => {
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  const ts = Math.trunc(value);
  if (ts < 0) return null;
  return ts;
};

type EmbeddingsCacheEvictResult = Readonly<{
  evicted_embeddings: number;
  deleted_stale_index_keys: number;
}>;

const isEmbeddingsCacheQuotaError = (error: unknown): boolean => {
  const name = typeof (error as { name?: unknown })?.name === "string" ? String((error as { name: string }).name) : "";
  const message = error instanceof Error ? error.message : typeof error === "string" ? error : "";
  const combined = `${name} ${message}`.toLowerCase();
  if (!combined) return false;
  return (
    combined.includes("quota") ||
    (combined.includes("insufficient") && combined.includes("storage")) ||
    (combined.includes("insufficient") && combined.includes("space")) ||
    combined.includes("no space") ||
    combined.includes("storage limit") ||
    (combined.includes("storage") && combined.includes("exceeded"))
  );
};

const writeEmbeddingsCacheEntry = async (
  kv: Deno.Kv,
  cacheModelKey: string,
  hash: string,
  embedding: number[],
  createdAtMs: number,
): Promise<{ isNew: boolean }> => {
  const byHashKey = embeddingsCacheIndexByHashKey(cacheModelKey, hash);
  const cacheKey: Deno.KvKey = ["embeddings", "v1", cacheModelKey, hash];

  // Concurrency-safe: if multiple requests try to cache the same hash, only one
  // will win the "create index" CAS; the others will reuse the winner's index.
  for (let attempt = 0; attempt < 5; attempt += 1) {
    const entry = await kv.get<number>(byHashKey);
    const existingCreatedAtMs = normalizeEmbeddingsCacheTimestampMs(entry.value);
    if (existingCreatedAtMs !== null) {
      const indexKey = embeddingsCacheIndexKey(cacheModelKey, existingCreatedAtMs, hash);
      const updated = await kv.atomic()
        .check(entry)
        .set(cacheKey, { embedding, created_at: new Date(existingCreatedAtMs).toISOString() })
        .set(indexKey, 1)
        .commit();
      if (updated.ok) return { isNew: false };
      continue;
    }

    const createdAtIso = new Date(createdAtMs).toISOString();
    const indexKey = embeddingsCacheIndexKey(cacheModelKey, createdAtMs, hash);
    const created = await kv.atomic()
      .check(entry)
      .set(cacheKey, { embedding, created_at: createdAtIso })
      .set(indexKey, 1)
      .set(byHashKey, createdAtMs)
      .commit();
    if (created.ok) return { isNew: true };
    // CAS failed: `byHashKey` was updated/created in between, or was evicted and
    // recreated concurrently. Retry to reuse the now-canonical pointer.
  }
  return { isNew: false };
};

const writeEmbeddingsCacheEntryBestEffort = async (
  kv: Deno.Kv,
  cacheModelKey: string,
  hash: string,
  embedding: number[],
  createdAtMs: number,
  deadlineMs: number,
): Promise<{ isNew: boolean }> => {
  let evictBatch = EMBEDDINGS_CACHE_EVICT_BATCH;
  // Attempts = 1 (initial write) + max retries.
  for (let attempt = 0; attempt <= EMBEDDINGS_CACHE_QUOTA_MAX_RETRIES; attempt += 1) {
    if (Date.now() >= deadlineMs) return { isNew: false };
    try {
      return await writeEmbeddingsCacheEntry(kv, cacheModelKey, hash, embedding, createdAtMs);
    } catch (error) {
      if (!isEmbeddingsCacheQuotaError(error)) {
        console.warn("[ai.ubq.fi] embeddings_cache write failed:", error);
        return { isNew: false };
      }

      // KV rejected the write (likely storage quota). Evict oldest entries and retry.
      try {
        const evicted = await evictOldestEmbeddingsCacheEntries(kv, cacheModelKey, evictBatch);
        console.warn(
          `[ai.ubq.fi] embeddings_cache quota eviction model=${cacheModelKey} evicted=${evicted.evicted_embeddings} stale_index_deleted=${evicted.deleted_stale_index_keys} batch=${evictBatch}`,
        );
        if (evicted.evicted_embeddings <= 0 && evicted.deleted_stale_index_keys <= 0) return { isNew: false };
      } catch (evictError) {
        console.warn("[ai.ubq.fi] embeddings_cache quota eviction failed:", evictError);
        return { isNew: false };
      }

      evictBatch = Math.min(EMBEDDINGS_CACHE_EVICT_MAX_BATCH, evictBatch * 2);
    }
  }
  return { isNew: false };
};

const evictOldestEmbeddingsCacheEntries = async (
  kv: Deno.Kv,
  cacheModelKey: string,
  count: number,
): Promise<EmbeddingsCacheEvictResult> => {
  const prefix = embeddingsCacheIndexPrefix(cacheModelKey);
  const keys: Array<{ indexKey: Deno.KvKey; createdAtMs: number; hash: string }> = [];
  for await (const entry of kv.list({ prefix }, { limit: count })) {
    const key = entry.key;
    const hash = key.at(-1);
    const createdAtMs = key.at(-2);
    if (typeof hash !== "string" || !hash) continue;
    if (typeof createdAtMs !== "number" || !Number.isFinite(createdAtMs)) continue;
    keys.push({ indexKey: key, createdAtMs: Math.trunc(createdAtMs), hash });
  }
  if (!keys.length) return { evicted_embeddings: 0, deleted_stale_index_keys: 0 };

  const byHashKeys = keys.map((item) => embeddingsCacheIndexByHashKey(cacheModelKey, item.hash));
  const byHashEntries = await Promise.all(byHashKeys.map((key) => kv.get<number>(key)));

  let evictedEmbeddings = 0;
  let deletedStaleIndexKeys = 0;
  for (let i = 0; i < keys.length; i += 1) {
    const { indexKey, createdAtMs, hash } = keys[i]!;
    const pointerEntry = byHashEntries[i]!;
    const pointer = normalizeEmbeddingsCacheTimestampMs(pointerEntry.value);
    const cacheKey: Deno.KvKey = ["embeddings", "v1", cacheModelKey, hash];

    if (pointer !== null && pointer !== createdAtMs) {
      // Stale duplicate index key for this hash; delete the index entry only.
      const deleted = await kv.atomic().check(pointerEntry).delete(indexKey).commit();
      if (deleted.ok) deletedStaleIndexKeys += 1;
      continue;
    }

    if (pointer === null) {
      // Missing pointer (legacy / partial state): only delete the embedding value
      // if it still matches the index timestamp to avoid deleting a newer cache
      // entry that happens to share the same hash.
      const valueEntry = await kv.get<{ created_at?: unknown }>(cacheKey);
      const value = valueEntry.value;
      const createdAtIso = isRecord(value) && typeof value.created_at === "string" ? value.created_at : null;
      const expectedIso = new Date(createdAtMs).toISOString();
      if (createdAtIso !== expectedIso) {
        const deleted = await kv.atomic().check(pointerEntry).delete(indexKey).commit();
        if (deleted.ok) deletedStaleIndexKeys += 1;
        continue;
      }

      const commit = await kv.atomic()
        .check(pointerEntry)
        .delete(indexKey)
        .delete(cacheKey)
        .commit();
      if (commit.ok) evictedEmbeddings += 1;
      continue;
    }

    // Canonical pointer match: evict embedding + index + pointer as an atomic unit.
    const commit = await kv.atomic()
      .check(pointerEntry)
      .delete(indexKey)
      .delete(cacheKey)
      .delete(embeddingsCacheIndexByHashKey(cacheModelKey, hash))
      .commit();
    if (commit.ok) evictedEmbeddings += 1;
  }
  return { evicted_embeddings: evictedEmbeddings, deleted_stale_index_keys: deletedStaleIndexKeys };
};

const resolveEmbeddingsJobTokenSeed = (
  jobId: string,
  authToken: string | null,
  usageContext?: UsageContext,
): string => {
  // Prefer stable identities so queued jobs remain resolvable even if bearer tokens refresh/rotate.
  if (usageContext?.keyId) return `uos_api_key_id:${usageContext.keyId}`;
  if (usageContext?.kernelRepo) {
    return `uos_kernel_repo:${usageContext.kernelRepo.owner}/${usageContext.kernelRepo.repo}`;
  }
  if (authToken) return authToken;
  return jobId;
};

const TOKEN_ESTIMATOR = new TextEncoder();

const getEnv = (key: string): string | undefined => {
  try {
    return Deno.env.get(key);
  } catch {
    return undefined;
  }
};

const sleep = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms));

const estimateTokens = (text: string): number => {
  if (!text) return 0;
  const bytes = TOKEN_ESTIMATOR.encode(text).byteLength;
  return Math.ceil(bytes / 4);
};

const estimateTokenCount = (texts: string[]): number => texts.reduce((sum, text) => sum + estimateTokens(text), 0);

const chunkByTokenBudget = (
  items: ReadonlyArray<{ hash: string; text: string }>,
  maxItems: number,
  maxTokens: number,
): Array<Array<{ hash: string; text: string }>> => {
  const out: Array<Array<{ hash: string; text: string }>> = [];
  const itemLimit = Math.max(1, Math.trunc(maxItems));
  const tokenLimit = Math.max(1, Math.trunc(maxTokens));

  let current: Array<{ hash: string; text: string }> = [];
  let currentTokens = 0;

  for (const item of items) {
    const tokens = estimateTokens(item.text);
    const nextTokens = currentTokens + tokens;
    const hitsItemLimit = current.length >= itemLimit;
    const hitsTokenLimit = nextTokens > tokenLimit && current.length > 0;
    if (hitsItemLimit || hitsTokenLimit) {
      out.push(current);
      current = [];
      currentTokens = 0;
    }
    current.push(item);
    currentTokens += tokens;
  }
  if (current.length) out.push(current);
  return out;
};

const normalizeVoyageRateLimitState = (value: unknown): VoyageRateLimitState | null => {
  if (!isRecord(value)) return null;
  const windowStart = typeof value.window_start_ms === "number" && Number.isFinite(value.window_start_ms)
    ? Math.trunc(value.window_start_ms)
    : null;
  const requests = typeof value.requests === "number" && Number.isFinite(value.requests)
    ? Math.trunc(value.requests)
    : null;
  const tokens = typeof value.tokens === "number" && Number.isFinite(value.tokens) ? Math.trunc(value.tokens) : null;
  if (windowStart === null || requests === null || tokens === null) return null;
  if (windowStart < 0 || requests < 0 || tokens < 0) return null;
  return { window_start_ms: windowStart, requests, tokens };
};

const tryReserveVoyageBudget = async (
  kv: Deno.Kv,
  tokens: number,
): Promise<{ ok: true } | { ok: false; wait_ms: number }> => {
  const windowMs = 60_000;
  const now = Date.now();
  const entry = await kv.get<VoyageRateLimitState>(VOYAGE_RATE_LIMIT_KEY);
  const current = normalizeVoyageRateLimitState(entry.value);
  const state = !current || now - current.window_start_ms >= windowMs
    ? { window_start_ms: now, requests: 0, tokens: 0 }
    : current;

  const wouldExceedRequests = VOYAGE_RATE_LIMIT_RPM > 0 && state.requests + 1 > VOYAGE_RATE_LIMIT_RPM;
  const wouldExceedTokens = VOYAGE_RATE_LIMIT_TPM > 0 && state.tokens + tokens > VOYAGE_RATE_LIMIT_TPM;
  if (wouldExceedRequests || wouldExceedTokens) {
    const waitMs = Math.max(0, windowMs - (now - state.window_start_ms));
    return { ok: false, wait_ms: waitMs };
  }

  const next: VoyageRateLimitState = {
    window_start_ms: state.window_start_ms,
    requests: state.requests + 1,
    tokens: state.tokens + tokens,
  };
  const commit = await kv.atomic().check(entry).set(VOYAGE_RATE_LIMIT_KEY, next).commit();
  if (commit.ok) return { ok: true };
  return { ok: false, wait_ms: 0 };
};

const applyVoyageRateLimit = async (
  kv: Deno.Kv,
  tokens: number,
  deadlineMs: number,
): Promise<{ ok: true } | { ok: false; wait_ms: number }> => {
  // Best-effort concurrency-safe rate limiting using KV. If we can't reserve
  // within the request deadline, we fail with 429 and let clients retry.
  for (;;) {
    const now = Date.now();
    if (now >= deadlineMs) return { ok: false, wait_ms: 0 };
    let reserved: { ok: true } | { ok: false; wait_ms: number } = { ok: false, wait_ms: 0 };
    for (let attempt = 0; attempt < 6; attempt += 1) {
      reserved = await tryReserveVoyageBudget(kv, tokens);
      if (reserved.ok) return reserved;
      if (reserved.wait_ms > 0) break;
      await sleep(5 + attempt * 5);
    }
    if (reserved.ok) return reserved;
    const waitMs = reserved.wait_ms;
    if (waitMs <= 0) {
      // CAS contention without a concrete rate-limit wait; avoid tight spinning.
      const now2 = Date.now();
      const sleepMs = Math.min(25, Math.max(0, deadlineMs - now2));
      if (sleepMs > 0) await sleep(sleepMs);
      continue;
    }
    if (now + waitMs > deadlineMs) return { ok: false, wait_ms: waitMs };
    await sleep(waitMs);
  }
};

const resolveEmbeddingsModel = (raw: string): { upstream: "voyage"; model: string } | null => {
  const trimmed = raw.trim();
  if (!trimmed) return null;
  const normalized = trimmed.toLowerCase();
  if (normalized === "text-embedding-3-small" || normalized === "text-embedding-3-large") {
    // OpenAI-compatible model names; backed by Voyage (dimensionality may differ from OpenAI).
    return { upstream: "voyage", model: "voyage-4-large" };
  }
  if (normalized.startsWith("voyage-")) {
    return { upstream: "voyage", model: normalized };
  }
  return null;
};

const parseEmbeddingsEncodingFormat = (
  value: unknown,
): { ok: true; value: EmbeddingsEncodingFormat } | { ok: false; message: string } => {
  if (value === undefined) return { ok: true, value: "float" };
  if (typeof value !== "string") return { ok: false, message: "encoding_format must be a string" };
  const normalized = value.trim().toLowerCase();
  if (normalized === "float" || normalized === "base64") return { ok: true, value: normalized };
  return { ok: false, message: 'encoding_format must be one of: "float", "base64"' };
};

const floatEmbeddingToBase64 = (embedding: number[]): string => {
  const buffer = new ArrayBuffer(embedding.length * 4);
  const view = new DataView(buffer);
  for (let i = 0; i < embedding.length; i += 1) {
    view.setFloat32(i * 4, embedding[i], true);
  }
  const bytes = new Uint8Array(buffer);
  const chunkSize = 0x8000; // Avoid large variadic calls and quadratic string concatenation.
  const chunks: string[] = [];
  for (let i = 0; i < bytes.length; i += chunkSize) {
    const chunk = bytes.subarray(i, i + chunkSize);
    chunks.push(String.fromCharCode(...chunk));
  }
  return btoa(chunks.join(""));
};

const bytesToBase64 = (bytes: Uint8Array): string => {
  const chunkSize = 0x8000;
  const chunks: string[] = [];
  for (let i = 0; i < bytes.length; i += chunkSize) {
    const chunk = bytes.subarray(i, i + chunkSize);
    chunks.push(String.fromCharCode(...chunk));
  }
  return btoa(chunks.join(""));
};

const base64ToBytes = (value: string): Uint8Array<ArrayBuffer> | null => {
  try {
    const raw = atob(value);
    const bytes = new Uint8Array(new ArrayBuffer(raw.length));
    for (let i = 0; i < raw.length; i += 1) bytes[i] = raw.charCodeAt(i);
    return bytes;
  } catch {
    return null;
  }
};

const normalizeEmbeddingsJobInputRecord = (value: unknown): EmbeddingsJobInputRecord | null => {
  if (!isRecord(value)) return null;
  const v = value.v;
  if (v !== 1) return null;
  const iv = getString(value.iv_b64);
  const data = getString(value.data_b64);
  if (!iv || !data) return null;
  const createdAt = typeof value.created_at_ms === "number" && Number.isFinite(value.created_at_ms)
    ? Math.trunc(value.created_at_ms)
    : null;
  if (createdAt === null || createdAt < 0) return null;
  return { v: 1, iv_b64: iv, data_b64: data, created_at_ms: createdAt };
};

const importEmbeddingsJobKey = async (tokenSeed: string): Promise<CryptoKey> => {
  const material = new TextEncoder().encode(`uos_embeddings_job_v1:${tokenSeed}`);
  const digest = await crypto.subtle.digest("SHA-256", material);
  return await crypto.subtle.importKey("raw", digest, { name: "AES-GCM" }, false, ["encrypt", "decrypt"]);
};

const encryptEmbeddingsJobInput = async (tokenSeed: string, text: string): Promise<EmbeddingsJobInputRecord> => {
  const key = await importEmbeddingsJobKey(tokenSeed);
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const bytes = new TextEncoder().encode(text);
  const encrypted = await crypto.subtle.encrypt({ name: "AES-GCM", iv }, key, bytes);
  return {
    v: 1,
    iv_b64: bytesToBase64(iv),
    data_b64: bytesToBase64(new Uint8Array(encrypted)),
    created_at_ms: Date.now(),
  };
};

const decryptEmbeddingsJobInput = async (
  tokenSeed: string,
  record: EmbeddingsJobInputRecord,
): Promise<string | null> => {
  const iv = base64ToBytes(record.iv_b64);
  const data = base64ToBytes(record.data_b64);
  if (!iv || !data) return null;
  try {
    const key = await importEmbeddingsJobKey(tokenSeed);
    const decrypted = await crypto.subtle.decrypt({ name: "AES-GCM", iv }, key, data);
    return new TextDecoder().decode(new Uint8Array(decrypted));
  } catch {
    return null;
  }
};

const extractRetryAfterMs = (value: string | null): number | null => {
  if (!value) return null;
  const trimmed = value.trim();
  if (!trimmed) return null;
  const seconds = Number(trimmed);
  if (Number.isFinite(seconds) && seconds > 0) {
    return Math.min(60_000, Math.trunc(seconds * 1000));
  }
  const asDate = Date.parse(trimmed);
  if (!Number.isNaN(asDate)) {
    const delta = asDate - Date.now();
    if (delta > 0) return Math.min(60_000, Math.trunc(delta));
  }
  return null;
};

const readVoyageApiKey = async (kv: Deno.Kv | null): Promise<string | null> => {
  const envKey = (getEnv("VOYAGEAI_API_KEY") ?? "").trim();
  if (envKey) return envKey;
  if (!kv) return null;
  const entry = await kv.get<string>(VOYAGE_API_KEY_KV_KEY);
  const kvKey = typeof entry.value === "string" ? entry.value.trim() : "";
  return kvKey || null;
};

const fetchVoyageEmbeddings = async (params: {
  apiKey: string;
  model: string;
  inputs: string[];
  deadlineMs: number;
}): Promise<{ vectors: number[][]; totalTokens: number | null }> => {
  const controller = new AbortController();
  const now = Date.now();
  const timeoutMs = Math.max(1, Math.min(EMBEDDINGS_TIMEOUT_MS, params.deadlineMs - now));
  const timeout = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const resp = await fetch(VOYAGE_EMBEDDINGS_URL, {
      method: "POST",
      headers: {
        "Authorization": `Bearer ${params.apiKey}`,
        "Content-Type": "application/json",
      },
      body: JSON.stringify({
        model: params.model,
        input: params.inputs.length === 1 ? params.inputs[0] : params.inputs,
        input_type: VOYAGE_INPUT_TYPE,
      }),
      signal: controller.signal,
    });

    if (!resp.ok) {
      // Avoid echoing upstream bodies; they can contain provider details and may be surfaced to clients/logs.
      const err = new Error(`Voyage embeddings failed (${resp.status}).`);
      (err as { status?: number; retry_after_ms?: number }).status = resp.status;
      (err as { retry_after_ms?: number }).retry_after_ms = extractRetryAfterMs(resp.headers.get("Retry-After")) ??
        undefined;
      throw err;
    }

    const payload = await resp.json().catch(() => null) as unknown;
    if (!isRecord(payload) || !Array.isArray(payload.data)) {
      throw new Error("Voyage embeddings returned invalid JSON.");
    }

    let totalTokens: number | null = null;
    if (isRecord(payload.usage)) {
      const rawTotalTokens = payload.usage.total_tokens;
      if (typeof rawTotalTokens === "number" && Number.isFinite(rawTotalTokens)) {
        totalTokens = Math.max(0, Math.trunc(rawTotalTokens));
      }
    }

    const data = payload.data as Array<Record<string, unknown>>;
    const vectors: number[][] = [];
    for (const item of data) {
      const embedding = isRecord(item) ? item.embedding : null;
      if (!Array.isArray(embedding)) {
        throw new Error("Voyage embeddings response missing embedding vector.");
      }
      const vec: number[] = [];
      for (const v of embedding) {
        if (typeof v !== "number" || !Number.isFinite(v)) {
          throw new Error("Voyage embeddings response contained non-numeric values.");
        }
        vec.push(v);
      }
      vectors.push(vec);
    }
    return { vectors, totalTokens };
  } finally {
    clearTimeout(timeout);
  }
};

const extractMessageContentItems = (role: ResponseMessageItem["role"], content: unknown): MessageContentItem[] => {
  const isAssistant = role === "assistant";
  const textItemType: MessageContentItem["type"] = isAssistant ? "output_text" : "input_text";

  if (typeof content === "string") {
    return [{ type: textItemType, text: content }];
  }

  if (!Array.isArray(content)) {
    return [{ type: textItemType, text: "" }];
  }

  const items: MessageContentItem[] = [];
  for (const part of content) {
    if (!isRecord(part)) continue;
    const partType = getString(part.type);

    if (partType === "text" || partType === "input_text" || partType === "output_text") {
      const text = getString(part.text);
      if (text) items.push({ type: textItemType, text });
      continue;
    }

    if (partType === "image_url" || partType === "input_image") {
      if (isAssistant) continue;
      let url: string | null = null;
      if (partType === "image_url") {
        const image = isRecord(part.image_url) ? part.image_url : null;
        url = image ? getString(image.url) : null;
      } else {
        url = getString(part.image_url);
      }
      const trimmed = (url ?? "").trim();
      if (trimmed) items.push({ type: "input_image", image_url: trimmed });
      continue;
    }
  }

  if (items.length > 0) return items;
  return [{ type: textItemType, text: "" }];
};

const messageContentToText = (items: MessageContentItem[]): string =>
  items
    .filter((item) => item.type === "input_text" || item.type === "output_text")
    .map((item) => item.text)
    .filter((text) => text && text.trim())
    .join("\n");

const chatRoleToCodexRole = (role: string): ResponseMessageItem["role"] | null => {
  if (role === "system") return "developer";
  if (role === "user") return "user";
  if (role === "assistant") return "assistant";
  if (role === "developer") return "developer";
  if (role === "tool") return "developer";
  return null;
};

const normalizeModelForCodex = (model: string): string => {
  const trimmed = model.trim();
  if (!trimmed) return "gpt-5.1-codex-mini";
  if (trimmed === "gpt-5.3-chat-latest") return "gpt-5.3";
  if (trimmed === "gpt-5.2-chat-latest") return "gpt-5.2";
  if (trimmed === "gpt-5.1-chat-latest") return "gpt-5.1";
  if (trimmed === "gpt-5-chat-latest") return "gpt-5.2";
  return trimmed;
};

const normalizeModelEntry = (value: unknown): Record<string, unknown> | null => {
  if (!isRecord(value)) return null;
  const id = getString(value.id) ?? getString(value.slug) ?? getString(value.model) ?? getString(value.name);
  if (!id) return null;
  const normalized: Record<string, unknown> = { ...value, id };
  const object = getString(value.object);
  const ownedBy = getString(value.owned_by);
  normalized.object = object ?? "model";
  normalized.owned_by = ownedBy ?? "openai";
  return normalized;
};

const normalizeModelList = (payload: unknown): { object: "list"; data: Record<string, unknown>[] } | null => {
  if (!isRecord(payload)) return null;
  const data = Array.isArray(payload.data) ? payload.data : null;
  if (data) {
    const normalized = data.map(normalizeModelEntry).filter(Boolean) as Record<string, unknown>[];
    return { object: "list", data: normalized };
  }
  const models = Array.isArray(payload.models) ? payload.models : null;
  if (models) {
    const normalized = models.map(normalizeModelEntry).filter(Boolean) as Record<string, unknown>[];
    return { object: "list", data: normalized };
  }
  return null;
};

const toResponseMessageItem = (message: unknown): ResponseMessageItem | null => {
  if (!isRecord(message)) return null;
  const roleRaw = getString(message.role);
  if (!roleRaw) return null;
  const role = chatRoleToCodexRole(roleRaw);
  if (!role) return null;
  const content = extractMessageContentItems(role, message.content);
  return { type: "message", role, content };
};

const normalizeResponseContentItem = (value: unknown): MessageContentItem | null => {
  if (!isRecord(value)) return null;
  const partType = getString(value.type);
  if (!partType) return null;

  if (partType === "input_text" || partType === "text") {
    const text = getString(value.text);
    if (text === null) return null;
    return { type: "input_text", text };
  }

  if (partType === "input_image" || partType === "image_url") {
    let url: string | null = null;
    if (partType === "image_url") {
      const image = isRecord(value.image_url) ? value.image_url : null;
      url = image ? getString(image.url) : null;
    } else {
      url = getString(value.image_url);
    }
    const trimmed = (url ?? "").trim();
    if (!trimmed) return null;
    return { type: "input_image", image_url: trimmed };
  }

  return null;
};

const normalizeResponseInputItem = (value: unknown): ResponseMessageItem | null => {
  if (!isRecord(value)) return null;
  const itemType = getString(value.type);
  if (itemType && itemType !== "message") return null;
  return toResponseMessageItem(value);
};

const parseSseEvents = async function* (stream: ReadableStream<Uint8Array>): AsyncGenerator<unknown> {
  const reader = stream.getReader();
  const decoder = new TextDecoder();
  let buffer = "";
  try {
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      buffer += decoder.decode(value, { stream: true });
      const normalized = buffer.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
      const parts = normalized.split("\n\n");
      buffer = parts.pop() ?? "";
      for (const part of parts) {
        if (!part.trim()) continue;
        const lines = part.split("\n");
        const dataLines = lines.filter((line) => line.startsWith("data:"));
        const data = dataLines.map((line) => line.slice(5).trim()).join("\n");
        if (!data) continue;
        try {
          yield JSON.parse(data);
        } catch (error) {
          const message = error instanceof Error ? error.message : String(error);
          console.warn("[ai.ubq.fi] SSE parse error:", message);
          continue;
        }
      }
    }
  } finally {
    try {
      await reader.cancel();
    } catch {
      // ignore
    }
  }
};

const collectResponsesStreamUsage = async (
  stream: ReadableStream<Uint8Array>,
  usageContext?: UsageContext,
): Promise<void> => {
  if (!usageContext?.keyId && !usageContext?.kernelRepo && !usageContext?.kernelOrg) return;
  try {
    for await (const ev of parseSseEvents(stream)) {
      if (!isRecord(ev)) continue;
      if (getString(ev.type) === "response.completed" && isRecord(ev.response)) {
        const usageTokens = extractUsageTokens(ev.response.usage);
        await recordCompletionUsage(usageContext, usageTokens);
        return;
      }
    }
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to parse responses usage stream:", error);
  }
};

const streamChatCompletions = (upstream: Response, model: string, usageContext?: UsageContext): Response => {
  if (!upstream.body) {
    return openaiError(502, "Codex upstream response missing body.", "codex_upstream_missing_body");
  }

  const encoder = new TextEncoder();
  const stream = new ReadableStream<Uint8Array>({
    async start(controller) {
      let id = `chatcmpl_${crypto.randomUUID().replace(/-/g, "")}`;
      let created = Math.floor(Date.now() / 1000);
      let sentRole = false;

      try {
        for await (const ev of parseSseEvents(upstream.body!)) {
          if (!isRecord(ev)) continue;
          const type = getString(ev.type);
          if (type === "response.created" && isRecord(ev.response)) {
            const upstreamId = getString(ev.response.id);
            const createdAt = typeof ev.response.created_at === "number" ? ev.response.created_at : null;
            if (upstreamId) id = upstreamId;
            if (createdAt) created = createdAt;
            continue;
          }

          if (type === "response.output_text.delta") {
            const delta = getString(ev.delta) ?? "";
            const chunk: Record<string, unknown> = {
              id,
              object: "chat.completion.chunk",
              created,
              model,
              choices: [
                {
                  index: 0,
                  delta: sentRole ? { content: delta } : { role: "assistant", content: delta },
                  finish_reason: null,
                },
              ],
            };
            sentRole = true;
            controller.enqueue(encoder.encode(`data: ${JSON.stringify(chunk)}\n\n`));
            continue;
          }

          if (type === "response.completed") {
            const usageTokens = isRecord(ev.response) ? extractUsageTokens(ev.response.usage) : null;
            await recordCompletionUsage(usageContext, usageTokens);
            const chunk: Record<string, unknown> = {
              id,
              object: "chat.completion.chunk",
              created,
              model,
              choices: [
                {
                  index: 0,
                  delta: sentRole ? {} : { role: "assistant" },
                  finish_reason: "stop",
                },
              ],
            };
            controller.enqueue(encoder.encode(`data: ${JSON.stringify(chunk)}\n\n`));
            controller.enqueue(encoder.encode("data: [DONE]\n\n"));
            controller.close();
            return;
          }
        }

        controller.enqueue(encoder.encode("data: [DONE]\n\n"));
        controller.close();
      } catch (error) {
        controller.error(error);
      }
    },
  });

  return new Response(stream, {
    status: 200,
    headers: {
      "Content-Type": "text/event-stream",
      "Cache-Control": "no-cache",
      "x-ubq-upstream": "chatgpt_codex",
    },
  });
};

const completeChatCompletions = async (
  upstream: Response,
  model: string,
  usageContext?: UsageContext,
): Promise<Response> => {
  if (!upstream.body) return openaiError(502, "Codex upstream response missing body.", "codex_upstream_missing_body");

  let id = `chatcmpl_${crypto.randomUUID().replace(/-/g, "")}`;
  let created = Math.floor(Date.now() / 1000);
  let content = "";
  let usage: Record<string, unknown> | null = null;

  for await (const ev of parseSseEvents(upstream.body)) {
    if (!isRecord(ev)) continue;
    const type = getString(ev.type);
    if (type === "response.created" && isRecord(ev.response)) {
      const upstreamId = getString(ev.response.id);
      const createdAt = typeof ev.response.created_at === "number" ? ev.response.created_at : null;
      if (upstreamId) id = upstreamId;
      if (createdAt) created = createdAt;
      continue;
    }
    if (type === "response.output_text.delta") {
      content += getString(ev.delta) ?? "";
      continue;
    }
    if (type === "response.completed" && isRecord(ev.response)) {
      const usageTokens = extractUsageTokens(ev.response.usage);
      if (usageTokens) {
        usage = {
          prompt_tokens: usageTokens.inputTokens,
          completion_tokens: usageTokens.outputTokens,
          total_tokens: usageTokens.totalTokens,
        };
      }
      await recordCompletionUsage(usageContext, usageTokens);
      break;
    }
  }

  const body: Record<string, unknown> = {
    id,
    object: "chat.completion",
    created,
    model,
    choices: [
      {
        index: 0,
        message: { role: "assistant", content },
        finish_reason: "stop",
      },
    ],
  };
  if (usage) body.usage = usage;
  return json(200, body, { "x-ubq-upstream": "chatgpt_codex" });
};

export const handleModels = async (): Promise<Response> => {
  let upstream: Response | null = null;
  try {
    upstream = await fetchCodexModels();
  } catch (error) {
    console.error("[ai.ubq.fi] Upstream models fetch failed:", error);
    upstream = null;
    const snapshot = await loadCodexModelsSnapshot();
    if (snapshot && snapshot.source !== "codex_cli" && Array.isArray(snapshot.models) && snapshot.models.length > 0) {
      const normalized = normalizeModelList({ models: snapshot.models });
      if (normalized) return json(200, normalized, { "x-ubq-upstream": snapshot.source || "chatgpt_codex" });
    }
    return toCodexErrorResponse(error);
  }

  if (!upstream.ok) {
    const text = await upstream.text().catch(() => "");
    if (upstream.status !== 400 && upstream.status !== 401 && upstream.status !== 403) {
      const snapshot = await loadCodexModelsSnapshot();
      if (snapshot && snapshot.source !== "codex_cli" && Array.isArray(snapshot.models) && snapshot.models.length > 0) {
        const normalized = normalizeModelList({ models: snapshot.models });
        if (normalized) return json(200, normalized, { "x-ubq-upstream": snapshot.source || "chatgpt_codex" });
      }
    }
    return new Response(text || upstream.statusText, {
      status: upstream.status,
      headers: {
        "Content-Type": upstream.headers.get("Content-Type") ?? "text/plain",
        "x-ubq-upstream": "chatgpt_codex",
      },
    });
  }

  const payloadText = await upstream.text().catch(() => "");
  let parsed: unknown = null;
  try {
    parsed = payloadText ? JSON.parse(payloadText) : null;
  } catch {
    parsed = null;
  }

  const normalized = normalizeModelList(parsed);
  if (!normalized) {
    return openaiError(502, "Upstream models response did not include a model list.", "codex_upstream_invalid");
  }

  return json(200, normalized, { "x-ubq-upstream": "chatgpt_codex" });
};

export const handleEmbeddings = async (req: Request, usageContext?: UsageContext): Promise<Response> => {
  const requestId = crypto.randomUUID();
  const startedAtMs = Date.now();

  const rawBody = (await readJsonBody(req)) as Record<string, unknown> | null;
  if (!rawBody || !isRecord(rawBody)) {
    return openaiError(400, "Invalid JSON body", "invalid_request_error");
  }

  const unknownKey = findUnknownKey(rawBody, EMBEDDINGS_ALLOWED_KEYS);
  if (unknownKey) {
    return openaiError(400, `Unrecognized request argument supplied: ${unknownKey}`, "invalid_request_error");
  }

  const modelRaw = getString(rawBody.model);
  if (!modelRaw || !modelRaw.trim()) {
    return openaiError(400, "model is required and must be a non-empty string", "invalid_request_error", {
      param: "model",
    });
  }
  const model = modelRaw.trim();

  const encodingFormat = parseEmbeddingsEncodingFormat(rawBody.encoding_format);
  if (!encodingFormat.ok) {
    return openaiError(400, encodingFormat.message, "invalid_request_error", { param: "encoding_format" });
  }

  if (Object.prototype.hasOwnProperty.call(rawBody, "dimensions")) {
    const rawDimensions = rawBody.dimensions;
    if (typeof rawDimensions !== "number" || !Number.isFinite(rawDimensions)) {
      return openaiError(400, "dimensions must be a number", "invalid_request_error", { param: "dimensions" });
    }
    const dims = Math.trunc(rawDimensions);
    if (dims <= 0) {
      return openaiError(400, "dimensions must be a positive integer", "invalid_request_error", {
        param: "dimensions",
      });
    }
    // Voyage does not guarantee OpenAI-compatible dimension control.
    return openaiError(400, "dimensions is not supported by this gateway", "invalid_request_error", {
      param: "dimensions",
    });
  }

  if (Object.prototype.hasOwnProperty.call(rawBody, "user")) {
    const user = rawBody.user;
    if (user !== undefined && user !== null && typeof user !== "string") {
      return openaiError(400, "user must be a string", "invalid_request_error", { param: "user" });
    }
  }

  const inputRaw = rawBody.input;
  let inputs: string[] = [];
  if (typeof inputRaw === "string") {
    inputs = [inputRaw];
  } else if (Array.isArray(inputRaw)) {
    for (const item of inputRaw) {
      if (typeof item !== "string") {
        return openaiError(400, "input must be a string or an array of strings", "invalid_request_error", {
          param: "input",
        });
      }
      inputs.push(item);
    }
  } else {
    return openaiError(400, "input must be a string or an array of strings", "invalid_request_error", {
      param: "input",
    });
  }

  if (inputs.length === 0) {
    return openaiError(400, "input must be a non-empty string or a non-empty array", "invalid_request_error", {
      param: "input",
    });
  }

  if (inputs.length > EMBEDDINGS_MAX_INPUTS_PER_REQUEST) {
    return openaiError(
      400,
      `Too many inputs: ${inputs.length} (max ${EMBEDDINGS_MAX_INPUTS_PER_REQUEST})`,
      "invalid_request_error",
      { param: "input" },
    );
  }

  let totalChars = 0;
  for (const text of inputs) {
    const len = text.length;
    if (len > EMBEDDINGS_MAX_CHARS_PER_INPUT) {
      return openaiError(
        400,
        `Input too large: ${len} chars (max ${EMBEDDINGS_MAX_CHARS_PER_INPUT})`,
        "invalid_request_error",
        { param: "input" },
      );
    }
    totalChars += len;
    if (totalChars > EMBEDDINGS_MAX_TOTAL_CHARS) {
      return openaiError(
        400,
        `Request too large: ${totalChars} chars total (max ${EMBEDDINGS_MAX_TOTAL_CHARS})`,
        "invalid_request_error",
        { param: "input" },
      );
    }
  }

  const resolved = resolveEmbeddingsModel(model);
  if (!resolved) {
    return openaiError(400, `Unsupported embedding model: ${model}`, "model_not_found", { param: "model" });
  }

  for (const text of inputs) {
    const tokenEstimate = estimateTokens(text);
    if (tokenEstimate > VOYAGE_RATE_LIMIT_TPM) {
      return openaiError(
        400,
        `Input too large for embeddings provider: ~${tokenEstimate} tokens (max ${VOYAGE_RATE_LIMIT_TPM}).`,
        "invalid_request_error",
        { param: "input" },
      );
    }
  }

  await recordRequestUsage(usageContext, { model, route: "embeddings", stream: false, reasoning: null });

  const deadlineMs = startedAtMs + EMBEDDINGS_TIMEOUT_MS;
  const kv = await kvPromise;
  const apiKey = await readVoyageApiKey(kv);
  if (!apiKey) {
    await recordErrorUsage(usageContext);
    return openaiError(
      503,
      "Embeddings provider is not configured: set VOYAGEAI_API_KEY (or store it in Deno KV)",
      "server_error",
      { type: "server_error", param: null },
    );
  }
  const shouldCache = encodingFormat.value === "float" && !Object.prototype.hasOwnProperty.call(rawBody, "dimensions");

  const hashes = await Promise.all(inputs.map((text) => sha256Hex(text)));

  // Dedupe within a request (hash collisions are astronomically unlikely).
  const buckets = new Map<string, { text: string; indices: number[] }>();
  for (let i = 0; i < inputs.length; i += 1) {
    const hash = hashes[i]!;
    const existing = buckets.get(hash);
    if (existing) {
      existing.indices.push(i);
    } else {
      buckets.set(hash, { text: inputs[i]!, indices: [i] });
    }
  }

  const cacheModelKey = model.toLowerCase();
  const cacheKeyFor = (hash: string): Deno.KvKey => ["embeddings", "v1", cacheModelKey, hash];
  const vectorsByIndex: Array<number[] | null> = Array.from({ length: inputs.length }, () => null);

  let voyageTotalTokens = 0;
  let sawVoyageTokenUsage = false;

  const missing: Array<{ hash: string; text: string; indices: number[] }> = [];
  if (shouldCache && kv) {
    const unique = Array.from(buckets.entries()).map(([hash, bucket]) => ({ hash, ...bucket }));
    const entries = await Promise.all(unique.map((item) => kv.get<{ embedding?: unknown }>(cacheKeyFor(item.hash))));
    for (let i = 0; i < unique.length; i += 1) {
      const item = unique[i]!;
      const entry = entries[i]!;
      const cached = entry.value?.embedding;
      if (Array.isArray(cached) && cached.every((v) => typeof v === "number" && Number.isFinite(v))) {
        for (const idx of item.indices) vectorsByIndex[idx] = cached as number[];
      } else {
        missing.push(item);
      }
    }
  } else {
    for (const [hash, bucket] of buckets.entries()) {
      missing.push({ hash, text: bucket.text, indices: bucket.indices });
    }
  }

  console.info(
    `[ai.ubq.fi] embeddings request_id=${requestId} model=${model} upstream=${resolved.upstream} inputs=${inputs.length} unique=${buckets.size} chars=${totalChars} cache=${
      shouldCache && Boolean(kv)
    }`,
  );

  if (missing.length > 0) {
    const chunks = chunkByTokenBudget(
      missing.map((item) => ({ hash: item.hash, text: item.text })),
      EMBEDDINGS_MAX_INPUTS_PER_REQUEST,
      VOYAGE_RATE_LIMIT_TPM,
    );

    let offset = 0;
    for (const chunk of chunks) {
      const now = Date.now();
      if (now >= deadlineMs) {
        await recordErrorUsage(usageContext);
        return openaiError(502, "Embeddings request timed out.", "timeout", { type: "server_error", param: null });
      }

      const chunkItems = missing.slice(offset, offset + chunk.length);
      offset += chunk.length;
      const texts = chunkItems.map((item) => item.text);
      const tokenEstimate = estimateTokenCount(texts);

      if (kv) {
        const reserved = await applyVoyageRateLimit(kv, tokenEstimate, deadlineMs);
        if (!reserved.ok) {
          await recordErrorUsage(usageContext);
          const retryAfterSeconds = Math.max(1, Math.ceil(reserved.wait_ms / 1000));
          const body = {
            error: {
              message: `Rate limit exceeded; retry after ~${retryAfterSeconds}s`,
              type: "rate_limit_error",
              code: "rate_limit_exceeded",
              param: null,
            },
          };
          return json(429, body, { "Retry-After": String(retryAfterSeconds) });
        }
      }

      const retryable = new Set([429, 500, 502, 503, 504]);
      let attempt = 0;
      let backoffMs = 250;
      let vectors: number[][] | null = null;

      for (;;) {
        try {
          const upstream = await fetchVoyageEmbeddings({ apiKey, model: resolved.model, inputs: texts, deadlineMs });
          vectors = upstream.vectors;
          if (typeof upstream.totalTokens === "number") {
            sawVoyageTokenUsage = true;
            voyageTotalTokens += upstream.totalTokens;
          }
          break;
        } catch (error) {
          const status = (error as { status?: number }).status;
          const retryAfterMs = (error as { retry_after_ms?: number | null }).retry_after_ms ?? null;
          const snippet = formatErrorSnippet(error);
          const message = snippet
            ? `Embeddings upstream request failed: ${snippet}`
            : "Embeddings upstream request failed.";

          if (!status || !retryable.has(status) || attempt >= 2) {
            console.error(`[ai.ubq.fi] embeddings request_id=${requestId} upstream_error:`, error);
            await recordErrorUsage(usageContext);
            return openaiError(502, message, "upstream_error", { type: "server_error", param: null });
          }

          const now = Date.now();
          const waitMs = Math.max(0, retryAfterMs ?? backoffMs);
          if (now + waitMs >= deadlineMs) {
            if (status === 429) {
              await recordErrorUsage(usageContext);
              const retryAfterSeconds = Math.max(1, Math.ceil(waitMs / 1000));
              const body = {
                error: {
                  message,
                  type: "rate_limit_error",
                  code: "rate_limit_exceeded",
                  param: null,
                },
              };
              return json(429, body, { "Retry-After": String(retryAfterSeconds) });
            }
            await recordErrorUsage(usageContext);
            return openaiError(502, message, "upstream_error", { type: "server_error", param: null });
          }

          await sleep(waitMs);
          backoffMs = Math.min(2000, backoffMs * 2);
          attempt += 1;
        }
      }

      if (!vectors || vectors.length !== chunkItems.length) {
        await recordErrorUsage(usageContext);
        return openaiError(502, "Embeddings upstream returned a size mismatch.", "upstream_error", {
          type: "server_error",
          param: null,
        });
      }

      for (let i = 0; i < chunkItems.length; i += 1) {
        const item = chunkItems[i]!;
        const vec = vectors[i]!;
        for (const idx of item.indices) vectorsByIndex[idx] = vec;
        if (shouldCache && kv) {
          await writeEmbeddingsCacheEntryBestEffort(
            kv,
            cacheModelKey,
            item.hash,
            vec,
            Date.now(),
            deadlineMs,
          );
        }
      }
    }
  }

  const data: Array<{ object: "embedding"; index: number; embedding: number[] | string }> = [];
  for (let i = 0; i < vectorsByIndex.length; i += 1) {
    const vec = vectorsByIndex[i];
    if (!vec) {
      await recordErrorUsage(usageContext);
      return openaiError(502, "Embeddings gateway failed to construct a complete response.", "server_error", {
        type: "server_error",
        param: null,
      });
    }
    data.push({
      object: "embedding",
      index: i,
      embedding: encodingFormat.value === "base64" ? floatEmbeddingToBase64(vec) : vec,
    });
  }

  const elapsedMs = Date.now() - startedAtMs;
  console.info(
    `[ai.ubq.fi] embeddings request_id=${requestId} status=200 upstream=${resolved.upstream} ms=${elapsedMs}`,
  );

  const usageTokens: UsageTokens | null = sawVoyageTokenUsage
    ? { inputTokens: voyageTotalTokens, outputTokens: 0, totalTokens: voyageTotalTokens }
    : null;
  await recordCompletionUsage(usageContext, usageTokens);

  return json(200, {
    object: "list",
    data,
    model,
    usage: {
      prompt_tokens: usageTokens?.inputTokens ?? 0,
      total_tokens: usageTokens?.totalTokens ?? 0,
    },
  }, { "x-ubq-upstream": resolved.upstream });
};

const buildEmbeddingsJobBody = (
  job: EmbeddingsJobRecord,
  result: Record<string, unknown> | null,
): Record<string, unknown> => ({
  id: job.id,
  object: "embeddings.job",
  status: job.status,
  created_at_ms: job.created_at_ms,
  updated_at_ms: job.updated_at_ms,
  model: job.model,
  input_count: job.input_count,
  total_chars: job.total_chars,
  retry_after_seconds: job.retry_after_seconds,
  error: job.error,
  result,
});

const loadEmbeddingsVectorsFromCache = async (
  kv: Deno.Kv,
  cacheModelKey: string,
  hashesByIndex: string[],
): Promise<Array<number[] | null>> => {
  const uniqueHashes = Array.from(new Set(hashesByIndex));
  const cacheKeyFor = (hash: string): Deno.KvKey => ["embeddings", "v1", cacheModelKey, hash];
  const entries = await Promise.all(uniqueHashes.map((hash) => kv.get<{ embedding?: unknown }>(cacheKeyFor(hash))));
  const vectorsByHash = new Map<string, number[]>();
  for (let i = 0; i < uniqueHashes.length; i += 1) {
    const hash = uniqueHashes[i]!;
    const cached = entries[i]?.value?.embedding;
    if (Array.isArray(cached) && cached.every((v) => typeof v === "number" && Number.isFinite(v))) {
      vectorsByHash.set(hash, cached as number[]);
    }
  }
  return hashesByIndex.map((hash) => vectorsByHash.get(hash) ?? null);
};

const buildOpenAiEmbeddingsResult = (
  model: string,
  vectorsByIndex: Array<number[] | null>,
  usageTotalTokens: number,
): Record<string, unknown> => ({
  object: "list",
  data: vectorsByIndex.map((vec, index) => ({
    object: "embedding",
    index,
    embedding: vec ?? [],
  })),
  model,
  usage: { prompt_tokens: usageTotalTokens, total_tokens: usageTotalTokens },
});

const reserveVoyageBudgetForJob = async (
  kv: Deno.Kv,
  tokens: number,
): Promise<{ ok: true } | { ok: false; wait_ms: number }> => {
  for (let attempt = 0; attempt < 6; attempt += 1) {
    const reserved = await tryReserveVoyageBudget(kv, tokens);
    if (reserved.ok) return reserved;
    if (reserved.wait_ms > 0) return reserved;
    await sleep(5 + attempt * 5);
  }
  return { ok: false, wait_ms: 1000 };
};

const updateEmbeddingsJobRecord = async (
  kv: Deno.Kv,
  key: Deno.KvKey,
  job: EmbeddingsJobRecord,
): Promise<void> => {
  await kv.set(key, job, { expireIn: EMBEDDINGS_JOB_TTL_MS });
};

const deleteEmbeddingsJobInputs = async (
  kv: Deno.Kv,
  tokenHash: string,
  jobId: string,
  uniqueHashes: string[],
): Promise<void> => {
  await Promise.all(uniqueHashes.map((hash) => kv.delete(embeddingsJobInputKey(tokenHash, jobId, hash))));
};

const runEmbeddingsJobAttempt = async (params: {
  reqId: string;
  kv: Deno.Kv;
  apiKey: string;
  tokenSeed: string;
  tokenHash: string;
  jobKey: Deno.KvKey;
  jobEntry: Deno.KvEntryMaybe<EmbeddingsJobRecord>;
  job: EmbeddingsJobRecord;
  deadlineMs: number;
  usageContext?: UsageContext;
}): Promise<Response> => {
  const now = Date.now();
  if (params.job.locked_until_ms && params.job.locked_until_ms > now) {
    const retryAfterSeconds = Math.max(1, Math.ceil((params.job.locked_until_ms - now) / 1000));
    const body = buildEmbeddingsJobBody(params.job, null);
    return json(202, body, { "Retry-After": String(retryAfterSeconds) });
  }

  const lockedUntilMs = now + EMBEDDINGS_JOB_LOCK_MS;
  const locked: EmbeddingsJobRecord = {
    ...params.job,
    status: "running",
    locked_until_ms: lockedUntilMs,
    updated_at_ms: now,
    retry_after_seconds: null,
  };

  const lockCommit = await params.kv.atomic()
    .check(params.jobEntry)
    .set(params.jobKey, locked, { expireIn: EMBEDDINGS_JOB_TTL_MS })
    .commit();
  if (!lockCommit.ok) {
    const body = buildEmbeddingsJobBody(params.job, null);
    return json(202, body, { "Retry-After": "1" });
  }

  const cacheModelKey = locked.cache_model_key;
  const hashesByIndex = locked.input_hashes;
  const uniqueHashes = Array.from(new Set(hashesByIndex));
  const cacheKeyFor = (hash: string): Deno.KvKey => ["embeddings", "v1", cacheModelKey, hash];

  let currentJob: EmbeddingsJobRecord = locked;
  let queueRetryAfterMs: number | null = null;

  const computeMissing = async (): Promise<string[]> => {
    const entries = await Promise.all(
      uniqueHashes.map((hash) => params.kv.get<{ embedding?: unknown }>(cacheKeyFor(hash))),
    );
    const missing: string[] = [];
    for (let i = 0; i < uniqueHashes.length; i += 1) {
      const hash = uniqueHashes[i]!;
      const cached = entries[i]?.value?.embedding;
      if (Array.isArray(cached) && cached.every((v) => typeof v === "number" && Number.isFinite(v))) continue;
      missing.push(hash);
    }
    return missing;
  };

  const failJob = async (message: string, code: string): Promise<Response> => {
    const failed: EmbeddingsJobRecord = {
      ...currentJob,
      status: "failed",
      updated_at_ms: Date.now(),
      locked_until_ms: null,
      retry_after_seconds: null,
      error: { message, type: "server_error", code },
    };
    currentJob = failed;
    await updateEmbeddingsJobRecord(params.kv, params.jobKey, failed);
    await deleteEmbeddingsJobInputs(params.kv, params.tokenHash, failed.id, uniqueHashes);
    await recordErrorUsage(params.usageContext);
    return json(200, buildEmbeddingsJobBody(failed, null), { "x-ubq-upstream": failed.upstream });
  };

  const queueJob = async (waitMs: number): Promise<Response> => {
    const retryAfterSeconds = Math.max(1, Math.ceil(waitMs / 1000));
    const queued: EmbeddingsJobRecord = {
      ...currentJob,
      status: "queued",
      updated_at_ms: Date.now(),
      locked_until_ms: null,
      retry_after_seconds: retryAfterSeconds,
      error: null,
    };
    currentJob = queued;
    await updateEmbeddingsJobRecord(params.kv, params.jobKey, queued);
    const body = buildEmbeddingsJobBody(queued, null);
    return json(202, body, { "Retry-After": String(retryAfterSeconds), "x-ubq-upstream": queued.upstream });
  };

  const succeedJob = async (): Promise<Response> => {
    const vectorsByIndex = await loadEmbeddingsVectorsFromCache(params.kv, cacheModelKey, hashesByIndex);
    if (vectorsByIndex.some((vec) => !vec)) {
      return await failJob("Embeddings job completed but cache entries were missing.", "embeddings_job_cache_miss");
    }
    const succeeded: EmbeddingsJobRecord = {
      ...currentJob,
      status: "succeeded",
      updated_at_ms: Date.now(),
      locked_until_ms: null,
      retry_after_seconds: null,
      error: null,
    };
    currentJob = succeeded;
    await updateEmbeddingsJobRecord(params.kv, params.jobKey, succeeded);
    await deleteEmbeddingsJobInputs(params.kv, params.tokenHash, succeeded.id, uniqueHashes);
    const result = buildOpenAiEmbeddingsResult(succeeded.model, vectorsByIndex, succeeded.usage_total_tokens);
    const usageTokens: UsageTokens | null = succeeded.usage_total_tokens > 0
      ? { inputTokens: succeeded.usage_total_tokens, outputTokens: 0, totalTokens: succeeded.usage_total_tokens }
      : null;
    await recordCompletionUsage(params.usageContext, usageTokens);
    return json(200, buildEmbeddingsJobBody(succeeded, result), { "x-ubq-upstream": succeeded.upstream });
  };

  try {
    const missingBefore = await computeMissing();
    if (missingBefore.length === 0) return await succeedJob();

    const inputEntries = await Promise.all(
      missingBefore.map((hash) =>
        params.kv.get<EmbeddingsJobInputRecord>(embeddingsJobInputKey(params.tokenHash, locked.id, hash))
      ),
    );
    const items: Array<{ hash: string; text: string }> = [];
    for (let i = 0; i < missingBefore.length; i += 1) {
      const hash = missingBefore[i]!;
      const entry = inputEntries[i]!;
      const normalized = normalizeEmbeddingsJobInputRecord(entry.value);
      if (!normalized) {
        return await failJob("Embeddings job input expired or was unavailable.", "embeddings_job_input_missing");
      }
      const text = await decryptEmbeddingsJobInput(params.tokenSeed, normalized);
      if (text === null) {
        return await failJob("Embeddings job input could not be decrypted.", "embeddings_job_input_decrypt_failed");
      }
      items.push({ hash, text });
    }

    const chunks = chunkByTokenBudget(items, EMBEDDINGS_MAX_INPUTS_PER_REQUEST, VOYAGE_RATE_LIMIT_TPM);
    for (const chunk of chunks) {
      if (Date.now() >= params.deadlineMs) {
        queueRetryAfterMs = 1000;
        break;
      }

      const texts = chunk.map((item) => item.text);
      const tokenEstimate = estimateTokenCount(texts);

      const reserved = await reserveVoyageBudgetForJob(params.kv, tokenEstimate);
      if (!reserved.ok) {
        queueRetryAfterMs = reserved.wait_ms > 0 ? reserved.wait_ms : 1000;
        break;
      }

      let vectors: number[][] | null = null;
      let totalTokens: number | null = null;
      try {
        const upstream = await fetchVoyageEmbeddings({
          apiKey: params.apiKey,
          model: currentJob.upstream_model,
          inputs: texts,
          deadlineMs: params.deadlineMs,
        });
        vectors = upstream.vectors;
        totalTokens = upstream.totalTokens;
      } catch (error) {
        const status = (error as { status?: number }).status;
        const retryAfterMs = (error as { retry_after_ms?: number | null }).retry_after_ms ?? null;
        if (status === 429) {
          queueRetryAfterMs = retryAfterMs ?? 60_000;
          break;
        }
        const snippet = formatErrorSnippet(error);
        const message = snippet
          ? `Embeddings upstream request failed: ${snippet}`
          : "Embeddings upstream request failed.";
        return await failJob(message, "embeddings_job_upstream_error");
      }

      if (!vectors || vectors.length !== chunk.length) {
        return await failJob("Embeddings upstream returned a size mismatch.", "embeddings_job_upstream_mismatch");
      }

      if (typeof totalTokens === "number") {
        currentJob = { ...currentJob, usage_total_tokens: currentJob.usage_total_tokens + totalTokens };
      }

      for (let i = 0; i < chunk.length; i += 1) {
        const item = chunk[i]!;
        const vec = vectors[i]!;
        await writeEmbeddingsCacheEntryBestEffort(
          params.kv,
          currentJob.cache_model_key,
          item.hash,
          vec,
          Date.now(),
          params.deadlineMs,
        );
      }
    }

    const missingAfter = await computeMissing();
    if (missingAfter.length === 0) return await succeedJob();

    const waitMs = queueRetryAfterMs ?? 60_000;
    return await queueJob(waitMs);
  } finally {
    const elapsedMs = Date.now() - (params.deadlineMs - EMBEDDINGS_TIMEOUT_MS);
    console.info(
      `[ai.ubq.fi] embeddings_job request_id=${params.reqId} job_id=${params.job.id} status=${currentJob.status} ms=${elapsedMs}`,
    );
  }
};

export const handleEmbeddingsJobCreate = async (
  req: Request,
  authToken: string | null,
  usageContext?: UsageContext,
): Promise<Response> => {
  const requestId = crypto.randomUUID();
  const startedAtMs = Date.now();

  const rawBody = (await readJsonBody(req)) as Record<string, unknown> | null;
  if (!rawBody || !isRecord(rawBody)) {
    await recordErrorUsage(usageContext);
    return openaiError(400, "Invalid JSON body", "invalid_request_error");
  }

  const unknownKey = findUnknownKey(rawBody, EMBEDDINGS_ALLOWED_KEYS);
  if (unknownKey) {
    await recordErrorUsage(usageContext);
    return openaiError(400, `Unrecognized request argument supplied: ${unknownKey}`, "invalid_request_error");
  }

  const modelRaw = getString(rawBody.model);
  if (!modelRaw || !modelRaw.trim()) {
    await recordErrorUsage(usageContext);
    return openaiError(400, "model is required and must be a non-empty string", "invalid_request_error", {
      param: "model",
    });
  }
  const model = modelRaw.trim();

  const encodingFormat = parseEmbeddingsEncodingFormat(rawBody.encoding_format);
  if (!encodingFormat.ok) {
    await recordErrorUsage(usageContext);
    return openaiError(400, encodingFormat.message, "invalid_request_error", { param: "encoding_format" });
  }
  if (encodingFormat.value !== "float") {
    await recordErrorUsage(usageContext);
    return openaiError(
      400,
      'Embeddings jobs only support encoding_format="float"',
      "invalid_request_error",
      { param: "encoding_format" },
    );
  }

  if (Object.prototype.hasOwnProperty.call(rawBody, "dimensions")) {
    await recordErrorUsage(usageContext);
    return openaiError(400, "dimensions is not supported by this gateway", "invalid_request_error", {
      param: "dimensions",
    });
  }

  if (Object.prototype.hasOwnProperty.call(rawBody, "user")) {
    const user = rawBody.user;
    if (user !== undefined && user !== null && typeof user !== "string") {
      await recordErrorUsage(usageContext);
      return openaiError(400, "user must be a string", "invalid_request_error", { param: "user" });
    }
  }

  const inputRaw = rawBody.input;
  let inputs: string[] = [];
  if (typeof inputRaw === "string") {
    inputs = [inputRaw];
  } else if (Array.isArray(inputRaw)) {
    for (const item of inputRaw) {
      if (typeof item !== "string") {
        await recordErrorUsage(usageContext);
        return openaiError(400, "input must be a string or an array of strings", "invalid_request_error", {
          param: "input",
        });
      }
      inputs.push(item);
    }
  } else {
    await recordErrorUsage(usageContext);
    return openaiError(400, "input must be a string or an array of strings", "invalid_request_error", {
      param: "input",
    });
  }

  if (inputs.length === 0) {
    await recordErrorUsage(usageContext);
    return openaiError(400, "input must be a non-empty string or a non-empty array", "invalid_request_error", {
      param: "input",
    });
  }

  if (inputs.length > EMBEDDINGS_MAX_INPUTS_PER_REQUEST) {
    await recordErrorUsage(usageContext);
    return openaiError(
      400,
      `Too many inputs: ${inputs.length} (max ${EMBEDDINGS_MAX_INPUTS_PER_REQUEST})`,
      "invalid_request_error",
      { param: "input" },
    );
  }

  let totalChars = 0;
  for (const text of inputs) {
    const len = text.length;
    if (len > EMBEDDINGS_MAX_CHARS_PER_INPUT) {
      await recordErrorUsage(usageContext);
      return openaiError(
        400,
        `Input too large: ${len} chars (max ${EMBEDDINGS_MAX_CHARS_PER_INPUT})`,
        "invalid_request_error",
        { param: "input" },
      );
    }
    totalChars += len;
    if (totalChars > EMBEDDINGS_MAX_TOTAL_CHARS) {
      await recordErrorUsage(usageContext);
      return openaiError(
        400,
        `Request too large: ${totalChars} chars total (max ${EMBEDDINGS_MAX_TOTAL_CHARS})`,
        "invalid_request_error",
        { param: "input" },
      );
    }
  }

  const resolved = resolveEmbeddingsModel(model);
  if (!resolved) {
    await recordErrorUsage(usageContext);
    return openaiError(400, `Unsupported embedding model: ${model}`, "model_not_found", { param: "model" });
  }

  for (const text of inputs) {
    const tokenEstimate = estimateTokens(text);
    if (tokenEstimate > VOYAGE_RATE_LIMIT_TPM) {
      await recordErrorUsage(usageContext);
      return openaiError(
        400,
        `Input too large for embeddings provider: ~${tokenEstimate} tokens (max ${VOYAGE_RATE_LIMIT_TPM}).`,
        "invalid_request_error",
        { param: "input" },
      );
    }
  }

  await recordRequestUsage(usageContext, { model, route: "embeddings.jobs.create", stream: false, reasoning: null });

  const kv = await kvPromise;
  if (!kv) {
    await recordErrorUsage(usageContext);
    return openaiError(503, "Embeddings jobs require Deno KV", "server_error", { type: "server_error", param: null });
  }

  const apiKey = await readVoyageApiKey(kv);
  if (!apiKey) {
    await recordErrorUsage(usageContext);
    return openaiError(
      503,
      "Embeddings provider is not configured: set VOYAGEAI_API_KEY (or store it in Deno KV)",
      "server_error",
      { type: "server_error", param: null },
    );
  }

  const hashesByIndex = await Promise.all(inputs.map((text) => sha256Hex(text)));
  const uniqueTextsByHash = new Map<string, string>();
  for (let i = 0; i < inputs.length; i += 1) uniqueTextsByHash.set(hashesByIndex[i]!, inputs[i]!);
  const uniqueHashes = Array.from(uniqueTextsByHash.keys());

  const jobId = `embjob_${crypto.randomUUID().replace(/-/g, "")}`;
  const tokenSeed = resolveEmbeddingsJobTokenSeed(jobId, authToken, usageContext);
  const tokenHash = await sha256Hex(tokenSeed);
  const now = Date.now();
  const cacheModelKey = model.toLowerCase();

  // Store encrypted inputs (no raw text) so queued jobs can be processed later without the client resending inputs.
  const inputWrites = uniqueHashes.map(async (hash) => {
    const record = await encryptEmbeddingsJobInput(tokenSeed, uniqueTextsByHash.get(hash)!);
    await kv.set(embeddingsJobInputKey(tokenHash, jobId, hash), record, { expireIn: EMBEDDINGS_JOB_TTL_MS });
  });
  await Promise.all(inputWrites);

  const job: EmbeddingsJobRecord = {
    id: jobId,
    status: "queued",
    created_at_ms: now,
    updated_at_ms: now,
    model,
    cache_model_key: cacheModelKey,
    upstream: resolved.upstream,
    upstream_model: resolved.model,
    input_hashes: hashesByIndex,
    input_count: inputs.length,
    total_chars: totalChars,
    usage_total_tokens: 0,
    retry_after_seconds: null,
    locked_until_ms: null,
    error: null,
  };
  const jobKey = embeddingsJobKey(tokenHash, jobId);
  await kv.set(jobKey, job, { expireIn: EMBEDDINGS_JOB_TTL_MS });

  const deadlineMs = startedAtMs + EMBEDDINGS_TIMEOUT_MS;
  const entry = await kv.get<EmbeddingsJobRecord>(jobKey);
  const value = entry.value;
  if (!value) {
    await recordErrorUsage(usageContext);
    return openaiError(502, "Embeddings job could not be persisted.", "server_error", {
      type: "server_error",
      param: null,
    });
  }

  return await runEmbeddingsJobAttempt({
    reqId: requestId,
    kv,
    apiKey,
    tokenSeed,
    tokenHash,
    jobKey,
    jobEntry: entry,
    job: value,
    deadlineMs,
    usageContext,
  });
};

export const handleEmbeddingsJobGet = async (
  _req: Request,
  authToken: string | null,
  jobId: string,
  usageContext?: UsageContext,
): Promise<Response> => {
  const requestId = crypto.randomUUID();
  const startedAtMs = Date.now();

  const kv = await kvPromise;
  if (!kv) {
    await recordErrorUsage(usageContext);
    return openaiError(503, "Embeddings jobs require Deno KV", "server_error", { type: "server_error", param: null });
  }

  const preferredSeed = resolveEmbeddingsJobTokenSeed(jobId, authToken, usageContext);
  const preferredHash = await sha256Hex(preferredSeed);
  let tokenSeed = preferredSeed;
  let tokenHash = preferredHash;
  let jobKey = embeddingsJobKey(preferredHash, jobId);
  let entry = await kv.get<EmbeddingsJobRecord>(jobKey);

  // Backwards compatibility: older versions keyed jobs to the raw bearer token.
  if (!entry.value && authToken && preferredSeed !== authToken) {
    const legacySeed = authToken;
    const legacyHash = await sha256Hex(legacySeed);
    const legacyKey = embeddingsJobKey(legacyHash, jobId);
    const legacyEntry = await kv.get<EmbeddingsJobRecord>(legacyKey);
    if (legacyEntry.value) {
      tokenSeed = legacySeed;
      tokenHash = legacyHash;
      jobKey = legacyKey;
      entry = legacyEntry;
    }
  }

  const job = entry.value;
  if (!job) {
    await recordErrorUsage(usageContext);
    return openaiError(404, "Embeddings job not found", "not_found", { type: "invalid_request_error", param: null });
  }

  await recordRequestUsage(usageContext, {
    model: job.model,
    route: "embeddings.jobs.get",
    stream: false,
    reasoning: null,
  });

  if (job.status === "succeeded") {
    const vectorsByIndex = await loadEmbeddingsVectorsFromCache(kv, job.cache_model_key, job.input_hashes);
    const result = vectorsByIndex.some((vec) => !vec)
      ? null
      : buildOpenAiEmbeddingsResult(job.model, vectorsByIndex, job.usage_total_tokens);
    if (!result) {
      // Cache misses are unexpected (cache TTL is longer than job TTL), but if it happens
      // there's nothing the client can do besides resubmitting the job.
      const failed: EmbeddingsJobRecord = {
        ...job,
        status: "failed",
        updated_at_ms: Date.now(),
        locked_until_ms: null,
        retry_after_seconds: null,
        error: {
          message: "Embeddings job result was unavailable; please resubmit.",
          type: "server_error",
          code: "embeddings_job_result_missing",
        },
      };
      await updateEmbeddingsJobRecord(kv, jobKey, failed);
      return json(200, buildEmbeddingsJobBody(failed, null), { "x-ubq-upstream": failed.upstream });
    }
    return json(200, buildEmbeddingsJobBody(job, result), { "x-ubq-upstream": job.upstream });
  }

  if (job.status === "failed") {
    return json(200, buildEmbeddingsJobBody(job, null), { "x-ubq-upstream": job.upstream });
  }

  const apiKey = await readVoyageApiKey(kv);
  if (!apiKey) {
    await recordErrorUsage(usageContext);
    return openaiError(
      503,
      "Embeddings provider is not configured: set VOYAGEAI_API_KEY (or store it in Deno KV)",
      "server_error",
      { type: "server_error", param: null },
    );
  }

  const deadlineMs = startedAtMs + EMBEDDINGS_TIMEOUT_MS;
  const response = await runEmbeddingsJobAttempt({
    reqId: requestId,
    kv,
    apiKey,
    tokenSeed,
    tokenHash,
    jobKey,
    jobEntry: entry,
    job,
    deadlineMs,
    usageContext,
  });

  const elapsedMs = Date.now() - startedAtMs;
  console.info(`[ai.ubq.fi] embeddings_job_get request_id=${requestId} job_id=${jobId} ms=${elapsedMs}`);
  return response;
};

export const handleChatCompletions = async (req: Request, usageContext?: UsageContext): Promise<Response> => {
  const body = (await readJsonBody(req)) as ChatCompletionRequest | null;
  if (!body || !isRecord(body)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const rawRecord = body as Record<string, unknown>;
  const unknownKey = findUnknownKey(rawRecord, CHAT_COMPLETIONS_ALLOWED_KEYS);
  if (unknownKey) {
    return openaiError(400, `Unrecognized request argument supplied: ${unknownKey}`, "invalid_request_error");
  }
  const warnings = buildIgnoredWarnings(
    rawRecord,
    new Set([
      "messages",
      "model",
      "stream",
      "reasoning_effort",
      "tools",
      "tool_choice",
      "parallel_tool_calls",
      "prompt_cache_key",
    ]),
  );

  const hasModel = Object.prototype.hasOwnProperty.call(rawRecord, "model");
  const rawModelValue = rawRecord.model;
  const modelRawValue = getString(rawModelValue);
  if (hasModel && modelRawValue === null && rawModelValue !== null && rawModelValue !== undefined) {
    return openaiError(400, "model must be a string", "invalid_request_error");
  }
  const modelRaw = (modelRawValue ?? "").trim() || await getDefaultModel();
  const model = normalizeModelForCodex(modelRaw);
  const messagesRaw = body.messages;
  if (!Array.isArray(messagesRaw)) return openaiError(400, "messages must be an array", "invalid_request_error");
  if (messagesRaw.length === 0) return openaiError(400, "messages must be a non-empty array", "invalid_request_error");

  const reasoningEffort = parseReasoningEffortField(body.reasoning_effort, "reasoning_effort");
  if (!reasoningEffort.ok) return openaiError(400, reasoningEffort.message, "invalid_request_error");

  const input: ResponseMessageItem[] = [];
  const instructionParts: string[] = [];
  for (const msg of messagesRaw) {
    if (!isRecord(msg)) return openaiError(400, "Invalid message in messages[]", "invalid_request_error");
    const roleRaw = getString(msg.role);
    if (!roleRaw) return openaiError(400, "Invalid message in messages[]", "invalid_request_error");
    if (roleRaw === "system" || roleRaw === "developer") {
      const converted = toResponseMessageItem(msg);
      if (!converted) return openaiError(400, "Invalid message in messages[]", "invalid_request_error");
      const instructionText = messageContentToText(converted.content).trim();
      if (instructionText) instructionParts.push(instructionText);
      continue;
    }
    const converted = toResponseMessageItem(msg);
    if (!converted) return openaiError(400, "Invalid message in messages[]", "invalid_request_error");
    input.push(converted);
  }

  if (input.length === 0) {
    // Ensure upstream receives a non-empty input for system-only chats.
    input.push({
      type: "message",
      role: "user",
      content: [{ type: "input_text", text: "" }],
    });
  }

  const instructions = instructionParts.join("\n\n").trim();
  const defaultEffort = await getDefaultReasoningEffort();
  const defaultReasoningLabel = resolveDefaultReasoningLabel(model, defaultEffort);
  let reasoningValue: Record<string, unknown> | null | undefined;
  if (reasoningEffort.value === undefined) {
    reasoningValue = looksLikeReasoningModel(model) ? { effort: defaultReasoningLabel } : undefined;
  } else if (reasoningEffort.value === null) {
    reasoningValue = null;
  } else {
    reasoningValue = { effort: reasoningEffort.value };
  }
  const codexBody = await buildCodexRequest(model, input, {
    reasoning: reasoningValue,
    instructions,
  });
  const passthroughKeys: PassthroughToolSchemaKey[] = [
    "tools",
    "tool_choice",
    "parallel_tool_calls",
    "prompt_cache_key",
  ];
  applyPassthroughToCodexRequest(codexBody, rawRecord, passthroughKeys);
  codexBody.store = false;

  const stream = Boolean(body.stream);
  const reasoningLabel = resolveReasoningLabelFromEffort(reasoningEffort.value, defaultReasoningLabel);
  await recordRequestUsage(usageContext, {
    model: modelRaw,
    route: "chat.completions",
    stream,
    reasoning: reasoningLabel,
  });

  let upstream: Response;
  try {
    upstream = await fetchCodexResponses(codexBody);
  } catch (error) {
    console.error("[ai.ubq.fi] Upstream fetch failed:", error);
    await recordErrorUsage(usageContext);
    return toCodexErrorResponse(error);
  }

  if (!upstream.ok) {
    await recordErrorUsage(usageContext);
    const text = await upstream.text().catch(() => "");
    return new Response(text || upstream.statusText, {
      status: upstream.status,
      headers: {
        "Content-Type": upstream.headers.get("Content-Type") ?? "text/plain",
        "x-ubq-upstream": "chatgpt_codex",
      },
    });
  }

  if (!upstream.body) {
    await recordErrorUsage(usageContext);
    return openaiError(502, "Codex upstream response missing body.", "codex_upstream_missing_body");
  }

  const response = stream ? streamChatCompletions(upstream, model, usageContext) : await completeChatCompletions(
    upstream,
    model,
    usageContext,
  );
  return withUosWarning(response, warnings);
};

export const handleResponses = async (req: Request, usageContext?: UsageContext): Promise<Response> => {
  const rawBody = (await readJsonBody(req)) as ResponsesRequest | null;
  if (!rawBody || !isRecord(rawBody)) return openaiError(400, "Invalid JSON body", "invalid_request_error");

  const rawRecord = rawBody as Record<string, unknown>;
  const unknownKey = findUnknownKey(rawRecord, RESPONSES_ALLOWED_KEYS);
  if (unknownKey) {
    return openaiError(400, `Unrecognized request argument supplied: ${unknownKey}`, "invalid_request_error");
  }
  const warnings = buildIgnoredWarnings(
    rawRecord,
    new Set([
      "model",
      "input",
      "stream",
      "reasoning",
      "instructions",
      "tools",
      "tool_choice",
      "parallel_tool_calls",
      "prompt_cache_key",
      "text",
      "include",
    ]),
  );

  const clientWantsStream = Boolean(rawBody.stream);

  const hasModel = Object.prototype.hasOwnProperty.call(rawRecord, "model");
  const rawModelValue = rawRecord.model;
  const modelRawValue = getString(rawModelValue);
  if (hasModel && modelRawValue === null && rawModelValue !== null && rawModelValue !== undefined) {
    return openaiError(400, "model must be a string", "invalid_request_error");
  }
  const modelRaw = (modelRawValue ?? "").trim() || await getDefaultModel();
  const model = normalizeModelForCodex(modelRaw);

  const inputRaw = rawBody.input;
  let input: ResponseInputItem[];
  if (inputRaw === undefined) {
    input = [];
  } else if (typeof inputRaw === "string") {
    input = [{ type: "message", role: "user", content: [{ type: "input_text", text: inputRaw }] }];
  } else if (Array.isArray(inputRaw)) {
    const converted: ResponseInputItem[] = [];
    let contentBuffer: MessageContentItem[] = [];

    const flushContentBuffer = () => {
      if (!contentBuffer.length) return;
      converted.push({ type: "message", role: "user", content: contentBuffer });
      contentBuffer = [];
    };

    let sawNonContentItem = false;
    for (const msg of inputRaw) {
      const mapped = normalizeResponseInputItem(msg);
      if (mapped) {
        flushContentBuffer();
        converted.push(mapped);
        sawNonContentItem = true;
        continue;
      }

      if (typeof msg === "string") {
        const contentItem: MessageContentItem = { type: "input_text", text: msg };
        if (sawNonContentItem) {
          converted.push({ type: "message", role: "user", content: [contentItem] });
        } else {
          contentBuffer.push(contentItem);
        }
        continue;
      }

      const contentItem = normalizeResponseContentItem(msg);
      if (contentItem) {
        if (sawNonContentItem) {
          converted.push({ type: "message", role: "user", content: [contentItem] });
        } else {
          contentBuffer.push(contentItem);
        }
        continue;
      }

      // Codex CLI uses the Responses API and can send additional input item types
      // (e.g. reasoning + function_call + function_call_output). Pass them through
      // so tool-calling conversations work end-to-end.
      if (isRecord(msg) && typeof msg.type === "string" && msg.type !== "message") {
        flushContentBuffer();
        converted.push(msg as ResponseInputItem);
        sawNonContentItem = true;
        continue;
      }

      return openaiError(400, "Invalid message in input[]", "invalid_request_error");
    }
    if (!sawNonContentItem || contentBuffer.length) {
      flushContentBuffer();
    }
    input = converted;
  } else {
    return openaiError(400, "input must be a string or an array", "invalid_request_error");
  }

  const reasoning = parseReasoningParam(rawBody.reasoning);
  if (!reasoning.ok) return openaiError(400, reasoning.message, "invalid_request_error");

  let instructions = "";
  if (Object.prototype.hasOwnProperty.call(rawRecord, "instructions")) {
    const rawInstructions = getString(rawBody.instructions);
    if (rawInstructions === null) {
      return openaiError(400, "instructions must be a string", "invalid_request_error");
    }
    instructions = rawInstructions;
  }
  const defaultEffort = await getDefaultReasoningEffort();
  const defaultReasoningLabel = resolveDefaultReasoningLabel(model, defaultEffort);
  const reasoningLabel = resolveReasoningLabelFromParam(reasoning.value, defaultReasoningLabel);

  let reasoningValue = reasoning.value;
  if (reasoningValue === undefined && looksLikeReasoningModel(model)) {
    reasoningValue = { effort: defaultReasoningLabel };
  }

  const codexBody = await buildCodexRequest(model, input, { reasoning: reasoningValue, instructions });
  const passthroughKeys: PassthroughToolSchemaKey[] = [
    "tools",
    "tool_choice",
    "parallel_tool_calls",
    "prompt_cache_key",
    "text",
    "include",
  ];
  applyPassthroughToCodexRequest(codexBody, rawRecord, passthroughKeys);
  codexBody.model = model;
  codexBody.input = input;
  codexBody.stream = true;
  codexBody.store = false;

  await recordRequestUsage(usageContext, {
    model: modelRaw,
    route: "responses",
    stream: clientWantsStream,
    reasoning: reasoningLabel,
  });

  let upstream: Response;
  try {
    upstream = await fetchCodexResponses(codexBody);
  } catch (error) {
    console.error("[ai.ubq.fi] Upstream fetch failed:", error);
    await recordErrorUsage(usageContext);
    return toCodexErrorResponse(error);
  }

  if (!upstream.ok) {
    await recordErrorUsage(usageContext);
    const text = await upstream.text().catch(() => "");
    return new Response(text || upstream.statusText, {
      status: upstream.status,
      headers: {
        "Content-Type": upstream.headers.get("Content-Type") ?? "text/plain",
        "x-ubq-upstream": "chatgpt_codex",
      },
    });
  }

  if (clientWantsStream) {
    if (!upstream.body) {
      await recordErrorUsage(usageContext);
      return openaiError(502, "Codex upstream response missing body.", "codex_upstream_missing_body");
    }
    const headers = new Headers(upstream.headers);
    headers.set("x-ubq-upstream", "chatgpt_codex");
    if (usageContext?.keyId || usageContext?.kernelRepo || usageContext?.kernelOrg) {
      const [clientStream, analyticsStream] = upstream.body.tee();
      void collectResponsesStreamUsage(analyticsStream, usageContext);
      const response = new Response(clientStream, {
        status: upstream.status,
        statusText: upstream.statusText,
        headers,
      });
      return withUosWarning(response, warnings);
    }
    const response = new Response(upstream.body, {
      status: upstream.status,
      statusText: upstream.statusText,
      headers,
    });
    return withUosWarning(response, warnings);
  }

  if (!upstream.body) {
    await recordErrorUsage(usageContext);
    return openaiError(502, "Codex upstream response missing body.", "codex_upstream_missing_body");
  }

  let finalResponse: Record<string, unknown> | null = null;
  for await (const ev of parseSseEvents(upstream.body)) {
    if (!isRecord(ev)) continue;
    if (getString(ev.type) === "response.completed" && isRecord(ev.response)) {
      finalResponse = ev.response;
      break;
    }
  }
  if (!finalResponse) {
    await recordErrorUsage(usageContext);
    return openaiError(502, "Codex upstream stream ended unexpectedly.", "codex_upstream_stream_error");
  }
  const usageTokens = extractUsageTokens(finalResponse.usage);
  await recordCompletionUsage(usageContext, usageTokens);
  const response = json(200, finalResponse, { "x-ubq-upstream": "chatgpt_codex" });
  return withUosWarning(response, warnings);
};
