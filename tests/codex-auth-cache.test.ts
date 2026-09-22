import assert from "node:assert/strict";
import { CODEX_BANKED_RESET_LEASE_MS, type CodexBankedResetConfig } from "../src/codex_banked_reset.ts";
import { PROVIDER_CAPACITY_SNAPSHOT_KEY } from "../src/provider_capacity_contract.ts";
import { setKvForTest } from "../src/kv.ts";
import type { CodexUsageResetProvider } from "../src/codex_banked_reset_provider.ts";
import type { CodexAuthPoolState, CodexAuthState } from "../src/types.ts";

const AUTH_KEY = ["ubq_ai", "codex_auth"] as const;

type FakeKvWrite = { type: "set" | "delete"; key: Deno.KvKey; value?: unknown };

/** `void` may not appear as a call-site type argument, so name the deferred shape. */
type VoidDeferred = PromiseWithResolvers<void>;

const isAuthKey = (key: Deno.KvKey): boolean => JSON.stringify(key) === JSON.stringify(AUTH_KEY);

/** Upstream error type a banked-reset probe status maps to. */
const probeErrorType = (status: number): string => {
  if (status === 429) return "usage_limit_reached";
  if (status === 401) return "authentication_error";
  return "forbidden";
};

const isProviderHealthSuccessWrite = (write: FakeKvWrite): boolean =>
  write.key[0] === "uos_ai" &&
  write.key[1] === "provider_health" &&
  write.key[2] === "v1" &&
  (write.value as { event?: unknown } | undefined)?.event === "success";

class AuthKv {
  auth: CodexAuthPoolState;
  reads = 0;
  routingReads = 0;
  routingCommitFailures = 0;
  nextReadGate: Promise<void> | null = null;
  onRoutingRead: ((read: number) => void | Promise<void>) | null = null;
  providerHealthSuccessCommitGate: Promise<void> | null = null;
  onProviderHealthSuccessCommit: (() => void) | null = null;
  onProviderHealthSuccessCommitted: (() => void) | null = null;
  authVersion = 1;
  readonly extra = new Map<string, { value: unknown; version: number }>();

  constructor(auth: CodexAuthPoolState) {
    this.auth = auth;
  }

  get<T>(key: Deno.KvKey, options?: { consistency?: "strong" | "eventual" }): Promise<Deno.KvEntryMaybe<T>> {
    assert.equal(options?.consistency, "strong");
    if (JSON.stringify(key) !== JSON.stringify(AUTH_KEY)) {
      const routingRead = ++this.routingReads;
      const hook = this.onRoutingRead;
      return Promise.resolve(hook?.(routingRead)).then(() => {
        const entry = this.extra.get(JSON.stringify(key));
        return {
          key,
          value: (entry?.value ?? null) as T | null,
          versionstamp: entry ? String(entry.version).padStart(20, "0") : null,
        } as Deno.KvEntryMaybe<T>;
      });
    }
    this.reads += 1;
    const value = this.auth as T;
    const versionstamp = String(this.authVersion).padStart(20, "0");
    const gate = this.nextReadGate;
    this.nextReadGate = null;
    return (gate ?? Promise.resolve()).then(() => ({ key, value, versionstamp }));
  }

  getMany<T extends readonly unknown[]>(
    keys: readonly Deno.KvKey[],
    options?: { consistency?: "strong" | "eventual" }
  ): Promise<{ [K in keyof T]: Deno.KvEntryMaybe<T[K]> }> {
    assert.equal(options?.consistency, "strong");
    // Capture every value and versionstamp synchronously, before any
    // routing-read hook or auth-read gate can mutate the fixture, so the
    // resolved tuple is one coherent snapshot.
    const snapshot = keys.map((key) => this.#snapshotRead(key));
    const gate = snapshot.some((entry) => entry.authRead) ? this.nextReadGate : null;
    if (gate !== null) this.nextReadGate = null;
    return Promise.resolve()
      .then(async () => {
        for (const entry of snapshot) {
          if (entry.routingRead !== null) await this.onRoutingRead?.(entry.routingRead);
        }
      })
      .then(() => gate ?? undefined)
      .then(
        () =>
          snapshot.map((entry) => entry.entry) as {
            [K in keyof T]: Deno.KvEntryMaybe<T[K]>;
          }
      );
  }

  /** One synchronous row snapshot plus the read accounting its `get` would record. */
  #snapshotRead(key: Deno.KvKey): Readonly<{ entry: Deno.KvEntryMaybe<unknown>; authRead: boolean; routingRead: number | null }> {
    if (isAuthKey(key)) {
      this.reads += 1;
      return {
        entry: { key, value: this.auth, versionstamp: String(this.authVersion).padStart(20, "0") },
        authRead: true,
        routingRead: null,
      };
    }
    const routingRead = ++this.routingReads;
    const stored = this.extra.get(JSON.stringify(key));
    return {
      entry:
        stored === undefined ? { key, value: null, versionstamp: null } : { key, value: stored.value, versionstamp: String(stored.version).padStart(20, "0") },
      authRead: false,
      routingRead,
    };
  }

  set(key: Deno.KvKey, value: unknown): Promise<Deno.KvCommitResult> {
    if (JSON.stringify(key) === JSON.stringify(AUTH_KEY)) {
      this.auth = value as CodexAuthPoolState;
      this.authVersion += 1;
    } else {
      const encoded = JSON.stringify(key);
      this.extra.set(encoded, { value, version: (this.extra.get(encoded)?.version ?? 0) + 1 });
    }
    return Promise.resolve({ ok: true, versionstamp: "00000000000000000001" });
  }

  atomic(): Deno.AtomicOperation {
    const checks: { key: Deno.KvKey; versionstamp: string | null }[] = [];
    const writes: FakeKvWrite[] = [];
    const chain = {
      check: (...entries: { key: Deno.KvKey; versionstamp: string | null }[]) => {
        checks.push(...entries);
        return chain;
      },
      set: (key: Deno.KvKey, value: unknown, _options?: { expireIn?: number }) => {
        writes.push({ type: "set", key, value });
        return chain;
      },
      delete: (key: Deno.KvKey) => {
        writes.push({ type: "delete", key });
        return chain;
      },
      commit: async () => {
        const providerHealthSuccess = writes.some(isProviderHealthSuccessWrite);
        if (providerHealthSuccess) {
          this.onProviderHealthSuccessCommit?.();
          await this.providerHealthSuccessCommitGate;
        }
        if (this.routingCommitFailures > 0 && writes.some((write) => !isAuthKey(write.key))) {
          this.routingCommitFailures -= 1;
          return Promise.resolve({ ok: false } as const);
        }
        for (const check of checks) {
          if (this.#checkVersion(check.key) !== check.versionstamp) return Promise.resolve({ ok: false } as const);
        }
        for (const write of writes) this.#applyWrite(write);
        if (providerHealthSuccess) this.onProviderHealthSuccessCommitted?.();
        return Promise.resolve({ ok: true, versionstamp: "00000000000000000001" } as const);
      },
    };
    return chain as unknown as Deno.AtomicOperation;
  }

  /** Versionstamp the atomic check for `key` must match, or null when absent. */
  #checkVersion(key: Deno.KvKey): string | null {
    if (isAuthKey(key)) return String(this.authVersion).padStart(20, "0");
    const encoded = JSON.stringify(key);
    const entry = this.extra.get(encoded);
    return entry === undefined ? null : String(entry.version).padStart(20, "0");
  }

  /** Applies one committed atomic write to the in-memory store. */
  #applyWrite(write: FakeKvWrite): void {
    if (isAuthKey(write.key)) {
      if (write.type === "set") this.auth = write.value as CodexAuthPoolState;
      this.authVersion += 1;
      return;
    }
    const encoded = JSON.stringify(write.key);
    if (write.type === "delete") this.extra.delete(encoded);
    else this.extra.set(encoded, { value: write.value, version: (this.extra.get(encoded)?.version ?? 0) + 1 });
  }
}

const fixedStartMs = 1_000_000;
const utf8ByteLength = (value: string): number => new TextEncoder().encode(value).byteLength;
/** URL of a fetch input, for the request-shape fixtures below. */
const requestUrl = (input: RequestInfo | URL): string => {
  if (typeof input === "string") return input;
  if (input instanceof URL) return input.toString();
  return input.url;
};

/** A transport rejection reason as an Error; a DOMException already is one. */
const abortReasonError = (reason: unknown): Error => (reason instanceof Error ? reason : new Error(`transport aborted: ${String(reason)}`, { cause: reason }));
const encodeBase64Url = (value: unknown): string =>
  btoa(JSON.stringify(value))
    .replace(/\+/g, "-")
    .replace(/\//g, "_")
    .replace(/={1,2}$/, "");
const accessToken = (label: string): string => `${encodeBase64Url({ alg: "none" })}.${encodeBase64Url({ exp: (fixedStartMs + 60 * 60_000) / 1000 })}.${label}`;
const auth = (label: string): CodexAuthState => ({
  access_token: accessToken(label),
  refresh_token: `refresh-${label}`,
  account_id: `account-${label}`,
  updated_at_ms: fixedStartMs,
});
const staleAuth = (label: string): CodexAuthState => ({
  ...auth(label),
  access_token: `${encodeBase64Url({ alg: "none" })}.${encodeBase64Url({ exp: (fixedStartMs + 30_000) / 1000 })}.${label}`,
});
const pool = (...accounts: CodexAuthState[]): CodexAuthPoolState => ({
  accounts,
  updated_at_ms: fixedStartMs,
});

const kv = new AuthKv(pool(auth("old")));
setKvForTest(kv as unknown as Deno.Kv);

const { config } = await import("../src/config.ts");

const {
  cacheCodexAuthPool,
  CODEX_AUTH_REAUTH_WARNING,
  CODEX_QUOTA_BLOCKED_ERROR_CODE,
  CodexError,
  beginCodexCacheScopeExperiment,
  fetchCodexResponses,
  fetchCodexResponsesForCacheScopeExperiment,
  getCodexResponseActiveTelemetry,
  getCodexResponseSlot,
  getCodexRoutingError,
  getCodexRoutingProbe,
  markCodexResponseCompleted,
  orderCodexAuthAccounts,
  releaseCodexResponseProbe,
  resetCodexAuthCacheForTest,
} = await import("../src/codex.ts");
const {
  claimCodexRoutingProbe,
  CODEX_ACCOUNT_ROUTING_KV_KEY,
  CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY,
  CODEX_HALF_OPEN_LEASE_MS,
  CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS,
  markCodexQuotaBlocked,
  markCodexUpstreamTimeout,
  parseCodexActiveAccountSelection,
  parseCodexAccountRoutingState,
  recordCodexCapacityRoutingObservations,
  resetCodexAccountRoutingForTest,
  selectCodexRoutingAccounts,
  selectCodexRoutingAccountsStrong,
} = await import("../src/codex_account_routing.ts");
const { resetProviderHealthThrottleForTest } = await import("../src/provider_health.ts");

Deno.test("Codex auth account ordering rotates from the selected account", () => {
  const accounts = [auth("one"), auth("two")];
  assert.deepEqual(
    orderCodexAuthAccounts(accounts, 0).map((candidate) => candidate.account_id),
    ["account-one", "account-two"]
  );
  assert.deepEqual(
    orderCodexAuthAccounts(accounts, 1).map((candidate) => candidate.account_id),
    ["account-two", "account-one"]
  );
});

Deno.test("repeated requests preserve subscription account order", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = (_input, init) => {
    accountIds.push(new Headers(init?.headers).get("ChatGPT-Account-ID") ?? "missing");
    return Promise.resolve(new Response("{}", { status: 200 }));
  };

  try {
    for (let iteration = 0; iteration < 2; iteration += 1) {
      const response = await fetchCodexResponses({ model: "gpt-5.6-luna", input: "stable routing order" }, {});
      assert.equal(response.status, 200);
      await markCodexResponseCompleted(response);
    }
    assert.deepEqual(accountIds, ["account-one", "account-one"]);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("AuthKv strong getMany snapshots auth and routing rows before a routing-read hook mutates them", async () => {
  const fixture = new AuthKv(pool(auth("old")));
  const routingKey = CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY;
  const initialRoutingRow = { v: 1, generation: 1 } as const;
  fixture.extra.set(JSON.stringify(routingKey), { value: initialRoutingRow, version: 3 });
  const rotatedAuth = pool(auth("rotated"));
  const rotatedRoutingRow = { v: 1, generation: 2 } as const;
  let mutated = false;
  // The hook fires only after getMany captured the tuple; replacing both rows
  // here must not leak into the snapshot it returns.
  fixture.onRoutingRead = () => {
    if (mutated) return;
    mutated = true;
    fixture.auth = rotatedAuth;
    fixture.authVersion += 1;
    fixture.extra.set(JSON.stringify(routingKey), { value: rotatedRoutingRow, version: 4 });
  };

  try {
    const snapshot = await fixture.getMany<[CodexAuthPoolState, Readonly<{ v: number; generation: number }>]>([AUTH_KEY, routingKey], {
      consistency: "strong",
    });
    assert.equal(mutated, true, "the routing read hook must observe the read");
    assert.deepEqual(snapshot[0].value, pool(auth("old")));
    assert.equal(snapshot[0].versionstamp, "00000000000000000001");
    assert.deepEqual(snapshot[1].value, initialRoutingRow);
    assert.equal(snapshot[1].versionstamp, "00000000000000000003");

    // A fresh strong read observes both rows the hook wrote.
    const fresh = await fixture.getMany<[CodexAuthPoolState, Readonly<{ v: number; generation: number }>]>([AUTH_KEY, routingKey], { consistency: "strong" });
    assert.deepEqual(fresh[0].value, rotatedAuth);
    assert.equal(fresh[0].versionstamp, "00000000000000000002");
    assert.deepEqual(fresh[1].value, rotatedRoutingRow);
    assert.equal(fresh[1].versionstamp, "00000000000000000004");
    assert.equal(fixture.reads, 2);
    assert.equal(fixture.routingReads, 2);
  } finally {
    fixture.onRoutingRead = null;
  }
});

Deno.test("retired affinity rows are never written by terminal completion", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  const retiredKey = ["uos_ai", "codex_account_affinity", "v1", "retired-terminal-fixture"] as const;
  const retiredRow = { account_cohort_hash: "uos-prompt-cache-account-cohort-v1\u0000account-one", expires_at_ms: fixedStartMs + 60_000 };
  await kv.set(retiredKey, retiredRow);
  const storedBefore = kv.extra.get(JSON.stringify(retiredKey));
  const accountIds: string[] = [];
  globalThis.fetch = (_input, init) => {
    accountIds.push(new Headers(init?.headers).get("ChatGPT-Account-ID") ?? "missing");
    return Promise.resolve(new Response("{}", { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "terminal affinity", prompt_cache_key: "retired-terminal-key" },
      { cacheScope: "api-key:terminal-affinity" }
    );
    assert.equal(response.status, 200);
    assert.deepEqual(accountIds, ["account-one"]);
    assert.deepEqual(kv.extra.get(JSON.stringify(retiredKey)), storedBefore, "inference never reads or rewrites retired affinity rows");

    await markCodexResponseCompleted(response);
    assert.deepEqual(kv.extra.get(JSON.stringify(retiredKey)), storedBefore, "terminal completion never rewrites retired affinity rows");
    assert.deepEqual(getCodexResponseActiveTelemetry(response), { activeGeneration: 1, activeTransitionReason: null });
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a durable routing-state outage fails retryably before dispatch", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalOpenKv = Object.getOwnPropertyDescriptor(Deno, "openKv");
  let providerCalls = 0;
  let beforeDispatchCalls = 0;
  let transportStarts = 0;
  Date.now = () => fixedStartMs;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  cacheCodexAuthPool(kv.auth);
  Object.defineProperty(Deno, "openKv", { value: undefined, configurable: true });
  setKvForTest(null);
  globalThis.fetch = () => {
    providerCalls += 1;
    return Promise.resolve(new Response("{}", { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { model: "gpt-5.6-luna", input: "wait for KV" },
      {
        beforeDispatch: () => {
          beforeDispatchCalls += 1;
          return Promise.resolve({
            markTransportStarted: () => {
              transportStarts += 1;
            },
            cancelBeforeTransport: () => Promise.resolve(),
          });
        },
      }
    );
    // Unavailable durable routing state is a retryable error before dispatch:
    // the gateway never guesses a sibling or deletes existing state.
    assert.equal(response.status, 503);
    assert.equal(providerCalls, 0);
    assert.equal(beforeDispatchCalls, 0);
    assert.equal(transportStarts, 0);
    await response.arrayBuffer();
  } finally {
    if (originalOpenKv) Object.defineProperty(Deno, "openKv", originalOpenKv);
    setKvForTest(kv as unknown as Deno.Kv);
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
  }
});

Deno.test("concurrent Codex requests dispatch without gateway admission rejection", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let providerCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();

  globalThis.fetch = async () => {
    providerCalls += 1;
    await new Promise((resolve) => setTimeout(resolve, 5));
    return new Response("{}", { status: 200, headers: { "Content-Type": "application/json" } });
  };

  try {
    const responses = await Promise.all(Array.from({ length: 8 }, (_, index) => fetchCodexResponses({ model: "gpt-5.6-luna", input: `agent-${index}` })));
    assert.equal(providerCalls, 8);
    assert.deepEqual(
      responses.map((response) => response.status),
      Array(8).fill(200)
    );
    await Promise.all(responses.map(markCodexResponseCompleted));
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("Codex responses use the native prompt-cache wire contract and stable keyed sessions", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const requests: Request[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = (input, init) => {
    requests.push(new Request(input, init));
    return Promise.resolve(new Response("{}", { status: 200 }));
  };

  const cacheableBody = {
    model: "gpt-5.6-luna",
    prompt_cache_key: "stable-cache-key",
    prompt_cache_options: { mode: "explicit", ttl: "30m" },
    prompt_cache_retention: "24h",
    max_output_tokens: 64,
    max_completion_tokens: 64,
    input: [
      {
        type: "message",
        role: "user",
        content: [
          {
            type: "input_text",
            text: "stable prefix",
            prompt_cache_breakpoint: { mode: "explicit" },
          },
        ],
      },
    ],
    tools: [
      {
        type: "function",
        name: "cache_schema_fixture",
        parameters: {
          type: "object",
          properties: { prompt_cache_breakpoint: { type: "string" } },
        },
      },
    ],
  };
  const originalBody = structuredClone(cacheableBody);

  try {
    const first = await fetchCodexResponses(cacheableBody, { cacheScope: "principal-one" });
    const second = await fetchCodexResponses(cacheableBody, { cacheScope: "principal-one" });
    const differentKey = await fetchCodexResponses({ ...cacheableBody, prompt_cache_key: "different-cache-key" }, { cacheScope: "principal-one" });
    const differentPrincipal = await fetchCodexResponses(cacheableBody, { cacheScope: "principal-two" });
    await fetchCodexResponses(cacheableBody);
    await fetchCodexResponses(cacheableBody);
    await fetchCodexResponses({ model: "gpt-5.6-luna", input: "no key" }, { cacheScope: "principal-one" });
    await fetchCodexResponses({ model: "gpt-5.6-luna", input: "no key" }, { cacheScope: "principal-one" });

    assert.deepEqual(cacheableBody, originalBody);
    assert.equal(requests.length, 8);
    const bodies = await Promise.all(requests.map((request) => request.clone().json() as Promise<Record<string, unknown>>));
    const firstBody = bodies[0];
    assert.equal(firstBody.prompt_cache_key, "stable-cache-key");
    assert.equal("prompt_cache_options" in firstBody, false);
    assert.equal("prompt_cache_retention" in firstBody, false);
    assert.equal("max_output_tokens" in firstBody, false);
    assert.equal("max_completion_tokens" in firstBody, false);
    const input = firstBody.input as Record<string, unknown>[];
    const content = input[0]?.content as Record<string, unknown>[];
    assert.equal("prompt_cache_breakpoint" in content[0], false);
    const tools = firstBody.tools as Record<string, unknown>[];
    assert.deepEqual(tools[0], cacheableBody.tools[0]);
    assert.deepEqual(bodies[1], firstBody);

    const identityHeaders = ["conversation_id", "session-id", "thread-id", "x-client-request-id"] as const;
    const stableIdentity = requests[0].headers.get("conversation_id");
    assert.match(stableIdentity ?? "", /^[0-9a-f]{8}-[0-9a-f]{4}-8[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
    for (const header of identityHeaders) {
      assert.equal(requests[0].headers.get(header), stableIdentity);
      assert.equal(requests[1].headers.get(header), stableIdentity);
      assert.notEqual(requests[2].headers.get(header), stableIdentity);
      assert.notEqual(requests[3].headers.get(header), stableIdentity);
    }
    for (const request of requests.slice(4)) {
      assert.equal(request.headers.get("session-id"), null);
      assert.equal(request.headers.get("thread-id"), null);
      assert.equal(request.headers.get("x-client-request-id"), null);
    }
    assert.notEqual(requests[4].headers.get("conversation_id"), requests[5].headers.get("conversation_id"));
    assert.notEqual(requests[6].headers.get("conversation_id"), requests[7].headers.get("conversation_id"));

    const expectedWarnings = ["prompt_cache_options_ignored", "prompt_cache_retention_ignored", "max_output_tokens_ignored", "prompt_cache_breakpoint_ignored"];
    for (const response of [first, second, differentKey, differentPrincipal]) {
      const warnings =
        response.headers
          .get("x-uos-warning")
          ?.split(",")
          .map((value) => value.trim()) ?? [];
      assert.deepEqual(warnings, expectedWarnings);
    }
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("Codex responses retry the same active account after an account-level 429", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    if (accountIds.length > 2) throw new Error("a bounded 429 retry must not dispatch a third attempt");
    return Promise.resolve(new Response("{}", { status: accountIds.length === 1 ? 429 : 200 }));
  };

  try {
    const response = await fetchCodexResponses({ input: "balance" });
    assert.equal(response.status, 200);
    // The serial active account owns the one bounded retry: a sibling is never
    // dispatched for an account-level 429.
    assert.deepEqual(accountIds, ["account-one", "account-one"]);
    assert.equal(accountIds.length, 2);
    assert.equal(getCodexResponseSlot(response), 1);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("Codex responses keep the durable active account over sibling dashboard headroom", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(new Response("{}", { status: 200 }));
  };

  try {
    const initial = await selectCodexRoutingAccounts(kv.auth, kv.auth.accounts, fixedStartMs);
    assert.equal(initial.kind, "eligible");

    const accountTwo = initial.accounts.find((account) => account.auth.account_id === "account-two");
    assert.ok(accountTwo);

    const blockedUntil = fixedStartMs + 60_000;
    await markCodexQuotaBlocked(
      accountTwo,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(blockedUntil).toUTCString() },
      }),
      fixedStartMs - 1
    );
    await kv.set(PROVIDER_CAPACITY_SNAPSHOT_KEY, {
      snapshot_at_ms: fixedStartMs,
      sources: [
        {
          source: "codex",
          slot: 2,
          state: "available",
          source_observed_at_ms: fixedStartMs,
          snapshot_at_ms: fixedStartMs,
          windows: {
            primary: { limit_window_seconds: 604_800, used_percent: 100, reset_at_ms: fixedStartMs + 604_800_000 },
            secondary: null,
          },
          additional_rate_limits: [
            {
              limit_name: "GPT-5.3-Codex-Spark",
              metered_feature: "codex_bengalfox",
              windows: {
                primary: { limit_window_seconds: 18_000, used_percent: 50, reset_at_ms: fixedStartMs + 18_000_000 },
                secondary: null,
              },
            },
          ],
        },
      ],
    });
    resetCodexAccountRoutingForTest();

    const response = await fetchCodexResponses({ model: "gpt-5.3-codex-spark", input: "dashboard-positive" });
    assert.equal(response.status, 200);
    // Fresh sibling dashboard headroom is not a transition reason: the durable
    // active account stays selected.
    assert.deepEqual(accountIds, ["account-one"]);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("Codex responses return a raw 403 without sibling failover", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    if (accountIds.length > 1) throw new Error("a raw 403 with a valid bearer must not dispatch a sibling");
    return Promise.resolve(new Response("{}", { status: 403 }));
  };

  try {
    const response = await fetchCodexResponses({ input: "forbidden-failover" });
    assert.equal(response.status, 403);
    assert.deepEqual(accountIds, ["account-one"]);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("Codex responses do not replay a dispatched transport failure", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    if (accountIds.length === 1) {
      return Promise.reject(new DOMException("upstream socket closed", "TimeoutError"));
    }
    return Promise.resolve(new Response("{}", { status: 200 }));
  };

  try {
    await assert.rejects(
      () => fetchCodexResponses({ input: "timeout-sibling-retry" }, {}),
      (error: unknown) => error instanceof CodexError && error.code === "gateway_timeout"
    );
    assert.deepEqual(accountIds, ["account-one"]);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("transport failures preserve retired affinity rows and provider health", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  const retiredKey = ["uos_ai", "codex_account_affinity", "v1", "retired-transport-fixture"] as const;
  const retiredRow = { account_cohort_hash: "uos-prompt-cache-account-cohort-v1\u0000account-one", expires_at_ms: fixedStartMs + 60_000 };
  await kv.set(retiredKey, retiredRow);
  const affinityBefore = kv.extra.get(JSON.stringify(retiredKey));
  globalThis.fetch = (_input, init) => {
    accountIds.push(new Headers(init?.headers).get("ChatGPT-Account-ID") ?? "missing");
    return Promise.reject(new DOMException("upstream socket closed", "TimeoutError"));
  };

  try {
    await assert.rejects(
      () =>
        fetchCodexResponses(
          { input: "all siblings fail", prompt_cache_key: "transport-failure-affinity-key" },
          { cacheScope: "api-key:transport-failure-affinity" }
        ),
      (error: unknown) => error instanceof CodexError && error.code === "gateway_timeout" && error.status === 504
    );
    assert.deepEqual(accountIds, ["account-one"]);
    assert.deepEqual(kv.extra.get(JSON.stringify(retiredKey)), affinityBefore);
    // A transient timeout never advances or rolls back the durable active
    // generation, and it never dispatches a sibling or paid provider.
    const active = parseCodexActiveAccountSelection(kv.extra.get(JSON.stringify(CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY))?.value);
    assert.equal(active?.generation, 1);
    assert.equal(active.slot, 0);
    for (const encoded of kv.extra.keys()) {
      const key = JSON.parse(encoded) as Deno.KvKey;
      assert.notEqual(key[1], "provider_health", `transport failure mutated provider health at ${encoded}`);
    }
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("post-dispatch client cancellation stops Codex transport", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const transportStarted: VoidDeferred = Promise.withResolvers();
  const requestAbort = new AbortController();
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (_input, init) =>
    new Promise<Response>((_resolve, reject) => {
      const signal = init?.signal;
      assert.ok(signal);
      transportStarted.resolve();
      const rejectFromSignal = (): void => {
        reject(abortReasonError(signal.reason));
      };
      if (signal.aborted) rejectFromSignal();
      else signal.addEventListener("abort", rejectFromSignal, { once: true });
    });

  try {
    const pending = fetchCodexResponses(
      { input: "cancel after dispatch" },
      {
        signal: requestAbort.signal,
      }
    );
    await transportStarted.promise;
    requestAbort.abort(new DOMException("client disconnected", "AbortError"));
    await assert.rejects(
      () => pending,
      (error: unknown) => error instanceof Error && error.name === "AbortError"
    );
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("Codex responses make one bounded same-active retry when a generic 429 repeats", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const serializedBodies: string[] = [];
  const retryDelays: number[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    const serializedBody = init?.body;
    if (typeof serializedBody !== "string") throw new Error("Expected Codex request body to be a serialized string.");
    serializedBodies.push(serializedBody);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    if (accountIds.length > 2) throw new Error("a generic 429 gets exactly one same-active bounded retry");
    return Promise.resolve(
      new Response(JSON.stringify({ error: { type: "rate_limit_error" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": "1" },
      })
    );
  };

  try {
    const response = await fetchCodexResponses(
      { input: "bounded-retry" },
      {
        requestId: "request-bounded-retry",
        retrySleep: (milliseconds) => {
          retryDelays.push(milliseconds);
          return Promise.resolve();
        },
      }
    );
    // A generic rate_limit_error is not quota exhaustion, so the one bounded
    // retry stays on the same active account and the second 429 is final.
    assert.equal(response.status, 429);
    assert.deepEqual(accountIds, ["account-one", "account-one"]);
    assert.deepEqual(retryDelays, [1_000]);
    const expectedSerializedBody = JSON.stringify({ input: "bounded-retry" });
    assert.equal(utf8ByteLength(expectedSerializedBody), 25);
    assert.deepEqual(serializedBodies, [expectedSerializedBody, expectedSerializedBody]);
    assert.deepEqual(serializedBodies.map(utf8ByteLength), [25, 25]);
    assert.equal(
      serializedBodies.reduce((total, body) => total + utf8ByteLength(body), 0),
      50
    );
    await response.arrayBuffer();
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("Codex 429 retry sleep normalizes a shared timeout as a gateway timeout", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const controller = new AbortController();
  const timeoutReason = new DOMException("request deadline exceeded", "TimeoutError");
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(
      new Response(JSON.stringify({ error: { type: "rate_limit_error" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": "1" },
      })
    );
  };

  try {
    await assert.rejects(
      () =>
        fetchCodexResponses(
          { input: "retry-timeout" },
          {
            signal: controller.signal,
            retrySleep: () => {
              queueMicrotask(() => {
                controller.abort(timeoutReason);
              });
              return new Promise<void>(() => {});
            },
          }
        ),
      (error: unknown) => {
        if (!(error instanceof CodexError)) return false;
        assert.equal(error.code, "gateway_timeout");
        assert.equal(error.status, 504);
        assert.equal((error as Error & { cause?: unknown }).cause, timeoutReason);
        return true;
      }
    );
    assert.deepEqual(accountIds, ["account-one"]);
    assert.equal(accountIds.length, 1, "the retry transport never began");
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("Codex 429 retry sleep preserves ordinary cancellation", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const controller = new AbortController();
  const abortReason = new DOMException("client disconnected", "AbortError");
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(
      new Response(JSON.stringify({ error: { type: "rate_limit_error" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": "1" },
      })
    );
  };

  try {
    await assert.rejects(
      () =>
        fetchCodexResponses(
          { input: "retry-cancelled" },
          {
            signal: controller.signal,
            retrySleep: () => {
              queueMicrotask(() => {
                controller.abort(abortReason);
              });
              return new Promise<void>(() => {});
            },
          }
        ),
      (error: unknown) => {
        assert.equal(error, abortReason);
        return true;
      }
    );
    assert.deepEqual(accountIds, ["account-one"]);
    assert.equal(accountIds.length, 1, "the retry transport never began");
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("an expired generic 429 retry preserves the subsequent raw 403 response", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  let now = fixedStartMs;
  Date.now = () => now;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    if (accountIds.length === 1) {
      return Promise.resolve(
        Response.json(
          {
            error: {
              message: "Active account temporarily rate limited",
              type: "rate_limit_error",
              code: "rate_limit_exceeded",
            },
          },
          { status: 429 }
        )
      );
    }
    if (accountIds.length === 2) {
      now = fixedStartMs + 5_000;
      return Promise.resolve(
        Response.json(
          {
            error: {
              message: "Active account forbidden",
              type: "invalid_request_error",
              code: "active_account_forbidden",
            },
          },
          { status: 403 }
        )
      );
    }
    throw new Error("a raw 403 is terminal and must not dispatch a third attempt");
  };

  try {
    const response = await fetchCodexResponses(
      { input: "expired-generic-retry" },
      {
        requestId: "request-expired-generic-retry",
        retrySleep: () => Promise.resolve(),
      }
    );
    // The generic 429's one bounded retry stays on the active account, and its
    // raw 403 is the final response.
    assert.equal(response.status, 403);
    assert.deepEqual(accountIds, ["account-one", "account-one"]);
    assert.equal(accountIds.length, 2);
    assert.equal(now, fixedStartMs + 5_000);
    assert.deepEqual(await response.json(), {
      error: {
        message: "Active account forbidden",
        type: "invalid_request_error",
        code: "active_account_forbidden",
      },
    });
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("Codex routing logs attempts, refresh, and bounded retry without sensitive values", async () => {
  const originalFetch = globalThis.fetch;
  const originalInfo = console.info;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const lines: string[] = [];
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  console.info = (...args: unknown[]) => lines.push(args.map(String).join(" "));
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    if (request.url.includes("auth.openai.com/oauth/token")) {
      return Promise.resolve(
        new Response(
          JSON.stringify({
            access_token: "refreshed-secret-access",
            refresh_token: "refreshed-secret-refresh",
          }),
          { status: 200, headers: { "Content-Type": "application/json" } }
        )
      );
    }
    inferenceCalls += 1;
    const status = inferenceCalls === 1 ? 401 : 429;
    return Promise.resolve(
      new Response(JSON.stringify({ error: { message: "full-sensitive-upstream-error-body" } }), {
        status,
        headers: status === 429 ? { "Retry-After": "1" } : undefined,
      })
    );
  };

  try {
    const response = await fetchCodexResponses(
      { input: "redacted-logs" },
      {
        requestId: "request-redacted-logs",
        retrySleep: () => Promise.resolve(),
      }
    );
    assert.equal(response.status, 429);
    const output = lines.join("\n");
    assert.match(output, /"event":"codex_attempt"/);
    assert.match(output, /"event":"codex_token_refresh"/);
    assert.match(output, /"event":"codex_two_second_retry"/);
    assert.match(output, /"status_class":"401"/);
    assert.match(output, /"status_class":"429"/);
    for (const forbidden of [
      "account-one",
      "account-two",
      "access-one",
      "refresh-one",
      "refreshed-secret-access",
      "refreshed-secret-refresh",
      "full-sensitive-upstream-error-body",
    ]) {
      assert.equal(output.includes(forbidden), false, forbidden);
    }
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    console.info = originalInfo;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("Codex responses retry the other account when a 401 cannot refresh", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  let firstCancellationStarted = false;
  let refreshCancellationStarted = false;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    if (request.url.includes("auth.openai.com/oauth/token")) {
      return Promise.resolve(
        new Response(
          new ReadableStream<Uint8Array>({
            start(controller) {
              controller.enqueue(new TextEncoder().encode('{"error":"invalid_grant"}'));
            },
            cancel() {
              refreshCancellationStarted = true;
              return new Promise<void>(() => {});
            },
          }),
          { status: 401 }
        )
      );
    }
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    if (accountIds.length === 1) {
      return Promise.resolve(
        new Response(
          new ReadableStream<Uint8Array>({
            start(controller) {
              controller.enqueue(new TextEncoder().encode("{}"));
            },
            cancel() {
              firstCancellationStarted = true;
              return new Promise<void>(() => {});
            },
          }),
          { status: 401 }
        )
      );
    }
    return Promise.resolve(new Response("{}", { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses({ input: "auth-failover" });
    assert.equal(response.status, 200);
    assert.equal(accountIds.length, 2);
    assert.equal(new Set(accountIds).size, 2);
    assert.equal(firstCancellationStarted, true);
    assert.equal(refreshCancellationStarted, true);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a credential replacement landing after 401 is retried without an OAuth refresh", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const attempted = auth("one");
  const replacement: CodexAuthState = {
    ...attempted,
    access_token: accessToken("replacement"),
    refresh_token: "refresh-replacement",
    updated_at_ms: fixedStartMs + 1,
  };
  const authorizationHeaders: string[] = [];
  const serializedBodies: string[] = [];
  let oauthCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(attempted);
  kv.authVersion += 1;
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    if (request.url.includes("auth.openai.com/oauth/token")) {
      oauthCalls += 1;
      return Promise.resolve(new Response('{"error":"must_not_refresh_replacement"}', { status: 401 }));
    }
    const serializedBody = init?.body;
    if (typeof serializedBody !== "string") throw new Error("Expected Codex request body to be a serialized string.");
    serializedBodies.push(serializedBody);
    authorizationHeaders.push(request.headers.get("Authorization") ?? "");
    if (authorizationHeaders.length === 1) {
      kv.auth = pool(replacement);
      kv.authVersion += 1;
      return Promise.resolve(new Response("{}", { status: 401 }));
    }
    return Promise.resolve(new Response("{}", { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses({ input: "rotation-between-401-and-refresh" });
    assert.equal(response.status, 200);
    assert.equal(oauthCalls, 0);
    assert.deepEqual(authorizationHeaders, [`Bearer ${attempted.access_token}`, `Bearer ${replacement.access_token}`]);
    const expectedSerializedBody = JSON.stringify({ input: "rotation-between-401-and-refresh" });
    assert.equal(utf8ByteLength(expectedSerializedBody), 44);
    assert.deepEqual(serializedBodies, [expectedSerializedBody, expectedSerializedBody]);
    assert.deepEqual(serializedBodies.map(utf8ByteLength), [44, 44]);
    assert.equal(
      serializedBodies.reduce((total, body) => total + utf8ByteLength(body), 0),
      88
    );
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("Codex responses synthesize 401 only after every account has an invalid refresh credential", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  let refreshCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    if (request.url.includes("auth.openai.com/oauth/token")) {
      refreshCalls += 1;
      return Promise.resolve(new Response('{"error":"invalid_grant"}', { status: 401 }));
    }
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(new Response("{}", { status: 401 }));
  };

  try {
    const response = await fetchCodexResponses({ input: "auth-exhaustion" });
    assert.equal(response.status, 401);
    assert.equal(((await response.json()) as { error?: { code?: string } }).error?.code, "codex_auth_invalid");
    assert.equal(accountIds.length, 2);
    assert.equal(new Set(accountIds).size, 2);
    assert.equal(refreshCalls, 2);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("concurrent proactive refreshes share one OAuth exchange", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let refreshCalls = 0;
  let inferenceCalls = 0;
  let releaseRefresh = (): void => {};
  const refreshGate = new Promise<void>((resolve) => {
    releaseRefresh = resolve;
  });
  let releaseFirstRoutingRead = (): void => {};
  const firstRoutingReadGate = new Promise<void>((resolve) => {
    releaseFirstRoutingRead = resolve;
  });
  let routingReads = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(staleAuth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  // Hold only the first selector's routing read so every later caller queues on
  // the admission tail; never wait for eight strong reads, which would deadlock.
  kv.onRoutingRead = () => {
    routingReads += 1;
    if (routingReads === 1) return firstRoutingReadGate;
  };
  globalThis.fetch = async (input) => {
    const url = requestUrl(input);
    if (url.includes("auth.openai.com/oauth/token")) {
      refreshCalls += 1;
      await refreshGate;
      return new Response(JSON.stringify({ access_token: "refreshed-access", refresh_token: "refreshed-refresh" }), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      });
    }
    inferenceCalls += 1;
    return new Response("{}", { status: 200 });
  };

  try {
    const requests = Array.from({ length: 8 }, (_, index) => fetchCodexResponses({ input: `refresh-${index}` }));
    for (let attempt = 0; attempt < 100 && routingReads === 0; attempt += 1) {
      await new Promise((resolve) => setTimeout(resolve, 1));
    }
    assert.equal(routingReads, 1, "only the first queued selector may reach the strong read");
    releaseFirstRoutingRead();
    for (let attempt = 0; attempt < 100 && refreshCalls === 0; attempt += 1) {
      await new Promise((resolve) => setTimeout(resolve, 1));
    }
    assert.equal(refreshCalls, 1, "expected one refresh to begin");
    releaseRefresh();
    const responses = await Promise.all(requests);
    assert.equal(refreshCalls, 1);
    // Only non-200 responses are read, so a failure is diagnosable here without
    // changing the test just to inspect its body.
    const failedResponses: string[] = [];
    for (const [index, response] of responses.entries()) {
      if (response.status !== 200) failedResponses.push(`${index}: ${response.status} ${await response.text()}`);
    }
    assert.deepEqual(
      responses.map((response) => response.status),
      Array(8).fill(200),
      failedResponses.join("\n")
    );
    assert.equal(inferenceCalls, 8, "every response is a distinct ordinary inference");
  } finally {
    releaseFirstRoutingRead();
    releaseRefresh();
    kv.onRoutingRead = null;
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a failed strong selector read releases the admission queue for the next request", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = (_input, init) => {
    accountIds.push(new Headers(init?.headers).get("ChatGPT-Account-ID") ?? "missing");
    return Promise.resolve(new Response("{}", { status: 200 }));
  };
  kv.onRoutingRead = () => {
    throw new Error("routing KV unavailable");
  };

  try {
    const failed = await fetchCodexResponses({ input: "selector-failure" });
    assert.equal(failed.status, 503);
    await failed.arrayBuffer();

    // The failed selector released its queue, so the next request may bootstrap.
    kv.onRoutingRead = null;
    const recovered = await fetchCodexResponses({ input: "selector-recovery" });
    assert.equal(recovered.status, 200);
    assert.deepEqual(accountIds, ["account-one"]);
    await recovered.arrayBuffer();
  } finally {
    kv.onRoutingRead = null;
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a deterministic proactive refresh rejection quarantines the credential before inference", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let refreshCalls = 0;
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(staleAuth("invalid"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input) => {
    const url = requestUrl(input);
    if (url.includes("auth.openai.com/oauth/token")) {
      refreshCalls += 1;
      return Promise.resolve(new Response(JSON.stringify({ error: "invalid_grant" }), { status: 401 }));
    }
    inferenceCalls += 1;
    return Promise.resolve(new Response("{}", { status: 200 }));
  };

  try {
    const first = await fetchCodexResponses({ input: "expired-auth" });
    const second = await fetchCodexResponses({ input: "expired-auth-again" });
    assert.equal(first.status, 401);
    assert.equal(second.status, 401);
    assert.equal(first.headers.get("x-uos-warning"), CODEX_AUTH_REAUTH_WARNING);
    assert.equal(second.headers.get("x-uos-warning"), CODEX_AUTH_REAUTH_WARNING);
    assert.equal(refreshCalls, 1);
    assert.equal(inferenceCalls, 0);
    assert.equal(((await first.json()) as { error?: { code?: string } }).error?.code, "codex_auth_invalid");
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("refresh-token reuse returns an actionable re-auth warning without exposing the OAuth body", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(staleAuth("reused"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input) => {
    const url = requestUrl(input);
    if (url.includes("auth.openai.com/oauth/token")) {
      return Promise.resolve(
        new Response(
          JSON.stringify({
            error: {
              code: "refresh_token_reused",
              message: "provider secret must not escape",
            },
          }),
          { status: 400, headers: { "Content-Type": "application/json" } }
        )
      );
    }
    inferenceCalls += 1;
    return Promise.resolve(new Response("{}", { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses({ input: "reused-refresh-token" });
    assert.equal(response.status, 401);
    assert.equal(response.headers.get("x-uos-warning"), CODEX_AUTH_REAUTH_WARNING);
    const payload = (await response.json()) as { error?: { message?: string; code?: string } };
    assert.equal(payload.error?.code, "refresh_token_reused");
    assert.match(payload.error.message ?? "", /already used/i);
    assert.equal((payload.error.message ?? "").includes("provider secret"), false);
    assert.equal(inferenceCalls, 0);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a refresh failure warning survives a later quota-shaped 403 from another account", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(staleAuth("expired"), auth("quota"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    if (request.url.includes("auth.openai.com/oauth/token")) {
      return Promise.resolve(new Response(JSON.stringify({ error: "invalid_grant" }), { status: 401 }));
    }
    const accountId = request.headers.get("chatgpt-account-id") ?? "";
    accountIds.push(accountId);
    return Promise.resolve(
      new Response(JSON.stringify({ error: { message: "user quota is not enough" } }), {
        status: accountId === "account-quota" ? 403 : 200,
        headers: { "Content-Type": "application/json" },
      })
    );
  };

  try {
    const response = await fetchCodexResponses({ input: "expired-auth-with-quota-shaped-403" });
    assert.equal(response.status, 403);
    assert.equal(response.headers.get("x-uos-warning"), CODEX_AUTH_REAUTH_WARNING);
    assert.deepEqual(accountIds, ["account-quota"]);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a malformed successful refresh is transient and does not quarantine the credential", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let refreshCalls = 0;
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input) => {
    const url = requestUrl(input);
    if (url.includes("auth.openai.com/oauth/token")) {
      refreshCalls += 1;
      if (refreshCalls === 1) {
        return Promise.resolve(
          new Response(JSON.stringify({ refresh_token: "refresh-one" }), {
            status: 200,
            headers: { "Content-Type": "application/json" },
          })
        );
      }
      return Promise.resolve(
        new Response(JSON.stringify({ access_token: accessToken("recovered"), refresh_token: "refresh-recovered" }), {
          status: 200,
          headers: { "Content-Type": "application/json" },
        })
      );
    }
    inferenceCalls += 1;
    return Promise.resolve(new Response("{}", { status: inferenceCalls <= 2 ? 401 : 200 }));
  };

  try {
    await assert.rejects(
      () => fetchCodexResponses({ input: "malformed-refresh" }),
      (error: unknown) => error instanceof Error && "status" in error && error.status === 503 && error.message.includes("missing access_token")
    );
    const recovered = await fetchCodexResponses({ input: "valid-refresh" });
    assert.equal(recovered.status, 200);
    assert.equal(refreshCalls, 2);
    assert.equal(inferenceCalls, 3);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("direct failures release quota probes and timeouts do not gate the next request", async (t) => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let now = fixedStartMs;
  Date.now = () => now;
  (config as { isDeploy: boolean }).isDeploy = true;
  try {
    for (const testCase of [
      { name: "403", status: 403 },
      { name: "invalid 400", status: 400 },
      { name: "500", status: 500 },
      { name: "network", status: null },
      { name: "timeout", status: null, timeout: true },
    ] as const) {
      await t.step(testCase.name, async () => {
        now = fixedStartMs;
        kv.auth = pool(auth("one"));
        kv.extra.clear();
        resetCodexAuthCacheForTest();
        const initial = await selectCodexRoutingAccounts(kv.auth, kv.auth.accounts, now);
        assert.equal(initial.kind, "eligible");

        await markCodexQuotaBlocked(
          initial.accounts[0],
          new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
            status: 429,
            headers: { "Content-Type": "application/json", "Retry-After": "1" },
          }),
          now
        );

        now += 1_001;
        let codexCalls = 0;
        let timeoutController: AbortController | null = null;
        globalThis.fetch = (_input, init) => {
          codexCalls += 1;
          if (codexCalls > 1) return Promise.resolve(new Response("{}", { status: 200 }));
          if (testCase.timeout) {
            const reason = new DOMException("timed out", "TimeoutError");
            timeoutController?.abort(reason);
            return Promise.reject(abortReasonError(init?.signal?.reason ?? reason));
          }
          if (testCase.status === null) return Promise.reject(new TypeError("network fixture"));
          return Promise.resolve(new Response("{}", { status: testCase.status }));
        };

        if (testCase.timeout) {
          const abortController = new AbortController();
          timeoutController = abortController;
          await assert.rejects(
            () => fetchCodexResponses({ input: testCase.name }, { signal: abortController.signal }),
            (error: unknown) => error instanceof Error && "status" in error && error.status === 504
          );
        } else if (testCase.status === null) {
          await assert.rejects(
            () => fetchCodexResponses({ input: testCase.name }),
            (error: unknown) => error instanceof Error && "status" in error && error.status === 502
          );
        } else {
          const direct = await fetchCodexResponses({ input: testCase.name });
          assert.equal(direct.status, testCase.status);
        }

        const second = await fetchCodexResponses({ input: `${testCase.name}-second` });
        if (testCase.timeout) {
          // An in-flight timeout keeps the active account and its half-open
          // lease: the next admission is retryable and never dispatches a
          // speculative sibling or a paid provider.
          assert.equal(second.status, 503);
          assert.equal(codexCalls, 1);
        } else {
          assert.equal(second.status, 200);
          assert.equal(codexCalls, 2);
        }
      });
    }
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("cache-scope dispatch timeouts remain request-scoped", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const controller = new AbortController();
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  const session = await beginCodexCacheScopeExperiment();
  globalThis.fetch = (_input, init) => {
    inferenceCalls += 1;
    controller.abort(new DOMException("cache-scope timeout", "TimeoutError"));
    return Promise.reject(abortReasonError(init?.signal?.reason ?? controller.signal.reason));
  };

  try {
    await assert.rejects(
      () =>
        fetchCodexResponsesForCacheScopeExperiment(
          { input: "cache-scope-timeout" },
          {
            session,
            slot: 1,
            conversationId: "cache-scope-timeout-conversation",
            signal: controller.signal,
          }
        ),
      (error: unknown) => error instanceof CodexError && error.code === "gateway_timeout" && error.status === 504
    );
    resetCodexAccountRoutingForTest();
    const selected = await selectCodexRoutingAccounts(kv.auth, [kv.auth.accounts[0]], fixedStartMs);
    assert.equal(selected.kind, "eligible");

    assert.equal(selected.accounts[0]?.probeRequired, false);
    assert.equal(inferenceCalls, 1);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a legacy timeout probe cannot block provider transport", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  const initial = await selectCodexRoutingAccounts(kv.auth, kv.auth.accounts, fixedStartMs);
  assert.equal(initial.kind, "eligible");

  await markCodexUpstreamTimeout(initial.accounts[0], fixedStartMs - CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS - 1);
  globalThis.fetch = () => {
    inferenceCalls += 1;
    return Promise.resolve(new Response("transport proceeds", { status: 200 }));
  };

  try {
    // The normalized read discards the legacy timeout circuit, so transport
    // proceeds without any speculative sibling or paid dispatch.
    const response = await fetchCodexResponses({ input: "timeout-probe-race" });
    assert.equal(response.status, 200);
    assert.equal(await response.text(), "transport proceeds");
    assert.equal(inferenceCalls, 1);

    // A durable normalization write that cannot commit fails the admission
    // retryably before any provider transport.
    resetCodexAccountRoutingForTest();
    const reseeded = await selectCodexRoutingAccounts(kv.auth, kv.auth.accounts, fixedStartMs);
    assert.equal(reseeded.kind, "eligible");
    await markCodexUpstreamTimeout(reseeded.accounts[0], fixedStartMs - CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS - 1);
    resetCodexAccountRoutingForTest();
    kv.routingCommitFailures = 3;
    const blocked = await fetchCodexResponses({ input: "routing-commit-unavailable" });
    assert.equal(blocked.status, 503);
    assert.equal(inferenceCalls, 1);
    await blocked.arrayBuffer();
  } finally {
    kv.routingCommitFailures = 0;
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a bounded retry that proves quota keeps its quota retry classification", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let inferenceCalls = 0;
  let concurrentStatus: number | null = null;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  const initial = await selectCodexRoutingAccounts(kv.auth, kv.auth.accounts, fixedStartMs);
  assert.equal(initial.kind, "eligible");

  await markCodexUpstreamTimeout(initial.accounts[0], fixedStartMs - CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS - 1);
  globalThis.fetch = async () => {
    inferenceCalls += 1;
    if (inferenceCalls === 1) {
      return new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": "1" },
      });
    }
    if (inferenceCalls === 2) {
      concurrentStatus = (await fetchCodexResponses({ input: "timeout-probe-race-concurrent" })).status;
      return new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": "60" },
      });
    }
    return new Response("unexpected extra transport", { status: 200 });
  };

  try {
    const response = await fetchCodexResponses({ input: "timeout-probe-quota-retry" }, { retrySleep: () => Promise.resolve() });
    assert.equal(response.status, 429);
    // The concurrent admission sees the retry's half-open lease and reports the
    // same retryable quota classification instead of guessing a sibling or a
    // paid provider.
    assert.equal(concurrentStatus, 429);
    assert.equal(inferenceCalls, 2);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a timeout during bounded retry refresh preserves only the quota fence", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const controller = new AbortController();
  let inferenceCalls = 0;
  let refreshCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = (input, init) => {
    const url = requestUrl(input);
    if (url.includes("auth.openai.com/oauth/token")) {
      refreshCalls += 1;
      return Promise.resolve(
        new Response(JSON.stringify({ access_token: accessToken("one"), refresh_token: "refresh-one" }), {
          status: 200,
          headers: { "Content-Type": "application/json" },
        })
      );
    }
    inferenceCalls += 1;
    if (inferenceCalls === 1) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": "1" },
        })
      );
    }
    if (inferenceCalls === 2) return Promise.resolve(new Response("{}", { status: 401 }));
    controller.abort(new DOMException("bounded retry timeout", "TimeoutError"));
    return Promise.reject(abortReasonError(init?.signal?.reason ?? controller.signal.reason));
  };

  try {
    await assert.rejects(
      () =>
        fetchCodexResponses(
          { input: "bounded-retry-refresh-timeout" },
          {
            signal: controller.signal,
            retrySleep: () => Promise.resolve(),
          }
        ),
      (error: unknown) => error instanceof CodexError && error.code === "gateway_timeout" && error.status === 504
    );
    resetCodexAccountRoutingForTest();
    const selected = await selectCodexRoutingAccounts(kv.auth, kv.auth.accounts, fixedStartMs);
    assert.equal(selected.kind, "quota_blocked");
    assert.equal(inferenceCalls, 3);
    assert.equal(refreshCalls, 1);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("an already-aborted timeout signal does not open or dispatch an account circuit", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const controller = new AbortController();
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  controller.abort(new DOMException("deadline elapsed before dispatch", "TimeoutError"));
  globalThis.fetch = () => {
    inferenceCalls += 1;
    return Promise.reject(new DOMException("transport should not start", "TimeoutError"));
  };

  try {
    await assert.rejects(
      () => fetchCodexResponses({ input: "pre-dispatch-timeout" }, { signal: controller.signal }),
      (error: unknown) => error instanceof CodexError && error.code === "gateway_timeout" && error.status === 504
    );
    resetCodexAccountRoutingForTest();
    const selected = await selectCodexRoutingAccounts(kv.auth, kv.auth.accounts, fixedStartMs);
    assert.equal(selected.kind, "eligible");
    assert.equal(inferenceCalls, 0);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a 429 retry that proves invalid credentials remains quarantined", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let now = fixedStartMs;
  let inferenceCalls = 0;
  let refreshCalls = 0;
  Date.now = () => now;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  const initial = await selectCodexRoutingAccounts(kv.auth, kv.auth.accounts, now);
  assert.equal(initial.kind, "eligible");

  await markCodexQuotaBlocked(
    initial.accounts[0],
    new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
      status: 429,
      headers: { "Content-Type": "application/json", "Retry-After": "1" },
    }),
    now
  );
  now += 1_001;
  globalThis.fetch = (input) => {
    const url = requestUrl(input);
    if (url.includes("auth.openai.com/oauth/token")) {
      refreshCalls += 1;
      return Promise.resolve(new Response('{"error":"invalid_grant"}', { status: 401 }));
    }
    inferenceCalls += 1;
    if (inferenceCalls === 1) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": "1" },
        })
      );
    }
    return Promise.resolve(new Response("{}", { status: 401 }));
  };

  try {
    const first = await fetchCodexResponses({ input: "retry-invalid" }, { retrySleep: () => Promise.resolve() });
    assert.equal(first.status, 401);
    const second = await fetchCodexResponses({ input: "retry-invalid-again" });
    assert.equal(second.status, 401);
    assert.equal(inferenceCalls, 2);
    assert.equal(refreshCalls, 1);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a 401 after proactive refresh does not refresh the same account twice", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let refreshCalls = 0;
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(staleAuth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input) => {
    const url = requestUrl(input);
    if (url.includes("auth.openai.com/oauth/token")) {
      refreshCalls += 1;
      return Promise.resolve(
        new Response(
          JSON.stringify({
            access_token: accessToken("refreshed-once"),
            refresh_token: "refresh-refreshed-once",
          }),
          { status: 200, headers: { "Content-Type": "application/json" } }
        )
      );
    }
    inferenceCalls += 1;
    return Promise.resolve(new Response("{}", { status: 401 }));
  };

  try {
    const response = await fetchCodexResponses({ input: "one-refresh-only" });
    assert.equal(response.status, 401);
    assert.equal(refreshCalls, 1);
    assert.equal(inferenceCalls, 1);
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("every admission strongly reads the current durable credentials", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const authorizations: string[] = [];
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    authorizations.push(request.headers.get("authorization") ?? "");
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(new Response("{}", { status: 200 }));
  };

  try {
    kv.auth = pool(auth("old"));
    kv.extra.clear();
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();

    await fetchCodexResponses({ input: "cold" });
    assert.equal(authorizations.at(-1), `Bearer ${accessToken("old")}`);
    assert.equal(accountIds.at(-1), "account-old");

    // A rotation is observed on the very next admission: there is no warm TTL
    // that could keep serving a replaced credential.
    await kv.set(AUTH_KEY, pool(auth("rotated")));
    await fetchCodexResponses({ input: "rotated" });
    assert.equal(authorizations.at(-1), `Bearer ${accessToken("rotated")}`);
    assert.equal(accountIds.at(-1), "account-rotated");

    // Concurrent admissions converge on the same durable credential.
    await Promise.all(Array.from({ length: 4 }, (_, index) => fetchCodexResponses({ input: `concurrent-${index}` })));
    assert.deepEqual(authorizations.slice(-4), Array(4).fill(`Bearer ${accessToken("rotated")}`));
    assert.deepEqual(accountIds.slice(-4), Array(4).fill("account-rotated"));

    // A pool replacement between admissions is immediate as well.
    await kv.set(AUTH_KEY, pool(auth("replacement")));
    await fetchCodexResponses({ input: "replacement" });
    assert.equal(authorizations.at(-1), `Bearer ${accessToken("replacement")}`);
    assert.equal(accountIds.at(-1), "account-replacement");
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a valid persisted Codex pool is not overlaid by a local configured seed", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const originalSeed = config.codexAuthJsonB64;
  const authorizations: string[] = [];
  const persisted = auth("persisted");
  const localSeed = { ...auth("local-stale"), account_id: persisted.account_id };
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean; codexAuthJsonB64: string }).isDeploy = false;
  (config as { isDeploy: boolean; codexAuthJsonB64: string }).codexAuthJsonB64 = btoa(
    JSON.stringify({
      tokens: {
        access_token: localSeed.access_token,
        refresh_token: localSeed.refresh_token,
        account_id: localSeed.account_id,
      },
    })
  );
  kv.auth = pool(persisted);
  kv.extra.clear();
  const versionBefore = kv.authVersion;
  resetCodexAuthCacheForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    authorizations.push(request.headers.get("authorization") ?? "");
    return Promise.resolve(new Response("{}", { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses({ input: "persisted-authority" });
    assert.equal(response.status, 200);
    assert.deepEqual(authorizations, [`Bearer ${persisted.access_token}`]);
    assert.deepEqual(kv.auth, pool(persisted));
    assert.equal(kv.authVersion, versionBefore, "loading a persisted pool must not write a local seed into KV");
  } finally {
    resetCodexAuthCacheForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean; codexAuthJsonB64: string }).isDeploy = originalDeployFlag;
    (config as { isDeploy: boolean; codexAuthJsonB64: string }).codexAuthJsonB64 = originalSeed;
  }
});

const liveBankedResetConfig = (): CodexBankedResetConfig => ({
  enabled: true,
  mode: "live",
  maxGlobalPerDay: 1,
  maxPerAccountPerWindow: 1,
});

/**
 * The shared banked-reset request options used by the fixtures below: the live
 * config, the in-memory KV and the fixed clock are pinned per request id.
 */
const bankedResetRequestOptions = (requestId: string) => ({
  clientVersion: "0.145.0",
  requestId,
  bankedReset: {
    config: liveBankedResetConfig(),
    kv: kv as unknown as Deno.Kv,
    now: () => fixedStartMs,
    newOwnerToken: () => `owner-${requestId}`,
  },
});

const stableBankedResetRetryAfter = new Date(fixedStartMs + 60_000).toUTCString();

const seedStableBankedResetBlock = async (accountId = "account-one", observedAtMs = fixedStartMs): Promise<void> => {
  const initial = await selectCodexRoutingAccounts(kv.auth, kv.auth.accounts, fixedStartMs);
  assert.equal(initial.kind, "eligible");

  const account = initial.accounts.find((candidate) => candidate.auth.account_id === accountId);
  assert.ok(account);
  await markCodexQuotaBlocked(
    account,
    new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
      status: 429,
      headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
    }),
    observedAtMs
  );
  resetCodexAccountRoutingForTest();
};

const scriptedResetProvider = (
  options: Readonly<{
    verify?: boolean;
    redeemKind?: "completed" | "unknown" | "already_redeemed";
    onInventory?: () => void | Promise<void>;
    onRedeem?: () => void;
    onVerify?: () => void;
    redeemGate?: Promise<void>;
  }> = {}
) => {
  const calls: string[] = [];
  const inventoryAccountIds: string[] = [];
  const idempotencyKeys: string[] = [];
  const redeemAccountIds: string[] = [];
  const provider: CodexUsageResetProvider = {
    contract: {
      idempotency: { callerSupplied: true, retentionMs: 86_400_000 },
      lookup: { byIdempotencyKey: true, byProviderReceiptId: true },
      verification: { independentlyVerifiable: true },
      receiptIdsSafeToPersistAndLog: true,
      supportedResetTypes: ["codex_rate_limits"],
    },
    readInventory: async (input) => {
      calls.push("inventory");
      inventoryAccountIds.push(input.accountId);
      await options.onInventory?.();
      return {
        availableCount: 1,
        observedAtMs: fixedStartMs,
        credits: [{ id: "fixture-credit", status: "available", resetType: "codex_rate_limits", expiresAtMs: null }],
      };
    },
    redeem: async (input) => {
      calls.push("redeem");
      idempotencyKeys.push(input.idempotencyKey);
      redeemAccountIds.push(input.accountId);
      options.onRedeem?.();
      if (options.redeemGate) await options.redeemGate;
      if (options.redeemKind === "unknown") return { kind: "unknown", providerReceiptId: null };
      if (options.redeemKind === "already_redeemed") return { kind: "already_redeemed", providerReceiptId: "receipt-sanitized" };
      return { kind: "completed", providerReceiptId: "receipt-sanitized" };
    },
    lookup: (input) => {
      calls.push("lookup");
      idempotencyKeys.push(input.idempotencyKey);
      return Promise.resolve({ kind: "completed", providerReceiptId: "receipt-sanitized" });
    },
    verifyApplied: () => {
      calls.push("verify");
      options.onVerify?.();
      return Promise.resolve(options.verify ?? true);
    },
  };
  return { provider, calls, inventoryAccountIds, idempotencyKeys, redeemAccountIds };
};

Deno.test("banked reset exhausts normal routing, verifies, and retries the redeemed account once", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const reset = scriptedResetProvider();
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    if (accountIds.length === 1) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
        })
      );
    }
    return Promise.resolve(new Response(JSON.stringify({ id: "response-after-reset" }), { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-happy" },
      {
        requestId: "banked-reset-happy",
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-happy",
        },
      }
    );
    assert.equal(response.status, 200);
    assert.deepEqual(accountIds, ["account-one", "account-one"]);
    assert.deepEqual(reset.calls, ["inventory", "redeem", "verify"]);
    assert.equal(reset.idempotencyKeys.length, 1);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a partially blocked cohort is served by ordinary capacity with no reset-provider contact", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const originalCodexBaseUrl = config.codexBaseUrl;
  const requests: Readonly<{ url: string; method: string; headers: Headers; body: string; signal: AbortSignal | null }>[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean; codexBaseUrl: string }).isDeploy = true;
  (config as { isDeploy: boolean; codexBaseUrl: string }).codexBaseUrl = "https://upstream-reset.test/backend-api/codex";
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = async (input, init) => {
    const request = new Request(input, init);
    requests.push({
      url: request.url,
      method: request.method,
      headers: new Headers(request.headers),
      body: request.method === "POST" ? await request.text() : "",
      signal: init?.signal ?? null,
    });
    const accountId = request.headers.get("chatgpt-account-id");
    if (request.url.endsWith("/backend-api/codex/responses")) {
      if (accountId === "account-one" && requests.filter((entry) => entry.url.endsWith("/responses")).length === 1) {
        return new Response(
          JSON.stringify({
            error: {
              type: "usage_limit_reached",
              resets_at: Math.floor(Date.parse(stableBankedResetRetryAfter) / 1_000),
            },
          }),
          {
            status: 429,
            headers: { "Content-Type": "application/json" },
          }
        );
      }
      return new Response(JSON.stringify({ id: `response-${accountId}` }), { status: 200 });
    }
    if (request.url.endsWith("/backend-api/wham/rate-limit-reset-credits")) {
      return Response.json({
        available_count: 1,
        credits: [{ id: "expiring-credit", status: "available", reset_type: "codex_rate_limits", expires_at: null }],
      });
    }
    if (request.url.endsWith("/backend-api/wham/rate-limit-reset-credits/consume")) {
      return Response.json({ code: "reset", windows_reset: 1 });
    }
    throw new Error(`unexpected request ${request.method} ${request.url}`);
  };

  try {
    const seeded = await fetchCodexResponses({ input: "seed-partial-block" }, bankedResetRequestOptions("seed-partial-block"));
    assert.equal(seeded.status, 200);

    const shadowed = await fetchCodexResponses({ input: "shadow-partial-block" }, bankedResetRequestOptions("shadow-partial-block"));
    assert.equal(shadowed.status, 200);

    const duplicateShadow = await fetchCodexResponses({ input: "shadow-partial-block-duplicate" }, bankedResetRequestOptions("shadow-partial-block-duplicate"));
    assert.equal(duplicateShadow.status, 200);

    // Ordinary eligible capacity wins: no inventory read, no consume, and no
    // recovery inference for the blocked sibling.
    assert.deepEqual(
      requests.map((request) => `${request.method} ${new URL(request.url).pathname} ${request.headers.get("chatgpt-account-id")}`),
      [
        "POST /backend-api/codex/responses account-one",
        "POST /backend-api/codex/responses account-two",
        "POST /backend-api/codex/responses account-two",
        "POST /backend-api/codex/responses account-two",
      ]
    );
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean; codexBaseUrl: string }).isDeploy = originalDeployFlag;
    (config as { isDeploy: boolean; codexBaseUrl: string }).codexBaseUrl = originalCodexBaseUrl;
  }
});

Deno.test("persistent live never arms a partially blocked cohort while ordinary capacity serves", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const originalCodexBaseUrl = config.codexBaseUrl;
  const inventoryAccountIds: string[] = [];
  const consumeAccountIds: string[] = [];
  const inferenceAccountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean; codexBaseUrl: string }).isDeploy = true;
  (config as { isDeploy: boolean; codexBaseUrl: string }).codexBaseUrl = "https://upstream-reset.test/backend-api/codex";
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  await seedStableBankedResetBlock();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    const accountId = request.headers.get("chatgpt-account-id") ?? "";
    if (request.url.endsWith("/backend-api/codex/responses")) {
      inferenceAccountIds.push(accountId);
      return Promise.resolve(Response.json({ id: `response-${accountId}` }));
    }
    if (request.url.endsWith("/backend-api/wham/rate-limit-reset-credits")) {
      inventoryAccountIds.push(accountId);
      return Promise.resolve(
        Response.json({
          available_count: 1,
          credits: [{ id: `credit-${accountId}`, status: "available", reset_type: "codex_rate_limits", expires_at: null }],
        })
      );
    }
    if (request.url.endsWith("/backend-api/wham/rate-limit-reset-credits/consume")) {
      consumeAccountIds.push(accountId);
      return Promise.resolve(Response.json({ code: "reset", windows_reset: 1 }));
    }
    throw new Error(`unexpected request ${request.method} ${request.url}`);
  };

  try {
    const first = await fetchCodexResponses({ input: "persistent-live-partial-arm" }, bankedResetRequestOptions("persistent-live-partial-arm"));
    assert.equal(first.status, 200);
    const second = await fetchCodexResponses({ input: "persistent-live-partial-consume" }, bankedResetRequestOptions("persistent-live-partial-consume"));
    assert.equal(second.status, 200);

    // The healthy configured sibling serves both requests; the blocked account
    // never reaches inventory, redemption or recovery inference.
    assert.deepEqual(inferenceAccountIds, ["account-two", "account-two"]);
    assert.deepEqual(inventoryAccountIds, []);
    assert.deepEqual(consumeAccountIds, []);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean; codexBaseUrl: string }).isDeploy = originalDeployFlag;
    (config as { isDeploy: boolean; codexBaseUrl: string }).codexBaseUrl = originalCodexBaseUrl;
  }
});

Deno.test("persistent live auto-arms an all-blocked cohort before one later consume and reset retry", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const originalCodexBaseUrl = config.codexBaseUrl;
  const inventoryAccountIds: string[] = [];
  const consumeAccountIds: string[] = [];
  const inferenceAccountIds: string[] = [];
  const consumeBodies: unknown[] = [];
  const persistentLiveConfig: CodexBankedResetConfig = {
    ...liveBankedResetConfig(),
  };
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean; codexBaseUrl: string }).isDeploy = true;
  (config as { isDeploy: boolean; codexBaseUrl: string }).codexBaseUrl = "https://upstream-reset.test/backend-api/codex";
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  await seedStableBankedResetBlock("account-one");
  await seedStableBankedResetBlock("account-two");
  globalThis.fetch = async (input, init) => {
    const request = new Request(input, init);
    const accountId = request.headers.get("chatgpt-account-id") ?? "";
    if (request.url.endsWith("/backend-api/codex/responses")) {
      inferenceAccountIds.push(accountId);
      return Response.json({ id: `response-${accountId}` });
    }
    if (request.url.endsWith("/backend-api/wham/rate-limit-reset-credits")) {
      inventoryAccountIds.push(accountId);
      return Response.json({
        available_count: 1,
        credits: [
          {
            id: `credit-${accountId}`,
            status: "available",
            reset_type: "codex_rate_limits",
            expires_at: null,
          },
        ],
      });
    }
    if (request.url.endsWith("/backend-api/wham/rate-limit-reset-credits/consume")) {
      consumeAccountIds.push(accountId);
      consumeBodies.push(JSON.parse(await request.text()));
      return Response.json({ code: "reset", windows_reset: 1 });
    }
    throw new Error(`unexpected request ${request.method} ${request.url}`);
  };

  const options = (requestId: string) => ({
    clientVersion: "0.145.0",
    requestId,
    bankedReset: {
      config: persistentLiveConfig,
      kv: kv as unknown as Deno.Kv,
      now: () => fixedStartMs,
      newOwnerToken: () => `owner-${requestId}`,
    },
  });

  try {
    const armed = await fetchCodexResponses({ input: "persistent-live-all-blocked-arm" }, options("persistent-live-all-blocked-arm"));
    assert.equal(armed.status, 429);
    assert.equal((await armed.json()).error.code, "codex_quota_blocked");
    assert.deepEqual(
      [...inventoryAccountIds].sort((a, b) => a.localeCompare(b)),
      ["account-one", "account-two"]
    );
    assert.deepEqual(consumeAccountIds, []);
    assert.deepEqual(inferenceAccountIds, []);

    const consumed = await fetchCodexResponses({ input: "persistent-live-all-blocked-consume" }, options("persistent-live-all-blocked-consume"));
    assert.equal(consumed.status, 200);
    assert.ok(getCodexRoutingProbe(consumed));
    assert.equal(inventoryAccountIds.filter((accountId) => accountId === "account-one").length, 2);
    assert.equal(inventoryAccountIds.filter((accountId) => accountId === "account-two").length, 2);
    assert.deepEqual(consumeAccountIds, ["account-one"]);
    assert.deepEqual(inferenceAccountIds, ["account-one"]);
    assert.equal(consumeBodies.length, 1);
    assert.equal((consumeBodies[0] as { credit_id: unknown }).credit_id, "credit-account-one");
    assert.equal(typeof (consumeBodies[0] as { redeem_request_id: unknown }).redeem_request_id, "string");
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean; codexBaseUrl: string }).isDeploy = originalDeployFlag;
    (config as { isDeploy: boolean; codexBaseUrl: string }).codexBaseUrl = originalCodexBaseUrl;
  }
});

Deno.test("post-reset response probes retain their tombstone until an explicit completed outcome", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  resetProviderHealthThrottleForTest();
  let releaseProviderHealthCommit = () => {};
  let providerHealthCommitted: Promise<void> | null = null;

  const routingSlot = () => {
    const state = parseCodexAccountRoutingState(kv.extra.get(JSON.stringify(CODEX_ACCOUNT_ROUTING_KV_KEY))?.value);
    return state?.slots[0] ?? null;
  };
  const assertTombstone = () => {
    const slot = routingSlot();
    assert.equal(slot?.banked_reset_generation_ambiguous, true);
    assert.notEqual(slot.observed_reset_at_ms, null);
  };
  const fetchPostResetResponse = async (owner: string): Promise<Response> => {
    const reset = scriptedResetProvider();
    let inferenceCalls = 0;
    kv.auth = pool(auth("one"));
    kv.extra.clear();
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = () => {
      inferenceCalls += 1;
      return Promise.resolve(
        inferenceCalls === 1
          ? new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
              status: 429,
              headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
            })
          : new Response('data: {"type":"response.completed","response":{"output":[]}}\n\n', { status: 200, headers: { "Content-Type": "text/event-stream" } })
      );
    };
    const response = await fetchCodexResponses(
      { input: `post-reset-probe-${owner}` },
      {
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => owner,
        },
      }
    );
    assert.equal(response.status, 200);
    assert.equal(inferenceCalls, 2);
    assert.deepEqual(reset.calls, ["inventory", "redeem", "verify"]);
    assert.ok(getCodexRoutingProbe(response));
    assertTombstone();
    return response;
  };

  try {
    for (const outcome of ["failed", "incomplete", "cancelled"] as const) {
      const response = await fetchPostResetResponse(`owner-post-reset-${outcome}`);
      await releaseCodexResponseProbe(response);
      assert.equal(getCodexRoutingProbe(response), null, outcome);
      assertTombstone();
      assert.equal(routingSlot()?.probe_lease, null, outcome);
    }

    const completed = await fetchPostResetResponse("owner-post-reset-completed");
    const providerHealthCommitGate = new Promise<void>((resolve) => {
      releaseProviderHealthCommit = resolve;
    });
    let signalProviderHealthCommit = () => {};
    const providerHealthCommitEntered = new Promise<void>((resolve) => {
      signalProviderHealthCommit = resolve;
    });
    let signalProviderHealthCommitted = () => {};
    providerHealthCommitted = new Promise<void>((resolve) => {
      signalProviderHealthCommitted = resolve;
    });
    kv.providerHealthSuccessCommitGate = providerHealthCommitGate;
    kv.onProviderHealthSuccessCommit = signalProviderHealthCommit;
    kv.onProviderHealthSuccessCommitted = signalProviderHealthCommitted;
    let completionSettled = false;
    const completion = markCodexResponseCompleted(completed).then(() => {
      completionSettled = true;
    });
    assert.equal(getCodexRoutingProbe(completed), null);
    await providerHealthCommitEntered;
    await new Promise<void>((resolve) => setTimeout(resolve, 0));
    assert.equal(completionSettled, true);
    await completion;
    const slot = routingSlot();
    assert.equal(slot?.banked_reset_generation_ambiguous, false);
    assert.equal(slot.observed_reset_at_ms, null);
    assert.equal(slot.probe_lease, null);
  } finally {
    releaseProviderHealthCommit();
    if (providerHealthCommitted) await providerHealthCommitted;
    kv.providerHealthSuccessCommitGate = null;
    kv.onProviderHealthSuccessCommit = null;
    kv.onProviderHealthSuccessCommitted = null;
    resetProviderHealthThrottleForTest();
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("simultaneous gateway requests share one durable banked-reset submission", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let signalRedeemEntered!: () => void;
  let releaseRedeem!: () => void;
  const redeemEntered = new Promise<void>((resolve) => {
    signalRedeemEntered = resolve;
  });
  const redeemGate = new Promise<void>((resolve) => {
    releaseRedeem = resolve;
  });
  const reset = scriptedResetProvider({
    onRedeem: signalRedeemEntered,
    redeemGate,
  });
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = () => {
    inferenceCalls += 1;
    if (inferenceCalls === 1) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
        })
      );
    }
    return Promise.resolve(new Response(JSON.stringify({ id: "response-after-concurrent-reset" }), { status: 200 }));
  };

  const bankedReset = {
    config: liveBankedResetConfig(),
    provider: reset.provider,
    kv: kv as unknown as Deno.Kv,
    now: () => fixedStartMs,
    newOwnerToken: () => "owner-concurrent-gateway-reset",
  };
  try {
    const first = fetchCodexResponses({ input: "first-concurrent-banked-reset" }, { requestId: "first-concurrent-banked-reset", bankedReset });
    await redeemEntered;

    // The second request sees the durable `submitted` transaction while its
    // provider call is stalled. It may return the normal quota response but
    // must neither dispatch inference nor submit another reset.
    const second = await fetchCodexResponses({ input: "second-concurrent-banked-reset" }, { requestId: "second-concurrent-banked-reset", bankedReset });
    assert.equal(second.status, 429);
    assert.equal(inferenceCalls, 1);
    assert.deepEqual(reset.calls, ["inventory", "redeem"]);
    assert.equal(reset.idempotencyKeys.length, 1);

    releaseRedeem();
    const firstResponse = await first;
    assert.equal(firstResponse.status, 200);
    assert.equal(inferenceCalls, 2);
    assert.deepEqual(reset.calls, ["inventory", "redeem", "verify"]);
    assert.equal(reset.idempotencyKeys.length, 1);
    assert.equal(new Set(reset.idempotencyKeys).size, 1);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("an already-redeemed reset is independently verified before one same-account retry", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const reset = scriptedResetProvider({ redeemKind: "already_redeemed" });
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    if (accountIds.length === 1) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
        })
      );
    }
    return Promise.resolve(new Response(JSON.stringify({ id: "response-after-already-redeemed" }), { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-already-redeemed" },
      {
        requestId: "banked-reset-already-redeemed",
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-already-redeemed",
        },
      }
    );
    assert.equal(response.status, 200);
    assert.deepEqual(accountIds, ["account-one", "account-one"]);
    assert.deepEqual(reset.calls, ["inventory", "redeem", "verify"]);
    assert.equal(reset.idempotencyKeys.length, 1);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("an auth rotation after verification fences off the post-reset retry", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  const reset = scriptedResetProvider({
    onVerify: () => {
      kv.auth = pool({
        ...auth("one"),
        access_token: accessToken("one-rotated"),
        refresh_token: "refresh-one-rotated",
      });
      kv.authVersion += 1;
    },
  });
  globalThis.fetch = () => {
    inferenceCalls += 1;
    return Promise.resolve(
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
      })
    );
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-auth-rotation" },
      {
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-auth-rotation",
        },
      }
    );
    assert.equal(response.status, 429);
    assert.equal(inferenceCalls, 1, "a rotated auth-pool entry must prevent the post-reset retry");
    assert.deepEqual(reset.calls, ["inventory", "redeem", "verify"]);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("an auth rotation inside the final dispatch hook fences off a post-reset retry", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let inferenceCalls = 0;
  let beforeDispatchCalls = 0;
  const startedDispatchGenerations: number[] = [];
  const cancelledDispatchGenerations: number[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  const reset = scriptedResetProvider();
  globalThis.fetch = () => {
    inferenceCalls += 1;
    return Promise.resolve(
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
      })
    );
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-dispatch-race" },
      {
        beforeDispatch: () => {
          beforeDispatchCalls += 1;
          const dispatchGeneration = beforeDispatchCalls;
          if (beforeDispatchCalls === 2) {
            // This occurs after the verified record and routing repair, but
            // before the post-reset transport can mark itself started.
            kv.auth = pool({
              ...auth("one"),
              access_token: accessToken("one-rotated-during-dispatch"),
              refresh_token: "refresh-one-rotated-during-dispatch",
            });
            kv.authVersion += 1;
          }
          return Promise.resolve({
            markTransportStarted: () => {
              startedDispatchGenerations.push(dispatchGeneration);
            },
            cancelBeforeTransport: () => {
              cancelledDispatchGenerations.push(dispatchGeneration);
              return Promise.resolve();
            },
          });
        },
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-dispatch-race",
        },
      }
    );
    assert.equal(response.status, 429);
    assert.equal(beforeDispatchCalls, 2);
    assert.equal(inferenceCalls, 1, "the rotated second attempt must not reach upstream transport");
    assert.deepEqual(startedDispatchGenerations, [1]);
    assert.deepEqual(cancelledDispatchGenerations, [2]);
    assert.deepEqual(reset.calls, ["inventory", "redeem", "verify"]);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a global active-account transition inside the final dispatch hook fences off a post-reset retry", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let inferenceCalls = 0;
  let beforeDispatchCalls = 0;
  const startedDispatchGenerations: number[] = [];
  const cancelledDispatchGenerations: number[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  const reset = scriptedResetProvider();
  globalThis.fetch = () => {
    inferenceCalls += 1;
    return Promise.resolve(
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
      })
    );
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-active-account-fence" },
      {
        beforeDispatch: async () => {
          beforeDispatchCalls += 1;
          const dispatchGeneration = beforeDispatchCalls;
          if (beforeDispatchCalls === 2) {
            // Another request advances the global active selection while the
            // post-reset retry is paused in providerDispatch.claim().
            const active = parseCodexActiveAccountSelection(kv.extra.get(JSON.stringify(CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY))?.value);
            if (!active) throw new Error("expected a durable active selection");
            await kv.set(CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, { ...active, generation: active.generation + 1, updated_at_ms: fixedStartMs });
          }
          return Promise.resolve({
            markTransportStarted: () => {
              startedDispatchGenerations.push(dispatchGeneration);
            },
            cancelBeforeTransport: () => {
              cancelledDispatchGenerations.push(dispatchGeneration);
              return Promise.resolve();
            },
          });
        },
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-active-fence",
        },
      }
    );
    assert.equal(response.status, 429);
    assert.equal(beforeDispatchCalls, 2);
    assert.equal(inferenceCalls, 1, "the stale active selection on the second attempt must not reach upstream transport");
    assert.deepEqual(startedDispatchGenerations, [1]);
    assert.deepEqual(cancelledDispatchGenerations, [2]);
    assert.deepEqual(reset.calls, ["inventory", "redeem", "verify"]);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("an auth-pool slot reorder during a claimed reset fences submission before redemption", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const stableRetryAfter = new Date(fixedStartMs + 60_000).toUTCString();
  const upstreamAccounts: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  const reset = scriptedResetProvider({
    onInventory: () => {
      // Both accounts are quota-blocked. The second account is the reset
      // candidate, then an operator reorders the pool while it is claimed.
      kv.auth = pool(auth("two"), auth("one"));
      kv.authVersion += 1;
    },
  });
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    upstreamAccounts.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": stableRetryAfter },
      })
    );
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-slot-reorder" },
      {
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-slot-reorder",
        },
      }
    );
    assert.equal(response.status, 429);
    assert.deepEqual(upstreamAccounts, ["account-one", "account-two"]);
    // The all-blocked evaluator must inspect both account-bound inventories
    // before it can rule out a live spend after the routing reorder.
    assert.deepEqual(reset.calls, ["inventory", "inventory"]);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("the request that first discovers a healthy fallback does not spend before a fresh cohort read", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const reset = scriptedResetProvider();
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(
      accountIds.length === 1
        ? new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
            status: 429,
            headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
          })
        : new Response(JSON.stringify({ id: "fallback-success" }), { status: 200 })
    );
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-fallback" },
      {
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-fallback",
        },
      }
    );
    assert.equal(response.status, 200);
    assert.deepEqual(accountIds, ["account-one", "account-two"]);
    assert.deepEqual(reset.calls, []);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a sibling re-blocked during reset inventory cannot be dispatched from the stale snapshot", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  await seedStableBankedResetBlock();
  await seedStableBankedResetBlock("account-two");
  const reset = scriptedResetProvider({
    onInventory: () => {
      // Simulate a different isolate writing the durable record directly. The
      // local routing module's five-second cache intentionally remains stale.
      const routingKey = JSON.stringify(CODEX_ACCOUNT_ROUTING_KV_KEY);
      const entry = kv.extra.get(routingKey);
      assert.ok(entry);
      const state = structuredClone(parseCodexAccountRoutingState(entry.value));
      assert.ok(state);

      const sibling = state.slots[1];
      assert.ok(sibling);
      const retryAtMs = Date.parse(stableBankedResetRetryAfter);
      const slots = [...state.slots];
      slots[1] = {
        ...sibling,
        quota_blocked_until_ms: retryAtMs,
        quota_block_source: "header_retry_after",
        observed_reset_at_ms: retryAtMs,
        observed_reset_at_is_stable: true,
        banked_reset_generation_ambiguous: false,
        generation: sibling.generation + 1,
        probe_lease: null,
      };
      kv.extra.set(routingKey, {
        value: { ...state, updated_at_ms: fixedStartMs, slots },
        version: entry.version + 1,
      });
    },
  });
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(new Response(JSON.stringify({ id: "stale-sibling-fallback" }), { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "partial-preflight-sibling-blocked" },
      {
        requestId: "partial-preflight-sibling-blocked",
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-partial-preflight-sibling-blocked",
        },
      }
    );
    assert.equal(response.status, 429);
    assert.equal((await response.json()).error.code, "codex_quota_blocked");
    assert.deepEqual(
      [...reset.inventoryAccountIds].sort((left, right) => left.localeCompare(right)),
      ["account-one", "account-two"]
    );
    assert.deepEqual(reset.redeemAccountIds, []);
    assert.equal(reset.calls.filter((call) => call === "inventory").length, 2);
    assert.equal(reset.calls.includes("redeem"), false);
    assert.deepEqual(accountIds, []);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a legacy timeout mutation during reset inventory cannot open a stale fallback", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  await seedStableBankedResetBlock();
  await seedStableBankedResetBlock("account-two");
  const reset = scriptedResetProvider({
    onInventory: () => {
      const routingKey = JSON.stringify(CODEX_ACCOUNT_ROUTING_KV_KEY);
      const entry = kv.extra.get(routingKey);
      assert.ok(entry);
      const state = structuredClone(parseCodexAccountRoutingState(entry.value));
      assert.ok(state);

      const sibling = state.slots[1];
      assert.ok(sibling);
      const slots = [...state.slots];
      slots[1] = {
        ...sibling,
        upstream_timeout_blocked_until_ms: fixedStartMs + CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS,
        generation: sibling.generation + 1,
        probe_lease: null,
      };
      kv.extra.set(routingKey, {
        value: { ...state, updated_at_ms: fixedStartMs, slots },
        version: entry.version + 1,
      });
    },
  });
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(new Response(JSON.stringify({ id: "stale-timeout-fallback" }), { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "partial-preflight-sibling-timeout" },
      {
        requestId: "partial-preflight-sibling-timeout",
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-partial-preflight-sibling-timeout",
        },
      }
    );
    assert.equal(response.status, 429);
    assert.equal((await response.json()).error.code, "codex_quota_blocked");
    assert.deepEqual(reset.redeemAccountIds, []);
    assert.equal(reset.calls.includes("redeem"), false);
    assert.deepEqual(accountIds, []);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a routing KV outage during reset inventory never dispatches a stale fallback", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  await seedStableBankedResetBlock();
  await seedStableBankedResetBlock("account-two");
  const reset = scriptedResetProvider({
    onInventory: () => {
      kv.onRoutingRead = () => {
        throw new Error("routing KV unavailable");
      };
    },
  });
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(new Response(JSON.stringify({ id: "cached-sibling-fallback" }), { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "reset-inventory-routing-unavailable" },
      {
        requestId: "reset-inventory-routing-unavailable",
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-reset-inventory-routing-unavailable",
        },
      }
    );
    assert.equal(response.status, 429);
    assert.equal((await response.json()).error.code, "codex_capacity_unavailable");
    assert.deepEqual(reset.redeemAccountIds, []);
    assert.equal(reset.calls.includes("redeem"), false);
    assert.deepEqual(accountIds, []);
  } finally {
    kv.onRoutingRead = null;
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a sibling credential rotation during reset inventory fences the consume and serves on the next admission", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const authorizations: string[] = [];
  const rotated = {
    ...auth("two"),
    access_token: accessToken("two-rotated"),
    refresh_token: "refresh-two-rotated",
  };
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  await seedStableBankedResetBlock();
  await seedStableBankedResetBlock("account-two");
  const reset = scriptedResetProvider({
    onInventory: async () => {
      await kv.set(AUTH_KEY, pool(auth("one"), rotated));
    },
  });
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    authorizations.push(request.headers.get("authorization") ?? "");
    return Promise.resolve(new Response(JSON.stringify({ id: "rotated-sibling" }), { status: 200 }));
  };

  try {
    const fenced = await fetchCodexResponses(
      { input: "reset-inventory-sibling-rotated" },
      {
        requestId: "reset-inventory-sibling-rotated",
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-reset-inventory-sibling-rotated",
        },
      }
    );
    assert.equal(fenced.status, 429);
    assert.equal((await fenced.json()).error.code, "codex_capacity_unavailable");
    assert.deepEqual(reset.redeemAccountIds, []);
    assert.deepEqual(accountIds, []);

    // The rotated credential is a newly eligible ordinary account, so the next
    // admission serves it without inheriting the failed reset state.
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    const served = await fetchCodexResponses({ input: "rotated-sibling-after-fence" });
    assert.equal(served.status, 200);
    assert.deepEqual(accountIds, ["account-two"]);
    assert.deepEqual(authorizations, [`Bearer ${rotated.access_token}`]);
    await served.arrayBuffer();
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a positive capacity observation during reset inventory fences the consume", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  // Account one's quota signal predates the fresh positive capacity sample
  // taken during inventory, so that sample proves the account recovered.
  await seedStableBankedResetBlock("account-one", fixedStartMs - 60_000);
  await seedStableBankedResetBlock("account-two");
  const reset = scriptedResetProvider({
    onInventory: async () => {
      // A fresh positive capacity sample arrives after preflight. It clears the
      // account's class circuit, so the stored capacity snapshot and routing
      // fence the consume was bound to are both stale.
      await recordCodexCapacityRoutingObservations(
        [
          {
            slot: 0,
            account_id: "account-one",
            state: "available",
            source_observed_at_ms: fixedStartMs,
            snapshot_at_ms: fixedStartMs,
            windows: {
              primary: { limit_window_seconds: 10_800, used_percent: 10, reset_at_ms: fixedStartMs + 10_800_000 },
              secondary: null,
            },
            additional_rate_limits: [],
          },
        ],
        fixedStartMs
      );
    },
  });
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(new Response(JSON.stringify({ id: "capacity-recovered" }), { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "reset-inventory-positive-capacity" },
      {
        requestId: "reset-inventory-positive-capacity",
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-reset-inventory-positive-capacity",
        },
      }
    );
    assert.equal(response.status, 429);
    assert.equal((await response.json()).error.code, "codex_capacity_unavailable");
    assert.deepEqual(reset.redeemAccountIds, []);
    assert.equal(reset.calls.includes("redeem"), false);
    assert.deepEqual(accountIds, [], "the stale reset must not dispatch a recovery inference");

    // The newly eligible account is served by a fresh admission instead.
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    const served = await fetchCodexResponses({ input: "capacity-recovered-next-admission" });
    assert.equal(served.status, 200);
    const lastServedAccountId = accountIds[accountIds.length - 1];
    assert.equal(lastServedAccountId, "account-one");
    await served.arrayBuffer();
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a whole-pool reorder during reset inventory fences the consume", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  await seedStableBankedResetBlock();
  await seedStableBankedResetBlock("account-two");
  const reset = scriptedResetProvider({
    onInventory: async () => {
      await kv.set(AUTH_KEY, pool(auth("two"), auth("one")));
    },
  });
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.resolve(new Response(JSON.stringify({ id: "reordered-sibling-fallback" }), { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "reset-inventory-pool-reordered" },
      {
        requestId: "reset-inventory-pool-reordered",
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-reset-inventory-pool-reordered",
        },
      }
    );
    assert.equal(response.status, 429);
    assert.equal((await response.json()).error.code, "codex_quota_blocked");
    assert.deepEqual(reset.redeemAccountIds, []);
    assert.equal(reset.calls.includes("redeem"), false);
    assert.deepEqual(accountIds, []);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("definitive post-reset probe failures are returned directly without sibling replay", async (t) => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;

  try {
    for (const status of [401, 403, 429]) {
      await t.step(String(status), async () => {
        const accountIds: string[] = [];
        const reset = scriptedResetProvider();
        kv.auth = pool(auth("one"), auth("two"));
        kv.extra.clear();
        resetCodexAuthCacheForTest();
        resetCodexAccountRoutingForTest();
        await seedStableBankedResetBlock();
        await seedStableBankedResetBlock("account-two");
        globalThis.fetch = (input, init) => {
          const request = new Request(input, init);
          const accountId = request.headers.get("chatgpt-account-id") ?? "";
          accountIds.push(accountId);
          const headers = new Headers({ "Content-Type": "application/json" });
          if (status === 429) headers.set("Retry-After", stableBankedResetRetryAfter);
          return Promise.resolve(
            new Response(
              JSON.stringify({
                error: {
                  type: probeErrorType(status),
                },
              }),
              { status, headers }
            )
          );
        };

        const response = await fetchCodexResponses(
          { input: `complete-probe-${status}` },
          {
            requestId: `complete-probe-${status}`,
            bankedReset: {
              config: liveBankedResetConfig(),
              provider: reset.provider,
              kv: kv as unknown as Deno.Kv,
              now: () => fixedStartMs,
              newOwnerToken: () => `owner-complete-probe-${status}`,
            },
          }
        );

        // Every account is exhausted, so the definitive probe answer is the
        // final response: exactly one recovery inference, no sibling replay.
        assert.equal(response.status, status);
        assert.equal(accountIds.length, 1);
        assert.deepEqual(reset.redeemAccountIds, accountIds);
        assert.equal(reset.calls.filter((call) => call === "redeem").length, 1);
        await response.arrayBuffer();
      });
    }
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("an ambiguous post-reset transport outcome never replays on a sibling", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const reset = scriptedResetProvider();
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  await seedStableBankedResetBlock();
  await seedStableBankedResetBlock("account-two");
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    return Promise.reject(new DOMException("post-reset deadline elapsed", "TimeoutError"));
  };

  try {
    await assert.rejects(() =>
      fetchCodexResponses(
        { input: "complete-probe-transport-ambiguous" },
        {
          requestId: "complete-probe-transport-ambiguous",
          bankedReset: {
            config: liveBankedResetConfig(),
            provider: reset.provider,
            kv: kv as unknown as Deno.Kv,
            now: () => fixedStartMs,
            newOwnerToken: () => "owner-complete-probe-transport-ambiguous",
          },
        }
      )
    );
    assert.equal(accountIds.length, 1);
    assert.deepEqual(reset.redeemAccountIds, accountIds);
    assert.equal(reset.calls.filter((call) => call === "redeem").length, 1);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("simultaneous all-exhausted requests share one consume and never replay it", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  let signalRedeemEntered!: () => void;
  let releaseRedeem!: () => void;
  const redeemEntered = new Promise<void>((resolve) => {
    signalRedeemEntered = resolve;
  });
  const redeemGate = new Promise<void>((resolve) => {
    releaseRedeem = resolve;
  });
  const reset = scriptedResetProvider({ onRedeem: signalRedeemEntered, redeemGate });
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  await seedStableBankedResetBlock();
  await seedStableBankedResetBlock("account-two");
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    const accountId = request.headers.get("chatgpt-account-id") ?? "";
    accountIds.push(accountId);
    return Promise.resolve(new Response(JSON.stringify({ id: `response-${accountId}` }), { status: 200 }));
  };
  const bankedReset = {
    config: liveBankedResetConfig(),
    provider: reset.provider,
    kv: kv as unknown as Deno.Kv,
    now: () => fixedStartMs,
    newOwnerToken: () => "owner-all-exhausted-concurrent",
  };

  try {
    const first = fetchCodexResponses({ input: "all-exhausted-concurrent-first" }, { requestId: "all-exhausted-concurrent-first", bankedReset });
    await redeemEntered;

    // The contender cannot start a second consume while the first submission
    // owns the durable record; it reports the exhausted cohort instead.
    const second = await fetchCodexResponses({ input: "all-exhausted-concurrent-second" }, { requestId: "all-exhausted-concurrent-second", bankedReset });
    assert.equal(second.status, 429);
    assert.equal(reset.idempotencyKeys.length, 1);
    assert.deepEqual(accountIds, []);

    releaseRedeem();
    const firstResponse = await first;
    assert.equal(firstResponse.status, 200);
    assert.equal(accountIds.length, 1);
    assert.equal(reset.idempotencyKeys.length, 1);
    assert.deepEqual(reset.redeemAccountIds, accountIds);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a skipped half-open probe prevents a sibling banked-reset redemption", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const reset = scriptedResetProvider();
  let now = fixedStartMs;
  Date.now = () => now;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  kv.onRoutingRead = null;
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();

  try {
    const initial = await selectCodexRoutingAccounts(kv.auth, kv.auth.accounts, now);
    assert.equal(initial.kind, "eligible");

    const second = initial.accounts.find((account) => account.auth.account_id === "account-two");
    assert.ok(second);

    const expiredAtMs = now + 1_000;
    await markCodexQuotaBlocked(
      second,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(expiredAtMs).toUTCString() },
      }),
      now
    );
    now = expiredAtMs + 1;

    resetCodexAccountRoutingForTest();
    const halfOpen = await selectCodexRoutingAccounts(kv.auth, kv.auth.accounts, now);
    assert.equal(halfOpen.kind, "eligible");

    const foreignProbeCandidate = halfOpen.accounts.find((account) => account.auth.account_id === "account-two");
    assert.ok(foreignProbeCandidate?.probeRequired);

    // Another isolate owns the half-open lease, so this request must not reset
    // or dispatch it. Block the first account too so the pool has no ordinary
    // capacity while the foreign lease remains held.
    assert.ok(await claimCodexRoutingProbe(kv.auth, foreignProbeCandidate, now));
    const first = initial.accounts.find((account) => account.auth.account_id === "account-one");
    assert.ok(first);
    await markCodexQuotaBlocked(
      first,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
      }),
      now
    );
    resetCodexAccountRoutingForTest();
    globalThis.fetch = (input, init) => {
      const request = new Request(input, init);
      accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
      return Promise.resolve(new Response(JSON.stringify({ id: "unexpected-dispatch" }), { status: 200 }));
    };

    const response = await fetchCodexResponses(
      { input: "banked-reset-probe-unavailable" },
      {
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => now,
          newOwnerToken: () => "owner-probe-unavailable",
        },
      }
    );

    // A held recovery lease is retryable, never quota proof for redemption.
    assert.equal(response.status, 429);
    assert.deepEqual(reset.calls, []);
    assert.deepEqual(accountIds, []);
    await response.arrayBuffer();
  } finally {
    kv.onRoutingRead = null;
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a 403 sibling blocks a full-pool banked reset", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const reset = scriptedResetProvider();
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    const accountId = request.headers.get("chatgpt-account-id") ?? "";
    accountIds.push(accountId);
    if (accountIds.length === 1) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
        })
      );
    }
    if (accountIds.length === 2) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "forbidden" } }), {
          status: 403,
          headers: { "Content-Type": "application/json" },
        })
      );
    }
    return Promise.resolve(new Response(JSON.stringify({ id: "response-after-reset" }), { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-after-403" },
      {
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-after-403",
        },
      }
    );

    assert.equal(response.status, 403);
    assert.deepEqual(accountIds, ["account-one", "account-two"]);
    assert.deepEqual(reset.calls, []);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("an earlier allowlisted exhausted account is redeemed after a later sibling also exhausts quota", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const reset = scriptedResetProvider();
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    accountIds.push(request.headers.get("chatgpt-account-id") ?? "");
    if (accountIds.length <= 2) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
        })
      );
    }
    return Promise.resolve(new Response(JSON.stringify({ id: "response-after-earlier-reset" }), { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-earlier-allowlisted" },
      {
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-earlier-allowlisted",
        },
      }
    );

    assert.equal(response.status, 200);
    assert.deepEqual(accountIds, ["account-one", "account-two", "account-one"]);
    assert.deepEqual(reset.redeemAccountIds, ["account-one"]);
    assert.deepEqual(reset.calls, ["inventory", "inventory", "redeem", "verify"]);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a non-expired raw 403 during the bounded retry never quarantines credentials", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const accountIds: string[] = [];
  const reset = scriptedResetProvider();
  const shortStableRetryAfter = new Date(fixedStartMs + 2_000).toUTCString();
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    const accountId = request.headers.get("chatgpt-account-id") ?? "";
    accountIds.push(accountId);
    if (accountIds.length <= 2) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": shortStableRetryAfter },
        })
      );
    }
    if (accountIds.length === 3) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "forbidden" } }), {
          status: 403,
          headers: { "Content-Type": "application/json" },
        })
      );
    }
    throw new Error("a raw 403 is terminal and must not dispatch a fourth attempt");
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-bounded-retry-403" },
      {
        retrySleep: () => Promise.resolve(),
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-bounded-retry-403",
        },
      }
    );

    // The first 429 moves the active account to the sibling; the second 429
    // captures the one bounded retry, which lands back on the active account and
    // returns a raw 403. A raw 403 with a valid bearer does not quarantine
    // credentials, and it removes the complete-cohort quota proof, so it can
    // neither authorize the banked reset nor a speculative sibling fallback.
    assert.equal(response.status, 403);
    assert.deepEqual(accountIds, ["account-one", "account-two", "account-two"]);
    assert.deepEqual(reset.redeemAccountIds, []);
    assert.equal(reset.calls.includes("redeem"), false);
    const state = parseCodexAccountRoutingState(kv.extra.get(JSON.stringify(CODEX_ACCOUNT_ROUTING_KV_KEY))?.value);
    assert.equal(state?.slots[0]?.invalid_credential_version, null);
    assert.equal(state.slots[1]?.invalid_credential_version, null);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a successful ordinary bounded retry never spends a banked reset", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const reset = scriptedResetProvider();
  const shortStableRetryAfter = new Date(fixedStartMs + 2_000).toUTCString();
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = () => {
    inferenceCalls += 1;
    if (inferenceCalls === 1) {
      return Promise.resolve(
        new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
          status: 429,
          headers: { "Content-Type": "application/json", "Retry-After": shortStableRetryAfter },
        })
      );
    }
    return Promise.resolve(new Response(JSON.stringify({ id: "ordinary-retry-success" }), { status: 200 }));
  };

  try {
    const response = await fetchCodexResponses(
      { input: "ordinary-retry-success" },
      {
        retrySleep: () => Promise.resolve(),
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-ordinary-retry-success",
        },
      }
    );

    assert.equal(response.status, 200);
    assert.equal(inferenceCalls, 2, "the ordinary retry is the only successful inference request");
    assert.deepEqual(reset.calls, [], "a served request must not read inventory or submit a reset");
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("only a complete stable usage-limit response can reach the banked-reset provider", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const stableRetryAfter = new Date(fixedStartMs + 60_000).toUTCString();
  const cases: Readonly<{
    name: string;
    status: number;
    body: string;
    retryAfter?: string;
    expectedStatus: number;
  }>[] = [
    {
      name: "generic rate limit",
      status: 429,
      body: JSON.stringify({ error: { type: "rate_limit_error" } }),
      retryAfter: stableRetryAfter,
      expectedStatus: 429,
    },
    {
      name: "burst throttling",
      status: 429,
      body: JSON.stringify({ error: { type: "requests_per_minute" } }),
      retryAfter: stableRetryAfter,
      expectedStatus: 429,
    },
    {
      name: "overload",
      status: 429,
      body: JSON.stringify({ error: { type: "server_error" } }),
      retryAfter: stableRetryAfter,
      expectedStatus: 429,
    },
    {
      name: "policy error",
      status: 429,
      body: JSON.stringify({ error: { type: "policy_error" } }),
      retryAfter: stableRetryAfter,
      expectedStatus: 429,
    },
    {
      name: "invalid request",
      status: 429,
      body: JSON.stringify({ error: { type: "invalid_request_error" } }),
      retryAfter: stableRetryAfter,
      expectedStatus: 429,
    },
    {
      name: "unknown future error type",
      status: 429,
      body: JSON.stringify({ error: { type: "future_quota_signal" } }),
      retryAfter: stableRetryAfter,
      expectedStatus: 429,
    },
    {
      name: "malformed body",
      status: 429,
      body: "{not JSON",
      retryAfter: stableRetryAfter,
      expectedStatus: 429,
    },
    {
      name: "relative retry-after cannot name a reset window",
      status: 429,
      body: JSON.stringify({ error: { type: "usage_limit_reached" } }),
      retryAfter: "60",
      expectedStatus: 429,
    },
    {
      name: "invalid decimal retry-after",
      status: 429,
      body: JSON.stringify({ error: { type: "usage_limit_reached" } }),
      retryAfter: "0.5",
      expectedStatus: 429,
    },
    {
      name: "expired retry-after",
      status: 429,
      body: JSON.stringify({ error: { type: "usage_limit_reached" } }),
      retryAfter: new Date(fixedStartMs - 1_000).toUTCString(),
      expectedStatus: 429,
    },
    {
      name: "overflowing retry-after",
      status: 429,
      body: JSON.stringify({ error: { type: "usage_limit_reached" } }),
      retryAfter: "999999999999999999999999999999999999",
      expectedStatus: 429,
    },
    {
      name: "401",
      status: 401,
      body: JSON.stringify({ error: { type: "invalid_auth" } }),
      expectedStatus: 401,
    },
    {
      name: "403",
      status: 403,
      body: JSON.stringify({ error: { type: "forbidden" } }),
      expectedStatus: 403,
    },
  ];

  try {
    Date.now = () => fixedStartMs;
    (config as { isDeploy: boolean }).isDeploy = true;
    for (const testCase of cases) {
      kv.auth = pool(auth("one"));
      kv.extra.clear();
      resetCodexAuthCacheForTest();
      resetCodexAccountRoutingForTest();
      const reset = scriptedResetProvider();
      globalThis.fetch = (input) => {
        const url = requestUrl(input);
        if (url.includes("oauth/token")) {
          return Promise.resolve(new Response(JSON.stringify({ error: "invalid_grant" }), { status: 401 }));
        }
        const headers = new Headers({ "Content-Type": "application/json" });
        if (testCase.retryAfter) headers.set("Retry-After", testCase.retryAfter);
        return Promise.resolve(new Response(testCase.body, { status: testCase.status, headers }));
      };

      const response = await fetchCodexResponses(
        { input: `nonqualifying-${testCase.name}` },
        {
          retrySleep: async () => {},
          bankedReset: {
            config: liveBankedResetConfig(),
            provider: reset.provider,
            kv: kv as unknown as Deno.Kv,
            now: () => fixedStartMs,
            newOwnerToken: () => `owner-nonqualifying-${testCase.name}`,
          },
        }
      );
      assert.equal(response.status, testCase.expectedStatus, testCase.name);
      assert.deepEqual(reset.calls, [], testCase.name);
    }
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a later non-qualifying 429 clears an earlier banked-reset candidate", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const reset = scriptedResetProvider();
  const stableRetryAfter = new Date(fixedStartMs + 60_000).toUTCString();
  const accountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    const accountId = request.headers.get("chatgpt-account-id") ?? "";
    accountIds.push(accountId);
    const errorType = accountIds.length === 1 ? "usage_limit_reached" : "rate_limit_error";
    return Promise.resolve(
      new Response(JSON.stringify({ error: { type: errorType } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": stableRetryAfter },
      })
    );
  };

  try {
    const response = await fetchCodexResponses(
      { input: "later-nonqualifying-429" },
      {
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-later-nonqualifying-429",
        },
      }
    );
    assert.equal(response.status, 429);
    assert.deepEqual(accountIds, ["account-one", "account-two"]);
    assert.deepEqual(reset.calls, []);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("post-reset inference may return one normal 429 but never triggers a second redemption", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const reset = scriptedResetProvider();
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = () => {
    inferenceCalls += 1;
    return Promise.resolve(
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
      })
    );
  };

  try {
    const response = await fetchCodexResponses(
      { input: "banked-reset-second-429" },
      {
        bankedReset: {
          config: liveBankedResetConfig(),
          provider: reset.provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-second-429",
        },
      }
    );
    assert.equal(response.status, 429);
    assert.equal(inferenceCalls, 2);
    assert.deepEqual(reset.calls, ["inventory", "redeem", "verify"]);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a failed banked-reset probe gets a bounded retry and eventually reopens the account", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const reset = scriptedResetProvider();
  const inferenceAccounts: string[] = [];
  const delayedPropagationRetryAfter = new Date(fixedStartMs + 7 * 24 * 60 * 60_000).toUTCString();
  const delayedPropagationResetAtMs = fixedStartMs + 7 * 24 * 60 * 60_000;
  let now = fixedStartMs;
  Date.now = () => now;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = (input, init) => {
    const request = new Request(input, init);
    const accountId = request.headers.get("chatgpt-account-id") ?? "";
    inferenceAccounts.push(accountId);
    if (inferenceAccounts.length > 5) {
      return Promise.resolve(new Response(JSON.stringify({ id: "recovered-after-propagation" }), { status: 200 }));
    }
    return Promise.resolve(
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": delayedPropagationRetryAfter },
      })
    );
  };

  const bankedReset = {
    config: liveBankedResetConfig(),
    provider: reset.provider,
    kv: kv as unknown as Deno.Kv,
    now: () => now,
    newOwnerToken: () => "owner-failed-probe",
  };
  try {
    const first = await fetchCodexResponses({ input: "banked-reset-probe-fails" }, { bankedReset });
    assert.equal(first.status, 429);
    assert.deepEqual(inferenceAccounts, ["account-one", "account-two", "account-one"]);
    assert.equal(reset.calls.filter((call) => call === "redeem").length, 1);
    assert.equal(reset.calls.filter((call) => call === "verify").length, 1);

    // While the recovery lease is held, the request reports the retryable quota
    // classification and neither dispatches nor spends another reset.
    const inferenceCountAfterFirst = inferenceAccounts.length;
    const resetCallsAfterFirst = [...reset.calls];
    const held = await fetchCodexResponses({ input: "after-failed-bank-reset-probe" }, { bankedReset });
    assert.equal(held.status, 429);
    assert.equal(inferenceAccounts.length, inferenceCountAfterFirst);
    assert.deepEqual(reset.calls, resetCallsAfterFirst);
    await held.arrayBuffer();

    // Each expired lease grants one bounded probe; the ledger is never spent
    // again, and the account eventually reopens on a successful probe.
    let recovered: Response | null = null;
    for (let attempt = 0; attempt < 4; attempt += 1) {
      now += CODEX_HALF_OPEN_LEASE_MS + 1;
      const response = await fetchCodexResponses({ input: `during-reset-propagation-${attempt}` }, { bankedReset });
      if (response.status === 200) {
        recovered = response;
        break;
      }
      assert.equal(response.status, 429);
      assert.equal(inferenceAccounts.at(-1), "account-one");
      assert.deepEqual(reset.calls, resetCallsAfterFirst);
      const routingAfterProbe = parseCodexAccountRoutingState((await kv.get(CODEX_ACCOUNT_ROUTING_KV_KEY, { consistency: "strong" })).value);
      assert.equal(routingAfterProbe?.slots[0]?.quota_blocked_until_ms, now + CODEX_HALF_OPEN_LEASE_MS);
      assert.equal(routingAfterProbe.slots[0]?.banked_reset_generation_ambiguous, true);
      await response.arrayBuffer();
    }
    assert.ok(recovered, "the bounded probe retry eventually reopens the account");
    assert.deepEqual(reset.calls, resetCallsAfterFirst);
    await markCodexResponseCompleted(recovered);
    const routing = parseCodexAccountRoutingState((await kv.get(CODEX_ACCOUNT_ROUTING_KV_KEY, { consistency: "strong" })).value);
    assert.equal(routing?.slots[0]?.quota_blocked_until_ms, null);
    assert.equal(routing.slots[0]?.banked_reset_generation_ambiguous, false);
    assert.equal(routing.slots[1]?.quota_blocked_until_ms, delayedPropagationResetAtMs);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("a stale verified reset recovers the existing account without another reset", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const reset = scriptedResetProvider();
  const delayedResetAtMs = fixedStartMs + 7 * 24 * 60 * 60_000;
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = () => {
    inferenceCalls += 1;
    return Promise.resolve(
      inferenceCalls <= 2
        ? new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
            status: 429,
            headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
          })
        : new Response(JSON.stringify({ id: "recovered-stale-reset" }), { status: 200 })
    );
  };

  const bankedReset = {
    config: liveBankedResetConfig(),
    provider: reset.provider,
    kv: kv as unknown as Deno.Kv,
    now: () => fixedStartMs,
    newOwnerToken: () => "owner-stale-verified-reset",
  };
  try {
    const first = await fetchCodexResponses({ input: "seed-stale-verified-reset" }, { bankedReset });
    assert.equal(first.status, 429);
    assert.equal(inferenceCalls, 2);
    assert.deepEqual(reset.calls, ["inventory", "redeem", "verify"]);

    const routingKey = JSON.stringify(CODEX_ACCOUNT_ROUTING_KV_KEY);
    const current = parseCodexAccountRoutingState(kv.extra.get(routingKey)?.value);
    if (current === null) throw new Error("expected durable routing state");
    await kv.set(CODEX_ACCOUNT_ROUTING_KV_KEY, {
      ...current,
      slots: [
        {
          ...current.slots[0],
          quota_blocked_until_ms: delayedResetAtMs,
          generation: current.slots[0].generation + 1,
          probe_lease: null,
          banked_reset_generation_ambiguous: true,
        },
      ],
    });
    resetCodexAccountRoutingForTest();

    const recovered = await fetchCodexResponses({ input: "recover-stale-verified-reset" }, { bankedReset });
    assert.equal(recovered.status, 200);
    assert.equal(inferenceCalls, 3);
    assert.deepEqual(reset.calls, ["inventory", "redeem", "verify"]);
    await markCodexResponseCompleted(recovered);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("all-blocked routing recovers an unknown reset while new submissions are disabled", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const reset = scriptedResetProvider({ redeemKind: "unknown" });
  let now = fixedStartMs;
  let live = true;
  let inferenceCalls = 0;
  Date.now = () => now;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = () => {
    inferenceCalls += 1;
    return Promise.resolve(
      inferenceCalls === 1
        ? new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
            status: 429,
            headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
          })
        : new Response(JSON.stringify({ id: "recovered-after-unknown" }), { status: 200 })
    );
  };

  const liveConfig = liveBankedResetConfig();
  const disabledConfig: CodexBankedResetConfig = { ...liveConfig, enabled: false, mode: "disabled" };
  const bankedReset = {
    config: liveConfig,
    reloadConfig: () => (live ? liveConfig : disabledConfig),
    provider: reset.provider,
    kv: kv as unknown as Deno.Kv,
    now: () => now,
    newOwnerToken: () => "owner-recovery",
  };
  try {
    const first = await fetchCodexResponses({ input: "unknown-reset" }, { bankedReset });
    assert.equal(first.status, 429);
    assert.deepEqual(reset.calls, ["inventory", "redeem"]);

    // Let the durable unknown record's lease expire while retaining its
    // original 60-second routing fence, then simulate an operator rollback.
    now += CODEX_BANKED_RESET_LEASE_MS + 1;
    live = false;
    const recovered = await fetchCodexResponses({ input: "recover-reset" }, { bankedReset });
    assert.equal(recovered.status, 200);
    assert.equal(inferenceCalls, 2);
    assert.deepEqual(reset.calls, ["inventory", "redeem", "lookup", "verify"]);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});

Deno.test("an all-blocked cohort with credit only on the inactive sibling elects it after the reset", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const originalCodexBaseUrl = config.codexBaseUrl;
  const consumeAccountIds: string[] = [];
  const inferenceAccountIds: string[] = [];
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean; codexBaseUrl: string }).isDeploy = true;
  (config as { isDeploy: boolean; codexBaseUrl: string }).codexBaseUrl = "https://upstream-reset.test/backend-api/codex";
  kv.auth = pool(auth("one"), auth("two"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();

  try {
    // Block the first configured account, then let the strong selector admit the
    // second as the durable active account before it is exhausted too.
    await seedStableBankedResetBlock("account-one");
    const bootstrapped = await selectCodexRoutingAccountsStrong(kv.auth, kv.auth.accounts, fixedStartMs);
    assert.equal(bootstrapped.kind, "eligible");
    assert.equal(bootstrapped.accounts[0]?.auth.account_id, "account-two");
    await seedStableBankedResetBlock("account-two");

    globalThis.fetch = (input, init) => {
      const request = new Request(input, init);
      const accountId = request.headers.get("chatgpt-account-id") ?? "";
      if (request.url.endsWith("/backend-api/codex/responses")) {
        inferenceAccountIds.push(accountId);
        return Promise.resolve(Response.json({ id: `response-${accountId}` }));
      }
      if (request.url.endsWith("/backend-api/wham/rate-limit-reset-credits")) {
        return Promise.resolve(
          Response.json({
            available_count: accountId === "account-one" ? 1 : 0,
            credits: accountId === "account-one" ? [{ id: "credit-account-one", status: "available", reset_type: "codex_rate_limits", expires_at: null }] : [],
          })
        );
      }
      if (request.url.endsWith("/backend-api/wham/rate-limit-reset-credits/consume")) {
        consumeAccountIds.push(accountId);
        return Promise.resolve(Response.json({ code: "reset", windows_reset: 1 }));
      }
      throw new Error(`unexpected request ${request.method} ${request.url}`);
    };

    const armed = await fetchCodexResponses({ input: "inactive-sibling-credit-arm" }, bankedResetRequestOptions("inactive-sibling-credit-arm"));
    assert.equal(armed.status, 429);
    assert.equal((await armed.json()).error.code, "codex_quota_blocked");

    const consumed = await fetchCodexResponses({ input: "inactive-sibling-credit-consume" }, bankedResetRequestOptions("inactive-sibling-credit-consume"));
    assert.equal(consumed.status, 200);
    assert.ok(getCodexRoutingProbe(consumed));
    assert.deepEqual(consumeAccountIds, ["account-one"]);
    assert.deepEqual(inferenceAccountIds, ["account-one"]);

    const active = parseCodexActiveAccountSelection(kv.extra.get(JSON.stringify(CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY))?.value);
    assert.equal(active?.slot, 0);
    assert.equal(active.generation, 2);
    await markCodexResponseCompleted(consumed);
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean; codexBaseUrl: string }).isDeploy = originalDeployFlag;
    (config as { isDeploy: boolean; codexBaseUrl: string }).codexBaseUrl = originalCodexBaseUrl;
  }
});

Deno.test("a concurrent active switch during a verified reset prevents stale recovery inference", async () => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  const originalDeployFlag = config.isDeploy;
  const baseReset = scriptedResetProvider();
  let inferenceCalls = 0;
  Date.now = () => fixedStartMs;
  (config as { isDeploy: boolean }).isDeploy = true;
  kv.auth = pool(auth("one"));
  kv.extra.clear();
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  globalThis.fetch = () => {
    inferenceCalls += 1;
    return Promise.resolve(
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": stableBankedResetRetryAfter },
      })
    );
  };
  const provider: CodexUsageResetProvider = {
    ...baseReset.provider,
    verifyApplied: async () => {
      baseReset.calls.push("verify");
      const active = parseCodexActiveAccountSelection(kv.extra.get(JSON.stringify(CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY))?.value);
      if (!active) throw new Error("expected a durable active selection");
      await kv.set(CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, { ...active, generation: active.generation + 1, updated_at_ms: fixedStartMs });
      return true;
    },
  };

  try {
    const response = await fetchCodexResponses(
      { input: "concurrent-active-switch" },
      {
        requestId: "concurrent-active-switch",
        bankedReset: {
          config: liveBankedResetConfig(),
          provider,
          kv: kv as unknown as Deno.Kv,
          now: () => fixedStartMs,
          newOwnerToken: () => "owner-concurrent-active-switch",
        },
      }
    );
    assert.equal(response.status, 429);
    assert.equal(getCodexRoutingError(response), CODEX_QUOTA_BLOCKED_ERROR_CODE);
    assert.equal(inferenceCalls, 1, "a superseded reset must not start a recovery inference");
    assert.equal(getCodexRoutingProbe(response), null);
    assert.deepEqual(baseReset.calls, ["inventory", "redeem", "verify"]);
    await response.arrayBuffer();
  } finally {
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
  }
});
