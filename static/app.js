import "./network.js";

const getBadge = () => (typeof document === "undefined" ? null : document.getElementById("status-badge"));

const isHealthAvailable = (response, data) => {
  return Boolean(response?.ok && data?.status === "available");
};

const setBadge = (badge, state, text) => {
  if (!badge) return;
  badge.dataset.state = state;
  badge.textContent = text;
};

const refreshHealthBadge = async (fetcher = globalThis.fetch, badge = getBadge()) => {
  const actualFetcher = typeof fetcher === "function" ? fetcher : globalThis.fetch;
  try {
    const res = await actualFetcher("/health", { cache: "no-store" });
    const data = await res.json().catch(() => null);
    const ok = isHealthAvailable(res, data);
    setBadge(badge, ok ? "ok" : "bad", ok ? "OK" : "Degraded");
  } catch {
    setBadge(badge, "bad", "Offline");
  }
};

export { isHealthAvailable, refreshHealthBadge };

if (typeof document !== "undefined") {
  await refreshHealthBadge();
}
