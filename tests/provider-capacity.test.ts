import assert from "node:assert/strict";

import { setKvForTest } from "../src/kv.ts";
import { CODEX_AUTH_POOL_KV_KEY, resetCodexAuthCacheForTest } from "../src/codex.ts";
import {
  getProviderCapacitySnapshot,
  PROVIDER_CAPACITY_LEASE_KEY,
  PROVIDER_CAPACITY_SNAPSHOT_KEY,
} from "../src/provider_capacity.ts";
import { YUNWU_QUOTA_STATE_KEY } from "../src/yunwu_quota.ts";

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);

class CapacityKvStore extends Map<string, unknown> {
  override clear(): void {
    super.clear();
    resetCodexAuthCacheForTest();
  }
}

const kvStore = new CapacityKvStore();
const kvStub = {
  get: (key: Deno.KvKey) =>
    Promise.resolve(
      ({ key, value: kvStore.get(keyToString(key)) ?? null, versionstamp: "v1" }) as Deno.KvEntryMaybe<unknown>,
    ),
  set: (key: Deno.KvKey, value: unknown) => {
    kvStore.set(keyToString(key), value);
    return Promise.resolve({ ok: true } as const);
  },
  delete: (key: Deno.KvKey) => {
    kvStore.delete(keyToString(key));
    return Promise.resolve();
  },
  list: async function* () {
    yield* [];
  },
  atomic: () => {
    const operations: Array<{ type: "set" | "delete"; key: Deno.KvKey; value?: unknown }> = [];
    const chain = {
      check: () => chain,
      set: (key: Deno.KvKey, value: unknown) => {
        operations.push({ type: "set", key, value });
        return chain;
      },
      delete: (key: Deno.KvKey) => {
        operations.push({ type: "delete", key });
        return chain;
      },
      commit: () => {
        for (const operation of operations) {
          if (operation.type === "set") kvStore.set(keyToString(operation.key), operation.value);
          else kvStore.delete(keyToString(operation.key));
        }
        return Promise.resolve({ ok: true } as const);
      },
    };
    return chain;
  },
  close: () => {},
} as unknown as Deno.Kv;

(Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = () => Promise.resolve(kvStub);
setKvForTest(kvStub);

const nowMs = 1_800_000_000_000;

const seed = (): void => {
  kvStore.clear();
  kvStore.set(keyToString(CODEX_AUTH_POOL_KV_KEY), {
    accounts: [
      { access_token: "token-one", refresh_token: "refresh-one", account_id: "account-one", updated_at_ms: nowMs },
      { access_token: "token-two", refresh_token: "refresh-two", account_id: "account-two", updated_at_ms: nowMs },
    ],
    updated_at_ms: nowMs,
  });
  kvStore.set(keyToString(YUNWU_QUOTA_STATE_KEY), {
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

Deno.test("capacity snapshot binds each Codex request and redacts account credentials", async () => {
  seed();
  const calls: Array<{ account: string | null; authorization: string | null; url: string }> = [];
  const fetcher = (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    const headers = new Headers(init?.headers);
    const account = headers.get("ChatGPT-Account-ID");
    calls.push({ account, authorization: headers.get("Authorization"), url: String(input) });
    const body = account === "account-one" ? codexUsageBody(12.5, 38) : codexUsageBody(67, 81.25);
    return Promise.resolve(
      new Response(JSON.stringify(body), { status: 200, headers: { "Content-Type": "application/json" } }),
    );
  };

  const snapshot = await getProviderCapacitySnapshot({ kv: kvStub, fetcher, now: () => nowMs });
  const codex = snapshot.sources.filter((source) => source.source === "codex");
  const yunwu = snapshot.sources.find((source) => source.source === "yunwu");

  assert.equal(calls.length, 2);
  assert.deepEqual(calls.map((call) => call.account).sort(), ["account-one", "account-two"]);
  assert.deepEqual(calls.map((call) => call.authorization).sort(), ["Bearer token-one", "Bearer token-two"]);
  assert.ok(calls.every((call) => call.url.endsWith("/backend-api/wham/usage")));
  assert.deepEqual(codex.map((source) => source.source_observed_at_ms), [nowMs, nowMs]);
  assert.equal(codex[0]?.source, "codex");
  assert.equal(codex[0]?.windows.primary?.used_percent, 12.5);
  assert.equal(codex[0]?.windows.secondary?.used_percent, 38);
  assert.equal(codex[0]?.windows.primary?.reset_at_ms, 1_800_010_000_000);
  assert.equal(codex[1]?.windows.secondary?.reset_at_ms, 1_800_020_000_000);
  assert.equal(yunwu?.source, "yunwu");
  assert.equal(yunwu?.wallet.balance_credits, 7.5);
  assert.equal(yunwu?.wallet.refill_cycle_remaining_percent, 75);
  assert.equal(yunwu?.wallet.confidence, "refill_observed");
  assert.equal(yunwu?.wallet.reset_at_ms, null);

  const serialized = JSON.stringify(snapshot);
  assert.equal(serialized.includes("account-one"), false);
  assert.equal(serialized.includes("account-two"), false);
  assert.equal(serialized.includes("token-one"), false);
  assert.equal(serialized.includes("must-not-escape"), false);
  assert.equal(serialized.includes('used_percent":'), true);
  assert.equal(serialized.includes("combined"), false);
});

Deno.test("capacity cache serves fresh snapshots and reports partial source failures", async () => {
  seed();
  let calls = 0;
  const fetcher = (_input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    calls += 1;
    const account = new Headers(init?.headers).get("ChatGPT-Account-ID");
    if (account === "account-one" && calls > 2) {
      return Promise.resolve(new Response("upstream-secret-body", { status: 503 }));
    }
    return Promise.resolve(
      new Response(JSON.stringify(codexUsageBody(20, 40)), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      }),
    );
  };

  const first = await getProviderCapacitySnapshot({ kv: kvStub, fetcher, now: () => nowMs });
  assert.equal(first.cache_state, "refreshed");
  assert.equal(calls, 2);

  const cached = await getProviderCapacitySnapshot({
    kv: kvStub,
    fetcher: () => {
      throw new Error("fresh cache must not fetch");
    },
    now: () => nowMs + 1_000,
  });
  assert.equal(cached.cache_state, "fresh");
  assert.equal(calls, 2);

  const partial = await getProviderCapacitySnapshot({ kv: kvStub, fetcher, now: () => nowMs + 30_000 });
  const accountOne = partial.sources.find((source) => source.source === "codex" && source.slot === 1);
  const accountTwo = partial.sources.find((source) => source.source === "codex" && source.slot === 2);
  assert.equal(accountOne?.state, "unavailable");
  assert.equal(accountTwo?.state, "available");
  assert.equal(partial.sources.find((source) => source.source === "yunwu")?.state, "available");
  assert.equal(JSON.stringify(partial).includes("upstream-secret-body"), false);
  assert.equal(kvStore.has(keyToString(PROVIDER_CAPACITY_SNAPSHOT_KEY)), true);
  assert.equal(kvStore.has(keyToString(PROVIDER_CAPACITY_LEASE_KEY)), false);
});

Deno.test("capacity route requires admin authentication", async () => {
  const { default: handler } = await import("../src/handler.ts");
  const response = await handler(new Request("https://ai.ubq.fi/admin/providers/capacity"));
  assert.equal(response.status, 401);
});
