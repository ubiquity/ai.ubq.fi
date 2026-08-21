/// <reference lib="deno.ns" />

import handler from "./src/handler.ts";
import { config } from "./src/config.ts";
import { getKv } from "./src/kv.ts";
import { reconcileDuePaidFallbacksV3 } from "./src/paid_fallback_ledger.ts";
import { sampleProviderCapacityForCron } from "./src/provider_capacity.ts";

export const createRequestDeliveryLifecycle = (
  requestSignal: AbortSignal,
  completed: Promise<void>,
): Readonly<{ signal: AbortSignal; handoff: () => void }> => {
  const downstream = new AbortController();
  let handedOff = false;
  const abortBeforeHandoff = (): void => {
    if (handedOff || downstream.signal.aborted) return;
    downstream.abort(requestSignal.reason);
  };
  if (requestSignal.aborted) abortBeforeHandoff();
  else requestSignal.addEventListener("abort", abortBeforeHandoff, { once: true });
  void completed.catch((reason) => {
    if (!downstream.signal.aborted) downstream.abort(reason);
  });
  return {
    signal: downstream.signal,
    handoff: () => {
      if (handedOff) return;
      handedOff = true;
      requestSignal.removeEventListener("abort", abortBeforeHandoff);
    },
  };
};

if (config.isDeploy) {
  Deno.cron("reconcile pending metered billing", "* * * * *", async () => {
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

type DeliveryAwareHandler = (
  request: Request,
  delivery: Readonly<{ completed: Promise<void>; downstreamSignal: AbortSignal }>,
) => Promise<Response>;

export const createServeHandler = (
  requestHandler: DeliveryAwareHandler = handler,
): Deno.ServeHandler =>
async (request, info) => {
  const delivery = createRequestDeliveryLifecycle(request.signal, info.completed);
  try {
    return await requestHandler(request, {
      completed: info.completed,
      downstreamSignal: delivery.signal,
    });
  } finally {
    delivery.handoff();
  }
};

const fetch = createServeHandler();

export default { fetch };
