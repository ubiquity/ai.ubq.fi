import assert from "node:assert/strict";
import { CodexAffinityCache, deriveCodexAffinityKey, selectWeightedRendezvous } from "../src/codex_affinity.ts";

Deno.test("Codex affinity hashes the documented key precedence without retaining raw identifiers", async () => {
  const prompt = await deriveCodexAffinityKey({
    prompt_cache_key: "prompt-secret",
    client_metadata: { session_id: "session-secret" },
    thread_id: "thread-secret",
  });
  const session = await deriveCodexAffinityKey({
    client_metadata: { session_id: "session-secret" },
    thread_id: "thread-secret",
  });
  const thread = await deriveCodexAffinityKey({ thread_id: "thread-secret" });

  assert.ok(prompt && session && thread);
  assert.notEqual(prompt, session);
  assert.notEqual(session, thread);
  assert.equal(prompt.includes("prompt-secret"), false);
  assert.equal(await deriveCodexAffinityKey({ client_metadata: { session_id: 42 } }), null);
});

Deno.test("Codex affinity is stable and an ineligible assignment is evicted", async () => {
  const key = await deriveCodexAffinityKey({ prompt_cache_key: "stable-session" });
  assert.ok(key);
  const candidates = [
    { id: "slot-a", value: "slot-a", weight: 1 },
    { id: "slot-b", value: "slot-b", weight: 1 },
  ];
  const first = await selectWeightedRendezvous(key, candidates);
  const second = await selectWeightedRendezvous(key, candidates);
  assert.equal(first, second);

  const cache = new CodexAffinityCache();
  cache.set(key, first!);
  assert.equal(cache.get(key, new Set(["slot-a", "slot-b"])), first);
  assert.equal(cache.get(key, new Set(first === "slot-a" ? ["slot-b"] : ["slot-a"])), null);
});

Deno.test("Codex affinity gives a higher-headroom account a proportionally larger rendezvous share", async () => {
  let lowHeadroom = 0;
  let highHeadroom = 0;
  for (let index = 0; index < 200; index += 1) {
    const selected = await selectWeightedRendezvous(`affinity-${index}`, [
      { id: "low", value: "low", weight: 1 },
      { id: "high", value: "high", weight: 3 },
    ]);
    if (selected === "high") highHeadroom += 1;
    else lowHeadroom += 1;
  }
  assert.ok(highHeadroom > lowHeadroom, `high=${highHeadroom}, low=${lowHeadroom}`);
  // A 3:1 rendezvous weight should be near 75%, not the ~94% produced when
  // the 52-bit hash sample was divided by a 53-bit denominator.
  assert.ok(highHeadroom >= 125 && highHeadroom <= 175, `high=${highHeadroom}, low=${lowHeadroom}`);
});
