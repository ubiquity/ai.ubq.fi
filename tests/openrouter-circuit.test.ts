import assert from "node:assert/strict";
import { setKvForTest } from "../src/kv.ts";
import {
  claimOpenRouterEarlyRecoveryProbe,
  closeOpenRouterCircuit,
  getOpenRouterCircuitView,
  OPENROUTER_CIRCUIT_KEY,
  OPENROUTER_OPEN_MS,
  parseOpenRouterCircuitState,
  recordOpenRouterEligibleFailure,
  releaseOpenRouterCircuitProbe,
  selectOpenRouterCircuitRoute,
} from "../src/openrouter_circuit.ts";
import { CountingKv } from "./helpers/counting_kv.ts";

const withKv = async (run: (kv: CountingKv) => Promise<void>): Promise<void> => {
  const kv = new CountingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  try {
    await run(kv);
  } finally {
    setKvForTest(null);
  }
};

Deno.test("OpenRouter circuit opens after two eligible failures in sixty seconds", async () => {
  await withKv(async () => {
    assert.equal(await recordOpenRouterEligibleFailure(null, 1_000), "none");
    assert.equal((await selectOpenRouterCircuitRoute(1_001)).route, "codex");
    assert.equal(await recordOpenRouterEligibleFailure(null, 60_999), "opened");
    assert.deepEqual(await selectOpenRouterCircuitRoute(61_000), {
      route: "openrouter",
      reason: "open",
      probe: null,
      transition: "none",
    });
  });
});

Deno.test("OpenRouter circuit ignores a stale failure outside the rolling window", async () => {
  await withKv(async () => {
    await recordOpenRouterEligibleFailure(null, 1_000);
    assert.equal(await recordOpenRouterEligibleFailure(null, 61_001), "none");
    assert.equal((await getOpenRouterCircuitView(61_001)).recent_failures, 1);
  });
});

Deno.test("OpenRouter circuit grants exactly one half-open claim and closes on semantic recovery", async () => {
  await withKv(async () => {
    await recordOpenRouterEligibleFailure(null, 100);
    await recordOpenRouterEligibleFailure(null, 101);
    const now = 101 + OPENROUTER_OPEN_MS;
    let ownerIndex = 0;
    const first = await selectOpenRouterCircuitRoute(now, () => `owner-${++ownerIndex}`);
    const concurrent = await selectOpenRouterCircuitRoute(now, () => `owner-${++ownerIndex}`);
    assert.equal(first.route, "codex");
    assert.equal(first.reason, "probe");
    assert.equal(concurrent.route, "openrouter");
    assert.equal(concurrent.reason, "concurrent_probe");
    assert.ok(first.probe);
    assert.equal(await closeOpenRouterCircuit(first.probe, now + 1), "closed");
    assert.equal((await selectOpenRouterCircuitRoute(now + 2)).route, "codex");
  });
});

Deno.test("OpenRouter failed probe reopens for two minutes", async () => {
  await withKv(async () => {
    await recordOpenRouterEligibleFailure(null, 100);
    await recordOpenRouterEligibleFailure(null, 101);
    const now = 101 + OPENROUTER_OPEN_MS;
    const probe = await selectOpenRouterCircuitRoute(now, () => "probe-owner");
    assert.ok(probe.probe);
    assert.equal(await recordOpenRouterEligibleFailure(probe.probe, now + 1), "reopened");
    assert.equal((await selectOpenRouterCircuitRoute(now + 2)).route, "openrouter");
  });
});

Deno.test("OpenRouter circuit preserves a live half-open probe when an older ordinary request fails", async () => {
  await withKv(async () => {
    await recordOpenRouterEligibleFailure(null, 100);
    await recordOpenRouterEligibleFailure(null, 101);
    const now = 101 + OPENROUTER_OPEN_MS;
    const owner = await selectOpenRouterCircuitRoute(now, () => "probe-owner");
    assert.ok(owner.probe);

    assert.equal(await recordOpenRouterEligibleFailure(null, now + 1), "none");
    assert.deepEqual(await getOpenRouterCircuitView(now + 1), {
      available: true,
      state: "half_open",
      open_until_ms: 101 + OPENROUTER_OPEN_MS,
      recent_failures: 0,
      probe_active: true,
    });
    assert.equal(await closeOpenRouterCircuit(owner.probe, now + 2), "closed");
    assert.equal((await selectOpenRouterCircuitRoute(now + 3)).route, "codex");
  });
});

Deno.test("OpenRouter early recovery claim is atomic and uses the same recovery transitions", async () => {
  await withKv(async () => {
    await recordOpenRouterEligibleFailure(null, 100);
    await recordOpenRouterEligibleFailure(null, 101);
    let ownerIndex = 0;
    const claims = await Promise.all(
      Array.from({ length: 20 }, () => claimOpenRouterEarlyRecoveryProbe(102, () => `early-${++ownerIndex}`)),
    );
    const probes = claims.filter((value): value is NonNullable<typeof value> => value !== null);
    assert.equal(probes.length, 1);
    assert.equal(await closeOpenRouterCircuit(probes[0]!, 103), "closed");
  });
});

Deno.test("OpenRouter circuit fences stale probes and releases matching cancellation", async () => {
  await withKv(async () => {
    await recordOpenRouterEligibleFailure(null, 100);
    await recordOpenRouterEligibleFailure(null, 101);
    const first = await selectOpenRouterCircuitRoute(101 + OPENROUTER_OPEN_MS, () => "first");
    assert.ok(first.probe);
    assert.equal(await releaseOpenRouterCircuitProbe(first.probe, 101 + OPENROUTER_OPEN_MS + 1), "released");
    const second = await selectOpenRouterCircuitRoute(101 + OPENROUTER_OPEN_MS + 2, () => "second");
    assert.ok(second.probe);
    assert.notEqual(first.probe.generation, second.probe.generation);
    assert.equal(await closeOpenRouterCircuit(first.probe, 101 + OPENROUTER_OPEN_MS + 3), "none");
    assert.equal(await recordOpenRouterEligibleFailure(first.probe, 101 + OPENROUTER_OPEN_MS + 3), "none");
    assert.equal(await closeOpenRouterCircuit(second.probe, 101 + OPENROUTER_OPEN_MS + 4), "closed");
  });
});

Deno.test("OpenRouter circuit rejects malformed state and safely replaces stale state", async () => {
  await withKv(async (kv) => {
    assert.equal(parseOpenRouterCircuitState({ v: 1, failure_at_ms: ["bad"] }), null);
    kv.seed(OPENROUTER_CIRCUIT_KEY, { unexpected: "stale" });
    assert.equal((await selectOpenRouterCircuitRoute(10)).route, "codex");
    assert.equal(await recordOpenRouterEligibleFailure(null, 10), "none");
    assert.equal((await getOpenRouterCircuitView(10)).recent_failures, 1);
  });
});
