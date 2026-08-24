const path = "src/openai.ts";
let source = await Deno.readTextFile(path);

const replaceOnce = (needle: string, replacement: string): void => {
  const first = source.indexOf(needle);
  if (first < 0) throw new Error(`Missing source anchor:\n${needle}`);
  if (source.indexOf(needle, first + needle.length) >= 0) {
    throw new Error(`Source anchor is not unique:\n${needle}`);
  }
  source = source.slice(0, first) + replacement + source.slice(first + needle.length);
};

const importAnchor = 'import { loadDebugRoutingConfig } from "./debug_routing.ts";';
if (!source.includes('from "./recent_model_capabilities.ts"')) {
  replaceOnce(
    importAnchor,
    `${importAnchor}\nimport { getRecentGatewayModelCapabilities } from "./recent_model_capabilities.ts";`,
  );
}

const providerType = `type PublicModelProvider = Readonly<{\n  id: "codex" | "openlux" | "surplus";\n  owned_by: string;\n  supported_endpoints: readonly string[];\n}>;`;
const catalogType = `${providerType}\n\ntype PublicModelCatalogEntry = {\n  id: string;\n  providers: PublicModelProvider[];\n  model_class?: string;\n  supported_reasoning_levels?: readonly string[];\n  default_reasoning_effort?: string | null;\n  max_context_window_tokens?: number;\n  context_window_tokens?: number;\n  max_output_tokens?: number | null;\n  auto_compact_token_limit_tokens?: number;\n};`;
if (!source.includes("type PublicModelCatalogEntry")) replaceOnce(providerType, catalogType);

const addBlock = `  const models = new Map<string, { id: string; providers: PublicModelProvider[] }>();\n  const includedOpenLuxModelIds = new Set<string>();\n  const add = (id: string, provider: PublicModelProvider): void => {\n    const existing = models.get(id);\n    if (existing) existing.providers.push(provider);\n    else models.set(id, { id, providers: [provider] });\n  };`;
const enrichedAddBlock = `  const models = new Map<string, PublicModelCatalogEntry>();\n  const includedOpenLuxModelIds = new Set<string>();\n  const add = (id: string, provider: PublicModelProvider): void => {\n    const existing = models.get(id);\n    if (existing) {\n      existing.providers.push(provider);\n      return;\n    }\n    const capabilities = getRecentGatewayModelCapabilities(id);\n    models.set(id, {\n      id,\n      providers: [provider],\n      ...(capabilities\n        ? {\n          model_class: capabilities.model_class,\n          supported_reasoning_levels: capabilities.supported_reasoning_levels,\n          default_reasoning_effort: capabilities.default_reasoning_effort,\n          max_context_window_tokens: capabilities.max_context_window_tokens,\n          context_window_tokens: capabilities.context_window_tokens,\n          max_output_tokens: capabilities.max_output_tokens,\n          auto_compact_token_limit_tokens: capabilities.auto_compact_token_limit_tokens,\n        }\n        : {}),\n    });\n  };`;
if (!source.includes("getRecentGatewayModelCapabilities(id)")) replaceOnce(addBlock, enrichedAddBlock);

const dynamicCapabilitiesBlock = `      data.push({\n        id: model.id,\n        object: "uos.model_capabilities",\n        owned_by: model.owned_by,\n        display_name: model.id,\n        upstream_provider: provider,\n        supported_endpoints: supportedEndpoints,\n        supported_reasoning_levels: ["none"],\n        default_reasoning_effort: "none",\n        reasoning_effort_wire_map: {},\n        context_window_tokens: null,\n        max_context_window_tokens: null,\n        auto_compact_token_limit_tokens: null,\n      });`;
const enrichedDynamicCapabilitiesBlock = `      const recentCapabilities = getRecentGatewayModelCapabilities(model.id);\n      data.push({\n        id: model.id,\n        object: "uos.model_capabilities",\n        owned_by: model.owned_by,\n        display_name: model.id,\n        upstream_provider: provider,\n        supported_endpoints: supportedEndpoints,\n        supported_reasoning_levels: recentCapabilities?.supported_reasoning_levels ?? ["none"],\n        default_reasoning_effort: recentCapabilities?.default_reasoning_effort ?? "none",\n        reasoning_effort_wire_map: {},\n        context_window_tokens: recentCapabilities?.context_window_tokens ?? null,\n        max_context_window_tokens: recentCapabilities?.max_context_window_tokens ?? null,\n        auto_compact_token_limit_tokens: recentCapabilities?.auto_compact_token_limit_tokens ?? null,\n        ...(recentCapabilities\n          ? {\n            model_class: recentCapabilities.model_class,\n            max_output_tokens: recentCapabilities.max_output_tokens,\n          }\n          : {}),\n      });`;
if (!source.includes("const recentCapabilities = getRecentGatewayModelCapabilities(model.id)")) {
  replaceOnce(dynamicCapabilitiesBlock, enrichedDynamicCapabilitiesBlock);
}

await Deno.writeTextFile(path, source);
