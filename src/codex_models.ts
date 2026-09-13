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
export type PromptCacheAccountSlots = "shared" | "account_scoped" | "unknown";
export type PromptCacheTokenRefresh = "preserved" | "changed" | "unknown";
export type PromptCacheConversationId = "independent" | "scoped" | "unknown";

/**
 * Scope evidence is valid only for this fixed v5 implicit Responses request
 * shape. Keep this literal immutable so an earlier experiment definition,
 * plain-key observation, explicit-mode request, or provider-wide result cannot
 * be read as current scope evidence.
 */
export const PROMPT_CACHE_SCOPE_PROBE_PROFILE = "responses_implicit_input_text_keyed_cycle_isolated_v5" as const;
export type PromptCacheScopeProbeProfile = typeof PROMPT_CACHE_SCOPE_PROBE_PROFILE;

/** The capability-catalog identity for the ChatGPT Codex transport. */
export const CODEX_CHATGPT_PROMPT_CACHE_PROVIDER = "codex_chatgpt" as const;

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
  probe_profile: PromptCacheScopeProbeProfile;
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
      providers: Readonly<{
        id: string;
        controls: PromptCacheControls;
      }>[];
    }>;

const PROMPT_CACHE_CONTROL_SOURCES = new Set<PromptCacheControlSource>(["catalog", "live_probe", "inferred"]);
const PROMPT_CACHE_SCOPE_SOURCES = new Set<PromptCacheScopeSource>(["live_probe"]);
const PROMPT_CACHE_MODES = new Set<PromptCacheMode>(["implicit", "explicit"]);
const PROMPT_CACHE_USAGE_FIELDS = new Set<PromptCacheUsageField>(["cached_tokens", "cache_write_tokens"]);
// These are the public controls that the gateway accepts in openai.ts. Cache
// capability metadata is a gateway-compatibility projection, so it must not
// publish a value that the public request validator will reject.
const PROMPT_CACHE_TTLS = new Set(["30m"]);
const PROMPT_CACHE_LEGACY_RETENTIONS = new Set(["in_memory", "24h"]);
// The official Chat contract permits additional marked blocks, but the
// Chat-to-Responses adapter currently has no lossless upstream representation
// for them. Keep published/enforced capabilities aligned with the subset this
// gateway can preserve rather than advertising a marker it will fail closed.
const PROMPT_CACHE_RESPONSE_BREAKPOINT_BLOCK_TYPES = new Set(["input_text", "input_image", "input_file"]);
const PROMPT_CACHE_CHAT_BREAKPOINT_BLOCK_TYPES = new Set(["text", "image_url", "file"]);
const PROMPT_CACHE_ACCOUNT_SLOTS = new Set<PromptCacheAccountSlots>(["shared", "account_scoped", "unknown"]);
const PROMPT_CACHE_TOKEN_REFRESH = new Set<PromptCacheTokenRefresh>(["preserved", "changed", "unknown"]);
const PROMPT_CACHE_CONVERSATION_ID = new Set<PromptCacheConversationId>(["independent", "scoped", "unknown"]);

const isObjectRecord = (value: unknown): value is Record<string, unknown> => isRecord(value) && !Array.isArray(value);

const hasOnlyKeys = (value: Record<string, unknown>, allowed: readonly string[]): boolean => {
  const allowedKeys = new Set(allowed);
  return Object.keys(value).every((key) => allowedKeys.has(key));
};

const hasOwn = (value: Record<string, unknown>, key: string): boolean => Object.prototype.hasOwnProperty.call(value, key);

const normalizePromptCacheString = (value: unknown): string | null => {
  const normalized = getString(value)?.trim();
  if (!normalized) return null;
  return normalized;
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

/**
 * Drop catalog values the public gateway cannot accept while retaining the
 * rest of otherwise valid capability evidence. An empty result is either
 * omitted as an unknown scalar control or invalidates an exhaustive block-type
 * declaration, depending on the caller's compatibility semantics.
 */
const normalizeGatewayPromptCacheStringList = (value: unknown, allowed: ReadonlySet<string>): string[] | null => {
  const normalized = normalizePromptCacheStringList(value);
  if (normalized === null) return null;
  return normalized.filter((entry) => allowed.has(entry));
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

const normalizePromptCacheCycles = (value: unknown): number | null => (typeof value === "number" && Number.isSafeInteger(value) && value >= 0 ? value : null);

const normalizePromptCacheBreakpointBlockTypes = (value: unknown): PromptCacheControls["breakpoint_block_types"] | null => {
  if (!isObjectRecord(value) || !hasOnlyKeys(value, ["responses", "chat_completions"])) return null;
  const responses = hasOwn(value, "responses")
    ? normalizeGatewayPromptCacheStringList(value.responses, PROMPT_CACHE_RESPONSE_BREAKPOINT_BLOCK_TYPES)
    : undefined;
  const chatCompletions = hasOwn(value, "chat_completions")
    ? normalizeGatewayPromptCacheStringList(value.chat_completions, PROMPT_CACHE_CHAT_BREAKPOINT_BLOCK_TYPES)
    : undefined;
  if ((hasOwn(value, "responses") && !responses) || (hasOwn(value, "chat_completions") && !chatCompletions)) {
    return null;
  }
  const supportedResponses = responses?.length ? responses : undefined;
  const supportedChatCompletions = chatCompletions?.length ? chatCompletions : undefined;
  if (!supportedResponses && !supportedChatCompletions) return null;
  return {
    ...(supportedResponses ? { responses: supportedResponses } : {}),
    ...(supportedChatCompletions ? { chat_completions: supportedChatCompletions } : {}),
  };
};

const PROMPT_CACHE_CONTROLS_KEYS = [
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
] as const;
const PROMPT_CACHE_CONTROLS_BOOLEAN_KEYS = ["key", "implicit", "explicit_breakpoints"] as const;

/** An optional control field: absent, or present with the normalizer's verdict. */
type PromptCacheControlField<T> = Readonly<{ present: boolean; value: T | null }>;

type PromptCacheControlFields = Readonly<{
  modes: PromptCacheControlField<PromptCacheMode[]>;
  ttls: PromptCacheControlField<string[]>;
  legacyRetentions: PromptCacheControlField<string[]>;
  breakpointBlockTypes: PromptCacheControlField<PromptCacheControls["breakpoint_block_types"]>;
  expectedUsageFields: PromptCacheControlField<PromptCacheUsageField[]>;
}>;

const readPromptCacheControlField = <T>(value: Record<string, unknown>, key: string, normalize: (raw: unknown) => T | null): PromptCacheControlField<T> => {
  if (!hasOwn(value, key)) return { present: false, value: null };
  return { present: true, value: normalize(value[key]) };
};

/** Presence was validated as a boolean, so absence is the only non-boolean case left. */
const readPromptCacheControlBoolean = (value: Record<string, unknown>, key: string): boolean | undefined => {
  const raw = value[key];
  return typeof raw === "boolean" ? raw : undefined;
};

const buildPromptCacheControls = (value: Record<string, unknown>, fields: PromptCacheControlFields, verifiedAtMs: number): PromptCacheControls => {
  const key = readPromptCacheControlBoolean(value, "key");
  const implicit = readPromptCacheControlBoolean(value, "implicit");
  const explicitBreakpoints = readPromptCacheControlBoolean(value, "explicit_breakpoints");
  return {
    ...(key !== undefined ? { key } : {}),
    ...(implicit !== undefined ? { implicit } : {}),
    ...(explicitBreakpoints !== undefined ? { explicit_breakpoints: explicitBreakpoints } : {}),
    ...(fields.modes.value ? { modes: fields.modes.value } : {}),
    ...(fields.ttls.value?.length ? { ttls: fields.ttls.value } : {}),
    ...(fields.legacyRetentions.value?.length ? { legacy_retentions: fields.legacyRetentions.value } : {}),
    ...(fields.breakpointBlockTypes.value ? { breakpoint_block_types: fields.breakpointBlockTypes.value } : {}),
    ...(fields.expectedUsageFields.value ? { expected_usage_fields: fields.expectedUsageFields.value } : {}),
    source: value.source as PromptCacheControlSource,
    verified_at_ms: verifiedAtMs,
  };
};

const normalizePromptCacheControls = (value: unknown): PromptCacheControls | null => {
  if (!isObjectRecord(value) || !hasOnlyKeys(value, PROMPT_CACHE_CONTROLS_KEYS)) return null;
  if (!PROMPT_CACHE_CONTROL_SOURCES.has(value.source as PromptCacheControlSource)) return null;
  const verifiedAtMs = normalizePromptCacheTimestamp(value.verified_at_ms);
  if (verifiedAtMs === null) return null;
  if (!PROMPT_CACHE_CONTROLS_BOOLEAN_KEYS.every((key) => !hasOwn(value, key) || typeof value[key] === "boolean")) return null;

  const fields: PromptCacheControlFields = {
    modes: readPromptCacheControlField(value, "modes", (raw: unknown) => normalizePromptCacheEnumList(raw, PROMPT_CACHE_MODES)),
    ttls: readPromptCacheControlField(value, "ttls", (raw: unknown) => normalizeGatewayPromptCacheStringList(raw, PROMPT_CACHE_TTLS)),
    legacyRetentions: readPromptCacheControlField(value, "legacy_retentions", (raw: unknown) =>
      normalizeGatewayPromptCacheStringList(raw, PROMPT_CACHE_LEGACY_RETENTIONS)
    ),
    breakpointBlockTypes: readPromptCacheControlField(value, "breakpoint_block_types", normalizePromptCacheBreakpointBlockTypes),
    expectedUsageFields: readPromptCacheControlField(value, "expected_usage_fields", (raw: unknown) =>
      normalizePromptCacheEnumList(raw, PROMPT_CACHE_USAGE_FIELDS)
    ),
  };
  // A declared field that fails its own normalizer invalidates the whole record,
  // exactly as an exhaustive block-type declaration does.
  if (Object.values(fields).some((field) => field.present && field.value === null)) return null;
  return buildPromptCacheControls(value, fields, verifiedAtMs);
};

const normalizePromptCacheScope = (value: unknown): PromptCacheScope | null => {
  if (
    !isObjectRecord(value) ||
    !hasOnlyKeys(value, [
      "probe_profile",
      "account_slots",
      "token_refresh",
      "conversation_id",
      "effective_model",
      "reproducible_cycles",
      "source",
      "verified_at_ms",
    ])
  )
    return null;
  if (value.probe_profile !== PROMPT_CACHE_SCOPE_PROBE_PROFILE) return null;
  if (!PROMPT_CACHE_ACCOUNT_SLOTS.has(value.account_slots as PromptCacheAccountSlots)) return null;
  if (!PROMPT_CACHE_TOKEN_REFRESH.has(value.token_refresh as PromptCacheTokenRefresh)) return null;
  if (!PROMPT_CACHE_CONVERSATION_ID.has(value.conversation_id as PromptCacheConversationId)) return null;
  if (!PROMPT_CACHE_SCOPE_SOURCES.has(value.source as PromptCacheScopeSource)) return null;
  const reproducibleCycles = normalizePromptCacheCycles(value.reproducible_cycles);
  const verifiedAtMs = normalizePromptCacheTimestamp(value.verified_at_ms);
  if (reproducibleCycles === null || verifiedAtMs === null) return null;
  // Scope is only published after the bounded live probe has reproduced the
  // complete classification. An unverified "shared" result is no safer than
  // an unverified "account_scoped" result, and callers should treat omitted
  // scope as unknown rather than consuming an early classification.
  if (reproducibleCycles < 3) return null;
  const effectiveModel = hasOwn(value, "effective_model") ? normalizePromptCacheString(value.effective_model) : undefined;
  if (hasOwn(value, "effective_model") && !effectiveModel) return null;
  return {
    probe_profile: PROMPT_CACHE_SCOPE_PROBE_PROFILE,
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
  const providers: Readonly<{ id: string; controls: PromptCacheControls }>[] = [];
  for (const provider of normalized.providers) {
    if (provider.controls) providers.push({ id: provider.id, controls: provider.controls });
  }
  return providers.length ? { version: 1, providers } : null;
};

const modelSlug = (model: Record<string, unknown>): string | null => {
  const slug = getString(model.slug) ?? getString(model.id) ?? getString(model.model) ?? getString(model.name);
  const trimmed = slug?.trim();
  if (!trimmed) return null;
  return trimmed;
};

const isCodexModelWithSlug = (value: unknown, slug: string): boolean => isObjectRecord(value) && modelSlug(value) === slug;

/**
 * Snapshot writers use this exact-slug lookup before publishing probe
 * evidence. A duplicate or renamed entry is intentionally indistinguishable
 * from absence: neither is safe to update from a prior live observation.
 */
export const getUniqueCodexModelBySlug = (snapshot: CodexModelsSnapshot, slug: string): Record<string, unknown> | null => {
  const target = slug.trim();
  if (!target || !Array.isArray(snapshot.models)) return null;
  const matches = snapshot.models.filter((model) => isCodexModelWithSlug(model, target));
  if (matches.length !== 1) return null;
  return matches.at(0) ?? null;
};

export const getCodexModelPromptCacheProvider = (snapshot: CodexModelsSnapshot, slug: string, providerId: string): PromptCacheProvider | null => {
  const model = getUniqueCodexModelBySlug(snapshot, slug);
  if (!model) return null;
  const promptCache = normalizePromptCacheCapabilities(model.prompt_cache);
  if (promptCache === null || promptCache === false) return null;
  return promptCache.providers.find((provider) => provider.id === providerId) ?? null;
};

/** Exact controls required before the fixed plain-key scope matrix may dispatch. */
export const isCodexModelPromptCacheScopeExperimentEligible = (snapshot: CodexModelsSnapshot, slug: string): boolean => {
  const controls = getCodexModelPromptCacheProvider(snapshot, slug, CODEX_CHATGPT_PROMPT_CACHE_PROVIDER)?.controls;
  const expectedUsageFields = controls?.expected_usage_fields;
  return Boolean(
    controls?.key === true &&
    controls.implicit !== false &&
    // The fixed profile uses only prompt_cache_key. `modes` describes the
    // optional prompt_cache_options field, so an explicit-only options
    // declaration cannot disqualify this distinct request shape.
    expectedUsageFields?.includes("cached_tokens") &&
    expectedUsageFields.includes("cache_write_tokens")
  );
};

/** A scope may be schema-valid but still unsafe to publish from a live probe. */
export const isConcretePromptCacheScope = (scope: PromptCacheScope, reproducibleCycles = 3): boolean => {
  // `probe_profile` and `source` are single literals at the type level, but this
  // predicate also guards admin-supplied scope records, whose runtime values can
  // still disagree with the declared type. Comparing through widened locals keeps
  // the guards without asking the compiler to prove them constant.
  const probeProfile: string = scope.probe_profile;
  const source: string = scope.source;
  return (
    probeProfile === PROMPT_CACHE_SCOPE_PROBE_PROFILE &&
    scope.account_slots !== "unknown" &&
    scope.token_refresh !== "unknown" &&
    scope.conversation_id !== "unknown" &&
    scope.reproducible_cycles === reproducibleCycles &&
    source === "live_probe" &&
    Boolean(scope.effective_model?.trim())
  );
};

/**
 * Return a copy with scope evidence attached to one already-present provider.
 * Controls and all other provider records remain byte-for-byte catalog-owned.
 */
export const withCodexModelPromptCacheScope = (
  snapshot: CodexModelsSnapshot,
  slug: string,
  providerId: string,
  scope: PromptCacheScope
): CodexModelsSnapshot | null => {
  const target = slug.trim();
  const model = getUniqueCodexModelBySlug(snapshot, target);
  const normalizedScope = normalizePromptCacheScope(scope);
  if (!model || !normalizedScope) return null;
  const promptCache = normalizePromptCacheCapabilities(model.prompt_cache);
  if (promptCache === null || promptCache === false) return null;
  if (!promptCache.providers.some((provider) => provider.id === providerId)) return null;

  const providers = promptCache.providers.map((provider) => (provider.id === providerId ? { ...provider, scope: normalizedScope } : provider));
  const nextPromptCache = normalizePromptCacheCapabilities({ version: 1, providers });
  if (nextPromptCache === null || nextPromptCache === false) return null;

  if (!snapshot.models.some((candidate) => isCodexModelWithSlug(candidate, target))) return null;
  const models = snapshot.models.map((candidate) => (isCodexModelWithSlug(candidate, target) ? { ...candidate, prompt_cache: nextPromptCache } : candidate));
  return { ...snapshot, models };
};

const mergePromptCacheProvider = (previous: PromptCacheProvider | undefined, next: PromptCacheProvider): PromptCacheProvider => {
  const controls = next.controls ?? previous?.controls;
  const scope = next.scope ?? previous?.scope;
  return {
    id: next.id,
    ...(controls ? { controls } : {}),
    ...(scope ? { scope } : {}),
  };
};

/**
 * Prefer the incoming catalog's evidence when it supplies it, while retaining
 * cached evidence that a catalog refresh cannot know (for example a live
 * account-scope probe). Provider IDs are deliberately not collapsed.
 */
export const mergePromptCacheCapabilities = (previousRaw: unknown, nextRaw: unknown): PromptCacheCapabilities | null => {
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

/** Returns the same entry when the merge adds nothing, so callers can detect changes by reference. */
const mergeCodexModelPromptCache = (value: Record<string, unknown>, previousBySlug: ReadonlyMap<string, Record<string, unknown>>): Record<string, unknown> => {
  if (!isObjectRecord(value)) return value;
  const slug = modelSlug(value);
  const prior = slug ? previousBySlug.get(slug) : undefined;
  const promptCache = mergePromptCacheCapabilities(prior?.prompt_cache, value.prompt_cache);
  if (promptCache === null) return value;
  return { ...value, prompt_cache: promptCache };
};

/**
 * Catalog snapshots are refreshed from upstream metadata, while cache evidence
 * can be written independently. Preserve valid evidence only for matching
 * model slugs so removed/renamed models do not inherit stale capabilities.
 */
export const mergeCodexModelPromptCacheCapabilities = (next: CodexModelsSnapshot, previous: CodexModelsSnapshot | null | undefined): CodexModelsSnapshot => {
  // `models` is required by the snapshot type, but the previous snapshot is read
  // back from a stored record, so the shape is validated rather than trusted.
  const previousModels = previous?.models;
  if (!Array.isArray(previousModels) || previousModels.length === 0) return next;
  const previousBySlug = new Map<string, Record<string, unknown>>();
  for (const value of previousModels) {
    if (!isObjectRecord(value)) continue;
    const slug = modelSlug(value);
    if (slug && !previousBySlug.has(slug)) previousBySlug.set(slug, value);
  }

  const models = next.models.map((value) => mergeCodexModelPromptCache(value, previousBySlug));
  if (models.every((model, index) => model === next.models[index])) return next;
  return { ...next, models };
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
    const wireEffort: ReasoningEffort = effort === "ultra" ? "max" : (explicitWireEffort ?? effort);
    if (wireEffort !== effort) wireMap.set(effort, wireEffort);
  }
  return Object.fromEntries(wireMap);
};

const CODEX_MODELS_DEFAULT_SOURCE = "codex_cli";
const CODEX_MODEL_CONTEXT_WINDOW_KEYS = ["context_window", "max_context_window", "auto_compact_token_limit", "effective_context_window_percent"] as const;

const normalizeCodexModelsUpdatedAtMs = (value: unknown): number | null => (typeof value === "number" && Number.isFinite(value) ? Math.trunc(value) : null);

type CodexModelsPayloadEnvelope = Readonly<{
  modelsRaw: unknown;
  source: string;
  clientVersion: string | null;
  updatedAtMs: number | null;
}>;

const normalizeCodexModelsEnvelope = (value: unknown): CodexModelsPayloadEnvelope => {
  if (Array.isArray(value)) {
    return { modelsRaw: value, source: CODEX_MODELS_DEFAULT_SOURCE, clientVersion: null, updatedAtMs: null };
  }
  if (!isRecord(value)) {
    return { modelsRaw: null, source: CODEX_MODELS_DEFAULT_SOURCE, clientVersion: null, updatedAtMs: null };
  }
  let modelsRaw: unknown = null;
  if (Array.isArray(value.models)) modelsRaw = value.models;
  else if (Array.isArray(value.data)) modelsRaw = value.data;
  return {
    modelsRaw,
    source: getString(value.source) ?? CODEX_MODELS_DEFAULT_SOURCE,
    clientVersion: getString(value.client_version) ?? getString(value.clientVersion),
    updatedAtMs: normalizeCodexModelsUpdatedAtMs(value.updated_at_ms),
  };
};

/** Copies the context-window fields in list order, so catalog key order is stable. */
const applyCodexModelContextWindowFields = (item: Record<string, unknown>, normalized: Record<string, unknown>): void => {
  for (const key of CODEX_MODEL_CONTEXT_WINDOW_KEYS) {
    if (item[key] === null) normalized[key] = null;
    else {
      const count = normalizeNonNegativeInteger(item[key]);
      if (count !== null) normalized[key] = count;
    }
  }
};

const applyCodexModelReasoningFields = (item: Record<string, unknown>, normalized: Record<string, unknown>): void => {
  const defaultReasoning = item.default_reasoning_level === null ? "none" : normalizeReasoningEffort(item.default_reasoning_level);
  if (defaultReasoning) normalized.default_reasoning_level = defaultReasoning;
  // Every non-empty advertised tier is preserved: the uploaded catalog is the
  // source of truth, and `none` is the only gateway-known special case.
  const rawLevels = item.supported_reasoning_levels;
  if (!Array.isArray(rawLevels)) return;
  const levels = rawLevels.map(reasoningLevelEffort).filter((entry): entry is ReasoningEffort => entry !== null);
  if (!levels.includes("none")) levels.unshift("none");
  if (levels.length) normalized.supported_reasoning_levels = levels;
  const wireMap = deriveReasoningEffortWireMap(rawLevels);
  if (Object.keys(wireMap).length) normalized.reasoning_effort_wire_map = wireMap;
};

const normalizeCodexModelEntry = (item: Record<string, unknown>, slug: string): Record<string, unknown> => {
  const normalized: Record<string, unknown> = { slug };
  const displayName = getString(item.display_name) ?? getString(item.displayName) ?? getString(item.name);
  if (displayName) normalized.display_name = displayName;
  const description = getString(item.description);
  if (description) normalized.description = description;
  const visibility = getString(item.visibility);
  if (visibility) normalized.visibility = visibility;
  if (typeof item.supported_in_api === "boolean") normalized.supported_in_api = item.supported_in_api;
  applyCodexModelContextWindowFields(item, normalized);
  applyCodexModelReasoningFields(item, normalized);
  const promptCache = normalizePromptCacheCapabilities(item.prompt_cache);
  if (promptCache !== null) normalized.prompt_cache = promptCache;
  return normalized;
};

export const normalizeCodexModelsPayload = (
  value: unknown,
  overrides: Readonly<{ source?: string; clientVersion?: string | null; updatedAtMs?: number | null }> = {}
): CodexModelsSnapshot | null => {
  const envelope = normalizeCodexModelsEnvelope(value);
  const modelsRaw = envelope.modelsRaw;
  if (!Array.isArray(modelsRaw)) return null;

  const models: Record<string, unknown>[] = [];
  const seen = new Set<string>();
  for (const item of modelsRaw) {
    if (!isRecord(item) || isHiddenCodexModel(item)) continue;
    const slug = getString(item.slug) ?? getString(item.id) ?? getString(item.model) ?? getString(item.name);
    if (!slug || seen.has(slug)) continue;
    models.push(normalizeCodexModelEntry(item, slug));
    seen.add(slug);
  }
  if (models.length === 0) return null;

  const updatedAtMs = normalizeCodexModelsUpdatedAtMs(overrides.updatedAtMs) ?? envelope.updatedAtMs;
  // Only a non-empty override replaces the payload's own field, matching the
  // original truthiness rule; `??` would let an empty string win.
  let source = envelope.source;
  if (overrides.source) source = overrides.source;
  let clientVersion = envelope.clientVersion;
  if (overrides.clientVersion) clientVersion = overrides.clientVersion;
  return {
    models,
    source,
    updated_at_ms: updatedAtMs ?? Date.now(),
    client_version: clientVersion ?? undefined,
  };
};
