import assert from "node:assert/strict";

import { setKvForTest } from "../src/kv.ts";
import { CODEX_AUTH_POOL_KV_KEY, resetCodexAuthCacheForTest } from "../src/codex.ts";
import {
  getPersistedProviderCapacityView,
  handleProviderCapacity,
  PROVIDER_CAPACITY_HISTORY_BUCKET_MS,
  PROVIDER_CAPACITY_HISTORY_KEY_PREFIX,
  PROVIDER_CAPACITY_HISTORY_RETENTION_MS,
  PROVIDER_CAPACITY_LEASE_KEY,
  PROVIDER_CAPACITY_SNAPSHOT_KEY,
  PROVIDER_CAPACITY_SOURCE_STALE_MS,
  providerCapacityHistoryKey,
  refreshProviderCapacity,
} from "../src/provider_capacity.ts";
import { YUNWU_QUOTA_STATE_KEY } from "../src/yunwu_quota.ts";

type StoredValue = {
  value: unknown;
  versionstamp: string;
};

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);

class CapacityKvStore extends Map<string, StoredValue> {
  private nextVersion = 0;

  clearStore(): void {
    super.clear();
    this.nextVersion = 0;
    resetCodexAuthCacheForTest();
  }

  put(key: Deno.KvKey, value: unknown): void {
    this.nextVersion += 1;
    this.set(keyToString(key), { value, versionstamp: `v${this.nextVersion}` });
  }

  remove(key: Deno.KvKey): void {
    this.delete(keyToString(key));
  }

  version(key: Deno.KvKey): string | null {
    return this.get(keyToString(key))?.versionstamp ?? null;
  }
}

const kvStore = new CapacityKvStore();
const kvStub = {
  get: (key: Deno.KvKey) => {
    const stored = kvStore.get(keyToString(key));
    return {
      key,
      value: stored?.value ?? null,
      versionstamp: stored?.versionstamp ?? null,
    } as Deno.KvEntryMaybe<unknown>;
  },
  set: (key: Deno.KvKey, value: unknown) => {
    kvStore.put(key, value);
    return { ok: true } as const;
  },
  delete: (key: Deno.KvKey) => {
    kvStore.remove(key);
  },
  list: async function* (selector?: { prefix?: Deno.KvKey }) {
    const prefix = selector?.prefix;
    for (const [encodedKey, stored] of kvStore.entries()) {
      const key = JSON.parse(encodedKey) as Deno.KvKey;
      if (prefix && !prefix.every((part, index) => key[index] === part)) continue;
      yield { key, value: stored.value, versionstamp: stored.versionstamp } as Deno.KvEntry<unknown>;
    }
  },
  atomic: () => {
    const checks: Array<{ key: Deno.KvKey; versionstamp: string | null }> = [];
    const operations: Array<
      | { type: "set"; key: Deno.KvKey; value: unknown }
      | { type: "delete"; key: Deno.KvKey }
    > = [];
    const chain = {
      check: (entry: Deno.KvEntryMaybe<unknown>) => {
        checks.push({ key: entry.key, versionstamp: entry.versionstamp });
        return chain;
      },
      set: (key: Deno.KvKey, value: unknown) => {
        operations.push({ type: "set", key, value });
        return chain;
      },
      delete: (key: Deno.KvKey) => {
        operations.push({ type: "delete", key });
        return chain;
      },
      commit: () => {
        for (const check of checks) {
          if (kvStore.version(check.key) !== check.versionstamp) return { ok: false, versionstamp: null } as const;
        }
        for (const operation of operations) {
          if (operation.type === "set") kvStore.put(operation.key, operation.value);
          else kvStore.remove(operation.key);
        }
        return { ok: true, versionstamp: `v${kvStore.size}` } as const;
      },
    };
    return chain;
  },
  close: () => {},
} as unknown as Deno.Kv;

setKvForTest(kvStub);

const nowMs = 1_800_000_000_000;

const seed = (): void => {
  kvStore.clearStore();
  kvStore.put(CODEX_AUTH_POOL_KV_KEY, {
    accounts: [
      { access_token: "token-one", refresh_token: "refresh-one", account_id: "account-one", updated_at_ms: nowMs },
      { access_token: "token-two", refresh_token: "refresh-two", account_id: "account-two", updated_at_ms: nowMs },
    ],
    updated_at_ms: nowMs,
  });
  kvStore.put(YUNWU_QUOTA_STATE_KEY, {
    current_balance_quota: 750,
    post_refill_baseline_quota: 1_000,
    last_observed_used_quota: 250,
    quota_per_credit: 100,
    observed_at_ms: nowMs - 1_000,
    cycle_started_at_ms: nowMs - 5_000,
    confidence: "refill_observed",
    last_known_debits_quota: 0,
    last_inferred_credit_quota: 1_000,
    last_credit_at_ms: nowMs - 5_000,
    latest_refill_id: "refill-one",
    latest_refill_amount_credits: 10,
    latest_refill_completed_at_ms: nowMs - 5_000,
  });
  Deno.env.set("YUNWU_SYSTEM_TOKEN", "test-system-token");
  Deno.env.set("YUNWU_USER_ID", "123456");
};

const codexUsageBody = (primaryUsed: number, secondaryUsed: number) => ({
  rate_limit: {
    primary_window: {
      limit_window_seconds: 10_800,
      used_percent: primaryUsed,
      reset_at: 1_800_010_000,
    },
    secondary_window: {
      limit_window_seconds: 86_400,
      used_percent: secondaryUsed,
      reset_at: 1_800_020_000,
    },
  },
  private_account_field: "must-not-escape",
});

const createFetcher = (
  calls: Array<{ account: string | null; authorization: string | null; url: string }>,
  failureAccount: string | null = null,
) =>
(input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
  const headers = new Headers(init?.headers);
  const account = headers.get("ChatGPT-Account-ID");
  calls.push({ account, authorization: headers.get("Authorization"), url: String(input) });
  if (account === failureAccount) return Promise.resolve(new Response("upstream-secret-body", { status: 503 }));
  const used = account === "account-one" ? [12.5, 38] : [67, 81.25];
  return Promise.resolve(
    new Response(JSON.stringify(codexUsageBody(used[0], used[1])), {
      status: 200,
      headers: { "Content-Type": "application/json" },
    }),
  );
};

const historySource = (slot: 1 | 2, sampledAtMs: number, state: "available" | "unavailable" = "available") => ({
  source: "codex" as const,
  label: `Codex account ${slot}`,
  slot,
  state,
  source_observed_at_ms: state === "available" ? sampledAtMs : null,
  snapshot_at_ms: sampledAtMs,
  windows: state === "available"
    ? {
      primary: {
        limit_window_seconds: 10_800,
        used_percent: slot === 1 ? 20 : 40,
        reset_at_ms: sampledAtMs + 10_800_000,
      },
      secondary: {
        limit_window_seconds: 86_400,
        used_percent: slot === 1 ? 30 : 50,
        reset_at_ms: sampledAtMs + 86_400_000,
      },
    }
    : { primary: null, secondary: null },
});

const historyRecord = (
  bucketStartAtMs: number,
  sampledAtMs = bucketStartAtMs + 1_000,
  state: "available" | "unavailable" = "available",
) => ({
  bucket_start_at_ms: bucketStartAtMs,
  sampled_at_ms: sampledAtMs,
  sources: [historySource(1, sampledAtMs, state), historySource(2, sampledAtMs, state)],
});

Deno.test("sampler creates one fixed combined bucket and redacts account credentials", async () => {
  seed();
  const calls: Array<{ account: string | null; authorization: string | null; url: string }> = [];
  const live = await refreshProviderCapacity({ kv: kvStub, fetcher: createFetcher(calls), now: () => nowMs });
  assert.equal(live.cache_state, "live");
  assert.equal(calls.length, 2);
  assert.deepEqual(calls.map((call) => call.account).sort(), ["account-one", "account-two"]);
  assert.deepEqual(calls.map((call) => call.authorization).sort(), ["Bearer token-one", "Bearer token-two"]);
  assert.ok(calls.every((call) => call.url.endsWith("/backend-api/wham/usage")));
  assert.equal(live.history.length, 1);
  assert.equal(
    live.history[0]?.bucket_start_at_ms,
    Math.floor(nowMs / PROVIDER_CAPACITY_HISTORY_BUCKET_MS) * PROVIDER_CAPACITY_HISTORY_BUCKET_MS,
  );
  assert.equal(live.history[0]?.sources.length, 2);
  assert.equal(live.history[0]?.sources[0]?.windows.primary?.limit_window_seconds, 10_800);
  assert.equal(live.history[0]?.sources[0]?.windows.primary?.used_percent, 12.5);
  assert.equal(live.history[0]?.sources[1]?.windows.secondary?.reset_at_ms, 1_800_020_000_000);
  assert.equal(live.sources.find((source) => source.source === "yunwu")?.wallet.reset_at_ms, null);

  const callsInSameBucket: Array<{ account: string | null; authorization: string | null; url: string }> = [];
  const second = await refreshProviderCapacity({
    kv: kvStub,
    fetcher: createFetcher(callsInSameBucket),
    now: () => nowMs + 1_000,
  });
  assert.equal(callsInSameBucket.length, 2);
  assert.equal(second.history.length, 1);
  const historyKeyCount = [...kvStore.keys()]
    .map((key) => JSON.parse(key) as Deno.KvKey)
    .filter((key) => PROVIDER_CAPACITY_HISTORY_KEY_PREFIX.every((part, index) => key[index] === part)).length;
  assert.equal(historyKeyCount, 1);

  const serialized = JSON.stringify({
    response: second,
    storedSnapshot: kvStore.get(keyToString(PROVIDER_CAPACITY_SNAPSHOT_KEY)),
  });
  for (const secret of ["account-one", "account-two", "token-one", "token-two", "refresh-one", "must-not-escape"]) {
    assert.equal(serialized.includes(secret), false, secret);
  }
});

Deno.test("persisted history keeps seven days and filters older buckets", async () => {
  seed();
  const currentBucket = Math.floor(nowMs / PROVIDER_CAPACITY_HISTORY_BUCKET_MS) * PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
  const oldBucket = currentBucket - PROVIDER_CAPACITY_HISTORY_RETENTION_MS - PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
  const retainedBucket = currentBucket - PROVIDER_CAPACITY_HISTORY_RETENTION_MS + PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
  kvStore.put(providerCapacityHistoryKey(oldBucket), historyRecord(oldBucket));
  kvStore.put(providerCapacityHistoryKey(retainedBucket), historyRecord(retainedBucket));
  kvStore.put(providerCapacityHistoryKey(currentBucket), historyRecord(currentBucket));

  const view = await getPersistedProviderCapacityView({ kv: kvStub, now: () => nowMs });
  assert.equal(view.cache_state, "unavailable");
  assert.deepEqual(view.history.map((point) => point.bucket_start_at_ms), [retainedBucket, currentBucket]);
  assert.equal(view.history[0]?.sources[1]?.windows.secondary?.limit_window_seconds, 86_400);
});

Deno.test("capacity endpoint reads persisted state by default and probes only for refresh=live", async () => {
  seed();
  let calls = 0;
  const fetcher = (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    calls += 1;
    return createFetcher([], null)(input, init);
  };

  const passive = await handleProviderCapacity(
    new Request("https://ai.ubq.fi/admin/providers/capacity"),
    { kv: kvStub, fetcher: () => Promise.reject(new Error("passive capacity must not fetch")), now: () => nowMs },
  );
  assert.equal(passive.status, 200);
  assert.equal((await passive.json() as { cache_state?: string }).cache_state, "unavailable");
  assert.equal(calls, 0);

  const live = await handleProviderCapacity(
    new Request("https://ai.ubq.fi/admin/providers/capacity?refresh=live"),
    { kv: kvStub, fetcher, now: () => nowMs },
  );
  assert.equal(live.status, 200);
  assert.equal((await live.json() as { cache_state?: string }).cache_state, "live");
  assert.equal(calls, 2);

  const persisted = await handleProviderCapacity(
    new Request("https://ai.ubq.fi/admin/providers/capacity"),
    {
      kv: kvStub,
      fetcher: () => Promise.reject(new Error("persisted capacity must not fetch")),
      now: () => nowMs + 1_000,
    },
  );
  const persistedBody = await persisted.json() as { cache_state?: string; history?: unknown[] };
  assert.equal(persistedBody.cache_state, "persisted");
  assert.equal(persistedBody.history?.length, 1);
  assert.equal(calls, 2);
});

Deno.test("concurrent live refreshes coalesce through the durable lease", async () => {
  seed();
  const calls: Array<{ account: string | null; authorization: string | null; url: string }> = [];
  let releaseFetch = () => {};
  const fetchReleased = new Promise<void>((resolve) => {
    releaseFetch = () => resolve();
  });
  const fetcher = async (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    const headers = new Headers(init?.headers);
    calls.push({
      account: headers.get("ChatGPT-Account-ID"),
      authorization: headers.get("Authorization"),
      url: String(input),
    });
    await fetchReleased;
    return new Response(JSON.stringify(codexUsageBody(10, 20)), { status: 200 });
  };

  const firstPromise = refreshProviderCapacity({
    kv: kvStub,
    fetcher,
    now: () => nowMs,
    createLeaseOwner: () => "sampler",
  });
  const waitDeadline = Date.now() + 2_000;
  while (calls.length < 2) {
    assert.ok(Date.now() < waitDeadline, "first refresh did not issue two upstream calls");
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
  const secondPromise = refreshProviderCapacity({
    kv: kvStub,
    fetcher,
    now: () => nowMs,
    createLeaseOwner: () => "tab",
  });
  releaseFetch();
  const [first, second] = await Promise.all([firstPromise, secondPromise]);
  assert.equal(calls.length, 2);
  assert.equal(first.history.length, 1);
  assert.equal(second.history.length, 1);
  assert.equal(kvStore.get(keyToString(PROVIDER_CAPACITY_LEASE_KEY)), undefined);
});

Deno.test("partial sampler failures stay unavailable and produce graph gaps", async () => {
  seed();
  const firstCalls: Array<{ account: string | null; authorization: string | null; url: string }> = [];
  await refreshProviderCapacity({ kv: kvStub, fetcher: createFetcher(firstCalls), now: () => nowMs });
  const secondCalls: Array<{ account: string | null; authorization: string | null; url: string }> = [];
  const partial = await refreshProviderCapacity({
    kv: kvStub,
    fetcher: createFetcher(secondCalls, "account-one"),
    now: () => nowMs + PROVIDER_CAPACITY_HISTORY_BUCKET_MS,
  });
  const accountOne = partial.sources.find((source) => source.source === "codex" && source.slot === 1);
  const accountTwo = partial.sources.find((source) => source.source === "codex" && source.slot === 2);
  assert.equal(accountOne?.state, "unavailable");
  assert.equal(accountTwo?.state, "available");
  assert.equal(partial.history.length, 2);
  assert.equal(partial.history[1]?.sources[0]?.state, "unavailable");
  assert.equal(partial.history[1]?.sources[0]?.windows.primary, null);
  assert.equal(partial.history[1]?.sources[1]?.windows.secondary?.used_percent, 81.25);
  assert.equal(JSON.stringify(partial).includes("upstream-secret-body"), false);
});

Deno.test("persisted Codex data becomes stale after the missed-run allowance", async () => {
  seed();
  await refreshProviderCapacity({ kv: kvStub, fetcher: createFetcher([]), now: () => nowMs });
  const stale = await getPersistedProviderCapacityView({
    kv: kvStub,
    now: () => nowMs + PROVIDER_CAPACITY_SOURCE_STALE_MS,
  });
  assert.equal(stale.cache_state, "stale");
  assert.equal(stale.sources.find((source) => source.source === "codex" && source.slot === 1)?.state, "stale");
  assert.equal(stale.sources.find((source) => source.source === "yunwu")?.state, "stale");
  assert.equal(stale.history[0]?.sources[0]?.state, "available");
});

Deno.test("capacity route requires admin authentication", async () => {
  const { default: handler } = await import("../src/handler.ts");
  const response = await handler(new Request("https://ai.ubq.fi/admin/providers/capacity"));
  assert.equal(response.status, 401);
});
