import { normalizeReasoningEffort, type ReasoningEffort } from "./defaults.ts";
import { getString, isRecord } from "./utils.ts";

export type CodexModelsSnapshot = Readonly<{
  models: Record<string, unknown>[];
  source: string;
  updated_at_ms: number;
  client_version?: string | null;
}>;

export const parseCodexClientVersion = (value: string): [number, number, number] | null => {
  const match = /^(\d+)\.(\d+)\.(\d+)$/.exec(value.trim());
  if (!match) return null;
  const parsed = match.slice(1).map(Number) as [number, number, number];
  return parsed.every((part) => Number.isSafeInteger(part)) ? parsed : null;
};

export const compareCodexClientVersions = (left: string, right: string): number | null => {
  const leftParts = parseCodexClientVersion(left);
  const rightParts = parseCodexClientVersion(right);
  if (!leftParts || !rightParts) return null;
  for (let index = 0; index < leftParts.length; index += 1) {
    if (leftParts[index] !== rightParts[index]) return leftParts[index] - rightParts[index];
  }
  return 0;
};

export const getCodexModelsSnapshotDefaultModel = (snapshot: CodexModelsSnapshot | null): string | null => {
  if (!snapshot || !Array.isArray(snapshot.models)) return null;
  for (const model of snapshot.models) {
    if (!isRecord(model)) continue;
    const id = getString(model.slug) ?? getString(model.id) ?? getString(model.model) ?? getString(model.name);
    const trimmed = id?.trim();
    if (trimmed) return trimmed;
  }
  return null;
};

const isHiddenCodexModel = (value: Record<string, unknown>): boolean =>
  getString(value.visibility)?.trim().toLowerCase() === "hide";

const normalizeNonNegativeInteger = (value: unknown): number | null => {
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  const normalized = Math.trunc(value);
  return normalized >= 0 ? normalized : null;
};

const reasoningLevelEffort = (value: unknown): ReasoningEffort | null => {
  if (value === null) return "none";
  if (typeof value === "string") return normalizeReasoningEffort(value);
  if (!isRecord(value)) return null;
  return value.effort === null ? "none" : normalizeReasoningEffort(value.effort);
};

const deriveReasoningEffortWireMap = (levels: unknown[]): Record<string, ReasoningEffort> => {
  const wireMap = new Map<ReasoningEffort, ReasoningEffort>();

  for (const level of levels) {
    const effort = reasoningLevelEffort(level);
    if (!effort) continue;
    const explicitWireEffort = isRecord(level) ? normalizeReasoningEffort(level.wire_effort) : null;
    const wireEffort: ReasoningEffort = effort === "ultra" ? "max" : explicitWireEffort ?? effort;
    if (wireEffort !== effort) wireMap.set(effort, wireEffort);
  }
  return Object.fromEntries(wireMap);
};

export const normalizeCodexModelsPayload = (
  value: unknown,
  overrides: Readonly<{ source?: string; clientVersion?: string | null; updatedAtMs?: number | null }> = {},
): CodexModelsSnapshot | null => {
  let modelsRaw: unknown = null;
  let source = "codex_cli";
  let clientVersion: string | null = null;
  let updatedAtMs: number | null = null;

  if (Array.isArray(value)) {
    modelsRaw = value;
  } else if (isRecord(value)) {
    if (Array.isArray(value.models)) modelsRaw = value.models;
    else if (Array.isArray(value.data)) modelsRaw = value.data;
    source = getString(value.source) ?? source;
    clientVersion = getString(value.client_version) ?? getString(value.clientVersion);
    if (typeof value.updated_at_ms === "number" && Number.isFinite(value.updated_at_ms)) {
      updatedAtMs = Math.trunc(value.updated_at_ms);
    }
  }
  if (overrides.source) source = overrides.source;
  if (overrides.clientVersion) clientVersion = overrides.clientVersion;
  if (typeof overrides.updatedAtMs === "number" && Number.isFinite(overrides.updatedAtMs)) {
    updatedAtMs = Math.trunc(overrides.updatedAtMs);
  }
  if (!Array.isArray(modelsRaw)) return null;

  const models: Record<string, unknown>[] = [];
  const seen = new Set<string>();
  for (const item of modelsRaw) {
    if (!isRecord(item) || isHiddenCodexModel(item)) continue;
    const slug = getString(item.slug) ?? getString(item.id) ?? getString(item.model) ?? getString(item.name);
    if (!slug || seen.has(slug)) continue;
    const normalized: Record<string, unknown> = { slug };
    const displayName = getString(item.display_name) ?? getString(item.displayName) ?? getString(item.name);
    if (displayName) normalized.display_name = displayName;
    const description = getString(item.description);
    if (description) normalized.description = description;
    const visibility = getString(item.visibility);
    if (visibility) normalized.visibility = visibility;
    if (typeof item.supported_in_api === "boolean") normalized.supported_in_api = item.supported_in_api;
    for (const key of ["context_window", "max_context_window", "auto_compact_token_limit"]) {
      if (item[key] === null) normalized[key] = null;
      else {
        const count = normalizeNonNegativeInteger(item[key]);
        if (count !== null) normalized[key] = count;
      }
    }
    const defaultReasoning = item.default_reasoning_level === null
      ? "none"
      : normalizeReasoningEffort(item.default_reasoning_level);
    if (defaultReasoning) normalized.default_reasoning_level = defaultReasoning;
    if (Array.isArray(item.supported_reasoning_levels)) {
      const levels = item.supported_reasoning_levels.map(reasoningLevelEffort)
        .filter((entry): entry is ReasoningEffort => entry !== null);
      if (!levels.includes("none")) levels.unshift("none");
      if (levels.length) normalized.supported_reasoning_levels = levels;
      const wireMap = deriveReasoningEffortWireMap(item.supported_reasoning_levels);
      if (Object.keys(wireMap).length) normalized.reasoning_effort_wire_map = wireMap;
    }
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
