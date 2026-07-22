import assert from "node:assert/strict";
import {
  closeCodexRateLimitProbe,
  CODEX_RATE_LIMIT_DEFAULT_COOLDOWN_MS,
  CODEX_RATE_LIMIT_KV_KEY,
  getCodexRateLimitDecision,
  openCodexRateLimitCircuit,
  retryDeadlineFromHeader,
} from "../src/codex_rate_limit.ts";

const encode = (key: Deno.KvKey): string => JSON.stringify(key);

class CircuitKv {
  readonly values = new Map<string, { value: unknown; version: number }>();
  nextVersion = 1;

  seed(key: Deno.KvKey, value: unknown): void {
    this.values.set(encode(key), { value, version: this.nextVersion++ });
  }

  get<T>(key: Deno.KvKey): Promise<Deno.KvEntryMaybe<T>> {
    const entry = this.values.get(encode(key));
    return Promise.resolve({
      key,
      value: (entry?.value as T | undefined) ?? null,
      versionstamp: entry ? String(entry.version) : null,
    } as Deno.KvEntryMaybe<T>);
  }

  atomic(): Deno.AtomicOperation {
    const checks: Deno.KvEntryMaybe<unknown>[] = [];
    const writes: Array<{ key: Deno.KvKey; value?: unknown; deleted?: boolean }> = [];
    const operation = {
      check: (...entries: Deno.KvEntryMaybe<unknown>[]) => {
        checks.push(...entries);
        return operation;
      },
      set: (key: Deno.KvKey, value: unknown) => {
        writes.push({ key, value });
        return operation;
      },
      delete: (key: Deno.KvKey) => {
        writes.push({ key, deleted: true });
        return operation;
      },
      commit: () => {
        for (const check of checks) {
          const current = this.values.get(encode(check.key));
          if ((current ? String(current.version) : null) !== check.versionstamp) {
            return Promise.resolve({ ok: false } as const);
          }
        }
        for (const write of writes) {
          if (write.deleted) this.values.delete(encode(write.key));
          else this.seed(write.key, write.value);
        }
        return Promise.resolve({ ok: true, versionstamp: String(this.nextVersion) });
      },
    };
    return operation as unknown as Deno.AtomicOperation;
  }
}

Deno.test("Codex circuit honors Retry-After and never shortens a cooldown", async () => {
  const now = Date.parse("2026-07-22T12:00:00Z");
  assert.equal(retryDeadlineFromHeader("15", now), now + 15_000);
  assert.equal(retryDeadlineFromHeader("Wed, 22 Jul 2026 12:02:00 GMT", now), now + 120_000);
  assert.equal(retryDeadlineFromHeader("invalid", now), now + CODEX_RATE_LIMIT_DEFAULT_COOLDOWN_MS);

  const kv = new CircuitKv();
  assert.equal(await openCodexRateLimitCircuit(kv as unknown as Deno.Kv, "30", now), now + 30_000);
  kv.seed(CODEX_RATE_LIMIT_KV_KEY, { observed_at_ms: now, retry_at_ms: now + 120_000 });
  assert.equal(await openCodexRateLimitCircuit(kv as unknown as Deno.Kv, "10", now + 1_000), now + 120_000);
});

Deno.test("Codex circuit grants one recovery probe and stale probes cannot clear newer cooldowns", async () => {
  const kv = new CircuitKv();
  const now = 2_000_000;
  kv.seed(CODEX_RATE_LIMIT_KV_KEY, { observed_at_ms: now - 60_000, retry_at_ms: now - 1 });
  let sequence = 0;
  const decisions = await Promise.all(
    Array.from(
      { length: 20 },
      () => getCodexRateLimitDecision(kv as unknown as Deno.Kv, now, () => `probe-${++sequence}`),
    ),
  );
  assert.equal(decisions.filter((decision) => decision.kind === "probe").length, 1);

  const probe = decisions.find((decision) => decision.kind === "probe");
  assert.ok(probe && probe.kind === "probe");
  await openCodexRateLimitCircuit(kv as unknown as Deno.Kv, "90", now);
  await closeCodexRateLimitProbe(kv as unknown as Deno.Kv, probe.probeId);
  const state = (await kv.get<{ retry_at_ms?: number }>(CODEX_RATE_LIMIT_KV_KEY)).value;
  assert.equal(state?.retry_at_ms, now + 90_000);
});

Deno.test("Codex circuit fails open when KV is unavailable", async () => {
  assert.deepEqual(await getCodexRateLimitDecision(null), { kind: "primary" });
  const failing = { get: () => Promise.reject(new Error("offline")) } as unknown as Deno.Kv;
  assert.deepEqual(await getCodexRateLimitDecision(failing), { kind: "primary" });
});
