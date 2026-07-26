type DenoWithKv = typeof Deno & {
  openKv?: () => Promise<Deno.Kv>;
};

let openPromise: Promise<Deno.Kv | null> | null = null;
let openedKv: Deno.Kv | null = null;
let nextOpenAttemptAtMs = 0;
let openFailureCount = 0;

const retryDelayMs = (failureCount: number): number => {
  const capped = Math.min(5_000, 250 * 2 ** Math.min(5, Math.max(0, failureCount - 1)));
  return Math.trunc(capped * (0.75 + Math.random() * 0.5));
};

const openKv = async (): Promise<Deno.Kv | null> => {
  const denoOpenKv = (Deno as DenoWithKv).openKv;
  if (typeof denoOpenKv !== "function") return null;
  try {
    const kv = await denoOpenKv();
    openedKv = kv;
    openFailureCount = 0;
    nextOpenAttemptAtMs = 0;
    return kv;
  } catch (error) {
    openFailureCount += 1;
    nextOpenAttemptAtMs = Date.now() + retryDelayMs(openFailureCount);
    console.error("[ai.ubq.fi] Failed to open Deno KV; a later request will retry:", error);
    return null;
  }
};

export const getKv = (): Promise<Deno.Kv | null> => {
  if (openedKv) return Promise.resolve(openedKv);
  const denoOpenKv = (Deno as DenoWithKv).openKv;
  if (typeof denoOpenKv !== "function") return Promise.resolve(null);
  if (Date.now() < nextOpenAttemptAtMs) return Promise.resolve(null);
  openPromise ??= openKv().finally(() => {
    openPromise = null;
  });
  return openPromise;
};

export const setKvForTest = (kv: Deno.Kv | null): void => {
  openedKv = kv;
  openPromise = null;
  openFailureCount = 0;
  nextOpenAttemptAtMs = 0;
};
