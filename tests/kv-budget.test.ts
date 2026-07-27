import assert from "node:assert/strict";
import { sha256Base64Url } from "../src/utils.ts";

if (typeof Deno.KvU64 !== "function") {
  (Deno as unknown as { KvU64: typeof Deno.KvU64 }).KvU64 = class {
    constructor(readonly value: bigint) {}
  } as typeof Deno.KvU64;
}

const encodeKey = (key: Deno.KvKey): string => JSON.stringify(key);
const textEncoder = new TextEncoder();

const kvFingerprint = (value: unknown): string =>
  JSON.stringify(value, (_key, item) => typeof item === "bigint" ? `${item}n` : item);

const encodeBase64Url = (bytes: Uint8Array): string => {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
};

const encodeJsonBase64Url = (value: unknown): string => encodeBase64Url(textEncoder.encode(JSON.stringify(value)));

const toPublicKeyPem = (spki: Uint8Array): string => {
  let binary = "";
  for (const byte of spki) binary += String.fromCharCode(byte);
  const lines = btoa(binary).match(/.{1,64}/g) ?? [];
  return `-----BEGIN PUBLIC KEY-----\n${lines.join("\n")}\n-----END PUBLIC KEY-----`;
};

class CountingKv {
  readonly values = new Map<string, unknown>();
  private readonly versions = new Map<string, { fingerprint: string; revision: number }>();
  private nextRevision = 1;
  reads = 0;
  readUnits = 0;
  writes = 0;
  sums = 0;
  sumCommitAttempts = 0;
  failNextSumCommits = 0;
  failNextCommits = 0;
  failApiKeyV3Reads = false;
  sumCommitDelayMs = 0;
  apiKeyV3DispatchCommitGate: Promise<void> | null = null;
  onApiKeyV3DispatchCommit: (() => void) | null = null;
  retries = 0;
  listCalls = 0;
  readonly readKeys: Deno.KvKey[] = [];

  private versionstamp(key: Deno.KvKey): string | null {
    const encoded = encodeKey(key);
    if (!this.values.has(encoded)) return null;
    const fingerprint = kvFingerprint(this.values.get(encoded));
    const existing = this.versions.get(encoded);
    if (!existing || existing.fingerprint !== fingerprint) {
      const revision = this.nextRevision++;
      this.versions.set(encoded, { fingerprint, revision });
      return String(revision).padStart(20, "0");
    }
    return String(existing.revision).padStart(20, "0");
  }

  private write(key: Deno.KvKey, value: unknown): void {
    const encoded = encodeKey(key);
    this.values.set(encoded, value);
    this.versions.set(encoded, { fingerprint: kvFingerprint(value), revision: this.nextRevision++ });
  }

  private remove(key: Deno.KvKey): void {
    const encoded = encodeKey(key);
    this.values.delete(encoded);
    this.versions.delete(encoded);
    this.nextRevision += 1;
  }

  resetCounts(): void {
    this.reads = 0;
    this.readUnits = 0;
    this.writes = 0;
    this.sums = 0;
    this.sumCommitAttempts = 0;
    this.failNextSumCommits = 0;
    this.failNextCommits = 0;
    this.sumCommitDelayMs = 0;
    this.apiKeyV3DispatchCommitGate = null;
    this.onApiKeyV3DispatchCommit = null;
    this.retries = 0;
    this.listCalls = 0;
    this.readKeys.length = 0;
  }

  get<T>(key: Deno.KvKey, _options?: { consistency?: "strong" | "eventual" }): Promise<Deno.KvEntryMaybe<T>> {
    if (
      this.failApiKeyV3Reads && key[0] === "uos_ai" && key[1] === "api_key_usage" && key[2] === "v3"
    ) {
      return Promise.reject(new Error("injected API-key V3 ledger read failure"));
    }
    this.reads += 1;
    this.readKeys.push(key);
    const value = this.values.get(encodeKey(key)) as T | undefined;
    const bytes = value === undefined
      ? 0
      : new TextEncoder().encode(JSON.stringify(value, (_, item) => typeof item === "bigint" ? item.toString() : item))
        .length;
    this.readUnits += Math.max(1, Math.ceil(bytes / 4096));
    return Promise.resolve({
      key,
      value: value ?? null,
      versionstamp: this.versionstamp(key),
    } as Deno.KvEntryMaybe<T>);
  }

  set(key: Deno.KvKey, value: unknown): Promise<Deno.KvCommitResult> {
    this.write(key, value);
    this.writes += 1;
    return Promise.resolve({ ok: true, versionstamp: "00000000000000000001" });
  }

  delete(key: Deno.KvKey): Promise<void> {
    this.remove(key);
    this.writes += 1;
    return Promise.resolve();
  }

  list<T>(selector: Deno.KvListSelector): Deno.KvListIterator<T> {
    this.listCalls += 1;
    const prefix = "prefix" in selector ? selector.prefix : [];
    const entries = [...this.values].filter(([encoded]) => {
      const key = JSON.parse(encoded) as Deno.KvKey;
      return prefix.every((part, index) => part === key[index]);
    });
    return (async function* () {
      for (const [encoded, value] of entries) {
        const key = JSON.parse(encoded) as Deno.KvKey;
        yield { key, value, versionstamp: "00000000000000000001" } as Deno.KvEntry<T>;
      }
    })() as unknown as Deno.KvListIterator<T>;
  }

  atomic(): Deno.AtomicOperation {
    const checks: Deno.KvEntryMaybe<unknown>[] = [];
    const mutations: Array<
      | { kind: "set"; key: Deno.KvKey; value: unknown }
      | { kind: "delete"; key: Deno.KvKey }
      | { kind: "sum"; key: Deno.KvKey; value: bigint }
    > = [];
    const operation = {
      check: (entry: Deno.KvEntryMaybe<unknown>) => {
        checks.push(entry);
        return operation;
      },
      set: (key: Deno.KvKey, value: unknown) => {
        mutations.push({ kind: "set", key, value });
        return operation;
      },
      delete: (key: Deno.KvKey) => {
        mutations.push({ kind: "delete", key });
        return operation;
      },
      sum: (key: Deno.KvKey, value: bigint) => {
        mutations.push({ kind: "sum", key, value });
        return operation;
      },
      commit: async () => {
        if (this.failNextCommits > 0) {
          this.failNextCommits -= 1;
          return { ok: false, versionstamp: null };
        }
        for (const entry of checks) {
          if (this.versionstamp(entry.key) !== entry.versionstamp) {
            return { ok: false, versionstamp: null };
          }
        }
        const hasSum = mutations.some((mutation) => mutation.kind === "sum");
        if (hasSum) {
          this.sumCommitAttempts += 1;
          if (this.failNextSumCommits > 0) {
            this.failNextSumCommits -= 1;
            throw new Error("injected API-key usage sum failure");
          }
        }
        if (hasSum && this.sumCommitDelayMs > 0) {
          await new Promise<void>((resolve) => setTimeout(resolve, this.sumCommitDelayMs));
        }
        const apiKeyV3Dispatch = mutations.some((mutation) =>
          mutation.kind === "set" &&
          mutation.key[0] === "uos_ai" &&
          mutation.key[1] === "api_key_usage" &&
          mutation.key[2] === "v3" &&
          typeof mutation.value === "object" &&
          mutation.value !== null &&
          (mutation.value as { state?: unknown }).state === "dispatched"
        );
        if (apiKeyV3Dispatch) {
          this.onApiKeyV3DispatchCommit?.();
          if (this.apiKeyV3DispatchCommitGate) await this.apiKeyV3DispatchCommitGate;
        }
        for (const mutation of mutations) {
          const encoded = encodeKey(mutation.key);
          if (mutation.kind === "delete") this.remove(mutation.key);
          else if (mutation.kind === "set") this.write(mutation.key, mutation.value);
          else {
            const current = this.values.get(encoded) as Deno.KvU64 | undefined;
            this.write(mutation.key, new Deno.KvU64((current?.value ?? 0n) + mutation.value));
            this.sums += 1;
          }
          this.writes += 1;
        }
        return { ok: true, versionstamp: "00000000000000000001" };
      },
    };
    return operation as unknown as Deno.AtomicOperation;
  }
}

const kv = new CountingKv();
(Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = () => Promise.resolve(kv as unknown as Deno.Kv);

const { default: handler } = await import("../src/handler.ts");
const { handleResponses } = await import("../src/openai.ts");
const {
  API_KEY_USAGE_V3_REQUEST_PREFIX,
  ApiKeyQuotaDispatchError,
  apiKeyPolicyFromHashRecord,
  apiKeyUsageV3RequestKey,
  apiKeyUsageV3WindowKey,
  authenticateApiKeyToken,
  makeApiKeyUsageWindowV3,
  reserveApiKeyUsageV3,
  invalidateApiKeyPolicy,
  resetApiKeyPolicyCacheForTest,
} = await import("../src/api_key_policy.ts");
const { kernelOrgWindowKey } = await import("../src/kernel_quota_v2.ts");
const {
  loadRuntimeConfig,
  RUNTIME_CONFIG_CACHE_TTL_MS,
  RUNTIME_CONFIG_V2_KEY,
  resetRuntimeConfigCacheForTest,
} = await import("../src/runtime_config.ts");
const { resetCodexAuthCacheForTest } = await import("../src/codex.ts");
const {
  getCodexProviderHealth,
  getYunwuProviderHealth,
  resetProviderHealthThrottleForTest,
} = await import("../src/provider_health.ts");

const MODEL = "gpt-5-kv-budget";
const now = Date.now();
const runtime = {
  version: 2,
  default_model: MODEL,
  default_reasoning_effort: "medium",
  codex_models: {
    source: "chatgpt_codex",
    client_version: "0.150.0",
    updated_at_ms: now,
    models: [{
      slug: MODEL,
      default_reasoning_level: "medium",
      supported_reasoning_levels: ["none", "low", "medium", "high"],
    }],
  },
  updated_at_ms: now,
};
const codexAuthPool = (accountCount = 1) => ({
  accounts: Array.from({ length: accountCount }, (_, index) => ({
    access_token: `access-${index + 1}`,
    refresh_token: `refresh-${index + 1}`,
    account_id: `acct-${index + 1}`,
    updated_at_ms: Date.now(),
  })),
  updated_at_ms: Date.now(),
});

const seedKey = async (token: string, id: string, limit: number) => {
  const hash = await sha256Base64Url(token);
  const record = {
    id,
    expires_at_ms: -1,
    revoked_at_ms: null,
    usage_limit_requests: limit,
    usage_requests: 0,
    usage_reset_at_ms: now + 60_000,
    window_ms: 60_000,
    usage_quota_version: 3 as const,
    paid_fallback_enabled: false,
    paid_fallback_limit_microcredits: 0,
    paid_fallback_spent_microcredits: 0,
    paid_fallback_reserved_microcredits: 0,
    paid_fallback_reservation_request_id: null,
  };
  kv.values.set(encodeKey(["ubq_ai", "api_keys", "hash", hash]), record);
  return { hash, record };
};

const seedPaidFallbackKey = async (token: string, id: string) => {
  const hash = await sha256Base64Url(token);
  const resetAtMs = Date.now() + 60_000;
  const policy = {
    expires_at_ms: -1,
    revoked_at_ms: null,
    usage_limit_requests: -1,
    usage_requests: 0,
    usage_reset_at_ms: resetAtMs,
    window_ms: 60_000,
    usage_quota_version: 3 as const,
    paid_fallback_enabled: true,
    paid_fallback_limit_microcredits: 1_000_000,
    paid_fallback_spent_microcredits: 0,
    paid_fallback_reserved_microcredits: 0,
    paid_fallback_reservation_request_id: null,
  };
  kv.values.set(encodeKey(["ubq_ai", "api_keys", "hash", hash]), { id, ...policy });
  kv.values.set(encodeKey(["ubq_ai", "api_keys", "id", id]), {
    id,
    name: "First fallback quota",
    prefix: token.slice(0, 12),
    hash,
    created_at_ms: Date.now(),
    ...policy,
    paid_fallback_model_ids: [MODEL],
    paid_fallback_quota_per_credit: 500_000,
    paid_fallback_max_exposure_microcredits: { [MODEL]: 250_000 },
    paid_fallback_pricing_checked_at_ms: Date.now(),
  });
  return { hash, record: { id, ...policy } };
};

const request = (token: string): Request =>
  new Request("https://ai.ubq.fi/v1/responses", {
    method: "POST",
    headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
    body: JSON.stringify({ input: "ping" }),
  });

const streamingRequest = (token: string, route: "responses" | "chat"): Request =>
  new Request(
    route === "responses" ? "https://ai.ubq.fi/v1/responses" : "https://ai.ubq.fi/v1/chat/completions",
    {
      method: "POST",
      headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
      body: JSON.stringify(
        route === "responses"
          ? { input: "ping", stream: true }
          : { messages: [{ role: "user", content: "ping" }], stream: true },
      ),
    },
  );

const completedSseEvent = (inputTokens = 1, outputTokens = 1): string =>
  `data: ${
    JSON.stringify({
      type: "response.completed",
      response: {
        model: MODEL,
        output: [],
        usage: {
          input_tokens: inputTokens,
          output_tokens: outputTokens,
          total_tokens: inputTokens + outputTokens,
        },
      },
    })
  }\n\n`;

const sse = (
  usage: Record<string, unknown> = { input_tokens: 1, output_tokens: 1, total_tokens: 2 },
): Response =>
  new Response(
    `data: ${
      JSON.stringify({
        type: "response.completed",
        response: { model: MODEL, output: [], usage },
      })
    }\n\n`,
    { status: 200, headers: { "Content-Type": "text/event-stream" } },
  );

const usageWindow = (
  policy: NonNullable<ReturnType<typeof apiKeyPolicyFromHashRecord>>,
): { committed_requests: number; reserved_requests: number; window_reset_at_ms: number } => {
  const value = kv.values.get(encodeKey(apiKeyUsageV3WindowKey(policy)));
  assert.ok(value, "V3 aggregate must exist");
  const window = value as { committed_requests: number; reserved_requests: number; window_reset_at_ms: number };
  return {
    committed_requests: window.committed_requests,
    reserved_requests: window.reserved_requests,
    window_reset_at_ms: window.window_reset_at_ms,
  };
};

const prepareApiKeyInference = async (tokenDigit: string, keyId: string, limit: number) => {
  kv.values.clear();
  resetApiKeyPolicyCacheForTest();
  resetRuntimeConfigCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool());
  const token = `u_${tokenDigit.repeat(64)}`;
  const { hash, record } = await seedKey(token, keyId, limit);
  const policy = apiKeyPolicyFromHashRecord(hash, record, Date.now());
  assert.ok(policy);
  return { token, hash, record, policy };
};

const requiredTerminalTiming = (terminal: Record<string, unknown>, field: string): number => {
  const value = terminal[field];
  if (typeof value !== "number") assert.fail(`${field} must be a number`);
  assert.ok(Number.isFinite(value), `${field} must be finite`);
  assert.ok(value >= 0, `${field} must be nonnegative`);
  return value;
};

const assertOrderedTerminalTimings = (
  terminal: Record<string, unknown>,
  expectsDownstreamDrain: boolean,
): void => {
  const ordered = [
    "first_codex_dispatch_ms",
    "first_codex_headers_ms",
    "first_sse_event_ms",
    "stream_terminal_ms",
    "latency_ms",
  ].map((field) => requiredTerminalTiming(terminal, field));
  for (let index = 1; index < ordered.length; index += 1) {
    assert.ok(ordered[index - 1]! <= ordered[index]!, "terminal timing fields must be ordered");
  }
  if (!expectsDownstreamDrain) {
    assert.equal(terminal.downstream_drain_ms, null);
    return;
  }
  const downstreamDrain = requiredTerminalTiming(terminal, "downstream_drain_ms");
  assert.ok(ordered[3]! + downstreamDrain <= ordered[4]!);
};

Deno.test("V3 dispatch ledger commits unlimited API-key requests exactly once", async () => {
  const { token, policy } = await prepareApiKeyInference("1", "unlimited", -1);
  const originalFetch = globalThis.fetch;
  globalThis.fetch = () => Promise.resolve(sse());
  try {
    assert.equal((await handler(request(token))).status, 200);
    assert.equal((await handler(request(token))).status, 200);
    assert.deepEqual(usageWindow(policy), {
      committed_requests: 2,
      reserved_requests: 0,
      window_reset_at_ms: policy.usage_reset_at_ms,
    });
    assert.equal(
      [...kv.values.keys()].some((key) => key.includes('"api_key_usage","v2"')),
      false,
      "runtime inference must not write V2 counters",
    );
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("V3 unlimited concurrent reservations avoid local CAS exhaustion and lease scans", async () => {
  const { policy } = await prepareApiKeyInference("9", "unlimited-concurrent", -1);
  const concurrency = 100;
  kv.resetCounts();

  const admissions = await Promise.all(
    Array.from(
      { length: concurrency },
      (_, index) =>
        reserveApiKeyUsageV3(policy, `unlimited-concurrent-${index}`, "responses", {
          kv: kv as unknown as Deno.Kv,
        }),
    ),
  );
  const reservations = admissions.map((admission) => {
    if (!admission.ok) throw new Error(`unexpected quota admission status ${admission.response.status}`);
    return admission.reservation;
  });
  assert.equal(reservations.length, concurrency);

  await Promise.all(reservations.map((reservation) => reservation.beforeProviderDispatch("yunwu")));

  assert.deepEqual(usageWindow(policy), {
    committed_requests: concurrency,
    reserved_requests: 0,
    window_reset_at_ms: policy.usage_reset_at_ms,
  });
  assert.equal(kv.listCalls, 0, "unlimited admission must not eagerly scan reservation leases");
});

Deno.test("V3 reservations release validation failures before any provider dispatch", async () => {
  const { token, policy } = await prepareApiKeyInference("2", "release-before-dispatch", 1);
  const originalFetch = globalThis.fetch;
  let fetchCalls = 0;
  globalThis.fetch = () => {
    fetchCalls += 1;
    return Promise.resolve(sse());
  };
  try {
    const invalid = new Request("https://ai.ubq.fi/v1/responses", {
      method: "POST",
      headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
      body: JSON.stringify({ input: 42 }),
    });
    assert.equal((await handler(invalid)).status, 400);
    assert.equal(fetchCalls, 0);
    assert.deepEqual(usageWindow(policy), {
      committed_requests: 0,
      reserved_requests: 0,
      window_reset_at_ms: policy.usage_reset_at_ms,
    });
    const released = [...kv.values.entries()].find(([key]) =>
      key.includes(JSON.stringify(API_KEY_USAGE_V3_REQUEST_PREFIX).slice(1, -1))
    )?.[1] as { state?: string; release_reason?: string } | undefined;
    assert.equal(released?.state, "released");
    assert.equal(released?.release_reason, "route_completed_without_provider_dispatch");

    assert.equal((await handler(request(token))).status, 200);
    assert.equal(fetchCalls, 1);
    assert.equal(usageWindow(policy).committed_requests, 1);
    assert.equal(usageWindow(policy).reserved_requests, 0);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("V3 dispatch is idempotent across retries and remains consumed after provider failure", async () => {
  const { policy } = await prepareApiKeyInference("a", "dispatch-once", 2);
  const admission = await reserveApiKeyUsageV3(policy, "request-dispatch-once", "responses", {
    kv: kv as unknown as Deno.Kv,
  });
  assert.equal(admission.ok, true);
  if (!admission.ok) return;

  await admission.reservation.beforeProviderDispatch("chatgpt_codex");
  await admission.reservation.beforeProviderDispatch("yunwu");
  await admission.reservation.release("provider_http_failure");

  assert.deepEqual(usageWindow(policy), {
    committed_requests: 1,
    reserved_requests: 0,
    window_reset_at_ms: policy.usage_reset_at_ms,
  });
  const requestRecord = kv.values.get(
    encodeKey(apiKeyUsageV3RequestKey(policy, "request-dispatch-once")),
  ) as { state?: string; provider?: string; dispatched_at_ms?: number | null } | undefined;
  assert.equal(requestRecord?.state, "dispatched");
  assert.equal(requestRecord?.provider, "chatgpt_codex");
  assert.equal(typeof requestRecord?.dispatched_at_ms, "number");
});

Deno.test("V3 cancellation during the Codex dispatch commit releases quota before fetch", async () => {
  const { token, policy } = await prepareApiKeyInference("e", "dispatch-cancelled", 1);
  const originalFetch = globalThis.fetch;
  let fetchCalls = 0;
  let releaseDispatchCommit = () => {};
  const dispatchCommitGate = new Promise<void>((resolve) => {
    releaseDispatchCommit = resolve;
  });
  let dispatchCommitStarted = () => {};
  const dispatchCommitStartedPromise = new Promise<void>((resolve) => {
    dispatchCommitStarted = resolve;
  });
  kv.apiKeyV3DispatchCommitGate = dispatchCommitGate;
  kv.onApiKeyV3DispatchCommit = dispatchCommitStarted;
  const controller = new AbortController();
  globalThis.fetch = () => {
    fetchCalls += 1;
    return Promise.resolve(sse());
  };
  try {
    const pending = handler(
      new Request("https://ai.ubq.fi/v1/responses", {
        method: "POST",
        headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
        body: JSON.stringify({ input: "cancel before transport" }),
        signal: controller.signal,
      }),
    );
    await dispatchCommitStartedPromise;
    controller.abort(new DOMException("cancelled", "AbortError"));
    releaseDispatchCommit();
    await pending;

    assert.equal(fetchCalls, 0);
    assert.deepEqual(usageWindow(policy), {
      committed_requests: 0,
      reserved_requests: 0,
      window_reset_at_ms: policy.usage_reset_at_ms,
    });
  } finally {
    releaseDispatchCommit();
    kv.apiKeyV3DispatchCommitGate = null;
    kv.onApiKeyV3DispatchCommit = null;
    globalThis.fetch = originalFetch;
  }
});

Deno.test("V3 limit-one concurrent admission dispatches once and rejects seven requests", async () => {
  const { token, policy } = await prepareApiKeyInference("b", "concurrent-bounded", 1);
  const concurrency = 8;
  let fetchCalls = 0;
  let releaseUpstreams: () => void = () => {};
  const upstreamGate = new Promise<void>((resolve) => {
    releaseUpstreams = resolve;
  });
  let resolveFirstDispatch: () => void = () => {};
  const firstDispatch = new Promise<void>((resolve) => {
    resolveFirstDispatch = resolve;
  });
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () => {
    fetchCalls += 1;
    resolveFirstDispatch();
    await upstreamGate;
    return sse();
  };
  try {
    const pending = Array.from({ length: concurrency }, () => handler(request(token)));
    await firstDispatch;
    releaseUpstreams();

    const responses = await Promise.all(pending);
    assert.equal(responses.filter((response) => response.status === 200).length, 1);
    assert.equal(responses.filter((response) => response.status === 429).length, 7);
    assert.equal(fetchCalls, 1, "over-limit reservations must not reach a provider");
    assert.deepEqual(usageWindow(policy), {
      committed_requests: 1,
      reserved_requests: 0,
      window_reset_at_ms: policy.usage_reset_at_ms,
    });
  } finally {
    releaseUpstreams();
    globalThis.fetch = originalFetch;
  }
});

Deno.test("V3 admission reclaims expired reservations and preserves dispatch identity", async () => {
  const { policy } = await prepareApiKeyInference("c", "expired-lease", 1);
  const expiredRequestId = "expired-request";
  const nowMs = Date.now();
  kv.values.set(
    encodeKey(apiKeyUsageV3WindowKey(policy)),
    { ...makeApiKeyUsageWindowV3(policy, nowMs), reserved_requests: 1 },
  );
  kv.values.set(encodeKey(apiKeyUsageV3RequestKey(policy, expiredRequestId)), {
    v: 3,
    key_id: policy.key_id,
    request_id: expiredRequestId,
    route: "responses",
    state: "reserved",
    reserved_at_ms: nowMs - 10_000,
    lease_expires_at_ms: nowMs - 1,
    provider: null,
    dispatched_at_ms: null,
    released_at_ms: null,
    release_reason: null,
  });

  const admission = await reserveApiKeyUsageV3(policy, "replacement-request", "responses", {
    kv: kv as unknown as Deno.Kv,
    nowMs,
  });
  assert.equal(admission.ok, true);
  if (!admission.ok) return;
  const expired = kv.values.get(
    encodeKey(apiKeyUsageV3RequestKey(policy, expiredRequestId)),
  ) as { state?: string; release_reason?: string } | undefined;
  assert.equal(expired?.state, "released");
  assert.equal(expired?.release_reason, "lease_expired");
  assert.equal(usageWindow(policy).reserved_requests, 1);

  await admission.reservation.release();
  assert.equal(usageWindow(policy).reserved_requests, 0);
  assert.equal(usageWindow(policy).committed_requests, 0);
});

Deno.test("V3 dispatch CAS failure prevents a provider fetch and exhausted quota does not block models", async () => {
  const { token, policy } = await prepareApiKeyInference("d", "dispatch-cas", 1);
  const originalFetch = globalThis.fetch;
  let fetchCalls = 0;
  globalThis.fetch = () => {
    fetchCalls += 1;
    return Promise.resolve(sse());
  };
  try {
    // Reservation retries five conflicts, then fails closed before openai.ts
    // can call the configured provider transport.
    kv.failNextCommits = 5;
    const unavailable = await handler(request(token));
    assert.equal(unavailable.status, 503);
    assert.equal(fetchCalls, 0);

    kv.failNextCommits = 0;
    kv.values.set(
      encodeKey(apiKeyUsageV3WindowKey(policy)),
      { ...makeApiKeyUsageWindowV3(policy), committed_requests: 1 },
    );
    const models = await handler(
      new Request("https://ai.ubq.fi/v1/models", {
        headers: { Authorization: `Bearer ${token}` },
      }),
    );
    assert.equal(models.status, 200);
    assert.equal(fetchCalls, 0);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("streaming V3 quota is committed at dispatch, including premature and cancelled streams", async () => {
  const originalFetch = globalThis.fetch;
  const encoder = new TextEncoder();
  const prepare = async (tokenDigit: string, keyId: string) => {
    kv.values.clear();
    resetApiKeyPolicyCacheForTest();
    resetRuntimeConfigCacheForTest();
    resetCodexAuthCacheForTest();
    kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
    kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool());
    const token = `u_${tokenDigit.repeat(64)}`;
    const { hash, record } = await seedKey(token, keyId, 100);
    const policy = apiKeyPolicyFromHashRecord(hash, record, Date.now());
    assert.ok(policy);
    return { token, policy };
  };

  try {
    for (const [route, tokenDigit] of [["responses", "a"], ["chat", "b"]] as const) {
      const { token, policy } = await prepare(tokenDigit, `stream-completed-${route}`);
      const upstream = { controller: null as ReadableStreamDefaultController<Uint8Array> | null };
      globalThis.fetch = () =>
        Promise.resolve(
          new Response(
            new ReadableStream<Uint8Array>({
              start(controller) {
                upstream.controller = controller;
                controller.enqueue(
                  encoder.encode(`data: ${JSON.stringify({ type: "response.created", response: { id: route } })}\n\n`),
                );
              },
            }),
            { status: 200, headers: { "Content-Type": "text/event-stream" } },
          ),
        );

      const response = await handler(streamingRequest(token, route));
      assert.equal(response.status, 200);
      assert.equal(usageWindow(policy).committed_requests, 1, `${route} did not commit at provider dispatch`);
      const reader = response.body!.getReader();
      if (route === "responses") {
        const created = await reader.read();
        assert.equal(created.done, false);
        assert.equal(usageWindow(policy).committed_requests, 1, "Responses committed more than once");
      }

      const completedChunk = reader.read();
      upstream.controller!.enqueue(encoder.encode(completedSseEvent(3, 4)));
      upstream.controller!.enqueue(encoder.encode(completedSseEvent(5, 6)));
      upstream.controller!.close();
      assert.equal((await completedChunk).done, false);
      while (!(await reader.read()).done) {
        // Drain any trailing [DONE] or duplicate upstream completion chunks.
      }
      assert.equal(usageWindow(policy).committed_requests, 1, `${route} counted one response more than once`);
      assert.equal(usageWindow(policy).reserved_requests, 0);
    }

    for (const [route, tokenDigit] of [["responses", "c"], ["chat", "d"]] as const) {
      const { token, policy } = await prepare(tokenDigit, `stream-truncated-${route}`);
      globalThis.fetch = () =>
        Promise.resolve(
          new Response(
            `data: ${JSON.stringify({ type: "response.created", response: { id: route } })}\n\n`,
            { status: 200, headers: { "Content-Type": "text/event-stream" } },
          ),
        );
      const response = await handler(streamingRequest(token, route));
      assert.equal(response.status, 200);
      await response.text();
      assert.equal(usageWindow(policy).committed_requests, 1, `${route} did not count a dispatched truncated stream`);
      assert.equal(usageWindow(policy).reserved_requests, 0);
    }

    for (const [route, tokenDigit] of [["responses", "e"], ["chat", "f"]] as const) {
      const { token, policy } = await prepare(tokenDigit, `stream-cancelled-${route}`);
      const upstream = { controller: null as ReadableStreamDefaultController<Uint8Array> | null };
      globalThis.fetch = () =>
        Promise.resolve(
          new Response(
            new ReadableStream<Uint8Array>({
              start(controller) {
                upstream.controller = controller;
                controller.enqueue(
                  encoder.encode(`data: ${JSON.stringify({ type: "response.created", response: { id: route } })}\n\n`),
                );
              },
            }),
            { status: 200, headers: { "Content-Type": "text/event-stream" } },
          ),
        );
      const response = await handler(streamingRequest(token, route));
      assert.equal(response.status, 200);
      if (route === "responses") {
        const reader = response.body!.getReader();
        assert.equal((await reader.read()).done, false);
        await reader.cancel("test cancelled before completion");
      } else {
        await response.body!.cancel("test cancelled before completion");
      }
      try {
        upstream.controller?.close();
      } catch {
        // Cancellation may already have closed the upstream source.
      }
      assert.equal(usageWindow(policy).committed_requests, 1, `${route} did not count a dispatched cancelled stream`);
      assert.equal(usageWindow(policy).reserved_requests, 0);
    }
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("provider dispatch commits API-key V3 while kernel completion writes only the split window", async () => {
  const { token, policy } = await prepareApiKeyInference("0", "stream-kernel-and-key", 100);

  const keyPair = await crypto.subtle.generateKey(
    {
      name: "RSASSA-PKCS1-v1_5",
      modulusLength: 2048,
      publicExponent: new Uint8Array([0x01, 0x00, 0x01]),
      hash: "SHA-256",
    },
    true,
    ["sign", "verify"],
  );
  const publicKey = new Uint8Array(await crypto.subtle.exportKey("spki", keyPair.publicKey));
  kv.values.set(encodeKey(["uos_ai", "kernel_pubkeys"]), [{ pem: toPublicKeyPem(publicKey) }]);
  const nowSeconds = Math.floor(Date.now() / 1000);
  const header = encodeJsonBase64Url({ alg: "RS256", typ: "JWT" });
  const makeKernelToken = async (): Promise<string> => {
    const payload = encodeJsonBase64Url({
      iss: "ubiquity-os-kernel",
      aud: "ai.ubq.fi",
      iat: nowSeconds,
      exp: nowSeconds + 600,
      jti: `jti_${crypto.randomUUID()}`,
      owner: "lifecycle-org",
      repo: "lifecycle-repo",
      installation_id: null,
      auth_token_sha256: await sha256Base64Url(token),
      state_id: "state_lifecycle",
    });
    const signingInput = `${header}.${payload}`;
    const signature = new Uint8Array(
      await crypto.subtle.sign("RSASSA-PKCS1-v1_5", keyPair.privateKey, textEncoder.encode(signingInput)),
    );
    return `${signingInput}.${encodeBase64Url(signature)}`;
  };
  const kernelToken = await makeKernelToken();

  const upstream = { controller: null as ReadableStreamDefaultController<Uint8Array> | null };
  const originalFetch = globalThis.fetch;
  globalThis.fetch = () =>
    Promise.resolve(
      new Response(
        new ReadableStream<Uint8Array>({
          start(controller) {
            upstream.controller = controller;
            controller.enqueue(
              textEncoder.encode(
                `data: ${JSON.stringify({ type: "response.created", response: { id: "kernel" } })}\n\n`,
              ),
            );
          },
        }),
        { status: 200, headers: { "Content-Type": "text/event-stream" } },
      ),
    );
  try {
    const baseRequest = streamingRequest(token, "responses");
    const headers = new Headers(baseRequest.headers);
    headers.set("X-Ubiquity-Kernel-Token", kernelToken);
    const response = await handler(new Request(baseRequest, { headers }));
    assert.equal(response.status, 200);
    const orgWindowKey = kernelOrgWindowKey("lifecycle-org");
    assert.equal(usageWindow(policy).committed_requests, 1);
    assert.equal(kv.values.has(encodeKey(orgWindowKey)), false);

    const body = response.text();
    upstream.controller!.enqueue(textEncoder.encode(completedSseEvent(2, 3)));
    upstream.controller!.enqueue(textEncoder.encode(completedSseEvent(4, 5)));
    upstream.controller!.close();
    await body;

    assert.equal(usageWindow(policy).committed_requests, 1);
    const kernelWindow = kv.values.get(encodeKey(orgWindowKey)) as { usage_requests?: number } | undefined;
    assert.equal(kernelWindow?.usage_requests, 1);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("KV budget: warm kernel inference writes no ordinary usage aggregates", async () => {
  kv.values.clear();
  resetRuntimeConfigCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool());
  const originalFetch = globalThis.fetch;
  globalThis.fetch = () => Promise.resolve(sse());
  const kernelContext = {
    keyId: null,
    kernelRepo: { owner: "ubiquity", repo: "kernel" },
    kernelOrg: { owner: "ubiquity" },
    paidFallbackEnabled: false,
    requestId: "kernel-telemetry-budget",
    startedAtMs: Date.now(),
  };
  const kernelRequest = () =>
    new Request("https://ai.ubq.fi/v1/responses", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ input: "ping" }),
    });
  try {
    assert.equal((await handleResponses(kernelRequest(), kernelContext)).status, 200);
    kv.resetCounts();
    assert.equal((await handleResponses(kernelRequest(), kernelContext)).status, 200);
    assert.deepEqual({ reads: kv.reads, writes: kv.writes }, { reads: 0, writes: 0 });
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("terminal inference telemetry includes resolved defaults and response usage", async () => {
  kv.values.clear();
  resetApiKeyPolicyCacheForTest();
  resetRuntimeConfigCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool());
  const token = `u_${"5".repeat(64)}`;
  await seedKey(token, "telemetry", -1);
  const originalFetch = globalThis.fetch;
  const originalInfo = console.info;
  const originalGitRevision = Deno.env.get("GIT_REVISION");
  const originalGithubSha = Deno.env.get("GITHUB_SHA");
  const originalBuildId = Deno.env.get("DENO_DEPLOY_BUILD_ID");
  const originalDeploymentId = Deno.env.get("DENO_DEPLOYMENT_ID");
  const logs: unknown[][] = [];
  globalThis.fetch = () =>
    Promise.resolve(sse({
      input_tokens: 1,
      input_tokens_details: { cached_tokens: 0, cache_write_tokens: 1 },
      output_tokens: 1,
      total_tokens: 2,
    }));
  console.info = (...args: unknown[]) => logs.push(args);
  Deno.env.delete("GIT_REVISION");
  Deno.env.delete("GITHUB_SHA");
  Deno.env.delete("DENO_DEPLOY_BUILD_ID");
  Deno.env.delete("DENO_DEPLOYMENT_ID");
  const promptCacheKey = "must-not-appear-in-terminal-telemetry";
  try {
    const response = await handler(
      new Request("https://ai.ubq.fi/v1/responses", {
        method: "POST",
        headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
        body: JSON.stringify({ input: "ping", prompt_cache_key: promptCacheKey }),
      }),
    );
    assert.equal(response.status, 200);
    const terminal = logs.find((entry) => entry[0] === "[ai.ubq.fi] request_terminal");
    assert.ok(terminal);
    const requestId = response.headers.get("x-uos-request-id");
    assert.ok(requestId);
    const terminalPayload = JSON.parse(String(terminal[1])) as Record<string, unknown>;
    assert.deepEqual(terminalPayload, {
      request_id: requestId,
      route: "responses",
      status: 200,
      provider: "chatgpt_codex",
      latency_ms: terminalPayload.latency_ms,
      first_codex_dispatch_ms: terminalPayload.first_codex_dispatch_ms,
      first_codex_headers_ms: terminalPayload.first_codex_headers_ms,
      first_sse_event_ms: terminalPayload.first_sse_event_ms,
      stream_terminal_ms: terminalPayload.stream_terminal_ms,
      downstream_drain_ms: null,
      model: MODEL,
      reasoning: "medium",
      input_tokens: 1,
      cached_input_tokens: 0,
      cache_write_input_tokens: 1,
      output_tokens: 1,
      total_tokens: 2,
      usage_observed: true,
      usage_telemetry_status: "reported",
      prompt_cache_key_present: true,
      prompt_cache_mode: "unspecified",
      explicit_breakpoint_count: 0,
      account_slot: 1,
      affinity_outcome: "none",
      key_id: "telemetry",
      fallback_reason: null,
      stream: false,
      stream_terminal_type: "response.completed",
      git_sha: "unknown",
      deno_revision: "unknown",
      router_revision: null,
    });
    assert.doesNotMatch(String(terminal[1]), new RegExp(promptCacheKey));
    assertOrderedTerminalTimings(terminalPayload, false);
    assert.equal(logs.filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal").length, 1);
    const accepted = logs.find((entry) => entry[0] === "[ai.ubq.fi] request_accepted");
    assert.ok(accepted);
    assert.equal(JSON.parse(String(accepted[1])).request_id, requestId);

    globalThis.fetch = () =>
      Promise.resolve(sse({
        input_tokens: 1,
        input_tokens_details: { cached_tokens: 2, cache_write_tokens: 0 },
        output_tokens: 0,
        total_tokens: 1,
      }));
    const invalid = await handler(
      new Request("https://ai.ubq.fi/v1/responses", {
        method: "POST",
        headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
        body: JSON.stringify({ input: "invalid cache telemetry" }),
      }),
    );
    assert.equal(invalid.status, 200);
    const terminalEvents = logs.filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal");
    assert.equal(terminalEvents.length, 2);
    const invalidTerminal = JSON.parse(String(terminalEvents[1]?.[1])) as Record<string, unknown>;
    assert.equal(invalidTerminal.cached_input_tokens, 2);
    assert.equal(invalidTerminal.cache_write_input_tokens, 0);
    assert.equal(invalidTerminal.usage_telemetry_status, "invalid");

    // An omitted cache-details object is distinct from the provider explicitly
    // reporting a zero cache read/write. Keep that distinction in the terminal
    // event so downstream counter aggregation does not turn missing telemetry
    // into a false zero-token observation.
    globalThis.fetch = () =>
      Promise.resolve(sse({
        input_tokens: 1,
        output_tokens: 0,
        total_tokens: 1,
      }));
    const partial = await handler(
      new Request("https://ai.ubq.fi/v1/responses", {
        method: "POST",
        headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
        body: JSON.stringify({ input: "cache details omitted" }),
      }),
    );
    assert.equal(partial.status, 200);
    const partialTerminalEvents = logs.filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal");
    assert.equal(partialTerminalEvents.length, 3);
    const partialTerminal = JSON.parse(String(partialTerminalEvents[2]?.[1])) as Record<string, unknown>;
    assert.equal(partialTerminal.cached_input_tokens, null);
    assert.equal(partialTerminal.cache_write_input_tokens, null);
    assert.equal(partialTerminal.usage_telemetry_status, "partial");
  } finally {
    console.info = originalInfo;
    globalThis.fetch = originalFetch;
    if (originalGitRevision === undefined) Deno.env.delete("GIT_REVISION");
    else Deno.env.set("GIT_REVISION", originalGitRevision);
    if (originalGithubSha === undefined) Deno.env.delete("GITHUB_SHA");
    else Deno.env.set("GITHUB_SHA", originalGithubSha);
    if (originalBuildId === undefined) Deno.env.delete("DENO_DEPLOY_BUILD_ID");
    else Deno.env.set("DENO_DEPLOY_BUILD_ID", originalBuildId);
    if (originalDeploymentId === undefined) Deno.env.delete("DENO_DEPLOYMENT_ID");
    else Deno.env.set("DENO_DEPLOYMENT_ID", originalDeploymentId);
  }
});

Deno.test("streaming inference emits one terminal log only after the response body completes", async () => {
  kv.values.clear();
  resetApiKeyPolicyCacheForTest();
  resetRuntimeConfigCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool());
  const token = `u_${"7".repeat(64)}`;
  await seedKey(token, "stream-telemetry", -1);

  const encoder = new TextEncoder();
  let resolveUpstreamController: (controller: ReadableStreamDefaultController<Uint8Array>) => void = () => {};
  const upstreamControllerPromise = new Promise<ReadableStreamDefaultController<Uint8Array>>((resolve) => {
    resolveUpstreamController = resolve;
  });
  const originalFetch = globalThis.fetch;
  const originalInfo = console.info;
  const logs: unknown[][] = [];
  globalThis.fetch = () =>
    Promise.resolve(
      new Response(
        new ReadableStream<Uint8Array>({
          start(controller) {
            resolveUpstreamController(controller);
            controller.enqueue(
              encoder.encode(`data: ${JSON.stringify({ type: "response.created", response: { id: "stream" } })}\n\n`),
            );
          },
        }),
        { status: 200, headers: { "Content-Type": "text/event-stream" } },
      ),
    );
  console.info = (...args: unknown[]) => logs.push(args);
  try {
    const response = await handler(
      new Request("https://ai.ubq.fi/v1/responses", {
        method: "POST",
        headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
        body: JSON.stringify({ input: "ping", stream: true }),
      }),
    );
    assert.equal(response.status, 200);
    assert.equal(logs.filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal").length, 0);

    const bodyPromise = response.text();
    const upstreamController = await upstreamControllerPromise;
    upstreamController.enqueue(
      encoder.encode(
        `data: ${
          JSON.stringify({
            type: "response.completed",
            response: { model: MODEL, output: [], usage: { input_tokens: 3, output_tokens: 4, total_tokens: 7 } },
          })
        }\n\n`,
      ),
    );
    upstreamController.close();
    await bodyPromise;

    const terminalLogs = logs.filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal");
    assert.equal(terminalLogs.length, 1);
    const terminal = JSON.parse(String(terminalLogs[0]?.[1])) as Record<string, unknown>;
    assert.equal(terminal.provider, "chatgpt_codex");
    assert.equal(terminal.model, MODEL);
    assert.equal(terminal.reasoning, "medium");
    assert.equal(terminal.stream_terminal_type, "response.completed");
    assertOrderedTerminalTimings(terminal, true);
    assert.equal(terminal.input_tokens, 3);
    assert.equal(terminal.output_tokens, 4);
    assert.equal(terminal.total_tokens, 7);
    assert.equal(terminal.usage_telemetry_status, "partial");
    assert.equal(terminal.request_id, response.headers.get("x-uos-request-id"));
  } finally {
    console.info = originalInfo;
    globalThis.fetch = originalFetch;
  }
});

Deno.test("streaming drain timing remains separate from V3 dispatch accounting", async () => {
  kv.values.clear();
  resetApiKeyPolicyCacheForTest();
  resetRuntimeConfigCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool());
  const token = `u_${"c".repeat(64)}`;
  await seedKey(token, "stream-drain-timing", 5);

  const encoder = new TextEncoder();
  let resolveUpstreamController: (controller: ReadableStreamDefaultController<Uint8Array>) => void = () => {};
  const upstreamControllerPromise = new Promise<ReadableStreamDefaultController<Uint8Array>>((resolve) => {
    resolveUpstreamController = resolve;
  });
  const originalFetch = globalThis.fetch;
  const originalInfo = console.info;
  const logs: unknown[][] = [];
  globalThis.fetch = () =>
    Promise.resolve(
      new Response(
        new ReadableStream<Uint8Array>({
          start(controller) {
            resolveUpstreamController(controller);
            controller.enqueue(
              encoder.encode(`data: ${JSON.stringify({ type: "response.created", response: { id: "stream" } })}\n\n`),
            );
          },
        }),
        { status: 200, headers: { "Content-Type": "text/event-stream" } },
      ),
    );
  console.info = (...args: unknown[]) => logs.push(args);
  try {
    const response = await handler(streamingRequest(token, "responses"));
    assert.ok(response.body);
    const reader = response.body.getReader();
    assert.equal((await reader.read()).done, false);

    const upstreamController = await upstreamControllerPromise;
    upstreamController.enqueue(encoder.encode(completedSseEvent(3, 4)));
    upstreamController.close();
    assert.equal((await reader.read()).done, false);
    await new Promise<void>((resolve) => setTimeout(resolve, 5));
    assert.equal((await reader.read()).done, true);

    const terminalLogs = logs.filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal");
    assert.equal(terminalLogs.length, 1);
    const terminal = JSON.parse(String(terminalLogs[0]?.[1])) as Record<string, unknown>;
    assertOrderedTerminalTimings(terminal, true);
    const postTerminalMs = requiredTerminalTiming(terminal, "latency_ms") -
      requiredTerminalTiming(terminal, "stream_terminal_ms");
    const downstreamDrainMs = requiredTerminalTiming(terminal, "downstream_drain_ms");
    assert.ok(postTerminalMs >= downstreamDrainMs, "downstream drain must be part of terminal latency");
  } finally {
    console.info = originalInfo;
    globalThis.fetch = originalFetch;
  }
});

Deno.test("Chat streaming terminal telemetry reports ordered timings once", async () => {
  kv.values.clear();
  resetApiKeyPolicyCacheForTest();
  resetRuntimeConfigCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool());
  const token = `u_${"b".repeat(64)}`;
  await seedKey(token, "chat-stream-telemetry", -1);
  const originalFetch = globalThis.fetch;
  const originalInfo = console.info;
  const logs: unknown[][] = [];
  globalThis.fetch = () => Promise.resolve(sse());
  console.info = (...args: unknown[]) => logs.push(args);
  try {
    const response = await handler(streamingRequest(token, "chat"));
    assert.equal(response.status, 200);
    await response.text();

    const terminalLogs = logs.filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal");
    assert.equal(terminalLogs.length, 1);
    const terminal = JSON.parse(String(terminalLogs[0]?.[1])) as Record<string, unknown>;
    assert.equal(terminal.route, "chat.completions");
    assert.equal(terminal.stream_terminal_type, "response.completed");
    assertOrderedTerminalTimings(terminal, true);
  } finally {
    console.info = originalInfo;
    globalThis.fetch = originalFetch;
  }
});

Deno.test("first bounded paid fallback response exposes settled spend and consumes one V3 dispatch", async () => {
  kv.values.clear();
  resetApiKeyPolicyCacheForTest();
  resetRuntimeConfigCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool(2));
  const token = `u_${"8".repeat(64)}`;
  const keyId = "first-fallback-quota";
  const { hash, record } = await seedPaidFallbackKey(token, keyId);
  const policy = apiKeyPolicyFromHashRecord(hash, record, Date.now());
  assert.ok(policy);

  const originalFetch = globalThis.fetch;
  const originalYunwuApiKey = Deno.env.get("YUNWU_API_KEY");
  let calls = 0;
  const primaryAccountIds: string[] = [];
  Deno.env.set("YUNWU_API_KEY", "yunwu-test-key");
  globalThis.fetch = (input, init) => {
    calls += 1;
    const request = new Request(input, init);
    const url = request.url;
    if (url === "https://yunwu.ai/v1/responses") {
      const response = sse();
      const headers = new Headers(response.headers);
      headers.set("X-Oneapi-Request-Id", "first-fallback-provider-request");
      return Promise.resolve(new Response(response.body, { status: 200, headers }));
    }
    primaryAccountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(
      new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
        status: 429,
        headers: { "Content-Type": "application/json" },
      }),
    );
  };
  try {
    kv.resetCounts();
    const response = await handler(
      new Request("https://ai.ubq.fi/v1/responses", {
        method: "POST",
        headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
        body: JSON.stringify({ input: "ping", stream: true }),
      }),
    );
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-codex-primary-used-percent"), "0");
    assert.equal(calls, 4);
    assert.deepEqual(primaryAccountIds, ["acct-1", "acct-2", "acct-1"]);
    // Three Codex 429s plus Yunwu still belong to one routed inference. The
    // V3 request is committed before the first transport and not incremented
    // again by retries or fallback.
    assert.equal(usageWindow(policy).committed_requests, 1);
    assert.equal(usageWindow(policy).reserved_requests, 0);
    await response.body?.cancel();
  } finally {
    globalThis.fetch = originalFetch;
    if (originalYunwuApiKey === undefined) Deno.env.delete("YUNWU_API_KEY");
    else Deno.env.set("YUNWU_API_KEY", originalYunwuApiKey);
  }
});

Deno.test("paid fallback releases its dispatch intent when Yunwu quota admission fails before fetch", async () => {
  kv.values.clear();
  resetApiKeyPolicyCacheForTest();
  resetRuntimeConfigCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool());
  const token = `u_${"7".repeat(64)}`;
  const keyId = "fallback-pre-dispatch-quota-failure";
  await seedPaidFallbackKey(token, keyId);

  const originalFetch = globalThis.fetch;
  const originalYunwuApiKey = Deno.env.get("YUNWU_API_KEY");
  let yunwuCalls = 0;
  Deno.env.set("YUNWU_API_KEY", "yunwu-test-key");
  globalThis.fetch = (input) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    if (url === "https://yunwu.ai/v1/responses") {
      yunwuCalls += 1;
      return Promise.reject(new Error("Yunwu transport must not start"));
    }
    return Promise.resolve(
      new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
        status: 429,
        headers: { "Content-Type": "application/json" },
      }),
    );
  };
  try {
    const response = await handleResponses(
      new Request("https://ai.ubq.fi/v1/responses", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ model: MODEL, input: "ping" }),
      }),
      {
        keyId,
        kernelRepo: null,
        kernelOrg: null,
        requestId: "fallback-pre-dispatch-quota-failure-request",
        startedAtMs: Date.now(),
        beforeProviderDispatch: (provider) =>
          provider === "yunwu"
            ? Promise.reject(new ApiKeyQuotaDispatchError("API key quota reservation is unavailable"))
            : Promise.resolve(),
      },
    );
    assert.equal(response.status, 503);
    assert.equal(yunwuCalls, 0);
    const stored = [...kv.values.entries()].find(([key]) => key.includes(`"paid_fallback","v3","request","${keyId}"`))
      ?.[1] as {
        dispatch_state?: string;
        terminal_state?: string;
        billing_state?: string;
      } | undefined;
    assert.equal(stored?.dispatch_state, "not_dispatched");
    assert.equal(stored?.terminal_state, "cancelled");
    assert.equal(stored?.billing_state, "not_billed");
    const window = [...kv.values.entries()].find(([key]) => key.includes(`"paid_fallback","v3","window","${keyId}"`))
      ?.[1] as { reserved_microcredits?: number; pending_count?: number } | undefined;
    assert.equal(window?.reserved_microcredits, 0);
    assert.equal(window?.pending_count, 0);
  } finally {
    globalThis.fetch = originalFetch;
    if (originalYunwuApiKey === undefined) Deno.env.delete("YUNWU_API_KEY");
    else Deno.env.set("YUNWU_API_KEY", originalYunwuApiKey);
  }
});

Deno.test("expired Codex credentials exhaust both accounts before paid Yunwu fallback", async () => {
  kv.values.clear();
  resetProviderHealthThrottleForTest();
  resetApiKeyPolicyCacheForTest();
  resetRuntimeConfigCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool(2));
  const token = `u_${"b".repeat(64)}`;
  const keyId = "expired-codex-fallback";
  await seedPaidFallbackKey(token, keyId);

  const originalFetch = globalThis.fetch;
  const originalInfo = console.info;
  const originalYunwuApiKey = Deno.env.get("YUNWU_API_KEY");
  const accountIds: string[] = [];
  const logs: unknown[][] = [];
  let refreshCalls = 0;
  let yunwuCalls = 0;
  Deno.env.set("YUNWU_API_KEY", "yunwu-test-key");
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    if (request.url === "https://auth.openai.com/oauth/token") {
      refreshCalls += 1;
      return Promise.resolve(
        new Response('{"error":"invalid_grant","error_description":"refresh token reused"}', {
          status: 401,
          headers: { "Content-Type": "application/json" },
        }),
      );
    }
    if (request.url === "https://yunwu.ai/v1/responses") {
      yunwuCalls += 1;
      return Promise.resolve(sse());
    }
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(
      new Response(JSON.stringify({ error: { message: "Access token expired" } }), {
        status: 401,
        headers: { "Content-Type": "application/json" },
      }),
    );
  };
  console.info = (...args: unknown[]) => logs.push(args);

  try {
    const response = await handler(
      new Request("https://ai.ubq.fi/v1/responses", {
        method: "POST",
        headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
        body: JSON.stringify({ input: "ping", stream: true }),
      }),
    );
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-uos-upstream"), "yunwu");
    await response.text();
    assert.equal(accountIds.length, 2);
    assert.equal(new Set(accountIds).size, 2);
    assert.equal(refreshCalls, 2);
    assert.equal(yunwuCalls, 1);
    assert.deepEqual(
      await Promise.all(["acct-1", "acct-2"].map(async (accountId) => (await getCodexProviderHealth(accountId)).state)),
      ["invalid", "invalid"],
    );
    assert.equal((await getYunwuProviderHealth()).state, "healthy");

    const terminalLogs = logs.filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal");
    assert.equal(terminalLogs.length, 1);
    const terminal = JSON.parse(String(terminalLogs[0]?.[1])) as Record<string, unknown>;
    assert.equal(terminal.provider, "yunwu");
    assert.equal(terminal.fallback_reason, "primary_401");
    assert.equal(terminal.stream_terminal_type, "response.completed");
  } finally {
    console.info = originalInfo;
    globalThis.fetch = originalFetch;
    resetCodexAuthCacheForTest();
    if (originalYunwuApiKey === undefined) Deno.env.delete("YUNWU_API_KEY");
    else Deno.env.set("YUNWU_API_KEY", originalYunwuApiKey);
  }
});

Deno.test("paid fallback terminal telemetry records YunWu lifecycle", async () => {
  kv.values.clear();
  resetApiKeyPolicyCacheForTest();
  resetRuntimeConfigCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool());
  const token = `u_${"9".repeat(64)}`;
  await seedPaidFallbackKey(token, "fallback-terminal-telemetry");

  const originalFetch = globalThis.fetch;
  const originalInfo = console.info;
  const originalYunwuApiKey = Deno.env.get("YUNWU_API_KEY");
  const logs: unknown[][] = [];
  Deno.env.set("YUNWU_API_KEY", "yunwu-test-key");
  globalThis.fetch = (input) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    if (url === "https://yunwu.ai/v1/responses") {
      return new Promise<Response>((resolve) => setTimeout(() => resolve(sse()), 30));
    }
    return Promise.resolve(
      new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
        status: 429,
        headers: { "Content-Type": "application/json" },
      }),
    );
  };
  console.info = (...args: unknown[]) => logs.push(args);
  try {
    const response = await handler(request(token));
    assert.equal(response.status, 200);
    await response.text();
    const terminalLogs = logs.filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal");
    assert.equal(terminalLogs.length, 1);
    const terminal = JSON.parse(String(terminalLogs[0]?.[1])) as Record<string, unknown>;
    assert.equal(terminal.provider, "yunwu");
    assert.equal(terminal.fallback_reason, "primary_429");
    assert.equal(terminal.stream_terminal_type, "response.completed");
    assert.equal(terminal.request_id, response.headers.get("x-uos-request-id"));
    const firstCodexHeadersMs = requiredTerminalTiming(terminal, "first_codex_headers_ms");
    const firstSseEventMs = requiredTerminalTiming(terminal, "first_sse_event_ms");
    assert.ok(
      firstSseEventMs >= firstCodexHeadersMs + 10,
      "YunWu response time must remain outside first Codex timing",
    );
    assert.equal(Object.prototype.hasOwnProperty.call(terminal, "upstream_headers_ms"), false);
  } finally {
    console.info = originalInfo;
    globalThis.fetch = originalFetch;
    if (originalYunwuApiKey === undefined) Deno.env.delete("YUNWU_API_KEY");
    else Deno.env.set("YUNWU_API_KEY", originalYunwuApiKey);
  }
});

Deno.test("paid fallback cancellation telemetry records a cancelled YunWu lifecycle", async () => {
  kv.values.clear();
  resetApiKeyPolicyCacheForTest();
  resetRuntimeConfigCacheForTest();
  resetCodexAuthCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  kv.values.set(encodeKey(["ubq_ai", "codex_auth"]), codexAuthPool());
  const token = `u_${"a".repeat(64)}`;
  const keyId = "fallback-cancel-telemetry";
  await seedPaidFallbackKey(token, keyId);

  const originalFetch = globalThis.fetch;
  const originalInfo = console.info;
  const originalYunwuApiKey = Deno.env.get("YUNWU_API_KEY");
  const logs: unknown[][] = [];
  const encoder = new TextEncoder();
  Deno.env.set("YUNWU_API_KEY", "yunwu-test-key");
  globalThis.fetch = (input) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    if (url === "https://yunwu.ai/v1/responses") {
      return Promise.resolve(
        new Response(
          new ReadableStream<Uint8Array>({
            start(controller) {
              controller.enqueue(
                encoder.encode(
                  `data: ${JSON.stringify({ type: "response.created", response: { id: "cancelled" } })}\n\n`,
                ),
              );
            },
          }),
          { status: 200, headers: { "Content-Type": "text/event-stream" } },
        ),
      );
    }
    return Promise.resolve(
      new Response(JSON.stringify({ error: { message: "Primary limited" } }), {
        status: 429,
        headers: { "Content-Type": "application/json" },
      }),
    );
  };
  console.info = (...args: unknown[]) => logs.push(args);
  try {
    const response = await handler(streamingRequest(token, "responses"));
    assert.equal(response.status, 200);
    assert.ok(response.body);
    const reader = response.body.getReader();
    assert.equal((await reader.read()).done, false);
    await reader.cancel("client disconnected");

    const terminalLogs = logs.filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal");
    assert.equal(terminalLogs.length, 1);
    const terminal = JSON.parse(String(terminalLogs[0]?.[1])) as Record<string, unknown>;
    assert.equal(terminal.provider, "yunwu");
    assert.equal(terminal.fallback_reason, "primary_429");
    assert.equal(terminal.stream_terminal_type, "cancelled");
  } finally {
    console.info = originalInfo;
    globalThis.fetch = originalFetch;
    if (originalYunwuApiKey === undefined) Deno.env.delete("YUNWU_API_KEY");
    else Deno.env.set("YUNWU_API_KEY", originalYunwuApiKey);
  }
});

Deno.test("V3 limit-only changes preserve the active aggregate identity and committed usage", async () => {
  const token = `u_${"4".repeat(64)}`;
  const { hash, record } = await seedKey(token, "limit-change", 100);
  const original = apiKeyPolicyFromHashRecord(hash, record, now);
  const lowered = apiKeyPolicyFromHashRecord(hash, { ...record, usage_limit_requests: 10 }, now);
  assert.ok(original && lowered);
  assert.deepEqual(apiKeyUsageV3WindowKey(lowered), apiKeyUsageV3WindowKey(original));
  kv.values.set(
    encodeKey(apiKeyUsageV3WindowKey(original)),
    { ...makeApiKeyUsageWindowV3(original), committed_requests: 12 },
  );
  kv.values.set(encodeKey(["ubq_ai", "api_keys", "hash", hash]), { ...record, usage_limit_requests: 10 });
  resetApiKeyPolicyCacheForTest();
  const decision = await authenticateApiKeyToken(token, { kv: kv as unknown as Deno.Kv, nowMs: now });
  assert.equal(decision.ok, true, "authentication must not perform inference quota admission");
  assert.equal(usageWindow(lowered).committed_requests, 12);
});

Deno.test("/uos/auth projects committed V3 usage while models stay quota-ledger independent", async () => {
  const { token, hash, record, policy } = await prepareApiKeyInference("5", "auth-projection", 10);
  kv.values.set(
    encodeKey(apiKeyUsageV3WindowKey(policy)),
    { ...makeApiKeyUsageWindowV3(policy), committed_requests: 7, reserved_requests: 1 },
  );
  kv.values.set(encodeKey(["ubq_ai", "api_keys", "id", policy.key_id]), {
    ...record,
    id: policy.key_id,
    name: "Auth projection",
    prefix: token.slice(0, 12),
    hash,
    created_at_ms: Date.now(),
    paid_fallback_model_ids: [],
    paid_fallback_quota_per_credit: 0,
    paid_fallback_pricing_checked_at_ms: null,
  });

  const auth = await handler(
    new Request("https://ai.ubq.fi/uos/auth", { headers: { Authorization: `Bearer ${token}` } }),
  );
  assert.equal(auth.status, 200);
  const authBody = await auth.json() as { auth?: { method?: { key?: { usage_requests?: number } } } };
  assert.equal(authBody.auth?.method?.key?.usage_requests, 7);

  kv.failApiKeyV3Reads = true;
  try {
    const models = await handler(
      new Request("https://ai.ubq.fi/v1/models", { headers: { Authorization: `Bearer ${token}` } }),
    );
    assert.equal(models.status, 200, "non-inference routes must not read the V3 quota aggregate");

    const unavailableProjection = await handler(
      new Request("https://ai.ubq.fi/uos/auth", { headers: { Authorization: `Bearer ${token}` } }),
    );
    assert.equal(unavailableProjection.status, 503, "/uos/auth must fail rather than report stale usage");
  } finally {
    kv.failApiKeyV3Reads = false;
  }
});

Deno.test("V3 automatic window advancement changes aggregate identity only when the effective window changes", async () => {
  const token = `u_${"6".repeat(64)}`;
  const { hash, record } = await seedKey(token, "window-advance", 100);
  const expired = { ...record, usage_reset_at_ms: now - 1 };
  const beforeAdvance = apiKeyPolicyFromHashRecord(hash, expired, now);
  assert.ok(beforeAdvance);
  const afterAdvance = apiKeyPolicyFromHashRecord(
    hash,
    { ...expired, usage_reset_at_ms: beforeAdvance.usage_reset_at_ms },
    now,
  );
  const explicitlyReset = apiKeyPolicyFromHashRecord(
    hash,
    { ...expired, usage_reset_at_ms: now + expired.window_ms },
    now,
  );
  assert.ok(afterAdvance && explicitlyReset);
  assert.deepEqual(apiKeyUsageV3WindowKey(afterAdvance), apiKeyUsageV3WindowKey(beforeAdvance));
  assert.notDeepEqual(apiKeyUsageV3WindowKey(explicitlyReset), apiKeyUsageV3WindowKey(beforeAdvance));
});

Deno.test("KV budget: runtime configuration revalidates after the bounded isolate TTL", async () => {
  resetRuntimeConfigCacheForTest();
  const first = structuredClone(runtime);
  const second = {
    ...runtime,
    default_model: "gpt-5-kv-budget-next",
    codex_models: {
      ...runtime.codex_models,
      models: [{
        slug: "gpt-5-kv-budget-next",
        default_reasoning_level: "high",
        supported_reasoning_levels: ["none", "high"],
      }],
    },
    updated_at_ms: now + 1,
  };
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), first);
  kv.resetCounts();
  assert.equal((await loadRuntimeConfig(kv as unknown as Deno.Kv, now))?.default_model, MODEL);
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), second);
  assert.equal(
    (await loadRuntimeConfig(kv as unknown as Deno.Kv, now + RUNTIME_CONFIG_CACHE_TTL_MS - 1))?.default_model,
    MODEL,
  );
  const refreshed = await Promise.all(
    Array.from(
      { length: 20 },
      () => loadRuntimeConfig(kv as unknown as Deno.Kv, now + RUNTIME_CONFIG_CACHE_TTL_MS + 1),
    ),
  );
  assert.ok(refreshed.every((config) => config?.default_model === second.default_model));
  assert.equal(kv.reads, 2);
});

Deno.test("KV budget: failed runtime configuration refresh backs off with stale configuration", async () => {
  resetRuntimeConfigCacheForTest();
  kv.values.set(encodeKey(RUNTIME_CONFIG_V2_KEY), runtime);
  assert.equal((await loadRuntimeConfig(kv as unknown as Deno.Kv, now))?.default_model, MODEL);

  let failedReads = 0;
  const unavailableKv = {
    get: () => {
      failedReads += 1;
      return Promise.reject(new Error("runtime config KV unavailable"));
    },
  } as unknown as Deno.Kv;
  assert.equal(
    (await loadRuntimeConfig(unavailableKv, now + RUNTIME_CONFIG_CACHE_TTL_MS + 1))?.default_model,
    MODEL,
  );
  assert.equal(
    (await loadRuntimeConfig(unavailableKv, now + RUNTIME_CONFIG_CACHE_TTL_MS * 2))?.default_model,
    MODEL,
  );
  assert.equal(failedReads, 1);
});

Deno.test("KV budget: malformed tokens are rejected without KV and policy expiry refreshes revocation", async () => {
  const originalInfo = console.info;
  const logs: unknown[][] = [];
  console.info = (...args: unknown[]) => logs.push(args);
  try {
    kv.resetCounts();
    const malformedResponse = await handler(request("malformed"));
    assert.equal(malformedResponse.status, 401);
    assert.deepEqual({ reads: kv.reads, writes: kv.writes }, { reads: 0, writes: 0 });
    const terminalLogs = logs.filter((entry) => entry[0] === "[ai.ubq.fi] request_terminal");
    assert.equal(terminalLogs.length, 1);
    const terminal = JSON.parse(String(terminalLogs[0]?.[1])) as Record<string, unknown>;
    assert.equal(terminal.status, 401);
    assert.equal(terminal.provider, "gateway");
    assert.equal(terminal.key_id, null);
    assert.equal(terminal.request_id, malformedResponse.headers.get("x-uos-request-id"));
    assert.equal(terminal.input_tokens, null);
    assert.equal(terminal.output_tokens, null);
    assert.equal(terminal.total_tokens, null);
    assert.equal(terminal.usage_telemetry_status, "missing");
  } finally {
    console.info = originalInfo;
  }

  const token = `u_${"3".repeat(64)}`;
  const { hash, record } = await seedKey(token, "revoked", -1);
  resetApiKeyPolicyCacheForTest();
  assert.equal((await authenticateApiKeyToken(token, { kv: kv as unknown as Deno.Kv, nowMs: now })).ok, true);
  kv.values.set(encodeKey(["ubq_ai", "api_keys", "hash", hash]), { ...record, revoked_at_ms: now + 1 });
  assert.equal(
    (await authenticateApiKeyToken(token, { kv: kv as unknown as Deno.Kv, nowMs: now + 29_999 })).ok,
    true,
  );
  assert.equal(
    (await authenticateApiKeyToken(token, { kv: kv as unknown as Deno.Kv, nowMs: now + 30_001 })).ok,
    false,
  );
  invalidateApiKeyPolicy("revoked");
});
