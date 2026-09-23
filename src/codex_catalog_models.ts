// Codex catalog model list assembly, split out of src/codex_catalog.ts.

import { CODEX_MODELS_KV_KEY, type CodexModelsSnapshot, preserveCodexDefaultModel } from "./codex.ts";
import { compareCodexClientVersions, mergeCodexModelPromptCacheCapabilities, normalizeCodexModelsPayload } from "./codex_models.ts";
import { buildRuntimeConfig, cacheRuntimeConfig, normalizeRuntimeConfig, RUNTIME_CONFIG_V2_KEY, type RuntimeConfigV2 } from "./runtime_config.ts";
import { getString, isRecord } from "./utils.ts";
import { DEEPSEEK_CONTEXT_WINDOW_TOKENS, DEEPSEEK_DISPLAY_NAMES, DEEPSEEK_OFFICIAL_MODEL_IDS, readDeepSeekApiKey } from "./deepseek.ts";
import {
  LITHOS_CONTEXT_WINDOW_TOKENS,
  LITHOS_DEFAULT_REASONING_EFFORT,
  LITHOS_DISPLAY_NAMES,
  LITHOS_EFFECTIVE_CONTEXT_WINDOW_PERCENT,
  LITHOS_MODEL_IDS,
  LITHOS_REASONING_LEVELS,
  readLithosApiKey,
} from "./lithos.ts";
import { codexSnapshotMetadataHint, codexSubscriptionMetadataHint, resolveModelMetadata } from "./model_metadata.ts";
import { CODEX_CATALOG_AUTH_GENERATION_KEY } from "./codex_catalog_types.ts";

const maybeUpdateNormalizedSnapshot = async (
  kv: Deno.Kv,
  version: string,
  authGeneration: string,
  parsed: Record<string, unknown>,
  updatedAtMs: number
): Promise<void> => {
  const next = normalizeCodexModelsPayload(parsed, {
    source: "chatgpt_codex",
    clientVersion: version,
    updatedAtMs,
  });
  if (!next) return;
  for (let attempt = 0; attempt < 3; attempt += 1) {
    const generation = await kv.get<string>(CODEX_CATALOG_AUTH_GENERATION_KEY);
    if (generation.value !== authGeneration) return;
    const [current, runtimeEntry] = await Promise.all([
      kv.get<CodexModelsSnapshot>(CODEX_MODELS_KV_KEY, { consistency: "strong" }),
      kv.get<RuntimeConfigV2>(RUNTIME_CONFIG_V2_KEY, { consistency: "strong" }),
    ]);
    const currentVersion = current.value?.client_version;
    if (currentVersion) {
      const comparison = compareCodexClientVersions(version, currentVersion);
      if (comparison === null || comparison < 0) return;
    }
    const nextWithPromptCacheEvidence = mergeCodexModelPromptCacheCapabilities(next, current.value);
    const currentRuntime = normalizeRuntimeConfig(runtimeEntry.value);
    const nextRuntime = buildRuntimeConfig(nextWithPromptCacheEvidence, {
      defaultModel: preserveCodexDefaultModel(nextWithPromptCacheEvidence, currentRuntime?.default_model),
      defaultReasoningEffort: currentRuntime?.default_reasoning_effort,
      nowMs: updatedAtMs,
    });
    const commit = await kv
      .atomic()
      .check(generation)
      .check(current)
      .check(runtimeEntry)
      .set(CODEX_MODELS_KV_KEY, nextWithPromptCacheEvidence)
      .set(RUNTIME_CONFIG_V2_KEY, nextRuntime)
      .commit();
    if (commit.ok) {
      cacheRuntimeConfig(nextRuntime);
      return;
    }
  }
};

const etagMatches = (requestValue: string | null, etag: string | null): boolean => {
  if (!requestValue || !etag) return false;
  return requestValue.split(",").some((candidate) => candidate.trim() === "*" || candidate.trim() === etag);
};

/**
 * Codex requires a human description next to every advertised effort. The
 * description is a UI label, so a tier discovered dynamically gets a generic one
 * rather than a claim about what the model does.
 */
const codexReasoningEffortDescription = (effort: string): string => (effort === "none" ? "No reasoning" : `Reasoning effort: ${effort}`);

/**
 * Raw uploaded records by id. A Codex-served id must keep the Codex endpoint's
 * own window: discovery and enrichment describe how the gateway can serve the id,
 * not how much context the subscription honors.
 */
const codexSnapshotRecords = (snapshot: CodexModelsSnapshot | null): Map<string, Record<string, unknown>> => {
  const records = new Map<string, Record<string, unknown>>();
  for (const entry of snapshot?.models ?? []) {
    if (!isRecord(entry)) continue;
    const id = (getString(entry.slug) ?? getString(entry.id) ?? getString(entry.model) ?? getString(entry.name))?.trim();
    if (id) records.set(id, entry);
  }
  return records;
};

const meteredCodexModelRecord = (
  model: Readonly<{
    id: string;
    description?: string;
    owned_by: string;
    supported_endpoint_types: readonly string[];
  }>,
  codexRecord: Record<string, unknown> | null = null
) => {
  // A Codex-served id keeps the Codex endpoint's window and tiers, capped by the
  // subscription bound; a paid-only id falls back to discovery plus enrichment.
  // Without this, OpenRouter's API-level maximum (1,050,000 for gpt-6-astra)
  // reached Codex clients as if the subscription served it.
  const resolved = codexRecord
    ? resolveModelMetadata(model.id, { codex: codexSnapshotMetadataHint(codexRecord), codexSubscription: codexSubscriptionMetadataHint() })
    : resolveModelMetadata(model.id);
  const levels = resolved.supported_reasoning_levels;
  return {
    slug: model.id,
    display_name: model.id,
    description: model.description,
    owned_by: model.owned_by,
    supported_endpoint_types: [...model.supported_endpoint_types],
    supported_reasoning_levels: levels?.length
      ? levels.map((effort) => ({ effort, description: codexReasoningEffortDescription(effort) }))
      : [{ effort: "none", description: "No reasoning" }],
    default_reasoning_level: resolved.default_reasoning_effort ?? "none",
    ...(resolved.context_window_tokens === null
      ? {}
      : {
          context_window: resolved.context_window_tokens,
          max_context_window: resolved.max_context_window_tokens,
          ...(resolved.auto_compact_token_limit_tokens === null ? {} : { auto_compact_token_limit: resolved.auto_compact_token_limit_tokens }),
          ...(resolved.effective_context_window_percent === null ? {} : { effective_context_window_percent: resolved.effective_context_window_percent }),
        }),
    shell_type: "shell_command",
    visibility: "list",
    supported_in_api: true,
    priority: 1000,
    availability_nux: null,
    upgrade: null,
    base_instructions: "",
    support_verbosity: false,
    default_verbosity: null,
    apply_patch_tool_type: null,
    web_search_tool_type: "text",
    truncation_policy: { mode: "tokens", limit: 10000 },
    supports_parallel_tool_calls: false,
    experimental_supported_tools: [],
  };
};

const uniqueResponsesModels = <
  T extends Readonly<{
    id: string;
    supported_endpoint_types: readonly string[];
  }>,
>(
  models: readonly T[]
): T[] => {
  const seen = new Set<string>();
  return models.filter((model) => {
    if (!model.supported_endpoint_types.includes("openai-response") || seen.has(model.id)) return false;
    seen.add(model.id);
    return true;
  });
};

/**
 * Codex catalog records for the DeepSeek official route.
 *
 * The official API is Chat Completions only, but the Responses adapter
 * (`src/deepseek_responses.ts`) serves `/v1/responses`, so both ids are
 * advertised as Responses-capable. `deepseek-v4-flash` is the interchangeable
 * legacy id, and both are listed here so the picker shows them and no request
 * falls back to "model metadata not found" defaults.
 */
const deepSeekOfficialCodexModels = (): Record<string, unknown>[] => {
  if (!readDeepSeekApiKey()) return [];
  return DEEPSEEK_OFFICIAL_MODEL_IDS.map((id) => {
    // The route declares one window for both interchangeable ids; anything more
    // specific comes from the dynamic sources.
    const resolved = resolveModelMetadata(id, {
      provider: { context_window_tokens: DEEPSEEK_CONTEXT_WINDOW_TOKENS, max_context_window_tokens: DEEPSEEK_CONTEXT_WINDOW_TOKENS },
    });
    return {
      slug: id,
      display_name: DEEPSEEK_DISPLAY_NAMES[id] ?? id,
      description: "DeepSeek official API (deepseek-flash) served by this gateway.",
      owned_by: "deepseek",
      supported_endpoint_types: ["openai-response", "openai-chat"],
      supported_reasoning_levels: [
        { effort: "none", description: "Disable thinking mode" },
        { effort: "low", description: "Thinking effort: low" },
        { effort: "high", description: "Thinking effort: high" },
        { effort: "max", description: "Thinking effort: maximum" },
      ],
      default_reasoning_level: "high",
      ...(resolved.context_window_tokens === null
        ? {}
        : {
            context_window: resolved.context_window_tokens,
            max_context_window: resolved.max_context_window_tokens,
            ...(resolved.auto_compact_token_limit_tokens === null ? {} : { auto_compact_token_limit: resolved.auto_compact_token_limit_tokens }),
            ...(resolved.effective_context_window_percent === null ? {} : { effective_context_window_percent: resolved.effective_context_window_percent }),
          }),
      shell_type: "shell_command",
      visibility: "list",
      supported_in_api: true,
      priority: 1,
      availability_nux: null,
      upgrade: null,
      base_instructions: "",
      support_verbosity: false,
      default_verbosity: null,
      apply_patch_tool_type: null,
      web_search_tool_type: "text",
      truncation_policy: { mode: "tokens", limit: 10000 },
      supports_parallel_tool_calls: false,
      experimental_supported_tools: [],
    };
  });
};

/**
 * Appends the official ids that the stored catalog does not already advertise.
 * A discovery source may already publish the legacy alias with equivalent
 * tiers, and replacing it would churn that record for no behavioral gain.
 */
const withDeepSeekOfficialModels = (models: readonly Record<string, unknown>[]): Record<string, unknown>[] => {
  const configured = deepSeekOfficialCodexModels();
  if (!configured.length) return [...models];
  const present = new Set(models.map((model) => getString(model.slug) ?? getString(model.id) ?? ""));
  return [...models, ...configured.filter((model) => !present.has(String(model.slug)))];
};

/**
 * Codex catalog records for the LithosAI route.
 *
 * The vendor serves Chat Completions only, but the shared Responses adapter
 * (`src/deepseek_responses.ts`, under `LITHOS_RESPONSES_PROFILE`) serves
 * `/v1/responses`, so every id is advertised as Responses-capable. All eight
 * ids are listed: the `-fast`, `-ultra` and `-ultra-chat` variants are the same
 * weights at higher per-token rates, but they are separately callable ids, so
 * the picker must show each one. The advertised tiers are exactly the seven the
 * provider accepted on 2026-09-23 — no Codex `ultra` preset, which this vendor
 * refuses, is advertised.
 */
const lithosCodexModels = (): Record<string, unknown>[] => {
  if (!readLithosApiKey()) return [];
  return LITHOS_MODEL_IDS.map((id) => {
    const resolved = resolveModelMetadata(id, {
      provider: {
        context_window_tokens: LITHOS_CONTEXT_WINDOW_TOKENS,
        max_context_window_tokens: LITHOS_CONTEXT_WINDOW_TOKENS,
        effective_context_window_percent: LITHOS_EFFECTIVE_CONTEXT_WINDOW_PERCENT,
      },
    });
    return {
      slug: id,
      display_name: LITHOS_DISPLAY_NAMES[id] ?? id,
      description: "LithosAI API (Chat Completions) served by this gateway.",
      owned_by: "lithos",
      supported_endpoint_types: ["openai-response", "openai-chat"],
      supported_reasoning_levels: LITHOS_REASONING_LEVELS.map((effort) => ({ effort, description: codexReasoningEffortDescription(effort) })),
      default_reasoning_level: LITHOS_DEFAULT_REASONING_EFFORT,
      ...(resolved.context_window_tokens === null
        ? {}
        : {
            context_window: resolved.context_window_tokens,
            max_context_window: resolved.max_context_window_tokens,
            ...(resolved.auto_compact_token_limit_tokens === null ? {} : { auto_compact_token_limit: resolved.auto_compact_token_limit_tokens }),
            ...(resolved.effective_context_window_percent === null ? {} : { effective_context_window_percent: resolved.effective_context_window_percent }),
          }),
      shell_type: "shell_command",
      visibility: "list",
      supported_in_api: true,
      priority: 1,
      availability_nux: null,
      upgrade: null,
      base_instructions: "",
      support_verbosity: false,
      default_verbosity: null,
      apply_patch_tool_type: null,
      web_search_tool_type: "text",
      truncation_policy: { mode: "tokens", limit: 10000 },
      supports_parallel_tool_calls: false,
      experimental_supported_tools: [],
    };
  });
};

/**
 * Appends the LithosAI ids the stored catalog does not already advertise. Every
 * one of the eight ids is appended under its own slug, so a client can select
 * any speed tier the provider publishes.
 */
const withLithosModels = (models: readonly Record<string, unknown>[]): Record<string, unknown>[] => {
  const configured = lithosCodexModels();
  if (!configured.length) return [...models];
  const present = new Set(models.map((model) => getString(model.slug) ?? getString(model.id) ?? ""));
  return [...models, ...configured.filter((model) => !present.has(String(model.slug)))];
};

/** Trimmed model slugs advertised by a catalog body, in their stored order. */
const catalogModelIds = (models: readonly unknown[]): Set<string> => {
  const ids = models
    .map((model) => {
      if (!isRecord(model)) return null;
      return (getString(model.slug) ?? getString(model.id) ?? getString(model.model) ?? getString(model.name))?.trim() ?? null;
    })
    .filter((id): id is string => Boolean(id));
  return new Set(ids);
};

/** ETag for a response body, reusing the stored tag when the bytes are unchanged. */

export {
  catalogModelIds,
  codexSnapshotRecords,
  deepSeekOfficialCodexModels,
  etagMatches,
  lithosCodexModels,
  maybeUpdateNormalizedSnapshot,
  meteredCodexModelRecord,
  uniqueResponsesModels,
  withDeepSeekOfficialModels,
  withLithosModels,
};
