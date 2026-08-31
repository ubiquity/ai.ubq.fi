import assert from "node:assert/strict";

import { createAdminSnapshotCache } from "../static/admin-cache.js";

Deno.test("admin cache retries IndexedDB after a temporary open failure", async () => {
  let openCalls = 0;
  const transaction: {
    onabort?: () => void;
    oncomplete?: () => void;
    onerror?: () => void;
    objectStore: () => { put: () => void };
  } = {
    objectStore: () => ({
      put: () => queueMicrotask(() => transaction.oncomplete?.()),
    }),
  };
  const database = { transaction: () => transaction };
  const indexedDB = {
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
  const cache = createAdminSnapshotCache({ indexedDB });

  assert.equal(await cache.write("scope", "key", { cached: true }), false);
  assert.equal(await cache.write("scope", "key", { cached: true }), true);
  assert.equal(openCalls, 2);
});

Deno.test("admin cache serializes invalidation behind a pending write", async () => {
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
          openCursor: () => {
            const request: {
              onerror?: () => void;
              onsuccess?: () => void;
              result?: null;
            } = {};
            queueMicrotask(() => {
              records.clear();
              request.result = null;
              request.onsuccess?.();
              queueMicrotask(() => transaction.oncomplete?.());
            });
            return request;
          },
        }),
      };
      return transaction;
    },
  };
  const indexedDB = {
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
    indexedDB,
    keyRange: { only: (scope: string) => scope },
  });

  const pendingWrite = cache.write("scope", "key", { cached: true });
  const pendingClear = cache.clear("scope");

  assert.equal(await pendingWrite, true);
  assert.equal(await pendingClear, true);
  assert.equal(records.size, 0);
});
