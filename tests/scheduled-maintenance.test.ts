import assert from "node:assert/strict";

import macServiceSource from "../scripts/serve-mac.ts" with { type: "text" };
import { setKvForTest } from "../src/kv.ts";
import {
  admitPaidFallbackV3,
  paidFallbackPendingV3Key,
  paidFallbackRequestV3Key,
  paidFallbackWindowV3Key,
  updatePaidFallbackRequestV3,
} from "../src/paid_fallback_ledger.ts";
import {
  PROMPT_CACHE_ANALYTICS_BUCKET_MS,
  PROMPT_CACHE_ANALYTICS_RETENTION_MS,
  promptCacheAnalyticsCounterKey,
} from "../src/prompt_cache_analytics.ts";
import {
  isScheduledMaintenanceRuntime,
  pruneScheduledPromptCacheAnalytics,
  reconcileScheduledPaidFallbacks,
} from "../src/scheduled_maintenance.ts";
import { isSentinelProductionRuntime } from "../src/sentinel_incident_outbox.ts";
import { CountingKv } from "./helpers/counting_kv.ts";

const environment = (values: Record<string, string>) => ({
  get: (key: string): string | undefined => values[key],
});

const bucketStart = (atMs: number): number =>
  Math.floor(atMs / PROMPT_CACHE_ANALYTICS_BUCKET_MS) * PROMPT_CACHE_ANALYTICS_BUCKET_MS;

Deno.test("scheduled maintenance runs on the VPS and Mac companion without enabling Sentinel", () => {
  assert.equal(isScheduledMaintenanceRuntime(environment({ DENO_TIMELINE: "production" })), true);
  assert.equal(isScheduledMaintenanceRuntime(environment({ DENO_LOCAL_MAINTENANCE: "1" })), true);
  assert.equal(isScheduledMaintenanceRuntime(environment({})), false);
  assert.equal(
    isSentinelProductionRuntime(environment({
      DENO_LOCAL_MAINTENANCE: "1",
      DENO_DEPLOY_ORG_SLUG: "ubiquity-dao",
      DENO_DEPLOY_APP_SLUG: "ai-ubq-fi",
    })),
    false,
    "the Mac maintenance flag must not enable production-only Sentinel automation",
  );
});

Deno.test("Mac launcher enables local maintenance while keeping the production timeline out", () => {
  assert.match(macServiceSource, /Deno\.env\.set\("DENO_LOCAL_MAINTENANCE", "1"\)/);
  assert.doesNotMatch(macServiceSource, /DENO_TIMELINE.*production/u);
  const installIndex = macServiceSource.indexOf("initializeKv(kv)");
  const serveImportIndex = macServiceSource.indexOf('import(new URL("serve.ts", release).href)');
  assert.ok(installIndex >= 0, "the Mac launcher must install its local KV");
  assert.ok(serveImportIndex > installIndex, "the local KV must be installed before serve.ts is imported");
});

Deno.test("scheduled Mac maintenance settles capped billing against the local KV", async () => {
  const kv = new CountingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  const originalFetch = globalThis.fetch;
  const originalGet = Deno.env.get;
  const keyId = "mac-billing-maintenance";
  const requestId = "mac-billing-request";
  const providerRequestId = "mac-provider-request";
  const now = Date.now();
  const resetAtMs = now + 60_000;
  let fetchCalls = 0;
  Deno.env.get = (key: string): string | undefined =>
    key === "METERED_API_KEY" ? "metered-test-key" : originalGet.call(Deno.env, key);
  globalThis.fetch = () => {
    fetchCalls += 1;
    return Promise.resolve(
      Response.json({
        success: true,
        data: [{
          request_id: providerRequestId,
          quota: 10_000,
          prompt_tokens: 1,
          completion_tokens: 2,
          model_name: "gpt-5-codex",
          created_at: Math.trunc(Date.now() / 1_000),
        }],
      }),
    );
  };
  try {
    const admission = await admitPaidFallbackV3({
      keyId,
      requestId,
      createdAtMs: now,
      policyVersion: "policy-v3",
      limitMicrocredits: 1_000_000,
      maximumExposureMicrocredits: 250_000,
      initialSettledMicrocredits: 0,
      quotaPerCredit: 500_000,
      windowResetAtMs: resetAtMs,
      model: "gpt-5-codex",
      route: "responses",
      path: "/v1/responses",
      stream: true,
      reasoning: "high",
      dispatchIntent: true,
    });
    assert.equal(admission.kind, "reserved");
    if (admission.kind !== "reserved") throw new Error("expected a paid fallback reservation");
    await updatePaidFallbackRequestV3(admission.reservation, {
      provider_request_id: providerRequestId,
      dispatch_state: "dispatched",
    });

    await reconcileScheduledPaidFallbacks();

    assert.equal(fetchCalls, 1);
    const request = await kv.get<Record<string, unknown>>(paidFallbackRequestV3Key(keyId, requestId));
    assert.equal(request.value?.billing_state, "settled");
    const window = await kv.get<Record<string, unknown>>(paidFallbackWindowV3Key(keyId, resetAtMs));
    assert.equal(window.value?.reserved_microcredits, 0);
    assert.equal(window.value?.pending_count, 0);
    const pending = await kv.get(paidFallbackPendingV3Key(keyId, requestId));
    assert.equal(pending.value, null);
  } finally {
    globalThis.fetch = originalFetch;
    Deno.env.get = originalGet;
    setKvForTest(null);
  }
});

Deno.test("scheduled Mac maintenance prunes analytics against the local KV", async () => {
  const kv = new CountingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  const expiredKey = promptCacheAnalyticsCounterKey(
    bucketStart(Date.now() - PROMPT_CACHE_ANALYTICS_RETENTION_MS - PROMPT_CACHE_ANALYTICS_BUCKET_MS),
    "sample_count",
  );
  const freshKey = promptCacheAnalyticsCounterKey(bucketStart(Date.now()), "sample_count");
  // Pruning inspects key buckets, so the fixture values do not need KvU64.
  kv.seed(expiredKey, 1n);
  kv.seed(freshKey, 1n);
  try {
    await pruneScheduledPromptCacheAnalytics();
    assert.equal(kv.entries.has(JSON.stringify(expiredKey)), false);
    assert.equal(kv.entries.has(JSON.stringify(freshKey)), true);
  } finally {
    setKvForTest(null);
  }
});
