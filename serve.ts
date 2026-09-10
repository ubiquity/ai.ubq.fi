/// <reference lib="deno.ns" />

import { getKv } from "./src/kv.ts";
import { config } from "./src/config.ts";
import {
  configureAdminAuthForListener,
  configureAdminAuthPeerForRequest,
  parseServeRuntimeOptions,
} from "./src/local_admin_auth.ts";
import { sampleProviderCapacityForCron } from "./src/provider_capacity.ts";
import {
  isScheduledMaintenanceRuntime,
  pruneScheduledPromptCacheAnalytics,
  reconcileScheduledPaidFallbacks,
} from "./src/scheduled_maintenance.ts";
import { createServeHandler } from "./src/serve_handler.ts";
const isProductionRuntime = (): boolean => Deno.env.get("DENO_TIMELINE") === "production";

Deno.cron("reconcile pending metered billing", "* * * * *", async () => {
  if (!isScheduledMaintenanceRuntime()) return;
  await reconcileScheduledPaidFallbacks();
});

Deno.cron("sample Codex provider capacity", "*/15 * * * *", async () => {
  if (!isProductionRuntime()) return;
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
  if (!isScheduledMaintenanceRuntime()) return;
  await pruneScheduledPromptCacheAnalytics();
});

const serveHandler = createServeHandler();

const runtimeOptions = parseServeRuntimeOptions(Deno.args, { isDeploy: config.isDeploy });

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
      console.warn(
        "[ai.ubq.fi] WARNING: admin authentication is disabled for this loopback development server.",
      );
    },
  }
  : { fetch: serveHandler };

export default server;
