/**
 * @typedef {EventTarget & { visibilityState?: string }} ForegroundDocumentTarget
 * @typedef {{
 *   windowTarget?: EventTarget,
 *   documentTarget?: ForegroundDocumentTarget,
 *   delayMs?: number,
 * }} ForegroundRefreshOptions
 */

/**
 * @param {() => void} refresh
 * @param {ForegroundRefreshOptions} [options]
 */
export const bindForegroundRefresh = (
  refresh,
  {
    windowTarget = globalThis,
    documentTarget = globalThis.document,
    delayMs = 100,
  } = {},
) => {
  let timer = null;

  const scheduleRefresh = () => {
    if (documentTarget?.visibilityState === "hidden") return;
    if (timer !== null) clearTimeout(timer);
    timer = setTimeout(() => {
      timer = null;
      if (documentTarget?.visibilityState === "hidden") return;
      refresh();
    }, delayMs);
  };

  windowTarget.addEventListener("focus", scheduleRefresh);
  documentTarget?.addEventListener("visibilitychange", scheduleRefresh);

  return () => {
    windowTarget.removeEventListener("focus", scheduleRefresh);
    documentTarget?.removeEventListener("visibilitychange", scheduleRefresh);
    if (timer !== null) clearTimeout(timer);
    timer = null;
  };
};
