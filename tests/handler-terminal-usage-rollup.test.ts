import assert from "node:assert/strict";

import { withTerminalRequestLog } from "../src/handler.ts";
import { setKvForTest } from "../src/kv.ts";
import { CountingKv } from "./helpers/counting_kv.ts";

type TerminalLogInput = Parameters<typeof withTerminalRequestLog>[1];
type RecordUsageRollup = NonNullable<TerminalLogInput["recordUsageRollup"]>;
type UsageRollupInput = Parameters<RecordUsageRollup>[0];
type RecordTelemetry = NonNullable<TerminalLogInput["recordTelemetry"]>;
type RecordAnalytics = NonNullable<TerminalLogInput["recordCacheAnalytics"]>;

const ignoredTelemetry: RecordTelemetry = () =>
  Promise.resolve({
    status: "ignored" as const,
    reason: "unknown_release" as const,
    release: null,
    provider: null,
    route: null,
    model_hash: null,
  });

const ignoredAnalytics: RecordAnalytics = () =>
  Promise.resolve({
    status: "ignored" as const,
    reason: "unknown_release" as const,
    bucket_start_at_ms: null,
  });

/** A terminal response labelled with the Codex subscription route. */
const codexResponse = (): Response =>
  new Response("complete", { status: 200, headers: { "Content-Type": "application/json", "x-uos-upstream": "chatgpt_codex" } });

const terminalOptions = (requestId: string, extra: Partial<TerminalLogInput> = {}): TerminalLogInput => ({
  route: "responses",
  startedAtMonotonicMs: performance.now(),
  requestId,
  recordTelemetry: ignoredTelemetry,
  recordCacheAnalytics: ignoredAnalytics,
  ...extra,
});

Deno.test("terminal usage accounting records exactly one Codex observation with the request hour", async () => {
  const observations: UsageRollupInput[] = [];
  const requestStartedAtMs = Date.parse("2026-09-22T16:00:00Z");
  const response = await withTerminalRequestLog(
    codexResponse(),
    terminalOptions("handler-usage-rollup-codex", {
      requestStartedAtMs,
      recordUsageRollup: (input) => {
        observations.push(input);
        return Promise.resolve(true);
      },
    })
  );

  assert.equal(response.status, 200);
  assert.equal(await response.text(), "complete");
  assert.equal(observations.length, 1, "one terminal response records exactly one usage observation");
  assert.deepEqual(observations[0], {
    model: null,
    provider: "chatgpt_codex",
    request_id: "handler-usage-rollup-codex",
    request_created_at_ms: requestStartedAtMs,
    input_tokens: null,
    cached_input_tokens: null,
    output_tokens: null,
  });
});

Deno.test("terminal usage accounting falls back to the terminal clock on rejection paths", async () => {
  const observations: UsageRollupInput[] = [];
  const before = Date.now();
  const response = await withTerminalRequestLog(
    codexResponse(),
    terminalOptions("handler-usage-rollup-rejection", {
      recordUsageRollup: (input) => {
        observations.push(input);
        return Promise.resolve(true);
      },
    })
  );
  const after = Date.now();

  assert.equal(response.status, 200);
  assert.equal(await response.text(), "complete");
  assert.equal(observations.length, 1);
  const observed = observations[0].request_created_at_ms;
  assert.ok(
    typeof observed === "number" && observed >= before && observed <= after,
    `a rejection path must bucket with the terminal wall clock, observed ${observed} outside ${before}..${after}`
  );
});

Deno.test("a failed terminal usage-rollup write never changes the terminal response", async () => {
  let attempts = 0;
  const response = await withTerminalRequestLog(
    codexResponse(),
    terminalOptions("handler-usage-rollup-failure", {
      recordUsageRollup: () => {
        attempts += 1;
        return Promise.reject(new Error("terminal usage rollup KV unavailable"));
      },
    })
  );

  assert.equal(attempts, 1, "the failed observation must still be attempted exactly once");
  assert.equal(response.status, 200);
  assert.equal(await response.text(), "complete");
});

Deno.test("terminal usage accounting performs no KV work for settled, aggregate or route-less terminals", async () => {
  const kv = new CountingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  try {
    for (const provider of ["metered", "surplus", "mixed"]) {
      const response = await withTerminalRequestLog(
        new Response("complete", { status: 200, headers: { "Content-Type": "application/json", "x-uos-upstream": provider } }),
        terminalOptions(`handler-usage-rollup-skip-${provider}`)
      );
      assert.equal(await response.text(), "complete");
    }
    const gateway = await withTerminalRequestLog(
      new Response("complete", { status: 200, headers: { "Content-Type": "application/json" } }),
      terminalOptions("handler-usage-rollup-skip-gateway")
    );
    assert.equal(await gateway.text(), "complete");
    assert.equal(kv.commands.length, 0, "the usage-rollup writer must skip settled providers, aggregate labels and route-less terminals before any KV call");
    assert.equal(kv.entries.size, 0, "a skipped observation must not create a rollup entry");
  } finally {
    setKvForTest(null);
  }
});
