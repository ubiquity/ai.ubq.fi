import assert from "node:assert/strict";

import {
  acquirePromptCacheKvCooldown,
  PROMPT_CACHE_KV_COOLDOWN_BASE_MS,
  PROMPT_CACHE_KV_COOLDOWN_JITTER,
  PROMPT_CACHE_KV_COOLDOWN_MIN_MS,
} from "../src/prompt_cache_kv_cooldown.ts";
import { recordPromptCacheAnalytics } from "../src/prompt_cache_analytics.ts";
import { recordPromptCacheTelemetry } from "../src/prompt_cache_telemetry_gate.ts";

const RELEASE = "0123456789abcdef0123456789abcdef01234567";

class TransportKv {
  commitCalls = 0;
  getManyCalls = 0;
  failCommitsRemaining = 0;
  conflictCommitsRemaining = 0;

  getMany(keys: readonly Deno.KvKey[]) {
    this.getManyCalls += 1;
    return Promise.resolve(keys.map((key) => ({ key, value: null, versionstamp: null })));
  }

  atomic(): Deno.AtomicOperation {
    const operation = {
      check: () => operation,
      set: () => operation,
      sum: () => operation,
      commit: () => {
        this.commitCalls += 1;
        if (this.failCommitsRemaining > 0) {
          this.failCommitsRemaining -= 1;
          return Promise.reject(new Error("simulated KV transport outage"));
        }
        if (this.conflictCommitsRemaining > 0) {
          this.conflictCommitsRemaining -= 1;
          return Promise.resolve({ ok: false });
        }
        return Promise.resolve({ ok: true, versionstamp: "00000000000000000001" });
      },
    };
    return operation as unknown as Deno.AtomicOperation;
  }
}

const telemetryEvent = {
  provider: "chatgpt_codex",
  model: "gpt-cache-cooldown",
  route: "responses",
  status: 200,
  completed: true,
  usageTelemetryStatus: "reported",
  cacheWriteTokensPresent: true,
};

const analyticsEvent = {
  provider: "chatgpt_codex",
  model: "gpt-cache-cooldown",
  route: "responses",
  status: 200,
  completed: true,
  usageTelemetryStatus: "reported",
  inputTokens: 100,
  cachedInputTokens: 25,
  cacheWriteInputTokens: 10,
  promptCacheKeyPresent: true,
  promptCacheMode: "explicit",
  fallbackReason: null,
};

Deno.test("prompt-cache writers share a bounded cooldown and admit one successful probe", async () => {
  const kv = new TransportKv();
  let nowMs = 1_000;
  const options = {
    kv: kv as unknown as Deno.Kv,
    release: RELEASE,
    now: () => nowMs,
    random: () => 0.5,
  };

  kv.failCommitsRemaining = 1;
  const firstFailure = await recordPromptCacheTelemetry(telemetryEvent, options);
  assert.equal(firstFailure.status, "unavailable");
  assert.equal(kv.commitCalls, 1);

  const suppressed = await Promise.all([
    ...Array.from({ length: 8 }, () => recordPromptCacheTelemetry(telemetryEvent, options)),
    ...Array.from({ length: 8 }, () => recordPromptCacheAnalytics(analyticsEvent, options)),
  ]);
  assert.ok(suppressed.every((result) => result.status === "unavailable"));
  assert.equal(kv.commitCalls, 1);
  assert.equal(kv.getManyCalls, 0);

  nowMs = 5_999;
  assert.equal((await recordPromptCacheTelemetry(telemetryEvent, options)).status, "unavailable");
  assert.equal((await recordPromptCacheAnalytics(analyticsEvent, options)).status, "unavailable");
  assert.equal(kv.commitCalls, 1);
  assert.equal(kv.getManyCalls, 0);

  nowMs = 6_000;
  const probe = await recordPromptCacheAnalytics(analyticsEvent, options);
  assert.equal(probe.status, "recorded");
  assert.equal(kv.getManyCalls, 1);
  assert.equal(kv.commitCalls, 2);

  const resumed = await recordPromptCacheTelemetry(telemetryEvent, options);
  assert.equal(resumed.status, "recorded");
  assert.equal(kv.commitCalls, 3);
});

Deno.test("prompt-cache cooldown jitters within bounds and serializes its probe window", () => {
  const lowScope = {};
  let lowNowMs = 0;
  const lowLease = acquirePromptCacheKvCooldown(lowScope, { now: () => lowNowMs, random: () => 0 });
  assert.equal(lowLease.admitted, true);
  assert.equal(lowLease.probe, false);
  lowLease.fail();

  lowNowMs = PROMPT_CACHE_KV_COOLDOWN_MIN_MS - 1;
  assert.equal(acquirePromptCacheKvCooldown(lowScope, { now: () => lowNowMs }).admitted, false);

  lowNowMs = PROMPT_CACHE_KV_COOLDOWN_MIN_MS;
  const lowProbe = acquirePromptCacheKvCooldown(lowScope, { now: () => lowNowMs, random: () => 0 });
  assert.equal(lowProbe.admitted, true);
  assert.equal(lowProbe.probe, true);
  assert.equal(acquirePromptCacheKvCooldown(lowScope, { now: () => lowNowMs }).admitted, false);
  lowProbe.succeed();
  assert.equal(acquirePromptCacheKvCooldown(lowScope, { now: () => lowNowMs }).admitted, true);

  const highScope = {};
  let highNowMs = 0;
  const highLease = acquirePromptCacheKvCooldown(highScope, { now: () => highNowMs, random: () => 1 });
  highLease.fail();
  highNowMs = Math.round(PROMPT_CACHE_KV_COOLDOWN_BASE_MS * (1 + PROMPT_CACHE_KV_COOLDOWN_JITTER)) - 1;
  assert.equal(acquirePromptCacheKvCooldown(highScope, { now: () => highNowMs }).admitted, false);
  highNowMs += 1;
  assert.equal(acquirePromptCacheKvCooldown(highScope, { now: () => highNowMs }).probe, true);
});
