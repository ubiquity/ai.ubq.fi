/// <reference lib="deno.ns" />

import handler from "./src/handler.ts";
import { config } from "./src/config.ts";
import { getKv } from "./src/kv.ts";
import { reconcileDuePaidFallbacksV3 } from "./src/paid_fallback_ledger.ts";
import { sampleProviderCapacityForCron } from "./src/provider_capacity.ts";

if (config.isDeploy) {
  Deno.cron("reconcile pending Yunwu billing", "* * * * *", async () => {
    try {
      // KV is optional at process boot. Resolve it only when the scheduled
      // reconciliation actually runs so a slow KV connection cannot prevent
      // a new Deploy revision from reaching the serving state.
      const kv = await getKv();
      if (!kv) return;
      await reconcileDuePaidFallbacksV3(Date.now(), kv);
    } catch (error) {
      console.error(
        "[ai.ubq.fi] Scheduled paid fallback reconciliation failed:",
        error instanceof Error ? error.message : String(error),
      );
    }
  });

  Deno.cron("sample Codex provider capacity", "*/15 * * * *", async () => {
    try {
      const kv = await getKv();
      if (!kv) return;
      await sampleProviderCapacityForCron({ kv });
    } catch (error) {
      console.error(
        "[ai.ubq.fi] Provider capacity sampler failed:",
        error instanceof Error ? error.message : String(error),
      );
    }
  });
}

export default { fetch: handler };
