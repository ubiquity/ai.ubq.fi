import assert from "node:assert/strict";
import {
  closeCodexRateLimitProbe,
  CODEX_RATE_LIMIT_DEFAULT_COOLDOWN_MS,
  CODEX_RATE_LIMIT_KV_KEY,
  type CodexRateLimitState,
  getCodexRateLimitDecision,
  openCodexRateLimitCircuit,
  retryDeadlineFromHeader,
} from "../src/codex_rate_limit.ts";

const keyString = (key: Deno.KvKey): string => JSON.stringify(key);

class MemoryKv {
  #values = new Map<string, { value: unknown; version: number }>();
  #nextVersion = 1;

  seed(key: Deno.KvKey, value: unknown): void {
    this.#values.set(keyString(key), { value, version: this.#nextVersion++ });
  }

  value<T>(key: Deno.KvKey): T | null {
    return (this.#values.get(keyString(key))?.value as T | undefined) ?? null;
  }

  get<T>(key: Deno.KvKey): Promise<Deno.KvEntryMaybe<T>> {
    const stored = this.#values.get(keyString(key));
    return Promise.resolve({
      key,
      value: (stored?.value as T | undefined) ?? null,
      versionstamp: stored ? String(stored.version).padStart(20, "0") : null,
    } as Deno.KvEntryMaybe<T>);
  }

  atomic(): Deno.AtomicOperation {
    const checks: Deno.KvEntryMaybe<unknown>[] = [];
    const mutations: Array<{ kind: "set" | "delete"; key: Deno.KvKey; value?: unknown }> = [];
    const operation = {
      check: (...entries: Deno.KvEntryMaybe<unknown>[]) => {
        checks.push(...entries);
        return operation;
      },
      set: (key: Deno.KvKey, value: unknown) => {
        mutations.push({ kind: "set", key, value });
        return operation;
      },
      delete: (key: Deno.KvKey) => {
        mutations.push({ kind: "delete", key });
        return operation;
      },
      commit: () => {
        for (const check of checks) {
          const stored = this.#values.get(keyString(check.key));
          const versionstamp = stored ? String(stored.version).padStart(20, "0") : null;
          if (versionstamp !== check.versionstamp) return Promise.resolve({ ok: false } as const);
        }
        for (const mutation of mutations) {
          if (mutation.kind === "delete") this.#values.delete(keyString(mutation.key));
          else this.seed(mutation.key, mutation.value);
        }
        return Promise.resolve({ ok: true, versionstamp: String(this.#nextVersion).padStart(20, "0") } as const);
      },
    };
    return operation as unknown as Deno.AtomicOperation;
  }
}

Deno.test("Codex rate-limit Retry-After parsing honors future values and defaults invalid values", () => {
  const now = Date.parse("2026-07-21T12:00:00.000Z");
  assert.equal(retryDeadlineFromHeader("15", now), now + 15_000);
  assert.equal(retryDeadlineFromHeader("Tue, 21 Jul 2026 12:02:00 GMT", now), now + 120_000);
  for (const value of [null, "", "invalid", "0", "Tue, 21 Jul 2026 11:59:59 GMT"]) {
    assert.equal(retryDeadlineFromHeader(value, now), now + CODEX_RATE_LIMIT_DEFAULT_COOLDOWN_MS);
  }
});

Deno.test("Codex rate-limit circuit caches 429s and never shortens a longer deadline", async () => {
  const kv = new MemoryKv();
  const now = 1_000_000;
  assert.equal(await openCodexRateLimitCircuit(kv as unknown as Deno.Kv, "30", now), now + 30_000);
  assert.deepEqual(await getCodexRateLimitDecision(kv as unknown as Deno.Kv, now + 1), {
    kind: "cached",
    retryAtMs: now + 30_000,
  });

  kv.seed(CODEX_RATE_LIMIT_KV_KEY, { observed_at_ms: now, retry_at_ms: now + 120_000 });
  assert.equal(await openCodexRateLimitCircuit(kv as unknown as Deno.Kv, "10", now + 1_000), now + 120_000);
});

Deno.test("Codex rate-limit expiry grants exactly one atomic recovery probe", async () => {
  const kv = new MemoryKv();
  const now = 2_000_000;
  kv.seed(CODEX_RATE_LIMIT_KV_KEY, { observed_at_ms: now - 60_000, retry_at_ms: now - 1 });

  let nextProbe = 0;
  const decisions = await Promise.all(
    Array.from(
      { length: 20 },
      () => getCodexRateLimitDecision(kv as unknown as Deno.Kv, now, () => `probe-${++nextProbe}`),
    ),
  );
  assert.equal(decisions.filter((decision) => decision.kind === "probe").length, 1);
  assert.equal(decisions.filter((decision) => decision.kind === "cached").length, 19);
});

Deno.test("Codex rate-limit probe closes on non-429 and stale probes cannot clear newer state", async () => {
  const kv = new MemoryKv();
  const now = 3_000_000;
  kv.seed(CODEX_RATE_LIMIT_KV_KEY, { observed_at_ms: now - 60_000, retry_at_ms: now - 1 });
  const decision = await getCodexRateLimitDecision(kv as unknown as Deno.Kv, now, () => "probe-current");
  assert.deepEqual(decision, { kind: "probe", probeId: "probe-current" });
  await closeCodexRateLimitProbe(kv as unknown as Deno.Kv, "probe-current");
  assert.equal(kv.value(CODEX_RATE_LIMIT_KV_KEY), null);

  kv.seed(CODEX_RATE_LIMIT_KV_KEY, { observed_at_ms: now - 60_000, retry_at_ms: now - 1 });
  const staleDecision = await getCodexRateLimitDecision(kv as unknown as Deno.Kv, now, () => "probe-stale");
  assert.deepEqual(staleDecision, { kind: "probe", probeId: "probe-stale" });
  assert.equal(await openCodexRateLimitCircuit(kv as unknown as Deno.Kv, "90", now), now + 90_000);
  await closeCodexRateLimitProbe(kv as unknown as Deno.Kv, "probe-stale");
  assert.deepEqual(kv.value<CodexRateLimitState>(CODEX_RATE_LIMIT_KV_KEY), {
    observed_at_ms: now,
    retry_at_ms: now + 90_000,
  });

  kv.seed(
    CODEX_RATE_LIMIT_KV_KEY,
    {
      observed_at_ms: now,
      retry_at_ms: now + 60_000,
    } satisfies CodexRateLimitState,
  );
  await closeCodexRateLimitProbe(kv as unknown as Deno.Kv, "probe-current");
  assert.notEqual(kv.value(CODEX_RATE_LIMIT_KV_KEY), null);
});

Deno.test("Codex rate-limit circuit fails open when KV is unavailable", async () => {
  assert.deepEqual(await getCodexRateLimitDecision(null), { kind: "primary" });
  const failingKv = {
    get: () => Promise.reject(new Error("KV unavailable")),
  } as unknown as Deno.Kv;
  assert.deepEqual(await getCodexRateLimitDecision(failingKv), { kind: "primary" });
});
