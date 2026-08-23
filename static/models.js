const summary = document.querySelector("[data-source-summary]");
const list = document.querySelector("[data-model-list]");
const count = document.querySelector("[data-model-count]");
const search = document.querySelector("[data-model-search]");

if (!summary || !list || !count || !(search instanceof HTMLInputElement)) {
  throw new Error("Models page markup is incomplete");
}

const providerNames = { codex: "Codex", openlux: "Metered 2", surplus: "Metered 1" };
let catalog = [];

const render = () => {
  const query = search.value.trim().toLowerCase();
  const visible = catalog.filter((model) =>
    !query || model.id.toLowerCase().includes(query) ||
    model.providers.some((provider) => providerNames[provider.id].toLowerCase().includes(query))
  );
  count.textContent = `${visible.length} model${visible.length === 1 ? "" : "s"}`;
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
    return article;
  }));
};

search.addEventListener("input", render);

try {
  const response = await fetch("/uos/models/catalog", { headers: { Accept: "application/json" } });
  if (!response.ok) throw new Error(`Catalog request failed with HTTP ${response.status}`);
  const payload = await response.json();
  catalog = Array.isArray(payload.data) ? payload.data : [];
  summary.replaceChildren(
    ...Object.entries(payload.sources ?? {}).map(([id, source]) => {
      const article = document.createElement("article");
      const name = document.createElement("h2");
      const total = document.createElement("strong");
      const state = document.createElement("span");
      name.textContent = providerNames[id] ?? id;
      total.textContent = String(source.count ?? 0);
      state.textContent = source.status === "available" ? "available models" : "catalog unavailable";
      article.dataset.state = source.status ?? "unavailable";
      article.append(name, total, state);
      return article;
    }),
  );
  render();
} catch (error) {
  count.textContent = "Catalog unavailable";
  list.textContent = error instanceof Error ? error.message : "Unable to load models.";
}
