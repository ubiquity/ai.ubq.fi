import assert from "node:assert/strict";

const keyToString = (key: Deno.KvKey): string => JSON.stringify(key);
const kvStore = new Map<string, unknown>();

const kvStub = {
  get: (key: Deno.KvKey) =>
    Promise.resolve(({ key, value: kvStore.get(keyToString(key)) ?? null }) as Deno.KvEntryMaybe<unknown>),
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
    const chain = {
      check: () => chain,
      set: (key: Deno.KvKey, value: unknown, _options?: { expireIn?: number }) => {
        ops.push({ type: "set", key, value });
        return chain;
      },
      delete: (key: Deno.KvKey) => {
        ops.push({ type: "delete", key });
        return chain;
      },
      commit: () => {
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
  handlePasskeyUsersList,
  handlePasskeyUsersUpdate,
  hasPasskeyUsers,
  normalizePasskeyHandle,
  passkeyCredentialKey,
  passkeyHandleKey,
  passkeySessionKey,
  passkeyUserKey,
  saveVerifiedPasskeyRegistration,
} = await import("../src/passkeys.ts");
const { authenticateAdmin, authenticateClient, handleV1Auth, requireAdminAuth } = await import("../src/auth.ts");

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
  const req = new Request("https://ai.ubq.fi/v1/auth", {
    headers: { Authorization: `Bearer ${token}` },
  });

  const clientAuth = await authenticateClient(req);
  assert.equal(clientAuth.ok, true);
  if (clientAuth.ok) {
    assert.equal(clientAuth.method.kind, "passkey_session");
    assert.equal(clientAuth.method.handle, user.handle);
  }

  const adminError = await requireAdminAuth(req);
  assert.equal(adminError, null);

  const whoami = await handleV1Auth(req);
  assert.equal(whoami.status, 200);
  const body = await whoami.json();
  assert.equal(body.auth.is_admin, true);
  assert.equal(body.auth.method.kind, "passkey_session");
  assert.equal(body.auth.method.user.handle, user.handle);
  assert.equal(body.auth.method.user.is_admin, true);
});

Deno.test("non-admin passkey session authenticates as client but not admin", async () => {
  kvStore.clear();
  const { token, user } = seedPasskeySession("uos_ai_session_user", { isAdmin: false });
  const req = new Request("https://ai.ubq.fi/v1/auth", {
    headers: { Authorization: `Bearer ${token}` },
  });

  const clientAuth = await authenticateClient(req);
  assert.equal(clientAuth.ok, true);
  if (clientAuth.ok) {
    assert.equal(clientAuth.method.kind, "passkey_session");
    assert.equal(clientAuth.method.handle, user.handle);
    assert.equal(clientAuth.method.is_admin, false);
  }

  const adminError = await requireAdminAuth(req);
  assert.equal(adminError?.status, 403);

  const whoami = await handleV1Auth(req);
  assert.equal(whoami.status, 200);
  const body = await whoami.json();
  assert.equal(body.auth.is_admin, false);
  assert.equal(body.auth.method.user.is_admin, false);
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
      const req = new Request("https://ai.ubq.fi/v1/auth", {
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
      const req = new Request("https://ai.ubq.fi/v1/auth", {
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
      const req = new Request("https://ai.ubq.fi/v1/auth", {
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
      const req = new Request("https://ai.ubq.fi/v1/auth", {
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
});

Deno.test("passkey registration start keeps a session bound to its own user", async () => {
  kvStore.clear();
  const { token, user } = seedPasskeySession();
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

  assert.equal(response.status, 200);
  const body = await response.json();
  const encodedUserId = btoa(user.id).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
  assert.equal(body.handle, user.handle);
  assert.equal(body.publicKey.user.id, encodedUserId);
  assert.equal(body.publicKey.user.name, user.handle);
  assert.deepEqual(body.publicKey.excludeCredentials, [{ id: "credential-test", type: "public-key" }]);
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

  const req = new Request("https://ai.ubq.fi/v1/auth", {
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
