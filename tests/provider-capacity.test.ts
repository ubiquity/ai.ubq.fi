import assert from "node:assert/strict";

import { setKvForTest } from "../src/kv.ts";
import { codexResetRedemptionKey, codexResetShadowDecisionKey } from "../src/codex_banked_reset.ts";
import {
  CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY,
  resetCodexAccountRoutingForTest,
} from "../src/codex_account_routing.ts";
import { CODEX_AUTH_POOL_KV_KEY, resetCodexAuthCacheForTest } from "../src/codex.ts";
import {
  getPersistedProviderCapacityView,
  handleProviderCapacity,
  PROVIDER_CAPACITY_HISTORY_BUCKET_MS,
  PROVIDER_CAPACITY_HISTORY_KEY_PREFIX,
  PROVIDER_CAPACITY_HISTORY_RETENTION_MS,
  PROVIDER_CAPACITY_LEASE_KEY,
  PROVIDER_CAPACITY_RATE_LIMIT_RESET_MIN_GAIN_PERCENTAGE_POINTS,
  PROVIDER_CAPACITY_SNAPSHOT_KEY,
  PROVIDER_CAPACITY_SOURCE_STALE_MS,
  type ProviderCapacityCodexSource,
  providerCapacityHistoryKey,
  refreshProviderCapacity,
  sampleProviderCapacityForCron,
} from "../src/provider_capacity.ts";
import { PROMPT_CACHE_ANALYTICS_BUCKET_MS, promptCacheAnalyticsCounterKey } from "../src/prompt_cache_analytics.ts";
import {
  listProviderCapacityDowntimeEvents,
  PROVIDER_CAPACITY_DOWNTIME_EVENT_KV_PREFIX,
  PROVIDER_CAPACITY_RATE_LIMIT_RESET_EVENT_KV_PREFIX,
  PROVIDER_CAPACITY_RESET_EVENT_KV_PREFIX,
  recordProviderCapacityDowntimeEvent,
} from "../src/provider_capacity_events.ts";
import { METERED_QUOTA_STATE_KEY } from "../src/metered_quota.ts";
import { CountingKv } from "./helpers/counting_kv.ts";

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
    resetCodexAccountRoutingForTest();
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
  kvStore.put(METERED_QUOTA_STATE_KEY, {
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
  Deno.env.set("METERED_API_KEY", "metered-api-key");
};

const codexUsageBody = (
  primaryUsed: number,
  secondaryUsed: number,
  primaryResetAt = 1_800_010_000,
  secondaryResetAt = 1_800_020_000,
) => ({
  rate_limit: {
    primary_window: {
      limit_window_seconds: 10_800,
      used_percent: primaryUsed,
      reset_at: primaryResetAt,
    },
    secondary_window: {
      limit_window_seconds: 86_400,
      used_percent: secondaryUsed,
      reset_at: secondaryResetAt,
    },
  },
  private_account_field: "must-not-escape",
});

const codexSparkUsageBody = (
  primaryUsed: number,
  sparkUsed: number,
  sparkResetAt = 1_800_011_000,
) => ({
  rate_limit: {
    primary_window: {
      limit_window_seconds: 604_800,
      used_percent: primaryUsed,
      reset_at: 1_800_010_000,
    },
    secondary_window: null,
  },
  additional_rate_limits: [{
    limit_name: "GPT-5.3-Codex-Spark",
    metered_feature: "codex_bengalfox",
    rate_limit: {
      primary_window: {
        limit_window_seconds: 18_000,
        used_percent: sparkUsed,
        reset_at: sparkResetAt,
      },
      secondary_window: null,
    },
  }],
  private_account_field: "must-not-escape",
});

const createFetcher = (
  calls: Array<{ account: string | null; authorization: string | null; url: string }>,
  failureAccount: string | null = null,
  metered: Readonly<{
    total_available?: number;
    total_granted?: number;
    total_used?: number;
    unlimited_quota?: boolean;
  }> = {},
  codexUsage: ((account: string | null) => readonly [number, number]) | null = null,
  codexSpark = false,
  codexSparkResetAt = 1_800_011_000,
  failureStatus = 503,
  codexSparkForAccount: ((account: string | null) => boolean) | null = null,
  codexResetAt: ((account: string | null) => readonly [number, number]) | null = null,
) =>
(input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
  const headers = new Headers(init?.headers);
  const url = String(input);
  const account = headers.get("ChatGPT-Account-ID");
  calls.push({ account, authorization: headers.get("Authorization"), url });
  if (url === "https://api.openlux.ai/api/usage/token/") {
    return Promise.resolve(
      new Response(
        JSON.stringify({
          success: true,
          data: {
            total_available: metered.total_available ?? 750,
            total_granted: metered.total_granted ?? 1_000,
            total_used: metered.total_used ?? 250,
            unlimited_quota: metered.unlimited_quota ?? false,
          },
        }),
        { status: 200, headers: { "Content-Type": "application/json" } },
      ),
    );
  }
  if (account === failureAccount) {
    return Promise.resolve(new Response("upstream-secret-body", { status: failureStatus }));
  }
  const used = codexUsage?.(account) ?? (account === "account-one" ? [12.5, 38] : [67, 81.25]);
  const resetAt = codexResetAt?.(account);
  const includeCodexSpark = codexSparkForAccount ? codexSparkForAccount(account) : codexSpark;
  return Promise.resolve(
    new Response(
      JSON.stringify(
        includeCodexSpark
          ? codexSparkUsageBody(used[0], used[1], codexSparkResetAt)
          : codexUsageBody(used[0], used[1], resetAt?.[0], resetAt?.[1]),
      ),
      {
        status: 200,
        headers: { "Content-Type": "application/json" },
      },
    ),
  );
};

Deno.test("sampler carries named Codex model limits alongside null secondary windows", async () => {
  seed();
  const live = await refreshProviderCapacity({
    kv: kvStub,
    fetcher: createFetcher([], null, {}, null, true),
    now: () => nowMs,
  });
  const accountOne = live.sources.find(
    (source): source is ProviderCapacityCodexSource => source.source === "codex" && source.slot === 1,
  );
  const accountTwo = live.sources.find(
    (source): source is ProviderCapacityCodexSource => source.source === "codex" && source.slot === 2,
  );
  assert.equal(accountOne?.windows.secondary, null);
  assert.deepEqual(accountOne?.additional_rate_limits, [{
    limit_name: "GPT-5.3-Codex-Spark",
    metered_feature: "codex_bengalfox",
    windows: {
      primary: {
        limit_window_seconds: 18_000,
        used_percent: 38,
        reset_at_ms: 1_800_011_000_000,
      },
      secondary: null,
    },
  }]);
  assert.deepEqual(accountTwo?.additional_rate_limits, [{
    limit_name: "GPT-5.3-Codex-Spark",
    metered_feature: "codex_bengalfox",
    windows: {
      primary: {
        limit_window_seconds: 18_000,
        used_percent: 81.25,
        reset_at_ms: 1_800_011_000_000,
      },
      secondary: null,
    },
  }]);

  const persisted = await getPersistedProviderCapacityView({ kv: kvStub, now: () => nowMs });
  const persistedAccount = persisted.sources.find(
    (source): source is ProviderCapacityCodexSource => source.source === "codex" && source.slot === 1,
  );
  const persistedAccountTwo = persisted.sources.find(
    (source): source is ProviderCapacityCodexSource => source.source === "codex" && source.slot === 2,
  );
  assert.deepEqual(persistedAccount?.additional_rate_limits, accountOne?.additional_rate_limits);
  assert.deepEqual(persistedAccountTwo?.additional_rate_limits, accountTwo?.additional_rate_limits);
  assert.equal(JSON.stringify(live).includes("must-not-escape"), false);
  const routingObservation = JSON.stringify(kvStore.get(keyToString(CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY)));
  assert.equal(routingObservation.includes("account-one"), false);
  assert.equal(routingObservation.includes("account-two"), false);
  assert.equal(routingObservation.includes("GPT-5.3-Codex-Spark"), true);
});

Deno.test("sampler keeps unused additional limits visible while deferring them from routing", async () => {
  seed();
  const live = await refreshProviderCapacity({
    kv: kvStub,
    fetcher: createFetcher([], null, {}, () => [12.5, 0], true, nowMs / 1_000 + 18_000),
    now: () => nowMs,
  });
  const accountOne = live.sources.find(
    (source): source is ProviderCapacityCodexSource => source.source === "codex" && source.slot === 1,
  );
  assert.deepEqual(accountOne?.additional_rate_limits, [{
    limit_name: "GPT-5.3-Codex-Spark",
    metered_feature: "codex_bengalfox",
    windows: {
      primary: {
        limit_window_seconds: 18_000,
        used_percent: 0,
        reset_at_ms: nowMs + 18_000_000,
      },
      secondary: null,
    },
  }]);
  const routingObservation = JSON.stringify(kvStore.get(keyToString(CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY)));
  assert.equal(routingObservation.includes("GPT-5.3-Codex-Spark"), false);
});

Deno.test("sampler mirrors a reported Spark limit to an available sibling for the admin view", async () => {
  seed();
  const live = await refreshProviderCapacity({
    kv: kvStub,
    fetcher: createFetcher([], null, {}, null, true, 1_800_011_000, 503, (account) => account === "account-one"),
    now: () => nowMs,
  });
  const accountOne = live.sources.find(
    (source): source is ProviderCapacityCodexSource => source.source === "codex" && source.slot === 1,
  );
  const accountTwo = live.sources.find(
    (source): source is ProviderCapacityCodexSource => source.source === "codex" && source.slot === 2,
  );
  assert.equal(accountOne?.additional_rate_limits.length, 1);
  assert.deepEqual(accountTwo?.additional_rate_limits, accountOne?.additional_rate_limits);

  const storedSnapshot = kvStore.get(keyToString(PROVIDER_CAPACITY_SNAPSHOT_KEY))?.value as {
    sources?: readonly { source: string; slot?: number; additional_rate_limits?: readonly unknown[] }[];
  } | undefined;
  assert.deepEqual(
    storedSnapshot?.sources?.find((source) => source.source === "codex" && source.slot === 2)
      ?.additional_rate_limits,
    [],
  );

  const persisted = await getPersistedProviderCapacityView({ kv: kvStub, now: () => nowMs });
  const persistedAccountTwo = persisted.sources.find(
    (source): source is ProviderCapacityCodexSource => source.source === "codex" && source.slot === 2,
  );
  assert.deepEqual(persistedAccountTwo?.additional_rate_limits, accountTwo?.additional_rate_limits);
  const routingStore = kvStore.get(keyToString(CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY))?.value as {
    observations?: readonly { slot: number; additional_rate_limits: readonly unknown[] }[];
  } | undefined;
  assert.deepEqual(
    routingStore?.observations?.map((observation) => [observation.slot, observation.additional_rate_limits.length]),
    [[0, 1], [1, 0]],
  );
});

const TEST_ACCOUNT_COHORT_IDS = {
  1: "1".repeat(64),
  2: "2".repeat(64),
} as const;

const historySource = (
  slot: 1 | 2,
  sampledAtMs: number,
  state: "available" | "unavailable" = "available",
  options: Readonly<{
    primaryUsed?: number;
    primaryResetAtMs?: number;
    accountCohortId?: string | null;
  }> = {},
) => ({
  source: "codex" as const,
  label: `Codex account ${slot}`,
  slot,
  account_cohort_id: options.accountCohortId === undefined ? TEST_ACCOUNT_COHORT_IDS[slot] : options.accountCohortId,
  state,
  source_observed_at_ms: state === "available" ? sampledAtMs : null,
  snapshot_at_ms: sampledAtMs,
  windows: state === "available"
    ? {
      primary: {
        limit_window_seconds: 10_800,
        used_percent: options.primaryUsed ?? (slot === 1 ? 20 : 40),
        reset_at_ms: options.primaryResetAtMs ?? sampledAtMs + 10_800_000,
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
  assert.equal(calls.length, 3);
  const codexCalls = calls.filter((call) => call.url.endsWith("/backend-api/wham/usage"));
  const meteredCalls = calls.filter((call) => call.url.startsWith("https://api.openlux.ai/api/"));
  assert.equal(codexCalls.length, 2);
  assert.equal(meteredCalls.length, 1);
  assert.deepEqual(codexCalls.map((call) => call.account).sort(), ["account-one", "account-two"]);
  assert.deepEqual(codexCalls.map((call) => call.authorization).sort(), ["Bearer token-one", "Bearer token-two"]);
  assert.equal(live.history.length, 1);
  assert.equal(
    live.history[0]?.bucket_start_at_ms,
    Math.floor(nowMs / PROVIDER_CAPACITY_HISTORY_BUCKET_MS) * PROVIDER_CAPACITY_HISTORY_BUCKET_MS,
  );
  assert.equal(live.history[0]?.sources.length, 3);
  assert.equal(live.history[0]?.sources[0]?.windows.primary?.limit_window_seconds, 10_800);
  assert.equal(live.history[0]?.sources[0]?.windows.primary?.used_percent, 12.5);
  assert.equal(live.history[0]?.sources[1]?.windows.secondary?.reset_at_ms, 1_800_020_000_000);
  assert.equal(live.sources.find((source) => source.source === "metered")?.wallet.reset_at_ms, null);
  for (const source of live.sources.filter((candidate) => candidate.source === "codex")) {
    assert.match(source.account_cohort_id ?? "", /^[a-f0-9]{64}$/);
  }

  const callsInSameBucket: Array<{ account: string | null; authorization: string | null; url: string }> = [];
  const second = await refreshProviderCapacity({
    kv: kvStub,
    fetcher: createFetcher(callsInSameBucket),
    now: () => nowMs + 1_000,
  });
  assert.equal(callsInSameBucket.length, 3);
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

Deno.test("sampler refresh observes Metered token usage", async () => {
  seed();
  const initial = await refreshProviderCapacity({
    kv: kvStub,
    fetcher: createFetcher([]),
    now: () => nowMs,
  });
  assert.equal(initial.history[0]?.sources[2]?.wallet.total_available, 750);

  const topupCalls: Array<{ account: string | null; authorization: string | null; url: string }> = [];
  const topup = await refreshProviderCapacity({
    kv: kvStub,
    fetcher: createFetcher(topupCalls, null, {
      total_available: 1_750,
      total_granted: 2_000,
      total_used: 250,
    }),
    now: () => nowMs + PROVIDER_CAPACITY_HISTORY_BUCKET_MS,
  });
  const current = topup.sources.find((source) => source.source === "metered");
  assert.equal(current?.state, "available");
  assert.equal(current?.wallet.total_available, 1_750);
  assert.equal(current?.wallet.total_used, 250);
  assert.equal(topup.history.length, 2);
  assert.equal(topup.history[0]?.sources[2]?.wallet.total_available, 750);
  assert.equal(topup.history[1]?.sources[2]?.wallet.total_available, 1_750);
  assert.equal(topupCalls.length, 3);
});

Deno.test("sampler preserves an exhaustion point when a reset refills the same bucket", async () => {
  seed();
  let phase = 0;
  const calls: Array<{ account: string | null; authorization: string | null; url: string }> = [];
  const fetcher = createFetcher(calls, null, {}, () => phase === 0 ? [100, 100] : [0, 0]);
  const exhausted = await refreshProviderCapacity({ kv: kvStub, fetcher, now: () => nowMs });
  phase = 1;
  const refilled = await refreshProviderCapacity({
    kv: kvStub,
    fetcher,
    now: () => nowMs + 1_000,
  });
  assert.equal(exhausted.history.length, 1);
  assert.equal(refilled.history.length, 2);
  assert.equal(refilled.history[0]?.sampled_at_ms, nowMs);
  assert.equal(refilled.history[0]?.sources[0]?.windows.primary?.used_percent, 100);
  assert.equal(refilled.history[1]?.sampled_at_ms, nowMs + 1_000);
  assert.equal(refilled.history[1]?.sources[0]?.windows.primary?.used_percent, 0);
});

Deno.test("sampler records a substantial rate-limit reset and preserves both same-bucket samples", async () => {
  seed();
  let phase = 0;
  const fetcher = createFetcher(
    [],
    null,
    {},
    (account) => account === "account-one" ? (phase === 0 ? [80, 35] : [20, 35]) : [45, 55],
    false,
    1_800_011_000,
    503,
    null,
    (account) =>
      account === "account-one"
        ? (phase === 0 ? [1_800_010_000, 1_800_020_000] : [1_800_020_000, 1_800_020_000])
        : [1_800_010_000, 1_800_020_000],
  );

  await refreshProviderCapacity({ kv: kvStub, fetcher, now: () => nowMs });
  phase = 1;
  const reset = await refreshProviderCapacity({
    kv: kvStub,
    fetcher,
    now: () => nowMs + 1_000,
  });

  assert.deepEqual(reset.history.map((point) => point.sampled_at_ms), [nowMs, nowMs + 1_000]);
  assert.equal(reset.history[0]?.sources[0]?.windows.primary?.used_percent, 80);
  assert.equal(reset.history[1]?.sources[0]?.windows.primary?.used_percent, 20);
  assert.deepEqual(reset.rate_limit_reset_events, [{
    v: 1,
    event_id: "openai-1-primary-1800000000000-1800000001000",
    provider: "openai",
    slot: 1,
    window: "primary",
    observed_at_ms: nowMs + 1_000,
    previous_sampled_at_ms: nowMs,
    previous_reset_at_ms: 1_800_010_000_000,
    reset_at_ms: 1_800_020_000_000,
    previous_used_percent: 80,
    current_used_percent: 20,
    capacity_gain_percentage_points: 60,
  }]);

  const persisted = await getPersistedProviderCapacityView({ kv: kvStub, now: () => nowMs + 1_000 });
  assert.deepEqual(persisted.rate_limit_reset_events, reset.rate_limit_reset_events);
  const eventKeys = [...kvStore.keys()]
    .map((key) => JSON.parse(key) as Deno.KvKey)
    .filter((key) => PROVIDER_CAPACITY_RATE_LIMIT_RESET_EVENT_KV_PREFIX.every((part, index) => key[index] === part));
  assert.equal(eventKeys.length, 1);
});

Deno.test("sampler does not correlate reset evidence across reordered accounts", async () => {
  seed();
  let phase = 0;
  const fetcher = createFetcher(
    [],
    null,
    {},
    (account) => account === "account-one" ? [80, 35] : [20, 35],
    false,
    1_800_011_000,
    503,
    null,
    () => phase === 0 ? [1_800_010_000, 1_800_020_000] : [1_800_020_000, 1_800_020_000],
  );

  const first = await refreshProviderCapacity({ kv: kvStub, fetcher, now: () => nowMs });
  const firstSlotOne = first.sources.find(
    (source): source is ProviderCapacityCodexSource => source.source === "codex" && source.slot === 1,
  );
  const firstSlotTwo = first.sources.find(
    (source): source is ProviderCapacityCodexSource => source.source === "codex" && source.slot === 2,
  );
  assert.match(firstSlotOne?.account_cohort_id ?? "", /^[a-f0-9]{64}$/);
  assert.match(firstSlotTwo?.account_cohort_id ?? "", /^[a-f0-9]{64}$/);

  kvStore.put(CODEX_AUTH_POOL_KV_KEY, {
    accounts: [
      { access_token: "token-two", refresh_token: "refresh-two", account_id: "account-two", updated_at_ms: nowMs },
      { access_token: "token-one", refresh_token: "refresh-one", account_id: "account-one", updated_at_ms: nowMs },
    ],
    updated_at_ms: nowMs + 1,
  });
  resetCodexAuthCacheForTest();
  phase = 1;
  const reordered = await refreshProviderCapacity({ kv: kvStub, fetcher, now: () => nowMs + 1_000 });
  const reorderedSlotOne = reordered.sources.find(
    (source): source is ProviderCapacityCodexSource => source.source === "codex" && source.slot === 1,
  );
  const reorderedSlotTwo = reordered.sources.find(
    (source): source is ProviderCapacityCodexSource => source.source === "codex" && source.slot === 2,
  );

  assert.deepEqual(reordered.rate_limit_reset_events, []);
  assert.equal(reorderedSlotOne?.account_cohort_id, firstSlotTwo?.account_cohort_id);
  assert.equal(reorderedSlotTwo?.account_cohort_id, firstSlotOne?.account_cohort_id);
  const persisted = JSON.stringify(
    [...kvStore.entries()].flatMap(([encodedKey, stored]) => {
      const key = JSON.parse(encodedKey) as Deno.KvKey;
      return key[0] === "uos_ai" && key[1] === "provider_capacity" ? [stored.value] : [];
    }),
  );
  assert.equal(persisted.includes("account-one"), false);
  assert.equal(persisted.includes("account-two"), false);
});

Deno.test("rejected comparison read preserves the coherent durable reset evidence", async () => {
  seed();
  let phase = 0;
  const fetcher = createFetcher(
    [],
    null,
    {},
    (account) => account === "account-one" ? (phase === 0 ? [80, 35] : [10, 35]) : [45, 55],
    false,
    1_800_011_000,
    503,
    null,
    (account) =>
      account === "account-one"
        ? (phase === 0 ? [1_800_010_000, 1_800_020_000] : [1_800_020_000, 1_800_020_000])
        : [1_800_010_000, 1_800_020_000],
  );
  await refreshProviderCapacity({ kv: kvStub, fetcher, now: () => nowMs });
  const bucketKey = providerCapacityHistoryKey(nowMs);
  const lastAvailableKey = ["uos_ai", "provider_capacity", "v1", "last_available", 1] as const;
  let rejectedHistoryRead = false;
  const failingKv = {
    ...kvStub,
    get: (key: Deno.KvKey, options?: { consistency?: "strong" | "eventual" }) => {
      if (
        !rejectedHistoryRead && options?.consistency === "strong" &&
        keyToString(key) === keyToString(bucketKey)
      ) {
        rejectedHistoryRead = true;
        return Promise.reject(new Error("injected history comparison read failure"));
      }
      return kvStub.get(key, options);
    },
  } as unknown as Deno.Kv;

  phase = 1;
  const live = await refreshProviderCapacity({ kv: failingKv, fetcher, now: () => nowMs + 1_000 });
  assert.equal(rejectedHistoryRead, true);
  assert.deepEqual(live.history.map((point) => point.sampled_at_ms), [nowMs, nowMs + 1_000]);
  assert.equal(
    (kvStore.get(keyToString(PROVIDER_CAPACITY_SNAPSHOT_KEY))?.value as { snapshot_at_ms?: number })?.snapshot_at_ms,
    nowMs,
  );
  assert.equal(
    (kvStore.get(keyToString(bucketKey))?.value as { sampled_at_ms?: number })?.sampled_at_ms,
    nowMs,
  );
  assert.equal(
    (kvStore.get(keyToString(lastAvailableKey))?.value as { sampled_at_ms?: number })?.sampled_at_ms,
    nowMs,
  );
  assert.equal(kvStore.get(keyToString(PROVIDER_CAPACITY_LEASE_KEY)), undefined);

  const retried = await refreshProviderCapacity({ kv: kvStub, fetcher, now: () => nowMs + 2_000 });
  assert.equal(retried.rate_limit_reset_events.length, 1);
  assert.deepEqual(retried.history.map((point) => point.sampled_at_ms), [nowMs, nowMs + 2_000]);
});

Deno.test("sampler detects a reset when the healthy sample follows a 401 outage", async () => {
  seed();
  let phase = 0;
  const calls: Array<{ account: string | null; authorization: string | null; url: string }> = [];
  const baseFetcher = createFetcher(
    calls,
    null,
    {},
    (account) => account === "account-one" ? (phase === 2 ? [10, 35] : [80, 35]) : [45, 55],
    false,
    1_800_011_000,
    503,
    null,
    (account) =>
      account === "account-one"
        ? (phase === 2 ? [1_800_020_000, 1_800_020_000] : [1_800_010_000, 1_800_020_000])
        : [1_800_010_000, 1_800_020_000],
  );
  const fetcher = (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    const account = new Headers(init?.headers).get("ChatGPT-Account-ID");
    if (phase === 1 && account === "account-one") {
      calls.push({ account, authorization: new Headers(init?.headers).get("Authorization"), url: String(input) });
      return Promise.resolve(new Response("unauthorized", { status: 401 }));
    }
    return baseFetcher(input, init);
  };

  await refreshProviderCapacity({ kv: kvStub, fetcher, now: () => nowMs });
  // Exercise the migration path: snapshots written before the recovery marker
  // existed still have enough retained history to identify the prior quota.
  kvStore.remove(["uos_ai", "provider_capacity", "v1", "last_available", 1]);
  kvStore.remove(["uos_ai", "provider_capacity", "v1", "last_available", 2]);
  phase = 1;
  const outageAtMs = nowMs + PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
  const outage = await refreshProviderCapacity({ kv: kvStub, fetcher, now: () => outageAtMs });
  assert.deepEqual(outage.rate_limit_reset_events, []);
  assert.equal(outage.history[1]?.sources[0]?.state, "unavailable");
  assert.equal(outage.history[1]?.sources[0]?.failure_status, 401);
  const secondOutageAtMs = nowMs + 2 * PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
  const secondOutage = await refreshProviderCapacity({ kv: kvStub, fetcher, now: () => secondOutageAtMs });
  assert.deepEqual(secondOutage.rate_limit_reset_events, []);
  assert.equal(secondOutage.history[2]?.sources[0]?.state, "unavailable");
  assert.equal(secondOutage.history[2]?.sources[0]?.failure_status, 401);

  phase = 2;
  const recoveredAtMs = nowMs + 3 * PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
  const recovered = await refreshProviderCapacity({ kv: kvStub, fetcher, now: () => recoveredAtMs });
  assert.deepEqual(recovered.rate_limit_reset_events, [{
    v: 1,
    event_id: `openai-1-primary-${nowMs}-${recoveredAtMs}`,
    provider: "openai",
    slot: 1,
    window: "primary",
    observed_at_ms: recoveredAtMs,
    previous_sampled_at_ms: nowMs,
    previous_reset_at_ms: 1_800_010_000_000,
    reset_at_ms: 1_800_020_000_000,
    previous_used_percent: 80,
    current_used_percent: 10,
    capacity_gain_percentage_points: 70,
  }]);
  assert.deepEqual(
    recovered.history.map((point) => [point.sampled_at_ms, point.sources[0]?.state]),
    [
      [nowMs, "available"],
      [outageAtMs, "unavailable"],
      [secondOutageAtMs, "unavailable"],
      [recoveredAtMs, "available"],
    ],
  );
});

Deno.test("sampler does not backfill an outage reset across an account replacement", async () => {
  seed();
  let phase = 0;
  const calls: Array<{ account: string | null; authorization: string | null; url: string }> = [];
  const baseFetcher = createFetcher(
    calls,
    null,
    {},
    (account) => account === "replacement-account" ? [10, 35] : account === "account-one" ? [80, 35] : [45, 55],
    false,
    1_800_011_000,
    503,
    null,
    (account) => account === "replacement-account" ? [1_800_020_000, 1_800_020_000] : [1_800_010_000, 1_800_020_000],
  );
  const fetcher = (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    const headers = new Headers(init?.headers);
    if (phase === 1 && headers.get("ChatGPT-Account-ID") === "account-one") {
      calls.push({
        account: "account-one",
        authorization: headers.get("Authorization"),
        url: String(input),
      });
      return Promise.resolve(new Response("unauthorized", { status: 401 }));
    }
    return baseFetcher(input, init);
  };

  await refreshProviderCapacity({ kv: kvStub, fetcher, now: () => nowMs });
  phase = 1;
  await refreshProviderCapacity({
    kv: kvStub,
    fetcher,
    now: () => nowMs + PROVIDER_CAPACITY_HISTORY_BUCKET_MS,
  });
  kvStore.put(CODEX_AUTH_POOL_KV_KEY, {
    accounts: [
      {
        access_token: "replacement-token",
        refresh_token: "replacement-refresh",
        account_id: "replacement-account",
        updated_at_ms: nowMs + 1,
      },
      { access_token: "token-two", refresh_token: "refresh-two", account_id: "account-two", updated_at_ms: nowMs },
    ],
    updated_at_ms: nowMs + 1,
  });
  resetCodexAuthCacheForTest();
  phase = 2;
  const recoveredAtMs = nowMs + 2 * PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
  const recovered = await refreshProviderCapacity({ kv: kvStub, fetcher, now: () => recoveredAtMs });
  assert.deepEqual(recovered.rate_limit_reset_events, []);
  const persisted = await getPersistedProviderCapacityView({ kv: kvStub, now: () => recoveredAtMs });
  assert.deepEqual(persisted.rate_limit_reset_events, []);
});

Deno.test("sampler does not record a capacity gain when the reset timer does not advance", async () => {
  seed();
  let phase = 0;
  const fetcher = createFetcher(
    [],
    null,
    {},
    (account) => account === "account-one" ? (phase === 0 ? [80, 35] : [20, 35]) : [45, 55],
    false,
    1_800_011_000,
    503,
    null,
    () => [1_800_010_000, 1_800_020_000],
  );

  await refreshProviderCapacity({ kv: kvStub, fetcher, now: () => nowMs });
  phase = 1;
  const changed = await refreshProviderCapacity({ kv: kvStub, fetcher, now: () => nowMs + 1_000 });

  assert.deepEqual(changed.rate_limit_reset_events, []);
});

Deno.test("sampler does not record a timer advance below the substantial-gain threshold", async () => {
  seed();
  let phase = 0;
  const belowThreshold = PROVIDER_CAPACITY_RATE_LIMIT_RESET_MIN_GAIN_PERCENTAGE_POINTS - 1;
  const fetcher = createFetcher(
    [],
    null,
    {},
    (account) => account === "account-one" ? (phase === 0 ? [60, 35] : [60 - belowThreshold, 35]) : [45, 55],
    false,
    1_800_011_000,
    503,
    null,
    (account) =>
      account === "account-one"
        ? (phase === 0 ? [1_800_010_000, 1_800_020_000] : [1_800_020_000, 1_800_020_000])
        : [1_800_010_000, 1_800_020_000],
  );

  await refreshProviderCapacity({ kv: kvStub, fetcher, now: () => nowMs });
  phase = 1;
  const changed = await refreshProviderCapacity({ kv: kvStub, fetcher, now: () => nowMs + 1_000 });

  assert.deepEqual(changed.rate_limit_reset_events, []);
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

Deno.test("persisted history backfills a reset across an unavailable outage", async () => {
  seed();
  const beforeMs = nowMs - 3 * PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
  const outageMs = nowMs - 2 * PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
  const recoveredMs = nowMs - PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
  const putPoint = (
    sampledAtMs: number,
    state: "available" | "unavailable",
    primaryUsed: readonly [number, number],
    primaryResetAtMs: readonly [number, number],
  ): void => {
    const bucketStartAtMs = Math.floor(sampledAtMs / PROVIDER_CAPACITY_HISTORY_BUCKET_MS) *
      PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
    kvStore.put(providerCapacityHistoryKey(bucketStartAtMs), {
      bucket_start_at_ms: bucketStartAtMs,
      sampled_at_ms: sampledAtMs,
      sources: [
        historySource(1, sampledAtMs, state, {
          primaryUsed: primaryUsed[0],
          primaryResetAtMs: primaryResetAtMs[0],
        }),
        historySource(2, sampledAtMs, state, {
          primaryUsed: primaryUsed[1],
          primaryResetAtMs: primaryResetAtMs[1],
        }),
      ],
    });
  };
  putPoint(beforeMs, "available", [53, 51], [1_787_011_235_000, 1_787_012_131_000]);
  putPoint(outageMs, "unavailable", [0, 0], [0, 0]);
  putPoint(recoveredMs, "available", [1, 2], [1_787_197_026_000, 1_787_197_532_000]);

  const view = await getPersistedProviderCapacityView({ kv: kvStub, now: () => nowMs });
  assert.deepEqual(
    view.rate_limit_reset_events.map((event) => [event.slot, event.window, event.capacity_gain_percentage_points]),
    [
      [1, "primary", 52],
      [2, "primary", 49],
    ],
  );
  assert.equal(
    [...kvStore.keys()].filter((key) => key.includes("rate_limit_reset_event")).length,
    2,
  );
});

Deno.test("legacy history without account cohorts remains readable but cannot infer resets", async () => {
  seed();
  const sampledAtMs = [
    nowMs - 3 * PROVIDER_CAPACITY_HISTORY_BUCKET_MS,
    nowMs - 2 * PROVIDER_CAPACITY_HISTORY_BUCKET_MS,
    nowMs - PROVIDER_CAPACITY_HISTORY_BUCKET_MS,
  ];
  for (const [index, sampleMs] of sampledAtMs.entries()) {
    const state = index === 1 ? "unavailable" : "available";
    const bucketStartAtMs = Math.floor(sampleMs / PROVIDER_CAPACITY_HISTORY_BUCKET_MS) *
      PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
    kvStore.put(providerCapacityHistoryKey(bucketStartAtMs), {
      bucket_start_at_ms: bucketStartAtMs,
      sampled_at_ms: sampleMs,
      sources: [
        historySource(1, sampleMs, state, {
          accountCohortId: null,
          primaryUsed: index === 0 ? 80 : 10,
          primaryResetAtMs: index === 0 ? 1_787_011_235_000 : 1_787_197_026_000,
        }),
        historySource(2, sampleMs, state, { accountCohortId: null }),
      ],
    });
  }

  const view = await getPersistedProviderCapacityView({ kv: kvStub, now: () => nowMs });
  assert.equal(view.history.length, 3);
  assert.equal(view.history[0]?.sources[0]?.account_cohort_id, null);
  assert.deepEqual(view.rate_limit_reset_events, []);
});

Deno.test("capacity view backfills recent verified reset events from the redacted redemption ledger", async () => {
  seed();
  const accountIdHash = "account-hash-one";
  const quotaGeneration = "v1:quota-generation-one";
  const verifiedAtMs = nowMs - 10_000;
  kvStore.put(codexResetShadowDecisionKey("episode-one"), {
    v: 1,
    episode_hash: "episode-one",
    created_at_ms: verifiedAtMs - 1_000,
    expires_at_ms: verifiedAtMs + 60_000,
    decision_reason: "selected",
    selected_account_id_hash: accountIdHash,
    selected_credit_id_hash: "credit-hash-one",
    selected_credit_expires_at_ms: null,
    fences: [
      {
        slot: 1,
        account_id_hash: accountIdHash,
        quota_generation: quotaGeneration,
        routing_generation: 3,
        quota_reset_at_ms: verifiedAtMs + 60_000,
      },
      {
        slot: 2,
        account_id_hash: "account-hash-two",
        quota_generation: "v1:quota-generation-two",
        routing_generation: 4,
        quota_reset_at_ms: verifiedAtMs + 60_000,
      },
    ],
  });
  kvStore.put(codexResetRedemptionKey(accountIdHash, quotaGeneration), {
    v: 1,
    account_id_hash: accountIdHash,
    credential_version: "credential-version-one",
    quota_generation: quotaGeneration,
    routing_generation: 3,
    idempotency_key_hash: "idempotency-hash-one",
    state: "verified",
    owner_token: "owner-one",
    fence: 1,
    lease_expires_at_ms: verifiedAtMs + 30_000,
    provider_receipt_id: null,
    created_at_ms: verifiedAtMs - 5_000,
    updated_at_ms: verifiedAtMs,
    submitted_at_ms: verifiedAtMs - 1_000,
    verified_at_ms: verifiedAtMs,
    last_error_code: null,
  });

  const view = await getPersistedProviderCapacityView({ kv: kvStub, now: () => nowMs });
  assert.deepEqual(view.reset_events, [{
    v: 1,
    event_id: "idempotency-hash-one",
    slot: 1,
    observed_at_ms: verifiedAtMs,
  }]);
  const storedEventKeys = [...kvStore.keys()]
    .map((key) => JSON.parse(key) as Deno.KvKey)
    .filter((key) => PROVIDER_CAPACITY_RESET_EVENT_KV_PREFIX.every((part, index) => key[index] === part));
  assert.equal(storedEventKeys.length, 1);
});

Deno.test("capacity endpoint reads persisted state by default and probes only for refresh=live", async () => {
  seed();
  kvStore.put(promptCacheAnalyticsCounterKey(nowMs, "input_tokens"), { value: 200n } as Deno.KvU64);
  kvStore.put(promptCacheAnalyticsCounterKey(nowMs, "cached_input_tokens"), { value: 100n } as Deno.KvU64);
  kvStore.put(promptCacheAnalyticsCounterKey(nowMs, "cache_write_input_tokens"), { value: 50n } as Deno.KvU64);
  kvStore.put(
    promptCacheAnalyticsCounterKey(nowMs, "cache_write_reported_sample_count"),
    { value: 2n } as Deno.KvU64,
  );
  kvStore.put(promptCacheAnalyticsCounterKey(nowMs, "sample_count"), { value: 2n } as Deno.KvU64);
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
  const passiveBody = await passive.json() as {
    cache_state?: string;
    prompt_cache?: {
      bucket_ms?: number;
      buckets?: Array<{ cached_percentage?: number; cache_write_input_tokens?: number }>;
    };
  };
  assert.equal(passiveBody.cache_state, "unavailable");
  assert.equal(passiveBody.prompt_cache?.bucket_ms, PROMPT_CACHE_ANALYTICS_BUCKET_MS);
  assert.equal(passiveBody.prompt_cache?.buckets?.[0]?.cached_percentage, 50);
  assert.equal(passiveBody.prompt_cache?.buckets?.[0]?.cache_write_input_tokens, 50);
  assert.equal(calls, 0);

  const live = await handleProviderCapacity(
    new Request("https://ai.ubq.fi/admin/providers/capacity?refresh=live"),
    { kv: kvStub, fetcher, now: () => nowMs },
  );
  assert.equal(live.status, 200);
  assert.equal((await live.json() as { cache_state?: string }).cache_state, "live");
  assert.equal(calls, 3);

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
  assert.equal(calls, 3);
});

Deno.test("concurrent live refreshes coalesce through the durable lease", async () => {
  seed();
  const calls: Array<{ account: string | null; authorization: string | null; url: string }> = [];
  let releaseFetch = () => {};
  const fetchReleased = new Promise<void>((resolve) => {
    releaseFetch = () => resolve();
  });
  const baseFetcher = createFetcher(calls);
  const fetcher = async (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    const response = await baseFetcher(input, init);
    await fetchReleased;
    return response;
  };

  const firstPromise = refreshProviderCapacity({
    kv: kvStub,
    fetcher,
    now: () => nowMs,
    createLeaseOwner: () => "sampler",
  });
  const waitDeadline = Date.now() + 2_000;
  while (calls.length < 3) {
    assert.ok(Date.now() < waitDeadline, "first refresh did not issue all provider calls");
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
  assert.equal(calls.length, 3);
  assert.equal(first.history.length, 1);
  assert.equal(second.history.length, 1);
  assert.equal(kvStore.get(keyToString(PROVIDER_CAPACITY_LEASE_KEY)), undefined);
});

Deno.test("partial sampler failures retain redacted downtime evidence for graph bridges", async () => {
  seed();
  const firstCalls: Array<{ account: string | null; authorization: string | null; url: string }> = [];
  await refreshProviderCapacity({ kv: kvStub, fetcher: createFetcher(firstCalls), now: () => nowMs });
  const secondCalls: Array<{ account: string | null; authorization: string | null; url: string }> = [];
  const partial = await refreshProviderCapacity({
    kv: kvStub,
    fetcher: createFetcher(secondCalls, "account-one", {}, null, false, 1_800_011_000, 504),
    now: () => nowMs + PROVIDER_CAPACITY_HISTORY_BUCKET_MS,
  });
  const accountOne = partial.sources.find((source) => source.source === "codex" && source.slot === 1);
  const accountTwo = partial.sources.find((source) => source.source === "codex" && source.slot === 2);
  assert.equal(accountOne?.state, "unavailable");
  assert.equal(accountTwo?.state, "available");
  assert.equal(partial.history.length, 2);
  assert.equal(partial.history[1]?.sources[0]?.state, "unavailable");
  assert.equal(partial.history[1]?.sources[0]?.failure_kind, "upstream_error");
  assert.equal(partial.history[1]?.sources[0]?.failure_status, 504);
  assert.equal(partial.history[1]?.sources[0]?.source_observed_at_ms, nowMs + PROVIDER_CAPACITY_HISTORY_BUCKET_MS);
  assert.equal(partial.history[1]?.sources[0]?.windows.primary, null);
  assert.equal(partial.history[1]?.sources[1]?.windows.secondary?.used_percent, 81.25);
  assert.equal(JSON.stringify(partial).includes("upstream-secret-body"), false);
});

Deno.test("downtime evidence is redacted, deduplicated per chart bucket, and retained in the view", async () => {
  seed();
  const firstObservedAtMs = nowMs + 1_000;
  assert.equal(
    await recordProviderCapacityDowntimeEvent({
      failure_kind: "upstream_error",
      status: 504,
      observed_at_ms: firstObservedAtMs,
    }, kvStub),
    true,
  );
  assert.equal(
    await recordProviderCapacityDowntimeEvent({
      failure_kind: "unreachable",
      status: null,
      observed_at_ms: firstObservedAtMs + 1_000,
    }, kvStub),
    true,
  );
  const events = await listProviderCapacityDowntimeEvents({ kv: kvStub, now: () => firstObservedAtMs + 1_000 });
  assert.equal(events.length, 1);
  assert.equal(events[0]?.provider, "openai");
  assert.equal(events[0]?.status, 504);
  assert.equal([...kvStore.keys()].some((key) => key.includes("upstream-secret-body")), false);
  assert.equal(
    [...kvStore.keys()].filter((key) => {
      const parsed = JSON.parse(key) as Deno.KvKey;
      return PROVIDER_CAPACITY_DOWNTIME_EVENT_KV_PREFIX.every((part, index) => parsed[index] === part);
    }).length,
    1,
  );
  const view = await getPersistedProviderCapacityView({ kv: kvStub, now: () => firstObservedAtMs + 1_000 });
  assert.deepEqual(view.downtime_events, events);
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
  assert.equal(stale.sources.find((source) => source.source === "metered")?.state, "stale");
  assert.equal(stale.history[0]?.sources[0]?.state, "available");
});

Deno.test("capacity route requires admin authentication", async () => {
  const { default: handler } = await import("../src/handler.ts");
  const response = await handler(new Request("https://ai.ubq.fi/admin/providers/capacity"));
  assert.equal(response.status, 401);
});

const seedCountingCapacityKv = (kv: CountingKv): void => {
  kv.clearData();
  kv.clearMeasurements();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  kv.seed(CODEX_AUTH_POOL_KV_KEY, {
    accounts: [
      { access_token: "token-one", refresh_token: "refresh-one", account_id: "account-one", updated_at_ms: nowMs },
      { access_token: "token-two", refresh_token: "refresh-two", account_id: "account-two", updated_at_ms: nowMs },
    ],
    updated_at_ms: nowMs,
  });
  kv.seed(METERED_QUOTA_STATE_KEY, {
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
};

const withCountingCapacityEnvironment = async (run: () => Promise<void>): Promise<void> => {
  const originalApiKey = Deno.env.get("METERED_API_KEY");
  Deno.env.set("METERED_API_KEY", "metered-api-key");
  try {
    await run();
  } finally {
    setKvForTest(kvStub);
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    if (originalApiKey === undefined) Deno.env.delete("METERED_API_KEY");
    else Deno.env.set("METERED_API_KEY", originalApiKey);
  }
};

Deno.test("cron sampler persists capacity without building the discarded admin projection", async () => {
  await withCountingCapacityEnvironment(async () => {
    const samplerKv = new CountingKv();
    seedCountingCapacityKv(samplerKv);
    setKvForTest(samplerKv as unknown as Deno.Kv);
    const samplerCalls: Array<{ account: string | null; authorization: string | null; url: string }> = [];
    const finishSamplerBudget = samplerKv.beginMeasurement({ authKind: "background", outcome: "capacity_sampler" });
    await sampleProviderCapacityForCron({
      kv: samplerKv as unknown as Deno.Kv,
      fetcher: createFetcher(samplerCalls),
      now: () => nowMs,
      createLeaseOwner: () => "cron-sampler",
    });
    finishSamplerBudget();

    const samplerBudget = samplerKv.budgets()[0];
    assert.ok(samplerBudget);
    assert.equal(samplerCalls.length, 3);
    assert.equal(
      samplerKv.commands.filter((command) =>
        command.scenario === "background:capacity_sampler" && command.command === "list"
      )
        .length,
      0,
      "the cron sampler must not enumerate history or reset-event projection prefixes",
    );
    // The quota refresh inside a sample also appends at most one hourly
    // balance-history read plus one upsert, hence the 22-command ceiling.
    assert.ok(
      samplerBudget.commands <= 22,
      `cron sampler budget unexpectedly grew to ${samplerBudget.commands} KV commands`,
    );
    assert.notEqual((await samplerKv.get(PROVIDER_CAPACITY_SNAPSHOT_KEY)).value, null);
    assert.notEqual((await samplerKv.get(providerCapacityHistoryKey(nowMs))).value, null);
    assert.notEqual((await samplerKv.get(CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY)).value, null);
    assert.equal((await samplerKv.get(PROVIDER_CAPACITY_LEASE_KEY)).value, null);

    const stale = await getPersistedProviderCapacityView({
      kv: samplerKv as unknown as Deno.Kv,
      now: () => nowMs + PROVIDER_CAPACITY_SOURCE_STALE_MS,
    });
    assert.equal(stale.cache_state, "stale");

    const liveRefreshKv = new CountingKv();
    seedCountingCapacityKv(liveRefreshKv);
    setKvForTest(liveRefreshKv as unknown as Deno.Kv);
    const liveRefreshCalls: Array<{ account: string | null; authorization: string | null; url: string }> = [];
    const finishLiveRefreshBudget = liveRefreshKv.beginMeasurement({
      authKind: "background",
      outcome: "capacity_view",
    });
    await refreshProviderCapacity({
      kv: liveRefreshKv as unknown as Deno.Kv,
      fetcher: createFetcher(liveRefreshCalls),
      now: () => nowMs,
      createLeaseOwner: () => "live-refresh",
    });
    finishLiveRefreshBudget();

    const liveRefreshBudget = liveRefreshKv.budgets()[0];
    assert.ok(liveRefreshBudget);
    assert.equal(liveRefreshCalls.length, 3);
    assert.ok(
      liveRefreshKv.commands.filter((command) =>
        command.scenario === "background:capacity_view" && command.command === "list"
      )
        .length >= 8,
      "the full admin view should retain its history and reset-event projections",
    );
    assert.ok(
      liveRefreshBudget.commands - samplerBudget.commands >= 10,
      "the cron-only path must retain the measured projection reduction",
    );
  });
});

Deno.test("cron sampler preserves a same-bucket reset transition", async () => {
  await withCountingCapacityEnvironment(async () => {
    const countingKv = new CountingKv();
    seedCountingCapacityKv(countingKv);
    setKvForTest(countingKv as unknown as Deno.Kv);
    let phase = 0;
    const calls: Array<{ account: string | null; authorization: string | null; url: string }> = [];
    const fetcher = createFetcher(calls, null, {}, () => phase === 0 ? [100, 100] : [0, 0]);

    await sampleProviderCapacityForCron({
      kv: countingKv as unknown as Deno.Kv,
      fetcher,
      now: () => nowMs,
      createLeaseOwner: () => "exhausted",
    });
    phase = 1;
    await sampleProviderCapacityForCron({
      kv: countingKv as unknown as Deno.Kv,
      fetcher,
      now: () => nowMs + 1_000,
      createLeaseOwner: () => "refilled",
    });

    const view = await getPersistedProviderCapacityView({
      kv: countingKv as unknown as Deno.Kv,
      now: () => nowMs + 1_000,
    });
    assert.equal(calls.length, 6);
    assert.deepEqual(view.history.map((point) => point.sampled_at_ms), [nowMs, nowMs + 1_000]);
    assert.equal(view.history[0]?.sources[0]?.windows.primary?.used_percent, 100);
    assert.equal(view.history[1]?.sources[0]?.windows.primary?.used_percent, 0);
  });
});

Deno.test("concurrent cron samplers keep one provider probe under the durable lease", async () => {
  await withCountingCapacityEnvironment(async () => {
    const countingKv = new CountingKv();
    seedCountingCapacityKv(countingKv);
    setKvForTest(countingKv as unknown as Deno.Kv);
    const calls: Array<{ account: string | null; authorization: string | null; url: string }> = [];
    let releaseFetch = (): void => {};
    const fetchReleased = new Promise<void>((resolve) => {
      releaseFetch = resolve;
    });
    const baseFetcher = createFetcher(calls);
    const fetcher = async (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
      const response = await baseFetcher(input, init);
      await fetchReleased;
      return response;
    };

    const first = sampleProviderCapacityForCron({
      kv: countingKv as unknown as Deno.Kv,
      fetcher,
      now: () => nowMs,
      createLeaseOwner: () => "first-cron",
    });
    const waitDeadline = Date.now() + 2_000;
    while (calls.length < 3) {
      assert.ok(Date.now() < waitDeadline, "first cron sampler did not issue all provider calls");
      await new Promise((resolve) => setTimeout(resolve, 5));
    }
    const second = sampleProviderCapacityForCron({
      kv: countingKv as unknown as Deno.Kv,
      fetcher,
      now: () => nowMs,
      createLeaseOwner: () => "second-cron",
    });
    releaseFetch();
    await Promise.all([first, second]);

    assert.equal(calls.length, 3);
    assert.equal((await countingKv.get(PROVIDER_CAPACITY_LEASE_KEY)).value, null);
    assert.equal(countingKv.commands.filter((command) => command.command === "list").length, 0);
  });
});
