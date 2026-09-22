/// <reference lib="deno.ns" />

import { config } from "./src/config.ts";
import { getKv } from "./src/kv.ts";
import { configureAdminAuthForListener, configureAdminAuthPeerForRequest, parseServeRuntimeOptions } from "./src/local_admin_auth.ts";
import { ensureLocalDevelopmentApiKey } from "./src/local_development_key.ts";
import { closeOptionalPromptCacheAnalytics, optionalPromptCacheAnalyticsSnapshot } from "./src/prompt_cache_analytics.ts";
import { createServeHandler } from "./src/serve_handler.ts";

/**
 * Bounded optional-telemetry shutdown for both launchers.
 *
 * Optional aggregate analytics is the only lossy terminal write, and it is
 * drained after the server's in-flight work settles and before the KV handle
 * closes. The queue's absolute drain deadline ends the wait even when an
 * optional write never progresses, so a stalled sink cannot hang shutdown; the
 * unresolved entry stays retained as bounded capacity until process exit
 * instead of being orphaned or retried. One sanitized snapshot then makes
 * drops, failures, retained capacity and an incomplete drain operationally
 * observable. Required durable evidence - quota/accounting, admin errors and
 * authenticated failure capture - is never routed through this queue.
 */
export const shutdownOptionalTelemetry = async (): Promise<void> => {
  try {
    await closeOptionalPromptCacheAnalytics();
  } catch {
    // The queue's close settles by contract; a fault still must not block exit.
  }
  const snapshot = optionalPromptCacheAnalyticsSnapshot();
  if (!snapshot) return;
  try {
    console.info(
      "[ai.ubq.fi] optional_telemetry_shutdown",
      JSON.stringify({
        enqueued: snapshot.enqueued,
        delivered: snapshot.delivered,
        failed: snapshot.failed,
        dropped_by_entries: snapshot.dropped_by_entries,
        dropped_by_bytes: snapshot.dropped_by_bytes,
        dropped_by_age: snapshot.dropped_by_age,
        dropped_after_closed: snapshot.dropped_after_closed,
        drain_timeouts: snapshot.drain_timeouts,
        // Charged entries and bytes include unresolved in-flight writes, so a
        // stalled sink is visible as retained capacity instead of a gap.
        retained_entries: snapshot.retained_entries,
        retained_bytes: snapshot.retained_bytes,
        queued_entries: snapshot.queued_entries,
        writes_in_flight: snapshot.writes_in_flight,
        last_error_class: snapshot.last_error_class,
        shutdown_incomplete: snapshot.shutdown_incomplete,
      })
    );
  } catch {
    // Shutdown telemetry is best effort and cannot block process exit.
  }
};

/**
 * No scheduled work runs in this process. Everything the deploy crons used to do
 * now happens because an event happened, and the mapping is deliberate:
 *
 * - "reconcile pending metered billing" (every minute) -> a paid-fallback request
 *   reaching a terminal state (`src/paid_fallback.ts`) or an operator reading the
 *   paid-fallback ledger (`src/admin.ts`).
 * - "sample Codex provider capacity" (every 15 minutes) -> a capacity observation
 *   in `src/codex.ts` (quota exhaustion, upstream outage or unreachable host, a
 *   verified banked reset, or a served request), or an operator asking for a live
 *   view (`?refresh=live`). One probe per fifteen-minute history bucket,
 *   lease-guarded.
 * - "prune prompt cache analytics" (hourly) -> the first analytics write in a new
 *   bucket (`src/prompt_cache_analytics.ts`).
 *
 * Consequences are intentional: with no traffic and no operator, nothing runs.
 * Durable state (pending reconciliation markers, capacity buckets, retained
 * analytics) waits for the next event instead of a timer, and `deno.json` no
 * longer enables the `cron` unstable feature, so `Deno.cron` does not exist here.
 * See `docs/event-driven-maintenance.md`.
 */
const serveHandler = createServeHandler();

const runtimeOptions = parseServeRuntimeOptions(Deno.args, { isDeploy: config.isDeploy });

// `--disable-admin-auth` is loopback-only (a non-loopback listener fails at
// startup), so provisioning here can never reach a hosted deployment. The local
// development key makes loopback requests a super-admin inference principal
// with unlimited paid-provider routing instead of a policy-free one.
if (runtimeOptions.disableAdminAuth) {
  try {
    const kv = await getKv();
    if (kv) {
      const status = await ensureLocalDevelopmentApiKey(kv);
      if (status === "created") console.log("[ai.ubq.fi] Provisioned the local development API key for loopback inference.");
      else if (status === "revoked") console.warn("[ai.ubq.fi] The local development API key is revoked; local paid-provider routing stays off.");
    }
  } catch (error) {
    console.warn("[ai.ubq.fi] Local development key provisioning failed:", error instanceof Error ? error.message : String(error));
  }
}

const server: Deno.ServeDefaultExport = runtimeOptions.disableAdminAuth
  ? {
      fetch(request, info) {
        configureAdminAuthPeerForRequest(info.remoteAddr);
        return serveHandler(request, info);
      },
      onListen(address) {
        configureAdminAuthForListener(runtimeOptions, address);
        const netAddress = address as Deno.NetAddr;
        const hostname = netAddress.hostname.includes(":") ? `[${netAddress.hostname}]` : netAddress.hostname;
        console.log(`Listening on http://${hostname}:${netAddress.port}/`);
        console.warn("[ai.ubq.fi] WARNING: admin authentication is disabled for this loopback development server.");
      },
    }
  : { fetch: serveHandler };

export default server;
