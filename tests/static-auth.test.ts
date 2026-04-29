import assert from "node:assert/strict";

const { STORAGE_KEYS, signInWithPasskey, registerPasskey } = await import("../static/auth.js");

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

const withLocalStorage = async (items: Record<string, string>, fn: () => Promise<void>): Promise<void> => {
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
    await fn();
  } finally {
    restoreLocalStorage();
  }
};

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
  input: { handle?: string; useHandle?: boolean; baseUrl?: string },
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
