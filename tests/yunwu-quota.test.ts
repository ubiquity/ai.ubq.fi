import assert from "node:assert/strict";
import { buildCodexQuotaHeaders, OPENAI_SUBSCRIPTION_LIMIT_NAME, YUNWU_CODEX_LIMIT_NAME } from "../src/codex_quota.ts";
import {
  fetchYunwuQuotaObservation,
  getCachedYunwuQuotaSnapshot,
  getYunwuQuotaSnapshot,
  invalidateYunwuQuotaSnapshot,
  readYunwuAccountCredentials,
  updateYunwuQuotaState,
  YUNWU_QUOTA_FRESH_MS,
  YUNWU_QUOTA_INVALIDATION_KEY,
  YUNWU_QUOTA_RETENTION_MS,
  YUNWU_QUOTA_STATE_KEY,
  YUNWU_SYSTEM_TOKEN_ENV,
  YUNWU_USER_ID_ENV,
  type YunwuQuotaObservation,
  type YunwuQuotaSnapshot,
  type YunwuQuotaState,
} from "../src/yunwu_quota.ts";

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
      set: (key: Deno.KvKey, value: unknown, _options?: { expireIn?: number }) => {
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

const credentials = { systemToken: "system-token", userId: "717235" };

const restoreEnv = (key: string, value: string | undefined): void => {
  if (value === undefined) Deno.env.delete(key);
  else Deno.env.set(key, value);
};

Deno.test("YunWu account credentials require a system token and numeric user id", () => {
  const originalToken = Deno.env.get(YUNWU_SYSTEM_TOKEN_ENV);
  const originalUserId = Deno.env.get(YUNWU_USER_ID_ENV);
  try {
    Deno.env.set(YUNWU_SYSTEM_TOKEN_ENV, "system-token");
    Deno.env.set(YUNWU_USER_ID_ENV, "717235");
    assert.deepEqual(readYunwuAccountCredentials(), credentials);

    Deno.env.set(YUNWU_USER_ID_ENV, "not-numeric");
    assert.equal(readYunwuAccountCredentials(), null);

    Deno.env.set(YUNWU_SYSTEM_TOKEN_ENV, "contains whitespace");
    Deno.env.set(YUNWU_USER_ID_ENV, "717235");
    assert.equal(readYunwuAccountCredentials(), null);
  } finally {
    restoreEnv(YUNWU_SYSTEM_TOKEN_ENV, originalToken);
    restoreEnv(YUNWU_USER_ID_ENV, originalUserId);
  }
});

const observation = (overrides: Partial<YunwuQuotaObservation> = {}): YunwuQuotaObservation => ({
  balance_quota: 50_000_000,
  used_quota: 100_000,
  quota_per_credit: 500_000,
  observed_at_ms: 1_000_000,
  latest_refill: {
    id: "refill-1",
    amount_credits: 100,
    completed_at_ms: 900_000,
  },
  ...overrides,
});

const state = (overrides: Partial<YunwuQuotaState> = {}): YunwuQuotaState => ({
  current_balance_quota: 50_000_000,
  post_refill_baseline_quota: 50_000_000,
  last_observed_used_quota: 100_000,
  quota_per_credit: 500_000,
  observed_at_ms: 1_000_000,
  cycle_started_at_ms: 1_000_000,
  confidence: "provisional",
  last_known_debits_quota: 0,
  last_inferred_credit_quota: 0,
  last_credit_at_ms: null,
  latest_refill_id: "refill-1",
  latest_refill_amount_credits: 100,
  latest_refill_completed_at_ms: 900_000,
  ...overrides,
});

const jsonResponse = (body: unknown): Response =>
  new Response(JSON.stringify(body), {
    status: 200,
    headers: { "Content-Type": "application/json; charset=utf-8" },
  });

const yunwuFetcher =
  (calls: Array<{ url: string; headers: Headers }>) =>
  (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    const headers = new Headers(init?.headers);
    calls.push({ url, headers });
    if (url === "https://yunwu.ai/api/user/self") {
      return Promise.resolve(jsonResponse({ success: true, data: { quota: 49_956_296, used_quota: 143_704 } }));
    }
    if (url === "https://yunwu.ai/api/user/topuprecords?page=1&page_size=10") {
      return Promise.resolve(jsonResponse({
        success: true,
        data: {
          page: 1,
          page_size: 10,
          total: 3,
          records: [
            { id: 10, amount: 100, complete_time: 1_784_494_386, status: "success" },
            { id: 11, amount: 200, complete_time: 0, status: "pending" },
            { id: 9, amount: 50, complete_time: 1_700_000_000, status: "success" },
          ],
        },
      }));
    }
    if (url === "https://yunwu.ai/api/status") {
      return Promise.resolve(jsonResponse({ success: true, data: { quota_per_unit: 500_000 } }));
    }
    throw new Error(`Unexpected URL: ${url}`);
  };

Deno.test("YunWu account observation reads wallet balance and latest successful top-up", async () => {
  const calls: Array<{ url: string; headers: Headers }> = [];
  const result = await fetchYunwuQuotaObservation(credentials, {
    fetcher: yunwuFetcher(calls),
    now: () => 2_000_000,
  });

  assert.deepEqual(result, {
    balance_quota: 49_956_296,
    used_quota: 143_704,
    quota_per_credit: 500_000,
    observed_at_ms: 2_000_000,
    latest_refill: {
      id: "10",
      amount_credits: 100,
      completed_at_ms: 1_784_494_386_000,
    },
  });
  assert.equal(calls.length, 3);
  for (const call of calls.filter((entry) => entry.url.includes("/api/user/"))) {
    assert.equal(call.headers.get("Authorization"), "Bearer system-token");
    assert.equal(call.headers.get("New-API-User"), "717235");
    assert.equal(call.headers.has("Cookie"), false);
  }
  const statusCall = calls.find((entry) => entry.url.endsWith("/api/status"));
  assert.equal(statusCall?.headers.has("Authorization"), false);
});

Deno.test("YunWu quota seeds a provisional baseline from the first observation", () => {
  const next = updateYunwuQuotaState(null, observation());
  assert.equal(next.current_balance_quota, 50_000_000);
  assert.equal(next.post_refill_baseline_quota, 50_000_000);
  assert.equal(next.confidence, "provisional");
  assert.equal(next.last_inferred_credit_quota, 0);
});

Deno.test("YunWu quota seeds a partially spent first cycle from the latest refill amount", () => {
  const next = updateYunwuQuotaState(null, observation({ balance_quota: 40_000_000 }));
  assert.equal(next.current_balance_quota, 40_000_000);
  assert.equal(next.post_refill_baseline_quota, 50_000_000);
  assert.equal(next.confidence, "provisional");
});

Deno.test("YunWu quota keeps the refill baseline across known and external debits", () => {
  const previous = state();
  const next = updateYunwuQuotaState(
    previous,
    observation({
      balance_quota: 48_500_000,
      used_quota: 1_600_000,
      observed_at_ms: 2_000_000,
    }),
  );
  assert.equal(next.post_refill_baseline_quota, 50_000_000);
  assert.equal(next.last_known_debits_quota, 1_500_000);
  assert.equal(next.last_inferred_credit_quota, 0);
  assert.equal(next.confidence, "provisional");
});

Deno.test("YunWu quota detects a refill despite intervening debits and starts a new baseline", () => {
  const previous = state({
    current_balance_quota: 20_000_000,
    post_refill_baseline_quota: 50_000_000,
    last_observed_used_quota: 30_000_000,
  });
  const next = updateYunwuQuotaState(
    previous,
    observation({
      balance_quota: 44_000_000,
      used_quota: 31_000_000,
      observed_at_ms: 2_000_000,
      latest_refill: {
        id: "refill-2",
        amount_credits: 50,
        completed_at_ms: 1_900_000,
      },
    }),
  );
  assert.equal(next.last_known_debits_quota, 1_000_000);
  assert.equal(next.last_inferred_credit_quota, 25_000_000);
  assert.equal(next.post_refill_baseline_quota, 44_000_000);
  assert.equal(next.cycle_started_at_ms, 1_900_000);
  assert.equal(next.confidence, "refill_observed");
  assert.equal(next.latest_refill_amount_credits, 50);
});

Deno.test("YunWu quota never starts a refill cycle below the observed top-up amount", () => {
  const previous = state({
    current_balance_quota: 20_000_000,
    post_refill_baseline_quota: 50_000_000,
    last_observed_used_quota: 30_000_000,
  });
  const next = updateYunwuQuotaState(
    previous,
    observation({
      balance_quota: 20_000_000,
      used_quota: 30_000_000,
      observed_at_ms: 2_000_000,
      latest_refill: {
        id: "refill-2",
        amount_credits: 50,
        completed_at_ms: 1_900_000,
      },
    }),
  );
  assert.equal(next.last_inferred_credit_quota, 0);
  assert.equal(next.post_refill_baseline_quota, 25_000_000);
  assert.equal(next.confidence, "refill_observed");
});

Deno.test("YunWu quota treats an unlisted positive adjustment as an inferred credit", () => {
  const previous = state({ current_balance_quota: 10_000_000 });
  const next = updateYunwuQuotaState(
    previous,
    observation({
      balance_quota: 11_000_000,
      used_quota: previous.last_observed_used_quota,
      observed_at_ms: 2_000_000,
    }),
  );
  assert.equal(next.last_inferred_credit_quota, 1_000_000);
  assert.equal(next.post_refill_baseline_quota, 11_000_000);
  assert.equal(next.confidence, "inferred_adjustment");
});

Deno.test("YunWu quota ignores a reset used counter unless the wallet itself rises", () => {
  const previous = state({ current_balance_quota: 10_000_000, last_observed_used_quota: 5_000_000 });
  const debit = updateYunwuQuotaState(
    previous,
    observation({
      balance_quota: 9_000_000,
      used_quota: 100,
      observed_at_ms: 2_000_000,
    }),
  );
  assert.equal(debit.last_inferred_credit_quota, 0);
  assert.equal(debit.post_refill_baseline_quota, 50_000_000);

  const credit = updateYunwuQuotaState(
    previous,
    observation({
      balance_quota: 12_000_000,
      used_quota: 100,
      observed_at_ms: 2_000_000,
    }),
  );
  assert.equal(credit.last_inferred_credit_quota, 2_000_000);
});

Deno.test("YunWu quota cache serves fresh state without an upstream request", async () => {
  const kv = new MemoryKv();
  const now = 10_000_000;
  kv.seed(YUNWU_QUOTA_STATE_KEY, state({ observed_at_ms: now - YUNWU_QUOTA_FRESH_MS + 1 }));
  let fetches = 0;
  const snapshot = await getYunwuQuotaSnapshot(credentials, {
    kv: kv as unknown as Deno.Kv,
    now: () => now,
    fetcher: () => {
      fetches += 1;
      throw new Error("should not fetch");
    },
  });
  assert.equal(fetches, 0);
  assert.equal(snapshot?.cache_state, "fresh");
  assert.equal(snapshot?.remaining_percent, 100);
});

Deno.test("YunWu quota cache marks an invalidated observation stale", async () => {
  const kv = new MemoryKv();
  const now = 15_000_000;
  kv.seed(YUNWU_QUOTA_STATE_KEY, state({ observed_at_ms: now }));
  kv.seed(YUNWU_QUOTA_INVALIDATION_KEY, { invalidated_at_ms: now });

  const snapshot = await getCachedYunwuQuotaSnapshot({
    kv: kv as unknown as Deno.Kv,
    now: () => now,
  });

  assert.equal(snapshot?.cache_state, "stale");
  assert.equal(snapshot?.remaining_percent, 100);
});

Deno.test("YunWu quota invalidation forces a fresh account observation", async () => {
  const kv = new MemoryKv();
  const now = 16_000_000;
  kv.seed(YUNWU_QUOTA_STATE_KEY, state({ observed_at_ms: now }));
  await invalidateYunwuQuotaSnapshot({
    kv: kv as unknown as Deno.Kv,
    now: () => now,
  });
  const calls: Array<{ url: string; headers: Headers }> = [];

  const snapshot = await getYunwuQuotaSnapshot(credentials, {
    kv: kv as unknown as Deno.Kv,
    now: () => now,
    fetcher: yunwuFetcher(calls),
    createLeaseOwner: () => "invalidation-refresh-owner",
  });

  assert.equal(snapshot?.cache_state, "refreshed");
  assert.equal(calls.length, 3);
  assert.equal(kv.value(YUNWU_QUOTA_INVALIDATION_KEY), null);
});

Deno.test("YunWu quota refresh stores a new observation and computes its percentage", async () => {
  const kv = new MemoryKv();
  const now = 20_000_000;
  kv.seed(
    YUNWU_QUOTA_STATE_KEY,
    state({
      current_balance_quota: 50_000_000,
      post_refill_baseline_quota: 50_000_000,
      last_observed_used_quota: 100_000,
      observed_at_ms: now - YUNWU_QUOTA_FRESH_MS,
      latest_refill_id: "10",
    }),
  );
  const calls: Array<{ url: string; headers: Headers }> = [];
  const snapshot = await getYunwuQuotaSnapshot(credentials, {
    kv: kv as unknown as Deno.Kv,
    now: () => now,
    fetcher: yunwuFetcher(calls),
    createLeaseOwner: () => "refresh-owner",
  });
  assert.equal(snapshot?.cache_state, "refreshed");
  assert.equal(snapshot?.state.current_balance_quota, 49_956_296);
  assert.equal(snapshot?.state.last_known_debits_quota, 43_704);
  assert.ok(Math.abs((snapshot?.used_percent ?? 0) - 0.087408) < 1e-9);
  assert.equal(calls.length, 3);
});

Deno.test("YunWu quota serves stale state on refresh failure and drops expired state", async () => {
  const now = 30_000_000;
  const staleKv = new MemoryKv();
  staleKv.seed(YUNWU_QUOTA_STATE_KEY, state({ observed_at_ms: now - YUNWU_QUOTA_FRESH_MS }));
  const stale = await getYunwuQuotaSnapshot(credentials, {
    kv: staleKv as unknown as Deno.Kv,
    now: () => now,
    fetcher: () => Promise.reject(new Error("temporary outage")),
  });
  assert.equal(stale?.cache_state, "stale");

  const expiredKv = new MemoryKv();
  expiredKv.seed(YUNWU_QUOTA_STATE_KEY, state({ observed_at_ms: now - YUNWU_QUOTA_RETENTION_MS }));
  const expired = await getYunwuQuotaSnapshot(credentials, {
    kv: expiredKv as unknown as Deno.Kv,
    now: () => now,
    fetcher: () => Promise.reject(new Error("temporary outage")),
  });
  assert.equal(expired, null);
});

Deno.test("YunWu quota refresh lease permits only one upstream refresh", async () => {
  const kv = new MemoryKv();
  const now = 40_000_000;
  kv.seed(YUNWU_QUOTA_STATE_KEY, state({ observed_at_ms: now - YUNWU_QUOTA_FRESH_MS }));
  const calls: Array<{ url: string; headers: Headers }> = [];
  let releaseFetch = (): void => {};
  const gate = new Promise<void>((resolve) => {
    releaseFetch = resolve;
  });
  const baseFetcher = yunwuFetcher(calls);
  const fetcher = async (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    await gate;
    return await baseFetcher(input, init);
  };
  let owner = 0;
  const requests = Array.from({ length: 12 }, () =>
    getYunwuQuotaSnapshot(credentials, {
      kv: kv as unknown as Deno.Kv,
      now: () => now,
      fetcher,
      createLeaseOwner: () => `owner-${++owner}`,
    }));
  await Promise.resolve();
  await Promise.resolve();
  releaseFetch();
  const snapshots = await Promise.all(requests);
  assert.equal(calls.length, 3);
  assert.equal(snapshots.filter((snapshot) => snapshot?.cache_state === "refreshed").length, 1);
  assert.equal(snapshots.filter((snapshot) => snapshot?.cache_state === "stale").length, 11);
});

Deno.test("YunWu quota invalidation rejects an in-flight pre-debit refresh", async () => {
  const kv = new MemoryKv();
  const now = 50_000_000;
  const previous = state({ observed_at_ms: now - YUNWU_QUOTA_FRESH_MS });
  kv.seed(YUNWU_QUOTA_STATE_KEY, previous);
  let releaseFetch = (): void => {};
  let markFetchStarted = (): void => {};
  const fetchStarted = new Promise<void>((resolve) => {
    markFetchStarted = resolve;
  });
  const gate = new Promise<void>((resolve) => {
    releaseFetch = resolve;
  });
  const baseFetcher = yunwuFetcher([]);
  const fetcher = async (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    markFetchStarted();
    await gate;
    return await baseFetcher(input, init);
  };
  const pending = getYunwuQuotaSnapshot(credentials, {
    kv: kv as unknown as Deno.Kv,
    now: () => now,
    fetcher,
    createLeaseOwner: () => "pre-debit-refresh-owner",
  });
  await fetchStarted;
  await invalidateYunwuQuotaSnapshot({
    kv: kv as unknown as Deno.Kv,
    now: () => now + 1,
  });
  releaseFetch();

  const snapshot = await pending;
  assert.equal(snapshot?.cache_state, "stale");
  assert.deepEqual(kv.value(YUNWU_QUOTA_STATE_KEY), previous);
  assert.deepEqual(kv.value(YUNWU_QUOTA_INVALIDATION_KEY), { invalidated_at_ms: now + 1 });
});

const quotaSnapshot = (usedPercent: number): YunwuQuotaSnapshot => ({
  state: state({
    current_balance_quota: Math.round((100 - usedPercent) * 500_000),
    post_refill_baseline_quota: 50_000_000,
  }),
  cache_state: "fresh",
  balance_credits: 100 - usedPercent,
  baseline_credits: 100,
  last_inferred_credit_credits: 0,
  remaining_percent: 100 - usedPercent,
  used_percent: usedPercent,
});

Deno.test("Codex quota headers move OpenAI limits to a named family and make YunWu canonical", () => {
  const headers = buildCodexQuotaHeaders(
    new Headers({
      "x-codex-primary-used-percent": "96.5",
      "x-codex-primary-window-minutes": "10080",
      "x-codex-primary-reset-at": "1785000000",
      "x-codex-secondary-used-percent": "20",
      "x-codex-secondary-window-minutes": "300",
      "x-codex-credits-has-credits": "true",
      "x-codex-credits-unlimited": "false",
      "x-codex-credits-balance": "12",
      "x-codex-rate-limit-reached-type": "workspace_owner_usage_limit_reached",
    }),
    quotaSnapshot(95),
  );

  assert.equal(headers.get("x-codex-limit-name"), YUNWU_CODEX_LIMIT_NAME);
  assert.equal(headers.get("x-codex-primary-used-percent"), "95");
  assert.equal(headers.has("x-codex-primary-window-minutes"), false);
  assert.equal(headers.has("x-codex-primary-reset-at"), false);
  assert.equal(headers.has("x-codex-secondary-used-percent"), false);
  assert.equal(headers.get("x-openai-subscription-limit-name"), OPENAI_SUBSCRIPTION_LIMIT_NAME);
  assert.equal(headers.get("x-openai-subscription-primary-used-percent"), "96.5");
  assert.equal(headers.get("x-openai-subscription-primary-window-minutes"), "10080");
  assert.equal(headers.get("x-openai-subscription-primary-reset-at"), "1785000000");
  assert.equal(headers.get("x-openai-subscription-secondary-used-percent"), "20");
  assert.equal(headers.has("x-codex-credits-has-credits"), false);
  assert.equal(headers.has("x-codex-rate-limit-reached-type"), false);
});

Deno.test("Codex quota headers never leak the shared canonical family without YunWu state", () => {
  const headers = buildCodexQuotaHeaders({
    "x-codex-primary-used-percent": "42",
    "x-codex-primary-window-minutes": "300",
  }, null);
  assert.equal(headers.has("x-codex-primary-used-percent"), false);
  assert.equal(headers.has("x-codex-primary-window-minutes"), false);
  assert.equal(headers.get("x-openai-subscription-primary-used-percent"), "42");
  assert.equal(headers.get("x-openai-subscription-primary-window-minutes"), "300");
});
