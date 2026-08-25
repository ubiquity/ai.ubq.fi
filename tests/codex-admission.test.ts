import assert from "node:assert/strict";
import {
  acquireCodexAdmission,
  CODEX_ADMISSION_ACCOUNT_LIMIT,
  CODEX_ADMISSION_LEASE_MS,
  CODEX_ADMISSION_LEASE_SAFETY_MARGIN_MS,
  CODEX_ADMISSION_RENEW_AFTER_MS,
  codexAdmissionCallerKey,
  codexAdmissionSlotKey,
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
    accountLimit?: number;
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
  for (let attempt = 0; attempt < 500; attempt += 1) {
    if (kv.entries.size === 0) return;
    await new Promise((resolve) => setTimeout(resolve, 2));
  }
  assert.fail(`timed out waiting for admission cleanup; ${kv.entries.size} records remain`);
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

Deno.test("Codex admission briefly rechecks a slot released within the bounded wait", async () => {
  const kv = new CountingKv();
  const first = await admission(kv, "21".padStart(64, "0"), "account-a", { accountLimit: 1 });
  assert.equal(first.kind, "acquired");
  if (first.kind !== "acquired") return;

  const release = new Promise<void>((resolve) => {
    setTimeout(() => {
      void releaseCodexAdmission(first.lease).then(() => resolve());
    }, 10);
  });
  const second = await admission(kv, "22".padStart(64, "0"), "account-a", { accountLimit: 1 });

  await release;
  assert.equal(second.kind, "acquired");
  if (second.kind === "acquired") await releaseCodexAdmission(second.lease);
});

Deno.test("Codex admission returns busy after its one bounded recheck expires", async () => {
  const kv = new CountingKv();
  const first = await admission(kv, "23".padStart(64, "0"), "account-a", { accountLimit: 1 });
  assert.equal(first.kind, "acquired");
  if (first.kind !== "acquired") return;

  const startedAtMs = performance.now();
  const second = await admission(kv, "24".padStart(64, "0"), "account-a", { accountLimit: 1 });
  assert.equal(second.kind, "account_busy");
  assert.ok(performance.now() - startedAtMs >= 40, "the foreground recheck wait must not be skipped");
  await releaseCodexAdmission(first.lease);
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

  assert.equal(await releaseCodexAdmission(first.lease), true);
  assert.equal((await admission(kv, caller, "account-b", { now: () => nowMs })).kind, "caller_busy");
  assert.equal(await releaseCodexAdmission(replacement.lease), true);
});

Deno.test("terminal cleanup retries a transient admission release read failure", async () => {
  const kv = new CountingKv();
  const caller = "10".padStart(64, "0");
  const first = await admission(kv, caller);
  assert.equal(first.kind, "acquired");
  if (first.kind !== "acquired") return;

  const originalGetMany = kv.getMany.bind(kv);
  let getManyCalls = 0;
  Object.defineProperty(kv, "getMany", {
    configurable: true,
    value: (<T extends readonly unknown[]>(
      keys: readonly Deno.KvKey[],
      options?: Readonly<{ consistency?: "strong" | "eventual" }>,
    ) => {
      getManyCalls += 1;
      if (getManyCalls === 1) return Promise.reject(new Error("transient release read failure"));
      return originalGetMany<T>(keys, options);
    }) as typeof kv.getMany,
  });

  const response = new Response("stream", { status: 200 });
  setCodexResponseAdmissionForTest(response, first.lease);
  await releaseCodexResponseProbe(response);

  assert.equal(getManyCalls, 2);
  assert.equal(kv.entries.size, 0);
  const reacquired = await admission(kv, caller);
  assert.equal(reacquired.kind, "acquired");
  if (reacquired.kind === "acquired") await releaseCodexAdmission(reacquired.lease);
});

Deno.test("terminal cleanup retries after admission release CAS exhaustion", async () => {
  const kv = new CountingKv();
  const caller = "11".padStart(64, "0");
  const first = await admission(kv, caller);
  assert.equal(first.kind, "acquired");
  if (first.kind !== "acquired") return;

  const originalAtomic = kv.atomic.bind(kv);
  let commitAttempts = 0;
  Object.defineProperty(kv, "atomic", {
    configurable: true,
    value: (): Deno.AtomicOperation => {
      const operation = originalAtomic();
      const originalCommit = operation.commit.bind(operation);
      Object.defineProperty(operation, "commit", {
        configurable: true,
        value: (): Promise<Deno.KvCommitResult | Deno.KvCommitError> => {
          commitAttempts += 1;
          if (commitAttempts <= 4) return Promise.resolve({ ok: false } as Deno.KvCommitError);
          return originalCommit();
        },
      });
      return operation;
    },
  });

  const response = new Response("stream", { status: 200 });
  setCodexResponseAdmissionForTest(response, first.lease);
  await releaseCodexResponseProbe(response);

  assert.equal(commitAttempts, 5);
  assert.equal(kv.entries.size, 0);
  const reacquired = await admission(kv, caller);
  assert.equal(reacquired.kind, "acquired");
  if (reacquired.kind === "acquired") await releaseCodexAdmission(reacquired.lease);
});

Deno.test("terminal cleanup keeps retrying in the background after all immediate releases fail", async () => {
  const kv = new CountingKv();
  const caller = "12".padStart(64, "0");
  const first = await admission(kv, caller);
  assert.equal(first.kind, "acquired");
  if (first.kind !== "acquired") return;

  const originalGetMany = kv.getMany.bind(kv);
  let getManyCalls = 0;
  Object.defineProperty(kv, "getMany", {
    configurable: true,
    value: (<T extends readonly unknown[]>(
      keys: readonly Deno.KvKey[],
      options?: Readonly<{ consistency?: "strong" | "eventual" }>,
    ) => {
      getManyCalls += 1;
      if (getManyCalls <= 3) return Promise.reject(new Error("release remains unavailable"));
      return originalGetMany<T>(keys, options);
    }) as typeof kv.getMany,
  });

  const response = new Response("stream", { status: 200 });
  setCodexResponseAdmissionForTest(response, first.lease);
  await releaseCodexResponseProbe(response);

  assert.equal(getManyCalls, 3, "only the bounded immediate release attempts may delay terminal delivery");
  assert.equal(kv.entries.size, 2, "the token-fenced lease must remain available to the background retry");
  await waitForEmptyKv(kv);
  assert.equal(getManyCalls, 4);
  const reacquired = await admission(kv, caller);
  assert.equal(reacquired.kind, "acquired");
  if (reacquired.kind === "acquired") await releaseCodexAdmission(reacquired.lease);
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

Deno.test("an ambiguous distributed acquire fails closed and compensates instead of using local admission", async () => {
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

Deno.test("unavailable distributed KV automatically uses a conservative isolate-local limit", async () => {
  resetCodexAdmissionForTest();
  const first = await admission(null, "5".padStart(64, "0"), "account-a");
  const second = await admission(null, "6".padStart(64, "0"), "account-a");
  assert.equal(first.kind, "acquired");
  assert.equal(second.kind, "account_busy");
  if (first.kind === "acquired") assert.equal(first.lease.backend, "local");
  if (first.kind === "acquired") await releaseCodexAdmission(first.lease);
  assert.equal((await admission(null, "6".padStart(64, "0"), "account-a")).kind, "acquired");
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

Deno.test("malformed caller admission state is CAS-cleaned and retried", async () => {
  const kv = new CountingKv();
  const caller = "7".padStart(64, "0");
  const callerKey = codexAdmissionCallerKey(caller);
  kv.seed(callerKey, { v: 999 });

  const decision = await admission(kv, caller);
  assert.equal(decision.kind, "acquired");
  const repair = kv.commands.find((command) => command.command === "atomic.commit" && command.atomicChecks === 1);
  assert.ok(repair);
  assert.deepEqual(repair.keys, [callerKey, callerKey]);
  assert.equal(
    (kv.entries.get(JSON.stringify(callerKey))?.value as { v?: number } | undefined)?.v,
    1,
  );
  if (decision.kind === "acquired") await releaseCodexAdmission(decision.lease);
});

Deno.test("malformed slot admission state is CAS-cleaned and retried", async () => {
  const kv = new CountingKv();
  const caller = "0".padStart(64, "0");
  const slotKey = codexAdmissionSlotKey("account-a", 0);
  kv.seed(slotKey, { v: 999 });

  const decision = await admission(kv, caller, "account-a", { accountLimit: 1 });
  assert.equal(decision.kind, "acquired");
  const repair = kv.commands.find((command) => command.command === "atomic.commit" && command.atomicChecks === 1);
  assert.ok(repair);
  assert.deepEqual(repair.keys, [slotKey, slotKey]);
  assert.equal(
    (kv.entries.get(JSON.stringify(slotKey))?.value as { v?: number } | undefined)?.v,
    1,
  );
  if (decision.kind === "acquired") await releaseCodexAdmission(decision.lease);
});

Deno.test("malformed admission repair fails closed when the exact key changes", async () => {
  const kv = new CountingKv();
  const caller = "8".padStart(64, "0");
  const callerKey = codexAdmissionCallerKey(caller);
  const replacement = {
    v: 1,
    token: "replacement-token",
    account_id_hash: "account-a",
    quota_class: "standard",
    caller_lane_hash: caller,
    slot: 0,
    acquired_at_ms: 1_000,
    expires_at_ms: 2_000,
  };
  kv.seed(callerKey, { v: 999 });

  const originalAtomic = kv.atomic.bind(kv);
  let commitCalls = 0;
  Object.defineProperty(kv, "atomic", {
    configurable: true,
    value: (): Deno.AtomicOperation => {
      const operation = originalAtomic();
      const originalCommit = operation.commit.bind(operation);
      Object.defineProperty(operation, "commit", {
        configurable: true,
        value: (): Promise<Deno.KvCommitResult | Deno.KvCommitError> => {
          commitCalls += 1;
          if (commitCalls === 1) kv.seed(callerKey, replacement);
          return originalCommit();
        },
      });
      return operation;
    },
  });

  const decision = await admission(kv, caller);
  assert.equal(decision.kind, "unavailable");
  assert.deepEqual(kv.entries.get(JSON.stringify(callerKey))?.value, replacement);
  assert.equal(kv.entries.size, 1);
});

Deno.test("malformed admission repair fails closed when its CAS acknowledgement times out", async () => {
  const kv = new CountingKv();
  const caller = "9".padStart(64, "0");
  const callerKey = codexAdmissionCallerKey(caller);
  const malformed = { v: 999 };
  kv.seed(callerKey, malformed);

  const originalAtomic = kv.atomic.bind(kv);
  Object.defineProperty(kv, "atomic", {
    configurable: true,
    value: (): Deno.AtomicOperation => {
      const operation = originalAtomic();
      Object.defineProperty(operation, "commit", {
        configurable: true,
        value: (): Promise<Deno.KvCommitResult | Deno.KvCommitError> => new Promise(() => {}),
      });
      return operation;
    },
  });

  const startedAtMs = performance.now();
  const decision = await admission(kv, caller, "account-a", { kvTimeoutMs: 5 });
  assert.equal(decision.kind, "unavailable");
  assert.ok(performance.now() - startedAtMs < 500, "malformed repair must stay within its deadline");
  assert.deepEqual(kv.entries.get(JSON.stringify(callerKey))?.value, malformed);
});

Deno.test("malformed admission repair fails closed when KV rejects the delete", async () => {
  const kv = new CountingKv();
  const caller = "a".padStart(64, "0");
  const callerKey = codexAdmissionCallerKey(caller);
  const malformed = { v: 999 };
  kv.seed(callerKey, malformed);
  Object.defineProperty(kv, "atomic", {
    configurable: true,
    value: (): Deno.AtomicOperation => {
      throw new Error("KV unavailable");
    },
  });

  const decision = await admission(kv, caller);
  assert.equal(decision.kind, "unavailable");
  assert.deepEqual(kv.entries.get(JSON.stringify(callerKey))?.value, malformed);
});
