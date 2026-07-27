import { normalizeReasoningEffort, type ReasoningEffort } from "./defaults.ts";
import { getString, isRecord } from "./utils.ts";

export type CodexModelsSnapshot = Readonly<{
  models: Record<string, unknown>[];
  source: string;
  updated_at_ms: number;
  client_version?: string | null;
}>;

type PromptCacheControlSource = "catalog" | "live_probe" | "inferred";
type PromptCacheScopeSource = "live_probe";
type PromptCacheMode = "implicit" | "explicit";
type PromptCacheUsageField = "cached_tokens" | "cache_write_tokens";
type PromptCacheAccountSlots = "shared" | "account_scoped" | "unknown";
type PromptCacheTokenRefresh = "preserved" | "changed" | "unknown";
type PromptCacheConversationId = "independent" | "scoped" | "unknown";

export type PromptCacheControls = Readonly<{
  key?: boolean;
  implicit?: boolean;
  explicit_breakpoints?: boolean;
  modes?: PromptCacheMode[];
  ttls?: string[];
  legacy_retentions?: string[];
  breakpoint_block_types?: Readonly<{
    responses?: string[];
    chat_completions?: string[];
  }>;
  expected_usage_fields?: PromptCacheUsageField[];
  source: PromptCacheControlSource;
  verified_at_ms: number;
}>;

export type PromptCacheScope = Readonly<{
  account_slots: PromptCacheAccountSlots;
  token_refresh: PromptCacheTokenRefresh;
  conversation_id: PromptCacheConversationId;
  effective_model?: string;
  reproducible_cycles: number;
  source: PromptCacheScopeSource;
  verified_at_ms: number;
}>;

export type PromptCacheProvider = Readonly<{
  id: string;
  controls?: PromptCacheControls;
  scope?: PromptCacheScope;
}>;

/**
 * `false` is an explicit, verified unsupported result. An omitted field stays
 * unknown so standard OpenAI cache controls continue to pass through.
 */
export type PromptCacheCapabilities =
  | false
  | Readonly<{
    version: 1;
    providers: PromptCacheProvider[];
  }>;

/** Runtime config retains cache controls only; probe scope remains full-catalog evidence. */
export type RuntimePromptCacheCapabilities =
  | false
  | Readonly<{
    version: 1;
    providers: Array<
      Readonly<{
        id: string;
        controls: PromptCacheControls;
      }>
    >;
  }>;

const PROMPT_CACHE_CONTROL_SOURCES = new Set<PromptCacheControlSource>(["catalog", "live_probe", "inferred"]);
const PROMPT_CACHE_SCOPE_SOURCES = new Set<PromptCacheScopeSource>(["live_probe"]);
const PROMPT_CACHE_MODES = new Set<PromptCacheMode>(["implicit", "explicit"]);
const PROMPT_CACHE_USAGE_FIELDS = new Set<PromptCacheUsageField>(["cached_tokens", "cache_write_tokens"]);
const PROMPT_CACHE_ACCOUNT_SLOTS = new Set<PromptCacheAccountSlots>(["shared", "account_scoped", "unknown"]);
const PROMPT_CACHE_TOKEN_REFRESH = new Set<PromptCacheTokenRefresh>(["preserved", "changed", "unknown"]);
const PROMPT_CACHE_CONVERSATION_ID = new Set<PromptCacheConversationId>(["independent", "scoped", "unknown"]);

const isObjectRecord = (value: unknown): value is Record<string, unknown> => isRecord(value) && !Array.isArray(value);

const hasOnlyKeys = (value: Record<string, unknown>, allowed: readonly string[]): boolean => {
  const allowedKeys = new Set(allowed);
  return Object.keys(value).every((key) => allowedKeys.has(key));
};

const hasOwn = (value: Record<string, unknown>, key: string): boolean =>
  Object.prototype.hasOwnProperty.call(value, key);

const normalizePromptCacheString = (value: unknown): string | null => {
  const normalized = getString(value)?.trim();
  return normalized || null;
};

const normalizePromptCacheStringList = (value: unknown): string[] | null => {
  if (!Array.isArray(value)) return null;
  const normalized: string[] = [];
  const seen = new Set<string>();
  for (const item of value) {
    const entry = normalizePromptCacheString(item);
    if (!entry) return null;
    if (!seen.has(entry)) {
      normalized.push(entry);
      seen.add(entry);
    }
  }
  return normalized;
};

const normalizePromptCacheEnumList = <T extends string>(value: unknown, allowed: ReadonlySet<T>): T[] | null => {
  if (!Array.isArray(value)) return null;
  const normalized: T[] = [];
  const seen = new Set<T>();
  for (const item of value) {
    if (typeof item !== "string" || !allowed.has(item as T)) return null;
    const entry = item as T;
    if (!seen.has(entry)) {
      normalized.push(entry);
      seen.add(entry);
    }
  }
  return normalized;
};

const normalizePromptCacheTimestamp = (value: unknown): number | null =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0 ? value : null;

const normalizePromptCacheCycles = (value: unknown): number | null =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0 ? value : null;

const normalizePromptCacheBreakpointBlockTypes = (
  value: unknown,
): PromptCacheControls["breakpoint_block_types"] | null => {
  if (!isObjectRecord(value) || !hasOnlyKeys(value, ["responses", "chat_completions"])) return null;
  const responses = hasOwn(value, "responses") ? normalizePromptCacheStringList(value.responses) : undefined;
  const chatCompletions = hasOwn(value, "chat_completions")
    ? normalizePromptCacheStringList(value.chat_completions)
    : undefined;
  if ((hasOwn(value, "responses") && !responses) || (hasOwn(value, "chat_completions") && !chatCompletions)) {
    return null;
  }
  if (!responses && !chatCompletions) return null;
  return {
    ...(responses ? { responses } : {}),
    ...(chatCompletions ? { chat_completions: chatCompletions } : {}),
  };
};

const normalizePromptCacheControls = (value: unknown): PromptCacheControls | null => {
  if (
    !isObjectRecord(value) ||
    !hasOnlyKeys(value, [
      "key",
      "implicit",
      "explicit_breakpoints",
      "modes",
      "ttls",
      "legacy_retentions",
      "breakpoint_block_types",
      "expected_usage_fields",
      "source",
      "verified_at_ms",
    ])
  ) return null;
  if (!PROMPT_CACHE_CONTROL_SOURCES.has(value.source as PromptCacheControlSource)) return null;
  const verifiedAtMs = normalizePromptCacheTimestamp(value.verified_at_ms);
  if (verifiedAtMs === null) return null;

  const booleans = ["key", "implicit", "explicit_breakpoints"] as const;
  for (const key of booleans) {
    if (hasOwn(value, key) && typeof value[key] !== "boolean") return null;
  }

  const modes = hasOwn(value, "modes") ? normalizePromptCacheEnumList(value.modes, PROMPT_CACHE_MODES) : undefined;
  const ttls = hasOwn(value, "ttls") ? normalizePromptCacheStringList(value.ttls) : undefined;
  const legacyRetentions = hasOwn(value, "legacy_retentions")
    ? normalizePromptCacheStringList(value.legacy_retentions)
    : undefined;
  const breakpointBlockTypes = hasOwn(value, "breakpoint_block_types")
    ? normalizePromptCacheBreakpointBlockTypes(value.breakpoint_block_types)
    : undefined;
  const expectedUsageFields = hasOwn(value, "expected_usage_fields")
    ? normalizePromptCacheEnumList(value.expected_usage_fields, PROMPT_CACHE_USAGE_FIELDS)
    : undefined;
  if (
    (hasOwn(value, "modes") && !modes) ||
    (hasOwn(value, "ttls") && !ttls) ||
    (hasOwn(value, "legacy_retentions") && !legacyRetentions) ||
    (hasOwn(value, "breakpoint_block_types") && !breakpointBlockTypes) ||
    (hasOwn(value, "expected_usage_fields") && !expectedUsageFields)
  ) return null;

  return {
    ...(hasOwn(value, "key") ? { key: value.key as boolean } : {}),
    ...(hasOwn(value, "implicit") ? { implicit: value.implicit as boolean } : {}),
    ...(hasOwn(value, "explicit_breakpoints") ? { explicit_breakpoints: value.explicit_breakpoints as boolean } : {}),
    ...(modes ? { modes } : {}),
    ...(ttls ? { ttls } : {}),
    ...(legacyRetentions ? { legacy_retentions: legacyRetentions } : {}),
    ...(breakpointBlockTypes ? { breakpoint_block_types: breakpointBlockTypes } : {}),
    ...(expectedUsageFields ? { expected_usage_fields: expectedUsageFields } : {}),
    source: value.source as PromptCacheControlSource,
    verified_at_ms: verifiedAtMs,
  };
};

const normalizePromptCacheScope = (value: unknown): PromptCacheScope | null => {
  if (
    !isObjectRecord(value) ||
    !hasOnlyKeys(value, [
      "account_slots",
      "token_refresh",
      "conversation_id",
      "effective_model",
      "reproducible_cycles",
      "source",
      "verified_at_ms",
    ])
  ) return null;
  if (!PROMPT_CACHE_ACCOUNT_SLOTS.has(value.account_slots as PromptCacheAccountSlots)) return null;
  if (!PROMPT_CACHE_TOKEN_REFRESH.has(value.token_refresh as PromptCacheTokenRefresh)) return null;
  if (!PROMPT_CACHE_CONVERSATION_ID.has(value.conversation_id as PromptCacheConversationId)) return null;
  if (!PROMPT_CACHE_SCOPE_SOURCES.has(value.source as PromptCacheScopeSource)) return null;
  const reproducibleCycles = normalizePromptCacheCycles(value.reproducible_cycles);
  const verifiedAtMs = normalizePromptCacheTimestamp(value.verified_at_ms);
  if (reproducibleCycles === null || verifiedAtMs === null) return null;
  if (value.account_slots === "account_scoped" && reproducibleCycles < 3) return null;
  const effectiveModel = hasOwn(value, "effective_model")
    ? normalizePromptCacheString(value.effective_model)
    : undefined;
  if (hasOwn(value, "effective_model") && !effectiveModel) return null;
  return {
    account_slots: value.account_slots as PromptCacheAccountSlots,
    token_refresh: value.token_refresh as PromptCacheTokenRefresh,
    conversation_id: value.conversation_id as PromptCacheConversationId,
    ...(effectiveModel ? { effective_model: effectiveModel } : {}),
    reproducible_cycles: reproducibleCycles,
    source: value.source as PromptCacheScopeSource,
    verified_at_ms: verifiedAtMs,
  };
};

const normalizePromptCacheProvider = (value: unknown): PromptCacheProvider | null => {
  if (!isObjectRecord(value) || !hasOnlyKeys(value, ["id", "controls", "scope"])) return null;
  const id = normalizePromptCacheString(value.id);
  if (!id) return null;
  const controls = hasOwn(value, "controls") ? normalizePromptCacheControls(value.controls) : undefined;
  const scope = hasOwn(value, "scope") ? normalizePromptCacheScope(value.scope) : undefined;
  if ((hasOwn(value, "controls") && !controls) || (hasOwn(value, "scope") && !scope)) return null;
  return {
    id,
    ...(controls ? { controls } : {}),
    ...(scope ? { scope } : {}),
  };
};

/**
 * Normalize the gateway-owned cache capability envelope. This stays separate
 * from OpenAI request schemas; it only describes observed provider behavior.
 */
export const normalizePromptCacheCapabilities = (value: unknown): PromptCacheCapabilities | null => {
  if (value === false) return false;
  if (!isObjectRecord(value) || !hasOnlyKeys(value, ["version", "providers"]) || value.version !== 1) return null;
  if (!Array.isArray(value.providers) || value.providers.length === 0) return null;
  const providers: PromptCacheProvider[] = [];
  const seen = new Set<string>();
  for (const rawProvider of value.providers) {
    const provider = normalizePromptCacheProvider(rawProvider);
    if (!provider || seen.has(provider.id)) return null;
    providers.push(provider);
    seen.add(provider.id);
  }
  return { version: 1, providers };
};

export const compactPromptCacheCapabilities = (value: unknown): RuntimePromptCacheCapabilities | null => {
  const normalized = normalizePromptCacheCapabilities(value);
  if (normalized === null || normalized === false) return normalized;
  const providers: Array<Readonly<{ id: string; controls: PromptCacheControls }>> = [];
  for (const provider of normalized.providers) {
    if (provider.controls) providers.push({ id: provider.id, controls: provider.controls });
  }
  return providers.length ? { version: 1, providers } : null;
};

const modelSlug = (model: Record<string, unknown>): string | null => {
  const slug = getString(model.slug) ?? getString(model.id) ?? getString(model.model) ?? getString(model.name);
  return slug?.trim() || null;
};

const mergePromptCacheProvider = (
  previous: PromptCacheProvider | undefined,
  next: PromptCacheProvider,
): PromptCacheProvider => ({
  id: next.id,
  ...(next.controls ? { controls: next.controls } : previous?.controls ? { controls: previous.controls } : {}),
  ...(next.scope ? { scope: next.scope } : previous?.scope ? { scope: previous.scope } : {}),
});

/**
 * Prefer the incoming catalog's evidence when it supplies it, while retaining
 * cached evidence that a catalog refresh cannot know (for example a live
 * account-scope probe). Provider IDs are deliberately not collapsed.
 */
export const mergePromptCacheCapabilities = (
  previousRaw: unknown,
  nextRaw: unknown,
): PromptCacheCapabilities | null => {
  const previous = normalizePromptCacheCapabilities(previousRaw);
  const next = normalizePromptCacheCapabilities(nextRaw);
  if (next === false) return false;
  if (next === null) return previous;
  if (previous === null || previous === false) return next;

  const previousById = new Map(previous.providers.map((provider) => [provider.id, provider]));
  const providers = next.providers.map((provider) => mergePromptCacheProvider(previousById.get(provider.id), provider));
  const nextIds = new Set(next.providers.map((provider) => provider.id));
  for (const provider of previous.providers) {
    if (!nextIds.has(provider.id)) providers.push(provider);
  }
  return { version: 1, providers };
};

/**
 * Catalog snapshots are refreshed from upstream metadata, while cache evidence
 * can be written independently. Preserve valid evidence only for matching
 * model slugs so removed/renamed models do not inherit stale capabilities.
 */
export const mergeCodexModelPromptCacheCapabilities = (
  next: CodexModelsSnapshot,
  previous: CodexModelsSnapshot | null | undefined,
): CodexModelsSnapshot => {
  if (!previous?.models?.length) return next;
  const previousBySlug = new Map<string, Record<string, unknown>>();
  for (const value of previous.models) {
    if (!isObjectRecord(value)) continue;
    const slug = modelSlug(value);
    if (slug && !previousBySlug.has(slug)) previousBySlug.set(slug, value);
  }

  let changed = false;
  const models = next.models.map((value) => {
    if (!isObjectRecord(value)) return value;
    const slug = modelSlug(value);
    const prior = slug ? previousBySlug.get(slug) : undefined;
    const promptCache = mergePromptCacheCapabilities(prior?.prompt_cache, value.prompt_cache);
    if (promptCache === null) return value;
    changed = true;
    return { ...value, prompt_cache: promptCache };
  });
  return changed ? { ...next, models } : next;
};

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
  getString(value.visibility)?.trim().toLowerCase() === "hide" && value.supported_in_api !== true;

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
    const promptCache = normalizePromptCacheCapabilities(item.prompt_cache);
    if (promptCache !== null) normalized.prompt_cache = promptCache;
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
