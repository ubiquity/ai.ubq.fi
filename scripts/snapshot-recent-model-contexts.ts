import { getRecentModelReasoning } from "../static/reasoning-select.js";

type JsonRecord = Record<string, unknown>;

type ProviderCapability = Readonly<{
  provider: "codex" | "openlux" | "surplus";
  matched_model_id: string;
  max_context_window_tokens: number | null;
  context_window_tokens: number | null;
  max_output_tokens: number | null;
  auto_compact_token_limit_tokens: number | null;
}>;

type ResolvedCapability = Readonly<{
  model_class: string;
  max_context_window_tokens: number | null;
  context_window_tokens: number | null;
  max_output_tokens: number | null;
  auto_compact_token_limit_tokens: number | null;
  sources: readonly ProviderCapability[];
}>;

const CATALOG_URL = "https://ai.ubq.fi/uos/models/catalog";
const CAPABILITIES_URL = "https://ai.ubq.fi/uos/models/capabilities";
const SURPLUS_MODELS_URL = "https://api.surplusintelligence.ai/v1/models";
const OPENLUX_MODELS_URL = "https://api.openlux.ai/v1/models";
const REASONING_SELECT_PATH = "static/reasoning-select.js";
const MODELS_PAGE_PATH = "static/models.js";
const AUDIT_PATH = "docs/recent-model-context-audit.md";
const GENERATED_START = "// BEGIN GENERATED RECENT MODEL CONTEXTS";
const GENERATED_END = "// END GENERATED RECENT MODEL CONTEXTS";

const isRecord = (value: unknown): value is JsonRecord =>
  value !== null && typeof value === "object" && !Array.isArray(value);

const nonEmptyString = (value: unknown): string | null => {
  if (typeof value !== "string") return null;
  const trimmed = value.trim();
  return trimmed || null;
};

const positiveInteger = (value: unknown): number | null => {
  const number = typeof value === "string" && value.trim() ? Number(value) : value;
  if (typeof number !== "number" || !Number.isFinite(number) || number <= 0) return null;
  return Math.trunc(number);
};

const valueAt = (record: JsonRecord, path: readonly string[]): unknown => {
  let value: unknown = record;
  for (const part of path) {
    if (!isRecord(value)) return undefined;
    value = value[part];
  }
  return value;
};

const firstInteger = (record: JsonRecord, paths: readonly (readonly string[])[]): number | null => {
  for (const path of paths) {
    const value = positiveInteger(valueAt(record, path));
    if (value !== null) return value;
  }
  return null;
};

const modelId = (record: JsonRecord): string | null =>
  nonEmptyString(record.id) ?? nonEmptyString(record.slug) ?? nonEmptyString(record.model) ??
  nonEmptyString(record.name);

const modelArray = (payload: unknown): JsonRecord[] => {
  if (!isRecord(payload)) return [];
  const values = Array.isArray(payload.data) ? payload.data : Array.isArray(payload.models) ? payload.models : [];
  return values.filter(isRecord);
};

const fetchJson = async (
  url: string,
  headers: HeadersInit = {},
  required = false,
): Promise<unknown | null> => {
  try {
    const response = await fetch(url, {
      headers: { Accept: "application/json", ...headers },
      signal: AbortSignal.timeout(30_000),
    });
    if (!response.ok) {
      if (required) throw new Error(`${url} returned HTTP ${response.status}`);
      return null;
    }
    return await response.json() as unknown;
  } catch (error) {
    if (required) throw error;
    return null;
  }
};

const normalizedId = (value: string): string => value.trim().toLowerCase();

const idVariants = (value: string): Set<string> => {
  const normalized = normalizedId(value);
  const variants = new Set([normalized]);
  const slash = normalized.lastIndexOf("/");
  if (slash >= 0 && slash + 1 < normalized.length) variants.add(normalized.slice(slash + 1));
  return variants;
};

const CONTEXT_PATHS = [
  ["max_context_window_tokens"],
  ["max_context_window"],
  ["max_context_length"],
  ["context_length"],
  ["context_window"],
  ["context_window_tokens"],
  ["max_model_len"],
  ["top_provider", "context_length"],
  ["limits", "context_length"],
  ["capabilities", "context_length"],
  ["architecture", "context_length"],
] as const;

const USABLE_CONTEXT_PATHS = [
  ["input_context_window_tokens"],
  ["max_input_tokens"],
  ["input_token_limit"],
  ["max_prompt_tokens"],
] as const;

const OUTPUT_PATHS = [
  ["max_output_tokens"],
  ["max_completion_tokens"],
  ["output_token_limit"],
  ["top_provider", "max_completion_tokens"],
  ["limits", "max_output_tokens"],
  ["capabilities", "max_output_tokens"],
] as const;

const COMPACT_PATHS = [
  ["auto_compact_token_limit_tokens"],
  ["auto_compact_token_limit"],
] as const;

const capabilityFromRecord = (
  provider: ProviderCapability["provider"],
  record: JsonRecord,
): ProviderCapability | null => {
  const id = modelId(record);
  if (!id) return null;

  let maxContext = firstInteger(record, CONTEXT_PATHS);
  let usableContext = firstInteger(record, USABLE_CONTEXT_PATHS);
  let maxOutput = firstInteger(record, OUTPUT_PATHS);
  const explicitCompact = firstInteger(record, COMPACT_PATHS);

  if (provider === "codex") {
    usableContext = positiveInteger(record.context_window_tokens) ?? usableContext;
    maxContext = positiveInteger(record.max_context_window_tokens) ?? maxContext ?? usableContext;
    if (maxOutput === null && maxContext !== null && usableContext !== null && maxContext > usableContext) {
      maxOutput = maxContext - usableContext;
    }
  } else if (usableContext === null && maxContext !== null) {
    usableContext = maxOutput !== null && maxContext > maxOutput ? maxContext - maxOutput : maxContext;
  }

  if (maxContext === null && usableContext !== null) {
    maxContext = maxOutput === null ? usableContext : usableContext + maxOutput;
  }
  if (maxContext === null && usableContext === null) return null;

  // Conversation compaction is based on prompt/history tokens. When a provider
  // documents a total context window and an output ceiling, reserve that full
  // output allowance first. Trigger at 90% of the remaining prompt budget so
  // token-estimation drift and the next tool turn still have headroom.
  const autoCompact = explicitCompact ??
    (usableContext === null ? null : Math.max(1, Math.floor(usableContext * 0.9)));

  return {
    provider,
    matched_model_id: id,
    max_context_window_tokens: maxContext,
    context_window_tokens: usableContext,
    max_output_tokens: maxOutput,
    auto_compact_token_limit_tokens: autoCompact,
  };
};

const candidatesFor = (
  targetId: string,
  targetClass: string,
  records: readonly JsonRecord[],
): JsonRecord[] => {
  const variants = idVariants(targetId);
  const exact = records.filter((record) => {
    const id = modelId(record);
    return id ? [...idVariants(id)].some((candidate) => variants.has(candidate)) : false;
  });
  if (exact.length) return exact;
  return records.filter((record) => {
    const id = modelId(record);
    return id ? getRecentModelReasoning(id)?.modelClass === targetClass : false;
  });
};

const conservativeProviderCapability = (
  provider: ProviderCapability["provider"],
  records: readonly JsonRecord[],
): ProviderCapability | null => {
  const capabilities = records
    .map((record) => capabilityFromRecord(provider, record))
    .filter((value): value is ProviderCapability => value !== null);
  if (!capabilities.length) return null;
  capabilities.sort((left, right) =>
    (left.context_window_tokens ?? Number.MAX_SAFE_INTEGER) -
      (right.context_window_tokens ?? Number.MAX_SAFE_INTEGER) ||
    (left.max_context_window_tokens ?? Number.MAX_SAFE_INTEGER) -
      (right.max_context_window_tokens ?? Number.MAX_SAFE_INTEGER)
  );
  return capabilities[0];
};

const minimum = (values: readonly (number | null)[]): number | null => {
  const numbers = values.filter((value): value is number => value !== null);
  return numbers.length ? Math.min(...numbers) : null;
};

const resolveCapability = (
  id: string,
  modelClass: string,
  sources: Readonly<{
    codex: readonly JsonRecord[];
    openlux: readonly JsonRecord[];
    surplus: readonly JsonRecord[];
  }>,
): ResolvedCapability | null => {
  const providerCapabilities = (["codex", "openlux", "surplus"] as const)
    .map((provider) =>
      conservativeProviderCapability(provider, candidatesFor(id, modelClass, sources[provider]))
    )
    .filter((value): value is ProviderCapability => value !== null);
  if (!providerCapabilities.length) return null;
  return {
    model_class: modelClass,
    // Gateway-safe values use the narrowest route because the same model ID
    // may move from the primary provider to a paid fallback mid-request.
    max_context_window_tokens: minimum(providerCapabilities.map((entry) => entry.max_context_window_tokens)),
    context_window_tokens: minimum(providerCapabilities.map((entry) => entry.context_window_tokens)),
    max_output_tokens: minimum(providerCapabilities.map((entry) => entry.max_output_tokens)),
    auto_compact_token_limit_tokens: minimum(
      providerCapabilities.map((entry) => entry.auto_compact_token_limit_tokens),
    ),
    sources: providerCapabilities,
  };
};

const classSummary = (members: readonly ResolvedCapability[]): ResolvedCapability => ({
  model_class: members[0]!.model_class,
  max_context_window_tokens: minimum(members.map((entry) => entry.max_context_window_tokens)),
  context_window_tokens: minimum(members.map((entry) => entry.context_window_tokens)),
  max_output_tokens: minimum(members.map((entry) => entry.max_output_tokens)),
  auto_compact_token_limit_tokens: minimum(
    members.map((entry) => entry.auto_compact_token_limit_tokens),
  ),
  sources: [],
});

const replaceGeneratedBlock = (source: string, generated: string): string => {
  const block = `${GENERATED_START}\n${generated}\n${GENERATED_END}`;
  const start = source.indexOf(GENERATED_START);
  const end = source.indexOf(GENERATED_END);
  if (start >= 0 && end > start) {
    return source.slice(0, start) + block + source.slice(end + GENERATED_END.length);
  }
  const anchor = 'export const REASONING_NONE_VALUE = "none";';
  if (!source.includes(anchor)) throw new Error(`Missing ${anchor} in ${REASONING_SELECT_PATH}`);
  return source.replace(anchor, `${anchor}\n\n${block}`);
};

const ensureCapabilityExport = (source: string): string => {
  if (source.includes("export const getRecentModelCapabilities")) return source;
  const anchor = "const getTrimmedString =";
  const index = source.indexOf(anchor);
  if (index < 0) throw new Error(`Missing ${anchor} in ${REASONING_SELECT_PATH}`);
  const addition = `export const getRecentModelCapabilities = (modelId) => {\n  const reasoning = getRecentModelReasoning(modelId);\n  if (!reasoning) return null;\n  const normalized = typeof modelId === \"string\" ? modelId.trim().toLowerCase() : \"\";\n  const exact = RECENT_MODEL_CONTEXT_CAPABILITIES.models[normalized] ?? null;\n  const fallback = RECENT_MODEL_CONTEXT_CAPABILITIES.classes[reasoning.modelClass] ?? null;\n  const context = exact ?? fallback;\n  return context ? { ...reasoning, ...context } : reasoning;\n};\n\n`;
  return source.slice(0, index) + addition + source.slice(index);
};

const updateModelsPage = (source: string): string => {
  let updated = source.replace(
    'import { getRecentModelReasoning } from "./reasoning-select.js";',
    'import { getRecentModelCapabilities } from "./reasoning-select.js";',
  );
  updated = updated.replace("return getRecentModelReasoning(model.id);", "return getRecentModelCapabilities(model.id);");
  if (!updated.includes("data-context-window")) {
    const anchor = "      article.append(levels);\n    }\n    return article;";
    const addition = `      article.append(levels);\n    }\n\n    if (reasoning?.max_context_window_tokens || reasoning?.context_window_tokens) {\n      const context = document.createElement(\"div\");\n      context.dataset.contextWindow = \"\";\n      const format = new Intl.NumberFormat(\"en-US\");\n      const parts = [];\n      if (reasoning.max_context_window_tokens) {\n        parts.push(\`total window \\${format.format(reasoning.max_context_window_tokens)}\`);\n      }\n      if (reasoning.context_window_tokens) {\n        parts.push(\`usable history \\${format.format(reasoning.context_window_tokens)}\`);\n      }\n      if (reasoning.auto_compact_token_limit_tokens) {\n        parts.push(\`compact at \\${format.format(reasoning.auto_compact_token_limit_tokens)}\`);\n      }\n      if (reasoning.max_output_tokens) {\n        parts.push(\`max output \\${format.format(reasoning.max_output_tokens)}\`);\n      }\n      context.textContent = \`Context · \\${parts.join(\" · \")}\`;\n      if (Array.isArray(reasoning.sources) && reasoning.sources.length) {\n        context.title = reasoning.sources.map((source) =>\n          \`\\${source.provider}: \\${source.matched_model_id}\`\n        ).join(\"\\n\");\n      }\n      article.append(context);\n    }\n    return article;`;
    if (!updated.includes(anchor)) throw new Error(`Missing models page rendering anchor in ${MODELS_PAGE_PATH}`);
    updated = updated.replace(anchor, addition);
  }
  return updated;
};

const formatNumber = (value: number | null): string => value === null ? "—" : value.toLocaleString("en-US");

const auditMarkdown = (
  generatedAt: string,
  models: Readonly<Record<string, ResolvedCapability>>,
  missing: readonly string[],
): string => {
  const rows = Object.entries(models).sort(([left], [right]) => left.localeCompare(right)).map(([id, capability]) => {
    const sourceSummary = capability.sources.map((source) =>
      `${source.provider}:${source.matched_model_id}`
    ).join("<br>");
    return `| \`${id}\` | ${capability.model_class} | ${formatNumber(capability.max_context_window_tokens)} | ${formatNumber(capability.context_window_tokens)} | ${formatNumber(capability.max_output_tokens)} | ${formatNumber(capability.auto_compact_token_limit_tokens)} | ${sourceSummary} |`;
  });
  return `# Recent model context audit\n\nGenerated: ${generatedAt}\n\nThe gateway-safe values use the smallest documented limit among Codex, OpenLux, and Surplus records that can serve the model ID. For provider records that publish a total context window and maximum output, usable history is \`total - max output\`. When no explicit auto-compaction value is published, the snapshot uses 90% of usable history.\n\n| Model | Class | Total window | Usable history | Max output | Auto-compact at | Matched sources |\n|---|---|---:|---:|---:|---:|---|\n${rows.join("\n")}\n\n## Missing provider metadata\n\n${missing.length ? missing.map((id) => `- \`${id}\``).join("\n") : "None."}\n`;
};

const catalogPayload = await fetchJson(CATALOG_URL, {}, true);
const uosToken = Deno.env.get("UOS_AI_TOKEN")?.trim() ?? "";
const openLuxKey = Deno.env.get("OPENLUX_API_KEY")?.trim() ?? Deno.env.get("METERED_API_KEY")?.trim() ?? "";
const [capabilitiesPayload, surplusPayload, openLuxPayload] = await Promise.all([
  uosToken
    ? fetchJson(CAPABILITIES_URL, { Authorization: `Bearer ${uosToken}` })
    : Promise.resolve(null),
  fetchJson(SURPLUS_MODELS_URL),
  openLuxKey
    ? fetchJson(OPENLUX_MODELS_URL, { Authorization: `Bearer ${openLuxKey}` })
    : Promise.resolve(null),
]);

const catalogRecords = modelArray(catalogPayload);
const sources = {
  codex: modelArray(capabilitiesPayload),
  openlux: modelArray(openLuxPayload),
  surplus: modelArray(surplusPayload),
} as const;

const models: Record<string, ResolvedCapability> = {};
const missing: string[] = [];
for (const record of catalogRecords) {
  const id = modelId(record);
  if (!id) continue;
  const reasoning = getRecentModelReasoning(id);
  if (!reasoning) continue;
  const resolved = resolveCapability(id, reasoning.modelClass, sources);
  if (resolved) models[normalizedId(id)] = resolved;
  else missing.push(id);
}

const grouped = new Map<string, ResolvedCapability[]>();
for (const capability of Object.values(models)) {
  const members = grouped.get(capability.model_class) ?? [];
  members.push(capability);
  grouped.set(capability.model_class, members);
}
const classes = Object.fromEntries(
  [...grouped.entries()].sort(([left], [right]) => left.localeCompare(right)).map(([name, members]) => [
    name,
    classSummary(members),
  ]),
);

const generatedAt = new Date().toISOString();
const generatedObject = { generated_at: generatedAt, models, classes };
const generatedSource = `const RECENT_MODEL_CONTEXT_CAPABILITIES = ${JSON.stringify(generatedObject, null, 2)};`;

let reasoningSource = await Deno.readTextFile(REASONING_SELECT_PATH);
reasoningSource = replaceGeneratedBlock(reasoningSource, generatedSource);
reasoningSource = ensureCapabilityExport(reasoningSource);
await Deno.writeTextFile(REASONING_SELECT_PATH, reasoningSource);

const modelsPageSource = await Deno.readTextFile(MODELS_PAGE_PATH);
await Deno.writeTextFile(MODELS_PAGE_PATH, updateModelsPage(modelsPageSource));
await Deno.writeTextFile(AUDIT_PATH, auditMarkdown(generatedAt, models, missing));

console.log(JSON.stringify({ models: Object.keys(models).length, classes: Object.keys(classes).length, missing }, null, 2));
