/// <reference lib="deno.ns" />

import handler from "./src/handler.ts";
import { kvPromise } from "./src/kv.ts";
import { reconcilePaidFallbackV3 } from "./src/paid_fallback_ledger.ts";

if (Deno.env.get("DENO_DEPLOYMENT_ID")) {
  const kv = await kvPromise;
  kv?.listenQueue(async (message) => {
    if (
      typeof message === "object" &&
      message !== null &&
      (message as { type?: unknown }).type === "paid_fallback_reconcile_v3" &&
      typeof (message as { key_id?: unknown }).key_id === "string"
    ) {
      await reconcilePaidFallbackV3((message as { key_id: string }).key_id);
    }
  });
}

export default { fetch: handler };
