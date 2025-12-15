type DenoWithKv = typeof Deno & {
  openKv?: () => Promise<Deno.Kv>;
};

export const kvPromise: Promise<Deno.Kv | null> = (async () => {
  const openKv = (Deno as DenoWithKv).openKv;
  if (typeof openKv !== "function") return null;
  try {
    return await openKv();
  } catch (error) {
    console.error("[ai.ubq.fi] Failed to open Deno KV (token refresh will be in-memory only):", error);
    return null;
  }
})();
