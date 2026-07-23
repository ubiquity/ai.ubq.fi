/// <reference lib="deno.ns" />

import handler from "./src/handler.ts";
import { reconcileDuePaidFallbacksV3 } from "./src/paid_fallback_ledger.ts";

if (Deno.env.get("DENO_DEPLOYMENT_ID")) {
  Deno.cron("reconcile pending Yunwu billing", "* * * * *", async () => {
    try {
      await reconcileDuePaidFallbacksV3();
    } catch (error) {
      console.error(
        "[ai.ubq.fi] Scheduled paid fallback reconciliation failed:",
        error instanceof Error ? error.message : String(error),
      );
    }
  });
}

export default { fetch: handler };
