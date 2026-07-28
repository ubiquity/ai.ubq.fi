import assert from "node:assert/strict";
import {
  type CodexUsageResetFetch,
  CodexUsageResetProviderConfigurationError,
  CodexUsageResetProviderHttpError,
  createStatusOnlyCodexUsageResetProvider,
  type RedeemResetInput,
  type ResetAccountContext,
  resolveCodexUsageResetCreditEndpoints,
} from "../src/codex_banked_reset_provider.ts";

const resetAccount = (accountId = "account-one"): ResetAccountContext => ({
  accountId,
  accountIdHash: "account-one-hash",
  credentialVersion: "credential-v1",
  quotaGeneration: "quota-generation-v1",
});

const redeemInput = (accountId = "account-one"): RedeemResetInput => ({
  ...resetAccount(accountId),
  idempotencyKey: "redeem-request-id-123",
});

const signal = (): AbortSignal => new AbortController().signal;

const createProvider = (fetch: CodexUsageResetFetch) =>
  createStatusOnlyCodexUsageResetProvider({
    codexBaseUrl: "https://chatgpt.com/backend-api/codex",
    accountId: "account-one",
    accessToken: "test-access-token",
    userAgent: "codex-banked-reset-provider-test/1.0",
    originator: "codex_cli_rs",
    fetch,
    now: () => 1_700_000_000_000,
  });

type BodyTrap = Readonly<{
  response: Response;
  cancellations: () => number;
  parseMethods: () => readonly string[];
}>;

/** A response double that fails if the adapter tries to interpret its payload. */
const responseWhoseBodyMustNotBeParsed = (status: number): BodyTrap => {
  const parsed: string[] = [];
  let cancellationCount = 0;
  const parsingAttempt = <T>(method: string): Promise<T> => {
    parsed.push(method);
    return Promise.reject(new Error(`provider response body was parsed through ${method}`));
  };
  const body = {
    cancel: (): Promise<void> => {
      cancellationCount += 1;
      return Promise.resolve();
    },
    getReader: (): never => {
      parsed.push("body.getReader");
      throw new Error("provider response body was parsed through body.getReader");
    },
  } as unknown as ReadableStream<Uint8Array>;
  const response = {
    status,
    body,
    arrayBuffer: (): Promise<ArrayBuffer> => parsingAttempt("arrayBuffer"),
    blob: (): Promise<Blob> => parsingAttempt("blob"),
    clone: (): Response => {
      parsed.push("clone");
      throw new Error("provider response body was parsed through clone");
    },
    formData: (): Promise<FormData> => parsingAttempt("formData"),
    json: (): Promise<unknown> => parsingAttempt("json"),
    text: (): Promise<string> => parsingAttempt("text"),
  } as unknown as Response;
  return {
    response,
    cancellations: () => cancellationCount,
    parseMethods: () => parsed,
  };
};

const capturedHeaders = (init: RequestInit | undefined): readonly [string, string][] => {
  assert.ok(init?.headers instanceof Headers);
  return [...init.headers.entries()];
};

Deno.test("status-only Codex reset provider derives the documented credit routes", () => {
  assert.deepEqual(
    resolveCodexUsageResetCreditEndpoints("https://chatgpt.com/backend-api/codex"),
    {
      inventoryUrl: "https://chatgpt.com/backend-api/wham/rate-limit-reset-credits",
      consumeUrl: "https://chatgpt.com/backend-api/wham/rate-limit-reset-credits/consume",
    },
  );
  assert.deepEqual(
    resolveCodexUsageResetCreditEndpoints("https://gateway.example/api/codex/"),
    {
      inventoryUrl: "https://gateway.example/api/codex/rate-limit-reset-credits",
      consumeUrl: "https://gateway.example/api/codex/rate-limit-reset-credits/consume",
    },
  );

  for (
    const baseUrl of [
      "not an absolute URL",
      "ftp://chatgpt.com/backend-api/codex",
      "https://chatgpt.com/backend-api/codex?unexpected=query",
      "https://chatgpt.com/other-layout",
    ]
  ) {
    assert.throws(
      () => resolveCodexUsageResetCreditEndpoints(baseUrl),
      CodexUsageResetProviderConfigurationError,
      baseUrl,
    );
  }
});

Deno.test("status-only Codex reset provider sends exact inventory and consume requests", async () => {
  const calls: Array<Readonly<{ url: string; init: RequestInit | undefined }>> = [];
  const responses = [new Response(null, { status: 200 }), new Response(null, { status: 201 })];
  const provider = createProvider((input, init) => {
    calls.push({ url: String(input), init });
    const response = responses.shift();
    assert.ok(response, "unexpected reset provider request");
    return Promise.resolve(response);
  });

  assert.deepEqual(await provider.readInventory(resetAccount(), signal()), {
    availableCount: 1,
    observedAtMs: 1_700_000_000_000,
    resetType: "banked_reset",
  });
  assert.deepEqual(await provider.redeem(redeemInput(), signal()), {
    kind: "completed",
    providerReceiptId: "status-201",
  });

  assert.equal(calls.length, 2);
  assert.equal(calls[0].url, "https://chatgpt.com/backend-api/wham/rate-limit-reset-credits");
  assert.equal(calls[0].init?.method, "GET");
  assert.equal(calls[0].init?.body, undefined);
  assert.equal(calls[0].init?.redirect, "manual");
  assert.deepEqual(capturedHeaders(calls[0].init), [
    ["accept", "application/json"],
    ["authorization", "Bearer test-access-token"],
    ["chatgpt-account-id", "account-one"],
    ["originator", "codex_cli_rs"],
    ["user-agent", "codex-banked-reset-provider-test/1.0"],
  ]);
  assert.equal(calls[1].url, "https://chatgpt.com/backend-api/wham/rate-limit-reset-credits/consume");
  assert.equal(calls[1].init?.method, "POST");
  assert.equal(calls[1].init?.redirect, "manual");
  assert.equal(calls[1].init?.body, '{"redeem_request_id":"redeem-request-id-123"}');
  assert.deepEqual(capturedHeaders(calls[1].init), [
    ["accept", "application/json"],
    ["authorization", "Bearer test-access-token"],
    ["chatgpt-account-id", "account-one"],
    ["content-type", "application/json"],
    ["originator", "codex_cli_rs"],
    ["user-agent", "codex-banked-reset-provider-test/1.0"],
  ]);
});

Deno.test("status-only Codex reset consume treats every HTTP 2xx as final without parsing the body", async (t) => {
  for (const status of [200, 201, 204, 299]) {
    await t.step(String(status), async () => {
      let calls = 0;
      const trapped = responseWhoseBodyMustNotBeParsed(status);
      const provider = createProvider(() => {
        calls += 1;
        return Promise.resolve(trapped.response);
      });

      assert.deepEqual(await provider.redeem(redeemInput(), signal()), {
        kind: "completed",
        providerReceiptId: `status-${status}`,
      });
      assert.equal(calls, 1);
      assert.deepEqual(trapped.parseMethods(), []);
      assert.equal(trapped.cancellations(), 1);
    });
  }
});

Deno.test("status-only Codex reset inventory accepts 2xx and ignores the provider payload", async () => {
  let calls = 0;
  const trapped = responseWhoseBodyMustNotBeParsed(299);
  const provider = createProvider(() => {
    calls += 1;
    return Promise.resolve(trapped.response);
  });

  assert.deepEqual(await provider.readInventory(resetAccount(), signal()), {
    availableCount: 1,
    observedAtMs: 1_700_000_000_000,
    resetType: "banked_reset",
  });
  assert.equal(calls, 1);
  assert.deepEqual(trapped.parseMethods(), []);
  assert.equal(trapped.cancellations(), 1);
});

Deno.test("status-only Codex reset consume makes one request for non-2xx and transport failures", async (t) => {
  await t.step("non-2xx", async () => {
    let calls = 0;
    const trapped = responseWhoseBodyMustNotBeParsed(429);
    const provider = createProvider(() => {
      calls += 1;
      return Promise.resolve(trapped.response);
    });

    assert.deepEqual(await provider.redeem(redeemInput(), signal()), {
      kind: "unknown",
      providerReceiptId: null,
    });
    assert.equal(calls, 1);
    assert.deepEqual(trapped.parseMethods(), []);
  });

  await t.step("transport failure", async () => {
    const transportFailure = new Error("offline transport failure");
    let calls = 0;
    const provider = createProvider(() => {
      calls += 1;
      return Promise.reject(transportFailure);
    });

    await assert.rejects(
      () => provider.redeem(redeemInput(), signal()),
      (error: unknown) => error === transportFailure,
    );
    assert.equal(calls, 1);
  });
});

Deno.test("status-only Codex reset provider rejects mismatched and aborted calls before transport", async () => {
  let calls = 0;
  const provider = createProvider(() => {
    calls += 1;
    return Promise.resolve(new Response(null, { status: 200 }));
  });

  await assert.rejects(
    () => provider.readInventory(resetAccount("other-account"), signal()),
    CodexUsageResetProviderConfigurationError,
  );
  await assert.rejects(
    () => provider.redeem(redeemInput("other-account"), signal()),
    CodexUsageResetProviderConfigurationError,
  );

  const controller = new AbortController();
  const abortReason = new Error("caller cancelled before reset transport");
  controller.abort(abortReason);
  await assert.rejects(
    () => provider.readInventory(resetAccount(), controller.signal),
    (error: unknown) => error === abortReason,
  );
  await assert.rejects(
    () => provider.redeem(redeemInput(), controller.signal),
    (error: unknown) => error === abortReason,
  );
  assert.equal(calls, 0);
});

Deno.test("status-only Codex reset inventory reports non-2xx status without parsing the body", async () => {
  let calls = 0;
  const trapped = responseWhoseBodyMustNotBeParsed(503);
  const provider = createProvider(() => {
    calls += 1;
    return Promise.resolve(trapped.response);
  });

  await assert.rejects(
    () => provider.readInventory(resetAccount(), signal()),
    (error: unknown) =>
      error instanceof CodexUsageResetProviderHttpError && error.operation === "inventory" && error.status === 503,
  );
  assert.equal(calls, 1);
  assert.deepEqual(trapped.parseMethods(), []);
  assert.equal(trapped.cancellations(), 1);
});
