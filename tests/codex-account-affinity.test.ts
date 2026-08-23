import assert from "node:assert/strict";
import {
  cacheCodexAuthPool,
  CODEX_AUTH_POOL_KV_KEY,
  CodexError,
  fetchCodexResponses,
  getCodexResponseAffinityOutcome,
  getCodexRoutingProbe,
  markCodexResponseCompleted,
  markCodexResponseUpstreamError,
  releaseCodexResponseProbe,
  resetCodexAuthCacheForTest,
} from "../src/codex.ts";
import {
  CODEX_ACCOUNT_AFFINITY_KV_PREFIX,
  CODEX_ACCOUNT_AFFINITY_TTL_MS,
  deriveCodexAccountAffinityIdentity,
  readCodexAccountAffinity,
  recordCodexAccountAffinity,
} from "../src/codex_account_affinity.ts";
import {
  markCodexQuotaBlocked,
  resetCodexAccountRoutingForTest,
  selectCodexRoutingAccounts,
} from "../src/codex_account_routing.ts";
import { setKvForTest } from "../src/kv.ts";
import type { CodexAuthPoolState, CodexAuthState } from "../src/types.ts";

type StoredValue = Readonly<{
  value: unknown;
  version: number;
  expiresAtMs: number | null;
}>;

const keyOf = (key: Deno.KvKey): string => JSON.stringify(key);
const versionstamp = (version: number): string => String(version).padStart(20, "0");

class AffinityKv {
  readonly values = new Map<string, StoredValue>();
  expireIns: number[] = [];
  affinitySetGate: Promise<void> | null = null;
  onAffinitySet: (() => void) | null = null;

  put(key: Deno.KvKey, value: unknown, expireIn?: number): void {
    const encoded = keyOf(key);
    const prior = this.values.get(encoded);
    this.values.set(encoded, {
      value,
      version: (prior?.version ?? 0) + 1,
      expiresAtMs: expireIn === undefined ? null : Date.now() + expireIn,
    });
  }

  get<T>(key: Deno.KvKey, _options?: { consistency?: "strong" | "eventual" }): Promise<Deno.KvEntryMaybe<T>> {
    const encoded = keyOf(key);
    const stored = this.values.get(encoded);
    if (stored?.expiresAtMs !== null && stored?.expiresAtMs !== undefined && stored.expiresAtMs <= Date.now()) {
      this.values.delete(encoded);
    }
    const current = this.values.get(encoded);
    return Promise.resolve({
      key,
      value: (current?.value ?? null) as T | null,
      versionstamp: current ? versionstamp(current.version) : null,
    } as Deno.KvEntryMaybe<T>);
  }

  async set(key: Deno.KvKey, value: unknown, options?: { expireIn?: number }): Promise<Deno.KvCommitResult> {
    const isAffinityKey = CODEX_ACCOUNT_AFFINITY_KV_PREFIX.every((part, index) => key[index] === part);
    if (isAffinityKey) {
      this.onAffinitySet?.();
      await this.affinitySetGate;
    }
    if (options?.expireIn !== undefined) this.expireIns.push(options.expireIn);
    this.put(key, value, options?.expireIn);
    const stored = this.values.get(keyOf(key))!;
    return { ok: true, versionstamp: versionstamp(stored.version) };
  }

  atomic(): Deno.AtomicOperation {
    const checks: Array<{ key: Deno.KvKey; versionstamp: string | null }> = [];
    const writes: Array<{ type: "delete" | "set"; key: Deno.KvKey; value?: unknown; expireIn?: number }> = [];
    const chain = {
      check: (...entries: Array<{ key: Deno.KvKey; versionstamp: string | null }>) => {
        checks.push(...entries);
        return chain;
      },
      delete: (key: Deno.KvKey) => {
        writes.push({ type: "delete", key });
        return chain;
      },
      set: (key: Deno.KvKey, value: unknown, options?: { expireIn?: number }) => {
        writes.push({ type: "set", key, value, expireIn: options?.expireIn });
        return chain;
      },
      commit: () => {
        for (const check of checks) {
          const current = this.values.get(keyOf(check.key));
          const actual = current ? versionstamp(current.version) : null;
          if (actual !== check.versionstamp) return Promise.resolve({ ok: false } as const);
        }
        for (const write of writes) {
          if (write.type === "delete") this.values.delete(keyOf(write.key));
          else this.put(write.key, write.value, write.expireIn);
        }
        const last = writes.at(-1);
        const stored = last ? this.values.get(keyOf(last.key)) : null;
        return Promise.resolve({ ok: true, versionstamp: versionstamp(stored?.version ?? 0) } as const);
      },
    };
    return chain as unknown as Deno.AtomicOperation;
  }

  affinityRecords(): Array<Readonly<{ key: Deno.KvKey; value: unknown }>> {
    return [...this.values.entries()]
      .map(([encoded, stored]) => ({ key: JSON.parse(encoded) as Deno.KvKey, value: stored.value }))
      .filter(({ key }) =>
        key[0] === CODEX_ACCOUNT_AFFINITY_KV_PREFIX[0] &&
        key[1] === CODEX_ACCOUNT_AFFINITY_KV_PREFIX[1] &&
        key[2] === CODEX_ACCOUNT_AFFINITY_KV_PREFIX[2]
      );
  }
}

let nowMs = 1_700_000_000_000;

const base64Url = (value: unknown): string =>
  btoa(JSON.stringify(value)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");

const auth = (label: string, tokenLabel = label): CodexAuthState => ({
  access_token: `${base64Url({ alg: "none" })}.${base64Url({ exp: (nowMs + 60 * 60_000) / 1_000 })}.${tokenLabel}`,
  refresh_token: `refresh-${tokenLabel}`,
  account_id: `account-${label}`,
  updated_at_ms: nowMs,
});

const pool = (...accounts: CodexAuthState[]): CodexAuthPoolState => ({ accounts, updated_at_ms: nowMs });

const installPool = (kv: AffinityKv, next: CodexAuthPoolState): void => {
  kv.put(CODEX_AUTH_POOL_KV_KEY, next);
  cacheCodexAuthPool(next);
};

const withFixture = async (
  callback: (fixture: Readonly<{ kv: AffinityKv; advance: (milliseconds: number) => void }>) => Promise<void>,
): Promise<void> => {
  const originalFetch = globalThis.fetch;
  const originalNow = Date.now;
  nowMs = 1_700_000_000_000;
  Date.now = () => nowMs;
  const kv = new AffinityKv();
  setKvForTest(kv as unknown as Deno.Kv);
  resetCodexAuthCacheForTest();
  resetCodexAccountRoutingForTest();
  try {
    await callback({ kv, advance: (milliseconds) => nowMs += milliseconds });
  } finally {
    globalThis.fetch = originalFetch;
    Date.now = originalNow;
    setKvForTest(null);
    resetCodexAuthCacheForTest();
    resetCodexAccountRoutingForTest();
  }
};

const cacheableBody = (promptCacheKey = "explicit-cache-key"): Record<string, unknown> => ({
  model: "gpt-5.6-luna",
  prompt_cache_key: promptCacheKey,
  input: "cache affinity fixture",
});

Deno.test("Codex account-affinity stores only bounded opaque data and separates principals, keys, expiry, and concurrent writes", async () => {
  await withFixture(async ({ kv, advance }) => {
    const principal = "api-key:principal-secret";
    const cacheKey = "cache-key-secret";
    const identity = await deriveCodexAccountAffinityIdentity(principal, cacheKey);
    const anotherPrincipal = await deriveCodexAccountAffinityIdentity("api-key:another-principal", cacheKey);
    const anotherKey = await deriveCodexAccountAffinityIdentity(principal, "another-cache-key");
    const broadRepositoryPrincipal = await deriveCodexAccountAffinityIdentity("github-repo:owner/repository", cacheKey);
    const broadMethodPrincipal = await deriveCodexAccountAffinityIdentity("auth-method:admin_allowlist", cacheKey);

    assert.ok(identity);
    assert.ok(anotherPrincipal);
    assert.ok(anotherKey);
    assert.notDeepEqual(identity.kvKey, anotherPrincipal.kvKey);
    assert.notDeepEqual(identity.kvKey, anotherKey.kvKey);
    assert.equal(broadRepositoryPrincipal, null);
    assert.equal(broadMethodPrincipal, null);
    assert.equal(JSON.stringify(identity.kvKey).includes(principal), false);
    assert.equal(JSON.stringify(identity.kvKey).includes(cacheKey), false);

    const firstCohort = "a".repeat(64);
    const secondCohort = "b".repeat(64);
    await Promise.all([
      recordCodexAccountAffinity(identity, firstCohort),
      recordCodexAccountAffinity(identity, secondCohort),
    ]);
    const stored = kv.affinityRecords();
    assert.equal(stored.length, 1);
    assert.deepEqual(Object.keys(stored[0]!.value as Record<string, unknown>).sort(), [
      "account_cohort_hash",
      "expires_at_ms",
    ]);
    assert.equal(JSON.stringify(stored[0]).includes(principal), false);
    assert.equal(JSON.stringify(stored[0]).includes(cacheKey), false);
    assert.equal(kv.expireIns.every((expireIn) => expireIn === CODEX_ACCOUNT_AFFINITY_TTL_MS), true);
    assert.match(String((stored[0]!.value as { account_cohort_hash?: unknown }).account_cohort_hash), /^[a-f0-9]{64}$/);
    const observedCohort = await readCodexAccountAffinity(identity);
    assert.ok(observedCohort === firstCohort || observedCohort === secondCohort);

    advance(CODEX_ACCOUNT_AFFINITY_TTL_MS + 1);
    assert.equal(await readCodexAccountAffinity(identity), null);
  });
});

Deno.test("Codex account-affinity prefers the completed keyed account, separates keys and principals, and leaves unkeyed traffic unchanged", async () => {
  await withFixture(async ({ kv }) => {
    const one = auth("one");
    const two = auth("two");
    installPool(kv, pool(one, two));
    const accountIds: string[] = [];
    const logs: string[] = [];
    const originalInfo = console.info;
    console.info = (...args: unknown[]) => logs.push(args.map(String).join(" "));
    globalThis.fetch = (input, init) => {
      const request = new Request(input, init);
      accountIds.push(request.headers.get("ChatGPT-Account-ID") ?? "");
      return Promise.resolve(new Response("{}", { status: 200 }));
    };

    try {
      const first = await fetchCodexResponses(cacheableBody(), { cacheScope: "api-key:principal-one" });
      assert.equal(getCodexResponseAffinityOutcome(first), "none");
      await markCodexResponseCompleted(first);

      // A reordered credential pool would normally select account two. The
      // durable opaque affinity record must restore account one only here.
      installPool(kv, pool(two, one));
      const preferred = await fetchCodexResponses(cacheableBody(), { cacheScope: "api-key:principal-one" });
      assert.equal(getCodexResponseAffinityOutcome(preferred), "preferred");
      await markCodexResponseCompleted(preferred);

      const otherKey = await fetchCodexResponses(cacheableBody("different-cache-key"), {
        cacheScope: "api-key:principal-one",
      });
      const otherPrincipal = await fetchCodexResponses(cacheableBody(), { cacheScope: "api-key:principal-two" });
      const unkeyed = await fetchCodexResponses({ model: "gpt-5.6-luna", input: "unkeyed" }, {
        cacheScope: "api-key:principal-one",
      });
      assert.equal(getCodexResponseAffinityOutcome(otherKey), "none");
      assert.equal(getCodexResponseAffinityOutcome(otherPrincipal), "none");
      assert.equal(getCodexResponseAffinityOutcome(unkeyed), "none");
      assert.deepEqual(accountIds, ["account-one", "account-one", "account-two", "account-two", "account-two"]);
      assert.equal(kv.affinityRecords().length, 1, "only completed keyed responses may create affinity records");
      assert.equal(logs.join("\n").includes("explicit-cache-key"), false);
      assert.equal(logs.join("\n").includes("principal-one"), false);
      assert.equal(logs.join("\n").includes("account-one"), false);
    } finally {
      console.info = originalInfo;
    }
  });
});

Deno.test("Codex account-affinity remaps only after authoritative quota failure and a later successful account", async () => {
  await withFixture(async ({ kv }) => {
    const one = auth("one");
    const two = auth("two");
    installPool(kv, pool(one, two));
    const accountIds: string[] = [];
    let shouldQuotaFailOne = false;
    globalThis.fetch = (input, init) => {
      const request = new Request(input, init);
      const accountId = request.headers.get("ChatGPT-Account-ID") ?? "";
      accountIds.push(accountId);
      if (shouldQuotaFailOne && accountId === "account-one") {
        return Promise.resolve(
          new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
            status: 429,
            headers: { "Content-Type": "application/json", "Retry-After": new Date(nowMs + 60_000).toUTCString() },
          }),
        );
      }
      return Promise.resolve(new Response("{}", { status: 200 }));
    };

    const established = await fetchCodexResponses(cacheableBody(), { cacheScope: "api-key:quota-principal" });
    await markCodexResponseCompleted(established);
    shouldQuotaFailOne = true;

    const remapped = await fetchCodexResponses(cacheableBody(), { cacheScope: "api-key:quota-principal" });
    assert.equal(remapped.status, 200);
    assert.equal(getCodexResponseAffinityOutcome(remapped), "remapped");
    await markCodexResponseCompleted(remapped);
    shouldQuotaFailOne = false;

    const preferredAfterRemap = await fetchCodexResponses(cacheableBody(), {
      cacheScope: "api-key:quota-principal",
    });
    assert.equal(getCodexResponseAffinityOutcome(preferredAfterRemap), "preferred");
    assert.deepEqual(accountIds, ["account-one", "account-one", "account-two", "account-two"]);
  });
});

Deno.test("Codex account-affinity remaps after an authoritative credential failure without retrying that account", async () => {
  await withFixture(async ({ kv }) => {
    const one = auth("one");
    const two = auth("two");
    installPool(kv, pool(one, two));
    const accountIds: string[] = [];
    let rejectOneCredential = false;
    globalThis.fetch = (input, init) => {
      const request = new Request(input, init);
      if (request.url === "https://auth.openai.com/oauth/token") {
        return Promise.resolve(new Response(JSON.stringify({ error: "invalid_grant" }), { status: 400 }));
      }
      const accountId = request.headers.get("ChatGPT-Account-ID") ?? "";
      accountIds.push(accountId);
      if (rejectOneCredential && accountId === "account-one") {
        return Promise.resolve(new Response("{}", { status: 401 }));
      }
      return Promise.resolve(new Response("{}", { status: 200 }));
    };

    const established = await fetchCodexResponses(cacheableBody(), { cacheScope: "api-key:auth-principal" });
    await markCodexResponseCompleted(established);
    rejectOneCredential = true;

    const remapped = await fetchCodexResponses(cacheableBody(), { cacheScope: "api-key:auth-principal" });
    assert.equal(remapped.status, 200);
    assert.equal(getCodexResponseAffinityOutcome(remapped), "remapped");
    await markCodexResponseCompleted(remapped);

    const preferredAfterRemap = await fetchCodexResponses(cacheableBody(), {
      cacheScope: "api-key:auth-principal",
    });
    assert.equal(getCodexResponseAffinityOutcome(preferredAfterRemap), "preferred");
    assert.deepEqual(accountIds, ["account-one", "account-one", "account-two", "account-two"]);
  });
});

Deno.test("Codex account-affinity does not turn transient failures into quota, remaps removed accounts, and honors same-account credential rotation", async () => {
  await withFixture(async ({ kv }) => {
    const one = auth("one");
    const two = auth("two");
    installPool(kv, pool(one, two));
    const accountIds: string[] = [];
    let responseMode: "network" | "ok" | "server_error" = "ok";
    globalThis.fetch = (input, init) => {
      const request = new Request(input, init);
      accountIds.push(request.headers.get("ChatGPT-Account-ID") ?? "");
      if (responseMode === "network") return Promise.reject(new TypeError("fixture network failure"));
      if (responseMode === "server_error") return Promise.resolve(new Response("upstream failure", { status: 503 }));
      return Promise.resolve(new Response("{}", { status: 200 }));
    };

    const established = await fetchCodexResponses(cacheableBody(), { cacheScope: "api-key:rotation-principal" });
    await markCodexResponseCompleted(established);

    responseMode = "network";
    await assert.rejects(
      () => fetchCodexResponses(cacheableBody(), { cacheScope: "api-key:rotation-principal" }),
      (error: unknown) => error instanceof CodexError && error.code === "codex_upstream_unreachable",
    );
    responseMode = "server_error";
    const serverError = await fetchCodexResponses(cacheableBody(), { cacheScope: "api-key:rotation-principal" });
    assert.equal(serverError.status, 503);
    assert.equal(getCodexResponseAffinityOutcome(serverError), "preferred");
    responseMode = "ok";

    // A new token for the same account keeps the same opaque account cohort.
    installPool(kv, pool({ ...auth("one", "rotated"), account_id: one.account_id }, two));
    const afterCredentialRotation = await fetchCodexResponses(cacheableBody(), {
      cacheScope: "api-key:rotation-principal",
    });
    assert.equal(getCodexResponseAffinityOutcome(afterCredentialRotation), "preferred");
    await markCodexResponseCompleted(afterCredentialRotation);

    // Removing the preferred account makes it unavailable. A failed alternate
    // reports that state without overwriting affinity; a later successful one
    // is the only operation that remaps it.
    installPool(kv, pool(two));
    responseMode = "server_error";
    const unavailable = await fetchCodexResponses(cacheableBody(), { cacheScope: "api-key:rotation-principal" });
    assert.equal(getCodexResponseAffinityOutcome(unavailable), "preferred_unavailable");
    responseMode = "ok";
    const remapped = await fetchCodexResponses(cacheableBody(), { cacheScope: "api-key:rotation-principal" });
    assert.equal(getCodexResponseAffinityOutcome(remapped), "remapped");
    await markCodexResponseCompleted(remapped);

    assert.deepEqual(accountIds, [
      "account-one",
      "account-one",
      "account-two",
      "account-one",
      "account-one",
      "account-two",
      "account-two",
    ]);
  });
});

Deno.test("Codex account-affinity remaps a sibling transport success only after its validated terminal", async () => {
  await withFixture(async ({ kv }) => {
    const one = auth("one");
    const two = auth("two");
    installPool(kv, pool(one, two));
    const accountIds: string[] = [];
    let failPreferredTransport = false;
    globalThis.fetch = (input, init) => {
      const request = new Request(input, init);
      const accountId = request.headers.get("ChatGPT-Account-ID") ?? "";
      accountIds.push(accountId);
      if (failPreferredTransport && accountId === one.account_id) {
        return Promise.reject(new TypeError("fixture transport failure"));
      }
      return Promise.resolve(new Response("{}", { status: 200 }));
    };

    const cacheScope = "api-key:transport-remap-principal";
    const identity = await deriveCodexAccountAffinityIdentity(cacheScope, "explicit-cache-key");
    assert.ok(identity);
    const established = await fetchCodexResponses(cacheableBody(), { cacheScope });
    await markCodexResponseCompleted(established);
    const priorCohort = await readCodexAccountAffinity(identity);
    assert.ok(priorCohort);

    failPreferredTransport = true;
    const remapped = await fetchCodexResponses(cacheableBody(), { cacheScope });
    assert.equal(remapped.status, 200);
    assert.equal(getCodexResponseAffinityOutcome(remapped), "remapped");
    assert.equal(
      await readCodexAccountAffinity(identity),
      priorCohort,
      "headers alone must not persist the sibling account",
    );

    await markCodexResponseCompleted(remapped);
    assert.notEqual(await readCodexAccountAffinity(identity), priorCohort);
    failPreferredTransport = false;
    accountIds.length = 0;
    const preferredSibling = await fetchCodexResponses(cacheableBody(), { cacheScope });
    assert.equal(getCodexResponseAffinityOutcome(preferredSibling), "preferred");
    assert.deepEqual(accountIds, [two.account_id]);
  });
});

Deno.test("Codex account-affinity keeps the prior account after all sibling transports fail", async () => {
  await withFixture(async ({ kv }) => {
    const one = auth("one");
    const two = auth("two");
    installPool(kv, pool(one, two));
    const accountIds: string[] = [];
    let failEveryTransport = false;
    globalThis.fetch = (input, init) => {
      const request = new Request(input, init);
      accountIds.push(request.headers.get("ChatGPT-Account-ID") ?? "");
      if (failEveryTransport) return Promise.reject(new TypeError("fixture transport failure"));
      return Promise.resolve(new Response("{}", { status: 200 }));
    };

    const cacheScope = "api-key:all-transports-fail-principal";
    const identity = await deriveCodexAccountAffinityIdentity(cacheScope, "explicit-cache-key");
    assert.ok(identity);
    const established = await fetchCodexResponses(cacheableBody(), { cacheScope });
    await markCodexResponseCompleted(established);
    const priorCohort = await readCodexAccountAffinity(identity);
    assert.ok(priorCohort);

    failEveryTransport = true;
    accountIds.length = 0;
    await assert.rejects(
      () => fetchCodexResponses(cacheableBody(), { cacheScope }),
      (error: unknown) => error instanceof CodexError && error.code === "codex_upstream_unreachable",
    );
    assert.deepEqual(accountIds, [one.account_id, two.account_id]);
    assert.equal(await readCodexAccountAffinity(identity), priorCohort);

    failEveryTransport = false;
    accountIds.length = 0;
    const next = await fetchCodexResponses(cacheableBody(), { cacheScope });
    assert.equal(getCodexResponseAffinityOutcome(next), "preferred");
    assert.deepEqual(accountIds, [one.account_id]);
  });
});

Deno.test("Codex account-affinity ignores 5xx, progress cancellation, and empty completion terminals", async () => {
  await withFixture(async ({ kv }) => {
    const one = auth("one");
    const two = auth("two");
    installPool(kv, pool(one, two));
    let responseMode: "ok" | "server_error" | "remapped_progress" | "remapped_empty" = "ok";
    const accountIds: string[] = [];
    globalThis.fetch = (input, init) => {
      const request = new Request(input, init);
      const accountId = request.headers.get("ChatGPT-Account-ID") ?? "";
      accountIds.push(accountId);
      if (responseMode === "server_error") return Promise.resolve(new Response("upstream failure", { status: 503 }));
      if ((responseMode === "remapped_progress" || responseMode === "remapped_empty") && accountId === one.account_id) {
        return Promise.reject(new TypeError("fixture transport failure"));
      }
      if (responseMode === "remapped_progress") {
        return Promise.resolve(
          new Response('data: {"type":"response.reasoning_summary_text.delta","delta":"thinking"}\n\n', {
            status: 200,
            headers: { "Content-Type": "text/event-stream" },
          }),
        );
      }
      if (responseMode === "remapped_empty") {
        return Promise.resolve(
          new Response('data: {"type":"response.completed","response":{"output":[]}}\n\n', {
            status: 200,
            headers: { "Content-Type": "text/event-stream" },
          }),
        );
      }
      return Promise.resolve(new Response("{}", { status: 200 }));
    };

    const cacheScope = "api-key:nonterminal-principal";
    const identity = await deriveCodexAccountAffinityIdentity(cacheScope, "explicit-cache-key");
    assert.ok(identity);
    const established = await fetchCodexResponses(cacheableBody(), { cacheScope });
    await markCodexResponseCompleted(established);
    const priorCohort = await readCodexAccountAffinity(identity);
    assert.ok(priorCohort);

    responseMode = "server_error";
    accountIds.length = 0;
    const serverError = await fetchCodexResponses(cacheableBody(), { cacheScope });
    assert.equal(serverError.status, 503);
    assert.equal(getCodexResponseAffinityOutcome(serverError), "preferred");
    assert.deepEqual(accountIds, [one.account_id], "a 5xx response must not retry a sibling");
    assert.equal(await readCodexAccountAffinity(identity), priorCohort);

    responseMode = "remapped_progress";
    const cancelled = await fetchCodexResponses(cacheableBody(), { cacheScope });
    assert.equal(getCodexResponseAffinityOutcome(cancelled), "remapped");
    await releaseCodexResponseProbe(cancelled);
    assert.equal(await readCodexAccountAffinity(identity), priorCohort);

    responseMode = "remapped_empty";
    const empty = await fetchCodexResponses(cacheableBody(), { cacheScope });
    assert.equal(getCodexResponseAffinityOutcome(empty), "remapped");
    await markCodexResponseUpstreamError(empty);
    assert.equal(await readCodexAccountAffinity(identity), priorCohort);
  });
});

Deno.test("Codex account-affinity never bypasses an existing quota or banked-reset routing fence", async () => {
  await withFixture(async ({ kv }) => {
    const one = auth("one");
    const two = auth("two");
    installPool(kv, pool(one, two));
    const accountIds: string[] = [];
    globalThis.fetch = (input, init) => {
      const request = new Request(input, init);
      accountIds.push(request.headers.get("ChatGPT-Account-ID") ?? "");
      return Promise.resolve(new Response("{}", { status: 200 }));
    };

    const established = await fetchCodexResponses(cacheableBody(), { cacheScope: "api-key:fenced-principal" });
    await markCodexResponseCompleted(established);
    const initial = await selectCodexRoutingAccounts(pool(one, two), [one, two], nowMs, "gpt-5.6-luna");
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    await markCodexQuotaBlocked(
      initial.accounts.find((account) => account.auth.account_id === "account-one")!,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(nowMs + 60_000).toUTCString() },
      }),
      nowMs,
    );

    const fenced = await fetchCodexResponses(cacheableBody(), { cacheScope: "api-key:fenced-principal" });
    assert.equal(fenced.status, 200);
    assert.equal(getCodexResponseAffinityOutcome(fenced), "remapped");
    assert.deepEqual(accountIds, ["account-one", "account-two"]);
  });
});

Deno.test("completed half-open probes clear routing before slow affinity persistence", async () => {
  await withFixture(async ({ kv, advance }) => {
    const one = auth("one");
    installPool(kv, pool(one));
    const initial = await selectCodexRoutingAccounts(pool(one), [one], nowMs, "gpt-5.6-luna");
    assert.equal(initial.kind, "eligible");
    if (initial.kind !== "eligible") return;
    await markCodexQuotaBlocked(
      initial.accounts[0]!,
      new Response(JSON.stringify({ error: { type: "usage_limit_reached" } }), {
        status: 429,
        headers: { "Content-Type": "application/json", "Retry-After": new Date(nowMs + 1_000).toUTCString() },
      }),
      nowMs,
    );
    advance(1_001);

    let inferenceCalls = 0;
    globalThis.fetch = () => {
      inferenceCalls += 1;
      return Promise.resolve(new Response("{}", { status: 200 }));
    };

    let signalAffinitySet!: () => void;
    const affinitySetEntered = new Promise<void>((resolve) => {
      signalAffinitySet = resolve;
    });
    let releaseAffinitySet!: () => void;
    kv.affinitySetGate = new Promise<void>((resolve) => {
      releaseAffinitySet = resolve;
    });
    kv.onAffinitySet = signalAffinitySet;

    let completion: Promise<void> | null = null;
    try {
      const recovered = await fetchCodexResponses(cacheableBody(), {
        cacheScope: "api-key:slow-affinity-principal",
      });
      assert.equal(recovered.status, 200);
      assert.ok(getCodexRoutingProbe(recovered));

      completion = markCodexResponseCompleted(recovered);
      await affinitySetEntered;

      const concurrent = await fetchCodexResponses(cacheableBody(), {
        cacheScope: "api-key:slow-affinity-principal",
      });
      assert.equal(concurrent.status, 200);
      assert.equal(inferenceCalls, 2);
    } finally {
      releaseAffinitySet();
      await completion;
      kv.affinitySetGate = null;
      kv.onAffinitySet = null;
    }
  });
});
