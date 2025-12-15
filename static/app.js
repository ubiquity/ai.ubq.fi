const badge = document.getElementById("status-badge");

const setBadge = (state, text) => {
  if (!badge) return;
  badge.dataset.state = state;
  badge.textContent = text;
};

try {
  const res = await fetch("/health", { cache: "no-store" });
  const data = await res.json().catch(() => null);
  const ok = Boolean(data?.ok) && res.ok;
  setBadge(ok ? "ok" : "bad", ok ? "OK" : "Degraded");
} catch {
  setBadge("bad", "Offline");
}
