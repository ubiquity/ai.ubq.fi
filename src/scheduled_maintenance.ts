import { getKv } from "./kv.ts";
import { reconcileDuePaidFallbacksV3 } from "./paid_fallback_ledger.ts";
import { prunePromptCacheAnalytics } from "./prompt_cache_analytics.ts";

type EnvironmentReader = Readonly<{ get(key: string): string | undefined }>;

/**
 * Billing reconciliation and analytics pruning run on the VPS production
 * timeline and in the Mac companion. The Mac sets DENO_LOCAL_MAINTENANCE so
 * it can maintain its own KV without claiming the production timeline, which
 * would also re-enable production-only automation such as Sentinel.
 */
export const isScheduledMaintenanceRuntime = (
  environment: EnvironmentReader = Deno.env,
): boolean =>
  environment.get("DENO_TIMELINE") === "production" ||
  environment.get("DENO_LOCAL_MAINTENANCE") === "1";

export const reconcileScheduledPaidFallbacks = async (): Promise<void> => {
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
};

export const pruneScheduledPromptCacheAnalytics = async (): Promise<void> => {
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
};
