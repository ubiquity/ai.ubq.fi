const TRACE_LAYER_ATTR = "data-network-trace-layer";
const TRACE_ATTR = "data-network-trace";
const INSTALL_FLAG = "__ubqNetworkTraceInstalled";
const DEFAULT_DURATION_MS = 1400;
const DEFAULT_ORIGIN_KEY = "default";

const statsByOrigin = new Map();
let cachedDefaultDurationMs = null;

const parseDurationMs = (value) => {
  if (!value) return null;
  const trimmed = value.trim();
  if (!trimmed) return null;
  if (trimmed.endsWith("ms")) {
    const parsed = Number.parseFloat(trimmed.slice(0, -2));
    return Number.isFinite(parsed) ? parsed : null;
  }
  if (trimmed.endsWith("s")) {
    const parsed = Number.parseFloat(trimmed.slice(0, -1));
    return Number.isFinite(parsed) ? parsed * 1000 : null;
  }
  const parsed = Number.parseFloat(trimmed);
  return Number.isFinite(parsed) ? parsed : null;
};

const resolveDefaultDurationMs = () => {
  if (cachedDefaultDurationMs !== null) return cachedDefaultDurationMs;
  if (typeof window === "undefined" || typeof getComputedStyle !== "function") {
    cachedDefaultDurationMs = DEFAULT_DURATION_MS;
    return cachedDefaultDurationMs;
  }
  const cssValue = getComputedStyle(document.documentElement).getPropertyValue("--network-trace-duration");
  cachedDefaultDurationMs = parseDurationMs(cssValue) ?? DEFAULT_DURATION_MS;
  return cachedDefaultDurationMs;
};

const getStats = (originKey) => {
  const key = originKey || DEFAULT_ORIGIN_KEY;
  const stats = statsByOrigin.get(key) ?? { totalDurationMs: 0, requestCount: 0 };
  if (!statsByOrigin.has(key)) statsByOrigin.set(key, stats);
  return stats;
};

const getAverageDurationMs = (originKey) => {
  const stats = getStats(originKey);
  if (stats.requestCount <= 0) return resolveDefaultDurationMs();
  const average = stats.totalDurationMs / stats.requestCount;
  return Number.isFinite(average) && average > 0 ? average : resolveDefaultDurationMs();
};

const recordDuration = (originKey, startMs) => {
  const elapsed = performance.now() - startMs;
  if (!Number.isFinite(elapsed) || elapsed <= 0) return;
  const stats = getStats(originKey);
  stats.totalDurationMs += elapsed;
  stats.requestCount += 1;
};

const resolveOriginKey = (input) => {
  if (!input) return DEFAULT_ORIGIN_KEY;
  try {
    if (input instanceof Request) return new URL(input.url).origin;
    if (input instanceof URL) return input.origin;
    const url = new URL(typeof input === "string" ? input : String(input), window.location.href);
    return url.origin;
  } catch {
    return DEFAULT_ORIGIN_KEY;
  }
};

const ensureTraceLayer = (side) => {
  const sideKey = side === "right" ? "right" : "left";
  let layer = document.querySelector(`[${TRACE_LAYER_ATTR}][data-side="${sideKey}"]`);
  if (layer) return layer;
  const parent = document.body || document.documentElement;
  if (!parent) return null;
  layer = document.createElement("div");
  layer.setAttribute(TRACE_LAYER_ATTR, "");
  layer.setAttribute("data-side", sideKey);
  layer.setAttribute("aria-hidden", "true");
  parent.appendChild(layer);
  return layer;
};

const renderNetworkTrace = (originKey) => {
  const upLayer = ensureTraceLayer("left");
  const downLayer = ensureTraceLayer("right");
  if (!upLayer || !downLayer) return;
  const durationMs = getAverageDurationMs(originKey);
  const halfDurationMs = Math.max(1, durationMs / 2);

  const createTrace = (direction, delayMs) => {
    const trace = document.createElement("div");
    trace.setAttribute(TRACE_ATTR, direction);
    trace.style.animationDuration = `${halfDurationMs}ms`;
    if (delayMs) trace.style.animationDelay = `${delayMs}ms`;
    trace.addEventListener(
      "animationend",
      () => {
        trace.remove();
      },
      { once: true },
    );
    return trace;
  };

  upLayer.appendChild(createTrace("up", 0));
  downLayer.appendChild(createTrace("down", halfDurationMs));
};

const installNetworkTrace = () => {
  if (window[INSTALL_FLAG]) return;
  window[INSTALL_FLAG] = true;
  if (typeof window.fetch !== "function") return;
  const nativeFetch = window.fetch.bind(window);
  window.fetch = (...args) => {
    const startMs = performance.now();
    const originKey = resolveOriginKey(args[0]);
    renderNetworkTrace(originKey);
    const responsePromise = nativeFetch(...args);
    responsePromise.finally(() => {
      recordDuration(originKey, startMs);
    });
    return responsePromise;
  };
};

installNetworkTrace();

export { renderNetworkTrace };
