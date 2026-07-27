import assert from "node:assert/strict";

type StoredEntry = {
  key: Deno.KvKey;
  value: unknown;
  versionstamp: string;
};

const encodeKey = (key: Deno.KvKey): string => JSON.stringify(key);
const clone = <T>(value: T): T => structuredClone(value);
const startsWithKey = (key: Deno.KvKey, prefix: Deno.KvKey): boolean =>
  prefix.every((part, index) => key[index] === part);

class MemoryKv {
  readonly entries = new Map<string, StoredEntry>();
  #version = 0;

  #nextVersionstamp(): string {
    this.#version += 1;
    return String(this.#version).padStart(20, "0");
  }

  get<T = unknown>(key: Deno.KvKey): Promise<Deno.KvEntryMaybe<T>> {
    const entry = this.entries.get(encodeKey(key));
    return Promise.resolve({
      key: clone(key),
      value: entry ? clone(entry.value) as T : null,
      versionstamp: entry?.versionstamp ?? null,
    } as Deno.KvEntryMaybe<T>);
  }

  set(key: Deno.KvKey, value: unknown): Promise<Deno.KvCommitResult> {
    const versionstamp = this.#nextVersionstamp();
    this.entries.set(encodeKey(key), {
      key: clone(key),
      value: clone(value),
      versionstamp,
    });
    return Promise.resolve({ ok: true, versionstamp });
  }

  list<T = unknown>(selector: Deno.KvListSelector): Deno.KvListIterator<T> {
    const prefix = "prefix" in selector ? selector.prefix : [];
    const entries = [...this.entries.values()].filter((entry) => startsWithKey(entry.key, prefix));
    const iterator = (async function* (): AsyncGenerator<Deno.KvEntry<T>> {
      for (const entry of entries) {
        yield {
          key: clone(entry.key),
          value: clone(entry.value) as T,
          versionstamp: entry.versionstamp,
        };
      }
    })() as unknown as Deno.KvListIterator<T>;
    Object.defineProperty(iterator, "cursor", { get: () => "" });
    return iterator;
  }

  atomic(): Deno.AtomicOperation {
    const checks: Array<{ key: Deno.KvKey; versionstamp: string | null }> = [];
    const mutations: Array<
      | { type: "set"; key: Deno.KvKey; value: unknown }
      | { type: "delete"; key: Deno.KvKey }
    > = [];
    const operation = {
      check: (entry: { key: Deno.KvKey; versionstamp: string | null }) => {
        checks.push({ key: clone(entry.key), versionstamp: entry.versionstamp });
        return operation;
      },
      set: (key: Deno.KvKey, value: unknown) => {
        mutations.push({ type: "set", key: clone(key), value: clone(value) });
        return operation;
      },
      delete: (key: Deno.KvKey) => {
        mutations.push({ type: "delete", key: clone(key) });
        return operation;
      },
      commit: (): Promise<Deno.KvCommitResult | Deno.KvCommitError> => {
        const changed = checks.some((check) =>
          (this.entries.get(encodeKey(check.key))?.versionstamp ?? null) !== check.versionstamp
        );
        if (changed) return Promise.resolve({ ok: false });
        const versionstamp = this.#nextVersionstamp();
        for (const mutation of mutations) {
          if (mutation.type === "delete") {
            this.entries.delete(encodeKey(mutation.key));
          } else {
            this.entries.set(encodeKey(mutation.key), {
              key: clone(mutation.key),
              value: clone(mutation.value),
              versionstamp,
            });
          }
        }
        return Promise.resolve({ ok: true, versionstamp });
      },
    };
    return operation as unknown as Deno.AtomicOperation;
  }

  close(): void {}
}

const memoryKv = new MemoryKv();
const kv = memoryKv as unknown as Deno.Kv;
const denoWithKv = Deno as unknown as { openKv?: () => Promise<Deno.Kv> };
const originalOpenKv = denoWithKv.openKv;
denoWithKv.openKv = () => Promise.resolve(kv);

const { apiKeyHashKey, apiKeyIdKey } = await import("../src/api_keys.ts");
const {
  recordYunwuAmbiguousFailure,
  recordYunwuTerminal,
  recordYunwuUndispatchedCancellation,
  recordYunwuUpstreamResponse,
  reservePaidFallback,
} = await import("../src/paid_fallback.ts");
const {
  paidFallbackPendingV3Key,
  paidFallbackRequestV3Key,
  paidFallbackWindowV3Key,
} = await import("../src/paid_fallback_ledger.ts");
const { getKv } = await import("../src/kv.ts");
await getKv();
type ApiKeyRecord = import("../src/types.ts").ApiKeyRecord;
type PaidFallbackRequestV3 = import("../src/types.ts").PaidFallbackRequestV3;
type PaidFallbackWindowV3 = import("../src/types.ts").PaidFallbackWindowV3;

denoWithKv.openKv = originalOpenKv;

const keyId = "v3-cutover-key";
const keyHash = "v3-cutover-hash";
const legacyRequestLogPrefix: Deno.KvKey = ["ubq_ai", "api_keys", "request_log", keyId];

const countPrefix = async (prefix: Deno.KvKey): Promise<number> => {
  let count = 0;
  for await (const _entry of memoryKv.list({ prefix })) count += 1;
  return count;
};

const reservationInput = (requestId: string, createdAtMs: number) => ({
  keyId,
  requestId,
  createdAtMs,
  model: "gpt-5-codex",
  route: "responses",
  path: "/v1/responses",
  stream: true,
  reasoning: "high",
  reason: "primary_429",
} as const);

const reserve = async (requestId: string, createdAtMs: number) => {
  const decision = await reservePaidFallback(reservationInput(requestId, createdAtMs));
  assert.equal(decision.kind, "reserved");
  if (decision.kind !== "reserved") throw new Error("expected paid fallback reservation");
  return decision.reservation;
};

const readRequest = async (requestId: string): Promise<PaidFallbackRequestV3> => {
  const entry = await memoryKv.get<PaidFallbackRequestV3>(paidFallbackRequestV3Key(keyId, requestId));
  assert.ok(entry.value);
  return entry.value;
};

const assertLegacyStateUnchanged = async (expected: ApiKeyRecord): Promise<void> => {
  const idEntry = await memoryKv.get<ApiKeyRecord>(apiKeyIdKey(keyId));
  const hashEntry = await memoryKv.get<Record<string, unknown>>(apiKeyHashKey(keyHash));
  assert.deepEqual(idEntry.value, expected);
  assert.equal(hashEntry.value?.paid_fallback_spent_microcredits, expected.paid_fallback_spent_microcredits);
  assert.equal(hashEntry.value?.paid_fallback_reserved_microcredits, expected.paid_fallback_reserved_microcredits);
  assert.equal(
    hashEntry.value?.paid_fallback_reservation_request_id,
    expected.paid_fallback_reservation_request_id,
  );
  assert.equal(await countPrefix(legacyRequestLogPrefix), 0);
};

Deno.test("Yunwu runtime lifecycle hard-cuts legacy counters and request logs in favor of V3", async () => {
  const now = Date.now();
  const record: ApiKeyRecord = {
    id: keyId,
    name: "V3 cutover",
    prefix: "u_v3",
    hash: keyHash,
    created_at_ms: now - 60_000,
    expires_at_ms: -1,
    revoked_at_ms: null,
    usage_limit_requests: 100,
    usage_requests: 0,
    usage_reset_at_ms: now + 60_000,
    window_ms: 60_000,
    usage_quota_version: 3,
    paid_fallback_enabled: true,
    paid_fallback_limit_microcredits: 5_000_000,
    paid_fallback_spent_microcredits: 4_900_000,
    paid_fallback_reserved_microcredits: 200_000,
    paid_fallback_reservation_request_id: "legacy-stale-reservation",
    paid_fallback_model_ids: ["gpt-5-codex"],
    paid_fallback_quota_per_credit: 500_000,
    paid_fallback_max_exposure_microcredits: { "gpt-5-codex": 250_000 },
    paid_fallback_pricing_checked_at_ms: now - 30_000,
  };
  const hashRecord = {
    id: record.id,
    expires_at_ms: record.expires_at_ms,
    revoked_at_ms: record.revoked_at_ms,
    usage_limit_requests: record.usage_limit_requests,
    usage_requests: record.usage_requests,
    usage_reset_at_ms: record.usage_reset_at_ms,
    window_ms: record.window_ms,
    usage_quota_version: record.usage_quota_version,
    paid_fallback_enabled: record.paid_fallback_enabled,
    paid_fallback_limit_microcredits: record.paid_fallback_limit_microcredits,
    paid_fallback_spent_microcredits: record.paid_fallback_spent_microcredits,
    paid_fallback_reserved_microcredits: record.paid_fallback_reserved_microcredits,
    paid_fallback_reservation_request_id: record.paid_fallback_reservation_request_id,
  };
  await memoryKv.set(apiKeyIdKey(keyId), record);
  await memoryKv.set(apiKeyHashKey(keyHash), hashRecord);

  const originalEnvGet = Deno.env.get;
  Deno.env.get = (name: string): string | undefined =>
    name === "YUNWU_API_KEY" ? "test-yunwu-key" : originalEnvGet.call(Deno.env, name);
  try {
    const terminalReservation = await reserve("terminal-missing-id", now);
    assert.equal(terminalReservation.reserved_microcredits, 250_000);
    assert.equal(terminalReservation.quota_used_percent, 0);
    await recordYunwuUpstreamResponse(
      terminalReservation,
      new Response(null, { status: 502 }),
      null,
    );
    await recordYunwuTerminal(terminalReservation, "incomplete");
    const terminal = await readRequest("terminal-missing-id");
    assert.equal(terminal.dispatch_state, "dispatched");
    assert.equal(terminal.terminal_state, "incomplete");
    assert.equal(terminal.provider_request_id, null);
    assert.equal(terminal.billing_state, "pending");
    assert.equal(terminal.reserved_microcredits, 250_000);
    assert.equal(terminal.reconciliation_attempts, 1);
    assert.ok((await memoryKv.get(paidFallbackPendingV3Key(keyId, terminal.request_id))).value);

    const ambiguousReservation = await reserve("ambiguous", now + 1);
    await recordYunwuAmbiguousFailure(ambiguousReservation);
    const ambiguous = await readRequest("ambiguous");
    assert.equal(ambiguous.dispatch_state, "dispatched");
    assert.equal(ambiguous.terminal_state, "ambiguous");
    assert.equal(ambiguous.billing_state, "pending");
    assert.equal(ambiguous.reserved_microcredits, 250_000);
    assert.ok((await memoryKv.get(paidFallbackPendingV3Key(keyId, ambiguous.request_id))).value);

    const dispatchedReservation = await reserve("dispatched-http-error", now + 2);
    await recordYunwuUpstreamResponse(
      dispatchedReservation,
      new Response(null, { status: 503 }),
      "provider-request-id",
    );
    const dispatched = await readRequest("dispatched-http-error");
    assert.equal(dispatched.dispatch_state, "dispatched");
    assert.equal(dispatched.terminal_state, "pending");
    assert.equal(dispatched.provider_request_id, "provider-request-id");
    assert.equal(dispatched.billing_state, "pending");
    assert.equal(dispatched.reserved_microcredits, 250_000);

    const cancelledReservation = await reserve("cancelled-before-dispatch", now + 3);
    await recordYunwuUndispatchedCancellation(cancelledReservation);
    const cancelled = await readRequest("cancelled-before-dispatch");
    assert.equal(cancelled.dispatch_state, "not_dispatched");
    assert.equal(cancelled.terminal_state, "cancelled");
    assert.equal(cancelled.billing_state, "not_billed");
    assert.equal(cancelled.spend_microcredits, 0);
    assert.equal((await memoryKv.get(paidFallbackPendingV3Key(keyId, cancelled.request_id))).value, null);

    const window = await memoryKv.get<PaidFallbackWindowV3>(
      paidFallbackWindowV3Key(keyId, terminalReservation.window_reset_at_ms),
    );
    assert.equal(window.value?.settled_microcredits, 0);
    assert.equal(window.value?.reserved_microcredits, 750_000);
    assert.equal(window.value?.pending_count, 3);
    await assertLegacyStateUnchanged(record);
  } finally {
    Deno.env.get = originalEnvGet;
  }
});
