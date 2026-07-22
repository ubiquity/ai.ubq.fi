import type { CodexModelsSnapshot } from "./codex_models.ts";
import { getCodexModelsSnapshotDefaultModel } from "./codex_models.ts";
import { DEFAULT_REASONING_EFFORT, normalizeReasoningEffort, type ReasoningEffort } from "./defaults.ts";
import { kvPromise } from "./kv.ts";
import { isRecord } from "./utils.ts";

export const RUNTIME_CONFIG_V2_KEY = ["uos_ai", "runtime_config", "v2"] as const;

export type RuntimeConfigV2 = Readonly<{
  version: 2;
  default_model: string;
  default_reasoning_effort: ReasoningEffort;
  codex_models: CodexModelsSnapshot;
  updated_at_ms: number;
}>;

let cachedRuntimeConfig: RuntimeConfigV2 | null = null;

export const normalizeRuntimeConfig = (value: unknown): RuntimeConfigV2 | null => {
  if (!isRecord(value) || value.version !== 2 || !isRecord(value.codex_models)) return null;
  const snapshot = value.codex_models as CodexModelsSnapshot;
  if (!Array.isArray(snapshot.models) || snapshot.models.length === 0) return null;
  const defaultModel = typeof value.default_model === "string" ? value.default_model.trim() : "";
  const defaultReasoning = normalizeReasoningEffort(value.default_reasoning_effort);
  const updatedAtMs = typeof value.updated_at_ms === "number" && Number.isFinite(value.updated_at_ms)
    ? Math.trunc(value.updated_at_ms)
    : 0;
  if (!defaultModel || !defaultReasoning || updatedAtMs <= 0) return null;
  return {
    version: 2,
    default_model: defaultModel,
    default_reasoning_effort: defaultReasoning,
    codex_models: snapshot,
    updated_at_ms: updatedAtMs,
  };
};

export const buildRuntimeConfig = (
  snapshot: CodexModelsSnapshot,
  options: Readonly<{ defaultModel?: string | null; defaultReasoningEffort?: string | null; nowMs?: number }> = {},
): RuntimeConfigV2 => {
  const defaultModel = options.defaultModel?.trim() || getCodexModelsSnapshotDefaultModel(snapshot);
  if (!defaultModel) throw new Error("runtime config requires a default model");
  return {
    version: 2,
    default_model: defaultModel,
    default_reasoning_effort: normalizeReasoningEffort(options.defaultReasoningEffort) ?? DEFAULT_REASONING_EFFORT,
    codex_models: snapshot,
    updated_at_ms: options.nowMs ?? Date.now(),
  };
};

export const loadRuntimeConfig = async (kvOverride?: Deno.Kv | null): Promise<RuntimeConfigV2 | null> => {
  if (cachedRuntimeConfig) return cachedRuntimeConfig;
  const kv = kvOverride === undefined ? await kvPromise : kvOverride;
  if (!kv) return null;
  const entry = await kv.get<RuntimeConfigV2>(RUNTIME_CONFIG_V2_KEY, { consistency: "strong" });
  cachedRuntimeConfig = normalizeRuntimeConfig(entry.value);
  return cachedRuntimeConfig;
};

export const storeRuntimeConfig = async (
  config: RuntimeConfigV2,
  kvOverride?: Deno.Kv | null,
): Promise<boolean> => {
  const kv = kvOverride === undefined ? await kvPromise : kvOverride;
  if (!kv) return false;
  await kv.set(RUNTIME_CONFIG_V2_KEY, config);
  cachedRuntimeConfig = config;
  return true;
};

export const cacheRuntimeConfig = (config: RuntimeConfigV2): void => {
  cachedRuntimeConfig = config;
};

export const resetRuntimeConfigCacheForTest = (): void => {
  cachedRuntimeConfig = null;
};
