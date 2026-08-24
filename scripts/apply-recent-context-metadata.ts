const read = (path: string): string => Deno.readTextFileSync(path);
const write = (path: string, content: string): void => Deno.writeTextFileSync(path, content);

const replaceOnce = (content: string, before: string, after: string, label: string): string => {
  const first = content.indexOf(before);
  if (first < 0) throw new Error(`Missing ${label}`);
  if (content.indexOf(before, first + before.length) >= 0) throw new Error(`Ambiguous ${label}`);
  return content.slice(0, first) + after + content.slice(first + before.length);
};

const replaceSection = (
  content: string,
  startMarker: string,
  endMarker: string,
  replacement: string,
  label: string,
): string => {
  const start = content.indexOf(startMarker);
  if (start < 0) throw new Error(`Missing ${label} start`);
  const end = content.indexOf(endMarker, start);
  if (end < 0) throw new Error(`Missing ${label} end`);
  return content.slice(0, start) + replacement + content.slice(end);
};

{
  const path = "src/codex_catalog.ts";
  let content = read(path);
  content = replaceOnce(
    content,
    'import { fetchSurplusModels, SURPLUS_MODELS_CACHE_TTL_MS } from "./surplus.ts";\n',
    'import { fetchSurplusModels, SURPLUS_MODELS_CACHE_TTL_MS } from "./surplus.ts";\n' +
      'import { recentModelContextFor } from "./recent_model_context.ts";\n',
    "codex catalog context import",
  );
  const replacement = `const meteredCodexModelRecord = (
  model: Readonly<{
    id: string;
    description?: string;
    owned_by: string;
    supported_endpoint_types: readonly string[];
  }>,
) => {
  const context = recentModelContextFor(model.id);
  return {
    slug: model.id,
    display_name: model.id,
    description: model.description,
    owned_by: model.owned_by,
    supported_endpoint_types: [...model.supported_endpoint_types],
    supported_reasoning_levels: /^deepseek-v4-flash(?:-0731|:web)?$/.test(model.id)
      ? [
        { effort: "none", description: "Disable optional reasoning" },
        { effort: "low", description: "Reasoning effort: low" },
        { effort: "high", description: "Reasoning effort: high" },
        { effort: "max", description: "Maximum reasoning depth" },
      ]
      : [{ effort: "none", description: "No reasoning" }],
    default_reasoning_level: /^deepseek-v4-flash(?:-0731|:web)?$/.test(model.id) ? "high" : "none",
    ...(context
      ? {
        model_class: context.model_class,
        context_window: context.context_window_tokens,
        max_context_window: context.max_context_window_tokens,
        auto_compact_token_limit: context.auto_compact_token_limit_tokens,
        effective_context_window_percent: context.effective_context_window_percent,
      }
      : {}),
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
};`;
  content = replaceSection(
    content,
    "const meteredCodexModelRecord = (",
    "\n\nconst uniqueResponsesModels",
    replacement,
    "metered Codex model record",
  );
  write(path, content);
}

{
  const path = "src/openai.ts";
  let content = read(path);
  content = replaceOnce(
    content,
    'import { loadRuntimeConfig } from "./runtime_config.ts";\n',
    'import { loadRuntimeConfig } from "./runtime_config.ts";\n' +
      'import { recentModelContextFor } from "./recent_model_context.ts";\n',
    "OpenAI context import",
  );

  const capabilitiesReplacement = `const normalizeModelCapabilitiesEntry = (value: unknown): Record<string, unknown> | null => {
  if (!isRecord(value)) return null;
  const id = modelIdFromSnapshotRecord(value);
  if (!id) return null;
  const reasoning = getCodexModelReasoning(value);
  const promptCache = normalizePromptCacheCapabilities(value.prompt_cache);
  const fallbackContext = recentModelContextFor(id);
  const contextWindow = normalizeTokenCount(value.context_window) ?? fallbackContext?.context_window_tokens ?? null;
  const maxContextWindow = normalizeTokenCount(value.max_context_window) ??
    fallbackContext?.max_context_window_tokens ?? contextWindow;
  const autoCompactTokenLimit = normalizeTokenCount(value.auto_compact_token_limit) ??
    fallbackContext?.auto_compact_token_limit_tokens ?? null;
  return {
    id,
    object: "uos.model_capabilities",
    owned_by: getString(value.owned_by) ?? "openai",
    display_name: getString(value.display_name),
    upstream_provider: "codex_chatgpt",
    supported_endpoints: ["/v1/chat/completions", "/v1/responses"],
    supported_reasoning_levels: reasoning.levels,
    default_reasoning_effort: reasoning.defaultLevel,
    reasoning_effort_wire_map: Object.fromEntries(reasoning.wireEfforts),
    context_window_tokens: contextWindow,
    max_context_window_tokens: maxContextWindow,
    auto_compact_token_limit_tokens: autoCompactTokenLimit,
    ...(fallbackContext
      ? {
        model_class: fallbackContext.model_class,
        effective_context_window_percent: normalizeTokenCount(value.effective_context_window_percent) ??
          fallbackContext.effective_context_window_percent,
      }
      : {}),
    ...(promptCache !== null ? { prompt_cache: promptCache } : {}),
  };
};`;
  content = replaceSection(
    content,
    "const normalizeModelCapabilitiesEntry = (",
    "\n\nconst normalizeResponseContentItem",
    capabilitiesReplacement,
    "model capabilities normalizer",
  );

  const providerType = `type PublicModelProvider = Readonly<{
  id: "codex" | "openlux" | "surplus";
  owned_by: string;
  supported_endpoints: readonly string[];
}>;

type PublicModelCatalogEntry = {
  id: string;
  providers: PublicModelProvider[];
  model_class?: string;
  context_window_tokens?: number;
  max_context_window_tokens?: number;
  auto_compact_token_limit_tokens?: number;
  effective_context_window_percent?: number;
};`;
  content = replaceSection(
    content,
    "type PublicModelProvider = Readonly<",
    "\n\nexport const handlePublicModelCatalog",
    providerType,
    "public model provider types",
  );

  content = replaceOnce(
    content,
    `  const models = new Map<string, { id: string; providers: PublicModelProvider[] }>();
  const includedOpenLuxModelIds = new Set<string>();
  const add = (id: string, provider: PublicModelProvider): void => {
    const existing = models.get(id);
    if (existing) existing.providers.push(provider);
    else models.set(id, { id, providers: [provider] });
  };`,
    `  const models = new Map<string, PublicModelCatalogEntry>();
  const includedOpenLuxModelIds = new Set<string>();
  const add = (id: string, provider: PublicModelProvider): void => {
    const existing = models.get(id);
    if (existing) {
      existing.providers.push(provider);
      return;
    }
    const context = recentModelContextFor(id);
    models.set(id, {
      id,
      providers: [provider],
      ...(context
        ? {
          model_class: context.model_class,
          context_window_tokens: context.context_window_tokens,
          max_context_window_tokens: context.max_context_window_tokens,
          auto_compact_token_limit_tokens: context.auto_compact_token_limit_tokens,
          effective_context_window_percent: context.effective_context_window_percent,
        }
        : {}),
    });
  };`,
    "public catalog context enrichment",
  );

  const capabilitiesStart = content.indexOf("export const handleModelCapabilities");
  const capabilitiesEnd = content.indexOf("\n\nconst withVoyageUpstreamHeader", capabilitiesStart);
  if (capabilitiesStart < 0 || capabilitiesEnd < 0) throw new Error("Missing model capabilities handler");
  const beforeCapabilities = content.slice(0, capabilitiesStart);
  let handler = content.slice(capabilitiesStart, capabilitiesEnd);
  const endpointBlock = `      const supportedEndpoints = [
        ...(model.supported_endpoint_types.includes("openai-response") ? ["/v1/responses"] : []),
        ...(model.supported_endpoint_types.includes("openai") ? ["/v1/chat/completions"] : []),
      ];
      data.push({`;
  handler = replaceOnce(
    handler,
    endpointBlock,
    `      const supportedEndpoints = [
        ...(model.supported_endpoint_types.includes("openai-response") ? ["/v1/responses"] : []),
        ...(model.supported_endpoint_types.includes("openai") ? ["/v1/chat/completions"] : []),
      ];
      const context = recentModelContextFor(model.id);
      data.push({`,
    "paid capability context lookup",
  );
  handler = replaceOnce(
    handler,
    `        context_window_tokens: null,
        max_context_window_tokens: null,
        auto_compact_token_limit_tokens: null,`,
    `        context_window_tokens: context?.context_window_tokens ?? null,
        max_context_window_tokens: context?.max_context_window_tokens ?? null,
        auto_compact_token_limit_tokens: context?.auto_compact_token_limit_tokens ?? null,
        ...(context
          ? {
            model_class: context.model_class,
            effective_context_window_percent: context.effective_context_window_percent,
          }
          : {}),`,
    "paid capability context fields",
  );
  content = beforeCapabilities + handler + content.slice(capabilitiesEnd);
  write(path, content);
}

{
  const path = "tests/codex-catalog.test.ts";
  let content = read(path);
  content = replaceOnce(
    content,
    `          { id: "deepseek-v4-flash:web", provider: "surplus" },
        ],`,
    `          { id: "deepseek-v4-flash:web", provider: "surplus" },
          { id: "minimax-m2.7", provider: "surplus" },
        ],`,
    "paid catalog MiniMax fixture",
  );
  content = replaceOnce(
    content,
    `      "deepseek-v4-flash:web",
    ]);`,
    `      "deepseek-v4-flash:web",
      "minimax-m2.7",
    ]);`,
    "paid catalog expected slugs",
  );
  content = replaceOnce(
    content,
    `      assert.equal(model?.default_reasoning_level, "high");
    }
    assert.equal(slugs.includes("chat-only-model"), false);`,
    `      assert.equal(model?.default_reasoning_level, "high");
      assert.equal(model?.context_window, 1_000_000);
      assert.equal(model?.max_context_window, 1_000_000);
      assert.equal(model?.auto_compact_token_limit, 850_000);
      assert.equal(model?.effective_context_window_percent, 95);
    }
    const minimax = payload.models.find((model) => model.slug === "minimax-m2.7");
    assert.equal(minimax?.context_window, 204_800);
    assert.equal(minimax?.auto_compact_token_limit, 154_800);
    assert.equal(slugs.includes("chat-only-model"), false);`,
    "paid catalog context assertions",
  );
  write(path, content);
}

for (const path of [
  ".github/workflows/context-catalog-audit.yml",
  ".github/workflows/apply-context-window-patch.yml",
  "docs/context-catalog-link.md",
  "scripts/apply-recent-context-metadata.ts",
]) {
  try {
    Deno.removeSync(path);
  } catch (error) {
    if (!(error instanceof Deno.errors.NotFound)) throw error;
  }
}
