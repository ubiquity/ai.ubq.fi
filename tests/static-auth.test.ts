import assert from "node:assert/strict";

const { registerPasskey } = await import("../static/auth.js");

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
