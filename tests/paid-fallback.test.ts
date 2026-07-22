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
  beforeAtomicCommit: (() => void) | null = null;
  #version = 0;

  clear(): void {
    this.entries.clear();
    this.beforeAtomicCommit = null;
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
  API_KEY_HASH_PREFIX,
  API_KEY_ID_PREFIX,
  apiKeyHashKey,
  apiKeyIdKey,
} = await import("../src/api_keys.ts");
const {
  apiKeyRequestLogKey,
  apiKeyUsageDailyKey,
  apiKeyUsageKey,
  listApiKeyRequestLogs,
} = await import("../src/analytics.ts");
const {
  ensurePaidFallbackBackfill,
  hasStrictPaidFallbackPolicy,
  PAID_FALLBACK_UNRECONCILABLE_TIMEOUT_MS,
  reconcileApiKeyPaidFallbacks,
  recordYunwuAmbiguousFailure,
  recordYunwuUpstreamResponse,
  reservePaidFallback,
} = await import("../src/paid_fallback.ts");

const migrationKey = ["uos_ai", "migrations", "api_key_paid_fallback_v1"] as const;

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
});

Deno.test("paid fallback backfill is one-time and idempotent for legacy ID/hash pairs", async () => {
  memoryKv.clear();
  const now = Date.now();
  const legacyId = {
    id: "legacy-key",
    name: "Legacy",
    prefix: "u_legacy",
    hash: "legacy-hash",
    created_at_ms: now - 1_000,
    expires_at_ms: -1,
    revoked_at_ms: null,
    usage_limit_requests: 50,
    usage_requests: 2,
    usage_reset_at_ms: now + 10_000,
    window_ms: 60_000,
  };
  const legacyHash = {
    id: legacyId.id,
    expires_at_ms: legacyId.expires_at_ms,
    revoked_at_ms: legacyId.revoked_at_ms,
    usage_limit_requests: legacyId.usage_limit_requests,
    usage_requests: legacyId.usage_requests,
    usage_reset_at_ms: legacyId.usage_reset_at_ms,
    window_ms: legacyId.window_ms,
  };
  await memoryKv.set([...API_KEY_ID_PREFIX, legacyId.id], legacyId);
  await memoryKv.set([...API_KEY_HASH_PREFIX, legacyId.hash], legacyHash);

  await ensurePaidFallbackBackfill(kv);

  const migratedId = await memoryKv.get<Record<string, unknown>>(apiKeyIdKey(legacyId.id));
  const migratedHash = await memoryKv.get<Record<string, unknown>>(apiKeyHashKey(legacyId.hash));
  assert.equal(hasStrictPaidFallbackPolicy(migratedId.value), true);
  assert.equal(hasStrictPaidFallbackPolicy(migratedHash.value), true);
  assert.deepEqual(
    {
      enabled: migratedId.value?.paid_fallback_enabled,
      limit: migratedId.value?.paid_fallback_limit_microcredits,
      spent: migratedId.value?.paid_fallback_spent_microcredits,
      reserved: migratedId.value?.paid_fallback_reserved_microcredits,
      reservation: migratedId.value?.paid_fallback_reservation_request_id,
      models: migratedId.value?.paid_fallback_model_ids,
      quota: migratedId.value?.paid_fallback_quota_per_credit,
      checked: migratedId.value?.paid_fallback_pricing_checked_at_ms,
    },
    {
      enabled: false,
      limit: 0,
      spent: 0,
      reserved: 0,
      reservation: null,
      models: [],
      quota: 0,
      checked: null,
    },
  );
  assert.equal(migratedHash.value?.paid_fallback_enabled, false);
  assert.equal(migratedHash.value?.paid_fallback_limit_microcredits, 0);

  const firstIdVersion = migratedId.versionstamp;
  const firstHashVersion = migratedHash.versionstamp;
  const firstMarkerVersion = memoryKv.versionstamp(migrationKey);
  await ensurePaidFallbackBackfill(kv);

  assert.equal(memoryKv.versionstamp(apiKeyIdKey(legacyId.id)), firstIdVersion);
  assert.equal(memoryKv.versionstamp(apiKeyHashKey(legacyId.hash)), firstHashVersion);
  assert.equal(memoryKv.versionstamp(migrationKey), firstMarkerVersion);
  assert.deepEqual((await memoryKv.get(migrationKey)).value, {
    version: 1,
    completed_at_ms: (await memoryKv.get<Record<string, unknown>>(migrationKey)).value?.completed_at_ms,
  });
});

Deno.test("paid fallback atomically reserves the remaining cap and permits only one outstanding request", async () => {
  memoryKv.clear();

  await withYunwuApiKey(async () => {
    const record = await seedStrictKey();
    const first = await reservePaidFallback(reservationInput(String(record.id), "request-one"));
    assert.equal(first.kind, "reserved");
    if (first.kind !== "reserved") throw new Error("expected reservation");
    assert.equal(first.reservation.reserved_microcredits, 4_000_000);

    const storedId = await memoryKv.get<Record<string, unknown>>(apiKeyIdKey(String(record.id)));
    const storedHash = await memoryKv.get<Record<string, unknown>>(apiKeyHashKey(String(record.hash)));
    assert.equal(storedId.value?.paid_fallback_reserved_microcredits, 4_000_000);
    assert.equal(storedId.value?.paid_fallback_reservation_request_id, "request-one");
    assert.equal(storedHash.value?.paid_fallback_reserved_microcredits, 4_000_000);
    assert.equal(storedHash.value?.paid_fallback_reservation_request_id, "request-one");

    const second = await reservePaidFallback(reservationInput(String(record.id), "request-two"));
    assert.deepEqual(second, {
      kind: "blocked",
      reason: "reconciliation_pending",
      reset_at_ms: record.usage_reset_at_ms,
    });

    const capped = await seedStrictKey({
      id: "key-capped",
      hash: "hash-capped",
      paid_fallback_spent_microcredits: 5_000_000,
    });
    assert.deepEqual(
      await reservePaidFallback(reservationInput(String(capped.id), "request-capped")),
      {
        kind: "blocked",
        reason: "limit_exceeded",
        reset_at_ms: capped.usage_reset_at_ms,
      },
    );

    const unlimited = await seedStrictKey({
      id: "key-unlimited",
      hash: "hash-unlimited",
      paid_fallback_limit_microcredits: -1,
      paid_fallback_spent_microcredits: 50_000_000,
    });
    const unlimitedFirst = await reservePaidFallback(
      reservationInput(String(unlimited.id), "request-unlimited"),
    );
    assert.equal(unlimitedFirst.kind, "reserved");
    if (unlimitedFirst.kind !== "reserved") throw new Error("expected unlimited reservation");
    assert.equal(unlimitedFirst.reservation.reserved_microcredits, 0);
    const storedUnlimited = await memoryKv.get<Record<string, unknown>>(
      apiKeyIdKey(String(unlimited.id)),
    );
    assert.equal(storedUnlimited.value?.paid_fallback_reserved_microcredits, 0);
    assert.equal(storedUnlimited.value?.paid_fallback_reservation_request_id, "request-unlimited");
    assert.deepEqual(
      await reservePaidFallback(reservationInput(String(unlimited.id), "request-unlimited-second")),
      {
        kind: "blocked",
        reason: "reconciliation_pending",
        reset_at_ms: unlimited.usage_reset_at_ms,
      },
    );

    const concurrent = await seedStrictKey({
      id: "key-concurrent",
      hash: "hash-concurrent",
    });
    memoryKv.beforeAtomicCommit = () => {
      const idKey = apiKeyIdKey(String(concurrent.id));
      const encoded = encodeKey(idKey);
      const existing = memoryKv.entries.get(encoded);
      if (!existing) throw new Error("missing concurrent test key");
      void memoryKv.set(idKey, {
        ...(existing.value as Record<string, unknown>),
        name: "Changed concurrently",
      });
    };
    assert.deepEqual(
      await reservePaidFallback(reservationInput(String(concurrent.id), "request-concurrent")),
      {
        kind: "blocked",
        reason: "concurrent_update",
        reset_at_ms: concurrent.usage_reset_at_ms,
      },
    );
    const concurrentHash = await memoryKv.get<Record<string, unknown>>(
      apiKeyHashKey(String(concurrent.hash)),
    );
    assert.equal(concurrentHash.value?.paid_fallback_reserved_microcredits, 0);
    assert.equal(
      (await listApiKeyRequestLogs(String(concurrent.id))).length,
      0,
    );
  });
});

Deno.test("YunWu failures and stale request-id-less reservations cannot deadlock a key", async () => {
  memoryKv.clear();

  await withYunwuApiKey(async () => {
    const record = await seedStrictKey({ paid_fallback_limit_microcredits: -1 });
    const immediate = await reservePaidFallback(reservationInput(String(record.id), "request-immediate"));
    assert.equal(immediate.kind, "reserved");
    if (immediate.kind !== "reserved") throw new Error("expected reservation");
    await recordYunwuAmbiguousFailure(immediate.reservation);

    const afterFailure = await memoryKv.get<Record<string, unknown>>(apiKeyIdKey(String(record.id)));
    assert.equal(afterFailure.value?.paid_fallback_reservation_request_id, null);
    assert.equal((await listApiKeyRequestLogs(String(record.id)))[0].billing_status, "unresolved");

    const staleCreatedAt = Date.now() - PAID_FALLBACK_UNRECONCILABLE_TIMEOUT_MS - 1;
    const stale = await reservePaidFallback(
      reservationInput(String(record.id), "request-stale", staleCreatedAt),
    );
    assert.equal(stale.kind, "reserved");
    assert.equal(await reconcileApiKeyPaidFallbacks(String(record.id)), 1);

    const afterReconcile = await memoryKv.get<Record<string, unknown>>(apiKeyIdKey(String(record.id)));
    assert.equal(afterReconcile.value?.paid_fallback_reservation_request_id, null);
    const logs = await listApiKeyRequestLogs(String(record.id));
    assert.equal(logs.find((entry) => entry.id === "request-stale")?.billing_status, "unresolved");
  });
});

Deno.test("YunWu reconciliation records exact microcredits once and releases the reservation", async () => {
  memoryKv.clear();
  const originalFetch = globalThis.fetch;
  let logFetches = 0;

  globalThis.fetch = (input: RequestInfo | URL, init?: RequestInit) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.href : input.url;
    assert.equal(url, "https://yunwu.ai/api/log/token");
    assert.equal(new Headers(init?.headers).get("Authorization"), "Bearer yunwu-test-key");
    logFetches += 1;
    return Promise.resolve(
      Response.json({
        success: true,
        data: [{
          request_id: "provider-request-one",
          quota: 123_456,
          prompt_tokens: 40,
          completion_tokens: 60,
          model_name: "gpt-5-codex",
          created_at: Math.trunc(Date.now() / 1000),
        }],
      }),
    );
  };

  try {
    await withYunwuApiKey(async () => {
      const record = await seedStrictKey({ paid_fallback_limit_microcredits: -1 });
      const createdAtMs = Date.now();
      const decision = await reservePaidFallback(
        reservationInput(String(record.id), "gateway-request-one", createdAtMs),
      );
      assert.equal(decision.kind, "reserved");
      if (decision.kind !== "reserved") throw new Error("expected reservation");

      await recordYunwuUpstreamResponse(
        decision.reservation,
        new Response("{}", { status: 200 }),
        "provider-request-one",
      );

      assert.equal(await reconcileApiKeyPaidFallbacks(String(record.id)), 1);

      const storedId = await memoryKv.get<Record<string, unknown>>(apiKeyIdKey(String(record.id)));
      const storedHash = await memoryKv.get<Record<string, unknown>>(apiKeyHashKey(String(record.hash)));
      assert.equal(storedId.value?.paid_fallback_spent_microcredits, 1_246_912);
      assert.equal(storedId.value?.paid_fallback_limit_microcredits, -1);
      assert.equal(storedId.value?.paid_fallback_reserved_microcredits, 0);
      assert.equal(storedId.value?.paid_fallback_reservation_request_id, null);
      assert.equal(storedHash.value?.paid_fallback_spent_microcredits, 1_246_912);
      assert.equal(storedHash.value?.paid_fallback_limit_microcredits, -1);
      assert.equal(storedHash.value?.paid_fallback_reserved_microcredits, 0);

      const logs = await listApiKeyRequestLogs(String(record.id));
      assert.equal(logs.length, 1);
      assert.deepEqual(
        {
          billing: logs[0].billing_status,
          provider: logs[0].provider,
          providerRequestId: logs[0].provider_request_id,
          quota: logs[0].provider_quota,
          input: logs[0].input_tokens,
          output: logs[0].output_tokens,
          spend: logs[0].spend_microcredits,
        },
        {
          billing: "reconciled",
          provider: "yunwu",
          providerRequestId: "provider-request-one",
          quota: 123_456,
          input: 40,
          output: 60,
          spend: 246_912,
        },
      );
      assert.ok(
        (memoryKv.expiration(
          apiKeyRequestLogKey(String(record.id), createdAtMs, "gateway-request-one"),
        ) ?? 0) > Date.now(),
      );

      const usage = await memoryKv.get<Record<string, unknown>>(apiKeyUsageKey(String(record.id)));
      assert.equal(usage.value?.yunwu_fallback_requests, 1);
      assert.equal(usage.value?.yunwu_input_tokens, 40);
      assert.equal(usage.value?.yunwu_output_tokens, 60);
      assert.equal(usage.value?.yunwu_total_tokens, 100);
      assert.equal(usage.value?.yunwu_spend_microcredits, 246_912);
      const daily = await memoryKv.get<{ days?: Array<Record<string, unknown>> }>(
        apiKeyUsageDailyKey(String(record.id)),
      );
      assert.equal(daily.value?.days?.[0]?.yunwu_fallback_requests, 1);
      assert.equal(daily.value?.days?.[0]?.yunwu_spend_microcredits, 246_912);

      assert.equal(await reconcileApiKeyPaidFallbacks(String(record.id)), 0);
      assert.equal(logFetches, 1);
      assert.equal(
        (await memoryKv.get<Record<string, unknown>>(apiKeyUsageKey(String(record.id)))).value
          ?.yunwu_spend_microcredits,
        246_912,
      );

      const next = await reservePaidFallback(
        reservationInput(String(record.id), "gateway-request-two"),
      );
      assert.equal(next.kind, "reserved");
      if (next.kind !== "reserved") throw new Error("expected another unlimited reservation");
      assert.equal(next.reservation.reserved_microcredits, 0);
    });
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("late reconciliation after a window reset updates lifetime spend but not the new window", async () => {
  memoryKv.clear();
  const originalFetch = globalThis.fetch;

  globalThis.fetch = () =>
    Promise.resolve(
      Response.json({
        success: true,
        data: [{
          request_id: "provider-late",
          quota: 50_000,
          prompt_tokens: 10,
          completion_tokens: 5,
          model_name: "gpt-5-codex",
          created_at: Math.trunc(Date.now() / 1000),
        }],
      }),
    );

  try {
    await withYunwuApiKey(async () => {
      const record = await seedStrictKey({
        id: "key-late",
        hash: "hash-late",
        paid_fallback_spent_microcredits: 0,
      });
      const createdAtMs = Date.now();
      const decision = await reservePaidFallback(
        reservationInput(String(record.id), "gateway-late", createdAtMs),
      );
      assert.equal(decision.kind, "reserved");
      if (decision.kind !== "reserved") throw new Error("expected reservation");
      await recordYunwuUpstreamResponse(
        decision.reservation,
        new Response("{}", { status: 200 }),
        "provider-late",
      );

      const nextResetAtMs = Number(record.usage_reset_at_ms) + Number(record.window_ms);
      const resetId = {
        ...(await memoryKv.get<Record<string, unknown>>(apiKeyIdKey(String(record.id)))).value,
        usage_reset_at_ms: nextResetAtMs,
        paid_fallback_spent_microcredits: 0,
      };
      const resetHash = {
        ...(await memoryKv.get<Record<string, unknown>>(apiKeyHashKey(String(record.hash)))).value,
        usage_reset_at_ms: nextResetAtMs,
        paid_fallback_spent_microcredits: 0,
      };
      await memoryKv.set(apiKeyIdKey(String(record.id)), resetId);
      await memoryKv.set(apiKeyHashKey(String(record.hash)), resetHash);

      assert.deepEqual(
        await reservePaidFallback(reservationInput(String(record.id), "new-window-request")),
        {
          kind: "blocked",
          reason: "reconciliation_pending",
          reset_at_ms: nextResetAtMs,
        },
      );
      assert.equal(await reconcileApiKeyPaidFallbacks(String(record.id)), 1);
      const after = await memoryKv.get<Record<string, unknown>>(apiKeyIdKey(String(record.id)));
      assert.equal(after.value?.usage_reset_at_ms, nextResetAtMs);
      assert.equal(after.value?.paid_fallback_spent_microcredits, 0);
      assert.equal(after.value?.paid_fallback_reserved_microcredits, 0);
      assert.equal(after.value?.paid_fallback_reservation_request_id, null);
      assert.equal(
        (await memoryKv.get<Record<string, unknown>>(apiKeyUsageKey(String(record.id)))).value
          ?.yunwu_spend_microcredits,
        100_000,
      );
      assert.equal((await listApiKeyRequestLogs(String(record.id)))[0].spend_microcredits, 100_000);
    });
  } finally {
    globalThis.fetch = originalFetch;
  }
});

(Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = originalOpenKv;
