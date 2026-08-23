import assert from "node:assert/strict";
import {
  acquireCodexAdmission,
  CODEX_ADMISSION_ACCOUNT_LIMIT,
  CODEX_ADMISSION_LEASE_MS,
  CODEX_ADMISSION_LEASE_SAFETY_MARGIN_MS,
  CODEX_ADMISSION_RENEW_AFTER_MS,
  codexAdmissionCallerKey,
  deriveCodexAdmissionCallerLaneHash,
  releaseCodexAdmission,
  renewCodexAdmission,
  resetCodexAdmissionForTest,
} from "../src/codex_admission.ts";
import { STREAM_INACTIVITY_DEADLINE_MS } from "../src/inference_deadline.ts";
import {
  releaseCodexResponseProbe,
  renewCodexResponseAdmission,
  setCodexResponseAdmissionForTest,
} from "../src/codex.ts";
import { CountingKv } from "./helpers/counting_kv.ts";

const admission = (
  kv: CountingKv | null,
  callerLaneHash: string,
  accountIdHash = "account-a",
  dependencies: Readonly<{
    now?: () => number;
    leaseMs?: number;
    kvTimeoutMs?: number;
    newToken?: () => string;
    allowLocalFallback?: boolean;
  }> = {},
) =>
  acquireCodexAdmission(
    {
      accountIdHash,
      quotaClass: "standard",
      callerLaneHash,
    },
    { kv: kv as Deno.Kv | null, ...dependencies },
  );

const delayAtomicAcknowledgements = (kv: CountingKv, delayMs: number): void => {
  const originalAtomic = kv.atomic.bind(kv);
  Object.defineProperty(kv, "atomic", {
    configurable: true,
    value: (): Deno.AtomicOperation => {
      const operation = originalAtomic();
      const originalCommit = operation.commit.bind(operation);
      Object.defineProperty(operation, "commit", {
        configurable: true,
        value: async () => {
          const result = await originalCommit();
          await new Promise((resolve) => setTimeout(resolve, delayMs));
          return result;
        },
      });
      return operation;
    },
  });
};

const waitForEmptyKv = async (kv: CountingKv): Promise<void> => {
  for (let attempt = 0; attempt < 100; attempt += 1) {
    if (kv.entries.size === 0) return;
    await new Promise((resolve) => setTimeout(resolve, 2));
  }
  assert.fail(`timed out waiting for late-commit compensation; ${kv.entries.size} records remain`);
};

Deno.test("Codex admission scopes child thread lanes by authenticated principal", async () => {
  const first = await deriveCodexAdmissionCallerLaneHash(
    "principal-a",
    { thread_id: "child-one", session_id: "shared-session" },
    "shared-cache",
  );
  const same = await deriveCodexAdmissionCallerLaneHash(
    "principal-a",
    { thread_id: "child-one", session_id: "different-session" },
    "different-cache",
  );
  const sibling = await deriveCodexAdmissionCallerLaneHash(
    "principal-a",
    { thread_id: "child-two", session_id: "shared-session" },
    "shared-cache",
  );
  const otherPrincipal = await deriveCodexAdmissionCallerLaneHash(
    "principal-b",
    { thread_id: "child-one", session_id: "shared-session" },
    "shared-cache",
  );

  assert.equal(first, same);
  assert.notEqual(first, sibling);
  assert.notEqual(first, otherPrincipal);
  assert.match(first, /^[a-f0-9]{64}$/);
  assert.equal(first.includes("child-one"), false);
});

Deno.test("Codex admission caps a 100-agent burst per physical account", async () => {
  const kv = new CountingKv();
  const decisions = await Promise.all(
    Array.from({ length: 100 }, (_, index) => admission(kv, index.toString(16).padStart(64, "0"))),
  );
  const acquired = decisions.filter((decision) => decision.kind === "acquired");

  assert.equal(acquired.length, CODEX_ADMISSION_ACCOUNT_LIMIT);
  assert.equal(decisions.filter((decision) => decision.kind === "account_busy").length, 96);
  assert.equal(
    new Set(acquired.map((decision) => decision.kind === "acquired" ? decision.lease.slot : -1)).size,
    CODEX_ADMISSION_ACCOUNT_LIMIT,
  );

  await Promise.all(
    acquired.map((decision) => decision.kind === "acquired" ? releaseCodexAdmission(decision.lease) : false),
  );
});

Deno.test("mixed quota classes share one physical account ceiling", async () => {
  const kv = new CountingKv();
  const quotaClasses = ["standard", "spark", "gpt_oss_120b", "unknown"] as const;
  const decisions = await Promise.all(
    Array.from({ length: 100 }, (_, index) =>
      acquireCodexAdmission(
        {
          accountIdHash: "account-a",
          quotaClass: quotaClasses[index % quotaClasses.length]!,
          callerLaneHash: index.toString(16).padStart(64, "0"),
        },
        { kv: kv as unknown as Deno.Kv },
      )),
  );
  const acquired = decisions.filter((decision) => decision.kind === "acquired");
  assert.equal(acquired.length, CODEX_ADMISSION_ACCOUNT_LIMIT);
  assert.equal(decisions.filter((decision) => decision.kind === "account_busy").length, 96);
  await Promise.all(
    acquired.map((decision) => decision.kind === "acquired" ? releaseCodexAdmission(decision.lease) : false),
  );
});

Deno.test("one Codex caller cannot occupy two subscription accounts", async () => {
  const kv = new CountingKv();
  const first = await admission(kv, "1".padStart(64, "0"), "account-a");
  const second = await admission(kv, "1".padStart(64, "0"), "account-b");

  assert.equal(first.kind, "acquired");
  assert.equal(second.kind, "caller_busy");
  if (first.kind === "acquired") await releaseCodexAdmission(first.lease);
});

Deno.test("Codex admission release is token-fenced against replacement leases", async () => {
  const kv = new CountingKv();
  let nowMs = 1_000;
  const caller = "2".padStart(64, "0");
  const first = await admission(kv, caller, "account-a", {
    now: () => nowMs,
    leaseMs: 100,
    newToken: () => "first-token",
  });
  assert.equal(first.kind, "acquired");

  nowMs += 101;
  await kv.delete(first.lease.callerKey);
  await kv.delete(first.lease.slotKey);
  const replacement = await admission(kv, caller, "account-a", {
    now: () => nowMs,
    leaseMs: 100,
    newToken: () => "replacement-token",
  });
  assert.equal(replacement.kind, "acquired");
  if (first.kind !== "acquired" || replacement.kind !== "acquired") return;

  assert.equal(await releaseCodexAdmission(first.lease), false);
  assert.equal((await admission(kv, caller, "account-b", { now: () => nowMs })).kind, "caller_busy");
  assert.equal(await releaseCodexAdmission(replacement.lease), true);
});

Deno.test("Codex admission renews active work and abandoned leases expire", async () => {
  const kv = new CountingKv();
  let nowMs = 5_000;
  const caller = "3".padStart(64, "0");
  const first = await admission(kv, caller, "account-a", {
    now: () => nowMs,
    leaseMs: 100,
    newToken: () => "renew-token",
  });
  assert.equal(first.kind, "acquired");
  if (first.kind !== "acquired") return;

  nowMs += 50;
  const renewed = await renewCodexAdmission(first.lease, { now: () => nowMs, leaseMs: 100 });
  assert.ok(renewed);
  assert.equal(renewed.expiresAtMs, nowMs + 100);

  nowMs += 70;
  assert.equal((await admission(kv, caller, "account-b", { now: () => nowMs })).kind, "caller_busy");
  nowMs += 31;
  await kv.delete(renewed.callerKey);
  await kv.delete(renewed.slotKey);
  const reclaimed = await admission(kv, caller, "account-b", { now: () => nowMs });
  assert.equal(reclaimed.kind, "acquired");
  if (reclaimed.kind === "acquired") await releaseCodexAdmission(reclaimed.lease);
});

Deno.test("distributed admission waits for server TTL before replacing a present record", async () => {
  const kv = new CountingKv();
  let nowMs = 10_000;
  const caller = "a".padStart(64, "0");
  const first = await admission(kv, caller, "account-a", {
    now: () => nowMs,
    leaseMs: 25,
  });
  assert.equal(first.kind, "acquired");
  if (first.kind !== "acquired") return;

  nowMs += 100;
  assert.equal((await admission(kv, caller, "account-a", { now: () => nowMs })).kind, "caller_busy");
  await kv.delete(first.lease.callerKey);
  await kv.delete(first.lease.slotKey);
  const afterServerExpiry = await admission(kv, caller, "account-a", { now: () => nowMs });
  assert.equal(afterServerExpiry.kind, "acquired");
  if (afterServerExpiry.kind === "acquired") await releaseCodexAdmission(afterServerExpiry.lease);
});

Deno.test("a stalled admission read does not block an unrelated account", async () => {
  const kv = new CountingKv();
  const stalledCaller = "b".padStart(64, "0");
  const healthyCaller = "c".padStart(64, "0");
  const stalledKey = JSON.stringify(codexAdmissionCallerKey(stalledCaller));
  const originalGet = kv.get.bind(kv);
  let releaseRead = (): void => {};
  const readGate = new Promise<void>((resolve) => {
    releaseRead = resolve;
  });
  let observeStall = (): void => {};
  const stalled = new Promise<void>((resolve) => {
    observeStall = resolve;
  });
  Object.defineProperty(kv, "get", {
    configurable: true,
    value: <T>(key: Deno.KvKey, options?: Readonly<{ consistency?: "strong" | "eventual" }>) => {
      if (JSON.stringify(key) !== stalledKey) return originalGet<T>(key, options);
      observeStall();
      return readGate.then(() => originalGet<T>(key, options));
    },
  });

  const blocked = admission(kv, stalledCaller, "account-a", { kvTimeoutMs: 500 });
  await stalled;
  const healthy = await admission(kv, healthyCaller, "account-b", { kvTimeoutMs: 100 });
  assert.equal(healthy.kind, "acquired");
  if (healthy.kind === "acquired") await releaseCodexAdmission(healthy.lease);
  releaseRead();
  const recovered = await blocked;
  assert.equal(recovered.kind, "acquired");
  if (recovered.kind === "acquired") await releaseCodexAdmission(recovered.lease);
});

Deno.test("a late successful acquire commit is compensated after its deadline", async () => {
  const kv = new CountingKv();
  delayAtomicAcknowledgements(kv, 30);
  const decision = await admission(kv, "f".padStart(64, "0"), "account-a", { kvTimeoutMs: 1 });

  assert.equal(decision.kind, "unavailable");
  assert.equal(kv.entries.size, 2, "the delayed acknowledgement must reproduce a committed unknown outcome");
  await waitForEmptyKv(kv);
});

Deno.test("a late successful renewal commit cannot extend ghost capacity", async () => {
  const kv = new CountingKv();
  const nowMs = Date.now();
  const decision = await admission(kv, "0f".padStart(64, "0"), "account-a", {
    now: () => nowMs - CODEX_ADMISSION_RENEW_AFTER_MS - 1,
  });
  assert.equal(decision.kind, "acquired");
  if (decision.kind !== "acquired") return;
  delayAtomicAcknowledgements(kv, 30);

  assert.equal(await renewCodexAdmission(decision.lease, { kvTimeoutMs: 1 }), null);
  assert.equal(kv.entries.size, 2, "the delayed renewal acknowledgement must extend the existing records first");
  await waitForEmptyKv(kv);
});

Deno.test("Codex admission fails closed without distributed KV", async () => {
  resetCodexAdmissionForTest();
  assert.equal((await admission(null, "4".padStart(64, "0"))).kind, "unavailable");
  resetCodexAdmissionForTest();
});

Deno.test("an explicit local test seam uses a conservative isolate limit", async () => {
  resetCodexAdmissionForTest();
  const local = { allowLocalFallback: true } as const;
  const first = await admission(null, "5".padStart(64, "0"), "account-a", local);
  const second = await admission(null, "6".padStart(64, "0"), "account-a", local);
  assert.equal(first.kind, "acquired");
  assert.equal(second.kind, "account_busy");
  if (first.kind === "acquired") await releaseCodexAdmission(first.lease);
  assert.equal((await admission(null, "6".padStart(64, "0"), "account-a", local)).kind, "acquired");
  resetCodexAdmissionForTest();
});

Deno.test("the lease safely covers renewal threshold plus legal stream inactivity", () => {
  assert.ok(
    CODEX_ADMISSION_LEASE_MS >=
      CODEX_ADMISSION_RENEW_AFTER_MS + STREAM_INACTIVITY_DEADLINE_MS +
        CODEX_ADMISSION_LEASE_SAFETY_MARGIN_MS,
  );
});

Deno.test("concurrent response activity coalesces into one lease renewal", async () => {
  const kv = new CountingKv();
  const nowMs = Date.now();
  const decision = await admission(kv, "8".padStart(64, "0"), "account-a", {
    now: () => nowMs - CODEX_ADMISSION_LEASE_MS + 10_000,
  });
  assert.equal(decision.kind, "acquired");
  if (decision.kind !== "acquired") return;
  const response = new Response("stream", { status: 200 });
  const signal = setCodexResponseAdmissionForTest(response, decision.lease);
  const committedBefore =
    kv.commands.filter((command) => command.command === "atomic.commit" && command.atomicResult === "committed").length;

  assert.deepEqual(
    await Promise.all(Array.from({ length: 100 }, () => renewCodexResponseAdmission(response))),
    Array.from({ length: 100 }, () => true),
  );
  const committedAfter =
    kv.commands.filter((command) => command.command === "atomic.commit" && command.atomicResult === "committed").length;
  assert.equal(committedAfter - committedBefore, 1);
  assert.equal(signal.aborted, false);
  await releaseCodexResponseProbe(response);
});

Deno.test("a failed due renewal aborts its provider request immediately", async () => {
  const kv = new CountingKv();
  const nowMs = Date.now();
  const decision = await admission(kv, "9".padStart(64, "0"), "account-a", {
    now: () => nowMs - CODEX_ADMISSION_RENEW_AFTER_MS - 1,
  });
  assert.equal(decision.kind, "acquired");
  if (decision.kind !== "acquired") return;
  kv.getMany = () => Promise.reject(new Error("renewal unavailable"));
  const response = new Response("stream", { status: 200 });
  const signal = setCodexResponseAdmissionForTest(response, decision.lease);

  assert.equal(await renewCodexResponseAdmission(response), false);
  assert.equal(signal.aborted, true);
  assert.equal(signal.reason?.name, "CodexAdmissionLeaseError");
});

Deno.test("the response admission watchdog reschedules only after successful renewal", async () => {
  const kv = new CountingKv();
  const decision = await admission(kv, "d".padStart(64, "0"), "account-a", { leaseMs: 40 });
  assert.equal(decision.kind, "acquired");
  if (decision.kind !== "acquired") return;
  const response = new Response("stream", { status: 200 });
  const signal = setCodexResponseAdmissionForTest(response, decision.lease, 5);

  assert.equal(await renewCodexResponseAdmission(response), true);
  await new Promise((resolve) => setTimeout(resolve, 50));
  assert.equal(signal.aborted, false);
  await releaseCodexResponseProbe(response);
});

Deno.test("the response admission watchdog aborts before an unrenewed lease expires", async () => {
  const kv = new CountingKv();
  const decision = await admission(kv, "e".padStart(64, "0"), "account-a", { leaseMs: 40 });
  assert.equal(decision.kind, "acquired");
  if (decision.kind !== "acquired") return;
  const response = new Response("stream", { status: 200 });
  const signal = setCodexResponseAdmissionForTest(response, decision.lease, 5);

  await new Promise((resolve) => setTimeout(resolve, 50));
  assert.equal(signal.aborted, true);
  assert.equal(signal.reason?.name, "CodexAdmissionLeaseError");
  await releaseCodexResponseProbe(response);
});

Deno.test("malformed distributed admission state fails closed", async () => {
  const kv = new CountingKv();
  const caller = "7".padStart(64, "0");
  kv.seed(codexAdmissionCallerKey(caller), { v: 999 });
  assert.equal((await admission(kv, caller)).kind, "unavailable");
});
