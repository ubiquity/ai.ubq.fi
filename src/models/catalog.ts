// Model catalog assembly and the public model endpoints, extracted from src/openai.ts.

import { type CodexModelsSnapshot, loadCodexModelsSnapshot, loadFullCodexModelsSnapshot } from "../codex/index.ts";
import { CEREBRAS_GPT_OSS_120B_MODEL, readCerebrasApiKey } from "../provider/cerebras.ts";
import {
  DEEPSEEK_CONTEXT_WINDOW_TOKENS,
  DEEPSEEK_DEFAULT_REASONING_EFFORT,
  DEEPSEEK_OFFICIAL_MODEL_IDS,
  DEEPSEEK_REASONING_LEVELS,
  readDeepSeekApiKey,
} from "../deepseek/index.ts";
import { LITHOS_MODEL_IDS, readLithosApiKey } from "../provider/lithos.ts";
import { getCatalogClientVersion, handleCodexCatalogModels } from "../catalog/index.ts";
import { loadCodexModelsWhitelist, filterWhitelistedModelList, filterWhitelistedModelMap } from "./codex-models-whitelist.ts";
import {
  filterCatalogEntriesByProviderSelection,
  isProviderEnabled,
  loadProviderSelectionCached,
  type ProviderSelection,
  SELECTABLE_PROVIDER_IDS,
} from "../provider/selection.ts";
import { json } from "../http.ts";
import { getKv } from "../kv.ts";
import {
  codexSnapshotMetadataHint,
  codexSubscriptionMetadataHint,
  resolveModelMetadata,
  type ModelMetadataHint,
  type ModelMetadataSource,
  type ModelMetadataSources,
} from "./metadata.ts";
import { warmOpenRouterModels, openRouterModelsSnapshot } from "./openrouter-models.ts";
import { getString, isRecord } from "../utils.ts";
import { fetchMeteredModels } from "../provider/metered.ts";
import { fetchSurplusModels } from "../provider/surplus.ts";
import { CEREBRAS_PROVIDER_HINT, LITHOS_PROVIDER_HINT, modelIdFromSnapshotRecord } from "../request-policy.ts";
import {
  configuredCerebrasModelCapabilities,
  normalizeModelCapabilitiesEntry,
  normalizeModelList,
  withConfiguredCerebrasModel,
  withConfiguredDeepSeekCapabilities,
  withConfiguredDeepSeekModels,
  withConfiguredLithosCapabilities,
  withConfiguredLithosModels,
} from "../input-normalization.ts";

const snapshotUpstreamSource = (snapshot: CodexModelsSnapshot | null): string => {
  const source = snapshot?.source;
  if (!source) return "stored_codex_models";
  return source;
};

export const handleModels = async (req?: Request): Promise<Response> => {
  if (req) {
    const clientVersion = getCatalogClientVersion(req);
    if (clientVersion !== null) return await handleCodexCatalogModels(req, clientVersion);
  }
  // A switched-off provider is not advertised, so its rows leave this list even
  // while the discovery snapshots still hold them.
  const selection = await loadProviderSelectionCached();
  const snapshot = await loadCodexModelsSnapshot();
  const normalized = snapshot && Array.isArray(snapshot.models) && snapshot.models.length > 0 ? normalizeModelList(snapshot) : null;
  const codexModels = isProviderEnabled("codex", selection) ? (normalized?.data ?? []) : [];
  const data = withConfiguredLithosModels(
    withConfiguredDeepSeekModels(withConfiguredCerebrasModel(codexModels, isProviderEnabled("cerebras", selection)), isProviderEnabled("deepseek", selection)),
    isProviderEnabled("lithos", selection)
  );
  const [metered, surplus] = await Promise.all([
    isProviderEnabled("openlux", selection) ? fetchMeteredModels() : Promise.resolve(null),
    isProviderEnabled("surplus", selection) ? fetchSurplusModels() : Promise.resolve(null),
  ]);
  const merged = [...data];
  for (const model of [...(metered?.models ?? []), ...(surplus?.models ?? [])]) {
    if (!model.supported_endpoint_types.some((type) => type === "openai" || type === "openai-response")) continue;
    if (merged.some((candidate) => candidate.id === model.id)) continue;
    merged.push({
      id: model.id,
      object: "model",
      created: model.created,
      owned_by: model.owned_by,
    });
  }

  const modelsKv = await getKv();
  const whitelist = modelsKv ? await loadCodexModelsWhitelist(modelsKv) : null;
  const filtered = filterWhitelistedModelList(merged, whitelist);

  return json(200, { object: "list", data: filtered }, { "x-uos-upstream": snapshotUpstreamSource(snapshot) });
};

type PublicModelProvider = Readonly<{
  id: "codex" | "openlux" | "surplus" | "deepseek" | "cerebras" | "lithos";
  owned_by: string;
  supported_endpoints: readonly string[];
}>;

export type PublicModelCatalogEntry = {
  id: string;
  providers: PublicModelProvider[];
  created?: number;
  context_window_tokens?: number;
  max_context_window_tokens?: number;
  auto_compact_token_limit_tokens?: number;
  effective_context_window_percent?: number;
  /**
   * The reasoning tiers this model accepts, from the same resolver the
   * capabilities endpoint uses. Absent means no source advertises any tier.
   */
  supported_reasoning_levels?: readonly string[];
  default_reasoning_effort?: string;
  /** Where the context numbers came from: `codex_upload`, `provider_discovery`, `openrouter`, or `unknown`. */
  context_source?: ModelMetadataSource;
  /** Where the reasoning tiers came from, in the same vocabulary. */
  reasoning_source?: ModelMetadataSource;
};

/**
 * What each credential-gated route declares about its own model. These are the
 * route's statements, not curated per-model knowledge, so both the catalog and
 * the capabilities endpoint read them from here.
 */
const DEEPSEEK_PROVIDER_HINT: ModelMetadataHint = {
  context_window_tokens: DEEPSEEK_CONTEXT_WINDOW_TOKENS,
  max_context_window_tokens: DEEPSEEK_CONTEXT_WINDOW_TOKENS,
  supported_reasoning_levels: [...DEEPSEEK_REASONING_LEVELS],
  default_reasoning_effort: DEEPSEEK_DEFAULT_REASONING_EFFORT,
};

const providerSupportedEndpointPaths = (supportedEndpointTypes: readonly string[]): string[] => [
  ...(supportedEndpointTypes.includes("openai-response") ? ["/v1/responses"] : []),
  ...(supportedEndpointTypes.includes("openai") ? ["/v1/chat/completions"] : []),
];

const catalogAvailabilityStatus = (available: unknown): "available" | "unavailable" => (available ? "available" : "unavailable");

/**
 * One catalog row for one model id. Metadata comes from `resolveModelMetadata`,
 * so a row carries a value only when a source actually published one, and it
 * names that source. Ids no source describes stay bare instead of inheriting a
 * curated guess.
 */
const publicModelCatalogEntry = (id: string, provider: PublicModelProvider, created: unknown, sources: ModelMetadataSources = {}): PublicModelCatalogEntry => {
  const resolved = resolveModelMetadata(id, sources);
  const createdSeconds = typeof created === "number" && Number.isSafeInteger(created) && created > 0 ? created : null;
  return {
    id,
    providers: [provider],
    ...(createdSeconds === null ? {} : { created: createdSeconds }),
    ...(resolved.context_window_tokens === null ? {} : { context_window_tokens: resolved.context_window_tokens }),
    ...(resolved.max_context_window_tokens === null ? {} : { max_context_window_tokens: resolved.max_context_window_tokens }),
    ...(resolved.auto_compact_token_limit_tokens === null ? {} : { auto_compact_token_limit_tokens: resolved.auto_compact_token_limit_tokens }),
    ...(resolved.effective_context_window_percent === null ? {} : { effective_context_window_percent: resolved.effective_context_window_percent }),
    ...(resolved.supported_reasoning_levels === null ? {} : { supported_reasoning_levels: resolved.supported_reasoning_levels }),
    ...(resolved.default_reasoning_effort === null ? {} : { default_reasoning_effort: resolved.default_reasoning_effort }),
    context_source: resolved.context_source,
    reasoning_source: resolved.reasoning_source,
  };
};

const addPublicModelCatalogEntry = (
  models: Map<string, PublicModelCatalogEntry>,
  id: string,
  provider: PublicModelProvider,
  created: unknown = null,
  sources: ModelMetadataSources = {}
): void => {
  const existing = models.get(id);
  if (existing) {
    existing.providers.push(provider);
    if (existing.created === undefined && typeof created === "number" && Number.isSafeInteger(created) && created > 0) {
      existing.created = created;
    }
    return;
  }
  models.set(id, publicModelCatalogEntry(id, provider, created, sources));
};

/** Raw uploaded records by id, so the catalog can read metadata `normalizeModelList` drops. */
const collectCodexSnapshotRecords = (snapshot: CodexModelsSnapshot | null): Map<string, Record<string, unknown>> => {
  const records = new Map<string, Record<string, unknown>>();
  if (!snapshot || !Array.isArray(snapshot.models)) return records;
  for (const entry of snapshot.models) {
    if (!isRecord(entry)) continue;
    const id = modelIdFromSnapshotRecord(entry);
    if (id) records.set(id, entry);
  }
  return records;
};

export type ModelCatalogSourceId = "codex" | "openlux" | "surplus" | "deepseek" | "cerebras" | "lithos" | "openrouter";

export type ModelCatalogSource = Readonly<{
  status: "available" | "unavailable";
  count: number;
  updated_at_ms: number | null;
  /**
   * Present only for credential-gated providers, which are served from a
   * configured API key instead of discovery. `false` means the gateway does not
   * serve that provider at all, which is not the same as a failed discovery.
   */
  configured?: boolean;
  /** Set when the operator switched this provider off in the admin console. */
  disabled?: boolean;
}>;

export type ModelCatalogSnapshot = Readonly<{
  models: PublicModelCatalogEntry[];
  sources: Readonly<Record<ModelCatalogSourceId, ModelCatalogSource>>;
}>;

/** A credential-gated provider is configured or it is absent; it has no upstream timestamp. */
const credentialGatedCatalogSource = (configured: boolean, count: number): ModelCatalogSource => ({
  status: configured ? "available" : "unavailable",
  count: configured ? count : 0,
  updated_at_ms: null,
  configured,
});

/**
 * Every provider's models are listed, each attributed to every provider that
 * publishes it. Nothing is filtered or re-attributed here: the operator's
 * whitelist selection is the only thing that decides what the gateway
 * advertises, so an id this route can serve must stay selectable.
 */
const addCredentialGatedCatalogProviders = (models: Map<string, PublicModelCatalogEntry>): { deepseek: number; cerebras: number; lithos: number } => {
  let deepseek = 0;
  if (readDeepSeekApiKey()) {
    const provider: PublicModelProvider = {
      id: "deepseek",
      owned_by: "deepseek",
      supported_endpoints: ["/v1/chat/completions", "/v1/responses"],
    };
    for (const id of DEEPSEEK_OFFICIAL_MODEL_IDS) {
      // Both official ids are one served model behind two aliases, and the route
      // declares its window and tiers so the row matches what the capabilities
      // endpoint reports for the same id.
      addPublicModelCatalogEntry(models, id, provider, null, { provider: DEEPSEEK_PROVIDER_HINT });
      deepseek += 1;
    }
  }
  let cerebras = 0;
  if (readCerebrasApiKey()) {
    addPublicModelCatalogEntry(
      models,
      CEREBRAS_GPT_OSS_120B_MODEL,
      { id: "cerebras", owned_by: "cerebras", supported_endpoints: ["/v1/chat/completions"] },
      null,
      { provider: CEREBRAS_PROVIDER_HINT }
    );
    cerebras = 1;
  }
  let lithos = 0;
  if (readLithosApiKey()) {
    // Eight ids, each individually addressable: the three per-model speed tiers
    // (`-fast`, `-ultra`, `-ultra-chat`) are the same weights at higher per-token
    // rates, so they are commercial tiers of one model rather than one model
    // behind an alias. The catalog and capabilities schemas have no speed-tier
    // field, and collapsing the ids would make a client-requestable model
    // unselectable, so the tier stays in the id and the display name.
    const provider: PublicModelProvider = {
      id: "lithos",
      owned_by: "lithos",
      // Chat Completions is the vendor's own endpoint; `/v1/responses` is served
      // by this gateway's translation, exactly as the DeepSeek adapter's rows
      // declare it.
      supported_endpoints: ["/v1/chat/completions", "/v1/responses"],
    };
    for (const id of LITHOS_MODEL_IDS) {
      addPublicModelCatalogEntry(models, id, provider, null, { provider: LITHOS_PROVIDER_HINT });
      lithos += 1;
    }
  }
  return { deepseek, cerebras, lithos };
};

/**
 * Build the complete catalog without applying the operator whitelist.
 * `/uos/models/catalog` filters this snapshot for the public page, while the
 * admin console reads it unfiltered so a disabled model stays visible (and can
 * be switched back on) in the operator's picker.
 */
export const buildModelCatalogSnapshot = async (): Promise<ModelCatalogSnapshot> => {
  // Enrichment is cache-only and never awaited, so a slow third party cannot
  // delay the catalog; the first load after a cold start simply shows less.
  warmOpenRouterModels();
  // The compacted runtime copy keeps reasoning tiers and drops context windows,
  // so reading it here let a third-party API-level window (1M) outrank the window
  // Codex actually serves (272k for these ids). The full uploaded catalog is the
  // Codex override; the runtime copy is only a fallback when it is unavailable.
  const snapshot = (await loadFullCodexModelsSnapshot()) ?? (await loadCodexModelsSnapshot());
  const normalized = snapshot && Array.isArray(snapshot.models) && snapshot.models.length > 0 ? normalizeModelList(snapshot) : null;
  const [metered, surplus] = await Promise.all([fetchMeteredModels(), fetchSurplusModels({ requireApiKey: false })]);
  const codexModels = normalized?.data ?? [];
  const surplusModels = surplus?.models ?? [];
  const models = new Map<string, PublicModelCatalogEntry>();
  // The uploaded records carry the context and reasoning metadata that
  // `normalizeModelList` strips down to id/object/created/owned_by.
  const codexRecords = collectCodexSnapshotRecords(snapshot);

  for (const model of codexModels) {
    const id = getString(model.id);
    if (!id) continue;
    const codexRecord = codexRecords.get(id) ?? null;
    addPublicModelCatalogEntry(
      models,
      id,
      {
        id: "codex",
        owned_by: getString(model.owned_by) ?? "openai",
        supported_endpoints: ["/v1/responses", "/v1/chat/completions"],
      },
      model.created,
      { codex: codexSnapshotMetadataHint(codexRecord), codexSubscription: codexSubscriptionMetadataHint() }
    );
  }
  for (const model of metered?.models ?? []) {
    addPublicModelCatalogEntry(
      models,
      model.id,
      {
        id: "openlux",
        owned_by: model.owned_by,
        supported_endpoints: providerSupportedEndpointPaths(model.supported_endpoint_types),
      },
      model.created
    );
  }
  for (const model of surplusModels) {
    addPublicModelCatalogEntry(
      models,
      model.id,
      {
        id: "surplus",
        owned_by: model.owned_by,
        supported_endpoints: providerSupportedEndpointPaths(model.supported_endpoint_types),
      },
      model.created
    );
  }

  const credentialGated = addCredentialGatedCatalogProviders(models);

  return {
    models: [...models.values()].sort((left, right) => left.id.localeCompare(right.id)),
    sources: {
      codex: {
        status: catalogAvailabilityStatus(normalized),
        count: normalized?.data.length ?? 0,
        updated_at_ms: snapshot?.updated_at_ms ?? null,
      },
      openlux: {
        status: catalogAvailabilityStatus(metered),
        count: metered?.models.length ?? 0,
        updated_at_ms: metered?.updated_at_ms ?? null,
      },
      surplus: {
        status: catalogAvailabilityStatus(surplus),
        count: surplus?.models.length ?? 0,
        updated_at_ms: surplus?.updated_at_ms ?? null,
      },
      deepseek: credentialGatedCatalogSource(readDeepSeekApiKey() !== null, credentialGated.deepseek),
      cerebras: credentialGatedCatalogSource(readCerebrasApiKey() !== null, credentialGated.cerebras),
      lithos: credentialGatedCatalogSource(readLithosApiKey() !== null, credentialGated.lithos),
      // Enrichment is listed as a source so the page can show how much of THIS
      // catalog it fills, counted the same way as every other source: rows it
      // supplied, not the size of the upstream catalog.
      openrouter: {
        status: catalogAvailabilityStatus(openRouterModelsSnapshot()),
        count: [...models.values()].filter((model) => model.context_source === "openrouter" || model.reasoning_source === "openrouter").length,
        updated_at_ms: openRouterModelsSnapshot()?.updated_at_ms ?? null,
      },
    },
  };
};

/** `sources` carries exactly one key per catalog source id, so a key it owns is one of them. */
const isModelCatalogSourceId = (sources: ModelCatalogSnapshot["sources"], id: string): id is ModelCatalogSourceId => Object.hasOwn(sources, id);

/**
 * Public catalog sources for one provider selection. A switched-off provider
 * contributes nothing and is marked `disabled`, which the models page reads as
 * "not served at all" rather than as a failed discovery. The admin picker keeps
 * using the unfiltered snapshot so every provider stays visible and switchable.
 *
 * The selectable roster can name a provider this snapshot has no source for: a
 * provider is wired into the routing vocabulary before its catalog source is
 * published, so only ids this snapshot actually reports are marked disabled.
 */
export const selectedCatalogSources = (sources: ModelCatalogSnapshot["sources"], selection: ProviderSelection | null): ModelCatalogSnapshot["sources"] => {
  if (!selection || selection.provider_ids.length === 0) return sources;
  const adjusted = { ...sources };
  for (const id of SELECTABLE_PROVIDER_IDS) {
    if (isProviderEnabled(id, selection)) continue;
    if (!isModelCatalogSourceId(adjusted, id)) continue;
    adjusted[id] = { status: "unavailable", count: 0, updated_at_ms: null, configured: false, disabled: true };
  }
  return adjusted;
};

export const handlePublicModelCatalog = async (): Promise<Response> => {
  const [catalog, selection] = await Promise.all([buildModelCatalogSnapshot(), loadProviderSelectionCached()]);
  const catalogKv = await getKv();
  const catalogWhitelist = catalogKv ? await loadCodexModelsWhitelist(catalogKv) : null;
  return json(200, {
    object: "uos.model_catalog",
    data: filterWhitelistedModelMap(filterCatalogEntriesByProviderSelection(catalog.models, selection), catalogWhitelist),
    sources: selectedCatalogSources(catalog.sources, selection),
  });
};

const discoveredModelCapabilitiesEntry = (
  model: Readonly<{ id: string; owned_by: string; supported_endpoint_types: readonly string[] }>,
  provider: "metered" | "surplus"
): Record<string, unknown> => {
  const supportedEndpoints = providerSupportedEndpointPaths(model.supported_endpoint_types);
  // A discovery row states which endpoints exist, not what the model can do, so
  // enrichment is the only source left for these ids. When even that is silent
  // the model advertises `none` alone: that is the gateway's no-reasoning
  // default, not a claim about the model's tiers.
  const resolved = resolveModelMetadata(model.id);
  return {
    id: model.id,
    object: "uos.model_capabilities",
    owned_by: model.owned_by,
    display_name: model.id,
    upstream_provider: provider,
    supported_endpoints: supportedEndpoints,
    supported_reasoning_levels: resolved.supported_reasoning_levels ?? ["none"],
    default_reasoning_effort: resolved.default_reasoning_effort ?? "none",
    reasoning_effort_wire_map: {},
    context_window_tokens: resolved.context_window_tokens,
    max_context_window_tokens: resolved.max_context_window_tokens,
    auto_compact_token_limit_tokens: resolved.auto_compact_token_limit_tokens,
    ...(resolved.effective_context_window_percent === null ? {} : { effective_context_window_percent: resolved.effective_context_window_percent }),
    context_source: resolved.context_source,
    reasoning_source: resolved.reasoning_source,
  };
};

export const handleModelCapabilities = async (): Promise<Response> => {
  warmOpenRouterModels();
  // Capabilities describe the routes the gateway can still dispatch to, so a
  // switched-off provider contributes no entries here either.
  const selection = await loadProviderSelectionCached();
  const snapshot = await loadFullCodexModelsSnapshot();
  let data =
    snapshot && Array.isArray(snapshot.models) && snapshot.models.length > 0 && isProviderEnabled("codex", selection)
      ? (snapshot.models.map(normalizeModelCapabilitiesEntry).filter(Boolean) as Record<string, unknown>[])
      : [];
  const cerebras = isProviderEnabled("cerebras", selection) ? configuredCerebrasModelCapabilities() : null;
  if (cerebras && !data.some((model) => model.id === CEREBRAS_GPT_OSS_120B_MODEL)) {
    data.push(cerebras);
  }
  const [metered, surplus] = await Promise.all([
    isProviderEnabled("openlux", selection) ? fetchMeteredModels() : Promise.resolve(null),
    isProviderEnabled("surplus", selection) ? fetchSurplusModels() : Promise.resolve(null),
  ]);
  for (const [provider, models] of [
    ["metered", metered?.models ?? []],
    ["surplus", surplus?.models ?? []],
  ] as const) {
    for (const model of models) {
      if (data.some((candidate) => candidate.id === model.id)) continue;
      data.push(discoveredModelCapabilitiesEntry(model, provider));
    }
  }
  // Applied last so a DeepSeek-official id discovered above from the paid
  // fallback is replaced by the capabilities of the route it actually uses.
  data = withConfiguredDeepSeekCapabilities(data, isProviderEnabled("deepseek", selection));
  data = withConfiguredLithosCapabilities(data, isProviderEnabled("lithos", selection));

  const capabilitiesKv = await getKv();
  const capabilitiesWhitelist = capabilitiesKv ? await loadCodexModelsWhitelist(capabilitiesKv) : null;
  const filteredData = filterWhitelistedModelList(data, capabilitiesWhitelist);

  return json(
    200,
    {
      object: "list",
      data: filteredData,
      upstream_provider: "codex_chatgpt",
      source: snapshot?.source ?? "stored_codex_models",
      client_version: snapshot?.client_version ?? null,
      updated_at_ms: snapshot?.updated_at_ms ?? null,
    },
    { "x-uos-upstream": snapshotUpstreamSource(snapshot) }
  );
};
