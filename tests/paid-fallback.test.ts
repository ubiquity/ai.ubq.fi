import assert from "node:assert/strict";

type StoredEntry = {
  key: Deno.KvKey;
  value: unknown;
  versionstamp: string;
  expiresAtMs: number | null;
};

const encodeKey = (key: Deno.KvKey): string => JSON.stringify(key);
const clone = <T>(value: T): T => structuredClone(value);

const compareKeyPart = (left: Deno.KvKeyPart, right: Deno.KvKeyPart): number => {
  if (left === right) return 0;
  if (typeof left === "number" && typeof right === "number") return left - right;
  const leftText = String(left);
  const rightText = String(right);
  return leftText < rightText ? -1 : 1;
};

const compareKeys = (left: Deno.KvKey, right: Deno.KvKey): number => {
  const length = Math.max(left.length, right.length);
  for (let index = 0; index < length; index += 1) {
    if (index >= left.length) return -1;
    if (index >= right.length) return 1;
    const comparison = compareKeyPart(left[index], right[index]);
    if (comparison !== 0) return comparison;
  }
  return 0;
};

const startsWithKey = (key: Deno.KvKey, prefix: Deno.KvKey): boolean =>
  prefix.every((part, index) => key[index] === part);

class MemoryKv {
  readonly entries = new Map<string, StoredEntry>();
  readonly queueMessages: Array<{ message: unknown; options: unknown }> = [];
  enqueueFailure: Error | null = null;
  beforeAtomicCommit: (() => void) | null = null;
  atomicCommitFailures = 0;
  #version = 0;

  clear(): void {
    this.entries.clear();
    this.queueMessages.length = 0;
    this.enqueueFailure = null;
    this.beforeAtomicCommit = null;
    this.atomicCommitFailures = 0;
    this.#version = 0;
  }

  #nextVersionstamp(): string {
    this.#version += 1;
    return String(this.#version).padStart(20, "0");
  }

  #purgeExpired(): void {
    const now = Date.now();
    for (const [encodedKey, entry] of this.entries) {
      if (entry.expiresAtMs !== null && entry.expiresAtMs <= now) {
        this.entries.delete(encodedKey);
      }
    }
  }

  #write(
    key: Deno.KvKey,
    value: unknown,
    options: { expireIn?: number } | undefined,
    versionstamp: string,
  ): void {
    const expireIn = options?.expireIn;
    const expiresAtMs = typeof expireIn === "number" ? Date.now() + Math.max(0, expireIn) : null;
    this.entries.set(encodeKey(key), {
      key: clone(key),
      value: clone(value),
      versionstamp,
      expiresAtMs,
    });
  }

  versionstamp(key: Deno.KvKey): string | null {
    this.#purgeExpired();
    return this.entries.get(encodeKey(key))?.versionstamp ?? null;
  }

  expiration(key: Deno.KvKey): number | null {
    this.#purgeExpired();
    return this.entries.get(encodeKey(key))?.expiresAtMs ?? null;
  }

  get<T = unknown>(key: Deno.KvKey): Promise<Deno.KvEntryMaybe<T>> {
    this.#purgeExpired();
    const entry = this.entries.get(encodeKey(key));
    return Promise.resolve({
      key: clone(key),
      value: entry ? clone(entry.value) as T : null,
      versionstamp: entry?.versionstamp ?? null,
    } as Deno.KvEntryMaybe<T>);
  }

  set(
    key: Deno.KvKey,
    value: unknown,
    options?: { expireIn?: number },
  ): Promise<Deno.KvCommitResult> {
    const versionstamp = this.#nextVersionstamp();
    this.#write(key, value, options, versionstamp);
    return Promise.resolve({ ok: true, versionstamp });
  }

  delete(key: Deno.KvKey): Promise<void> {
    this.#purgeExpired();
    this.entries.delete(encodeKey(key));
    this.#nextVersionstamp();
    return Promise.resolve();
  }

  enqueue(message: unknown, options?: unknown): Promise<void> {
    if (this.enqueueFailure) return Promise.reject(this.enqueueFailure);
    this.queueMessages.push({ message: clone(message), options: clone(options) });
    return Promise.resolve();
  }

  list<T = unknown>(
    selector: Deno.KvListSelector,
    options: Deno.KvListOptions = {},
  ): Deno.KvListIterator<T> {
    this.#purgeExpired();
    let entries = [...this.entries.values()]
      .filter((entry) => {
        const prefix = "prefix" in selector ? selector.prefix : [];
        if (!startsWithKey(entry.key, prefix)) return false;
        if ("start" in selector && selector.start && compareKeys(entry.key, selector.start) < 0) return false;
        if ("end" in selector && selector.end && compareKeys(entry.key, selector.end) >= 0) return false;
        return true;
      })
      .sort((left, right) => compareKeys(left.key, right.key));
    if (options.reverse) entries = entries.reverse();
    if (typeof options.limit === "number") entries = entries.slice(0, options.limit);

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
      | { type: "set"; key: Deno.KvKey; value: unknown; options?: { expireIn?: number } }
      | { type: "delete"; key: Deno.KvKey }
    > = [];
    const operation = {
      check: (entry: { key: Deno.KvKey; versionstamp: string | null }) => {
        checks.push({ key: clone(entry.key), versionstamp: entry.versionstamp });
        return operation;
      },
      set: (
        key: Deno.KvKey,
        value: unknown,
        options?: { expireIn?: number },
      ) => {
        mutations.push({ type: "set", key: clone(key), value: clone(value), options });
        return operation;
      },
      delete: (key: Deno.KvKey) => {
        mutations.push({ type: "delete", key: clone(key) });
        return operation;
      },
      commit: (): Promise<Deno.KvCommitResult | Deno.KvCommitError> => {
        this.beforeAtomicCommit?.();
        this.beforeAtomicCommit = null;
        this.#purgeExpired();
        if (checks.some((check) => this.versionstamp(check.key) !== check.versionstamp)) {
          this.atomicCommitFailures += 1;
          return Promise.resolve({ ok: false });
        }

        const versionstamp = this.#nextVersionstamp();
        for (const mutation of mutations) {
          if (mutation.type === "delete") {
            this.entries.delete(encodeKey(mutation.key));
          } else {
            this.#write(mutation.key, mutation.value, mutation.options, versionstamp);
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
const originalOpenKv = (Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv;
(Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = () => Promise.resolve(kv);

const {
  apiKeyHashKey,
  apiKeyIdKey,
} = await import("../src/api_keys.ts");
const {
  apiKeyRequestLogKey,
  recordApiKeyRequestLog,
  updateApiKeyRequestLog,
} = await import("../src/analytics.ts");
const {
  hasStrictPaidFallbackKeyPolicy,
  hasStrictPaidFallbackPolicy,
  reservePaidFallback,
} = await import("../src/paid_fallback.ts");
const {
  admitPaidFallbackV3,
  deletePaidFallbackStateV3,
  getPaidFallbackOutstandingV3,
  getPaidFallbackWindowProjectionV3,
  listPaidFallbackRequestsV3,
  paidFallbackDeletionGuardV3Key,
  paidFallbackRequestV3Key,
  paidFallbackPendingV3Key,
  paidFallbackReconciliationLeaseV3Key,
  paidFallbackWindowV3Key,
  reconcileDuePaidFallbacksV3,
  reconcilePaidFallbackV3,
  recordPaidFallbackTerminalV3,
  releasePaidFallbackBeforeProviderFetchV3,
  releaseUndispatchedPaidFallbackV3,
  enqueueDuePaidFallbackReconciliationJobsV3,
  handlePaidFallbackReconciliationJobV3,
  updatePaidFallbackRequestV3,
} = await import("../src/paid_fallback_ledger.ts");
const { getKv } = await import("../src/kv.ts");
await getKv();

const strictKeyRecord = (
  overrides: Record<string, unknown> = {},
): Record<string, unknown> => {
  const now = Date.now();
  return {
    id: "key-paid",
    name: "Paid fallback test",
    prefix: "u_test",
    hash: "hash-paid",
    created_at_ms: now - 60_000,
    expires_at_ms: -1,
    revoked_at_ms: null,
    usage_limit_requests: 100,
    usage_requests: 3,
    usage_reset_at_ms: now + 60 * 60 * 1000,
    window_ms: 7 * 24 * 60 * 60 * 1000,
    paid_fallback_enabled: true,
    paid_fallback_limit_microcredits: 5_000_000,
    paid_fallback_spent_microcredits: 1_000_000,
    paid_fallback_reserved_microcredits: 0,
    paid_fallback_reservation_request_id: null,
    paid_fallback_model_ids: ["gpt-5-codex"],
    paid_fallback_quota_per_credit: 500_000,
    paid_fallback_max_exposure_microcredits: { "gpt-5-codex": 250_000 },
    paid_fallback_pricing_checked_at_ms: now - 30_000,
    ...overrides,
  };
};

const strictHashRecord = (
  record: Record<string, unknown>,
): Record<string, unknown> => ({
  id: record.id,
  expires_at_ms: record.expires_at_ms,
  revoked_at_ms: record.revoked_at_ms,
  usage_limit_requests: record.usage_limit_requests,
  usage_requests: record.usage_requests,
  usage_reset_at_ms: record.usage_reset_at_ms,
  window_ms: record.window_ms,
  paid_fallback_enabled: record.paid_fallback_enabled,
  paid_fallback_limit_microcredits: record.paid_fallback_limit_microcredits,
  paid_fallback_spent_microcredits: record.paid_fallback_spent_microcredits,
  paid_fallback_reserved_microcredits: record.paid_fallback_reserved_microcredits,
  paid_fallback_reservation_request_id: record.paid_fallback_reservation_request_id,
});

const seedStrictKey = async (
  overrides: Record<string, unknown> = {},
): Promise<Record<string, unknown>> => {
  const record = strictKeyRecord(overrides);
  await memoryKv.set(apiKeyIdKey(String(record.id)), record);
  await memoryKv.set(apiKeyHashKey(String(record.hash)), strictHashRecord(record));
  return record;
};

const withYunwuApiKey = async (fn: () => Promise<void>): Promise<void> => {
  const originalGet = Deno.env.get;
  Deno.env.get = (key: string): string | undefined =>
    key === "YUNWU_API_KEY" ? "yunwu-test-key" : originalGet.call(Deno.env, key);
  try {
    await fn();
  } finally {
    Deno.env.get = originalGet;
  }
};

const reservationInput = (
  keyId: string,
  requestId: string,
  createdAtMs = Date.now(),
) => ({
  keyId,
  requestId,
  createdAtMs,
  model: "gpt-5-codex",
  route: "responses",
  path: "/v1/responses",
  stream: false,
  reasoning: "high",
  reason: "primary_429" as const,
});

const v3AdmissionInput = (
  keyId: string,
  requestId: string,
  overrides: Partial<Parameters<typeof admitPaidFallbackV3>[0]> = {},
): Parameters<typeof admitPaidFallbackV3>[0] => ({
  keyId,
  requestId,
  createdAtMs: Date.now(),
  policyVersion: "policy-v3",
  limitMicrocredits: 1_000_000,
  maximumExposureMicrocredits: 250_000,
  initialSettledMicrocredits: 0,
  quotaPerCredit: 500_000,
  windowResetAtMs: Date.now() + 60_000,
  model: "gpt-5-codex",
  route: "responses",
  path: "/v1/responses",
  stream: true,
  reasoning: "high",
  ...overrides,
});

Deno.test("strict paid fallback policy accepts valid disabled history and unlimited reservation state", () => {
  const disabledHistory = strictKeyRecord({
    paid_fallback_enabled: false,
    paid_fallback_limit_microcredits: 5_000_000,
    paid_fallback_spent_microcredits: 1_250_000,
    paid_fallback_reserved_microcredits: 0,
    paid_fallback_reservation_request_id: null,
  });
  assert.equal(hasStrictPaidFallbackPolicy(disabledHistory), true);
  assert.equal(hasStrictPaidFallbackKeyPolicy(disabledHistory), true);

  const neverEnabled = strictKeyRecord({
    paid_fallback_enabled: false,
    paid_fallback_limit_microcredits: 0,
    paid_fallback_spent_microcredits: 0,
    paid_fallback_reserved_microcredits: 0,
    paid_fallback_reservation_request_id: null,
    paid_fallback_model_ids: [],
    paid_fallback_quota_per_credit: 0,
    paid_fallback_pricing_checked_at_ms: null,
  });
  assert.equal(hasStrictPaidFallbackKeyPolicy(neverEnabled), true);

  const unlimitedReservation = strictKeyRecord({
    paid_fallback_limit_microcredits: -1,
    paid_fallback_spent_microcredits: 50_000_000,
    paid_fallback_reserved_microcredits: 0,
    paid_fallback_reservation_request_id: "request-unlimited",
  });
  assert.equal(hasStrictPaidFallbackPolicy(unlimitedReservation), true);
  assert.equal(hasStrictPaidFallbackKeyPolicy(unlimitedReservation), true);
});

Deno.test("strict paid fallback policy rejects invalid values but ignores legacy counter relationships", () => {
  const invalidCommonPolicies = [
    { paid_fallback_spent_microcredits: -1 },
    { paid_fallback_spent_microcredits: Number.MAX_SAFE_INTEGER + 1 },
    { paid_fallback_reserved_microcredits: -1 },
    { paid_fallback_reserved_microcredits: Number.MAX_SAFE_INTEGER + 1 },
    { paid_fallback_limit_microcredits: 0 },
    { paid_fallback_limit_microcredits: -2 },
  ];
  for (const overrides of invalidCommonPolicies) {
    assert.equal(
      hasStrictPaidFallbackPolicy(strictKeyRecord(overrides)),
      false,
      JSON.stringify(overrides),
    );
  }

  const ignoredLegacyCounterRelationships = [
    {
      paid_fallback_reserved_microcredits: 1,
      paid_fallback_reservation_request_id: null,
    },
    {
      paid_fallback_limit_microcredits: 1_000_000,
      paid_fallback_spent_microcredits: 900_000,
      paid_fallback_reserved_microcredits: 100_001,
      paid_fallback_reservation_request_id: "request-over-cap",
    },
    {
      paid_fallback_reserved_microcredits: 0,
      paid_fallback_reservation_request_id: "request-empty-bounded-reservation",
    },
    {
      paid_fallback_enabled: false,
      paid_fallback_reserved_microcredits: 0,
      paid_fallback_reservation_request_id: "request-disabled",
    },
    {
      paid_fallback_limit_microcredits: -1,
      paid_fallback_reserved_microcredits: 1,
      paid_fallback_reservation_request_id: "request-unlimited-with-amount",
    },
  ];
  for (const overrides of ignoredLegacyCounterRelationships) {
    assert.equal(
      hasStrictPaidFallbackPolicy(strictKeyRecord(overrides)),
      true,
      JSON.stringify(overrides),
    );
  }

  const invalidEnabledKeyPolicies = [
    { paid_fallback_model_ids: [] },
    { paid_fallback_quota_per_credit: 0 },
    { paid_fallback_quota_per_credit: Number.MAX_SAFE_INTEGER + 1 },
    { paid_fallback_pricing_checked_at_ms: null },
    { paid_fallback_pricing_checked_at_ms: 0 },
    { paid_fallback_pricing_checked_at_ms: Number.MAX_SAFE_INTEGER + 1 },
  ];
  for (const overrides of invalidEnabledKeyPolicies) {
    assert.equal(
      hasStrictPaidFallbackKeyPolicy(strictKeyRecord(overrides)),
      false,
      JSON.stringify(overrides),
    );
  }
});

Deno.test("concurrent paid fallback ledger patches retry without losing either update", async () => {
  memoryKv.clear();
  const keyId = "key-concurrent-ledger";
  const requestId = "request-concurrent-ledger";
  const createdAtMs = Date.now();

  await recordApiKeyRequestLog(keyId, {
    id: requestId,
    route: "responses",
    path: "/v1/responses",
    method: "POST",
    status_code: 0,
    stream: false,
    created_at_ms: createdAtMs,
    provider: "yunwu",
    billing_status: "pending",
  }, kv);

  await Promise.all([
    updateApiKeyRequestLog(
      keyId,
      createdAtMs,
      requestId,
      { provider_request_id: "provider-concurrent" },
      kv,
    ),
    updateApiKeyRequestLog(
      keyId,
      createdAtMs,
      requestId,
      { status_code: 200, completed_at_ms: createdAtMs + 50 },
      kv,
    ),
  ]);

  assert.equal(memoryKv.atomicCommitFailures, 1);
  const stored = await memoryKv.get<Record<string, unknown>>(
    apiKeyRequestLogKey(keyId, createdAtMs, requestId),
  );
  assert.equal(stored.value?.provider_request_id, "provider-concurrent");
  assert.equal(stored.value?.status_code, 200);
  assert.equal(stored.value?.completed_at_ms, createdAtMs + 50);
});

Deno.test("V3 admits concurrent bounded requests without a single reservation slot", async () => {
  memoryKv.clear();
  const keyId = "v3-concurrent";
  const resetAtMs = Date.now() + 60_000;
  const requests = await Promise.all(
    Array.from({ length: 100 }, (_, index) =>
      admitPaidFallbackV3({
        keyId,
        requestId: `request-${index}`,
        createdAtMs: Date.now(),
        policyVersion: "policy-v3",
        limitMicrocredits: 1_000_000,
        maximumExposureMicrocredits: 10_000,
        initialSettledMicrocredits: 0,
        quotaPerCredit: 500_000,
        windowResetAtMs: resetAtMs,
        model: "gpt-5-codex",
        route: "responses",
        path: "/v1/responses",
        stream: true,
        reasoning: "high",
      })),
  );
  assert.deepEqual(requests.map((decision) => decision.kind), Array(100).fill("reserved"));
  const window = await memoryKv.get<Record<string, unknown>>(paidFallbackWindowV3Key(keyId, resetAtMs));
  assert.equal(window.value?.reserved_microcredits, 1_000_000);
  assert.equal(window.value?.pending_count, 100);
  const rows = await Promise.all(
    Array.from(
      { length: 100 },
      (_, index) => memoryKv.get(paidFallbackRequestV3Key(keyId, `request-${index}`)),
    ),
  );
  assert.equal(rows.filter((entry) => entry.value !== null).length, 100);
  const pending = await memoryKv.get<Record<string, unknown>>(paidFallbackPendingV3Key(keyId, "request-0"));
  assert.equal(typeof pending.value?.next_reconciliation_at_ms, "number");
});

Deno.test("V3 unlimited admission writes independent rows without a shared window", async () => {
  memoryKv.clear();
  const keyId = "v3-unlimited";
  const resetAtMs = Date.now() + 60_000;
  const decisions = await Promise.all(
    Array.from({ length: 100 }, (_, index) =>
      admitPaidFallbackV3({
        keyId,
        requestId: `unlimited-${index}`,
        createdAtMs: Date.now(),
        policyVersion: "policy-v3",
        limitMicrocredits: -1,
        maximumExposureMicrocredits: null,
        initialSettledMicrocredits: 0,
        quotaPerCredit: 500_000,
        windowResetAtMs: resetAtMs,
        model: "gpt-5-codex",
        route: "responses",
        path: "/v1/responses",
        stream: true,
        reasoning: "high",
      })),
  );
  assert.deepEqual(decisions.map((decision) => decision.kind), Array(100).fill("reserved"));
  assert.equal((await memoryKv.get(paidFallbackWindowV3Key(keyId, resetAtMs))).value, null);
});

Deno.test("V3 terminal reconciliation settles a pending request exactly once without KV queues", async () => {
  memoryKv.clear();
  const originalFetch = globalThis.fetch;
  const keyId = "v3-terminal";
  const requestId = "gateway-terminal";
  const providerRequestId = "provider-terminal";
  const resetAtMs = Date.now() + 60_000;
  const createdAtMs = Date.now();
  globalThis.fetch = () =>
    Promise.resolve(
      Response.json({
        success: true,
        data: [{
          request_id: providerRequestId,
          quota: 123_456,
          prompt_tokens: 40,
          completion_tokens: 60,
          model_name: "gpt-5-codex",
          created_at: Math.trunc(Date.now() / 1_000),
        }],
      }),
    );
  try {
    await withYunwuApiKey(async () => {
      const decision = await admitPaidFallbackV3({
        keyId,
        requestId,
        createdAtMs,
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
      });
      assert.equal(decision.kind, "reserved");
      if (decision.kind !== "reserved") throw new Error("expected reservation");
      await updatePaidFallbackRequestV3(decision.reservation, {
        provider_request_id: providerRequestId,
        dispatch_state: "dispatched",
      });

      assert.equal(await recordPaidFallbackTerminalV3(decision.reservation, "completed"), 1);
      assert.equal(await recordPaidFallbackTerminalV3(decision.reservation, "completed"), 0);

      const request = await memoryKv.get<Record<string, unknown>>(paidFallbackRequestV3Key(keyId, requestId));
      assert.equal(request.value?.billing_state, "settled");
      assert.equal(request.value?.terminal_state, "completed");
      assert.equal(request.value?.spend_microcredits, 246_912);
      assert.equal(request.value?.provider_quota, 123_456);
      assert.equal(request.value?.input_tokens, 40);
      assert.equal(request.value?.output_tokens, 60);
      assert.equal(typeof request.value?.dispatched_at_ms, "number");
      assert.equal(typeof request.value?.terminal_at_ms, "number");
      assert.equal(typeof request.value?.settled_at_ms, "number");
      const window = await memoryKv.get<Record<string, unknown>>(paidFallbackWindowV3Key(keyId, resetAtMs));
      assert.equal(window.value?.reserved_microcredits, 0);
      assert.equal(window.value?.settled_microcredits, 246_912);
      assert.equal(window.value?.pending_count, 0);
      assert.equal((await memoryKv.get(paidFallbackPendingV3Key(keyId, requestId))).value, null);
    });
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("V3 reconciliation records billing facts without manufacturing a terminal event", async () => {
  memoryKv.clear();
  const originalFetch = globalThis.fetch;
  const keyId = "v3-out-of-order";
  const requestId = "gateway-out-of-order";
  const providerRequestId = "provider-out-of-order";
  const requestKey = paidFallbackRequestV3Key(keyId, requestId);
  let injectedContention = false;
  globalThis.fetch = () => {
    if (!injectedContention) {
      injectedContention = true;
      memoryKv.beforeAtomicCommit = () => {
        const existing = memoryKv.entries.get(encodeKey(requestKey));
        if (!existing) throw new Error("missing request for contention injection");
        void memoryKv.set(requestKey, {
          ...(existing.value as Record<string, unknown>),
          updated_at_ms: Number((existing.value as Record<string, unknown>).updated_at_ms) + 1,
        });
      };
    }
    return Promise.resolve(
      Response.json({
        success: true,
        data: [{
          request_id: providerRequestId,
          quota: 50_000,
          prompt_tokens: 12,
          completion_tokens: 34,
          model_name: "gpt-5-codex",
          created_at: Math.trunc(Date.now() / 1_000),
        }],
      }),
    );
  };
  try {
    await withYunwuApiKey(async () => {
      const decision = await admitPaidFallbackV3(v3AdmissionInput(keyId, requestId));
      assert.equal(decision.kind, "reserved");
      if (decision.kind !== "reserved") throw new Error("expected reservation");
      await updatePaidFallbackRequestV3(decision.reservation, {
        provider_request_id: providerRequestId,
        dispatch_state: "dispatched",
      });

      assert.equal(await reconcilePaidFallbackV3(keyId, Date.now() + 100, kv), 1);
      const settledBeforeTerminal = await memoryKv.get<Record<string, unknown>>(requestKey);
      assert.equal(settledBeforeTerminal.value?.billing_state, "settled");
      assert.equal(settledBeforeTerminal.value?.terminal_state, "pending");
      assert.equal(settledBeforeTerminal.value?.provider_quota, 50_000);
      assert.equal(settledBeforeTerminal.value?.input_tokens, 12);
      assert.equal(settledBeforeTerminal.value?.output_tokens, 34);
      assert.equal(settledBeforeTerminal.value?.spend_microcredits, 100_000);
      assert.equal(memoryKv.atomicCommitFailures, 1);

      assert.equal(await recordPaidFallbackTerminalV3(decision.reservation, "ambiguous"), 0);
      const afterTerminal = await memoryKv.get<Record<string, unknown>>(requestKey);
      assert.equal(afterTerminal.value?.terminal_state, "ambiguous");
      assert.equal(typeof afterTerminal.value?.terminal_at_ms, "number");
      assert.equal(afterTerminal.value?.spend_microcredits, 100_000);
      assert.equal(await reconcilePaidFallbackV3(keyId, Date.now() + 200, kv), 0);
    });
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("V3 terminal delivery expedites a request deferred before provider billing appeared", async () => {
  memoryKv.clear();
  const originalFetch = globalThis.fetch;
  const keyId = "v3-terminal-expedite";
  const requestId = "gateway-terminal-expedite";
  const providerRequestId = "provider-terminal-expedite";
  let billingVisible = false;
  globalThis.fetch = () =>
    Promise.resolve(
      Response.json({
        success: true,
        data: billingVisible
          ? [{
            request_id: providerRequestId,
            quota: 25_000,
            prompt_tokens: 5,
            completion_tokens: 6,
            model_name: "gpt-5-codex",
            created_at: Math.trunc(Date.now() / 1_000),
          }]
          : [],
      }),
    );
  try {
    await withYunwuApiKey(async () => {
      const decision = await admitPaidFallbackV3(v3AdmissionInput(keyId, requestId));
      assert.equal(decision.kind, "reserved");
      if (decision.kind !== "reserved") throw new Error("expected reservation");
      await updatePaidFallbackRequestV3(decision.reservation, {
        provider_request_id: providerRequestId,
        dispatch_state: "dispatched",
      });
      const deferredAtMs = Date.now() + 100;
      assert.equal(await reconcilePaidFallbackV3(keyId, deferredAtMs, kv), 0);
      const deferred = await memoryKv.get<Record<string, unknown>>(paidFallbackPendingV3Key(keyId, requestId));
      assert.equal(deferred.value?.next_reconciliation_at_ms, deferredAtMs + 5_000);

      billingVisible = true;
      assert.equal(await recordPaidFallbackTerminalV3(decision.reservation, "completed"), 1);
      const settled = await memoryKv.get<Record<string, unknown>>(paidFallbackRequestV3Key(keyId, requestId));
      assert.equal(settled.value?.billing_state, "settled");
      assert.equal(settled.value?.terminal_state, "completed");
      assert.equal(settled.value?.spend_microcredits, 50_000);
      assert.equal(settled.value?.reconciliation_attempts, 2);
    });
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("V3 bounded policy edits preserve exposure, admit concurrently, and retain final-call overshoot", async () => {
  memoryKv.clear();
  const originalFetch = globalThis.fetch;
  const keyId = "v3-policy-edit";
  const resetAtMs = Date.now() + 60_000;
  const first = await admitPaidFallbackV3(
    v3AdmissionInput(keyId, "policy-first", {
      policyVersion: "policy-v1",
      windowResetAtMs: resetAtMs,
      maximumExposureMicrocredits: 400_000,
    }),
  );
  assert.equal(first.kind, "reserved");
  if (first.kind !== "reserved") throw new Error("expected first reservation");
  assert.equal(first.reservation.quota_used_percent, 0);

  const concurrent = await Promise.all(
    ["policy-second", "policy-third"].map((requestId) =>
      admitPaidFallbackV3(
        v3AdmissionInput(keyId, requestId, {
          policyVersion: "policy-v2",
          limitMicrocredits: 600_000,
          windowResetAtMs: resetAtMs,
          maximumExposureMicrocredits: 200_000,
        }),
      )
    ),
  );
  assert.deepEqual(
    concurrent.map((decision) => decision.kind).sort(),
    ["blocked", "reserved"],
  );
  const second = concurrent.find((decision) => decision.kind === "reserved");
  if (!second || second.kind !== "reserved") throw new Error("expected concurrent reservation");

  const lowered = await admitPaidFallbackV3(
    v3AdmissionInput(keyId, "policy-lowered", {
      policyVersion: "policy-v3",
      limitMicrocredits: 500_000,
      windowResetAtMs: resetAtMs,
      maximumExposureMicrocredits: 200_000,
    }),
  );
  assert.deepEqual(lowered, { kind: "blocked", reason: "limit_exceeded" });
  const transitioned = await memoryKv.get<Record<string, unknown>>(paidFallbackWindowV3Key(keyId, resetAtMs));
  assert.equal(transitioned.value?.policy_version, "policy-v3");
  assert.equal(transitioned.value?.limit_microcredits, 500_000);
  assert.equal(transitioned.value?.reserved_microcredits, 600_000);

  const providerIds = new Map([
    [first.reservation.request_id, "provider-policy-first"],
    [second.reservation.request_id, "provider-policy-second"],
  ]);
  await Promise.all(
    [first.reservation, second.reservation].map((reservation) =>
      updatePaidFallbackRequestV3(reservation, {
        provider_request_id: providerIds.get(reservation.request_id)!,
        dispatch_state: "dispatched",
        terminal_state: "completed",
      })
    ),
  );
  globalThis.fetch = () =>
    Promise.resolve(
      Response.json({
        success: true,
        data: [
          {
            request_id: "provider-policy-first",
            quota: 200_000,
            prompt_tokens: 10,
            completion_tokens: 20,
            model_name: "gpt-5-codex",
            created_at: Math.trunc(Date.now() / 1_000),
          },
          {
            request_id: "provider-policy-second",
            quota: 150_000,
            prompt_tokens: 20,
            completion_tokens: 30,
            model_name: "gpt-5-codex",
            created_at: Math.trunc(Date.now() / 1_000),
          },
        ],
      }),
    );
  try {
    await withYunwuApiKey(async () => {
      assert.equal(await reconcilePaidFallbackV3(keyId, Date.now() + 100, kv), 2);
    });
  } finally {
    globalThis.fetch = originalFetch;
  }

  const overshot = await memoryKv.get<Record<string, unknown>>(paidFallbackWindowV3Key(keyId, resetAtMs));
  assert.equal(overshot.value?.settled_microcredits, 700_000);
  assert.equal(overshot.value?.reserved_microcredits, 0);
  assert.equal(overshot.value?.pending_count, 0);
  assert.deepEqual(
    await admitPaidFallbackV3(
      v3AdmissionInput(keyId, "policy-after-overshoot", {
        policyVersion: "policy-v3",
        limitMicrocredits: 500_000,
        windowResetAtMs: resetAtMs,
      }),
    ),
    { kind: "blocked", reason: "limit_exceeded" },
  );
  assert.deepEqual(
    await getPaidFallbackWindowProjectionV3(keyId, resetAtMs, 500_000, kv),
    {
      key_id: keyId,
      policy_version: "policy-v3",
      window_reset_at_ms: resetAtMs,
      limit_microcredits: 500_000,
      settled_microcredits: 700_000,
      reserved_microcredits: 0,
      pending_count: 0,
      updated_at_ms: overshot.value?.updated_at_ms,
    },
  );
});

Deno.test("V3 due scanning honors leases and backoff while retaining unresolved exposure after 24 hours", async () => {
  memoryKv.clear();
  const keyId = "v3-unresolved";
  const requestId = "missing-provider-id";
  const createdAtMs = Date.now();
  const resetAtMs = createdAtMs + 60_000;
  const decision = await admitPaidFallbackV3(
    v3AdmissionInput(keyId, requestId, {
      createdAtMs,
      windowResetAtMs: resetAtMs,
      maximumExposureMicrocredits: 300_000,
    }),
  );
  assert.equal(decision.kind, "reserved");
  if (decision.kind !== "reserved") throw new Error("expected reservation");
  const firstAttemptAtMs = Date.now() + 100;
  await memoryKv.set(
    paidFallbackReconciliationLeaseV3Key(keyId),
    { token: "held-lease", expires_at_ms: firstAttemptAtMs + 60_000 },
    { expireIn: 60_000 },
  );
  assert.equal(await reconcileDuePaidFallbacksV3(firstAttemptAtMs, kv), 0);
  let request = await memoryKv.get<Record<string, unknown>>(paidFallbackRequestV3Key(keyId, requestId));
  assert.equal(request.value?.reconciliation_attempts, 0);

  await memoryKv.delete(paidFallbackReconciliationLeaseV3Key(keyId));
  assert.equal(await reconcileDuePaidFallbacksV3(firstAttemptAtMs, kv), 0);
  request = await memoryKv.get<Record<string, unknown>>(paidFallbackRequestV3Key(keyId, requestId));
  assert.equal(request.value?.reconciliation_attempts, 1);
  let pending = await memoryKv.get<Record<string, unknown>>(paidFallbackPendingV3Key(keyId, requestId));
  assert.equal(pending.value?.next_reconciliation_at_ms, firstAttemptAtMs + 5_000);

  assert.equal(await reconcileDuePaidFallbacksV3(firstAttemptAtMs + 4_999, kv), 0);
  assert.equal(
    (await memoryKv.get<Record<string, unknown>>(paidFallbackRequestV3Key(keyId, requestId))).value
      ?.reconciliation_attempts,
    1,
  );
  assert.equal(await reconcileDuePaidFallbacksV3(firstAttemptAtMs + 5_000, kv), 0);
  pending = await memoryKv.get<Record<string, unknown>>(paidFallbackPendingV3Key(keyId, requestId));
  assert.equal(pending.value?.next_reconciliation_at_ms, firstAttemptAtMs + 35_000);

  const unresolvedAtMs = createdAtMs + 24 * 60 * 60_000 + 1;
  assert.equal(await reconcileDuePaidFallbacksV3(unresolvedAtMs, kv), 0);
  request = await memoryKv.get<Record<string, unknown>>(paidFallbackRequestV3Key(keyId, requestId));
  assert.equal(request.value?.billing_state, "unresolved");
  assert.equal(request.value?.terminal_state, "pending");
  assert.equal(request.value?.reconciliation_attempts, 3);
  assert.equal(
    (await memoryKv.get<Record<string, unknown>>(paidFallbackPendingV3Key(keyId, requestId))).value !== null,
    true,
  );
  const window = await memoryKv.get<Record<string, unknown>>(paidFallbackWindowV3Key(keyId, resetAtMs));
  assert.equal(window.value?.reserved_microcredits, 300_000);
  assert.equal(window.value?.pending_count, 1);
  assert.deepEqual(await getPaidFallbackOutstandingV3(keyId, kv), {
    pending_requests: 0,
    unresolved_requests: 1,
    pending_markers: 1,
    has_outstanding: true,
  });
  const deletion = await deletePaidFallbackStateV3(keyId, kv);
  assert.equal(deletion.kind, "blocked");
});

Deno.test("V3 unlimited projection and newest-first history support safe settled-state deletion", async () => {
  memoryKv.clear();
  const originalFetch = globalThis.fetch;
  const keyId = "v3-unlimited-projection";
  const resetAtMs = Date.now() + 60_000;
  const createdAtMs = Date.now();
  const older = await admitPaidFallbackV3(
    v3AdmissionInput(keyId, "unlimited-older", {
      createdAtMs,
      limitMicrocredits: -1,
      maximumExposureMicrocredits: null,
      windowResetAtMs: resetAtMs,
    }),
  );
  const newer = await admitPaidFallbackV3(
    v3AdmissionInput(keyId, "unlimited-newer", {
      createdAtMs: createdAtMs + 10,
      limitMicrocredits: -1,
      maximumExposureMicrocredits: null,
      windowResetAtMs: resetAtMs,
    }),
  );
  assert.equal(older.kind, "reserved");
  assert.equal(newer.kind, "reserved");
  if (older.kind !== "reserved" || newer.kind !== "reserved") throw new Error("expected reservations");
  await Promise.all([
    updatePaidFallbackRequestV3(older.reservation, {
      provider_request_id: "provider-unlimited-older",
      dispatch_state: "dispatched",
      terminal_state: "completed",
    }),
    updatePaidFallbackRequestV3(newer.reservation, {
      provider_request_id: "provider-unlimited-newer",
      dispatch_state: "dispatched",
      terminal_state: "completed",
    }),
  ]);
  globalThis.fetch = () =>
    Promise.resolve(
      Response.json({
        success: true,
        data: [
          {
            request_id: "provider-unlimited-older",
            quota: 10_000,
            prompt_tokens: 1,
            completion_tokens: 2,
            model_name: "gpt-5-codex",
            created_at: Math.trunc(Date.now() / 1_000),
          },
          {
            request_id: "provider-unlimited-newer",
            quota: 15_000,
            prompt_tokens: 3,
            completion_tokens: 4,
            model_name: "gpt-5-codex",
            created_at: Math.trunc(Date.now() / 1_000),
          },
        ],
      }),
    );
  try {
    await withYunwuApiKey(async () => {
      assert.equal(await reconcilePaidFallbackV3(keyId, Date.now() + 100, kv), 2);
    });
  } finally {
    globalThis.fetch = originalFetch;
  }

  assert.equal((await memoryKv.get(paidFallbackWindowV3Key(keyId, resetAtMs))).value, null);
  assert.deepEqual(await getPaidFallbackWindowProjectionV3(keyId, resetAtMs, -1, kv), {
    key_id: keyId,
    policy_version: "policy-v3",
    window_reset_at_ms: resetAtMs,
    limit_microcredits: -1,
    settled_microcredits: 50_000,
    reserved_microcredits: 0,
    pending_count: 0,
    updated_at_ms: (await memoryKv.get<Record<string, unknown>>(paidFallbackRequestV3Key(keyId, "unlimited-newer")))
      .value?.updated_at_ms,
  });
  assert.deepEqual(
    (await listPaidFallbackRequestsV3(keyId, 1, kv)).map((request) => request.request_id),
    ["unlimited-newer"],
  );
  assert.deepEqual(await getPaidFallbackOutstandingV3(keyId, kv), {
    pending_requests: 0,
    unresolved_requests: 0,
    pending_markers: 0,
    has_outstanding: false,
  });
  await memoryKv.set(paidFallbackReconciliationLeaseV3Key(keyId), {
    token: "stale-cleanup-lease",
    expires_at_ms: Date.now() - 1,
  });
  assert.deepEqual(await deletePaidFallbackStateV3(keyId, kv), {
    kind: "deleted",
    deleted_requests: 2,
    deleted_windows: 0,
    deleted_pending: 0,
    deleted_leases: 1,
  });
  assert.deepEqual(await listPaidFallbackRequestsV3(keyId, 100, kv), []);
  assert.equal((await memoryKv.get(paidFallbackDeletionGuardV3Key(keyId))).value !== null, true);
  assert.deepEqual(await deletePaidFallbackStateV3(keyId, kv), {
    kind: "deleted",
    deleted_requests: 0,
    deleted_windows: 0,
    deleted_pending: 0,
    deleted_leases: 0,
  });
  assert.deepEqual(
    await admitPaidFallbackV3(
      v3AdmissionInput(keyId, "stale-after-deletion", {
        limitMicrocredits: -1,
        maximumExposureMicrocredits: null,
        windowResetAtMs: resetAtMs,
      }),
    ),
    { kind: "blocked", reason: "invalid_policy" },
  );
});

Deno.test("V3 window rollover isolates prior exposure from new admissions", async () => {
  memoryKv.clear();
  const keyId = "v3-window-rollover";
  const firstResetAtMs = Date.now() + 60_000;
  const secondResetAtMs = firstResetAtMs + 60_000;
  const first = await admitPaidFallbackV3(
    v3AdmissionInput(keyId, "rollover-first", {
      windowResetAtMs: firstResetAtMs,
      initialSettledMicrocredits: 900_000,
      maximumExposureMicrocredits: 250_000,
    }),
  );
  const second = await admitPaidFallbackV3(
    v3AdmissionInput(keyId, "rollover-second", {
      windowResetAtMs: secondResetAtMs,
      initialSettledMicrocredits: 0,
      maximumExposureMicrocredits: 250_000,
    }),
  );
  assert.equal(first.kind, "reserved");
  assert.equal(second.kind, "reserved");
  if (first.kind !== "reserved" || second.kind !== "reserved") throw new Error("expected reservations");
  assert.equal(first.reservation.reserved_microcredits, 100_000);
  assert.equal(first.reservation.quota_used_percent, 90);
  assert.equal(second.reservation.reserved_microcredits, 250_000);
  assert.equal(second.reservation.quota_used_percent, 0);
  const firstWindow = await memoryKv.get<Record<string, unknown>>(
    paidFallbackWindowV3Key(keyId, firstResetAtMs),
  );
  const secondWindow = await memoryKv.get<Record<string, unknown>>(
    paidFallbackWindowV3Key(keyId, secondResetAtMs),
  );
  assert.equal(firstWindow.value?.settled_microcredits, 900_000);
  assert.equal(firstWindow.value?.reserved_microcredits, 100_000);
  assert.equal(secondWindow.value?.settled_microcredits, 0);
  assert.equal(secondWindow.value?.reserved_microcredits, 250_000);
});

Deno.test("V3 deletion guard linearizes against an in-flight admission", async () => {
  memoryKv.clear();
  const keyId = "v3-deletion-race";
  memoryKv.beforeAtomicCommit = () => {
    void memoryKv.set(paidFallbackDeletionGuardV3Key(keyId), { created_at_ms: Date.now() });
  };
  assert.deepEqual(
    await admitPaidFallbackV3(v3AdmissionInput(keyId, "stale-in-flight-request")),
    { kind: "blocked", reason: "invalid_policy" },
  );
  assert.equal(memoryKv.atomicCommitFailures, 1);
  assert.equal((await memoryKv.get(paidFallbackRequestV3Key(keyId, "stale-in-flight-request"))).value, null);
  assert.deepEqual(await getPaidFallbackOutstandingV3(keyId, kv), {
    pending_requests: 0,
    unresolved_requests: 0,
    pending_markers: 0,
    has_outstanding: false,
  });
});

Deno.test("V3 undispatched release is idempotent and cannot erase dispatched exposure", async () => {
  memoryKv.clear();
  const keyId = "v3-release";
  const resetAtMs = Date.now() + 60_000;
  const released = await admitPaidFallbackV3(
    v3AdmissionInput(keyId, "release-before-dispatch", { windowResetAtMs: resetAtMs }),
  );
  assert.equal(released.kind, "reserved");
  if (released.kind !== "reserved") throw new Error("expected reservation");
  await releaseUndispatchedPaidFallbackV3(released.reservation);
  await releaseUndispatchedPaidFallbackV3(released.reservation);
  const releasedRequest = await memoryKv.get<Record<string, unknown>>(
    paidFallbackRequestV3Key(keyId, released.reservation.request_id),
  );
  assert.equal(releasedRequest.value?.billing_state, "not_billed");
  assert.equal(releasedRequest.value?.dispatch_state, "not_dispatched");
  assert.equal(releasedRequest.value?.terminal_state, "cancelled");
  assert.equal(typeof releasedRequest.value?.terminal_at_ms, "number");

  const dispatched = await admitPaidFallbackV3(
    v3AdmissionInput(keyId, "release-after-dispatch", { windowResetAtMs: resetAtMs }),
  );
  assert.equal(dispatched.kind, "reserved");
  if (dispatched.kind !== "reserved") throw new Error("expected reservation");
  await updatePaidFallbackRequestV3(dispatched.reservation, { dispatch_state: "dispatched" });
  await releaseUndispatchedPaidFallbackV3(dispatched.reservation);
  const dispatchedRequest = await memoryKv.get<Record<string, unknown>>(
    paidFallbackRequestV3Key(keyId, dispatched.reservation.request_id),
  );
  assert.equal(dispatchedRequest.value?.billing_state, "pending");
  assert.equal(dispatchedRequest.value?.dispatch_state, "dispatched");
  assert.equal(
    (await memoryKv.get<Record<string, unknown>>(paidFallbackPendingV3Key(keyId, dispatched.reservation.request_id)))
      .value !== null,
    true,
  );

  const prefetch = await admitPaidFallbackV3(
    v3AdmissionInput(keyId, "release-prefetch-intent", {
      windowResetAtMs: resetAtMs,
      dispatchIntent: true,
    }),
  );
  assert.equal(prefetch.kind, "reserved");
  if (prefetch.kind !== "reserved") throw new Error("expected reservation");
  const dispatchIntent = await memoryKv.get<Record<string, unknown>>(
    paidFallbackRequestV3Key(keyId, prefetch.reservation.request_id),
  );
  assert.equal(dispatchIntent.value?.dispatch_state, "dispatched");
  assert.equal(typeof dispatchIntent.value?.dispatched_at_ms, "number");
  await releasePaidFallbackBeforeProviderFetchV3(prefetch.reservation);
  const prefetchRequest = await memoryKv.get<Record<string, unknown>>(
    paidFallbackRequestV3Key(keyId, prefetch.reservation.request_id),
  );
  assert.equal(prefetchRequest.value?.billing_state, "not_billed");
  assert.equal(prefetchRequest.value?.dispatch_state, "not_dispatched");
  assert.equal(prefetchRequest.value?.terminal_state, "cancelled");
});

Deno.test("V3 queue delivery coalesces due rows by key and duplicate delivery is idempotent", async () => {
  memoryKv.clear();
  const originalFetch = globalThis.fetch;
  const keyId = "v3-queue-coalesced";
  const resetAtMs = Date.now() + 60_000;
  const first = await admitPaidFallbackV3(
    v3AdmissionInput(keyId, "queue-first", { windowResetAtMs: resetAtMs }),
  );
  const second = await admitPaidFallbackV3(
    v3AdmissionInput(keyId, "queue-second", { windowResetAtMs: resetAtMs }),
  );
  assert.equal(first.kind, "reserved");
  assert.equal(second.kind, "reserved");
  if (first.kind !== "reserved" || second.kind !== "reserved") throw new Error("expected reservations");
  assert.deepEqual(memoryKv.queueMessages, []);
  await updatePaidFallbackRequestV3(first.reservation, {
    provider_request_id: "provider-queue-first",
    dispatch_state: "dispatched",
  });
  await updatePaidFallbackRequestV3(second.reservation, {
    provider_request_id: "provider-queue-second",
    dispatch_state: "dispatched",
  });
  assert.equal(await enqueueDuePaidFallbackReconciliationJobsV3(Date.now() + 1, kv), 1);
  assert.deepEqual(memoryKv.queueMessages.map(({ message }) => message), [{ key_id: keyId }]);

  globalThis.fetch = () =>
    Promise.resolve(
      Response.json({
        success: true,
        data: [{
          request_id: "provider-queue-first",
          quota: 10_000,
          prompt_tokens: 1,
          completion_tokens: 2,
          model_name: "gpt-5-codex",
          created_at: Math.trunc(Date.now() / 1_000),
        }],
      }),
    );
  try {
    await withYunwuApiKey(async () => {
      assert.equal(await handlePaidFallbackReconciliationJobV3({ key_id: keyId }, kv), 1);
      assert.equal(await handlePaidFallbackReconciliationJobV3({ key_id: keyId }, kv), 0);
    });
  } finally {
    globalThis.fetch = originalFetch;
  }
  const firstRequest = await memoryKv.get<Record<string, unknown>>(
    paidFallbackRequestV3Key(keyId, "queue-first"),
  );
  const secondRequest = await memoryKv.get<Record<string, unknown>>(
    paidFallbackRequestV3Key(keyId, "queue-second"),
  );
  assert.equal(firstRequest.value?.billing_state, "settled");
  assert.equal(secondRequest.value?.billing_state, "pending");
  assert.equal(secondRequest.value?.reconciliation_attempts, 1);
});

Deno.test("V3 queue enqueue failure does not roll back durable reconciliation backoff", async () => {
  memoryKv.clear();
  const originalFetch = globalThis.fetch;
  const keyId = "v3-queue-failure";
  const requestId = "queue-failure-request";
  const decision = await admitPaidFallbackV3(v3AdmissionInput(keyId, requestId));
  assert.equal(decision.kind, "reserved");
  if (decision.kind !== "reserved") throw new Error("expected reservation");
  await updatePaidFallbackRequestV3(decision.reservation, {
    provider_request_id: "provider-queue-failure",
    dispatch_state: "dispatched",
  });
  globalThis.fetch = () => Promise.resolve(Response.json({ success: true, data: [] }));
  memoryKv.enqueueFailure = new Error("queue unavailable");
  try {
    await withYunwuApiKey(async () => {
      assert.equal(await reconcilePaidFallbackV3(keyId, Date.now() + 100, kv), 0);
    });
  } finally {
    globalThis.fetch = originalFetch;
  }
  const request = await memoryKv.get<Record<string, unknown>>(
    paidFallbackRequestV3Key(keyId, requestId),
  );
  const pending = await memoryKv.get<Record<string, unknown>>(
    paidFallbackPendingV3Key(keyId, requestId),
  );
  assert.equal(request.value?.reconciliation_attempts, 1);
  assert.equal(pending.value?.next_reconciliation_at_ms, Number(request.value?.last_reconciliation_at_ms) + 5_000);
});

Deno.test("V3 unresolved rows remain queue-reconcilable when late provider billing appears", async () => {
  memoryKv.clear();
  const originalFetch = globalThis.fetch;
  const keyId = "v3-unresolved-late";
  const requestId = "unresolved-late-request";
  const providerRequestId = "provider-unresolved-late";
  const decision = await admitPaidFallbackV3(
    v3AdmissionInput(keyId, requestId, { createdAtMs: Date.now() - 24 * 60 * 60_000 - 1 }),
  );
  assert.equal(decision.kind, "reserved");
  if (decision.kind !== "reserved") throw new Error("expected reservation");
  await updatePaidFallbackRequestV3(decision.reservation, {
    provider_request_id: providerRequestId,
    dispatch_state: "dispatched",
  });
  globalThis.fetch = () => Promise.resolve(Response.json({ success: true, data: [] }));
  try {
    await withYunwuApiKey(async () => {
      assert.equal(await reconcilePaidFallbackV3(keyId, Date.now() + 100, kv), 0);
    });
  } finally {
    globalThis.fetch = originalFetch;
  }
  const unresolved = await memoryKv.get<Record<string, unknown>>(
    paidFallbackRequestV3Key(keyId, requestId),
  );
  assert.equal(unresolved.value?.billing_state, "unresolved");
  const pending = await memoryKv.get<Record<string, unknown>>(
    paidFallbackPendingV3Key(keyId, requestId),
  );
  await memoryKv.set(paidFallbackPendingV3Key(keyId, requestId), {
    ...pending.value as Record<string, unknown>,
    next_reconciliation_at_ms: Date.now() - 1,
  });
  globalThis.fetch = () =>
    Promise.resolve(
      Response.json({
        success: true,
        data: [{
          request_id: providerRequestId,
          quota: 20_000,
          prompt_tokens: 1,
          completion_tokens: 2,
          model_name: "gpt-5-codex",
          created_at: Math.trunc(Date.now() / 1_000),
        }],
      }),
    );
  try {
    await withYunwuApiKey(async () => {
      assert.equal(await handlePaidFallbackReconciliationJobV3({ key_id: keyId }, kv), 1);
    });
  } finally {
    globalThis.fetch = originalFetch;
  }
  const settled = await memoryKv.get<Record<string, unknown>>(
    paidFallbackRequestV3Key(keyId, requestId),
  );
  assert.equal(settled.value?.billing_state, "settled");
  assert.equal((await memoryKv.get(paidFallbackPendingV3Key(keyId, requestId))).value, null);
});

Deno.test("disabled paid fallback does not rewrite an expired usage window", async () => {
  memoryKv.clear();
  const expiredResetAtMs = Date.now() - 1;
  const record = await seedStrictKey({
    id: "key-disabled-expired",
    hash: "hash-disabled-expired",
    paid_fallback_enabled: false,
    usage_reset_at_ms: expiredResetAtMs,
  });

  assert.deepEqual(
    await reservePaidFallback(reservationInput(String(record.id), "request-disabled")),
    { kind: "skip", reason: "disabled" },
  );
  const stored = await memoryKv.get<Record<string, unknown>>(apiKeyIdKey(String(record.id)));
  assert.equal(stored.value?.usage_reset_at_ms, expiredResetAtMs);
  assert.equal(stored.value?.paid_fallback_reservation_request_id, null);
});

(Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = originalOpenKv;
