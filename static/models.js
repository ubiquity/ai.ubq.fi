import { getRecentModelReasoning } from "./reasoning-select.js?v=20260824-recent-reasoning-v2";
import { toast } from "./toast.js?v=20260903-toast-v1";

const summary = document.querySelector("[data-source-summary]");
const list = document.querySelector("[data-model-list]");
const count = document.querySelector("[data-model-count]");
const search = document.querySelector("[data-model-search]");

if (!summary || !list || !count || !(search instanceof HTMLInputElement)) {
  throw new Error("Models page markup is incomplete");
}

const providerNames = { codex: "Codex", openlux: "Metered 2", surplus: "Metered 1" };
const reasoningOrder = ["none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra"];
const tokenNumber = new Intl.NumberFormat("en-US");

const normalizeReasoningLevels = (value) => {
  if (!Array.isArray(value)) return [];
  const levels = value
    .map((entry) => typeof entry === "string" ? entry : entry?.effort)
    .filter((entry) => typeof entry === "string" && entry.length > 0);
  return [...new Set(levels)].sort((a, b) => {
    const aIndex = reasoningOrder.indexOf(a);
    const bIndex = reasoningOrder.indexOf(b);
    return (aIndex < 0 ? reasoningOrder.length : aIndex) - (bIndex < 0 ? reasoningOrder.length : bIndex);
  });
};

const reasoningFor = (model) => {
  const advertised = normalizeReasoningLevels(model.supported_reasoning_levels);
  if (advertised.length) {
    return {
      modelClass: model.model_class ?? null,
      levels: advertised,
      defaultLevel: model.default_reasoning_effort ?? null,
    };
  }
  return getRecentModelReasoning(model.id);
};

const positiveTokenCount = (value) =>
  typeof value === "number" && Number.isSafeInteger(value) && value > 0 ? value : null;

let catalog = [];
const assignCatalog = (next) => {
  catalog = next;
};

const render = () => {
  const query = search.value.trim().toLowerCase();
  const visible = catalog.filter((model) => {
    const reasoning = reasoningFor(model);
    const contextWindow = positiveTokenCount(model.context_window_tokens);
    const maxContextWindow = positiveTokenCount(model.max_context_window_tokens);
    const autoCompact = positiveTokenCount(model.auto_compact_token_limit_tokens);
    const contextSearch = [
      model.model_class,
      contextWindow && tokenNumber.format(contextWindow),
      maxContextWindow && tokenNumber.format(maxContextWindow),
      autoCompact && tokenNumber.format(autoCompact),
      contextWindow ? "context compact compression" : "",
    ].filter(Boolean).join(" ").toLowerCase();
    return !query || model.id.toLowerCase().includes(query) ||
      model.providers.some((provider) => providerNames[provider.id].toLowerCase().includes(query)) ||
      reasoning?.modelClass?.includes(query) ||
      reasoning?.levels.some((level) => level.includes(query)) ||
      contextSearch.includes(query);
  });
  count.textContent = `${visible.length} cataloged model${visible.length === 1 ? "" : "s"}`;
  list.replaceChildren(...visible.map((model) => {
    const article = document.createElement("article");
    const heading = document.createElement("h2");
    const providers = document.createElement("div");
    heading.textContent = model.id;
    providers.dataset.providers = "";
    for (const provider of model.providers) {
      const badge = document.createElement("span");
      badge.dataset.provider = provider.id;
      badge.textContent = providerNames[provider.id];
      badge.title = provider.supported_endpoints.join(", ");
      providers.append(badge);
    }
    article.append(heading, providers);

    const reasoning = reasoningFor(model);
    if (reasoning?.levels.length) {
      const levels = document.createElement("div");
      levels.dataset.reasoningLevels = "";
      const classLabel = reasoning.modelClass ? `${reasoning.modelClass}: ` : "";
      levels.textContent = `Reasoning · ${classLabel}${reasoning.levels.join(", ")}`;
      if (reasoning.defaultLevel) levels.title = `Default: ${reasoning.defaultLevel}`;
      article.append(levels);
    }

    const contextWindow = positiveTokenCount(model.context_window_tokens);
    const maxContextWindow = positiveTokenCount(model.max_context_window_tokens);
    const autoCompact = positiveTokenCount(model.auto_compact_token_limit_tokens);
    if (contextWindow) {
      const context = document.createElement("div");
      context.dataset.contextWindow = "";
      const maxSuffix = maxContextWindow && maxContextWindow !== contextWindow
        ? ` / ${tokenNumber.format(maxContextWindow)} max`
        : "";
      context.textContent = `Context · ${tokenNumber.format(contextWindow)} tokens${maxSuffix}`;
      if (model.model_class) context.title = `Model class: ${model.model_class}`;
      article.append(context);
    }
    if (autoCompact) {
      const compact = document.createElement("div");
      compact.dataset.autoCompact = "";
      compact.textContent = `Auto-compact · ${tokenNumber.format(autoCompact)} tokens`;
      compact.title = "Summarize older conversation state before the physical context window fills";
      article.append(compact);
    }
    return article;
  }));
};

search.addEventListener("input", render);

try {
  const response = await fetch("/uos/models/catalog", { headers: { Accept: "application/json" } });
  if (!response.ok) throw new Error(`Catalog request failed with HTTP ${response.status}`);
  const payload = await response.json();
  assignCatalog(Array.isArray(payload.data) ? payload.data : []);
  summary.replaceChildren(
    ...Object.entries(payload.sources ?? {}).map(([id, source]) => {
      const article = document.createElement("article");
      const name = document.createElement("h2");
      const total = document.createElement("strong");
      const state = document.createElement("span");
      name.textContent = providerNames[id] ?? id;
      total.textContent = String(source.count ?? 0);
      state.textContent = source.status === "available" ? "cataloged models" : "catalog unavailable";
      article.dataset.state = source.status ?? "unavailable";
      article.append(name, total, state);
      return article;
    }),
  );
  render();
} catch (error) {
  count.textContent = "Catalog unavailable";
  list.textContent = error instanceof Error ? error.message : "Unable to load models.";
  toast.error("Catalog unavailable", {
    description: error instanceof Error ? error.message : "Unable to load models.",
  });
}
