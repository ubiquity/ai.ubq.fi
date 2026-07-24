type DenoWithKv = typeof Deno & {
  openKv?: () => Promise<Deno.Kv>;
};

let openPromise: Promise<Deno.Kv | null> | null = null;

const openKv = async (): Promise<Deno.Kv | null> => {
  const denoOpenKv = (Deno as DenoWithKv).openKv;
  if (typeof denoOpenKv !== "function") return null;
  try {
    return await denoOpenKv();
  } catch (error) {
    console.error("[ai.ubq.fi] Failed to open Deno KV (token refresh will be in-memory only):", error);
    return null;
  }
};

export const getKv = (): Promise<Deno.Kv | null> => {
  openPromise ??= openKv();
  return openPromise;
};
