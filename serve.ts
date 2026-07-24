/// <reference lib="deno.ns" />

import handler from "./src/handler.ts";
import { config } from "./src/config.ts";
import { kvPromise } from "./src/kv.ts";
import { reconcileDuePaidFallbacksV3 } from "./src/paid_fallback_ledger.ts";

if (config.isDeploy) {
  Deno.cron("reconcile pending Yunwu billing", "* * * * *", async () => {
    try {
      // KV is optional at process boot. Resolve it only when the scheduled
      // reconciliation actually runs so a slow KV connection cannot prevent
      // a new Deploy revision from reaching the serving state.
      const kv = await kvPromise;
      if (!kv) return;
      await reconcileDuePaidFallbacksV3(Date.now(), kv);
    } catch (error) {
      console.error(
        "[ai.ubq.fi] Scheduled paid fallback reconciliation failed:",
        error instanceof Error ? error.message : String(error),
      );
    }
  });
}

export default { fetch: handler };
