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
import type { ChatCompletionRequest, MessageContentItem, ResponseMessageItem, ResponsesRequest } from "./types.ts";

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
const EMBEDDINGS_CACHE_TTL_MS = 30 * 24 * 60 * 60_000;

const VOYAGE_EMBEDDINGS_URL = "https://api.voyageai.com/v1/embeddings";
const VOYAGE_INPUT_TYPE = "document";
// Voyage free-tier throttles are tiny; we enforce conservative defaults to avoid 429s.
const VOYAGE_RATE_LIMIT_RPM = 3;
const VOYAGE_RATE_LIMIT_TPM = 10_000;
const VOYAGE_RATE_LIMIT_KEY: Deno.KvKey = ["embeddings", "v1", "rate", "voyage"];
const VOYAGE_API_KEY_KV_KEY: Deno.KvKey = ["uos_ai", "voyage_api_key"];

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
    if (waitMs <= 0) continue;
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
  let binary = "";
  for (const b of bytes) binary += String.fromCharCode(b);
  return btoa(binary);
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
}): Promise<number[][]> => {
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
      const text = await resp.text().catch(() => "");
      const snippet = text.trim().slice(0, 800);
      const message = snippet
        ? `Voyage embeddings failed (${resp.status}): ${snippet}`
        : `Voyage embeddings failed (${resp.status}).`;
      const err = new Error(message);
      (err as { status?: number; retry_after_ms?: number }).status = resp.status;
      (err as { retry_after_ms?: number }).retry_after_ms = extractRetryAfterMs(resp.headers.get("Retry-After")) ??
        undefined;
      throw err;
    }

    const payload = await resp.json().catch(() => null) as unknown;
    if (!isRecord(payload) || !Array.isArray(payload.data)) {
      throw new Error("Voyage embeddings returned invalid JSON.");
    }
    const data = payload.data as Array<Record<string, unknown>>;
    const vectors: number[][] = [];
    for (const item of data) {
      const embedding = isRecord(item) ? item.embedding : null;
      if (!Array.isArray(embedding)) {
        throw new Error("Voyage embeddings response missing embedding vector.");
      }
      const vec = embedding.map((v) => Number(v)).filter((v) => Number.isFinite(v));
      if (vec.length !== embedding.length) {
        throw new Error("Voyage embeddings response contained non-numeric values.");
      }
      vectors.push(vec);
    }
    return vectors;
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

  await recordRequestUsage(usageContext, { model, route: "embeddings", stream: false, reasoning: null });

  const deadlineMs = startedAtMs + EMBEDDINGS_TIMEOUT_MS;
  const kv = await kvPromise;
  const apiKey = await readVoyageApiKey(kv);
  if (!apiKey) {
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

  const cacheModelKey = model;
  const cacheKeyFor = (hash: string): Deno.KvKey => ["embeddings", "v1", cacheModelKey, hash];
  const vectorsByIndex: Array<number[] | null> = Array.from({ length: inputs.length }, () => null);

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
        return openaiError(502, "Embeddings request timed out.", "timeout", { type: "server_error", param: null });
      }

      const chunkItems = missing.slice(offset, offset + chunk.length);
      offset += chunk.length;
      const texts = chunkItems.map((item) => item.text);
      const tokenEstimate = estimateTokenCount(texts);

      if (kv) {
        const reserved = await applyVoyageRateLimit(kv, tokenEstimate, deadlineMs);
        if (!reserved.ok) {
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
          vectors = await fetchVoyageEmbeddings({ apiKey, model: resolved.model, inputs: texts, deadlineMs });
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
            return openaiError(502, message, "upstream_error", { type: "server_error", param: null });
          }

          const now = Date.now();
          const waitMs = Math.max(0, retryAfterMs ?? backoffMs);
          if (now + waitMs >= deadlineMs) {
            if (status === 429) {
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
            return openaiError(502, message, "upstream_error", { type: "server_error", param: null });
          }

          await sleep(waitMs);
          backoffMs = Math.min(2000, backoffMs * 2);
          attempt += 1;
        }
      }

      if (!vectors || vectors.length !== chunkItems.length) {
        return openaiError(502, "Embeddings upstream returned a size mismatch.", "upstream_error", {
          type: "server_error",
          param: null,
        });
      }

      const cacheWrites: Promise<unknown>[] = [];
      for (let i = 0; i < chunkItems.length; i += 1) {
        const item = chunkItems[i]!;
        const vec = vectors[i]!;
        for (const idx of item.indices) vectorsByIndex[idx] = vec;
        if (shouldCache && kv) {
          cacheWrites.push(
            kv.set(
              cacheKeyFor(item.hash),
              { embedding: vec, created_at: new Date().toISOString() },
              { expireIn: EMBEDDINGS_CACHE_TTL_MS },
            ),
          );
        }
      }
      if (cacheWrites.length) await Promise.all(cacheWrites);
    }
  }

  const data: Array<{ object: "embedding"; index: number; embedding: number[] | string }> = [];
  for (let i = 0; i < vectorsByIndex.length; i += 1) {
    const vec = vectorsByIndex[i];
    if (!vec) {
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

  return json(200, {
    object: "list",
    data,
    model,
    usage: { prompt_tokens: 0, total_tokens: 0 },
  }, { "x-ubq-upstream": resolved.upstream });
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
  const passthroughKeys = ["tools", "tool_choice", "parallel_tool_calls", "prompt_cache_key"];
  for (const key of passthroughKeys) {
    if (Object.prototype.hasOwnProperty.call(rawRecord, key)) {
      codexBody[key] = rawRecord[key];
    }
  }
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
  let input: ResponseMessageItem[];
  if (inputRaw === undefined) {
    input = [];
  } else if (typeof inputRaw === "string") {
    input = [{ type: "message", role: "user", content: [{ type: "input_text", text: inputRaw }] }];
  } else if (Array.isArray(inputRaw)) {
    const converted: ResponseMessageItem[] = [];
    let contentBuffer: MessageContentItem[] = [];

    const flushContentBuffer = () => {
      if (!contentBuffer.length) return;
      converted.push({ type: "message", role: "user", content: contentBuffer });
      contentBuffer = [];
    };

    let sawMessage = false;
    for (const msg of inputRaw) {
      const mapped = normalizeResponseInputItem(msg);
      if (mapped) {
        flushContentBuffer();
        converted.push(mapped);
        sawMessage = true;
        continue;
      }
      const contentItem = normalizeResponseContentItem(msg);
      if (!contentItem) return openaiError(400, "Invalid message in input[]", "invalid_request_error");
      if (sawMessage) {
        converted.push({ type: "message", role: "user", content: [contentItem] });
      } else {
        contentBuffer.push(contentItem);
      }
    }
    if (!sawMessage || contentBuffer.length) {
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
  const passthroughKeys = ["tools", "tool_choice", "parallel_tool_calls", "prompt_cache_key", "text", "include"];
  for (const key of passthroughKeys) {
    if (Object.prototype.hasOwnProperty.call(rawRecord, key)) {
      codexBody[key] = rawRecord[key];
    }
  }
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
