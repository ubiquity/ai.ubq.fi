import assert from "node:assert/strict";

const encodeBase64Url = (value: string): string =>
  btoa(value).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);
const kvVersions = new Map<string, number>();
let beforeAtomicCommit: (() => void) | null = null;

class KvTestStore extends Map<string, unknown> {
  override set(key: string, value: unknown): this {
    kvVersions.set(key, (kvVersions.get(key) ?? 0) + 1);
    return super.set(key, value);
  }

  override clear(): void {
    beforeAtomicCommit = null;
    kvVersions.clear();
    super.clear();
  }
}

const kvStore = new KvTestStore();
const versionstampFor = (rawKey: string): string | null =>
  kvStore.has(rawKey) ? String(kvVersions.get(rawKey) ?? 0).padStart(20, "0") : null;

const kvStub = {
  get: (key: Deno.KvKey) => {
    const rawKey = keyToString(key);
    return Promise.resolve(
      ({
        key,
        value: kvStore.has(rawKey) ? kvStore.get(rawKey) : null,
        versionstamp: versionstampFor(rawKey),
      }) as Deno.KvEntryMaybe<unknown>,
    );
  },
  set: (key: Deno.KvKey, value: unknown, _options?: { expireIn?: number }) => {
    kvStore.set(keyToString(key), value);
    return Promise.resolve({ ok: true } as const);
  },
  delete: (key: Deno.KvKey) => {
    kvStore.delete(keyToString(key));
    return Promise.resolve();
  },
  list: async function* (selector: Deno.KvListSelector, options?: Deno.KvListOptions) {
    const prefix = "prefix" in selector ? selector.prefix : [];
    let yielded = 0;
    const limit = typeof options?.limit === "number" ? options.limit : Infinity;
    for (const [rawKey, value] of kvStore.entries()) {
      const key = JSON.parse(rawKey) as Deno.KvKey;
      const matchesPrefix = prefix.every((part, index) => key[index] === part);
      if (!matchesPrefix) continue;
      yield { key, value, versionstamp: "00000000000000010000" } as Deno.KvEntry<unknown>;
      yielded += 1;
      if (yielded >= limit) break;
    }
  },
  atomic: () => {
    const ops: Array<{ type: "set" | "delete"; key: Deno.KvKey; value?: unknown }> = [];
    const checks: Array<{ key: Deno.KvKey; versionstamp: string | null }> = [];
    const chain = {
      check: (check: { key: Deno.KvKey; versionstamp: string | null }) => {
        checks.push(check);
        return chain;
      },
      set: (key: Deno.KvKey, value: unknown, _options?: { expireIn?: number }) => {
        ops.push({ type: "set", key, value });
        return chain;
      },
      delete: (key: Deno.KvKey) => {
        ops.push({ type: "delete", key });
        return chain;
      },
      commit: () => {
        beforeAtomicCommit?.();
        beforeAtomicCommit = null;
        for (const check of checks) {
          if (versionstampFor(keyToString(check.key)) !== check.versionstamp) {
            return Promise.resolve({ ok: false } as const);
          }
        }
        for (const op of ops) {
          if (op.type === "set") kvStore.set(keyToString(op.key), op.value);
          else kvStore.delete(keyToString(op.key));
        }
        return Promise.resolve({ ok: true } as const);
      },
    };
    return chain;
  },
  close: () => {},
} as unknown as Deno.Kv;

(Deno as unknown as { openKv?: () => Promise<Deno.Kv> }).openKv = () => Promise.resolve(kvStub);

const {
  PASSKEY_SESSION_TTL_MS,
  buildPasskeyHandle,
  getPasskeyRequestMeta,
  handlePasskeyLoginFinish,
  handlePasskeyLoginStart,
  handlePasskeyRegisterStart,
  handlePasskeySession,
  handlePasskeyUsersList,
  handlePasskeyUsersUpdate,
  hasPasskeyUsers,
  normalizePasskeyHandle,
  passkeyChallengeKey,
  passkeyCredentialKey,
  passkeyHandleKey,
  passkeySessionKey,
  passkeyUserKey,
  saveVerifiedPasskeyRegistration,
  updatePasskeyCredentialSignCount,
} = await import("../src/passkeys.ts");
const { authenticateAdmin, authenticateClient, handleV1Auth, requireAdminAuth } = await import("../src/auth.ts");
const { apiKeyHashKey, apiKeyIdKey } = await import("../src/api_keys.ts");
const { sha256Base64Url } = await import("../src/utils.ts");

const withEnv = async (updates: Record<string, string | null>, fn: () => Promise<void>): Promise<void> => {
  const originalGet = Deno.env.get;
  Deno.env.get = (key: string): string | undefined => {
    if (Object.prototype.hasOwnProperty.call(updates, key)) return updates[key] ?? undefined;
    return originalGet.call(Deno.env, key);
  };
  try {
    await fn();
  } finally {
    Deno.env.get = originalGet;
  }
};

const seedPasskeySession = (token = "uos_ai_session_test", { isAdmin = true } = {}) => {
  const now = Date.now();
  const user = {
    id: "user-test",
    handle: "uos-passkey-test",
    is_admin: isAdmin,
    credential_ids: ["credential-test"],
    created_at_ms: now,
    updated_at_ms: now,
  };
  kvStore.set(keyToString(passkeyUserKey(user.id)), user);
  kvStore.set(keyToString(passkeyHandleKey(user.handle)), user.id);
  kvStore.set(keyToString(passkeySessionKey(token)), {
    token,
    user_id: user.id,
    created_at_ms: now,
    expires_at_ms: now + PASSKEY_SESSION_TTL_MS,
  });
  return { token, user };
};

Deno.test("passkey session authenticates as client and admin", async () => {
  kvStore.clear();
  const { token, user } = seedPasskeySession();
  const req = new Request("https://ai.ubq.fi/uos/auth", {
    headers: { Authorization: `Bearer ${token}` },
  });

  const clientAuth = await authenticateClient(req);
  assert.equal(clientAuth.ok, true);
  if (clientAuth.ok) {
    assert.equal(clientAuth.method.kind, "passkey_session");
    assert.equal(clientAuth.method.handle, user.handle);
    assert.equal(clientAuth.method.credential_count, 1);
  }

  const adminError = await requireAdminAuth(req);
  assert.equal(adminError, null);

  const whoami = await handleV1Auth(req);
  assert.equal(whoami.status, 200);
  const body = await whoami.json();
  assert.equal(body.auth.is_admin, true);
  assert.equal(body.auth.is_super_admin, false);
  assert.equal(body.auth.method.kind, "passkey_session");
  assert.equal(body.auth.method.user.handle, user.handle);
  assert.equal(body.auth.method.user.is_admin, true);
  assert.equal(body.auth.method.user.credential_count, 1);

  const sessionResponse = await handlePasskeySession(req);
  assert.equal(sessionResponse.status, 200);
  const sessionBody = await sessionResponse.json();
  assert.equal(sessionBody.user.handle, user.handle);
  assert.equal(sessionBody.user.credential_count, 1);
});

Deno.test("non-admin passkey session authenticates as client but not admin", async () => {
  kvStore.clear();
  const { token, user } = seedPasskeySession("uos_ai_session_user", { isAdmin: false });
  const req = new Request("https://ai.ubq.fi/uos/auth", {
    headers: { Authorization: `Bearer ${token}` },
  });

  const clientAuth = await authenticateClient(req);
  assert.equal(clientAuth.ok, true);
  if (clientAuth.ok) {
    assert.equal(clientAuth.method.kind, "passkey_session");
    assert.equal(clientAuth.method.handle, user.handle);
    assert.equal(clientAuth.method.is_admin, false);
    assert.equal(clientAuth.method.credential_count, 1);
  }

  const adminError = await requireAdminAuth(req);
  assert.equal(adminError?.status, 403);

  const whoami = await handleV1Auth(req);
  assert.equal(whoami.status, 200);
  const body = await whoami.json();
  assert.equal(body.auth.is_admin, false);
  assert.equal(body.auth.is_super_admin, false);
  assert.equal(body.auth.method.user.is_admin, false);
  assert.equal(body.auth.method.user.credential_count, 1);
});

Deno.test("API key window reset releases stale paid fallback reservations", async () => {
  kvStore.clear();
  const token = "uos_ai_key_window_reset_test";
  const hash = await sha256Base64Url(token);
  const id = "key-window-reset";
  const now = Date.now();
  const common = {
    expires_at_ms: -1,
    revoked_at_ms: null,
    usage_limit_requests: 100,
    usage_requests: 9,
    usage_reset_at_ms: now - 1,
    window_ms: 60_000,
    paid_fallback_enabled: true,
    paid_fallback_limit_microcredits: 1_000_000,
    paid_fallback_spent_microcredits: 250_000,
    paid_fallback_reserved_microcredits: 750_000,
    paid_fallback_reservation_request_id: "stale-request",
  };
  kvStore.set(keyToString(apiKeyIdKey(id)), {
    id,
    name: "Window reset key",
    prefix: "uos_ai_key",
    hash,
    created_at_ms: now - 120_000,
    ...common,
    paid_fallback_model_ids: ["gpt-5-codex"],
    paid_fallback_quota_per_credit: 500_000,
    paid_fallback_pricing_checked_at_ms: now - 60_000,
  });
  kvStore.set(keyToString(apiKeyHashKey(hash)), { id, ...common });

  const result = await authenticateClient(
    new Request("https://ai.ubq.fi/uos/auth", {
      headers: { Authorization: `Bearer ${token}` },
    }),
  );
  assert.equal(result.ok, true);

  for (const key of [apiKeyIdKey(id), apiKeyHashKey(hash)]) {
    const stored = kvStore.get(keyToString(key)) as Record<string, unknown>;
    assert.equal(stored.usage_requests, 0);
    assert.equal(stored.paid_fallback_spent_microcredits, 0);
    assert.equal(stored.paid_fallback_reserved_microcredits, 0);
    assert.equal(stored.paid_fallback_reservation_request_id, null);
  }
});

Deno.test("Deno Deploy tokens are verified with the Deno API outside deployed runtime", async () => {
  kvStore.clear();
  const originalFetch = globalThis.fetch;
  const requested: Array<{ url: string; authorization: string | null }> = [];
  const token = "ddo_test_token_1234567890abcdefghijklmnopqrstuvwxyz";

  globalThis.fetch = (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    const headers = new Headers(init?.headers);
    requested.push({
      url: String(input),
      authorization: headers.get("authorization"),
    });
    return Promise.resolve(
      new Response("{}", {
        status: 200,
        headers: { "Content-Type": "application/json" },
      }),
    );
  };

  try {
    await withEnv({ DENO_DEPLOY_APP_SLUG: "ai-ubq-fi-test" }, async () => {
      const req = new Request("https://ai.ubq.fi/uos/auth", {
        headers: { Authorization: `Bearer ${token}` },
      });
      const adminAuth = await authenticateAdmin(req);
      assert.equal(adminAuth.ok, true);
      if (adminAuth.ok) {
        assert.equal(adminAuth.is_super_admin, true);
        assert.equal(adminAuth.method.kind, "deno_deploy_token");
      }

      const clientAuth = await authenticateClient(req);
      assert.equal(clientAuth.ok, true);
      if (clientAuth.ok) assert.equal(clientAuth.method.kind, "deno_deploy_token");

      const whoami = await handleV1Auth(req);
      assert.equal(whoami.status, 200);
      const body = await whoami.json();
      assert.equal(body.auth.is_admin, true);
      assert.equal(body.auth.is_super_admin, true);

      assert.equal(requested.length, 1);
      assert.equal(requested[0].url, "https://api.deno.com/v2/apps/ai-ubq-fi-test");
      assert.equal(requested[0].authorization, `Bearer ${token}`);
    });
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("Deno Deploy console tokens are verified against the app page", async () => {
  kvStore.clear();
  const originalFetch = globalThis.fetch;
  const requested: Array<{ url: string; authorization: string | null; cookie: string | null }> = [];
  const token = "ddo_console_token_1234567890abcdefghijklmnopqrstuvwxyz";

  globalThis.fetch = (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    const headers = new Headers(init?.headers);
    const url = String(input);
    requested.push({
      url,
      authorization: headers.get("authorization"),
      cookie: headers.get("cookie"),
    });
    if (url.includes("https://api.deno.com/v2/apps/")) {
      return Promise.resolve(new Response("{}", { status: 401 }));
    }
    return Promise.resolve(
      new Response("<title>Overview | ai-ubq-fi | Deploy</title>", {
        status: 200,
        headers: { "Content-Type": "text/html" },
      }),
    );
  };

  try {
    await withEnv({
      DENO_DEPLOY_APP_SLUG: "ai-ubq-fi",
      DENO_DEPLOY_ORG_SLUG: "ubiquity-dao",
    }, async () => {
      const req = new Request("https://ai.ubq.fi/uos/auth", {
        headers: { Authorization: `Bearer ${token}` },
      });
      const adminAuth = await authenticateAdmin(req);
      assert.equal(adminAuth.ok, true);
      if (adminAuth.ok) assert.equal(adminAuth.method.kind, "deno_deploy_token");

      assert.equal(requested.length, 2);
      assert.equal(requested[0].authorization, `Bearer ${token}`);
      assert.equal(requested[1].url, "https://console.deno.com/ubiquity-dao/ai-ubq-fi");
      assert.equal(requested[1].cookie, `token=${token}; deno_auth_ghid=force`);
    });
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("Deno Deploy console fallback rejects path-only HTML", async () => {
  kvStore.clear();
  const originalFetch = globalThis.fetch;
  const token = "ddo_console_path_token_1234567890abcdefghijklmnopqrstuvwxyz";

  globalThis.fetch = (input: RequestInfo | URL): Promise<Response> => {
    const url = String(input);
    if (url.includes("https://api.deno.com/v2/apps/")) {
      return Promise.resolve(new Response("{}", { status: 401 }));
    }
    return Promise.resolve(
      new Response("Sign in to view /ubiquity-dao/ai-ubq-fi", {
        status: 200,
        headers: { "Content-Type": "text/html" },
      }),
    );
  };

  try {
    await withEnv({
      DENO_DEPLOY_APP_SLUG: "ai-ubq-fi",
      DENO_DEPLOY_ORG_SLUG: "ubiquity-dao",
      DENO_DEPLOYMENT_ID: null,
    }, async () => {
      const req = new Request("https://ai.ubq.fi/uos/auth", {
        headers: { Authorization: `Bearer ${token}` },
      });
      const adminAuth = await authenticateAdmin(req);
      assert.equal(adminAuth.ok, false);
      if (!adminAuth.ok) assert.equal(adminAuth.response?.status, 401);
    });
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("Deno Deploy tokens do not fall back to the production app slug", async () => {
  kvStore.clear();
  const originalFetch = globalThis.fetch;
  const requested: string[] = [];
  const token = "ddo_deployment_token_1234567890abcdefghijklmnopqrstuvwxyz";

  globalThis.fetch = (input: RequestInfo | URL): Promise<Response> => {
    const url = String(input);
    requested.push(url);
    return Promise.resolve(new Response("{}", { status: url.includes("/v1/deployments/dep_test") ? 200 : 401 }));
  };

  try {
    await withEnv({
      DENO_DEPLOY_APP_SLUG: null,
      DENO_DEPLOY_ORG_SLUG: null,
      DENO_DEPLOYMENT_ID: "dep_test",
    }, async () => {
      const req = new Request("https://ai.ubq.fi/uos/auth", {
        headers: { Authorization: `Bearer ${token}` },
      });
      const adminAuth = await authenticateAdmin(req);
      assert.equal(adminAuth.ok, true);
      if (adminAuth.ok) assert.equal(adminAuth.method.kind, "deno_deploy_token");

      assert.deepEqual(requested, ["https://api.deno.com/v1/deployments/dep_test"]);
    });
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("passkey handles are deterministic and normalized", async () => {
  assert.equal(await buildPasskeyHandle("admin-token"), await buildPasskeyHandle("admin-token"));
  assert.match(await buildPasskeyHandle("admin-token"), /^uos-passkey-[0-9a-f]{16}$/);
  assert.equal(normalizePasskeyHandle(" Admin Passkey: Laptop! "), "admin-passkey-laptop");
});

Deno.test("passkey user presence is detected from KV", async () => {
  kvStore.clear();
  assert.equal(await hasPasskeyUsers(), false);

  const now = Date.now();
  kvStore.set(keyToString(passkeyUserKey("user-existing")), {
    id: "user-existing",
    handle: "uos-passkey-existing",
    is_admin: false,
    credential_ids: [],
    created_at_ms: now,
    updated_at_ms: now,
  });

  assert.equal(await hasPasskeyUsers(), true);
});

Deno.test("passkey RP ID follows browser origin behind Deno Deploy custom domain", () => {
  const req = new Request("https://ai-ubq-fi.ubiquity-dao.deno.net/api/auth/register/start", {
    method: "POST",
    headers: {
      "Origin": "https://ai.ubq.fi",
      "Content-Type": "application/json",
    },
    body: "{}",
  });

  assert.deepEqual(getPasskeyRequestMeta(req), {
    origin: "https://ai.ubq.fi",
    rpId: "ai.ubq.fi",
  });
});

Deno.test("passkey RP ID uses client window location when headers are unavailable", () => {
  const req = new Request("https://ai-ubq-fi.ubiquity-dao.deno.net/api/auth/register/start", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: "{}",
  });

  assert.deepEqual(getPasskeyRequestMeta(req, "https://ai.ubq.fi"), {
    origin: "https://ai.ubq.fi",
    rpId: "ai.ubq.fi",
  });
});

Deno.test("passkey RP ID allows localhost client origin for remote target", () => {
  const req = new Request("https://ai.ubq.fi/api/auth/register/start", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: "{}",
  });

  assert.deepEqual(getPasskeyRequestMeta(req, "http://localhost:8000"), {
    origin: "http://localhost:8000",
    rpId: "localhost",
  });
});

Deno.test("passkey RP ID ignores untrusted client origin", () => {
  const req = new Request("https://ai.ubq.fi/api/auth/register/start", {
    method: "POST",
    headers: {
      "Origin": "https://evil.example",
      "Content-Type": "application/json",
    },
    body: "{}",
  });

  assert.deepEqual(getPasskeyRequestMeta(req, "https://evil.example"), {
    origin: "https://ai.ubq.fi",
    rpId: "ai.ubq.fi",
  });
});

Deno.test("passkey RP ID ignores opaque origins", () => {
  const req = new Request("https://ai.ubq.fi/api/auth/register/start", {
    method: "POST",
    headers: {
      "Origin": "foo://bar",
      "Content-Type": "application/json",
    },
    body: "{}",
  });

  assert.deepEqual(getPasskeyRequestMeta(req), {
    origin: "https://ai.ubq.fi",
    rpId: "ai.ubq.fi",
  });
});

Deno.test("passkey registration start requires admin proof", async () => {
  kvStore.clear();
  const { default: handler } = await import("../src/handler.ts");

  const response = await handler(
    new Request("https://ai.ubq.fi/api/auth/register/start", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: "{}",
    }),
  );

  assert.equal(response.status, 401);
  const body = await response.json();
  assert.equal(body.error.code, "invalid_api_key");
});

Deno.test("passkey registration start can use an existing passkey session", async () => {
  kvStore.clear();
  const { token, user } = seedPasskeySession();
  const { default: handler } = await import("../src/handler.ts");

  const response = await handler(
    new Request("https://ai.ubq.fi/api/auth/register/start", {
      method: "POST",
      headers: {
        "Authorization": `Bearer ${token}`,
        "Content-Type": "application/json",
      },
      body: "{}",
    }),
  );

  assert.equal(response.status, 200);
  const body = await response.json();
  const encodedUserId = btoa(user.id).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
  assert.equal(body.handle, user.handle);
  assert.equal(body.publicKey.user.id, encodedUserId);
  assert.equal(body.publicKey.user.name, user.handle);
  assert.equal(body.publicKey.authenticatorSelection.userVerification, "required");
});

Deno.test("passkey registration start rejects session claims for another user handle", async () => {
  kvStore.clear();
  const { token } = seedPasskeySession();
  const now = Date.now();
  const otherUser = {
    id: "user-other",
    handle: "other-admin",
    is_admin: true,
    credential_ids: ["credential-other"],
    created_at_ms: now,
    updated_at_ms: now,
  };
  kvStore.set(keyToString(passkeyUserKey(otherUser.id)), otherUser);
  kvStore.set(keyToString(passkeyHandleKey(otherUser.handle)), otherUser.id);
  const { default: handler } = await import("../src/handler.ts");

  const response = await handler(
    new Request("https://ai.ubq.fi/api/auth/register/start", {
      method: "POST",
      headers: {
        "Authorization": `Bearer ${token}`,
        "Content-Type": "application/json",
      },
      body: JSON.stringify({ handle: otherUser.handle }),
    }),
  );

  assert.equal(response.status, 409);
  const body = await response.json();
  assert.match(body.error.message, /already exists/);
});

Deno.test("passkey registration start rejects token bootstrap claims for another user handle", async () => {
  kvStore.clear();
  const token = "ddo_bootstrap_claim_token_1234567890abcdefghijklmnopqrstuvwxyz";
  const now = Date.now();
  const otherUser = {
    id: "user-claimed",
    handle: "claimed-admin",
    is_admin: true,
    credential_ids: ["credential-claimed"],
    created_at_ms: now,
    updated_at_ms: now,
  };
  kvStore.set(keyToString(passkeyUserKey(otherUser.id)), otherUser);
  kvStore.set(keyToString(passkeyHandleKey(otherUser.handle)), otherUser.id);

  const response = await handlePasskeyRegisterStart(
    new Request("https://ai.ubq.fi/api/auth/register/start", {
      method: "POST",
      headers: {
        "Authorization": `Bearer ${token}`,
        "Content-Type": "application/json",
      },
      body: JSON.stringify({ handle: otherUser.handle }),
    }),
    { defaultIsAdmin: true },
  );

  assert.equal(response.status, 409);
  const body = await response.json();
  assert.match(body.error.message, /already exists/);
});

Deno.test("passkey registration start reuses an existing token-handle user", async () => {
  kvStore.clear();
  const token = "ddo_register_token_1234567890abcdefghijklmnopqrstuvwxyz";
  const handle = await buildPasskeyHandle(token);
  const now = Date.now();
  const user = {
    id: "user-token-handle",
    handle,
    is_admin: true,
    credential_ids: ["credential-token"],
    created_at_ms: now,
    updated_at_ms: now,
  };
  kvStore.set(keyToString(passkeyUserKey(user.id)), user);
  kvStore.set(keyToString(passkeyHandleKey(handle)), user.id);

  const response = await handlePasskeyRegisterStart(
    new Request("https://ai.ubq.fi/api/auth/register/start", {
      method: "POST",
      headers: {
        "Authorization": `Bearer ${token}`,
        "Content-Type": "application/json",
      },
      body: "{}",
    }),
    { defaultIsAdmin: true },
  );

  assert.equal(response.status, 200);
  const body = await response.json();
  const encodedUserId = btoa(user.id).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
  assert.equal(body.handle, handle);
  assert.equal(body.publicKey.user.id, encodedUserId);
  assert.equal(body.publicKey.authenticatorSelection.userVerification, "required");
  assert.deepEqual(body.publicKey.excludeCredentials, [{ id: "credential-token", type: "public-key" }]);
});

Deno.test("passkey login start requires user verification for admin handles", async () => {
  kvStore.clear();
  const now = Date.now();
  const adminUser = {
    id: "user-admin-login",
    handle: "admin-login",
    is_admin: true,
    credential_ids: ["credential-admin-login"],
    created_at_ms: now,
    updated_at_ms: now,
  };
  const memberUser = {
    id: "user-member-login",
    handle: "member-login",
    is_admin: false,
    credential_ids: ["credential-member-login"],
    created_at_ms: now,
    updated_at_ms: now,
  };
  kvStore.set(keyToString(passkeyUserKey(adminUser.id)), adminUser);
  kvStore.set(keyToString(passkeyHandleKey(adminUser.handle)), adminUser.id);
  kvStore.set(keyToString(passkeyUserKey(memberUser.id)), memberUser);
  kvStore.set(keyToString(passkeyHandleKey(memberUser.handle)), memberUser.id);

  const adminResponse = await handlePasskeyLoginStart(
    new Request("https://ai.ubq.fi/api/auth/login/start", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ handle: adminUser.handle }),
    }),
  );
  const memberResponse = await handlePasskeyLoginStart(
    new Request("https://ai.ubq.fi/api/auth/login/start", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ handle: memberUser.handle }),
    }),
  );

  assert.equal(adminResponse.status, 200);
  assert.equal(memberResponse.status, 200);
  assert.equal((await adminResponse.json()).publicKey.userVerification, "required");
  assert.equal((await memberResponse.json()).publicKey.userVerification, "preferred");
});

Deno.test("passkey login start without username remains discoverable", async () => {
  kvStore.clear();

  const response = await handlePasskeyLoginStart(
    new Request("https://ai.ubq.fi/api/auth/login/start", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: "{}",
    }),
  );

  assert.equal(response.status, 200);
  const body = await response.json();
  assert.equal(body.publicKey.allowCredentials, undefined);
  assert.equal(body.publicKey.userVerification, "preferred");
});

Deno.test("passkey login finish does not log raw user handles on assertion failure", async () => {
  kvStore.clear();
  const now = Date.now();
  const challenge = "auth-log-challenge";
  const user = {
    id: "user-log",
    handle: "sensitive-admin-handle",
    is_admin: true,
    credential_ids: ["credential-log"],
    created_at_ms: now,
    updated_at_ms: now,
  };
  kvStore.set(keyToString(passkeyUserKey(user.id)), user);
  kvStore.set(keyToString(passkeyCredentialKey("credential-log")), {
    credential_id: "credential-log",
    user_id: user.id,
    public_key: encodeBase64Url("not a valid public key"),
    sign_count: 0,
    transports: [],
    created_at_ms: now,
  });
  kvStore.set(keyToString(passkeyChallengeKey(challenge)), {
    challenge,
    type: "authentication",
    origin: "https://ai.ubq.fi",
    rp_id: "ai.ubq.fi",
    created_at_ms: now,
    expires_at_ms: now + 300_000,
  });

  const warnings: unknown[][] = [];
  const originalWarn = console.warn;
  console.warn = (...args: unknown[]) => {
    warnings.push(args);
  };
  try {
    const response = await handlePasskeyLoginFinish(
      new Request("https://ai.ubq.fi/api/auth/login/finish", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          response: {
            id: "credential-log",
            rawId: "credential-log",
            type: "public-key",
            response: {
              clientDataJSON: encodeBase64Url(JSON.stringify({
                type: "webauthn.get",
                challenge,
                origin: "https://ai.ubq.fi",
              })),
              authenticatorData: encodeBase64Url("invalid authenticator data"),
              signature: encodeBase64Url("invalid signature"),
              userHandle: encodeBase64Url(user.id),
            },
          },
        }),
      }),
    );

    assert.equal(response.status, 400);
  } finally {
    console.warn = originalWarn;
  }

  assert.equal(warnings.length, 1);
  const details = warnings[0][1] as Record<string, unknown>;
  assert.equal(details.has_user_handle, true);
  assert.equal(Object.prototype.hasOwnProperty.call(details, "user_handle"), false);
});

Deno.test("passkey registration deletes stale handle mapping when a user handle changes", async () => {
  kvStore.clear();
  const now = Date.now();
  const user = {
    id: "user-rename",
    handle: "old-name",
    is_admin: true,
    credential_ids: ["credential-old"],
    created_at_ms: now,
    updated_at_ms: now,
  };
  kvStore.set(keyToString(passkeyUserKey(user.id)), user);
  kvStore.set(keyToString(passkeyHandleKey(user.handle)), user.id);

  const saved = await saveVerifiedPasskeyRegistration(kvStub, {
    userId: user.id,
    handle: "new-name",
    isAdmin: false,
    credentialId: "credential-new",
    publicKey: "public-key",
    signCount: 0,
    transports: [],
  });

  assert.equal(saved.ok, true);
  if (!saved.ok) throw new Error("registration save failed");
  assert.equal(saved.user.handle, "new-name");
  assert.equal(kvStore.has(keyToString(passkeyHandleKey("old-name"))), false);
  assert.equal(kvStore.get(keyToString(passkeyHandleKey("new-name"))), user.id);
  assert.deepEqual(
    (kvStore.get(keyToString(passkeyUserKey(user.id))) as { credential_ids: string[] }).credential_ids,
    ["credential-old", "credential-new"],
  );
  assert.equal(
    (kvStore.get(keyToString(passkeyCredentialKey("credential-new"))) as { user_id: string }).user_id,
    user.id,
  );
});

Deno.test("passkey registration rejects concurrent handle claims", async () => {
  kvStore.clear();
  beforeAtomicCommit = () => {
    kvStore.set(keyToString(passkeyHandleKey("race-name")), "user-other");
  };

  const saved = await saveVerifiedPasskeyRegistration(kvStub, {
    userId: "user-race",
    handle: "race-name",
    isAdmin: true,
    credentialId: "credential-race",
    publicKey: "public-key",
    signCount: 0,
    transports: [],
  });

  assert.equal(saved.ok, false);
  if (saved.ok) throw new Error("registration save unexpectedly succeeded");
  assert.equal(saved.response.status, 409);
  assert.equal(kvStore.has(keyToString(passkeyUserKey("user-race"))), false);
  assert.equal(kvStore.get(keyToString(passkeyHandleKey("race-name"))), "user-other");
});

Deno.test("passkey credential sign count update rejects concurrent writes", async () => {
  kvStore.clear();
  const credentialKey = passkeyCredentialKey("credential-counter");
  kvStore.set(keyToString(credentialKey), {
    credential_id: "credential-counter",
    user_id: "user-counter",
    public_key: "public-key",
    sign_count: 4,
    transports: [],
    created_at_ms: Date.now(),
  });
  const entry = await kvStub.get(credentialKey) as Deno.KvEntryMaybe<{
    credential_id: string;
    user_id: string;
    public_key: string;
    sign_count: number;
    transports: string[];
    created_at_ms: number;
  }>;
  if (!entry.value || !entry.versionstamp) throw new Error("missing seeded credential");
  beforeAtomicCommit = () => {
    kvStore.set(keyToString(credentialKey), { ...entry.value, sign_count: 6 });
  };

  const updated = await updatePasskeyCredentialSignCount(kvStub, {
    key: entry.key,
    value: entry.value,
    versionstamp: entry.versionstamp,
  }, 5);

  assert.equal(updated, false);
  assert.equal((kvStore.get(keyToString(credentialKey)) as { sign_count: number }).sign_count, 6);
});

Deno.test("passkey logout deletes the cached session", async () => {
  kvStore.clear();
  const { token } = seedPasskeySession("uos_ai_session_logout");
  const { default: handler } = await import("../src/handler.ts");

  const response = await handler(
    new Request("https://ai.ubq.fi/api/auth/logout", {
      method: "POST",
      headers: { Authorization: `Bearer ${token}` },
    }),
  );

  assert.equal(response.status, 204);
  assert.equal(kvStore.has(keyToString(passkeySessionKey(token))), false);

  const req = new Request("https://ai.ubq.fi/uos/auth", {
    headers: { Authorization: `Bearer ${token}` },
  });
  const clientAuth = await authenticateClient(req);
  assert.equal(clientAuth.ok, false);
});

Deno.test("passkey user handlers list and toggle admin", async () => {
  kvStore.clear();
  const { user } = seedPasskeySession("uos_ai_session_role", { isAdmin: false });

  const listBefore = await handlePasskeyUsersList();
  assert.equal(listBefore.status, 200);
  const beforeBody = await listBefore.json();
  assert.equal(beforeBody.data[0].id, user.id);
  assert.equal(beforeBody.data[0].is_admin, false);

  const update = await handlePasskeyUsersUpdate(
    new Request("https://ai.ubq.fi/admin/passkey-users", {
      method: "PATCH",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ id: user.id, is_admin: true }),
    }),
  );
  assert.equal(update.status, 200);
  const updateBody = await update.json();
  assert.equal(updateBody.user.is_admin, true);

  const listAfter = await handlePasskeyUsersList();
  const afterBody = await listAfter.json();
  assert.equal(afterBody.data[0].is_admin, true);
});
