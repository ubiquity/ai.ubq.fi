import assert from "node:assert/strict";

const {
  LOCAL_DEVELOPMENT_ADMIN_TOKEN,
  STORAGE_KEYS,
  formatAuthSessionLabel,
  hasAuthPasskeyCredential,
  isLocalDevelopmentOrigin,
  signInWithPasskey,
  signOut,
  registerPasskey,
} = await import(
  "../static/auth.js"
);

type Restore = () => void;

const setGlobal = (key: string, value: unknown): Restore => {
  const original = Object.getOwnPropertyDescriptor(globalThis, key);
  Object.defineProperty(globalThis, key, {
    configurable: true,
    writable: true,
    value,
  });
  return () => {
    if (original) Object.defineProperty(globalThis, key, original);
    else delete (globalThis as Record<string, unknown>)[key];
  };
};

const withPasskeyBrowser = async (fn: () => Promise<void>): Promise<void> => {
  const restoreSecureContext = setGlobal("isSecureContext", true);
  const restorePublicKeyCredential = setGlobal("PublicKeyCredential", function PublicKeyCredential() {});
  const restoreLocation = setGlobal("location", { origin: "http://localhost:8000" });
  const restoreNavigator = setGlobal("navigator", {
    credentials: {
      create: () => {
        throw new Error("test should stop before credential creation");
      },
      get: () => {
        throw new Error("test should stop before credential lookup");
      },
    },
  });
  try {
    await fn();
  } finally {
    restoreNavigator();
    restoreLocation();
    restorePublicKeyCredential();
    restoreSecureContext();
  }
};

const withLocalStorage = async (
  items: Record<string, string>,
  fn: (store: Map<string, string>) => Promise<void>,
): Promise<void> => {
  const store = new Map(Object.entries(items));
  const restoreLocalStorage = setGlobal("localStorage", {
    getItem: (key: string) => store.get(key) ?? null,
    setItem: (key: string, value: string) => {
      store.set(key, value);
    },
    removeItem: (key: string) => {
      store.delete(key);
    },
  });
  try {
    await fn(store);
  } finally {
    restoreLocalStorage();
  }
};

const bufferFromText = (value: string): ArrayBuffer => new TextEncoder().encode(value).buffer;

Deno.test("hasAuthPasskeyCredential recognizes passkey sessions and credential counts", () => {
  assert.equal(hasAuthPasskeyCredential({ method: { kind: "passkey_session" } }), true);
  assert.equal(
    hasAuthPasskeyCredential({ method: { kind: "passkey_session", user: { credential_count: 0 } } }),
    true,
  );
  assert.equal(hasAuthPasskeyCredential({ method: { kind: "admin_allowlist" } }), false);
  assert.equal(hasAuthPasskeyCredential({ method: { user: { credential_count: 1 } } }), true);
  assert.equal(hasAuthPasskeyCredential({ user: { credential_count: 1 } }), true);
  assert.equal(hasAuthPasskeyCredential({ user: { credential_count: 0 } }), false);
});

Deno.test("formatAuthSessionLabel distinguishes fallback token and passkey auth", () => {
  assert.equal(formatAuthSessionLabel({ method: { kind: "passkey_session" } }), "Passkey signed in");
  assert.equal(formatAuthSessionLabel({ method: { kind: "admin_allowlist" } }), "Fallback token active");
  assert.equal(formatAuthSessionLabel({ method: { kind: "auth_tokens_allowlist" } }), "Allowlist token active");
  assert.equal(formatAuthSessionLabel({ method: { kind: "disabled" } }), "Auth disabled");
  assert.equal(formatAuthSessionLabel({ method: { kind: "deno_deploy_token" } }), "Deno token active");
  assert.equal(formatAuthSessionLabel({ method: { kind: "kv_api_key" } }), "API key active");
  assert.equal(formatAuthSessionLabel({}), "Token active");
});

Deno.test("local development auth is restricted to loopback HTTP origins", () => {
  assert.equal(LOCAL_DEVELOPMENT_ADMIN_TOKEN, "local-dev-admin");

  const restoreLocal = setGlobal("location", { protocol: "http:", hostname: "localhost" });
  assert.equal(isLocalDevelopmentOrigin(), true);
  restoreLocal();

  const restoreRemote = setGlobal("location", { protocol: "https:", hostname: "ai.ubq.fi" });
  assert.equal(isLocalDevelopmentOrigin(), false);
  restoreRemote();

  const restoreLoopbackTls = setGlobal("location", { protocol: "https:", hostname: "127.0.0.1" });
  assert.equal(isLocalDevelopmentOrigin(), false);
  restoreLoopbackTls();

  const restoreIpv6Loopback = setGlobal("location", { protocol: "http:", hostname: "::1" });
  assert.equal(isLocalDevelopmentOrigin(), false);
  restoreIpv6Loopback();
});

const captureRegisterStartBody = async (
  input: { handle?: string; token: string; baseUrl?: string },
): Promise<Record<string, unknown>> => {
  let requestBody: Record<string, unknown> | null = null;
  const originalFetch = globalThis.fetch;
  globalThis.fetch = (_input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    requestBody = JSON.parse(String(init?.body ?? "{}")) as Record<string, unknown>;
    return Promise.resolve(
      new Response(JSON.stringify({ error: { message: "Unauthorized" } }), {
        status: 401,
        headers: { "Content-Type": "application/json" },
      }),
    );
  };
  try {
    await assert.rejects(() => registerPasskey({ baseUrl: "https://ai.ubq.fi", ...input }), /Unauthorized/);
    assert.ok(requestBody);
    return requestBody;
  } finally {
    globalThis.fetch = originalFetch;
  }
};

const captureLoginStartBody = async (
  input: { handle?: string; useHandle?: boolean; audienceOrigin?: string; baseUrl?: string },
): Promise<Record<string, unknown>> => {
  let requestBody: Record<string, unknown> | null = null;
  const originalFetch = globalThis.fetch;
  globalThis.fetch = (_input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    requestBody = JSON.parse(String(init?.body ?? "{}")) as Record<string, unknown>;
    return Promise.resolve(
      new Response(JSON.stringify({ error: { message: "Unauthorized" } }), {
        status: 401,
        headers: { "Content-Type": "application/json" },
      }),
    );
  };
  try {
    await assert.rejects(() => signInWithPasskey({ baseUrl: "https://ai.ubq.fi", ...input }), /Unauthorized/);
    assert.ok(requestBody);
    return requestBody;
  } finally {
    globalThis.fetch = originalFetch;
  }
};

Deno.test("signInWithPasskey ignores stored username by default", async () => {
  await withPasskeyBrowser(async () => {
    const body = await captureLoginStartBody({ handle: "uos-passkey-stale" });
    assert.equal(Object.prototype.hasOwnProperty.call(body, "handle"), false);
  });
});

Deno.test("signInWithPasskey sends username only when explicitly requested", async () => {
  await withPasskeyBrowser(async () => {
    const body = await captureLoginStartBody({ handle: " Admin Laptop ", useHandle: true });
    assert.equal(body.handle, "admin-laptop");
  });
});

Deno.test("signInWithPasskey sends the relay audience only when requested", async () => {
  await withPasskeyBrowser(async () => {
    const body = await captureLoginStartBody({
      audienceOrigin: "https://agent-worker-4d2p9cx7m1ab.ubiquity-os.deno.net",
    });
    assert.equal(body.relay_origin, "https://agent-worker-4d2p9cx7m1ab.ubiquity-os.deno.net");
  });
});

Deno.test("signOut clears a relay cookie without sending an empty bearer header", async () => {
  let requestUrl = "";
  let requestInit: RequestInit | undefined;
  const originalFetch = globalThis.fetch;
  globalThis.fetch = (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    requestUrl = String(input);
    requestInit = init;
    return Promise.resolve(new Response(null, { status: 204 }));
  };
  try {
    const audienceOrigin = "https://telegram-daily-exporter.0x4007.deno.net";
    await signOut({ baseUrl: "https://ai.ubq.fi", corsOrigin: audienceOrigin });
    assert.equal(new URL(requestUrl).searchParams.get("cors_origin"), audienceOrigin);
    assert.equal(requestInit?.credentials, "include");
    assert.equal(new Headers(requestInit?.headers).has("Authorization"), false);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

Deno.test("signInWithPasskey does not restrict discoverable login to cached credential ids", async () => {
  await withPasskeyBrowser(async () => {
    await withLocalStorage({
      [STORAGE_KEYS.passkeyCredentialIds]: JSON.stringify(["cached-credential-id"]),
    }, async () => {
      let requestOptions: Record<string, unknown> | null = null;
      const restoreNavigator = setGlobal("navigator", {
        credentials: {
          get: ({ publicKey }: { publicKey: Record<string, unknown> }) => {
            requestOptions = publicKey;
            throw new Error("stop after credential options");
          },
          create: () => {
            throw new Error("test should not create credentials");
          },
        },
      });
      const originalFetch = globalThis.fetch;
      globalThis.fetch = () =>
        Promise.resolve(
          new Response(JSON.stringify({ publicKey: { challenge: "AAAA", rpId: "localhost" } }), {
            status: 200,
            headers: { "Content-Type": "application/json" },
          }),
        );
      try {
        await assert.rejects(() => signInWithPasskey({ baseUrl: "https://ai.ubq.fi" }), /stop after credential/);
        assert.ok(requestOptions);
        assert.equal("allowCredentials" in requestOptions, false);
      } finally {
        globalThis.fetch = originalFetch;
        restoreNavigator();
      }
    });
  });
});

Deno.test("signInWithPasskey clears stale cached passkey metadata when server does not know the credential", async () => {
  await withPasskeyBrowser(async () => {
    await withLocalStorage({
      [STORAGE_KEYS.passkeyHandle]: "uos-passkey-stale",
      [STORAGE_KEYS.passkeyCredentialIds]: JSON.stringify(["stale-credential-id"]),
    }, async (store) => {
      const restoreNavigator = setGlobal("navigator", {
        credentials: {
          get: () =>
            Promise.resolve({
              id: "stale-credential-id",
              rawId: bufferFromText("stale-credential-id"),
              type: "public-key",
              response: {
                clientDataJSON: bufferFromText("{}"),
                authenticatorData: bufferFromText("authenticator"),
                signature: bufferFromText("signature"),
              },
            }),
          create: () => {
            throw new Error("test should not create credentials");
          },
        },
      });
      const originalFetch = globalThis.fetch;
      let requestIndex = 0;
      globalThis.fetch = () => {
        requestIndex += 1;
        if (requestIndex === 1) {
          return Promise.resolve(
            new Response(JSON.stringify({ publicKey: { challenge: "AAAA", rpId: "localhost" } }), {
              status: 200,
              headers: { "Content-Type": "application/json" },
            }),
          );
        }
        return Promise.resolve(
          new Response(JSON.stringify({ error: { message: "Unknown passkey" } }), {
            status: 400,
            headers: { "Content-Type": "application/json" },
          }),
        );
      };
      try {
        await assert.rejects(() => signInWithPasskey({ baseUrl: "https://ai.ubq.fi" }), /Unknown passkey/);
        assert.equal(store.has(STORAGE_KEYS.passkeyHandle), false);
        assert.equal(store.has(STORAGE_KEYS.passkeyCredentialIds), false);
      } finally {
        globalThis.fetch = originalFetch;
        restoreNavigator();
      }
    });
  });
});

Deno.test("registerPasskey leaves handle blank for passkey session registration", async () => {
  await withPasskeyBrowser(async () => {
    const body = await captureRegisterStartBody({ token: "uos_ai_session_existing" });
    assert.equal(Object.prototype.hasOwnProperty.call(body, "handle"), false);
  });
});

Deno.test("registerPasskey sends explicit normalized handle when provided", async () => {
  await withPasskeyBrowser(async () => {
    const body = await captureRegisterStartBody({
      handle: " Admin Laptop ",
      token: "uos_ai_session_existing",
    });
    assert.equal(body.handle, "admin-laptop");
  });
});
