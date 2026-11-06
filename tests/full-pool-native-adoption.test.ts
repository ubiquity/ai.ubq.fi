import assert from "node:assert/strict";
import { test } from "bun:test";
import { createHash } from "node:crypto";
import { openKv } from "@deno/kv";
import { executeFullPoolNativeAdoption, type FullPoolPartialReceipt } from "../src/codex/full-pool-native-adoption.ts";
import {
  executeOwnedPrimaryNativeAdoption,
  executeManagedNativeAdoption,
  managedAdoptionHash,
  planOwnedPrimaryNativeAdoption,
  type ManagedAdoptionPort,
  type ManagedAdoptionReceipt,
} from "../src/codex/managed-native-adoption.ts";
import { nativeCodexCredentialGeneration } from "../src/codex/native-auth.ts";
import { CODEX_AUTH_POOL_KV_KEY } from "../src/codex/auth.ts";
import { managedAdoptionKvPort } from "../tools/cloudflare/managed-native-adoption.ts";
import type { CodexAuthPoolState, CodexAuthState } from "../src/types.ts";

const now = Date.now();
const hash = (x: string): string => createHash("sha256").update(x).digest("hex");
const auth = (id: string, hours: number): CodexAuthState => ({
  account_id: id,
  access_token: `fixture.${Buffer.from(
    JSON.stringify({
      exp: Math.floor(now / 1000) + hours * 3600,
      "https://api.openai.com/auth": { chatgpt_account_id: id },
    })
  ).toString("base64url")}.signature`,
  refresh_token: `private-${id}-${hours}`,
  updated_at_ms: now - 1000,
});
const document = (id: string) => ({ auth_mode: "chatgpt", last_refresh: new Date(now).toISOString(), tokens: { ...auth(id, 3) } });
const primarySelection = { accountSha256: hash("primary"), slot: 1, home: "/fixture/primary" };
const secondarySelection = { accountSha256: hash("secondary"), slot: 2, home: "/fixture/secondary" };
const source = async (): Promise<CodexAuthPoolState> => {
  const primary = auth("primary", 1);
  return {
    updated_at_ms: now - 1000,
    accounts: [
      {
        ...primary,
        native_owner: {
          codex_home: primarySelection.home,
          generation_hash: await nativeCodexCredentialGeneration(primary),
          native_refreshed_at: new Date(now - 3_600_000).toISOString(),
        },
      },
      auth("secondary", 1),
    ],
  };
};

test("owned primary adopts only a strictly newer current generation and preserves secondary", async () => {
  const before = await source();
  const result = await planOwnedPrimaryNativeAdoption(before, document("primary"), primarySelection, now);
  assert.deepEqual(result.pool.accounts[1], before.accounts[1]);
  assert.deepEqual(
    result.pool.accounts.map((a) => a.account_id),
    ["primary", "secondary"]
  );
  assert.equal(result.pool.accounts[0].native_owner?.generation_hash, await nativeCodexCredentialGeneration(result.pool.accounts[0]));
  assert.equal(result.pool.accounts[0].native_owner.native_refreshed_at, document("primary").last_refresh);
  const later = await planOwnedPrimaryNativeAdoption(before, document("primary"), primarySelection, now + 60_000);
  assert.equal(result.generationSha256, later.generationSha256);
});

test("owned primary refuses missing/wrong owner, physical fence, identity, role and non-newer native data", async () => {
  const before = await source();
  const owner = before.accounts[0].native_owner;
  assert.ok(owner);
  for (const nextOwner of [undefined, { ...owner, codex_home: "/other" }, { ...owner, generation_hash: "f".repeat(64) }]) {
    const pool = { ...before, accounts: [{ ...before.accounts[0], native_owner: nextOwner }, before.accounts[1]] };
    await assert.rejects(planOwnedPrimaryNativeAdoption(pool, document("primary"), primarySelection, now), /owned_primary/);
  }
  await assert.rejects(planOwnedPrimaryNativeAdoption(before, document("other"), primarySelection, now), /native_document_invalid/);
  await assert.rejects(
    planOwnedPrimaryNativeAdoption(before, document("primary"), { ...primarySelection, accountSha256: hash("other") }, now),
    /fence_changed/
  );
  await assert.rejects(planOwnedPrimaryNativeAdoption(before, document("primary"), { ...primarySelection, slot: 2 }, now), /source_invalid/);
  const old = { ...document("primary"), tokens: auth("primary", 0) };
  await assert.rejects(planOwnedPrimaryNativeAdoption(before, old, primarySelection, now), /did not advance/);
  await assert.rejects(
    planOwnedPrimaryNativeAdoption(before, { ...document("primary"), auth_mode: "chatgptAuthTokens" }, primarySelection, now),
    /managed_document/
  );
});

const setup = async () => {
  const kv = await openKv(undefined, { implementation: "in-memory" });
  const original = await source();
  await kv.set(CODEX_AUTH_POOL_KV_KEY, original);
  const otherKeys = [
    ["uos_ai", "codex_active_account", "v1"],
    ["ubq_ai", "codex_models"],
    ["fixture", "paid_limit"],
  ];
  for (const key of otherKeys) await kv.set(key, { unchanged: true, generation: 7, limit: 3 });
  const events: string[] = [];
  const partials: FullPoolPartialReceipt[] = [];
  let commits = 0;
  const native = [document("primary"), document("secondary")];
  const nativeHashes = [hash("primary-bytes"), hash("secondary-bytes")];
  const port = (index: number): ManagedAdoptionPort =>
    managedAdoptionKvPort(kv, {
      assertOwned: () => Promise.resolve(),
      native: () => Promise.resolve({ raw: native[index], sha256: nativeHashes[index] }),
      proveNative: () => {
        events.push(`passive-${index}`);
        return Promise.resolve();
      },
      backup: () => {
        events.push(`backup-${index}`);
        return Promise.resolve();
      },
      intent: () => {
        events.push(`intent-${index}`);
        return Promise.resolve();
      },
    });
  const a = port(0),
    b = port(1);
  const commitA = a.commit,
    commitB = b.commit;
  const primaryPort = {
    ...a,
    commit: async (...args: Parameters<typeof commitA>) => {
      commits++;
      events.push("commit-a");
      return await commitA(...args);
    },
  };
  const secondaryPort = {
    ...b,
    commit: async (...args: Parameters<typeof commitB>) => {
      commits++;
      events.push("commit-b");
      return await commitB(...args);
    },
  };
  const row = await a.read();
  const expected = { poolVersionstamp: row.versionstamp, poolSha256: managedAdoptionHash(row.pool), nativeSha256: nativeHashes[0] };
  return { kv, original, otherKeys, events, partials, nativeHashes, primaryPort, secondaryPort, expected, commits: () => commits };
};

test("two single CAS operations use accepted A readback for B and leave every other key unchanged", async () => {
  const f = await setup();
  let acceptedA: ManagedAdoptionReceipt | null = null;
  try {
    const result = await executeFullPoolNativeAdoption(
      {
        primary: async () => {
          acceptedA = await executeOwnedPrimaryNativeAdoption(f.primaryPort, primarySelection, f.expected, now);
          return acceptedA;
        },
        secondary: async (expected) => {
          assert.ok(acceptedA);
          assert.equal(expected.poolVersionstamp, acceptedA.current_versionstamp);
          assert.equal(expected.poolSha256, acceptedA.after_sha256);
          assert.notEqual(expected.poolVersionstamp, f.expected.poolVersionstamp);
          return await executeManagedNativeAdoption(f.secondaryPort, secondarySelection, expected, now);
        },
        verifyBoth: () => {
          f.events.push("both-fences");
          return Promise.resolve();
        },
        recordPartial: (x) => {
          f.partials.push(x);
          return Promise.resolve();
        },
      },
      f.nativeHashes[1]
    );
    assert.equal(result.status, "completed");
    assert.equal(f.commits(), 2);
    assert.equal(f.partials.length, 0);
    const pool = (await f.kv.get<CodexAuthPoolState>(CODEX_AUTH_POOL_KV_KEY)).value;
    assert.ok(pool);
    assert.equal(pool.accounts[0].native_owner?.codex_home, primarySelection.home);
    assert.equal(pool.accounts[1].native_owner?.codex_home, secondarySelection.home);
    for (const key of f.otherKeys) assert.deepEqual((await f.kv.get(key)).value, { unchanged: true, generation: 7, limit: 3 });
    assert.ok(f.events.indexOf("backup-0") < f.events.indexOf("commit-a"));
    assert.ok(f.events.indexOf("backup-1") < f.events.indexOf("commit-b"));
    assert.ok(!JSON.stringify(result).includes("private-primary"));
  } finally {
    f.kv.close();
  }
});

test("B failure retains accepted A/current custody and records partial recovery without retry or rollback", async () => {
  const f = await setup();
  let primaryPool: unknown;
  let attemptsB = 0;
  try {
    const failedB = {
      ...f.secondaryPort,
      commit: () => {
        attemptsB++;
        return Promise.resolve(false);
      },
    };
    await assert.rejects(
      executeFullPoolNativeAdoption(
        {
          primary: async () => {
            const receipt = await executeOwnedPrimaryNativeAdoption(f.primaryPort, primarySelection, f.expected, now);
            primaryPool = (await f.kv.get(CODEX_AUTH_POOL_KV_KEY)).value;
            return receipt;
          },
          secondary: (expected) => executeManagedNativeAdoption(failedB, secondarySelection, expected, now),
          verifyBoth: () => {
            throw Error("must not finalize");
          },
          recordPartial: (x) => {
            f.partials.push(x);
            return Promise.resolve();
          },
        },
        f.nativeHashes[1]
      ),
      /full_pool_partial_recovery_required/
    );
    assert.deepEqual((await f.kv.get(CODEX_AUTH_POOL_KV_KEY)).value, primaryPool);
    assert.equal(f.commits(), 1);
    assert.equal(attemptsB, 1);
    assert.equal(f.partials[0].primary_accepted, true);
    assert.equal(f.partials[0].phase, "secondary");
    assert.equal(f.partials[0].stale_rollback_permitted, false);
  } finally {
    f.kv.close();
  }
});

test("primary home race and unaccepted/unknown A prevent B entirely", async () => {
  const f = await setup();
  let secondaryCalls = 0;
  try {
    const raced = {
      ...f.primaryPort,
      backup: () => {
        f.nativeHashes[0] = hash("rotated");
        return Promise.resolve();
      },
    };
    await assert.rejects(
      executeFullPoolNativeAdoption(
        {
          primary: () => executeOwnedPrimaryNativeAdoption(raced, primarySelection, f.expected, now),
          secondary: () => {
            secondaryCalls++;
            throw Error("must not run");
          },
          verifyBoth: () => Promise.resolve(),
          recordPartial: (x) => {
            f.partials.push(x);
            return Promise.resolve();
          },
        },
        f.nativeHashes[1]
      ),
      /partial_recovery/
    );
    assert.equal(secondaryCalls, 0);
    assert.equal(f.commits(), 0);
    assert.deepEqual((await f.kv.get(CODEX_AUTH_POOL_KV_KEY)).value, f.original);
  } finally {
    f.kv.close();
  }
});

test("unknown B delivery is reconciled without retry and final-fence failure retains both accepted writes", async () => {
  for (const state of ["unknown-applied", "unknown-unapplied", "final-fence-failed"] as const) {
    const f = await setup();
    let callsB = 0;
    try {
      const commit = f.secondaryPort.commit;
      const port = {
        ...f.secondaryPort,
        commit: async (...args: Parameters<typeof commit>) => {
          callsB++;
          if (state !== "unknown-unapplied") await commit(...args);
          if (state !== "final-fence-failed") throw Error("synthetic lost reply");
          return true;
        },
      };
      const operation = executeFullPoolNativeAdoption(
        {
          primary: () => executeOwnedPrimaryNativeAdoption(f.primaryPort, primarySelection, f.expected, now),
          secondary: (expected) => executeManagedNativeAdoption(port, secondarySelection, expected, now),
          verifyBoth: () => (state === "final-fence-failed" ? Promise.reject(new Error("synthetic final fence")) : Promise.resolve()),
          recordPartial: (receipt) => {
            f.partials.push(receipt);
            return Promise.resolve();
          },
        },
        f.nativeHashes[1]
      );
      if (state === "unknown-applied") {
        assert.equal((await operation).secondary.outcome, "reconciled");
        assert.equal(f.partials.length, 0);
      } else {
        await assert.rejects(operation, /full_pool_partial_recovery_required/);
        assert.equal(f.partials[0].primary_accepted, true);
        assert.equal(f.partials[0].stale_rollback_permitted, false);
      }
      const pool = (await f.kv.get<CodexAuthPoolState>(CODEX_AUTH_POOL_KV_KEY)).value;
      assert.ok(pool);
      assert.equal(pool.accounts[0].native_owner?.codex_home, primarySelection.home);
      assert.equal(callsB, 1);
      assert.equal(f.commits(), state === "unknown-unapplied" ? 1 : 2);
    } finally {
      f.kv.close();
    }
  }
});
