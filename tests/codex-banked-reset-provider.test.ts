import assert from "node:assert/strict";
import {
  type CodexUsageResetFetch,
  CodexUsageResetProviderConfigurationError,
  CodexUsageResetProviderHttpError,
  createUpstreamCodexUsageResetProvider,
  type RedeemResetInput,
  type ResetAccountContext,
  resolveCodexUsageResetCreditEndpoints,
} from "../src/codex_banked_reset_provider.ts";

const context = (accountId = "account-one"): ResetAccountContext => ({
  accountId,
  accountIdHash: "account-one-hash",
  credentialVersion: "credential-v1",
  quotaGeneration: "quota-generation-v1",
});

const redeem = (accountId = "account-one"): RedeemResetInput => ({
  ...context(accountId),
  idempotencyKey: "redeem-request-id-123",
  creditId: "credit-selected",
});

const signal = (): AbortSignal => new AbortController().signal;

const provider = (fetch: CodexUsageResetFetch) =>
  createUpstreamCodexUsageResetProvider({
    codexBaseUrl: "https://chatgpt.com/backend-api/codex",
    accountId: "account-one",
    accessToken: "test-access-token",
    userAgent: "codex-banked-reset-provider-test/1.0",
    fetch,
    now: () => 1_700_000_000_000,
  });

Deno.test("upstream reset adapter derives the Codex reset-credit routes", () => {
  assert.deepEqual(
    resolveCodexUsageResetCreditEndpoints("https://chatgpt.com/backend-api/codex"),
    {
      inventoryUrl: "https://chatgpt.com/backend-api/wham/rate-limit-reset-credits",
      consumeUrl: "https://chatgpt.com/backend-api/wham/rate-limit-reset-credits/consume",
    },
  );
  assert.deepEqual(
    resolveCodexUsageResetCreditEndpoints("https://gateway.example/"),
    {
      inventoryUrl: "https://gateway.example/api/codex/rate-limit-reset-credits",
      consumeUrl: "https://gateway.example/api/codex/rate-limit-reset-credits/consume",
    },
  );
  assert.throws(
    () => resolveCodexUsageResetCreditEndpoints("https://chatgpt.com/unknown-layout"),
    CodexUsageResetProviderConfigurationError,
  );
  assert.throws(
    () => resolveCodexUsageResetCreditEndpoints("http://chatgpt.com/backend-api/codex"),
    CodexUsageResetProviderConfigurationError,
  );
});

Deno.test("upstream reset adapter parses inventory and sends the selected credit contract", async () => {
  const calls: Array<Readonly<{ url: string; init: RequestInit | undefined }>> = [];
  const responses = [
    new Response(
      JSON.stringify({
        available_count: 2,
        credits: [
          { id: "other", reset_type: "other", status: "available", expires_at: null },
          {
            id: "credit-available",
            reset_type: "codex_rate_limits",
            status: "available",
            expires_at: "2026-12-31T00:00:00Z",
          },
        ],
      }),
      { status: 200 },
    ),
    new Response(JSON.stringify({ code: "reset", windows_reset: 2 }), { status: 201 }),
  ];
  const resetProvider = provider((input, init) => {
    calls.push({ url: String(input), init });
    const response = responses.shift();
    assert.ok(response, "unexpected provider request");
    return Promise.resolve(response);
  });

  assert.deepEqual(await resetProvider.readInventory(context(), signal()), {
    availableCount: 2,
    observedAtMs: 1_700_000_000_000,
    credits: [
      { id: "other", resetType: "other", status: "available", expiresAtMs: null },
      { id: "credit-available", resetType: "codex_rate_limits", status: "available", expiresAtMs: 1_798_675_200_000 },
    ],
  });
  assert.deepEqual(await resetProvider.redeem({ ...redeem(), creditId: "credit-available" }, signal()), {
    kind: "completed",
    providerReceiptId: "upstream-reset",
  });
  assert.deepEqual(calls.map(({ url, init }) => `${init?.method} ${url}`), [
    "GET https://chatgpt.com/backend-api/wham/rate-limit-reset-credits",
    "POST https://chatgpt.com/backend-api/wham/rate-limit-reset-credits/consume",
  ]);
  assert.equal(calls[0].init?.body, undefined);
  assert.equal(calls[1].init?.body, '{"redeem_request_id":"redeem-request-id-123","credit_id":"credit-available"}');
  const headers = new Headers(calls[1].init?.headers);
  assert.equal(headers.get("Authorization"), "Bearer test-access-token");
  assert.equal(headers.get("ChatGPT-Account-ID"), "account-one");
  assert.equal(headers.get("User-Agent"), "codex-banked-reset-provider-test/1.0");
  assert.equal(headers.get("Content-Type"), "application/json");
  assert.equal(headers.get("originator"), null);
});

Deno.test("upstream reset adapter maps documented 2xx codes and preserves the exact request id", async (t) => {
  for (
    const [status, code, expected] of [
      [200, "reset", { kind: "completed", providerReceiptId: "upstream-reset" }],
      [201, "already_redeemed", { kind: "already_redeemed", providerReceiptId: "upstream-already-redeemed" }],
      [299, "nothing_to_reset", { kind: "rejected", reason: "nothing_to_reset" }],
      [200, "no_credit", { kind: "rejected", reason: "no_credit" }],
    ] as const
  ) {
    await t.step(`${status} ${code}`, async () => {
      let body = "";
      const resetProvider = provider((_input, init) => {
        body = String(init?.body);
        return Promise.resolve(new Response(JSON.stringify({ code }), { status }));
      });
      assert.deepEqual(
        await resetProvider.redeem({ ...redeem(), idempotencyKey: "  stable-id  " }, signal()),
        expected,
      );
      assert.equal(body, '{"redeem_request_id":"  stable-id  ","credit_id":"credit-selected"}');
    });
  }
});

Deno.test("upstream reset adapter treats non-2xx and malformed 2xx consume replies as ambiguous", async (t) => {
  for (
    const response of [
      new Response(null, { status: 204 }),
      new Response("not json", { status: 200 }),
      new Response(JSON.stringify({ code: "unrecognized" }), { status: 200 }),
      new Response("not relevant", { status: 429 }),
    ]
  ) {
    await t.step(String(response.status), async () => {
      const resetProvider = provider(() => Promise.resolve(response));
      assert.deepEqual(await resetProvider.redeem(redeem(), signal()), { kind: "unknown", providerReceiptId: null });
    });
  }
});

Deno.test("upstream reset adapter rejects malformed inventory and makes no transport call for invalid inputs", async () => {
  const badInventory = provider(() => Promise.resolve(new Response("{}", { status: 200 })));
  await assert.rejects(
    () => badInventory.readInventory(context(), signal()),
    CodexUsageResetProviderConfigurationError,
  );
  const unavailableInventory = provider(() => Promise.resolve(new Response(null, { status: 503 })));
  await assert.rejects(
    () => unavailableInventory.readInventory(context(), signal()),
    (error: unknown) => error instanceof CodexUsageResetProviderHttpError && error.status === 503,
  );

  let calls = 0;
  const resetProvider = provider(() => {
    calls += 1;
    return Promise.resolve(new Response(JSON.stringify({ code: "reset" }), { status: 200 }));
  });
  await assert.rejects(
    () => resetProvider.redeem(redeem("different-account"), signal()),
    CodexUsageResetProviderConfigurationError,
  );
  await assert.rejects(
    () => resetProvider.redeem({ ...redeem(), idempotencyKey: "   " }, signal()),
    CodexUsageResetProviderConfigurationError,
  );
  const controller = new AbortController();
  controller.abort(new Error("cancelled"));
  await assert.rejects(() => resetProvider.redeem(redeem(), controller.signal));
  assert.equal(calls, 0);
});

Deno.test("upstream reset adapter rejects summary-only, capped, malformed, or duplicate detailed inventories", async (t) => {
  const completeCredit = {
    id: "credit-one",
    reset_type: "codex_rate_limits",
    status: "available",
    expires_at: null,
  };
  for (
    const [name, payload] of [
      ["summary only", { available_count: 1, credits: null }],
      ["capped details", { available_count: 2, credits: [completeCredit] }],
      ["malformed expiry", { available_count: 1, credits: [{ ...completeCredit, expires_at: "not-a-date" }] }],
      ["duplicate opaque ID", { available_count: 2, credits: [completeCredit, { ...completeCredit }] }],
    ] as const
  ) {
    await t.step(name, async () => {
      const resetProvider = provider(() => Promise.resolve(new Response(JSON.stringify(payload), { status: 200 })));
      await assert.rejects(
        () => resetProvider.readInventory(context(), signal()),
        CodexUsageResetProviderConfigurationError,
      );
    });
  }
});
