import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { test } from "bun:test";
import {
  executeManagedNativeAdoption,
  managedAdoptionHash,
  planManagedNativeAdoption,
  type ManagedAdoptionExpectation,
  type ManagedAdoptionPort,
  type ManagedAdoptionSelection,
  type ManagedAdoptionSnapshot,
} from "../src/codex/managed-native-adoption.ts";
import type { CodexAuthPoolState } from "../src/types.ts";

const hash = (value: string): string => createHash("sha256").update(value).digest("hex");
const NOW = Date.now();
const jwt = (accountId: string, extraMs: number): string =>
  `fixture.${Buffer.from(
    JSON.stringify({
      exp: Math.floor((NOW + extraMs) / 1000),
      "https://api.openai.com/auth": { chatgpt_account_id: accountId },
      email: "fixture@example.invalid",
    })
  ).toString("base64url")}.fixture`;
const source = (): CodexAuthPoolState => ({
  updated_at_ms: NOW - 1000,
  accounts: [
    { account_id: "sibling", access_token: jwt("sibling", 3_600_000), refresh_token: "private-sibling-refresh", updated_at_ms: NOW - 1000 },
    { account_id: "selected", access_token: jwt("selected", 3_600_000), refresh_token: "private-old-refresh", updated_at_ms: NOW - 1000 },
  ],
});
const document = () => ({
  auth_mode: "chatgpt",
  last_refresh: new Date(NOW).toISOString(),
  tokens: {
    account_id: "selected",
    access_token: jwt("selected", 7_200_000),
    refresh_token: "private-new-refresh",
    id_token: jwt("selected", 7_200_000),
  },
});
const selection: ManagedAdoptionSelection = { accountSha256: hash("selected"), slot: 2, home: "/fixture/native-home" };

test("managed adoption uses current newer credentials and preserves sibling and order", async () => {
  const before = source();
  const raw = document();
  const plan = await planManagedNativeAdoption(before, raw, selection, NOW);
  assert.deepEqual(plan.pool.accounts[0], before.accounts[0]);
  assert.deepEqual(
    plan.pool.accounts.map((a) => a.account_id),
    before.accounts.map((a) => a.account_id)
  );
  assert.equal(plan.pool.accounts[1].access_token, raw.tokens.access_token);
  assert.equal(plan.pool.accounts[1].refresh_token, raw.tokens.refresh_token);
  assert.equal(plan.pool.accounts[1].native_owner?.codex_home, selection.home);
  assert.equal(plan.pool.accounts[1].native_owner.generation_hash, plan.generationSha256);
  assert.equal(plan.pool.updated_at_ms, NOW);
  assert.equal(plan.beforeSha256, managedAdoptionHash(before));
  assert.equal(plan.afterSha256, managedAdoptionHash(plan.pool));
  assert.deepEqual(before, source());
});

test("planner refuses a changed identity, owner, home, external mode or stale access", async () => {
  await assert.rejects(planManagedNativeAdoption(source(), document(), { ...selection, accountSha256: hash("other") }, NOW), /account_or_owner_changed/);
  await assert.rejects(planManagedNativeAdoption(source(), document(), { ...selection, home: "/fixture/../other" }, NOW), /selection_invalid/);
  await assert.rejects(planManagedNativeAdoption(source(), { ...document(), auth_mode: "chatgptAuthTokens" }, selection, NOW), /managed_document_required/);
  const previous = source();
  const owned = {
    ...previous,
    accounts: [
      previous.accounts[0],
      {
        ...previous.accounts[1],
        native_owner: {
          codex_home: selection.home,
          generation_hash: "a".repeat(64),
        },
      },
    ],
  };
  await assert.rejects(planManagedNativeAdoption(owned, document(), selection, NOW), /account_or_owner_changed/);
  for (const extra of [30_000, 3_600_000]) {
    const raw = document();
    raw.tokens.access_token = jwt("selected", extra);
    await assert.rejects(planManagedNativeAdoption(source(), raw, selection, NOW), /did not advance/);
  }
  const other = document();
  other.tokens.account_id = "other";
  await assert.rejects(planManagedNativeAdoption(source(), other, selection, NOW), /native_document_invalid/);
});

test("identical native bytes keep the physical generation and actual refresh time across observations", async () => {
  const bytes = JSON.stringify(document());
  const first = await planManagedNativeAdoption(source(), JSON.parse(bytes), selection, NOW);
  const later = await planManagedNativeAdoption(source(), JSON.parse(bytes), selection, NOW + 60_000);
  assert.equal(first.generationSha256, later.generationSha256);
  assert.equal(first.pool.accounts[1].native_owner?.generation_hash, later.pool.accounts[1].native_owner?.generation_hash);
  assert.equal(first.pool.accounts[1].native_owner?.native_refreshed_at, document().last_refresh);
  assert.equal(later.pool.accounts[1].native_owner?.native_refreshed_at, document().last_refresh);
  assert.equal(first.pool.accounts[1].updated_at_ms, NOW);
  assert.equal(later.pool.accounts[1].updated_at_ms, NOW + 60_000);
  assert.notEqual(first.afterSha256, later.afterSha256);
});

class MemoryAdoption {
  snapshot: ManagedAdoptionSnapshot = { pool: source(), versionstamp: "00000000000000010000" };
  raw = document();
  nativeHash = hash("original-home-bytes");
  events: string[] = [];
  assertions = 0;
  commits = 0;
  failure: "none" | "conflict" | "unknown-applied" | "unknown-unapplied" | "diverged" = "none";
  stopped = true;
  peer = true;
  onBackup?: () => void;
  onCommit?: () => void;
  expected: ManagedAdoptionExpectation = {
    poolVersionstamp: this.snapshot.versionstamp,
    poolSha256: managedAdoptionHash(this.snapshot.pool),
    nativeSha256: this.nativeHash,
  };
  port: ManagedAdoptionPort = {
    assertOwned: () => {
      this.events.push("owned");
      this.assertions++;
      if (!this.stopped) return Promise.reject(new Error("writer resumed"));
      return Promise.resolve();
    },
    read: () => {
      this.events.push("read");
      return Promise.resolve(this.snapshot);
    },
    native: () => {
      this.events.push("native");
      return Promise.resolve({ raw: this.raw, sha256: this.nativeHash });
    },
    proveNative: () => {
      this.events.push("peer");
      return this.peer ? Promise.resolve() : Promise.reject(new Error("peer changed"));
    },
    backup: () => {
      this.events.push("backup");
      this.onBackup?.();
      return Promise.resolve();
    },
    intent: () => {
      this.events.push("intent");
      return Promise.resolve();
    },
    commit: (stamp, pool) => {
      this.events.push("commit");
      this.commits++;
      assert.equal(stamp, this.expected.poolVersionstamp);
      this.onCommit?.();
      if (this.failure === "conflict") return Promise.resolve(false);
      if (this.failure === "unknown-unapplied") return Promise.reject(new Error("lost acknowledgement"));
      this.snapshot = { pool: this.failure === "diverged" ? { ...pool, updated_at_ms: NOW + 1 } : pool, versionstamp: "00000000000000020000" };
      return this.failure === "none" ? Promise.resolve(true) : Promise.reject(new Error("lost acknowledgement"));
    },
  };
  run() {
    return executeManagedNativeAdoption(this.port, selection, this.expected, NOW);
  }
}

test("fixed-key transaction backs up and journals before one CAS with secret-free receipt", async () => {
  const fixture = new MemoryAdoption();
  const result = await fixture.run();
  assert.equal(result.outcome, "committed");
  assert.equal(fixture.commits, 1);
  assert.ok(fixture.events.indexOf("backup") < fixture.events.indexOf("intent"));
  assert.ok(fixture.events.indexOf("intent") < fixture.events.indexOf("commit"));
  assert.deepEqual((fixture.snapshot.pool as CodexAuthPoolState).accounts[0], source().accounts[0]);
  const output = JSON.stringify(result);
  for (const secret of ["private-new-refresh", "private-old-refresh", "private-sibling-refresh", document().tokens.access_token])
    assert.ok(!output.includes(secret));
});

test("source, stopped-writer, native-file and peer races refuse before any effect", async () => {
  const stale = new MemoryAdoption();
  stale.expected = { ...stale.expected, poolVersionstamp: "00000000000000030000" };
  await assert.rejects(stale.run(), /source_changed/);
  assert.equal(stale.commits, 0);
  const writer = new MemoryAdoption();
  writer.stopped = false;
  await assert.rejects(writer.run(), /writer resumed/);
  assert.equal(writer.commits, 0);
  const peer = new MemoryAdoption();
  peer.peer = false;
  await assert.rejects(peer.run(), /peer changed/);
  assert.equal(peer.commits, 0);
  const home = new MemoryAdoption();
  home.onBackup = () => {
    home.nativeHash = hash("rotated-home");
  };
  await assert.rejects(home.run(), /home_changed/);
  assert.equal(home.commits, 0);
  const resume = new MemoryAdoption();
  resume.onBackup = () => {
    resume.stopped = false;
  };
  await assert.rejects(resume.run(), /writer resumed/);
  assert.equal(resume.commits, 0);
});

test("unknown delivery reconciles once without another mutation; conflicts and divergent states stay closed", async () => {
  for (const [failure, outcome] of [
    ["unknown-applied", "reconciled"],
    ["unknown-unapplied", "not_committed"],
  ] as const) {
    const fixture = new MemoryAdoption();
    fixture.failure = failure;
    assert.equal((await fixture.run()).outcome, outcome);
    assert.equal(fixture.commits, 1);
  }
  const conflict = new MemoryAdoption();
  conflict.failure = "conflict";
  await assert.rejects(conflict.run(), /cas_conflict/);
  assert.equal(conflict.commits, 1);
  const diverged = new MemoryAdoption();
  diverged.failure = "diverged";
  await assert.rejects(diverged.run(), /commit_outcome_diverged/);
  assert.equal(diverged.commits, 1);
  const changed = new MemoryAdoption();
  changed.onCommit = () => {
    changed.nativeHash = hash("rotated-after-commit");
  };
  await assert.rejects(changed.run(), /home_changed_after_commit/);
  assert.equal(changed.commits, 1);
});
