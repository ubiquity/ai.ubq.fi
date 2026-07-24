/// <reference lib="deno.ns" />

import handler from "./src/handler.ts";
import { config } from "./src/config.ts";
import { kvPromise } from "./src/kv.ts";
import { reconcileDuePaidFallbacksV3 } from "./src/paid_fallback_ledger.ts";

if (config.isDeploy) {
  // `kvPromise` fails closed when KV cannot be opened, so a transient KV
  // outage does not prevent the deployment from serving requests.
  const kv = await kvPromise;
  Deno.cron("reconcile pending Yunwu billing", "* * * * *", async () => {
    try {
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
