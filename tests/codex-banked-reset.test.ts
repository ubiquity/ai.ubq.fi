import assert from "node:assert/strict";
import {
  attemptCodexBankedReset,
  CODEX_BANKED_RESET_INVENTORY_MAX_AGE_MS,
  CODEX_BANKED_RESET_LEASE_MS,
  type CodexBankedResetCandidate,
  type CodexBankedResetConfig,
  type CodexBankedResetDependencies,
  type CodexBankedResetFence,
  type CodexBankedResetTelemetry,
  type CodexBankedResetTelemetryFields,
  codexResetGlobalDailyKey,
  codexResetRedemptionKey,
  parseCodexBankedResetConfig,
  parseCodexResetRedemptionRecord,
  reconcileCodexBankedReset,
} from "../src/codex_banked_reset.ts";
import {
  type CodexUsageResetProvider,
  type CodexUsageResetProviderContract,
  type LookupRedeemResetInput,
  providerSupportsLiveRedemption,
  providerSupportsResetType,
  type RedeemResetInput,
  type RedeemResetResult,
  type ResetAccountContext,
  type ResetInventory,
  unavailableCodexUsageResetProvider,
} from "../src/codex_banked_reset_provider.ts";
import type { CodexResetRedemptionRecord } from "../src/types.ts";

const clone = <T>(value: T): T => structuredClone(value);
const encodeKey = (key: Deno.KvKey): string => JSON.stringify(key);

type StoredEntry = Readonly<{
  key: Deno.KvKey;
  value: unknown;
  versionstamp: string;
}>;

/** A small KV fake with versionstamps and deliberately injectable CAS failures. */
class MemoryKv {
  readonly entries = new Map<string, StoredEntry>();
  readonly failCommitNumbers = new Set<number>();
  readonly atomicCheckBatches: Deno.KvKey[][] = [];
  atomicCommitCount = 0;
  beforeAtomicCommit: ((commitNumber: number) => void) | null = null;
  beforeGet: ((key: Deno.KvKey, getNumber: number) => Promise<void> | null) | null = null;
  getFailure: Error | null = null;
  getCount = 0;
  #version = 0;

  #nextVersionstamp(): string {
    this.#version += 1;
    return String(this.#version).padStart(20, "0");
  }

  #put(key: Deno.KvKey, value: unknown, versionstamp: string): void {
    this.entries.set(encodeKey(key), {
      key: clone(key),
      value: clone(value),
      versionstamp,
    });
  }

  versionstamp(key: Deno.KvKey): string | null {
    return this.entries.get(encodeKey(key))?.versionstamp ?? null;
  }

  async get<T>(key: Deno.KvKey, _options?: unknown): Promise<Deno.KvEntryMaybe<T>> {
    if (this.getFailure) throw this.getFailure;
    const pause = this.beforeGet?.(clone(key), ++this.getCount);
    if (pause) await pause;
    const entry = this.entries.get(encodeKey(key));
    return {
      key: clone(key),
      value: entry ? clone(entry.value) as T : null,
      versionstamp: entry?.versionstamp ?? null,
    } as Deno.KvEntryMaybe<T>;
  }

  set(key: Deno.KvKey, value: unknown): Promise<Deno.KvCommitResult> {
    const versionstamp = this.#nextVersionstamp();
    this.#put(key, value, versionstamp);
    return Promise.resolve({ ok: true, versionstamp });
  }

  value<T>(key: Deno.KvKey): T | null {
    const value = this.entries.get(encodeKey(key))?.value;
    return value === undefined ? null : clone(value) as T;
  }

  atomic(): Deno.AtomicOperation {
    const checks: Array<Readonly<{ key: Deno.KvKey; versionstamp: string | null }>> = [];
    const writes: Array<Readonly<{ key: Deno.KvKey; value: unknown }>> = [];
    const operation = {
      check: (entry: Readonly<{ key: Deno.KvKey; versionstamp: string | null }>) => {
        checks.push({ key: clone(entry.key), versionstamp: entry.versionstamp });
        return operation;
      },
      set: (key: Deno.KvKey, value: unknown) => {
        writes.push({ key: clone(key), value: clone(value) });
        return operation;
      },
      commit: (): Promise<Deno.KvCommitResult | Deno.KvCommitError> => {
        this.atomicCommitCount += 1;
        this.atomicCheckBatches.push(checks.map((check) => clone(check.key)));
        this.beforeAtomicCommit?.(this.atomicCommitCount);
        if (this.failCommitNumbers.has(this.atomicCommitCount)) return Promise.resolve({ ok: false });
        if (checks.some((check) => this.versionstamp(check.key) !== check.versionstamp)) {
          return Promise.resolve({ ok: false });
        }

        const versionstamp = this.#nextVersionstamp();
        for (const write of writes) this.#put(write.key, write.value, versionstamp);
        return Promise.resolve({ ok: true, versionstamp });
      },
    };
    return operation as unknown as Deno.AtomicOperation;
  }
}

class Deferred<T> {
  readonly promise: Promise<T>;
  #resolve!: (value: T | PromiseLike<T>) => void;

  constructor() {
    this.promise = new Promise<T>((resolve) => {
      this.#resolve = resolve;
    });
  }

  resolve(value: T): void {
    this.#resolve(value);
  }
}

const provenContract = (): CodexUsageResetProviderContract => ({
  idempotency: { callerSupplied: true, retentionMs: 86_400_000 },
  lookup: { byIdempotencyKey: true, byProviderReceiptId: true },
  verification: { independentlyVerifiable: true },
  receiptIdsSafeToPersistAndLog: true,
  supportedResetTypes: ["banked_reset"],
});

type FakeProviderCall = Readonly<{
  method: "readInventory" | "redeem" | "lookup" | "verifyApplied";
  accountId: string;
  quotaGeneration: string;
  idempotencyKey: string | null;
  commitCount: number;
  providerReceiptId: string | null;
  timeoutStage: "before_provider_commit" | "after_provider_commit" | null;
}>;

/**
 * Sanitized boundary fixtures, not inferred provider wire payloads. There is
 * no approved live adapter yet, so these cover every result shape accepted by
 * the injectable provider contract while keeping tests completely offline.
 */
const sanitizedProviderFixtures = Object.freeze({
  inventory_available: Object.freeze({ availableCount: 1, observedAtMs: 1_700_000_000_000, resetType: "banked_reset" }),
  inventory_empty: Object.freeze({ availableCount: 0, observedAtMs: 1_700_000_000_000, resetType: "banked_reset" }),
  redemption_completed: Object.freeze({ kind: "completed", providerReceiptId: "fixture-completed" } as const),
  redemption_accepted: Object.freeze({ kind: "accepted", providerReceiptId: "fixture-accepted" } as const),
  redemption_already_redeemed: Object.freeze(
    { kind: "already_redeemed", providerReceiptId: "fixture-replay" } as const,
  ),
  redemption_rejected: Object.freeze({ kind: "rejected", reason: "fixture-definitive-rejection" } as const),
  authentication_error: Object.freeze({ kind: "rejected", reason: "fixture-authentication-error" } as const),
  rate_limit: Object.freeze({ kind: "unknown", providerReceiptId: null } as const),
  server_error: Object.freeze({ kind: "unknown", providerReceiptId: null } as const),
  malformed_success: Object.freeze({ kind: "completed" }),
  schema_drift: Object.freeze({ kind: "future_completed", providerReceiptId: "fixture-unknown" }),
  lookup_pending: Object.freeze({ kind: "unknown", providerReceiptId: null } as const),
  lookup_completed: Object.freeze({ kind: "completed", providerReceiptId: "fixture-lookup-completed" } as const),
  lookup_rejected: Object.freeze({ kind: "rejected", reason: "fixture-lookup-rejected" } as const),
  lookup_not_found: Object.freeze({ kind: "rejected", reason: "fixture-lookup-not-found" } as const),
});

class FakeCodexUsageResetProvider implements CodexUsageResetProvider {
  readonly contract: CodexUsageResetProviderContract;
  readonly calls: FakeProviderCall[] = [];
  readonly inventoryInputs: ResetAccountContext[] = [];
  readonly redeemInputs: RedeemResetInput[] = [];
  readonly lookupInputs: LookupRedeemResetInput[] = [];
  readonly verificationInputs: ResetAccountContext[] = [];
  inventory: ResetInventory = { availableCount: 1, observedAtMs: 1_700_000_000_000, resetType: "banked_reset" };
  redeemResult: RedeemResetResult = { kind: "completed", providerReceiptId: "receipt-completed" };
  lookupResult: RedeemResetResult = { kind: "completed", providerReceiptId: "receipt-lookup" };
  verifyResult: unknown = true;
  inventoryFailure: Error | null = null;
  redeemFailure: Error | null = null;
  /** Simulates a response loss after the provider has already committed. */
  redeemFailureAfterCommit: Error | null = null;
  lookupFailure: Error | null = null;
  verifyFailure: Error | null = null;
  inventoryGate: Promise<void> | null = null;
  inventoryEntered: Deferred<void> | null = null;
  redeemGate: Promise<void> | null = null;
  redeemEntered: Deferred<void> | null = null;
  verifyGate: Promise<void> | null = null;
  verifyEntered: Deferred<void> | null = null;
  commitOnRedeem: boolean | null = null;
  commitCount = 0;

  constructor(contract: CodexUsageResetProviderContract = provenContract()) {
    this.contract = contract;
  }

  get callCount(): number {
    return this.inventoryInputs.length + this.redeemInputs.length + this.lookupInputs.length +
      this.verificationInputs.length;
  }

  #record(
    method: FakeProviderCall["method"],
    input: ResetAccountContext | RedeemResetInput | LookupRedeemResetInput,
    providerReceiptId: string | null = null,
    timeoutStage: FakeProviderCall["timeoutStage"] = null,
  ): void {
    const withKey = input as Partial<RedeemResetInput>;
    this.calls.push({
      method,
      accountId: input.accountId,
      quotaGeneration: input.quotaGeneration,
      idempotencyKey: typeof withKey.idempotencyKey === "string" ? withKey.idempotencyKey : null,
      commitCount: this.commitCount,
      providerReceiptId,
      timeoutStage,
    });
  }

  async readInventory(input: ResetAccountContext, _signal: AbortSignal): Promise<ResetInventory> {
    this.inventoryInputs.push(clone(input));
    this.#record("readInventory", input);
    this.inventoryEntered?.resolve(undefined);
    const gate = this.inventoryGate;
    this.inventoryGate = null;
    if (gate) await gate;
    if (this.inventoryFailure) throw this.inventoryFailure;
    return clone(this.inventory);
  }

  async redeem(input: RedeemResetInput, _signal: AbortSignal): Promise<RedeemResetResult> {
    this.redeemInputs.push(clone(input));
    this.redeemEntered?.resolve(undefined);
    if (this.redeemGate) await this.redeemGate;
    if (this.redeemFailure) {
      this.#record("redeem", input, null, "before_provider_commit");
      throw this.redeemFailure;
    }
    const result = clone(this.redeemResult);
    if (this.commitOnRedeem ?? (result.kind === "completed" || result.kind === "accepted")) this.commitCount += 1;
    const receipt = "providerReceiptId" in result && typeof result.providerReceiptId === "string"
      ? result.providerReceiptId
      : null;
    if (this.redeemFailureAfterCommit) {
      this.#record("redeem", input, receipt, "after_provider_commit");
      throw this.redeemFailureAfterCommit;
    }
    this.#record("redeem", input, receipt);
    return result;
  }

  lookup(input: LookupRedeemResetInput, _signal: AbortSignal): Promise<RedeemResetResult> {
    this.lookupInputs.push(clone(input));
    const receipt = "providerReceiptId" in this.lookupResult && typeof this.lookupResult.providerReceiptId === "string"
      ? this.lookupResult.providerReceiptId
      : null;
    this.#record("lookup", input, receipt);
    if (this.lookupFailure) return Promise.reject(this.lookupFailure);
    return Promise.resolve(clone(this.lookupResult));
  }

  async verifyApplied(input: ResetAccountContext, _signal: AbortSignal): Promise<boolean> {
    this.verificationInputs.push(clone(input));
    this.verifyEntered?.resolve(undefined);
    const gate = this.verifyGate;
    this.verifyGate = null;
    if (gate) await gate;
    this.#record("verifyApplied", input);
    if (this.verifyFailure) return Promise.reject(this.verifyFailure);
    return this.verifyResult as boolean;
  }
}

class TestClock {
  nowMs: number;

  constructor(nowMs = 1_700_000_000_000) {
    this.nowMs = nowMs;
  }

  advance(ms: number): void {
    this.nowMs += ms;
  }
}

const testHash = (value: string): Promise<string> => {
  let hash = 2_166_136_261;
  for (let index = 0; index < value.length; index += 1) {
    hash ^= value.charCodeAt(index);
    hash = Math.imul(hash, 16_777_619);
  }
  return Promise.resolve(`test-hash-${(hash >>> 0).toString(16)}-${value.length}`);
};

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const routingFenceKey = (accountId: string): Deno.KvKey => ["test", "codex-banked-reset", "routing", accountId];
const credentialFenceKey = (accountId: string): Deno.KvKey => ["test", "codex-banked-reset", "credential", accountId];

const bankedResetFences = (
  accountId: string,
  routingGeneration: number,
  credentialVersion: string,
): readonly CodexBankedResetFence[] => [
  {
    key: routingFenceKey(accountId),
    isCurrent: (value) => isRecord(value) && value.kind === "routing" && value.routing_generation === routingGeneration,
  },
  {
    key: credentialFenceKey(accountId),
    isCurrent: (value) =>
      isRecord(value) && value.kind === "credential" && value.credential_version === credentialVersion,
  },
];

const candidate = (overrides: Partial<CodexBankedResetCandidate> = {}): CodexBankedResetCandidate => {
  const accountId = overrides.accountId ?? "test-account-a";
  const credentialVersion = overrides.credentialVersion ?? "test-credential-v1";
  const quotaResetAtMs = overrides.quotaResetAtMs ?? 1_700_000_060_000;
  const routingGeneration = overrides.routingGeneration ?? 7;
  return {
    accountId,
    credentialVersion,
    quotaResetAtMs,
    routingGeneration,
    fences: overrides.fences ?? bankedResetFences(accountId, routingGeneration, credentialVersion),
    requestId: overrides.requestId ?? "test-request",
    ...(overrides.signal === undefined ? {} : { signal: overrides.signal }),
  };
};

const seedFences = async (kv: MemoryKv, input: CodexBankedResetCandidate): Promise<void> => {
  await kv.set(routingFenceKey(input.accountId), {
    kind: "routing",
    routing_generation: input.routingGeneration,
  });
  await kv.set(credentialFenceKey(input.accountId), {
    kind: "credential",
    credential_version: input.credentialVersion,
  });
};

const hasKey = (keys: readonly Deno.KvKey[], expected: Deno.KvKey): boolean =>
  keys.some((key) => encodeKey(key) === encodeKey(expected));

const config = (overrides: Partial<CodexBankedResetConfig> = {}): CodexBankedResetConfig => ({
  enabled: true,
  mode: "live",
  accountAllowlist: new Set(["test-account-a"]),
  maxGlobalPerDay: 5,
  maxPerAccountPerWindow: 1,
  ...overrides,
});

const dependencies = (
  kv: MemoryKv,
  provider: CodexUsageResetProvider,
  clock: TestClock,
  configured = config(),
  telemetry: CodexBankedResetTelemetry = {},
  reloadConfig?: () => CodexBankedResetConfig,
): CodexBankedResetDependencies => {
  let owner = 0;
  return {
    config: configured,
    provider,
    kv: kv as unknown as Deno.Kv,
    now: () => clock.nowMs,
    newOwnerToken: () => `test-owner-${++owner}`,
    hash: testHash,
    telemetry,
    ...(reloadConfig ? { reloadConfig } : {}),
  };
};

Deno.test("banked reset disabled, shadow, missing allowlist, and invalid limits make zero provider calls", async () => {
  const clock = new TestClock();
  const cases: Array<
    Readonly<{
      name: string;
      configured: CodexBankedResetConfig;
      reason: string;
      expectShadowEvent?: boolean;
    }>
  > = [
    {
      name: "feature disabled",
      configured: config({ enabled: false }),
      reason: "feature_disabled",
    },
    {
      name: "shadow",
      configured: config({ mode: "shadow", maxGlobalPerDay: 0 }),
      reason: "shadow",
      expectShadowEvent: true,
    },
    {
      name: "missing allowlist",
      configured: config({ accountAllowlist: new Set() }),
      reason: "allowlist_missing",
    },
    {
      name: "global cap disabled",
      configured: config({ maxGlobalPerDay: 0 }),
      reason: "global_limit_disabled",
    },
    {
      name: "per-account cap invalid",
      configured: config({ maxPerAccountPerWindow: 2 }),
      reason: "per_account_window_limit_invalid",
    },
  ];

  for (const testCase of cases) {
    const kv = new MemoryKv();
    const provider = new FakeCodexUsageResetProvider();
    const events: string[] = [];
    const result = await attemptCodexBankedReset(
      candidate(),
      dependencies(kv, provider, clock, testCase.configured, { event: (event) => events.push(event) }),
    );

    assert.equal(result.kind, "skipped", testCase.name);
    assert.equal(result.reason, testCase.reason, testCase.name);
    assert.equal(provider.callCount, 0, testCase.name);
    assert.equal(provider.commitCount, 0, testCase.name);
    assert.equal(events.includes("codex_reset_shadow_candidate"), testCase.expectShadowEvent ?? false, testCase.name);
  }
});

Deno.test("banked reset requires an exact allowlisted account ID or stable account hash", async () => {
  const clock = new TestClock();
  const reset = candidate();
  const accountIdHash = await testHash(reset.accountId);

  const rejectedKv = new MemoryKv();
  const rejectedProvider = new FakeCodexUsageResetProvider();
  const rejected = await attemptCodexBankedReset(
    reset,
    dependencies(rejectedKv, rejectedProvider, clock, config({ accountAllowlist: new Set(["other-account"]) })),
  );
  assert.equal(rejected.kind, "skipped");
  assert.equal(rejected.reason, "account_not_allowlisted");
  assert.equal(rejectedProvider.callCount, 0);
  assert.equal(rejectedProvider.commitCount, 0);

  const allowedKv = new MemoryKv();
  const allowedProvider = new FakeCodexUsageResetProvider();
  await seedFences(allowedKv, reset);
  const allowed = await attemptCodexBankedReset(
    reset,
    dependencies(allowedKv, allowedProvider, clock, config({ accountAllowlist: new Set([accountIdHash]) })),
  );
  assert.equal(allowed.kind, "verified");
  assert.equal(allowed.accountIdHash, accountIdHash);
  assert.equal(allowedProvider.commitCount, 1);
});

Deno.test("banked reset live happy path commits exactly once with a stable durable identity", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const deps = dependencies(kv, provider, clock);
  const reset = candidate();
  await seedFences(kv, reset);
  const first = await attemptCodexBankedReset(reset, deps);

  assert.equal(first.kind, "verified");
  assert.equal(first.reason, "verified");
  assert.equal(provider.commitCount, 1);
  assert.equal(provider.inventoryInputs.length, 1);
  assert.equal(provider.redeemInputs.length, 1);
  assert.equal(provider.verificationInputs.length, 1);
  assert.ok(first.accountIdHash);
  assert.ok(first.quotaGeneration);
  assert.ok(first.idempotencyKeyHash);
  assert.equal(first.record?.state, "verified");
  assert.equal(first.record?.idempotency_key_hash, first.idempotencyKeyHash);
  assert.equal(provider.redeemInputs[0]?.accountId, "test-account-a");
  assert.match(provider.redeemInputs[0]?.idempotencyKey ?? "", /^uos_ai_codex_reset_v1_/);

  const redemptionKey = codexResetRedemptionKey(first.accountIdHash, first.quotaGeneration);
  const durable = await kv.get<CodexResetRedemptionRecord>(redemptionKey);
  assert.equal(durable.value?.state, "verified");
  assert.equal(durable.value?.provider_receipt_id, "receipt-completed");
  assert.equal(durable.value?.idempotency_key_hash, first.idempotencyKeyHash);

  const day = new Date(clock.nowMs).toISOString().slice(0, 10);
  const daily = await kv.get<{ claimed_count: number }>(codexResetGlobalDailyKey(day));
  assert.equal(daily.value?.claimed_count, 1);

  const duplicate = await attemptCodexBankedReset(
    candidate({ requestId: "same-window-later-request" }),
    deps,
  );
  assert.equal(duplicate.kind, "verified");
  assert.equal(duplicate.reason, "previously_verified");
  assert.equal(duplicate.idempotencyKeyHash, first.idempotencyKeyHash);
  assert.equal(provider.redeemInputs.length, 1);
  assert.equal(provider.commitCount, 1);
});

Deno.test("unknown provider outcome is recovered through lookup with the same key and no second redemption", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  provider.redeemResult = { kind: "unknown", providerReceiptId: null };
  provider.commitOnRedeem = true;
  const deps = dependencies(kv, provider, clock);
  const reset = candidate();
  await seedFences(kv, reset);

  const first = await attemptCodexBankedReset(reset, deps);
  assert.equal(first.kind, "pending");
  assert.equal(first.reason, "provider_commit_unknown");
  assert.equal(first.record?.state, "unknown");
  assert.equal(provider.redeemInputs.length, 1);
  assert.equal(provider.commitCount, 1);
  const stableKey = provider.redeemInputs[0]?.idempotencyKey;
  assert.ok(stableKey);

  provider.lookupResult = { kind: "already_redeemed", providerReceiptId: "receipt-recovered" };
  clock.advance(CODEX_BANKED_RESET_LEASE_MS + 1);
  const recovered = await attemptCodexBankedReset(
    candidate({ requestId: "recovery-request" }),
    deps,
  );

  assert.equal(recovered.kind, "verified");
  assert.equal(recovered.reason, "verified");
  assert.equal(recovered.record?.state, "verified");
  assert.equal(recovered.record?.fence, 3);
  assert.equal(provider.redeemInputs.length, 1);
  assert.equal(provider.commitCount, 1);
  assert.equal(provider.lookupInputs.length, 1);
  assert.equal(provider.lookupInputs[0]?.idempotencyKey, stableKey);
  assert.equal(provider.lookupInputs[0]?.providerReceiptId, null);
  assert.equal(recovered.idempotencyKeyHash, first.idempotencyKeyHash);
});

Deno.test("concurrent claims for one generation permit one provider submission and prevent the duplicate", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const gate = new Deferred<void>();
  const entered = new Deferred<void>();
  provider.redeemGate = gate.promise;
  provider.redeemEntered = entered;
  const deps = dependencies(kv, provider, clock);
  const reset = candidate();
  await seedFences(kv, reset);

  const firstPromise = attemptCodexBankedReset(reset, deps);
  await entered.promise;
  const duplicate = await attemptCodexBankedReset(
    candidate({ requestId: "concurrent-request" }),
    deps,
  );

  assert.equal(duplicate.kind, "pending");
  assert.equal(duplicate.reason, "transaction_in_progress");
  assert.equal(provider.inventoryInputs.length, 1);
  assert.equal(provider.redeemInputs.length, 1);
  assert.equal(provider.commitCount, 0);

  gate.resolve(undefined);
  const first = await firstPromise;
  assert.equal(first.kind, "verified");
  assert.equal(provider.commitCount, 1);
  assert.equal(provider.redeemInputs.length, 1);
});

Deno.test("an expired recovery lookup rejection stays unknown while the original redemption may be in flight", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const reset = candidate();
  await seedFences(kv, reset);
  const redeemGate = new Deferred<void>();
  const redeemEntered = new Deferred<void>();
  provider.redeemGate = redeemGate.promise;
  provider.redeemEntered = redeemEntered;
  const deps = dependencies(kv, provider, clock);

  const original = attemptCodexBankedReset(reset, deps);
  await redeemEntered.promise;
  clock.advance(CODEX_BANKED_RESET_LEASE_MS + 1);
  provider.lookupResult = { kind: "rejected", reason: "not yet visible" };
  provider.verifyResult = false;
  const recovery = await reconcileCodexBankedReset(
    candidate({ requestId: "in-flight-negative-lookup" }),
    deps,
  );

  assert.equal(recovery.kind, "pending");
  assert.equal(recovery.reason, "verification_not_applied");
  assert.equal(recovery.record?.state, "unknown");
  assert.equal(provider.redeemInputs.length, 1);
  assert.equal(provider.lookupInputs.length, 1);

  redeemGate.resolve(undefined);
  const originalOutcome = await original;
  assert.equal(originalOutcome.kind, "pending");
  assert.equal(originalOutcome.reason, "receipt_cas_failed");
  assert.equal(provider.redeemInputs.length, 1);
  assert.equal(
    kv.value<CodexResetRedemptionRecord>(
      codexResetRedemptionKey(recovery.accountIdHash!, recovery.quotaGeneration!),
    )?.state,
    "unknown",
  );
});

Deno.test("live claims require seeded current fences and CAS-check both routing and credential records", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const deps = dependencies(kv, provider, clock);

  const missing = await attemptCodexBankedReset(candidate({ fences: [] }), deps);
  assert.equal(missing.kind, "skipped");
  assert.equal(missing.reason, "routing_fence_missing");
  assert.equal(provider.callCount, 0);

  const reset = candidate();
  const stale = await attemptCodexBankedReset(reset, deps);
  assert.equal(stale.kind, "skipped");
  assert.equal(stale.reason, "routing_fence_stale");
  assert.equal(provider.callCount, 0);

  await seedFences(kv, reset);
  const verified = await attemptCodexBankedReset(reset, deps);
  assert.equal(verified.kind, "verified");
  assert.equal(provider.commitCount, 1);
  assert.equal(
    kv.atomicCheckBatches.filter((batch) =>
      hasKey(batch, routingFenceKey(reset.accountId)) && hasKey(batch, credentialFenceKey(reset.accountId))
    ).length,
    3,
  );
});

Deno.test("a malformed truthy verification result remains unknown and cannot authorize a reset", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const reset = candidate();
  await seedFences(kv, reset);
  provider.verifyResult = { applied: true };

  const result = await attemptCodexBankedReset(reset, dependencies(kv, provider, clock));
  assert.equal(result.kind, "pending");
  assert.equal(result.reason, "verification_response_invalid");
  assert.equal(result.record?.state, "unknown");
  assert.equal(result.record?.last_error_code, "verification_response_invalid");
  assert.equal(provider.commitCount, 1);
  assert.equal(provider.redeemInputs.length, 1);
  assert.equal(provider.verificationInputs.length, 1);
});

Deno.test("a reloadable kill switch changed during inventory leaves the claim durable and makes no redemption", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const reset = candidate();
  await seedFences(kv, reset);
  const inventoryGate = new Deferred<void>();
  const inventoryEntered = new Deferred<void>();
  provider.inventoryGate = inventoryGate.promise;
  provider.inventoryEntered = inventoryEntered;
  let currentConfig = config();
  const deps = dependencies(kv, provider, clock, currentConfig, {}, () => currentConfig);

  const pending = attemptCodexBankedReset(reset, deps);
  await inventoryEntered.promise;
  currentConfig = config({ enabled: false });
  inventoryGate.resolve(undefined);
  const result = await pending;

  assert.equal(result.kind, "pending");
  assert.equal(result.reason, "new_submission_feature_disabled");
  assert.equal(result.record?.state, "claimed");
  assert.equal(provider.inventoryInputs.length, 1);
  assert.equal(provider.redeemInputs.length, 0);
  assert.equal(provider.commitCount, 0);
});

Deno.test("inventory that returns after the quota deadline cannot cross the submission boundary", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const reset = candidate({ quotaResetAtMs: clock.nowMs + 1 });
  await seedFences(kv, reset);
  const inventoryGate = new Deferred<void>();
  const inventoryEntered = new Deferred<void>();
  provider.inventoryGate = inventoryGate.promise;
  provider.inventoryEntered = inventoryEntered;

  const pending = attemptCodexBankedReset(reset, dependencies(kv, provider, clock));
  await inventoryEntered.promise;
  clock.advance(1);
  provider.inventory = { ...provider.inventory, observedAtMs: clock.nowMs };
  inventoryGate.resolve(undefined);
  const result = await pending;

  assert.equal(result.kind, "rejected");
  assert.equal(result.reason, "quota_window_expired");
  assert.equal(result.record?.state, "rejected");
  assert.equal(provider.inventoryInputs.length, 1);
  assert.equal(provider.redeemInputs.length, 0);
  assert.equal(provider.commitCount, 0);
  assert.equal(
    kv.atomicCheckBatches.filter((batch) =>
      hasKey(batch, routingFenceKey(reset.accountId)) && hasKey(batch, credentialFenceKey(reset.accountId))
    ).length,
    1,
  );
});

Deno.test("an already-expired quota window is never claimed or charged against the daily cap", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const reset = candidate({ quotaResetAtMs: clock.nowMs });
  await seedFences(kv, reset);

  const result = await attemptCodexBankedReset(reset, dependencies(kv, provider, clock));

  assert.equal(result.kind, "skipped");
  assert.equal(result.reason, "quota_window_expired");
  assert.equal(provider.callCount, 0);
  assert.equal(provider.commitCount, 0);
  assert.equal(kv.atomicCommitCount, 0);
  const day = new Date(clock.nowMs).toISOString().slice(0, 10);
  assert.equal((await kv.get(codexResetGlobalDailyKey(day))).value, null);
});

Deno.test("a quota deadline crossing during claim preparation cannot reserve daily capacity", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const reset = candidate({ quotaResetAtMs: clock.nowMs + 1 });
  await seedFences(kv, reset);
  const day = new Date(clock.nowMs).toISOString().slice(0, 10);
  const dailyKey = codexResetGlobalDailyKey(day);
  kv.beforeGet = (key) => {
    if (encodeKey(key) !== encodeKey(dailyKey)) return null;
    kv.beforeGet = null;
    clock.advance(1);
    return null;
  };

  const result = await attemptCodexBankedReset(reset, dependencies(kv, provider, clock));

  assert.equal(result.kind, "skipped");
  assert.equal(result.reason, "quota_window_expired");
  assert.equal(provider.callCount, 0);
  assert.equal(provider.commitCount, 0);
  assert.equal(kv.atomicCommitCount, 0);
  assert.equal((await kv.get(dailyKey)).value, null);
});

Deno.test("a quota deadline crossing during the final renewal cannot reach redemption", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const reset = candidate({ quotaResetAtMs: clock.nowMs + 1 });
  await seedFences(kv, reset);

  // Claim and `claimed -> submitted` are commits one and two. Make the
  // window expire while the final submitted-owner/fence CAS commits: its
  // post-CAS timestamp was still valid, so the immediate pre-call guard is
  // the only safe place to stop this external spend.
  kv.beforeAtomicCommit = (commitNumber) => {
    if (commitNumber !== 3) return;
    kv.beforeAtomicCommit = null;
    clock.advance(1);
  };

  const result = await attemptCodexBankedReset(reset, dependencies(kv, provider, clock));

  assert.equal(result.kind, "pending");
  assert.equal(result.reason, "quota_window_expired");
  assert.equal(result.record?.state, "submitted");
  assert.equal(provider.inventoryInputs.length, 1);
  assert.equal(provider.redeemInputs.length, 0);
  assert.equal(provider.commitCount, 0);
  assert.equal(kv.atomicCommitCount, 3);
});

Deno.test("a kill switch landing after the durable boundary still blocks provider redemption", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const reset = candidate();
  await seedFences(kv, reset);
  let currentConfig = config();
  const deps = dependencies(kv, provider, clock, currentConfig, {}, () => currentConfig);

  // The first atomic commit creates `claimed`; the second changes it to
  // `submitted`. Flip the operator configuration in that exact final window
  // before the fake provider could observe a redeem call.
  kv.beforeAtomicCommit = (commitNumber) => {
    if (commitNumber !== 2) return;
    kv.beforeAtomicCommit = null;
    currentConfig = config({ enabled: false, mode: "disabled" });
  };

  const result = await attemptCodexBankedReset(reset, deps);
  assert.equal(result.kind, "pending");
  assert.equal(result.reason, "new_submission_feature_disabled");
  assert.equal(result.record?.state, "submitted");
  assert.equal(provider.inventoryInputs.length, 1);
  assert.equal(provider.redeemInputs.length, 0);
  assert.equal(provider.commitCount, 0);
});

Deno.test("a kill switch landing during the final renewal still blocks provider redemption", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const reset = candidate();
  await seedFences(kv, reset);
  let currentConfig = config();
  const deps = dependencies(kv, provider, clock, currentConfig, {}, () => currentConfig);

  // Claim is commit 1 and `claimed -> submitted` is commit 2. Flip the
  // operator configuration immediately before the final submitted-lease
  // renewal commits. The post-renewal synchronous read must win before the
  // provider can observe a redeem call.
  kv.beforeAtomicCommit = (commitNumber) => {
    if (commitNumber !== 3) return;
    kv.beforeAtomicCommit = null;
    currentConfig = config({ enabled: false, mode: "disabled" });
  };

  const result = await attemptCodexBankedReset(reset, deps);
  assert.equal(result.kind, "pending");
  assert.equal(result.reason, "new_submission_feature_disabled");
  assert.equal(result.record?.state, "submitted");
  assert.equal(provider.inventoryInputs.length, 1);
  assert.equal(provider.redeemInputs.length, 0);
  assert.equal(provider.commitCount, 0);
});

Deno.test("an unapproved provider receipt stays out of the durable record and telemetry", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider({
    ...provenContract(),
    receiptIdsSafeToPersistAndLog: false,
  });
  const clock = new TestClock();
  const reset = candidate();
  await seedFences(kv, reset);
  const submittedFields: CodexBankedResetTelemetryFields[] = [];
  const result = await attemptCodexBankedReset(
    reset,
    dependencies(kv, provider, clock, config(), {
      event: (event, fields) => {
        if (event === "codex_reset_submitted") submittedFields.push(fields);
      },
    }),
  );

  assert.equal(result.kind, "verified");
  assert.equal(result.record?.provider_receipt_id, null);
  assert.equal(
    kv.value<CodexResetRedemptionRecord>(
      codexResetRedemptionKey(result.accountIdHash!, result.quotaGeneration!),
    )?.provider_receipt_id,
    null,
  );
  assert.deepEqual(submittedFields.map((fields) => fields.provider_receipt_id), [null]);
});

Deno.test("an explicit HTTP-2xx-final provider completes without lookup or verification", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider({
    ...provenContract(),
    idempotency: { callerSupplied: true, retentionMs: null },
    lookup: { byIdempotencyKey: false, byProviderReceiptId: false },
    verification: { independentlyVerifiable: false },
    http2xxIsFinal: true,
    receiptIdsSafeToPersistAndLog: false,
  });
  const clock = new TestClock();
  const reset = candidate();
  const events: string[] = [];
  await seedFences(kv, reset);
  provider.redeemResult = { kind: "completed", providerReceiptId: "status-204" };
  provider.verifyResult = false;

  const result = await attemptCodexBankedReset(
    reset,
    dependencies(kv, provider, clock, config(), { event: (event) => events.push(event) }),
  );

  assert.equal(result.kind, "verified");
  assert.equal(result.reason, "http_2xx");
  assert.equal(result.record?.state, "verified");
  assert.equal(result.record?.provider_receipt_id, null);
  assert.equal(provider.redeemInputs.length, 1);
  assert.equal(provider.verificationInputs.length, 0);
  assert.equal(provider.lookupInputs.length, 0);
  assert.ok(events.includes("codex_reset_submitted"));
  assert.ok(events.includes("codex_reset_verified"));
});

Deno.test("banked-reset telemetry retains only safe correlation fields", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider({
    ...provenContract(),
    receiptIdsSafeToPersistAndLog: false,
  });
  const clock = new TestClock();
  const reset = candidate({
    accountId: "raw-account-id-must-not-leak",
    credentialVersion: "raw-credential-version-must-not-leak",
    requestId: "safe-request-id",
  });
  await seedFences(kv, reset);
  provider.redeemResult = { kind: "completed", providerReceiptId: "unapproved-provider-receipt" };
  const events: Array<Readonly<{ event: string; fields: CodexBankedResetTelemetryFields }>> = [];
  const metrics: CodexBankedResetTelemetryFields[] = [];

  const result = await attemptCodexBankedReset(
    reset,
    dependencies(kv, provider, clock, config({ accountAllowlist: new Set([reset.accountId]) }), {
      event: (event, fields) => events.push({ event, fields }),
      metric: (_metric, _value, fields) => metrics.push(fields),
    }),
  );

  assert.equal(result.kind, "verified");
  assert.ok(provider.redeemInputs[0]?.idempotencyKey);
  assert.ok(events.some(({ event }) => event === "codex_reset_eligible"));
  assert.ok(events.some(({ event }) => event === "codex_reset_claimed"));
  assert.ok(events.some(({ event }) => event === "codex_reset_submitted"));
  assert.ok(events.some(({ event }) => event === "codex_reset_verified"));
  assert.ok(events.every(({ fields }) => typeof fields.account_id_hash === "string"));

  const rawValues = [
    reset.accountId,
    reset.credentialVersion,
    provider.redeemInputs[0]!.idempotencyKey,
    "unapproved-provider-receipt",
  ];
  for (const fields of [...events.map(({ fields }) => fields), ...metrics]) {
    const serialized = JSON.stringify(fields);
    for (const raw of rawValues) assert.equal(serialized.includes(raw), false, `telemetry leaked ${raw}`);
  }
});

Deno.test("a lease-expired replacement takes over while the stale owner is blocked before redemption", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const reset = candidate();
  await seedFences(kv, reset);
  const inventoryGate = new Deferred<void>();
  const inventoryEntered = new Deferred<void>();
  provider.inventoryGate = inventoryGate.promise;
  provider.inventoryEntered = inventoryEntered;
  const deps = dependencies(kv, provider, clock);

  const staleOwner = attemptCodexBankedReset(reset, deps);
  await inventoryEntered.promise;
  clock.advance(CODEX_BANKED_RESET_LEASE_MS + 1);
  provider.inventory = { ...provider.inventory, observedAtMs: clock.nowMs };
  const replacement = await attemptCodexBankedReset(candidate({ requestId: "lease-takeover" }), deps);
  assert.equal(replacement.kind, "verified");
  assert.equal(replacement.record?.fence, 3);
  assert.equal(provider.redeemInputs.length, 1);

  inventoryGate.resolve(undefined);
  const staleOutcome = await staleOwner;
  assert.equal(staleOutcome.kind, "pending");
  assert.equal(staleOutcome.reason, "stale_owner");
  assert.equal(provider.redeemInputs.length, 1);
  assert.equal(provider.commitCount, 1);
});

Deno.test("an expired claimed record cannot be taken over after its quota deadline", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const reset = candidate({ quotaResetAtMs: clock.nowMs + 1 });
  await seedFences(kv, reset);
  const inventoryGate = new Deferred<void>();
  const inventoryEntered = new Deferred<void>();
  provider.inventoryGate = inventoryGate.promise;
  provider.inventoryEntered = inventoryEntered;
  const deps = dependencies(kv, provider, clock);

  const original = attemptCodexBankedReset(reset, deps);
  await inventoryEntered.promise;
  clock.advance(CODEX_BANKED_RESET_LEASE_MS + 1);

  const takeover = await attemptCodexBankedReset(candidate({ ...reset, requestId: "expired-claim-takeover" }), deps);
  assert.equal(takeover.kind, "skipped");
  assert.equal(takeover.reason, "quota_window_expired");
  assert.equal(provider.inventoryInputs.length, 1);
  assert.equal(provider.redeemInputs.length, 0);
  assert.equal(provider.commitCount, 0);
  const day = new Date(clock.nowMs).toISOString().slice(0, 10);
  assert.equal((await kv.get<{ claimed_count: number }>(codexResetGlobalDailyKey(day))).value?.claimed_count, 1);

  inventoryGate.resolve(undefined);
  const originalResult = await original;
  assert.equal(originalResult.kind, "rejected");
  assert.equal(originalResult.reason, "quota_window_expired");
  assert.equal(provider.redeemInputs.length, 0);
});

Deno.test("a quota deadline crossing during claimed-takeover fence reads leaves the claim untouched", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const reset = candidate({ quotaResetAtMs: clock.nowMs + CODEX_BANKED_RESET_LEASE_MS + 2 });
  await seedFences(kv, reset);
  const inventoryGate = new Deferred<void>();
  const inventoryEntered = new Deferred<void>();
  provider.inventoryGate = inventoryGate.promise;
  provider.inventoryEntered = inventoryEntered;
  const deps = dependencies(kv, provider, clock);

  const original = attemptCodexBankedReset(reset, deps);
  await inventoryEntered.promise;
  clock.advance(CODEX_BANKED_RESET_LEASE_MS + 1);
  kv.beforeGet = (key) => {
    if (encodeKey(key) !== encodeKey(credentialFenceKey(reset.accountId))) return null;
    kv.beforeGet = null;
    clock.advance(1);
    return null;
  };

  const takeover = await attemptCodexBankedReset(candidate({ ...reset, requestId: "claim-takeover-deadline" }), deps);
  assert.equal(takeover.kind, "skipped");
  assert.equal(takeover.reason, "quota_window_expired");
  assert.equal(kv.atomicCommitCount, 1);
  assert.equal(provider.inventoryInputs.length, 1);
  assert.equal(provider.redeemInputs.length, 0);

  inventoryGate.resolve(undefined);
  const originalResult = await original;
  assert.equal(originalResult.kind, "rejected");
  assert.equal(originalResult.reason, "quota_window_expired");
  assert.equal(provider.redeemInputs.length, 0);
});

Deno.test("a post-submission lease takeover fences the paused owner before provider redemption", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const reset = candidate();
  await seedFences(kv, reset);
  const renewalGate = new Deferred<void>();
  const renewalEntered = new Deferred<void>();
  kv.beforeGet = () => {
    // Claim and `claimed -> submitted` are the first two commits. The next
    // redemption-record read is the final owner/fence/lease renewal.
    if (kv.atomicCommitCount !== 2) return null;
    kv.beforeGet = null;
    renewalEntered.resolve(undefined);
    return renewalGate.promise;
  };
  const deps = dependencies(kv, provider, clock);

  const pausedOwner = attemptCodexBankedReset(reset, deps);
  await renewalEntered.promise;
  clock.advance(CODEX_BANKED_RESET_LEASE_MS + 1);
  provider.lookupResult = { kind: "unknown", providerReceiptId: null };
  provider.verifyResult = false;
  const takeover = await reconcileCodexBankedReset(
    candidate({ requestId: "post-submission-takeover" }),
    deps,
  );

  assert.equal(takeover.kind, "pending");
  assert.equal(takeover.reason, "verification_not_applied");
  assert.equal(takeover.record?.state, "unknown");
  assert.equal(takeover.record?.fence, 2);
  assert.equal(provider.redeemInputs.length, 0);

  renewalGate.resolve(undefined);
  const staleOutcome = await pausedOwner;
  assert.equal(staleOutcome.kind, "pending");
  assert.equal(staleOutcome.reason, "stale_owner");
  assert.equal(provider.redeemInputs.length, 0);
  assert.equal(provider.commitCount, 0);
});

Deno.test("a routing-fence change between the strong read and durable submission CAS prevents redemption", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const reset = candidate();
  await seedFences(kv, reset);
  kv.beforeAtomicCommit = (commitNumber) => {
    if (commitNumber !== 2) return;
    kv.beforeAtomicCommit = null;
    void kv.set(routingFenceKey(reset.accountId), {
      kind: "routing",
      routing_generation: reset.routingGeneration + 1,
    });
  };

  const result = await attemptCodexBankedReset(reset, dependencies(kv, provider, clock));
  assert.equal(result.kind, "pending");
  assert.equal(result.reason, "routing_fence_stale");
  assert.equal(result.record?.state, "claimed");
  assert.equal(provider.redeemInputs.length, 0);
  assert.equal(provider.commitCount, 0);
  assert.equal(kv.atomicCommitCount, 2);
  assert.equal(hasKey(kv.atomicCheckBatches[1] ?? [], routingFenceKey(reset.accountId)), true);
});

Deno.test("a routing-fence change after submission is caught by the final renewal CAS", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const reset = candidate();
  await seedFences(kv, reset);
  kv.beforeAtomicCommit = (commitNumber) => {
    if (commitNumber !== 3) return;
    kv.beforeAtomicCommit = null;
    void kv.set(routingFenceKey(reset.accountId), {
      kind: "routing",
      routing_generation: reset.routingGeneration + 1,
    });
  };

  const result = await attemptCodexBankedReset(reset, dependencies(kv, provider, clock));
  assert.equal(result.kind, "pending");
  assert.equal(result.reason, "routing_fence_stale");
  assert.equal(result.record?.state, "submitted");
  assert.equal(provider.redeemInputs.length, 0);
  assert.equal(provider.commitCount, 0);
  assert.equal(hasKey(kv.atomicCheckBatches[2] ?? [], routingFenceKey(reset.accountId)), true);
  assert.equal(hasKey(kv.atomicCheckBatches[2] ?? [], credentialFenceKey(reset.accountId)), true);
});

Deno.test("an auth fence change after submission is caught by the final renewal CAS", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const reset = candidate();
  await seedFences(kv, reset);
  kv.beforeAtomicCommit = (commitNumber) => {
    if (commitNumber !== 3) return;
    kv.beforeAtomicCommit = null;
    void kv.set(credentialFenceKey(reset.accountId), {
      kind: "credential",
      credential_version: "test-credential-v2-rotated",
    });
  };

  const result = await attemptCodexBankedReset(reset, dependencies(kv, provider, clock));
  assert.equal(result.kind, "pending");
  assert.equal(result.reason, "routing_fence_stale");
  assert.equal(result.record?.state, "submitted");
  assert.equal(provider.redeemInputs.length, 0);
  assert.equal(provider.commitCount, 0);
  assert.equal(hasKey(kv.atomicCheckBatches[2] ?? [], routingFenceKey(reset.accountId)), true);
  assert.equal(hasKey(kv.atomicCheckBatches[2] ?? [], credentialFenceKey(reset.accountId)), true);
});

Deno.test("a credential-rotation-style fence change before durable submission prevents redemption", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const reset = candidate();
  await seedFences(kv, reset);
  kv.beforeAtomicCommit = (commitNumber) => {
    if (commitNumber !== 2) return;
    kv.beforeAtomicCommit = null;
    void kv.set(credentialFenceKey(reset.accountId), {
      kind: "credential",
      credential_version: "test-credential-v2-rotated",
    });
  };

  const result = await attemptCodexBankedReset(reset, dependencies(kv, provider, clock));
  assert.equal(result.kind, "pending");
  assert.equal(result.reason, "routing_fence_stale");
  assert.equal(result.record?.state, "claimed");
  assert.equal(provider.redeemInputs.length, 0);
  assert.equal(provider.commitCount, 0);
  assert.equal(hasKey(kv.atomicCheckBatches[1] ?? [], credentialFenceKey(reset.accountId)), true);
});

Deno.test("a durable unknown reset is reconciled while live configuration is disabled without a second redemption", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const reset = candidate();
  await seedFences(kv, reset);
  provider.redeemResult = { kind: "unknown", providerReceiptId: null };
  provider.commitOnRedeem = true;
  const liveDeps = dependencies(kv, provider, clock);

  const first = await attemptCodexBankedReset(reset, liveDeps);
  assert.equal(first.kind, "pending");
  assert.equal(first.record?.state, "unknown");
  assert.equal(provider.redeemInputs.length, 1);

  clock.advance(CODEX_BANKED_RESET_LEASE_MS + 1);
  provider.lookupResult = { kind: "completed", providerReceiptId: "receipt-recovered-while-disabled" };
  const disabledConfig = config({ enabled: false });
  const recovered = await reconcileCodexBankedReset(
    candidate({ requestId: "disabled-recovery" }),
    dependencies(kv, provider, clock, disabledConfig, {}, () => disabledConfig),
  );

  assert.equal(recovered.kind, "verified");
  assert.equal(recovered.record?.state, "verified");
  assert.equal(provider.redeemInputs.length, 1);
  assert.equal(provider.lookupInputs.length, 1);
  assert.equal(provider.commitCount, 1);
});

Deno.test("seeded state-machine invariant: one account/window never reaches more than one fake provider commit", async () => {
  let seed = 0x5eed_c0de;
  const next = (): number => {
    seed = (Math.imul(seed, 1_664_525) + 1_013_904_223) >>> 0;
    return seed;
  };

  for (let index = 0; index < 24; index += 1) {
    const caseSeed = next();
    const accountId = `property-account-${index}`;
    const reset = candidate({
      accountId,
      credentialVersion: `property-credential-${caseSeed}`,
      quotaResetAtMs: 1_700_000_060_000 + index,
      routingGeneration: caseSeed % 1_000,
      requestId: `property-${caseSeed}`,
    });
    const kv = new MemoryKv();
    const provider = new FakeCodexUsageResetProvider();
    const clock = new TestClock();
    await seedFences(kv, reset);
    const deps = dependencies(kv, provider, clock, config({ accountAllowlist: new Set([accountId]) }));
    const scenario = caseSeed % 3;

    if (scenario === 1) {
      provider.redeemResult = { kind: "unknown", providerReceiptId: null };
      provider.commitOnRedeem = true;
      const first = await attemptCodexBankedReset(reset, deps);
      assert.equal(first.kind, "pending", `seed ${caseSeed}`);
      clock.advance(CODEX_BANKED_RESET_LEASE_MS + 1);
      provider.lookupResult = { kind: "completed", providerReceiptId: `receipt-${caseSeed}` };
      await reconcileCodexBankedReset(candidate({ ...reset, requestId: `recovery-${caseSeed}` }), deps);
    } else if (scenario === 2) {
      provider.redeemResult = { kind: "rejected", reason: `rejected-${caseSeed}` };
      await attemptCodexBankedReset(reset, deps);
      await attemptCodexBankedReset(candidate({ ...reset, requestId: `duplicate-${caseSeed}` }), deps);
    } else {
      await attemptCodexBankedReset(reset, deps);
      await attemptCodexBankedReset(candidate({ ...reset, requestId: `duplicate-${caseSeed}` }), deps);
    }

    const idempotencyKeys = new Set(provider.redeemInputs.map((input) => input.idempotencyKey));
    assert.ok(provider.commitCount <= 1, `seed ${caseSeed} committed more than once`);
    assert.ok(provider.redeemInputs.length <= 1, `seed ${caseSeed} submitted more than once`);
    assert.ok(idempotencyKeys.size <= 1, `seed ${caseSeed} generated multiple idempotency keys`);
  }
});

Deno.test("generated banked-reset event sequences retain the durable state-machine invariants", async () => {
  type Event =
    | "request"
    | "qualifying_429"
    | "claim"
    | "submit"
    | "provider_commit"
    | "response_loss"
    | "lookup"
    | "verify"
    | "retry"
    | "crash"
    | "lease_expire"
    | "credential_rotate"
    | "kv_failure";

  const allEvents: readonly Event[] = [
    "request",
    "qualifying_429",
    "claim",
    "submit",
    "provider_commit",
    "response_loss",
    "lookup",
    "verify",
    "retry",
    "crash",
    "lease_expire",
    "credential_rotate",
    "kv_failure",
  ];
  const sequenceCount = 48;
  let seed = 0x41c6_0de5;
  const next = (): number => {
    seed = (Math.imul(seed, 1_103_515_245) + 12_345) >>> 0;
    return seed;
  };

  for (let sequenceIndex = 0; sequenceIndex < sequenceCount; sequenceIndex += 1) {
    const sequenceSeed = next();
    let random = sequenceSeed;
    const choose = (): number => {
      random = (Math.imul(random, 1_664_525) + 1_013_904_223) >>> 0;
      return random;
    };
    // Each reproducible sequence includes the full model alphabet before a
    // deterministic shuffle/repetition, so a failing seed identifies the
    // exact interleaving without relying on a flaky random test runner.
    const sequence = [...allEvents];
    for (let index = sequence.length - 1; index > 0; index -= 1) {
      const swap = choose() % (index + 1);
      [sequence[index], sequence[swap]] = [sequence[swap]!, sequence[index]!];
    }
    for (let index = 0; index < 11; index += 1) sequence.push(allEvents[choose() % allEvents.length]!);

    const accountId = `generated-account-${sequenceIndex}`;
    const reset = candidate({
      accountId,
      credentialVersion: `generated-credential-${sequenceSeed}`,
      quotaResetAtMs: 1_700_000_060_000 + sequenceIndex,
      routingGeneration: sequenceSeed % 1000,
      requestId: `generated-${sequenceSeed}`,
    });
    const kv = new MemoryKv();
    const provider = new FakeCodexUsageResetProvider();
    const clock = new TestClock();
    const modes: readonly CodexBankedResetConfig["mode"][] = ["live", "shadow", "disabled"];
    const mode = modes[sequenceSeed % modes.length]!;
    const configured = config({
      mode,
      enabled: mode === "live" || mode === "shadow",
      accountAllowlist: new Set([accountId]),
    });
    const deps = dependencies(kv, provider, clock, configured);
    await seedFences(kv, reset);
    provider.redeemResult = clone(sanitizedProviderFixtures.rate_limit);
    provider.commitOnRedeem = true;

    let lastOutcome: Awaited<ReturnType<typeof attemptCodexBankedReset>> | null = null;
    let postResetInferenceRetries = 0;
    let credentialWasRotated = false;

    for (const event of sequence) {
      const label = `seed ${sequenceSeed} event ${event}`;
      switch (event) {
        case "request":
        case "qualifying_429":
        case "claim":
        case "submit":
          lastOutcome = await attemptCodexBankedReset(reset, deps);
          break;
        case "provider_commit":
          provider.redeemResult = clone(sanitizedProviderFixtures.rate_limit);
          provider.commitOnRedeem = true;
          lastOutcome = await attemptCodexBankedReset(reset, deps);
          break;
        case "response_loss":
          provider.redeemFailureAfterCommit = new Error(`response loss ${sequenceSeed}`);
          lastOutcome = await attemptCodexBankedReset(reset, deps);
          provider.redeemFailureAfterCommit = null;
          break;
        case "lookup":
          clock.advance(CODEX_BANKED_RESET_LEASE_MS + 1);
          provider.lookupResult = clone(sanitizedProviderFixtures.lookup_pending);
          lastOutcome = await reconcileCodexBankedReset(reset, deps);
          break;
        case "verify":
          clock.advance(CODEX_BANKED_RESET_LEASE_MS + 1);
          provider.lookupResult = clone(sanitizedProviderFixtures.lookup_completed);
          provider.verifyResult = true;
          lastOutcome = await reconcileCodexBankedReset(reset, deps);
          break;
        case "retry":
          // The state machine only grants the gateway a retry permit through
          // a verified outcome. The gateway integration separately proves
          // that it consumes this permit at most once.
          if (lastOutcome?.kind === "verified" && postResetInferenceRetries === 0) {
            assert.equal(lastOutcome.record?.state, "verified", label);
            postResetInferenceRetries += 1;
          }
          assert.ok(postResetInferenceRetries <= 1, label);
          break;
        case "crash":
        case "lease_expire":
          clock.advance(CODEX_BANKED_RESET_LEASE_MS + 1);
          break;
        case "credential_rotate":
          credentialWasRotated = true;
          await kv.set(credentialFenceKey(accountId), {
            kind: "credential",
            credential_version: `rotated-${sequenceSeed}`,
          });
          break;
        case "kv_failure": {
          kv.getFailure = new Error(`generated KV outage ${sequenceSeed}`);
          const failure = await attemptCodexBankedReset(reset, deps);
          assert.notEqual(failure.kind, "verified", label);
          kv.getFailure = null;
          break;
        }
      }

      const idempotencyKeys = new Set(provider.redeemInputs.map((input) => input.idempotencyKey));
      assert.ok(provider.commitCount <= 1, `${label}: more than one provider commit`);
      assert.ok(provider.redeemInputs.length <= 1, `${label}: more than one submission`);
      assert.ok(idempotencyKeys.size <= 1, `${label}: different idempotency keys`);
      if (mode === "disabled" || mode === "shadow") {
        assert.equal(provider.commitCount, 0, `${label}: inactive mode committed`);
      }
      if (credentialWasRotated) {
        // A rotated fence may still permit provider-level reconciliation of a
        // prior unknown record, but it cannot start another submission.
        assert.ok(provider.redeemInputs.length <= 1, `${label}: stale fence submitted again`);
      }
    }
  }
});

Deno.test("global daily cap stops a second account before it reaches the provider", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const configured = config({ accountAllowlist: new Set(["test-account-a", "test-account-b"]), maxGlobalPerDay: 1 });
  const deps = dependencies(kv, provider, clock, configured);
  const firstCandidate = candidate();
  const secondCandidate = candidate({ accountId: "test-account-b", requestId: "second-account" });
  await seedFences(kv, firstCandidate);
  await seedFences(kv, secondCandidate);
  const first = await attemptCodexBankedReset(firstCandidate, deps);
  const second = await attemptCodexBankedReset(
    secondCandidate,
    deps,
  );

  assert.equal(first.kind, "verified");
  assert.equal(second.kind, "skipped");
  assert.equal(second.reason, "global_limit_reached");
  assert.equal(provider.redeemInputs.length, 1);
  assert.equal(provider.commitCount, 1);
});

Deno.test("empty or unsupported inventory and provider rejection become durable terminal rejections", async () => {
  const scenarios: Array<
    Readonly<{
      name: string;
      configure: (provider: FakeCodexUsageResetProvider) => void;
      reason: string;
      redeemCalls: number;
    }>
  > = [
    {
      name: "empty inventory",
      configure: (provider) => {
        provider.inventory = { availableCount: 0, observedAtMs: 1_700_000_000_000, resetType: "banked_reset" };
      },
      reason: "inventory_empty",
      redeemCalls: 0,
    },
    {
      name: "unsupported inventory type",
      configure: (provider) => {
        provider.inventory = { availableCount: 1, observedAtMs: 1_700_000_000_000, resetType: "unreviewed_reset" };
      },
      reason: "inventory_response_invalid_or_unsupported",
      redeemCalls: 0,
    },
    {
      name: "provider rejection",
      configure: (provider) => {
        provider.redeemResult = { kind: "rejected", reason: "provider validation rejected the request" };
      },
      reason: "provider_rejected",
      redeemCalls: 1,
    },
  ];

  for (const scenario of scenarios) {
    const kv = new MemoryKv();
    const provider = new FakeCodexUsageResetProvider();
    const clock = new TestClock();
    scenario.configure(provider);
    const deps = dependencies(kv, provider, clock);
    const reset = candidate();
    await seedFences(kv, reset);
    const first = await attemptCodexBankedReset(reset, deps);
    const repeat = await attemptCodexBankedReset(
      candidate({ requestId: "repeat-rejection" }),
      deps,
    );

    assert.equal(first.kind, "rejected", scenario.name);
    assert.equal(first.reason, scenario.reason, scenario.name);
    assert.equal(first.record?.state, "rejected", scenario.name);
    assert.equal(repeat.kind, "rejected", scenario.name);
    assert.equal(provider.redeemInputs.length, scenario.redeemCalls, scenario.name);
    assert.equal(provider.commitCount, 0, scenario.name);
  }
});

Deno.test("stale or future inventory cannot authorize a redemption", async () => {
  const cases: Array<Readonly<{ name: string; observedAtDeltaMs: number }>> = [
    { name: "stale", observedAtDeltaMs: -(CODEX_BANKED_RESET_INVENTORY_MAX_AGE_MS + 1) },
    { name: "future", observedAtDeltaMs: 1 },
  ];
  for (const testCase of cases) {
    const kv = new MemoryKv();
    const provider = new FakeCodexUsageResetProvider();
    const clock = new TestClock();
    const reset = candidate({ requestId: `inventory-${testCase.name}` });
    await seedFences(kv, reset);
    provider.inventory = {
      ...provider.inventory,
      observedAtMs: clock.nowMs + testCase.observedAtDeltaMs,
    };

    const result = await attemptCodexBankedReset(reset, dependencies(kv, provider, clock));
    assert.equal(result.kind, "rejected", testCase.name);
    assert.equal(result.reason, "inventory_response_invalid_or_unsupported", testCase.name);
    assert.equal(provider.inventoryInputs.length, 1, testCase.name);
    assert.equal(provider.redeemInputs.length, 0, testCase.name);
    assert.equal(provider.commitCount, 0, testCase.name);
  }
});

Deno.test("KV outage and claim CAS exhaustion fail closed before any provider interaction", async () => {
  const clock = new TestClock();

  const unavailableKv = new MemoryKv();
  const unavailableCandidate = candidate();
  await seedFences(unavailableKv, unavailableCandidate);
  unavailableKv.getFailure = new Error("in-memory KV unavailable");
  const unavailableProvider = new FakeCodexUsageResetProvider();
  const unavailable = await attemptCodexBankedReset(
    unavailableCandidate,
    dependencies(unavailableKv, unavailableProvider, clock),
  );
  assert.equal(unavailable.kind, "skipped");
  assert.equal(unavailable.reason, "kv_unavailable");
  assert.equal(unavailableProvider.callCount, 0);
  assert.equal(unavailableProvider.commitCount, 0);

  const casKv = new MemoryKv();
  casKv.failCommitNumbers.add(1);
  casKv.failCommitNumbers.add(2);
  casKv.failCommitNumbers.add(3);
  casKv.failCommitNumbers.add(4);
  const casProvider = new FakeCodexUsageResetProvider();
  const casCandidate = candidate();
  await seedFences(casKv, casCandidate);
  const exhausted = await attemptCodexBankedReset(casCandidate, dependencies(casKv, casProvider, clock));
  assert.equal(exhausted.kind, "skipped");
  assert.equal(exhausted.reason, "kv_cas_exhausted");
  assert.equal(casKv.atomicCommitCount, 4);
  assert.equal(casProvider.callCount, 0);
  assert.equal(casProvider.commitCount, 0);
});

Deno.test("a transient CAS conflict at every happy-path transition retains one logical redemption", async () => {
  // A normal successful path has five durable writes: claim, submitted,
  // final renewal, receipt persistence, and verification. Each operation
  // retries its own CAS conflict without issuing another provider mutation or
  // changing the deterministic idempotency key.
  for (const failedCommit of [1, 2, 3, 4, 5]) {
    const kv = new MemoryKv();
    const provider = new FakeCodexUsageResetProvider();
    const clock = new TestClock();
    const reset = candidate({ requestId: `transient-cas-${failedCommit}` });
    await seedFences(kv, reset);
    kv.failCommitNumbers.add(failedCommit);

    const result = await attemptCodexBankedReset(reset, dependencies(kv, provider, clock));
    assert.equal(result.kind, "verified", `failed durable commit ${failedCommit}`);
    assert.equal(provider.commitCount, 1, `failed durable commit ${failedCommit}`);
    assert.equal(provider.redeemInputs.length, 1, `failed durable commit ${failedCommit}`);
    assert.equal(
      new Set(provider.redeemInputs.map((input) => input.idempotencyKey)).size,
      1,
      `failed durable commit ${failedCommit}`,
    );
  }
});

Deno.test("KV unavailability after claim, after submission, and during verification never opens a second spend path", async () => {
  {
    const kv = new MemoryKv();
    const provider = new FakeCodexUsageResetProvider();
    const clock = new TestClock();
    const reset = candidate({ requestId: "kv-after-claim" });
    await seedFences(kv, reset);
    const entered = new Deferred<void>();
    const gate = new Deferred<void>();
    provider.inventoryEntered = entered;
    provider.inventoryGate = gate.promise;
    const pending = attemptCodexBankedReset(reset, dependencies(kv, provider, clock));
    await entered.promise;
    kv.getFailure = new Error("KV failed after claim");
    gate.resolve(undefined);
    const outcome = await pending;

    assert.equal(outcome.kind, "pending");
    assert.equal(outcome.reason, "kv_unavailable");
    assert.equal(outcome.record?.state, "claimed");
    assert.equal(provider.redeemInputs.length, 0);
  }

  {
    const kv = new MemoryKv();
    const provider = new FakeCodexUsageResetProvider();
    const clock = new TestClock();
    const reset = candidate({ requestId: "kv-after-submission" });
    await seedFences(kv, reset);
    kv.beforeAtomicCommit = (commitNumber) => {
      if (commitNumber === 2) kv.getFailure = new Error("KV failed after submitted transition");
    };
    const outcome = await attemptCodexBankedReset(reset, dependencies(kv, provider, clock));

    assert.equal(outcome.kind, "pending");
    assert.equal(outcome.reason, "kv_unavailable");
    assert.equal(outcome.record?.state, "submitted");
    assert.equal(provider.redeemInputs.length, 0);
  }

  {
    const kv = new MemoryKv();
    const provider = new FakeCodexUsageResetProvider();
    const clock = new TestClock();
    const reset = candidate({ requestId: "kv-during-verification" });
    await seedFences(kv, reset);
    const entered = new Deferred<void>();
    const gate = new Deferred<void>();
    provider.verifyEntered = entered;
    provider.verifyGate = gate.promise;
    const pending = attemptCodexBankedReset(reset, dependencies(kv, provider, clock));
    await entered.promise;
    kv.getFailure = new Error("KV failed while finalizing verification");
    gate.resolve(undefined);
    const outcome = await pending;

    assert.equal(outcome.kind, "pending");
    assert.equal(outcome.reason, "verification_cas_failed");
    assert.equal(outcome.record?.state, "submitted");
    assert.equal(provider.redeemInputs.length, 1);
    assert.equal(provider.commitCount, 1);
  }
});

Deno.test("a stale owner cannot finalize verified after a lease-takeover reconciliation", async () => {
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const reset = candidate({ requestId: "stale-owner-verify" });
  await seedFences(kv, reset);
  const verifyEntered = new Deferred<void>();
  const verifyGate = new Deferred<void>();
  provider.verifyEntered = verifyEntered;
  provider.verifyGate = verifyGate.promise;
  const deps = dependencies(kv, provider, clock);
  const original = attemptCodexBankedReset(reset, deps);
  await verifyEntered.promise;

  clock.advance(CODEX_BANKED_RESET_LEASE_MS + 1);
  provider.lookupResult = clone(sanitizedProviderFixtures.lookup_pending);
  provider.verifyResult = false;
  const takeover = await reconcileCodexBankedReset(
    candidate({ requestId: "stale-owner-verify-takeover" }),
    deps,
  );
  assert.equal(takeover.kind, "pending");
  assert.equal(takeover.record?.state, "unknown");
  assert.equal(takeover.record?.fence, 3);

  provider.verifyResult = true;
  verifyGate.resolve(undefined);
  const stale = await original;
  assert.equal(stale.kind, "pending");
  assert.equal(stale.reason, "verification_cas_failed");
  assert.equal(provider.redeemInputs.length, 1);
  assert.equal(
    kv.value<CodexResetRedemptionRecord>(
      codexResetRedemptionKey(takeover.accountIdHash!, takeover.quotaGeneration!),
    )?.state,
    "unknown",
  );
});

Deno.test("receipt CAS loss leaves the transaction pending and recovery uses lookup instead of resubmission", async () => {
  const kv = new MemoryKv();
  // Claim, submitted state, and the final pre-redeem lease renewal are commits
  // 1 through 3. Exhaust every receipt-persist CAS retry.
  for (const commit of [4, 5, 6, 7]) kv.failCommitNumbers.add(commit);
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const deps = dependencies(kv, provider, clock);
  const reset = candidate();
  await seedFences(kv, reset);
  const first = await attemptCodexBankedReset(reset, deps);

  assert.equal(first.kind, "pending");
  assert.equal(first.reason, "receipt_cas_failed");
  assert.equal(provider.redeemInputs.length, 1);
  assert.equal(provider.commitCount, 1);
  assert.equal(provider.verificationInputs.length, 0);

  clock.advance(CODEX_BANKED_RESET_LEASE_MS + 1);
  provider.lookupResult = { kind: "completed", providerReceiptId: "receipt-after-cas-recovery" };
  const recovered = await attemptCodexBankedReset(
    candidate({ requestId: "cas-recovery" }),
    deps,
  );
  assert.equal(recovered.kind, "verified");
  assert.equal(provider.redeemInputs.length, 1);
  assert.equal(provider.lookupInputs.length, 1);
  assert.equal(provider.commitCount, 1);
});

Deno.test("sanitized provider fixtures accept only complete known boundary results", async () => {
  const successfulFixtures: Array<
    Readonly<{
      name: string;
      result: RedeemResetResult;
      expectedCommitCount: number;
      expectedReceipt: string;
    }>
  > = [
    {
      name: "completed",
      result: sanitizedProviderFixtures.redemption_completed,
      expectedCommitCount: 1,
      expectedReceipt: "fixture-completed",
    },
    {
      name: "accepted",
      result: sanitizedProviderFixtures.redemption_accepted,
      expectedCommitCount: 1,
      expectedReceipt: "fixture-accepted",
    },
    {
      name: "idempotent replay",
      result: sanitizedProviderFixtures.redemption_already_redeemed,
      expectedCommitCount: 0,
      expectedReceipt: "fixture-replay",
    },
  ];

  for (const fixture of successfulFixtures) {
    const kv = new MemoryKv();
    const provider = new FakeCodexUsageResetProvider();
    const clock = new TestClock();
    const reset = candidate({ requestId: `fixture-${fixture.name}` });
    await seedFences(kv, reset);
    provider.inventory = clone(sanitizedProviderFixtures.inventory_available);
    provider.redeemResult = clone(fixture.result);
    const outcome = await attemptCodexBankedReset(reset, dependencies(kv, provider, clock));

    assert.equal(outcome.kind, "verified", fixture.name);
    assert.equal(outcome.record?.provider_receipt_id, fixture.expectedReceipt, fixture.name);
    assert.equal(provider.commitCount, fixture.expectedCommitCount, fixture.name);
    assert.equal(provider.redeemInputs.length, 1, fixture.name);
  }

  for (
    const fixture of [
      sanitizedProviderFixtures.malformed_success,
      sanitizedProviderFixtures.schema_drift,
    ]
  ) {
    const kv = new MemoryKv();
    const provider = new FakeCodexUsageResetProvider();
    const clock = new TestClock();
    const reset = candidate({ requestId: `fixture-malformed-${fixture.kind}` });
    await seedFences(kv, reset);
    provider.redeemResult = fixture as unknown as RedeemResetResult;
    const outcome = await attemptCodexBankedReset(reset, dependencies(kv, provider, clock));

    assert.equal(outcome.kind, "pending", fixture.kind);
    assert.equal(outcome.reason, "submit_response_invalid", fixture.kind);
    assert.equal(outcome.record?.state, "unknown", fixture.kind);
    assert.equal(provider.verificationInputs.length, 0, fixture.kind);
  }

  for (
    const lookupFixture of [
      sanitizedProviderFixtures.lookup_pending,
      sanitizedProviderFixtures.lookup_rejected,
      sanitizedProviderFixtures.lookup_not_found,
    ]
  ) {
    const kv = new MemoryKv();
    const provider = new FakeCodexUsageResetProvider();
    const clock = new TestClock();
    const reset = candidate({ requestId: `fixture-lookup-${lookupFixture.kind}` });
    await seedFences(kv, reset);
    provider.redeemResult = clone(sanitizedProviderFixtures.rate_limit);
    provider.commitOnRedeem = true;
    const first = await attemptCodexBankedReset(reset, dependencies(kv, provider, clock));
    assert.equal(first.kind, "pending", lookupFixture.kind);
    const stableKey = provider.redeemInputs[0]?.idempotencyKey;
    clock.advance(CODEX_BANKED_RESET_LEASE_MS + 1);
    provider.lookupResult = clone(lookupFixture);
    provider.verifyResult = false;
    const recovered = await reconcileCodexBankedReset(
      candidate({ requestId: `fixture-lookup-recovery-${lookupFixture.kind}` }),
      dependencies(kv, provider, clock),
    );

    assert.equal(recovered.kind, "pending", lookupFixture.kind);
    assert.equal(recovered.record?.state, "unknown", lookupFixture.kind);
    assert.equal(provider.redeemInputs.length, 1, lookupFixture.kind);
    assert.equal(provider.lookupInputs[0]?.idempotencyKey, stableKey, lookupFixture.kind);
  }
});

Deno.test("provider rejection matrix keeps definitive failures terminal and ambiguous failures unknown", async () => {
  const cases: Array<
    Readonly<{
      name: string;
      configure: (provider: FakeCodexUsageResetProvider) => void;
      expectedKind: "pending" | "rejected";
      expectedState: "unknown" | "rejected";
      expectedReason: string;
    }>
  > = [
    {
      name: "reset allowance exhausted",
      configure: (provider) => {
        provider.inventory = clone(sanitizedProviderFixtures.inventory_empty);
      },
      expectedKind: "rejected",
      expectedState: "rejected",
      expectedReason: "inventory_empty",
    },
    {
      name: "provider authentication failure",
      configure: (provider) => {
        provider.redeemResult = clone(sanitizedProviderFixtures.authentication_error);
      },
      expectedKind: "rejected",
      expectedState: "rejected",
      expectedReason: "provider_rejected",
    },
    {
      name: "provider validation failure",
      configure: (provider) => {
        provider.redeemResult = {
          kind: "rejected",
          reason: "fixture-provider-validation-failure",
        };
      },
      expectedKind: "rejected",
      expectedState: "rejected",
      expectedReason: "provider_rejected",
    },
    {
      name: "provider rate limit with unknown commit status",
      configure: (provider) => {
        provider.redeemResult = clone(sanitizedProviderFixtures.rate_limit);
        provider.commitOnRedeem = true;
      },
      expectedKind: "pending",
      expectedState: "unknown",
      expectedReason: "provider_commit_unknown",
    },
    {
      name: "provider server failure with unknown commit status",
      configure: (provider) => {
        provider.redeemResult = clone(sanitizedProviderFixtures.server_error);
        provider.commitOnRedeem = true;
      },
      expectedKind: "pending",
      expectedState: "unknown",
      expectedReason: "provider_commit_unknown",
    },
  ];

  for (const testCase of cases) {
    const kv = new MemoryKv();
    const provider = new FakeCodexUsageResetProvider();
    const clock = new TestClock();
    const reset = candidate({ requestId: `rejection-${testCase.name}` });
    await seedFences(kv, reset);
    testCase.configure(provider);
    const deps = dependencies(kv, provider, clock);
    const first = await attemptCodexBankedReset(reset, deps);
    const repeat = await attemptCodexBankedReset(
      candidate({ requestId: `rejection-repeat-${testCase.name}` }),
      deps,
    );

    assert.equal(first.kind, testCase.expectedKind, testCase.name);
    assert.equal(first.reason, testCase.expectedReason, testCase.name);
    assert.equal(first.record?.state, testCase.expectedState, testCase.name);
    assert.equal(provider.redeemInputs.length, testCase.name === "reset allowance exhausted" ? 0 : 1, testCase.name);
    assert.notEqual(repeat.kind, "verified", testCase.name);
    assert.equal(provider.redeemInputs.length, testCase.name === "reset allowance exhausted" ? 0 : 1, testCase.name);
  }
});

Deno.test("timeout and crash matrix never creates a second logical redemption", async () => {
  const timeoutCases: Array<
    Readonly<{
      name: string;
      configure: (provider: FakeCodexUsageResetProvider) => void;
      expectedTimeoutStage: FakeProviderCall["timeoutStage"];
      expectedReason: string;
      expectedCommitCount: number;
    }>
  > = [
    {
      name: "timeout before provider receives a commit",
      configure: (provider) => {
        provider.redeemFailure = new DOMException("timeout before provider commit", "TimeoutError");
      },
      expectedTimeoutStage: "before_provider_commit",
      expectedReason: "submit_transport_unknown",
      expectedCommitCount: 0,
    },
    {
      name: "provider commits and its response is lost",
      configure: (provider) => {
        provider.redeemFailureAfterCommit = new DOMException("response lost", "TimeoutError");
      },
      expectedTimeoutStage: "after_provider_commit",
      expectedReason: "submit_transport_unknown",
      expectedCommitCount: 1,
    },
    {
      name: "verification times out after a confirmed submission",
      configure: (provider) => {
        provider.verifyFailure = new DOMException("verification timeout", "TimeoutError");
      },
      expectedTimeoutStage: null,
      expectedReason: "verification_unavailable",
      expectedCommitCount: 1,
    },
  ];

  for (const testCase of timeoutCases) {
    const kv = new MemoryKv();
    const provider = new FakeCodexUsageResetProvider();
    const clock = new TestClock();
    const reset = candidate({ requestId: `timeout-${testCase.name}` });
    await seedFences(kv, reset);
    testCase.configure(provider);
    const deps = dependencies(kv, provider, clock);
    const first = await attemptCodexBankedReset(reset, deps);

    assert.equal(first.kind, "pending", testCase.name);
    assert.equal(first.reason, testCase.expectedReason, testCase.name);
    assert.equal(first.record?.state, "unknown", testCase.name);
    assert.equal(provider.commitCount, testCase.expectedCommitCount, testCase.name);
    assert.equal(provider.redeemInputs.length, 1, testCase.name);
    const stableKey = provider.redeemInputs[0]?.idempotencyKey;
    const submission = provider.calls.find((call) => call.method === "redeem");
    assert.equal(submission?.timeoutStage, testCase.expectedTimeoutStage, testCase.name);

    provider.redeemFailure = null;
    provider.redeemFailureAfterCommit = null;
    provider.verifyFailure = null;
    provider.lookupResult = clone(sanitizedProviderFixtures.lookup_completed);
    provider.verifyResult = true;
    clock.advance(CODEX_BANKED_RESET_LEASE_MS + 1);
    const recovered = await reconcileCodexBankedReset(
      candidate({ requestId: `timeout-recovery-${testCase.name}` }),
      deps,
    );

    assert.equal(recovered.kind, "verified", testCase.name);
    assert.equal(provider.redeemInputs.length, 1, testCase.name);
    assert.equal(provider.lookupInputs.at(-1)?.idempotencyKey, stableKey, testCase.name);
    assert.equal(provider.commitCount, testCase.expectedCommitCount, testCase.name);
  }

  // Pause after receipt persistence, as if the isolate died before it could
  // report verification. A later worker may reconcile, but never resubmit.
  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider();
  const clock = new TestClock();
  const reset = candidate({ requestId: "receipt-persisted-then-crash" });
  await seedFences(kv, reset);
  const verifyGate = new Deferred<void>();
  const verifyEntered = new Deferred<void>();
  provider.verifyGate = verifyGate.promise;
  provider.verifyEntered = verifyEntered;
  const deps = dependencies(kv, provider, clock);
  const original = attemptCodexBankedReset(reset, deps);
  await verifyEntered.promise;
  const persisted = [...kv.entries.values()]
    .map((entry) => entry.value)
    .find((value) => isRecord(value) && value.state === "submitted");
  assert.equal((persisted as { provider_receipt_id?: unknown } | undefined)?.provider_receipt_id, "receipt-completed");
  assert.equal(provider.commitCount, 1);
  assert.equal(provider.redeemInputs.length, 1);
  clock.advance(CODEX_BANKED_RESET_LEASE_MS + 1);
  provider.lookupResult = clone(sanitizedProviderFixtures.lookup_completed);
  provider.verifyResult = false;
  const recovery = await reconcileCodexBankedReset(
    candidate({ requestId: "receipt-persisted-recovery" }),
    deps,
  );
  assert.equal(recovery.kind, "pending");
  assert.equal(provider.redeemInputs.length, 1);
  provider.verifyResult = true;
  verifyGate.resolve(undefined);
  const stale = await original;
  assert.equal(stale.kind, "pending");
  assert.equal(stale.reason, "verification_cas_failed");
  assert.equal(provider.redeemInputs.length, 1);
});

Deno.test("client aborts before submission and after a possible commit fail closed without resubmission", async () => {
  const beforeKv = new MemoryKv();
  const beforeProvider = new FakeCodexUsageResetProvider();
  const beforeClock = new TestClock();
  const beforeReset = candidate({ requestId: "abort-before-submission" });
  await seedFences(beforeKv, beforeReset);
  const beforeController = new AbortController();
  beforeController.abort(new DOMException("client cancelled", "AbortError"));
  const before = await attemptCodexBankedReset(
    candidate({ ...beforeReset, signal: beforeController.signal }),
    dependencies(beforeKv, beforeProvider, beforeClock),
  );
  assert.equal(before.kind, "rejected");
  assert.equal(before.reason, "client_aborted_before_submission");
  assert.equal(beforeProvider.redeemInputs.length, 0);

  const afterKv = new MemoryKv();
  const afterProvider = new FakeCodexUsageResetProvider();
  const afterClock = new TestClock();
  const afterReset = candidate({ requestId: "abort-after-possible-commit" });
  await seedFences(afterKv, afterReset);
  const afterController = new AbortController();
  const entered = new Deferred<void>();
  const gate = new Deferred<void>();
  afterProvider.redeemEntered = entered;
  afterProvider.redeemGate = gate.promise;
  const afterDeps = dependencies(afterKv, afterProvider, afterClock);
  const original = attemptCodexBankedReset(
    candidate({ ...afterReset, signal: afterController.signal }),
    afterDeps,
  );
  await entered.promise;
  afterController.abort(new DOMException("client cancelled after submit", "AbortError"));
  gate.resolve(undefined);
  const after = await original;
  assert.equal(after.kind, "pending");
  assert.equal(after.record?.state, "submitted");
  assert.equal(afterProvider.redeemInputs.length, 1);

  afterClock.advance(CODEX_BANKED_RESET_LEASE_MS + 1);
  afterProvider.lookupResult = clone(sanitizedProviderFixtures.lookup_completed);
  const recovered = await reconcileCodexBankedReset(
    candidate({ requestId: "abort-after-recovery" }),
    afterDeps,
  );
  assert.equal(recovered.kind, "verified");
  assert.equal(afterProvider.redeemInputs.length, 1);
});

Deno.test("config and durable-record parsers are strict, and an unproven provider contract keeps live mode disabled", async () => {
  const defaults = parseCodexBankedResetConfig(() => undefined);
  assert.deepEqual(
    {
      enabled: defaults.enabled,
      mode: defaults.mode,
      allowlist: [...defaults.accountAllowlist],
      maxGlobalPerDay: defaults.maxGlobalPerDay,
      maxPerAccountPerWindow: defaults.maxPerAccountPerWindow,
    },
    { enabled: false, mode: "disabled", allowlist: [], maxGlobalPerDay: 0, maxPerAccountPerWindow: 0 },
  );

  const environment = new Map<string, string>([
    ["CODEX_BANKED_RESET_ENABLED", " true "],
    ["CODEX_BANKED_RESET_MODE", " LIVE "],
    ["CODEX_BANKED_RESET_ACCOUNT_ALLOWLIST", " account-a, hash-a\naccount-b "],
    ["CODEX_BANKED_RESET_MAX_GLOBAL_PER_DAY", "2"],
    ["CODEX_BANKED_RESET_MAX_PER_ACCOUNT_PER_WINDOW", "1"],
  ]);
  const parsedConfig = parseCodexBankedResetConfig((key) => environment.get(key));
  assert.equal(parsedConfig.enabled, true);
  assert.equal(parsedConfig.mode, "live");
  assert.deepEqual([...parsedConfig.accountAllowlist], ["account-a", "hash-a", "account-b"]);
  assert.equal(parsedConfig.maxGlobalPerDay, 2);
  assert.equal(parsedConfig.maxPerAccountPerWindow, 1);
  assert.equal(parseCodexBankedResetConfig(() => "1").enabled, false);
  assert.equal(parseCodexBankedResetConfig(() => "1.5").maxGlobalPerDay, 0);

  const validRecord: CodexResetRedemptionRecord = {
    v: 1,
    account_id_hash: "account-hash",
    credential_version: "credential-v1",
    quota_generation: "generation-v1",
    routing_generation: 1,
    idempotency_key_hash: "idempotency-hash",
    state: "submitted",
    owner_token: "owner",
    fence: 1,
    lease_expires_at_ms: 1_700_000_030_000,
    provider_receipt_id: "receipt",
    created_at_ms: 1_700_000_000_000,
    updated_at_ms: 1_700_000_000_001,
    submitted_at_ms: 1_700_000_000_001,
    verified_at_ms: null,
    last_error_code: null,
  };
  assert.deepEqual(parseCodexResetRedemptionRecord(validRecord), validRecord);
  assert.equal(parseCodexResetRedemptionRecord({ ...validRecord, fence: -1 }), null);
  assert.equal(parseCodexResetRedemptionRecord({ ...validRecord, provider_receipt_id: "" }), null);
  assert.equal(parseCodexResetRedemptionRecord({ ...validRecord, state: "future_state" }), null);

  const semanticInvalidRecords: Array<Readonly<{ name: string; value: Record<string, unknown> }>> = [
    {
      name: "claimed cannot carry a submission timestamp",
      value: {
        ...validRecord,
        state: "claimed",
        submitted_at_ms: validRecord.created_at_ms,
        provider_receipt_id: null,
      },
    },
    {
      name: "claimed cannot carry a receipt",
      value: { ...validRecord, state: "claimed", submitted_at_ms: null },
    },
    {
      name: "submitted requires its submission timestamp",
      value: { ...validRecord, submitted_at_ms: null },
    },
    {
      name: "unknown requires a stable error code",
      value: { ...validRecord, state: "unknown", last_error_code: null },
    },
    {
      name: "verified requires a verified timestamp",
      value: { ...validRecord, state: "verified" },
    },
    {
      name: "verified cannot predate submission",
      value: { ...validRecord, state: "verified", verified_at_ms: validRecord.submitted_at_ms! - 1 },
    },
    {
      name: "rejected requires a stable error code",
      value: { ...validRecord, state: "rejected", submitted_at_ms: null, provider_receipt_id: null },
    },
    {
      name: "timestamps cannot regress",
      value: { ...validRecord, updated_at_ms: validRecord.created_at_ms - 1 },
    },
  ];
  for (const invalid of semanticInvalidRecords) {
    assert.equal(parseCodexResetRedemptionRecord(invalid.value), null, invalid.name);
  }

  const unprovenContract: CodexUsageResetProviderContract = {
    ...provenContract(),
    idempotency: { callerSupplied: false, retentionMs: null },
    lookup: { byIdempotencyKey: false, byProviderReceiptId: false },
    verification: { independentlyVerifiable: false },
    supportedResetTypes: [],
  };
  assert.equal(providerSupportsLiveRedemption(new FakeCodexUsageResetProvider()), true);
  assert.equal(providerSupportsLiveRedemption(unavailableCodexUsageResetProvider), false);
  assert.equal(providerSupportsLiveRedemption(new FakeCodexUsageResetProvider(unprovenContract)), false);
  assert.equal(providerSupportsResetType(new FakeCodexUsageResetProvider(), "banked_reset"), true);

  const malformedContractProvider = new FakeCodexUsageResetProvider();
  (malformedContractProvider as unknown as { contract: unknown }).contract = {
    idempotency: null,
    supportedResetTypes: "not-an-array",
  };
  assert.equal(providerSupportsLiveRedemption(malformedContractProvider), false);
  assert.equal(providerSupportsResetType(malformedContractProvider, "banked_reset"), false);

  const kv = new MemoryKv();
  const provider = new FakeCodexUsageResetProvider(unprovenContract);
  const result = await attemptCodexBankedReset(candidate(), dependencies(kv, provider, new TestClock()));
  assert.equal(result.kind, "skipped");
  assert.equal(result.reason, "provider_contract_unproven");
  assert.equal(provider.callCount, 0);
  assert.equal(provider.commitCount, 0);

  const malformedResult = await attemptCodexBankedReset(
    candidate({ requestId: "malformed-provider-contract" }),
    dependencies(new MemoryKv(), malformedContractProvider, new TestClock()),
  );
  assert.equal(malformedResult.kind, "skipped");
  assert.equal(malformedResult.reason, "provider_contract_unproven");
  assert.equal(malformedContractProvider.callCount, 0);
});
