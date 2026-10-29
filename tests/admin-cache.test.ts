import { test } from "bun:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { runInNewContext } from "node:vm";

import { createAdminSnapshotCache } from "../static/admin-cache.js";

type StorageTransaction = Readonly<{ oncomplete?: () => void }>;

type CursorRequest = {
  onerror?: () => void;
  onsuccess?: () => void;
  result?: null;
};

/** IndexedDB settles a transaction one microtask after its request completes. */
const completeTransactionSoon = (transaction: StorageTransaction): void => {
  queueMicrotask(() => transaction.oncomplete?.());
};

/** Empties the store through an empty cursor and settles the transaction. */
const emptyCursorRequest = (records: Map<string, unknown>, transaction: StorageTransaction): CursorRequest => {
  const request: CursorRequest = {};
  queueMicrotask(() => {
    records.clear();
    request.result = null;
    request.onsuccess?.();
    completeTransactionSoon(transaction);
  });
  return request;
};

/** Same as {@link emptyCursorRequest}, settled after a delay (deferred invalidation). */
const deferredEmptyCursorRequest = (records: Map<string, unknown>, transaction: StorageTransaction): CursorRequest => {
  const request: CursorRequest = {};
  setTimeout(() => {
    records.clear();
    request.result = null;
    request.onsuccess?.();
    completeTransactionSoon(transaction);
  }, 10);
  return request;
};

const capacityHydration = (payload: unknown) => {
  const script = readFileSync(new URL("../static/admin.js", import.meta.url), "utf8");
  const meter = script.slice(script.indexOf("const claudeCapacityWindowForDisplay ="), script.indexOf("const renderProviderCapacityList ="));
  const chart = script.slice(script.indexOf("const CLAUDE_CAPACITY_CHART_SERIES ="), script.indexOf("const claudeHistoryAccountId ="));
  const render = script.slice(script.indexOf("const renderProviderCapacity ="), script.indexOf("const loadProviderCapacity ="));
  const load = script.slice(script.indexOf("const loadProviderCapacity ="), script.indexOf("const scheduleProviderCapacityChartResize ="));
  const hydrate = script.slice(script.indexOf("const cachedPayload ="), script.indexOf("const scheduleDefaultsSave ="));
  return runInNewContext(
    `let presentedClaude=[],presentedCodex=[],presentedSeries=[];
     const renderProviderCapacityList=(sources,claude,accounts)=>{
       presentedClaude=claudeCapacitySourcesForDisplay(claude,accounts);presentedCodex=sources;
     };
     const renderProviderCapacityChart=(snapshot)=>{presentedSeries=claudeCapacityChartSeries(snapshot,"a");};\n` +
      meter +
      chart +
      render +
      load +
      hydrate +
      `\n({hydrate:hydrateAdminSnapshots,refresh:loadProviderCapacity,rows:()=>presentedClaude,
       codex:()=>presentedCodex,series:()=>presentedSeries,history:()=>latestProviderCapacityChartState.snapshot.claude_history,
       status:()=>providerCapacityUpdated.textContent})`,
    {
      adminCacheScope: "fixture-scope",
      adminCacheEpoch: 1,
      adminAccessState: { isAdmin: true },
      providerCapacityLoadedAt: 0,
      providerCapacityLoading: false,
      latestProviderCapacityChartState: null,
      codexProviderSettingsLoadedAt: 1,
      codexResetSettingsToken: "fixture-token",
      providerCapacityUpdated: { textContent: "" },
      providerCapacityBadge: {},
      readAdminSnapshot: (path: string) => Promise.resolve(path === "/admin/providers/capacity" ? { savedAt: 100, payload } : null),
      getAdminToken: () => "fixture-token",
      hasAdminCredential: () => true,
      loadProviders: () => {},
      apiUrl: (path: string) => path,
      fetch: () => Promise.resolve(Response.json({}, { status: 503 })),
      setBadge: () => {},
      formatDate: String,
      formatCapacityTimestamp: String,
      unavailableCodexCapacitySource: (slot: number) => ({ source: "codex", slot, state: "unavailable" }),
      renderProviderCapacityLedger: () => {},
      updateAccessApiKeysSummary: () => {},
      updateAccessGithubSummary: () => {},
      updateAccessPubkeysSummary: () => {},
    }
  ) as {
    hydrate: () => Promise<void>;
    refresh: () => Promise<boolean>;
    rows: () => { source_observed_at_ms: number | null; windows: { five_hour: unknown }; standard: unknown[] }[];
    codex: () => unknown[];
    series: () => { window: string }[];
    history: () => unknown;
    status: () => string;
  };
};

test("actual snapshot hydration cannot relabel an old Claude row when the fresh capacity fetch fails", async () => {
  const old = {
    source: "claude_code",
    account_id: "a",
    account_cohort_id: "cohort",
    state: "available",
    source_observed_at_ms: 90,
    windows: { five_hour: { used_percent: 98, observed_at_ms: 90, reset_at_ms: 200 } },
    standard: [{ family: "requests", limit: 50, remaining: 2 }],
  };
  const codex = [
    { source: "codex", slot: 1, state: "available" },
    { source: "codex", slot: 2, state: "available" },
  ];
  const history = { points: [{ source: "response_headers", account_id: "a", windows: { seven_day: { used_percent: 20, observed_at_ms: 80 } } }] };
  const snapshot = { sources: codex, claude: old, claude_accounts: [old], claude_history: history, prompt_cache: { status: "ready" } };
  const cached = capacityHydration(snapshot);
  await cached.hydrate();
  assert.equal(await cached.refresh(), false);
  assert.match(cached.status(), /Cached.*refresh unavailable/);
  assert.equal(cached.rows()[0]?.windows.five_hour, null);
  assert.equal(cached.rows()[0]?.source_observed_at_ms, null);
  assert.equal(cached.rows()[0]?.standard.length, 0);
  assert.deepEqual(Array.from(cached.codex()), codex);
  assert.equal(cached.history(), history);
  assert.deepEqual(
    Array.from(cached.series(), (entry) => entry.window),
    ["seven_day"]
  );
  assert.equal(old.windows.five_hour.used_percent, 98, "the persisted row is never rewritten as proven");
  const proven = { ...old, quota_source: "response_headers" };
  const marked = capacityHydration({ ...snapshot, claude: proven, claude_accounts: [proven] });
  await marked.hydrate();
  assert.equal(await marked.refresh(), false);
  assert.equal(marked.rows()[0]?.source_observed_at_ms, 90);
  assert.deepEqual(marked.rows()[0]?.windows.five_hour, proven.windows.five_hour);
});

test("admin cache retries IndexedDB after a temporary open failure", async () => {
  let openCalls = 0;
  const transaction: {
    onabort?: () => void;
    oncomplete?: () => void;
    onerror?: () => void;
    objectStore: () => { put: () => void };
  } = {
    objectStore: () => ({
      put: () => {
        queueMicrotask(() => transaction.oncomplete?.());
      },
    }),
  };
  const database = { transaction: () => transaction };
  const indexedDb = {
    open: () => {
      openCalls += 1;
      if (openCalls === 1) {
        const request = {
          error: new Error("temporarily blocked"),
          onerror: undefined as (() => void) | undefined,
        };
        queueMicrotask(() => request.onerror?.());
        return request;
      }
      const request = {
        onsuccess: undefined as (() => void) | undefined,
        result: database,
      };
      queueMicrotask(() => request.onsuccess?.());
      return request;
    },
  };
  const cache = createAdminSnapshotCache({ indexedDB: indexedDb });

  assert.equal(await cache.write("scope", "key", { cached: true }), false);
  assert.equal(await cache.write("scope", "key", { cached: true }), true);
  assert.equal(openCalls, 2);
});

test("admin cache serializes invalidation behind a pending write", async () => {
  const records = new Map<string, unknown>();
  const database = {
    transaction: () => {
      const transaction: {
        onabort?: () => void;
        oncomplete?: () => void;
        onerror?: () => void;
        objectStore: () => {
          put: (record: { id: string }) => void;
          index: () => {
            openCursor: () => {
              onerror?: () => void;
              onsuccess?: () => void;
              result?: null;
            };
          };
        };
      } = {
        objectStore: () => store,
      };
      const store = {
        put: (record: { id: string }) => {
          setTimeout(() => {
            records.set(record.id, record);
            transaction.oncomplete?.();
          }, 10);
        },
        index: () => ({
          openCursor: () => emptyCursorRequest(records, transaction),
        }),
      };
      return transaction;
    },
  };
  const indexedDb = {
    open: () => {
      const request = {
        onsuccess: undefined as (() => void) | undefined,
        result: database,
      };
      queueMicrotask(() => request.onsuccess?.());
      return request;
    },
  };
  const cache = createAdminSnapshotCache({
    indexedDB: indexedDb,
    keyRange: { only: (scope: string) => scope },
  });

  const pendingWrite = cache.write("scope", "key", { cached: true });
  const pendingClear = cache.clear("scope");

  assert.equal(await pendingWrite, true);
  assert.equal(await pendingClear, true);
  assert.equal(records.size, 0);
});

test("admin cache reads wait for a queued invalidation", async () => {
  const records = new Map<string, unknown>();
  const database = {
    transaction: () => {
      const transaction: {
        onabort?: () => void;
        oncomplete?: () => void;
        onerror?: () => void;
        objectStore: () => {
          get: (id: string) => {
            onerror?: () => void;
            onsuccess?: () => void;
            result?: unknown;
          };
          index: () => {
            openCursor: () => {
              onerror?: () => void;
              onsuccess?: () => void;
              result?: null;
            };
          };
          put: (record: { id: string }) => void;
        };
      } = {
        objectStore: () => store,
      };
      const store = {
        get: (id: string) => {
          const request: {
            onerror?: () => void;
            onsuccess?: () => void;
            result?: unknown;
          } = {};
          queueMicrotask(() => {
            request.result = records.get(id);
            request.onsuccess?.();
            completeTransactionSoon(transaction);
          });
          return request;
        },
        index: () => ({
          openCursor: () => deferredEmptyCursorRequest(records, transaction),
        }),
        put: (record: { id: string }) => {
          records.set(record.id, record);
          completeTransactionSoon(transaction);
        },
      };
      return transaction;
    },
  };
  const indexedDb = {
    open: () => {
      const request = {
        onsuccess: undefined as (() => void) | undefined,
        result: database,
      };
      queueMicrotask(() => request.onsuccess?.());
      return request;
    },
  };
  const cache = createAdminSnapshotCache({
    indexedDB: indexedDb,
    keyRange: { only: (scope: string) => scope },
  });

  assert.equal(await cache.write("scope", "key", { cached: true }), true);
  const pendingClear = cache.clear("scope");
  const readAfterClear = cache.read("scope", "key");

  assert.equal(await pendingClear, true);
  assert.equal(await readAfterClear, null);
});
