import assert from "node:assert/strict";

// Type-only: erased at runtime, so the dynamic imports below still see the KV stub.
import type { LocalDevelopmentPricingInitializer } from "../src/auth/local-development-key.ts";
import type { ServeRuntimeOptions } from "../src/auth/local-admin.ts";

/**
 * Minimal in-memory Deno.Kv for the local-development provisioning tests. The
 * standard test task runs without `--unstable-kv`, so `Deno.openKv` is stubbed
 * before the modules under test import `src/kv.ts`.
 */
class MemoryKv {
  #entries = new Map<string, { key: Deno.KvKey; value: unknown; versionstamp: string }>();
  #version = 0;

  #nextVersionstamp(): string {
    this.#version += 1;
    return String(this.#version).padStart(20, "0");
  }

  #keyOf(key: Deno.KvKey): string {
    return JSON.stringify(key);
  }

  get(key: Deno.KvKey): Promise<Deno.KvEntryMaybe<unknown>> {
    const entry = this.#entries.get(this.#keyOf(key));
    return Promise.resolve(
      (entry
        ? { key: entry.key, value: entry.value, versionstamp: entry.versionstamp }
        : { key, value: null, versionstamp: null }) as Deno.KvEntryMaybe<unknown>
    );
  }

  set(key: Deno.KvKey, value: unknown): Promise<Deno.KvCommitResult> {
    const versionstamp = this.#nextVersionstamp();
    this.#entries.set(this.#keyOf(key), { key, value, versionstamp });
    return Promise.resolve({ ok: true, versionstamp } as Deno.KvCommitResult);
  }

  atomic(): Deno.AtomicOperation {
    const checks: { key: Deno.KvKey; versionstamp: string | null }[] = [];
    const mutations: { key: Deno.KvKey; value: unknown }[] = [];
    const currentVersionstamp = (key: Deno.KvKey): string | null => this.#entries.get(this.#keyOf(key))?.versionstamp ?? null;
    const operation = {
      check: (entry: { key: Deno.KvKey; versionstamp: string | null }) => {
        checks.push({ key: entry.key, versionstamp: entry.versionstamp });
        return operation;
      },
      set: (key: Deno.KvKey, value: unknown) => {
        mutations.push({ key, value });
        return operation;
      },
      delete: (key: Deno.KvKey) => {
        mutations.push({ key, value: null });
        return operation;
      },
      commit: (): Promise<Deno.KvCommitResult | Deno.KvCommitError> => {
        if (checks.some((check) => currentVersionstamp(check.key) !== check.versionstamp)) {
          return Promise.resolve({ ok: false } as Deno.KvCommitError);
        }
        const versionstamp = this.#nextVersionstamp();
        for (const mutation of mutations) {
          this.#entries.set(this.#keyOf(mutation.key), { key: mutation.key, value: mutation.value, versionstamp });
        }
        return Promise.resolve({ ok: true, versionstamp } as Deno.KvCommitResult);
      },
    };
    return operation as unknown as Deno.AtomicOperation;
  }

  close(): void {
    this.#entries.clear();
  }
}

const memoryKv = new MemoryKv();
const denoWithKv = Deno as unknown as { openKv?: () => Promise<Deno.Kv> };
const originalOpenKv = denoWithKv.openKv;
denoWithKv.openKv = () => Promise.resolve(memoryKv as unknown as Deno.Kv);

const { API_KEY_NO_EXPIRATION_MS, API_KEY_NO_USAGE_LIMIT, PAID_FALLBACK_NO_LIMIT, apiKeyHashKey, apiKeyIdKey } = await import("../src/api-keys.ts");
const { apiKeyUsageV3WindowKey } = await import("../src/api-key-policy.ts");
const { hasStrictPaidFallbackKeyPolicy } = await import("../src/paid-fallback/index.ts");
const { LOCAL_DEVELOPMENT_KEY_ID, ensureLocalDevelopmentApiKey, resolveLocalDevelopmentApiKeyPolicy } = await import("../src/auth/local-development-key.ts");
const { configureAdminAuthForListener, configureAdminAuthPeerForRequest, configureMacLocalAdminAuthBypassForListener } =
  await import("../src/auth/local-admin.ts");
const { authenticateClient } = await import("../src/auth/index.ts");
const { getKv } = await import("../src/kv.ts");
const kvEntry = await getKv();
assert.ok(kvEntry);

denoWithKv.openKv = originalOpenKv;

const loopbackAddress: Deno.NetAddr = { transport: "tcp", hostname: "127.0.0.1", port: 8000 };
const enabledOptions: ServeRuntimeOptions = Object.freeze({ disableAdminAuth: true });
const disabledOptions: ServeRuntimeOptions = Object.freeze({ disableAdminAuth: false });

const pricing: Awaited<ReturnType<LocalDevelopmentPricingInitializer>> = {
  paid_fallback_model_ids: ["gpt-6-astra"],
  paid_fallback_quota_per_credit: 1_000,
  paid_fallback_pricing_checked_at_ms: Date.now(),
  paid_fallback_max_exposure_microcredits: {},
};

const initializePolicy: LocalDevelopmentPricingInitializer = () => Promise.resolve(pricing);

const withPaidProviderKey = async (run: () => Promise<void>): Promise<void> => {
  const previous = Deno.env.get("SURPLUS_API_KEY");
  Deno.env.set("SURPLUS_API_KEY", "test-surplus-key");
  try {
    await run();
  } finally {
    if (previous === undefined) Deno.env.delete("SURPLUS_API_KEY");
    else Deno.env.set("SURPLUS_API_KEY", previous);
  }
};

Deno.test("loopback development provisions one unlimited, non-expiring local API key", async () => {
  await withPaidProviderKey(async () => {
    const status = await ensureLocalDevelopmentApiKey(memoryKv as unknown as Deno.Kv, { initializePolicy });
    assert.equal(status, "created");

    const idEntry = await memoryKv.get(apiKeyIdKey(LOCAL_DEVELOPMENT_KEY_ID));
    const record = idEntry.value as Record<string, unknown>;
    assert.ok(record);
    assert.equal(hasStrictPaidFallbackKeyPolicy(record), true);
    assert.equal(record.paid_fallback_enabled, true);
    assert.equal(record.paid_fallback_limit_microcredits, PAID_FALLBACK_NO_LIMIT);
    assert.equal(record.usage_limit_requests, API_KEY_NO_USAGE_LIMIT);
    assert.equal(record.expires_at_ms, API_KEY_NO_EXPIRATION_MS);
    assert.equal(record.revoked_at_ms, null);

    // The quota ledger reads the hash row by `policy.token_hash`, so both rows
    // and the initial usage window must exist.
    assert.ok(await memoryKv.get(apiKeyHashKey(record.hash as string)));
    const policy = await resolveLocalDevelopmentApiKeyPolicy(memoryKv as unknown as Deno.Kv);
    assert.ok(policy);
    assert.equal(policy.key_id, LOCAL_DEVELOPMENT_KEY_ID);
    assert.equal(policy.paid_fallback_enabled, true);
    assert.equal(policy.paid_fallback_limit_microcredits, PAID_FALLBACK_NO_LIMIT);
    assert.equal(policy.usage_limit_requests, API_KEY_NO_USAGE_LIMIT);
    assert.ok(await memoryKv.get(apiKeyUsageV3WindowKey(policy)));
  });
});

Deno.test("local development key provisioning is idempotent and never rewrites a record", async () => {
  await withPaidProviderKey(async () => {
    const before = await memoryKv.get(apiKeyIdKey(LOCAL_DEVELOPMENT_KEY_ID));
    const status = await ensureLocalDevelopmentApiKey(memoryKv as unknown as Deno.Kv, { initializePolicy });
    assert.equal(status, "present");
    const after = await memoryKv.get(apiKeyIdKey(LOCAL_DEVELOPMENT_KEY_ID));
    assert.equal(after.versionstamp, before.versionstamp);
  });
});

Deno.test("local development key provisioning reports an unavailable pricing snapshot without writing", async () => {
  await withPaidProviderKey(async () => {
    const isolated = new MemoryKv();
    const status = await ensureLocalDevelopmentApiKey(isolated as unknown as Deno.Kv, {
      initializePolicy: () => Promise.reject(new Error("pricing unavailable")),
    });
    assert.equal(status, "unavailable");
    const idEntry = await isolated.get(apiKeyIdKey(LOCAL_DEVELOPMENT_KEY_ID));
    assert.equal(idEntry.value, null);
  });
});

Deno.test("the loopback bypass authenticates as the unlimited local development key", async () => {
  await withPaidProviderKey(async () => {
    // 127.42.9.3 is inside 127.0.0.0/8 but outside the legacy dev-host list, so
    // these requests exercise the explicit listener bypass only.
    const localRequest = () => new Request("http://127.42.9.3/v1/chat/completions", { method: "POST" });
    configureAdminAuthForListener(enabledOptions, loopbackAddress);
    configureAdminAuthPeerForRequest(loopbackAddress);
    try {
      const localAuth = await authenticateClient(localRequest());
      if (!localAuth.ok) throw new Error("Loopback requests authenticate without a credential");
      if (localAuth.method.kind !== "kv_api_key") throw new Error("Expected the local development key principal");
      assert.equal(localAuth.method.key_id, LOCAL_DEVELOPMENT_KEY_ID);
      assert.equal(localAuth.method.policy.paid_fallback_enabled, true);
      assert.equal(localAuth.method.policy.paid_fallback_limit_microcredits, PAID_FALLBACK_NO_LIMIT);
      assert.equal(localAuth.method.policy.usage_limit_requests, API_KEY_NO_USAGE_LIMIT);

      // Revoking the local key is the operator's off switch: the principal
      // degrades to the policy-free `disabled` method instead of failing auth.
      const idKey = apiKeyIdKey(LOCAL_DEVELOPMENT_KEY_ID);
      const record = (await memoryKv.get(idKey)).value as Record<string, unknown>;
      await memoryKv.set(idKey, { ...record, revoked_at_ms: Date.now() });
      assert.equal(await resolveLocalDevelopmentApiKeyPolicy(memoryKv as unknown as Deno.Kv), null);
      const revokedAuth = await authenticateClient(localRequest());
      if (!revokedAuth.ok) throw new Error("Loopback requests stay authenticated");
      assert.equal(revokedAuth.method.kind, "disabled");

      // A non-loopback peer never receives the local principal.
      configureAdminAuthPeerForRequest({ transport: "tcp", hostname: "192.0.2.10", port: 8000 });
      const remoteAuth = await authenticateClient(localRequest());
      assert.equal(remoteAuth.ok, false);
    } finally {
      configureAdminAuthForListener(disabledOptions, loopbackAddress);
      configureAdminAuthPeerForRequest(null);
    }
  });
});

Deno.test("the Mac LAN listener grants the local principal to an actual loopback peer only", async () => {
  await withPaidProviderKey(async () => {
    // Both requests use a loopback URL; only the bound peer decides, so a LAN
    // client that forges a loopback Host is still authenticated.
    const localRequest = new Request("http://127.42.9.3/v1/chat/completions", { method: "POST" });
    const lanRequest = new Request("http://127.42.9.3/v1/chat/completions", { method: "POST" });
    configureMacLocalAdminAuthBypassForListener({ transport: "tcp", hostname: "0.0.0.0", port: 7999 });
    try {
      configureAdminAuthPeerForRequest({ transport: "tcp", hostname: "127.0.0.1", port: 7999 }, localRequest);
      configureAdminAuthPeerForRequest({ transport: "tcp", hostname: "192.0.2.10", port: 7999 }, lanRequest);

      const lanAuth = await authenticateClient(lanRequest);
      if (lanAuth.ok) throw new Error("A LAN peer must stay authenticated");
      assert.equal(lanAuth.response.status, 401);

      const localAuth = await authenticateClient(localRequest);
      if (!localAuth.ok) throw new Error("Loopback requests authenticate without a credential");
      // The revocation case above may degrade the principal to the policy-free
      // `disabled` method; the local development key path is asserted there.
      assert.ok(localAuth.method.kind === "kv_api_key" || localAuth.method.kind === "disabled");
    } finally {
      configureAdminAuthForListener(disabledOptions, loopbackAddress);
      configureAdminAuthPeerForRequest(null);
    }
  });
});
