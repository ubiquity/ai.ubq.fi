/// <reference lib="deno.ns" />

import { getKv } from "./src/kv.ts";
import { reconcileDuePaidFallbacksV3 } from "./src/paid_fallback_ledger.ts";
import { prunePromptCacheAnalytics } from "./src/prompt_cache_analytics.ts";
import { sampleProviderCapacityForCron } from "./src/provider_capacity.ts";
import { createServeHandler } from "./src/serve_handler.ts";
import {
  isSentinelProductionRuntime,
  logSentinelIncidentReconcileResult,
  reconcileSentinelIncidentOutboxFromEnvironment,
} from "./src/sentinel_incident_outbox.ts";

Deno.cron("reconcile pending metered billing", "* * * * *", async () => {
  if (!isSentinelProductionRuntime()) return;
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
  if (!isSentinelProductionRuntime()) return;
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

Deno.cron("prune prompt cache analytics", "7 * * * *", async () => {
  if (!isSentinelProductionRuntime()) return;
  try {
    const kv = await getKv();
    if (!kv) return;
    const result = await prunePromptCacheAnalytics({ kv });
    if (result.status === "unavailable") {
      console.warn("[ai.ubq.fi] prompt_cache_analytics", JSON.stringify({ status: "prune_unavailable" }));
    }
  } catch {
    console.warn("[ai.ubq.fi] prompt_cache_analytics", JSON.stringify({ status: "prune_failed" }));
  }
});

Deno.cron("deliver pending Provider Sentinel incidents", "* * * * *", async () => {
  if (!isSentinelProductionRuntime()) return;
  try {
    logSentinelIncidentReconcileResult(await reconcileSentinelIncidentOutboxFromEnvironment());
  } catch {
    console.warn(
      "[ai.ubq.fi] sentinel_incident",
      JSON.stringify({ status: "deferred", reason: "cron_reconcile_failed" }),
    );
  }
});

const fetch = createServeHandler();

export default { fetch };
